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
  Without it configuration fails; a CPU-only build sets
  `LOTUS_ENABLE_VULKAN=OFF`, as [core CI](#ci-contracts) does, and its GPU
  checks report `SKIP`.
- CMake 3.24 or newer, Ninja, and a C++20 compiler. On Windows, `ost` loads
  the MSVC environment itself.

## The default build — no OpenUSD

```sh
ost build --jobs auto
ost test --jobs 8
ost validate
```

`ost test --jobs` takes a number (not `auto`). The GPU checks that assert no
timing share the device; the fixed performance baseline and install-tree
checks still run alone. The default suite's
[measured times](../reports/2026-10-18-test-tiers.md#results) are in the
test-tier report.

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

## Test tiers

Every CTest carries one [tier](../design/TEST_POLICY.md#2-tiers) label:
`fast` (no Vulkan device needed), `gpu` (renders when a device is present)
or `research` (the campaign procedures of Renderer Phases 5 and 6).
`lotus-test-tiers` fails when a test has no tier or more than one. The
default build registers `fast` and `gpu`; the `research` intent adds the
research tier:

```sh
ost build --intent research --jobs auto
ost test --intent research --jobs 8
```

`ost test` selects tests only by name (`--filter`). To run one tier or one
area (`hydra`, `viewport`), call CTest on the build directory:

```sh
ctest --test-dir build/cy2026-windows-x86_64-py313-core -L fast
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

## Shared static performance workloads

After building the default and viewport trees, the shared manifest drives
both frontends with the same fixture, camera and rendering settings:

```sh
python scripts/performance_baseline.py --headless build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless.exe --viewport build/cy2026-windows-x86_64-py313-core--renderer-viewport/adapters/viewport/lotus-viewport.exe --output build/shared-baseline-reference --label "952372f plus shared static workloads; core Release; 2026-10-21" --clocks "Unlocked; initial nvidia-smi P0 graphics 1905 MHz memory 8001 MHz; driver 597.16"
```

This run and its viewport driver failure are preserved in the
[shared baseline report](../reports/2026-10-21-shared-performance-workloads.md).
Use the identity and clock conditions actually measured, and a fresh output
directory. Omit `--viewport` to collect headless evidence independently of
presentation failures; `--integrator wavefront` selects the other integrator.
`--workload <id>` selects a manifest subset, and `--plan` prints commands
without writing or running. Select each workload only once. The runner stops
on failure, retains earlier results and never retries a failed launch.
Inspect the failed attempt's `command`, `log`, `report` and `exit_code` in
`run.json`; timeout logs include partial process output. Exit 77 denotes an explained
capability skip, not performance or correctness evidence. The
[benchmark reference](../reference/SCENE.md#fixed-benchmarks) defines scopes,
unavailable metrics and the remaining motion/quality measurements.

To observe NVIDIA device conditions during a campaign, add
`--gpu-telemetry nvidia-smi` (optionally `--gpu-sample-ms 100`). This records
raw clock, power-state, utilization and device-memory samples in a separate
`*.gpu.json` for each attempt; `run.json` links them and distinguishes
available, partial and unavailable collection. A missing tool or failed
query does not change the renderer result. Inspect query diagnostics before
using the observations. Collection is disabled by default, may perturb
timing and never locks clocks. Device-memory maxima include other processes
and do not measure peak renderer residency. The
[collector report](../reports/2026-10-24-gpu-telemetry.md) records a tested
invocation and its uninstrumented control.

### Camera and object motion

`--motion camera`, `transform` or `deform` edits every measured headless
frame instead of accumulating one pose (viewport runs reject it). The
[motion campaign](../reports/2026-10-25-motion-workloads.md) ran each motion
under both integrators into its own output directory:

```sh
python scripts/performance_baseline.py --headless build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless.exe --integrator reference --motion deform --output build/motion-deform-reference-20261025 --label "516c659 plus LOTUS-PERF-01 motion (uncommitted); core Release; 2026-10-25" --clocks "Unlocked; initial nvidia-smi P8, 210 MHz, 405 MHz; driver 597.16"
```

A single fixture can be measured directly with
`lotus-headless --benchmark <json> --benchmark-workload <id> --benchmark-motion <kind>`.

### Host phases of a frame

Each report's `render_host_ms` splits the blocking `RenderScene` wall time
into preparation, recording, submission, fence wait, readback and results
([contract](../reference/SCENE.md#fixed-benchmarks)). Validation layers add
to most of them; the [readback evaluation](../reports/2026-10-25-frame-host-split.md)
also measured single fixtures without them:

```sh
VK_LOADER_LAYERS_DISABLE=VK_LAYER_KHRONOS_validation build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/host-split.json --benchmark-workload cornell-v1 --benchmark-light-sampling mis --benchmark-size 512
```

The report's `validation.enabled` records which condition applied.

### Light-count control

`--benchmark-light-grid <1..16>` replaces `many-lights-v1`'s 16×16 lights by
a smaller grid at the same footprint and total power
([contract](../reference/SCENE.md#fixed-benchmarks)). The
[crossing-cost evaluation](../reports/2026-10-25-light-crossing-cost.md) ran
sides 1, 2, 4, 8 and 16 per integrator and size, for example:

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark build/light-grid.json --benchmark-workload many-lights-v1 --benchmark-light-sampling mis --benchmark-integrator wavefront --benchmark-light-grid 4
```

### Renderer residency

`--benchmark-residency` lists the renderer's allocations by owner and the
process's heap usage in each frame-state mode
([contract](../reference/SCENE.md#renderer-residency)). The
[residency report](../reports/2026-10-26-frame-residency.md) ran one process
per size:

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark-residency build/residency-1920x1080.json --benchmark-label "5e598b6 + residency; core Release" --residency-width 1920 --residency-height 1080
```

The [lazy-mirror remeasurement](../reports/2026-10-26-lazy-readback-mirrors.md)
used the same command, plus a control with `--residency-read-back 1`, which
reads every frame back and so allocates the host mirrors. The
[reservoir history split](../reports/2026-10-26-reservoir-history-split.md)
added pass timings from 16-frame runs of the reservoir mode, with and
without spatial reuse:

```sh
build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless --benchmark-residency build/history-split-1080p.json --benchmark-label "split" --residency-width 1920 --residency-height 1080 --residency-mode restir-reuse --residency-frames 16 --residency-restir-spatial 0
```

## Cornell quality checkpoints

After the default build, measure error against the committed reference with
a disjoint measured sample sequence:

```sh
python scripts/quality_curve.py --headless build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless.exe --output build/quality-cornell-reference-20261025 --label "53f737a + independent quality checkpoints; core Release; 2026-10-25" --clocks "Unlocked; initial nvidia-smi P3 graphics 1410 MHz memory 5001 MHz; driver 597.16; no clock settings changed" --rmse-target 0.2 --rmse-target 0.1 --rmse-target 0.05
```

Use the actual source/build and clock conditions in the label and a fresh
directory. `--integrator wavefront` uses the other path tracer. Defaults are
1, 16, 64 and 256 spp, three repetitions and eight warmup frames;
`--frames 1 16 64`, `--runs 1` and `--warmup 2` can bound a smaller run.
`quality.json` links each checkpoint's `run.json` and retains each repeat's
RGB error and summed rendering times. Unreached error targets have null
times. The [contract](../reference/SCENE.md#fixed-benchmarks) defines the
finite-reference and timing limits; the
[report](../reports/2026-10-25-quality-curve.md) records both integrators.

For a quality population across disjoint measured sequences, repeat
`--sample-index`; `--runs` remains the timing repetition count for each
sequence. The bounded population comparison uses:

```sh
python scripts/quality_curve.py --headless build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless.exe --output build/quality-populations-reference-20261025 --label "c3ea4a4 plus quality population runner; existing core Release binary; 2026-10-25" --clocks "Unlocked; initial nvidia-smi P8 graphics 210 MHz memory 405 MHz; driver 597.16; no clock settings changed; polling disabled" --sample-index 1048576 --sample-index 1114112 --sample-index 1179648 --runs 2 --rmse-target 0.2 --rmse-target 0.1 --rmse-target 0.05
```

Repeat with `--integrator wavefront` and a fresh output path after the first
campaign finishes. `quality.json` now uses `lotus-quality-curve-v2`, with
`sample_indices`, sequence-tagged checkpoints/curves and `sample_populations`.
The latter counts each sequence once; deterministic timing repetitions
do not count as new quality observations. The
[population report](../reports/2026-10-25-quality-populations.md) records
the evaluation budget, results and remaining limits.

### Other static workloads

Create one finite Reference PT baseline for the selected workload before
measuring it. For the bounded `textured-v1` comparison, create this manifest
in PowerShell:

```powershell
$qualityManifest = Get-Content validation/performance-workloads.json -Raw | ConvertFrom-Json
$qualityManifest.settings.frames = 4096
$qualityManifest.settings.runs = 1
$qualityManifest.workloads = @('textured-v1')
$qualityManifest | ConvertTo-Json -Depth 8 | Set-Content build/quality-textured-reference-manifest-20261025.json -Encoding utf8
```

Run the reference, then pass its successful single-attempt `run.json` to the
quality runner. Use your source/build identity and actual clock conditions:

```sh
python scripts/performance_baseline.py --manifest build/quality-textured-reference-manifest-20261025.json --headless build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless.exe --output build/quality-textured-finite-reference-20261025 --label "ca7edbf plus workload quality runner; unchanged core Release binary; 2026-10-25" --clocks "Unlocked; initial nvidia-smi P0 graphics 1695 MHz memory 8001 MHz; driver 597.16; polling disabled; no clock changes"
python scripts/quality_curve.py --headless build/cy2026-windows-x86_64-py313-core/adapters/headless/lotus-headless.exe --workload textured-v1 --reference-run build/quality-textured-finite-reference-20261025/run.json --output build/quality-textured-reference-20261025 --label "ca7edbf plus workload quality runner; unchanged core Release binary; 2026-10-25" --clocks "Unlocked; initial nvidia-smi P0 graphics 1695 MHz memory 8001 MHz; driver 597.16; polling disabled; no clock changes" --integrator reference --sample-index 1048576 --sample-index 1114112 --runs 2 --rmse-target 0.2 --rmse-target 0.1 --rmse-target 0.05
```

After the first campaign finishes, repeat the second command with
`--integrator wavefront` and a fresh output directory. The reference PFM,
benchmark JSON, process log and run summary are copied into each campaign's
`reference/` directory. The [contract](../reference/SCENE.md#fixed-benchmarks)
defines input validation and range separation. The
[textured report](../reports/2026-10-25-textured-quality.md) also compares
a second 4096-spp reference made with the first command plus
`--sample-index 65536` and output `build/quality-textured-split-reference-20261025`.
That reference discrepancy is recorded without subtracting it from errors.

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

`lotus-viewport` path-traces a scene progressively and presents each frame
from the backend: the colour never leaves the GPU. Without a stage it renders
the headless runner's reference Cornell box:

```sh
ost renderer viewport
ost renderer viewport -- --frames 8 --hidden
ost validate --intent renderer-viewport
```

The first run fetches GLFW and Dear ImGui (and, for the Hydra-enabled build,
nativefiledialog-extended). The viewport builds in a tree of its own
(`build/<target>--renderer-viewport`), so it leaves the default target's
validation untouched, and that build has no OpenUSD dependency.

To open a USD stage, build the Hydra-enabled viewport with the `viewport-usd`
intent and a real OpenUSD runtime, as for [the Hydra adapter](#the-hydra-adapter):

```sh
ost renderer viewport --intent viewport-usd --profile lookdev -- --usd path/to/stage.usda
```

It drives the Hydra adapter in process: UsdImaging populates a render index
over the Lotus render delegate, and the render pass presents to the window.
The stage is framed as usdview frames it initially (its bounds, authored up
axis and a 60 degree field of view), at the stage's start time code when it
authors a time range and at the default time otherwise.
On Windows that build also writes `run-lotus-viewport.cmd` beside the
executable (`build/<target>--viewport-usd--renderer-viewport/adapters/viewport/`),
which puts the OpenUSD runtime's DLLs on `PATH` for that process only:

```sh
run-lotus-viewport.cmd --usd path/to/stage.usda
```

On the measured NVIDIA driver a launch can stop at frame 2 with
`VK_ERROR_DEVICE_LOST`; launch it again
([limitation](../reference/SUPPORTED_CONFIGURATIONS.md#driver-limitations)).

`ost renderer viewport --intent viewport-usd` launches the copy that the
`lotus-renderer-usdview-host` test installs in that tree
(`adapters/hydra2/usdview-install/bin/`), which is stale until that test runs
again after a change; the launcher above runs the executable just built
([evidence](../reports/2026-10-17-viewport-camera.md#limits)).

| Option | Effect |
| --- | --- |
| `--usd <stage>` | open a USD stage (Hydra-enabled build only) |
| `--camera <prim>` | look through the stage's camera prim at first, instead of the free camera |
| `--time T` | open the stage at time code `T` |
| `--width N`, `--height N` | window size, 1280x720 by default |
| `--fullscreen` | start on the primary monitor; F11 toggles while running |
| `--samples N` | samples per pixel to accumulate, 1024 by default; the accumulation restarts on a resize |
| `--integrator reference\|wavefront` | the path tracer to start with |
| `--nee on\|off` | next event estimation with MIS, on by default; off leaves BSDF sampling alone |
| `--restir off\|initial\|temporal\|spatial\|combined` | reservoir direct lighting at the camera surface, off by default |
| `--temporal-radiance` | reuse each pixel's radiance history |
| `--denoise` | filter the temporal radiance with SVGF's edge-aware à-trous filter; turns temporal radiance on |
| `--filter-iterations N` | the filter's à-trous iterations, 1–8, 5 by default |
| `--no-accumulation` | trace one fresh sample per pixel every frame |
| `--view <name>` | the product to present: `beauty`, `depth`, `normal`, `motion`, `temporal-validity`, `raw`, `accumulated`, `history-length`, `variance` (these four need `--temporal-radiance` or `--denoise`), `reservoir-validity`, `reservoir-age`, `reservoir-weight` (these three need `--restir`) |
| `--split` | present the view on the left half and beauty on the right |
| `--vsync on\|off` | FIFO, or immediate (else mailbox) presentation |
| `--frames N`, `--hidden` | scripted runs: exit after N presented frames, without showing the window |
| `--validate` | fail unless the Khronos validation layer captured the run; captured messages fail every run |
| `--resize-test` | resize once after the second frame and fail unless the swapchain followed |
| `--interaction-test` | apply the scene's scripted inputs and edits, one per frame from the second, and fail unless each restarts or continues the accumulation as it should |
| `--allow-unavailable` | exit 77 instead of 1, with a `SKIP:` explanation, where no driver or device can present |
| `--benchmark <file>` | write the run's frame counts, CPU and GPU times, each GPU pass's mean time, the latest scene update, renderer memory, swapchain and validation state as JSON (`lotus.viewport-benchmark/v1`) |

The free camera follows usdview's bindings, without requiring Alt:

| Input | Action |
| --- | --- |
| left drag | tumble, a quarter degree per pixel |
| middle drag, or Ctrl + left drag | track: the point under the cursor follows it |
| right drag, wheel | dolly |
| F | restore the initial framing |
| C | the stage's next authored camera, then the free camera again |
| Left, Right | one time code back or forward, within an authored time range |
| R | reload the stage's layers |
| F1 | hide or show the developer UI |
| F2 | switch the denoiser on or off |
| F11, Esc | fullscreen, exit |

C, the arrows and R act on USD stages only. Navigating while looking through
an authored camera hands its view, conformed to the window, to the free
camera, which then orbits the point in front of it at the stage bounds'
depth; roll is kept, scale and an orthographic projection are not. Every
frame applies the stage's pending edits (reloaded layers, a new time code)
before rendering. A changed camera or scene restarts the accumulation; a
time code that changes nothing continues it.

The Renderer window (Dear ImGui, drawn over the presented frame) changes the
options above while the viewport runs: the integrator, the sample limit and
accumulation, NEE/MIS, the ReSTIR mode and candidate count, temporal radiance
reuse and its history length, the denoiser and its iteration count, and the
presented product with its colour scale and an optional split at a chosen
position. It shows the extent and samples per pixel. Pointer and keyboard input
over the window goes to it rather than the camera; it writes no files. The
settings logic is `adapters/viewport/controls.{hpp,cpp}`, which
`lotus-viewport-controls` covers without a window or GPU.

In the Hydra-enabled build the Renderer window's **Open USD...** button picks
a stage (`.usd`, `.usda`, `.usdc`, `.usdz`) from the native file dialog, as
hydra-merlin's viewport does, and renders it in the same window with the
controls as they are; the built-in scene can open a stage the same way. A
picked stage opens through the free camera at its start time code, with a new
renderer, so its accumulation and history start again. A stage that does not
open leaves the current one running and says why in the window.

The Timing window (the timing HUD) shows, for the latest frames that ran the
same passes (up to 32, started again when the controls change the passes):

- the host's frame time (rendering and presenting, the developer UI
  included; with vsync on it follows the display) and the GPU time of every
  timed submission, each with a graph of the last 120 frames;
- the GPU passes that ran, each with its share of the GPU time: the path
  trace (with the wavefront kernels under it), the ReSTIR initial, temporal
  and spatial passes, the temporal surfaces (primary surfaces and
  reprojection), temporal accumulation, the variance estimate and the
  denoise filter (its à-trous iterations and composite). Together they add
  up to the GPU time;
- the latest scene update that submitted GPU work, and before which frame:
  its upload, BLAS and TLAS build or refit times;
- the reservoir, surface-history, radiance-history and denoise-filter memory
  (device buffers with their host mirrors, as the latest frame that used
  them reported it;
  "retained" when the controls no longer use them, since the backend keeps
  them allocated), the scene's backing memory and its BLAS and TLAS storage.

The figures come from `adapters/viewport/timings.{hpp,cpp}`, which
`lotus-viewport-timings` covers without a window or GPU, and `--benchmark`
writes the run's means of the same passes
([evidence](../reports/2026-10-18-viewport-timing.md)).

Changing the integrator, the estimator or the candidate count restarts the
accumulation; the sample limit and the presented product continue it.
Temporal and spatial reuse and temporal radiance trace one fresh sample per
pixel every frame, through `RenderTemporalScene`, as does turning
accumulation off; leaving them restarts the accumulation. The depth, normal,
motion and temporal-validity views present the temporal primary-surface
products, so they render through `RenderTemporalScene` too (without reuse
the accumulation continues); the reservoir views present the last fresh
sample's final reservoir and need ReSTIR. Every view maps an existing GPU
debug product on the GPU (`Lotus::DebugView`); nothing is read back, the
temporal and reservoir products included
([evidence](../reports/2026-10-18-viewport-controls.md)).

The denoiser filters the temporal radiance, so it traces one fresh sample a
frame and turns temporal radiance on while it runs; switching it, or
changing its iteration count, keeps the history, since the filter never
writes into it. Its inputs have views of their own: `raw` is the frame's
fresh sample before any reuse, `accumulated` the temporal mean before the
filter, `history-length` each pixel's samples of history (to the history
length's full scale) and `variance` the illumination luminance variance that
steers the filter, on a log10 scale; beauty is the filtered image. With
**Split**, the view fills the frame left of the divider and beauty the
rest, so `raw` or `accumulated` against beauty compares the filter's input
and output on the same frame
([evidence](../reports/2026-10-18-svgf-viewport.md)).

The presented image is the linear radiance encoded as sRGB and clamped, with
no exposure or tone mapping. The title bar shows the scene, the camera and time code, the
integrator, estimator and view, device, extent, samples per pixel and the
last frame's GPU time. Validation is enabled
whenever the Vulkan SDK's layer is installed, which costs CPU time; set
`VK_LOADER_LAYERS_DISABLE=VK_LAYER_KHRONOS_validation` for an interactive
session without it.

CTest runs `lotus-viewport-present` and `lotus-viewport-interaction` (the
built-in scene), `lotus-viewport-hydra`, `lotus-viewport-hydra-interaction`
and `lotus-viewport-hydra-camera` (the smoke stage, Hydra-enabled build
only) and `lotus-viewport-no-driver` (an explained skip with Vulkan driver discovery
pointed at a missing manifest), each in the viewport's tree, plus the
window-free `lotus-viewport-options`, `lotus-viewport-controls`,
`lotus-viewport-timings` and `lotus-viewport-camera` in every tree. The interaction tests also step
through every renderer control and debug view, the denoiser and the split
view included.

> ⚠️ On a Windows host whose MSVC prints Japanese, some objects record no
> header dependencies, and editing a header does not rebuild them. Delete
> the affected objects before building
> ([tooling limitations](../reference/SUPPORTED_CONFIGURATIONS.md#build-and-tooling-limitations)).

## Plain CMake

The project is an ordinary CMake project. `ost` adds the runtime prefix, the
generator and the evidence bookkeeping. The options are `LOTUS_ENABLE_VULKAN`
(default `ON`), `LOTUS_ENABLE_HYDRA2`, `LOTUS_ENABLE_VIEWPORT` and
`LOTUS_BUILD_TESTS` (default `ON`). An enabled option whose dependency is
missing fails configuration. A plain-CMake tree is validated with
`ost validate --build-dir <dir>`, which does not claim `ost` built it. Which
directory owns which part of the build is
[PROJECT_LAYOUT.md §7](../architecture/PROJECT_LAYOUT.md#7-build-structure).

## CI contracts

Inspect and validate the CI cells from the repository root:

```sh
ost ci validate
ost ci plan
ost ci matrix --cell hydra-windows --json
```

The source workflow runs on pull requests, main pushes and manual dispatch.
Its `changes` job skips the core and Hydra cells for a pull request that
changes only `docs/` and top-level Markdown; every other event builds both.
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
