"""合成数据：生成 5 分钟 HRV 序列 + 带时间的睡眠分期，用于演示和测试（非真实生理模型）。"""
from __future__ import annotations

from datetime import date, datetime, time, timedelta
from typing import List, Optional

import numpy as np

from .models import N1, N2, N3, REM, WAKE, HRVSample, NightRaw, StageInterval
from .stages import hypnogram_to_intervals

_LN = {WAKE: 3.30, N1: 3.55, N2: 3.80, N3: 4.00, REM: 3.50}
_HR = {WAKE: 64.0, N1: 59.0, N2: 56.5, N3: 54.5, REM: 59.5}


def make_hypnogram(tst_min: float, rng: np.random.Generator, latency_min: float = 12,
                   waso_bouts: int = 1, epoch_sec: int = 30) -> np.ndarray:
    m = 60 // epoch_sec
    hyp = [WAKE] * int(latency_min * m)
    slept = 0.0
    cycle = 0
    while slept < tst_min:
        frac_n3 = max(0.05, 0.35 - 0.08 * cycle)
        frac_rem = min(0.40, 0.12 + 0.07 * cycle)
        L = 90
        parts = [(N1, 0.05), (N2, 1 - 0.05 - frac_n3 - frac_rem), (N3, frac_n3), (N2, 0.0), (REM, frac_rem)]
        for st, f in parts:
            n = int(round(L * f * m))
            hyp += [st] * n
            slept += n / m
        cycle += 1
    hyp = np.array(hyp, dtype=int)
    # 截断到目标 TST
    sleep_idx = np.flatnonzero(hyp != WAKE)
    cut = sleep_idx[int(tst_min * m) - 1] + 1 if sleep_idx.size >= tst_min * m else len(hyp)
    hyp = hyp[:cut]
    # 插入夜醒
    for _ in range(waso_bouts):
        pos = rng.integers(int(len(hyp) * 0.3), int(len(hyp) * 0.8))
        dur = int(rng.integers(4, 10) * m)
        hyp = np.r_[hyp[:pos], [WAKE] * dur, hyp[pos:]]
    return np.r_[hyp, [WAKE] * (5 * m)]


def make_night(d: date, rng: Optional[np.random.Generator] = None, *, tst_min: float = 450,
               ln_shift: float = 0.0, late_shift: float = 0.0, early_shift: float = 0.0,
               hr_shift: float = 0.0, resp_shift: float = 0.0, resp_base: float = 14.5,
               waso_bouts: int = 1, artifact_rate: float = 0.005, bedtime: time = time(23, 0),
               drop_hrv: bool = False, drop_rr: bool = False, drop_resp: bool = False,
               drop_hr: bool = False, wear_gap_frac: float = 0.0, stage_labels: str = "code",
               noise: float = 1.0) -> NightRaw:
    """生成一晚 V1.0.2 标准输入：每 5 分钟一条 HRV（RMSSD ms + 平均心率 + 呼吸率 + 质量）
    + 带起止时间的睡眠分期区间。stage_labels="code" 用 0–4 编码，"text" 用 wake/light/deep/rem。"""
    rng = rng or np.random.default_rng(0)
    epoch, L = 30, 300
    per = L // epoch
    hyp = make_hypnogram(tst_min, rng, waso_bouts=waso_bouts)
    sleep_idx = np.flatnonzero(hyp != WAKE)
    on, off = sleep_idx[0], sleep_idx[-1] + 1
    day_ln = rng.normal(0, 0.07 * noise)
    day_hr = rng.normal(0, 1.0 * noise)
    day_resp = rng.normal(0, 0.25 * noise)
    start = datetime.combine(d - timedelta(days=1), bedtime)

    n_win = int(np.ceil(len(hyp) / per))
    g0 = int(n_win * 0.35)
    g1 = g0 + int(n_win * wear_gap_frac)
    samples: List[HRVSample] = []
    for i in range(n_win):
        if wear_gap_frac > 0 and g0 <= i < g1:   # 中间一段脱戴：这段没有任何记录
            continue
        ep = np.arange(i * per, min((i + 1) * per, len(hyp)))
        pos = np.clip((ep - on) / max(off - on, 1), 0, 1)
        st = hyp[ep]
        ln_e = np.array([_LN[s] for s in st]) + 0.15 * pos + day_ln + ln_shift \
            + np.where(pos >= 0.5, late_shift, early_shift)
        hr_e = np.array([_HR[s] for s in st]) - 3.0 * pos + day_hr + hr_shift
        ln = float(np.log(np.mean(np.exp(ln_e)))) + rng.normal(0, 0.05)
        if rng.random() < artifact_rate:            # 偶发伪迹窗口
            ln += float(rng.choice([-1.0, 1.0]))
        rmssd = None if (drop_hrv or drop_rr) else round(float(np.exp(ln)), 1)
        hr = None if (drop_hr or drop_rr) else round(float(np.mean(hr_e) + rng.normal(0, 0.3)), 1)
        resp = None if drop_resp else round(float(resp_base + day_resp + resp_shift + rng.normal(0, 0.3)), 1)
        samples.append(HRVSample(t_start=start + timedelta(seconds=i * L), rmssd=rmssd, hr_bpm=hr,
                                 resp_brpm=resp, quality=round(float(rng.uniform(0.85, 1.0)), 2),
                                 duration_sec=L))
    stages = hypnogram_to_intervals(hyp, start, epoch)
    if stage_labels == "text":
        txt = {WAKE: "wake", N1: "light", N2: "light", N3: "deep", REM: "rem"}
        stages = [StageInterval(s.start, s.end, txt[s.stage]) for s in stages]
    return NightRaw(night_date=d, hrv_5min=samples, stages=stages, in_bed_start=start)
