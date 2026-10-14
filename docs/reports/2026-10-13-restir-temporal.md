# GPU temporal direct-light reservoirs

Measured on 2026-10-13–14 against `879e879` plus LOTUS-RESTIR-03, MSVC Release,
Windows, NVIDIA RTX A5000, using the existing OpenStrata core and OpenUSD 26.08
lookdev configurations. The [reservoir reference](../reference/RESTIR.md#temporal-gpu-reuse)
owns settings, normalization, sample-domain compatibility and limits.

## CPU/GPU recursive arithmetic

`lotus-renderer-restir-gpu` checks both integrators with a 64×64 index-matched
Lambert plane (base RGB 0.5/0.75/1), two emitting parallel lights with RGB
(2,1,0.5) and (1,2,0.5), and one equal-power below-surface light with zero target.
The base sample index is 17, with two fresh candidates and a history cap of
three. Twelve consecutive frames compare each GPU result with the CPU
`MergeReevaluatedReservoir` and `ReservoirReuseNormalization` oracle, using
independent Lambert targets and the separate merge PCG stream.

Selected light/UV, represented counts and supporting counts match exactly;
weight sums and stored normalization agree within 4e-6 and selected RGB within
5e-6. Counts stay at most five. The preceding published record matches the
immutable previous product byte for byte. Five hundred all-zero observed
source streams participate per integrator, preserving their represented draws.
The final mean matches the independently enumerated RGB integral within 0.025
per channel. Both initial and reuse timestamps are positive.

Fresh 64×64 reservoir capacity is 3,538,944 bytes (864 bytes/pixel), checked
against device storage plus host mirrors. These timings and capacities are
diagnostics, not a speedup claim; exact source shading is deliberately retained
for support evaluation before later storage optimization.

## Moving targets and visibility

A second 64×64 plane uses perspective X/Y scale 100, one fresh candidate and
a one-candidate history cap. Two equal-power parallel directions
(-0.8,0,0.6) and (0.8,0,0.6) illuminate complementary supports when the plane
rotates from -60° to +60° about Y around (0,0,-4). Normal acceptance is -1 and
relative depth tolerance is one specifically to exercise differing targets.
The source has both positive and all-zero observed streams. 3,998 pixels
accept history, including 1,973 all-zero observed sources. GPU results
retain both draws but count only the current source in the support denominator
for a positive receiving sample. Their normalization agrees with the CPU
support oracle within 2e-6. Both integrators compare the resulting mean with
1,024-spp NEE/MIS within 0.025 per channel. Mean red is 0.321271 for temporal
reuse and 0.316052 for NEE/MIS (difference 0.005219, about 1.65%); this noisy
single-frame comparison does not establish arbitrary moving-history bias.

The 16×16 lifecycle fixture moves the camera and nonemissive plane by 0.25
world units, retaining history. Accepted reservoirs fetch the exact index
reported by shared reprojection, including displaced indices.

A 32×32 moving-occluder sequence retains compatible scene history while
revealing new receiver pixels. A coloured 2×2 foreground panel at z=-3 moves
1.5 units along X in front of the plane at z=-4. One parallel light travels
toward (0.8,0,0.6). Rejected pixels retain only eight fresh draws; other pixels
reuse history (80 shared rejections and 923 accepted reservoir sources).
At the same jitter/sample index, every RGBA channel agrees with
NEE/MIS within 3e-6, including the current shadow. PFM outputs are retained in
the core build's `restir-initial-images/temporal-disocclusion-*.pfm`.

## Lifecycle and numerical failure

Checks cover first frames, light edits, candidate count/cap changes, camera
cuts, frame gaps, target resize, successful ordinary-render interruption and
combined radiance/reservoir history. Reset output matches the initial-only
record byte for byte. An invalid zero-cap request publishes no products and
does not consume the preceding frame; the next valid request reuses it.
An invalid integrator with a resized target is rejected before metadata
reallocation and preserves the old target's history. Caps of 65536 succeed;
65537 fails without publishing a frame.

A separate GPU failure fixture uses one parallel light with RGB 1.6e38,
unit Lambert albedo, one initial candidate and a history cap of 64. Initial
draws remain representable, but repeated merge sums overflow float. The failed
call publishes neither reservoir nor temporal history and the renderer remains
failed. Existing initial overflow, lighting-integral and multibounce tests
continue to run under both integrators.

## Configuration verification

- `ost build --jobs auto`, `ost test`,
  `ost validate --strict-renderer-evidence`: all 26 core tests pass, including
  source/install-tree, deterministic references, GPU and synchronization checks;
  strict evidence passes. Hydra/viewport capability SKIPs are intentional here.
- `ost build --profile lookdev --intent hydra --jobs auto`,
  `ost test --profile lookdev --intent hydra`,
  `ost validate --profile lookdev --intent hydra --strict-renderer-evidence`:
  all 51 tests pass, including installed usdview, and strict evidence passes.
  After final request-validation changes, the affected temporal/ReSTIR,
  render-pass/integrator, install-tree, boundary and synchronization tests are
  rerun on the rebuilt Hydra tree.
- `ost build --without-runtime --intent ci-core --jobs auto`,
  `ost test --without-runtime --intent ci-core`: 14 tests pass and the two GPU
  checks explain their Vulkan-disabled SKIPs.
- `python scripts/check_docs.py` and
  `python -m unittest discover -s scripts -p 'test_*.py'`: documentation checks
  and all 27 Python regressions pass.

## Scope disposition

This delivers bounded temporal reuse and its inspectable arithmetic/lifecycle
evidence. Spatial reuse and Hydra controls remain in the
[execution plan](../roadmap/current.md#execution-order). Slots/UVs are reused
only across a fixed light domain; light edits and any scene edit with
table-backed emissive triangles reset conservatively. General light remapping,
arbitrary moving-history unbiasedness, many-light convergence/time advantages,
temporal stability and production artifacts require the later phase-exit
measurements. Correlated recursive draws are not independent new information.
