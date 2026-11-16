---
status: accepted
owner: hydra-lotus
---

# glTF PBR extensions

> Status: **accepted**, 2026-11-16, as written, and
> [implemented](../reports/2026-11-17-gltf-pbr-extensions.md) with the
> [changes found in implementation](#11-changes-found-in-implementation),
> which await review. This is
> LOTUS-MATERIALX-06's contract, written before any code: the
> [P1 features](MATERIALX_SHADING_MODELS.md#phase-a--foundation-and-gltf-pbr)
> of MaterialX's glTF PBR surface — index of refraction, specular weight
> and colour, transmission, clearcoat and sheen — on top of the
> [glTF PBR core](GLTF_PBR.md). It is the first task to stack more than one
> layer, to give a lobe a normal of its own and to add a lobe the
> [layered contract](LAYERED_BSDF.md) left to its model, so it also decides
> how a coloured top scales what lies below it. The
> [scene reference](../reference/SCENE.md#gltf-pbr) owns the implemented
> behaviour. "Today" below describes `main` after LOTUS-MATERIALX-05.

## 1. Scope

`ND_gltf_pbr_surfaceshader` 2.0.1's `ior`, `specular`, `specular_color`,
`transmission`, `clearcoat`, `clearcoat_roughness`, `clearcoat_normal`,
`sheen_color` and `sheen_roughness`: their IR parameters, GPU record and
slots, the lobes and layers they add to the closure, their translation,
their fixture and their tests.

Not in this document: iridescence and anisotropy (P2); volume — `thickness`
and the attenuation inputs — and dispersion, which stay diagnosed
([core §5.2](GLTF_PBR.md#52-every-other-input)); vertex colour
([core §5.4](GLTF_PBR.md#54-vertex-colour-is-not-translated)); and
mixed-model performance, which LOTUS-MATERIALX-10 measures.

## 2. Today

- `GltfPbr` holds the core's five parameters in a 128-byte GPU record, with
  texture slots 2 to 5. The translator raises `UnrepresentedInput` for each
  of this task's inputs away from its default, and the material renders as
  if it were at the default.
- The closure is three lobes about the shading normal:
  `mix(layer(GgxSchlick(0.04), Lambert), GgxSchlick(base_color), metallic)`.
- The scene passes compile a reduced composed closure: three lobes, no
  dielectric lobe, no lobe normals, no attenuation
  ([core §12.1](GLTF_PBR.md#12-changes-found-in-implementation)). The CPU
  probe compiles all of it with six lobes.
- `ComposedLayer` scales the base by `1 − E_top(wo)` per channel. Every top
  built so far is grey, so no image depends on that choice.
- There is no sheen lobe and no sheen albedo table
  ([layered BSDFs §1](LAYERED_BSDF.md#1-scope)).
- The stored reservoir surface is 144 bytes and packs seven floats for a
  glTF PBR closure.

## 3. What the node definition defines

The pinned definition's graph, with iridescence and anisotropy at 0:

```text
alpha      = clamp(roughness², 1e-5, 1)
f0         = min(specular_color · ((1 − ior) / (1 + ior))², 1) · specular
f90        = specular
dielectric = layer(top  = generalized_schlick(f0, f90, alpha),
                   base = mix(bg = oren_nayar_diffuse(base_color),
                              fg = dielectric_bsdf(T, tint = base_color,
                                                   ior, alpha),
                              mix = transmission))
metal      = generalized_schlick(F0 = base_color, F90 = 1, alpha)
base       = mix(bg = dielectric, fg = metal, mix = metallic)
sheen      = layer(top  = sheen_bsdf(weight = max(sheen_color),
                                     color  = sheen_color / max(sheen_color),
                                     roughness = sheen_roughness²),
                   base = base)
bsdf       = layer(top  = dielectric_bsdf(R, weight = clearcoat, ior = 1.5,
                                          alpha = clearcoat_roughness²,
                                          normal = clearcoat_normal),
                   base = sheen)
```

Emission and opacity are the core's: the `surface` node takes the EDF beside
the BSDF, so neither the coat nor the sheen dims emission. `sheen_bsdf` is
in its default `conty_kulla` mode. `clearcoat_normal` defaults to the
geometry's normal, not to `normal`: a clearcoat over a normal-mapped base
is smooth unless it has a map of its own.

glTF's own specification of the same features
([KHR_materials_transmission](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_transmission),
[KHR_materials_specular](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_specular))
differs from this graph in two places, which §6 decides: transmission
without the volume extension is a thin surface that does not refract and
has no Fresnel term of its own, where the graph's `dielectric_bsdf` carries
`ior` and its own `1 − F`; and a coloured specular lobe dims the base by
the largest channel of its reflectance, where MaterialX's `layer` leaves
the rule to the implementation.

## 4. IR parameters

`GltfPbr` gains, with the node definition's defaults:

```cpp
float ior = 1.5F;
MaterialInput<float> specular{1.0F};
MaterialInput<std::array<float, 3>> specular_color{{1.0F, 1.0F, 1.0F}};
MaterialInput<float> transmission{0.0F};
MaterialInput<float> clearcoat{0.0F};
MaterialInput<float> clearcoat_roughness{0.0F};
std::optional<TextureInput> clearcoat_normal_texture;
MaterialInput<std::array<float, 3>> sheen_color{{0.0F, 0.0F, 0.0F}};
MaterialInput<float> sheen_roughness{0.0F};
```

| Field | Range | Meaning |
| --- | --- | --- |
| `ior` | finite, ≥ 1 | the dielectric's index: its reflectance at normal incidence. A uniform of the definition, so a constant |
| `specular` | [0, 1] | the dielectric reflection's weight, on `f0` and `f90` alike |
| `specular_color` | finite, ≥ 0 | a factor on `f0`, which is clamped to 1 after it |
| `transmission` | [0, 1] | the share of the dielectric's base that transmits instead of scattering diffusely |
| `clearcoat` | [0, 1] | the coat's weight |
| `clearcoat_roughness` | [0, 1] | the coat's roughness; alpha is its square |
| `clearcoat_normal_texture` | — | a tangent-space lookup for the coat's normal; without one the coat lies about the mesh's shading normal, before `geometry.normal` (§3) |
| `sheen_color` | each in [0, 1] | the sheen's colour; its largest channel is the sheen's weight |
| `sheen_roughness` | [0, 1] | the sheen's roughness; the lobe's is its square |

A defaulted `GltfPbr` is the core's material: every field above is at the
value the core's closure assumed.

**Texture slots.** `specular` 6, `specular_color` 7, `transmission` 8,
`clearcoat` 9, `clearcoat_roughness` 10, `clearcoat_normal` 11,
`sheen_color` 12, `sheen_roughness` 13. `kMaterialTextureInputs` grows from
7 to 14; the header's 64-bit mask already has room
([IR v2 §5.2](MATERIAL_IR_V2.md#52-lookups-out-of-the-record)).

**GPU record.** The model's constants are now 21 floats, one more than the
80-byte block holds. The record's stride grows from 128 to 144 bytes, as
[IR v2 §5.1](MATERIAL_IR_V2.md#51-one-fixed-stride-record) provides for: the
stride is the largest model's block. A `UsdPreviewSurface` record gains 16
zero bytes. The report records the material table's size at the
[fixed benchmarks](../reference/SCENE.md#fixed-benchmarks) before and after.

**Validation and the light table.** `ValidateMaterial` checks the ranges
above. `EmissionEstimate` is unchanged: no layer dims emission.

## 5. Translation

The reader follows the core's rules
([core §5](GLTF_PBR.md#5-translation)); these rows replace the
corresponding ones of its "every other input" table.

| IR field | Input | Rule |
| --- | --- | --- |
| `ior` | `ior` | a constant, clamped to at least 1; a connection is `UnrepresentedInput` and the index stays 1.5 |
| `specular` | `specular` | clamped to [0, 1] |
| `specular_color` | `specular_color` | each component clamped to be non-negative |
| `transmission` | `transmission` | clamped to [0, 1] |
| `clearcoat`, `clearcoat_roughness` | the same | clamped to [0, 1] |
| `clearcoat_normal_texture` | `clearcoat_normal` | an `ND_normalmap` or `ND_gltf_normalmap` of an image. An authored constant, which is in world space, is `UnrepresentedInput`, as for `normal`. Read only while `clearcoat` is not a constant 0 |
| `sheen_color` | `sheen_color` | each component clamped to [0, 1] |
| `sheen_roughness` | `sheen_roughness` | clamped to [0, 1]. Read only while `sheen_color` is not a constant black |

The translation contract's one-coordinate-set rule stands: a clearcoat
normal map on a second set is diagnosed as any other lookup on one is.

Still `UnrepresentedInput`, with the material rendering every input that
translates: `iridescence` and `anisotropy_strength` other than 0;
`thickness` above 0, an authored `attenuation_distance`, an
`attenuation_color` other than white and `dispersion` above 0. A material
with `thickness` above 0 and positive `transmission` therefore renders as
the thin surface of §6.2 and warns: refraction through a solid is the
volume extension's, with its attenuation.

A `KHR_materials_unlit` material as usd-gltf-plugins writes it — black
base, `specular` 0, emissive — now renders without a reflection lobe and
without a warning.

## 6. The closure

### 6.1 Construction

`gltf_pbr.slang`'s builder becomes, for the hit's `wo`, with `N` the base
shading normal and `Nc` the coat's:

```text
k0 = Add(GgxDielectric(R, eta 1.5, coat_alpha, Nc, compensate)); Scale(clearcoat)   coat
k1 = Add(Sheen(sheen_color / s, sheen_alpha, N)); Scale(s)        s = max(sheen_color)
k2 = Add(GgxSchlick(f0, f90, alpha, N, compensate))               dielectric reflection
k3 = Add(Lambert(base_color, N))
k4 = Add(GgxSchlick(1, 1, alpha, N, compensate, transmit)); Scale(base_color)
     Mix(bg = [k3, k4), fg = [k4, k5), transmission)
     Layer(top = [k2, k3), base = [k3, k5), scalar)               §6.3
k5 = Add(GgxSchlick(base_color, 1, alpha, N, compensate))         metal
     Mix(bg = [k2, k5), fg = [k5, k6), metallic)
     Layer(top = [k1, k2), base = [k2, k6))
     Layer(top = [k0, k1), base = [k1, k6))
     Finalize
```

Six lobes at most, the list the CPU probe already compiles. A lobe whose
weight is 0 at the hit — no clearcoat, black sheen, `transmission` 0 or 1,
`metallic` 0 or 1, `specular` 0 — is not added, so a material at the
extensions' defaults builds the core's three lobes with the core's weights
and reads the core's two table values.

- **The alpha floor** is `kMinAlpha` for the coat as for every other GGX
  lobe of the model
  ([core §6.1](GLTF_PBR.md#61-construction)): a clearcoat of roughness 0 is
  a near-mirror with a value and a density, not the delta
  [layered BSDFs §5.1](LAYERED_BSDF.md#51-primitive-lobes) makes of a
  smooth dielectric. Next event estimation then sees the coat's highlight,
  and no composed closure in the scene passes has a delta component.
- **The coat's Fresnel is the dielectric's**, exact, at index 1.5, with
  the layered contract's compensation and `Edielectric` table, entering
  sheet only: a coat is never seen from inside a solid.
- **Table reads** are per layer present: 32 for the base as today, 16 more
  for a sheen, 80 more for a coat (its albedo and its compensation).

### 6.2 Transmission is thin

**Decision.** Without the volume extension a glTF surface encloses no
medium, so the transmitted share is a thin surface, as glTF specifies it:
the base's reflection lobe mirrored through the tangent plane, tinted by
`base_color`, with no Fresnel term of its own and no dependence on `ior`.

```text
f_t(wo, wi) = base_color · f_ggx(wo, (wi.x, wi.y, −wi.z); F = 1, alpha)    wi.z < 0
```

`GgxSchlick` gains a `transmit` flag for it: the same value, density and
sampler, the sampled direction mirrored. The lobe is compensated like any
other, so with `f0 = f90 = 1` its albedo is exactly 1, and the dielectric
stack reflects `E_top` and passes `(1 − E_top) · base_color` at every
roughness: a white transmitting surface passes the furnace.

- **The integrators are unchanged.** The
  [transmission contract](TRANSMISSION.md#6-direct-light-and-mis)'s rules
  hold for this lobe as for the preview closure's thin wall: next event
  estimation asks for the reflection side, the light a transmitted
  direction finds is added unweighted, and shadow rays treat the surface as
  opaque ([§7](TRANSMISSION.md#7-shadow-rays-and-the-light-table)).
- **No side is tracked.** A thin surface has no inside
  ([transmission §3](TRANSMISSION.md#3-sides-of-the-interface)), so an open
  glTF mesh — a pane, a leaf, a lampshade — is well defined.
- **The scene passes need no refraction.** The dielectric lobe they compile
  for the coat is its reflection alone.

Rejected: **the graph's `dielectric_bsdf` in `T` mode, read literally.** It
would refract by `ior` at a solid interface and multiply the transmitted
light by its own `1 − F` under a top that has already taken `1 − E_top`.
At index 1.5 and normal incidence a smooth pane would reflect 0.04 and
transmit 0.92 where a real one, and glTF's specification, account for all
of it. It would also require closed, outward-wound meshes, which glTF
assets with transmission and no volume do not promise, and would lose the
light of every total internal reflection, since the top lobe is Schlick's.
The difference is listed in the compatibility table.

### 6.3 A coloured top dims the base by its largest channel

`specular_color` makes the dielectric reflection's albedo coloured. The
[layered contract §4.1](LAYERED_BSDF.md#41-albedo-scaling-on-the-outgoing-direction)
wrote the factor `1 − E_top(wo)` without saying how a coloured `E_top`
enters, and MaterialX's specification leaves it to the implementation.

**Decision.** For the dielectric layer of this model the factor is the
scalar `1 − max(E_top(wo))`, as glTF's specular extension defines its
Fresnel mix. `ComposedLayer` takes the rule as a parameter; the sheen and
coat layers have scalar albedos, so they are unaffected, and with a white
`specular_color` the result is the core's exactly.

| Rule | Energy bound | White base under a coloured top | Note |
| --- | --- | --- | --- |
| `1 − max(E)` | holds | at most 1 per channel | glTF's rule |
| `1 − E` per channel | holds | exactly 1 per channel | the base takes the top's complementary colour: a blue highlight yellows the diffuse |
| `1 − mean(E)` | **fails** | above 1 in the top's brightest channel | what MaterialX's GLSL does; a red `f0` of 1 over a white base returns about 1.6 in red |

Later models state their own rule: Standard Surface's and OpenPBR's
coloured specular is a tinted dielectric, whose tint the layer already
leaves out ([layered BSDFs §11.3](LAYERED_BSDF.md#11-changes-found-in-implementation)).

### 6.4 The sheen lobe

A fourth primitive lobe, `Sheen`, MaterialX's `sheen_bsdf` in
`conty_kulla` mode (Estevez and Kulla, "Production Friendly Microfacet
Sheen BRDF", 2017):

```text
D(h)       = (2 + 1/r) · sin(θh)^(1/r) / (2π)          r = max(roughness, 0.005)
f · cos θi = color · D(h) · cos θi / (4 · (cos θi + cos θo − cos θi · cos θo))
```

without a Fresnel or shadowing term, as the definition's implementation
has it. It reflects only, is reciprocal, and is never a delta.

- **Sampling** is uniform over the hemisphere about the lobe's normal,
  density `1 / (2π)`: the lobe peaks toward grazing directions, where a
  cosine-weighted sampler is thinnest.
- **Albedo.** A new table, `Esheen`, over `cos θo` and roughness, 32 × 32,
  generated by the committed generator from the lobe's own function and
  held to `1e-3` like the others
  ([layered BSDFs §5.6](LAYERED_BSDF.md#56-directional-albedo-tables)).
  The table buffer grows by 1,024 floats. The layer below a sheen is
  scaled by `1 − s · Esheen(wo)`, uncoloured, as the definition's
  `sheen_bsdf` reports its throughput.
- **No compensation.** The lobe is an empirical fit, not a microfacet
  model that loses energy; its albedo is what the table says.

Rejected: the `zeltner` mode's LTC sheen. It is the better lobe and
OpenPBR's fuzz; LOTUS-MATERIALX-08 adds it. This definition names the
other mode.

### 6.5 The coat's normal

The coat is the first lobe with a normal of its own
([layered BSDFs §5.3](LAYERED_BSDF.md#53-normals)). `EvaluateGltfPbr`
builds it as the base normal is built: from the mesh's shading normal and,
with a lookup in slot 11, the tangent frame of that lookup. It hands the
closure the coat normal in the base shading frame, and the builder is
called with the full `wo`, not its cosine alone.

A coat normal below the geometric surface on the viewer's side is treated
as the base normal is today. Where `wo` lies below the coat's normal the
coat contributes nothing and dims nothing, by the layered contract's rule.

### 6.6 What the scene passes compile

[Core §12.1](GLTF_PBR.md#12-changes-found-in-implementation) left this
task to turn on what its layers need. They need: six lobes, the sheen
lobe, the dielectric lobe's rough reflection, and lobe normals. They do
not need the dielectric's refraction, thin wall or delta paths, nor
attenuation, which stay behind the probe's switch; `LOTUS_COMPOSED_FULL`
becomes one switch per part.

**Measurement.** The report repeats
[the core's](../reports/2026-11-16-gltf-pbr-core.md#cost-of-the-second-closure):
SPIR-V size of `path_trace` and `wavefront`, renderer creation time with
and without validation, GPU time on the fixed benchmarks under both
integrators, and `cornell-gltf-v1`, whose materials use no extension,
against its time today.

**Fallback rule.** A complete closure doubled renderer creation in the
core's measurement. If this task's closure raises creation time by more
than half over today's, the scene passes build the glTF closure from
fixed-position lobes — a struct with one member per lobe of §6.1 rather
than a list of six general lobes — so that each evaluation compiles each
lobe kind once. The probe keeps the general list, and `lotus-bsdf-test`
compares the two forms value for value. A fixed benchmark without glTF
materials that slows beyond its own spread is a defect of this task, as
in [core §6.2](GLTF_PBR.md#62-bsdfclosure-with-two-representations).

### 6.7 The stored reservoir surface

The reflection side of the larger closure needs 22 floats: the core's
seven; `f0` and `f90`; `transmission`; the coat's weight, alpha, normal
(two components) and albedo; the sheen's colour, alpha and albedo. The
144-byte record has 18 lanes a glTF surface can use, so the record grows
to 160 bytes, one more `float4`
([core §6.4](GLTF_PBR.md#64-the-stored-reservoir-surface)). `LoadClosure`
still reads no table, and the preview packing is unchanged in its lanes
and its arithmetic. The report records the reservoir memory before and
after.

### 6.8 Surface summary

| Field | Value |
| --- | --- |
| `albedo` | `base_color`: the diffuse and the transmitted light both carry it |
| `specular_roughness` | `roughness` |
| `specular_share` | `metallic + (1 − metallic) · max(f0)` |
| Debug AOVs | `base_color`, `roughness`, `metallic`, as today |

The coat and the sheen do not enter the summary: its consumers weigh the
base's principal lobe.

## 7. Tests

### 7.1 The lobes and the table, on the CPU

`lotus-bsdf-test` gains:

- **`Sheen` alone**, over roughness 0, 0.1, 0.3, 0.6 and 1:
  [BSDF API §7.3](BSDF_API.md#73-assertions)'s assertions, reciprocity,
  and an albedo of at most 1.
- **`GgxSchlick` with `transmit`**: the same assertions on the lower
  hemisphere, the value equal to the reflecting lobe's at the mirrored
  direction, and an albedo of 1 when white and compensated.
- **`Esheen`** between grid nodes against a fresh quadrature, within
  `1e-3`.

### 7.2 The closure, on the CPU

The glTF PBR target of [core §8.1](GLTF_PBR.md#8-tests) is extended over
each extension alone and all together: index 1, 1.33, 1.5 and 2.4;
`specular` 0, 0.5 and 1 with white and saturated `specular_color`;
`transmission` 0, 0.5 and 1; `clearcoat` 0.5 and 1 at coat roughness 0,
0.3 and 1 and coat normals tilted 0°, 15° and 45°; white, saturated and
half-strength sheen at roughness 0.1, 0.5 and 1.

1. **BSDF API §7.3's assertions**, with the mixture ratio required.
2. **White furnace.** Albedo 1 within the table tolerance for a white base
   colour, white `specular_color` and white sheen at every `specular`,
   `transmission`, `clearcoat`, coat roughness and metallic, with the coat
   untilted.
3. **Energy bound.** No configuration's albedo exceeds 1 beyond that
   tolerance, coloured tops and tilted coats included.
4. **Definition oracle.** The core's independent double-precision
   evaluation, extended to §3's graph with §6.2's and §6.3's readings and
   its own quadratures for the coat's and the sheen's albedos, equals the
   closure at random direction pairs on both hemispheres.
5. **Defaults.** With every extension at its default the closure has the
   core's three lobes and equals the core's closure bit for bit.
6. **Stored surface.** `StoreClosure` then `LoadClosure` reproduces value
   and density on the reflection side, to rounding, with every layer
   present.

The run time on four cores is reported against today's.

### 7.3 Translation, without a device

In `lotus-renderer-hydra-material-graph`: every row of §5 from a
hand-built network; the completeness test's expectations for the nine
inputs moved from diagnosed to translated; the networks usd-gltf-plugins
authors for each of the five glTF extensions, transcribed into the test,
translating without a diagnostic; a `thickness` above 0 beside a
transmission, warned.

### 7.4 The fixture

`material-gltf-extensions-v1`, generated, mirrored and referenced under
both integrators as every material fixture.

| Row | Cards and spheres |
| --- | --- |
| Specular | `ior` 1, 1.33 and 2.4; `specular` 0 and 0.5; a saturated `specular_color`; `specular` from an image |
| Transmission | 0.5 and 1 over a patterned backdrop, smooth and rough, white and tinted; `transmission` from an image; an open single-sided pane |
| Clearcoat | 0.5 and 1 over a rough dielectric and over a metal; a rough coat; a normal-mapped base under a smooth coat; a coat with its own normal map |
| Sheen | white and coloured over a dark diffuse, at three roughnesses; sheen under a clearcoat |
| Together | a material with every extension; the plugin's unlit material |
| Diagnosed | `thickness` 1 beside `transmission` 1, `iridescence` 1, `anisotropy_strength` 1 |

Its negative control exchanges `clearcoat` and `clearcoat_roughness`.

`material-gltf-v1`'s Diagnosed row holds `clearcoat` 1, `transmission` 1
and `ior` 1.8, which this task renders. Those three cards become
`iridescence` 1, `anisotropy_strength` 1 and `thickness` 1, the fixture's
reference is rewritten, and the report shows that its other cards are
unchanged within the
[reference tolerance](../reference/SCENE.md#reference-images).

### 7.5 Rendered furnace

`renderer.path.gltf_furnace` gains planes with a clearcoat, a white sheen,
and `transmission` 1 in front of the white environment, under both
integrators and both light samplings. The transmitting plane is the check
that crosses a composed closure's transmitted sample and the integrators'
unweighted-light rule.

### 7.6 Regression

Every committed reference and fixture image that holds no glTF material is
byte-identical, the ReSTIR evidence included. `cornell-gltf-v1`'s images
are compared with today's and the difference, expected to be none by
§7.2.5, is reported.

### 7.7 The sample assets

The formation of
[`formations/gltf-host-session`](../../formations/gltf-host-session/README.md)
opens the Khronos sample assets through usd-gltf-plugins with the packaged
renderer. The inventory the core deferred
([core §12.6](GLTF_PBR.md#12-changes-found-in-implementation)) is taken
with it: of the 11,776 materials, how many translate without a diagnostic
and how many raise each kind, by input, before and after this task. It
uses local assets, so it is reported and gates nothing.

## 8. Rejected alternatives in one place

| Alternative | Why not |
| --- | --- |
| The graph's refracting `dielectric_bsdf` for transmission | A second Fresnel loss, closed meshes required, total internal reflection lost; not what glTF specifies without the volume extension (§6.2). |
| A solid interface when `thickness` is above 0 | It is the volume extension, P2, and needs its attenuation to be worth its mesh requirements (§5). |
| The layer's factor per channel, or by the mean | Complementary colour in the base; the mean breaks the energy bound (§6.3). |
| A delta coat at roughness 0 | Next event estimation would not see the highlight, and the scene passes would compile the delta paths (§6.1). |
| A Schlick coat | The layered contract's dielectric lobe exists, is tested under layers, and is exact at the definition's index (§6.1). |
| Cosine-weighted sheen sampling | Thinnest where the lobe peaks (§6.4). |
| The `zeltner` sheen | Not this definition's mode; it arrives as OpenPBR's fuzz (§6.4). |
| An analytic fit for the sheen's albedo | An error no test ties to the code, as for the other albedos (§6.4). |
| Keeping the 128-byte record by folding constants | `emissive_strength` into `emissive` was rejected by the core; any other fold costs a parameter its range (§4). |
| Dropping the coat's normal from the stored surface | Reuse would shade neighbours with an untilted coat (§6.7). |

## 9. Acceptance for LOTUS-MATERIALX-06

1. `GltfPbr` holds §4's parameters; the render world, the GPU scene and
   the readback accept and round-trip them in the 144-byte record with
   slots 2 to 13.
2. The translator reads the nine inputs as §5 states, and §7.3's tests
   pass.
3. Both integrators shade the extended closure of §6.1 through
   `BsdfEvaluate`, `BsdfPdf` and `BsdfSample`; `lotus-bsdf-test` passes
   §7.1 and §7.2 in the `fast` tier.
4. `material-gltf-extensions-v1` is committed with references under both
   integrators, Hydra and headless agree on it, and
   `renderer.path.gltf_furnace` passes with §7.5's planes.
5. §7.6 holds.
6. The report records §6.6's measurements and outcome, the material and
   reservoir records' growth and memory, the test's run time and §7.7's
   inventory.
7. The scene reference documents the inputs, and the compatibility table
   lists them as supported, with thin transmission, the scalar layer
   factor and the coat's alpha floor as stated differences from the node
   definition's graph, and iridescence, anisotropy, volume and dispersion
   as unsupported.

## 10. Points settled in review

Accepted as written, 2026-11-16:

1. **Transmission is glTF's thin surface, not the graph's refracting
   dielectric** (§6.2). It follows glTF's specification where the pinned
   MaterialX graph, read literally, departs from it; the oracle and the
   compatibility table say so.
2. **A coloured specular top dims the base by its largest channel**
   (§6.3), glTF's rule, rather than per channel or by MaterialX's mean.
3. **The coat is never a delta** (§6.1): its alpha is floored like the
   base's.
4. **The GPU material record grows to 144 bytes and the stored reservoir
   surface to 160** (§4, §6.7).
5. **The scene passes compile six lobes, the sheen, the dielectric's
   reflection and lobe normals**, with a fixed-position closure as the
   fallback if renderer creation grows by more than half (§6.6).
6. **`material-gltf-v1`'s Diagnosed row and reference change** (§7.4).
7. **A `thickness` above 0 renders thin, with a warning**, until volume
   is taken (§5).

## 11. Changes found in implementation

LOTUS-MATERIALX-06's implementation departs from the text above in these
points, which await review; the
[report](../reports/2026-11-17-gltf-pbr-extensions.md#changes-to-the-contract)
gives the measurements.

1. **The sheen is normalized where its albedo exceeds 1.** §6.4 took the
   lobe's albedo to be at most 1. Its quadrature reaches 5.6 at a grazing
   outgoing direction and the roughness floor, and exceeds 1 below an
   outgoing cosine of about 0.09 for lobe roughnesses up to about 0.16:
   the fit returns more light than it receives there, and MaterialX only
   clamps the throughput it reports. Where the table value `E` exceeds 1
   the closure scales the lobe by `1 / E` and dims the base fully, so the
   energy bound holds. `Esheen`'s accuracy is held relative to the value
   where it exceeds 1.
2. **A reflecting lobe scatters into the shading normal's hemisphere
   only.** §6.5 let a tilted coat follow the layered contract's rule about
   its own normal alone. A coat's reflection that lands below the shading
   normal would be taken for a transmission by the integrators and could
   pass through an opaque surface, so it has no value, no density and is
   not sampled. A coat tilted by 45° loses up to 5% this way; the layered
   test's tilted coats assert that bound instead of a furnace.
3. **One switch, not one per part** (§6.6). The scene passes always
   compile lobe normals, the sheen and the dielectric's rough reflection;
   `LOTUS_COMPOSED_FULL` keeps only the dielectric's refraction, thin wall
   and delta paths and attenuation for the probe.
4. **The stored surface keeps terms, not inputs** (§6.7). The coat is
   stored as its weight times its compensation, that times its albedo, its
   alpha and two components of its normal, and the sheen as its normalized
   colour, its roughness and its weight times its albedo: 22 floats, as
   counted. The terms travel with the hit, beside the closure, instead of
   being read back from the closure's lobes. A coat normal in the shading
   frame's lower hemisphere cannot be stored by two components; such a
   coat lies about the base normal.
5. **The dielectric's reflectance at normal incidence is computed on the
   host**, in double precision, from the uniform `ior`, and the record
   holds both: the default index then gives the float nearest 0.04, which
   is what keeps a material at the defaults bit for bit the core's.
6. **§6.6's fallback rule was not exercised.** Renderer creation time was
   not measured against it; the shaders' sizes are in the report.
7. **§7.7's sample-asset inventory was not run.**
