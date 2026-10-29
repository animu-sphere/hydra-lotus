# MaterialX Standard Surface subset

## Evaluation brief

LOTUS-MTLX-01, the first task of the
[Materials & Lighting milestone](../roadmap/README.md#v080--materials--lighting).
The [asset gap survey](2026-10-29-asset-gap-survey.md#decision) found
MaterialX-only surfaces rendering with the default material: all 21 meshes
of OpenChessSet, StandardShaderBall's four example materials and
MaterialXTest. Every one of their surfaces is an
`ND_standard_surface_surfaceshader`, fed by `ND_image`, `ND_tiledimage`,
`ND_normalmap`, `ND_texcoord`, `ND_extract` and `ND_convert` nodes. The
delegate never asked for MaterialX networks, so each material arrived without
a surface.

This change has the delegate request the `mtlx` render context and
translates the Standard Surface inputs that the existing
[material IR](../reference/SCENE.md#materials-and-environment) already
represents: base colour and its weight, metalness, specular roughness and
IOR, emission and its weight, grey opacity and tangent-space normal maps. It
reads them through those nodes. Lobes the IR lacks warn instead. It also
settles which surface wins when a material has both a `UsdPreviewSurface`
and a MaterialX one, and explains the shader ball's missing ground texture.

Adopt the translation if a committed MaterialX fixture extracts through
Hydra to an independent C++ mirror and matches its reference images
headless and through Hydra. OpenChessSet and StandardShaderBall must also
render their authored base colours, metalness, roughness and normal maps.
The task adds no shader path or IR field: transmission, subsurface and coat
need transport contracts, and OpenPBR and arbitrary node graphs are not
used by a surveyed asset. Those stay deferred.

Conditions: Windows AMD64, MSVC 19.51 Release, NVIDIA RTX A5000, driver
597.16, Vulkan 1.4.329, validation and synchronization validation active,
clocks unlocked. Source `e556173` plus this change. The binaries' hashes,
the reference's and fixture's hashes and every value are in the
[data](2026-10-29-materialx-standard-surface.json). The surveyed assets are
local and not redistributable; they are named relative to the collection,
and their images are not committed.

## Translation

The delegate's `GetMaterialRenderContexts` returns `mtlx`. UsdImaging then
hands over a material's MaterialX network when it has one and its universal
network otherwise, so a material with only a `UsdPreviewSurface` is
translated as before.

**Precedence.** A material with both surfaces is translated from its
MaterialX surface. Storm renders the MaterialX surface of such a material
too, as the shader ball's ground shows. StandardShaderBall shows why this matters: its ground and
walls author both surfaces, and only the MaterialX one reads the image as
its author intended (see [the ground texture](#the-shader-balls-ground-texture)).
The alternative, preferring `UsdPreviewSurface` because the IR represents
all of it, would differ from Storm on exactly these assets.

**Inputs.** The translator starts from Standard Surface's defaults, base
colour 0.8 and roughness 0.2. It multiplies `base` into `base_color` and
`emission` into `emission_color`, folding a constant weight into a lookup's
scale, bias and fallback. `specular` 0 sets the IR's ior to 1, which removes
the dielectric interface, as the shader ball's Houdini-authored materials
rely on. A node graph is evaluated into a constant or one texture lookup.
An image without a file is its `default` constant, which turns the shader
ball's empty emission image into zero emission rather than an untranslated
graph. An image's colour space is sRGB only when its file names an `srgb_*`
one; otherwise it is raw, as MaterialX defines for unannotated scalar and
vector images. `ND_image`'s default address mode is periodic, unlike
`UsdUVTexture`'s. The
[reference](../reference/SCENE.md#materialx-standard-surface) lists every
rule.

**Warnings.** Inputs the IR cannot represent are ignored with one warning
per material, naming them: "Lotus ignores \<material\> inputs its material
cannot represent: coat, sheen, transmission". These are the
`coat`, `diffuse_roughness`, `sheen`, `specular_anisotropy`, `subsurface`,
`thin_film_thickness` and `transmission` lobes, `specular` weights other than
0 and 1, a non-white `specular_color` (a metal's edge tint), a coloured
`opacity` and a constant world-space `normal`. Graphs it does not translate
keep the input's default and report the input as connected, as for
`UsdPreviewSurface`. An image whose lookup no input takes is not decoded.

## Fixture

`material-mtlx-v1` is a 5×5 grid of the emission fixture's coarse spheres
and cards under the other fixtures' camera, dome, rect key light and
`displayColor` backdrop, at 160×160. Every material is an
`ND_standard_surface_surfaceshader` behind `outputs:mtlx:surface`. `scripts/material_fixtures.py`
authors it with the existing PNG images and no new texture. Its rows cover
constants on spheres (the defaults, a base weight, a metal, an IOR and
specular 0), constants on cards (weighted emission, grey opacity, clamped
values, ignored lobes and surface precedence), images (sRGB, raw, weighted,
an extracted channel and a converted emission map), address modes and a
named coordinate set, and normal maps, an identity tiled image, an image
without a file and an untranslated node. The
[scene reference](../reference/SCENE.md#material-fixtures) lists every
object and its expected translation.

`usdchecker` passes the stage. The reference was written with
`lotus-headless --write-material-reference`; all six fixtures took 12.0
seconds, and the rewritten references of the other five are byte-identical
to the committed ones.

## Audit

| Item | Contract | Evidence | Finding |
| --- | --- | --- | --- |
| Network selection | Not stated: the delegate requested no context, so MaterialX-only materials had no surface | The fixture's 25 materials extract from their MaterialX surfaces | **Fixed**: the delegate requests `mtlx` |
| Precedence | Not stated | Precedence, authored red in `UsdPreviewSurface` and blue in MaterialX, extracts and renders blue | **Decided**: MaterialX wins, as in Storm |
| Defaults and weights | Standard Surface's defaults; `base` and `emission` multiply their colours | Defaults extracts base colour 0.8 and roughness 0.2; Weighted, Emissive and WeightedImage extract the products, the last a lookup scaled by 0.5 | As documented |
| Metalness, roughness, IOR | Clamped as for `UsdPreviewSurface`; Standard Surface squares roughness into alpha as the IR does | Metal, Ior and Clamped extract as authored or clamped; the spheres match the reference under NEE/MIS | As documented |
| Specular weight | 0 removes the interface; weights other than 0 and 1 are not represented | NoSpecular extracts ior 1 | As documented |
| Opacity | A grey constant is coverage | Opacity's 0.5 renders as stochastic half coverage | As documented |
| Lobes the IR lacks | Ignored with a warning | Lobes renders as its teal base and warns for coat, sheen and transmission | As documented |
| Images and colour spaces | `srgb_*` decodes as sRGB, anything else raw | Srgb and Raw extract `palette.png` under `sRGB` and `raw`; Hio's texels equal the mirror's | As documented |
| Channels and conversions | `ND_extract` selects a lookup's channel; `ND_convert` keeps three-component lookups | Channels reads `rgba.png`'s blue as metalness; EmissionMap reads the raw palette as emission | As documented |
| Address modes and coordinates | Periodic by default; `constant` is transparent black; `texcoord` reads `st` unless connected | Periodic, Clamp, Mirror and Constant over `st` from −0.5 to 1.5; Named reads `uv1` | As documented |
| Normal maps | `ND_normalmap` signs the map and scales x and y | NormalMap and NormalScale extract scale 2 and 1 with bias −1 and −0.5 | As documented |
| Untranslated graphs | The input keeps its default and warns; its image is not decoded | Untranslated keeps base colour 0.8 and warns for `base_color`; no lookup names its image | As documented |
| Not covered | Graphs whose texture coordinates are transformed, OpenPBR, lobes beyond the IR, displacement, `ND_UsdUVTexture` | Not authored | Transforms belong to LOTUS-MATERIAL-02; the rest are deferred |

`material_test.cpp` checks the same rules on hand-built networks: the
defaults, weights, specular 0, the order of the ignored inputs, each node's
lookup, two-sided failures, `ND_UsdPreviewSurface_surfaceshader` and the
requested context.

## Results

**Extraction.** UsdImaging's snapshot of the stage equals its mirror
exactly: materials, bindings, triangles, texture-coordinate sets, five
decoded textures (the palette under `sRGB` and `raw`, and `rgba.png`,
`ramp.png` and `tilts.png` under `raw`), lights and environment. No
transform, camera, position or normal set differs even by rounding. The
warnings are Lobes' and Untranslated's.

**Images against the reference** (sample index 2²⁰, 8-pixel tiles):

| Mode | spp | Image z (R, G, B) | Max tile \|z\| | Mean tile z² |
| --- | ---: | --- | ---: | ---: |
| Reference integrator, BSDF | 16 | −0.28, −0.46, −0.54 | 3.62 | 1.11 |
| Reference integrator, BSDF | 256 | −0.00, 0.38, 0.07 | 3.21 | 1.06 |
| Reference integrator, NEE/MIS | 16 | 0.43, 0.32, 0.19 | 3.00 | 0.17 |
| Reference integrator, NEE/MIS | 256 | 0.27, 0.51, 0.50 | 2.31 | 0.24 |
| Wavefront integrator, NEE/MIS | 16 | 0.43, 0.32, 0.19 | 3.00 | 0.17 |
| Wavefront integrator, NEE/MIS | 256 | 0.27, 0.51, 0.50 | 2.31 | 0.24 |
| Hydra, NEE/MIS | 256 | 0.27, 0.51, 0.50 | 2.31 | 0.24 |
| Hydra, BSDF | 256 | the backend's image of the mirror bit for bit | | |

The Hydra images are each byte-identical to the backend's image of the
mirror, so the BSDF row equals the reference integrator's. The other five
fixtures' checks pass unchanged.

**Power and reproducibility.** WeightedImage with its base weight ignored,
as a translator reading only `base_color` would, is rejected at max tile |z|
85.30 at 256 spp. Re-rendering the reference's own samples reproduces the
committed mean bit for bit.

**Cost.** The Hydra fixture CTests took 0.13 s (CPU) and 71.4 s (GPU, all
six fixtures). The new fixture's share was not timed separately.

## Survey assets

Each stage was rendered by `usdrecord` at 480–640 pixels wide, with Storm
as the visual control as in the survey; no pixel values are compared.

**OpenChessSet** (`assets/full_assets/OpenChessSet/chess_set.usda`). Every
piece and the board now render their authored base-colour, metalness,
roughness and normal maps instead of the default grey: black and white
pieces, their green accents, the gold crowns and the board's
checker and gold border. The 16 remaining warnings are the lobes it uses:
subsurface on the kings and queens, and transmission on the pawns' tops.
Ignoring transmission leaves those tops opaque white (their `base_color`)
where Storm shows tinted glass. With no authored light, `usdrecord`'s
default lighting leaves the horizontal board darker than the pieces in
both renderers; under an added uniform dome the board's checker is plainly
visible.

**StandardShaderBall** (`assets/full_assets/StandardShaderBall/standard_shader_ball_scene.usda`).
The ground shows its numbered grid. The four example materials translate,
warning for what each uses beyond the IR: gold's specular tint,
bubblegum's specular weight and subsurface, and the glass's transmission.
`sss_bars` warns for its `diffuse_roughness`. The remaining `displayColor`
warnings belong to LOTUS-MATERIAL-02.

**MaterialXTest** (`assets/test_assets/MaterialXTest/basicTextured.usda`).
Its brass translates, warning for `coat`. Its roughness image's path
resolves to `./textures/./textures/brass_roughness.jpg`, which neither
renderer opens, so both render the teapot nearly black.

### The shader ball's ground texture

The ground's mesh authors `st` with t from 0 to −1, below the image. Its
material carries both surfaces. The `UsdPreviewSurface`'s `UsdUVTexture`
leaves `wrapS` and `wrapT` at `useMetadata`, which Lotus reads as black, so
every lookup fell outside the image and the ground showed only its
specular reflection: an even grey. The MaterialX surface's `ND_image`
repeats by default, and Storm, which renders the MaterialX surface, showed
the grid. With the ground's MaterialX output blocked in a local override,
Storm shows no grid either, only faint streaks along t. Lotus now
translates the MaterialX surface and shows the grid. No decoder or size
limit was involved: the 7500×7500 half-float EXR decodes.

## Decision

Adopted: MaterialX networks, the Standard Surface subset, the precedence of
MaterialX over `UsdPreviewSurface`, warnings for unrepresented inputs, and
`material-mtlx-v1` with its references and mirror. OpenChessSet and
StandardShaderBall render their authored materials, and the shader ball's
ground is explained and fixed. LOTUS-MTLX-01 is complete.

## Next items

- Transmission and subsurface. OpenChessSet's pawn tops and royal pieces
  and the shader ball's glass and bubblegum use them; each needs a
  transport contract before it can enter the IR. Transmission is the next
  task, LOTUS-TRANSMISSION-01.
- Texture-coordinate transforms. `ND_tiledimage` with tiling, like
  `UsdTransform2d`, needs a transform in the lookup; LOTUS-MATERIAL-02 adds
  one, after which tiled images can use it.
- Storm's reading of the ground's `useMetadata` wraps showed streaks where
  Lotus reads black. Whether Storm clamps an image without wrap metadata is
  not established; it matters only for `UsdPreviewSurface` lookups outside
  the image.
