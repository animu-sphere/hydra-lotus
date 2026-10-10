# Capability matrix

Capability entry points and their validation evidence. Detailed technical
behavior and limitations are owned by the linked [reference pages](README.md);
the [design policy](../design/DESIGN_POLICY.md) describes intent. Phase and
release status belongs in the [canonical roadmap](../roadmap/README.md#status-at-a-glance).

Legend: ✅ implemented and tested · 🧪 scaffold only (works, but is the
generated bootstrap, not the design) · ◐ partial coverage

Configurations each row was measured on are
[SUPPORTED_CONFIGURATIONS.md](SUPPORTED_CONFIGURATIONS.md).

## Renderer foundation

| Capability | Coverage | Evidence / notes |
| --- | --- | --- |
| Host-neutral core with an enforced header boundary | ✅ | `renderer.core.boundary`; CTest `lotus-renderer-core-boundary` recursively discovers all public headers, including backend headers. `lotus-renderer-core-boundary-discovery` checks newly added nested headers against foreign dependencies |
| Core and Hydra source CI | ✅ | Registered external workflow and `openstrata.ci.yaml`; [hosted core/Hydra success and capability SKIPs](../reports/2026-10-10-foundation-closure.md#hosted-follow-up), with strict physical-GPU evidence. Tooling constraints are owned by [supported configurations](SUPPORTED_CONFIGURATIONS.md#build-and-tooling-limitations) |
| Vulkan device bring-up with validation layers, messages treated as errors | ✅ | `renderer.backend.capability`, `renderer.validation.messages` |
| Vulkan synchronization validation | ✅ | The shared instance helper enables `VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT` when the Khronos layer advertises `VK_EXT_validation_features`; offscreen and presentation statistics expose availability and an unavailable explanation. `renderer.validation.synchronization` checks zero captured messages or an explained SKIP; usdview checks enablement on every Hydra frame when headless reports it available. CTest `lotus-renderer-synchronization-validation` verifies clean synchronized writes and captures an intentionally missing barrier as a write-after-write hazard ([evidence](../reports/2026-10-08-synchronization-validation.md)) |
| Offscreen colour and depth (D32) render products, read back | ✅ | RGBA8 bootstrap and RGBA32F path-traced and barycentric diagnostic output at the requested size, with display/data windows; `renderer.render_product.color`, `.depth`, `renderer.ray_query.triangle`, `renderer.path.bsdf`, `.accumulation` |
| Foundation AOV formats and clears | ✅ | [AOV reference](AOVS.md); transparent-black colour, window depth 1, CPU ID sentinels. `renderer.aov.clears` checks full-target clears, empty scenes and preservation across frames; [measured run](../reports/2026-10-04-foundation-aovs.md) |
| Repeated frames on persistent resources | 🧪 | `Lotus::OffscreenRenderer`: the instance, device and pipeline outlive frames, and the targets are recreated only when their size changes; one frame in flight, read back each frame. 1,000 deterministic frames, another at the same size and a resize; `renderer.gpu.frame`, `renderer.frame.persistence`. The Hydra adapter keeps one renderer across frames (usdview smoke test) |
| Swapchain presentation, one frame in flight | 🧪 | `lotus-viewport`; the bootstrap triangle through the core camera, with its perspective aspect updated from the framebuffer extent |
| Slang shaders compiled to SPIR-V | ✅ | bootstrap `triangle.slang` and the scene pass `path_trace.slang`, which imports the `common/`, `bsdf/` and `sampling/` modules and explicitly enables `spvRayQueryKHR`. A shader change relinks the adapters that copy the SPIR-V |
| Camera | ✅ | view and projection in the core, converted to Vulkan clip space by the backend; all three entry points supply it. The headless runner's perspective camera is checked by `renderer.render_product.color`, `.depth`; `lotus-viewport-camera` checks square, landscape and portrait projection and scene revisions on resize ([foundation follow-up](../reports/2026-10-04-foundation-camera-boundary.md)) |
| Per-pass GPU timestamps | ✅ | persistent query pools, queue capability gated. The scene pass: `GpuFrameEvidence::primary_ray_gpu_ms` and `renderer.ray_query.timestamp` (measured on the barycentric output). A scene update's copies, BLAS builds and TLAS build or refit: `GpuSceneEvidence::timings` and `renderer.scene.timestamp` ([scene reference](SCENE.md#scene-update-timestamps), [measured run](../reports/2026-10-05-gpu-timestamps.md)). The camera and radiance passes are timed together |
| Fixed procedural benchmark suite | ✅ | Headless timing JSON, raw samples and PFM outputs; [workloads, metric scopes and unavailable metrics](SCENE.md#fixed-benchmarks), [three-run baseline](../reports/2026-10-10-performance-baseline.md). CTest validates report/workload/CLI contracts and explained missing-driver SKIPs |
| Deterministic mode and golden-image tests | ✅ | [design policy §26](../design/DESIGN_POLICY.md#26-reference--deterministic-mode): an image is a function of its first sample index, sample count, scene, camera and target. The headless runner's reference images ([scene reference](SCENE.md#reference-images)) are compared statistically, not bitwise; Hydra exposes the first sample index as `lotus:sampleIndex` ([scene reference](SCENE.md#deterministic-mode-through-hydra), [measured run](../reports/2026-10-05-hydra-deterministic-mode.md)) |

The synchronization executable probes Vulkan 1.3 driver support before layer
setup and skips an incompatible driver. CTest
`lotus-renderer-synchronization-no-driver` verifies that exit code 77 carries
an explanation even on a workstation with a GPU
([CI follow-up](../reports/2026-10-09-synchronization-ci.md)).

## Light transport

Technical contracts are owned by [SCENE.md](SCENE.md).

| Capability | Coverage | Contract / evidence |
| --- | --- | --- |
| CPU geometry and mesh placement | ✅ | [Scene snapshots](SCENE.md#geometry-and-placement); [extraction evidence](../reports/2026-10-05-cpu-mesh-extraction.md) |
| GPU scene upload | ✅ | [GPU scene](SCENE.md#gpu-scene); [readback evidence](../reports/2026-10-05-gpu-scene-upload.md) |
| GPU scene memory suballocation | ✅ | [Pools, ownership, fragmentation statistics and limits](SCENE.md#gpu-scene-memory); [allocation/reuse and lifecycle evidence](../reports/2026-10-10-gpu-memory-pools.md) |
| BLAS / TLAS | ◐ | [Acceleration structures and update limits](SCENE.md#acceleration-structures); [point refit and compaction evaluation](../reports/2026-10-10-blas-refit-compaction.md) |
| Ray queries and primary rays | ✅ | [Primary-ray contract](SCENE.md#primary-rays); [projection and capability evidence](../reports/2026-10-05-primary-rays.md) |
| Surface reconstruction, Lambert/GGX and multi-bounce transport | ✅ | [Path-tracing contract](SCENE.md#path-tracing); [BSDF evidence](../reports/2026-10-05-bsdf-multibounce.md), [dielectric/specular evidence](../reports/2026-10-08-dielectric-specular.md) |
| Environment lighting | ◐ | [Light behavior and limits](SCENE.md#lights); [dome evidence](../reports/2026-10-08-dome-lights.md) |
| HDR accumulation and float colour | ✅ | [Accumulation contract](SCENE.md#accumulation-and-the-pixel-filter); [coverage/restart evidence](../reports/2026-10-05-hdr-accumulation.md) |
| Deterministic reference images | ✅ | [Reference-image contract](SCENE.md#reference-images); [statistical comparison evidence](../reports/2026-10-05-reference-images.md) |
| Minimal material IR | ✅ | [Material contract](SCENE.md#materials-and-environment); [material-table evidence](../reports/2026-10-05-material-ir.md) |
| Texture inputs | ◐ | [Texture behavior and limits](SCENE.md#materials-and-environment); [texture evidence](../reports/2026-10-06-textures.md) |
| Authored and computed shading normals | ◐ | [Path shading](SCENE.md#path-tracing), [Hydra extraction limits](SCENE.md#hydra-extraction); [authored evidence](../reports/2026-10-06-authored-normals.md), [computed evidence](../reports/2026-10-07-computed-normals.md) |
| Tangent-space normal inputs | ◐ | [Shading frames and compatibility limits](SCENE.md#path-tracing); [normal-map evidence](../reports/2026-10-08-normal-maps.md) |
| Opacity coverage and alpha masks | ◐ | [Alpha behavior and limits](SCENE.md#materials-and-environment); [opacity evidence](../reports/2026-10-08-opacity.md) |

Future integrator and material scope is defined by the
[phase policy](../design/ROADMAP_POLICY.md#4-roadmap); its status is not
maintained in this capability index.

## Hydra adapter (`hdLotus`)

| Capability | Coverage | Evidence / notes |
| --- | --- | --- |
| Plugin discovery through `plugInfo.json` | ✅ | `renderer.plugin.discovery` |
| Render delegate creation | ✅ | `renderer.delegate.creation`; measured against OpenUSD 26.08 (`HD_API_VERSION` 98) |
| CPU colour / depth / primId `HdRenderBuffer`s | ✅ | `renderer.render_buffer.cpu`; `lotus-renderer-hydra-render-buffer` checks descriptors, formats, colour conversion, map guards and row order. Float32 colour by default, 8-bit accepted. IDs are clear sentinels only ([AOV reference](AOVS.md)) |
| AOV binding validation and empty-scene output | ✅ | `lotus-renderer-hydra-aov`: all bindings checked before rendering, host clear values, no-clear restoration across buffer switches and reallocation, disappearance of the last mesh and depth-only recovery. GPU capability-gated CTest; [foundation run](../reports/2026-10-04-foundation-aovs.md), [restoration evidence](../reports/2026-10-09-aov-restoration.md); behavior is owned by the [AOV reference](AOVS.md#clears-and-successive-frames) |
| First frame and a stable update in `testusdview` | ✅ | `renderer.host.first_frame`, `.host.stable_update`; progressive path tracing through the Hydra camera/framing to 64 spp on ray-query devices, bootstrap otherwise, with no Vulkan validation message; [measured run](../reports/2026-10-05-hdr-accumulation.md) |
| GPU capability gate for `testusdview` | ✅ | Explicit `renderer.gpu.frame` SKIP returns before launching the viewer; CTest reports the host test as skipped. Failed, missing, malformed or unexplained evidence fails. `lotus-renderer-host-capability-gate` and [CI report](../reports/2026-10-04-foundation-ci.md) |
| Supported prim types | ✅ | `mesh` (coarse geometry and ordinary or instanced placement extracted, uploaded and traced on ray-query devices), `material` (`UsdPreviewSurface` constants), `camera` (through render pass state), `domeLight` (constant environment radiance), `renderBuffer` |
| Render-pass selection | ◐ | [Collection/tag behavior and limits](SCENE.md#render-pass-selection); [selection evidence](../reports/2026-10-05-render-pass-selection.md) |
| Render settings | ✅ | [Deterministic Hydra settings](SCENE.md#deterministic-mode-through-hydra); [converged-image evidence](../reports/2026-10-05-hydra-deterministic-mode.md) |
| Instancers | ✅ | [Instancer contract and limits](SCENE.md#instancers); [placement evidence](../reports/2026-10-05-hydra-instancers.md) |
| Materials | ◐ | [Translator inputs and limits](SCENE.md#materials); [material evidence](../reports/2026-10-05-material-ir.md), [texture evidence](../reports/2026-10-06-textures.md) |
| Mesh normals | ◐ | [Hydra extraction and refinement limits](SCENE.md#hydra-extraction); [normal evidence](../reports/2026-10-07-computed-normals.md) |
| Lights | ◐ | [Light behavior and limits](SCENE.md#lights); [dome evidence](../reports/2026-10-08-dome-lights.md) |

## Hosts

| Host | Coverage | How |
| --- | --- | --- |
| Headless runner (`lotus-headless`) | ✅ | runs during `ost build`; writes `renderer-report.json` |
| Standalone viewport (`lotus-viewport`) | 🧪 | `ost renderer viewport` |
| `usdview` | 🧪 | `testusdview` in CTest; `ost renderer view` opens the smoke scene interactively ([building guide](../guides/BUILDING.md#the-hydra-adapter)) |
