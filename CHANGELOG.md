# Changelog

All notable changes to `hydra-lotus` are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). Each released
version will have a record in [docs/releases/](docs/releases/README.md).

## [Unreleased]

### Added

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
