/* 数值小工具、日期换算、字符串解析、枚举名称。 */
#include "rc_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

double rc_clamp(double x, double lo, double hi)
{
    double m = (lo > x) ? lo : x;
    return (hi < m) ? hi : m;
}

double rc_ramp(double x, double a, double b)
{
    if (b == a)
        return x >= b ? 1.0 : 0.0;
    return rc_clamp((x - a) / (b - a), 0.0, 1.0);
}

int rc_round_half_up(double x)
{
    return (int)floor(x + 0.5);
}

double rc_round_dp(double x, int ndigits)
{
    char buf[64];

    if (!isfinite(x))
        return x;
    /* 评分里的数量级都在 1e3 以内，64 字节足够；真超了就原样返回 */
    if (snprintf(buf, sizeof buf, "%.*f", ndigits, x) >= (int)sizeof buf)
        return x;
    return strtod(buf, NULL);
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

double rc_median(double *v, size_t n)
{
    if (n == 0)
        return RC_NA;
    qsort(v, n, sizeof *v, cmp_double);
    if (n & 1)
        return v[n / 2];
    return (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

double rc_weighted_median(double *v, double *w, size_t n)
{
    size_t i, j, m = 0;
    double half, cum = 0.0;

    for (i = 0; i < n; i++) {
        if (w[i] > 0) {
            v[m] = v[i];
            w[m] = w[i];
            m++;
        }
    }
    if (m == 0)
        return RC_NA;

    /* 插入排序，稳定。片段数一晚一百来个，够用。 */
    for (i = 1; i < m; i++) {
        double vi = v[i], wi = w[i];
        for (j = i; j > 0 && v[j - 1] > vi; j--) {
            v[j] = v[j - 1];
            w[j] = w[j - 1];
        }
        v[j] = vi;
        w[j] = wi;
    }

    /* 先求总权重再从头累加，累加顺序与参考实现的 cumsum 相同 */
    for (i = 0; i < m; i++)
        cum += w[i];
    half = 0.5 * cum;
    cum = 0.0;
    for (i = 0; i < m; i++) {
        cum += w[i];
        if (cum >= half)
            return v[i];
    }
    return v[m - 1];
}

double rc_interp(double x, const double *xp, const double *fp, size_t n,
                 double left, double right)
{
    size_t j;
    double slope;

    if (x < xp[0])
        return left;
    if (x > xp[n - 1])
        return right;
    for (j = n - 1; j > 0 && x < xp[j]; j--)
        ;
    if (j == n - 1 || x == xp[j])
        return fp[j];
    slope = (fp[j + 1] - fp[j]) / (xp[j + 1] - xp[j]);
    return slope * (x - xp[j]) + fp[j];
}

double rc_pairwise_sum(const double *a, size_t n)
{
    if (n < 8) {
        size_t i;
        double res = 0.0;
        for (i = 0; i < n; i++)
            res += a[i];
        return res;
    }
    if (n <= 128) {
        size_t i, k;
        double r[8], res;
        for (k = 0; k < 8; k++)
            r[k] = a[k];
        for (i = 8; i < n - (n % 8); i += 8)
            for (k = 0; k < 8; k++)
                r[k] += a[i + k];
        res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
        for (; i < n; i++)
            res += a[i];
        return res;
    }
    {
        size_t n2 = n / 2;
        n2 -= n2 % 8;
        return rc_pairwise_sum(a, n2) + rc_pairwise_sum(a + n2, n - n2);
    }
}

double rc_slope(const double *y, size_t n)
{
    size_t i;
    double xm, ym = 0.0, sxy = 0.0, sxx = 0.0;

    if (n < 3)
        return 0.0;
    for (i = 0; i < n; i++)
        ym += y[i];
    ym /= (double)n;
    xm = (double)(n - 1) / 2.0;
    for (i = 0; i < n; i++) {
        double dx = (double)i - xm;
        sxy += dx * (y[i] - ym);
        sxx += dx * dx;
    }
    return sxy / sxx;
}

void rc_strcpy(char *dst, size_t cap, const char *src)
{
    size_t n;

    if (cap == 0)
        return;
    n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ---------------------------------------------------------------- 日期 */

/* 公历 ↔ 天数，Howard Hinnant 的 days_from_civil / civil_from_days 算法 */
rc_date rc_date_from_ymd(int year, int month, int day)
{
    int y = year - (month <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (rc_date)(era * 146097 + doe - 719468);
}

void rc_date_to_ymd(rc_date d, int *year, int *month, int *day)
{
    int z = (int)d + 719468;
    int era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = z - era * 146097;
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    int m = mp + (mp < 10 ? 3 : -9);

    if (year)
        *year = yoe + era * 400 + (m <= 2);
    if (month)
        *month = m;
    if (day)
        *day = doy - (153 * mp + 2) / 5 + 1;
}

/* ---------------------------------------------------------------- 初始化 */

void rc_hrv_sample_init(rc_hrv_sample *s, int64_t t_start, double rmssd)
{
    if (!s)
        return;
    s->t_start = t_start;
    s->rmssd = rmssd;
    s->hr_bpm = RC_NA;
    s->resp_brpm = RC_NA;
    s->quality = RC_NA;
    s->motion = RC_NA;
    s->duration_sec = 0;
}

void rc_night_input_init(rc_night_input *in, rc_date night_date)
{
    if (!in)
        return;
    memset(in, 0, sizeof *in);
    in->night_date = night_date;
    in->sleep_score_external = RC_NA;
}

void rc_night_metrics_init(rc_night_metrics *m, rc_date night_date)
{
    if (!m)
        return;
    memset(m, 0, sizeof *m);
    m->night_date = night_date;
    m->recovery_hrv = RC_NA;
    m->early_hrv = RC_NA;
    m->late_hrv = RC_NA;
    m->rhr = RC_NA;
    m->resp = RC_NA;
    m->midpoint_min = RC_NA;
    m->sleep_score_external = RC_NA;
    m->wear_coverage = 1.0;
    m->valid_night = 1;
    m->invalid_reason = RC_INVALID_NONE;
}

double rc_hrv_rebound(const rc_night_metrics *m)
{
    if (!m || rc_is_na(m->early_hrv) || rc_is_na(m->late_hrv))
        return RC_NA;
    return m->late_hrv - m->early_hrv;
}

/* ---------------------------------------------------------------- 字符串解析 */

/* 去首尾 ASCII 空白、ASCII 字母转小写后拷到 buf。放不下返回 0。 */
static int normalize_token(const char *text, char *buf, size_t cap)
{
    size_t n, i;

    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
        text++;
    n = strlen(text);
    while (n > 0 && (text[n - 1] == ' ' || text[n - 1] == '\t' ||
                     text[n - 1] == '\r' || text[n - 1] == '\n'))
        n--;
    if (n >= cap)
        return 0;
    for (i = 0; i < n; i++) {
        char ch = text[i];
        buf[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
    }
    buf[n] = '\0';
    return 1;
}

int rc_parse_stage(const char *text, int *stage)
{
    static const struct { const char *name; int code; } alias[] = {
        { "wake", RC_WAKE }, { "awake", RC_WAKE }, { "w", RC_WAKE }, { "清醒", RC_WAKE },
        { "n1", RC_N1 },
        { "n2", RC_N2 }, { "light", RC_N2 }, { "core", RC_N2 }, { "浅睡", RC_N2 },
        { "n3", RC_N3 }, { "deep", RC_N3 }, { "sws", RC_N3 }, { "深睡", RC_N3 },
        { "rem", RC_REM }, { "快速眼动", RC_REM },
    };
    char buf[32];
    size_t i;

    if (!text || !stage)
        return RC_ERR_ARG;
    if (!normalize_token(text, buf, sizeof buf))
        return RC_ERR_STAGE;
    for (i = 0; i < sizeof alias / sizeof alias[0]; i++) {
        if (strcmp(buf, alias[i].name) == 0) {
            *stage = alias[i].code;
            return RC_OK;
        }
    }
    return RC_ERR_STAGE;
}

int rc_parse_symptoms(const char *text, int *symptoms)
{
    char buf[32];

    if (!text || !symptoms || !normalize_token(text, buf, sizeof buf))
        return RC_ERR_ARG;
    if (strcmp(buf, "none") == 0)
        *symptoms = RC_SYMPTOMS_NONE;
    else if (strcmp(buf, "mild") == 0)
        *symptoms = RC_SYMPTOMS_MILD;
    else if (strcmp(buf, "moderate") == 0 || strcmp(buf, "severe") == 0 ||
             strcmp(buf, "moderate_severe") == 0)
        *symptoms = RC_SYMPTOMS_MODERATE_SEVERE;
    else
        return RC_ERR_ARG;
    return RC_OK;
}

/* ---------------------------------------------------------------- 名称 */

static const char *pick(const char *const *names, int count, int v)
{
    return (v >= 0 && v < count) ? names[v] : "?";
}

#define RC_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

const char *rc_state_name(int v)
{
    static const char *const n[] = { "scored", "calibrating", "insufficient_data",
                                     "sleep_too_short", "no_core_signal" };
    return pick(n, RC_COUNT(n), v);
}

const char *rc_color_name(int v)
{
    static const char *const n[] = { "", "green", "yellow", "red" };
    return pick(n, RC_COUNT(n), v);
}

const char *rc_tier_name(int v)
{
    static const char *const n[] = { "", "peak", "green", "high_yellow", "low_yellow", "red" };
    return pick(n, RC_COUNT(n), v);
}

const char *rc_confidence_name(int v)
{
    static const char *const n[] = { "", "high", "medium", "low" };
    return pick(n, RC_COUNT(n), v);
}

const char *rc_baseline_stage_name(int v)
{
    static const char *const n[] = { "calibrating", "estimating", "trial", "provisional",
                                     "formal" };
    return pick(n, RC_COUNT(n), v);
}

const char *rc_illness_level_name(int v)
{
    static const char *const n[] = { "none", "mild", "moderate", "high" };
    return pick(n, RC_COUNT(n), v);
}

const char *rc_invalid_reason_name(int v)
{
    static const char *const n[] = { "", "sleep_too_short", "low_wear_coverage" };
    return pick(n, RC_COUNT(n), v);
}

const char *rc_symptoms_name(int v)
{
    static const char *const n[] = { "none", "mild", "moderate_severe" };
    return pick(n, RC_COUNT(n), v);
}

const char *rc_gate_code_name(int v)
{
    static const char *const n[RC_GATE_COUNT] = {
        "R_HRV_COMBO", "R_LATE_COMBO", "R_RESP_COMBO", "R_MULTI_SEVERE", "R_SLEEP",
        "R_SYMPTOM", "R_ILLNESS",
        "Y_HRV", "Y_LATE", "Y_RHR", "Y_RESP", "Y_SLEEP", "Y_TST", "Y_SYMPTOM",
        "Y_LOW_CONF", "Y_ILLNESS",
    };
    return pick(n, RC_GATE_COUNT, v);
}

const char *rc_stage_name(int v)
{
    static const char *const n[] = { "Wake", "N1", "N2", "N3/SWS", "REM" };
    return pick(n, RC_COUNT(n), v);
}

const char *rc_factor_name(const rc_factor *f)
{
    static const char *const n[] = { "", "hrv", "late", "rhr", "sleep", "resp", "trend" };

    if (!f)
        return "?";
    if (f->key == RC_FACTOR_GATE)
        return rc_gate_code_name(f->gate);
    return pick(n, RC_COUNT(n), f->key);
}

const char *rc_strerror(int status)
{
    switch (status) {
    case RC_OK:           return "成功";
    case RC_ERR_ARG:      return "参数不合法";
    case RC_ERR_STAGE:    return "未知的睡眠分期";
    case RC_ERR_NOMEM:    return "内存不足";
    case RC_ERR_CAPACITY: return "输出缓冲区容量不够";
    default:              return "未知错误";
    }
}

const char *rc_version(void)
{
    return RC_VERSION;
}
