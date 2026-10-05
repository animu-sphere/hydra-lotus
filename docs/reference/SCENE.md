# Scene snapshots and the GPU scene

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
`std::overflow_error`. `DrawSummary` describes the
bootstrap raster pass: it selects one fixed triangle when any visible
triangle exists. It does not draw the stored scene geometry; the scene
reaches the GPU through the update plan below. `RenderScene` uses only its
camera matrix and traces the GPU scene instead of its bootstrap counts.

## Update plan

`include/lotus/extraction.hpp` owns the host-neutral update plan.
`SceneExtraction::Update` compares a snapshot's scene with the one it last
planned and returns a `SceneUpdate`:

- `geometry_releases`: geometry buffers the scene no longer references, in
  the previous scene's key order;
- `geometry_uploads`: geometry buffers that become resident, in key order;
- `instances`: when `instances_changed`, the complete replacement list,
  one `SceneInstance` (geometry and `world_from_object`) per visible mesh
  with triangles, in key order.

A geometry buffer is identified by its address. Every mesh with triangles
keeps its geometry resident, hidden or not, so visibility and transform
changes rewrite only the instances. Geometry without triangles is never
resident or instanced. The extraction holds a reference to every resident
buffer, so an address is not reused before its release.

An unchanged scene pointer, including a camera-only commit, returns an
empty plan without traversing the scene. Every plan must be applied in
order. `Reset` is for a lost GPU scene: the next plan uploads everything
again and releases nothing.

## GPU scene

The Vulkan backend owns the GPU scene inside `Lotus::OffscreenRenderer`
(`include/lotus/vulkan_backend.hpp`); its Vulkan objects stay in
`backend/vulkan/`. `UpdateScene` validates the whole plan, then releases,
creates and uploads, and returns after the uploads have completed. An
empty plan records no GPU work.

- Each resident geometry has one device-local buffer: the positions as
  tightly packed float triples, then, at an offset aligned to the device's
  storage-buffer offset alignment, the triangles as uint32 triples.
- Geometry occupies a slot in the GPU scene's geometry table. A released
  slot is reused lowest first, so slots are deterministic for a
  deterministic sequence of plans.
- One device-local instance buffer holds an 80-byte std430 record per
  instance: the column-major `world_from_object` and the geometry slot.
  It grows when needed and is rewritten only when the instances change.
- Uploads go through a persistent, grow-only host-visible staging buffer,
  in one submission per plan, followed by a barrier that makes them
  visible to later shader reads and transfers.

`GpuSceneStats` reports resident geometry, instances, bytes and lifetime
upload counts. `ReadBackScene` copies the device buffers back for
validation. A failed update or readback leaves the renderer failed, as a
failed frame does: create a renderer and reset the extraction.

The primary-ray pass traverses the acceleration structures built from these
buffers. It does not bind geometry or instance storage buffers yet:
barycentrics and hit distance come from the committed intersection.
Source-face indices stay on the CPU. Every geometry buffer, and every
BLAS below, is its own device allocation, so an update fails with an
explanation once a scene would exceed the device's allocation limit.

## Acceleration structures

When the device exposes `VK_KHR_acceleration_structure` with the
`accelerationStructure` and `bufferDeviceAddress` features, the renderer
enables them and the GPU scene keeps one BLAS per resident geometry and one
TLAS over the instances. Without them the buffers are still uploaded, no
acceleration structure exists, and `GpuSceneStats::acceleration_detail`
says why; `renderer.scene.acceleration` is then a SKIP.

- Geometry buffers also carry device addresses and acceleration-structure
  build-input usage. A BLAS is built from the geometry buffer when it is
  uploaded: one opaque triangle geometry, built for fast trace. It lives
  and dies with the buffer, so a point or topology edit builds a new BLAS,
  and hidden geometry keeps its BLAS. BLASes are neither refitted nor
  compacted.
