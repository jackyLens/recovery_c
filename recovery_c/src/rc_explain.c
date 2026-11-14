/* 文案与影响因素（第 11 章）。所有面向用户的中文都在这个文件和 rc_scoring.c 的门控表里。 */
#include "rc_internal.h"

#include <stdio.h>
#include <string.h>

const char *rc_tier_label(int tier)
{
    switch (tier) {
    case RC_TIER_PEAK:        return "状态很好";
    case RC_TIER_GREEN:       return "恢复良好";
    case RC_TIER_HIGH_YELLOW: return "恢复一般";
    case RC_TIER_LOW_YELLOW:  return "恢复偏低";
    case RC_TIER_RED:         return "需要休息";
    default:                  return "";
    }
}

const char *rc_tier_advice(int tier)
{
    switch (tier) {
    case RC_TIER_PEAK:
        return "身体恢复充分，适合安排高强度训练或重要任务。";
    case RC_TIER_GREEN:
        return "可以正常训练和工作，按计划进行。";
    case RC_TIER_HIGH_YELLOW:
        return "可以活动，但建议降低强度，避免冲击极限。";
    case RC_TIER_LOW_YELLOW:
        return "建议以轻松活动为主，比如散步、拉伸，并早点休息。";
    case RC_TIER_RED:
        return "身体信号提示恢复不足，今天建议以休息为主，避免高强度训练。";
    default:
        return "";
    }
}

const char *rc_illness_text(int level)
{
    switch (level) {
    case RC_ILL_MILD:
        return "你的夜间恢复指标出现轻度异常，今天可以适当降低强度，多补充水分并保证睡眠。";
    case RC_ILL_MODERATE:
        return "你的夜间恢复指标出现多项异常，与平时状态相比明显偏离。"
               "建议今天以休息为主，注意身体感受。";
    case RC_ILL_HIGH:
        return "你的夜间多项身体信号明显偏离个人基线。建议今天充分休息；"
               "如果同时感到明显不适，请关注身体状况，必要时咨询专业人士。";
    default:
        return "";
    }
}

const char *rc_state_text(int state)
{
    switch (state) {
    case RC_STATE_CALIBRATING:
        return "正在建立你的个人基线，再佩戴几晚即可看到恢复分。";
    case RC_STATE_SLEEP_TOO_SHORT:
        return "昨晚睡眠时间太短，无法可靠评估恢复状态。";
    case RC_STATE_INSUFFICIENT_DATA:
        return "昨晚有效佩戴数据不足，今天暂不给出恢复分。";
    case RC_STATE_NO_CORE_SIGNAL:
        return "昨晚 HRV 和静息心率均无法可靠获取，今天暂不给出恢复分。";
    default:
        return "";
    }
}

/*
 * 去重用的"话题"编号：同一话题只展示一条。
 * 门控按它说的是哪项指标归类；说不清是哪项的（多项严重、症状、不适风险）各自单独一类。
 */
enum {
    TOPIC_HRV = 0, TOPIC_LATE, TOPIC_RHR, TOPIC_RESP, TOPIC_SLEEP, TOPIC_TREND,
    TOPIC_GATE_BASE   /* + rc_gate_code */
};

static int gate_topic(int code)
{
    switch (code) {
    case RC_GATE_R_HRV_COMBO:
    case RC_GATE_Y_HRV:        return TOPIC_HRV;
    case RC_GATE_R_LATE_COMBO:
    case RC_GATE_Y_LATE:       return TOPIC_LATE;
    case RC_GATE_Y_RHR:        return TOPIC_RHR;
    case RC_GATE_R_RESP_COMBO:
    case RC_GATE_Y_RESP:       return TOPIC_RESP;
    case RC_GATE_R_SLEEP:
    case RC_GATE_Y_SLEEP:
    case RC_GATE_Y_TST:        return TOPIC_SLEEP;
    default:                   return TOPIC_GATE_BASE + code;
    }
}

typedef struct {
    int         key;      /* rc_factor_key */
    int         topic;
    double      impact;
    const char *text;
    double      order;    /* 排序键，小的在前 */
} candidate;

