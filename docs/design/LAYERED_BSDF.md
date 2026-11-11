---
status: accepted
owner: hydra-lotus
---

# Layered BSDFs

> Status: **accepted**, 2026-11-11. This is LOTUS-MATERIALX-03's contract,
> written before any code: how lobes compose into mixtures and layers on
> top of the [BSDF API](BSDF_API.md), the layering approximation the
> [shading-model contract](MATERIALX_SHADING_MODELS.md#10-decisions-for-the-design-review)
> left to this task (§10.2), and the energy, reciprocity and consistency
> tests that hold the composition. It changes no rendered image: no shading
> model uses the composition until LOTUS-MATERIALX-05. "Today" below
> describes `main` after LOTUS-MATERIALX-02.

## 1. Scope

The shared machinery every MaterialX model builds its closure from: the
primitive lobes this task provides, the `mix` and `layer` operations, how a
composed closure is evaluated, sampled and given a density, the directional
albedo tables the layering needs, and the tests.

Not in this document: which lobes a model stacks in which order and with
which weights (05–08), the lobes no model needs before its own task (sheen,
fuzz, anisotropy, thin film, Oren–Nayar), OpenPBR's coat darkening (08),
and how the composed closure is carried in `BsdfClosure` and on the GPU,
which [BSDF API §3](BSDF_API.md#3-the-functions) leaves to
LOTUS-MATERIALX-05.

## 2. Today

- [`bsdf.slang`](../../backend/vulkan/shaders/bsdf/bsdf.slang) holds one
  closure, `PreviewClosure`. Its composition is written out by hand: a
  Lambert base attenuated by Schlick transmittance in both directions,
  `(1 − F(wo))(1 − F(wi))`, a GGX lobe mixed between metal and dielectric,
  and a transmitted share. Its lobe functions take the preview closure
  itself, so no other closure can reuse them.
- GGX lobes are single-scattering: rough metals lose energy, and the
  preview closure's coat comment records that GGX multiple scattering is
  not compensated.
- No directional albedo is computed anywhere. Lobe selection uses the
  view Fresnel as an estimate.
- `lotus-bsdf-test` checks [BSDF API §7](BSDF_API.md#73-assertions)'s six
  assertions for the preview closure on the CPU. It checks no energy bound
  and no reciprocity.

## 3. What the specifications define

The three models' specifications agree on the operations and leave their
implementation partly open.

- **MaterialX 1.39** (PBR specification, at the pinned
  [`v1.39.5`](MATERIAL_IR_V2.md#3-versions-contract-103)). `mix` is
  "`bg∗(1−mix) + fg∗mix`" with a float weight; `add` sums; `multiply`
  scales by a float or colour that should not exceed 1. `layer` places a
  layerable BSDF over a base: "The implementation is target specific, but
  a standard way of handling this is by albedo scaling, using the function
  `base*(1-reflectance(top)) + top`, where the reflectance function
  calculates the directional albedo of a given BSDF." Microfacet lobes
  "are expected to preserve energy as the roughness of the surface
  increases, with multiple scattering compensation being a popular
  implementation strategy." `layer` has no normal of its own; each lobe
  has one, and "each layer can use different bump and normal maps."
- **OpenPBR Surface** (the published page is v1.1.2; the pinned node
  definition is 1.1.1, against which LOTUS-MATERIALX-08 checks). Layering
  is albedo scaling on the outgoing direction,
  `f_coat + T²_coat · (1 − E_coat(ω_o)) · f_sub`, where `T²_coat` is the
  coat's volumetric transmittance along both rays,
  `T_coat^(1/μ_i^t + 1/μ_o^t)`. The specification calls this form
  non-reciprocal and states that it passes a white furnace when the
  substrate does. Fuzz dims what lies below by its own albedo,
  `1 − F · E_fuzz`, without tinting it. Coat and base take separate
  normals.
- **glTF PBR** reaches Lotus only through MaterialX's `gltf_pbr` node
  definition ([contract §12](MATERIALX_SHADING_MODELS.md#12-relation-to-earlier-policy)),
  whose graph composes with the same `layer` and `mix` nodes.

All three, therefore, need one layering rule: the top lobe's directional
albedo, evaluated toward the viewer, scales what lies below it.

## 4. The layering approximation (contract §10.2)

### 4.1 Albedo scaling on the outgoing direction

**Decision.** `layer(top, base)` is

```text
f(wo, wi) = f_top(wo, wi) + A(wo, wi) · (1 − E_top(wo)) · f_base(wo, wi)
```

where `E_top(wo)` is the top lobe's directional albedo, the integral of its
`f · |cos θi|` over the sphere for the given `wo` (its reflected albedo
for a reflecting lobe), and `A` is an attenuation the model may supply
(§5.4), 1 by default. `wo` is the BSDF API's outgoing direction, the
direction toward the viewer, which is the specifications' `ω_o`.

- **`E_top` is the albedo of the top lobe as implemented**, multiple
  scattering compensation included (§4.2). Then a non-absorbing stack over
  a white base reflects `E_top + (1 − E_top) · 1 = 1` exactly, up to the
  table error (§5.6), which is the furnace the specifications promise.
- **Delta tops.** For a smooth top, `E_top(wo)` is its Fresnel
  reflectance at `wo`, computed exactly, not looked up.
- **Nesting.** `layer` is associative, as OpenPBR notes: fuzz over coat
  over base is `layer(fuzz, layer(coat, base))`, and every lobe of the
  base carries the product of the factors above it.
- **Transmission below a layer.** A base lobe that transmits (`wi.z < 0`)
  is scaled the same way. The top's refraction of the direction is not
  modelled: the layer is infinitely thin, as in the specifications.

Rejected for this delivery:

- **A symmetric factor**, `(1 − E_top(wo))(1 − E_top(wi))`, possibly over
  `1 − Ē_top`. It restores reciprocity, but no specification uses it, the
  furnace no longer holds without a further normalisation, and images
  would differ from the MaterialX and OpenPBR references the delivery is
  compared with (contract §7). The preview closure keeps its own symmetric
  Schlick form, which is its model's rule, not this one.
- **Explicit transport between the layers** (position-free random walks,
  tabulated adding-doubling). It is the "general layered-transport solver"
  the contract defers (§4), and it would make the sampled direction depend
  on more than three random numbers ([BSDF API §5.3](BSDF_API.md#53-bsdfsample)).

### 4.2 Multiple scattering

**Decision.** Every GGX **reflection** lobe this task provides is
compensated for multiple scattering by scaling, after Turquin et al.,
"Practical multiple scattering compensation for microfacet models" (2019):

```text
f_ms(wo, wi) = (1 + F0 · (1 − E1(wo)) / E1(wo)) · f_ss(wo, wi)
```

with `E1` the single-scattering albedo for a Fresnel of 1 and `F0` the
lobe's reflectance at normal incidence (per channel). A white conductor's
albedo is then exactly 1. The factor depends on `wo` only, which is what
makes it non-reciprocal, as OpenPBR records for it.

- **Transmitting lobes are not compensated** in this task. A rough
  dielectric's reflection and refraction together lose the multiply
  scattered share; the tests bound it (§7) and its model task reports it.
  Compensating refraction needs tables of the transmitted albedo per
  index and is taken by the first task whose fixtures show the loss.
- **Compensation is per lobe**, selectable by the model builder, so a
  model whose specification asks for uncompensated lobes can have them.
- **Interreflection between layers** (light the top sends back down onto
  the base) is not modelled by the operation; OpenPBR's coat darkening,
  which approximates it, is a model input LOTUS-MATERIALX-08 adds as an
  attenuation (§5.4).
- **The preview closure is unchanged**: uncompensated, so every committed
  reference stays byte-identical.

### 4.3 Reciprocity

It follows from §4.1 and §4.2 that only part of a composed closure is
reciprocal. The contract's reciprocity test (contract §7) applies to what
should be:

- every primitive lobe, uncompensated (§5.1);
- `mix` of reciprocal lobes;
- the attenuation `A`, which must be symmetric in `wo` and `wi`.

Layers and compensation factors are documented exceptions, as in the
specifications. Their test is structural instead: the composed value
equals §4.1's formula with the factors computed from `wo` alone (§7).

## 5. The composed closure

### 5.1 Primitive lobes

This task provides three lobes, the ones the first model (glTF PBR) and
every later model's base need:

| Lobe | MaterialX node | Parameters |
| --- | --- | --- |
| `Lambert` | `oren_nayar_diffuse_bsdf` at roughness 0 | colour |
| `GgxSchlick` | `generalized_schlick_bsdf`, `R` mode, exponent 5 | `f0`, `f90` colours, alpha, compensation flag |
| `GgxDielectric` | `dielectric_bsdf`, `R`, `T` or `RT` | relative index, tint, alpha, thin wall, compensation flag (reflection only) |

Each provides, in its own frame (§5.3): `f · |cos θi|`, its density, a
sampler drawing from `u.xy` (and `u.z` rescaled when it chooses between
reflection and refraction), and its directional albedo `E(wo)`. The
dielectric reuses the
[transmission contract](TRANSMISSION.md)'s interface code and measures
([BSDF API §5.5](BSDF_API.md#55-transmission-and-transport-mode)):
radiance mode, `1/η²` across a solid's interface, delta below the alpha
floor or when index-matched. A `generalized_schlick_bsdf` with an exponent
other than 5 is not represented; its model task diagnoses it.

The preview closure's private lobe functions are not rewritten onto these
lobes. They differ in Fresnel form, compensation and composition, and
re-expressing them would change committed images for no gain.

### 5.2 Flattening

A composed closure is a flat list of at most `kMaxLobes` lobes, each with
a **weight** `W_k`, an RGB spectrum fixed when the closure is built:

```text
f(wo, wi) = Σ_k  W_k · A_k(wo, wi) · f_k(wo, wi)
```

The operations act on the weights while a model's builder assembles the
list:

- `mix(bg, fg, m)` multiplies `fg`'s weights by `m` and `bg`'s by
  `1 − m`; `add` concatenates; `multiply` scales the weights.
- `layer(top, base)` multiplies every base lobe's weight by
  `1 − E_top(wo)` and attaches the top's attenuation to it.

Because `wo` is known when `ReconstructHit` builds the closure, `E_top(wo)`
is computed once there; the three functions read only weights. This keeps
the functions pure in `(closure, wo, wi)` as the
[BSDF API](BSDF_API.md#3-the-functions) requires, since a closure is
always built for the `wo` it is evaluated with. A stored ReSTIR closure
keeps its weights with the `wo` it was built for.

`kMaxLobes` is the largest list a model needs, fixed by the model tasks
(OpenPBR's fuzz, coat, specular, metal, transmission and diffuse make six).
Its register cost in the wavefront kernel is LOTUS-MATERIALX-05's
measurement, with [BSDF API §3](BSDF_API.md#3-the-functions)'s
representation question.

### 5.3 Normals

The BSDF API's frame stays as [§4](BSDF_API.md#4-frame-and-directions)
defines it: the hit's shading frame, about the base normal. Each lobe
carries its own unit normal in that frame, the base normal by default and
the coat's for a coat. A lobe evaluates in the frame about its normal; in
that frame:

- a lobe for which `wo` lies below its normal contributes nothing and is
  not chosen, and its selection probability goes to the others;
- a reflection that lands below the lobe's normal, or a transmission above
  it, has no value or density, as for a single lobe today;
- the side rules of [BSDF API §6.1](BSDF_API.md#61-sides), against the
  geometric normal, stay the integrator's.

`E_top(wo)` is evaluated in the top's frame, so a tilted coat dims the base
by the albedo it has toward the viewer in its own frame.

### 5.4 Attenuation

`A_k(wo, wi)` is an RGB factor a model attaches to the lobes below one of
its layers, for absorption in that layer: OpenPBR's coat colour
`T_coat^(1/μ_i^t + 1/μ_o^t)`, with `μ^t` the cosines refracted into the
coat. It must be symmetric in `wo` and `wi`, and it must not exceed 1. It
scales the value only; sampling estimates it at `wo` (§5.5). This task
provides the path-length form above as the one attenuation, since it is
the only one the models' specifications name; a lobe has at most one.

### 5.5 Sampling

`BsdfSample` makes **one** choice among the lobes with `u.z`, then lets
the chosen lobe draw a direction from `u.xy` and, if it has a choice of
its own, from `u.z` rescaled. Three random numbers suffice however deep
the layers nest ([BSDF API §5.3](BSDF_API.md#53-bsdfsample)): the
composition never rescales `u.z` once per level.

- **Selection probability.** `p_k ∝ max(W_k · A_k(wo, wo) · E_k(wo))`,
  the lobe's estimated contribution toward the viewer, over the lobes
  valid in their frame (§5.3). A lobe with a positive weight gets at least
  a floor probability (0.05, renormalised), so the sampler covers every
  lobe's support; the floor is not part of any value.
- **Density.** `BsdfPdf = Σ_k p_k · pdf_k(wo, wi)` over the non-delta
  lobes, every lobe's density evaluated whichever was chosen.
- **Weight.** The **mixture ratio** `BsdfEvaluate / BsdfPdf` for every
  non-delta sample. Composed closures use no component ratios: the
  cancellations the preview closure relies on hold within one lobe, not
  across weights computed from tables. A delta sample's weight is the
  chosen lobe's delta albedo times its weight and attenuation, over its
  selection probability.

### 5.6 Directional albedo tables

The albedos the layers and the compensation need come from tables, the
same data on the GPU and in the CPU test:

| Table | Axes | Use |
| --- | --- | --- |
| `E1` | `cos θo`, alpha | GGX albedo with a Fresnel of 1; compensation |
| `Eschlick` | `cos θo`, alpha | the share Schlick's `f90 − f0` term adds; with `E1`, a `GgxSchlick` lobe's albedo for any `f0`, `f90` |
| `Edielectric` | `cos θo`, alpha, relative index | a rough `GgxDielectric`'s reflected albedo, entering and leaving, total internal reflection included |

- **Generation.** A committed generator integrates the lobes' own
  single-scattering functions by quadrature, through the CPU probe, so a
  table cannot drift from the code it describes. Its output is a committed
  data file; a lobe change that moves a table regenerates it in the same
  pull request.
- **Resolution** is the smallest that keeps interpolation error below
  `1e-3` absolute at every probe point between grid nodes (§7);
  32 × 32 for the 2D tables is the starting point.
- **Storage.** On the GPU, one read-only storage buffer with linear
  interpolation in the shader, bound by LOTUS-MATERIALX-05 when a model
  first reads it. Not `static const` arrays in Slang: dynamically indexed
  constant arrays can be copied into per-invocation memory by the SPIR-V
  compiler. In the CPU test, the probe binds the same data.

## 6. Where it lives

- New shader modules beside `bsdf.slang` for the lobes, the composed
  closure and the table lookups. `bsdf.slang` keeps the API and the
  preview closure.
- `BsdfClosure` gains no field and no tag in this task: no model builds a
  composed closure until LOTUS-MATERIALX-05 wires it in. The probe module
  exports the composed closure's three functions directly, beside the API's.
- The generator and the table data sit with the validation sources that
  already compile the probe.

Since no shader the integrators run imports the new modules, every
rendered image is unchanged by construction; the acceptance still checks
the references (§9).

## 7. Tests

`lotus-bsdf-test` grows; it stays in the `fast` tier, and its run time on
four cores is reported against today's 22 s.

**Configurations**, each over [BSDF API §7.2](BSDF_API.md#72-configurations)'s
outgoing directions:

| Stack | Varied |
| --- | --- |
| Each primitive lobe alone | its parameters; compensation on and off |
| `mix(Lambert, GgxSchlick)` | `m` 0, 0.3, 1 |
| `layer(GgxDielectric R, Lambert)` | coat roughness 0 to 1, index 1 to 2.4 |
| `layer(GgxDielectric R, GgxSchlick)` | as above, white and tinted metal |
| Coat with its own normal | tilt 0°, 15°, 45° |
| Coat with absorption | `T_coat` white, tinted |
| `layer(coat, mix(GgxSchlick, layer(GgxDielectric R, Lambert)))` | the shape glTF PBR and Standard Surface share |
| `GgxDielectric RT` under a coat | solid and thin wall |

**Assertions.**

1. **BSDF API §7.3's six assertions** on every composed configuration,
   with the mixture ratio required everywhere (§5.5).
2. **White furnace.** For a non-absorbing stack over a white base (white
   Lambert, or a compensated `GgxSchlick` with `f0 = f90 = 1`), the albedo
   (the estimator's mean weight, including delta samples) is 1 within a
   tolerance fixed per configuration family from the table error, for
   every `wo`. Uncompensated and transmitting configurations instead
   assert the measured loss stays under a stated bound per roughness.
3. **Energy bound.** No configuration's albedo exceeds 1 beyond the same
   tolerance, at any `wo`, grazing ones included.
4. **Reciprocity** (§4.3). For the reciprocal parts:
   `f(wo, wi) = f(wi, wo)` on the reflection side, and across a solid's
   interface the radiance-mode relation `f(wo, wi) / η_o² = f(wi, wo) / η_i²`
   (Veach, 1997, §5.2), with `η_o` and `η_i` the indices on `wo`'s and
   `wi`'s sides; a thin wall is symmetric.
5. **Structure.** For every layer configuration, the composed value equals
   §4.1's formula built from the lobes' own values and the table albedo
   at `wo`, at random direction pairs.
6. **Tables.** At points between grid nodes, each table's interpolated
   value equals a fresh quadrature of its lobe within `1e-3`.

Thresholds are fixed once from the deterministic generator, as in BSDF
API §7.1, and recorded in the report.

## 8. Rejected alternatives in one place

| Alternative | Why not |
| --- | --- |
| A symmetric layering factor | No specification uses it, it breaks the furnace, and images would differ from the references compared against (§4.1). |
| Explicit inter-layer transport | The general solver the contract defers; more than three random numbers (§4.1). |
| No multiple scattering compensation | Rough metals stay dark, and a white base under a layer fails the furnace the specifications promise (§4.2). |
| Rewriting the preview closure onto the new lobes | Changes committed images for no gain (§5.1). |
| Nested lobe choice, rescaling `u.z` per level | Precision falls with depth; one flat choice costs the same (§5.5). |
| Component-ratio weights in composed closures | Cancellations do not hold across table-derived weights (§5.5). |
| Albedos computed per hit by quadrature, or by analytic fits | Quadrature is too slow per hit; fits add an error no test ties to the code (§5.6). |
| `static const` table arrays in the shader | Risk of per-invocation copies (§5.6). |

## 9. Acceptance for LOTUS-MATERIALX-03

1. The three lobes of §5.1, the `mix`, `add`, `multiply` and `layer`
   operations of §5.2, normals (§5.3), attenuation (§5.4), sampling (§5.5)
   and the tables (§5.6) exist in shader modules the probe compiles.
2. `lotus-bsdf-test` passes §7's assertions in the `fast` tier.
3. Every committed reference image is unchanged, reference and wavefront,
   Hydra and headless.
4. The contract's §10.2 points here as settled, and the report records the
   thresholds, the measured furnace losses of uncompensated and
   transmitting configurations, and the test's run time.

## 10. Points settled in review

Accepted as written, 2026-11-11:

1. Albedo scaling toward the viewer, non-reciprocal as the specifications
   are (§4.1), rather than a symmetric form.
2. Turquin-style compensation on reflection lobes now, refraction later
   (§4.2).
3. The composed closure's tables are bound on the GPU by
   LOTUS-MATERIALX-05, not by this task, which keeps 03 render-neutral by
   construction (§5.6, §6).
4. A selection floor of 0.05 (§5.5), as the preview closure's specular
   probability clamp uses today.
