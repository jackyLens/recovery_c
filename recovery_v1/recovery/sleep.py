"""睡眠结构提取与睡眠子分（不依赖深睡/REM 绝对分钟数，不依赖训练负荷）。"""
from __future__ import annotations

from datetime import date, datetime, timedelta
from typing import Dict, List, Optional, Tuple

import numpy as np

from .config import Config
from .models import WAKE, DayRecord, NightMetrics
from .utils import clamp, ramp
from .stages import extract_sleep_metrics  # noqa: F401  (兼容旧导入路径)


def sleep_debt(history: List[DayRecord], today: date, need: float, cfg: Config, days: int) -> float:
    """today 之前 days 个日历日内的累计睡眠债（每晚封顶）。缺失夜不计债。"""
    debt = 0.0
    for r in history:
        delta = (today - r.night.night_date).days
        if 1 <= delta <= days:
            debt += min(cfg.debt_night_cap, max(0.0, need - r.night.tst_min))
    return debt


def compute_sleep_score(night: NightMetrics, need: float, debt_prev: float,
                        midpoint_median: Optional[float], cfg: Config) -> Tuple[float, Dict]:
    need_today = need + min(cfg.debt_need_factor * debt_prev, cfg.debt_need_cap)
    ratio = night.tst_min / need_today if need_today > 0 else 0.0
    duration = 100 * ramp(ratio, cfg.duration_zero_ratio, 1.0)
    efficiency = 100 * ramp(night.efficiency, cfg.se_zero, cfg.se_full)
    if midpoint_median is None or night.midpoint_min is None:
        regularity = cfg.reg_unknown
    else:
        regularity = 100 * (1 - ramp(abs(night.midpoint_min - midpoint_median), 0, cfg.reg_zero_min))
    continuity = 100 * (1 - ramp(night.waso_min, cfg.waso_full, cfg.waso_zero))
    continuity = clamp(continuity - cfg.long_awake_penalty * max(0, night.long_awakenings - 1), 0, 100)
    debt_after = debt_prev + min(cfg.debt_night_cap, max(0.0, need - night.tst_min))
    debt_score = 100 * (1 - ramp(debt_after, 0, cfg.debt_zero_score))
    parts = dict(duration=duration, efficiency=efficiency, regularity=regularity,
                 continuity=continuity, debt=debt_score)
    w = cfg.sleep_weights
    score = sum(w[k] * parts[k] for k in w)
    if night.sleep_score_external is not None:
        score = float(night.sleep_score_external)
    detail = {**{k: round(v, 1) for k, v in parts.items()}, "need_min": round(need_today, 1),
              "debt_prev_min": round(debt_prev, 1), "debt_after_min": round(debt_after, 1)}
    return clamp(score, 0, 100), detail
