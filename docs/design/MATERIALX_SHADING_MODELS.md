---
status: accepted
owner: hydra-lotus
---

# MaterialX shading models

> Status: **accepted**, 2026-11-10, merged without review changes. This is the project owner's direction for
> rendering MaterialX's glTF PBR, Standard Surface and OpenPBR surfaces with
> each model's semantics intact, distilled into English with its §1–§11
> numbering kept; §12 records where it departs from earlier policy. It is a
> direction, not an audit of the current code or a proof of conformance to
> any specification; the work it orders starts with the design documents of
> §11. The [scene reference](../reference/SCENE.md#materials-and-environment)
> owns the implemented behaviour. "Today" below describes `main` at v0.9.0.

## 1. Purpose

Build a shading foundation that renders MaterialX's **glTF PBR**, **Standard
Surface** and **OpenPBR** while keeping each model's physical and semantic
differences.

The goal is not complete support of all three models. It is the principal
surface reflection and transmission features of each, evaluated and sampled
consistently, with the supported range stated.

**Success conditions.**

1. A path from MaterialX-authored input to the rendered image exists for all
   three models.
2. Model-specific parameters and layer order are preserved.
3. `Evaluate`, `Sample` and `Pdf` are verified to agree.
4. Existing `UsdPreviewSurface` rendering does not regress.
5. The reference and wavefront integrators keep sharing one BSDF
   implementation.

## 2. Non-goals

Out of scope, or deferred to later deliveries:

- Executing arbitrary MaterialX BSDF node graphs.
- Every MaterialX procedural node and custom shaders.
- OSL integration.
- Full multiple scattering, volume transport, or exact simulation of
  arbitrary layered media.
- Full dispersion, and advanced subsurface or volume features.
- "Apparent support" that silently ignores inputs the implementation does
  not handle.

## 3. Design principles

### 3.1 Shading-model semantics are not merged

**Avoid** flattening the three models' parameters into one uber material
and treating similarly named parameters as the same thing.

**Adopt** a tagged model that keeps each model's structure. Only the BSDF
building blocks and the evaluation interface are shared.

- **glTF PBR:** metallic-roughness at its core; extensions follow glTF's
  composition rules.
- **Standard Surface:** keeps its own lobes, weights and layering.
- **OpenPBR:** keeps in particular the **fuzz → coat → base** order. Fuzz
  is not Standard Surface's sheen.

Parameters with the same name — IOR, roughness, coat, sheen or fuzz,
normal — may mean different things in different models, so no implicit
conversion between them is made.

### 3.2 MaterialX translation is separate from rendering

```text
OpenUSD / MaterialX input
         │
         ▼
MaterialX / Hydra translation
  ├─ graph resolution
  ├─ supported node lowering
  ├─ constant folding
  └─ diagnostics
         │
         ▼
Lotus Material IR v2
  ├─ shading model identity
  ├─ model-specific parameters
  ├─ texture / parameter expressions
  └─ layer semantics
         │
         ▼
GPU material parameter evaluation
         │
         ▼
Model-specific BSDF composition
         │
         ▼
Shared BSDF primitives and APIs
         │
         ▼
Reference PT / Wavefront PT
```

Any dependency on the MaterialX library stays in the adapter and translator
layer; it does not enter the rendering core. Today the translator reads
Hydra's material network and links no MaterialX library.

### 3.3 The GPU representation is independent

The CPU side is a tagged representation whose semantics read clearly. The
GPU side carries an explicit tag and a fixed or compressed layout. A CPU
`std::variant` is not the GPU ABI.

Conceptually:

```cpp
enum class ShadingModel : uint8_t {
    UsdPreviewSurface,
    GltfPbr,
    StandardSurface,
    OpenPbr,
};

struct MaterialDefinition {
    ShadingModel model;
    // Model-specific parameter set or handle
};
```

Watch compatibility of the existing API, the amount of data transferred,
and branching and register use in the wavefront integrator.

### 3.4 One BSDF API, per-model composition

Shared building blocks include:

- Diffuse (starting from the existing model)
- GGX microfacet, dielectric and conductor
- Transmission and Fresnel
- The dedicated distributions and evaluation that sheen and fuzz need
- Mixing and layering of lobes
- Emission, through an EDF path separate from the BSDF

Each model's composition rules are managed independently. Conceptually the
BSDF API keeps three functions consistent:

```cpp
BsdfEval   Evaluate(material, context, wi, wo);
BsdfSample Sample(material, context, wo, rng);
float      Pdf(material, context, wi, wo);
```

The direction, contribution and PDF that `Sample` returns share their
definitions and measure with `Evaluate` and `Pdf` in the same direction.
Delta components, the measure of transmission, and the difference between
transport modes under a relative IOR are handled explicitly.

## 4. Layered BSDFs

The cross-cutting foundation this direction weighs most.

- **Mix and layer are different.** A weighted sum is not optical layering,
  where light passes the upper layer to reach the lower one.
- **Fresnel and attenuation are respected.** A structure that merely adds a
  coat's reflection on top is not allowed.
- **Sampling distributions match.** The probability of choosing a lobe need
  not equal its contribution weight.
- **Normals stay independent.** Coat and base can have different normals.
- **The model's specification comes first.** Rather than first building a
  general, complete layered-transport solver, implement the behaviour each
  model requires in a verifiable form.

OpenPBR's conceptual order:

```text
Incoming Light
     │
     ▼
 Fuzz layer
     │
     ▼
 Coat layer
     │
     ▼
 Base components
```

The diagram shows the model's structure; it does not restrict a path's
actual transport to a single straight passage.

## 5. Boundary of MaterialX graph support

Not general node-graph execution: start from a **practical subset that
supplies each model's inputs**.

| Node family | Policy |
| --- | --- |
| Constant / parameter | Required |
| Image / texture coordinate | Required |
| Normal map / tangent handling | Required |
| Colour-space conversion | Required |
| Add / multiply | Required |
| Mix / clamp | Recommended |
| Other math and utility nodes | Chosen from actual use |
| Procedural noise | Later |
| Arbitrary BSDF graphs | Later |
| Custom shaders / OSL | Out of scope |

**Unsupported nodes and connections are diagnosable.** Each is defined to
warn, show an explicit substitute, or fail to load. An input is never
dropped silently while the result renders as if it were correct.

Colour space, texture channels, UVs, normal and tangent space, and the
defaults of unconnected inputs are fixed per specification and verified
with fixtures.

## 6. Implementation priority

### Phase A — Foundation and glTF PBR

Start from the model closest to today's metallic-roughness material, and
use it to validate IR v2, the BSDF API and the first multi-lobe support.

| Feature | Priority |
| --- | --- |
| Base colour / metallic / roughness | P0 |
| Normal / emission / opacity | P0 |
| Basic translation of MaterialX input | P0 |
| IOR / transmission / clearcoat | P1 |
| Sheen | P1 |
| Anisotropy / iridescence | P2 |
| Volume attenuation | P2 (supported range to be defined) |
| Dispersion | Later |

### Phase B — Standard Surface

- Base diffuse and specular combined with the metallic part.
- The principal surface properties: coat, sheen and anisotropy.
- Transmission integrated with the existing mechanism.
- Subsurface, thin film and the like are limited or deferred on quality and
  cost, and stated feature by feature.
- Standard Surface's own composition rules are verified and not forced onto
  the other models.

### Phase C — OpenPBR

- The principal base reflection and transmission features.
- **Fuzz over coat over base**, represented correctly.
- The dedicated fuzz BSDF, and the coat's attenuation and sampling,
  verified.
- Different normals, roughness and similar inputs between coat and base,
  sorted out.
- No implicit equivalence with Standard Surface because they look alike.

### Phase D — Integration and quality assurance

- Comparisons on standard scenes and fixtures for all three models.
- Reference/wavefront parity and performance-regression checks.
- The existing `UsdPreviewSurface` tests kept.
- A published list of unsupported features, the implemented specification
  and known limitations.

## 7. Tests and quality gates

| Test | Purpose | Note |
| --- | --- | --- |
| Evaluate / Sample / Pdf consistency | Unbiasedness and API agreement | Delta and transmission verified separately |
| PDF normalization | Direction sampling is valid | Measures and event probabilities stated |
| White furnace | Energy conservation | Criteria suited to the model and incidence |
| Reciprocity | Scattering is reciprocal | Transport mode and IOR correction made explicit |
| MaterialX reference comparison | Model behaviour follows the specification | Independent reference implementations where possible |
| Reference/wavefront parity | The integrators agree | They can share the same error, so this is no substitute for an independent truth |
| Regression scenes | Existing features kept | `UsdPreviewSurface` is mandatory |
| Performance / memory | Load in real GPU use | Observe divergence, registers, shader size and VRAM |

Comparison tolerances are defined per lobe, input domain, sample count and
confidence interval of each stochastic test. Image matching alone does not
decide completion.

CI runs fast numerical and translation tests by default; heavy rendering
comparisons are limited in scene count and frequency. Nothing assumes a
nightly run.

## 8. Tasks

| ID | Task | Main deliverables |
| --- | --- | --- |
| LOTUS-MATERIALX-01 | Material IR v2 | Per-model data design, CPU/GPU layouts |
| LOTUS-MATERIALX-02 | BSDF API | Evaluate / Sample / Pdf contract shared by both integrators |
| LOTUS-MATERIALX-03 | Layer / mix foundation | Multiple lobes, attenuation, PDF consistency |
| LOTUS-MATERIALX-04 | MaterialX graph translation | Supported node subset, diagnostics |
| LOTUS-MATERIALX-05 | glTF PBR core | Basic material behaviour, fixtures |
| LOTUS-MATERIALX-06 | glTF PBR extensions | The P1 extensions and their tests |
| LOTUS-MATERIALX-07 | Standard Surface | Principal lobes and its own composition |
| LOTUS-MATERIALX-08 | OpenPBR | Principal features including fuzz, coat and base |
| LOTUS-MATERIALX-09 | MaterialX validation | Independent reference comparisons, furnace tests |
| LOTUS-MATERIALX-10 | Release gate | Integrator parity, regressions, performance, compatibility table |

**Dependencies:** 01–03 come first. 04 can partly run in parallel. Models
grow 05 → 06/07 → 08. 09 is not a final step; it runs from each stage on.

## 9. Scope tracking and acceptance

Each model and feature is tracked as one of:

- **Supported:** input translation, evaluation, sampling and verification
  fixtures are all in place.
- **Partial:** usable within stated limits.
- **Unsupported:** not handled; input raises a diagnostic.

The delivery's acceptance:

1. A representative textured material renders end to end in all three
   models.
2. The principal coat, sheen and fuzz layering semantics are preserved and
   independently verified.
3. The BSDF API consistency tests pass.
4. Existing `UsdPreviewSurface` scenes do not regress.
5. The reference and wavefront integrators agree as specified.
6. Unsupported features are listed in a per-model compatibility table, and
   input diagnostics exist for them.
7. The performance and memory impact is measured, and any serious
   regression is fixed or recorded.

## 10. Decisions for the design review

Settled in a design review when implementation starts. §10.1, §10.3 and
§10.6 are settled by the [material IR v2 design](MATERIAL_IR_V2.md).

1. **IR v2 granularity:** per-model structures, and how texture expressions
   are held. *Settled:* [tagged models and one affine lookup per input](MATERIAL_IR_V2.md#4-cpu-representation-contract-101).
2. **Layering approximation:** agreement with the target specification, and
   the treatment of multiple scattering.
3. **MaterialX version:** the specification fixed as the basis for
   features, defaults and implementation comparisons. *Settled:*
   [MaterialX 1.39.5 and its node definitions](MATERIAL_IR_V2.md#3-versions-contract-103).
4. **Input paths:** how a Hydra network and a MaterialX document are each
   handled.
5. **Feature boundary:** how much of Standard Surface's and OpenPBR's
   subsurface, thin film and volume enters this delivery.
6. **GPU strategy:** switch dispatch or shader specialization, with the cost
   of model branching evaluated. *Settled:*
   [switch dispatch first](MATERIAL_IR_V2.md#6-shader-structure-contract-106),
   specialization only on measurement.

## 11. Recommended order

First write the **Material IR v2 design and the BSDF API contract**, and
render the existing `UsdPreviewSurface` through the new path. Then complete
the glTF PBR core as the smallest end-to-end slice, and grow features with
layer and mix verification alongside. Standard Surface and OpenPBR share
that foundation and each implement **their own composition rules**.

> Success is not three more model names. It is a verifiable Lotus shading
> architecture that extends without losing MaterialX's semantics.

**Specifications** (pin the versions when implementing):
[MaterialX](https://github.com/AcademySoftwareFoundation/MaterialX),
[MaterialX PBR specification](https://github.com/AcademySoftwareFoundation/MaterialX/blob/main/documents/Specification/MaterialX.PBRSpec.md),
[OpenPBR Surface](https://academysoftwarefoundation.github.io/OpenPBR/),
[glTF 2.0](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html).
The implementation names the MaterialX, OpenPBR and glTF versions it adopts.

## 12. Relation to earlier policy

This direction changes four earlier positions; each owner now points here.

| Earlier position | Change |
| --- | --- |
| [Roadmap policy, Renderer Phase 8](ROADMAP_POLICY.md#renderer-phase-8--production-material-support): every source is translated into one material IR, and "shader implementations do not multiply per material source"; targets in the order `UsdPreviewSurface`, OpenPBR, Standard Surface, glTF PBR | The IR stays the single boundary between translation and rendering, but it carries the shading model's identity, and BSDF composition is per model over shared primitives (§3). Material sources still do not get renderers of their own. The order of work is glTF PBR, Standard Surface, OpenPBR (§6), since glTF PBR is closest to today's material; `UsdPreviewSurface` stays first as the regression baseline. |
| [Strategy §5](POST_V060_STRATEGY.md#5-entry-conditions-for-later-work): production-material expansion starts from demonstrated asset gaps | This delivery is chosen by direction rather than by an asset gap. Its entry condition is this contract and §11's design documents, and §7's gates replace the asset survey as evidence. |
| [v0.8.0's scope](../roadmap/README.md#v080--materials--lighting): coat and OpenPBR enter only with an asset gap and, for new lobes, a transport contract | The transport contract still comes first: LOTUS-MATERIALX-02 and 03 are it. |
| [DES-Q6](DESIGN_POLICY.md#53-open-questions): how a glTF PBR–compatible mapping reaches Lotus | Through MaterialX's glTF PBR surface on the composed stage, as the [integration scope](INTEGRATION_SCOPE_POLICY.md#1-the-rule) requires; no glTF file is parsed. |

Today's material is the single
[`Lotus::Material`](../reference/SCENE.md#materials-and-environment), with
`UsdPreviewSurface`'s defaults. The
[Standard Surface subset](../reference/SCENE.md#materialx-standard-surface)
is translated into it and warns for every input it cannot represent, and
other MaterialX surfaces, OpenPBR's included, warn and fall back to the
default material. IR v2 replaces that flattening for the three models;
`UsdPreviewSurface` keeps its current behaviour.
