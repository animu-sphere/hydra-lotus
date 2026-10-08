# Changelog

All notable changes to `hydra-lotus` are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). Each released
version will have a record in [docs/releases/](docs/releases/README.md).

## [Unreleased]

### Added

- Opacity coverage and alpha masks (Renderer Phase 1.5).
  `UsdPreviewSurface.opacity` translates constants or scalar texture lookups,
  including alpha, and `opacityThreshold` translates constants. Positive
  thresholds select cut-outs; zero selects stochastic presence coverage.
  Non-opaque ray-query candidates test coverage for primary and secondary
  rays without consuming bounces on rejected surfaces. Depth uses a fixed
  coverage realization. `renderer.path.opacity` and the Hydra texture CPU
  and GPU tests cover layer mixtures, background alpha, masks and edits.
  Glass and `opacityMode` semantics are not implemented.

- Tangent-space normal inputs and normal maps (Renderer Phase 1.5).
  `UsdPreviewSurface.normal` translates signed constants or RGB texture
  lookups, with authored scale, bias and colour space. The path tracer
  builds per-hit UV frames about the mesh shading normal, preserving
  mirrored UVs and transforms, with a stable frame for absent or degenerate
  UVs. Headless normal diagnostics and exact mirror radiance, plus Hydra
  image, UV and connection edits, cover the implementation.

- Constant `displayColor` fallback for Hydra meshes without a material
  binding, including indexed colours and live colour edits. Authored
  materials take priority; missing or unusable colours keep the default
  grey. Private fallback materials are released on binding, colour removal
  and mesh removal. The Hydra material CPU and GPU tests cover these
  transitions and rendered radiance.

- Textures (Renderer Phase 1.5). A material input can be a texture lookup
  (`Lotus::TextureInput`: a texture key, the channel a scalar reads, wrap
  modes, scale, bias and fallback) that reads the mesh's texture-coordinate
  set the material names (`MeshGeometry::texcoords`, one (s, t) per
  triangle corner). `LotusScene::textures` keys decoded `Lotus::Texture`s:
  8-bit linear, 8-bit sRGB or float. The update plan uploads and releases
  textures and resolves each lookup's texture (`SceneMaterial`) and each
  instance's set. The GPU scene keeps one image per texture in a bindless
  table of 1,024 that the path tracer samples bilinearly, and reads them
  back. `SceneOutput::Albedo` and `SceneOutput::RoughnessMetallic` render
  the inputs after the lookups. The Hydra adapter translates `UsdUVTexture`
  with `UsdPrimvarReader_float2`, decodes images with Hio and reads every
  float-pair primvar as a texture-coordinate set. `renderer.path.textures`,
  `renderer.scene.upload` and `lotus-renderer-hydra-texture` (and `-gpu`)
  check them. The scene passes now also require
  `descriptorBindingPartiallyBound` and
  `shaderSampledImageArrayNonUniformIndexing`.

- Authored normals as shading normals (Renderer Phase 1.5).
  `MeshGeometry::normals` holds one object-space normal per triangle
  corner; the GPU scene stores them after the triangles and the instance
  record carries their address. The path tracer interpolates them,
  transforms them by the inverse transpose and samples the BSDF around
  them, falling back to the geometric normal where they vanish or face
  away from the ray, and ends a path whose sampled direction lies below
  the geometric surface. `SceneOutput::ShadingNormal` renders the shading
  normal as a diagnostic. The Hydra adapter reads the `normals` primvar in
  every interpolation, indexed ones included. `renderer.path.normals` and
  `lotus-renderer-hydra-normals` check them. The scene passes now require
  `shaderInt64`.

