/*
 * test_recovery.c —— 单元测试。
 *
 * 前半部分对应 Python 版 tests/test_recovery.py 的用例（函数名相同，去掉 test_ 前缀后一致），
 * 后半部分是 C 接口自己的东西：解析函数、日期、错误码、缓冲区容量、引擎的装载和裁剪。
 * 需要整晚数据的用例用 synth.c 生成，数值与 Python 的合成数据不同，所以这里只断言性质；
 * 与 Python 的逐字段比对在 test_parity.c。
 *
 * 全部通过返回 0。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rc_internal.h"
#include "recovery.h"
#include "synth.h"

static int g_checks, g_failed_checks, g_tests, g_failed_tests;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        g_checks++;                                                                 \
        if (!(cond)) {                                                              \
            g_failed_checks++;                                                      \
            printf("    %s:%d: 断言失败：%s\n", __FILE__, __LINE__, #cond);         \
        }                                                                           \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                       \
    do {                                                                            \
        double a_ = (a), b_ = (b);                                                  \
        g_checks++;                                                                 \
        if (!(fabs(a_ - b_) <= (tol))) {                                            \
            g_failed_checks++;                                                      \
            printf("    %s:%d: %s = %.12g，期望 %.12g（±%g）\n", __FILE__, __LINE__, \
                   #a, a_, b_, (double)(tol));                                      \
        }                                                                           \
    } while (0)

#define CHECK_STR(a, b)                                                             \
    do {                                                                            \
        const char *a_ = (a), *b_ = (b);                                            \
        g_checks++;                                                                 \
        if (strcmp(a_, b_) != 0) {                                                  \
            g_failed_checks++;                                                      \
            printf("    %s:%d: %s = \"%s\"，期望 \"%s\"\n", __FILE__, __LINE__, #a,  \
                   a_, b_);                                                         \
        }                                                                           \
    } while (0)

#define RUN(fn) run_test(#fn, fn)

static void run_test(const char *name, void (*fn)(void))
{
    int before = g_failed_checks;

    g_tests++;
    fn();
    if (g_failed_checks != before) {
        g_failed_tests++;
        printf("FAIL %s\n", name);
    }
}

/* ------------------------------------------------------------------ 公共数据 */

#define BASE_NIGHTS 35

static rc_config CFG;
static rc_date D0, DNEXT;
static rc_day_record g_base[BASE_NIGHTS];   /* 35 晚正常数据的历史，各用例从这里起步 */

/* 生成一晚并加入引擎。res、m 可以为 NULL。返回 rc_engine_add_night 的返回值。 */
static int add_synth(rc_engine *e, rc_date d, const synth_opts *o, int symptoms, rc_result *res,
                     rc_night_metrics *m)
{
    synth_night n;
    rc_result tmp;
    int rc;

    if (!synth_make(d, o, &n))
        return RC_ERR_NOMEM;
    rc = rc_engine_add_night(e, &n.in, symptoms, res ? res : &tmp, m);
    synth_free(&n);
    return rc;
}

/* 生成一晚并只做单晚处理。 */
static int process_synth(rc_date d, const synth_opts *o, rc_night_metrics *m)
{
    synth_night n;
    int rc;

    if (!synth_make(d, o, &n))
        return RC_ERR_NOMEM;
    rc = rc_process_night(&n.in, NULL, m, NULL, 0, NULL);
    synth_free(&n);
    return rc;
}

static int build_base(void)
{
    rc_engine *e = rc_engine_create(NULL);
    unsigned rng = 7;
    int i;

    if (!e)
        return 0;
    for (i = 0; i < BASE_NIGHTS; i++) {
        synth_opts o;
        synth_opts_default(&o, 1000u + (unsigned)i);
        o.tst_min = synth_gauss(&rng, 450.0, 20.0);
        if (add_synth(e, D0 + i, &o, RC_SYMPTOMS_NONE, NULL, NULL) != RC_OK) {
            rc_engine_destroy(e);
            return 0;
        }
    }
    memcpy(g_base, rc_engine_history(e), sizeof g_base);
    rc_engine_destroy(e);
    return 1;
}

/* 带着 35 晚历史的新引擎 */
static rc_engine *base_engine(void)
{
    rc_engine *e = rc_engine_create(NULL);

    if (e && rc_engine_load(e, g_base, BASE_NIGHTS) != RC_OK) {
        rc_engine_destroy(e);
        e = NULL;
    }
    if (!e) {
        fprintf(stderr, "内存不足\n");
        exit(2);
    }
    return e;
}

/* 在 35 晚历史后面加一晚，返回结果 */
static rc_result score_next(const synth_opts *o, int symptoms)
{
    rc_engine *e = base_engine();
    rc_result r;

    CHECK(add_synth(e, DNEXT, o, symptoms, &r, NULL) == RC_OK);
    rc_engine_destroy(e);
    return r;
}

static rc_result score_next_metrics(const rc_night_metrics *m)
{
    rc_engine *e = base_engine();
    rc_result r;

    CHECK(rc_engine_add_metrics(e, m, RC_SYMPTOMS_NONE, &r) == RC_OK);
    rc_engine_destroy(e);
    return r;
}

/* 参与加权的权重之和 */
static double weight_sum(const rc_result *r)
{
    const double w[4] = { r->weights_used.hrv, r->weights_used.rhr, r->weights_used.sleep,
                          r->weights_used.resp };
    double s = 0.0;
    int i;

    for (i = 0; i < 4; i++)
        if (!rc_is_na(w[i]))
            s += w[i];
    return s;
}

static int has_gate(const rc_gate *g, int n, int code)
{
    int i;

    for (i = 0; i < n; i++)
        if (g[i].code == code)
            return 1;
    return 0;
}

static double min_cap(const rc_gate *g, int n)
{
    double c = 100.0;
    int i;

    for (i = 0; i < n; i++)
        if (g[i].cap < c)
            c = g[i].cap;
    return c;
}

static rc_zscores zs(double hrv, double late, double rhr, double resp_r)
{
    rc_zscores z;

    z.hrv = hrv;
    z.late = late;
    z.early = RC_NA;
    z.rhr = rhr;
    z.resp = RC_NA;
    z.resp_r = resp_r;
    return z;
}

/* ------------------------------------------------------------------ 工具函数 */

static void round_half_up(void)
{
    CHECK(rc_round_half_up(66.5) == 67);
    CHECK(rc_round_half_up(66.49) == 66);
    CHECK(rc_round_half_up(0.5) == 1);
}

static void weighted_median(void)
{
    double v1[] = { 1, 2, 3 }, w1[] = { 1, 1, 1 };
    double v2[] = { 1, 2, 3 }, w2[] = { 0, 0, 1 };
    double v3[] = { 3, 1, 2 }, w3[] = { 1, 5, 1 };
    double v4[] = { 1, 2 }, w4[] = { 0, 0 };

    CHECK(rc_weighted_median(v1, w1, 3) == 2);
    CHECK(rc_weighted_median(v2, w2, 3) == 3);
    CHECK(rc_weighted_median(v3, w3, 3) == 1);
    CHECK(rc_is_na(rc_weighted_median(v4, w4, 2)));
    CHECK(rc_is_na(rc_weighted_median(v4, w4, 0)));
}

