# Current

The only active task checklist. Phase and release status belongs in the
[canonical status table](README.md#status-at-a-glance); completed work is
represented by its [evidence](../reports/README.md) and
[change history](../../CHANGELOG.md), rather than retained here.

## Execution order

Renderer Phase 3's [exit review](../reports/2026-10-12-renderer-phase3-exit.md#renderer-phase-3-exit)
provides the NEE / MIS baseline for the temporal cycle. The release target
and phase status belong to the [canonical table](README.md#status-at-a-glance).

LOTUS-TEMPORAL-01 provides CPU frame history, rigid-placement correspondence,
full-frame backward reprojection and shared validity
([evidence](../reports/2026-10-12-temporal-foundation.md),
[contract and limits](../reference/TEMPORAL.md)). The next slices connect
that metadata to the GPU. Work proceeds in this order:

- [ ] **LOTUS-TEMPORAL-02 — Primary-surface temporal AOVs and GPU history.**
  Retain previous camera/placement transforms for successfully rendered
  frames, with stable instance/material identities independent of GPU table
  slots. Add unjittered primary depth, normal, identity and backward motion
  products to both integrators. Cover display/data windows and resizing;
  check stationary, camera-motion and rigid-object-motion sequences against
  independent projections, including instancer churn. Preserve progressive
  reference output. Carry GPU timestamps and debug AOVs with each new pass.
- [ ] **LOTUS-TEMPORAL-03 — GPU reprojection and shared validity.**
  Fetch the preceding frame's surface metadata and implement the shared
  rejection contract on the GPU, checked against the CPU oracle. Exercise
  disocclusion, depth/normal discontinuity, changed material/texture/instance,
  missing history, large motion and camera cuts. Expose rejection reasons
  and history validity as debug AOVs, with per-pass GPU timestamps. Keep
  all validity code independent of ReSTIR and denoiser state.
- [ ] **LOTUS-TEMPORAL-04 — Opt-in temporal radiance accumulation.**
  Accumulate reprojected radiance and per-pixel sample counts with a bounded
  history. Define reset handling for lighting, render settings, scene edits,
  target/framing changes and explicit cuts, preserving history on supported
  camera/rigid motion. Compare static convergence with progressive reference
  transport and moving sequences with independently rendered references;
  report rejection behavior, ghosting, GPU time and history memory.
- [ ] **LOTUS-TEMPORAL-05 — Hydra integration and temporal exit review.**
  Expose temporal selection and reset controls through Hydra, add hosted
  moving-scene validation and motion/history AOV coverage, and keep the
  deterministic progressive mode selectable. Review every temporal scope
  item and its evidence, then run core, runtime-free and Hydra release checks
  before preparing the next release record.

The exit review must demonstrate frame history, previous transforms, motion,
reprojection, disocclusion and shared validity, temporal accumulation and a
measured reset policy on static, camera-motion and object-motion sequences.
The temporal cycle excludes ReSTIR, denoising, spectral research, production
material expansion and broad platform/backend expansion; their scope remains
in the [phase policy](../design/ROADMAP_POLICY.md#4-roadmap).

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
