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

Continue with direct-light reservoirs under the
[ReSTIR DI scope](../design/ROADMAP_POLICY.md#renderer-phase-5--restir-di).
The CPU arithmetic and same-target combination are checked against independent
discrete integrals ([evidence](../reports/2026-10-13-restir-reservoir.md));
the [reservoir reference](../reference/RESTIR.md) owns their limits.
Implement the remaining work in this order:

- [ ] **LOTUS-RESTIR-02 — GPU initial candidates and direct-light resolve.**
  Define reconstructible light samples and their measures for emissive,
  analytic and environment lights. Port reservoir updates to Slang, validate
  them against the CPU oracle, and add opt-in primary-surface direct lighting
  under both integrators. Apply visibility to the selected contribution and
  account for BSDF-found emission without double counting. Preserve the
  NEE/MIS mode; carry a separate timestamp, reservoir debug products and
  memory accounting. Compare initial-only estimates with independent lighting
  oracles and converged NEE/MIS images, including zero-target/occluded samples.
- [ ] **LOTUS-RESTIR-03 — Temporal reservoir reuse.**
  Use shared reprojection/validity without putting reservoir state into the
  temporal infrastructure. Reevaluate the previous selected sample at the
  receiving surface; validate support-aware normalization, candidate-count
  limits, light/sample identity, resets and failed-frame publication. Compare
  static and moving sequences, disocclusion and lighting edits with NEE/MIS.
- [ ] **LOTUS-RESTIR-04 — Spatial reservoir reuse.**
  Read immutable source reservoirs into separate destination storage.
  Reevaluate samples and normalize across differing surface support;
  test depth/normal/material boundaries, visibility changes and correlations.
  Measure bias/artifacts and temporal stability against temporal-only and
  initial-only modes, with each reuse pass timed and inspectable.
- [ ] **LOTUS-RESTIR-05 — Hydra controls and exit evidence.**
  Expose estimator/reuse controls and reservoir diagnostics through Hydra,
  retaining deterministic progressive NEE/MIS selection. Validate installed
  moving USD-stage/usdview sequences under both integrators. Measure many-light
  convergence, GPU time, temporal stability, memory and bias/artifacts against
  the NEE/MIS baseline; evaluate candidate strategies and record the scope
  review and release gate.

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
