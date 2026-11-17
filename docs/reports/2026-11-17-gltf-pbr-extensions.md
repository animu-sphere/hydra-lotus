# glTF PBR extensions

## Evaluation brief

LOTUS-MATERIALX-06 implements the
[glTF PBR extensions contract](../design/GLTF_PBR_EXTENSIONS.md): the index
of refraction, the specular weight and colour, transmission, clearcoat and
sheen of MaterialX's glTF PBR surface, from the Hydra network to the image.
Adopt it if the larger closure is the node definition's in the readings the
contract states, a white surface still returns everything, a material at
the extensions' defaults is the core's, the translator reads the nine
inputs, `material-gltf-extensions-v1` is committed with references under
both integrators and agrees between Hydra and headless, and every reference
that holds no glTF material is unchanged
([acceptance](../design/GLTF_PBR_EXTENSIONS.md#9-acceptance-for-lotus-materialx-06)).
Seven [changes found in implementation](#changes-to-the-contract) were
accepted in review, 2026-11-17.

## What changed

- **Model.** `Lotus::GltfPbr` gains `ior`, `specular`, `specular_color`,
  `transmission`, `clearcoat`, `clearcoat_roughness`,
  `clearcoat_normal_texture`, `sheen_color` and `sheen_roughness`, with
  texture slots 6 to 13. The GPU material record is 144 bytes.
- **Lobes.** [`lobes.slang`](../../backend/vulkan/shaders/bsdf/lobes.slang)
  has a fourth lobe, the sheen, and a `transmit` flag that mirrors a
  `GgxSchlick` lobe through the tangent plane.
  [`albedo.slang`](../../backend/vulkan/shaders/bsdf/albedo.slang) has a
  fourth table, `Esheen`; the committed data grows from 75,776 to 76,800
  floats, the first 75,776 regenerated identically.
- **Closure.** [`gltf_pbr.slang`](../../backend/vulkan/shaders/bsdf/gltf_pbr.slang)
  reduces a hit's inputs to 22 terms and builds up to six lobes from them;
  [`composed.slang`](../../backend/vulkan/shaders/bsdf/composed.slang) gains
  the scalar layer rule and lobes whose weight and albedo the model
  supplies.
- **GPU.** `EvaluateGltfPbr` reads the new slots and builds the coat's
  normal; the stored reservoir surface is 160 bytes and keeps the 22 terms.
- **Translation.** The `GltfPbrReader` reads the nine inputs; a normal map
  reaches `clearcoat_normal` as it reaches `normal`.
- **Evidence.** `material-gltf-extensions-v1` with its mirror and
  references, 72 more planes in `renderer.path.gltf_furnace`, and the
  extensions in `lotus-bsdf-test` and `lotus-renderer-hydra-material-graph`.

## Results

Windows 11, MSVC 14.51, OpenUSD 26.08 with MaterialX 1.39.5 from the
`lookdev` runtime, NVIDIA RTX A5000, Release.

| Check | Result |
| --- | --- |
| `lotus-bsdf-test` | Passes, 3,680 configurations: 612 of glTF PBR, 432 of them with an extension. White furnace within 0.0070 of 1, no albedo above 1.0039, the node definition's independent evaluation within 0.39%, the stored closure identical, and the 180 configurations at the defaults bit for bit the core's closure with at most three lobes |
| The new lobes | The sheen alone, 40 configurations: albedo at most 1.0029 with its normalization. The mirrored lobe, 40: white and compensated within 0.0056 of 1, reciprocal uncompensated. A sheen over a white base, 20: within 0.0081 of 1 |
| `Esheen` | 61 nodes reproduce within 4.8·10⁻⁶; 193 cell centres within 7.9·10⁻⁴ of a fresh quadrature, relative to the value where it exceeds 1 |
| `renderer.path.gltf_furnace` | 120 white planes under both integrators and both light samplings, the transmitting ones included: largest deviation of a mean from 1, 0.0013 |
| `renderer.material.fixtures` | `material-gltf-extensions-v1` under BSDF sampling, NEE/MIS and the wavefront integrator; its control, CoatRough's weight and roughness exchanged, rejected |
| References without a glTF material | `--write-reference` and `--write-material-reference` rewrote all 24 byte-identically |
| `material-gltf-v1` | Its reference changes in the Unlit card, which loses its reflection lobe: 18 standard errors there, at most 0.2 in each of the other 24 cells |
| Headless report | 31 checks pass, 6 skip without OpenUSD |
| `lotus-renderer-hydra-material-graph` | Passes: the nine inputs' constants, clamps and lookups, a lobe's inputs read only with its weight, the constant clearcoat normal and the thickness diagnosed, the plugin's unlit material clean, and the definition's 29 inputs each translated, diagnosed or without effect |
| `lotus-renderer-hydra-material-fixtures` | All 13 fixtures extract to their mirrors exactly, the two glTF ones included |
| `lotus-renderer-hydra-material-fixtures-gpu` | Every fixture is its mirror's image bit for bit under NEE/MIS and BSDF sampling and matches its reference; `material-gltf-extensions-v1` image z −0.28 −0.50 −0.42 and −0.36 −0.98 −1.28 |
| Core CTests run alone | `lotus-renderer-scene-mesh`, `-scene-update`, `-evidence`, `-temporal-gpu`, `-memory-ranges` and `lotus-test-tiers` pass |
| Script tests, `check_docs.py`, `material_fixtures.py check` | Pass |

Seven deliberate defects in the closure each fail `lotus-bsdf-test`:
transmission left on the reflection side (72 configurations), the
dielectric's layer per channel (72), a sheen that does not dim its base
(163), the coat about the base normal (112), the coat uncompensated (13),
the specular weight missing from `f90` (48), and a stored surface without
its sheen albedo (96).

`lotus-bsdf-test` takes 146 s on four cores, against 82 s before, and 20 s
on 32 threads.

Not run for this report: the full default suites of both configurations,
`lotus-renderer-restir-gpu`, whose memory constants follow the 160-byte
surface, ReSTIR reuse over glTF PBR surfaces on a device, locked-clock
frame timings, and the sample-asset inventory.

## Cost of the larger closure

| Shader | LOTUS-MATERIALX-05 | This task |
| --- | --- | --- |
| `path_trace.frag.spv` | 1,458,536 B | 2,132,032 B |
| `wavefront.comp.spv` | 784,892 B | 1,099,644 B |

`slangc` inlines every call, so six lobes, the sheen, the dielectric's
reflection and lobe normals show as size: 46% and 40% more. Renderer
creation time, which the contract's
[fallback rule](../design/GLTF_PBR_EXTENSIONS.md#66-what-the-scene-passes-compile)
is stated in, was not measured, so the fixed-position closure is neither
adopted nor ruled out; LOTUS-MATERIALX-10 measures it with the frame
times.

The GPU material record grows from 128 to 144 bytes for every model, and
the stored reservoir surface from 144 to 160: temporal reuse from 640 to
672 bytes a pixel with readback, spatial from 800 to 832.

## Changes to the contract

Listed in the
[contract](../design/GLTF_PBR_EXTENSIONS.md#11-changes-found-in-implementation),
accepted in review, 2026-11-17.

1. **The sheen is normalized where its albedo exceeds 1.** The lobe's
   quadrature gives 5.59 at the smallest cosine and roughness of the
   table, and more than 1 below an outgoing cosine of 0.066 to 0.089 for
   lobe roughnesses from 0.005 to 0.031, 0.047 at 0.073 and 0.011 at
   0.158; from 0.3 up it never does. There the closure scales the lobe by
   the inverse of the table value.
2. **A reflecting lobe scatters into the shading normal's hemisphere
   only.** A coat tilted by 45° at normal incidence loses 0.051 when
   smooth and 0.027 at roughness 0.3; at 15° nothing measurable.
3. **One compile-time switch** remains, for what only the probe builds.
4. **The stored surface keeps the closure's 22 terms**, carried with the
   hit.
5. **The dielectric's reflectance at normal incidence is computed on the
   host** in double precision. In single precision `((1 − 1.5) / 2.5)²` is
   one unit in the last place above 0.04, which would have changed every
   glTF PBR image.
6. **The fallback rule was not exercised**, above.
7. **The sample-asset inventory was not run.**

## Limits

- On this host Ninja records no header dependencies for most objects, so
  after the material header changed a stale test binary crashed until the
  objects without dependencies were deleted in each build tree.
- `material-gltf-v1`'s other cards are not byte-identical: paths that
  reach the Unlit card continue differently. The comparison above is by
  cell, in the reference's own standard errors.
