/*
 * test_parity.c —— 与 Python 参考实现逐晚比对。
 *
 * 用法：test_parity <parity_cases.json>
 *
 * 用例文件由 tools/export_parity_cases.py 生成：若干序列，每个序列是一台引擎上依次加入的夜晚，
 * 每晚带输入和 Python 的结果。这里用同样的输入驱动 C 引擎，逐字段比较。
 *
 * 比较规则
 *   枚举、整数、布尔、字符串（含中文文案）：必须完全相同。
 *   浮点：|C − Python| ≤ TOL。Python 为 None / 字典里没有这个键时，C 必须是 RC_NA。
 * 全部一致返回 0，否则返回 1。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "recovery.h"

#define TOL 1e-9
#define MAX_PRINT 40     /* 最多打印多少条不一致，其余只计数 */
#define MAX_SEQ 256

static long g_checks;         /* 比较过的字段数 */
static long g_mismatch;       /* 不一致的字段数 */
static double g_max_diff;     /* 浮点字段的最大绝对差 */
static const char *g_seq;     /* 当前序列名和夜序号，报错时用 */
static size_t g_night;

static void mismatch(const char *field, const char *expect, const char *got)
{
    g_mismatch++;
    if (g_mismatch <= MAX_PRINT)
        printf("  不一致 [%s #%lu] %s：Python=%s C=%s\n", g_seq, (unsigned long)g_night, field,
               expect, got);
}

static void cmp_num(const char *field, const json_value *expect, double got)
{
    char a[40], b[40];

    g_checks++;
    if (json_is_null(expect)) {
        if (rc_is_na(got))
            return;
        snprintf(b, sizeof b, "%.17g", got);
        mismatch(field, "null", b);
        return;
    }
    if (!rc_is_na(got)) {
        double d = fabs(got - expect->number);
        if (d > g_max_diff)
            g_max_diff = d;
        if (d <= TOL)
            return;
    }
    snprintf(a, sizeof a, "%.17g", expect->number);
    snprintf(b, sizeof b, "%.17g", got);
    mismatch(field, a, b);
}

static void cmp_int(const char *field, long expect, long got)
{
    char a[24], b[24];

    g_checks++;
    if (expect == got)
        return;
    snprintf(a, sizeof a, "%ld", expect);
    snprintf(b, sizeof b, "%ld", got);
    mismatch(field, a, b);
}

/* Python 的 None 对应 C 的空串 */
static void cmp_str(const char *field, const json_value *expect, const char *got)
{
    const char *e = json_string(expect);

    g_checks++;
    if (!e)
        e = "";
    if (strcmp(e, got) != 0)
        mismatch(field, e, got);
}

static rc_date parse_date(const json_value *v)
{
    int y = 1970, m = 1, d = 1;
    const char *s = json_string(v);

    if (s)
        sscanf(s, "%d-%d-%d", &y, &m, &d);
    return rc_date_from_ymd(y, m, d);
}

static double num_or_na(const json_value *v)
{
    return json_number(v, RC_NA);
}

/* ---- 输入的三种形式 ---- */

