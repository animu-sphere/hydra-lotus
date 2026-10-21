# GPU initial direct-light reservoirs

Measured on 2026-10-13 against `ae0f473` plus LOTUS-RESTIR-02, MSVC Release,
Windows, NVIDIA RTX A5000 and the existing OpenStrata core / OpenUSD 26.08
lookdev configurations. The [reservoir reference](../reference/RESTIR.md)
owns the estimator, sample measures, products and limits. This is initial-only
correctness evidence, not a many-light performance or reuse result.

## CPU/GPU arithmetic and payloads

CTest `lotus-renderer-restir-gpu` renders a 64×64 diffuse plane with index
matched Lambert material (IOR 1, base RGB 0.5/0.75/1), three equal-power parallel
lights and eight candidates per surface. Lights carry RGB (2,1,0.5), (1,2,0.5)
and (0.5,1,2); the third points below the receiver and has zero target.
The base sample index is 17.

The test independently replays the candidate PCG stream and uniform alias
selection, evaluates known Lambert targets, and streams them through the
double-intermediate CPU oracle. All 4,096 records under each integrator match
selected identity, payload variates and represented count exactly, weight sum
within 2e-6 and target within 2e-7. Selected, normalized RGB matches the
independent irradiance within 2e-6. One stream is all-zero and retains all eight
draws. Changing the material to black checks all-zero targets across every
pixel. The same sample produces identical reservoirs and colour under the
two integrators.

The pass reports positive separate GPU timestamps on this device. Fresh
64×64 device storage plus readback is 393,216 bytes (96 bytes/pixel), checked
directly against the product's memory accounting. Timestamps are diagnostic
durations of the last submitted sample; this run does not benchmark speedup.

## Independent lighting integrals and NEE/MIS images

Each fixture uses an 8×8 image, sample index 17, 1,024 spp, one scattering
event and eight candidates. The perspective projection has X/Y scale 100,
so receivers span approximately ±0.04 around (0,0,-4). Material is the same
index-matched Lambert. RGB source radiance is (4,2,1).

Rect, disk, sphere and emissive fixtures are centred at (1,0,-2). Rect and
emissive panel are 1×1; disk and sphere have radius 0.5. The distant light is
centred along +Z with angular diameter 60°. The environment image is a uniform
4×2 float texture. The constant environment uses the same RGB.

Rect/disk/emissive expectations use independent double 512×512 midpoint
area quadrature with both receiver/emitter cosines and distance squared.
Sphere expectation uses projected solid angle,
`albedo * radiance * r²/d² * cos(theta_centre)`; distant expectation uses
`albedo * radiance * sin²(30°)`. Environment expectation is albedo times
radiance. The small receiving footprint accounts for less than the accepted
tolerance of 2% of expected radiance plus 0.0001 per channel.

Both integrators produced the same mean values shown here:

| Fixture | Expected red | Initial reservoir red | NEE/MIS red |
| --- | --- | --- | --- |
| Rect | 0.0990437 | 0.0990397 | 0.0990334 |
| Disk | 0.0783646 | 0.0783600 | 0.0783458 |
| Sphere | 0.0894427 | 0.0893541 | 0.0894122 |
| Distant | 0.5000000 | 0.5000090 | 0.5002090 |
| Emissive | 0.0990437 | 0.0990373 | 0.0991168 |
| Environment image | 2.0000000 | 2.0059000 | 1.9975000 |
| Constant environment | 2.0000000 | 2.0000000 | 2.0000000 |

Every RGB channel passes both independent-oracle and NEE/MIS comparisons.
These cover the sphere area's back-facing zero targets and primary BSDF
emission partition for emissive, analytic and environment sources. They are
finite-sample checks, not an exhaustive proof for arbitrary materials/scenes.

The Cornell box is 32×32, sample index 17, 1,024 spp, maximum 16 scattering
events and eight initial candidates. Its mean RGB is
(0.686254, 0.560244, 0.391769), compared with NEE/MIS
(0.685823, 0.560187, 0.391589), identical under both integrators.
Whole-image tolerance is 2% of baseline plus 0.001; all 8×8 tiles pass
5% plus 0.005. Later-bounce NEE/MIS, diffuse interreflection and GGX remain
active. Final PFM comparisons are retained under each build's
`restir-initial-images/`.

## Visibility, lifecycle and failure

A 16×16 diffuse plane receives a parallel light along
(normalized 5,0,1). A black panel at Z=-2, X=[5,15], Y=[-6,6] is outside camera
rays but crosses all shadow rays. Opacities 0, 0.75 and 1 produce selected
transmittance 1, 0.25 and 0, with unchanged reservoir selection/counts and
independent RGB irradiance within 2e-6. Fully occluded selected samples retain
their normalization and resolve to zero.

Lifecycle checks cover invalid counts 0/65, endpoints 1/64, candidate-change
accumulation resets, convergence-limit calls with no stale product publication,
target resize and crops, pixels outside the data window, light edits and
domain generation, zero scattering events, camera misses, empty scenes and
temporal-radiance compatibility after candidate-count changes. Reservoir state
does not enter shared history metadata.

A fresh renderer with maximum-float parallel irradiance overflows the streamed
weight sum in a two-sample batch. Both integrators reject the call, publish no
successful reservoir product and remain failed. Numerical errors are latched
across a batch rather than overwritten by a later sample.

## Regression commands

Commands run from the repository root:

```sh
ost build --jobs auto
ost test --filter restir-gpu
ost test
ost validate --strict-renderer-evidence
ost build --without-runtime --intent ci-core --jobs auto
ost test --without-runtime --intent ci-core
ost build --profile lookdev --intent hydra --jobs auto
ost test --profile lookdev --intent hydra
ost validate --profile lookdev --intent hydra --strict-renderer-evidence
python scripts/check_docs.py
python -m unittest discover -s scripts -p 'test_*.py'
```

Core and Hydra objects were rebuilt to avoid the documented Windows incremental
header dependency limitation. Runtime-free checks passed 14 tests, with temporal
and reservoir GPU tests explicitly skipped (16 registered). Physical-GPU core
passed 26/26 and Hydra passed 51/51, including installed shader rendering,
committed reference images, temporal transport, installed usdview and
synchronization validation. Both strict renderer-evidence validations passed;
core-only Hydra assertions retained their explained SKIPs. Documentation
checks and all 27 Python documentation/release tests passed. No Hydra reservoir
control or reservoir-reuse capability is claimed by the Hydra regression.

Build/test/validation logs use `build/restir-initial-*.log`. The GPU executable
prints arithmetic counts, lighting values, timestamp observations and explicit
PASS/FAIL results; CTest retains them in `Testing/Temporary/LastTest.log`.

## Scope disposition

LOTUS-RESTIR-02 supplies reconstructible initial candidates and direct-light
resolve for emissive, analytic and environment sources, selected visibility,
primary BSDF-emission accounting and opt-in integration under both integrators.
It carries debug products, separate timing and retained memory totals while
preserving NEE/MIS.

Temporal/spatial reuse, support-aware differing-target normalization, persistent
light/sample identities, alternative candidate strategies, Hydra settings/AOVs
and many-light convergence/time/bias/temporal-stability exit evidence remain in
the [execution plan](../archive/pre-v070-execution-plan.md#execution-order). GPU subnormal and
extreme dynamic-range behavior is not established by the ordinary fixtures.
