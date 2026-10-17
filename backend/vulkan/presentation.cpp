// SPDX-License-Identifier: Apache-2.0
// Swapchain presentation of the renderer's scene colour. One blit per
// presented frame, from the RGBA32F colour target into an sRGB swapchain
// image: the format conversion encodes and clamps, so no shader or render
// pass is involved and the colour never reaches the host.
#include "presentation.hpp"

#include <algorithm>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

#include "vulkan_internal.hpp"

namespace Lotus::vulkan_internal {
namespace {

constexpr std::uint64_t kTimeoutNs = 10'000'000'000ULL;

template <typename Handle>
std::uintptr_t EncodeHandle(Handle handle) noexcept {
  if constexpr (std::is_pointer_v<Handle>) {
    return reinterpret_cast<std::uintptr_t>(handle);
  } else {
    return static_cast<std::uintptr_t>(handle);
  }
}

template <typename Handle>
Handle DecodeHandle(std::uintptr_t handle) noexcept {
  if constexpr (std::is_pointer_v<Handle>) {
    return reinterpret_cast<Handle>(handle);
  } else {
    return static_cast<Handle>(handle);
  }
}

bool HasDeviceExtension(VkPhysicalDevice device, const char* name) {
  std::uint32_t count = 0;
  vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> extensions(count);
  vkEnumerateDeviceExtensionProperties(device, nullptr, &count,
      extensions.data());
  return std::any_of(extensions.begin(), extensions.end(),
      [&](const VkExtensionProperties& extension) {
        return std::string_view(extension.extensionName) == name;
      });
}

const char* FormatName(VkFormat format) {
  switch (format) {
  case VK_FORMAT_B8G8R8A8_SRGB:
    return "B8G8R8A8_SRGB";
  case VK_FORMAT_R8G8B8A8_SRGB:
    return "R8G8B8A8_SRGB";
  case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    return "A8B8G8R8_SRGB_PACK32";
  default:
    return "other";
  }
}

const char* PresentModeName(VkPresentModeKHR mode) {
  switch (mode) {
  case VK_PRESENT_MODE_IMMEDIATE_KHR:
    return "IMMEDIATE";
  case VK_PRESENT_MODE_MAILBOX_KHR:
    return "MAILBOX";
  case VK_PRESENT_MODE_FIFO_KHR:
    return "FIFO";
  case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
    return "FIFO_RELAXED";
  default:
    return "other";
  }
}

} // namespace

bool Presentation::CreateSurface(VkInstance instance,
    const PresentSurfaceProvider& provider, std::string& detail) {
  if (provider.create_surface == nullptr) {
    detail = "the surface provider carries no create_surface callback";
    return false;
  }
  std::uintptr_t encoded = 0;
  const auto result = static_cast<VkResult>(provider.create_surface(
      provider.user_data, EncodeHandle(instance), &encoded));
  if (result != VK_SUCCESS || encoded == 0) {
    detail = "the window layer could not create a presentation surface "
             "(VkResult " +
             std::to_string(static_cast<std::int32_t>(result)) + ")";
    return false;
  }
  surface_ = DecodeHandle<VkSurfaceKHR>(encoded);
  return true;
}

bool Presentation::Supports(VkPhysicalDevice device, std::uint32_t family) const {
  VkBool32 supported = VK_FALSE;
  return HasDeviceExtension(device, VK_KHR_SWAPCHAIN_EXTENSION_NAME) &&
         vkGetPhysicalDeviceSurfaceSupportKHR(device, family, surface_,
             &supported) == VK_SUCCESS &&
         supported == VK_TRUE;
}

bool Presentation::Initialize(VkPhysicalDevice physical, VkDevice device,
    VkQueue queue, std::uint32_t family, VkFormat source, bool vsync,
    bool& unsupported, std::string& detail) {
  unsupported = false;
  physical_device_ = physical;
  device_ = device;
  queue_ = queue;

  VkFormatProperties source_properties{};
  vkGetPhysicalDeviceFormatProperties(physical, source, &source_properties);
  if ((source_properties.optimalTilingFeatures &
          VK_FORMAT_FEATURE_BLIT_SRC_BIT) == 0) {
    unsupported = true;
    detail = "the scene colour format cannot be a blit source";
    return false;
  }
  VkSurfaceCapabilitiesKHR capabilities{};
  if (!VulkanOk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
                    physical, surface_, &capabilities),
          "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", detail)) {
    return false;
  }
  if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ==
      0) {
    unsupported = true;
    detail = "the presentation surface's images cannot be transfer targets";
    return false;
  }
  std::uint32_t format_count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface_, &format_count,
      nullptr);
  std::vector<VkSurfaceFormatKHR> formats(format_count);
  vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface_, &format_count,
      formats.data());
  // The blit's conversion does the sRGB encoding, so only sRGB formats
  // display the linear colour correctly.
  const auto found = std::find_if(formats.begin(), formats.end(),
      [&](const VkSurfaceFormatKHR& format) {
        if (format.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR ||
            (format.format != VK_FORMAT_B8G8R8A8_SRGB &&
                format.format != VK_FORMAT_R8G8B8A8_SRGB &&
                format.format != VK_FORMAT_A8B8G8R8_SRGB_PACK32)) {
          return false;
        }
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physical, format.format,
            &properties);
        return (properties.optimalTilingFeatures &
                   VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
      });
  if (found == formats.end()) {
    unsupported = true;
    detail = "the presentation surface offers no 8-bit sRGB format the "
             "scene colour can be blitted to";
    return false;
  }
  surface_format_ = *found;

  present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
  if (!vsync) {
    std::uint32_t mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface_, &mode_count,
        nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface_, &mode_count,
        modes.data());
    for (const VkPresentModeKHR preferred :
        {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR}) {
      if (std::find(modes.begin(), modes.end(), preferred) != modes.end()) {
        present_mode_ = preferred;
        break;
      }
    }
  }

  VkCommandPoolCreateInfo pool_create{
      VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_create.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_create.queueFamilyIndex = family;
  if (!VulkanOk(vkCreateCommandPool(device, &pool_create, nullptr,
                    &command_pool_),
          "vkCreateCommandPool(present)", detail)) {
    return false;
  }
  VkCommandBufferAllocateInfo command_allocate{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  command_allocate.commandPool = command_pool_;
  command_allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_allocate.commandBufferCount = 1;
  if (!VulkanOk(vkAllocateCommandBuffers(device, &command_allocate, &command_),
          "vkAllocateCommandBuffers(present)", detail)) {
    return false;
  }
  VkSemaphoreCreateInfo semaphore_create{
      VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkFenceCreateInfo fence_create{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fence_create.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  return VulkanOk(vkCreateSemaphore(device, &semaphore_create, nullptr,
                      &image_available_),
             "vkCreateSemaphore(acquire)", detail) &&
         VulkanOk(vkCreateFence(device, &fence_create, nullptr, &in_flight_),
             "vkCreateFence(present)", detail);
}

PresentEvidence Presentation::Evidence(FrameStatus status,
    std::string detail) const {
  PresentEvidence evidence;
  evidence.status = status;
  evidence.detail = std::move(detail);
  evidence.frames_presented = frames_presented_;
  evidence.swapchain_recreates = swapchain_recreates_;
  evidence.swapchain_width = extent_.width;
  evidence.swapchain_height = extent_.height;
  evidence.swapchain_format = FormatName(surface_format_.format);
  evidence.present_mode = PresentModeName(present_mode_);
  return evidence;
}

bool Presentation::RecreateSwapchain(std::uint32_t width,
    std::uint32_t height, std::string& detail) {
  vkDeviceWaitIdle(device_);
  DestroySwapchain();

  VkSurfaceCapabilitiesKHR capabilities{};
  if (!VulkanOk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
                    physical_device_, surface_, &capabilities),
          "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", detail)) {
    return false;
  }
  VkExtent2D extent = capabilities.currentExtent;
  if (extent.width == std::numeric_limits<std::uint32_t>::max()) {
    extent.width = std::clamp(width, capabilities.minImageExtent.width,
        capabilities.maxImageExtent.width);
    extent.height = std::clamp(height, capabilities.minImageExtent.height,
        capabilities.maxImageExtent.height);
  }
  if (extent.width == 0 || extent.height == 0) {
    // Minimized between the caller's size query and now.
    return true;
  }
  std::uint32_t image_count = capabilities.minImageCount + 1;
  if (capabilities.maxImageCount > 0) {
    image_count = std::min(image_count, capabilities.maxImageCount);
  }
  VkCompositeAlphaFlagBitsKHR composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  if ((capabilities.supportedCompositeAlpha & composite) == 0) {
    for (const VkCompositeAlphaFlagBitsKHR candidate :
        {VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR}) {
      if ((capabilities.supportedCompositeAlpha & candidate) != 0) {
        composite = candidate;
        break;
      }
    }
  }
  VkSwapchainCreateInfoKHR create{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  create.surface = surface_;
  create.minImageCount = image_count;
  create.imageFormat = surface_format_.format;
  create.imageColorSpace = surface_format_.colorSpace;
  create.imageExtent = extent;
  create.imageArrayLayers = 1;
  create.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  create.preTransform = capabilities.currentTransform;
  create.compositeAlpha = composite;
  create.presentMode = present_mode_;
  create.clipped = VK_TRUE;
  if (!VulkanOk(vkCreateSwapchainKHR(device_, &create, nullptr, &swapchain_),
          "vkCreateSwapchainKHR", detail)) {
    return false;
  }
  std::uint32_t count = 0;
  vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
  images_.resize(count);
  vkGetSwapchainImagesKHR(device_, swapchain_, &count, images_.data());
  render_finished_.resize(count, VK_NULL_HANDLE);
  for (VkSemaphore& semaphore : render_finished_) {
    VkSemaphoreCreateInfo semaphore_create{
        VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (!VulkanOk(vkCreateSemaphore(device_, &semaphore_create, nullptr,
                      &semaphore),
            "vkCreateSemaphore(present)", detail)) {
      return false;
    }
  }
  extent_ = extent;
  if (swapchain_created_once_) {
    ++swapchain_recreates_;
  }
  swapchain_created_once_ = true;
  return true;
}

PresentEvidence Presentation::Present(VkImage source,
    VkExtent2D source_extent, std::uint32_t width, std::uint32_t height) {
  std::string detail;
  if (width == 0 || height == 0) {
    return Evidence(FrameStatus::Skip, "the window has a zero extent");
  }
  if (swapchain_ == VK_NULL_HANDLE || width != extent_.width ||
      height != extent_.height) {
    if (!RecreateSwapchain(width, height, detail)) {
      return Evidence(FrameStatus::Fail, detail);
    }
    if (swapchain_ == VK_NULL_HANDLE) {
      return Evidence(FrameStatus::Skip, "the surface has a zero extent");
    }
  }
  if (!VulkanOk(vkWaitForFences(device_, 1, &in_flight_, VK_TRUE, kTimeoutNs),
          "vkWaitForFences(present)", detail)) {
    return Evidence(FrameStatus::Fail, detail);
  }
  std::uint32_t image_index = 0;
  const VkResult acquire = vkAcquireNextImageKHR(device_, swapchain_,
      kTimeoutNs, image_available_, VK_NULL_HANDLE, &image_index);
  if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
    if (!RecreateSwapchain(width, height, detail)) {
      return Evidence(FrameStatus::Fail, detail);
    }
    return Evidence(FrameStatus::Skip, "the swapchain was out of date");
  }
  if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
    VulkanOk(acquire, "vkAcquireNextImageKHR", detail);
    return Evidence(FrameStatus::Fail, detail);
  }

  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (!VulkanOk(vkResetFences(device_, 1, &in_flight_), "vkResetFences",
          detail) ||
      !VulkanOk(vkResetCommandBuffer(command_, 0), "vkResetCommandBuffer",
          detail) ||
      !VulkanOk(vkBeginCommandBuffer(command_, &begin), "vkBeginCommandBuffer",
          detail)) {
    return Evidence(FrameStatus::Fail, detail);
  }
  const VkImage target = images_[image_index];
  // The acquire semaphore's wait stage is TRANSFER; the barrier chains on
  // it. The scene render pass's outgoing dependency already orders its
  // colour writes before transfer reads, as for the readback copy, and its
  // incoming dependency orders the next frame's writes after this blit.
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = target;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  VkImageBlit blit{};
  blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.srcOffsets[1] = {static_cast<std::int32_t>(source_extent.width),
      static_cast<std::int32_t>(source_extent.height), 1};
  blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.dstOffsets[1] = {static_cast<std::int32_t>(extent_.width),
      static_cast<std::int32_t>(extent_.height), 1};
  vkCmdBlitImage(command_, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
      VK_FILTER_NEAREST);
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = 0;
  vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
      &barrier);
  if (!VulkanOk(vkEndCommandBuffer(command_), "vkEndCommandBuffer", detail)) {
    return Evidence(FrameStatus::Fail, detail);
  }
  const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.waitSemaphoreCount = 1;
  submit.pWaitSemaphores = &image_available_;
  submit.pWaitDstStageMask = &wait_stage;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command_;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &render_finished_[image_index];
  if (!VulkanOk(vkQueueSubmit(queue_, 1, &submit, in_flight_),
          "vkQueueSubmit(present)", detail)) {
    return Evidence(FrameStatus::Fail, detail);
  }
  VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &render_finished_[image_index];
  present.swapchainCount = 1;
  present.pSwapchains = &swapchain_;
  present.pImageIndices = &image_index;
  const VkResult result = vkQueuePresentKHR(queue_, &present);
  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    if (!RecreateSwapchain(width, height, detail)) {
      return Evidence(FrameStatus::Fail, detail);
    }
    return Evidence(FrameStatus::Skip, "the swapchain was out of date");
  }
  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    VulkanOk(result, "vkQueuePresentKHR", detail);
    return Evidence(FrameStatus::Fail, detail);
  }
  ++frames_presented_;
  // Recreate after a suboptimal present only; the frame itself was shown.
  if (result == VK_SUBOPTIMAL_KHR || acquire == VK_SUBOPTIMAL_KHR) {
    if (!RecreateSwapchain(width, height, detail)) {
      return Evidence(FrameStatus::Fail, detail);
    }
  }
  return Evidence(FrameStatus::Pass, "");
}

void Presentation::DestroySwapchain() {
  for (VkSemaphore semaphore : render_finished_) {
    vkDestroySemaphore(device_, semaphore, nullptr);
  }
  render_finished_.clear();
  images_.clear();
  if (swapchain_ != VK_NULL_HANDLE) {
    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
  }
  extent_ = {0, 0};
}

void Presentation::Destroy(VkInstance instance) {
  if (device_ != VK_NULL_HANDLE) {
    DestroySwapchain();
    vkDestroyFence(device_, in_flight_, nullptr);
    vkDestroySemaphore(device_, image_available_, nullptr);
    vkDestroyCommandPool(device_, command_pool_, nullptr);
    device_ = VK_NULL_HANDLE;
  }
  if (surface_ != VK_NULL_HANDLE) {
    vkDestroySurfaceKHR(instance, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
  }
}

} // namespace Lotus::vulkan_internal
