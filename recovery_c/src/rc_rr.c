/*
 * 逐搏 RR + 等长 epoch 分期 → 标准输入。
 * 主流程不依赖这里；只有拿不到 5 分钟 HRV 的设备才用。
 */
#include "rc_internal.h"

#include <stdlib.h>

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/*
 * RR 伪迹标记。valid[i] = 1 表示可用：
 *   1) 在 [rr_min_ms, rr_max_ms] 内；
 *   2) 与以它为中心的 w 拍中位数（只算满足 1 的拍）相差不超过 rr_jump_ratio。
 * 序列两端按边缘值延拓。窗口里没有任何满足 1 的拍时判无效。
 *   in_range  临时区，n 个字节
 *   win       临时区，w 个 double
 */
static void rr_valid_mask(const double *rr, size_t n, size_t w, const rc_config *cfg,
                          unsigned char *valid, unsigned char *in_range, double *win)
{
    size_t pad = w / 2, i, k;

    for (i = 0; i < n; i++)
        in_range[i] = rr[i] >= cfg->rr_min_ms && rr[i] <= cfg->rr_max_ms;

    for (i = 0; i < n; i++) {
        size_t c = 0;
        double med;

        for (k = 0; k < w; k++) {
            /* 延拓后的下标 i − pad + k，越界取两端 */
            size_t j = i + k < pad ? 0 : i + k - pad;
            if (j >= n)
                j = n - 1;
            if (in_range[j])
                win[c++] = rr[j];
        }
        valid[i] = 0;
        if (!in_range[i] || c == 0)
            continue;
        qsort(win, c, sizeof *win, cmp_double);
        med = (c & 1) ? win[c / 2] : (win[c / 2 - 1] + win[c / 2]) / 2.0;
        valid[i] = fabs(rr[i] - med) <= cfg->rr_jump_ratio * med;
    }
}

