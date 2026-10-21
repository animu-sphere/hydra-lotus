---
status: accepted
owner: hydra-lotus
---

# hydra-lotus — roadmap policy

> Status: **accepted** as the project's roadmap policy, 2026-10-04. Nothing
> below is implemented beyond what
> [reference/CAPABILITY_MATRIX.md](../reference/CAPABILITY_MATRIX.md) says.
>
> This document owns **the Renderer Phase 0–10 definitions**: each phase's goal,
> scope and exit criteria, the milestones and the testing strategy.
> Execution order and incomplete work belong in [current.md](../roadmap/current.md).
> This policy is distilled from the 2026-10-04 roadmap policy and
> keeps its section numbers. The [design policy](DESIGN_POLICY.md) owns what
> the renderer is and how it is shaped; its §36–§49 defer here. Which release
> carries a phase is the [roadmap](../roadmap/README.md#status-at-a-glance).
> Where this document departs from the source policy, §11 records it; open
> questions are kept with the design policy's, in
> [DESIGN_POLICY.md §53](DESIGN_POLICY.md#53-open-questions).

---

## 1. Purpose

`hydra-lotus` is a **practical, OpenUSD-native GPU path tracer** with an
independent rendering research track. The [development strategy](POST_V060_STRATEGY.md)
owns priorities and research stop rules. Every delivery stands on a
reference implementation whose correctness can be checked.

What matters is not stacking advanced algorithms from the start but **always
keeping a reference path tracer to compare against**.

## 2. Core principles

### 2.1 Correctness before performance

A deterministic, checkable reference renderer exists before anything is made
faster. Every accelerated mode can be compared with the reference path tracer.

```text
Reference path tracer → correctness baseline → wavefront / MIS / ReSTIR / denoising → reference comparison
```

### 2.2 Hydra is an adapter, not the renderer core

Hydra's types and lifecycle do not enter the renderer core
([design policy §3.1](DESIGN_POLICY.md#31-hydra-is-a-scene-integration-layer)).

```text
OpenUSD / Hydra → hdLotus adapter → LotusScene → render extraction → GpuScene → integrator
```

The headless runner, the viewport and Hydra use the same renderer core. (The
source policy draws render extraction before `LotusScene`; §11 says why it is
after it here.)

### 2.3 Explicit backend boundaries

The core and the GPU backend are separate:

- the core's public API exposes no Vulkan type;
- the renderer core exposes no Hydra or OpenUSD type;
- the renderer core exposes no windowing or UI type;
- GPU traversal sits behind an abstract boundary (`TraversalBackend`,
  [design policy §6](DESIGN_POLICY.md#6-ray-tracing-backend)).

This leaves room to change the Vulkan implementation, to compare an RT
pipeline, and to research WebGPU, Metal or a CPU reference backend (§6). The
binding form of these rules is
[PROJECT_LAYOUT.md §4](../architecture/PROJECT_LAYOUT.md#4-dependency-directions).

### 2.4 Compute + ray query first

The main implementation path is **compute shaders with ray query**. The RT
pipeline is not rejected; it becomes another `TraversalBackend`
implementation, to compare against.

### 2.5 Research features stay measurable

An advanced feature is not done when it merely works. It aims to be
measurable in:

- correctness / image difference;
- GPU time;
- rays per second;
- samples per second;
- queue occupancy;
- memory consumption;
- convergence rate;
- temporal stability.

## 3. Target architecture

```text
OpenUSD stage
    │
    ▼
Hydra / HdSceneIndex
    │
    ▼
hdLotus adapter
    │
    ▼
LotusScene
    │
    ▼
render extraction
    │
    ▼
GpuScene
    ├──────────────────┐
    ▼                  ▼
traversal backend   material system
    └────────┬─────────┘
             ▼
     wavefront engine
    ┌────────┼────────────┐
    ▼        ▼            ▼
 NEE / MIS  ReSTIR   path sampling
    └────────┼────────────┘
             ▼
       accumulation
             │
             ▼
    temporal / denoise
             │
             ▼
           AOVs
    ┌────────┴────────┐
    ▼                 ▼
  Hydra           viewport
```

Where each box lives is
[PROJECT_LAYOUT.md §3](../architecture/PROJECT_LAYOUT.md#3-where-new-code-goes).

## 4. Roadmap

A phase is always cited as "Renderer Phase *n*"
([documentation guidelines](../contributing/documentation.md#naming)). A
phase is not a release.

### Cross-phase delivery

Phase numbers identify capability scopes and their exit criteria, rather
than imposing a total execution order after the reference, temporal,
denoising and viewport foundations. Selected Renderer Phase 8 or 9 work may
precede Renderer Phase 7 when its own dependencies exist and measurements
justify it. Advanced reservoir transport is not a prerequisite for
production-material or GPU-efficiency improvements.

Record each selected subset as a stable task ID in
[current.md](../roadmap/current.md), with its parent phase, bounded scope,
acceptance evidence and deferred remainder. Assign a release only in the
[canonical table](../roadmap/README.md#status-at-a-glance). A release may
span several phases or improve an existing path without completing a phase.
Unstarted parent phases retain their status until implementation begins;
partial delivery must be labeled as a subset, never as a phase exit.
Phase completion still requires an exit review against the full agreed scope.

### Practical foundation workstreams

These contracts bound selected cross-phase delivery. The roadmap chooses
which milestone carries them; the strategy owns why they come first.

**A — GPU observation and selected optimization (P0).** Establish a shared
headless/viewport workload manifest covering Cornell box, many instances,
many analytic lights, texture-heavy scenes and camera/object motion. Record
GPU, driver, build, resolution, spp, seed, warmup, run count, clock conditions
and renderer mode. Reuse timestamps, HUD and headless benchmarks for raygen,
ray query, shading, wavefront queues, accumulation, light sampling, temporal,
SVGF and scene updates. State any inseparable or unavailable timing rather
than implying a breakdown that was not measured; use GPU captures as needed.
Report mean and percentile GPU time, time to equal quality, image error,
peak VRAM and CPU update time.

**Exit evidence:** a reproducible baseline and environment, pass-cost
breakdown, explanation of the top three bottlenecks, adoption/rejection
decisions, before/after measurements for adopted optimizations and no
reference-quality regression. No predetermined speedup factor is required.

**B — VRAM and temporal buffers (P1).** Inventory temporal/SVGF buffers,
lifetimes, owners and peak residency, including host mirrors separately.
Compare representation and reuse candidates under the strategy's
[adoption rules](POST_V060_STRATEGY.md#4-measurement-and-adoption).

**Exit evidence:** 1080p/4K estimates and measured peak residency where the
available hardware permits it; explicitly report unavailable measurements.
Preserve history/rejection meaning, CPU-oracle agreement and dynamic-scene
stability. Record before/after memory, bandwidth/time and precision effects
for any adopted change.

**C — Material foundation (P1, Renderer Phase 8 subset).** Audit and fix
representative assets in this order: base color, metallic/roughness,
textures/UV, shading normals/tangents/normal maps, emission, alpha masks.
Build on the [existing material contract](../reference/SCENE.md#materials-and-environment);
this is not a claim that those features are absent. Translation remains
UsdPreviewSurface/OpenPBR/MaterialX → Lotus Material IR. Alpha blending
requires an explicit transport, sorting and stochastic-transparency model
and a bounded support statement; coverage opacity is not full transparency.

**Exit evidence:** representative USD fixtures and reference images,
Hydra/headless consistency, documented unsupported parameters, profiling
and regression coverage for new shader paths. Full OpenPBR, arbitrary
MaterialX graphs and subsurface scattering are outside this subset.

**D — Light acceleration investigation (P1, Renderer Phase 9 subset).**
Profile analytic-light intersection separately from NEE/MIS light selection
on fixed many-light workloads. Compare a hierarchy/BVH or suitable
procedural primitives for intersection and a light tree/importance
distribution for selection. Only implement a minimal experiment if the
measured cost is a leading bottleneck.

**Exit evidence:** separate cost attribution and an adopt/defer decision
covering GPU time, equal-quality behavior, memory and low-light-count
regressions. An evidence-based decision to defer implementation satisfies
this investigation; it does not complete the parent phase.

### Renderer Phase 0 — Foundation

**Goal.** Before the renderer proper, stabilize the dependency boundaries,
the build, Hydra integration, Vulkan initialization, the shader pipeline and
headless validation.

**Scope.** Renderer-core separation; render world and render extraction;
Vulkan backend bootstrap; Slang → SPIR-V; Hydra plugin discovery; render
delegate creation; render buffer and AOV bootstrap; standalone viewport;
headless runner; Vulkan validation; CI boundary tests.

**Exit criteria.** The Hydra, headless and viewport entry points all start
the same renderer core.

### Renderer Phase 1 — Reference path tracer

**Goal.** **The first physically correct image.** This is the most important
milestone in Lotus.

**Scene / GPU work.** Mesh extraction; vertex and index buffers; instance
data; BLAS construction; TLAS construction; camera representation; GPU scene
upload.

**Integrator.** Primary ray generation; triangle intersection; surface hit
reconstruction; Lambert BSDF; a minimal GGX BSDF; emissive surfaces;
environment lighting; multi-bounce path tracing; Russian roulette; HDR
accumulation.

**Validation.** A fixed, deterministic test scene, rendered by the headless
runner at

```text
1 spp · 16 spp · 64 spp · 256 spp · 1024 spp
```

These images are the reference every later acceleration is compared with
(§7).

**Exit criteria.**

- meshes are actually ray traced;
- multi-bounce illumination works;
- a deterministic reference image can be produced;
- there are no validation errors.

### Renderer Phase 1.5 — Minimal material IR

Production material support as a whole is **not** brought forward. Only a
minimal, renderer-independent material IR is introduced early
([design policy §18](DESIGN_POLICY.md#18-material-ir)):

```text
Hydra material → material translator → Lotus material IR → GPU material
```

**Initial material model.** Base colour; roughness; metallic; emissive;
opacity and an alpha policy; normal; Lambert; GGX. The first translator reads
the basic parameters of `UsdPreviewSurface`.

**Why.** So that ReSTIR and denoising are not developed only on artificial
single-material scenes, and realistic USD scenes reach validation early.

### Renderer Phase 2 — Wavefront path tracing

**Goal.** Move from the megakernel-style reference integrator to a wavefront
architecture suited to GPU scheduling.

**Initial queue model.**

```text
active paths → intersect → shading → continuation / termination → next bounce
```

**Scope.** Active-path, ray/intersection, shading, terminated-path and
next-bounce queues, with persistent path state. First reproduce reference
transport; only then optimize compaction, indirect dispatch, scheduling,
occupancy and path classification. Shadow work accompanies explicit direct
lighting in Renderer Phase 3 rather than changing transport during equivalence.

**Validation.** Deterministic output is compared with the Renderer Phase 1
reference integrator using the
[defined correctness tolerance](../reference/SCENE.md#reference-images).
The first transition excludes ReSTIR, NEE/MIS redesign and neural components.

**Exit criteria.** Reference-equivalent images and GPU scheduling that can
be optimized independently, backed by a repeatable baseline measured before
the architectural transition.

### Renderer Phase 3 — Direct lighting / NEE / MIS

**Goal.** Explicit light sampling, for practical convergence speed.

**Scope.** Light representation; light sampling; next event estimation;
shadow rays; the BSDF sampling PDF; the light sampling PDF; multiple
importance sampling with the power heuristic; emissive geometry sampling;
environment importance sampling.

**Exit criteria.** Time to equal quality improves clearly over brute-force
reference path tracing.

### Renderer Phase 4 — Temporal infrastructure

**Goal.** The temporal foundation ReSTIR and temporal denoising share, built
first ([design policy §15](DESIGN_POLICY.md#15-temporal-system)).

**Scope.** Frame history; motion vectors; reprojection; previous-frame
transforms; disocclusion detection; history validity; temporal accumulation;
a history reset policy.

**Rule.** No ReSTIR-specific code enters the temporal infrastructure.

### Renderer Phase 5 — ReSTIR DI

**Goal.** Faster direct lighting with many lights.

**Scope.** Reservoir representation; candidate generation; reservoir update;
temporal reuse; spatial reuse; visibility handling; bias and normalization
validation; light candidate strategies.

**Validation.** Always against the Renderer Phase 3 NEE / MIS renderer, on
convergence, GPU time, temporal stability, memory, and bias / artifacts.

**Exit criteria.** The phase closes on a bounded set of evidence, not once
every fixture that could be added has been:

1. **Animation and reset.** Light, material and emissive-state animation keeps
   the history lifecycle and invalidation contract: no stale reservoir is
   reused, reset classification is deterministic, the reference and wavefront
   integrators agree, Hydra applies the same reset policy, and validation is
   clean. This checks the contract; it is not a convergence campaign.
2. **Reuse cost and an equal-quality decision.** On representative workloads,
   NEE / MIS, initial-only, temporal-only, spatial-only and combined reuse are
   compared on GPU time, time to a target quality, memory and temporal
   stability.
3. **A recorded decision.** For each representative workload the exit review
   records whether there is an equal-quality speedup, the known bad cases, the
   correlation cost and the unsupported conditions, and it chooses the default
   renderer path: where ReSTIR is worth using and where NEE / MIS is.

ReSTIR does not have to win. A negative result is a result, and closes the
phase as well as a positive one.

**Stop rule.** Once the exit review is recorded, ReSTIR DI gains no new
fixtures. A later finding is routed rather than reopening the phase: a
correctness bug is fixed as a bug; a production material or scene issue goes
to Renderer Phase 8; advanced reservoir transport to Renderer Phase 7; light
selection or scheduling to Renderer Phase 9. From then on ReSTIR DI is one
selectable transport mode of the rendering pipeline, not a research theme of
its own.

### Renderer Phase 6 — Denoising / SVGF-class pipeline

**Goal.** Better quality for low-spp interactive rendering.

Renderer Phase 4 already provides motion and reprojection, stable surface
identity, temporal validity, the history reset policy, depth / normal history
and temporal radiance storage — most of what an SVGF-class denoiser needs. This
phase uses that infrastructure for denoising; it does not build a new temporal
architecture.

**Required AOVs.** Radiance; albedo; normal; depth; motion vector; variance /
moments.

**Scope.** An SVGF-class baseline, in this order: noisy radiance input;
temporal reprojection; history validation; temporal accumulation; first and
second moments; variance estimation; edge-aware à-trous filtering; final
composite. Guidance comes from depth, geometric and shading normals, motion,
stable identity and luminance variance. Disocclusion and motion rejection are
part of history validation.

**Debug products.** Every stage can be inspected, not only the final image:
raw noisy radiance, reprojected history, history validity, history length,
first and second moments, variance, filtered output and the rejection reason.

**Evaluation.** Raw low-spp radiance, temporal accumulation alone and the
filtered output are compared with a high-spp reference on RMS / MSE, temporal
residual, disocclusion response, static convergence and moving-camera
stability. A perceptual metric is not required at first.

**Rule.** The denoiser does not presuppose ReSTIR. Its input is the renderer's
noisy radiance and the shared surface metadata, so it applies equally to
NEE / MIS, initial-only and reusing ReSTIR, and later to ReSTIR GI and other
samplers; it is not coupled to the reservoir passes. The baseline is completed
first: firefly suppression, feature-adaptive filtering and neural filters
come after it.

**Exit criteria.** Temporal reprojection, moments and variance, à-trous
filtering, and motion / disocclusion rejection work under both the reference
and wavefront integrators; the output is compared with a reference; and the
denoiser can be switched on and off, with its diagnostics shown, in the
[standalone viewport](#standalone-viewport-frontend).

Neural denoising comes later, as a replacement or an additional backend
compared against this one (§5).

### Renderer Phase 7 — ReSTIR GI / advanced reservoir transport

**Goal.** Reservoir sampling beyond direct illumination. Candidates: ReSTIR
GI; path-space reservoir reuse; indirect candidate reuse; temporal GI reuse;
spatial GI reuse.

This phase is research-oriented and kept apart from production requirements.
It may start once the Renderer Phase 6 denoiser and the
[standalone viewport](#standalone-viewport-frontend) give it an interactive
evaluation environment, and its comparisons are made there as well as
headless. Their availability does not automatically schedule it. Use a
bounded experiment brief and the strategy's
[research stop rule](POST_V060_STRATEGY.md#6-research-track-and-stop-rule).

### Renderer Phase 8 — Production material support

**Goal.** Grow the minimal material IR into a material system that handles
production USD scenes.

**Targets, in priority order.**

1. `UsdPreviewSurface`
2. OpenPBR
3. MaterialX Standard Surface
4. glTF PBR–compatible mapping

**Architecture.** Shader implementations do not multiply per material source
inside the renderer:

```text
UsdPreviewSurface ─┐
OpenPBR ───────────┼─→ material translator → Lotus material IR
MaterialX ─────────┘
```

The GPU evaluates the Lotus material IR.

Selected foundation work may be delivered under the
[cross-phase rule](#cross-phase-delivery). Expansion starts from demonstrated
asset gaps, with the strategy's [entry conditions](POST_V060_STRATEGY.md#5-entry-conditions-for-later-work).
That subset does not assert complete production-material support.

### Renderer Phase 9 — Advanced sampling and scheduling

**Candidates.** Adaptive sampling; better path compaction; material sorting;
ray sorting; a light tree; hierarchical light sampling; path guiding;
blue-noise sampling; quasi-Monte Carlo; work graphs and device-generated
scheduling.

None of these is a required feature. Each is adopted on a quantitative
evaluation against the reference renderer.

Selected scheduling and light-acceleration work may use the
[cross-phase rule](#cross-phase-delivery). Intersection acceleration and
light selection require separate evidence; path guiding uses the
[research brief and stop rule](POST_V060_STRATEGY.md#6-research-track-and-stop-rule).

### Renderer Phase 10 — Spectral rendering research

**Goal.** Spectral light transport beyond RGB
([design policy §28](DESIGN_POLICY.md#28-spectral-rendering)).

**Candidates.** Sampled wavelengths; hero wavelength sampling; a spectral
BSDF representation; spectral lights; spectral textures; wavelength-dependent
IOR; dispersion; RGB ↔ spectrum reconstruction.

**Policy.** Spectral rendering is not an early requirement. It arrives as a
research branch or an experimental mode once the RGB reference renderer and
the production material pipeline are stable.

### Standalone viewport frontend

The standalone viewport (`lotus-viewport`) is not a phase. Renderer Phase 0
made it an entry point that starts the same core; it becomes the frontend
for renderer development, debugging and interactive look development, a
tool for seeing the renderer's state. It is not a usdview replacement.

**Scope.**

- *Window and presentation:* a stable swapchain lifecycle, resize, basic
  fullscreen / windowed operation, a predictable present loop, validation-clean
  presentation.
- *Scene and camera:* opening a USD stage, through an optional
  Hydra-enabled viewport build that drives the Hydra adapter in process
  ([DES-Q8](DESIGN_POLICY.md#53-open-questions)); orbit / pan / dolly,
  selecting an authored camera, restarting accumulation when the camera
  changes, applying scene dirty updates.
- *Renderer controls at run time:* the reference or wavefront integrator; spp
  and accumulation; NEE / MIS; ReSTIR off, initial, temporal, spatial or
  combined; temporal radiance reuse; the denoiser on or off.
- *Debug views:* beauty, depth, normal, motion vector, temporal validity,
  reservoir validity / age / weight, denoiser history and variance — the
  existing AOVs and debug products, reused rather than duplicated as
  viewport-only renderer state.
- *Timing:* frame time and the GPU time of path tracing, ReSTIR, temporal and
  denoise passes; sample count; scene / TLAS update time; reservoir and
  history memory. Measurement is first-class in the viewport as everywhere
  else.

**Outside its scope.** A production-grade docking UI, a scene editor, a
material editor, a node graph, an animation timeline, an asset browser, a
gizmo framework, usdview feature parity. The viewport stays a renderer
frontend; it does not become a UI framework project.

## 5. Neural features

No neural technique is a required dependency of the renderer architecture.
Candidates: neural denoising; a neural radiance cache; learned importance
sampling; learned path guiding.

```text
classical renderer
    ├── classical denoiser
    └── neural backend
```

ONNX Runtime or a vendor-specific inference runtime, if adopted, is an
optional component
([integration scope §4](INTEGRATION_SCOPE_POLICY.md#4-dependency-rules)).

## 6. Backend strategy

| Primary | Comparative / future |
| --- | --- |
| Vulkan; compute shaders; ray query; Slang | Vulkan RT pipeline; CPU reference traversal; WebGPU research; Metal / HgiMetal research |

Completing several backends at once is not a goal. The Vulkan backend is
completed first, as the reference GPU implementation.

## 7. Testing strategy

Lotus tests the renderer's architecture as well as its images.

| Kind | What |
| --- | --- |
| Structural | the public-header dependency boundary; a Hydra-independent core; a Vulkan-independent scene representation; plugin discovery; shader compilation |
| Rendering | a deterministic camera; a deterministic RNG seed; known geometry, materials and lights; reference image comparison ([design policy §26](DESIGN_POLICY.md#26-reference--deterministic-mode)) |
| GPU validation | Vulkan validation layers; synchronization validation; resource lifetime validation; descriptor validation |
| Performance | repeatable baseline before Renderer Phase 2, then tracked across changes: GPU frame time; samples/s; rays/s; primary rays/s; shadow rays/s; queue occupancy; VRAM usage; BLAS / TLAS build time; scene upload time |

A measurement is a [report](../reports/); the documentation guidelines say
what a report names. Where each check runs, and which become permanent tests,
is the [test policy](TEST_POLICY.md).

## 8. Milestones

Version numbers follow the implementation; the milestones that matter are

```text
Foundation
  → first ray-traced triangle
  → first physically correct image
  → reference path tracer
  → wavefront path tracer
  → NEE / MIS renderer
  → temporal renderer
  → ReSTIR DI renderer
  → interactive denoised renderer
  → production material renderer
  → advanced / spectral research renderer
```

Each milestone's **correctness evidence** matters more than its release
number. Which release carries which phase is decided in the
[roadmap](../roadmap/README.md#status-at-a-glance) when the selected work's
dependencies and exit evidence are defined. Earlier foundation work retains
its dependency order; later delivery follows the
[cross-phase rule](#cross-phase-delivery), not numeric phase order alone.

## 9. Near-term priority

Execution order and incomplete tasks belong in
[current.md](../roadmap/current.md#execution-order), and phase/release status
belongs in the [canonical table](../roadmap/README.md#status-at-a-glance).
This stable policy defines scope and exit criteria rather than a live backlog.

The sequencing rule is correctness → measurement → optimization: stabilize
the foundation, measure repeatable backend behavior, and preserve a fixed
reference through the Wavefront transition. Queue optimization follows
transport equivalence. Advanced features must not expand foundation closure
into an unbounded feature cycle.

The same rule bounds research phases. A phase whose investigation no longer
raises its exit confidence as fast as it adds validation cost is closed on
its exit criteria, whatever the result, and the renderer moves on. After
Renderer Phase 5 the interactive path comes first — the
[standalone viewport](#standalone-viewport-frontend) and the Renderer Phase 6
denoiser — so that further transport research can be evaluated interactively.
After that foundation, practical scene correctness, measurement and selected
optimization take priority under the [strategy](POST_V060_STRATEGY.md);
research does not gate ordinary releases. Every new pass still carries its
timing and debug AOV (§7; [design policy §24–§25](DESIGN_POLICY.md#24-gpu-profiling)).

## 10. Long-term identity

| Renderer | Role |
| --- | --- |
| `hydra-merlin` | high-performance raster rendering |
| `hydra-toon` | toon / avatar / stylized rendering |
| `hydra-lotus` | practical OpenUSD-native GPU path tracing, with an independent research track |

Lotus aims to render representative USD scenes correctly, reliably and at
a measurable GPU cost. Its reference path tracer also supports bounded
comparisons of new light-transport algorithms.

### Summary

```text
Foundation → reference path tracer → minimal material IR → wavefront → NEE / MIS
  → temporal infrastructure → ReSTIR DI → denoising + standalone viewport
      ├─ practical scene correctness → measurement → selected optimization
      └─ independent, bounded transport / spectral / neural research
```

The principle throughout: **correctness → measurement → optimization →
research.**

---

## 11. Where this repository departs from the source policy

These decisions explain the binding structural contract in
[PROJECT_LAYOUT.md](../architecture/PROJECT_LAYOUT.md), rather than tracking
phase completion.

| Source policy | Here | Why |
| --- | --- | --- |
| §2.2, §3 — render extraction sits between the `hdLotus` adapter and `LotusScene` | The adapter extracts Hydra prims into `LotusScene`; `core/render-extraction` runs **after** `LotusScene` and turns its changes into GPU update work for `GpuScene` | That is the scaffold's existing order (`RenderWorld::Commit` → `ExtractDrawSummary` → backend) and the [layout's](../architecture/PROJECT_LAYOUT.md#3-where-new-code-goes) placement of the update plan. Hydra-side extraction (mesh extraction, dirty tracking) is the adapter's job ([design policy §3.1](DESIGN_POLICY.md#31-hydra-is-a-scene-integration-layer)), so the core never sees a Hydra type either way. |

## 12. How the design policy's phases map here

The design policy's §36–§46 phase list, from the implementation direction, is
replaced by §4. Where its items went:

| Design policy | Here |
| --- | --- |
| §36 Bootstrap — plugin, delegate, render pass, camera, device, image output, triangle, basic AOVs | Renderer Phase 0, which adds the core separation, the viewport, the headless runner and CI boundary tests, and makes the exit criterion "all three entry points start the same core" |
| §37 Baseline path tracer | Renderer Phase 1, now the **reference path tracer**: GGX is in it (minimal), and so are mesh extraction, GPU scene upload and HDR accumulation |
| §39 NEE / MIS — "makes the reference path tracer" | Renderer Phase 3. The reference is the Renderer Phase 1 brute-force path tracer; NEE / MIS is the baseline ReSTIR DI is compared against |
| §44 Material and production scene support | Split: the basic `UsdPreviewSurface` parameters move up to Renderer Phase 1.5; the rest is Renderer Phase 8, now with OpenPBR ahead of MaterialX. Instance data moves to Renderer Phase 1 |
| §45 Advanced sampling research | Renderer Phase 9, which adds scheduling (sorting, compaction, work graphs). ReSTIR path guiding, reservoir splatting, control variates and MCMC stay on the [research track](DESIGN_POLICY.md#27-advanced-research-track) |
| §46 Quality / spectral research | Renderer Phase 10, spectral only. BDPT and ReSTIR BDPT stay on the research track |
| §47 Versioning guide | §8: milestones, not a version sketch |
| §49 Development order | §10 Summary |
