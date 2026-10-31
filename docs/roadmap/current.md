# Current

The only active task checklist. Phase/release status and targets belong in
[the canonical table](README.md#status-at-a-glance); priorities are defined
by the [development strategy](../design/POST_V060_STRATEGY.md).

## Execution order

The [Interactive Quality milestone](README.md#v090--interactive-quality) is
active. Its [contract](../design/INTERACTIVE_QUALITY.md) comes first, because
the [strategy](../design/POST_V060_STRATEGY.md#5-entry-conditions-for-later-work)
requires it before defaults or history behaviour change.

1. **LOTUS-INTERACTIVE-01 (P0).** Review the
   [contract](../design/INTERACTIVE_QUALITY.md), then mark it accepted.
2. **LOTUS-INTERACTIVE-02 (P0).** Add the
   [fixed fixture set](../design/INTERACTIVE_QUALITY.md#7-measuring-still-and-moving-separately)
   and record a baseline of today's behaviour, with no renderer change.
3. **LOTUS-INTERACTIVE-03 (P0).** Add the Interactive and Reference presets to
   both hosts, and make the
   [invalidation table](../design/INTERACTIVE_QUALITY.md#5-invalidation-by-change-category)'s
   changes, with a test per row.
4. **LOTUS-INTERACTIVE-04 (P1).** Add Balanced
   [settling](../design/INTERACTIVE_QUALITY.md#4-settling).
5. **LOTUS-INTERACTIVE-05 (P1).** Measure the ghosting candidates under the
   contract's adoption and stop rule. A measured negative closes the task.

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
