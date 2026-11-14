/* 评分主流程 rc_score_day（纯函数）和保存历史的 rc_engine。 */
#include "rc_internal.h"

#include <string.h>

/* 结果置为"没有分数"的初始状态 */
static void result_init(rc_result *res, rc_date date)
{
    memset(res, 0, sizeof *res);
    res->date = date;
    res->state = RC_STATE_SCORED;
    res->score = -1;
    res->score_internal = RC_NA;
    res->raw = RC_NA;
    res->cap = 100.0;
    res->subscores.hrv = res->subscores.rhr = res->subscores.sleep = res->subscores.resp = RC_NA;
    res->weights_used = res->subscores;
    res->z.hrv = res->z.late = res->z.early = res->z.rhr = res->z.resp = res->z.resp_r = RC_NA;
    res->sleep_detail.duration = res->sleep_detail.efficiency = RC_NA;
    res->sleep_detail.regularity = res->sleep_detail.continuity = RC_NA;
    res->sleep_detail.debt = res->sleep_detail.need_min = RC_NA;
    res->sleep_detail.debt_prev_min = res->sleep_detail.debt_after_min = RC_NA;
}

static void set_unscored(rc_result *res, int state)
{
    res->state = state;
    rc_strcpy(res->summary, sizeof res->summary, rc_state_text(state));
}

static double round_or_na(double v, int ndigits)
{
    return rc_is_na(v) ? v : rc_round_dp(v, ndigits);
}

