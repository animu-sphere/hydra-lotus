# Scene-update GPU timestamps

Measured on 2026-10-05 in the working tree based on `c9e615f`, on Windows 11
x86_64 with MSVC 14.51, OpenStrata 0.23.14 and Release builds. The GPU was an
NVIDIA RTX A5000 (driver 597.16). Hydra used OpenUSD 26.08 lookdev and
CPython 3.13. All runs were local; hosted CI was not measured.

## Scope

This extends per-pass GPU timestamps
([design policy §24](../design/DESIGN_POLICY.md#24-gpu-profiling)) beyond
the scene pass to the scene update, the last item
[Renderer Phase 1](../roadmap/README.md#status-at-a-glance)
needed alongside its vertical slice. Only the backend's GPU scene, its
public evidence types and the headless runner changed; the shaders did not,
so the committed reference images are unaffected.

The durations below are one run's, recorded to show what the check
reports. They are not a performance record: the roadmap's
[performance records](../roadmap/current.md#performance-baseline-and-foundation-release) remain
separate work.

## Design

`UpdateScene` records its plan in one submission: the staging copies, then
the BLAS builds, then the TLAS build or refit, with a barrier between each
phase. The GPU scene now has a persistent four-entry timestamp query pool,
created only where the queue reports timestamp bits. The submission writes
a top-of-pipe timestamp before the copies and a bottom-of-pipe timestamp
after each phase; since each barrier holds the next phase back until the
previous one finishes, the differences are the phases' GPU durations.

| Field | Phase | 0 when |
| --- | --- | --- |
| `GpuSceneTimings::upload_gpu_ms` | geometry, instance-record and TLAS-input copies | nothing was copied |
| `blas_build_gpu_ms` | the uploaded geometries' BLAS builds | no geometry was uploaded |
| `tlas_build_gpu_ms` | the TLAS build or refit | the TLAS was left as it was |

A phase without commands reports exactly 0 rather than the gap between two
timestamps. `timings.available` is false when the call submitted nothing,
such as an empty or environment-only plan, or after a failure;
`GpuSceneStats::timestamps_available` says whether the queue supports
timestamps at all, so a caller can tell the two apart. The results are read
after the submission's fence, so no further synchronization is needed.

The scene pass keeps its single measurement, `primary_ray_gpu_ms`, over the
camera and radiance passes together.

## Fixture

`renderer.scene.timestamp`, written by `lotus-headless`, builds a
256×256-quad grid (131,072 triangles) placed by an instancer at 1,024
positions, so each phase has real work, and drives it through the GPU
scene after `renderer.scene.upload`:

| Step | Copies | BLAS | TLAS |
| --- | --- | --- | --- |
| insertion | positive | positive | positive (build) |
| every placement moved | positive | 0 | positive (refit) |
| material edit | positive | 0 | 0 |
| unchanged commit | no submission | | |
| removal | 0 | 0 | finite (empty build) |

Without acceleration structures only the copies are required to run. The
check is a SKIP when the queue has no timestamp support, and its detail
lists the durations. A measured run:

| Step | `upload_gpu_ms` | `blas_build_gpu_ms` | `tlas_build_gpu_ms` |
| --- | --- | --- | --- |
| insertion | 0.1023 | 0.8410 | 0.1465 |
| placements moved | 0.0094 | 0 | 0.0162 |
| material edit | 0.0057 | 0 | 0 |
| removal | 0 | 0 | 0.0168 |

The Hydra tree's run measured the same order of magnitude (BLAS build
0.5191 ms).

Two deliberate faults were each reverted before the runs below:

- not resetting the timings at the start of an update failed the check
  ("unchanged commit: timings were reported without a submission");
- reporting the TLAS duration only when the TLAS was *not* built failed it
  ("insertion: tlas_build_gpu_ms=0.000000 is not a positive duration").

Faults that only move a timestamp, such as writing one at the top of the
pipe, can still leave a few nonzero ticks, so the check cannot be relied on
to catch them.

## Interactive usdview

`ost renderer view --profile lookdev --intent hydra` built its own Hydra
tree, installed it under `.strata/renderer-view/`, and opened the smoke
scene in usdview with Lotus selected through `/Camera`; it returned 0 when
the window was closed. `ost plugin view` stops with "no
openstrata.plugin.yaml" here: it serves plugin bundles, not renderer
projects. Both are in the
[building guide](../guides/BUILDING.md#the-hydra-adapter).

## Verification

Before the builds, `ninja -t deps` listed objects without recorded header
dependencies in the ci-core tree, the known Japanese-MSVC issue
([roadmap](../reference/SUPPORTED_CONFIGURATIONS.md#build-and-tooling-limitations)); they were
deleted. The core and Hydra trees listed none.

- `ost build --jobs auto`, `ost test`: **8/8 passed**;
  `ost validate --strict-renderer-evidence`: **passed**, with
  `renderer.scene.timestamp` PASS and zero Vulkan validation messages.
- `ost build --without-runtime --intent ci-core --jobs auto`,
  `ost test --without-runtime --intent ci-core`: **8/8 passed**.
- `ost build --profile lookdev --intent hydra --jobs auto`,
  `ost test --profile lookdev --intent hydra`: **18/18 passed**;
  `ost validate --profile lookdev --intent hydra --strict-renderer-evidence`:
  **passed**.
- `ost renderer viewport -- --frames 8 --hidden`: presented 8 frames;
  `ost validate --intent renderer-viewport`: **passed**.

The current behaviour is in the
[scene reference](../reference/SCENE.md#scene-update-timestamps); remaining
work is in the [roadmap](../roadmap/current.md).
