# Interactive quality: presets and invalidation

Checked 2026-11-08 on Windows 11, MSVC 14.51 Release, NVIDIA RTX A5000
(driver 597.16), clocks not locked. Source base: main `69c24ba` plus the
LOTUS-INTERACTIVE-03 change. The
[interactive-quality contract](../design/INTERACTIVE_QUALITY.md) owns the
presets and the invalidation table; the
[temporal reference](../reference/TEMPORAL.md) owns the implemented
behaviour; the [roadmap](../roadmap/README.md#status-at-a-glance) owns phase
status.

## Question

LOTUS-INTERACTIVE-03 adds the Interactive and Reference
[presets](../design/INTERACTIVE_QUALITY.md#3-presets) to both hosts and makes
the [invalidation table's](../design/INTERACTIVE_QUALITY.md#5-invalidation-by-change-category)
changes, with a test for each row. Balanced is LOTUS-INTERACTIVE-04's.

## What changed

**One preset function.** `ApplyQualityPreset` in
`include/lotus/temporal.hpp` maps a `QualityPreset` to the three settings a
preset owns: Interactive turns on radiance accumulation with a history cap
of 32 and five filter iterations; Reference turns accumulation and the filter
off. It touches nothing else, so a preset never changes the estimator. It is
inline because the viewport's controls are built and tested without the
backend.

**Hydra.** `lotus:interactive` (bool, false) applies the Interactive preset
over `lotus:temporal`, `lotus:historySamples` and `lotus:filterIterations`.
With it off those settings work as before, and their defaults are Reference.
A render pass state that looks through a different camera prim now cuts the
history, as `lotus:historyReset` and a render pass change already did.
`lotus:balanced` is not added yet: it arrives with settling.

**Viewport.** `--preset interactive|reference` and a Quality list in the
Renderer window apply a preset to the controls through the same function;
options after `--preset` edit it. The list shows the preset the controls are
in, and Custom once a control the preset sets has been edited. Reference also
turns accumulation back on, since it is the progressive image.

**Backend invalidation.**

| Row | Before | Now |
| --- | --- | --- |
| Rigid motion of an emitter | Radiance history kept per pixel | `SceneChanged` |
| Deformation of a non-emitter, same topology | `SceneChanged` | Per pixel: the deformed placement's pixels are rejected, the others kept |
| Sample target (`max_samples`) | `RenderSettingsChanged` | No reset |
| History cap | `RenderSettingsChanged` | No reset; the new cap bounds the kept samples |
| First temporal frame after ordinary (progressive) frames | `ExplicitReset` | `RenderSettingsChanged` |

An *emitter* is a visible mesh whose bound material has a nonzero emission
value or an emission texture; this is conservative where a texture or its
fallback emits nothing. *Same topology* is the same point count, triangles
and source faces; normals, texture coordinates and colours may change with
the points. A non-emitter's deformation already lost its placement's identity
and rigid correspondence, so validity rejects exactly its pixels; only the
scene classification had to accept the edit. Reservoir reuse is unchanged: it
still resets on any scene edit while emissive triangles exist, and now keeps
per-pixel reservoirs through a non-emitter's deformation when there are none.

The last row makes the contract's "entering or leaving Reference" reason
exact. Inside the backend only an ordinary frame resets the history
(`TemporalHistory::Reset`), so its `ExplicitReset` always meant that the
stream had been progressive. The CPU `TemporalHistory` still reports
`ExplicitReset` after a caller's `Reset()`.

## Each row's test

Both GPU tests run every row under the reference and wavefront integrators.
"temporal-gpu" is `lotus-renderer-temporal-gpu`; "Hydra" is
`lotus-renderer-hydra-temporal-gpu`.

| Row | Checked by |
| --- | --- |
| Camera motion | temporal-gpu (motion against 256-sample references); Hydra (camera prim moved) |
| Host cut, render pass change, stage camera change | temporal-gpu (`camera_cut`); Hydra (`lotus:historyReset`, a new pass, **a second camera prim and back**) |
| Target extent, display or data window | temporal-gpu |
| Non-emitter rigid motion | temporal-gpu; Hydra (panel moved) |
| **Emitter rigid motion** | temporal-gpu (the emissive plane moved); `lotus-renderer-restir-gpu` (emitter moved with reservoirs and radiance) |
| **Non-emitter deformation** | temporal-gpu (a panel in front of the plane deformed: reason `None`, every panel pixel rejected, plane pixels kept); Hydra (the panel's `points` edited) |
| Emitter deformation | temporal-gpu |
| Insertion, removal, **topology**, binding, visibility, **instancing mode and count** | temporal-gpu |
| Material value, unused texture | temporal-gpu |
| Analytic light, environment | temporal-gpu |
| Same-count instance reordering | A host cut or key change: temporal-gpu (`camera_cut`, `history_key`); Hydra (`lotus:historyKey`) |
| Integrator, estimator, bounces, first sample index | temporal-gpu; Hydra (integrator) |
| **Sample target** | temporal-gpu (`max_samples`); Hydra (`convergedSamplesPerPixel`) |
| **History cap** | temporal-gpu (every pixel's count against the new cap); Hydra (`lotus:historySamples`) |
| Switching between Interactive and Balanced | LOTUS-INTERACTIVE-04 |
| **Entering or leaving Reference** | temporal-gpu (metadata-only and ordinary frames); Hydra (`lotus:interactive` off and on, with and without debug AOVs) |
| Clear colour, **environment background** | temporal-gpu |
| Filter iterations | temporal-gpu (alternated every frame); Hydra (`lotus:filterIterations`, and `lotus:interactive` over a different value) |

Rows in bold gained a check in this task. The debug view and the split
comparison are viewport-only presentation; they never reach the renderer's
history. `lotus-renderer-temporal` checks `ApplyQualityPreset` without a GPU,
and `lotus-viewport-controls` and `lotus-viewport-options` check the
viewport's preset, its override by later edits, Custom, and that a preset
keeps the estimator, the sample limit and the view.

With the Reference preset selected through Hydra and temporal debug AOVs
bound, the frame is a metadata-only temporal frame and reports `None`: it
keeps no radiance, so nothing is reused, and entering Interactive afterwards
reports `RenderSettingsChanged`.

## Effect on the fixed fixtures

`lotus-headless --benchmark-svgf` with the
[baseline's](2026-11-08-interactive-baseline.md#procedure) 32-frame
procedure, reference integrator, against the baseline's report:

- `svgf-room` static and moving, the shadow sweep and the glossy pan: every
  output's MSE, frame-delta MSE and lag-region MSE is identical. None moves
  an emitter or deforms a mesh.
- `deforming-sheet-v1`: no resets after the cut (248 `scene-changed` in the
  baseline). The room around the sheet now keeps its history:

| Output | MSE | Frame-delta MSE | Sheet (lag region) MSE |
| --- | --- | --- | --- |
| raw | 0.3628 → 0.3525 | 0.0103 → 0.7030 | 0.0594 → 0.0741 |
| temporal | 0.3628 → 0.0513 | 0.0103 → 0.0229 | 0.0594 → 0.0741 |
| filtered | 0.0885 → 0.0314 | 0.00059 → 0.0018 | 0.0029 → 0.0043 |
| progressive | 0.3628 (unchanged) | 0.0103 | 0.0594 |

The temporal mean's error falls sevenfold. The frame-delta MSE rises because
the baseline's per-frame reset restarted every frame at the same first sample
index, so its noise was frozen
([DES-Q7](../design/DESIGN_POLICY.md#53-open-questions)): stable but wrong.
The sheet's own pixels are rejected every frame, as the table requires, so
there the temporal mean is the raw sample; its noise now changes each frame
instead of repeating. The run was deterministic, as before.

## Regression

Built in the research, Hydra (`lookdev--hydra`) and core viewport trees, after
deleting the objects Ninja held without dependencies.

- `lotus-renderer-temporal`, `-temporal-gpu`, `-restir-gpu`,
  `lotus-viewport-controls`, `lotus-viewport-options` (research tree).
- `lotus-renderer-hydra-temporal-contract`, `-temporal-gpu`,
  `-restir-contract`, `-restir-gpu`, `-integrator`,
  `-material-fixtures-gpu`, `-skel-gpu` (Hydra tree).
- `lotus-renderer-restir-benchmark-reference-animation` and
  `lotus-renderer-svgf-benchmark-reference`.
- `lotus-viewport --preset interactive --frames 30 --hidden`: 30 frames
  presented, controls reported as temporal radiance with the denoiser at five
  iterations.

Reference stays byte-identical: each tree's build-time headless report
passes `renderer.path.reference` (the committed reference images reproduced
bit for bit) and `renderer.path.wavefront` (the wavefront integrator equal to
the reference integrator), 30 assertions pass and the six installed-host
assertions skip, as they do outside an install. The material fixtures pass
against their committed references through Hydra. The progressive path's
code is unchanged.

## Limits

- Not rerun: the full CTest suites, the ci-core and USD viewport trees, the
  wavefront SVGF benchmark and the viewport's Hydra path.
- The emitter test is by material, not by measured contribution: an emitter
  hidden behind everything still resets globally when it moves.
- A host that switches the camera prim to one with an identical pose still
  cuts. usdview hands its free camera to Hydra as a camera prim of its own,
  so switching from it to a stage camera should cut too; that was not
  checked in usdview.
