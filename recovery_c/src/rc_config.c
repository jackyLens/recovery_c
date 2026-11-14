/* 默认配置。数值与 Python 版 config.py 逐项相同，改一边要同步另一边。 */
#include "rc_internal.h"

#include <string.h>

void rc_config_default(rc_config *c)
{
    static const double sigma_floor[RC_METRIC_COUNT] = { 0.12, 0.15, 0.15, 2.0, 0.5, 8.0 };
    static const double sigma_prior[RC_METRIC_COUNT] = { 0.20, 0.25, 0.25, 3.0, 0.8, 10.0 };
    static const double resp_delta_xp[RC_RESP_TABLE_N] = { 0.0, 0.5, 1.5, 3.0, 4.5 };
    static const double resp_delta_fp[RC_RESP_TABLE_N] = { 0.0, 1.0, 2.0, 3.0, 4.0 };
    static const double resp_pen_xp[RC_RESP_TABLE_N]   = { 0.5, 1.0, 2.0, 3.0, 4.0 };
    static const double resp_pen_fp[RC_RESP_TABLE_N]   = { 0.0, 8.0, 20.0, 35.0, 40.0 };

    if (!c)
        return;
    memset(c, 0, sizeof *c);

    c->segment_sec = 300;
    c->rmssd_min_ms = 3.0;
    c->rmssd_max_ms = 250.0;
    c->hr_min_bpm = 30.0;
    c->hr_max_bpm = 130.0;
    c->ln_outlier = 0.7;
    c->ln_outlier_neighbors = 3;
    c->min_stage_label_frac = 0.5;
    c->wake_frac_max = 0.2;
    c->q_good_segment = 0.5;
    c->motion_max = 0.5;

    c->rr_min_ms = 300.0;
    c->rr_max_ms = 2000.0;
    c->rr_local_window = 11;
    c->rr_jump_ratio = 0.20;
    c->seg_min_beats = 150;
    c->min_rmssd_pairs = 100;
    c->q_cov_good = 0.90;
    c->q_cov_min = 0.60;
    c->q_art_good = 0.05;
    c->q_art_max = 0.20;

    c->stable_frac = 0.8;
    c->w_n3_stable = 1.0;
    c->w_n3_mixed = 0.8;
    c->w_n2_stable = 0.75;
    c->w_n2_mixed = 0.5;
    c->w_n1 = 0.3;
    c->w_rem = 0.2;
    c->w_wake = 0.0;

    c->onset_min = 45.0;
    c->prewake_min = 20.0;
    c->tw_onset = 0.6;
    c->tw_prewake = 0.0;
    c->tw_first = 0.95;
    c->tw_mid = 1.0;
    c->tw_late = 1.0;
    c->tw_late_bonus = 1.10;
    c->mid_band[0] = 0.4;
    c->mid_band[1] = 0.6;

    c->stab_hr_mild = 6.0;
    c->stab_hr_severe = 10.0;
    c->stab_w_mild = 0.6;
    c->stab_w_severe = 0.3;
    c->stab_resp_dev = 3.0;
    c->stab_w_resp = 0.6;

    c->min_tst_min = 180.0;
    c->min_wear_coverage = 0.70;
    c->min_good_segments = 6;
    c->min_nrem_segments = 3;
    c->min_early_late_segments = 3;
    c->min_resp_segments = 36;
    c->rhr_lowest_segments = 6;
    c->conf_high_segments = 12;
    c->conf_high_wear = 0.85;
    c->conf_low_tst = 240.0;

    c->baseline_window = 30;
    c->baseline_lookback_days = 60;
    c->baseline_min_values = 3;
    c->baseline_exclude_tst_min = 210.0;
    c->prior_k = 5.0;
    memcpy(c->sigma_floor, sigma_floor, sizeof sigma_floor);
    memcpy(c->sigma_prior, sigma_prior, sizeof sigma_prior);
    c->outlier_z = 3.0;
    c->new_normal_run = 7;
    c->stage_estimating = 3;
    c->stage_trial = 7;
    c->stage_provisional = 14;
    c->stage_formal = 30;

    c->sleep_need_default = 450.0;
    c->sleep_need_min = 390.0;
    c->sleep_need_max = 540.0;
    c->debt_days = 7;
    c->debt_night_cap = 180.0;
    c->debt_need_factor = 0.25;
    c->debt_need_cap = 60.0;
    c->debt_zero_score = 360.0;
    c->duration_zero_ratio = 0.5;
    c->se_full = 0.90;
    c->se_zero = 0.70;
    c->reg_zero_min = 120.0;
    c->reg_unknown = 85.0;
    c->waso_full = 20.0;
    c->waso_zero = 90.0;
    c->long_awake_penalty = 10.0;
    c->sleep_weights.duration = 0.45;
    c->sleep_weights.efficiency = 0.20;
    c->sleep_weights.regularity = 0.15;
    c->sleep_weights.continuity = 0.10;
    c->sleep_weights.debt = 0.10;

    c->weights.hrv = 0.45;
    c->weights.rhr = 0.20;
    c->weights.sleep = 0.25;
    c->weights.resp = 0.10;
    c->hrv_base = 70.0;
    c->hrv_slope = 18.0;
    c->hrv_max = 95.0;
    c->late_neg_start = -0.5;
    c->late_neg_slope = 6.0;
    c->late_neg_max = 6.0;
    c->rebound_early_max = -0.5;
    c->rebound_dz_min = 0.5;
    c->rebound_max = 3.0;
    c->rebound_rhr_max = 0.5;
    c->rebound_r_max = 1.0;
    c->rhr_p_start = 0.5;
    c->rhr_p_slope1 = 8.0;
    c->rhr_p_knee = 1.5;
    c->rhr_p_slope2 = 14.0;
    c->rhr_p_max = 35.0;
    memcpy(c->resp_delta_xp, resp_delta_xp, sizeof resp_delta_xp);
    memcpy(c->resp_delta_fp, resp_delta_fp, sizeof resp_delta_fp);
    memcpy(c->resp_pen_xp, resp_pen_xp, sizeof resp_pen_xp);
    memcpy(c->resp_pen_fp, resp_pen_fp, sizeof resp_pen_fp);

    c->trend_min = -8.0;
    c->trend_max = 5.0;

    c->cap_yellow = 66.0;
    c->cap_red = 33.0;
    c->y_hrv = -0.75;
    c->y_late = -1.0;
    c->y_rhr = 1.0;
    c->y_resp_r = 2.0;
    c->y_sleep = 60.0;
    c->y_tst = 300.0;
    c->combo_resp_r = 1.5;
    c->sev_hrv = -2.0;
    c->sev_rhr = 2.0;
    c->sev_resp_r = 3.0;
    c->sev_sleep = 35.0;

    c->ill_mild = 2;
    c->ill_moderate = 4;
    c->ill_high = 6;

    c->green_min = 67;
    c->yellow_min = 34;
    c->tier_peak = 85;
    c->tier_high_yellow = 50;
}
