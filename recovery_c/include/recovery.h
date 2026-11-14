/*
 * recovery.h —— Recovery 恢复分 V1.0.2，C99 实现。
 *
 * 输入：一晚的 5 分钟 HRV 记录 + 带起止时刻的睡眠分期。
 * 输出：当晚指标（rc_night_metrics）和 0–100 的恢复分（rc_result）。
 * 行为与 Python 参考实现 recovery 1.0.2 一致，章节号指《Recovery 功能设计方案 V1.0 修订版》。
 *
 * 约定
 *   时间    int64 Unix 秒。睡眠中点要用本地时间，时区偏移放在 rc_night_input.utc_offset_sec。
 *           如果传进来的已经是本地墙上时间的秒数，偏移填 0。
 *   日期    rc_date，自 1970-01-01 起的天数，表示"醒来当天"的本地日历日。
 *   缺失值  double 字段缺失一律用 RC_NA（NaN），用 rc_is_na() 判断。
 *           因此不要用 -ffast-math / -ffinite-math-only 编译本库和调用方。
 *   字符串  UTF-8。结果里的文案都是定长 char 数组，结构体里没有指针，可以整体拷贝或落盘。
 *   内存    除 rc_engine 外，所有结构体由调用方分配。rc_process_night / rc_build_baseline /
 *           rc_score_day 内部会临时 malloc，返回前释放。rc_engine 由库分配，
 *           rc_engine_destroy 释放。
 *   线程    没有全局可变状态。纯函数可以并发调用；同一个 rc_engine 不能并发访问，
 *           不同的 rc_engine 互不影响。
 *   返回值  返回 int 的函数：RC_OK(0) 成功，负数是 rc_status 错误码。
 */
#ifndef RECOVERY_H
#define RECOVERY_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RC_VERSION "1.0.2"

#define RC_NA        (NAN)
#define rc_is_na(x)  (isnan(x))

/* 结果结构体里各数组的容量 */
#define RC_MAX_GATES    16   /* 门控规则总数，不会溢出 */
#define RC_MAX_REASONS   8   /* 置信度原因最多 4 条 + 趋势原因最多 4 条 */
#define RC_MAX_FACTORS   3
#define RC_TEXT_MAX    128   /* 单条短文案（含结尾 0） */
#define RC_ADVICE_MAX  160
#define RC_MESSAGE_MAX 256
#define RC_SUMMARY_MAX 512

typedef int32_t rc_date;

/* ------------------------------------------------------------------ 枚举 */

typedef enum {
    RC_OK           =  0,
    RC_ERR_ARG      = -1,   /* 空指针或参数不合法 */
    RC_ERR_STAGE    = -2,   /* 睡眠分期编码不在 0–4，或字符串别名不认识 */
    RC_ERR_NOMEM    = -3,   /* malloc 失败 */
    RC_ERR_CAPACITY = -4    /* 调用方给的输出缓冲区不够 */
} rc_status;

/* 睡眠分期（3.3） */
typedef enum { RC_WAKE = 0, RC_N1 = 1, RC_N2 = 2, RC_N3 = 3, RC_REM = 4 } rc_stage;

/* 用户主动上报的不适程度（3.1）。moderate 和 severe 在算法里不区分。 */
typedef enum {
    RC_SYMPTOMS_NONE = 0,
    RC_SYMPTOMS_MILD = 1,
    RC_SYMPTOMS_MODERATE_SEVERE = 2
} rc_symptoms;

/* 当晚无效的原因（6.1） */
typedef enum {
    RC_INVALID_NONE = 0,
    RC_INVALID_SLEEP_TOO_SHORT = 1,     /* TST < min_tst_min */
    RC_INVALID_LOW_WEAR_COVERAGE = 2    /* 佩戴覆盖率 < min_wear_coverage */
} rc_invalid_reason;

/* 结果状态（11.3）。只有 RC_STATE_SCORED 才有分数。 */
typedef enum {
    RC_STATE_SCORED = 0,
    RC_STATE_CALIBRATING = 1,           /* 基线不足 stage_estimating 晚 */
    RC_STATE_INSUFFICIENT_DATA = 2,     /* 佩戴覆盖率不够 */
    RC_STATE_SLEEP_TOO_SHORT = 3,
    RC_STATE_NO_CORE_SIGNAL = 4         /* HRV 和 RHR 的 z 都算不出来 */
} rc_state;

typedef enum { RC_COLOR_NONE = 0, RC_COLOR_GREEN, RC_COLOR_YELLOW, RC_COLOR_RED } rc_color;

/* 分档（1.3）：85+ / 67–84 / 50–66 / 34–49 / 0–33 */
typedef enum {
    RC_TIER_NONE = 0,
    RC_TIER_PEAK,
    RC_TIER_GREEN,
    RC_TIER_HIGH_YELLOW,
    RC_TIER_LOW_YELLOW,
    RC_TIER_RED
} rc_tier;

typedef enum { RC_CONF_NONE = 0, RC_CONF_HIGH, RC_CONF_MEDIUM, RC_CONF_LOW } rc_confidence;

