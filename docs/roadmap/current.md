# Current

The only active task checklist. Phase/release status and targets belong in
[the canonical table](README.md#status-at-a-glance); priorities are defined
by the [development strategy](../design/POST_V060_STRATEGY.md).

## Execution order

Start with the [practical foundation milestone](README.md#v070--performance--production-foundation).
Measure first, select bottleneck fixes, establish material fixtures, then
compare memory optimizations. The contracts and exit evidence are the
[workstream policy's](../design/ROADMAP_POLICY.md#practical-foundation-workstreams).
Only the following bounded tasks are active; later work enters this queue
when the measurements justify it.

- [ ] **LOTUS-PERF-01 — Shared workload manifest and GPU baseline (P0).**
  Workstream A, improving existing renderer paths: define common settings
  for headless and viewport runs across Cornell box, many instances, many
  analytic lights, textures and camera/object motion. Reuse the
  [fixed baseline](../reports/2026-10-10-performance-baseline.md) and
  [viewport timing infrastructure](../reports/2026-10-18-viewport-timing.md).
  Record pass timings and environment, image error, time to equal quality,
  peak VRAM and CPU update cost; name unavailable metrics. Report the top
  three bottlenecks and an adopt/defer decision for each before opening
  optimization tasks. Preserve deterministic Reference PT comparisons.
  The [initial static manifest and measurements](../reports/2026-10-21-shared-performance-workloads.md)
  provide shared fixtures, warmup exclusion and deterministic equivalence.
  [Optional whole-device telemetry](../reports/2026-10-24-gpu-telemetry.md)
  now retains clock/power-state ranges and sampled device-memory use;
  those observations do not establish locked clocks or process peak residency.
  [Cornell quality checkpoints](../reports/2026-10-25-quality-curve.md) now
  compare independent measured samples with the committed finite reference
  and retain observed error-threshold rendering times for both integrators.
  [Disjoint-sequence quality populations](../reports/2026-10-25-quality-populations.md)
  now separate three measured sample ranges from timing repetitions and
  retain deterministic agreement between integrators.
  [Textured quality checkpoints](../reports/2026-10-25-textured-quality.md)
  extend this to a second static fixture with validated finite Reference PT
  evidence, a split-reference discrepancy and deterministic comparisons.
  [Headless camera/object motion](../reports/2026-10-25-motion-workloads.md)
  now measures per-frame authoring, extraction, scene-update and refit costs.
  [Changed-section staging](../reports/2026-10-25-changed-section-staging.md)
  was adopted: deformation no longer restages unchanged triangle indices
  (67% fewer staged bytes and about 20% less update wall on the large mesh).
  [Frame host phases](../reports/2026-10-25-frame-host-split.md) explain the
  blocking frame overhead; host-cached readback was adopted, removing about
  1.1 ms of readback from each 128x128 frame with identical images.
  [Many-light crossing](../reports/2026-10-25-light-crossing-cost.md)
  explained the many-light continuation cost, and a
  [crossing hierarchy](../reports/2026-10-25-light-crossing-hierarchy.md)
  was adopted: a 256-light frame is 5.3–8.0× faster with images equal to
  rounding.
  Remaining: controlled timing, Hydra/viewport motion, temporal/SVGF modes,
  quality curves for the other fixtures, peak residency and complete viewport
  repeats; preserve the recorded driver failure when extending the baseline.
- [ ] **LOTUS-MEMORY-01 — Temporal/SVGF ownership and peak residency (P1).**
  Workstream B, improving existing temporal/denoise paths: inventory buffers,
  owners and lifetimes, GPU residency and host mirrors. Produce 1080p/4K
  estimates plus measured peaks where possible, explaining unavailable
  measurements. Compare only justified representation/reuse candidates
  against timing, precision, CPU oracles and moving-scene history/rejection
  behavior; defer broader layout changes until the baseline identifies them.
  A [residency inventory](../reports/2026-10-26-frame-residency.md) now
  lists every renderer allocation by owner with process heap usage at
  1080p/4K; the renderer's groups explain all frame-sized growth.
  [Lazy readback mirrors](../reports/2026-10-26-lazy-readback-mirrors.md)
  were adopted: GPU-only frames hold no host mirrors (1.71 GiB less
  host-visible memory for SVGF with reuse at 1080p, 6.83 GiB at 4K) with
  identical read-back products. A
  [reservoir history split](../reports/2026-10-26-reservoir-history-split.md)
  was adopted: each frame's source surfaces are stored once beside 64-byte
  records, so spatial reuse holds 512 instead of 640 B per pixel (1.0 GiB
  less at 4K) with byte-identical images and faster spatial frames.
  Remaining: value compaction of the surfaces needs a precision contract;
  lifetime aliasing stays deferred.

## Deferred adapter work

These retain their parent-phase assignments. The
[cross-phase rule](../design/ROADMAP_POLICY.md#cross-phase-delivery) permits
only the foundation subsets described here; broader parent-phase work stays
outside this queue.

- [ ] **LOTUS-MATERIAL-01 — Representative material fixtures (P1).**
  Workstream C, Renderer Phase 8 subset: audit UsdPreviewSurface assets in
  the policy's order against the
  [material contract](../reference/SCENE.md#materials-and-environment),
  [Hydra extraction](../reference/SCENE.md#hydra-extraction) and
  [capability matrix](../reference/CAPABILITY_MATRIX.md).
  Establish USD fixtures/reference images and Hydra/headless agreement;
  record normal/UV/alpha errors and unsupported inputs, then prioritize
  bounded fixes. Full OpenPBR/MaterialX and subsurface remain outside this
  subset. The [material basics fixture](../reports/2026-10-26-material-basics.md)
  established committed USD fixtures with C++ mirrors and reference images:
  base colour and metallic/roughness extract exactly through Hydra, render
  byte-identically to the headless mirror and match the reference; no fix
  was needed, and the roughness-0 floor is now documented. The
  [textures fixture](../reports/2026-10-27-material-textures.md) adds wrap
  modes, colour spaces, channel counts and texture-coordinate sources, with
  Hio's decoded texels equal to the mirror's; they need no fix. Untranslated
  connections such as `UsdTransform2d` silently took their defaults and now
  warn; translating `UsdTransform2d` is a candidate bounded fix. Minification
  without mipmaps is unbiased, with 17% more per-sample variance on the
  minified card. The [normals fixture](../reports/2026-10-27-material-normals.md)
  adds authored and computed mesh normals, normal-map encodings and tangent
  frames; Hydra's normals, Storm's computed ones included, equal the
  mirror's exactly, and none needs a fix. A constant tangent normal through
  Hydra names no texture-coordinate set and takes the normal-only frame;
  this is now documented, and naming `st` for it is a candidate bounded fix.
  The [emission fixture](../reports/2026-10-27-material-emission.md) adds
  constant, clamped and non-finite emission, emitters' sides and transforms,
  8-bit and HDR emission maps and fallbacks; they extract and render as
  documented and need no fix. `GeomSubset` material bindings were silently
  ignored and now warn; per-face materials are a candidate bounded fix.
  The [alpha fixture](../reports/2026-10-27-material-alpha.md) completes the
  audit order with constant coverage, thresholds and masks from every
  channel layout, inverted, black-bordered, missing and minified masks,
  stacked layers and masked emitters; they extract and render as documented
  and need no fix. A texture-driven `opacityThreshold` already warned;
  `displayOpacity` on unbound meshes is not read, and reading it is a
  candidate bounded fix.
  Remaining: prioritize the candidate bounded fixes.
  Path classification waits for distinct shading classes and
  measured benefit over its [existing upper bound](../reports/2026-10-11-wavefront-camera-resolve.md#path-classification).

## Previous delivery evidence

Completed work is tracked by the [release records](../releases/README.md),
including the [ReSTIR DI exit review](../reports/2026-10-17-renderer-phase5-exit.md),
[viewport foundation](../releases/v0.5.2.md),
[SVGF exit review](../reports/2026-10-18-svgf-viewport.md#renderer-phase-6-exit)
and [interactive-rendering release gate](../reports/2026-10-18-v0.6.0-release-gate.md).
The [archive index](../archive/README.md) preserves earlier plan citations.
