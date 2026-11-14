#!/usr/bin/env python3
"""用 Python 参考实现生成一致性用例，供 tests/test_parity.c 读取。

用法：python3 tools/export_parity_cases.py <recovery_v1 目录> <输出.json>

输出是若干"序列"。每个序列是一台引擎上按顺序加入的若干晚；base 不为空时，
先从 base 序列结束时的历史拷贝一份再继续。每晚带原始输入（或直接给指标、或 RR 输入）、
症状，以及 Python 算出的夜间指标和评分结果。时间一律是墙上时间相对 1970-01-01 的整秒。
"""
import copy
import json
import sys
from dataclasses import replace
from datetime import date, datetime, timedelta

import numpy as np

if len(sys.argv) != 3:
    sys.exit(__doc__)
sys.path.insert(0, sys.argv[1])

from recovery import NightRaw, RecoveryEngine, process_night  # noqa: E402
from recovery.models import HRVSample, StageInterval  # noqa: E402
from recovery.rr_adapter import night_from_rr  # noqa: E402
from recovery.synthetic import make_night  # noqa: E402

EPOCH = datetime(1970, 1, 1)
D0 = date(2026, 9, 1)


def secs(dt):
    return int((dt - EPOCH).total_seconds())


def stage_out(v):
    return v if isinstance(v, str) else int(v)


def raw_to_json(raw):
    return {
        "night_date": raw.night_date.isoformat(),
        "in_bed_start": None if raw.in_bed_start is None else secs(raw.in_bed_start),
        "hrv_is_ln": bool(raw.hrv_is_ln),
        "sleep_score_external": raw.sleep_score_external,
        # 每条：t_start, rmssd, hr_bpm, resp_brpm, quality, motion, duration_sec
        "hrv": [[secs(s.t_start), s.rmssd, s.hr_bpm, s.resp_brpm, s.quality, s.motion, s.duration_sec]
                for s in raw.hrv_5min],
        "stages": [[secs(s.start), secs(s.end), stage_out(s.stage)] for s in raw.stages],
    }


def metrics_to_json(nm):
    d = dict(nm.__dict__)
    d["night_date"] = nm.night_date.isoformat()
    return d


class Seq:
    """一个序列：包着一台 Python 引擎，边算边记录。"""

    def __init__(self, name, base=None):
        self.name = name
        self.base = base.name if base else None
        self.eng = copy.deepcopy(base.eng) if base else RecoveryEngine()
        self.nights = []

    def _record(self, label, symptoms, entry, res):
        nm = next(r.night for r in self.eng.history if r.night.night_date == res.date)
        entry.update(label=label, symptoms=symptoms, metrics=metrics_to_json(nm), result=res.to_dict())
        self.nights.append(entry)
        return res

    def add(self, raw, symptoms="none", label=""):
        res = self.eng.add_night(raw, symptoms=symptoms)
        return self._record(label, symptoms, {"input": raw_to_json(raw)}, res)

    def add_metrics(self, nm, symptoms="none", label=""):
        res = self.eng.add_night(nm, symptoms=symptoms)
        return self._record(label, symptoms, {"metrics_input": metrics_to_json(nm)}, res)

    def add_rr(self, d, start, rr, hyp, resp=None, label=""):
        raw = night_from_rr(d, start, rr, hyp, resp_brpm=resp)
        res = self.eng.add_night(raw)
        entry = {"rr": {"night_date": d.isoformat(), "start": secs(start), "epoch_sec": 30,
                        "rr_ms": [float(x) for x in rr], "hyp": [int(x) for x in hyp],
                        "resp": None if resp is None else [None if np.isnan(x) else float(x) for x in resp]}}
        return self._record(label, "none", entry, res)

    def to_json(self):
        return {"name": self.name, "base": self.base, "nights": self.nights}


