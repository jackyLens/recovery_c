"""数据结构定义。"""
from __future__ import annotations

from dataclasses import dataclass, field
from datetime import date, datetime
from typing import Dict, List, Optional, Union

import numpy as np

# 睡眠分期编码
WAKE, N1, N2, N3, REM = 0, 1, 2, 3, 4
STAGE_NAMES = {WAKE: "Wake", N1: "N1", N2: "N2", N3: "N3/SWS", REM: "REM"}
NREM_STAGES = (N2, N3)

# 用户主观不适标签（可选输入）
SYMPTOMS = ("none", "mild", "moderate_severe")


# 上游分期字符串 → 内部编码。上游只给"浅睡/核心睡眠"不区分 N1/N2 时，按 N2 处理。
STAGE_ALIASES = {
    "wake": WAKE, "awake": WAKE, "w": WAKE, "清醒": WAKE,
    "n1": N1,
    "n2": N2, "light": N2, "core": N2, "浅睡": N2,
    "n3": N3, "deep": N3, "sws": N3, "深睡": N3,
    "rem": REM, "快速眼动": REM,
}


def parse_stage(v: Union[int, str]) -> int:
    if isinstance(v, (int, np.integer)):
        if int(v) not in STAGE_NAMES:
            raise ValueError(f"未知睡眠分期编码: {v}")
        return int(v)
    key = str(v).strip().lower()
    if key not in STAGE_ALIASES:
        raise ValueError(f"未知睡眠分期: {v!r}")
    return STAGE_ALIASES[key]


def _dt(v) -> datetime:
    return v if isinstance(v, datetime) else datetime.fromisoformat(str(v))


@dataclass
class HRVSample:
    """夜间每 5 分钟一条 HRV 记录（戒指 / 上游已算好）。"""
    t_start: datetime                       # 该 5 分钟窗口开始时刻
    rmssd: Optional[float]                  # RMSSD（ms）；NightRaw.hrv_is_ln=True 时为 lnRMSSD；None = 该窗无有效值
    hr_bpm: Optional[float] = None          # 该窗平均心率（可选，用于 RHR 与生理稳定性）
    resp_brpm: Optional[float] = None       # 该窗呼吸率（可选）
    quality: Optional[float] = None         # 该窗数据质量 0–1（可选；缺省按 1）
    motion: Optional[float] = None          # 该窗体动强度 0–1（可选）
    duration_sec: int = 300


@dataclass
class StageInterval:
    """一段连续的睡眠分期，带起止时刻。"""
    start: datetime
    end: datetime
    stage: Union[int, str]                  # WAKE/N1/N2/N3/REM 编码，或 "wake"/"light"/"deep"/"rem" 等字符串


@dataclass
class NightRaw:
    """一晚输入：5 分钟 HRV 序列 + 带时间的睡眠分期。"""
    night_date: date                        # 归属日期 = 醒来当天
    hrv_5min: List[HRVSample]
    stages: List[StageInterval]
    in_bed_start: Optional[datetime] = None  # 上床时刻（可选；缺省取第一段分期开始，用于睡眠效率）
    hrv_is_ln: bool = False                 # True：rmssd 字段已是 lnRMSSD
    sleep_score_external: Optional[float] = None  # 已有睡眠评分时可直接传入

    @classmethod
    def from_dict(cls, d: Dict) -> "NightRaw":
        """从 JSON 字典构造（时间用 ISO 8601 字符串）。"""
        samples = [HRVSample(t_start=_dt(x["t_start"]), rmssd=x.get("rmssd"), hr_bpm=x.get("hr_bpm"),
                             resp_brpm=x.get("resp_brpm"), quality=x.get("quality"), motion=x.get("motion"),
                             duration_sec=int(x.get("duration_sec", 300))) for x in d["hrv_5min"]]
        stages = [StageInterval(start=_dt(x["start"]), end=_dt(x["end"]), stage=x["stage"])
                  for x in d["stages"]]
        nd = d["night_date"]
        return cls(night_date=nd if isinstance(nd, date) else date.fromisoformat(nd), hrv_5min=samples,
                   stages=stages, in_bed_start=_dt(d["in_bed_start"]) if d.get("in_bed_start") else None,
                   hrv_is_ln=bool(d.get("hrv_is_ln", False)),
                   sleep_score_external=d.get("sleep_score_external"))


@dataclass
class Segment:
    idx: int
    t_start_s: float
    stage: int
    stable: bool
    pos: float                  # 在主睡眠中的相对位置 0–1
    coverage: float
    rmssd: Optional[float]
    ln_rmssd: Optional[float]
    hr: Optional[float]
    resp: Optional[float]
    w_quality: float = 0.0
    w_stage: float = 0.0
    w_time: float = 0.0
    w_stability: float = 1.0
    weight: float = 0.0
    dur_s: float = 300.0


@dataclass
class NightMetrics:
    """一晚处理后的指标（评分引擎的输入）。可由 process_night 生成，也可直接构造。"""
    night_date: date
    recovery_hrv: Optional[float] = None   # lnRMSSD
    early_hrv: Optional[float] = None
    late_hrv: Optional[float] = None
    rhr: Optional[float] = None            # bpm
    resp: Optional[float] = None           # brpm
    tst_min: float = 0.0
    efficiency: float = 0.0
    waso_min: float = 0.0
    long_awakenings: int = 0
    midpoint_min: Optional[float] = None   # 睡眠中点，距前一日 12:00 的分钟数
    sleep_score_external: Optional[float] = None
    n_good_segments: int = 0
    n_nrem_segments: int = 0
    wear_coverage: float = 1.0
    valid_night: bool = True
    invalid_reason: Optional[str] = None

    @property
    def hrv_rebound(self) -> Optional[float]:
        if self.early_hrv is None or self.late_hrv is None:
            return None
        return self.late_hrv - self.early_hrv


@dataclass
class RecoveryResult:
    date: date
    state: str = "scored"                  # scored / calibrating / insufficient_data / sleep_too_short / no_core_signal
    score: Optional[int] = None            # 对外 0–100 整数
    score_internal: Optional[float] = None
    color: Optional[str] = None            # green / yellow / red
    tier: Optional[str] = None
    advice: str = ""
    confidence: Optional[str] = None       # high / medium / low
    confidence_reasons: List[str] = field(default_factory=list)
    baseline_stage: str = "calibrating"
    baseline_nights: int = 0
    raw: Optional[float] = None
    trend_adjustment: float = 0.0
    late_adjustment: float = 0.0
    cap: float = 100.0
    subscores: Dict[str, Optional[float]] = field(default_factory=dict)
    weights_used: Dict[str, float] = field(default_factory=dict)
    z: Dict[str, Optional[float]] = field(default_factory=dict)
    gates: List[Dict] = field(default_factory=list)
    illness_points: int = 0
    illness_physio_points: int = 0
    illness_level: str = "none"
    illness_message: Optional[str] = None
    top_factors: List[Dict] = field(default_factory=list)
    summary: str = ""
    baseline_outlier: bool = False
    sleep_detail: Dict[str, float] = field(default_factory=dict)

    def to_dict(self) -> Dict:
        from dataclasses import asdict
        d = asdict(self)
        d["date"] = self.date.isoformat()
        return d


@dataclass
class DayRecord:
    night: NightMetrics
    result: Optional[RecoveryResult]
    symptoms: str = "none"