/* "input"：标准的 5 分钟 HRV + 分期区间。成功返回 RC_OK，结果写入 res / m。 */
static int run_raw(rc_engine *e, const json_value *in, int symptoms, rc_result *res,
                   rc_night_metrics *m)
{
    const json_value *jh = json_get(in, "hrv"), *js = json_get(in, "stages");
    size_t nh = json_count(jh), ns = json_count(js), i;
    rc_hrv_sample *hrv = (rc_hrv_sample *)malloc((nh + 1) * sizeof *hrv);
    rc_stage_interval *st = (rc_stage_interval *)malloc((ns + 1) * sizeof *st);
    rc_night_input ni;
    int rc = RC_OK;

    if (!hrv || !st) {
        free(hrv);
        free(st);
        return RC_ERR_NOMEM;
    }
    rc_night_input_init(&ni, parse_date(json_get(in, "night_date")));
    for (i = 0; i < nh; i++) {
        const json_value *x = json_at(jh, i);
        rc_hrv_sample_init(&hrv[i], (int64_t)json_number(json_at(x, 0), 0), num_or_na(json_at(x, 1)));
        hrv[i].hr_bpm = num_or_na(json_at(x, 2));
        hrv[i].resp_brpm = num_or_na(json_at(x, 3));
        hrv[i].quality = num_or_na(json_at(x, 4));
        hrv[i].motion = num_or_na(json_at(x, 5));
        hrv[i].duration_sec = (int32_t)json_number(json_at(x, 6), 0);
    }
    for (i = 0; i < ns && rc == RC_OK; i++) {
        const json_value *x = json_at(js, i), *sv = json_at(x, 2);
        st[i].start = (int64_t)json_number(json_at(x, 0), 0);
        st[i].end = (int64_t)json_number(json_at(x, 1), 0);
        if (sv && sv->type == JSON_STRING)
            rc = rc_parse_stage(sv->string, &st[i].stage);
        else
            st[i].stage = (int)json_number(sv, -1);
    }
    ni.hrv = hrv;
    ni.n_hrv = nh;
    ni.stages = st;
    ni.n_stages = ns;
    ni.hrv_is_ln = json_bool(json_get(in, "hrv_is_ln"), 0);
    ni.sleep_score_external = num_or_na(json_get(in, "sleep_score_external"));
    if (!json_is_null(json_get(in, "in_bed_start"))) {
        ni.has_in_bed_start = 1;
        ni.in_bed_start = (int64_t)json_number(json_get(in, "in_bed_start"), 0);
    }
    if (rc == RC_OK)
        rc = rc_engine_add_night(e, &ni, symptoms, res, m);
    free(hrv);
    free(st);
    return rc;
}

/* JSON 里的夜间指标 → rc_night_metrics */
static void metrics_from_json(const json_value *j, rc_night_metrics *m)
{
    const char *reason = json_string(json_get(j, "invalid_reason"));

    rc_night_metrics_init(m, parse_date(json_get(j, "night_date")));
    m->recovery_hrv = num_or_na(json_get(j, "recovery_hrv"));
    m->early_hrv = num_or_na(json_get(j, "early_hrv"));
    m->late_hrv = num_or_na(json_get(j, "late_hrv"));
    m->rhr = num_or_na(json_get(j, "rhr"));
    m->resp = num_or_na(json_get(j, "resp"));
    m->tst_min = json_number(json_get(j, "tst_min"), 0);
    m->efficiency = json_number(json_get(j, "efficiency"), 0);
    m->waso_min = json_number(json_get(j, "waso_min"), 0);
    m->long_awakenings = (int)json_number(json_get(j, "long_awakenings"), 0);
    m->midpoint_min = num_or_na(json_get(j, "midpoint_min"));
    m->sleep_score_external = num_or_na(json_get(j, "sleep_score_external"));
    m->n_good_segments = (int)json_number(json_get(j, "n_good_segments"), 0);
    m->n_nrem_segments = (int)json_number(json_get(j, "n_nrem_segments"), 0);
    m->wear_coverage = json_number(json_get(j, "wear_coverage"), 1);
    m->valid_night = json_bool(json_get(j, "valid_night"), 1);
    m->invalid_reason = RC_INVALID_NONE;
    if (reason && strcmp(reason, "sleep_too_short") == 0)
        m->invalid_reason = RC_INVALID_SLEEP_TOO_SHORT;
    else if (reason && strcmp(reason, "low_wear_coverage") == 0)
        m->invalid_reason = RC_INVALID_LOW_WEAR_COVERAGE;
}

