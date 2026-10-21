# Viewport camera and accumulation

Measured 2026-10-17 on Windows 11, MSVC 14.51 Release, Vulkan SDK 1.3.290
with the Khronos validation layer, NVIDIA RTX A5000 (driver 597.16). Source
base: main `02dc81e` plus the LOTUS-VIEWPORT-02 change. OpenUSD 26.08 from
the OpenStrata `cy2026` `lookdev` runtime for the Hydra-enabled build.

## Question

[LOTUS-VIEWPORT-02](../archive/pre-v070-execution-plan.md#step-2--viewport-foundation) adds
orbit, pan and dolly, authored camera selection, accumulation restarted on
camera change and scene dirty updates to `lotus-viewport`, and the stage
time that the [presentation report](2026-10-17-viewport-presentation.md#limits)
deferred. This report records whether each input and edit restarts the
progressive accumulation exactly when it changes the image, and continues it
otherwise.

## What changed

- **Free camera.** `adapters/viewport/orbit_camera.{hpp,cpp}` is usdview's
  FreeCamera without OpenUSD: a centre, distance, yaw, pitch and roll over
  the authored up axis, usdview's framing and clipping from the scene bounds,
  and its tumble (a quarter degree per pixel), track and dolly steps. It is
  GPU-, window- and USD-free, and both scenes use it. The built-in Cornell
  box starts at the headless bootstrap view (three units away, 45 degrees);
  its clipping planes now follow the box, which changes only the depth range.
- **Input.** The window layer reports pointer buttons, cursor motion in
  framebuffer pixels, the wheel and modifiers (`adapters/viewport/input.hpp`).
  Left drag tumbles, middle or Ctrl + left drag tracks, right drag and the
  wheel dolly, F restores the initial framing; Alt, which usdview needs, is
  accepted and not required.
- **USD stages.** C cycles through the stage's camera prims and back to the
  free camera; `--camera` starts on one. The render pass state then defers
  to `HdRenderPassState`, which conforms the authored camera to the window by
  its window policy. Navigating from an authored camera hands its position,
  orientation, roll and conformed vertical field of view to the free camera,
  as usdview does. `--time` and Left/Right set the time code; R reloads the
  layers. Every frame calls `UsdImagingDelegate::SetTime`, which applies the
  stage's pending edits before moving to the time code.
- **Accumulation.** The backend already restarts an accumulation when the
  world-to-clip matrix or the scene generation changes; the viewport relies
  on that rather than tracking restarts itself.
- **Failures are not skips.** A Hydra frame that failed (for example with
  `VK_ERROR_DEVICE_LOST`) was reported as an environment that cannot render:
  exit 77 with an empty `SKIP:` reason, which CTest counts as skipped. It
  now fails the run, and every frame error names its frame.

## Scripted interaction

`--interaction-test` applies one step after each frame from the second and
checks the next frame's samples per pixel: 1 for a step that must restart the
accumulation, one more than before for a step that must continue it. Inputs
go through the same event handling as the window's. Runs: `--hidden --frames
16 --vsync off --validate --interaction-test --allow-unavailable`.

| Step | Cornell box | `usdview-smoke.usda` |
| --- | --- | --- |
| time forward on a static stage (default time to 0) | — | continued (3 spp) |
| add a time-sampled mesh in the session layer | — | restarted |
| time forward with that mesh (0 to 1) | — | restarted |
| remove the session-layer mesh | — | restarted |
| next camera (C): `/Camera` | — | restarted |
| tumble from the authored camera | — | restarted |
| tumble (left drag) | restarted | restarted |
| track (middle drag) | restarted | restarted |
| dolly (wheel) | restarted | restarted |
| frame (F) | restarted | restarted |
| next camera (C) without authored cameras | continued (2 spp) | — |

Both runs presented 16 frames with zero validation messages. Before the
tumble from `/Camera`, the step compares the free camera that took over the
authored view with it: the view matrices and the projection's focal terms
agree within 1e-6, so the hand-over itself does not move the image.
`lotus-viewport-hydra-camera` starts on `/Camera` at time code 1 and presents
4 frames, validation-clean.

## The checks detect the defects they target

Two mutations, each built and run, then reverted:

| Mutation | Result |
| --- | --- |
| No `SetTime` per frame, so pending stage edits are not applied | `lotus-viewport-hydra-interaction` fails: "add a time-sampled mesh in the session layer left 4 samples per pixel, expected 1" |
| Roll dropped when taking over an authored view | `lotus-viewport-camera` fails four of its six take-over cases (the two at 90 degrees pitch, where roll and yaw coincide, still pass) |

`lotus-viewport-camera` also checks the framing distance and clipping against
usdview's formulas, the projection against `GfFrustum`'s, square pixels on
square, landscape and portrait extents, that a track keeps the centre under
the cursor to 1e-9 pixels, the dolly steps and bounds, and the take-over at
Y and Z up.

## Regression

| Tree | Result |
| --- | --- |
| `core--renderer-viewport` (`ctest`) | 51/52 in 1074 s; `lotus-viewport-present` failed once, then passed 5 of 5 reruns |
| `lookdev--viewport-usd--renderer-viewport` (`ctest`) | 82/82 in 1374 s, including `lotus-renderer-usdview-host` |
| runtime-free `ci-core` (`ost test`) | 38/38, the two GPU tests skipped |

The failed run stopped at frame 2 with `VK_ERROR_DEVICE_LOST` from the scene
frame's fence wait, in the presentation smoke test's configuration, which
this change does not alter. The same intermittent loss, a GPU reset a few
seconds after the second frame's submission, appeared in repeated runs of
both scenes while other GPU work was running on the host; it was not
investigated further here.

## Limits

- `ost renderer viewport --intent viewport-usd` launches
  `adapters/hydra2/usdview-install/bin/lotus-viewport.exe` in that tree, a
  copy that the `lotus-renderer-usdview-host` test installs, rather than the
  executable it has just built; until that test runs again the launched
  viewport is stale. Run `run-lotus-viewport.cmd` beside the built executable
  instead ([building guide](../guides/BUILDING.md#the-standalone-viewport)).
- Taking over an orthographic camera keeps the free camera's field of view,
  and aperture offsets are not carried over.
- Stage edits from the session layer and time codes are scripted; a reload
  (R) of an edited file was not scripted, since it goes through the same
  `SetTime` path as the session-layer edits.
- The scripted runs are hidden and use synthetic inputs. Mouse navigation
  was checked by hand on a visible Cornell box window (1124 frames presented,
  282 samples per pixel at exit, so it restarted during the session); the
  hand check did not navigate a USD stage or an authored camera's hand-over,
  which only the scripted run covers.
