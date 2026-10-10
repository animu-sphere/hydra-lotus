# Foundation release gate

Verified on 2026-10-10 for LOTUS-RELEASE-01 against `fa54d11`, with only
release documentation changes on top. Renderer implementation, shaders and
committed reference images were unchanged by release preparation. The
[foundation dispositions](2026-10-10-foundation-closure.md) and
[fixed performance baseline](2026-10-10-performance-baseline.md), including
its three-run raw data, are preserved. Release and phase status belong in
the [canonical roadmap](../roadmap/README.md#status-at-a-glance).

## Local environment and commands

Windows 11 x86_64, MSVC 14.51, OpenStrata 0.23.14, Release and NVIDIA RTX
A5000 (Vulkan device API 1.4.329). Hydra used the adopted OpenUSD 26.08
lookdev runtime with Python 3.13. No public headers were changed in this
release preparation; the existing Japanese MSVC clean-rebuild workaround
remains an accepted tooling constraint.

| Invocation | Result |
| --- | --- |
| `ost build --jobs auto` | Passed. |
| `ost test` | 17/17 CTest entries passed. |
| `ost validate --strict-renderer-evidence` | Passed; physical-GPU evidence, with only the expected absent-Hydra checks skipped. |
| `ost build --profile lookdev --intent hydra --jobs auto` | Passed. |
| `ost test --profile lookdev --intent hydra` | 34/34 CTest entries passed, including usdview. |
| `ost validate --profile lookdev --intent hydra --strict-renderer-evidence` | Passed; all 30 renderer assertions passed. |
| `ost build --without-runtime --intent ci-core --jobs auto` | Passed with Vulkan disabled. |
| `ost test --without-runtime --intent ci-core` | 9/9 CTest entries passed. |
| `python scripts/check_docs.py` | Passed. |
| `python -m unittest discover -s scripts -p test_check_docs.py` | 14/14 checker regressions passed. |
| `ost ci validate` | Both declared CI cells validated. |

The renderer reports are completion-bound to producers
`ost-test-5956ee5db39cb32b` (core), `ost-test-09e2b7622edfbe83` (Hydra) and
`ost-test-fb7a3d26590068a8` (runtime-free). Their reports and CTest logs
remain in the corresponding `build/` trees. Runtime-free evidence has
two PASS assertions (core boundary and install tree), 28 explained SKIPs
and no failures; GPU-disabled evidence is not a physical-GPU pass.

## Required gate observations

- Both physical-GPU configurations rendered 1,000 deterministic bootstrap
  frames. Synchronization validation was enabled, with zero captured
  validation messages. Synchronized writes and intentional missing-barrier
  hazard detection passed, as did the isolated missing-driver regressions.
- The fixed Cornell scene at 1, 16, 64, 256 and 1024 spp passed the committed
  mean/variance tolerance. The committed sample sequence reproduced the
  reference bit for bit; a 10% brighter red wall was rejected (maximum
  absolute z score 6.32). References were not regenerated. Scene/seed and
  tolerance definitions remain in the
  [reference-image contract](../reference/SCENE.md#reference-images).
- Hydra discovery, delegate creation, CPU buffers, deterministic sample
  settings, materials/textures/normals/opacity, instancing and render-pass
  selection passed. usdview first-frame and stable-update evidence passed;
  screenshots remain in its installed smoke-test tree.
- Benchmark report/CLI contracts and the missing-driver benchmark regression
  passed. The existing four-workload baseline remains the performance
  evidence; this release verification does not claim a new timing baseline.
- The standalone viewport's shared-core/bootstrap disposition remains the
  previously measured [foundation review](2026-10-10-foundation-closure.md).
  This session did not rerun the viewport.

## Hosted evidence

The implementation's
[PR run at `40e3e57`](https://github.com/animu-sphere/hydra-lotus/actions/runs/38025096248)
passed all three jobs: docs, runtime-free core (9/9 CTest entries) and
Hydra (34/34 entries, zero failures). Its core GPU SKIP states that Vulkan
was not compiled. Hosted Hydra capability handling is checked separately
from the physical-GPU results above; a successful hosted job does not
establish that a GPU was available.

## Scope disposition

No foundation implementation task remains after these gates. Wavefront
equivalence and the deferred light/material tasks retain their named phase
assignments in [current.md](../roadmap/current.md). Known bootstrap,
material, platform and tooling constraints remain with their behavior in
[reference documentation](../reference/SUPPORTED_CONFIGURATIONS.md#foundation-bootstrap-limitations).
The release record describes the shipped source snapshot.
