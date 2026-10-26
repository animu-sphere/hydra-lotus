# Lazy readback mirrors

## Evaluation brief

The [frame-residency inventory](2026-10-26-frame-residency.md#candidates-and-decisions)
found every host readback mirror allocated with its device buffer although
neither interactive host reads frames back: the viewport and Hydra's
presentation render with `read_back = false`. In `svgf-restir` the mirrors
were 884 B per pixel, 1.71 GiB of host-visible memory at 1080p and 6.83 GiB
at 4K. That candidate was marked go, to be measured with the same benchmark
and checked for unchanged products when read back.

This change allocates each mirror with the first frame that needs it:

- **Scene targets** (bootstrap and scene): the colour and depth mirrors are
  created by the first frame at a size with `read_back`, or one restoring
  preserved contents, which stages its upload through them. They persist
  with the targets and are replaced with them.
- **Temporal surfaces, validity and radiance**, and the **initial, history
  and spatial reservoirs**: each mirror is ensured only when the frame has
  `read_back`, and then persists like its device buffer.
- `TemporalProducts::surface_bytes`, `validity_bytes` and `radiance_bytes`
  count the mirrors only once allocated, as `RestirProducts::reservoir_bytes`
  already did by summing allocations.

Device buffers, passes, barriers and copies are unchanged; a frame without
`read_back` recorded no mirror copy before either. The wavefront counter
readback (32 KiB) is read by every wavefront frame and stays.

`lotus-headless --benchmark-residency` gains `--residency-read-back <0|1>`
([contract](../reference/SCENE.md#renderer-residency)); its CTest now runs
every mode both ways.

Conditions as in the inventory: Windows AMD64, MSVC 19.51.36256.0 Release,
NVIDIA RTX A5000 (24 GiB), driver 597.16, Vulkan 1.4.329, validation and
synchronization validation active, `cornell-v1`, three one-sample frames per
mode on a fresh renderer. Built from `d348446` plus this change; binary
SHA-256 `0ce46e274152ca25…` (full hash and every value in the
[data](2026-10-26-lazy-readback-mirrors.json)). One process at 1920x1080 and
one at 3840x2160 without readback, then a 1920x1080 control reading every
frame back. "Before" is the inventory's campaign.

## Correctness

- All 21 mode runs passed with zero validation messages.
- Without readback, no `.readback` owner exists in any mode at either size,
  and every device-local group (owner, allocation count and bytes) is
  identical to before.
- The read-back control reproduces the inventory's groups exactly in all
  seven modes, mirrors included.
- `lotus-renderer-restir-gpu` adds a mirror lifecycle check under both
  integrators: SVGF (5 iterations) with temporal and spatial reuse at 16x16
  on two renderers, one reading every frame back, the other rendering two
  GPU-only frames first. The GPU-only frames allocate no mirror and return
  no host products; the third frame allocates the same mirrors, reports the
  same byte fields and returns byte-identical colour, depth, temporal
  surfaces, validity, radiance histories, batch and reservoir products, so
  the history produced without mirrors is the same history.
- `lotus-renderer-temporal-gpu` and the rest of `lotus-renderer-restir-gpu`,
  which read back every frame and compare against their CPU oracles, pass
  unchanged.

## Results

Renderer host groups and the process's host-visible heap usage after the
last frame, MiB. Device-local groups and the process's device-local heap
usage are unchanged in every mode (for example `svgf-restir` 1,895.5 /
2,040.0 MiB at 1080p and 7,516.9 / 7,661.2 MiB at 4K).

| Mode | 1080p host before | after | 1080p process host heap before | after | 4K host before | after | 4K process host heap before | after |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `progressive-reference` | 43.6 | 4.0 | 91.9 | 52.3 | 162.2 | 4.0 | 210.5 | 52.3 |
| `progressive-wavefront` | 43.6 | 4.0 | 99.9 | 60.4 | 162.2 | 4.0 | 218.6 | 60.4 |
| `temporal-metadata` | 328.3 | 4.0 | 384.6 | 60.3 | 1,301.3 | 4.0 | 1,357.6 | 60.3 |
| `temporal-radiance` | 486.5 | 4.0 | 540.3 | 60.3 | 1,934.1 | 4.0 | 1,987.9 | 60.3 |
| `svgf` | 486.5 | 4.0 | 540.3 | 60.3 | 1,934.1 | 4.0 | 1,987.9 | 60.3 |
| `restir-reuse` | 1,593.9 | 4.0 | 1,647.8 | 60.3 | 6,363.8 | 4.0 | 6,417.6 | 60.3 |
| `svgf-restir` | 1,752.1 | 4.0 | 1,806.0 | 60.3 | 6,996.6 | 4.0 | 7,050.4 | 60.3 |

The 4.0 MiB left are the scene pools' host block, the path constants and
the temporal metadata; none scales with resolution. The removed host bytes
equal the mirror groups to the byte, and the process's host heap fell by the
same amount: the residual above the renderer's groups stays at 48–56 MiB, so
the driver did not keep the memory in another form.

## Decision

Adopted. GPU-only frames now hold no frame-sized host memory; SVGF with
both reuse passes saves 1.71 GiB of host-visible memory at 1080p and
6.83 GiB at 4K, SVGF alone 483 MiB and 1.88 GiB, with no device-side or
product change.

Next in LOTUS-MEMORY-01: the reservoir history representation (the
inventory's candidate 2), now the leading frame-state cost at 640 B per
pixel on the device under reuse, which needs the reservoir CPU-oracle,
bias and moving-scene comparisons before adoption.

## Unavailable and unmeasured

- Time: the first read-back frame after GPU-only frames pays the mirrors'
  allocation and mapping; that cost and the frame time of GPU-only frames
  were not measured (no recorded work changed for them).
- Release: a mirror allocated by one read-back frame persists while later
  frames are GPU-only, as device buffers do on a mode switch (the
  inventory's candidate 5, still deferred).
- The viewport and Hydra were not run; they use the same backend paths
  with `read_back = false`. Hydra's CPU render-buffer path reads back every
  frame and keeps its mirrors.
- Other GPUs, drivers and memory architectures, as in the inventory.
