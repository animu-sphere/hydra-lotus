# Direct-light reservoirs

`backend/vulkan/reservoir.hpp` and `reservoir.cpp` implement an internal CPU
reference in `Lotus::vulkan_internal`, built by `Lotus::Vulkan` even with
Vulkan disabled. The header is not installed. GPU float arithmetic and
initial, temporal and spatial direct lighting are in `shaders/sampling/reservoir.slang` and
`shaders/common/restir.slang`. The [roadmap](../roadmap/README.md#status-at-a-glance)
owns delivery status; the [phase policy](../design/ROADMAP_POLICY.md#renderer-phase-5--restir-di)
owns the intended transport scope.

## When to use reservoirs

ReSTIR is opt-in. **Progressive NEE/MIS is the default renderer path** under
both integrators, headless and through Hydra, and the comparison baseline.
The [Renderer Phase 5 exit review](../reports/2026-10-17-renderer-phase5-exit.md)
records the decision per workload, from the
[reuse-cost measurements](../reports/2026-10-17-restir-reuse-cost.md) at locked
GPU clocks:

| Mode | Use |
| --- | --- |
| NEE/MIS | Default. With area lights, an environment, glossy materials or deep interreflection no reservoir mode reaches equal quality faster. |
| Initial-only, eight candidates | For many lights of similar importance that BSDF sampling cannot find: about 1.4 times faster to equal quality with many distant lights, accumulating and in motion; 96 bytes per pixel, no history, deterministic progressive accumulation. Slower than NEE/MIS (0.82–0.97) elsewhere. |
| Initial-only, one candidate | Not worth using; never beats NEE/MIS. |
| Temporal, combined | Not worth using with accumulation: correlated frames accumulate 0.03–0.74 as efficiently as NEE/MIS. Cuts raw flicker only where reservoirs already win; its value as denoiser input is a Renderer Phase 6 question. |
| Spatial (radius 1) | Not worth using: lower error per frame, but too costly (0.49–0.89 static, at most 1.02 in motion). |

Initial-only sampling supports deterministic progressive accumulation.
Temporal/spatial reuse uses fresh batches and bounded, correlated histories;
represented candidate counts are not counts of independent samples. Reuse
frames through `RenderTemporalScene` read back and check their evidence
products, 28–36 ms of wall time at 128x128. A target with `read_back` false
leaves them on the GPU: presented in `lotus-viewport`, reuse frames take
0.6–0.7 ms at 128x128 and 4–6 ms at 1280x720
([measurement](../reports/2026-10-18-viewport-controls.md#presenting-path-cost)).

No bias, ghosting or localized error attributable to reuse was found in the
bounded fixtures: the [static penalty](../reports/2026-10-16-restir-sequence-ensemble.md)
is temporal correlation, not detectable bias;
[four moving suites](../reports/2026-10-17-restir-moving-bias.md), a
[closed interreflecting room](../reports/2026-10-17-restir-interreflection.md)
and [light/material animation](../reports/2026-10-17-restir-animation.md) show
no reuse error distinguishable from non-reusing estimators, with narrow glossy
highlights limited by rare-event variance; the
[reset-contract tests](../reports/2026-10-17-restir-reset-contract.md) cover
emissive-state edits and Hydra, identically under both integrators. Reuse
noise stays correlated across resets (see
[compatibility](#compatibility-and-publication)). Finite references, bounded
fixtures and shared integrator draws limit these conclusions, and the exit
review lists the known bad cases and unsupported conditions. ReSTIR DI gains no
further fixtures ([stop rule](../design/ROADMAP_POLICY.md#renderer-phase-5--restir-di)).

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
The fixed headless benchmark CLI exposes `Mis` and `Bsdf`; the separate
[reservoir comparison](#headless-reservoir-comparison) measures all four
reservoir modes. The [Hydra controls](#hydra-controls-and-diagnostics) also
expose reservoirs.
Later bounces retain NEE/MIS.

A separate fullscreen pass traces the same jittered camera ray and primary
opacity realization as transport. It streams candidates, reconstructs the
selected sample, queries its shadow transmittance and stores normalized RGB.
Both integrators read this result at their first scattering event.

The payload is a light-table slot plus two unit-square variates. The core
`PathTracingSettings::restir_proposal` selects `RestirProposal::Power` (default),
`Uniform`, or `PowerUniform` (an equal mixture). For N table slots with existing
power probability p, the marginal proposal q is respectively p, 1/N, or
`0.5 * (p + 1/N)`. Uniform selection covers the existing table, including
zero-power slots retained for emissive-triangle indexing; it does not add lights
excluded during table construction. Invalid enum values reject before dispatch.
Hydra retains the default power proposal; alternatives are exposed through the
core and headless comparison.

Proposal q is in the light's **unit-square parameter measure**, with unit
conditional density. Reconstruction keeps the existing power-selection factor
p in `sample.pdf`; cancelling p yields the same mapping Jacobian and target
under every proposal:

| Light | Fixed mapping and Jacobian |
| --- | --- |
| Emissive triangle | Uniform barycentrics; world area times emitter cosine / distance squared, with textured emission and coverage, two-sided |
| Rect / disk | Uniform world area with the same area-to-solid-angle conversion and emitting-side cosine |
| Sphere | Uniform whole surface, area `4πr²`; back-facing points have zero target |
| Positive-angle distant | Fixed cone map; Jacobian is its solid angle |
| Parallel distant | Discrete direction carrying irradiance; Jacobian one |
| Environment image | Fixed inverse row/column CDF map; Jacobian is inverse conditional solid-angle density |

The parameter-space RGB integrand is
`BSDF_eval_cos * radiance * (p / sample.pdf)`; target is maximum RGB,
covering every positive channel. Weight is target divided by selection
probability q. Mixture draws use the full marginal density, irrespective of
which component selected the light. Zero-target/back-facing draws count toward M. Visibility is
applied only after selection, including fractional opacity transmittance.
Sphere candidates use fixed area points, avoiding a receiver-dependent cone
payload. These mappings can be reevaluated at another receiver, but no
cross-surface normalization or reuse occurs in initial-only mode.

Candidate RNG uses `SeedRandom(pixel, sample_index ^ 0xD1B54A35)` and five
variates per draw (alias selection, payload, replacement). Primary coverage
and continuation BSDF draws retain their own sequence.
Uniform and mixture selection consume the same five variates. The mixture
uses the second selection variate to choose its component, remapping its upper
half for the power alias threshold. Default power draws remain unchanged.

### Emission accounting

Primary camera-visible emission is unchanged. At the first scattering event,
the final reservoir's shadowed estimate is multiplied by the fixed partition
`w_L = p_L^2 / (p_L^2 + p_B^2)`. Here `p_L` is the existing **power-table
NEE solid-angle density** at this receiver and `p_B` is its BSDF density.
Delta lights receive weight one. This density is independent of candidate
proposal, candidate count and reuse mode; it is not the unknown marginal
density of the selected reservoir sample.

At depth one, BSDF-found table-backed triangle emission, analytic crossings
and the environment image retain the complementary existing NEE/MIS weight
`w_B = p_B^2 / (p_B^2 + p_L^2)`. The two estimators integrate `w_L * f` and
`w_B * f`, whose sum is `f`. Reservoir targets, represented counts, support
correction and normalization still use the full unpartitioned integrand.
The receiver's partition is applied after selection and reuse, so source
view-dependent partitions are never carried as source targets. No selected
reservoir PDF is assumed, and correlated copies add no independent draws.

Sphere reservoir samples still use whole-area points, but their partition
density uses the power-selected sphere **cone** density, matching BSDF
crossings rather than the reservoir area mapping. Other light mappings
already reconstruct the matching NEE density. Unsampled surface emission and
the constant environment remain BSDF sampled. Later bounces use existing
NEE/MIS. `max_bounces = 0` adds only camera-visible emission and generates no
candidates. This fixed partition can reduce narrow-glossy variance; it does
not establish optimal reservoir/BSDF weights or an equal-quality speedup.
Transport reconstructs the final selected light once more to evaluate this
partition, without another shadow query, and restores the BSDF ray's existing
analytic-light crossing loop. These costs belong in scene GPU measurements.

### Products, memory and lifecycle

`GpuFrameEvidence::restir` is available on a successful Radiance call adding
at least one sample. It contains the last fresh sample's tightly packed,
top-left rows of 48-byte `RestirPixel` records:

| Fields | Meaning |
| --- | --- |
| `light`, `weight_sum`, `selected_target`, `candidate_count` | Same first 16-byte layout as CPU Reservoir |
| `uv`, `proposal` | Selected reconstruction variates and selection probability |
| `flags` | Bit 0: camera surface; bit 1: invalid/unrepresentable arithmetic |
| `contribution.xyz` | Selected, normalized, shadowed RGB direct light before the receiver's BSDF partition |
| `contribution.w` | Selected transmittance, zero for no selected sample |

Misses, empty scenes and pixels outside the data window overwrite records with
the sentinel and zero count/weights/contribution. A hit with no lights represents
zero draws; an all-zero stream with lights retains the requested count. Calls
adding no sample publish no reservoir products or timing. There is no reservoir
history.

`scene_generation` accompanies the products. Slots are valid only for that
unchanged table, never a persistent light identity. Scene, camera, target,
integrator, estimator, proposal and candidate-count changes restart progressive radiance.
Proposal and candidate-count changes also reset temporal radiance and reservoir
history compatibility. Reservoir
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

CPU/GPU proposal weights, mixed-light Jacobians, independent integrals and
spatial/temporal reuse are checked under all three proposals
([proposal evidence](../reports/2026-10-15-restir-proposals.md)).
Initial-only correctness checks establish
no many-light speedup, temporal stability or reuse bias result.

## Headless reservoir comparison

`lotus-headless --benchmark-restir <json-path>` uses
`adapters/headless/benchmarks/restir_benchmark.cpp` to compare NEE/MIS, initial-only,
temporal, spatial and spatiotemporal reservoirs under the selected integrator.
It writes `lotus-restir-benchmark-v1` JSON and a sibling `<report-stem>-images/`
directory of linear RGB PFMs. It is separate from the fixed benchmark and
OpenStrata renderer correctness evidence. Exit codes are 0 for a completed
measurement, 77 for explained unavailable Vulkan/ray-query capability, 1 for
measurement/output failures and 2 for invalid CLI input. A completed run
does not assert a quality or speed threshold. Invalid arguments leave existing
reports and images untouched. The JSON is published from a complete temporary
file; measurement failures carry `fail`, with only completed scenes retained.

The versioned workloads have a Lambert material (`ior = 1`, RGB 0.5/0.75/1),
a perspective view of a two-triangle plane at Z = -4, and parallel distant
lights distributed over positive-Z directions using golden-angle azimuths.
Light RGBs vary by index; irradiance scales by `32 / light_count`.
`many-distant-plane-v1` is static and has an independently enumerated Lambert
RGB integral. `many-distant-moving-shadow-v1` adds a shaded opaque two-triangle
panel at Z = -2.5: its X translation is `0.7 sin(phase + 0.3)` and camera view
X translation is `0.12 sin(phase)`, with `phase = 2 pi frame / frames`.
By default both workloads use one scattering bounce to isolate direct lighting.

`--restir-workload mixed-boundaries` selects a second pair:
`mixed-area-environment-boundaries-v1` and
`mixed-area-environment-moving-boundaries-v1`. A red Lambert left panel
at Z = -4 meets a blue GGX metal right panel at X = 0. The right panel
starts at Z = -3.6 and slopes away with `z = -3.6 - 0.25 x`, providing a
depth and geometric-normal step as well as a material boundary. The metal
has roughness 0.35 and RGB F0 0.06/0.3/0.85; the Lambert albedo is
0.8/0.12/0.04. The moving case adds a 0.9-wide panel and the same camera
and blocker motion as the distant fixture.

These scenes alternate rect and disk lights in a radius-3 golden-angle
disk at Z = -1, facing -Z. Rect sides are 0.6 and disk radii 0.3, with the
same indexed colours scaled by `96 / light_count`. A procedural 16x8 HDR
lat-long environment adds broad colour variation and a bright four-texel
patch. It is generated in code and needs no external asset. Both scenes
default to one scattering bounce; light/material animation and production
multibounce coverage are outside this fixture.

`--restir-workload glossy-disocclusion` selects
`glossy-area-environment-static-v1` and
`glossy-area-environment-camera-disocclusion-v1`. They use the mixed fixture's
geometry, colours and environment, with metal roughness 0.08. Rect sides
are 0.12, disk radii 0.06 and radiance scales by `2400 / light_count`,
preserving the original area lights' emitted power. Both scenes contain the
fixed foreground panel at Z = -2.5. In the moving scene only the camera
changes: its view is `rotationY(0.16 sin(phase)) * translationX(1.25 sin(phase))`.
Large parallax and changing view direction exercise disocclusion and narrow
glossy highlights while geometry, materials and light domains stay fixed.
The static camera is the moving sequence's first pose. These scenes default to one
bounce and finite NEE/MIS references; neither has an exact integral.

`--restir-workload glossy-spheres` selects
`glossy-sphere-environment-static-v1` and
`glossy-sphere-environment-camera-disocclusion-v1`. They retain the fixed
foreground, procedural environment and camera-only motion of the narrow-glossy
pair, but the left panel is a roughness-0.18 dielectric with IOR 1.5 and the
right panel is roughness-0.5 metal with slope `z = -3.6 - 0.8 x`. The red base
colour and blue metal F0 are unchanged. Lights alternate 0.6-sided rects and
radius-0.35 spheres in the same radius-3 disk at Z = -1. Indexed radiance scales
by `96 / light_count`; this is not a power-matched comparison with the other
fixtures. This pair exercises a diffuse/specular dielectric mixture, broader
metal lobes, more oblique receiver normals and the sphere's distinct
reservoir-area/NEE-cone mappings. Both scenes default to one scattering bounce and
finite NEE/MIS references. The static camera matches the first moving pose.

`--restir-workload interreflection` selects `interreflection-room-static-v1`
and `interreflection-room-moving-block-v1`: a closed room spanning X and Y in
[-2, 2] and Z in [-7, 1] around the camera, with albedo-0.8 white floor,
ceiling, back and front walls, a red (0.8, 0.15, 0.1) left wall and a green
(0.15, 0.7, 0.2) right wall. A white Lambert block stands at X in [-1.3, -0.3],
Z in [-5.5, -4.5], and a roughness-0.3 metal block with F0 (0.9, 0.75, 0.5) at
X in [0.4, 1.4], Z in [-4.2, -3.2]. Lights cycle rect (0.24 sides), disk and
sphere (radius 0.12) in a radius-1.5 golden-angle disk at Y = 1.85 around
Z = -4, rects and disks facing the floor, with indexed radiance scaled by
`480 / light_count`. No path escapes, so indirect light is a large share of
each pixel at depth: run it with `--restir-bounces` above one. In the moving
scene the camera translates by `0.3 sin(phase)` in X and the metal block by
`0.8 (sin 0.3 - sin(phase + 0.3))`; this nonemissive rigid placement retains
reservoir history. The static scene is the first moving pose. Both use finite
NEE/MIS references and fixed bands `center-boundary-band-v1` and
`left-red-wall-band-v1` (the leftmost quarter).

`--restir-workload animation` selects `animated-area-environment-static-v1`
and `animated-area-environment-lights-materials-v1`. They use the
mixed-boundaries geometry, rect/disk lights and procedural environment, with
its fixed foreground panel and a fixed camera. The moving scene edits lights and
materials every `step = max(2, floor(frames / 8))` frames, at
`phase = 2 pi frame / frames`: the light disk rotates by `0.6 sin(phase)`, light
`i`'s radiance scales by `1 + 0.75 sin(phase) cos(1.7 i)`, the metal roughness
becomes `0.35 + 0.2 sin(phase)` and the Lambert green becomes
`0.12 + 0.15 (1 - cos(phase))`. Frames between edits are unchanged. Analytic
light and material edits [reset reservoir and shared temporal history](#compatibility-and-publication),
so each edit is a reset and the following frames rebuild history; the
benchmark requires both exactly. The static scene is the first moving pose. Both
default to one bounce, use finite NEE/MIS references and report
`center-boundary-band-v1` and `right-glossy-band-v1` (the metal side).

Each scene identifies its `error_reference`: the static distant plane uses
`independent-lambert-integral`; moving scenes and the mixed/glossy static scenes
use `finite-spp-nee-mis`, with null exact-integral fields. Their static convergence
is measured against the saved reference image, retaining its finite sample
noise. Mixed cases additionally report `regions` with ensemble RMS, signed
RGB means and pixel count for `center-boundary-band-v1`: all rows and columns
`[floor(3 width/8), floor(5 width/8))`. This fixed central quarter covers the
material/depth step and some moving disocclusion, rather than classifying
every boundary pixel. Glossy and animated cases retain this band and add
`right-glossy-band-v1`, columns `[floor(5 width/8), floor(7 width/8))` over all
rows. These are fixed screen bands, not material or disocclusion masks, and
camera motion can move surfaces across them. Distant cases have an empty
region array. The sphere pair also reports `left-dielectric-band-v1`, columns
`[floor(width/8), floor(3 width/8))` over all rows, with the same screen-space
interpretation and error metrics.

Defaults and accepted CLI ranges:

| Option | Default | Accepted |
| --- | --- | --- |
| `--benchmark-label` | empty | build identity string |
| `--benchmark-integrator` | `reference` | `reference`, `wavefront` |
| `--benchmark-size` | 64 | square side 16–256 |
| `--benchmark-frames` | 32 | 2–256 |
| `--benchmark-warmup` | 4 | 0–64 |
| `--restir-sequences` | 4 | 2–64 independent sequences |
| `--restir-seed` | 17 | 0–8388607; full measured range must end below warmup |
| `--restir-reference-seed` | 16777216 | 16777216–1073741823; base index for the finite reference |
| `--restir-bounces` | 1 | 1–64 scattering events, shared by measured and reference rendering |
| `--restir-workload` | `distant` | `distant`, `mixed-boundaries`, `glossy-disocclusion`, `glossy-spheres`, `interreflection`, `animation` |
| `--restir-lights` | 128 | 2–4096 |
| `--restir-reference-samples` | 256 | 16–4096 per reference half |
| `--restir-candidates` | `1,2,8` | distinct comma-separated counts, each 1–64 |
| `--restir-proposal` | `power-alias` | `power-alias`, `uniform`, `power-uniform` |
| `--restir-history-cap` | 32 | 1–65536 |
| `--restir-radius` | 1 | 1–32 |

Each measured case owns a fresh renderer, so retained reservoir capacity from
other modes cannot contaminate its memory measurement. Warmup has a separate
RNG range and no measured history is inherited. Each sequence starts at
`seed + sequence * 65537`; its frame `f` uses the next sample index. Measured
indices must be below `2^23`, where the warmup range begins; overlapping
settings reject before writing outputs. Reference indices begin at the selected
reference seed, at least `2^24`; its upper bound leaves room for every pose
and reference half without unsigned index overflow.
Reuse modes
carry a fixed base index through `RenderTemporalScene`, with a cut at sequence
start; fresh NEE/MIS and initial-only frames use `RenderScene` with explicit
indices. Successful calls must expose one fresh sample, zero validation
messages and the expected history availability. Radiance reuse is disabled.

The independent NEE/MIS reference always uses the reference integrator,
including when wavefront is measured. Each pose uses two disjoint equal-spp
batches starting at `reference_seed + frame * 8192 + half * 4096`. Their mean is saved;
`reference_split_rmse` records their discrepancy without subtracting reference
noise. The static distant reference also reports its error to the independent integral.

`--restir-bounces` changes both reference and measured transport, including later
NEE/MIS bounces after the primary reservoir. Workload IDs identify geometry,
materials and motion; `procedure.max_bounces` identifies the transport depth.
The exact static distant-plane integral remains valid because its isolated plane
has no other surface to scatter from. The glossy/boundary fixtures can include
interreflection and retain finite references at every depth. A larger depth alone
does not establish broad production-scene coverage.

Changing only `--restir-reference-seed` leaves measured draws unchanged and
allows finite-reference sensitivity checks. Independent reference groups must use
disjoint ranges: reserving `256 * 8192 = 2097152` indices per seed is sufficient
for every accepted configuration (for example, seeds 16777216 and 18874368).
Reference seeds, like transport depth, must match within a repeated quality-budget
group; analyze different reference groups separately. Shared references across
measured repeats or integrators are not additional independent reference trials.

The JSON retains raw populations and summaries of fresh-frame RGB RMS error,
GPU scene duration, reservoir and shared metadata pass durations, blocking
update/render wall time, and frame-difference RMS after subtracting the matched
reference's motion. Differences exclude sequence boundaries. Static cases
record power-of-two/final prefix convergence: RMS over independent sequences
of each CPU-averaged prefix's error to the scene's `error_reference`, with its mean cumulative
GPU time. Each checkpoint also retains `rmse_raw` and
`cumulative_gpu_ms_raw`, one entry per independent sequence (GPU populations
are null without timestamps). The RMS of the raw errors equals the aggregate
RMS, while the arithmetic mean of the raw times equals cumulative GPU time.
Moving cases have no static convergence curve. Both modes retain
the final fresh image and an ensemble image: all samples/sequences for static,
only the final pose across sequences for moving. Ensemble RMS, signed channel
mean error and maximum absolute 8x8 tile mean error help inspect bias/artifacts,
but finite sampling and recursive correlation prevent a proof of unbiasedness.

`scene_gpu_ms` already includes metadata and reservoir work; components must
not be added to it. It excludes scene updates and readback. Wall time includes
both. Reservoir and metadata bytes include device storage and host mirrors;
scene-pool reserved bytes exclude renderer targets. Total VRAM remains null.
GPU series/cumulative times are null when unsupported or inapplicable.

Each invocation sweeps counts under one proposal, identified in
`procedure.candidate_proposal`; compare separate reports with matched settings
to evaluate alternatives. NEE/MIS references and cases retain their existing
power selection regardless of the reservoir proposal. The distant fixtures do
not establish area-light/environment performance; the mixed fixtures add
bounded fixture coverage, without establishing production scene guarantees. The
reference split discrepancy and finite number of independent sequences limit
moving bias/artifact conclusions. Reduced frame differences alone do not
establish improved convergence or speed. Workload/settings/build identity must
match before comparing reports.

Each case also records `temporal_residuals`, with one entry per independent
sequence. For each fixed screen-space RGB component, let `r[t]` be the fresh
sample minus that frame's matched `error_reference`, and `m` its sequence
mean. `sequence_mean_rmse` is the RMS of `m` across components;
`centered_rmse` is the RMS of `r[t] - m` across frames and components.
The sum of their squares equals that sequence's mean fresh-frame squared
error. In static scenes, the sequence mean RMS also equals its final
`convergence.rmse_raw` entry. Moving means average residuals at fixed screen
coordinates, not reprojected surfaces or a static transport solution.

`lag1_autocorrelation_raw` contains
`sum((r[t]-m)*(r[t-1]-m)) / sum((r[t]-m)^2)`, pooled across components;
the numerator uses adjacent pairs and the denominator uses all frames.
This is a finite-sequence, variance-weighted sample autocorrelation, not an
effective sample count or an unbiased population-correlation estimate.
Zero centered variance produces null, including a frozen nonzero error;
the sequence mean RMS keeps that persistent error visible. Every sequence
starts a separate accumulator, so warmup and sequence-boundary pairs are
excluded. Per-component Welford moments and squared adjacent differences
need storage proportional to image size, rather than sequence length.
Motion, disocclusion, short sequences and finite reference noise limit
interpretation; static finite references contribute a persistent error to
the mean, and moving finite references also affect the centered statistics.

Static cases also record `sequence_ensemble`, null for moving cases. It treats
the independent sequences as replications. For each component, `m_k` is
sequence `k`'s mean residual and `s_k^2` its within-sequence sample variance
(denominator `T - 1`). Averaged over components, `frame_variance` is the mean
`s_k^2` and `sequence_mean_variance` the across-sequence sample variance of `m_k`
(denominator `K - 1`), which is unaffected by correlation within a sequence.
`variance_inflation = T * sequence_mean_variance / frame_variance` is one for
uncorrelated frames and estimates the integrated autocorrelation time otherwise;
`effective_frames = T / variance_inflation`. Both are null when frame variance
is zero (effective frames also when inflation is zero).

`grand_mean_mse` is the squared error of the mean over all sequences, equal to
the static ensemble MSE. Without bias, its expectation is
`grand_mean_noise_mse = sequence_mean_variance / K` plus `reference_noise_mse`,
the squared reference split RMS over four (a mean of two independent halves),
or zero for the exact integral. `noise_normalized_mse` divides the grand mean MSE
by that expected noise: values near one are consistent with no detectable bias.
`excess_mse` subtracts it, estimating squared bias, and can be negative.
Pooled pixels can be spatially correlated (spatial reuse) or heavy-tailed, and
within-sequence variance is slightly underestimated under correlation; no
confidence interval is asserted. Use many sequences: two give a single degree
of freedom. CTest checks presence, populations and the exact-integral zero
reference noise; `lotus-renderer-restir-statistics` checks the decomposition
against an independent definition.

Every case also records `replication_bias`, which applies the same
replication test to moving poses and to localized regions. Replications are
independent images sharing one reference: each sequence's mean residual image
in static scenes (`replication: "sequence-mean"`, one pose), or each pose's
fresh residual from every sequence in moving scenes (`"pose-frame"`, one pose
per frame). Moving histories differ between poses, but every sequence reaches a
pose through the same motion, so its frames across sequences are independent
and share the pose's reference. For each pose, `pose_grand_mean_mse` is the
squared error of the replications' mean, averaged over components, and
`pose_noise_mse` its expectation without bias: the across-replication sample
variance over the replication count plus the reference's per-component
variance, estimated as `(a - b)^2 / 4` from the two halves.
`pose_reference_noise_mse` is that reference term, equal to the pose's squared
split RMS over four (zero for the exact integral). Pooled fields average the
poses; `noise_normalized_mse` and `excess_mse` are defined as for the sequence
ensemble. Static pooled values equal the sequence ensemble's grand mean MSE and
expected noise; a moving case's final pose equals its ensemble MSE.

`regions` repeat the pooled test within each fixed screen band of the case's
`regions`. `tiles` tests each 8x8 tile's mean residual per channel and pose:
`z` divides the replications' mean tile error by the square root of its expected
noise, from the across-replication variance of tile means (so spatial
correlation between neighbouring pixels is included) plus the summed reference
pixel variances over the squared pixel count (reference pixels are independent).
`tests` counts defined tests (zero expected noise is skipped), `beyond_3` those
with `|z| > 3`, and `max` identifies the largest `|z|` by pose, tile column/row
and channel, or is null. Without bias, about 0.27% of approximately normal tests
exceed three and the largest `|z|` grows with the number of tests, so a single
large value is not a detection. Sample variances from few replications, heavy
tails and the noisy reference estimate widen the tails; no confidence interval is
asserted. A pose or tile test sees persistent error only at the resolution its
noise allows: a localized error that changes sign over a tile, or a bias smaller
than its noise, can pass. Moving storage grows with frames times image size, like
the per-pose references. CTest checks populations, region identities, tile
counts and reference-noise presence; `lotus-renderer-restir-statistics` compares
the test with a direct definition, including a partial tile and a detected
persistent error.

`animation` is null except in the animated moving scene, where it records the
edit `step`, `edits_per_sequence` and a `step_lag_correlation` over
`step_lag_pairs` pose pairs. It pairs each sequence's residuals at frames `f`
and `f + step`, centers each component across sequences (removing persistent
error and both poses' reference noise) and pools the co-moments over components
and pairs. An edit separates every pair, so reservoir history cannot connect
them. Reuse modes restart their fresh-batch RNG at the fixed base index after
each reset, by the [reset rule](#compatibility-and-publication) that
[temporal radiance batches](TEMPORAL.md#temporal-radiance-accumulation) share;
non-reusing modes advance it every frame. The correlation therefore
measures how much replayed random numbers correlate noise across an edit. It is
not a bias test; `lotus-renderer-restir-statistics` checks it against a direct
definition.

CTest `lotus-renderer-restir-benchmark-reference` and `-wavefront` check both
workloads, candidate/mode populations, fresh memory accounting, history,
static integral means, output images and CLI preservation. The remaining
checks in this paragraph are Renderer Phase 5's campaign procedures, in the
[research tier](../design/TEST_POLICY.md#2-tiers): the `research` build
intent registers them, alongside `lotus-renderer-restir-controls-reference`
and `-wavefront`, and the `-uniform` and `-power-uniform` proposal variants.
The `-mixed` variants check the alternate scene identities, finite-reference labelling,
boundary metrics and all reservoir modes under both integrators. The `-glossy`
variants check the camera-disocclusion suite and both regional measurements. The
four-bounce `-interreflection` variants check the closed-room identities,
the red-wall band and the static control against the first moving pose.
The `-animation` variants check the animated identities, both bands, the
static control and the edit step and step-lag population; the benchmark
fails if any edit retains history or any unedited frame drops it.
The Vulkan-only
`lotus-renderer-restir-benchmark-no-driver` checks explained SKIPs. These
checks assert no timing and may run concurrently under `ost test --jobs N`.
Measured evidence belongs in [reports](../reports/README.md).

### Campaigns

`scripts/restir_campaign.py <plan.json> [--jobs N] [--dry-run]` runs a set of
invocations from one JSON plan (executable, output directory, label, shared
`defaults` and per-run option overrides; the script documents the format),
writing `<output>/<name>.json`, its log and a `campaign.json` manifest of
commands, wall times and statuses. A passing report produced by the same
command is kept, so an interrupted or extended campaign reruns only missing,
failed or changed runs.

Each invocation's frames are small and synchronous, so the GPU is mostly
idle and concurrent invocations overlap well: four concurrent 32x32 runs
finished in 44% of their serial time, eight in 38%. Validation layer
overhead, a third of a 64-sequence run, also overlaps. Images and
statistics depend only on the seeds, but timings are shared with concurrent
work: `--jobs` above one appends `; jobs=N` to every label, and timing or
cost evidence must use `--jobs 1`. Reducers that require identical labels
therefore reject a mix of serial and concurrent runs.

Serial runs are still mostly idle, so with dynamic clocks the GPU spends most
frames in low power states and the same case can time five times slower
depending on when it runs. Timing evidence also needs locked graphics and
memory clocks (for example `nvidia-smi -lgc` and `-lmc`, set by an
administrator), a clock log covering the campaign, and the clock state in the
label. Disabling validation layers through the loader
(`VK_LOADER_LAYERS_DISABLE=VK_LAYER_KHRONOS_validation`) removes their CPU
overhead without changing any image; the report must say so
([reuse-cost procedure](../reports/2026-10-17-restir-reuse-cost.md#timing-procedure)).

### Repeated quality budgets

`scripts/restir_budget.py` consumes at least two successful comparison reports
with identical build, device, procedure and workloads, except for the measured
seed. Measured sample intervals must be disjoint both within and between
reports. Run it separately for each integrator/proposal/settings group.
For four sequences, spacing invocation seeds by `4 * 65537` gives disjoint
ranges. Keep the label identical and use distinct report/image paths.

The `--rmse` argument accepts one or more positive static RMS thresholds;
`--output` selects a `lotus-restir-budget-v1` JSON report. Each budget uses the
first observed checkpoint at or below the threshold that remains below at
all subsequent measured checkpoints. It performs no interpolation, and makes
no assertion between checkpoints or after the measured horizon. This avoids
using an early crossing followed by a measured regression as attainment.

The reducer retains each invocation's attained sample count and cumulative
scene GPU time, with null for unattained budgets or unavailable timestamps.
Paired speedup is the same invocation's NEE/MIS time divided by the case time
at the same threshold. Summary populations (mean, median, min, max) are emitted
only when every invocation supplies a value; partial attainment remains visible
without a successful-subset average. Thresholds are applied to each invocation's
RMS over independent sequences, not to an ensemble formed by pooling runs.
Raw checkpoint population/aggregate consistency is checked before reduction.
Reference labels and split discrepancies are retained in each scene, and
boundary-region errors are reduced without dropping their identities or pixel
counts. Temporal residual populations retain run/sequence boundaries and
undefined correlation entries; their means, variance decomposition and static
convergence consistency are validated. Sequence ensembles are retained per
run, with inflation and noise-normalized summaries only when every run defines
them; their identities with the ensemble error, split RMS and each other are
validated, and moving cases must report null. Replication bias tests are
retained per run, with per-pose series, band ratios, tile counts and the largest
tile `|z|` across runs; their pooling, reference noise against each pose's split,
final-pose ensemble identity and agreement with the static sequence ensemble are
validated. Missing measurements in every run
produce null for older reports; mixing measured and unmeasured runs rejects.
Mismatched reference kinds or regions reject. Older v1 distant reports
without explicit labels/regions remain supported. A budget against a finite
NEE/MIS image describes agreement with that image, including reference noise;
it is not a budget against an exact transport solution.

Moving scenes retain across-invocation fresh/residual-delta RMS, final-pose
ensemble and tile error summaries and signed channel means, with no static
quality budget. Repeated references use the same independent NEE/MIS ranges;
these shared noisy references do not provide new independent bias evidence.
Dynamic clocks, validation overhead, checkpoint spacing, finite sampling and
recursive temporal correlation still constrain interpretation. This tool
measures budgets; it does not declare renderer exit or release readiness.

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
separately and carried into later merges. Visibility is resolved at the current
receiver and never substituted for unshadowed support evaluation. When the final
light slot and both unit-square variates exactly match the current frame's
initial sample at that receiver, its already queried transmittance is reused.
Every changed sample gets a new shadow query; previous-frame visibility is
never used.

For support correction, an exact light-slot/UV match to a participating
source uses that source's stored unshadowed `selected_target` at its own
jittered surface. Every other selection is evaluated at that source surface,
including sources whose observed stream was all-zero. This target cache
never substitutes a source's target for the receiving-surface contribution.
At the receiver, final shading and any required shadow query share one
reconstructed light sample. Source shading and transmittance remain distinct.

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
After any reset the fresh batch starts again at `sample_index`, so frames
after successive edits reuse the same random numbers: under light/material
animation, reuse noise stays correlated across edits (about 0.58 in the
[animation measurements](../reports/2026-10-17-restir-animation.md)), and a
scene edited on every frame replays one set of random numbers on every frame.
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
An independent moving fractional-occluder fixture also checks same-light,
different-UV transmittance and unchanged selections in temporal and combined
reuse ([visibility evidence](../reports/2026-10-15-restir-visibility.md)).
The same area fixture independently checks selected targets and resolved RGB
at the current jittered receiver using the Lambert/rect area Jacobian
([evaluation evidence](../reports/2026-10-16-restir-evaluation.md)).
Emissive emission, toggle, visibility and placement edits, with reservoir and
radiance histories together, check each reset reason, the conservative
emissive-scene reset, that no reset publishes or reuses a stale record, and
byte-identical reference/wavefront sequences
([reset-contract evidence](../reports/2026-10-17-restir-reset-contract.md)).

These bounded fixtures do not establish unbiased arbitrary moving-history
selection, many-light speedup or production temporal stability. Correlated
recursive samples add no independent draws; nearest-pixel reprojection and
geometric rejection can affect history selection. Visibility is always current,
but indirect radiance reuse keeps its separate limitations. General light-domain
remapping remains later work.

## Spatial GPU reuse

Set `PathTracingSettings::restir_spatial = true` with `RestirInitial` and call
`RenderTemporalScene` with one fresh sample. `restir_temporal` is independently
selectable: the spatial input is this frame's temporal result when enabled,
or fresh initial reservoirs with their exact primary shading otherwise.
Ordinary `RenderScene` Radiance calls reject spatial requests. The default is off.

One fullscreen pass reads immutable current input records and writes separate
destination history. It visits left, right, up and down at
`restir_spatial_radius` pixels (default 1, range 1–32), without duplicate or
clamped edge samples. Misses and pixels outside the data window are rejected.
Acceptance requires equal material identity, geometric and shading normal
cosines at least 0.9, and exact jittered view depths differing by at most
`0.01 + 0.02 * max(depths)`, with both depths positive. Different instances can participate. Acceptance
depends only on surfaces, never selected sample target or visibility.

The centre is streamed using its explicit resolved normalization and full
represented count; each accepted neighbour uses its resolved normalization
and `min(M, restir_max_history_candidates)`. This preserves preceding temporal
support correction instead of reconstructing it from raw sum/M. The final
selection is reevaluated at every participating surface; only counts whose
unshadowed target is positive enter the support denominator. Observed all-zero
neighbours participate too. The exact light-slot/UV target cache described
under [temporal reuse](#temporal-gpu-reuse) applies at each participating
source. Current visibility/transmittance is resolved at the
receiver after selection, with the same emission accounting as initial lighting.
An exact light-slot/UV match to the centre's current temporal input reuses that
receiver's transmittance. A changed selection gets a new shadow query, including
when only its UV changes. Neighbour visibility is never used at the receiver.
With no accepted neighbours the input record is copied unchanged. Spatial
selection preserves the selected source's age; only temporal reuse advances it.
The spatial result retains the receiver's exact shading and becomes next
frame's history when temporal reuse is enabled.

The represented count is bounded by `restir_candidates + 5 * cap` with temporal
reuse, or `restir_candidates + 4 * min(restir_candidates, cap)` without it.
These counts describe represented streams, not independent information:
neighbours and repeated frames can share underlying draws. Spatial selection
uses `SeedRandom(pixel, sample_index ^ 0x369DEA0F)` and one variate per accepted
neighbour in left/right/up/down order. Mode and radius changes reset transport
compatibility as `RenderSettingsChanged`; other temporal resets and the fixed
light-domain restrictions remain as described above. Failed GPU arithmetic
latches renderer failure and publishes neither reservoir nor temporal products.

### Products and costs

`restir.initial` and `restir.temporal` expose the two preceding stages;
`restir.spatial` exposes 64-byte resolved output records with
`spatial_row_pitch = width * sizeof(RestirReusePixel)`. `spatial_sources` has
four uint32 indices per pixel in stencil order, UINT32_MAX for rejected sources.
`previous` contains the preceding frame's final spatial output when compatible
temporal history is available. `previous_index` in a spatial record identifies
the receiver's participating temporal correspondence; it does not identify a
selected spatial source. The source-index diagnostic records all participants.

The immutable input buffer and its readback add 384 bytes/pixel; source-index
storage and readback add 32. With initial and two-frame history allocations,
fresh spatial capacity totals 1280 bytes/pixel, excluding shared temporal
metadata. Capacities remain retained after smaller targets and mode switches.
`spatial_gpu_ms` times only the spatial pass, including target/support evaluation
and selected visibility, excluding readback. `temporal_gpu_ms` still includes
the exact shading preparation pass when temporal reuse is off. All three
pretransport passes are included in the scene duration.

### Validation and limits

`lotus-renderer-restir-gpu` checks CPU/GPU selection, corrected weights/counts,
immutable input/output history, all-zero streams, differing support (including
previously corrected temporal weights), depth/geometric-normal/shading-normal/
material boundaries, crop and screen edges, current fractional/opaque visibility,
moving disocclusion, rejected requests, mode/radius resets and spatial overflow
under both integrators. A 32-light moving-camera fixture compares four reuse
modes with an independent integral and NEE/MIS; it records frame variation,
error, GPU costs and memory ([measurement](../reports/2026-10-14-restir-spatial.md)).

The stencil, geometry thresholds and candidate strategy are deliberately fixed
for this first pass. These bounded comparisons establish no general many-light
speedup or production artifact/stability guarantee. Reservoir correlations,
nearest-pixel temporal selection and geometric rejection retain the limits
above; the [exit review](../reports/2026-10-17-renderer-phase5-exit.md) records
where spatial reuse is worth its cost.

## Hydra controls and diagnostics

`hdLotus` exposes primary direct-light reservoirs under either integrator:

| Setting | Type/default | Behavior |
| --- | --- | --- |
| `lotus:restir` | bool, false | Select `RestirInitial`, overriding `lotus:lightSampling`; disabling restores that flag's NEE/MIS or BSDF selection |
| `lotus:restirCandidates` | int, 8 | Fresh initial draws, clamped to [1, 64] |
| `lotus:restirTemporal` | bool, false | Reuse compatible preceding reservoirs while `lotus:restir` is enabled |
| `lotus:restirSpatial` | bool, false | One immutable spatial pass while `lotus:restir` is enabled |
| `lotus:restirHistoryCandidates` | int, 32 | Temporal/neighbour source cap, clamped to [1, 65536] |
| `lotus:restirSpatialRadius` | int, 1 | Four-axis pixel radius, clamped to [1, 32] |

The three bool settings are checkable usdview menu items. Reuse selects
`RenderTemporalScene`, one fresh sample per pass, and keeps colour/the render
pass unconverged. It requires ray queries and colour clears. Initial-only
reservoirs remain deterministic progressive transport, converging at
`convergedSamplesPerPixel`. Reuse flags are inactive with `lotus:restir` off.
`lotus:temporal` independently enables bounded radiance accumulation; turning
it off while reservoir reuse is active still produces fresh colour batches.
The shared `lotus:historyKey`, `lotus:historyReset` and single-view reset policy
retain their [temporal meanings](TEMPORAL.md#hydra-controls-and-debug-aovs).

The [reservoir AOVs](AOVS.md#reservoir-hydra-channels) expose the final spatial,
otherwise temporal, otherwise initial record. Binding them does not select
an estimator or enable reuse. They finish after one pass. In progressive
mode the delegate retains the last fresh diagnostic records so newly bound
buffers can read them after convergence, when the backend publishes no new
reservoirs. This CPU cache adds 48 bytes/pixel for initial-only records or
64 for resolved records, outside backend `reservoir_bytes`; intermediate
and preceding histories are not cached for these AOVs. Disabling reservoirs
writes empty sentinels/zero diagnostics, including with empty AOV clears.

`LOTUS_HYDRA_EVIDENCE` reports estimator, reservoir availability, requested
candidate count, effective reuse flags, compatible history and allocated
backend reservoir bytes. `lotus-renderer-hydra-restir-contract` checks defaults,
synthetic final-stage priority, row layout and invalid writes without a GPU.
`lotus-renderer-hydra-restir-gpu` checks moving USD stages, controls, resets,
diagnostics and deterministic NEE/MIS restoration under both integrators, and
that USD light, material, emission, placement and visibility edits reset as
headless does, byte-identically under both integrators
([reset-contract evidence](../reports/2026-10-17-restir-reset-contract.md)).
Installed usdview also exercises all four reservoir modes and moving/cut
sequences ([measurement](../reports/2026-10-14-restir-hydra.md)). These
adapter checks do not establish many-light speedup; the
[exit review](../reports/2026-10-17-renderer-phase5-exit.md) records when to
enable reservoirs.
