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
| `lotus:temporal` | bool, false | True selects bounded temporal radiance, one fresh sample per pass under either integrator; false selects deterministic progressive transport |
| `lotus:historySamples` | int, 32 | Effective history cap, clamped to [1, 65536] |
| `lotus:historyKey` | int, 0 | Caller compatibility token, clamped to nonnegative; a change resets history |
| `lotus:historyReset` | int, 0 | Change this counter to declare one camera cut; the new value is consumed only by a successful temporal frame |

Temporal colour and the render pass remain unconverged so hosts keep drawing
fresh batches. `convergedSamplesPerPixel` continues to control progressive
convergence when temporal radiance is disabled. The sample seed, integrator
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
The validity pipeline and six-query timestamp pool are created at renderer
initialization; fixed extents reuse buffers and resize replaces them.
History is published only after both passes, readbacks and timestamps succeed.
No transport, reservoir or denoiser state enters validity.

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
| Enabling radiance after metadata-only, integrator, light estimator, base RNG index, bounce limit, per-batch sample limit, history cap or background colour | `RenderSettingsChanged` |
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
`sample_count` and three zero reserved words. The rows are top-left and
`radiance_row_pitch` is width * 32. Previous records are absent on a reset.
The colour attachment receives the temporal mean; the D32 attachment keeps
ordinary primary depth. `radiance_available` is false in metadata-only mode.

`accumulation_gpu_ms` uses a separate timestamp pair in the persistent
six-query pool, with the existing `timestamp_available` flag. It measures
the accumulation draw, excluding batch copy, transport and readbacks.
`radiance_bytes` counts two device history buffers, their two-buffer host
readback mirror and the fresh RGBA device batch: 144 bytes per pixel. These
buffers persist at a fixed extent and are replaced on resize; returned
evidence vectors and Vulkan allocation alignment are outside this accounting.
The pipeline is created at renderer initialization. History and RNG state
publish only after the passes, readbacks and timestamps succeed.

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
History filtering remains outside this implementation. Hydra controls and
AOV bindings are described [above](#hydra-controls-and-debug-aovs).
Geometry deformation is conservatively rejected.
No ReSTIR or denoiser state is introduced.
