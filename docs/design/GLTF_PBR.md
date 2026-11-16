---
status: proposed
owner: hydra-lotus
---

# glTF PBR core

> Status: **proposed**, 2026-11-15, awaiting review;
> [implemented](../reports/2026-11-16-gltf-pbr-core.md) as proposed, with
> the [changes found in implementation](#12-changes-found-in-implementation)
> also awaiting review. This is
> LOTUS-MATERIALX-05's contract, written before any code: the first
> MaterialX shading model, MaterialX's glTF PBR surface with its
> [P0 features](MATERIALX_SHADING_MODELS.md#phase-a--foundation-and-gltf-pbr),
> from the Hydra network to the image. It is the first task that wires the
> [composed closure](LAYERED_BSDF.md) into the integrators, so it also takes
> the three decisions earlier contracts left to it: how `BsdfClosure`
> carries more than one model ([BSDF API §3](BSDF_API.md#3-the-functions)),
> how the albedo tables reach the GPU
> ([layered BSDFs §5.6](LAYERED_BSDF.md#56-directional-albedo-tables)), and
> how the stored reservoir surface grows
> ([IR v2 §6.3](MATERIAL_IR_V2.md#63-what-stays-for-lotus-materialx-02)).
> The [scene reference](../reference/SCENE.md#materials-and-environment)
> owns the implemented behaviour. "Today" below describes `main` after
> LOTUS-MATERIALX-04.

## 1. Scope

`ND_gltf_pbr_surfaceshader` 2.0.1 with base colour, metallic, roughness,
normal, emission and alpha: its IR parameters, its GPU record, its closure,
its translation and diagnostics, its fixture, and the tests that hold it to
the node definition.

Not in this document: the P1 extensions — index of refraction, specular
weight and colour, transmission, clearcoat and sheen — which are
LOTUS-MATERIALX-06; anisotropy and iridescence (P2); volume and dispersion;
vertex colour (§5.4); and mixed-model performance, which
LOTUS-MATERIALX-10 measures.

## 2. Today

- `ShadingModel::GltfPbr` and an empty `GltfPbr` structure are reserved in
  [`material.hpp`](../../include/lotus/material.hpp). The render world and
  the GPU scene reject a material that has the tag.
- The translator raises `UnsupportedSurface` for a glTF PBR surface and
  leaves the default material; no graph below it is read.
- The lobes, the `mix` and `layer` operations and the albedo tables exist
  in [`lobes.slang`](../../backend/vulkan/shaders/bsdf/lobes.slang),
  [`composed.slang`](../../backend/vulkan/shaders/bsdf/composed.slang) and
  [`albedo.slang`](../../backend/vulkan/shaders/bsdf/albedo.slang), and only
  the CPU probe compiles them. No integrator imports them, the tables are
  bound nowhere on the GPU, and `BsdfClosure` holds the preview closure
  alone.
- `RestirSurface` packs the preview closure into 128 bytes and stores no
  model tag.
- The [node inventory](../reports/2026-11-15-materialx-graph-translation.md#gltf-sample-assets)
  of the Khronos sample assets found 11,776 glTF PBR surfaces whose graphs
  use nodes the translator's table holds, except the anisotropy direction
  (six assets) and vertex colour and alpha (seven and two assets).

## 3. What the node definition defines

The pinned definition's graph, with every P1 and P2 input at its default
(transmission 0, sheen colour black, clearcoat 0, iridescence 0,
anisotropy 0, `ior` 1.5, `specular` 1, `specular_color` white), reduces to:

```text
alpha      = clamp(roughness², 1e-5, 1)
dielectric = layer(top  = generalized_schlick(F0 = 0.04, F90 = 1, alpha),
                   base = oren_nayar_diffuse(base_color, roughness 0))
metal      = generalized_schlick(F0 = base_color, F90 = 1, alpha)
bsdf       = mix(bg = dielectric, fg = metal, mix = metallic)
edf        = uniform(emissive · emissive_strength)
opacity    = 1                         alpha_mode OPAQUE
           | alpha ≥ alpha_cutoff      alpha_mode MASK
           | alpha                     alpha_mode BLEND
```

`0.04` is `((1 − ior) / (1 + ior))²` at 1.5. The sheen and clearcoat
layers have weight 0 and the transmission mix selects the diffuse lobe, so
none of them appears. The `thickness`, `attenuation_distance`,
`attenuation_color` and `dispersion` inputs reach no node the surface
reads: the definition itself draws every material thin and without
dispersion.

## 4. IR parameters

```cpp
struct GltfPbr {
  MaterialInput<std::array<float, 3>> base_color{{1.0F, 1.0F, 1.0F}};
  MaterialInput<float> metallic{1.0F};
  MaterialInput<float> roughness{1.0F};
  MaterialInput<std::array<float, 3>> emissive{{0.0F, 0.0F, 0.0F}};
  float emissive_strength = 1.0F;
};
```

`MaterialInput<T>` is [IR v2 §4.2](MATERIAL_IR_V2.md#42-inputs-and-texture-expressions)'s
type, a constant and an optional lookup that replaces it; glTF PBR is the
first model to use it. The defaults are the node definition's, so a default
`GltfPbr` is a rough white metal, not `UsdPreviewSurface`'s grey
dielectric.

| Field | Range | Meaning |
| --- | --- | --- |
| `base_color` | each in [0, 1] | the diffuse lobe's albedo, and the metal's reflectance at normal incidence |
| `metallic` | [0, 1] | the `mix` weight between the dielectric stack and the metal |
| `roughness` | [0, 1] | both reflection lobes' roughness; alpha is its square |
| `emissive` | finite, ≥ 0 | emitted colour before the strength |
| `emissive_strength` | finite, ≥ 0 | a constant factor on `emissive` |

The inputs that act outside the BSDF are `MaterialGeometry`'s, as
[IR v2 §4.1](MATERIAL_IR_V2.md#41-tagged-models) decided:

| glTF PBR input | `MaterialGeometry` |
| --- | --- |
| `normal` | `normal`, `normal_texture` |
| `alpha`, `alpha_mode`, `alpha_cutoff` | `opacity`, `opacity_texture`, `opacity_threshold` (§5.3) |

`emissive` and `emissive_strength` stay two fields: the strength is a
uniform of the model, and keeping it apart keeps the fixture's mirror and
the readback in the definition's terms. Emission leaves both sides of a
surface, as every Lotus emitter does.

**Texture slots.** Slots 0 and 1 stay the geometry's. glTF PBR numbers its
own from 2: `base_color` 2, `metallic` 3, `roughness` 4, `emissive` 5.
Metallic and roughness from one packed image are two lookups of one
texture, each with its channel; sharing the sample is an optimization for
a measurement to ask for.

**GPU record.** The record stays 128 bytes. The model's 80-byte block
holds base colour and roughness, emissive and metallic, and the strength;
the rest is zero and is where LOTUS-MATERIALX-06's inputs go.

**Validation and the light table.** `ValidateMaterial` checks the ranges
above. `EmissionEstimate` is `emissive · emissive_strength`, by the same
texture estimate as `UsdPreviewSurface`'s emission.

## 5. Translation

The reader follows the [translation contract](MATERIALX_TRANSLATION.md):
each input is an authored constant or a graph the reader folds, and
everything else is diagnosed. It names all 29 inputs of the definition
([§6.3](MATERIALX_TRANSLATION.md#63-authored-inputs-the-table-does-not-name)).

### 5.1 Translated inputs

| IR field | Input | Rule |
| --- | --- | --- |
| `base_color` | `base_color` | each component clamped to [0, 1] |
| `metallic` | `metallic` | clamped to [0, 1] |
| `roughness` | `roughness` | clamped to [0, 1] |
| `emissive` | `emissive` | each component clamped to be non-negative |
| `emissive_strength` | `emissive_strength` | a constant clamped to be non-negative; a connection is `UnrepresentedInput` |
| `geometry.normal`, `normal_texture` | `normal` | an `ND_normalmap` of an image or of its default; `ND_gltf_normalmap` likewise. An authored constant, which is in world space, is `UnrepresentedInput`, as for Standard Surface |
| `geometry.opacity`, `opacity_texture`, `opacity_threshold` | `alpha`, `alpha_mode`, `alpha_cutoff` | §5.3 |

A clamp node before one of these inputs passes under the translation
contract's rule: only where it does nothing the input's own range does not.

### 5.2 Every other input

| Inputs | Outcome |
| --- | --- |
| `occlusion` | Without effect in a path tracer; its graph is not read, so an occlusion lookup on a second coordinate set raises nothing |
| `tangent` | Without effect while `anisotropy_strength` is 0 |
| `transmission`, `sheen_color`, `clearcoat`, `iridescence`, `anisotropy_strength` other than 0 or a lookup; `specular` other than 1; `specular_color` other than white; `ior` other than 1.5 | `UnrepresentedInput`. Transmission, sheen, clearcoat, `specular`, `specular_color` and `ior` are LOTUS-MATERIALX-06's; iridescence and anisotropy are P2 |
| `sheen_roughness`; `clearcoat_roughness`, `clearcoat_normal`; `iridescence_ior`, `iridescence_thickness`; `anisotropy_rotation` | Without effect while their lobe's weight is 0; that weight's diagnostic stands for them otherwise |
| `thickness` above 0, an authored `attenuation_distance`, `attenuation_color` other than white, `dispersion` above 0 | `UnrepresentedInput`. The pinned definition ignores them too (§3), but glTF gives them a meaning, so they are not dropped in silence |
| A parameter or connection the definition does not name | `UnknownInput` |

A material with one unrepresented input still renders with every input
that translates, as the [translation contract §6.2](MATERIALX_TRANSLATION.md#62-what-each-kind-does)
decided.

### 5.3 Alpha

glTF's alpha is coverage, and coverage is `MaterialGeometry`'s
([IR v2 §4.1](MATERIAL_IR_V2.md#41-tagged-models)):

| `alpha_mode` | Opacity | Threshold |
| --- | --- | --- |
| `OPAQUE` (0), the default | 1; `alpha` and its graph are not read, and are without effect | 0 |
| `MASK` (1) | `alpha`, clamped to [0, 1] | `alpha_cutoff`. The definition accepts `alpha ≥ alpha_cutoff`, which is the IR's rule |
| `BLEND` (2) | `alpha`, clamped to [0, 1] | 0: stochastic coverage, whose expectation is the definition's opacity |

Two cutoffs have no threshold in the IR, whose 0 means stochastic
coverage, and translate exactly to a constant instead: `MASK` with a
cutoff of 0 or less accepts every alpha and is opacity 1 without a lookup;
a cutoff above 1 accepts none and is opacity 0. `alpha_mode` and
`alpha_cutoff` are uniform: a connection on either, or a mode other than
0, 1 or 2, is `UnsupportedValue` and the surface is opaque.

### 5.4 Vertex colour is not translated

glTF multiplies base colour by `COLOR_0`, which
[usd-gltf-plugins](https://github.com/animu-sphere/usd-gltf-plugins)
authors as `ND_geompropvalue_color3` under an `ND_multiply_color3`, and
alpha by an `ND_geompropvalue_float`. The IR's `base_color_primvar`
*replaces* a colour input and loses to a lookup; a product of a colour set
with a factor and a lookup is a new value form in the graph reader and a
second meaning for that field. Seven of the 142 sample assets use the
colour and two the alpha. This task leaves both as they are after
LOTUS-MATERIALX-04: `UnsupportedNode`, the input at its default, with the
warning naming the node. The product joins the
[deferred adapter work](../roadmap/current.md#deferred-adapter-work) with
that count as its evidence.

## 6. The closure

### 6.1 Construction

A new shader module, `bsdf/gltf_pbr.slang`, builds §3's stack from the
[layered contract](LAYERED_BSDF.md)'s parts for the hit's `wo`:

```text
k0 = ComposedAdd(GgxSchlick(f0 = 0.04, f90 = 1, alpha, compensate))   top
k1 = ComposedAdd(Lambert(base_color))                                 base
     ComposedLayer(top = [k0, k1), base = [k1, k2))
k2 = ComposedAdd(GgxSchlick(f0 = base_color, f90 = 1, alpha, compensate))
     ComposedMix(bg = [k0, k2), fg = [k2, k3), metallic)
     ComposedFinalize
```

Three lobes, all about the base shading normal. The points where it reads
the definition:

- **Compensation is on** for both reflection lobes: MaterialX expects its
  microfacet lobes to preserve energy as roughness grows
  ([layered BSDFs §3](LAYERED_BSDF.md#3-what-the-specifications-define)).
  A white metal and a white dielectric stack therefore both pass a white
  furnace (§8).
- **The alpha floor is Lotus's**, `kMinAlpha` = 10⁻³, not the graph's
  10⁻⁵: roughness 0 is a near-mirror with a value and a density, never a
  delta, as for the preview closure
  ([BSDF API §5.4](BSDF_API.md#54-delta-components)). The difference is
  below roughness 0.032 and is listed in the compatibility table.
- **The diffuse lobe is Lambert**: `oren_nayar_diffuse_bsdf` at its
  default roughness 0.
- **One table lookup serves both reflection lobes.** They share alpha and
  normal, so `E1` and `Eschlick` are interpolated once per hit: 32 table
  reads for a glTF hit, none for a preview hit.

Sampling, the density and the weight are the composed closure's: one
choice with `u.z`, the mixture ratio, three random numbers
([layered BSDFs §5.5](LAYERED_BSDF.md#55-sampling)).

The builder is the function the CPU probe exports, so `lotus-bsdf-test`
runs the code the integrators run (§8).

### 6.2 `BsdfClosure` with two representations

[BSDF API §3](BSDF_API.md#3-the-functions) left open how several models
share one closure value: a field per model, or one payload.

**Decision: a field per representation.** All three MaterialX models build
a composed closure; only `UsdPreviewSurface` has its own. So:

```text
struct BsdfClosure {
  uint            model;     // the hit's ShadingModel
  PreviewClosure  preview;   // UsdPreviewSurface
  ComposedClosure composed;  // glTF PBR, and later Standard Surface, OpenPBR
}
```

The three functions switch on `model`; a later model adds a case that
calls the same composed functions, not a field. Two changes keep the value
small:

- **The closure carries only what the three functions read.** Each lobe's
  albedo and estimate are needed while the stack is built and are dropped
  at `ComposedFinalize`; they move out of `ComposedClosure` into the
  builder's state.
- **`kMaxLobes` is the largest list an implemented model builds**: 3 with
  this task. LOTUS-MATERIALX-06 and 08 raise it with their layers, each
  repeating this section's measurement.

`bsdf.slang` stays the API's module. The preview closure and the
microfacet helpers that `lobes.slang` reuses move into modules it imports,
so that the API can import the composed closure without a cycle; no
function changes.

**Measurement.** The closure is a local of the shading code, never path
state: the wavefront kernels store the hit and rebuild it. What a larger
value can cost is registers and shader size, which shows as time. The
report records, before and after, on the
[fixed benchmarks](../reference/SCENE.md#fixed-benchmarks) under both
integrators: GPU time, the SPIR-V size of `path_trace` and `wavefront`,
and, for a copy of `cornell-v1` whose materials are glTF PBR with the same
colours, the time against the preview original.

**Rejected: one payload with a union's reading.** Packing the preview
closure and the composed lobes into one array of words would bound the
value by the larger, not the sum, but every field access becomes an
unpack, and the preview path, which must stay byte-identical, would be
rewritten for it. It is the fallback if the measurement shows the sum
costs preview scenes time.

**Fallback rule.** A fixed benchmark that is all `UsdPreviewSurface` and
slows beyond the run-to-run spread of its own five-run median is a defect
of this task: it is fixed here, by the payload form or by building the
composed closure only on the model's branch, or it is recorded with its
cause for LOTUS-MATERIALX-10.

### 6.3 Albedo tables on the GPU

[Layered BSDFs §5.6](LAYERED_BSDF.md#56-directional-albedo-tables) decided
one read-only storage buffer, bound by this task.

- The renderer uploads the committed table data once, at creation: 75,776
  floats, 303 KB. It is not scene data and is never rewritten.
- The shaders reach it as the scene's other buffers are reached, by a
  device address in `PathConstants`, which grows by 8 bytes.
- `albedo.slang`'s `IAlbedoTables` gets its second implementation, over
  that address, beside the probe's. The interpolation code is shared, so
  the CPU test and the GPU read the same values up to floating-point
  contraction.

A scene without a glTF material reads no table. The buffer is created
regardless: 303 KB against a conditional path.

### 6.4 The stored reservoir surface

[BSDF API §6.3](BSDF_API.md#63-reservoir-targets-and-the-stored-surface)
stores the closure, packed per model, and
[IR v2 §6.3](MATERIAL_IR_V2.md#63-what-stays-for-lotus-materialx-02) has
this task add the tag.

- **The record grows from 128 to 144 bytes**: one more `float4`, holding
  the model tag and, for glTF PBR, three of its lanes. The preview packing
  uses every lane of today's record, so the tag has no place in it.
- **glTF PBR packs seven floats**: base colour, alpha, metallic, and the
  two table values at the stored `wo`, `E1` and the Schlick share. From
  those `LoadClosure` rebuilds the three lobes, their compensation, the
  layer's factor and the selection probabilities without reading a table,
  with the weights of the `wo` the surface was stored for
  ([layered BSDFs §5.2](LAYERED_BSDF.md#52-flattening)).
- **The preview packing is unchanged** in its lanes and its arithmetic, so
  ReSTIR's committed evidence for preview scenes stays as it is.

The report records the reservoir memory at the fixed benchmarks before and
after. If 16 bytes a pixel proves unacceptable, the alternative is to
derive the preview closure's `fresnel` lanes from its other fields and
free them, which risks its byte-identity and is therefore not the first
choice.

## 7. Scene pass and summary

`ReconstructHit`'s switch gains `GltfPbr → EvaluateGltfPbr`, which reads
the slots of §4, builds the shading normal exactly as `EvaluatePreview`
does, and calls §6.1's builder. The any-hit coverage test is unchanged: it
reads slot 0 and the geometry block and never the model.

The [surface summary](MATERIAL_IR_V2.md#62-what-leaves-the-material-besides-the-bsdf)
for glTF PBR:

| Field | Value |
| --- | --- |
| `albedo` | `base_color` |
| `specular_roughness` | `roughness` |
| `specular_share` | `metallic + 0.04 · (1 − metallic)` |
| Debug AOVs | `base_color`, `roughness`, `metallic` |

These are the preview closure's forms, which both models' principal lobes
share.

## 8. Tests

LOTUS-MATERIALX-09 runs from this stage on
([contract §8](MATERIALX_SHADING_MODELS.md#8-tasks)): the model is held to
something other than its own images.

### 8.1 The closure, on the CPU

`lotus-bsdf-test` gains the glTF PBR closure as a target, built by the
shader's own builder through the probe, over base colours white, grey and
saturated, metallic 0, 0.5 and 1, roughness 0, 0.1, 0.3, 0.6 and 1, and
[BSDF API §7.2](BSDF_API.md#72-configurations)'s outgoing directions.

1. **BSDF API §7.3's assertions**, with the mixture ratio required.
2. **White furnace.** With a white base colour the albedo is 1 at every
   metallic, roughness and `wo`, within the table tolerance of the
   [layered tests](LAYERED_BSDF.md#7-tests).
3. **Energy bound.** No configuration's albedo exceeds 1 beyond that
   tolerance.
4. **Definition oracle.** At random direction pairs the closure's value
   equals §3's expression evaluated by an independent implementation: C++
   in double precision, written from the node definition's graph and the
   MaterialX specification's lobe formulas, with the top lobe's
   directional albedo by its own quadrature rather than from Lotus's
   tables. The tolerance is the table error. This is the task's
   independent reference: it shares no code with the shader.
5. **Stored surface.** A closure packed by `StoreClosure` and rebuilt by
   `LoadClosure` evaluates and has the density of the original, on the
   reflection side, to rounding.

The run time on four cores is reported against today's 82 s.

### 8.2 Translation, without a device

In `lotus-renderer-hydra-material-graph`:

- **Input table.** Every row of §5.1 to §5.3 from a hand-built network,
  field by field, and every diagnosed input's kind.
- **Completeness.** An authored value on each of the definition's 29
  inputs yields a translation, a diagnostic or a stated absence of effect,
  never none of them.
- **Plugin shape.** The networks usd-gltf-plugins authors for base colour
  with alpha, packed metallic-roughness, a normal map and a texture
  transform, transcribed into the test, translate without a diagnostic.
- **Stage round trip.** `graph-stage-v1`'s pattern for glTF PBR: a
  committed `.usda` and a `.mtlx` it references give one material.

The definitions test already holds the surface's 29 defaults to the
runtime's.

### 8.3 The fixture

`material-gltf-v1`, generated by `material_fixtures.py`, mirrored in
`material_fixtures.cpp`, with references under both integrators and a
Hydra/headless comparison, as every material fixture.

| Row | Cards and spheres |
| --- | --- |
| Constants | The definition's defaults; dielectrics and metals over a roughness ramp; metallic 0.25 and 0.75 |
| Textures | sRGB base colour with a colour factor; packed metallic-roughness read from G and B; `ND_gltf_colorimage` with a rotation; the same material written in the plugin's primitive nodes |
| Normals | `ND_gltf_normalmap` and `ND_normalmap` of the tilts image, on a card and on a glossy metal sphere |
| Emission | A constant; an sRGB image; `emissive_strength` 4; an unlit-style black dielectric that only emits |
| Alpha | `OPAQUE` over an alpha below 1; `MASK` at cutoffs 0, 0.5 and above 1; `BLEND`; alpha from the base colour image's fourth channel |
| Diagnosed | `clearcoat` 1, `transmission` 1, `ior` 1.8 and a vertex-colour product, each rendering as §5 states |

Pairs that must agree — the compound node against its primitive graph,
the two normal-map nodes — are compared with each other in the test. Its
negative control reads metallic from G and roughness from B, the packed
image's channels exchanged.

### 8.4 Rendered furnace

A headless check, `renderer.path.gltf_furnace`: white glTF PBR spheres at
metallic 0 and 1 and roughness 0.2 and 1 in a constant white environment,
under both integrators and both light-sampling modes. Every sphere's mean
radiance equals the environment's within the
[reference tolerance](../reference/SCENE.md#reference-images) and the
table error. It is the one test that crosses the GPU table binding, the
scene pass and the integrators with a known answer.

### 8.5 Regression

Every committed reference and fixture image is unchanged, reference and
wavefront, Hydra and headless, the ReSTIR evidence included.

### 8.6 The sample assets, not a gate

The inventory of the 04 report is repeated with the surface in place: of
the 11,776 materials, how many translate without a diagnostic and how
many raise each kind, by input. It uses a workstation build of the plugin
and local assets, so it is reported and decides nothing here; it is the
evidence LOTUS-MATERIALX-06 and the deferred work are ordered by.

## 9. Rejected alternatives in one place

| Alternative | Why not |
| --- | --- |
| Reading `ior`, `specular` and `specular_color` now, since the builder already takes an F0 | The contract orders them P1; a coloured top lobe also raises how the layer's factor is averaged, which belongs with its fixture in 06 (§5.2). |
| Translating vertex colour now | A new value form and a second meaning for `base_color_primvar`, for seven sample assets (§5.4). |
| Folding `emissive_strength` into `emissive` | The mirror and the readback would hold a product the definition does not name (§4). |
| Treating `thickness`, the attenuation inputs and `dispersion` as without effect, as the definition does | glTF gives them meaning; silence would render as if supported (§5.2). |
| One payload for every closure | Rewrites the preview path that must stay byte-identical (§6.2). |
| A tag hidden in the preview packing's lanes | No lane is free; freeing one changes the preview arithmetic (§6.4). |
| Recomputing a stored glTF closure from the tables | 32 table reads for every temporal and spatial neighbour (§6.4). |
| Comparing against Storm's image as the independent reference | Storm's lighting is a rasterizer's approximation; the closure's value against the definition is exact and needs no device (§8.1). |

## 10. Acceptance for LOTUS-MATERIALX-05

1. `GltfPbr` holds §4's parameters; the render world, the GPU scene and
   the readback accept and round-trip it in the 128-byte record.
2. The translator reads `ND_gltf_pbr_surfaceshader` as §5 states, and
   §8.2's tests pass.
3. Both integrators shade glTF PBR through `BsdfEvaluate`, `BsdfPdf` and
   `BsdfSample` with §6.1's closure; `lotus-bsdf-test` passes §8.1 in the
   `fast` tier.
4. `material-gltf-v1` is committed with references under both integrators,
   Hydra and headless agree on it, and `renderer.path.gltf_furnace`
   passes.
5. Every existing committed reference and fixture is unchanged.
6. The report records §6.2's measurements and its outcome, the reservoir
   record's growth and memory, the test's run time, and §8.6's inventory.
7. The scene reference documents the model, and the compatibility table
   starts with glTF PBR's rows: supported, partial (the alpha floor) and
   unsupported inputs.

## 11. Points for review

1. **`ior`, `specular` and `specular_color` wait for 06** (§5.2). Until
   then a glTF material that authors them — every `KHR_materials_unlit`
   material the plugin writes sets `specular` 0 — renders with the default
   specular lobe and a warning.
2. **Vertex colour is diagnosed, not translated** (§5.4).
3. **The reservoir record grows by 16 bytes a pixel** (§6.4) rather than
   re-deriving preview lanes.
4. **The independent reference is a CPU oracle of the node definition**
   (§8.1), not another renderer's image.
5. **The alpha floor stays 10⁻³** (§6.1), a stated difference from the
   definition's 10⁻⁵.

## 12. Changes found in implementation

LOTUS-MATERIALX-05's implementation departs from the text above in these
points, which await review with §11; the
[report](../reports/2026-11-16-gltf-pbr-core.md#changes-to-the-contract)
gives the measurements.

1. **The scene passes compile a reduced composed closure.** §6.2 sized the
   closure by `kMaxLobes` alone. `slangc` inlines every call, so the whole
   composed closure was compiled at each evaluation, and renderer creation
   took twice as long. The scene passes therefore also leave out dielectric
   lobes, lobe normals and attenuation, which no implemented model builds,
   by a compile-time switch (`LOTUS_COMPOSED_FULL`) that the CPU probe
   turns on with six lobes. For a glTF PBR closure the two builds compute
   the same values; LOTUS-MATERIALX-06 turns on what its layers need.
2. **§8.4's furnace renders planes**, not spheres, whose reflected rays
   never meet them again.
3. **`StoreClosure` looks the two table values up again** (§6.4) rather
   than carrying them in the closure; `LoadClosure` reads no table.
4. **The table data is `backend/vulkan/bsdf-albedo-tables.inc`** (§6.3),
   moved from `validation/`.
5. **§6.2's glTF copy of `cornell-v1` is the benchmark workload
   `cornell-gltf-v1`.**
6. **§8.6's sample-asset inventory was not run.**
