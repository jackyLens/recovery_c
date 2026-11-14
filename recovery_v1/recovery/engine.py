"""评分主流程：score_day() 纯函数 + RecoveryEngine（保存历史，逐晚调用）。"""
from __future__ import annotations

from typing import List, Optional, Union

from .baseline import build_baseline
from .config import Config
from .explain import STATE_TEXT, summary_text, top_factors
from .models import DayRecord, NightMetrics, NightRaw, RecoveryResult
from .night import process_night
from .scoring import (ADVICE, ILLNESS_MESSAGES, confidence_level, evaluate_gates, hrv_subscore,
                      illness_points, late_adjustment, resp_penalty, resp_severity, rhr_penalty,
                      tier_of, trend_adjustment)
from .sleep import compute_sleep_score, sleep_debt
from .utils import clamp, round_half_up


def score_day(night: NightMetrics, history: List[DayRecord], symptoms: str = "none",
              cfg: Optional[Config] = None) -> RecoveryResult:
    cfg = cfg or Config()
    today = night.night_date
    bl = build_baseline(history, today, cfg)
    res = RecoveryResult(date=today, baseline_stage=bl.stage, baseline_nights=bl.n_nights)

    # 1. 有效性
    if not night.valid_night:
        res.state = "sleep_too_short" if night.invalid_reason == "sleep_too_short" else "insufficient_data"
        res.summary = STATE_TEXT[res.state]
        return res

    # 2. 睡眠子分（冷启动期也计算并保存，用于之后的睡眠分基线）
    debt_prev = sleep_debt(history, today, bl.sleep_need, cfg, cfg.debt_days)
    s_sleep, sdetail = compute_sleep_score(night, bl.sleep_need, debt_prev, bl.midpoint_median, cfg)
    res.subscores["sleep"] = round(s_sleep, 1)
    res.sleep_detail = sdetail

    if bl.n_nights < cfg.stage_estimating:
        res.state = "calibrating"
        res.summary = STATE_TEXT["calibrating"]
        return res

    # 3. z 分数
    z = {"hrv": bl.z("recovery_hrv", night.recovery_hrv), "late": bl.z("late_hrv", night.late_hrv),
         "early": bl.z("early_hrv", night.early_hrv), "rhr": bl.z("rhr", night.rhr),
         "resp": bl.z("resp", night.resp)}
    if z["hrv"] is None and z["rhr"] is None:
        res.state = "no_core_signal"
        res.summary = STATE_TEXT["no_core_signal"]
        return res
    mb_resp, mb_rhr = bl.get("resp"), bl.get("rhr")
    delta_resp = (night.resp - mb_resp.median) if (night.resp is not None and mb_resp) else None
    rhr_delta = (night.rhr - mb_rhr.median) if (night.rhr is not None and mb_rhr) else None
    r_resp = resp_severity(z["resp"], delta_resp, cfg)
    z["resp_r"] = r_resp

    # 4. 子分
    late_adj = late_adjustment(z["early"], z["late"], z["rhr"], r_resp, cfg)
    s_hrv = hrv_subscore(z["hrv"], cfg)
    if s_hrv is not None:
        s_hrv = clamp(s_hrv + late_adj, 0, cfg.hrv_max)
    else:
        late_adj = 0.0
    s_rhr = None if z["rhr"] is None else 100 - rhr_penalty(z["rhr"], cfg)
    s_resp = None if r_resp is None else 100 - resp_penalty(r_resp, cfg)
    subs = {"hrv": s_hrv, "rhr": s_rhr, "sleep": s_sleep, "resp": s_resp}

    # 5. 缺失项权重按比例重新分配
    avail = {k: w for k, w in cfg.weights.items() if subs[k] is not None}
    tot = sum(avail.values())
    weights = {k: w / tot for k, w in avail.items()}
    raw = sum(weights[k] * subs[k] for k in weights)

    # 6. 置信度 / 不适风险 / 趋势 / 门控
    conf, conf_reasons = confidence_level(night, bl, cfg)
    ill_p, ill_phys, ill_level = illness_points(z, r_resp, rhr_delta, s_sleep, symptoms, cfg)
    deficit_today = min(cfg.debt_night_cap, max(0.0, bl.sleep_need - night.tst_min))
    trend, trend_reasons = trend_adjustment(history, today, z, r_resp, deficit_today, bl.sleep_need, cfg)
    gates = evaluate_gates(z, r_resp, s_sleep, night.tst_min, symptoms, conf, ill_level, cfg)
    cap = min([g["cap"] for g in gates], default=100.0)

    internal = clamp(min(raw + trend, cap), 0, 100)
    score = round_half_up(internal)
    color, tier = tier_of(score, cfg)
    label, advice = ADVICE[tier]

    res.state = "scored"
    res.score, res.score_internal, res.raw = score, round(internal, 2), round(raw, 2)
    res.color, res.tier, res.advice = color, tier, advice
    res.confidence, res.confidence_reasons = conf, conf_reasons + trend_reasons
    res.trend_adjustment, res.late_adjustment, res.cap = round(trend, 2), round(late_adj, 2), cap
    res.subscores = {k: (None if v is None else round(v, 1)) for k, v in subs.items()}
    res.weights_used = {k: round(v, 3) for k, v in weights.items()}
    res.z = {k: (None if v is None else round(v, 3)) for k, v in z.items()}
    res.gates = gates
    res.illness_points, res.illness_physio_points, res.illness_level = ill_p, ill_phys, ill_level
    res.illness_message = ILLNESS_MESSAGES.get(ill_level)
    core = [z["hrv"], z["late"], z["rhr"], r_resp]
    res.baseline_outlier = any(v is not None and abs(v) > cfg.outlier_z for v in core) \
        or ill_level in ("moderate", "high")
    res.top_factors = top_factors(res)
    res.summary = summary_text(res, label, advice)
    return res


class RecoveryEngine:
    """有状态封装：保存历史，逐晚 add_night()。"""

    def __init__(self, cfg: Optional[Config] = None):
        self.cfg = cfg or Config()
        self.history: List[DayRecord] = []

    def add_night(self, night: Union[NightRaw, NightMetrics], symptoms: str = "none") -> RecoveryResult:
        metrics = process_night(night, self.cfg)[0] if isinstance(night, NightRaw) else night
        self.history = [r for r in self.history if r.night.night_date != metrics.night_date]
        res = score_day(metrics, self.history, symptoms, self.cfg)
        self.history.append(DayRecord(metrics, res, symptoms))
        self.history.sort(key=lambda r: r.night.night_date)
        return res