/* "rr"：逐搏 RR + 30 秒分期，先转成标准输入 */
static int run_rr(rc_engine *e, const json_value *in, rc_result *res, rc_night_metrics *m)
{
    const json_value *jr = json_get(in, "rr_ms"), *jh = json_get(in, "hyp");
    const json_value *jp = json_get(in, "resp");
    size_t n_rr = json_count(jr), n_hyp = json_count(jh), n_resp = json_count(jp), i;
    int epoch = (int)json_number(json_get(in, "epoch_sec"), 30);
    int64_t start = (int64_t)json_number(json_get(in, "start"), 0);
    double dur = (double)n_hyp * epoch;
    size_t n_win = rc_rr_window_count(dur, NULL), n_out = 0, n_iv = 0;
    double *rr = (double *)malloc((n_rr + 1) * sizeof *rr);
    double *resp = (double *)malloc((n_resp + 1) * sizeof *resp);
    int *hyp = (int *)malloc((n_hyp + 1) * sizeof *hyp);
    rc_hrv_sample *hrv = (rc_hrv_sample *)malloc((n_win + 1) * sizeof *hrv);
    rc_stage_interval *st = (rc_stage_interval *)malloc((n_hyp + 1) * sizeof *st);
    rc_night_input ni;
    int rc = RC_ERR_NOMEM;

    if (rr && resp && hyp && hrv && st) {
        for (i = 0; i < n_rr; i++)
            rr[i] = json_number(json_at(jr, i), RC_NA);
        for (i = 0; i < n_resp; i++)
            resp[i] = num_or_na(json_at(jp, i));
        for (i = 0; i < n_hyp; i++)
            hyp[i] = (int)json_number(json_at(jh, i), -1);

        rc = rc_rr_to_hrv5min(rr, n_rr, NULL, start, dur, json_is_null(jp) ? NULL : resp, n_resp,
                              NULL, hrv, n_win, &n_out);
        if (rc == RC_OK)
            rc = rc_hypnogram_to_intervals(hyp, n_hyp, start, epoch, st, n_hyp, &n_iv);
        if (rc == RC_OK) {
            rc_night_input_init(&ni, parse_date(json_get(in, "night_date")));
            ni.hrv = hrv;
            ni.n_hrv = n_out;
            ni.stages = st;
            ni.n_stages = n_iv;
            ni.has_in_bed_start = 1;
            ni.in_bed_start = start;
            rc = rc_engine_add_night(e, &ni, RC_SYMPTOMS_NONE, res, m);
        }
    }
    free(rr);
    free(resp);
    free(hyp);
    free(hrv);
    free(st);
    return rc;
}

/* ---- 结果比较 ---- */

static void cmp_metrics(const json_value *j, const rc_night_metrics *m)
{
    cmp_int("metrics.night_date", parse_date(json_get(j, "night_date")), m->night_date);
    cmp_num("metrics.recovery_hrv", json_get(j, "recovery_hrv"), m->recovery_hrv);
    cmp_num("metrics.early_hrv", json_get(j, "early_hrv"), m->early_hrv);
    cmp_num("metrics.late_hrv", json_get(j, "late_hrv"), m->late_hrv);
    cmp_num("metrics.rhr", json_get(j, "rhr"), m->rhr);
    cmp_num("metrics.resp", json_get(j, "resp"), m->resp);
    cmp_num("metrics.tst_min", json_get(j, "tst_min"), m->tst_min);
    cmp_num("metrics.efficiency", json_get(j, "efficiency"), m->efficiency);
    cmp_num("metrics.waso_min", json_get(j, "waso_min"), m->waso_min);
    cmp_int("metrics.long_awakenings", (long)json_number(json_get(j, "long_awakenings"), -1),
            m->long_awakenings);
    cmp_num("metrics.midpoint_min", json_get(j, "midpoint_min"), m->midpoint_min);
    cmp_num("metrics.sleep_score_external", json_get(j, "sleep_score_external"),
            m->sleep_score_external);
    cmp_int("metrics.n_good_segments", (long)json_number(json_get(j, "n_good_segments"), -1),
            m->n_good_segments);
    cmp_int("metrics.n_nrem_segments", (long)json_number(json_get(j, "n_nrem_segments"), -1),
            m->n_nrem_segments);
    cmp_num("metrics.wear_coverage", json_get(j, "wear_coverage"), m->wear_coverage);
    cmp_int("metrics.valid_night", json_bool(json_get(j, "valid_night"), -1), m->valid_night);
    cmp_str("metrics.invalid_reason", json_get(j, "invalid_reason"),
            rc_invalid_reason_name(m->invalid_reason));
}

