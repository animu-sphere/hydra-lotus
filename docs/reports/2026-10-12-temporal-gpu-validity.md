# GPU backward reprojection and shared validity

Measured on 2026-10-12 against `48fb0d5` plus LOTUS-TEMPORAL-03, using MSVC
Release, Vulkan/Slang, the OpenStrata core and OpenUSD 26.08 lookdev/Hydra
configurations, and an NVIDIA RTX A5000 on Windows. The runtime-free
configuration disables Vulkan. The [temporal reference](../reference/TEMPORAL.md#gpu-reprojection-and-validity)
owns the product and precision contract; the [roadmap](../roadmap/README.md#status-at-a-glance)
owns phase status.

## CPU oracle and moving sequences

`lotus-renderer-temporal-gpu` compares every pixel's validity, rejection code
and row-major previous-frame fetch index with `ReprojectTemporalHistory`.
The CPU fetch uses the same float pixel coordinates and invokes the existing
double-intermediate `ValidateTemporalHistory` geometry oracle. Both
integrators use the fixed pixel-centre/sample-0 coverage realization;
radiance uses the default sample index 0 and zero scattering bounces.
The stationary sequence adds 3 then 2 progressive samples; subsequent
sequence calls generally add one, including a converged 5-sample call that
adds none. The primary scene is a large plane at Z=-4, rendered at 64×48,
with cropped/display-window checks at 80×60.

Each integrator compared 142,656 pixels, with the same rejection histogram:

| Result | Pixels per integrator |
| --- | ---: |
| Valid (`None`) | 53,716 |
| `Miss` | 39,392 |
| `InvalidData` | 3,072 |
| `InstanceChanged` | 192 |
| `MaterialChanged` | 9,216 |
| `LargeMotion` | 2,240 |
| `DepthDiscontinuity` | 48 |
| `NormalDiscontinuity` | 1,405 |
| `HistoryUnavailable` | 23,616 |
| `CorrespondenceMissing` | 7,872 |
| `ReprojectionOutside` | 1,887 |

The test requires nonzero coverage for every code, rather than accepting an
all-invalid output. It also checks:

- All stationary plane pixels reuse history. Camera, object and combined
  motion retain independently checked projection/depth/normal products.
  Material/used-texture edits reject history while unused texture edits and
  unrelated table insertions preserve identity. Instancing changes reject
  correspondence; resets recover on the next compatible frame.
- A single mesh containing a background plane at Z=-4 and a foreground quad
  at Z=-2 exercises self-disocclusion without changing instance identity.
  Moving the camera eye +0.5 in X reveals background whose previous fetch
  finds the foreground: 48 pixels reject depth. A separate foreground
  placement translated +1.5 in X supplies the instance-discontinuity case.
- Rotating the plane 60 degrees about Y exercises normal discontinuity.
  Temporarily lowering the allowed motion to 1 pixel supplies large-motion
  rejection. A normal-cosine threshold of 2 produces `InvalidData` without
  consuming/resetting the compatible history stream.
- A fresh renderer starts with a 4×3 empty scene, before any TLAS is bound.
  All 12 debug pixels are `Miss`, invalid and unfetched. A missing
  push-constant initialization discovered by this case was fixed in the
  validity pass, independently of whether a primary draw occurs.
- Duplicate/failed frames retain the last successful surface history;
  camera cuts, frame gaps, compatibility keys and framing changes produce
  missing history. Empty scenes and pixels outside the data window overwrite
  their debug records. Ordinary progressive colour/depth remain unchanged.

Every successful GPU frame requires zero Vulkan validation messages,
including synchronization validation. CPU-only checks additionally exercise
nearest fetches, integer pixel boundaries, top-left/right image edges,
non-finite motion, absent/malformed history and rejection priority; existing
geometry tests cover inclusive motion, depth and normal thresholds.

An exploratory foreground translation of +2 produced nominal motion length
32 and a boundary disagreement: the GPU float predicate accepted history
while the CPU double predicate rejected large motion. The
measured independent-occluder case uses +1.5 (24-pixel motion), away from the
threshold. Exact code agreement is established for these sequences, not for
all values within float rounding distance of a threshold. This precision
limit is part of the [contract](../reference/TEMPORAL.md#gpu-reprojection-and-validity).

## GPU time and memory

The final targeted core correctness run measured the stationary
primary-surface/validity passes at 0.040128/0.017408 ms under reference
transport and 0.050624/0.021504 ms under wavefront transport. These are single
timestamp samples, not a comparative performance benchmark. The primary
pair covers its buffer clear and surface draw; the validity pair covers the
fullscreen backward-fetch/rejection draw. Both exclude host metadata writes
and readback.

At 64×48, surface buffers and their readback mirror remain 786,432 bytes.
The additional validity device buffer and readback mirror total 98,304 bytes
(32 bytes per pixel). One-instance metadata is 352 bytes: a 256-byte
camera/window/threshold block and a 96-byte instance record. Fixed extents
reuse the buffers; resize replaces them. The second pipeline and expanded
four-query timestamp pool are created at renderer initialization.

## Regression commands

Commands run from the repository root:

```sh
ost build --jobs auto
ctest --test-dir build/cy2026-windows-x86_64-py313-core -R 'temporal|core-boundary' --output-on-failure -V
ctest --test-dir build/cy2026-windows-x86_64-py313-core --output-on-failure
ost build --without-runtime --intent ci-core --jobs auto
ctest --test-dir build/cy2026-windows-x86_64-py313-core-runtime-free--ci-core --output-on-failure
ost build --profile lookdev --intent hydra --jobs auto
ctest --test-dir build/cy2026-windows-x86_64-py313-lookdev--hydra --output-on-failure
ost test --filter 'install-tree'
ost validate --strict-renderer-evidence
ost test --profile lookdev --intent hydra --filter 'install-tree'
ost validate --profile lookdev --intent hydra --strict-renderer-evidence
python scripts/check_docs.py
python -m unittest discover -s scripts -p 'test_*.py'
```

The final targeted core checks passed 5/5, including the explained missing
driver SKIP regression. The full physical-GPU core suite passed 23/23 and
the full Hydra suite passed 46/46, including the usdview host smoke and
existing light/material/integrator regressions. Runtime-free tests passed
13 checks with one explained GPU SKIP out of 14. Documentation links,
anchors, indexes and ownership checks passed, as did all 27 Python
documentation/release regression tests. Logs use the `build/temporal03-` prefix:
`core-build.log`, `targeted-test.log`, `core-test.log`,
`runtime-free-build.log`, `runtime-free-test.log`, `hydra-build.log` and
`hydra-test.log`.

Core and Hydra install-tree checks also passed through `ost test`, and both
configurations passed strict renderer-evidence validation. Core's Hydra
checks are explained SKIPs; no new release or package was prepared. Those
logs are `build/temporal03-core-install-test.log`,
`build/temporal03-hydra-install-test.log`,
`build/temporal03-core-validate.log` and
`build/temporal03-hydra-validate.log`.

## Scope disposition

This slice supplies GPU backward metadata fetch, shared geometric rejection,
debug validity/rejection products and per-pass timestamps without radiance,
reservoir or denoiser state. Temporal radiance accumulation remains the next
slice. Hydra temporal settings and AOV bindings remain in the integration
slice. No temporal convergence or ghosting claim is made here.
