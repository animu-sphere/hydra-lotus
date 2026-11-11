# Current

The only active task checklist. Phase/release status and targets belong in
[the canonical table](README.md#status-at-a-glance); priorities are defined
by the [development strategy](../design/POST_V060_STRATEGY.md).

## Execution order

The [MaterialX Shading Models milestone](README.md#v0100--materialx-shading-models)
is active. Its [contract](../design/MATERIALX_SHADING_MODELS.md) comes first,
then the IR v2 design and the BSDF API contract, before any model
([contract §11](../design/MATERIALX_SHADING_MODELS.md#11-recommended-order)).
LOTUS-MATERIALX-09 runs from each stage on rather than at the end.

1. **Contract review.** Done: the
   [contract](../design/MATERIALX_SHADING_MODELS.md) is accepted. Its
   remaining [design-review decisions](../design/MATERIALX_SHADING_MODELS.md#10-decisions-for-the-design-review)
   (§10.2, §10.4, §10.5) are settled as the tasks that need them start.
2. **LOTUS-MATERIALX-01.** Material IR v2: the
   [design](../design/MATERIAL_IR_V2.md) is accepted, settling contract
   §10.1, §10.3 and §10.6. Done: `UsdPreviewSurface` renders through the
   tagged material and the 128-byte GPU record with every committed
   reference byte-identical
   ([report](../reports/2026-11-11-material-ir-v2.md)).
3. **LOTUS-MATERIALX-02.** The BSDF API: the
   [contract](../design/BSDF_API.md) is accepted. Done: both integrators
   call `BsdfEvaluate`, `BsdfPdf` and `BsdfSample`, which cover the whole
   sphere, `lotus-bsdf-test` checks them on the CPU in the `fast` tier,
   and every committed reference is byte-identical
   ([report](../reports/2026-11-11-bsdf-api.md)).
4. **LOTUS-MATERIALX-03.** Add layer and mix composition, with consistency,
   white-furnace and reciprocity tests.
5. **LOTUS-MATERIALX-04.** Translate the
   [supported node subset](../design/MATERIALX_SHADING_MODELS.md#5-boundary-of-materialx-graph-support)
   and diagnose everything else. It may start beside 01–03.
6. **LOTUS-MATERIALX-05.** glTF PBR core, the smallest end-to-end slice.
7. **LOTUS-MATERIALX-06 and 07.** glTF PBR's P1 extensions, and Standard
   Surface with its own composition.
8. **LOTUS-MATERIALX-08.** OpenPBR, fuzz over coat over base.
9. **LOTUS-MATERIALX-10.** The release gate and the per-model compatibility
   table.

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
including [interactive quality](../releases/v0.9.0.md),
[materials and lighting](../releases/v0.8.0.md),
the [practical foundation](../releases/v0.7.0.md),
[ReSTIR DI exit review](../reports/2026-10-17-renderer-phase5-exit.md),
[viewport foundation](../releases/v0.5.2.md),
[SVGF exit review](../reports/2026-10-18-svgf-viewport.md#renderer-phase-6-exit)
and [interactive-rendering release gate](../reports/2026-10-18-v0.6.0-release-gate.md).
The [archive index](../archive/README.md) preserves earlier plan citations.
