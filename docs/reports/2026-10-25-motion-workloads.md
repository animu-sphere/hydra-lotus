# Camera and object motion on the shared workloads

## Evaluation brief

LOTUS-PERF-01 follow-up to the [shared static baseline](2026-10-21-shared-performance-workloads.md),
whose third investigation candidate (the geometry update path) was deferred
until moving-object extraction, upload and acceleration work were measured
continuously. This change adds headless per-frame motion to the existing
benchmark and runner; renderer, backend and fixtures are unchanged.

Predeclared budget: 126 sequential headless invocations, the seven manifest
workloads × three motions (`camera`, `transform`, `deform`) × two integrators
× three fresh-process repetitions, at the manifest's 128×128, MIS, 64 bounces,
8 discarded warmup frames and 64 measured frames. No retries, extra
fixtures or alternative amplitudes enter this budget; a driver or validation
failure stops the campaign and remains evidence. The question is which
per-frame update costs lead under motion, and whether any justifies an
optimization task. Speed rankings between integrators, and any default
change, are out of scope.

Hardware: Windows AMD64, MSVC 19.51.36256.0 Release, 32 logical processors,
NVIDIA RTX A5000, driver 597.16 (raw 2504261632), device API 1.4.329.
Clocks are unlocked and unchanged; the initial `nvidia-smi` observation was
P8, graphics 210 MHz, memory 405 MHz. Telemetry polling was disabled. No
build, test or other benchmark overlapped the campaign; desktop GPU activity
is not isolated. Validation and synchronization validation were active, as
in every shared measurement, so CPU times include validation-layer cost.
Binary SHA-256 `897995790c560aaefefc81d31591e9e0bda374b9b99422e29796ee1ddd60a825`,
built from `516c659` plus this change before it was committed.

## Procedure and retained evidence

