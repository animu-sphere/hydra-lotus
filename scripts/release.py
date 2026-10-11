#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate release identity and assemble OpenStrata Hydra release assets."""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tomllib

ROOT = Path(__file__).resolve().parents[1]
VERSION = re.compile(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)")
TARGET = "cy2026-windows-x86_64-py313-lookdev"
REQUIRED = {
    "lib/usd/hdLotus/hdLotus.dll",
    "lib/usd/hdLotus/resources/plugInfo.json",
    "bin/lotus-headless.exe",
    "bin/reference/cornell-box-mean.pfm",
    "bin/reference/cornell-box-variance.pfm",
    "lib/cmake/Lotus/LotusConfig.cmake",
    "lib/cmake/Lotus/LotusConfigVersion.cmake",
    "share/lotus/openstrata.renderer.yaml",
    "share/lotus/tests/usdview-smoke.usda",
} | {f"{directory}/{shader}.spv"
     for directory in ("bin/shaders", "lib/usd/hdLotus/shaders")
     for shader in ("triangle.vert", "triangle.frag", "path_trace.vert", "path_trace.frag",
                    "wavefront.comp")}


def changelog(root: Path, version: str, allow_unreleased: bool = False) -> str:
    text = (root / "CHANGELOG.md").read_text(encoding="utf-8")
    sections = list(re.finditer(r"^## \[([^\]]+)\]([^\n]*)$", text, re.M))
    names = [version, "Unreleased"] if allow_unreleased else [version]
    for name in names:
        matches = [(i, m) for i, m in enumerate(sections) if m[1] == name]
        if len(matches) > 1:
            raise ValueError(f"duplicate changelog section: {name}")
        if not matches:
            continue
        i, heading = matches[0]
        if not allow_unreleased:
            date = re.fullmatch(r" - (\d{4}-\d{2}-\d{2})", heading[2])
            if not date:
                raise ValueError("release changelog must have a finalized date")
            datetime.date.fromisoformat(date[1])
        end = sections[i + 1].start() if i + 1 < len(sections) else len(text)
        body = text[heading.end():end].strip()
        if not body:
            raise ValueError(f"empty changelog section: {name}")
        return body
    raise ValueError(f"no changelog section for {version}")


def project_version(root: Path, tag: str | None = None,
                    allow_unreleased: bool = False) -> str:
    # CMake reads VERSION; OpenStrata names packages from openstrata.toml.
    version = (root / "VERSION").read_text(encoding="utf-8").removesuffix("\n")
    manifest = tomllib.loads((root / "openstrata.toml").read_text(encoding="utf-8"))
    if not VERSION.fullmatch(version) or manifest["project"]["version"] != version:
        raise ValueError("VERSION and openstrata.toml must agree on stable SemVer")
    if tag is not None and tag != f"v{version}":
        raise ValueError(f"tag {tag} does not match v{version}")
    changelog(root, version, allow_unreleased)
    return version


