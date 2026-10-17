# Viewport renderer controls and debug views

Measured 2026-10-18 on Windows 11, MSVC 14.51 Release, Vulkan SDK 1.3.290
with the Khronos validation layer, NVIDIA RTX A5000 (driver 597.16), clocks
not locked. Source base: main `b0e7a76` plus the LOTUS-VIEWPORT-03 change.
OpenUSD 26.08 from the OpenStrata `cy2026` `lookdev` runtime for the
Hydra-enabled build.

## Question

[LOTUS-VIEWPORT-03](../roadmap/current.md#step-2--viewport-foundation) asks
`lotus-viewport` to switch the integrator, spp and accumulation, NEE/MIS,
ReSTIR off/initial/temporal/spatial/combined and temporal radiance reuse
while running, and to show beauty, depth, normal, motion vectors, temporal
validity and reservoir validity, age and weight from the existing AOVs and
debug products, without viewport-only renderer state. Reuse frames read back
and checked their reservoir and temporal products every frame, 28–36 ms at
128x128 ([measurement](2026-10-17-restir-reuse-cost.md#wall-time-and-memory)),
so presented reuse needed a path that skips them. This report records whether
each control restarts or continues the accumulation as it should, whether
the views show the products they name, and what the presenting path costs.

## What changed

- **Controls.** `adapters/viewport/controls.{hpp,cpp}` holds the renderer
  controls and turns them into each frame's plan: `PathTracingSettings`,
  whether the frame goes through `RenderTemporalScene`, the temporal settings
  and the view to present. It also says which views are available and which
  changes restart the accumulation. It is GPU-, window- and USD-free;
  `lotus-viewport-controls` covers it. The command line sets the controls the
  run starts with (`--integrator`, `--nee`, `--restir`, `--temporal-radiance`,
  `--no-accumulation`, `--view`).
- **Developer UI.** `adapters/viewport/developer_ui.{hpp,cpp}` is one Dear
  ImGui window (1.92.8, the commit hydra-merlin's viewport pins), its GLFW
  input and Vulkan renderer backends private to that file. F1 hides it;
  pointer and keyboard input it takes does not reach the camera. The
  swapchain is sRGB, so the style's colours are decoded first. It writes no
  `imgui.ini`.
- **Overlay.** As in hydra-merlin, the backend owns the drawing:
  `PresentOptions::overlay` is called to initialize with the device and a
  render pass compatible with the swapchain images, to record into a
  load-preserving render pass over each presented image after the blit, and
  to shut down with the device idle. The swapchain images gain colour
  attachment usage and per-image framebuffers only when an overlay is set.
- **Debug views.** `Present` takes a `PresentView`. A view other than beauty
  runs one compute dispatch (`backend/vulkan/shaders/present/present_view.slang`)
  that reads the latest frame's product from its existing GPU buffer as raw
  words at its record stride and writes display colours to an RGBA16F image,
  which is blitted in place of the scene colour. Depth, normal, motion and
  temporal validity read the current temporal surfaces and validity buffer;
  the reservoir views read the final reservoirs, the reused records after
  temporal or spatial reuse and the initial ones otherwise. The renderer
  keeps no new state beyond which products the latest frame wrote; a view
  whose product it did not write fails before any GPU work and leaves the
  renderer usable. The temporal views render through `RenderTemporalScene`,
  which leaves a progressive accumulation as it is. The Hydra adapter takes
  the view through `HdLotusRenderDelegate::SetPresentView`, and the viewport
  maps its controls onto the delegate's existing render settings.
- **Presenting path.** `OffscreenTarget::read_back` false now also leaves the
  temporal and reservoir records on the GPU and skips the host check of the
  reservoirs' arithmetic flags. Availability, extents, byte counts and GPU
  times are reported as before. The viewport's readback check counts these
  records too, so a presented frame that copied any of them fails the run.
- **Restart after reuse.** A progressive frame after a fresh batch
  (temporal radiance or reservoir reuse) restarts the accumulation. Before,
  it continued from the fresh sample whenever the batch's sample index
  matched the progressive key, so whether leaving reuse restarted depended on
  history.

## Controls and views, scripted

`--interaction-test` now follows the scene's navigation and edit steps with
25 control steps, from the run's controls and back, each checked by the next
frame's samples per pixel as before
([camera report](2026-10-17-viewport-camera.md#scripted-interaction)). Run:
`--hidden --frames 40 --vsync off --validate --interaction-test`, Cornell box.

| Step | Expected | Cornell box |
| --- | --- | --- |
| wavefront integrator | restart | 1 spp |
| sample limit 512 | continue | 2 spp |
| view depth, normal, motion, temporal validity (progressive) | continue | 3, 4, 5, 6 spp |
| NEE/MIS off; reference integrator; ReSTIR initial | restart | 1 spp each |
| view reservoir validity, age, weight (initial-only) | continue | 2, 3, 4 spp |
| 4 candidates | restart | 1 spp |
| ReSTIR temporal, spatial, combined | restart | 1 spp each |
| view reservoir age, temporal validity (under reuse) | restart | 1 spp each |
| wavefront, still combined | restart | 1 spp |
| ReSTIR off; temporal radiance; view motion; temporal radiance off; accumulation off; the run's controls | restart | 1 spp each |

All 30 steps, the five navigation steps included, behaved as expected, with
zero validation messages and nothing read back over 40 presented frames.
Every view was therefore presented under validation at least once, and each
reservoir view both from initial-only and from reused reservoirs.

## The views show their products

Each view was captured on a visible 960x600 Cornell box window:

- **Normal:** each wall and box face one flat colour, 0.5 + 0.5 n.
- **Depth:** white near, grey at the back wall, over the free camera's
  clipping range (1.98–4.04), misses black.
- **Temporal validity** (static camera, combined reuse): every surface
  accepted (green), misses black.
- **Reservoir validity** (combined reuse): green except the emitter, the
  short block's front face and the tall block's left face, dark red ("no
  candidate target"). The emitter has no other light to sample; the short
  block's front face lies at z ≈ 0.6 and the tall block's left face at
  x ≈ −0.65, both facing away from the emitter (z ≤ 0.35, |x| ≤ 0.35), so
  every candidate's target is zero there. Beauty lights those faces only
  indirectly.
- **Reservoir age** shows the same faces at age 0 and the rest as per-pixel
  heat noise; **reservoir weight** shows log10 W between −2 and 2, dark red
  where no sample was selected.

The motion view was presented in the scripted runs only; a static camera
shows uniform mid-grey.

## Presenting path cost

Same sources, built twice: as committed (`read_back` false), and with
`read_back` true in the built-in scene, a temporary edit reverted afterwards.
Hidden, `--vsync off`, 300 frames, validation layers disabled through the
loader as in the [reuse-cost measurement](2026-10-17-restir-reuse-cost.md#timing-procedure),
reference integrator (wavefront within 0.1–0.3 ms of it). Wall time is the
host's render and present per frame, the developer UI included.

| Mode | 128x128 evidence path | 128x128 presented | 1280x720 presented |
| --- | --- | --- | --- |
| NEE/MIS | 1.72 ms (wavefront) | 0.40 ms | 1.80 ms |
| Initial-only | 4.40 ms | — | 2.55 ms (wavefront) |
| Temporal | 32.6 ms | 0.58 ms | 3.97 ms |
| Spatial | 33.4 ms | 0.68 ms | 5.61 ms |
| Combined | 42.3 ms | 0.66 ms | 5.69 ms |
| Temporal radiance | 17.0 ms | 0.50 ms | 2.82 ms |

The 128x128 evidence path reproduces the reuse-cost report's 28–36 ms for
temporal and spatial reuse on another scene; presented, reuse costs 0.2–0.3
ms more than NEE/MIS per frame there and 2–4 ms at 1280x720. At 1280x720 the
evidence path takes 100 ms per NEE/MIS frame and 1.09 s per temporal-radiance
frame. A pinpoint rerun on an idle GPU gave 0.39, 0.57 and 0.62 ms for
NEE/MIS, temporal and combined at 128x128. The clocks were not locked and
another GPU workload ran during part of the campaign, so the presented
figures are indicative; the gap is fifty-fold. GPU times on the evidence path
are higher than presented (1.06 against 0.19 ms for temporal reuse) because
the GPU idles between frames and clocks down, as the reuse-cost report
found.

## Findings fixed on the way

- **First reservoir candidate left unselected.** The evidence path failed
  1280x720 Cornell box runs under initial-only, temporal and spatial reuse at
  frame 121 with "initial reservoir arithmetic is invalid". The pixel,
  (971, 578), held a weight sum of 0.0703 from one candidate and no selected
  sample. The first positive candidate is selected with probability
  w / w = 1, but the GPU's float quotient can fall just below 1, so a random
  number within about 1e-7 of 1 left it unselected; the next candidate then
  failed the reservoir's consistency check. That is about one draw in 10^7,
  a few per hundred 1280x720 frames and none in the 128x128 fixtures. Both
  `StreamReservoirCandidate` and `MergeReevaluatedReservoir` now select the
  first positive weight unconditionally, as the CPU oracle's double quotient
  does. After the fix, temporal reuse ran 200 evidence-path frames at
  1280x720.
- **Viewport GPU time.** The title and the benchmark JSON added the wavefront
  kernels to `primary_ray_gpu_ms`, which already contains them, as it
  contains the reservoir passes and, under reuse, the temporal surfaces. The
  viewport now adds only the temporal passes submitted after the scene pass.
  The [presentation report](2026-10-17-viewport-presentation.md)'s wavefront
  GPU figures carry the double count.

## Regression

Targeted at the change, not the full suites:

| Run | Result |
| --- | --- |
| Cornell box `--interaction-test`, 40 frames, validation | 30/30 steps, zero messages, nothing read back |
| `usdview-smoke.usda` `--interaction-test`, 40 frames, validation (Hydra-enabled build) | 35/35 steps (10 navigation and stage steps, 25 control steps), zero messages |
| `lotus-viewport-controls`, `lotus-viewport-options`, `lotus-viewport-camera` | 3/3 |

The full core, Hydra and `ci-core` suites were not rerun for this report.

## Limits

- **Intermittent `VK_ERROR_DEVICE_LOST` at frame 2.** Runs stopped at the
  second frame's scene fence with the device lost, each with a pair of
  `nvlddmkm` event 153 GPU resets, under every mode, more often while another
  GPU workload ran on the host. It predates this change: main `b0e7a76` and
  this change, built in the same tree and run alternately (`--hidden --frames
  5`, validation layers off), failed 1 and 2 of 8 runs. It was also seen
  during [LOTUS-VIEWPORT-02](2026-10-17-viewport-camera.md#regression). Its
  cause is not established.
- The presented path no longer checks the reservoirs' arithmetic flags on the
  host; the reservoir validity view shows a flagged pixel in magenta instead.
- The motion view was not captured on screen with a moving camera.
- The reservoir views show the last fresh sample's final reservoir; with
  initial-only reservoirs and a full accumulation, that is the last sample
  added.
- The developer UI has no timing breakdown; that is
  [LOTUS-VIEWPORT-04](../roadmap/current.md#step-2--viewport-foundation).
