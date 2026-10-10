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
ost test
ost validate
```

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
and [scheduling](../reports/2026-10-10-wavefront-scheduling.md#performance)
measurements). `--benchmark-size <pixels>` measures another square target.
For a new revision, put its identity in the label. JSON and final PFM images
are written under `build/`; preserve them when comparing changes. The
[benchmark reference](../reference/SCENE.md#fixed-benchmarks) owns workload
identities, defaults, metric scopes, capability handling and exit codes.
CTest uses 16 measured/2 warmup frames for the report regression; the
performance baseline uses the default 64/8.

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
