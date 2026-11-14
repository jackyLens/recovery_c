"""演示：40 晚正常数据建立基线后，跑 7 个典型场景 + 边界情况。运行：python3 demo.py"""
import copy
import json
from datetime import date, timedelta

import numpy as np

from recovery import RecoveryEngine
from recovery.synthetic import make_night

COLOR = {"green": "Green", "yellow": "Yellow", "red": "Red", None: "—"}
START = date(2026, 9, 1)


def build_engine(n=40, seed=7):
    rng = np.random.default_rng(seed)
    eng = RecoveryEngine()
    first = []
    for i in range(n):
        d = START + timedelta(days=i)
        r = eng.add_night(make_night(d, rng, tst_min=float(rng.normal(450, 20))))
        if i < 4:
            first.append((d, r))
    return eng, rng, first


def line(name, r):
    s = "—" if r.score is None else f"{r.score:>3d}"
    gates = ",".join(g["code"] for g in r.gates) or "-"
    print(f"{name:<18}{s:>5}  {COLOR[r.color]:<7}{r.state:<17}{str(r.confidence):<7}"
          f"趋势{r.trend_adjustment:+5.1f}  不适:{r.illness_level:<8} 门控:{gates}")
    if r.summary:
        print(f"{'':<20}{r.summary}")
        if r.illness_message:
            print(f"{'':<20}[提示] {r.illness_message}")


SCENARIOS = [
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


def main():
    eng, rng, first = build_engine()
    out = {}
    print("=== 冷启动（前 4 晚）===")
    for d, r in first:
        line(d.isoformat(), r)
        out[f"冷启动 {d.isoformat()}"] = r.to_dict()
    last = eng.history[-1].result
    print(f"\n=== 第 40 晚（基线 {last.baseline_stage}，{last.baseline_nights} 晚）===")
    line("正常夜", last)

    print("\n=== 典型场景（各自独立接在 40 晚正常数据之后）===")
    d = START + timedelta(days=40)
    for i, (name, kw, sym) in enumerate(SCENARIOS):
        e = copy.deepcopy(eng)
        r = e.add_night(make_night(d, np.random.default_rng(100 + i), **kw), symptoms=sym)
        line(name, r)
        out[name] = r.to_dict()

    # HRV 缺失但心率可用（例如 RR 质量差）：权重按比例重新分配，置信度降为 low
    from dataclasses import replace
    from recovery.night import process_night
    nm = process_night(make_night(d, np.random.default_rng(150)))[0]
    nm = replace(nm, recovery_hrv=None, early_hrv=None, late_hrv=None, n_good_segments=0)
    r = copy.deepcopy(eng).add_night(nm)
    line("边界 HRV 缺失", r)
    out["边界 HRV 缺失"] = r.to_dict()

    print("\n=== 连续 4 晚逐步疲劳（看趋势修正）===")
    e = copy.deepcopy(eng)
    for k in range(4):
        r = e.add_night(make_night(d + timedelta(days=k), np.random.default_rng(200 + k),
                                   ln_shift=-0.08 * (k + 1), hr_shift=1.2 * (k + 1)))
        line(f"疲劳第{k + 1}晚", r)
        out[f"连续疲劳第{k + 1}晚"] = r.to_dict()

    print("\n=== 疲劳后连续回升（看正向趋势）===")
    e = copy.deepcopy(eng)
    shifts = [-0.30, -0.30, -0.25, -0.05, 0.05, 0.10, 0.12]
    for k, s in enumerate(shifts):
        r = e.add_night(make_night(d + timedelta(days=k), np.random.default_rng(300 + k), ln_shift=s,
                                   hr_shift=2.0 if s < -0.2 else 0.0))
        line(f"回升第{k + 1}晚", r)
        out[f"回升第{k + 1}晚"] = r.to_dict()

    with open("demo_results.json", "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=1, default=str)


if __name__ == "__main__":
    main()
