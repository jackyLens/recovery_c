"""5 分钟 HRV 记录 → 片段：数值校验、离群剔除、分期映射、四类权重。"""
from __future__ import annotations

import math
from datetime import datetime, timedelta
from typing import List, Optional, Tuple

import numpy as np

from .config import Config
from .models import N1, N2, N3, REM, WAKE, NightRaw, Segment
from .stages import Interval, sleep_bounds, stage_overlap
from .utils import clamp


def _stage_weight(stage: int, stable: bool, cfg: Config) -> float:
    if stage == N3:
        return cfg.w_n3_stable if stable else cfg.w_n3_mixed
    if stage == N2:
        return cfg.w_n2_stable if stable else cfg.w_n2_mixed
    if stage == N1:
        return cfg.w_n1
    if stage == REM:
        return cfg.w_rem
    return cfg.w_wake


def _finite(v) -> Optional[float]:
    if v is None:
        return None
    try:
        v = float(v)
    except (TypeError, ValueError):
        return None
    return v if math.isfinite(v) else None


def _ln_value(v: Optional[float], is_ln: bool, cfg: Config) -> Tuple[Optional[float], Optional[float]]:
    """返回 (rmssd_ms, lnRMSSD)；超出合理范围返回 (None, None)。"""
    if v is None:
        return None, None
    rmssd = math.exp(v) if is_ln else v
    if not (cfg.rmssd_min_ms <= rmssd <= cfg.rmssd_max_ms):
        return None, None
    return rmssd, math.log(rmssd)


def _flag_outliers(ln: List[Optional[float]], cfg: Config) -> List[bool]:
    """与前后各 k 个有效窗口的中位数比较，偏离 > ln_outlier 判为离群（单点尖刺）。"""
    idx = [i for i, v in enumerate(ln) if v is not None]
    vals = [ln[i] for i in idx]
    flags = [False] * len(ln)
    k = cfg.ln_outlier_neighbors
    for j, i in enumerate(idx):
        nb = vals[max(0, j - k):j] + vals[j + 1:j + 1 + k]
        if len(nb) >= 2 and abs(vals[j] - float(np.median(nb))) > cfg.ln_outlier:
            flags[i] = True
    return flags


def segment_night(raw: NightRaw, iv: List[Interval], cfg: Config) -> List[Segment]:
    onset, offset = sleep_bounds(iv)
    if onset is None:
        return []
    ref = raw.in_bed_start or iv[0][0]
    onset_s = (onset - ref).total_seconds()
    offset_s = (offset - ref).total_seconds()
    spt = max(offset_s - onset_s, 1.0)

    samples = sorted(raw.hrv_5min, key=lambda x: x.t_start)
    # 同一时刻重复上报只保留最后一条
    dedup = {}
    for x in samples:
        dedup[x.t_start] = x
    samples = [dedup[t] for t in sorted(dedup)]

    vals = [_ln_value(_finite(x.rmssd), raw.hrv_is_ln, cfg) for x in samples]
    outlier = _flag_outliers([v[1] for v in vals], cfg)

    segs: List[Segment] = []
    for i, x in enumerate(samples):
        dur = x.duration_sec or cfg.segment_sec
        t0, t1 = x.t_start, x.t_start + timedelta(seconds=dur)
        sec, labelled = stage_overlap(iv, t0, t1)
        lab_total = sec.sum()
        if labelled < cfg.min_stage_label_frac or lab_total <= 0:
            stage, stable = WAKE, False
        else:
            stage = int(np.argmax(sec))
            frac = sec[stage] / lab_total
            wake_frac = sec[WAKE] / lab_total
            stable = bool(frac >= cfg.stable_frac and sec[WAKE] == 0 and labelled >= cfg.stable_frac)
            if wake_frac > cfg.wake_frac_max:
                stage, stable = WAKE, False

        rmssd, ln = vals[i]
        hr = _finite(x.hr_bpm)
        if hr is not None and not (cfg.hr_min_bpm <= hr <= cfg.hr_max_bpm):
            hr = None
        resp = _finite(x.resp_brpm)

        # 质量权重：上游质量（缺省 1）；离群 / 体动大 → 0
        qin = _finite(x.quality)
        q = 1.0 if qin is None else clamp(qin, 0.0, 1.0)
        mot = _finite(x.motion)
        if mot is not None and mot > cfg.motion_max:
            q = 0.0
        if outlier[i]:
            q = 0.0
        coverage = q if ln is not None else 0.0

        s0 = (t0 - ref).total_seconds()
        s1 = s0 + dur
        pos = ((s0 + s1) / 2 - onset_s) / spt
        # 时间位置权重
        if s1 <= onset_s or s0 >= offset_s:
            wt = 0.0
        elif s1 > offset_s - cfg.prewake_min * 60:
            wt = cfg.tw_prewake
        elif s0 < onset_s + cfg.onset_min * 60:
            wt = cfg.tw_onset
        elif pos < cfg.mid_band[0]:
            wt = cfg.tw_first
        elif pos <= cfg.mid_band[1]:
            wt = cfg.tw_mid
        else:
            wt = cfg.tw_late

        segs.append(Segment(idx=i, t_start_s=s0, stage=stage, stable=stable, pos=float(pos),
                            coverage=coverage, rmssd=rmssd, ln_rmssd=ln, hr=hr, resp=resp,
                            w_quality=float(q), w_stage=_stage_weight(stage, stable, cfg), w_time=wt, dur_s=float(dur)))

    # 生理稳定性权重：与邻近 ±2 窗 HR 中位数比较；RESP 与整晚中位数比较
    hrs = np.array([s.hr if s.hr is not None else np.nan for s in segs])
    resps = np.array([s.resp if s.resp is not None else np.nan for s in segs])
    resp_med = np.nanmedian(resps) if np.isfinite(resps).any() else np.nan
    for k, s in enumerate(segs):
        w = 1.0
        if s.hr is not None:
            nb = np.r_[hrs[max(0, k - 2):k], hrs[k + 1:k + 3]]
            nb = nb[np.isfinite(nb)]
            if nb.size:
                dev = abs(s.hr - np.median(nb))
                if dev > cfg.stab_hr_severe:
                    w = cfg.stab_w_severe
                elif dev > cfg.stab_hr_mild:
                    w = cfg.stab_w_mild
        if s.resp is not None and np.isfinite(resp_med) and abs(s.resp - resp_med) > cfg.stab_resp_dev:
            w *= cfg.stab_w_resp
        s.w_stability = w
        # 后半夜加权：仅稳定 NREM + 高质量 + 生理稳定
        if (s.w_time == cfg.tw_late and s.pos > cfg.mid_band[1] and s.stage in (N2, N3)
                and s.stable and s.w_quality >= 0.8 and w == 1.0):
            s.w_time = cfg.tw_late_bonus
        s.weight = 0.0 if s.ln_rmssd is None else s.w_quality * s.w_stage * s.w_time * s.w_stability
    return segs


def wear_coverage(segs: List[Segment], onset_s: float, offset_s: float, q_min: float) -> float:
    """主睡眠期内被"有效 HRV 窗口"覆盖的时间比例（缺窗 = 未佩戴 / 无数据）。"""
    spt = offset_s - onset_s
    if spt <= 0:
        return 0.0
    covered = 0.0
    for s in segs:
        if s.ln_rmssd is None or s.w_quality < q_min:
            continue
        a, b = s.t_start_s, s.t_start_s + s.dur_s
        covered += max(0.0, min(b, offset_s) - max(a, onset_s))
    return float(min(covered / spt, 1.0))
