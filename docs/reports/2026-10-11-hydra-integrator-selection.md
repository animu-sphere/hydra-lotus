# Integrator selection through Hydra

Measured on 2026-10-11 for LOTUS-WAVE-03, in the working tree based on
`9813d6d`, on the machine and toolchain of the
[camera and resolve report](2026-10-11-wavefront-camera-resolve.md):
Windows 11 x86_64, MSVC 14.51, OpenStrata 0.23.14, Release builds, an
NVIDIA RTX A5000 (driver 597.16) with Vulkan validation and synchronization
validation enabled, and OpenUSD 26.08 lookdev with CPython 3.13 for Hydra.
All runs were local; hosted CI was not measured.

## Scope

The wavefront integrator was reachable only from the headless runner and
the benchmark; the Hydra adapter always traced with the reference
integrator and did not ship the wavefront kernels. This change exposes
`PathTracingSettings::integrator` as a Hydra render setting and installs
the kernels with the plugin. Only the Hydra adapter, its tests and the
release product list changed; the core, the backend and the shaders did
not, so the committed reference images and the fixed benchmarks are
unaffected. It was the last Renderer Phase 2 task; the
[exit review](#renderer-phase-2-exit) follows.

## Design

| Decision | Choice |
| --- | --- |
| Setting | `lotus:wavefront`, beside `lotus:sampleIndex`: a `bool`, false by default. True selects the wavefront integrator, false the reference integrator. It is read on every pass, so a change restarts the accumulation through the backend's existing restart rule |
| Value type | a flag, because usdview lists flag settings directly in its Hydra Settings menu as checkable items, while other types appear only as fields of its *More…* dialog. A string setting (`reference` or `wavefront`) was implemented first and replaced before commit: switching meant opening the dialog and typing a name |
| Other values | converted by `VtValue::Cast`, as the other settings are read; a value that cannot be cast selects the reference integrator |
| Shaders | `wavefront.comp.spv` is copied beside the built plugin and installed under `lib/usd/hdLotus/shaders`; the adapter passes it as `RayQueryShaders::wavefront`. `scripts/release.py` now requires it there and under `bin/shaders` |
| Evidence | the host evidence log gains `integrator=reference|wavefront` and `wavefront_rounds`; `HdLotusRenderDelegate::GetFrameEvidence` returns the latest pass's `GpuFrameEvidence` without its colour and depth products, so a test can tell which integrator produced a sample |

The integrator is proved from the backend's evidence, not from the
setting: only wavefront frames report queue occupancy
(`wavefront_path_counts`). The reference and wavefront images are
identical on this device, so an image comparison alone could not tell a
wavefront frame from a reference frame that ignored the setting.

## Checks

**Hydra.** The new GPU-gated `lotus-renderer-hydra-integrator` test syncs
the stage of the
[deterministic-mode test](2026-10-05-hydra-deterministic-mode.md) (a floor,
a back wall and a tilted triangle under the white environment) through
UsdImaging and renders it through a Lotus render pass into a 32×32 float
colour AOV at 8 spp from sample index 4096. Each step renders until the
colour converges and requires convergence after exactly 8 passes. Each
pass must hold one more sample, and its sample must come from the selected
integrator: wavefront frames must report queue occupancy with one camera
path per pixel, and reference frames none. The backend's images come from
a separate `OffscreenRenderer` tracing the pass's snapshot in one 8-sample
call. Comparisons are bitwise.

| Step | Required |
| --- | --- |
| no `lotus:wavefront` | the descriptor is a `bool` defaulting to false; reference frames; the backend's reference image |
| set to true | restart; wavefront frames; the backend's wavefront image; channel means within 10⁻³ of the reference image's |
| set to false | restart; the first reference image |
| set to true again | restart; the first wavefront image |
| a new delegate created with true | the first wavefront image |

Measured: the two integrators' 8-spp images were identical bit for bit, as
the [equivalence report](2026-10-10-wavefront-equivalence.md) found for
the headless reference scene. The test prints the difference and requires
only the mean agreement, as other devices may differ in floating-point
evaluation.

**usdview.** The smoke test runs from the staged install tree, so it
exercises the installed kernel. After its sample-index phases it finds the
Hydra Settings menu's action for `lotus:wavefront`, which must be
checkable and unchecked, and triggers it as a click would. The evidence
must then report `integrator=wavefront`, a restart (a pass with fewer than
64 samples), a frame with wavefront rounds, and convergence at 64 samples.
Triggering it again must uncheck it and converge with no wavefront rounds
to a screenshot whose PNG bytes equal the `stable-update` screenshot's. In
the measured run 65 wavefront passes ran: the first recorded 65 rounds (a
first wavefront call records every bounce), the next 63 two rounds each,
and the converged pass none. The wavefront screenshot was also
byte-identical to the reference one, which the test does not require. The
parser of the evidence log now accepts the `integrator` field's text.

Three deliberate faults were each reverted before the runs below:

| Fault | `lotus-renderer-hydra-integrator` | usdview |
| --- | --- | --- |
| the pass ignores the setting | failed: "wavefront: pass 1 of 8: convergence is early" | failed: `integrator` was `reference` |
| the descriptor's default is the int 0 | failed: "lotus:wavefront is not a flag that defaults to false" | failed: "the Hydra Settings menu has no wavefront item" |
| `wavefront.comp.spv` not installed | passed (it reads the build tree) | failed: renderer creation failed on every pass and usdview timed out |

The last fault shows that a plugin installed without the kernel renders
nothing, under either integrator: `CreateOffscreenRenderer` fails when a
named shader file is missing. The release product check now rejects such
a package.

## Renderer Phase 2 exit

The [exit criteria](../design/ROADMAP_POLICY.md#renderer-phase-2--wavefront-path-tracing)
were reviewed against the Renderer Phase 2 evidence, and the phase was
accepted as complete on 2026-10-11.

| Criterion | Evidence |
| --- | --- |
| Reference-equivalent images | `renderer.path.wavefront` passes the transport scenarios and the committed reference by the DES-Q5 metric under the wavefront integrator, and matches the reference integrator's mean where a coverage draw precedes scattering ([equivalence](2026-10-10-wavefront-equivalence.md), [path state](2026-10-10-wavefront-path-state.md)). On the measured device both integrators' images, headless and through Hydra, are identical bit for bit |
| GPU scheduling that can be optimized independently | queue bookkeeping, round scheduling, path-state layout and the camera and resolve kernels each changed in isolation, with timings measured and images unchanged ([scheduling](2026-10-10-wavefront-scheduling.md), [path state](2026-10-10-wavefront-path-state.md), [camera and resolve](2026-10-11-wavefront-camera-resolve.md)) |
| A repeatable baseline measured before the transition | the [fixed performance baseline](2026-10-10-performance-baseline.md), which the benchmark's `--benchmark-integrator wavefront` repeats |

Dispositions of the phase's scope:

- The terminated-path queue was replaced by the resolve pass reading each
  ended path's sample from its slot
  ([camera and resolve](2026-10-11-wavefront-camera-resolve.md)).
- Path classification is assigned to Renderer Phase 8 (LOTUS-MATERIAL-01),
  where materials first differ in how they shade
  ([upper bound](2026-10-11-wavefront-camera-resolve.md#path-classification)).
- Shadow work belongs to Renderer Phase 3, as the policy states.
- The wavefront integrator is still slower than the reference integrator:
  1.43 times at 1024×1024 on `cornell-v1` and 2.5–2.8 times at 128×128.
  The exit criteria do not require it to be faster, so the reference
  integrator stays the default in the backend and in Hydra.

## Builds and tests

Before the builds, `ninja -t deps` in the Hydra tree listed objects
without recorded header dependencies, the known Japanese-MSVC issue
([supported configurations](../reference/SUPPORTED_CONFIGURATIONS.md#build-and-tooling-limitations));
they were deleted so that the header change rebuilt them.

| Configuration | Result |
| --- | --- |
| `ost build --profile lookdev --intent hydra`, `ost test --profile lookdev --intent hydra`, `ost validate --profile lookdev --intent hydra --strict-renderer-evidence` | built; 36 of 36 passed, including the new test and usdview; validation passed |
| `python scripts/test_release.py` | 12 of 12 passed |
| `python scripts/check_docs.py` | passed |

The core, ci-core and viewport configurations compile none of the changed
files and were not rerun.

The current behaviour and its limits are in the
[scene reference](../reference/SCENE.md#integrator-selection-through-hydra);
remaining work is in the [roadmap](../roadmap/current.md).
