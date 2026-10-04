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
| Offscreen colour (RGBA8) and depth (D32) render products, read back | 🧪 | a fixed 64×64 rasterized bootstrap triangle; `renderer.render_product.color`, `.depth` |
| Repeated frames on persistent resources | 🧪 | 1,000 deterministic frames; `renderer.gpu.frame`, `renderer.frame.persistence` |
| Swapchain presentation, one frame in flight | 🧪 | `lotus-viewport`; the bootstrap triangle |
| Slang shaders compiled to SPIR-V | 🧪 | `backend/vulkan/shaders/triangle.slang` only |
| Camera | ⬜ | the Hydra camera is accepted but not used for projection; Renderer Phase 0 |
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
| First frame and a stable update in `testusdview` | 🧪 | `renderer.host.first_frame`, `.host.stable_update`; the bootstrap triangle, upscaled from 64×64 |
| Supported prim types | 🧪 | `mesh` (read only to decide whether a visible mesh exists; none of it is drawn), `camera`, `renderBuffer` |
| Instancers, materials, lights, render settings | ⬜ | |

## Hosts

| Host | Status | How |
| --- | --- | --- |
| Headless runner (`lotus-headless`) | ✅ | runs during `ost build`; writes `renderer-report.json` |
| Standalone viewport (`lotus-viewport`) | 🧪 | `ost renderer viewport` |
| `usdview` | 🧪 | `testusdview` in CTest; `ost renderer view` not yet run |
