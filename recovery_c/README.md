# Recovery 恢复分 C 版（V1.0.2）

Python 参考实现 `recovery_v1`（1.0.2）的 C99 移植。输入是一晚的 5 分钟 HRV 记录和带起止时刻的睡眠分期，
输出当晚指标和 0–100 的整数恢复分。只依赖 C 标准库和 libm。

文中的章节号指《Recovery 功能设计方案 V1.0 修订版》。V1.0 不使用体温、SpO₂、训练负荷、标签、问卷和生理周期，
接口里没有这些字段。结果是健康参考信息，不是医疗诊断。

## 目录

```
include/recovery.h        公开接口：全部结构体、枚举、错误码、函数
src/rc_config.c           默认阈值                         config.py
src/rc_stages.c           分期区间规整、睡眠结构           stages.py
src/rc_segments.c         5 分钟记录校验、分期映射、权重   segments.py
src/rc_night.c            单晚指标聚合、有效性             night.py
src/rc_sleep.c            睡眠子分、睡眠债                 sleep.py
src/rc_baseline.c         个人基线、sigma 收缩、新常态     baseline.py
src/rc_scoring.c          子分、趋势、门控、不适风险、置信度 scoring.py
src/rc_explain.c          影响因素和中文文案               explain.py
src/rc_engine.c           rc_score_day 和多晚引擎          engine.py
src/rc_rr.c               逐搏 RR 的兼容路径               rr_adapter.py
src/rc_util.c             数值工具、解析、枚举名           utils.py、models.py
src/rc_internal.h         内部声明，不对外
examples/night_example.c  读一晚 JSON，打印当晚指标
examples/json.[ch]        示例和测试用的小型 JSON 读取器，不进库
tests/test_recovery.c     单元测试
tests/synth.[ch]          单元测试用的合成夜晚
tests/test_parity.c       与 Python 逐字段比对
tests/data/parity_cases.json  Python 导出的比对用例
tools/export_parity_cases.py  生成上面这个文件
```

右边一列是对应的 Python 模块。`rc_config` 的字段名和默认值与 `config.py` 的 `Config` 一一对应。

## 编译

```
make                  # build/librecovery.a、示例、两个测试程序
make test             # 单元测试 + 一致性比对（用已导出的用例，不需要 Python）
make compare-example  # 示例 JSON 分别过 Python 和 C，diff 输出
make parity-data      # 用 Python 重新导出比对用例（需要 numpy）
```

编译选项 `-std=c99 -Wall -Wextra -O2`，gcc 12.2 下没有警告。`PYREF` 指向 Python 参考实现的目录，
默认 `../recovery_v1`。

集成到别的工程：把 `include/recovery.h` 和 `src/` 下的文件加进去即可，没有平台相关代码。两点要注意：

- 缺失值用 NaN 表示，不能开 `-ffast-math` 或 `-ffinite-math-only`。
- `rc_round_dp` 借 `printf("%.Nf")` 做十进制舍入，要求 C 库的 printf 是正确舍入的（glibc、Apple libc、musl 都是）。

## 输入

一晚对应一个 `rc_night_input`：

| 字段 | 说明 |
|---|---|
| `night_date` | 归属日期，醒来当天。`rc_date` 是自 1970-01-01 起的天数，用 `rc_date_from_ymd` 换算 |
| `hrv` / `n_hrv` | 每 5 分钟一条 `rc_hrv_sample` |
| `stages` / `n_stages` | 睡眠分期区间 `rc_stage_interval`，`[start, end)` |
| `has_in_bed_start` / `in_bed_start` | 上床时刻，可选。没有时用第一段分期的开始 |
| `hrv_is_ln` | 非 0 表示 `rmssd` 字段已经是 lnRMSSD |
| `sleep_score_external` | 外部睡眠分 0–100，可选。有则替代库内的睡眠子分 |
| `utc_offset_sec` | 本地时间 − UTC。只影响睡眠中点（规律性） |

