/* 子分、趋势、不适风险、门控、置信度、分档（6.2、第 7–10 章）。 */
#include "rc_internal.h"

#include <stdio.h>

/* ---------------------------------------------------------------- 子分（第 7 章） */

/* HRV 子分：hrv_base + hrv_slope × z，限制在 [0, hrv_max]。z 缺失返回 RC_NA。 */
double rc_hrv_subscore(double z_hrv, const rc_config *cfg)
{
    if (rc_is_na(z_hrv))
        return RC_NA;
    return rc_clamp(cfg->hrv_base + cfg->hrv_slope * z_hrv, 0, cfg->hrv_max);
}

/*
 * 后半夜修正（7.1），加到 HRV 子分上。扣分和加分互斥：
 *   后半夜 z 低于 late_neg_start → 按差值线性扣分，最多 late_neg_max；
 *   否则若前半夜偏低而后半夜回升，且 RHR、RESP 都正常 → 加分，最多 rebound_max。
 * z_late 缺失返回 0；z_rhr / r_resp 缺失视为正常。
 */
double rc_late_adjustment(double z_early, double z_late, double z_rhr, double r_resp,
                          const rc_config *cfg)
{
    double gap, neg, dz, bonus;

    if (rc_is_na(z_late))
        return 0.0;
    gap = cfg->late_neg_start - z_late;
    if (!(gap > 0.0))
        gap = 0.0;
    neg = cfg->late_neg_slope * gap;
    if (cfg->late_neg_max < neg)
        neg = cfg->late_neg_max;
    if (neg > 0)
        return -neg;

    if (rc_is_na(z_early))
        return 0.0;
    dz = z_late - z_early;
    if (z_early <= cfg->rebound_early_max && dz >= cfg->rebound_dz_min &&
        (rc_is_na(z_rhr) || z_rhr <= cfg->rebound_rhr_max) &&
        (rc_is_na(r_resp) || r_resp <= cfg->rebound_r_max)) {
        bonus = cfg->rebound_max * (dz - cfg->rebound_dz_min) / 1.0;
        return cfg->rebound_max < bonus ? cfg->rebound_max : bonus;
    }
    return 0.0;
}

/*
 * RHR 扣分（7.2）：z ≤ rhr_p_start 不扣；到拐点前斜率 slope1，之后 slope2，封顶 rhr_p_max。
 * 两段在拐点处连续。只罚升高。
 */
double rc_rhr_penalty(double z_rhr, const rc_config *cfg)
{
    double knee_val, p;

    if (rc_is_na(z_rhr) || z_rhr <= cfg->rhr_p_start)
        return 0.0;
    if (z_rhr <= cfg->rhr_p_knee)
        return cfg->rhr_p_slope1 * (z_rhr - cfg->rhr_p_start);
    knee_val = cfg->rhr_p_slope1 * (cfg->rhr_p_knee - cfg->rhr_p_start);
    p = knee_val + cfg->rhr_p_slope2 * (z_rhr - cfg->rhr_p_knee);
    return cfg->rhr_p_max < p ? cfg->rhr_p_max : p;
}

/*
 * RESP 严重度 r（7.3）= max(z, 绝对升幅折算的等效 z)。
 * 个人波动很小的人 z 容易虚高，波动大的人 z 又偏低，所以同时看绝对升幅。
 * 两个输入都缺失返回 RC_NA；只缺一个时缺的那个按 0。
 */
double rc_resp_severity(double z_resp, double delta_resp, const rc_config *cfg)
{
    double eqz = 0.0, z;

    if (rc_is_na(z_resp) && rc_is_na(delta_resp))
        return RC_NA;
    if (!rc_is_na(delta_resp)) {
        double d = delta_resp > 0.0 ? delta_resp : 0.0;
        eqz = rc_interp(d, cfg->resp_delta_xp, cfg->resp_delta_fp, RC_RESP_TABLE_N,
                        cfg->resp_delta_fp[0], cfg->resp_delta_fp[RC_RESP_TABLE_N - 1]);
    }
    z = rc_is_na(z_resp) ? 0.0 : z_resp;
    return eqz > z ? eqz : z;
}