/* Python 的 round() 对 2.675、0.125 这类值的结果 */
static void round_dp_matches_python(void)
{
    CHECK(rc_round_dp(2.675, 2) == 2.67);
    CHECK(rc_round_dp(0.125, 2) == 0.12);
    CHECK(rc_round_dp(2.5, 0) == 2.0);
    CHECK(rc_round_dp(3.5, 0) == 4.0);
    CHECK(rc_is_na(rc_round_dp(RC_NA, 2)));
}

static void median_interp_sum(void)
{
    double a[] = { 5, 1, 3 }, b[] = { 4, 1, 3, 2 };
    const double xp[] = { 0, 1, 3 }, fp[] = { 0, 10, 30 };
    double big[1000], s = 0.0;
    int i;

    CHECK(rc_median(a, 3) == 3);
    CHECK(rc_median(b, 4) == 2.5);
    CHECK(rc_is_na(rc_median(a, 0)));
    CHECK_NEAR(rc_interp(0.5, xp, fp, 3, -1, 99), 5.0, 1e-12);
    CHECK_NEAR(rc_interp(2.0, xp, fp, 3, -1, 99), 20.0, 1e-12);
    CHECK(rc_interp(-1.0, xp, fp, 3, -1, 99) == -1);
    CHECK(rc_interp(4.0, xp, fp, 3, -1, 99) == 99);
    CHECK(rc_interp(3.0, xp, fp, 3, -1, 99) == 30);
    for (i = 0; i < 1000; i++) {
        big[i] = 0.1 * (i + 1);
        s += big[i];
    }
    CHECK_NEAR(rc_pairwise_sum(big, 1000), s, 1e-8);
    CHECK_NEAR(rc_pairwise_sum(big, 1000), 50050.0, 1e-9);
    CHECK(rc_pairwise_sum(big, 0) == 0.0);
    CHECK(rc_clamp(5, 0, 3) == 3 && rc_clamp(-1, 0, 3) == 0 && rc_clamp(2, 0, 3) == 2);
    CHECK_NEAR(rc_ramp(15, 10, 20), 0.5, 1e-12);
    CHECK(rc_ramp(25, 10, 20) == 1 && rc_ramp(5, 10, 20) == 0);
}

/* ------------------------------------------------------------------ 子分 */

static void hrv_cap_95(void)
{
    CHECK(rc_hrv_subscore(5.0, &CFG) == 95.0);
    CHECK(rc_hrv_subscore(0.0, &CFG) == 70.0);
    CHECK(rc_hrv_subscore(-10.0, &CFG) == 0.0);
}

static void rhr_penalty_continuous_monotone(void)
{
    double prev = rc_rhr_penalty(-2.0, &CFG), max_step = 0.0;
    int i, monotone = 1;

    for (i = 1; i <= 800; i++) {
        double p = rc_rhr_penalty(-2.0 + 0.01 * i, &CFG);
        if (p < prev - 1e-9)
            monotone = 0;
        if (fabs(p - prev) > max_step)
            max_step = fabs(p - prev);
        prev = p;
    }
    CHECK(monotone);
    CHECK(max_step < 0.2);   /* 没有台阶 */
    CHECK_NEAR(rc_rhr_penalty(1.5, &CFG), 8.0, 1e-9);
    CHECK(rc_rhr_penalty(10.0, &CFG) == 35.0);
}

static void resp_penalty_continuous_monotone(void)
{
    double prev = rc_resp_penalty(0.0, &CFG), max_step = 0.0;
    int i, monotone = 1;

    for (i = 1; i <= 600; i++) {
        double p = rc_resp_penalty(0.01 * i, &CFG);
        if (p < prev - 1e-9)
            monotone = 0;
        if (fabs(p - prev) > max_step)
            max_step = fabs(p - prev);
        prev = p;
    }
    CHECK(monotone);
    CHECK(max_step < 0.2);
}

static void resp_severity_uses_absolute_delta(void)
{
    /* z 很小但绝对升幅 3 次/分，等效 z = 3 */
    CHECK_NEAR(rc_resp_severity(0.2, 3.0, &CFG), 3.0, 1e-9);
    CHECK_NEAR(rc_resp_severity(2.5, 0.0, &CFG), 2.5, 1e-9);
    CHECK(rc_is_na(rc_resp_severity(RC_NA, RC_NA, &CFG)));
}

static void late_adjustment_exclusive_and_bounded(void)
{
    CHECK_NEAR(rc_late_adjustment(0, -0.5, 0, 0, &CFG), 0.0, 1e-12);
    CHECK_NEAR(rc_late_adjustment(0, -3, 0, 0, &CFG), -6.0, 1e-12);
    CHECK(rc_late_adjustment(-1.0, 0.5, 0, 0, &CFG) > 0);
    CHECK(rc_late_adjustment(-3.0, 3.0, 0, 0, &CFG) <= 3.0);
    /* 回升但 RHR 高，不给加分 */
    CHECK(rc_late_adjustment(-1.0, 0.5, 1.0, 0, &CFG) == 0.0);
    CHECK(rc_late_adjustment(-1.0, RC_NA, 0, 0, &CFG) == 0.0);
}

/* ------------------------------------------------------------------ 分档与门控 */

static void check_tier(int score, int color, int tier)
{
    int c = -1, t = -1;

    rc_tier_of(score, &CFG, &c, &t);
    CHECK(c == color);
    CHECK(t == tier);
}

static void tier_boundaries(void)
{
    check_tier(100, RC_COLOR_GREEN, RC_TIER_PEAK);
    check_tier(85, RC_COLOR_GREEN, RC_TIER_PEAK);
    check_tier(84, RC_COLOR_GREEN, RC_TIER_GREEN);
    check_tier(67, RC_COLOR_GREEN, RC_TIER_GREEN);
    check_tier(66, RC_COLOR_YELLOW, RC_TIER_HIGH_YELLOW);
    check_tier(50, RC_COLOR_YELLOW, RC_TIER_HIGH_YELLOW);
    check_tier(49, RC_COLOR_YELLOW, RC_TIER_LOW_YELLOW);
    check_tier(34, RC_COLOR_YELLOW, RC_TIER_LOW_YELLOW);
    check_tier(33, RC_COLOR_RED, RC_TIER_RED);
    check_tier(0, RC_COLOR_RED, RC_TIER_RED);
}

static void caps_match_bands(void)
{
    int c, t;

    rc_tier_of((int)CFG.cap_yellow, &CFG, &c, &t);
    CHECK(c == RC_COLOR_YELLOW);
    rc_tier_of((int)CFG.cap_red, &CFG, &c, &t);
    CHECK(c == RC_COLOR_RED);
}

static void clean_night_has_no_gate(void)
{
    rc_gate g[RC_MAX_GATES];
    rc_zscores z = zs(0.2, 0.1, -0.2, 0.3);

    CHECK(rc_evaluate_gates(&z, 85, 450, RC_SYMPTOMS_NONE, RC_CONF_HIGH, RC_ILL_NONE, &CFG, g) == 0);
}

static void false_recovery_never_green(void)
{
    rc_gate g[RC_MAX_GATES];
    rc_zscores z = zs(1.0, 0.8, 1.6, 2.2);
    int n = rc_evaluate_gates(&z, 85, 450, RC_SYMPTOMS_NONE, RC_CONF_HIGH, RC_ILL_NONE, &CFG, g);

    CHECK(n > 0);
    CHECK(min_cap(g, n) <= 66);
}