int rc_score_day(const rc_night_metrics *night,
                 const rc_day_record *history, size_t n,
                 int symptoms, const rc_config *cfg_in, rc_result *res)
{
    rc_config cfg_default;
    const rc_config *cfg = cfg_in;
    rc_baseline bl;
    rc_zscores z;
    size_t *order = NULL;
    double *work = NULL;
    double subs[4], weights[4], wcfg[4];   /* 顺序固定：hrv, rhr, sleep, resp */
    double debt_prev, s_sleep, delta_resp, rhr_delta, r_resp, late_adj, s_hrv, s_rhr, s_resp;
    double tot, raw, deficit_today, trend, cap, internal, d;
    char trend_reasons[4][RC_TEXT_MAX];
    int n_trend = 0, rc, i, score;
    rc_date today;

    if (!night || !res || (n && !history) ||
        symptoms < RC_SYMPTOMS_NONE || symptoms > RC_SYMPTOMS_MODERATE_SEVERE)
        return RC_ERR_ARG;
    if (!cfg) {
        rc_config_default(&cfg_default);
        cfg = &cfg_default;
    }
    today = night->night_date;

    order = RC_ALLOC_N(size_t, n);
    work = RC_ALLOC_N(double, 3 * (n + 1));
    if (!order || !work) {
        rc = RC_ERR_NOMEM;
        goto done;
    }
    rc_order_by_date(history, n, order);
    rc = rc_baseline_from_order(history, order, n, today, cfg, &bl);
    if (rc != RC_OK)
        goto done;

    result_init(res, today);
    res->baseline_stage = bl.stage;
    res->baseline_nights = bl.n_nights;

    /* 1. 有效性（6.1） */
    if (!night->valid_night) {
        set_unscored(res, night->invalid_reason == RC_INVALID_SLEEP_TOO_SHORT
                              ? RC_STATE_SLEEP_TOO_SHORT
                              : RC_STATE_INSUFFICIENT_DATA);
        goto done;
    }

    /* 2. 睡眠子分。冷启动期也算并保存，之后建睡眠分基线要用。 */
    debt_prev = rc_sleep_debt(history, n, today, bl.sleep_need, cfg, cfg->debt_days);
    s_sleep = rc_sleep_score(night, bl.sleep_need, debt_prev, bl.midpoint_median, cfg,
                             &res->sleep_detail);
    res->subscores.sleep = rc_round_dp(s_sleep, 1);

    if (bl.n_nights < cfg->stage_estimating) {
        set_unscored(res, RC_STATE_CALIBRATING);
        goto done;
    }

    /* 3. z 分数。HRV 和 RHR 都没有就不出分。 */
    z.hrv = rc_baseline_z(&bl, RC_METRIC_RECOVERY_HRV, night->recovery_hrv);
    z.late = rc_baseline_z(&bl, RC_METRIC_LATE_HRV, night->late_hrv);
    z.early = rc_baseline_z(&bl, RC_METRIC_EARLY_HRV, night->early_hrv);
    z.rhr = rc_baseline_z(&bl, RC_METRIC_RHR, night->rhr);
    z.resp = rc_baseline_z(&bl, RC_METRIC_RESP, night->resp);
    if (rc_is_na(z.hrv) && rc_is_na(z.rhr)) {
        set_unscored(res, RC_STATE_NO_CORE_SIGNAL);
        goto done;
    }
    delta_resp = (!rc_is_na(night->resp) && bl.metrics[RC_METRIC_RESP].n)
                     ? night->resp - bl.metrics[RC_METRIC_RESP].median : RC_NA;
    rhr_delta = (!rc_is_na(night->rhr) && bl.metrics[RC_METRIC_RHR].n)
                    ? night->rhr - bl.metrics[RC_METRIC_RHR].median : RC_NA;
    r_resp = rc_resp_severity(z.resp, delta_resp, cfg);
    z.resp_r = r_resp;

    /* 4. 子分（第 7 章） */
    late_adj = rc_late_adjustment(z.early, z.late, z.rhr, r_resp, cfg);
    s_hrv = rc_hrv_subscore(z.hrv, cfg);
    if (!rc_is_na(s_hrv))
        s_hrv = rc_clamp(s_hrv + late_adj, 0, cfg->hrv_max);
    else
        late_adj = 0.0;
    s_rhr = rc_is_na(z.rhr) ? RC_NA : 100 - rc_rhr_penalty(z.rhr, cfg);
    s_resp = rc_is_na(r_resp) ? RC_NA : 100 - rc_resp_penalty(r_resp, cfg);
    subs[0] = s_hrv;
    subs[1] = s_rhr;
    subs[2] = s_sleep;
    subs[3] = s_resp;

    /* 5. 缺失项的权重按比例分给其余项（8.1） */
    wcfg[0] = cfg->weights.hrv;
    wcfg[1] = cfg->weights.rhr;
    wcfg[2] = cfg->weights.sleep;
    wcfg[3] = cfg->weights.resp;
    tot = 0.0;
    for (i = 0; i < 4; i++)
        if (!rc_is_na(subs[i]))
            tot += wcfg[i];
    raw = 0.0;
    for (i = 0; i < 4; i++) {
        weights[i] = rc_is_na(subs[i]) ? RC_NA : wcfg[i] / tot;
        if (!rc_is_na(subs[i]))
            raw += weights[i] * subs[i];
    }

    /* 6. 置信度、不适风险、趋势、门控 */
    res->confidence = rc_confidence_level(night, &bl, cfg, res->confidence_reasons,
                                          &res->n_confidence_reasons);
    res->illness_level = rc_illness_points(&z, rhr_delta, s_sleep, symptoms, cfg,
                                           &res->illness_points, &res->illness_physio_points);
    d = bl.sleep_need - night->tst_min;
    if (!(d > 0.0))
        d = 0.0;
    deficit_today = cfg->debt_night_cap < d ? cfg->debt_night_cap : d;
    trend = rc_trend_adjustment(history, order, n, today, z.hrv, z.rhr, r_resp, deficit_today,
                                bl.sleep_need, cfg, work, trend_reasons, &n_trend);
    res->n_gates = rc_evaluate_gates(&z, s_sleep, night->tst_min, symptoms, res->confidence,
                                     res->illness_level, cfg, res->gates);
    cap = 100.0;
    for (i = 0; i < res->n_gates; i++)
        if (res->gates[i].cap < cap)
            cap = res->gates[i].cap;

    /* 7. 最终分数（8.3）：原始分 + 趋势，再封顶，四舍五入到整数 */
    d = raw + trend;
    internal = rc_clamp(cap < d ? cap : d, 0, 100);
    score = rc_round_half_up(internal);
    rc_tier_of(score, cfg, &res->color, &res->tier);

    res->state = RC_STATE_SCORED;
    res->score = score;
    res->score_internal = rc_round_dp(internal, 2);
    res->raw = rc_round_dp(raw, 2);
    rc_strcpy(res->advice, sizeof res->advice, rc_tier_advice(res->tier));
    for (i = 0; i < n_trend && res->n_confidence_reasons < RC_MAX_REASONS; i++)
        rc_strcpy(res->confidence_reasons[res->n_confidence_reasons++], RC_TEXT_MAX,
                  trend_reasons[i]);
    res->trend_adjustment = rc_round_dp(trend, 2);
    res->late_adjustment = rc_round_dp(late_adj, 2);
    res->cap = cap;
    res->subscores.hrv = round_or_na(s_hrv, 1);
    res->subscores.rhr = round_or_na(s_rhr, 1);
    res->subscores.sleep = round_or_na(s_sleep, 1);
    res->subscores.resp = round_or_na(s_resp, 1);
    res->weights_used.hrv = round_or_na(weights[0], 3);
    res->weights_used.rhr = round_or_na(weights[1], 3);
    res->weights_used.sleep = round_or_na(weights[2], 3);
    res->weights_used.resp = round_or_na(weights[3], 3);
    res->z.hrv = round_or_na(z.hrv, 3);
    res->z.late = round_or_na(z.late, 3);
    res->z.early = round_or_na(z.early, 3);
    res->z.rhr = round_or_na(z.rhr, 3);
    res->z.resp = round_or_na(z.resp, 3);
    res->z.resp_r = round_or_na(r_resp, 3);
    rc_strcpy(res->illness_message, sizeof res->illness_message,
              rc_illness_text(res->illness_level));

    /* 核心指标偏离过大或不适风险偏高的夜，之后不进基线（5.4） */
    res->baseline_outlier =
        (!rc_is_na(z.hrv) && fabs(z.hrv) > cfg->outlier_z) ||
        (!rc_is_na(z.late) && fabs(z.late) > cfg->outlier_z) ||
        (!rc_is_na(z.rhr) && fabs(z.rhr) > cfg->outlier_z) ||
        (!rc_is_na(r_resp) && fabs(r_resp) > cfg->outlier_z) ||
        res->illness_level == RC_ILL_MODERATE || res->illness_level == RC_ILL_HIGH;

    rc_top_factors(res);
    rc_summary_text(res);

done:
    RC_FREE(order);
    RC_FREE(work);
    return rc;
}

