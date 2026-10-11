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
| [current.md](current.md) | Deferred adapter work; Renderer Phase 2 is complete and the next phase is not yet scheduled. |

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
a version here when its predecessor is done.

| Phase | Status | Target |
| --- | --- | --- |
| Renderer Phase 0 — Foundation | ✅ scope and entry-point exit reviewed ([closure evidence](../reports/2026-10-10-foundation-closure.md)); [release gate passed](../reports/2026-10-10-release-gate.md) | [v0.1.0](../releases/v0.1.0.md) |
| Renderer Phase 1 — Reference path tracer | ✅ exit criteria met ([reference images](../reports/2026-10-05-reference-images.md), [Hydra determinism](../reports/2026-10-05-hydra-deterministic-mode.md), [GPU timestamps](../reports/2026-10-05-gpu-timestamps.md)) | [v0.1.0](../releases/v0.1.0.md) |
| Renderer Phase 1.5 — Minimal material IR | ✅ minimal scope implemented (including dielectric GGX and the specular workflow, [evidence](../reports/2026-10-08-dielectric-specular.md); limits in the [scene reference](../reference/SCENE.md#materials-and-environment)) | [v0.1.0](../releases/v0.1.0.md) |
| Renderer Phase 2 — Wavefront path tracing | ✅ exit criteria met ([exit review](../reports/2026-10-11-hydra-integrator-selection.md#renderer-phase-2-exit)): reference equivalence reached ([evidence](../reports/2026-10-10-wavefront-equivalence.md)); queue bookkeeping and round scheduling [measured](../reports/2026-10-10-wavefront-scheduling.md), path state [stored by section](../reports/2026-10-10-wavefront-path-state.md), generate and accumulate kernels [replaced](../reports/2026-10-11-wavefront-camera-resolve.md); path classification deferred to Renderer Phase 8 ([upper bound](../reports/2026-10-11-wavefront-camera-resolve.md#path-classification)); integrator [selected through Hydra](../reports/2026-10-11-hydra-integrator-selection.md) | unscheduled |
| Renderer Phase 3 — Direct lighting / NEE / MIS | ⬜ | unscheduled |
| Renderer Phase 4 — Temporal infrastructure | ⬜ | unscheduled |
| Renderer Phase 5 — ReSTIR DI | ⬜ | unscheduled |
| Renderer Phase 6 — Denoising / SVGF-class pipeline | ⬜ | unscheduled |
| Renderer Phase 7 — ReSTIR GI / advanced reservoir transport | ⬜ | unscheduled |
| Renderer Phase 8 — Production material support | ⬜ | unscheduled |
| Renderer Phase 9 — Advanced sampling and scheduling | ⬜ | unscheduled |
| Renderer Phase 10 — Spectral rendering research | ⬜ | unscheduled |

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
