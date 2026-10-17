// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Lotus {

// The window layer supplies the native pieces the backend needs to own a
// VkSurfaceKHR, without Vulkan or windowing types crossing this header.
// Handles travel encoded in std::uintptr_t: `create_surface` receives the
// encoded VkInstance, writes the encoded VkSurfaceKHR, and returns the raw
// VkResult as an integer.
struct PresentSurfaceProvider {
  std::vector<std::string> instance_extensions;
  std::int32_t (*create_surface)(void* user_data, std::uintptr_t instance,
      std::uintptr_t* surface) = nullptr;
  void* user_data = nullptr;
};

// A renderer created for a window (CreateOffscreenRenderer's
// `presentation`). The backend owns the surface and the swapchain; the window
// must outlive the renderer.
struct PresentOptions {
  PresentSurfaceProvider surface;
  // FIFO when true. When false, IMMEDIATE, else MAILBOX, else FIFO: whichever
  // the surface offers first.
  bool vsync = true;
};

} // namespace Lotus
