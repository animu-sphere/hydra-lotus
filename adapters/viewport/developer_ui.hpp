// SPDX-License-Identifier: Apache-2.0
// The developer UI: Dear ImGui windows over the presented frame, one that
// edits the renderer controls and one, the timing HUD, that shows what the
// frames cost. It is a developer tool, not a UI framework: two windows, no
// docking or persistence. Dear ImGui and its GLFW and Vulkan backends stay
// private to developer_ui.cpp; the settings logic it drives is controls.hpp's
// and the figures it shows are timings.hpp's.
#pragma once

#include "controls.hpp"
#include "timings.hpp"
#include "window.hpp"

#include <lotus/vulkan_present.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace Lotus::viewport {

// What the window reports about the latest frame.
struct UiStatus {
  std::string_view source;
  std::string scene;
  std::string_view device;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t samples_per_pixel = 0;
  // The frames so far; empty before the first.
  const TimingRecord* timings = nullptr;
  std::array<float, 2> depth_range{};
  // The open USD stage; empty for the built-in scene.
  std::string_view stage;
  // Whether this build opens a stage the UI picks.
  bool can_open_stage = false;
};

class DeveloperUi {
public:
  // Throws std::runtime_error when Dear ImGui cannot use the window.
  static std::unique_ptr<DeveloperUi> Create(Window& window);
  virtual ~DeveloperUi() = default;

  // Draws the UI over every presented frame. The UI must outlive the
  // renderer created with `presentation`.
  virtual void ConfigurePresentation(PresentOptions& presentation) = 0;
  // Builds this frame's UI, before the frame renders, and applies the
  // user's edits to `controls`. True when they changed.
  virtual bool Draw(Controls& controls, const UiStatus& status) = 0;
  virtual void ToggleVisible() = 0;
  // A USD stage the user picked in the file dialog since the last call.
  [[nodiscard]] virtual std::optional<std::filesystem::path>
  TakeStageRequest() = 0;
  // Why the picked stage did not open, shown until the next pick.
  virtual void SetStageError(std::string error) = 0;
  // Whether the UI takes the pointer or the keyboard from the scene.
  [[nodiscard]] virtual bool WantsPointer() const noexcept = 0;
  [[nodiscard]] virtual bool WantsKeyboard() const noexcept = 0;
};

} // namespace Lotus::viewport
