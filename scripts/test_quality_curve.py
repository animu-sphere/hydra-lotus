# SPDX-License-Identifier: Apache-2.0
from contextlib import redirect_stderr, redirect_stdout
import hashlib
import io
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import quality_curve as quality


def pfm(path, values, width=1, height=1, scale=-1):
    endian = "<" if scale < 0 else ">"
    path.write_bytes(f"PF\n{width} {height}\n{scale}\n".encode() +
                     struct.pack(endian + "f" * len(values), *values))


class QualityCurveTest(unittest.TestCase):
    def test_pfm_scale_endian_and_rgb_error(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "image.pfm"
            for scale in (-2, 2):
                pfm(path, [1, 2, 3], scale=scale)
                self.assertEqual(quality.read_pfm(path), ((1, 1), [2, 4, 6]))
            result = quality.image_error(path, ((1, 1), [1, 2, 3]))
            self.assertAlmostEqual(result["mse"], 14 / 3)
            self.assertAlmostEqual(result["rmse"], math.sqrt(14 / 3))
            with self.assertRaisesRegex(ValueError, "dimensions differ"):
                quality.image_error(path, ((2, 1), [0] * 6))

    def test_pfm_rejects_wrong_format_nonfinite_and_truncated_payloads(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "image.pfm"
            for content in (b"Pf\n1 1\n-1\n", b"PF\n1 1\n0\n", b"PF\n1 1\nnan\n",
                            b"PF\n0 1\n-1\n", b"PF\n1 1\n-1\nshort"):
                path.write_bytes(content)
                with self.assertRaises(ValueError):
                    quality.read_pfm(path)
            pfm(path, [0, float("inf"), 1])
            with self.assertRaisesRegex(ValueError, "non-finite"):
                quality.read_pfm(path)

    def test_sum_uses_every_frame_and_preserves_unavailable_gpu(self):
        self.assertEqual(quality.total_ms({"raw": [1, 7, 2], "mean": 99}, 3), 10)
        self.assertIsNone(quality.total_ms(None, 3, nullable=True))
        for series in (None, {"raw": [1]}, {"raw": [0, 1]}, {"raw": [True, 1]},
                       {"raw": [1, -1]}, {"raw": [1, float("nan")]}):
            with self.assertRaises(ValueError):
                quality.total_ms(series, 2)

    def test_target_recrossing_no_interpolation_or_extrapolation(self):
        curve = [{"samples": n, "rmse": error, "gpu_total_ms": n * 2,
                  "render_wall_total_ms": n * 3}
                 for n, error in ((1, 0.5), (4, 0.1), (16, 0.3), (64, 0.1))]
        self.assertEqual(quality.observed_target(curve, 0.2)["samples"], 64)
        self.assertEqual(quality.observed_target(curve, 0.5)["gpu_total_ms"], 2)
        self.assertIsNone(quality.observed_target(curve, 0.05)["samples"])
        self.assertEqual(quality.observed_target(curve[:2], 0.2)["samples"], 4)

    def successful_run(self, cmd, **kwargs):
        value = lambda flag: cmd[cmd.index(flag) + 1]
        report = Path(value("--benchmark"))
        image = report.with_suffix(".pfm")
        # Real reference dimensions are 128x128. File order is preserved.
        pfm(image, [0.5] * (128 * 128 * 3), width=128, height=128)
        frames = int(value("--benchmark-frames"))
        data = {"schema": "lotus-benchmark-v1", "status": "pass",
                "procedure": {"integrator": value("--benchmark-integrator"), "light_sampling": "mis",
                              "width": 128, "height": 128, "measured_frames": frames,
                              "warmup_frames": int(value("--benchmark-warmup")),
                              "sample_index": int(value("--benchmark-sample-index")),
                              "max_bounces": 64, "samples_per_frame": 1},
                "scenes": [{"id": value("--benchmark-workload"), "status": "pass",
                            "measurements": {"image": image.name, "validation": {"messages": 0},
                                             "gpu_frame_ms": {"raw": [2] * frames},
                                             "render_wall_ms": {"raw": [3] * frames}}}]}
        report.write_text(json.dumps(data), encoding="utf-8")
        return subprocess.CompletedProcess(cmd, 0, "success\n", "")

    def run_curve(self, output, execute, extra=()):
        with patch.object(quality.baseline.subprocess, "run", side_effect=execute) as process:
            with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                code = quality.main(["--headless", "runner", "--output", str(output),
                    "--label", "revision", "--clocks", "unlocked", "--runs", "2",
                    "--frames", "1", "4", *extra])
        return code, process

    def make_reference(self, directory):
        directory.mkdir()
        manifest = quality.baseline.load_manifest(quality.ROOT / "validation/performance-workloads.json")
        manifest["settings"].update(frames=4096, runs=1)
        manifest["workloads"] = ["textured-v1"]
        report = directory / "headless-textured-v1-0.json"
        command = quality.baseline.command(Path("runner"), "headless", "textured-v1", "reference",
                                            manifest["settings"], report, "reference build")
        self.successful_run(command + ["--benchmark-sample-index", "0"])
        log = report.with_suffix(".log")
        log.write_text("success\n", encoding="utf-8")
        run = {"schema": "lotus-performance-run-v1", "status": "pass", "integrator": "reference",
               "manifest": manifest, "runs": [{"adapter": "headless", "workload": "textured-v1",
                   "run": 0, "exit_code": 0, "status": "pass", "report": report.name,
                   "log": log.name, "image_sha256": hashlib.sha256(report.with_suffix(".pfm").read_bytes()).hexdigest()}]}
        path = directory / "run.json"
        path.write_text(json.dumps(run), encoding="utf-8")
        return path

    def test_external_reference_selects_workload_and_snapshots_provenance(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "output"
            reference = self.make_reference(Path(temp) / "input")
            code, process = self.run_curve(output, self.successful_run,
                ("--workload", "textured-v1", "--reference-run", str(reference)))
            self.assertEqual(code, 0)
            summary = json.loads((output / "quality.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["workload"], "textured-v1")
            self.assertEqual(summary["reference"]["samples"], 4096)
            self.assertEqual(summary["reference"]["sample_index"], 0)
            self.assertEqual(summary["curves"][0]["points"][0]["rmse"], 0)
            for call in process.call_args_list:
                command = call.args[0]
                self.assertEqual(command[command.index("--benchmark-workload") + 1], "textured-v1")
            for name, digest in summary["reference"]["evidence_sha256"].items():
                self.assertEqual(hashlib.sha256((output / "reference" / name).read_bytes()).hexdigest(), digest)
            self.assertTrue((output / summary["reference"]["run_summary"]).is_file())

    def test_external_reference_rejects_failed_mismatched_and_modified_evidence(self):
        for fault in ("status", "exit", "integrator", "workload", "size", "depth", "samples",
                      "hash", "dimensions", "log", "multiple", "path", "overlap", "short"):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as temp:
                reference = self.make_reference(Path(temp) / "input")
                run = json.loads(reference.read_text(encoding="utf-8"))
                report = reference.parent / run["runs"][0]["report"]
                data = json.loads(report.read_text(encoding="utf-8"))
                extra = []
                if fault == "status": run["status"] = "fail"
                elif fault == "exit": run["runs"][0]["exit_code"] = 1
                elif fault == "integrator": run["integrator"] = "wavefront"
                elif fault == "workload": run["runs"][0]["workload"] = "cornell-v1"
                elif fault == "size": run["manifest"]["settings"]["size"] = 64
                elif fault == "depth": data["procedure"]["max_bounces"] = 1
                elif fault == "samples": data["procedure"]["samples_per_frame"] = 2
                elif fault == "hash": report.with_suffix(".pfm").write_bytes(b"changed")
                elif fault == "dimensions":
                    pfm(report.with_suffix(".pfm"), [0, 0, 0])
                    run["runs"][0]["image_sha256"] = hashlib.sha256(report.with_suffix(".pfm").read_bytes()).hexdigest()
                elif fault == "log": report.with_suffix(".log").unlink()
                elif fault == "multiple": run["runs"].append(run["runs"][0])
                elif fault == "path": run["runs"][0]["report"] = "../report.json"
                elif fault == "overlap": extra = ["--sample-index", "4095"]
                elif fault == "short": extra = ["--frames", "4096"]
                reference.write_text(json.dumps(run), encoding="utf-8")
                report.write_text(json.dumps(data), encoding="utf-8")
                output = Path(temp) / "output"
                with self.assertRaises(SystemExit):
                    self.run_curve(output, self.successful_run,
                        ("--workload", "textured-v1", "--reference-run", str(reference), *extra))
                self.assertFalse(output.exists())

    def test_external_reference_boundary_accepts_first_disjoint_sample(self):
        with tempfile.TemporaryDirectory() as temp:
            reference = self.make_reference(Path(temp) / "input")
            code, _ = self.run_curve(Path(temp) / "output", self.successful_run,
                ("--workload", "textured-v1", "--reference-run", str(reference),
                 "--sample-index", "4096"))
            self.assertEqual(code, 0)

    def test_full_campaign_retains_independent_sequences_and_raw_evidence(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "output"
            code, process = self.run_curve(output, self.successful_run)
            self.assertEqual(code, 0)
            self.assertEqual(process.call_count, 4)
            summary = json.loads((output / "quality.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["status"], "pass")
            self.assertEqual(summary["schema"], "lotus-quality-curve-v2")
            self.assertEqual(summary["sample_indices"], [1 << 20])
            self.assertEqual(summary["reference"]["sample_index"], 0)
            self.assertEqual(summary["reference"]["sha256"], hashlib.sha256(
                (quality.ROOT / summary["reference"]["path"]).read_bytes()).hexdigest())
            self.assertEqual(len(summary["curves"]), 2)
            for curve in summary["curves"]:
                self.assertEqual([p["samples"] for p in curve["points"]], [1, 4])
                self.assertEqual([p["gpu_total_ms"] for p in curve["points"]], [2, 8])
                self.assertEqual([p["render_wall_total_ms"] for p in curve["points"]], [3, 12])
            self.assertTrue((output / "spp-4/headless-cornell-v1-1.log").is_file())
            self.assertIsNone(summary["sample_populations"][0]["rmse"]["sample_stddev"])

    def test_disjoint_sequences_keep_separate_curves_and_population_counts(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "output"
            def execute(cmd, **kwargs):
                result = self.successful_run(cmd, **kwargs)
                index = int(cmd[cmd.index("--benchmark-sample-index") + 1])
                report = Path(cmd[cmd.index("--benchmark") + 1])
                pfm(report.with_suffix(".pfm"), [index / 4096] * (128 * 128 * 3),
                    width=128, height=128)
                return result
            code, process = self.run_curve(output, execute,
                ("--sample-index", "1024", "--sample-index", "2048"))
            self.assertEqual(code, 0)
            self.assertEqual(process.call_count, 8)
            summary = json.loads((output / "quality.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["sample_indices"], [1024, 2048])
            self.assertEqual([(c["sample_index"], c["run"]) for c in summary["curves"]],
                             [(1024, 0), (1024, 1), (2048, 0), (2048, 1)])
            for point in summary["sample_populations"]:
                self.assertEqual(point["independent_sequences"], 2)
                self.assertLess(point["rmse"]["min"], point["rmse"]["max"])
                self.assertGreater(point["rmse"]["sample_stddev"], 0)
            for checkpoint in summary["checkpoints"]:
                path = output / checkpoint["run_summary"]
                self.assertTrue(path.is_file())
                run = json.loads(path.read_text(encoding="utf-8"))
                self.assertEqual(run["manifest"]["settings"]["sample_index"],
                                 checkpoint["sample_index"])

    def test_population_does_not_count_timing_repeats_as_independent(self):
        curves = [{"run": run, "points": [{"rmse": error}]}
                  for run, error in ((0, 1), (1, 100), (0, 3), (1, 200))]
        population = quality.sample_populations(curves, [4])[0]
        self.assertEqual(population["independent_sequences"], 2)
        self.assertEqual(population["rmse"], {"min": 1, "median": 2, "max": 3,
                         "mean": 2, "sample_stddev": math.sqrt(2)})

    def test_same_sequence_image_mismatch_fails_with_evidence(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "output"
            def execute(cmd, **kwargs):
                result = self.successful_run(cmd, **kwargs)
                report = Path(cmd[cmd.index("--benchmark") + 1])
                if report.stem.endswith("-1"):
                    pfm(report.with_suffix(".pfm"), [0.25] * (128 * 128 * 3),
                        width=128, height=128)
                return result
            code, process = self.run_curve(output, execute)
            self.assertEqual(code, 1)
            self.assertEqual(process.call_count, 2)
            summary = json.loads((output / "quality.json").read_text(encoding="utf-8"))
            self.assertIn("images differ", summary["error"])
            self.assertEqual(len(summary["checkpoints"][0]["points"]), 2)
            self.assertEqual(summary["sample_populations"], [])

    def test_later_sequence_failure_preserves_completed_sequence_without_retry(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "output"
            def execute(cmd, **kwargs):
                if cmd[cmd.index("--benchmark-sample-index") + 1] == "2048":
                    return subprocess.CompletedProcess(cmd, 1, "driver failed", "")
                return self.successful_run(cmd, **kwargs)
            code, process = self.run_curve(output, execute,
                ("--sample-index", "1024", "--sample-index", "2048"))
            self.assertEqual(code, 1)
            self.assertEqual(process.call_count, 5)
            summary = json.loads((output / "quality.json").read_text(encoding="utf-8"))
            self.assertEqual(len(summary["curves"]), 2)
            self.assertEqual(summary["curves"][0]["sample_index"], 1024)
            self.assertEqual(summary["checkpoints"][-1]["status"], "fail")
            self.assertEqual(summary["sample_populations"], [])

    def test_failure_skip_and_timeout_stop_without_retry_and_preserve_previous_checkpoint(self):
        for outcome in (subprocess.CompletedProcess([], 1, "driver failed", ""),
                        subprocess.CompletedProcess([], 77, "SKIP: no GPU", ""),
                        subprocess.TimeoutExpired([], 300, output=b"partial")):
            with self.subTest(outcome=outcome), tempfile.TemporaryDirectory() as temp:
                calls = []
                def execute(cmd, **kwargs):
                    calls.append(cmd)
                    if len(calls) <= 2:
                        return self.successful_run(cmd, **kwargs)
                    if isinstance(outcome, Exception):
                        raise outcome
                    return outcome
                output = Path(temp) / "output"
                code, process = self.run_curve(output, execute)
                skipped = isinstance(outcome, subprocess.CompletedProcess) and outcome.returncode == 77
                self.assertEqual(code, 77 if skipped else 1)
                self.assertEqual(process.call_count, 4 if skipped else 3)
                summary = json.loads((output / "quality.json").read_text(encoding="utf-8"))
                self.assertEqual(summary["checkpoints"][0]["status"], "pass")
                self.assertEqual(summary["status"], "skip" if skipped else "fail")
                self.assertEqual(summary["curves"], [])
                self.assertTrue((output / "spp-4/run.json").is_file())

    def test_invalid_image_or_timing_marks_checkpoint_failed(self):
        for field in ("image", "gpu_frame_ms"):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as temp:
                def execute(cmd, **kwargs):
                    result = self.successful_run(cmd, **kwargs)
                    path = Path(cmd[cmd.index("--benchmark") + 1])
                    data = json.loads(path.read_text(encoding="utf-8"))
                    measurements = data["scenes"][0]["measurements"]
                    if field == "image":
                        (path.parent / measurements["image"]).write_bytes(b"corrupt image")
                    else:
                        measurements[field] = {"raw": []}
                        path.write_text(json.dumps(data), encoding="utf-8")
                    return result
                output = Path(temp) / "output"
                code, process = self.run_curve(output, execute)
                self.assertEqual(code, 1)
                self.assertEqual(process.call_count, 2)
                summary = json.loads((output / "quality.json").read_text(encoding="utf-8"))
                self.assertEqual(summary["status"], "fail")
                self.assertEqual(summary["checkpoints"][0]["status"], "fail")
                self.assertIn("error", summary)

    def test_invalid_arguments_do_not_launch_or_overwrite(self):
        for extra in (("--frames", "4", "1"), ("--frames", "1", "1"),
                      ("--workload", "textured-v1"), ("--workload", "unknown-v1"),
                      ("--frames", "4097"), ("--runs", "0"),
                      ("--rmse-target", "nan"), ("--rmse-target", "0"),
                      ("--sample-index", "-1"), ("--sample-index", "0"),
                      ("--sample-index", "1023"),
                      ("--sample-index", "1024", "--warmup", "1024"),
                      ("--sample-index", str((1 << 21) - 3)),
                      ("--sample-index", "1024", "--sample-index", "1024"),
                      ("--sample-index", "1024", "--sample-index", "1027"),
                      tuple(value for i in range(17) for value in
                            ("--sample-index", str(1024 + i * 4)))):
            with self.subTest(extra=extra), tempfile.TemporaryDirectory() as temp:
                output = Path(temp) / "output"
                with self.assertRaises(SystemExit):
                    self.run_curve(output, self.successful_run, extra)
                self.assertFalse(output.exists())
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            sentinel = output / "quality.json"
            sentinel.write_text("preserve", encoding="utf-8")
            with self.assertRaises(SystemExit):
                self.run_curve(output, self.successful_run)
            self.assertEqual(sentinel.read_text(encoding="utf-8"), "preserve")

    def test_adjacent_sequence_ranges_and_variance_boundary_are_allowed(self):
        with tempfile.TemporaryDirectory() as temp:
            code, process = self.run_curve(Path(temp) / "output", self.successful_run,
                ("--sample-index", "1024", "--sample-index", "1028",
                 "--sample-index", str((1 << 21) - 4)))
            self.assertEqual(code, 0)
            self.assertEqual(process.call_count, 12)


if __name__ == "__main__":
    unittest.main()
