# Viewport timing HUD and stage selection

Measured 2026-10-18 on Windows 11, MSVC 14.51 Release, Vulkan SDK 1.3.290
with the Khronos validation layer, NVIDIA RTX A5000 (driver 597.16), clocks
not locked. Source base: main `f58ece0` plus the LOTUS-VIEWPORT-04 change.
OpenUSD 26.08 from the OpenStrata `cy2026` `lookdev` runtime for the
Hydra-enabled build.

## Question

[LOTUS-VIEWPORT-04](../roadmap/current.md#step-2--viewport-foundation) asks
`lotus-viewport` to show the frame time; the GPU path-trace, ReSTIR, temporal
and (once it exists) denoise pass times; the sample count; the scene and TLAS
update time; and the reservoir and history memory. The backend already
reports each of these in its frame and scene evidence
([GPU timestamps](2026-10-05-gpu-timestamps.md)); the viewport showed only
one GPU total. This report records how the HUD splits that total, whether
the split adds up, and what the figures show at 1280x720. The same change
lets the Hydra-enabled viewport open a stage from a file dialog, as
hydra-merlin's viewport does.

## What changed

- **Figures.** `adapters/viewport/timings.{hpp,cpp}` turns a frame's
  evidence into named passes and keeps the HUD's record: the mean of the
  latest frames (up to 32) that ran the same passes, a 120-frame history of
  frame and GPU times, the latest scene update with GPU work, and memory. It
  is GPU-, window- and USD-free; `lotus-viewport-timings` covers it with
  synthetic evidence. `FrameGpuMs`, which the title and benchmark already
  used, moved there.
- **Passes.** The scene pass's timestamps enclose the wavefront kernels, the
  reservoir passes and, under reservoir reuse, the temporal surfaces recorded
  before them; temporal surfaces without reuse, and temporal accumulation,
  are a later submission. The HUD's path trace is therefore the scene pass
  less the reservoir passes and the temporal surfaces it contains, with the
  wavefront kernels shown as its parts. The path trace, ReSTIR initial,
  temporal and spatial, temporal surfaces and temporal accumulation add up
  to the frame's GPU time, which the unit test checks for NEE/MIS, wavefront,
  initial-only, combined reuse and temporal radiance. Spatial reuse alone
  runs the temporal reservoir pass too, to resolve the records it reads.
- **Scene update.** `FrameResult` carries the scene update's
  `GpuSceneTimings` and the scene's `GpuSceneStats`. The built-in scene has
  them from `UpdateScene`; `HdLotusRenderDelegate::GetGpuSceneTimings`
  returns the latest render pass's, cleared at the start of each pass.
- **Timing window.** A second Dear ImGui window beside the Renderer window,
  hidden with it by F1. Both now keep a fixed width and follow their contents'
  height, so passes appearing at run time do not scroll.
- **Benchmark.** `--benchmark` adds each pass's mean over the frames that
  ran it (`gpu_pass_average_ms`, `null` for a pass that never ran), the scene
  update count and the latest update, and the memory figures. Existing keys
  keep their meaning.
- **Open USD.** In the Hydra-enabled build the Renderer window has an
  **Open USD...** button: the native file dialog of nativefiledialog-extended
  1.3.0, the commit hydra-merlin pins, filtered to `usd, usda, usdc, usdz`.
  As in hydra-merlin, a picked stage ends the frame loop; `main` then renders
  it in the same window with a new Hydra scene and renderer, the renderer
  controls as they were, through the free camera at the stage's start time.
  The stage is opened before the current scene is torn down, so one that does
  not open leaves the current scene running and the reason in the window.
  The built-in scene can switch to a stage the same way.

## The split adds up

The HUD's means at 1280x720 with vsync on (so the host frame time is the
display's 16.7 ms), validation layer active, the built-in Cornell box:

| Pass (ms) | Wavefront, combined reuse | Wavefront, temporal radiance |
| --- | --- | --- |
| Path trace | 1.818 | 3.087 |
| — wavefront camera / intersect / shade / tail | 0.178 / 0.444 / 0.805 / 0.146 | 0.265 / 0.660 / 1.533 / 0.277 |
| ReSTIR initial / temporal / spatial | 0.430 / 1.466 / 1.793 | — |
| Temporal surfaces | 0.446 | 1.211 |
| Temporal accumulation | — | 0.290 |
| **GPU** | **5.954** | **4.589** |

Each column's passes add up to its GPU total. The clocks were not locked
and the GPU idles between vsync-paced frames, so these are indicative; the
[reuse-cost measurement](2026-10-17-restir-reuse-cost.md#timing-procedure)
owns timing claims. The ReSTIR temporal and spatial passes take 55% of the
combined frame, and the wavefront shade kernel is the largest part of the
path trace under both.

The built-in scene's one scene update, before frame 1, took 0.002 ms of
upload, 0.088 ms of BLAS builds and 0.048 ms of TLAS build. Through Hydra,
the smoke stage's interaction test made four updates with GPU work, the last
before frame 6: 0.045 ms of TLAS build or refit and no BLAS work.

## Memory

At 1280x720 (921,600 pixels) the HUD shows:

| Category | Size | Per pixel |
| --- | --- | --- |
| Reservoirs (combined reuse) | 1,125.00 MiB | 1,280 B |
| Surface history | 253.13 MiB | 288 B |
| Radiance history | 126.56 MiB | 144 B |
| Scene (Cornell box) | 0.05 MiB used of 20.00 MiB reserved | |
| BLAS + TLAS | 0.03 MiB | |

The reservoir figure is `RestirProducts::reservoir_bytes` and matches the
backend's buffers: initial reservoirs 48 B and their mirror 48 B, two
192-byte history records and a 384-byte mirror, the spatial input 192 B and
its mirror 192 B, and the spatial sources 16 B and their mirror 16 B. Half
of it, 640 B per pixel (562.5 MiB here), is host-visible readback mirrors
that a presented frame never reads since
[LOTUS-VIEWPORT-03](2026-10-18-viewport-controls.md#what-changed) stopped
reading them back. The backend allocates them on first use regardless of
`OffscreenTarget::read_back`. This change does not alter that; it is a
candidate for whoever next touches reuse memory, since the temporal surfaces
and radiance history carry mirrors in the same way.

## Stage selection

The Open USD flow was driven on a visible 1280x720 window by a script that
clicked the button, typed the smoke stage's path into the Windows file
dialog and pressed Enter, with `--validate`. The stage replaced the Cornell
box in the same window; the developer UI came back on the new renderer, the
file name showed beside the button, and controls set before the switch
(temporal and spatial reuse) applied to the stage.

The first attempts lost the device at the new renderer's second frame, three
times out of three. The cause was in the developer UI: a Hydra scene creates
its renderer during its first render, so on that frame the UI had no backend
to build a frame with, and the new renderer's overlay drew the previous
frame's ImGui draw data, which names the font texture the old backend had
destroyed. The overlay now draws only a frame built since its backend was
initialized. Of five recorded switches after the fix, four were clean and
one lost the device at frame 2, the intermittent loss described under
[Limits](#limits).

## Regression

Targeted at the change, not the full suites (the
[release gate](2026-10-18-v0.5.2-release-gate.md) reruns them):

| Run | Result |
| --- | --- |
| Cornell box `--interaction-test`, 40 frames, validation | 30/30 steps, zero messages, nothing read back |
| `usdview-smoke.usda` `--interaction-test`, 40 frames, validation (Hydra-enabled build) | 35/35 steps, zero messages |
| `ctest -R lotus-viewport`, core viewport tree | 7/7 |
| `ctest -R lotus-viewport`, Hydra-enabled viewport tree | 8/10; `lotus-viewport-present` and `lotus-viewport-hydra-camera` lost the device at frame 2, and both passed on rerun |
| `lotus-viewport-timings`, `-controls`, `-options`, `-camera` | 4/4 |

## Limits

- **Intermittent `VK_ERROR_DEVICE_LOST` at frame 2**, as in
  [LOTUS-VIEWPORT-03](2026-10-18-viewport-controls.md#limits): six hidden
  five-frame launches each of the core and the Hydra-enabled viewport lost
  the device once. It predates the developer UI
  ([LOTUS-VIEWPORT-02](2026-10-17-viewport-camera.md#regression)); its cause
  is not established.
- No denoise pass exists to time; [LOTUS-SVGF-03](../roadmap/current.md#step-4--svgf-in-the-viewport)
  adds it to the HUD.
- The present blit, the debug-view kernel and the overlay have no
  timestamps; the host frame time covers them.
- Memory is as the latest frame that used each category reported it. Reuse
  buffers stay allocated after the controls stop using them, and the HUD
  marks them retained; renderer attachments and accumulation are outside
  every figure.
- The file dialog blocks the frame loop while open. A picked stage starts
  without `--camera` or `--time`, and recreates the renderer, so its
  accumulation and history start again. The built-in-scene-only build has no
  dialog. The dialog was exercised on Windows only.
