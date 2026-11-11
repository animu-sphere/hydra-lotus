---
status: accepted
owner: hydra-lotus
---

# BSDF API

> Status: **accepted**, 2026-11-11, and
> [implemented](../reports/2026-11-11-bsdf-api.md). This is LOTUS-MATERIALX-02's contract,
> written before any code as the
> [shading-model contract](MATERIALX_SHADING_MODELS.md#34-one-bsdf-api-per-model-composition)
> requires: the `Evaluate`, `Sample` and `Pdf` functions every shading
> model implements and both integrators call, their measures, and the tests
> that hold them consistent. It refines the
> [transmission contract](TRANSMISSION.md)'s §5 and §6 where it says so;
> elsewhere that contract holds. It also takes the reservoir-packing
> decision the [material IR v2 design](MATERIAL_IR_V2.md#63-what-stays-for-lotus-materialx-02)
> left here. "Today" below describes `main` after LOTUS-MATERIALX-01.
> Two clauses changed in implementation: the sample record is named
> `BsdfSampleResult` (§3), and the layered lobes are never delta (§5.4).

## 1. Scope

The interface between a shading model's closure and the light transport:
what the three functions return, in which measure, for which components,
and what the integrators may assume of them. LOTUS-MATERIALX-02 delivers it
with one closure, `UsdPreviewSurface`'s, and the tests in §7; every
committed reference stays unchanged (§9).

Not in this document: how lobes compose into layers and mixtures
(LOTUS-MATERIALX-03), any model's lobes (05–08), and emission, which stays
a separate surface output (contract §3.4).

## 2. Today

- [`bsdf.slang`](../../backend/vulkan/shaders/bsdf/bsdf.slang) holds one
  closure, `SurfaceBsdf`, built by `MakeSurfaceBsdf` in `ReconstructHit`,
  and three functions: `BsdfEvalCos`, `BsdfPdf` and `SampleBsdf`.
- Both integrators already share them: the reference and wavefront paths
  both run `transport.slang`'s `ScatterPath`, and ReSTIR's targets and MIS
  weights in `restir.slang` call the same functions.
- `BsdfEvalCos` and `BsdfPdf` cover **reflection only**. Rough refraction
  is sampled with a weight derived from its density, but neither function
  evaluates it ([transmission §5](TRANSMISSION.md#5-sampling)), so nothing
  can test the sampled refraction against a density.
- `SampleBsdf`'s `weight` is not always `BsdfEvalCos / BsdfPdf` for the
  sampled direction: with transmission, a layered sample's weight is the
  layered lobes' own ratio, and a pure metal's and the interface's weights
  cancel `D` analytically. Both are correct estimators; neither is written
  down as allowed.
- There are no tests of the BSDF functions themselves. Their correctness
  is inferred from reference images.
- `RestirSurface` stores the preview closure's reflection-side fields in
  128 bytes.

## 3. The functions

Every closure provides three functions in the shading frame (§4):

```text
Spectrum   BsdfEvaluate(BsdfClosure closure, float3 wo, float3 wi)
float      BsdfPdf     (BsdfClosure closure, float3 wo, float3 wi)
bool       BsdfSample  (BsdfClosure closure, float3 wo, float3 u, out BsdfSampleResult sample)
```

The contract's `Evaluate(material, context, wi, wo)` maps onto these as
follows: the material and its context — the hit's inputs, the side the
path arrives from, the relative index — are folded into the closure when
the model's evaluator builds it in `ReconstructHit`, so the functions take
only the closure and the two directions. The argument order stays
`(wo, wi)`, as today.

```text
struct BsdfSampleResult {
  float3   wi;           // the sampled direction, shading frame
  Spectrum weight;       // §5.3
  float    pdf;          // §5.2: solid-angle density, 0 for a delta sample
  bool     delta;        // chosen from a Dirac delta component
  bool     transmitted;  // wi.z < 0: through the surface
}
```

`BsdfClosure` is the closure of the hit's shading model: its model tag and
that model's data, one value per hit whatever the model. With one model it
is the preview closure; how several models share one value (a field per
model, or one payload sized to the largest) is decided by the first task
that adds a second model, LOTUS-MATERIALX-05, by measuring register use in
the wavefront shading kernel. The functions dispatch on the tag with a
`switch`, as the [IR v2 design](MATERIAL_IR_V2.md#61-switch-dispatch-one-kernel)
decided for the material evaluators.

The old names (`BsdfEvalCos`, `SampleBsdf`, `SurfaceBsdf` as the
integrators' type) are removed, not kept as aliases: every caller moves.
The sample record is `BsdfSampleResult`, not `BsdfSample`: Slang does not
let a type and a function share a name. The preview closure's type is
`PreviewClosure`, built by `MakePreviewClosure` and wrapped by
`MakeBsdfClosure`.

## 4. Frame and directions

- Directions are unit vectors in the **shading frame**: `z` is the shading
  normal turned toward `wo`'s side, and both directions point away from
  the surface.
- `wo` is the direction toward the path's previous vertex (toward the
  camera for a primary hit); `wi` is the direction the path continues in,
  or toward a light. For `wo.z ≤ 0` every function returns zero and
  `BsdfSample` fails.
- `wi.z > 0` is **reflection**, `wi.z < 0` **transmission**. The
  functions are defined over the whole sphere, not one hemisphere.
- The closure knows only the shading frame. Disagreement between the
  shading and geometric normals is the integrator's to resolve (§6.1).

## 5. Measures

### 5.1 `BsdfEvaluate`

`f(wo, wi) · |cos θi|`, with `cos θi = wi.z`, as an RGB `Spectrum`: the
**non-delta** components' value, reflection and transmission alike.
Delta components have no value in it. Transmission is in **radiance
mode** (§5.5).

This changes [transmission §5](TRANSMISSION.md#5-sampling): today rough
refraction has no value. It gains one so that sampling can be tested
against it; no integrator reads it, since no estimator connects through a
transmissive surface (§6.2).

### 5.2 `BsdfPdf`

The **solid-angle density** with which `BsdfSample` returns `wi` as a
non-delta sample, over the whole sphere, every lobe-selection probability
included. It is the density of the procedure, not of any one lobe, so its
integral is the probability that a sample is non-delta and succeeds:

```text
∫_S² BsdfPdf(wo, wi) dωi  =  P(non-delta sample, success)  ≤  1
```

A delta component, and a sample that fails (a microfacet direction on the
wrong side, total internal reflection with no reflected alternative), take
the rest of the probability. Delta components have no density.

The rough refraction density, `(1 − F) · D_vis · |wi·h| / (wi·h + wo·h/η)²`
times its selection probability, joins `BsdfPdf` with this task;
[transmission §5](TRANSMISSION.md#5-sampling) wrote it down and left it
unread.

### 5.3 `BsdfSample`

`u` is three uniform numbers in [0, 1). The function returns false when
the procedure produces no direction; the path then ends, the energy it
would have carried counting as absorbed. Otherwise:

- **`pdf`** is `BsdfPdf(wo, wi)` for a non-delta sample and 0 for a delta
  sample. It is the full procedure's density, not the chosen lobe's,
  because it is what MIS needs.
- **`weight`** is an unbiased one-sample estimate of the BSDF's action,
  `f · |cos θi| / p` for some density `p` of the choice actually made. It
  may be either form, and each closure documents which it uses:
  - the **mixture ratio** `BsdfEvaluate / BsdfPdf` at `wi`; or
  - a **component ratio**: the chosen component's value over its own
    selection probability times its own density, with the factors that
    cancel analytically cancelled (today's pure metal, the transmitted
    share, and the layered lobes under transmission).

  Both are unbiased, and both are compatible with MIS, whose weight
  depends only on `wi` and `pdf`. A delta sample's weight is the delta
  component's albedo in `wi` over its selection probability.
- **Random numbers.** A sample draws exactly three, in the order the
  integrators draw them today, whatever the model, so both integrators
  stay in step. A model with more discrete choices rescales a number it
  has already used, as today's lobe selection rescales `u.z`. Raising the
  count would change every committed image; it is a decision a task takes
  explicitly with its references, not a side effect.

### 5.4 Delta components

A dielectric interface is a Dirac delta when its roughness is below the
alpha floor (`kMinAlpha`) or it is index-matched. The preview closure's
layered lobes are not: their GGX alpha is clamped at the floor, a
near-mirror with a value and a density, as before this task. It appears only through
`BsdfSample`, with `delta` set and `pdf` 0; `BsdfEvaluate` and `BsdfPdf`
never include it. A closure may mix delta and non-delta components; the
non-delta ones keep their value and density, and the delta selection
probability is what §5.2's integral is missing.

### 5.5 Transmission and transport mode

The functions describe light flowing **from `wi` to `wo`**: camera paths
carry importance and gather radiance, so every function is in radiance
mode. Crossing a solid's interface scales radiance by `1/η²`, with `η`
the relative index `η_wi / η_wo` that the closure holds from `wo`'s side;
a thin wall does not scale. `BsdfEvaluate` and the sample weight carry the
same scale, so §5.3's mixture ratio holds for refraction too.

Importance mode — the adjoint, without `1/η²` and with the shading-normal
correction — is not provided. No estimator here traces from lights; one
that does (light tracing, bidirectional paths) adds the mode with its own
tests, and these functions keep radiance as the default.

## 6. What the integrators guarantee

The functions are pure; the integrators keep the following, identically in
both, since both run `transport.slang`.

### 6.1 Sides

A direction is used only on the side its shading-frame sign and the
geometric normal agree on. A sampled reflection below the geometric
surface, or a transmission above it, ends the path (today's rule). A
light direction below the geometric surface contributes nothing, and with
this task neither does one below the shading normal: with `BsdfEvaluate`
now covering the sphere, next event estimation and ReSTIR's targets ask
for the reflection side explicitly, which returns exactly today's values.

### 6.2 MIS and unweighted light

- Next event estimation weighs a light sample against `BsdfPdf` in its
  direction, which on the reflection side is today's density.
- After a delta or a transmission event, the light the continuation ray
  finds is added unweighted, and the path's stored density is unused
  ([transmission §6](TRANSMISSION.md#6-direct-light-and-mis), unchanged).
  The refraction density this task adds is recorded but read by nothing.
- Shadow rays treat transmissive surfaces as opaque
  ([transmission §7](TRANSMISSION.md#7-shadow-rays-and-the-light-table),
  unchanged).

### 6.3 Reservoir targets and the stored surface

ReSTIR's targets are `BsdfEvaluate · L` on the reflection side, at the
pixel's own surface or at a stored source surface. The
[IR v2 design](MATERIAL_IR_V2.md#63-what-stays-for-lotus-materialx-02)
left open whether reuse stores the closure or the hit to re-evaluate:

**Decision: store the closure.** `RestirSurface` keeps the source
surface's geometry and the closure's reflection-side data, packed per
model. The preview closure keeps today's 128-byte packing. A model whose
packed closure does not fit grows the record to the largest implemented
model; its task reports the growth, and LOTUS-MATERIALX-10 measures the
memory at the fixed benchmarks.

Re-evaluating the material from a stored hit was rejected. The temporal
pass reads surfaces of the previous frame, whose geometry and material
may have changed since; re-evaluation would compute a target different
from the one the sample was selected with, which is a bias unless every
such change also invalidates the history. It would also repeat texture
lookups for every spatial neighbour. A projected, approximate target (a
preview-like closure for every model) keeps the record small and stays
unbiased, but it weakens reuse exactly at coat and sheen highlights; it is
the named fallback if the measured memory is unacceptable.

## 7. Tests

### 7.1 Harness

A `fast`-tier executable, `lotus-bsdf-test` (the ctest
`lotus-renderer-bsdf`), runs the **real shader
source on the CPU**: `slangc -target cpp` compiles a test module that
imports `bsdf.slang` and exports probe functions, and the generated C++,
with the Slang prelude the Vulkan SDK installs, builds into the test. It
needs no device, so hosted CI asserts it on every pull request; it is
registered wherever `slangc` is, which every Vulkan-enabled build already
requires. A GPU compile can differ from the CPU build in floating-point
contraction; that difference is covered by the integrator parity and
reference checks, not here.

The sampler is a fixed-seed generator, so every test is deterministic and
its thresholds are fixed once.

### 7.2 Configurations

Each test runs over a grid of closures and outgoing directions:

| Axis | Values |
| --- | --- |
| Roughness | 0 (delta), 0.02, 0.1, 0.3, 0.6, 1 |
| Metallic | 0, 0.5, 1 |
| Transmission | 0, 0.5, 1; solid and thin wall |
| IOR | 1 (index-matched), 1.33, 1.5, 2.4; entering and leaving |
| Workflow | metallic, specular |
| `wo` | `cos θo` of 1, 0.7, 0.3, 0.05 |

### 7.3 Assertions

1. **Pointwise consistency.** For every non-delta sample, `pdf` equals
   `BsdfPdf(wo, wi)` to a relative tolerance, and `BsdfEvaluate` is
   positive only where `BsdfPdf` is (the sampler covers the support).
   Where the closure documents the mixture ratio, `weight` equals
   `BsdfEvaluate / BsdfPdf`.
2. **Estimator consistency.** Over bins of the sphere, the mean of
   `weight` falling in each bin equals the integral of `BsdfEvaluate`
   over it, within a fixed multiple of its standard error. This holds the
   component-ratio weights of §5.3 to the same value as the mixture ratio.
3. **PDF normalization.** `∫ BsdfPdf` over the sphere, by quadrature,
   equals the fraction of samples that are non-delta and succeed, and is
   at most 1.
4. **Sample distribution.** A χ² goodness-of-fit test of the sampled
   directions against `BsdfPdf` integrated per bin, with low-expectation
   bins pooled, at a significance corrected for the number of
   configurations.
5. **Delta components.** At roughness 0 and index 1, samples are delta,
   along the mirror or straight-through direction, with the Fresnel
   split's probabilities and weights.
6. **Transmission measure.** For a rough solid, refraction samples match
   §5.5's radiance-mode density and value, the `1/η²` included; a thin
   wall's do without it.

Quadrature and χ² bins resolve a lobe only as narrow as their cells, so
assertions 2 to 4 run from roughness 0.1 up; 0.02 is covered by 1 and 5.

Energy conservation, white furnaces and reciprocity are
LOTUS-MATERIALX-03's tests; they need the composition this task does not
change.

## 8. Rejected alternatives in one place

| Alternative | Why not |
| --- | --- |
| Keep `Evaluate` and `Pdf` reflection-only | Rough refraction stays untestable against a density (§5.1). |
| Require `weight = Evaluate / Pdf` always | Forbids the analytic cancellations today's sampler relies on for near-mirror precision (§5.3). |
| A transport-mode parameter now | Every caller passes radiance; an untested adjoint is worse than none (§5.5). |
| A variable random-number count per model | The integrators would fall out of step, and every reference image would move (§5.3). |
| Slang interfaces with dynamic dispatch | The tagged `switch` is explicit and matches the material evaluators (§3). |
| Re-evaluate the material for reuse | Biased under scene changes and repeats texture lookups (§6.3). |
| BSDF tests through rendered images only | Images confirm agreement, not where a density and a sampler disagree (§7). |

## 9. Acceptance for LOTUS-MATERIALX-02

1. `bsdf.slang` provides §3's functions with §5's measures, and the
   preview closure documents which weights are component ratios.
2. `transport.slang` and `restir.slang` call only §3's functions; the old
   names are gone.
3. `lotus-bsdf-test` runs in the `fast` tier and passes §7's assertions.
4. Every committed reference image is unchanged: reference and wavefront,
   Hydra and headless. An image that changes is a defect unless it is
   traced to a rounding difference and recorded.
5. [Transmission §5 and §6](TRANSMISSION.md#5-sampling) and the
   [scene reference](../reference/SCENE.md#path-tracing) point here for
   the BSDF's measures, and `RestirSurface`'s packing rule is recorded in
   [IR v2 §6.3](MATERIAL_IR_V2.md#63-what-stays-for-lotus-materialx-02).