/* 基线阶段（5.3），按可用夜数划分：<3 / 3–6 / 7–13 / 14–29 / ≥30 */
typedef enum {
    RC_BL_CALIBRATING = 0,
    RC_BL_ESTIMATING,
    RC_BL_TRIAL,
    RC_BL_PROVISIONAL,
    RC_BL_FORMAL
} rc_baseline_stage;

/* 不适风险等级（10） */
typedef enum { RC_ILL_NONE = 0, RC_ILL_MILD, RC_ILL_MODERATE, RC_ILL_HIGH } rc_illness_level;

/* 有个人基线的指标，同时是 rc_config.sigma_* 和 rc_baseline.metrics 的下标 */
typedef enum {
    RC_METRIC_RECOVERY_HRV = 0,   /* lnRMSSD */
    RC_METRIC_LATE_HRV,
    RC_METRIC_EARLY_HRV,
    RC_METRIC_RHR,                /* bpm */
    RC_METRIC_RESP,               /* 次/分 */
    RC_METRIC_SLEEP_SCORE,        /* 0–100 */
    RC_METRIC_COUNT
} rc_metric;

/* 门控规则（9）。枚举顺序就是判定和输出顺序：先 Red 后 Yellow。 */
typedef enum {
    RC_GATE_R_HRV_COMBO = 0,
    RC_GATE_R_LATE_COMBO,
    RC_GATE_R_RESP_COMBO,
    RC_GATE_R_MULTI_SEVERE,
    RC_GATE_R_SLEEP,
    RC_GATE_R_SYMPTOM,
    RC_GATE_R_ILLNESS,
    RC_GATE_Y_HRV,
    RC_GATE_Y_LATE,
    RC_GATE_Y_RHR,
    RC_GATE_Y_RESP,
    RC_GATE_Y_SLEEP,
    RC_GATE_Y_TST,
    RC_GATE_Y_SYMPTOM,
    RC_GATE_Y_LOW_CONF,
    RC_GATE_Y_ILLNESS,
    RC_GATE_COUNT
} rc_gate_code;

typedef enum { RC_GATE_YELLOW = 0, RC_GATE_RED = 1 } rc_gate_level;

/* 影响因素的来源（11.1） */
typedef enum {
    RC_FACTOR_GATE = 0,   /* 来自触发的门控，gate 字段有效，impact 为 RC_NA */
    RC_FACTOR_HRV,
    RC_FACTOR_LATE,
    RC_FACTOR_RHR,
    RC_FACTOR_SLEEP,
    RC_FACTOR_RESP,
    RC_FACTOR_TREND
} rc_factor_key;

/* ------------------------------------------------------------------ 配置 */

#define RC_RESP_TABLE_N 5

typedef struct { double hrv, rhr, sleep, resp; } rc_weights;
typedef struct { double duration, efficiency, regularity, continuity, debt; } rc_sleep_weights;

/*
 * 全部阈值。字段名、默认值与 Python 的 config.py 一一对应，用 rc_config_default() 取默认值后再改。
 * 库里不保存配置指针以外的任何状态；rc_engine 在创建时拷贝一份。
 */
