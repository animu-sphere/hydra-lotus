# Practical foundation closure: LOTUS-PERF-01 and LOTUS-MEMORY-01

Reviewed 2026-10-28 against `1f22d4c`. This review adds no measurement and
changes no renderer, shader or harness code. It closes LOTUS-PERF-01 and
LOTUS-MEMORY-01, workstreams A and B of the
[practical foundation](../design/ROADMAP_POLICY.md#practical-foundation-workstreams),
from the evidence of their slices. Workstream C closed with its
[own review](2026-10-28-material-foundation-closure.md), and workstream D
with the [crossing hierarchy](2026-10-25-light-crossing-hierarchy.md#decision).
Each remaining item gets one disposition, as
[foundation closure](../contributing/documentation.md#status-and-evidence)
requires: implemented, deferred to a named phase, or an accepted limitation
with its rationale. Closing these tasks completes the v0.7.0 delivery
scope; it does not complete Renderer Phase 8 or 9.

## Decision

Both tasks are closed. The
[shared static baseline](2026-10-21-shared-performance-workloads.md#leading-costs-and-decisions)
named three leading costs and deferred each until it was attributed. All
three were then attributed and fixed with before/after measurements:

| Leading cost | Attribution | Adopted fix | Effect |
| --- | --- | --- | --- |
| Blocking frame overhead | [Host phases](2026-10-25-frame-host-split.md): readback from uncached host memory | Cached readback memory | About 1.1 ms less per 128x128 frame, 21.6 ms at 512x512, 24–34 ms per ReSTIR reuse frame; identical images and statistics |
| Many-light continuation | [Crossing versus selection](2026-10-25-light-crossing-cost.md#decisions): crossing tests 81–88% of a 256-light frame | [Crossing hierarchy](2026-10-25-light-crossing-hierarchy.md#decision) | 256-light scene pass 5.3–8.0x faster under both integrators; images equal to rounding; no low-light-count regression beyond process spread |
| Geometry update path | [Motion workloads](2026-10-25-motion-workloads.md#decisions): deformation restages unchanged triangle indices | [Changed-section staging](2026-10-25-changed-section-staging.md#decision) | 67% fewer staged bytes, about 20% less update wall on the large mesh; identical images, refits and read-back contents |

The [residency inventory](2026-10-26-frame-residency.md#candidates-and-decisions)
did the same for memory, and its two leading candidates were adopted:
[lazy readback mirrors](2026-10-26-lazy-readback-mirrors.md#decision)
(6.83 GiB less host-visible memory at 4K with SVGF and both reuse passes)
and the [reservoir history split](2026-10-26-reservoir-history-split.md#decision)
(1.0 GiB less device memory at 4K with spatial reuse). Every adopted change
left images byte-identical or equal to rounding, so each speedup is also an
equal-quality speedup. The open items below are extensions of the baseline
or candidates whose expected saving is smaller than what was adopted. None
of them is needed to explain a leading cost.

## Workstream A exit evidence

| Exit evidence | Delivered |
| --- | --- |
| Reproducible baseline and environment | Versioned shared fixtures (Cornell box, textures, instances, geometry, analytic lights, environment, 256 sphere lights) and a manifest runner for headless and viewport runs, recording build, device, driver, resolution, spp, seed, warmup and run count, and keeping failed attempts ([baseline](2026-10-21-shared-performance-workloads.md)); optional clock, power-state and device-memory telemetry ([telemetry](2026-10-24-gpu-telemetry.md)) |
| Pass-cost breakdown | Per-pass timestamps for the wavefront kernels, temporal, variance, filter and ReSTIR passes; blocking host phases ([host split](2026-10-25-frame-host-split.md)); scene-update upload, BLAS and TLAS phases under motion ([motion](2026-10-25-motion-workloads.md)); crossing separated from selection ([crossing cost](2026-10-25-light-crossing-cost.md)). The Reference integrator's single pass remains one timestamp, as the baseline states |
| Top three bottlenecks explained | The table above |
| Adoption/rejection decisions | Each report ends in one; the baseline's deferrals are all resolved above |
| Before/after for adopted optimizations | Each adopting report compares the same procedure before and after, with images and counters |
| No reference-quality regression | Images identical or equal to rounding for every adopted change; Cornell's statistical reference check and the [quality checkpoints](2026-10-25-quality-curve.md), [populations](2026-10-25-quality-populations.md) and [textured curves](2026-10-25-textured-quality.md) against finite references; the [v0.7.0 gate](2026-10-28-v0.7.0-release-gate.md) reruns every committed reference |
| Time to equal quality, image error, peak VRAM, CPU update time | Observed error-threshold times for both integrators on the Cornell and textured fixtures; per-frame authoring, extraction and update wall under motion; process heap usage from the residency benchmark (see workstream B) |

## Workstream B exit evidence

| Exit evidence | Delivered |
| --- | --- |
| 1080p/4K estimates and measured residency | Every renderer allocation by owner and memory kind with `VK_EXT_memory_budget` process heap usage at 720p, 1080p and 4K in every progressive, temporal, SVGF and ReSTIR mode; the renderer's groups explain all frame-sized growth ([inventory](2026-10-26-frame-residency.md)) |
| Unavailable measurements reported | Continuous peaks within a call, swapchain memory outside the heap total, other devices ([inventory](2026-10-26-frame-residency.md#unavailable-and-unmeasured)) |
| History/rejection meaning, CPU oracles, dynamic scenes | Both adopted changes keep every product byte-identical, under both integrators, including the moving glossy-disocclusion workload; the temporal and ReSTIR CPU oracles pass unchanged |
| Before/after memory, time and precision | Lazy mirrors: host memory by mode and size, unchanged device memory and products. History split: device memory by mode and size, per-pass GPU time at 1080p and 4K, identical images |

## Deferred and accepted items

| Item | Source | Disposition |
| --- | --- | --- |
| Controlled (locked-clock) timing | [Baseline](2026-10-21-shared-performance-workloads.md#leading-costs-and-decisions), every PERF-01 report | Accepted limitation: no adopted decision rests on a small timing difference. Readback and staging gains are memory-type and byte effects, the hierarchy's 5–8x exceeds the per-process spread, and memory results do not depend on clocks. A later comparison of a small effect locks clocks, as the [ReSTIR reuse cost](2026-10-17-restir-reuse-cost.md) did |
| Hydra and viewport motion | [Motion limits](2026-10-25-motion-workloads.md#limits-and-remaining-evidence) | Accepted limitation: both frontends call the measured backend update path. Hydra dirty-bit translation and USD time samples are unmeasured; the viewport has no motion mode |
| Temporal/SVGF modes in the shared manifest | [Baseline](2026-10-21-shared-performance-workloads.md#unavailable-metrics-and-remaining-evidence) | Implemented outside the manifest: the residency benchmark times every temporal, SVGF and ReSTIR mode's passes at 1080p and 4K, and the [Renderer Phase 6 filter comparison](2026-10-18-svgf-filter.md) measures their quality, static and moving. Moving quality curves on the shared fixtures are not built |
| Quality curves for the other fixtures | [Textured curves](2026-10-25-textured-quality.md#decision-and-remaining-evidence) | Deferred to Renderer Phase 9: no adopted change altered an estimator, so equal quality followed from identical images. A sampling change (light selection, adaptive sampling) needs finite references for the fixtures it affects, `many-lights-v1` first |
| Continuous process peak residency | [Inventory](2026-10-26-frame-residency.md#unavailable-and-unmeasured) | Accepted limitation: residency is sampled between calls; transient allocations live in persistent pools, so a sampled total misses only validation readback |
| Complete three-repeat viewport baseline | [Viewport failure](2026-10-21-shared-performance-workloads.md#viewport-evidence-and-failure) | Accepted limitation: the [known driver fault](../reference/SUPPORTED_CONFIGURATIONS.md#driver-limitations) loses the device at frame 2 intermittently; the runner keeps the failure rather than retrying it |
| Blocking update floor, instancer rewrites, frames in flight | [Motion](2026-10-25-motion-workloads.md#decisions), [host split](2026-10-25-frame-host-split.md#decisions) | Accepted limitation: each is below 0.5 ms per frame at the measured sizes, after the adopted fixes |
| Host work of a large deformation | [Staging](2026-10-25-changed-section-staging.md#decision) | Accepted limitation: deforming `geometry-v1` still spends 2.1–2.4 ms per frame in a blocking update whose GPU phases take 0.59 ms; `UpdateScene` does not time its comparisons, staging copy, submission and wait separately. It is the only update above 1 ms, and an optimization task for it starts by splitting those phases |
| Light selection (light tree, importance distribution) | [Crossing cost](2026-10-25-light-crossing-cost.md#decisions) | Deferred to Renderer Phase 9: selection cost is constant per shading point; a tree would be adopted for variance, which needs fixtures with spatially varied light power |
| Reservoir surface value compaction | [History split](2026-10-26-reservoir-history-split.md#remaining-candidates) | Deferred to Renderer Phase 7, which owns reservoir representation: half precision exceeds the oracles' tolerance and changes images, so it needs a precision contract first |
| Lifetime aliasing of within-frame buffers | [Inventory](2026-10-26-frame-residency.md#candidates-and-decisions) | Accepted limitation: its saving is below the adopted changes', and it needs a barrier and pass-order analysis |
| Temporal surface compaction | [Inventory](2026-10-26-frame-residency.md#candidates-and-decisions) | Accepted limitation: the two 64-byte records carry the shared validity contract of Renderer Phase 4 and its CPU oracle |
| Releasing buffers on a mode switch | [Inventory](2026-10-26-frame-residency.md#candidates-and-decisions) | Accepted limitation: disabled modes keep their buffers until the renderer is destroyed; matters only for long viewport sessions that switch modes |

## Not covered

- Only one device, the NVIDIA RTX A5000 with driver 597.16, was measured;
  other GPUs, drivers and memory architectures are unmeasured.
- 1080p/4K timings exist for the temporal, SVGF and ReSTIR modes only; the
  progressive shared workloads ran at 128 and 512 pixels.
- The adopting reports measured headless; the
  [v0.7.0 gate](2026-10-28-v0.7.0-release-gate.md) rebuilds and reruns the
  Hydra and viewport trees on the same backend.
