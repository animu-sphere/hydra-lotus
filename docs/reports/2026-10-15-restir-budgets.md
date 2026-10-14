# Repeated ReSTIR quality budgets

Measured 2026-10-15 against `7f0f6ab` plus the seed/budget harness change,
Windows, MSVC 19.51 Release, Vulkan/Slang, NVIDIA RTX A5000. All six measurement
invocations complete successfully with Vulkan/synchronization validation
available and zero captured messages. The [comparison contract](../reference/RESTIR.md#headless-reservoir-comparison)
owns workloads and measurement scopes; the [budget contract](../reference/RESTIR.md#repeated-quality-budgets)
owns attainment and paired timing semantics.

## Procedure and independent verification

Both integrators run three serial invocations each, with seeds 17, 262165 and
524313. Every invocation measures the two versioned many-distant-light scenes
at 64x64, with 256 lights, four independent 64-frame sequences, four warmup
frames, candidate counts 1 and 8, history cap 32, spatial radius 1 and the
default power proposal. Each independent NEE/MIS reference half uses 256 spp.
Radiance reuse and denoising are disabled. Compared with the earlier
[comparison](2026-10-14-restir-comparison.md), this doubles lights and sequence
length and adds independent invocation repetitions.

Modes share sample indices within an invocation; invocations have disjoint
measured ranges. Each integrator has twelve independent sequences, not 768
independent frames for recursively reused modes. The two integrators use the
same stochastic draws and are not extra independent quality replications.
The label is `7f0f6ab + repeated quality budgets; core Release; 2026-10-15`.
Invocation durations are 90/85/81 seconds for reference and 83/83/80 for
wavefront, including renderer setup, references, measurements and output.
No other repository GPU test runs during these measurements.

The [summary JSON](2026-10-15-restir-budgets.json) retains all budgets at
prespecified RMS thresholds 0.5, 0.35 and 0.25, run seeds, reference checks,
timing medians, memory capacities and across-invocation error populations.
Raw benchmark reports, populations and images remain under
`build/restir-budgets-<integrator>-<run>` prefixes; the reducer outputs use
`build/restir-budgets-<integrator>-summary.json`.

An independent standard-library Python reader recomputes RMS, signed RGB means
and maximum absolute 8x8 tile mean errors from all 108 saved ensemble PFMs.
These agree with the original JSON within absolute 2e-6 after float PFM
rounding. It also checks raw population finiteness, means, medians and p95s.
All 303 corresponding PFM pairs are bit identical between integrators,
including reference, last-frame and ensemble images. Raw per-sequence
convergence RMS and cumulative-time population consistency is checked by the
reducer before generating budgets.

The independent static Lambert integral is
`(2.2309669, 2.9384697, 3.9149382)`. The saved static NEE/MIS reference has RMS
0.087535 to that integral and split-half discrepancy 0.174633. Static quality
budgets use the exact integral, not this noisy reference. Moving references
retain finite-spp noise and are shared across invocations.

## Static convergence and observed budgets

Final prefix RMS ranges below span three invocation-level RMS estimates, each
computed over four separate CPU-averaged 64-frame sequences. Observed budget
attainment requires staying below the threshold at every subsequent measured
checkpoint. There is no interpolation between checkpoints or claim beyond
64 frames. Speedup is paired NEE/MIS cumulative scene GPU time divided by the
case's time at RMS 0.35; values above one are faster. Dashes mean no invocation
reaches that target, not zero cost.

| Mode / candidates | 64-frame prefix RMS range | Samples to RMS 0.35 | Reference speedup range | Wavefront speedup range |
| --- | --- | --- | --- | --- |
| NEE/MIS / 0 | 0.2447–0.2471 | 32 | 1.00 | 1.00 |
| Initial / 1 | 0.2462–0.2475 | 32 | 1.33–1.61 | 0.98–1.09 |
| Temporal / 1 | 0.9551–0.9614 | — | — | — |
| Spatial / 1 | 0.1861–0.1888 | 32 | 0.16–0.27 | 0.19–1.14 |
| Combined / 1 | 0.4508–0.4686 | — | — | — |
| Initial / 8 | 0.1793–0.1795 | 32 | 0.65–0.80 | 0.58–0.64 |
| Temporal / 8 | 0.4847–0.4874 | — | — | — |
| Spatial / 8 | 0.1696–0.1707 | 16 | 0.22–0.37 | 0.31–0.43 |
| Combined / 8 | 0.2700–0.2760 | 64 | 0.055–0.072 | 0.064–0.104 |

One initial candidate preserves NEE/MIS-like convergence here, with lower
reference-integrator time but no clear wavefront improvement. More initial
candidates and spatial-only reuse lower error per frame, while their measured
GPU work offsets that gain at this threshold. Temporal-only and combined
reuse retain the slower prefix convergence seen in earlier measurements;
lower fresh-frame differences do not imply faster accumulated convergence.
The complete three-threshold attainment records, including missing budgets,
remain in the JSON rather than averaging only successful cases.

GPU time includes all reservoir and shared-metadata passes and excludes scene
updates/readback. Three invocations, small kernels, validation and dynamic
clocks limit speed conclusions. The wavefront spatial-one-candidate budget
varies from 6.23 to 37.26 ms despite identical convergence samples/settings;
its single paired ratio above one does not establish a repeatable gain.
No fixed-clock or production equal-quality speedup is established.

## Moving errors and local artifacts

Moving poses have no static convergence budget. Fresh/residual-delta values
below are ranges of invocation means over 256 fresh frames and 252
within-sequence transitions. Tile errors use each invocation's final-pose
ensemble over four independent sequences, against the shared NEE/MIS image.
Identical integrator images give identical quality ranges.

| Mode / candidates | Fresh RMS range | Residual-delta RMS range | Maximum tile mean error range |
| --- | --- | --- | --- |
| NEE/MIS / 0 | 2.0079–2.0105 | 2.8386–2.8428 | 0.3761–0.5021 |
| Initial / 8 | 1.4990–1.4993 | 2.1203–2.1211 | 0.2483–0.3503 |
| Temporal / 8 | 1.4229–1.4273 | 1.0695–1.0719 | 0.2495–0.2825 |
| Spatial / 8 | 1.4290–1.4301 | 2.0208–2.0241 | 0.3279–0.4532 |
| Combined / 8 | 1.4125–1.4148 | 1.6972–1.6976 | 0.3869–0.5638 |
| Combined / 1 | 1.4142–1.4251 | 1.5549–1.5635 | 0.4987–0.8036 |

Temporal reuse reduces fresh frame differences, while combined reuse can
retain larger local final-pose errors. The largest absolute signed image-mean
channel error across all runs/cases is 0.059166; this alone would hide those
tile errors. These are finite-sample observations, not estimator bias bounds.
Repeated noisy references are not independent reference replications, and
these two parallel-light fixtures do not establish production ghosting or
area-light/environment/material-boundary behavior.

## Validation

The build and targeted physical-GPU benchmark checks pass 7/7, covering both
integrators, all proposals, seed selection/populations and the missing-driver
case. Runtime-free build/test passes 20 checks with two explained GPU SKIPs.
The Python suite passes 38 tests, including independent budget fixtures,
temporal recrossing, heterogeneous RMS arithmetic, overlapping seeds, mismatched
conditions, missing timestamps/attainment and invalid-input output preservation.
Documentation validation passes.

The full managed core suite passes 33/33 in 450.25 seconds after all
measurements. Strict renderer-evidence validation passes with completion-bound
build/test producers. Hydra/viewport/package evidence retains its explained
configuration SKIPs. This harness change does not rerun the Hydra host suite;
it changes neither Hydra translation nor renderer estimator behavior.

```sh
ost build --jobs auto
ctest --test-dir build/cy2026-windows-x86_64-py313-core -C Release -R restir-benchmark --output-on-failure
ost build --without-runtime --intent ci-core --jobs auto
ost test --without-runtime --intent ci-core
ost test
ost validate --strict-renderer-evidence
python scripts/check_docs.py
python -m unittest discover -s scripts -p 'test_*.py'
```

The exact measurement/reduction procedure is in the
[build guide](../guides/BUILDING.md#reservoir-comparison). Logs remain under
`build/restir-budgets-` prefixes: `build.log`, `targeted.log`,
`runtime-free-build.log`, `runtime-free-test.log`, `measurement.log`, `test.log`
and `validate.log`. The independent PFM consumer is retained as
`build/verify-restir-budgets.py`.

## Scope disposition

This adds reproducible repeated quality budgets for LOTUS-RESTIR-05 and
confirms that temporal correlation and reuse cost persist at a longer horizon.
It changes the measurement harness, not the estimator or its correction.
Lower-cost reuse/history policies, broader light/material and bias/artifact
coverage, the scope review and the release gate remain in the
[execution plan](../roadmap/current.md#execution-order).
