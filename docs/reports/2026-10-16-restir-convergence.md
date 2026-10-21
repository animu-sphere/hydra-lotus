# ReSTIR transport depth and finite-reference sensitivity

Measured 2026-10-16 on Windows, MSVC 19.51.36256 Release, Vulkan/Slang,
NVIDIA RTX A5000. Source base: `19d6e42`, plus headless convergence controls
and their validation. The [reservoir reference](../reference/RESTIR.md#headless-reservoir-comparison)
owns option ranges, workload definitions and metric semantics. Transport shaders,
reservoir arithmetic and public renderer settings are unchanged.

## Change and procedure

The comparison previously fixed one scattering bounce and one finite-reference
seed. `--restir-bounces` now selects the same scattering depth for measured
and reference transport; `--restir-reference-seed` selects only reference draws.
Existing procedure fields retain these identities. Defaults are unchanged.
The repeated-budget reducer already requires both fields to match, and new
regressions prevent comparisons from mixing depths or reference seeds.

Eight invocations run serially, after physical-GPU tests finish. Each integrator
uses reference seeds `16777216` and `18874368`, with measured seeds `17` and
`262165` within each reference group. Settings are 32x32, 64 measured frames,
four independent sequences, four warmup frames, 64 analytic lights, eight initial
candidates, history cap 32, radius 1, power proposals and **four scattering
bounces**. Radiance reuse is disabled. The unchanged `glossy-spheres` pair
contains rough dielectric/metal receivers, rect/sphere lights, a foreground panel,
an HDR environment and static/camera-only moving sequences.

Each pose has two disjoint 1024-spp NEE/MIS reference halves, always rendered
by the reference integrator. The two reference groups reserve disjoint ranges
of 2097152 indices. Each measured sequence starts with a cut and a disjoint
sample range. Measured draws are shared across reference groups and integrators:
these eight invocations are not eight independent measured replications.
References are also shared across the two measured repetitions in each group.
There are eight independent measured sequences per mode, reused for each
reference group and integrator. This broadens the earlier
[one-bounce measurement](2026-10-16-restir-glossy-spheres.md), without being a
matched causal comparison of bounce count alone.

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark-restir build/restir-convergence-reference-16777216-0.json --benchmark-label "19d6e42 plus ReSTIR convergence controls; Release; 2026-10-16" --benchmark-integrator reference --restir-workload glossy-spheres --benchmark-size 32 --benchmark-frames 64 --benchmark-warmup 4 --restir-sequences 4 --restir-lights 64 --restir-reference-samples 1024 --restir-candidates 8 --restir-history-cap 32 --restir-seed 17 --restir-bounces 4 --restir-reference-seed 16777216
```

Repeat with measured seed `262165` and suffix `-1.json`, then reference seed
`18874368` and corresponding paths. Repeat both groups with `wavefront` and
distinct paths. Reduce each matched pair separately with `scripts/restir_budget.py`.
The [summary JSON](2026-10-16-restir-convergence.json) retains build/device,
procedure, per-group budgets, temporal/regional populations, GPU timing medians
and independent verification counts. Raw JSON, PFMs, logs and the serial runner
`build/run-restir-convergence.py` remain under `build/`.

## Static convergence

Values are arithmetic means of the two invocations' prefix RMS errors. Each
invocation's RMS covers four independent sequence means; it is not the error of
a pooled image. Both integrators give exactly equal quality populations. Lag-one
is averaged over eight sequence residual correlations. Reference group A uses
seed `16777216`; B uses `18874368`.

| Mode | 32-sample RMS A | 64-sample RMS A | 64-sample RMS B | Lag-one A |
| --- | --- | --- | --- | --- |
| NEE/MIS | 0.051549 | 0.036688 | 0.036855 | -0.015997 |
| Initial | 0.050247 | 0.036037 | 0.036291 | -0.017157 |
| Temporal | 0.073956 | 0.054831 | 0.055100 | 0.110411 |
| Spatial | 0.050077 | 0.035987 | 0.036176 | -0.016194 |
| Spatiotemporal | 0.058904 | 0.042021 | 0.042214 | 0.034814 |

Temporal-only error remains about 1.52 times initial-only error at 64 samples
under either reference group. Static reference-half split RMS is 0.012406/0.012500;
the two independently seeded reference means differ by RMS 0.008875. Changing
the reference therefore does not remove the observed reuse penalty in this
fixture. Positive residual correlation accompanies it. Errors still fall between
32 and 64 samples: this horizon does not demonstrate a fixed error floor, rule
out bias or rare-event variance, or establish an effective independent sample
count. Static centered correlations are unchanged by subtracting another constant
reference image and are not additional independent evidence across groups.

Moving fresh-frame RMS in group A averages 0.285982 for NEE/MIS, 0.279313 for
initial, 0.277034 for temporal, 0.277470 for spatial and 0.276506 for combined
reuse. These are pose-matched fresh-frame errors, not convergence of a static
solution. They do not establish absence of ghosting.

## Cost and quality budgets

At static RMS 0.05, both groups and integrators agree: NEE/MIS needs 64 samples
in both repetitions, initial and spatial need 32/64, combined needs 64/64, and
temporal-only never attains the budget within 64 frames.

At RMS 0.1, combined reuse attains the budget at 16 samples in every invocation.
Its paired NEE/MIS speedup spans 0.0512–0.1545 under reference and 0.0950–0.1134
under wavefront; values below one mean longer cumulative GPU time than NEE/MIS.
All reservoir modes are slower at that budget in all measured invocations.
These runs establish no equal-quality speedup. Scene time includes metadata
and reservoir work, excluding upload/readback. Validation and dynamic clocks
remain enabled; serial, non-interleaved timings do not isolate operations' costs.

## Independent checks and validation

All eight invocations pass, with Vulkan and synchronization validation available
and zero captured messages. Checks retained in the summary confirm:

- All 40 matched integrator case pairs have equal quality, temporal, regional,
  memory and history populations; all 340 matched PFM pairs are byte identical.
- Changing only the reference seed changes all 260 compared reference images
  while preserving all 80 compared measured images byte for byte.
- Independently reading the saved PFMs reproduces every ensemble RMS and all
  120 regional RMS/signed-RGB measurements within 2e-6 absolute/relative tolerance.

The core suite passes all 42 tests in 728.10 seconds. After adding the two
control-isolation tests, both pass in 119.76 seconds, giving coverage of all
44 tests in the final configuration. They check omitted-option defaults,
reference-seed isolation and a visible scattering-depth effect under both
integrators. All 41 saved default sphere-fixture PFMs are byte identical to
their pre-change counterparts. Strict renderer-evidence validation passes.
The final runtime-free build/test records 31 passes and two explained GPU SKIPs.
All 46 Python tests and documentation validation pass. The upper accepted
seed/depth settings also pass procedure/CLI checks with an explicitly missing
driver; this is not physical-GPU coverage at the upper bounds.

```sh
ost build --jobs auto
ost test
ctest --test-dir build/cy2026-windows-x86_64-py313-core -C Release -R 'lotus-renderer-restir-controls-' --output-on-failure
ost validate --strict-renderer-evidence
ost build --without-runtime --intent ci-core --jobs auto
ost test --without-runtime --intent ci-core
python -m unittest discover -s scripts -p 'test_*.py'
python scripts/check_docs.py
```

Hydra and the release gate are not rerun for this headless-only change.
This is bounded multibounce/reference-sensitivity evidence for LOTUS-RESTIR-05.
Broader scene/artifact coverage, longer-horizon bias/variance discrimination,
correlation and reuse-cost investigation remain in the
[execution plan](../archive/pre-v070-execution-plan.md#execution-order). No phase exit is claimed.
