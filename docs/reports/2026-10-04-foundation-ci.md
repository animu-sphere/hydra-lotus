# Foundation CI contracts and host capability gating

Measured on 2026-10-04 in `hydra-lotus`, Windows 11 x86_64, MSVC 14.51,
OpenStrata 0.23.14, Release. Hydra used the installed canonical OpenUSD 26.08
lookdev GL runtime, CPython 3.13 and NVIDIA RTX A5000. No GitHub-hosted job was
run in this session.

## CI definition

`openstrata.ci.yaml` declares two source cells. The registered external
workflow builds the `ci-core` intent without a runtime or Vulkan backend and
the `hydra` intent against a digest-pinned canonical lookdev runtime. It runs
on PRs, main pushes and manual dispatch, uses read-only repository permissions,
checksums CLI/SDK downloads and uploads renderer reports and CTest logs.

The Hydra pins are the Windows leaf from OpenStrata's
[canonical runtime report](https://github.com/animu-sphere/open-strata/blob/main/docs/reports/2026-09-26-v0.23.11-renderer-formations.md#canonical-runtime-evidence):

- Artifact: `sha256:b982656c07dd9147973e3c7197d9b050ee48dd1ac291988f7e6025c76c4a785b`.
- OCI manifest: `sha256:b840ed4690aa39d4582bc03c0a09fa7bab717216635b55a4eb718482dbfb196b`.
- Materialized runtime: `sha256:330d3e83f9d79b3fb28d541e776dfa0db68f2ca2e953677748b0f78126d036c6`.

`ost ci validate` passes structural and external-workflow consumption checks.
`ost ci validate --resolve` resolves every artifact pin in the local registry.
`ost artifact verify` passes archive and 13,681 file checks, SBOM, provenance,
the local trust floor and `cy2026/windows/x86_64/gl` at OpenUSD 26.08. Anonymous
transport and hosted SDK/CLI installation were not rerun here.

## Local renderer checks

`ost build --without-runtime --intent ci-core` and
`ost test --without-runtime --intent ci-core` pass all 6 tests. The primary
report records core boundary and install-tree PASS, and explained GPU SKIPs.
The initial sandbox could not run the host toolchain; verification used the
normal workstation toolchain outside that sandbox and an explicit installed
Ninja executable. No project/compiler setting was changed to accommodate it.

`ost build --profile lookdev --intent hydra`, its matching `ost test` and
`ost validate --profile lookdev --intent hydra --strict-renderer-evidence`
pass: 11/11 tests including AOV integration, first-frame/stable-update usdview,
recursive header discovery and the new host capability gate. GPU validation
messages remain zero on the bootstrap triangle; this is foundation evidence,
not a path tracer measurement.

The host capability regression checks PASS, explained SKIP, FAIL, unknown
status, absent assertion/report, empty SKIP detail and malformed JSON. It also
exercises the host wrapper with an explicit SKIP and confirms the existing
staging tree is preserved and the viewer is not launched. The ordinary Hydra
run exercises the PASS path.

## OpenStrata 0.23.14 limitations

1. Generated workspace CI runs `ost plugin test --workspace --graph-only`.
   Here it fails with `PRECONDITION_FAILED`: no bundles, libraries or tools
   in the workspace member set. This renderer intentionally has internal
   CMake targets rather than plugin workspace descriptors. Use the supported
   `external_workflows` registration until a renderer source-cell shape exists.
2. `ost ci matrix --github-output` omits `host_python`, `require_openusd` and
   `require_openusd_version`. The workflow reads those from the JSON projection
   instead, so the matrix remains their owner.
3. `ost validate --intent ci-core` resolves the runtime-consuming target and
   cannot find the runtime-free completion. `--build-dir` cannot verify that
   report's managed producer either, and a full runtime-free ID is not accepted
   as `--target`. The core job uses build/test completions plus explicit checks
   of its own renderer PASS/SKIP assertions; Hydra uses strict OST validation.

The first hosted run and generator adoption remain open in the
[roadmap](../roadmap/README.md#status-at-a-glance). This report does
not claim hosted CI or Renderer Phase 0 is complete.
