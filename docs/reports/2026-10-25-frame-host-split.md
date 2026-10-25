# Blocking frame host phases and cached readback

## Evaluation brief

LOTUS-PERF-01 follow-up to the first leading cost of the
[shared static workloads](2026-10-21-shared-performance-workloads.md#leading-costs-and-decisions):
headless blocking wall time was 1.4–1.8 ms per 128x128 frame against a
0.1–0.4 ms scene pass, and its submission, wait and readback were
inseparable. That decision was deferred until they were observed
separately.

This change times them. `GpuFrameEvidence::host_timings` splits each
passing frame call into preparation, recording, submission, fence wait,
readback and results, and the benchmark keeps them as `render_host_ms`
([contract](../reference/SCENE.md#fixed-benchmarks)). The first
measurement attributed about 1.1 ms of every frame to readback: copying the
colour and depth products (320 KiB) out of their host-visible buffers ran at
about 290 MB/s. The renderer chose the first host-visible, coherent memory
type for every host buffer, which on this device is uncached (type 3,
`HOST_VISIBLE | HOST_COHERENT`); type 4 adds `HOST_CACHED`. Host reads of
uncached memory are bus reads.

Candidate: allocate host buffers that the GPU copies into
(`VK_BUFFER_USAGE_TRANSFER_DST_BIT`) from host-cached memory, preferring a
coherent type, and fall back to the previous choice. Non-coherent memory
was already invalidated before every read and flushed after host writes.
This covers the colour/depth products, the wavefront queue counts, and the
temporal and reservoir readback mirrors. Buffers the host only writes, such
as constants, keep their type.

Adoption criteria: before/after wall and phase measurements on the shared
manifest, identical images, unchanged correctness checks and no phase made
slower. Predeclared budget: the static manifest under Reference, seven
workloads × three fresh processes per side (42 invocations); eight
single-process Cornell probes (128 and 512 pixels, validation on and off,
each side); one short ReSTIR benchmark per side. No retries or exclusions.

Conditions: Windows AMD64, MSVC 19.51.36256.0 Release, NVIDIA RTX A5000,
driver 597.16, Vulkan loader 1.4.321, unlocked clocks (`nvidia-smi` P8 at
the start of the campaign), no overlapping build or benchmark. Validation
and synchronization validation were active except where stated. Both
binaries were built from `764b740` plus this change before it was
committed. The before binary (SHA-256
`4cd2e73abdc1e847969274b05e309367ed1fb4e1b0a156da832e8ed43d940c5c`)
has the phase timing but not the allocation change; the after binary is
`f98df077ee874a7979a2c8dd594af8a166fc6b194ed1bcba73e0d973ddbc7808`.

## Correctness

- All 42 manifest attempts passed with zero validation messages. Every
  workload's six final images are byte-identical, before and after.
- The eight probes' images are identical within each size, with and
  without validation.
- ReSTIR's 29 image files are byte-identical, and so is every non-timing field
  of its report: errors, ensembles, residuals, bias tests and memory.
- The headless renderer report, written by the build, passed its 28
  applicable checks. These include restoring supplied colour and depth
  through the readback buffers as staging, which now writes cached memory.

## Phases of a 128x128 frame

Medians in milliseconds over the 64 measured frames per process. Ranges
span the three processes; the [data](2026-10-25-frame-host-split.json)
keeps each process's phase medians, wall, GPU time and image hash.
"Host" is every phase except the fence wait.

| Workload | Readback before → after | Host before → after | Wall before → after |
| --- | ---: | ---: | ---: |
| `cornell-v1` | 1.128–1.141 → 0.014–0.016 | 1.263 → 0.137 | 1.441–1.450 → 0.324–0.559 |
| `textured-v1` | 1.127–1.149 → 0.014 | 1.262 → 0.137 | 1.401–1.569 → 0.253–0.436 |
| `instanced-v1` | 1.138–1.143 → 0.015–0.016 | 1.264 → 0.139 | 1.410–1.512 → 0.235–0.261 |
| `geometry-v1` | 1.137–1.158 → 0.014–0.015 | 1.261 → 0.136 | 1.438–1.458 → 0.299–1.130 |
| `lights-v1` | 1.127–1.155 → 0.014–0.017 | 1.278 → 0.139 | 1.406–1.460 → 0.284–0.525 |
| `environment-v1` | 1.141–1.151 → 0.013–0.016 | 1.277 → 0.138 | 1.438–1.696 → 0.291–0.564 |
| `many-lights-v1` | 1.118–1.166 → 0.015–0.024 | 1.268 → 0.155 | 2.079–3.020 → 1.021–2.279 |

Readback fell about 80-fold and every frame lost about 1.1 ms of host time.
The other host phases did not change: recording 0.07–0.10, submission
0.03–0.04, preparation and results at most 0.01 ms each. The wide wall ranges
come from the fence wait, which follows the GPU's power state. That state
was again bimodal per process. For example, Cornell's after-processes ran
the scene pass in 0.299, 0.099 and 0.289 ms, against 0.098–0.099 ms before.
This is the clock variance seen in the
[motion report](2026-10-25-motion-workloads.md#power-state-bimodality), not
an effect of the change: the waits stay 0.06–0.17 ms above the scene pass
on both sides.

## Size and validation

Single-process Cornell probes, 8 warmup and 64 measured frames, medians in
milliseconds. Validation was disabled with
`VK_LOADER_LAYERS_DISABLE=VK_LAYER_KHRONOS_validation`.

| Size, validation | Readback before → after | Wall before → after | Record | Submit | Wait | GPU |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 128, off | 1.119 → 0.013 | 1.303 → 0.182 | 0.007–0.008 | 0.010–0.011 | 0.148–0.153 | 0.099 |
| 128, on | 1.131 → 0.014 | 1.423 → 0.558 | 0.076 | 0.032–0.033 | 0.165 → 0.419 | 0.099 → 0.289 |
| 512, off | 22.468 → 0.852 | 23.641 → 1.855 | 0.013–0.026 | 0.015–0.024 | 0.871–0.894 | 0.635 |
| 512, on | 22.463 → 0.940 | 26.134 → 4.338 | 0.086–0.135 | 0.037–0.051 | 3.099–3.169 | 2.069–2.282 |

Uncached readback cost was proportional to the products' size: 5 MiB at
512x512 took 22.5 ms, about 230 MB/s. Cached readback took 0.85–0.94 ms
there. That includes allocating and zero-filling the products' vectors,
not only the copy. Validation added about 0.07 ms to recording, 0.02 ms to
submission and 0.01 ms to preparation per frame. Without validation, a
cached 128x128 frame takes 0.18 ms of wall time for a 0.10 ms scene pass.
These are single observations under unlocked clocks; the slower GPU times
of the validated runs differ between processes as above.

## ReSTIR evidence frames

`RenderTemporalScene` reads back its reservoir and temporal products every
frame. The [reference](../reference/RESTIR.md) cited 28–36 ms per reuse
frame at 128x128 for this path. One short run per side
(`--benchmark-restir --benchmark-size 128 --benchmark-frames 8
--benchmark-warmup 2 --restir-sequences 2 --restir-reference-samples 32
--restir-candidates 8`), median render/update wall in milliseconds:

| Mode | `many-distant-plane-v1` | `many-distant-moving-shadow-v1` |
| --- | ---: | ---: |
| NEE/MIS | 2.02 → 0.89 | 2.81 → 1.11 |
| Initial-only | 5.58 → 1.72 | 6.54 → 1.84 |
| Temporal | 32.69 → 5.69 | 32.01 → 5.21 |
| Spatial | 32.96 → 9.03 | 36.57 → 5.42 |
| Spatiotemporal | 42.17 → 8.48 | 40.50 → 8.36 |

Most of the earlier reuse-frame cost was therefore uncached reads of the
evidence products. The presented path, which reads nothing back, is
unaffected.

## Decisions

- **Adopt** cached readback memory. With identical images and statistics,
  it removes about 1.1 ms per 128x128 frame from every workload, 21.6 ms at
  512x512, and 24–34 ms from ReSTIR reuse frames. No interface changes;
  `host_timings` is additive.
- **Explained, defer the rest** of the blocking frame overhead. After this
  change a validated 128x128 frame spends about 0.14 ms on the host outside
  the wait, mostly recording and submission under validation; without it,
  0.03 ms. The wait exceeds the scene pass by 0.05–0.17 ms, which includes
  the product copies outside the timestamped pass and completion latency.
  Re-recording each frame, frames in flight and reusing product vectors
  are not justified by these costs at the measured sizes.
- The other two leading costs keep their decisions: many-light continuation
  is [LOTUS-LIGHT-05](../roadmap/current.md#deferred-adapter-work), and the
  geometry update path adopted
  [changed-section staging](2026-10-25-changed-section-staging.md#decision).

## Limits

| Missing | Reason / next evidence |
| --- | --- |
| Hydra | The render delegate reads back each frame and would use the same buffers; the Hydra tree was not rebuilt or measured. |
| Viewport | Presented frames read nothing back, so the change does not apply. Viewport host phases are not reported. |
| Wavefront campaign | Readback does not depend on the integrator. The wavefront queue counts now use cached memory too, but only the wavefront benchmark CTest checked them. |
| Other devices | Only this device's memory types were measured. A device without a cached type keeps the previous choice. |
| Controlled timing | Clocks were unlocked and per-process bimodal; walls are reported as ranges, and the readback result does not depend on clocks. |
| Scene-update host split | `UpdateScene` still does not time its comparisons, staging copies, submission and wait separately. |
| 1080p/4K | The products' readback was measured at 128 and 512 pixels only. |

## Verification

- `ost build --jobs auto` (core Release): passed, including the renderer
  report.
- `ctest -R "lotus-renderer-(benchmark|benchmark-wavefront|benchmark-mis|benchmark-no-driver|temporal-gpu|restir-gpu)$"`:
  6/6 passed. The benchmark checks require six host-phase series of the
  measured population per scene, finite and non-negative phases and a sum
  within the frame's wall time.
- The 42 manifest attempts, 8 probes and 2 ReSTIR runs passed;
  `build/compare-host-split-20261025.py` assembled the evidence. The
  manifest runs used the [shared baseline runner](../guides/BUILDING.md#shared-static-performance-workloads)
  with `--output build/host-split-before-20261025` and
  `build/host-split-after-20261025`.
- Not rerun: the full gpu and research tiers, Hydra, viewport and `ci-core`
  trees.
