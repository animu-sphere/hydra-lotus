# Current

The only active task checklist. Phase and release status belongs in the
[canonical status table](README.md#status-at-a-glance); completed work is
represented by its [evidence](../reports/README.md) and
[change history](../../CHANGELOG.md), rather than retained here.

## Execution order

The temporal release snapshot and verification are recorded in the
[release record](../releases/v0.4.0.md) and
[release gate](../reports/2026-10-13-v0.4.0-release-gate.md).
Phase and release status belong to the
[canonical table](README.md#status-at-a-glance).

The next renderer cycle follows the [phase policy](../design/ROADMAP_POLICY.md#4-roadmap);
its implementation tasks have not yet been scheduled.
ReSTIR, denoising, spectral research, production material expansion and broad
platform/backend expansion retain their separate phase assignments.

## Performance baseline and foundation release

The [fixed baseline](../reports/2026-10-10-performance-baseline.md),
[foundation dispositions](../reports/2026-10-10-foundation-closure.md) and
[foundation release gate](../reports/2026-10-10-release-gate.md) remain the
reference for later transport changes. Shipped scope is retained in the
[release records](../releases/README.md).

## Deferred adapter work

These are assignments to later phases, rather than additional foundation
requirements:

- [ ] **LOTUS-MATERIAL-01 — Renderer Phase 8 production scene coverage.**
  Evaluate remaining material, texture and shading-normal coverage within
  the [production material scope](../design/ROADMAP_POLICY.md#renderer-phase-8--production-material-support),
  using the [material reference](../reference/SCENE.md#materials-and-environment)
  and [Hydra extraction reference](../reference/SCENE.md#hydra-extraction).
  Once materials differ in how they shade, measure wavefront path
  classification (hits queued per shading class) against its
  [upper bound on the fixed workloads](../reports/2026-10-11-wavefront-camera-resolve.md#path-classification),
  1–2% of the shade kernel where every material is of one kind.
- [ ] **LOTUS-LIGHT-05 — Renderer Phase 9 light acceleration.** Every ray
  tests every analytic light, and a frame's cost grows linearly with their
  number ([baseline](../reports/2026-10-12-renderer-phase3-exit.md#analytic-lights-along-each-ray)).
  Evaluate a hierarchy for the crossing test, or the lights as procedural
  primitives in the ray-tracing acceleration structure, with a light tree
  for selection, within the
  [advanced sampling candidates](../design/ROADMAP_POLICY.md#renderer-phase-9--advanced-sampling-and-scheduling).
