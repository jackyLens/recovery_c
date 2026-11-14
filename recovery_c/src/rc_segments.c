/* 5 分钟 HRV 记录 → 片段：数值校验、离群剔除、分期映射、四类权重（4.1–4.3）。 */
#include "rc_internal.h"

static double stage_weight(int stage, int stable, const rc_config *cfg)
{
    switch (stage) {
    case RC_N3:  return stable ? cfg->w_n3_stable : cfg->w_n3_mixed;
    case RC_N2:  return stable ? cfg->w_n2_stable : cfg->w_n2_mixed;
    case RC_N1:  return cfg->w_n1;
    case RC_REM: return cfg->w_rem;
    default:     return cfg->w_wake;
    }
}

/* NaN 和 ±inf 都当缺失 */
static double finite_or_na(double v)
{
    return isfinite(v) ? v : RC_NA;
}

/*
 * 输入值 → (rmssd_ms, lnRMSSD)。is_ln 时输入已是对数，先还原成毫秒再判范围。
 * 超出 [rmssd_min_ms, rmssd_max_ms] 两个输出都置 RC_NA。
 */
static void ln_value(double v, int is_ln, const rc_config *cfg, double *rmssd, double *ln)
{
    double r;

    *rmssd = RC_NA;
    *ln = RC_NA;
    if (rc_is_na(v))
        return;
    r = is_ln ? exp(v) : v;
    if (!(cfg->rmssd_min_ms <= r && r <= cfg->rmssd_max_ms))
        return;
    *rmssd = r;
    *ln = log(r);
}

/*
 * 离群检测（4.1）：每个有效 ln 与前后各 k 个有效值的中位数比较，偏差超过 ln_outlier 置标记。
 * 邻居不足 2 个不判。只抓单点尖刺，整段偏移不会被判掉。
 *   segs   已填好 ln_rmssd
 *   vals   临时区，容量 n + 2k；前 n 个放有效值，后面给中位数用
 */
static void flag_outliers(const rc_segment *segs, size_t n, const rc_config *cfg,
                          double *vals, unsigned char *flags)
{
    size_t i, j, m = 0;
    size_t k = cfg->ln_outlier_neighbors > 0 ? (size_t)cfg->ln_outlier_neighbors : 0;
    double *nb = vals + n;

    for (i = 0; i < n; i++) {
        flags[i] = 0;
        if (!rc_is_na(segs[i].ln_rmssd))
            vals[m++] = segs[i].ln_rmssd;
    }
    for (i = 0, j = 0; i < n; i++) {
        size_t lo, hi, c = 0, t;

        if (rc_is_na(segs[i].ln_rmssd))
            continue;
        /* j 是当前值在有效序列里的位置 */
        lo = j > k ? j - k : 0;
        hi = j + 1 + k < m ? j + 1 + k : m;
        for (t = lo; t < j; t++)
            nb[c++] = vals[t];
        for (t = j + 1; t < hi; t++)
            nb[c++] = vals[t];
        if (c >= 2 && fabs(vals[j] - rc_median(nb, c)) > cfg->ln_outlier)
            flags[i] = 1;
        j++;
    }
}

/* 按 t_start 稳定排序；同一时刻重复上报只留最后一条。返回去重后的条数。 */
static size_t sort_dedup_samples(rc_hrv_sample *s, size_t n)
{
    size_t i, j, m = 0;

    for (i = 1; i < n; i++) {
        rc_hrv_sample cur = s[i];
        for (j = i; j > 0 && s[j - 1].t_start > cur.t_start; j--)
            s[j] = s[j - 1];
        s[j] = cur;
    }
    for (i = 0; i < n; i++) {
        if (m > 0 && s[m - 1].t_start == s[i].t_start)
            s[m - 1] = s[i];
        else
            s[m++] = s[i];
    }
    return m;
}

