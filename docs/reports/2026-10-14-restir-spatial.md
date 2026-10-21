# Spatial direct-light reservoir reuse

Measured 2026-10-14 on Windows 11, MSVC Release, Vulkan SDK 1.4.350.0 / Slang,
NVIDIA RTX A5000. Source base: `279c746` plus this LOTUS-RESTIR-04 change.
`lotus-renderer-restir-gpu` exercises Reference and Wavefront through the same
backend. The [reservoir reference](../reference/RESTIR.md#spatial-gpu-reuse)
owns settings, storage, estimator behavior and limitations.

## Arithmetic and immutable sources

A 64×64 Lambert plane has three discrete parallel lights, one with zero target.
Two initial candidates and a source cap of three run for six spatial-only
frames and six spatiotemporal frames per integrator. The test independently
replays every accepted merge with the CPU differing-target oracle, including
the centre's explicitly resolved weight, and checks selection, UV, represented
counts, supporting counts, weight sum, normalization and selected RGB.
The fixed stencil agrees with independently enumerated screen neighbours;
all-zero source streams remain in represented/support counts.

CPU/GPU sums agree within 2e-5, normalization within 8e-6 and selected RGB
within 1e-5. Counts stay at most 10 without temporal reuse and 17 with it.
The preceding frame's spatial output matches `previous` byte for byte;
the temporal intermediate remains separately inspectable. Initial-only,
temporal and spatial pass timestamps are collected separately.
Fresh 64×64 spatial storage plus readback is 5,242,880 bytes (1280/pixel),
checked against the allocated buffers. Smaller targets and disabled modes
retain allocated capacity.

## Boundaries, support and visibility

Adjacent panels test equal-material cross-instance acceptance and rejection
at differing depths, geometric normals, authored shading normals and material
identities. A narrow perspective camera isolates geometric rejection from
depth/shading-normal rejection. Their single-light RGB is checked against
NEE/MIS per channel within 4e-6. Cropped targets overwrite outside records
and never accept a source outside their data window.

Two coplanar panels with shading normals differing by a cosine of 0.92 pass
the geometry gate but have complementary support for two grazing parallel
lights. One initial candidate and cap one produce five represented draws,
with four supporting draws when the cross-boundary source has zero support
for the final selection. Across 16 frames, 1856 positive boundary selections
use the corrected denominator; 3930 observed all-zero neighbours participate.
Mean red is 0.0793535 versus NEE/MIS 0.0792112. A moving tilted-plane test
also runs spatial reuse after a temporal correction: the centre's resolved
normalization survives the second stage, including complementary support.
Its red mean is 0.321340 versus NEE/MIS 0.316052, within the existing bounded
moving-support tolerance of 0.025.

Transparent, 0.75-opacity and opaque shadow blockers retain unshadowed stream
counts and produce current transmittance 1, 0.25 and 0 respectively; RGB agrees
with an independent Lambert integral within 2e-6. A moving opaque foreground
panel exercises disocclusion and current shadow changes with spatiotemporal
reuse: 80 shared correspondences reject, 923 reuse, and the final image matches
the same-jitter NEE/MIS image per channel within 3e-6. Visibility is never used
as the support-denominator test.

## Correlations and moving many-light comparison

A 32×32 Lambert plane with 32 parallel lights uses two initial candidates,
source cap eight, one bounce and one fresh sample/frame. The camera translates
0.02 units/frame. Four independent 16-frame sequences reset the base RNG index
between sequences; each mode uses matching initial sample indices. Independent
RGB integration gives `(3.88338, 5.13275, 7.00282)`; a 512-spp NEE/MIS image
gives `(3.88147, 5.13519, 7.00337)`. PFMs for all four modes and NEE/MIS are
retained under the build tree's `restir-initial-images/`.

The test records RGB mean, per-pixel RMS error to that integral, RMS difference
between consecutive frames (excluding sequence resets), total scene GPU time,
all three reservoir pass times and allocated reservoir bytes. The core run
recorded the following means (RGB/error products agree between integrators):

| Mode | Mean RGB | Pixel RMS error | Frame-difference RMS | Reservoir bytes |
| --- | --- | --- | --- | --- |
| Initial | 3.88855, 5.12226, 6.99889 | 2.33518 | 3.30457 | 98,304 |
| Temporal | 3.90792, 5.14725, 6.97218 | 2.33107 | 1.57631 | 884,736 |
| Spatial | 3.88157, 5.11187, 7.00500 | 2.32450 | 3.28252 | 1,310,720 |
| Temporal + spatial | 3.92575, 5.07988, 6.98150 | 2.32426 | 2.77226 | 1,310,720 |

Maximum relative channel-mean discrepancy is 1.1%; the independent-integral
regression tolerance is 2.5%. Spatial-only pixel RMS error is only 0.5% below
initial, and adding spatial to temporal increases frame differences in this
fixture. The same accepted source data and resolved RGB are used by both
integrators; the table is not independent evidence from two stochastic runs.

| Integrator / mode | Total scene GPU ms | Initial ms | Shading/temporal ms | Spatial ms |
| --- | --- | --- | --- | --- |
| Reference / initial | 0.1684 | 0.0897 | 0 | 0 |
| Reference / temporal | 0.5916 | 0.0879 | 0.2661 | 0 |
| Reference / spatial | 0.4365 | 0.0661 | 0.0646 | 0.0914 |
| Reference / temporal + spatial | 0.4835 | 0.0685 | 0.1126 | 0.0916 |
| Wavefront / initial | 0.3072 | 0.0882 | 0 | 0 |
| Wavefront / temporal | 0.7364 | 0.0888 | 0.2245 | 0 |
| Wavefront / spatial | 0.6192 | 0.0775 | 0.0743 | 0.1034 |
| Wavefront / temporal + spatial | 0.7932 | 0.0766 | 0.2132 | 0.1517 |

The reported total already includes shared metadata and reservoir passes;
readback is excluded. No many-light speedup was observed on this workload.

The common light direction deliberately isolates estimator noise from scene
motion. Similar per-pixel RMS errors across modes do not demonstrate faster
convergence. Reduced frame differences can reflect shared correlated samples,
not new independent information. Spatial merges sample a single final light,
not an average of neighbour RGBs. Timings are small-workload observations with
validation enabled, no fixed-clock or speedup claim. This comparison does not
establish production artifact/stability bounds or phase-exit quality per time.

## Lifecycle and failure

Zero/oversized radius and source caps, wrong estimator/output, multiple fresh
samples and ordinary-render requests reject without publishing products or
consuming previous history. Subsequent valid calls still read the same final
spatial history. Radius and mode edits reset as `RenderSettingsChanged`.
A radius larger than the target copies the input byte for byte.

The spatial overflow fixture has a unit-albedo plane and a parallel light with
RGB 2.4e38, one fresh candidate and no temporal reuse. Initial and shading
records remain finite; summing five spatial streams exceeds float storage.
The call fails specifically at spatial arithmetic, publishes neither reservoir
nor temporal products and leaves renderer failure latched.

## Configuration verification

- `ost build --jobs auto`, `ost test` and
  `ost validate --strict-renderer-evidence`: 26/26 core tests pass; strict
  evidence passes, including installed rendering, deterministic references,
  missing-driver regressions and synchronization validation.
- `ost build --profile lookdev --intent hydra --jobs auto`,
  `ost test --profile lookdev --intent hydra` and
  `ost validate --profile lookdev --intent hydra --strict-renderer-evidence`:
  51/51 tests and strict evidence pass, including installed usdview moving
  sequences and both integrators' spatial GPU checks. The Hydra tree was
  cleaned before rebuilding to avoid stale public-header objects on MSVC.
- `ost build --without-runtime --intent ci-core --jobs auto` and
  `ost test --without-runtime --intent ci-core`: 14 CPU tests pass; temporal
  and ReSTIR GPU tests explain their Vulkan-disabled SKIPs.
- `python scripts/check_docs.py` and
  `python -m unittest discover -s scripts -p 'test_*.py'`: documentation
  checks and 27 Python regressions pass.

## Scope disposition

LOTUS-RESTIR-04 delivers a single, bounded, inspectable spatial pass under both
integrators. The power-alias proposal and four-axis stencil are retained for
measurement. General light-domain remapping, candidate-strategy comparison,
Hydra settings/diagnostics and broad convergence, memory, time and artifact
evaluation remain in LOTUS-RESTIR-05, the
[execution plan](../archive/pre-v070-execution-plan.md#execution-order). Phase 5 remains in
progress; these fixtures do not declare its exit.
