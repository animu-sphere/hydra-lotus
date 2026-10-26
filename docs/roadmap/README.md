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
| [current.md](current.md) | Next measurements, material fixtures, memory inventory and light profiling. |

## Sequences

Capability scopes and research are tracked separately:

- **Renderer Phase 0–10**, with Renderer Phase 1.5
  ([roadmap policy §4](../design/ROADMAP_POLICY.md#4-roadmap)) — stable capability
  scopes: each phase's goal, scope and exit criteria. Phase numbers are not
  a total delivery order; the [cross-phase rule](../design/ROADMAP_POLICY.md#cross-phase-delivery)
  permits measured production subsets ahead of reservoir research. A reference is
  always qualified — "Renderer Phase 1", never a bare "Phase 1" — because
  every sibling repository has phases of its own.
- **The research track**
  ([design policy §27](../design/DESIGN_POLICY.md#27-advanced-research-track)) —
  kept separate from the core milestones. Its items enter Renderer Phase 7, 9
  and 10 as experimental integrators or passes. Foundation availability
  permits evaluation but does not schedule it. Follow the
  [strategy brief and stop rule](../design/POST_V060_STRATEGY.md#6-research-track-and-stop-rule). Neural features are optional components
  throughout ([roadmap policy §5](../design/ROADMAP_POLICY.md#5-neural-features)).

## Status at a glance

**This table is the single source of truth for phase status and release
targets.** Other documents link here instead of repeating either. Version
numbers follow the implementation
([roadmap policy §8](../design/ROADMAP_POLICY.md#8-milestones)): a delivery gets
a version here once its dependencies, bounded scope and exit evidence are
defined. Selected cross-phase work is labeled as a subset and never implies
completion of its parent phase.

| Phase | Status | Target |
| --- | --- | --- |
| Renderer Phase 0 — Foundation | ✅ scope and entry-point exit reviewed ([closure evidence](../reports/2026-10-10-foundation-closure.md)); [release gate passed](../reports/2026-10-10-release-gate.md) | [v0.1.0](../releases/v0.1.0.md) |
| Renderer Phase 1 — Reference path tracer | ✅ exit criteria met ([reference images](../reports/2026-10-05-reference-images.md), [Hydra determinism](../reports/2026-10-05-hydra-deterministic-mode.md), [GPU timestamps](../reports/2026-10-05-gpu-timestamps.md)) | [v0.1.0](../releases/v0.1.0.md) |
| Renderer Phase 1.5 — Minimal material IR | ✅ minimal scope implemented (including dielectric GGX and the specular workflow, [evidence](../reports/2026-10-08-dielectric-specular.md); limits in the [scene reference](../reference/SCENE.md#materials-and-environment)) | [v0.1.0](../releases/v0.1.0.md) |
| Renderer Phase 2 — Wavefront path tracing | ✅ exit criteria met ([exit review](../reports/2026-10-11-hydra-integrator-selection.md#renderer-phase-2-exit)): reference equivalence reached ([evidence](../reports/2026-10-10-wavefront-equivalence.md)); queue bookkeeping and round scheduling [measured](../reports/2026-10-10-wavefront-scheduling.md), path state [stored by section](../reports/2026-10-10-wavefront-path-state.md), generate and accumulate kernels [replaced](../reports/2026-10-11-wavefront-camera-resolve.md); path classification deferred to Renderer Phase 8 ([upper bound](../reports/2026-10-11-wavefront-camera-resolve.md#path-classification)); integrator [selected through Hydra](../reports/2026-10-11-hydra-integrator-selection.md) | [v0.2.0](../releases/v0.2.0.md) |
| Renderer Phase 3 — Direct lighting / NEE / MIS | ✅ exit criteria met ([exit review](../reports/2026-10-12-renderer-phase3-exit.md#renderer-phase-3-exit)): next event estimation of emissive triangles with MIS in both integrators and through Hydra ([evidence](../reports/2026-10-11-emissive-nee-mis.md)); UsdLux sphere, rect, disk and distant lights sampled beside them ([evidence](../reports/2026-10-11-analytic-lights.md)); dome textures as an environment image sampled in proportion to its radiance ([evidence](../reports/2026-10-11-environment-importance-sampling.md)); time to equal quality 2.6–3.0× shorter in the Cornell box and by three orders of magnitude or more in the lit workloads under both integrators; light acceleration assigned to Renderer Phase 9 (LOTUS-LIGHT-05) | [v0.3.0](../releases/v0.3.0.md) |
| Renderer Phase 4 — Temporal infrastructure | ✅ exit criteria met ([scope review](../reports/2026-10-13-temporal-hydra-exit.md#renderer-phase-4-scope-review), [release gate](../reports/2026-10-13-v0.4.0-release-gate.md)): CPU/GPU frame history, rigid motion, reprojection, shared validity, bounded radiance and reset policy; Hydra controls/AOVs and moving installed-host sequences verified under both integrators | [v0.4.0](../releases/v0.4.0.md) |
| Renderer Phase 5 — ReSTIR DI | ✅ exit criteria met ([exit review](../reports/2026-10-17-renderer-phase5-exit.md)): initial, temporal and spatial reservoirs, proposals and Hydra controls/AOVs ([contract](../reference/RESTIR.md)); animation/reset contract kept ([animation](../reports/2026-10-17-restir-animation.md), [reset contract](../reports/2026-10-17-restir-reset-contract.md)); no reuse bias, ghosting or localized error detected in the bounded fixtures; [reuse cost](../reports/2026-10-17-restir-reuse-cost.md) at locked clocks: NEE/MIS stays the default, initial-only reservoirs with eight candidates about 1.4× faster to equal quality only with many distant lights, temporal, spatial and combined reuse not worth using; the [stop rule](../design/ROADMAP_POLICY.md#renderer-phase-5--restir-di) applies | [v0.5.0 experimental snapshot](../releases/v0.5.0.md); closure [v0.5.1](../releases/v0.5.1.md) |
| Renderer Phase 6 — Denoising / SVGF-class pipeline | ✅ exit criteria met ([exit review](../reports/2026-10-18-svgf-viewport.md#renderer-phase-6-exit)): temporal accumulation, moments and variance ([evidence](../reports/2026-10-18-svgf-moments-variance.md)); edge-aware filtering and reference comparison under both integrators ([evidence](../reports/2026-10-18-svgf-filter.md)); run-time switching, raw/filtered comparison, history and variance views and denoise timing in the viewport ([evidence](../reports/2026-10-18-svgf-viewport.md)); [release gate passed](../reports/2026-10-18-v0.6.0-release-gate.md) | [v0.6.0](../releases/v0.6.0.md) |
| Renderer Phase 7 — ReSTIR GI / advanced reservoir transport | ⬜ | unscheduled |
| Renderer Phase 8 — Production material support | ⬜ | full phase unscheduled; selected foundation subset in the practical-foundation delivery below |
| Renderer Phase 9 — Advanced sampling and scheduling | ⬜ | full phase unscheduled; selected profiling subset in the practical-foundation delivery below |
| Renderer Phase 10 — Spectral rendering research | ⬜ | unscheduled |
| Practical foundation — cross-phase delivery | ⬜ scope adopted; implementation not started | [v0.7.0 — Performance & Production Foundation](#v070--performance--production-foundation) |

## v0.7.0 — Performance & Production Foundation

Adopt the [development strategy](../design/POST_V060_STRATEGY.md) for the next
delivery: correct practical scene rendering, GPU efficiency and everyday use.
This is a delivery milestone across existing paths and selected later-phase
subsets, with no whole-phase exit implied. No release date is fixed.

| Workstream | Delivery scope | Contract and exit evidence |
| --- | --- | --- |
| A (P0) — GPU observation and selected optimization | Shared headless/viewport baseline, major cost attribution and measured fixes to existing paths. | [Practical foundation workstreams](../design/ROADMAP_POLICY.md#practical-foundation-workstreams), A. |
| B (P1) — VRAM / temporal buffers | Ownership, lifetimes, peak residency and justified representation/reuse comparisons. LOTUS-MEMORY-01 [inventoried frame-state residency](../reports/2026-10-26-frame-residency.md#candidates-and-decisions) at 1080p/4K and adopted [lazy readback mirrors](../reports/2026-10-26-lazy-readback-mirrors.md#decision) (6.83 GiB less host memory at 4K with SVGF and reuse) and a [reservoir history split](../reports/2026-10-26-reservoir-history-split.md#decision) (1.0 GiB less device memory at 4K with spatial reuse); value compaction deferred. | Same policy, B. |
| C (P1) — Material foundation | Selected Renderer Phase 8 fixture, coverage and correctness work on real USD assets. | Same policy, C; full production materials remain outside this subset. |
| D (P1) — Light profiling | Selected Renderer Phase 9 investigation separating intersection and selection; acceleration implementation is conditional. LOTUS-LIGHT-05 [attributed the many-light cost to crossing tests](../reports/2026-10-25-light-crossing-cost.md#decisions) (go) and deferred selection; LOTUS-LIGHT-06 [adopted a crossing hierarchy](../reports/2026-10-25-light-crossing-hierarchy.md#decision) (256-light frame 5.3–8.0× faster). | Same policy, D; a measured deferral is a valid investigation outcome. |

The [active queue](current.md#execution-order) contains only the first bounded
tasks. Establish the benchmark, select bottleneck fixes, add representative
material fixtures and compare memory optimizations. An unmeasured speedup,
ReSTIR GI, complete MaterialX support and a UI redesign are not exit gates.
Reference-quality and dynamic-history regressions remain disqualifying.

## Later directions and entry conditions

These are proposals, not scheduled release targets. Keep the immediate next
milestone's entry conditions clear; do not turn the rest into an active
checklist.

| Candidate | Direction | Entry condition |
| --- | --- | --- |
| v0.8.0 — Materials & Lighting | Prioritized OpenPBR/MaterialX subsets and measured lighting improvements. | Foundation fixtures demonstrate actual asset gaps; lighting comparisons include GPU time, quality, memory and low-light-count regressions. |
| v0.9.0 — Interactive Quality | Disocclusion, ghosting, reset contracts and Interactive/Balanced/Reference presets of the same core. | Separate static convergence from motion; define preset behavior and invalidation by change category before implementation. |
| v0.10.0 or later — Advanced Transport (tentative) | Bounded ReSTIR GI / path-guiding evaluation. | An experiment brief meets the [research rules](../design/POST_V060_STRATEGY.md#6-research-track-and-stop-rule); no automatic adoption or default change. |
| Optional future modes | Neural denoising and spectral experiments. | Optional dependencies and a stable ordinary RGB path; costs and contracts established before expansion. |

Only the [canonical table](#status-at-a-glance) assigns targets. Later
candidate numbers do not assign releases to the corresponding renderer
phases, and research does not gate the practical milestones.

## v0.6.0 — Interactive rendering

Historical delivery context is preserved by the [release record](../releases/v0.6.0.md)
and [release gate](../reports/2026-10-18-v0.6.0-release-gate.md), including
links to the earlier [ReSTIR DI closure](../releases/v0.5.1.md) and
[viewport foundation](../releases/v0.5.2.md). Dated reports retain their
historical plan citations through the [archive index](../archive/README.md).
The forward execution order is owned by [current.md](current.md).

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
