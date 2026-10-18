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
progressive path tracer from the backend, opens USD stages through the
Hydra adapter ([evidence](../reports/2026-10-17-viewport-presentation.md)),
and navigates them with usdview's free camera or an authored camera, at a
stage time code, restarting the accumulation when the camera or scene changes
([evidence](../reports/2026-10-17-viewport-camera.md),
[usage](../guides/BUILDING.md#the-standalone-viewport)). A Dear ImGui
developer UI switches the integrator, accumulation, NEE/MIS, ReSTIR and
temporal radiance reuse while running, and presents the depth, normal,
motion, temporal-validity and reservoir debug products from the GPU; reuse
frames that are presented no longer read back their evidence products
([evidence](../reports/2026-10-18-viewport-controls.md)). A timing HUD shows
the frame time, each GPU pass's time (path trace and wavefront kernels,
ReSTIR, temporal), the sample count, the latest scene update's upload, BLAS
and TLAS times and the reuse and scene memory, and the Hydra-enabled build
opens stages from a native file dialog
([evidence](../reports/2026-10-18-viewport-timing.md)). The step is complete
and shipped in [v0.5.2](../releases/v0.5.2.md); denoise timing joins the HUD
with LOTUS-SVGF-03.

### Step 3 — SVGF core

The [SVGF-class baseline](../design/ROADMAP_POLICY.md#renderer-phase-6--denoising--svgf-class-pipeline)
is built and compared with references headless before it reaches the viewport.
Its regression tests stay small and its quality study is a research-tier
campaign and a report, by the [test policy](../design/TEST_POLICY.md#8-svgf).

The temporal radiance accumulation keeps its batches' luminance moments,
and a variance pass estimates each pixel's variance, temporally from four
batches of history and with SVGF's spatial estimate before; raw radiance,
reprojected history, validity, history length, moments, variance and
rejection reasons are products, with their GPU times, from noisy radiance and
shared surface metadata alone
([evidence](../reports/2026-10-18-svgf-moments-variance.md),
[contract](../reference/TEMPORAL.md#luminance-moments-and-variance)).
[DES-Q7](../design/DESIGN_POLICY.md#53-open-questions) is resolved: every
reset keeps restarting the RNG.

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
