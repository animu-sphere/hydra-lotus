# Material alpha fixture

## Evaluation brief

LOTUS-MATERIAL-01, fifth slice. The
[first](2026-10-26-material-basics.md), [second](2026-10-27-material-textures.md),
[third](2026-10-27-material-normals.md) and [fourth](2026-10-27-material-emission.md)
slices established committed `UsdPreviewSurface` fixtures with independent
C++ mirrors, reference images and Hydra/headless agreement, and audited
base colour, metallic/roughness, textures and UV, shading normals and
emission. This slice audits the sixth and last item of the
[workstream's order](../design/ROADMAP_POLICY.md#practical-foundation-workstreams),
alpha masks, against the [coverage contract](../reference/SCENE.md#materials-and-environment),
[alpha acceptance](../reference/SCENE.md#path-tracing), the
[light table](../reference/SCENE.md#light-sampling) and the
[translator](../reference/SCENE.md#materials), on a stage that reaches them
from a USD file.

The new fixture, `material-alpha-v1`, is authored in this repository by
`scripts/material_fixtures.py`, with an 8×8 RGBA cut-out image. Its mirror
holds what the translator should make of each authored value, Hio's
decoded texels included, so the Hydra CTest checks the translator and the
decoder from file to image, and the NEE/MIS images check light sampling and
shadow rays through partly covered surfaces against the BSDF-sampled
reference.

Adopt the fixture if the stage extracts to the mirror, every rendered mode
matches the reference by the [DES-Q5 metric](../reference/SCENE.md#reference-images),
Hydra's images are the mirror's bit for bit, and the metric rejects alpha
decoded as sRGB colour. Coverage transport itself is already checked
against analytic mixtures by `renderer.path.opacity`
([report](2026-10-08-opacity.md)); this slice checks what real USD assets
feed it. Alpha blending, glass and the specification's `transparent`
lighting response stay outside the
[workstream's subset](../design/ROADMAP_POLICY.md#practical-foundation-workstreams):
coverage opacity is not full transparency. The viewport, depth and surface
AOVs, temporal/SVGF modes, ReSTIR and performance are not covered. This
slice adds no shader path.

Conditions: Windows AMD64, MSVC 19.51 Release, NVIDIA RTX A5000, driver
597.16, Vulkan 1.4.329, validation and synchronization validation active,
clocks unlocked. Source `1b95029` plus this change. The binaries' hashes,
the reference's and fixture files' hashes and every value are in the
[data](2026-10-27-material-alpha.json).

## Fixture

`material-alpha-v1` is a 5×5 grid of the earlier fixtures' cards and one
coarse sphere under their camera, dome, rect key light and `displayColor`
backdrop, at 160×160. Its rows cover constant coverage, including an
authored `opacityMode` and values that clamp; constant thresholds below,
at and above the opacity, non-finite values and a texture-driven threshold;
masks from an RGBA image, the new `cutout.png`, a grey-alpha image, a
one-channel image's `r` and an image without alpha; lookups with
stochastic coverage, an inverted mask, a `black` border, a missing image's
fallback and a minified checker; and arrangements real assets use: two
stacked partly covered layers, a masked sphere whose far side shows
through, a masked emitter, a half-covered emitter and an unbound card with
`displayOpacity`. `cutout.png`'s alpha is an opaque disc of 24 texels, a
ring of 20 at 96 and 20 transparent corner texels, so its bilinear 0.5
contour is a rounded diamond. The
[scene reference](../reference/SCENE.md#material-fixtures) lists every
object.

`usdchecker` reports only the unresolvable `missing.png`, which the
Missing card names on purpose; it does not report the connection to
`opacityThreshold`, which the specification makes interface-only. The
reference was written with
`lotus-headless --write-material-reference validation/reference`; all five
fixtures took 11.3 seconds, and the rewritten references of the other four
are byte-identical to the committed ones.

## Audit

| Item | Contract | Evidence | Finding |
| --- | --- | --- | --- |
| Constant coverage | `opacity` in [0, 1]; threshold 0 accepts a surface with probability opacity, scaling its whole response | Half and Quarter show the backdrop through them in proportion; Zero leaves only the backdrop | As documented |
| `opacityMode` | Not read: presence coverage for every material | Presence's `presence` extracts to Half's material. Under the specification's default, `transparent`, opacity reduces only diffuse lighting, so Half's specular reflection would stay whole; Lotus scales it with coverage | As documented; the departure was already stated |
| Clamped and non-finite values | Constants clamped to [0, 1]; a non-finite value keeps the default | Clamped's 1.5 and 2 extract as 1 and 1 and accept at equality; NonFinite's −inf and inf keep 1 and 0, opaque | As documented |
| Thresholds | Opacity at or above a positive threshold accepts; below rejects | Below (0.4) vanishes, Equal (0.5) and Above (0.6) are fully opaque | As documented |
| Texture-driven threshold | Thresholds are constants | ThresholdMap's connection is reported and warns; the threshold keeps 0, so the card is half covered | As documented; the translator section now names `opacityThreshold` beside `ior` |
| Channel layouts | Scalar channels include alpha; two-channel images give (r, r, r, g), three-channel ones alpha 1 | Hio's texels of `rgba.png`, `cutout.png`, `grey-alpha.png`, `ramp.png` and the palette equal the mirror's; Alpha keeps the half where its alpha passes 0.5, GreyAlpha the upper half, Ramp the lower, and NoAlpha is opaque | As documented |
| sRGB images' alpha | `Rgba8Srgb` decodes colour, not alpha | Alpha and Cutout render as the mirror's linear alpha predicts; alpha decoded as sRGB is rejected (below) | As documented |
| Stochastic texture coverage, inversion | A lookup's value is the acceptance probability; scale and bias apply per channel | Fractional's ring shows the backdrop through it; Inverted's scale −1 and bias 1 on alpha cut a hole | As documented |
| `black` borders | The border is transparent black | BlackBorder's coordinates outside [0, 1] are rejected; the palette inside is opaque | As documented |
| Missing images | A key with no image returns its fallback | Missing renders the fallback colour at coverage 0.3 | As documented |
| Minified masks | No mipmaps | Minified's checker under threshold 0.5 matches the reference like every other card | As documented; per-sample aliasing as for the [minified checker](2026-10-27-material-textures.md#audit) |
| Layers and far sides | Each surface on a ray is tested independently; surfaces are two-sided | Stacked's overlap shows both layers; SphereCutout's far side shows through its holes | As documented |
| Masked emitters | NEE radiance is emission times coverage; shadow rays carry 1 − coverage | GlowingMask and GlowingHalf: the NEE/MIS images agree with the BSDF-sampled reference, max tile \|z\| 1.39 at 256 spp | As documented |
| Light-table estimate of covered emitters | Not stated | The weight ignores coverage, so GlowingMask's transparent corners keep their share of samples | **Documented**: costs variance only |
| `displayOpacity` | Not read (Hydra extraction) | DisplayOpacity's 0.3 is ignored without a warning and the card is opaque | As documented; the translator section now repeats it, and reading it is a candidate fix, see [next items](#next-items) |
| Not covered | Opacity edits, alpha with normal maps, depth/surface AOV realizations, UDIM and premultiplied alpha | Not authored | Edits are checked by `lotus-renderer-hydra-texture` ([report](2026-10-08-opacity.md#verification)); UDIM and premultiplied alpha are already listed as unsupported |

The only warnings are the unreadable `missing.png` and ThresholdMap's:
"Lotus uses the defaults of /MaterialAlpha/Looks/ThresholdMap inputs whose
shader graph it does not translate: opacityThreshold".

## Results

**Extraction.** UsdImaging's snapshot of the stage equals its mirror
exactly: materials, bindings, triangles, texture-coordinate sets, six
decoded textures (`rgba.png`, `cutout.png` and the palette under `sRGB`,
`grey-alpha.png` under `auto`, `ramp.png` and `checker.png` under `raw`),
lights and environment, and the unbound card's private `displayColor`
material. No transform, camera, position or normal set differs even by
rounding.

**Images against the reference** (sample index 2²⁰, 8-pixel tiles):

| Mode | spp | Image z (R, G, B) | Max tile \|z\| | Mean tile z² |
| --- | ---: | --- | ---: | ---: |
| Reference integrator, BSDF | 16 | 0.86, 0.19, −0.24 | 2.95 | 0.94 |
| Reference integrator, BSDF | 256 | −1.00, −0.26, −0.66 | 3.32 | 1.06 |
| Reference integrator, NEE/MIS | 16 | 0.56, 0.27, 0.21 | 1.26 | 0.07 |
| Reference integrator, NEE/MIS | 256 | 0.18, 0.25, 0.14 | 1.39 | 0.12 |
| Wavefront integrator, NEE/MIS | 16 | 0.56, 0.27, 0.21 | 1.26 | 0.07 |
| Wavefront integrator, NEE/MIS | 256 | 0.18, 0.25, 0.14 | 1.39 | 0.12 |
| Hydra, NEE/MIS | 256 | 0.18, 0.25, 0.14 | 1.39 | 0.12 |
| Hydra, BSDF | 256 | −1.00, −0.26, −0.66 | 3.32 | 1.06 |

The wavefront images are byte-identical to the reference integrator's, and
each Hydra image is byte-identical to the backend's image of the mirror.
The other four fixtures' rows are unchanged to the last digit.

**Power and reproducibility.** The 8-bit sRGB images' alpha decoded as
sRGB, which changes six objects, is rejected at max tile |z| 62.72 at
256 spp. Re-rendering the reference's own samples reproduces the committed
mean bit for bit on this device and build, for every fixture.

**Cost.** The headless post-build report, all checks included, took 56 s;
the Hydra CTests took 0.12 s (CPU) and 73.4 s (GPU, 61.7 s before). The
new fixture's share was not timed separately.

## Decision

Adopted: `material-alpha-v1`, `cutout.png` and its mirror. Constant and
textured coverage, thresholds, clamping, channel layouts, borders,
fallbacks, stacked layers and covered emitters need no renderer or
translator fix. The light table's disregard of coverage and the unread
`displayOpacity` of unbound meshes are now stated where a reader of the
light table and the translator looks. With this slice every item of the
audit order has a committed fixture.

## Next items

- Reading a constant `displayOpacity` into the unbound mesh's private
  material, as its `displayColor` is read, is a bounded candidate fix; it
  waits for an asset that needs it.
- The candidate fixes from the earlier slices are unchanged:
  [`UsdTransform2d`](2026-10-27-material-textures.md#next-items), the
  [constant normal's set](2026-10-27-material-normals.md#next-items) and
  [per-face materials](2026-10-27-material-emission.md#next-items).
  Prioritizing them is LOTUS-MATERIAL-01's remaining work.
- The specification's `transparent` mode and alpha blending need the
  explicit transport model the workstream policy requires; they are not
  candidates for this subset.

## Unavailable and unmeasured

- The reference is a finite 4096-spp estimate, not ground truth; agreement
  is statistical except where stated byte-identical.
- Bit-for-bit reference reproduction holds on this device, driver and
  build only.
- The variance cost of stochastic coverage, and of the light table's
  weight on covered emitters, was not isolated: the grid's lighting differs
  from cell to cell, so per-card variances are not comparable.
- Depth and surface diagnostics, which use a fixed coverage realization,
  were not rendered for the fixture.
- The viewport, usdview and the installed Hydra host were not run with the
  fixture. `ci-core` was not run; of the Hydra CTests only the two
  material-fixture CTests and the test-tier check ran, and of the core
  CTests the evidence, assets, install-tree and test-tier checks.
- Third-party production assets were not audited.
