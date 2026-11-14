"""单元测试：python3 -m pytest -q tests  或  python3 -m unittest discover tests"""
import copy
import json
import os
import sys
import unittest
from dataclasses import fields, replace
from datetime import date, datetime, timedelta

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from recovery import Config, NightMetrics, NightRaw, RecoveryEngine, process_night  # noqa: E402
from recovery.baseline import build_baseline  # noqa: E402
from recovery.models import DayRecord, HRVSample, RecoveryResult, StageInterval  # noqa: E402
from recovery.scoring import (evaluate_gates, hrv_subscore, late_adjustment, resp_penalty,  # noqa: E402
                              resp_severity, rhr_penalty, tier_of)
from recovery.synthetic import make_night  # noqa: E402
from recovery.utils import round_half_up, weighted_median  # noqa: E402

CFG = Config()
D0 = date(2026, 9, 1)

_ENGINE = None


def base_engine():
    global _ENGINE
    if _ENGINE is None:
        rng = np.random.default_rng(7)
        e = RecoveryEngine()
        for i in range(35):
            e.add_night(make_night(D0 + timedelta(days=i), rng, tst_min=float(rng.normal(450, 20))))
        _ENGINE = e
    return copy.deepcopy(_ENGINE)


DNEXT = D0 + timedelta(days=35)


class TestUtils(unittest.TestCase):
    def test_round_half_up(self):
        self.assertEqual(round_half_up(66.5), 67)
        self.assertEqual(round_half_up(66.49), 66)
        self.assertEqual(round_half_up(0.5), 1)

    def test_weighted_median(self):
        self.assertEqual(weighted_median([1, 2, 3], [1, 1, 1]), 2)
        self.assertEqual(weighted_median([1, 2, 3], [0, 0, 1]), 3)


class TestNoTemperature(unittest.TestCase):
    def test_no_temperature_or_spo2_inputs(self):
        names = [f.name for cls in (NightRaw, NightMetrics, Config) for f in fields(cls)]
        for n in names:
            low = n.lower()
            self.assertNotIn("temp", low)
            self.assertNotIn("spo2", low)
            self.assertNotIn("load", low)


class TestSubscores(unittest.TestCase):
    def test_hrv_cap_95(self):
        self.assertEqual(hrv_subscore(5.0, CFG), 95.0)
        self.assertEqual(hrv_subscore(0.0, CFG), 70.0)
        self.assertEqual(hrv_subscore(-10, CFG), 0.0)

    def test_rhr_penalty_continuous_monotone(self):
        xs = np.linspace(-2, 6, 801)
        p = [rhr_penalty(x, CFG) for x in xs]
        self.assertTrue(all(b >= a - 1e-9 for a, b in zip(p, p[1:])))
        self.assertLess(max(abs(b - a) for a, b in zip(p, p[1:])), 0.2)  # 无台阶
        self.assertAlmostEqual(rhr_penalty(1.5, CFG), 8.0)
        self.assertEqual(rhr_penalty(10, CFG), 35.0)

    def test_resp_penalty_continuous_monotone(self):
        xs = np.linspace(0, 6, 601)
        p = [resp_penalty(x, CFG) for x in xs]
        self.assertTrue(all(b >= a - 1e-9 for a, b in zip(p, p[1:])))
        self.assertLess(max(abs(b - a) for a, b in zip(p, p[1:])), 0.2)

    def test_resp_severity_uses_absolute_delta(self):
        # z 很小但绝对升幅 3 brpm → 等效 z = 3
        self.assertAlmostEqual(resp_severity(0.2, 3.0, CFG), 3.0)
        self.assertAlmostEqual(resp_severity(2.5, 0.0, CFG), 2.5)
        self.assertIsNone(resp_severity(None, None, CFG))

    def test_late_adjustment_exclusive_and_bounded(self):
        self.assertAlmostEqual(late_adjustment(0, -0.5, 0, 0, CFG), 0.0)
        self.assertAlmostEqual(late_adjustment(0, -3, 0, 0, CFG), -6.0)
        self.assertGreater(late_adjustment(-1.0, 0.5, 0, 0, CFG), 0)
        self.assertLessEqual(late_adjustment(-3.0, 3.0, 0, 0, CFG), 3.0)
        # 回升但 RHR 高 → 不给加分
        self.assertEqual(late_adjustment(-1.0, 0.5, 1.0, 0, CFG), 0.0)


