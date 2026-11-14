/* 个人基线（第 5 章）：只用今天之前的合格夜；中位数 + MAD；sigma 向人群先验收缩；新常态回纳。 */
#include "rc_internal.h"

#include <string.h>

void rc_order_by_date(const rc_day_record *history, size_t n, size_t *order)
{
    size_t i, j;

    for (i = 0; i < n; i++) {
        rc_date d = history[i].night.night_date;
        for (j = i; j > 0 && history[order[j - 1]].night.night_date > d; j--)
            order[j] = order[j - 1];
        order[j] = i;
    }
}

/* 被标记为异常的夜：基线离群，或不适风险 ≥ moderate（5.4） */
static int is_flagged(const rc_day_record *r)
{
    return r->has_result &&
           (r->baseline_outlier || r->illness_level == RC_ILL_MODERATE ||
            r->illness_level == RC_ILL_HIGH);
}

static int stage_of(int n, const rc_config *cfg)
{
    if (n < cfg->stage_estimating)
        return RC_BL_CALIBRATING;
    if (n < cfg->stage_trial)
        return RC_BL_ESTIMATING;
    if (n < cfg->stage_provisional)
        return RC_BL_TRIAL;
    if (n < cfg->stage_formal)
        return RC_BL_PROVISIONAL;
    return RC_BL_FORMAL;
}

static double metric_value(const rc_day_record *r, int metric)
{
    switch (metric) {
    case RC_METRIC_RECOVERY_HRV: return r->night.recovery_hrv;
    case RC_METRIC_LATE_HRV:     return r->night.late_hrv;
    case RC_METRIC_EARLY_HRV:    return r->night.early_hrv;
    case RC_METRIC_RHR:          return r->night.rhr;
    case RC_METRIC_RESP:         return r->night.resp;
    case RC_METRIC_SLEEP_SCORE:  return r->has_result ? r->sleep_subscore : RC_NA;
    default:                     return RC_NA;
    }
}

