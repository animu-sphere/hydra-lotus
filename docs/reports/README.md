# Reports

Dated evidence from real runs: builds, benchmarks, convergence and
correctness comparisons, and OpenUSD or OpenStrata behaviour this project had
to measure.

| Document | Contents |
| --- | --- |
| [2026-10-09-aov-restoration.md](2026-10-09-aov-restoration.md) | No-clear AOV restoration across buffer switches, host writes and reallocation; GPU staging reuse, hit/miss preservation and regression checks. |
| [2026-10-09-synchronization-ci.md](2026-10-09-synchronization-ci.md) | Hosted Hydra CI failed on missing Vulkan driver support; explicit driver probing and a child-process no-driver regression test. |
| [2026-10-08-synchronization-validation.md](2026-10-08-synchronization-validation.md) | Explicit synchronization validation in the shared Vulkan instance setup, clean writes and an intentionally missing barrier, per-frame Hydra enablement and explained unavailable evidence. |
| [2026-10-08-dome-lights.md](2026-10-08-dome-lights.md) | Constant Hydra dome lighting: composed USD colour/intensity/exposure and visibility edits, additive domes and removal, analytic mirror radiance, accumulation restarts and unchanged geometry/build counters. |
| [ost/](ost/) | The `ost` dogfooding series — one report per version exercised. Append-only; the newest report carries the live upstream ask list. |
| [2026-10-04-foundation-camera-boundary.md](2026-10-04-foundation-camera-boundary.md) | Viewport camera integration and automatic public-header boundary discovery; core and viewport verification. |
| [2026-10-04-foundation-aovs.md](2026-10-04-foundation-aovs.md) | Foundation AOV formats, binding validation, full-frame clears, attachment preservation and empty-scene output; core and Hydra verification. |
| [2026-10-04-foundation-ci.md](2026-10-04-foundation-ci.md) | Renderer CI matrix and external workflow, pinned-runtime verification, local core/Hydra checks, host capability gating and OpenStrata CI limitations. |
| [2026-10-05-cpu-mesh-extraction.md](2026-10-05-cpu-mesh-extraction.md) | CPU mesh snapshots and Hydra coarse-mesh extraction, dirty updates, malformed-input recovery and multi-triangle bootstrap AOV compatibility. |
| [2026-10-05-gpu-scene-upload.md](2026-10-05-gpu-scene-upload.md) | The scene update plan and the GPU scene's geometry and instance buffers, compared with the CPU scene by readback. |
| [2026-10-05-blas-tlas.md](2026-10-05-blas-tlas.md) | A BLAS per resident geometry and a TLAS refitted or rebuilt per instance rewrite; TLAS build input and build counts checked by readback, two fault injections. |
| [2026-10-05-primary-rays.md](2026-10-05-primary-rays.md) | First ray-traced triangle: closest-hit barycentrics/depth compared with CPU projections across scene edits, clipping, framing and resize; primary-ray timestamps, Hydra silhouette checks. |
| [2026-10-05-bsdf-multibounce.md](2026-10-05-bsdf-multibounce.md) | Lambert and GGX-metal BSDFs, emission, a constant environment, multiple bounces and Russian roulette: exact single-scattering and bounce-limited radiance, GGX albedo against independent quadrature, a closed-box mean, five fault injections. |
| [2026-10-05-hdr-accumulation.md](2026-10-05-hdr-accumulation.md) | First physically correct image: RGBA32F accumulation, 1-pixel box filter against analytic pixel coverage, unclamped HDR, split-frame equality, restart rules and `max_samples`, float Hydra colour and progressive usdview convergence; five fault injections. |
| [2026-10-05-reference-images.md](2026-10-05-reference-images.md) | Reference path tracer: a Cornell box at 1–1024 spp compared with a committed 1024-spp mean and per-pixel variance (DES-Q5), seven independent sample sequences, the metric's power against a brighter wall, five fault injections. |
| [2026-10-05-hydra-instancers.md](2026-10-05-hydra-instancers.md) | Hydra instancer placements: point, nested and native instancing synced through UsdImaging and compared with UsdGeom's transforms, GPU instance and TLAS counts, primary rays through instanced triangles, four fault injections. |
| [2026-10-05-render-pass-selection.md](2026-10-05-render-pass-selection.md) | Render-pass selection by collection root and exclude paths and render tags, through UsdImaging purposes, with excluded meshes hidden over resident geometry; four fault injections, one not detectable on OpenUSD 26.08. |
| [2026-10-05-hydra-deterministic-mode.md](2026-10-05-hydra-deterministic-mode.md) | Deterministic mode through Hydra: the `lotus:sampleIndex` render setting, converged Hydra images compared bit for bit with the backend's across setting changes, camera moves and render delegates, and in usdview; three fault injections. |
| [2026-10-05-material-ir.md](2026-10-05-material-ir.md) | Renderer Phase 1.5 material IR: keyed materials and bindings, the planned material table and its GPU buffer, `UsdPreviewSurface` constants through `HdMaterial`, the Cornell reference reproduced bit for bit, exact Lambert radiance through Hydra; four fault injections. |
| [2026-10-06-authored-normals.md](2026-10-06-authored-normals.md) | Renderer Phase 1.5 authored normals: per-corner normals through the GPU scene, the `ShadingNormal` diagnostic against an independent oracle, sky-view-factor Lambert and mirror radiance, every Hydra interpolation through UsdImaging, the Cornell reference reproduced bit for bit; six fault injections. |
| [2026-10-06-textures.md](2026-10-06-textures.md) | Renderer Phase 1.5 textures: texture-coordinate sets and a bindless texture table, the `Albedo` and `RoughnessMetallic` diagnostics against an independent bilinear oracle, exact textured radiance, `UsdUVTexture` and Hio-decoded images through UsdImaging, the device's sRGB decoding precision, the Cornell reference reproduced bit for bit; eight fault injections. |
| [2026-10-07-computed-normals.md](2026-10-07-computed-normals.md) | Renderer Phase 1.5 computed coarse smooth normals: Storm's `hd` utilities through UsdImaging, compared with an independent double-precision polygon-corner oracle; scheme, point, topology, orientation, hole and authored-normal edits, degenerate geometry and invalid-point recovery. |
| [2026-10-08-normal-maps.md](2026-10-08-normal-maps.md) | Renderer Phase 1.5 signed tangent normal inputs and normal maps: per-hit UV frames compared with an independent oracle, exact mirror radiance, mirrored UVs and transforms, missing and degenerate UVs, Hydra image/UV/connection edits, two fault injections and bit-identical reference regression. |
| [2026-10-08-opacity.md](2026-10-08-opacity.md) | Renderer Phase 1.5 opacity coverage and alpha masks: independent bilinear mask/depth oracle, analytic primary/secondary layer mixtures, background alpha and deterministic repetition, Hydra image/threshold/connection edits, two fault injections and bit-identical reference regression. |
| [2026-10-08-dielectric-specular.md](2026-10-08-dielectric-specular.md) | Renderer Phase 1.5 dielectric GGX and specular workflow: independent furnace integrals, specular images and input edits, Hydra coat/connection edits, two fault injections and regenerated Cornell references. |
| [2026-10-05-gpu-timestamps.md](2026-10-05-gpu-timestamps.md) | Scene-update GPU timestamps: copies, BLAS builds and TLAS build or refit timed per submission on an instanced 131,072-triangle grid, zero for skipped phases, two fault injections; `ost renderer view` run. |

Renderer measurements — the frame-time and ray-count metrics of
[design policy §24](../design/DESIGN_POLICY.md#24-gpu-profiling), and
comparisons against the reference path tracer — belong here, one report per
measured session. The benchmark baseline requirements and execution order are
owned by [current.md](../roadmap/current.md#performance-baseline-and-foundation-release).
Numbers stay in the dated report; other categories link to the measurement.

## What belongs where

A report captures *how* something was validated, on a specific machine, at a
specific time. It is working history, not a current-state contract:

- Current structure belongs in [architecture/](../architecture/), and current
  capability in [reference/](../reference/).
- Design rationale belongs in [design/](../design/).
- Incomplete work belongs in the [roadmap](../roadmap/).
- Shipped scope belongs in the [changelog](../../CHANGELOG.md), with
  per-version detail in [releases/](../releases/).

When a report disagrees with a current-state document, the current-state
document wins and the report is history.
