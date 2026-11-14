/*
 * night_example.c —— 读一晚 JSON 输入，打印当晚指标。
 *
 * 用法：night_example <night.json>
 *
 * 输入格式与 Python 版 NightRaw.from_dict 相同，见 recovery_v1/examples/night_input_example.json。
 * 输出格式与 recovery_v1/examples/run_example.py 相同（浮点保留 3 位小数），可以直接 diff。
 *
 * JSON 里的时间是不带时区的 ISO 8601 字符串，这里按本地墙上时间换成秒，utc_offset_sec 填 0。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "recovery.h"

/* "YYYY-MM-DD" → rc_date。格式不对返回 0。 */
static int parse_date(const char *s, rc_date *out)
{
    int y, m, d;

    if (!s || sscanf(s, "%d-%d-%d", &y, &m, &d) != 3)
        return 0;
    *out = rc_date_from_ymd(y, m, d);
    return 1;
}

/* "YYYY-MM-DDTHH:MM[:SS]" → 秒（按墙上时间，不做时区换算）。格式不对返回 0。 */
static int parse_datetime(const char *s, int64_t *out)
{
    int y, mo, d, h = 0, mi = 0, sec = 0;

    if (!s || sscanf(s, "%d-%d-%d%*1[T ]%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 5)
        return 0;
    *out = (int64_t)rc_date_from_ymd(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec;
    return 1;
}

/* 可选数字字段：没有或为 null 返回 RC_NA */
static double opt_number(const json_value *obj, const char *key)
{
    return json_number(json_get(obj, key), RC_NA);
}

/* stage 字段可以是 0–4 的整数，也可以是字符串别名 */
static int read_stage(const json_value *v, int *stage)
{
    if (v && v->type == JSON_NUMBER) {
        *stage = (int)v->number;
        return RC_OK;
    }
    return rc_parse_stage(json_string(v), stage);
}

/*
 * JSON → rc_night_input。hrv 和 stages 两个数组由本函数 malloc，调用方 free。
 * 成功返回 1；失败返回 0 并在 stderr 说明原因。
 */
static int load_night(const json_value *root, rc_night_input *in,
                      rc_hrv_sample **hrv_out, rc_stage_interval **stages_out)
{
    const json_value *jh = json_get(root, "hrv_5min");
    const json_value *js = json_get(root, "stages");
    size_t nh = json_count(jh), ns = json_count(js), i;
    rc_hrv_sample *hrv;
    rc_stage_interval *stages;
    rc_date date;

    if (!parse_date(json_string(json_get(root, "night_date")), &date)) {
        fprintf(stderr, "night_date 缺失或格式不对\n");
        return 0;
    }
    rc_night_input_init(in, date);

    hrv = (rc_hrv_sample *)malloc((nh ? nh : 1) * sizeof *hrv);
    stages = (rc_stage_interval *)malloc((ns ? ns : 1) * sizeof *stages);
    *hrv_out = hrv;
    *stages_out = stages;
    if (!hrv || !stages) {
        fprintf(stderr, "内存不足\n");
        return 0;
    }

    for (i = 0; i < nh; i++) {
        const json_value *x = json_at(jh, i);
        int64_t t;

        if (!parse_datetime(json_string(json_get(x, "t_start")), &t)) {
            fprintf(stderr, "hrv_5min[%lu].t_start 格式不对\n", (unsigned long)i);
            return 0;
        }
        rc_hrv_sample_init(&hrv[i], t, opt_number(x, "rmssd"));
        hrv[i].hr_bpm = opt_number(x, "hr_bpm");
        hrv[i].resp_brpm = opt_number(x, "resp_brpm");
        hrv[i].quality = opt_number(x, "quality");
        hrv[i].motion = opt_number(x, "motion");
        hrv[i].duration_sec = (int32_t)json_number(json_get(x, "duration_sec"), 0);
    }
    for (i = 0; i < ns; i++) {
        const json_value *x = json_at(js, i);

        if (!parse_datetime(json_string(json_get(x, "start")), &stages[i].start) ||
            !parse_datetime(json_string(json_get(x, "end")), &stages[i].end)) {
            fprintf(stderr, "stages[%lu] 的时间格式不对\n", (unsigned long)i);
            return 0;
        }
        if (read_stage(json_get(x, "stage"), &stages[i].stage) != RC_OK) {
            fprintf(stderr, "stages[%lu].stage 不认识\n", (unsigned long)i);
            return 0;
        }
    }

    in->hrv = hrv;
    in->n_hrv = nh;
    in->stages = stages;
    in->n_stages = ns;
    in->hrv_is_ln = json_bool(json_get(root, "hrv_is_ln"), 0);
    in->sleep_score_external = opt_number(root, "sleep_score_external");
    if (parse_datetime(json_string(json_get(root, "in_bed_start")), &in->in_bed_start))
        in->has_in_bed_start = 1;
    return 1;
}

/*
 * 按 Python 的方式打印 round(v, 3)：去掉多余的尾零，至少留一位小数；缺失打印 null。
 */
static void print_number(const char *key, double v, int last)
{
    char buf[64];
    size_t n;

    if (rc_is_na(v)) {
        printf(" \"%s\": null%s\n", key, last ? "" : ",");
        return;
    }
    snprintf(buf, sizeof buf, "%.3f", v);
    n = strlen(buf);
    while (n > 2 && buf[n - 1] == '0' && buf[n - 2] != '.')
        buf[--n] = '\0';
    printf(" \"%s\": %s%s\n", key, buf, last ? "" : ",");
}

static void print_metrics(const rc_night_metrics *m)
{
    int y, mo, d;

    rc_date_to_ymd(m->night_date, &y, &mo, &d);
    printf("{\n");
    printf(" \"night_date\": \"%04d-%02d-%02d\",\n", y, mo, d);
    print_number("recovery_hrv", m->recovery_hrv, 0);
    print_number("early_hrv", m->early_hrv, 0);
    print_number("late_hrv", m->late_hrv, 0);
    print_number("rhr", m->rhr, 0);
    print_number("resp", m->resp, 0);
    print_number("tst_min", m->tst_min, 0);
    print_number("efficiency", m->efficiency, 0);
    print_number("waso_min", m->waso_min, 0);
    printf(" \"long_awakenings\": %d,\n", m->long_awakenings);
    print_number("midpoint_min", m->midpoint_min, 0);
    print_number("sleep_score_external", m->sleep_score_external, 0);
    printf(" \"n_good_segments\": %d,\n", m->n_good_segments);
    printf(" \"n_nrem_segments\": %d,\n", m->n_nrem_segments);
    print_number("wear_coverage", m->wear_coverage, 0);
    printf(" \"valid_night\": %s,\n", m->valid_night ? "true" : "false");
    if (m->invalid_reason == RC_INVALID_NONE)
        printf(" \"invalid_reason\": null\n");
    else
        printf(" \"invalid_reason\": \"%s\"\n", rc_invalid_reason_name(m->invalid_reason));
    printf("}\n");
}

int main(int argc, char **argv)
{
    char err[128];
    json_value *root;
    rc_night_input in;
    rc_night_metrics m;
    rc_hrv_sample *hrv = NULL;
    rc_stage_interval *stages = NULL;
    int rc = 1;

    if (argc != 2) {
        fprintf(stderr, "用法：%s <night.json>\n", argv[0]);
        return 2;
    }
    root = json_parse_file(argv[1], err, sizeof err);
    if (!root) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    if (load_night(root, &in, &hrv, &stages)) {
        int st = rc_process_night(&in, NULL, &m, NULL, 0, NULL);
        if (st == RC_OK) {
            print_metrics(&m);
            rc = 0;
        } else {
            fprintf(stderr, "rc_process_night：%s\n", rc_strerror(st));
        }
    }
    free(hrv);
    free(stages);
    json_free(root);
    return rc;
}
