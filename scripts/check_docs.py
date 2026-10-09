#!/usr/bin/env python3
"""Validate repository documentation without third-party dependencies.

Checks local Markdown links/anchors, recursive category indexes and the
ownership conventions in docs/contributing/documentation.md. External URLs
are deliberately not fetched. Paths in command examples are not hyperlinks.
"""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
import html
from pathlib import Path
import re
from urllib.parse import unquote, urlsplit


CATEGORIES = (
    "architecture", "design", "reference", "roadmap", "reports", "guides",
    "releases", "archive", "contributing",
)
PHASE = re.compile(r"Renderer Phase (\d+(?:\.\d+)?)\b")
STATE = re.compile(r"✅|🚧|⬜|⛔|⚠️|\b(?:done|in progress|not started|blocked)\b", re.I)
TASK = re.compile(r"^\s*[-*] \[([ xX])\]\s+\*\*([A-Z][A-Z0-9]*(?:-[A-Z0-9]+)+)\b")
INLINE_LINK = re.compile(
    r"!?\[[^\]\n]*(?:\[[^\]\n]*\][^\]\n]*)?\]\(\s*"
    r"(<[^>\n]+>|(?:\\.|[^()\s]|\([^()\n]*\))+)"
    r"(?:\s+[\"'][^\n]*?[\"'])?\s*\)"
)
DEFINITION = re.compile(r"^\s{0,3}\[([^\]]+)\]:\s*(<[^>]+>|\S+)", re.M)
REFERENCE_LINK = re.compile(r"!?\[([^\]\n]+)\]\[([^\]\n]*)\]")


@dataclass(frozen=True)
class Link:
    destination: str
    line: int


def prose(source: str) -> str:
    """Blank fenced code and comments while preserving line numbers."""
    source = re.sub(r"<!--.*?-->", lambda m: "\n" * m[0].count("\n"), source, flags=re.S)
    result = []
    fence = None
    for line in source.splitlines():
        marker = re.match(r"^\s{0,3}(`{3,}|~{3,})", line)
        if fence:
            if marker and marker[1][0] == fence[0] and len(marker[1]) >= len(fence):
                fence = None
            result.append("")
        elif marker:
            fence = marker[1]
            result.append("")
        else:
            result.append(line)
    return "\n".join(result)


def without_inline_code(source: str) -> str:
    return re.sub(r"(`+).*?\1", lambda m: " " * len(m[0]), source)


def links(source: str) -> list[Link]:
    source = without_inline_code(prose(source))
    found = [Link(m[1].strip("<>"), source.count("\n", 0, m.start()) + 1)
             for m in INLINE_LINK.finditer(source)]
    definitions = {m[1].casefold(): m[2].strip("<>") for m in DEFINITION.finditer(source)}
    # Definitions are checked even when the reference has no remaining uses.
    found.extend(Link(m[2].strip("<>"), source.count("\n", 0, m.start()) + 1)
                 for m in DEFINITION.finditer(source))
    for match in REFERENCE_LINK.finditer(source):
        key = (match[2] or match[1]).casefold()
        destination = definitions.get(key, f"missing-reference:{key}")
        found.append(Link(destination, source.count("\n", 0, match.start()) + 1))
    return found


def anchors(source: str) -> set[str]:
    source = prose(source)
    result = set(re.findall(r'<a\s+(?:[^>]*?\s)?(?:id|name)=["\']([^"\']+)', source, re.I))
    counts: Counter[str] = Counter()
    for line in source.splitlines():
        heading = re.match(r"^\s{0,3}#{1,6}\s+(.+?)(?:\s+#+\s*)?$", line)
        if not heading:
            continue
        title = re.sub(r"!?\[([^\]]+)\]\([^)]*\)", r"\1", heading[1])
        title = html.unescape(re.sub(r"<[^>]*>", "", title)).lower()
        slug = "".join(c for c in title if c.isalnum() or c in "_ -")
        slug = slug.replace(" ", "-")
        count = counts[slug]
        counts[slug] += 1
        result.add(f"{slug}-{count}" if count else slug)
    return result


