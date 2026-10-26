# Frame-state residency at 1080p and 4K

## Evaluation brief

LOTUS-MEMORY-01's first step: the
[workstream contract](../design/ROADMAP_POLICY.md#practical-foundation-workstreams)
asks for an inventory of temporal/SVGF buffers, their owners, lifetimes and
peak residency, with host mirrors separately, and for 1080p/4K estimates
plus measured peaks where the hardware permits. Until now the fixed
benchmark reported `total_vram_bytes` as unavailable: scene pool statistics
and the per-product byte fields of `TemporalProducts` and `RestirProducts`
covered parts of the renderer, without alignment, and nothing measured the
process.

This change adds two observations:

- **`OffscreenRenderer::Residency()`** ([contract](../reference/SCENE.md#renderer-residency))
  lists every allocation the renderer makes itself, grouped by owner and
  memory kind, at its Vulkan allocation size: the scene pools' blocks and
  each dedicated buffer or image of the targets, wavefront state, temporal,
  reservoir and presentation paths. The memory kind comes from the same
  memory-type choice the creation helpers make.
- **`VK_EXT_memory_budget`**, enabled when the device exposes it, reports
  the driver's estimate of the whole process's usage of each heap. The
  difference between it and the renderer's groups is what the renderer does
  not allocate itself: driver objects, pipelines, descriptor and query pools.

`lotus-headless --benchmark-residency` renders `cornell-v1` in seven modes,
each on a fresh renderer, and samples `Residency()` after creation, after
the initial upload and after each of three one-sample frames. Frames keep
their products on the GPU (`read_back = false`), as
[Hydra](../../adapters/hydra2/src/adapter.cpp) and the
[viewport](../../adapters/viewport/scenes/builtin_scene.cpp) render:

| Mode | Settings |
| --- | --- |
| `progressive-reference` | `RenderScene`, reference integrator |
| `progressive-wavefront` | `RenderScene`, wavefront integrator |
| `temporal-metadata` | `RenderTemporalScene`, metadata and validity only |
| `temporal-radiance` | plus `accumulate_radiance` |
| `svgf` | plus 5 filter iterations |
| `restir-reuse` | metadata plus `RestirInitial` with temporal and spatial reuse |
| `svgf-restir` | radiance, 5 filter iterations and both reuse passes |

Conditions: Windows AMD64, MSVC 19.51.36256.0 Release, NVIDIA RTX A5000
(24 GiB), driver 597.16, Vulkan 1.4.329, validation and synchronization
validation active. Built from `5e598b6` plus this change; binary SHA-256
`7f3e52bc60377b91…` (full hash and every value below in the
[data](2026-10-26-frame-residency.json)). One process each at 1280x720,
1920x1080 and 3840x2160, then a 1920x1080 control with the validation layer
disabled (`VK_LOADER_LAYERS_DISABLE=VK_LAYER_KHRONOS_validation`).

## Correctness

- All 28 mode runs passed with zero validation messages; every frame
  reported the expected radiance and reservoir availability.
- In every sample the groups sum exactly to the reported device-local and
  host totals (also checked by `lotus-renderer-residency-benchmark` at
  40x24, with each owner's allocation count and per-pixel lower bound).
- The validation-disabled control produced identical groups and identical
  heap usage in all seven modes: the layer allocates no device or
  host-visible heap memory here.

## Inventory

Bytes per pixel at 3840x2160 (allocation sizes divided by pixels; image
alignment makes the scene targets 38.4 B at 1280x720). "Host" is
host-visible system memory; ".readback" owners are the mirrors frames copy
their products into.

| Owner | Kind | Allocations | B/pixel | Created by | Held for |
| --- | --- | ---: | ---: | --- | --- |
| `targets.scene` | device | 3 | 36.3 | first scene frame at a size | accumulation RGBA32F, colour RGBA32F, depth D32 |
| `targets.scene.readback` | host | 2 | 20 | same | colour and depth products |
| `wavefront.state` | device | 3 | 92 | first wavefront frame | queue counters, three 4-byte queues, five 16-byte path sections |
| `temporal.surfaces` | device | 2 | 128 | first temporal frame | current and previous 64-byte `TemporalPixel` |
| `temporal.surfaces.readback` | host | 1 | 128 | same | both surfaces' products |
| `temporal.validity` | device | 1 | 16 | same | `TemporalValidityPixel` |
| `temporal.validity.readback` | host | 1 | 16 | same | validity product |
| `temporal.radiance` | device | 3 | 80 | first radiance frame | two 32-byte histories, 16-byte fresh batch |
| `temporal.radiance.readback` | host | 1 | 80 | same | histories and batch products |
| `temporal.filter` | device | 1 | 48 | first filtered frame | two ping-pong halves and the guides |
| `restir.initial` | device | 1 | 48 | first `RestirInitial` frame | `RestirPixel` |
| `restir.initial.readback` | host | 1 | 48 | same | initial reservoirs |
| `restir.history` | device | 2 | 384 | first reuse frame | two 192-byte `RestirHistoryRecord` histories |
| `restir.history.readback` | host | 1 | 384 | same | temporal and previous reuse records |
| `restir.spatial` | device | 2 | 208 | first spatial frame | 192-byte spatial input and 16-byte sources |
| `restir.spatial.readback` | host | 2 | 208 | same | spatial records and sources |

Outside the per-pixel owners: `scene.pools` (16 MiB device and 4 MiB host
blocks for `cornell-v1`), `constants.path` (256 B), `temporal.metadata`
(1,088 B for 272-byte constants and the instance records) and
`wavefront.readback` (32 KiB). `present.view` (8 B/pixel, RGBA16F) exists
only with presentation, which this headless campaign does not create.

Lifetimes, from the code rather than this campaign: every owner is created
by the first frame that needs it and persists until the renderer is
destroyed. Scene targets and wavefront state are replaced when the target
size changes, temporal surfaces, validity, radiance and filter buffers on a
temporal resize; reservoir buffers only grow. Switching a mode
off (for example the viewport's denoiser or reuse) keeps its buffers.

## Totals

Renderer groups and the process's heap usage after the last frame, MiB:

| Mode | 1080p device | 1080p host | 1080p process device heap | 4K device | 4K host | 4K process device heap |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `progressive-reference` | 91.9 | 43.6 | 236.2 | 302.9 | 162.2 | 447.2 |
| `progressive-wavefront` | 273.9 | 43.6 | 418.3 | 1,030.6 | 162.2 | 1,175.0 |
| `temporal-metadata` | 376.7 | 328.3 | 521.0 | 1,441.9 | 1,301.3 | 1,586.2 |
| `temporal-radiance` | 534.9 | 486.5 | 679.4 | 2,074.8 | 1,934.1 | 2,219.0 |
| `svgf` | 629.8 | 486.5 | 774.3 | 2,454.4 | 1,934.1 | 2,598.7 |
| `restir-reuse` | 1,642.3 | 1,593.9 | 1,786.7 | 6,504.4 | 6,363.8 | 6,648.7 |
| `svgf-restir` | 1,895.5 | 1,752.1 | 2,040.0 | 7,516.9 | 6,996.6 | 7,661.2 |

The sampled peak equals the last frame in every mode: allocations made by
the frames persist, and no sample saw a larger value.

### What the groups explain

At every size and in every mode, the process's device-local heap usage
exceeds the renderer's device-local groups by 144.3–144.5 MiB, and its host
heap usage exceeds the host groups by 48.3–56.3 MiB. Before any scene the
process already uses 93.8 MiB of device memory with only the 256-byte path
constants allocated. The residual does not grow with resolution or mode, so
the groups account for all of the frame-sized growth: no frame state is
allocated outside the inventory.

### Estimates

Frame-state bytes per pixel, from the inventory (scene targets rounded to
36):

- device-local: 36 + 92 (wavefront) + 144 (temporal metadata and validity)
  + 80 (radiance) + 48 (filter) + 640 (reservoirs: 48 initial, 384 history,
  208 spatial);
- host mirrors: 20 + 144 + 80 + 640, with no filter or wavefront mirror.

The fixed parts are the scene pools and the measured 144 MiB device / about
50 MiB host residual. These reproduce the measured totals above, and give
for SVGF without reuse 310 B device and 244 B host per pixel: 613 MiB and
483 MiB of frame state at 1080p, 2.39 and 1.88 GiB at 4K.

## Candidates and decisions

1. **Host mirrors without readback — go.** Both interactive hosts render
   with `read_back = false`, so no frame they render copies into a mirror,
   yet every mirror is allocated: 884 B per pixel in `svgf-restir`, 1.71
   GiB of host-visible memory at 1080p and 6.83 GiB at 4K; 244 B (483 MiB)
   at 1080p with SVGF alone. Allocating a mirror only when a frame reads its
   product back changes no GPU computation, history or precision. The
   [viewport timing report](2026-10-18-viewport-timing.md) observed the
   reservoir half of this. Next MEMORY-01 slice: lazy mirrors, measured with
   this benchmark and checked for unchanged products when read back.
2. **Reservoir history representation — deferred, leading device cost.**
   Reuse holds 640 B per pixel on the device, 4.94 GiB at 4K, of which the
   two 192-byte history records are 384 B. Compacting them (half-precision
   or octahedral fields, dropping fields the receiver can reconstruct)
   changes stored values, so it needs the reservoir CPU-oracle, bias and
   moving-scene comparisons the strategy's
   [adoption rules](../design/POST_V060_STRATEGY.md#4-measurement-and-adoption)
   require. Compare after the mirrors.
3. **Lifetime reuse and aliasing — deferred.** The spatial input (192 B)
   and the filter's ping-pong halves (32 B) live within a frame, as does the
   fresh batch (16 B); aliasing them needs a barrier and pass-order
   analysis, and the whole saving is below candidate 2's.
4. **Temporal surfaces — deferred.** Two 64-byte records are inherent to
   reprojection (current and previous); compacting identities or normals
   touches the shared validity contract and the CPU oracle.
5. **Release on mode switch — deferred.** Disabled modes keep their
   buffers; worth a measurement in the viewport once the mirrors are lazy.

Wavefront state (92 B) belongs to the progressive wavefront path, not to
temporal or denoise state, and stays outside this task.

## Unavailable and unmeasured

- Continuous peaks: samples are taken between calls. Transient allocations
  inside a call (scene staging and scratch live in the persistent pools;
  `ReadBackScene`'s readback is validation only) are not sampled.
- Swapchain image memory is in the heap usage only; the viewport and Hydra
  were not run at these sizes. Their frame state uses the same backend
  owners.
- Other GPUs and drivers, integrated memory and devices without
  `VK_EXT_memory_budget` (the report then gives null heap usage).
- Bandwidth and time for any candidate: no representation was changed.