def normal_run(name, n, seed=7):
    """demo.py / tests 里建立基线的那段正常数据。"""
    s = Seq(name)
    rng = np.random.default_rng(seed)
    for i in range(n):
        s.add(make_night(D0 + timedelta(days=i), rng, tst_min=float(rng.normal(450, 20))))
    return s


def drop_hrv_metrics(raw, drop_rhr=False):
    nm = process_night(raw)[0]
    if drop_rhr:
        return replace(nm, recovery_hrv=None, early_hrv=None, late_hrv=None, rhr=None)
    return replace(nm, recovery_hrv=None, early_hrv=None, late_hrv=None, n_good_segments=0)


def demo_sequences(out):
    base = normal_run("demo_base40", 40)
    out.append(base)
    d = D0 + timedelta(days=40)
    scenarios = [
        ("场景1 正常高恢复", dict(ln_shift=0.15, hr_shift=-1.0), "none"),
        ("场景2 假性恢复", dict(ln_shift=0.10, hr_shift=3.0, resp_shift=1.0), "none"),
        ("场景3 典型疲劳", dict(ln_shift=-0.35, hr_shift=5.0), "none"),
        ("场景4 可能不适", dict(ln_shift=-0.25, hr_shift=7.0, resp_shift=3.0), "mild"),
        ("场景5 睡眠差生理正常", dict(tst_min=290, waso_bouts=3), "none"),
        ("场景6 后半夜恢复回来", dict(early_shift=-0.25, late_shift=0.10), "none"),
        ("场景7 后半夜仍未恢复", dict(early_shift=-0.15, late_shift=-0.45, hr_shift=4.0), "none"),
        ("边界 睡眠<3h", dict(tst_min=150), "none"),
        ("边界 脱戴过多", dict(wear_gap_frac=0.45), "none"),
    ]
    for i, (name, kw, sym) in enumerate(scenarios):
        s = Seq("demo_" + name, base)
        s.add(make_night(d, np.random.default_rng(100 + i), **kw), sym, name)
        out.append(s)

    s = Seq("demo_边界 HRV 缺失", base)
    s.add_metrics(drop_hrv_metrics(make_night(d, np.random.default_rng(150))), label="边界 HRV 缺失")
    out.append(s)

    s = Seq("demo_连续疲劳", base)
    for k in range(4):
        s.add(make_night(d + timedelta(days=k), np.random.default_rng(200 + k),
                         ln_shift=-0.08 * (k + 1), hr_shift=1.2 * (k + 1)), label=f"疲劳第{k + 1}晚")
    out.append(s)

    s = Seq("demo_疲劳后回升", base)
    for k, sh in enumerate([-0.30, -0.30, -0.25, -0.05, 0.05, 0.10, 0.12]):
        s.add(make_night(d + timedelta(days=k), np.random.default_rng(300 + k), ln_shift=sh,
                         hr_shift=2.0 if sh < -0.2 else 0.0), label=f"回升第{k + 1}晚")
    out.append(s)


