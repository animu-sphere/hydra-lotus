# Dielectric transmission

## Evaluation brief

LOTUS-TRANSMISSION-01, the second task of the
[Materials & Lighting milestone](../roadmap/README.md#v080--materials--lighting).
After [LOTUS-MTLX-01](2026-10-29-materialx-standard-surface.md#decision),
OpenChessSet's pawn tops and StandardShaderBall's glass translated with a
transmission warning and rendered as opaque base colour. The task required
a transport contract before any code. [TRANSMISSION.md](../design/TRANSMISSION.md)
was merged without review changes and is now accepted. This change
implements it in the material IR, the BSDF, both integrators, the reservoir
and temporal inputs and the Standard Surface translation.

Adopt the lobe if four things hold. Its smooth reflectance and
transmittance, a rough thin wall's transmittance and white-furnace energy
must match independent oracles through both integrators and estimators.
Every committed reference image must render unchanged. A committed
Standard Surface fixture must match its reference headless and through
Hydra. And the two surveyed assets must render glass.

Conditions: Windows AMD64, MSVC 19.51 Release, NVIDIA RTX A5000, driver
597.16, Vulkan 1.4.329, validation and synchronization validation active,
clocks unlocked. Source `bd23086` plus this change. Hashes and every value
are in the [data](2026-10-29-dielectric-transmission.json). The surveyed
assets are local and not redistributable; their images are not committed.

## Implementation

The [scene reference](../reference/SCENE.md#path-tracing) now owns the
behaviour. In outline:

- **IR.** `Material` gains `transmission`, `transmission_color` and
  `thin_walled`, validated as the contract states. The GPU material record
  grows from 544 to 560 bytes: one more `float4` for the colour and weight,
  and the thin-wall flag in an unused opacity lane.
- **BSDF.** `bsdf.slang` keeps the coat over Lambert and the metal as the
  *layered* lobes and adds the interface, chosen with probability
  `(1 − m)t`. The interface uses exact Fresnel, Dirac deltas below the
  alpha floor or at `ior = 1`, and the Walter et al. GGX dielectric
  otherwise. The metal's share of the layered lobes is renormalized to
  `m / (1 − (1 − m)t)`. When `t = 0` every function returns the layered
  result directly.
- **Sides.** `ReconstructHit` takes the world winding normal times the sign
  of the instance transform's determinant as the outward normal, but only
  for materials with transmission.
- **Transport.** A transmitted continuation must lie below both normals,
  and its origin is offset to that side. After a delta or transmission
  event, `kUnweightedLight` makes the next hit's emission, crossed analytic
  lights and the escaping environment image unweighted. The wavefront
  integrator has no flags section, so it stores the flag as a last density
  of −1, which is never otherwise used.
- **Reservoirs and temporal inputs.** The 128-byte reservoir surface
  record stores the transmitted share and relative index in two unused
  lanes, using their signs for the thin-wall and smooth flags. The SVGF
  albedo is blended towards the transmission colour.
- **Translation.** Standard Surface's `transmission`, `transmission_color`
  and `thin_walled` are now translated. The other transmission inputs warn,
  and `specular` 0 keeps `specular_IOR`. The adapter's warning list no
  longer names `transmission` when it is a constant.

One clause changed in implementation. `BsdfEvalCos` and `BsdfPdf` cover
reflection only, as next event estimation and MIS need. The contract's §5
also gave them the refraction density, which nothing reads: every
transmission event's light is unweighted. §5 is amended to match.

## Analytic checks

The new headless check `renderer.material.transmission` renders 64×64
orthographic views at 64 spp. It uses five modes: the reference and
wavefront integrators, each with BSDF sampling and with NEE/MIS, and the
reference integrator with initial reservoirs. Each mean must be within five
standard errors of a double-precision oracle. The glass has index 1.5 and
the tint is (0.9, 0.6, 0.3).

Slabs and walls are tilted about X and lowered so that they stay beyond
the near plane at 70 degrees. A huge emitter sits parallel to them and
behind them, so only transmitted light reaches it, and reflected light
escapes to a black sky.

| Check | Oracle (R G B) | Measured R, all five modes | 5 SE (R) |
| --- | --- | --- | --- |
| Solid slab, 0° | `(1 − R)/(1 + R)` × tint²: 0.7477 0.3323 0.0831 | 0.7475–0.7476 | 0.0021 |
| Solid slab, 45° | 0.7325 0.3256 0.0814 | 0.7322–0.7325 | 0.0024 |
| Solid slab, 70° | 0.5734 0.2548 0.0637 | 0.5734–0.5740 | 0.0039 |
| Thin wall, 0° | `(1 − R)/(1 + R)` × tint: 0.8308 0.5538 0.2769 | 0.8304–0.8306 | 0.0024 |
| Thin wall, 45° | 0.8139 0.5426 0.2713 | 0.8138–0.8140 | 0.0026 |
| Thin wall, 70° | 0.6371 0.4247 0.2124 | 0.6369–0.6374 | 0.0040 |
| Mirrored slab (x scale −1), 60° | 0.6773 0.3010 0.0753 | 0.6773–0.6776 | 0.0030 |
| Rough thin wall (0.5), 0° | half-vector quadrature × tint: 0.7601 0.5068 0.2534 | 0.7591–0.7598 | 0.0030 |
| Rough thin wall (0.5), 60° | 0.6613 0.4409 0.2204 | 0.6611–0.6613 | 0.0035 |
| Index-matched rough wall | the tint exactly | 0.9000 | 10⁻⁶ |
| Slab reflection, 60°, white sky | `2R/(1 + R)` = 0.1638 | 0.1635 | 0.0036 |
| Wall reflection, 60°, white sky | 0.1638 | 0.1637 | 0.0036 |
| Furnace: smooth sphere | 1 | 0.9999 | 0.0010 |
| Furnace: smooth cube (total internal reflection) | 1 | 1.0000 | 0.0042 |
| Furnace: smooth thin-walled sphere | 1 | 1.0000 | 0.0003 |
| Furnace: rough sphere (0.5) | at most 1 | 0.7876 (loss 0.2124) | — |

The largest deviation is 1.6 standard errors, on the rough wall at 0°
under NEE/MIS. The mirrored slab matters for the sides: if the inside were
taken from the world winding alone, every ray would meet total internal
reflection on entry and the slab would transmit nothing.

The check then renders a glass scene: rough and smooth, solid and thin
spheres before an emitter, with a second emitter in front whose light the
rough glass reflects. Against BSDF sampling's 4096-spp estimate, NEE/MIS
matches at 256 spp with image z −0.64, −1.16, −1.36 (max tile |z| 2.58).
Initial reservoirs match with −0.26, −0.18, 0.01 (2.19). Both integrators
render its 64 spp bit for bit.

### The furnace and the bounce limit

The first furnace run used a 48×24 sphere with the default 64 bounces and
measured 0.9951. At 4096 spp that was 0.99527, 12.8 standard errors below
1.

| Geometry | Bounces | Mean at 4096 spp | 5 SE |
| --- | --- | --- | --- |
| Sphere, 48×24 facets | 64 | 0.99527 | 0.0018 |
| Sphere, 48×24 facets | 1024 | 1.0015 | 0.0124 |
| Sphere, 256×128 facets | 64 | 0.99971 | 0.0003 |
| Sphere, 256×128 facets | 1024 | 0.9998 | 0.0003 |
| Cube | 64 | 1.0001 | 0.0009 |
| Thin-walled sphere, 48×24 | 64 | 1.0000 | 0.0001 |

The loss comes from the bounce limit, not the lobe. Light that enters near
a faceted sphere's rim meets the next facet beyond the critical angle and
circles inside by total internal reflection. The default limit ends some
of those paths. With 1024 bounces the loss disappears, but the circling
paths raise the standard error sevenfold at 48×24 facets. The check therefore
uses 256×128 facets, 1024 bounces and 256 spp. A thin wall has no inside,
and a cube's sides reflect totally but its ends let the light out, so
neither loses energy at 64 bounces. Whether 64 is the right default for
scenes with glass is left open.

## Fixture

`material-transmission-v1` is a 160×160 Standard Surface stage on the
studio's camera, lights and backdrop
([layout](../reference/SCENE.md#material-fixtures)). It has three rows:

- solid spheres: smooth, rough, tinted, index-matched, and partly
  transmitting over teal metal;
- thin-walled cards: smooth, rough, tinted, index-matched, and index 2.42;
- spheres: a mirrored solid sphere, a `transmission_depth` that warns and
  leaves the light untinted, `specular` 0, ignored scattering and
  dispersion, and a graph-driven transmission that keeps 0.

Hydra extracts the stage to the C++ mirror exactly. It warns for Depth,
NoSpecular, Ignored and Graph, and for nothing else.

| Mode | spp | Image z (R G B) | Max tile \|z\| | Mean tile z² |
| --- | --- | --- | --- | --- |
| Reference, BSDF | 16 | 0.10 0.39 0.62 | 2.92 | 0.94 |
| Reference, BSDF | 256 | 0.65 0.72 0.84 | 3.65 | 1.10 |
| Reference, NEE/MIS | 16 | −0.31 −0.25 −0.23 | 3.30 | 0.45 |
| Reference, NEE/MIS | 256 | −0.47 −0.77 −0.81 | 2.68 | 0.49 |
| Wavefront, NEE/MIS | 16 and 256 | same as the reference integrator | | |

The Hydra GPU test renders the stage at 256 spp. Each image is the
backend's image of the mirror bit for bit and matches the reference. The
negative control ignores Tinted's transmission colour, and the metric
rejects it at max tile |z| 534.

`material-mtlx-v1`'s Lobes card authored `transmission` 1 to show an
ignored lobe. It would now refract, so `subsurface` 1 replaced it: still
ignored, with the same image.

## Determinism

`--write-material-reference` rewrote every fixture's reference. Only the
new fixture's two files differed. The Cornell box and all six existing
fixtures reproduce their references bit for bit, and the wavefront and
light-sampling checks still render the same samples bit for bit under
both integrators. So `t = 0` takes the old code path exactly, as §5
requires.

## Fault injection

Each fault was injected into the shaders, rebuilt, run, and then reverted.

| Fault | `renderer.material.transmission` | `renderer.material.fixtures` |
| --- | --- | --- |
| Light found after a delta or transmission event weighed by MIS | reference-mis slab at 0°: 0 instead of 0.7477 | NEE/MIS 16 spp: image −32.9 SE |
| Outward normal without the determinant's sign | reference-bsdf mirrored slab: 0 instead of 0.6773 | BSDF 256 spp: the Mirrored sphere's tile 11.9 SE |

## Survey assets

`usdrecord` rendered both assets at 640 pixels wide, with Storm as a visual
control as in the survey. Local override layers selected the shader ball's
`mtlx_glass` example under its authored camera, and added a dome light and
a camera near the white pawns to OpenChessSet.

- **OpenChessSet.** The pawn tops (`transmission` 1, colour (1, 1, 0.828))
  refract the board and the pieces behind them as faintly yellow glass.
  Storm draws them almost invisibly, as transparency without refraction.
  The eight transmission warnings are gone; the eight subsurface warnings
  for the kings and queens remain.
- **StandardShaderBall.** The glass shell (`specular_IOR` 1.54, roughness
  0.016, colour (0.942, 1, 0.988)) refracts the core, the bars and the
  numbered ground, with the noise of light found only by BSDF sampling.
  Storm shows a translucent shell. The material no longer warns.

## Cost

The headless report took 59 s to build and run, all checks included. The
Hydra material fixture CTests took 0.13 s on the CPU and 92.9 s on the GPU
for seven fixtures. The ReSTIR and temporal GPU CTests passed unchanged:
`temporal-gpu` in 24.8 s, `restir-gpu` in 235.7 s and the reference ReSTIR
benchmark in 57.4 s. A 640-pixel `usdrecord` of either asset took 18–19 s.
Scenes without transmission run the same code as before. Glass makes paths
longer, which was not measured.

## Decision

**Adopted.** Through both integrators and every estimator, the
transmission lobe matches its oracles and conserves energy. It changes no
committed reference image. Its fixture matches headless and through Hydra,
and OpenChessSet's pawn tops and the shader ball's glass render as tinted
glass. LOTUS-TRANSMISSION-01 leaves the active queue, and the contract is
accepted with §5 amended.

Not run: the full Hydra GPU suite; the viewport, research and ci-core
trees; usdview and the installed Hydra host; temporal and spatial
reservoir reuse or SVGF on a transmissive scene; and the other survey
stages.

## Deferred

As the [contract](../design/TRANSMISSION.md#1-scope) states:

- volume absorption and scattering (`transmission_depth`,
  `transmission_scatter`);
- dispersion;
- nested dielectrics and their priorities;
- caustics through light sampling, since shadow rays treat glass as
  opaque;
- multiple-scattering compensation for rough interfaces;
- texture-driven transmission;
- subsurface and coat.

Each needs an asset gap first. The bounce-limit finding above is recorded
for Renderer Phase 8's path classification.
