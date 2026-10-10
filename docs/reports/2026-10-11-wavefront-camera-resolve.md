# Wavefront camera kernel and resolve-pass samples

Measured on 2026-10-10 and 2026-10-11 for LOTUS-WAVE-02, in the working
tree based on `d1f2474`, on the machine and toolchain of the
[path-state report](2026-10-10-wavefront-path-state.md): Windows 11 x86_64
(AMD Ryzen 9 7950X), MSVC 14.51, OpenStrata 0.23.14, Release builds, Vulkan
headers from SDK 1.3.290 and `slangc` 2026.8 from SDK 1.4.350, an NVIDIA
RTX A5000 (device API 1.4.329, driver 597.16) with Vulkan validation and
synchronization validation enabled, and OpenUSD 26.08 lookdev with CPython
3.13 for Hydra. All runs were local; hosted CI was not measured.

## Scope

The [path-state measurements](2026-10-10-wavefront-path-state.md#larger-targets)
left the wavefront integrator 1.7 times slower than the reference
integrator on `cornell-v1` at 1024×1024, with a ray and its shading
costing about the same in both, and placed the difference in work the
reference integrator does not have: generating and accumulating paths
through memory, and the barriers between kernels. This step removes the
generate and accumulate kernels, within the
[phase contract](../design/ROADMAP_POLICY.md#renderer-phase-2--wavefront-path-tracing);
the transport and the round schedule are unchanged, and the
[scene reference](../reference/SCENE.md#wavefront-integrator) owns the
resulting behavior.

1. **The camera kernel traces the camera rays.** The generate kernel
   stored each camera path's ray, throughput and radiance and queued the
   path, and the first round's intersect kernel read them back. The camera
   kernel starts the path and traces its camera ray in the same
   invocation, storing only what shading reads (the ray direction, the
   radiance entry with the random-number state, and the hit) for a hit
   and nothing but an empty sample for a miss. Shading at bounce 0 takes
   the throughput and last density from the path's start rather than
   loading them. Bounce 0 has no ray queue; its count is still the camera
   rays.
2. **The resolve pass adds the samples.** The accumulate kernel read the
   terminated-path queue and added each path's radiance to its pixel of
   the accumulation image, in queue order. Now an ending path's radiance
   entry becomes its sample in the accumulation's form, the radiance and
   a weight of 1, and a camera ray that misses leaves zero; the resolve
   pass, which already read every pixel's accumulation, adds the slot's
   sample in pixel order and writes the accumulation back, as the
   reference integrator's radiance pass does with the sample it traces.
   The terminated-path queue, its appends and the accumulate kernel's
   dispatch and barrier are gone, and the path buffer becomes visible to
   the fragment stage (partially bound, since it exists only after the
   first wavefront frame).

The path-state layout moves to `common/path_state.slang`, which the
wavefront kernels and the resolve pass share. `WavefrontTimings` reports
`camera_gpu_ms` (camera paths and their rays) in place of
`generate_gpu_ms`, `intersect_gpu_ms` covers the rounds after the first,
and `accumulate_gpu_ms` is gone; the benchmark report's `wavefront`
object changes its keys accordingly.

## Checks

**`renderer.path.wavefront`** is unchanged and passes:

- The coverage scenario measured a mean of 0.0704 against the reference
  integrator's 0.0704 ± 0.0036 in each channel over 131,072 samples.
- The reference-image comparison has the same z-scores as before (image z
  at 1024 spp: 2.49, 2.14, 2.13; maximum tile |z| 2.99), and the brighter
  red wall is rejected at max |z| 6.32.
- The reference's own samples reproduce the committed reference **bit for
  bit**, and the same 64 samples from both integrators are **bit for bit
  identical**.
- The occupancy sample traced the same 59,654 rays in 17 bounces, 6 of
  them as rounds. Camera, intersect, shade and tail took 0.012, 0.045,
  0.043 and 0.031 ms of a 0.152 ms scene pass.

**`renderer.path.reference`** is unchanged and still reproduces the
committed reference bit for bit. The references were not regenerated.

Three fault injections each changed one line of `wavefront.slang`; the
default build's report then failed `renderer.path.wavefront` while
`renderer.path.reference` passed:

| Fault | Detail of the failing check |
| --- | --- |
| The camera kernel leaves a missed slot as it was | `bsdf: emission without bounces: radiance 0.200000 instead of 0.100000 in channel 0 at pixel 996` |
| Shade stores an ending path's radiance with its random-number state rather than its sample | `bsdf: emission without bounces: alpha 105645045746499584.000000 is not a fraction of 4 samples at pixel 931` |
| Shade loads a camera path's throughput, which the camera kernel did not store | `bsdf: Lambert front face: radiance 0.025000 instead of 0.275000 in channel 0 at pixel 931` |

The restored source rebuilt to the same SPIR-V and passed.

## Performance

The [fixed benchmarks](../reference/SCENE.md#fixed-benchmarks) were run
three times per configuration, sequentially, at the default 128×128 with
64 measured frames after 8 warmup frames:

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/wave02c/f2-1.json --benchmark-integrator wavefront
```

"Before" is a build of `d1f2474`, "camera kernel" the first change
alone, and "reference" the final build with `--benchmark-integrator
reference`; "before" is within 0.3% of the path-state report's final
medians. Every benchmark run of this session (32 reports, 128 scene
measurements) passed with zero validation messages. Every wavefront image
of both changed builds, at 128×128 and at 1024×1024, was **identical to
the "before" images** and to the reference integrator's.

Medians of the three invocations' GPU medians, minimum–maximum for the
final build:

| Scene | Reference ms | Before ms | Camera kernel ms | Final ms | Speedup | Final / reference |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `cornell-v1` | 0.06538 | 0.18480 | 0.17824 | 0.16912 (0.16906–0.16918) | 1.09× | 2.6× |
| `textured-v1` | 0.04381 | 0.13285 | 0.12656 | 0.11810 (0.11790–0.11880) | 1.12× | 2.7× |
| `instanced-v1` | 0.03427 | 0.11157 | 0.10578 | 0.09763 (0.09739–0.09805) | 1.14× | 2.8× |
| `geometry-v1` | 0.09174 | 0.24531 | 0.23570 | 0.22637 (0.22608–0.22640) | 1.08× | 2.5× |

Per-kernel medians, before → final. Before, the first column is the
generate kernel and intersect includes the camera rays; after, the first
column is the camera kernel, which traces them:

| Scene | Rounds | Generate → camera ms | Intersect ms | Shade ms | Tail ms | Accumulate ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `cornell-v1` | 6 | 0.0078 → 0.0128 | 0.0639 → 0.0502 | 0.0469 → 0.0460 | 0.0396 → 0.0367 | 0.0030 → — |
| `textured-v1` | 3 | 0.0080 → 0.0146 | 0.0420 → 0.0287 | 0.0314 → 0.0304 | 0.0213 → 0.0183 | 0.0032 → — |
| `instanced-v1` | 3 | 0.0078 → 0.0140 | 0.0397 → 0.0263 | 0.0228 → 0.0223 | 0.0133 → 0.0104 | 0.0028 → — |
| `geometry-v1` | 4 | 0.0081 → 0.0213 | 0.1253 → 0.1027 | 0.0410 → 0.0388 | 0.0296 → 0.0265 | 0.0034 → — |

At this size, removing the accumulate kernel saved two to three times its
own duration: its dispatch, its barrier and the terminated-queue appends
cost more than its work.

### Larger targets

`cornell-v1` at 1024×1024 (`--benchmark-size 1024`), 16 measured and 4
warmup frames; three invocations before, of the camera kernel alone and of
the reference integrator, five of the final build. Every run of
`cornell-v1`, which runs first, stayed in the
[power state](2026-10-10-wavefront-path-state.md#power-states) of the
path-state report's comparisons; the other scenes are not compared.

| | Reference | Before | Camera kernel | Final |
| --- | ---: | ---: | ---: | ---: |
| GPU frame ms | 1.51910 (1.51907–1.51910) | 2.56550 (2.56126–2.57456) | 2.34539 (2.34402–2.35285) | 2.17941 (2.17544–2.18026) |
| Generate → camera ms | | 0.1066 | 0.2376 | 0.2373 |
| Intersect ms | | 0.9388 | 0.6983 | 0.6967 |
| Shade ms | | 0.9525 | 0.8380 | 0.8388 |
| Tail ms | | 0.0873 | 0.0866 | 0.0840 |
| Accumulate ms | | 0.1860 | 0.1859 | — |

The final build is 1.18 times faster than before and 1.43 times slower
than the reference integrator. The camera kernel replaced 0.35 ms of
generation and camera-ray intersection with 0.24 ms, and shading fell by
0.11 ms, loading less of each camera path; the resolve pass's additions
cost about 0.02 ms of the 0.19 ms the accumulate kernel took.

## Path classification

Path classification would queue hits by the kind of shading they need,
so that each class's shade kernel runs without the others' code. Its
upper bound on the fixed workloads was measured first, with a build whose
hit reconstruction skipped the texture lookups and normal mapping
entirely: the shade kernel of a class that needs neither. The
measurement used the "before" build otherwise, and only the untextured
scenes, whose images it leaves unchanged, are comparable:

| Scene | Shade before ms | Shade without texture code ms | Frame before ms | Frame without ms |
| --- | ---: | ---: | ---: | ---: |
| `cornell-v1` | 0.0469 | 0.0464 | 0.18480 | 0.18331 |
| `instanced-v1` | 0.0228 | 0.0224 | 0.11157 | 0.11062 |
| `geometry-v1` | 0.0410 | 0.0406 | 0.24531 | 0.24395 |
| `cornell-v1`, 1024×1024 | 0.9525 | 0.9474 | 2.56550 | 2.56958 |

Shading code a hit does not execute costs it at most 1–2% of the shade
kernel, so a separate kernel for constant materials could save no more
than that, while each class adds a dispatch and a barrier to every round.
`textured-v1` (0.0314 → 0.0231 ms) shows the cost of the lookups
themselves, which every hit there needs. Each fixed workload's materials
are of one kind, so classification has nothing to separate in them; it
was not implemented.

## Builds and tests

| Configuration | Result |
| --- | --- |
| `ost build --jobs auto`, `ost test`, `ost validate --strict-renderer-evidence` | built; 18 of 18 tests passed, including the install tree and `lotus-renderer-benchmark-wavefront`; validation passed |
| `ost build --without-runtime --intent ci-core --jobs auto`, `ost test --without-runtime --intent ci-core` | built; 10 of 10 passed; `renderer.path.wavefront` is an explained SKIP |
| `ost build --profile lookdev --intent hydra --jobs auto`, `ost test --profile lookdev --intent hydra`, `ost validate --profile lookdev --intent hydra --strict-renderer-evidence` | built; 35 of 35 passed, including usdview; validation passed |