/* 升序数组里第一个 ≥ x 的位置 */
static size_t lower_bound(const double *a, size_t n, double x)
{
    size_t lo = 0, hi = n;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (a[mid] < x)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

size_t rc_rr_window_count(double duration_sec, const rc_config *cfg_in)
{
    rc_config cfg_default;
    const rc_config *cfg = cfg_in;

    if (!cfg) {
        rc_config_default(&cfg_default);
        cfg = &cfg_default;
    }
    if (!(duration_sec > 0) || cfg->segment_sec <= 0)
        return 0;
    return (size_t)ceil(duration_sec / cfg->segment_sec);
}

int rc_rr_to_hrv5min(const double *rr_ms, size_t n_rr, const double *rr_t_s,
                     int64_t start, double duration_sec,
                     const double *resp_brpm, size_t n_resp,
                     const rc_config *cfg_in,
                     rc_hrv_sample *out, size_t out_cap, size_t *n_out)
{
    rc_config cfg_default;
    const rc_config *cfg = cfg_in;
    double *t_end = NULL, *tmp = NULL, *win = NULL;
    unsigned char *valid = NULL, *in_range = NULL;
    size_t n_win, i, k, w;
    int L, rc = RC_OK;

    if (!n_out || (!out && out_cap) || (n_rr && !rr_ms) || (n_resp && !resp_brpm))
        return RC_ERR_ARG;
    if (!cfg) {
        rc_config_default(&cfg_default);
        cfg = &cfg_default;
    }
    L = cfg->segment_sec;
    if (L <= 0)
        return RC_ERR_ARG;
    n_win = rc_rr_window_count(duration_sec, cfg);
    if (n_win > out_cap)
        return RC_ERR_CAPACITY;

    w = cfg->rr_local_window > 0 ? (size_t)cfg->rr_local_window : 1;
    t_end = RC_ALLOC_N(double, n_rr);
    tmp = RC_ALLOC_N(double, n_rr);
    win = RC_ALLOC_N(double, w);
    valid = RC_ALLOC_N(unsigned char, n_rr);
    in_range = RC_ALLOC_N(unsigned char, n_rr);
    if (!t_end || !tmp || !win || !valid || !in_range) {
        rc = RC_ERR_NOMEM;
        goto done;
    }

    /* 每个 RR 的结束时刻，秒。没给时间戳就认为 RR 首尾相接、从 start 开始。 */
    if (rr_t_s) {
        for (i = 0; i < n_rr; i++)
            t_end[i] = rr_t_s[i];
    } else {
        double acc = 0.0;
        for (i = 0; i < n_rr; i++) {
            acc += rr_ms[i];
            t_end[i] = acc / 1000.0;
        }
    }
    rr_valid_mask(rr_ms, n_rr, w, cfg, valid, in_range, win);

    for (i = 0; i < n_win; i++) {
        size_t lo = lower_bound(t_end, n_rr, (double)i * L);
        size_t hi = lower_bound(t_end, n_rr, (double)(i + 1) * L);
        size_t n = hi - lo, n_ok = 0, n_pair = 0;
        const double *seg = rr_ms + lo;
        const unsigned char *ok = valid + lo;
        rc_hrv_sample *s = &out[i];
        double q = 0.0;

        rc_hrv_sample_init(s, start + (int64_t)i * L, RC_NA);
        s->duration_sec = L;

        if (n > 0) {
            double sum_ok, cov, art, q_cov, q_art;

            for (k = 0; k < n; k++)
                if (ok[k])
                    tmp[n_ok++] = seg[k];
            sum_ok = rc_pairwise_sum(tmp, n_ok);

            /* 质量 = min(有效 RR 时间覆盖率得分, 伪迹比例得分)；拍数太少直接 0 */
            cov = sum_ok / (L * 1000.0);
            art = 1 - (double)n_ok / (double)n;
            q_cov = rc_ramp(cov, cfg->q_cov_min, cfg->q_cov_good);
            q_art = 1 - rc_ramp(art, cfg->q_art_good, cfg->q_art_max);
            q = q_art < q_cov ? q_art : q_cov;
            if (n < (size_t)(cfg->seg_min_beats > 0 ? cfg->seg_min_beats : 0))
                q = 0.0;

            if (n_ok >= 30)
                s->hr_bpm = 60000.0 / (sum_ok / (double)n_ok);

            /* RMSSD 只用前后两拍都有效的相邻差 */
            for (k = 1; k < n; k++) {
                if (ok[k] && ok[k - 1]) {
                    double d = seg[k] - seg[k - 1];
                    tmp[n_pair++] = d * d;
                }
            }
            if (n_pair > 0 &&
                n_pair >= (size_t)(cfg->min_rmssd_pairs > 0 ? cfg->min_rmssd_pairs : 0))
                s->rmssd = sqrt(rc_pairwise_sum(tmp, n_pair) / (double)n_pair);
        }
        s->quality = q;
        if (resp_brpm && i < n_resp && isfinite(resp_brpm[i]))
            s->resp_brpm = resp_brpm[i];
    }
    *n_out = n_win;

done:
    RC_FREE(t_end);
    RC_FREE(tmp);
    RC_FREE(win);
    RC_FREE(valid);
    RC_FREE(in_range);
    return rc;
}

int rc_hypnogram_to_intervals(const int *hyp, size_t n, int64_t start, int epoch_sec,
                              rc_stage_interval *out, size_t out_cap, size_t *n_out)
{
    size_t i = 0, m = 0;

    if (!n_out || (n && (!hyp || !out)) || epoch_sec <= 0)
        return RC_ERR_ARG;
    while (i < n) {
        size_t j = i;

        while (j < n && hyp[j] == hyp[i])
            j++;
        if (m >= out_cap)
            return RC_ERR_CAPACITY;
        out[m].start = start + (int64_t)i * epoch_sec;
        out[m].end = start + (int64_t)j * epoch_sec;
        out[m].stage = hyp[i];
        m++;
        i = j;
    }
    *n_out = m;
    return RC_OK;
}
