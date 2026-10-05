# Supported configurations

Configurations a build and test run has actually passed on. A configuration
that has only been reviewed is not listed. No CI runs yet, so every row is a
local run, and the report named in the row holds its detail.

## Measured

| OS | Compiler | Build | Runtime | GPU | Result | Report |
| --- | --- | --- | --- | --- | --- | --- |
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
acceleration structures and buffer device addresses. The bootstrap remains
available without them, and headless ray-query and path-tracing checks
report an explained SKIP. The measured runs are in the
[ray-query report](../reports/2026-10-05-primary-rays.md) and the
[BSDF and multi-bounce report](../reports/2026-10-05-bsdf-multibounce.md).

Linux, macOS, AMD and Intel GPUs are not measured.
