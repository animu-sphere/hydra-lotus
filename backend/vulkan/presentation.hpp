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

// What Present copies to the swapchain: the scene colour, or a debug
// product that the view kernel maps to display colours first.
struct PresentSource {
  // The scene colour, in TRANSFER_SRC_OPTIMAL after a scene frame.
  VkImage color = VK_NULL_HANDLE;
  VkExtent2D extent{};
  PresentView view;
  // Debug views: the product's buffer, one record per pixel of `extent`,
  // each `stride` 16-byte words long.
  VkBuffer product = VK_NULL_HANDLE;
  std::uint32_t stride = 0;
};

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
  // creates the presentation's own command buffer and synchronization, the
  // debug views' pipeline from `view_kernel` (none when it is empty) and the
  // overlay's render pass. `unsupported` reports a surface or device that
  // cannot do this.
  bool Initialize(VkInstance instance, VkPhysicalDevice physical,
      VkDevice device, VkQueue queue, std::uint32_t family, VkFormat source,
      const PresentOptions& options,
      const std::vector<std::uint32_t>& view_kernel, bool& unsupported,
      std::string& detail);
  [[nodiscard]] bool has_view_kernel() const noexcept {
    return view_pipeline_ != VK_NULL_HANDLE;
  }
  // Copies `source` to the next swapchain image, draws the overlay over it
  // and presents it. The caller fills the validation fields.
  [[nodiscard]] PresentEvidence Present(const PresentSource& source,
      std::uint32_t width, std::uint32_t height);
  // The overlay's Shutdown, device objects, then the surface; call with the
  // device idle.
  void Destroy(VkInstance instance);

private:
  bool RecreateSwapchain(std::uint32_t width, std::uint32_t height,
      std::string& detail);
  void DestroySwapchain();
  bool CreateViewPipeline(const std::vector<std::uint32_t>& words,
      std::string& detail);
  bool CreateOverlayRenderPass(std::string& detail);
  // The view image at `extent`, recreated when it changes.
  bool EnsureViewImage(VkExtent2D extent, std::string& detail);
  void DestroyViewImage();
  void RecordView(const PresentSource& source);
  void CallOverlay(PresentOverlayPhase phase);
  PresentEvidence Evidence(FrameStatus status, std::string detail) const;

  VkInstance instance_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  std::uint32_t queue_family_ = 0;
  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer command_ = VK_NULL_HANDLE;
  VkSurfaceFormatKHR surface_format_{};
  VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
  VkSemaphore image_available_ = VK_NULL_HANDLE;
  // Signalled when the previous frame's commands have finished with the
  // command buffer, the view image and the view descriptor set.
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

  // Debug views: a compute kernel writes the RGBA16F view image, which is
  // blitted in place of the scene colour.
  VkDescriptorSetLayout view_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout view_layout_ = VK_NULL_HANDLE;
  VkPipeline view_pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool view_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet view_set_ = VK_NULL_HANDLE;
  VkImage view_image_ = VK_NULL_HANDLE;
  VkDeviceMemory view_memory_ = VK_NULL_HANDLE;
  VkImageView view_image_view_ = VK_NULL_HANDLE;
  VkExtent2D view_extent_{};

  // The overlay draws in a load-preserving render pass over each swapchain
  // image.
  PresentOverlay overlay_;
  bool overlay_initialized_ = false;
  VkRenderPass overlay_pass_ = VK_NULL_HANDLE;
  std::vector<VkImageView> image_views_;
  std::vector<VkFramebuffer> framebuffers_;
};

} // namespace Lotus::vulkan_internal
