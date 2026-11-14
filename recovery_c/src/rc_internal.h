/*
 * rc_internal.h —— 库内部共用的声明。不安装，调用方不要包含。
 * 单元测试为了直接测子分、门控等函数会包含它。
 */
#ifndef RC_INTERNAL_H
#define RC_INTERNAL_H

#include "recovery.h"

/* 需要替换分配器时在编译选项里定义这三个宏 */
#ifndef RC_MALLOC
#include <stdlib.h>
#define RC_MALLOC(n)     malloc(n)
#define RC_REALLOC(p, n) realloc((p), (n))
#define RC_FREE(p)       free(p)
#endif

/* malloc(0) 的返回值各平台不一致，统一至少要 1 个元素 */
#define RC_ALLOC_N(type, n) ((type *)RC_MALLOC(sizeof(type) * ((n) != 0 ? (n) : 1)))

/* ---------------------------------------------------------------- rc_util.c */

/* min(max(x, lo), hi) */
double rc_clamp(double x, double lo, double hi);

/* x 从 a 到 b 线性映射到 0→1，两端截断。a == b 时退化为阶跃。 */
double rc_ramp(double x, double a, double b);

/* floor(x + 0.5)，最终分数取整用 */
int rc_round_half_up(double x);

/*
 * 保留 ndigits 位小数，结果与 Python 的 round(x, ndigits) 相同（按十进制精确值四舍六入五成双）。
 * 借 printf 做正确舍入，要求 C 库的 %.Nf 是正确舍入的（glibc、Apple libc、musl 都是）。
 */
double rc_round_dp(double x, int ndigits);

/* 中位数。会把 v 原地排序。n == 0 返回 RC_NA。偶数个取中间两个的均值。 */
double rc_median(double *v, size_t n);

/*
 * 加权中位数：按值升序累加权重，取累计权重首次 ≥ 总权重一半的那个值。
 * 权重 ≤ 0 的元素忽略。v、w 会被原地重排。没有正权重元素时返回 RC_NA。
 */
double rc_weighted_median(double *v, double *w, size_t n);

/*
 * 折线插值，xp 严格递增。x < xp[0] 返回 left，x > xp[n-1] 返回 right。
 */
double rc_interp(double x, const double *xp, const double *fp, size_t n,
                 double left, double right);

/* 成对求和，分块方式与 numpy 的 sum 相同，为了 RR 路径和 Python 逐位一致。 */
double rc_pairwise_sum(const double *a, size_t n);

/* 最小二乘直线斜率，x 取 0..n-1。n < 3 返回 0。 */
double rc_slope(const double *y, size_t n);

/* 有界字符串拷贝，保证以 0 结尾。 */
void rc_strcpy(char *dst, size_t cap, const char *src);

/* ---------------------------------------------------------------- rc_stages.c */

/* 规整后的分期区间 */
typedef struct {
    int64_t start, end;
    int     stage;
} rc_iv;

/*
 * 分期区间规整（3.3）：按开始时刻稳定排序，丢弃零长和倒序区间，
 * 重叠时后一段截断前一段。out 容量至少 n。
 * 返回 RC_OK 或 RC_ERR_STAGE（任何一条的编码不在 0–4，包括会被丢弃的零长区间）。
 */
int rc_normalize_stages(const rc_stage_interval *in, size_t n, rc_iv *out, size_t *n_out);

/* 主睡眠期边界：第一个非 Wake 区间的开始、所有非 Wake 区间的最晚结束。没有睡眠返回 0。 */
int rc_sleep_bounds(const rc_iv *iv, size_t n, int64_t *onset, int64_t *offset);

/* 睡眠结构（4.6）：写 m 的 tst_min / efficiency / waso_min / long_awakenings / midpoint_min。 */
void rc_extract_sleep_metrics(const rc_iv *iv, size_t n, int has_in_bed, int64_t in_bed_start,
                              int32_t utc_offset_sec, rc_night_metrics *m);

/* 窗口 [t0, t1) 与各阶段的重叠秒数 sec[5]，返回被分期覆盖的比例。 */
double rc_stage_overlap(const rc_iv *iv, size_t n, int64_t t0, int64_t t1, double sec[5]);

/* ---------------------------------------------------------------- rc_segments.c */

/*
 * 5 分钟记录 → 片段（4.1–4.3）。
 *   samples  in->hrv 的可写拷贝，n = in->n_hrv；函数内排序去重
 *   segs     容量至少 in->n_hrv
 *   work     容量至少 in->n_hrv + 2 * ln_outlier_neighbors 个 double
 *   flags    容量至少 in->n_hrv
 * 没有睡眠时 *n_segs = 0。
 */
