# Material emission fixture

> Later: `GeomSubset` bindings are read, which changes the Subset card and this fixture's references ([report](2026-10-31-texture-transforms-subsets-primvars.md#existing-fixtures)).

## Evaluation brief

LOTUS-MATERIAL-01, fourth slice. The
[first](2026-10-26-material-basics.md), [second](2026-10-27-material-textures.md)
and [third](2026-10-27-material-normals.md) slices established committed
`UsdPreviewSurface` fixtures with independent C++ mirrors, reference images
and Hydra/headless agreement, and audited base colour, metallic/roughness,
textures and UV, and shading normals. This slice audits the fifth item of
the [workstream's order](../design/ROADMAP_POLICY.md#practical-foundation-workstreams),
emission, against the [material contract](../reference/SCENE.md#materials-and-environment),
the [light table](../reference/SCENE.md#light-sampling), the
[translator](../reference/SCENE.md#materials) and the
[texture decoding](../reference/SCENE.md#materials), on a stage that reaches
them from a USD file.

The new fixture, `material-emission-v1`, is authored in this repository by
`scripts/material_fixtures.py`, with a Radiance HDR image. Its mirror holds
what the translator should make of each authored value, Hio's decoded float
texels included, so the Hydra CTest checks the translator and the decoder
from file to image, and the NEE/MIS images check the light table's
treatment of every emitter against the BSDF-sampled reference.

Adopt the fixture if the stage extracts to the mirror, every rendered mode
matches the reference by the [DES-Q5 metric](../reference/SCENE.md#reference-images),
Hydra's images are the mirror's bit for bit, and the metric rejects an
emission map decoded in the wrong colour space. Emission transport itself
is already checked against exact expectations by `renderer.path.light_sampling`
([report](2026-10-11-emissive-nee-mis.md)); this slice checks what real USD
assets feed it. Alpha masks are the next slice; the viewport, temporal/SVGF
modes, ReSTIR and performance are not covered. This slice adds no shader
path.

Conditions: Windows AMD64, MSVC 19.51 Release, NVIDIA RTX A5000, driver
597.16, Vulkan 1.4.329, validation and synchronization validation active,
clocks unlocked. Source `2a2ab47` plus this change. The binaries' hashes,
the reference's and fixture files' hashes and every value are in the
[data](2026-10-27-material-emission.json).

## Fixture

`material-emission-v1` is a 5×5 grid of the normals fixture's cards and
coarse spheres under the other fixtures' camera, dome, rect key light and
`displayColor` backdrop, at 160×160. Its rows cover constant emission,
including a value above 1, a negative component and a non-finite one;
emitters' surfaces, sides and transforms; 8-bit emission maps; a missing
image's fallback, an inverted lookup and an HDR image under three colour
spaces; and arrangements real assets use: a tilted emitter, a textured
sphere, one lookup driving base colour and emission, an unlit material
(`diffuseColor` 0 and `ior` 1) and an emissive `GeomSubset`. `glow.hdr` is
4×4 with flat RGBE scanlines whose values decode exactly, from at most 0.47
on its top row to 3.75 on its bottom one. The
[scene reference](../reference/SCENE.md#material-fixtures) lists every
object.

`usdchecker` reports only the unresolvable `missing.png`, which the
Missing card names on purpose; the `inf` component, the HDR image and the
subset pass. The reference was written with
`lotus-headless --write-material-reference validation/reference`; all four
fixtures took 13.1 seconds, and the rewritten references of the other three
are byte-identical to the committed ones.

## Audit

| Item | Contract | Evidence | Finding |
| --- | --- | --- | --- |
| Constant emission | `emissiveColor` in linear RGB, each component clamped to be non-negative, no upper clamp | Warm, Bright (6), Dim and Clamped extract as authored, Clamped's −1 as 0 | As documented |
| Non-finite values | A non-finite component leaves the input's default | NonFinite's `(inf, 1, 1)` extracts as (0, 0, 0) and renders as the default grey | As documented |
| Emission and the BSDF | Emission is added at every hit, beside the full BSDF | Metal's copper reflection shows the key light over its emission | As documented |
| Two-sided emission | Emitted from both sides | Back, wound clockwise and seen from behind, shows Warm's (1, 0.6, 0.2) plus 0.17 of reflected light; front-facing cards glow onto the backdrop behind them | As documented |
| Transforms and angle | Radiance independent of the view angle; light-table areas in world space | Mirrored, Stretched and Tilted (60° about X) show the same emission plus 0.10–0.21 of reflected light; the NEE/MIS images agree with the BSDF-sampled reference | As documented |
| 8-bit emission maps | Lookups as for any colour input; `auto` 8-bit RGB is sRGB | Srgb, Raw, Auto, Grey and ScaleBias (up to 3.2) extract and render as documented | As documented |
| Fallbacks and inverted lookups | A key with no image returns its fallback unscaled; the light table uses the fallback's mean, or `max(scale, 0) + max(bias, 0)` | Missing emits (2, 1, 0.5); Inverted's scale −1 and bias 1 render as documented | As documented |
| HDR emission maps | 16-bit, half and float images become `Rgba32Float`; under `sRGB` the adapter decodes them on the CPU | Hio's `glow.hdr` texels equal the mirror's under `raw`, `auto` (linear) and `sRGB` (decoded) exactly | As documented |
| Light-table estimate of float maps | The bound assumes texels in [0, 1]; the estimate only steers sampling | Hdr's texels reach 3.75 against a bound of 1, so its triangles are under-weighted; NEE/MIS still matches the reference at 256 spp | **Documented**: the underestimate was implied, not stated |
| Unlit assets | `ior` 1 removes the specular interface | Unlit, black with `ior` 1, reflects nothing: its per-sample variance is about 10⁻⁴ against 0.03 for Srgb, which has the same emission | As documented |
| Shared lookups | Each input that a `UsdUVTexture` drives becomes a lookup | Shared's base colour and emission read the same image | As documented |
| `GeomSubset` bindings | Not stated | The adapter read only the mesh's binding and said nothing: Subset's emissive right face rendered grey without a warning | **Fixed**: the adapter now warns; per-face materials remain unsupported, see [next items](#next-items) |
| Not covered | UsdLux mesh lights, emission with coverage, emissive instancers, emission animation | Not authored | Mesh lights are already listed as unsupported; coverage is the alpha slice's |

The warning names the mesh whenever the adapter reads a topology with a
bound subset: "Lotus ignores the GeomSubset material bindings of
/MaterialEmission/Objects/Subset: the mesh's own binding covers every face".
It changes nothing else: the snapshot and the images are those the mirror,
which binds the whole card to grey, predicts.

## Results

**Extraction.** UsdImaging's snapshot of the stage equals its mirror
exactly: materials, bindings, triangles, texture-coordinate sets, seven
decoded textures (the palette under `sRGB`, `raw` and `auto`, `ramp.png`
under `auto`, and `glow.hdr` under `raw`, `auto` and `sRGB`), lights and
environment. No transform, camera, position or normal set differs even by
rounding, the 60° rotation included. The warnings are the unreadable
`missing.png` and the subset's.

**Images against the reference** (sample index 2²⁰, 8-pixel tiles):

| Mode | spp | Image z (R, G, B) | Max tile \|z\| | Mean tile z² |
| --- | ---: | --- | ---: | ---: |
| Reference integrator, BSDF | 16 | 0.38, 0.59, 0.67 | 3.66 | 0.92 |
| Reference integrator, BSDF | 256 | 0.71, 0.65, 0.75 | 3.95 | 1.03 |
| Reference integrator, NEE/MIS | 16 | −0.54, −1.11, −0.75 | 2.83 | 0.30 |
| Reference integrator, NEE/MIS | 256 | −0.87, −0.18, −0.29 | 2.57 | 0.39 |
| Wavefront integrator, NEE/MIS | 16 | −0.54, −1.11, −0.75 | 2.83 | 0.30 |
| Wavefront integrator, NEE/MIS | 256 | −0.87, −0.18, −0.29 | 2.57 | 0.39 |
| Hydra, NEE/MIS | 256 | −0.87, −0.18, −0.29 | 2.57 | 0.39 |
| Hydra, BSDF | 256 | 0.71, 0.65, 0.75 | 3.95 | 1.03 |

The wavefront images are byte-identical to the reference integrator's, and
each Hydra image is byte-identical to the backend's image of the mirror.
The other three fixtures' rows are unchanged to the last digit.

**Power and reproducibility.** The sRGB palette decoded as raw, which
changes four objects, is rejected at max tile |z| 4005.20 at 256 spp.
Re-rendering the reference's own samples reproduces the committed mean bit
for bit on this device and build, for every fixture.

**Cost.** Linking `lotus-headless` and its post-build report, all checks
included, took 54 s; the Hydra CTests took 0.10 s (CPU) and 61.7 s (GPU,
45.3 s before). The new fixture's share was not timed separately.

## Decision

Adopted: `material-emission-v1`, its HDR image and its mirror. Constant and
textured emission, its clamping, two sides, fallbacks and HDR decoding need
no renderer or translator fix; the light table's underestimate for float
maps is now documented. `GeomSubset` material bindings now warn instead of
being ignored silently.

## Next items

- Per-face materials. Splitting a mesh by its `GeomSubset` bindings into
  one record per material, or giving the instance record a per-triangle
  material, is a bounded candidate fix; it waits for an asset that needs
  it, as emissive panels on modelled fixtures would.
- A float emission map's light-table weight could use the image's maximum
  instead of the [0, 1] bound. The underestimate costs variance only, and
  this fixture shows no bias; it waits for a measured need.
- The candidate fixes from the [textures](2026-10-27-material-textures.md#next-items)
  and [normals](2026-10-27-material-normals.md#next-items) slices
  (`UsdTransform2d`, the constant normal's set) are unchanged.
- Following the audit order: alpha masks, as a fixture beside these.

## Unavailable and unmeasured

- The reference is a finite 4096-spp estimate, not ground truth; agreement
  is statistical except where stated byte-identical.
- Bit-for-bit reference reproduction holds on this device, driver and
  build only. The mirror's sRGB-decoded HDR texels equal this build's
  `std::pow`; another C runtime could round differently.
- The light table's entries and weights were not inspected; the NEE/MIS
  images' agreement with the reference is the evidence that each emitter
  is sampled without bias.
- The viewport, usdview and the installed Hydra host were not run with the
  fixture. `ci-core` was not run; of the Hydra CTests only the two
  material-fixture CTests ran, and of the core CTests the evidence, assets,
  install-tree and test-tier checks.
- Third-party production assets were not audited.
