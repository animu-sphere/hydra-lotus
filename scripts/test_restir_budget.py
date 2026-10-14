"""Independent reducer fixtures for repeated static quality budgets."""

import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from restir_budget import checkpoint, summarize, validate_curve


def report(seed=17):
    def curve(errors, times):
        return [{"samples": 2**i, "rmse": e, "rmse_raw": [e, e],
                 "cumulative_gpu_ms": t, "cumulative_gpu_ms_raw": [t, t] if t else None}
                for i, (e, t) in enumerate(zip(errors, times))]

    def case(mode, errors, times):
        return {"mode": mode, "candidates": 0 if mode == "mis" else 8,
                "convergence": curve(errors, times), "fresh_rmse": {"mean": 2},
                "residual_frame_delta_rmse": {"mean": 3}, "ensemble_rmse": .1,
                "max_abs_tile_mean_error": .2, "signed_mean_error_rgb": [.01, -.02, .03]}

    return {"schema": "lotus-restir-benchmark-v1", "status": "pass", "build": {"label": "fixture"},
            "device": {"name": "fixture"},
            "procedure": {"sample_index": seed, "sequences": 2, "sequence_stride": 65537, "frames": 4},
            "scenes": [{"id": "static", "moving": False, "cases": [case("mis", [1, .5, .25], [1, 2, 4]),
                                                     case("initial", [.5, .25, .125], [1.5, 3, 6])]}]}


class BudgetTests(unittest.TestCase):
    def test_observed_paired_budgets(self):
        result = summarize([report(), report(17 + 2 * 65537)], [.25, .1])
        budgets = result["scenes"][0]["cases"][1]["quality_budgets"]
        self.assertEqual(budgets[0]["samples"], [2, 2])
        self.assertEqual(budgets[0]["cumulative_gpu_ms"], [3, 3])
        self.assertEqual(budgets[0]["paired_mis_speedup"], [4/3, 4/3])
        self.assertEqual(budgets[1]["reached_runs"], 0)
        self.assertIsNone(budgets[1]["speedup_summary"])

    def test_recrossing_requires_later_sustained_checkpoint(self):
        points = [{"rmse": x} for x in [.1, .4, .2, .1]]
        self.assertIs(checkpoint(points, .2), points[2])
        self.assertIsNone(checkpoint(points, .05))

    def test_aggregate_uses_rms_of_sequence_errors(self):
        r = report()
        point = r["scenes"][0]["cases"][0]["convergence"][-1]
        point["rmse_raw"] = [.3, .4]
        point["rmse"] = (.125)**.5
        validate_curve(r["scenes"][0]["cases"][0]["convergence"], r["procedure"])
        point["rmse"] = .35
        with self.assertRaisesRegex(ValueError, "RMS"):
            validate_curve(r["scenes"][0]["cases"][0]["convergence"], r["procedure"])

    def test_rejects_reused_or_overlapping_seed_ranges(self):
        for seed in [17, 18, 65554]:
            with self.subTest(seed=seed), self.assertRaisesRegex(ValueError, "overlapping"):
                summarize([report(), report(seed)], [.25])

    def test_rejects_unmatched_or_unsuccessful_measurements(self):
        for key, value in [("build", {}), ("device", {}), ("status", "skip")]:
            other = report(262165)
            other[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                summarize([report(), other], [.25])
        other = report(262165)
        other["procedure"]["frames"] = 8
        with self.assertRaisesRegex(ValueError, "procedure"):
            summarize([report(), other], [.25])

    def test_unavailable_timestamps_retain_quality(self):
        reports = [report(), report(262165)]
        for r in reports:
            for c in r["scenes"][0]["cases"]:
                for p in c["convergence"]:
                    p["cumulative_gpu_ms"] = p["cumulative_gpu_ms_raw"] = None
        result = summarize(reports, [.25])["scenes"][0]["cases"][1]["quality_budgets"][0]
        self.assertEqual(result["reached_runs"], 2)
        self.assertIsNone(result["gpu_ms_summary"])
        self.assertEqual(result["paired_mis_speedup"], [None, None])

    def test_partial_attainment_does_not_average_successful_subset(self):
        reports = [report(), report(262165)]
        for p in reports[1]["scenes"][0]["cases"][1]["convergence"]:
            p["rmse"] = 1
            p["rmse_raw"] = [1, 1]
        result = summarize(reports, [.25])["scenes"][0]["cases"][1]["quality_budgets"][0]
        self.assertEqual(result["reached_runs"], 1)
        self.assertIsNone(result["gpu_ms_summary"])
        self.assertIsNone(result["speedup_summary"])

    def test_moving_poses_never_get_static_budget(self):
        reports = [report(), report(262165)]
        for r in reports:
            moving = copy.deepcopy(r["scenes"][0])
            moving["id"] = "moving"
            moving["moving"] = True
            for c in moving["cases"]:
                c["convergence"] = None
            r["scenes"].append(moving)
        self.assertIsNone(summarize(reports, [.25])["scenes"][1]["cases"][0]["quality_budgets"])

    def test_rejects_static_curve_for_moving_workload(self):
        reports = [report(), report(262165)]
        for r in reports:
            r["scenes"][0]["moving"] = True
        with self.assertRaisesRegex(ValueError, "static/moving"):
            summarize(reports, [.25])

    def test_raw_populations_and_finite_metrics_are_checked(self):
        for field, value in [("rmse", float("nan")), ("rmse_raw", [.1, .9]),
                             ("cumulative_gpu_ms_raw", [1]), ("samples", 5)]:
            reports = [report(), report(262165)]
            reports[1]["scenes"][0]["cases"][0]["convergence"][0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                summarize(reports, [.25])
        for target in [0, -1, float("inf")]:
            with self.subTest(target=target), self.assertRaises(ValueError):
                summarize([report(), report(262165)], [target])

    def test_cli_preserves_output_on_invalid_or_overlapping_input(self):
        with tempfile.TemporaryDirectory(prefix="lotus-budget-", dir=Path(__file__).resolve().parent) as directory:
            root = Path(directory)
            a, b, output = (root / name for name in ("a.json", "b.json", "out.json"))
            a.write_text(json.dumps(report()), encoding="utf-8")
            b.write_text(json.dumps(report(262165)), encoding="utf-8")
            command = [sys.executable, str(Path(__file__).with_name("restir_budget.py")),
                       str(a), str(b), "--rmse", ".25", "--output", str(output)]
            success = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(success.returncode, 0, success.stderr)
            self.assertEqual(json.loads(output.read_text())["run_seeds"], [17, 262165])
            previous = output.read_bytes()
            b.write_text(json.dumps(report()), encoding="utf-8")
            failure = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(failure.returncode, 0)
            self.assertIn("overlapping", failure.stderr)
            self.assertEqual(output.read_bytes(), previous)
            b.write_text("invalid JSON", encoding="utf-8")
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)
            self.assertEqual(output.read_bytes(), previous)


if __name__ == "__main__":
    unittest.main()
