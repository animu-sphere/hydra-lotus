# Changelog

All notable changes to `hydra-lotus` are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). Each released
version will have a record in [docs/releases/](docs/releases/README.md).

## [Unreleased]

### Fixed

- Allocate frame-product, temporal and reservoir readback buffers from
  host-cached memory where the device offers it, so reading back a frame
  no longer performs uncached bus reads; a blocking 128x128 headless frame
  read back in about 1.1 ms before and 0.01 ms after
  ([contract](docs/reference/SCENE.md#primary-rays),
  [evidence](docs/reports/2026-10-25-frame-host-split.md)).

- Preserve shared-baseline attempt records and logs on process launch
  failure or timeout, including partial output and reports; reject duplicate
  workload selections before they can overwrite evidence
  ([contract](docs/reference/SCENE.md#fixed-benchmarks)).

### Changed

- Host readback mirrors of the scene targets, temporal surfaces, validity,
  radiance and reservoirs are allocated by the first frame that reads back
  (or restores preserved contents) instead of with their device buffers, so
  frames that keep their products on the GPU, as the viewport and Hydra's
  presentation do, hold no mirrors: SVGF with both reuse passes uses
  1.71 GiB less host-visible memory at 1080p and 6.83 GiB at 4K, with
  identical products when read back. The temporal and reservoir byte fields
  count mirrors only once allocated, and `--residency-read-back` measures
  the read-back case
  ([contract](docs/reference/SCENE.md#renderer-residency),
  [evidence](docs/reports/2026-10-26-lazy-readback-mirrors.md)).
- Rays find the analytic lights they cross through a binary hierarchy over
  the sphere, rect and disk lights' bounds, built with the light table,
  instead of testing every light: a 256-light `many-lights-v1` frame's
  scene pass is 5.3–8.0x faster under both integrators, with images equal
  to rounding and no measured low-light-count regression.
  `PathTracingSettings::light_crossing` and `--benchmark-light-crossing`
  keep the linear loop as a baseline, and
  `GpuSceneStats::light_hierarchy_bytes` reports the hierarchy's size
  ([contract](docs/reference/SCENE.md#path-tracing),
  [evidence](docs/reports/2026-10-25-light-crossing-hierarchy.md)).
- Compatible geometry replacements that keep their buffer layout stage and
  copy only the sections whose contents changed, so deformation no longer
  restages unchanged triangle indices
  ([contract](docs/reference/SCENE.md#gpu-scene),
  [evidence](docs/reports/2026-10-25-changed-section-staging.md)).
- Cornell quality summaries use `lotus-quality-curve-v2` with selectable
  disjoint sample sequences, separate quality populations and timing
  repetitions, and deterministic image checks within each sequence
  ([contract](docs/reference/SCENE.md#fixed-benchmarks)).
- Group adapter sources by responsibility: shared fixtures in
  `adapters/common/`, headless campaigns in `benchmarks/`, and viewport
  application, camera, platform, scene and UI code in dedicated directories
  ([layout](docs/architecture/PROJECT_LAYOUT.md#adapter-source-organization)).

### Added

- `OffscreenRenderer::Residency()` lists every allocation the renderer makes
  by owner and memory kind, with `VK_EXT_memory_budget` process heap usage
  when the device exposes it; `lotus-headless --benchmark-residency` samples
  it in each progressive, temporal, SVGF and ReSTIR mode. At 4K, SVGF with
  both reuse passes holds 7.3 GiB of device and 6.8 GiB of host memory
  ([contract](docs/reference/SCENE.md#renderer-residency),
  [evidence](docs/reports/2026-10-26-frame-residency.md)).
- `--benchmark-light-grid` replaces `many-lights-v1`'s lights by a smaller
  grid at the same footprint and total power, a light-count control that
  attributed 81–88% of a 256-light frame to analytic-light crossing tests
  ([contract](docs/reference/SCENE.md#fixed-benchmarks),
  [evidence](docs/reports/2026-10-25-light-crossing-cost.md)).
- `GpuFrameEvidence::host_timings` and the benchmark's `render_host_ms`
  split a blocking frame's host wall time into preparation, recording,
  submission, fence wait, readback and results; CPU render-submit time is
  no longer reported as unavailable
  ([contract](docs/reference/SCENE.md#fixed-benchmarks)).
- Headless camera, rigid-transform and deformation motion for the shared
  benchmark workloads (`--benchmark-motion`, runner `--motion`), recording
  per-frame authoring, extraction, blocking scene-update wall time,
  upload/BLAS/TLAS timestamps and staged bytes, and checking the expected
  refits in every frame
  ([contract](docs/reference/SCENE.md#fixed-benchmarks)).
- Static-workload quality curves against validated finite Reference PT
  baseline images, preserving reference evidence and rejecting overlapping
  sample ranges or mismatched workload/settings
  ([contract](docs/reference/SCENE.md#fixed-benchmarks)).
- Independent-sequence Cornell MIS quality curves with linear-RGB error,
  observed error-threshold rendering times and retained raw evidence;
  configurable headless benchmark sample indices with variance-range checks
  ([contract](docs/reference/SCENE.md#fixed-benchmarks)).
- Optional read-only NVIDIA clock, power-state, utilization and whole-device
  memory observations for shared benchmark attempts, with raw samples and
  explicit unavailable/partial diagnostics
  ([scope and limitations](docs/reference/SCENE.md#fixed-benchmarks)).
- Shared versioned headless/viewport benchmark fixtures, including a
  256-sphere-light workload; a static workload manifest and sequential
  baseline runner; viewport warmup exclusion and raw GPU/CPU mean/p95
  measurements ([contract](docs/reference/SCENE.md#fixed-benchmarks)).

## [0.6.0] - 2026-10-18

The interactive rendering release completes the v0.6.0 plan: Renderer
Phase 6's SVGF-class denoiser estimates illumination moments and variance and
filters the accumulated radiance with an edge-aware à-trous filter under both
integrators, compared with a high-spp reference and checked against a CPU
oracle at every pixel; the standalone viewport switches it at run time,
presents its inputs beside the filtered image and times it; Hydra hosts reach
it through `lotus:filterIterations`. Tests carry tiers, and Renderer Phase
5's campaign variants move to a research tier. See the
[release record](docs/releases/v0.6.0.md).

### Added

- A [test policy](docs/design/TEST_POLICY.md): every CTest carries one tier
  label — `fast` (no Vulkan device), `gpu` (renders when a device is present)
  or `research` (a closed campaign's procedures) — given with
  `lotus_test_tier`, and `lotus-test-tiers` fails a test with none or several
  ([guide](docs/guides/BUILDING.md#test-tiers)).
- The `research` build intent (`LOTUS_RESEARCH_TESTS=ON`) registers the
  research tier.
- Temporal radiance keeps the first and second moments of each batch's
  illumination luminance (the colour demodulated by the surface albedo), and
  a variance pass estimates each pixel's luminance variance: temporally from
  four batches of history, with SVGF's edge-aware 7x7 spatial estimate
  before ([reference](docs/reference/TEMPORAL.md#luminance-moments-and-variance)).
  `TemporalRadiancePixel`'s reserved words became `moments` and `variance`;
  `TemporalProducts` adds the raw radiance `batch` and `variance_gpu_ms`;
  `TemporalLuminance` and `EstimateTemporalVariance` are the CPU oracle.
- `TemporalPixel` records the primary surface's base colour and shading
  normal in two reserved words, `albedo` (unorm8) and `shading_normal`
  (octahedral snorm16), with `UnpackTemporalAlbedo`, `UnpackTemporalNormal`,
  `DemodulateTemporalColor` and `RemodulateTemporalColor`.
- `TemporalFrameSettings::filter_iterations` runs SVGF's edge-aware à-trous
  filter over the accumulated radiance, guided by depth, shading normals,
  stable identity and illumination variance, into the colour attachment;
  the history stays unfiltered
  ([reference](docs/reference/TEMPORAL.md#edge-aware-filtering)).
  `TemporalProducts` adds `filter_iterations`, `filter_gpu_ms` and
  `filter_bytes`; the temporal timestamp pool has ten queries;
  `FilterTemporalRadiance` is the CPU oracle.
- `lotus-headless --benchmark-svgf` compares raw, temporal and filtered
  radiance with a high-spp reference
  ([reference](docs/reference/TEMPORAL.md#headless-svgf-comparison),
  [evidence](docs/reports/2026-10-18-svgf-filter.md)); the `research` tier
  runs it under both integrators.
- `lotus-viewport` runs the denoiser: `--denoise`, `--filter-iterations`,
  F2 and the developer UI switch it and its iteration count at run time
  without resetting the history; the `raw`, `accumulated`, `history-length`
  and `variance` views show its inputs, `--split` (or **Split**) shows a view
  beside the filtered image, and the timing HUD and `--benchmark` report the
  variance and filter passes and the filter's memory
  ([usage](docs/guides/BUILDING.md#the-standalone-viewport),
  [evidence](docs/reports/2026-10-18-svgf-viewport.md)).
- `DebugView` adds `RawRadiance`, `AccumulatedRadiance`, `HistoryLength` and
  `Variance`, presented from the latest radiance frame's GPU buffers, and
  `PresentView::split` presents a view beside the frame's colour.
- The Hydra render setting `lotus:filterIterations` filters `lotus:temporal`'s
  colour ([reference](docs/reference/TEMPORAL.md#hydra-controls-and-debug-aovs)).

### Changed

- The à-trous filter reads compact per-pixel guides (identity, depth and
  shading normal) and its first input, which the variance pass writes on a
  filtered frame, instead of the surface and radiance records: 2.4 ms for
  five iterations at 1280x720 (was 11.6 ms), with the same output. Its
  buffer is 48 bytes per pixel (was 32).
- The viewport's GPU frame time counts the variance pass, which it left out
  since the pass was added.
- Renderer Phase 5's reservoir benchmark workload and proposal variants and
  procedure controls (18 CTests) moved to the research tier; the default
  suite keeps the reservoir arithmetic, the GPU reservoir lifecycle, the
  distant reservoir benchmark under both integrators and the Hydra reservoir
  checks.
- Renderer CI skips the core and Hydra cells for a pull request that changes
  only documentation; main pushes, dispatches and release calls always build.
- Temporal radiance memory is 160 bytes per pixel (was 144), the raw batch's
  host mirror added; the temporal timestamp pool has eight queries.

## [0.5.2] - 2026-10-18

The viewport foundation release makes `lotus-viewport` the renderer's
everyday frontend: it presents progressive path tracing from the backend
without a readback, opens USD stages through the Hydra adapter (from the
command line or a file dialog), navigates them with usdview's camera, changes
the integrator, NEE/MIS, ReSTIR and temporal reuse while running, presents
the GPU debug products, and shows per-pass GPU timings and renderer memory.
The build now follows component ownership. See the
[release record](docs/releases/v0.5.2.md).

### Added

- `lotus-viewport` presents the renderer's progressive path tracing from the
  backend, without a CPU readback, in place of the bootstrap triangle: the
  reference Cornell box by default, or a USD stage (`--usd`) through the Hydra
  adapter driven in process in a build with the new `viewport-usd` intent.
  Resize, fullscreen (F11), scripted runs with validation, a resize test,
  explained skips and benchmark JSON; a Windows launcher puts the OpenUSD
  runtime on `PATH` for the process
  ([guide](docs/guides/BUILDING.md#the-standalone-viewport),
  [evidence](docs/reports/2026-10-17-viewport-presentation.md)).
- `lotus-viewport` navigates with usdview's free camera (tumble, track,
  dolly, frame), looks through a stage's authored cameras (`--camera`, C) and
  takes over their view when navigated, opens and steps a stage time code
  (`--time`, Left/Right), reloads layers (R), and applies the stage's pending
  edits every frame; camera and scene changes restart the accumulation. The
  scripted `--interaction-test` checks each restart
  ([guide](docs/guides/BUILDING.md#the-standalone-viewport),
  [evidence](docs/reports/2026-10-17-viewport-camera.md)).
- `CreateOffscreenRenderer` accepts `PresentOptions`, and
  `OffscreenRenderer::Present` copies the latest scene frame into the window's
  sRGB swapchain; `OffscreenTarget::read_back` skips the colour and depth
  readback. `HdLotusRenderDelegate::SetPresentation` presents a render pass
  whose colour binding has no render buffer.
- `ShadersIn` names the backend's shader files in a directory, so hosts pass
  only the `shaders/` directory beside them.
- `lotus-viewport` changes the renderer while it runs, from a Dear ImGui
  developer UI (F1) or the command line: the reference or wavefront
  integrator, the sample limit and accumulation, NEE/MIS, ReSTIR off, initial,
  temporal, spatial or combined, and temporal radiance reuse. It presents
  beauty, depth, normal, motion vectors, temporal validity and reservoir
  validity, age and weight from the existing GPU debug products, without
  reading them back. `--interaction-test` steps through every control and
  view ([guide](docs/guides/BUILDING.md#the-standalone-viewport),
  [evidence](docs/reports/2026-10-18-viewport-controls.md)).
- `OffscreenRenderer::Present` takes a `PresentView`: a `DebugView` of the
  latest frame's temporal or reservoir products, mapped to display colours
  by a compute kernel (`present_view.comp.spv`, in `ShadersIn`).
  `PresentOptions::overlay` lets a host draw over each presented frame in a
  backend-owned render pass. `HdLotusRenderDelegate::SetPresentView` selects
  the view a presenting render pass shows.
- `lotus-viewport` shows a timing HUD: the host frame time and GPU time with
  graphs, each GPU pass's time (path trace with its wavefront kernels, ReSTIR
  initial, temporal and spatial, temporal surfaces and accumulation), the
  sample count, the latest scene update's upload, BLAS and TLAS times, and
  reservoir, history and scene memory. `--benchmark` adds each pass's mean,
  the scene updates and the memory; `lotus-viewport-timings` covers the
  figures without a GPU
  ([guide](docs/guides/BUILDING.md#the-standalone-viewport),
  [evidence](docs/reports/2026-10-18-viewport-timing.md)).
- The Hydra-enabled `lotus-viewport` opens a USD stage from a native file
  dialog (Open USD..., nativefiledialog-extended 1.3.0 as in hydra-merlin)
  and renders it in the same window, keeping the renderer controls.
- `HdLotusRenderDelegate::GetGpuSceneTimings` returns the latest render
  pass's GPU scene update timings.

### Changed

- The build follows component ownership
  ([PROJECT_LAYOUT.md §7](docs/architecture/PROJECT_LAYOUT.md#7-build-structure)):
  `backend/vulkan/shaders/` owns the shader graph behind one `lotus-shaders`
  target, which every host stages and installs through `cmake/LotusShaders.cmake`
  instead of listing SPIR-V files; the Hydra CTests moved to
  `adapters/hydra2/tests/`; compiler warnings and the C++ standard are target
  properties (`Lotus::Warnings`) instead of global flags; options, install and
  package export live in `cmake/`. The installed tree is unchanged.
- `LOTUS_ENABLE_VULKAN=ON` (the default) now fails configuration when the
  Vulkan 1.3 SDK or `slangc` is missing, instead of building without the
  backend. Set it `OFF` for a CPU-only build, as the `ci-core` intent does.
- `lotus-temporal-gpu-test` and `lotus-restir-gpu-test` take the shader
  directory instead of five shader paths.
- `OffscreenTarget::read_back` false now also leaves the temporal and
  reservoir records on the GPU and skips the host check of the reservoirs'
  arithmetic flags, so reuse frames that are presented no longer pay for the
  evidence path; availability, extents, byte counts and GPU times are still
  reported.
- A progressive frame after a fresh batch (temporal radiance or reservoir
  reuse) restarts the accumulation, instead of continuing from that sample
  when its sample sequence happened to match.

### Fixed

- GPU reservoirs could leave their first positive candidate unselected when
  the random number was within about 1e-7 of 1, which the evidence path then
  reported as invalid reservoir arithmetic (a 1280x720 Cornell box at frame
  121). The first positive weight is now selected unconditionally, as the
  CPU oracle does ([evidence](docs/reports/2026-10-18-viewport-controls.md#findings-fixed-on-the-way)).
- `lotus-viewport`'s GPU time (title, benchmark JSON) added the wavefront
  kernels to the scene pass time that already contains them.

### Removed

- `PresentSession` and `CreatePresentSession`, the bootstrap triangle's
  separate presentation device; presentation belongs to the renderer.

## [0.5.1] - 2026-10-17

The Renderer Phase 5 closure release records the ReSTIR DI exit review.
Progressive NEE/MIS stays the default renderer path; initial-only
reservoirs with eight candidates reach equal quality about 1.4 times faster
only with many distant lights, and temporal, spatial and combined reuse are
not worth using as a renderer path. No renderer or shader behaviour changes
from 0.5.0; the additions are measurement harness, tests and evidence. See the
[release record](docs/releases/v0.5.1.md) and the
[exit review](docs/reports/2026-10-17-renderer-phase5-exit.md).

### Changed

- Renderer Phase 5 is closed on its exit criteria: per-workload equal-quality
  results, known bad cases, correlation cost, unsupported conditions and the
  default path are recorded, the reservoir reference says when to enable
  reservoirs, and ReSTIR DI gains no further fixtures
  ([guidance](docs/reference/RESTIR.md#when-to-use-reservoirs),
  [exit review](docs/reports/2026-10-17-renderer-phase5-exit.md)).

### Added

- Headless ReSTIR comparison controls for scattering depth and the finite-reference
  seed, preserving one-bounce/default-seed results and recording both settings in
  the procedure identity ([contract](docs/reference/RESTIR.md#headless-reservoir-comparison),
  [four-bounce/reference-sensitivity evidence](docs/reports/2026-10-16-restir-convergence.md)).
- Headless ReSTIR comparison static cases report an across-sequence ensemble
  decomposition (variance inflation, effective frames, noise-normalized and excess
  MSE), validated by the budget reducer; it attributes the static temporal-reuse
  penalty to correlation rather than detectable bias
  ([contract](docs/reference/RESTIR.md#headless-reservoir-comparison),
  [evidence](docs/reports/2026-10-16-restir-sequence-ensemble.md)).
- Headless ReSTIR comparison cases report replication bias tests: noise-normalized
  error per moving pose (each pose's frames across independent sequences) or
  static sequence mean, within fixed bands, and per 8x8 tile mean, including each
  pose's finite-reference noise; the budget reducer validates them. Four moving
  suites show no reuse bias, ghosting or localized error distinguishable from
  non-reusing estimators
  ([contract](docs/reference/RESTIR.md#headless-reservoir-comparison),
  [evidence](docs/reports/2026-10-17-restir-moving-bias.md)).
- Headless ReSTIR comparison `interreflection` workload: a closed high-albedo room
  where indirect light is about half of each pixel at four bounces; reuse shows no
  bias, ghosting or localized error distinguishable from non-reusing estimators
  ([contract](docs/reference/RESTIR.md#headless-reservoir-comparison),
  [evidence](docs/reports/2026-10-17-restir-interreflection.md)).
- Headless ReSTIR comparison `animation` workload: stepped light placement, power
  and material edits that must reset reservoir history exactly at each edit, with
  an across-sequence step-lag residual correlation. No reset-related ghosting is
  detectable; each reset replays the fresh-batch random numbers, correlating reuse
  noise across edits
  ([contract](docs/reference/RESTIR.md#headless-reservoir-comparison),
  [evidence](docs/reports/2026-10-17-restir-animation.md)).
- ReSTIR reset-contract tests: emissive emission, toggle, visibility and placement
  edits in `lotus-renderer-restir-gpu`, and USD light, material, emission, placement
  and visibility edits in `lotus-renderer-hydra-restir-gpu`, each checking reset
  reasons, that no stale reservoir or radiance sample survives a reset, and
  byte-identical integrators ([evidence](docs/reports/2026-10-17-restir-reset-contract.md)).
- ReSTIR reuse-cost decision at locked GPU clocks: NEE/MIS stays the default
  on four workload suites under either integrator; initial-only reservoirs with
  eight candidates reach equal quality about 1.4 times faster only with many
  distant lights, and temporal, spatial and combined reuse are not worth using.
  Timing evidence now requires locked clocks ([contract](docs/reference/RESTIR.md#campaigns),
  [evidence](docs/reports/2026-10-17-restir-reuse-cost.md)).
- `scripts/restir_campaign.py` runs a JSON plan of headless ReSTIR invocations
  with resumable, command-checked reports, a manifest and optional concurrency
  (labelled, and excluded from timing evidence)
  ([contract](docs/reference/RESTIR.md#campaigns)).

### Changed

- ReSTIR CTest checks no longer run serially; they assert no timing, and
  `ost test --jobs 8` reduces the managed core suite from 859 to 228 seconds on
  the reference host ([building](docs/guides/BUILDING.md#the-default-build--no-openusd)).

## [0.5.0] - 2026-10-16

The experimental ReSTIR DI release adds opt-in initial, temporal, spatial
and combined direct-light reservoirs under both integrators, with Hydra
controls, diagnostic AOVs and reproducible quality/cost comparisons.
Progressive NEE/MIS remains the default. Reuse can increase static error
and GPU cost; broader bias, artifact and convergence assessment remains
open. See the [release record](docs/releases/v0.5.0.md) for compatibility,
validation and known limitations, and the
[canonical roadmap](docs/roadmap/README.md#status-at-a-glance) for ongoing scope.

### Changed

- ReSTIR primary direct lighting retains BSDF-found emission with complementary
  power-table weights, reducing narrow-glossy error while keeping reservoir
  targets and diagnostic RGB unpartitioned
  ([contract](docs/reference/RESTIR.md#emission-accounting),
  [evidence](docs/reports/2026-10-16-restir-bsdf-partition.md)).
- ReSTIR support correction uses stored source targets for exact light-slot/UV
  matches, and final visibility/shading share one reconstructed light sample
  ([contract](docs/reference/RESTIR.md#temporal-gpu-reuse),
  [evidence](docs/reports/2026-10-16-restir-evaluation.md)).
- ReSTIR temporal/spatial passes reuse current receiver transmittance when the
  selected light and both sample coordinates are unchanged, avoiding a repeated
  shadow query while retaining current-frame visibility and support correction
  ([contract](docs/reference/RESTIR.md#temporal-gpu-reuse),
  [evidence](docs/reports/2026-10-15-restir-visibility.md)).

### Added

- Opt-in ReSTIR `glossy-spheres` comparisons with rough dielectric/metal
  receivers, rect/sphere lights, camera disocclusion and three regional error
  bands under both integrators
  ([contract](docs/reference/RESTIR.md#headless-reservoir-comparison),
  [evidence](docs/reports/2026-10-16-restir-glossy-spheres.md)).
- Opt-in narrow-glossy ReSTIR workloads with small area emitters, camera-only
  parallax/disocclusion and separate central/right screen-band errors under
  both integrators ([contract](docs/reference/RESTIR.md#headless-reservoir-comparison),
  [evidence](docs/reports/2026-10-16-restir-glossy.md)).
- ReSTIR comparison per-sequence residual means, centered RMS and lag-one
  autocorrelation, with undefined zero-variance correlations preserved and
  repeated-report consistency checks
  ([contract](docs/reference/RESTIR.md#headless-reservoir-comparison),
  [evidence](docs/reports/2026-10-16-restir-correlation.md)).
- Opt-in mixed area/environment ReSTIR comparison workloads with material,
  depth and normal boundaries, static/moving sequences, explicit finite-spp
  reference labels and regional error populations in repeated quality budgets
  ([contract](docs/reference/RESTIR.md#headless-reservoir-comparison),
  [evidence](docs/reports/2026-10-15-restir-boundaries.md)).
- ReSTIR benchmark seed selection, per-sequence convergence/time populations
  and a repeated-report quality-budget reducer with explicit unattained
  budgets and paired NEE/MIS comparisons
  ([contract](docs/reference/RESTIR.md#repeated-quality-budgets),
  [evidence](docs/reports/2026-10-15-restir-budgets.md)).
- Core ReSTIR candidate proposals for power, uniform and equal-mixture light
  selection, with marginal-density weights and history resets; headless
  `--restir-proposal` comparisons and independent GPU arithmetic/lighting
  checks ([contract](docs/reference/RESTIR.md#initial-gpu-lighting),
  [evidence](docs/reports/2026-10-15-restir-proposals.md)).
- Separate headless ReSTIR benchmark comparing NEE/MIS and all four reservoir
  modes on static and moving many-light scenes, with a candidate-count sweep,
  independent Lambert/NEE/MIS references, convergence curves, GPU time,
  temporal differences, memory and inspectable PFM images
  ([contract](docs/reference/RESTIR.md#headless-reservoir-comparison),
  [evidence](docs/reports/2026-10-14-restir-comparison.md)).
- Hydra ReSTIR controls for initial candidates, temporal/spatial reuse,
  source caps and neighbour radius; six final-reservoir diagnostic AOVs.
  Moving USD-stage and installed usdview checks cover both integrators and
  return to deterministic progressive NEE/MIS
  ([contract](docs/reference/RESTIR.md#hydra-controls-and-diagnostics),
  [evidence](docs/reports/2026-10-14-restir-hydra.md)).
- Opt-in spatial direct-light reservoir reuse under both integrators, with
  immutable input/destination storage, surface rejection, support-corrected
  weights, current visibility, capped neighbour counts and final history
  retention. Reservoir/source diagnostics, separate GPU timing, memory
  accounting and CPU/GPU, boundary, lifecycle, correlation and moving-camera
  comparisons accompany it ([contract](docs/reference/RESTIR.md#spatial-gpu-reuse),
  [evidence](docs/reports/2026-10-14-restir-spatial.md)).
- GitHub CI and Apache-2.0 license badges in the root README.
- Opt-in GPU temporal direct-light reservoir reuse under both integrators,
  using shared reprojection, receiving-surface reevaluation, support-corrected
  normalization and capped history counts. Separate immutable source/destination
  storage retains resolved weights and exact source shading; debug products,
  GPU timing, memory accounting, conservative light-domain resets and failed-frame
  publication checks accompany it. CPU/GPU and moving NEE/MIS comparisons run
  in `lotus-renderer-restir-gpu`
  ([contract](docs/reference/RESTIR.md#temporal-gpu-reuse),
  [evidence](docs/reports/2026-10-13-restir-temporal.md)).
- CPU differing-target reservoir combination with receiving-surface
  reevaluation, bounded represented counts and support-aware normalization.
  Exhaustive discrete RGB/visibility checks cover differing supports and
  count caps; resolved weights remain explicit for subsequent reuse
  ([contract](docs/reference/RESTIR.md#differing-target-cpu-combination),
  [evidence](docs/reports/2026-10-13-restir-reuse-arithmetic.md)).
- Internal CPU reservoir arithmetic for direct-light sampling: a 16-byte
  record, weighted candidate updates, same-target stream combination and
  normalization, including zero-target draw counts and transactional input/
  overflow rejection. CTest `lotus-renderer-reservoir` checks independent
  discrete RGB integrals, visibility and weighted selection frequencies
  without Vulkan or OpenUSD ([contract](docs/reference/RESTIR.md),
  [evidence](docs/reports/2026-10-13-restir-reservoir.md)).
- Opt-in initial-only GPU reservoir direct lighting at primary surfaces under
  both integrators, with reconstructible emissive/analytic/environment samples,
  selected-sample visibility and BSDF-emission accounting. Separate timestamps,
  reservoir debug products and retained memory accounting accompany the pass.
  CPU/GPU selection checks, independent light integrals, NEE/MIS image comparisons,
  lifecycle and overflow checks run in `lotus-renderer-restir-gpu`
  ([contract](docs/reference/RESTIR.md#initial-gpu-lighting),
  [evidence](docs/reports/2026-10-13-restir-initial.md)).

## [0.4.0] - 2026-10-13

The temporal infrastructure release adds Renderer Phase 4: frame history,
rigid motion, reprojection, shared validity and opt-in bounded radiance
accumulation under both integrators, with Hydra controls and debug AOVs.
Deterministic progressive rendering remains the default. See the
[release record](docs/releases/v0.4.0.md) for validation, compatibility and
known limitations.

### Added

- Community contribution guidelines, code of conduct, security reporting
  policy and GitHub issue and pull-request templates.
- Hydra temporal radiance selection, bounded history controls, compatibility
  keys and one-shot reset counters; motion, history length, shared validity
  and rejection AOVs, with moving USD-stage and installed usdview coverage
  under both integrators. Progressive deterministic rendering remains selectable.

- Opt-in temporal radiance accumulation under both integrators, with fresh
  disjoint sample batches, bounded per-pixel colour/count history, automatic
  scene/lighting/settings resets, camera/rigid-motion reuse, history debug
  products and a separate GPU timestamp. Static convergence matches
  progressive transport; moving sequences and disocclusion are compared
  with independent references
  ([contract](docs/reference/TEMPORAL.md#temporal-radiance-accumulation),
  [evidence](docs/reports/2026-10-12-temporal-radiance.md)).
- GPU backward reprojection and shared history rejection in both integrators,
  with a CPU fetch oracle, per-pixel validity/rejection/fetch-index debug AOV,
  configurable depth/normal/motion thresholds, persistent output/readback
  buffers and a separate GPU timestamp. Moving scenes exercise every
  rejection code and compare each pixel with the CPU oracle
  ([contract](docs/reference/TEMPORAL.md#gpu-reprojection-and-validity),
  [evidence](docs/reports/2026-10-12-temporal-gpu-validity.md)).
- Opt-in `OffscreenRenderer::RenderTemporalScene` for both integrators:
  pixel-centre linear depth/geometric normals, stable placement/material
  identities, backward motion and expected previous depth, retained GPU
  surface buffers and a separate primary-surface timestamp. Frame history
  advances only on successful calls, with display/data-window and resize
  support. Progressive transport is preserved
  ([contract](docs/reference/TEMPORAL.md),
  [evidence](docs/reports/2026-10-12-temporal-primary-surfaces.md)).
- The CPU temporal foundation in `lotus/temporal.hpp`, available through
  `Lotus::Vulkan` even without a Vulkan SDK: immutable current/previous
  snapshots, frame-index and compatibility resets, rigid placement pairing
  by mesh key and ordinal, full-frame backward pixel reprojection with
  expected previous-camera depth, and shared depth/normal/identity/motion
  validity. `lotus-renderer-temporal` validates lifecycle, resource edits,
  independent pinhole projections and rejection boundaries. The render
  paths now optionally consume it through `RenderTemporalScene`; progressive rendering behavior is unchanged
  ([contract](docs/reference/TEMPORAL.md),
  [evidence](docs/reports/2026-10-12-temporal-foundation.md)).

## [0.3.0] - 2026-10-12

The direct lighting release adds Renderer Phase 3: next event estimation
with multiple importance sampling towards emissive triangles, UsdLux
sphere, rect, disk and distant lights and textured dome lights, in both
integrators and through Hydra, on by default. See the
[release record](docs/releases/v0.3.0.md) for scope, validation and
compatibility notes.

### Added

- Renderer Phase 3 next event estimation with multiple importance
  sampling, selected with `PathTracingSettings::light_sampling` (`Mis` by
  default; `Bsdf` keeps brute-force BSDF sampling, the committed
  reference's estimator). The GPU scene builds a light table of emissive
  triangles, chosen by the alias method in proportion to area times
  emission; each scattering event traces a shadow ray to one point on
  them, with partial coverage as transmittance, and both light samples and
  BSDF-found emission are weighed by the power heuristic. Both integrators
  run it and agree bit for bit; BSDF sampling still reproduces the
  committed reference bit for bit. The constant environment is left to
  BSDF sampling, which sampled it better than uniform directions did.
  `GpuSceneStats` gains `emissive_triangles`, `light_bytes` and
  `light_writes`. The new check `renderer.path.light_sampling` compares
  the Cornell box with the committed reference under both integrators and
  open scenes with BSDF sampling's estimate, and records the variance and
  time to equal quality (2.61× shorter in the Cornell box). The benchmark
  gains `--benchmark-light-sampling` (default `bsdf`) and CTest
  `lotus-renderer-benchmark-mis`
  ([evidence](docs/reports/2026-10-11-emissive-nee-mis.md)).
- The `lotus:lightSampling` Hydra render setting, a flag that usdview lists
  as the checked *Light sampling (NEE + MIS)* item of its Hydra Settings
  menu; clearing it selects BSDF sampling. The host evidence log records
  `light_sampling` and `emissive_triangles`. CTest
  `lotus-renderer-hydra-light-sampling` compares converged Hydra images
  under both estimators with the backend's, and the usdview smoke test
  toggles the menu item.

- Renderer Phase 3 analytic lights. `Lotus::Light` in `LotusScene::lights`
  (`RenderWorld::SetLight`, `RemoveLight`, `SceneUpdate::lights`) holds
  UsdLux sphere, rect, disk and distant lights: shape, transform, radiance,
  `normalize` and sizes. The GPU scene adds them to the light table beside
  the emissive triangles, weighed by estimated power; next event
  estimation samples a sphere's cone, a rect or disk by area and a distant
  light's cone, and a ray adds the radiance of the lights it crosses,
  weighed by MIS. Lights only emit: they block nothing and camera rays do
  not see them. A parallel light (angle 0) is added at every scattering
  event under BSDF sampling too. `GpuSceneStats` gains `analytic_lights`.
  The new check `renderer.path.analytic_lights` compares each light, and
  all of them with an emissive square, with an independent irradiance
  oracle under both estimators and integrators
  ([evidence](docs/reports/2026-10-11-analytic-lights.md)).
- `sphereLight`, `rectLight`, `diskLight` and `distantLight` Hydra sprims,
  translated into analytic lights; the host evidence log records
  `analytic_lights`. CTest `lotus-renderer-hydra-analytic-lights` (and its
  GPU-gated `-gpu` variant) checks their translation and edits and
  compares converged images with the backend's.

- Renderer Phase 3 environment importance sampling.
  `LotusScene::environment_map` (`RenderWorld::SetEnvironmentMap`,
  `Lotus::EnvironmentMap`, `Lotus::EnvironmentFrame`,
  `SceneUpdate::environment_map`) holds a latitude-longitude environment
  image in the OpenEXR layout: a texture key, an RGB scale and a frame of
  which only the orientation counts. Its radiance adds to the constant
  environment's on escaping paths. The GPU scene builds its
  piecewise-constant distribution (radiance times sin θ, at most
  2,048 × 1,024 cells) on the host and gives it the light table's last
  entry, weighed like a distant light; next event estimation samples a
  row and a cell continuously with the usual four random numbers, and
  escaping BSDF-sampled directions are weighed by MIS. The constant
  environment stays with BSDF sampling. `GpuSceneStats` gains
  `environment_map`, `environment_sampled`, `environment_bytes` and
  `environment_writes`. The new check `renderer.path.environment` checks
  the OpenEXR layout by mirror reflections and compares a floor under
  OpenUSD's Stinson Beach image with an independent irradiance oracle
  under both estimators and integrators, where light sampling lowers the
  per-sample variance 3,728-fold
  ([evidence](docs/reports/2026-10-11-environment-importance-sampling.md)).
- Textured `domeLight` sprims: `texture:file` (latitude-longitude,
  `texture:format` `automatic` or `latlong`) decoded with Hio becomes the
  environment image, scaled by the dome's radiance and oriented by its
  transform after `domeOffset`, so DomeLight_1's `poleAxis` is honoured.
  One textured dome lights a scene; others warn. CTest
  `lotus-renderer-hydra-dome-texture` (and its GPU-gated `-gpu` variant)
  checks the translation, Hio's decoding against the headless reader and
  the layout's directions through a mirror.
- `validation/assets/`: OpenUSD's `StinsonBeach.hdr` (v26.05,
  `pxr/imaging/hdx/textures/`), its Tomorrow Open Source Technology
  License 1.0 text and a `manifest.json` of source, revision, size and
  SHA-256, checked by CTest `lotus-validation-assets`. `lotus-headless`
  reads them from `assets/` beside it (`--assets` overrides), installs
  them to `bin/assets/`, and releases require them.

- Two lit benchmark workloads, `lights-v1` (the Cornell box open at the
  top under one analytic light of each kind) and `environment-v1` (its
  blocks under OpenUSD's Stinson Beach image), and per-sample variance in
  every workload's measurements: `per_sample_variance` from 16
  accumulations of 16 samples and `variance_gpu_ms`, proportional to the
  time to equal quality. Scenes report their light table. With them the
  Renderer Phase 3 exit review measured light sampling's time to equal
  quality against BSDF sampling under both integrators: 2.6–3.0× shorter
  in the Cornell box and over a thousandfold in the lit workloads. This
  completes Renderer Phase 3
  ([exit review](docs/reports/2026-10-12-renderer-phase3-exit.md)).

### Changed

- Any synced analytic light, like any dome, replaces the Hydra adapter's
  white fallback environment, so a stage lit only by a sphere or the sun
  renders under a black sky.
- `lotus-renderer-hydra-texture-gpu` sets `lotus:lightSampling` to false:
  its exact radiance is BSDF sampling's, sample for sample.
- `GpuSceneStats::light_writes` also counts rewrites after an environment
  image change; a change of the constant environment alone still rewrites
  nothing.

## [0.2.0] - 2026-10-11

The wavefront release adds Renderer Phase 2: a wavefront integrator that
reproduces the reference transport, its measured queue scheduling and
path-state layout, and integrator selection through Hydra. It is also the
first release published through the tag-driven lane with a Windows Hydra
package. See the [release record](docs/releases/v0.2.0.md) for scope,
validation and compatibility notes.

### Added

- Integrator selection through Hydra. The `lotus:wavefront` render
  setting, a flag that usdview lists as the checkable *Wavefront
  integrator* item of its Hydra Settings menu, selects the wavefront
  integrator instead of the reference integrator (the default); a change
  restarts the accumulation. The plugin installs `wavefront.comp.spv`
  with its shaders, and releases require it. The host evidence log
  records each pass's `integrator` and `wavefront_rounds`, and
  `HdLotusRenderDelegate::GetFrameEvidence` returns the latest pass's
  frame evidence.
  `lotus-renderer-hydra-integrator` compares converged Hydra images under
  both integrators with the backend's, and the usdview smoke test toggles
  the installed plugin's menu item on and off. This completes Renderer
  Phase 2.

- Renderer Phase 2 wavefront path tracing, selected with
  `PathTracingSettings::integrator`: compute kernels pass persistent path
  state between ray, hit and terminated-path queues with indirect
  dispatches, and share the reference transport through new `common/scene`
  and `common/transport` Slang modules. It reproduces the committed
  reference and the fixed benchmarks' images bit for bit on the measured
  device; `renderer.path.wavefront` runs the transport scenarios and the
  reference comparison with it. Frames report per-bounce queue occupancy
  and per-kernel GPU timings, and `--benchmark-integrator wavefront`
  measures it. Queues and scheduling are not yet optimized.

- Tag-driven GitHub Actions releases, reusing renderer CI and the pinned
  Windows Hydra runtime. The lane checks version/changelog agreement,
  reproducible packaging and required products, assembles manifest/SBOM,
  source and checksums, and publishes as `hydra-lotus vX.Y.Z`. Manual
  dispatch builds a dry-run bundle without publishing.

### Changed

- The wavefront integrator's camera kernel traces the camera rays it
  starts, and the resolve pass adds each ended path's sample from its slot,
  replacing the generate and accumulate kernels and the terminated-path
  queue. The fixed benchmarks' wavefront GPU time fell 1.08–1.14 times,
  and 1.18 times for `cornell-v1` at 1024×1024, with identical images.
  `WavefrontTimings::camera_gpu_ms` replaces `generate_gpu_ms`,
  `accumulate_gpu_ms` is removed, and the benchmark report's wavefront
  keys follow.

- Wavefront path state is stored in five 16-byte sections per slot rather
  than a 112-byte record, and each kernel reads and writes only the
  sections it uses. The fixed benchmarks' wavefront GPU time fell
  1.19–1.30 times, and 2.0 times for `cornell-v1` at 1024×1024, with
  identical images. `renderer.path.wavefront` also compares both
  integrators' means where a coverage draw precedes scattering.

- Wavefront queue appends maintain each queue's indirect dispatch, so the
  bookkeeping kernels and half of each round's barriers are gone. A call
  records rounds only for the bounces its previous sample's queues held at
  least 1/16 of the camera rays, and a new tail kernel follows the
  remaining paths to their ends. The fixed benchmarks' wavefront GPU time
  fell 4.0–7.2 times with identical images. Frames report the rounds run
  (`GpuFrameEvidence::wavefront_rounds`) and the tail's GPU duration, and
  `--benchmark-size` measures other square targets.

- The project version is declared in a top-level `VERSION` file, which
  CMake's `project(Lotus VERSION)` reads; `scripts/release.py` checks that
  `openstrata.toml` declares the same version.

## [0.1.0] - 2026-10-10

The first foundation release includes the deterministic reference path tracer,
minimal material inputs, Hydra integration and a measured Vulkan backend.
See the [release record](docs/releases/v0.1.0.md) for scope, validation and
compatibility notes.

### Added

- A separate headless benchmark mode for fixed Cornell, textured, highly
  instanced and geometry-heavy workloads. Timing JSON retains raw samples,
  build/device identity, GPU and blocking wall times, samples/s, scene-pool
  bytes, build/refit timings and unchanged-scene CPU cost, with explicit
  unavailable metrics. Three repeated runs establish the baseline; CTest
  checks report/workload/CLI contracts and missing-driver handling.

- BLAS point refits for compatible geometry replacements, with dependent
  TLAS bounds refits and GPU buffer reuse. Attribute-only changes leave
  acceleration structures unchanged; vertex-count or index changes rebuild.
  Build/update counters, CPU/GPU update measurements and an isolated BLAS
  compaction size/copy experiment document the backend trade-offs.

- Backend-owned GPU scene memory pools for geometry, textures, material and
  instance buffers, acceleration storage, scratch, staging and persistent
  scene readback. Released ranges coalesce and reuse their backing memory;
  memory statistics expose allocation counts, retained bytes and free spans.
  Dedicated-allocation fallback, pool teardown, range placement and churn
  have CPU/GPU regressions and explained missing-driver capability handling.

- No-clear Hydra AOV restoration from the currently bound CPU colour/depth
  buffers, including buffer switches, host writes and same-size reallocation.
  Offscreen callers can supply preserved contents; persistent readback buffers
  also stage the restoration, without additional GPU allocations.

- Repository-owned documentation validation in `scripts/check_docs.py`,
  with regression fixtures and a runtime-independent CI job. Category indexes
  and ownership rules centralize phase/release status, incomplete tasks,
  technical limitations and measured evidence.

- Constant Hydra dome lighting from `DomeLight` colour, intensity, exposure
  and inherited visibility. Multiple domes add into the existing environment;
  deleting the last dome restores the white fallback. CPU and GPU tests
  check live USD edits, exact mirror radiance, accumulation restarts and
  unchanged geometry upload and acceleration build counters.

- GGX dielectric reflection over Lambert (Renderer Phase 1.5), from constant
  `ior` or the specular workflow's constant/RGB-textured `specularColor`.
  Fresnel attenuates the diffuse base on entry and exit; view-dependent lobe
  selection and its matching PDF sample the full BSDF. Index matching
  (`ior = 1`) recovers uncoated Lambert. Core validation, GPU readback,
  independent furnace integrals and Hydra edits cover the new inputs.
  The Cornell mean and variance references are regenerated for this model.

- Opacity coverage and alpha masks (Renderer Phase 1.5).
  `UsdPreviewSurface.opacity` translates constants or scalar texture lookups,
  including alpha, and `opacityThreshold` translates constants. Positive
  thresholds select cut-outs; zero selects stochastic presence coverage.
  Non-opaque ray-query candidates test coverage for primary and secondary
  rays without consuming bounces on rejected surfaces. Depth uses a fixed
  coverage realization. `renderer.path.opacity` and the Hydra texture CPU
  and GPU tests cover layer mixtures, background alpha, masks and edits.
  Glass and `opacityMode` semantics are not implemented.

- Tangent-space normal inputs and normal maps (Renderer Phase 1.5).
  `UsdPreviewSurface.normal` translates signed constants or RGB texture
  lookups, with authored scale, bias and colour space. The path tracer
  builds per-hit UV frames about the mesh shading normal, preserving
  mirrored UVs and transforms, with a stable frame for absent or degenerate
  UVs. Headless normal diagnostics and exact mirror radiance, plus Hydra
  image, UV and connection edits, cover the implementation.

- Constant `displayColor` fallback for Hydra meshes without a material
  binding, including indexed colours and live colour edits. Authored
  materials take priority; missing or unusable colours keep the default
  grey. Private fallback materials are released on binding, colour removal
  and mesh removal. The Hydra material CPU and GPU tests cover these
  transitions and rendered radiance.

- Textures (Renderer Phase 1.5). A material input can be a texture lookup
  (`Lotus::TextureInput`: a texture key, the channel a scalar reads, wrap
  modes, scale, bias and fallback) that reads the mesh's texture-coordinate
  set the material names (`MeshGeometry::texcoords`, one (s, t) per
  triangle corner). `LotusScene::textures` keys decoded `Lotus::Texture`s:
  8-bit linear, 8-bit sRGB or float. The update plan uploads and releases
  textures and resolves each lookup's texture (`SceneMaterial`) and each
  instance's set. The GPU scene keeps one image per texture in a bindless
  table of 1,024 that the path tracer samples bilinearly, and reads them
  back. `SceneOutput::Albedo` and `SceneOutput::RoughnessMetallic` render
  the inputs after the lookups. The Hydra adapter translates `UsdUVTexture`
  with `UsdPrimvarReader_float2`, decodes images with Hio and reads every
  float-pair primvar as a texture-coordinate set. `renderer.path.textures`,
  `renderer.scene.upload` and `lotus-renderer-hydra-texture` (and `-gpu`)
  check them. The scene passes now also require
  `descriptorBindingPartiallyBound` and
  `shaderSampledImageArrayNonUniformIndexing`.

- Authored normals as shading normals (Renderer Phase 1.5).
  `MeshGeometry::normals` holds one object-space normal per triangle
  corner; the GPU scene stores them after the triangles and the instance
  record carries their address. The path tracer interpolates them,
  transforms them by the inverse transpose and samples the BSDF around
  them, falling back to the geometric normal where they vanish or face
  away from the ray, and ends a path whose sampled direction lies below
  the geometric surface. `SceneOutput::ShadingNormal` renders the shading
  normal as a diagnostic. The Hydra adapter reads the `normals` primvar in
  every interpolation, indexed ones included. `renderer.path.normals` and
  `lotus-renderer-hydra-normals` check them. The scene passes now require
  `shaderInt64`.

- The Lotus material IR (Renderer Phase 1.5). `Lotus::Material`
  (`include/lotus/material.hpp`) holds constant base colour, roughness,
  metallic and emission. `LotusScene::materials` keys them; meshes bind to
  a key with `RenderWorld::BindMaterial`, and an unbound mesh, or one bound
  to a missing key, uses the default. The update plan carries a material
  table (`SceneUpdate::materials`, the default at slot 0) and each
  instance's slot, so a material's value edit rewrites only the table; the
  GPU scene keeps it in a device buffer that the path tracer reads, reports
  it in `GpuSceneStats` and reads it back in `GpuSceneContents::materials`.
  The Hydra adapter supports the `material` sprim: it translates
  `UsdPreviewSurface`'s constant `diffuseColor`, `roughness`, `metallic`
  and `emissiveColor` and follows mesh material bindings.
  `lotus-renderer-hydra-material` checks the translator and a UsdImaging
  stage through edits, rebinding and removal, and its `-gpu` variant exact
  Lambert radiance in a Hydra image.

- Per-pass GPU timestamps for scene updates. Each `UpdateScene`
  submission times its copies, BLAS builds and TLAS build or refit
  separately: `GpuSceneEvidence::timings`, with
  `GpuSceneStats::timestamps_available` saying whether the queue supports
  timestamps. The headless runner's `renderer.scene.timestamp` times a
  131,072-triangle grid with 1,024 instancer placements through insertion,
  a TLAS refit, a material edit, an unchanged commit and removal, and
  reports the durations.

- Deterministic mode through Hydra. The `lotus:sampleIndex` render setting
  (0 by default) sets the first sample index of the accumulation, which
  seeds its random numbers; changing it restarts the accumulation. With
  `convergedSamplesPerPixel`, the camera and the AOV it fixes the converged
  image. The host evidence log records each pass's `sample_index`.
  `lotus-renderer-hydra-deterministic` compares converged Hydra images bit
  for bit with the backend's own renders across setting changes, camera
  moves and new render delegates, and the usdview smoke test changes the
  setting in the viewer.

- Render-pass selection. A Hydra render pass traces only the meshes under
  its collection's root paths and outside its exclude paths whose render
  tag it was given (any, without tags). Excluded meshes are hidden for the
  pass with their geometry resident, so toggling a purpose in usdview
  rewrites only the instances. `HdLotusRenderDelegate::GetSelectedSnapshot`
  returns the snapshot the latest pass traced.
  `lotus-renderer-hydra-render-pass` syncs a USD stage through UsdImaging
  and checks the traced meshes across render-tag, purpose, visibility and
  collection changes; `lotus-renderer-hydra-render-pass-gpu` also checks
  the GPU scene's instances and that deselection uploads nothing.

- Hydra instancers. `HdLotusInstancer` places an instancer's prototypes
  from its translation, rotation, scale and transform primvars, nested
  instancers included, so point instancers and native instancing render.
  `MeshInstance::instancer_transforms` holds a prototype's placements, and
  the update plan lists one instance per placement over one resident
  geometry and BLAS. `lotus-renderer-hydra-instancer` syncs a USD stage
  through UsdImaging and compares the placements with UsdGeom's;
  `renderer.scene.upload`, `.scene.acceleration` and
  `.ray_query.triangle` cover instanced placements.

- Reference images, the reference path tracer milestone. `lotus-headless`
  renders a fixed Cornell box at 1, 16, 64, 256 and 1024 spp
  (`--images <directory>` writes them as PFM) and compares each image with
  the committed reference in `validation/reference/`: a 1024-spp mean and a
  per-pixel variance, written by deterministic mode
  (`--write-reference <directory>`). The comparison settles DES-Q5. An image
  matches when its mean difference over the whole image and over every tile
  is within 5 standard errors. `renderer.path.reference` runs it, and also
  checks that a brighter red wall is rejected. The installed product
  carries the reference in `bin/reference/`.

- HDR accumulation, the first physically correct image. `RenderScene`'s
  `Radiance` output adds `frame_count` samples per pixel to a persistent
  RGBA32F accumulation through a 1-pixel box filter and writes the
  unclamped mean as an `rgba32-sfloat` colour product; a missed sample counts
  as the clear colour. The accumulation restarts when the scene, camera,
  target framing, `sample_index` or `max_bounces` changes, and
  `PathTracingSettings::max_samples` caps it;
  `GpuFrameEvidence::samples_per_pixel` reports it. The scene pass is a
  camera pass for depth and barycentrics and a radiance pass, specialized
  from one shader module. The Hydra colour AOV defaults to
  `HdFormatFloat32Vec4` (8-bit buffers are still accepted) and converges
  progressively at the `convergedSamplesPerPixel` render setting, 64 by
  default. `renderer.path.accumulation` checks the filter's coverage against
  the projected area in each pixel, HDR output, split frames and the
  restart rules.

- The reference path tracer's transport: surface hit reconstruction from
  the instance records and geometry device addresses, a Lambert and
  GGX-metal BSDF, two-sided emission, a constant environment, multiple
  bounces with Russian roulette, and PCG sampling seeded by pixel and
  `PathTracingSettings::sample_index`. `RenderScene` writes one clamped
  sample per pixel by default and keeps the barycentric diagnostic as an
  output. `LotusScene` gains a per-mesh `SurfaceMaterial` and an environment
  radiance, carried by the update plan; the Hydra adapter uses the default
  material under a white fallback environment. Light-carrying shader values
  are a `Spectrum` type that holds RGB (DES-Q3). `renderer.path.bsdf` and
  `renderer.path.multibounce` check the transport against closed-form
  radiance and independent quadrature.

- Primary camera rays through the GPU scene's TLAS using Vulkan ray queries:
  diagnostic triangle barycentrics and projected depth, persistent pipeline
  and descriptors, perspective/orthographic camera support and GPU timestamps.
  Hydra uses this pass on supported devices. Headless evidence compares it
  against independent CPU projections across scene edits, clipping, framing
  and resize; missing ray-query capability produces explained SKIPs.

- Acceleration structures in the GPU scene: a BLAS per resident geometry
  and a TLAS over the instances, refitted when a rewrite changes only
  transforms and rebuilt otherwise, built in each plan's one submission.
  They are enabled when the device has `VK_KHR_acceleration_structure`; a
  `renderer.scene.acceleration` check compares the read-back TLAS build
  input and the build counts with the scene, and SKIPs with the reason on
  devices without them.

- The scene update plan (`SceneExtraction`, `SceneUpdate`) and the GPU
  scene: incremental geometry uploads and releases and instance rewrites
  into device-local buffers owned by `OffscreenRenderer`, with a
  `renderer.scene.upload` check that compares read-back buffers with the
  CPU scene. The Hydra adapter uploads its extracted meshes on every pass.

- Host-neutral CPU mesh geometry and placement in `LotusScene`, immutable
  frame snapshots, and Hydra coarse-mesh extraction with dirty updates,
  winding/hole handling and removal. CPU and Hydra tests cover snapshot
  lifetime, malformed input and multi-triangle bootstrap AOV compatibility.

- Source CI contracts for runtime-free core checks and digest-pinned Hydra
  builds, registered with OpenStrata as an external renderer workflow.
- A capability gate for the usdview smoke test: explicit GPU SKIPs skip the
  viewer, while failed or invalid evidence remains an error; regression checks
  cover capability verdicts and preserve staging on a skip.

- The Renderer Phase 0 AOV reference: colour/depth formats and CPU ID
  sentinels, with binding and clear-value validation, a Hydra AOV integration
  test and `renderer.aov.clears` GPU evidence.

- The project, generated with `ost init --template renderer --name lotus`
  (OpenStrata 0.23.14, template 0.5.4): the host-neutral core, the Vulkan
  backend, the headless runner, the standalone viewport and the `hdLotus`
  Hydra adapter, all drawing the template's bootstrap triangle.
- A `hydra` build intent in `openstrata.toml` that builds the Hydra adapter.
- Documentation: the design policy and integration scope; the project
  layout; the capability matrix and measured configurations; the roadmap;
  the building guide; and the first `ost` dogfooding report.
- The roadmap policy (`docs/design/ROADMAP_POLICY.md`), which now owns the
  Renderer Phase 0–10 sequence: Renderer Phase 1 is the reference path
  tracer (with a minimal GGX), a Renderer Phase 1.5 brings a minimal material
  IR forward, version numbers follow milestones, and the near-term priority
  is the Hydra-mesh-to-AOV vertical slice. It resolves the design policy's
  DES-Q1 and DES-Q2 and opens DES-Q5 and DES-Q6.

- A camera in the core (`Lotus::Camera`: view and projection, OpenGL
  conventions) that the Hydra adapter fills from the render pass state and
  the backend converts to Vulkan clip space. The bootstrap triangle is now in
  world space, where the usdview smoke scene's mesh is, and the headless
  runner looks at it through a perspective camera.
- `Lotus::OffscreenRenderer` (`CreateOffscreenRenderer`): a Vulkan instance,
  device and pipeline that outlive frames, with targets recreated only when
  their size changes. The Hydra adapter keeps one across frames instead of
  creating a Vulkan instance, device and pipeline for every frame, and its
  frame evidence records `renderer_creations` and `target_creations`, which
  the usdview smoke test checks. `RenderOffscreen` remains as a one-shot
  wrapper.

### Changed

- `SceneUpdate::materials` holds `SceneMaterial`s (a material and the
  textures its lookups read) and `GpuSceneContents::materials`
  `GpuMaterialContents` (a material and its lookups' texture slots). The GPU
  instance record grows from 96 to 112 bytes and the material record from
  32 to 304. The `ShadingNormal` output is one of three surface-pass
  diagnostics. `UsdPreviewSurface` inputs driven by a `UsdUVTexture` are
  translated instead of left at their defaults. The Cornell reference is
  reproduced bit for bit.

- `SurfaceMaterial` and `RenderWorld::SetMeshMaterial` are replaced by the
  material IR: `SetMaterial`, `RemoveMaterial` and `BindMaterial`.
  `SceneInstance::material` and `GpuInstanceContents::material_slot` are
  material table slots, and the GPU instance record shrinks from 128 to 96
  bytes. The headless Cornell box shares one material among its white
  surfaces and still reproduces the committed reference bit for bit.

- Offscreen colour clears match the AOV descriptor's transparent black.
  Hydra colour/depth clear values reach the GPU; clears cover the whole
  target, and empty scenes converge with cleared output. Empty clear values
  preserve the preceding attachments at the same size; clear changes reuse
  targets.

- The viewport supplies a perspective `Lotus::Camera` through `RenderWorld`
  and render extraction, updating its aspect from the framebuffer size.
  CPU tests check projection on square, landscape and portrait extents,
  resize revisions and unchanged-camera stability.
- The core boundary test discovers all public headers recursively at test
  time, including backend headers, and rejects GLFW, SDL and Slang
  dependencies alongside the existing checks. A regression test checks
  forbidden dependencies introduced in newly added nested headers.
- The Hydra adapter renders at the AOV's resolution instead of upscaling a
  64×64 image, honours the framing's display and data windows, and writes
  rows bottom-up as Hydra's render buffers expect. `RenderOffscreen` takes
  an `OffscreenTarget`.
- The usdview smoke test fails on any Vulkan validation message in a Hydra
  frame.
- `adapters/headless/main.cpp` is ASCII-only. One em dash in a comment made
  MSVC print that file's `/showIncludes` notes in a form Ninja did not
  recognize on a Japanese host, so the headless runner recorded no header
  dependencies and a header edit did not rebuild it.
- Adapters relink when their SPIR-V changes, so their shader copies and the
  headless report no longer survive a shader-only edit. A material-only
  instance rewrite no longer refits the TLAS.
