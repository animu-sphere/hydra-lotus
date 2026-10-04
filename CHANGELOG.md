# Changelog

All notable changes to `hydra-lotus` are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). Each released
version will have a record in [docs/releases/](docs/releases/README.md).

## [Unreleased]

### Added

- The project, generated with `ost init --template renderer --name lotus`
  (OpenStrata 0.23.14, template 0.5.4): the host-neutral core, the Vulkan
  backend, the headless runner, the standalone viewport and the `hdLotus`
  Hydra adapter, all drawing the template's bootstrap triangle.
- A `hydra` build intent in `openstrata.toml` that builds the Hydra adapter.
- Documentation: the design policy and integration scope; the project
  layout; the capability matrix and measured configurations; the roadmap;
  the building guide; and the first `ost` dogfooding report.

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
