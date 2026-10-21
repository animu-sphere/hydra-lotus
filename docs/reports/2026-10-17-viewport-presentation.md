# Viewport presentation and USD stages

Measured 2026-10-17 on Windows 11, MSVC 14.51 Release, Vulkan SDK 1.3.290
with the Khronos validation layer, NVIDIA RTX A5000. Source base: main
`72c5991` plus the LOTUS-VIEWPORT-01 change. OpenUSD 26.08 from the
OpenStrata `cy2026` `lookdev` runtime for the Hydra-enabled build. GPU clocks
were not locked, so the times below describe the frame loop, not renderer
performance.

## Question

[LOTUS-VIEWPORT-01](../archive/pre-v070-execution-plan.md#step-2--viewport-foundation) makes
`lotus-viewport` present the renderer's progressive output instead of the
bootstrap triangle, open a USD stage as
[`hydra-merlin`'s viewport](https://github.com/animu-sphere/hydra-merlin/blob/main/docs/guides/viewport.md#opening-usd-stages)
does ([DES-Q8](../design/DESIGN_POLICY.md#53-open-questions)), and present
normal frames without a CPU readback. This report records whether the
presentation loop is stable across resizes, validation-clean, readback-free
and explained where it cannot run.

## What changed

- **Backend.** `CreateOffscreenRenderer` takes optional `PresentOptions`
  (the window layer's surface provider and vsync). The renderer then owns the
  surface and swapchain on its own device, and `OffscreenRenderer::Present`
  blits the latest scene frame's RGBA32F colour into an 8-bit sRGB swapchain
  image. The blit's format conversion does the sRGB encoding and clamps; no
  tone mapping is applied. `OffscreenTarget::read_back = false` skips the
  colour and depth copies. The bootstrap-only `PresentSession`, a second
  device that drew the triangle, is removed.
- **Hydra adapter.** `HdLotusRenderDelegate::SetPresentation` creates the
  renderer for the window; a render pass whose one AOV binding is colour with
  no render buffer renders at the pass state's viewport extent without a
  readback and presents. `lotus-hydra2-host-dependencies` exports `usd`,
  `usdGeom` and `usdImaging` for the viewport
  ([layout rule 4](../architecture/PROJECT_LAYOUT.md#4-dependency-directions)).
- **Viewport.** The default build path-traces the headless reference Cornell
  box. With `LOTUS_ENABLE_HYDRA2` (intent `viewport-usd`) `--usd` opens a
  stage: `HdLotusRenderDelegate`, `HdRenderIndex`, `UsdImagingDelegate`,
  `HdEngine` and the viewport's own task, framed as usdview frames a stage
  initially. F11 or `--fullscreen` switches to the primary monitor. Options
  and CTests are listed in the [building guide](../guides/BUILDING.md#the-standalone-viewport).

## Finding: resize while a blit is in flight

The first resize run reported one validation error,
`VUID-vkDestroyImage-image-01000`: recreating the scene targets destroyed the
colour image while the previous presentation's blit could still read it. A
renderer with a presentation surface now waits for its queue before it
recreates targets; afterwards every run below reported zero messages.

## Scripted runs

Each run: `--hidden --frames 8 --vsync off --validate --resize-test
--allow-unavailable --benchmark`, 1280x720 resized to 1344x752 after the
second frame, reference integrator with NEE/MIS, sample index 0.

| Scene | Frames presented | Swapchain recreates | Samples per pixel at exit | Readback bytes | Validation / synchronization validation | Messages |
| --- | --- | --- | --- | --- | --- | --- |
| Cornell box (built in) | 8 of 8 | 1 | 6 | 0 | active / active | 0 |
| `usdview-smoke.usda` through Hydra | 8 of 8 | 1 | 6 | 0 | active / active | 0 |

Six samples at exit show that the resize restarted the accumulation. Both
swapchains were `B8G8R8A8_SRGB` with `IMMEDIATE` presentation.

With Vulkan driver discovery pointed at a missing manifest, the viewport exits
77 and prints `SKIP: the Vulkan loader offers no window-system surface
extensions: no Vulkan driver presents`: without a driver the loader lists no
surface extensions, so the window layer stops before instance creation.

| Tree | Viewport CTests |
| --- | --- |
| `core--renderer-viewport` | `lotus-viewport-present`, `-no-driver`, `-options`, `-camera`: 4/4 |
| `lookdev--viewport-usd--renderer-viewport` | the same plus `lotus-viewport-hydra`: 5/5 |

## Frame loop cost

240 presented frames at 1280x720, `--vsync off`, `IMMEDIATE`, each run
accumulating 240 samples per pixel. CPU time is the viewport's per-frame wall
time around rendering and presentation, averaged over every frame including
the first, which creates the device and pipelines. GPU time is the scene
pass's timestamps (both scenes ran the reference integrator).

| Scene | Validation layer | CPU average per frame | GPU average per frame |
| --- | --- | --- | --- |
| Cornell box | enabled | 2.05 ms | 1.40 ms |
| Cornell box | disabled | 1.77 ms | 1.34 ms |
| smoke stage | enabled | 7.59 ms | 0.18 ms |
| smoke stage | disabled | 1.27 ms | 0.16 ms |

The validation layer was disabled with
`VK_LOADER_LAYERS_DISABLE=VK_LAYER_KHRONOS_validation`. The Hydra run's
enabled-layer average is dominated by its first frame, in which the delegate
creates the renderer with synchronization validation. Per-pass GPU timing in
the viewport, beyond the scene pass in the title bar and benchmark JSON, is
LOTUS-VIEWPORT-04.

## Visual checks

Visible 800x600 windows were captured after a few seconds: the Cornell box
matched the headless reference scene's layout and colours at 134 samples
per pixel; the smoke stage showed the orange triangle and the three
instanced triangles in their authored positions and orientations (upright,
rotated 90 degrees counter-clockwise, rotated 180 degrees) at 248 samples
per pixel. The images were inspected, not compared statistically; the
renderer's correctness evidence remains the headless and Hydra tests.

## Regression

| Build | Result |
| --- | --- |
| `core` (`ost build`, `ost test`, `ost validate`) | 49/49, validate passed |
| runtime-free `ci-core` | 38/38 |
| `hydra` intent, `lookdev` (`ost test`, `ost validate`) | 76/76 including `lotus-renderer-usdview-host` (1164 s), validate passed |

The `hydra` tree builds the adapter with the presentation path but no
viewport, so its tests cover the unchanged readback path.

## Limits

- One frame in flight: a frame waits for its own submission before the next
  one, and presentation follows on the same queue.
- No exposure or tone mapping: radiance above 1 clips.
- The stage is framed once; camera navigation, authored cameras and stage
  time are LOTUS-VIEWPORT-02.
- Fullscreen is exercised by hand only; the scripted runs are windowed. By
  hand: F11 took a 640x480 Cornell box window to 3840x2160 on the primary
  monitor, where it kept accumulating, and a second F11 restored the
  window's previous size.
