# Analytic-light crossing hierarchy

## Experiment brief

LOTUS-LIGHT-06, the minimal acceleration experiment that the
[crossing-cost evaluation](2026-10-25-light-crossing-cost.md#decisions)
opened: crossing tests were 81–88% of a 256-light `many-lights-v1` frame
under both integrators, growing linearly with the light count, while
selection was constant. The
[workstream contract](../design/ROADMAP_POLICY.md#practical-foundation-workstreams)
asks for one bounded structure compared on GPU time, equal-quality behavior,
memory, build/update cost and the low-light-count regression.

The structure chosen is a light hierarchy traversed by the existing crossing
test, not the lights as procedural primitives in a second ray-tracing
acceleration structure: it needs no new pipeline, descriptor or build pass,
lives in the light table the scene already rewrites, and keeps the
per-light test (and its precision) unchanged.

- **Build** (`BuildLightHierarchy`, `gpu_scene.cpp`). Whenever the light
  table is rebuilt, the sphere, rect and disk lights get a binary hierarchy:
  split at the median of their centres along the axis over which the centres
  spread most, ties by index, so its depth is at most ⌈log₂ n⌉. Each 64-byte
  interior node stores both children's bounds, so a node is read only when a
  ray enters it. A light's bounds are widened by 10⁻⁴ of its extent plus
  10⁻⁵ of its coordinates' magnitude and rounded outwards. Distant lights
  get an index list; parallel lights, which no ray crosses, get neither.
  Nodes and indices follow the analytic records in the light buffer.
- **Traversal** (`CrossedLights`, `common/scene.slang`). The per-light test
  moved unchanged into `CrossedLight`. A ray descends the hierarchy with a
  32-entry stack, testing each child's box against its segment
  [0, distance] and running the per-light test at each leaf it enters; an
  escaping ray then tests the distant lights.
- **Baseline.** `PathTracingSettings::light_crossing` keeps the loop over
  every light as `LightCrossing::Linear`, so both run from one binary
  (`--benchmark-light-crossing`,
  [contract](../reference/SCENE.md#path-tracing)).

Predeclared budget: the evaluation's: `many-lights-v1` under MIS, both
integrators, 128 and 512 pixels, light-grid sides 1, 2, 4, 8 and 16 (1 to
256 lights), linear and hierarchy, 8 warmup and 64 measured frames, three
fresh processes per configuration in run-major round-robin order (120
invocations). No retries or exclusions. Because the 512-pixel one-light and
64-light results showed whole processes at lower power states, those four
configurations were run in three further processes (24 invocations), and
the one-light Reference comparison once more in four interleaved process
pairs without telemetry (below).

Conditions: Windows AMD64, MSVC 19.51.36256.0 Release, NVIDIA RTX A5000,
driver 597.16, Vulkan 1.4.329, validation and synchronization validation
active, clocks unlocked and never changed. `nvidia-smi` power states were
sampled every 250 ms around each campaign process. The binary was built from
`7b6370a` plus this change; its SHA-256 is `941f895663ea4543…` (full hash
in the [data](2026-10-25-light-crossing-hierarchy.json)).

## Correctness

- All 144 campaign invocations passed with zero validation messages; the
  core build's renderer report (including `renderer.path.analytic_lights`
  and the bitwise BSDF reference) passed.
- Within every configuration, the processes' images are byte-identical, and
  so are the Reference and Wavefront images of the same grid, size and
  crossing.
- **Linear and hierarchy agree to rounding.** With 1, 4 and 16 lights their
  images are byte-identical. With 64 and 256 lights a few values differ by
  at most 4.8 × 10⁻⁷ absolute (relative 2.2 × 10⁻⁷, one or two float ulps);
  per-sample variance is equal to every printed digit:

| Lights | Differing values, 128 pixels | Differing values, 512 pixels |
| ---: | ---: | ---: |
| 1, 4, 16 | 0 of 49,152 | 0 of 786,432 |
| 64 | 3 | 20 |
| 256 | 47 | 664 |

  A missed light would change a value by its MIS-weighted radiance, orders of
  magnitude more. The differences grow with how often one ray crosses
  several lights, consistent with the changed summation order of three or
  more crossed lights (a sum of one or two terms does not depend on order);
  the cause was not isolated further.

## GPU time against light count

GPU scene-pass milliseconds per frame, the lowest of the processes'
medians (as in the [evaluation](2026-10-25-light-crossing-cost.md#cost-against-light-count),
some processes ran at lower clocks throughout; the data keeps every
process's median, mean, p95 and sampled power states). The 512-pixel 1- and
64-light rows take the lowest of all six processes.

| Lights | Ref 128 linear | Ref 128 hierarchy | Ref 512 linear | Ref 512 hierarchy | WF 128 linear | WF 128 hierarchy | WF 512 linear | WF 512 hierarchy |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 0.097 | 0.099 | 0.632 | 1.271¹ | 0.223 | 0.227 | 0.798 | 0.829 |
| 4 | 0.301² | 0.101 | 1.248² | 0.652 | 0.240 | 0.231 | 0.858 | 0.830 |
| 16 | 0.147 | 0.105 | 0.949 | 0.686 | 0.309 | 0.244 | 1.040 | 0.866 |
| 64 | 0.311 | 0.110 | 2.120 | 0.738 | 0.594 | 0.285 | 1.946 | 0.933 |
| 256 | 0.994 | **0.125** | 6.636 | **1.080** | 2.055 | **0.384** | 5.795 | **1.077** |
| Speed-up at 256 | | 8.0× | | 6.1× | | 5.3× | | 5.4× |
| Slope (µs/light) | 3.26 | 0.10 | 22.8 | — | 7.22 | 0.60 | 19.7 | — |

¹ All six processes of this configuration were sampled in a P3 or P5
state. In four interleaved process pairs without telemetry
(`build/light-hierarchy-pinpoint-20261025`), the fastest medians were
0.636 ms (hierarchy) and 0.684 ms (linear), and the minima 0.553 and
0.550 ms; at the same clock the two are equal.
² All three processes ran at a lower clock; the evaluation measured
0.104 ms and 0.678 ms for this configuration.

Slopes are least-squares fits over the five light counts, footnoted points
included. At 512 pixels the hierarchy's slope is not given: its one-light
value is the clock artifact above. The 256-light linear values are 1.18–1.61× the
evaluation's for the same binary path (0.733, 4.125, 1.739, 4.331 ms):
this session's processes were slower throughout, which is why the
comparison is within one campaign.

With the hierarchy, 256 lights cost 1.3× (Reference 128) to 1.7×
(Wavefront 128) one light, where the linear loop cost 9–10×. The remaining
growth is the per-light test of the lights a ray does enter plus the
traversal.

### Wavefront kernels

Kernel medians of the fastest process, 128 pixels, milliseconds:

| | Camera | Intersect | Shade | Tail |
| --- | ---: | ---: | ---: | ---: |
| 1 light, linear | 0.013 | 0.052 | 0.071 | 0.064 |
| 1 light, hierarchy | 0.013 | 0.052 | 0.072 | 0.065 |
| 256 lights, linear | 0.013 | 0.442 | 0.460 | 1.112 |
| 256 lights, hierarchy | 0.013 | 0.112 | 0.145 | 0.091 |

The tail kernel, which the evaluation found to be crossing, falls from
1.112 to 0.091 ms; rounds (6) and rays per path (3.649) are unchanged.

## Low light counts

- **One light:** +1.5 µs (Reference 128, 1.5%), +4.2 µs (Wavefront 128,
  1.9%) and +31 µs (Wavefront 512, 3.9%; +15 µs within the rerun alone) between the fastest processes; at
  Reference 512 the interleaved pairs above show no difference. These are
  within the spread between processes at the same power state (for
  example 0.814–0.816 ms for linear Wavefront 512 in the rerun).
- **Four lights:** the hierarchy is faster or equal in every configuration.

No threshold that falls back to the linear loop below some light count is
warranted by these results.

## Memory and build cost

The hierarchy has n − 1 nodes of 64 bytes for n ≥ 2 bounded lights (one
for a single light) plus 4 bytes per distant light, recorded as
`GpuSceneStats::light_hierarchy_bytes`: 64, 192, 960, 4,032 and 16,320
bytes for 1, 4, 16, 64 and 256 lights, beside the analytic records' 96 bytes
per light (24 KiB for 256).

Build time, measured on the host with a copy of the build functions
compiled `/O2` (`build/light-hierarchy-build-cost-20261025.cpp`), median of
2,000 builds (200 above 256 lights) of the grid fixtures' sphere lights:

| Lights | 1 | 4 | 16 | 64 | 256 | 1,024 | 4,096 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Build (µs) | 0.1 | 0.6 | 1.8 | 7.6 | 50.5 | 158 | 895 |

The hierarchy is rebuilt, not refitted, with every light-table rewrite:
when lights change, and also when instance records or materials change (the
table's emissive-triangle weights depend on them). Its upload is part of
the light table's single copy. For 256 lights this adds about 50 µs to such
an update; the benchmark's initial update wall time (4.5–5.3 ms) does not
resolve it, because both crossing modes build the hierarchy.

## Decision

**Adopt: `LightCrossing::Hierarchy` is the default.** At 256 lights it cuts
the scene pass 5.3–8.0× under both integrators and both sizes with images
equal to rounding, costs 16 KiB and about 50 µs of host time per light-table
rebuild, and shows no low-light-count regression beyond the spread between
processes. The linear loop stays selectable as the evaluation baseline.

Not changed by this experiment: light selection (deferred by the
evaluation), the BSDF-sampling parallel-light loop, and the Hydra and
viewport frontends, which share the backend and therefore use the hierarchy
by default.

## Limits

| Missing | Reason / next evidence |
| --- | --- |
| Other light shapes and layouts | Only the grid fixtures' spheres in one plane were timed. Rect, disk and distant lights take the same per-light test; overlapping or very unequal lights, where median splits produce poor boxes, were not measured. |
| BSDF-only sampling | Not timed; crossing is the same function, and its parallel-light loop is unchanged. |
| Procedural-primitive alternative | Not built; the hierarchy removed nearly all of the measured crossing cost, leaving little for a hardware-traversed structure to recover at these counts. |
| More than 256 lights, 1080p/4K | Not measured on the GPU; build cost is reported to 4,096 lights. |
| Per-frame rebuilds | Motion that rewrites instance records (rigid transforms) rebuilds the light table each frame, now including the hierarchy; not measured with many lights. |
| Hydra and viewport | Shared backend code; their trees were not rebuilt or rerun. |
| Controlled timing | Clocks were unlocked and per-process bimodal; lowest process medians are used and every process is kept. |

## Verification

- `ost build --jobs auto` (core Release): passed, including the renderer
  report.
- `ctest -R "lotus-renderer-benchmark($|-mis|-no-driver|-wavefront)"`: 4/4
  passed. The MIS benchmark test now records the default crossing and a
  192-byte hierarchy for the side-2 grid, runs the linear baseline, and
  rejects an unknown crossing without touching output.
- Campaign: `build/light-hierarchy-campaign-20261025.py` (output
  `build/light-hierarchy-grid-20261025`), rerun
  `build/light-hierarchy-rerun-20261025.py`
  (`build/light-hierarchy-rerun-20261025`), pinpoint pairs
  `build/light-hierarchy-pinpoint-20261025`; summaries by
  `build/light-hierarchy-analyze-20261025.py`.
- Not rerun: the full gpu and research tiers, Hydra, viewport and `ci-core`
  trees.
