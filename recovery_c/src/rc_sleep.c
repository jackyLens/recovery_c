/* 睡眠债与睡眠子分（7.4）。不看深睡 / REM 的绝对分钟数。 */
#include "rc_internal.h"

/* 单晚欠的睡眠：max(0, need − tst)，封顶 debt_night_cap */
static double night_deficit(double need, double tst, const rc_config *cfg)
{
    double d = need - tst;

    if (d < 0.0)
        d = 0.0;
    return cfg->debt_night_cap < d ? cfg->debt_night_cap : d;
}

double rc_sleep_debt(const rc_day_record *history, size_t n, rc_date today, double need,
                     const rc_config *cfg, int days)
{
    double debt = 0.0;
    size_t i;

    for (i = 0; i < n; i++) {
        int delta = (int)today - (int)history[i].night.night_date;
        if (delta >= 1 && delta <= days)
            debt += night_deficit(need, history[i].night.tst_min, cfg);
    }
    return debt;
}

double rc_sleep_score(const rc_night_metrics *night, double need, double debt_prev,
                      double midpoint_median, const rc_config *cfg, rc_sleep_detail *detail)
{
    const rc_sleep_weights *w = &cfg->sleep_weights;
    double extra, need_today, ratio, duration, efficiency, regularity, continuity;
    double debt_after, debt_score, score;
    int extra_awakenings;

    /* 有睡眠债时当晚需求上调，最多 debt_need_cap 分钟 */
    extra = cfg->debt_need_factor * debt_prev;
    if (cfg->debt_need_cap < extra)
        extra = cfg->debt_need_cap;
    need_today = need + extra;

    ratio = need_today > 0 ? night->tst_min / need_today : 0.0;
    duration = 100 * rc_ramp(ratio, cfg->duration_zero_ratio, 1.0);
    efficiency = 100 * rc_ramp(night->efficiency, cfg->se_zero, cfg->se_full);

    if (rc_is_na(midpoint_median) || rc_is_na(night->midpoint_min))
        regularity = cfg->reg_unknown;
    else
        regularity = 100 * (1 - rc_ramp(fabs(night->midpoint_min - midpoint_median), 0,
                                        cfg->reg_zero_min));

    /* 连续性：WASO 线性扣分，第 2 次起的长觉醒每次再扣 long_awake_penalty */
    continuity = 100 * (1 - rc_ramp(night->waso_min, cfg->waso_full, cfg->waso_zero));
    extra_awakenings = night->long_awakenings - 1;
    if (extra_awakenings < 0)
        extra_awakenings = 0;
    continuity = rc_clamp(continuity - cfg->long_awake_penalty * extra_awakenings, 0, 100);

    debt_after = debt_prev + night_deficit(need, night->tst_min, cfg);
    debt_score = 100 * (1 - rc_ramp(debt_after, 0, cfg->debt_zero_score));

    score = w->duration * duration;
    score += w->efficiency * efficiency;
    score += w->regularity * regularity;
    score += w->continuity * continuity;
    score += w->debt * debt_score;

    /* 外部睡眠分直接替代，分项照常输出 */
    if (!rc_is_na(night->sleep_score_external))
        score = night->sleep_score_external;

    if (detail) {
        detail->duration = rc_round_dp(duration, 1);
        detail->efficiency = rc_round_dp(efficiency, 1);
        detail->regularity = rc_round_dp(regularity, 1);
        detail->continuity = rc_round_dp(continuity, 1);
        detail->debt = rc_round_dp(debt_score, 1);
        detail->need_min = rc_round_dp(need_today, 1);
        detail->debt_prev_min = rc_round_dp(debt_prev, 1);
        detail->debt_after_min = rc_round_dp(debt_after, 1);
    }
    return rc_clamp(score, 0, 100);
}