typedef struct {
    /* ---- 5 分钟 HRV 记录校验（4.1、4.2） ---- */
    int    segment_sec;            /* 记录未给 duration_sec 时的窗口长度，秒。默认 300 */
    double rmssd_min_ms;           /* RMSSD 合法下限，ms。默认 3 */
    double rmssd_max_ms;           /* RMSSD 合法上限，ms。默认 250 */
    double hr_min_bpm;             /* 窗口心率合法下限，超出只丢心率。默认 30 */
    double hr_max_bpm;             /* 默认 130 */
    double ln_outlier;             /* |ln − 邻近中位数| 超过此值判离群。默认 0.7 */
    int    ln_outlier_neighbors;   /* 离群检测取前后各几个有效窗。默认 3 */
    double min_stage_label_frac;   /* 窗口被分期覆盖的比例低于此值按 Wake。默认 0.5 */
    double wake_frac_max;          /* 窗口内 Wake 占比超过此值按 Wake。默认 0.2 */
    double q_good_segment;         /* 质量权重 ≥ 此值算"好片段"。默认 0.5 */
    double motion_max;             /* 体动超过此值质量置 0。默认 0.5 */

    /* ---- 只有 rc_rr_to_hrv5min 用（逐搏 RR 兼容路径） ---- */
    double rr_min_ms;              /* RR 合法范围，ms。默认 300–2000 */
    double rr_max_ms;
    int    rr_local_window;        /* 局部中位数窗口，拍数，取奇数。默认 11 */
    double rr_jump_ratio;          /* 偏离局部中位数的比例上限。默认 0.20 */
    int    seg_min_beats;          /* 窗口内拍数少于此值质量置 0。默认 150 */
    int    min_rmssd_pairs;        /* 算 RMSSD 至少需要的相邻有效 RR 对数。默认 100 */
    double q_cov_good;             /* 有效 RR 覆盖率 ≥ 此值质量满分。默认 0.90 */
    double q_cov_min;              /* 覆盖率 ≤ 此值质量为 0。默认 0.60 */
    double q_art_good;             /* 伪迹比例 ≤ 此值不扣质量。默认 0.05 */
    double q_art_max;              /* 伪迹比例 ≥ 此值质量为 0。默认 0.20 */

    /* ---- 阶段权重（4.3） ---- */
    double stable_frac;            /* 主导阶段占比 ≥ 此值且无 Wake 记为稳定。默认 0.8 */
    double w_n3_stable, w_n3_mixed;   /* 1.0 / 0.8 */
    double w_n2_stable, w_n2_mixed;   /* 0.75 / 0.5 */
    double w_n1;                      /* 0.3 */
    double w_rem;                     /* 0.2 */
    double w_wake;                    /* 0 */

    /* ---- 时间位置权重（4.3） ---- */
    double onset_min;              /* 入睡后这段时间内用 tw_onset，分钟。默认 45 */
    double prewake_min;            /* 醒前这段时间内用 tw_prewake，分钟。默认 20 */
    double tw_onset;               /* 0.6 */
    double tw_prewake;             /* 0 */
    double tw_first;               /* 前半夜，0.95 */
    double tw_mid;                 /* 中段，1.0 */
    double tw_late;                /* 后半夜，1.0 */
    double tw_late_bonus;          /* 后半夜稳定 NREM 加权，1.10 */
    double mid_band[2];            /* 中段的相对位置范围，默认 {0.4, 0.6} */

    /* ---- 生理稳定性权重（4.3） ---- */
    double stab_hr_mild;           /* 心率偏离邻近中位数超过此值乘 stab_w_mild，bpm。默认 6 */
    double stab_hr_severe;         /* 默认 10 */
    double stab_w_mild;            /* 0.6 */
    double stab_w_severe;          /* 0.3 */
    double stab_resp_dev;          /* RESP 偏离整晚中位数超过此值再乘 stab_w_resp。默认 3 */
    double stab_w_resp;            /* 0.6 */

    /* ---- 有效性与置信度（6） ---- */
    double min_tst_min;            /* TST 低于此值不出分，分钟。默认 180 */
    double min_wear_coverage;      /* 佩戴覆盖率低于此值不出分。默认 0.70 */
    int    min_good_segments;      /* Recovery HRV 至少需要的好片段数。默认 6 */
    int    min_nrem_segments;      /* 其中 N2/N3 至少几个。默认 3 */
    int    min_early_late_segments;/* Early / Late HRV 各自至少几个片段。默认 3 */
    int    min_resp_segments;      /* RESP 至少需要的片段数。默认 36（约 3 小时） */
    int    rhr_lowest_segments;    /* RHR 取心率最低的几个片段求均值。默认 6 */
    int    conf_high_segments;     /* 高置信度要求的好片段数。默认 12 */
    double conf_high_wear;         /* 高置信度要求的佩戴覆盖率。默认 0.85 */
    double conf_low_tst;           /* TST 低于此值置信度为 low，分钟。默认 240 */

    /* ---- 基线（5） ---- */
    int    baseline_window;        /* 最多取最近多少个合格夜。默认 30 */
    int    baseline_lookback_days; /* 只看今天之前多少天内的记录。默认 60 */
    int    baseline_min_values;    /* 某指标至少有几个值才建基线。默认 3 */
    double baseline_exclude_tst_min; /* TST 低于此值的夜不进基线，分钟。默认 210 */
    double prior_k;                /* sigma 收缩里先验的等效样本数。默认 5 */
    double sigma_floor[RC_METRIC_COUNT];  /* sigma 下限，下标 rc_metric */
    double sigma_prior[RC_METRIC_COUNT];  /* 人群典型的个体内波动，下标 rc_metric */
    double outlier_z;              /* 核心指标 |z| 超过此值标记为基线离群。默认 3 */
    int    new_normal_run;         /* 末尾连续多少晚离群且无症状则重新纳入基线。默认 7 */
    int    stage_estimating;       /* 基线阶段的夜数分界：3 / 7 / 14 / 30 */
    int    stage_trial;
    int    stage_provisional;
    int    stage_formal;

    /* ---- 睡眠子分（7.4），时长单位都是分钟 ---- */
    double sleep_need_default;     /* 基线不足时的睡眠需求。默认 450 */
    double sleep_need_min;         /* 个人睡眠需求的下限 / 上限。默认 390 / 540 */
    double sleep_need_max;
    int    debt_days;              /* 睡眠债回看天数。默认 7 */
    double debt_night_cap;         /* 单晚记入睡眠债的上限。默认 180 */
    double debt_need_factor;       /* 睡眠债折算进当晚需求的比例。默认 0.25 */
    double debt_need_cap;          /* 折算上限。默认 60 */
    double debt_zero_score;        /* 睡眠债达到此值债务项得 0 分。默认 360 */
    double duration_zero_ratio;    /* TST / 需求 低于此值时长项得 0 分。默认 0.5 */
    double se_full;                /* 睡眠效率 ≥ 此值满分。默认 0.90 */
    double se_zero;                /* 睡眠效率 ≤ 此值 0 分。默认 0.70 */
    double reg_zero_min;           /* 睡眠中点偏离中位数达到此值规律项 0 分。默认 120 */
    double reg_unknown;            /* 没有中点基线时规律项的取值。默认 85 */
    double waso_full;              /* WASO ≤ 此值连续性满分。默认 20 */
    double waso_zero;              /* WASO ≥ 此值连续性 0 分。默认 90 */
    double long_awake_penalty;     /* 第 2 次起每次长觉醒扣的分。默认 10 */
    rc_sleep_weights sleep_weights;/* 0.45 / 0.20 / 0.15 / 0.10 / 0.10 */

    /* ---- 子分与总分（7、8.1） ---- */
    rc_weights weights;            /* HRV 0.45 / RHR 0.20 / 睡眠 0.25 / RESP 0.10 */
    double hrv_base;               /* z=0 时的 HRV 子分。默认 70 */
    double hrv_slope;              /* 每 1 个 z 的分值。默认 18 */
    double hrv_max;                /* HRV 子分上限。默认 95 */
    double late_neg_start;         /* 后半夜 z 低于此值开始扣分。默认 −0.5 */
    double late_neg_slope;         /* 默认 6 */
    double late_neg_max;           /* 扣分上限。默认 6 */
    double rebound_early_max;      /* 回升加分要求前半夜 z ≤ 此值。默认 −0.5 */
    double rebound_dz_min;         /* 且后半夜比前半夜高出至少这么多。默认 0.5 */
    double rebound_max;            /* 加分上限。默认 3 */
    double rebound_rhr_max;        /* 且 RHR z ≤ 此值。默认 0.5 */
    double rebound_r_max;          /* 且 RESP 严重度 ≤ 此值。默认 1.0 */
    double rhr_p_start;            /* RHR z 超过此值开始扣分。默认 0.5 */
    double rhr_p_slope1;           /* 第一段斜率。默认 8 */
    double rhr_p_knee;             /* 拐点。默认 1.5 */
    double rhr_p_slope2;           /* 第二段斜率。默认 14 */
    double rhr_p_max;              /* 扣分上限。默认 35 */
    double resp_delta_xp[RC_RESP_TABLE_N];  /* RESP 绝对升幅（次/分）→ 等效 z 的折线 */
    double resp_delta_fp[RC_RESP_TABLE_N];
    double resp_pen_xp[RC_RESP_TABLE_N];    /* RESP 严重度 → 扣分的折线 */
    double resp_pen_fp[RC_RESP_TABLE_N];

    /* ---- 趋势（8.2） ---- */
    double trend_min;              /* 趋势修正下限。默认 −8 */
    double trend_max;              /* 上限。默认 +5 */

    /* ---- 门控（9） ---- */
    double cap_yellow;             /* Yellow 门控的封顶分。默认 66 */
    double cap_red;                /* Red 门控的封顶分。默认 33 */
    double y_hrv;                  /* HRV z 低于此值触发 Y_HRV。默认 −0.75 */
    double y_late;                 /* 默认 −1.0 */
    double y_rhr;                  /* RHR z 高于此值触发 Y_RHR。默认 1.0 */
    double y_resp_r;               /* RESP 严重度高于此值触发 Y_RESP。默认 2.0 */
    double y_sleep;                /* 睡眠子分低于此值触发 Y_SLEEP。默认 60 */
    double y_tst;                  /* TST 低于此值触发 Y_TST，分钟。默认 300 */
    double combo_resp_r;           /* 组合规则里"RESP 升高"的门槛。默认 1.5 */
    double sev_hrv;                /* 以下四项是"严重偏离"的门槛：−2 / 2 / 3 / 35 */
    double sev_rhr;
    double sev_resp_r;
    double sev_sleep;

    /* ---- 不适风险（10），累计点数的分界 ---- */
    int ill_mild;                  /* 默认 2 */
    int ill_moderate;              /* 默认 4 */
    int ill_high;                  /* 默认 6 */

    /* ---- 分档（1.3） ---- */
    int green_min;                 /* 默认 67 */
    int yellow_min;                /* 默认 34 */
    int tier_peak;                 /* 默认 85 */
    int tier_high_yellow;          /* 默认 50 */
} rc_config;

