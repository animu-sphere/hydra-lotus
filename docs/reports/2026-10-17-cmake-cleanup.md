# Build structure cleanup

Measured 2026-10-17 on Windows, MSVC 19.51 Release, Vulkan/Slang, NVIDIA
RTX A5000. Source base: main `5040d2c`. Renderer behaviour, shaders and
public settings are unchanged; the change is the CMake structure and the
shader handoff between the backend and its hosts.
[PROJECT_LAYOUT.md §7](../architecture/PROJECT_LAYOUT.md#7-build-structure)
owns the resulting rules.

## Question

Before the dedicated viewport and SVGF add shaders and passes, the build had
to stop spreading backend detail. The backend exported five SPIR-V paths to
the top level with `PARENT_SCOPE`; the top level, the headless runner, the
viewport, `hdLotus`, two validation tests and the install rules each listed
them, and the C++ hosts and Hydra tests named the files again. Warnings and
the C++ standard were global flags, which also reached fetched GLFW; one
373-line file held the Hydra adapter and its 17 test executables; and
`LOTUS_ENABLE_VULKAN` could be on while the backend was silently built
without Vulkan.

## Change

- `backend/vulkan/shaders/CMakeLists.txt` finds `slangc` and declares the
  three shader sources with `lotus_add_slang_shader`, which derives each
  output name (`<source>.<vert|frag|comp>.spv`) and builds it behind a
  per-source target. The `lotus-shaders` aggregate lists the outputs in a
  target property. Hosts call `lotus_stage_shaders` (copy into `shaders/`
  beside the binary, relink on a shader change) and `lotus_install_shaders`;
  none names a shader file. In C++, `ShadersIn(directory)` is the one place
  the backend names them.
- No `PARENT_SCOPE` exports remain: the headless runner installs its own
  reference images and assets, and the OpenUSD runtime root the viewport
  needs is a property of `lotus-hydra2-host-dependencies`.
- `adapters/hydra2/tests/CMakeLists.txt` holds the Hydra CTests;
  `lotus_add_hydra_test` builds each test beside `hdLotus` with the adapter's
  compile definitions. Test names, executables and locations are unchanged.
- `Lotus::Warnings` replaces `add_compile_options`, and every target states
  `cxx_std_20` instead of `CMAKE_CXX_STANDARD`. `cmake/LotusOptions.cmake`,
  `cmake/LotusInstall.cmake` and the top-level file (orchestration only)
  complete the split.
- Each component finds its own dependencies: Vulkan in the backend, the
  viewport and validation; OpenUSD in the Hydra adapter; GLFW in the
  viewport. `LOTUS_WITH_VULKAN` is gone: with `LOTUS_ENABLE_VULKAN=ON`, a
  missing Vulkan 1.3 SDK or `slangc` now fails configuration.

## Equivalence

Every object's compile flags, definitions and include directories were
compared between the generated `build.ninja` before and after the change, in
the `core`, runtime-free `ci-core` and Hydra trees: all are identical, the
only addition being the new `shaders.cpp`. In particular `/W4 /permissive-
/EHsc /utf-8` and `-std:c++20` still reach every Lotus object. The one
intended difference is in the viewport trees: the fetched GLFW sources now
compile with their own flags (`/W3`) instead of also receiving Lotus's.

The install manifests of the `core` and `ci-core` trees (written by the
install-tree CTest) list the same files as before, and so does the Hydra tree's.

Touching `common/spectrum.slang` rebuilds the path-tracing and wavefront
SPIR-V and relinks `lotus-headless` (which restages them and rewrites the
report); touching `triangle.slang` rebuilds only the bootstrap pair.

## Regression

| Build | Result |
| --- | --- |
| `core` (`ost build`, `ost test --jobs 8`) | 49/49 |
| runtime-free `ci-core` | 38/38 |
| `hydra` intent, `lookdev` (`ost test`) | 76/76 including `lotus-renderer-usdview-host` (1307 s) |
| standalone viewport (`ost renderer viewport -- --hidden --frames 8 --validate`, then `ctest`) | Cornell box presented; 51/51 |
| viewport opening USD stages (`viewport-usd`, `lookdev`, the same, with `--usd` on the smoke stage) | smoke stage presented; 79/79 including `lotus-viewport-hydra` |
| `scripts/check_docs.py`, script tests | passed, 58 tests |

One scripted `viewport-usd` run, directly after its rebuild, failed with
`VK_ERROR_DEVICE_LOST` in `vkWaitForFences`; the run before it, the CTest
run and three repeats through `run-lotus-viewport.cmd` presented normally,
so it is recorded here as unexplained rather than attributed to this change,
which compiles the viewport with unchanged flags.

## Limits

- `scripts/release.py` still lists the SPIR-V files it expects in a package:
  it checks the shipped layout, so a new shader adds a line there too.
- The Hydra adapter still sets `POSITION_INDEPENDENT_CODE` on the core and
  backend libraries, since only the module needs it.
