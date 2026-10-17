"""Campaign runner fixtures with a stand-in benchmark executable."""

import json
from pathlib import Path
import sys
import tempfile
import time
import unittest

from restir_campaign import campaign, commands, load_plan

# Writes the report named by --benchmark-restir; fails when --restir-seed is 13.
FAKE = """
import json, sys, time
args = dict(zip(sys.argv[1::2], sys.argv[2::2]))
time.sleep(float(args.get("--sleep", 0)))
status = "fail" if args.get("--restir-seed") == "13" else "pass"
open(args["--benchmark-restir"], "w").write(json.dumps(
    {"status": status, "label": args["--benchmark-label"], "args": args}))
sys.exit(1 if status == "fail" else 0)
"""


class CampaignTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        (self.root / "fake.py").write_text(FAKE, encoding="utf-8")

    def tearDown(self):
        self.directory.cleanup()

    def plan(self, runs, **extra):
        plan = {"executable": [sys.executable, str(self.root / "fake.py")], "output": "out",
                "label": "fixture", "defaults": {"benchmark-size": 32, "restir-seed": 17}, "runs": runs}
        plan.update(extra)
        path = self.root / "plan.json"
        path.write_text(json.dumps(plan), encoding="utf-8")
        return path

    def report(self, name):
        return json.loads((self.root / "out" / f"{name}.json").read_text(encoding="utf-8"))

    def test_options_merge_over_defaults_in_plan_order(self):
        path = self.plan({"b": {"restir-seed": 4194385}, "a": {"restir-workload": "distant"}})
        work = commands(load_plan(path), self.root, 1)
        self.assertEqual([name for name, _, _ in work], ["b", "a"])
        self.assertEqual(work[0][2][2:], ["--benchmark-restir", str(self.root / "out" / "b.json"),
                                          "--benchmark-label", "fixture",
                                          "--benchmark-size", "32", "--restir-seed", "4194385"])
        self.assertEqual(work[1][2][-2:], ["--restir-workload", "distant"])

    def test_string_executable_resolves_against_plan(self):
        path = self.plan({"a": {}}, executable="bin/lotus-headless")
        self.assertEqual(commands(load_plan(path), self.root, 1)[0][2][0],
                         str(self.root / "bin" / "lotus-headless"))

    def test_concurrent_runs_label_reports_and_write_manifest(self):
        path = self.plan({name: {"sleep": 0.5} for name in "abcd"})
        started = time.time()
        self.assertEqual(campaign(path, jobs=4, log=lambda _: None), [])
        self.assertLess(time.time() - started, 1.8)
        for name in "abcd":
            self.assertEqual(self.report(name)["label"], "fixture; jobs=4")
        manifest = json.loads((self.root / "out" / "campaign.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["schema"], "lotus-restir-campaign-v1")
        self.assertEqual(manifest["jobs"], 4)
        self.assertEqual([r["status"] for r in manifest["runs"]], ["pass"] * 4)

    def test_serial_label_is_unchanged(self):
        campaign(self.plan({"a": {}}), log=lambda _: None)
        self.assertEqual(self.report("a")["label"], "fixture")

    def test_failures_are_returned_and_passing_reports_resume(self):
        path = self.plan({"good": {}, "bad": {"restir-seed": 13}})
        failed = campaign(path, jobs=2, log=lambda _: None)
        self.assertEqual([r["name"] for r in failed], ["bad"])
        self.assertTrue((self.root / "out" / "bad.log").exists())
        before = (self.root / "out" / "good.json").stat().st_mtime_ns
        lines = []
        failed = campaign(path, log=lines.append)
        self.assertEqual(lines[:2], ["KEEP good", "RUN bad"])
        self.assertEqual([r["name"] for r in failed], ["bad"])
        self.assertEqual((self.root / "out" / "good.json").stat().st_mtime_ns, before)
        manifest = json.loads((self.root / "out" / "campaign.json").read_text(encoding="utf-8"))
        self.assertEqual([(r["name"], r.get("kept", False)) for r in manifest["runs"]],
                         [("good", True), ("bad", False)])

    def test_changed_command_reruns_but_jobs_alone_does_not(self):
        campaign(self.plan({"a": {}}), log=lambda _: None)
        lines = []
        campaign(self.plan({"a": {}}), jobs=3, log=lines.append)
        self.assertEqual(lines, ["KEEP a"])
        lines = []
        campaign(self.plan({"a": {"restir-seed": 18}}), log=lines.append)
        self.assertEqual(lines[0], "RUN a")
        self.assertEqual(self.report("a")["args"]["--restir-seed"], "18")

    def test_dry_run_writes_nothing(self):
        lines = []
        self.assertEqual(campaign(self.plan({"a": {}}), dry_run=True, log=lines.append), [])
        self.assertEqual(lines[0], "RUN a")
        self.assertIn("--benchmark-restir", lines[1])
        self.assertFalse((self.root / "out").exists())

    def test_rejected_plans(self):
        for runs, extra in (({}, {}), ({"a": {"benchmark-label": "x"}}, {}),
                            ({"a": {"benchmark-restir": "x.json"}}, {}), ({"../a": {}}, {}),
                            ({"a": []}, {}), ({"a": {}}, {"label": ""})):
            with self.subTest(runs=runs, extra=extra), self.assertRaises(ValueError):
                load_plan(self.plan(runs, **extra))
        with self.assertRaises(ValueError):
            campaign(self.plan({"a": {}}), jobs=0)


if __name__ == "__main__":
    unittest.main()
