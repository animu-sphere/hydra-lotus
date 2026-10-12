# CPU temporal foundation

Measured on 2026-10-12, against `3251db0` plus LOTUS-TEMPORAL-01, on the
Windows workstation with an NVIDIA RTX A5000, using the existing OpenStrata
core Release configuration and MSVC. The CPU helper is built in the existing backend target and also
tested in the runtime-free, Vulkan-disabled configuration.

The [temporal reference](../reference/TEMPORAL.md) owns the API and limits;
the [roadmap](../roadmap/README.md#status-at-a-glance) owns phase status.
This report records CPU contract checks, not GPU temporal rendering or
image-quality evidence.

## Contract checks

CTest `lotus-renderer-temporal` passed in both configurations. Its scenarios
established:

- A rendered frame advances even at an unchanged scene revision, sharing
  immutable scene resources. Duplicate/decreasing indices and empty extents
  throw without consuming history. Explicit reset, cuts, frame gaps, extent
  changes and compatibility-key changes drop history for one frame and then
  recover. Reset releases retained scene resources; the largest representable
  consecutive frame indices remain usable.
- Mesh-key correspondence survives insertion earlier in scene-table order.
  Returned frames retain old camera/placement/scene data across later edits.
  Instancer and prototype transforms compose correctly. Changed instancing
  mode/count, visibility, geometry, removal and reinsertion reject
  correspondence. Binding, material and used-texture resource edits invalidate
  material history; an unrelated texture does not.
- Independent pinhole calculations match static and combined camera/object
  motion. A 90-degree, aspect-1 camera, near 1/far 11, projects object point
  `(1, 0.5, -4)` to `(80, 28)` in a 128 by 64 target. With object translation
  X +0.5 and camera view translation `(-0.25, 0, +1)`, its current pixel is
  `(90.666667, 26.666667)`, backward motion is `(-10.666667, 1.333333)`,
  and expected previous linear depth stays 4 while current depth is 3.
- Projection rejects behind-camera, near/far-clipped, out-of-frame and
  non-finite points/transforms and zero clip W. Top/left edges are included;
  bottom/right edges are excluded. A point outside the previous frame cannot
  reuse history. An orthographic camera with X/Y range -2 to 2 projects
  `(1, 0.5, -4)` to `(96, 24)`; view translation X -0.5 moves its current X
  to 80 with backward motion +16 pixels.
- Shared validity accepts that camera-depth change and nonunit but aligned
  normals. It rejects misses, different instances/materials, foreground and
  background disocclusion, normal discontinuity/reversal, diagonal large
  motion, invalid thresholds, NaNs and zero normals. Exact motion and absolute
  or relative depth threshold boundaries are accepted.

## Commands and regression evidence

The commands run from the repository root were:

```sh
ost build --jobs auto
ctest --test-dir build/cy2026-windows-x86_64-py313-core -R 'temporal|core-boundary' --output-on-failure
ost build --without-runtime --intent ci-core --jobs auto
ost test --without-runtime --intent ci-core
ost test
ost validate --strict-renderer-evidence
```

The targeted temporal/header-boundary checks passed 3/3. The runtime-free
suite passed 13/13, including the temporal, scene, boundary, install-tree and
benchmark contracts. The new temporal source compiled without warnings in
the core and runtime-free builds. Core `ost build` regenerated the renderer
evidence, retaining existing reference and wavefront transport checks.

The complete physical-GPU core suite passed 21/21, including reference and
wavefront transport, install-tree rendering, all three benchmark modes,
missing-driver handling, scene memory/acceleration and synchronization
validation. Strict renderer evidence validation passed with explained
Hydra SKIPs in this core-only build. Documentation links, anchors, indexes
and ownership checks passed; the documentation/release Python regressions
passed 27/27. No Hydra build or moving GPU temporal sequence was claimed.

After final formatting and the added orthographic assertion, the
`lotus-temporal-test` target was rebuilt in both configurations and the
temporal/header-boundary subset passed 3/3 in each. Build logs are
`build/temporal-core-build.log` and `build/temporal-runtime-free-build.log`.

## Scope disposition

This slice provides immutable frame metadata, previous rigid transforms,
CPU reprojection and shared history validity. It adds no GPU pass, radiance
history or render setting and changes no existing progressive accumulation
behavior. Full-frame targets are the only projection domain supported here.
Stable instancer IDs are not present in the scene representation, so
same-count reordering needs an explicit caller reset. Geometry deformation
is conservatively rejected.

GPU products and their timestamps, history fetch/rejection AOVs, temporal
radiance accumulation and moving Hydra validation remain in the
[execution plan](../roadmap/current.md#execution-order). No ReSTIR or denoiser
state was introduced.
