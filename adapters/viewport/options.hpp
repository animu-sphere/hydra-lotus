// SPDX-License-Identifier: Apache-2.0
// Command-line options of lotus-viewport. GPU- and window-free, so that a
// unit test covers them in every configuration.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace Lotus::viewport {

struct Options {
  std::uint32_t width = 1280;
  std::uint32_t height = 720;
  // Exit after this many presented frames; 0 runs until the window closes.
  std::uint64_t frame_limit = 0;
  // Radiance samples per pixel at which the accumulation stops growing.
  std::uint32_t samples = 1024;
  bool visible = true;
  bool fullscreen = false;
  bool vsync = true;
  // Fail unless the Khronos validation layer captured the run. Messages fail
  // the run either way.
  bool validate = false;
  // Resize the window once, after the second presented frame, and fail
  // unless the swapchain followed it.
  bool resize_test = false;
  // Apply the scene's scripted inputs and edits, one per frame from the
  // second, and fail unless each restarts or continues the accumulation as
  // it should.
  bool interaction_test = false;
  // Exit 77 with an explanation instead of 1 when this environment cannot
  // present: no window system, Vulkan driver or capable device.
  bool allow_unavailable = false;
  // A USD stage, opened through the Hydra adapter; empty renders the
  // built-in reference scene.
  std::filesystem::path stage;
  // The stage's camera prim to look through at first; empty starts with the
  // free camera.
  std::string camera;
  // The stage time code to open at; nothing uses the stage's start time
  // code when it authors a time range, and the default time otherwise.
  std::optional<double> time;
  // Where to write the run's benchmark JSON; empty writes none.
  std::filesystem::path benchmark;
  bool help = false;
};

// Throws std::invalid_argument naming the offending option.
[[nodiscard]] Options ParseOptions(int argc, const char* const* argv);

[[nodiscard]] std::string Usage(bool hydra);

} // namespace Lotus::viewport
