# SPDX-License-Identifier: Apache-2.0
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import gpu_telemetry as telemetry


SNAPSHOT = ("2026/10/24 15:00:00.000, 0, GPU-first, NVIDIA RTX A5000, 597.16, "
            "P0, 1905, 8001, 45, 2040, 24564\n")


class GpuTelemetryTest(unittest.TestCase):
    def test_multiple_devices_keep_unavailable_counters_and_separate_ranges(self):
        other = SNAPSHOT.replace("GPU-first", "GPU-second").replace(
            "1905, 8001, 45, 2040", "[N/A], [Not Supported], 0, 1024")
        devices = telemetry.parse_snapshot(SNAPSHOT + other)
        later = telemetry.parse_snapshot(SNAPSHOT.replace("1905, 8001, 45, 2040", "210, 405, 0, 4096"))
        summary = telemetry.summarize([{"devices": devices}, {"devices": later}])
        self.assertEqual(summary["GPU-first"]["samples"], 2)
        self.assertEqual(summary["GPU-first"]["ranges"]["device_memory_used_mib"], {"min": 2040, "max": 4096})
        self.assertEqual(summary["GPU-first"]["ranges"]["graphics_clock_mhz"], {"min": 210, "max": 1905})
        self.assertEqual(summary["GPU-second"]["samples"], 1)
        self.assertEqual(summary["GPU-second"]["ranges"]["graphics_clock_mhz"], {"min": None, "max": None})

    def test_malformed_snapshot_is_not_silently_accepted(self):
        for output in ("", "driver failure", SNAPSHOT + SNAPSHOT,
                       SNAPSHOT.replace("GPU-first", "N/A"),
                       SNAPSHOT.replace("1905", "nan"), SNAPSHOT.replace("1905", "-1")):
            with self.subTest(output=output), self.assertRaises(ValueError):
                telemetry.parse_snapshot(output)

    def collect(self, execute, during=False):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "run.gpu.json"
            observer = telemetry.GpuTelemetry(path, 100)
            with patch.object(telemetry.subprocess, "run", side_effect=execute), \
                    patch.object(telemetry.threading, "Thread") as thread:
                observer.start()
                if during:
                    observer.sample("process_window")
                reference = observer.finish()
                if observer.thread:
                    thread.return_value.join.assert_called_once()
            self.assertTrue(observer.stop_event.is_set())
            self.assertEqual(reference["report"], path.name)
            data = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(reference["status"], data["status"])
            return data

    def test_bracketing_samples_and_units(self):
        data = self.collect(lambda cmd, **kw: subprocess.CompletedProcess(cmd, 0, SNAPSHOT, ""))
        self.assertEqual(data["status"], "available")
        self.assertEqual([s["phase"] for s in data["samples"]], ["before_process", "after_process"])
        self.assertEqual(data["samples"][0]["devices"][0]["device_memory_used_mib"], 2040)
        self.assertIn("not process peak residency", data["scope"])

    def test_unavailable_tool_does_not_become_a_renderer_failure(self):
        failures = (FileNotFoundError("missing nvidia-smi"), subprocess.TimeoutExpired("nvidia-smi", 5),
                    subprocess.CompletedProcess("nvidia-smi", 9, "", "unsupported query"),
                    subprocess.CompletedProcess("nvidia-smi", 0, "malformed", ""))
        for failure in failures:
            with self.subTest(failure=failure):
                data = self.collect([failure])
                self.assertEqual(data["status"], "unavailable")
                self.assertEqual(len(data["errors"]), 1)
                self.assertEqual(data["samples"], [])

    def test_mid_run_failure_keeps_successful_samples(self):
        data = self.collect([subprocess.CompletedProcess("nvidia-smi", 0, SNAPSHOT, ""),
                             OSError("device disappeared")], during=True)
        self.assertEqual(data["status"], "partial")
        self.assertEqual(len(data["samples"]), 1)
        self.assertIn("device disappeared", data["errors"][0]["error"])


if __name__ == "__main__":
    unittest.main()
