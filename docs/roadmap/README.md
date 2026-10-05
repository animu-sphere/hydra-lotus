# Roadmap

The roadmap holds only **incomplete** work owned by this repository. Shipped
work is in the [CHANGELOG](../../CHANGELOG.md) and the
[release records](../releases/); rationale lives in [design/](../design/).
Sibling repositories' work is planned in their own roadmaps and not mirrored
here.

Legend: ✅ done · 🚧 in progress · ⬜ not started · ⛔ blocked · ⚠️ accepted workaround

| Document | Contents |
| --- | --- |
| [current.md](current.md) | Renderer Phase 0's open items, the Renderer Phase 1 vertical slice, and project infrastructure. |

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

**This table is the single source of truth for which release a phase lands
in.** Other documents name a phase and defer the version here. Version
numbers follow the implementation
([roadmap policy §8](../design/ROADMAP_POLICY.md#8-milestones)): a phase gets
a version here when its predecessor is done.

| Phase | Status | Target |
| --- | --- | --- |
| Renderer Phase 0 — Foundation | 🚧 in progress (scaffold generated 2026-10-04) | v0.1.0 |
| Renderer Phase 1 — Reference path tracer | ✅ exit criteria met ([reference images](../reports/2026-10-05-reference-images.md)), with deterministic mode through Hydra and per-pass GPU timestamps alongside ([current](current.md#renderer-phase-1--reference-path-tracer)) | unscheduled |
| Renderer Phase 1.5 — Minimal material IR | ⬜ | unscheduled |
| Renderer Phase 2 — Wavefront path tracing | ⬜ | unscheduled |
| Renderer Phase 3 — Direct lighting / NEE / MIS | ⬜ | unscheduled |
| Renderer Phase 4 — Temporal infrastructure | ⬜ | unscheduled |
| Renderer Phase 5 — ReSTIR DI | ⬜ | unscheduled |
| Renderer Phase 6 — Denoising / SVGF-class pipeline | ⬜ | unscheduled |
| Renderer Phase 7 — ReSTIR GI / advanced reservoir transport | ⬜ | unscheduled |
| Renderer Phase 8 — Production material support | ⬜ | unscheduled |
| Renderer Phase 9 — Advanced sampling and scheduling | ⬜ | unscheduled |
| Renderer Phase 10 — Spectral rendering research | ⬜ | unscheduled |

## Milestones

The [roadmap policy's milestones](../design/ROADMAP_POLICY.md#8-milestones).
A milestone is reached when its correctness evidence is a
[report](../reports/), not when a release ships.

| Milestone | Reached in | Status |
| --- | --- | --- |
| Foundation | Renderer Phase 0 | 🚧 |
| First ray-traced triangle | Renderer Phase 1 | ✅ [measured ray queries](../reports/2026-10-05-primary-rays.md) |
| First physically correct image | Renderer Phase 1 | ✅ [measured accumulation](../reports/2026-10-05-hdr-accumulation.md) |
| Reference path tracer | Renderer Phase 1 | ✅ [measured reference images](../reports/2026-10-05-reference-images.md) |
| Wavefront path tracer | Renderer Phase 2 | ⬜ |
| NEE / MIS renderer | Renderer Phase 3 | ⬜ |
| Temporal renderer | Renderer Phase 4 | ⬜ |
| ReSTIR DI renderer | Renderer Phase 5 | ⬜ |
| Interactive denoised renderer | Renderer Phase 6 | ⬜ |
| Production material renderer | Renderer Phase 8 | ⬜ |
| Advanced / spectral research renderer | Renderer Phase 9–10 | ⬜ |

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
