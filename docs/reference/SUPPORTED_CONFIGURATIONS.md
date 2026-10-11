# Supported configurations

Configurations a build and test run has actually passed on. A configuration
that has only been reviewed is not listed. The report named in each row
holds its detail; hosted capability handling is identified separately from
physical-GPU rendering.

## Measured

| OS | Compiler | Build | Runtime | GPU | Result | Report |
| --- | --- | --- | --- | --- | --- | --- |
| Windows 11 x86_64 | MSVC 14.51 | `core` / `hydra`, Release; runtime-free `ci-core` | no OpenUSD / OpenUSD 26.08 lookdev, Python 3.13 / none | NVIDIA RTX A5000; Vulkan disabled in `ci-core` | Fixed benchmark baseline, deterministic images, schema/CLI and capability regression; core 17/17, Hydra 34/34, runtime-free 9/9 and strict GPU evidence passed | [Performance baseline](../reports/2026-10-10-performance-baseline.md) |
| Windows 11 x86_64 | MSVC 14.51 | `core` / `hydra`, Release | `core` without OpenUSD / OpenUSD 26.08 lookdev, Python 3.13 | NVIDIA RTX A5000 | Memory pool/readback/churn, synchronization, deterministic references; core 13/13, Hydra 30/30 and strict evidence passed | [GPU memory pools](../reports/2026-10-10-gpu-memory-pools.md) |
| Windows 11 x86_64 | MSVC 14.51 | runtime-free `ci-core`, Release | none | Vulkan disabled | 8/8 tests; explained scene-memory and synchronization SKIPs | [GPU memory pools](../reports/2026-10-10-gpu-memory-pools.md) |
| GitHub-hosted Windows 2022 x86_64 | MSVC (hosted toolchain) | runtime-free `ci-core` / `hydra`, Release | none / pinned OpenUSD runtime | No usable Vulkan driver in Hydra job | Core 8/8 and Hydra 27/27 entries without failures, GPU/host/synchronization capability SKIPs explained | [Foundation closure](../reports/2026-10-10-foundation-closure.md#hosted-follow-up) |
| Windows 11 x86_64 | MSVC 14.51 | `hydra` intent, Release | OpenUSD 26.08 lookdev, Python 3.13 | NVIDIA RTX A5000 | Synchronization hazard and simulated missing-driver checks, 27/27 tests and strict evidence validation passed | [Synchronization CI follow-up](../reports/2026-10-09-synchronization-ci.md) |
| Windows 11 x86_64 | MSVC 14.51 | `core`, Release | OpenStrata `cy2026` `core` (no OpenUSD) | NVIDIA RTX A5000 | Synchronization validation, intentional hazard capture, 9/9 tests and strict evidence validation passed | [Synchronization validation](../reports/2026-10-08-synchronization-validation.md) |
| Windows 11 x86_64 | MSVC 14.51 | `hydra` intent, Release | OpenUSD 26.08 lookdev, Python 3.13 | NVIDIA RTX A5000 | Synchronization validation on every usdview frame, 26/26 tests and strict evidence validation passed | [Synchronization validation](../reports/2026-10-08-synchronization-validation.md) |
| Windows 11 x86_64 | MSVC 14.51 | runtime-free `ci-core`, Release | none | Vulkan disabled | 8/8 tests; explained synchronization-validation SKIP | [Synchronization validation](../reports/2026-10-08-synchronization-validation.md) |
| Windows 11 x86_64 | MSVC 14.51 | `hydra` intent, Release | OpenUSD 26.08 lookdev, Python 3.13 | NVIDIA RTX A5000 | Constant dome CPU/GPU checks, 25/25 tests and strict evidence validation passed | [Dome lights](../reports/2026-10-08-dome-lights.md) |
| Windows 11 x86_64 | MSVC 14.51 (Visual Studio 18) | `core`, Release | OpenStrata `cy2026` `core` (no OpenUSD) | NVIDIA RTX A5000 | `ost build`, `ost test` 3/3, `ost validate` passed | [ost 01](../reports/ost/01-2026-10-04-v0.23.14-renderer-template-bootstrap.md) |
| Windows 11 x86_64 | MSVC 14.51 (Visual Studio 18) | `hydra` intent, Release | OpenStrata `cy2026` `lookdev`, OpenUSD 26.08, Python 3.13 | NVIDIA RTX A5000 | `ost build`, `ost test` 7/7 including `testusdview`, `ost validate` passed | [ost 01](../reports/ost/01-2026-10-04-v0.23.14-renderer-template-bootstrap.md) |
| Windows 11 x86_64 | MSVC 14.51 (Visual Studio 18) | standalone viewport | OpenStrata `cy2026` `core` | NVIDIA RTX A5000 | `ost renderer viewport -- --frames 8 --hidden` presented 8 frames | [ost 01](../reports/ost/01-2026-10-04-v0.23.14-renderer-template-bootstrap.md) |
| Windows 11 x86_64 | MSVC 14.51 | runtime-free `ci-core`, Release | none | Vulkan disabled | `ost build`, `ost test` 7/7 passed | [CPU mesh extraction](../reports/2026-10-05-cpu-mesh-extraction.md) |
| Windows 11 x86_64 | MSVC 14.51 | `hydra` intent, Release | OpenUSD 26.08 lookdev, Python 3.13 | NVIDIA RTX A5000 | `ost build`, `ost test` 13/13 and strict evidence validation passed | [CPU mesh extraction](../reports/2026-10-05-cpu-mesh-extraction.md) |
| Windows 11 x86_64 | MSVC 14.51 | `core`, Release | OpenStrata `cy2026` `core` | NVIDIA RTX A5000 | Primary-ray CPU comparisons, 8/8 tests and strict evidence validation passed | [Primary rays](../reports/2026-10-05-primary-rays.md) |
| Windows 11 x86_64 | MSVC 14.51 | `hydra` intent, Release | OpenUSD 26.08 lookdev, Python 3.13 | NVIDIA RTX A5000 | Ray-query AOVs, usdview, 14/14 tests and strict evidence validation passed | [Primary rays](../reports/2026-10-05-primary-rays.md) |
| Windows 11 x86_64 | MSVC 14.51 | runtime-free `ci-core`, Release | none | Vulkan disabled | 8/8 tests; explained ray-query SKIPs | [Primary rays](../reports/2026-10-05-primary-rays.md) |
| Windows 11 x86_64 | MSVC 14.51 | `core`, Release | OpenStrata `cy2026` `core` | NVIDIA RTX A5000 | Path-tracing checks, 8/8 tests and strict evidence validation passed | [BSDF and multi-bounce](../reports/2026-10-05-bsdf-multibounce.md) |
| Windows 11 x86_64 | MSVC 14.51 | `hydra` intent, Release | OpenUSD 26.08 lookdev, Python 3.13 | NVIDIA RTX A5000 | Path-traced AOVs, usdview, 14/14 tests and strict evidence validation passed | [BSDF and multi-bounce](../reports/2026-10-05-bsdf-multibounce.md) |
| Windows 11 x86_64 | MSVC 14.51 | runtime-free `ci-core`, Release | none | Vulkan disabled | 8/8 tests; explained ray-query and path-tracing SKIPs | [BSDF and multi-bounce](../reports/2026-10-05-bsdf-multibounce.md) |
| Windows 11 x86_64 | MSVC 14.51 | `core`, Release | OpenStrata `cy2026` `core` | NVIDIA RTX A5000 | Accumulation and path-tracing checks, 8/8 tests and strict evidence validation passed | [HDR accumulation](../reports/2026-10-05-hdr-accumulation.md) |
| Windows 11 x86_64 | MSVC 14.51 | `hydra` intent, Release | OpenUSD 26.08 lookdev, Python 3.13 | NVIDIA RTX A5000 | Float colour AOVs, progressive usdview convergence, 14/14 tests and strict evidence validation passed | [HDR accumulation](../reports/2026-10-05-hdr-accumulation.md) |
| Windows 11 x86_64 | MSVC 14.51 | runtime-free `ci-core`, Release | none | Vulkan disabled | 8/8 tests; explained ray-query and path-tracing SKIPs | [HDR accumulation](../reports/2026-10-05-hdr-accumulation.md) |
| Windows 11 x86_64 | MSVC 14.51 | `core`, Release | OpenStrata `cy2026` `core` | NVIDIA RTX A5000 | Opacity masks and coverage, 8/8 tests and strict evidence validation passed | [Opacity](../reports/2026-10-08-opacity.md) |
| Windows 11 x86_64 | MSVC 14.51 | `hydra` intent, Release | OpenUSD 26.08 lookdev, Python 3.13 | NVIDIA RTX A5000 | Alpha image/threshold edits and stochastic AOV coverage, 23/23 tests and strict evidence validation passed | [Opacity](../reports/2026-10-08-opacity.md) |
| Windows 11 x86_64 | MSVC 14.51 | runtime-free `ci-core`, Release | none | Vulkan disabled | 8/8 tests; explained GPU SKIPs | [Opacity](../reports/2026-10-08-opacity.md) |

## Requirements

| Requirement | Version | Notes |
| --- | --- | --- |
| `ost` | 0.23.14 or newer | the version the project was generated and validated with |
| CMake | 3.24 or newer | measured with 4.4 |
| C++ | C++20 | |
| Vulkan | 1.3 device, loader and headers | without them the GPU checks report an explained `SKIP`, not a failure |
| `slangc` | bundled with the Vulkan SDK from 1.3.296 | found through `VULKAN_SDK`, then `PATH` |
| OpenUSD | 26.08 measured | Hydra adapter only; a real `lookdev` or `usd` runtime |
| GLFW | 3.4 | standalone viewport only; `find_package`, else a pinned FetchContent |

Ray-traced scene rendering requires `VK_KHR_ray_query`, `rayQuery`,
acceleration structures, buffer device addresses, `fragmentStoresAndAtomics`
and RGBA32F colour attachments with storage. The bootstrap remains
available without them, and headless ray-query and path-tracing checks
report an explained SKIP. The measured runs are in the
[ray-query report](../reports/2026-10-05-primary-rays.md) and the
[BSDF and multi-bounce report](../reports/2026-10-05-bsdf-multibounce.md) and the
[HDR accumulation report](../reports/2026-10-05-hdr-accumulation.md).

Linux, macOS, AMD and Intel GPUs are not measured.

## Build and tooling limitations

- OpenStrata 0.23.14 workflow generation expects plugin workspace descriptors
  that this renderer does not own, and `ost validate` cannot select the
  runtime-free target. The repository-owned workflow consumes
  `openstrata.ci.yaml` and verifies runtime-free evidence directly. See the
  [measured CI behavior](../reports/2026-10-04-foundation-ci.md) and
  [CI procedure](../guides/BUILDING.md#ci-contracts).
  This workaround is accepted for the foundation: renderer adoption does
  not require plugin-workspace descriptors, and direct report checks retain
  the runtime-free acceptance contract without extending OpenStrata here.
- On the measured Japanese MSVC host, Hydra and viewport objects can record
  `#deps 0` in Ninja's dependency log, so header edits do not rebuild them.
  The [build guide](../guides/BUILDING.md#the-standalone-viewport) gives the
  clean-rebuild workaround; the
  [bootstrap report](../reports/ost/01-2026-10-04-v0.23.14-renderer-template-bootstrap.md)
  records the observation. This is a tooling constraint, not renderer behavior.
  The clean-rebuild workaround is accepted for the foundation because it
  makes the affected consumer builds reviewable without owning compiler or
  OpenStrata environment detection; an incremental build with missing
  dependency records is not sufficient evidence for a header change.

## Foundation bootstrap limitations

- Standalone presentation remains a bootstrap triangle, one frame in flight
  with CPU readback in the offscreen path. This is accepted for the entry-point
  and resource-lifetime foundation. Renderer Phase 2 optimized the
  transport kernels, not presentation; scene presentation and frames in
  flight are not yet assigned to a phase.
- Hydra ID channels are CPU sentinels, so picking is unavailable. The
  [AOV contract](AOVS.md#channels) accepts these bootstrap placeholders because
  the required products are color and depth. Additional debug channels are
  introduced with their producing passes; temporal outputs belong to
  Renderer Phase 4.
- Broader light sampling and production material/scene coverage are assigned
  to Renderer Phase 3 and Renderer Phase 8 respectively in the
  [deferred adapter work](../roadmap/current.md#deferred-adapter-work).
  Platform/backend expansion remains outside the measured configuration
  set. These limitations do not weaken deterministic reference, validation
  or dependency-boundary requirements.
