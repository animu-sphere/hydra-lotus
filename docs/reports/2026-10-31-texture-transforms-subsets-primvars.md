# Texture transforms, per-face materials and colour primvars

> Later: LOTUS-SKEL-01 renders the bike's four `UsdSkel` meshes ([report](2026-10-31-skinned-meshes.md#survey-assets)).

## Evaluation brief

LOTUS-MATERIAL-02, the third task of the
[Materials & Lighting milestone](../roadmap/README.md#v080--materials--lighting).
The [asset gap survey](2026-10-29-asset-gap-survey.md#results) found
`UsdTransform2d` lookups grey, `GeomSubset` bindings ignored, colour primvars
grey and unbound `displayOpacity` opaque, each with a visible effect. The
[material foundation review](2026-10-28-material-foundation-closure.md#deferred-and-accepted-items)
had deferred these fixes until an asset justified them.

Adopt them if committed fixtures match their references headless and through
Hydra, references no case reaches stay byte-identical, and
TextureTransformTest, PrimvarInterpolation, Teapot, CarbonFrameBike and
AutodeskTelescope lose those warnings and show what Storm shows.

## Environment

Measured 2026-10-31 on top of `e23ac9e`: Windows 11 x86_64, NVIDIA RTX A5000
and the OpenUSD 26.08 lookdev runtime with Python 3.13. Fixtures render at
160×160; references hold 4,096 BSDF-sampled samples; comparisons use 16 and
256 spp from sample index 2²⁰. [Data](2026-10-31-texture-transforms-subsets-primvars.json)
has the commands, digests and every comparison.

## Implementation

- **Transforms.** `TextureInput::st_transform` is a 2×3 matrix per lookup, not
  per material, because a `UsdTransform2d` feeds one `UsdUVTexture` and a
  material's lookups may differ over one coordinate source. The translator composes
  `in * scale * rotate + translation`, as the
  `UsdTransform2d` definition specifies, along chains and from a constant `in`.
  The shader applies it in `Lookup`, and `TangentFrame` applies its linear part
  to the normal map's differentials. A lookup record grows from 64 to 96 bytes,
  a material record from 560 to 784.
- **Per-face materials.** The adapter splits a mesh, so the GPU scene,
  light table, reservoirs and temporal metadata keep one material per instance
  and need no change. The closure review's alternative, a per-triangle material,
  would touch all of them and needs a coordinate set per material per instance.
  Each part costs a BLAS, and the telescope's 627 subsets on 82 meshes
  become up to that many; its `usdrecord` wall time stayed about 16 s, dominated
  by start-up, and was not measured further.
- **Colour primvars.** `MeshGeometry::colors` holds named sets and
  `Material::base_color_primvar` names one. A set is one value per corner or
  one for the mesh: Teapot reads a constant `displayColor` through a bound
  material, and the telescope meshes inspected author one no material reads,
  which per-corner storage would expand to 36 bytes per triangle for nothing.
  An instance record grows from 112
  to 128 bytes. Only `diffuseColor` is translated, the only input an asset
  connects; other inputs warn.
- **Coverage.** Constant `displayOpacity` below 1 goes on the unbound mesh's
  private material. Non-constant values warn.

## Fixtures

Three fixtures are new, [described in the scene reference](../reference/SCENE.md#material-fixtures).
Hydra's snapshots equal the mirrors, and its images under NEE/MIS and BSDF
sampling equal the backend's images of the mirrors bit for bit.

| Fixture | Reference integrator, BSDF, 256 spp | NEE/MIS, 256 spp | Negative control, max \|z\| |
| --- | --- | --- | --- |
| `material-transforms-v1` | image z 0.49 0.27 0.34, 8px tiles max \|z\| 3.34 mean z^2 1.06 | image z 0.39 0.41 0.45, 8px tiles max \|z\| 1.75 mean z^2 0.13 | 273.98 |
| `material-subsets-v1` | image z -0.01 0.08 -0.24, 8px tiles max \|z\| 3.06 mean z^2 1.06 | image z 0.20 -0.09 -0.19, 8px tiles max \|z\| 1.34 mean z^2 0.12 | 604.98 |
| `material-primvars-v1` | image z 0.83 -0.39 -0.42, 8px tiles max \|z\| 3.27 mean z^2 1.05 | image z -0.13 -0.04 0.12, 8px tiles max \|z\| 1.53 mean z^2 0.11 | 327.21 |

The wavefront integrator's NEE/MIS images equal the reference integrator's in
these figures. The references come from the shader under test, so
`material-transforms-v1` adds an independent authoring: the same transforms baked
into the coordinates and fallbacks, identity on the lookups, is accepted at max
\|z\| 1.75. It covers Lookup and the tangent frame, NormalTurned included.

### Existing fixtures

Rewriting every reference left `material-basics-v1`, `material-normals-v1`,
`material-mtlx-v1` and `material-transmission-v1` byte-identical, so the larger
records and the identity transform change no image. Three changed on
purpose, each on one card: Transform2d in `material-textures-v1`, Subset in
`material-emission-v1` and DisplayOpacity in `material-alpha-v1`, which recorded
the old behaviour.

## Edits

`lotus-renderer-hydra-material` follows a subset's new binding, its faces
growing to all of the mesh and shrinking back, its removal and its mesh's
removal, and `displayOpacity` edits, clamps and malformed values.
`lotus-renderer-hydra-render-pass` requires a part to follow its rprim's
selection. `lotus-renderer-scene-update` checks colour-set selection and sizes.

Two UsdImaging behaviours shaped the rules. It resolves a subset without a
binding to the mesh's material, which would split Teapot into four identical
parts, so such a subset makes no part. It hands over the binding of any family, not
only `materialBind`, and Storm shows it, so Lotus does too. It also hands over an
invisible mesh's topology without subsets, so parts merge until it is shown.

## Fault injection

| Defect | Caught by |
| --- | --- |
| Tangent frame ignores the transform | `renderer.material.fixtures`, −5.3 standard errors |
| Colour set interpolated with two corners exchanged | `renderer.material.fixtures`, 11.4 standard errors |
| Rotation composed clockwise | `lotus-renderer-hydra-material-fixtures` |
| A part ignores render-pass selection | `lotus-renderer-hydra-render-pass` |
| `displayOpacity` not applied | `lotus-renderer-hydra-material-fixtures` |

The first was caught by the reference before the baked comparison ran. The baked
comparison's power was not tested separately with a defect written into the
references.

## Survey assets

| Stage | Warnings of these gaps | Against Storm |
| --- | --- | --- |
| TextureTransformTest | None | The six transformed quads show the same labels and arrows, rotated the same way |
| PrimvarInterpolation | None | Every interpolation shows the same colour pattern |
| Teapot | None | Green, and one mesh without parts |
| CarbonFrameBike | None; 4 `UsdSkel` meshes stay rejected, for LOTUS-SKEL-01 | Tyre lettering shows; the cables Storm shows are those meshes |
| AutodeskTelescope | None; 1 material without a surface warns | The same red and bright parts show on the same faces |
| Kitchen Set | None | No visible regression; coverage on its 13 meshes not separately compared |

Storm's lighting and tone differ, as the survey noted, so no pixel values are
compared. These renders stay local, since the assets are not redistributable.

## Decision

Adopt all four. The fixtures match headless and through Hydra, unrelated references
are byte-identical, and the surveyed stages lose the warnings and show the features.
LOTUS-MATERIAL-02 is complete.

## Deferred

- `ND_tiledimage` tiling can now use `st_transform`; no surveyed asset needs it,
  so it keeps warning.
- Colour sets on inputs other than the base colour, and MaterialX colour readers.
- Non-constant `displayOpacity`, and faces an invisible `GeomSubset` hides.
- A float-triple primvar no material reads is still uploaded. Reading only
  named sets needs the mesh to follow material edits.
- Storm ignores chained `UsdTransform2d`, and shows fallbacks and `displayColor`
  out of range unclamped. Lotus composes chains and clamps, as the IR requires.

## Not covered

Not rerun: the full core and Hydra GPU suites, the viewport, research and ci-core
trees. Run: the headless report; Hydra's fixtures on GPU and CPU, material,
render-pass, texture, normals and every test without the `gpu` label; core
temporal and reservoir GPU tests, evidence, assets, install tree, test tiers and
scene update; 114 script tests and `check_docs`.
