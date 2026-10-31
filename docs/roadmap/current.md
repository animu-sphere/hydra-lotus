# Current

The only active task checklist. Phase/release status and targets belong in
[the canonical table](README.md#status-at-a-glance); priorities are defined
by the [development strategy](../design/POST_V060_STRATEGY.md).

## Execution order

Work on the [Materials & Lighting milestone](README.md#v080--materials--lighting).
The [asset gap survey](../reports/2026-10-29-asset-gap-survey.md#decision)
rendered 18 third-party USD stages through Hydra. It found four material
gaps and dropped skinned meshes, each with a visible effect; no asset
demonstrated a lighting gap. LOTUS-MTLX-01
[translated MaterialX Standard Surface](../reports/2026-10-29-materialx-standard-surface.md#decision),
closing the first gap, and LOTUS-TRANSMISSION-01 added
[dielectric transmission](../reports/2026-10-29-dielectric-transmission.md#decision)
under its [contract](../design/TRANSMISSION.md), so OpenChessSet's pawn
tops and the shader ball's glass render as tinted glass. LOTUS-MATERIAL-02
[translated texture transforms, per-face materials, colour primvars and
display coverage](../reports/2026-10-31-texture-transforms-subsets-primvars.md#decision),
and LOTUS-SKEL-01 [rendered skinned meshes at their posed
points](../reports/2026-10-31-skinned-meshes.md#decision). Every selected task is done and no
task is active; the milestone's release gate is next.

The survey also found that `usdrecord --renderer Storm` creates the Lotus
renderer when `hdLotus` is on the plugin path. Its cause is unexplained and
outside these tasks; investigate it before any Hydra host relies on renderer
selection.

## Deferred adapter work

These retain their parent-phase assignments. The
[cross-phase rule](../design/ROADMAP_POLICY.md#cross-phase-delivery) permits
only foundation subsets; broader parent-phase work stays outside this queue.

LOTUS-MATERIAL-02 took the material candidate fixes the survey justified.
The constant tangent normal's `st`, path classification and
`ND_tiledimage` tiling wait in Renderer Phase 8, and light selection and the light-table estimates in Renderer
Phase 9, until an asset or measurement demonstrates the need
([material review](../reports/2026-10-28-material-foundation-closure.md#deferred-and-accepted-items),
[practical foundation review](../reports/2026-10-28-practical-foundation-closure.md#deferred-and-accepted-items),
[asset gap survey](../reports/2026-10-29-asset-gap-survey.md#decision)).

## Previous delivery evidence

Completed work is tracked by the [release records](../releases/README.md),
including the [practical foundation](../releases/v0.7.0.md),
[ReSTIR DI exit review](../reports/2026-10-17-renderer-phase5-exit.md),
[viewport foundation](../releases/v0.5.2.md),
[SVGF exit review](../reports/2026-10-18-svgf-viewport.md#renderer-phase-6-exit)
and [interactive-rendering release gate](../reports/2026-10-18-v0.6.0-release-gate.md).
The [archive index](../archive/README.md) preserves earlier plan citations.
