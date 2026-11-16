# glTF PBR core

## Evaluation brief

LOTUS-MATERIALX-05 implements the
[glTF PBR contract](../design/GLTF_PBR.md): MaterialX's glTF PBR surface
with base colour, metallic, roughness, normal, emission and alpha, from the
Hydra network to the image, and the first composed closure in the
integrators. Adopt it if the closure is the node definition's, a white
surface reflects everything, the translator reads every input of the
definition or diagnoses it, `material-gltf-v1` is committed with references
under both integrators and agrees between Hydra and headless, and every
existing committed reference is unchanged
([acceptance](../design/GLTF_PBR.md#10-acceptance-for-lotus-materialx-05)).
The contract was implemented as proposed, before its review; its five
[points for review](../design/GLTF_PBR.md#11-points-for-review) and the
[changes found in implementation](#changes-to-the-contract) are open.

## What changed

- **Model.** `Lotus::GltfPbr` with `MaterialInput` inputs, in the 128-byte
  GPU record; validation, the light table's estimate and the readback
  round trip.
- **Closure.** [`gltf_pbr.slang`](../../backend/vulkan/shaders/bsdf/gltf_pbr.slang)
  builds the definition's stack from the composed closure.
  [`bsdf.slang`](../../backend/vulkan/shaders/bsdf/bsdf.slang) is the API
  alone and dispatches on the model; the preview closure moved, unchanged,
  to [`preview.slang`](../../backend/vulkan/shaders/bsdf/preview.slang).
  The composed closure is built in a `ComposedBuilder` that keeps what only
  the operations need.
- **GPU.** The albedo tables are a device-local buffer uploaded once and
  read through its address; `EvaluateGltfPbr` in the scene pass; the
  stored reservoir surface carries a model tag and glTF PBR's terms.
- **Translation.** A `GltfPbrReader` beside the Standard Surface reader,
  both on a shared `MaterialXReader`.
- **Evidence.** `material-gltf-v1` with its mirror and references,
  `gltf-stage-v1.usda` and `.mtlx`, `renderer.path.gltf_furnace`, the
  benchmark workload `cornell-gltf-v1`, and glTF PBR in `lotus-bsdf-test`
  and `lotus-renderer-hydra-material-graph`.

## Results

Windows 11, MSVC 14.51, OpenUSD 26.08 with MaterialX 1.39.5 from the
`lookdev` runtime, NVIDIA RTX A5000, Release.

| Check | Result |
| --- | --- |
| `lotus-bsdf-test` | Passes, 3,100 configurations: 180 of glTF PBR with a white furnace within 0.0024 of 1, no albedo above 1, the node definition's independent evaluation within 0.41% and the stored closure identical |
| `renderer.path.gltf_furnace` | 48 white planes under both integrators and both light samplings: largest deviation of a mean from 1, 0.0007 |
| `renderer.material.fixtures` | `material-gltf-v1` under BSDF sampling, NEE/MIS and the wavefront integrator at 16 and 256 spp; the wavefront images are the reference integrator's; the control, Packed's channels exchanged, rejected at a largest tile z of 75.00 |
| Existing references | `--write-reference` and `--write-material-reference` rewrote all 24 existing files byte-identically |
| Headless report | 37 checks pass |
| `lotus-renderer-hydra-material-graph` | Passes: glTF PBR's input table, alpha modes, the plugin's networks, its 29 inputs, the stage round trip; 213 node definitions and 602 defaults are the runtime's |
| `lotus-renderer-hydra-material-fixtures` | All 12 fixtures extract to their mirrors exactly; `material-gltf-v1` raises its five expected diagnostics and its pairs agree |
| `lotus-renderer-hydra-material-fixtures-gpu` | Every fixture is its mirror's image bit for bit under NEE/MIS and BSDF sampling and matches its reference; `material-gltf-v1` image z −0.41 −0.21 −0.12 and −0.66 0.32 0.48 |
| `lotus-renderer-restir-gpu` | Passes with the 144-byte stored surface: initial, temporal and spatial reservoirs under both integrators, and 640 and 800 bytes a pixel accounted |
| Script tests, `check_docs.py`, `material_fixtures.py check` | Pass |

Five deliberate defects in the closure each fail `lotus-bsdf-test`: the
layer's factor dropped (furnace 1.040), the metal uncompensated (furnace
0.988), F0 0.05 (oracle), the mix reversed (oracle) and a stored surface
without its Schlick share (stored closure).

Not run for this report: the full default suites of both configurations,
ReSTIR reuse over glTF PBR surfaces on a device (the stored closure is
tested on the CPU only), and the sample-asset inventory of [contract §8.6](../design/GLTF_PBR.md#86-the-sample-assets-not-a-gate).

## Cost of the second closure

The contract asked what a larger closure costs
([§6.2](../design/GLTF_PBR.md#62-bsdfclosure-with-two-representations)).
`slangc` inlines every call into the entry point, so the closure's size
shows as shader size.

| Shader | Before | Composed closure complete | As committed |
| --- | --- | --- | --- |
| `path_trace.frag.spv` | 929,348 B | 1,674,228 B | 1,458,536 B |
| `wavefront.comp.spv` | 566,896 B | 936,572 B | 784,892 B |

"As committed" compiles the composed closure for the scene passes with
three lobes and without dielectric lobes, lobe normals and attenuation,
which no implemented model builds. Renderer creation, for modules the
driver had not cached:

| Shaders | Validation off | Validation on |
| --- | --- | --- |
| Before | 7.5 s | 12.2 s |
| Composed closure complete | 15.6 s | 24.5 s |
| As committed | 7.5 s | 14.7 s |

GPU frame time on the fixed benchmarks at 1024², the lowest of five
interleaved runs' medians, new over old: the reference integrator 1.04 to
1.08 on five workloads and 1.35 on `geometry-v1`; the wavefront integrator
0.93 to 1.01 on five and 1.14 on `instanced-v1`. A workload's own runs
spread by up to a factor of 1.5 on this workstation, whose GPU clocks were
not locked, so these do not establish a regression or its absence;
LOTUS-MATERIALX-10 measures it at locked clocks. `cornell-gltf-v1` takes
3.85 ms against `cornell-v1`'s 1.62 ms under the reference integrator and
3.63 ms against 2.21 ms under the wavefront one: the glTF PBR closure's
table lookups and three-lobe evaluation, unoptimized.

The stored reservoir surface grows from 128 to 144 bytes: temporal reuse
from 608 to 640 bytes a pixel with readback, spatial from 768 to 800.

## Changes to the contract

Listed in the
[contract](../design/GLTF_PBR.md#12-changes-found-in-implementation),
awaiting review.

1. **The scene passes compile a reduced composed closure.** Beside
   `kMaxLobes` 3, they leave out dielectric lobes, lobe normals and
   attenuation, by a compile-time switch the CPU probe turns on. Without it
   renderer creation doubled, above.
2. **The furnace check renders planes**, not spheres: a plane's reflected
   rays never meet it again, so its mean has an exact answer.
3. **`StoreClosure` looks the two table values up again** rather than
   carrying them in the closure; `LoadClosure` reads no table, as
   contracted.
4. **The table data moved** to `backend/vulkan/bsdf-albedo-tables.inc`,
   beside the code that uploads it.
5. **`cornell-gltf-v1`** is a named benchmark workload rather than an
   ad-hoc copy.

## Limits

- The fixture's last row was reordered once: under BSDF sampling at 16 spp
  one 8-pixel tile of a red dielectric card differed from the reference by
  5.01 standard errors, against the limit of 5. The image's tiles had a
  mean z of 0.05 and a mean z² of 0.99, and the same tile reaches 3.4 in
  `material-graphs-v1`: the fixed sequence finds the rect light's
  reflection there. The roughest material now sits at that position.
- On this workstation the driver's shader cache accepted no new module
  during the work, so every renderer creation compiled its shaders, and
  `lotus-renderer-hydra-material-fixtures-gpu` took 376 s, beyond the
  300 s at which `ost test` stopped it; run directly it passes. With a
  working cache it took about 160 s before this task.
- `ior`, `specular`, `specular_color` and vertex colour are diagnosed, not
  translated, as the contract's points for review state.