static void cmp_result(const json_value *j, const rc_result *r)
{
    const json_value *sub = json_get(j, "subscores"), *w = json_get(j, "weights_used");
    const json_value *z = json_get(j, "z"), *sd = json_get(j, "sleep_detail");
    const json_value *gates = json_get(j, "gates"), *tf = json_get(j, "top_factors");
    const json_value *cr = json_get(j, "confidence_reasons");
    char name[64];
    size_t i;

    cmp_int("date", parse_date(json_get(j, "date")), r->date);
    cmp_str("state", json_get(j, "state"), rc_state_name(r->state));
    cmp_int("score", (long)json_number(json_get(j, "score"), -1), r->score);
    cmp_num("score_internal", json_get(j, "score_internal"), r->score_internal);
    cmp_str("color", json_get(j, "color"), rc_color_name(r->color));
    cmp_str("tier", json_get(j, "tier"), rc_tier_name(r->tier));
    cmp_str("advice", json_get(j, "advice"), r->advice);
    cmp_str("confidence", json_get(j, "confidence"), rc_confidence_name(r->confidence));
    cmp_str("baseline_stage", json_get(j, "baseline_stage"), rc_baseline_stage_name(r->baseline_stage));
    cmp_int("baseline_nights", (long)json_number(json_get(j, "baseline_nights"), -1),
            r->baseline_nights);
    cmp_num("raw", json_get(j, "raw"), r->raw);
    cmp_num("trend_adjustment", json_get(j, "trend_adjustment"), r->trend_adjustment);
    cmp_num("late_adjustment", json_get(j, "late_adjustment"), r->late_adjustment);
    cmp_num("cap", json_get(j, "cap"), r->cap);

    cmp_num("subscores.hrv", json_get(sub, "hrv"), r->subscores.hrv);
    cmp_num("subscores.rhr", json_get(sub, "rhr"), r->subscores.rhr);
    cmp_num("subscores.sleep", json_get(sub, "sleep"), r->subscores.sleep);
    cmp_num("subscores.resp", json_get(sub, "resp"), r->subscores.resp);
    cmp_num("weights_used.hrv", json_get(w, "hrv"), r->weights_used.hrv);
    cmp_num("weights_used.rhr", json_get(w, "rhr"), r->weights_used.rhr);
    cmp_num("weights_used.sleep", json_get(w, "sleep"), r->weights_used.sleep);
    cmp_num("weights_used.resp", json_get(w, "resp"), r->weights_used.resp);
    cmp_num("z.hrv", json_get(z, "hrv"), r->z.hrv);
    cmp_num("z.late", json_get(z, "late"), r->z.late);
    cmp_num("z.early", json_get(z, "early"), r->z.early);
    cmp_num("z.rhr", json_get(z, "rhr"), r->z.rhr);
    cmp_num("z.resp", json_get(z, "resp"), r->z.resp);
    cmp_num("z.resp_r", json_get(z, "resp_r"), r->z.resp_r);
    cmp_num("sleep_detail.duration", json_get(sd, "duration"), r->sleep_detail.duration);
    cmp_num("sleep_detail.efficiency", json_get(sd, "efficiency"), r->sleep_detail.efficiency);
    cmp_num("sleep_detail.regularity", json_get(sd, "regularity"), r->sleep_detail.regularity);
    cmp_num("sleep_detail.continuity", json_get(sd, "continuity"), r->sleep_detail.continuity);
    cmp_num("sleep_detail.debt", json_get(sd, "debt"), r->sleep_detail.debt);
    cmp_num("sleep_detail.need_min", json_get(sd, "need_min"), r->sleep_detail.need_min);
    cmp_num("sleep_detail.debt_prev_min", json_get(sd, "debt_prev_min"), r->sleep_detail.debt_prev_min);
    cmp_num("sleep_detail.debt_after_min", json_get(sd, "debt_after_min"),
            r->sleep_detail.debt_after_min);

    cmp_int("len(gates)", (long)json_count(gates), r->n_gates);
    for (i = 0; i < json_count(gates) && (int)i < r->n_gates; i++) {
        const json_value *g = json_at(gates, i);
        snprintf(name, sizeof name, "gates[%lu].code", (unsigned long)i);
        cmp_str(name, json_get(g, "code"), rc_gate_code_name(r->gates[i].code));
        snprintf(name, sizeof name, "gates[%lu].level", (unsigned long)i);
        cmp_str(name, json_get(g, "level"), r->gates[i].level == RC_GATE_RED ? "red" : "yellow");
        snprintf(name, sizeof name, "gates[%lu].cap", (unsigned long)i);
        cmp_num(name, json_get(g, "cap"), r->gates[i].cap);
        snprintf(name, sizeof name, "gates[%lu].text", (unsigned long)i);
        cmp_str(name, json_get(g, "text"), r->gates[i].text);
    }

    cmp_int("illness_points", (long)json_number(json_get(j, "illness_points"), -1), r->illness_points);
    cmp_int("illness_physio_points", (long)json_number(json_get(j, "illness_physio_points"), -1),
            r->illness_physio_points);
    cmp_str("illness_level", json_get(j, "illness_level"), rc_illness_level_name(r->illness_level));
    cmp_str("illness_message", json_get(j, "illness_message"), r->illness_message);

    cmp_int("len(top_factors)", (long)json_count(tf), r->n_top_factors);
    for (i = 0; i < json_count(tf) && (int)i < r->n_top_factors; i++) {
        const json_value *f = json_at(tf, i);
        const char *dir = json_string(json_get(f, "direction"));
        snprintf(name, sizeof name, "top_factors[%lu].key", (unsigned long)i);
        cmp_str(name, json_get(f, "key"), rc_factor_name(&r->top_factors[i]));
        snprintf(name, sizeof name, "top_factors[%lu].direction", (unsigned long)i);
        cmp_int(name, dir && strcmp(dir, "positive") == 0, r->top_factors[i].positive);
        snprintf(name, sizeof name, "top_factors[%lu].impact", (unsigned long)i);
        cmp_num(name, json_get(f, "impact"), r->top_factors[i].impact);
        snprintf(name, sizeof name, "top_factors[%lu].text", (unsigned long)i);
        cmp_str(name, json_get(f, "text"), r->top_factors[i].text);
    }

    cmp_int("len(confidence_reasons)", (long)json_count(cr), r->n_confidence_reasons);
    for (i = 0; i < json_count(cr) && (int)i < r->n_confidence_reasons; i++) {
        snprintf(name, sizeof name, "confidence_reasons[%lu]", (unsigned long)i);
        cmp_str(name, json_at(cr, i), r->confidence_reasons[i]);
    }

    cmp_str("summary", json_get(j, "summary"), r->summary);
    cmp_int("baseline_outlier", json_bool(json_get(j, "baseline_outlier"), -1), r->baseline_outlier);
}