static void red_rules(void)
{
    rc_gate g[RC_MAX_GATES];
    rc_zscores z = zs(-1.8, -1.0, 1.3, 0.5);
    int n = rc_evaluate_gates(&z, 80, 450, RC_SYMPTOMS_NONE, RC_CONF_HIGH, RC_ILL_NONE, &CFG, g);

    CHECK(has_gate(g, n, RC_GATE_R_HRV_COMBO));
    z = zs(0, 0, 0, 0);
    n = rc_evaluate_gates(&z, 80, 450, RC_SYMPTOMS_MODERATE_SEVERE, RC_CONF_HIGH, RC_ILL_NONE, &CFG, g);
    CHECK(has_gate(g, n, RC_GATE_R_SYMPTOM));
    CHECK(min_cap(g, n) == 33);
}

static void missing_resp_drops_resp_gates(void)
{
    rc_gate g[RC_MAX_GATES];
    rc_zscores z = zs(0.1, 0.1, 0.1, RC_NA);

    CHECK(rc_evaluate_gates(&z, 85, 450, RC_SYMPTOMS_NONE, RC_CONF_HIGH, RC_ILL_NONE, &CFG, g) == 0);
}

/* 门控按枚举顺序输出，红在黄前 */
static void gates_in_enum_order(void)
{
    rc_gate g[RC_MAX_GATES];
    rc_zscores z = zs(-2.5, -2.0, 2.5, 3.5);
    int n = rc_evaluate_gates(&z, 30, 200, RC_SYMPTOMS_MODERATE_SEVERE, RC_CONF_LOW, RC_ILL_HIGH,
                              &CFG, g);
    int i;

    CHECK(n > 4 && n <= RC_MAX_GATES);
    for (i = 1; i < n; i++)
        CHECK(g[i].code > g[i - 1].code);
    for (i = 0; i < n; i++) {
        CHECK(g[i].cap == (g[i].level == RC_GATE_RED ? CFG.cap_red : CFG.cap_yellow));
        CHECK(g[i].text[0] != '\0');
    }
}

/* ------------------------------------------------------------------ 单晚处理 */

static void metrics_reasonable(void)
{
    synth_opts o;
    rc_night_metrics m;

    synth_opts_default(&o, 1);
    CHECK(process_synth(D0, &o, &m) == RC_OK);
    CHECK(m.valid_night);
    CHECK(m.recovery_hrv > 3.5 && m.recovery_hrv < 4.5);
    CHECK(m.rhr > 48 && m.rhr < 60);
    CHECK(m.resp > 13 && m.resp < 16);
    CHECK_NEAR(m.tst_min, 450, 1);
    CHECK(m.efficiency > 0.9 && m.efficiency <= 1.0);
    CHECK(m.wear_coverage > 0.95);
    CHECK(m.midpoint_min >= 0 && m.midpoint_min < 1440);
}

static void short_sleep_invalid(void)
{
    synth_opts o;
    rc_night_metrics m;

    synth_opts_default(&o, 1);
    o.tst_min = 150;
    CHECK(process_synth(D0, &o, &m) == RC_OK);
    CHECK(!m.valid_night);
    CHECK(m.invalid_reason == RC_INVALID_SLEEP_TOO_SHORT);
}

/* ------------------------------------------------------------------ 引擎 */

static void cold_start(void)
{
    rc_engine *e = rc_engine_create(NULL);
    rc_result r[5];
    int i;

    for (i = 0; i < 5; i++) {
        synth_opts o;
        synth_opts_default(&o, 300u + (unsigned)i);
        CHECK(add_synth(e, D0 + i, &o, RC_SYMPTOMS_NONE, &r[i], NULL) == RC_OK);
    }
    for (i = 0; i < 3; i++) {
        CHECK(r[i].state == RC_STATE_CALIBRATING);
        CHECK(r[i].score == -1);
        CHECK(r[i].summary[0] != '\0');
    }
    CHECK(r[3].state == RC_STATE_SCORED);
    CHECK(r[3].confidence == RC_CONF_LOW);
    CHECK(r[3].score >= 0 && r[3].score <= 66);
    CHECK(r[3].baseline_stage == RC_BL_ESTIMATING);
    CHECK(has_gate(r[3].gates, r[3].n_gates, RC_GATE_Y_LOW_CONF));
    rc_engine_destroy(e);
}

static void normal_night_green_integer(void)
{
    synth_opts o;
    rc_result r;
    int c, t;

    synth_opts_default(&o, 11);
    o.night_noise = 0;
    r = score_next(&o, RC_SYMPTOMS_NONE);
    CHECK(r.state == RC_STATE_SCORED);
    CHECK(r.score >= 0 && r.score <= 100);
    CHECK(r.color == RC_COLOR_GREEN);
    CHECK(r.confidence == RC_CONF_HIGH);
    CHECK_NEAR(weight_sum(&r), 1.0, 0.01);
    CHECK(r.baseline_stage == RC_BL_FORMAL);
    rc_tier_of(r.score, &CFG, &c, &t);
    CHECK(r.color == c && r.tier == t);
    CHECK(r.advice[0] != '\0' && r.summary[0] != '\0');
    CHECK(r.n_top_factors >= 1 && r.n_top_factors <= RC_MAX_FACTORS);
}

static void fatigue_red(void)
{
    synth_opts o;
    rc_result r;

    synth_opts_default(&o, 12);
    o.night_noise = 0;
    o.ln_shift = -0.35;
    o.hr_shift = 5;
    r = score_next(&o, RC_SYMPTOMS_NONE);
    CHECK(r.color == RC_COLOR_RED);
    CHECK(r.score >= 0 && r.score <= 33);
    CHECK(r.cap == 33);
    CHECK(r.top_factors[0].key == RC_FACTOR_GATE);
}

static void false_recovery_capped(void)
{
    synth_opts o;
    rc_result r;

    synth_opts_default(&o, 13);
    o.night_noise = 0;
    o.ln_shift = 0.1;
    o.hr_shift = 3;
    o.resp_shift = 1.0;
    r = score_next(&o, RC_SYMPTOMS_NONE);
    CHECK(r.state == RC_STATE_SCORED);
    CHECK(r.color != RC_COLOR_GREEN);
    CHECK(r.score <= (int)r.cap);
}

static void sleep_too_short_no_score(void)
{
    synth_opts o;
    rc_result r;

    synth_opts_default(&o, 14);
    o.tst_min = 150;
    r = score_next(&o, RC_SYMPTOMS_NONE);
    CHECK(r.state == RC_STATE_SLEEP_TOO_SHORT);
    CHECK(r.score == -1);
    CHECK(r.color == RC_COLOR_NONE && r.tier == RC_TIER_NONE && r.confidence == RC_CONF_NONE);
    CHECK(rc_is_na(r.score_internal) && rc_is_na(r.raw));
    CHECK(r.n_gates == 0 && r.n_top_factors == 0);
    CHECK_STR(r.advice, "");
}

static void missing_hrv_redistributes(void)
{
    synth_opts o;
    rc_night_metrics m;
    rc_result r;

    synth_opts_default(&o, 15);
    o.night_noise = 0;
    CHECK(process_synth(DNEXT, &o, &m) == RC_OK);
    m.recovery_hrv = m.early_hrv = m.late_hrv = RC_NA;
    m.n_good_segments = 0;
    r = score_next_metrics(&m);
    CHECK(r.state == RC_STATE_SCORED);
    CHECK(rc_is_na(r.weights_used.hrv));
    CHECK(rc_is_na(r.subscores.hrv));
    CHECK_NEAR(weight_sum(&r), 1.0, 0.01);
    CHECK(r.confidence == RC_CONF_LOW);
}

