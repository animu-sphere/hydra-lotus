# Scene snapshots and the GPU scene

`include/lotus/render_world.hpp` owns the host-neutral CPU scene interface.
`RenderWorld` remains the mutable entry point; `LotusScene` is its mesh and
lighting state, published through `FrameSnapshot::scene`. Only standard C++
types cross this boundary. Camera conventions are in the header and the
[camera evidence](../reports/2026-10-04-foundation-camera-boundary.md).

## Geometry and placement

Each entry in `LotusScene::meshes` has a stable, host-supplied string key and
a `SceneMesh` record. Keys iterate in deterministic order.

- `MeshGeometry` contains object-space float positions, uint32 triangle
  indices and one authored coarse-face index per triangle (`source_faces`).
- `MeshInstance` contains visibility and `world_from_object`, using the
  core's column-major matrix convention for column vectors.
- Without `MeshInstance::instancer_transforms` a mesh has one ordinary
  placement. With them it is an instancer prototype, placed once per entry,
  in order, at the entry times `world_from_object`; an empty list places it
  nowhere. `PlacementTransforms` returns each placement's composed
  transform, and `Multiply` is the composition.
- `SceneMesh::geometry` shares an immutable geometry buffer among all of a
  mesh's placements.

`SetMesh` validates and owns the geometry, inserting or replacing the keyed
record. Instancer transforms are copied with the record. `SetMeshInstance` updates an existing placement without copying its
geometry. `RemoveMesh` removes the keyed record. Invalid indices, mismatched
face mappings, non-finite positions or transforms (instancer transforms included), empty insertion keys and
instance updates without geometry throw `std::invalid_argument` without
changing the world.

`Commit` publishes the scene and its revision. A retained snapshot stays
unchanged across edits and removals: changed scene records are copied only
when shared with a snapshot, and their geometry buffers remain shared until
geometry changes. Identical input and removal of a missing key do not advance
the revision. Camera-only changes share the same scene. An unchanged commit
does not traverse or copy the meshes.

`FrameSnapshot::triangle_count` counts visible scene triangles, once per
placement, plus any
explicit bootstrap triangles. Counts exceeding uint32 throw
`std::overflow_error`. `DrawSummary` describes the
bootstrap raster pass: it selects one fixed triangle when any visible
triangle exists. It does not draw the stored scene geometry; the scene
reaches the GPU through the update plan below. `RenderScene` uses only its
camera matrix and traces the GPU scene instead of its bootstrap counts.

## Materials and environment

Until the material IR of Renderer Phase 1.5, a mesh's appearance is a
`SurfaceMaterial` on its `SceneMesh`, in linear RGB. Its defaults are
`UsdPreviewSurface`'s:

| Field | Default | Range | Meaning |
| --- | --- | --- | --- |
| `base_color` | `(0.18, 0.18, 0.18)` | each in [0, 1] | Lambert albedo, and the metal's reflectance at normal incidence |
| `roughness` | `0.5` | [0, 1] | GGX roughness; alpha is its square |
| `metallic` | `0` | [0, 1] | the GGX metal's share of the reflectance; the rest is Lambert |
| `emission` | `(0, 0, 0)` | finite, ≥ 0 | radiance emitted from both sides |

`SetMeshMaterial` changes an existing mesh's material; `SetMesh` keeps it
when it replaces the geometry or the instance. `LotusScene::environment` is
a constant environment radiance, finite and non-negative, black by default,
changed with `SetEnvironment`. Out-of-range values and a material for a
missing mesh throw `std::invalid_argument` without changing the world.

## Update plan

`include/lotus/extraction.hpp` owns the host-neutral update plan.
`SceneExtraction::Update` compares a snapshot's scene with the one it last
planned and returns a `SceneUpdate`:

- `geometry_releases`: geometry buffers the scene no longer references, in
  the previous scene's key order;
- `geometry_uploads`: geometry buffers that become resident, in key order;
- `instances`: when `instances_changed`, the complete replacement list,
  one `SceneInstance` (geometry, composed `world_from_object` and material)
  per placement of each visible mesh with triangles, in key order and then
  placement order;
