# Current

The only active task checklist. Phase and release status belongs in the
[canonical status table](README.md#status-at-a-glance); completed work is
represented by its [evidence](../reports/README.md) and
[change history](../../CHANGELOG.md), rather than retained here.

## Execution order

Renderer Phase 2 is complete
([exit review](../reports/2026-10-11-hydra-integrator-selection.md#renderer-phase-2-exit));
the next phase's tasks are not yet scheduled.

The wavefront integrator reproduces the reference transport
([equivalence evidence](../reports/2026-10-10-wavefront-equivalence.md));
the [scheduling](../reports/2026-10-10-wavefront-scheduling.md),
[path-state](../reports/2026-10-10-wavefront-path-state.md) and
[camera and resolve](../reports/2026-10-11-wavefront-camera-resolve.md)
reports record its cost against the reference integrator after queue
bookkeeping, round scheduling, per-path state traffic and the generate
and accumulate kernels. Hydra selects either integrator
([integrator selection](../reports/2026-10-11-hydra-integrator-selection.md)).
Path classification is assigned to Renderer Phase 8 (LOTUS-MATERIAL-01).

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
The Wavefront cycle excludes ReSTIR, denoising, spectral research,
production material coverage and broad platform/backend expansion. Their
scope remains in the [phase policy](../design/ROADMAP_POLICY.md#4-roadmap).

## Performance baseline and foundation release

The [fixed baseline](../reports/2026-10-10-performance-baseline.md),
[scope dispositions](../reports/2026-10-10-foundation-closure.md) and
[release-gate evidence](../reports/2026-10-10-release-gate.md) are retained
as the reference for subsequent transport changes. Shipped scope belongs
in the [release records](../releases/README.md); known limitations remain
in the reference documentation.

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
  Once materials differ in how they shade, measure wavefront path
  classification (hits queued per shading class) against its
  [upper bound on the fixed workloads](../reports/2026-10-11-wavefront-camera-resolve.md#path-classification),
  1–2% of the shade kernel where every material is of one kind.
