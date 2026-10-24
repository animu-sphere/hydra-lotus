# Shared static performance workloads

Measured 2026-10-21 from `952372f` plus the shared-workload change, on Windows
AMD64, MSVC 19.51.36256.0 Release, Vulkan SDK 1.3.290 headers/loader import
library and SDK 1.4.350 `slangc`. NVIDIA RTX A5000, driver 597.16 (raw
2504261632), reported device API 1.4.329, 32 logical CPU processors.
Validation and synchronization validation were active with zero messages
in every successful measurement. [Raw measurement populations and hashes](2026-10-21-shared-performance-workloads.json)
retain the successful runs and the failed viewport attempt separately.

This is initial static evidence for LOTUS-PERF-01, not its exit review.
The [active queue](../roadmap/current.md) retains the missing work.

## Procedure

The [manifest](../../validation/performance-workloads.json) fixes seven
versioned procedural workloads, 128×128, MIS, one sample per frame, seed
(initial sample index) 0, 64 bounces, 8 discarded warmup frames starting at
sample index 1, and 64 measured frames after an accumulation reset. Three
fresh-process repetitions were run for each headless fixture under each
integrator: 42 successful measurements. Each also retains the existing
16×16-sample variance procedure and unchanged/point/transform update checks.

Clocks were **unlocked**. The initial `nvidia-smi` observation was P0,
1905 MHz graphics and 8001 MHz memory; no per-frame power/clock capture was
made. No other GPU test or benchmark overlapped the dedicated headless
campaigns. These timings characterize this run, and do not justify an
integrator speed ranking or a default change. Some run means vary by more
than an order of magnitude. The mixed-adapter attempt stopped before
repetitions and is exploratory evidence only.

