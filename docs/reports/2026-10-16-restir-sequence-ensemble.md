# ReSTIR static reuse penalty: correlation, not detectable bias

Measured 2026-10-16 on Windows, MSVC 19.51.36256 Release, Vulkan/Slang,
NVIDIA RTX A5000. Source base: `d4153be`, plus the headless sequence-ensemble
decomposition. The [reservoir reference](../reference/RESTIR.md#headless-reservoir-comparison)
owns its definitions. Transport shaders, reservoir arithmetic and public
renderer settings are unchanged.

## Question and change

The [four-bounce measurement](2026-10-16-restir-convergence.md) reproduced a
static temporal-reuse penalty: about 1.5 times initial-only prefix RMS, with
positive residual correlation, under two reference seeds. Four sequences per
invocation could not separate three explanations: persistent bias, finite-reference
or rare-event variance, and correlated frames that add fewer independent samples.

Static cases now report `sequence_ensemble`. Independent sequences serve as
replications: the across-sequence variance of sequence means is unaffected by
correlation within a sequence. Compared with within-sequence frame variance, it
gives a **variance inflation** (one for uncorrelated frames) and effective frame
count. The grand mean error is compared with its expected noise, including the
shared finite reference, giving a **noise-normalized MSE** (near one without
detectable bias) and an excess-MSE estimate of squared bias. The reducer validates
these identities and retains them per run.

## Procedure

Six invocations run serially with no other GPU work. All use the unchanged
`glossy-spheres` pair at 32x32, **64 independent sequences** of 64 frames, four
warmup frames, 64 analytic lights, eight power-proposal candidates, radius 1,
reference seed `16777216` and two disjoint **4096-spp** NEE/MIS reference halves.
Radiance reuse is disabled.

| Run | Integrator | History cap | Measured seed | Bounces |
| --- | --- | --- | --- | --- |
| cap32-a | reference | 32 | 17 | 4 |
| cap32-b | reference | 32 | 4194385 | 4 |
| cap8-a | reference | 8 | 17 | 4 |
| cap2-a | reference | 2 | 17 | 4 |
| cap32-bounce1-a | reference | 32 | 17 | 1 |
| wavefront-cap32-a | wavefront | 32 | 17 | 4 |

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark-restir build/restir-ensemble-reference-cap32-a.json --benchmark-label "d4153be plus ReSTIR sequence-ensemble decomposition; Release; 2026-10-16" --benchmark-integrator reference --restir-workload glossy-spheres --benchmark-size 32 --benchmark-frames 64 --benchmark-warmup 4 --restir-sequences 64 --restir-lights 64 --restir-reference-samples 4096 --restir-candidates 8 --restir-history-cap 32 --restir-seed 17 --restir-bounces 4 --restir-reference-seed 16777216
```

Other runs change only the table's fields and report path. Seeds `17` and
`4194385` give disjoint measured ranges, and the two cap-32 runs are reduced
together by `scripts/restir_budget.py`. Seed-17 runs share measured draws, so
modes the cap does not affect (NEE/MIS, initial-only, and spatial-only except
at cap 2, where the neighbour cap falls below eight) repeat identical values;
they are not independent replications. All runs
share one reference. The [summary JSON](2026-10-16-restir-sequence-ensemble.json)
retains build/device, procedures, static decompositions, the repeated cap-32
reduction and verification counts. Raw JSON, PFMs, logs and the runner
`build/run-restir-sequence-ensemble.py` remain under `build/`.

## Static results

Static reference split RMS is 0.006319; reference noise MSE is 9.98e-6. Mean
static radiance is about (0.378, 0.071, 0.080). Both cap-32 seeds:

| Mode | Prefix RMS a/b | Frame variance a | Inflation a/b | Effective frames a | Normalized MSE a/b |
| --- | --- | --- | --- | --- | --- |
| NEE/MIS | 0.03642/0.03650 | 0.08434 | 0.997/1.003 | 64.2 | 1.103/1.009 |
| Initial | 0.03563/0.03595 | 0.08053 | 0.999/1.018 | 64.0 | 1.063/1.066 |
| Temporal | 0.05355/0.05432 | 0.07817 | 2.340/2.396 | 27.4 | 1.001/1.055 |
| Spatial | 0.03559/0.03555 | 0.07999 | 1.003/1.007 | 63.8 | 1.106/0.990 |
| Spatiotemporal | 0.04115/0.04103 | 0.07915 | 1.359/1.347 | 47.1 | 1.062/1.043 |

Temporal reuse lowers per-frame variance by only 2.9% relative to initial-only,
but the frames' correlation more than doubles the variance of their mean:
64 temporal frames carry about 27 frames of independent information. The
inflation alone accounts for the penalty: initial RMS times the square root of
the frame-variance ratio and inflation, `0.03563 * sqrt(0.07817 / 0.08053 * 2.340)`,
gives 0.0537, against 0.05355 measured. Combined reuse is between, with
inflation 1.36. Spatial-only reuse is uncorrelated in time and matches initial-only.

No mode shows detectable bias. Normalized MSE spans 0.990–1.106, the same
range as unbiased NEE/MIS (1.009–1.103) between seeds. The largest excess MSE,
3.1e-6, is in NEE/MIS and spatial runs; its square root, 0.0018, is about 1% of
mean radiance. Squared bias explaining temporal reuse's extra prefix error would
need about `0.05355^2 - 0.03563^2 = 1.6e-3` per sequence mean, roughly 500 times
the largest observed excess. The penalty is therefore not persistent bias or
reference noise in this fixture. Bias below this method's resolution, about 1%
of mean radiance pooled over the image, is not excluded, and pooled statistics
can hide localized bias. Spatial reuse correlates neighbouring pixels, which
widens the pooled ratio's uncertainty; no confidence interval is asserted.

## History cap and depth

The cap bounds history weight against eight fresh candidates. Seed `17`, four bounces:

| Cap | Temporal prefix RMS | Temporal inflation | Temporal lag-one | Combined prefix RMS | Combined inflation |
| --- | --- | --- | --- | --- | --- |
| 32 | 0.05355 | 2.340 | 0.108 | 0.04115 | 1.359 |
| 8 | 0.04115 | 1.356 | 0.070 | 0.03761 | 1.129 |
| 2 | 0.03704 | 1.087 | 0.019 | 0.03651 | 1.060 |

Inflation falls monotonically with the cap, as expected when a retained
selection persists for more frames under a larger history weight. Even at cap 2,
temporal error remains above initial-only (0.03563), because per-frame variance
reduction (0.08008, 0.6%) is smaller than the remaining inflation. Normalized MSE
stays within 1.023–1.067 for temporal and combined reuse at caps 8 and 2. Static temporal reuse thus
provides no static convergence benefit at any measured cap in this fixture.

The one-bounce control gives nearly the same values: temporal inflation 2.340,
prefix 0.05358, normalized MSE 0.998, NEE/MIS frame variance 0.08442 against
0.08434. Interreflection in this open fixture contributes little variance, so
the [four-bounce measurement](2026-10-16-restir-convergence.md) is not
substantially broader multibounce coverage than the one-bounce measurement.

At static RMS 0.05, both cap-32 runs agree: NEE/MIS, initial, spatial and combined
modes need 64 samples, and temporal-only does not reach it within 64 frames.
GPU timings are recorded but not analysed; serial runs with validation and
dynamic clocks do not isolate reuse cost.

## Independent checks and validation

All six invocations pass with Vulkan and synchronization validation available
and zero captured messages. Reference and wavefront cap-32 runs have equal
sequence ensembles, temporal residuals, regional and convergence populations,
and all 85 matched PFM pairs are byte identical. Rereading the saved ensemble
and reference PFMs reproduces all 30 static grand mean MSEs within 2e-5
relative tolerance. The reducer accepts both cap-32 runs, validating each
decomposition identity, ensemble error and reference noise.

`lotus-renderer-restir-statistics` compares the decomposition with an independent
two-pass definition and checks frozen, one-frame, unequal and mismatched sequences.
The benchmark CTest variants check the field's presence, populations, null moving
values and zero reference noise for the exact integral. Two new reducer tests
cover retention, summaries, frozen ensembles and rejected inconsistencies.

The managed core build and all 44 tests pass in 820.57 seconds, followed by
strict renderer-evidence validation. The runtime-free build/test records 31
passes and two explained GPU SKIPs. All 48 Python tests and documentation
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

This settles the static penalty's cause in this fixture: it is reduced information
from temporally correlated reservoirs, not detectable bias or finite-reference
error. It does not establish unbiasedness in moving scenes, localized artifacts,
ghosting, behavior in production multibounce scenes, or a reuse speedup.
Remaining LOTUS-RESTIR-05 items stay in the [execution plan](../roadmap/current.md#execution-order).
No phase exit is claimed.
