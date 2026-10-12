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
| Width or height differs | `TargetChanged` | absent |
| Caller changes `history_key` | `HistoryKeyChanged` | absent |
| Otherwise, including ordinary camera or object motion | `None` | present |

Zero width/height and duplicate or decreasing indices throw
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
to the image; bottom/right edges do not. This helper currently supports only
full-frame targets, with no cropped display/data windows or jitter offsets.

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

## Entry points and limits

CTest `lotus-renderer-temporal` exercises snapshot lifetime, reset recovery,
invalid-input atomicity, stable placement pairing, material/texture edits,
independent pinhole projections, camera/object motion, clipping, disocclusion,
normal and identity rejection, and threshold boundaries. It runs without a
GPU ([measured checks](../reports/2026-10-12-temporal-foundation.md)).

The existing reference and wavefront render paths do not yet consume this
API. Their progressive accumulation still resets on camera and scene edits;
no image behavior changes. GPU history buffers, motion/identity AOVs, history
fetch/filtering, GPU validity, radiance accumulation and Hydra temporal
settings remain outside this implementation. It provides no GPU pass or GPU
timestamp, and makes no image-quality or performance claim.