void rc_segment_night(const rc_night_input *in, rc_hrv_sample *samples,
                      const rc_iv *iv, size_t n_iv, const rc_config *cfg,
                      rc_segment *segs, double *work, unsigned char *flags, size_t *n_segs)
{
    int64_t onset = 0, offset = 0, ref;
    double onset_s, offset_s, spt, resp_med;
    size_t n, i, c;

    *n_segs = 0;
    if (!rc_sleep_bounds(iv, n_iv, &onset, &offset))
        return;

    /* 片段内的时间都用相对 ref 的秒数 */
    ref = in->has_in_bed_start ? in->in_bed_start : iv[0].start;
    onset_s = (double)(onset - ref);
    offset_s = (double)(offset - ref);
    spt = offset_s - onset_s;
    if (spt < 1.0)
        spt = 1.0;

    n = sort_dedup_samples(samples, in->n_hrv);

    for (i = 0; i < n; i++)
        ln_value(finite_or_na(samples[i].rmssd), in->hrv_is_ln, cfg,
                 &segs[i].rmssd, &segs[i].ln_rmssd);
    flag_outliers(segs, n, cfg, work, flags);

    for (i = 0; i < n; i++) {
        const rc_hrv_sample *x = &samples[i];
        rc_segment *s = &segs[i];
        int dur = x->duration_sec > 0 ? x->duration_sec : cfg->segment_sec;
        int64_t t0 = x->t_start, t1 = x->t_start + dur;
        double sec[5], labelled, lab_total, q, qin, mot, s0, s1, pos, wt;
        int stage = RC_WAKE, stable = 0, k;

        /* 分期映射（4.2） */
        labelled = rc_stage_overlap(iv, n_iv, t0, t1, sec);
        lab_total = sec[0] + sec[1] + sec[2] + sec[3] + sec[4];
        if (!(labelled < cfg->min_stage_label_frac || lab_total <= 0)) {
            for (k = 1; k < 5; k++)
                if (sec[k] > sec[stage])
                    stage = k;
            stable = sec[stage] / lab_total >= cfg->stable_frac && sec[RC_WAKE] == 0 &&
                     labelled >= cfg->stable_frac;
            if (sec[RC_WAKE] / lab_total > cfg->wake_frac_max) {
                stage = RC_WAKE;
                stable = 0;
            }
        }

        s->hr = finite_or_na(x->hr_bpm);
        if (!rc_is_na(s->hr) && !(cfg->hr_min_bpm <= s->hr && s->hr <= cfg->hr_max_bpm))
            s->hr = RC_NA;
        s->resp = finite_or_na(x->resp_brpm);

        /* 质量权重：上游质量，缺省 1；体动过大或离群置 0 */
        qin = finite_or_na(x->quality);
        q = rc_is_na(qin) ? 1.0 : rc_clamp(qin, 0.0, 1.0);
        mot = finite_or_na(x->motion);
        if (!rc_is_na(mot) && mot > cfg->motion_max)
            q = 0.0;
        if (flags[i])
            q = 0.0;

        /* 时间位置权重。判断顺序不能换：睡眠期外 → 醒前 → 入睡初期 → 前/中/后 */
        s0 = (double)(t0 - ref);
        s1 = s0 + dur;
        pos = ((s0 + s1) / 2 - onset_s) / spt;
        if (s1 <= onset_s || s0 >= offset_s)
            wt = 0.0;
        else if (s1 > offset_s - cfg->prewake_min * 60)
            wt = cfg->tw_prewake;
        else if (s0 < onset_s + cfg->onset_min * 60)
            wt = cfg->tw_onset;
        else if (pos < cfg->mid_band[0])
            wt = cfg->tw_first;
        else if (pos <= cfg->mid_band[1])
            wt = cfg->tw_mid;
        else
            wt = cfg->tw_late;

        s->idx = (int)i;
        s->t_start_s = s0;
        s->stage = stage;
        s->stable = stable;
        s->pos = pos;
        s->coverage = rc_is_na(s->ln_rmssd) ? 0.0 : q;
        s->w_quality = q;
        s->w_stage = stage_weight(stage, stable, cfg);
        s->w_time = wt;
        s->w_stability = 1.0;
        s->weight = 0.0;
        s->dur_s = (double)dur;
    }

    /* 整晚 RESP 中位数，稳定性权重用 */
    for (i = 0, c = 0; i < n; i++)
        if (!rc_is_na(segs[i].resp))
            work[c++] = segs[i].resp;
    resp_med = rc_median(work, c);

    for (i = 0; i < n; i++) {
        rc_segment *s = &segs[i];
        double w = 1.0;

        /* 心率与前后各 2 个窗口（不含自己）的中位数比较 */
        if (!rc_is_na(s->hr)) {
            double nb[4];
            size_t lo = i >= 2 ? i - 2 : 0, hi = i + 3 < n ? i + 3 : n, t;

            for (t = lo, c = 0; t < hi; t++)
                if (t != i && !rc_is_na(segs[t].hr))
                    nb[c++] = segs[t].hr;
            if (c > 0) {
                double dev = fabs(s->hr - rc_median(nb, c));
                if (dev > cfg->stab_hr_severe)
                    w = cfg->stab_w_severe;
                else if (dev > cfg->stab_hr_mild)
                    w = cfg->stab_w_mild;
            }
        }
        if (!rc_is_na(s->resp) && !rc_is_na(resp_med) &&
            fabs(s->resp - resp_med) > cfg->stab_resp_dev)
            w *= cfg->stab_w_resp;
        s->w_stability = w;

        /* 后半夜加权：只给稳定 NREM、质量高、生理稳定的片段。
           tw_mid 和 tw_late 默认相等，所以还要再判一次 pos。 */
        if (s->w_time == cfg->tw_late && s->pos > cfg->mid_band[1] &&
            (s->stage == RC_N2 || s->stage == RC_N3) && s->stable &&
            s->w_quality >= 0.8 && w == 1.0)
            s->w_time = cfg->tw_late_bonus;

        s->weight = rc_is_na(s->ln_rmssd)
                        ? 0.0
                        : s->w_quality * s->w_stage * s->w_time * s->w_stability;
    }
    *n_segs = n;
}

double rc_wear_coverage(const rc_segment *segs, size_t n, double onset_s, double offset_s,
                        double q_min)
{
    double spt = offset_s - onset_s, covered = 0.0, r;
    size_t i;

    if (spt <= 0)
        return 0.0;
    for (i = 0; i < n; i++) {
        double a, b, lo, hi;

        if (rc_is_na(segs[i].ln_rmssd) || segs[i].w_quality < q_min)
            continue;
        a = segs[i].t_start_s;
        b = a + segs[i].dur_s;
        lo = a > onset_s ? a : onset_s;
        hi = b < offset_s ? b : offset_s;
        if (hi > lo)
            covered += hi - lo;
    }
    r = covered / spt;
    return r < 1.0 ? r : 1.0;
}
