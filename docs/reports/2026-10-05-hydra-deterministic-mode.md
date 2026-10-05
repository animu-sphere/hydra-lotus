# Hydra deterministic mode

Measured on 2026-10-05 in the working tree based on `4a391c6`, on Windows 11
x86_64 with MSVC 14.51, OpenStrata 0.23.14 and Release builds. The GPU was an
NVIDIA RTX A5000 (driver 597.16). Hydra used OpenUSD 26.08 lookdev and
CPython 3.13. All runs were local; hosted CI was not measured.

## Scope

This covers deterministic mode through Hydra, one of the items
[Renderer Phase 1](../roadmap/current.md#renderer-phase-1--reference-path-tracer)
needs alongside its vertical slice
([design policy §26](../design/DESIGN_POLICY.md#26-reference--deterministic-mode)):
a fixed RNG seed, spp, camera and frame index for a Hydra render. Only the
Hydra adapter changed; the core, the backend and the shaders did not, so the
committed reference images are unaffected.

The checks are bitwise comparisons of converged images on one device and
build. Across devices and drivers GPU float results may differ; the
statistical comparison of the [reference images](../reference/SCENE.md#reference-images)
covers that case.

## Design

The backend's deterministic mode already existed: an image is a function of
its first sample index, its sample count, the scene, the camera and the
target. Hydra lacked only a way to set the first sample index, which it
always left at 0. The four inputs of design policy §26 map as follows:

| Input | Through Hydra |
| --- | --- |
| RNG seed | the new `lotus:sampleIndex` render setting (int, 0 by default): `PathTracingSettings::sample_index` |
| spp | the existing `convergedSamplesPerPixel` render setting: `max_samples` |
| camera | the host's camera and framing in the render pass state |
| frame index | none: Hydra's pass count does not feed the random numbers. The k-th sample of an accumulation uses `lotus:sampleIndex + k` |

The setting is named for what it is, the first sample index, rather than
`seed`: indices closer than the sample count share samples, which a seed
would not suggest. The render pass reads both settings on every pass, so a
change restarts the accumulation through the backend's existing restart
rule. Negative values count as 0. The host evidence log gained a
`sample_index` field.

## Fixtures

**Hydra.** The new `lotus-renderer-hydra-deterministic` test composes a USD
stage in memory: a floor, a back wall and a tilted triangle, all the default
grey under the adapter's white environment, so the surfaces light each other
and every sample index gives different noise. A USD camera views them. The
test syncs the stage through UsdImaging and renders through a Lotus render
pass, with the camera's sprim and a 32×32 framing, into a 32×32 float colour
AOV at 8 spp. Each step renders until the colour converges and requires
convergence after exactly 8 passes. The reference for a comparison is the
backend's own render: a separate `OffscreenRenderer` traces the pass's
selected snapshot from the given sample index in one 8-sample call, into the
target the pass configured. Comparisons are bitwise.

| Step | Required |
| --- | --- |
| `lotus:sampleIndex` 4096 | the backend's image from 4096, and not the image from 4097; one more pass changes nothing |
| setting changed to 8192 | restart and 8 more passes; the backend's image from 8192, different from 4096's |
| back to 4096; 3 passes, camera moved, 2 passes, camera restored | after 8 more passes, the first 4096 image |
| a new render delegate, with a new Vulkan renderer, at 4096 | the first 4096 image |
| a delegate without the setting | the backend's image from 0 |

It is labelled `gpu` and returns CTest's skip code without a Vulkan device
or ray queries.

**usdview.** The smoke test gained two phases after its point edit. Setting
`lotus:sampleIndex` to 4096 through `StageView.SetRendererSetting` must
restart the accumulation (some pass reports fewer than 64 samples) and
converge at 64 with `sample_index=4096` in the evidence. Setting it back
to 0 must converge to a viewport image whose PNG bytes equal the earlier
`stable-update` image's. In the measured run the 4096 image also differed
from it, although the test does not require that: the 8-bit viewport shot
can hide the difference.

Three deliberate faults were each reverted before the runs below:

- tracing every pass from sample index 0 while reporting the setting failed
  `lotus-renderer-hydra-deterministic` ("Hydra's image differs from the
  backend's for sample index A") and the usdview test ("changing
  lotus:sampleIndex did not restart the accumulation");
- reading the setting only when the renderer is created failed both
  ("sample index B: convergence after pass 1 of 8 is early", and the same
  usdview assertion);
- offsetting the index by a per-delegate session counter failed
  `lotus-renderer-hydra-deterministic` ("a new render delegate's image
  differs from sample index A's"). The usdview test, with one delegate,
  passed.

## Verification

Before the builds, `ninja -t deps` in the Hydra tree listed no object
without recorded header dependencies, the known Japanese-MSVC issue
([roadmap](../roadmap/current.md#project-infrastructure)).

- `ost build --profile lookdev --intent hydra --jobs auto`,
  `ost test --profile lookdev --intent hydra`: **18/18 passed**, including
  the new test and the usdview host test.
- `ost validate --profile lookdev --intent hydra --strict-renderer-evidence`:
  **passed**; the usdview host evidence reports zero Vulkan validation
  messages.

The core, ci-core and viewport configurations compile none of the changed
files and were not rerun.

The current behaviour and its limits are in the
[scene reference](../reference/SCENE.md#deterministic-mode-through-hydra);
remaining work is in the
[roadmap](../roadmap/current.md#renderer-phase-1--reference-path-tracer).
