# Current

The only active task checklist. Phase/release status and targets belong in
[the canonical table](README.md#status-at-a-glance); priorities are defined
by the [development strategy](../design/POST_V060_STRATEGY.md).

## Execution order

Work on the [Materials & Lighting milestone](README.md#v080--materials--lighting).
The [asset gap survey](../reports/2026-10-29-asset-gap-survey.md#decision)
rendered 18 third-party USD stages through Hydra. It found four material
gaps and dropped skinned meshes, each with a visible effect; no asset
demonstrated a lighting gap. Only the following bounded tasks are active,
in this order. The surveyed assets are local and not redistributable, so
each task's committed evidence is a fixture authored here. The survey's
stages are its acceptance check.

- [ ] **LOTUS-MTLX-01 — MaterialX Standard Surface subset (P0).**
  Renderer Phase 8 subset. Have the delegate request Hydra's `mtlx`
  material networks, and translate `ND_standard_surface_surfaceshader` into
  the existing material IR. The subset covers base colour (`base` ×
  `base_color`), metalness, specular roughness and IOR, emission, opacity
  and normal maps. It reads them through the image, tiled-image,
  texture-coordinate, normal-map, extract and convert nodes the surveyed
  assets use. Decide and document the precedence when a material carries
  both a `UsdPreviewSurface` and a MaterialX surface. Inputs the IR cannot
  represent warn, naming the material and the input: transmission,
  subsurface, coat, sheen, thin film and anisotropy.
  Exit evidence: a committed MaterialX fixture with a C++ mirror, reference
  images and Hydra/headless agreement; OpenChessSet and StandardShaderBall
  rendering their authored base colours, metalness, roughness and normal
  maps instead of the default material; an explanation of the
  shader ball's missing ground texture.
  Deferred: transmission, subsurface and coat lobes (each needs a transport
  contract), OpenPBR (no surveyed asset) and arbitrary node graphs.
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
