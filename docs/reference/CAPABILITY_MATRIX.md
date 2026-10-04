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
| Host-neutral core with an enforced header boundary | ✅ | `renderer.core.boundary`; CTest `lotus-renderer-core-boundary` |
| Vulkan device bring-up with validation layers, messages treated as errors | ✅ | `renderer.backend.capability`, `renderer.validation.messages` |
| Offscreen colour (RGBA8) and depth (D32) render products, read back | 🧪 | the rasterized bootstrap triangle at the requested size, with a display and a data window; the headless runner renders 64×64; `renderer.render_product.color`, `.depth` |
| Repeated frames on persistent resources | 🧪 | `Lotus::OffscreenRenderer`: the instance, device and pipeline outlive frames, and the targets are recreated only when their size changes; one frame in flight, read back each frame. 1,000 deterministic frames, another at the same size and a resize; `renderer.gpu.frame`, `renderer.frame.persistence`. The Hydra adapter keeps one renderer across frames (usdview smoke test) |
| Swapchain presentation, one frame in flight | 🧪 | `lotus-viewport`; the bootstrap triangle |
| Slang shaders compiled to SPIR-V | 🧪 | `backend/vulkan/shaders/triangle.slang` only |
| Camera | ✅ | view and projection in the core, converted to Vulkan clip space by the backend; the headless runner's perspective camera is checked by `renderer.render_product.color`, `.depth` |
| Per-pass GPU timestamps | ⬜ | [design policy §24](../design/DESIGN_POLICY.md#24-gpu-profiling) |
| Deterministic mode and golden-image tests | ⬜ | [design policy §26](../design/DESIGN_POLICY.md#26-reference--deterministic-mode) |

## Light transport

| Capability | Status | Phase |
| --- | --- | --- |
| BLAS / TLAS, ray query traversal | ⬜ | Renderer Phase 1 |
| Primary rays, triangle intersection, multiple bounces, Russian roulette | ⬜ | Renderer Phase 1 |
| Lambert, GGX, emissive surfaces | ⬜ | Renderer Phase 1 |
| Environment light | ⬜ | Renderer Phase 1 |
| Wavefront queues, compaction, indirect dispatch | ⬜ | Renderer Phase 2 |
| NEE, MIS — the reference path tracer | ⬜ | Renderer Phase 3 |
| Temporal infrastructure: motion vectors, history, validation, accumulation | ⬜ | Renderer Phase 4 |
| ReSTIR DI | ⬜ | Renderer Phase 5 |
| SVGF denoising | ⬜ | Renderer Phase 6 |
| ReSTIR GI / PT | ⬜ | Renderer Phase 7 |
| `UsdPreviewSurface`, MaterialX, OpenPBR; textures | ⬜ | Renderer Phase 8 |
| RT pipeline traversal backend | ⬜ | unscheduled ([design policy §6](../design/DESIGN_POLICY.md#6-ray-tracing-backend)) |
| Debug AOVs beyond colour / depth / primId | ⬜ | [design policy §25](../design/DESIGN_POLICY.md#25-debug-and-validation) |

## Hydra adapter (`hdLotus`)

| Capability | Status | Evidence / notes |
| --- | --- | --- |
| Plugin discovery through `plugInfo.json` | ✅ | `renderer.plugin.discovery` |
| Render delegate creation | ✅ | `renderer.delegate.creation`; measured against OpenUSD 26.08 (`HD_API_VERSION` 98) |
| CPU colour / depth / primId `HdRenderBuffer`s | ✅ | `renderer.render_buffer.cpu` |
| First frame and a stable update in `testusdview` | 🧪 | `renderer.host.first_frame`, `.host.stable_update`; the bootstrap triangle at the AOV's resolution, through the Hydra camera and framing, with no Vulkan validation message |
| Supported prim types | 🧪 | `mesh` (read only to decide whether a visible mesh exists; none of it is drawn), `camera` (through the render pass state), `renderBuffer` |
| Instancers, materials, lights, render settings | ⬜ | |

## Hosts

| Host | Status | How |
| --- | --- | --- |
| Headless runner (`lotus-headless`) | ✅ | runs during `ost build`; writes `renderer-report.json` |
| Standalone viewport (`lotus-viewport`) | 🧪 | `ost renderer viewport` |
| `usdview` | 🧪 | `testusdview` in CTest; `ost renderer view` not yet run |
