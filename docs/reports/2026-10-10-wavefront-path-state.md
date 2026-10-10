# Wavefront path state by section

Measured on 2026-10-10 for LOTUS-WAVE-02, in the working tree based on
`970646e`, on the machine and toolchain of the
[scheduling report](2026-10-10-wavefront-scheduling.md): Windows 11 x86_64
(AMD Ryzen 9 7950X), MSVC 14.51, OpenStrata 0.23.14, Release builds, Vulkan
headers from SDK 1.3.290 and `slangc` 2026.8 from SDK 1.4.350, an NVIDIA
RTX A5000 (device API 1.4.329, driver 597.16) with Vulkan validation and
synchronization validation enabled, and OpenUSD 26.08 lookdev with CPython
3.13 for Hydra. All runs were local; hosted CI was not measured.

## Scope

The [scheduling measurements](2026-10-10-wavefront-scheduling.md#larger-targets)
left the wavefront integrator 3.4 times slower than the reference
integrator on `cornell-v1` at 1024×1024, and pointed at the rounds' work
per path: every intersect and shade invocation loaded and stored a 112-byte
path record. This step changes how path state is stored, within the
[phase contract](../design/ROADMAP_POLICY.md#renderer-phase-2--wavefront-path-tracing);
the transport, queues and schedule are unchanged, and the
[scene reference](../reference/SCENE.md#wavefront-integrator) owns the
resulting behavior. Path classification is not part of this step.

1. **Path state is stored by section.** A slot's state is five 16-byte
   entries, each in its own array over the slots: ray origin with TMin,
   ray direction with TMax, throughput with the last sampling density,
   radiance with the random-number state, and the hit (instance,
   primitive, barycentrics). A slot's pixel index is the slot, and a path
   queued at bounce *b* has depth *b*, so neither is stored; the hit
   distance, which shading does not read, and the path flags, which no
   transport step sets yet, are not stored either. The record was 112
   bytes; the sections are 80.
2. **Each kernel touches only its sections.** Intersect reads the ray and
   the radiance entry (for the random-number state) and writes the hit;
   it writes the radiance entry back only when a coverage test drew a
   random number or a continuation ray missed. Shade writes every section
   but the hit for a path that continues and only the radiance for one
   that ends, which is all accumulate reads. Generate no longer writes a
   hit. Per path, a round moved 448 bytes of path record before and moves
   64–144 now.

`Spectrum` values enter and leave storage through `PackSpectrum` and
`UnpackSpectrum`, which replace `StoredSpectrum`; entries are words, so no
storage struct has a `float3` member (the
[`slangc` stride finding](2026-10-10-wavefront-equivalence.md)).

Two alternatives were measured and rejected (GPU medians of three
invocations; images identical to the "before" images):

| Variant | `cornell-v1` 128² ms | `geometry-v1` 128² ms | `cornell-v1` 1024² ms | Shade 1024² ms |
| --- | ---: | ---: | ---: | ---: |
| Six sections: the random-number state and flags in their own section | 0.18691 | 0.24850 | 2.78701 | 1.1446 |
| Six sections, and generate numbering pixels in 8×8 tiles rather than rows | 0.18587 | 0.24742 | 2.79986 | 1.1639 |
| **Five sections (final)** | 0.18474 | 0.24480 | 2.56923 | 0.9522 |

The sixth section cost shade a load and a store per path; tile order made
no difference beyond noise, so the camera rays' order was left as it was.

## Checks

**`renderer.path.wavefront`** gains a scenario. A fractional coverage test
draws its random number in the intersect kernel and the path's next draws
happen in the shade kernel, so the random-number state must survive the
hand-over; none of the existing scenarios depended on it, since their
coverage tests either end the path or average over draws whose order does
not change the mean. In the new scenario a white Lambertian layer of 25%
coverage, in front of an opaque black square, is lit only by a 3×3 emitter
0.5 behind the camera's near plane, which camera rays cannot reach. Both
integrators render the same 32 samples per pixel at 64×64, and the
wavefront mean must be within 5 standard errors of the reference
integrator's. Were the coverage draw reused as the next BSDF sample, paths
that pass the layer would sample only directions within 30° of its normal
(cosine sampling takes the radius from that number), most of which reach
the emitter.

Its default-build result:

- Every scenario passes. The new one measured a mean of 0.0704 against
  the reference integrator's 0.0704 ± 0.0036 in each channel over 131,072
  samples.
- The reference-image comparison has the same z-scores as before (image z
  at 1024 spp: 2.49, 2.14, 2.13; maximum tile |z| 2.99), and the brighter
  red wall is rejected at max |z| 6.32.
- The reference's own samples reproduce the committed reference **bit for
  bit**, and the same 64 samples from both integrators are **bit for bit
  identical**.
- The occupancy sample traced the same 59,654 rays in 17 bounces as
  before, 6 of them as rounds. Generate, intersect, shade, tail and
  accumulate took 0.007, 0.058, 0.043, 0.034 and 0.003 ms of a 0.166 ms
  scene pass, which took 0.217 ms before.

**`renderer.path.reference`** is unchanged and still reproduces the
committed reference bit for bit. The references were not regenerated.

Three fault injections each changed one line of `wavefront.slang`; the
default build's report then failed `renderer.path.wavefront` while
`renderer.path.reference` passed:

| Fault | Detail of the failing check |
| --- | --- |
| Shade does not store the radiance of a path that ends | `bsdf: emission without bounces: radiance 0.025000 instead of 0.075000 in channel 0 at pixel 931` |
| Intersect does not store the random-number state a coverage test advanced | `mean 0.201462 instead of 0.070450 +- 0.005988 in channel 0` |
| Shade gives a path the depth of the next bounce | `opacity: secondary cut-out: radiance 0.000000 instead of 2.000000 in channel 2 at pixel 0` |

Before the new scenario was added, the second fault passed every check.
The restored source rebuilt and passed.

## Performance

The [fixed benchmarks](../reference/SCENE.md#fixed-benchmarks) were run
three times per configuration, sequentially, at the default 128×128 with
64 measured frames after 8 warmup frames:

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/wave02b/final-1.json --benchmark-label "wave02 path state" --benchmark-integrator wavefront
```

"Before" and "Reference" are a build of `970646e`, run in this session with
`--benchmark-integrator wavefront` and `reference`; "before" is within 0.4%
of the scheduling report's final medians. Every benchmark run of this
session (40 reports, 160 scene measurements) passed with zero validation
messages. Every wavefront image of the final build, at 128×128 and at
1024×1024, was **identical to the "before" images**, and at 128×128 also
to the reference integrator's.

Medians of the three invocations' GPU medians, minimum–maximum for the
final build:

| Scene | Reference ms | Before ms | Final ms | Speedup | Final / reference |
| --- | ---: | ---: | ---: | ---: | ---: |
| `cornell-v1` | 0.06496 | 0.23974 | 0.18474 (0.18429–0.18494) | 1.30× | 2.8× |
| `textured-v1` | 0.04376 | 0.16107 | 0.13269 (0.13230–0.13282) | 1.21× | 3.0× |
| `instanced-v1` | 0.03416 | 0.13966 | 0.11158 (0.11125–0.11158) | 1.25× | 3.3× |
| `geometry-v1` | 0.09136 | 0.29019 | 0.24480 (0.24451–0.24576) | 1.19× | 2.7× |

Per-kernel medians, before → final:

| Scene | Rounds | Generate ms | Intersect ms | Shade ms | Tail ms | Accumulate ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `cornell-v1` | 6 | 0.0135 → 0.0078 | 0.0828 → 0.0637 | 0.0746 → 0.0470 | 0.0423 → 0.0397 | 0.0033 → 0.0030 |
| `textured-v1` | 3 | 0.0139 → 0.0081 | 0.0535 → 0.0421 | 0.0419 → 0.0313 | 0.0217 → 0.0212 | 0.0034 → 0.0033 |
| `instanced-v1` | 3 | 0.0135 → 0.0078 | 0.0498 → 0.0396 | 0.0344 → 0.0227 | 0.0142 → 0.0132 | 0.0030 → 0.0028 |
| `geometry-v1` | 4 | 0.0139 → 0.0081 | 0.1401 → 0.1252 | 0.0635 → 0.0410 | 0.0300 → 0.0294 | 0.0038 → 0.0034 |

The tail, which keeps its path in registers between bounces, gains least.

### Larger targets

`cornell-v1` at 1024×1024 (`--benchmark-size 1024`), 16 measured and 4
warmup frames; three invocations before and of the reference integrator,
five of the final build:

| | Reference | Before | Final |
| --- | ---: | ---: | ---: |
| GPU frame ms | 1.51909 (1.51546–1.52011) | 5.20262 (5.19651–5.20931) | 2.56923 (2.56485–2.57133) |
| Generate ms | | 0.4053 | 0.1063 |
| Intersect ms | | 2.0945 | 0.9392 |
| Shade ms | | 2.0752 | 0.9522 |
| Tail ms | | 0.1069 | 0.0880 |
| Accumulate ms | | 0.2192 | 0.1851 |

The final build is 2.0 times faster than before and 1.7 times slower than
the reference integrator, which traces the frame's 3.80 million rays and
shades their hits in 1.52 ms. Timed separately, in a build instrumented to
print each round of a first frame (which records every bounce as a
round), the rounds over the first four bounces' queues took 0.15–0.21 ms
to intersect and 0.16–0.23 ms to shade each, so a ray and its shading now
cost about what they cost in the reference integrator. The difference
that remains is in the work the reference integrator does not have:
generating and accumulating paths through memory, and the barrier between
each pair of kernels.

### Power states

The [scheduling report](2026-10-10-wavefront-scheduling.md#larger-targets)
found the non-Cornell scenes' 1024×1024 timings bimodal and took it as the
desktop's use of the GPU. It is the GPU's power state. At that size a
measured frame's blocking call takes about 100 ms, mostly readback and
validation on the CPU, and the GPU is busy for a few milliseconds of it;
the driver lowers the GPU's performance state accordingly. `nvidia-smi`
sampled every 100 ms during the five final 1024×1024 invocations found
P0 (memory clock 8001 MHz) in 115 samples, P5 (810 MHz) in 252 and P8
(405 MHz) in 156. The generate kernel, whose work does not depend on the
scene, took 0.106 ms in `cornell-v1`, which runs first, and 1.43 or
3.5 ms in later scenes, in step with those clocks. The same kernel at
128×128 shows no such split. Only `cornell-v1`'s 1024×1024 rows are
therefore compared above; the benchmark report does not record the power
state.

## Builds and tests

| Configuration | Result |
| --- | --- |
| `ost build --jobs auto`, `ost test`, `ost validate --strict-renderer-evidence` | built; 18 of 18 tests passed, including the install tree and `lotus-renderer-benchmark-wavefront`; validation passed |
| `ost build --without-runtime --intent ci-core --jobs auto`, `ost test --without-runtime --intent ci-core` | built; 10 of 10 passed; `renderer.path.wavefront` is an explained SKIP |
| `ost build --profile lookdev --intent hydra --jobs auto`, `ost test --profile lookdev --intent hydra`, `ost validate --profile lookdev --intent hydra --strict-renderer-evidence` | built; 35 of 35 passed, including usdview; validation passed |
