# Foundation follow-up: viewport camera and header discovery

- Date: 2026-10-04 (JST)
- Build: working tree based on `90c3b14`, with the camera and boundary changes
  described below; Release, MSVC 14.51, OpenStrata 0.23.14 on Windows.
- GPU: NVIDIA RTX A5000, Vulkan device API 1.4.329, loader API 1.4.321.
- Runtime: `cy2026-windows-x86_64-py313-core`.
- Scene: the scaffold's one world-space triangle, with a 45-degree vertical
  perspective camera three units away and clipping range [1, 10]. This is a
  raster bootstrap check: sample count and RNG seed do not apply.

## Changes exercised

The viewport now supplies `Lotus::Camera` to `RenderWorld`, commits it and
extracts its world-to-clip matrix before presenting. Its aspect comes from
framebuffer pixels, so resizing updates the camera. The presentation backend
already used the extracted matrix and converted it to Vulkan clip space.

The boundary checker recursively discovers public headers under
`include/lotus/` when it runs, including backend headers. A fixture test
introduces a new nested `.inl` file containing OpenUSD, Vulkan, GLFW or Slang
includes and confirms that each fails the actual checker. Standard-library
includes pass. This remains a token-based source check.

## Results

| Run | Result |
| --- | --- |
| `ost build --jobs auto` | Passed |
| `ost test` | 5/5 passed, including the two new tests |
| `ost validate` | Passed; Hydra checks skipped by configuration |
| `ost renderer viewport -- --frames 8 --hidden` | 8 frames presented, zero swapchain recreations, no validation messages |
| `ctest --test-dir build/cy2026-windows-x86_64-py313-core--renderer-viewport --build-config Release --output-on-failure` | 6/6 passed, including presentation and install-tree checks |
| `ost validate --intent renderer-viewport` after regenerating managed evidence | Passed; `renderer-viewport` and `renderer.validation.messages` OK |

`lotus-viewport-camera` runs without GLFW or Vulkan. It checks projected
world-space offsets against equal pixel distances at 720×720, 1280×720 and
720×1280, the homogeneous coordinate and expected depth, a changed scene
revision on aspect change, a stable revision for an unchanged camera, and
rejection of zero extents. The GPU smoke check presents eight frames; it does
not read the swapchain back or exercise a live window resize.

The headless evidence still passes colour, depth and persistence checks over
1,000 frames, a repeated size and a target resize.

## Workflow observation

`ost test --intent renderer-viewport` is rejected because this is a renderer
workflow intent, not a project-declared build intent. Direct CTest works, but
the existing install-tree test merges an assertion into the primary
`renderer-report.json`. That changes the digest bound to the managed build,
so a subsequent `ost validate --intent renderer-viewport` fails.

A no-op viewport build did not repair that binding. For this run, forcing
the viewport tree's headless executable to relink regenerated the report
during `ost renderer viewport`; validation then passed. The final managed
viewport evidence therefore has `tested` and `renderer.install_tree` as
explained SKIPs, while the separate direct CTest run above passed all six
tests. The core's managed test and validation run passed normally.

## Remaining foundation work

The camera path and automatic discovery items are complete. Renderer Phase 0
still needs its AOV contract and generated CI lanes; synchronization
validation remains an open testing-infrastructure item in the
[roadmap](../roadmap/current.md).
