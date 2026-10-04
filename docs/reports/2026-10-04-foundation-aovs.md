# Foundation AOV formats, clears and empty scenes

- Date: 2026-10-04 (JST).
- Build: working tree based on `5f52457`, with the changes below; Release,
  MSVC 14.51, OpenStrata 0.23.14 on Windows.
- GPU: NVIDIA RTX A5000, Vulkan device API 1.4.329, loader API 1.4.321.
- Runtimes: `cy2026-windows-x86_64-py313-core` and
  `cy2026-windows-x86_64-py313-lookdev` (OpenUSD 26.08).
- Scenes: the headless bootstrap triangle with a 45-degree perspective
  camera three units away, clipping [1, 10]; a 16x16 Hydra test triangle
  through identity camera matrices; the installed usdview smoke scene.
  These are raster checks: sample count and RNG seed do not apply.

## Changes exercised

The [AOV reference](../reference/AOVS.md) defines the foundation formats:
linear RGBA8 colour, Float32 window depth, and Int32 ID sentinel buffers.
`primId`, `instanceId` and `elementId` do not yet identify geometry.

The GPU background now matches the colour descriptor's transparent-black
clear. Explicit colour/depth clears reach the Vulkan render pass, and a
clear applies to the full target independently of the data window. Empty
draw summaries render clears instead of failing. The Hydra adapter no
longer returns early when the last visible mesh disappears.

The backend loads persistent attachments and issues explicit clears when
requested. Empty clear values retain preceding colour/depth contents at the
same size. The Hydra adapter preserves ID buffers when their clear is empty.
Arbitrary AOV buffer-set switching is outside this foundation implementation.

Before GPU work, the adapter checks the whole binding set for supported
names, matching formats, common positive extents, unmapped buffers, duplicate
names, and clear value types/ranges. Malformed bindings emit a Hydra error
and leave the buffers unconverged without partial writes.

## Results

| Run | Result |
| --- | --- |
| `ost build --jobs auto` | Passed; GPU evidence generated |
| `ost test` | 5/5 passed, including install-tree validation |
| `ost validate` | Passed; Hydra checks skipped by configuration |
| `ost build --profile lookdev --intent hydra --jobs auto` | Passed |
| `ost test --profile lookdev --intent hydra` | 10/10 passed, including AOV integration and usdview |
| `ost validate --profile lookdev --intent hydra` | Passed; all fourteen renderer assertions OK |

`renderer.aov.clears` checks a cropped frame's cleared centre, a subsequent
empty frame's entire colour/depth payload, attachment preservation without
clears, and target reuse when clears change. The existing evidence checks
1,000 frames, a repeated size and a resize. No Vulkan validation messages
were captured over these frames or the usdview smoke frames.

`lotus-renderer-hydra-render-buffer` checks default descriptors, unsupported
formats, multisampling and volume rejection, mapped-buffer write/allocation
rejection, row flipping, and sentinel storage. It is CPU-only.

`lotus-renderer-hydra-aov` renders a visible test mesh through the adapter,
hides it, checks preservation with empty clear values, then clears every
pixel to a custom colour/depth/ID sentinel. It checks invalid names, formats,
duplicate names, mismatched extents, mapped buffers, wrong clear types,
non-finite/out-of-range depth, and recovery with a depth-only pass. Missing
GPU capability is a CTest SKIP (return code 77), not a PASS.

The GPU converted a 0.5 colour clear to byte 127 on this device. The UNORM
checks allow one byte of rounding error rather than requiring byte 128.

## Remaining work

Renderer Phase 0 still needs the generated CI boundary lanes. Real ID
outputs and float radiance follow geometry extraction and the reference path
tracer. Restoring arbitrary AOV buffer sets with no clear is a later adapter
follow-up. Synchronization validation remains an open infrastructure item.
