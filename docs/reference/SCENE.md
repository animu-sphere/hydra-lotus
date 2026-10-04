# CPU scene snapshots

`include/lotus/render_world.hpp` owns the host-neutral CPU scene interface.
`RenderWorld` remains the mutable entry point; `LotusScene` is its mesh state,
published through `FrameSnapshot::scene`. Only standard C++ types cross this
boundary. Camera conventions are in the header and the
[camera evidence](../reports/2026-10-04-foundation-camera-boundary.md).

## Geometry and placement

Each entry in `LotusScene::meshes` has a stable, host-supplied string key and
a `SceneMesh` record. Keys iterate in deterministic order.

- `MeshGeometry` contains object-space float positions, uint32 triangle
  indices and one authored coarse-face index per triangle (`source_faces`).
- `MeshInstance` contains visibility and `world_from_object`, using the
  core's column-major matrix convention for column vectors.
- `SceneMesh::geometry` shares an immutable geometry buffer. Each mesh has
  one ordinary placement; Hydra instancers are not expanded yet.

`SetMesh` validates and owns the geometry, inserting or replacing the keyed
record. `SetMeshInstance` updates an existing placement without copying its
geometry. `RemoveMesh` removes the keyed record. Invalid indices, mismatched
face mappings, non-finite positions/transforms, empty insertion keys and
instance updates without geometry throw `std::invalid_argument` without
changing the world.

`Commit` publishes the scene and its revision. A retained snapshot stays
unchanged across edits and removals: changed scene records are copied only
when shared with a snapshot, and their geometry buffers remain shared until
geometry changes. Identical input and removal of a missing key do not advance
the revision. Camera-only changes share the same scene. An unchanged commit
does not traverse or copy the meshes.

`FrameSnapshot::triangle_count` counts visible scene triangles plus any
explicit bootstrap triangles. Counts exceeding uint32 throw
`std::overflow_error`. The current `DrawSummary` still describes the
bootstrap raster pass: it selects one fixed triangle when any visible
triangle exists. It does not upload or draw the stored scene geometry.

## Hydra extraction

The adapter keys each mesh by its `SdfPath` string and reads points,
topology, transform and visibility only when their dirty bits require it.
Points and topology are cached so a transform or visibility update does not
fetch or re-triangulate geometry.

Coarse polygons use OpenUSD's
[`HdMeshUtil` triangulation](https://openusd.org/dev/api/class_hd_mesh_util.html):
fan triangles, winding normalization, hole-face exclusion and coarse-face
mapping. Subdivision refinement, general concave-polygon tessellation,
normals, face-varying primvars, materials and instancer expansion are not
implemented. The existing render pass also does not filter geometry by
collection or render tag.

Malformed mesh input is warned about and removes any earlier geometry for
that mesh. A later valid sync recovers it. Mesh destruction removes its scene
record. `HdLotusRenderDelegate::GetFrameSnapshot` exposes the same CPU snapshot
for inspection without creating a GPU renderer.

The CPU scene and Hydra extraction tests, plus multi-triangle bootstrap AOV
regression coverage, are recorded in the
[mesh extraction report](../reports/2026-10-05-cpu-mesh-extraction.md).