/* RESP 扣分：严重度查折线表，r 缺失不扣。 */
double rc_resp_penalty(double r, const rc_config *cfg)
{
    if (rc_is_na(r))
        return 0.0;
    return rc_interp(r, cfg->resp_pen_xp, cfg->resp_pen_fp, RC_RESP_TABLE_N, 0.0,
                     cfg->resp_pen_fp[RC_RESP_TABLE_N - 1]);
}

/* ---------------------------------------------------------------- 趋势（8.2） */

static int day_delta(rc_date today, const rc_day_record *r)
{
    return (int)today - (int)r->night.night_date;
}

static int is_scored(const rc_day_record *r)
{
    return r->has_result && r->state == RC_STATE_SCORED;
}

/*
 * 把"前 3 天内有评分的夜"的某项 z（按日期升序）加上今天的值组成序列，跳过缺失。
 * which：0 = z_hrv，1 = z_rhr，2 = resp_r。out 容量 n + 1，返回长度。
 */
static size_t recent_series(const rc_day_record *history, const size_t *order, size_t n,
                            rc_date today, int which, double today_value, double *out)
{
    size_t i, c = 0;

    for (i = 0; i < n; i++) {
        const rc_day_record *r = &history[order[i]];
        int d = day_delta(today, r);
        double v;

        if (!is_scored(r) || d < 1 || d > 3)
            continue;
        v = which == 0 ? r->z_hrv : which == 1 ? r->z_rhr : r->resp_r;
        if (!rc_is_na(v))
            out[c++] = v;
    }
    if (!rc_is_na(today_value))
        out[c++] = today_value;
    return c;
}

static double mean(const double *v, size_t n)
{
    return rc_pairwise_sum(v, n) / (double)n;
}

