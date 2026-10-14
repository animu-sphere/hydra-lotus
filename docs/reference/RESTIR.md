# Direct-light reservoirs

`backend/vulkan/reservoir.hpp` and `reservoir.cpp` implement an internal CPU
reference in `Lotus::vulkan_internal`, built by `Lotus::Vulkan` even with
Vulkan disabled. The header is not installed. GPU float arithmetic and
initial and temporal direct lighting are in `shaders/sampling/reservoir.slang` and
`shaders/common/restir.slang`. The [roadmap](../roadmap/README.md#status-at-a-glance)
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
normalization is computed from the current count/selected target. The GPU
implementation uses float intermediates; its near-boundary decisions can
differ. GPU subnormal flush behavior and extreme dynamic-range robustness
are not established by ordinary lighting comparisons.

## Validation and integration limits

CTest `lotus-renderer-reservoir` checks replacement boundaries, zero-target
counting, same-target combination, invalid/overflow transaction behavior,
independently enumerated RGB integrals and visibility, and fixed-seed weighted
selection frequencies ([measurement](../reports/2026-10-13-restir-reservoir.md)).
It runs without Vulkan or OpenUSD.

The CPU helper's opaque sample key is valid only inside its caller's unchanged
domain. Same-target combination must not be used as cross-surface reuse.
The CPU differing-target operations below provide the arithmetic oracle;
GPU integration and its limits are described afterward.

## Differing-target CPU combination

`MergeReevaluatedReservoir` streams a source's selected sample after the caller
reevaluates its target at the receiving surface. Its resampling weight is
`receiver_target * source_normalization * m`, where
`m = min(source.candidate_count, max_source_candidates)`. It adds `m` to the
destination's represented count even if the sample has zero receiving target
or the observed source stream is all-zero. An empty source adds zero. The
cap is nonzero; scaling both count and merge weight preserves the source's
resolved weight. The source is not modified, and self-merge is supported.

The caller supplies the source's resolved normalization `W`. For initial
streams this is `ReservoirNormalization(source)`; after differing-target reuse
it must be the previously resolved, support-corrected weight, stored separately
from the raw 16-byte record. Reconstructing it from the raw sum and total count
would lose that correction. This CPU API accepts double `W`, including values
outside float range when the receiving merge weight remains representable.

After selection finishes, evaluate the **final selected sample** at every
participating source surface. Sum the capped represented counts only where
that source target is positive, and supply this count to
`ReservoirReuseNormalization`. Its result is
`destination.weight_sum / (supporting_candidate_count * destination.selected_target)`.
An all-zero observed source still participates in this support evaluation:
its draws could have produced a different sample. The denominator is neither
the number of source reservoirs nor the total represented count when source
supports differ. A positive result requires a supporting count in
`[1, destination.candidate_count]`; zero records return zero. With identical
support it reduces to ordinary total-count normalization.

These primitives implement count-weighted support correction over a fixed
sample domain and measure. Each source's resolved estimator must be valid on
its declared target support, proposals must cover that support, and the union
of participating supports must cover the receiving contribution. The caller
owns reconstructible sample identity, source target evaluation and the
associated payload. Visibility may multiply the final receiving contribution;
an unshadowed source target must not be replaced with a visibility test for
the support count. Correlated copies do not add independent information.
For general target-weighted MIS alternatives, see the
[ReSTIR supplemental algorithms](https://benedikt-bitterli.me/restir/bitterli20restir-supplemental.pdf).

Invalid reevaluations, normalizations, caps or records throw
`std::invalid_argument`; unrepresentable receiving weights/sums or total count
overflow throw `std::overflow_error`. Failed merges preserve the destination,
including when it aliases the source. An empty/all-zero source requires zero
receiving target and zero resolved weight. A positive source requires finite,
positive resolved weight. The CPU helpers contain no temporal state and do
not validate the caller's scene identity, target functions or support union.

`lotus-renderer-reservoir` exhaustively compares differing-target RGB and
visibility expectations with independent discrete integrals, including
unequal streams, count caps, complementary support, zero observed streams,
receiving zero targets and reversed merge order. It also checks preservation
of previously corrected weights and numerical failure
([evidence](../reports/2026-10-13-restir-reuse-arithmetic.md)).
This establishes a CPU arithmetic oracle, not GPU temporal/spatial reuse or
unbiased moving-frame history selection.

## Initial GPU lighting

Set `PathTracingSettings::light_sampling = LightSampling::RestirInitial` for
opt-in primary-surface direct lighting under either integrator.
`restir_candidates` defaults to 8 and accepts 1–64. `Mis` remains the default.
The headless benchmark CLI and Hydra controls still expose `Mis` and `Bsdf`
only. Later bounces retain NEE/MIS.

A separate fullscreen pass traces the same jittered camera ray and primary
opacity realization as transport. It streams candidates, reconstructs the
selected sample, queries its shadow transmittance and stores normalized RGB.
Both integrators read this result at their first scattering event.

The payload is a light-table slot plus two unit-square variates. The existing
power alias table selects the light. Its probability is proposal `q` in the
light's **unit-square parameter measure**, with unit conditional density:

| Light | Fixed mapping and Jacobian |
| --- | --- |
| Emissive triangle | Uniform barycentrics; world area times emitter cosine / distance squared, with textured emission and coverage, two-sided |
| Rect / disk | Uniform world area with the same area-to-solid-angle conversion and emitting-side cosine |
| Sphere | Uniform whole surface, area `4πr²`; back-facing points have zero target |
| Positive-angle distant | Fixed cone map; Jacobian is its solid angle |
| Parallel distant | Discrete direction carrying irradiance; Jacobian one |
| Environment image | Fixed inverse row/column CDF map; Jacobian is inverse conditional solid-angle density |

The parameter-space RGB integrand is
`BSDF_eval_cos * radiance * (selection / sample.pdf)`; target is maximum RGB,
covering every positive channel. Weight is target divided by selection
probability. Zero-target/back-facing draws count toward M. Visibility is
applied only after selection, including fractional opacity transmittance.
Sphere candidates use fixed area points, avoiding a receiver-dependent cone
payload. These mappings can be reevaluated at another receiver, but no
cross-surface normalization or reuse occurs in initial-only mode.

Candidate RNG uses `SeedRandom(pixel, sample_index ^ 0xD1B54A35)` and five
variates per draw (alias selection, payload, replacement). Primary coverage
and continuation BSDF draws retain their own sequence.

### Emission accounting

Primary camera-visible emission is unchanged. At depth one, transport suppresses
BSDF-found emission from table-backed triangles, analytic crossings and the
sampled environment image: the reservoir already estimates those direct-light
integrals. This partitions estimators without assuming a reservoir PDF for MIS.
Unsampled surface emission and the constant environment remain BSDF sampled.
Later bounces use existing NEE/MIS emission weights. `max_bounces = 0` adds only
camera-visible emission and generates no candidates.

### Products, memory and lifecycle

`GpuFrameEvidence::restir` is available on a successful Radiance call adding
at least one sample. It contains the last fresh sample's tightly packed,
top-left rows of 48-byte `RestirPixel` records:

| Fields | Meaning |
| --- | --- |
| `light`, `weight_sum`, `selected_target`, `candidate_count` | Same first 16-byte layout as CPU Reservoir |
| `uv`, `proposal` | Selected reconstruction variates and selection probability |
| `flags` | Bit 0: camera surface; bit 1: invalid/unrepresentable arithmetic |
| `contribution.xyz` | Selected, normalized, shadowed RGB direct light |
| `contribution.w` | Selected transmittance, zero for no selected sample |

Misses, empty scenes and pixels outside the data window overwrite records with
the sentinel and zero count/weights/contribution. A hit with no lights represents
zero draws; an all-zero stream with lights retains the requested count. Calls
adding no sample publish no reservoir products or timing. There is no reservoir
history.

`scene_generation` accompanies the products. Slots are valid only for that
unchanged table, never a persistent light identity. Scene, camera, target,
integrator, estimator and candidate-count changes restart progressive radiance.
Candidate-count changes also reset temporal radiance compatibility. Reservoir
state stays outside shared temporal metadata/validity.

Persistent device storage and its host readback mirror initially total 96 bytes
per pixel. Capacity is retained across smaller targets and mode switches;
`reservoir_bytes` reports it separately from scene-memory stats. Storage-range
limits reject before dispatch. A separate timestamp pair reports
`initial_gpu_ms`, covering primary tracing, candidates and selected visibility,
excluding readback. It is included in total scene duration; do not add it to
that total. Unavailable timestamps report false/zero.

GPU updates reject invalid/unrepresentable arithmetic without changing the
reservoir state. Candidate, normalization or resolved-RGB failure flags make
the call fail, with errors latched across a progressive batch. Failed GPU calls
publish no successful reservoir product or temporal history and leave the
renderer failed.

### GPU validation and limits

`lotus-renderer-restir-gpu` checks CPU/GPU records and payloads, independent
lighting integrals, fractional/opaque visibility, converged NEE/MIS images,
multibounce transport, candidate limits, resets, crops, empty scenes and
overflow failure under both integrators
([GPU evidence](../reports/2026-10-13-restir-initial.md)).
Vulkan-disabled and missing-driver configurations explain their GPU SKIPs.

Spatial reuse, candidate strategy evaluation and Hydra reservoir settings/AOVs
remain outside this implementation. Initial-only correctness checks establish
no many-light speedup, temporal stability or reuse bias result.

## Temporal GPU reuse

Set `PathTracingSettings::restir_temporal = true` with `RestirInitial` and call
`RenderTemporalScene` with `frame_count = 1`. Each successful call traces one
fresh sample, advancing the RNG independently of retained reservoir count.
`restir_max_history_candidates` defaults to 32 and accepts 1–65536. The current
initial stream contributes its full count; the previous stream contributes
`min(previous.M, cap)` to both merge weight and represented count. The total
therefore stays at most `restir_candidates + cap`. Ordinary `RenderScene`
Radiance calls reject reuse requests. Radiance accumulation is independently selectable;
switching it on/off resets compatibility to avoid reading uninitialized
radiance history.

The shared primary metadata and validity passes run before reservoir generation
and transport. The receiving sample must match its pixel-centre metadata's
instance/material; the reprojected jittered source must have the same identity.
Misses, disocclusion, excessive motion and shared depth/normal/material/instance
rejections fall back to the fresh initial estimator. No reservoir state or
policy enters the shared temporal infrastructure.

A separate fullscreen reuse pass reads immutable preceding history and writes
separate current storage. It retains each exact jittered surface's world
origin, geometric/shading normals, outgoing direction and evaluated BSDF.
The selected old light slot/UV is reevaluated at the current surface. After
selection, the final sample is evaluated at the old surface too; its capped
count enters the denominator only when that old target is positive, including
when the old observed stream was all-zero. The current stream always covers
the selected receiving target. This implements the CPU oracle's count-weighted
support correction using GPU floats. The resolved normalization is stored
separately and carried into later merges. Visibility is queried again at the
current receiver and never substituted for unshadowed support evaluation.

### Compatibility and publication

Camera and nonemissive rigid-placement motion retain compatible history.
Geometry, visibility, bindings, materials, textures, constant/environment-map
lighting and analytic light edits reset it as `SceneChanged`. For now, **any
scene edit with table-backed emissive triangles** resets reservoir history
conservatively, including a nonemissive placement edit in that scene. Camera-only
motion still reuses it. This keeps the light-slot/UV domain and reconstruction
mapping fixed for every accepted history source: slots are generation-local,
not persistent light IDs. No light-edit remapping is attempted.

Integrator, estimator, candidate count/cap, base RNG index, bounce/sample limits,
radiance mode/history cap and background changes reset as
`RenderSettingsChanged`. Frame gaps, target/window changes, explicit history
keys, camera cuts and successful ordinary renders retain the shared reset
policy. Same-count instancer reordering still requires a caller cut/key change.
Only successful submission, readback and timestamp collection publish the
current histories and rotate buffers. Invalid requests publish nothing and
do not consume a frame; GPU arithmetic failure latches renderer failure and
publishes no reservoir or temporal products.

### Products and costs

`restir.initial` remains the fresh 48-byte candidate product. `restir.temporal`
and, when available, `restir.previous` carry 64-byte `RestirReusePixel` records:
the resolved `RestirPixel`, explicit `normalization`, `supporting_candidates`,
`previous_index` (UINT32_MAX for no participating history) and selected sample
`age` (bounded at 65536). They use the same tightly packed top-left ordering;
`temporal_row_pitch` is `width * sizeof(RestirReusePixel)`. `history_available`
means a compatible preceding frame exists, not that every pixel accepted it.

Private GPU history records are 192 bytes including source shading data.
Two device buffers and a two-frame readback mirror add 768 bytes/pixel; with
initial storage/mirror, fresh capacity totals 864 bytes/pixel. Allocated
capacity remains retained across smaller targets and mode switches and is
included in `reservoir_bytes`. Shared temporal buffers are reported separately.
Storage-range limits reject before dispatch. `temporal_gpu_ms` times only the
reuse pass (primary trace, reevaluation, support and selected visibility),
excluding readback. Shared metadata runs once per frame; its own timestamps
remain in `temporal`. These pretransport passes are included in the scene
duration; do not add them to that total.

### Validation and limits

`lotus-renderer-restir-gpu` checks repeated GPU/CPU merges and corrected weights,
zero observed streams, caps, complementary support, camera/rigid motion,
disocclusion and current shadows against NEE/MIS under both integrators. It
checks resets, rejected requests and GPU merge overflow without history
publication ([measurement](../reports/2026-10-13-restir-temporal.md)). Existing
initial lighting, multibounce and temporal-radiance regressions remain active.

These bounded fixtures do not establish unbiased arbitrary moving-history
selection, many-light speedup or production temporal stability. Correlated
recursive samples add no independent draws; nearest-pixel reprojection and
geometric rejection can affect history selection. Visibility is always current,
but indirect radiance reuse keeps its separate limitations. Spatial reuse,
general light-domain remapping and Hydra reservoir controls remain later work.
