# Roadmap

This index owns **phase and release status**. The only active task checklist
is [current.md](current.md). Shipped work is in the
[CHANGELOG](../../CHANGELOG.md) and [release records](../releases/README.md);
rationale lives in [design](../design/README.md).
Sibling repositories' work is planned in their own roadmaps and not mirrored
here.

Legend: ✅ done · 🚧 in progress · ⬜ not started · ⛔ blocked · ⚠️ accepted workaround

| Document | Contents |
| --- | --- |
| [current.md](current.md) | v0.6.0 execution order (standalone viewport, denoising, after the ReSTIR DI closure) and deferred adapter work. |

## Sequences

Two sequences are live:

- **Renderer Phase 0–10**, with Renderer Phase 1.5
  ([roadmap policy §4](../design/ROADMAP_POLICY.md#4-roadmap)) — the core
  milestones: each phase's goal, scope and exit criteria. A reference is
  always qualified — "Renderer Phase 1", never a bare "Phase 1" — because
  every sibling repository has phases of its own.
- **The research track**
  ([design policy §27](../design/DESIGN_POLICY.md#27-advanced-research-track)) —
  kept separate from the core milestones. Its items enter Renderer Phase 7, 9
  and 10 as experimental integrators or passes and are not scheduled until the
  foundation they build on exists. Neural features are optional components
  throughout ([roadmap policy §5](../design/ROADMAP_POLICY.md#5-neural-features)).

## Status at a glance

**This table is the single source of truth for phase status and release
targets.** Other documents link here instead of repeating either. Version
numbers follow the implementation
([roadmap policy §8](../design/ROADMAP_POLICY.md#8-milestones)): a phase gets
a version here when its predecessor is done, or when its predecessor is
reduced to a bounded closure in the same release.

| Phase | Status | Target |
| --- | --- | --- |
| Renderer Phase 0 — Foundation | ✅ scope and entry-point exit reviewed ([closure evidence](../reports/2026-10-10-foundation-closure.md)); [release gate passed](../reports/2026-10-10-release-gate.md) | [v0.1.0](../releases/v0.1.0.md) |
| Renderer Phase 1 — Reference path tracer | ✅ exit criteria met ([reference images](../reports/2026-10-05-reference-images.md), [Hydra determinism](../reports/2026-10-05-hydra-deterministic-mode.md), [GPU timestamps](../reports/2026-10-05-gpu-timestamps.md)) | [v0.1.0](../releases/v0.1.0.md) |
| Renderer Phase 1.5 — Minimal material IR | ✅ minimal scope implemented (including dielectric GGX and the specular workflow, [evidence](../reports/2026-10-08-dielectric-specular.md); limits in the [scene reference](../reference/SCENE.md#materials-and-environment)) | [v0.1.0](../releases/v0.1.0.md) |
| Renderer Phase 2 — Wavefront path tracing | ✅ exit criteria met ([exit review](../reports/2026-10-11-hydra-integrator-selection.md#renderer-phase-2-exit)): reference equivalence reached ([evidence](../reports/2026-10-10-wavefront-equivalence.md)); queue bookkeeping and round scheduling [measured](../reports/2026-10-10-wavefront-scheduling.md), path state [stored by section](../reports/2026-10-10-wavefront-path-state.md), generate and accumulate kernels [replaced](../reports/2026-10-11-wavefront-camera-resolve.md); path classification deferred to Renderer Phase 8 ([upper bound](../reports/2026-10-11-wavefront-camera-resolve.md#path-classification)); integrator [selected through Hydra](../reports/2026-10-11-hydra-integrator-selection.md) | [v0.2.0](../releases/v0.2.0.md) |
| Renderer Phase 3 — Direct lighting / NEE / MIS | ✅ exit criteria met ([exit review](../reports/2026-10-12-renderer-phase3-exit.md#renderer-phase-3-exit)): next event estimation of emissive triangles with MIS in both integrators and through Hydra ([evidence](../reports/2026-10-11-emissive-nee-mis.md)); UsdLux sphere, rect, disk and distant lights sampled beside them ([evidence](../reports/2026-10-11-analytic-lights.md)); dome textures as an environment image sampled in proportion to its radiance ([evidence](../reports/2026-10-11-environment-importance-sampling.md)); time to equal quality 2.6–3.0× shorter in the Cornell box and by three orders of magnitude or more in the lit workloads under both integrators; light acceleration assigned to Renderer Phase 9 (LOTUS-LIGHT-05) | [v0.3.0](../releases/v0.3.0.md) |
| Renderer Phase 4 — Temporal infrastructure | ✅ exit criteria met ([scope review](../reports/2026-10-13-temporal-hydra-exit.md#renderer-phase-4-scope-review), [release gate](../reports/2026-10-13-v0.4.0-release-gate.md)): CPU/GPU frame history, rigid motion, reprojection, shared validity, bounded radiance and reset policy; Hydra controls/AOVs and moving installed-host sequences verified under both integrators | [v0.4.0](../releases/v0.4.0.md) |
| Renderer Phase 5 — ReSTIR DI | ✅ exit criteria met ([exit review](../reports/2026-10-17-renderer-phase5-exit.md)): initial, temporal and spatial reservoirs, proposals and Hydra controls/AOVs ([contract](../reference/RESTIR.md)); animation/reset contract kept ([animation](../reports/2026-10-17-restir-animation.md), [reset contract](../reports/2026-10-17-restir-reset-contract.md)); no reuse bias, ghosting or localized error detected in the bounded fixtures; [reuse cost](../reports/2026-10-17-restir-reuse-cost.md) at locked clocks: NEE/MIS stays the default, initial-only reservoirs with eight candidates about 1.4× faster to equal quality only with many distant lights, temporal, spatial and combined reuse not worth using; the [stop rule](../design/ROADMAP_POLICY.md#renderer-phase-5--restir-di) applies | [v0.5.0 experimental snapshot](../releases/v0.5.0.md); closure [v0.5.1](../releases/v0.5.1.md) |
| Renderer Phase 6 — Denoising / SVGF-class pipeline | ⬜ SVGF-class baseline [planned](current.md#step-3--svgf-core) | v0.6.0 |
| Renderer Phase 7 — ReSTIR GI / advanced reservoir transport | ⬜ | unscheduled |
| Renderer Phase 8 — Production material support | ⬜ | unscheduled |
| Renderer Phase 9 — Advanced sampling and scheduling | ⬜ | unscheduled |
| Renderer Phase 10 — Spectral rendering research | ⬜ | unscheduled |

## v0.6.0 — Interactive rendering

> **From validated transport research to interactive rendering.**

After v0.5.0 the ReSTIR DI investigation reached the point where more
fixtures cost more than they add to exit confidence. v0.6.0 turns from making
ReSTIR smarter to making Lotus usable on its own: open a scene, watch
progressive path tracing, and observe and switch the temporal, ReSTIR and
denoising state. ReSTIR DI becomes one selectable transport mode of that
pipeline rather than a research subject of its own.

| Track | Scope | Order |
| --- | --- | --- |
| ReSTIR DI closure | the [Renderer Phase 5 exit criteria and stop rule](../design/ROADMAP_POLICY.md#renderer-phase-5--restir-di); [exit review](../reports/2026-10-17-renderer-phase5-exit.md) shipped early in [v0.5.1](../releases/v0.5.1.md) | [step 1](current.md#step-1--restir-di-closure) |
| Standalone viewport | the [viewport frontend scope](../design/ROADMAP_POLICY.md#standalone-viewport-frontend); foundation shipped early in [v0.5.2](../releases/v0.5.2.md) | [step 2](current.md#step-2--viewport-foundation) |
| SVGF-class denoising | the [Renderer Phase 6 baseline](../design/ROADMAP_POLICY.md#renderer-phase-6--denoising--svgf-class-pipeline), headless first, then in the viewport | [steps 3–4](current.md#step-3--svgf-core) |

The Renderer Phase 5 exit review, with its reuse-cost / equal-quality
conclusion and no open research fixture, was recorded and released in
[v0.5.1](../releases/v0.5.1.md). v0.6.0 is complete when the viewport opens a
USD stage standalone, with an interactive camera, stable progressive rendering,
run-time switching of the major renderer modes, debug AOVs and GPU timings;
and the denoiser meets its exit criteria under both integrators. Its [gate](current.md#step-5--release-gate) adds headless
correctness, an interactive viewport smoke test, the installed Hydra
regression, deterministic denoiser validation, GPU timings, clean validation
and synchronization, and the package / release gate.

Deliberately later: ReSTIR GI (after the denoiser and viewport exist, so it is
evaluated interactively), path guiding, production MaterialX / OpenPBR
expansion, spectral rendering, neural denoising, broad platform / backend
expansion, light trees and many-light acceleration, and editor functionality
in the viewport.

```text
v0.5.0   ReSTIR DI experimental snapshot
   ↓
v0.5.1   Renderer Phase 5 closure — animation/reset validation, reuse cost / equal-quality decision
   ↓
v0.5.2   viewport foundation — presentation, camera, run-time controls, debug views, timing HUD
   ↓
v0.6.0   interactive rendering — standalone viewport, SVGF-class denoising
   ↓
Renderer Phase 7   ReSTIR GI / advanced reservoir transport
   ↓
Renderer Phase 8   production materials
   ↓
Renderer Phase 9   advanced sampling / scheduling
   ↓
Renderer Phase 10  spectral research
```

## Milestone evidence

Milestone definitions are owned by the
[phase policy](../design/ROADMAP_POLICY.md#8-milestones). Status remains in
[the table above](#status-at-a-glance); evidence of the reference foundation
is collected here without another status table.

| Milestone | Evidence |
| --- | --- |
| First ray-traced triangle | [Ray-query measurement](../reports/2026-10-05-primary-rays.md) |
| First physically correct image | [HDR accumulation measurement](../reports/2026-10-05-hdr-accumulation.md) |
| Reference path tracer | [Reference-image comparison](../reports/2026-10-05-reference-images.md) |

## Quality bar (applies to every phase)

- A faster mode is checked against the reference path tracer, on a
  deterministic run, before it is called correct
  ([roadmap policy §2.1, §7](../design/ROADMAP_POLICY.md#21-correctness-before-performance)).
- A research feature is adopted on a measurement against the reference, not
  because it works
  ([roadmap policy §2.5](../design/ROADMAP_POLICY.md#25-research-features-stay-measurable)).
- Public core headers stay free of OpenUSD, Hydra, Vulkan and windowing types,
  and CI enforces it ([PROJECT_LAYOUT.md §4](../architecture/PROJECT_LAYOUT.md#4-dependency-directions)).
- An unchanged scene costs almost no CPU time; no pipeline is created inside
  the render loop ([design policy §23](../design/DESIGN_POLICY.md#23-cpu-performance)).
- A new pass carries its GPU timestamp and its debug AOV from the change that
  adds it ([design policy §24–§25](../design/DESIGN_POLICY.md#24-gpu-profiling)).
- A research feature never breaks the RGB interactive path
  ([design policy §51](../design/DESIGN_POLICY.md#51-decision-principles)).
- A performance or quality claim cites a measurement, and a measurement is a
  [report](../reports/).
- Every documented command is one that has actually been run.
