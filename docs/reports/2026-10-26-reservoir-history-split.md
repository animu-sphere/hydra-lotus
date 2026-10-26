# Reservoir history split

## Evaluation brief

The [frame-residency inventory](2026-10-26-frame-residency.md#candidates-and-decisions)
deferred the reservoir history representation as the leading device cost:
with temporal and spatial reuse the reservoirs held 640 B per pixel, of
which 576 B were three 192-byte `RestirHistoryRecord` buffers (two history
frames and the spatial input). Each record was the 64-byte resolved
`RestirReusePixel` followed by 128 bytes of the exact jittered source
surface (origin and view depth, geometric and shading normals, outgoing
direction, evaluated BSDF, instance and material identity).

The spatial pass only copied that surface: its output kept the receiver's
surface, already in its input. Storing the surface once per frame removes
the copy without changing any stored value. Compacting fields
(half-precision colours, octahedral normals, derived Fresnel) changes
values, and needs the CPU-oracle tolerance and bias comparisons the
[adoption rules](../design/POST_V060_STRATEGY.md#4-measurement-and-adoption)
require. It was not attempted in this slice (see
[Remaining candidates](#remaining-candidates)).

Three lossless layouts were compared:

- **split**: two 64-byte record buffers (current/previous), a 64-byte
  spatial input and a separate current/previous pair of 128-byte surfaces.
  The temporal pass writes the frame's surfaces once; the spatial pass
  reads them and writes only 64-byte records. Surfaces are never read back,
  so every mirror holds records only.
- **nested** and **flat**: the 192-byte history kept as one record per
  pixel, with only the spatial input reduced to 64 bytes. The spatial pass
  writes the record half of the current history beside the surfaces it
  reads. The two differ only in shader structure layout.

All three hold 512 B per pixel on the device under both reuse passes.

Conditions: Windows AMD64, MSVC 19.51.36256.0 Release, NVIDIA RTX A5000
(24 GiB), driver 597.16, Vulkan 1.4.329, validation and synchronization
validation active. Every binary was built from `2aa9f5f` plus this
report's benchmark additions (per-frame `frame_gpu_ms` and
`--residency-restir-spatial` in `--benchmark-residency`, which change no
renderer work); "main" is that build without the layout change. Adopted
binary SHA-256 `b15a95c22e013181…`, main `f4d2d192b4ee3080…`; full hashes
and every value are in the [data](2026-10-26-reservoir-history-split.json).

## Correctness

- **Images:** `lotus-headless --benchmark-restir` with the
  `glossy-disocclusion` workload (static and moving-camera disocclusion
  scenes, 32x32, 16 frames after 4 warmup, 2 sequences, 64 lights, 8
  candidates, history cap 32, seed 17) wrote 37 last-frame and ensemble
  images per integrator covering NEE/MIS, initial, temporal, spatial and
  spatiotemporal reservoirs. Under both the reference and wavefront
  integrators, all 37 are byte-identical to `2aa9f5f`, so every error,
  bias and frame-variation metric is unchanged.
- **CPU oracles:** `lotus-renderer-restir-gpu` passes: GPU/CPU merges,
  corrected weights and counts, differing support, moving disocclusion,
  visibility, resets, overflow and the mirror lifecycle, under both
  integrators. Its memory accounting now expects 608 B per pixel for
  temporal reuse and 768 B with spatial reuse when frames read back (was
  864 and 1280).
- **Residency:** `lotus-renderer-residency-benchmark` checks the new
  `restir.surfaces` owner (two allocations, at least 256 B per pixel, no
  mirror), records of at least 128 B per pixel in `restir.history` and 80 B
  in `restir.spatial`, and one timing entry per frame.
- A wiring error in an intermediate nested build (the current-history
  binding left stale without spatial reuse) disabled temporal reuse. It
  showed only in the image comparison, as up to 50% higher ensemble error;
  the adopted layout does not share that binding.

## Residency

`--benchmark-residency`, `cornell-v1`, three one-sample frames per mode on
a fresh renderer, compared with the
[lazy-mirror campaign](2026-10-26-lazy-readback-mirrors.md). Renderer
groups after the last frame, MiB:

| Mode | 1080p device before | after | 4K device before | after | 1080p read-back host before | after |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `restir-reuse` | 1,642.3 | 1,389.2 | 6,504.4 | 5,491.9 | 1,593.9 | 834.6 |
| `svgf-restir` | 1,895.5 | 1,642.3 | 7,516.9 | 6,504.4 | 1,752.1 | 992.8 |

- Spatial reuse saves 128 B per pixel on the device: 253.1 MiB at 1080p
  and 1,012.5 MiB at 4K. The process's device-local heap usage fell by the
  same amount (`svgf-restir` 2,040.0 → 1,786.9 MiB at 1080p,
  7,661.2 → 6,648.7 MiB at 4K).
- Temporal reuse alone keeps 432 B per pixel on the device (48 initial,
  two 64-byte records, two 128-byte surfaces): unchanged.
- When frames read back, the mirrors hold records only: 192 B instead of
  576 B per pixel, 759 MiB less host memory at 1080p.
- Every other owner, in every mode and at every size, is identical to
  before.

Reservoir bytes per pixel on the device are now 48 initial, 128 records,
256 surfaces and 80 spatial (input and sources): 512 B.

## Time

GPU timestamps from `--benchmark-residency --residency-mode restir-reuse
--residency-frames 16`, `cornell-v1`, medians of frames 5–16 of each run,
ms. Runs alternated main and split, two rounds. "Scene" is the whole scene
duration, which includes the reservoir passes.

| Size | Spatial | Main scene | Split scene | Main temporal | Split temporal | Main spatial | Split spatial |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1080p | on | 17.25, 15.29 | 12.49, 12.61 | 3.05, 3.24 | 2.36, 2.38 | 6.09, 5.06 | 2.87, 2.90 |
| 4K | on | 57.13, 58.51 | 48.41, 50.55 | 9.99, 10.37 | 10.26, 10.27 | 20.56, 20.95 | 12.45, 13.28 |
| 1080p | off | 9.08, 9.17, 9.53, 9.26 | 9.47, 10.00, 9.57, 9.48, 9.40 | 2.56, 2.60, 2.59, 2.55 | 2.37, 2.37, 2.38, 2.32, 2.31 | — | — |
| 4K | off | 36.07, 36.53 | 35.62, 35.32 | 9.93, 10.38 | 10.16, 9.41 | — | — |

- With spatial reuse the spatial pass is 37–53% faster and the scene
  14–28% faster at both sizes. An earlier campaign comparing all layouts
  agrees (below).
- With temporal reuse alone, the reuse pass is 7–11% faster at 1080p and
  unchanged at 4K. The 1080p scene medians overlap, and their difference
  follows the clock state, not the layout. Frame times split into two
  clusters in both builds: main 9.03 ms (26 frames) and 9.71 ms (22);
  split 8.81 ms (24) and 9.85 ms (36). The split runs spent more frames in
  the slower cluster. The scene minus the reservoir passes is 5.44 ms
  (main) against 5.51 ms (split), within that spread. Clocks were not
  locked; two main runs and one split run whose first frames took
  20–32 ms are excluded from the 1080p row and kept in the data.
- Layout comparison, 1080p with both reuse passes, two runs each, ms:

  | Layout | Scene | Temporal | Spatial |
  | --- | ---: | ---: | ---: |
  | main | 13.76, 14.43 | 2.56, 2.69 | 4.69, 4.68 |
  | split | 11.77, 11.46 | 2.29, 2.28 | 2.72, 2.76 |
  | nested | 12.57, 13.28 | 2.44, 2.46 | 3.63, 3.66 |
  | flat | 14.20, 13.10 | 3.27, 2.97 | 3.67, 3.63 |

  Keeping the 192-byte history (nested, flat) gains less in the spatial
  pass, and flat slows the temporal pass.
- At 256x256 (`--benchmark-restir`, 32 frames) the split layout's spatial
  pass was 22–28% faster, but its temporal pass was 10% slower in temporal
  reuse alone. That did not reproduce at 1080p or 4K. All passes share one
  fragment shader, so small layout changes move code generation for all of
  them; at that size the passes are not bandwidth-bound. These small-target
  runs are kept in the data but not used for the decision.

The cause of the spatial pass gain was not profiled. The pass now writes
64 instead of 192 bytes per pixel and reads neighbour records from a
64-byte-stride buffer.

## Decision

Adopted: the split layout. Spatial reuse holds 512 instead of 640 B per
pixel on the device (1.0 GiB less at 4K), and its frames are faster at
1080p and 4K. Temporal-only reuse keeps its memory and reuse-pass time
within the measured spread. Every image and product is byte-identical,
and the CPU oracles pass unchanged.

## Remaining candidates

- **Value compaction.** The surfaces (256 B per pixel) are now the largest
  reservoir owner. Their two padding words and the Fresnel `f0`/`f90`
  (derivable from base colour, metallic and the dielectric terms) would
  save 16–24 B per surface if the derivation is bit-exact. Half-precision
  colours or octahedral normals change the receiver's stored shading. The
  spatial pass evaluates the final estimate from that stored surface, so
  this would change images. The existing oracles compare at 2e-6 to 2e-5,
  while half precision has 4.9e-4 relative error. Not pursued without a
  precision contract.
- **Lifetime aliasing** (the spatial input with other within-frame
  buffers) stays deferred as in the inventory.

## Unavailable and unmeasured

- Clocks were not locked; pass times include the driver's power-state
  changes, and validation layers were active.
- Hydra and the viewport were not rebuilt or run. They use the same
  backend and products; the reservoir debug views read the 64-byte
  history records (stride 4 instead of 12) and were not exercised.
- Wavefront timing at 1080p/4K, other GPUs, drivers and memory
  architectures.