class TestGatesAndTiers(unittest.TestCase):
    def test_tier_boundaries(self):
        self.assertEqual(tier_of(100, CFG), ("green", "peak"))
        self.assertEqual(tier_of(85, CFG), ("green", "peak"))
        self.assertEqual(tier_of(84, CFG), ("green", "green"))
        self.assertEqual(tier_of(67, CFG), ("green", "green"))
        self.assertEqual(tier_of(66, CFG), ("yellow", "high_yellow"))
        self.assertEqual(tier_of(50, CFG), ("yellow", "high_yellow"))
        self.assertEqual(tier_of(49, CFG), ("yellow", "low_yellow"))
        self.assertEqual(tier_of(34, CFG), ("yellow", "low_yellow"))
        self.assertEqual(tier_of(33, CFG), ("red", "red"))
        self.assertEqual(tier_of(0, CFG), ("red", "red"))

    def test_caps_match_bands(self):
        self.assertEqual(tier_of(int(CFG.cap_yellow), CFG)[0], "yellow")
        self.assertEqual(tier_of(int(CFG.cap_red), CFG)[0], "red")

    def test_clean_night_has_no_gate(self):
        g = evaluate_gates({"hrv": 0.2, "late": 0.1, "rhr": -0.2}, 0.3, 85, 450, "none", "high", "none", CFG)
        self.assertEqual(g, [])

    def test_false_recovery_never_green(self):
        g = evaluate_gates({"hrv": 1.0, "late": 0.8, "rhr": 1.6}, 2.2, 85, 450, "none", "high", "none", CFG)
        self.assertTrue(g)
        self.assertLessEqual(min(x["cap"] for x in g), 66)

    def test_red_rules(self):
        g = evaluate_gates({"hrv": -1.8, "late": -1.0, "rhr": 1.3}, 0.5, 80, 450, "none", "high", "none", CFG)
        self.assertIn("R_HRV_COMBO", [x["code"] for x in g])
        g = evaluate_gates({"hrv": 0, "late": 0, "rhr": 0}, 0, 80, 450, "moderate_severe", "high", "none", CFG)
        self.assertEqual(min(x["cap"] for x in g), 33)

    def test_missing_resp_drops_resp_gates(self):
        g = evaluate_gates({"hrv": 0.1, "late": 0.1, "rhr": 0.1}, None, 85, 450, "none", "high", "none", CFG)
        self.assertEqual(g, [])


class TestNightProcessing(unittest.TestCase):
    def test_metrics_reasonable(self):
        nm, segs = process_night(make_night(D0, np.random.default_rng(1)))
        self.assertTrue(nm.valid_night)
        self.assertTrue(3.5 < nm.recovery_hrv < 4.5)
        self.assertTrue(48 < nm.rhr < 60)
        self.assertTrue(13 < nm.resp < 16)
        self.assertAlmostEqual(nm.tst_min, 450, delta=1)

    def test_short_sleep_invalid(self):
        nm, _ = process_night(make_night(D0, np.random.default_rng(1), tst_min=150))
        self.assertFalse(nm.valid_night)
        self.assertEqual(nm.invalid_reason, "sleep_too_short")


