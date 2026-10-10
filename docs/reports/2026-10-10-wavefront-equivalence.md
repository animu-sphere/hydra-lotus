# Wavefront reference equivalence

Measured on 2026-10-10 for LOTUS-WAVE-01, in the working tree based on
`d6a6368`, on Windows 11 x86_64 (AMD Ryzen 9 7950X) with MSVC 14.51,
OpenStrata 0.23.14 and Release builds. Vulkan headers came from SDK 1.3.290
and `slangc` 2026.8 from SDK 1.4.350. The GPU was an NVIDIA RTX A5000
(device API 1.4.329), with Vulkan validation and synchronization validation
enabled. Hydra used OpenUSD 26.08 lookdev and CPython 3.13. All runs were
local; hosted CI was not measured.

## Scope

This report covers the first step of Renderer Phase 2: the wavefront
integrator's initial queues and transport, and deterministic equivalence
with the reference path tracer before any queue or scheduling optimization
([phase contract](../design/ROADMAP_POLICY.md#renderer-phase-2--wavefront-path-tracing)).
The [scene reference](../reference/SCENE.md#wavefront-integrator) owns the
integrator's behavior and limits.

Design decisions made for this step:

- **One transport, two schedules.** The reference transport moved into
  shared Slang modules: `common/scene.slang` (records, descriptors,
  traversal and hit reconstruction) and `common/transport.slang` (start a
  camera path, one scattering event, a miss). The fragment-pass reference
  integrator loops over them; the wavefront kernels call each step once
  per queued path. A path keeps its random-number state in its slot, so it
  draws its numbers in the same order under either schedule.
- **Selected per frame.** `PathTracingSettings::integrator` chooses the
  integrator; changing it restarts the accumulation. The Hydra adapter
  keeps the reference integrator.
- **Queues before optimization.** Ray, hit and terminated-path queues hold
  slot indices appended with atomic counters, and single-invocation kernels
  write the next indirect dispatch. Every frame records `max_bounces + 1`
  rounds, so no host readback decides when paths are finished; empty rounds
  dispatch no groups. Compaction beyond the append queues, scheduling,
  occupancy and path classification are not attempted.
- **The resolve pass stays a fragment pass.** The kernels add to the
  existing accumulation image, and a resolve specialization of the radiance
  pass writes the mean, so clears, the data and display windows,
  background coverage and `max_samples` behave as before.

## A compiler finding

The first implementation failed `renderer.path.bsdf` under the wavefront
integrator: some Lambert pixels carried two bounces of emission off a lone
triangle. Queue counts showed rays still being traced at the third bounce,
which a lone triangle cannot produce. Recording the first such ray showed
that its origin was off the triangle's plane: hit reconstruction in the
shade kernel read wrong vertex positions, from a correct hit.

`slangc` 2026.8 had emitted the scene's `float3*` position pointer with an
`ArrayStride` of 16 in the compute module (12 in the fragment module). The
compute module also declared a storage-buffer struct with `float3` members,
the path record, whose std430 stride of 16 was applied to the module's
physical-storage `float3` pointer type too. Without a `float3` in any
storage-buffer struct, the stride is 12 again: the path record now stores
its ray as `float4`s and its spectra as `StoredSpectrum`, a `float4` form in
`common/spectrum.slang` whose comment records the constraint. Full pipeline
barriers did not change the result, which ruled out synchronization before
the SPIR-V was inspected.

## Checks

**`renderer.path.wavefront`** is new; the
[scene reference](../reference/SCENE.md#wavefront-integrator) lists what it
covers. Its default-build result:

- The `bsdf`, `normals`, `textures`, `normal_maps`, `opacity`,
  `multibounce` and `accumulation` scenarios pass with the wavefront
  integrator. They include exact single-scattering radiance, partial pixel
  coverage, data and display windows, split frames, restarts and
  `max_samples`.
- The reference images at 1–1024 spp from sample index 2²⁰ match the
  committed reference with the same z-scores as the reference integrator
  (image z at 1024 spp: 2.49, 2.14, 2.13; maximum tile |z| 2.99). The
  brighter red wall is rejected at max |z| 6.32.
- Rendering the reference's own samples 0–1023 with the wavefront
  integrator **reproduces the committed reference bit for bit**; that
  reference was written by the reference integrator.
- The same 64 samples from sample index 2²⁰ are bit for bit identical from
  both integrators.
- One more sample traced 59,654 rays in 17 bounces, 3.641 rays per camera
  path; every one of the 16,384 camera rays was queued and no bounce queued
  more paths than the one before. Generate, intersect, shade and accumulate
  took 0.015, 0.288, 0.748 and 0.003 ms of the 1.079 ms scene pass.
- Switching integrators restarted the accumulation in both directions;
  `max_bounces` 1025 failed the frame with an explanation, and 1024 then
  rendered.

**`renderer.path.reference`** is unchanged and still reproduces the
committed reference bit for bit after the module split. The references were
not regenerated.

Two fault injections each changed one line of `wavefront.slang`; the
default build's report then failed `renderer.path.wavefront` while
`renderer.path.reference` passed:

| Fault | Detail of the failing check |
| --- | --- |
| A continuation ray that misses ends without the environment | `bsdf: Lambert front face: radiance 0.075000 instead of 0.275000` |
| Shade queues continuations into the ray queue being drained | `bsdf: Lambert front face: no pixel's samples all hit` |

The restored source rebuilt and passed.

## Performance

The [fixed benchmarks](../reference/SCENE.md#fixed-benchmarks) were run three
times per integrator on the same executable, sequentially:

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/wavefront-reference-1.json --benchmark-label "d6a6368 + LOTUS-WAVE-01; ost core Release"
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/wavefront-baseline-1.json --benchmark-label "d6a6368 + LOTUS-WAVE-01; ost core Release" --benchmark-integrator wavefront
```

with the output suffixes `2` and `3` for the other runs. All 24 scene
measurements passed with zero validation messages. Every scene's 64-spp
image was identical across the three runs of each integrator, **identical
between the two integrators**, and identical to the
[fixed baseline's](2026-10-10-performance-baseline.md) images of `ae72568`.

Medians of the three invocations' medians, minimum–maximum in parentheses;
throughput is the median of the three full-frame rates:

| Scene | Reference GPU ms | Wavefront GPU ms | Reference wall ms | Wavefront wall ms | Rays per camera path | Wavefront Grays/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `cornell-v1` | 0.06496 (0.06493–0.06517) | 1.16686 (1.16602–1.17437) | 1.4325 | 3.6894 | 3.628 | 0.050 |
| `textured-v1` | 0.04408 (0.04371–0.04419) | 1.05158 (1.05093–1.05182) | 1.3802 | 3.5487 | 2.214 | 0.034 |
| `instanced-v1` | 0.03424 (0.03403–0.03443) | 1.00605 (1.00546–1.00766) | 1.3620 | 3.5209 | 2.177 | 0.035 |
| `geometry-v1` | 0.09090 (0.09078–0.09166) | 1.17136 (1.17086–1.17160) | 1.4396 | 3.6259 | 3.137 | 0.043 |

The reference integrator's GPU medians are within the fixed baseline's
ranges after the module split. Per-kernel medians of the wavefront runs:

| Scene | Generate ms | Intersect ms | Shade ms | Accumulate ms |
| --- | ---: | ---: | ---: | ---: |
| `cornell-v1` | 0.0166 | 0.3194 | 0.8028 | 0.0031 |
| `textured-v1` | 0.0166 | 0.2494 | 0.7566 | 0.0033 |
| `instanced-v1` | 0.0166 | 0.2385 | 0.7234 | 0.0028 |
| `geometry-v1` | 0.0171 | 0.3426 | 0.7680 | 0.0037 |

At 128×128 the wavefront integrator is 13–29 times slower on the GPU. The
work is not the cause: every frame records 65 rounds, each with intersect,
shade and two bookkeeping dispatches separated by four barriers, and most
rounds have empty queues — the check's Cornell sample traced rays in only
17 of them. Shade's column includes both bookkeeping dispatches. The wall time also includes
recording the longer command buffer, which happens once per call. These
are the costs the queue and scheduling optimization starts from; they are
not a judgement of wavefront scheduling at production resolutions, which
were not measured.

## Builds and tests

| Configuration | Result |
| --- | --- |
| `ost build --jobs auto`, `ost test`, `ost validate --strict-renderer-evidence` | built; 18 of 18 tests passed, including the install tree, whose installed `lotus-headless` passed `renderer.path.wavefront`; validation passed |
| `ost build --without-runtime --intent ci-core --jobs auto`, `ost test --without-runtime --intent ci-core` | built; 10 of 10 passed; `renderer.path.wavefront` is an explained SKIP |
| `ost build --profile lookdev --intent hydra --jobs auto`, `ost test --profile lookdev --intent hydra`, `ost validate --profile lookdev --intent hydra --strict-renderer-evidence` | built; 35 of 35 passed, including usdview; validation passed |

CTest `lotus-renderer-benchmark-wavefront` checks the wavefront benchmark's
report contract, and the benchmark rejects an unknown integrator.
