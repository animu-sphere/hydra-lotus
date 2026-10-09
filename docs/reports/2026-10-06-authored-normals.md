# Authored normals as shading normals

Measured on 2026-10-06 in the working tree based on `11db31d`, on Windows 11
x86_64 with MSVC 14.51, OpenStrata 0.23.14 and Release builds. The GPU was an
NVIDIA RTX A5000 (driver 597.16). Hydra used OpenUSD 26.08 lookdev and
CPython 3.13. All runs were local; hosted CI was not measured.

## Scope

The second slice of
[Renderer Phase 1.5](../roadmap/README.md#status-at-a-glance):
the *normal* of the
[initial material model](../design/ROADMAP_POLICY.md#renderer-phase-15--minimal-material-ir),
as authored mesh normals that the path tracer shades with. Normal maps
need textures and come after them; meshes without authored normals are not
given computed ones yet. The Cornell box has no normals, so its committed
reference images must be reproduced exactly.

## Design

**The IR.** `MeshGeometry::normals` holds one object-space normal per
triangle corner. Per-corner storage is the one layout every Hydra
interpolation reduces to without an index buffer per primvar, and texture
coordinates can follow it. Normals need not be unit length; the core
rejects a count other than three per triangle and non-finite values. They
are part of the geometry, so a normal edit replaces the geometry buffer,
as a point edit does: no new plan field was needed.

**The GPU scene.** The geometry buffer gains a third aligned section after
the triangles, and the instance record's reserved eight bytes become the
normals' address (zero without normals), so the record stays 96 bytes.

**The shading normal** (`backend/vulkan/shaders/path_trace.slang`). At a
hit, the corner normals are interpolated with the barycentrics, transformed
by the cofactor matrix of the transform's linear part, which is the
inverse transpose times the determinant and needs no inverse, normalized,
and turned to the geometric normal's side. Two rules keep it physical:

- Where the shading normal faces away from the incoming ray, the BSDF
  could not reflect at all, so the geometric normal stands in. The same
  happens where the interpolated normal vanishes.
- A direction sampled above the shading normal but below the geometric
  surface would leave through a surface that only reflects, so the path
  ends there.

Ray origins stay offset along the geometric normal.

**The diagnostic.** `SceneOutput::ShadingNormal` writes the camera pass's
world-space shading normal, the first debug AOV of
[design policy §25](../design/DESIGN_POLICY.md#25-debug-and-validation).
It is a fourth pipeline specialized from the same fragment module.

**The adapter.** `HdLotusMesh` reads the `normals` primvar when the
normals or the primvars are dirty and expands it to corners. Constant
gives one value. Uniform is indexed by each triangle's coarse face. Vertex
and varying are indexed by the corner's point. Face-varying is triangulated
by `HdMeshUtil::ComputeTriangulatedFaceVaryingPrimvar`, the same
triangulation as the indices, holes and orientation included. Unusable
normals warn and leave the mesh with its geometric normals instead of
rejecting it.

## Findings

- **slangc pointers.** Comparing a buffer pointer with `nullptr`
  makes slangc emit a function-local variable of pointer type. The
  validation layer of Vulkan SDK 1.3.290, which the runs use, rejects it
  ("expected AliasedPointer or RestrictPointer"), although the 1.4.350
  `spirv-val` accepts it. The record therefore holds the normals as a
  `uint64_t` address, cast to a pointer where it is read. That needs the
  `shaderInt64` feature, which the scene passes now require and enable
  beside `fragmentStoresAndAtomics`.
- **Indexed primvars.** Through UsdImaging on OpenUSD 26.08, `GetPrimvar`
  returns an indexed primvar's values unflattened: three values for seven
  face vertices. The adapter reads indexed normals with `GetIndexedPrimvar`
  and flattens them itself. The test's indexed mesh found this.
- **Unchanged triangulation.** OpenUSD 26.08's
  `ComputeTriangulatedFaceVaryingPrimvar` returns `HdMeshComputationResult`.
  `Unchanged`, returned for triangles without holes, writes no output, and
  the input is used as it is.

## Verification of the slice

**The shading-normal diagnostic** (`renderer.path.normals`, first part).
An orthographic view holds three meshes:

- a square tilted 40° about X under a non-uniform scale (0.5, 0.8, 3),
  whose vertex normals point in very different directions;
- a triangle without normals;
- a square turned 180° about Y, so that its back is seen.

An oracle in the headless runner intersects each pixel centre's ray with
the world-space triangles in double precision and applies the rules above.
It shares no code with the shader. Each pixel's normal must match it
within 2·10⁻³ per component. Pixels whose outcome rounding could change are
skipped: within 0.02 of a triangle edge, or where the normal is within
0.02 of tangent to the surface or the ray. The run compared

| Rule | Pixels |
| --- | --- |
| interpolated | 775 |
| turned to the geometric side | 483 |
| facing away, geometric normal used | 218 |
| no normals, geometric | 248 |

These are exactly the counts a separate Python model of the same scene
predicted while the scene was being designed.

**Radiance the shading normal decides** (`renderer.path.normals`, second
part). The camera looks straight down on a Lambert plane under a white
environment:

- With authored normals along the geometric normal, at length 3, every
  sample reflects albedo exactly.
- With normals tilted 60°, only the cosine-weighted directions around the
  shading normal that stay above the plane escape. Their share is the
  tilted plane's sky view factor, (1 + cos 60°)/2 = 0.75. The mean was
  0.3756 / 0.1878 / 0.5634 against 0.3750 / 0.1875 / 0.5625, within five
  standard errors, over 262,144 samples.

A white near-mirror under a black environment, beside an emissive wall
outside the view, gives 0 exactly with its geometric normal. With normals
tilted 30° towards the wall, it gives the wall's radiance (2, 1, 0.5).

**The GPU scene.** `renderer.scene.upload`'s point edit now also carries
normals; their readback matches the CPU scene, in the same slot, with one
BLAS build.

**The reference, bit for bit.** `renderer.path.reference` passes and
reports that the reference's samples reproduce the committed mean bit for
bit. Neither the new branch nor the termination test changed a value in a
scene without normals.

**Hydra.** `lotus-renderer-hydra-normals` syncs a stage through
UsdImaging and compares every corner's normal with one read independently
through UsdGeom: `primvars:normals` over `normals`, flattened, and looked
up by point, face or face vertex. The stage has

- vertex, face-varying, indexed face-varying (beside a `normals` attribute
  it must override), uniform and constant normals;
- a left-handed mesh with a hole;
- an all-triangle face-varying mesh;
- normals of the wrong count (ignored, the mesh kept);
- a mesh without normals.

It then edits one mesh's normals, which replaces that mesh's geometry
alone. Finally it adds a constant primvar, changes an interpolation from
uniform to vertex and removes an attribute.

## Fault injections

Each was reverted before the runs below.

| Fault | Caught by |
| --- | --- |
| The shader ignores authored normals | `renderer.path.normals` (shading normal (0, 0, 1) instead of (0.078, 0.740, 0.668)) |
| Normals transform by the linear part instead of its inverse transpose | `renderer.path.normals` ((0.014, 0.174, 0.985) instead of (0.078, 0.740, 0.668)) |
| Directions below the geometric surface continue | `renderer.path.normals` (tilted Lambert mean 0.4378 instead of 0.3750 ± 0.0011) |
| A shading normal facing away from the ray is kept | `renderer.path.normals` (a fallback pixel) |
| The instance record carries no normal address | `renderer.path.normals` |
| Uniform normals take the first face's value | `lotus-renderer-hydra-normals` ("the corner normals of /Uniform differ") |

## Verification

Before the builds, `ninja -t deps` listed objects without recorded header
dependencies, the known Japanese-MSVC issue
([roadmap](../reference/SUPPORTED_CONFIGURATIONS.md#build-and-tooling-limitations)), in every tree;
they were deleted before the final builds. Restoring the fault-injected
sources with their earlier modification times first left a faulty object
in place; the sources were touched and everything rebuilt.

- `ost build --jobs auto`, `ost test`: **8/8 passed**;
  `ost validate --strict-renderer-evidence`: **passed**, with
  `renderer.path.normals`, `.path.reference`, `.scene.upload` PASS and zero
  Vulkan validation messages.
- `ost build --without-runtime --intent ci-core --jobs auto`,
  `ost test --without-runtime --intent ci-core`: **8/8 passed**.
- `ost build --profile lookdev --intent hydra --jobs auto`,
  `ost test --profile lookdev --intent hydra`: **21/21 passed**, with
  `lotus-renderer-hydra-normals` new;
  `ost validate --profile lookdev --intent hydra --strict-renderer-evidence`:
  **passed**.
- `ost renderer viewport -- --frames 8 --hidden`: presented 8 frames;
  `ost validate --intent renderer-viewport`: **passed**.
- `lotus-headless --write-reference` after the final build: mean and
  variance byte-identical to `validation/reference/`.

The current behaviour is in the
[scene reference](../reference/SCENE.md#path-tracing) and its
[Hydra extraction section](../reference/SCENE.md#hydra-extraction); remaining
work is in the
[roadmap](../roadmap/README.md#status-at-a-glance).
