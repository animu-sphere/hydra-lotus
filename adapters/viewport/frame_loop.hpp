// SPDX-License-Identifier: Apache-2.0
// The present loop that the built-in scene and USD stages share: window
// events, fullscreen, the resize test, the frame limit, the title, the run's
// checks and its benchmark JSON.
#pragma once

#include "options.hpp"
#include "window.hpp"

#include <lotus/vulkan_backend.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace Lotus::viewport {

inline constexpr int kExitSkip = 77;

// One rendered frame. `frame` may lack its colour and depth payloads; its
// product extents say whether they were read back.
struct FrameResult {
  GpuFrameEvidence frame;
  PresentEvidence present;
};

// Renders and presents one frame at the window's framebuffer extent. False,
// with `error`, ends the run as a failure.
using RenderFrameFunction = std::function<bool(std::uint32_t width,
    std::uint32_t height, FrameResult& result, std::string& error)>;

// Returns the process exit code.
[[nodiscard]] int RunFrameLoop(const Options& options, Window& window,
    std::string_view scene_source, const std::string& stage,
    const RenderFrameFunction& render);

// Reports an environment that cannot present: exit 77 with
// --allow-unavailable, otherwise 1. The explanation always goes to stdout as
// "SKIP: <reason>", as the renderer's other entry points print it.
[[nodiscard]] int Unavailable(const Options& options, std::string_view reason);

} // namespace Lotus::viewport
