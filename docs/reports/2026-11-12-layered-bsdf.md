# Layered BSDFs

## Evaluation brief

LOTUS-MATERIALX-03 implements the accepted
[layered BSDF contract](../design/LAYERED_BSDF.md): the primitive lobes,
the `mix`, `add`, `multiply` and `layer` operations, per-lobe normals,
attenuation, one-choice sampling and the directional albedo tables. Adopt
it if the modules exist and the probe compiles them, `lotus-bsdf-test`
passes section 7's assertions in the `fast` tier, every committed
reference image is unchanged, and this report records the thresholds,
the measured losses and the run time
([acceptance](../design/LAYERED_BSDF.md#9-acceptance-for-lotus-materialx-03)).
Three points depart from the contract's text; review accepted them
([below](#changes-to-the-contract)).

## What changed

- **Lobes** ([`lobes.slang`](../../backend/vulkan/shaders/bsdf/lobes.slang)).
  `Lambert`, `GgxSchlick` (exponent 5, alpha clamped at the floor, never
  delta) and `GgxDielectric` (`R`, `T` or `RT`, solid or thin wall, delta
  below the floor or at index 1), each in the frame about its own normal:
  value, density, sampler. The single-scattering terms reuse
  `bsdf.slang`'s microfacet helpers, now public; the preview closure is
  untouched.
- **Composed closure**
  ([`composed.slang`](../../backend/vulkan/shaders/bsdf/composed.slang)).
  A flat list of at most six lobes with RGB weights and an optional
  attenuation. `ComposedAdd` computes a lobe's compensation, albedo and
  selection estimate for `wo`; `ComposedScale`, `ComposedMix` and
  `ComposedLayer` act on index ranges (adding is concatenation);
  `ComposedAttenuate` attaches the path-length absorption;
  `ComposedFinalize` sets the selection probabilities, floor 0.05.
  `ComposedEvaluate`, `ComposedPdf` and `ComposedSample` read no table.
  `BsdfClosure` is unchanged: no model builds a composed closure yet.
- **Tables** ([`albedo.slang`](../../backend/vulkan/shaders/bsdf/albedo.slang)).
  `E1`, `Eschlick` and `Edielectric` behind an `IAlbedoTables` interface,
  so a storage buffer on the GPU and a pointer in the probe bind the same
  data. The data is
  [`validation/bsdf-albedo-tables.inc`](../../validation/bsdf-albedo-tables.inc),
  75,776 floats (716 KB of text), written by the new `lotus-bsdf-tables`.
- **Spectrum.** `operator-`, `SpectrumSaturate` and `SpectrumPow`.
- **Probe.** `validation/bsdf-probe.slang` exports the composed closure's
  three functions and its albedo, built from a small program of lobes and
  operations, and the tables' layout, nodes and lookups.

## The tables

**Generation.** For every node, `lotus-bsdf-tables` builds a closure of
the one uncompensated lobe at the node's `wo` and integrates its sample
weight over the sampler's square, `E(wo) = ∫ f cos θi / p (wi(u)) du`, by
adaptive 4 × 4 Gauss–Legendre panels (4 × 4 initial, split at least once,
to 2e-5 absolute over the square, at most 7 levels). A fixed rule
converged only as `1/N`: where the reflected direction crosses the
horizon, the weight falls from about 1 to 0 within a band of width of
order `α²`. An independent double-precision Monte Carlo integration
agreed with the tables, for example `E1` at `cos θo` 1 and alpha 1 is
0.307 (0.3069 ± 0.0003). Generation takes 3.7 min on 32 threads.

**Axes.** Each axis is `log v + k v`, normalized: logarithmic where the
features scale (the grazing dip of `E1` sits at `cos θo ≈ α`, and a
dielectric's grazing reflectance changes over `sqrt(η − 1)`), close to
linear where Fresnel terms and wide lobes vary.

| Axis | Range | Nodes | `k` |
| --- | --- | ---: | ---: |
| `cos θo` | 2e-4 to 1 (lookups below take the first node) | 32, dielectric 48 | 4 |
| alpha | 1e-3 to 1 | 32 | 2 |
| relative index | 1.01 to 3, entering, leaving and thin-wall sheets | 16 | 0.5 |

The leaving sheet's cosine axis is split at the critical angle, which is
always node 24: below it, `log μ − log τ + k μ` with `τ` the evanescent
cosine, so that both the grazing end and the critical edge are resolved;
above it, the refracted cosine on the common axis. Splines never cross the
node.

**Interpolation** is Catmull-Rom along every axis, with linearly
extrapolated nodes beyond an edge: 16 reads for a 2D lookup, 64 for the
dielectric's.

**Accuracy**, every node and every cell centre, against a fresh
quadrature (`lotus-bsdf-tables --check`):

| Part | Nodes (repeat) | Centres, largest error | Where |
| --- | ---: | ---: | --- |
| `E1` | 6.0e-7 | 9.7e-4 | `cos θo` 0.0012, alpha 0.0012 |
| `Eschlick` | 6.3e-7 | 8.5e-4 | the same cell |
| `Edielectric` entering | 8.5e-7 | 9.2e-4 | index 1.01, `cos θo` 0.004, alpha 0.0012 |
| `Edielectric` leaving | 4.3e-5 | 1.1e-2 | just inside the critical angle, index 2.8, alpha 0.02 |
| `Edielectric` thin wall | 9.4e-7 | 1.1e-3 | index 1.01, `cos θo` 0.11, alpha 0.0015 |

Node errors are the data's rounding and the quadrature's repeatability;
the leaving sheet's 4.3e-5 is at a node on the critical angle itself, at
the alpha floor.

### How the resolution was found

| Step | `E1` | `Eschlick` | Leaving sheet |
| --- | ---: | ---: | ---: |
| Linear, 32 × 32, `x²` axes, a node at `cos θo` 0 | 0.51 | 0.49 | 0.72 |
| Linear, 48 × 32, log axes | 3.5e-3 | 3.2e-3 | — |
| Linear, 64 × 48, log axes | 1.8e-3 | 1.7e-3 | — |
| Catmull-Rom, 32 × 32, slopes 4 and 2 | 9.7e-4 | 8.5e-4 | — |
| Catmull-Rom, dielectric 32 cosines, critical node | — | — | 2.9e-2 |
| Catmull-Rom, 48 cosines, `τ` axis below the edge | — | — | 1.1e-2 |

The first row's node at `cos θo` 0 was wrong in kind: `E1` does not fall
to 0 at grazing but tends to about 1, since the visible microfacets of a
grazing view face it and reflect upward.

## The test

`lotus-bsdf-test` keeps the BSDF API's 2,040 preview configurations
unchanged and adds 880 composed ones: 220 closures, each at `cos θo` 1,
0.7, 0.3 and 0.05, seeded by index after the preview grid's. A closure is
described to the probe by a program; `--composed <family>` runs one
family. The statistics runner is shared: a `Target` carries the functions,
lobe peaks and expectations.

| Family | Closures | Asserted |
| --- | ---: | --- |
| `lambert` | 2 | furnace (white), reciprocity |
| `schlick`, `schlick-uncompensated` | 20 | furnace (white compensated), loss (white uncompensated), bound, reciprocity |
| `dielectric-r`, `-rt`, `-t` | 90 | bound or loss, reciprocity across the interface, Walter et al. (rough `RT` alone) |
| `mix` | 18 | furnace (white, `m` 0, 0.3, 1), bound, reciprocity |
| `coat-lambert` | 30 | furnace, coat roughness 0 to 1, index 1, 1.5, 2.4, compensated or not |
| `coat-metal` | 12 | furnace (white metal), bound (tinted) |
| `coat-normal` | 8 | furnace, coat tilted 0°, 15°, 45° toward `wo` and 45° away |
| `coat-absorption` | 4 | furnace (white `T`), bound (tinted) |
| `gltf-shape` | 24 | `layer(coat, mix(layer(specular, Lambert), metal))`: furnace (white), bound |
| `coat-transmission` | 12 | `RT` under a coat, solid and thin wall: loss |

**Assertions** (section 7):

1. The BSDF API's six on every composed configuration, with the mixture
   ratio required everywhere; assertion 5's delta directions are each
   delta lobe's mirror or refraction about its own normal, and
   assertion 6 applies to a rough `RT` interface alone.
2. **Furnace.** The albedo, the mean energy weight including delta samples
   and failures (a transmitted sample through a solid counts `η²` times
   its radiance weight), is 1 within the family's tolerance plus 5
   standard errors at every `wo`.
3. **Energy bound.** No configuration's albedo exceeds 1 + 5e-3 plus 5
   standard errors. Uncompensated and transmitting configurations lose
   less than their roughness's bound.
4. **Reciprocity.** At 64 random directions, `f(wo, wi) = f(wi, wo)` for
   reflection and `f(wo, wi) = f(wi, wo) / η²` across a solid's interface,
   the reverse closure built for the other side.
5. **Structure.** At 64 random directions, a layer's value equals
   `f_top + clamp(1 − E_top(wo)) · f_base` from separately built top and
   base closures and the top's albedo.
6. **Tables.** At 61 to 65 nodes per part, integrated as the generator
   does, and at 193 to 201 cell centres per part, integrated to 1e-4; the
   full comparison is `lotus-bsdf-tables --check`.

### Thresholds and measured values

Fixed once from the deterministic run:

| Measure | Threshold | Largest observed |
| --- | ---: | ---: |
| Furnace, Lambert | 1e-5 | 0 |
| Furnace, one compensated `GgxSchlick` or a mix with it | 5e-3 | 3.6e-3 (Schlick), 9.9e-3 (mix, within 5 standard errors) |
| Furnace, a coat over Lambert, tilted or absorbing | 1.5e-3 | 2.1e-3 (within 5 standard errors), 7.9e-4, 1.4e-3 |
| Furnace, a coat over a compensated metal | 6.5e-3 | 1.1e-2 (within 5 standard errors) |
| Furnace, the glTF shape | 8e-3 | 7.9e-3 |
| Energy bound, albedo − 1 | 5e-3 | 0 |
| Reciprocity, reflection | 1e-4 | 4.6e-6 |
| Reciprocity, refraction | 1e-2 | 8.0e-3 |
| Structure, relative | 1e-4 | 2.2e-7 |
| Tables, nodes | 1e-4 | 6.4e-6 (4.3e-5 over every node) |
| Tables, centres | 1e-3; thin wall 1.2e-3; leaving 1.2e-2 | 8.9e-4, 7.1e-4, 5.0e-3 (subset) |

The refraction reciprocity threshold is the BSDF API's for the same
arithmetic: float rounding in Walter et al.'s Jacobian, which BSDF API
assertion 6 measures at 5.4e-3 against double precision. The coat and
metal furnaces' larger deviations are Monte Carlo noise at grazing `wo`
with a near-mirror coat; the means are within their five standard errors.

**Losses**, the largest `1 − albedo` over `cos θo` by roughness, and the
bounds the test holds:

| Roughness | Uncompensated white `GgxSchlick` | White `GgxDielectric RT` (solid and thin) | `RT` under a coat | Bound |
| ---: | ---: | ---: | ---: | ---: |
| 0 | 2e-4 | 0 | 1.1e-3 | 0.003 |
| 0.1 | 0.024 | 0.024 | — | 0.03 |
| 0.3 | 0.092 | 0.133 | 0.075 | 0.16 |
| 0.6 | 0.208 | 0.306 | — | 0.37 |
| 1 | 0.697 | 0.692 | — | 0.84 |

Single scattering loses most at normal incidence on the roughest
surfaces: at alpha 1 a large share of the visible normals at `cos θo` 1
tilt beyond 45° and reflect below the horizon, so `E1` is 0.31 there. Compensated
lobes recover it; refraction is not compensated in this task
([§4.2](../design/LAYERED_BSDF.md#42-multiple-scattering)).

### Sensitivity

Each of three deliberate defects in `composed.slang` fails the test:

| Defect | Family run | Failures reported |
| --- | --- | ---: |
| A dielectric's layer albedo looked up at normal incidence | `coat-lambert` | 46 (furnace) |
| `ComposedPdf` omits the last lobe | `mix` | 120 (assertions 2 and 3) |
| Compensation `1 + F0 (1 − E1)` without the `/E1` | `schlick` | 8 (furnace) |

A configuration reports at most four failures.

A first attempt at the first defect changed the `wo` given to
`ComposedAlbedo`, which reads it only for attenuation, and changed
nothing; the albedo itself is fixed when the lobe is added.

## Changes to the contract

Recorded in [contract §11](../design/LAYERED_BSDF.md#11-changes-found-in-implementation)
and accepted in review, 2026-11-12:

1. **Catmull-Rom instead of linear interpolation.** Linear interpolation
   missed 1e-3 even at 64 × 48 (see the table above); cubic interpolation
   reaches it at 32 × 32, for 16 to 64 reads per lookup at closure build.
2. **The thin-wall and leaving sheets' measured bounds**, 1.2e-3 and
   1.2e-2, instead of 1e-3. More leaving-sheet resolution is possible, at
   about 11 minutes per measurement over the full grid; the sheet is read
   only for a reflecting layer seen from inside its solid.
3. **A dielectric's tint is left out of the albedo a layer above sees**, as
   MaterialX does.

## Verification

On an NVIDIA RTX A5000 host (Windows, MSVC 19.51, Release, Vulkan SDK
1.4.350):

- **The test.** `ctest -R "lotus-renderer-bsdf|lotus-test-tiers"` passed.
  `lotus-bsdf-test` takes 12 s on 32 threads and 82 s pinned to four
  cores, against 22 s before: the composed configurations add about 42 s
  and the table assertion 18 s. The `fast` tier observes timing and does
  not threshold it ([test policy](../design/TEST_POLICY.md)).
- **Render neutrality.** No shader the integrators compile imports the new
  modules. All 22 SPIR-V binaries of the core tree, rebuilt after the
  changes to `bsdf.slang` and `spectrum.slang`, are byte-identical to
  `main`'s, so every reference image, reference and wavefront, Hydra and
  headless, is unchanged without a rerun.
- **GPU compilation.** A throwaway compute shader that builds a coat over
  a metal-and-diffuse mix from a `StructuredBuffer` and samples it
  compiles with `slangc -target spirv -warnings-as-errors all` and passes
  `spirv-val`.

## Decision

Adopt, with the three changes above, accepted in review. The lobes,
operations, normals, attenuation, sampling and tables exist in shader
modules the probe compiles; `lotus-bsdf-test` passes in the `fast` tier;
every reference is unchanged; and contract §10.2 was settled with the
contract. LOTUS-MATERIALX-04 (the node subset) can proceed; the first
model, glTF PBR (LOTUS-MATERIALX-05), wires the composed closure into
`BsdfClosure`, binds the tables and measures `kMaxLobes`'s register cost.