- The TLAS has one instance per instance record, in the same order, so a
  ray query's instance index is the instance record's index. Each instance
  references its geometry's BLAS with the record's transform, mask `0xFF`
  and facing culling disabled; the custom index is unused.
- An instance rewrite in which every instance keeps its BLAS, which is a
  transform-only change, updates (refits) the TLAS in place. Any other
  rewrite, including one that changes the instance count, rebuilds it. The
  TLAS storage grows when a build needs more and is otherwise reused.
- The builds are recorded in the plan's one submission after its copies,
  BLASes first, using a grow-only scratch buffer, and `UpdateScene`
  returns after they complete.

`GpuSceneStats` counts the BLASes, the TLAS instances, their storage bytes,
and lifetime BLAS builds, TLAS builds and TLAS updates. `ReadBackScene`
also returns the TLAS build input decoded
(`GpuSceneContents::tlas_instances`), each BLAS reference resolved to its
geometry slot. Primary-ray correctness is also checked against independently
projected CPU triangles ([ray-query report](../reports/2026-10-05-primary-rays.md)).

## Primary rays

`CreateOffscreenRenderer` accepts optional `RayQueryShaders` with explicit
vertex and fragment SPIR-V paths. On devices with acceleration structures,
`VK_KHR_ray_query` and the `rayQuery` feature, it enables ray queries;
`RayQueryCapability` reports support and the reason when it is absent.
Supplying both shader paths creates one persistent primary-ray pipeline and
descriptor set alongside the bootstrap pipeline. Missing shader files on a
supported device fail creation; omitting the paths preserves bootstrap-only
callers and makes `RenderScene` fail with an explanation.

`RenderScene` unprojects Vulkan near/far clip coordinates at pixel centres,
using the inverse of `DrawSummary::world_to_clip` after clip-space conversion.
It supports perspective (including an infinite far plane) and orthographic
cameras, honours display/data windows, and traces opaque triangles without
facing culling. Rays begin at the near plane and stop at the far plane. A
singular or non-finite camera fails before recording GPU work.

The closest triangle outputs its barycentric weights as linear RGBA8 RGB,
alpha 1, and its projected Vulkan window depth as D32. Misses preserve the
clear or preceding attachment values. The pass applies the existing depth
test, clear flags, target reuse and resize rules. Its TLAS descriptor is
updated before each synchronous frame, so scene rebuilds cannot leave a stale
reference. Before any scene update, there is no TLAS and the pass only clears.

`GpuFrameEvidence::ray_query_used` identifies this path. Where the graphics
queue supports timestamps, a persistent query pool measures the primary-ray
render pass, including clears and excluding readback; the last frame's
`primary_ray_gpu_ms` and `primary_ray_timestamp_available` carry that result.
The headless report checks `renderer.ray_query.capability`, `.triangle` and
`.timestamp`; unavailable features give explained SKIPs. This is diagnostic
intersection output: BSDFs, lighting, multi-bounce transport and HDR
accumulation remain [roadmap work](../roadmap/current.md).

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

Each Hydra render pass plans and applies a scene update before its frame,
and a newly created renderer resets the extraction.
On ray-query devices its frame uses `RenderScene`, so point, topology,
transform and visibility edits affect the traced image. Other devices retain
the bootstrap path. The host evidence log identifies the choice as
`ray_query=1` or `0`.
`HdLotusRenderDelegate::GetGpuSceneStats` returns the GPU scene after the
latest pass.

The CPU scene and Hydra extraction tests, plus multi-triangle bootstrap AOV
regression coverage, are recorded in the
[mesh extraction report](../reports/2026-10-05-cpu-mesh-extraction.md); the
update plan and GPU scene tests in the
[GPU scene upload report](../reports/2026-10-05-gpu-scene-upload.md); the
acceleration-structure checks in the
[BLAS and TLAS report](../reports/2026-10-05-blas-tlas.md); primary-ray CPU
comparisons and Hydra silhouette checks in the
[ray-query report](../reports/2026-10-05-primary-rays.md).
