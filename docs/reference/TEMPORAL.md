# Temporal metadata and history validity

`include/lotus/temporal.hpp` exposes the CPU temporal foundation in
`Lotus::Vulkan`, implemented in `backend/vulkan/temporal.cpp`. It is available
even when the build disables Vulkan and has no dependency on a device,
integrator, reservoir or denoiser. The
[phase policy](../design/ROADMAP_POLICY.md#renderer-phase-4--temporal-infrastructure)
owns its intended scope; the [roadmap](../roadmap/README.md#status-at-a-glance)
owns delivery status.

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

`history_key` is a caller-owned compatibility token. The consumer must change
it, or explicitly reset, for lighting, integrator, estimator, sampling settings
or other changes that invalidate radiance globally. There is no automatic
camera-cut heuristic or automatic scene-wide lighting-change classification.

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
reset rules remain in effect; this entry point does not accumulate temporal
radiance.

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
surface. GPU fetch/rejection belongs to the next implementation slice.

The primary-surface pass has its own persistent timestamp pair:
`timestamp_available` and `primary_surface_gpu_ms`, covering the buffer clear
and surface draw, excluding metadata host writes and surface readback.
Unavailable timestamps report false/zero. `surface_bytes` counts both device
buffers and their two-buffer host readback mirror (256 bytes per target pixel);
`metadata_bytes` counts the 224-byte camera block and the retained capacity
of the 96-byte-per-instance metadata buffer. Surface buffers/readback persist
at a fixed extent and are replaced on resize. Instance capacity grows when
needed. No pipeline or timestamp pool is created in a render call. Targets
or instance tables exceeding the device storage-buffer range fail explicitly.

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
([GPU evidence](../reports/2026-10-12-temporal-primary-surfaces.md)). Its
Vulkan-disabled configuration and missing-driver regression report explained
SKIPs.

History fetch/filtering, GPU shared validity, temporal radiance accumulation
and Hydra temporal settings/AOV bindings remain outside this implementation.
Geometry deformation is conservatively rejected. No ReSTIR or denoiser state
is introduced and no temporal image-quality or convergence claim is made.
