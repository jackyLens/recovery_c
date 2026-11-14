/* synth.c —— 见 synth.h。 */
#include "synth.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define EPOCH_SEC 30
#define WIN_SEC   300
#define PER_WIN   (WIN_SEC / EPOCH_SEC)

static const double LN_BY_STAGE[5] = { 3.30, 3.55, 3.80, 4.00, 3.50 };
static const double HR_BY_STAGE[5] = { 64.0, 59.0, 56.5, 54.5, 59.5 };

/* [0, 1) 均匀分布 */
static double uniform(unsigned *s)
{
    *s = *s * 1664525u + 1013904223u;
    return (double)(*s >> 8) / 16777216.0;
}

double synth_gauss(unsigned *s, double mean, double sd)
{
    double u1 = uniform(s), u2 = uniform(s);

    if (u1 < 1e-12)
        u1 = 1e-12;
    return mean + sd * sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}

void synth_opts_default(synth_opts *o, unsigned seed)
{
    memset(o, 0, sizeof *o);
    o->seed = seed;
    o->tst_min = 450.0;
    o->waso_bouts = 1;
    o->night_noise = 1;
}

/*
 * 生成 30 秒一格的分期：12 分钟入睡潜伏期，之后 90 分钟一个周期（N1 → N2 → N3 → REM，
 * 越往后 N3 越少、REM 越多），睡够 tst_min 截断，插入夜醒，末尾 5 分钟清醒。
 * 返回 malloc 的数组，*n 是格数。
 */
static int *make_hypnogram(const synth_opts *o, unsigned *rng, size_t *n)
{
    size_t target = (size_t)(o->tst_min * 2.0), cap = target + 24 + 10 + (size_t)o->waso_bouts * 18;
    int *hyp = (int *)malloc((cap + 1) * sizeof *hyp);
    size_t len = 0, slept = 0, k;
    int cycle = 0, b;

    if (!hyp)
        return NULL;
    for (k = 0; k < 24; k++)
        hyp[len++] = RC_WAKE;
    while (slept < target) {
        double f_n3 = 0.35 - 0.08 * cycle, f_rem = 0.12 + 0.07 * cycle;
        int stage[4] = { RC_N1, RC_N2, RC_N3, RC_REM };
        double frac[4];
        int p;

        if (f_n3 < 0.05)
            f_n3 = 0.05;
        if (f_rem > 0.40)
            f_rem = 0.40;
        frac[0] = 0.05;
        frac[1] = 1.0 - 0.05 - f_n3 - f_rem;
        frac[2] = f_n3;
        frac[3] = f_rem;
        for (p = 0; p < 4; p++) {
            size_t cnt = (size_t)(180.0 * frac[p] + 0.5);
            for (k = 0; k < cnt && slept < target; k++, slept++)
                hyp[len++] = stage[p];
        }
        cycle++;
    }
    for (b = 0; b < o->waso_bouts; b++) {
        size_t pos = (size_t)((0.3 + 0.5 * uniform(rng)) * (double)len);
        size_t dur = (size_t)(4 + (int)(uniform(rng) * 6.0)) * 2;
        memmove(hyp + pos + dur, hyp + pos, (len - pos) * sizeof *hyp);
        for (k = 0; k < dur; k++)
            hyp[pos + k] = RC_WAKE;
        len += dur;
    }
    for (k = 0; k < 10; k++)
        hyp[len++] = RC_WAKE;
    *n = len;
    return hyp;
}

int synth_make(rc_date d, const synth_opts *o, synth_night *out)
{
    unsigned rng = o->seed * 2654435761u + 12345u;
    int64_t start = ((int64_t)d - 1) * 86400 + 23 * 3600;
    size_t n_ep = 0, n_win, on = 0, off = 0, i, k, n_hrv = 0, n_st = 0, g0, g1;
    double day_ln = 0.0, day_hr = 0.0, day_resp = 0.0;
    int *hyp = make_hypnogram(o, &rng, &n_ep);

    memset(out, 0, sizeof *out);
    if (!hyp)
        return 0;
    n_win = (n_ep + PER_WIN - 1) / PER_WIN;
    out->hrv = (rc_hrv_sample *)malloc(n_win * sizeof *out->hrv);
    out->stages = (rc_stage_interval *)malloc(n_ep * sizeof *out->stages);
    if (!out->hrv || !out->stages) {
        free(hyp);
        synth_free(out);
        return 0;
    }

    for (i = 0; i < n_ep; i++)
        if (hyp[i] != RC_WAKE) {
            if (off == 0)
                on = i;
            off = i + 1;
        }
    if (o->night_noise) {
        day_ln = synth_gauss(&rng, 0.0, 0.07);
        day_hr = synth_gauss(&rng, 0.0, 1.0);
        day_resp = synth_gauss(&rng, 0.0, 0.25);
    }

    g0 = (size_t)((double)n_win * 0.35);
    g1 = g0 + (size_t)((double)n_win * o->wear_gap_frac);
    for (i = 0; i < n_win; i++) {
        size_t e0 = i * PER_WIN, e1 = e0 + PER_WIN < n_ep ? e0 + PER_WIN : n_ep;
        double ln = 0.0, hr = 0.0;
        rc_hrv_sample *s;

        if (i >= g0 && i < g1)
            continue;
        for (k = e0; k < e1; k++) {
            double pos = off > on ? ((double)k - (double)on) / (double)(off - on) : 0.0;
            if (pos < 0.0)
                pos = 0.0;
            if (pos > 1.0)
                pos = 1.0;
            ln += LN_BY_STAGE[hyp[k]] + 0.15 * pos + (pos >= 0.5 ? o->late_shift : o->early_shift);
            hr += HR_BY_STAGE[hyp[k]] - 3.0 * pos;
        }
        ln = ln / (double)(e1 - e0) + day_ln + o->ln_shift + synth_gauss(&rng, 0.0, 0.05);
        hr = hr / (double)(e1 - e0) + day_hr + o->hr_shift + synth_gauss(&rng, 0.0, 0.3);

        s = &out->hrv[n_hrv++];
        rc_hrv_sample_init(s, start + (int64_t)i * WIN_SEC, o->drop_hrv ? RC_NA : exp(ln));
        s->hr_bpm = o->drop_hr ? RC_NA : hr;
        s->resp_brpm = o->drop_resp ? RC_NA
                                    : 14.5 + day_resp + o->resp_shift + synth_gauss(&rng, 0.0, 0.3);
        s->quality = 0.85 + 0.15 * uniform(&rng);
        s->duration_sec = WIN_SEC;
    }

    if (rc_hypnogram_to_intervals(hyp, n_ep, start, EPOCH_SEC, out->stages, n_ep, &n_st) != RC_OK) {
        free(hyp);
        synth_free(out);
        return 0;
    }
    free(hyp);

    rc_night_input_init(&out->in, d);
    out->in.hrv = out->hrv;
    out->in.n_hrv = n_hrv;
    out->in.stages = out->stages;
    out->in.n_stages = n_st;
    out->in.has_in_bed_start = 1;
    out->in.in_bed_start = start;
    return 1;
}

void synth_free(synth_night *n)
{
    free(n->hrv);
    free(n->stages);
    n->hrv = NULL;
    n->stages = NULL;
}
