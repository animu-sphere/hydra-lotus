# Building and testing

Every command on this page has been run in this repository; the run is
[ost report 01](../reports/ost/01-2026-10-04-v0.23.14-renderer-template-bootstrap.md).
What each build contains is
[PROJECT_LAYOUT.md §5](../architecture/PROJECT_LAYOUT.md#5-build-intents-and-runtime-profiles),
and what it was measured on is
[SUPPORTED_CONFIGURATIONS.md](../reference/SUPPORTED_CONFIGURATIONS.md).

## Prerequisites

- `ost` 0.23.14 or newer (`ost --version`).
- A Vulkan SDK that provides Vulkan 1.3 and `slangc` (bundled from 1.3.296).
  Without it the build still succeeds, and the GPU checks report `SKIP`.
- CMake 3.24 or newer, Ninja, and a C++20 compiler. On Windows, `ost` loads
  the MSVC environment itself.

## The default build — no OpenUSD

```sh
ost build --jobs auto
ost test --jobs 8
ost validate
```

`ost test --jobs` takes a number (not `auto`). The GPU checks that assert no
timing share the device, so eight jobs ran the core suite in 228 seconds
instead of 859 serially on an RTX A5000 with validation layers enabled. The
fixed performance baseline and install-tree checks still run alone.

`ost build` runs `lotus-headless`, which renders the bootstrap frame 1,000
times, checks primary-ray barycentrics and depth against CPU projections and
path-traced radiance against known values when ray queries are available,
and writes `build/<target>/renderer-report.json`. `ost validate` reads
it; the Hydra assertions are `SKIP` in this build by design.

Inspect `renderer.validation.synchronization` in the generated evidence.
The [synchronization contract](../reference/CAPABILITY_MATRIX.md#renderer-foundation)
owns enablement, unavailable-capability behavior and the CTest checks;
[measured runs](../reports/2026-10-09-synchronization-ci.md) include strict
physical-GPU validation and the missing-driver regression.

The same run renders the reference scene at 1, 16, 64, 256 and 1024 spp
into `build/<target>/reference-images/` and compares each image with the
committed reference in `validation/reference/`
([scene reference](../reference/SCENE.md#reference-images)). PFM files open
in most HDR image viewers. When a change is meant to alter the reference
image, regenerate the reference with deterministic mode and commit both
files:

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --write-reference validation/reference
```

## Fixed performance baseline

After the default build, run the independent benchmark mode:

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/performance-baseline-1.json --benchmark-label "ae72568 + LOTUS-PERF-01; ost core Release"
```

This invocation was measured three times, changing the output suffix to `2`
and `3` ([baseline evidence](../reports/2026-10-10-performance-baseline.md)).
To measure the wavefront integrator, add `--benchmark-integrator wavefront`:

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/wavefront-baseline-1.json --benchmark-label "d6a6368 + LOTUS-WAVE-01; ost core Release" --benchmark-integrator wavefront
```

([equivalence](../reports/2026-10-10-wavefront-equivalence.md#performance)
[scheduling](../reports/2026-10-10-wavefront-scheduling.md#performance)
and [path-state](../reports/2026-10-10-wavefront-path-state.md#performance)
measurements). `--benchmark-size <pixels>` measures another square target.
For a new revision, put its identity in the label. JSON and final PFM images
are written under `build/`; preserve them when comparing changes. The
[benchmark reference](../reference/SCENE.md#fixed-benchmarks) owns workload
identities, defaults, metric scopes, capability handling and exit codes.
CTest uses 16 measured/2 warmup frames for the report regression; the
performance baseline uses the default 64/8.

## Reservoir comparison

After the default build, run the separate static/moving many-light comparison:

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark-restir build/restir-comparison-reference.json --benchmark-label "49f8d79 + headless reservoir comparison; core Release"
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark-restir build/restir-comparison-wavefront.json --benchmark-label "49f8d79 + headless reservoir comparison; core Release" --benchmark-integrator wavefront
```

Use the identity of the build being measured in its label. The
[reservoir reference](../reference/RESTIR.md#headless-reservoir-comparison)
owns workloads, settings, metric scopes and limits; the
[measured comparison](../reports/2026-10-14-restir-comparison.md) records these
runs. JSON raw populations and sibling PFM directories remain separate from
OpenStrata's renderer correctness report.

For matched candidate-proposal comparisons, add `--restir-proposal uniform`
or `--restir-proposal power-uniform`, keeping other settings and seeds the
same and using distinct output paths. Omitting this option keeps `power-alias`.
The [proposal measurements](../reports/2026-10-15-restir-proposals.md) record
the three-way comparison under both integrators.

For repeated quality budgets, use distinct measured seeds and output paths,
keeping all other settings and the build label identical:

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark-restir build/restir-budgets-reference-0.json --benchmark-label "7f0f6ab + repeated quality budgets; core Release; 2026-10-15" --benchmark-integrator reference --benchmark-size 64 --benchmark-frames 64 --benchmark-warmup 4 --restir-sequences 4 --restir-lights 256 --restir-reference-samples 256 --restir-candidates 1,8 --restir-history-cap 32 --restir-seed 17
```

Repeat with seeds `262165` and `524313`, using output suffixes `-1.json` and
`-2.json`. Then reduce the three reports:

```sh
python scripts/restir_budget.py build/restir-budgets-reference-0.json build/restir-budgets-reference-1.json build/restir-budgets-reference-2.json --rmse 0.5 0.35 0.25 --output build/restir-budgets-reference-summary.json
```

The [budget contract](../reference/RESTIR.md#repeated-quality-budgets) owns
attainment, missing results and paired timing semantics. Substitute your build
identity in the shared label. Measure each integrator/proposal/settings group
separately, without overlapping GPU tests or benchmarks.

For mixed area/environment lighting and material/depth boundaries, select the
alternate workload pair explicitly:

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark-restir build/restir-boundaries-reference-0.json --benchmark-label "0aeeb5d + mixed boundary workloads; core Release; 2026-10-15" --benchmark-integrator reference --benchmark-size 32 --benchmark-frames 32 --benchmark-warmup 4 --restir-sequences 4 --restir-lights 64 --restir-reference-samples 512 --restir-candidates 1,8 --restir-history-cap 32 --restir-seed 17 --restir-workload mixed-boundaries
```

Repeat with seeds `262165` and `524313` and output suffixes `-1.json` and
`-2.json`, then repeat the group using `wavefront` and distinct output paths.
Use your measured build identity in the label. Reduce each integrator's group:

```sh
python scripts/restir_budget.py build/restir-boundaries-reference-0.json build/restir-boundaries-reference-1.json build/restir-boundaries-reference-2.json --rmse 0.2 0.1 0.05 --output build/restir-boundaries-reference-summary.json
```

These budgets use finite NEE/MIS references, with reference split discrepancies
and central-boundary errors retained in the summary. The
[boundary measurement](../reports/2026-10-15-restir-boundaries.md) records the
runs and their limits.

For narrow glossy reflections and camera-only foreground disocclusion, select
`--restir-workload glossy-disocclusion`:

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark-restir build/restir-glossy-reference-0.json --benchmark-label "656da5e plus glossy disocclusion workloads; Release; 2026-10-16" --benchmark-integrator reference --restir-workload glossy-disocclusion --benchmark-size 32 --benchmark-frames 16 --benchmark-warmup 4 --restir-sequences 4 --restir-lights 64 --restir-reference-samples 512 --restir-candidates 1,8 --restir-history-cap 32 --restir-seed 17
```

Repeat with seed `262165` and suffix `-1.json`, then reduce using
`scripts/restir_budget.py` as above. Repeat the group with `wavefront` and
distinct paths. Substitute your build identity in the shared label. The
[glossy measurement](../reports/2026-10-16-restir-glossy.md) records both
integrators, finite-reference errors and the remaining convergence gap.

For a rough dielectric/metal pair with rect/sphere lights and camera-only
motion, select `--restir-workload glossy-spheres`:

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark-restir build/restir-spheres-reference-0.json --benchmark-label "9595454 plus glossy sphere dielectric workloads; Release; 2026-10-16" --benchmark-integrator reference --restir-workload glossy-spheres --benchmark-size 32 --benchmark-frames 32 --benchmark-warmup 4 --restir-sequences 4 --restir-lights 64 --restir-reference-samples 512 --restir-candidates 1,8 --restir-history-cap 32 --restir-seed 17
```

Repeat with seed `262165` and suffix `-1.json`, then repeat with `wavefront`
and distinct paths. Reduce each pair with `scripts/restir_budget.py` as above.
Use the identity of your measured build. The
[sphere/dielectric measurement](../reports/2026-10-16-restir-glossy-spheres.md)
records convergence, three fixed screen bands, temporal residuals and cost.

## The Hydra adapter

The adapter needs a real OpenUSD imaging runtime. This repository was measured
with an OpenUSD 26.08 `lookdev` runtime the workstation had already adopted;
`ost runtime list` shows what is available, and OpenStrata's
[adoption guide](https://github.com/animu-sphere/open-strata/blob/main/docs/guides/adopt-a-renderer-project.md#2-adopt-a-digest-pinned-runtime)
covers adopting one.

```sh
ost runtime list
ost build --profile lookdev --intent hydra --jobs auto
ost test --profile lookdev --intent hydra
ost validate --profile lookdev --intent hydra
```

`ost test` includes `lotus-renderer-usdview-host`, which opens `testusdview`
on the installed smoke scene for a few seconds and keeps
`usdview-first-frame.png` and `usdview-stable-update.png` under
`build/<target>--hydra/adapters/hydra2/usdview-install/`.

To look at the renderer interactively, open `usdview` with Lotus selected:

```sh
ost renderer view --profile lookdev --intent hydra
```

It builds and installs its own Hydra tree
(`build/<target>--hydra--renderer-hydra2`, installed under
`.strata/renderer-view/`) and opens the installed smoke scene through its
`/Camera`; pass a USD file to open another scene. The command returns when
the window is closed.

`ost plugin view <bundle> <fixture>` is the equivalent for OpenUSD plugin
bundles. It needs an `openstrata.plugin.yaml`, which this renderer project
does not have, so here it stops with "no openstrata.plugin.yaml".

## The standalone viewport

```sh
ost renderer viewport -- --frames 8 --hidden
ost validate --intent renderer-viewport
```

The first run fetches GLFW. Omit the arguments after `--` for an interactive
window. The viewport builds in a tree of its own
(`build/<target>--renderer-viewport`), so it leaves the default target's
validation untouched.

> ⚠️ On a Windows host whose MSVC prints Japanese, some objects record no
> header dependencies, and editing a header does not rebuild them. Delete
> the affected objects before building
> ([tooling limitations](../reference/SUPPORTED_CONFIGURATIONS.md#build-and-tooling-limitations)).

## Plain CMake

The project is an ordinary CMake project. `ost` adds the runtime prefix, the
generator and the evidence bookkeeping. The options are `LOTUS_ENABLE_VULKAN`
(default `ON`), `LOTUS_ENABLE_HYDRA2`, `LOTUS_ENABLE_VIEWPORT` and
`LOTUS_BUILD_TESTS` (default `ON`). A plain-CMake tree is validated with
`ost validate --build-dir <dir>`, which does not claim `ost` built it.

## CI contracts

Inspect and validate the CI cells from the repository root:

```sh
ost ci validate
ost ci plan
ost ci matrix --cell hydra-windows --json
```

The source workflow runs on pull requests, main pushes and manual dispatch.
Its runtime pins live in `openstrata.ci.yaml`; `ost ci validate` also checks
that the external workflow consumes them and uses the declared CLI version.
Do not generate a plugin workspace workflow for this renderer: the generated
graph gate requires descriptors this repository does not own.

The core CI configuration deliberately proves operation without a runtime or
Vulkan SDK:

```sh
ost build --without-runtime --intent ci-core --jobs auto
ost test --without-runtime --intent ci-core
```

OpenStrata 0.23.14's `validate` cannot select that runtime-free target. CI
checks the core and install-tree PASS assertions and the explained GPU SKIP
in its report after `ost test`. The Hydra cell instead runs the ordinary
`hydra` build, test and strict renderer-evidence validation against the pinned
OpenUSD runtime, with matching Python and a checksum-verified Vulkan SDK.
The usdview host test skips when headless evidence explicitly reports no GPU
capability; failed or missing GPU evidence is an error.

Hosted core/Hydra execution and explained capability SKIPs have been
verified ([foundation closure](../reports/2026-10-10-foundation-closure.md#hosted-follow-up));
strict physical-GPU rendering is measured separately in the local reports.
