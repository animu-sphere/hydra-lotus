# ReSTIR receiver visibility reuse

Measured 2026-10-15 on Windows, MSVC 19.51 Release, Vulkan/Slang,
NVIDIA RTX A5000. Source base: `25aa138`; the after build adds current-receiver
transmittance reuse. The [reservoir reference](../reference/RESTIR.md#temporal-gpu-reuse)
owns behavior and limits.
The before measurements use the existing core Release build with a clean
source checkout at that base; the after build is regenerated with `ost build`.

## Change and independent visibility check

Temporal resolution can keep the initial sample, and spatial resolution can
keep the centre's temporal sample. These selections already have visibility
at the exact current jittered receiver. Matching the light slot and both
unit-square variates permits reuse of that current-frame transmittance.
Changed selections still trace a shadow ray at the receiver. The resolved
normalization and unshadowed contribution are recomputed as before.

The new GPU fixture uses a 32x32 Lambert plane at z=-4, perspective zoom 4,
one 8x4 rect at (6,0,0), and a 75%-opaque panel at z=-2. The panel spans
x=[1,3] and alternates its placement by +1 in x for six fresh frames. One
initial candidate, default power proposal and sample-index base 17 exercise
temporal-only and combined reuse under both integrators. Rigid blocker motion
retains history, while the camera sees the receiving plane throughout.

An independent CPU ray/plane intersection reconstructs each current receiver
from its camera draw and each selected rect point from its UV. It checks every
initial, temporal and spatial transmittance against 0.25 or 1, omitting only
intersections within 1e-4 of an occluder edge. The fixture requires both
unchanged and changed samples, including changed transmittance with the same
light slot. This catches reuse based on light identity alone and tests current
visibility after occluder motion. No previous-frame or neighbour transmittance
is used at the receiver.

## Matched comparison procedure

Before and after builds each run four serial invocations: both integrators
on the distant and mixed-boundaries suites. Each invocation uses 32x32,
64 analytic lights, four independent 32-frame sequences, four warmup frames,
candidate counts 1 and 8, history cap 32, radius 1, sample-index base 17,
and 128 spp per NEE/MIS reference half. Radiance reuse is disabled.
The before label is
`25aa138 before current-sample visibility reuse; Release; 2026-10-15`;
the after label is
`25aa138 plus current-sample visibility reuse; Release; 2026-10-15`.

Each invocation follows this command, selecting its integrator and a distinct
output path. Add `--restir-workload mixed-boundaries` for that suite; omit it
for distant lighting:

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark-restir build/restir-visibility-after-reference-many-distant.json --benchmark-label "25aa138 plus current-sample visibility reuse; Release; 2026-10-15" --benchmark-integrator reference --benchmark-size 32 --benchmark-frames 32 --benchmark-warmup 4 --restir-sequences 4 --restir-lights 64 --restir-reference-samples 128 --restir-candidates 1,8 --restir-history-cap 32 --restir-seed 17
```

No GPU tests or benchmarks overlap these measurements. Four sequences form
the quality population; correlated reused frames and matching before/after
stochastic draws are not extra independent replications. GPU scene time
includes metadata and all reservoir passes, excluding updates and readback.
Validation and dynamic clocks remain enabled. One invocation per condition
does not establish a repeatable performance gain or an equal-quality speedup.

## Results

All eight invocations pass with Vulkan/synchronization validation available
and zero captured messages. All 276 corresponding PFM pairs are byte identical.
All 72 matched cases retain exactly equal reported quality populations,
convergence errors, memory values and history/surface counts. The independent
consumer is `build/verify-restir-visibility.py`; raw JSON, PFM and logs remain
under `build/restir-visibility-{before,after}-<integrator>-<suite>` prefixes.
The [summary JSON](2026-10-15-restir-visibility.json) retains scene, temporal
and spatial median GPU times for every case, including the NEE/MIS and
initial-only controls.

Eight-candidate combined reuse illustrates the inconsistent timing changes:

| Workload | Integrator | Before scene GPU ms | After scene GPU ms |
| --- | --- | --- | --- |
| Static distant | Reference | 0.486880 | 0.090880 |
| Moving distant | Reference | 0.431648 | 0.409824 |
| Static mixed boundaries | Reference | 0.585024 | 0.637984 |
| Moving mixed boundaries | Reference | 0.540288 | 0.479072 |
| Static distant | Wavefront | 0.120864 | 0.711648 |
| Moving distant | Wavefront | 0.655008 | 0.551360 |
| Static mixed boundaries | Wavefront | 0.848320 | 0.893728 |
| Moving mixed boundaries | Wavefront | 0.616000 | 0.736192 |

These are medians over the 128 measured frames per case. Across all reuse
cases, after/before scene-time ratios range from 0.187 to 5.888. Unchanged
NEE/MIS and initial-only controls range from 0.317 to 6.305. The large changes
in controls as well as reuse prevent attributing timing differences to the
removed queries. No repeatable speed improvement is established by this run.

The independent area-light checks pass under both integrators. Per integrator,
the temporal-only fixture has 2,508 unchanged selections and 3,636 changed
selections, including 1,840 changed transmittances. The combined fixture's
temporal and spatial comparisons total 3,251 unchanged and 9,037 changed
selections, including 4,384 changed transmittances. These are branch-coverage
observations on the test fixture, rather than GPU ray-count instrumentation.

## Validation

The managed core build passes, followed by all 35 core tests in 506.34 seconds
and strict renderer-evidence validation with completion-bound producers.
This includes the complete reservoir GPU suite, temporal/radiance regressions,
both integrators' benchmark proposals and workload suites, synchronization,
missing-driver behavior and install-tree checks.

The runtime-free build passes 22 tests with two explained temporal/reservoir
GPU SKIPs. The Python suite passes 42 tests, and documentation validation passes.
The managed Hydra build and three targeted integration checks pass: the ReSTIR
contract, moving-stage GPU test and installed usdview host. The latter completes
in 112.18 seconds. The full Hydra suite and a release gate are not rerun for
this shared-shader optimization.

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

Build, test and validation logs remain under `build/restir-visibility-` prefixes.

## Scope

This is a bounded implementation step in LOTUS-RESTIR-05. It removes repeated
visibility queries for unchanged selections without changing the estimator,
support correction, RNG draws, history caps, storage, settings or Hydra
translation. Temporal correlation, the remaining reuse cost, broader artifact
and bias coverage, the scope review and release gate remain in the
[execution plan](../roadmap/current.md#execution-order).
