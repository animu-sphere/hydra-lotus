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
  Its optional `normals` are object-space shading normals, one
  per triangle corner (corner `k` of triangle `t` at `3t + k`), so every
  interpolation a host has reaches the core the same way. They need not be
  unit length; empty means the surface shades with its geometric normal.
  Its `texcoords` are named texture-coordinate sets, each one (s, t) per
  triangle corner in the same layout; a material's texture inputs read the
  set it names.
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
face mappings, normals or texture-coordinate sets that are not one per
triangle corner or not finite, an unnamed texture-coordinate set,
non-finite positions or transforms (instancer transforms included), empty insertion keys and
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

`include/lotus/material.hpp` owns the Lotus material IR of Renderer Phase
1.5 ([design policy §18](../design/DESIGN_POLICY.md#18-material-ir)): a
`Material` that host translators produce and the GPU material table
evaluates, independent of any authoring schema. It holds constant values in
linear RGB, and its defaults are `UsdPreviewSurface`'s:

| Field | Default | Range | Meaning |
| --- | --- | --- | --- |
| `base_color` | `(0.18, 0.18, 0.18)` | each in [0, 1] | Lambert albedo, and the metal's reflectance at normal incidence |
| `roughness` | `0.5` | [0, 1] | GGX roughness; alpha is its square |
| `metallic` | `0` | [0, 1] | metal fraction; the remainder is a dielectric coat over Lambert |
| `ior` | `1.5` | finite, ≥ 1 | constant dielectric index relative to air; 1 removes the interface |
| `use_specular_workflow` | `false` | boolean | use explicit specular F0, ignoring metallic and ior |
| `specular_color` | `(0, 0, 0)` | each in [0, 1] | specular-workflow F0, with a white grazing limit |
| `emission` | `(0, 0, 0)` | finite, ≥ 0 | radiance emitted from both sides |
| `normal` | `(0, 0, 1)` | each in [-1, 1] | tangent-space shading normal, normalized after evaluation |
| `opacity` | `1` | [0, 1] | surface coverage, optionally replaced by `opacity_texture` |
| `opacity_threshold` | `0` | [0, 1] | positive values select binary masking; 0 selects stochastic coverage |

Each input is a constant or, when its `TextureInput` is set
(`base_color_texture`, `roughness_texture`, `metallic_texture`,
`emission_texture`, `normal_texture`, `opacity_texture`, `specular_color_texture`), a texture lookup that replaces the constant, as
`UsdUVTexture` does:

- The lookup names a texture by its key in `LotusScene::textures`. A key
  with no texture makes it return its `fallback` (default (0, 0, 0, 1)),
  unscaled.
- Otherwise the texture is filtered bilinearly, at level 0 and without
  mipmaps, at the material's texture coordinates; outside [0, 1] each
  coordinate follows its `wrap_s` or `wrap_t`: `Black` (transparent black,
  the default), `Clamp`, `Repeat` or `Mirror`. The result is
  `texel * scale + bias` (defaults 1 and 0).
- A colour input takes the result's red, green and blue (`channel` 0); a
  scalar input takes its `channel`, 0 to 3. The value is clamped into the
  input's range: [0, 1] for base colour, specular colour, roughness, metallic and opacity, and
  non-negative for emission and [-1, 1] for normal RGB.
- The texture coordinates are the mesh's set named `Material::texcoords`,
  interpolated at the hit. A mesh without that set, or an empty name, uses
  `texcoord_fallback` (default (0, 0)). A material has one
  texture-coordinate source for all its lookups.

The normal input is already signed tangent-space data. Lotus applies only
the lookup's authored scale and bias, with no implicit decoding to [-1, 1].
For the usual unsigned 8-bit encoding, author RGB scale 2, bias -1 and
`sourceColorSpace = raw`, following the
[UsdPreviewSurface specification](https://openusd.org/release/spec_usdpreviewsurface.html).
Normal input evaluation and the tangent-frame fallback are described
[below](#path-tracing).

Opacity is **presence coverage**: when `opacity_threshold > 0`, opacity
greater than or equal to it accepts the surface, and smaller values reject
it. With threshold 0, acceptance has probability opacity. A rejected
surface passes the ray straight through; an accepted one has its full BSDF
and emission, without another opacity factor. This scales the expected
entire surface response. It does not model glass, refraction, absorption,
or the specification's `opacityMode = transparent` lighting response;
`opacityMode` is not read. Thresholds are constants, not texture inputs.
Non-finite evaluated texture opacity is treated as zero; finite results
are clamped to [0, 1].

A `Texture` holds decoded texels, four channels each, rows from the top of
the image, so (s, t) = (0, 0) is its bottom-left corner as in USD:
`Rgba8Unorm`, `Rgba8Srgb` (sRGB-encoded colour, linear alpha; lookups
decode it before filtering) or `Rgba32Float`. `LotusScene::textures` keys
them by stable host-supplied identifiers and shares their texels with
retained snapshots; `SetTexture` inserts or replaces one and
`RemoveTexture` removes one. A texture input may name a key before its
texture exists.

`LotusScene::materials` maps stable host-supplied keys to materials, in
deterministic order; `SetMaterial` inserts or replaces one and
`RemoveMaterial` removes one. A mesh binds to a key with `BindMaterial`, and
`SceneMesh::material` holds the key. An empty key, or one with no material,
means the default `Material`, so a binding may name a material before it
exists and outlives the material's removal. `SetMesh` keeps a mesh's binding
when it replaces the geometry or the instance. `LotusScene::environment` is
a constant environment radiance, finite and non-negative, black by default,
changed with `SetEnvironment`. Out-of-range values, an empty material or
texture key, a texture input without a key, with a channel out of range or
with a non-finite scale, bias or fallback, a texture whose texels do not
match its size and format or are not finite, and a binding for a missing
mesh throw `std::invalid_argument` without changing the world.

## Update plan

`include/lotus/extraction.hpp` owns the host-neutral update plan.
`SceneExtraction::Update` compares a snapshot's scene with the one it last
planned and returns a `SceneUpdate`:

- `geometry_releases`: geometry buffers the scene no longer references, in
  the previous scene's key order;
- `geometry_uploads`: geometry buffers that become resident, in key order;
- `texture_releases` and `texture_uploads`: the same for textures. Every
  texture in the scene is resident, whether a material names it or not;
- `instances`: when `instances_changed`, the complete replacement list,
  one `SceneInstance` (geometry, composed `world_from_object`, material
  slot and texture-coordinate set) per placement of each visible mesh with
  triangles, in key order and then placement order. The set is the index,
  in key order, of the geometry's set that the material's lookups read, or
  `kNoTexcoords` when the material has neither a texture input nor a
  non-identity constant normal, or the geometry has no such set;
- `materials`: when `materials_changed`, the complete material table: the
  default `Material` at slot 0, then the scene's materials in key order,
  each a `SceneMaterial` with the resident texture each texture input's key
  names (null without one). A mesh whose binding names no material gets
  slot 0. A GPU scene, and the extraction after `Reset`, start with the
  default alone, so a scene without materials plans no table;
- `environment`: when `environment_changed`, the scene's environment
  radiance. A GPU scene, and the extraction after `Reset`, start from
  black, so only a different environment is planned.

A geometry buffer or a texture is identified by its address. Every mesh with triangles
keeps its geometry resident, even hidden or placed nowhere, so visibility,
transform, placement and binding changes rewrite only the instances, and a
material's value edit rewrites only the material table. Adding or removing
a material moves the later slots, so it rewrites both. A texture edit
replaces the texture (a release and an upload) and the table that samples
it; a material edit that changes its texture-coordinate set rewrites the
instances too. Geometry without
triangles is never resident or instanced. The extraction holds a reference to every resident
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
  storage-buffer offset alignment, the triangles as uint32 triples and,
  when the geometry has normals, at the next aligned offset, the corner
  normals as float triples, then each texture-coordinate set, in key
  order, as float pairs at the next aligned offset. Normals and texture
  coordinates are geometry: changing them uploads a new buffer and builds a
  new BLAS, as a point edit does.
- Geometry occupies a slot in the GPU scene's geometry table. A released
  slot is reused lowest first, so slots are deterministic for a
  deterministic sequence of plans.
- Each resident texture is a device-local, sampled 2D image of one level,
  `R8G8B8A8_UNORM`, `R8G8B8A8_SRGB` or `R32G32B32A32_SFLOAT`, in the
  shader-read layout, with a slot in the GPU scene's texture table. Slots
  are reused lowest first, as geometry slots are. The table holds at most
  1,024 textures, the size of the scene passes' sampled-image array.
- One device-local instance buffer holds a 112-byte record per instance:
  the column-major `world_from_object`, the device addresses of its
  geometry's positions and triangles (zero without acceleration
  structures), the geometry slot, the material slot, the address of
  its corner normals (zero without normals), and the address and index of
  the texture-coordinate set its material reads (zero and `kNoTexcoords`
  without one). It grows when
  needed and is rewritten only when the instances change.
- One device-local material table holds a 544-byte record per material:
  base colour and roughness, emission and metallic, the texture-coordinate
  fallback, the constant tangent normal (with the workflow flag in its fourth
  component), opacity and threshold, specular colour and ior, then one 64-byte lookup per
  texture input, in the order base colour, roughness, metallic, emission,
  normal, opacity and specular colour: its texture slot, its sampler
  (`wrap_s * 4 + wrap_t`), its channel, its mode (a constant, a lookup, or
  a lookup whose key names no texture, which returns the fallback), scale,
  bias and fallback. It reaches the
  device with the first instances and is rewritten whole whenever the plan
  changes it. A plan is rejected when an instance's slot lies outside the
  table, including a table that shrinks below the slots of instances it
  leaves unchanged, when an instance reads a texture-coordinate set its
  geometry lacks, and when the table after the plan samples a texture that
  is not resident.
- The environment is host state: a plan that changes only the environment
  records no GPU work, and the scene pass reads it with each frame.
- Uploads go through a persistent, grow-only host-visible staging buffer,
  in one submission per plan, followed by a barrier that makes them
  visible to later shader reads and transfers. Texture copies go to the
  new images in the transfer layout, which then move to the shader-read
  layout.

`GpuSceneStats` reports resident geometry and textures, instances, material
table entries, their bytes and lifetime upload counts. `ReadBackScene`
copies the device buffers and images back for validation: the corner
normals and texture-coordinate sets (`GpuGeometryContents`), each
instance's set, the material table with each lookup's texture slot
(`GpuMaterialContents`) and the textures' texels (`GpuTextureContents`). A failed update or readback leaves the renderer failed, as a
failed frame does: create a renderer and reset the extraction.

The scene pass traverses the acceleration structures built from these
buffers and reads a hit's instance record and, through its addresses, the
hit triangle. Source-face indices stay on the CPU. Every geometry buffer,
every texture, and every BLAS below, is its own device allocation, so an update fails with an
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
  uploaded: one non-opaque triangle geometry, built for fast trace with
  `VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR`. The query evaluates
  coverage before confirming candidates, once per intersection. Geometry
  may be shared by different alpha materials, and coverage edits rewrite
  the material table without rebuilding a BLAS or TLAS. It lives
  and dies with the buffer, so a point or topology edit builds a new BLAS,
  and hidden geometry keeps its BLAS. BLASes are neither refitted nor
  compacted.
- The TLAS has one instance per instance record, in the same order, so a
  ray query's instance index is the instance record's index. Each instance
  references its geometry's BLAS with the record's transform, mask `0xFF`
  and facing culling disabled; the custom index is unused.
- An instance rewrite in which every instance keeps its BLAS and its
  transform, such as a material binding change, leaves the TLAS as it is. One in
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

### Scene update timestamps

Where the queue supports timestamps (`GpuSceneStats::timestamps_available`),
the plan's submission writes one timestamp before its copies and one after
each of its three phases: the copies, the BLAS builds and the TLAS build or
refit. The barriers between the phases keep each from starting before the
previous one finishes, so `GpuSceneEvidence::timings` gives each phase's
GPU duration as `upload_gpu_ms`, `blas_build_gpu_ms` and
`tlas_build_gpu_ms`. A phase the plan does not need reports 0, and a call
that submits nothing, such as an empty or environment-only plan, reports
`timings.available` false. `renderer.scene.timestamp` times a 131,072-triangle
grid with 1,024 instancer placements through insertion, a transform edit
(a TLAS refit), a material binding, a material edit, an unchanged commit
and removal: every
phase a step runs must measure a positive duration, except the removal's
empty TLAS build, and every other phase must report 0. Its detail lists the
durations; it is a SKIP without timestamp support
([evidence](../reports/2026-10-05-gpu-timestamps.md)).

## Primary rays

`CreateOffscreenRenderer` accepts optional `RayQueryShaders` with explicit
vertex and fragment SPIR-V paths (`path_trace.vert.spv`,
`path_trace.frag.spv`). On devices with acceleration structures,
`VK_KHR_ray_query` and the `rayQuery` feature, it enables ray queries;
The scene passes also need `fragmentStoresAndAtomics`, `shaderInt64` (the
instance records' addresses), RGBA32F colour attachments with
storage and readback, and the texture table: `descriptorBindingPartiallyBound`
and `shaderSampledImageArrayNonUniformIndexing`, descriptor limits that
allow 1,024 sampled images and 16 samplers in the fragment stage, and
linearly filtered RGBA8 and RGBA32F images with transfers.
`RayQueryCapability` reports support
and the reason when any of these is absent. Supplying both shader paths
creates the scene render pass, four persistent pipelines specialized from
the one fragment module (below), a descriptor set and a uniform block
alongside the bootstrap pipeline. The descriptor set's partially bound
array of 1,024 sampled images holds a descriptor for each resident
texture's slot, written before a frame when the textures changed; a
released slot's stale descriptor is never read. Sixteen immutable bilinear
samplers without mipmaps, one per pair of wrap modes, go with it; black is
the transparent black border. Missing
shader files on a supported device fail creation; omitting the paths
preserves bootstrap-only callers and makes `RenderScene` fail with an
explanation.

`RenderScene` unprojects Vulkan near/far clip coordinates at pixel centres,
using the inverse of `DrawSummary::world_to_clip` after clip-space conversion.
It supports perspective (including an infinite far plane) and orthographic
cameras, honours display/data windows, and traces triangles with material coverage without
facing culling. Rays begin at the near plane and stop at the far plane. A
singular or non-finite camera fails before recording GPU work.

The scene passes render into their own RGBA32F colour and D32 depth
targets, separate from the bootstrap's RGBA8 ones; each set follows the
clear flags, target reuse and resize rules. The *camera pass* traces each
pixel centre: its closest alpha-accepted hit writes its projected Vulkan window depth
through the existing depth test, and misses preserve the clear or
preceding attachment values. What the colour holds is
`PathTracingSettings::output`: `Barycentrics` is the camera pass's closest
triangle's barycentric weights with alpha 1, the intersection diagnostic;
the *surface pass* writes a diagnostic of the pixel centre's closest hit,
with alpha 1: `ShadingNormal` is the world-space shading normal (below)
the path tracer
would evaluate the hit's BSDF around, the normal diagnostic
of [design policy section 25](../design/DESIGN_POLICY.md#25-debug-and-validation),
`Albedo` the base colour and `RoughnessMetallic` (roughness, metallic, 0),
both after texture lookups;
`Radiance`, the default, is the accumulated path-traced radiance of the
*radiance pass* (below), drawn after the camera pass has written depth.
The TLAS and accumulation-image descriptors and the uniform block (the
environment, the background, the display-window mapping, the instance
records' address and the settings) are updated before each synchronous
submission, so scene rebuilds cannot leave a stale reference. Before any
scene update, there is no TLAS and the passes only clear.

Depth and surface diagnostics apply the same coverage policy, using a
fixed per-pixel seed at sample index 0, independent of the progressive
radiance sample. For fractional opacity, depth is one deterministic
coverage realization, not averaged depth. Cut-outs give exact visibility.

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

- **Alpha acceptance.** Every triangle candidate evaluates only its opacity
  input before confirmation. Rejection keeps the ray interval, without an
  origin offset, scattering event, throughput change or bounce limit.
  Primary and secondary rays use the same rule. Fractional coverage draws
  from the path RNG; opacity 0, opacity 1 and masks consume no random
  numbers, preserving the opaque reference sample sequence. Primary misses
  use the clear background, secondary misses use the environment. Colour
  alpha with a transparent clear estimates the probability that a primary
  sample accepts any surface.
- **Hit reconstruction.** The instance record's transform places the hit
  triangle's corners in world space; the position is interpolated from the
  barycentrics, and the geometric normal is turned to face the incoming
  ray. Surfaces are two-sided. The hit's material is the material table's
  entry at the instance record's material slot.
- **Texture lookups.** When any of the material's inputs is a lookup, the
  instance's texture-coordinate set is interpolated with the barycentrics
  (or the material's fallback is used), and each lookup samples its
  texture as [above](#materials-and-environment), with the sampler of its
  wrap modes, at (s, 1 - t), since texture rows run from the top. The
  results replace the constants before the BSDF is built. A material
  without lookups interpolates no texture coordinates; a non-identity
  constant normal may still read the set to build its tangent frame.
- **Shading normal.** With supplied normals, the hit triangle's corner
  normals are interpolated with the barycentrics, transformed by the
  cofactor matrix of the transform's linear part (the inverse transpose up
  to a scale) and normalized, then turned to the geometric normal's side.
  The geometric normal stands in without supplied normals, where the
  interpolated normal vanishes, and where the shading normal faces away
  from the incoming ray, which the BSDF could not reflect. The BSDF is
  sampled and evaluated around the shading normal; ray origins are still
  offset along the geometric one.
- **Normal maps.** After the mesh normal is evaluated, the material's
  signed tangent normal perturbs it. The world-space triangle edges and
  its selected corner UVs supply the UV Jacobian. The tangent follows
  increasing s, projected perpendicular to the mesh shading normal and
  normalized; the bitangent is its cross product with that normal, signed
  towards increasing t. Mirrored UVs, mirrored transforms, non-uniform
  scales and shear therefore retain their handedness. Tangents use USD's
  upward t, independent of the image-row flip in texture sampling.
  Missing coordinates, degenerate or ill-conditioned UVs, or a vanishing
  projected tangent use the existing Duff normal-only orthonormal frame.
  The mapped vector is normalized and turned to the geometric normal's
  side. A zero or non-finite vector, or one facing away from the ray,
  leaves the mesh shading normal. The identity `(0, 0, 1)` takes the
  existing path exactly, preserving untextured reference images. Tangents
  are computed per hit; there are no stored vertex tangents or MikkTSpace
  compatibility guarantees.
- **Emission** is added at every hit, the camera's included.
- **BSDF.** A dielectric GGX coat over Lambert, blended with the existing
  GGX metal by `metallic`. Dielectric F0 is `((ior - 1)/(ior + 1))²`;
  `ior = 1` removes the interface and recovers uncoated Lambert. In the
  specular workflow, explicit `specular_color` supplies F0 and metallic and
  ior are ignored. Schlick Fresnel has a white grazing limit (including
  the established metal approximation). The diffuse base is attenuated
  on both entry and exit by `(1 - F(wi.z)) (1 - F(wo.z))`, and by the
  non-metal fraction. Reflection uses height-correlated Smith masking and
  alpha `max(roughness², 0.001)`. This reciprocal, single-scattering coat
  omits internal diffuse reflections, GGX multiple-scattering compensation,
  transmission and refraction. A non-finite specular lookup falls back to
  its constant F0. The GGX selection probability is the metal fraction plus
  the non-metal fraction times the maximum component of view Fresnel,
  clamped to [0.05, 0.95] for an active dielectric. The same probability
  weights the GGX visible-normal and cosine-hemisphere PDFs. Samples use
  the complete BSDF's f·cos divided by that mixture density; invalid
  below-surface samples terminate without resampling. For pure metal,
  the GGX density cancels analytically in the weight for mirror precision.
  [Measured verification](../reports/2026-10-08-dielectric-specular.md).
- **Environment.** A scattered ray that escapes adds the throughput times
  the environment radiance. Camera rays that miss do not see it.
- **Termination.** A path ends after `PathTracingSettings::max_bounces`
  scattering events (64 by default; 0 keeps only the emission the camera
  sees), when a sampled direction is not above the shading normal's
  hemisphere or not above the geometric surface (a surface only reflects;
  with authored normals the second can happen alone), or by Russian
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
`renderer.path.normals` checks the `ShadingNormal` output against an
independent double-precision oracle, and Lambert and mirror radiance that
authored normals decide
([report](../reports/2026-10-06-authored-normals.md));
`renderer.path.textures` checks the `Albedo` and `RoughnessMetallic`
outputs against an independent oracle of Vulkan's bilinear filter and
address modes, and the exact radiance of textured Lambert and emissive
surfaces ([report](../reports/2026-10-06-textures.md));
`renderer.path.normal_maps` compares mapped shading normals with an
independent UV-Jacobian and bilinear oracle, and checks exact mirror
radiance ([report](../reports/2026-10-08-normal-maps.md));
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
  towards the camera. Its walls have a dielectric coat over Lambert:
  white floor, ceiling and back,
  a red left wall and a green right wall. Under the ceiling hangs a black
  emitter with radiance (15, 13, 10). Inside stand a GGX metal block
  (base (0.95, 0.85, 0.6), roughness 0.35) and a white coated diffuse block.
  Dielectrics use the default ior 1.5 and roughness 0.5. The
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

  Both files are committed in `validation/reference/`, regenerated for
  the dielectric coat ([evidence](../reports/2026-10-08-dielectric-specular.md)). Each is a
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
mapping. Subdivision refinement and general concave-polygon tessellation
are not implemented. Materials
are [below](#materials).

The `normals` primvar, which UsdImaging takes from `primvars:normals` over
the `normals` attribute, becomes the corner normals: a constant value for
every corner, a uniform value per corner of its face's triangles, a vertex
or varying value by the corner's point, and face-varying values
triangulated by `HdMeshUtil` the way the faces are, holes and orientation
included. Indexed primvars are flattened. Normals whose count does not
match their interpolation, with an index out of range, of another type
than float triples or with a non-finite value are ignored with a warning,
and the mesh uses the same fallback as one without authored normals.
Authored normals are read when the normals or the primvars are dirty.

Without usable authored normals, a mesh whose subdivision scheme is neither
`none` nor `bilinear` receives Storm's coarse smooth normals, using
`Hd_VertexAdjacency` and `Hd_SmoothNormals` from `hd`. Each point's incident
polygon corners contribute their edge cross products before triangulation;
the sum is normalized and expanded to triangle corners. Left-handed
orientation reverses the contributions. As in Storm, hole faces contribute
to adjacency even though they are not traced. Unreferenced points and
degenerate corners contribute zero; a vanishing interpolated normal uses
the geometric normal in the shader. Point and topology edits recompute the
normals, including scheme, orientation and hole changes; removing authored
normals restores the fallback. Usable authored normals win on every scheme.
This smooths the coarse mesh; it does not refine subdivision surfaces or
evaluate limit normals or subdivision creases. `none` and `bilinear` without
authored normals retain geometric shading normals. The UsdImaging test
compares the generated corners with an independent double-precision oracle
([evidence](../reports/2026-10-07-computed-normals.md)).

Every other primvar whose values are float pairs (`float2`, `double2` or
`half2`, such as `texCoord2f[] primvars:st`) becomes a texture-coordinate
set of its name, expanded to corners in the same way, indexed ones
flattened; one that cannot be used warns and is left out. `points`,
`displayOpacity` and `widths` are not read. Texture-coordinate sets and
constant `displayColor` are read with the normals.

Malformed mesh input is warned about and removes any earlier geometry for
that mesh. A later valid sync recovers it. Mesh destruction removes its scene
record. `HdLotusRenderDelegate::GetFrameSnapshot` exposes the same CPU snapshot
for inspection without creating a GPU renderer.

Each Hydra render pass plans and applies a scene update before its frame,
and a newly created renderer resets the extraction.
On ray-query devices each pass adds one sample with `RenderScene`, with
`max_samples` set to the `convergedSamplesPerPixel` render setting (64 by
default) and `sample_index` to the `lotus:sampleIndex` render setting (0 by
default); the render pass and the colour buffer report convergence when the
accumulation reaches its sample count. Point, normal, topology, transform,
visibility and instancer edits, like camera, framing and
`lotus:sampleIndex` changes, restart the accumulation. Other devices
retain the bootstrap path and converge after one pass. Dome lights feed the
constant environment as [below](#lights); an environment change restarts the
accumulation. The host evidence log identifies the choice as
`ray_query=1` or `0`, with each pass's `sample_index`, `samples` and
`converged`.
`HdLotusRenderDelegate::GetGpuSceneStats` returns the GPU scene after the
latest pass.

### Lights

The delegate supports `domeLight` sprims. Each visible dome contributes
constant linear RGB radiance `color * intensity * 2^exposure`, using Hydra's
`HdLightTokens` parameters and the defaults white, 1 and 0 from
[UsdLux LightAPI](https://openusd.org/release/user_guides/schemas/usdLux/LightAPI.html).
Visibility is read from the scene delegate, including inherited visibility;
`HdLight::DirtyParams` covers it as well as parameter edits, and
`DirtyResource` also refreshes the contribution. Transforms have no effect
on a uniform environment.

Contributions are summed in path order with double precision and stored in
the existing `LotusScene::environment`. A hidden or zero-intensity dome
contributes black. Negative intensity and colour channels are clamped to
zero; non-finite inputs or a per-dome radiance outside float range warn and
make that dome black. A sum above float range saturates to the largest finite
float per channel. This numeric guard is not a useful rendering range.

With no synced domes, the adapter uses its white fallback environment. An
authored dome that is invisible, black or invalid still suppresses that
fallback; destroying the last dome restores it. Fallback sprims never
contribute. The aggregate changes the scene snapshot only when its RGB value
changes, so equivalent lighting edits preserve the accumulation. An
environment-only change neither uploads geometry nor rebuilds acceleration
structures. Environment radiance continues to light secondary misses only;
camera misses keep their AOV clear background.

This is constant dome lighting only: environment textures, colour
temperature, diffuse/specular contribution controls, light/shadow linking,
light filters, instanced lights and other light types are not evaluated.
There is no NEE or environment importance sampling. CTest
`lotus-renderer-hydra-light` checks composed USD edits and snapshot lifetimes;
its GPU-gated `-gpu` variant checks analytic mirror radiance, accumulation
restarts and unchanged upload/build counters
([evidence](../reports/2026-10-08-dome-lights.md)).

### Deterministic mode through Hydra

A converged Hydra image is the backend's deterministic image
([reference images](#reference-images)): a function of the
`lotus:sampleIndex` and `convergedSamplesPerPixel` render settings, the
scene the pass selects, the camera with its framing, and the colour AOV's
size and clear colour. `lotus:sampleIndex` is the RNG seed: the first
sample index, so the k-th pass's sample uses `lotus:sampleIndex + k`.
Indices whose distance is less than the sample count share samples, as
`renderer.path.reference` avoids by starting at 2²⁰. Negative values count
as 0, and a sample count below 1 as 1. The bounce limit stays at the
default 64.

Hydra's frame count does not feed the random numbers, so the converged
image does not depend on how many passes a host ran before, how it
interrupted or restarted the accumulation, or which renderer instance
traced it. One exception: lowering `convergedSamplesPerPixel` below the
samples already accumulated keeps them, as `max_samples` does; the image
is then that larger count's.

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
[render-pass selection report](../reports/2026-10-05-render-pass-selection.md);
deterministic Hydra renders against the backend's in the
[Hydra deterministic mode report](../reports/2026-10-05-hydra-deterministic-mode.md);
the material IR, the material table and the `UsdPreviewSurface`
translation in the [material IR report](../reports/2026-10-05-material-ir.md);
textures, texture coordinates and texture lookups in the
[textures report](../reports/2026-10-06-textures.md).

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

### Materials

The delegate supports the `material` sprim. A material translates its Hydra
material network (`GetMaterialResource`) into the material IR whenever its
parameters or resource are dirty and stores the result under its `SdfPath`
string; destroying the sprim removes it. A mesh binds to its
`GetMaterialId` path, read when its material id is dirty, so a material's
value edit rewrites only the material table and a binding change only the
instances. The fallback material sprim is never stored.

The material translator (`adapters/hydra2/src/material_translator.*`) reads
the network's surface terminal when it is a `UsdPreviewSurface`:

| IR field | Input | Rule |
| --- | --- | --- |
| `base_color`, `base_color_texture` | `diffuseColor` | each component clamped to [0, 1] |
| `roughness`, `roughness_texture` | `roughness` | clamped to [0, 1] |
| `metallic`, `metallic_texture` | `metallic` | clamped to [0, 1]; 0 under `useSpecularWorkflow` |
| `emission`, `emission_texture` | `emissiveColor` | each component clamped to be non-negative |
| `normal`, `normal_texture` | `normal` | signed RGB clamped to [-1, 1], then normalized in the shader |
| `opacity`, `opacity_texture` | `opacity` | clamped to [0, 1]; scalar texture channels including alpha |
| `opacity_threshold` | `opacityThreshold` | constant clamped to [0, 1] |
| `ior` | `ior` | constant clamped to [1, largest finite float]; read in the metallic workflow |
| `use_specular_workflow` | `useSpecularWorkflow` | constant; nonzero selects the specular workflow |
| `specular_color`, `specular_color_texture` | `specularColor` | RGB clamped to [0, 1]; read only in the specular workflow |

An unauthored input, a non-finite value and a value of another type take
the input's default. An input a `UsdUVTexture` drives becomes a texture
lookup:

| `TextureInput` | `UsdUVTexture` | Rule |
| --- | --- | --- |
| `channel` | the connected output | `rgb` for a colour or normal input; `r`, `g`, `b` or `a` for a scalar |
| `texture` | `file`, `sourceColorSpace` | the key `<resolved path>|<colour space>`; the authored path when it does not resolve |
| `wrap_s`, `wrap_t` | `wrapS`, `wrapT` | `black`, `clamp`, `repeat`, `mirror`; `useMetadata`, the default, is black |
| `scale`, `bias`, `fallback` | the same inputs | float4, defaults (1, 1, 1, 1), (0, 0, 0, 0) and (0, 0, 0, 1) |
| `Material::texcoords`, `texcoord_fallback` | `st` | a `UsdPrimvarReader_float2`'s `varname` and `fallback`, or, unconnected, no set and `st`'s value |

The colour space is `sourceColorSpace`, or the `colorSpace:file` input
colour space UsdImaging hands it over as: `raw` and linear or data colour
spaces are raw, `sRGB` and `srgb_*` spaces sRGB, and anything else `auto`.
A material has one texture-coordinate source, its first lookup's. An input
driven by anything else (another node type, a colour output on a scalar,
a `UsdTransform2d` between the reader and the texture, or a second
texture-coordinate source) takes its default and is reported as connected.
A `UsdUVTexture` without a file is a lookup whose key names no image, which
returns its fallback. Any other surface shader, or a network without a
surface, warns and leaves the default material.
Texture-driven ior is reported as an untranslated connection. `opacityMode`,
`clearcoat`, `clearcoatRoughness`, `occlusion` and `displacement` are not
read.

A mesh without a material binding uses its constant `displayColor` as
linear RGB base colour, with components clamped to [0, 1], and the other
material defaults. The adapter stores this private material under the
mesh's `lotus:displayColor` property path, which cannot collide with a
material prim path. Indexed constants are supported; absent, empty,
malformed or non-finite colours use the default grey. Other interpolation
modes are not supported and warn. Authored bindings always win, including
missing or unsupported materials. Primvar edits update the colour, and
binding a surface, removing the colour or removing the mesh releases the
private material. CTest `lotus-renderer-hydra-material` and its `-gpu`
variant check these transitions and the rendered radiance.

The adapter decodes each image a material names with Hio
(`HioImage::OpenForReading` under the source colour space) when the first
material names its key, and removes the texture when the last one stops.
One channel becomes (r, r, r, 1), two (r, r, r, g) and three (r, g, b, 1),
as Storm samples them. 8-bit images stay 8-bit, `Rgba8Srgb` when Hio
reports them sRGB (under `auto`, 8-bit three- and four-channel images
without colour metadata) and `Rgba8Unorm` otherwise; 16-bit, half and float
images become `Rgba32Float`, decoded from sRGB on the CPU when Hio reports
them sRGB. An image that cannot be read, of an unsupported pixel format, or
with a non-finite value warns, and its lookups return their fallbacks. An
image is not read again until no material names it, so editing a file on
disk is not seen while it is in use. UDIM tiles, wrap metadata and premultiplied
alpha are not supported.
