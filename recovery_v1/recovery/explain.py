"""可解释性：挑出影响今日分数最大的 2–3 个因素，生成中文文案。"""
from __future__ import annotations

from typing import Dict, List, Optional


def _fmt(v: Optional[float]) -> str:
    return "—" if v is None else f"{v:+.1f}"


_GATE_KEY = {"HRV": "hrv", "LATE": "late", "RHR": "rhr", "RESP": "resp", "SLEEP": "sleep", "TST": "sleep"}


def top_factors(res, max_n: int = 3) -> List[Dict]:
    out: List[Dict] = []
    seen = set()
    # 1) 触发的门控优先（红 > 黄）
    for g in sorted(res.gates, key=lambda g: 0 if g["level"] == "red" else 1):
        if g["code"] in ("Y_LOW_CONF",):
            continue
        key = _GATE_KEY.get(g["code"].split("_")[1], g["code"])
        if key in seen:
            continue
        seen.add(key)
        out.append({"key": g["code"], "direction": "negative", "impact": None, "text": g["text"]})
        if len(out) >= max_n:
            return out
    # 2) 按对原始分的贡献排序
    s, w = res.subscores, res.weights_used
    cand = []
    if s.get("hrv") is not None:
        imp = (s["hrv"] - 70.0) * w.get("hrv", 0)
        cand.append(("hrv", imp, "整夜 HRV 高于个人基线" if imp > 0 else "整夜 HRV 低于个人基线"))
    if res.late_adjustment:
        imp = res.late_adjustment * w.get("hrv", 0)
        cand.append(("late", imp, "后半夜 HRV 明显回升，恢复在夜里逐步完成" if imp > 0 else "后半夜 HRV 回升不足"))
    if s.get("rhr") is not None:
        imp = (s["rhr"] - 100.0) * w.get("rhr", 0)
        cand.append(("rhr", imp if imp < 0 else 0.5, "静息心率高于个人基线" if imp < 0 else "静息心率处于个人正常范围"))
    if s.get("sleep") is not None:
        imp = (s["sleep"] - 80.0) * w.get("sleep", 0)
        cand.append(("sleep", imp, "睡眠充足、连续" if imp > 0 else "睡眠时长或质量不足"))
    if s.get("resp") is not None:
        imp = (s["resp"] - 100.0) * w.get("resp", 0)
        cand.append(("resp", imp if imp < 0 else 0.3, "夜间呼吸率偏高" if imp < 0 else "夜间呼吸率平稳"))
    if res.trend_adjustment:
        cand.append(("trend", res.trend_adjustment,
                     "近几天整体在回升" if res.trend_adjustment > 0 else "近几天指标持续走弱"))
    # 负分时优先展示负向因素，正分时优先展示正向因素
    neg_first = res.color != "green"
    cand.sort(key=lambda c: (-(abs(c[1])) if (c[1] < 0) == neg_first else 1000 - abs(c[1])))
    for key, imp, text in cand:
        if key in seen or abs(imp) < 0.3:
            continue
        seen.add(key)
        out.append({"key": key, "direction": "positive" if imp > 0 else "negative",
                    "impact": round(imp, 1), "text": text})
        if len(out) >= max_n:
            break
    return out


def summary_text(res, label: str, advice: str) -> str:
    if not res.top_factors:
        return f"{label}。{advice}"
    main = "；".join(f["text"] for f in res.top_factors[:2])
    return f"{label}：{main}。{advice}"


STATE_TEXT = {
    "calibrating": "正在建立你的个人基线，再佩戴几晚即可看到恢复分。",
    "sleep_too_short": "昨晚睡眠时间太短，无法可靠评估恢复状态。",
    "insufficient_data": "昨晚有效佩戴数据不足，今天暂不给出恢复分。",
    "no_core_signal": "昨晚 HRV 和静息心率均无法可靠获取，今天暂不给出恢复分。",
}
