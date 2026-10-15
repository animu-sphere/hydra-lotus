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

Continue with direct-light reservoir reuse under the
[ReSTIR DI scope](../design/ROADMAP_POLICY.md#renderer-phase-5--restir-di).
The CPU arithmetic and same-target combination are checked against independent
discrete integrals ([evidence](../reports/2026-10-13-restir-reservoir.md));
GPU initial-only lighting is checked against independent integrals and NEE/MIS
images under both integrators ([evidence](../reports/2026-10-13-restir-initial.md)).
The [reservoir reference](../reference/RESTIR.md) owns behavior and limits.
Temporal GPU reuse now carries explicit corrected weights and exact source
shading, using shared reprojection and bounded history counts
([evidence](../reports/2026-10-13-restir-temporal.md)).
Spatial reuse reads immutable current reservoirs, rejects surface boundaries
and retains explicit support-corrected weights. CPU/GPU, visibility, lifecycle
and four-mode moving many-light comparisons are recorded in its
[evidence](../reports/2026-10-14-restir-spatial.md).
Implement the remaining work in this order:

- [ ] **LOTUS-RESTIR-05 — Hydra controls and exit evidence.**
  Hydra controls/diagnostics and moving USD-stage/installed usdview sequences
  under both integrators are recorded in the
  [adapter evidence](../reports/2026-10-14-restir-hydra.md), including
  deterministic progressive NEE/MIS restoration. A reproducible headless
  candidate-count sweep now measures static/moving many-light convergence,
  GPU time, temporal differences, memory and finite-sample local error against
  independent Lambert/NEE/MIS references
  ([comparison evidence](../reports/2026-10-14-restir-comparison.md)). Power,
  uniform and equal-mixture proposals are implemented and checked against
  CPU/GPU arithmetic, independent mixed-light integrals and matched many-light
  comparisons ([proposal evidence](../reports/2026-10-15-restir-proposals.md)).
  Three independent repetitions per integrator now measure observed
  equal-quality budgets at a longer horizon and higher light count
  ([budget evidence](../reports/2026-10-15-restir-budgets.md)).
  Mixed area/environment lighting with material/depth boundaries now has
  static/moving headless workloads, explicit finite NEE/MIS references and
  regional errors ([boundary evidence](../reports/2026-10-15-restir-boundaries.md)).
  Same-frame receiver visibility is reused for unchanged light-slot/UV
  selections, with independent moving fractional-occluder checks and matched
  before/after comparisons ([visibility evidence](../reports/2026-10-15-restir-visibility.md)).
  Remaining: broaden artifact/bias coverage beyond these bounded fixtures;
  address the reproduced
  reuse cost and temporal correlation before recording the scope review and
  release gate.

Denoising, spectral research, production material expansion and broad
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
