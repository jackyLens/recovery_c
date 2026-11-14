"""个人基线：只用“今天之前”的有效夜；中位数 + MAD；人群波动先验收缩；新常态回纳。"""
from __future__ import annotations

from dataclasses import dataclass, field
from datetime import date
from typing import Dict, List, Optional

import numpy as np

from .config import Config
from .models import DayRecord
from .utils import clamp

METRICS = ("recovery_hrv", "late_hrv", "early_hrv", "rhr", "resp", "sleep_score")


@dataclass
class MetricBaseline:
    median: float
    sigma: float
    n: int


@dataclass
class Baseline:
    metrics: Dict[str, MetricBaseline] = field(default_factory=dict)
    n_nights: int = 0
    stage: str = "calibrating"
    sleep_need: float = 450.0
    midpoint_median: Optional[float] = None
    readmitted: int = 0
    excluded: int = 0

    def get(self, key: str) -> Optional[MetricBaseline]:
        return self.metrics.get(key)

    def z(self, key: str, value: Optional[float]) -> Optional[float]:
        mb = self.metrics.get(key)
        if mb is None or value is None:
            return None
        return (value - mb.median) / mb.sigma


def _flagged(r: DayRecord) -> bool:
    res = r.result
    return bool(res is not None and (res.baseline_outlier or res.illness_level in ("moderate", "high")))


def _stage(n: int, cfg: Config) -> str:
    if n < cfg.stage_estimating:
        return "calibrating"
    if n < cfg.stage_trial:
        return "estimating"
    if n < cfg.stage_provisional:
        return "trial"
    if n < cfg.stage_formal:
        return "provisional"
    return "formal"


def build_baseline(history: List[DayRecord], today: date, cfg: Config) -> Baseline:
    past = sorted([r for r in history if 0 < (today - r.night.night_date).days <= cfg.baseline_lookback_days],
                  key=lambda r: r.night.night_date)
    base_ok = [r for r in past if r.night.valid_night and r.night.tst_min >= cfg.baseline_exclude_tst_min
               and r.symptoms != "moderate_severe"]
    # 新常态：末尾连续 ≥N 晚被标记异常且均无不适标签 → 重新纳入
    readmit = set()
    run = []
    for r in reversed(base_ok):
        if _flagged(r) and r.symptoms == "none":
            run.append(id(r))
        else:
            break
    if len(run) >= cfg.new_normal_run:
        readmit = set(run)
    eligible = [r for r in base_ok if (not _flagged(r)) or id(r) in readmit]
    eligible = eligible[-cfg.baseline_window:]

    bl = Baseline(n_nights=len(eligible), stage=_stage(len(eligible), cfg),
                  readmitted=len(readmit), excluded=len(past) - len(eligible))
    for key in METRICS:
        if key == "sleep_score":
            vals = [r.result.subscores.get("sleep") for r in eligible if r.result is not None]
        else:
            vals = [getattr(r.night, key) for r in eligible]
        vals = np.array([v for v in vals if v is not None], float)
        n = vals.size
        if n < cfg.baseline_min_values:
            continue
        med = float(np.median(vals))
        mad = float(np.median(np.abs(vals - med)))
        sp = max(1.4826 * mad, cfg.sigma_floor[key])
        prior = cfg.sigma_prior[key]
        k = cfg.prior_k
        sigma = float(np.sqrt((n * sp ** 2 + k * prior ** 2) / (n + k)))
        bl.metrics[key] = MetricBaseline(med, max(sigma, cfg.sigma_floor[key]), n)

    tst = [r.night.tst_min for r in eligible]
    if len(tst) >= cfg.baseline_min_values:
        bl.sleep_need = clamp(float(np.median(tst)), cfg.sleep_need_min, cfg.sleep_need_max)
    else:
        bl.sleep_need = cfg.sleep_need_default
    mids = [r.night.midpoint_min for r in eligible if r.night.midpoint_min is not None]
    if len(mids) >= cfg.baseline_min_values:
        bl.midpoint_median = float(np.median(mids))
    return bl
