# Tag-driven release workflow verification

Measured on 2026-10-10 against `ad564be` plus the release-lane changes.
The implementation follows hydra-merlin's tag trigger and automatic GitHub
publication, and hydra-toon's OpenStrata Hydra packaging, naming and dry run.
The [release guide](../guides/RELEASING.md) owns the procedure; the original
[first-release record](../releases/v0.1.0.md) retains its source-only snapshot.

## Local checks

The existing validated Release Hydra build was packaged on Windows 11
x86_64, MSVC 14.51, OpenStrata 0.23.14 and OpenUSD 26.08/Python 3.13 lookdev.
The [first-release gate](2026-10-10-release-gate.md) records its physical-GPU
correctness, synchronization, deterministic Cornell sample sequences and
usdview observations. Renderer code and reference images were unchanged by
the workflow implementation.

Two `ost package --profile lookdev --intent hydra --json` invocations returned
the same archive digest:

```text
sha256:907b4395321d417aea5446aba08c462e43b649e88e541875e46e39893bd9e0e9
```

The 1,231,976-byte archive contains 28 manifest entries. The helper staged
the archive, manifest and SPDX-2.3 SBOM under the OpenStrata package stem,
checked the actual archive digest and required Hydra registration, shaders,
headless product, references and CMake configuration, then assembled a
source archive, four-file `SHA256SUMS` and notes with links to the version tag.
The logs and bundle remain under `build/`.

Commands run:

```sh
python scripts/release.py version --tag v0.1.0
python -m unittest discover -s scripts -p "test_*.py"
python scripts/check_docs.py
ost ci validate
python scripts/release.py stage --version 0.1.0 --first build/release-package-first.log --second build/release-package-second.log --out build/tag-release-bundle
python scripts/release.py bundle --version 0.1.0 --out build/tag-release-bundle --notes build/tag-release-notes.md
build/actionlint/bin/actionlint.exe -color .github/workflows/release.yml .github/workflows/renderer-ci.yml
```

All checks passed: 26 Python regressions (14 documentation and 12 release),
documentation ownership/links, both OpenStrata CI cells and actionlint 1.7.12.
The actionlint archive was verified against the official release checksum.
Release regressions reject mismatched/prerelease tags, divergent project
versions, missing/invalid/duplicate changelog sections, failed package
validation, unmeasured targets, nondeterministic packaging, corrupt archives,
wrong package identity and missing scene shaders. Bundle tests verify every
asset's checksum and tagged Markdown links.

## Hosted boundary

Release-lane pull requests exercise the reusable docs/core/Hydra workflow,
package the actual hosted Hydra build and upload a complete dry-run bundle.
Manual dispatch uses the same read-only path. Only tag pushes reach the
publication job; it alone has repository write permission. Assets are staged
on a draft before publication, and retries refuse an already-published release.
The host's explained GPU SKIPs do not substitute for the physical-GPU evidence
linked above. Workflow-run artifacts retain the measured hosted outputs.

## Existing release naming

The GitHub Release title was changed to `hydra-lotus v0.1.0`, matching
hydra-toon's repository-based titles. Its annotated tag still resolves to
`ad564be2aae01f16065e0c2f704574028a0ead76`. The release lane is added after
that tag, so its implementation does not retroactively trigger or replace
the existing source release.
