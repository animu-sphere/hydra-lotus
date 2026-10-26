# Material textures fixture

## Evaluation brief

LOTUS-MATERIAL-01, second slice. The
[first slice](2026-10-26-material-basics.md) established committed
`UsdPreviewSurface` fixtures with independent C++ mirrors, reference images
and Hydra/headless agreement, and audited base colour and
metallic/roughness. This slice audits the third item of the
[workstream's order](../design/ROADMAP_POLICY.md#practical-foundation-workstreams),
textures and UV: wrap modes, colour spaces, texture-coordinate sources,
the untranslated `UsdTransform2d` and minification without mipmaps,
against the [material contract](../reference/SCENE.md#materials-and-environment)
and the [Hydra translator](../reference/SCENE.md#materials).

The mechanism now holds a list of fixtures, each with its own size and
negative control. The new fixture, `material-textures-v1`, is authored in
this repository as USD text and 8-bit PNG images written by
`scripts/material_fixtures.py` with the standard library only; the PNG
streams use stored deflate blocks, so the committed bytes depend on no
compressor version. The C++ mirror computes the same texels from the same
formulas, so the Hydra CTest checks Hio's decoding as well as the
translator.

Adopt the fixture if the stage extracts to the mirror (textures included),
every rendered mode matches the reference by the
[DES-Q5 metric](../reference/SCENE.md#reference-images), Hydra's images are
the mirror's bit for bit, and the metric rejects a half-texel shift on one
card. Shading normals and normal maps, emission and alpha masks are later
slices; the viewport, temporal/SVGF modes and performance are not covered.
This slice adds no shader path.

Conditions: Windows AMD64, MSVC 19.51 Release, NVIDIA RTX A5000, driver
597.16, Vulkan 1.4.329, validation and synchronization validation active,
clocks unlocked. Source `228a3e9` plus this change. The binaries' hashes,
the reference's and fixture files' hashes and every value are in the
[data](2026-10-27-material-textures.json).

## Fixture

`material-textures-v1` is a 5×5 grid of textured cards facing the camera,
under `material-basics-v1`'s camera, dome, rect key light and
`displayColor` backdrop, at 160×160. Its rows cover the wrap modes over
coordinates from −0.5 to 1.5; colour spaces and one-, two- and
three-channel images; scalar channels, scale and bias, an unresolvable file
and a texture without a file; texture-coordinate sources (per vertex,
indexed, a named second set, a constant `st` and a missing set); and four
connections the translator does not read, beside a 64×64 one-texel checker
minified about 2.4 times. The
[scene reference](../reference/SCENE.md#material-fixtures) lists every card
and image.

`usdchecker` reports only the deliberately missing
`./textures/missing.png`. The reference was written with
`lotus-headless --write-material-reference validation/reference`, which
now writes every fixture, each in a cleared GPU scene; both fixtures took
6.0 seconds. The rewritten `material-basics-v1` reference is byte-identical
to the committed one.

## Audit

| Item | Contract | Evidence | Finding |
| --- | --- | --- | --- |
| Wrap modes | `black`, `clamp`, `repeat`, `mirror`; `useMetadata`, the default, is black | Five wrap cards, including `repeat` with `black`, extract and render as the mirror | As documented |
| Colour spaces | `sRGB` and `raw` as authored; under `auto`, 8-bit three- and four-channel images are sRGB | The palette decodes as `Rgba8Srgb` under `sRGB` and `auto` and `Rgba8Unorm` under `raw`; the one- and two-channel images stay `Rgba8Unorm` under `auto` | As documented; the first committed evidence for one- and two-channel images under `auto` |
| Channel expansion | One channel (r, r, r, 1), two (r, r, r, g), three (r, g, b, 1); sRGB decodes colour, not alpha | Every decoded texel equals the mirror's, including the RGBA image's alpha under `sRGB` driving roughness | As documented |
| Scalar channels, scale and bias | `r`, `g`, `b`, `a` select a channel; `texel * scale + bias`, then the input's clamp | Orm reads `g` and `b`; ScaleBias's out-of-range results clamp in the shader | As documented |
| Missing image | The authored path when it does not resolve; a warning; the fallback, unscaled | Keyed `./textures/missing.png\|sRGB`, warns, renders its fallback | As documented |
| No file | A key that names no image; the fallback | Keyed `\|auto` | As documented |
| Texture-coordinate sources | A reader's `varname` and `fallback`, or an unconnected `st`; per-vertex and indexed primvars reach every corner | Vertex, Indexed, Named, Constant and Absent extract to the mirror's sets and fallbacks | As documented |
| Untranslated connections | Take their defaults and are reported as connected | `UsdTransform2d`, a second texture-coordinate source, a colour output on roughness and a scalar output on the base colour took their defaults, but the adapter never reported them | **Fixed**: the adapter now warns, naming the material and the inputs |
| Minification without mipmaps | Level 0, bilinear | See [minification](#minification) | No bias; more variance on that card |
| Not covered | UDIM tiles, wrap metadata, premultiplied alpha, 16-bit and float images, `UsdTransform2d` translation | Not authored | Unchanged; already listed as unsupported |

The translator's `connected_inputs` were recorded and unit-tested but never
surfaced, so an asset whose texture passes through `UsdTransform2d` lost
the texture without a diagnostic. The new warning appears once per
translation; it changes no extracted value or image.

## Results

**Extraction.** UsdImaging's snapshot of each stage equals its mirror
exactly: materials (texture keys taken relative to the fixtures'
directory), bindings, triangles, texture-coordinate sets, eight decoded
textures, lights and environment. No transform, camera, position or normal
set differs even by rounding.

**Images against the reference** (sample index 2²⁰, 8-pixel tiles):

| Mode | spp | Image z (R, G, B) | Max tile \|z\| | Mean tile z² |
| --- | ---: | --- | ---: | ---: |
| Reference integrator, BSDF | 16 | 0.65, 0.82, 0.22 | 3.59 | 1.03 |
| Reference integrator, BSDF | 256 | −0.65, −0.51, −0.40 | 3.55 | 1.14 |
| Reference integrator, NEE/MIS | 16 | 0.32, 0.37, 0.33 | 1.88 | 0.07 |
| Reference integrator, NEE/MIS | 256 | 0.22, 0.32, 0.28 | 1.44 | 0.12 |
| Wavefront integrator, NEE/MIS | 16 | 0.32, 0.37, 0.33 | 1.88 | 0.07 |
| Wavefront integrator, NEE/MIS | 256 | 0.22, 0.32, 0.28 | 1.44 | 0.12 |
| Hydra, NEE/MIS | 256 | 0.22, 0.32, 0.28 | 1.44 | 0.12 |
| Hydra, BSDF | 256 | −0.65, −0.51, −0.40 | 3.55 | 1.14 |

The wavefront images are byte-identical to the reference integrator's, and
each Hydra image is byte-identical to the backend's image of the mirror.
`material-basics-v1`'s rows are unchanged from the first slice to the last
digit.

**Power and reproducibility.** The Repeat card's coordinates half a texel
off in s are rejected at max tile |z| 17.46 at 256 spp. Re-rendering the
reference's own samples reproduces the committed mean bit for bit on this
device and build, for both fixtures.

### Minification

A paired control, not committed: the Minified card's checker replaced by
its prefiltered value, a constant albedo of 0.5, rendered with the
reference's own 4096 samples. Over the card's 361 interior pixels:

| Minified card | Mean | Per-sample variance |
| --- | ---: | ---: |
| `checker.png`, level 0 | 0.33862 | 0.2212 |
| Constant 0.5 | 0.33871 | 0.1894 |

The converged means agree within 0.03% (difference −0.00008, a standard
error of 0.00053 if the runs were independent; they share samples, so the
agreement is tighter than that suggests). Without mipmaps the pixel filter
integrates the footprint, so the converged image is unbiased; the cost is
16.8% more per-sample variance on that card under BSDF sampling, that is
about 17% more samples for the same noise there. Under NEE/MIS, whose
lighting noise is lower, the texture's share would be larger; at the
viewport's few samples per pixel it shows as aliasing. Neither was
measured.

**Cost.** The headless evidence CTest took 37.5 s and the Hydra CTests
0.08 s (CPU) and 27.4 s (GPU, 11.6 s before). The check renders about
5,100 spp at 160×160 for the new fixture; it was not timed separately.

## Decision

Adopted: `material-textures-v1`, the fixture list with per-fixture size and
negative control, and the adapter's warning for untranslated connections.
Wrap modes, colour spaces, channels and texture-coordinate sources need no
renderer or translator fix.

## Next items

- `UsdTransform2d` remains untranslated. Translating it needs a
  per-material coordinate transform in the material IR (consistent with the
  one-source rule), the GPU material table and the shader; it is a bounded
  candidate fix, to be prioritized against real assets before it is opened.
- Mipmaps are not proposed: the converged images are unbiased, and the
  interactive aliasing they would remove is unmeasured.
- Following the audit order: shading normals and normal maps, then
  emission and alpha masks, each as a fixture beside these.

## Unavailable and unmeasured

- The reference is a finite 4096-spp estimate, not ground truth; agreement
  is statistical except where stated byte-identical.
- Bit-for-bit reference reproduction holds on this device, driver and
  build only.
- The minification control shares the reference's samples, and its
  variance is per sample under BSDF sampling only.
- The viewport, usdview and the installed Hydra host were not run with the
  fixture. `ci-core` was not run; of the Hydra CTests only the material,
  texture, material-fixture and test-tier CTests ran, after a full build of
  the Hydra tree.
- Third-party production assets were not audited.