- The Lotus material IR (Renderer Phase 1.5). `Lotus::Material`
  (`include/lotus/material.hpp`) holds constant base colour, roughness,
  metallic and emission. `LotusScene::materials` keys them; meshes bind to
  a key with `RenderWorld::BindMaterial`, and an unbound mesh, or one bound
  to a missing key, uses the default. The update plan carries a material
  table (`SceneUpdate::materials`, the default at slot 0) and each
  instance's slot, so a material's value edit rewrites only the table; the
  GPU scene keeps it in a device buffer that the path tracer reads, reports
  it in `GpuSceneStats` and reads it back in `GpuSceneContents::materials`.
  The Hydra adapter supports the `material` sprim: it translates
  `UsdPreviewSurface`'s constant `diffuseColor`, `roughness`, `metallic`
  and `emissiveColor` and follows mesh material bindings.
  `lotus-renderer-hydra-material` checks the translator and a UsdImaging
  stage through edits, rebinding and removal, and its `-gpu` variant exact
  Lambert radiance in a Hydra image.

- Per-pass GPU timestamps for scene updates. Each `UpdateScene`
  submission times its copies, BLAS builds and TLAS build or refit
  separately: `GpuSceneEvidence::timings`, with
  `GpuSceneStats::timestamps_available` saying whether the queue supports
  timestamps. The headless runner's `renderer.scene.timestamp` times a
  131,072-triangle grid with 1,024 instancer placements through insertion,
  a TLAS refit, a material edit, an unchanged commit and removal, and
  reports the durations.

- Deterministic mode through Hydra. The `lotus:sampleIndex` render setting
  (0 by default) sets the first sample index of the accumulation, which
  seeds its random numbers; changing it restarts the accumulation. With
  `convergedSamplesPerPixel`, the camera and the AOV it fixes the converged
  image. The host evidence log records each pass's `sample_index`.
  `lotus-renderer-hydra-deterministic` compares converged Hydra images bit
  for bit with the backend's own renders across setting changes, camera
  moves and new render delegates, and the usdview smoke test changes the
  setting in the viewer.

- Render-pass selection. A Hydra render pass traces only the meshes under
  its collection's root paths and outside its exclude paths whose render
  tag it was given (any, without tags). Excluded meshes are hidden for the
  pass with their geometry resident, so toggling a purpose in usdview
  rewrites only the instances. `HdLotusRenderDelegate::GetSelectedSnapshot`
  returns the snapshot the latest pass traced.
  `lotus-renderer-hydra-render-pass` syncs a USD stage through UsdImaging
  and checks the traced meshes across render-tag, purpose, visibility and
  collection changes; `lotus-renderer-hydra-render-pass-gpu` also checks
  the GPU scene's instances and that deselection uploads nothing.

- Hydra instancers. `HdLotusInstancer` places an instancer's prototypes
  from its translation, rotation, scale and transform primvars, nested
  instancers included, so point instancers and native instancing render.
  `MeshInstance::instancer_transforms` holds a prototype's placements, and
  the update plan lists one instance per placement over one resident
  geometry and BLAS. `lotus-renderer-hydra-instancer` syncs a USD stage
  through UsdImaging and compares the placements with UsdGeom's;
  `renderer.scene.upload`, `.scene.acceleration` and
  `.ray_query.triangle` cover instanced placements.

- Reference images, the reference path tracer milestone. `lotus-headless`
  renders a fixed Cornell box at 1, 16, 64, 256 and 1024 spp
  (`--images <directory>` writes them as PFM) and compares each image with
  the committed reference in `validation/reference/`: a 1024-spp mean and a
  per-pixel variance, written by deterministic mode
  (`--write-reference <directory>`). The comparison settles DES-Q5. An image
  matches when its mean difference over the whole image and over every tile
  is within 5 standard errors. `renderer.path.reference` runs it, and also
  checks that a brighter red wall is rejected. The installed product
  carries the reference in `bin/reference/`.

