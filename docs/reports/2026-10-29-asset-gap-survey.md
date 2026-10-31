# Asset gap survey: LOTUS-ASSET-01

> Later: LOTUS-MATERIAL-02 closed the texture transform, per-face material, primvar colour and `displayOpacity` gaps ([report](2026-10-31-texture-transforms-subsets-primvars.md#decision)). LOTUS-SKEL-01 closed the skinned mesh gap ([report](2026-10-31-skinned-meshes.md#decision)); the elephant's automatic camera also sits inside the meshes, in Storm as well ([survey assets](2026-10-31-skinned-meshes.md#survey-assets)).

## Evaluation brief

The [entry condition](../design/POST_V060_STRATEGY.md#5-entry-conditions-for-later-work)
of the Materials & Lighting milestone requires actual asset gaps, and the
[material foundation review](2026-10-28-material-foundation-closure.md#deferred-and-accepted-items)
deferred its candidate fixes until an asset demonstrated the need. Its
fixtures were authored in this repository. This survey renders 18
third-party USD stages through the installed Lotus Hydra delegate and
records what each one uses that Lotus does not translate. The survey is
the evidence for the v0.8.0 task selection.

It changes no renderer, shader, translator or harness code. It adds
`scripts/asset_survey.py`, which counts a stage's material, geometry and
light features with OpenUSD and classifies the Lotus warnings in
`usdrecord` logs. The assets were already on the measurement machine. They
are named here, not committed or redistributed; paths are relative to the
local collection's root.

## Environment

Measured 2026-10-29 at `b217d3e` (v0.7.0): Windows 11 x86_64, NVIDIA RTX
A5000 (driver 597.16), the OpenUSD 26.08 lookdev runtime with Python 3.13,
and the Hydra tree's installed `hdLotus` from the v0.7.0 release gate.
Each stage was rendered by `usdrecord` at 640 pixels wide with default
render settings. The authored camera was used where a stage has one
(StandardShaderBall, McUsd, NormalsTextureBiasAndScale); otherwise,
`usdrecord`'s free camera frames the stage. The stage's lights are used
where it has any; otherwise, `usdrecord`'s default lighting is used. Storm
images of the same stages and cameras are the visual control. They show
which authored features exist, not reference radiance: Storm's lighting
and tone differ from a path tracer's, so this survey compares no pixel values.

```sh
python scripts/asset_survey.py inventory --root <usd_sample> --out inventory.json <stage>...
usdrecord --renderer Lotus --imageWidth 640 [--camera <path>] <stage> <name>.png > <name>.log 2>&1
python scripts/asset_survey.py warnings --out warnings.json <name>.log...
```

`PXR_PLUGINPATH_NAME` named the installed `hdLotus` resources for the Lotus
renders, and was empty for the Storm renders (see
[other findings](#other-findings)). In Git Bash, `MSYS_NO_PATHCONV=1` keeps
a camera path from being rewritten into a file path.

## Assets

| Stage (relative to the collection) | Origin | Material source |
| --- | --- | --- |
| `assets/full_assets/OpenChessSet/chess_set.usda` | USD WG assets | MaterialX Standard Surface only |
| `assets/full_assets/StandardShaderBall/standard_shader_ball_scene.usda` | USD WG assets | `UsdPreviewSurface`, MaterialX examples |
| `assets/full_assets/CarbonFrameBike/CarbonFrameBike.usdz` | USD WG assets | `UsdPreviewSurface` |
| `assets/full_assets/ElephantWithMonochord/SoC-ElephantWithMonochord.usdz` | USD WG assets | `UsdPreviewSurface` |
| `assets/full_assets/McUsd/McUsd.usda` | USD WG assets | `UsdPreviewSurface` |
| `assets/full_assets/Teapot/Teapot.usd` | USD WG assets | `UsdPreviewSurface` |
| `assets/full_assets/cookie/usd_cookie.usdz` | USD WG assets | `UsdPreviewSurface` |
| `Kitchen_set/Kitchen_set/Kitchen_set.usd` | Pixar Kitchen Set | `displayColor` only |
| `AutodeskTelescope/AutodeskTelescope.usdz` | Autodesk sample | `UsdPreviewSurface` |
| `assets/test_assets/USDZ/DamagedHelmet/DamagedHelmet.usdz` | USD WG test assets | `UsdPreviewSurface` |
| `pixar/Mat_2021.usd` | Pixar RenderMan sample | `UsdPreviewSurface` |
| `assets/test_assets/MaterialXTest/basicTextured.usda` | USD WG test assets | MaterialX only |
| `assets/test_assets/TextureTransformTest/TextureTransformTest.usd` | USD WG test assets | `UsdPreviewSurface` |
| `assets/test_assets/NormalsTextureBiasAndScale/NormalsTextureBiasAndScale.usda` | USD WG test assets | `UsdPreviewSurface` |
| `assets/test_assets/RoughnessTest/RoughnessTest.usdz` | USD WG test assets | `UsdPreviewSurface` |
| `assets/test_assets/AlphaBlendModeTest/AlphaBlendModeTest.usd` | USD WG test assets | `UsdPreviewSurface` |
| `assets/test_assets/TextureCoordinateTest/TextureCoordinateTest.usda` | USD WG test assets | `UsdPreviewSurface` |
| `assets/test_assets/PrimvarInterpolation/primvar_interpolation.usda` | USD WG test assets | `displayColor` only |

Eleven are production-style assets and seven are USD WG conformance tests.
The larger local collections (Moana Island, Pixar's Stirling and
FunDaMental scenes, 1–17 GB each) were left out to keep the survey short.
Every stage opened and every `usdrecord` run exited 0 under both renderers.

## Results

Each gap below is backed by Lotus warnings, the inventory and a visible
difference from the Storm control. [Data](2026-10-29-asset-gap-survey.json)
has every count per stage.

| Gap | Stages | Evidence | Effect in Lotus |
| --- | --- | --- | --- |
| MaterialX surfaces without a `UsdPreviewSurface` | OpenChessSet (21 of 21 meshes), StandardShaderBall (4 example materials), MaterialXTest | 49, 4 and 2 "no surface terminal" warnings; every surface is `ND_standard_surface_surfaceshader`, fed by `ND_image_*`, `ND_tiledimage_*`, `ND_normalmap`, `ND_texcoord`, `ND_extract` and `ND_convert` nodes | The default grey material on every chess piece and the board; Storm shows textured pieces |
| `UsdTransform2d` between reader and texture | CarbonFrameBike (11 inputs, scales 4–15), AutodeskTelescope (10, inside node graphs; 9 are identity), TextureTransformTest (6) | 25 untranslated-connection warnings on `diffuseColor` and `normal` | Tyre lettering and normal-map tiling missing; all six transformed test quads grey |
| Per-face materials (`GeomSubset` bindings) | AutodeskTelescope (627 bound subsets on 82 meshes) | 82 warnings | Each mesh takes its own binding on every face |
| Primvar colour: a `UsdPrimvarReader_float3` into `diffuseColor`, or non-constant `displayColor` | Teapot, PrimvarInterpolation (6 meshes), StandardShaderBall (5), CarbonFrameBike (2) | 1 untranslated-connection and 13 `displayColor` warnings | A grey teapot (green in Storm); uniform grey instead of vertex, varying and face-varying colours |
| Skinned meshes (`UsdSkel`) | ElephantWithMonochord (2 of 3 meshes), CarbonFrameBike (4) | 6 "rejected mesh: face vertex index is out of range" warnings, all on meshes bound to a skeleton | The meshes are dropped; the elephant renders empty |
| `displayOpacity` below 1 on unbound meshes | Kitchen Set (13 meshes) | Inventory only; not read, as [documented](../reference/SCENE.md#materials) | Those meshes are opaque |

The MaterialX surfaces use these lobes beyond the
[material IR](../reference/SCENE.md#materials-and-environment): subsurface
(8 of OpenChessSet's 41 surface shaders, through textures, and 1
shader-ball example) and transmission (4 chess shaders and the shader-ball
glass). Metalness is texture-driven in 29 chess shaders. Coat appears only
in MaterialXTest. No surveyed stage uses OpenPBR.

### Not a gap in these assets

- **Supported paths agree with Storm.** DamagedHelmet, cookie, Kitchen
  Set, AlphaBlendModeTest and NormalsTextureBiasAndScale show the same
  authored features in both renderers: textures, normal maps, coverage
  masks, `displayColor` and the authored lights.
- **Unread `UsdPreviewSurface` inputs.** `occlusion` is authored in five
  stages, through a texture in Teapot and DamagedHelmet. It is ambient
  occlusion for raster renderers, and a path tracer correctly ignores it.
  `clearcoat` and `displacement` are authored in AutodeskTelescope,
  ElephantWithMonochord and three test stages, but every value is zero,
  and no `UsdPreviewSurface` authors `opacityMode`.
- **Lights.** The only lights are 5 rect lights (StandardShaderBall), a
  distant light and a dome light (McUsd) and 4 distant lights
  (NormalsTextureBiasAndScale). No stage uses shaping, IES profiles, light
  textures other than the dome's, colour temperature, filters, mesh lights
  or light types Lotus lacks. No stage has many lights.
- **UDIM textures.** None. Formats are JPEG, PNG, TGA and EXR, all decoded.
- **Subdivision.** 1,850 visible meshes in 8 stages author `catmullClark`, and
  Lotus renders their control meshes with smoothed normals, as
  [documented](../reference/SCENE.md#hydra-extraction). Storm at its
  default complexity does not refine either, so the controls show no
  difference.

### Other findings

- **StandardShaderBall's ground texture.** The ground's
  `UsdPreviewSurface` reads an ACEScg EXR through a plain `st` reader.
  Storm shows its numbered grid; Lotus, even brightened, shows an even grey
  and gives no warning. The cause is not established. (Later: the ground's
  `st` lies outside the image, where the `UsdPreviewSurface` lookup's
  `useMetadata` wraps read black; Storm renders the MaterialX surface,
  which repeats. See the
  [MaterialX report](2026-10-29-materialx-standard-surface.md#the-shader-balls-ground-texture).)
- **Renderer selection.** With `hdLotus` on `PXR_PLUGINPATH_NAME`,
  `usdrecord --renderer Storm` (or `GL`) created the Lotus renderer and
  produced byte-identical Lotus images, although the `HD_RENDERER_PLUGIN`
  debug output discovered both plugins; with the path empty, Storm rendered. The plugin's
  `plugInfo.json` gives Lotus priority 0. The cause is not established.
- **Determinism.** That accidental second Lotus run of OpenChessSet was
  byte-identical to the first.

## Decision

The entry condition for production-material expansion is met: local
third-party assets demonstrate MaterialX surfaces, `UsdTransform2d`,
per-face materials and primvar colours, each with a visible effect. They
also demonstrate that skinned meshes are dropped. That is a geometry gap
outside Renderer Phase 8; it enters v0.8.0 because a dropped asset
defeats the [strategy's](../design/POST_V060_STRATEGY.md#1-decision)
first measure, correct rendering of representative scenes.

No asset demonstrates a lighting gap. The lighting half of the milestone
keeps its [entry condition](../design/POST_V060_STRATEGY.md#5-entry-conditions-for-later-work)
and opens no task. Light selection and the light-table estimates stay in
Renderer Phase 9, as the
[practical foundation review](2026-10-28-practical-foundation-closure.md#deferred-and-accepted-items)
assigned them.

The selected tasks, their exit evidence and their deferred remainder are in
[current.md](../roadmap/current.md#execution-order):

| Gap | Disposition |
| --- | --- |
| MaterialX surfaces | Selected: LOTUS-MTLX-01, translating the subset that the IR already represents |
| `UsdTransform2d`, per-face materials, primvar colour, `displayOpacity` | Selected: LOTUS-MATERIAL-02, the deferred candidate fixes that these assets now justify |
| Skinned meshes | Selected: LOTUS-SKEL-01 |
| Transmission and subsurface lobes | Deferred: used by OpenChessSet and the shader ball, but each needs a transport contract; LOTUS-MTLX-01 warns about them instead |
| Coat, OpenPBR | Deferred: one test stage uses coat, and no local asset uses OpenPBR |
| Lights, subdivision refinement | Not selected: no visible gap |
| Shader-ball ground, renderer selection | Investigated in LOTUS-MTLX-01 (whose fixture it is) and on its own, respectively |

## Not covered

- Radiance agreement. Storm is not a reference renderer, and these assets
  have no reference images. Tone differences, such as McUsd's metal crate or
  Mat_2021's highlights, are not interpreted. The fixtures of the selected
  tasks provide that evidence.
- Performance. `usdrecord` wall times (1–27 s, dominated by start-up) are not
  a measurement of Lotus.
- Assets not on this machine, and the large production scenes left out above.
