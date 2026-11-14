/*
 * synth.h —— 单元测试用的合成夜晚。
 *
 * 结构仿照 Python 版 synthetic.make_night（90 分钟周期的分期、各阶段的 HRV / 心率均值、
 * 整晚的缓慢漂移），但随机数是自带的 LCG，数值和 Python 版不同。
 * 与 Python 的逐位比对不走这里，见 test_parity.c。
 */
#ifndef RC_TEST_SYNTH_H
#define RC_TEST_SYNTH_H

#include "recovery.h"

typedef struct {
    unsigned seed;         /* 随机种子，同一种子结果相同 */
    double tst_min;        /* 目标睡眠时长，分钟。默认 450 */
    double ln_shift;       /* 整晚 lnRMSSD 平移 */
    double early_shift;    /* 只加在前半夜 */
    double late_shift;     /* 只加在后半夜 */
    double hr_shift;       /* 心率平移，bpm */
    double resp_shift;     /* 呼吸率平移，次/分 */
    int    waso_bouts;     /* 夜醒次数，每次 4–9 分钟。默认 1 */
    int    drop_hrv;       /* 非 0：rmssd 全部缺失 */
    int    drop_hr;        /* 非 0：心率全部缺失 */
    int    drop_resp;      /* 非 0：呼吸率全部缺失 */
    double wear_gap_frac;  /* 中间脱戴的比例，这段没有记录。默认 0 */
    int    night_noise;    /* 非 0：加上每晚不同的整体偏移（建基线时用）。默认 1 */
} synth_opts;

/* 一晚合成数据。in.hrv / in.stages 指向 hrv / stages，用完调 synth_free。 */
typedef struct {
    rc_night_input     in;
    rc_hrv_sample     *hrv;
    rc_stage_interval *stages;
} synth_night;

void synth_opts_default(synth_opts *o, unsigned seed);

/*
 * 生成 d 这一天（醒来日）的一晚，前一天 23:00 上床，时间按墙上时间算（utc_offset_sec = 0）。
 * 成功返回 1，内存不足返回 0。
 */
int synth_make(rc_date d, const synth_opts *o, synth_night *out);

void synth_free(synth_night *n);

/* 均值 mean、标准差 sd 的正态随机数，state 是 LCG 状态。RR 往返测试用。 */
double synth_gauss(unsigned *state, double mean, double sd);

#endif
