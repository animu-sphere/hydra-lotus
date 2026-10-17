# ReSTIR in a closed interreflecting room: no detectable bias at four bounces

Measured 2026-10-17 on Windows, MSVC 19.51.36256 Release, Vulkan/Slang,
NVIDIA RTX A5000. Source base: `d5dbb14`, plus the `interreflection` workload
and the concurrent campaign runner. The [reservoir reference](../reference/RESTIR.md#headless-reservoir-comparison)
owns the workload and test definitions. Transport shaders, reservoir arithmetic
and public renderer settings are unchanged.

## Question and change

The [sequence-ensemble](2026-10-16-restir-sequence-ensemble.md) and
[replication bias](2026-10-17-restir-moving-bias.md) measurements found no
detectable reuse bias, but their fixtures are open: most escaping paths leave
the scene, and one versus four bounces gave nearly the same images. They could
not show whether reuse stays unbiased when indirect light, sampled afresh by
NEE/MIS at later vertices, makes up much of each pixel.

`--restir-workload interreflection` adds a closed room: high-albedo white
floor, ceiling, back and front walls, red and green side walls, a white Lambert
block and a rough metal block, lit by 64 small rect, disk and sphere lights
under the ceiling. The moving scene translates the camera and the metal block;
nonemissive rigid motion retains reservoir history, so temporal reuse stays
active through the motion. Only primary-vertex direct light uses reservoirs.

## Procedure

Five invocations at 32x32, **64 independent sequences** of 64 frames, four
warmup frames, 64 analytic lights, eight power-proposal candidates, history cap
32, radius 1, **four bounces**, reference seed `16777216` and two disjoint
**4096-spp** NEE/MIS reference halves per pose. Radiance reuse is disabled.

| Run | Change from the base |
| --- | --- |
| `reference-a` | measured seed `17`, reference integrator |
| `reference-b` | measured seed `4194385` (disjoint measured range) |
| `reference-a-refb` | reference seed `18874368` (independent reference) |
| `wavefront-a` | wavefront integrator |
| `direct-a` | one bounce (direct-light control) |

The runs used [`scripts/restir_campaign.py`](../reference/RESTIR.md#campaigns)
with `--jobs 5`, so every label ends in `; jobs=5` and no timing from them is
evidence. The five runs completed in 737 seconds of wall time. The base
command, with the other runs changing one option as above:

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark-restir build/restir-interreflection/reference-a.json --benchmark-label "d5dbb14 plus ReSTIR interreflection workload; Release; 2026-10-17; jobs=5" --benchmark-integrator reference --restir-workload interreflection --benchmark-size 32 --benchmark-frames 64 --benchmark-warmup 4 --restir-sequences 64 --restir-lights 64 --restir-reference-samples 4096 --restir-candidates 8 --restir-history-cap 32 --restir-seed 17 --restir-bounces 4 --restir-reference-seed 16777216
```

The [summary JSON](2026-10-17-restir-interreflection.json) retains build/device,
procedures, campaign wall times, every replication test, per-pose ratios, the
motion-speed split, indirect shares and verification counts. Raw reports, PFMs,
logs, the plan `build/restir-interreflection-plan.json` and the summarizer
`build/summarize-restir-interreflection.py` remain under `build/`.

## Interreflection share

The one-bounce and four-bounce references share their sample seeds. Their
difference is the light contributed by the second to fourth scattering events:

| Scene | Image mean share | Pose-0 pixel share (p10 / median / p90) |
| --- | --- | --- |
| static | 0.479 | 0.42 / 0.56 / 0.78 |
| moving (64 poses) | 0.478–0.481 | same pose |

About half of the image, and most of the shadowed floor and walls, is indirect
light, compared with almost none in the earlier fixtures.

## Moving results

Noise-normalized MSE pooled over all 64 poses for seeds a/b and the
independent reference, with `|z| > 3` tile counts out of 3072:

| Mode | a | b | a, reference b |
| --- | --- | --- | --- |
| NEE/MIS | 1.010, 21 | 0.995, 11 | 1.011, 20 |
| initial | 1.013, 14 | 0.998, 20 | 1.010, 16 |
| temporal | 1.007, 14 | 1.005, 20 | 1.004, 16 |
| spatial | 1.007, 16 | 0.998, 20 | 1.004, 20 |
| spatiotemporal | 1.003, 16 | 1.005, 22 | 1.000, 19 |

Reuse modes span 0.998–1.007 against 0.995–1.013 for the non-reusing estimators.
Band ratios agree: center 0.986–1.014 for reuse against 0.986–1.020, red wall
1.006–1.014 against 0.991–1.014. Tile counts (14–22 for reuse, 11–21 otherwise)
again exceed the normal expectation of about eight in every mode, the
heavy-tailed noise the [replication bias report](2026-10-17-restir-moving-bias.md#moving-results)
describes. The largest tile `|z|` is 5.6 for reuse, 5.4 for initial-only and
3.9 for NEE/MIS; the largest per-pose ratio is 1.25 for reuse against 1.46 for
NEE/MIS. Temporal history reached 83% of moving and 92% of static
pixel-frames.

Per-pose excess over initial-only, averaged separately over the 34 fastest and
30 slowest poses, stays within ±0.020 for every reuse mode and run, against
±0.018 for NEE/MIS. No ghosting-like motion-speed dependence is visible.
The one-bounce control agrees: reuse 1.014–1.020 against 1.022–1.027.

## Static results and correlation

Static pooled ratios span 0.865–1.160 over the three four-bounce runs and all
modes, NEE/MIS 0.966–1.070, within the static spread of the
[earlier suites](2026-10-17-restir-moving-bias.md#static-results-and-resolution).
Every static case has 0 of 48 tiles beyond `|z| = 3`; the largest is 2.8.

Temporal correlation is weaker than in the open fixtures. Variance inflation at
cap 32 is 1.27/1.22 (seeds a/b) for temporal and 1.34/1.17 for spatiotemporal
reuse at four bounces, against 1.52 and 1.62 in the one-bounce control and
[2.34](2026-10-16-restir-sequence-ensemble.md) in the open glossy-sphere scene.
Indirect light is drawn afresh each frame, so it adds uncorrelated variance
that dilutes the correlated reservoir term. Initial-only and spatial
reuse stay near one.

## Resolution

Each moving pose's replication mean has a noise of 8.3e-3 per component, about
twice the one-bounce control's, because the later bounces add noise. Pooled over
64 poses, a uniform persistent error adding 3% of that noise would exceed the
non-reusing spread: an RMS bias of about 0.016, near 4% of the mean red radiance
0.40. That is coarser than in the open glossy fixture. The static test resolves
an RMS bias near 0.005 (about 1%) given its ±15% spread. Smaller, localized or
sign-changing errors can pass, and no confidence interval is asserted.

## Independent checks and validation

All five invocations pass with Vulkan and synchronization validation available
and zero captured messages. The reducer accepts the two measured seeds as a
repeated group. Rereading the saved ensemble and reference PFMs reproduces all
50 final-pose and static grand mean MSEs within 2e-5 relative tolerance.
Reference and wavefront runs have equal replication tests, sequence ensembles,
residuals and regions, and all 85 PFM pairs are byte identical at four bounces.
Changing only the reference seed leaves all 20 measured images byte identical.

CTest `lotus-renderer-restir-benchmark-reference-interreflection` and
`-wavefront-interreflection` check both scene identities, the red-wall band,
populations and the static control against the first moving pose at four
bounces.

The managed core build passes all 46 tests under `ost test --jobs 8` in 263
seconds, followed by strict renderer-evidence validation. The runtime-free
build/test records 33 passes and two explained GPU SKIPs. All 58 Python tests and documentation validation
pass.

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

In a closed room where indirect light is about half of every pixel at four
bounces, no reuse mode shows moving-scene bias, ghosting or localized error
distinguishable from NEE/MIS or initial-only reservoirs, over the image,
fixed bands, 8x8 tiles or motion speed, and static reuse remains consistent
with no detectable bias. Later-bounce noise both weakens temporal correlation
and coarsens the bias resolution. This does not establish unbiasedness, cover
light or material animation (which [resets history](../reference/RESTIR.md#compatibility-and-publication)),
production scenes or larger images, or show a reuse speedup. The remaining
LOTUS-RESTIR-05 items stay in the [execution plan](../roadmap/current.md#execution-order).
No phase exit is claimed.