int main(int argc, char **argv)
{
    char err[128];
    json_value *root;
    const json_value *seqs;
    const char *names[MAX_SEQ];
    rc_engine *engines[MAX_SEQ];
    size_t n_seq, si, n_nights = 0, n_nights_ok = 0, n_seq_ok = 0;
    long score_checked = 0;
    int fatal = 0;

    memset(names, 0, sizeof names);
    memset(engines, 0, sizeof engines);
    if (argc != 2) {
        fprintf(stderr, "用法：%s <parity_cases.json>\n", argv[0]);
        return 2;
    }
    root = json_parse_file(argv[1], err, sizeof err);
    if (!root) {
        fprintf(stderr, "%s\n", err);
        return 2;
    }
    seqs = json_get(root, "sequences");
    n_seq = json_count(seqs);
    if (n_seq == 0 || n_seq > MAX_SEQ) {
        fprintf(stderr, "用例文件里的序列数不对：%lu\n", (unsigned long)n_seq);
        json_free(root);
        return 2;
    }

    for (si = 0; si < n_seq && !fatal; si++) {
        const json_value *seq = json_at(seqs, si), *nights = json_get(seq, "nights");
        const char *base = json_string(json_get(seq, "base"));
        rc_engine *e = rc_engine_create(NULL);
        long before = g_mismatch;
        size_t k;

        names[si] = json_string(json_get(seq, "name"));
        engines[si] = e;
        g_seq = names[si] ? names[si] : "?";
        if (!e) {
            fatal = 1;
            break;
        }
        /* 从 base 序列结束时的历史接着算，对应 Python 里的 copy.deepcopy(engine) */
        if (base) {
            size_t b;
            for (b = 0; b < si; b++)
                if (names[b] && strcmp(names[b], base) == 0)
                    break;
            if (b == si || rc_engine_load(e, rc_engine_history(engines[b]),
                                          rc_engine_count(engines[b])) != RC_OK) {
                fprintf(stderr, "序列 %s 的 base 无法装载\n", g_seq);
                fatal = 1;
                break;
            }
        }

        for (k = 0; k < json_count(nights); k++) {
            const json_value *n = json_at(nights, k);
            const json_value *expect = json_get(n, "result");
            long before_night = g_mismatch;
            rc_night_metrics m;
            rc_result res;
            int symptoms = RC_SYMPTOMS_NONE, rc;

            g_night = k;
            n_nights++;
            rc_parse_symptoms(json_string(json_get(n, "symptoms")), &symptoms);

            if (json_get(n, "input")) {
                rc = run_raw(e, json_get(n, "input"), symptoms, &res, &m);
            } else if (json_get(n, "rr")) {
                rc = run_rr(e, json_get(n, "rr"), &res, &m);
            } else {
                metrics_from_json(json_get(n, "metrics_input"), &m);
                rc = rc_engine_add_metrics(e, &m, symptoms, &res);
            }
            if (rc != RC_OK) {
                mismatch("返回值", "RC_OK", rc_strerror(rc));
                continue;
            }
            cmp_metrics(json_get(n, "metrics"), &m);
            cmp_result(expect, &res);
            if (!json_is_null(json_get(expect, "score")))
                score_checked++;
            if (g_mismatch == before_night)
                n_nights_ok++;
        }
        if (g_mismatch == before)
            n_seq_ok++;
        else
            printf("序列 %s：%ld 处不一致\n", g_seq, g_mismatch - before);
    }

    for (si = 0; si < n_seq; si++)
        rc_engine_destroy(engines[si]);

    if (g_mismatch > MAX_PRINT)
        printf("  ……另有 %ld 处不一致未打印\n", g_mismatch - MAX_PRINT);
    printf("一致性比对：%lu/%lu 个序列、%lu/%lu 晚完全一致（其中出分 %ld 晚）；"
           "比较 %ld 个字段，不一致 %ld 个；浮点最大绝对差 %.3g（容差 %g）\n",
           (unsigned long)n_seq_ok, (unsigned long)n_seq, (unsigned long)n_nights_ok,
           (unsigned long)n_nights, score_checked, g_checks, g_mismatch, g_max_diff, TOL);

    json_free(root);
    return (fatal || g_mismatch) ? 1 : 0;
}
