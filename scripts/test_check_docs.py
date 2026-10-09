"""Small repository fixtures exercise the failures that documentation CI owns."""

from pathlib import Path
import tempfile
import unittest

from check_docs import CATEGORIES, anchors, check_docs, links


class DocumentationChecks(unittest.TestCase):
    def setUp(self):
        self.workspace = tempfile.TemporaryDirectory(
            prefix="lotus-doc-check-", dir=Path(__file__).resolve().parent
        )
        self.addCleanup(self.workspace.cleanup)
        self.root = Path(self.workspace.name)
        self.write("README.md", "# Lotus\n\n[Docs](docs/README.md)\n")
        self.write("docs/README.md", "# Docs\n\n" + "\n".join(
            f"[{name}]({name}/README.md)" for name in CATEGORIES
        ))
        for category in CATEGORIES:
            self.write(f"docs/{category}/README.md", f"# {category}\n")
        self.write("docs/design/README.md", "# Design\n\n[Policy](ROADMAP_POLICY.md)\n")
        self.write("docs/design/ROADMAP_POLICY.md", "---\nstatus: accepted\nowner: hydra-lotus\n---\n\n# Policy\n\n### Renderer Phase 0 — Foundation\n")
        self.write("docs/roadmap/README.md", "# Roadmap\n\n[Current](current.md)\n\n## Status at a glance\n\n| Phase | Status | Target |\n| --- | --- | --- |\n| Renderer Phase 0 — Foundation | 🚧 in progress | v0.1.0 |\n")
        self.write("docs/roadmap/current.md", "# Current\n\n- [ ] **LOTUS-CI-01 — Hosted evidence.** Verify the result.\n")

    def write(self, name, source):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(source, encoding="utf-8")

    def append(self, name, source):
        path = self.root / name
        self.write(name, path.read_text(encoding="utf-8") + source)

    def assert_error(self, fragment):
        errors = check_docs(self.root)
        self.assertTrue(any(fragment in error for error in errors), errors)

    def test_valid_repository_and_arbitrary_working_directory(self):
        self.assertEqual(check_docs(self.root), [])

    def test_missing_file_and_anchor(self):
        self.append("README.md", "\n[Missing](docs/gone.md)\n[Anchor](docs/README.md#absent)\n")
        self.assert_error("missing link target")
        self.assert_error("missing heading anchor")

    def test_directory_index_and_new_document(self):
        self.write("docs/reference/NEW.md", "# New\n")
        self.assert_error("unindexed document: NEW.md")
        self.write("docs/reference/nested/NEW.md", "# New\n")
        self.assert_error("missing documentation index")
        self.write("docs/reference/empty-parent/deep/NEW.md", "# New\n")
        self.assert_error("docs/reference/empty-parent:1: missing documentation index")

    def test_reference_links_and_encoded_path(self):
        self.write("docs/reference/with space.md", "# Space\n")
        self.append("docs/reference/README.md", "\n[Space][s]\n\n[s]: with%20space.md#space\n")
        self.assertEqual(check_docs(self.root), [])
        self.append("README.md", "\n[Missing][undefined]\n")
        self.assert_error("undefined link reference")

    def test_code_examples_and_external_links_are_not_local_targets(self):
        self.append("README.md", "\n```md\n[x](missing.md)\n```\n`[x](also-missing.md)`\n<!-- [x](hidden.md) -->\n[Web](https://example.invalid/page)\n")
        self.assertEqual(check_docs(self.root), [])

    def test_duplicate_phase_status_and_target(self):
        self.append("docs/reference/README.md", "\n| Renderer Phase 0 | 🚧 | unscheduled |\nTarget release v0.1.0\n")
        self.assert_error("phase status belongs only")
        self.assert_error("release target belongs only")
        self.append("docs/roadmap/README.md", "| Renderer Phase 0 — Foundation | 🚧 | v0.1.0 |\n")
        self.assert_error("duplicate phase status")
        self.append("docs/reference/README.md", "\nThis page owns canonical phase status.\n")
        self.assert_error("duplicate canonical status declaration")

    def test_completed_phase_requires_evidence(self):
        path = self.root / "docs/roadmap/README.md"
        self.write("docs/roadmap/README.md", path.read_text(encoding="utf-8").replace("🚧 in progress", "✅ done"))
        self.assert_error("completed phase requires a report")

    def test_checklist_ownership_duplicate_ids_and_completed_tasks(self):
        self.append("docs/roadmap/current.md", "\n- [ ] **LOTUS-CI-01 — Duplicate.**\n- [x] **LOTUS-CI-02 — Completed.**\n")
        self.append("docs/guides/README.md", "\n- [ ] **LOTUS-GUIDE-01 — Wrong owner.**\n")
        self.assert_error("duplicate active task ID")
        self.assert_error("remove completed tasks")
        self.assert_error("active task checklists belong only")

    def test_archive_is_indexed_only_by_archive(self):
        self.write("docs/archive/old.md", "# Old\n\n> Historical only. Archived on 2026-10-09.\n> Superseded by [Current](../roadmap/current.md).\n")
        self.append("docs/archive/README.md", "\n[Old](old.md)\n")
        self.assertEqual(check_docs(self.root), [])
        self.append("docs/design/README.md", "\n[Old guidance](../archive/old.md)\n")
        self.assert_error("active guidance links to an archived document")

    def test_archive_banner_and_date(self):
        self.write("docs/archive/old.md", "# Old\n")
        self.append("docs/archive/README.md", "\n[Old](old.md)\n")
        self.assert_error("non-authoritative banner")
        self.assert_error("archived date")
        self.assert_error("linked superseding owner")

    def test_design_metadata_and_phase_names(self):
        self.write("docs/design/ROADMAP_POLICY.md", "# Policy\n\n### Phase 0 — Foundation\n")
        self.assert_error("design metadata requires owner")
        self.assert_error("qualify phase headings")
        self.assert_error("canonical phase rows must match")

    def test_link_cannot_escape_repository(self):
        self.append("README.md", "\n[Outside](../outside.md)\n[Local](file:///C:/scene.md)\n")
        self.assert_error("link leaves repository")
        self.assert_error("machine-local link")

    def test_superseded_design_links_its_replacement(self):
        self.write("docs/design/old.md", "---\nstatus: superseded\nowner: hydra-lotus\ncanonical: ROADMAP_POLICY.md\n---\n\n# Old\n")
        self.append("docs/design/README.md", "\n[Old](old.md)\n")
        self.assert_error("superseded design must link")
        self.append("docs/design/old.md", "\n[Replacement](ROADMAP_POLICY.md)\n")
        self.assertEqual(check_docs(self.root), [])

    def test_github_heading_duplicates_and_balanced_link_parentheses(self):
        self.assertEqual(anchors("# Renderer Phase 1.5 — Minimal `IR`\n# Same\n# Same\n"),
                         {"renderer-phase-15--minimal-ir", "same", "same-1"})
        self.assertEqual(links("[File](image_(1).png)\n")[0].destination, "image_(1).png")


if __name__ == "__main__":
    unittest.main()
