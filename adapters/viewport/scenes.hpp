// SPDX-License-Identifier: Apache-2.0
// What the viewport renders: the built-in reference scene, or a USD stage
// through the Hydra adapter in a Hydra-enabled build.
#pragma once

#include "options.hpp"

#include <filesystem>

namespace Lotus::viewport {

// Shaders live beside the executable, under shaders/.
[[nodiscard]] int RunBuiltinScene(const Options& options,
    const std::filesystem::path& shader_directory);

#if defined(LOTUS_VIEWPORT_HYDRA)
[[nodiscard]] int RunHydraStage(const Options& options);
#endif

} // namespace Lotus::viewport
