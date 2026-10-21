---
status: accepted
owner: hydra-lotus
---

# Post-v0.6.0 development strategy

> Accepted 2026-10-21. This document owns development priorities and research
> stop rules. It describes intent, not implemented capabilities or measured
> performance. Release targets and phase status belong only in the
> [canonical roadmap](../roadmap/README.md#status-at-a-glance).

## 1. Decision

Hydra Lotus's primary purpose is a **practical, OpenUSD-native GPU path
tracer**, with advanced rendering research maintained as a separate track.
Build on the shared renderer core, reference and wavefront integrators,
NEE/MIS, temporal infrastructure, SVGF and standalone viewport. Improve
correct rendering of representative USD scenes, GPU efficiency and everyday
use before adding advanced transport. ReSTIR GI is not a gate for ordinary
releases.

Judge progress by whether representative USD scenes render correctly and
reliably at a measurable cost, rather than by the number of algorithms added.

## 2. Invariants

1. **Correctness → measurement → optimization → research.** Keep the
   reference path tracer and deterministic comparison conditions.
2. **One core, several entry points.** Hydra, headless and the standalone
   viewport share light transport. Interactive and offline rendering do not
   become separate renderers.
3. **Hydra is an input adapter.** Scene extraction, dirty updates and
   standard-material translation belong in `hdLotus`; the core stays free
   of Hydra and OpenUSD types.
4. **Read existing standards.** Normalize UsdPreviewSurface, OpenPBR and
   MaterialX into Lotus Material IR. Avoid new proprietary USD schemas.
5. **Compare at equal quality.** Record image error, time to equal quality,
   GPU time, peak VRAM and temporal stability together.
6. **Measure before choosing a default.** NEE/MIS remains the baseline;
   convenient experimental implementations do not automatically replace it.
7. **Keep CI and documentation bounded.** Regression belongs in ordinary
   CI; long campaigns belong in `research`. Each subject has one owner (§7).
8. **Keep repository boundaries.** OpenStrata owns build/runtime tooling,
   Merlin raster rendering and Toon stylized rendering, as defined by the
   [integration scope](INTEGRATION_SCOPE_POLICY.md).

## 3. Delivery priorities

First establish repeatable performance and production foundations: a fixed
benchmark manifest shared by headless and viewport runs, pass timings,
memory ownership, selected optimizations and representative material
fixtures. Reuse existing timestamps, the viewport HUD and headless benchmark
infrastructure. Profile before choosing an optimization.

Then expand materials and lighting according to missing behavior found in
real assets. Improve interactive quality by separating still-image
convergence from moving-camera behavior, reducing disocclusion artifacts
and ghosting, and defining resets for camera, geometry, material, light,
resize and integration-setting changes. Interactive (low spp plus SVGF),
Balanced (progressive convergence) and Reference (high spp without filtering)
are intended presets of the same core; SVGF need not always be enabled.

Only the next delivery milestone receives a detailed task breakdown. The
following milestone keeps a direction and entry conditions; later ones are
hypotheses. Dates are not fixed, and exit evidence takes priority over a
version number. The [roadmap](../roadmap/README.md) owns milestone names and
release assignments; [current.md](../roadmap/current.md) owns the work queue.

Renderer phase numbers retain their meaning. A selected production-material
or scheduling task can precede advanced reservoir research under the
[partial-delivery rule](ROADMAP_POLICY.md#cross-phase-delivery).
Delivering that subset does not complete its parent phase.

## 4. Measurement and adoption

The [workstream contracts](ROADMAP_POLICY.md#practical-foundation-workstreams)
define scope and exit evidence. Apply these decision rules:

- Establish the baseline and explain the largest costs first. Adopt only
  optimizations with measured benefit and no reference-quality regression;
  do not promise an unmeasured speedup factor.
- Treat FP16, compact guides, lifetime-based buffer reuse/aliasing and
  AoS/SoA layouts as candidates. Reject bandwidth or precision regressions.
  Preserve history/rejection semantics and CPU-oracle agreement.
- Expand the existing material contract using representative USD fixtures,
  reference images and Hydra/headless agreement. Document unsupported
  parameters. Full OpenPBR, arbitrary MaterialX graphs and subsurface
  scattering are not prerequisites. Do not redesign the IR for spectral
  rendering before a spectral contract exists.
- Separate **light intersection** (every ray testing analytic lights) from
  **light selection** (choosing a light for NEE/MIS). A light tree for
  selection does not by itself accelerate every intersection test. Permit
  a small acceleration experiment only when many-light profiling shows a
  dominant cost. Measure low-light-count regressions before adoption.

## 5. Entry conditions for later work

Production-material expansion starts from asset gaps demonstrated by the
foundation fixtures. Candidates include dielectric/metal GGX, IOR,
transmission, coat, sheen and representative MaterialX node translation.
Existing support is defined by the [scene reference](../reference/SCENE.md#materials-and-environment),
not this list. Choose subsurface scattering, arbitrary graphs and additional
UsdLux options separately on cost and benefit.

Lighting adoption requires a fixed many-light comparison covering GPU time,
quality, memory and low-light-count behavior. Interactive-quality work
requires an explicit preset and invalidation contract before changing
defaults or history behavior.

## 6. Research track and stop rule

ReSTIR GI, path guiding, neural denoising and spectral rendering do not block
ordinary releases. Before starting a topic, write a short experiment brief
in [reports](../reports/README.md): hypothesis, baseline, bounded fixture
set, hardware and clock conditions, error metric, GPU time and time to equal
quality, memory cost, evaluation budget and adopt/defer/stop criteria.

- **ReSTIR GI:** start with a few fixtures and a short comparison. Temporal
  and spatial reuse are candidates; keep the mode experimental if their
  costs exceed their gains.
- **Path guiding:** focus on difficult indirect lighting. Include training
  warmup and guide memory in the equal-quality comparison.
- **Neural denoising:** retain SVGF. Compare OIDN, ONNX or other candidates
  as optional dependencies, including GPU/CPU transfer in total time.
- **Spectral:** start as an independent mode that preserves ordinary RGB
  rendering. Define material, light and sampling contracts before broad
  structural changes.

**Stop rule:** if the predeclared comparison set shows no advantage, do not
extend fixtures and metrics repeatedly to postpone the conclusion. Record
the decision once in a report and keep the campaign out of ordinary CI.
Respect the existing [ReSTIR DI stop rule](ROADMAP_POLICY.md#renderer-phase-5--restir-di);
do not reopen that investigation without new evidence.

The [test policy](TEST_POLICY.md) owns `fast` / `gpu` / `research` execution
and test budgets. Start a new contract with a small correctness case, the
necessary GPU smoke coverage and a research report when needed; do not make
a long combination matrix the default.

## 7. Documentation ownership

Keep this strategy's priorities and stop rule here, and link to them.
Do not copy the strategy into other documents.

| Owner | Subject |
| --- | --- |
| [ROADMAP_POLICY.md](ROADMAP_POLICY.md) | Phase application, partial-delivery rules, scope and exit criteria. |
| [roadmap/README.md](../roadmap/README.md#status-at-a-glance) | Phase/release status and targets. |
| [roadmap/current.md](../roadmap/current.md) | The sole active checklist and next actions. |
| [reference/](../reference/README.md) | Implemented contracts, support and limitations. |
| [reports/](../reports/README.md) | Environments, measurements, comparisons, conclusions and reproduction. |
| [releases/](../releases/README.md) and [CHANGELOG](../../CHANGELOG.md) | Shipped scope and history. |

Before removing completed tasks from the active queue, verify that their
reports and release records remain reachable. Preserve historical citations
through link repairs; use the [archive](../archive/README.md) for superseded
plans. Only implementation evidence updates the capability matrix.