void rc_day_record_make(rc_day_record *rec, const rc_night_metrics *night,
                        int symptoms, const rc_result *result)
{
    if (!rec || !night)
        return;
    memset(rec, 0, sizeof *rec);
    rec->night = *night;
    rec->symptoms = symptoms;
    rec->sleep_subscore = RC_NA;
    rec->z_hrv = rec->z_rhr = rec->resp_r = RC_NA;
    if (!result)
        return;
    rec->has_result = 1;
    rec->state = result->state;
    rec->baseline_outlier = result->baseline_outlier;
    rec->illness_level = result->illness_level;
    rec->sleep_subscore = result->subscores.sleep;
    rec->z_hrv = result->z.hrv;
    rec->z_rhr = result->z.rhr;
    rec->resp_r = result->z.resp_r;
}

/* ---------------------------------------------------------------- rc_engine */

struct rc_engine {
    rc_config      cfg;
    rc_day_record *records;   /* 按 night_date 升序，日期唯一 */
    size_t         count;
    size_t         cap;
};

rc_engine *rc_engine_create(const rc_config *cfg)
{
    rc_engine *e = (rc_engine *)RC_MALLOC(sizeof *e);

    if (!e)
        return NULL;
    if (cfg)
        e->cfg = *cfg;
    else
        rc_config_default(&e->cfg);
    e->records = NULL;
    e->count = 0;
    e->cap = 0;
    return e;
}

void rc_engine_destroy(rc_engine *e)
{
    if (!e)
        return;
    RC_FREE(e->records);
    RC_FREE(e);
}