- `environment`: when `environment_changed`, the scene's environment
  radiance. A GPU scene, and the extraction after `Reset`, start from
  black, so only a different environment is planned.

A geometry buffer is identified by its address. Every mesh with triangles
keeps its geometry resident, even hidden or placed nowhere, so visibility,
transform, placement and material changes rewrite only the instances. Geometry without triangles is never
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
- One device-local instance buffer holds a 128-byte record per instance:
  the column-major `world_from_object`, the device addresses of its
  geometry's positions and triangles (zero without acceleration
  structures), the geometry slot, and the material as base colour and
  roughness, then emission and metallic. It grows when needed and is
  rewritten only when the instances change.
- The environment is host state: a plan that changes only the environment
  records no GPU work, and the scene pass reads it with each frame.
- Uploads go through a persistent, grow-only host-visible staging buffer,
  in one submission per plan, followed by a barrier that makes them
  visible to later shader reads and transfers.

`GpuSceneStats` reports resident geometry, instances, bytes and lifetime
upload counts. `ReadBackScene` copies the device buffers back for
validation. A failed update or readback leaves the renderer failed, as a
failed frame does: create a renderer and reset the extraction.

The scene pass traverses the acceleration structures built from these
buffers and reads a hit's instance record and, through its addresses, the
hit triangle. Source-face indices stay on the CPU. Every geometry buffer, and every
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
- An instance rewrite in which every instance keeps its BLAS and its
  transform, such as a material edit, leaves the TLAS as it is. One in
  which every instance keeps its BLAS but a transform changes updates
  (refits) the TLAS in place. Any other rewrite, including one that
  changes the instance count, rebuilds it. The TLAS storage grows when a
  build needs more and is otherwise reused.
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
vertex and fragment SPIR-V paths (`path_trace.vert.spv`,
`path_trace.frag.spv`). On devices with acceleration structures,
`VK_KHR_ray_query` and the `rayQuery` feature, it enables ray queries;
The scene passes also need `fragmentStoresAndAtomics` and RGBA32F colour
attachments with storage and readback; `RayQueryCapability` reports support
and the reason when any of these is absent. Supplying both shader paths
creates the scene render pass, three persistent pipelines specialized from
the one fragment module (below), a descriptor set and a uniform block
alongside the bootstrap pipeline. Missing
shader files on a supported device fail creation; omitting the paths
preserves bootstrap-only callers and makes `RenderScene` fail with an
explanation.

`RenderScene` unprojects Vulkan near/far clip coordinates at pixel centres,
using the inverse of `DrawSummary::world_to_clip` after clip-space conversion.
It supports perspective (including an infinite far plane) and orthographic
cameras, honours display/data windows, and traces opaque triangles without
facing culling. Rays begin at the near plane and stop at the far plane. A
singular or non-finite camera fails before recording GPU work.

