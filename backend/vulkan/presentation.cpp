// SPDX-License-Identifier: Apache-2.0
// Swapchain presentation of the renderer's scene colour. One blit per
// presented frame, from the RGBA32F colour target into an sRGB swapchain
// image: the format conversion encodes and clamps, so no shader is involved
// and the colour never reaches the host. A debug view first maps its product
// to an RGBA16F image with one compute dispatch and blits that instead; a
// host overlay then draws over the swapchain image in a render pass of its
// own.
#include "presentation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

#include "vulkan_internal.hpp"

namespace Lotus::vulkan_internal {
namespace {

constexpr std::uint64_t kTimeoutNs = 10'000'000'000ULL;
constexpr VkFormat kViewFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

// Mirrors ViewConstants in shaders/present/present_view.slang.
struct ViewConstants {
  std::uint32_t view;
  std::uint32_t stride;
  std::uint32_t width;
  std::uint32_t height;
  float range[2];
  std::uint32_t reserved[2];
};
static_assert(sizeof(ViewConstants) == 32);

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

bool Presentation::Initialize(VkInstance instance, VkPhysicalDevice physical,
    VkDevice device, VkQueue queue, std::uint32_t family, VkFormat source,
    const PresentOptions& options,
    const std::vector<std::uint32_t>& view_kernel, bool& unsupported,
    std::string& detail) {
  unsupported = false;
  instance_ = instance;
  physical_device_ = physical;
  device_ = device;
  queue_ = queue;
  queue_family_ = family;
  overlay_ = options.overlay;

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
          0 ||
      (overlay_.record != nullptr &&
          (capabilities.supportedUsageFlags &
              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0)) {
    unsupported = true;
    detail = "the presentation surface's images cannot be transfer targets "
             "(or, for an overlay, colour attachments)";
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
  if (!options.vsync) {
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
  if (!VulkanOk(vkCreateSemaphore(device, &semaphore_create, nullptr,
                    &image_available_),
          "vkCreateSemaphore(acquire)", detail) ||
      !VulkanOk(vkCreateFence(device, &fence_create, nullptr, &in_flight_),
          "vkCreateFence(present)", detail)) {
    return false;
  }
  if (!view_kernel.empty()) {
    VkFormatProperties view_properties{};
    vkGetPhysicalDeviceFormatProperties(physical, kViewFormat,
        &view_properties);
    constexpr VkFormatFeatureFlags kViewFeatures =
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT;
    if ((view_properties.optimalTilingFeatures & kViewFeatures) !=
        kViewFeatures) {
      unsupported = true;
      detail = "RGBA16F cannot be a storage image and a blit source";
      return false;
    }
    if (!CreateViewPipeline(view_kernel, detail)) {
      return false;
    }
  }
  return overlay_.record == nullptr || CreateOverlayRenderPass(detail);
}

bool Presentation::CreateViewPipeline(const std::vector<std::uint32_t>& words,
    std::string& detail) {
  const VkDescriptorSetLayoutBinding bindings[2] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
          nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
          nullptr}};
  VkDescriptorSetLayoutCreateInfo set_create{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_create.bindingCount = 2;
  set_create.pBindings = bindings;
  if (!VulkanOk(vkCreateDescriptorSetLayout(device_, &set_create, nullptr,
                    &view_set_layout_),
          "vkCreateDescriptorSetLayout(present view)", detail)) {
    return false;
  }
  const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0,
      sizeof(ViewConstants)};
  VkPipelineLayoutCreateInfo layout_create{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_create.setLayoutCount = 1;
  layout_create.pSetLayouts = &view_set_layout_;
  layout_create.pushConstantRangeCount = 1;
  layout_create.pPushConstantRanges = &push;
  if (!VulkanOk(vkCreatePipelineLayout(device_, &layout_create, nullptr,
                    &view_layout_),
          "vkCreatePipelineLayout(present view)", detail)) {
    return false;
  }
  const VkDescriptorPoolSize sizes[2] = {
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
  VkDescriptorPoolCreateInfo pool_create{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_create.maxSets = 1;
  pool_create.poolSizeCount = 2;
  pool_create.pPoolSizes = sizes;
  if (!VulkanOk(vkCreateDescriptorPool(device_, &pool_create, nullptr,
                    &view_pool_),
          "vkCreateDescriptorPool(present view)", detail)) {
    return false;
  }
  VkDescriptorSetAllocateInfo set_allocate{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  set_allocate.descriptorPool = view_pool_;
  set_allocate.descriptorSetCount = 1;
  set_allocate.pSetLayouts = &view_set_layout_;
  if (!VulkanOk(vkAllocateDescriptorSets(device_, &set_allocate, &view_set_),
          "vkAllocateDescriptorSets(present view)", detail)) {
    return false;
  }
  const VkShaderModule shader = CreateShader(device_, words, detail);
  if (shader == VK_NULL_HANDLE) {
    return false;
  }
  VkComputePipelineCreateInfo pipeline_create{
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pipeline_create.stage.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pipeline_create.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pipeline_create.stage.module = shader;
  pipeline_create.stage.pName = "main";
  pipeline_create.layout = view_layout_;
  const bool created = VulkanOk(
      vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline_create,
          nullptr, &view_pipeline_),
      "vkCreateComputePipelines(present view)", detail);
  vkDestroyShaderModule(device_, shader, nullptr);
  return created;
}

bool Presentation::CreateOverlayRenderPass(std::string& detail) {
  VkAttachmentDescription attachment{};
  attachment.format = surface_format_.format;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  const VkAttachmentReference reference{0,
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &reference;
  // A barrier before the pass orders the blit's writes; the present waits
  // on the submission's semaphore.
  VkSubpassDependency dependencies[2]{};
  dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[0].dstSubpass = 0;
  dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[1].srcSubpass = 0;
  dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
  dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  VkRenderPassCreateInfo create{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  create.attachmentCount = 1;
  create.pAttachments = &attachment;
  create.subpassCount = 1;
  create.pSubpasses = &subpass;
  create.dependencyCount = 2;
  create.pDependencies = dependencies;
  return VulkanOk(vkCreateRenderPass(device_, &create, nullptr, &overlay_pass_),
      "vkCreateRenderPass(overlay)", detail);
}

bool Presentation::EnsureViewImage(VkExtent2D extent, std::string& detail) {
  if (view_image_ != VK_NULL_HANDLE && view_extent_.width == extent.width &&
      view_extent_.height == extent.height) {
    return true;
  }
  DestroyViewImage();
  VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  create.imageType = VK_IMAGE_TYPE_2D;
  create.format = kViewFormat;
  create.extent = {extent.width, extent.height, 1};
  create.mipLevels = 1;
  create.arrayLayers = 1;
  create.samples = VK_SAMPLE_COUNT_1_BIT;
  create.tiling = VK_IMAGE_TILING_OPTIMAL;
  create.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!VulkanOk(vkCreateImage(device_, &create, nullptr, &view_image_),
          "vkCreateImage(present view)", detail)) {
    return false;
  }
  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(device_, view_image_, &requirements);
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = FindMemoryType(physical_device_,
      requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
  if (allocate.memoryTypeIndex == std::numeric_limits<std::uint32_t>::max()) {
    detail = "no device-local memory for the present view image";
    return false;
  }
  if (!VulkanOk(vkAllocateMemory(device_, &allocate, nullptr, &view_memory_),
          "vkAllocateMemory(present view)", detail) ||
      !VulkanOk(vkBindImageMemory(device_, view_image_, view_memory_, 0),
          "vkBindImageMemory(present view)", detail)) {
    return false;
  }
  VkImageViewCreateInfo view_create{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_create.image = view_image_;
  view_create.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_create.format = kViewFormat;
  view_create.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (!VulkanOk(vkCreateImageView(device_, &view_create, nullptr,
                    &view_image_view_),
          "vkCreateImageView(present view)", detail)) {
    return false;
  }
  view_extent_ = extent;
  return true;
}

void Presentation::DestroyViewImage() {
  vkDestroyImageView(device_, view_image_view_, nullptr);
  vkDestroyImage(device_, view_image_, nullptr);
  vkFreeMemory(device_, view_memory_, nullptr);
  view_image_view_ = VK_NULL_HANDLE;
  view_image_ = VK_NULL_HANDLE;
  view_memory_ = VK_NULL_HANDLE;
  view_extent_ = {0, 0};
}

// Recorded after the in-flight fence, so the previous frame no longer uses
// the descriptor set or the view image.
void Presentation::RecordView(const PresentSource& source) {
  const VkDescriptorImageInfo image{VK_NULL_HANDLE, view_image_view_,
      VK_IMAGE_LAYOUT_GENERAL};
  const VkDescriptorBufferInfo buffer{source.product, 0, VK_WHOLE_SIZE};
  VkWriteDescriptorSet writes[2]{};
  for (std::uint32_t binding = 0; binding < 2; ++binding) {
    writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[binding].dstSet = view_set_;
    writes[binding].dstBinding = binding;
    writes[binding].descriptorCount = 1;
  }
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[0].pImageInfo = &image;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[1].pBufferInfo = &buffer;
  vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);

  // The frame's writes to the product, in earlier submissions, before the
  // kernel reads it; the previous frame's blit of the view image before the
  // kernel overwrites it.
  VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  memory.srcAccessMask =
      VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  VkImageMemoryBarrier image_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  image_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  image_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  image_barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  image_barrier.image = view_image_;
  image_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(command_,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
          VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory, 0, nullptr, 1,
      &image_barrier);
  ViewConstants constants{};
  constants.view = static_cast<std::uint32_t>(source.view.view);
  constants.stride = source.stride;
  constants.width = source.extent.width;
  constants.height = source.extent.height;
  constants.range[0] = source.view.range[0];
  constants.range[1] = source.view.range[1];
  vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_COMPUTE, view_pipeline_);
  vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_COMPUTE,
      view_layout_, 0, 1, &view_set_, 0, nullptr);
  vkCmdPushConstants(command_, view_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
      sizeof(constants), &constants);
  vkCmdDispatch(command_, (source.extent.width + 7) / 8,
      (source.extent.height + 7) / 8, 1);
  image_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  image_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  image_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  image_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
      &image_barrier);
}

void Presentation::CallOverlay(PresentOverlayPhase phase) {
  PresentOverlayContext context;
  context.phase = phase;
  context.instance = EncodeHandle(instance_);
  context.physical_device = EncodeHandle(physical_device_);
  context.device = EncodeHandle(device_);
  context.queue = EncodeHandle(queue_);
  context.render_pass = EncodeHandle(overlay_pass_);
  context.api_version = VK_API_VERSION_1_3;
  context.queue_family = queue_family_;
  context.image_count = static_cast<std::uint32_t>(images_.size());
  context.color_format = static_cast<std::uint32_t>(surface_format_.format);
  if (phase == PresentOverlayPhase::Render) {
    context.command_buffer = EncodeHandle(command_);
    context.width = extent_.width;
    context.height = extent_.height;
  }
  overlay_.record(overlay_.user_data, context);
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
  if (overlay_pass_ != VK_NULL_HANDLE) {
    create.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  }
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
  if (overlay_pass_ != VK_NULL_HANDLE) {
    image_views_.resize(count, VK_NULL_HANDLE);
    framebuffers_.resize(count, VK_NULL_HANDLE);
    for (std::uint32_t index = 0; index < count; ++index) {
      VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view.image = images_[index];
      view.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view.format = surface_format_.format;
      view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      if (!VulkanOk(vkCreateImageView(device_, &view, nullptr,
                        &image_views_[index]),
              "vkCreateImageView(swapchain)", detail)) {
        return false;
      }
      VkFramebufferCreateInfo framebuffer{
          VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      framebuffer.renderPass = overlay_pass_;
      framebuffer.attachmentCount = 1;
      framebuffer.pAttachments = &image_views_[index];
      framebuffer.width = extent.width;
      framebuffer.height = extent.height;
      framebuffer.layers = 1;
      if (!VulkanOk(vkCreateFramebuffer(device_, &framebuffer, nullptr,
                        &framebuffers_[index]),
              "vkCreateFramebuffer(swapchain)", detail)) {
        return false;
      }
    }
  }
  extent_ = extent;
  if (swapchain_created_once_) {
    ++swapchain_recreates_;
  }
  swapchain_created_once_ = true;
  return true;
}

PresentEvidence Presentation::Present(const PresentSource& source,
    std::uint32_t width, std::uint32_t height) {
  std::string detail;
  const bool view = source.view.view != DebugView::Beauty;
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
  if (overlay_pass_ != VK_NULL_HANDLE && !overlay_initialized_) {
    // RecreateSwapchain left the device idle.
    CallOverlay(PresentOverlayPhase::Initialize);
    overlay_initialized_ = true;
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
  if (view && !EnsureViewImage(source.extent, detail)) {
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
  if (view) {
    RecordView(source);
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
  // A split view blits the view's left columns and the colour's right ones,
  // each scaled as the whole frame is.
  const auto columns = [](std::uint32_t width, float split) {
    return static_cast<std::int32_t>(
        std::lround(static_cast<double>(width) * split));
  };
  const float split = view ? std::clamp(source.view.split, 0.0F, 1.0F) : 0.0F;
  std::int32_t source_split = columns(source.extent.width, split);
  std::int32_t target_split = columns(extent_.width, split);
  // Both parts empty or neither, so that every target column is written.
  const auto source_width = static_cast<std::int32_t>(source.extent.width);
  const auto target_width = static_cast<std::int32_t>(extent_.width);
  if (source_split == 0 || target_split == 0) {
    source_split = target_split = 0;
  } else if (source_split == source_width || target_split == target_width) {
    source_split = source_width;
    target_split = target_width;
  }
  const auto blit = [&](VkImage image, std::int32_t source_begin,
                        std::int32_t source_end, std::int32_t target_begin,
                        std::int32_t target_end) {
    if (source_begin >= source_end || target_begin >= target_end) {
      return;
    }
    VkImageBlit region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[0] = {source_begin, 0, 0};
    region.srcOffsets[1] = {source_end,
        static_cast<std::int32_t>(source.extent.height), 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffsets[0] = {target_begin, 0, 0};
    region.dstOffsets[1] = {target_end,
        static_cast<std::int32_t>(extent_.height), 1};
    vkCmdBlitImage(command_, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
        VK_FILTER_NEAREST);
  };
  blit(view_image_, 0, source_split, 0, target_split);
  blit(source.color, source_split, source_width, target_split, target_width);
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  if (overlay_pass_ != VK_NULL_HANDLE) {
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
        nullptr, 1, &barrier);
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = overlay_pass_;
    pass.framebuffer = framebuffers_[image_index];
    pass.renderArea.extent = extent_;
    vkCmdBeginRenderPass(command_, &pass, VK_SUBPASS_CONTENTS_INLINE);
    CallOverlay(PresentOverlayPhase::Render);
    vkCmdEndRenderPass(command_);
  } else {
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
        &barrier);
  }
  if (view) {
    // The kernel's reads of the product before the next frame rewrites it.
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 0,
        nullptr);
  }
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
  for (VkFramebuffer framebuffer : framebuffers_) {
    vkDestroyFramebuffer(device_, framebuffer, nullptr);
  }
  framebuffers_.clear();
  for (VkImageView view : image_views_) {
    vkDestroyImageView(device_, view, nullptr);
  }
  image_views_.clear();
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
    if (overlay_initialized_) {
      CallOverlay(PresentOverlayPhase::Shutdown);
      overlay_initialized_ = false;
    }
    DestroySwapchain();
    DestroyViewImage();
    vkDestroyPipeline(device_, view_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, view_layout_, nullptr);
    vkDestroyDescriptorPool(device_, view_pool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, view_set_layout_, nullptr);
    vkDestroyRenderPass(device_, overlay_pass_, nullptr);
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
