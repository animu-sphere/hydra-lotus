# Reservoir candidate proposal alternatives

Measured 2026-10-14–15 against `3daf01e` plus this candidate-proposal change,
Windows, MSVC 19.51 Release, Vulkan/Slang, NVIDIA RTX A5000.
The [reservoir reference](../reference/RESTIR.md#initial-gpu-lighting) owns
selection probabilities, target measure, lifecycle and implementation limits.
The [headless comparison contract](../reference/RESTIR.md#headless-reservoir-comparison)
owns workloads, metrics, seeds and CLI ranges.

## Arithmetic and integration

Power selection remains the default. Uniform and equal power/uniform mixture
selection use marginal proposal densities while retaining the same light
payload and parameter-space target. Mixture weights include both components,
regardless of which component produced a draw. No extra variate is consumed;
primary coverage and continuation sampling remain separate.

The GPU test independently enumerates two parallel lights with power ratio
1:3. All 4096 initial records match the CPU streaming oracle in selected slot,
payload, count, target, marginal density, weight sum and normalized RGB under
every proposal and both integrators. A 256-sample accumulation agrees with
the independent Lambert integral within absolute 0.002 per channel.
Changing proposal restarts progressive accumulation and both reservoir and
radiance history; unchanged settings resume both histories. An invalid enum
rejects without changing accumulated samples.

Uniform and mixture tests also add a weaker parallel light to rect, disk,
sphere, positive-angle distant, emissive-mesh, environment-image and constant
environment fixtures. Unequal table probabilities exercise the separation of
proposal density from reconstruction Jacobians. Independent quadrature and
analytic integrals, and 1024-spp NEE/MIS comparisons, use the existing relative
2% plus absolute 0.0001 tolerance under both integrators. Six-frame spatial
and combined-reuse checks retain the CPU merge/support oracle with zero-target
draws, capped sources and explicit corrected normalization.

## Matched many-light measurements

Six serial invocations measure all three proposals under both integrators.
Each uses the two versioned 128-light workloads, 32x32 targets, four independent
16-frame sequences, two warmup frames, candidates 1 and 8, history cap 32,
radius 1 and 128 samples per independent NEE/MIS reference half. Radiance
accumulation and denoising are disabled. Seeds remain `17 + 65537 * sequence`,
with the reference ranges defined by the comparison contract. The label is
`3daf01e + candidate proposals; core Release; 2026-10-15`.

The [summary JSON](2026-10-15-restir-proposals.json) retains procedure, build,
device, quality/memory summaries, timing summaries and static convergence
checkpoints. Full raw populations and images remain under
`build/restir-proposals-<integrator>-<proposal>` prefixes. All six runs finish
successfully with synchronization validation available and zero captured
messages. No other repository GPU test runs during these measurements.

An independent Python consumer recomputes ensemble RGB RMS and signed means
from every PFM, agreeing with JSON within 2e-6. It also checks finite raw
populations and their means. All 53 PFM pairs per proposal are bit identical
between integrators. NEE/MIS case images and all reference images remain bit
identical across proposals. These shared deterministic images are not extra
independent stochastic replications.

The static reference's error to the independent Lambert integral is 0.12459;
its half-to-half RMS discrepancy is 0.24661. The moving reference's mean
discrepancy is 0.25013. Moving errors retain reference noise. Static prefix RMS
below is measured against the independent integral, over the four separately
averaged 16-frame sequences. Moving fresh and residual-delta RMS are means
over 64 fresh frames and 60 within-sequence transitions.

| Proposal / mode (8 candidates) | Static prefix RMS | Moving fresh RMS | Moving residual delta RMS |
| --- | --- | --- | --- |
| Power / initial | 0.3560 | 1.4927 | 2.1057 |
| Uniform / initial | 0.3670 | 1.5306 | 2.1607 |
| Mixture / initial | 0.3608 | 1.5004 | 2.1179 |
| Power / temporal | 0.8393 | 1.4304 | 1.2143 |
| Uniform / temporal | 0.8585 | 1.4410 | 1.2178 |
| Mixture / temporal | 0.8454 | 1.4287 | 1.2134 |
| Power / spatial | 0.3358 | 1.4280 | 2.0159 |
| Uniform / spatial | 0.3397 | 1.4389 | 2.0357 |
| Mixture / spatial | 0.3399 | 1.4301 | 2.0210 |
| Power / combined | 0.5005 | 1.4051 | 1.7175 |
| Uniform / combined | 0.5130 | 1.4103 | 1.7241 |
| Mixture / combined | 0.5034 | 1.4016 | 1.7183 |

NEE/MIS has static prefix RMS 0.4883, moving fresh RMS 1.9883 and residual
delta RMS 2.8093. Power selection has the lowest observed initial-only error
in this fixture; mixture is close, with no uniform/mixture convergence gain
established. Changing proposals does not resolve temporal correlation: all
temporal-only and combined prefix errors remain worse than their respective
initial-only estimates, despite lower fresh-frame differences. The maximum
absolute signed image-mean channel error over all cases is 0.11710. Four
sequences, short histories and noisy moving references do not establish a
recursive-reuse bias bound or production artifact behavior.

Initial-only eight-candidate scene GPU medians, milliseconds, include all
reservoir work and exclude updates/readback:

| Proposal | Reference | Wavefront |
| --- | --- | --- |
| Power | 0.1458 | 0.3531 |
| Uniform | 0.2083 | 0.3483 |
| Mixture | 0.1915 | 0.3543 |

All reuse timings remain in the JSON. These are one run per integrator/proposal,
small kernels with validation enabled, without fixed clocks or repeated budgets.
Even unchanged NEE/MIS images have wavefront medians ranging from 0.0757 to
0.2605 across the three runs. Timing differences do not establish a proposal
speedup or an equal-quality advantage. Reservoir and shared-metadata capacities
are identical across proposals at a fixed mode/target; selection adds no new
per-pixel allocation.

The reference-integrator uniform invocation, from the repository root:

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark-restir build/restir-proposals-reference-uniform.json --benchmark-label "3daf01e + candidate proposals; core Release; 2026-10-15" --benchmark-integrator reference --benchmark-size 32 --benchmark-frames 16 --benchmark-warmup 2 --restir-sequences 4 --restir-lights 128 --restir-reference-samples 128 --restir-candidates 1,8 --restir-proposal uniform
```

Other runs substitute proposal `power-alias` or `power-uniform`, and integrator
`wavefront`, with distinct matching output paths.

## Validation

The runtime-free managed build/test succeeds: 20 checks pass, with two
explained GPU SKIPs. Core physical-GPU reservoir, proposal benchmark and
missing-driver checks pass 9/9 in 348.36 seconds. The full managed Hydra suite
passes 60/60 in 661.70 seconds, including installed usdview, both integrators'
proposal regressions, deterministic images, install-tree rendering and
synchronization validation. Strict Hydra renderer-evidence validation passes
with completion-bound build/test producers. These are correctness runs;
overlapping test execution is not used as performance evidence.

Documentation validation and 27 Python checker regressions pass. Logs remain
under `build/restir-proposals-` prefixes (`build.log`, `targeted.log`,
`runtime-free-build.log`, `runtime-free-test.log`, `hydra-build.log`,
`hydra-test.log`, `hydra-validate.log`).

Commands run for validation:

```sh
ost build --jobs auto
ctest --test-dir build/cy2026-windows-x86_64-py313-core -C Release -R restir --output-on-failure
ost build --without-runtime --intent ci-core --jobs auto
ost test --without-runtime --intent ci-core
ost build --profile lookdev --intent hydra --jobs auto
ost test --profile lookdev --intent hydra
ost validate --profile lookdev --intent hydra --strict-renderer-evidence
python scripts/check_docs.py
python -m unittest discover -s scripts -p 'test_*.py'
```

## Scope disposition

This implements alternative proposals for LOTUS-RESTIR-05 through the core
and headless harness. Hydra continues to select the default power proposal.
It does not introduce a light hierarchy or change the reused estimator's
support correction. Broader/repeated equal-quality budgets, temporal
correlation, reuse cost, production artifact/bias coverage and the release
gate remain in the [execution plan](../archive/pre-v070-execution-plan.md#execution-order).