- HDR accumulation, the first physically correct image. `RenderScene`'s
  `Radiance` output adds `frame_count` samples per pixel to a persistent
  RGBA32F accumulation through a 1-pixel box filter and writes the
  unclamped mean as an `rgba32-sfloat` colour product; a missed sample counts
  as the clear colour. The accumulation restarts when the scene, camera,
  target framing, `sample_index` or `max_bounces` changes, and
  `PathTracingSettings::max_samples` caps it;
  `GpuFrameEvidence::samples_per_pixel` reports it. The scene pass is a
  camera pass for depth and barycentrics and a radiance pass, specialized
  from one shader module. The Hydra colour AOV defaults to
  `HdFormatFloat32Vec4` (8-bit buffers are still accepted) and converges
  progressively at the `convergedSamplesPerPixel` render setting, 64 by
  default. `renderer.path.accumulation` checks the filter's coverage against
  the projected area in each pixel, HDR output, split frames and the
  restart rules.

- The reference path tracer's transport: surface hit reconstruction from
  the instance records and geometry device addresses, a Lambert and
  GGX-metal BSDF, two-sided emission, a constant environment, multiple
  bounces with Russian roulette, and PCG sampling seeded by pixel and
  `PathTracingSettings::sample_index`. `RenderScene` writes one clamped
  sample per pixel by default and keeps the barycentric diagnostic as an
  output. `LotusScene` gains a per-mesh `SurfaceMaterial` and an environment
  radiance, carried by the update plan; the Hydra adapter uses the default
  material under a white fallback environment. Light-carrying shader values
  are a `Spectrum` type that holds RGB (DES-Q3). `renderer.path.bsdf` and
  `renderer.path.multibounce` check the transport against closed-form
  radiance and independent quadrature.

- Primary camera rays through the GPU scene's TLAS using Vulkan ray queries:
  diagnostic triangle barycentrics and projected depth, persistent pipeline
  and descriptors, perspective/orthographic camera support and GPU timestamps.
  Hydra uses this pass on supported devices. Headless evidence compares it
  against independent CPU projections across scene edits, clipping, framing
  and resize; missing ray-query capability produces explained SKIPs.

- Acceleration structures in the GPU scene: a BLAS per resident geometry
  and a TLAS over the instances, refitted when a rewrite changes only
  transforms and rebuilt otherwise, built in each plan's one submission.
  They are enabled when the device has `VK_KHR_acceleration_structure`; a
  `renderer.scene.acceleration` check compares the read-back TLAS build
  input and the build counts with the scene, and SKIPs with the reason on
  devices without them.

- The scene update plan (`SceneExtraction`, `SceneUpdate`) and the GPU
  scene: incremental geometry uploads and releases and instance rewrites
  into device-local buffers owned by `OffscreenRenderer`, with a
  `renderer.scene.upload` check that compares read-back buffers with the
  CPU scene. The Hydra adapter uploads its extracted meshes on every pass.

- Host-neutral CPU mesh geometry and placement in `LotusScene`, immutable
  frame snapshots, and Hydra coarse-mesh extraction with dirty updates,
  winding/hole handling and removal. CPU and Hydra tests cover snapshot
  lifetime, malformed input and multi-triangle bootstrap AOV compatibility.

- Source CI contracts for runtime-free core checks and digest-pinned Hydra
  builds, registered with OpenStrata as an external renderer workflow.
- A capability gate for the usdview smoke test: explicit GPU SKIPs skip the
  viewer, while failed or invalid evidence remains an error; regression checks
  cover capability verdicts and preserve staging on a skip.

- The Renderer Phase 0 AOV reference: colour/depth formats and CPU ID
  sentinels, with binding and clear-value validation, a Hydra AOV integration
  test and `renderer.aov.clears` GPU evidence.

- The project, generated with `ost init --template renderer --name lotus`
  (OpenStrata 0.23.14, template 0.5.4): the host-neutral core, the Vulkan
  backend, the headless runner, the standalone viewport and the `hdLotus`
  Hydra adapter, all drawing the template's bootstrap triangle.
