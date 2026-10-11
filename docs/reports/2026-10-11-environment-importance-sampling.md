# Environment importance sampling

Measured on 2026-10-11 for LOTUS-LIGHT-03, in the working tree based on
`9f937c5` (analytic lights merged), on the machine and toolchain of the
[analytic lights report](2026-10-11-analytic-lights.md): Windows 11
x86_64, MSVC 14.51, OpenStrata 0.23.14, Release builds, an NVIDIA RTX
A5000 (driver 597.16, Vulkan 1.4.329) with Vulkan validation and
synchronization validation enabled, and OpenUSD 26.08 lookdev with
CPython 3.13 for Hydra. All runs were local; hosted CI was not measured.

## Scope

Until this slice the environment was one constant radiance, which the
[first Renderer Phase 3 slice](2026-10-11-emissive-nee-mis.md#the-constant-environment)
left to BSDF sampling because uniform directions sampled it worse than
cosine-weighted ones. This slice adds dome-light textures as a
latitude-longitude environment image in the core scene and samples it by
next event estimation in proportion to its radiance, in the same light
table as the emissive triangles and analytic lights, with the radiance
escaping BSDF-sampled directions find weighed by MIS. The phase's exit
review remains ([current.md](../roadmap/current.md)).

## Design

| Decision | Choice |
| --- | --- |
| Core representation | `LotusScene::environment_map`, an optional `Lotus::EnvironmentMap`: a key into `LotusScene::textures`, an RGB `scale` (UsdLux's `color * intensity * 2^exposure`) and `world_from_environment`. `RenderWorld::SetEnvironmentMap`; `SceneUpdate::environment_map` travels with `environment_changed`, as the resident texture, scale and frame. A key with no texture, or a black scale, plans no image. The constant environment stays and adds to the image |
| Layout | OpenEXR's latitude-longitude map, which UsdLux's DomeLight specifies: column 0 at longitude +π, the top row at +Y, latitude 0 and longitude 0 at +Z, longitude π/2 at +X. Bilinear filtering, repeating across longitudes and clamped at the poles, with the existing sampler table |
| Orientation | only the frame's orientation counts: `Lotus::EnvironmentFrame` orthonormalizes the X, Y and Z columns in that order, so scale and shear do not distort the image, a mirroring transform mirrors it, and a frame without independent axes is rejected |
| Distribution | built on the host when the image, its scale or its frame changes: a grid of at most 2,048 × 1,024 cells, each the mean of the texels whose centres it holds, valued at its mean scaled RGB radiance times sin θ at its row's centre; per cell its density per unit image area, then the rows' cumulative distribution and each row's (Pharr et al., *Physically Based Rendering*, 3rd edition, sections 13.6.7 and 14.2.4), after an 80-byte record, in one device buffer |
| Light table | one more entry, last, flagged `kEnvironmentLight` in its instance word. Its weight is a distant light's: the flux through a disk of the scene's radius over 2π, π² R² times the mean cell value. Its probability travels in the path constants, beside the buffer's address |
| Sampling | of the four random numbers per event, the last two choose a row and then a cell of it, each by binary search and continuously within the cell; the density per solid angle is the selection probability times the cell's density over 2π² sin θ. Shadow rays are unbounded, as towards a distant light |
| MIS | a light sample by the power heuristic against `BsdfPdf`; an escaping BSDF-sampled direction's image radiance against the image's selection probability times the density for its direction. The constant environment is still added unweighted, and so is an image without a light-table entry (no instances to light) |
| Bilinear filtering and the piecewise-constant density | a direction whose filtered radiance is positive in a cell of density 0 is found by BSDF sampling alone at full weight, so the estimator stays unbiased |
| Determinism | without an image no step changes: BSDF sampling reproduces the committed reference bit for bit, and the light-sampled Cornell box's images are unchanged |
| Hydra | a dome with `texture:file` (format `automatic` or `latlong`) decoded with Hio is the image; one textured dome per scene |

Angular, mirrored-ball and vertical-cross maps, colour temperature,
portals and more than one textured dome are not supported
([dome reference](../reference/SCENE.md#lights)).

## The Stinson Beach image

The check lights its scenes with OpenUSD's Stinson Beach image, committed
as `validation/assets/StinsonBeach.hdr` beside OpenUSD's licence text
(`LICENSE.OpenUSD.txt`, the Tomorrow Open Source Technology License 1.0)
and `manifest.json`:

| Field | Value |
| --- | --- |
| Upstream | `pxr/imaging/hdx/textures/StinsonBeach.hdr`, OpenUSD tag `v26.05`, commit `2095fafafd033fa23386d7ec6d58c7cc33974518`; the same file Storm installs as its default dome texture |
| Format | Radiance RGBE, run-length encoded, 1024 × 512, `-Y 512 +X 1024` |
| Size, SHA-256 | 1,463,672 bytes, `4897697c757edc524dc9b7bcc692e8e05a7f02dbede3e30d2291dc0831dece17` |

CTest `lotus-validation-assets` checks each listed file's size and SHA-256
and that its licence file is present. `lotus-headless` reads the assets
from `assets/` beside it (`--assets <directory>` overrides); the install
tree puts them in `bin/assets/`, and `scripts/release.py` requires all
three files in a package. The headless adapter has no OpenUSD, so it reads
the image with a reader of its own that decodes RGBE as stb_image does,
each channel times 2^(e − 136); the Hydra test below compares it with Hio.

The image's sun holds 56% of its sin θ-weighted power in ten texels, at
radiance up to 28,843 around a mean of 0.18, which is what makes it a test
of importance sampling: a cosine-weighted direction finds the sun about
once in ten thousand.

## Checks

`renderer.path.environment` is a new headless check.

**Layout.** A mirror floor (white GGX metal, roughness 0) facing
OrthographicCamera reflects every camera ray straight up, to +Z. An 8×4
image of exact float colours has, in its middle rows, one colour around
each of +Z (columns 3–4), +X (1–2), −X (5–6) and −Z (0 and 7), and its
top and bottom rows a colour each, as the OpenEXR layout places them;
bilinear filtering at +Z under each frame sees only one region. Under
BSDF sampling every sample of every pixel must be that region's colour
times the scale (2, 1, 0.5):

| Frame | Region at +Z | Result |
| --- | --- | --- |
| identity | longitude 0 | exact |
| 90° about Y | −X | exact |
| −90° about Y | +X | exact |
| 180° about Y | −Z, across the image's left and right edges | exact |
| 90° about X | +Y, the top row | exact |
| −90° about X | −Y, the bottom row | exact |
| 90° about Y, with a constant sky (0.5, 0.25, 0.125) | −X plus the sky | exact |

The first version also required exact radiance under light sampling. It
failed at 0.4934 instead of 0.5 in one pixel: GGX at the minimum alpha
(10⁻³) still has lobe tails where the light sample's density is not
negligible against the BSDF's, so the power heuristic gives a sampled
direction slightly less than full weight and light samples make up the
rest only in expectation. The layout is shared code under both
estimators, so it is checked under BSDF sampling; the light-sampling
comparisons below cover the sampling.

**Floor oracle.** The uncoated Lambert floor of the analytic lights check
(albedo 0.8, 0.7, 0.6) fills an orthographic 64×64 view under the beach
image scaled by 0.75, its pole tilted to the floor's normal (90° about X),
turned 30° about its pole and scaled (2, 5, 3) along its axes before that
(which must not distort it), plus a constant sky (0.05, 0.04, 0.03).
The oracle is the floor's albedo over π times the irradiance, by the
midpoint rule on 4×4 points per texel over the image's 2π² sin θ du dv
steradians, of the bilinear image in double precision, with the OpenEXR
layout written out independently of the shader's. The floor is uniform:
0.37094 / 0.34033 / 0.32409. Each image is a 1024-spp mean of 16 batches
of 64 samples, compared by the DES-Q5 metric with the batches' per-sample
variance.

BSDF sampling hits the sun about once per ten pixels at 1024 spp, too
rarely for the metric's normal approximation, so both estimators are also
compared on a copy of the image whose texels are limited to 8 (oracle
0.070731 / 0.071792 / 0.087983):

| Image | Estimator, integrator | Image z | Max tile \|z\| |
| --- | --- | --- | --- |
| without the sun | BSDF, reference | −0.03 / −0.03 / 0.07 | 3.21 |
| without the sun | light sampling, reference | −0.95 / −1.17 / −1.43 | 2.82 |
| Stinson Beach | light sampling, reference | 0.38 / 0.14 / 0.81 | 2.85 |
| Stinson Beach | light sampling, wavefront | 0.38 / 0.14 / 0.81 | 2.85 |

**Open scene.** The open scene of the light sampling check (coated
diffuse floor, a rough metal panel, a 25% coverage layer) under the copy
without the sun, at 256 spp against BSDF sampling's 4096-spp estimate:
image z 0.49 / 0.54 / 0.44, max tile |z| 1.54, the same through the
wavefront integrator, whose 64 spp are the reference integrator's bit for
bit.

**Bookkeeping.** With an image, the GPU scene reads and samples it. A
constant edit restarts the accumulation and rewrites neither the light
table nor the image; a scale edit restarts it and rewrites the image's
distribution and the light table once each, with no geometry, texture,
instance, material, BLAS or TLAS work; an image whose key names no
texture reaches neither.

`renderer.path.reference`, `renderer.path.wavefront`,
`renderer.path.light_sampling` and `renderer.path.analytic_lights` report
the same values as before this change: the committed reference is
reproduced bit for bit and the Cornell box's light-sampled images are
unchanged.

### Variance and time

Each camera path on the floor scatters once and escapes, so a
BSDF-sampled sample is the albedo times the radiance in a
cosine-distributed direction, and the oracle's quadrature gives BSDF
sampling's exact per-sample variance as well. Sixteen batches measure it
only roughly when the sun dominates; on the copy without the sun the
measured value must be within 25% of the exact one.

| Image | BSDF sampling, exact | BSDF sampling, measured | Light sampling, measured | Lower |
| --- | --- | --- | --- | --- |
| Stinson Beach | 1648 | 1512 | 0.4056 | 4063× |
| without the sun | 0.008085 | 0.00815 | 0.001182 | 6.84× |

On the open scene without the sun, with the glossy panel and the
coverage layer, light sampling lowers the measured per-sample variance
2.43×.

Median GPU time of 16 single-sample 64×64 frames on the beach floor, in
the two final runs: BSDF 0.014 and 0.017 ms, light sampling 0.021 and
0.025 ms, so the time to equal quality is 2,640× to 2,800× shorter. As in
the earlier reports, frame times at this size vary between runs by tens
of percent, which does not change the order of magnitude. The sun is the
measurement the [constant environment's](2026-10-11-emissive-nee-mis.md#the-constant-environment)
lacked: there the sky had no structure and uniform directions lost to
cosine-weighted ones; here the image's power is concentrated, and
sampling it in proportion to its radiance wins by more than three orders
of magnitude.

### Fault injections

Each fault was built into the core configuration and the headless report
inspected; the source was restored with a fresh modification time after
each build. Every fault failed `renderer.path.environment` and no other
check.

| Fault | First failure |
| --- | --- |
| F1: escaping image radiance unweighted under light sampling | sunless floor under mis: image 196 SE |
| F2: the image sample's density without sin θ | sunless floor under mis: image 57 SE |
| F3: longitude increasing to the right | mirror, +Z turned to −X: 4 instead of 1 |
| F4: a cell chosen from the next row's distribution | beach floor under mis: image 11.8 SE |
| F5: the frame's axes not orthonormalized | sunless floor under bsdf: image −1769 SE |
| F6: shadow rays towards the image never blocked | open scene: image 11.6 SE |

F4 passes the copy without the sun, whose neighbouring rows differ little,
and fails on the sun, whose rows differ most. F6 passes the floor, which
nothing shades, and fails in the open scene, where the panel and the
layer do.

## Fixed benchmarks

The fixed workloads have no environment image. Commands, from the
repository root:

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/light03-bench-reference-bsdf.json --benchmark-label "9f937c5 + LOTUS-LIGHT-03; ost core Release" --benchmark-light-sampling bsdf
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/light03-bench-reference-mis.json --benchmark-label "9f937c5 + LOTUS-LIGHT-03; ost core Release" --benchmark-light-sampling mis
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/light03-bench-wavefront-bsdf.json --benchmark-label "9f937c5 + LOTUS-LIGHT-03; ost core Release" --benchmark-integrator wavefront
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/light03-bench-wavefront-mis.json --benchmark-label "9f937c5 + LOTUS-LIGHT-03; ost core Release" --benchmark-integrator wavefront --benchmark-light-sampling mis
```

Median GPU ms per 128×128 sample, against the
[analytic lights report](2026-10-11-analytic-lights.md#fixed-benchmarks)'s:

| Workload | Reference, BSDF | Reference, MIS | Wavefront, BSDF | Wavefront, MIS |
| --- | --- | --- | --- | --- |
| `cornell-v1` | 0.069 (0.069) | 0.099 (0.099) | 0.175 (0.176) | 0.222 (0.221) |
| `textured-v1` | 0.047 (0.047) | 0.048 (0.047) | 0.122 (0.121) | 0.122 (0.122) |
| `instanced-v1` | 0.036 (0.036) | 0.036 (0.035) | 0.101 (0.102) | 0.102 (0.101) |
| `geometry-v1` | 0.097 (0.096) | 0.100 (0.095) | 0.230 (0.228) | 0.232 (0.229) |

All sixteen images are byte-identical to that report's. The first
reference/MIS and wavefront/BSDF runs measured `geometry-v1` at 0.146 and
0.380 ms; repeats measured 0.100 and 0.230 ms with the same images, and
the table shows them, as that report did for the same workload.

## Hydra

`HdLotusDomeLight` now also reads `texture:file` and `texture:format`,
the dome's world transform (`HdLight::DirtyTransform`) and `domeOffset`,
which UsdImaging supplies for DomeLight_1 to align its pole axis and which
Lotus applies before the transform, as Storm's `HdStLight` does. The file
is decoded with the material textures' Hio path under a key of the dome's
own when the file changes, so colour, intensity, transform and visibility
edits keep the decoded image. A format other than `automatic` or
`latlong`, a file that does not decode and a transform without an
orientation each warn, and the dome lights uniformly with its radiance.
The first textured dome in path order that emits is the image; a further
one is left out with a warning, once.

CTest `lotus-renderer-hydra-dome-texture` (CPU):

| Step | Required | Measured |
| --- | --- | --- |
| a dome reading `StinsonBeach.hdr`, intensity 2 | an image of scale 2 under the dome's key, identity frame, no constant radiance; Hio's 1024 × 512 RGBA32F texels equal to the headless reader's, bit for bit | pass |
| colour edit, `rotateX` 90 | the scale follows; the image's pole turned to +Z; the same decoded texture | pass |
| hidden | no image; the texture still resident | pass |
| a second textured dome and an untextured one | the first is the image, the second warns, the untextured one's radiance is the constant | pass |
| the first removed | the second becomes the image; the first's texture leaves the scene | pass |
| DomeLight_1 with `poleAxis = "Z"` | the image's pole at +Z | pass |
| `texture:format = "mirroredBall"`, then `latlong`, then a missing file | uniform with a warning, then the image, then uniform with a warning | pass |
| the last dome removed | the white fallback environment | pass |

Its GPU-gated `-gpu` variant writes the 8×4 layout image as flat RGBE
(every colour exact in RGBE) and renders the dome test's mirror stage at
8×8, 4 spp, with `lotus:lightSampling` false: the untransformed dome, a
coloured one, turns of 90°, −90° and 180° about Y, a textured dome with an
untextured one, and DomeLight_1 with its pole on Z and on Y each converge
to the expected region's radiance within 10⁻⁵ after a restarted
accumulation, without geometry, BLAS or TLAS work.

## Runs

| Configuration | Result |
| --- | --- |
| core | `ost build`, `ost test` 20/20 (`lotus-validation-assets` new), `ost validate --strict-renderer-evidence` passed |
| Hydra | `ost build`, `ost test` 43/43 (`lotus-renderer-hydra-dome-texture` and `-gpu` new), strict validate passed |
| ci-core, without a runtime | `ost build`, `ost test` 12/12 |
| scripts | `scripts/check_docs.py` and the `scripts/test_*.py` regressions passed |

Before the builds, every object that `ninja -t deps` listed with `#deps 0`
was deleted ([building guide](../guides/BUILDING.md)).