/* 填入默认值。cfg 为 NULL 时什么都不做。 */
void rc_config_default(rc_config *cfg);

/* ------------------------------------------------------------------ 输入 */

/* 一条 5 分钟 HRV 记录（3.2）。可选字段没有就填 RC_NA。 */
typedef struct {
    int64_t t_start;       /* 窗口开始时刻，Unix 秒 */
    double  rmssd;         /* RMSSD，ms；rc_night_input.hrv_is_ln 非 0 时是 lnRMSSD。RC_NA = 该窗无值 */
    double  hr_bpm;        /* 窗口平均心率，bpm。可选 */
    double  resp_brpm;     /* 窗口呼吸率，次/分。可选 */
    double  quality;       /* 信号质量 0–1，超出会被截断。可选，缺省按 1 */
    double  motion;        /* 体动强度 0–1。可选 */
    int32_t duration_sec;  /* 窗口长度，秒。≤0 时用 cfg.segment_sec */
} rc_hrv_sample;

/* 一段睡眠分期，左闭右开 [start, end) */
typedef struct {
    int64_t start;         /* Unix 秒 */
    int64_t end;           /* Unix 秒。end ≤ start 的区间会被丢弃 */
    int     stage;         /* rc_stage，0–4。文字别名先用 rc_parse_stage 转换 */
} rc_stage_interval;

