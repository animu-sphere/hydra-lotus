---
status: proposed
owner: hydra-lotus
---

# Interactive quality: presets and invalidation

> Status: **proposed**, 2026-10-31. This is the contract the
> [strategy](POST_V060_STRATEGY.md#5-entry-conditions-for-later-work) requires
> before interactive-quality work changes defaults or history behaviour,
> written before any code as LOTUS-INTERACTIVE-01. It extends the
> [temporal reference](../reference/TEMPORAL.md), which owns the implemented
> behaviour; where this contract is silent, that reference holds unchanged.
> "Today" below describes `main` at v0.8.0.

## 1. Scope

Three named presets of one core, a rule for what each kind of change
invalidates, and how still-image convergence and moving-view behaviour are
measured separately.

Not in this contract: new transport, adaptive sampling, a new denoiser,
firefly clamping of the estimator, reservoir defaults, or jittered primary
metadata. Which ghosting fix to adopt, if any, is a measurement's to decide
(§7).

## 2. A preset never changes the estimator

A preset selects only how samples are kept and shown: batch size, history,
filter and when to stop. Integrator, light estimator, bounce limit, first
sample index and scene translation stay the host's, so the three presets
estimate the same image. Reservoirs stay separate: their
[reuse cost](../reports/2026-10-17-restir-reuse-cost.md) gives no preset a
reason to select them.

## 3. Presets

| | Interactive | Balanced | Reference |
| --- | --- | --- | --- |
| Purpose | Stable image while things move | Stable while moving, correct once still | The unbiased image |
| While changing | One fresh sample per frame, temporal history capped at 32, five filter iterations | As Interactive | Restart on every change |
| While still | As while changing | [Settles](#4-settling): accumulates to the sample target, the filter fading out | Accumulates to the sample target |
| History across a change | Per pixel, by [shared validity](../reference/TEMPORAL.md#shared-validity) | Same | None |
| Filter in the final image | Yes | No | No |
| Converges | Never; hosts keep drawing | At the sample target | At the sample target |
| Bias | Accepted and measured | None once settled | None |

The sample target is the host's existing one: `convergedSamplesPerPixel`
through Hydra, `--samples` in the viewport. One function in the public backend
header maps a preset to settings, and both hosts call it, so no host has its
own idea of a preset.

**Defaults do not change.** Today's defaults through Hydra and the viewport
already are Reference: progressive, unfiltered, no history. Changing a default
is a separate decision after the evidence of §7.

**Selection.**

- *Hydra.* Two bool render settings, `lotus:interactive` and `lotus:balanced`,
  because usdview lists only bool settings as menu items and the
  [integrator selection](../reports/2026-10-11-hydra-integrator-selection.md)
  showed a value in the settings dialog is cumbersome to switch. With both on,
  `lotus:interactive` wins. With either on, the preset overrides
  `lotus:temporal`, `lotus:historySamples` and `lotus:filterIterations`. With
  neither, those settings work as they do today, so existing hosts and tests
  see no change.
- *Viewport.* A `--preset` option and a developer UI control that write the
  existing `Controls`. Editing a control afterwards leaves the preset, and the
  UI shows that.

## 4. Settling

Balanced is *still* on a frame when all of these match the previous frame: the
camera, the target and its windows, the scene (no scene update) and the
transport settings. Any difference makes it *changing* again at once, and the
history cap returns to 32. Because a pixel's history weight is already
`min(previous_count, cap - n)`, a long history needs no special handling when
that happens.

Settling has to meet these requirements, whichever mechanism delivers them:

1. **Correct end.** At the sample target, the image passes the
   [statistical comparison](../reference/SCENE.md#reference-images) with
   Reference at the same sample count, over all pixels. It holds only samples
   traced while still. A history that reprojected through motion must not
   survive into the converged image, or its lag stays in it as a small bias.
2. **Every pixel converges.** Today a primary miss writes the fresh batch with
   no history, so a silhouette pixel whose centre misses but whose jittered
   samples hit would flicker forever. While still, every pixel accumulates at
   its own position, misses included.
3. **No visible pop.** Entering settling and leaving the filter must not step
   the image. The measure is frame-delta MSE (§7): no frame of the settling
   transition exceeds the moving view's mean.
4. **It stops.** The render pass reports convergence at the target and traces
   no further samples, like a progressive render today.

The expected mechanism, for LOTUS-INTERACTIVE-04 to confirm or replace: keep
the changing-state mean as a fixed prior whose weight falls to zero as still
samples arrive, and fade the filter by the same measure. The
[SVGF comparison](../reports/2026-10-18-svgf-filter.md#static-convergence)
shows why the filter must leave: in its room the temporal mean overtakes the
filtered image within about fourteen frames of a held view.

## 5. Invalidation by change category

Every change belongs to exactly one row. *Global* drops all history (one
`TemporalResetReason`). *Per pixel* keeps history where shared validity accepts
it. *None* keeps everything. Reference has no history, and its accumulation
restarts as it does today: on a change of camera, target, scene or transport
settings, and not on a change of sample target, clear colour or display.

| Change | Interactive, Balanced | `reset_reason` | Against today |
| --- | --- | --- | --- |
| Camera motion | Per pixel | `None` | Same |
| Camera cut declared by the host, render pass change or stage camera change | Global | `CameraCut` | **Changes:** the Hydra adapter cuts on `lotus:historyReset` and a render pass change, not when the pass's camera prim changes |
| Target extent, display or data window | Global | `TargetChanged` | Same |
| Rigid motion of a non-emissive placement | Per pixel, illumination approximate (§6) | `None` | Same |
| Rigid motion of an emissive placement | Global | `SceneChanged` | **Changes:** today only reservoir reuse resets; radiance history keeps the old lighting |
| Deformation of a non-emissive mesh, same topology | Per pixel: that placement's pixels are rejected, others kept, illumination approximate (§6) | `None` | **Changes:** today every pixel resets, so a skinned stage never builds any history |
| Deformation of an emissive mesh | Global | `SceneChanged` | Same |
| Mesh insertion, removal, topology, material binding, visibility, or instancing mode or count | Global | `SceneChanged` | Same |
| Material value or texture resource, used or not | Global | `SceneChanged` | Same; the unused-resource case stays conservative |
| Analytic light, environment or environment map | Global | `SceneChanged` | Same |
| Same-count instance reordering | Host cut or key change | `CameraCut`, `HistoryKeyChanged` | Same; the scene cannot identify it |
| Integrator, estimator, reservoir settings, bounces, first sample index | Global | `RenderSettingsChanged` | Same |
| Sample target | None | `None` | **Changes:** it is part of today's transport key, as a per-batch limit |
| History cap, including settling's | None; the weight formula applies the new cap | `None` | **Changes:** part of today's transport key |
| Switching between Interactive and Balanced | None | `None` | New |
| Entering or leaving Reference | Global | `RenderSettingsChanged` | New |
| Clear colour, environment background | Global | `RenderSettingsChanged` | Same; they change no sample, so this is conservative, and worth relaxing only if a host switches them often |
| Filter iterations, debug view, split comparison, validation thresholds | None | `None` | Same |

Three rules keep the table honest:

- **Automatic classification comes first.** A host declares a cut only for what
  the renderer cannot see, as the same-count reordering row does.
- **Unknown means global.** A change the classifier cannot place resets.
- **Each row is a test.** LOTUS-INTERACTIVE-03 checks each row's reason and
  history under both integrators, and through Hydra where a Hydra setting or
  edit reaches it.

The first sample after a global reset keeps
[DES-Q7's decision](DESIGN_POLICY.md#53-open-questions): it restarts at
`sample_index`.

## 6. What per-pixel history may get wrong

Per-pixel validity tests the primary surface, not its lighting. A shadow moving
across a still surface, an indirect change, or a reflection in a surface seen
from a moving camera all pass it and lag; that is what ghosting means here.
The deformation row accepts the same approximation that rigid motion already
accepts. These artifacts are accepted for Interactive and Balanced while
changing, and are limited by the cap and measured (§7). They can never reach
Balanced's converged image (§4) or Reference.

## 7. Measuring still and moving separately

`--benchmark-svgf` already reports each output's MSE against a per-pose
reference, frame-delta MSE, MSE by history length and the last frame's squared
bias. Its one room has weak view-dependent lighting and no moving shadow on a
still surface, so LOTUS-INTERACTIVE-02 adds a fixed fixture set, declared
before any fix:

| Fixture | Exposes |
| --- | --- |
| The existing `svgf-room`, static and moving | Baseline continuity and disocclusion |
| An occluder moving over a still receiver | Shadow lag: a surface validity accepts |
| A camera moving over a low-roughness surface | Reflection lag |
| A deforming mesh | The deformation row's benefit and approximation |

| Question | Metric | Held to |
| --- | --- | --- |
| Still: does it converge correctly? | Statistical comparison with Reference; last-frame squared bias | Balanced: passes. Interactive: reported |
| Still: how fast is it usable? | MSE and frame-delta MSE per frame after a change | Reported against Reference and each preset |
| Moving: how stable? | Frame-delta MSE | A fix must lower it or hold it |
| Moving: how much lag? | Squared error in a mask of the fixture's lagging region | A fix must lower it |
| Disocclusion response | MSE by history length | A fix must not raise it |
| Cost | GPU time per pass and peak memory, at [locked clocks](../reference/RESTIR.md#campaigns) | Reported; a fix's cost is part of its adoption |

**Adoption and stop rule.** LOTUS-INTERACTIVE-05 may try the candidates the
[temporal reference](../reference/TEMPORAL.md#headless-svgf-comparison) names as
absent: a radiance neighbourhood clamp, anti-lag and motion-aware reflection
weighting. A candidate is adopted only if it lowers the lag metric on its
fixture without raising any other row, and leaves Reference byte-identical. If
none does on this fixture set, record that once in a report and stop; do not
extend the set to postpone the conclusion
([strategy §6](POST_V060_STRATEGY.md#6-research-track-and-stop-rule)).

## 8. Invariants

- Reference through both integrators stays byte-identical to the committed
  references, and with no preset selected every existing setting behaves as it
  does today, apart from the rows §5 marks as changes.
- A deterministic run stays deterministic under every preset: the same settings
  and frame sequence reproduce every output bit for bit.
- The filter's output is never history
  ([temporal reference](../reference/TEMPORAL.md#edge-aware-filtering)).
- No preset creates a pipeline in the render loop.

## 9. Delivery order

| Task | Delivers |
| --- | --- |
| LOTUS-INTERACTIVE-01 | This contract, reviewed |
| LOTUS-INTERACTIVE-02 | The fixtures and a baseline of today's behaviour, with no renderer change |
| LOTUS-INTERACTIVE-03 | Interactive and Reference presets in both hosts, and §5's changes, each row tested |
| LOTUS-INTERACTIVE-04 | Balanced settling, meeting §4 |
| LOTUS-INTERACTIVE-05 | Measured ghosting candidates under §7's stop rule |

The viewport's
[intermittent device loss](../reference/SUPPORTED_CONFIGURATIONS.md#driver-limitations)
has no known application-side cause, so evidence comes from headless and Hydra
offscreen runs, and the viewport is checked as it is at release gates, with
reruns. A measured preset does not depend on resolving it.