def check_docs(root: Path) -> list[str]:
    root = root.resolve()
    errors: list[str] = []
    docs = root / "docs"
    sources = {p.resolve(): p.read_text(encoding="utf-8-sig") for p in docs.rglob("*.md")}
    for name in ("README.md", "CHANGELOG.md"):
        p = root / name
        if p.is_file():
            sources[p.resolve()] = p.read_text(encoding="utf-8-sig")

    def error(path: Path, line: int, message: str) -> None:
        errors.append(f"{path.relative_to(root).as_posix()}:{line}: {message}")

    def historical(path: Path) -> bool:
        relative = path.relative_to(root).parts
        return (
            relative[:2] in (("docs", "archive"), ("docs", "reports"), ("docs", "releases"))
            and path.name != "README.md"
        ) or path.name == "CHANGELOG.md"

    resolved: dict[Path, set[Path]] = {}
    anchor_sets = {p: anchors(s) for p, s in sources.items()}
    for path, source in sources.items():
        resolved[path] = set()
        for link in links(source):
            destination = link.destination
            if destination.startswith("missing-reference:"):
                error(path, link.line, f"undefined link reference: {destination.split(':', 1)[1]}")
                continue
            parsed = urlsplit(destination)
            if parsed.scheme or parsed.netloc:
                if re.match(r"^[A-Za-z]:[\\/]", destination) or parsed.scheme == "file":
                    error(path, link.line, "machine-local link is not portable")
                continue
            if parsed.path.startswith(("/", "\\")):
                error(path, link.line, f"use a repository-relative link: {destination}")
                continue
            target = (path.parent / unquote(parsed.path)).resolve() if parsed.path else path
            if not target.is_relative_to(root):
                error(path, link.line, f"link leaves repository: {destination}")
                continue
            if not target.exists():
                error(path, link.line, f"missing link target: {destination}")
                continue
            if target.is_dir() and target.is_relative_to(docs):
                target = target / "README.md"
                if not target.is_file():
                    error(path, link.line, f"documentation directory has no index: {destination}")
                    continue
            resolved[path].add(target)
            if parsed.fragment and target.suffix.lower() == ".md":
                if target not in anchor_sets:
                    anchor_sets[target] = anchors(target.read_text(encoding="utf-8-sig"))
                if unquote(parsed.fragment) not in anchor_sets[target]:
                    error(path, link.line, f"missing heading anchor: {destination}")
            archive = docs / "archive"
            if (not path.is_relative_to(archive) and not historical(path)
                    and target.is_relative_to(archive) and target != archive / "README.md"):
                error(path, link.line, "active guidance links to an archived document")

    for category in CATEGORIES:
        index = docs / category / "README.md"
        if not index.is_file():
            error(docs / "README.md", 1, f"missing category index: {category}/README.md")
    # Every directory containing documentation has an index. The parent index
    # links immediate documents and subcategory indexes; grandchildren are not copied.
    directories = {docs}
    for path in sources:
        if path.is_relative_to(docs):
            directories.update(parent for parent in path.parents if parent.is_relative_to(docs))
    for directory in sorted(directories):
        index = directory / "README.md"
        if not index.is_file():
            error(directory, 1, "missing documentation index README.md")
            continue
        expected = {p for p in sources if p.parent == directory and p != index}
        expected.update(d / "README.md" for d in directories if d.parent == directory)
        for target in sorted(expected - resolved.get(index, set())):
            error(index, 1, f"unindexed document: {target.relative_to(directory).as_posix()}")

    canonical = docs / "roadmap" / "README.md"
    current = docs / "roadmap" / "current.md"
    tasks: dict[str, Path] = {}
    canonical_rows: dict[str, str] = {}
    in_status_table = False
    for path, source in sources.items():
        if historical(path):
            if path.is_relative_to(docs / "archive"):
                header = source[:1000]
                if not re.search(r"Historical only|not authoritative", header, re.I):
                    error(path, 1, "archive requires a non-authoritative banner")
                if not re.search(r"Archived on \d{4}-\d{2}-\d{2}", header):
                    error(path, 1, "archive requires an archived date")
                if not re.search(r"Superseded by", header, re.I) or not links(header):
                    error(path, 1, "archive requires a linked superseding owner")
            continue
        for number, line in enumerate(prose(source).splitlines(), 1):
            if path == canonical and line.startswith("## "):
                in_status_table = line == "## Status at a glance"
            phase = PHASE.search(line)
            if path != canonical and re.search(
                r"(?:this (?:table|page|document).*(?:owns|canonical)|single source of truth)"
                r".*(?:phase status|release targets)", line, re.I
            ):
                error(path, number, "duplicate canonical status declaration")
            if path != canonical and re.match(r"\|\s*Phase\s*\|\s*Status\s*\|", line, re.I):
                error(path, number, "phase status table belongs only in the canonical roadmap")
            if phase and STATE.search(line) and line.startswith("|"):
                if path != canonical or not in_status_table:
                    error(path, number, "phase status belongs only in the canonical status table")
                else:
                    if phase[1] in canonical_rows:
                        error(path, number, f"duplicate phase status: {phase[1]}")
                    canonical_rows[phase[1]] = line
                    if "✅" in line and not any("reports/" in link.destination for link in links(line)):
                        error(path, number, "completed phase requires a report evidence link")
            if phase and re.search(r"(?:=|\bis\b)\s*(?:✅|🚧|⬜|done\b|not started\b|planned\b|in progress\b)", line, re.I):
                error(path, number, "phase status prose duplicates the canonical table")
            if path != canonical and re.search(r"\bv\d+\.\d+\.\d+\b", line):
                if phase or re.search(r"(?:target|planned|aiming)\s+(?:release\s+|for\s+|is\s+)?v\d", line, re.I):
                    error(path, number, "release target belongs only in the canonical status table")
            if line.startswith("#") and re.search(r"(?<!Renderer )\bPhase \d", line):
                error(path, number, "qualify phase headings as Renderer Phase")
            if re.match(r"\s*[-*] \[[ xX]\]", line):
                task = TASK.match(line)
                if path != current:
                    error(path, number, "active task checklists belong only in roadmap/current.md")
                if not task:
                    error(path, number, "task requires a stable ID, e.g. **LOTUS-CI-01 — Title.**")
                elif task[1] != " ":
                    error(path, number, "remove completed tasks; link evidence from the canonical roadmap")
                else:
                    if task[2] in tasks:
                        error(path, number, f"duplicate active task ID: {task[2]}")
                    tasks[task[2]] = path
        # Existing design metadata is consumed here; it describes decisions,
        # not renderer phase status. Category indexes have no decision metadata.
        if path.parent == docs / "design" and path.name != "README.md":
            metadata = re.match(r"\A---\n(.*?)\n---", source, re.S)
            if not metadata or not re.search(r"^owner: hydra-lotus$", metadata[1], re.M):
                error(path, 1, "design metadata requires owner: hydra-lotus")
            if not metadata or not re.search(r"^status: (proposed|accepted|binding|superseded|rejected)$", metadata[1], re.M):
                error(path, 1, "design metadata requires a valid decision status")
            if metadata and re.search(r"^status: superseded$", metadata[1], re.M):
                replacement = re.search(r"^canonical: (.+)$", metadata[1], re.M)
                target = (path.parent / replacement[1]).resolve() if replacement else None
                if not target or not target.is_relative_to(root) or not target.is_file():
                    error(path, 1, "superseded design requires an existing canonical replacement")
                elif target not in resolved[path] or target.is_relative_to(docs / "archive"):
                    error(path, 1, "superseded design must link an active canonical replacement")
    policy = sources.get(docs / "design" / "ROADMAP_POLICY.md", "")
    defined = set(re.findall(r"^### Renderer Phase (\d+(?:\.\d+)?)\b", policy, re.M))
    if not defined or set(canonical_rows) != defined:
        error(canonical, 1, "canonical phase rows must match ROADMAP_POLICY.md phase definitions")
    return sorted(errors)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    errors = check_docs(args.root)
    if errors:
        print("\n".join(errors))
        print(f"Documentation check failed: {len(errors)} error(s).")
        return 1
    print("Documentation check passed: links, anchors, indexes and ownership conventions.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
