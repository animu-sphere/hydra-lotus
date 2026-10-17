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

enum class PresentOverlayPhase {
  // Once, when the first swapchain exists, with the device idle.
  Initialize,
  // Every presented frame, inside a render pass over the swapchain image
  // that loads the presented frame and ends ready for presentation.
  Render,
  // Once, before the renderer destroys its device, with the device idle.
  Shutdown,
};

// The native state a host overlay (the viewport's developer UI) records its
// draws with, encoded as in PresentSurfaceProvider. The backend keeps
// ownership of every handle; the overlay creates and destroys only its own
// objects, between Initialize and Shutdown, and submits to `queue` only
// during those two phases.
struct PresentOverlayContext {
  PresentOverlayPhase phase = PresentOverlayPhase::Render;
  std::uintptr_t instance = 0;
  std::uintptr_t physical_device = 0;
  std::uintptr_t device = 0;
  std::uintptr_t queue = 0;
  // A render pass compatible with every swapchain image's: one colour
  // attachment of `color_format`, single-sampled.
  std::uintptr_t render_pass = 0;
  // Render only: the command buffer, inside that render pass.
  std::uintptr_t command_buffer = 0;
  std::uint32_t api_version = 0;
  std::uint32_t queue_family = 0;
  std::uint32_t image_count = 0;
  // Render only: the swapchain extent.
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  // The swapchain's VkFormat, an sRGB one: the overlay's colours are
  // encoded on the way in.
  std::uint32_t color_format = 0;
};

struct PresentOverlay {
  void (*record)(void* user_data, const PresentOverlayContext& context) =
      nullptr;
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
  // Drawn over every presented frame when `record` is set; it must outlive
  // the renderer.
  PresentOverlay overlay;
};

} // namespace Lotus
