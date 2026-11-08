# Interactive quality: ghosting candidates

Measured 2026-11-09 on Windows 11, MSVC 14.51 Release, Vulkan 1.4.329 with
validation layers disabled through the loader, NVIDIA RTX A5000 (driver
597.16), clocks not locked. Source base: main `e76dac4` plus the
LOTUS-INTERACTIVE-05 change. The
[interactive-quality contract](../design/INTERACTIVE_QUALITY.md) owns the
adoption and stop rule; the
[temporal reference](../reference/TEMPORAL.md#history-rectification) owns
the candidates' implemented behaviour; the
[roadmap](../roadmap/README.md#status-at-a-glance) owns phase status.

## Question

The [baseline](2026-11-08-interactive-baseline.md#moving-stability-lag-and-disocclusion)
found the temporal mean worse than one fresh sample in two lag regions: the
shadow swept over a still floor (1.9 times) and the metal floor under a
panning camera (1.6 times). LOTUS-INTERACTIVE-05 tries the three candidates
the contract's
[§7](../design/INTERACTIVE_QUALITY.md#7-measuring-still-and-moving-separately)
names, a radiance neighbourhood clamp, anti-lag and motion-aware reflection
weighting, on the fixed fixture set, and adopts one only if it lowers the
lag metric on its fixture without raising any other row and leaves Reference
byte-identical.

## Decision

**No candidate is adopted; this is the contract's measured negative, and
LOTUS-INTERACTIVE-05 is closed.** Each candidate lowers the lag metric on
its fixtures, so ghosting can be reduced, but each also raises other rows:

- The **neighbourhood clamp** and **anti-lag** raise the still room's error
  and flicker. Their clamp and their lag test cannot tell lag from the
  one-sample noise of the neighbourhood they compare against, so they
  discard a correct history in every workload. The still room's filtered
  MSE rises 2.6 and 1.4 times.
- **Reflection weighting** changes nothing that does not move over a
  low-roughness metal. On the glossy floor it lowers every row of the
  filtered image, but the unfiltered temporal mean's frame-delta MSE rises
  2.6 times and its 2–3-batch bucket by 1.8%.

The rule was applied to both the filtered composite and the temporal mean,
as declared before the runs below (next section). The temporal mean is the
history, and it is the image a host shows with the filter off. Under that
reading reflection weighting fails on its own fixture. Judged on the
filtered image alone, the image Interactive and moving Balanced show, it
would pass. That reading was considered and not taken. Revisiting it would
need a contract change, not a new fixture
([strategy §6](../design/POST_V060_STRATEGY.md#6-research-track-and-stop-rule)).

The candidates remain as an experimental setting,
`TemporalFrameSettings::history_rectification`, which defaults to `None`. No
preset, Hydra setting or viewport control selects them; the headless
benchmark's `--svgf-rectification` reproduces this measurement. With `None`
the renderer is today's bit for bit: every output of every workload matches
the [settling campaign](2026-11-08-balanced-settling.md) to every digit.

## Candidates and rule

Each candidate's definition and parameters were fixed before any run and
not tuned afterwards. Every candidate acts in the accumulation pass on a
pixel whose history shared validity accepted. It changes either the colour
it keeps or how many samples of history it keeps; validity, the fresh
batch, the moments' weights and the filter are unchanged. All three work in
illumination: colour over the primary albedo, as the filter uses it.

| Candidate | Changes | Definition |
| --- | --- | --- |
| Neighbourhood clamp | The kept colour | Clamp each channel to the 3x3 fresh batch's mean ± one standard deviation, over neighbours on the same placement (variance clipping with γ = 1) |
| Anti-lag | The kept count | Scale it by 1 − saturate((z − 2) / 2), where z is the difference between the 5x5 same-placement neighbourhood's fresh and reprojected mean luminance, over the fresh mean's standard error: none below two standard errors, all of it from four |
| Reflection weighting | The kept count | Scale it by 1 − s + s / (1 + (θ / a)²), where θ is the angle between the surface point's directions to the previous and current camera, a = max(roughness², 0.001) the GGX lobe's width and s = metallic + 0.04 (1 − metallic) the specular share. The primary-surface pass writes the factor |

Scaled counts round to the nearest sample. None of the three adds a pass,
a pipeline or a buffer.

**Rule as declared.** A candidate's fixture is the shadow sweep for the
clamp and anti-lag, and the glossy pan for all three. On each fixture the
lag-region MSE must fall in both the temporal mean and the filtered
composite. No other row of any workload may rise in either output: mean
MSE, frame-delta MSE, MSE by history length, settling's convergence to
Reference and its no-pop check. Reference, the progressive output, must
stay byte-identical. Cost is reported.

## Procedure

As the [baseline](2026-11-08-interactive-baseline.md#procedure): 128x128,
32 frames, eight sequences, one fresh sample per frame, history cap 32, five
filter iterations, four bounces, NEE/MIS, 1024-sample reference halves, and
the settling series with a 64-frame hold. One run per candidate, serially,
with `VK_LOADER_LAYERS_DISABLE=VK_LAYER_KHRONOS_validation`, 73–79 s each:

```sh
lotus-headless --benchmark-svgf build/interactive-ghosting/reflection-reference.json --benchmark-label "main e76dac4 plus LOTUS-INTERACTIVE-05; Release; 2026-11-09; dynamic clocks; validation layers disabled" --benchmark-integrator reference --benchmark-size 128 --benchmark-frames 32 --svgf-sequences 8 --svgf-reference-samples 1024 --svgf-rectification reflection
```

The four modes `none`, `clamp`, `anti-lag` and `reflection` ran under the
reference integrator, and `none` and `reflection` again under `wavefront`.
**Wavefront reproduced the reference integrator's outputs exactly** in both,
and every run was deterministic. The
[summary JSON](2026-11-09-ghosting-candidates.json) keeps the procedure,
device and each mode's rows per workload; raw reports and PFMs remain under
`build/interactive-ghosting/`.

## Results

Ratios are candidate over `none`; below one is better. "Unchanged" is bit
for bit.

**Lag.** Lag-region MSE, temporal / filtered:

| Workload | None | Clamp | Anti-lag | Reflection |
| --- | --- | --- | --- | --- |
| Shadow sweep | 1.27e-3 / 1.20e-3 | 0.19× / 0.17× | 0.22× / 0.050× | unchanged |
| Glossy pan | 3.03 / 2.84 | 0.20× / 0.20× | 0.22× / 0.082× | 0.55× / 0.073× |
| Deforming sheet | 0.0741 / 0.00428 | unchanged | unchanged | unchanged |

The clamp and anti-lag cut both lags to about a fifth, below the fresh
batch's error (6.67e-4 and 1.93). The sheet's own pixels are rejected every
frame, so no candidate sees a history there.

**Still and the rest of the moving set.** Mean MSE and frame-delta MSE,
temporal / filtered:

| Workload | Row | None | Clamp | Anti-lag | Reflection |
| --- | --- | --- | --- | --- | --- |
| Still room | MSE | 0.0460 / 0.0305 | **1.43× / 2.60×** | **1.29× / 1.41×** | unchanged |
| | Frame-delta | 0.0109 / 8.43e-4 | **2.34× / 1.82×** | **4.15× / 2.97×** | unchanged |
| | Last frame | 0.0113 / 0.0181 | **5.44× / 4.89×** | **3.13× / 2.00×** | unchanged |
| Moving room | MSE | 0.0798 / 0.0391 | **1.07× / 2.13×** | **1.03× / 1.25×** | 1.000 / 1.000 (lower) |
| | Frame-delta | 0.126 / 0.0471 | 0.83× / **1.12×** | **1.09× / 1.08×** | 1.000 / 1.000 (lower) |
| Shadow sweep | MSE | 1.80e-4 / 1.50e-4 | 0.26× / 0.19× | 0.32× / 0.064× | unchanged |
| | Frame-delta | 3.37e-5 / 2.69e-5 | **1.06×** / 0.77× | **1.70×** / 0.26× | unchanged |
| Glossy pan | MSE | 1.04 / 0.872 | 0.30× / 0.27× | 0.34× / 0.14× | 0.62× / 0.12× |
| | Frame-delta | 0.447 / 0.155 | 0.76× / **1.02×** | **1.37×** / 0.74× | **2.63×** / 0.63× |
| Deforming sheet | MSE | 0.0513 / 0.0314 | **1.40× / 2.54×** | **1.22× / 1.40×** | unchanged |
| | Frame-delta | 0.0229 / 0.00179 | **1.63× / 1.39×** | **2.35× / 1.91×** | unchanged |

Bold marks a raised row. The still room has no lag to remove, so what the
clamp and anti-lag do there is their cost in noise. Clamped to a
one-sample neighbourhood, a long history keeps its noise floor. The still
room's 8+-batch bucket rises 2.5 times (temporal) and 3.3 times (filtered),
and the image no longer approaches the progressive one. Anti-lag drops
history wherever 25 one-sample neighbours happen to disagree with it by
two standard errors, and a heavy-tailed path-traced distribution often
does.

Reflection weighting's moving-room changes are at the fifth digit, from
the rough (0.3) metal block. Its last-frame MSE rises 0.01% in both
outputs. On the glossy pan the temporal mean, with its history shortened,
moves towards the raw batch, whose frame-delta MSE is 2.48. The filter
hides that noise, which is why the two outputs part.

**Disocclusion.** By history length, the clamp and anti-lag lower the
temporal mean's short buckets. They raise the filtered 2–3-batch bucket in
the still room (1.35 and 1.22 times) and on the sheet (1.34 and 1.56
times). Reflection weighting raises only the
glossy pan's temporal 2–3-batch bucket, 0.402 to 0.409, and lowers every
filtered bucket there (2–3 batches 0.61×, 8+ 0.039×).

**Settling and Reference.** Under every candidate, every workload's
Balanced image converged to its Reference image bit for bit, and no hold
frame exceeded the moving view's frame-delta MSE. The progressive
(Reference) output is unchanged in every workload, as it must be: no
candidate touches the progressive render. The clamp and anti-lag raised the
largest hold-frame delta in the moving room and the sheet by up to 18%,
still under the no-pop bound.

**Cost.** Accumulation-pass medians at dynamic clocks were 0.005 ms for
`none` and reflection weighting, 0.010 ms for the clamp and 0.030 ms for
anti-lag (9 and 25 taps). These are 1.024 µs timestamp ticks at
unlocked clocks, so they are observations, not the contract's cost
evidence. Locked clocks need an administrator, and with no candidate
adopted no adoption depends on them. Memory is unchanged: 2,621,440 history
and 786,432 filter bytes in every run.

## What changed

- `Lotus::TemporalHistoryRectification` and
  `TemporalFrameSettings::history_rectification` (default `None`). A mode
  other than `None` requires radiance accumulation. Changing it resets no
  history.
- The accumulation pass applies the selected candidate. Under
  `ReflectionWeight` the primary-surface pass writes its factor to
  `TemporalPixel::reserved` as float bits. Otherwise that word stays zero.
- `TemporalConstants` carries both camera positions and the mode.
- `lotus-headless --benchmark-svgf --svgf-rectification
  <none|clamp|anti-lag|reflection>`; the report's procedure records it. The
  schema stays `lotus-svgf-benchmark-v3`.

## Regression

In the `research` configuration: `lotus-renderer-temporal-gpu`, which gains
a rectification sequence under both integrators (the fresh batch is never
changed; the clamp keeps `None`'s counts; anti-lag and reflection weighting
never exceed them; a still view is `None`'s image bit for bit under
reflection weighting, and camera motion over metal shortens its history;
invalid modes fail), `lotus-renderer-temporal-gpu-no-driver` and both
`lotus-renderer-svgf-benchmark-*` tests, which also reject an unknown
`--svgf-rectification`. Not rerun: the core, Hydra, runtime-free and
viewport configurations, whose code gains only the defaulted field.

## Limits

- One run per candidate at one resolution, one speed and one parameter
  set, as the stop rule requires. A different clamp width or anti-lag
  threshold might shift the trade-off. Searching for one would extend the
  set to postpone the conclusion, which the rule forbids.
- Anti-lag here is a neighbourhood statistic, not
  [A-SVGF](https://doi.org/10.1145/3233301)'s gradient samples, which
  re-shade the previous frame's paths and need transport changes outside
  this contract's scope.
- Reflection weighting uses world-space directions, so an object's rotation
  under a still camera does not shorten its history.
- The timings were taken at dynamic clocks and are not cost evidence.
