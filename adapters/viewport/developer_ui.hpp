// SPDX-License-Identifier: Apache-2.0
// The developer UI: a Dear ImGui window over the presented frame that edits
// the renderer controls and shows what the frame did. It is a developer
// tool, not a UI framework: one window, no docking or persistence. Dear
// ImGui and its GLFW and Vulkan backends stay private to developer_ui.cpp;
// the settings logic it drives is controls.hpp's.
#pragma once

#include "controls.hpp"
#include "window.hpp"

#include <lotus/vulkan_present.hpp>

#include <array>
#include <cstdint>
#include <memory>
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
  // The frame's wall time on the host, rendering and presenting.
  double frame_ms = 0.0;
  // GPU time of the scene pass (and wavefront kernels), the temporal
  // passes and the reservoir passes; negative without timestamps.
  double gpu_ms = -1.0;
  std::array<float, 2> depth_range{};
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
  // Whether the UI takes the pointer or the keyboard from the scene.
  [[nodiscard]] virtual bool WantsPointer() const noexcept = 0;
  [[nodiscard]] virtual bool WantsKeyboard() const noexcept = 0;
};

} // namespace Lotus::viewport
