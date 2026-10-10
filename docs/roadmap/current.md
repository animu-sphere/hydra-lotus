# Current

The only active task checklist. Phase and release status belongs in the
[canonical status table](README.md#status-at-a-glance); completed work is
represented by its [evidence](../reports/README.md) and
[change history](../../CHANGELOG.md), rather than retained here.

## Execution order

Renderer Phase 2 equivalence → Wavefront optimization.

The [fixed performance baseline](../reports/2026-10-10-performance-baseline.md)
records four repeatable workloads and metric availability. The
[foundation review](../reports/2026-10-10-foundation-closure.md) records
closure dispositions and hosted capability evidence; the
[memory-pool report](../reports/2026-10-10-gpu-memory-pools.md) records backend
suballocation and reuse. The
[release-gate evidence](../reports/2026-10-10-release-gate.md) records the
foundation verification that precedes the Wavefront transition.

Backend update classification and the compaction disposition are measured in
the [BLAS refit/compaction report](../reports/2026-10-10-blas-refit-compaction.md).

The release target is owned by the [status table](README.md#status-at-a-glance).
The Wavefront equivalence cycle excludes ReSTIR, denoising, spectral research,
production material coverage and broad platform/backend expansion. Their
scope remains in the [phase policy](../design/ROADMAP_POLICY.md#4-roadmap).

## Performance baseline and foundation release

The [fixed baseline](../reports/2026-10-10-performance-baseline.md),
[scope dispositions](../reports/2026-10-10-foundation-closure.md) and
[release-gate evidence](../reports/2026-10-10-release-gate.md) are retained
as the reference for subsequent transport changes. Shipped scope belongs
in the [release records](../releases/README.md); known limitations remain
in the reference documentation.

## Renderer Phase 2 — Wavefront path tracing

- [ ] **LOTUS-WAVE-01 — Reference equivalence first.** After the foundation
  release, implement the initial queues and transport defined by the
  [phase contract](../design/ROADMAP_POLICY.md#renderer-phase-2--wavefront-path-tracing).
  Record deterministic equivalence evidence using the
  [reference correctness tolerance](../reference/SCENE.md#reference-images)
  before beginning queue/scheduling optimization.

## Deferred adapter work

These are assignments to later phases, rather than additional foundation
requirements:

- [ ] **LOTUS-LIGHT-01 — Renderer Phase 3 light representation and sampling.**
  Extend the light representation alongside explicit direct-light sampling.
  Implementation limits are in the [light reference](../reference/SCENE.md#lights).
- [ ] **LOTUS-MATERIAL-01 — Renderer Phase 8 production scene coverage.**
  Evaluate remaining material, texture and shading-normal coverage within
  the [production material scope](../design/ROADMAP_POLICY.md#renderer-phase-8--production-material-support),
  using the [material reference](../reference/SCENE.md#materials-and-environment)
  and [Hydra extraction reference](../reference/SCENE.md#hydra-extraction).
