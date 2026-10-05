# BSDFs and multi-bounce transport

Measured on 2026-10-05 in the working tree based on `be7e507`, Windows 11
x86_64, MSVC 14.51, OpenStrata 0.23.14, Release. Vulkan headers came from
SDK 1.3.290; `slangc` 2026.8 came from SDK 1.4.350. The GPU was an NVIDIA
RTX A5000, device API 1.4.329 and loader API 1.4.321. Hydra used OpenUSD
26.08 lookdev and CPython 3.13. These are local runs; hosted CI was not
measured.

## Scope

The Renderer Phase 1 step after the first ray-traced triangle: surface hit
reconstruction, a Lambert and GGX-metal BSDF, emissive surfaces, a constant
environment, multiple bounces and Russian roulette, as one brute-force path
per pixel centre. The pass and its parameters are in the
[scene reference](../reference/SCENE.md#path-tracing). Its colour output is
one sample per frame, clamped to [0, 1] in RGBA8; there is no HDR
accumulation, so the comparisons below average frames on the CPU.

The design decision this step required, DES-Q3, was settled before
`PathState` was written: light-carrying values are a `Spectrum` type that
holds RGB ([design policy §53](../design/DESIGN_POLICY.md#53-open-questions)).

## Scenes and expected radiance

`lotus-headless` renders every scene into a 64×64 target. Each frame is one
sample per pixel; a pixel's random sequence is seeded from its index in the
target and the frame's `sample_index`. Exact cases use sample index 0;
mean cases average sample indices 0–15, 65,536 samples per channel.

`renderer.path.bsdf` uses single-scattering scenes, where no reflected ray
can hit the scene again:

- **Lambert, exact.** The primary-ray fixture's triangle, base colour
  (0.5, 0.25, 0.75), emission (0.1, 0.05, 0), environment (0.8, 0.6, 0.4).
  Cosine sampling of a Lambert surface under a constant environment returns
  albedo × environment for every direction, so every hit pixel must be
  (0.5, 0.2, 0.3) within one byte, front face and back face (the triangle
  turned half a revolution about Y). With `max_bounces` 0 it must be the
  emission alone. Coverage, misses and depth must equal those of the
  barycentric output of the same frame.
- **GGX directional albedo, mean.** An orthographic camera looks down −Z at
  a plane tilted 60 degrees, so every pixel has cos θ_o = 0.5, under a white
  environment. The expected value is the BSDF's directional albedo,
  computed on the CPU by a 1024×1024 midpoint rule over the GGX
  distribution's inverse CDF in half-vector space. It shares the
  definitions of D, Smith G2 and Schlick's F with the shader, not its
  visible-normal sampling, reflection, shading frame or weights.

`renderer.path.multibounce` uses a closed box from (−1, −1, −1) to
(1, 1, 1), seen from its centre through a 90-degree perspective camera.
Every surface has albedo a = (0.5, 0.25, 0.75) and emission
Le = (0.1, 0.15, 0.025); the environment is white, so a ray escaping
through a crack would show. A path bounces until it ends, so:

- **Bounce limits, exact.** With `max_bounces` n ≤ 3, before Russian
  roulette starts, every pixel is Le (1 + a + … + aⁿ) within one byte, for
  n = 0, 1 and 3.
- **Russian roulette, mean.** With the default 64 bounces, the expectation is
  Le / (1 − a) = (0.2, 0.2, 0.1).
- **Determinism.** Sample index 0 rendered twice gives identical bytes;
  sample index 1 gives a different image.

A mean passes when it is within 5 standard errors, estimated from the
samples, plus half a byte of rounding, of the expected value. Clamping is
negligible here: GGX and mixture samples cannot exceed 1 under a white
environment, and a box sample exceeds 1 only after about 18 surviving
bounces.

## Results

The exact cases all passed. Means, as measured / expected ± tolerance:

| Case | Red | Green | Blue |
| --- | --- | --- | --- |
| GGX metal, roughness 0.5, F0 (1, 0.6, 0.2) | 0.8578 / 0.8573 ± 0.0073 | 0.5235 / 0.5233 ± 0.0051 | 0.1892 / 0.1893 ± 0.0031 |
| GGX metal, roughness 0.25, F0 (0.9, 0.5, 0.1) | 0.8937 / 0.8931 ± 0.0034 | 0.5103 / 0.5107 ± 0.0028 | 0.1280 / 0.1282 ± 0.0024 |
| metallic 0.5, roughness 0.5, base (0.9, 0.6, 0.3) | 0.8362 / 0.8369 ± 0.0055 | 0.5615 / 0.5616 ± 0.0043 | 0.2856 / 0.2864 ± 0.0031 |
| Box, Russian roulette | 0.2007 / 0.2000 ± 0.0023 | 0.2000 / 0.2000 ± 0.0020 | 0.0991 / 0.1000 ± 0.0031 |

These are the same for every run on this build: the scenes, cameras and
sample indices are fixed.

## Fault injections

Each of these shader changes was built and run on its own, after the
isolation fix below. The named checks failed; any check not named passed:

| Change | Failing check and first message |
| --- | --- |
| GGX density without G1 | `bsdf`: roughness 0.5 red mean 0.8209 instead of 0.8573 ± 0.0070 |
| Fresnel at θ_o instead of the half-vector angle | `bsdf`: roughness 0.5 blue mean 0.1929 instead of 0.1893 ± 0.0032 |
| Russian roulette without dividing by the survival probability | `multibounce`: red mean 0.1906 instead of 0.2000 ± 0.0020 |
| Secondary ray origins not offset | `bsdf`: Lambert front face 89 instead of 128; `multibounce`: three bounces 102 instead of 48 |
| A uniform-hemisphere density for the cosine-sampled Lambert lobe | `bsdf`: Lambert front face 99 instead of 128; `multibounce`: one bounce 44 instead of 38 |

The first runs of these injections changed nothing: the adapters copied
their SPIR-V only when they relinked, so a shader-only edit ran the old
shaders. The adapters now relink when the SPIR-V changes. The next runs
showed a failed BSDF scene leaking its meshes into the box; each scene is
now cleared whether or not it passes.

## Hydra

Hydra renders with the default settings: every mesh has the default
`SurfaceMaterial` (0.18 grey Lambert) and the adapter sets a white
environment. `lotus-renderer-hydra-aov` checks that the triangle's centre
pixel is 46 of 255 in each channel, then repeats its existing silhouette,
topology, visibility and clear checks. The usdview smoke test logged
`ray_query=1` and zero validation messages for every frame; its first-frame
and stable-update PNGs were inspected and show the triangle in uniform grey
before and after the point edit.

## Verification

The documented Japanese-MSVC dependency workaround was applied before the
final builds: it removed two missing-dependency objects from core, 14 from
Hydra and eight from runtime-free ci-core, none from the viewport, and they
were rebuilt.

- `ost build --jobs auto`, `ost test`: **8/8 passed**.
  `ost validate --strict-renderer-evidence`: **passed**, including
  `renderer.path.bsdf` and `renderer.path.multibounce`.
- `ost build --profile lookdev --intent hydra --jobs auto`,
  `ost test --profile lookdev --intent hydra`: **14/14 passed**.
  `ost validate --profile lookdev --intent hydra --strict-renderer-evidence`:
  **passed**, zero Vulkan validation messages.
- `ost build --without-runtime --intent ci-core --jobs auto`,
  `ost test --without-runtime --intent ci-core`: **8/8 passed**. Both path
  checks SKIP with "Vulkan backend was not compiled for this
  configuration".
- `ost renderer viewport -- --frames 8 --hidden`: presented 8 frames.

Devices without ray queries, non-NVIDIA GPUs and synchronization validation
were not measured. HDR accumulation and the reference images remain
[roadmap work](../roadmap/current.md#renderer-phase-1--reference-path-tracer).
