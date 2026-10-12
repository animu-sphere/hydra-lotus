# Hydra temporal integration and Renderer Phase 4 exit review

Measured on 2026-10-13 against `cd94723` plus LOTUS-TEMPORAL-05, with MSVC
Release, Vulkan/Slang and OpenUSD 26.08 lookdev on Windows, using an
NVIDIA RTX A5000. The runtime-free configuration disables Vulkan. The
[temporal reference](../reference/TEMPORAL.md#hydra-controls-and-debug-aovs)
owns current controls and limits; the
[roadmap](../roadmap/README.md#status-at-a-glance) owns phase/release status.

## Moving USD stage and AOV checks

`lotus-renderer-hydra-temporal-contract` runs without a GPU. It verifies
descriptor types/defaults and synthetic motion/history records, including
full-frame bottom-up storage, mapped-write rejection, malformed extents,
short records and zero history length in metadata-only mode.

`lotus-renderer-hydra-temporal-gpu` drives a composed USD stage through
UsdImaging, Hydra extraction and actual render passes under both integrators.
The stage has a floor, back wall and tilted panel, a perspective camera at
(0, 1, 4), a fixed white environment and no analytic lights. Images are 32x32,
MIS is enabled, sample index is 4096, each temporal pass traces one fresh
sample, and the initial history cap is three samples.

Each frame compares all motion/history AOV records against GPU evidence
and checks colour against temporal radiance bit for bit after row flipping.
Every GPU shared-validity code/fetch index is compared with the CPU oracle.
The static sequence advances disjoint seeds through five frames, keeps
622 valid primary hits and reaches/exceeds the three-sample cap. Translating
the camera 0.12 in X preserves 609 valid pixels, produces motion for 615
pixels and rejects 13 current hits. Moving the panel 0.3 in X preserves
599 valid pixels, produces motion for 60 and rejects 23 current hits.
Both integrators produce these same geometric counts.

The sequence checks counter cuts, unchanged counters, compatibility-key
changes, cap and integrator changes, rejected no-clear colour bindings,
metadata-only progressive convergence, re-enabling radiance, render-pass
switches and colour-only temporal bindings. Failed binding validation leaves
the reset pending. A new render-pass token conservatively cuts the shared
delegate view. There is no cross-view history reuse.

## Installed usdview host

`lotus-renderer-usdview-host` exercises the checkable temporal setting in
the installed plugin, alongside its existing deterministic seed, integrator
and estimator regressions. Its new sequences use a four-sample history cap,
one sample per frame and seed zero under each integrator. They keep screenshots
for static accumulation, camera motion, rigid object motion, explicit cuts
and return to progressive rendering under `usdview-install/`.

Temporal shots observe five actual frame completions without waiting for
convergence. Qt event-loop iterations alone initially produced only two
frames, so the test now counts completed frames with a bounded deadline.
The extended host test has a 100-second child-process timeout and a
120-second CTest timeout. The targeted successful host run took 53.26 seconds.

The first camera-motion experiment reset with `SceneChanged`: usdview's
default light follows its camera, so moving the camera also changes lighting.
This is consistent with the measured reset policy. Reprojection sequences
disable that light and the default dome before temporal accumulation,
keeping the fallback environment fixed. Camera and triangle translations
then produce nonzero motion without global resets. A reset counter change
produces exactly one `CameraCut`, followed by compatible history. Host
evidence checks no synchronization messages, retained renderer/targets,
temporal batch progression and bounded history, and records screenshots.

## Renderer Phase 4 scope review

| Scope item | Disposition and evidence |
| --- | --- |
| Frame history and previous transforms | Implemented with immutable CPU frame pairs, rigid-placement correspondence and success-only publication ([foundation](2026-10-12-temporal-foundation.md), [GPU surface history](2026-10-12-temporal-primary-surfaces.md)) |
| Motion vectors and reprojection | Implemented under both integrators, with CPU projections and nearest-pixel GPU fetch; Hydra motion AOVs and moving USD/host sequences above ([surface evidence](2026-10-12-temporal-primary-surfaces.md), [fetch evidence](2026-10-12-temporal-gpu-validity.md)) |
| Disocclusion and shared validity | Implemented with stable identities, depth/normal/motion tests and per-pixel CPU-oracle comparisons; moving occluder/self-disocclusion checks ([validity evidence](2026-10-12-temporal-gpu-validity.md)) |
| Temporal accumulation | Implemented as bounded per-pixel radiance/count history; static progressive equivalence and independent moving fresh-batch/blend checks ([radiance evidence](2026-10-12-temporal-radiance.md)) |
| Reset policy | Implemented with frame/window/key/cut priorities and automatic scene/settings classification; cuts, lighting and both integrators measured ([radiance evidence](2026-10-12-temporal-radiance.md), Hydra/host checks above) |
| Deterministic progressive mode | Retained and selectable, including metadata-only debug products; existing integrator/deterministic and installed host regressions |

Nearest history fetch, approximate reuse under moving illumination, rigid
correspondence, fixed coverage metadata and explicit cuts for same-count
instance reordering remain accepted limits in the
[temporal contract](../reference/TEMPORAL.md). Alternating Hydra passes cut
the delegate's single retained history; per-view concurrent storage is not
claimed. No reservoir, denoiser or ReSTIR-specific state enters these passes.

## Regression commands

```sh
ost build --jobs auto
ost test
ost validate --strict-renderer-evidence
ost build --without-runtime --intent ci-core --jobs auto
ost test --without-runtime --intent ci-core
ost build --profile lookdev --intent hydra --jobs auto
ost test --profile lookdev --intent hydra
ost validate --profile lookdev --intent hydra --strict-renderer-evidence
python scripts/check_docs.py
python -m unittest discover -s scripts -p 'test_*.py'
```

Targeted temporal contract/GPU tests passed 2/2; the installed host test
passed with no synchronization validation messages. The final core suite
passed 23/23, and Hydra passed 48/48, including both new temporal tests and
the extended installed-host test (53.71 seconds). Runtime-free passed 13
checks with one explained temporal-GPU SKIP out of 14. Core and Hydra strict
renderer-evidence validation passed. Documentation checks and all 27 Python
documentation/release helper tests passed.

Logs use the `build/temporal05-` prefix: `core-build.log`, `core-test.log`,
`core-validate.log`, `runtime-free-build.log`, `runtime-free-test.log`,
`hydra-final-build.log`, `hydra-test.log`, `hydra-validate.log`,
`usdview-test.log`, `targeted-test.log` and `python-test.log`.
The existing CI test command discovers both new tests; unavailable GPU
capability returns CTest SKIP 77, while the synthetic contract creates no
Vulkan device and still requires the OpenUSD runtime linked by the adapter.
This report measures the local installed host, not a new GitHub-hosted run.

No release record, version bump or package is prepared by this implementation.
