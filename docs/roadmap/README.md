# Roadmap

The roadmap holds only **incomplete** work owned by this repository. Shipped
work is in the [CHANGELOG](../../CHANGELOG.md) and the
[release records](../releases/); rationale lives in [design/](../design/).
Sibling repositories' work is planned in their own roadmaps and not mirrored
here.

Legend: ✅ done · 🚧 in progress · ⬜ not started · ⛔ blocked · ⚠️ accepted workaround

| Document | Contents |
| --- | --- |
| [current.md](current.md) | Renderer Phase 0's open items, what Renderer Phase 1 needs first, and project infrastructure. |

## Sequences

Two sequences are live, both defined in the design policy:

- **Renderer Phase 0–10**
  ([design policy §36–§46](../design/DESIGN_POLICY.md#36-renderer-phase-0--bootstrap)) —
  the core milestones. A reference is always qualified — "Renderer Phase 1",
  never a bare "Phase 1" — because every sibling repository has phases of
  its own.
- **The research track**
  ([§27](../design/DESIGN_POLICY.md#27-advanced-research-track)) — kept
  separate from the core milestones. Its items enter Renderer Phase 9 and 10
  as experimental integrators or passes and are not scheduled until the
  foundation they build on exists.

## Status at a glance

**This table is the single source of truth for which release a phase lands
in.** Other documents name a phase and defer the version here. The design
policy's version sketch ([§47](../design/DESIGN_POLICY.md#47-versioning-guide))
is indicative; a phase gets a version here when its predecessor is done, and
Renderer Phase 1–2 and 5–6 wait on
[DES-Q1 and DES-Q2](../design/DESIGN_POLICY.md#53-open-questions).

| Phase | Status | Target |
| --- | --- | --- |
| Renderer Phase 0 — Bootstrap | 🚧 in progress (scaffold generated 2026-10-04) | v0.1.0 |
| Renderer Phase 1 — Baseline path tracer | ⬜ | unscheduled |
| Renderer Phase 2 — Wavefront | ⬜ | unscheduled |
| Renderer Phase 3 — NEE / MIS (reference path tracer) | ⬜ | unscheduled |
| Renderer Phase 4 — Temporal infrastructure | ⬜ | unscheduled |
| Renderer Phase 5 — ReSTIR DI | ⬜ | unscheduled |
| Renderer Phase 6 — SVGF / denoising | ⬜ | unscheduled |
| Renderer Phase 7 — ReSTIR GI / PT | ⬜ | unscheduled |
| Renderer Phase 8 — Material and production scene support | ⬜ | unscheduled |
| Renderer Phase 9 — Advanced sampling research | ⬜ | unscheduled |
| Renderer Phase 10 — Quality / spectral research | ⬜ | unscheduled |

## Quality bar (applies to every phase)

- A faster mode is checked against the reference path tracer, on a
  deterministic run, before it is called correct
  ([design policy §10, §26](../design/DESIGN_POLICY.md#10-nee--mis)).
- Public core headers stay free of OpenUSD, Hydra, Vulkan and windowing types,
  and CI enforces it ([PROJECT_LAYOUT.md §4](../architecture/PROJECT_LAYOUT.md#4-dependency-directions)).
- An unchanged scene costs almost no CPU time; no pipeline is created inside
  the render loop ([§23](../design/DESIGN_POLICY.md#23-cpu-performance)).
- A new pass carries its GPU timestamp and its debug AOV from the change that
  adds it ([§24–§25](../design/DESIGN_POLICY.md#24-gpu-profiling)).
- A research feature never breaks the RGB interactive path
  ([§51](../design/DESIGN_POLICY.md#51-decision-principles)).
- A performance or quality claim cites a measurement, and a measurement is a
  [report](../reports/).
- Every documented command is one that has actually been run.
