# Dielectric GGX and the specular workflow

Measured on 2026-10-08 in the working tree based on `32d5f47`, on Windows
x86_64 with MSVC 14.51, OpenStrata 0.23.14 and Release builds. The GPU was
an NVIDIA RTX A5000, Vulkan API 1.4.329. Hydra used OpenUSD 26.08 lookdev
and Python 3.13. All runs were local; hosted CI was not measured.

## Scope and model

The remaining material slice of
[Renderer Phase 1.5](../roadmap/README.md#status-at-a-glance):
constant `ior`, and `useSpecularWorkflow` with constant or RGB-textured
`specularColor`, consumed from the
[OpenUSD specification](https://openusd.org/release/spec_usdpreviewsurface.html).
The current contract belongs in the [scene reference](../reference/SCENE.md#path-tracing).

The material IR defaults to ior 1.5. Index matching (ior 1) removes the
dielectric interface and leaves uncoated Lambert. The specular workflow
ignores metallic and ior, using explicit F0 and a white grazing limit.
Fresnel attenuates diffuse light on entry and exit; reflection uses GGX
with correlated Smith masking, sampled by the existing visible-normal
sampler. View Fresnel and metallic determine lobe selection, and the same
probabilities form the mixture PDF used for the whole BSDF's weight.

This is a reciprocal single-scattering approximation, without internal
diffuse reflections, microfacet multiple-scattering compensation,
transmission, refraction or clearcoat. The existing metal approximation
keeps its white grazing limit. Ior textures are untranslated connections.
No target, dependency, pipeline or pass is added. The GPU material record
is 544 bytes, with seven texture inputs; upload validation, resolution
and readback include the new constants, workflow flag and lookup.

## Independent transport checks

`renderer.path.bsdf` compares a plane under a white environment with an
independent double-precision integral. GGX is integrated in half-vector
space with a 1024-by-1024 midpoint rule over its normal-distribution CDF,
rather than reproducing the shader's visible-normal sampling. The diffuse
coat integrates analytically: the cosine-weighted hemispherical mean of
`(1 - cos(theta))^5` is 1/21.

Orthographic views have outgoing cosines 1, 0.5 and 0.125. Each case uses
32 spp, first sample index 0, at 64x64, comparing only pixels whose samples
all hit: 131,072 samples at the first two angles and 73,728 at the grazing
angle after camera clipping. Means must agree within five standard errors
estimated from pixel means. Cases include black/white diffuse bases,
indices 1.5 and 2.5, roughness 0 and 1, coloured specular F0, F0 endpoints,
and metallic 1 ignored in the specular workflow. Expected furnace
integrals must remain at most one.

| Scene | Expected | Measured |
| --- | --- | --- |
| Black base, ior 1.5, near mirror, normal view | 0.0400 | 0.0404 |
| Black base, ior 1.5, near mirror, cos(view) 0.5 | 0.0700 | 0.0706 |
| Black base, ior 1.5, near mirror, cos(view) 0.125 | 0.5323 | 0.5333 |
| White base, ior 1.5, roughness 1, normal view | 0.8900 | 0.8898 |
| Black base, ior 2.5, roughness 0.5, normal view | 0.1682 | 0.1687 |
| White base, F0 `(0.7, 0.2, 0.05)`, normal view | `(0.7268, 0.7927, 0.9053)` | `(0.7268, 0.7922, 0.9046)` |

The existing exact Lambert, shading-normal, mirror, texture, opacity,
multi-bounce and accumulation checks remain. Their uncoated diffuse and
absorbing emissive fixtures now explicitly use ior 1, preserving the
analytic quantities they test. Pure-metal GGX weights cancel the sampling
density analytically to avoid mirror-density rounding.

## Inputs and edits

Core tests reject non-finite and out-of-range indices/F0 and invalid colour
lookup channels. A specular-only image resolves the seventh texture slot;
parameter edits rewrite only the material table. The scene-upload walk
reads back non-default ior, coloured F0, the workflow flag and a shared
specular lookup with scale.

`renderer.path.textures` checks specular reflection on a diffuse plane
against independent texel decoding and the transport integral, at 32 spp,
first sample index 0 and 64x64. It covers float and sRGB images, scale/bias,
range clamping, image replacement, an unscaled missing-image fallback,
and lookup overflow falling back to constant F0.

Hydra material CPU tests check defaults, clamping, non-finite inputs and
ignored metallic/ior. The GPU test uses 128 spp at 8x8, first sample index
0, with a composed USD plane and independent incoming-hemisphere
quadrature (65,536 midpoint samples) plus the analytic diffuse integral.
It checks default and display-colour coats, a live ior edit, workflow
switching and coloured-F0 edits without replacing geometry. The texture
tests check shared RGB translation, scalar-channel rejection, a composed
USD specular connection and release of an unused lookup on switching back
to metallic workflow, retaining the shared decoded image.

The existing scalar-opacity rejection test now edits the relationship's
upstream channel (`inputName`); its old `outputName` edit removed the
downstream connection instead of testing a wrong channel.

## Regenerated reference

`lotus-headless --write-reference validation/reference` regenerates the
128x128 Cornell mean at 1024 spp from sample index 0 and its per-sample
variance from 64 batches of 16 of those samples. Walls, the diffuse block
and the emitter now have the default dielectric coat. Both PFM files are
196,622 bytes.

| File | Minimum | Maximum | Mean over pixels and channels |
| --- | --- | --- | --- |
| `cornell-box-mean.pfm` | 0 | 15.1233 | 0.546231 |
| `cornell-box-variance.pfm` | 0 | 58.1396 | 1.418442 |

The independent sequence starting at 2^20 matches at 1, 16, 64, 256 and
1024 spp. Across these comparisons the largest tile discrepancy is 3.49
standard errors; the limit is five. A red wall 10% brighter is rejected
at 6.32 standard errors. Repeating the reference sequence reproduces the
committed mean bit for bit. Vulkan validation reports zero messages.

## Fault injections

Temporary shader copies were compiled into the core executable's shader
directory. The original SPIR-V was restored in `finally` and verified by
SHA-256; repository shader sources were unchanged. Both runs exited 1 and
failed `renderer.path.bsdf`:

| Fault | Detected result |
| --- | --- |
| Remove dielectric F0 and its grazing reflection | Half-metallic red mean 0.838311 instead of 0.797385, tolerance 0.003459 |
| Use metallic alone as the PDF's GGX mixture probability | Black near-mirror dielectric mean 145.693412 instead of 0.040000, tolerance 11.811270 |

## Commands and results

- `ost build --jobs auto`, `ost test`: **8/8 passed**;
  `ost validate --strict-renderer-evidence`: **passed**.
- `ost build --without-runtime --intent ci-core --jobs auto`,
  `ost test --without-runtime --intent ci-core`: **8/8 passed**, with
  explained GPU SKIPs.
- `ost build --profile lookdev --intent hydra --jobs auto`,
  `ost test --profile lookdev --intent hydra`: **23/23 passed**, including
  material/texture GPU checks and the usdview host test;
  `ost validate --profile lookdev --intent hydra --strict-renderer-evidence`:
  **passed**.

The runtime-free Ninja tree had objects with `#deps 0`; a header change
left an old extraction object and caused the scene-update test to crash.
Removing those objects within that build tree and rebuilding, following
the documented workaround, resolved it. Core and Hydra dependency records
were checked and had no zero-dependency objects to remove.
