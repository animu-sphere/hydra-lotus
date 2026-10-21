# ReSTIR reuse cost: initial-only reservoirs pay off only for many distant lights

Measured 2026-10-17 on Windows, MSVC 19.51.36256 Release, Vulkan/Slang,
NVIDIA RTX A5000 (driver 597.16) with **graphics and memory clocks locked
at 1500 and 8001 MHz**. Source: `8e179cd`, unchanged; no renderer, shader
or harness code changes. The [reservoir reference](../reference/RESTIR.md#headless-reservoir-comparison)
owns the workloads, metrics and budget semantics. This is
LOTUS-RESTIR-05B in the [execution plan](../archive/pre-v070-execution-plan.md#step-1--restir-di-closure).

## Question

The [Renderer Phase 5 exit criteria](../design/ROADMAP_POLICY.md#renderer-phase-5--restir-di)
require comparing NEE/MIS, initial-only, temporal-only, spatial-only and
combined reuse on GPU time, time to a target quality, memory and temporal
stability on representative workloads, and deciding where ReSTIR is worth
using. Earlier timings ([budgets](2026-10-15-restir-budgets.md),
[evaluation reuse](2026-10-16-restir-evaluation.md)) varied by factors of
five or more between identical cases and could not support that decision.

## Timing procedure

**Dynamic clocks made the earlier timings unusable.** A pilot at 128x128
sampled the GPU every 200 ms: only 28% of samples were at P0
(1695–1920 MHz); the rest were P5/P8 at 225–870 MHz. Each benchmark frame is
synchronous and, in reuse modes, reads back reservoir and temporal products
for 28–36 ms (see [wall time](#wall-time-and-memory)), so the GPU idles most
of the time and its clock follows that duty cycle rather than the work.
Initial-only reuse with one candidate measured 0.83 ms in one pilot and
0.14 ms in the next, and heavier cases ran at higher clocks than light ones.
Without validation layers in both, the unlocked pilot measured NEE/MIS at
0.107 ms (its clock can boost above 1500 MHz) and temporal-only at 1.832 ms;
with locked clocks they took 0.121 and 0.238 ms.

The campaign therefore ran with locked clocks (`nvidia-smi -lgc 1500,1500`
and `-lmc 8001,8001`, set by an administrator and reset afterwards). A
one-second `nvidia-smi` log covers the whole campaign: all 2,927 samples
are P0 at 1500/8001 MHz, between 51 and 61 °C. Validation layers were
disabled through the loader (`VK_LOADER_LAYERS_DISABLE=VK_LAYER_KHRONOS_validation`)
to remove their CPU overhead from wall time; every pilot PFM (101 pairs) is
byte identical with and without them, and the validated CTests cover
correctness. Runs were serial (`scripts/restir_campaign.py --jobs 1`) with no
other GPU work.

## Workloads and settings

Four workload suites, each a static scene and a moving scene, under both
integrators, with three invocations at disjoint measured seeds (`17`,
`262165`, `524313`): 24 invocations in 49 minutes of wall time.

| Suite | Scenes | Bounces | What it represents |
| --- | --- | --- | --- |
| `distant` | `many-distant-plane-v1`, `many-distant-moving-shadow-v1` | 1 | many parallel lights on a Lambert plane, moving occluder |
| `mixed-boundaries` | `mixed-area-environment-boundaries-v1`, `...-moving-boundaries-v1` | 1 | rect/disk lights, HDR environment, Lambert/GGX boundary |
| `glossy-spheres` | `glossy-sphere-environment-static-v1`, `...-camera-disocclusion-v1` | 1 | rect/sphere lights, rough dielectric and metal, camera disocclusion |
| `interreflection` | `interreflection-room-static-v1`, `...-moving-block-v1` | 4 | closed room, indirect light about half of each pixel |

Common settings: 128x128, 256 analytic lights, 64 frames, four warmup
frames, four independent sequences per invocation, candidates 1 and 8,
power-alias proposal, history cap 32, spatial radius 1, reference seed
`16777216` with two 256-spp NEE/MIS reference halves, radiance reuse off.
The plan is `build/restir-reuse-cost-plan.json`; one invocation:

```sh
VK_LOADER_LAYERS_DISABLE=VK_LAYER_KHRONOS_validation build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark-restir build/restir-reuse-cost/mixed-wavefront-b.json --benchmark-label "8e179cd; Release; 2026-10-17; validation layers disabled; clocks locked 1500/8001 MHz" --benchmark-size 128 --benchmark-frames 64 --benchmark-warmup 4 --restir-sequences 4 --restir-lights 256 --restir-reference-samples 256 --restir-candidates 1,8 --restir-history-cap 32 --restir-radius 1 --restir-reference-seed 16777216 --benchmark-integrator wavefront --restir-seed 262165 --restir-workload mixed-boundaries
```

All 1,212 PFM pairs are byte identical between the integrators, so quality
figures apply to both and only timings differ. The
[summary JSON](2026-10-17-restir-reuse-cost.json) keeps every case's GPU
time (median of each run's 256 frames, with the range across runs), pass
times, wall time, memory, budgets, equal-time ratios and stability metrics;
`build/analyze-restir-reuse-cost.py` produced it from the raw reports under
`build/restir-reuse-cost/`.

Case labels below: I = initial-only, T = temporal-only, S = spatial-only,
C = combined (spatiotemporal), followed by the candidate count.

## GPU cost

Median scene GPU time per frame, as a ratio to NEE/MIS in the same
invocation (static scenes; moving scenes are within a few percent), as the
median of the three invocations. Their spread is at most 0.01 for 106 of the
144 static and moving ratios and at most 0.04 for all.

| Workload / integrator | NEE/MIS ms | I1 | I8 | T1 | T8 | S1 | S8 | C1 | C8 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Distant / ref | 0.121 | 1.23 | 1.39 | 1.96 | 2.08 | 2.48 | 2.60 | 2.51 | 2.66 |
| Distant / wf | 0.137 | 1.19 | 1.32 | 1.81 | 1.93 | 2.26 | 2.37 | 2.28 | 2.42 |
| Mixed / ref | 0.220 | 1.12 | 1.18 | 1.51 | 1.58 | 1.84 | 1.91 | 1.87 | 1.94 |
| Mixed / wf | 0.324 | 1.08 | 1.14 | 1.35 | 1.40 | 1.55 | 1.61 | 1.57 | 1.63 |
| Spheres / ref | 0.174 | 1.13 | 1.23 | 1.62 | 1.71 | 1.99 | 2.08 | 2.04 | 2.16 |
| Spheres / wf | 0.257 | 1.09 | 1.17 | 1.45 | 1.51 | 1.67 | 1.74 | 1.71 | 1.78 |
| Room / ref | 0.428 | 1.06 | 1.11 | 1.27 | 1.31 | 1.40 | 1.45 | 1.42 | 1.46 |
| Room / wf | 0.678 | 1.04 | 1.07 | 1.16 | 1.19 | 1.25 | 1.27 | 1.26 | 1.28 |

Reuse adds a roughly fixed cost over NEE/MIS at this resolution: 0.02–0.05 ms
for initial candidates, 0.11–0.13 ms for temporal reuse (history
reprojection, primary surface and temporal pass) and 0.17–0.21 ms for
spatial or combined reuse.
Its ratio is therefore largest where a NEE/MIS frame is cheapest and falls
with transport depth: 1.07–1.28 for every mode in the four-bounce room under
the wavefront integrator. [256x256](#resolution-check) checks this at four
times the pixel count.

## Time to equal quality (static scenes)

A static scene accumulates, so the question is how much GPU time each case
needs to reach a given error. Two measures answer it.

**Horizon efficiency** compares 64-frame prefix MSE times cumulative GPU
time, `(MSE_mis x time_mis) / (MSE_case x time_case)`. Where MSE falls as
`1/N`, this is the ratio of times to equal quality; above one the case is
faster. Initial-only and spatial-only frames are uncorrelated (variance
inflation 1.0 in every scene), and their horizon efficiencies agree within
0.03 with the same ratio formed from within-sequence frame variance, which
excludes the finite reference. Temporal and combined frames are correlated
(inflation 1.1–56), so their prefix MSE falls more slowly at first and the
measure describes the 64-frame horizon. The table gives the range across the
three runs:

| Workload / integrator | NEE/MIS RMS64 | I1 | I8 | T1 | T8 | S1 | S8 | C1 | C8 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Distant / ref | 0.2465 | 0.81–0.82 | **1.35–1.37** | 0.03 | 0.12 | 0.70–0.71 | 0.81 | 0.12 | 0.31 |
| Distant / wf | 0.2465 | 0.84 | **1.41–1.42** | 0.04 | 0.13 | 0.77–0.78 | 0.89 | 0.13–0.14 | 0.34 |
| Mixed / ref | 0.0303 | 0.89 | 0.93–0.94 | 0.10 | 0.27–0.28 | 0.60 | 0.58–0.59 | 0.25 | 0.42 |
| Mixed / wf | 0.0303 | 0.92–0.93 | 0.96–0.97 | 0.11 | 0.31 | 0.71 | 0.69–0.70 | 0.30 | 0.51 |
| Spheres / ref | 0.0359 | 0.88–0.89 | 0.82–0.84 | 0.09–0.10 | 0.25 | 0.51–0.52 | 0.49–0.50 | 0.22–0.23 | 0.36 |
| Spheres / wf | 0.0359 | 0.91–0.92 | 0.84–0.87 | 0.10–0.11 | 0.28–0.29 | 0.61–0.62 | 0.59–0.60 | 0.27–0.28 | 0.44 |
| Room / ref | 0.0679 | 0.81–0.85 | 0.89–0.91 | 0.36–0.37 | 0.62–0.63 | 0.70–0.71 | 0.69–0.70 | 0.55–0.56 | 0.63–0.65 |
| Room / wf | 0.0679 | 0.86–0.87 | 0.92–0.93 | 0.39–0.41 | 0.68–0.70 | 0.78–0.80 | 0.79 | 0.62–0.63 | 0.71–0.74 |

**Observed budgets** use `scripts/restir_budget.py` without any scaling
assumption. Targets come from the NEE/MIS control alone: the worst of its
three runs' RMS at 16, 32 and 64 frames, fixed before any reuse case was
read. A budget is the first power-of-two checkpoint that stays below the
target, and the speedup divides NEE/MIS cumulative GPU time by the case's.
At the 64-frame target (a dash: no run reaches it; parentheses: the speedup
at the 16-frame target instead):

| Workload / integrator | Target RMS | I1 | I8 | T1 | T8 | S1 | S8 | C1 | C8 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Distant / ref | 0.2470 | 0.81–0.82 | 0.72 | — | — (0.12) | 0.40 | 0.77 | — (0.10) | — (0.19) |
| Distant / wf | 0.2470 | 0.84 | 0.75 | — | — (0.13) | 0.44–0.45 | 0.85 | — (0.11) | — (0.21) |
| Mixed / ref | 0.0304 | — (0.89) | 0.85 | — | — (0.16) | 0.54 | 0.51–0.52 | — (0.26) | — (0.25–0.26) |
| Mixed / wf | 0.0304 | — (0.93) | 0.87 | — | — (0.18) | 0.64–0.65 | 0.62 | — (0.31–0.32) | — (0.30–0.31) |
| Spheres / ref | 0.0359 | 0.88 | 0.81 | — | — (0.15) | 0.50 | 0.48 | — (0.24) | — (0.23) |
| Spheres / wf | 0.0359 | 0.91 | 0.84 | — | — (0.17) | 0.60 | 0.57 | — (0.29) | — (0.28) |
| Room / ref | 0.0681 | — (0.45–0.47) | 0.88–0.90 | — (0.39) | — (0.38) | — (0.36–0.71) | 0.68–0.69 | — (0.35) | — (0.34) |
| Room / wf | 0.0681 | — (0.47–0.48) | 0.92–0.93 | — (0.43) | — (0.42) | — (0.40–0.80) | 0.78–0.79 | — (0.39–0.40) | — (0.38–0.39) |

The checkpoints quantize attainment by factors of two, which matters exactly
once: initial-only with eight candidates in the distant scene has RMS
0.2538–0.2543 at 32 frames against the 0.2470 target, so its budget is
recorded at 64 frames (0.72–0.75) although it reaches the target only a few
frames after 32. Its horizon efficiency, 1.35–1.42, is the better estimate,
and the [256x256 check](#resolution-check) repeats it. Every other case is
below one on both measures, so the decision does not depend on which is used.

**Initial-only reservoirs with eight candidates reach equal quality about
1.4 times faster than NEE/MIS for many distant lights**, under both
integrators. No other case does in any static scene. Initial-only with eight
candidates comes close with area lights and an environment (0.93–0.97) and
in the room (0.89–0.93), and loses with glossy spheres (0.82–0.87). Spatial
reuse lowers the error per frame but costs too much (0.49–0.89). Temporal and
combined reuse are 0.03–0.74: correlated frames accumulate far less than
independent ones, as the
[sequence-ensemble decomposition](2026-10-16-restir-sequence-ensemble.md)
found, and most never reach the 64-frame target. With one candidate in the
room, reservoir selection without BSDF sampling has higher error than MIS
(fresh RMS 0.543 against 0.512).

## Equal GPU time in motion (moving scenes)

A moving scene cannot accumulate, so each frame is judged alone. NEE/MIS
frames are independent, so spending a case's GPU time on NEE/MIS samples
would divide its MSE by the time ratio. The table gives
`(MSE_mis x time_mis) / (MSE_case x time_case)`: above one, the case gives a
lower per-frame error than NEE/MIS for the same GPU time. MSE is the mean
fresh-frame squared error against the finite reference, whose noise slightly
pulls every ratio towards one.

| Workload / integrator | I1 | I8 | T1 | T8 | S1 | S8 | C1 | C8 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Distant / ref | 0.84–0.85 | **1.37–1.38** | 1.05–1.06 | 1.05–1.07 | 0.76–0.77 | 0.86–0.87 | 0.91–0.92 | 0.86–0.87 |
| Distant / wf | 0.88–0.89 | **1.48** | 1.20 | 1.22 | 0.90–0.91 | 1.02 | 1.07–1.08 | 1.03 |
| Mixed / ref | 0.89 | 0.90–0.92 | 0.71 | 0.67–0.68 | 0.57 | 0.56 | 0.57 | 0.55 |
| Mixed / wf | 0.92 | 0.95–0.96 | 0.79–0.80 | 0.76–0.77 | 0.67 | 0.66 | 0.67–0.68 | 0.65 |
| Spheres / ref | 0.87–0.88 | 0.84–0.85 | 0.64–0.65 | 0.61 | 0.51–0.52 | 0.50 | 0.51–0.52 | 0.48–0.49 |
| Spheres / wf | 0.92 | 0.89–0.90 | 0.73–0.74 | 0.69–0.70 | 0.61–0.62 | 0.60–0.61 | 0.61–0.62 | 0.59 |
| Room / ref | 0.83 | 0.90 | 0.78 | 0.77–0.78 | 0.69–0.70 | 0.69–0.70 | 0.67–0.72 | 0.68–0.69 |
| Room / wf | 0.86 | 0.94 | 0.85–0.86 | 0.85–0.86 | 0.78–0.79 | 0.80 | 0.76–0.81 | 0.79 |

Only the many-distant-light scene favours reservoirs: initial-only with
eight candidates by 1.37–1.48, temporal reuse by 1.05–1.22, spatial and
combined reuse by at most 8% and only under the wavefront
integrator. Where area lights, an environment, glossy materials or
interreflection appear, NEE/MIS with BSDF sampling already samples well and
every reuse mode loses (0.48–0.96).

## Temporal stability

Frame-to-frame residual difference (fresh frame minus its reference,
differenced between consecutive frames) as a ratio to NEE/MIS, and the lag-1
residual autocorrelation, in the moving scenes (reference integrator; the
images are identical under wavefront):

| Workload | I8 | T1 | T8 | S8 | C1 | C8 |
| --- | --- | --- | --- | --- | --- | --- |
| Distant | 0.75 / −0.02 | 0.30 / 0.69 | 0.38 / 0.65 | 0.71 / −0.02 | 0.56 / 0.30 | 0.60 / 0.23 |
| Mixed | 0.95 / −0.02 | 0.85 / 0.11 | 0.87 / 0.12 | 0.95 / −0.02 | 0.90 / 0.06 | 0.92 / 0.05 |
| Spheres | 0.98 / −0.02 | 0.93 / 0.05 | 0.94 / 0.05 | 0.97 / −0.02 | 0.93 / 0.05 | 0.95 / 0.03 |
| Room | 1.00 / −0.02 | 0.98 / 0.01 | 0.98 / 0.00 | 0.99 / −0.02 | 0.98 / 0.01 | 0.99 / −0.00 |

Temporal reuse reduces raw flicker strongly only in the distant scene, at
the price of the correlation that slows accumulation (lag-1 0.65–0.69).
Elsewhere it changes frame differences by 2–15%. Whether that correlation
helps or hurts a temporal denoiser is a Renderer Phase 6 question, decided
with [DES-Q7](../design/DESIGN_POLICY.md#53-open-questions). Every case's
replication noise-normalized MSE lies between 0.95 and 1.07, consistent with
the earlier finding of no detectable reuse bias; this campaign's four
sequences per run are not a bias test.

## Wall time and memory

| Mode (eight candidates) | Reservoir + metadata memory at 128x128 | Per pixel | Render/update wall time |
| --- | --- | --- | --- |
| NEE/MIS | 0 | 0 | 1.3–2.0 ms |
| Initial-only | 1.5 MiB | 96 B | 4.2–4.9 ms |
| Temporal-only | 18.0 MiB | 1,152 B | 27.7–28.7 ms |
| Spatial-only, combined | 24.5 MiB | 1,568 B | 28.6–36.4 ms |

Memory counts device buffers and their host readback mirrors, as the
benchmark reports it; it is independent of the candidate count and the
workload. Wall time covers the blocking update and render call. Reuse frames
read back the reservoir records (current, previous and spatial inputs), the
temporal surfaces and validity, and check them on the CPU, through a second
submission: an evidence path, 28–36 ms per frame at only 16,384 pixels,
which rules out interactive use of reuse through
`RenderTemporalScene` as it stands. The GPU times above exclude it. The
viewport ([LOTUS-VIEWPORT-03](../archive/pre-v070-execution-plan.md#step-2--viewport-foundation))
needs a reuse path that skips these products.

## Resolution check

At 128x128 a frame is a few hundred microseconds, so fixed dispatch costs
could distort the ratios. The `distant` and `interreflection` suites were
rerun at 256x256 (four times the pixels), seed `17` only, under both
integrators: four invocations in 34 minutes, all 2,003 clock samples at
P0 1500/8001 MHz (56–65 °C). Plan `build/restir-reuse-cost-256-plan.json`,
comparison `build/compare-restir-reuse-cost-256.py`, output
`build/restir-reuse-cost-256-summary.json`.

| Case | GPU ratio, distant | GPU ratio, room | Equal-time, distant moving | Equal-time, room moving |
| --- | --- | --- | --- | --- |
| I8 | 1.33 | 1.09 | 1.41–1.42 | 0.92 |
| T8 | 2.11–2.14 | 1.30–1.31 | 1.02–1.03 | 0.76 |
| S8 | 2.89–2.91 | 1.50 | 0.76 | 0.68 |
| C8 | 2.99–3.03 | 1.52–1.53 | 0.75–0.76 | 0.66–0.67 |

Each cell spans both integrators. NEE/MIS frames take 0.31 ms (distant) and
1.18 ms (room) under both, so at this size the wavefront integrator no longer
pays the per-frame overhead it shows at 128x128. Reuse does not become
relatively cheaper: initial-only keeps its ratio, temporal reuse keeps or
raises it, and spatial and combined reuse rise from 2.3–2.7 to 2.9–3.0 in the
distant scene. Initial-only with eight candidates keeps its gain for many
distant lights in motion (1.41–1.42); temporal reuse falls to break-even
there (1.02–1.04 with one or eight candidates). Static scenes repeat the
128x128 pattern. Initial-only with eight candidates has horizon efficiency
1.27 (reference) and 1.41 (wavefront) in the distant scene and 0.91–0.92 in
the room; every other case stays below one (spatial 0.63–0.73, temporal and
combined 0.03–0.63). Memory per pixel is
unchanged, so reservoirs take 6.0, 72.0 and 98.0 MiB for initial,
temporal and spatial/combined reuse. Reuse wall time scales with the pixels:
120–154 ms, against 5.3–6.4 ms for NEE/MIS.

## Decision

- **Default path: NEE/MIS**, under both integrators. With area lights, an
  environment, glossy materials or deep interreflection, NEE/MIS with BSDF
  sampling already samples well, and no reuse mode reaches equal quality
  faster, accumulating or in motion.
- **Where ReSTIR is worth using: initial-only reservoirs with eight
  candidates, for many lights of similar importance that BSDF sampling
  cannot find** — here many distant lights on a diffuse surface. They reach
  equal static quality 1.35–1.42 times faster (1.27–1.41 at 256x256) and give
  1.37–1.48 per-frame efficiency in motion, with 96 bytes per pixel, no
  history and no frame correlation.
- **Temporal reuse** gains only per frame in that same distant scene
  (1.05–1.22 at 128x128, break-even at 256x256) and cuts raw flicker to
  0.30–0.38 of NEE/MIS, but its correlation makes accumulation 8–30 times
  slower there. Its value as denoiser input is a Renderer Phase 6 question.
- **Not worth using:** temporal or combined reuse with accumulation, in every
  workload; spatial reuse at radius 1 anywhere (0.49–0.89 static, at most
  1.02 in motion); one-candidate initial-only reservoirs, which never beat
  NEE/MIS.

This is the reuse-cost decision. The Renderer Phase 5 exit review
(LOTUS-RESTIR-06) records it with the bad cases, correlation cost and
unsupported conditions, and updates the reservoir reference.

## Limits

One GPU at one locked clock; 128x128 (and the 256x256 check); 256 lights;
the power-alias proposal; history cap 32 and spatial radius 1; one to four
bounces; synthetic fixtures without emissive meshes or production scenes.
Light count was not swept, and every ray still tests every analytic light
([LOTUS-LIGHT-05](../archive/pre-v070-execution-plan.md#deferred-adapter-work)), so the
NEE/MIS baseline itself grows with the light count. Budgets use power-of-two
checkpoints without interpolation; horizon efficiencies assume `1/N`
convergence, which holds for the uncorrelated modes that carry the decision.
Equal-time and horizon ratios include finite-reference noise, which pulls
them slightly towards one. Locked clocks below the boost clock and disabled
validation layers mean the absolute times are not production frame times.

## Validation

No source changed since `8e179cd`, whose core (48/48), Hydra (75/75) and
runtime-free suites passed for the
[reset contract](2026-10-17-restir-reset-contract.md). Every invocation
here exited 0 with status `pass`, which requires one fresh sample per frame
and the expected history availability and resets. Documentation validation
passes (`python scripts/check_docs.py`).
