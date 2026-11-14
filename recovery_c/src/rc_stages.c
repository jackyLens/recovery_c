/* 带时间的睡眠分期：规整、主睡眠边界、睡眠结构、与 5 分钟窗口的重叠（3.3、4.2、4.6）。 */
#include "rc_internal.h"

int rc_normalize_stages(const rc_stage_interval *in, size_t n, rc_iv *out, size_t *n_out)
{
    size_t i, j, m = 0;

    /* 先整体拷贝并按 start 稳定排序。同一 start 的多条里靠后的会把靠前的截成零长，
       所以排序必须稳定。上游数据基本有序，插入排序接近线性。 */
    for (i = 0; i < n; i++) {
        rc_iv cur;

        if (in[i].stage < RC_WAKE || in[i].stage > RC_REM)
            return RC_ERR_STAGE;
        cur.start = in[i].start;
        cur.end = in[i].end;
        cur.stage = in[i].stage;
        for (j = i; j > 0 && out[j - 1].start > cur.start; j--)
            out[j] = out[j - 1];
        out[j] = cur;
    }

    /* 原地压缩：out[0..m) 是已确定的结果，i 是读指针，m ≤ i 恒成立 */
    for (i = 0; i < n; i++) {
        rc_iv cur = out[i];

        if (cur.end <= cur.start)
            continue;
        if (m > 0 && cur.start < out[m - 1].end) {
            out[m - 1].end = cur.start;
            if (out[m - 1].end <= out[m - 1].start)
                m--;
        }
        out[m++] = cur;
    }
    *n_out = m;
    return RC_OK;
}

int rc_sleep_bounds(const rc_iv *iv, size_t n, int64_t *onset, int64_t *offset)
{
    size_t i;
    int found = 0;

    for (i = 0; i < n; i++) {
        if (iv[i].stage == RC_WAKE)
            continue;
        if (!found) {
            *onset = iv[i].start;
            *offset = iv[i].end;
            found = 1;
        } else if (iv[i].end > *offset) {
            *offset = iv[i].end;
        }
    }
    return found;
}

static double minutes(int64_t a, int64_t b)
{
    return (double)(b - a) / 60.0;
}

static int64_t floor_mod(int64_t a, int64_t m)
{
    int64_t r = a % m;
    return r < 0 ? r + m : r;
}

void rc_extract_sleep_metrics(const rc_iv *iv, size_t n, int has_in_bed, int64_t in_bed_start,
                              int32_t utc_offset_sec, rc_night_metrics *m)
{
    int64_t onset = 0, offset = 0, rec_start, mid;
    int64_t run_start = 0, run_end = 0;
    int have_run = 0, long_aw = 0;
    double tst = 0.0, waso = 0.0, denom, eff;
    size_t i;

    m->tst_min = 0.0;
    m->efficiency = 0.0;
    m->waso_min = 0.0;
    m->long_awakenings = 0;
    m->midpoint_min = RC_NA;
    if (!rc_sleep_bounds(iv, n, &onset, &offset))
        return;

    /* 记录开始：上床时刻与第一段分期开始中较早的 */
    rec_start = iv[0].start;
    if (has_in_bed && in_bed_start < rec_start)
        rec_start = in_bed_start;

    for (i = 0; i < n; i++)
        if (iv[i].stage != RC_WAKE)
            tst += minutes(iv[i].start, iv[i].end);

    /* 主睡眠期内的清醒。首尾相接的清醒区间并成一次夜醒，≥5 分钟才算长觉醒。 */
    for (i = 0; i < n; i++) {
        int64_t s = iv[i].start, e = iv[i].end;

        if (e <= onset || s >= offset || iv[i].stage != RC_WAKE)
            continue;
        if (s < onset)
            s = onset;
        if (e > offset)
            e = offset;
        waso += minutes(s, e);
        if (have_run && s <= run_end) {
            if (e > run_end)
                run_end = e;
        } else {
            if (have_run && minutes(run_start, run_end) >= 5)
                long_aw++;
            run_start = s;
            run_end = e;
            have_run = 1;
        }
    }
    if (have_run && minutes(run_start, run_end) >= 5)
        long_aw++;

    denom = minutes(rec_start, offset);
    eff = denom > 0 ? tst / denom : 0.0;

    /* 睡眠中点换成本地时间，取"时:分"，再以前一日 12:00 为零点 */
    mid = onset + (offset - onset) / 2 + utc_offset_sec;
    mid = floor_mod(mid, 86400) / 60;

    m->tst_min = tst;
    m->efficiency = eff < 1.0 ? eff : 1.0;
    m->waso_min = waso;
    m->long_awakenings = long_aw;
    m->midpoint_min = (double)floor_mod(mid - 720, 1440);
}

double rc_stage_overlap(const rc_iv *iv, size_t n, int64_t t0, int64_t t1, double sec[5])
{
    size_t i;
    double total, sum = 0.0;
    int k;

    for (k = 0; k < 5; k++)
        sec[k] = 0.0;
    for (i = 0; i < n; i++) {
        int64_t a, b;

        if (iv[i].end <= t0)
            continue;
        if (iv[i].start >= t1)
            break;
        a = iv[i].start > t0 ? iv[i].start : t0;
        b = iv[i].end < t1 ? iv[i].end : t1;
        sec[iv[i].stage] += (double)(b - a);
    }
    for (k = 0; k < 5; k++)
        sum += sec[k];
    total = (double)(t1 - t0);
    return total > 0 ? sum / total : 0.0;
}
