# ReSTIR under light and material animation: resets are exact, random numbers replay

Measured 2026-10-17 on Windows, MSVC 19.51.36256 Release, Vulkan/Slang,
NVIDIA RTX A5000. Source base: `e63b743` (main `7777640` plus the `animation`
workload and step-lag correlation). The [reservoir reference](../reference/RESTIR.md#headless-reservoir-comparison)
owns the workload and statistic definitions. Transport shaders, reservoir
arithmetic and public renderer settings are unchanged.

## Question and change

Earlier moving suites ([replication bias](2026-10-17-restir-moving-bias.md),
[closed room](2026-10-17-restir-interreflection.md)) move only the camera and
nonemissive geometry, which retain reservoir history. Analytic light and
material edits instead [reset history](../reference/RESTIR.md#compatibility-and-publication).
Under animation, that reset must happen at every edit and nowhere else, stale
history must not leave artifacts after it, and reuse must rebuild between edits.

`--restir-workload animation` uses the mixed-boundaries geometry (red Lambert
and blue GGX metal panels, a fixed foreground panel, rect/disk lights and a
procedural environment) with a fixed camera. Every `max(2, frames / 8)` frames,
eight for 64 frames, it rotates the light disk, reweights each light's power
by up to ±75% (changing the power proposal's table), and changes the metal
roughness (0.15–0.55) and the Lambert green. Pose mean reference radiance
ranges from 0.167 to 0.200. Frames between edits are unchanged, so each edit is
followed by seven frames of rebuilding history. The benchmark fails if an edit
retains reservoir or shared temporal history, or if an unedited frame loses it.

After a reset the fresh-batch RNG restarts at the base sample index, as the
backend does for every reset. Reuse modes (fixed base index) therefore replay
the random numbers of the previous epoch's frame at the same age; NEE/MIS and
initial-only frames (explicit index per frame) do not. A new
`animation.step_lag_correlation` measures the effect: residuals one edit step
apart in a sequence, centered across sequences so that persistent error and
reference noise drop out.

## Procedure

Seven invocations at 32x32, **64 independent sequences** of 64 frames, four
warmup frames, 64 analytic lights, eight power-proposal candidates, history cap
32, radius 1, one bounce, reference seed `16777216` and two disjoint
**4096-spp** NEE/MIS reference halves per pose. Radiance reuse is disabled.

| Run | Change from the base |
| --- | --- |
| `reference-a` | measured seed `17`, reference integrator |
| `reference-b` | measured seed `4194385` |
| `reference-c` | measured seed `145` |
| `reference-d` | measured seed `4194513` |
| `reference-a-refb` | reference seed `18874368` (independent reference) |
| `wavefront-a` | wavefront integrator |
| `step2-a` | 16 frames: an edit every two frames |

The four measured seeds draw disjoint sample indices (`c` and `d` are offset by
128 within each 65537-index sequence block from `a` and `b`). Seeds `c` and `d`
were added after the first five runs, to test the age dependence below. The
runs used [`scripts/restir_campaign.py`](../reference/RESTIR.md#campaigns) with
`--jobs 5`, so every label ends in `; jobs=5` and no timing from them is
evidence. The first five ran concurrently in about 541 seconds, the last two in
307 seconds. The base command:

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark-restir build/restir-animation/reference-a.json --benchmark-label "e63b743 plus ReSTIR animation workload; Release; 2026-10-17; jobs=5" --benchmark-integrator reference --restir-workload animation --benchmark-size 32 --benchmark-frames 64 --benchmark-warmup 4 --restir-sequences 64 --restir-lights 64 --restir-reference-samples 4096 --restir-candidates 8 --restir-history-cap 32 --restir-seed 17 --restir-bounces 1 --restir-reference-seed 16777216
```

The [summary JSON](2026-10-17-restir-animation.json) retains build/device,
procedures, every replication test, per-pose ratios, the frames-since-reset
analysis, step-lag correlations and verification counts. Raw reports, PFMs,
logs, the plan `build/restir-animation-plan.json` and the summarizer
`build/summarize-restir-animation.py` remain under `build/`.

## Resets and history

Every reuse case completed with history reset exactly at each of the seven
in-sequence edits and retained elsewhere. Temporal history reached 87% of moving
pixel-frames (one frame in eight is a reset) and 98% of static ones; with an
edit every two frames, 50% and 93%.

## Replayed random numbers

| Mode | step-lag correlation, step 8 (seeds a–d) | step 2 |
| --- | --- | --- |
| NEE/MIS | −0.001 to +0.001 | −0.001 |
| initial | −0.001 to +0.001 | 0.000 |
| temporal | 0.584–0.586 | 0.589 |
| spatial | 0.583–0.585 | 0.581 |
| spatiotemporal | 0.569–0.571 | 0.576 |

Noise in reuse modes stays correlated at about 0.58 across an edit although no
history crosses it: the same jitter, candidate and BSDF variates recur with
different lights and materials. Non-reusing estimators are uncorrelated. A
scene edited on every frame would replay one set of random numbers on every
frame. With an edit every two frames, the period-two replay drives within-sequence
lag-one autocorrelation to −0.37 to −0.41 in reuse modes (−0.06 otherwise).
Consumers that average over time or denoise temporally see this correlation;
Hydra uses a fixed `lotus:sampleIndex`, so the same applies there.

## Moving results

Noise-normalized MSE pooled over all 64 poses, four independent measured seeds:

| Modes | Image | Center band | Metal band | Tiles beyond `|z| = 3` (of 3072) |
| --- | --- | --- | --- | --- |
| NEE/MIS, initial | 0.995–1.010 | 0.990–1.022 | 0.984–1.040 | 8–24 |
| temporal, spatial, spatiotemporal | 1.000–1.019 | 0.982–1.034 | 0.968–1.035 | 5–41 |

The largest tile `|z|` is 5.5 for reuse (spatial) against 4.7 otherwise; the
largest per-pose ratio is 1.20 against 1.15. Tile counts exceed the normal
expectation of about eight in every mode, the heavy tails seen in the earlier
suites; the 41 (temporal, seed b) is not repeated by seeds c and d (5, 11).

Reuse ratios average 1.009 against 1.001. Two observations argue against an
edit-related bias. Spatial-only reuse, which carries no history across frames,
has the same excess (1.002–1.013). And replay correlates the per-pose ratios of
poses at equal age within a seed by 0.31 (temporal), 0.23 (spatial) and 0.10
(spatiotemporal), against −0.02 to +0.01 for non-reusing modes, so a reuse
case's 64 poses are fewer independent tests and its pooled ratio spreads more.
The same geometry under camera and blocker motion, without resets, showed reuse
0.994–1.006 against 0.993–1.011. Treated as a uniform persistent error, the
0.008 excess would be an RMS error of 0.003, about 1% of the mean red radiance
0.346; this measurement cannot resolve an error of that size.

### Frames since reset

Ghosting would appear just after an edit (age 0), and a history-length effect
late in an epoch. Per-pose ratio excess over initial-only (paired by pose and
reference), averaged over each age's eight poses and the four seeds:

| Mode | age 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| NEE/MIS | −0.005 | +0.003 | +0.001 | +0.019 | −0.008 | −0.020 | +0.005 | +0.017 |
| temporal | +0.007 | −0.022 | −0.004 | −0.004 | +0.008 | +0.016 | +0.021 | +0.041 |
| spatial | +0.038 | −0.010 | −0.003 | +0.014 | −0.004 | +0.017 | −0.004 | +0.012 |
| spatiotemporal | +0.038 | −0.014 | +0.017 | +0.007 | +0.009 | +0.027 | −0.012 | +0.024 |

Seed a alone gave +0.070 and +0.103 for temporal at ages 6 and 7, which led to
seeds c and d. Seeds b, c and d give +0.024, +0.032 and +0.007 at age 7. Spatial
reuse has no temporal history, so its age means (−0.010 to +0.038) show the
spread expected without any age effect; every temporal mean lies within it.
No reset-frame excess or growth with history length is resolved.

Per-frame replication noise relative to initial-only is 0.97–1.01 for every
reuse mode at every age (NEE/MIS 1.06–1.09): in this fixture reuse barely
reduces per-frame noise, before or after an edit.

## Static results

Static pooled ratios span 0.983–1.120 for reuse and 0.951–1.128 otherwise, with
at most one of 48 tiles beyond `|z| = 3`. Variance inflation is 2.48–2.55 for
temporal and 1.35–1.40 for spatiotemporal reuse (spatial and non-reusing modes
0.98–1.01), similar to the [open glossy fixture](2026-10-16-restir-sequence-ensemble.md).

## Independent checks and validation

All seven invocations pass with Vulkan and synchronization validation available
and zero captured messages. The reducer accepts the four measured seeds as a
repeated group. Rereading the saved ensemble and reference PFMs reproduces all
70 final-pose and static grand mean MSEs within 2e-5 relative tolerance.
Reference and wavefront runs have equal replication tests, sequence ensembles,
residuals, regions and animation statistics, and all 85 PFM pairs are byte
identical. Changing only the reference seed leaves all 20 measured images byte
identical. Every run's static reference equals its first moving pose.

`lotus-renderer-restir-statistics` compares the step-lag correlation with a
direct two-pass definition, including pooling, an anticorrelated pair and
undefined zero-variance cases. CTest `lotus-renderer-restir-benchmark-reference-animation`
and `-wavefront-animation` check both scene identities, both bands, the static
control, the edit step and the step-lag population.

The managed core build passes all 48 tests under `ost test --jobs 8` in 295
seconds, followed by strict renderer-evidence validation. The runtime-free build/test records 35
passes and two explained GPU SKIPs. All 58 Python tests and documentation
validation pass.

```sh
ost build --jobs auto
ost test --jobs 8
ost validate --strict-renderer-evidence
ost build --without-runtime --intent ci-core --jobs auto
ost test --without-runtime --intent ci-core --jobs 8
python -m unittest discover -s scripts -p 'test_*.py'
python scripts/check_docs.py
```

Hydra and the release gate are not rerun for this headless-only change.

## Disposition

Light and material edits reset reservoir and shared history exactly, and no reuse
mode shows reset-related ghosting, a frames-since-reset dependence or localized
error distinguishable from NEE/MIS or initial-only reservoirs. A small pooled
reuse excess, shared by spatial-only reuse, stays unresolved at about 1% of
the mean signal. Each reset restarts the fresh-batch RNG at the base index, so
reuse noise is correlated (about 0.58) across edits, and an edit on every frame
would replay one set of random numbers throughout. This does not cover
environment, texture or geometry animation, emissive-mesh scenes, production
scenes or larger images, and does not show a reuse speedup. Reuse cost remains
in the [execution plan](../roadmap/current.md#execution-order). No phase exit
is claimed.
