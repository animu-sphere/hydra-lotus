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
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

#if defined(LOTUS_VIEWPORT_HYDRA)
constexpr bool kHydra = true;
#else
constexpr bool kHydra = false;
#endif

// The options' scene, then each stage the developer UI opens, in one
// window.
int Run(const Lotus::viewport::Options& options,
    const std::filesystem::path& shader_directory) {
  using namespace Lotus::viewport;
#if defined(LOTUS_VIEWPORT_HYDRA)
  if (std::string error; !options.stage.empty() &&
                         !OpenStage(options.stage, error)) {
    std::cerr << "lotus-viewport: " << error << '\n';
    return 1;
  }
#endif
  std::string reason;
  std::optional<ViewportWindow> window = OpenWindow(options, reason);
  if (!window) {
    return Unavailable(options, reason);
  }
#if defined(LOTUS_VIEWPORT_HYDRA)
  window->open_stage = &OpenStage;
#endif
  Options run = options;
  std::filesystem::path stage = options.stage;
  for (;;) {
#if defined(LOTUS_VIEWPORT_HYDRA)
    const int result = stage.empty()
                           ? RunBuiltinScene(run, *window, shader_directory)
                           : RunHydraStage(run, *window, stage);
#else
    const int result = RunBuiltinScene(run, *window, shader_directory);
#endif
    if (!window->next_stage) {
      return result;
    }
    stage = *std::exchange(window->next_stage, std::nullopt);
    // A picked stage opens as a stage without options does.
    run.camera.clear();
    run.time.reset();
  }
}

} // namespace

int main(int argc, char** argv) {
  try {
    const auto options = Lotus::viewport::ParseOptions(argc, argv);
    if (options.help) {
      std::cout << Lotus::viewport::Usage(kHydra);
      return 0;
    }
    if (!kHydra && !options.stage.empty()) {
      std::cerr << "lotus-viewport: --usd needs a viewport built with "
                   "LOTUS_ENABLE_HYDRA2 (the viewport-usd intent)\n";
      return 1;
    }
    return Run(options,
        std::filesystem::absolute(argv[0]).parent_path() / "shaders");
  } catch (const std::exception& fatal) {
    std::cerr << "lotus-viewport: " << fatal.what() << '\n';
    return 1;
  }
}
