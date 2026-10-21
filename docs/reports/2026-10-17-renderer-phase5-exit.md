# Renderer Phase 5 exit review: NEE/MIS stays the default

Reviewed 2026-10-17 against `6705c61`. This review adds no measurement and
changes no renderer, shader or harness code; it records the decision that the
[Renderer Phase 5 exit criteria](../design/ROADMAP_POLICY.md#renderer-phase-5--restir-di)
require, from the evidence linked below. This is LOTUS-RESTIR-06 in the
[execution plan](../archive/pre-v070-execution-plan.md#step-1--restir-di-closure). The
[reservoir reference](../reference/RESTIR.md#when-to-use-reservoirs) carries
the resulting guidance.

## Decision

- **Default renderer path: progressive NEE/MIS**, under both integrators,
  headless and through Hydra. With area lights, an environment, glossy
  materials or deep interreflection it already samples well, and no reservoir
  mode reaches equal quality faster, accumulating or in motion.
- **Choose ReSTIR for many lights of similar importance that BSDF sampling
  cannot find**: initial-only reservoirs with eight candidates (`lotus:restir`
  on, reuse off; eight is the Hydra default). They keep deterministic
  progressive accumulation, need 96 bytes per pixel and no history, and add no
  frame correlation.
- **Do not use** temporal or combined reuse with accumulation, spatial reuse at
  radius 1, or one-candidate initial-only reservoirs as a renderer path. They
  remain selectable for comparison, diagnostics and denoiser input.
- **Temporal reuse as denoiser input** is a Renderer Phase 6 question: it cuts
  raw flicker where reservoirs win, at a correlation cost, and its random
  numbers replay after each reset ([DES-Q7](../design/DESIGN_POLICY.md#53-open-questions)).

ReSTIR did not have to win, and on three of the four representative workloads
it does not.

## Per-workload results

The [reuse-cost decision](2026-10-17-restir-reuse-cost.md) measured four
suites, each a static and a moving scene, under both integrators at locked GPU
clocks, three runs at disjoint seeds, 128x128 with a 256x256 check. Images are
byte identical between the integrators, so quality applies to both. Static
figures are horizon efficiency (time to equal quality relative to NEE/MIS,
above one is faster); motion figures are per-frame error at equal GPU time
relative to NEE/MIS. I8 is initial-only with eight candidates.

| Workload | Equal-quality speedup, static | Equal GPU time, moving | GPU time of reuse vs NEE/MIS | Path |
| --- | --- | --- | --- | --- |
| Many distant lights on a Lambert plane, moving occluder | **yes**: I8 1.35–1.42 (1.27–1.41 at 256x256); every other mode 0.03–0.89 | **yes**: I8 1.37–1.48; temporal 1.05–1.22, break-even at 256x256 | I8 1.32–1.39; temporal 1.81–2.08; spatial/combined 2.26–2.66 (2.9–3.0 at 256x256) | ReSTIR I8 |
| Rect/disk lights, HDR environment, Lambert/GGX boundaries | no: best I8 0.93–0.97 | no: best I8 0.90–0.96 | 1.08–1.94 | NEE/MIS |
| Rect/sphere lights, rough dielectric and metal, camera disocclusion | no: best I1 0.88–0.92, I8 0.82–0.87 | no: at most 0.92 | 1.09–2.16 | NEE/MIS |
| Closed four-bounce room, indirect about half of each pixel | no: best I8 0.89–0.93 | no: best I8 0.90–0.94 | 1.04–1.46 | NEE/MIS |

Reuse adds a roughly fixed per-frame cost (0.02–0.05 ms for initial candidates,
0.11–0.13 ms for temporal and 0.17–0.21 ms for spatial or combined reuse at
128x128), so its ratio falls with transport depth but never turns into a
speedup outside the distant-light scene.

## Bias and artifacts

No bias, ghosting or localized error attributable to reuse was found in the
bounded fixtures:
[static ensembles](2026-10-16-restir-sequence-ensemble.md) (the static temporal
penalty is correlation, not detectable bias),
[four moving suites](2026-10-17-restir-moving-bias.md) per pose, band, 8x8 tile
and motion speed, a [closed interreflecting room](2026-10-17-restir-interreflection.md)
at four bounces, and [light/material animation](2026-10-17-restir-animation.md)
with exact resets and no frames-since-reset dependence. These bound, rather
than establish, unbiasedness: finite references, at most four independent
sequences per invocation in the cost campaign, and matched integrator draws
that are not independent trials.

## Known bad cases

| Case | Effect | Evidence |
| --- | --- | --- |
| Temporal or combined reuse with accumulation | correlated frames accumulate far less: horizon efficiency 0.03–0.74, variance inflation 1.1–56; most cases never reach the 64-frame NEE/MIS error | [reuse cost](2026-10-17-restir-reuse-cost.md#time-to-equal-quality-static-scenes), [sequence ensemble](2026-10-16-restir-sequence-ensemble.md) |
| Static rough dielectric/sphere lights with temporal reuse | prefix RMS 0.074 against 0.052 for NEE/MIS | [glossy spheres](2026-10-16-restir-glossy-spheres.md#results) |
| Spatial reuse at radius 1 | lower error per frame, but 0.49–0.89 static and at most 1.02 in motion for its cost | [reuse cost](2026-10-17-restir-reuse-cost.md#decision) |
| One-candidate initial-only reservoirs | never beat NEE/MIS; in the room, fresh RMS 0.543 against 0.512 | [reuse cost](2026-10-17-restir-reuse-cost.md#time-to-equal-quality-static-scenes) |
| Narrow glossy highlights | resolution limited by rare-event variance for every estimator; complementary BSDF weights reduce but do not remove the reproduced gap | [moving bias](2026-10-17-restir-moving-bias.md#rare-event-limit-in-the-narrow-glossy-fixture), [BSDF partition](2026-10-16-restir-bsdf-partition.md) |
| Light/material animation | a small pooled reuse excess, about 1% of the mean signal and shared by spatial-only reuse, stays unresolved | [animation](2026-10-17-restir-animation.md#disposition) |
| Reuse frames through `RenderTemporalScene` | 28–36 ms of wall time at 128x128 (120–154 ms at 256x256) reading back and checking evidence products, against 1.3–2.0 ms for NEE/MIS | [wall time](2026-10-17-restir-reuse-cost.md#wall-time-and-memory) |

## Correlation cost

- **Within a sequence.** Temporal reuse raises lag-one residual
  autocorrelation to 0.65–0.69 in the distant-light scene, where it also cuts
  raw frame-to-frame flicker to 0.30–0.38 of NEE/MIS; accumulation there is
  8–30 times slower. In the other workloads it changes frame differences by
  2–15% (lag-one 0.00–0.12). Initial-only and spatial-only frames are
  uncorrelated (variance inflation 1.0).
- **Across the history cap.** Variance inflation in the static glossy fixture
  falls from 2.34 at cap 32 to 1.36 at cap 8 and 1.09 at cap 2; represented
  candidate counts are not independent samples.
- **Across resets.** Each reset restarts the fresh batch at `sample_index`,
  so reuse noise correlates about 0.58 across edits, and a scene edited every
  frame replays one set of random numbers. Kept as is and decided with the
  denoiser ([DES-Q7](../design/DESIGN_POLICY.md#53-open-questions)).

## Unsupported conditions

- Reservoirs light only the primary surface; later bounces keep NEE/MIS.
  There are no indirect (GI) reservoirs.
- Reuse history is valid only within one light-table generation: analytic,
  environment and material edits reset it, and any edit in a scene with
  emissive triangles resets it conservatively. Light slots are not persistent
  identities and are not remapped.
- History follows camera and rigid placement motion with nearest-pixel
  reprojection; deformation and same-count instance reordering need a caller
  cut or history key; Hydra retains one view per delegate
  ([compatibility](../reference/RESTIR.md#compatibility-and-publication)).
- Hydra exposes only the power proposal; the uniform and mixture proposals are
  core and headless only. Reuse needs ray queries and colour clears and keeps
  the Hydra pass unconverged.
- The spatial stencil and rejection thresholds are fixed; GPU float behaviour
  near selection boundaries and at extreme dynamic range is not established.
- Not measured: emissive-mesh, textured-light and production scenes for cost;
  environment, texture or geometry animation; images above 256x256; a light-count
  sweep (every ray still tests every analytic light, LOTUS-LIGHT-05); other
  GPUs, clocks and platforms.
- Interactive reuse: the wall time above rules out reuse through
  `RenderTemporalScene` in an interactive loop until a path skips the evidence
  readbacks.

## Exit criteria

| Criterion | Evidence |
| --- | --- |
| 1. Animation and reset | stepped light, power and material edits reset exactly, with no ghosting ([animation](2026-10-17-restir-animation.md)); emissive emission, toggle, visibility and placement edits headless and USD light, material, emission, placement and visibility edits through Hydra check reset reasons, no stale reservoir or radiance, byte-identical integrators and clean validation ([reset contract](2026-10-17-restir-reset-contract.md)) |
| 2. Reuse cost and an equal-quality decision | NEE/MIS, initial-only, temporal-only, spatial-only and combined reuse with one and eight candidates on four suites under both integrators: GPU time, time to equal quality, equal-time error in motion, memory and temporal stability at locked clocks ([reuse cost](2026-10-17-restir-reuse-cost.md)) |
| 3. A recorded decision | this review: per-workload speedups, bad cases, correlation cost, unsupported conditions and the default path |

The criteria are met, and Renderer Phase 5 closes with a mostly negative
result.

## Scope dispositions

| Scope | Disposition |
| --- | --- |
| Reservoir representation | 16-byte CPU record and 48/64-byte GPU records with independent arithmetic oracles ([reservoir](2026-10-13-restir-reservoir.md), [reuse arithmetic](2026-10-13-restir-reuse-arithmetic.md)) |
| Candidate generation | emissive triangles, analytic lights and textured environments in one parameter measure ([initial](2026-10-13-restir-initial.md)) |
| Reservoir update | streaming, same-target and support-corrected differing-target merges, bounded counts and transactional failure ([temporal](2026-10-13-restir-temporal.md)) |
| Temporal reuse | shared reprojection and validity, receiver reevaluation, exact source targets ([temporal](2026-10-13-restir-temporal.md), [evaluation](2026-10-16-restir-evaluation.md)) |
| Spatial reuse | one immutable pass with surface-boundary rejection ([spatial](2026-10-14-restir-spatial.md)) |
| Visibility handling | current-receiver transmittance, same-sample reuse only within a frame ([visibility](2026-10-15-restir-visibility.md)) |
| Bias and normalization validation | the bias evidence above and the [complementary BSDF partition](2026-10-16-restir-bsdf-partition.md) |
| Light candidate strategies | power, uniform and equal-mixture proposals ([proposals](2026-10-15-restir-proposals.md)); power remains the default |
| Hydra | controls, six diagnostic AOVs and moving/cut sequences in installed usdview ([Hydra](2026-10-14-restir-hydra.md)) |

## After the exit

The [stop rule](../design/ROADMAP_POLICY.md#renderer-phase-5--restir-di)
applies: ReSTIR DI gains no new fixtures and is one selectable transport mode.
Open items are routed rather than reopening the phase:

| Item | Route |
| --- | --- |
| Fresh-batch random numbers replayed after resets | [DES-Q7](../design/DESIGN_POLICY.md#53-open-questions), decided with the denoiser (LOTUS-SVGF-01) |
| Temporal reuse as denoiser input | Renderer Phase 6 |
| Reuse path without evidence readbacks | LOTUS-VIEWPORT-03 ([plan](../archive/pre-v070-execution-plan.md#step-2--viewport-foundation)) |
| Production materials and scenes | Renderer Phase 8 (LOTUS-MATERIAL-01) |
| Indirect reservoirs, light-domain remapping | Renderer Phase 7 |
| Light selection, light trees, many-light acceleration | Renderer Phase 9 (LOTUS-LIGHT-05) |
| A correctness bug | fixed as a bug |

## Validation

Documentation only: `python scripts/check_docs.py` passes. The tree is
verified by the [v0.5.1 release gate](2026-10-17-v0.5.1-release-gate.md).
