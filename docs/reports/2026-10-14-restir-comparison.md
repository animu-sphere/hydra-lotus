# Headless many-light reservoir comparison

Measured 2026-10-14 against `49f8d79` plus this headless comparison change,
Windows, MSVC 19.51 Release, Vulkan/Slang, NVIDIA RTX A5000. Vulkan and
synchronization validation were available, with zero captured messages.
The [reservoir reference](../reference/RESTIR.md#headless-reservoir-comparison)
owns the CLI, workload geometry, RNG ranges, metric definitions and limits.
The [summary JSON](2026-10-14-restir-comparison.json) retains build/device,
procedure, all case summaries and static convergence checkpoints. Raw timing
populations and PFMs remain under `build/restir-comparison-` prefixes.

## Procedure and independent checks

Both versioned workloads use 128 parallel lights with varying colour and
direction, a 64x64 target, one scattering bounce and one fresh sample/frame.
Each case runs four independent 32-frame sequences after four warmup frames,
under each integrator. The sweep compares NEE/MIS with initial, temporal,
spatial and combined reuse at 1, 2 and 8 candidates, source cap 32 and radius 1.
Each case has its own renderer. No temporal radiance accumulation or denoising
is enabled. Sequence `s` starts at `17 + 65537 s`; modes share these indices.

The static Lambert integral is `(2.2284428, 2.9188582, 3.9083482)`. Its saved
NEE/MIS reference has pixel RGB RMS error 0.08727 to that integral. Each pose's
reference averages two disjoint 256-spp NEE/MIS halves under the reference
integrator, starting at `2^24 + 8192 frame + 4096 half`. The static half-to-half
RMS discrepancy is 0.17209; the moving mean discrepancy is 0.17624. Reference
noise is retained in moving comparisons and is not subtracted from error.

The reference invocation took 85 seconds and wavefront 84 seconds, including
reference generation and renderer creation. All 85 PFM pairs are bit identical
between integrators, including reference, fresh and ensemble images. These
are shared sampling/transport results, not independent stochastic replications.
An independent Python consumer recomputed ensemble RMS and signed RGB mean
error from every saved ensemble PFM and checked raw population means, finiteness
and memory totals. The RMS/mean reducers agree within 2e-7 after PFM rounding.

## Static convergence and temporal correlation

Fresh error and frame delta are averages over 128 fresh images and 124
within-sequence transitions. Prefix RMS at 32 samples is the square root of
the mean squared pixel RGB error over four separately averaged sequences,
against the independent integral. Times are scene GPU medians in milliseconds,
already including metadata and reservoir passes. They exclude uploads/readback.

| Mode / candidates | Fresh RMS | Frame-delta RMS | 32-sample prefix RMS | Reference GPU ms | Wavefront GPU ms |
| --- | --- | --- | --- | --- | --- |
| NEE/MIS / 0 | 1.9531 | 2.7619 | 0.3440 | 0.1685 | 0.0772 |
| Initial / 1 | 1.9540 | 2.7614 | 0.3446 | 0.1263 | 0.2733 |
| Temporal / 1 | 1.4342 | 0.5778 | 1.0274 | 0.3559 | 0.7451 |
| Spatial / 1 | 1.4888 | 2.1031 | 0.2632 | 0.7524 | 0.8804 |
| Combined / 1 | 1.3629 | 1.4992 | 0.5719 | 0.9181 | 1.0340 |
| Initial / 2 | 1.6778 | 2.3703 | 0.2959 | 0.1826 | 0.3233 |
| Temporal / 2 | 1.3912 | 0.5997 | 0.9664 | 0.5298 | 0.7363 |
| Spatial / 2 | 1.4183 | 2.0051 | 0.2498 | 0.7792 | 0.9166 |
| Combined / 2 | 1.3536 | 1.5289 | 0.5093 | 0.8449 | 1.0369 |
| Initial / 8 | 1.4341 | 2.0276 | 0.2540 | 0.1861 | 0.3671 |
| Temporal / 8 | 1.3570 | 0.8850 | 0.6558 | 0.6071 | 0.7630 |
| Spatial / 8 | 1.3630 | 1.9271 | 0.2408 | 0.8097 | 0.9844 |
| Combined / 8 | 1.3435 | 1.6048 | 0.3820 | 0.7864 | 1.0705 |

More fresh candidates improve the initial estimator's measured convergence.
Temporal reuse lowers fresh error and frame differences but preserves sample
correlation: at one candidate its 32-frame prefix error is almost three times
the NEE/MIS prefix error. Combined reuse also converges more slowly than
initial-only at each candidate count in this fixture. Spatial-only reduces
the prefix error but requires more GPU work.

At eight candidates, the reference integrator's initial-only prefix reaches
RMS 0.2540 in mean cumulative GPU time 6.697 ms, versus NEE/MIS RMS 0.3440
in 5.348 ms. Wavefront reaches the same two errors in 11.740 and 3.498 ms.
These are unequal quality/time points, not an equal-quality speedup. The full
checkpoint curves and raw timing populations must be used for later budgets.
One run per integrator, small kernels, dynamic clocks and validation overhead
limit timing conclusions; no fixed-clock or repeatable speedup is established.

## Moving shadows and finite-sample artifacts

The camera and opaque panel move sinusoidally through identical poses in each
independent sequence. Fresh RMS uses each pose's NEE/MIS reference. Residual
frame delta subtracts the reference's frame difference before taking RGB RMS.
Final ensemble RMS averages only the final pose across four sequences; averaging
different poses would smear motion and is deliberately not used as convergence.

| Mode / candidates | Fresh RMS | Residual frame-delta RMS | Final ensemble RMS | Reference GPU ms | Wavefront GPU ms |
| --- | --- | --- | --- | --- | --- |
| NEE/MIS / 0 | 1.9901 | 2.8140 | 0.9951 | 0.0492 | 0.2459 |
| Initial / 1 | 1.9908 | 2.8125 | 0.9991 | 0.1251 | 0.2508 |
| Temporal / 1 | 1.5147 | 0.9959 | 0.7456 | 0.6383 | 0.7937 |
| Spatial / 1 | 1.5484 | 2.1875 | 0.7782 | 0.7311 | 0.9333 |
| Combined / 1 | 1.4339 | 1.6127 | 0.7164 | 0.8883 | 1.0403 |
| Initial / 2 | 1.7259 | 2.4376 | 0.8725 | 0.1887 | 0.3611 |
| Temporal / 2 | 1.4613 | 0.9724 | 0.7141 | 0.6584 | 0.6324 |
| Spatial / 2 | 1.4806 | 2.0930 | 0.7320 | 0.8084 | 0.9755 |
| Combined / 2 | 1.4245 | 1.6439 | 0.7203 | 0.9106 | 1.0061 |
| Initial / 8 | 1.4950 | 2.1135 | 0.7435 | 0.2406 | 0.4035 |
| Temporal / 8 | 1.4263 | 1.1260 | 0.7151 | 0.6330 | 0.8101 |
| Spatial / 8 | 1.4283 | 2.0191 | 0.7195 | 0.8000 | 0.9227 |
| Combined / 8 | 1.4100 | 1.7098 | 0.7094 | 0.9890 | 1.0824 |

Temporal reuse reduces residual frame changes substantially; adding spatial
reuse increases them relative to temporal-only. Gains above two candidates
are modest for the reuse modes here. Every measured sequence retained expected
history availability after its first frame; local rejections still fall back
to fresh lighting. The maximum absolute signed image-mean channel error over
all static/moving cases is 0.07253. This is a finite-sample observation, not a
bias bound. In the moving eight-candidate cases, maximum absolute 8x8 tile
mean errors are 0.2784 initial, 0.2787 temporal, 0.3384 spatial and 0.4667
combined, versus 0.5222 for NEE/MIS. Global means alone would hide these local
differences. Four sequences and a noisy reference are insufficient to separate
recursive estimator bias from correlated sampling artifacts.

The final-pose ensemble/absolute-error contact sheet was visually inspected
from `build/restir-comparison-preview.png`. The reference's broad soft panel
shadow is visible; all low-sample modes retain noise and the combined output
has larger local colour patches. This fixture does not establish production
ghosting or disocclusion limits. Area lights, environments, indirect transport,
stronger surface/material boundaries and proposal alternatives need separate
coverage before exit review.

## Memory and reproducibility

Fresh renderer capacity at 4096 pixels, excluding scene pools and attachments:

| Mode | Reservoir device + host bytes | Shared metadata device + host bytes |
| --- | --- | --- |
| NEE/MIS | 0 | 0 |
| Initial | 393216 | 0 |
| Temporal | 3538944 | 1180016 |
| Spatial / combined | 5242880 | 1180016 |

Candidate counts do not change these record capacities. Each static case
reserves 20971520 scene-pool bytes; these include host-visible backing and are
not total VRAM. Target, accumulation, constants outside the reported groups,
driver allocations and process VRAM remain outside the comparison.

Commands run from the repository root:

```sh
ost build --jobs auto
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark-restir build/restir-comparison-reference.json --benchmark-label "49f8d79 + headless reservoir comparison; core Release"
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark-restir build/restir-comparison-wavefront.json --benchmark-label "49f8d79 + headless reservoir comparison; core Release" --benchmark-integrator wavefront
ctest --test-dir build/cy2026-windows-x86_64-py313-core -C Release -R restir-benchmark --output-on-failure
ost build --without-runtime --intent ci-core --jobs auto
ost test --without-runtime --intent ci-core
ost test
ost validate --strict-renderer-evidence
python scripts/check_docs.py
python -m unittest discover -s scripts -p 'test_*.py'
```

The targeted physical-GPU checks pass 3/3: both integrators' report/workload,
independent static means, history, memory, populations/images and rejected CLI
contracts, plus the missing-driver report. Output-failure preservation is also
checked before GPU setup. Runtime-free checks pass 16 with two explained GPU
SKIPs; the two new report tests exercise their unavailable-capability path.
Documentation checks and 27 Python regressions pass.

The final managed core suite passes 29/29 in 287.49 seconds, including existing
temporal/reservoir GPU oracles, deterministic images, installed rendering,
benchmark regressions and synchronization checks. Strict renderer-evidence
validation passes with completion-bound build/test producers. Hydra/viewport
and package checks retain their explained configuration SKIPs. No Hydra adapter
or backend boundary changed, so the Hydra suite was not rerun for this harness.
Logs use `build/restir-benchmark-` prefixes (`build.log`, `targeted.log`,
`test.log`, `validate.log`, `runtime-free-build.log`, `runtime-free-test.log`).

The installed binary also completes the new CLI with a 16x16 target, three
frames, zero warmup, two sequences, eight lights, 16 samples/reference half
and candidate count one. Both workloads/all five cases pass, and the static
checkpoints are 1/2/3, covering a final count that is not a power of two.
Its command/output are retained as `build/restir-installed-comparison.json`
and `build/restir-benchmark-installed.log`; this is an installed CLI check,
not an additional performance measurement.

## Scope disposition

This supplies a reproducible convergence/time/stability/memory/error comparison
and a candidate-count evaluation for LOTUS-RESTIR-05. It changes no renderer
sampling, visibility, reuse arithmetic, Hydra setting or default estimator.
NEE/MIS remains the default. The power-alias proposal is retained; alternative
candidate proposals are not evaluated. Broader and repeated equal-quality
measurements, production artifact/bias checks, the phase scope review and
release gate remain in the [execution plan](../archive/pre-v070-execution-plan.md#execution-order).
No Renderer Phase 5 exit is declared by these measurements.
