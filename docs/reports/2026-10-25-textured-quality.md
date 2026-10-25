# Textured workload finite-reference quality

## Evaluation brief

LOTUS-PERF-01 follow-up to the [Cornell populations](2026-10-25-quality-populations.md).
Extend the existing quality runner to a shared static workload using an
explicit, validated finite Reference PT baseline, with disjoint measured
sample ranges and preserved reference provenance. This changes measurement
scripts only; it reuses the existing core Release binary from `ca7edbf`,
SHA-256 `6d498533dc4987191d13dcf6b6ab96b190810b109d0d96c06acfae02c3f38bd5`.
The binary predates this commit and is unchanged from the Cornell population
campaign; `ca7edbf` identifies the source checkout, not a fresh build.

The predeclared budget is 34 sequential benchmark invocations on
`textured-v1`, 128×128, MIS, 64 bounces and eight discarded warmup frames:
two Reference PT images at 4096 spp, starting at 0 and 65536; then 1, 16,
64 and 256 spp, measured starts 1048576 and 1114112, two fresh-process
timing repetitions, both integrators. The first 4096-spp image is the
comparison reference; the second measures a finite-reference discrepancy
without subtracting noise or establishing ground truth. Targets are linear
RGB RMSE 0.2, 0.1 and 0.05. No extra checkpoints, seeds or automatic retries
enter this budget. Any driver or validation failure stops the campaign and
remains evidence.

Hardware: Windows AMD64, MSVC 19.51.36256.0 Release, 32 logical processors,
NVIDIA RTX A5000, driver 597.16, device API 1.4.329. Clocks are unlocked
and unchanged. Initial `nvidia-smi`: P0, graphics 1695 MHz, memory 8001 MHz;
polling is disabled. No renderer benchmark, build or GPU test overlaps this
campaign; desktop GPU activity is not isolated. These conditions permit
quality observations and recorded rendering times, with speed rankings and
optimization/default decisions deferred.

