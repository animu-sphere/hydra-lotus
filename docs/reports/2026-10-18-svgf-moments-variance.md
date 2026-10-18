# SVGF temporal accumulation, moments and variance

Measured 2026-10-18 on Windows 11, MSVC 14.51 Release, Vulkan SDK 1.3.290
with the Khronos validation layer, NVIDIA RTX A5000 (driver 597.16), clocks
not locked. Source base: main `0726e68` plus the LOTUS-SVGF-01 change. The
[temporal reference](../reference/TEMPORAL.md#luminance-moments-and-variance)
owns the contract; the [roadmap](../roadmap/README.md#status-at-a-glance)
owns phase status.

## Question

[LOTUS-SVGF-01](../roadmap/current.md#step-3--svgf-core) is the first half
of the [SVGF-class baseline](../design/ROADMAP_POLICY.md#renderer-phase-6--denoising--svgf-class-pipeline):
reprojection and history validation through the existing temporal
infrastructure, temporal accumulation, first and second moments and variance
estimation, with timestamps and debug products, from noisy radiance and
shared surface metadata only. Renderer Phase 4 already reprojects, validates
and accumulates radiance
([accumulation evidence](2026-10-12-temporal-radiance.md)); what it lacked was
the moments, the variance and their products. This report records what was
added, how it is checked, and the decision on
[DES-Q7](../design/DESIGN_POLICY.md#53-open-questions).

## What changed

- **Moments.** The temporal accumulation pass keeps the first and second
  moments of each fresh batch mean's Rec. 709 luminance and blends them with
  the colour's bounded weights. They occupy the three words of
  `TemporalRadiancePixel` that were reserved, so the record stays 32 bytes
  and the history buffers do not grow.
- **Variance.** A new fullscreen pass after the accumulation, once every
  pixel's moments exist, writes the variance of one batch mean's luminance:
  the temporal variance `m2 - m1^2` from a history of four batches or more,
  otherwise SVGF's spatial estimate from the 7x7 neighbourhood's moments,
  weighted by geometric normal, linear depth against the local depth
  gradient and luminance, scaled by `4 / batches`. Unlike SVGF the
  neighbourhood is limited to the same placement, using the stable instance
  identity the temporal metadata already carries.
- **Products.** `TemporalProducts::batch` reads back the raw radiance (the
  fresh batch the accumulation read); `radiance` and `previous_radiance` now
  carry the history length, moments and variance, and the reprojected history
  is `previous_radiance[validity.previous_index]`; validity and rejection
  reasons were already products. `variance_gpu_ms` times the new pass; the
  temporal timestamp pool grew from six queries to eight. Radiance memory is
  160 bytes per pixel, 16 more for the batch's host mirror.
- **Oracle.** `TemporalLuminance` and `EstimateTemporalVariance` in
  `temporal.hpp` implement the same arithmetic on the CPU, in float and in
  the GPU's order.

Nothing reads the integrator, the light estimator or the reservoirs, so the
products are the same for NEE/MIS, initial-only and reusing ReSTIR.

## Checks

`lotus-renderer-temporal` (fast tier) adds hand-computed cases: a long
history's own variance and its clamp at zero, a uniform neighbourhood at the
centre and a corner, the batch-count boost including a batch above the
history cap, exclusion of other placements, facing-away normals and misses,
malformed input, and the edge-stopping weights on a depth ramp.

`lotus-renderer-temporal-gpu` (gpu tier) extends its radiance sequence, which
already renders every fresh batch independently and compares every
colour and count, under both integrators:

| Check, per integrator | Result |
| --- | --- |
| Pixels compared (static, cap saturation, camera/object motion, every reset reason, failed calls, disocclusion) | 259,584 |
| Raw radiance against the independent batch | within 2e-5 at every pixel |
| Moments against double-precision weighted means of the independent batches' luminance | max relative error 2.60e-7 |
| `m2 >= m1^2` at every pixel | holds |
| Variance against the CPU oracle | max absolute error 4.35e-6 |
| Pixels with a temporal / spatial estimate | 54,368 / 193,184 |
| Exposed background after a foreground leaves | 638 pixels, fresh moments, spatial variance |
| Misses | zero moments and variance |
| Validation messages | none |

The two integrators produced identical figures. On the static view (GGX
metal under a constant environment, two-sample batches) the first frame's
mean spatial estimate is 0.0674 and the mean temporal variance after eight
batches is 0.0156. Undoing the `4 / b` boost and the `(b - 1) / b` bias of a
population variance gives 0.0169 and 0.0178: the two estimators agree on the
noise they measure. These means are printed, not asserted, as the
[test policy](../design/TEST_POLICY.md#8-svgf) keeps quality figures out of
the regression tiers.

`lotus-renderer-restir-gpu`, which runs temporal radiance under reservoir
reuse (the path that records the shared metadata before transport), passed
too.

## Cost

At 64x48 the accumulation draw took 0.036 ms and the variance draw
0.025 ms, observed with the validation layer and dynamic clocks. These are
not viewport figures; denoise timing at viewport resolution joins the HUD with
LOTUS-SVGF-03.

## DES-Q7

The question was whether fresh temporal batches should keep advancing the
RNG across scene-edit resets, because restarting at `sample_index` replays
random numbers and correlated reuse noise across edits (about 0.58 in the
[animation evidence](2026-10-17-restir-animation.md#replayed-random-numbers)),
and "temporal filtering is the consumer that would notice".

It does not notice. A scene edit resets with `SceneChanged`, which drops the
previous frame: every pixel rejects history (`HistoryUnavailable`), its count
restarts at the batch, its moments at `(L, L^2)`. Samples replayed after an
edit therefore never meet the samples they repeat in a colour mean, a moment
or a temporal variance. The spatial variance estimate combines pixels of one
frame, whose random numbers already differ by pixel. The restart stays, and
a frame stays reproducible from its settings alone. What remains is visible
rather than statistical: under continuous edits the noise pattern stands
still instead of changing, and a comparison across edits (a temporal
residual under animation in LOTUS-SVGF-02) has to account for it.

## Regression

Run after the change, in the core configuration: `lotus-renderer-temporal`,
`lotus-renderer-temporal-gpu`, `lotus-renderer-restir-gpu` and the `fast`
tier. Not rerun: the rest of the `gpu` tier, the Hydra configuration (the
adapter reads only `sample_count` of the changed record), the runtime-free
`ci-core` configuration and the viewport builds.

## Limits

- The variance assumes batches of one size; a stream whose batch size
  changes mixes their variances.
- Geometric normals only guide the spatial estimate; shading normals and
  albedo are for the filter (LOTUS-SVGF-02) to decide.
- No viewport views of history length, moments or variance yet, and no Hydra
  AOVs for them; the views come with LOTUS-SVGF-03.
- No filtering: the colour is still the bounded temporal mean.
