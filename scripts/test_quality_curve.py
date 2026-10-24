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
                              "sample_index": int(value("--benchmark-sample-index"))},
                "scenes": [{"id": "cornell-v1", "status": "pass",
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

    def test_full_campaign_retains_independent_sequences_and_raw_evidence(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "output"
            code, process = self.run_curve(output, self.successful_run)
            self.assertEqual(code, 0)
            self.assertEqual(process.call_count, 4)
            summary = json.loads((output / "quality.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["status"], "pass")
            self.assertEqual(summary["sample_index"], 1 << 20)
            self.assertEqual(summary["reference"]["sample_index"], 0)
            self.assertEqual(summary["reference"]["sha256"], hashlib.sha256(
                (quality.ROOT / summary["reference"]["path"]).read_bytes()).hexdigest())
            self.assertEqual(len(summary["curves"]), 2)
            for curve in summary["curves"]:
                self.assertEqual([p["samples"] for p in curve["points"]], [1, 4])
                self.assertEqual([p["gpu_total_ms"] for p in curve["points"]], [2, 8])
                self.assertEqual([p["render_wall_total_ms"] for p in curve["points"]], [3, 12])
            self.assertTrue((output / "spp-4/headless-cornell-v1-1.log").is_file())

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
                      ("--frames", "4097"), ("--runs", "0"),
                      ("--rmse-target", "nan"), ("--rmse-target", "0")):
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


if __name__ == "__main__":
    unittest.main()
