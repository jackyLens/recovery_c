"""Recovery V1.0 修订版 —— 全部阈值集中在这里（单一事实来源）。

对外分数 0–100（整数显示），内部连续计算。V1.0 不使用体温 / SpO2 / 训练负荷。
"""
from dataclasses import dataclass, field
from typing import Dict, Tuple


def _sigma_floor() -> Dict[str, float]:
    return {"recovery_hrv": 0.12, "late_hrv": 0.15, "early_hrv": 0.15,
            "rhr": 2.0, "resp": 0.5, "sleep_score": 8.0}


def _sigma_prior() -> Dict[str, float]:
    # 人群典型“个体内”日间波动，用于冷启动收缩（不提供绝对水平，只提供波动幅度）
    return {"recovery_hrv": 0.20, "late_hrv": 0.25, "early_hrv": 0.25,
            "rhr": 3.0, "resp": 0.8, "sleep_score": 10.0}


def _weights() -> Dict[str, float]:
    return {"hrv": 0.45, "rhr": 0.20, "sleep": 0.25, "resp": 0.10}


def _sleep_weights() -> Dict[str, float]:
    return {"duration": 0.45, "efficiency": 0.20, "regularity": 0.15,
            "continuity": 0.10, "debt": 0.10}


@dataclass
class Config:
    # ---------------- 5 分钟 HRV 输入校验与质量 ----------------
    segment_sec: int = 300            # 上游 HRV 窗口长度（单条记录未给 duration_sec 时使用）
    rmssd_min_ms: float = 3.0         # RMSSD 合理范围，超出视为无效
    rmssd_max_ms: float = 250.0
    hr_min_bpm: float = 30.0          # 窗口心率合理范围，超出视为缺失
    hr_max_bpm: float = 130.0
    ln_outlier: float = 0.7           # |lnRMSSD − 邻近窗口中位数| 超过此值判为离群，质量置 0
    ln_outlier_neighbors: int = 3     # 邻近窗口：前后各 3 个有效窗
    min_stage_label_frac: float = 0.5  # 窗口被分期覆盖不足一半时按清醒处理（权重 0）
    wake_frac_max: float = 0.2        # 窗口内清醒占比 > 20% 时按清醒处理
    q_good_segment: float = 0.5
    motion_max: float = 0.5

    # ---- 仅供 rr_to_hrv5min 辅助函数使用（上游只能给逐搏 RR 时）----
    rr_min_ms: float = 300.0
    rr_max_ms: float = 2000.0
    rr_local_window: int = 11
    rr_jump_ratio: float = 0.20
    seg_min_beats: int = 150
    min_rmssd_pairs: int = 100
    q_cov_good: float = 0.90
    q_cov_min: float = 0.60
    q_art_good: float = 0.05
    q_art_max: float = 0.20

    # 睡眠阶段权重
    stable_frac: float = 0.8
    w_n3_stable: float = 1.0
    w_n3_mixed: float = 0.8
    w_n2_stable: float = 0.75
    w_n2_mixed: float = 0.5
    w_n1: float = 0.3
    w_rem: float = 0.2
    w_wake: float = 0.0

    # 时间位置权重
    onset_min: float = 45.0
    prewake_min: float = 20.0
    tw_onset: float = 0.6
    tw_prewake: float = 0.0
    tw_first: float = 0.95
    tw_mid: float = 1.0
    tw_late: float = 1.0
    tw_late_bonus: float = 1.10
    mid_band: Tuple[float, float] = (0.4, 0.6)

    # 生理稳定性权重
    stab_hr_mild: float = 6.0
    stab_hr_severe: float = 10.0
    stab_w_mild: float = 0.6
    stab_w_severe: float = 0.3
    stab_resp_dev: float = 3.0
    stab_w_resp: float = 0.6

    # ---------------- 有效性 ----------------
    min_tst_min: float = 180.0
    min_wear_coverage: float = 0.70
    min_good_segments: int = 6
    min_nrem_segments: int = 3
    min_early_late_segments: int = 3
    min_resp_segments: int = 36          # ≈3 小时
    rhr_lowest_segments: int = 6         # 最低 30 分钟
    conf_high_segments: int = 12
    conf_high_wear: float = 0.85
    conf_low_tst: float = 240.0

    # ---------------- 基线 ----------------
    baseline_window: int = 30
    baseline_lookback_days: int = 60
    baseline_min_values: int = 3
    baseline_exclude_tst_min: float = 210.0
    prior_k: float = 5.0
    sigma_floor: Dict[str, float] = field(default_factory=_sigma_floor)
    sigma_prior: Dict[str, float] = field(default_factory=_sigma_prior)
    outlier_z: float = 3.0
    new_normal_run: int = 7
    stage_estimating: int = 3
    stage_trial: int = 7
    stage_provisional: int = 14
    stage_formal: int = 30

    # ---------------- 睡眠子分 ----------------
    sleep_need_default: float = 450.0
    sleep_need_min: float = 390.0
    sleep_need_max: float = 540.0
    debt_days: int = 7
    debt_night_cap: float = 180.0
    debt_need_factor: float = 0.25
    debt_need_cap: float = 60.0
    debt_zero_score: float = 360.0
    duration_zero_ratio: float = 0.5
    se_full: float = 0.90
    se_zero: float = 0.70
    reg_zero_min: float = 120.0
    reg_unknown: float = 85.0
    waso_full: float = 20.0
    waso_zero: float = 90.0
    long_awake_penalty: float = 10.0
    sleep_weights: Dict[str, float] = field(default_factory=_sleep_weights)

    # ---------------- 子分与总分 ----------------
    weights: Dict[str, float] = field(default_factory=_weights)
    hrv_base: float = 70.0
    hrv_slope: float = 18.0
    hrv_max: float = 95.0
    late_neg_start: float = -0.5
    late_neg_slope: float = 6.0
    late_neg_max: float = 6.0
    rebound_early_max: float = -0.5
    rebound_dz_min: float = 0.5
    rebound_max: float = 3.0
    rebound_rhr_max: float = 0.5
    rebound_r_max: float = 1.0
    rhr_p_start: float = 0.5
    rhr_p_slope1: float = 8.0
    rhr_p_knee: float = 1.5
    rhr_p_slope2: float = 14.0
    rhr_p_max: float = 35.0
    # RESP：delta(brpm) → 等效 z；严重度 r = max(z, 等效 z)
    resp_delta_xp: Tuple[float, ...] = (0.0, 0.5, 1.5, 3.0, 4.5)
    resp_delta_fp: Tuple[float, ...] = (0.0, 1.0, 2.0, 3.0, 4.0)
    resp_pen_xp: Tuple[float, ...] = (0.5, 1.0, 2.0, 3.0, 4.0)
    resp_pen_fp: Tuple[float, ...] = (0.0, 8.0, 20.0, 35.0, 40.0)

    # ---------------- 趋势 ----------------
    trend_min: float = -8.0
    trend_max: float = 5.0

    # ---------------- 门控 ----------------
    cap_yellow: float = 66.0
    cap_red: float = 33.0
    y_hrv: float = -0.75
    y_late: float = -1.0
    y_rhr: float = 1.0
    y_resp_r: float = 2.0
    y_sleep: float = 60.0
    y_tst: float = 300.0
    combo_resp_r: float = 1.5          # 组合规则中“RESP 升高”
    sev_hrv: float = -2.0
    sev_rhr: float = 2.0
    sev_resp_r: float = 3.0
    sev_sleep: float = 35.0

    # ---------------- 不适风险 ----------------
    ill_mild: int = 2
    ill_moderate: int = 4
    ill_high: int = 6

    # ---------------- 分档 ----------------
    green_min: int = 67
    yellow_min: int = 34
    tier_peak: int = 85
    tier_high_yellow: int = 50
