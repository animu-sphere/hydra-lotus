# Temporal metadata and history validity

`include/lotus/temporal.hpp` exposes the CPU temporal foundation in
`Lotus::Vulkan`, implemented in `backend/vulkan/temporal.cpp`. It is available
even when the build disables Vulkan and has no dependency on a device,
integrator, reservoir or denoiser. The
[phase policy](../design/ROADMAP_POLICY.md#renderer-phase-4--temporal-infrastructure)
owns its intended scope; the [roadmap](../roadmap/README.md#status-at-a-glance)
owns delivery status.

## Hydra controls and debug AOVs

`hdLotus` exposes the same backend through render settings:

| Setting | Type/default | Behavior |
| --- | --- | --- |
| `lotus:temporal` | bool, false | True selects bounded temporal radiance, one fresh sample per pass under either integrator; false disables radiance reuse while preserving the estimator/reuse selection |
| `lotus:historySamples` | int, 32 | Effective history cap, clamped to [1, 65536] |
| `lotus:historyKey` | int, 0 | Caller compatibility token, clamped to nonnegative; a change resets history |
| `lotus:historyReset` | int, 0 | Change this counter to declare one camera cut; the new value is consumed only by a successful temporal frame |
| `lotus:filterIterations` | int, 0 | [Edge-aware filter](#edge-aware-filtering) iterations over `lotus:temporal`'s colour, clamped to [0, 8]; unused without `lotus:temporal`, and a change never resets history |

Temporal colour and the render pass remain unconverged so hosts keep drawing
fresh batches. `convergedSamplesPerPixel` continues to control progressive
convergence when temporal radiance and
[reservoir reuse](RESTIR.md#hydra-controls-and-diagnostics) are disabled. The sample seed, integrator
and light-sampling settings retain their existing meanings. Radiance mode
requires colour clears and ray queries; unsupported bindings/capabilities
report errors rather than producing temporal bootstrap data.

Binding any [temporal debug AOV](AOVS.md#temporal-hydra-channels) requests the
GPU metadata/validity passes even when radiance reuse is disabled. This
metadata-only mode keeps progressive convergence and writes zero history
length. Debug AOVs finish after one pass and are overwritten over the full
extent, regardless of their clear value (including an empty clear).
Their records use the backend's top-left motion convention even though the
adapter flips storage rows for Hydra. Binding debug channels is optional;
temporal colour works by itself. AOV validation errors do not consume a
rendered frame index or a pending reset counter.

One delegate retains one GPU view. Render-pass identity is retained with a
token; switching passes conservatively cuts temporal history, including
when cameras and extents happen to match. Alternating views do not retain
independent histories. Successful ordinary renders interrupt the stream;
failed submissions recreate the renderer on the next pass. Camera/rigid
motion, scene/settings classification and same-count instancer reordering
follow the backend contracts below. There is no automatic camera-cut
heuristic: increment `lotus:historyReset` or change `lotus:historyKey` when
the host knows a cut or incompatible edit occurred.

`LOTUS_HYDRA_EVIDENCE` optionally appends per-frame availability, reset code,
batch seed, valid/moving pixel counts and maximum effective history count
alongside the existing render evidence. The
[Hydra measurement and exit review](../reports/2026-10-13-temporal-hydra-exit.md)
checks moving USD stages and the installed usdview host.

## Frame lifecycle

One `TemporalHistory` belongs to one rendered view. `Advance(snapshot,
settings)` publishes a `TemporalFrame` and retains its current snapshot for
the next call. Call it once after a successfully rendered frame, independently
of scene commits and samples per pixel. Do not call it for failed or abandoned
renders. The caller supplies the rendered frame index; it may start at zero.
An unchanged scene revision still advances temporal history.

Each returned frame owns immutable current and, when compatible, previous
`FrameSnapshot`s. Camera data is copied; scene resources remain shared. Advance
does not traverse scene records or copy geometry, materials or textures. A
retained returned frame remains valid across later advances and edits.

| Condition, in priority order | `reset_reason` | Previous snapshot |
| --- | --- | --- |
| No retained frame | `FirstFrame`, or `ExplicitReset` after `Reset()` | absent |
| Caller declares a camera cut | `CameraCut` | absent |
| Frame index is not the preceding index plus one | `FrameGap` | absent |
| Extent, display window or data window differs | `TargetChanged` | absent |
| Caller changes `history_key` | `HistoryKeyChanged` | absent |
| Otherwise, including ordinary camera or object motion | `None` | present |

Zero width/height, non-finite display windows and duplicate or decreasing indices throw
`std::invalid_argument` without consuming the preceding frame. `Reset()`
releases the retained snapshot and permits the frame index to restart. After
any accepted reset frame, the next compatible frame can reuse it.

`history_key` is a caller-owned compatibility token. In CPU/metadata-only
mode the consumer must change
it, or explicitly reset, for lighting, integrator, estimator, sampling settings
or other changes that invalidate radiance globally. There is no automatic
camera-cut heuristic. The opt-in GPU radiance mode additionally classifies
scene and render-setting incompatibilities as described below.

## Placement correspondence

`FindTemporalPlacement(frame, mesh_key, placement_ordinal)` pairs rigid
placements by stable host mesh key and ordinal, independently of transient
GPU geometry, material or instance table positions. It returns the composed
current and previous `world_from_object`, including an instancer's transform
times the prototype's transform. Mesh insertion elsewhere in key order does
not alter correspondence.

There is no correspondence when either frame lacks a visible, nonempty mesh,
the geometry resource changes, instancing mode or placement count changes,
or the ordinal is absent. Geometry replacements are conservatively rejected,
including point deformation with unchanged topology. Same-count instancer
reordering cannot be identified from the scene's ordinal-only representation;
the consumer must reset history when reordering instances.

`TemporalPlacement::material_unchanged` compares the material binding key,
effective material values (including missing-binding fallback) and the
resources of textures that its inputs read. Material edits and texture
insertion, replacement or removal invalidate it; unrelated texture edits do
not. Consumers must require this flag before reusing a surface's history.

## Reprojection

`ReprojectTemporalPoint` takes the current object-space surface position and
its paired placement. It projects that position through each frame's object,
view and projection matrices using the core's column-vector, OpenGL clip
convention. Cameras must be unjittered. The CPU calculation uses double
intermediates and returns float products:

- Continuous current and previous pixels, with the origin at the top-left
  image edge. Pixel `(x, y)` has centre `(x + 0.5, y + 0.5)`.
- Backward motion in pixels: `previous_pixel - current_pixel`. Add this motion
  to the current position to locate history.
- The same surface's positive linear depth in the **previous** camera:
  `-previous_view_position.z / previous_view_position.w`.

No result is returned for missing history, non-finite transforms/positions,
nonpositive view or clip W, nonpositive view depth, clipping outside the
OpenGL depth range, or positions outside either image. Top/left edges belong
to the image; bottom/right edges do not. `TemporalFrameSettings` accepts the
same top-left display/data rectangles as `OffscreenTarget`: empty windows
mean the full image; display windows map NDC and may extend beyond the image;
data windows restrict usable pixels and are clamped to the image. Both
projected positions must lie inside the clip volume, image and data window.
Window changes conservatively reset history. Jitter offsets are not supported.

## Shared validity

`ValidateTemporalHistory` compares the current surface and the previous
surface fetched at the reprojected position. Both carry a hit flag, positive
linear view depth, world-space normal, and stable instance and material
identities supplied by the consumer. Identities must not be transient table
slots. The function returns one rejection reason, in this order:

1. A miss in either frame (`Miss`).
2. Non-finite products, nonpositive depths, zero normals or invalid thresholds
   (`InvalidData`).
3. Different instance or material identity (`InstanceChanged`,
   `MaterialChanged`).
4. Euclidean motion longer than `max_motion_pixels` (`LargeMotion`).
5. Previous fetched depth differing from the expected reprojected depth by
   more than `absolute_depth_tolerance + relative_depth_tolerance *
   expected_previous_depth` (`DepthDiscontinuity`). This tests disocclusion
   without rejecting a valid surface simply because the camera moved in Z.
6. Dot product of normalized world normals below `minimum_normal_cosine`
   (`NormalDiscontinuity`).

Otherwise it returns `None`. Defaults are 32 pixels of motion, 0.01 absolute
plus 1% relative depth tolerance, and normal cosine 0.9. Absolute depth uses
the scene's units; consumers can tune it. Threshold boundaries are inclusive.
This function validates geometry metadata only; the consumer also requires
a compatible frame, successful reprojection and an unchanged material.

## GPU primary-surface products

`OffscreenRenderer::RenderTemporalScene(snapshot, target, frame_count,
temporal_settings, path_settings)` is the opt-in backend entry point for both
integrators. Apply `SceneExtraction::Update(snapshot)` with `UpdateScene`
first, including empty plans. The extraction's `source_scene` token lets the
renderer reject a snapshot that does not match the uploaded scene. The
caller supplies unjittered cameras, an increasing frame index, a compatibility
key and explicit cuts. Target extent and windows come from `OffscreenTarget`;
those fields in the supplied temporal settings are ignored.

One successful call advances one temporal frame even when it adds multiple
progressive samples, adds no sample at the convergence limit, or renders an
empty scene. A failed validation does not consume history. A successful
ordinary `Render` or `RenderScene` breaks the temporal stream. Device failures
leave the renderer failed. The existing progressive accumulation and its
reset rules remain in effect in the default metadata-only mode. Enabling
`TemporalFrameSettings::accumulate_radiance` selects the radiance mode below.

`GpuFrameEvidence::temporal` carries tightly packed top-left rows of 64-byte
`TemporalPixel` records. Every record starts cleared, independent of the
colour/depth attachment clears and preservation settings. Pixels outside the
data/display window and misses stay zero. The pass uses a pixel-centre ray
and the existing fixed sample-0 alpha-coverage realization, independent of
the radiance RNG and sample count. Analytic lights and the environment are
not primary surfaces, consistent with ordinary scene depth.

| Product | Meaning |
| --- | --- |
| `normal_depth.xyz` | Unit world geometric normal facing the camera; not a material shading/normal-map normal |
| `normal_depth.w` | Positive linear depth in the current camera, distinct from the D32 window-depth attachment |
| `instance_id` | Renderer-stream-local nonzero 64-bit placement identity; zero for a miss |
| `material_id` | Renderer-stream-local nonzero identity of the binding and effective material/used texture resources; zero for a miss |
| `flags` | Bit 0 hit; bit 1 rigid placement correspondence; bit 2 unchanged effective material |
| `albedo` | The material's base colour at the hit, after texture lookups, clamped to [0, 1] as unorm8 R, G, B in bits 0–23 (`UnpackTemporalAlbedo`) |
| `shading_normal` | The shading normal facing the camera (authored, computed or mapped), octahedral snorm16 x and y in the low and high half (`UnpackTemporalNormal`) |
| `motion_depth.xy` | Backward motion, previous pixel minus current pixel, in pixels |
| `motion_depth.z` | Expected positive depth of this surface in the previous camera |
| `motion_depth.w` | 1 for a successful projection within the previous clip volume/image/data window; 0 otherwise |

Instance IDs survive camera/rigid motion and insertions before them in GPU
table order. Geometry, visibility, placement count or instancing-mode changes
create new placement incarnations. Material IDs change when the binding,
effective values or a used texture resource changes; unused texture edits
and material table reorderings leave them stable. IDs are monotonic allocated
tokens, never hashes, GPU slots or device addresses. They are retained by a
transaction published only after GPU completion. Same-count instance
reordering still requires a caller cut/key change.

Two device surface buffers alternate across successful temporal calls.
`current` is the newly generated buffer; `previous` is the retained preceding
GPU buffer copied for validation, empty when `history_available` is false.
That flag describes frame compatibility, not per-pixel visibility or history
validity. `motion_depth.w` likewise does not validate the fetched previous
surface; the separate shared-validity pass supplies that result.

The primary-surface pass has its own persistent timestamp pair:
`timestamp_available` and `primary_surface_gpu_ms`, covering the buffer clear
and surface draw, excluding metadata host writes and surface readback.
Unavailable timestamps report false/zero. `surface_bytes` counts both device
buffers and their two-buffer host readback mirror (256 bytes per target pixel);
`metadata_bytes` counts the 272-byte camera/window/validation/radiance block and the retained capacity
of the 96-byte-per-instance metadata buffer. Surface buffers/readback persist
at a fixed extent and are replaced on resize. Instance capacity grows when
needed. No pipeline or timestamp pool is created in a render call. Targets
or instance tables exceeding the device storage-buffer range fail explicitly.

## GPU reprojection and validity

After the primary-surface pass, a separate fullscreen fragment pass reads
the retained previous device buffer. Add backward motion to the current
pixel centre and fetch the pixel containing that continuous coordinate
(`floor` of each coordinate, equivalent to the nearest pixel centre).
There is no bilinear filtering of depth, normals or identities.
`ReprojectTemporalHistory` in `temporal.hpp` implements the CPU fetch oracle
over the same packed records, then calls `ValidateTemporalHistory`.

`TemporalFrameSettings::validation` supplies the shared thresholds. Changing
thresholds affects this frame's validity without resetting the frame history.
Invalid thresholds produce `InvalidData` when the geometric test is reached.
The pass checks, in priority order, current miss, missing compatible history,
missing rigid correspondence, changed effective material, and unsuccessful
or out-of-image reprojection. Only then does it fetch the previous surface
and apply the shared miss/data/identity/motion/depth/normal contract above.
Pixels outside the current data/display window are misses. Previous-window
clipping is encoded by the primary pass in `motion_depth.w`.

`GpuFrameEvidence::temporal.validity` is a debug AOV of 16-byte
`TemporalValidityPixel` records for the **entire** top-left target.
`validity_row_pitch` is `width * 16`. Every record is overwritten, including
empty scenes, misses and pixels outside the data window:

| Field | Meaning |
| --- | --- |
| `valid` | 1 only when rejection is `None`; otherwise 0 |
| `rejection` | Stable numeric `TemporalRejection` code below |
| `previous_index` | Row-major fetched pixel, including rejected fetches; `UINT32_MAX` if no fetch occurred |
| `reserved` | Zero |

| Code | Rejection |
| --- | --- |
| 0 | `None` |
| 1 | `Miss` |
| 2 | `InvalidData` |
| 3 | `InstanceChanged` |
| 4 | `MaterialChanged` |
| 5 | `LargeMotion` |
| 6 | `DepthDiscontinuity` |
| 7 | `NormalDiscontinuity` |
| 8 | `HistoryUnavailable` |
| 9 | `CorrespondenceMissing` |
| 10 | `ReprojectionOutside` |

GPU arithmetic uses float precision; the CPU geometry oracle retains double
intermediates. Threshold comparisons are inclusive in both implementations,
but rounding can change a decision within float precision of a boundary.
For example, motion `(32, epsilon)` can round to length 32 on the GPU while
the CPU length exceeds 32. The measured sequences compare exact codes and
fetch indices away from such ambiguous boundaries; CPU checks separately
exercise inclusive threshold and fetch-edge behavior.

`reprojection_gpu_ms` has its own persistent timestamp pair, sharing
`timestamp_available` with the primary-surface pass. It covers the fullscreen
validity pass and excludes host writes and readback. Unavailable timestamps
report false/zero. `validity_bytes` counts the persistent device output and
host readback mirror (32 bytes per pixel), separately from `surface_bytes`.
The two passes have explicit shader-write/read and transfer/readback barriers.
The validity pipeline and ten-query timestamp pool are created at renderer
initialization; fixed extents reuse buffers and resize replaces them.
History is published only after both passes, readbacks and timestamps succeed.
No transport, reservoir or denoiser state enters validity.

The reservoir consumer can run this same metadata/validity before transport.
Its sample payloads, history, compatibility policy and normalization remain
outside shared temporal state; see [temporal reservoir reuse](RESTIR.md#temporal-gpu-reuse).

## Temporal radiance accumulation

Set `TemporalFrameSettings::accumulate_radiance = true` in
`RenderTemporalScene`. The default is false. Both integrators trace a fresh
batch of `frame_count` samples per call, capped by `PathTracingSettings::max_samples`
when nonzero. That limit applies to each batch in this mode, rather than
stopping the temporal stream. `samples_per_pixel` reports fresh batch samples;
per-pixel retained counts are in `temporal.radiance`. Colour clears must be
enabled and output must be `Radiance`; unsupported settings fail before
advancing history. Depth clears/preservation keep their ordinary behavior.

Each compatible batch uses the next disjoint RNG range, starting with
`sample_index` after a reset. `batch_sample_index` and `batch_samples` expose
it. The 32-bit RNG index wraps modulo 2^32. Camera and rigid motion do not
restart the sequence. Failed calls do not consume it. A call is still one
temporal frame, regardless of its batch size. Ordinary progressive transport
remains selectable through `RenderScene` or metadata-only temporal mode.

The fresh RGBA batch mean is copied to a persistent device buffer. A third
fullscreen pass consumes the existing shared validity and nearest previous
fetch index. For a current primary hit, let `n = min(batch_samples, cap)`
and `h = min(previous_count, cap - n)` when shared validity accepts history,
otherwise `h = 0`. The output is `(n * batch_mean + h * previous_mean) / (n + h)`
and its effective count is `n + h`. `max_history_samples` supplies `cap`,
default 32, and must be in [1, 65536]. A batch as large as the cap replaces
history completely. At the cap this becomes a bounded exponential mean;
the count is an effective weight, not a count of independent surviving paths.
Below the cap a stationary scene matches progressive transport of the same
disjoint samples within float summation precision.

Misses and pixels outside the data/display window write the fresh batch
colour and a zero history count. Rejected hits start with fresh samples only.
RGBA is accumulated in linear space, including coverage alpha. Primary
metadata retains its fixed pixel-centre/sample-0 coverage realization while
radiance is box-filtered and jittered within the pixel, as before.

Radiance mode adds automatic global resets, after the ordinary frame/window/
key/cut reset priorities:

| Change | `reset_reason` |
| --- | --- |
| Enabling radiance after metadata-only, integrator, light estimator, reservoir proposal/candidate count/reuse mode/spatial radius, base RNG index, bounce limit, per-batch sample limit, history cap or background colour | `RenderSettingsChanged` |
| Constant environment, environment map, analytic lights, material table, texture resources, mesh insertion/removal, geometry resource, binding, visibility or instancing mode/count | `SceneChanged` |
| Camera or existing rigid-placement transform only | No global reset; shared validity controls each pixel |

Scene classification conservatively resets for unused material/texture edits
too. Same-count instance reordering requires a caller cut/key change. Moving
emissive/occluding geometry may change illumination without changing the
primary surface: supported rigid motion therefore preserves an approximate
history, not an unbiased moving-scene estimator. Use explicit cuts/keys for
changes requiring immediate radiance replacement. Validation thresholds
remain per-frame controls and do not reset the stream globally.

`TemporalProducts::radiance` and `previous_radiance` are 32-byte
`TemporalRadiancePixel` debug records: linear RGBA `color`, uint32
`sample_count` (the history length), the illumination luminance `moments`
and their `variance` ([below](#luminance-moments-and-variance)), all
unfiltered. The rows are top-left
and `radiance_row_pitch` is width * 32. Previous records are absent on a
reset; a pixel's reprojected history is
`previous_radiance[validity[i].previous_index]` wherever a fetch took place.
`batch` is the raw radiance: the fresh RGBA batch mean the accumulation read,
16 bytes per pixel in the same rows. The colour attachment receives the
temporal mean; the D32 attachment keeps ordinary primary depth.
`radiance_available` is false in metadata-only mode.

`accumulation_gpu_ms` and `variance_gpu_ms` use separate timestamp pairs in
the persistent ten-query pool, with the existing `timestamp_available`
flag. They measure the accumulation and variance draws, excluding batch
copy, transport and readbacks. `radiance_bytes` counts two device history
buffers, their two-buffer host readback mirror, and the fresh RGBA device
batch with its host mirror: 160 bytes per pixel. These buffers persist at a
fixed extent and are replaced on resize; returned evidence vectors and
Vulkan allocation alignment are outside this accounting. The pipelines are
created at renderer initialization. History and RNG state publish only after
the passes, readbacks and timestamps succeed.

### Luminance moments and variance

The accumulation also keeps the first two moments of each fresh batch mean's
illumination luminance: the colour demodulated by the primary surface's
`albedo`, each channel divided by the albedo's or left unchanged where that
channel is zero (`DemodulateTemporalColor`), then
`L = 0.2126 R + 0.7152 G + 0.0722 B` (`TemporalLuminance`). A fresh history
starts at `(L, L^2)`, and an accepted one blends its previous moments with
the colour's weights `n` and `h`. Demodulation keeps texture contrast out of
the variance; the moments of a textured surface measure its lighting
noise.
Misses and pixels outside the windows have zero moments. Previous moments
that are not finite are not reused, like a non-finite previous colour. The
inputs are the noisy radiance and the shared surface metadata only: the
moments do not depend on the integrator, the light estimator or reservoirs.

A fourth fullscreen pass, after every pixel's moments exist, writes
`variance`: an estimate of the variance of one fresh batch mean's luminance.
With `b = sample_count / min(batch_samples, max_history_samples)` batches of
history:

- `b >= 4` (`kTemporalVarianceBatches`): the temporal variance
  `max(0, m2 - m1^2)`. It divides by the batches, not one fewer, as SVGF does.
- `b < 4`, including every rejected or reset pixel: SVGF's spatial estimate.
  The moments of the 7x7 neighbourhood's histories on the same placement are
  averaged with weight `exp(-(|m1 - m1_q| / 10 + |z - z_q| / (phi * d))) *
  max(0, n . n_q)^128`, where `z` is linear depth, `n` the geometric normal,
  `d` the pixel distance and `phi` three times the local depth gradient (per
  axis the smaller step to a usable neighbour, the two axes summed). Their
  variance is scaled by `4 / b`, so short histories err on the noisy side.
- Pixels without a history have zero variance.

The accumulated colour's own variance is about `variance / b`, which is
what the [filter](#edge-aware-filtering) starts from.
`EstimateTemporalVariance` in `temporal.hpp` is the CPU oracle over the same
records; the GPU test compares every pixel of every radiance frame with it.

### Edge-aware filtering

`TemporalFrameSettings::filter_iterations` (default 0, at most
`kTemporalFilterMaxIterations`, 8) runs SVGF's edge-aware a-trous filter
over the accumulated radiance after the variance pass; SVGF uses
`kTemporalFilterIterations`, 5. It requires `accumulate_radiance`; other
settings fail before advancing history. The filter reads this frame's
records only and writes nothing back into the history, so `radiance`,
`previous_radiance` and the moments stay unfiltered and changing the
iteration count, or switching the filter off, never resets the stream. With
any iterations the colour attachment holds the filtered composite.

For each pixel with a history the filter starts from its illumination
(`DemodulateTemporalColor` of the accumulated colour) and the variance of its
mean's luminance, `variance / b`. Iteration `i` averages a 5x5 B3-spline
footprint (`3/8, 1/4, 1/16` per axis) whose taps lie `2^i` pixels apart,
over histories on the same placement only, each tap weighted by

`exp(-(|l - l_q| / (4 sqrt(g + 1e-10)) + |z - z_q| / (phi * d))) * max(0, n . n_q)^128`

where `l` is the illumination luminance, `g` the 3x3 Gaussian of the
variance over usable neighbours, `z` linear depth, `phi` the local depth
gradient (per axis the smaller step to a usable neighbour, the two axes
summed), `d` the tap's pixel distance and `n` the shading normal. Variances
combine with the squared weights, so each iteration's luminance test
tightens as the noise falls. The composite multiplies the last iteration's
illumination by the albedo again (`RemodulateTemporalColor`) and keeps the
accumulated alpha; misses and pixels outside the windows keep the
accumulated colour. Motion and disocclusion reach the filter through the
history: a rejected pixel restarts at one batch with the boosted spatial
variance, so it is filtered hardest.

| Decision | Here | SVGF |
| --- | --- | --- |
| History | Unfiltered temporal mean | The first iteration's output is fed back |
| Luminance test | Variance of the accumulated mean, `variance / b` | Per-sample variance |
| Neighbours | Same placement (stable identity) only | Depth and normal tests only |
| Normal | Shading normal; geometric in the variance estimate | G-buffer normal |

Without feedback the history converges to the unbiased mean, and with the
variance of the mean the filter fades as the history grows: a static view
converges to the temporal mean rather than to a blurred image. The filtered
output is never history, so its bias does not accumulate.

`FilterTemporalRadiance` in `temporal.hpp` is the CPU oracle, in the GPU's
float arithmetic. On a filtered frame the variance pass also writes the
first iteration's input (illumination and the variance of its mean) and a
16-byte guide per pixel: its placement identity, depth and shading normal,
or a mark for a pixel without a history. Each iteration is a fullscreen
pass that reads only its taps' guides and the previous iteration's values,
from a persistent buffer of two ping-pong halves and the guides (48 bytes
per pixel, `filter_bytes`), the draw's first instance giving the iteration;
the last also composites into the colour attachment. `filter_gpu_ms` times
every iteration and the composite with the ninth and tenth queries of the
temporal pool; `variance_gpu_ms` includes writing the guides.
`filter_iterations` reports the frame's count.

`OffscreenRenderer::Present` shows the filter's inputs from the latest
radiance frame's GPU buffers, without reading them back: `RawRadiance` (the
fresh batch), `AccumulatedRadiance` (the unfiltered temporal mean),
`HistoryLength` and `Variance` (`DebugView`), and `PresentView::split`
shows a view beside the colour, the filtered composite when the frame
filtered. `lotus-viewport` switches the filter, its iteration count and
these views at run time and times the variance and filter passes in its
HUD ([usage](../guides/BUILDING.md#the-standalone-viewport),
[evidence](../reports/2026-10-18-svgf-viewport.md)).

### Headless SVGF comparison

`lotus-headless --benchmark-svgf <json-path>` compares the fresh batch
(raw), the temporal mean and the filtered composite of the same renders
with a high-spp reference. Its workload, `svgf-room`, is a closed room lit
by an emissive ceiling panel, with a checker-textured floor, a white box and
a rough metal block: a static view, and a moving one that pans the camera
and slides the block. Every pose's reference is the mean of two disjoint
halves from the reference integrator, NEE/MIS, which also estimate its
noise. Each sequence starts with a camera cut and a disjoint sample range;
the first is then rendered again and must reproduce every output bit for
bit (`deterministic`).

| Option | Default | Range |
| --- | --- | --- |
| `--benchmark-integrator` | `reference` | `reference`, `wavefront` |
| `--benchmark-size` | 128 | 16–512 (square) |
| `--benchmark-frames` | 32 | 4–256 |
| `--svgf-sequences` | 4 | 2–64 |
| `--svgf-batch` (fresh samples per frame) | 1 | 1–64 |
| `--svgf-history-cap` | 32 | 1–65536 |
| `--svgf-iterations` | 5 | 1–8 |
| `--svgf-bounces` | 4 | 1–64 |
| `--svgf-seed` | 17 | 0–8388607 |
| `--svgf-reference-seed` | 16777216 | 16777216–1073741823 |
| `--svgf-reference-samples` (per half) | 1024 | 16–8192 |

Per workload and output the report gives the MSE per frame and its mean,
the frame-delta MSE (the change of each pixel's error from the previous
frame: flicker and lag), the MSE by history length in batches (the response
after disocclusion or rejection) and the last frame's squared error of the
sequences' mean, split into expected noise and an estimate of squared bias.
`metric_notes` in the report defines each; it asserts no quality or speed
threshold. Its PFMs are the reference and the three outputs of the first
sequence's last frame. The `research` tier runs a small configuration under
both integrators (`lotus-renderer-svgf-benchmark-*`).

Nearest history fetch, geometric rejection and bounded weighting supply this
foundation. There is no radiance neighbourhood clamp, history filtering,
anti-lag heuristic or motion-aware BRDF correction. View-dependent shading,
indirect-light changes and jittered silhouette coverage can therefore lag or
ghost during motion. The [accumulation report](../reports/2026-10-12-temporal-radiance.md)
measures static equivalence, moving-image error and exposed-surface rejection;
it does not establish production-quality temporal reconstruction.

## Entry points and limits

CTest `lotus-renderer-temporal` exercises snapshot lifetime, reset recovery,
invalid-input atomicity, stable placement pairing, material/texture edits,
independent pinhole projections, camera/object motion, clipping, disocclusion,
normal and identity rejection, and threshold boundaries. It runs without a
GPU ([measured checks](../reports/2026-10-12-temporal-foundation.md)), with
additional window and identity-registry checks. `lotus-renderer-temporal-gpu`
checks stationary, camera-only, object-only, combined motion, rotated normals,
orthographic projection, framing, resize, scene/material/texture/instancer
churn, successful-only history and progressive output under both integrators
([GPU evidence](../reports/2026-10-12-temporal-primary-surfaces.md)). It also
compares GPU rejection, validity and fetch indices with the CPU oracle at
every pixel, requires all rejection codes to be exercised, and checks
self-disocclusion, an independent moving occluder, invalid thresholds and an
initial empty scene without a TLAS
([validity evidence](../reports/2026-10-12-temporal-gpu-validity.md)). Its
Vulkan-disabled configuration and missing-driver regression report explained
SKIPs.

The GPU test also compares every temporal colour/count with independent
fresh batches, static convergence with progressive transport, and moving
sequences with 256-sample reference images, including cap saturation,
disocclusion, global resets and failed-call recovery
([radiance evidence](../reports/2026-10-12-temporal-radiance.md)).
It compares every moment with the independent batches and every variance
with the CPU oracle, and requires both the temporal and spatial estimates
([moments and variance evidence](../reports/2026-10-18-svgf-moments-variance.md)).
It alternates the filter's iteration count from frame to frame, compares
every filtered pixel with the CPU oracle, and checks the albedo and shading
normal products ([filter evidence](../reports/2026-10-18-svgf-filter.md)).
The filter is not history: nothing filtered is reused. Hydra controls and
AOV bindings are described [above](#hydra-controls-and-debug-aovs).
Geometry deformation is conservatively rejected.
No ReSTIR state is introduced, and the filter keeps none across frames.
