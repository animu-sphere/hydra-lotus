# Analytic-light crossing versus selection cost

## Evaluation brief

LOTUS-LIGHT-05, the investigation for the second leading cost of the
[shared static workloads](2026-10-21-shared-performance-workloads.md#leading-costs-and-decisions):
on `many-lights-v1` (256 sphere lights), the wavefront tail kernel took a
median 53% of the scene pass, and that kernel's transport and analytic-light
work were inseparable. The
[workstream contract](../design/ROADMAP_POLICY.md#practical-foundation-workstreams)
asks for intersection to be profiled separately from NEE/MIS selection, with
a low-light-count control, before any acceleration experiment.

The renderer does the two kinds of work in different places:

- **Crossing.** `CrossedLights` (`common/scene.slang`) loops over every
  analytic light at each continuation ray's hit and escape, adding the lights
  the ray crossed weighed against light sampling. Its cost grows with the
  light count; the [Renderer Phase 3 measurement](2026-10-12-renderer-phase3-exit.md#analytic-lights-along-each-ray)
  found it linear with a temporary build.
- **Selection.** `SampleLight` chooses one light-table entry with the alias
  method (two 16-byte reads) and samples a direction towards it for one
  shadow ray. Nothing in it depends on the light count.

Two measurements separate them:

1. **Light-count control.** `--benchmark-light-grid <side>`
   ([contract](../reference/SCENE.md#fixed-benchmarks)) replaces the
   fixture's 16×16 grid by a smaller square grid over the same footprint,
   radius and total power. Side 16 reproduces the fixture byte for byte.
   With selection constant, the time that grows with the light count is
   crossing.
2. **Crossing disabled.** A temporary build, not committed, returned zero
   from `CrossedLights` before its loop. Its images are wrong by
   construction (the MIS-weighted emission that BSDF sampling finds is
   dropped), so only its timings are used. What remains of a frame is
   everything except crossing, selection included.

Predeclared budget: `many-lights-v1` under MIS, both integrators, 128 and
512 pixels, 8 warmup and 64 measured frames, three fresh processes per
configuration in run-major round-robin order. Grids 1, 2, 4, 8 and 16 on the
committed binary (60 invocations); grids 1 and 16 on the probe (24). No
retries or exclusions.

Conditions: Windows AMD64, MSVC 19.51.36256.0 Release, NVIDIA RTX A5000,
driver 597.16, Vulkan loader 1.4.321, validation and synchronization
validation active, clocks unlocked and never changed. `nvidia-smi` power
states were sampled every 250 ms around each process. Both binaries were
built from `e54bc8d` plus this change; the committed binary's SHA-256 is
`5a849a20cf3a278c…` and the probe's `956970d5ef50c85f…` (full hashes in the
[data](2026-10-25-light-crossing-cost.json)).

## Correctness

- All 84 invocations passed with zero validation messages.
- Within every configuration, the three processes' images are
  byte-identical, and so are the Reference and Wavefront images of the
  same grid and size.
- Grid side 16 and the unmodified fixture produce identical images.

## Cost against light count

GPU scene-pass milliseconds per frame. Each value is the lowest of the
three process medians: as in the
[motion report](2026-10-25-motion-workloads.md#power-state-bimodality), some
processes ran at a lower clock throughout (for example Reference 128, one
light: 0.097, 0.097 and 0.303 ms). The data keeps every process's median,
mean, p95 and sampled power states.

| Lights | Reference 128 | Reference 512 | Wavefront 128 | Wavefront 512 |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 0.097 | 0.628 | 0.222 | 0.822 |
| 4 | 0.104 | 0.678 | 0.234 | 0.845 |
| 16 | 0.134 | 0.861 | 0.292 | 1.569 |
| 64 | 0.252 | 1.589 | 0.536 | 1.598 |
| 256 | 0.733 | 4.125 | 1.739 | 4.331 |
| **Crossing disabled, 1** | 0.085 | 0.943 | 0.198 | 0.800 |
| **Crossing disabled, 256** | 0.085 | 0.633 | 0.201 | 0.840 |
| Least-squares slope (µs/light) | 2.49 | 13.7 | 5.98 | 13.2 |
| Intercept (ms) | 0.094 | 0.644 | 0.197 | 0.933 |

Without crossing, 256 lights cost what one light costs. At 128 pixels the
committed renderer's times lie on a straight line within 1.3 µs (Reference)
and 44 µs (Wavefront). At 512 pixels the fit is looser: all three
Wavefront 16-light processes were sampled in a P3 state and ran 0.4 ms or
more above the line, and the probe's one-light Reference value is the
slower of the clocks seen. Neither changes the attribution at 256 lights:

| | Reference 128 | Reference 512 | Wavefront 128 | Wavefront 512 |
| --- | ---: | ---: | ---: | ---: |
| Crossing at 256 lights (ms) | 0.647 | 3.492 | 1.538 | 3.491 |
| Share of the frame | 88% | 85% | 88% | 81% |

The remainder, 0.09–0.84 ms, holds camera rays, BSDF sampling, traversal,
shadow rays and selection. Selection is therefore at most 12–19% of the
256-light frame, and it does not grow with the light count.

### Wavefront kernels

Kernel medians of the fastest process, milliseconds:

| 128 pixels | Camera | Intersect | Shade | Tail |
| --- | ---: | ---: | ---: | ---: |
| 1 light | 0.013 | 0.051 | 0.072 | 0.062 |
| 256 lights | 0.013 | 0.383 | 0.421 | 0.898 |
| 256 lights, crossing disabled | 0.012 | 0.046 | 0.065 | 0.056 |

The intersect kernel crosses lights on escape, shade at every hit, and the
tail does both for the paths left after six rounds. Rounds (6) and rays per
path (3.649) do not depend on the light count. The tail's earlier 53% share
was therefore crossing, not scheduling: without crossing it is 0.056 ms.

### Light count and variance

Per-sample variance at 128 pixels was 2.47, 12.86, 8.24, 4.03 and 1.91 for
1, 4, 16, 64 and 256 lights. The grids light the box differently (side 2
puts its four lights near the walls), so these values do not compare
selection quality, and this campaign does not measure a better selection
distribution. Equal quality is not in question for crossing: an
acceleration structure that finds the same lights leaves the estimator
unchanged, so its benefit is its time.

## Memory

Each analytic light is a 96-byte record and a 16-byte light-table entry:
28 KiB for 256 lights, read by every crossing test. A crossing structure
adds to this and must report its size.

## Decisions

- **Go: crossing acceleration experiment.** Crossing is 81–88% of the
  256-light frame under both integrators and both sizes, grows linearly at
  2.5–14 µs per light per frame, and accounts for the wavefront tail's
  share. With one light, crossing costs 0.012 ms (Reference) and 0.024 ms
  (Wavefront) at 128 pixels, so an experiment's low-light-count regression
  bound is tight. The experiment is
  [LOTUS-LIGHT-06](../roadmap/current.md#deferred-adapter-work). Its
  candidates are the policy's: a light hierarchy traversed by the crossing
  test, or the lights as procedural primitives in a ray-tracing
  acceleration structure (lights neither block rays nor each other, so every
  crossed light must be found, not only the nearest).
- **Defer: selection.** Its cost is constant per shading point and bounded
  by the remainder above. A light tree or importance distribution would be
  adopted for lower variance, not lower cost; that needs a quality
  comparison with spatially varied light power, which neither this
  campaign nor the current fixtures provide.

## Limits

| Missing | Reason / next evidence |
| --- | --- |
| BSDF-only sampling | Not measured. Under it, `ParallelLights` adds a second per-light loop at every scattering event; the Phase 3 measurement found BSDF sampling pays more. |
| Selection alone | Not isolated from shadow rays and BSDF evaluation. Only its upper bound (the remainder) is reported; it does not grow with the light count. |
| Crossing inside the remainder | The probe still stores each wavefront path's ray origin for crossing, so the crossing estimate slightly undercounts. |
| Other light shapes | Only spheres. Rect and disk tests were not timed; distant lights are tested only on escape. |
| Hydra and viewport | The crossing loop is shared core shader code; neither frontend was measured. |
| Controlled timing | Clocks were unlocked and per-process bimodal; best-of-three medians are used and every process is kept. |
| More than 256 lights, 1080p/4K | Not measured; Phase 3 measured up to 1,028 lights at 128 pixels. |

## Verification

- `ost build --jobs auto` (core Release): passed, including the renderer
  report.
- `ctest -R "lotus-renderer-benchmark($|-mis|-no-driver|-wavefront)"`: 4/4
  passed. The MIS benchmark test now also runs a side-2 grid (4 analytic
  lights, recorded side) and rejects a grid without `many-lights-v1`, side 0
  and side 17 without touching output.
- Campaigns: `build/light-cost-campaign-20261025.py` with output in
  `build/light-cost-grid-20261025` and `build/light-cost-probe-20261025`;
  `build/light-cost-analyze-20261025.py` assembled the summaries.
- The probe build's renderer report failed as expected (its images are
  wrong); its executable and shaders were copied before the shader was
  restored and the core tree rebuilt and checked against the committed
  shaders.
- Not rerun: the full gpu and research tiers, Hydra, viewport and `ci-core`
  trees.
