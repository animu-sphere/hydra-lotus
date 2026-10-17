# Current

The only active task checklist. Phase and release status belongs in the
[canonical status table](README.md#status-at-a-glance); completed work is
represented by its [evidence](../reports/README.md) and
[change history](../../CHANGELOG.md), rather than retained here.

## Execution order

The [v0.6.0 plan](README.md#v060--interactive-rendering) sets the order
below; scope and exit criteria are the
[roadmap policy's](../design/ROADMAP_POLICY.md#4-roadmap).

### Step 1 — ReSTIR DI closure

Closed by the [exit review](../reports/2026-10-17-renderer-phase5-exit.md) and
shipped ahead of the other steps ([release record](../releases/v0.5.1.md)).
NEE/MIS is the default renderer path; initial-only reservoirs with eight
candidates are worth using for many lights of similar importance, and the
[reservoir reference](../reference/RESTIR.md#when-to-use-reservoirs) owns that
guidance. The [stop rule](../design/ROADMAP_POLICY.md#renderer-phase-5--restir-di)
applies: ReSTIR DI gains no new fixtures, and later findings follow the
review's [routing](../reports/2026-10-17-renderer-phase5-exit.md#after-the-exit).

### Step 2 — Viewport foundation

`lotus-viewport` becomes the
[renderer frontend](../design/ROADMAP_POLICY.md#standalone-viewport-frontend)
for everyday use; it does not become a UI framework. It already presents the
progressive path tracer from the backend and opens USD stages through the
Hydra adapter ([evidence](../reports/2026-10-17-viewport-presentation.md),
[usage](../guides/BUILDING.md#the-standalone-viewport)).

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