class TestEngine(unittest.TestCase):
    def test_cold_start(self):
        e = RecoveryEngine()
        rng = np.random.default_rng(3)
        states = [e.add_night(make_night(D0 + timedelta(days=i), rng)).state for i in range(5)]
        self.assertEqual(states[:3], ["calibrating"] * 3)
        self.assertEqual(states[3], "scored")
        r4 = e.history[3].result
        self.assertEqual(r4.confidence, "low")
        self.assertLessEqual(r4.score, 66)

    def test_normal_night_green_integer(self):
        r = base_engine().add_night(make_night(DNEXT, np.random.default_rng(11)))
        self.assertIsInstance(r.score, int)
        self.assertTrue(0 <= r.score <= 100)
        self.assertEqual(r.color, "green")
        self.assertEqual(r.confidence, "high")
        self.assertAlmostEqual(sum(r.weights_used.values()), 1.0, places=2)

    def test_fatigue_red(self):
        r = base_engine().add_night(make_night(DNEXT, np.random.default_rng(12), ln_shift=-0.35, hr_shift=5))
        self.assertEqual(r.color, "red")
        self.assertLessEqual(r.score, 33)

    def test_false_recovery_capped(self):
        r = base_engine().add_night(make_night(DNEXT, np.random.default_rng(13), ln_shift=0.1,
                                               hr_shift=3, resp_shift=1.0))
        self.assertNotEqual(r.color, "green")

    def test_sleep_too_short_no_score(self):
        r = base_engine().add_night(make_night(DNEXT, np.random.default_rng(14), tst_min=150))
        self.assertEqual(r.state, "sleep_too_short")
        self.assertIsNone(r.score)
        self.assertIsNone(r.color)

    def test_missing_hrv_redistributes(self):
        nm = process_night(make_night(DNEXT, np.random.default_rng(15)))[0]
        nm = replace(nm, recovery_hrv=None, early_hrv=None, late_hrv=None, n_good_segments=0)
        r = base_engine().add_night(nm)
        self.assertEqual(r.state, "scored")
        self.assertNotIn("hrv", r.weights_used)
        self.assertAlmostEqual(sum(r.weights_used.values()), 1.0, places=2)
        self.assertEqual(r.confidence, "low")

    def test_missing_resp(self):
        r = base_engine().add_night(make_night(DNEXT, np.random.default_rng(16), drop_resp=True))
        self.assertEqual(r.state, "scored")
        self.assertNotIn("resp", r.weights_used)
        self.assertNotEqual(r.confidence, "high")

    def test_no_core_signal(self):
        nm = process_night(make_night(DNEXT, np.random.default_rng(17)))[0]
        nm = replace(nm, recovery_hrv=None, early_hrv=None, late_hrv=None, rhr=None)
        r = base_engine().add_night(nm)
        self.assertEqual(r.state, "no_core_signal")

    def test_baseline_uses_only_past(self):
        e = base_engine()
        h = e.history
        b1 = build_baseline(h, D0 + timedelta(days=20), CFG)
        b2 = build_baseline(h[:20], D0 + timedelta(days=20), CFG)
        self.assertAlmostEqual(b1.get("recovery_hrv").median, b2.get("recovery_hrv").median)

    def test_new_normal_readmission(self):
        h = base_engine().history
        for r in h[-8:]:
            r.result.baseline_outlier = True
        b = build_baseline(h, DNEXT, CFG)
        self.assertEqual(b.readmitted, 8)
        for r in h[-3:]:
            r.result.baseline_outlier = True
        h2 = copy.deepcopy(h)
        for r in h2[-8:-3]:
            r.result.baseline_outlier = False
        self.assertEqual(build_baseline(h2, DNEXT, CFG).readmitted, 0)

    def test_reprocessing_same_date_replaces(self):
        e = base_engine()
        n = len(e.history)
        e.add_night(make_night(DNEXT, np.random.default_rng(18)))
        e.add_night(make_night(DNEXT, np.random.default_rng(19)))
        self.assertEqual(len(e.history), n + 1)

    def test_trend_bounds_and_symptoms(self):
        r = base_engine().add_night(make_night(DNEXT, np.random.default_rng(20)), symptoms="moderate_severe")
        self.assertEqual(r.color, "red")
        self.assertTrue(CFG.trend_min <= r.trend_adjustment <= CFG.trend_max)


