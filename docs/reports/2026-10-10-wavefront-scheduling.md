# Wavefront queue bookkeeping and round scheduling

> Later: the bimodal 1024×1024 timings below are GPU power states, not desktop interference ([path-state report](2026-10-10-wavefront-path-state.md#power-states)).

Measured on 2026-10-10 for LOTUS-WAVE-02, in the working tree based on
`ec643a2`, on the machine and toolchain of the
[equivalence report](2026-10-10-wavefront-equivalence.md): Windows 11
x86_64 (AMD Ryzen 9 7950X), MSVC 14.51, OpenStrata 0.23.14, Release builds,
Vulkan headers from SDK 1.3.290 and `slangc` 2026.8 from SDK 1.4.350, an
NVIDIA RTX A5000 (device API 1.4.329, driver 597.16) with Vulkan validation
and synchronization validation enabled, and OpenUSD 26.08 lookdev with
CPython 3.13 for Hydra. All runs were local; hosted CI was not measured.
The GPU also drives the desktop, which matters for the larger-target
measurements below.

## Scope

The [equivalence measurements](2026-10-10-wavefront-equivalence.md#performance)
found the wavefront integrator 13–29 times slower than the reference
integrator at 128×128, with the cost in its per-bounce structure rather
than its work: every frame recorded 65 rounds of four dispatches and four
barriers, most of them over empty queues. This step removes that
structure's fixed costs within the
[phase contract](../design/ROADMAP_POLICY.md#renderer-phase-2--wavefront-path-tracing);
the transport, path state and queue layout are unchanged. The
[scene reference](../reference/SCENE.md#wavefront-integrator) owns the
resulting behavior.

1. **Appends maintain the indirect dispatch.** Each queue now has a
   descriptor: its length and its `VkDispatchIndirectCommand`. An append
   increments the length and, when its entry begins a new group, the
   dispatch's group count, so the single-invocation *prepare* and
   *advance* kernels and their barriers are gone. Each bounce has its own
   ray and hit descriptors, written once per frame with
   `vkCmdUpdateBuffer`, so nothing is reset between rounds. The intersect
   and shade kernels find their bounce by handing it to each other: each
   kernel's first invocation writes the word the other kernel reads, and
   neither writes a word it reads. A round whose queue is empty runs no
   invocation, and neither does any later round, so a bounce index that
   stops advancing is never read. A round is now two dispatches and two
   barriers.
2. **Rounds follow the previous sample's occupancy, and a tail finishes
   the rest.** A call records rounds for the bounces whose queue held at
   least 1/16 of the camera rays in its previous sample, at least one;
   then a *tail* kernel follows each path still queued to its end in one
   invocation, as the reference integrator does. The path's transport and
   random-number order do not depend on where it finishes, so the
   schedule cannot change the image; the first call records every bounce.

A third change was measured and rejected: aggregating each wave's appends
into one atomic with subgroup ballots. It made no difference beyond noise
on this device (0.68605 against 0.67606 ms for `cornell-v1` with every
round recorded; 5.1826 against 5.2019 ms at 1024×1024), presumably because
the driver already aggregates these atomics, and it would have required
compute subgroup ballots. Appends keep one atomic per lane.

## Checks

**`renderer.path.wavefront`** additionally requires that the reference
scene's occupancy sample ran fewer rounds than it traced bounces, so the
tail is exercised. Its default-build result:

- Every scenario and the reference-image comparison pass, with the same
  z-scores as before (image z at 1024 spp: 2.49, 2.14, 2.13; maximum tile
  |z| 2.99); the brighter red wall is rejected at max |z| 6.32.
- The reference's own samples 0–1023 **reproduce the committed reference
  bit for bit**, and the same 64 samples from both integrators are **bit
  for bit identical**; both now run with rounds and the tail.
- The occupancy sample traced the same 59,654 rays in 17 bounces as
  before, 6 of them as rounds and the rest in the tail. Generate,
  intersect, shade, tail and accumulate took 0.013, 0.074, 0.068, 0.035
  and 0.003 ms of a 0.217 ms scene pass, which took 1.079 ms before.

**`renderer.path.reference`** is unchanged and still reproduces the
committed reference bit for bit. The references were not regenerated.

Three fault injections each changed one line of `wavefront.slang`; the
default build's report then failed `renderer.path.wavefront` while
`renderer.path.reference` passed:

| Fault | Detail of the failing check |
| --- | --- |
| A tail ray that misses ends without the environment | `bsdf: Lambert back face: radiance 0.062500 instead of 0.162500 in channel 0 at pixel 923` |
| Appends never grow their queue's dispatch | `bsdf: Lambert front face: no pixel's samples all hit` |
| Shade hands intersect its own bounce instead of the next | `bsdf: Lambert front face: no pixel's samples all hit` |

The restored source rebuilt and passed.

## Performance

The [fixed benchmarks](../reference/SCENE.md#fixed-benchmarks) were run
three times per configuration, sequentially, at the default 128×128 with
64 measured frames after 8 warmup frames:

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/wave02/final-1.json --benchmark-label "wave02 final" --benchmark-integrator wavefront
```

"Before" is the same command on a build of `ec643a2`, run in this session;
its medians are within 0.3% of the equivalence report's. "Every round" is
the first change alone, from a build of this tree that could be told to
record every bounce. Every benchmark run of this session (98 reports, 392
scene measurements) passed with zero validation messages. Every 128×128
wavefront image, under every schedule below, was **identical to the
"before" images**, which the equivalence report found identical to the
reference integrator's.

Medians of the three invocations' GPU medians, minimum–maximum for the
final build:

| Scene | Reference ms | Before ms | Every round ms | Final ms | Final speedup | Final / reference |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `cornell-v1` | 0.06522 | 1.16922 | 0.67606 | 0.23955 (0.23930–0.23957) | 4.88× | 3.7× |
| `textured-v1` | 0.04434 | 1.05147 | 0.53992 | 0.16056 (0.16030–0.16093) | 6.55× | 3.6× |
| `instanced-v1` | 0.03434 | 1.00464 | 0.51173 | 0.13971 (0.13952–0.13982) | 7.19× | 4.1× |
| `geometry-v1` | 0.09194 | 1.17110 | 0.66542 | 0.29043 (0.29000–0.29053) | 4.03× | 3.2× |

Blocking wall medians fell from 3.54–3.74 ms to 1.61–1.74 ms, mostly
because the command buffer recorded once per call is shorter. Per-kernel
medians of the final build:

| Scene | Rounds | Generate ms | Intersect ms | Shade ms | Tail ms | Accumulate ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `cornell-v1` | 6 | 0.0135 | 0.0825 | 0.0743 | 0.0425 | 0.0033 |
| `textured-v1` | 3 | 0.0138 | 0.0531 | 0.0418 | 0.0219 | 0.0033 |
| `instanced-v1` | 3 | 0.0135 | 0.0497 | 0.0343 | 0.0142 | 0.0030 |
| `geometry-v1` | 4 | 0.0138 | 0.1402 | 0.0636 | 0.0307 | 0.0037 |

### Choosing the tail threshold

The threshold was chosen from a sweep at 128×128 (three invocations each,
GPU medians in ms, median rounds in parentheses):

| Rounds while the queue holds at least | `cornell-v1` | `textured-v1` | `instanced-v1` | `geometry-v1` |
| --- | ---: | ---: | ---: | ---: |
| every bounce | 0.67606 (65) | 0.53992 (65) | 0.51173 (65) | 0.66542 (65) |
| 1 path | 0.39832 (19) | 0.19456 (6) | 0.16082 (5) | 0.32422 (7) |
| 1/64 of the camera rays | 0.27182 (8) | 0.17757 (4) | 0.15365 (4) | 0.28950 (4) |
| **1/16 of the camera rays** | 0.23941 (6) | 0.16083 (3) | 0.13939 (3) | 0.29014 (4) |
| 1/4 of the camera rays | 0.20790 (4) | 0.14128 (2) | 0.12328 (2) | 0.28963 (4) |
| more than all of them (only the camera rays' round) | 0.13312 (1) | 0.11870 (1) | 0.10358 (1) | 0.18318 (1) |

Each bounce moved from the rounds into the tail saved 15–25 µs of
`cornell-v1`'s frame, so the fewer the rounds, the faster the frame: the
fastest schedule is the reference integrator in a compute kernel after
one round. 1/16 keeps
the bulk of each sample's paths in queues, where later scheduling such as
path classification can act on them, and gives most of the gain (4.0–7.2×
of the 6.4–9.7× available). It is a single constant,
`kWavefrontTailDivisor`.

### Larger targets

To see whether rounds pay off once queues are large, the benchmark gained
`--benchmark-size`; at 1024×1024 the Cornell image is not compared with
the reference, which exists only at 128. Five invocations of 16 measured
and 4 warmup frames each, GPU medians with minimum–maximum:

| Scene | Reference ms | Every round ms | 1 path ms | 1/16 (final) ms | Camera round only ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| `cornell-v1` | 1.5190 (1.5168–1.5213) | 5.8885 (5.8632–6.0777) | 5.6455 (5.6318–5.6759) | 5.2048 (5.1963–5.2127) | 3.0472 (3.0463–3.0520) |
| `textured-v1` | 6.3994 (4.3221–7.0041) | 28.2291 (26.1966–29.3124) | 26.8680 (17.9212–29.1459) | 25.4793 (25.3008–28.1227) | 21.3269 (7.9830–23.5223) |
| `instanced-v1` | 7.1012 (6.1902–7.8581) | 30.4408 (27.0809–30.8592) | 26.2588 (26.1073–27.6406) | 26.4739 (25.6914–28.3935) | 22.9295 (21.4757–23.3165) |
| `geometry-v1` | 12.5202 (11.6209–13.6704) | 7.9665 (7.3651–45.7911) | 7.7255 (5.3656–43.8174) | 40.6893 (7.4892–42.6106) | 13.4075 (3.9457–26.6538) |

Each schedule's images were identical across all five invocations and
to the every-round images. Only `cornell-v1`'s timings are stable. The
other scenes varied between and within invocations, by up to eight times
under the wavefront schedules (an invocation's frames would change speed
partway through and stay there) and by up to 1.6 times under the
reference integrator; with the desktop on the same GPU, this was taken as
outside interference, and those rows are not a basis for comparison.
`cornell-v1` is: with a million camera paths, the wavefront schedules
still rank as at 128×128, and the final build is 3.4 times slower than the
reference integrator. Its rounds' intersect and shade kernels take 2.09
and 2.08 ms, while the reference integrator traces the whole frame in
1.52 ms. That points at the rounds' work per path — each kernel of each
round loads and stores a 112-byte path record — rather than their number;
it was not measured separately.

## Builds and tests

| Configuration | Result |
| --- | --- |
| `ost build --jobs auto`, `ost test`, `ost validate --strict-renderer-evidence` | built; 18 of 18 tests passed, including the install tree and `lotus-renderer-benchmark-wavefront`; validation passed |
| `ost build --without-runtime --intent ci-core --jobs auto`, `ost test --without-runtime --intent ci-core` | built; 10 of 10 passed; `renderer.path.wavefront` is an explained SKIP |
| `ost build --profile lookdev --intent hydra --jobs auto`, `ost test --profile lookdev --intent hydra`, `ost validate --profile lookdev --intent hydra --strict-renderer-evidence` | built; 35 of 35 passed, including usdview; validation passed |

The benchmark's `wavefront` measurements now include the bounces run as
rounds and the tail's GPU duration. Its CTest checks that the default
target is 128 pixels with the Cornell reference compared, that every
wavefront frame ran at least one round, and that sizes outside 16–4096
are rejected.
