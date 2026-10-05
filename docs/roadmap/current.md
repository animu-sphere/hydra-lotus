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
CPU coarse-mesh extraction, ordinary mesh placement and Hydra instancer
placements ([instancer evidence](../reports/2026-10-05-hydra-instancers.md)),
the incremental
upload of geometry and instance buffers, and a BLAS per geometry with a TLAS
over the instances are implemented
([scene reference](../reference/SCENE.md),
[extraction evidence](../reports/2026-10-05-cpu-mesh-extraction.md),
[upload evidence](../reports/2026-10-05-gpu-scene-upload.md),
[BLAS / TLAS evidence](../reports/2026-10-05-blas-tlas.md)). Primary camera
rays now traverse the GPU scene and write diagnostic barycentrics and depth,
including through Hydra on ray-query devices
([ray-query evidence](../reports/2026-10-05-primary-rays.md)). Each sample
then follows one brute-force path with Lambert and GGX metal surfaces,
emission, a constant environment and Russian roulette
([BSDF and multi-bounce evidence](../reports/2026-10-05-bsdf-multibounce.md)),
through a box-filtered point of its pixel, into an unclamped RGBA32F
accumulation that Hydra receives as a float colour AOV and converges
progressively
([HDR accumulation evidence](../reports/2026-10-05-hdr-accumulation.md)) —
the *first physically correct image*. A fixed Cornell box, rendered by the
headless runner at 1, 16, 64, 256 and 1024 spp, statistically matches a
committed deterministic reference
([reference images evidence](../reports/2026-10-05-reference-images.md)) —
the *reference path tracer* milestone, which meets the phase's exit
criteria.

Needed alongside the slice
([design policy §51](../design/DESIGN_POLICY.md#51-decision-principles),
principle 8):

- ⬜ **Render-pass selection.** Respect the render pass's collection and
  render tags; every synced mesh is traced today
  ([scene reference](../reference/SCENE.md#hydra-extraction)).
- ⬜ **Deterministic mode through Hydra**: a fixed RNG seed, spp, camera and
  frame index for a Hydra render. The headless runner's reference renders
  are deterministic already
  ([scene reference](../reference/SCENE.md#reference-images)). Hydra
  always starts its accumulation at sample index 0 and has no setting for
  it.
- ⬜ **Extend per-pass GPU timestamps** beyond the scene pass to
  scene upload and BLAS/TLAS builds, reported by the headless runner
  ([design policy §24](../design/DESIGN_POLICY.md#24-gpu-profiling)).

## Backend follow-up

- ⬜ **GPU memory suballocation.** Each geometry buffer and each BLAS is
  its own device allocation, so scenes are limited by the device's
  allocation count ([scene reference](../reference/SCENE.md#gpu-scene)).
- ⬜ **BLAS refit and compaction.** A point edit that keeps the topology
  uploads a new buffer and builds a new BLAS instead of refitting the old
  one, and BLASes are not compacted
  ([design policy §20](../design/DESIGN_POLICY.md#20-acceleration-structure),
  [scene reference](../reference/SCENE.md#acceleration-structures)).

## Adapter follow-up

- ⬜ **Hydra lights and materials.** The adapter reads no light or
  material: every mesh has the default `SurfaceMaterial` under a constant
  white fallback environment
  ([scene reference](../reference/SCENE.md#hydra-extraction)). A dome
  light's colour and intensity can feed the constant environment;
  `UsdPreviewSurface` translation is Renderer Phase 1.5.
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
