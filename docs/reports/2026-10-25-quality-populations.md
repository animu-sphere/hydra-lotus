# Cornell quality across disjoint sample sequences

Measured 2026-10-25 from `c3ea4a4` plus the population runner, using the
previous report's final core binary, SHA-256
`6d498533dc4987191d13dcf6b6ab96b190810b109d0d96c06acfae02c3f38bd5`.
The binary was reused unchanged; this change touches no renderer code.
Environment: Windows AMD64, MSVC 19.51.36256.0 Release, 32 logical CPU
processors, NVIDIA RTX A5000, driver 597.16 (raw 2504261632), Vulkan device
API 1.4.329. The [data](2026-10-25-quality-populations.json) retains both
quality summaries and all 48 raw benchmark reports, including timings,
validation, process outcomes, logs and image hashes.

## Evaluation brief

LOTUS-PERF-01 follow-up to the [single-sequence checkpoints](2026-10-25-quality-curve.md).
Measure how finite-reference error and observed threshold attainment vary
across disjoint measured sample ranges. Keep timing repetitions separate
from independent quality observations and preserve deterministic Reference
and Wavefront comparisons.

The predeclared budget is three sequences starting at 1048576, 1114112
and 1179648, four checkpoints (1, 16, 64 and 256 spp), two fresh-process
timing repetitions, and both integrators: 48 benchmark invocations, run
sequentially, with eight warmup frames. Use Cornell at 128×128, MIS and
64 bounces against the unchanged committed 1024-spp BSDF reference.
RMSE targets are 0.2, 0.1 and 0.05. No interpolation, extra checkpoints,
additional seeds or automatic failure retries enter this comparison.

Adopt the population reporting if sequence separation, evidence links and
deterministic comparisons hold. Any driver/validation failure stops the
campaign and remains evidence. Unlocked clocks permit quality observations
and rendering-time records, but defer performance rankings and renderer
optimization decisions. Finite-reference noise remains in the error;
three disjoint ranges do not establish a confidence interval or statistical
independence of the renderer's pseudorandom generator. Memory peaks,
motion, SVGF and other workloads remain separate work.

## Procedure and controls

The [quality runner](../../scripts/quality_curve.py) completed Reference's
24 attempts before starting Wavefront's 24 attempts. No other renderer
benchmark, build or GPU test overlapped the campaigns. Other desktop GPU
activity was not isolated. Every invocation passed its statistical reference
check with validation and synchronization validation active and zero
messages. There were no retries or capability skips.

All measured ranges are disjoint from the reference's indices 0–1023,
warmup's 1–8 and the variance batches starting at 2097152. The reference
SHA-256 remains
`4f50e58917f7c512b4b3680a4d20073053dabfdc98a2ab1764e4cdbfaf018eba`.
Clocks were unlocked and unchanged; the initial `nvidia-smi` observation
was P8, graphics 210 MHz and memory 405 MHz. Polling was disabled, so no
observation attributes clocks to measured frames. These conditions do not
support a speed ranking.

