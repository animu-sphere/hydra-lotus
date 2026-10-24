# Independent Cornell quality checkpoints

Measured 2026-10-25 from `53f737a` plus the independent-quality-checkpoint
change, on Windows AMD64, MSVC 19.51.36256.0 Release, Vulkan SDK 1.3.290
headers/loader import library, NVIDIA RTX A5000, driver 597.16 (raw
2504261632), device API 1.4.329, 32 logical CPU processors. The
[data](2026-10-25-quality-curve.json) retains the binary/reference hashes,
all 24 benchmark reports with raw timing populations, image hashes, errors,
process outcomes and observed error-threshold times, plus final-binary
warmup-collision and image controls.

This advances LOTUS-PERF-01's quality evidence on one fixture. It does not
close the [active task](../roadmap/current.md) or replace the
[shared static baseline](2026-10-21-shared-performance-workloads.md).

## Procedure

The [quality runner](../../scripts/quality_curve.py) reused the shared
baseline runner for `cornell-v1`, 128×128, MIS, 64 maximum bounces, one
sample per frame, eight discarded warmup frames and 1, 16, 64 and 256
measured frames. Each checkpoint ran in three fresh processes. Reference
completed first, then Wavefront; no other renderer test, build or benchmark
overlapped these campaigns. Other desktop GPU activity was not isolated.
Validation and synchronization validation were active, with zero messages
in all 24 successful attempts. There were no retries or capability skips.

Measured samples start at 2²⁰, disjoint from the committed BSDF reference's
1024 samples starting at 0 and the variance batches starting at 2²¹.
Warmup still starts at 1. Repetitions use the same measured prefix, so they
observe timing repeatability, not independent-seed quality variance.
The reference's SHA-256 is
`4f50e58917f7c512b4b3680a4d20073053dabfdc98a2ab1764e4cdbfaf018eba`;
the measured executable's is
`530fa774d46f1713ddd62cc1ef8fea80199259281264d86e29771334617d7922`.

Clocks were unlocked and unchanged. The initial `nvidia-smi` observation
was P3, 1410 MHz graphics and 5001 MHz memory. Polling was disabled; there
is no clock attribution to measured frames and no controlled speed ranking.
The invocation is in the [build guide](../guides/BUILDING.md#cornell-quality-checkpoints).
Wavefront added `--integrator wavefront` with its own output directory.
Raw JSON, logs and PFMs remain under
`build/quality-cornell-{reference,wavefront}-20261025`.

## Observations

Errors are linear-RGB RMSE against the finite committed reference, averaged
over every pixel and channel. Every image hash matched across all three
repetitions and both integrators at the same sample count; their
cross-integrator RMSE was zero. The existing statistical reference check
also passed in every attempt.

Times below are **total measured rendering milliseconds** at each
checkpoint, listed in repetition order. GPU time sums the raw sample-pass
durations; blocking wall time includes submission, waiting, readback and
product copies. Startup, warmup, separate variance rendering and scene
update checks are excluded. Checkpoints are independent invocations;
their times are not added to one another.

| Integrator | spp | RGB RMSE | Total GPU ms, three runs | Total blocking wall ms, three runs |
| --- | ---: | ---: | --- | --- |
| Reference | 1 | 0.607532823 | 0.687616 / 0.090176 / 0.090368 | 2.7561 / 1.4228 / 1.4627 |
| Reference | 16 | 0.159236992 | 4.485920 / 1.607424 / 1.602368 | 30.5725 / 23.3275 / 24.8324 |
| Reference | 64 | 0.083229405 | 18.340640 / 6.444896 / 6.454656 | 120.7124 / 95.3539 / 95.5344 |
| Reference | 256 | 0.050831260 | 26.136032 / 26.174688 / 72.086464 | 382.4991 / 380.9398 / 448.1122 |
| Wavefront | 1 | 0.607532823 | 1.434368 / 0.580192 / 0.221216 | 3.8265 / 2.1463 / 1.7754 |
| Wavefront | 16 | 0.159236992 | 3.569632 / 3.574976 / 3.551168 | 28.1514 / 27.8559 / 28.6563 |
| Wavefront | 64 | 0.083229405 | 30.671808 / 34.981824 / 14.279296 | 133.5146 / 137.5754 / 112.7273 |
| Wavefront | 256 | 0.050831260 | 58.572160 / 72.472608 / 101.077088 | 457.1584 / 472.2452 / 509.2520 |

The preselected RMSE targets were 0.2, 0.1 and 0.05. For both integrators
and every repetition, the first observed checkpoint remaining below 0.2
was 16 spp, and below 0.1 was 64 spp; the corresponding times are the rows
above. The 0.05 target was not reached through 256 spp, so its sample count
and times are null. No interpolation or extrapolation is used.

These are finite-reference error thresholds. The BSDF reference is noisy,
and its noise floor is neither estimated nor subtracted here. Passing a
threshold at four discrete checkpoints does not prove continuous
convergence or error against ground truth. The procedure's definitions
belong in the [benchmark contract](../reference/SCENE.md#fixed-benchmarks).

## Decision and remaining evidence

**Adopt** the independent sample-index control and bounded quality runner
as measurement infrastructure. **Defer** optimization/default changes:
the clock conditions are uncontrolled, timing ranges are material and this
is one small static scene. Equal deterministic images support integrator
equivalence for this procedure, not production-material completeness.

Other workload references and independent-seed quality populations,
controlled timing, camera/object motion, temporal/SVGF quality curves,
peak process residency and complete viewport repetitions remain open.
The previous viewport driver failure is preserved; this campaign did not
launch a viewport or retry that failure.

## Verification

- Python suite: 86 tests passed, covering RGB error, PFM scale/endianness,
  malformed/non-finite images, timing population length and missing GPU
  timestamps, target recrossings and unmet targets, sample-index forwarding
  and rejection, and evidence preservation across failure/skip/timeout.
- Default core build and all 24 dedicated physical-GPU attempts passed.
- Runtime-free `ci-core` build/test passed: 21 tests passed and two GPU
  checks were explained SKIPs, 23 entries total.
- `ost build --intent research --jobs auto` and
  `ost test --intent research --jobs 8`: 54/54 passed (22 fast, 12 GPU,
  20 research), 455.52 seconds wall.
- Final core build and `ost test --jobs 8`: 34/34 passed (22 fast, 12 GPU),
  288.30 seconds wall; strict renderer-evidence validation passed.
- The final build also guards the measured-start-1/warmup-start-1 collision:
  it warms up from 0 when measuring from 1, ensuring accumulation resets.
  The existing selected many-light GPU check exercised that case and passed
  at four measured samples with zero validation messages. Default seed 0
  behavior and the quality campaign's 2²⁰/1 procedure remain unchanged.
  The final binary independently reproduced the campaign's 64-spp Cornell
  PFM hash and error exactly, with zero validation messages. The data retains
  both binary identities and these final controls; their timings are not
  pooled with the original campaign.
- Final runtime-free build/test passed with the same 21 passes and two
  explained GPU SKIPs. Documentation checker and whitespace validation
  passed. Hydra and viewport code were unchanged and their hosts were not
  exercised here; the earlier driver failure remains separate evidence.
