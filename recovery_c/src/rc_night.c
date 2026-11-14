/* 一晚输入 → 当晚指标：Recovery / Early / Late HRV、RHR、RESP、睡眠结构、有效性（4.4–4.6、6.1）。 */
#include "rc_internal.h"

#include <stdlib.h>
#include <string.h>

static int is_nrem(int stage)
{
    return stage == RC_N2 || stage == RC_N3;
}

static int is_good(const rc_segment *s, const rc_config *cfg)
{
    return s->weight > 0 && s->w_quality >= cfg->q_good_segment;
}

/* 片段中点落在主睡眠期内 */
static int in_sleep(const rc_segment *s)
{
    return 0.0 <= s->pos && s->pos <= 1.0;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/*
 * 对满足条件的好片段求 lnRMSSD 加权中位数。
 *   nrem_only  只取 N2/N3
 *   half       0 = 不限；-1 = pos < 0.5；+1 = pos ≥ 0.5
 *   min_count  片段数不足返回 RC_NA
 *   v, w       临时区，各 n 个 double
 */
static double hrv_weighted_median(const rc_segment *segs, size_t n, const rc_config *cfg,
                                  int nrem_only, int half, int min_count,
                                  double *v, double *w)
{
    size_t i, c = 0;

    for (i = 0; i < n; i++) {
        const rc_segment *s = &segs[i];

        if (!is_good(s, cfg))
            continue;
        if (nrem_only && !is_nrem(s->stage))
            continue;
        if ((half < 0 && !(s->pos < 0.5)) || (half > 0 && !(s->pos >= 0.5)))
            continue;
        v[c] = s->ln_rmssd;
        w[c] = s->weight;
        c++;
    }
    if (c < (size_t)(min_count > 0 ? min_count : 0))
        return RC_NA;
    return rc_weighted_median(v, w, c);
}

int rc_process_night(const rc_night_input *in, const rc_config *cfg_in,
                     rc_night_metrics *out,
                     rc_segment *segs_out, size_t segs_cap, size_t *n_segs_out)
{
    rc_config cfg_default;
    const rc_config *cfg = cfg_in;
    rc_iv *iv = NULL;
    rc_hrv_sample *samples = NULL;
    rc_segment *segs = NULL;
    double *work = NULL;
    unsigned char *flags = NULL;
    size_t n_iv = 0, n_segs = 0, n_work, i, c;
    int64_t onset = 0, offset = 0;
    int n_good = 0, n_nrem = 0, rc = RC_OK;

    if (!in || !out || (in->n_hrv && !in->hrv) || (in->n_stages && !in->stages) ||
        (segs_cap && !segs_out))
        return RC_ERR_ARG;
    if (!cfg) {
        rc_config_default(&cfg_default);
        cfg = &cfg_default;
    }

    /* work 要同时够离群检测（n + 2k）和加权中位数（2n）用 */
    n_work = 2 * in->n_hrv +
             2 * (size_t)(cfg->ln_outlier_neighbors > 0 ? cfg->ln_outlier_neighbors : 0);
    iv = RC_ALLOC_N(rc_iv, in->n_stages);
    samples = RC_ALLOC_N(rc_hrv_sample, in->n_hrv);
    segs = RC_ALLOC_N(rc_segment, in->n_hrv);
    work = RC_ALLOC_N(double, n_work);
    flags = RC_ALLOC_N(unsigned char, in->n_hrv);
    if (!iv || !samples || !segs || !work || !flags) {
        rc = RC_ERR_NOMEM;
        goto done;
    }

    rc = rc_normalize_stages(in->stages, in->n_stages, iv, &n_iv);
    if (rc != RC_OK)
        goto done;

    rc_night_metrics_init(out, in->night_date);
    out->sleep_score_external = in->sleep_score_external;
    rc_extract_sleep_metrics(iv, n_iv, in->has_in_bed_start, in->in_bed_start,
                             in->utc_offset_sec, out);

    if (in->n_hrv)
        memcpy(samples, in->hrv, in->n_hrv * sizeof *samples);
    rc_segment_night(in, samples, iv, n_iv, cfg, segs, work, flags, &n_segs);

    for (i = 0; i < n_segs; i++) {
        if (is_good(&segs[i], cfg)) {
            n_good++;
            if (is_nrem(segs[i].stage))
                n_nrem++;
        }
    }
    out->n_good_segments = n_good;
    out->n_nrem_segments = n_nrem;

    if (rc_sleep_bounds(iv, n_iv, &onset, &offset)) {
        int64_t ref = in->has_in_bed_start ? in->in_bed_start : iv[0].start;
        out->wear_coverage = rc_wear_coverage(segs, n_segs, (double)(onset - ref),
                                              (double)(offset - ref), cfg->q_good_segment);
    } else {
        out->wear_coverage = 0.0;
    }

    /* Recovery HRV 用全部好片段；Early / Late 只用 NREM，按相对位置 0.5 切开（4.4） */
    if (n_good >= cfg->min_good_segments && n_nrem >= cfg->min_nrem_segments) {
        double *v = work, *w = work + in->n_hrv;

        out->recovery_hrv = hrv_weighted_median(segs, n_segs, cfg, 0, 0, 0, v, w);
        out->early_hrv = hrv_weighted_median(segs, n_segs, cfg, 1, -1,
                                             cfg->min_early_late_segments, v, w);
        out->late_hrv = hrv_weighted_median(segs, n_segs, cfg, 1, +1,
                                            cfg->min_early_late_segments, v, w);
    }

    /* RHR：主睡眠内非清醒、质量合格的片段里，心率最低的若干个取均值（4.5） */
    for (i = 0, c = 0; i < n_segs; i++) {
        const rc_segment *s = &segs[i];
        if (in_sleep(s) && !rc_is_na(s->hr) && s->stage != RC_WAKE &&
            s->w_quality >= cfg->q_good_segment)
            work[c++] = s->hr;
    }
    if (cfg->rhr_lowest_segments > 0 && c >= (size_t)cfg->rhr_lowest_segments) {
        qsort(work, c, sizeof *work, cmp_double);
        out->rhr = rc_pairwise_sum(work, (size_t)cfg->rhr_lowest_segments) /
                   cfg->rhr_lowest_segments;
    }

    /* RESP：同样范围内的中位数，片段数要够（4.5） */
    for (i = 0, c = 0; i < n_segs; i++) {
        const rc_segment *s = &segs[i];
        if (in_sleep(s) && !rc_is_na(s->resp) && s->stage != RC_WAKE &&
            s->w_quality >= cfg->q_good_segment)
            work[c++] = s->resp;
    }
    if (c > 0 && c >= (size_t)(cfg->min_resp_segments > 0 ? cfg->min_resp_segments : 0))
        out->resp = rc_median(work, c);

    /* 有效性（6.1）：先看时长，再看佩戴 */
    if (out->tst_min < cfg->min_tst_min) {
        out->valid_night = 0;
        out->invalid_reason = RC_INVALID_SLEEP_TOO_SHORT;
    } else if (out->wear_coverage < cfg->min_wear_coverage) {
        out->valid_night = 0;
        out->invalid_reason = RC_INVALID_LOW_WEAR_COVERAGE;
    }

    if (segs_out) {
        size_t ncopy = n_segs < segs_cap ? n_segs : segs_cap;
        if (ncopy)
            memcpy(segs_out, segs, ncopy * sizeof *segs);
    }
    if (n_segs_out)
        *n_segs_out = n_segs;

done:
    RC_FREE(iv);
    RC_FREE(samples);
    RC_FREE(segs);
    RC_FREE(work);
    RC_FREE(flags);
    return rc;
}