/*
 * 一晚输入（3.1）。hrv 和 stages 不要求有序，可以有重复时刻和重叠区间，库内部会整理；
 * 两个数组只在调用期间被读取，不会被修改或保存。
 */
typedef struct {
    rc_date                  night_date;        /* 归属日期 = 醒来当天 */
    const rc_hrv_sample     *hrv;               /* n_hrv 为 0 时可以为 NULL */
    size_t                   n_hrv;
    const rc_stage_interval *stages;            /* n_stages 为 0 时可以为 NULL */
    size_t                   n_stages;
    int                      has_in_bed_start;  /* 0：没有上床时刻，用第一段分期的开始 */
    int64_t                  in_bed_start;      /* 上床时刻，Unix 秒。has_in_bed_start 非 0 时有效 */
    int                      hrv_is_ln;         /* 非 0：rmssd 字段已经是 lnRMSSD */
    double                   sleep_score_external; /* 外部睡眠分 0–100，有则替代睡眠子分。RC_NA = 没有 */
    int32_t                  utc_offset_sec;    /* 本地时间 − UTC，秒。只影响睡眠中点 */
} rc_night_input;

/* 把一条记录的可选字段全部置为缺失，duration_sec 置 0。 */
void rc_hrv_sample_init(rc_hrv_sample *s, int64_t t_start, double rmssd);

/* 把 rc_night_input 清零并把 sleep_score_external 置为 RC_NA。 */
void rc_night_input_init(rc_night_input *in, rc_date night_date);

/*
 * 分期别名 → 编码（3.3）。不区分大小写，忽略首尾空白。
 * 认识的写法：wake/awake/w/清醒、n1、n2/light/core/浅睡、n3/deep/sws/深睡、rem/快速眼动。
 * 上游只给"浅睡"不区分 N1/N2 时按 N2 处理。
 * 返回 RC_OK 并写入 *stage；不认识返回 RC_ERR_STAGE，*stage 不变。
 */
int rc_parse_stage(const char *text, int *stage);

/*
 * 症状字符串 → rc_symptoms。认识 none、mild、moderate、severe、moderate_severe，不区分大小写。
 * 不认识返回 RC_ERR_ARG。
 */
int rc_parse_symptoms(const char *text, int *symptoms);

/* 公历年月日 ↔ rc_date。不检查日期是否存在（2 月 30 日会顺延到 3 月）。 */
rc_date rc_date_from_ymd(int year, int month, int day);
void    rc_date_to_ymd(rc_date d, int *year, int *month, int *day);

/* ------------------------------------------------------------------ 单晚处理 */

/* 一个 5 分钟片段的中间结果，调试和可视化用，评分不需要它。 */
typedef struct {
    int    idx;          /* 去重排序后的序号，从 0 开始 */
    double t_start_s;    /* 相对"上床时刻或第一段分期开始"的秒数 */
    int    stage;        /* 映射到的主导阶段 rc_stage */
    int    stable;       /* 1 = 稳定片段（4.2） */
    double pos;          /* 片段中点在主睡眠期中的相对位置，0 = 入睡，1 = 醒来；可以超出 0–1 */
    double coverage;     /* 有 lnRMSSD 时等于 w_quality，否则 0 */
    double rmssd;        /* ms，无效为 RC_NA */
    double ln_rmssd;     /* 无效为 RC_NA */
    double hr;           /* bpm，缺失或超范围为 RC_NA */
    double resp;         /* 次/分，缺失为 RC_NA */
    double w_quality;    /* 四类权重，见 4.3 */
    double w_stage;
    double w_time;
    double w_stability;
    double weight;       /* 四者乘积；ln_rmssd 缺失时为 0 */
    double dur_s;        /* 窗口长度，秒 */
} rc_segment;

/* 一晚处理后的指标（13.1），评分的输入。也可以不经 rc_process_night 直接填。 */
typedef struct {
    rc_date night_date;
    double  recovery_hrv;         /* lnRMSSD 加权中位数。缺失为 RC_NA */
    double  early_hrv;            /* 前半夜 NREM 的 lnRMSSD。缺失为 RC_NA */
    double  late_hrv;             /* 后半夜 NREM 的 lnRMSSD。缺失为 RC_NA */
    double  rhr;                  /* bpm。缺失为 RC_NA */
    double  resp;                 /* 次/分。缺失为 RC_NA */
    double  tst_min;              /* 总睡眠时长，分钟 */
    double  efficiency;           /* 睡眠效率 0–1 */
    double  waso_min;             /* 入睡后清醒时长，分钟 */
    int     long_awakenings;      /* ≥5 分钟的夜醒次数 */
    double  midpoint_min;         /* 睡眠中点，距前一日 12:00 的分钟数，0–1439。没有睡眠为 RC_NA */
    double  sleep_score_external; /* 原样带过来的外部睡眠分。RC_NA = 没有 */
    int     n_good_segments;      /* 权重 > 0 且质量合格的片段数 */
    int     n_nrem_segments;      /* 其中 N2/N3 的个数 */
    double  wear_coverage;        /* 佩戴覆盖率 0–1 */
    int     valid_night;          /* 1 = 可以评分 */
    int     invalid_reason;       /* rc_invalid_reason */
} rc_night_metrics;

