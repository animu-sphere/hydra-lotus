# Current

The only active task checklist. Phase and release status belongs in the
[canonical status table](README.md#status-at-a-glance); completed work is
represented by its [evidence](../reports/README.md) and
[change history](../../CHANGELOG.md), rather than retained here.

## Execution order

Foundation release →
Renderer Phase 2 equivalence → Wavefront optimization.

The [fixed performance baseline](../reports/2026-10-10-performance-baseline.md)
records four repeatable workloads and metric availability. The
[foundation review](../reports/2026-10-10-foundation-closure.md) records
closure dispositions and hosted capability evidence; the
[memory-pool report](../reports/2026-10-10-gpu-memory-pools.md) records backend
suballocation and reuse. These do not substitute for the baseline/release
gates below.

Backend update classification and the compaction disposition are measured in
the [BLAS refit/compaction report](../reports/2026-10-10-blas-refit-compaction.md).

The release target is owned by the [status table](README.md#status-at-a-glance).
The stabilization cycle excludes ReSTIR, denoising, spectral research,
production material coverage and broad platform/backend expansion. Their
scope remains in the [phase policy](../design/ROADMAP_POLICY.md#4-roadmap).

## Performance baseline and foundation release

- [ ] **LOTUS-RELEASE-01 — Foundation release gate.** Preserve the
  [foundation dispositions](../reports/2026-10-10-foundation-closure.md), verify
  synchronization behavior and documentation CI, pass deterministic references
  and Hydra discovery/smoke tests, and
  link the [baseline report](../reports/2026-10-10-performance-baseline.md).
  Keep known limitations in reference documentation;
  release records describe the shipped snapshot. The release should provide
  a deterministic Vulkan reference renderer for OpenUSD Hydra with minimal
  materials/textures, stable extraction and measured backend behavior.

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
