# GPU telemetry around shared benchmark attempts

Measured 2026-10-24 on Windows AMD64, NVIDIA RTX A5000, driver 597.16,
Vulkan device API 1.4.329. The existing core Release binary (MSVC
19.51.36256.0, built 2026-10-21) was used without renderer changes; the
runner was `fb4a618` plus the optional telemetry collector. The
[raw evidence](2026-10-24-gpu-telemetry.json) records the binary SHA-256,
compiler, procedure, timing populations, image hashes and telemetry samples.

This is collector validation for LOTUS-PERF-01. It does not close the
[active task](../roadmap/current.md), establish controlled clocks or measure
peak process residency. The previous
[shared baseline and viewport failure](2026-10-21-shared-performance-workloads.md)
remain separate evidence; no viewport launch or driver-failure retry was
made here.

## Procedure

The shared manifest selected only `cornell-v1`: 128×128, MIS, initial sample
index 0, 64 bounces, one sample per frame, eight warmup frames, accumulation
reset, 64 measured frames and three fresh-process repetitions. The existing
16×16-sample variance procedure and update checks were retained. Validation
and synchronization validation were active. No other renderer test or
benchmark overlapped these campaigns. Other desktop GPU activity was not
isolated. Clocks remained unlocked and were never modified.

Reference with telemetry, Wavefront with telemetry, then Reference without
telemetry ran sequentially. Telemetry used a requested 100 ms delay between
queries; it brackets each whole process and is not aligned to measured
frames. It covers startup, warmup, measured rendering, variance rendering,
scene-update checks and teardown. The
[reference](../reference/SCENE.md#fixed-benchmarks) defines the collector's
scope, units and unavailable behavior.

The Reference invocation was:

```sh
python scripts/performance_baseline.py --headless build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless.exe --output build/telemetry-cornell-reference-20261024 --workload cornell-v1 --label "fb4a618 + GPU telemetry; existing core Release binary; 2026-10-24" --clocks "Unlocked; observed by nvidia-smi; no clock settings changed" --gpu-telemetry nvidia-smi
```

Wavefront added `--integrator wavefront` with a distinct output directory.
The control omitted `--gpu-telemetry`, used its own output directory and
labeled clocks as unlocked without telemetry. Raw local reports and logs
remain under `build/telemetry-cornell-{reference,wavefront,control}-20261024`.

## Observations and decision

Each cell lists the three run observations, in order. GPU and blocking wall
times below are per-run frame means in milliseconds.

| Case | GPU mean (ms) | Wall mean (ms) | Telemetry sample count |
| --- | --- | --- | --- |
| Reference, telemetry | 0.949131 / 0.100655 / 0.281382 | 2.980338 / 1.537342 / 1.747484 | 56 / 20 / 21 |
| Wavefront, telemetry | 1.667225 / 0.224459 / 0.222976 | 3.995472 / 1.830063 / 1.754092 | 56 / 22 / 22 |
| Reference, telemetry off | 0.832049 / 0.100586 / 0.328675 | 2.941903 / 1.482089 / 1.851894 | disabled |

All nine PFM hashes matched exactly across repeats, integrators and the
uninstrumented control. The existing Cornell statistical reference check
passed in all nine runs; each reported zero validation messages.

The six instrumented processes each retained 18–54 periodic observations,
in addition to before/after samples, without query errors. Across their
whole-process observations, graphics clocks ranged from 210 to 1695 MHz,
memory clocks from 405 to 8001 MHz, and power states included P0, P3, P5
and P8. Each process crossed power states, even when sampled clocks were
constant. These are process-wide conditions, not an explanation of any
particular measured frame. The first run means were higher in all three
campaigns; the observations do not identify their cause.

Reference sampled whole-device memory maxima were 2134, 2142 and 2147 MiB;
Wavefront maxima were 2151, 2182 and 2188 MiB. Total device memory was
24564 MiB. These figures include other processes and are neither renderer
allocation totals nor process-residency peaks. No subtractive estimate is
used to attribute memory to the renderer.

**Adopt** the opt-in collector as environment evidence. **Defer** performance
ranking and renderer changes: these small, unlocked-clock campaigns do not
isolate polling overhead or prove stable conditions. Keep collection off
by default. Controlled timing, frame attribution, independent quality curves,
motion/temporal/SVGF workloads, process peak residency and complete viewport
repetitions remain outstanding.

## Verification

- Python suite: 76 tests passed, including unavailable tools, query timeout,
  malformed/duplicate device rows, unsupported counters, multiple GPUs,
  partial telemetry and preservation across renderer failure/timeout.
- Real GPU: all nine benchmark runs passed; six instrumented runs retained
  complete telemetry and all images agreed with the control.
- An intentional missing-executable attempt retained its failure log and
  telemetry sidecar, with null process exit code and exactly one attempt.
  It remained a renderer failure rather than becoming a telemetry SKIP.
- Runtime-free `ost build --without-runtime --intent ci-core --jobs auto`
  and `ost test --without-runtime --intent ci-core`: passed, 21 tests passed
  and two GPU checks were explained SKIPs, 23 entries in total.
- Documentation checker and whitespace validation passed. C++/shader source
  and closed research procedures were unchanged, so their full GPU/research
  suites were not rerun.

The first Python test invocation encountered sandbox temporary-directory
permissions. Pointing `TMP` and `TEMP` to `build/test-tmp` allowed the full
suite to run without changing test or collector behavior.