/* 把指标置为"什么都没有"的初始状态：double 缺失项为 RC_NA，valid_night=1，wear_coverage=1。 */
void rc_night_metrics_init(rc_night_metrics *m, rc_date night_date);

/* Late − Early（lnRMSSD 差）。任一缺失返回 RC_NA。 */
double rc_hrv_rebound(const rc_night_metrics *m);

/*
 * 一晚原始输入 → 当晚指标（第 4 章、6.1）。
 *
 *   in        [入] 一晚输入
 *   cfg       [入] 配置；NULL 用默认值
 *   out       [出] 当晚指标
 *   segs      [出] 可选。逐片段的中间结果，最多写 segs_cap 个；不需要传 NULL
 *   segs_cap  [入] segs 的容量
 *   n_segs    [出] 可选。实际片段数（去重后的记录数；没有任何睡眠时为 0）。
 *                  大于 segs_cap 说明被截断，out 不受影响
 *
 * 返回 RC_OK；分期编码非法返回 RC_ERR_STAGE；内存不足返回 RC_ERR_NOMEM。出错时 out 内容未定义。
 *
 * 边界：没有记录、没有分期、整晚都是 Wake 都算成功，结果是 valid_night=0（sleep_too_short）。
 * HRV 片段不够时 recovery_hrv 等为 RC_NA，但睡眠结构照常给出。
 */
int rc_process_night(const rc_night_input *in, const rc_config *cfg,
                     rc_night_metrics *out,
                     rc_segment *segs, size_t segs_cap, size_t *n_segs);

/* ------------------------------------------------------------------ 历史与基线 */

/*
 * 一晚的历史记录：当晚指标 + 后续评分要用到的少量结果字段。
 * 没有指针，可以直接写文件，下次启动用 rc_engine_load 装回（同一 ABI 下）。
 */
typedef struct {
    rc_night_metrics night;
    int    symptoms;          /* rc_symptoms */
    int    has_result;        /* 0：这一晚没有评分结果，下面的字段无效 */
    int    state;             /* rc_state */
    int    baseline_outlier;  /* 1 = 当晚被标记为基线离群（5.4） */
    int    illness_level;     /* rc_illness_level */
    double sleep_subscore;    /* 睡眠子分（保留 1 位小数）。无效夜为 RC_NA */
    double z_hrv;             /* 以下三项只在 state == RC_STATE_SCORED 时有值，保留 3 位小数 */
    double z_rhr;
    double resp_r;            /* RESP 严重度 */
} rc_day_record;

typedef struct {
    double median;
    double sigma;   /* 收缩后的波动幅度，≥ sigma_floor */
    int    n;       /* 参与统计的夜数；0 表示该指标还没有基线，median / sigma 无意义 */
} rc_metric_baseline;

/* 某一天评分时用的基线快照（13.2） */
typedef struct {
    rc_metric_baseline metrics[RC_METRIC_COUNT];
    int    n_nights;         /* 进入基线的夜数，≤ baseline_window */
    int    stage;            /* rc_baseline_stage */
    double sleep_need;       /* 个人睡眠需求，分钟 */
    double midpoint_median;  /* 睡眠中点中位数；不足 baseline_min_values 晚为 RC_NA */
    int    readmitted;       /* 按"新常态"重新纳入的夜数 */
    int    excluded;         /* 回看窗口内没进基线的夜数 */
} rc_baseline;

/*
 * 用 today 之前的历史建立基线（第 5 章）。today 当天及之后的记录不会被用到。
 *
 *   history  [入] 历史记录，顺序不限；n 为 0 时可以为 NULL
 *   today    [入] 要评分的日期
 *   cfg      [入] NULL 用默认值
 *   out      [出] 基线
 *
 * 返回 RC_OK / RC_ERR_ARG / RC_ERR_NOMEM。
 */
int rc_build_baseline(const rc_day_record *history, size_t n, rc_date today,
                      const rc_config *cfg, rc_baseline *out);

/* ------------------------------------------------------------------ 评分结果 */

typedef struct {
    int    code;                /* rc_gate_code */
    int    level;               /* rc_gate_level */
    double cap;                 /* 该门控的封顶分 */
    char   text[RC_TEXT_MAX];
} rc_gate;

typedef struct {
    int    key;                 /* rc_factor_key */
    int    gate;                /* key == RC_FACTOR_GATE 时是 rc_gate_code，否则为 -1 */
    int    positive;            /* 1 = 正向因素，0 = 负向 */
    double impact;              /* 对原始分的贡献（分），1 位小数；门控类因素为 RC_NA */
    char   text[RC_TEXT_MAX];
} rc_factor;

typedef struct { double hrv, rhr, sleep, resp; } rc_subscores;
typedef struct { double hrv, late, early, rhr, resp, resp_r; } rc_zscores;

