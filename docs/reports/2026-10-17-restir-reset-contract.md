# ReSTIR reset contract: emissive-state edits and Hydra

Measured 2026-10-17 on Windows, MSVC 19.51.36256 Release, Vulkan/Slang,
NVIDIA RTX A5000. Source base: main `357575a` plus the two tests below.
Renderer code, shaders and public settings are unchanged. The
[reservoir reference](../reference/RESTIR.md#compatibility-and-publication)
owns the reset policy.

## Question

The [Renderer Phase 5 exit criteria](../design/ROADMAP_POLICY.md#renderer-phase-5--restir-di)
require that light, material and emissive-state animation keeps the history
lifecycle and invalidation contract: no stale reservoir is reused, reset
classification is deterministic, the reference and wavefront integrators
agree, Hydra applies the same reset policy, and validation is clean. The
[light/material animation measurements](2026-10-17-restir-animation.md)
covered analytic light and material edits headless. This change checks the
remaining two parts, emissive-state edits and the Hydra path, as contract
tests rather than a convergence campaign.

The policy under test: camera motion and nonemissive rigid placement retain
history; light, material, visibility and geometry edits reset it as
`SceneChanged`; and while the scene has table-backed emissive triangles,
any scene edit resets reservoir history conservatively, since light-table
slots are local to a scene generation.

## Fixtures

**Core.** `EmissiveLifecycle` in `lotus-renderer-restir-gpu` renders a 16x16
Lambert plane under a distant light and a two-triangle emissive panel, with
temporal reservoir reuse and temporal radiance accumulation enabled together,
once with temporal-only and once with spatiotemporal reuse, under both
integrators. **Hydra.** `Animation` in `lotus-renderer-hydra-restir-gpu`
renders a 32x32 USD stage (floor, wall, a panel with a `UsdPreviewSurface`
material, a sun and an emissive lamp quad) through `HdRenderIndex`,
UsdImaging scene indices and `HdEngine`, with `lotus:restir`,
`lotus:restirTemporal` and `lotus:temporal` enabled, under both integrators.
Both apply the same sequence of edits, one per frame:

| Frame | Core edit | Hydra (USD) edit | Expected reason | Emissive triangles |
| --- | --- | --- | --- | --- |
| 1 | — | — | `FirstFrame` | 2 |
| 2 | none | none | retained | 2 |
| 3 | camera translation | `/Camera` translate | retained | 2 |
| 4 | emission (4, 2, 1) → (1, 2, 4) | `/Sun` intensity 2 → 3 | `SceneChanged` | 2 |
| 5 | none | none | retained | 2 |
| 6 | plane placement (nonemissive) | panel `diffuseColor` | `SceneChanged` | 2 |
| 7 | emitter placement | lamp `emissiveColor` (8, 6, 4) → (4, 6, 8) | `SceneChanged` | 2 |
| 8 | emission off | none | `SceneChanged` / retained | 0 / 2 |
| 9 | plane placement | panel translate (nonemissive) | retained / `SceneChanged` | 0 / 2 |
| 10 | emission on | lamp translate | `SceneChanged` | 2 |
| 11 | emitter hidden | lamp `emissiveColor` (0, 0, 0) | `SceneChanged` | 0 |
| 12 | emitter visible | panel translate back | `SceneChanged` / retained | 2 / 0 |
| 13 | none | lamp `emissiveColor` (8, 6, 4) | retained / `SceneChanged` | 2 |
| 14 | — | lamp `visibility` invisible | `SceneChanged` | 0 |
| 15 | — | lamp `visibility` inherited | `SceneChanged` | 2 |
| 16 | — | none | retained | 2 |

Where the two columns differ, the reason and count are given as core / Hydra.
The core sequence has 13 frames and the Hydra sequence 16. In both, a
nonemissive placement edit resets history while the emitter emits (core frame
6, Hydra frame 9) and retains it once the emission is zero (core frame 9,
Hydra frame 12).

## Checks

Every frame checks:

- the reset reason and `history_available` against the table, and the
  emissive light-table size from the GPU scene statistics;
- on a reset: no preceding reservoir or radiance products are published,
  every resolved temporal pixel has no previous source (`previous_index`
  UINT32_MAX) and age zero, equals its fresh initial reservoir byte for byte,
  and has at most one accumulated radiance sample;
- on retention: preceding reservoir and radiance products are published and
  some pixel actually reuses its history;
- zero captured Vulkan validation messages.

Hydra frames also check the reservoir AOVs against the backend products and
that no Hydra diagnostics are raised (the existing `Session::Render` checks).

**Integrator agreement.** Each fixture keeps every frame's reset reason,
resolved reservoir records and colour from the reference integrator, and the
wavefront run must reproduce them byte for byte; it does, for all 26 core
frames (temporal and spatiotemporal) and all 16 Hydra frames. Reset
classification is therefore the same function of the edit sequence in both
integrators and in both entry points.

**Sensitivity.** With the conservative emissive condition removed from the
backend's compatibility test, `EmissiveLifecycle` fails at core frame 6
("emissive reset reason/history"); restored, it passes.

## Results

Both tests pass. In the Hydra sequence, retained frames reuse history at
558–586 of 1024 pixels and reset frames at none; 372–474 pixels hold a
positive light reservoir. Hydra applies the backend's reset policy unchanged:
the USD light, material, emission, placement and visibility edits arrive as
new scene snapshots and are classified by the same compatibility test as the
headless edits.

## Validation

```sh
ost build --jobs auto
ost test --jobs 8
ost validate --strict-renderer-evidence
ost build --profile lookdev --intent hydra --jobs auto
ost test --profile lookdev --intent hydra
ost validate --profile lookdev --intent hydra
ost build --without-runtime --intent ci-core --jobs auto
ost test --without-runtime --intent ci-core --jobs 8
python -m unittest discover -s scripts -p 'test_*.py'
python scripts/check_docs.py
```

The managed core build passes all 48 tests under `ost test --jobs 8` in 288
seconds, followed by strict renderer-evidence validation. The Hydra build
passes all 75 tests in 1200 seconds, and its validation passes. The
runtime-free build/test records 35 passes and two explained GPU SKIPs. All
58 Python tests and documentation validation pass. The installed usdview
host and the release gate are not rerun for this test-only change.

## Disposition

Emissive emission, toggle, visibility and placement edits, and light,
material, emissive and placement edits made through Hydra, keep the reset
contract: history resets exactly where the policy says, no stale reservoir or
radiance sample survives a reset, classification is identical under both
integrators, and validation is clean. With the
[headless light/material animation](2026-10-17-restir-animation.md) this
completes the animation-and-reset criterion of the Renderer Phase 5 exit.

Not exercised here: emission textures, instanced or deforming emitters,
environment-map animation and production scenes. Rigid emitter motion under
radiance accumulation alone (without reservoir reuse) retains radiance
history by the [temporal reset policy](../reference/TEMPORAL.md); the
conservative reset applies only with reservoir reuse. Fresh batches still
replay random numbers after each reset
([DES-Q7](../design/DESIGN_POLICY.md#53-open-questions)). The reuse-cost and
equal-quality decision remains in the
[execution plan](../roadmap/current.md#step-1--restir-di-closure). No phase
exit is claimed.