`rc_hrv_sample`：`t_start`（Unix 秒）、`rmssd`（ms）必填；`hr_bpm`、`resp_brpm`、`quality`（0–1，缺省按 1）、
`motion`（0–1）可选，没有填 `RC_NA`；`duration_sec` ≤ 0 时按 300 秒。用 `rc_hrv_sample_init` 初始化可以省掉逐个填 `RC_NA`。

分期编码 Wake=0、N1=1、N2=2、N3=3、REM=4。上游给文字时用 `rc_parse_stage` 转换，认识
wake/awake/w/清醒、n1、n2/light/core/浅睡、n3/deep/sws/深睡、rem/快速眼动。只给"浅睡"不分 N1/N2 的按 N2。

记录和分期都不要求有序。同一时刻的重复记录留后出现的一条；分期区间重叠时后一段截断前一段。

症状是 `rc_engine_add_night` / `rc_score_day` 的参数，不在 `rc_night_input` 里：`RC_SYMPTOMS_NONE`、`_MILD`、
`_MODERATE_SEVERE`。`rc_parse_symptoms` 接受 none、mild、moderate、severe、moderate_severe。

时间一律是 int64 Unix 秒。如果上游给的是不带时区的本地时间，直接按墙上时间换成秒、`utc_offset_sec` 填 0 即可，
示例程序就是这么做的。

只有逐搏 RR 和 30 秒分期的设备，先用 `rc_rr_to_hrv5min` 和 `rc_hypnogram_to_intervals` 转成上面的输入。

## 输出

`rc_night_metrics`（当晚指标，13.1）：Recovery / Early / Late HRV（lnRMSSD）、RHR、RESP、TST、睡眠效率、WASO、
长觉醒次数、睡眠中点、好片段数、佩戴覆盖率、`valid_night` 和 `invalid_reason`。

`rc_result`（评分结果，13.3）：

- `state`：只有 `RC_STATE_SCORED` 有分数。其余是 calibrating（基线不足 3 晚）、insufficient_data（佩戴不够）、
  sleep_too_short（睡眠不足 3 小时）、no_core_signal（HRV 和 RHR 都没有）。无分时 `score` 为 -1，`summary` 是给用户的说明。
- `score`（0–100 整数）、`color`（≥67 绿，34–66 黄，≤33 红）、`tier`、`advice`。
- `confidence` 和 `confidence_reasons`。
- `subscores`、`weights_used`、`z`、`raw`、`late_adjustment`、`trend_adjustment`、`cap`：分数怎么来的。
- `gates`：触发的门控，Red 封顶 33，Yellow 封顶 66。
- `illness_level`、`illness_points`、`illness_message`：不适风险提示。
- `top_factors`（最多 3 条）和 `summary`：首页展示用的文案。
- `baseline_stage`、`baseline_nights`、`baseline_outlier`、`sleep_detail`。

double 字段缺失时是 `RC_NA`，用 `rc_is_na()` 判断。枚举可以用 `rc_state_name()` 等函数换成和 Python 版相同的英文标识。
每个字段的单位和取值范围见头文件。

## 调用

```c
#include <stdio.h>
#include "recovery.h"

int main(void)
{
    /* 前一天 23:00 上床，06:30 醒：一段清醒 + 一段 N2 + 一段 N3 + 一段 REM */
    int64_t t0 = (int64_t)rc_date_from_ymd(2026, 9, 1) * 86400 - 3600;
    rc_stage_interval stages[4] = {
        { t0,         t0 + 900,   RC_WAKE },
        { t0 + 900,   t0 + 14400, RC_N2 },
        { t0 + 14400, t0 + 21600, RC_N3 },
        { t0 + 21600, t0 + 27000, RC_REM },
    };
    rc_hrv_sample hrv[90];
    rc_night_input in;
    rc_night_metrics m;
    rc_result res;
    rc_engine *eng;
    int i, rc;

    for (i = 0; i < 90; i++) {
        rc_hrv_sample_init(&hrv[i], t0 + 300 * i, 42.0);   /* RMSSD 42 ms */
        hrv[i].hr_bpm = 56.0;                              /* 可选字段，没有就不填 */
        hrv[i].resp_brpm = 14.5;
    }

    rc_night_input_init(&in, rc_date_from_ymd(2026, 9, 1));
    in.hrv = hrv;
    in.n_hrv = 90;
    in.stages = stages;
    in.n_stages = 4;

    eng = rc_engine_create(NULL);          /* NULL = 默认配置 */
    if (!eng)
        return 1;
    rc = rc_engine_add_night(eng, &in, RC_SYMPTOMS_NONE, &res, &m);
    if (rc != RC_OK) {
        fprintf(stderr, "%s\n", rc_strerror(rc));
        rc_engine_destroy(eng);
        return 1;
    }

    printf("TST %.0f 分钟，lnRMSSD %.3f，RHR %.1f\n", m.tst_min, m.recovery_hrv, m.rhr);
    if (res.state == RC_STATE_SCORED)
        printf("恢复分 %d（%s）\n", res.score, rc_color_name(res.color));
    printf("%s\n", res.summary);

    rc_engine_destroy(eng);
    return 0;
}
```