The [build guide](../guides/BUILDING.md#shared-static-performance-workloads)
records the mixed-adapter invocation. Independent headless runs then used:

```sh
python scripts/performance_baseline.py --headless build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless.exe --output build/shared-headless-reference --label "952372f plus shared static workloads; core Release; 2026-10-21" --clocks "Unlocked; initial nvidia-smi P0 graphics 1905 MHz memory 8001 MHz; driver 597.16"
python scripts/performance_baseline.py --headless build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless.exe --integrator wavefront --output build/shared-headless-wavefront --label "952372f plus shared static workloads; core Release; 2026-10-21" --clocks "Unlocked; initial nvidia-smi P0 graphics 1905 MHz memory 8001 MHz; driver 597.16"
```

The runner preserves individual JSON, logs and PFMs under these build
directories. The committed data retains raw timing populations, scene and
update measurements, environment and PFM hashes, without machine-local
paths. The shared factory leaves the original six fixture definitions and
the default headless workload selection intact; `many-lights-v1` adds 256
sphere lights. The [reference](../reference/SCENE.md#fixed-benchmarks) owns
the fixture and metric contracts.

## Headless observations

Milliseconds below are the median of three run means; the JSON preserves
each run's mean, median, nearest-rank p95 and raw frame population.
GPU time encloses the scene pass; blocking wall time includes submission,
wait, readback and product copies. Neither is a CPU-submit measurement.

| Fixture | Reference GPU | Wavefront GPU | Reference wall | Wavefront wall |
| --- | ---: | ---: | ---: | ---: |
| `cornell-v1` | 0.10152 | 0.22586 | 1.5722 | 1.7948 |
| `textured-v1` | 0.11617 | 0.12211 | 1.5387 | 1.5799 |
| `instanced-v1` | 0.10860 | 0.23620 | 1.4961 | 1.8134 |
| `geometry-v1` | 0.39204 | 0.23415 | 1.8876 | 1.7077 |
| `lights-v1` | 0.33925 | 0.20449 | 1.8066 | 1.7768 |
| `environment-v1` | 0.11383 | 0.15670 | 1.4986 | 1.6180 |
| `many-lights-v1` | 0.77394 | 1.81378 | 2.1440 | 3.3886 |

For example, Reference Cornell's three GPU means were 1.18395, 0.10084 and
0.10152 ms. Wavefront many-lights means were 1.81378, 3.46330 and 1.78926 ms.
The ranges are material; a single number above is not a stable baseline.

All three PFM hashes matched within every fixture/integrator group. The
64-spp Reference and Wavefront RGB images also matched exactly for every
fixture (cross-integrator RMSE 0). Cornell's RMSE against the committed
1024-spp mean was 0.0856835835 for both; the existing statistical reference
check passed. Other fixtures have no independent high-spp reference in this
campaign. Equal deterministic images establish equivalence of these paths,
not complete production-material correctness or absence of Monte Carlo bias.

## Viewport evidence and failure

The first incremental launch terminated with process exit -1073740791,
without a Vulkan diagnostic. After `cmake --build` of the viewport tree with
`--target clean`, rebuilding and launching three Cornell frames succeeded.
This is consistent with the documented
[Windows header dependency limitation](../reference/SUPPORTED_CONFIGURATIONS.md#build-and-tooling-limitations);
the source of that initial process failure was not independently diagnosed.

The mixed-adapter manifest run then completed Cornell, textured, instanced
and geometry viewport cases. Each presented 72 frames, recorded 64 measured
frames after 8 warmup frames, ended at 64 spp, and read back zero bytes.
Their measured GPU means were 0.805324, 0.048116, 0.119495 and 0.099955 ms;
their CPU means, including UI/update/render/present, were 2.834461, 0.867739,
0.940981 and 0.877083 ms. These are single exploratory observations.

The next launch, `lights-v1`, failed at frame 2 with `vkWaitForFences`
VkResult -4 (`VK_ERROR_DEVICE_LOST`). The runner stopped and retained the
failure in `build/shared-baseline-reference/run.json` and its log. The
symptom matches the existing [driver limitation](2026-10-18-viewport-device-loss.md).
It was not converted to a capability SKIP or retried by the runner; there
is no complete three-repeat viewport baseline.

A separate targeted smoke run of `many-lights-v1`, 10 presented frames and
2 warmup frames, succeeded at 8 spp with validation and no readback. This
checks the shared fixture and warmup reset; it does not fill the missing
manifest repetitions.

## Leading costs and decisions

These are the three leading **investigation candidates** exposed by this
small static comparison. Controlled-clock and moving/high-resolution data
are required before adopting an optimization.

1. **Blocking frame overhead.** Headless wall time is substantially larger
   than the timed pass on the small fixtures. It includes readback and waits;
   their individual costs are inseparable here. **Defer** changes until
   submission/wait/readback are separately observed and compared with the
   presented path at practical resolutions.
2. **Many-light continuation.** Wavefront's many-light tail accounts for a
   median 53% of the measured scene-pass time across its three runs. That
   kernel follows remaining paths to completion, including transport and
   analytic-light work. **Defer** acceleration or scheduling changes until
   intersection tests and light selection are profiled separately, with a
   low-light-count control. This is not evidence for a light tree specifically.
3. **Geometry update path.** Wavefront geometry-heavy initial backend update
   wall time was 8.08–8.98 ms and point refit wall time 2.72–2.90 ms, outside
   steady rendering. **Defer** layout/reuse changes until moving-object CPU
   extraction, upload and acceleration work are measured continuously; a
   single startup/edit sample cannot establish an interactive-frame bottleneck.

No renderer optimization or default change was adopted by this investigation.
The uncontrolled clock variance also needs resolving before before/after
performance comparisons.

## Unavailable metrics and remaining evidence

| Metric | Reason / next evidence |
| --- | --- |
| Peak process VRAM | Scene pool reserved/used bytes include host-visible memory and exclude renderer targets; they are not VRAM residency or a peak. |
| Absolute time to equal quality | No error-threshold curve or independent high-spp references for all fixtures. The retained variance×GPU-time measure is an efficiency proxy. |
| Viewport image error | Presented frames intentionally read back no image. Sharing the scene factory is not an image comparison. |
| Fine Reference pass attribution | Existing timestamp encloses the whole pass; ray generation, ray query, shading and resolve are not separately timed. |
| Analytic-light intersection versus selection | Both are embedded in transport kernels; the many-light fixture alone does not separate their costs. |
| Motion, temporal and SVGF | This static manifest does not yet drive camera/object motion or temporal/filter modes. |
| Complete viewport repeats | The known frame-2 device loss interrupted the run; preserve failures and capture the driver/environment conditions before extending measurements. |

## Verification

- Core `ost build --jobs auto`: passed; reference correctness evidence generated.
- `ost build --intent research --jobs auto` and
  `ost test --intent research --jobs 8`: 54/54 passed (22 fast, 12 GPU,
  20 research), 364.66 seconds wall. This includes the fixed benchmark
  reports, the selected many-light fixture and invalid-ID rejection.
- Clean viewport build, Cornell launch and many-light warmup smoke: passed.
- Runtime-free `ci-core` build/test: 23 entries, no failures; 21 passed and
  two GPU checks explained SKIPs.
- Viewport CTest selection: 6/7 passed; `lotus-viewport-present` failed at
  frame 2 with VkResult -4, matching the recorded driver limitation.
  Interaction, missing-driver, camera, options, controls and timings passed.
  The final build's targeted interaction/missing-driver/camera/options/controls/
  timings selection also passed 6/6; the known-failing present case was not
  retried. A final 10-frame, 2-warmup Cornell run with accumulation disabled
  ended at 1 spp and recorded 8 measured frames, zero messages and no readback.
  Raw timing populations are stored only when a benchmark was requested.
- `ost validate --intent research --strict-renderer-evidence`: passed;
  Hydra-host and durable viewport-launch evidence remain capability SKIPs
  for this core build.
- Python suite: 61 tests passed, including matched frontend arguments,
  unsupported seeds/duplicate workloads and rejection of a viewport report
  without the expected reset or with readback/validation messages.

The benchmark regression additionally checks selection of one 256-light
fixture and rejection of unknown workload IDs before touching output.
