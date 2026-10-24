# SPDX-License-Identifier: Apache-2.0
from contextlib import redirect_stderr, redirect_stdout
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import performance_baseline as baseline


class PerformanceBaselineTest(unittest.TestCase):
    def setUp(self):
        self.manifest = baseline.load_manifest(
            Path(__file__).resolve().parents[1] / "validation/performance-workloads.json")
        self.settings = self.manifest["settings"]

    def test_adapters_match_samples_and_warmup(self):
        commands = {a: baseline.command(Path("runner"), a, "cornell-v1", "wavefront",
                    self.settings, Path("out.json"), "revision") for a in ("headless", "viewport")}
        headless, viewport = commands["headless"], commands["viewport"]
        value = lambda cmd, key: cmd[cmd.index(key) + 1]
        self.assertEqual(value(headless, "--benchmark-frames"), value(viewport, "--samples"))
        self.assertEqual(int(value(viewport, "--frames")), self.settings["frames"] + self.settings["warmup"])
        self.assertEqual(value(headless, "--benchmark-size"), value(viewport, "--width"))
        self.assertEqual(value(viewport, "--width"), value(viewport, "--height"))
        self.assertEqual(value(headless, "--benchmark-warmup"), value(viewport, "--benchmark-warmup"))

    def test_rejects_unsupported_seed_or_duplicate_fixture(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "manifest.json"
            self.manifest["settings"]["sample_index"] = 17
            path.write_text(json.dumps(self.manifest), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "seed"):
                baseline.load_manifest(path)
            self.manifest["settings"]["sample_index"] = 0
            self.manifest["workloads"].append("cornell-v1")
            path.write_text(json.dumps(self.manifest), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "duplicate"):
                baseline.load_manifest(path)

    def test_rejects_viewport_without_accumulation_reset_or_with_readback(self):
        data = {"schema": "lotus.viewport-benchmark/v1", "scene_source": "cornell-v1",
                "width": 128, "height": 128, "samples_per_pixel": 64,
                "controls": {"integrator": "reference", "nee": True, "restir": "off",
                             "temporal_radiance": False, "denoise": False, "accumulate": True},
                "measurement": {"frames": 64, "warmup_frames": 8},
                "validation_messages": 0, "readback_bytes": 0}
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "report.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            self.assertEqual(baseline.check_report(path, "viewport", "cornell-v1", "reference", self.settings), {})
            for key, value in (("samples_per_pixel", 72), ("readback_bytes", 16), ("validation_messages", 1)):
                invalid = dict(data, **{key: value})
                path.write_text(json.dumps(invalid), encoding="utf-8")
                with self.assertRaises(ValueError):
                    baseline.check_report(path, "viewport", "cornell-v1", "reference", self.settings)

    def run_baseline(self, output, execute, extra=()):
        args = ["--headless", "runner", "--output", str(output), "--label", "revision",
                "--clocks", "unlocked", "--workload", "cornell-v1", *extra]
        with patch.object(baseline.subprocess, "run", side_effect=execute) as process:
            with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                code = baseline.main(args)
        return code, process

    def successful_run(self, cmd, **kwargs):
        report = Path(cmd[cmd.index("--benchmark") + 1])
        image = report.with_suffix(".pfm")
        image.write_bytes(b"deterministic image")
        data = {"schema": "lotus-benchmark-v1", "status": "pass",
                "procedure": {"integrator": "reference", "light_sampling": "mis",
                              "width": 128, "height": 128,
                              "measured_frames": 64, "warmup_frames": 8},
                "scenes": [{"id": "cornell-v1", "status": "pass",
                            "measurements": {"image": image.name, "validation": {"messages": 0}}}]}
        report.write_text(json.dumps(data), encoding="utf-8")
        return subprocess.CompletedProcess(cmd, 0, "successful run\n", "")

    def test_success_preserves_each_repetition(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "output"
            code, process = self.run_baseline(output, self.successful_run)
            summary = json.loads((output / "run.json").read_text(encoding="utf-8"))
            self.assertEqual(code, 0)
            self.assertEqual(process.call_count, 3)
            self.assertEqual(summary["status"], "pass")
            self.assertEqual(len({entry["report"] for entry in summary["runs"]}), 3)
            self.assertEqual(len({entry["image_sha256"] for entry in summary["runs"]}), 1)
            for entry in summary["runs"]:
                self.assertEqual(entry["status"], "pass")
                self.assertEqual((output / entry["log"]).read_text(encoding="utf-8"), "successful run\n")

    def test_timeout_preserves_partial_output_and_previous_run_without_retry(self):
        for stdout, stderr in ((b"frame 2\n", b"driver\xff\n"), ("frame 2\n", "driver\n"), (None, None)):
            with self.subTest(stdout=stdout), tempfile.TemporaryDirectory() as temp:
                output = Path(temp) / "output"
                calls = []

                def execute(cmd, **kwargs):
                    calls.append(cmd)
                    if len(calls) == 1:
                        return self.successful_run(cmd, **kwargs)
                    report = Path(cmd[cmd.index("--benchmark") + 1])
                    report.write_text("partial report", encoding="utf-8")
                    raise subprocess.TimeoutExpired(cmd, 300, output=stdout, stderr=stderr)

                code, process = self.run_baseline(output, execute)
                summary = json.loads((output / "run.json").read_text(encoding="utf-8"))
                self.assertEqual(code, 1)
                self.assertEqual(process.call_count, 2)
                self.assertEqual(summary["status"], "fail")
                self.assertEqual(summary["runs"][0]["status"], "pass")
                failure = summary["runs"][1]
                self.assertEqual(failure["status"], "fail")
                self.assertIsNone(failure["exit_code"])
                self.assertEqual(failure["command"], calls[1])
                self.assertEqual((output / failure["report"]).read_text(encoding="utf-8"), "partial report")
                log = (output / failure["log"]).read_text(encoding="utf-8")
                self.assertIn("timed out", log)
                if stdout:
                    self.assertIn("frame 2\n", log)
                    self.assertIn("driver", log)

    def test_launch_failure_is_recorded_without_retry(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "output"
            code, process = self.run_baseline(output, FileNotFoundError("missing runner"))
            summary = json.loads((output / "run.json").read_text(encoding="utf-8"))
            self.assertEqual(code, 1)
            self.assertEqual(process.call_count, 1)
            self.assertEqual(summary["status"], "fail")
            failure, = summary["runs"]
            self.assertEqual(failure["status"], "fail")
            self.assertIsNone(failure["exit_code"])
            self.assertIsNone(failure["report"])
            self.assertIn("missing runner", (output / failure["log"]).read_text(encoding="utf-8"))

    def test_driver_failure_and_invalid_report_are_not_skips_or_retried(self):
        for exit_code in (-1073740791, 1, 0):
            with self.subTest(exit_code=exit_code), tempfile.TemporaryDirectory() as temp:
                output = Path(temp) / "output"

                def execute(cmd, **kwargs):
                    report = Path(cmd[cmd.index("--benchmark") + 1])
                    report.write_text("invalid report", encoding="utf-8")
                    return subprocess.CompletedProcess(cmd, exit_code, "frame 2\n", "diagnostic\n")

                code, process = self.run_baseline(output, execute)
                summary = json.loads((output / "run.json").read_text(encoding="utf-8"))
                self.assertEqual(code, 1)
                self.assertEqual(process.call_count, 1)
                self.assertEqual(summary["status"], "fail")
                failure, = summary["runs"]
                self.assertEqual(failure["exit_code"], exit_code)
                self.assertEqual(failure["status"], "fail")
                self.assertEqual((output / failure["log"]).read_text(encoding="utf-8"), "frame 2\ndiagnostic\n")

    def test_capability_skip_requires_explanation(self):
        for explanation in ("SKIP: no Vulkan device\n", ""):
            with self.subTest(explanation=explanation), tempfile.TemporaryDirectory() as temp:
                output = Path(temp) / "output"
                code, process = self.run_baseline(output, lambda cmd, **kwargs:
                    subprocess.CompletedProcess(cmd, 77, explanation, ""))
                summary = json.loads((output / "run.json").read_text(encoding="utf-8"))
                self.assertEqual(code, 77 if explanation else 1)
                self.assertEqual(summary["status"], "skip" if explanation else "fail")
                self.assertEqual(process.call_count, 3 if explanation else 1)

    def test_duplicate_selection_is_rejected_before_launch_or_output(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "output"
            with self.assertRaises(SystemExit) as error:
                self.run_baseline(output, self.successful_run, ("--workload", "cornell-v1"))
            self.assertEqual(error.exception.code, 2)
            self.assertFalse(output.exists())

    def test_existing_evidence_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            evidence = output / "run.json"
            evidence.write_text("previous failure", encoding="utf-8")
            with self.assertRaises(SystemExit):
                self.run_baseline(output, self.successful_run)
            self.assertEqual(evidence.read_text(encoding="utf-8"), "previous failure")

    def test_optional_telemetry_is_finalized_on_success_failure_and_timeout(self):
        outcomes = (self.successful_run, FileNotFoundError("missing runner"),
                    subprocess.TimeoutExpired("runner", 300),
                    lambda cmd, **kw: subprocess.CompletedProcess(cmd, 1, "driver failure", ""))
        for outcome in outcomes:
            with self.subTest(outcome=outcome), tempfile.TemporaryDirectory() as temp:
                output = Path(temp) / "output"
                with patch.object(baseline, "GpuTelemetry") as observer:
                    observer.return_value.finish.return_value = {
                        "status": "unavailable", "report": "observation.gpu.json"}
                    code, process = self.run_baseline(output, outcome, ("--gpu-telemetry", "nvidia-smi"))
                count = 3 if outcome == self.successful_run else 1
                self.assertEqual(code, 0 if count == 3 else 1)
                self.assertEqual(process.call_count, count)
                self.assertEqual(observer.return_value.start.call_count, count)
                self.assertEqual(observer.return_value.finish.call_count, count)
                summary = json.loads((output / "run.json").read_text(encoding="utf-8"))
                self.assertEqual(summary["gpu_telemetry"], "nvidia-smi")
                self.assertTrue(all(r["gpu_telemetry"]["status"] == "unavailable" for r in summary["runs"]))

    def test_plan_never_starts_telemetry_or_creates_output(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "output"
            with patch.object(baseline, "GpuTelemetry") as observer:
                code, process = self.run_baseline(output, self.successful_run,
                    ("--gpu-telemetry", "nvidia-smi", "--plan"))
            self.assertEqual(code, 0)
            observer.assert_not_called()
            process.assert_not_called()
            self.assertFalse(output.exists())

    def test_invalid_telemetry_interval_is_rejected_before_launch(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "output"
            with self.assertRaises(SystemExit):
                self.run_baseline(output, self.successful_run, ("--gpu-sample-ms", "0"))
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
