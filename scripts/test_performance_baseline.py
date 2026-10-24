# SPDX-License-Identifier: Apache-2.0
import json
from pathlib import Path
import tempfile
import unittest

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


if __name__ == "__main__":
    unittest.main()
