# ReSTIR sample evaluation reuse

Measured 2026-10-16 on Windows, MSVC 19.51 Release, Vulkan/Slang,
NVIDIA RTX A5000. Source base: `4ba26dd`; the after build adds source-target
caching and shared final light reconstruction. The
[reservoir reference](../reference/RESTIR.md#temporal-gpu-reuse) owns behavior
and limits.

## Change

Temporal and spatial support correction previously reevaluated the final
selection at each source even when it was that source's own selected sample.
An exact light-slot/UV match now uses its stored unshadowed selected target.
Other selections retain full source-surface evaluation, including all-zero
observed streams. Full RGB finiteness checks remain on evaluated contributions;
cached targets were validated before their source was published.

Final receiving-surface shading and changed-sample visibility now share one
reconstructed light sample. A matching current receiver sample still uses its
same-frame transmittance. No old or neighbouring transmittance is used at the
receiver. Receiving contributions are still evaluated at the current exact
jittered surface. RNG draws, support counts, normalization, history caps,
storage and public controls retain their existing contracts.

## Independent area-light check

The moving fractional-occluder fixture from the
[visibility measurement](2026-10-15-restir-visibility.md) now checks targets
and RGB as well as transmittance. It reconstructs the current receiver's X/Y
from the independent CPU camera draws and the selected rect point from both
UV coordinates. With area 32 and parallel receiving/emitting planes, the
unit-square Jacobian is `32 * z^2 / distance^4`. Multiplying by the Lambert
albedo, radiance and `1/pi` gives each unshadowed RGB contribution. The test
checks maximum RGB against the stored target (absolute tolerance 3e-6), and
resolved RGB against this contribution times explicit normalization and
transmittance (4e-6).

Both integrators pass six frames of temporal-only and combined reuse at
32x32, one candidate and sample-index base 17. Per integrator, temporal-only
has 2,508 unchanged selections and 3,636 changed selections, including 1,840
changed transmittances. Combined temporal/spatial comparisons have 3,251
unchanged and 9,037 changed selections, including 4,384 changed transmittances.
These counts describe receiver-visibility branches, rather than source-target
cache hit rates or GPU instruction/ray instrumentation. Existing
complementary-support tests cover zero observed streams and differing support
denominators separately.

## Matched comparison procedure

The before build is regenerated at the clean source base. Each build runs
four serial invocations: both integrators on the distant and mixed-boundaries
suites. Every invocation uses 32x32, 64 analytic lights, four independent
16-frame sequences, four warmup frames, candidates 1 and 8, history cap 32,
radius 1, power proposals and measured seed 17. Each finite NEE/MIS reference
half uses 128 spp. Radiance reuse is disabled. No physical-GPU tests or
benchmarks overlap the measurements.

Each invocation follows this command, selecting its integrator and a distinct
output path. Add `--restir-workload mixed-boundaries` for that suite:

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark-restir build/restir-evaluation-after-reference-many-distant.json --benchmark-label "4ba26dd plus evaluation reuse; Release; 2026-10-16" --benchmark-integrator reference --benchmark-size 32 --benchmark-frames 16 --benchmark-warmup 4 --restir-sequences 4 --restir-lights 64 --restir-reference-samples 128 --restir-candidates 1,8 --restir-history-cap 32 --restir-seed 17
```

Before labels use `4ba26dd before evaluation reuse; Release; 2026-10-16`.
The independent comparison consumer is `build/verify-restir-evaluation.py`;
raw JSON, PFMs and logs remain under `build/restir-evaluation-` prefixes.
The [summary JSON](2026-10-16-restir-evaluation.json) retains all scene,
temporal and spatial median times, settings and device identity.

## Results

All eight invocations pass with Vulkan/synchronization validation available
and zero captured messages. All 212 matched PFM pairs are byte identical.
All 72 matched cases have exactly equal quality populations, convergence
errors, memory and history/surface counts. Scene references, settings and
device identity also match. The consumer compares every nontiming case field,
including per-sequence convergence errors.

Eight-candidate combined reuse gives these scene-time medians over 64
measured frames per case:

| Workload | Integrator | Before GPU ms | After GPU ms |
| --- | --- | --- | --- |
| Static distant | Reference | 0.571712 | 0.460736 |
| Moving distant | Reference | 0.463808 | 0.610336 |
| Static mixed boundaries | Reference | 0.795744 | 0.691584 |
| Moving mixed boundaries | Reference | 0.799904 | 0.790656 |
| Static distant | Wavefront | 0.712864 | 0.560608 |
| Moving distant | Wavefront | 0.763392 | 0.770752 |
| Static mixed boundaries | Wavefront | 0.460640 | 0.934848 |
| Moving mixed boundaries | Wavefront | 0.847456 | 0.909184 |

Across reuse cases, after/before scene-time ratios range from 0.701 to 2.588.
Unchanged NEE/MIS and initial-only controls range from 0.392 to 5.568. Changes
in these controls prevent attributing the timing differences to the removed
evaluations. This run establishes no repeatable performance improvement.
GPU scene time includes metadata and reservoir passes, excluding updates and
readback. Dynamic clocks and validation remain enabled.

Four sequences form each invocation's quality population. Correlated frames
and matched before/after or integrator draws are not extra independent
replications. One invocation per condition and finite NEE/MIS references do
not establish unbiasedness, temporal stability or equal-quality speedup.

## Validation

The managed core build and all 35 core tests pass in 511.34 seconds, followed
by strict renderer-evidence validation with completion-bound producers. This
includes the full reservoir GPU suite, temporal/radiance regressions,
both integrators' benchmark proposals/workloads, synchronization,
missing-driver behavior and install-tree checks. The extended area-light
target/RGB/visibility fixture passes separately under both integrators.
The runtime-free build/test passes 22 checks with two explained
temporal/reservoir GPU SKIPs. The Python suite passes 42 tests, and
documentation validation passes.

The managed Hydra build and all three targeted integration checks pass:
the ReSTIR contract, moving-stage GPU test and installed usdview host. The
host test completes in 111.01 seconds. The full Hydra suite and release gate
are not rerun for this shared-shader change.

```sh
ost build --jobs auto
ost test
ost validate --strict-renderer-evidence
ost build --without-runtime --intent ci-core --jobs auto
ost test --without-runtime --intent ci-core
ost build --profile lookdev --intent hydra --jobs auto
ctest --test-dir build/cy2026-windows-x86_64-py313-lookdev--hydra -C Release -R 'hydra-restir|usdview-host' --output-on-failure
python -m unittest discover -s scripts -p 'test_*.py'
python scripts/check_docs.py
```

Build, test and validation logs remain under `build/restir-evaluation-`
prefixes.

## Scope

This is a bounded reuse-cost step in LOTUS-RESTIR-05. Stored-target reuse
removes work on exact matches, and sharing reconstruction removes duplicate
work for changed final selections. This source-level reduction does not
establish an equal-quality speedup. Broader artifact/bias coverage, temporal
correlation, remaining cost, scope review and the release gate remain in the
[execution plan](../roadmap/current.md#execution-order).
