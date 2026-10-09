# Material IR and constant UsdPreviewSurface translation

Measured on 2026-10-05 in the working tree based on `6acafee`, on Windows 11
x86_64 with MSVC 14.51, OpenStrata 0.23.14 and Release builds. The GPU was an
NVIDIA RTX A5000 (driver 597.16). Hydra used OpenUSD 26.08 lookdev and
CPython 3.13. All runs were local; hosted CI was not measured.

## Scope

The first slice of
[Renderer Phase 1.5](../roadmap/README.md#status-at-a-glance):
the minimal material IR and its path from Hydra to the GPU
([design policy §18](../design/DESIGN_POLICY.md#18-material-ir)),

```text
Hydra material → material translator → Lotus material IR → GPU material
```

for constant values only. The IR holds what the path tracer already
evaluated: base colour, roughness, metallic and emission. Textures, normals,
opacity and a dielectric specular layer are the phase's later items. The
shading is unchanged, so the committed reference images must be reproduced
exactly.

## Design

**The IR.** `Lotus::Material` (`include/lotus/material.hpp`) replaces the
per-mesh `SurfaceMaterial`, with the same fields, ranges and
`UsdPreviewSurface` defaults. Materials are scene objects:
`LotusScene::materials` keys them, `SetMaterial` and `RemoveMaterial` edit
them, and `BindMaterial` binds a mesh to a key. A binding to a key with no
material, or no binding, means the default, so a host can bind before the
material exists and a removal leaves the binding in place. The header is
part of `lotus-render-world`; `core/material/` waits until the IR needs
code.

**The plan.** `SceneExtraction` turns the scene's materials into a table:
the default at slot 0, then the materials in key order. A `SceneInstance`
carries its mesh's slot instead of a copy of the material. The table is
planned whole when it changes, like the instances, and a GPU scene starts
with the default alone, so a scene without materials still plans nothing.
The consequences:

| Edit | Instances | Material table | TLAS |
| --- | --- | --- | --- |
| a material's value | — | rewritten | — |
| a binding | rewritten | — | — |
| adding or removing a material | rewritten (later slots move) | rewritten | — |

Moving slots on insertion keeps the table deterministic and the plan
simple; stable slots with reuse, as the geometry table has, can come when a
scene's material count makes the instance rewrite matter.

**The GPU material.** The private `GpuScene` keeps a grow-only device
buffer of 32-byte `GpuMaterialRecord`s, written through the same staging
submission as the other uploads, and the instance record shrinks from 128
to 96 bytes, its material replaced by `material_slot`. The path tracer gets
the table's device address in `PathConstants` and reads
`materials[instance.material_slot]` at each hit. Because only instances
read the table, it reaches the device with the first instances. The update
validation rejects an instance slot outside the table and, found by a
fault injection below, a table that shrinks below the slots of instances
the plan leaves unchanged.

**The translator.** The Hydra adapter supports the `material` sprim.
`HdLotusMaterial` passes its `GetMaterialResource` network to
`HdLotusTranslateMaterial` (`adapters/hydra2/src/material_translator.*`) when
its parameters or resource are dirty and stores the result under its path;
a mesh reads `GetMaterialId` when its material id is dirty and binds to it.
The translator converts the network to `HdMaterialNetwork2`, follows the
surface terminal and, for a `UsdPreviewSurface`, reads the constant
`diffuseColor`, `roughness`, `metallic` and `emissiveColor`. Values are
clamped into the IR's ranges, a non-finite or mistyped value takes its
default, `useSpecularWorkflow` makes metallic 0, and an input a shader
graph drives takes its default too. Any other surface shader warns and
leaves the default material.

## Verification of the slice

**The reference, bit for bit.** Before any change,
`lotus-headless --write-reference` from the `6acafee` build wrote a mean and
variance identical to the committed `validation/reference/` files. After the
change, with the Cornell box sharing one material among its five white
surfaces and its emitter, metal and coloured walls as keyed materials, the
regenerated files are again byte-identical to the committed ones, and
`renderer.path.reference` passes. The material table changes where the
shader reads a material, not what it computes.

**The plan.** `lotus-renderer-scene-update` checks the table's order, that
a value edit rewrites only the table and a binding only the instances, that
a removal falls back to slot 0 and a returning material resumes its
binding, and that `Reset` replans the table. `lotus-renderer-scene-mesh`
checks the IR's validation, bindings before materials exist, bindings kept
across geometry replacement and removal, and retained snapshots.

**The GPU scene.** `renderer.scene.upload` binds a material, edits it and
removes it, comparing the read-back instances and table with the CPU scene
after each step: the edit wrote the table alone in one submission, and
neither the binding nor the edit touched the TLAS.
`renderer.scene.timestamp` now times a material binding and a material
edit on its instanced 131,072-triangle grid:

| Step | `upload_gpu_ms` | `blas_build_gpu_ms` | `tlas_build_gpu_ms` |
| --- | --- | --- | --- |
| material binding | 0.0044 | 0 | 0 |
| material edit | 0.0008 | 0 | 0 |

**Hydra.** `lotus-renderer-hydra-material` first translates hand-built
networks: no surface, a MaterialX surface (named in the warning), NaN,
infinite, double-precision and mistyped values, and a `UsdUVTexture`
connection beside an authored value. It then syncs a USD stage through
UsdImaging with five materials — authored values, out-of-range values, the
specular workflow with a texture-driven `diffuseColor`, and an unknown
shader id — and checks the IR and the bindings through a value edit (no
mesh changes), a new binding and a removed material. Its `-gpu` variant
renders an orthographic view of two coplanar quads under the adapter's
white environment: one unbound, one bound to a Lambert material with
emission. Every path scatters once and escapes, so each pixel is exactly
albedo + emission: 0.18 and (1.5, 2.25, 3.125) in the first frame, the
edited emission after the edit, and the second material on the unbound quad
after it is bound.

One finding: through UsdImaging on OpenUSD 26.08, a connected input's
network carries no value for the input at all, so the stage alone cannot
show whether the translator would wrongly read an authored value under a
connection. The hand-built network case covers it.

## Fault injections

Each was reverted before the runs below.

| Fault | Caught by |
| --- | --- |
| A material value edit does not mark the device table stale | `renderer.scene.upload` ("material binding: device buffers differ from the scene"), and `renderer.path.bsdf`, `.multibounce`, `.accumulation` and `.reference` (−56.4 standard errors at 1 spp), whose scenes reuse one renderer |
| The shader reads slot 0 for every hit | `renderer.path.bsdf`, `.multibounce`, `.accumulation` and `.reference` |
| A mesh bound to a removed material keeps a slot past the table | `lotus-renderer-scene-update` ("a material removal did not fall back to the default material") and `renderer.scene.upload` |
| The translator reads an authored value under a connection | `lotus-renderer-hydra-material` ("a connected input: got base (0.5, 0.5, 0.5) …") after the hand-built case was added; the stage case alone did not catch it (see the finding above) |

The third fault first showed `renderer.scene.upload` failing on a readback
mismatch rather than a rejected plan: the instances did not change, so the
GPU scene's validation, which checked only the plan's own instances, let a
shrunken table under them. The validation now tracks the highest slot its
instances use, and the same fault is rejected as "the scene update's
material table drops materials that the instances use".

## Verification

Before the builds, `ninja -t deps` listed objects without recorded header
dependencies, the known Japanese-MSVC issue
([roadmap](../reference/SUPPORTED_CONFIGURATIONS.md#build-and-tooling-limitations)): after the header
edits every object in the Hydra tree recorded none, and some in the core,
viewport and ci-core trees. They were deleted before each final build.

- `ost build --jobs auto`, `ost test`: **8/8 passed**;
  `ost validate --strict-renderer-evidence`: **passed**, with
  `renderer.scene.upload`, `.scene.timestamp`, `.path.reference` PASS and
  zero Vulkan validation messages.
- `ost build --without-runtime --intent ci-core --jobs auto`,
  `ost test --without-runtime --intent ci-core`: **8/8 passed**.
- `ost build --profile lookdev --intent hydra --jobs auto`,
  `ost test --profile lookdev --intent hydra`: **20/20 passed**, with
  `lotus-renderer-hydra-material` and `-material-gpu` new;
  `ost validate --profile lookdev --intent hydra --strict-renderer-evidence`:
  **passed**.
- `ost renderer viewport -- --frames 8 --hidden`: presented 8 frames;
  `ost validate --intent renderer-viewport`: **passed**.
- `lotus-headless --write-reference` after the final build: mean and
  variance byte-identical to `validation/reference/`.

The current behaviour is in the
[scene reference](../reference/SCENE.md#materials-and-environment) and its
[Hydra materials section](../reference/SCENE.md#materials); remaining work
is in the [roadmap](../roadmap/README.md#status-at-a-glance).
