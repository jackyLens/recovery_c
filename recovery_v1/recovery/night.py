"""一晚输入（5 分钟 HRV + 带时间的分期） → NightMetrics（Recovery HRV / Early / Late / RHR / RESP / 睡眠结构 / 有效性）。"""
from __future__ import annotations

from typing import List, Optional, Tuple

import numpy as np

from .config import Config
from .models import NREM_STAGES, WAKE, NightMetrics, NightRaw, Segment
from .segments import segment_night, wear_coverage
from .stages import extract_sleep_metrics, normalize_stages, sleep_bounds
from .utils import weighted_median


def process_night(raw: NightRaw, cfg: Optional[Config] = None) -> Tuple[NightMetrics, List[Segment]]:
    cfg = cfg or Config()
    iv = normalize_stages(raw.stages)
    sm = extract_sleep_metrics(iv, raw.in_bed_start)
    segs = segment_night(raw, iv, cfg)
    nm = NightMetrics(night_date=raw.night_date, sleep_score_external=raw.sleep_score_external, **sm)

    in_sleep = [s for s in segs if 0.0 <= s.pos <= 1.0]
    good = [s for s in segs if s.weight > 0 and s.w_quality >= cfg.q_good_segment]
    nrem = [s for s in good if s.stage in NREM_STAGES]
    nm.n_good_segments, nm.n_nrem_segments = len(good), len(nrem)
    onset, offset = sleep_bounds(iv)
    if onset is not None:
        ref = raw.in_bed_start or iv[0][0]
        nm.wear_coverage = wear_coverage(segs, (onset - ref).total_seconds(),
                                         (offset - ref).total_seconds(), cfg.q_good_segment)
    else:
        nm.wear_coverage = 0.0

    # Recovery HRV：加权中位数
    if len(good) >= cfg.min_good_segments and len(nrem) >= cfg.min_nrem_segments:
        nm.recovery_hrv = weighted_median([s.ln_rmssd for s in good], [s.weight for s in good])
        early = [s for s in nrem if s.pos < 0.5]
        late = [s for s in nrem if s.pos >= 0.5]
        if len(early) >= cfg.min_early_late_segments:
            nm.early_hrv = weighted_median([s.ln_rmssd for s in early], [s.weight for s in early])
        if len(late) >= cfg.min_early_late_segments:
            nm.late_hrv = weighted_median([s.ln_rmssd for s in late], [s.weight for s in late])

    # RHR：主睡眠内非清醒、质量合格片段中最低 30 分钟（6 段）平均心率
    hr_cand = sorted(s.hr for s in in_sleep
                     if s.hr is not None and s.stage != WAKE and s.w_quality >= cfg.q_good_segment)
    if len(hr_cand) >= cfg.rhr_lowest_segments:
        nm.rhr = float(np.mean(hr_cand[:cfg.rhr_lowest_segments]))

    # RESP：主睡眠内非清醒片段的中位数，需 ≥3 小时覆盖
    resp = [s.resp for s in in_sleep if s.resp is not None and s.stage != WAKE
            and s.w_quality >= cfg.q_good_segment]
    if len(resp) >= cfg.min_resp_segments:
        nm.resp = float(np.median(resp))

    if nm.tst_min < cfg.min_tst_min:
        nm.valid_night, nm.invalid_reason = False, "sleep_too_short"
    elif nm.wear_coverage < cfg.min_wear_coverage:
        nm.valid_night, nm.invalid_reason = False, "low_wear_coverage"
    return nm, segs
