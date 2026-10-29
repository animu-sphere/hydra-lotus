---
status: proposed
owner: hydra-lotus
---

# Dielectric transmission

> Status: **proposed**, 2026-10-29. This document is the transport contract
> for the fifth lobe of the
> [design policy's material order](DESIGN_POLICY.md#17-material), written
> before any code as LOTUS-TRANSMISSION-01 requires. It extends the
> implemented [material contract](../reference/SCENE.md#materials-and-environment)
> and [path tracing contract](../reference/SCENE.md#path-tracing); where it
> is silent they hold unchanged. Implemented behaviour will be documented
> in the [scene reference](../reference/SCENE.md), which then owns it.

## 1. Scope

A dielectric interface that refracts as well as reflects: glass, clear
plastic and liquids seen as a surface, smooth or rough, as a closed solid or
as a thin wall. The light it transmits is tinted at the interface; nothing
is absorbed or scattered inside the medium.

Not in this contract: volume absorption and scattering inside the medium
(MaterialX `transmission_depth`, `transmission_scatter`), dispersion,
nested dielectrics and their priorities, caustics through light sampling
(§7), subsurface scattering, coat, multiple-scattering energy compensation
for rough interfaces, and texture-driven transmission inputs. Each needs
an asset gap and, for a new lobe, a contract of its own
([strategy §5](POST_V060_STRATEGY.md#5-entry-conditions-for-later-work)).

## 2. Material IR

`Material` gains three constant fields. Like `ior`, they have no texture
inputs.

| Field | Default | Range | Meaning |
| --- | --- | --- | --- |
| `transmission` | `0` | [0, 1] | share of the non-metal part that is a transmitting dielectric interface instead of the coat over Lambert |
| `transmission_color` | `(1, 1, 1)` | each in [0, 1] | tint of the light the interface transmits, applied at every crossing |
| `thin_walled` | `false` | boolean | the surface is a thin wall enclosing no medium, rather than the boundary of a solid |

The surface's BSDF becomes, with metal fraction `m` and transmission `t`:

```text
f = m · f_metal + (1 − m) · [ (1 − t) · f_coat + t · f_dielectric ]
```

`f_metal` and `f_coat` (the dielectric GGX coat over Lambert) are the
existing lobes, unchanged. `f_dielectric` is §4's lobe, with the same
`roughness` and `ior`. `base_color` no longer reaches the transmitted
share; `transmission_color` does instead.

The interface is a dielectric against air: `ior` stays relative to air,
finite and at least 1, so light refracts towards the normal on entry, and
total internal reflection happens only on leaving. At `ior = 1` the
interface vanishes, as it does for the coat: the transmitted share passes
straight through, tinted, at any roughness, and reflects nothing.

The specular workflow has no index to refract with. A material with
`use_specular_workflow` and positive `transmission` throws
`std::invalid_argument`, as do out-of-range or non-finite values of the new
fields, without changing the world. `opacity` stays presence coverage: the
coverage test runs first, and an accepted surface has its full BSDF,
transmission included. `UsdPreviewSurface` opacity is never transmission.

## 3. Sides of the interface

Shading normals interpolate, can be mapped, and are turned towards the
incoming ray, so they cannot say which side of a solid a ray is on. The
geometric normal decides:

- The **outward normal** is the triangle's winding normal,
  `cross(p1 − p0, p2 − p0)` in world space, times the sign of the
  determinant of the instance transform's linear part. Hydra's
  triangulation already normalizes left-handed winding, and the sign keeps
  a mirroring transform from turning a solid inside out.
- A ray whose direction opposes the outward normal **enters** the solid:
  `η_i = 1`, `η_t = ior`. Otherwise it **leaves**: `η_i = ior`,
  `η_t = 1`. The relative index is `η = η_t / η_i`.
- A thin wall has no inside: every crossing uses `η = ior` against air on
  both sides, and no radiance scaling applies.
- A solid must be closed and wound outwards. Open or inconsistently wound
  meshes give inconsistent indices along a path. This is not detected.

The BSDF is evaluated in the existing shading frame (z along the shading
normal, turned towards the incoming direction `wo`). A reflected direction
must lie above both the shading normal's hemisphere and the geometric
surface on the incoming side; a transmitted one below both. A sampled
direction that disagrees ends the path without resampling, as a reflected
direction below the geometric surface does today. Ray origins are offset
along the geometric normal towards the side the continuation leaves into.

## 4. The dielectric lobe

**Fresnel.** The transmitted share uses the exact unpolarized dielectric
Fresnel reflectance `F(cos θ, η)` (Pharr et al., *Physically Based
Rendering*, 4th edition, section 9.3), which reaches 1 at the critical
angle on leaving. The coat keeps its Schlick approximation; only `f_coat`
uses it.

**Smooth interface.** Where `roughness²` is below the existing alpha floor
of 10⁻³, the transmitted share is smooth, and both of its directions are
Dirac deltas:

- reflection about the shading normal with weight `F`;
- refraction by Snell's law with weight
  `(1 − F) · transmission_color / η²` for a solid; under total internal
  reflection `F = 1` and only reflection remains.

A thin wall sums the interreflections inside the wall
(Pharr et al., section 9.5): with `R = F(cos θ, ior)`, it reflects
`2R / (1 + R)` and transmits `(1 − R) / (1 + R) · transmission_color`
straight through, in direction `−wo`.

The coat and metal lobes keep their near-mirror GGX at the alpha floor; only
the transmitted share switches to deltas.

**Rough interface.** Otherwise the transmitted share is the single-scattering
GGX dielectric of Walter et al. (*Microfacet Models for Refraction through
Rough Surfaces*, 2007), with alpha `roughness²` and the existing
height-correlated Smith term `G2`:

```text
reflection   (wi.z > 0):  h = normalize(wo + wi)
  f · |wi.z| = F(wo·h) · D(h) · G2 / (4 · wo.z)
transmission (wi.z < 0):  h = ±normalize(wo + η · wi), h.z > 0
  f · |wi.z| = tint · (1 − F(wo·h)) · D(h) · G2 · |wi·h| · |wo·h|
               / (wo.z · (wi·h + wo·h / η)²) / η²
```

`tint` is `transmission_color`.

A thin wall replaces refraction by the reflection lobe mirrored through the
tangent plane: a transmitted `wi` is evaluated as the reflected direction
`(wi.x, wi.y, −wi.z)` with `2R / (1 + R)` and `(1 − R) / (1 + R) · tint` in
place of `F` and `1 − F`. Rough thin walls therefore blur what is behind
them without displacing it.

**Radiance scaling.** Camera paths carry importance and gather radiance,
so the solid's transmission divides by `η²` (radiance mode, Pharr et al.,
section 9.7). A path that enters and leaves a solid gets `η²` and `1/η²`,
which cancel. The scaling matters only for paths that end inside a solid.

**Reciprocity.** The reflection lobes are reciprocal. The transmission lobe
is not: exchanging its directions scales it as Veach's generalized
reciprocity requires (*Robust Monte Carlo Methods for Light Transport
Simulation*, 1997, section 5.2), `f(wi, wo) = (η_t / η_i)² · f(wo, wi)`
with `η_i` on `wo`'s side. Nothing traces light paths, so only this
radiance form is implemented, not its adjoint.

**Energy.** A smooth interface and a smooth thin wall reflect and transmit
exactly `R + T = 1` before tinting. A rough interface loses the energy of
microfacet multiple scattering, increasingly with roughness, as the
existing GGX lobes do; this is not compensated.

## 5. Sampling

The surface still draws three random numbers per scattering event, in the
same order, and a material whose `transmission` is 0 takes the existing
code path exactly, so every committed reference image and the reference
check's bitwise reproduction are unaffected.

- The lobe-selection number chooses the transmitted share with probability
  `P_t = (1 − m) · t`. Otherwise it is rescaled to [0, 1) and chooses
  among the existing lobes as today, with the metal fraction renormalized
  over the remaining shares, `m / (1 − P_t)`.
- In the transmitted share, a rough interface samples a visible normal for
  `wo` with the existing spherical-cap sampler; a smooth one uses the
  shading normal. The rescaled selection number then reflects with
  probability `F(wo·h)` (for a thin wall `2R / (1 + R)`) and transmits
  otherwise. The weight is `G2 / G1` for a rough reflection,
  `tint · G2 / G1 / η²` for a rough refraction, 1 and `tint / η²` for the
  smooth ones, and the thin wall's without `η²`; the densities cancel
  analytically, as they do today for a pure metal.
- `BsdfPdf` is the non-delta lobes' mixture density, `P_t` times the
  transmitted share's density (`F · D_vis / (4 wo·h)` for reflection, `(1
  − F) · D_vis · |wi·h| / (wi·h + wo·h / η)²` for refraction) plus
  `(1 − P_t)` times the existing mixture. Delta lobes have no density and
  `BsdfEvalCos` returns no value for them.
- Each transmission event counts as a scattering event towards
  `max_bounces` and Russian roulette, so a solid consumes at least two.

## 6. Direct light and MIS

Next event estimation at a transmissive surface samples the light table as
today, on the incoming side only. A light point below the geometric surface
still contributes nothing: a shadow ray cannot follow a refracted
direction. Its BSDF value and MIS density are those of the non-delta
reflection lobes, the transmitted share's rough reflection included.

Light found by a sampled direction is weighed as today, except after a
**delta event or any transmission event**. There next event estimation
had no density for the direction, so the emission the continuation ray
finds, the analytic lights it crosses and the environment image's radiance
when it escapes are all added unweighted. The path records this in its
flags; neither integrator draws extra random numbers for it.

A surface whose BSDF is entirely delta (smooth, `t = 1`, `m = 0`) still
draws next event estimation's four numbers and adds nothing. Light behind
or inside glass is found by BSDF sampling alone, which is unbiased and
converges slowly for small lights; §7 defers the alternative.

## 7. Shadow rays and the light table

Shadow rays treat an accepted transmissive surface as opaque, after the
existing coverage test. Passing them through a thin wall or a smooth
interface with the interface's transmittance would be caustic transport
through light sampling, which is deferred: for a solid it is biased
without refraction, and for a thin wall it needs its own MIS treatment.

The light table is unchanged. An emitter with transmission keeps its
weight, and its emission is added from both sides as today.

## 8. Reservoirs and temporal inputs

**ReSTIR initial lighting** at the primary surface uses next event
estimation's measure: candidates' targets are the non-delta reflection-side
`f · L` of §6, and lights on the transmitted side have target 0. A
fully delta primary surface keeps empty reservoirs, and BSDF sampling
carries all its light. Temporal and spatial reuse keep their current
compatibility rules, the material identifier included, and evaluate the
same target from the stored source surface. That surface record carries
the transmitted share, the relative index and the smooth flag; it stays
within its 128 bytes if spare lanes suffice, and any growth is reported.

**Temporal and SVGF inputs.** The albedo that demodulates and remodulates
radiance becomes `lerp(base_color, transmission_color, (1 − m) · t)`, so
tinted glass demodulates by its tint instead of its unused base colour.
Depth, normals, motion and the history identity still describe the
primary transmissive surface. What is seen through it reprojects with that
surface's motion, which ghosts refracted content under motion; this is an
accepted limitation, as it already is for mirrors.

The light table, light crossing, accumulation restarts and the AOV set are
otherwise unchanged; a transmission edit is a material edit.

## 9. MaterialX Standard Surface

The translator consumes MaterialX's `standard_surface` definition, whose
transmission is the tinted, transmitting `dielectric_bsdf` of its
[nodegraph](https://github.com/AcademySoftwareFoundation/MaterialX/blob/main/libraries/bxdf/standard_surface.mtlx)
layered under the specular reflection. It maps:

| IR field | Input | Rule |
| --- | --- | --- |
| `transmission` | `transmission` | a constant, clamped to [0, 1] |
| `transmission_color` | `transmission_color` | a constant, each component clamped to [0, 1] |
| `thin_walled` | `thin_walled` | as authored |

These remain unrepresented, warn as other inputs do, and translate as
listed:

- a graph-driven `transmission` or `transmission_color`: the default (0, or
  white);
- `transmission_depth` above 0, under which the colour describes absorption
  at that depth rather than a tint: white, keeping refraction;
- non-black `transmission_scatter`, and non-zero
  `transmission_scatter_anisotropy`, `transmission_dispersion` or
  `transmission_extra_roughness`: ignored;
- `specular` 0 with positive transmission: `specular_IOR` is kept for
  refraction, so the reflection the IR cannot remove separately remains.

`transmission` leaves the unrepresented-input list when it is a constant.
`UsdPreviewSurface` translation is unchanged; its `opacity` stays coverage
and `opacityMode` is not read.

## 10. Evidence

The task's exit evidence, made specific:

- **Analytic checks**, each against an independent double-precision oracle
  by the [reference-image metric](../reference/SCENE.md#reference-images),
  through both estimators and both integrators. A smooth solid slab and a
  smooth thin wall, seen at several angles in front of a uniform emitter
  under a black sky, transmit `(1 − R) / (1 + R)`, the slab summing its
  internal reflections, times the tint for the wall and its square for the
  slab, which is crossed twice. Untinted, with the emitter dark and the sky
  white, they reflect `2R / (1 + R)`. A rough thin wall transmits its
  directional transmittance, by half-vector quadrature independent of the
  shader's sampling.
- **White furnace.** Under a constant white sky and no other light,
  untinted smooth solids (a sphere, and a cube that exercises total
  internal reflection) and a smooth thin-walled sphere render 1 within the
  metric. Rough ones render at most 1, with the loss recorded.
- **Determinism.** Every committed reference image and material fixture
  renders unchanged, and both integrators render a transmissive scene's
  64 spp identically.
- **Fixture.** A committed Standard Surface fixture with a C++ mirror,
  reference images and Hydra/headless agreement: solid and thin-walled,
  smooth and rough, tinted, `ior = 1`, and a warned `transmission_depth`.
- **Acceptance.** OpenChessSet's pawn tops and StandardShaderBall's glass
  render as tinted glass, compared with Storm, in a dated report.
