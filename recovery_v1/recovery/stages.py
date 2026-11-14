"""带时间的睡眠分期：规范化、主睡眠边界、睡眠结构指标、与 5 分钟窗口的重叠。"""
from __future__ import annotations

from datetime import datetime, timedelta
from typing import Dict, List, Optional, Tuple

import numpy as np

from .models import WAKE, StageInterval, parse_stage

Interval = Tuple[datetime, datetime, int]


def normalize_stages(stages: List[StageInterval]) -> List[Interval]:
    """转成 (start, end, code)，按时间排序，丢弃零长 / 倒序区间，重叠部分以后一段为准截断前一段。"""
    out: List[Interval] = []
    for s in sorted(stages, key=lambda x: x.start):
        st, en, code = s.start, s.end, parse_stage(s.stage)
        if en <= st:
            continue
        if out and st < out[-1][1]:
            ps, pe, pc = out[-1]
            out[-1] = (ps, st, pc)
            if out[-1][1] <= out[-1][0]:
                out.pop()
        out.append((st, en, code))
    return out


def sleep_bounds(iv: List[Interval]) -> Tuple[Optional[datetime], Optional[datetime]]:
    sl = [x for x in iv if x[2] != WAKE]
    if not sl:
        return None, None
    return sl[0][0], max(x[1] for x in sl)


def _min(a: datetime, b: datetime) -> float:
    return (b - a).total_seconds() / 60.0


def extract_sleep_metrics(iv: List[Interval], in_bed_start: Optional[datetime] = None) -> Dict:
    onset, offset = sleep_bounds(iv)
    if onset is None:
        return dict(tst_min=0.0, efficiency=0.0, waso_min=0.0, long_awakenings=0, midpoint_min=None)
    rec_start = min(in_bed_start or iv[0][0], iv[0][0])
    tst = sum(_min(s, e) for s, e, c in iv if c != WAKE)
    # 主睡眠期内的清醒：合并相邻清醒区间后统计
    waso, long_aw = 0.0, 0
    run_start: Optional[datetime] = None
    run_end: Optional[datetime] = None
    for s, e, c in iv:
        if e <= onset or s >= offset:
            continue
        s, e = max(s, onset), min(e, offset)
        if c == WAKE:
            waso += _min(s, e)
            if run_end is not None and s <= run_end:
                run_end = max(run_end, e)
            else:
                if run_start is not None and _min(run_start, run_end) >= 5:
                    long_aw += 1
                run_start, run_end = s, e
    if run_start is not None and _min(run_start, run_end) >= 5:
        long_aw += 1
    denom = _min(rec_start, offset)
    eff = tst / denom if denom > 0 else 0.0
    mid = onset + (offset - onset) / 2
    midpoint = ((mid.hour * 60 + mid.minute) - 720) % 1440
    return dict(tst_min=float(tst), efficiency=float(min(eff, 1.0)), waso_min=float(waso),
                long_awakenings=int(long_aw), midpoint_min=float(midpoint))


def stage_overlap(iv: List[Interval], t0: datetime, t1: datetime) -> Tuple[np.ndarray, float]:
    """窗口 [t0, t1) 与各分期的重叠秒数（长度 5 的数组）及被分期覆盖的比例。"""
    sec = np.zeros(5)
    for s, e, c in iv:
        if e <= t0:
            continue
        if s >= t1:
            break
        sec[c] += (min(e, t1) - max(s, t0)).total_seconds()
    total = (t1 - t0).total_seconds()
    return sec, float(sec.sum() / total) if total > 0 else 0.0


def hypnogram_to_intervals(hyp, start: datetime, epoch_sec: int = 30) -> List[StageInterval]:
    """30 秒 epoch 分期序列 → 带时间的分期区间（兼容旧数据）。"""
    hyp = np.asarray(hyp, int)
    out: List[StageInterval] = []
    i = 0
    while i < len(hyp):
        j = i
        while j < len(hyp) and hyp[j] == hyp[i]:
            j += 1
        out.append(StageInterval(start=start + timedelta(seconds=i * epoch_sec),
                                 end=start + timedelta(seconds=j * epoch_sec), stage=int(hyp[i])))
        i = j
    return out
