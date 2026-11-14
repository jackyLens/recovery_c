"""兼容辅助：上游只能提供逐搏 RR + 30s 分期时，先转换成 V1.0.2 的标准输入。

主流程不再依赖逐搏 RR；只有拿不到 5 分钟 HRV 的设备才需要这里。
"""
from __future__ import annotations

import warnings
from datetime import date, datetime, timedelta
from typing import List, Optional

import numpy as np
from numpy.lib.stride_tricks import sliding_window_view

from .config import Config
from .models import HRVSample, NightRaw
from .stages import hypnogram_to_intervals
from .utils import ramp


def rr_valid_mask(rr: np.ndarray, cfg: Config) -> np.ndarray:
    rr = np.asarray(rr, float)
    valid = (rr >= cfg.rr_min_ms) & (rr <= cfg.rr_max_ms)
    if rr.size == 0:
        return valid
    w = cfg.rr_local_window
    pad = w // 2
    x = np.pad(np.where(valid, rr, np.nan), pad, mode="edge")
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)
        med = np.nanmedian(sliding_window_view(x, w), axis=1)
    ok = np.isfinite(med) & (np.abs(rr - med) <= cfg.rr_jump_ratio * np.where(np.isfinite(med), med, 1))
    return valid & ok


def rr_to_hrv5min(rr_ms, start: datetime, duration_sec: float, rr_t_s=None,
                  resp_brpm=None, cfg: Optional[Config] = None) -> List[HRVSample]:
    """逐搏 RR → 每 5 分钟一条 HRVSample（RMSSD ms、平均心率、质量）。"""
    cfg = cfg or Config()
    rr = np.asarray(rr_ms, float)
    t_end = np.asarray(rr_t_s, float) if rr_t_s is not None else np.cumsum(rr) / 1000.0
    valid = rr_valid_mask(rr, cfg)
    L = cfg.segment_sec
    out: List[HRVSample] = []
    for i in range(int(np.ceil(duration_sec / L))):
        lo, hi = np.searchsorted(t_end, [i * L, (i + 1) * L])
        seg, ok = rr[lo:hi], valid[lo:hi]
        rmssd = hr = None
        q = 0.0
        if seg.size:
            cov = float(seg[ok].sum() / (L * 1000.0))
            art = float(1 - ok.mean())
            q = min(ramp(cov, cfg.q_cov_min, cfg.q_cov_good), 1 - ramp(art, cfg.q_art_good, cfg.q_art_max))
            if seg.size < cfg.seg_min_beats:
                q = 0.0
            pair = ok[1:] & ok[:-1]
            if pair.sum() >= cfg.min_rmssd_pairs:
                rmssd = float(np.sqrt(np.mean(np.diff(seg)[pair] ** 2)))
            if ok.sum() >= 30:
                hr = float(60000.0 / seg[ok].mean())
        resp = None
        if resp_brpm is not None and i < len(resp_brpm) and np.isfinite(resp_brpm[i]):
            resp = float(resp_brpm[i])
        out.append(HRVSample(t_start=start + timedelta(seconds=i * L), rmssd=rmssd, hr_bpm=hr,
                             resp_brpm=resp, quality=q, duration_sec=L))
    return out


def night_from_rr(night_date: date, start: datetime, rr_ms, hypnogram, epoch_sec: int = 30,
                  rr_t_s=None, resp_brpm=None, sleep_score_external=None,
                  cfg: Optional[Config] = None) -> NightRaw:
    dur = len(hypnogram) * epoch_sec
    return NightRaw(night_date=night_date,
                    hrv_5min=rr_to_hrv5min(rr_ms, start, dur, rr_t_s, resp_brpm, cfg),
                    stages=hypnogram_to_intervals(hypnogram, start, epoch_sec),
                    in_bed_start=start, sleep_score_external=sleep_score_external)
