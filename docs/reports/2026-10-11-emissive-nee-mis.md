# Emissive next event estimation and MIS

Measured on 2026-10-11 for the first LOTUS-LIGHT-01 slice, in the working
tree based on `41c6b43` (v0.2.0), on the machine and toolchain of the
[integrator selection report](2026-10-11-hydra-integrator-selection.md):
Windows 11 x86_64, MSVC 14.51, OpenStrata 0.23.14, Release builds, an
NVIDIA RTX A5000 (driver 597.16, Vulkan 1.4.329) with Vulkan validation
and synchronization validation enabled, and OpenUSD 26.08 lookdev with
CPython 3.13 for Hydra. All runs were local; hosted CI was not measured.

## Scope

Renderer Phase 3 starts here. Until now a path found emission only where
a camera ray or a sampled BSDF direction happened to hit it. This slice
adds next event estimation (NEE) towards emissive triangles, shadow rays,
and multiple importance sampling (MIS) with the power heuristic between
light sampling and BSDF sampling, in the transport both integrators
share. Brute-force BSDF sampling remains as `LightSampling::Bsdf`, the
estimator of the committed reference. Analytic lights, environment maps
and environment importance sampling are later slices
([current.md](../roadmap/current.md)).

## Design

| Decision | Choice |
| --- | --- |
| Setting | `PathTracingSettings::light_sampling`: `Mis` (the default) or `Bsdf`; a change restarts the accumulation. Hydra exposes it as the flag `lotus:lightSampling`, true by default, so usdview lists it as a checked menu item |
| Light representation | a backend-private light table derived from the GPU scene: one 16-byte entry per triangle of each instance whose material emits, after a uint32 per instance record (its first entry or none). No core light type yet: emission is still the material IR's |
| Selection | Vose's alias method over area (double precision, world space) times the mean of the material's RGB emission; a lookup is estimated by `max(scale, 0) + max(bias, 0)`, its bound for texels in [0, 1], and a missing texture by its fallback. Each entry stores its own probability, which the MIS weight reads back for BSDF-found emission |
| Point | uniform by area on the triangle; its radiance is the emission after texture lookup times its coverage, the probability that a ray reaching it stops there |
| Shadow rays | from the offset hit position to the point offset off the light, towards the receiver; the first opaque surface blocks, a partial one multiplies by `1 - coverage` without drawing random numbers |
| MIS | power heuristic, in ratio form so that neither large densities overflow nor two zeros divide; a light sample against `BsdfPdf`, BSDF-found emission against the table's density for the hit point; the camera's view of an emitter is unweighted |
| Placement | after Russian roulette and before the BSDF sample, at every scattering event that may continue; four random numbers per event when the table has entries, none otherwise |
| Constant environment | left to BSDF sampling, unweighted ([measurement](#the-constant-environment)) |
| Wavefront | the shade kernel traces the shadow ray inline; it now reads the ray-origin section at bounces after the first, which the emission weight needs. A separate shadow-ray queue is left for later measurement |
| Rebuilds | the table is rebuilt with every instance or material table rewrite (`GpuSceneStats::light_writes`), never for an environment-only change; `emissive_triangles` and `light_bytes` report its size |

No pass was added, so no timestamp or debug AOV comes with this change
([quality bar](../roadmap/README.md#quality-bar-applies-to-every-phase)):
the shadow rays are timed with the scene pass, or with the wavefront shade
kernel.

### Keeping the reference bit for bit

The first build reproduced the committed reference with 70 of its 49,152
values different under `Bsdf`, although no BSDF-mode step drew a number or
added a term. The emission sum had been split into two branches, one
multiplying by the MIS weight, and the driver's shader compiler fused the
multiply-add differently. Weighting the emission in a separate variable
and keeping a single `radiance + throughput * emission` statement
restored the reference bit for bit; every image below reproduces it.

## Checks

`renderer.path.light_sampling` is a new headless check, registered in
`openstrata.renderer.yaml`. The existing transport scenarios, whose exact
expectations are brute-force samples, keep BSDF sampling, and so do
`--write-reference` and the reference check's bitwise reproduction.

| Part | Result |
| --- | --- |
| Cornell box under light sampling, 1–1024 spp from sample index 2²⁰, against the committed reference (DES-Q5) | image z ≤ 0.48, max tile \|z\| 1.22 / 2.30 / 1.92 / 2.13 / 2.96 at 1 / 16 / 64 / 256 / 1024 spp; a 10% brighter red wall rejected at 5.88 |
| The same through the wavefront integrator | identical z values; its BSDF samples reproduce the reference bit for bit |
| The same 64 spp through both integrators | bit for bit |
| Light-table size | the Cornell box's emitter: 2 triangles; open scenes: 0 (sky alone) and 4 (two emitters) |
| Sky alone, 64×64, 256 spp under light sampling against BSDF sampling's 4096 spp (64 batches) | image z −1.08 / −0.90 / −0.74, max tile \|z\| 2.36 |
| Sky and two emitters, the same comparison | image z 0.01 / 0.07 / 0.17, max tile \|z\| 0.69 |
| Switching the estimator | restarts the accumulation |
| A sky edit | leaves `light_writes` unchanged |

The open scenes have a coated diffuse floor, a rough metal panel (GGX
roughness 0.25) and a 25% coverage layer above part of the floor, under a
constant sky, in an orthographic view every camera ray hits. The emitters
lie outside the view: a half-covered panel with textured, two-sided
emission whose triangles differ in area, and a small panel of constant
emission. Its shadow rays cross the coverage layer.

### Variance and time to equal quality

Per-sample variance is the mean over pixels and channels of each value's
variance across 16 accumulations of 16 samples; GPU time is the median of
16 single-sample frames of the 128×128 scene pass. Both estimators are
unbiased, so equal variance per unit time is equal quality.

| Scene | BSDF sampling | Light sampling | Variance | GPU ms per sample | Time to equal quality |
| --- | --- | --- | --- | --- | --- |
| Cornell box | 1.431 | 0.3785 | 3.78× lower | 0.058 → 0.085 | **2.61× shorter** |
| Sky alone | | | 0.99× (unchanged) | | |
| Sky and two emitters | | | **72.4× lower** | | |

The Cornell box gains less than the open scene: much of its variance is
indirect light between its walls, which NEE shortens by one bounce at
most, and the camera sees the emitter directly, which neither estimator
samples. The check requires at least half the Cornell box's variance and
no increase for the sky alone.

### The constant environment

The plan first sampled a constant environment as a light: uniform
directions over the sphere, chosen half the time beside emitters and
always without them, weighed by MIS like the triangles. On the open
scenes, before the second emitter was added:

| Scene | Variance with the sky sampled | With the sky left to BSDF sampling |
| --- | --- | --- |
| Sky alone | 0.51× lower (twice the variance) | 1.00× |
| Sky and one emitter | 16.6× lower | 27.7× lower |

Uniform directions lose to BSDF sampling's cosine-weighted ones on a
uniform sky, waste half their shadow rays below the surface, and take
samples from the emitters. The sky is therefore left to BSDF sampling and
its environment path was removed; environment light sampling returns with
importance sampling of environment textures, where the environment has
structure to sample.

### Fault injections

Each fault was built into the core configuration and the headless report
inspected; the source was restored and touched after each build. Every
fault failed `renderer.path.light_sampling` and no other check.

| Fault | First failure |
| --- | --- |
| F1: emission found by BSDF sampling unweighted | Cornell box 1 spp, image mean 19.3 SE high |
| F2: light samples unweighted | Cornell box 16 spp, 22.6 SE high |
| F3: shadow rays ignore partial coverage | sky and emitters, 5.8 SE |
| F4: the alias table never aliases (thresholds 1) | sky and emitters, 5.3 SE |
| F5: the wavefront shade kernel without the path origin | wavefront Cornell box 1 spp, a tile at −5.3 SE |
| F6: shadow rays reach the light's own surface | Cornell box 1 spp, 19.4 SE low |

F3 and F4 bias only the open scene, by little more than the 5-SE
threshold: they are caught on this device and sample sequence, with a
small margin.

## Fixed benchmarks

`--benchmark-light-sampling` (default `bsdf`, so that earlier measurements
stay comparable) selects the estimator; CTest `lotus-renderer-benchmark-mis`
covers `mis`. Commands, from the repository root:

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/nee-bench-reference-bsdf.json --benchmark-label "41c6b43 + LOTUS-LIGHT-01; ost core Release" --benchmark-light-sampling bsdf
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/nee-bench-reference-mis.json --benchmark-label "41c6b43 + LOTUS-LIGHT-01; ost core Release" --benchmark-light-sampling mis
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/nee-bench-wavefront-bsdf.json --benchmark-label "41c6b43 + LOTUS-LIGHT-01; ost core Release" --benchmark-integrator wavefront
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/nee-bench-wavefront-mis.json --benchmark-label "41c6b43 + LOTUS-LIGHT-01; ost core Release" --benchmark-integrator wavefront --benchmark-light-sampling mis
```

Median GPU ms per 128×128 sample:

| Workload | Reference, BSDF | Reference, MIS | Wavefront, BSDF | Wavefront, MIS |
| --- | --- | --- | --- | --- |
| `cornell-v1` | 0.067 | 0.095 | 0.172 | 0.217 |
| `textured-v1` | 0.045 | 0.046 | 0.120 | 0.120 |
| `instanced-v1` | 0.035 | 0.035 | 0.100 | 0.100 |
| `geometry-v1` | 0.094 | 0.094 | 0.226 | 0.228 |

Only `cornell-v1` has an emitter; the others, lit by the white
environment, have an empty light table and cost the same under either
estimator, with byte-identical images. Every BSDF image is byte-identical
to the reference-integrator images of the
[equivalence report](2026-10-10-wavefront-equivalence.md)'s third run,
and every MIS image is the same under both integrators.
`cornell-v1` passes the reference comparison under both estimators.

## Hydra

The new GPU-gated `lotus-renderer-hydra-light-sampling` test syncs a stage
with a floor, a back wall and a 0.6×0.6 `UsdPreviewSurface` panel of
`emissiveColor` (8, 6, 4) under the adapter's white environment through
UsdImaging, into a 32×32 colour AOV at 16 spp from sample index 4096.

| Step | Required | Measured |
| --- | --- | --- |
| no `lotus:lightSampling` | a `bool` descriptor defaulting to true; the panel's 2 light-table triangles; the backend's light-sampled image bit for bit | pass |
| set to false | restart; the backend's BSDF image bit for bit; channel means within 5 SE of the pixel differences | mis 0.1258 / 0.1181 / 0.1105, bsdf 0.1248 / 0.1171 / 0.1094, differences ≈0.001 against ±0.004–0.006 |
| true, then false again | restart each time; each first image again | pass |
| a new delegate created with false | the BSDF image | pass |

`lotus-renderer-hydra-texture-gpu` compares exact mirror radiance from an
emissive texture; under the new default its first run measured 2.000726
instead of 2.0, because light sampling has the same mean but not the same
samples. The test now sets `lotus:lightSampling` to false. The usdview
smoke test checks the installed plugin's *Light sampling (NEE + MIS)*
menu item is checked by default, unchecks it, checks that the
accumulation restarted with `light_sampling=bsdf` in the host evidence
log, and that checking it again reproduces the earlier screenshot.

## Finding: stale objects again

`lotus-renderer-acceleration-compaction` segfaulted after `gpu_scene.hpp`
gained members. Its object file, like several others, had `#deps 0` in
`ninja -t deps` on this Japanese-locale MSVC host, so ninja relinked it
without recompiling against the new `GpuScene` layout. Deleting every
`#deps 0` object and rebuilding fixed it
([building guide](../guides/BUILDING.md)).

## Remaining Renderer Phase 3 work

Analytic UsdLux lights in a light representation of the core, environment
textures with importance sampling, and the exit review of time to equal
quality are listed in [current.md](../roadmap/current.md).