def unit_test_sequences(out):
    """tests/test_recovery.py 里走引擎的用例。"""
    s = Seq("test_cold_start")
    rng = np.random.default_rng(3)
    for i in range(5):
        s.add(make_night(D0 + timedelta(days=i), rng))
    out.append(s)

    base = normal_run("test_base35", 35)
    out.append(base)
    dn = D0 + timedelta(days=35)

    def one(name, raw, sym="none"):
        q = Seq("test_" + name, base)
        q.add(raw, sym, name)
        out.append(q)

    one("normal_night", make_night(dn, np.random.default_rng(11)))
    one("fatigue_red", make_night(dn, np.random.default_rng(12), ln_shift=-0.35, hr_shift=5))
    one("false_recovery", make_night(dn, np.random.default_rng(13), ln_shift=0.1, hr_shift=3, resp_shift=1.0))
    one("sleep_too_short", make_night(dn, np.random.default_rng(14), tst_min=150))
    one("missing_resp", make_night(dn, np.random.default_rng(16), drop_resp=True))
    one("symptoms_moderate_severe", make_night(dn, np.random.default_rng(20)), "moderate_severe")
    one("missing_hr", make_night(dn, np.random.default_rng(6), drop_hr=True))
    one("drop_hrv", make_night(dn, np.random.default_rng(8), drop_hrv=True))
    one("drop_rr", make_night(dn, np.random.default_rng(8), drop_rr=True))
    one("text_stages", make_night(dn, np.random.default_rng(3), stage_labels="text"))

    q = Seq("test_missing_hrv", base)
    q.add_metrics(drop_hrv_metrics(make_night(dn, np.random.default_rng(15))))
    out.append(q)
    q = Seq("test_no_core_signal", base)
    q.add_metrics(drop_hrv_metrics(make_night(dn, np.random.default_rng(17)), drop_rhr=True))
    out.append(q)

    q = Seq("test_same_date_replaces", base)
    q.add(make_night(dn, np.random.default_rng(18)))
    q.add(make_night(dn, np.random.default_rng(19)))
    out.append(q)

    # lnRMSSD 输入
    raw = make_night(dn, np.random.default_rng(4))
    for x in raw.hrv_5min:
        x.rmssd = None if x.rmssd is None else float(np.log(x.rmssd))
    raw.hrv_is_ln = True
    one("ln_input", raw)

    # 尖刺和超范围窗口
    raw = make_night(dn, np.random.default_rng(5), artifact_rate=0.0)
    for k in (40, 51, 70):
        raw.hrv_5min[k].rmssd *= 4
    raw.hrv_5min[60].rmssd = 900.0
    one("outlier_windows", raw)

    # 记录和分期乱序、带重复时刻、带体动和超范围质量
    raw = make_night(dn, np.random.default_rng(21))
    rng = np.random.default_rng(22)
    raw.hrv_5min.append(copy.deepcopy(raw.hrv_5min[10]))
    raw.hrv_5min[-1].rmssd = 55.5
    for k in (20, 21, 30):
        raw.hrv_5min[k].motion = 0.8
    raw.hrv_5min[33].quality = 1.4
    raw.hrv_5min[34].quality = None
    raw.hrv_5min[35].hr_bpm = 140.0
    rng.shuffle(raw.hrv_5min)
    rng.shuffle(raw.stages)
    raw.in_bed_start = None
    one("shuffled_input", raw)

    # 外部睡眠分
    raw = make_night(dn, np.random.default_rng(23), tst_min=330, waso_bouts=2)
    raw.sleep_score_external = 48.0
    one("external_sleep_score", raw)

    # 手写分期区间（相邻清醒合并、文字别名）
    t0 = datetime(2026, 10, 5, 23, 0)
    st = [(0, 15, "wake"), (15, 200, "light"), (200, 210, "清醒"), (210, 213, "Awake"),
          (213, 400, "deep"), (400, 420, " REM "), (420, 430, "w")]
    raw = NightRaw(
        night_date=dn,
        hrv_5min=[HRVSample(t0 + timedelta(minutes=5 * i), 40.0, hr_bpm=55.0, resp_brpm=14.0) for i in range(86)],
        stages=[StageInterval(t0 + timedelta(minutes=a), t0 + timedelta(minutes=b), x) for a, b, x in st])
    one("hand_written_intervals", raw)

    # 没有任何记录 / 没有分期
    one("no_hrv_records", NightRaw(night_date=dn, hrv_5min=[], stages=raw.stages))
    one("no_stages", NightRaw(night_date=dn, hrv_5min=raw.hrv_5min, stages=[]))


