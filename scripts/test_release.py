"""Regressions for failures that must stop a release before publication."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import release


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="lotus-release-check-", dir=Path(__file__).resolve().parent)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "openstrata.toml").write_text('[project]\nversion = "0.1.0"\n', encoding="utf-8")
        (self.root / "VERSION").write_text("0.1.0\n", encoding="utf-8")
        self.write_changelog('## [Unreleased]\n\n## [0.1.0] - 2026-10-10\n\nShipped [scope](docs/releases/v0.1.0.md).\n')
        self.stem = f"lotus-0.1.0-{release.TARGET}--hydra"
        self.archive = self.root / (self.stem + ".tar.zst")
        self.archive.write_bytes(b"a reproducible archive fixture")
        self.hash = "sha256:" + hashlib.sha256(self.archive.read_bytes()).hexdigest()
        self.manifest = dict(name="lotus", version="0.1.0", target=release.TARGET,
                             archive=self.archive.name, archive_digest=self.hash,
                             files=[{"path": name} for name in release.REQUIRED])
        self.write_manifest()
        (self.root / "sbom.spdx.json").write_text(json.dumps({"spdxVersion": "SPDX-2.3", "name": "lotus-0.1.0"}), encoding="utf-8")
        self.result = dict(ok=True, data=dict(packaged=True, validation="passed",
                           archive=str(self.archive), archive_digest=self.hash, target=release.TARGET))
        self.first = self.root / "first.log"
        self.second = self.root / "second.log"
        self.write_results()
        self.out = self.root / "staged"

    def write_changelog(self, text):
        (self.root / "CHANGELOG.md").write_text(text, encoding="utf-8")

    def write_manifest(self):
        (self.root / "manifest.json").write_text(json.dumps(self.manifest), encoding="utf-8")

    def write_results(self):
        for path in (self.first, self.second):
            path.write_text("-- CMake install log\n" + json.dumps(self.result, indent=2), encoding="utf-8-sig")

    def stage(self):
        release.stage("0.1.0", self.first, self.second, self.out)

    def test_matching_tag_and_versions(self):
        self.assertEqual(release.project_version(self.root, "v0.1.0"), "0.1.0")

    def test_wrong_or_prerelease_tags_are_rejected(self):
        for tag in ("v0.1.1", "0.1.0", "v0.1.0-rc.1", "v00.1.0"):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                release.project_version(self.root, tag)

    def test_version_file_manifest_disagreement(self):
        (self.root / "VERSION").write_text("0.2.0\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            release.project_version(self.root)

    def test_malformed_version_file(self):
        for text in ("", "0.1.0\n\n", " 0.1.0\n", "v0.1.0\n", "0.1.0-rc.1\n"):
            (self.root / "VERSION").write_text(text, encoding="utf-8")
            (self.root / "openstrata.toml").write_text(f'[project]\nversion = "{text.strip()}"\n', encoding="utf-8")
            with self.subTest(text=text), self.assertRaises(ValueError):
                release.project_version(self.root)

    def test_invalid_or_undated_changelog(self):
        for heading in ("## [0.1.0]", "## [0.1.0] - 2026-02-30"):
            self.write_changelog(heading + "\n\nContent.\n")
            with self.subTest(heading=heading), self.assertRaises(ValueError):
                release.project_version(self.root, "v0.1.0")

    def test_duplicate_and_empty_changelog(self):
        for text in ("## [0.1.0] - 2026-10-10\n", "## [0.1.0] - 2026-10-10\nOne\n## [0.1.0] - 2026-10-10\nTwo\n"):
            self.write_changelog(text)
            with self.assertRaises(ValueError):
                release.project_version(self.root)

    def test_dry_run_accepts_unreleased_without_finalized_section(self):
        self.write_changelog("## [Unreleased]\n\nNext milestone.\n")
        self.assertEqual(release.project_version(self.root, allow_unreleased=True), "0.1.0")
        with self.assertRaises(ValueError):
            release.project_version(self.root, "v0.1.0")

    def test_valid_package_is_staged_with_unambiguous_names(self):
        self.stage()
        self.assertEqual({p.name for p in self.out.iterdir()},
                         {self.stem + suffix for suffix in (".tar.zst", ".manifest.json", ".sbom.spdx.json")})

    def test_nondeterministic_packaging(self):
        different = json.loads(json.dumps(self.result))
        different["data"]["archive_digest"] = "sha256:" + "0" * 64
        self.second.write_text(json.dumps(different), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "two packagings"):
            self.stage()
        self.assertFalse(self.out.exists())

    def test_failed_validation_or_wrong_target(self):
        for key, value in (("validation", "failed"), ("target", "cy2026-linux-x86_64-py313-core")):
            original = self.result["data"][key]
            self.result["data"][key] = value
            self.write_results()
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.stage()
            self.result["data"][key] = original

    def test_corrupt_archive(self):
        self.archive.write_bytes(b"corruption")
        with self.assertRaisesRegex(ValueError, "archive hash"):
            self.stage()

    def test_wrong_package_identity_and_missing_shader(self):
        self.manifest["version"] = "0.2.0"
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "version"):
            self.stage()
        self.manifest["version"] = "0.1.0"
        self.manifest["files"] = [p for p in self.manifest["files"] if not p["path"].endswith("path_trace.frag.spv")]
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "missing required products"):
            self.stage()

    def test_bundle_checksums_cover_each_asset_and_links_use_tag(self):
        self.stage()
        def archive_fixture(command, **kwargs):
            Path(command[command.index("-o") + 1]).write_bytes(b"source archive fixture")
        with patch.object(release.subprocess, "run", side_effect=archive_fixture):
            release.bundle(self.root, "0.1.0", self.out, False)
        lines = (self.out / "SHA256SUMS").read_text().splitlines()
        self.assertEqual(len(lines), 4)
        for line in lines:
            expected, name = line.split("  ")
            self.assertEqual(expected, hashlib.sha256((self.out / name).read_bytes()).hexdigest())
        notes = (self.root / "release-notes.md").read_text()
        self.assertIn("# hydra-lotus v0.1.0", notes)
        self.assertIn("https://github.com/animu-sphere/hydra-lotus/blob/v0.1.0/docs/releases/v0.1.0.md", notes)


if __name__ == "__main__":
    unittest.main()
