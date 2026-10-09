# First ray-traced triangle

Measured on 2026-10-05 in the working tree based on `b52c07f`, Windows 11
x86_64, MSVC 14.51, OpenStrata 0.23.14, Release. Vulkan headers came from SDK
1.3.290; `slangc` 2026.8 came from SDK 1.4.350. The GPU was NVIDIA RTX A5000,
device API 1.4.329 and loader API 1.4.321. Hydra used OpenUSD 26.08 lookdev
and CPython 3.13. These are local runs; hosted CI was not measured.

## Scope

The Renderer Phase 1 *first ray-traced triangle* milestone: camera rays
traverse the scene's BLAS/TLAS and the closest triangle writes diagnostic
barycentric RGB and projected depth. Hydra uses the same pass on ray-query
devices; the standalone viewport retains its bootstrap raster pass. Current
API and behaviour are in the [scene reference](../reference/SCENE.md#primary-rays).

This run contains no BSDF, lighting or accumulation. Each diagnostic pixel
gets one centre ray, with no jitter, RNG or seed. Camera and scene inputs
are fixed by the fixture; the comparisons below check intersection output.

## Independent image comparisons

`renderer.ray_query.triangle` projects the CPU snapshot's transformed
triangles into screen space, computes screen-space intersection weights,
perspective-corrects the barycentrics and selects the nearest window depth.
It compares these results with GPU colour/depth readback. It does not reuse
the shader's inverse-camera ray construction or GPU scene readback geometry.

The initial triangle's points are `(-0.65, -0.35, 0)`, `(0.55, -0.45, 0)`
and `(0.15, 0.70, 0)`. The initial perspective camera looks down -Z from
`(0, 0, 3)`, with 45-degree vertical FOV, aspect 1 and clipping range [1, 10].
The clear colour is `(0.05, 0.1, 0.15, 0)` and clear depth is 1. Cases cover:

- insertion and an identical repeated frame, including exact colour/depth
  payload equality and target reuse;
- a transform-only TLAS refit with translation, nonuniform scale and shear;
- visibility off, then a point edit that also tilts the triangle out of its
  original plane and makes it visible again;
- two overlapping instances, selecting the closer hit;
- wholly near-clipped and wholly far-clipped triangles;
- an infinite-far perspective camera and an orthographic camera;
- display window `(-8, 5, 80, 50)` and data window `(16, 8, 32, 40)` in a
  64×64 target, then a resize to 96×48;
- singular-camera rejection followed by successful rendering;
- removal of the final mesh, empty-scene clears and no-clear preservation.

For every case, interior hit and miss pixels are compared with a tolerance
of two UNORM byte values per colour channel and `2e-5` in depth. Samples
within 0.015 of an edge in screen barycentric coordinates are excluded to
avoid treating device edge precision as an intersection error. Clipping
fixtures are wholly inside or outside the depth range; they do not validate
CPU clipping of triangles that cross a plane.

`renderer.ray_query.capability` reports extension/feature support. On this
GPU it passed. `renderer.ray_query.timestamp` measured the initial 64×64
perspective render pass, including clears and excluding readback, using a
persistent query pool: the core test reported `0.008288` ms and the Hydra
test `0.008128` ms. These single timing samples check the measurement path;
they are not a throughput benchmark.

## Hydra and installation

`lotus-renderer-hydra-aov` checks the identity-camera triangle's centre
barycentrics, then translates it by `(0.6, 0, 0)`: the original centre pixel
becomes a miss and a pixel to its right becomes a hit. It also checks topology
replacement, visibility, full clears and no-clear preservation through the
same existing AOV path.

The usdview smoke test checks that Hydra selects ray queries when the
headless capability assertion passes. Its evidence log reported
`ray_query=1`, one persistent renderer and zero validation messages for
every frame. First-frame and stable-update PNGs were produced under the
Hydra build's `adapters/hydra2/usdview-install/` and visually inspected;
the edited triangle displays the expected barycentric colour gradient.
The install-tree tests exercise the installed primary-ray SPIR-V files,
with no source-tree shader fallback.

## Verification

The documented Japanese-MSVC dependency workaround was checked before the
final builds: zero missing-dependency objects in core, Hydra and viewport;
eight were removed from runtime-free ci-core and rebuilt.

- `ost build --jobs auto`, `ost test`: **8/8 passed**.
  `ost validate --strict-renderer-evidence`: **passed**; the three ray-query
  assertions and Vulkan validation assertion passed. Hydra checks are
  explained configuration SKIPs.
- `ost build --profile lookdev --intent hydra --jobs auto`,
  `ost test --profile lookdev --intent hydra`: **14/14 passed**, including
  AOV and usdview integration.
  `ost validate --profile lookdev --intent hydra --strict-renderer-evidence`:
  **passed**, all renderer assertions OK and zero Vulkan validation messages.
- `ost build --without-runtime --intent ci-core --jobs auto`,
  `ost test --without-runtime --intent ci-core`: **8/8 passed**. All three
  ray-query assertions SKIP with "Vulkan backend was not compiled for this
  configuration".

The Vulkan device without ray-query support and the queue without timestamp
support were not available on this machine, so those hardware paths remain
unmeasured. Synchronization validation was not enabled. Remaining transport
work is in the [roadmap](../roadmap/README.md#status-at-a-glance).
