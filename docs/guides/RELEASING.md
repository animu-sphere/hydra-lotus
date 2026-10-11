# Releasing

The [release workflow](../../.github/workflows/release.yml) follows
[hydra-merlin's tag-driven publication](https://github.com/animu-sphere/hydra-merlin/blob/main/.github/workflows/release.yml)
and [hydra-toon's OpenStrata packaging and dry run](https://github.com/animu-sphere/hydra-toon/blob/main/.github/workflows/release.yml).
The workflow is named `release`, the GitHub Release title is
`hydra-lotus vX.Y.Z`, and `vX.Y.Z` is the Git tag. `lotus` remains the
OpenStrata package name; `hdLotus` remains the Hydra module name.

## Prepare and verify

Before tagging, set the version in the top-level `VERSION` file, which
`project(Lotus VERSION)` in `CMakeLists.txt` reads, and make
`[project].version` in `openstrata.toml`, from which OpenStrata names the
package, agree with it. Finalize the dated
`## [X.Y.Z] - YYYY-MM-DD` changelog section, retain an empty `[Unreleased]`
section above it, and write the release record. Phase/release status remains
in the [canonical roadmap](../roadmap/README.md#status-at-a-glance).

The repository-owned helper checks version agreement, stable SemVer and the
finalized changelog. Local verification used:

```sh
python scripts/release.py version --tag v0.2.0
python -m unittest discover -s scripts -p "test_*.py"
ost ci validate
```

Run `release` with `workflow_dispatch` on the prepared branch before tagging.
Manual execution is always a dry run: it uploads `release-bundle` and
renderer evidence as workflow artifacts and never creates or publishes a
GitHub Release. It permits an unfinalized changelog for the pending version.
Pull requests changing the release lane run the same dry run before merge.

Physical-GPU milestone checks and their reports must already be complete.
Hosted GPU SKIPs are explained capability evidence; they do not replace
reference-image, synchronization or physical-GPU checks.

## Tag-driven lane

Pushing a stable `vX.Y.Z` tag starts these jobs:

1. **preflight** checks the tag against both version declarations and requires
   a nonempty, dated changelog section.
2. **checks** reuses `renderer-ci.yml`: documentation and helper regressions,
   runtime-free core, and the digest-pinned Windows CY2026 lookdev/Hydra
   build, tests and strict evidence validation. It packages the Hydra build
   twice and requires identical archive digests, the expected package
   identity, the actual archive hash, all scene shaders, plugin registration,
   references and the headless product.
3. **bundle** adds a source archive from the checked-out commit, renders notes
   from the changelog and computes `SHA256SUMS` over every release asset.
4. **publish** stages assets on a draft and publishes only after every upload
   succeeds. This job alone has `contents: write`. Failed uploads leave a
   retryable draft. A published release is never overwritten by a retry.

The asset names follow OpenStrata and hydra-toon:

| Asset | Name |
| --- | --- |
| Windows Hydra package | `lotus-X.Y.Z-cy2026-windows-x86_64-py313-lookdev--hydra.tar.zst` |
| Package manifest | Same stem with `.manifest.json` |
| SPDX SBOM | Same stem with `.sbom.spdx.json` |
| Source archive | `hydra-lotus-X.Y.Z-src.tar.gz` |
| Checksums | `SHA256SUMS` |

Only the [measured Windows configuration](../reference/SUPPORTED_CONFIGURATIONS.md)
is packaged. The binary package needs the matching OpenUSD 26.08/Python 3.13
runtime and Vulkan; it does not bundle those dependencies. This lane publishes
GitHub assets. Formation-facing OCI distribution is a separate integration.

## Existing first release

The original [first release](../releases/v0.1.0.md) predates this workflow and
was published manually as source. Its GitHub title has been aligned to
`hydra-lotus v0.1.0`. The original tag and release record retain their shipped
snapshot. Adding a workflow later does not retroactively trigger an existing
tag; subsequent tags containing this workflow use the lane above.