第一晚没有基线，输出是：

```
TST 435 分钟，lnRMSSD 3.738，RHR 56.0
正在建立你的个人基线，再佩戴几晚即可看到恢复分。
```

连续加入 4 晚后开始出分，30 晚后基线进入正式阶段。

三种用法：

1. `rc_engine`：库保存历史，每晚调一次 `rc_engine_add_night`。最省事。
2. 自己管历史：`rc_process_night` 得到指标，`rc_score_day(night, history, n, ...)` 评分，再用 `rc_day_record_make`
   把这一晚压成一条 `rc_day_record` 存起来。这两个函数都不保存状态。
3. 上游已经有当晚指标：填 `rc_night_metrics`，调 `rc_engine_add_metrics` 或 `rc_score_day`。

改阈值：`rc_config_default(&cfg)` 之后改字段，传给 `rc_engine_create` 或各函数的 `cfg` 参数。

持久化：`rc_day_record` 里没有指针，`rc_engine_history()` 返回的数组可以整块写文件，启动时 `rc_engine_load` 装回。
这是按结构体内存布局存的，换编译器或架构前要自己做序列化。评分只用到最近 60 天，更早的记录可以
`rc_engine_prune_before` 删掉。

同一日期重复加入会替换旧记录。补录更早的日期可以，但之后各天已经给出的结果不会重算。

## 内存和线程

- 除 `rc_engine` 外，所有结构体由调用方分配，库不持有调用方的指针。`rc_night_input` 里的两个数组只在调用期间被读取。
- `rc_result` 约 5 KB，文案是结构体里的定长数组，不需要释放。
- `rc_process_night`、`rc_build_baseline`、`rc_score_day` 内部按输入大小临时 malloc，返回前释放；失败返回 `RC_ERR_NOMEM`。
  要换分配器，编译时定义 `RC_MALLOC` / `RC_REALLOC` / `RC_FREE`（见 `src/rc_internal.h`）。
- `rc_engine_create` 分配，`rc_engine_destroy` 释放。`rc_engine_history()` 返回的指针在下一次 add / load / prune / destroy 后失效。
- 没有全局可变状态。无状态函数可以在多个线程里同时调用；同一个 `rc_engine` 不能并发访问，需要的话在外面加锁；
  不同的 `rc_engine` 互不相干。
- 出错时 `rc_engine` 的历史保持不变。

## 与 Python 版的差别

算法没有差别，下面是接口形式上的不同：

- Python 的 `None` 在 C 里是 `RC_NA`（double）、空串（文案和枚举名）或 -1（`score`）。
- Python 用字符串表示的状态、颜色、门控代码等，在 C 里是枚举；`rc_*_name()` 给出相同的字符串。
- Python 的 `subscores` / `weights_used` 字典里不存在的键，在 C 里是对应字段为 `RC_NA`。
- 症状多接受 `moderate` 和 `severe` 两个写法，都当作 `moderate_severe`。
- Python 用 datetime，C 用 Unix 秒加 `utc_offset_sec`。
- 分期编码非法时 Python 抛 `ValueError`，C 返回 `RC_ERR_STAGE`。

