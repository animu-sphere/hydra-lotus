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
closing the first gap. It left transmission ignored with a warning, so
OpenChessSet's pawn tops and the shader ball's glass render as opaque base
colour; transmission enters first. Only the following bounded tasks are
active, in this order. The surveyed assets are local and not
redistributable, so each task's committed evidence is a fixture authored
here. The survey's stages are its acceptance check.

- [ ] **LOTUS-TRANSMISSION-01 — Dielectric transmission lobe (P1).**
  Renderer Phase 8 subset, the fifth lobe of the
  [design policy's material order](../design/DESIGN_POLICY.md#17-material).
  Write the transport contract first, then implement it in the material IR,
  both integrators and the translator. The contract covers the BTDF
  (smooth and rough GGX refraction with Fresnel, reciprocity and the
  radiance scaling across the interface), the IR fields (a transmission
  weight and colour, and thin versus solid walls), entering and leaving
  through the IOR and the shading normal against the geometric one, and
  what light sampling does at a transmissive surface. The last includes NEE
  shadow rays and MIS, the light table, ReSTIR candidates and the
  temporal/SVGF albedo and history inputs. The translator maps Standard
  Surface `transmission`, `transmission_color` and `thin_walled`.
  `UsdPreviewSurface` opacity stays coverage.
  Exit evidence: the contract reviewed before code; analytic checks of a
  smooth dielectric's reflectance and transmittance and a white-furnace
  energy check; a committed fixture with a C++ mirror, reference images and
  Hydra/headless agreement, the reference and wavefront integrators
  agreeing; the other material fixtures unchanged; OpenChessSet's pawn tops
  and the shader ball's glass rendering as tinted glass instead of their
  opaque base colour.
  Deferred: `transmission_depth` and `transmission_scatter` (volume
  absorption and scattering), dispersion, nested-dielectric priorities,
  caustics through light sampling, subsurface and coat.
- [ ] **LOTUS-MATERIAL-02 — Texture transforms, per-face materials and primvar colour (P1).**
  Renderer Phase 8 subset: the candidate fixes the
  [material foundation review](../reports/2026-10-28-material-foundation-closure.md#deferred-and-accepted-items)
  deferred, now justified by assets. It covers four cases. `UsdTransform2d` goes
  between a primvar reader and a texture. `GeomSubset` material bindings
  give per-face materials. Colour primvars drive colour inputs, through
  `UsdPrimvarReader_float3` and as non-constant `displayColor` on unbound
  meshes. Constant `displayOpacity` below 1 on unbound meshes becomes
  coverage.
  Exit evidence: committed fixtures with reference images and Hydra/headless
  agreement for each case; TextureTransformTest, PrimvarInterpolation,
  Teapot, CarbonFrameBike and AutodeskTelescope render without those
  warnings and show the features Storm shows.
  Deferred: the constant tangent normal's `st` and path classification,
  which stay in Renderer Phase 8.
- [ ] **LOTUS-SKEL-01 — Skinned meshes (P1).**
  Hydra adapter geometry: Lotus rejects every mesh bound to a `UsdSkel`
  skeleton ("face vertex index is out of range"), so ElephantWithMonochord
  renders empty and CarbonFrameBike loses four tubes. Establish the cause
  first; Lotus supports no Hydra ext computation, which is how skinned
  points usually arrive. Then render skinned meshes at their posed points.
  Exit evidence: a committed skinned fixture whose extracted points match
  OpenUSD's skinning, a time change updating them through the existing
  deformation path, and the two survey assets rendering their skinned
  meshes. Deferred: GPU skinning.

The survey also found that `usdrecord --renderer Storm` creates the Lotus
renderer when `hdLotus` is on the plugin path. Its cause is unexplained and
outside these tasks; investigate it before any Hydra host relies on renderer
selection.

## Deferred adapter work

These retain their parent-phase assignments. The
[cross-phase rule](../design/ROADMAP_POLICY.md#cross-phase-delivery) permits
only foundation subsets; broader parent-phase work stays outside this queue.

LOTUS-MATERIAL-02 takes the material candidate fixes the survey justified.
The constant tangent normal's `st` and path classification wait in Renderer
Phase 8, and light selection and the light-table estimates in Renderer
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
