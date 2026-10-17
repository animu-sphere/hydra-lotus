// SPDX-License-Identifier: Apache-2.0
// The window abstraction owned by the viewport adapter. GLFW is the native
// host boundary: its types stay inside window_glfw.cpp and never reach the
// core or backend targets.
#pragma once

#include "input.hpp"

#include <lotus/vulkan_present.hpp>

#include <cstdint>
#include <memory>
#include <string_view>

namespace Lotus::viewport {

class Window {
public:
  // Throws std::runtime_error when the windowing environment is unavailable
  // (no display, no Vulkan-capable GLFW); callers report that as a skip.
  static std::unique_ptr<Window> Create(std::string_view title,
      std::uint32_t width,
      std::uint32_t height, bool visible, bool fullscreen);
  virtual ~Window() = default;

  [[nodiscard]] virtual bool PollEvent(Event& event) = 0;
  virtual void WaitForEvent() = 0;
  virtual void SetTitle(std::string_view title) = 0;
  // Resizes the windowed client area; the framebuffer follows with a Resize
  // event.
  virtual void SetSize(std::uint32_t width, std::uint32_t height) = 0;
  // Fullscreen covers the primary monitor at its current video mode;
  // leaving it restores the previous windowed position and size.
  virtual void SetFullscreen(bool fullscreen) = 0;
  [[nodiscard]] virtual bool fullscreen() const noexcept = 0;
  [[nodiscard]] virtual std::uint32_t width() const noexcept = 0;
  [[nodiscard]] virtual std::uint32_t height() const noexcept = 0;
  // The GLFWwindow, for the developer UI's input backend alone.
  [[nodiscard]] virtual void* native_handle() const noexcept = 0;
};

// Bundles the GLFW-required instance extensions and the surface-creation
// callback for the backend-owned VkSurfaceKHR. The window must outlive every
// session created from the returned provider.
[[nodiscard]] PresentSurfaceProvider MakeSurfaceProvider(Window& window);

} // namespace Lotus::viewport
