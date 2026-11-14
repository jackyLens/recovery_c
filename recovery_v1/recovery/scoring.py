"""子分、趋势、门控、不适风险、分档与置信度（V1.0 修订版，无体温 / SpO2 / 训练负荷）。"""
from __future__ import annotations

from datetime import date
from typing import Dict, List, Optional, Tuple

import numpy as np

from .baseline import Baseline
from .config import Config
from .models import DayRecord, NightMetrics
from .utils import clamp, ramp


# ---------------------------------------------------------------- 子分
def hrv_subscore(z_hrv: Optional[float], cfg: Config) -> Optional[float]:
    if z_hrv is None:
        return None
    return clamp(cfg.hrv_base + cfg.hrv_slope * z_hrv, 0, cfg.hrv_max)


def late_adjustment(z_early: Optional[float], z_late: Optional[float], z_rhr: Optional[float],
                    r_resp: Optional[float], cfg: Config) -> float:
    """后半夜修正：负向（后半夜偏低）与正向（前低后高的回升）互斥，均为连续函数。"""
    if z_late is None:
        return 0.0
    neg = min(cfg.late_neg_max, cfg.late_neg_slope * max(0.0, cfg.late_neg_start - z_late))
    if neg > 0:
        return -neg
    if z_early is None:
        return 0.0
    dz = z_late - z_early
    if (z_early <= cfg.rebound_early_max and dz >= cfg.rebound_dz_min
            and (z_rhr is None or z_rhr <= cfg.rebound_rhr_max)
            and (r_resp is None or r_resp <= cfg.rebound_r_max)):
        return min(cfg.rebound_max, cfg.rebound_max * (dz - cfg.rebound_dz_min) / 1.0)
    return 0.0


def rhr_penalty(z_rhr: Optional[float], cfg: Config) -> float:
    if z_rhr is None or z_rhr <= cfg.rhr_p_start:
        return 0.0
    if z_rhr <= cfg.rhr_p_knee:
        return cfg.rhr_p_slope1 * (z_rhr - cfg.rhr_p_start)
    knee_val = cfg.rhr_p_slope1 * (cfg.rhr_p_knee - cfg.rhr_p_start)
    return min(cfg.rhr_p_max, knee_val + cfg.rhr_p_slope2 * (z_rhr - cfg.rhr_p_knee))


def resp_severity(z_resp: Optional[float], delta_resp: Optional[float], cfg: Config) -> Optional[float]:
    """r = max(z_RESP, 由绝对升幅换算的等效 z)。只关心升高，降低不惩罚。"""
    if z_resp is None and delta_resp is None:
        return None
    eqz = float(np.interp(max(0.0, delta_resp), cfg.resp_delta_xp, cfg.resp_delta_fp)) if delta_resp is not None else 0.0
    return max(z_resp if z_resp is not None else 0.0, eqz)


def resp_penalty(r: Optional[float], cfg: Config) -> float:
    if r is None:
        return 0.0
    return float(np.interp(r, cfg.resp_pen_xp, cfg.resp_pen_fp, left=0.0, right=cfg.resp_pen_fp[-1]))


# ---------------------------------------------------------------- 趋势
def _slope(y: List[float]) -> float:
    if len(y) < 3:
        return 0.0
    x = np.arange(len(y), dtype=float)
    return float(np.polyfit(x, np.asarray(y, float), 1)[0])