double rc_trend_adjustment(const rc_day_record *history, const size_t *order, size_t n,
                           rc_date today, double z_hrv, double z_rhr, double r_today,
                           double deficit_today, double need, const rc_config *cfg,
                           double *work, char reasons[][RC_TEXT_MAX], int *n_reasons)
{
    double neg = 0.0, pos = 0.0, debt3, debt_prev2 = 0.0;
    double *hz = work, *rz = work + (n + 1), *tmp = work + 2 * (n + 1);
    size_t nh, nr, nt, i, n_last3 = 0, n_early = 0;
    int prev_normal = 1;

    *n_reasons = 0;

    /* ① HRV 连降同时 RHR 连升：最近 3 个点严格单调，且今天 HRV 在基线下、RHR 在基线上 */
    nh = recent_series(history, order, n, today, 0, z_hrv, hz);
    nr = recent_series(history, order, n, today, 1, z_rhr, rz);
    if (nh >= 3 && nr >= 3) {
        double sh = -rc_slope(hz, nh), sr = rc_slope(rz, nr);
        double mag = sr < sh ? sr : sh;
        int mono = hz[nh - 3] > hz[nh - 2] && hz[nh - 2] > hz[nh - 1] &&
                   rz[nr - 3] < rz[nr - 2] && rz[nr - 2] < rz[nr - 1];

        if (mono && mag >= 0.3 && hz[nh - 1] < 0 && rz[nr - 1] > 0) {
            neg += 5 + 3 * rc_ramp(mag, 0.3, 0.8);
            rc_strcpy(reasons[(*n_reasons)++], RC_TEXT_MAX,
                      "近几晚 HRV 持续下降、静息心率持续上升");
        }
    }

    /* ② RESP 严重度连续 3 晚 ≥ 1 且逐晚升高 */
    nt = recent_series(history, order, n, today, 2, r_today, tmp);
    if (nt >= 3) {
        double a = tmp[nt - 3], b = tmp[nt - 2], c = tmp[nt - 1];
        double lo = a < b ? a : b;

        if (c < lo)
            lo = c;
        if (lo >= 1.0 && a < b && b < c) {
            double s = rc_slope(tmp, nt);
            if (s >= 0.2) {
                neg += 4 + 4 * rc_ramp(s, 0.2, 1.0);
                rc_strcpy(reasons[(*n_reasons)++], RC_TEXT_MAX, "夜间呼吸率连续多晚升高");
            }
        }
    }

    /* ③ 今晚加前两晚的睡眠债 */
    for (i = 0; i < n; i++) {
        int d = day_delta(today, &history[i]);
        if (d >= 1 && d <= 2) {
            double def = need - history[i].night.tst_min;
            if (!(def > 0.0))
                def = 0.0;
            debt_prev2 += cfg->debt_night_cap < def ? cfg->debt_night_cap : def;
        }
    }
    debt3 = deficit_today + debt_prev2;
    if (debt3 >= 120) {
        neg += 3 + 2 * rc_ramp(debt3, 120, 240);
        snprintf(reasons[(*n_reasons)++], RC_TEXT_MAX, "近 3 晚累计少睡约 %d 分钟",
                 (int)rint(debt3));
    }

    /* 前 3 晚都有评分且都正常、今天才变差：按单晚波动处理，扣分减半 */
    for (i = 0; i < n; i++) {
        const rc_day_record *r = &history[order[i]];
        int d = day_delta(today, r);

        if (!is_scored(r) || d < 1 || d > 3)
            continue;
        n_last3++;
        if (!((rc_is_na(r->z_hrv) || r->z_hrv >= -0.5) &&
              (rc_is_na(r->z_rhr) || r->z_rhr <= 0.5)))
            prev_normal = 0;
    }
    if (neg > 0 && n_last3 >= 3 && prev_normal)
        neg *= 0.5;

    /* ④ 4–7 天前 HRV 偏低、近 3 天加今天明显回升，且今天 RHR / RESP 正常：加分 */
    for (i = 0; i < n; i++) {
        const rc_day_record *r = &history[order[i]];
        int d = day_delta(today, r);

        if (is_scored(r) && d >= 4 && d <= 7 && !rc_is_na(r->z_hrv))
            tmp[n_early++] = r->z_hrv;
    }
    if (n_early >= 2 && nh >= 2) {
        double m_early = mean(tmp, n_early);
        double gain = mean(hz, nh) - m_early;

        if (m_early <= -0.5 && gain >= 0.5 && (rc_is_na(z_rhr) || z_rhr <= 0.5) &&
            (rc_is_na(r_today) || r_today <= 1.0)) {
            pos = 2 + 3 * rc_ramp(gain, 0.5, 1.5);
            rc_strcpy(reasons[(*n_reasons)++], RC_TEXT_MAX, "HRV 较前几天明显回升");
        }
    }

    return rc_clamp(pos - neg, cfg->trend_min, cfg->trend_max);
}

/* ---------------------------------------------------------------- 不适风险（第 10 章） */

int rc_illness_points(const rc_zscores *z, double rhr_delta, double s_sleep, int symptoms,
                      const rc_config *cfg, int *points, int *physio)
{
    int p = 0;

    if (!rc_is_na(z->resp_r))
        p += z->resp_r > 3 ? 3 : (z->resp_r > 2 ? 2 : 0);
    if (!rc_is_na(z->rhr)) {
        /* z 不算高但绝对值比基线高 5 bpm 以上也记 2 点 */
        if (z->rhr > 2.5)
            p += 3;
        else if (z->rhr > 1.5 || (!rc_is_na(rhr_delta) && rhr_delta >= 5))
            p += 2;
    }
    if (!rc_is_na(z->hrv))
        p += z->hrv < -2 ? 2 : (z->hrv < -1 ? 1 : 0);
    if (!rc_is_na(z->late) && z->late < -1.5)
        p += 1;
    if (!rc_is_na(s_sleep) && s_sleep < 50)
        p += 1;
    *physio = p;

    if (symptoms == RC_SYMPTOMS_MILD)
        p += 2;
    else if (symptoms == RC_SYMPTOMS_MODERATE_SEVERE)
        p += 3;
    *points = p;

    if (p >= cfg->ill_high)
        return RC_ILL_HIGH;
    if (p >= cfg->ill_moderate)
        return RC_ILL_MODERATE;
    if (p >= cfg->ill_mild)
        return RC_ILL_MILD;
    return RC_ILL_NONE;
}