static void missing_resp(void)
{
    synth_opts o;
    rc_result r;

    synth_opts_default(&o, 16);
    o.night_noise = 0;
    o.drop_resp = 1;
    r = score_next(&o, RC_SYMPTOMS_NONE);
    CHECK(r.state == RC_STATE_SCORED);
    CHECK(rc_is_na(r.weights_used.resp));
    CHECK(rc_is_na(r.z.resp_r));
    CHECK(r.confidence != RC_CONF_HIGH);
    CHECK_NEAR(weight_sum(&r), 1.0, 0.01);
}

static void no_core_signal(void)
{
    synth_opts o;
    rc_night_metrics m;
    rc_result r;

    synth_opts_default(&o, 17);
    CHECK(process_synth(DNEXT, &o, &m) == RC_OK);
    m.recovery_hrv = m.early_hrv = m.late_hrv = m.rhr = RC_NA;
    r = score_next_metrics(&m);
    CHECK(r.state == RC_STATE_NO_CORE_SIGNAL);
    CHECK(r.score == -1);
}

static void baseline_uses_only_past(void)
{
    rc_baseline b1, b2;
    int k;

    CHECK(rc_build_baseline(g_base, BASE_NIGHTS, D0 + 20, &CFG, &b1) == RC_OK);
    CHECK(rc_build_baseline(g_base, 20, D0 + 20, &CFG, &b2) == RC_OK);
    for (k = 0; k < RC_METRIC_COUNT; k++) {
        CHECK(b1.metrics[k].n == b2.metrics[k].n);
        CHECK(b1.metrics[k].median == b2.metrics[k].median);
        CHECK(b1.metrics[k].sigma == b2.metrics[k].sigma);
    }
    CHECK(b1.n_nights == b2.n_nights);
    CHECK(b1.n_nights <= 20);
}

static void new_normal_readmission(void)
{
    rc_day_record h[BASE_NIGHTS];
    rc_baseline full, b;
    int i;

    CHECK(rc_build_baseline(g_base, BASE_NIGHTS, DNEXT, &CFG, &full) == RC_OK);

    /* 末尾 8 晚全是离群：连续满 7 晚，按新常态重新纳入 */
    memcpy(h, g_base, sizeof h);
    for (i = BASE_NIGHTS - 8; i < BASE_NIGHTS; i++)
        h[i].baseline_outlier = 1;
    CHECK(rc_build_baseline(h, BASE_NIGHTS, DNEXT, &CFG, &b) == RC_OK);
    CHECK(b.readmitted == 8);

    /* 只有末尾 3 晚离群：不够 7 晚，不纳入 */
    for (i = BASE_NIGHTS - 8; i < BASE_NIGHTS - 3; i++)
        h[i].baseline_outlier = 0;
    CHECK(rc_build_baseline(h, BASE_NIGHTS, DNEXT, &CFG, &b) == RC_OK);
    CHECK(b.readmitted == 0);
    CHECK(b.n_nights == full.n_nights - 3 || full.n_nights == CFG.baseline_window);
    CHECK(b.excluded >= 3);
}

static void reprocessing_same_date_replaces(void)
{
    rc_engine *e = base_engine();
    synth_opts o;
    rc_result r1, r2, r3;

    synth_opts_default(&o, 18);
    CHECK(add_synth(e, DNEXT, &o, RC_SYMPTOMS_NONE, &r1, NULL) == RC_OK);
    o.seed = 19;
    o.ln_shift = -0.3;
    CHECK(add_synth(e, DNEXT, &o, RC_SYMPTOMS_NONE, &r2, NULL) == RC_OK);
    CHECK(rc_engine_count(e) == BASE_NIGHTS + 1);
    /* 第二次的结果不受第一次影响：换回原来的输入，分数应该和第一次一样 */
    o.seed = 18;
    o.ln_shift = 0;
    CHECK(add_synth(e, DNEXT, &o, RC_SYMPTOMS_NONE, &r3, NULL) == RC_OK);
    CHECK(r3.score == r1.score && r3.score_internal == r1.score_internal);
    CHECK(rc_engine_count(e) == BASE_NIGHTS + 1);
    rc_engine_destroy(e);
}

static void trend_bounds_and_symptoms(void)
{
    synth_opts o;
    rc_result r;

    synth_opts_default(&o, 20);
    r = score_next(&o, RC_SYMPTOMS_MODERATE_SEVERE);
    CHECK(r.color == RC_COLOR_RED);
    CHECK(r.trend_adjustment >= CFG.trend_min && r.trend_adjustment <= CFG.trend_max);
    CHECK(has_gate(r.gates, r.n_gates, RC_GATE_R_SYMPTOM));
    CHECK(r.illness_points > r.illness_physio_points);
}

/* 连续几晚变差：趋势修正为负且不越界 */
static void trend_negative_on_decline(void)
{
    rc_engine *e = base_engine();
    rc_result r;
    int k;

    for (k = 0; k < 4; k++) {
        synth_opts o;
        synth_opts_default(&o, 200u + (unsigned)k);
        o.night_noise = 0;
        o.ln_shift = -0.08 * (k + 1);
        o.hr_shift = 1.2 * (k + 1);
        CHECK(add_synth(e, DNEXT + k, &o, RC_SYMPTOMS_NONE, &r, NULL) == RC_OK);
        CHECK(r.trend_adjustment >= CFG.trend_min && r.trend_adjustment <= CFG.trend_max);
    }
    CHECK(r.trend_adjustment < 0);
    rc_engine_destroy(e);
}

/* ------------------------------------------------------------------ V1.0.2 输入 */

#define T0 ((int64_t)rc_date_from_ymd(2026, 8, 31) * 86400 + 23 * 3600)

typedef struct { int a, b; const char *stage; } span;   /* 相对 T0 的分钟 */

/* 按分钟区间和文字分期拼一晚：每 5 分钟一条 rmssd=40、hr=55、resp=14 的记录 */
static int process_spans(const span *sp, size_t n_sp, rc_night_metrics *m)
{
    rc_hrv_sample hrv[128];
    rc_stage_interval st[16];
    rc_night_input in;
    size_t n = (size_t)(sp[n_sp - 1].b - sp[0].a) / 5, i;

    for (i = 0; i < n; i++) {
        rc_hrv_sample_init(&hrv[i], T0 + 300 * (int64_t)i, 40.0);
        hrv[i].hr_bpm = 55.0;
        hrv[i].resp_brpm = 14.0;
    }
    for (i = 0; i < n_sp; i++) {
        st[i].start = T0 + 60 * (int64_t)sp[i].a;
        st[i].end = T0 + 60 * (int64_t)sp[i].b;
        if (rc_parse_stage(sp[i].stage, &st[i].stage) != RC_OK)
            return RC_ERR_STAGE;
    }
    rc_night_input_init(&in, D0);
    in.hrv = hrv;
    in.n_hrv = n;
    in.stages = st;
    in.n_stages = n_sp;
    return rc_process_night(&in, NULL, m, NULL, 0, NULL);
}