def trend_adjustment(history: List[DayRecord], today: date, z_today: Dict[str, Optional[float]],
                     r_today: Optional[float], deficit_today: float, need: float,
                     cfg: Config) -> Tuple[float, List[str]]:
    """基于"最近 3 个有评分的夜 + 今天"的 z 序列。返回 (修正值, 原因)。"""
    recent = sorted([r for r in history if r.result is not None and r.result.state == "scored"
                     and 1 <= (today - r.night.night_date).days <= 7], key=lambda r: r.night.night_date)
    last3 = [r for r in recent if (today - r.night.night_date).days <= 3]
    reasons: List[str] = []
    neg = 0.0
    pos = 0.0

    def ser(key, recs):
        return [r.result.z.get(key) for r in recs]

    # ① HRV 连续下降且 RHR 同步上升
    hz = [v for v in ser("hrv", last3) + [z_today.get("hrv")] if v is not None]
    rz = [v for v in ser("rhr", last3) + [z_today.get("rhr")] if v is not None]
    if len(hz) >= 3 and len(rz) >= 3:
        mag = min(-_slope(hz), _slope(rz))
        mono = hz[-3] > hz[-2] > hz[-1] and rz[-3] < rz[-2] < rz[-1]   # 至少连续 3 晚同向
        if mono and mag >= 0.3 and hz[-1] < 0 and rz[-1] > 0:
            neg += 5 + 3 * ramp(mag, 0.3, 0.8)
            reasons.append("近几晚 HRV 持续下降、静息心率持续上升")
    # ② RESP 连续升高
    rr = [r.result.z.get("resp_r") for r in last3] + [r_today]
    rr = [v for v in rr if v is not None]
    if len(rr) >= 3 and min(rr[-3:]) >= 1.0 and rr[-3] < rr[-2] < rr[-1]:
        s = _slope(rr)
        if s >= 0.2:
            neg += 4 + 4 * ramp(s, 0.2, 1.0)
            reasons.append("夜间呼吸率连续多晚升高")
    # ③ 近 3 晚睡眠债
    debt3 = deficit_today + sum(min(cfg.debt_night_cap, max(0.0, need - r.night.tst_min))
                                for r in history if 1 <= (today - r.night.night_date).days <= 2)
    if debt3 >= 120:
        neg += 3 + 2 * ramp(debt3, 120, 240)
        reasons.append(f"近 3 晚累计少睡约 {int(round(debt3))} 分钟")
    # 单日异常：前 3 晚都正常，今天才突然变差 → 负向减半（避免单晚噪声被放大）
    prev_normal = len(last3) >= 3 and all(
        (r.result.z.get("hrv") is None or r.result.z["hrv"] >= -0.5)
        and (r.result.z.get("rhr") is None or r.result.z["rhr"] <= 0.5) for r in last3)
    if neg > 0 and prev_normal:
        neg *= 0.5
    # ④ 7 天内 HRV 回升，且今天 RHR / RESP 正常
    early = [r.result.z.get("hrv") for r in recent if (today - r.night.night_date).days >= 4]
    early = [v for v in early if v is not None]
    late = [v for v in ser("hrv", last3) + [z_today.get("hrv")] if v is not None]
    if len(early) >= 2 and len(late) >= 2:
        gain = float(np.mean(late) - np.mean(early))
        if (np.mean(early) <= -0.5 and gain >= 0.5
                and (z_today.get("rhr") is None or z_today["rhr"] <= 0.5)
                and (r_today is None or r_today <= 1.0)):
            pos = 2 + 3 * ramp(gain, 0.5, 1.5)
            reasons.append("HRV 较前几天明显回升")
    return clamp(pos - neg, cfg.trend_min, cfg.trend_max), reasons


# ---------------------------------------------------------------- 不适风险（无体温重配权）
ILLNESS_MESSAGES = {
    "mild": "你的夜间恢复指标出现轻度异常，今天可以适当降低强度，多补充水分并保证睡眠。",
    "moderate": "你的夜间恢复指标出现多项异常，与平时状态相比明显偏离。建议今天以休息为主，注意身体感受。",
    "high": "你的夜间多项身体信号明显偏离个人基线。建议今天充分休息；如果同时感到明显不适，请关注身体状况，必要时咨询专业人士。",
}


def illness_points(z: Dict[str, Optional[float]], r_resp: Optional[float], rhr_delta: Optional[float],
                   s_sleep: Optional[float], symptoms: str, cfg: Config) -> Tuple[int, int, str]:
    p = 0
    if r_resp is not None:
        p += 3 if r_resp > 3 else (2 if r_resp > 2 else 0)
    zr = z.get("rhr")
    if zr is not None:
        if zr > 2.5:
            p += 3
        elif zr > 1.5 or (rhr_delta is not None and rhr_delta >= 5):
            p += 2
    zh = z.get("hrv")
    if zh is not None:
        p += 2 if zh < -2 else (1 if zh < -1 else 0)
    zl = z.get("late")
    if zl is not None and zl < -1.5:
        p += 1
    if s_sleep is not None and s_sleep < 50:
        p += 1
    physio = p
    p += {"none": 0, "mild": 2, "moderate_severe": 3}.get(symptoms, 0)
    level = ("high" if p >= cfg.ill_high else "moderate" if p >= cfg.ill_moderate
             else "mild" if p >= cfg.ill_mild else "none")
    return p, physio, level