/* ---------------------------------------------------------------- 门控（第 9 章） */

static void add_gate(rc_gate *gates, int *n, int code, const rc_config *cfg, const char *text)
{
    rc_gate *g = &gates[(*n)++];

    g->code = code;
    g->level = code <= RC_GATE_R_ILLNESS ? RC_GATE_RED : RC_GATE_YELLOW;
    g->cap = g->level == RC_GATE_RED ? cfg->cap_red : cfg->cap_yellow;
    rc_strcpy(g->text, sizeof g->text, text);
}

/*
 * 所有封顶规则都在这一张表里。缺失的指标不触发任何规则。
 * 没有任何门控触发 = 有 Green 资格。
 */
int rc_evaluate_gates(const rc_zscores *z, double s_sleep, double tst, int symptoms,
                      int confidence, int illness_level, const rc_config *cfg, rc_gate *gates)
{
    double zh = z->hrv, zl = z->late, zr = z->rhr, r = z->resp_r;
    int has_h = !rc_is_na(zh), has_l = !rc_is_na(zl), has_r = !rc_is_na(zr);
    int has_resp = !rc_is_na(r), has_sleep = !rc_is_na(s_sleep);
    int rhr_up = has_r && zr > cfg->y_rhr;
    int resp_up = has_resp && r > cfg->combo_resp_r;
    int severe, n = 0;
    char buf[RC_TEXT_MAX];

    /* Red */
    if (has_h && zh < -1.5 && (rhr_up || resp_up))
        add_gate(gates, &n, RC_GATE_R_HRV_COMBO, cfg,
                 "HRV 明显低于基线，同时静息心率或呼吸率升高");
    if (has_l && zl < -1.5 && (rhr_up || resp_up))
        add_gate(gates, &n, RC_GATE_R_LATE_COMBO, cfg,
                 "后半夜 HRV 恢复明显不足，同时静息心率或呼吸率升高");
    if (has_resp && r > cfg->sev_resp_r && (rhr_up || symptoms != RC_SYMPTOMS_NONE))
        add_gate(gates, &n, RC_GATE_R_RESP_COMBO, cfg,
                 "夜间呼吸率显著升高，并伴随心率升高或不适");
    severe = (has_h && zh < cfg->sev_hrv) + (has_r && zr > cfg->sev_rhr) +
             (has_resp && r > cfg->sev_resp_r) + (has_sleep && s_sleep < cfg->sev_sleep);
    if (severe >= 2)
        add_gate(gates, &n, RC_GATE_R_MULTI_SEVERE, cfg, "两项及以上核心指标严重偏离基线");
    if (has_sleep && s_sleep < cfg->sev_sleep)
        add_gate(gates, &n, RC_GATE_R_SLEEP, cfg, "睡眠严重不足或质量很差");
    if (symptoms == RC_SYMPTOMS_MODERATE_SEVERE)
        add_gate(gates, &n, RC_GATE_R_SYMPTOM, cfg, "你标记了明显不适");
    if (illness_level == RC_ILL_HIGH)
        add_gate(gates, &n, RC_GATE_R_ILLNESS, cfg, "多项身体信号明显偏离个人基线");

    /* Yellow */
    if (has_h && zh < cfg->y_hrv)
        add_gate(gates, &n, RC_GATE_Y_HRV, cfg, "整夜 HRV 低于个人基线");
    if (has_l && zl < cfg->y_late)
        add_gate(gates, &n, RC_GATE_Y_LATE, cfg, "后半夜 HRV 恢复不足");
    if (has_r && zr > cfg->y_rhr)
        add_gate(gates, &n, RC_GATE_Y_RHR, cfg, "静息心率高于个人基线");
    if (has_resp && r > cfg->y_resp_r)
        add_gate(gates, &n, RC_GATE_Y_RESP, cfg, "夜间呼吸率高于个人基线");
    if (has_sleep && s_sleep < cfg->y_sleep)
        add_gate(gates, &n, RC_GATE_Y_SLEEP, cfg, "睡眠质量或时长不足");
    if (tst < cfg->y_tst) {
        snprintf(buf, sizeof buf, "总睡眠不足 %d 小时", (int)floor(cfg->y_tst / 60));
        add_gate(gates, &n, RC_GATE_Y_TST, cfg, buf);
    }
    if (symptoms == RC_SYMPTOMS_MILD)
        add_gate(gates, &n, RC_GATE_Y_SYMPTOM, cfg, "你标记了轻微不适");
    if (confidence == RC_CONF_LOW)
        add_gate(gates, &n, RC_GATE_Y_LOW_CONF, cfg, "今日数据可信度较低，结果仅供参考");
    if (illness_level == RC_ILL_MODERATE)
        add_gate(gates, &n, RC_GATE_Y_ILLNESS, cfg, "多项夜间指标偏离个人基线");
    return n;
}

