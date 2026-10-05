# Reference images

Measured on 2026-10-05 in the working tree based on `e83aeca`, on Windows 11
x86_64 with MSVC 14.51, OpenStrata 0.23.14 and Release builds. Vulkan headers
came from SDK 1.3.290 and `slangc` 2026.8 from SDK 1.4.350. The GPU was an
NVIDIA RTX A5000 (device API 1.4.329, loader API 1.4.321). Hydra used OpenUSD
26.08 lookdev and CPython 3.13. All runs were local; hosted CI was not
measured.

## Scope

This report covers the last step of the Renderer Phase 1 vertical slice: a
fixed deterministic scene, rendered by the headless runner at 1, 16, 64, 256
and 1024 spp, and compared with a committed reference. It is the *reference
path tracer* milestone of the
[roadmap policy](../design/ROADMAP_POLICY.md#renderer-phase-1--reference-path-tracer).
The [scene reference](../reference/SCENE.md#reference-images) describes the
scene, the files and the metric.

Three design decisions were settled before implementation:

- **The reference is committed and compared statistically.** GPU float
  results differ across vendors and drivers, so a bitwise golden image
  would hold only on the device that wrote it. Instead, the repository
  holds a 1024-spp mean and each pixel's per-sample variance. Every run
  renders an independent estimate and compares it with them. The images at
  1–256 spp are build outputs, not committed. This also settles DES-Q5.
- **The scene is a Cornell box.** It has red, green and white Lambert
  walls, an emitter under the ceiling, a GGX metal block and a Lambert
  block, and a black environment. It is rendered at 128×128.
- **The files are PFM.** Portable float maps need no dependency, so the
  runtime-free configuration reads and writes them too.

## The reference

`lotus-headless --write-reference validation/reference` took 0.70 s,
including device creation. It rendered 64 batches of 16 samples from sample
indices 0, 16, …, 1008, and then the 1024 samples in one accumulation. The
batches' mean had to equal the accumulation's within 10⁻⁶ + 10⁻⁴ relative;
only the summation order differs.

| File | Minimum | Maximum | Mean over pixels and channels |
| --- | --- | --- | --- |
| `cornell-box-mean.pfm` | 2.4 × 10⁻⁵ | 15.0 (the emitter) | 0.574 |
| `cornell-box-variance.pfm` | 0 | 58.2 | 1.70 |

Each file is 196,622 bytes. `.gitattributes` marks `*.pfm` binary, so
line-ending conversion cannot touch them.

The image was converted to sRGB PNG and inspected. It shows:

- the red and green walls bleeding colour onto the white surfaces;
- the emitter and the lit ceiling around it;
- the metal block reflecting the dark open front;
- the white block.

Brute-force BSDF sampling leaves visible noise at 1024 spp, as expected
without next-event estimation.

## Checks

**`renderer.path.reference`** is new. It reads the committed reference from
the `reference/` directory next to the executable. It then renders one
accumulation from sample index 2²⁰ and stops at 1, 16, 64, 256 and 1024 spp.
Every pixel's alpha must be 1. Each image is written with `--images` and
compared with the reference:

- **Image.** In each channel, the mean difference over the image must be
  within 5 standard errors.
- **Tiles.** In each channel, the mean difference over every tile must be
  within 5 standard errors. A tile is 8×8 pixels, doubled until it holds
  at least 1024 samples.

A pixel's difference has variance `s²(1/N + 1/M)`. A floor of
10⁻⁶ + 10⁻⁵ of the reference's magnitude is added to each standard error,
for pixels whose every sample has the same radiance, such as the emitter's.

Two more steps follow:

- **Power.** The red wall's albedo is scaled by 1.1. A 1024-spp render must
  then fail the comparison.
- **Reproduction.** The reference's own 1024 samples are rendered again and
  compared bitwise with the committed mean. The result is reported but does
  not decide the check, because another device or build need not reproduce
  the bytes.

## Results

The check passed. z is the difference in standard errors; the tile columns
give the largest |z| over all tiles and channels, and the mean of z² over
them.

| spp | Tile | Image z (R, G, B) | Max tile \|z\| | Mean tile z² |
| --- | --- | --- | --- | --- |
| 1 | 32×32 | −0.05, −0.47, 0.06 | 2.15 | 1.00 |
| 16 | 8×8 | 0.11, 0.82, 1.43 | 3.51 | 1.05 |
| 64 | 8×8 | 0.60, 0.85, 1.36 | 2.92 | 1.14 |
| 256 | 8×8 | 1.62, 1.25, 1.30 | 2.81 | 1.00 |
| 1024 | 8×8 | 2.48, 2.29, 2.22 | 2.90 | 1.05 |

- The red wall 10% brighter was rejected at a maximum tile |z| of 7.63.
- The reference's samples reproduced the committed mean bit for bit.

The mean of z² near 1 at every level shows that the estimated variance
describes the actual scatter. The levels are one accumulation, so their
image z values are correlated. The three channels share their paths, so
the 1024-spp row is one fluctuation of about 2.4 standard errors, not three.

To see whether that fluctuation was chance or a correlation between sample
sequences, six other first sample indices were built and run in place of
2²⁰. All passed at every level:

| First sample index | 1024-spp image z (R, G, B) | Max tile \|z\| at 256 and 1024 spp | Brighter wall rejected at |
| --- | --- | --- | --- |
| 2 × 2²⁰ | −0.32, −0.29, −0.13 | 3.49 | 7.79 |
| 3 × 2²⁰ | 0.61, 0.72, 0.41 | 3.20 | 7.92 |
| 2²⁴ | −0.60, −0.46, −0.59 | 2.86 | 8.63 |
| 123456789 | 0.90, 0.37, 0.73 | 3.46 | 7.48 |
| 3000000000 | 0.79, 0.37, 0.74 | 3.67 | 7.98 |
| 4096 | 0.84, 0.65, 0.51 | 3.35 | 8.62 |

The spread is consistent with chance, so the committed check keeps 2²⁰, the
index chosen before any run.

`lotus-renderer-evidence`, which runs every headless check, took 0.97 s.

## Fault injections

Each change below was built and run on its own, with the reference left as
committed. Every check not listed passed.

| Change | Failing checks and first messages |
| --- | --- |
| Russian roulette without dividing by the survival probability | `multibounce`: mean 0.190585 instead of 0.200000 ± 0.000075; `reference`: 16 spp, the image, channel 0: −11.05 standard errors |
| Emission added only at the camera ray's hit | `multibounce`: 1 bounce: radiance 0.100000 instead of 0.150000; `reference`: 1 spp, the image, channel 0: −25.68 standard errors |
| GGX alpha equal to roughness instead of its square | `bsdf`: GGX metal, roughness 0.5: mean 0.700287 instead of 0.857260 ± 0.006995; `reference`: 16 spp, the tile at 48,56, channel 1: 6.08 standard errors |
| Every sample through the pixel centre | `accumulation`: coverage 1.000000 instead of 0.514503 ± 0.160091; `reference`: 16 spp, the tile at 56,16, channel 2: 5.04 standard errors |
| No ray origin offset (self-intersection) | `bsdf`, `multibounce`, `accumulation`; `reference`: 1 spp, the image, channel 0: −6.45 standard errors |

The reference check caught each one. Each fault above is also caught by an
earlier check. The pixel-centre fault only moves the mean along the
emitter's edges, so the reference check caught it with little margin.

## Verification

Before the final builds, the documented workaround for the Japanese MSVC
dependency issue was applied to every build tree.

| Configuration | Build and test | Strict validation and other runs |
| --- | --- | --- |
| core | `ost build --jobs auto`, `ost test`: **8/8 passed** | `ost validate --strict-renderer-evidence`: **passed**, including `renderer.path.reference`; the installed `lotus-headless` passed it from `bin/reference/` |
| Hydra | `ost build --profile lookdev --intent hydra --jobs auto`, `ost test --profile lookdev --intent hydra`: **14/14 passed** | `ost validate --profile lookdev --intent hydra --strict-renderer-evidence`: **passed** |
| runtime-free ci-core | `ost build --without-runtime --intent ci-core --jobs auto`, `ost test --without-runtime --intent ci-core`: **8/8 passed** | `renderer.path.reference` SKIPs with "Vulkan backend was not compiled for this configuration" |
| viewport | — | `ost renderer viewport -- --frames 8 --hidden` presented 8 frames |

These were not measured:

- the comparison on another GPU, vendor or driver — the case the
  statistical match exists for;
- devices without ray queries;
- synchronization validation.