# ---------------------------------------------------------------- 门控（单一规则表）
def evaluate_gates(z: Dict[str, Optional[float]], r_resp: Optional[float], s_sleep: Optional[float],
                   tst: float, symptoms: str, confidence: str, illness_level: str,
                   cfg: Config) -> List[Dict]:
    """返回触发的门控列表 [{code, cap, level, text}]。Green 资格 = 没有任何 Yellow/Red 触发。"""
    g: List[Dict] = []
    zh, zl, zr = z.get("hrv"), z.get("late"), z.get("rhr")

    def add(code, level, text):
        g.append({"code": code, "level": level, "cap": cfg.cap_red if level == "red" else cfg.cap_yellow,
                  "text": text})

    rhr_up = zr is not None and zr > cfg.y_rhr
    resp_up = r_resp is not None and r_resp > cfg.combo_resp_r
    # ---- Red
    if zh is not None and zh < -1.5 and (rhr_up or resp_up):
        add("R_HRV_COMBO", "red", "HRV 明显低于基线，同时静息心率或呼吸率升高")
    if zl is not None and zl < -1.5 and (rhr_up or resp_up):
        add("R_LATE_COMBO", "red", "后半夜 HRV 恢复明显不足，同时静息心率或呼吸率升高")
    if r_resp is not None and r_resp > cfg.sev_resp_r and (rhr_up or symptoms != "none"):
        add("R_RESP_COMBO", "red", "夜间呼吸率显著升高，并伴随心率升高或不适")
    severe = sum([zh is not None and zh < cfg.sev_hrv, zr is not None and zr > cfg.sev_rhr,
                  r_resp is not None and r_resp > cfg.sev_resp_r, s_sleep is not None and s_sleep < cfg.sev_sleep])
    if severe >= 2:
        add("R_MULTI_SEVERE", "red", "两项及以上核心指标严重偏离基线")
    if s_sleep is not None and s_sleep < cfg.sev_sleep:
        add("R_SLEEP", "red", "睡眠严重不足或质量很差")
    if symptoms == "moderate_severe":
        add("R_SYMPTOM", "red", "你标记了明显不适")
    if illness_level == "high":
        add("R_ILLNESS", "red", "多项身体信号明显偏离个人基线")
    # ---- Yellow（同时也是 Green 资格的反面）
    if zh is not None and zh < cfg.y_hrv:
        add("Y_HRV", "yellow", "整夜 HRV 低于个人基线")
    if zl is not None and zl < cfg.y_late:
        add("Y_LATE", "yellow", "后半夜 HRV 恢复不足")
    if zr is not None and zr > cfg.y_rhr:
        add("Y_RHR", "yellow", "静息心率高于个人基线")
    if r_resp is not None and r_resp > cfg.y_resp_r:
        add("Y_RESP", "yellow", "夜间呼吸率高于个人基线")
    if s_sleep is not None and s_sleep < cfg.y_sleep:
        add("Y_SLEEP", "yellow", "睡眠质量或时长不足")
    if tst < cfg.y_tst:
        add("Y_TST", "yellow", f"总睡眠不足 {int(cfg.y_tst // 60)} 小时")
    if symptoms == "mild":
        add("Y_SYMPTOM", "yellow", "你标记了轻微不适")
    if confidence == "low":
        add("Y_LOW_CONF", "yellow", "今日数据可信度较低，结果仅供参考")
    if illness_level == "moderate":
        add("Y_ILLNESS", "yellow", "多项夜间指标偏离个人基线")
    return g


# ---------------------------------------------------------------- 置信度
def confidence_level(night: NightMetrics, bl: Baseline, cfg: Config) -> Tuple[str, List[str]]:
    reasons: List[str] = []
    low = False
    if bl.stage == "estimating":
        low = True; reasons.append(f"基线建立中（{bl.n_nights} 晚）")
    if night.recovery_hrv is None or bl.get("recovery_hrv") is None:
        low = True; reasons.append("今晚 HRV 数据不足")
    if night.n_good_segments < cfg.min_good_segments:
        low = True; reasons.append("有效 HRV 片段过少")
    if night.tst_min < cfg.conf_low_tst:
        low = True; reasons.append("睡眠时长偏短")
    if low:
        return "low", reasons
    high = (bl.stage == "formal" and night.n_good_segments >= cfg.conf_high_segments
            and night.wear_coverage >= cfg.conf_high_wear)
    if high and night.resp is not None:
        return "high", reasons
    if bl.stage in ("trial", "provisional"):
        reasons.append(f"基线{'试用' if bl.stage == 'trial' else '暂定'}期（{bl.n_nights} 晚）")
    if night.n_good_segments < cfg.conf_high_segments:
        reasons.append("有效 HRV 片段偏少")
    if night.wear_coverage < cfg.conf_high_wear:
        reasons.append("佩戴覆盖率偏低")
    if night.resp is None:
        reasons.append("呼吸率数据不足")
    return "medium", reasons


# ---------------------------------------------------------------- 分档与建议
ADVICE = {
    "peak": ("状态很好", "身体恢复充分，适合安排高强度训练或重要任务。"),
    "green": ("恢复良好", "可以正常训练和工作，按计划进行。"),
    "high_yellow": ("恢复一般", "可以活动，但建议降低强度，避免冲击极限。"),
    "low_yellow": ("恢复偏低", "建议以轻松活动为主，比如散步、拉伸，并早点休息。"),
    "red": ("需要休息", "身体信号提示恢复不足，今天建议以休息为主，避免高强度训练。"),
}


def tier_of(score: int, cfg: Config) -> Tuple[str, str]:
    if score >= cfg.tier_peak:
        return "green", "peak"
    if score >= cfg.green_min:
        return "green", "green"
    if score >= cfg.tier_high_yellow:
        return "yellow", "high_yellow"
    if score >= cfg.yellow_min:
        return "yellow", "low_yellow"
    return "red", "red"