Adopt the extension if reference/workload/settings validation, disjoint
ranges, evidence snapshots and deterministic integrator/repetition checks
hold. Reject mismatched, failed or modified reference evidence before
launch. This fixture does not cover production-material completeness,
motion, temporal/SVGF modes, process peak residency or the viewport. Keep
the [earlier viewport failure](2026-10-21-shared-performance-workloads.md#viewport-evidence-and-failure)
separate and preserved.

## Procedure and retained evidence

All 34 attempts passed, with validation and synchronization validation active
and zero messages. There were no retries or capability skips. The primary
reference completed first, followed by the split reference, the 16 Reference
checkpoints and the 16 Wavefront checkpoints. Research tests started only
after the campaign completed.

The [data](2026-10-25-textured-quality.json) retains both quality summaries,
all 34 raw benchmark reports, process outcomes, commands and logs, image
hashes, reference evidence hashes and runner/binary identities. Original PFMs
and logs remain under `build/quality-textured-{finite,split}-reference-20261025`
and `build/quality-textured-{reference,wavefront}-20261025`. Each quality
campaign also snapshots the primary reference's `run.json`, benchmark JSON,
process log and PFM. The reference PFM SHA-256 is
`e48d583a995390ba1987dac2db2d845d9f6e3da88e8f3aab025b3cffd1fde587`.
The [build guide](../guides/BUILDING.md#other-static-workloads) records the
commands; the [benchmark contract](../reference/SCENE.md#fixed-benchmarks)
owns range separation, reference validation and metric scopes.

The fixture contains sixteen procedural UV-mapped checker-textured patches,
each with a 256×256 sRGB texture and a 32×32 quad grid. These are shared
headless/viewport fixture inputs, lit by a constant white environment with
no analytic lights or emissive triangles; they do not establish quality for
textured scenes under complex lighting. No USD extraction or production
material asset was exercised. Measured indices are disjoint from both reference
ranges, warmup indices 1–8 and variance batches starting at 2097152.

## Quality observations

Linear-RGB RMSE covers every pixel and channel against the primary finite
4096-spp MIS reference. The population below counts **two measured sample
ranges**, with identical results under both integrators. Timing repetitions
reproduce each range's image and do not enlarge the quality population.

| spp | Minimum RMSE | Median RMSE | Maximum RMSE |
| ---: | ---: | ---: | ---: |
| 1 | 0.154770447 | 0.155192109 | 0.155613770 |
| 16 | 0.038665782 | 0.038729383 | 0.038792984 |
| 64 | 0.019429258 | 0.019453124 | 0.019476990 |
| 256 | 0.009932032 | 0.009963782 | 0.009995531 |

Every sequence/integrator/repetition first remained below RMSE 0.2 at the
1-spp checkpoint; RMSE 0.1 and 0.05 at 16 spp. The data retains each
sequence's observed target time, without interpolation between checkpoints.
All eight same-sequence/spp groups contained four identical image hashes
(two integrators × two repetitions). All 32 image errors and hashes were
independently rechecked when assembling the retained evidence.

The two disjoint 4096-spp Reference images differ by RGB RMSE **0.003404050**
(MSE 0.000011587559). This discrepancy exposes reference sampling variation;
it is neither an estimate of the primary reference's absolute error nor a
noise-floor correction. Neither reference is ground truth. The measured
population is small, and nonoverlapping indices do not prove statistical
independence of the renderer's sampler.

## Rendering times

Ranges cover four invocations per integrator/checkpoint (two measured ranges
× two timing repetitions). They sum measured rendering durations, excluding
startup, warmup, separate variance rendering, scene-update checks and
teardown. GPU time covers timed sample passes; blocking wall time includes
submission, waiting, readback and product copies. Each checkpoint is a fresh
invocation; checkpoint times are never added together.

| Integrator | spp | Total GPU ms, min–max | Total blocking wall ms, min–max |
| --- | ---: | ---: | ---: |
| Reference | 1 | 0.047040–0.049888 | 1.4332–1.7563 |
| Reference | 16 | 0.742880–0.766688 | 22.7683–25.7439 |
| Reference | 64 | 11.433664–12.676192 | 107.7713–118.8330 |
| Reference | 256 | 12.190976–112.281888 | 387.0983–925.5977 |
| Wavefront | 1 | 0.119008–0.126592 | 1.5716–1.9331 |
| Wavefront | 16 | 1.926336–6.534144 | 26.1905–39.8335 |
| Wavefront | 64 | 7.760576–22.941024 | 104.8137–184.2897 |
| Wavefront | 256 | 31.320352–108.836640 | 442.2928–637.4593 |

Reference generation is retained separately from these curves: its cost is
not included in measured target times. Material timing variation and
uncontrolled clocks preclude a speed ranking or an optimization decision.

## Decision and remaining evidence

**Adopt** explicit finite-reference validation and evidence snapshots for
static-workload quality curves. The textured fixture extends the Cornell
evidence while preserving deterministic integrator/repetition comparisons.
**Defer** renderer optimizations and default changes. This advances
[LOTUS-PERF-01](../roadmap/current.md) without closing it: other fixtures,
controlled timing, camera/object motion, temporal/SVGF modes, process peak
residency and complete viewport repetitions remain open. No viewport was
launched or retried here; the earlier driver failure remains evidence.

## Verification

- Python suite: 94 tests passed using a workspace-local temporary directory,
  including external-reference provenance/settings/hash rejection, missing
  logs, overlapping ranges, the first disjoint sample and workload forwarding.
  Existing Cornell, population and failure-retention tests also passed.
- All 34 predeclared physical-GPU attempts passed with zero validation
  messages. Evidence assembly rechecked all image hashes/errors, reference
  snapshots and equality of eight groups of four images. The renderer binary
  remained unchanged; no renderer source or build configuration changed.
- `ost test --intent research --jobs 8`: 54/54 passed (22 fast, 12 GPU,
  20 research), 382.75 seconds wall. This includes the default regression
  tiers; no rebuild was needed for this Python-only measurement change.
  The full log remains in `build/textured-quality-research-20261025.log`.
- `ost validate --intent research --strict-renderer-evidence`: passed against
  the existing build evidence, retaining its install-tree, Hydra-host and
  durable viewport-launch SKIPs. Those hosts were not exercised by this
  campaign. Log: `build/textured-quality-validation-20261025.log`.
- Documentation links, anchors, indexes and whitespace checks passed.
