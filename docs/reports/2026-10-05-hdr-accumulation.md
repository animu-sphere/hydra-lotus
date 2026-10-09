# HDR accumulation

Measured on 2026-10-05 in the working tree based on `e2b660e`, on Windows 11
x86_64 with MSVC 14.51, OpenStrata 0.23.14 and Release builds. Vulkan headers
came from SDK 1.3.290 and `slangc` 2026.8 from SDK 1.4.350. The GPU was an
NVIDIA RTX A5000 (device API 1.4.329, loader API 1.4.321). Hydra used OpenUSD
26.08 lookdev and CPython 3.13. All runs were local; hosted CI was not
measured.

## Scope

This report covers the Renderer Phase 1 step after BSDFs and multiple
bounces: floating-point radiance accumulated across sample indices, a pixel
filter, and a float colour AOV. It is the *first physically correct image*
milestone of the
[roadmap policy](../design/ROADMAP_POLICY.md#renderer-phase-1--reference-path-tracer).
The [scene reference](../reference/SCENE.md#path-tracing) and the
[AOV reference](../reference/AOVS.md) describe the pass and its output.

Three design decisions were settled before implementation:

- **The backend owns the accumulation.** `RenderScene` adds `frame_count`
  samples per pixel to a persistent RGBA32F image. It restarts the image
  when the camera, the target's size or windows, `sample_index`,
  `max_bounces` or the scene changes. `max_samples` caps it.
- **Pixel filter: a 1-pixel box.** Each sample's camera ray goes through a
  uniformly distributed point of the pixel, so every pixel's estimate is
  independent of its neighbours'.
- **Hydra colour defaults to `HdFormatFloat32Vec4`.** 8-bit colour buffers
  are still accepted, and their values are clamped.

The scene pass is split into two pipelines built from one SPIR-V module,
selected by a specialization constant:

- The camera pass traces each pixel centre and writes depth, plus the
  barycentric colour when that output is requested.
- The radiance pass adds one box-filtered sample to the accumulation image
  and writes its mean. It neither tests nor writes depth, so a depth test
  against preserved depth cannot hide a colour the accumulation has already
  counted.

A missed sample counts as the target's clear colour, as in HdEmbree. A
pixel whose samples all missed keeps the colour attachment.

## Checks

`lotus-headless` renders into 64×64 targets with clear colour
(0.05, 0.1, 0.15, 0). Because alpha 0 is cleared, a pixel's alpha is the
fraction of its samples that hit.

**`renderer.path.bsdf` and `renderer.path.multibounce`**, adapted to
floating-point output:

- **Exact cases.** Each case renders a fresh 4-sample accumulation. These
  values must hold within 10⁻⁶ + 10⁻⁴ relative:
  - Alpha must be a multiple of ¼.
  - A pixel with alpha `a` must equal `a·L + (1 − a)·clear`.
  - A pixel with alpha 0 must hold the clear colour exactly.
  - Depth must equal the barycentric pass's.
- **Means.** Each mean is one 16-sample accumulation. It is compared over
  the pixels whose samples all hit. The standard error is estimated from
  those pixels' means and must be within five of them.
- **Determinism.** Sample indices 1, 2 and then 1 again each restart the
  accumulation. The two index-1 images must be identical, and the index-2
  image must differ from them.

**`renderer.path.accumulation`** is new. It uses the primary-ray fixture's
triangle: a Lambert surface with base (0.5, 0.25, 0.75) and emission
(2.5, 1.25, 0.5), under environment (2, 1, 0.5). Every hit therefore
carries the HDR radiance (3.5, 1.5, 0.875). The check has three parts.

1. **Estimator.** The check renders 256 samples in one call.
   - The CPU clips the projected triangle against each pixel's square
     (Sutherland–Hodgman) to get its area fraction. Each pixel's alpha must
     match that fraction within five binomial standard deviations plus one
     sample. The sum of alpha over the image must match the projected area
     within five standard deviations.
   - Each pixel's colour must equal `a·L + (1 − a)·clear`.
   - Some value must exceed 1, which shows the output is not clamped.
2. **Split frames.** The material changes to a half-metallic GGX mixture,
   so every sample's radiance is random. Sixteen samples in one call must
   be bitwise identical to 1 + 3 + 12 samples in three calls.
3. **State.** Each step must report the stated samples per pixel:
   - **Continue the accumulation:** an unchanged frame; a barycentric frame
     in between (the barycentric frame itself reports 0); a clear-colour
     change.
   - **Restart it:** a camera change and its restoration; a data-window
     change; a display-window change; a bounce-limit change; an environment
     change; a target resize.
   - **`max_samples` 4, after one sample:** asking for ten frames submits 3
     and stops at 4. Asking again submits once and writes identical bytes.
     Raising the limit to 6 continues the accumulation.

## Results

All checks passed. The means, given as measured / expected ± tolerance,
were the same in every run of this build:

| Case | Red | Green | Blue |
| --- | --- | --- | --- |
| GGX metal, roughness 0.5 | 0.8581 / 0.8573 ± 0.0053 | 0.5237 / 0.5233 ± 0.0032 | 0.1893 / 0.1893 ± 0.0011 |
| GGX metal, roughness 0.25 | 0.8929 / 0.8931 ± 0.0015 | 0.5105 / 0.5107 ± 0.0008 | 0.1281 / 0.1282 ± 0.0004 |
| metallic 0.5, roughness 0.5 | 0.8383 / 0.8369 ± 0.0035 | 0.5626 / 0.5616 ± 0.0023 | 0.2868 / 0.2864 ± 0.0011 |
| Box, Russian roulette | 0.1999 / 0.2000 ± 0.0003 | 0.2000 / 0.2000 ± 0.0000 | 0.0997 / 0.1000 ± 0.0011 |

The tolerances are tighter than in the
[BSDF and multi-bounce report](2026-10-05-bsdf-multibounce.md), for two
reasons:

- They are estimated from 16-sample pixel means rather than from single
  samples.
- They no longer include half a byte of 8-bit rounding.

The box's green tolerance rounds to zero. Its paths reach roulette with
green throughput 0.25³, so green varies only below 10⁻⁴.

For the box filter, 111 pixels were partly covered. The total coverage was
444.301 px, against an expected 444.307 ± 1.251 px at 256 spp. The peak
radiance was 3.500.

`renderer.ray_query.timestamp` measured the barycentric scene pass, now the
camera pass alone, at 0.009 ms.

## Fault injections

Each change below was built and run on its own. Every check not listed
passed.

| Change | Failing check and first message |
| --- | --- |
| Every sample through the pixel centre | `accumulation`: coverage 1.000000 instead of 0.514503 ± 0.160091 at 35,14 |
| Mean divided by the earlier samples only | `bsdf`: alpha 0.666667 is not a fraction of 4 samples; `multibounce`: alpha 1.333333 …; `accumulation`: coverage 1.003922 instead of 1.000000 ± 0.003906 |
| Missed samples not counted as the background | `bsdf`: radiance 0.250000 instead of 0.275000 in channel 0; `accumulation`: radiance 1.804688 instead of 1.828906 |
| Every sample of an accumulation with the same random sequence | `accumulation`: coverage 1.000000 instead of 0.514503 ± 0.160091 |
| Scene updates not restarting the accumulation | `bsdf`: GGX metal, roughness 0.25: 32 samples instead of 16; `accumulation`: an environment change: 2 samples instead of 1 |

## Hydra

The adapter's colour AOV is `HdFormatFloat32Vec4` by default. It also
accepts `HdFormatUNorm8Vec4` buffers, and conversion to them clamps and
rounds.

Each Hydra pass adds one sample. It stops when the accumulation reaches the
`convergedSamplesPerPixel` render setting, which defaults to 64. Until then,
the render pass and the colour buffer report not converged. Depth and IDs
converge after one pass.

The Hydra tests check this behaviour:

- `lotus-renderer-hydra-render-buffer` checks the colour conversions in
  both directions:
  - HDR values and negative values are kept in a float buffer.
  - Values are clamped into 8 bits.
  - 8-bit values widen to floats.
  - Unknown and mis-sized products are rejected.
- `lotus-renderer-hydra-aov` keeps its existing checks with
  `convergedSamplesPerPixel` set to 1. It then checks a float buffer with a
  limit of 4:
  - The pass converges exactly at the fourth pass.
  - A fifth pass leaves the image unchanged.
  - The interior holds 0.18 radiance.
  - A transform edit restarts the accumulation.

In the usdview smoke test, `testusdview`'s wait for convergence ran the
first frame to `samples=64 converged=1`. The point edit restarted the
accumulation, which converged at 64 again. Every frame logged
`ray_query=1` and zero validation messages. The stable-update PNG was
inspected. It shows the triangle with antialiased edges, and it shows the
0.18 grey through usdview's sRGB colour correction.

## Verification

Before the final builds, the documented workaround for the Japanese MSVC
dependency issue was applied to every build tree.

| Configuration | Build and test | Strict validation and other runs |
| --- | --- | --- |
| core | `ost build --jobs auto`, `ost test`: **8/8 passed** | `ost validate --strict-renderer-evidence`: **passed**, including `renderer.path.accumulation` |
| Hydra | `ost build --profile lookdev --intent hydra --jobs auto`, `ost test --profile lookdev --intent hydra`: **14/14 passed** | `ost validate --profile lookdev --intent hydra --strict-renderer-evidence`: **passed** |
| runtime-free ci-core | `ost build --without-runtime --intent ci-core --jobs auto`, `ost test --without-runtime --intent ci-core`: **8/8 passed** | The three path checks SKIP with "Vulkan backend was not compiled for this configuration" |
| viewport | — | `ost renderer viewport -- --frames 8 --hidden` presented 8 frames |

These were not measured:

- devices without ray queries or `fragmentStoresAndAtomics`;
- non-NVIDIA GPUs;
- synchronization validation.

The reference images at 1–1024 spp remain
[roadmap work](../roadmap/README.md#status-at-a-glance).
