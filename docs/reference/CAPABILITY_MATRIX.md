# Capability matrix

What is implemented now. This page is the only document that says so; the
[design policy](../design/DESIGN_POLICY.md) describes intent and the
[roadmap](../roadmap/README.md) describes what is left.

Legend: ✅ implemented and tested · 🧪 scaffold only (works, but is the
generated bootstrap, not the design) · ⬜ not implemented

Configurations each row was measured on are
[SUPPORTED_CONFIGURATIONS.md](SUPPORTED_CONFIGURATIONS.md).

## Renderer foundation

| Capability | Status | Evidence / notes |
| --- | --- | --- |
| Host-neutral core with an enforced header boundary | ✅ | `renderer.core.boundary`; CTest `lotus-renderer-core-boundary` recursively discovers all public headers, including backend headers. `lotus-renderer-core-boundary-discovery` checks newly added nested headers against foreign dependencies |
| Core and Hydra source CI definition | 🧪 | Registered external workflow and `openstrata.ci.yaml`; matrix and pinned artifact verified locally, core 6/6 and Hydra 11/11 tests pass. GitHub-hosted execution is not measured ([CI report](../reports/2026-10-04-foundation-ci.md)) |
| Vulkan device bring-up with validation layers, messages treated as errors | ✅ | `renderer.backend.capability`, `renderer.validation.messages` |
| Offscreen colour and depth (D32) render products, read back | ✅ | RGBA8 bootstrap and RGBA32F path-traced and barycentric diagnostic output at the requested size, with display/data windows; `renderer.render_product.color`, `.depth`, `renderer.ray_query.triangle`, `renderer.path.bsdf`, `.accumulation` |
| Foundation AOV formats and clears | ✅ | [AOV reference](AOVS.md); transparent-black colour, window depth 1, CPU ID sentinels. `renderer.aov.clears` checks full-target clears, empty scenes and preservation across frames; [measured run](../reports/2026-10-04-foundation-aovs.md) |
| Repeated frames on persistent resources | 🧪 | `Lotus::OffscreenRenderer`: the instance, device and pipeline outlive frames, and the targets are recreated only when their size changes; one frame in flight, read back each frame. 1,000 deterministic frames, another at the same size and a resize; `renderer.gpu.frame`, `renderer.frame.persistence`. The Hydra adapter keeps one renderer across frames (usdview smoke test) |
| Swapchain presentation, one frame in flight | 🧪 | `lotus-viewport`; the bootstrap triangle through the core camera, with its perspective aspect updated from the framebuffer extent |
| Slang shaders compiled to SPIR-V | ✅ | bootstrap `triangle.slang` and the scene pass `path_trace.slang`, which imports the `common/`, `bsdf/` and `sampling/` modules and explicitly enables `spvRayQueryKHR`. A shader change relinks the adapters that copy the SPIR-V |
| Camera | ✅ | view and projection in the core, converted to Vulkan clip space by the backend; all three entry points supply it. The headless runner's perspective camera is checked by `renderer.render_product.color`, `.depth`; `lotus-viewport-camera` checks square, landscape and portrait projection and scene revisions on resize ([foundation follow-up](../reports/2026-10-04-foundation-camera-boundary.md)) |
| Per-pass GPU timestamps | ✅ | persistent query pools, queue capability gated. The scene pass: `GpuFrameEvidence::primary_ray_gpu_ms` and `renderer.ray_query.timestamp` (measured on the barycentric output). A scene update's copies, BLAS builds and TLAS build or refit: `GpuSceneEvidence::timings` and `renderer.scene.timestamp` ([scene reference](SCENE.md#scene-update-timestamps), [measured run](../reports/2026-10-05-gpu-timestamps.md)). The camera and radiance passes are timed together |
| Deterministic mode and golden-image tests | ✅ | [design policy §26](../design/DESIGN_POLICY.md#26-reference--deterministic-mode): an image is a function of its first sample index, sample count, scene, camera and target. The headless runner's reference images ([scene reference](SCENE.md#reference-images)) are compared statistically, not bitwise; Hydra exposes the first sample index as `lotus:sampleIndex` ([scene reference](SCENE.md#deterministic-mode-through-hydra), [measured run](../reports/2026-10-05-hydra-deterministic-mode.md)) |

## Light transport

Phases are the [roadmap policy's](../design/ROADMAP_POLICY.md#4-roadmap).

| Capability | Status | Phase |
| --- | --- | --- |
| CPU mesh extraction, geometry and ordinary mesh placement | ✅ | [Scene reference](SCENE.md); dirty points/topology/transform/visibility, coarse triangulation and immutable snapshots; [measured tests](../reports/2026-10-05-cpu-mesh-extraction.md) |
| Vertex, index and instance buffers; GPU scene upload | ✅ | [Scene reference](SCENE.md#gpu-scene): an incremental plan from snapshot changes, one device buffer per geometry and an instance buffer. CTest `lotus-renderer-scene-update`; `renderer.scene.upload` compares read-back buffers with the CPU scene; [measured run](../reports/2026-10-05-gpu-scene-upload.md) |
| BLAS / TLAS | ✅ | [Scene reference](SCENE.md#acceleration-structures): a BLAS per geometry, transform-only TLAS refits, material-only rewrites leave the TLAS, other rewrites rebuild; capability gated. Build inputs and counters checked by `renderer.scene.acceleration`; [measured run](../reports/2026-10-05-blas-tlas.md) |
| Ray-query capability and traversal | ✅ | [Scene reference](SCENE.md#primary-rays); `renderer.ray_query.capability`, `.triangle`, explained SKIPs without `VK_KHR_ray_query`/`rayQuery`; [measured run](../reports/2026-10-05-primary-rays.md) |
| Primary rays and closest triangle intersection | ✅ | perspective/orthographic camera, near/far clipping, barycentric RGB and projected depth; independent CPU projection comparisons across scene edits and framing |
| Surface hit reconstruction, multiple bounces and Russian roulette | ✅ | [Scene reference](SCENE.md#path-tracing): one brute-force path per pixel centre in a fragment pass, `PathTracingSettings::max_bounces` and `sample_index`. `renderer.path.multibounce`: exact bounce-limited radiance and the converged mean in a closed emissive box; [measured run](../reports/2026-10-05-bsdf-multibounce.md) |
| Lambert, minimal GGX, emissive surfaces | ✅ | Per-mesh `SurfaceMaterial` (Lambert and a GGX metal mixed by `metallic`, two-sided emission) until the material IR; `renderer.path.bsdf` compares exact Lambert radiance and GGX directional albedo from independent quadrature. No dielectric specular layer |
| Environment light | ✅ | A constant environment radiance on `LotusScene`, uniform, not seen by camera rays; Hydra sets a white fallback. Dome lights and environment maps are not read |
| HDR accumulation, 1-pixel box filter, float colour output | ✅ | [Scene reference](SCENE.md#accumulation-and-the-pixel-filter): an RGBA32F accumulation restarted by scene, camera, framing and settings changes, capped by `max_samples`; Hydra converges progressively at `convergedSamplesPerPixel`. `renderer.path.accumulation`: box coverage against each pixel's projected-triangle area, unclamped HDR radiance, split frames and restart rules; [measured run](../reports/2026-10-05-hdr-accumulation.md) |
| Deterministic reference images — the reference path tracer | ✅ | [Scene reference](SCENE.md#reference-images): a Cornell box at 1/16/64/256/1024 spp, compared with a committed 1024-spp mean and per-pixel variance. Statistical match (DES-Q5): image and tile mean differences within 5 standard errors. `renderer.path.reference` also checks that a 10% brighter wall is rejected; [measured run](../reports/2026-10-05-reference-images.md) |
| Minimal material IR; basic `UsdPreviewSurface` translation | ⬜ | Renderer Phase 1.5 |
| Wavefront queues, compaction, indirect dispatch | ⬜ | Renderer Phase 2 |
| NEE, MIS, environment importance sampling | ⬜ | Renderer Phase 3 |
| Temporal infrastructure: motion vectors, history, validation, accumulation | ⬜ | Renderer Phase 4 |
| ReSTIR DI | ⬜ | Renderer Phase 5 |
| SVGF-class denoising | ⬜ | Renderer Phase 6 |
| ReSTIR GI / advanced reservoir transport | ⬜ | Renderer Phase 7 |
| Full `UsdPreviewSurface`, OpenPBR, MaterialX Standard Surface; textures | ⬜ | Renderer Phase 8 |
| Adaptive sampling, sorting, light tree, path guiding | ⬜ | Renderer Phase 9 |
| Spectral transport | ⬜ | Renderer Phase 10 |
| RT pipeline or CPU reference traversal backend | ⬜ | unscheduled ([roadmap policy §6](../design/ROADMAP_POLICY.md#6-backend-strategy)) |
| Debug AOVs beyond colour / depth / primId | ⬜ | [design policy §25](../design/DESIGN_POLICY.md#25-debug-and-validation) |

## Hydra adapter (`hdLotus`)

| Capability | Status | Evidence / notes |
| --- | --- | --- |
| Plugin discovery through `plugInfo.json` | ✅ | `renderer.plugin.discovery` |
| Render delegate creation | ✅ | `renderer.delegate.creation`; measured against OpenUSD 26.08 (`HD_API_VERSION` 98) |
| CPU colour / depth / primId `HdRenderBuffer`s | ✅ | `renderer.render_buffer.cpu`; `lotus-renderer-hydra-render-buffer` checks descriptors, formats, colour conversion, map guards and row order. Float32 colour by default, 8-bit accepted. IDs are clear sentinels only ([AOV reference](AOVS.md)) |
| AOV binding validation and empty-scene output | ✅ | `lotus-renderer-hydra-aov`: all bindings checked before rendering, host clear values, no-clear preservation at the same size, disappearance of the last mesh and depth-only recovery. GPU capability-gated CTest; [measured run](../reports/2026-10-04-foundation-aovs.md) |
| First frame and a stable update in `testusdview` | ✅ | `renderer.host.first_frame`, `.host.stable_update`; progressive path tracing through the Hydra camera/framing to 64 spp on ray-query devices, bootstrap otherwise, with no Vulkan validation message; [measured run](../reports/2026-10-05-hdr-accumulation.md) |
| GPU capability gate for `testusdview` | ✅ | Explicit `renderer.gpu.frame` SKIP returns before launching the viewer; CTest reports the host test as skipped. Failed, missing, malformed or unexplained evidence fails. `lotus-renderer-host-capability-gate` and [CI report](../reports/2026-10-04-foundation-ci.md) |
| Supported prim types | ✅ | `mesh` (coarse geometry and ordinary or instanced placement extracted, uploaded and traced on ray-query devices), `camera` (through render pass state), `renderBuffer` |
| Render-pass selection | ✅ | A pass traces the meshes under its collection's root paths and outside its exclude paths whose render tag it was given; the rest are hidden for that pass, their geometry resident ([scene reference](SCENE.md#render-pass-selection)). `lotus-renderer-hydra-render-pass` (and its GPU-gated `-gpu` variant) checks UsdImaging-synced purposes and collection edits; [measured run](../reports/2026-10-05-render-pass-selection.md). Material tags are ignored |
| Render settings | 🧪 | `convergedSamplesPerPixel` and `lotus:sampleIndex`, the first sample index of deterministic mode ([scene reference](SCENE.md#deterministic-mode-through-hydra)). `lotus-renderer-hydra-deterministic` checks that a converged Hydra image is bit for bit the backend's for the same settings, camera and AOV, across setting changes, camera moves and render delegates; the usdview smoke test changes the setting in the viewer; [measured run](../reports/2026-10-05-hydra-deterministic-mode.md) |
| Instancers | ✅ | `HdInstancer` placements: point, nested point and native instancing; translations, rotations, scales and instance transforms; one BLAS per geometry and one TLAS instance per placement ([scene reference](SCENE.md#instancers)). `lotus-renderer-hydra-instancer` compares UsdImaging-synced placements with UsdGeom's; the usdview smoke test traces a point instancer; [measured run](../reports/2026-10-05-hydra-instancers.md). Per-instance primvars are not read |
| Materials, lights | ⬜ | every mesh has the default `SurfaceMaterial` under a white fallback environment |

## Hosts

| Host | Status | How |
| --- | --- | --- |
| Headless runner (`lotus-headless`) | ✅ | runs during `ost build`; writes `renderer-report.json` |
| Standalone viewport (`lotus-viewport`) | 🧪 | `ost renderer viewport` |
| `usdview` | 🧪 | `testusdview` in CTest; `ost renderer view` opens the smoke scene interactively ([building guide](../guides/BUILDING.md#the-hydra-adapter)) |
