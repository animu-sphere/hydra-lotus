# Computed coarse smooth normals

Measured on 2026-10-07 in the working tree based on `4286a60`, on Windows
x86_64 with MSVC 14.51, OpenStrata 0.23.14, OpenUSD 26.08 lookdev and a
Release build. The existing GPU regression checks ran on an NVIDIA RTX
A5000, Vulkan API 1.4.329. All runs were local; hosted CI was not measured.

## Scope

The next slice of [Renderer Phase 1.5](../roadmap/current.md#renderer-phase-15--minimal-material-ir):
coarse smooth normals for Hydra meshes without usable authored normals.
The core already accepts per-corner shading normals and the GPU already
interpolates them, so computation stays in the Hydra adapter. No new target,
dependency, GPU layout or shader is needed.

## Implementation

`HdLotusMesh::ExtractGeometry` first expands authored normals as before.
When none can be used and the subdivision scheme is neither `none` nor
`bilinear`, it builds `Hd_VertexAdjacency` and calls
`Hd_SmoothNormals::ComputeSmoothNormals` from the existing `hd` dependency.
The result becomes the mesh's per-corner normal array.

The coarse computation is the one Storm uses: incident polygon-corner
edge cross products are summed and normalized at each point, before
triangulation. Left-handed orientation reverses them. Hole faces still
contribute to adjacency, matching Storm, although they produce no traced
triangles. The helper's result stops at the last topology index; unused
trailing points are padded with zero for the vertex-primvar count check.
Degenerate contributions give zero normals, which the existing shading
normal fallback handles. Non-finite positions are rejected before invoking
the helper, with the same whole-mesh rejection and recovery as before.

Existing point, topology and primvar dirty tracking triggers recomputation,
including changes to scheme, orientation, holes and authored-normal presence.
Authored normals take priority on every scheme. Computation does not refine
subdivision surfaces, evaluate limit normals or honor subdivision creases.

## Verification

`lotus-renderer-hydra-normals` now syncs sixteen meshes through UsdImaging.
Alongside all existing authored-normal interpolations and indexed primvars,
it exercises a non-planar quad adjoining a tilted triangle, `catmullClark`,
`loop`, `none`, `bilinear`, the default scheme, left-handed orientation,
holes, a degenerate triangle and an unused trailing point.

A separate oracle reads positions and polygons directly through UsdGeom,
accumulates edge cross products in double precision without Hd adjacency
or normal utilities, normalizes them and maps them to the output corners.
Each computed component must be finite and within 2e-6 of the oracle.
The non-planar quad distinguishes coarse polygon contributions from
averaging triangulated faces or unit face normals.

The same test checks point and topology edits, smooth-to-flat scheme changes,
adding authored normals over computed ones, removing them to restore
computation, authored normals on a flat scheme, orientation and hole edits,
rejection and recovery of non-finite points, and a clean sync that retains
the scene and revision. A point edit also preserves every unrelated mesh's
geometry. Mismatched authored normals fall back to computed normals.

Commands and results:

- `ost build --profile lookdev --intent hydra --jobs auto`: **passed**.
- `ost test --profile lookdev --intent hydra`: **23/23 passed**, including
  the expanded normals test, GPU checks and the usdview host test.
- `ost validate --profile lookdev --intent hydra --strict-renderer-evidence`:
  **passed**.

The headless evidence reports `renderer.scene.upload`,
`renderer.path.normals`, `renderer.path.reference` and
`renderer.validation.messages` as **pass**. The Cornell reference checks at
1, 16, 64, 256 and 1024 spp still pass; the reference sample sequence
reproduces the committed reference bit for bit. These are the existing
backend regressions; the new computation itself is checked at the CPU
scene boundary by the UsdImaging test.

Current behaviour is owned by the
[scene reference](../reference/SCENE.md#hydra-extraction); remaining material
work is in the [roadmap](../roadmap/current.md#renderer-phase-15--minimal-material-ir).
