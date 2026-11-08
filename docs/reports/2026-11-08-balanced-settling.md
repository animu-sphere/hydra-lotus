# Interactive quality: Balanced settling

Checked 2026-11-08 on Windows 11, MSVC 14.51 Release, NVIDIA RTX A5000
(driver 597.16), clocks not locked. Source base: main `201fa32` plus the
LOTUS-INTERACTIVE-04 change. The
[interactive-quality contract](../design/INTERACTIVE_QUALITY.md) owns the
presets and [settling's requirements](../design/INTERACTIVE_QUALITY.md#4-settling);
the [temporal reference](../reference/TEMPORAL.md#settling) owns the
implemented behaviour; the [roadmap](../roadmap/README.md#status-at-a-glance)
owns phase status.

## Question

LOTUS-INTERACTIVE-04 adds the Balanced preset: Interactive while anything
changes, and once the view is still, a settling that must (1) end at an image
that passes the statistical comparison with Reference at the same sample count
and holds only still samples, (2) converge every pixel, misses included,
(3) step no frame of the transition above the moving view's mean frame-delta
MSE, and (4) report convergence at the sample target and trace nothing more.

## What changed

**Settling in the backend.** `TemporalFrameSettings::settle` (Balanced's) makes
a frame *still* when its camera, target and windows, scene snapshot and
transport key equal the preceding frame's, no cut is declared, the preceding
frame also had `settle`, and no reservoir reuse is selected. A still frame
traces no fresh batch: it continues the progressive accumulation `RenderScene`
keeps, restarted by the first still frame at the host's sample index and
stopped at `max_samples`. A crossfade pass writes the colour from it and the
*prior*, the last changing frame's displayed (filtered) colour, which that
frame copied into a second half of the batch buffer. The prior counts as the
history cap's 32 samples wherever that frame had a history and as none on a
miss; with `n` still samples and `w = min(32, sample target)` the colour is
`prior + (still - prior) * n / w`, and exactly the still mean once `n >= w`.
`SettleTemporalRadiance` is the CPU oracle.

So the mean and the filter leave together within 32 samples; requirement 1 is
met exactly rather than statistically, because at the target the colour *is*
the progressive image; requirements 2 and 4 come with the progressive
accumulation. The radiance history and the stream's RNG offset are left as
the last changing frame left them, so the next change continues them: the
contract's "the history cap returns to 32" holds with no raised cap. The
surface and validity passes still run, so debug products stay current. The
cost of `settle` is one 16-byte-per-pixel image copy per changing frame and
16 device bytes per pixel; a still frame runs one crossfade pass instead of
the accumulation, variance and five filter passes.

**Hosts.** `ApplyQualityPreset(QualityPreset::Balanced)` is Interactive's three
settings plus `settle`. Hydra adds `lotus:balanced` (bool, false); with
`lotus:interactive` also on, Interactive wins. A settling pass reports
convergence once its still samples reach `convergedSamplesPerPixel`. The
viewport adds `--preset balanced`, Balanced in the Quality list and *Settle
when still* under temporal radiance; its frame check accepts that a settling
frame runs no filter.

**Products.** A settling frame reports `settling`, `settled_samples`, the
samples it added as `batch_samples`, and `settle_gpu_ms`; `batch` is the still
mean, `radiance` the retained history and `previous_radiance` empty.

## The first mechanism failed requirement 3

The first implementation crossfaded each pixel over its own history length
`h`, as the contract's sketch suggested. Every workload converged to the
Reference image bit for bit, but on `deforming-sheet-v1` the first still
frame's frame-delta MSE was 0.00458 against the moving view's 0.00179, and on
the moving room the first still frame's MSE rose from 0.0295 to 0.0361. The
sheet's pixels are rejected every frame, so `h = 1` and they dropped from the
filtered colour to one raw sample at once; disoccluded pixels in the room did
the same. The filtered colour stands for its neighbourhood, not for `h`
samples, so the prior now counts as the cap. Both runs used the procedure
below.

## Measurements

`lotus-headless --benchmark-svgf` (schema `lotus-svgf-benchmark-v3`) gains a
`settling` series per workload: its 32 frames from a cut, then the last pose
held for 64 frames under a sample target of 64, shown as Balanced, Interactive
and Reference (the progressive render, restarted on every change) would show
it. The procedure is the
[baseline's](2026-11-08-interactive-baseline.md#procedure) 32-frame one:
128x128, one sample per frame, cap 32, five filter iterations, four bounces,
NEE/MIS, eight sequences, references of two 1024-sample halves.

```sh
lotus-headless --benchmark-svgf build/interactive-balanced/interactive-balanced-reference.json --benchmark-label "main 201fa32 plus LOTUS-INTERACTIVE-04; Release; 2026-11-08; dynamic clocks; validation layers disabled" --benchmark-integrator reference --benchmark-size 128 --benchmark-frames 32 --svgf-sequences 8 --svgf-reference-samples 1024 --svgf-hold 64
```

The same with `wavefront`, serially, with
`VK_LOADER_LAYERS_DISABLE=VK_LAYER_KHRONOS_validation`; runs took 82–99 s.
**Both integrators produced identical settling series.** The
[summary JSON](2026-11-08-balanced-settling.json) keeps the procedures, the
device and every workload's series; raw reports remain under
`build/interactive-balanced/`. The other outputs of the comparison are those
of the [presets report](2026-11-08-interactive-presets.md#effect-on-the-fixed-fixtures):
a changing Balanced frame is an Interactive frame.

### Requirements

| Workload | Moving view's frame-delta MSE | Largest hold frame-delta MSE (hold frame) | No pop | Last frame is Reference's, bit for bit |
| --- | --- | --- | --- | --- |
| `svgf-room-static-v1` | still workload | 0.000436 (0) | — | yes |
| `svgf-room-moving-camera-block-v1` | 0.0471 | 0.000415 (18) | yes | yes |
| `occluder-shadow-sweep-v1` | 0.0000269 | 0.000000417 (2) | yes | yes |
| `glossy-floor-camera-pan-v1` | 0.155 | 0.00197 (22) | yes | yes |
| `deforming-sheet-v1` | 0.00179 | 0.000393 (28) | yes | yes |

Every hold frame of every moving workload stays below its moving view's mean:
at most 0.9% to 1.6% of it, and 22% on the deforming sheet. Every sequence settled on every hold frame (64
per sequence; 95 in the still room, which is still from its second frame) and
ended on the Reference frame bit for bit, so the statistical comparison passes
by identity, every pixel included.

### How fast the still view becomes usable

MSE against the pose's reference after the view stops, averaged over
sequences; *last moving* is the frame before the hold.

| Workload | Shown | Last moving | Hold 1 | 8 | 16 | 32 | 64 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| room, moving | Balanced | 0.0295 | 0.0278 | 0.0190 | 0.0123 | 0.0117 | 0.00587 |
| | Interactive | 0.0295 | 0.0287 | 0.0245 | 0.0220 | 0.0189 | 0.0187 |
| | Reference | 0.344 | 0.186 | 0.0402 | 0.0216 | 0.0113 | 0.00587 |
| shadow sweep | Balanced | 0.000167 | 0.000157 | 0.0000964 | 0.0000466 | 0.00000804 | 0.00000405 |
| | Interactive | 0.000167 | 0.000157 | 0.000105 | 0.0000652 | 0.0000251 | 0.00000456 |
| | Reference | 0.000249 | 0.000127 | 0.0000282 | 0.0000150 | 0.00000780 | 0.00000405 |
| glossy pan | Balanced | 0.709 | 0.666 | 0.408 | 0.197 | 0.0384 | 0.0199 |
| | Interactive | 0.709 | 0.674 | 0.448 | 0.276 | 0.111 | 0.0330 |
| | Reference | 1.217 | 0.611 | 0.138 | 0.0721 | 0.0374 | 0.0199 |
| deforming sheet | Balanced | 0.0190 | 0.0182 | 0.0146 | 0.0120 | 0.0114 | 0.00583 |
| | Interactive | 0.0190 | 0.0190 | 0.0191 | 0.0188 | 0.0177 | 0.0181 |
| | Reference | 0.363 | 0.182 | 0.0398 | 0.0212 | 0.0112 | 0.00583 |

Balanced is never worse than Interactive and matches Reference at the target.
Where the moving history lags, the prior costs accuracy early in the hold: on
the glossy floor and the shadow sweep Reference's fresh accumulation is ahead
from about the eighth still frame until the prior has gone, by 3.0 and 3.4
times at 8 frames. In exchange Balanced's mean hold frame-delta MSE on the
glossy floor is 0.00125 against Reference's 0.0187, and Reference starts each
hold from a single sample. On the room and the sheet, whose histories do not
lag, Balanced is ahead of Reference for the first 16 still frames and within
4% of it at 32. The prior is a fixed span, not a measured weight; a span that
adapts to the lag is a candidate for LOTUS-INTERACTIVE-05's measurements, not
a requirement here.

## Tests

| Requirement or row | Checked by |
| --- | --- |
| Still accumulation is the progressive render | `lotus-renderer-temporal-gpu`, both integrators: every settling frame's still mean against an independent progressive render, bit for bit |
| Crossfade | The same test: every pixel of every settling frame against `SettleTemporalRadiance`, and pixels past the span equal to the still mean bit for bit; `lotus-renderer-temporal` checks the oracle |
| Correct end, every pixel, stops | The same test, on a card with misses around it: the colour at the sample target equals the Reference render bit for bit and does not change on the two frames after; `samples_per_pixel` stays at the target |
| History and samples resume | The same test: the retained history is unchanged on every settling frame, and the next changing frame reads it and continues the stream's sample index |
| Switching between Interactive and Balanced: none | The same test both ways; `lotus-renderer-hydra-temporal-gpu` from `lotus:interactive` to `lotus:balanced` |
| A change ends settling; a cut does not settle | The same test |
| Settling needs radiance accumulation | The same test: the call fails |
| Hydra convergence | `lotus-renderer-hydra-temporal-gpu`: `lotus:balanced` settles, converges at `convergedSamplesPerPixel`, keeps the converged colour, equals the Reference preset's converged colour bit for bit, and loses to `lotus:interactive` |
| Viewport | `lotus-viewport-controls`, `lotus-viewport-options` (preset, Custom, `--preset balanced`); `lotus-viewport --preset balanced --samples 16 --frames 40 --hidden` presented 40 frames and ended at 16 spp |
| Report | `lotus-renderer-svgf-benchmark-*` require every workload to settle and end on the Reference frame |

## Regression

Built in the research, Hydra (`lookdev--hydra`), core viewport and USD viewport
trees.

- `lotus-renderer-temporal`, `-temporal-gpu`, `-restir-gpu`,
  `lotus-viewport-controls`, `lotus-viewport-options` (research and core
  viewport trees), `lotus-renderer-restir-benchmark-reference-animation`,
  `lotus-renderer-svgf-benchmark-reference` and `-wavefront`.
- `lotus-renderer-hydra-temporal-contract`, `-temporal-gpu`, `-integrator`,
  `-restir-contract`, `-restir-gpu`, `-material-fixtures-gpu`, `-skel-gpu`.

Reference stays byte-identical: the build-time headless report passes all 36
assertions, among them `renderer.path.reference` (the committed reference
images reproduced bit for bit) and `renderer.path.wavefront`. Without
`settle` no code path changed except the radiance history's own write index,
which flips with the surfaces' as before.

## Limits

- Not rerun: the full CTest suites, the ci-core tree and the viewport's Hydra
  path in a window.
- `settle_gpu_ms` is reported but not measured here; the hold frames' cost
  was not compared with the changing frames'.
- Each stop restarts the still accumulation at the host's sample index, so a
  view held twice shows the same noise twice, as Reference does
  ([DES-Q7](../design/DESIGN_POLICY.md#53-open-questions)).
- Leaving a settled view returns to the last changing frame's history; the
  still samples do not seed it.
- With reservoir reuse Balanced never settles.