static void sleep_metrics_from_intervals(void)
{
    const span sp[] = { { 0, 15, "wake" },   { 15, 200, "light" }, { 200, 210, "wake" },
                        { 210, 213, "wake" }, { 213, 400, "deep" }, { 400, 420, "rem" },
                        { 420, 430, "wake" } };
    rc_night_metrics m;

    CHECK(process_spans(sp, 7, &m) == RC_OK);
    CHECK_NEAR(m.tst_min, 185 + 187 + 20, 1e-6);
    CHECK_NEAR(m.waso_min, 13, 1e-6);
    CHECK(m.long_awakenings == 1);    /* 两段相邻清醒合并为一次 13 分钟夜醒 */
    CHECK_NEAR(m.efficiency, 392.0 / 420.0, 1e-6);
    CHECK_NEAR(m.recovery_hrv, log(40.0), 1e-12);
    CHECK_NEAR(m.rhr, 55.0, 1e-12);
    CHECK_NEAR(m.resp, 14.0, 1e-12);
    /* 入睡 23:15，醒来 06:00，中点 02:37:30，距前一日 12:00 是 877 分钟（秒被截掉） */
    CHECK_NEAR(m.midpoint_min, 877.0, 1e-9);
}

/* 时区偏移只移动睡眠中点 */
static void utc_offset_shifts_midpoint_only(void)
{
    synth_opts o;
    synth_night n;
    rc_night_metrics a, b;

    synth_opts_default(&o, 31);
    CHECK(synth_make(D0, &o, &n));
    CHECK(rc_process_night(&n.in, NULL, &a, NULL, 0, NULL) == RC_OK);
    n.in.utc_offset_sec = 2 * 3600;
    CHECK(rc_process_night(&n.in, NULL, &b, NULL, 0, NULL) == RC_OK);
    CHECK_NEAR(fmod(b.midpoint_min - a.midpoint_min + 1440.0, 1440.0), 120.0, 1e-9);
    CHECK(a.recovery_hrv == b.recovery_hrv && a.tst_min == b.tst_min && a.rhr == b.rhr);
    synth_free(&n);
}

static void string_and_code_stages_equivalent(void)
{
    static const char *const text[5] = { "wake", "n1", "light", "deep", "rem" };
    synth_opts o;
    synth_night n;
    rc_night_metrics a, b;
    size_t i;

    synth_opts_default(&o, 3);
    CHECK(synth_make(D0, &o, &n));
    CHECK(rc_process_night(&n.in, NULL, &a, NULL, 0, NULL) == RC_OK);
    for (i = 0; i < n.in.n_stages; i++) {
        int code = -1;
        CHECK(rc_parse_stage(text[n.stages[i].stage], &code) == RC_OK);
        n.stages[i].stage = code;
    }
    CHECK(rc_process_night(&n.in, NULL, &b, NULL, 0, NULL) == RC_OK);
    CHECK(a.tst_min == b.tst_min);
    CHECK(a.recovery_hrv == b.recovery_hrv);
    CHECK(b.valid_night && !rc_is_na(b.recovery_hrv));
    synth_free(&n);
}

static void ln_input_equivalent(void)
{
    synth_opts o;
    synth_night n;
    rc_night_metrics a, b;
    size_t i;

    synth_opts_default(&o, 4);
    CHECK(synth_make(D0, &o, &n));
    CHECK(rc_process_night(&n.in, NULL, &a, NULL, 0, NULL) == RC_OK);
    for (i = 0; i < n.in.n_hrv; i++)
        n.hrv[i].rmssd = log(n.hrv[i].rmssd);
    n.in.hrv_is_ln = 1;
    CHECK(rc_process_night(&n.in, NULL, &b, NULL, 0, NULL) == RC_OK);
    CHECK_NEAR(a.recovery_hrv, b.recovery_hrv, 1e-9);
    CHECK_NEAR(a.late_hrv, b.late_hrv, 1e-9);
    synth_free(&n);
}

static void outlier_and_implausible_windows_ignored(void)
{
    static const size_t spikes[3] = { 40, 51, 70 };
    synth_opts o;
    synth_night n;
    rc_night_metrics base, m;
    rc_segment segs[160];
    size_t n_segs = 0, i;

    synth_opts_default(&o, 5);
    CHECK(synth_make(D0, &o, &n));
    CHECK(n.in.n_hrv > 80 && n.in.n_hrv <= 160);
    CHECK(rc_process_night(&n.in, NULL, &base, NULL, 0, NULL) == RC_OK);
    for (i = 0; i < 3; i++)
        n.hrv[spikes[i]].rmssd *= 4;     /* 单点尖刺 */
    n.hrv[60].rmssd = 900.0;             /* 超出合理范围 */
    CHECK(rc_process_night(&n.in, NULL, &m, segs, 160, &n_segs) == RC_OK);
    CHECK(n_segs == n.in.n_hrv);
    CHECK_NEAR(m.recovery_hrv, base.recovery_hrv, 0.03);
    CHECK(segs[40].w_quality == 0.0 && segs[40].weight == 0.0);
    CHECK(rc_is_na(segs[60].ln_rmssd) && segs[60].weight == 0.0);
    synth_free(&n);
}

static void missing_hr_keeps_hrv(void)
{
    synth_opts o;
    rc_night_metrics m;
    rc_result r;

    synth_opts_default(&o, 6);
    o.night_noise = 0;
    o.drop_hr = 1;
    CHECK(process_synth(D0, &o, &m) == RC_OK);
    CHECK(rc_is_na(m.rhr));
    CHECK(!rc_is_na(m.recovery_hrv));
    r = score_next(&o, RC_SYMPTOMS_NONE);
    CHECK(r.state == RC_STATE_SCORED);
    CHECK(rc_is_na(r.weights_used.rhr) || r.weights_used.rhr == 0);
}

static void wear_gap_invalid(void)
{
    synth_opts o;
    rc_night_metrics m;
    rc_result r;

    synth_opts_default(&o, 7);
    o.wear_gap_frac = 0.45;
    CHECK(process_synth(D0, &o, &m) == RC_OK);
    CHECK(!m.valid_night);
    CHECK(m.invalid_reason == RC_INVALID_LOW_WEAR_COVERAGE);
    CHECK(m.wear_coverage < CFG.min_wear_coverage);
    r = score_next(&o, RC_SYMPTOMS_NONE);
    CHECK(r.state == RC_STATE_INSUFFICIENT_DATA);
}

static void drop_hrv_no_core_signal(void)
{
    synth_opts o;
    rc_night_metrics m;

    synth_opts_default(&o, 8);
    o.drop_hrv = 1;
    CHECK(process_synth(D0, &o, &m) == RC_OK);
    CHECK(rc_is_na(m.recovery_hrv) && rc_is_na(m.early_hrv) && rc_is_na(m.late_hrv));
    CHECK(m.n_good_segments == 0);
    CHECK(!rc_is_na(m.rhr));
}