class TestInputV102(unittest.TestCase):
    """V1.0.2 输入：5 分钟 HRV + 带时间的睡眠分期。"""
    T0 = datetime(2026, 8, 31, 23, 0)

    def _night(self, stages, rmssd=40.0, hr=55.0, n=None, **kw):
        n = n or int((stages[-1][1] - stages[0][0]) / 5)
        samples = [HRVSample(self.T0 + timedelta(minutes=5 * i), rmssd, hr_bpm=hr, resp_brpm=14.0)
                   for i in range(n)]
        iv = [StageInterval(self.T0 + timedelta(minutes=a), self.T0 + timedelta(minutes=b), st)
              for a, b, st in stages]
        return NightRaw(night_date=D0, hrv_5min=samples, stages=iv, **kw)

    def test_sleep_metrics_from_intervals(self):
        st = [(0, 15, "wake"), (15, 200, "light"), (200, 210, "wake"), (210, 213, "wake"),
              (213, 400, "deep"), (400, 420, "rem"), (420, 430, "wake")]
        nm, _ = process_night(self._night(st))
        self.assertAlmostEqual(nm.tst_min, 185 + 187 + 20, places=6)
        self.assertAlmostEqual(nm.waso_min, 13, places=6)
        self.assertEqual(nm.long_awakenings, 1)           # 两段相邻清醒合并为一次 13 分钟夜醒
        self.assertAlmostEqual(nm.efficiency, 392 / 420, places=6)

    def test_string_and_code_stages_equivalent(self):
        a = process_night(make_night(D0, np.random.default_rng(3)))[0]
        b = process_night(make_night(D0, np.random.default_rng(3), stage_labels="text"))[0]
        self.assertAlmostEqual(a.tst_min, b.tst_min)
        self.assertTrue(b.valid_night and b.recovery_hrv is not None)

    def test_ln_input_equivalent(self):
        raw = make_night(D0, np.random.default_rng(4))
        ln_raw = copy.deepcopy(raw)
        for x in ln_raw.hrv_5min:
            x.rmssd = None if x.rmssd is None else float(np.log(x.rmssd))
        ln_raw.hrv_is_ln = True
        self.assertAlmostEqual(process_night(raw)[0].recovery_hrv, process_night(ln_raw)[0].recovery_hrv,
                               places=9)

    def test_outlier_and_implausible_windows_ignored(self):
        raw = make_night(D0, np.random.default_rng(5), artifact_rate=0.0)
        base = process_night(raw)[0].recovery_hrv
        bad = copy.deepcopy(raw)
        for k in (40, 41 + 10, 70):
            bad.hrv_5min[k].rmssd = bad.hrv_5min[k].rmssd * 4      # 单点尖刺
        bad.hrv_5min[60].rmssd = 900.0                             # 超出合理范围
        nm, segs = process_night(bad)
        self.assertAlmostEqual(nm.recovery_hrv, base, delta=0.03)
        self.assertEqual(segs[40].w_quality, 0.0)
        self.assertIsNone(segs[60].ln_rmssd)

    def test_missing_hr_keeps_hrv(self):
        nm = process_night(make_night(D0, np.random.default_rng(6), drop_hr=True))[0]
        self.assertIsNone(nm.rhr)
        self.assertIsNotNone(nm.recovery_hrv)
        r = base_engine().add_night(make_night(DNEXT, np.random.default_rng(6), drop_hr=True))
        self.assertEqual(r.state, "scored")
        self.assertNotIn("rhr", {k for k, v in r.weights_used.items() if v > 0})

    def test_wear_gap_invalid(self):
        nm = process_night(make_night(D0, np.random.default_rng(7), wear_gap_frac=0.45))[0]
        self.assertFalse(nm.valid_night)
        self.assertEqual(nm.invalid_reason, "low_wear_coverage")

    def test_drop_hrv_no_core_signal(self):
        nm = process_night(make_night(D0, np.random.default_rng(8), drop_hrv=True))[0]
        self.assertIsNone(nm.recovery_hrv)

    def test_from_dict_json(self):
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                            "examples", "night_input_example.json")
        with open(path, encoding="utf-8") as f:
            raw = NightRaw.from_dict(json.load(f))
        nm, _ = process_night(raw)
        self.assertGreater(nm.tst_min, 0)
        self.assertIsNotNone(nm.recovery_hrv)

    def test_rr_adapter_roundtrip(self):
        from recovery.rr_adapter import night_from_rr
        rng = np.random.default_rng(9)
        hyp = np.r_[[0] * 20, [2] * 600, [3] * 200, [4] * 100, [0] * 10]
        rr = rng.normal(1000, 30, int(len(hyp) * 30 * 1.0))
        raw = night_from_rr(D0, self.T0, rr, hyp)
        nm, _ = process_night(raw)
        self.assertAlmostEqual(nm.tst_min, 450, delta=0.01)
        self.assertAlmostEqual(nm.rhr, 60, delta=2)
        self.assertAlmostEqual(nm.recovery_hrv, np.log(30 * np.sqrt(2)), delta=0.1)


if __name__ == "__main__":
    unittest.main()