The scene passes render into their own RGBA32F colour and D32 depth
targets, separate from the bootstrap's RGBA8 ones; each set follows the
clear flags, target reuse and resize rules. The *camera pass* traces each
pixel centre: its closest hit writes its projected Vulkan window depth
through the existing depth test, and misses preserve the clear or
preceding attachment values. What the colour holds is
`PathTracingSettings::output`: `Barycentrics` is the camera pass's closest
triangle's barycentric weights with alpha 1, the intersection diagnostic;
`Radiance`, the default, is the accumulated path-traced radiance of the
*radiance pass* (below), drawn after the camera pass has written depth.
The TLAS and accumulation-image descriptors and the uniform block (the
environment, the background, the display-window mapping, the instance
records' address and the settings) are updated before each synchronous
submission, so scene rebuilds cannot leave a stale reference. Before any
scene update, there is no TLAS and the passes only clear.

`GpuFrameEvidence::ray_query_used` identifies this path. Where the graphics
queue supports timestamps, a persistent query pool measures the scene
render pass, including clears and excluding readback; the last submission's
`primary_ray_gpu_ms` and `primary_ray_timestamp_available` carry that result.
The headless report checks `renderer.ray_query.capability`, `.triangle` and
`.timestamp` with the barycentric output; unavailable features give
explained SKIPs.

## Path tracing

The `Radiance` output is the reference path tracer of
[design policy section 9](../design/DESIGN_POLICY.md#9-baseline-path-tracer)
in a fullscreen fragment pass (`backend/vulkan/shaders/path_trace.slang`):
one brute-force camera path per pixel per sample, with no light sampling.

- **Hit reconstruction.** The instance record's transform places the hit
  triangle's corners in world space; the position is interpolated from the
  barycentrics, and the geometric normal is turned to face the incoming
  ray. Surfaces are two-sided and have no shading normals, since normals
  are not extracted yet.
- **Emission** is added at every hit, the camera's included.
- **BSDF.** A Lambert lobe with albedo `base_color` and a GGX metal lobe,
  mixed by `metallic`. The metal has Schlick's Fresnel from `base_color`,
  the height-correlated Smith masking-shadowing term and alpha
  `max(roughness², 0.001)`. A direction is sampled by choosing the metal
  with probability `metallic`, then from the cosine-weighted hemisphere or
  from GGX visible normals by spherical caps; its weight is the mixture's
  f·cos divided by the mixture's density. A surface with `metallic` 0 is
  Lambert only; the dielectric specular layer arrives with the material IR.
- **Environment.** A scattered ray that escapes adds the throughput times
  the environment radiance. Camera rays that miss do not see it.
- **Termination.** A path ends after `PathTracingSettings::max_bounces`
  scattering events (64 by default; 0 keeps only the emission the camera
  sees), when a sampled direction is not above the surface, or by Russian
  roulette: before the fourth and every later scattering event, it
  continues with probability `min(max throughput component, 0.95)` and its
  throughput is divided by that probability.
- **Ray origins** are offset along the normal (Wächter and Binder, *Ray
  Tracing Gems* chapter 6), and secondary rays are unbounded.
- **Randomness.** A sample's PCG sequence is seeded from the pixel's index
  in the target and its sample index: an accumulation's k-th sample uses
  `PathTracingSettings::sample_index + k`. The same scene, camera, target
  and settings render the same bytes.

Light-carrying values in the shaders are `Spectrum`
(`shaders/common/spectrum.slang`), which holds RGB today
([DES-Q3](../design/DESIGN_POLICY.md#53-open-questions)).

### Accumulation and the pixel filter

Each submission of a `Radiance` frame adds one sample per pixel to an
RGBA32F accumulation image: the sum of the radiance of the samples that hit
the scene, and their count.

- **Pixel filter.** A 1-pixel box: a sample's camera ray goes through a
  uniformly distributed point of the pixel's square, through the display
  window's mapping, and every sample has weight 1. Pixels are estimated
  independently.
- **Output.** The colour is the mean of the pixel's `n` samples, a missed
  sample counting as the target's clear colour: hit radiance with alpha 1,
  so with a transparent clear, alpha is the pixel's coverage. It is not
  clamped or tone mapped. A pixel none of whose samples hit keeps the colour
  attachment, cleared or preserved.
- **Continuing and restarting.** A frame continues the accumulation when its
  camera, target size, display and data windows, `sample_index` and
  `max_bounces` match the previous `Radiance` frame's and no nonempty
  `UpdateScene` came between; otherwise it restarts it. Recreating the
  targets restarts it too. The clear colour and `max_samples` do not, and a
  `Barycentrics` frame leaves it as it is. `frame_count` samples split over
  several calls give the same bytes as one call.
- **Limit.** `PathTracingSettings::max_samples`, when nonzero, caps the
  samples per pixel; a frame with nothing left to add makes one submission
  that writes the same mean again.
  `GpuFrameEvidence::samples_per_pixel` reports the count, and
  `frames_rendered` the submissions.

`renderer.path.bsdf` and `renderer.path.multibounce` check the transport
against radiance known in closed form or by independent quadrature
([report](../reports/2026-10-05-bsdf-multibounce.md));
`renderer.path.accumulation` checks the box filter's coverage against the
projected triangle's area in each pixel, unclamped HDR output, split frames
and the restart rules
([report](../reports/2026-10-05-hdr-accumulation.md)).

### Reference images

The reference path tracer's images are of one fixed scene, rendered by
`lotus-headless`
([design policy §26](../design/DESIGN_POLICY.md#26-reference--deterministic-mode)).

- **Scene.** A Cornell box, 128×128 pixels, defined in
  `adapters/headless/reference.cpp`. The box spans [-1, 1]³ and is open
  towards the camera. Its walls are Lambert: white floor, ceiling and back,
  a red left wall and a green right wall. Under the ceiling hangs a black
  emitter with radiance (15, 13, 10). Inside stand a GGX metal block
  (base (0.95, 0.85, 0.6), roughness 0.35) and a white Lambert block. The
  environment is black, the bounce limit is the default 64, and the camera
  is 3 units in front of the box with a 45° vertical field of view. Every
  camera ray enters the box, so every pixel's alpha is 1.
- **Deterministic mode.** An image is a function of its first sample index,
  its sample count, the scene, the camera and the target
  ([accumulation](#accumulation-and-the-pixel-filter)). `lotus-headless
  --write-reference <directory>` renders the reference from sample index 0:
  - `cornell-box-mean.pfm`: 1024 samples per pixel in one accumulation.
  - `cornell-box-variance.pfm`: each pixel's per-sample variance,
    estimated from 64 accumulations of 16 samples. These are the same
    1024 samples.

  Both files are committed in `validation/reference/`. Each is a
  little-endian RGB portable float map, with rows from the bottom.
- **The compared images.** `renderer.path.reference` renders one
  accumulation from sample index 2²⁰, independent of the reference's
  samples. It stops at 1, 16, 64, 256 and 1024 spp and compares each image
  with the reference. `--images <directory>` writes the images as
  `cornell-box-NNNNspp.pfm`. `ost build` and the evidence CTest write them
  to `build/<target>/reference-images/`.
- **Statistical match (DES-Q5).** For `N` samples against the reference's
  `M`, a pixel's difference has variance `s²(1/N + 1/M)`, where `s²` is the
  reference's per-sample variance. An image matches the reference when, in
  every channel, two mean differences are within 5 standard errors:
  - the mean difference over the whole image;
  - the mean difference over every tile. A tile is 8×8 pixels, doubled
    until it holds at least 1024 samples, so 32×32 at 1 spp.

  A float-rounding floor of 10⁻⁶ + 10⁻⁵ of the reference's magnitude is
  added to each standard error. For an estimator with less variance than
  the reference's, such as NEE / MIS, `s²` overstates the variance, so the
  test is conservative.
- **Checks on the metric.** The same check renders the scene with the red
  wall reflecting 10% more, at 1024 spp, and the comparison must reject
  it. It also re-renders the reference's own samples and reports whether
  they reproduce the committed mean bit for bit. That is expected only on
  the device and build that wrote the reference, so it does not decide the
  check.

## Hydra extraction

The adapter keys each mesh by its `SdfPath` string and reads points,
topology, transform, visibility and instancer placements only when their
dirty bits require it. Points and topology are cached so a transform,
visibility or instancer update does not fetch or re-triangulate geometry.

Coarse polygons use OpenUSD's
[`HdMeshUtil` triangulation](https://openusd.org/dev/api/class_hd_mesh_util.html):
fan triangles, winding normalization, hole-face exclusion and coarse-face
mapping. Subdivision refinement, general concave-polygon tessellation,
normals, face-varying primvars and materials are not implemented.

Malformed mesh input is warned about and removes any earlier geometry for
that mesh. A later valid sync recovers it. Mesh destruction removes its scene
record. `HdLotusRenderDelegate::GetFrameSnapshot` exposes the same CPU snapshot
for inspection without creating a GPU renderer.

Each Hydra render pass plans and applies a scene update before its frame,
and a newly created renderer resets the extraction.
On ray-query devices each pass adds one sample with `RenderScene`, with
`max_samples` set to the `convergedSamplesPerPixel` render setting (64 by
default); the render pass and the colour buffer report convergence when the
accumulation reaches it. Point, topology, transform, visibility and instancer edits,
like camera and framing changes, restart the accumulation. Other devices
retain the bootstrap path and converge after one pass. Materials and
lights are not read yet: every mesh has the default `SurfaceMaterial`, and
the adapter sets a constant white environment, so a surface no other
surface occludes shows its 0.18 albedo. The host evidence log identifies the choice as
`ray_query=1` or `0`, with each pass's `samples` and `converged`.
`HdLotusRenderDelegate::GetGpuSceneStats` returns the GPU scene after the
latest pass.

### Render-pass selection

A render pass traces the meshes of its collection whose render tag it was
given. A render-index rprim under none of the collection's root paths, under
one of its exclude paths, or with a render tag outside the pass's render
tags is hidden for that pass; with no render tags every tag is traced, as
`HdRenderPass` specifies. The collection's material tag is ignored, as a
path tracer traces every material in one pass. Hydra syncs only rprims
whose tag some task requests, so an rprim it never synced is absent, while
one that was synced and then deselected stays in the CPU scene with its
geometry.

Selection hides rather than removes: the pass's snapshot is the CPU
snapshot with the excluded meshes' `visible` cleared, so their geometry and
BLASes stay resident and a selection change, such as usdview's proxy or
guide purpose toggles, rewrites only the instances and restarts the
accumulation. The selection is computed only when the collection, the render
tags, the render index's rprims or an rprim's render tag changes, and the
hidden copy of the scene only when that selection or the CPU scene changes
and hides a visible mesh; an unchanged frame neither traverses the scene nor
plans GPU work. `HdLotusRenderDelegate::GetSelectedSnapshot` returns the
latest pass's snapshot, recorded before the pass checks its AOVs, while
`GetFrameSnapshot` remains the whole CPU scene.

Every render pass of a delegate shares one renderer and GPU scene, so two
passes with different selections, like two with different cameras, restart
each other's accumulation. A mesh synced directly by a test, outside any
render index, is traced by every pass.

The CPU scene and Hydra extraction tests, plus multi-triangle bootstrap AOV
regression coverage, are recorded in the
[mesh extraction report](../reports/2026-10-05-cpu-mesh-extraction.md); the
update plan and GPU scene tests in the
[GPU scene upload report](../reports/2026-10-05-gpu-scene-upload.md); the
acceleration-structure checks in the
[BLAS and TLAS report](../reports/2026-10-05-blas-tlas.md); primary-ray CPU
comparisons and Hydra silhouette checks in the
[ray-query report](../reports/2026-10-05-primary-rays.md); materials, the
environment and the path tracer in the
[BSDF and multi-bounce report](../reports/2026-10-05-bsdf-multibounce.md); accumulation and
progressive Hydra convergence in the
[HDR accumulation report](../reports/2026-10-05-hdr-accumulation.md); the
reference images and the statistical match in the
[reference images report](../reports/2026-10-05-reference-images.md);
instancer placements against UsdGeom, and their GPU instances, in the
[Hydra instancers report](../reports/2026-10-05-hydra-instancers.md);
collection and render-tag selection in the
[render-pass selection report](../reports/2026-10-05-render-pass-selection.md).

### Instancers

`HdLotusInstancer` places an instancer's prototypes, with OpenUSD's
hdEmbree instancer as the model. A prototype mesh syncs its instancer and
the instancer's parents, then sets `instancer_transforms` to one transform
per instance index the instancer gives it. For column vectors, an
instance's transform is the instancer transform × translation × rotation ×
scale × instance transform, each factor from its
`hydra:instanceTranslations`, `hydra:instanceRotations`,
`hydra:instanceScales` or `hydra:instanceTransforms` primvar when present,
in half, single or double precision (matrices single or double); an index
beyond a primvar's array leaves that factor out. A nested instancer's instances repeat for each
instance of its parent, the parent's transform applied last. Point
instancers, nested point instancers and native instancing reach the adapter
this way through UsdImaging. Per-instance primvars other than these, such
as a per-instance colour, are not read. A mesh without an instancer keeps
its one ordinary placement.
