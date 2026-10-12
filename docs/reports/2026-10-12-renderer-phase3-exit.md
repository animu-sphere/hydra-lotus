# Renderer Phase 3 exit review

Measured on 2026-10-11 and 2026-10-12 for LOTUS-LIGHT-04, in the working
tree based on `7cb4922` (environment importance sampling merged), on the
machine and toolchain of the
[environment importance sampling report](2026-10-11-environment-importance-sampling.md):
Windows 11 x86_64, MSVC 14.51, OpenStrata 0.23.14, Release builds, an
NVIDIA RTX A5000 (driver 597.16, Vulkan 1.4.329) with Vulkan validation
and synchronization validation enabled, and OpenUSD 26.08 lookdev with
CPython 3.13 for Hydra. All runs were local; hosted CI was not measured.

## Scope

The [phase scope](../design/ROADMAP_POLICY.md#renderer-phase-3--direct-lighting--nee--mis)
was implemented in three slices: next event estimation of emissive
triangles with MIS ([evidence](2026-10-11-emissive-nee-mis.md)), analytic
UsdLux lights ([evidence](2026-10-11-analytic-lights.md)) and environment
importance sampling ([evidence](2026-10-11-environment-importance-sampling.md)).
Each slice measured time to equal quality on small check scenes. This
review measures it on the fixed benchmark workloads and two lit
workloads, at 128×128 under both integrators, and decides the two
questions the slices left: whether the wavefront integrator's inline
shadow rays should move to a queue of their own, and whether intersecting
every analytic light along each ray needs an acceleration structure.

## Benchmark changes

The benchmark gains two workloads and a variance measurement, so that
time to equal quality is repeatable with the fixed workloads' commands:

| Identity | Workload |
| --- | --- |
| `lights-v1` | The Cornell box without its ceiling and emissive panel, lit by one analytic light of each kind: a normalized distant light of UsdLux's default 0.53° through the open top, a sphere, a rect where the ceiling was and a disk on the green wall. 32 triangles, 6 instances, 4 analytic lights, a black environment |
| `environment-v1` | The Cornell box's blocks on its floor widened to 120 units, under OpenUSD's Stinson Beach image (the committed [validation asset](2026-10-11-environment-importance-sampling.md#the-stinson-beach-image)), its sun behind the camera, which looks 25° down and sees only the floor and the blocks. 26 triangles, 3 instances, 1 texture |

Every workload now also renders 16 fresh accumulations of 16 samples
from sample index 2²¹ and reports `per_sample_variance`, the mean over
pixels and channels of each value's variance across them times 16, as
the light sampling checks estimate it, and `variance_gpu_ms`, that times
the median GPU frame time: the time to a given variance is proportional to
it. Its scenes also report their light table (`emissive_triangles`,
`analytic_lights`, `environment_sampled`). The CTest contract checks six
workloads, their light tables and a positive variance. Commands, from the
repository root, each repeated five times as runs `r1`–`r5`:

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/light04-r1-bench-reference-bsdf.json --benchmark-label "7cb4922 + LOTUS-LIGHT-04; ost core Release" --benchmark-light-sampling bsdf
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/light04-r1-bench-reference-mis.json --benchmark-label "7cb4922 + LOTUS-LIGHT-04; ost core Release" --benchmark-light-sampling mis
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/light04-r1-bench-wavefront-bsdf.json --benchmark-label "7cb4922 + LOTUS-LIGHT-04; ost core Release" --benchmark-integrator wavefront
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/light04-r1-bench-wavefront-mis.json --benchmark-label "7cb4922 + LOTUS-LIGHT-04; ost core Release" --benchmark-integrator wavefront --benchmark-light-sampling mis
```

The images and variances were identical in all five runs. The images of
the four fixed workloads are byte-identical to the
[environment importance sampling report](2026-10-11-environment-importance-sampling.md#fixed-benchmarks)'s
under all four configurations, and the two lit workloads' light-sampled
images are the same under both integrators.

### Timing interference

Another application was rendering on the same GPU throughout, at about
half its utilization while the benchmark was idle. Frame times changed
by up to ten times between runs for the same images (`geometry-v1` under
the reference integrator: 0.097 ms in one run, 0.85–0.91 ms in the
others), as the [scheduling report](2026-10-10-wavefront-scheduling.md#larger-targets)
saw with the desktop. Interference only adds time, so the tables use the
lowest of the five runs' median GPU frame times. `cornell-v1` and
`textured-v1` varied by no more than 3% between runs under every
configuration; the other times are less certain, which the
conclusions below do not depend on.

## Time to equal quality

Both estimators are unbiased, so equal variance per unit time is equal
quality: light sampling shortens the time to equal quality by the ratio
of the two `variance_gpu_ms` values.

| Workload | Integrator | GPU ms, BSDF | GPU ms, MIS | Per-sample variance, BSDF | MIS | Variance | Time to equal quality |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `cornell-v1` | reference | 0.068 | 0.098 | 1.431 | 0.3785 | 3.78× lower | **2.63× shorter** |
| `textured-v1` | reference | 0.046 | 0.047 | 0.02405 | the same | equal | 1.00× |
| `instanced-v1` | reference | 0.035 | 0.036 | 0.005789 | the same | equal | 0.99× |
| `geometry-v1` | reference | 0.097 | 0.096 | 0.005566 | the same | equal | 1.01× |
| `lights-v1` | reference | 0.069 | 0.080 | 2962 | 0.3910 | 7,574× lower | **6,560× shorter** |
| `environment-v1` | reference | 0.037 | 0.072 | 2199 | 0.6819 | 3,225× lower | **1,657× shorter** |
| `cornell-v1` | wavefront | 0.175 | 0.221 | 1.431 | 0.3785 | 3.78× lower | **3.00× shorter** |
| `textured-v1` | wavefront | 0.120 | 0.121 | 0.02405 | the same | equal | 1.00× |
| `instanced-v1` | wavefront | 0.101 | 0.101 | 0.005789 | the same | equal | 0.99× |
| `geometry-v1` | wavefront | 0.229 | (0.335) | 0.005566 | the same | equal | (see below) |
| `lights-v1` | wavefront | 0.159 | 0.196 | 2962 | 0.3910 | 7,574× lower | **6,154× shorter** |
| `environment-v1` | wavefront | 0.093 | 0.154 | 2199 | 0.6819 | 3,225× lower | **1,937× shorter** |

`textured-v1`, `instanced-v1` and `geometry-v1` are lit by the constant
white environment alone, which stays with BSDF sampling
([measurement](2026-10-11-emissive-nee-mis.md#the-constant-environment)):
their light tables are empty, both estimators run the same instructions
and produce byte-identical images, and their times to equal quality are
equal within the timing noise. The wavefront integrator's MIS run of
`geometry-v1` was slowed in all five runs (0.34–1.36 ms); the
[environment importance sampling report](2026-10-11-environment-importance-sampling.md#fixed-benchmarks)
measured 0.232 ms for it against 0.230 ms under BSDF sampling.

The Cornell box's gain is the [first slice's](2026-10-11-emissive-nee-mis.md#variance-and-time-to-equal-quality)
2.61×, reproduced: its variance is mostly indirect light between the walls,
which next event estimation shortens by one bounce at most. The lit
workloads gain three to four orders of magnitude, because brute force
finds their small, bright sources rarely: a cosine-weighted direction
from the floor meets the 0.53° sun about once in 50,000 and the beach's
sun about once in ten thousand. For the same reason their BSDF
variances, from 4.2 million samples each, rest on comparatively few such
hits; the
[environment report](2026-10-11-environment-importance-sampling.md#variance-and-time)'s
floor measured BSDF sampling's variance 8% below its exact value. An
underestimate makes the ratios conservative.

Each workload's 64-sample image means agree between the estimators
within about two standard errors, estimated from the per-sample
variances: `cornell-v1` 0.5473 and 0.5465 (about 1.1), `lights-v1` 0.2271
and 0.2803 (about −1.7), `environment-v1` 0.3457 and 0.2977 (about 1.8);
the BSDF means of the lit
workloads move with their rare hits. The checks
`renderer.path.analytic_lights` and `renderer.path.environment` compare
both estimators with independent oracles.

Across integrators, the wavefront integrator under light sampling
reaches equal quality sooner than the reference integrator under BSDF
sampling on every lit workload: 1.17× on `cornell-v1`, 2,678× on
`lights-v1` and 776× on `environment-v1`. It remains 2.1–2.8 times slower
than the reference integrator per sample, so the reference integrator
stays the default in the backend and in Hydra, and light sampling is the
default under both.

## Shadow rays in the wavefront integrator

The shade kernel traces each event's shadow ray inline. A queue of its
own would still trace the same rays; it could save only the divergence
between paths that trace one and paths that do not, at the cost of one
more dispatch and barrier in each round. The wavefront integrator's
kernel times, lowest of five runs' medians:

| Workload | Rounds | Shade ms, BSDF → MIS | Frame ms, BSDF → MIS |
| --- | ---: | ---: | ---: |
| `cornell-v1` | 6 | 0.0476 → 0.0733 (+0.026) | 0.175 → 0.221 (+0.046) |
| `lights-v1` | 5 | 0.0434 → 0.0624 (+0.019) | 0.159 → 0.196 (+0.037) |
| `environment-v1` | 3 | 0.0206 → 0.0494 (+0.029) | 0.093 → 0.154 (+0.061) |

The whole of light sampling's cost in the shade kernel is 19–29 µs per
frame, light selection and evaluation included. The
[scheduling report](2026-10-10-wavefront-scheduling.md#choosing-the-tail-threshold)
measured 15–25 µs of `cornell-v1`'s frame per round of two dispatches
and two barriers; even at half that per added dispatch and barrier, a
shadow queue would add 45–75 µs to `cornell-v1`'s six rounds, more than
the shade kernel's whole increase, which it cannot remove. **The shadow
rays stay inline.** Visibility is measured again where it changes shape:
Renderer Phase 5 resolves visibility after resampling, and Renderer
Phase 9's ray sorting and scheduling candidates are evaluated against
these numbers.

## Analytic lights along each ray

Every ray tests every analytic light: `CrossedLights` loops over the
records at each hit and escape, and under BSDF sampling each scattering
event also loops over them for parallel lights. Light selection is
constant time (the alias table). The cost was measured with a temporary
build, not committed, that added *n* sphere lights of radius 0.02 and
radiance 0.01 in a grid above `lights-v1`'s open box; GPU ms per
128×128 sample, lowest of three runs' medians:

| Analytic lights | Reference, BSDF | Reference, MIS | Wavefront, MIS | Wavefront shade, MIS |
| ---: | ---: | ---: | ---: | ---: |
| 4 | 0.077 | 0.092 | 0.180 | 0.057 |
| 20 | 0.126 | 0.177 | 0.256 | 0.081 |
| 68 | 0.428 | 0.212 | 0.424 | 0.118 |
| 260 | 1.035 | 0.776 | 1.426 | 0.320 |
| 1028 | 6.954 | 3.892 | 5.945 | 1.050 |

The cost grows linearly, about 3.7 µs per light per frame under the
reference integrator's light sampling: 64 more lights double or treble a
light-sampled frame, a thousand more multiply it 33 to 42 times. BSDF
sampling pays more, through its second loop. The UsdLux scenes the phase targets, and its
checks, hold a handful of lights, where the loop is a small part of a
frame; the exit criteria are met without an acceleration structure.
**None is added in Renderer Phase 3.** Scenes of many lights are the
subject of Renderer Phase 5 (ReSTIR DI, many lights) and of Renderer
Phase 9's light tree and hierarchical light sampling candidates; the
crossing test needs the same hierarchy, or the lights as procedural
primitives in the ray-tracing acceleration structure. The assignment is
LOTUS-LIGHT-05 in the [roadmap](../roadmap/current.md), with this table
as its baseline.

## Renderer Phase 3 exit

The [exit criterion](../design/ROADMAP_POLICY.md#renderer-phase-3--direct-lighting--nee--mis)
was reviewed against the Renderer Phase 3 evidence, and the phase was
accepted as complete on 2026-10-12.

| Criterion | Evidence |
| --- | --- |
| Time to equal quality improves clearly over brute-force reference path tracing | under both integrators: 2.63× and 3.00× shorter in the Cornell box, 6,560× and 6,154× with analytic lights, 1,657× and 1,937× under the Stinson Beach image; unchanged where the constant environment alone lights a scene ([above](#time-to-equal-quality)) |

Dispositions of the phase's scope:

| Scope | Disposition |
| --- | --- |
| Light representation | emissive triangles from the material IR, `Lotus::Light` for UsdLux sphere, rect, disk and distant lights, `Lotus::EnvironmentMap` for textured domes ([analytic lights](2026-10-11-analytic-lights.md), [environment](2026-10-11-environment-importance-sampling.md)) |
| Light sampling, NEE, shadow rays | one alias-method light table for all three, four random numbers per event, shadow rays with partial coverage as transmittance ([emissive NEE](2026-10-11-emissive-nee-mis.md)) |
| BSDF and light sampling densities, MIS with the power heuristic | for every light kind, both ways; the camera's view of an emitter and parallel lights unweighted |
| Emissive geometry sampling | by area, in proportion to area times emission |
| Environment importance sampling | a piecewise-constant distribution of the image's radiance; the constant environment stays with BSDF sampling, which [samples it better](2026-10-11-emissive-nee-mis.md#the-constant-environment) |
| Wavefront shadow rays | inline ([above](#shadow-rays-in-the-wavefront-integrator)) |
| Analytic light acceleration | not needed for the phase; assigned to LOTUS-LIGHT-05 ([above](#analytic-lights-along-each-ray)) |

Not supported, as the [light reference](../reference/SCENE.md#lights)
lists: light textures other than the dome's, shaping, shadow controls,
colour temperature, light linking, `treatAsPoint`, cylinder, portal,
geometry and mesh lights, and more than one textured dome.

## Runs

| Configuration | Result |
| --- | --- |
| core | `ost build`, `ost test` 20/20 (the three benchmark contracts with six workloads), `ost validate --strict-renderer-evidence` passed |
| Hydra | `ost build`, `ost test` 43/43, `ost validate --strict-renderer-evidence` passed |
| ci-core, without a runtime | `ost build`, `ost test` 12/12 |
| scripts | `scripts/check_docs.py` and the 27 `scripts/test_*.py` regressions passed |

Only `adapters/headless/benchmark.cpp` and the benchmark's CTest contract
changed, no header; the renderer's images and checks are unchanged.
