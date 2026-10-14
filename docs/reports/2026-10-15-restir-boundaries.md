# Mixed-light ReSTIR boundary comparisons

Measured 2026-10-15 against `0aeeb5d` plus the mixed boundary workload change,
Windows, MSVC 19.51 Release, Vulkan/Slang, NVIDIA RTX A5000. The
[comparison contract](../reference/RESTIR.md#headless-reservoir-comparison)
owns the workloads and metrics; the
[budget contract](../reference/RESTIR.md#repeated-quality-budgets) owns
attainment and paired timing semantics.

## Procedure

Both integrators run three serial invocations with seeds 17, 262165 and
524313. Every invocation selects `mixed-boundaries` at 32x32 with 64 analytic
lights, four independent 32-frame sequences, four warmup frames, candidate
counts 1 and 8, history cap 32, spatial radius 1 and the default power proposal.
Each NEE/MIS reference half uses 512 spp, with the reference integrator even
when measuring wavefront. The label is
`0aeeb5d + mixed boundary workloads; core Release; 2026-10-15`.
The exact measurement/reduction commands are in the
[build guide](../guides/BUILDING.md#reservoir-comparison).

The static scene contains alternating rect/disk lights and a procedural HDR
environment, a red Lambert panel beside a blue GGX metal panel, and a depth
and geometric-normal step. The moving scene adds camera motion and a shaded
moving blocker. One scattering bounce isolates direct lighting; radiance reuse
and denoising are disabled. The central quarter of columns forms the fixed
boundary region, containing 256 pixels at this resolution.

Each integrator has twelve independent measured sequences. Frames in a reused
sequence are correlated, and matched integrator images are not independent
quality replications. Modes share measured sample indices; invocation ranges
are disjoint. References use separate ranges but repeat the same reference
seeds across invocations. No GPU tests or benchmarks overlap these runs.

Both workloads use `finite-spp-nee-mis` error references. Their exact-integral
fields are null. Static quality budgets at thresholds 0.2, 0.1 and 0.05 measure
agreement with that saved reference image, including its noise; they do not
measure error against an exact solution. Reference split discrepancies remain
in the summary. Moving frames have no static convergence budget.

## Measurements and independent verification

All six invocations pass with Vulkan/synchronization validation available and
zero captured messages. Invocation durations are 50/48/47 seconds for reference
and 46/47/44 for wavefront, including setup, references and image output. The
[summary JSON](2026-10-15-restir-boundaries.json) retains both complete reduced
reports, all three thresholds, missing budgets, run seeds, regional errors and
reference split populations. Raw reports and images remain under
`build/restir-boundaries-<integrator>-<run>` prefixes.

An independent standard-library PFM reader recomputes whole-image RMS, signed
RGB means, maximum absolute 8x8 tile mean errors, central-region RMS and signed
means for all 108 ensemble images. They agree with JSON within absolute 2e-6
after float image rounding. It also verifies finiteness and mean/median/p95
arithmetic for the raw timing/error populations. All 207 corresponding PFM
pairs are bit identical between integrators, including reference, last and
ensemble images. Thus quality populations match; timing populations differ.

The static reference half discrepancy is 0.015311. Moving discrepancies range
from 0.015636 to 0.018571 across poses. These are split-image discrepancies,
not a subtracted noise estimate or an exact-reference error bound.

## Static quality budgets

At RMS 0.1, the following observed budgets include all reservoir modes and
both candidate counts. GPU times are ranges across three invocations and are
reported only when all three reach the threshold. A dash retains a missing
budget rather than averaging only successful runs. Static prefix quality is
RMS across independent sequence means, not the lower-noise pooled ensemble.

| Mode / candidates | Reached | Samples | Reference GPU ms | Wavefront GPU ms |
| --- | --- | --- | --- | --- |
| NEE/MIS / 0 | 3/3 | 8 | 0.638–1.048 | 0.800–1.539 |
| Initial / 1 | 0/3 | — | — | — |
| Temporal / 1 | 0/3 | — | — | — |
| Spatial / 1 | 0/3 | — | — | — |
| Combined / 1 | 2/3 | 32 / 32 / — | — | — |
| Initial / 8 | 3/3 | 32 | 6.691–10.825 | 9.388–14.939 |
| Temporal / 8 | 3/3 | 32 | 15.874–26.447 | 10.177–26.737 |
| Spatial / 8 | 3/3 | 8 | 4.189–6.140 | 7.567–12.902 |
| Combined / 8 | 3/3 | 8 | 6.351–8.219 | 1.559–6.695 |

Spatial/combined reuse with eight candidates improves the final 32-frame
prefix RMS to 0.0364–0.0373 / 0.0310–0.0332, versus NEE/MIS's
0.0457–0.0467. Their additional work outweighs this improvement at RMS 0.1:
every attained reservoir-to-NEE/MIS paired speed ratio is below one under
both integrators. Combined one-candidate prefix RMS ranges from 0.0816 to
0.2193, leaving one invocation unattained. More samples and repeated reference
seeds do not establish a bias bound.

GPU times include metadata and reservoir passes, exclude scene updates and
readback, and vary with dynamic clocks and validation. Small kernels and three
invocations constrain performance conclusions; this records no equal-quality
speedup at RMS 0.1 and no fixed-clock or production result.

## Moving errors and boundary artifacts

These ranges are across invocation means over 128 fresh frames and 124
within-sequence transitions. Boundary RMS uses only the final-pose ensemble
over four independent sequences in the fixed central region. Identical
integrator images give identical quality ranges.

| Mode / candidates | Fresh RMS | Residual-delta RMS | Boundary ensemble RMS |
| --- | --- | --- | --- |
| NEE/MIS / 0 | 0.2708–0.2736 | 0.3851–0.3868 | 0.1679–0.1766 |
| Initial / 8 | 0.4503–0.4586 | 0.6430–0.6594 | 0.3060–0.3399 |
| Temporal / 8 | 0.2327–0.2458 | 0.2578–0.2879 | 0.1581–0.2103 |
| Spatial / 8 | 0.2304–0.2391 | 0.3300–0.3444 | 0.1641–0.1905 |
| Combined / 8 | 0.1098–0.1164 | 0.1280–0.1414 | 0.0748–0.0826 |
| Combined / 1 | 0.1778–0.1875 | 0.1526–0.1856 | 0.0983–0.1194 |

Combined reuse lowers these finite-sample errors here. Temporal-only reuse
reduces fresh/delta error but does not consistently improve the boundary
ensemble relative to NEE/MIS. Initial eight-candidate lighting is noisier on
this mixed-material fixture than NEE/MIS. The full populations and signed
regional means remain in JSON, including modes omitted from this compact
moving table. This small one-bounce experiment does not establish production
ghosting behavior or unbiasedness, and its central band is not a segmentation
of all disocclusion or material boundaries.

## Validation

The managed core build and its renderer smoke evidence pass. Physical-GPU
benchmark regressions pass 9/9, including both integrators, the new workload
pair, existing proposals, CLI output preservation and the missing-driver case.
Runtime-free build/test passes 22 checks with two explained temporal/reservoir
GPU SKIPs. The Python suite passes 42 tests, including finite-reference and
regional reduction, missing/invalid populations and legacy report support.
Re-reducing the three previous reference budget reports reproduces every
existing quality-budget record unchanged.

```sh
ost build --jobs auto
ctest --test-dir build/cy2026-windows-x86_64-py313-core -C Release -R restir-benchmark --output-on-failure
ost build --without-runtime --intent ci-core --jobs auto
ost test --without-runtime --intent ci-core
python -m unittest discover -s scripts -p 'test_*.py'
python scripts/check_docs.py
```

The full managed core/Hydra suites and release gate are not rerun for this
headless measurement-harness change. No backend estimator, public header,
Hydra translation or installed layout changes. Logs and raw PFM/report outputs
remain under `build/restir-boundaries-` prefixes; the independent PFM consumer
is `build/verify-restir-boundaries.py`.

## Scope

This extends LOTUS-RESTIR-05's evidence to bounded area/environment and
material/surface-boundary workloads. It changes the measurement harness and
budget reduction, without changing reservoir sampling, support correction,
visibility, history policy or Hydra controls. Lower reuse cost, temporal
correlation, broader production artifact/bias coverage, the scope review and
release gate remain in the [execution plan](../roadmap/current.md#execution-order).