static int reserve(rc_engine *e, size_t need)
{
    rc_day_record *p;
    size_t cap;

    if (need <= e->cap)
        return RC_OK;
    cap = e->cap ? e->cap * 2 : 64;
    if (cap < need)
        cap = need;
    p = (rc_day_record *)RC_REALLOC(e->records, cap * sizeof *p);
    if (!p)
        return RC_ERR_NOMEM;
    e->records = p;
    e->cap = cap;
    return RC_OK;
}

/* 第一个 night_date ≥ d 的位置 */
static size_t lower_bound(const rc_engine *e, rc_date d)
{
    size_t lo = 0, hi = e->count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (e->records[mid].night.night_date < d)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

int rc_engine_add_metrics(rc_engine *e, const rc_night_metrics *metrics, int symptoms,
                          rc_result *result)
{
    rc_day_record rec;
    size_t pos;
    int rc, replacing;

    if (!e || !metrics || !result)
        return RC_ERR_ARG;
    /* 先留好位置，后面就不会再失败，出错时历史保持原样 */
    rc = reserve(e, e->count + 1);
    if (rc != RC_OK)
        return rc;

    pos = lower_bound(e, metrics->night_date);
    replacing = pos < e->count && e->records[pos].night.night_date == metrics->night_date;

    /* 同日期的旧记录不能参与本次评分：先挪到数组末尾之外，评分失败再放回去 */
    if (replacing) {
        rec = e->records[pos];
        memmove(&e->records[pos], &e->records[pos + 1],
                (e->count - pos - 1) * sizeof *e->records);
        e->count--;
    }
    rc = rc_score_day(metrics, e->records, e->count, symptoms, &e->cfg, result);
    if (rc != RC_OK) {
        if (replacing) {
            memmove(&e->records[pos + 1], &e->records[pos],
                    (e->count - pos) * sizeof *e->records);
            e->records[pos] = rec;
            e->count++;
        }
        return rc;
    }

    rc_day_record_make(&rec, metrics, symptoms, result);
    memmove(&e->records[pos + 1], &e->records[pos], (e->count - pos) * sizeof *e->records);
    e->records[pos] = rec;
    e->count++;
    return RC_OK;
}

int rc_engine_add_night(rc_engine *e, const rc_night_input *in, int symptoms,
                        rc_result *result, rc_night_metrics *metrics)
{
    rc_night_metrics m;
    int rc;

    if (!e || !in || !result)
        return RC_ERR_ARG;
    rc = rc_process_night(in, &e->cfg, &m, NULL, 0, NULL);
    if (rc != RC_OK)
        return rc;
    rc = rc_engine_add_metrics(e, &m, symptoms, result);
    if (rc == RC_OK && metrics)
        *metrics = m;
    return rc;
}

size_t rc_engine_count(const rc_engine *e)
{
    return e ? e->count : 0;
}

const rc_day_record *rc_engine_history(const rc_engine *e)
{
    return (e && e->count) ? e->records : NULL;
}

int rc_engine_load(rc_engine *e, const rc_day_record *records, size_t n)
{
    rc_day_record *buf;
    size_t i, j, m = 0;

    if (!e || (n && !records))
        return RC_ERR_ARG;
    buf = RC_ALLOC_N(rc_day_record, n);
    if (!buf)
        return RC_ERR_NOMEM;

    /* 稳定插入排序后去重，同日期留靠后的 */
    for (i = 0; i < n; i++) {
        for (j = i; j > 0 && buf[j - 1].night.night_date > records[i].night.night_date; j--)
            buf[j] = buf[j - 1];
        buf[j] = records[i];
    }
    for (i = 0; i < n; i++) {
        if (m > 0 && buf[m - 1].night.night_date == buf[i].night.night_date)
            buf[m - 1] = buf[i];
        else
            buf[m++] = buf[i];
    }

    RC_FREE(e->records);
    e->records = buf;
    e->count = m;
    e->cap = n ? n : 1;
    return RC_OK;
}

size_t rc_engine_prune_before(rc_engine *e, rc_date before)
{
    size_t k;

    if (!e)
        return 0;
    k = lower_bound(e, before);
    if (k) {
        memmove(e->records, e->records + k, (e->count - k) * sizeof *e->records);
        e->count -= k;
    }
    return k;
}

const rc_config *rc_engine_config(const rc_engine *e)
{
    return e ? &e->cfg : NULL;
}
