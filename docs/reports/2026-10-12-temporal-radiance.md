# Bounded temporal radiance accumulation

Measured on 2026-10-12 against `19209df` plus LOTUS-TEMPORAL-04, using
MSVC Release, Vulkan/Slang, OpenStrata core and OpenUSD 26.08 lookdev/Hydra
configurations, and an NVIDIA RTX A5000 on Windows. The runtime-free
configuration disables Vulkan. The [temporal reference](../reference/TEMPORAL.md#temporal-radiance-accumulation)
owns the API, reset policy and reconstruction limits; the
[roadmap](../roadmap/README.md#status-at-a-glance) owns phase status.

## Independent batch and static checks

`lotus-renderer-temporal-gpu` now runs a radiance sequence with each
integrator, alongside its existing 142,656-pixel surface/validity sequence.
The radiance sequence uses a separate ordinary renderer and extraction
stream. It independently renders each fresh batch with the reported RNG
start, computes expected bounded counts and double-precision weighted
means from the preceding successful output and the shared fetch index,
and compares every RGBA/count record and final colour attachment. It also
checks that preceding GPU radiance records survive unchanged until the
next successful frame.

The initial scene is a 64×48 perspective view of a large plane at Z=-4:
GGX metal, roughness 0.5, base colour (0.6, 0.4, 0.2), constant environment
(1, 2, 3), one scattering bounce, MIS and sample index 31. It has stochastic
view-dependent radiance, rather than constant emission alone. Eight frames
of two fresh samples produce the same 16-sample mean as ordinary progressive
transport: maximum RGBA difference 2.38419e-7 under both integrators. Every
primary hit holds count 16. Ten more two-sample frames reach and exceed the
32-sample cap; a 64-sample batch additionally verifies replacement when
the batch exceeds the cap. Every step is checked against the independent
batch/count oracle.

Across the complete radiance sequence each integrator compared 259,584
pixels, accepted 157,024 shared history fetches, and rejected history for
90,528 current hits. The maximum difference from the double weighted-mean
oracle was 2.88089e-7. The two integrators produced the same reported counts,
static errors and moving-image errors. Existing metadata-only sequences
still compare all rejection codes with the CPU oracle and progressive
colour/depth with ordinary transport.

## Motion and exposed surfaces

Three compatible moves preserve frame/radiance history: camera eye X +0.25;
plane translation (+0.5, +0.25, 0); then camera eye (+0.5, 0, +0.1). Each
moving image is compared with an independently rendered 256-sample image
starting at sample index 8192, and with its own fresh two-sample image.
The average RGB mean squared errors over the three images are:

| Integrator | Temporal mean | Fresh two-sample mean |
| --- | ---: | ---: |
| Reference | 0.00200918 | 0.0149886 |
| Wavefront | 0.00200918 | 0.0149886 |

These are measurements on three poses of this plane, not a general
reconstruction benchmark. The reference is finite-sample, and motion changes
view-dependent shading. Bounded history can retain that old shading; there
is no radiance clamp or anti-lag mechanism. The result does not demonstrate
absence of ghosting for specular objects, moving shadows, indirect light,
fractional coverage or jittered silhouettes.

A separate foreground quad at Z=-2 emits red (8, 0, 0) before translating
+1.5 in X. The background emits (0.2, 0.4, 0.8), the target is 80×48, and
zero scattering bounces isolate directly visible colour. The move exercises
638 `InstanceChanged` pixels. Each exposed hit drops the old foreground
count to one fresh sample; the per-pixel fresh-batch oracle verifies that
no rejected foreground colour survives. This establishes stale-history
removal at shared rejection, including the actual box-filtered fresh colour;
it does not establish a silhouette filtering solution.

## Resets and lifecycle

The sequence checks a reset followed by compatible recovery for explicit
camera cuts, changed history keys, frame gaps, display/data-window changes
and target resize. It also checks automatic resets for constant environment,
analytic light insertion/removal, material emission, unused texture insertion,
geometry deformation, light estimator, integrator, base sample index, bounce
limit, per-batch sample limit, history cap and clear/background colour.
Metadata-only selection followed by re-enabling radiance resets it;
successful ordinary rendering breaks the stream too.

Duplicate indices, zero/oversized history caps, diagnostic outputs, colour
preservation and zero-sample calls fail without consuming the retained
radiance or RNG stream. Empty scenes write zero counts and fresh background
colour. Initial empty rendering also exercises the radiance pass before any
TLAS descriptor exists. Every successful GPU call requires zero Vulkan
validation messages, including synchronization validation.

## GPU time, memory and shader layout

The full core run measured the stationary accumulation draw at 0.030720 ms
for reference transport and 0.031744 ms for wavefront transport. These are
single timestamp samples from correctness execution, not performance
comparisons. The pair excludes transport, fresh-batch copy and readback.
All three temporal pipelines and the six-query pool are created at renderer
initialization.

At 64×48 the extra radiance storage is 442,368 bytes: two 32-byte-per-pixel
device colour/count buffers, their two-buffer host mirror and a 16-byte
fresh batch buffer, totalling 144 bytes per pixel. Existing surface storage
is 786,432 bytes and validity storage is 98,304 bytes. The one-instance
metadata block is now 368 bytes (272 constants + 96 instance). These counts
exclude evidence vectors, memory alignment and ordinary transport targets.
Buffers persist at a fixed extent and resize replaces them.

An initial shader record used a `uint3` reserved tail. SPIR-V std430 aligned
it to 16 bytes and emitted stride 48 instead of the host's expected 32;
the initial exploratory runs failed. The final record uses three scalar
uints, matching the existing surface-record pattern, and emits stride 32.
The independent full-frame RGBA/count readback checks exercise this layout.

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
ctest --test-dir build/cy2026-windows-x86_64-py313-lookdev--hydra -R 'temporal|core-boundary' --output-on-failure
ost test --filter 'install-tree'
ost validate --strict-renderer-evidence
ost test --profile lookdev --intent hydra --filter 'install-tree'
ost validate --profile lookdev --intent hydra --strict-renderer-evidence
python scripts/check_docs.py
python -m unittest discover -s scripts -p 'test_*.py'
```

The physical-GPU core suite passed 23/23 and Hydra passed 46/46, including
the usdview host smoke and existing material/light/integrator regressions.
Runtime-free checks passed 13 with one explained temporal-GPU SKIP out of
14. Final targeted core and Hydra checks passed 5/5 each after adding the
initial empty radiance case. Documentation checks and all 27 Python
documentation/release regression tests passed. Logs use the
`build/temporal04-` prefix: `core-build.log`, `targeted-test.log`,
`core-test.log`, `runtime-free-build.log`, `runtime-free-test.log`,
`hydra-build.log` and `hydra-test.log`.

Final incremental build/targeted logs are `core-final-build.log`,
`hydra-final-build.log` and `hydra-final-test.log`. Install-tree and strict
validation logs are `core-install-test.log`, `hydra-install-test.log`,
`core-validate.log` and `hydra-validate.log`. An initial strict Hydra
validation after a test-only incremental build rejected the old report's
managed producer binding; rebuilding the backend and running the install-tree
checks through `ost test` refreshed the completion evidence.
Core and Hydra install-tree checks and strict renderer-evidence validation
then both passed; core's Hydra assertions remain explained SKIPs.

This slice implements LOTUS-TEMPORAL-04. Hydra temporal controls, hosted
moving-scene/AOV coverage and the temporal exit review remain the next
execution item. No release or package was prepared.