The [build guide](../guides/BUILDING.md#cornell-quality-checkpoints) records
the invocation. Raw manifests, `run.json` summaries, logs and PFMs remain
under `build/quality-populations-{reference,wavefront}-20261025`.
The new `lotus-quality-curve-v2` summary identifies each sequence explicitly
and reports each quality observation once, separately from its timing
repetitions. The [benchmark contract](../reference/SCENE.md#fixed-benchmarks)
owns these definitions and validation rules.

## Quality observations

The following linear-RGB RMSE populations count **three sample ranges**,
not six timing repetitions. Both integrators have identical populations.

| spp | Minimum RMSE | Median RMSE | Maximum RMSE | Sample standard deviation |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 0.596910190 | 0.597537814 | 0.607532823 | 0.005960068 |
| 16 | 0.149394447 | 0.154502147 | 0.159236992 | 0.004922449 |
| 64 | 0.083229405 | 0.085570158 | 0.088315651 | 0.002545806 |
| 256 | 0.050831260 | 0.053097682 | 0.053625412 | 0.001484501 |

For every sequence, integrator and repetition, the first observed checkpoint
remaining at or below RMSE 0.2 was 16 spp; RMSE 0.1 was 64 spp. RMSE 0.05
was not reached through 256 spp, so its samples and times remain null.
The data retains every sequence's individual error and target times.

Every same-sequence/spp image hash matched across both repetitions and both
integrators: 12 matching groups of four images. The original sequence's
64-spp image also matched the previous report's final control exactly.
The sample ranges produce different errors; timing repetitions reproduce
the same error. These checks preserve the deterministic integrator
comparison while exposing the variation that a single measured range hid.

## Rendering times

Ranges below cover the six invocations at each integrator/checkpoint
(three sequences × two timing repetitions). They are **total measured
rendering milliseconds**, not per-frame means. GPU durations sum the raw
sample-pass population; blocking wall time includes submission, waiting,
readback and product copies. Startup, warmup, variance rendering and scene
update checks are excluded. Separate checkpoint times are never added.

| Integrator | spp | Total GPU ms, min–max | Total blocking wall ms, min–max |
| --- | ---: | ---: | ---: |
| Reference | 1 | 0.090528–0.098688 | 1.4695–2.0259 |
| Reference | 16 | 1.601216–1.667552 | 24.2175–26.6511 |
| Reference | 64 | 6.439104–43.966688 | 97.7260–253.8528 |
| Reference | 256 | 26.088448–124.447040 | 391.2516–697.3827 |
| Wavefront | 1 | 0.219584–0.483520 | 1.7497–2.0496 |
| Wavefront | 16 | 3.543392–11.421440 | 28.8702–37.6889 |
| Wavefront | 64 | 14.240000–14.504320 | 111.6571–127.9266 |
| Wavefront | 256 | 57.343968–137.890304 | 442.3160–565.3374 |

The material timing ranges reinforce the need for controlled timing before
choosing an optimization or integrator default. Reaching the same observed
threshold on three ranges is not an estimate of continuous convergence or
ground-truth error; the finite BSDF reference's noise floor was not estimated
or subtracted.

## Decision and remaining evidence

**Adopt** disjoint-sequence population reporting and deterministic timing
repeat checks as bounded measurement infrastructure. **Defer** optimization
and default changes: these clocks are uncontrolled and the population is
small and limited to one static fixture. No claim of a confidence interval
or sampler independence follows from nonoverlapping indices.

This advances, but does not close, [LOTUS-PERF-01](../roadmap/current.md).
Quality curves for other workloads, controlled timing, camera/object motion,
temporal/SVGF modes, peak process residency and complete viewport repeats
remain open. The earlier viewport driver failure remains preserved in the
[shared baseline](2026-10-21-shared-performance-workloads.md#viewport-evidence-and-failure);
no viewport was launched or retried in this campaign.

## Verification

- Python suite: 91 tests passed, including disjoint/overlapping ranges,
  reference/warmup/variance boundaries, population counts independent of
  timing repetitions, deterministic image mismatch rejection, and retention
  of earlier completed curves after a later sequence fails without retry.
- All 48 physical-GPU benchmark attempts passed with zero validation
  messages. Each PFM hash and RGB error was rechecked while assembling the
  data, together with cross-integrator equality and the unchanged binary
  and original-sequence control identities.
- Initial `ost test --intent research --jobs 8`: 53/54 passed in 338.39
  seconds; `lotus-renderer-benchmark-mis` failed its selected many-light
  warmup-collision check. Its older research executable used warmup sample
  index 1 while measuring from 1; the current source and campaign's core
  executable already use 0 for this case. The data retains that failure,
  both binary identities and the diagnostic report; the full initial log
  remains in `build/quality-populations-initial-research.log`.
- `ost build --intent research --jobs auto` rebuilt
  `adapters/headless/benchmarks/benchmark.cpp` and passed its renderer
  evidence generation. This updates the stale build to existing source;
  no renderer source fix was needed for this change.
- After rebuild, `ost test --intent research --filter
  '^lotus-renderer-benchmark($|-mis$|-wavefront$|-no-driver$)' --jobs 8`:
  4/4 passed in 68.55 seconds. The selected case now reports warmup index
  0 and measured index 1, four accumulated samples, and zero validation
  messages. Only the four fixed baseline checks affected by the rebuilt
  source were repeated; the other checks retain their initial passes.
- `ost validate --intent research --strict-renderer-evidence`: passed.
  The regenerated build evidence retains install-tree, Hydra-host and
  durable viewport-launch SKIPs; those hosts were not exercised here.
- Documentation links/anchors/indexes and whitespace validation passed.