为了数值上逐位一致做的处理：求和按 numpy 的成对求和分块；`round(x, n)` 按十进制精确值舍入；最终分数用
`floor(x + 0.5)`；加权中位数的排序是稳定的。趋势里的线性斜率 Python 用 `np.polyfit`，C 用闭式最小二乘，
两者有 1e-15 量级的差别，理论上可能在舍入边界上让 `trend_adjustment` 差 0.01，下面的比对里没有出现。

## 测试结果

环境：Linux x86_64，gcc 12.2.0，Python 3.11.17，numpy 1.26.4。

**编译**：`make clean && make all`，0 警告。另用 `-Wpedantic -Wshadow -Wconversion -Wno-sign-conversion` 编过一遍，也是 0 警告。

**单元测试**（`build/test_recovery`）：54 个用例、473 条断言，全部通过。其中 35 个对应 Python
`tests/test_recovery.py` 的 37 个用例；没有移植的两个是 `test_no_temperature_or_spo2_inputs`（用反射检查字段名，
C 里做不了，头文件里确实没有这些字段）和 `test_from_dict_json`（由 `make compare-example` 覆盖）。其余 19 个测
C 接口：解析函数、日期换算、错误码、缓冲区容量、乱序输入、引擎的装载和裁剪、自定义配置等。

**示例比对**（`make compare-example`）：`recovery_v1/examples/night_input_example.json` 分别经
`run_example.py` 和 `build/night_example` 处理，两份输出 diff 无差异。

**一致性比对**（`build/test_parity tests/data/parity_cases.json`）：

```
一致性比对：40/40 个序列、394/394 晚完全一致（其中出分 357 晚）；比较 30846 个字段，不一致 0 个；浮点最大绝对差 0（容差 1e-09）
```

用例由 `tools/export_parity_cases.py` 调 Python 的 `synthetic.make_night` 生成，共 40 个序列 394 晚：

- `demo.py` 的全部内容：40 晚建基线（含冷启动）、场景 1–7、睡眠不足 3 小时、脱戴过多、HRV 缺失、连续 4 晚疲劳、疲劳后 7 晚回升。
- Python 单元测试里走引擎的用例：冷启动、正常夜、疲劳、假性恢复、缺 RESP、缺心率、缺 HRV、无核心信号、症状、同日期重复、
  文字分期、ln 输入、尖刺和超范围窗口。
- 补充的输入形式：记录和分期乱序并带重复时刻、外部睡眠分、手写分期区间（含中文别名）、没有记录、没有分期、示例 JSON。
- RR 兼容路径两晚：干净的 RR，和带伪迹、带呼吸率、中间丢一段的 RR。
- 两个 120 天的随机序列（掺疲劳、不适、短睡、缺信号、脱戴、症状、缺测日）和一个生理水平整体平移的序列（覆盖基线排除和新常态）。

这些用例里 16 条门控规则每条都触发过，5 种结果状态、4 档不适风险、5 个基线阶段都出现过，被标记为基线离群的有 48 晚。

逐晚比较的内容：全部夜间指标；`state`、`score`、`score_internal`、`color`、`tier`、`confidence`、`cap`、`raw`、
趋势和后半夜修正、各子分、权重、z 分数、睡眠分项、门控（代码、级别、封顶、文案，含顺序）、不适风险点数和等级、
影响因素、置信度原因、`summary`、`advice`、`illness_message`、基线阶段和夜数、离群标记。字符串和整数要求完全相同，
浮点容差 1e-9。结果是全部相同，浮点字段的差也是 0。

**内存检查**：三个程序用 `-fsanitize=address,undefined` 编译后运行，没有报告。这台机器上 LeakSanitizer 不能用
（ptrace 受限），ASan 本身也有约两成概率在进入 main 之前崩溃（空程序同样如此），以上结论取自正常启动的那些次运行。
泄漏另用 `--wrap=malloc` 计数检查：三个程序退出时未释放的块数都是 0。没有跑 valgrind（未安装）。

没有验证的：gcc / x86_64 以外的编译器和架构；内存分配失败的路径只读过代码，没有做故障注入。
