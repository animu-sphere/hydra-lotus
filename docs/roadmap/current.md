# Current

Renderer Phase 0, the Renderer Phase 1 vertical slice, and the work around
them. Which release carries a phase is the
[status table](README.md#status-at-a-glance).

Legend: ✅ done · 🚧 in progress · ⬜ not started · ⛔ blocked · ⚠️ accepted workaround

## Renderer Phase 0 — Foundation

The OpenStrata renderer scaffold was generated on 2026-10-04 and passes its
own contract on Windows
([report](../reports/ost/01-2026-10-04-v0.23.14-renderer-template-bootstrap.md)).
Against the scope of
[roadmap policy §4](../design/ROADMAP_POLICY.md#renderer-phase-0--foundation):

- ✅ **Renderer-core separation, render world and render extraction.** The
  host-neutral `core/` targets, with a header boundary check.
- ✅ **Vulkan backend bootstrap and Vulkan validation.** Device bring-up with
  validation layers, messages treated as errors.
- ✅ **Slang → SPIR-V.** `slangc` compiles the shaders at build time.
- ✅ **Hydra plugin discovery and render delegate creation.** `hdLotus` is
  discovered, creates its delegate, and draws a first frame in
  `testusdview`.
- ✅ **Image output and camera.** The Hydra path renders offscreen at the
  AOV's resolution, through the Hydra camera and the framing's display and
  data windows, and copies the products into CPU `HdRenderBuffer`s; the
  headless runner checks a perspective camera.
- ✅ **A persistent offscreen renderer.** `Lotus::OffscreenRenderer` keeps
  the Vulkan instance, device and pipeline across frames and recreates its
  targets only when the AOV size changes
  ([design policy §23](../design/DESIGN_POLICY.md#23-cpu-performance)). The
  usdview smoke test checks that one renderer serves every Hydra frame and
  that the targets are created once per AOV size; the headless runner checks
  reuse and a resize.
- ✅ **Standalone viewport and headless runner.** Both draw the scaffold's
  bootstrap triangle.
- 🚧 **CI boundary tests.** The registered renderer workflow consumes
  `openstrata.ci.yaml`: runtime-free `ci-core` with boundary/install checks
  and explained GPU `SKIP`s, and `hydra` against a digest-pinned runtime.
  Local checks pass; the first GitHub-hosted run remains unmeasured. OpenStrata
  0.23.14's generator requires plugin workspace descriptors and its `validate`
  cannot select runtime-free targets; generator adoption awaits those upstream
  fixes ([report](../reports/2026-10-04-foundation-ci.md)).

## Renderer Phase 1 — Reference path tracer

The near-term priority is one complete vertical path
([roadmap policy §9](../design/ROADMAP_POLICY.md#9-near-term-priority)); the
phase's full scope and exit criteria are
[§4](../design/ROADMAP_POLICY.md#renderer-phase-1--reference-path-tracer).
CPU coarse-mesh extraction, ordinary mesh placement and the incremental
upload of geometry and instance buffers are implemented
([scene reference](../reference/SCENE.md),
[extraction evidence](../reports/2026-10-05-cpu-mesh-extraction.md),
[upload evidence](../reports/2026-10-05-gpu-scene-upload.md)). No pass reads
the GPU scene yet; the GPU still draws the fixed bootstrap triangle.
Remaining work, in order:

- ⬜ **BLAS and TLAS.** The geometry buffers gain device addresses and
  acceleration-structure build input usage.
- ⬜ **Camera ray → ray query → triangle hit.** The *first ray-traced
  triangle* milestone.
- ⬜ **BSDF and multi-bounce.** Lambert, a minimal GGX, emissive surfaces,
  environment lighting, Russian roulette, surface hit reconstruction.
- ⬜ **HDR accumulation → AOV / output.** The *first physically correct
  image* milestone.
- ⬜ **Reference images.** A fixed deterministic test scene rendered by the
  headless runner at 1, 16, 64, 256 and 1024 spp, compared in `validation/`
  ([design policy §26](../design/DESIGN_POLICY.md#26-reference--deterministic-mode)).
  The *reference path tracer* milestone.

Needed alongside the slice
([design policy §51](../design/DESIGN_POLICY.md#51-decision-principles),
principle 8):

- ⬜ **Hydra instancers and render-pass selection.** Expand instance data
  and respect collection/render-tag selection; current CPU meshes have one
  ordinary placement each.
- ⬜ **Ray query capability probe.** Report `VK_KHR_acceleration_structure`
  and `VK_KHR_ray_query` in `renderer-report.json` and `SKIP` the
  path-tracing checks with an explanation where they are missing.
- ⬜ **Deterministic mode**: fixed RNG seed, spp, camera and frame index.
- ⬜ **Per-pass GPU timestamps**, reported by the headless runner
  ([design policy §24](../design/DESIGN_POLICY.md#24-gpu-profiling)).
- ⬜ **Settle [DES-Q3](../design/DESIGN_POLICY.md#53-open-questions)**: what
  keeps `PathState` open to spectra.
- ⬜ **Settle [DES-Q5](../design/DESIGN_POLICY.md#53-open-questions)**: how
  "statistically matches the reference" is measured, before Renderer Phase 2
  compares against it.

## Backend follow-up

- ⬜ **GPU memory suballocation.** Each geometry buffer is its own device
  allocation, so scenes are limited by the device's allocation count
  ([scene reference](../reference/SCENE.md#gpu-scene)).

## Adapter follow-up

- ⬜ **No-clear restoration when switching AOV buffer sets.** The foundation
  renderer retains one colour/depth attachment pair at a time; preservation
  currently assumes successive passes reuse their bound buffers
  ([AOV reference](../reference/AOVS.md#clears-and-successive-frames)).

## Testing infrastructure

From the [roadmap policy's testing strategy](../design/ROADMAP_POLICY.md#7-testing-strategy),
what is not covered by a phase above:

- ⬜ **Synchronization validation.** The headless runner and the usdview
  smoke test enable the validation layers' synchronization validation, with
  its messages treated as errors like the others.
- ⬜ **Performance records.** GPU frame time, samples/s, rays/s, VRAM, BLAS /
  TLAS build and scene upload time, recorded as reports once there is a path
  tracer to measure.

## Project infrastructure

- ⬜ **Documentation check.** A `scripts/check_docs.py` that resolves
  relative links and checks category indexes.
- ⚠️ **Objects without header dependencies on a Japanese MSVC host.** In the
  `hydra` and viewport trees, objects record `#deps 0` in Ninja's log, so a
  header edit does not rebuild them. Until OpenStrata or the template
  resolves it, list them with `ninja -t deps` in the build tree and delete
  every `.obj` with `#deps 0` before building
  ([report](../reports/ost/01-2026-10-04-v0.23.14-renderer-template-bootstrap.md) §3).
