// SPDX-License-Identifier: Apache-2.0
// The swapchain of a renderer created for a window. The backend owns the
// VkSurfaceKHR and every swapchain object; the window layer only supplies the
// surface-creation callback and the platform instance extensions. Private to
// backend/vulkan.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include <lotus/vulkan_backend.hpp>

namespace Lotus::vulkan_internal {

class Presentation {
public:
  // Creates the window's surface on `instance`. False, with `detail`, when
  // the window layer cannot.
  bool CreateSurface(VkInstance instance, const PresentSurfaceProvider& provider,
      std::string& detail);
  // Whether `device` has VK_KHR_swapchain and presents to the surface from
  // queue family `family`.
  [[nodiscard]] bool Supports(VkPhysicalDevice device, std::uint32_t family) const;
  // After the device exists: chooses an sRGB surface format that `source`
  // (the scene colour format) can be blitted to and the present mode, and
  // creates the presentation's own command buffer and synchronization.
  // `unsupported` reports a surface or device that cannot do this.
  bool Initialize(VkPhysicalDevice physical, VkDevice device, VkQueue queue,
      std::uint32_t family, VkFormat source, bool vsync, bool& unsupported,
      std::string& detail);
  // Blits `source`, in TRANSFER_SRC_OPTIMAL after a scene frame, to the next
  // swapchain image and presents it. The caller fills the validation fields.
  [[nodiscard]] PresentEvidence Present(VkImage source, VkExtent2D source_extent,
      std::uint32_t width, std::uint32_t height);
  // Device objects first, then the surface; call with the device idle.
  void Destroy(VkInstance instance);

private:
  bool RecreateSwapchain(std::uint32_t width, std::uint32_t height,
      std::string& detail);
  void DestroySwapchain();
  PresentEvidence Evidence(FrameStatus status, std::string detail) const;

  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer command_ = VK_NULL_HANDLE;
  VkSurfaceFormatKHR surface_format_{};
  VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
  VkSemaphore image_available_ = VK_NULL_HANDLE;
  // Signalled when the previous blit has finished with the command buffer.
  VkFence in_flight_ = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkExtent2D extent_{};
  std::vector<VkImage> images_;
  // Present-wait semaphores are per swapchain image: a single semaphore may
  // still be in use by an outstanding present when the next frame needs it.
  std::vector<VkSemaphore> render_finished_;
  bool swapchain_created_once_ = false;
  std::uint64_t frames_presented_ = 0;
  std::uint32_t swapchain_recreates_ = 0;
};

} // namespace Lotus::vulkan_internal
