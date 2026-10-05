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
| Offscreen colour (RGBA8) and depth (D32) render products, read back | ✅ | bootstrap and primary-ray diagnostic output at the requested size, with display/data windows; `renderer.render_product.color`, `.depth`, `renderer.ray_query.triangle` |
| Foundation AOV formats and clears | ✅ | [AOV reference](AOVS.md); transparent-black colour, window depth 1, CPU ID sentinels. `renderer.aov.clears` checks full-target clears, empty scenes and preservation across frames; [measured run](../reports/2026-10-04-foundation-aovs.md) |
| Repeated frames on persistent resources | 🧪 | `Lotus::OffscreenRenderer`: the instance, device and pipeline outlive frames, and the targets are recreated only when their size changes; one frame in flight, read back each frame. 1,000 deterministic frames, another at the same size and a resize; `renderer.gpu.frame`, `renderer.frame.persistence`. The Hydra adapter keeps one renderer across frames (usdview smoke test) |
| Swapchain presentation, one frame in flight | 🧪 | `lotus-viewport`; the bootstrap triangle through the core camera, with its perspective aspect updated from the framebuffer extent |
| Slang shaders compiled to SPIR-V | ✅ | bootstrap `triangle.slang` and diagnostic `primary_ray.slang`; the latter explicitly enables `spvRayQueryKHR` |
| Camera | ✅ | view and projection in the core, converted to Vulkan clip space by the backend; all three entry points supply it. The headless runner's perspective camera is checked by `renderer.render_product.color`, `.depth`; `lotus-viewport-camera` checks square, landscape and portrait projection and scene revisions on resize ([foundation follow-up](../reports/2026-10-04-foundation-camera-boundary.md)) |
| Primary-ray GPU timestamps | ✅ | persistent query pool, queue capability gated; `GpuFrameEvidence::primary_ray_gpu_ms` and `renderer.ray_query.timestamp`. Other passes remain [roadmap work](../roadmap/current.md) |
| Deterministic mode and golden-image tests | ⬜ | [design policy §26](../design/DESIGN_POLICY.md#26-reference--deterministic-mode) |

## Light transport

Phases are the [roadmap policy's](../design/ROADMAP_POLICY.md#4-roadmap).

| Capability | Status | Phase |
| --- | --- | --- |
| CPU mesh extraction, geometry and ordinary mesh placement | ✅ | [Scene reference](SCENE.md); dirty points/topology/transform/visibility, coarse triangulation and immutable snapshots; [measured tests](../reports/2026-10-05-cpu-mesh-extraction.md) |
| Vertex, index and instance buffers; GPU scene upload | ✅ | [Scene reference](SCENE.md#gpu-scene): an incremental plan from snapshot changes, one device buffer per geometry and an instance buffer. CTest `lotus-renderer-scene-update`; `renderer.scene.upload` compares read-back buffers with the CPU scene; [measured run](../reports/2026-10-05-gpu-scene-upload.md) |
| BLAS / TLAS | ✅ | [Scene reference](SCENE.md#acceleration-structures): a BLAS per geometry, transform-only TLAS refits and other rewrites rebuild; capability gated. Build inputs and counters checked by `renderer.scene.acceleration`; [measured run](../reports/2026-10-05-blas-tlas.md) |
| Ray-query capability and traversal | ✅ | [Scene reference](SCENE.md#primary-rays); `renderer.ray_query.capability`, `.triangle`, explained SKIPs without `VK_KHR_ray_query`/`rayQuery`; [measured run](../reports/2026-10-05-primary-rays.md) |
| Primary rays and closest triangle intersection | ✅ | perspective/orthographic camera, near/far clipping, barycentric RGB and projected depth; independent CPU projection comparisons across scene edits and framing |
| Multiple bounces and Russian roulette | ⬜ | Renderer Phase 1 |
| Lambert, minimal GGX, emissive surfaces | ⬜ | Renderer Phase 1 |
| Environment light | ⬜ | Renderer Phase 1 |
| HDR accumulation; deterministic reference images — the reference path tracer | ⬜ | Renderer Phase 1 |
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
| CPU colour / depth / primId `HdRenderBuffer`s | ✅ | `renderer.render_buffer.cpu`; `lotus-renderer-hydra-render-buffer` checks descriptors, formats, map guards and row order. IDs are clear sentinels only ([AOV reference](AOVS.md)) |
| AOV binding validation and empty-scene output | ✅ | `lotus-renderer-hydra-aov`: all bindings checked before rendering, host clear values, no-clear preservation at the same size, disappearance of the last mesh and depth-only recovery. GPU capability-gated CTest; [measured run](../reports/2026-10-04-foundation-aovs.md) |
| First frame and a stable update in `testusdview` | ✅ | `renderer.host.first_frame`, `.host.stable_update`; primary rays through the Hydra camera/framing on ray-query devices, bootstrap otherwise, with no Vulkan validation message; [measured run](../reports/2026-10-05-primary-rays.md) |
| GPU capability gate for `testusdview` | ✅ | Explicit `renderer.gpu.frame` SKIP returns before launching the viewer; CTest reports the host test as skipped. Failed, missing, malformed or unexplained evidence fails. `lotus-renderer-host-capability-gate` and [CI report](../reports/2026-10-04-foundation-ci.md) |
| Supported prim types | ✅ | `mesh` (coarse geometry and ordinary placement extracted, uploaded and traced on ray-query devices), `camera` (through render pass state), `renderBuffer`; collection/render-tag filtering and instancers remain incomplete |
| Instancers, materials, lights, render settings | ⬜ | |

## Hosts

| Host | Status | How |
| --- | --- | --- |
| Headless runner (`lotus-headless`) | ✅ | runs during `ost build`; writes `renderer-report.json` |
| Standalone viewport (`lotus-viewport`) | 🧪 | `ost renderer viewport` |
| `usdview` | 🧪 | `testusdview` in CTest; `ost renderer view` not yet run |
