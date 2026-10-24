// SPDX-License-Identifier: Apache-2.0
// What the viewport renders: the built-in reference scene, or a USD stage
// through the Hydra adapter in a Hydra-enabled build. Each renders in the
// window the caller opened, until it closes or the developer UI opens a
// stage (window.next_stage).
#pragma once

#include "app/frame_loop.hpp"
#include "app/options.hpp"

#include <filesystem>
#include <string>

namespace Lotus::viewport {

// Shaders live beside the executable, under shaders/.
[[nodiscard]] int RunBuiltinScene(const Options& options,
    ViewportWindow& window, const std::filesystem::path& shader_directory);

#if defined(LOTUS_VIEWPORT_HYDRA)
// Opens `stage` for the next RunHydraStage, or says why it cannot.
[[nodiscard]] bool OpenStage(const std::filesystem::path& stage,
    std::string& error);
// Renders the stage OpenStage opened last.
[[nodiscard]] int RunHydraStage(const Options& options,
    ViewportWindow& window, const std::filesystem::path& stage);
#endif

} // namespace Lotus::viewport
