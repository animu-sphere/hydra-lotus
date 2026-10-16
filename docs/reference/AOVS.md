# Foundation AOVs

The Renderer Phase 0 AOV set, also used by the ray-traced scene pass.
The [capability matrix](CAPABILITY_MATRIX.md) owns implementation status;
[design policy section 25](../design/DESIGN_POLICY.md#25-debug-and-validation)
owns the later debug channels. The measured run is the
[foundation AOV report](../reports/2026-10-04-foundation-aovs.md).

## Channels

| Hydra name | Hydra buffer format | Backend product | Default clear | Meaning today |
| --- | --- | --- | --- | --- |
| `color` | `HdFormatFloat32Vec4`; `HdFormatUNorm8Vec4` also accepted | `rgba32-sfloat` from the scene passes, `rgba8-unorm` from the bootstrap; linear RGBA | `(0, 0, 0, 0)` | On ray-query devices, the mean of the accumulated path-traced radiance samples, box-filtered, unclamped and not tone mapped; a missed sample counts as the clear colour, so with a transparent clear alpha is coverage ([scene reference](SCENE.md#accumulation-and-the-pixel-filter)). Bootstrap colour otherwise. An 8-bit buffer receives the value clamped to [0, 1] |
| `depth` | `HdFormatFloat32` | `d32-sfloat` | `1.0` | Vulkan window depth in [0, 1], near 0 and far 1; not linear distance |
| `primId` | `HdFormatInt32` | CPU sentinel only | `-1` | No scene primitive identification yet |

`instanceId` and `elementId` are accepted compatibility channels with the
same Int32 sentinel behaviour as `primId`. They do not identify instances or
triangles. The primary-ray pass does not publish geometry IDs, so none of
the ID channels supports picking. These are CPU placeholders, not GPU ID
attachments or additional headless render products. The required headless
products remain `color` and `depth`.

The opt-in backend `RenderTemporalScene` additionally exposes primary linear
depth, geometric normal, stable identities and backward motion in
`GpuFrameEvidence::temporal` ([temporal product contract](TEMPORAL.md#gpu-primary-surface-products)).
It also publishes shared validity/rejection/fetch indices and, when radiance
accumulation is enabled, linear RGBA and per-pixel bounded history counts
([radiance contract](TEMPORAL.md#temporal-radiance-accumulation)).
The adapter exposes selected records through the temporal channels below.

Names outside the foundation, temporal and reservoir channels have an invalid default
descriptor. Normal, albedo,
roughness, path depth, throughput and the other debug channels are introduced
with the renderer passes that produce them. Each Hydra pass adds one
radiance sample; colour converges when the accumulation reaches the
`convergedSamplesPerPixel` render setting (64 by default), depth and IDs
after one pass. The `lotus:sampleIndex` render setting fixes the samples'
random numbers ([deterministic mode](SCENE.md#deterministic-mode-through-hydra)),
the `lotus:wavefront` flag selects the integrator that traces them
([integrator selection](SCENE.md#integrator-selection-through-hydra)), and
the `lotus:lightSampling` flag the estimator
([light sampling](SCENE.md#light-sampling-through-hydra)). The render pass's `IsConverged` follows the colour, so a host
that waits for convergence redraws until then. Temporal radiance and
reservoir reuse instead remain unconverged, as described in the
[Hydra temporal contract](TEMPORAL.md#hydra-controls-and-debug-aovs) and
[reservoir controls](RESTIR.md#hydra-controls-and-diagnostics).

## Temporal Hydra channels

| Hydra name | Format | Default clear | Product |
| --- | --- | --- | --- |
| `lotus:motion` | `HdFormatFloat32Vec4` | `(0, 0, 0, 0)` | Backward motion XY in top-left pixel coordinates, expected previous linear depth Z, successful projection flag W |
| `lotus:historyLength` | `HdFormatInt32` | `0` | Effective bounded radiance sample count; zero on misses and in metadata-only mode |
| `lotus:historyValidity` | `HdFormatInt32` | `0` | Shared validity, 1 when accepted and 0 when rejected |
| `lotus:historyRejection` | `HdFormatInt32` | `0` | Numeric `TemporalRejection` from the [shared rejection contract](TEMPORAL.md#gpu-reprojection-and-validity) |

Binding any channel above enables GPU primary-surface and shared-validity
products. `lotus:temporal` independently selects radiance reuse. Debug
channels are overwritten each pass, including empty scenes and pixels
outside the data window; empty clears do not preserve old debug records.
Motion components retain the backend convention while storage rows flip
to Hydra's bottom-up order. The validity flag is distinct from the motion
W projection flag. These buffers finish after one pass; colour and the
render pass follow the selected temporal/progressive mode.

## Reservoir Hydra channels

These describe the last fresh sample's final reservoir, as defined in the
[Hydra reservoir contract](RESTIR.md#hydra-controls-and-diagnostics).
Rows flip to Hydra storage order; vectors keep their backend conventions.

| Hydra name | Format | Default clear | Product |
| --- | --- | --- | --- |
| `lotus:restirLight` | `HdFormatInt32` | `-1` | Generation-local selected light-table slot; -1 for no sample, unrelated to picking IDs |
| `lotus:restirCandidates` | `HdFormatInt32` | `0` | Represented draws, including zero-target draws and capped reuse sources |
| `lotus:restirSupport` | `HdFormatInt32` | `0` | Supporting count used for corrected normalization; initial-only uses represented count |
| `lotus:restirAge` | `HdFormatInt32` | `0` | Selected temporal sample age; initial-only has age zero |
| `lotus:restirWeight` | `HdFormatFloat32Vec4` | `(0, 0, 0, 0)` | Raw weight sum, selected target, resolved normalization, selected proposal probability |
| `lotus:restirDirect` | `HdFormatFloat32Vec4` | `(0, 0, 0, 0)` | Selected normalized shadowed RGB and current transmittance W |

These AOVs overwrite the full frame, including outside the data window,
regardless of clear values. They do not enable reservoirs: NEE/MIS and BSDF
write zero values and the empty light sentinel. Progressive convergence
retains the last fresh reservoir rather than clearing diagnostics. The
weight vector carries the explicitly corrected normalization after reuse;
it is not reconstructed from the raw sum and total count. Direct light is
the fresh selected contribution before the receiver's
[BSDF partition](RESTIR.md#emission-accounting), separate from accumulated
colour/indirect transport. Generation-local light slots cannot track identity across scene
edits. Correlated represented counts are not independent sample counts.

## Storage and binding

On ray-query devices, depth is the pixel centre's closest alpha-accepted
surface. Cut-outs apply their mask; fractional opacity uses one fixed
per-pixel coverage realization at sample index 0, independent of progressive
colour samples. Depth is not averaged. With a transparent clear, colour
alpha also accounts for material presence coverage
([scene reference](SCENE.md#primary-rays)).

Buffers are single-sample, two-dimensional (`dimensions.z == 1`). Allocation
accepts only the four formats above. Zero width or height can be allocated,
but an empty buffer cannot be bound for rendering. Negative dimensions,
volume buffers, other formats and multisampling are rejected.

A pass accepts any nonempty subset of the channels, including depth alone.
Bound buffers must be Lotus buffers, match the name's format, share one
positive extent, and be unmapped. Duplicate names and invalid bindings fail
the entire pass before GPU work or writes; bound Lotus buffers become
unconverged. A successful write, including an empty scene, converges the
buffer. `Map` gives CPU storage; writes and reallocation are rejected while
it is mapped.

Backend colour and depth products have tightly packed rows and a top-left
origin. Row pitch is in bytes: `width * 16` for RGBA32F colour, `width * 4`
for RGBA8 colour and for depth. The adapter flips the rows when writing
Hydra's bottom-up buffers and converts colour between the product's and the
buffer's format, clamping and rounding into 8 bits. It does not scale one
product to a differently sized AOV.

## Clears and successive frames

The adapter consumes the host's
[`HdRenderPassAovBinding`](https://openusd.org/release/api/struct_hd_render_pass_aov_binding.html)
clear value. A supplied colour clear must be a finite `GfVec4f`, a depth clear
a finite `float` in [0, 1], and an ID clear an `int`. Colour conversion clamps
to UNORM; half-integer byte values may round in either direction. A supplied
clear applies to the whole buffer, including pixels outside the framing's
data window. ID channels write the requested sentinel across the whole
buffer, including triangle pixels.

An empty clear value preserves the currently bound CPU buffer's contents.
Before drawing, the adapter restores that channel's contents into the GPU
attachment, converting UNORM colour to linear floats and flipping bottom-up
rows. Switching buffer sets, a depth-only pass, host writes through `Map`,
and returning after another buffer's resize therefore preserve the bound
buffer rather than the preceding GPU image. Same-size reallocation uses the
new CPU storage (zero-initialized by `Allocate`), without resurrecting old
pixels. ID buffers remain untouched when no clear is supplied.

A radiance frame rewrites every pixel any of its accumulated samples hit;
pixel-centre depth hits overwrite the restored depth under the usual depth
test. Misses and pixels outside the data window retain the restored values.
Restoration does not restart radiance accumulation or change its background
sample convention. Each bound colour/depth channel with no clear requires a
CPU copy and a GPU upload, using the persistent readback buffers as staging;
it creates no additional GPU allocation. This synchronous foundation path
does not cache CPU contents, since the host may edit them between passes.
See the [buffer restoration evidence](../reports/2026-10-09-aov-restoration.md).

With the default descriptors' clears, hiding or removing the last mesh
produces transparent black, depth 1 and ID -1, and still converges. An empty
scene therefore replaces the preceding triangle instead of keeping its
image.

Standalone/headless callers supply clear values and enable flags in
`Lotus::OffscreenTarget`. Its defaults clear every frame to transparent black
and depth 1. Clear changes reuse the same targets; only an extent change
recreates them.

Backend callers can supply tightly packed top-left `preserved_color` (linear
RGBA floats) and `preserved_depth` (window-depth floats) in `OffscreenTarget`.
Nonempty arrays must match the target extent. With the respective clear
disabled, they restore contents before the first frame of a render call,
including when targets are created or resized. Without arrays, a disabled
clear retains the preceding GPU attachment; new targets initialize to the
clear values. Enabled clears take precedence over restoration arrays.
