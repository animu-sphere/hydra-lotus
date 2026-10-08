# Constant dome lighting through Hydra

Measured on 2026-10-08 in the working tree based on `0516993`, on Windows
x86_64 with MSVC 14.51, OpenStrata 0.23.14 and a Release Hydra build. The
GPU was an NVIDIA RTX A5000; Hydra used OpenUSD 26.08 lookdev and Python
3.13. These were local runs; hosted CI was not measured.

## Scope

The adapter follow-up in the roadmap: read a dome light's colour and
intensity into the existing constant environment. Exposure is included
according to [UsdLux LightAPI](https://openusd.org/release/user_guides/schemas/usdLux/LightAPI.html).
No core or shader representation changes were needed. Current behaviour
and unsupported inputs belong to the [scene reference](../reference/SCENE.md#lights).

## CPU evidence

`lotus-renderer-hydra-light` syncs a composed USD stage through UsdImaging's
scene indices and Hydra's engine. It starts without lights, then adds,
edits, hides and removes two `DomeLight` prims. Assertions check:

- White fallback with no domes, schema defaults on a newly added dome,
  and fallback sprim creation/destruction having no scene effect.
- Colour, intensity and positive/negative exposure edits, including HDR
  radiance `(1, 2, 4)`; additive radiance from two domes.
- Inherited parent visibility, individual visibility, deletion of a
  visible dome while another remains invisible, and last-dome removal.
- Zero/negative intensity and negative colour channels; non-finite
  intensity, exposure and colour, overflow, zero colour at an overflowing
  exposure, and underflow. Expected invalid-input warnings are emitted.
- Immutable old snapshots, unchanged scene identity on an idle sync,
  and preserved mesh geometry across all light edits.

The test runs without creating a Vulkan renderer.

## GPU evidence

`lotus-renderer-hydra-light-gpu` uses the same USD stage with a large planar
white ideal GGX metal mirror: roughness 0, metallic 1, facing a perspective
camera at `(0, 0, 4)`. Every primary ray hits it, every reflected ray misses
into the uniform environment, and unit Fresnel yields the independently
known RGB radiance of that environment.

The AOV is 8×8 RGBA32F, 4 spp, first sample index 123, rendered one sample
per Hydra pass, with the default 64-bounce limit. Every pixel's RGB is
compared with the expected radiance within `1e-5`; alpha must be exactly 1.
This covers white fallback, HDR and dim coloured lighting, additive domes,
visibility, black/negative-intensity lighting and deletion/restoration.
The CPU-only malformed-input cases are not rendered.

Every aggregate-radiance change must restart convergence for exactly four
passes. Edits preserving the aggregate, including adding a default white
dome to the white fallback and changing zero intensity to negative
intensity, must preserve convergence. An unchanged frame remains converged.
Geometry upload, BLAS build, TLAS build and TLAS update counters are unchanged
across all lighting edits.

## Commands and results

- `ost build --profile lookdev --intent hydra --jobs auto`: passed.
- `ctest --test-dir build/cy2026-windows-x86_64-py313-lookdev--hydra -R 'hydra-light' --output-on-failure`:
  both new CPU/GPU tests passed.
- `ost test --profile lookdev --intent hydra`: **25/25 passed**, including
  the new CPU/GPU tests, existing material/texture/deterministic checks,
  core boundary, install tree and usdview host smoke test.
- `ost validate --profile lookdev --intent hydra --strict-renderer-evidence`:
  **passed**. The Cornell comparisons at 1, 16, 64, 256 and 1024 spp passed,
  and the reference sequence reproduced the committed mean bit for bit.
  Renderer validation reported zero messages.

No core, shader or committed reference files changed. GPU checks remain capability-gated,
returning CTest SKIP 77 without a usable ray-query backend.
