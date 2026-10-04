# Reports

Dated evidence from real runs: builds, benchmarks, convergence and
correctness comparisons, and OpenUSD or OpenStrata behaviour this project had
to measure.

| Document | Contents |
| --- | --- |
| [ost/](ost/) | The `ost` dogfooding series — one report per version exercised. Append-only; the newest report carries the live upstream ask list. |
| [2026-10-04-foundation-camera-boundary.md](2026-10-04-foundation-camera-boundary.md) | Viewport camera integration and automatic public-header boundary discovery; core and viewport verification. |

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