/* ---------------------------------------------------------------- 置信度（6.2） */

int rc_confidence_level(const rc_night_metrics *night, const rc_baseline *bl,
                        const rc_config *cfg, char reasons[][RC_TEXT_MAX], int *n_reasons)
{
    int n = 0, low = 0, high;

    /* 任何一条成立即为 low */
    if (bl->stage == RC_BL_ESTIMATING) {
        low = 1;
        snprintf(reasons[n++], RC_TEXT_MAX, "基线建立中（%d 晚）", bl->n_nights);
    }
    if (rc_is_na(night->recovery_hrv) || bl->metrics[RC_METRIC_RECOVERY_HRV].n == 0) {
        low = 1;
        rc_strcpy(reasons[n++], RC_TEXT_MAX, "今晚 HRV 数据不足");
    }
    if (night->n_good_segments < cfg->min_good_segments) {
        low = 1;
        rc_strcpy(reasons[n++], RC_TEXT_MAX, "有效 HRV 片段过少");
    }
    if (night->tst_min < cfg->conf_low_tst) {
        low = 1;
        rc_strcpy(reasons[n++], RC_TEXT_MAX, "睡眠时长偏短");
    }
    *n_reasons = n;
    if (low)
        return RC_CONF_LOW;

    /* high 要求：正式基线、片段够多、佩戴够好、有呼吸率 */
    high = bl->stage == RC_BL_FORMAL && night->n_good_segments >= cfg->conf_high_segments &&
           night->wear_coverage >= cfg->conf_high_wear;
    if (high && !rc_is_na(night->resp))
        return RC_CONF_HIGH;

    if (bl->stage == RC_BL_TRIAL || bl->stage == RC_BL_PROVISIONAL)
        snprintf(reasons[n++], RC_TEXT_MAX, "基线%s期（%d 晚）",
                 bl->stage == RC_BL_TRIAL ? "试用" : "暂定", bl->n_nights);
    if (night->n_good_segments < cfg->conf_high_segments)
        rc_strcpy(reasons[n++], RC_TEXT_MAX, "有效 HRV 片段偏少");
    if (night->wear_coverage < cfg->conf_high_wear)
        rc_strcpy(reasons[n++], RC_TEXT_MAX, "佩戴覆盖率偏低");
    if (rc_is_na(night->resp))
        rc_strcpy(reasons[n++], RC_TEXT_MAX, "呼吸率数据不足");
    *n_reasons = n;
    return RC_CONF_MEDIUM;
}

/* ---------------------------------------------------------------- 分档（1.3） */

void rc_tier_of(int score, const rc_config *cfg, int *color, int *tier)
{
    if (score >= cfg->tier_peak) {
        *color = RC_COLOR_GREEN;
        *tier = RC_TIER_PEAK;
    } else if (score >= cfg->green_min) {
        *color = RC_COLOR_GREEN;
        *tier = RC_TIER_GREEN;
    } else if (score >= cfg->tier_high_yellow) {
        *color = RC_COLOR_YELLOW;
        *tier = RC_TIER_HIGH_YELLOW;
    } else if (score >= cfg->yellow_min) {
        *color = RC_COLOR_YELLOW;
        *tier = RC_TIER_LOW_YELLOW;
    } else {
        *color = RC_COLOR_RED;
        *tier = RC_TIER_RED;
    }
}