static void rr_adapter_roundtrip(void)
{
    enum { N_HYP = 20 + 600 + 200 + 100 + 10, N_RR = N_HYP * 30 };
    int *hyp = (int *)malloc(N_HYP * sizeof *hyp);
    double *rr = (double *)malloc(N_RR * sizeof *rr);
    rc_stage_interval st[8];
    rc_hrv_sample *hrv;
    rc_night_input in;
    rc_night_metrics m;
    size_t n_win = rc_rr_window_count(N_HYP * 30.0, NULL), n_hrv = 0, n_st = 0;
    unsigned rng = 9;
    int i;

    hrv = (rc_hrv_sample *)malloc(n_win * sizeof *hrv);
    CHECK(hyp && rr && hrv);
    if (!hyp || !rr || !hrv) {
        free(hyp);
        free(rr);
        free(hrv);
        return;
    }
    for (i = 0; i < N_HYP; i++)
        hyp[i] = i < 20 ? 0 : i < 620 ? 2 : i < 820 ? 3 : i < 920 ? 4 : 0;
    for (i = 0; i < N_RR; i++)
        rr[i] = synth_gauss(&rng, 1000.0, 30.0);

    CHECK(n_win == 93);
    CHECK(rc_rr_to_hrv5min(rr, N_RR, NULL, T0, N_HYP * 30.0, NULL, 0, NULL, hrv, n_win, &n_hrv) == RC_OK);
    CHECK(n_hrv == n_win);
    CHECK(rc_hypnogram_to_intervals(hyp, N_HYP, T0, 30, st, 8, &n_st) == RC_OK);
    CHECK(n_st == 5);

    rc_night_input_init(&in, D0);
    in.hrv = hrv;
    in.n_hrv = n_hrv;
    in.stages = st;
    in.n_stages = n_st;
    in.has_in_bed_start = 1;
    in.in_bed_start = T0;
    CHECK(rc_process_night(&in, NULL, &m, NULL, 0, NULL) == RC_OK);
    CHECK_NEAR(m.tst_min, 450, 0.01);
    CHECK_NEAR(m.rhr, 60, 2);
    CHECK_NEAR(m.recovery_hrv, log(30 * sqrt(2.0)), 0.1);

    /* 容量不够要报错，不能越界写 */
    CHECK(rc_rr_to_hrv5min(rr, N_RR, NULL, T0, N_HYP * 30.0, NULL, 0, NULL, hrv, n_win - 1, &n_hrv)
          == RC_ERR_CAPACITY);
    CHECK(rc_hypnogram_to_intervals(hyp, N_HYP, T0, 30, st, 4, &n_st) == RC_ERR_CAPACITY);
    free(hyp);
    free(rr);
    free(hrv);
}

/* ------------------------------------------------------------------ C 接口 */

static void config_defaults(void)
{
    rc_config c;

    rc_config_default(&c);
    CHECK(c.segment_sec == 300);
    CHECK_NEAR(c.weights.hrv + c.weights.rhr + c.weights.sleep + c.weights.resp, 1.0, 1e-12);
    CHECK_NEAR(c.sleep_weights.duration + c.sleep_weights.efficiency + c.sleep_weights.regularity +
               c.sleep_weights.continuity + c.sleep_weights.debt, 1.0, 1e-12);
    CHECK(c.cap_yellow == 66 && c.cap_red == 33);
    CHECK(c.green_min == 67 && c.yellow_min == 34 && c.tier_peak == 85 && c.tier_high_yellow == 50);
    CHECK(c.min_tst_min == 180 && c.min_wear_coverage == 0.70);
    CHECK(c.baseline_window == 30 && c.new_normal_run == 7);
    CHECK(c.trend_min == -8 && c.trend_max == 5);
    rc_config_default(NULL);   /* 不能崩 */
    CHECK_STR(rc_version(), "1.0.2");
}

static void parse_stage_aliases(void)
{
    static const struct { const char *text; int stage; } ok[] = {
        { "wake", RC_WAKE }, { "Awake", RC_WAKE }, { "W", RC_WAKE }, { "清醒", RC_WAKE },
        { "n1", RC_N1 },     { "N2", RC_N2 },      { "light", RC_N2 }, { "core", RC_N2 },
        { "浅睡", RC_N2 },   { "n3", RC_N3 },      { " Deep ", RC_N3 }, { "SWS", RC_N3 },
        { "深睡", RC_N3 },   { "rem", RC_REM },    { "REM\n", RC_REM }, { "快速眼动", RC_REM },
    };
    size_t i;
    int s;

    for (i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        s = -1;
        CHECK(rc_parse_stage(ok[i].text, &s) == RC_OK);
        CHECK(s == ok[i].stage);
    }
    s = 42;
    CHECK(rc_parse_stage("nap", &s) == RC_ERR_STAGE);
    CHECK(rc_parse_stage("", &s) == RC_ERR_STAGE);
    CHECK(rc_parse_stage(NULL, &s) != RC_OK);
    CHECK(s == 42);
}

static void parse_symptoms_names(void)
{
    int s = -1;

    CHECK(rc_parse_symptoms("none", &s) == RC_OK && s == RC_SYMPTOMS_NONE);
    CHECK(rc_parse_symptoms("Mild", &s) == RC_OK && s == RC_SYMPTOMS_MILD);
    CHECK(rc_parse_symptoms("moderate", &s) == RC_OK && s == RC_SYMPTOMS_MODERATE_SEVERE);
    CHECK(rc_parse_symptoms("severe", &s) == RC_OK && s == RC_SYMPTOMS_MODERATE_SEVERE);
    CHECK(rc_parse_symptoms("moderate_severe", &s) == RC_OK && s == RC_SYMPTOMS_MODERATE_SEVERE);
    CHECK(rc_parse_symptoms("fever", &s) == RC_ERR_ARG);
    CHECK_STR(rc_symptoms_name(RC_SYMPTOMS_MODERATE_SEVERE), "moderate_severe");
}

static void date_conversion(void)
{
    int y, m, d;

    CHECK(rc_date_from_ymd(1970, 1, 1) == 0);
    CHECK(rc_date_from_ymd(2000, 3, 1) == 11017);
    CHECK(rc_date_from_ymd(2026, 9, 1) == 20697);
    CHECK(rc_date_from_ymd(2024, 3, 1) - rc_date_from_ymd(2024, 2, 28) == 2);   /* 闰年 */
    CHECK(rc_date_from_ymd(2026, 3, 1) - rc_date_from_ymd(2026, 2, 28) == 1);
    rc_date_to_ymd(20697, &y, &m, &d);
    CHECK(y == 2026 && m == 9 && d == 1);
    rc_date_to_ymd(rc_date_from_ymd(2024, 2, 29), &y, &m, &d);
    CHECK(y == 2024 && m == 2 && d == 29);
    rc_date_to_ymd(-1, &y, &m, &d);
    CHECK(y == 1969 && m == 12 && d == 31);
}

static void names(void)
{
    rc_factor f;

    CHECK_STR(rc_state_name(RC_STATE_SCORED), "scored");
    CHECK_STR(rc_state_name(RC_STATE_NO_CORE_SIGNAL), "no_core_signal");
    CHECK_STR(rc_color_name(RC_COLOR_NONE), "");
    CHECK_STR(rc_color_name(RC_COLOR_YELLOW), "yellow");
    CHECK_STR(rc_tier_name(RC_TIER_HIGH_YELLOW), "high_yellow");
    CHECK_STR(rc_gate_code_name(RC_GATE_R_HRV_COMBO), "R_HRV_COMBO");
    CHECK_STR(rc_gate_code_name(RC_GATE_Y_ILLNESS), "Y_ILLNESS");
    CHECK_STR(rc_gate_code_name(99), "?");
    CHECK_STR(rc_invalid_reason_name(RC_INVALID_LOW_WEAR_COVERAGE), "low_wear_coverage");
    memset(&f, 0, sizeof f);
    f.key = RC_FACTOR_GATE;
    f.gate = RC_GATE_Y_RHR;
    CHECK_STR(rc_factor_name(&f), "Y_RHR");
    f.key = RC_FACTOR_TREND;
    CHECK_STR(rc_factor_name(&f), "trend");
    CHECK(rc_strerror(RC_ERR_STAGE)[0] != '\0' && rc_strerror(RC_OK)[0] != '\0');
}

