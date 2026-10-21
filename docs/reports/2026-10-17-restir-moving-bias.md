# ReSTIR moving-scene and localized bias: none detectable at fixture resolution

Measured 2026-10-17 on Windows, MSVC 19.51.36256 Release, Vulkan/Slang,
NVIDIA RTX A5000. Source base: `6fdcaa8`, plus the headless replication bias
tests. The [reservoir reference](../reference/RESTIR.md#headless-reservoir-comparison)
owns their definitions. Transport shaders, reservoir arithmetic and public
renderer settings are unchanged.

## Question and change

The [sequence-ensemble measurement](2026-10-16-restir-sequence-ensemble.md)
found no detectable bias in one static fixture, pooled over the image. It left
open moving-scene bias and ghosting, and localized errors that pooling can hide.
Existing moving metrics (final-pose ensemble RMS, maximum 8x8 tile mean error)
were not compared with their expected noise, so they could not separate bias
from finite sampling.

Every case now reports `replication_bias`. In moving scenes, each pose's fresh
frame from each of the independent sequences is a replication: every sequence
reaches the pose through the same motion with independent draws. Static scenes
use sequence means, as before. For each pose, the replications' mean error is
compared with its expected noise, including that pose's finite-reference
variance from the two reference halves, giving a **noise-normalized MSE**
(near one without detectable bias) over the image and over each fixed screen
band. Each 8x8 tile mean, per channel and pose, gets a **tile z**: its error over
its expected noise, with neighbouring-pixel correlation included because tile
means themselves are replicated. Ghosting (history carried from a previous pose)
and localized bias would appear as persistent error in these tests.

## Procedure

Ten invocations ran serially with no other GPU work: all four suites at 32x32,
**64 independent sequences** of 64 frames, four warmup frames, 64 analytic lights,
eight power-proposal candidates, history cap 32, radius 1, one bounce, reference
seed `16777216` and two disjoint **4096-spp** NEE/MIS reference halves per pose.
Radiance reuse is disabled. Each suite ran with measured seeds `17` (a) and
`4194385` (b), which give disjoint measured ranges. Two further invocations
repeat glossy-disocclusion seed a with the independent reference seed `18874368`,
and glossy-spheres seed a under the wavefront integrator.

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark-restir build/restir-bias-spheres-reference-cap32-a.json --benchmark-label "6fdcaa8 plus ReSTIR replication bias tests; Release; 2026-10-17" --benchmark-integrator reference --restir-workload glossy-spheres --benchmark-size 32 --benchmark-frames 64 --benchmark-warmup 4 --restir-sequences 64 --restir-lights 64 --restir-reference-samples 4096 --restir-candidates 8 --restir-history-cap 32 --restir-seed 17 --restir-bounces 1 --restir-reference-seed 16777216
```

Other runs change only the workload, seed, reference seed, integrator and report
path; each takes about 190 seconds. The moving suites cover a blocker and camera
translating together over a distant-lit plane (`distant`) and over the
material/depth boundary with area lights and an environment (`mixed-boundaries`),
and camera-only parallax with narrow glossy metal (`glossy-disocclusion`) or a
rough dielectric and sphere lights (`glossy-spheres`). Each moving pose has its
own reference; all modes of a run share them. Each pose's frame across 64
sequences gives 63 degrees of freedom; at 32x32 there are 16 tiles, so a moving
case has 3072 tile tests and a static case 48.

The [summary JSON](2026-10-17-restir-moving-bias.json) retains build/device,
procedures, every replication test, per-pose ratios, the motion-speed split and
verification counts. Raw JSON, PFMs, logs and the runner
`build/run-restir-moving-bias.py` remain under `build/`.

## Moving results

Noise-normalized MSE pooled over all poses, seed a/b, with `|z| > 3` tile counts
out of 3072:

| Suite | NEE/MIS | Initial | Temporal | Spatial | Spatiotemporal |
| --- | --- | --- | --- | --- | --- |
| distant | 1.001/0.995, 14/18 | 1.001/1.003, 10/12 | 1.015/1.002, 20/7 | 1.010/1.007, 6/15 | 0.995/1.011, 18/18 |
| mixed-boundaries | 1.002/1.008, 8/7 | 1.011/0.993, 28/8 | 1.006/0.994, 23/14 | 1.002/0.994, 31/8 | 1.005/0.995, 23/9 |
| glossy-disocclusion | 1.030/1.026, 41/24 | 1.021/1.012, 41/34 | 1.027/1.004, 39/47 | 1.024/1.005, 42/39 | 1.031/1.002, 49/48 |
| glossy-spheres | 1.000/1.009, 16/14 | 1.011/1.001, 19/22 | 1.006/1.000, 7/17 | 1.023/0.998, 16/19 | 1.002/1.002, 12/15 |

NEE/MIS and initial-only reservoirs do not reuse anything and serve as the
empirical null. Every reuse mode lies within their range in every suite and
seed: pooled ratios span 0.994–1.031 against 0.993–1.030 for the non-reusing
modes. Band ratios agree in the same way: center-boundary 0.98–1.03 for reuse
against 0.99–1.05, right-glossy 0.95–1.09 against 0.97–1.08, left-dielectric
1.01–1.03 against 1.00–1.02. Tile exceedance counts above the normal expectation
of about eight also occur in the non-reusing modes (up to 28 for initial-only in
mixed seed a, 41 for NEE/MIS in glossy-disocclusion). They reflect heavy-tailed
noise for which 64 replications underestimate the variance, not reuse-specific
error. The largest tile `|z|` is 7.9 for reuse, against 10.2 for NEE/MIS and
6.8 for initial-only.

Ghosting carries error from previous poses and should grow with motion speed.
Both motions follow `sin(2 pi f / frames)`; the 34 poses with `|cos| > 0.7` move
fastest. Per-pose ratio excess over initial-only, averaged separately over fast
and slow poses, stays within ±0.02 for every reuse mode, suite and seed, inside
the ±0.04 that NEE/MIS shows against the same baseline. No speed dependence is
visible.

## Rare-event limit in the narrow glossy fixture

In glossy-disocclusion every mode, NEE/MIS included, has its largest per-pose
ratio at pose 46 (1.63–1.79 seed a, 1.35–1.58 seed b) and large negative tile
`z` (NEE/MIS −10.2 at pose 62). Rerunning seed a with the independent reference
seed reproduces this: unchanged measured images, pose 46 ratios 1.60–1.78 and
NEE/MIS tile `z` −13.2. The cause is therefore the measured draws, not the
reference: one-sample frames rarely reach the narrow highlights of the small,
bright lights, so a pose's mean over 64 replications usually falls short of it
and its sample variance understates the true variance. This is rare-event
variance shared by all estimators, which limits the test's resolution here. It
is not evidence for or against reuse bias in this fixture.

## Static results and resolution

Static pooled ratios agree with the sequence ensemble by construction and span
0.846–1.174 over all suites, seeds and modes (NEE/MIS 0.846–1.109). Static tile
counts are 0 or 1 of 48 in every case and the largest `|z|` is 3.7. The
distant-plane spatiotemporal ratio is 0.928/0.912 against its exact integral, an
overestimate of noise consistent with spatially correlated pixels widening
the pooled ratio's spread; it is not excess error.

The resolution follows from the null's spread. Pooled over 64 poses, a uniform
persistent error adding a few percent of the expected noise would raise the
ratio beyond the non-reusing modes' spread; for glossy-spheres temporal reuse,
whose pose noise is 1.22e-3 per component, 3% is an RMS bias of about 0.006, under
2% of its mean red radiance. A single pose resolves about ±15%, and a
tile only errors comparable to its own noise. Smaller, sign-changing or rarer
localized errors can pass, and no confidence interval is asserted.

## Independent checks and validation

All ten invocations pass with Vulkan and synchronization validation available and
zero captured messages. The reducer accepts each suite's two seeds, validating
pose pooling, every pose's reference noise against its split RMS, the final-pose
ensemble identity and the static sequence-ensemble agreement. Rereading the saved
ensemble and reference PFMs reproduces all 100 static grand mean and moving
final-pose MSEs within 2e-5 relative tolerance. Reference and wavefront
glossy-spheres runs have equal replication tests, sequence ensembles, residuals
and regions, and all 85 matched PFM pairs are byte identical; changing only the
reference seed leaves all 20 measured images byte identical.

`lotus-renderer-restir-statistics` compares the test with a direct definition
over a partial tile, two bands and a finite reference variance, detects an
injected persistent error and checks frozen, rejected and single-replication
inputs. The benchmark CTest variants check replication kinds, pose and tile
populations, region identities and reference noise. Two reducer tests cover
retention, summaries and rejected inconsistencies.

The managed core build and all 44 tests pass in 859.38 seconds, followed by
strict renderer-evidence validation. The runtime-free build/test records 31
passes and two explained GPU SKIPs. All 50 Python tests and documentation
validation pass.

```sh
ost build --jobs auto
ost test
ost validate --strict-renderer-evidence
ost build --without-runtime --intent ci-core --jobs auto
ost test --without-runtime --intent ci-core
python -m unittest discover -s scripts -p 'test_*.py'
python scripts/check_docs.py
```

Hydra and the release gate are not rerun for this headless-only change.

## Disposition

In four bounded moving fixtures (moving blocker shadows, material/depth
boundaries, camera disocclusion over narrow and rough glossy surfaces) no
temporal, spatial or combined reuse mode shows moving-scene bias, ghosting or
localized error distinguishable from non-reusing estimators, over the image,
fixed bands, 8x8 tiles or motion speed, at the resolutions above. Narrow glossy
highlights are limited by rare-event variance affecting every estimator. This
does not establish unbiasedness, cover light or material animation, production
scenes, substantial interreflection or larger images, or show a reuse speedup.
Remaining LOTUS-RESTIR-05 items stay in the [execution plan](../archive/pre-v070-execution-plan.md#execution-order).
No phase exit is claimed.