void rc_top_factors(rc_result *res)
{
    const rc_subscores *s = &res->subscores, *w = &res->weights_used;
    candidate cand[6];
    unsigned seen = 0;
    int n_cand = 0, n_out = 0, pass, i, j, neg_first;
    double imp;

    /* 1) 触发的门控优先，红在黄前。Y_LOW_CONF 说的是数据而不是身体，不展示。 */
    for (pass = 0; pass < 2 && n_out < RC_MAX_FACTORS; pass++) {
        int level = pass == 0 ? RC_GATE_RED : RC_GATE_YELLOW;

        for (i = 0; i < res->n_gates && n_out < RC_MAX_FACTORS; i++) {
            const rc_gate *g = &res->gates[i];
            rc_factor *f;
            int topic;

            if (g->level != level || g->code == RC_GATE_Y_LOW_CONF)
                continue;
            topic = gate_topic(g->code);
            if (seen & (1u << topic))
                continue;
            seen |= 1u << topic;
            f = &res->top_factors[n_out++];
            f->key = RC_FACTOR_GATE;
            f->gate = g->code;
            f->positive = 0;
            f->impact = RC_NA;
            rc_strcpy(f->text, sizeof f->text, g->text);
        }
    }
    res->n_top_factors = n_out;
    if (n_out >= RC_MAX_FACTORS)
        return;

    /* 2) 各项对原始分的贡献 =（子分 − 参照分）× 权重。
          RHR、RESP 满分就是正常，没有"加分"，正常时给一个固定的小正值用于排序。 */
#define ADD(k, t, v, txt)                                                               \
    do {                                                                                \
        cand[n_cand].key = (k);                                                         \
        cand[n_cand].topic = (t);                                                       \
        cand[n_cand].impact = (v);                                                      \
        cand[n_cand].text = (txt);                                                      \
        n_cand++;                                                                       \
    } while (0)

    if (!rc_is_na(s->hrv)) {
        imp = (s->hrv - 70.0) * (rc_is_na(w->hrv) ? 0.0 : w->hrv);
        ADD(RC_FACTOR_HRV, TOPIC_HRV, imp,
            imp > 0 ? "整夜 HRV 高于个人基线" : "整夜 HRV 低于个人基线");
    }
    if (res->late_adjustment != 0.0) {
        imp = res->late_adjustment * (rc_is_na(w->hrv) ? 0.0 : w->hrv);
        ADD(RC_FACTOR_LATE, TOPIC_LATE, imp,
            imp > 0 ? "后半夜 HRV 明显回升，恢复在夜里逐步完成" : "后半夜 HRV 回升不足");
    }
    if (!rc_is_na(s->rhr)) {
        imp = (s->rhr - 100.0) * (rc_is_na(w->rhr) ? 0.0 : w->rhr);
        ADD(RC_FACTOR_RHR, TOPIC_RHR, imp < 0 ? imp : 0.5,
            imp < 0 ? "静息心率高于个人基线" : "静息心率处于个人正常范围");
    }
    if (!rc_is_na(s->sleep)) {
        imp = (s->sleep - 80.0) * (rc_is_na(w->sleep) ? 0.0 : w->sleep);
        ADD(RC_FACTOR_SLEEP, TOPIC_SLEEP, imp,
            imp > 0 ? "睡眠充足、连续" : "睡眠时长或质量不足");
    }
    if (!rc_is_na(s->resp)) {
        imp = (s->resp - 100.0) * (rc_is_na(w->resp) ? 0.0 : w->resp);
        ADD(RC_FACTOR_RESP, TOPIC_RESP, imp < 0 ? imp : 0.3,
            imp < 0 ? "夜间呼吸率偏高" : "夜间呼吸率平稳");
    }
    if (res->trend_adjustment != 0.0)
        ADD(RC_FACTOR_TREND, TOPIC_TREND, res->trend_adjustment,
            res->trend_adjustment > 0 ? "近几天整体在回升" : "近几天指标持续走弱");
#undef ADD

    /* 非绿色先讲负向因素，绿色先讲正向因素；同方向内按影响从大到小。稳定排序。 */
    neg_first = res->color != RC_COLOR_GREEN;
    for (i = 0; i < n_cand; i++) {
        double a = fabs(cand[i].impact);
        cand[i].order = ((cand[i].impact < 0) == neg_first) ? -a : 1000 - a;
    }
    for (i = 1; i < n_cand; i++) {
        candidate c = cand[i];
        for (j = i; j > 0 && cand[j - 1].order > c.order; j--)
            cand[j] = cand[j - 1];
        cand[j] = c;
    }

    for (i = 0; i < n_cand && n_out < RC_MAX_FACTORS; i++) {
        rc_factor *f;

        if ((seen & (1u << cand[i].topic)) || fabs(cand[i].impact) < 0.3)
            continue;
        seen |= 1u << cand[i].topic;
        f = &res->top_factors[n_out++];
        f->key = cand[i].key;
        f->gate = -1;
        f->positive = cand[i].impact > 0;
        f->impact = rc_round_dp(cand[i].impact, 1);
        rc_strcpy(f->text, sizeof f->text, cand[i].text);
    }
    res->n_top_factors = n_out;
}

void rc_summary_text(rc_result *res)
{
    const char *label = rc_tier_label(res->tier);

    if (res->n_top_factors == 0)
        snprintf(res->summary, sizeof res->summary, "%s。%s", label, res->advice);
    else if (res->n_top_factors == 1)
        snprintf(res->summary, sizeof res->summary, "%s：%s。%s", label,
                 res->top_factors[0].text, res->advice);
    else
        snprintf(res->summary, sizeof res->summary, "%s：%s；%s。%s", label,
                 res->top_factors[0].text, res->top_factors[1].text, res->advice);
}
