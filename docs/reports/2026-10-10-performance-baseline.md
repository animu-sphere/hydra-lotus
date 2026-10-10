# Fixed performance baseline

Measured on 2026-10-10 for LOTUS-PERF-01, based on `ae72568` plus the
benchmark implementation. The [raw three-run data](2026-10-10-performance-baseline.json)
retains every sample, build/device identity, source SHA-256 fingerprints and
final-image hashes. Workload and metric behavior is owned by the
[benchmark reference](../reference/SCENE.md#fixed-benchmarks).

## Hardware and build

- Windows 11 x86_64, AMD Ryzen 9 7950X (16 cores / 32 logical processors).
- NVIDIA RTX A5000, 24,564 MiB reported device memory, NVIDIA driver 597.16,
  Vulkan API 1.4.329. Device capacity is not renderer VRAM usage.
- OpenStrata 0.23.14, `core` Release build, MSVC 19.51.36256.0
  (toolset 14.51). CMake used Vulkan SDK 1.3.290 headers and the
  SDK 1.4.350 `slangc` executable.
- Vulkan validation and synchronization validation enabled, zero captured
  messages in every measured invocation. GPU clocks/power were not pinned;
  the observed range is retained instead of assuming invariant clocks.

## Procedure and repeatability

The following commands were run sequentially on the same executable:

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/performance-baseline-1.json --benchmark-label "ae72568 + LOTUS-PERF-01; ost core Release"
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/performance-baseline-2.json --benchmark-label "ae72568 + LOTUS-PERF-01; ost core Release"
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/performance-baseline-3.json --benchmark-label "ae72568 + LOTUS-PERF-01; ost core Release"
```

Each invocation created a fresh renderer per scene. Every scene rendered at
128×128, with 64 maximum bounces, 8 discarded warmup samples starting at
sample index 1, then an accumulation reset and 64 measured samples from
sample index 0. Each `RenderScene` call rendered exactly one sample, so its
GPU timestamp represented that sample rather than only the final submission
of a batch. Images and scene update plans were generated procedurally;
there were no external scene/texture inputs or licensing dependencies.

All twelve scene measurements passed. Each scene's 64-spp PFM was bit for
bit identical across the three invocations. Cornell also passed the
committed mean/variance reference's statistical tolerance; references were
not regenerated. Other scenes checked finite, visible, nonblack output and
the expected workload counts. These checks establish a repeatable workload,
not additional production-scene coverage.

## Steady rendering

Time columns are the median of each invocation's median. Parentheses show
the minimum–maximum of those three medians. Throughput is the median of
the three full-frame pixel-sample rates (total samples / total time), not
the reciprocal of a median duration. Wall time includes GPU waits,
readback and copying products into host vectors.

| Scene | GPU pass ms (range) | Render wall ms (range) | GPU Mpixel samples/s | Wall Mpixel samples/s | Unchanged CPU µs/call |
| --- | ---: | ---: | ---: | ---: | ---: |
| `cornell-v1` | 0.06571 (0.05758–0.06574) | 1.4129 (1.3834–1.5618) | 248.50 | 11.41 | 0.447 |
| `textured-v1` | 0.04410 (0.03902–0.04442) | 1.3764 (1.3710–1.3968) | 366.73 | 11.73 | 0.557 |
| `instanced-v1` | 0.03450 (0.03054–0.03467) | 1.3795 (1.3594–1.3863) | 466.94 | 11.71 | 0.426 |
| `geometry-v1` | 0.09117 (0.09075–0.09173) | 1.4423 (1.4324–1.4644) | 173.42 | 11.19 | 0.441 |

The blocking path's wall time is much larger than its GPU pass time for
these small targets. This baseline therefore records both quantities;
GPU-only throughput does not describe interactive/offscreen throughput.
The textured and geometry-heavy scenes represent data size and traversal
work, rather than complex multi-bounce production lighting.

Unchanged CPU measurements used 16 batches of 1,000 `Commit` → extraction →
empty backend-update calls per scene. Timing includes invariant checks;
GPU scene allocation, upload and acceleration counters did not change
during those calls or steady rendering.

## Upload and acceleration updates

Each invocation made one initial upload/build, one point edit and one
transform edit per scene, outside steady-frame timing. Tables show the
median of three single observations, in milliseconds. Backend-update CPU
wall time includes its waits; CPU scene construction and extraction are
excluded. GPU phases are timed independently. This is a baseline of three
observations per update kind, not a tail-latency characterization.

### Initial upload/build

| Scene | Backend update wall | GPU upload | GPU BLAS build | GPU TLAS build |
| --- | ---: | ---: | ---: | ---: |
| `cornell-v1` | 4.422300 | 0.002304 | 0.087744 | 0.047840 |
| `textured-v1` | 5.681400 | 0.210848 | 0.270368 | 0.050048 |
| `instanced-v1` | 4.113400 | 0.040256 | 0.071200 | 0.159200 |
| `geometry-v1` | 8.241100 | 0.359680 | 1.627744 | 0.033152 |

### Compatible point edit

Every position in one geometry was edited by `z += 0.01 * x`. BLAS update
and TLAS update counters each rose by one; BLAS build counters stayed fixed.

| Scene | Backend update wall | GPU upload | GPU BLAS refit | GPU TLAS bounds refit |
| --- | ---: | ---: | ---: | ---: |
| `cornell-v1` | 0.356400 | 0.001472 | 0.024896 | 0.012640 |
| `textured-v1` | 0.512300 | 0.005408 | 0.080544 | 0.012032 |
| `instanced-v1` | 0.593100 | 0.035104 | 0.024832 | 0.016480 |
| `geometry-v1` | 2.645100 | 0.359168 | 0.455456 | 0.010080 |

### Transform edit

The same mesh's root placement moved by `x += 0.01`. TLAS updates rose by
one; neither BLAS builds nor BLAS updates changed. The instanced workload
updates all 4,096 placements of its prototype.

| Scene | Backend update wall | GPU upload | GPU BLAS (skipped) | GPU TLAS refit |
| --- | ---: | ---: | ---: | ---: |
| `cornell-v1` | 0.163800 | 0.000832 | 0.000000 | 0.010496 |
| `textured-v1` | 0.222600 | 0.001024 | 0.000000 | 0.012224 |
| `instanced-v1` | 0.406200 | 0.030528 | 0.000000 | 0.016864 |
| `geometry-v1` | 0.182600 | 0.001120 | 0.000000 | 0.010176 |

## Memory and unavailable metrics

Values below were identical across all three invocations, after warmup and
steady rendering and before the edit experiments. These are scene-pool
bytes, including host-visible staging and scratch, not total VRAM.

| Scene | Pool reserved bytes | Pool occupied bytes |
| --- | ---: | ---: |
| `cornell-v1` | 20,971,520 | 56,720 |
| `textured-v1` | 39,149,600 | 13,495,488 |
| `instanced-v1` | 20,971,520 | 3,191,824 |
| `geometry-v1` | 59,387,360 | 54,292,976 |

The JSON explicitly records these requested but unavailable metrics as
null, with individual explanations:

| Metric | Availability |
| --- | --- |
| Rays/s | No reference-shader ray counter. Pixel samples/s is recorded separately. |
| Average path depth | No shader path-depth counter. |
| Total VRAM | Scene pools exclude offscreen attachments, accumulation, readback targets and constants, and include host-visible resources. |
| CPU render-submit time | The blocking API does not separate submission from GPU wait/readback; render wall time is measured separately. |

## Validation

- `ost build --jobs auto`, `ost test`: **17/17 passed**, including the
  16-frame/2-warmup benchmark contract, invalid argument rejection and
  isolated missing-driver benchmark regression.
- `ost validate --strict-renderer-evidence`: passed.
- `ost build --profile lookdev --intent hydra --jobs auto`,
  `ost test --profile lookdev --intent hydra`: **34/34 passed**, including
  Hydra discovery and usdview first-frame/stable-update evidence.
- `ost validate --profile lookdev --intent hydra --strict-renderer-evidence`:
  passed.
- `ost build --without-runtime --intent ci-core --jobs auto`,
  `ost test --without-runtime --intent ci-core`: **9/9 passed**. The
  benchmark produced four explained Vulkan-disabled SKIPs with no numeric
  measurements, while its schema/CLI regression passed.
- `python scripts/check_docs.py` and
  `python -m unittest discover -s scripts -p test_check_docs.py`: passed.

The missing-driver child returned 77 with `vkCreateInstance` / VkResult -9
for all four workloads; malformed counts and mixed correctness/benchmark
options returned 2 without overwriting their sentinel output. Benchmark
measurements remain separate from the renderer correctness report and
cannot supply substitute PASS evidence to OpenStrata validation.
