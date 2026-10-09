---
status: accepted
owner: hydra-lotus
---

# hydra-lotus — design policy

> Status: **accepted** as the project's design policy, 2026-10-04. Nothing
> below declares implementation status. Technical behavior and limitations
> belong in [reference](../reference/README.md); phase and release status
> belongs in the [canonical roadmap](../roadmap/README.md#status-at-a-glance).
>
> This is the canonical, long-form policy: what the renderer is for, how it is
> shaped, and the order it is built in. It is distilled from the 2026-10-04
> implementation direction, whose section numbers it keeps so either can be
> cited by number. Where this repository departs from that direction, §52
> records it; where the direction is not yet decided, §53 does. Focused
> documents own the detail of one area each, and **on its own area the
> focused document wins**:
>
> | Area | Owning document |
> | --- | --- |
> | What this repository owns, and what it consumes from whom | [INTEGRATION_SCOPE_POLICY.md](INTEGRATION_SCOPE_POLICY.md) |
> | Targets, directories and dependency directions | [architecture/PROJECT_LAYOUT.md](../architecture/PROJECT_LAYOUT.md) |
> | The Renderer Phase 0–10 sequence: goals, scope, exit criteria, milestones and testing strategy | [ROADMAP_POLICY.md](ROADMAP_POLICY.md) |
> | Which release carries which phase | [roadmap/README.md](../roadmap/README.md) |

---

## 1. Purpose

`hydra-lotus` is a **high-performance, research-oriented GPU path tracing
render delegate** for OpenUSD / Hydra. It is not merely "path tracing shown
through Hydra". It is a rendering foundation on which the following can be
implemented and verified continuously:

- GPU path tracing on Vulkan;
- wavefront path tracing;
- next event estimation (NEE), multiple importance sampling (MIS) and
  Russian roulette;
- ReSTIR DI, GI and PT, with temporal and spatial reuse;
- progressive rendering;
- SVGF-family denoising, and later neural denoising;
- current Monte Carlo sampling and light transport research;
- later, spectral rendering, BDPT and other advanced sampling experiments;
- a vendor-neutral implementation that runs on NVIDIA, AMD and Intel.

Among the sibling renderers, `hydra-merlin` is a real-time raster renderer and
`hydra-toon` a toon / avatar renderer. `hydra-lotus`'s ground is **physically
based, Monte Carlo, path-traced** rendering — physically based GPU light
transport and a rendering research platform
([roadmap policy §10](ROADMAP_POLICY.md#10-long-term-identity)).

## 2. Core concept

### 2.1 The role of Lotus

```text
OpenUSD scene
    │
    ▼
Hydra
    │
    ▼
hydra-lotus
    ├── interactive path tracing
    ├── progressive path tracing
    ├── ReSTIR
    ├── denoising
    └── research / quality mode
```

> **A renderer in which current GPU light transport algorithms can be
> exercised on OpenUSD / Hydra scenes.**

## 3. Design principles

### 3.1 Hydra is a scene integration layer

Hydra-specific state management does not spread through the renderer core.

```text
OpenUSD / Hydra → Hydra adapter → LotusScene → GpuScene → path tracing core → AOV / denoise / presentation
```

The Hydra adapter is responsible for: mesh, curves and points extraction;
transforms; instancing; camera; lights; materials; textures; dirty tracking;
AOVs; render settings.

The rendering algorithms are independent of Hydra, so that a standalone
viewport, a benchmark executable, a headless renderer and a test-scene runner
can all drive the same Lotus core.

## 4. Renderer core

### 4.1 LotusScene

The CPU-side, renderer-neutral scene representation:

```text
LotusScene
 ├── Geometry
 ├── Instances
 ├── Materials
 ├── Textures
 ├── Lights
 ├── Cameras
 └── RenderSettings
```

Hydra's `HdRprim`, `HdSprim` and related types are never exposed through the
renderer core's interfaces.

### 4.2 GpuScene

The scene representation optimized for the GPU:

```text
GpuScene
 ├── vertex buffer      ├── light buffer
 ├── index buffer       ├── texture table
 ├── material buffer    ├── BLAS
 ├── instance buffer    └── TLAS
```

Bindless / descriptor indexing is the long-term baseline. Policy:

- GPU resources are persistent;
- only dirty resources are updated, and a full scene rebuild is avoided;
- BLAS / TLAS **update** and **rebuild** are distinct operations;
- material, transform and light updates are separate from geometry upload.

Where `GpuScene` lives in this repository is §52.

## 5. GPU backend

### 5.1 Vulkan first

The first — and reference — backend is **Vulkan**, for: Windows and Linux;
NVIDIA, AMD and Intel; ray query; the ray tracing pipeline; descriptor
indexing; subgroup operations; timeline semaphores; and a mature profiling
ecosystem.

### 5.2 Slang

The shader language is **Slang**, for: shader code sharing; structured GPU
programming; Vulkan / SPIR-V; later backends; and a reusable BSDF and sampling
library. The Lotus core is not tightly coupled to the Slang runtime (§52).

## 6. Ray tracing backend

> **Compute + ray query is the main path; the RT pipeline stays available as
> a selectable backend.**

| | Compute + ray query | RT pipeline |
| --- | --- | --- |
| Why | suits wavefront; queue-driven architecture is easy to build; control over shader execution flow; integrates with ReSTIR and temporal passes | comparison benchmarks; straightforward use of hardware traversal; cases where a platform or GPU favours it |

The two are not separate renderers. They are abstracted as a
`TraversalBackend`. Which backends are primary and which are comparative —
including a CPU reference traversal — is the
[roadmap policy §6](ROADMAP_POLICY.md#6-backend-strategy).

## 7. Wavefront path tracing

The core architecture is **wavefront path tracing**: path state is split into
queues rather than run as one megakernel.

```text
Generate primary rays
        │
        ▼
    Ray queue
        │
        ▼
    Intersect
        │
        ▼
    Hit queue
   ╱          ╲
Miss         Surface
                │
                ▼
          Shade / BSDF
          ╱          ╲
 Shadow queue      Next-ray queue
      │                  │
      ▼                  │
 Visibility              │
      └──────────────────┘
```

Representative queues: `RayQueue`, `HitQueue`, `MissQueue`, `ShadowQueue`,
`ScatterQueue`, `TerminatedQueue`.

## 8. Path state

At minimum:

```cpp
struct PathState {
    Ray ray;
    float3 throughput;
    float3 radiance;

    uint32_t pixelIndex;
    uint32_t depth;
    uint32_t rngState;

    float lastPdf;
    uint32_t flags;
};
```

RGB throughput is **not** fixed into the renderer-wide interface, so a
spectral representation can follow (§28). The first implementation is RGB.

## 9. Baseline path tracer

Before any research feature:

> **An ordinary path tracer that is entirely correct.**

Required: camera rays; triangle intersection; surface normals; Lambert; GGX;
emissive surfaces; environment light; multiple bounces; Russian roulette.
This brute-force path tracer is Lotus's **reference path tracer**, the one
every faster mode is compared with
([roadmap policy §2.1](ROADMAP_POLICY.md#21-correctness-before-performance)).
Then NEE and MIS are added. The light transport ReSTIR is measured against is

```text
BSDF sampling  +  light sampling  →  MIS
```

## 10. NEE / MIS

**NEE + MIS is completed before ReSTIR**, because it is the baseline ReSTIR
is compared against, it isolates sampling bugs, it remains the offline /
progressive quality mode, and it is the benchmark baseline. It is itself
checked against the brute-force reference path tracer (§9). The first MIS
weight is the power heuristic.

## 11. ReSTIR

ReSTIR is one of Lotus's main differentiators. Order of introduction:

```text
ReSTIR DI → temporal reuse → spatial reuse → ReSTIR GI → ReSTIR PT
```

GI is not attempted first.

## 12. Reservoir

```cpp
struct Reservoir {
    Sample y;
    float wSum;
    float targetPdf;
    uint32_t M;
};
```

Extended as needed with the selected sample, source light, visibility,
age and confidence. Reservoir storage stays GPU-friendly.

## 13. ReSTIR DI

The first ReSTIR milestone. Temporal and spatial reuse are designed on direct
illumination first.

```text
Initial candidates → local reservoir ─┬─→ temporal reuse ─┐
                                      └─→ spatial reuse  ─┴─→ final reservoir → visibility test → shading
```

## 14. ReSTIR GI / PT

GI follows once DI is stable, for low-spp indirect illumination, path reuse,
temporal reuse and interactive global illumination. ReSTIR PT comes later
still. Interactive path tracing in Lotus is the combination

```text
low spp + sample reuse + temporal accumulation + denoising
```

## 15. Temporal system

ReSTIR and denoising share one temporal history infrastructure. Minimum
buffers: depth; normal; motion vector; material / instance identity;
previous radiance; reservoir; moments / variance; sample count.

**History validation is a separate, shared function.** Rejection conditions:
disocclusion; depth discontinuity; normal discontinuity; material change;
instance change; camera cut; large motion.

## 16. Denoising

| Phase | Content |
| --- | --- |
| 1 | a lightweight temporal denoiser on the GPU |
| 2 | SVGF: noisy radiance → temporal reprojection → moment / variance estimation → à-trous filter → final image |
| 3 | neural denoising |

Required AOVs: depth, normal, albedo, motion, roughness, and direct / indirect
separation where needed. The renderer core never depends on a neural
denoiser; denoisers are interchangeable behind a `DenoiserBackend`.

## 17. Material

Initial priority: 1. Lambert, 2. GGX dielectric, 3. GGX metallic,
4. emissive, 5. transmission, 6. clearcoat.

Long-term inputs from Hydra / USD, in priority order: `UsdPreviewSurface`,
OpenPBR, MaterialX Standard Surface, then a glTF PBR–compatible mapping. The
basic `UsdPreviewSurface` parameters come early, as the minimal material IR of
Renderer Phase 1.5
([roadmap policy §4](ROADMAP_POLICY.md#renderer-phase-15--minimal-material-ir)).

## 18. Material IR

Hydra and MaterialX shader graphs are not the path tracer's internal
representation:

```text
Hydra material → material translator → Lotus material IR → GPU material
```

This separates the backend and shader architecture from the authoring schema.

## 19. Lighting

Order: 1. environment, 2. point, 3. directional, 4. area light,
5. emissive mesh. For ReSTIR DI, many lights and emissive geometry matter
most. The structure allows comparing a light tree, hierarchical light
sampling, RIS and ReSTIR.

## 20. Acceleration structure

```text
Geometry → BLAS → instance → TLAS
```

Priorities: static BLAS; dynamic BLAS; refit vs rebuild; instancing; motion
and transform updates; a compact GPU memory layout. Hydra dirty bits drive the
acceleration-structure update policy.

## 21. Interactive and progressive mode

| | Interactive mode | Progressive mode |
| --- | --- | --- |
| For | viewport, camera operation, look development | still frames, quality evaluation, reference, research comparison |
| Character | low spp; ReSTIR; temporal reuse; denoising; aggressive history reuse | unbiased or low-bias; high sample count; NEE / MIS; minimal dependence on temporal heuristics |

## 22. Performance target

Long-term guide: **1920 × 1080, 1 spp, 2–4 bounces, ReSTIR, temporal reuse
and denoising at 30–60+ FPS** interactive. 60 FPS is not an absolute
requirement. What matters: stable frame time; low CPU overhead; a scalable GPU
architecture; good scaling with scene size.

## 23. CPU performance

In a Hydra renderer, CPU submit cost matters as much as GPU time. Goals: near
zero CPU cost when the scene is unchanged; pipeline creation outside the
render loop; minimal descriptor rebuilds; upload of dirty resources only; a
persistent command and resource architecture; a shader and pipeline cache.

## 24. GPU profiling

Per-pass GPU timestamps exist **from the start**: TLAS update, primary ray,
intersect, shade, shadow, ReSTIR temporal, ReSTIR spatial, denoise, tone
mapping, presentation. Performance is never judged by FPS alone. Recorded:
GPU frame time; CPU frame time; ray count; path count; shadow ray count;
average path depth; queue occupancy; reservoir reuse rate; denoiser cost;
memory usage. What a research feature is measured on, and the performance
tests, are the
[roadmap policy §2.5 and §7](ROADMAP_POLICY.md#25-research-features-stay-measurable).

## 25. Debug and validation

Correctness is hard to see in a path tracer, so debug AOVs are a first-class
feature: world normal; geometric normal; albedo; roughness; depth; instance
ID; primitive ID; path depth; throughput; direct; indirect; emission; sample
count; reservoir weight; reservoir M; temporal validity.

The foundation subset, formats and clear behaviour are owned by the
[AOV reference](../reference/AOVS.md).

## 26. Reference / deterministic mode

A deterministic mode exists for regression testing: fixed RNG seed, fixed
spp, fixed camera, fixed frame index. Golden-image tests run on it. The
reference images and the testing strategy are the
[roadmap policy §4 (Renderer Phase 1) and §7](ROADMAP_POLICY.md#7-testing-strategy).

## 27. Advanced research track

After the foundation is complete, research is taken in step by step: ReSTIR
PT; ReSTIR path guiding; reservoir splatting; area ReSTIR; multi-layer
ReSTIR; control variates; MCMC-based reuse; advanced path guiding; BDPT;
ReSTIR BDPT; difficult light transport and caustics; volumes.

These form a **research track kept separate from the core milestones**.

## 28. Spectral rendering

Spectral rendering is not an initial requirement: a complete RGB renderer
comes first; the material and texture pipeline gets more complex; and
introducing it alongside ReSTIR, wavefront and denoising would make
verification hard. The research path stays open:

> **The first implementation is RGB; the internal interfaces do not block a
> later spectrum representation.**

Spectral work, if it comes, is a quality / research mode, introduced in
steps:

```text
RGB → hero wavelength → wavelength sampling → dispersion / thin film → full spectral experiments
```

## 29. Volumes

Out of initial scope. The architecture leaves room for `UsdVol`, OpenVDB,
homogeneous and heterogeneous volumes, and delta / ratio tracking as research
subjects.

## 30. Relationship with hydra-merlin

Lotus and Merlin are not merged into one renderer: Merlin is raster, Lotus is
path tracing. Low-level utilities are **candidates** for sharing; sharing is
not a dependency today (§53 DES-Q4).

| May be shared | Not shared |
| --- | --- |
| Vulkan initialization; allocator; shader compilation; descriptor helpers; pipeline cache; GPU timers; image / buffer abstraction; debug utilities | scene representation; material runtime; render graph policy; renderer-specific scheduling; the Hydra synchronization core |

Over-generalizing into a common core is avoided.

## 31. Relationship with hydra-toon

`hydra-toon` is optimized for the VRM / MMD / MToon look and a low-latency
viewport; Lotus for physically based light transport. They are not
integrated. The ideal is a USD scene from which any of `hydra-toon`,
`hydra-merlin` and `hydra-lotus` can be selected as the render delegate.

## 32. Use of OpenUSD / Hydra

First: `HdMesh`, `HdInstancer`, `HdMaterial`, `HdLight`, `HdCamera`, AOVs,
render settings. Later: curves, points, volumes, procedurals. Hydra dirty
tracking is used, but the renderer core is never designed around the
convenience of the Hydra API.

## 33. Repository structure

The direction sketches `src/hdLotus/`, `src/lotus/` and `src/vulkan/`, and a
root `shaders/` tree. This repository uses the OpenStrata renderer layout
instead and keeps the same separation; the mapping is
[PROJECT_LAYOUT.md](../architecture/PROJECT_LAYOUT.md) §3, and the departure
is §52.

## 34. API boundaries

These are kept distinct:

```text
Hydra → Hydra adapter → LotusScene → GpuScene → Integrator → backend
```

The integrator is an interface, for example

```cpp
class Integrator {
public:
    virtual void Render(const GpuScene&, const Camera&, RenderTargets&) = 0;
};
```

so that a `ReferencePathTracer`, a `WavefrontPathTracer` and a
`ReSTIRPathTracer` can be switched.

## 35. Render graph

A lightweight render graph is introduced **when pass dependencies start to
grow**, not after the renderer has become complicated:

```text
scene update → AS update → path trace → ReSTIR → temporal → denoise → tone map → AOV / present
```

What matters is that resource lifetime and synchronization are explicit.

## 36–46. Implementation phases

> **Superseded** by the [roadmap policy §4](ROADMAP_POLICY.md#4-roadmap),
> 2026-10-04. The headings stay so citations resolve; where each phase's
> items went is [roadmap policy §12](ROADMAP_POLICY.md#12-how-the-design-policys-phases-map-here).

### 36. Renderer Phase 0 — Bootstrap

Now Renderer Phase 0 — Foundation.

### 37. Renderer Phase 1 — Baseline path tracer

Now Renderer Phase 1 — Reference path tracer, followed by Renderer Phase
1.5 — Minimal material IR.

### 38. Renderer Phase 2 — Wavefront

Now Renderer Phase 2 — Wavefront path tracing.

### 39. Renderer Phase 3 — NEE / MIS

Now Renderer Phase 3 — Direct lighting / NEE / MIS.

### 40. Renderer Phase 4 — Temporal infrastructure

Unchanged in name.

### 41. Renderer Phase 5 — ReSTIR DI

Unchanged in name.

### 42. Renderer Phase 6 — SVGF / denoising

Now Renderer Phase 6 — Denoising / SVGF-class pipeline.

### 43. Renderer Phase 7 — ReSTIR GI / PT

Now Renderer Phase 7 — ReSTIR GI / advanced reservoir transport.

### 44. Renderer Phase 8 — Material and production scene support

Now Renderer Phase 8 — Production material support.

### 45. Renderer Phase 9 — Advanced sampling research

Now Renderer Phase 9 — Advanced sampling and scheduling.

### 46. Renderer Phase 10 — Quality / spectral research

Now Renderer Phase 10 — Spectral rendering research. **The RGB interactive
path is never broken for it** (§51).

## 47. Versioning guide

> **Superseded** by the milestones of the
> [roadmap policy §8](ROADMAP_POLICY.md#8-milestones), 2026-10-04. The
> direction's version sketch is no longer used; a phase gets a version only
> in the [roadmap](../roadmap/README.md#status-at-a-glance).

The order still holds: **correctness → architecture → reuse → denoise →
advanced sampling**, which the roadmap policy states as **correctness →
measurement → optimization → research**.

## 48. Not done first

- spectral rendering from the start;
- ReSTIR GI before ReSTIR DI;
- a neural denoiser as a required dependency;
- Vulkan, WebGPU and Metal at the same time;
- a renderer core shared with `hydra-merlin`;
- a large project-specific material schema;
- optimizing the RT pipeline and ray query paths at the same time;
- complete production material support first.

## 49. Development order

The order is the [roadmap policy's summary](ROADMAP_POLICY.md#summary):

```text
Foundation → reference path tracer → minimal material IR → wavefront → NEE / MIS
  → temporal infrastructure → ReSTIR DI → denoising → ReSTIR GI / advanced reservoirs
  → production materials → advanced sampling → spectral / neural research
```

## 50. Direction

`hydra-lotus` does not aim to be a production renderer from day one. It
grows as

> **an experimental and practical renderer for cutting-edge GPU light
> transport, entered through OpenUSD / Hydra.**

Its core is OpenUSD / Hydra + Vulkan + wavefront path tracing + NEE / MIS +
ReSTIR + temporal reconstruction + denoising. Once that foundation is
complete, ReSTIR PT, path guiding, reservoir splatting, control variates,
BDPT, difficult light transport, spectral experiments and neural
reconstruction can be stacked on it safely.

## 51. Decision principles

When a decision is unclear, prefer in this order:

1. **Correctness before cleverness.**
2. **Baseline before ReSTIR.**
3. **Wavefront before premature micro-optimization.**
4. **Thin Hydra adapter.**
5. **Hydra-independent renderer core.**
6. **Vulkan first.**
7. **Cross-vendor design.**
8. **AOVs, profiler and deterministic tests early.**
9. **Research features as independent integrators or passes.**
10. **Never break the practical RGB renderer for an advanced feature.**

---

## 52. Where this repository departs from the implementation direction

These decisions explain the binding structural contract in
[PROJECT_LAYOUT.md](../architecture/PROJECT_LAYOUT.md), rather than tracking
phase completion.

| Direction | Here | Why |
| --- | --- | --- |
| §33 — `src/{hdLotus,lotus,vulkan}`, root `shaders/` | The OpenStrata renderer layout: `core/`, `backend/`, `adapters/`, `include/lotus/`, `validation/` ([PROJECT_LAYOUT.md](../architecture/PROJECT_LAYOUT.md) §2–3) | The scaffold's layout is what `ost build`, `ost validate` and the renderer evidence contract are wired to, and its core-boundary check enforces §3.1 and §4.1 mechanically. The direction's separation — Hydra adapter / renderer core / Vulkan / shaders — is kept one-to-one. |
| §32, §36 — "the Hydra adapter", `src/hdLotus/` | The adapter lives at `adapters/hydra2/` and is named `hydra2` in `openstrata.renderer.yaml` | That is OpenStrata's name for the Hydra scene-input slot. The code is a classic `HdRenderDelegate` + `HdRendererPlugin`; the directory name claims nothing about the Hydra 2.0 renderer interface. |
| §4.2, §34 — `GpuScene` and the integrators are part of the renderer core | `GpuScene`, the traversal backends and the integrator implementations live in the Vulkan backend. The core owns `LotusScene`, the host-neutral update plan that tells the backend what changed, and the integrator *selection* and settings. | `GpuScene` holds BLAS / TLAS and Vulkan buffers, and the core's public headers may not carry Vulkan types (§3.1; [PROJECT_LAYOUT.md](../architecture/PROJECT_LAYOUT.md) §4). The integrators are GPU passes over that scene. The direction's boundary — the core does not know Hydra, Hydra does not reach the GPU scene — is unchanged. |
| §5.2 — Slang is a strong candidate | Slang is the shader language from Renderer Phase 0, compiled offline to SPIR-V by `slangc` at build time | The scaffold already builds its shaders this way, and offline compilation keeps the core free of the Slang runtime, which is the coupling §5.2 warns against. |

## 53. Open questions

| ID | Question | Blocks |
| --- | --- | --- |
| DES-Q1 | ~~SVGF before or after ReSTIR DI?~~ **Resolved 2026-10-04** by the [roadmap policy](ROADMAP_POLICY.md#4-roadmap): ReSTIR DI (Renderer Phase 5) comes before denoising (Renderer Phase 6), and the §47 sketch that disagreed is superseded. | — |
| DES-Q2 | ~~Is GGX in the baseline?~~ **Resolved 2026-10-04** by the [roadmap policy](ROADMAP_POLICY.md#renderer-phase-1--reference-path-tracer): a minimal GGX BSDF is part of Renderer Phase 1, before wavefront. | — |
| DES-Q3 | **Resolved:** isolate spectrum representation in the shader library so transport does not depend on RGB components. Host-authored inputs and output conversions define the boundary; implementation details are owned by the [path-tracing reference](../reference/SCENE.md#path-tracing). | — |
| DES-Q4 | **How are low-level Vulkan utilities shared with `hydra-merlin` (§30)?** Copy, a shared package, or not at all. Until decided, nothing is shared and there is no dependency. | nothing yet |
| DES-Q5 | **Resolved:** deterministic reference comparisons use a statistical tolerance that accounts for Monte Carlo variance. The metric, sample counts and reference format are owned by the [reference-image contract](../reference/SCENE.md#reference-images). | — |
| DES-Q6 | **How does a glTF PBR–compatible mapping reach Lotus?** The roadmap policy lists it as Renderer Phase 8's fourth material target; the [integration scope](INTEGRATION_SCOPE_POLICY.md#1-the-rule) allows only standard schemas on the composed stage and no source-format parsing. One reading is MaterialX's glTF PBR node definition; that is not decided. | Renderer Phase 8 |
