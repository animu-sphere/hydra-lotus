# Changed-section staging for compatible geometry replacement

## Evaluation brief

LOTUS-PERF-01 follow-up to the [motion workloads](2026-10-25-motion-workloads.md#decisions),
which named restaging unchanged triangle indices on compatible geometry
replacement as the next candidate. A compatible replacement (same vertex
count, equal triangles) already kept its slot, buffer and BLAS, but staged
and copied its whole buffer. It now stages only the sections whose contents
differ, provided the buffer layout is unchanged
([contract](../reference/SCENE.md#gpu-scene)); a layout change, such as
normals appearing, still stages the whole buffer.

Adoption criteria, from the candidate: before/after update wall, GPU-phase
and staged-byte measurements on the motion procedure, CPU-oracle and
refit-counter agreement, and unchanged images. Predeclared budget: the
motion campaign's deformation half rerun on the changed build, 42 headless
invocations (seven workloads × two integrators × three fresh processes) at
the same settings, compared with the
[earlier deformation runs](2026-10-25-motion-workloads.json) as the
baseline. No retries or extra fixtures. Camera and transform motion do not
replace geometry and were not rerun.

Hardware and conditions as in the baseline: Windows AMD64, MSVC
19.51.36256.0 Release, NVIDIA RTX A5000, driver 597.16, unlocked clocks
(initial `nvidia-smi` P8, 210 MHz, 405 MHz), validation and synchronization
validation active, no overlapping build or benchmark. Binary SHA-256
`fa46329b41bcb952d42bc21d83719824fd62ea47d91f214058ac50a2a1af8e57`, built from
`9a0682f` plus this change before it was committed. A first run of the same
budget on an intermediate build that compared positions twice gave the same
staged bytes and images; it is excluded and kept under
`build/restage-draft-deform-*-20261025`.

## Correctness

- `lotus-renderer-acceleration-compaction` replaces one resident geometry
  three times and reads the device buffers back: a replacement that adds
  normals and a texture-coordinate set (layout change) staged 504 bytes, a
  positions-only change 404 bytes and a change of positions, normals and
  texture coordinates 464 bytes. Their difference, 60 bytes, is exactly the
  normals (36) and texture coordinates (24); the earlier code staged the
  same bytes for both. Read-back positions, triangles, normals and texture
  coordinates equal the last source, with BLAS refits only and no builds.
- All 42 attempts passed with zero validation messages. Every group's
  refit, submission and scene-pool counters equal the baseline's, and all
  final images are byte-identical to the baseline's, across repetitions and
  between Reference and Wavefront.

## Results

Per-frame medians in milliseconds and staged bytes per frame. GPU state
was again bimodal per process, so time ranges show **same-state runs**: those
whose median GPU frame time is below 1.5× the fastest of the group's six
runs. The [data](2026-10-25-changed-section-staging.json) keeps every run's
medians, image hashes and counters, and which runs were selected.

| Workload | Staged bytes before → after | Update wall before → after | Upload GPU before → after | BLAS refit GPU |
| --- | ---: | ---: | ---: | ---: |
| `geometry-v1` | 9,449,680 → 3,158,220 | 2.80–2.96 → 2.14–2.38 | 0.36 → 0.14 | 0.45 (unchanged) |
| `textured-v1` | 89,680 → 15,948 | 0.27–0.28 → 0.27–0.29 | 0.004 → 0.001–0.002 | 0.080 |
| `instanced-v1` | 737,352 → 737,328 | 0.36–0.39 → 0.35–0.38 | 0.040 | 0.022 |
| box-based fixtures | 632–30,008, −24 each | 0.19–0.28 → 0.20–0.29 | 0.001–0.002 | 0.023–0.025 |

For `geometry-v1` the remaining bytes are its positions (3,158,028) and 192
bytes of instance and TLAS input; the 6,291,456 bytes of triangle indices
are no longer staged. In slow-state Reference processes its update wall fell
from 5.6–6.4 to 4.6 ms and upload from 1.40 to 0.54 ms. For `textured-v1`
the saved 73,732 bytes are its triangles, normals and texture coordinates,
which this deformation leaves unchanged; the other fixtures save their edited
mesh's 24 bytes of indices. Authoring and extraction costs are unchanged.

Outside `geometry-v1` the update wall did not change measurably: same-state
ranges differ by at most about 0.02 ms in either direction (for example
Wavefront Cornell 0.195–0.203 → 0.212–0.219, Reference many-lights 0.276 →
0.233–0.238), which this unlocked-clock campaign does not resolve. The added
host work, comparing the replaced sections, is part of those walls.

## Decision

**Adopt.** On the only update above 1 ms, changed-section staging removes
67% of the staged bytes, 61% of the upload GPU phase and about 0.6 ms
(20%) of the fast-state update wall, with identical images, refits and
read-back contents and no measurable cost elsewhere. No default or
interface changes: `GpuSceneStats::uploaded_bytes` still counts bytes
copied through staging, which is now smaller.

`geometry-v1` deformation still spends 2.1–2.4 ms per frame in a blocking
update whose GPU phases take 0.59 ms. The remaining host work includes the
retention check's triangle and position comparisons, the 3 MB staging copy,
submission, the fence wait and validation; it is not further attributed
here. Authoring the replacement costs another 0.73 ms outside `UpdateScene`.
These, the fixed update floor and instancer rewrites stay deferred as in the
[motion decisions](2026-10-25-motion-workloads.md#decisions).

## Limits

| Missing | Reason / next evidence |
| --- | --- |
| Normal and texture-coordinate edits on the workloads | The deformation edits positions only; changed-normal and changed-texture-coordinate sections are covered by the unit test, not by a benchmark. |
| Controlled timing | Clocks were unlocked and bimodal per process; the wall differences rest on two or fewer same-state runs per side for `geometry-v1`. |
| Host-side split | `UpdateScene` does not time comparisons, staging copies, submission and waits separately. |
| Hydra, viewport and larger edits | As in the [motion report](2026-10-25-motion-workloads.md#limits-and-remaining-evidence). |

## Verification

- `ost build --jobs auto` (core Release): passed.
- `ctest -R "compaction|scene-update|gpu-scene|benchmark"`: 10/10 passed,
  including the new staging check in `lotus-renderer-acceleration-compaction`
  and the benchmark refit checks.
- The 42 predeclared attempts passed; `build/compare-restage-20261025.py`
  compared them with the baseline. Commands, per attempt, as in the
  [build guide](../guides/BUILDING.md#camera-and-object-motion) with
  `--output build/restage-deform-<integrator>-20261025`.
- Not rerun: the full gpu and research tiers, Hydra, viewport and `ci-core`
  trees. Only `backend/vulkan/gpu_scene.cpp` changed among renderer sources.