`--benchmark-motion` edits the scene before every measured frame, growing a
camera pan, a translation of the workload's edited mesh or a shear of its
z by x linearly to 0.05 at the last frame. Each frame is therefore one
sample after an accumulation restart; the
[benchmark contract](../reference/SCENE.md#fixed-benchmarks) owns the
metric scopes. The edited mesh is the existing single-edit mesh: a Cornell
wall for the box-based fixtures, one textured tile, the instanced prototype
(all 4,096 placements) and the 524,288-triangle grid. The commands are in
the [build guide](../guides/BUILDING.md#camera-and-object-motion).

All 126 attempts passed with zero validation messages. Every frame's
refit counters matched its motion: camera frames produced empty plans,
transforms refit only the TLAS, deformations the edited BLAS and the TLAS,
with no builds. Scene-pool reservations were identical before and after
motion in every attempt. All three repetitions of every group produced the
same final image, and Reference and Wavefront final images matched for all
21 workload/motion pairs.

The [data](2026-10-25-motion-workloads.json) retains each attempt's report
(raw per-frame populations), command, log, image hash, build identity and a
per-group summary. Original JSON, PFMs and logs remain under
`build/motion-{camera,transform,deform}-{reference,wavefront}-20261025`.

## Power-state bimodality

GPU durations were bimodal per process: in 15 of 126 attempts, the median
GPU frame time was over three times the group's fastest repetition, for
every motion and both integrators (for example, camera-moving Reference
geometry 0.094, 0.847 and 0.094 ms). Upload, BLAS and TLAS timestamps
slowed by the same factor in those processes. This matches the documented
[power-state dependence](2026-10-10-wavefront-path-state.md#power-states),
not the motion kind. Observations below therefore use the **fastest
repetition's median** for GPU phases and state the affected attempts; the
JSON retains all three run medians and p95s for each metric.

## Per-frame update costs

Median per frame, in milliseconds. *Author* is the world edit plus `Commit`,
*extract* is `SceneExtraction` plus the draw summary, *update* is blocking
`UpdateScene` wall time, and *GPU* sums its upload, BLAS and TLAS phases.
CPU columns vary little between repetitions (all three run medians are in
the JSON) and between integrators, so Reference values are shown.

| Workload | Motion | Author | Extract | Update wall | GPU phases | Staged bytes |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| any | camera | ≤0.0001 | ≤0.0001 | 0.0011–0.0018 | — | 0 |
| `cornell-v1` | transform | 0.0018 | 0.0054 | 0.161 | 0.012 | 1,472 |
| `instanced-v1` | transform | 0.038 | 0.111 | 0.310 | 0.054 | 737,280 |
| `geometry-v1` | transform | 0.0009 | 0.0017 | 0.160 | 0.012 | 192 |
| `textured-v1` | deform | 0.0095 | 0.0114 | 0.277 | 0.096 | 89,680 |
| `instanced-v1` | deform | 0.030 | 0.108 | 0.361 | 0.079 | 737,376 |
| `geometry-v1` | deform | 0.727 | 0.0049 | 2.80–2.98 | 0.82 | 9,449,680 |

The other box-based fixtures resemble Cornell (transform update wall
0.159–0.197 ms; deform 0.197–0.276 ms). Update-wall ranges show the fast
repetitions of both integrators. For `geometry-v1` deformation, the fastest
GPU phases were upload 0.36, BLAS refit 0.45 and TLAS refit 0.010–0.025 ms;
in the slow-state Reference processes they rose to 1.40, 2.15–2.25 and
0.025 ms and the update wall to 5.6–6.4 ms.

Rendering after each edit kept the static fixtures' scale: fastest-run GPU
frame medians were within 0.03 ms of the camera motion's for every fixture
and integrator except `many-lights-v1` (0.67–0.75 ms Reference, 1.67–1.75 ms
Wavefront). The blocking render wall stayed 1.4–3.5 ms, dominated as before
by waits and readback.

Three observations follow:

1. **A fixed blocking-update floor.** Every nonempty `UpdateScene` cost at
   least about 0.15 ms of wall time, including when its GPU phases took
   0.012 ms and it staged 192 bytes. Camera motion avoided it with empty plans. The floor is
   submission, fence wait and validation together; this API does not
   separate them.
2. **Whole-geometry restaging on deformation.** A compatible replacement
   stages the full geometry. For `geometry-v1`, 9,449,680 bytes per frame
   are positions (263,169 × 12 = 3,158,028 B), the **unchanged triangle
   indices** (524,288 × 12 = 6,291,456 B, 67%) and 196 bytes of instance
   and TLAS input. The update wall (2.8–3.0 ms) exceeds its GPU phases
   (0.82 ms) by about 2 ms of host work, which includes staging copies and
   validation but is not further attributed here. Authoring the replacement
   took a further 0.73 ms, outside `UpdateScene`.
3. **Per-placement instancer work.** Moving or deforming the instanced
   prototype rewrote all 4,096 instance records (737,280 bytes, 180 B per
   placement) with 0.11 ms of extraction, whereas a single-mesh transform
   staged 192–2,880 bytes. Its update wall stayed below 0.5 ms.

## Decisions

- **Investigate next** (candidate task): skip restaging unchanged triangle
  indices, and other unchanged attributes, on compatible geometry
  replacement. It is the largest per-frame staged cost under deformation
  and the only update here above 1 ms. Adoption requires before/after update
  wall, GPU-phase and staged-byte measurements on this procedure, CPU-oracle
  and refit-counter agreement, and unchanged images. It is not implemented
  by this change.
- **Defer** the blocking-update floor: at 0.15–0.2 ms per edited frame it is
  small beside the 1.4 ms render wall at 128×128; it needs measurement
  without validation layers and in the presented path before a decision.
- **Defer** instancer changes: per-placement rewrites stay below 0.5 ms for
  4,096 placements; larger instance counts are not measured.
- No renderer optimization or default change is adopted. Camera/object motion
  is now measured headlessly; this does not close LOTUS-PERF-01.

## Limits and remaining evidence

| Missing | Reason / next evidence |
| --- | --- |
| Hydra motion | Measures RenderWorld edits; Hydra dirty-bit translation and USD time samples are not exercised. |
| Viewport motion | The viewport has no motion mode; presentation, UI and the [known device loss](2026-10-21-shared-performance-workloads.md#viewport-evidence-and-failure) remain outside. |
| Temporal/SVGF under motion | Each frame restarts progressive accumulation; temporal history, reprojection and filtering are not used. |
| Image quality under motion | Final images are compared for determinism only; there is no moving reference. |
| Larger edits | One fixed mesh per workload, one amplitude; topology changes and many simultaneously edited meshes are not covered. |
| Controlled clocks | Clocks were unlocked and bimodal per process; GPU-phase comparisons across processes need locked clocks or same-state selection. |
| Host-side split | Staging copies, submission, waits and validation inside `UpdateScene` are not separately timed. |

## Verification

- `ost build --jobs auto` (core Release): passed, regenerating renderer evidence.
- `ctest -R "lotus-renderer-benchmark$|lotus-renderer-benchmark-mis$"`: 2/2
  passed. The MIS variant adds one Cornell run per motion (4 frames),
  checking recorded motion, refit totals, frame population and null static
  metrics, and rejection of an unknown motion without touching output.
- Python suite: 96 tests passed using a workspace-local temporary directory,
  including forwarding of `--motion`, per-frame refit checks, manifest
  validation and rejection with a viewport.
- Documentation links, anchors, indexes and whitespace checks passed.
- All 126 predeclared physical-GPU attempts passed with zero validation
  messages; evidence assembly rechecked the image determinism groups.
- Not rerun: the full research tier, Hydra, viewport and `ci-core` trees.
  Only `adapters/headless/benchmarks/benchmark.cpp` changed among C++
  sources; the static benchmark path keeps its procedure and report fields,
  adding `procedure.motion`, `procedure.motion_amplitude` and a null
  `motion` measurement.
