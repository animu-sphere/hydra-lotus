# Hydra instancers

Measured on 2026-10-05 in the working tree based on `f3061a6`, on Windows 11
x86_64 with MSVC 14.51, OpenStrata 0.23.14 and Release builds. Vulkan headers
came from SDK 1.3.290 and `slangc` 2026.8 from SDK 1.4.350. The GPU was an
NVIDIA RTX A5000 (device API 1.4.329, loader API 1.4.321). Hydra used OpenUSD
26.08 lookdev and CPython 3.13. All runs were local; hosted CI was not
measured.

## Scope

This covers the instance data of
[Renderer Phase 1](../design/ROADMAP_POLICY.md#renderer-phase-1--reference-path-tracer):
Hydra instancers expand into placements of one resident geometry, with one
BLAS and one TLAS instance per placement
([scene reference](../reference/SCENE.md#geometry-and-placement)). Point
instancers, nested instancers and native instancing are covered, as Hydra
presents them through `HdInstancer`. Per-instance primvars are not read, and
render-pass selection by collection or render tag is not part of this step.

The checks are exact or tolerance comparisons on synthetic scenes; sample
count and RNG seed do not apply.

## Fixtures

**Core.** `lotus-renderer-scene-mesh` checks that an instancer prototype is
placed once per instancer transform, at the transform times the mesh's own
`world_from_object`, with a non-commuting rotation; that an empty list places
nothing; that a non-finite instancer transform is rejected; and that a
retained snapshot keeps its placements. `lotus-renderer-scene-update` checks
that the update plan lists one instance per placement, in key order and then
placement order, over one resident geometry, and that removing every
placement keeps the geometry resident.

**GPU scene.** `renderer.scene.upload` and `renderer.scene.acceleration`
extend their walk with four steps after the point edit. After each step they
compare the read-back instance records and TLAS build input with every
placement of the CPU scene:

| Step | Instances | BLASes | BLAS builds | TLAS builds | TLAS updates |
| --- | --- | --- | --- | --- | --- |
| instancer expansion: three placements | 3 | 2 | 3 | 4 | 1 |
| instancer transform edit | 3 | 2 | 3 | 4 | 2 |
| instancer without instances | 0 | 2 | 3 | 5 | 2 |
| ordinary placement | 1 | 2 | 3 | 6 | 2 |
| removal | 0 | 0 | 3 | 7 | 2 |

**Primary rays.** `renderer.ray_query.triangle` adds a step that places its
triangle three times: translated, rotated and scaled, and in front of
another placement. Its CPU oracle applies the instancer transforms in double
precision itself, without the core's composed matrices.

**Hydra.** The new `lotus-renderer-hydra-instancer` test composes a USD
stage in memory and syncs it through UsdImaging's scene indices and the
render index into the Lotus render delegate, without a GPU. The stage holds
a point instancer with two prototypes, translations, half-precision
orientations, scales and a prototype with its own transform; a point
instancer nested in another, the outer one with orientations and scales; and
two native instances of one prototype. UsdGeom computes the expected world
transforms independently: `UsdGeomPointInstancer::ComputeInstanceTransformsAtTime`
and `ComputeMaskAtTime` for the point instancers, and `UsdGeomXformCache`
for the native instances. Each mesh's placements must match them as sets,
within 10⁻⁵, after the first sync and after each edit: instance positions,
`invisibleIds`, prototype indices, the instancer's transform, a prototype's
transform, the nested instancer's positions, the outer instancer's count,
orientations and scales, a native instance's transform and a third native
instance. The first edit must keep every geometry buffer.

**usdview.** The smoke scene adds a point instancer that places a small
triangle three times, rotated by 0°, 90° and 180°. Its evidence log must show
two resident geometries and four GPU instances; with acceleration
structures, two BLASes and four TLAS instances, before and after the point
edit. On this machine the screenshots show the three rotated placements
beside the large triangle.

Four deliberate faults were each reverted before the runs below:

- composing an instancer transform before the mesh's own instead of after
  it failed `lotus-renderer-scene-mesh` ("instancer placements were not
  composed after the mesh transform") and `renderer.ray_query.triangle`
  ("instancer placements: barycentric/miss mismatch at 30,3");
- placing each mesh once in the update plan failed
  `lotus-renderer-scene-update`, `renderer.scene.upload` ("instancer
  expansion: device buffers differ from the scene") and
  `renderer.ray_query.triangle`;
- ignoring the `hydra:instanceRotations` primvar failed
  `lotus-renderer-hydra-instancer` ("first sync: placements differ from
  UsdGeom");
- composing a nested instancer's parent transform before its own failed
  the same test. With translation-only outer instances it first passed,
  because translations commute; the fixture's outer instancer gained
  orientations and scales so that it fails.

## Verification

Before the builds, objects with no recorded header dependencies were
deleted, the known Japanese-MSVC workaround
([roadmap](../reference/SUPPORTED_CONFIGURATIONS.md#build-and-tooling-limitations)): 6 in the Hydra
tree, and those of the viewport tree.

- `ost build --jobs auto`, `ost test`: **8/8 passed**. `ost validate`:
  **passed**, Hydra checks skipped by configuration.
- `ost build --without-runtime --intent ci-core`,
  `ost test --without-runtime --intent ci-core`: **8/8 passed**.
- `ost build --profile lookdev --intent hydra`,
  `ost test --profile lookdev --intent hydra`: **15/15 passed**, including
  the new instancer test and the usdview host test.
- `ost validate --profile lookdev --intent hydra --strict-renderer-evidence`:
  **passed**, with zero Vulkan validation messages.
- `ost renderer viewport -- --frames 8 --hidden`: presented 8 frames;
  `ost validate --intent renderer-viewport`: **passed**.

Large instancers were not measured: the CPU scene copies a mesh's instancer
transforms with its record, and each placement is its own 128-byte instance
record and TLAS instance, so memory and update cost grow with the placement
count.

The current API and its limits are in the
[scene reference](../reference/SCENE.md#hydra-extraction); remaining work is
in the
[roadmap](../roadmap/README.md#status-at-a-glance).
