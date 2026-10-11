# Analytic lights

Measured on 2026-10-11 for LOTUS-LIGHT-02, in the working tree based on
`ceec2ee` (emissive next event estimation merged), on the machine and
toolchain of the [emissive NEE report](2026-10-11-emissive-nee-mis.md):
Windows 11 x86_64, MSVC 14.51, OpenStrata 0.23.14, Release builds, an
NVIDIA RTX A5000 (driver 597.16, Vulkan 1.4.329) with Vulkan validation
and synchronization validation enabled, and OpenUSD 26.08 lookdev with
CPython 3.13 for Hydra. All runs were local; hosted CI was not measured.

## Scope

The [first Renderer Phase 3 slice](2026-10-11-emissive-nee-mis.md) sampled
emissive triangles only, and the Hydra adapter translated no UsdLux light
but the dome. This slice adds a light representation to the core scene
for UsdLux sphere, rect, disk and distant lights, translates them in the
Hydra adapter, and samples them by next event estimation in the same
light table as the emissive triangles, with the radiance that BSDF-sampled
directions find weighed by MIS. Environment importance sampling and the
phase's exit review remain ([current.md](../roadmap/current.md)).

## Design

| Decision | Choice |
| --- | --- |
| Core representation | `Lotus::Light` in `LotusScene::lights`, keyed by host identifiers: a `LightShape` (sphere, rect, disk, distant), `world_from_light`, RGB `radiance` (UsdLux's `color * intensity * 2^exposure`), `normalize`, `radius`, `width`, `height` and `angle`. `RenderWorld::SetLight` and `RemoveLight`; `SceneUpdate::lights` replaces every light when they change. Derived world-space quantities stay in the backend |
| Transforms | rect and disk take the full transform (a parallelogram, an ellipse), emitting from the side to which the inverse transpose takes -Z; a sphere's radius scales by the cube root of the absolute determinant, so it stays a sphere; a distant light shines along -Z |
| `normalize` | UsdLux's size factors: the world-space area, and for a distant light of positive angle π sin²θ up to θ = 90°, (2 − sin²θ) π beyond. A distant light of angle 0 is a parallel light whose value is its irradiance at normal incidence |
| What a light is | an emitter only: it blocks neither rays, shadow rays nor other lights, and camera rays pass it, as they pass the environment. A ray adds the radiance of every light it crosses before its hit, or before escaping: a sphere's near side from outside, a rect's or disk's emitting side, and, on escaping, each distant light whose cone holds its direction |
| Light table | one entry per analytic light beside the triangles, flagged by `kAnalyticLight` in the entry's instance word, then a 96-byte world-space record per light. The weight is estimated power over 2π, the scale of a two-sided triangle's area-times-emission: half the mean normalized radiance times the area, or for a distant light half its irradiance at normal incidence times the squared radius of the sphere about the instances' bounding box. Black, zero-size and float-overflowing lights are left out |
| Sampling | a sphere uniformly over the cone it subtends from outside (Pharr et al., section 6.2.4, with its small-angle form); a rect uniformly by area; a disk by the concentric mapping; a distant light uniformly over its cone; a parallel light in its one direction, with its selection probability as its density and no MIS weight. The four random numbers per event of the first slice cover every kind |
| MIS | a light sample by the power heuristic against `BsdfPdf`, as for triangles; a crossed light against its selection probability times its density for the direction, computed from the same ray origin as next event estimation's |
| Parallel light under BSDF sampling | no sampled direction can find it, so each scattering event adds every parallel light through a shadow ray of its own, drawing no random numbers: both estimators converge to the same image, and BSDF sampling still reproduces the committed reference bit for bit |
| Wavefront | unchanged kernels; the shade kernel now also reads the ray-origin section under BSDF sampling when the scene has analytic lights, to find the lights a continuation ray crossed. The hit distance is the projection of the reconstructed hit onto the ray in both integrators |
| Cost | every ray tests every analytic light, a loop over the records; there is no acceleration structure for lights |
| Hydra | `sphereLight`, `rectLight`, `diskLight` and `distantLight` sprims; any synced analytic light, like any dome, replaces the white fallback environment |

Textures, shaping (cone and IES), shadow controls, colour temperature,
diffuse and specular multipliers, `treatAsPoint` and linking are not read;
cylinder, portal, geometry and mesh lights are not supported
([light reference](../reference/SCENE.md#lights)).

## Checks

`renderer.path.analytic_lights` is a new headless check. An uncoated
Lambert floor (albedo 0.8, 0.7, 0.6) under a black sky fills an
orthographic 64×64 view. Each image is a 1024-spp mean of 16 batches of
64 samples, compared by the DES-Q5 metric with an independent
double-precision oracle using the batches' per-sample variance: the
floor's albedo over π times the lights' irradiance, by Lambert's formula
for polygons (a 1024-gon for the disk, two triangles for an emissive
square), the projected solid angle of a sphere's or cone's cap, and a
parallel light's cosine, averaged over 4×4 points of each pixel.

| Light | Placement | BSDF sampling: image z, max tile \|z\| | Light sampling: image z, max tile \|z\| | Variance |
| --- | --- | --- | --- | --- |
| sphere | radius 0.6 under a 0.5 scale, 0.8 above the floor, in the camera's view | 0.64 / 0.64 / 0.64, 2.88 | −1.11 / −1.08 / −0.95, 2.59 | 1135× lower |
| ball | normalized sphere of radius 0.15, in view | −0.39 (all channels), 2.49 | 0.51 / 0.52 / 0.53, 2.40 | 2052× lower |
| rect | 0.5 × 0.4 under a (1.2, 0.8) scale, tilted 35°: part of the floor lies behind it | −0.24, 2.82 | −0.49, 2.61 | 112× lower |
| disk | normalized, radius 0.2 under a (1.6, 0.7) scale, tilted 25° | 1.25, 2.32 | −0.56, 2.34 | 85× lower |
| cone | normalized distant light of angle 20°, 30° from the zenith | 0.16, 1.82 | 0.93 / 0.91 / 0.88, 2.51 | 14463× lower |
| parallel | angle 0, 40° from the zenith | 0.02 / 0.00 / 0.03, 0.03 | the same | exact under both |

A normalized sphere's irradiance does not depend on its radius, so the
scaled sphere is not normalized; the ball covers normalization. Under
BSDF sampling the parallel light is added deterministically, as under
light sampling with it alone in the table, so both images are exact.

| Scenario | Result |
| --- | --- |
| every light at once and a two-sided emissive square outside the view | reference integrator: bsdf 0.56 / 0.56 / 0.79, max tile 2.97; mis 0.13 / 0.20 / 0.68, max tile 2.13; the wavefront integrator the same values |
| the same 64 spp through both integrators | bit for bit |
| the light table | 6 analytic lights and the square's 2 triangles |
| the lights above an opaque black roof beyond the near plane | black, exactly, under both estimators and integrators |
| above a roof of 50% coverage | half the oracle: bsdf 0.51 / 0.67 / 0.51, max tile 2.43; mis −0.30 / −0.08 / −0.06, 2.63 |
| below that opaque roof as a ceiling, which every sampled direction hits | the oracle without the distant lights: bsdf 1.23 / 1.01 / 0.98, max tile 2.70; mis 1.23 / 1.11 / 1.20, 2.60; the wavefront integrator the same values |
| a light edit, with a black light and a zero-width rect added | restarts the accumulation; one light-table rewrite and no geometry, instance, material, BLAS or TLAS work; neither added light reaches the GPU scene |

Every sphere is in the camera's view, so a camera ray that saw a light
would fail the comparison by hundreds of standard errors (F3 below).
`renderer.path.light_sampling`, `renderer.path.reference` and
`renderer.path.wavefront` report the same z values as before this change:
the committed reference is reproduced bit for bit, and the Cornell box's
light-sampled images are unchanged.

### Variance and time

With every light at once, light sampling lowers the per-sample variance
3.73× (bsdf 4.923, mis 1.321); BSDF sampling's parallel light is already
exact. The median GPU time of 16 single-sample 64×64 frames was not
stable between runs at this size: bsdf 0.105 to 0.189 ms and mis 0.086 to
0.203 ms across three runs of the check (the first before the ball was
added), giving times to equal quality from 2.15× to 4.78× shorter. The
exit review (LOTUS-LIGHT-04) measures time to equal quality on larger
workloads.

### Fault injections

Each fault was built into the core configuration and the headless report
inspected; the source was restored with a fresh modification time after
each build. Every fault failed `renderer.path.analytic_lights` and no
other check.

| Fault | First failure |
| --- | --- |
| F1: crossed lights ignore the hit distance | under the opaque roof, 0.3 instead of 0 |
| F2: lights crossed before a hit unweighted under light sampling | every light, reference, mis: image 9.6 SE |
| F3: camera rays see analytic lights | sphere under bsdf: image 772 SE |
| F4: rects and disks found from their back | rect under bsdf: image −2920 SE |
| F5: light sampling ignores the rect's cosine | rect under mis: image 2056 SE |
| F6: the disk's area ignores the transform | disk under bsdf: image 34.5 SE |
| F7: no parallel light under BSDF sampling | parallel under bsdf: image −53926 SE |
| F8: the wavefront shade kernel without the ray origin under BSDF sampling | every light, wavefront, bsdf: a tile at 7.4 SE |
| F9: the sphere's radius ignores the transform | sphere under bsdf: image 865 SE |
| F10: analytic records without their selection probability | sphere under mis: image 500 SE |
| F11: the sphere's cone without its small-angle form | sphere under mis: image 9361 SE |
| F12: lights crossed by escaping rays unweighted under light sampling | sphere under mis: image 500 SE |

The first run of F9 passed: the sphere was normalized, and its
irradiance did not depend on its radius. The scaled sphere lost
`normalize` and the normalized ball was added. F2 and F8 were first
caught only at 6.5 and 5.6 SE, through the emissive square, the only
surface behind a light, so the ceiling scenario was added for them. In the
final check they still first fail in the combined scene, before the
ceiling scenario runs, at the 9.6 and 7.4 SE in the table.

## Fixed benchmarks

The fixed workloads have no analytic lights. Commands, from the
repository root:

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/light02-bench-reference-bsdf.json --benchmark-label "ceec2ee + LOTUS-LIGHT-02; ost core Release" --benchmark-light-sampling bsdf
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/light02-bench-reference-mis.json --benchmark-label "ceec2ee + LOTUS-LIGHT-02; ost core Release" --benchmark-light-sampling mis
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/light02-bench-wavefront-bsdf.json --benchmark-label "ceec2ee + LOTUS-LIGHT-02; ost core Release" --benchmark-integrator wavefront
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/light02-bench-wavefront-mis.json --benchmark-label "ceec2ee + LOTUS-LIGHT-02; ost core Release" --benchmark-integrator wavefront --benchmark-light-sampling mis
```

Median GPU ms per 128×128 sample, against the
[emissive NEE report](2026-10-11-emissive-nee-mis.md#fixed-benchmarks)'s:

| Workload | Reference, BSDF | Reference, MIS | Wavefront, BSDF | Wavefront, MIS |
| --- | --- | --- | --- | --- |
| `cornell-v1` | 0.069 (0.067) | 0.099 (0.095) | 0.176 (0.172) | 0.221 (0.217) |
| `textured-v1` | 0.047 (0.045) | 0.047 (0.046) | 0.121 (0.120) | 0.122 (0.120) |
| `instanced-v1` | 0.036 (0.035) | 0.035 (0.035) | 0.102 (0.100) | 0.101 (0.100) |
| `geometry-v1` | 0.096 (0.094) | 0.095 (0.094) | 0.228 (0.226) | 0.229 (0.228) |

All sixteen images are byte-identical to that report's. The first
reference/MIS run measured `geometry-v1` at 0.160 ms; two repeats measured
0.095 ms with the same images, and the table shows them. The other costs
are 1–4% higher, within this device's run-to-run spread.

## Hydra

The adapter creates an `HdLotusLight` for each `sphereLight`, `rectLight`,
`diskLight` and `distantLight` sprim. It reads the world transform and
`color`, `intensity` (50,000 by default for a DistantLight), `exposure`,
`normalize`, `radius`, `width`, `height` and `angle` through
`GetLightParamValue`, with UsdLux's defaults for values it does not
receive, and keys the light by its path.

CTest `lotus-renderer-hydra-analytic-lights` syncs a floor, a back wall
and one light of each kind through UsdImaging:

| Step | Required | Measured |
| --- | --- | --- |
| no lights | none; the white fallback environment | pass |
| four lights added | each sprim populated; each light's shape, composed transform (as `UsdGeomXformable` computes it), radiance and sizes; a black environment | pass |
| a fallback sprim | no scene change | pass |
| intensity and transform edits, the sun's intensity cleared | the new values (the sun's 50,000); the earlier snapshot unchanged | pass |
| width −1, angle 400 | 0 and 360 | pass |
| NaN intensity, exposure 200 | the disk left out, with a warning; back once valid | pass |
| a light hidden, then its parent | left out; the environment stays black | pass |
| a dome beside the lights, then the lights removed, then the dome | the dome's radiance, then the fallback | pass |

Its GPU-gated `-gpu` variant converges the stage at 32×32 and 16 spp from
sample index 4096. Both estimators' images are the backend's images bit
for bit, the GPU scene holds 4 analytic lights and no emissive triangles,
and the channel means agree: mis 0.01661 / 0.01367 / 0.01065, bsdf
0.01687 / 0.01372 / 0.01058, differences within ±0.0037–0.0045. An
intensity edit restarts the accumulation and changes the image with one
light-table rewrite and no geometry, instance, BLAS or TLAS work.

## Runs

| Configuration | Result |
| --- | --- |
| core | `ost build`, `ost test` 19/19, `ost validate --strict-renderer-evidence` passed |
| Hydra | `ost build`, `ost test` 40/40 (the two new tests included), strict validate passed |
| ci-core, without a runtime | `ost build`, `ost test` 11/11 |
| documentation | `scripts/check_docs.py` passed |

Before the builds, every object that `ninja -t deps` listed with `#deps 0`
was deleted ([building guide](../guides/BUILDING.md)).
