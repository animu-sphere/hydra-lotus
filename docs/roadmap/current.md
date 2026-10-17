# Current

The only active task checklist. Phase and release status belongs in the
[canonical status table](README.md#status-at-a-glance); completed work is
represented by its [evidence](../reports/README.md) and
[change history](../../CHANGELOG.md), rather than retained here.

## Execution order

The [v0.6.0 plan](README.md#v060--interactive-rendering) sets the order
below; scope and exit criteria are the
[roadmap policy's](../design/ROADMAP_POLICY.md#4-roadmap). The experimental
ReSTIR snapshot is recorded in [the release record](../releases/v0.5.0.md) and
[release gate](../reports/2026-10-16-v0.5.0-release-gate.md), and the
[reservoir reference](../reference/RESTIR.md) owns current behavior and
limits. Step 1 comes first and is kept short.

### Step 1 — ReSTIR DI closure

The remaining Renderer Phase 5 work is limited to the exit review below;
no other ReSTIR DI fixture is added
([stop rule](../design/ROADMAP_POLICY.md#renderer-phase-5--restir-di)).
The starting evidence, including its adverse results and limits:
[quality budgets](../reports/2026-10-15-restir-budgets.md),
[boundaries](../reports/2026-10-15-restir-boundaries.md),
[residual correlations](../reports/2026-10-16-restir-correlation.md),
[complementary BSDF comparisons](../reports/2026-10-16-restir-bsdf-partition.md),
the [rough dielectric/sphere-light fixture](../reports/2026-10-16-restir-glossy-spheres.md),
[four-bounce/reference sensitivity](../reports/2026-10-16-restir-convergence.md),
the [sequence-ensemble decomposition](../reports/2026-10-16-restir-sequence-ensemble.md)
(the static temporal penalty is correlation, not detectable bias),
[replication bias tests](../reports/2026-10-17-restir-moving-bias.md)
(no moving-scene bias, ghosting or localized error in four moving suites;
narrow glossy highlights limited by rare-event variance), a
[closed interreflecting room](../reports/2026-10-17-restir-interreflection.md),
[light/material animation](../reports/2026-10-17-restir-animation.md) and the
[emissive-state and Hydra reset contract](../reports/2026-10-17-restir-reset-contract.md),
which together complete the animation-and-reset criterion. Fresh batches
replaying random numbers after each reset is
[DES-Q7](../design/DESIGN_POLICY.md#53-open-questions), decided with the
denoiser. The [reuse-cost decision](../reports/2026-10-17-restir-reuse-cost.md),
timed at locked clocks, completes the cost criterion: NEE/MIS stays the
default, and only initial-only reservoirs with eight candidates pay off, for
many distant lights (about 1.4 times faster to equal quality); temporal,
spatial and combined reuse are not worth using.

- [ ] **LOTUS-RESTIR-06 — Renderer Phase 5 exit review.** A report that
  records, per representative workload, whether there is an equal-quality
  speedup, plus the known bad cases, correlation cost and unsupported
  conditions; states the default renderer path and when to choose ReSTIR;
  and updates the [reservoir reference](../reference/RESTIR.md) and the
  [canonical status](README.md#status-at-a-glance). The experimental release
  gate validated the shipped snapshot; it is not this review.

### Step 2 — Viewport foundation

`lotus-viewport` becomes the
[renderer frontend](../design/ROADMAP_POLICY.md#standalone-viewport-frontend)
for everyday use; it does not become a UI framework.

- [ ] **LOTUS-VIEWPORT-01 — Presentation loop and scene loading.**
  A stable swapchain lifecycle, resize, basic fullscreen/windowed operation,
  a predictable present loop and validation-clean presentation, replacing
  the bootstrap triangle with the renderer's progressive output. Open a USD
  stage the way [`hydra-merlin`'s viewport](https://github.com/animu-sphere/hydra-merlin/blob/main/docs/guides/viewport.md#opening-usd-stages)
  does ([DES-Q8](../design/DESIGN_POLICY.md#53-open-questions),
  [layout edge](../architecture/PROJECT_LAYOUT.md#4-dependency-directions)):
  with `LOTUS_ENABLE_HYDRA2` the viewport links `lotus-hydra2-runtime` and
  a host-dependency interface target exported by `adapters/hydra2/`, creates
  the render delegate directly and drives it with `HdRenderIndex`,
  `UsdImagingDelegate`, `HdEngine` and its own task; normal frames present
  from the backend without a CPU readback. The default viewport build stays
  OpenUSD-free. Add an OpenStrata intent for the Hydra-enabled viewport, a
  generated Windows launcher that puts the OpenUSD runtime on `PATH` for the
  process only, and scripted CTests (`--hidden`, `--frames`, validation,
  a resize test, an explained SKIP without a device, `--usd <stage>` and a
  benchmark JSON), with and without a stage.
- [ ] **LOTUS-VIEWPORT-02 — Camera and accumulation.** Orbit, pan and dolly;
  authored camera selection; accumulation restarted on camera change; scene
  dirty updates applied.
- [ ] **LOTUS-VIEWPORT-03 — Run-time renderer controls and debug views.**
  Switch the reference/wavefront integrator, spp and accumulation, NEE/MIS,
  ReSTIR off/initial/temporal/spatial/combined and temporal radiance reuse
  while running. Show beauty, depth, normal, motion vector, temporal validity
  and reservoir validity/age/weight from the existing AOVs and debug products,
  without new viewport-only renderer state. Reuse frames currently read back
  and check reservoir and temporal products every frame, 28–36 ms of wall
  time at 128x128 against 1.3–2.0 ms for NEE/MIS
  ([measurement](../reports/2026-10-17-restir-reuse-cost.md#wall-time-and-memory)),
  so interactive reuse needs a path that skips them. The controls are a Dear
  ImGui developer UI, as in `hydra-merlin`'s viewport, with the settings logic
  in sources that a GPU-free unit test covers.
- [ ] **LOTUS-VIEWPORT-04 — Timing HUD.** Frame time; GPU path-trace, ReSTIR,
  temporal and (once it exists) denoise pass times; sample count; scene/TLAS
  update time; reservoir and history memory.

### Step 3 — SVGF core

The [SVGF-class baseline](../design/ROADMAP_POLICY.md#renderer-phase-6--denoising--svgf-class-pipeline)
is built and compared with references headless before it reaches the viewport.

- [ ] **LOTUS-SVGF-01 — Temporal accumulation, moments and variance.**
  Reprojection and history validation through the existing temporal
  infrastructure, temporal accumulation, first/second moments and variance
  estimation, with timestamps and debug products (raw radiance, reprojected
  history, validity, history length, moments, variance, rejection reason).
  The input is noisy radiance and shared surface metadata, with no ReSTIR
  coupling. Decide [DES-Q7](../design/DESIGN_POLICY.md#53-open-questions).
- [ ] **LOTUS-SVGF-02 — Edge-aware filtering and reference comparison.**
  À-trous filtering guided by depth, normals, motion, stable identity and
  luminance variance, and the final composite. Compare raw low spp, temporal
  accumulation alone and the filtered output with a high-spp reference on
  RMS/MSE, temporal residual, disocclusion response, static convergence and
  moving-camera stability, under both integrators, with deterministic
  validation.

### Step 4 — SVGF in the viewport

- [ ] **LOTUS-SVGF-03 — Viewport integration.** Denoiser toggle at run time,
  raw/filtered comparison, history and variance views, and live denoise
  timing.

### Step 5 — Release gate

- [ ] **LOTUS-RELEASE-02 — Interactive rendering release gate.** Headless
  correctness, an interactive viewport smoke test, the installed Hydra
  regression, Renderer Phase 5 closure evidence, deterministic denoiser
  validation, GPU timings, clean validation and synchronization, and the
  package/release gate ([releasing](../guides/RELEASING.md)).

What v0.6.0 leaves for later is listed with its
[plan](README.md#v060--interactive-rendering); those items keep their phase
assignments.

## Performance baseline and foundation release

The [fixed baseline](../reports/2026-10-10-performance-baseline.md),
[foundation dispositions](../reports/2026-10-10-foundation-closure.md) and
[foundation release gate](../reports/2026-10-10-release-gate.md) remain the
reference for later transport changes. Shipped scope is retained in the
[release records](../releases/README.md).

## Deferred adapter work

These are assignments to later phases, rather than additional foundation
requirements:

- [ ] **LOTUS-MATERIAL-01 — Renderer Phase 8 production scene coverage.**
  Evaluate remaining material, texture and shading-normal coverage within
  the [production material scope](../design/ROADMAP_POLICY.md#renderer-phase-8--production-material-support),
  using the [material reference](../reference/SCENE.md#materials-and-environment)
  and [Hydra extraction reference](../reference/SCENE.md#hydra-extraction).
  Once materials differ in how they shade, measure wavefront path
  classification (hits queued per shading class) against its
  [upper bound on the fixed workloads](../reports/2026-10-11-wavefront-camera-resolve.md#path-classification),
  1–2% of the shade kernel where every material is of one kind.
- [ ] **LOTUS-LIGHT-05 — Renderer Phase 9 light acceleration.** Every ray
  tests every analytic light, and a frame's cost grows linearly with their
  number ([baseline](../reports/2026-10-12-renderer-phase3-exit.md#analytic-lights-along-each-ray)).
  Evaluate a hierarchy for the crossing test, or the lights as procedural
  primitives in the ray-tracing acceleration structure, with a light tree
  for selection, within the
  [advanced sampling candidates](../design/ROADMAP_POLICY.md#renderer-phase-9--advanced-sampling-and-scheduling).
