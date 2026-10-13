# Direct-light reservoir arithmetic

`backend/vulkan/reservoir.hpp` and `reservoir.cpp` implement an internal CPU
reference in `Lotus::vulkan_internal`, built by `Lotus::Vulkan` even with
Vulkan disabled. The header is not installed and introduces no public API,
render mode or GPU pass. The [roadmap](../roadmap/README.md#status-at-a-glance)
owns delivery status; the [phase policy](../design/ROADMAP_POLICY.md#renderer-phase-5--restir-di)
owns the intended transport scope.

## Record and operations

`Reservoir` is a trivially copyable, standard-layout, 16-byte record:

| Field | Type | Meaning |
| --- | --- | --- |
| `sample` | uint32 | Opaque caller-owned candidate key; `kNoReservoirSample` is the empty sentinel |
| `weight_sum` | float | Sum of streamed resampling weights |
| `selected_target` | float | Unnormalized target at the selected sample |
| `candidate_count` | uint32 | Number of represented draws, including zero-target draws |

For `StreamReservoirCandidate`, candidate weight is `target / proposal_pdf`.
The candidate replaces the selected sample when its supplied uniform variate
in `[0, 1)` is strictly below its fraction of the new weight sum. Zero-target
draws increase the count but never select a sample. An empty or all-zero
stream has zero weight/target and the sentinel sample.

`ReservoirNormalization` returns `weight_sum / (candidate_count * selected_target)`
in double precision, or zero for empty/all-zero streams. Multiply this by the
selected RGB contribution, with current visibility/transmittance applied to
that contribution. Target and proposal density must share a measure; the
target must be positive wherever the contribution is nonzero, and proposal
support must cover the integral. The target need not include visibility.

`MergeSameTargetReservoir` combines streams from the **same target function
and candidate domain**, carrying each source's weight sum and full candidate
count. An all-zero source still carries its draws; an empty source carries
none. Sources can have different proposals and stream lengths. The source
and destination may alias, although duplication supplies no new independent
information.

The mathematical basis is weighted reservoir sampling and resampled
importance sampling from Bitterli et al.,
[Spatiotemporal Reservoir Resampling](https://research.nvidia.com/labs/rtr/publication/bitterli2020spatiotemporal/).
Its [supplemental algorithms](https://benedikt-bitterli.me/restir/bitterli20restir-supplemental.pdf)
describe combination across different targets; that requires additional
weighting beyond this same-target helper.

## Failure and precision contract

Inputs and stored weights/targets must be finite and nonnegative; proposal
PDFs must be strictly positive. A positive-weight record requires a selected
sample, positive selected target and nonzero candidate count. Invalid input
or malformed records throw `std::invalid_argument`. Positive candidate
weights that overflow or round to zero in float storage, weight-sum overflow
and uint32 count overflow throw `std::overflow_error`. Updates validate
before publishing and leave the destination unchanged on failure.

Weights are stored as floats with ordinary rounding. Ratios, replacement
probabilities, sum range checks and normalization use double intermediates
in this CPU reference. The sum is never reused as a cached normalized weight;
normalization is computed from the current count/selected target. GPU float
precision and subnormal behavior have not been validated by these CPU checks.

## Validation and integration limits

CTest `lotus-renderer-reservoir` checks replacement boundaries, zero-target
counting, same-target combination, invalid/overflow transaction behavior,
independently enumerated RGB integrals and visibility, and fixed-seed weighted
selection frequencies ([measurement](../reports/2026-10-13-restir-reservoir.md)).
It runs without Vulkan or OpenUSD.

No scene light identity, reconstructible light payload, candidate generator,
GPU reservoir allocation, reuse, visibility query, MIS integration, timestamp,
debug AOV or Hydra setting is implemented here. The opaque sample key is valid
only inside its caller's unchanged candidate domain and is not a persistent
light-table index. Same-target combination must not be used as cross-surface
temporal/spatial reuse: changing the target or its support needs reevaluation
and the appropriate normalization. CPU discrete-integral checks establish
arithmetic behavior, not rendered convergence, speed or temporal stability.
