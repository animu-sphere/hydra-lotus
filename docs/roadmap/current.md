# Current

The only active task checklist. Phase/release status and targets belong in
[the canonical table](README.md#status-at-a-glance); priorities are defined
by the [development strategy](../design/POST_V060_STRATEGY.md).

## Execution order

The [Interactive Quality milestone](README.md#v090--interactive-quality) is
active. Its [contract](../design/INTERACTIVE_QUALITY.md) is accepted, its
fixed fixtures and a
[baseline of today's behaviour](../reports/2026-11-08-interactive-baseline.md)
are recorded, the Interactive and Reference presets and the
[invalidation table](../design/INTERACTIVE_QUALITY.md#5-invalidation-by-change-category)
are [delivered](../reports/2026-11-08-interactive-presets.md), Balanced
[settles](../reports/2026-11-08-balanced-settling.md) into the Reference
image, and the ghosting candidates are
[measured](../reports/2026-11-09-ghosting-candidates.md#decision)
(LOTUS-INTERACTIVE-01 to -05). No candidate met the contract's adoption
rule, and the stop rule closes that investigation. Every task is done and no
task is active; the milestone's release gate is next.

The [asset gap survey](../reports/2026-10-29-asset-gap-survey.md#decision) also found that `usdrecord --renderer Storm` creates the Lotus
renderer when `hdLotus` is on the plugin path. Its cause is unexplained and
unassigned; investigate it before any Hydra host relies on renderer
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
including [materials and lighting](../releases/v0.8.0.md),
the [practical foundation](../releases/v0.7.0.md),
[ReSTIR DI exit review](../reports/2026-10-17-renderer-phase5-exit.md),
[viewport foundation](../releases/v0.5.2.md),
[SVGF exit review](../reports/2026-10-18-svgf-viewport.md#renderer-phase-6-exit)
and [interactive-rendering release gate](../reports/2026-10-18-v0.6.0-release-gate.md).
The [archive index](../archive/README.md) preserves earlier plan citations.