/* 睡眠子分的分项，1 位小数（7.4） */
typedef struct {
    double duration, efficiency, regularity, continuity, debt;   /* 各 0–100 */
    double need_min;         /* 当晚睡眠需求（含睡眠债折算），分钟 */
    double debt_prev_min;    /* 今晚之前的累计睡眠债，分钟 */
    double debt_after_min;   /* 计入今晚后的睡眠债，分钟 */
} rc_sleep_detail;

/*
 * 一天的评分结果（13.3）。
 * state != RC_STATE_SCORED 时：score = -1，color / tier / confidence 为 NONE，
 * 数值字段除 sleep 子分和 sleep_detail（有效夜才有）外都是 RC_NA 或 0，summary 是无分状态的说明。
 */
typedef struct {
    rc_date date;
    int     state;                 /* rc_state */
    int     score;                 /* 0–100；没有分数时为 -1 */
    double  score_internal;        /* 取整前的分数，2 位小数 */
    int     color;                 /* rc_color */
    int     tier;                  /* rc_tier */
    char    advice[RC_ADVICE_MAX]; /* 分档建议；无分时为空串 */
    int     confidence;            /* rc_confidence */
    int     n_confidence_reasons;
    char    confidence_reasons[RC_MAX_REASONS][RC_TEXT_MAX];  /* 置信度原因在前，趋势原因在后 */
    int     baseline_stage;        /* rc_baseline_stage */
    int     baseline_nights;
    double  raw;                   /* 加权原始分（8.1），2 位小数 */
    double  trend_adjustment;      /* 趋势修正（8.2），2 位小数，范围 [trend_min, trend_max] */
    double  late_adjustment;       /* 后半夜修正（7.1），2 位小数，已含在 HRV 子分里 */
    double  cap;                   /* 生效的封顶分；没有门控时为 100 */
    rc_subscores subscores;        /* 各子分 0–100，1 位小数；缺失为 RC_NA */
    rc_subscores weights_used;     /* 重新分配后的权重，3 位小数；没参与的项为 RC_NA */
    rc_zscores   z;                /* 各指标 z 分数，3 位小数；缺失为 RC_NA */
    int     n_gates;
    rc_gate gates[RC_MAX_GATES];
    int     illness_points;        /* 不适风险总点数（含症状） */
    int     illness_physio_points; /* 仅生理指标的点数 */
    int     illness_level;         /* rc_illness_level */
    char    illness_message[RC_MESSAGE_MAX];  /* 等级为 none 时为空串 */
    int     n_top_factors;
    rc_factor top_factors[RC_MAX_FACTORS];
    char    summary[RC_SUMMARY_MAX];          /* 首页卡片文案 */
    int     baseline_outlier;      /* 1 = 这一晚以后不进基线（除非被"新常态"重新纳入） */
    rc_sleep_detail sleep_detail;  /* 没算睡眠子分时全为 RC_NA */
} rc_result;

/*
 * 给一晚打分（第 5–11 章）。纯函数，不修改 history。
 *
 *   night     [入] 当晚指标
 *   history   [入] 历史记录，顺序不限；只用到 night->night_date 之前的记录。n 为 0 时可以为 NULL
 *   symptoms  [入] rc_symptoms
 *   cfg       [入] NULL 用默认值
 *   out       [出] 结果
 *
 * 返回 RC_OK / RC_ERR_ARG / RC_ERR_NOMEM。不出分不算错误，看 out->state。
 */
int rc_score_day(const rc_night_metrics *night,
                 const rc_day_record *history, size_t n,
                 int symptoms, const rc_config *cfg, rc_result *out);

/* 把当晚指标和评分结果压成一条历史记录。result 为 NULL 时 has_result = 0。 */
void rc_day_record_make(rc_day_record *rec, const rc_night_metrics *night,
                        int symptoms, const rc_result *result);

/* ------------------------------------------------------------------ 多晚引擎 */

/*
 * 保存历史、逐晚评分的封装。历史按日期升序存放，同一日期只留最后一次。
 * 历史不会自动裁剪；评分只用到最近 baseline_lookback_days 天，更早的可以用
 * rc_engine_prune_before 删掉。
 */
typedef struct rc_engine rc_engine;

/* 创建引擎。cfg 为 NULL 用默认值，否则拷贝一份。失败（内存不足）返回 NULL。 */
rc_engine *rc_engine_create(const rc_config *cfg);

/* 释放引擎。传 NULL 无操作。 */
void rc_engine_destroy(rc_engine *e);

/*
 * 加入一晚原始输入并评分：rc_process_night → rc_score_day → 写入历史。
 * 同一 night_date 再次加入会替换旧记录，并按"旧记录不存在"重新评分。
 * 可以补录更早的日期，但之后日期的旧结果不会被重算。
 *
 *   result   [出] 当天结果
 *   metrics  [出] 可选，当晚指标
 *
 * 出错时历史不变。
 */
int rc_engine_add_night(rc_engine *e, const rc_night_input *in, int symptoms,
                        rc_result *result, rc_night_metrics *metrics);

/* 同上，但直接给当晚指标（上游已经算好，或者想跳过单晚处理）。 */
int rc_engine_add_metrics(rc_engine *e, const rc_night_metrics *metrics, int symptoms,
                          rc_result *result);