static void error_codes(void)
{
    synth_opts o;
    synth_night n;
    rc_night_metrics m;
    rc_result r;
    rc_baseline bl;
    int saved;

    synth_opts_default(&o, 40);
    CHECK(synth_make(D0, &o, &n));
    CHECK(rc_process_night(NULL, NULL, &m, NULL, 0, NULL) == RC_ERR_ARG);
    CHECK(rc_process_night(&n.in, NULL, NULL, NULL, 0, NULL) == RC_ERR_ARG);
    saved = n.stages[3].stage;
    n.stages[3].stage = 7;
    CHECK(rc_process_night(&n.in, NULL, &m, NULL, 0, NULL) == RC_ERR_STAGE);
    n.stages[3].stage = saved;
    CHECK(rc_process_night(&n.in, NULL, &m, NULL, 0, NULL) == RC_OK);
    synth_free(&n);

    CHECK(rc_score_day(NULL, NULL, 0, RC_SYMPTOMS_NONE, NULL, &r) == RC_ERR_ARG);
    CHECK(rc_score_day(&m, NULL, 0, RC_SYMPTOMS_NONE, NULL, NULL) == RC_ERR_ARG);
    CHECK(rc_build_baseline(NULL, 0, D0, NULL, NULL) == RC_ERR_ARG);
    CHECK(rc_build_baseline(NULL, 0, D0, NULL, &bl) == RC_OK);
    CHECK(bl.n_nights == 0 && bl.stage == RC_BL_CALIBRATING);
    CHECK(rc_engine_add_metrics(NULL, &m, RC_SYMPTOMS_NONE, &r) == RC_ERR_ARG);
    rc_engine_destroy(NULL);
    CHECK(rc_engine_count(NULL) == 0);
}

/* 空输入不算错误，结果是无效夜 */
static void empty_input(void)
{
    rc_night_input in;
    rc_night_metrics m;
    rc_result r;
    size_t n_segs = 99;

    rc_night_input_init(&in, D0);
    CHECK(rc_process_night(&in, NULL, &m, NULL, 0, &n_segs) == RC_OK);
    CHECK(n_segs == 0);
    CHECK(!m.valid_night && m.invalid_reason == RC_INVALID_SLEEP_TOO_SHORT);
    CHECK(m.tst_min == 0 && rc_is_na(m.recovery_hrv) && rc_is_na(m.midpoint_min));
    CHECK(rc_score_day(&m, NULL, 0, RC_SYMPTOMS_NONE, NULL, &r) == RC_OK);
    CHECK(r.state == RC_STATE_SLEEP_TOO_SHORT && r.score == -1);
}

/* segs 容量不够只截断片段输出，指标不变 */
static void segment_buffer_truncation(void)
{
    synth_opts o;
    synth_night n;
    rc_night_metrics a, b;
    rc_segment segs[6];
    size_t n_segs = 0;

    synth_opts_default(&o, 41);
    CHECK(synth_make(D0, &o, &n));
    CHECK(rc_process_night(&n.in, NULL, &a, NULL, 0, NULL) == RC_OK);
    memset(segs, 0, sizeof segs);
    segs[5].idx = -7;   /* 哨兵：容量 5 时不能被写到 */
    CHECK(rc_process_night(&n.in, NULL, &b, segs, 5, &n_segs) == RC_OK);
    CHECK(n_segs == n.in.n_hrv);
    CHECK(segs[5].idx == -7);
    CHECK(segs[0].idx == 0 && segs[4].idx == 4);
    CHECK(a.recovery_hrv == b.recovery_hrv && a.rhr == b.rhr && a.wear_coverage == b.wear_coverage);
    synth_free(&n);
}

/* 记录和分期倒序、带一条重复时刻，结果与有序输入相同（重复时刻保留后出现的那条） */
static void unordered_input(void)
{
    synth_opts o;
    synth_night n;
    rc_night_metrics a, b;
    rc_hrv_sample *h2;
    rc_stage_interval *s2;
    size_t nh, ns, i;

    synth_opts_default(&o, 42);
    CHECK(synth_make(D0, &o, &n));
    nh = n.in.n_hrv;
    ns = n.in.n_stages;
    CHECK(rc_process_night(&n.in, NULL, &a, NULL, 0, NULL) == RC_OK);

    h2 = (rc_hrv_sample *)malloc((nh + 1) * sizeof *h2);
    s2 = (rc_stage_interval *)malloc(ns * sizeof *s2);
    CHECK(h2 && s2);
    if (h2 && s2) {
        /* 第 0 条是第 10 个窗口的一条坏记录，后面跟着倒序的正常记录，正常的那条应当胜出 */
        h2[0] = n.hrv[10];
        h2[0].rmssd = 7.0;
        for (i = 0; i < nh; i++)
            h2[1 + i] = n.hrv[nh - 1 - i];
        for (i = 0; i < ns; i++)
            s2[i] = n.stages[ns - 1 - i];
        n.in.hrv = h2;
        n.in.n_hrv = nh + 1;
        n.in.stages = s2;
        CHECK(rc_process_night(&n.in, NULL, &b, NULL, 0, NULL) == RC_OK);
        CHECK(a.recovery_hrv == b.recovery_hrv && a.early_hrv == b.early_hrv);
        CHECK(a.rhr == b.rhr && a.resp == b.resp && a.tst_min == b.tst_min);
        CHECK(a.n_good_segments == b.n_good_segments && a.wear_coverage == b.wear_coverage);
    }
    free(h2);
    free(s2);
    synth_free(&n);
}

static void external_sleep_score_replaces_subscore(void)
{
    synth_opts o;
    synth_night n;
    rc_engine *e = base_engine();
    rc_result r;
    rc_night_metrics m;

    synth_opts_default(&o, 43);
    o.night_noise = 0;
    CHECK(synth_make(DNEXT, &o, &n));
    n.in.sleep_score_external = 48.0;
    CHECK(rc_engine_add_night(e, &n.in, RC_SYMPTOMS_NONE, &r, &m) == RC_OK);
    CHECK(m.sleep_score_external == 48.0);
    CHECK(r.subscores.sleep == 48.0);
    CHECK(has_gate(r.gates, r.n_gates, RC_GATE_Y_SLEEP));
    CHECK(r.color != RC_COLOR_GREEN);
    synth_free(&n);
    rc_engine_destroy(e);
}

/* rc_score_day 是纯函数：和引擎给出同样的结果，且不依赖历史的顺序 */
static void score_day_matches_engine(void)
{
    rc_day_record rev[BASE_NIGHTS];
    synth_opts o;
    rc_night_metrics m;
    rc_result a, b, c;
    int i;

    synth_opts_default(&o, 44);
    o.ln_shift = -0.2;
    CHECK(process_synth(DNEXT, &o, &m) == RC_OK);
    a = score_next_metrics(&m);
    CHECK(rc_score_day(&m, g_base, BASE_NIGHTS, RC_SYMPTOMS_NONE, &CFG, &b) == RC_OK);
    for (i = 0; i < BASE_NIGHTS; i++)
        rev[i] = g_base[BASE_NIGHTS - 1 - i];
    CHECK(rc_score_day(&m, rev, BASE_NIGHTS, RC_SYMPTOMS_NONE, NULL, &c) == RC_OK);
    CHECK(a.score == b.score && b.score == c.score);
    CHECK(a.score_internal == b.score_internal && b.score_internal == c.score_internal);
    CHECK(a.n_gates == b.n_gates && b.n_gates == c.n_gates);
    CHECK_STR(a.summary, b.summary);
    CHECK_STR(b.summary, c.summary);
}