void rc_segment_night(const rc_night_input *in, rc_hrv_sample *samples,
                      const rc_iv *iv, size_t n_iv, const rc_config *cfg,
                      rc_segment *segs, double *work, unsigned char *flags, size_t *n_segs);

/* 佩戴覆盖率（4.6）：主睡眠期内被合格 HRV 窗口覆盖的时间比例。 */
double rc_wear_coverage(const rc_segment *segs, size_t n, double onset_s, double offset_s,
                        double q_min);

/* ---------------------------------------------------------------- rc_sleep.c */

/* today 之前 days 个日历日内的累计睡眠债，每晚封顶 debt_night_cap。缺失的夜不计。 */
double rc_sleep_debt(const rc_day_record *history, size_t n, rc_date today, double need,
                     const rc_config *cfg, int days);

/* 睡眠子分（7.4）。midpoint_median 为 RC_NA 时规律项取 reg_unknown。detail 可以为 NULL。 */
double rc_sleep_score(const rc_night_metrics *night, double need, double debt_prev,
                      double midpoint_median, const rc_config *cfg, rc_sleep_detail *detail);

/* ---------------------------------------------------------------- rc_baseline.c */

/*
 * 历史记录的下标按日期稳定升序排列，写入 order（容量 n）。
 * 历史本来就有序时是 O(n)。
 */
void rc_order_by_date(const rc_day_record *history, size_t n, size_t *order);

/* rc_build_baseline 的实现；order 是 rc_order_by_date 的结果。 */
int rc_baseline_from_order(const rc_day_record *history, const size_t *order, size_t n,
                           rc_date today, const rc_config *cfg, rc_baseline *out);

/* (value − median) / sigma。指标没有基线或 value 缺失返回 RC_NA。 */
double rc_baseline_z(const rc_baseline *bl, int metric, double value);

/* ---------------------------------------------------------------- rc_scoring.c */

double rc_hrv_subscore(double z_hrv, const rc_config *cfg);
double rc_late_adjustment(double z_early, double z_late, double z_rhr, double r_resp,
                          const rc_config *cfg);
double rc_rhr_penalty(double z_rhr, const rc_config *cfg);
double rc_resp_severity(double z_resp, double delta_resp, const rc_config *cfg);
double rc_resp_penalty(double r, const rc_config *cfg);

/*
 * 趋势修正（8.2）。reasons 至少 4 条容量，*n_reasons 返回写入的条数。
 * z_hrv / z_rhr / r_today 是今天未舍入的值，历史用记录里保存的值。
 * work 是临时区，容量 3 * (n + 1) 个 double。
 */
double rc_trend_adjustment(const rc_day_record *history, const size_t *order, size_t n,
                           rc_date today, double z_hrv, double z_rhr, double r_today,
                           double deficit_today, double need, const rc_config *cfg,
                           double *work, char reasons[][RC_TEXT_MAX], int *n_reasons);

/* 不适风险点数（10）。返回等级，*points 含症状，*physio 不含。 */
int rc_illness_points(const rc_zscores *z, double rhr_delta, double s_sleep, int symptoms,
                      const rc_config *cfg, int *points, int *physio);

/* 门控（9）。z->resp_r 是 RESP 严重度。gates 容量 RC_MAX_GATES，返回触发的条数。 */
int rc_evaluate_gates(const rc_zscores *z, double s_sleep, double tst, int symptoms,
                      int confidence, int illness_level, const rc_config *cfg, rc_gate *gates);

/* 置信度（6.2）。reasons 至少 4 条容量。 */
int rc_confidence_level(const rc_night_metrics *night, const rc_baseline *bl,
                        const rc_config *cfg, char reasons[][RC_TEXT_MAX], int *n_reasons);

/* 分数 → 颜色与分档（1.3）。 */
void rc_tier_of(int score, const rc_config *cfg, int *color, int *tier);

/* ---------------------------------------------------------------- rc_explain.c */

const char *rc_tier_label(int tier);
const char *rc_tier_advice(int tier);
const char *rc_illness_text(int level);     /* none 返回 "" */
const char *rc_state_text(int state);       /* scored 返回 "" */

/* 挑出最多 RC_MAX_FACTORS 个影响因素写入 res->top_factors（11.1）。用的是 res 里已舍入的值。 */
void rc_top_factors(rc_result *res);

/* 生成 res->summary：分档标签 + 前两个因素 + 建议。 */
void rc_summary_text(rc_result *res);

#endif /* RC_INTERNAL_H */