int rc_baseline_from_order(const rc_day_record *history, const size_t *order, size_t n,
                           rc_date today, const rc_config *cfg, rc_baseline *out)
{
    size_t *ok;          /* 通过基本排除规则的记录下标，按日期升序 */
    double *vals, *dev;
    size_t n_past = 0, n_ok = 0, n_el = 0, run = 0, readmit_from, i, first;
    int metric;

    memset(out, 0, sizeof *out);
    out->stage = RC_BL_CALIBRATING;
    out->sleep_need = cfg->sleep_need_default;
    out->midpoint_median = RC_NA;

    ok = RC_ALLOC_N(size_t, n);
    vals = RC_ALLOC_N(double, 2 * n);
    if (!ok || !vals) {
        RC_FREE(ok);
        RC_FREE(vals);
        return RC_ERR_NOMEM;
    }
    dev = vals + n;

    /* 回看窗口内的记录；其中有效、睡够、没有明显不适的才可能进基线 */
    for (i = 0; i < n; i++) {
        const rc_day_record *r = &history[order[i]];
        int delta = (int)today - (int)r->night.night_date;

        if (!(delta > 0 && delta <= cfg->baseline_lookback_days))
            continue;
        n_past++;
        if (r->night.valid_night && r->night.tst_min >= cfg->baseline_exclude_tst_min &&
            r->symptoms != RC_SYMPTOMS_MODERATE_SEVERE)
            ok[n_ok++] = order[i];
    }

    /* 新常态：最近连续 ≥ new_normal_run 晚都被标记异常且没有任何症状，
       说明是生理状态变了而不是偶发，这一串重新算数。 */
    while (run < n_ok) {
        const rc_day_record *r = &history[ok[n_ok - 1 - run]];
        if (!(is_flagged(r) && r->symptoms == RC_SYMPTOMS_NONE))
            break;
        run++;
    }
    if (run < (size_t)(cfg->new_normal_run > 0 ? cfg->new_normal_run : 0))
        run = 0;
    readmit_from = n_ok - run;

    /* 去掉其余被标记的夜，剩下的原地压缩到 ok[0..n_el) */
    for (i = 0; i < n_ok; i++)
        if (i >= readmit_from || !is_flagged(&history[ok[i]]))
            ok[n_el++] = ok[i];

    /* 只留最近 baseline_window 晚 */
    first = 0;
    if (cfg->baseline_window > 0 && n_el > (size_t)cfg->baseline_window)
        first = n_el - (size_t)cfg->baseline_window;
    n_el -= first;

    out->n_nights = (int)n_el;
    out->stage = stage_of((int)n_el, cfg);
    out->readmitted = (int)run;
    out->excluded = (int)(n_past - n_el);

    for (metric = 0; metric < RC_METRIC_COUNT; metric++) {
        size_t c = 0;
        double med, mad, sp, prior, k, sigma, floor_v = cfg->sigma_floor[metric];

        for (i = 0; i < n_el; i++) {
            double v = metric_value(&history[ok[first + i]], metric);
            if (!rc_is_na(v))
                vals[c++] = v;
        }
        if (c < (size_t)(cfg->baseline_min_values > 0 ? cfg->baseline_min_values : 0) || c == 0)
            continue;

        med = rc_median(vals, c);
        for (i = 0; i < c; i++)
            dev[i] = fabs(vals[i] - med);
        mad = rc_median(dev, c);

        /* 样本 sigma（MAD 估计，不低于下限）与先验按样本数加权合并（5.2）。
           夜数少时先验占主导，避免前几晚 sigma 过小把 z 放大。 */
        sp = 1.4826 * mad;
        if (floor_v > sp)
            sp = floor_v;
        prior = cfg->sigma_prior[metric];
        k = cfg->prior_k;
        sigma = sqrt(((double)c * pow(sp, 2.0) + k * pow(prior, 2.0)) / ((double)c + k));

        out->metrics[metric].median = med;
        out->metrics[metric].sigma = floor_v > sigma ? floor_v : sigma;
        out->metrics[metric].n = (int)c;
    }

    /* 个人睡眠需求 = 基线夜 TST 的中位数，限制在合理范围内 */
    if (n_el > 0 && n_el >= (size_t)(cfg->baseline_min_values > 0 ? cfg->baseline_min_values : 0)) {
        for (i = 0; i < n_el; i++)
            vals[i] = history[ok[first + i]].night.tst_min;
        out->sleep_need = rc_clamp(rc_median(vals, n_el), cfg->sleep_need_min,
                                   cfg->sleep_need_max);
    }

    {
        size_t c = 0;
        for (i = 0; i < n_el; i++) {
            double v = history[ok[first + i]].night.midpoint_min;
            if (!rc_is_na(v))
                vals[c++] = v;
        }
        if (c > 0 && c >= (size_t)(cfg->baseline_min_values > 0 ? cfg->baseline_min_values : 0))
            out->midpoint_median = rc_median(vals, c);
    }

    RC_FREE(ok);
    RC_FREE(vals);
    return RC_OK;
}

int rc_build_baseline(const rc_day_record *history, size_t n, rc_date today,
                      const rc_config *cfg_in, rc_baseline *out)
{
    rc_config cfg_default;
    const rc_config *cfg = cfg_in;
    size_t *order;
    int rc;

    if (!out || (n && !history))
        return RC_ERR_ARG;
    if (!cfg) {
        rc_config_default(&cfg_default);
        cfg = &cfg_default;
    }
    order = RC_ALLOC_N(size_t, n);
    if (!order)
        return RC_ERR_NOMEM;
    rc_order_by_date(history, n, order);
    rc = rc_baseline_from_order(history, order, n, today, cfg, out);
    RC_FREE(order);
    return rc;
}

double rc_baseline_z(const rc_baseline *bl, int metric, double value)
{
    const rc_metric_baseline *mb = &bl->metrics[metric];

    if (mb->n == 0 || rc_is_na(value))
        return RC_NA;
    return (value - mb->median) / mb->sigma;
}
