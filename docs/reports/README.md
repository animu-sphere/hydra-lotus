# Reports

Dated evidence from real runs: builds, benchmarks, convergence and
correctness comparisons, and OpenUSD or OpenStrata behaviour this project had
to measure.

| Document | Contents |
| --- | --- |
| [ost/](ost/) | The `ost` dogfooding series — one report per version exercised. Append-only; the newest report carries the live upstream ask list. |
| [2026-10-04-foundation-camera-boundary.md](2026-10-04-foundation-camera-boundary.md) | Viewport camera integration and automatic public-header boundary discovery; core and viewport verification. |
| [2026-10-04-foundation-aovs.md](2026-10-04-foundation-aovs.md) | Foundation AOV formats, binding validation, full-frame clears, attachment preservation and empty-scene output; core and Hydra verification. |
| [2026-10-04-foundation-ci.md](2026-10-04-foundation-ci.md) | Renderer CI matrix and external workflow, pinned-runtime verification, local core/Hydra checks, host capability gating and OpenStrata CI limitations. |
| [2026-10-05-cpu-mesh-extraction.md](2026-10-05-cpu-mesh-extraction.md) | CPU mesh snapshots and Hydra coarse-mesh extraction, dirty updates, malformed-input recovery and multi-triangle bootstrap AOV compatibility; core 7/7, Hydra 13/13 and strict evidence validation. |
| [2026-10-05-gpu-scene-upload.md](2026-10-05-gpu-scene-upload.md) | The scene update plan and the GPU scene's geometry and instance buffers, compared with the CPU scene by readback; core 8/8, Hydra 14/14 and strict evidence validation. |
| [2026-10-05-blas-tlas.md](2026-10-05-blas-tlas.md) | A BLAS per resident geometry and a TLAS refitted or rebuilt per instance rewrite; TLAS build input and build counts checked by readback, two fault injections; core 8/8, Hydra 14/14 and strict evidence validation. |
| [2026-10-05-primary-rays.md](2026-10-05-primary-rays.md) | First ray-traced triangle: closest-hit barycentrics/depth compared with CPU projections across scene edits, clipping, framing and resize; primary-ray timestamps, Hydra silhouette checks, core 8/8, ci-core 8/8 and Hydra 14/14. |
| [2026-10-05-bsdf-multibounce.md](2026-10-05-bsdf-multibounce.md) | Lambert and GGX-metal BSDFs, emission, a constant environment, multiple bounces and Russian roulette: exact single-scattering and bounce-limited radiance, GGX albedo against independent quadrature, a closed-box mean, five fault injections; core 8/8, ci-core 8/8 and Hydra 14/14. |
| [2026-10-05-hdr-accumulation.md](2026-10-05-hdr-accumulation.md) | First physically correct image: RGBA32F accumulation, 1-pixel box filter against analytic pixel coverage, unclamped HDR, split-frame equality, restart rules and `max_samples`, float Hydra colour and progressive usdview convergence; five fault injections, core 8/8, ci-core 8/8 and Hydra 14/14. |

Renderer measurements — the frame-time and ray-count metrics of
[design policy §24](../design/DESIGN_POLICY.md#24-gpu-profiling), and
comparisons against the reference path tracer — will be reports here too, one
per measured session, once there is something to measure.

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
