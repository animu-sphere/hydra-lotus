# Current

The only active task checklist. Phase and release status belongs in the
[canonical status table](README.md#status-at-a-glance); completed work is
represented by its [evidence](../reports/README.md) and
[change history](../../CHANGELOG.md), rather than retained here.

## Execution order

The experimental ReSTIR snapshot and release verification are recorded in
[the release record](../releases/v0.5.0.md) and
[release gate](../reports/2026-10-16-v0.5.0-release-gate.md).
Phase and release status belong to the
[canonical table](README.md#status-at-a-glance).
The [reservoir reference](../reference/RESTIR.md) owns current behavior and
limits; opt-in availability does not close the remaining investigations.

- [ ] **LOTUS-RESTIR-05 — Complete ReSTIR DI exit evidence.**
  Broaden bias/artifact and convergence assessment beyond the bounded
  one-bounce glossy fixtures, including broader glossy and multibounce
  scenes. Investigate the static temporal-reuse convergence penalty in the
  [rough dielectric/sphere-light fixture](../reports/2026-10-16-restir-glossy-spheres.md),
  distinguish persistent error from finite-reference and rare-event variance,
  and assess temporal correlation and ghosting. Address reuse cost and
  demonstrate any equal-quality speedup with repeated measurements against
  NEE/MIS under both integrators before recording a phase-exit scope review.
  Existing [quality budgets](../reports/2026-10-15-restir-budgets.md),
  [boundary measurements](../reports/2026-10-15-restir-boundaries.md),
  [residual correlations](../reports/2026-10-16-restir-correlation.md) and
  [complementary BSDF comparisons](../reports/2026-10-16-restir-bsdf-partition.md)
  are the starting evidence, including their adverse results and limits.
  The [four-bounce/reference-sensitivity measurement](../reports/2026-10-16-restir-convergence.md)
  reproduces the static reuse penalty with independent finite-reference groups.
  The [sequence-ensemble decomposition](../reports/2026-10-16-restir-sequence-ensemble.md)
  attributes it to temporal correlation, not detectable bias, in that static
  fixture; the fixture's interreflection is too weak to count as multibounce
  coverage. [Replication bias tests](../reports/2026-10-17-restir-moving-bias.md)
  find no moving-scene bias, ghosting or localized (band/tile) error
  distinguishable from non-reusing estimators in the four bounded moving
  suites; narrow glossy highlights are limited by rare-event variance.
  A [closed interreflecting room](../reports/2026-10-17-restir-interreflection.md),
  about half indirect light at four bounces, shows the same, with weaker
  temporal correlation. Under [light/material animation](../reports/2026-10-17-restir-animation.md)
  history resets exactly at each edit with no reset-related ghosting, but each
  reset replays random numbers, correlating reuse noise across edits. Reuse
  cost remains.
  The experimental release gate validates the shipped snapshot; it does not
  substitute for these remaining transport investigations.

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
