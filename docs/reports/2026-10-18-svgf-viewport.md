# SVGF in the viewport

Measured 2026-10-18 on Windows 11, MSVC 14.51 Release, Vulkan SDK 1.3.290
with the Khronos validation layer, NVIDIA RTX A5000 (driver 597.16), clocks
not locked. Source base: main `68e2829` plus the LOTUS-SVGF-03 change.
OpenUSD 26.08 from the OpenStrata `cy2026` `lookdev` runtime for the
Hydra-enabled builds. The [temporal reference](../reference/TEMPORAL.md#edge-aware-filtering)
owns the filter's contract, the [building guide](../guides/BUILDING.md#the-standalone-viewport)
the viewport's usage and the [roadmap](../roadmap/README.md#status-at-a-glance)
phase status.

## Question

[LOTUS-SVGF-03](../roadmap/current.md#step-4--svgf-in-the-viewport) asks
the standalone viewport to switch the SVGF denoiser at run time, compare the
raw and filtered images, show the history and variance and time the
denoiser live. The [SVGF comparison](2026-10-18-svgf-filter.md#cost) left
viewport-resolution cost to this step, with packing the filter's guides into
a compact buffer as the obvious reduction. This report records whether the
controls and views work under validation, what the filter costs at viewport
resolution before and after that reduction, and whether Renderer Phase 6
meets its exit criteria.

## What changed

- **Controls.** `Controls` gains `denoise`, `filter_iterations` (1–8,
  default 5), `split` with its position and the variance view's log10 span.
  The denoiser filters the temporal radiance, so it turns it on; changing
  either leaves `PlanFrame`'s fresh-batch settings and the history key as
  they were, so the history is kept. `--denoise`, `--filter-iterations`,
  `--split`, F2 and the developer UI's **Denoise (SVGF)** section set them.
- **Views.** `DebugView` adds `RawRadiance` (the fresh batch),
  `AccumulatedRadiance` (the unfiltered temporal mean), `HistoryLength` and
  `Variance`, which the present kernel reads from the latest radiance
  frame's GPU buffers, as the other views read theirs; nothing is read
  back. They need a radiance frame and fail before any GPU work otherwise.
  The two radiance views write linear colour, which the blit encodes as it
  encodes beauty.
- **Split.** `PresentView::split` blits the view's left columns and the
  frame colour's right ones, each scaled as the whole frame is; the
  developer UI draws the divider. Beauty is the filtered composite on a
  filtered frame.
- **Timing.** The HUD's passes add `Variance` and `Denoise filter`
  (`temporal_variance` and `temporal_filter` in the benchmark JSON), and its
  memory the filter's buffer. The frame's GPU time now counts the variance
  pass, which it had left out since LOTUS-SVGF-01.
- **Hydra.** `lotus:filterIterations` (int, 0, clamped to [0, 8]) filters
  `lotus:temporal`'s colour and is unused without it; the viewport's stage
  path sets it from the controls. The frame loop fails a run whose frame
  filtered with another iteration count than the controls asked for, so
  the scripted runs check that the setting reaches the backend.
- **Compact guides.** On a filtered frame the variance pass also writes the
  first iteration's input (illumination and the variance of its mean) and a
  16-byte guide per pixel (placement identity, depth, packed shading
  normal, or a mark for a pixel without a history). The iterations read
  only those and the previous iteration's values, instead of a 64-byte
  surface record, a 32-byte radiance record and, for the first iteration,
  the albedo at every tap. The arithmetic is the same expression for
  expression; the buffer grows from 32 to 48 bytes per pixel.

## Checks

| Run | Result |
| --- | --- |
| `lotus-viewport-options`, `-controls`, `-timings` (fast) | pass: options, plans, view availability, split clamping, restarts, pass sums and filter memory |
| `lotus-renderer-temporal-gpu` (core) | pass: 38 filtered frames per integrator against the CPU oracle at every pixel, maximum relative error 1.20997e-5 before and after the compact guides; filter memory 48 bytes per pixel |
| `lotus-viewport-interaction`, `lotus-viewport-present`, `lotus-viewport-no-driver` | pass |
| `lotus-renderer-hydra-temporal-contract`, `lotus-renderer-hydra-temporal-gpu` | pass: the setting's default, a filtered pass that keeps history, the setting unused without `lotus:temporal` |
| `lotus-viewport-hydra-interaction`, `-hydra-camera` | pass |
| `lotus-viewport-hydra` | pass, and fails with `VK_ERROR_DEVICE_LOST` at frame 2 in two of six runs: the [known driver fault](2026-10-18-viewport-device-loss.md), in a run that does not denoise |

The interaction test now takes 33 control steps after the scene's (38 in
all for the Cornell box; 48 frames, was 40): after temporal radiance, the
denoiser on; the raw, accumulated,
history-length and variance views; the split; one iteration; temporal
radiance off with the denoiser still on; the denoiser off. Each restarted as
`Restarts` predicts, every frame filtered with the iterations planned, and
the run, with synchronization validation active, reported no validation
message and read nothing back.

## The views

Captured on a visible 960x600 Cornell box window, five iterations, the
camera still:

- **Raw against beauty (split):** one-sample noise left of the divider, the
  filtered image right of it.
- **Accumulated against beauty (split):** the unfiltered temporal mean is
  still grainy after its 32 samples; the filtered half is smooth, with
  the soft blotches the [comparison](2026-10-18-svgf-filter.md#static-convergence)
  measured as bias.
- **Variance:** highest around the emitter and on the ceiling, lower on the
  boxes and walls facing away from the light, per-pixel noise in the
  temporal estimate; misses black.
- **History length:** every surface at the cap (dark red at full scale)
  once the view has been held; the scripted runs show it after resets.

## Cost

1280x720, built-in Cornell box, hidden, `--vsync off`, 300 frames,
validation layer disabled through the loader, reference integrator. Means
per frame from `--benchmark`; the second row of each pair is a repeat run.

| Mode | Path trace | Surfaces | Accumulation | Variance | Filter | GPU frame | Host frame |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Temporal radiance | 1.42 | 0.75 | 0.20 | 0.12 | — | 2.49 | 3.33 |
| Denoise, before the guides | 1.46 | 0.77 | 0.20 | 0.12 | 11.62 | 14.17 | 15.16 |
| Denoise, compact guides | 1.37 | 0.76 | 0.22 | 0.22 | 2.42 | 4.98 | 5.79 |
| Denoise, compact guides (repeat) | 1.40 | 0.75 | 0.20 | 0.22 | 2.40 | 4.96 | 5.81 |

Milliseconds. Five iterations cost 2.4 ms at 1280x720, 4.8 times less than
before; writing the guides adds 0.1 ms to the variance pass. Denoising
doubles the GPU frame of temporal radiance alone and keeps the viewport near
170 frames per second. Clocks were not locked; the GPU was at P0 (1,875 MHz)
right after the repeat, and repeats agree to 1%.

## Renderer Phase 6 exit

| [Exit criterion](../design/ROADMAP_POLICY.md#renderer-phase-6--denoising--svgf-class-pipeline) | Evidence |
| --- | --- |
| Temporal reprojection, moments and variance under both integrators | [moments and variance](2026-10-18-svgf-moments-variance.md), every pixel against independent batches and a CPU oracle |
| À-trous filtering under both integrators | [filter](2026-10-18-svgf-filter.md#checks), every pixel against a CPU oracle; unchanged here |
| Motion and disocclusion rejection under both integrators | [shared validity](2026-10-12-temporal-gpu-validity.md); [disocclusion response](2026-10-18-svgf-filter.md#moving-camera-and-disocclusion) |
| Compared with a reference | [comparison](2026-10-18-svgf-filter.md#comparison): raw, temporal and filtered against a high-spp reference, static and moving, bit-for-bit reproducible |
| Switched on and off, with diagnostics, in the standalone viewport | this report |

Every debug product the phase names can be inspected: raw radiance,
reprojected history (`previous_radiance` at the validity's index), validity,
history length, moments, variance, filtered output and rejection reason
from `TemporalProducts`, and all but the reprojected history and moments as
viewport views. The phase's rule holds: the denoiser reads the renderer's
noisy radiance and the shared surface metadata only, under NEE/MIS,
initial-only and reusing ReSTIR alike. Renderer Phase 6 meets its exit
criteria. Firefly suppression and feature-adaptive filtering remain after
the baseline, as the [comparison](2026-10-18-svgf-filter.md#after-the-comparison)
routes them.

## Regression

Run after the change: the fast viewport tests and `lotus-renderer-temporal-gpu`
in the core configuration; the viewport-labelled tests in the viewport
tree; the Hydra temporal tests in the `hydra` configuration; the three
`lotus-viewport-hydra*` tests in the Hydra-enabled viewport tree. Not
rerun: the rest of the core `gpu` tier and the Hydra suite (the backend
change is limited to the present kernel, `Present`'s product selection and
the filter's buffer, which `lotus-renderer-temporal-gpu` covers), the
`research` tier (the SVGF campaign's output is unchanged, as the oracle
comparison shows), the runtime-free `ci-core` configuration and the
installed hosts. The v0.6.0 [release gate](../roadmap/current.md#step-5--release-gate)
runs them all.

## Limits

- One scene and resolution for the cost; the filter's cost grows with the
  pixels and the iterations, and does not depend on the scene's lights.
- The history-length view at a static camera is uniform; its colour map
  spans the history samples, not the batches.
- The split compares two products of the same frame, not two frames: it
  cannot show the filter's temporal stability, which the
  [comparison](2026-10-18-svgf-filter.md#moving-camera-and-disocclusion)
  measures.
- The viewport has no moments view; `TemporalProducts` and the CPU oracle
  remain the place to read them.
