---
status: accepted
owner: hydra-lotus
---

# Material IR v2

> Status: **accepted**, 2026-11-10, merged without review changes. This is
> LOTUS-MATERIALX-01's design, written before any code as the
> [shading-model contract](MATERIALX_SHADING_MODELS.md#11-recommended-order)
> requires. It settles three of the contract's
> [design-review decisions](MATERIALX_SHADING_MODELS.md#10-decisions-for-the-design-review):
> IR granularity (§10.1), the MaterialX version (§10.3) and the GPU strategy
> (§10.6). The layering approximation (§10.2) belongs to LOTUS-MATERIALX-03,
> the input paths (§10.4) to LOTUS-MATERIALX-04, and each model's feature
> boundary (§10.5) to its own task. The
> [scene reference](../reference/SCENE.md#materials-and-environment) owns the
> implemented behaviour. "Today" below describes `main` at v0.9.0.

## 1. Scope

What the material IR holds on the CPU, how the GPU material table lays it
out, and how the scene pass turns it into a surface the integrators shade.
LOTUS-MATERIALX-01 delivers this with one model, `UsdPreviewSurface`, whose
fixtures and committed references stay unchanged (§9). The other three
models get their tags and empty parameter sets reserved; their parameters
arrive with their tasks.

Not in this document: the BSDF API and the closure the integrators evaluate
(LOTUS-MATERIALX-02), layer and mix composition (03), which MaterialX nodes
translate (04), and any model's lobes (05–08).

## 2. Today

- `Lotus::Material` ([`material.hpp`](../../include/lotus/material.hpp)) is
  one flat structure with `UsdPreviewSurface`'s defaults: 13 shading fields,
  seven optional texture inputs and three texture-coordinate and colour-set
  fields. The Standard Surface subset is flattened
  into it; other MaterialX surfaces warn and fall back to the default.
- Each input is a constant or one `TextureInput`: a bilinear lookup at one
  texture-coordinate set, `texel · scale + bias`, one channel or RGB,
  clamped into the input's range.
- `GpuMaterialRecord` is 784 bytes: seven `float4` of constants and seven
  96-byte `GpuTextureInputRecord`s, used or not. 672 of its bytes are
  lookups a constant material never reads.
- `ReconstructHit` evaluates the inputs and calls `MakeSurfaceBsdf`;
  `SurfaceHit` carries the `SurfaceBsdf` and three raw inputs
  (`base_color`, `roughness`, `metallic`) that the temporal albedo, the
  reflection heuristics and the debug AOVs read directly.
- ReSTIR's `RestirSurface` stores the derived `SurfaceBsdf` (112 bytes) for
  temporal and spatial reuse.
- The light table's `EmissionEstimate` and `ValidateMaterial` read the
  flat fields.

## 3. Versions (contract §10.3)

The basis for features, defaults and comparisons is what the
[lookdev lock](../../strata.openstrata-cy2026-windows-x86_64-py313-lookdev.lock)
installs: **MaterialX 1.39.5** (`1.39.x` in the lock), with its default node
definitions:

| Model | Node definition | Version |
| --- | --- | --- |
| glTF PBR | `ND_gltf_pbr_surfaceshader` | 2.0.1 |
| Standard Surface | `ND_standard_surface_surfaceshader` | 1.0.1 |
| OpenPBR | `ND_open_pbr_surface_surfaceshader` | 1.1.1 (OpenPBR Surface 1.1.1) |

Unconnected inputs take these definitions' defaults, which differ between
models (OpenPBR's base colour is 0.8 and its specular roughness 0.3; glTF
PBR's base colour, metallic and roughness are 1). glTF behaviour is glTF
2.0 and the KHR extensions that `ND_gltf_pbr_surfaceshader` 2.0.1's inputs
name. A lock that moves MaterialX to another minor version reopens this
section; a patch version records itself in the release notes only.

## 4. CPU representation (contract §10.1)

### 4.1 Tagged models

```cpp
enum class ShadingModel : std::uint32_t {
  UsdPreviewSurface, GltfPbr, StandardSurface, OpenPbr,
};

struct PreviewSurface { /* today's fields, names unchanged */ };
struct GltfPbr { /* LOTUS-MATERIALX-05 */ };
struct StandardSurface { /* LOTUS-MATERIALX-07 */ };
struct OpenPbr { /* LOTUS-MATERIALX-08 */ };

struct Material {
  // Model-independent: where lookups read, and what hides the surface.
  MaterialGeometry geometry;
  std::variant<PreviewSurface, GltfPbr, StandardSurface, OpenPbr> surface;
  ShadingModel Model() const;  // the variant's index
};
```

`Model()` is the variant's index, so the tag and the parameters cannot
disagree. A default `Material` is a default `PreviewSurface`, as today.

**Model-independent fields** are those that act before or outside the BSDF
and mean the same in every model:

| Field | Why it is shared |
| --- | --- |
| `texcoords`, `texcoord_fallback` | One texture-coordinate source per material, read before any input. |
| `opacity`, `opacity_texture`, `opacity_threshold` | Coverage is decided in the any-hit test, which must not dispatch on the model. Each model's translator maps its own coverage onto it: glTF's `OPAQUE` to 1, `MASK` to the threshold, `BLEND` to stochastic coverage; OpenPBR's `geometry_opacity` and Standard Surface's `opacity` as today. |
| `normal`, `normal_texture` | The base shading normal is built before the BSDF from a tangent-space input. A model's second normal, such as a coat's, is that model's parameter. |
| `base_color_primvar` | A colour set that drives one colour input; which input is the model's, named by a slot (§5.2). |

Everything else — emission included, because a coat and fuzz attenuate it
in Standard Surface and OpenPBR — belongs to the model. Same-named
parameters in different models stay distinct members of distinct
structures, and nothing converts one model's parameters into another's
(contract §3.1).

**Rejected: optional model blocks beside today's fields.** Keeping the
preview fields at the top level and adding `std::optional<GltfPbr>` and so
on would avoid renaming call sites, but a `Material` could then hold
preview fields that a glTF material ignores: the uber material the contract
avoids. The rename is mechanical: a few hundred field references in the adapters,
backend and core move behind `Preview(material)` accessors, and `PreviewSurface` keeps its
field names so the diff reads one-to-one.

### 4.2 Inputs and texture expressions

New models write each input as one type:

```cpp
template <class T> struct MaterialInput {
  T value;                             // the constant, or the model default
  std::optional<TextureInput> texture; // a lookup that replaces it
};
```

`PreviewSurface` keeps its constant and `_texture` pairs in
LOTUS-MATERIALX-01 so that its diff is the move alone; converting it is
cleanup that may follow.

An input's **expression** stays what `TextureInput` already is: a constant,
or one bilinear lookup with a per-channel affine map (`scale`, `bias`), a
channel choice and the input's clamp. That form absorbs the MaterialX
subset that matters most (contract §5): multiplication and addition by a
constant, `mix` between a lookup and a constant with a constant weight,
channel extraction, sRGB decoding by texture format, and a normal map.
LOTUS-MATERIALX-04 folds those graphs into it and diagnoses the rest —
two lookups combined, a clamp narrower than the input's range, a
procedural — as the contract requires.

**Rejected for now: a per-input expression program.** A small bytecode per
input would represent arbitrary graphs, but every input would then pay for
an interpreter in the wavefront shading kernel, and no fixture yet needs
it. The GPU binding record has a `mode` field (§5.2), so a later program
mode can be added without changing the table's layout. It enters with a
measured asset that needs it.

## 5. GPU layout

### 5.1 One fixed-stride record

The material table stays an array of one fixed-size record, so
`material_slot` keeps indexing it and the any-hit and shading code keep one
load path:

```text
GpuMaterialRecord (stride S, a multiple of 16)
  header        16 B   model, flags, lookup_base, textured mask (64 bits)
  geometry      32 B   texcoord fallback, opacity, threshold, tangent normal
  parameters  S−48 B   the model's constants, in that model's word order
```

`S` is the largest model's parameter block, rounded to 16 bytes. With only
`UsdPreviewSurface` it is 128 bytes (its 18 parameter words fit in 80); the
budget for OpenPBR's supported inputs is 256 bytes, which LOTUS-MATERIALX-10
measures. A model's unused tail is zero.

**Rejected: variable-size records.** They would save the tail on small
models but need an offset table and a second dependent load on every hit;
the table is a few kilobytes even for thousands of materials.

### 5.2 Lookups out of the record

Texture lookups move to a separate **binding table** of today's 96-byte
`GpuTextureInputRecord`, unchanged, packed per material. Each model numbers
its inputs from 0 to at most 63 (its *slots*); bit `k` of the header's
`textured` mask says slot `k` has a lookup, which is

```text
bindings[lookup_base + popcount(textured & ((1 << k) − 1))]
```

A constant material reads no binding; a preview material with a base-colour
texture reads one 96-byte record instead of carrying seven. The shader asks
for a slot by a compile-time constant, so no local array is indexed
dynamically. `base_color_primvar` names a slot the same way.

### 5.3 Size today and after

| Material | Today | v2 record | v2 bindings |
| --- | --- | --- | --- |
| Constant `UsdPreviewSurface` | 784 B | 128 B | 0 |
| `UsdPreviewSurface`, 3 textured inputs | 784 B | 128 B | 288 B |
| OpenPBR, 8 textured inputs (budget) | — | 256 B | 768 B |

The readback that tests use (`GpuMaterialContents`) decodes the header,
geometry, parameters and bindings back into a `Material`, so the
round-trip tests keep their shape.

## 6. Shader structure (contract §10.6)

### 6.1 Switch dispatch, one kernel

`ReconstructHit` reads the header and switches on the model:

```text
switch (record.model)
  UsdPreviewSurface → EvaluatePreview(record, hit) → SurfaceBsdf (today's)
  GltfPbr | StandardSurface | OpenPbr → reserved until their tasks
```

Each model has its own parameter evaluation (its slots, defaults and
clamps) and, from LOTUS-MATERIALX-02 on, its own closure construction. The
reference and wavefront integrators keep sharing this code through
`scene.slang` (contract §1, condition 5).

**Switch dispatch, not specialization, is the starting point.** A switch
keeps one pipeline and one wavefront shading kernel; specialization —
per-model pipelines, or wavefront shading queues sorted by model — costs
pipelines and a sort pass and is worth it only if divergence or register
pressure is measured. LOTUS-MATERIALX-10 measures both on mixed-model
scenes; per-model shading queues are the first alternative if the switch
costs more than the [fixed benchmarks](../reference/SCENE.md#fixed-benchmarks)
allows.

### 6.2 What leaves the material besides the BSDF

`SurfaceHit` today carries raw `base_color`, `roughness` and `metallic` for
consumers that are not the BSDF. Those become a model-independent
**surface summary** that each model's evaluator fills:

| Consumer | Reads |
| --- | --- |
| SVGF albedo demodulation | `albedo`: the colour the model's diffuse and transmitted light carry |
| Temporal reflection weighting | `roughness` and `specular_share`: the model's dominant specular lobe |
| Debug AOVs (`base_color`, `roughness`/`metallic`) | the model's own inputs, as today |

For `UsdPreviewSurface` the summary is today's values and today's formulas,
so the temporal outputs do not change.

### 6.3 What stays for LOTUS-MATERIALX-02

`SurfaceBsdf` stays the preview closure in LOTUS-MATERIALX-01, and
`RestirSurface` keeps packing it. Whether reuse packs a closure per model
(growing `RestirSurface` to the largest closure) or stores the hit and
re-evaluates the material is LOTUS-MATERIALX-02's decision, made with the
closure's size known.

## 7. Host-side consumers

- **Validation.** `ValidateMaterial` checks the geometry fields and then
  the model's own fields; each model's ranges are its node definition's.
- **Light table.** `EmissionEstimate` becomes per model. It may
  overestimate — a coat's attenuation is ignored — because the table only
  proportions sampling; emission it misses is still found by BSDF sampling,
  as today.
- **Hydra translator.** `UsdPreviewSurface` and the Standard Surface subset
  produce `PreviewSurface` as today. The Standard Surface subset moves to
  `StandardSurface` in LOTUS-MATERIALX-07, not before, so its warnings and
  fixtures do not change in LOTUS-MATERIALX-01.
- **Change tracking.** A material whose model changes is a material
  change; the [invalidation categories](INTERACTIVE_QUALITY.md#5-invalidation-by-change-category)
  are unchanged.

## 8. Rejected alternatives in one place

| Alternative | Why not |
| --- | --- |
| One uber structure with every model's parameters | Merges same-named parameters with different meanings (contract §3.1). |
| Optional model blocks beside today's fields | The tag and the fields can disagree (§4.1). |
| A per-input expression program now | An interpreter in every shading kernel without an asset that needs it (§4.2). |
| Variable-size GPU records | A second dependent load per hit to save a small table's tail (§5.1). |
| Per-model specialization first | Pipeline and sort cost before any measurement shows the switch costs more (§6.1). |

## 9. Acceptance for LOTUS-MATERIALX-01

1. The CPU `Material` is the tagged form of §4, and every caller compiles
   against it; `UsdPreviewSurface` is the only model with parameters.
2. The GPU table uses §5's record and binding table, and the readback
   round-trips every material fixture.
3. Every committed reference image and material fixture is unchanged:
   reference and wavefront, Hydra and headless, the
   [material fixtures](../reference/SCENE.md#material-fixtures) included.
   An image that changes is a defect unless it is traced to a rounding
   difference and recorded.
4. The GPU material memory of the fixed benchmarks is recorded before and
   after.
5. The scene reference documents the new layout, and the contract's
   §10.1, §10.3 and §10.6 point here as settled.