- A `hydra` build intent in `openstrata.toml` that builds the Hydra adapter.
- Documentation: the design policy and integration scope; the project
  layout; the capability matrix and measured configurations; the roadmap;
  the building guide; and the first `ost` dogfooding report.
- The roadmap policy (`docs/design/ROADMAP_POLICY.md`), which now owns the
  Renderer Phase 0–10 sequence: Renderer Phase 1 is the reference path
  tracer (with a minimal GGX), a Renderer Phase 1.5 brings a minimal material
  IR forward, version numbers follow milestones, and the near-term priority
  is the Hydra-mesh-to-AOV vertical slice. It resolves the design policy's
  DES-Q1 and DES-Q2 and opens DES-Q5 and DES-Q6.

- A camera in the core (`Lotus::Camera`: view and projection, OpenGL
  conventions) that the Hydra adapter fills from the render pass state and
  the backend converts to Vulkan clip space. The bootstrap triangle is now in
  world space, where the usdview smoke scene's mesh is, and the headless
  runner looks at it through a perspective camera.
- `Lotus::OffscreenRenderer` (`CreateOffscreenRenderer`): a Vulkan instance,
  device and pipeline that outlive frames, with targets recreated only when
  their size changes. The Hydra adapter keeps one across frames instead of
  creating a Vulkan instance, device and pipeline for every frame, and its
  frame evidence records `renderer_creations` and `target_creations`, which
  the usdview smoke test checks. `RenderOffscreen` remains as a one-shot
  wrapper.

### Changed

- `SceneUpdate::materials` holds `SceneMaterial`s (a material and the
  textures its lookups read) and `GpuSceneContents::materials`
  `GpuMaterialContents` (a material and its lookups' texture slots). The GPU
  instance record grows from 96 to 112 bytes and the material record from
  32 to 304. The `ShadingNormal` output is one of three surface-pass
  diagnostics. `UsdPreviewSurface` inputs driven by a `UsdUVTexture` are
  translated instead of left at their defaults. The Cornell reference is
  reproduced bit for bit.

- `SurfaceMaterial` and `RenderWorld::SetMeshMaterial` are replaced by the
  material IR: `SetMaterial`, `RemoveMaterial` and `BindMaterial`.
  `SceneInstance::material` and `GpuInstanceContents::material_slot` are
  material table slots, and the GPU instance record shrinks from 128 to 96
  bytes. The headless Cornell box shares one material among its white
  surfaces and still reproduces the committed reference bit for bit.

- Offscreen colour clears match the AOV descriptor's transparent black.
  Hydra colour/depth clear values reach the GPU; clears cover the whole
  target, and empty scenes converge with cleared output. Empty clear values
  preserve the preceding attachments at the same size; clear changes reuse
  targets.

- The viewport supplies a perspective `Lotus::Camera` through `RenderWorld`
  and render extraction, updating its aspect from the framebuffer size.
  CPU tests check projection on square, landscape and portrait extents,
  resize revisions and unchanged-camera stability.
- The core boundary test discovers all public headers recursively at test
  time, including backend headers, and rejects GLFW, SDL and Slang
  dependencies alongside the existing checks. A regression test checks
  forbidden dependencies introduced in newly added nested headers.
- The Hydra adapter renders at the AOV's resolution instead of upscaling a
  64×64 image, honours the framing's display and data windows, and writes
  rows bottom-up as Hydra's render buffers expect. `RenderOffscreen` takes
  an `OffscreenTarget`.
- The usdview smoke test fails on any Vulkan validation message in a Hydra
  frame.
- `adapters/headless/main.cpp` is ASCII-only. One em dash in a comment made
  MSVC print that file's `/showIncludes` notes in a form Ninja did not
  recognize on a Japanese host, so the headless runner recorded no header
  dependencies and a header edit did not rebuild it.
- Adapters relink when their SPIR-V changes, so their shader copies and the
  headless report no longer survive a shader-only edit. A material-only
  instance rewrite no longer refits the TLAS.
