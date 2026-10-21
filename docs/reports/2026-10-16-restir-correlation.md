# ReSTIR temporal residual diagnostics

Measured 2026-10-16 on Windows, MSVC 19.51.36256.0 Release, Vulkan 1.4.329,
NVIDIA RTX A5000. Source base: `2b2e9cb`; the measured build adds headless
temporal residual statistics and reducer validation. Transport shaders and
reservoir settings are unchanged. The
[reservoir reference](../reference/RESTIR.md#headless-reservoir-comparison)
owns the metric definitions and limits.

## Change and independent checks

Frame-difference RMS alone cannot distinguish lower noise from persistent
error. Each independent sequence now records its time-mean residual RMS,
centered residual RMS and centered lag-one sample autocorrelation at fixed
screen coordinates. A frozen residual has undefined correlation (null), while
its mean error remains visible. Streaming moments need storage proportional
to image size. Warmup and sequence boundaries do not enter the statistics.

The runtime-free arithmetic test compares streaming results against an
independent two-pass definition on a nonsymmetric multichannel sequence,
including a large constant offset. Frozen errors, alternating errors and a
trend check the endpoint convention and zero-variance behavior. Invalid input
does not modify accumulated state.

The repeated-report reducer retains run/sequence boundaries and nulls. It
validates each sequence's decomposition of fresh-frame mean squared error
into squared mean RMS plus squared centered RMS. Static sequence means must
also match final convergence populations. Missing/invalid populations,
correlations, mixed legacy/new measurements and inconsistent decomposition
reject. Older reports remain supported; all historically reported fields of
the prior distant-light budget summary reproduce exactly at its original
thresholds, with newer fields added separately.

## Procedure

Eight serial invocations cover both integrators, both workload suites and
two independent measured seeds per group. Every invocation uses 32x32,
64 analytic lights, four 32-frame sequences, four warmup frames, eight
candidates, history cap 32, radius 1 and power proposals. Radiance reuse is
disabled. Each finite NEE/MIS reference half has 128 spp. Measured seeds are
17 and 262165, with disjoint sequence ranges; reference ranges remain shared
between invocations. No physical-GPU tests overlap these measurements.

Run this command for each integrator (`reference`, `wavefront`), workload
(`distant`, `mixed-boundaries`) and seed, using distinct output paths:

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark-restir build/restir-correlation-reference-distant-0.json --benchmark-label "2b2e9cb plus temporal residual metrics; Release; 2026-10-16" --benchmark-integrator reference --restir-workload distant --benchmark-size 32 --benchmark-frames 32 --benchmark-warmup 4 --restir-sequences 4 --restir-lights 64 --restir-reference-samples 128 --restir-candidates 8 --restir-history-cap 32 --restir-seed 17
python scripts/restir_budget.py build/restir-correlation-reference-distant-0.json build/restir-correlation-reference-distant-1.json --rmse 0.2 0.1 0.05 --output build/restir-correlation-reference-distant-summary.json
```

The [summary JSON](2026-10-16-restir-correlation.json) retains per-sequence
populations, settings, build and device identity, reference discrepancies and
observed budgets for all four groups. Raw JSON, PFM images and logs remain
under `build/restir-correlation-` prefixes. The comparison consumer is
`build/verify-restir-correlation.py`.

## Results

All eight invocations pass with Vulkan/synchronization validation available
and zero captured messages. All sequence decompositions and static
convergence identities pass reducer validation. Across integrators, all
40 matched case pairs have exactly identical quality, temporal residual,
memory and history populations; all 212 matched PFM pairs are byte identical.
These matched integrator draws do not provide additional independent trials.

The table gives arithmetic means over eight sequence entries (two
invocations, four sequences each). Both integrators produce the same values.
Mean error is the RMS of each sequence's time-mean RGB residual, rather than
an error after pooling sequences. The distant static reference is an exact
Lambert integral; the mixed static reference is finite-spp NEE/MIS.

| Mode | Distant static lag-one | Distant static mean error | Mixed static lag-one | Mixed static mean error |
| --- | --- | --- | --- | --- |
| NEE/MIS | -0.029693 | 0.342785 | -0.032528 | 0.048111 |
| Initial | -0.031112 | 0.248558 | -0.033342 | 0.080263 |
| Temporal | 0.684874 | 0.644920 | 0.594550 | 0.093281 |
| Spatial | -0.031029 | 0.236628 | -0.033968 | 0.039248 |
| Spatiotemporal | 0.219616 | 0.373658 | 0.362595 | 0.033848 |

For static distant lights, temporal-only reuse reduces frame-difference RMS
from initial-only's 2.000474 to 0.870567, while its time-mean error increases
from 0.248558 to 0.644920. Static mixed combined reuse has positive measured
correlation but a lower time-mean error than the other measured modes.
Correlation must therefore be read alongside error and convergence, rather
than treated as a quality ranking by itself.

Moving residuals subtract each pose's finite reference and stay at fixed
screen coordinates. The same descriptive sequence averages are:

| Workload | Mode | Lag-one | Mean residual error |
| --- | --- | --- | --- |
| Distant moving shadow | Temporal | 0.579677 | 0.618645 |
| Distant moving shadow | Spatiotemporal | 0.210170 | 0.378219 |
| Mixed moving boundaries | Temporal | 0.235780 | 0.079604 |
| Mixed moving boundaries | Spatiotemporal | 0.207409 | 0.031514 |

These are finite-sequence sample autocorrelations with component means
estimated from those same frames. Their values are not unbiased population
correlations or effective sample counts. Moving surfaces/disocclusion and
finite reference noise also affect them. Static mixed reference-half split
RMS is 0.032915; its shared noise remains in time-mean error. Two invocations
and correlated frames do not establish bias bounds or temporal stability.

## Validation and scope

The managed core build and all 36 tests pass in 500.52 seconds, followed by
strict renderer-evidence validation with completion-bound producers. This
includes synchronization, reference images, install-tree checks, both
integrators' workload/proposal benchmarks and explained missing-driver cases.
The runtime-free build/test has 23 passes and two explained GPU SKIPs.
All 45 Python tests and documentation validation pass.

```sh
ost build --jobs auto
ost test
ost validate --strict-renderer-evidence
ost build --without-runtime --intent ci-core --jobs auto
ost test --without-runtime --intent ci-core
python -m unittest discover -s scripts -p 'test_*.py'
python scripts/check_docs.py
```

Hydra and its release gate were not rerun for this headless measurement
change. This step makes temporal correlation and persistent residual error
observable within LOTUS-RESTIR-05; it does not reduce them or demonstrate a
speedup. Broader artifact/bias coverage, reuse cost/correlation improvements,
scope review and the release gate remain in the
[execution plan](../archive/pre-v070-execution-plan.md#execution-order).
