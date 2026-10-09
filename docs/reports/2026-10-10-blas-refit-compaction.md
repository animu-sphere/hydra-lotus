# BLAS point refits and compaction evaluation

Measured on 2026-10-10, based on `1772644` plus this change, on Windows 11
x86_64, MSVC 14.51, OpenStrata 0.23.14, Release and NVIDIA RTX A5000
(Vulkan 1.4.329). Hydra uses OpenUSD 26.08 lookdev and Python 3.13.
The current update contract is owned by
[SCENE.md](../reference/SCENE.md#acceleration-structures).

## Scene update measurement

`renderer.scene.timestamp` uses an indexed 256-by-256 quad grid (66,049
vertices, 131,072 triangles) with 1,024 placements. Insertion is followed by
a point edit (`z = 0.1 * x`), an index swap in the first triangle, a transform
edit, material binding/value edits, an unchanged commit and removal.
There is one timed submission per nonempty step, with no deliberate warmup
or averaging in this diagnostic. It follows earlier scene checks on the same
renderer. The data below is from the core CTest evidence produced by
`ost-test-34f9981692aff278`, captured here from
`build/cy2026-windows-x86_64-py313-core/renderer-ctest-report.json`.
Subsequent test runs replace that build-tree file; this table preserves the
measured session rather than claiming the newest run has identical timings.

| Step | CPU update wall ms | GPU upload ms | GPU BLAS build/refit ms | GPU TLAS build/refit ms |
| --- | ---: | ---: | ---: | ---: |
| Insertion | 1.5181 | 0.1050 | 0.4884 | 0.1438 |
| Point edit | 1.1704 | 0.1061 | 0.1609 | 0.0174 |
| Index change, same primitive count | 1.7909 | 0.1066 | 0.4917 | 0.1448 |
| Transform edit | 0.2900 | 0.0081 | 0 | 0.0163 |
| Material binding | 0.2763 | 0.0054 | 0 | 0 |
| Material value | 0.2042 | 0.0009 | 0 | 0 |
| Unchanged commit | 0.0013 | 0 | 0 | 0 |

CPU wall time includes `Commit`, extraction, validation, CPU preparation,
submission and its blocking GPU wait; it is not isolated CPU execution time.
The timestamped phases include their synchronization. BLAS/TLAS storage stays
at 8,652,544 bytes through these steps; retained TLAS capacity from the earlier
scene walk contributes to that number. Point edits increment BLAS and TLAS
update counters without builds. Index changes increment both build counters;
transform edits increment only TLAS updates. Attribute-only edits in the
upload walk change neither structure. Unchanged scenes submit nothing.
These are backend diagnostics, not the fixed multi-scene performance baseline.
No resolution, sample count or RNG seed applies to this upload/build workload.

## Compaction experiment and disposition

`lotus-renderer-acceleration-compaction` builds a separate nonindexed copy of
the same grid (393,216 vertices, 131,072 triangles), with no TLAS, rendering,
resolution or sampling. It compares fast-trace-only flags, fast trace plus
`ALLOW_UPDATE`, and fast trace plus `ALLOW_UPDATE | ALLOW_COMPACTION`.
Each mode uses one warmup build and eight measured builds of the same BLAS;
reported build times are arithmetic means. The compacted-size query and copy
are each measured once after the final build. Geometry input is host-visible,
so upload costs are excluded. The compact destination is separate storage.

The captured run is in
`build/cy2026-windows-x86_64-py313-lookdev--hydra/compaction-evaluation.log`:

| Flags | Original storage bytes | Mean GPU build ms | Mean CPU build wall ms |
| --- | ---: | ---: | ---: |
| Fast trace | 7,341,312 | 1.105936 | 1.270338 |
| Fast trace + update | 8,295,680 | 1.005252 | 1.136475 |
| Fast trace + update + compaction | 8,295,680 | 1.070364 | 1.219037 |

The queried compact destination is **7,552,896 bytes**, saving 742,784 bytes
(8.95%) against the updatable source. Size-query GPU time is 0.006304 ms,
compact-copy GPU time 0.028640 ms, and the combined query/readback/allocation/
copy wall time is 1.099200 ms. Source and destination coexist during copying,
requiring 15,848,576 bytes of AS storage before the source can be released;
allocator backing/slack and scratch are additional.

**Production compaction is not adopted.** The modest saving on this workload
comes with extra storage and two additional blocking submissions. End-to-end
traversal throughput, compacted-BLAS refit behavior and image equivalence are
not measured here, so a favorable renderer trade-off is not established.
The isolated experiment remains available for later static-scene measurements;
the production build never enables `ALLOW_COMPACTION` or copies compacted ASes.
This disposition does not claim compaction is unfavorable on every scene or
GPU. The experiment follows the Vulkan
[compaction contract](https://docs.vulkan.org/spec/latest/chapters/accelstructures.html).

## Correctness and validation

- Compatible replacements preserve their GPU slot and BLAS; point changes
  refit both BLAS and referenced TLAS bounds. The primary-ray test expands a
  visible triangle outside its original bounds and repeats the point refit,
  checking barycentrics/depth against independently projected CPU triangles.
- Geometry/normal/UV readback, instanced placements, material binding,
  topology rebuilds and unchanged-scene counters pass. Shared-source CPU tests
  ensure geometry still used by another mesh cannot be overwritten, and
  merging onto resident geometry never fabricates a replacement upload.
- Invalid correspondence, duplicate correspondence and non-finite positions
  fail before changing ownership. A direct source-address permutation refits
  both BLASes and reads back the expected contents in the retained slots.
- The compaction experiment reports zero Vulkan validation messages. The
  missing-driver CTest returns an explained SKIP (77).
- Cornell deterministic references at 1, 16, 64, 256 and 1024 spp pass without
  regenerating the committed files. Synchronization validation is enabled
  with zero messages.

## Commands and results

- `ost build --jobs auto`, `ost test`: **15/15 passed**.
- `ost validate --strict-renderer-evidence`: passed.
- `ctest --test-dir build/cy2026-windows-x86_64-py313-lookdev--hydra -R 'acceleration-compaction' -V`:
  **2/2 passed**, producing the compaction measurement above.
- Hydra was cleaned with
  `cmake --build build/cy2026-windows-x86_64-py313-lookdev--hydra --target clean`
  before rebuilding, avoiding the documented MSVC header-dependency limitation.
- `ost build --profile lookdev --intent hydra --jobs auto`,
  `ost test --profile lookdev --intent hydra`: **32/32 passed**, including
  usdview first-frame/stable-update screenshots and the BLAS/TLAS refit counters.
- `ost validate --profile lookdev --intent hydra --strict-renderer-evidence`:
  passed.
- `ost build --without-runtime --intent ci-core --jobs auto`,
  `ost test --without-runtime --intent ci-core`: **8/8 passed**.

- `python scripts/check_docs.py`: passed;
  `python -m unittest discover -s scripts -p test_check_docs.py`: **14/14 passed**.