/* 历史条数。 */
size_t rc_engine_count(const rc_engine *e);

/*
 * 历史数组，按日期升序，共 rc_engine_count() 条。指针归引擎所有，
 * 下一次 add / load / prune / destroy 之后失效。没有历史时返回 NULL。
 */
const rc_day_record *rc_engine_history(const rc_engine *e);

/* 用给定记录替换全部历史（拷贝）。记录可以乱序；同日期的保留靠后的一条。 */
int rc_engine_load(rc_engine *e, const rc_day_record *records, size_t n);

/* 删除 night_date < before 的记录，返回删除的条数。 */
size_t rc_engine_prune_before(rc_engine *e, rc_date before);

/* 引擎当前使用的配置。 */
const rc_config *rc_engine_config(const rc_engine *e);

/* ------------------------------------------------------------------ RR 兼容路径 */

/*
 * 下面三个函数只给拿不到 5 分钟 HRV、只有逐搏 RR 和 30 秒分期的设备用：
 * 先转成标准输入，再走 rc_process_night。
 */

/* duration_sec 秒的记录会切成多少个 cfg.segment_sec 的窗口（向上取整）。 */
size_t rc_rr_window_count(double duration_sec, const rc_config *cfg);

/*
 * 逐搏 RR → 每窗一条 rc_hrv_sample（RMSSD、平均心率、质量）。
 *
 *   rr_ms         [入] RR 间期，ms，按时间顺序
 *   n_rr          [入] RR 个数
 *   rr_t_s        [入] 可选。每个 RR 结束时刻相对 start 的秒数；NULL 时用 RR 累加
 *   start         [入] 记录开始时刻，Unix 秒
 *   duration_sec  [入] 记录总时长，秒
 *   resp_brpm     [入] 可选。每窗一个呼吸率，NaN 表示缺失；没有传 NULL
 *   n_resp        [入] resp_brpm 的长度
 *   cfg           [入] NULL 用默认值
 *   out           [出] 至少 rc_rr_window_count() 个元素
 *   out_cap       [入] out 的容量
 *   n_out         [出] 写入的条数
 *
 * 伪迹处理：RR 超出 [rr_min_ms, rr_max_ms]，或偏离前后共 rr_local_window 拍的中位数
 * 超过 rr_jump_ratio，记为无效。窗内相邻有效 RR 对不足 min_rmssd_pairs 时 rmssd 为 RC_NA，
 * 有效拍不足 30 时 hr_bpm 为 RC_NA。
 *
 * 返回 RC_OK / RC_ERR_ARG / RC_ERR_CAPACITY / RC_ERR_NOMEM。
 */
int rc_rr_to_hrv5min(const double *rr_ms, size_t n_rr, const double *rr_t_s,
                     int64_t start, double duration_sec,
                     const double *resp_brpm, size_t n_resp,
                     const rc_config *cfg,
                     rc_hrv_sample *out, size_t out_cap, size_t *n_out);

/*
 * 等长 epoch 的分期序列 → 分期区间（相邻相同阶段合并）。
 *
 *   hyp        [入] 每个 epoch 的阶段编码
 *   n          [入] epoch 数
 *   start      [入] 第一个 epoch 的开始时刻，Unix 秒
 *   epoch_sec  [入] epoch 长度，秒，通常 30
 *   out        [出] 最坏情况需要 n 个元素
 *   out_cap    [入] out 的容量
 *   n_out      [出] 区间个数
 *
 * 这里不检查编码是否合法，非法编码会在 rc_process_night 里报 RC_ERR_STAGE。
 * 返回 RC_OK / RC_ERR_ARG / RC_ERR_CAPACITY。
 */
int rc_hypnogram_to_intervals(const int *hyp, size_t n, int64_t start, int epoch_sec,
                              rc_stage_interval *out, size_t out_cap, size_t *n_out);

/* ------------------------------------------------------------------ 名称 */

/*
 * 枚举 → 英文标识，与 Python 版输出的字符串相同（"scored"、"green"、"R_HRV_COMBO" 等）。
 * 返回静态字符串。Python 里为 None 的三项（颜色、分档、置信度的 NONE）返回 ""，
 * 越界的值返回 "?"。
 */
const char *rc_state_name(int state);
const char *rc_color_name(int color);
const char *rc_tier_name(int tier);
const char *rc_confidence_name(int confidence);
const char *rc_baseline_stage_name(int stage);
const char *rc_illness_level_name(int level);
const char *rc_invalid_reason_name(int reason);
const char *rc_symptoms_name(int symptoms);
const char *rc_gate_code_name(int code);
const char *rc_stage_name(int stage);

/* 影响因素的标识：门控类返回门控代码，其余返回 "hrv" / "late" / "rhr" / "sleep" / "resp" / "trend"。 */
const char *rc_factor_name(const rc_factor *f);

/* 错误码的中文说明。 */
const char *rc_strerror(int status);

/* 返回 RC_VERSION。 */
const char *rc_version(void);

#ifdef __cplusplus
}
#endif

#endif /* RECOVERY_H */
