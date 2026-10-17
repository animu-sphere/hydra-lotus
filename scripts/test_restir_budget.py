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
    def test_transport_depth_and_reference_seed_must_match(self):
        for field, original, changed in (("max_bounces", 1, 4),
                                         ("reference_seed", 16777216, 18874368)):
            reports = [report(), report(262165)]
            for run in reports:
                run["procedure"][field] = original
            self.assertEqual(summarize(reports, [.25])["procedure"][field], original)
            reports[1]["procedure"][field] = changed
            with self.assertRaisesRegex(ValueError, "procedure must match"):
                summarize(reports, [.25])

    @staticmethod
    def with_temporal_residuals():
        reports = [report(), report(262165)]
        for run in reports:
            for case in run["scenes"][0]["cases"]:
                mean = case["convergence"][-1]["rmse"]
                centered = .5 if case["mode"] == "mis" else 0
                fresh = (mean*mean + centered*centered)**.5
                case["fresh_rmse"] = {"mean": fresh, "raw": [fresh] * 8}
                case["temporal_residuals"] = {
                    "sequence_mean_rmse": {"mean": mean, "raw": [mean, mean]},
                    "centered_rmse": {"mean": centered, "raw": [centered, centered]},
                    "lag1_autocorrelation_raw": [-.75, .25] if centered else [None, None]}
        return reports

    def test_temporal_residuals_preserve_run_sequence_and_undefined_values(self):
        result = summarize(self.with_temporal_residuals(), [.25])["scenes"][0]["cases"]
        self.assertEqual(result[0]["temporal_residuals"]["sequence_mean_rmse"], [[.25, .25], [.25, .25]])
        self.assertEqual(result[0]["temporal_residuals"]["lag1_autocorrelation_raw"], [[-.75, .25], [-.75, .25]])
        self.assertEqual(result[1]["temporal_residuals"]["lag1_autocorrelation_raw"], [[None, None], [None, None]])
        legacy = summarize([report(), report(262165)], [.25])
        self.assertIsNone(legacy["scenes"][0]["cases"][0]["temporal_residuals"])

    def test_temporal_residuals_reject_bad_populations_correlations_and_decomposition(self):
        for metric, invalid in (
            ("sequence_mean_rmse", {"mean": .25, "raw": [.25]}),
            ("centered_rmse", {"mean": .5, "raw": [float("nan"), .5]}),
            ("centered_rmse", {"mean": .4, "raw": [.5, .5]}),
            ("centered_rmse", {"mean": .4, "raw": [.4, .4]}),
            ("lag1_autocorrelation_raw", [.25]),
            ("lag1_autocorrelation_raw", [None, .25]),
            ("lag1_autocorrelation_raw", [1.1, .25]),
            ("lag1_autocorrelation_raw", [float("inf"), .25]),
            ("lag1_autocorrelation_raw", [True, .25]),
        ):
            with self.subTest(metric=metric, invalid=invalid):
                reports = self.with_temporal_residuals()
                reports[1]["scenes"][0]["cases"][0]["temporal_residuals"][metric] = invalid
                with self.assertRaises(ValueError):
                    summarize(reports, [.25])
        reports = self.with_temporal_residuals()
        del reports[1]["scenes"][0]["cases"][0]["temporal_residuals"]
        with self.assertRaisesRegex(ValueError, "every run"):
            summarize(reports, [.25])
        reports = self.with_temporal_residuals()
        reports[1]["scenes"][0]["cases"][1]["temporal_residuals"]["lag1_autocorrelation_raw"] = [0, 0]
        with self.assertRaisesRegex(ValueError, "zero-variance"):
            summarize(reports, [.25])

    def test_temporal_residuals_check_each_sequence_and_static_convergence(self):
        reports = self.with_temporal_residuals()
        case = reports[1]["scenes"][0]["cases"][0]
        case["fresh_rmse"]["raw"] = [0] * 4 + [(.625)**.5] * 4
        # Same aggregate MSE as before, but wrong per-sequence decomposition.
        with self.assertRaisesRegex(ValueError, "decomposition"):
            summarize(reports, [.25])
        reports = self.with_temporal_residuals()
        case = reports[1]["scenes"][0]["cases"][0]
        case["temporal_residuals"]["sequence_mean_rmse"] = {"mean": .5, "raw": [.5, .5]}
        case["fresh_rmse"]["raw"] = [(.5)**.5] * 8
        with self.assertRaisesRegex(ValueError, "static convergence"):
            summarize(reports, [.25])
        # Moving residual means do not equal a final-pose convergence curve.
        for run in reports:
            run["scenes"][0]["moving"] = True
            for case in run["scenes"][0]["cases"]:
                case["convergence"] = None
        summarize(reports, [.25])

    @staticmethod
    def with_sequence_ensemble(split=None):
        reports = [report(), report(262165)]
        for run in reports:
            scene = run["scenes"][0]
            if split is not None:
                scene["error_reference"] = "finite-spp-nee-mis"
                scene["reference_split_rmse"] = [split]
            reference = 0 if split is None else split * split / 4
            for case in scene["cases"]:
                # ensemble_rmse .1: grand mean MSE .01, frames 4, sequences 2.
                inflation = 1.0 if case["mode"] == "mis" else 2.0
                case["sequence_ensemble"] = {
                    "sequences": 2, "frames": 4, "frame_variance": .02,
                    "sequence_mean_variance": .005 * inflation, "variance_inflation": inflation,
                    "effective_frames": 4 / inflation, "grand_mean_mse": .01,
                    "grand_mean_noise_mse": .0025 * inflation, "reference_noise_mse": reference,
                    "noise_normalized_mse": .01 / (.0025 * inflation + reference),
                    "excess_mse": .01 - .0025 * inflation - reference}
        return reports

    def test_sequence_ensemble_survives_reduction(self):
        for split in (None, .04):
            with self.subTest(split=split):
                cases = summarize(self.with_sequence_ensemble(split), [.25])["scenes"][0]["cases"]
                self.assertEqual(cases[1]["sequence_ensemble"]["variance_inflation"], [2.0, 2.0])
                self.assertEqual(cases[1]["sequence_ensemble"]["variance_inflation_summary"]["mean"], 2.0)
                self.assertEqual(cases[0]["sequence_ensemble"]["excess_mse"],
                                 [.01 - .0025 - (split or 0) ** 2 / 4] * 2)
        self.assertIsNone(summarize([report(), report(262165)], [.25])["scenes"][0]["cases"][0]["sequence_ensemble"])
        # A frozen ensemble has no inflation, effective frame count or summary.
        reports = self.with_sequence_ensemble()
        for run in reports:
            run["scenes"][0]["cases"][0]["sequence_ensemble"].update(
                frame_variance=0, variance_inflation=None, effective_frames=None)
        result = summarize(reports, [.25])["scenes"][0]["cases"][0]["sequence_ensemble"]
        self.assertIsNone(result["variance_inflation_summary"])

    def test_sequence_ensemble_rejects_inconsistent_decomposition(self):
        for field, invalid in (("grand_mean_mse", .02), ("grand_mean_noise_mse", .001),
                               ("reference_noise_mse", .001), ("variance_inflation", 3.0),
                               ("effective_frames", 1.0), ("noise_normalized_mse", 1.0),
                               ("excess_mse", 0.0), ("frames", 8), ("sequences", 4),
                               ("frame_variance", -1), ("noise_normalized_mse", None),
                               ("effective_frames", None)):
            with self.subTest(field=field, invalid=invalid):
                reports = self.with_sequence_ensemble()
                reports[1]["scenes"][0]["cases"][1]["sequence_ensemble"][field] = invalid
                with self.assertRaises(ValueError):
                    summarize(reports, [.25])
        reports = self.with_sequence_ensemble(.04)
        for run in reports:
            run["scenes"][0]["reference_split_rmse"] = [.08]
        with self.assertRaisesRegex(ValueError, "reference noise"):
            summarize(reports, [.25])
        reports = self.with_sequence_ensemble()
        del reports[1]["scenes"][0]["cases"][0]["sequence_ensemble"]
        with self.assertRaisesRegex(ValueError, "every run"):
            summarize(reports, [.25])
        reports = self.with_sequence_ensemble()
        reports[1]["scenes"][0]["cases"][0]["sequence_ensemble"] = None
        with self.assertRaisesRegex(ValueError, "static"):
            summarize(reports, [.25])
        for run in reports:
            run["scenes"][0]["moving"] = True
            for case in run["scenes"][0]["cases"]:
                case["convergence"] = None
        with self.assertRaisesRegex(ValueError, "moving"):
            summarize(reports, [.25])
        for run in reports:
            for case in run["scenes"][0]["cases"]:
                case["sequence_ensemble"] = None
        self.assertIsNone(summarize(reports, [.25])["scenes"][0]["cases"][0]["sequence_ensemble"])

    @staticmethod
    def with_replication_bias(split=None, moving=False):
        reports = [report(), report(262165)]
        poses = 4 if moving else 1
        for run in reports:
            scene = run["scenes"][0]
            scene["moving"] = moving
            if split is not None:
                scene["error_reference"] = "finite-spp-nee-mis"
                scene["reference_split_rmse"] = [split] * poses
            reference = 0 if split is None else split * split / 4
            for case in scene["cases"]:
                if moving:
                    case["convergence"] = None
                case["regions"] = [{"id": "boundary", "pixels": 64, "ensemble_rmse": .3,
                                    "signed_mean_error_rgb": [-.1, .2, .05]}]
                # ensemble_rmse .1: the final pose's grand mean MSE is .01.
                grand = [.02] * (poses - 1) + [.01]
                noise = [.004 + reference] * poses
                case["replication_bias"] = {
                    "replication": "pose-frame" if moving else "sequence-mean", "replications": 2,
                    "poses": poses, "grand_mean_mse": sum(grand) / poses, "noise_mse": noise[0],
                    "reference_noise_mse": reference, "noise_normalized_mse": sum(grand) / poses / noise[0],
                    "excess_mse": sum(grand) / poses - noise[0], "pose_grand_mean_mse": grand,
                    "pose_noise_mse": noise, "pose_reference_noise_mse": [reference] * poses,
                    "regions": [{"id": "boundary", "pixels": 64, "grand_mean_mse": .02, "noise_mse": .01,
                                 "noise_normalized_mse": 2.0}],
                    "tiles": {"size": 8, "tests": 12 * poses, "beyond_3": 1,
                              "max": {"pose": poses - 1, "x": 1, "y": 0, "channel": 2, "z": -3.5}}}
        return reports

    def test_replication_bias_survives_reduction(self):
        for split in (None, .04):
            for moving in (False, True):
                if moving and split is None:
                    continue  # Moving workloads always use a finite reference.
                with self.subTest(split=split, moving=moving):
                    bias = summarize(self.with_replication_bias(split, moving), [.25])["scenes"][0]["cases"][1][
                        "replication_bias"]
                    poses = 4 if moving else 1
                    expected = (.02 * (poses - 1) + .01) / poses / (.004 + (split or 0) ** 2 / 4)
                    self.assertAlmostEqual(bias["noise_normalized_mse_summary"]["mean"], expected)
                    self.assertEqual(len(bias["pose_grand_mean_mse"][0]), poses)
                    self.assertEqual(bias["regions"], [{"id": "boundary", "pixels": 64,
                                                        "noise_normalized_mse": [2.0, 2.0]}])
                    self.assertEqual(bias["max_abs_tile_z"], 3.5)
        self.assertIsNone(summarize([report(), report(262165)], [.25])["scenes"][0]["cases"][0]["replication_bias"])
        # Zero expected noise leaves the ratio and its summary undefined.
        reports = self.with_replication_bias()
        for run in reports:
            run["scenes"][0]["cases"][0]["replication_bias"].update(
                noise_mse=0, pose_noise_mse=[0], noise_normalized_mse=None, excess_mse=.01,
                tiles={"size": 8, "tests": 0, "beyond_3": 0, "max": None})
        bias = summarize(reports, [.25])["scenes"][0]["cases"][0]["replication_bias"]
        self.assertIsNone(bias["noise_normalized_mse_summary"])
        self.assertIsNone(bias["max_abs_tile_z"])

    def test_replication_bias_rejects_inconsistent_measurements(self):
        tile = {"size": 8, "tests": 12, "beyond_3": 1, "max": {"pose": 0, "x": 1, "y": 0, "channel": 2, "z": -3.5}}
        for field, invalid in (("replication", "pose-frame"), ("replications", 4), ("poses", 4),
                               ("pose_grand_mean_mse", [.01, .01]), ("pose_grand_mean_mse", [.02]),
                               ("grand_mean_mse", .02), ("noise_mse", .003), ("reference_noise_mse", .001),
                               ("pose_reference_noise_mse", [.001]), ("excess_mse", 0.0),
                               ("noise_normalized_mse", 1.0), ("noise_normalized_mse", None),
                               ("regions", []), ("tiles", dict(tile, beyond_3=13)),
                               ("tiles", dict(tile, max=None)), ("tiles", dict(tile, beyond_3=0)),
                               ("tiles", dict(tile, max=dict(tile["max"], pose=1))),
                               ("tiles", dict(tile, size=4))):
            with self.subTest(field=field, invalid=invalid):
                reports = self.with_replication_bias(.04)
                reports[1]["scenes"][0]["cases"][1]["replication_bias"][field] = invalid
                with self.assertRaises((ValueError, KeyError, TypeError)):
                    summarize(reports, [.25])
        # Pose noise cannot fall below the reference noise it includes.
        reports = self.with_replication_bias(.04)
        for run in reports:
            run["scenes"][0]["cases"][1]["replication_bias"].update(
                noise_mse=.0001, pose_noise_mse=[.0001], noise_normalized_mse=100.0, excess_mse=.0099)
        with self.assertRaisesRegex(ValueError, "below its reference"):
            summarize(reports, [.25])
        reports = self.with_replication_bias(.04)
        for run in reports:
            run["scenes"][0]["cases"][0]["regions"][0]["id"] = "other"
        with self.assertRaisesRegex(ValueError, "regions differ"):
            summarize(reports, [.25])
        reports = self.with_replication_bias()
        del reports[1]["scenes"][0]["cases"][0]["replication_bias"]
        with self.assertRaisesRegex(ValueError, "every run"):
            summarize(reports, [.25])
        # Static replication noise must equal the sequence ensemble's noise.
        reports = self.with_replication_bias()
        for run in reports:
            run["scenes"][0]["cases"][0]["sequence_ensemble"] = {
                "sequences": 2, "frames": 4, "frame_variance": .02, "sequence_mean_variance": .01,
                "variance_inflation": 2.0, "effective_frames": 2.0, "grand_mean_mse": .01,
                "grand_mean_noise_mse": .005, "reference_noise_mse": 0, "noise_normalized_mse": 2.0,
                "excess_mse": .005}
        with self.assertRaisesRegex(ValueError, "sequence ensemble"):
            summarize(reports, [.25])

    def test_finite_reference_and_boundary_regions_survive_reduction(self):
        reports = [report(), report(262165)]
        for run in reports:
            scene = run["scenes"][0]
            scene["error_reference"] = "finite-spp-nee-mis"
            scene["reference_split_rmse"] = [.04]
            for case in scene["cases"]:
                case["regions"] = [{"id": "boundary", "pixels": 64, "ensemble_rmse": .3,
                                    "signed_mean_error_rgb": [-.1, .2, .05]}]
        scene = summarize(reports, [.25])["scenes"][0]
        self.assertEqual(scene["error_reference"], "finite-spp-nee-mis")
        self.assertEqual(scene["reference_split_rmse"], [[.04], [.04]])
        self.assertEqual(scene["cases"][0]["regions"][0]["ensemble_rmse"]["raw"], [.3, .3])
        reports[1]["scenes"][0]["cases"][0]["regions"][0]["pixels"] = 32
        with self.assertRaisesRegex(ValueError, "regions must match"):
            summarize(reports, [.25])

    def test_rejects_reference_changes_and_invalid_region_errors(self):
        reports = [report(), report(262165)]
        reports[1]["scenes"][0]["error_reference"] = "finite-spp-nee-mis"
        with self.assertRaisesRegex(ValueError, "error reference"):
            summarize(reports, [.25])
        for run in reports:
            scene = run["scenes"][0]
            scene["error_reference"] = "finite-spp-nee-mis"
            scene["reference_split_rmse"] = [float("nan")]
        with self.assertRaisesRegex(ValueError, "finite"):
            summarize(reports, [.25])
        for run in reports:
            scene = run["scenes"][0]
            scene["reference_split_rmse"] = [.1]
            scene["cases"][0]["regions"] = [{"id": "boundary", "pixels": 64, "ensemble_rmse": .1,
                                             "signed_mean_error_rgb": [float("inf"), 0, 0]}]
        with self.assertRaisesRegex(ValueError, "region signed"):
            summarize(reports, [.25])

    def test_explicit_reference_requires_matched_split_populations(self):
        for split in (None, [], [.1, .2]):
            with self.subTest(split=split):
                reports = [report(), report(262165)]
                for run in reports:
                    scene = run["scenes"][0]
                    scene["error_reference"] = "finite-spp-nee-mis"
                    if split is not None:
                        scene["reference_split_rmse"] = split
                with self.assertRaisesRegex(ValueError, "split"):
                    summarize(reports, [.25])

    def test_region_population_rejects_missing_duplicate_or_invalid_values(self):
        valid = {"id": "boundary", "pixels": 64, "ensemble_rmse": .1,
                 "signed_mean_error_rgb": [.01, -.02, .03]}
        for invalid in ([], [valid, valid], [dict(valid, pixels=0)],
                        [dict(valid, ensemble_rmse=float("nan"))]):
            with self.subTest(invalid=invalid):
                reports = [report(), report(262165)]
                reports[0]["scenes"][0]["cases"][0]["regions"] = [valid]
                reports[1]["scenes"][0]["cases"][0]["regions"] = invalid
                with self.assertRaises(ValueError):
                    summarize(reports, [.25])

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
