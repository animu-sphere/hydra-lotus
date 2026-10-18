# Viewport device loss at frame 2

Measured 2026-10-18 on Windows 11 Pro (build 26200, console session), MSVC
Release, Vulkan SDK 1.3.290 (loader 1.4.321), NVIDIA RTX A5000 with driver
597.16, both displays on the A5000 and an AMD Radeon integrated GPU also
installed. Source base: main `eab5796`. The probes below were temporary
instrumentation, removed afterwards; this change alters no code.

## Question

[LOTUS-VIEWPORT-02](2026-10-17-viewport-camera.md#regression),
[-03](2026-10-18-viewport-controls.md#limits) and
[-04](2026-10-18-viewport-timing.md#limits) recorded an intermittent
`VK_ERROR_DEVICE_LOST` from the second frame's scene fence, cause not
established. What triggers it, and can the renderer avoid it?

## Answer

The first ray-query submission after a `vkQueuePresentKHR`, when it follows
the present within about a second, faults on the GPU in roughly half of the
runs. The fault is an instruction-pointer fault plus an invalid read, both at
addresses outside every allocation the renderer made. Nothing tried on the
application side changed the rate. The only workarounds found were to stop
presenting or to stop tracing rays. No newer driver exists for this GPU, and
the driver's present-method setting made no difference. The evidence points
at the driver. The defect is not fixed here.

## Reproduction

The core viewport tree (`build/<target>--renderer-viewport`), a hidden
window, three frames:

```sh
VK_LOADER_LAYERS_DISABLE=VK_LAYER_KHRONOS_validation \
  adapters/viewport/lotus-viewport.exe --hidden --frames 3 --vsync off
```

A failing run prints `frame 2: scene frame failed: vkWaitForFences failed
with VkResult -4` after about two seconds, and the system log records a pair
of `nvlddmkm` event 153 entries (`Error occurred on GPUID: 100`). The backend
enables the Khronos validation layer, with synchronization validation,
whenever the layer is installed. The layer slows the host enough to lower the
rate, and with it disabled the failure is about three times as frequent:

| Configuration | Failed runs |
| --- | ---: |
| Validation layer present (default), incremental build | 3/12 |
| The same tree after `cmake --build --target clean` | 2/12 |
| Validation layer disabled | 19/30 |

A clean build does not change the rate, so stale objects (the
[`#deps 0` constraint](../reference/SUPPORTED_CONFIGURATIONS.md#build-and-tooling-limitations))
are not involved.

## Where it fails

Timing every submission, with the queue idled after the present's own
submission and again after `vkQueuePresentKHR`:

| Step | Failing run |
| --- | --- |
| Frame 1 scene submission | complete in 2.4–2.8 ms |
| Present 0: blit and overlay submission | queue idle after 0.1–0.3 ms |
| Present 0: `vkQueuePresentKHR` | returns `VK_SUCCESS`, queue idle after 0.04 ms |
| Frame 2 scene submission | device lost after 2.4–2.6 s |

The present's work finishes before frame 2 is submitted, and frame 2's
scene update uploads and rebuilds nothing (no BLAS, TLAS, instance, material
or light writes after frame 1). The
Crash Diagnostic Layer (`VK_LAYER_LUNARG_crash_diagnostic`) placed the
GPU after the frame's last `vkCmdDraw` and before its `vkCmdEndRenderPass`.
`VK_EXT_device_fault` reported:

| Fault | Address |
| --- | --- |
| Instruction pointer fault | `0x2200C5FA10`–`0x2200C5FA1F` |
| Invalid read | `0xA000000000`–`0xA000000FFF` |

Neither range lies inside any buffer or image the application bound. The
nearest known allocation is a 3.9 MB image at `0x16400000`. The shader's
code address became unreadable, which no application binding controls.

## What it depends on

Each row changes one thing against the default run; counts are failed runs.

| Change | Failed | Reading |
| --- | ---: | --- |
| `--frames 30` | 6/10, all at frame 2 | only the second frame fails |
| `--vsync on` (FIFO) | 1/10 | present mode irrelevant |
| `--integrator wavefront` | 1/10 | compute ray queries fail too |
| Visible window, validation off | 5/12 | not a hidden-window effect |
| Present skipped entirely, 5 frames | 0/24 | presenting is required |
| Present skipped entirely, 2000 frames | 0/10 | |
| No overlay (developer UI) | 4/20 | |
| No blit to the swapchain image | 4/20 | |
| No `vkQueuePresentKHR` (blit submitted, 2 frames) | 0/20 | the present call is the trigger |
| Frame 2 draws nothing (clears only) | 0/20 | |
| Frame 2 draws only the depth pass (one primary ray per pixel) | 3/20 | any ray-query draw fails |
| Frame 2 draws only the radiance pass | 5/20 | |
| 32x32 window, validation off | 13/20 | not the amount of work |
| 1600x1600 window, validation off | 9/20 | |
| Background GPU load holding P0 (1890/8001 MHz), validation off | 10/20 | not a clock transition |
| Present from a second queue of the same family, validation off | 17/30 | not queue-specific |
| Depth and radiance pipelines created again after the first present, validation off | 20/30 | not the existing pipeline objects |
| NVIDIA Control Panel present method "Prefer native", validation off | 18/30 | not the layered-on-DXGI path |

Moving the first ray-query frame, either by waiting after presents or by
drawing nothing in the first frames, moves the failure with it
(validation off, six frames):

| Change | Failed | Failing frame |
| --- | ---: | --- |
| 1 s wait after present 0 | 6/12 | 3 |
| 1 s wait after presents 0 and 1 | 8/12 | 4 |
| 1 s wait after presents 0–2 | 3/12 | 5 |
| 300 ms wait after present 0 | 8/12 | 2 |
| Frame 1 draws nothing | 15/20 | 2 |
| Frames 1–2 draw nothing | 15/20 | 3 |
| Frames 1–3 draw nothing | 9/20 | 4 |

A ray-query frame before any present never failed. A frame that started
a second or more after the preceding present never failed. The first
ray-query frame that followed a present within that time failed in about
half of the runs, and once one passed, no later frame failed. With the
validation layer present, a 300 ms wait after every present gave 0/12 over
eight frames; at that rate this is not conclusive.

## Ruled out

- Application synchronization or lifetime errors. Validation with
  synchronization validation reports nothing in these runs. The faulting
  addresses are outside every allocation. Recreating the pipelines changes
  nothing.
- Vulkan layers. The registered implicit layers (Steam overlay, Steam
  Fossilize, Epic Online Services overlay) are not loaded into the process.
  NVIDIA App and its overlay are not installed.
- Cross-adapter presentation. Both displays are driven by the A5000.
- An outdated driver. NVIDIA's driver lookup for this GPU (`DEV_2231`) names
  597.16, "NVIDIA RTX Driver Release 595" (2026-09-22), as the newest.

One process difference was observed: the driver loads `nvppex.dll` only into
a process that creates a swapchain (it is absent from `lotus-headless`). No
new modules are loaded by the presents themselves.

## Consequences

- The scripted presentation tests (`lotus-viewport-present`,
  `lotus-viewport-hydra-camera` and the other launches with `--frames`) fail
  intermittently on this host, and a rerun is the remedy; the validation
  layer, which the backend enables whenever it is installed, lowers the rate.
- Interactive use loses the device at startup in the same proportion; a
  viewport that passes its second frame keeps running.
- Headless and Hydra offscreen rendering do not present and are unaffected.

## Not done

- An application-side recovery, recreating the renderer when the device is
  lost before the first frame has passed, was not implemented. It would hide
  a driver fault behind a retry, and its success depends on the next device
  not failing in the same way.
- No minimal reproducer outside this renderer was written for a driver
  report.
- Other GPUs, drivers and Linux were not measured.
