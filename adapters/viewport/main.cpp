// SPDX-License-Identifier: Apache-2.0
// Standalone viewport: the renderer's frontend for development and
// debugging. It path-traces the built-in reference scene, or a USD stage in a
// Hydra-enabled build, and presents the progressive output from the backend
// without a CPU readback. `ost renderer viewport` builds and launches it; it
// also runs as a scripted GPU smoke test (`--hidden --frames N`). Exit codes:
// 0 success, 1 failure, 77 skip (with --allow-unavailable, an environment
// that cannot present).
#include "options.hpp"
#include "scenes.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {

#if defined(LOTUS_VIEWPORT_HYDRA)
constexpr bool kHydra = true;
#else
constexpr bool kHydra = false;
#endif

} // namespace

int main(int argc, char** argv) {
  try {
    const auto options = Lotus::viewport::ParseOptions(argc, argv);
    if (options.help) {
      std::cout << Lotus::viewport::Usage(kHydra);
      return 0;
    }
    if (!options.stage.empty()) {
#if defined(LOTUS_VIEWPORT_HYDRA)
      return Lotus::viewport::RunHydraStage(options);
#else
      std::cerr << "lotus-viewport: --usd needs a viewport built with "
                   "LOTUS_ENABLE_HYDRA2 (the viewport-usd intent)\n";
      return 1;
#endif
    }
    return Lotus::viewport::RunBuiltinScene(options,
        std::filesystem::absolute(argv[0]).parent_path() / "shaders");
  } catch (const std::exception& fatal) {
    std::cerr << "lotus-viewport: " << fatal.what() << '\n';
    return 1;
  }
}
