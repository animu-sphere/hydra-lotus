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

Nothing here needs ray tracing hardware yet. Renderer Phase 1 adds the
ray query requirement ([roadmap](../roadmap/current.md#before-renderer-phase-1)).

Linux, macOS, AMD and Intel GPUs are not measured.