static void engine_history_load_prune(void)
{
    rc_engine *e = rc_engine_create(NULL), *e2;
    const rc_day_record *h;
    rc_night_metrics m;
    rc_result r;
    size_t i;

    CHECK(e != NULL);
    CHECK(rc_engine_count(e) == 0 && rc_engine_history(e) == NULL);
    CHECK(rc_engine_config(e)->green_min == 67);

    /* 乱序加入，历史保持日期升序 */
    rc_night_metrics_init(&m, D0 + 5);
    m.tst_min = 420;
    m.efficiency = 0.9;
    CHECK(rc_engine_add_metrics(e, &m, RC_SYMPTOMS_NONE, &r) == RC_OK);
    m.night_date = D0 + 2;
    CHECK(rc_engine_add_metrics(e, &m, RC_SYMPTOMS_MILD, &r) == RC_OK);
    m.night_date = D0 + 9;
    CHECK(rc_engine_add_metrics(e, &m, RC_SYMPTOMS_NONE, &r) == RC_OK);
    CHECK(rc_engine_count(e) == 3);
    h = rc_engine_history(e);
    CHECK(h[0].night.night_date == D0 + 2 && h[1].night.night_date == D0 + 5 &&
          h[2].night.night_date == D0 + 9);
    CHECK(h[0].symptoms == RC_SYMPTOMS_MILD && h[0].has_result);

    /* 装载：拷贝、排序、同日期保留靠后的一条 */
    e2 = rc_engine_create(rc_engine_config(e));
    CHECK(e2 != NULL);
    {
        rc_day_record recs[4];
        recs[0] = h[2];
        recs[1] = h[0];
        recs[2] = h[1];
        recs[3] = h[0];
        recs[3].symptoms = RC_SYMPTOMS_MODERATE_SEVERE;
        CHECK(rc_engine_load(e2, recs, 4) == RC_OK);
    }
    CHECK(rc_engine_count(e2) == 3);
    h = rc_engine_history(e2);
    for (i = 1; i < rc_engine_count(e2); i++)
        CHECK(h[i].night.night_date > h[i - 1].night.night_date);
    CHECK(h[0].symptoms == RC_SYMPTOMS_MODERATE_SEVERE);

    CHECK(rc_engine_prune_before(e2, D0 + 5) == 1);
    CHECK(rc_engine_count(e2) == 2);
    CHECK(rc_engine_history(e2)[0].night.night_date == D0 + 5);
    CHECK(rc_engine_prune_before(e2, D0 + 100) == 2);
    CHECK(rc_engine_count(e2) == 0);
    CHECK(rc_engine_load(e2, NULL, 0) == RC_OK);
    CHECK(rc_engine_count(e) == 3);   /* 两台引擎互不影响 */

    rc_engine_destroy(e);
    rc_engine_destroy(e2);
}

/* 自定义配置生效：把绿色下限提到 95，正常夜就不再是绿色 */
static void custom_config(void)
{
    rc_config c = CFG;
    rc_engine *e;
    synth_opts o;
    rc_result r;

    c.green_min = 95;
    c.tier_peak = 98;
    e = rc_engine_create(&c);
    CHECK(e != NULL);
    c.green_min = 1;   /* 引擎拷贝了配置，这里再改不影响它 */
    CHECK(rc_engine_load(e, g_base, BASE_NIGHTS) == RC_OK);
    synth_opts_default(&o, 11);
    o.night_noise = 0;
    CHECK(add_synth(e, DNEXT, &o, RC_SYMPTOMS_NONE, &r, NULL) == RC_OK);
    CHECK(r.state == RC_STATE_SCORED && r.score < 95);
    CHECK(r.color == RC_COLOR_YELLOW);
    rc_engine_destroy(e);
}

static void hrv_rebound_helper(void)
{
    rc_night_metrics m;

    rc_night_metrics_init(&m, D0);
    CHECK(rc_is_na(rc_hrv_rebound(&m)));
    CHECK(m.valid_night == 1 && m.wear_coverage == 1.0 && m.invalid_reason == RC_INVALID_NONE);
    m.early_hrv = 3.7;
    m.late_hrv = 3.9;
    CHECK_NEAR(rc_hrv_rebound(&m), 0.2, 1e-12);
}

int main(void)
{
    rc_config_default(&CFG);
    D0 = rc_date_from_ymd(2026, 9, 1);
    DNEXT = D0 + BASE_NIGHTS;
    if (!build_base()) {
        fprintf(stderr, "建立基础历史失败\n");
        return 2;
    }

    /* 对应 Python tests/test_recovery.py */
    RUN(round_half_up);
    RUN(weighted_median);
    RUN(hrv_cap_95);
    RUN(rhr_penalty_continuous_monotone);
    RUN(resp_penalty_continuous_monotone);
    RUN(resp_severity_uses_absolute_delta);
    RUN(late_adjustment_exclusive_and_bounded);
    RUN(tier_boundaries);
    RUN(caps_match_bands);
    RUN(clean_night_has_no_gate);
    RUN(false_recovery_never_green);
    RUN(red_rules);
    RUN(missing_resp_drops_resp_gates);
    RUN(metrics_reasonable);
    RUN(short_sleep_invalid);
    RUN(cold_start);
    RUN(normal_night_green_integer);
    RUN(fatigue_red);
    RUN(false_recovery_capped);
    RUN(sleep_too_short_no_score);
    RUN(missing_hrv_redistributes);
    RUN(missing_resp);
    RUN(no_core_signal);
    RUN(baseline_uses_only_past);
    RUN(new_normal_readmission);
    RUN(reprocessing_same_date_replaces);
    RUN(trend_bounds_and_symptoms);
    RUN(sleep_metrics_from_intervals);
    RUN(string_and_code_stages_equivalent);
    RUN(ln_input_equivalent);
    RUN(outlier_and_implausible_windows_ignored);
    RUN(missing_hr_keeps_hrv);
    RUN(wear_gap_invalid);
    RUN(drop_hrv_no_core_signal);
    RUN(rr_adapter_roundtrip);

    /* C 版自己的 */
    RUN(round_dp_matches_python);
    RUN(median_interp_sum);
    RUN(gates_in_enum_order);
    RUN(trend_negative_on_decline);
    RUN(utc_offset_shifts_midpoint_only);
    RUN(config_defaults);
    RUN(parse_stage_aliases);
    RUN(parse_symptoms_names);
    RUN(date_conversion);
    RUN(names);
    RUN(error_codes);
    RUN(empty_input);
    RUN(segment_buffer_truncation);
    RUN(unordered_input);
    RUN(external_sleep_score_replaces_subscore);
    RUN(score_day_matches_engine);
    RUN(engine_history_load_prune);
    RUN(custom_config);
    RUN(hrv_rebound_helper);

    printf("单元测试：%d/%d 个用例通过，%d 条断言，失败 %d 条\n", g_tests - g_failed_tests, g_tests,
           g_checks, g_failed_checks);
    return g_failed_tests ? 1 : 0;
}