def package_result(path: Path) -> dict:
    # ost 0.23.14 prints CMake install messages before its JSON result.
    text = path.read_text(encoding="utf-8-sig")
    start = re.search(r"^\{", text, re.M)
    if not start:
        raise ValueError(f"no package JSON in {path}")
    result = json.loads(text[start.start():])
    data = result["data"]
    if result.get("ok") is not True or data.get("packaged") is not True or data.get("validation") != "passed":
        raise ValueError("package did not pass validation")
    if data.get("target") != TARGET:
        raise ValueError(f"release requires the measured target {TARGET}")
    return data


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def stage(version: str, first: Path, second: Path, out: Path) -> None:
    a, b = package_result(first), package_result(second)
    if a["archive_digest"] != b["archive_digest"]:
        raise ValueError("two packagings disagree on archive digest")
    archive = Path(b["archive"])
    stem = f"lotus-{version}-{TARGET}--hydra"
    manifest_path = archive.parent / "manifest.json"
    sbom_path = archive.parent / "sbom.spdx.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if (manifest.get("name"), manifest.get("version"), manifest.get("target")) != ("lotus", version, TARGET):
        raise ValueError("packaged project/version/target does not match the release")
    if archive.name != stem + ".tar.zst" or manifest.get("archive") != archive.name:
        raise ValueError("unexpected Hydra release archive name")
    actual = "sha256:" + digest(archive)
    if actual != b["archive_digest"] or actual != manifest.get("archive_digest"):
        raise ValueError("archive hash does not match package evidence")
    missing = REQUIRED - {entry["path"] for entry in manifest["files"]}
    if missing:
        raise ValueError(f"package is missing required products: {sorted(missing)}")
    sbom = json.loads(sbom_path.read_text(encoding="utf-8"))
    if sbom.get("spdxVersion") != "SPDX-2.3" or sbom.get("name") != f"lotus-{version}":
        raise ValueError("SPDX SBOM does not identify the released package")
    out.mkdir(parents=True, exist_ok=True)
    for source, name in ((archive, archive.name), (manifest_path, stem + ".manifest.json"),
                         (sbom_path, stem + ".sbom.spdx.json")):
        shutil.copyfile(source, out / name)


def bundle(root: Path, version: str, out: Path, allow_unreleased: bool,
           notes_path: Path | None = None) -> None:
    if project_version(root, allow_unreleased=allow_unreleased) != version:
        raise ValueError("bundle version does not match the project")
    stem = f"lotus-{version}-{TARGET}--hydra"
    package_names = [stem + suffix for suffix in (".tar.zst", ".manifest.json", ".sbom.spdx.json")]
    if not all((out / name).is_file() for name in package_names):
        raise ValueError("release bundle has no complete staged Hydra package")
    source_name = f"hydra-lotus-{version}-src.tar.gz"
    subprocess.run(["git", "archive", "--format=tar.gz", f"--prefix=hydra-lotus-{version}/",
                    "-o", str((out / source_name).resolve()), "HEAD"], cwd=root, check=True)
    names = sorted(package_names + [source_name])
    checksums = "".join(f"{digest(out / name)}  {name}\n" for name in names)
    (out / "SHA256SUMS").write_text(checksums, encoding="utf-8", newline="\n")
    body = changelog(root, version, allow_unreleased)
    body = re.sub(r"\]\((?![a-z]+:|#)([^)]+)\)",
                  rf"](https://github.com/animu-sphere/hydra-lotus/blob/v{version}/\1)", body)
    notes = (f"# hydra-lotus v{version}\n\n"
             "Windows x86_64 CY2026 lookdev/Hydra package, manifest, SPDX SBOM, source archive "
             "and SHA256SUMS. Requires a matching OpenUSD 26.08/Python 3.13 runtime and Vulkan.\n\n"
             "Hosted GPU checks may report explained SKIPs; physical-GPU evidence is recorded "
             "in the repository reports.\n\n" + body + "\n\n## SHA-256 checksums\n\n```text\n" + checksums + "```\n")
    (notes_path or root / "release-notes.md").write_text(notes, encoding="utf-8", newline="\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    identity = commands.add_parser("version")
    identity.add_argument("--tag")
    for command in (identity, commands.add_parser("bundle")):
        command.add_argument("--root", type=Path, default=ROOT)
        command.add_argument("--allow-unreleased", action="store_true")
    package = commands.add_parser("stage")
    package.add_argument("--first", type=Path, required=True)
    package.add_argument("--second", type=Path, required=True)
    for command in (package, commands.choices["bundle"]):
        command.add_argument("--version", required=True)
        command.add_argument("--out", type=Path, required=True)
    commands.choices["bundle"].add_argument("--notes", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "version":
            print(project_version(args.root, args.tag, args.allow_unreleased))
        elif args.command == "stage":
            stage(args.version, args.first, args.second, args.out)
        else:
            bundle(args.root, args.version, args.out, args.allow_unreleased, args.notes)
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"release: {error}\n")


if __name__ == "__main__":
    main()