def rr_sequences(out):
    t0 = datetime(2026, 8, 31, 23, 0)
    hyp = np.r_[[0] * 20, [2] * 600, [3] * 200, [4] * 100, [0] * 10]
    s = Seq("rr_roundtrip")
    rng = np.random.default_rng(9)
    rr = rng.normal(1000, 30, int(len(hyp) * 30))
    s.add_rr(D0, t0, rr, hyp, label="干净的 RR")

    # 带伪迹、带呼吸率、中间丢一段
    rng = np.random.default_rng(10)
    rr = rng.normal(950, 35, 31000)
    bad = rng.choice(len(rr), 900, replace=False)
    rr[bad] *= rng.choice([0.4, 1.7, 2.5], len(bad))
    rr[12000:12600] = 2600.0
    n_win = int(np.ceil(len(hyp) * 30 / 300))
    resp = [float("nan") if i % 17 == 0 else float(14 + rng.normal(0, 0.3)) for i in range(n_win)]
    s.add_rr(D0 + timedelta(days=1), t0 + timedelta(days=1), rr, hyp, resp, label="带伪迹的 RR")
    out.append(s)


def random_sequence(name, seed, n_days):
    """随机长序列：正常夜里掺各种事件、症状和缺测日，用来覆盖基线排除、新常态、趋势、不适风险。"""
    s = Seq(name)
    rng = np.random.default_rng(seed)
    ctl = np.random.default_rng(seed + 1000)
    for i in range(n_days):
        d = D0 + timedelta(days=i)
        if ctl.random() < 0.06:
            continue
        kw = dict(tst_min=float(ctl.normal(440, 45)))
        sym = "none"
        ext = None
        u = ctl.random()
        if u < 0.06:
            kw.update(ln_shift=-0.30, hr_shift=4.0)
        elif u < 0.11:
            kw.update(ln_shift=-0.25, hr_shift=6.0, resp_shift=2.5)
            sym = ["none", "mild", "moderate_severe"][int(ctl.integers(0, 3))]
        elif u < 0.16:
            kw.update(tst_min=float(ctl.uniform(170, 320)), waso_bouts=3)
        elif u < 0.19:
            kw.update(drop_resp=True)
        elif u < 0.22:
            kw.update(drop_hr=True)
        elif u < 0.24:
            kw.update(drop_hrv=True)
        elif u < 0.27:
            kw.update(wear_gap_frac=float(ctl.choice([0.15, 0.25, 0.45])))
        elif u < 0.30:
            ext = float(round(ctl.uniform(30, 95), 1))
        elif u < 0.34:
            kw.update(stage_labels="text")
        elif u < 0.38:
            kw.update(early_shift=-0.25, late_shift=float(ctl.choice([0.10, -0.45])))
        elif u < 0.42:
            kw.update(ln_shift=0.15, hr_shift=-1.0)
        elif u < 0.45:
            kw.update(artifact_rate=0.15)
        raw = make_night(d, rng, **kw)
        raw.sleep_score_external = ext
        s.add(raw, sym)
    return s


def new_normal_sequence():
    """30 晚正常后生理水平整体平移 14 晚：先连续离群，满 7 晚后按新常态重新纳入基线。"""
    s = Seq("new_normal_shift")
    rng = np.random.default_rng(31)
    for i in range(44):
        kw = dict(ln_shift=-0.6, hr_shift=8.0) if i >= 30 else {}
        s.add(make_night(D0 + timedelta(days=i), rng, **kw))
    return s


def main():
    out = []
    demo_sequences(out)
    unit_test_sequences(out)
    rr_sequences(out)
    out.append(random_sequence("random_seed42", 42, 120))
    out.append(random_sequence("random_seed43", 43, 120))
    out.append(new_normal_sequence())

    with open(sys.argv[1] + "/examples/night_input_example.json", encoding="utf-8") as f:
        s = Seq("example_json")
        s.add(NightRaw.from_dict(json.load(f)))
        out.append(s)

    doc = {"reference": "recovery 1.0.2 (Python)", "sequences": [s.to_json() for s in out]}
    with open(sys.argv[2], "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, separators=(",", ":"))
    n_nights = sum(len(s.nights) for s in out)
    print(f"{len(out)} 个序列，{n_nights} 晚 → {sys.argv[2]}")


if __name__ == "__main__":
    main()
