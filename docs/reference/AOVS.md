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

All other names have an invalid default descriptor. Normal, albedo,
roughness, path depth, throughput and the other debug channels are introduced
with the renderer passes that produce them. Each Hydra pass adds one
radiance sample; colour converges when the accumulation reaches the
`convergedSamplesPerPixel` render setting (64 by default), depth and IDs
after one pass. The `lotus:sampleIndex` render setting fixes the samples'
random numbers ([deterministic mode](SCENE.md#deterministic-mode-through-hydra)). The render pass's `IsConverged` follows the colour, so a host
that waits for convergence redraws until then.

## Storage and binding

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

An empty clear value preserves preceding attachment contents across frames
at the same target size. The offscreen renderer retains one colour/depth
attachment pair for the bootstrap and one for the scene passes; this
foundation path assumes successive passes reuse their bound buffers. A
radiance frame rewrites every pixel any of its samples hit. It does not restore arbitrary buffer contents after switching
between different AOV buffer sets. Newly created or resized targets initialize
colour to transparent black and depth to 1 when no clear is supplied; old
pixels are not carried across a resize. ID buffers remain untouched when no
clear is supplied.

With the default descriptors' clears, hiding or removing the last mesh
produces transparent black, depth 1 and ID -1, and still converges. An empty
scene therefore replaces the preceding triangle instead of keeping its
image.

Standalone/headless callers supply clear values and enable flags in
`Lotus::OffscreenTarget`. Its defaults clear every frame to transparent black
and depth 1. Clear changes reuse the same targets; only an extent change
recreates them.
