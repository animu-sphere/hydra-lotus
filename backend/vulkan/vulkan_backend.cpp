// SPDX-License-Identifier: Apache-2.0
#include <lotus/vulkan_backend.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#if defined(LOTUS_HAS_VULKAN)
#include <vulkan/vulkan.h>

#include "gpu_scene.hpp"
#include "vulkan_internal.hpp"
#endif

namespace Lotus {

BackendCapability ProbeVulkanBackend() {
#if defined(LOTUS_HAS_VULKAN)
  std::uint32_t version = VK_API_VERSION_1_0;
  const VkResult result = vkEnumerateInstanceVersion(&version);
  if (result != VK_SUCCESS) {
    return {false, "Vulkan loader version query failed"};
  }
  std::ostringstream detail;
  detail << "Vulkan loader API " << VK_API_VERSION_MAJOR(version) << '.'
         << VK_API_VERSION_MINOR(version) << '.' << VK_API_VERSION_PATCH(version);
  return {version >= VK_API_VERSION_1_3, detail.str()};
#else
  return {false, "Vulkan 1.3 SDK/loader was not available at configure time"};
#endif
}

namespace {

GpuFrameEvidence Evidence(FrameStatus status, std::string detail) {
  GpuFrameEvidence evidence;
  evidence.status = status;
  evidence.detail = std::move(detail);
  return evidence;
}

#if defined(LOTUS_HAS_VULKAN)

using vulkan_internal::AccelerationSupport;
using vulkan_internal::CreateInstanceWithValidation;
using vulkan_internal::CreateShader;
using vulkan_internal::DestroyInstance;
using vulkan_internal::FindMemoryType;
using vulkan_internal::GpuScene;
using vulkan_internal::InstanceState;
using vulkan_internal::kAccelerationExtensions;
using vulkan_internal::kFrameConstantsSize;
using vulkan_internal::kMaxTextures;
using vulkan_internal::kTextureSamplers;
using vulkan_internal::LoadSpirv;
using vulkan_internal::ProbeAccelerationStructures;
using vulkan_internal::SupportsShaderDrawParameters;
using vulkan_internal::ValidationState;
using vulkan_internal::VulkanOk;
using vulkan_internal::VulkanWorldToClip;

constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
// The scene pass's colour target and its radiance accumulation image.
constexpr VkFormat kSceneColorFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

struct RayConstants {
  Matrix4 world_to_clip;
  Matrix4 clip_to_world;
};
static_assert(sizeof(RayConstants) == 128);

// The scene passes' uniform block, std140; mirrored in path_trace.slang.
struct PathConstants {
  float environment[4];
  float background[4];
  float ndc_from_pixel[4];
  std::uint64_t instances;
  std::uint64_t materials;
  std::uint32_t sample_index;
  std::uint32_t accumulated;
  std::uint32_t add_sample;
  std::uint32_t max_bounces;
  std::uint32_t width;
  // What the surface pass writes: a SurfaceOutput.
  std::uint32_t surface_output;
  // The wavefront integrator's path slots: one per target pixel.
  std::uint32_t path_capacity;
  std::uint32_t reserved;
  // DataScissor: {x, y, width, height}.
  std::uint32_t data_rect[4];
};
static_assert(sizeof(PathConstants) == 112 &&
              offsetof(PathConstants, instances) == 48 &&
              offsetof(PathConstants, materials) == 56 &&
              offsetof(PathConstants, width) == 80 &&
              offsetof(PathConstants, data_rect) == 96);

// The path tracer's kPass specialization constant. The resolve pass writes
// the mean of an accumulation the wavefront kernels added to.
constexpr std::uint32_t kCameraPass = 0;
constexpr std::uint32_t kRadiancePass = 1;
constexpr std::uint32_t kSurfacePass = 2;
constexpr std::uint32_t kResolvePass = 3;

// The wavefront kernels' kKernel specialization constant, in
// shaders/wavefront/wavefront.slang.
enum WavefrontKernel : std::uint32_t {
  kGenerateKernel,
  kIntersectKernel,
  kShadeKernel,
  kAccumulateKernel,
  kPrepareKernel,
  kAdvanceKernel,
  kWavefrontKernels,
};
// A dispatch covers its invocations in groups of kWavefrontGroupSize, in
// rows of at most kWavefrontMaxGroupsX groups.
constexpr std::uint32_t kWavefrontGroupSize = 64;
constexpr std::uint32_t kWavefrontMaxGroupsX = 65535;
// The words of wavefront.slang's queue_state: counters, then three
// VkDispatchIndirectCommand, then a path count per bounce.
constexpr std::uint32_t kRayArgsWord = 8;
constexpr std::uint32_t kHitArgsWord = 12;
constexpr std::uint32_t kTerminatedArgsWord = 16;
constexpr std::uint32_t kPathCountsWord = 20;
constexpr std::uint32_t kQueueStateWords =
    kPathCountsWord + kMaxWavefrontBounces + 2;
// Two ray queues, the hit queue and the terminated-path queue.
constexpr VkDeviceSize kWavefrontQueues = 4;
// PathRecord's std430 stride in wavefront.slang.
constexpr VkDeviceSize kPathRecordBytes = 112;
// Timestamps: the scene pass's first and last (kSceneTimestamps), then the
// wavefront kernels': before generate, after it, after each bounce's
// intersect and shade, and after accumulate.
constexpr std::uint32_t kSceneTimestamps = 2;
constexpr std::uint32_t kRayTimestampQueries =
    kSceneTimestamps + 3 + 2 * (kMaxWavefrontBounces + 1);

// PathConstants::surface_output: the surface pass's diagnostics.
constexpr std::uint32_t kShadingNormalOutput = 0;
constexpr std::uint32_t kAlbedoOutput = 1;
constexpr std::uint32_t kRoughnessMetallicOutput = 2;

// The scene passes' push constants and descriptors reach the fragment
// passes and the wavefront kernels.
constexpr VkShaderStageFlags kRayStages =
    VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

// What one frame draws: the bootstrap triangle into the RGBA8 targets, or
// the scene into the RGBA32F targets, either the camera pass alone
// (barycentrics), the surface pass alone (a diagnostic of the hit's
// surface) or the camera pass's depth and then the radiance pass.
enum class FramePass {
  Bootstrap,
  Barycentrics,
  Surface,
  Radiance,
};

// How a pipeline differs from the others. A scene pipeline specializes the
// path tracer's kPass; the radiance pass leaves depth to the camera pass.
struct PipelineOptions {
  VkRenderPass render_pass = VK_NULL_HANDLE;
  std::optional<std::uint32_t> pass;
  bool color_write = true;
  bool depth = true;
};

// What a Radiance accumulation depends on. A frame with another key, or
// after the scene targets were recreated, restarts it.
struct AccumulationKey {
  Matrix4 world_to_clip{};
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::array<float, 4> display_window{};
  std::array<std::int32_t, 4> data_window{};
  Integrator integrator = Integrator::Reference;
  std::uint32_t sample_index = 0;
  std::uint32_t max_bounces = 0;
  std::uint64_t scene_generation = 0;

  bool operator==(const AccumulationKey&) const = default;
};

bool Invert(const Matrix4& matrix, Matrix4& inverse) {
  double rows[4][8]{};
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 4; ++column) {
      const float value = matrix[column * 4 + row];
      if (!std::isfinite(value))
        return false;
      rows[row][column] = value;
    }
    rows[row][row + 4] = 1.0;
  }
  for (int column = 0; column < 4; ++column) {
    int pivot = column;
    for (int row = column + 1; row < 4; ++row) {
      if (std::abs(rows[row][column]) > std::abs(rows[pivot][column]))
        pivot = row;
    }
    if (rows[pivot][column] == 0.0)
      return false;
    for (int index = 0; index < 8; ++index)
      std::swap(rows[column][index], rows[pivot][index]);
    const double scale = rows[column][column];
    for (double& value : rows[column])
      value /= scale;
    for (int row = 0; row < 4; ++row) {
      if (row == column)
        continue;
      const double factor = rows[row][column];
      for (int index = 0; index < 8; ++index)
        rows[row][index] -= factor * rows[column][index];
    }
  }
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 4; ++column) {
      const float value = static_cast<float>(rows[row][column + 4]);
      if (!std::isfinite(value))
        return false;
      inverse[column * 4 + row] = value;
    }
  }
  return true;
}

std::optional<std::uint32_t> FindGraphicsQueue(VkPhysicalDevice device) {
  std::uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
  std::vector<VkQueueFamilyProperties> properties(count);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &count, properties.data());
  // The wavefront kernels dispatch on the scene passes' queue.
  constexpr VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
  for (std::uint32_t index = 0; index < count; ++index) {
    if (properties[index].queueCount > 0 &&
        (properties[index].queueFlags & required) == required) {
      return index;
    }
  }
  return std::nullopt;
}

bool CreateImage(VkPhysicalDevice physical_device,
    VkDevice device,
    std::uint32_t width,
    std::uint32_t height,
    VkFormat format,
    VkImageUsageFlags usage,
    VkImage& image,
    VkDeviceMemory& memory,
    std::string& detail) {
  VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  create.imageType = VK_IMAGE_TYPE_2D;
  create.format = format;
  create.extent = {width, height, 1};
  create.mipLevels = 1;
  create.arrayLayers = 1;
  create.samples = VK_SAMPLE_COUNT_1_BIT;
  create.tiling = VK_IMAGE_TILING_OPTIMAL;
  create.usage = usage;
  create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!VulkanOk(vkCreateImage(device, &create, nullptr, &image),
          "vkCreateImage", detail)) {
    return false;
  }
  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(device, image, &requirements);
  const std::uint32_t memory_type =
      FindMemoryType(physical_device, requirements.memoryTypeBits,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
  if (memory_type == std::numeric_limits<std::uint32_t>::max()) {
    detail = "no device-local image memory type is available";
    return false;
  }
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = memory_type;
  if (!VulkanOk(vkAllocateMemory(device, &allocate, nullptr, &memory),
          "vkAllocateMemory(image)", detail) ||
      !VulkanOk(vkBindImageMemory(device, image, memory, 0),
          "vkBindImageMemory", detail)) {
    return false;
  }
  return true;
}

// A device-local buffer of its own allocation, for renderer state outside
// the scene's memory pools.
bool CreateDeviceBuffer(VkPhysicalDevice physical_device,
    VkDevice device,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkBuffer& buffer,
    VkDeviceMemory& memory,
    std::string& detail) {
  VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  create.size = size;
  create.usage = usage;
  create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (!VulkanOk(vkCreateBuffer(device, &create, nullptr, &buffer),
          "vkCreateBuffer(device)", detail)) {
    return false;
  }
  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(device, buffer, &requirements);
  const std::uint32_t memory_type =
      FindMemoryType(physical_device, requirements.memoryTypeBits,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
  if (memory_type == std::numeric_limits<std::uint32_t>::max()) {
    detail = "no device-local buffer memory type is available";
    return false;
  }
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = memory_type;
  return VulkanOk(vkAllocateMemory(device, &allocate, nullptr, &memory),
             "vkAllocateMemory(device buffer)", detail) &&
         VulkanOk(vkBindBufferMemory(device, buffer, memory, 0),
             "vkBindBufferMemory(device)", detail);
}

bool CreateHostBuffer(VkPhysicalDevice physical_device,
    VkDevice device,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkBuffer& buffer,
    VkDeviceMemory& memory,
    bool& coherent,
    std::string& detail) {
  VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  create.size = size;
  create.usage = usage;
  create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (!VulkanOk(vkCreateBuffer(device, &create, nullptr, &buffer),
          "vkCreateBuffer", detail)) {
    return false;
  }
  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(device, buffer, &requirements);
  const std::uint32_t memory_type = FindMemoryType(
      physical_device, requirements.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
      &coherent);
  if (memory_type == std::numeric_limits<std::uint32_t>::max()) {
    detail = "no host-visible buffer memory type is available";
    return false;
  }
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = memory_type;
  if (!VulkanOk(vkAllocateMemory(device, &allocate, nullptr, &memory),
          "vkAllocateMemory(host buffer)", detail) ||
      !VulkanOk(vkBindBufferMemory(device, buffer, memory, 0),
          "vkBindBufferMemory", detail)) {
    return false;
  }
  return true;
}

bool InvalidateIfNeeded(VkDevice device,
    VkDeviceMemory memory,
    bool coherent,
    std::string& detail) {
  if (coherent) {
    return true;
  }
  VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
  range.memory = memory;
  range.offset = 0;
  range.size = VK_WHOLE_SIZE;
  return VulkanOk(vkInvalidateMappedMemoryRanges(device, 1, &range),
      "vkInvalidateMappedMemoryRanges", detail);
}

VkViewport DisplayViewport(const OffscreenTarget& target) {
  const auto& window = target.display_window;
  if (window[2] <= 0.0F || window[3] <= 0.0F) {
    return {0.0F, 0.0F, static_cast<float>(target.width),
        static_cast<float>(target.height), 0.0F, 1.0F};
  }
  return {window[0], window[1], window[2], window[3], 0.0F, 1.0F};
}

VkRect2D DataScissor(const OffscreenTarget& target) {
  const auto& window = target.data_window;
  if (window[2] <= 0 || window[3] <= 0) {
    return {{0, 0}, {target.width, target.height}};
  }
  const std::int64_t x0 = std::max<std::int64_t>(window[0], 0);
  const std::int64_t y0 = std::max<std::int64_t>(window[1], 0);
  const std::int64_t x1 = std::min<std::int64_t>(
      std::int64_t{window[0]} + window[2], target.width);
  const std::int64_t y1 = std::min<std::int64_t>(
      std::int64_t{window[1]} + window[3], target.height);
  if (x1 <= x0 || y1 <= y0) {
    return {{0, 0}, {0, 0}};
  }
  return {{static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0)},
      {static_cast<std::uint32_t>(x1 - x0),
          static_cast<std::uint32_t>(y1 - y0)}};
}

// The images, framebuffer and persistently mapped readback buffers of one
// target size, for one colour format. The scene targets also hold the
// radiance accumulation image.
struct Targets {
  VkFormat color_format = VK_FORMAT_UNDEFINED;
  std::uint32_t color_pixel_bytes = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  VkImage accumulation_image = VK_NULL_HANDLE;
  VkDeviceMemory accumulation_memory = VK_NULL_HANDLE;
  VkImageView accumulation_view = VK_NULL_HANDLE;
  VkImage color_image = VK_NULL_HANDLE;
  VkDeviceMemory color_memory = VK_NULL_HANDLE;
  VkImageView color_view = VK_NULL_HANDLE;
  VkImage depth_image = VK_NULL_HANDLE;
  VkDeviceMemory depth_memory = VK_NULL_HANDLE;
  VkImageView depth_view = VK_NULL_HANDLE;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  VkBuffer color_readback = VK_NULL_HANDLE;
  VkDeviceMemory color_readback_memory = VK_NULL_HANDLE;
  bool color_readback_coherent = false;
  void* color_mapped = nullptr;
  VkBuffer depth_readback = VK_NULL_HANDLE;
  VkDeviceMemory depth_readback_memory = VK_NULL_HANDLE;
  bool depth_readback_coherent = false;
  void* depth_mapped = nullptr;
  // The wavefront integrator's state, created by its first frame at this
  // size: queue counters and indirect dispatches, the queues' path slots,
  // the paths, and a host copy of the counters.
  VkBuffer queue_state = VK_NULL_HANDLE;
  VkDeviceMemory queue_state_memory = VK_NULL_HANDLE;
  VkBuffer queue_items = VK_NULL_HANDLE;
  VkDeviceMemory queue_items_memory = VK_NULL_HANDLE;
  VkBuffer paths = VK_NULL_HANDLE;
  VkDeviceMemory paths_memory = VK_NULL_HANDLE;
  VkBuffer queue_readback = VK_NULL_HANDLE;
  VkDeviceMemory queue_readback_memory = VK_NULL_HANDLE;
  bool queue_readback_coherent = false;
  void* queue_mapped = nullptr;
  bool initialized = false;
};

class VulkanOffscreenRenderer final : public OffscreenRenderer {
public:
  ~VulkanOffscreenRenderer() override {
    if (device_ != VK_NULL_HANDLE) {
      vkDeviceWaitIdle(device_);
      DestroyTargets(targets_);
      DestroyTargets(scene_targets_);
      scene_.Destroy();
      vkDestroyFence(device_, fence_, nullptr);
      vkDestroyPipeline(device_, pipeline_, nullptr);
      vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
      vkDestroyPipeline(device_, camera_pipeline_, nullptr);
      vkDestroyPipeline(device_, depth_pipeline_, nullptr);
      vkDestroyPipeline(device_, surface_pipeline_, nullptr);
      vkDestroyPipeline(device_, radiance_pipeline_, nullptr);
      vkDestroyPipeline(device_, resolve_pipeline_, nullptr);
      for (VkPipeline pipeline : wavefront_pipelines_)
        vkDestroyPipeline(device_, pipeline, nullptr);
      vkDestroyPipelineLayout(device_, ray_layout_, nullptr);
      vkDestroyDescriptorPool(device_, ray_descriptor_pool_, nullptr);
      vkDestroyDescriptorSetLayout(device_, ray_descriptor_layout_, nullptr);
      // The layout's immutable samplers outlive it.
      for (VkSampler sampler : samplers_)
        vkDestroySampler(device_, sampler, nullptr);
      if (path_constants_mapped_ != nullptr)
        vkUnmapMemory(device_, path_constants_memory_);
      vkDestroyBuffer(device_, path_constants_, nullptr);
      vkFreeMemory(device_, path_constants_memory_, nullptr);
      vkDestroyQueryPool(device_, ray_timestamps_, nullptr);
      vkDestroyRenderPass(device_, render_pass_, nullptr);
      vkDestroyRenderPass(device_, scene_render_pass_, nullptr);
      vkDestroyCommandPool(device_, command_pool_, nullptr);
      vkDestroyDevice(device_, nullptr);
    }
    DestroyInstance(instance_state_);
  }

  bool Initialize(const std::string& vertex_shader,
      const std::string& fragment_shader, FrameStatus& status,
      std::string& detail, const RayQueryShaders& ray_shaders) {
    status = FrameStatus::Fail;
    std::vector<std::uint32_t> vertex_words;
    std::vector<std::uint32_t> fragment_words;
    if (!LoadSpirv(vertex_shader, vertex_words, detail) ||
        !LoadSpirv(fragment_shader, fragment_words, detail)) {
      return false;
    }
    if (!CreateInstanceWithValidation("lotus-headless", {}, &validation_,
            instance_state_, detail)) {
      status = FrameStatus::Skip;
      return false;
    }
    if (!SelectDevice(status, detail) || !CreateDevice(detail) ||
        !scene_.Initialize(physical_device_, device_, queue_, queue_family_,
            acceleration_, detail) ||
        !CreateRenderPass(kColorFormat, render_pass_, detail) ||
        !CreatePipelineLayout(false, pipeline_layout_, detail) ||
        !CreatePipeline(vertex_words, fragment_words, pipeline_layout_,
            {render_pass_}, pipeline_, detail)) {
      return false;
    }
    targets_.color_format = kColorFormat;
    targets_.color_pixel_bytes = 4;
    scene_targets_.color_format = kSceneColorFormat;
    scene_targets_.color_pixel_bytes = 16;
    if (ray_query_.available && !ray_shaders.vertex.empty() &&
        !ray_shaders.fragment.empty()) {
      // One shader module serves five pipelines: the camera pass with
      // and without colour writes, the surface pass, and the radiance and
      // resolve passes, which leave depth to the camera pass.
      if (!CreateRayResources(detail) ||
          !CreateRenderPass(kSceneColorFormat, scene_render_pass_, detail) ||
          !CreatePipelineLayout(true, ray_layout_, detail) ||
          !LoadSpirv(ray_shaders.vertex, vertex_words, detail) ||
          !LoadSpirv(ray_shaders.fragment, fragment_words, detail) ||
          !CreatePipeline(vertex_words, fragment_words, ray_layout_,
              {scene_render_pass_, kCameraPass}, camera_pipeline_, detail) ||
          !CreatePipeline(vertex_words, fragment_words, ray_layout_,
              {scene_render_pass_, kCameraPass, false}, depth_pipeline_,
              detail) ||
          !CreatePipeline(vertex_words, fragment_words, ray_layout_,
              {scene_render_pass_, kSurfacePass}, surface_pipeline_, detail) ||
          !CreatePipeline(vertex_words, fragment_words, ray_layout_,
              {scene_render_pass_, kRadiancePass, true, false},
              radiance_pipeline_, detail) ||
          !CreatePipeline(vertex_words, fragment_words, ray_layout_,
              {scene_render_pass_, kResolvePass, true, false},
              resolve_pipeline_, detail))
        return false;
      if (!ray_shaders.wavefront.empty()) {
        std::vector<std::uint32_t> compute_words;
        if (!LoadSpirv(ray_shaders.wavefront, compute_words, detail))
          return false;
        for (std::uint32_t kernel = 0; kernel < kWavefrontKernels; ++kernel) {
          if (!CreateComputePipeline(compute_words, kernel,
                  wavefront_pipelines_[kernel], detail))
            return false;
        }
      }
    }
    status = FrameStatus::Pass;
    return true;
  }

  GpuFrameEvidence Render(const DrawSummary& draw,
      const OffscreenTarget& target, std::uint32_t frame_count) override {
    return RenderFrame(draw, target, frame_count, FramePass::Bootstrap, {});
  }

  BackendCapability RayQueryCapability() const override {
    return ray_query_;
  }

  GpuFrameEvidence RenderScene(const DrawSummary& draw,
      const OffscreenTarget& target, std::uint32_t frame_count,
      const PathTracingSettings& settings) override {
    if (!failure_.empty())
      return Evidence(FrameStatus::Fail, "the renderer failed earlier: " + failure_);
    if (!ray_query_.available)
      return Evidence(FrameStatus::Skip, ray_query_.detail);
    if (camera_pipeline_ == VK_NULL_HANDLE)
      return Evidence(FrameStatus::Fail, "ray-query shader paths were not supplied");
    FramePass pass = FramePass::Radiance;
    switch (settings.output) {
    case SceneOutput::Radiance:
      if (settings.integrator == Integrator::Wavefront) {
        if (wavefront_pipelines_[0] == VK_NULL_HANDLE)
          return Evidence(FrameStatus::Fail,
              "the wavefront shader path was not supplied");
        if (settings.max_bounces > kMaxWavefrontBounces)
          return Evidence(FrameStatus::Fail,
              "the wavefront integrator traces at most " +
                  std::to_string(kMaxWavefrontBounces) + " bounces");
      } else if (settings.integrator != Integrator::Reference) {
        return Evidence(FrameStatus::Fail, "unknown integrator");
      }
      break;
    case SceneOutput::Barycentrics:
      pass = FramePass::Barycentrics;
      break;
    case SceneOutput::ShadingNormal:
    case SceneOutput::Albedo:
    case SceneOutput::RoughnessMetallic:
      pass = FramePass::Surface;
      break;
    default:
      return Evidence(FrameStatus::Fail, "unknown scene output");
    }
    ray_constants_.world_to_clip = VulkanWorldToClip(draw.world_to_clip);
    if (!Invert(ray_constants_.world_to_clip, ray_constants_.clip_to_world))
      return Evidence(FrameStatus::Fail, "ray-query camera is singular or non-finite");
    return RenderFrame(draw, target, frame_count, pass, settings);
  }

  GpuFrameEvidence RenderFrame(const DrawSummary& draw,
      const OffscreenTarget& target, std::uint32_t frame_count,
      FramePass pass, const PathTracingSettings& settings) {
    if (!failure_.empty()) {
      return Evidence(FrameStatus::Fail,
          "the renderer failed earlier: " + failure_);
    }
    const bool trace = pass != FramePass::Bootstrap;
    if (!trace && !((draw.draw_count == 1 && draw.triangle_count == 1) ||
                      (draw.draw_count == 0 && draw.triangle_count == 0))) {
      return Evidence(FrameStatus::Fail,
          "bootstrap extraction must be empty or produce one triangle draw");
    }
    if (!std::isfinite(target.clear_depth) || target.clear_depth < 0.0F ||
        target.clear_depth > 1.0F ||
        !std::all_of(target.clear_color.begin(), target.clear_color.end(),
            [](float value) { return std::isfinite(value); })) {
      return Evidence(FrameStatus::Fail, "invalid offscreen clear values");
    }
    if (frame_count == 0) {
      return Evidence(FrameStatus::Fail, "frame_count must be at least 1");
    }
    if (target.width == 0 || target.height == 0) {
      return Evidence(FrameStatus::Fail, "the offscreen target is empty");
    }
    const VkPhysicalDeviceLimits& limits = device_properties_.limits;
    if (target.width > std::min(limits.maxImageDimension2D,
                           limits.maxFramebufferWidth) ||
        target.height > std::min(limits.maxImageDimension2D,
                            limits.maxFramebufferHeight)) {
      std::ostringstream message;
      message << "the offscreen target " << target.width << 'x'
              << target.height << " exceeds the device's framebuffer limits";
      return Evidence(FrameStatus::Fail, message.str());
    }

    Targets& targets = trace ? scene_targets_ : targets_;
    const std::size_t pixels = std::size_t{target.width} * target.height;
    if ((!target.preserved_color.empty() &&
            target.preserved_color.size() != pixels * 4) ||
        (!target.preserved_depth.empty() &&
            target.preserved_depth.size() != pixels)) {
      return Evidence(FrameStatus::Fail, "invalid preserved attachment extent");
    }
    std::string detail;
    if (!EnsureTargets(targets, target.width, target.height, detail)) {
      return Evidence(FrameStatus::Fail, detail);
    }
    if (!StagePreservedContents(target, targets, detail)) {
      return Evidence(FrameStatus::Fail, detail);
    }
    // A Radiance frame continues the accumulation when nothing it depends
    // on has changed, and adds at most `max_samples` in all. Once there is
    // nothing to add, one submission writes the accumulated mean again.
    std::uint32_t submissions = frame_count;
    std::uint32_t samples_to_add = 0;
    if (pass == FramePass::Radiance) {
      const AccumulationKey key{ray_constants_.world_to_clip, target.width,
          target.height, target.display_window, target.data_window,
          settings.integrator, settings.sample_index, settings.max_bounces,
          scene_generation_};
      if (accumulation_key_ != key) {
        accumulation_key_ = key;
        accumulated_samples_ = 0;
      }
      const std::uint32_t room =
          settings.max_samples == 0
              ? frame_count
              : settings.max_samples -
                    std::min(settings.max_samples, accumulated_samples_);
      samples_to_add = std::min(frame_count, room);
      submissions = std::max(samples_to_add, 1U);
    }
    // Bounces the wavefront kernels are recorded for: a path's last
    // scattering event is at depth max_bounces.
    const std::uint32_t wavefront_iterations =
        pass == FramePass::Radiance &&
                settings.integrator == Integrator::Wavefront
            ? settings.max_bounces + 1
            : 0;
    if (wavefront_iterations != 0 && !EnsureWavefront(targets, detail)) {
      return Evidence(FrameStatus::Fail, detail);
    }
    if (trace) {
      UpdateSceneDescriptors(targets);
    }
    bool initializing = !targets.initialized;
    if (!Record(draw, target, targets, pass, wavefront_iterations, true,
            detail)) {
      return Evidence(FrameStatus::Fail, detail);
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_;
    for (std::uint32_t frame = 0; frame < submissions; ++frame) {
      const bool add_sample = frame < samples_to_add;
      if (trace && !WritePathConstants(target, settings, add_sample, detail)) {
        return Evidence(FrameStatus::Fail, detail);
      }
      // A failure here may leave the command buffer pending, so it can never
      // be recorded again.
      if (!VulkanOk(vkResetFences(device_, 1, &fence_), "vkResetFences",
              detail) ||
          !VulkanOk(vkQueueSubmit(queue_, 1, &submit, fence_),
              "vkQueueSubmit", detail) ||
          !VulkanOk(vkWaitForFences(device_, 1, &fence_, VK_TRUE,
                        10'000'000'000ULL),
              "vkWaitForFences", detail)) {
        failure_ = detail;
        return Evidence(FrameStatus::Fail, detail);
      }
      ++completion_;
      if (add_sample) {
        ++accumulated_samples_;
      }
      targets.initialized = true;
      // The first recording gives fresh images their layouts, which would
      // discard the accumulation if it were submitted again.
      const bool restored = frame == 0 &&
          ((!target.clear_color_enabled && !target.preserved_color.empty()) ||
              (!target.clear_depth_enabled && !target.preserved_depth.empty()));
      if ((initializing || restored) && frame + 1 < submissions) {
        initializing = false;
        if (!Record(draw, target, targets, pass, wavefront_iterations, false,
                detail)) {
          return Evidence(FrameStatus::Fail, detail);
        }
      }
    }
    if (!InvalidateIfNeeded(device_, targets.color_readback_memory,
            targets.color_readback_coherent, detail) ||
        !InvalidateIfNeeded(device_, targets.depth_readback_memory,
            targets.depth_readback_coherent, detail)) {
      return Evidence(FrameStatus::Fail, detail);
    }

    const std::uint32_t width = targets.width;
    const std::uint32_t height = targets.height;
    const std::size_t pixel_count = std::size_t{width} * height;
    GpuFrameEvidence evidence = Evidence(FrameStatus::Pass, "");
    evidence.ray_query_used = trace;
    evidence.completion = completion_;
    evidence.frames_rendered = submissions;
    evidence.target_creations = target_creations_;
    evidence.samples_per_pixel =
        pass == FramePass::Radiance ? accumulated_samples_ : 0;
    evidence.validation_available = instance_state_.validation_available;
    evidence.synchronization_validation_available =
        instance_state_.synchronization_validation_available;
    evidence.synchronization_validation_detail =
        instance_state_.synchronization_validation_detail;
    evidence.validation_message_count = validation_.message_count;
    evidence.validation_detail = validation_.first_message.empty()
                                     ? instance_state_.validation_detail
                                     : validation_.first_message;
    evidence.color.width = width;
    evidence.color.height = height;
    evidence.color.row_pitch = width * targets.color_pixel_bytes;
    evidence.color.pixel_format = trace ? "rgba32-sfloat" : "rgba8-unorm";
    evidence.color.origin = "top-left";
    evidence.color.color_space = "linear";
    evidence.color.payload.resize(pixel_count * targets.color_pixel_bytes);
    std::memcpy(evidence.color.payload.data(), targets.color_mapped,
        evidence.color.payload.size());
    evidence.depth.width = width;
    evidence.depth.height = height;
    evidence.depth.row_pitch = width * sizeof(float);
    evidence.depth.pixel_format = "d32-sfloat";
    evidence.depth.origin = "top-left";
    evidence.depth.payload.resize(pixel_count);
    std::memcpy(evidence.depth.payload.data(), targets.depth_mapped,
        pixel_count * sizeof(float));

    if (trace && ray_timestamps_ != VK_NULL_HANDLE) {
      std::uint64_t ticks[2]{};
      if (!VulkanOk(vkGetQueryPoolResults(device_, ray_timestamps_, 0, 2,
                        sizeof(ticks), ticks, sizeof(std::uint64_t),
                        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
              "vkGetQueryPoolResults(primary ray)", detail)) {
        failure_ = detail;
        return Evidence(FrameStatus::Fail, detail);
      }
      const std::uint64_t mask = timestamp_bits_ == 64 ? ~std::uint64_t{0}
                                                       : (std::uint64_t{1} << timestamp_bits_) - 1;
      evidence.primary_ray_timestamp_available = true;
      evidence.primary_ray_gpu_ms = static_cast<double>((ticks[1] - ticks[0]) & mask) * device_properties_.limits.timestampPeriod / 1'000'000.0;
    }
    // The kernels ran only when the frame traced something; they add
    // nothing when the accumulation was full.
    if (recorded_wavefront_iterations_ != 0 && samples_to_add != 0) {
      if (!InvalidateIfNeeded(device_, targets.queue_readback_memory,
              targets.queue_readback_coherent, detail)) {
        return Evidence(FrameStatus::Fail, detail);
      }
      const auto* words = static_cast<const std::uint32_t*>(targets.queue_mapped);
      std::vector<std::uint32_t>& counts = evidence.wavefront_path_counts;
      counts.assign(words + kPathCountsWord,
          words + kPathCountsWord + recorded_wavefront_iterations_);
      while (!counts.empty() && counts.back() == 0)
        counts.pop_back();
      if (ray_timestamps_ != VK_NULL_HANDLE &&
          !ReadWavefrontTimings(evidence.wavefront_timings, detail)) {
        failure_ = detail;
        return Evidence(FrameStatus::Fail, detail);
      }
    }

    evidence.device_name = device_properties_.deviceName;
    evidence.vendor_id = device_properties_.vendorID;
    evidence.device_id = device_properties_.deviceID;
    evidence.driver_version = std::to_string(device_properties_.driverVersion);
    std::ostringstream api_version;
    api_version << VK_API_VERSION_MAJOR(device_properties_.apiVersion) << '.'
                << VK_API_VERSION_MINOR(device_properties_.apiVersion) << '.'
                << VK_API_VERSION_PATCH(device_properties_.apiVersion);
    evidence.api_version = api_version.str();
    std::ostringstream success;
    success << "rendered " << submissions << " deterministic frames on "
            << device_properties_.deviceName;
    evidence.detail = success.str();
    return evidence;
  }

  // Reuse persistent readback buffers as upload staging while no frame is
  // in flight. The restoration copy finishes before readback overwrites them.
  bool StagePreservedContents(const OffscreenTarget& target, Targets& targets,
      std::string& detail) {
    auto flush = [&](VkDeviceMemory memory, bool coherent) {
      if (coherent)
        return true;
      VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
      range.memory = memory;
      range.size = VK_WHOLE_SIZE;
      return VulkanOk(vkFlushMappedMemoryRanges(device_, 1, &range),
          "vkFlushMappedMemoryRanges(preserved contents)", detail);
    };
    if (!target.clear_color_enabled && !target.preserved_color.empty()) {
      if (targets.color_format == kSceneColorFormat) {
        std::memcpy(targets.color_mapped, target.preserved_color.data(),
            target.preserved_color.size() * sizeof(float));
      } else {
        auto* bytes = static_cast<std::uint8_t*>(targets.color_mapped);
        for (std::size_t i = 0; i < target.preserved_color.size(); ++i) {
          const float value = target.preserved_color[i];
          bytes[i] = std::isfinite(value) ? static_cast<std::uint8_t>(
              std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F)) : 0;
        }
      }
      if (!flush(targets.color_readback_memory, targets.color_readback_coherent))
        return false;
    }
    if (!target.clear_depth_enabled && !target.preserved_depth.empty()) {
      std::memcpy(targets.depth_mapped, target.preserved_depth.data(),
          target.preserved_depth.size() * sizeof(float));
      if (!flush(targets.depth_readback_memory, targets.depth_readback_coherent))
        return false;
    }
    return true;
  }

  // Called after the preceding synchronous frame or submission has
  // finished, so the block can be overwritten.
  bool WritePathConstants(const OffscreenTarget& target,
      const PathTracingSettings& settings, bool add_sample,
      std::string& detail) {
    PathConstants constants{};
    const std::array<float, 3>& environment = scene_.Environment();
    std::copy(environment.begin(), environment.end(), constants.environment);
    std::copy(target.clear_color.begin(), target.clear_color.end(),
        constants.background);
    const VkViewport viewport = DisplayViewport(target);
    constants.ndc_from_pixel[0] = 2.0F / viewport.width;
    constants.ndc_from_pixel[1] = 2.0F / viewport.height;
    constants.ndc_from_pixel[2] = -2.0F * viewport.x / viewport.width - 1.0F;
    constants.ndc_from_pixel[3] = -2.0F * viewport.y / viewport.height - 1.0F;
    constants.instances = scene_.InstanceAddress();
    constants.materials = scene_.MaterialAddress();
    constants.sample_index = settings.sample_index + accumulated_samples_;
    constants.accumulated = accumulated_samples_;
    constants.add_sample = add_sample ? 1U : 0U;
    constants.max_bounces = settings.max_bounces;
    constants.width = target.width;
    constants.path_capacity = target.width * target.height;
    const VkRect2D scissor = DataScissor(target);
    constants.data_rect[0] = static_cast<std::uint32_t>(scissor.offset.x);
    constants.data_rect[1] = static_cast<std::uint32_t>(scissor.offset.y);
    constants.data_rect[2] = scissor.extent.width;
    constants.data_rect[3] = scissor.extent.height;
    constants.surface_output =
        settings.output == SceneOutput::Albedo ? kAlbedoOutput
        : settings.output == SceneOutput::RoughnessMetallic
            ? kRoughnessMetallicOutput
            : kShadingNormalOutput;
    std::memcpy(path_constants_mapped_, &constants, sizeof(constants));
    if (path_constants_coherent_) {
      return true;
    }
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = path_constants_memory_;
    range.size = VK_WHOLE_SIZE;
    return VulkanOk(vkFlushMappedMemoryRanges(device_, 1, &range),
        "vkFlushMappedMemoryRanges(path constants)", detail);
  }

  // UpdateScene can replace the TLAS and the textures, and EnsureTargets the
  // accumulation image. They are written after the preceding synchronous
  // frame and scene update have finished. A released texture's descriptor
  // stays as it was: the binding is partially bound, and no material
  // samples it.
  void UpdateSceneDescriptors(const Targets& targets) {
    if (scene_.TextureGeneration() != texture_generation_) {
      const std::vector<VkImageView> views = scene_.TextureViews();
      std::vector<VkDescriptorImageInfo> images;
      std::vector<VkWriteDescriptorSet> texture_writes;
      images.reserve(views.size());
      for (std::size_t slot = 0; slot < views.size(); ++slot) {
        if (views[slot] == VK_NULL_HANDLE)
          continue;
        images.push_back({VK_NULL_HANDLE, views[slot],
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
        VkWriteDescriptorSet& write = texture_writes.emplace_back();
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = ray_descriptor_;
        write.dstBinding = 3;
        write.dstArrayElement = static_cast<std::uint32_t>(slot);
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.pImageInfo = &images.back();
      }
      vkUpdateDescriptorSets(device_,
          static_cast<std::uint32_t>(texture_writes.size()),
          texture_writes.data(), 0, nullptr);
      texture_generation_ = scene_.TextureGeneration();
    }
    VkWriteDescriptorSet writes[5]{};
    std::uint32_t count = 0;
    const VkAccelerationStructureKHR tlas = scene_.Tlas();
    VkWriteDescriptorSetAccelerationStructureKHR acceleration_write{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    acceleration_write.accelerationStructureCount = 1;
    acceleration_write.pAccelerationStructures = &tlas;
    if (tlas != VK_NULL_HANDLE) {
      VkWriteDescriptorSet& write = writes[count++];
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.pNext = &acceleration_write;
      write.dstSet = ray_descriptor_;
      write.dstBinding = 0;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    }
    const VkDescriptorImageInfo accumulation{VK_NULL_HANDLE,
        targets.accumulation_view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet& write = writes[count++];
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = ray_descriptor_;
    write.dstBinding = 2;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &accumulation;
    // Only the wavefront kernels use the queues, and only after their
    // first frame at this size has created them.
    const VkDescriptorBufferInfo queues[3] = {
        {targets.queue_state, 0, VK_WHOLE_SIZE},
        {targets.queue_items, 0, VK_WHOLE_SIZE},
        {targets.paths, 0, VK_WHOLE_SIZE}};
    if (targets.queue_state != VK_NULL_HANDLE) {
      for (std::uint32_t index = 0; index < 3; ++index) {
        VkWriteDescriptorSet& queue = writes[count++];
        queue.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        queue.dstSet = ray_descriptor_;
        queue.dstBinding = 5 + index;
        queue.descriptorCount = 1;
        queue.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        queue.pBufferInfo = &queues[index];
      }
    }
    vkUpdateDescriptorSets(device_, count, writes, 0, nullptr);
  }

  // The wavefront kernels' buffers for the scene targets' size: one path
  // slot per pixel. Kept until the targets are replaced.
  bool EnsureWavefront(Targets& targets, std::string& detail) {
    if (targets.queue_state != VK_NULL_HANDLE)
      return true;
    const VkDeviceSize slots = VkDeviceSize{targets.width} * targets.height;
    const VkDeviceSize state_bytes = kQueueStateWords * sizeof(std::uint32_t);
    if (CreateDeviceBuffer(physical_device_, device_, state_bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            targets.queue_state, targets.queue_state_memory, detail) &&
        CreateDeviceBuffer(physical_device_, device_,
            kWavefrontQueues * slots * sizeof(std::uint32_t),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, targets.queue_items,
            targets.queue_items_memory, detail) &&
        CreateDeviceBuffer(physical_device_, device_, slots * kPathRecordBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, targets.paths,
            targets.paths_memory, detail) &&
        CreateHostBuffer(physical_device_, device_, state_bytes,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT, targets.queue_readback,
            targets.queue_readback_memory, targets.queue_readback_coherent,
            detail) &&
        VulkanOk(vkMapMemory(device_, targets.queue_readback_memory, 0,
                     VK_WHOLE_SIZE, 0, &targets.queue_mapped),
            "vkMapMemory(queue state)", detail))
      return true;
    DestroyWavefront(targets);
    return false;
  }

  void DestroyWavefront(Targets& t) {
    if (t.queue_mapped != nullptr)
      vkUnmapMemory(device_, t.queue_readback_memory);
    vkDestroyBuffer(device_, t.queue_readback, nullptr);
    vkFreeMemory(device_, t.queue_readback_memory, nullptr);
    vkDestroyBuffer(device_, t.paths, nullptr);
    vkFreeMemory(device_, t.paths_memory, nullptr);
    vkDestroyBuffer(device_, t.queue_items, nullptr);
    vkFreeMemory(device_, t.queue_items_memory, nullptr);
    vkDestroyBuffer(device_, t.queue_state, nullptr);
    vkFreeMemory(device_, t.queue_state_memory, nullptr);
    t.queue_mapped = nullptr;
    t.queue_readback = t.paths = t.queue_items = t.queue_state = VK_NULL_HANDLE;
    t.queue_readback_memory = t.paths_memory = t.queue_items_memory =
        t.queue_state_memory = VK_NULL_HANDLE;
  }

  // Sums the last submission's wavefront timestamps by kernel.
  bool ReadWavefrontTimings(WavefrontTimings& timings, std::string& detail) {
    const std::uint32_t queries = 3 + 2 * recorded_wavefront_iterations_;
    std::vector<std::uint64_t> ticks(queries);
    if (!VulkanOk(vkGetQueryPoolResults(device_, ray_timestamps_,
                      kSceneTimestamps, queries,
                      ticks.size() * sizeof(std::uint64_t), ticks.data(),
                      sizeof(std::uint64_t),
                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
            "vkGetQueryPoolResults(wavefront)", detail))
      return false;
    const std::uint64_t mask = timestamp_bits_ == 64
                                   ? ~std::uint64_t{0}
                                   : (std::uint64_t{1} << timestamp_bits_) - 1;
    const auto ms = [&](std::uint32_t from) {
      return static_cast<double>((ticks[from + 1] - ticks[from]) & mask) *
             device_properties_.limits.timestampPeriod / 1'000'000.0;
    };
    timings = {};
    timings.available = true;
    timings.generate_gpu_ms = ms(0);
    for (std::uint32_t bounce = 0; bounce < recorded_wavefront_iterations_;
        ++bounce) {
      timings.intersect_gpu_ms += ms(1 + 2 * bounce);
      timings.shade_gpu_ms += ms(2 + 2 * bounce);
    }
    timings.accumulate_gpu_ms = ms(queries - 2);
    return true;
  }

  GpuSceneEvidence UpdateScene(const SceneUpdate& update) override {
    GpuSceneEvidence evidence;
    // Any change to the scene restarts the radiance accumulation.
    if (!update.Empty()) {
      ++scene_generation_;
    }
    if (!failure_.empty()) {
      evidence.status = FrameStatus::Fail;
      evidence.detail = "the renderer failed earlier: " + failure_;
    } else if (std::string detail; !scene_.Apply(update, detail)) {
      // A partly applied plan leaves the scene unknown to the extraction.
      failure_ = detail;
      evidence.status = FrameStatus::Fail;
      evidence.detail = detail;
    } else {
      evidence.status = FrameStatus::Pass;
      evidence.timings = scene_.Timings();
    }
    evidence.stats = scene_.Stats();
    evidence.validation_message_count = validation_.message_count;
    return evidence;
  }

  GpuSceneContents ReadBackScene() override {
    GpuSceneContents contents;
    if (!failure_.empty()) {
      contents.status = FrameStatus::Fail;
      contents.detail = "the renderer failed earlier: " + failure_;
    } else if (std::string detail; !scene_.ReadBack(contents, detail)) {
      failure_ = detail;
      contents.status = FrameStatus::Fail;
      contents.detail = detail;
    }
    return contents;
  }

private:
  bool SelectDevice(FrameStatus& status, std::string& detail) {
    const VkInstance instance = instance_state_.instance;
    std::uint32_t physical_count = 0;
    if (!VulkanOk(vkEnumeratePhysicalDevices(instance, &physical_count,
                      nullptr),
            "vkEnumeratePhysicalDevices", detail)) {
      return false;
    }
    if (physical_count == 0) {
      status = FrameStatus::Skip;
      detail = "no Vulkan physical device is available";
      return false;
    }
    std::vector<VkPhysicalDevice> physical_devices(physical_count);
    vkEnumeratePhysicalDevices(instance, &physical_count,
        physical_devices.data());
    for (VkPhysicalDevice physical : physical_devices) {
      const auto candidate = FindGraphicsQueue(physical);
      if (candidate) {
        physical_device_ = physical;
        queue_family_ = *candidate;
        break;
      }
    }
    if (physical_device_ == VK_NULL_HANDLE) {
      status = FrameStatus::Skip;
      detail = "no Vulkan physical device exposes a graphics queue";
      return false;
    }

    if (!SupportsShaderDrawParameters(physical_device_)) {
      status = FrameStatus::Skip;
      detail = "the device does not support shaderDrawParameters, which "
               "the Slang vertex-index lowering requires";
      return false;
    }

    VkFormatProperties color_properties{};
    VkFormatProperties depth_properties{};
    vkGetPhysicalDeviceFormatProperties(physical_device_, kColorFormat,
        &color_properties);
    vkGetPhysicalDeviceFormatProperties(physical_device_, kDepthFormat,
        &depth_properties);
    const VkFormatFeatureFlags color_required =
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    const VkFormatFeatureFlags depth_required =
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((color_properties.optimalTilingFeatures & color_required) !=
            color_required ||
        (depth_properties.optimalTilingFeatures & depth_required) !=
            depth_required) {
      status = FrameStatus::Skip;
      detail = "required RGBA8/depth32 attachment readback formats are "
               "unavailable";
      return false;
    }
    vkGetPhysicalDeviceProperties(physical_device_, &device_properties_);
    // Acceleration structures are optional: without them the GPU scene
    // still uploads its buffers and reports why it builds no BLAS or TLAS.
    acceleration_ = ProbeAccelerationStructures(physical_device_);
    ray_query_ = vulkan_internal::ProbeRayQueries(physical_device_, acceleration_);
    // The scene passes write RGBA32F, the radiance pass accumulates into a
    // storage image from the fragment stage, and the instance records hold
    // 64-bit normal addresses.
    if (ray_query_.available) {
      VkPhysicalDeviceFeatures features{};
      vkGetPhysicalDeviceFeatures(physical_device_, &features);
      VkFormatProperties scene_color{};
      vkGetPhysicalDeviceFormatProperties(physical_device_, kSceneColorFormat,
          &scene_color);
      const VkFormatFeatureFlags scene_required =
          VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
          VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
          VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
      if (features.fragmentStoresAndAtomics != VK_TRUE) {
        ray_query_ = {false, "the device does not support "
                             "fragmentStoresAndAtomics, which radiance "
                             "accumulation requires"};
      } else if (features.shaderInt64 != VK_TRUE) {
        ray_query_ = {false, "the device does not support shaderInt64, which "
                             "the instance records' normal addresses "
                             "require"};
      } else if ((scene_color.optimalTilingFeatures & scene_required) !=
                 scene_required) {
        ray_query_ = {false, "RGBA32F colour attachments with storage and "
                             "readback are unavailable"};
      } else if (std::string reason; !SupportsTextures(reason)) {
        ray_query_ = {false, reason};
      }
    }
    std::uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_count, queues.data());
    timestamp_bits_ = queues[queue_family_].timestampValidBits;
    return true;
  }

  // The scene passes sample the GPU scene's texture table: a partially bound
  // array of kMaxTextures sampled images and kTextureSamplers samplers,
  // indexed per hit, with bilinear filtering of every texture format.
  bool SupportsTextures(std::string& reason) const {
    VkPhysicalDeviceVulkan12Features vulkan12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &vulkan12;
    vkGetPhysicalDeviceFeatures2(physical_device_, &features);
    if (vulkan12.descriptorBindingPartiallyBound != VK_TRUE ||
        vulkan12.shaderSampledImageArrayNonUniformIndexing != VK_TRUE) {
      reason = "the device does not support descriptorBindingPartiallyBound "
               "and shaderSampledImageArrayNonUniformIndexing, which the "
               "texture table requires";
      return false;
    }
    const VkPhysicalDeviceLimits& limits = device_properties_.limits;
    if (limits.maxPerStageDescriptorSampledImages < kMaxTextures ||
        limits.maxDescriptorSetSampledImages < kMaxTextures ||
        limits.maxPerStageDescriptorSamplers < kTextureSamplers ||
        limits.maxDescriptorSetSamplers < kTextureSamplers ||
        limits.maxPerStageResources < kMaxTextures + kTextureSamplers + 6) {
      std::ostringstream message;
      message << "the device's descriptor limits do not allow a table of "
              << kMaxTextures << " sampled textures";
      reason = message.str();
      return false;
    }
    const VkFormatFeatureFlags sampled =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    for (const VkFormat format : {VK_FORMAT_R8G8B8A8_UNORM,
             VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R32G32B32A32_SFLOAT}) {
      VkFormatProperties properties{};
      vkGetPhysicalDeviceFormatProperties(physical_device_, format,
          &properties);
      if ((properties.optimalTilingFeatures & sampled) != sampled) {
        reason = "linearly filtered RGBA8 and RGBA32F textures with uploads "
                 "and readback are unavailable";
        return false;
      }
    }
    return true;
  }

  bool CreateDevice(std::string& detail) {
    const float priority = 1.0F;
    VkDeviceQueueCreateInfo queue_create{
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_create.queueFamilyIndex = queue_family_;
    queue_create.queueCount = 1;
    queue_create.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan11Features enabled_vulkan11{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    enabled_vulkan11.shaderDrawParameters = VK_TRUE;
    VkPhysicalDeviceVulkan12Features enabled_vulkan12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    enabled_vulkan12.bufferDeviceAddress = VK_TRUE;
    // The texture table; SupportsTextures checked both.
    enabled_vulkan12.descriptorBindingPartiallyBound =
        ray_query_.available ? VK_TRUE : VK_FALSE;
    enabled_vulkan12.shaderSampledImageArrayNonUniformIndexing =
        ray_query_.available ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR enabled_acceleration{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    enabled_acceleration.accelerationStructure = VK_TRUE;
    VkPhysicalDeviceRayQueryFeaturesKHR enabled_query{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    enabled_query.rayQuery = VK_TRUE;
    VkPhysicalDeviceFeatures enabled_features{};
    enabled_features.fragmentStoresAndAtomics =
        ray_query_.available ? VK_TRUE : VK_FALSE;
    enabled_features.shaderInt64 = ray_query_.available ? VK_TRUE : VK_FALSE;
    std::vector<const char*> extensions;
    VkDeviceCreateInfo device_create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_create.pNext = &enabled_vulkan11;
    device_create.pEnabledFeatures = &enabled_features;
    if (acceleration_.available) {
      enabled_vulkan11.pNext = &enabled_vulkan12;
      enabled_vulkan12.pNext = &enabled_acceleration;
      extensions.assign(kAccelerationExtensions.begin(), kAccelerationExtensions.end());
      if (ray_query_.available) {
        enabled_acceleration.pNext = &enabled_query;
        extensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
      }
      device_create.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
      device_create.ppEnabledExtensionNames = extensions.data();
    }
    device_create.queueCreateInfoCount = 1;
    device_create.pQueueCreateInfos = &queue_create;
    if (!VulkanOk(vkCreateDevice(physical_device_, &device_create, nullptr,
                      &device_),
            "vkCreateDevice", detail)) {
      return false;
    }
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    // Each frame re-records the one command buffer, since the camera and the
    // target's windows change between frames.
    VkCommandPoolCreateInfo pool_create{
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_create.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_create.queueFamilyIndex = queue_family_;
    if (!VulkanOk(vkCreateCommandPool(device_, &pool_create, nullptr,
                      &command_pool_),
            "vkCreateCommandPool", detail)) {
      return false;
    }
    VkCommandBufferAllocateInfo command_allocate{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_allocate.commandPool = command_pool_;
    command_allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_allocate.commandBufferCount = 1;
    if (!VulkanOk(vkAllocateCommandBuffers(device_, &command_allocate,
                      &command_),
            "vkAllocateCommandBuffers", detail)) {
      return false;
    }
    VkFenceCreateInfo fence_create{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    return VulkanOk(vkCreateFence(device_, &fence_create, nullptr, &fence_),
        "vkCreateFence", detail);
  }

  bool CreateRenderPass(VkFormat color_format, VkRenderPass& render_pass,
      std::string& detail) {
    VkAttachmentDescription attachments[2]{};
    attachments[0].format = color_format;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    attachments[1].format = kDepthFormat;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference color_reference{0,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_reference{1,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_reference;
    subpass.pDepthStencilAttachment = &depth_reference;
    // The first dependency also orders this frame's attachment writes after
    // the previous frame's readback copy of the same images.
    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dependencies[0].dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].srcAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo render_pass_create{
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    render_pass_create.attachmentCount = 2;
    render_pass_create.pAttachments = attachments;
    render_pass_create.subpassCount = 1;
    render_pass_create.pSubpasses = &subpass;
    render_pass_create.dependencyCount = 2;
    render_pass_create.pDependencies = dependencies;
    return VulkanOk(vkCreateRenderPass(device_, &render_pass_create, nullptr,
                        &render_pass),
        "vkCreateRenderPass", detail);
  }

  bool CreateRayResources(std::string& detail) {
    // Bilinear, without mipmaps. TextureWrap order: black, clamp, repeat,
    // mirror; black is the transparent black border.
    constexpr VkSamplerAddressMode kAddress[] = {
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT};
    for (std::uint32_t index = 0; index < kTextureSamplers; ++index) {
      VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
      sampler.magFilter = VK_FILTER_LINEAR;
      sampler.minFilter = VK_FILTER_LINEAR;
      sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
      sampler.addressModeU = kAddress[index / 4];
      sampler.addressModeV = kAddress[index % 4];
      sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      sampler.maxLod = 0.0F;
      sampler.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
      if (!VulkanOk(vkCreateSampler(device_, &sampler, nullptr,
                        &samplers_[index]),
              "vkCreateSampler", detail))
        return false;
    }
    VkDescriptorSetLayoutBinding bindings[8]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = kRayStages;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = kRayStages;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = kRayStages;
    // The texture table, written as textures become resident, and its
    // samplers.
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[3].descriptorCount = kMaxTextures;
    bindings[3].stageFlags = kRayStages;
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[4].descriptorCount = kTextureSamplers;
    bindings[4].stageFlags = kRayStages;
    bindings[4].pImmutableSamplers = samplers_.data();
    // The wavefront kernels' queue state, queues and paths.
    for (std::uint32_t index = 5; index < 8; ++index) {
      bindings[index].binding = index;
      bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      bindings[index].descriptorCount = 1;
      bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    const VkDescriptorBindingFlags binding_flags[8] = {0, 0, 0,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, 0, 0, 0, 0};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flags.bindingCount = 8;
    flags.pBindingFlags = binding_flags;
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.pNext = &flags;
    layout.bindingCount = 8;
    layout.pBindings = bindings;
    if (!VulkanOk(vkCreateDescriptorSetLayout(device_, &layout, nullptr,
                      &ray_descriptor_layout_),
            "vkCreateDescriptorSetLayout(ray)", detail))
      return false;
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxTextures},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kTextureSamplers},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = 6;
    pool.pPoolSizes = sizes;
    if (!VulkanOk(vkCreateDescriptorPool(device_, &pool, nullptr,
                      &ray_descriptor_pool_),
            "vkCreateDescriptorPool(ray)", detail))
      return false;
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = ray_descriptor_pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &ray_descriptor_layout_;
    if (!VulkanOk(vkAllocateDescriptorSets(device_, &allocate, &ray_descriptor_),
            "vkAllocateDescriptorSets(ray)", detail))
      return false;
    // One frame is in flight, so one persistently mapped block serves them
    // all. RenderScene writes it before recording.
    if (!CreateHostBuffer(physical_device_, device_, sizeof(PathConstants),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, path_constants_,
            path_constants_memory_, path_constants_coherent_, detail) ||
        !VulkanOk(vkMapMemory(device_, path_constants_memory_, 0,
                      sizeof(PathConstants), 0, &path_constants_mapped_),
            "vkMapMemory(path constants)", detail))
      return false;
    const VkDescriptorBufferInfo constants{path_constants_, 0, sizeof(PathConstants)};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = ray_descriptor_;
    write.dstBinding = 1;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &constants;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    if (timestamp_bits_ != 0) {
      VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      query.queryType = VK_QUERY_TYPE_TIMESTAMP;
      query.queryCount = kRayTimestampQueries;
      if (!VulkanOk(vkCreateQueryPool(device_, &query, nullptr, &ray_timestamps_),
              "vkCreateQueryPool(primary ray)", detail))
        return false;
    }
    return true;
  }

  // The bootstrap layout takes world-to-clip in the vertex stage; the scene
  // layout takes RayConstants and the scene descriptor set in the fragment
  // and compute stages.
  bool CreatePipelineLayout(bool trace, VkPipelineLayout& layout,
      std::string& detail) {
    const VkPushConstantRange push_range{
        static_cast<VkShaderStageFlags>(trace ? kRayStages
                                              : VK_SHADER_STAGE_VERTEX_BIT),
        0,
        trace ? static_cast<std::uint32_t>(sizeof(RayConstants)) : kFrameConstantsSize};
    VkPipelineLayoutCreateInfo layout_create{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_create.pushConstantRangeCount = 1;
    layout_create.pPushConstantRanges = &push_range;
    if (trace) {
      layout_create.setLayoutCount = 1;
      layout_create.pSetLayouts = &ray_descriptor_layout_;
    }
    return VulkanOk(vkCreatePipelineLayout(device_, &layout_create, nullptr,
                        &layout),
        "vkCreatePipelineLayout", detail);
  }

  bool CreatePipeline(const std::vector<std::uint32_t>& vertex_words,
      const std::vector<std::uint32_t>& fragment_words,
      VkPipelineLayout layout, const PipelineOptions& options,
      VkPipeline& pipeline, std::string& detail) {
    VkShaderModule vertex_module = CreateShader(device_, vertex_words, detail);
    VkShaderModule fragment_module =
        CreateShader(device_, fragment_words, detail);
    if (vertex_module == VK_NULL_HANDLE || fragment_module == VK_NULL_HANDLE) {
      vkDestroyShaderModule(device_, vertex_module, nullptr);
      vkDestroyShaderModule(device_, fragment_module, nullptr);
      return false;
    }
    const VkSpecializationMapEntry pass_entry{0, 0, sizeof(std::uint32_t)};
    const std::uint32_t pass = options.pass.value_or(0);
    VkSpecializationInfo specialization{};
    specialization.mapEntryCount = 1;
    specialization.pMapEntries = &pass_entry;
    specialization.dataSize = sizeof(pass);
    specialization.pData = &pass;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_module;
    stages[1].pName = "main";
    if (options.pass) {
      stages[1].pSpecializationInfo = &specialization;
    }
    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo input_assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport_state{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth_state{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth_state.depthTestEnable = options.depth ? VK_TRUE : VK_FALSE;
    depth_state.depthWriteEnable = options.depth ? VK_TRUE : VK_FALSE;
    depth_state.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    if (options.color_write) {
      blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                        VK_COLOR_COMPONENT_G_BIT |
                                        VK_COLOR_COMPONENT_B_BIT |
                                        VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    VkGraphicsPipelineCreateInfo pipeline_create{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline_create.stageCount = 2;
    pipeline_create.pStages = stages;
    pipeline_create.pVertexInputState = &vertex_input;
    pipeline_create.pInputAssemblyState = &input_assembly;
    pipeline_create.pViewportState = &viewport_state;
    pipeline_create.pRasterizationState = &raster;
    pipeline_create.pMultisampleState = &multisample;
    pipeline_create.pDepthStencilState = &depth_state;
    pipeline_create.pColorBlendState = &blend;
    pipeline_create.pDynamicState = &dynamic;
    pipeline_create.layout = layout;
    pipeline_create.renderPass = options.render_pass;
    pipeline_create.subpass = 0;
    const VkResult pipeline_result = vkCreateGraphicsPipelines(
        device_, VK_NULL_HANDLE, 1, &pipeline_create, nullptr, &pipeline);
    vkDestroyShaderModule(device_, vertex_module, nullptr);
    vkDestroyShaderModule(device_, fragment_module, nullptr);
    return VulkanOk(pipeline_result, "vkCreateGraphicsPipelines", detail);
  }

  // One wavefront kernel, selected by the kKernel specialization constant.
  bool CreateComputePipeline(const std::vector<std::uint32_t>& words,
      std::uint32_t kernel, VkPipeline& pipeline, std::string& detail) {
    VkShaderModule module = CreateShader(device_, words, detail);
    if (module == VK_NULL_HANDLE)
      return false;
    const VkSpecializationMapEntry kernel_entry{0, 0, sizeof(std::uint32_t)};
    VkSpecializationInfo specialization{};
    specialization.mapEntryCount = 1;
    specialization.pMapEntries = &kernel_entry;
    specialization.dataSize = sizeof(kernel);
    specialization.pData = &kernel;
    VkComputePipelineCreateInfo create{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    create.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    create.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    create.stage.module = module;
    create.stage.pName = "main";
    create.stage.pSpecializationInfo = &specialization;
    create.layout = ray_layout_;
    const VkResult result = vkCreateComputePipelines(device_, VK_NULL_HANDLE,
        1, &create, nullptr, &pipeline);
    vkDestroyShaderModule(device_, module, nullptr);
    return VulkanOk(result, "vkCreateComputePipelines(wavefront)", detail);
  }

  // Keep the targets when the size is unchanged; otherwise replace them. A
  // failed creation leaves no targets, so the next frame tries again.
  // Replacing the scene targets discards the radiance accumulation.
  bool EnsureTargets(Targets& targets, std::uint32_t width,
      std::uint32_t height, std::string& detail) {
    if (targets.framebuffer != VK_NULL_HANDLE && targets.width == width &&
        targets.height == height) {
      return true;
    }
    DestroyTargets(targets);
    if (&targets == &scene_targets_) {
      accumulation_key_.reset();
    }
    targets.width = width;
    targets.height = height;
    if (!CreateTargets(targets, &targets == &scene_targets_ ? scene_render_pass_
                                                            : render_pass_,
            detail)) {
      DestroyTargets(targets);
      return false;
    }
    ++target_creations_;
    return true;
  }

  bool CreateTargets(Targets& t, VkRenderPass render_pass,
      std::string& detail) {
    const bool accumulates = &t == &scene_targets_;
    if (!CreateImage(physical_device_, device_, t.width, t.height,
            t.color_format,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            t.color_image, t.color_memory, detail) ||
        !CreateImage(physical_device_, device_, t.width, t.height,
            kDepthFormat,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            t.depth_image, t.depth_memory, detail) ||
        (accumulates &&
            !CreateImage(physical_device_, device_, t.width, t.height,
                kSceneColorFormat, VK_IMAGE_USAGE_STORAGE_BIT,
                t.accumulation_image, t.accumulation_memory, detail))) {
      return false;
    }

    VkImageViewCreateInfo view_create{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_create.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_create.subresourceRange.baseMipLevel = 0;
    view_create.subresourceRange.levelCount = 1;
    view_create.subresourceRange.baseArrayLayer = 0;
    view_create.subresourceRange.layerCount = 1;
    view_create.image = t.color_image;
    view_create.format = t.color_format;
    view_create.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    if (!VulkanOk(vkCreateImageView(device_, &view_create, nullptr,
                      &t.color_view),
            "vkCreateImageView(color)", detail)) {
      return false;
    }
    if (accumulates) {
      view_create.image = t.accumulation_image;
      view_create.format = kSceneColorFormat;
      if (!VulkanOk(vkCreateImageView(device_, &view_create, nullptr,
                        &t.accumulation_view),
              "vkCreateImageView(accumulation)", detail)) {
        return false;
      }
    }
    view_create.image = t.depth_image;
    view_create.format = kDepthFormat;
    view_create.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (!VulkanOk(vkCreateImageView(device_, &view_create, nullptr,
                      &t.depth_view),
            "vkCreateImageView(depth)", detail)) {
      return false;
    }

    const VkImageView framebuffer_attachments[] = {t.color_view,
        t.depth_view};
    VkFramebufferCreateInfo framebuffer_create{
        VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer_create.renderPass = render_pass;
    framebuffer_create.attachmentCount = 2;
    framebuffer_create.pAttachments = framebuffer_attachments;
    framebuffer_create.width = t.width;
    framebuffer_create.height = t.height;
    framebuffer_create.layers = 1;
    if (!VulkanOk(vkCreateFramebuffer(device_, &framebuffer_create, nullptr,
                      &t.framebuffer),
            "vkCreateFramebuffer", detail)) {
      return false;
    }

    const VkDeviceSize pixel_count = VkDeviceSize{t.width} * t.height;
    const VkDeviceSize color_bytes = pixel_count * t.color_pixel_bytes;
    const VkDeviceSize depth_bytes = pixel_count * sizeof(float);
    if (!CreateHostBuffer(physical_device_, device_, color_bytes,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            t.color_readback,
            t.color_readback_memory, t.color_readback_coherent, detail) ||
        !CreateHostBuffer(physical_device_, device_, depth_bytes,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            t.depth_readback,
            t.depth_readback_memory, t.depth_readback_coherent, detail)) {
      return false;
    }
    return VulkanOk(vkMapMemory(device_, t.color_readback_memory, 0,
                        VK_WHOLE_SIZE, 0, &t.color_mapped),
               "vkMapMemory(color)", detail) &&
           VulkanOk(vkMapMemory(device_, t.depth_readback_memory, 0,
                        VK_WHOLE_SIZE, 0, &t.depth_mapped),
               "vkMapMemory(depth)", detail);
  }

  // Only called while no frame is in flight: Render waits for every
  // submission before it returns. Keeps the colour format.
  void DestroyTargets(Targets& t) {
    DestroyWavefront(t);
    if (t.color_mapped != nullptr) {
      vkUnmapMemory(device_, t.color_readback_memory);
    }
    if (t.depth_mapped != nullptr) {
      vkUnmapMemory(device_, t.depth_readback_memory);
    }
    vkDestroyBuffer(device_, t.depth_readback, nullptr);
    vkFreeMemory(device_, t.depth_readback_memory, nullptr);
    vkDestroyBuffer(device_, t.color_readback, nullptr);
    vkFreeMemory(device_, t.color_readback_memory, nullptr);
    vkDestroyFramebuffer(device_, t.framebuffer, nullptr);
    vkDestroyImageView(device_, t.depth_view, nullptr);
    vkDestroyImage(device_, t.depth_image, nullptr);
    vkFreeMemory(device_, t.depth_memory, nullptr);
    vkDestroyImageView(device_, t.color_view, nullptr);
    vkDestroyImage(device_, t.color_image, nullptr);
    vkFreeMemory(device_, t.color_memory, nullptr);
    vkDestroyImageView(device_, t.accumulation_view, nullptr);
    vkDestroyImage(device_, t.accumulation_image, nullptr);
    vkFreeMemory(device_, t.accumulation_memory, nullptr);
    Targets empty;
    empty.color_format = t.color_format;
    empty.color_pixel_bytes = t.color_pixel_bytes;
    t = empty;
  }

  bool Record(const DrawSummary& draw, const OffscreenTarget& target,
      Targets& targets, FramePass pass, std::uint32_t wavefront_iterations,
      bool restore, std::string& detail) {
    const bool trace = pass != FramePass::Bootstrap;
    VkCommandBufferBeginInfo command_begin{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (!VulkanOk(vkBeginCommandBuffer(command_, &command_begin),
            "vkBeginCommandBuffer", detail)) {
      return false;
    }
    if (trace && ray_timestamps_ != VK_NULL_HANDLE)
      vkCmdResetQueryPool(command_, ray_timestamps_, 0,
          wavefront_iterations == 0
              ? kSceneTimestamps
              : kSceneTimestamps + 3 + 2 * wavefront_iterations);
    if (!targets.initialized) {
      // LOAD preserves previous frames. Fresh images first need a defined
      // layout, then initialization inside the render pass below.
      VkImageMemoryBarrier barriers[2]{};
      for (auto& barrier : barriers) {
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
      }
      barriers[0].image = targets.color_image;
      barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      barriers[1].image = targets.depth_image;
      barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
      vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
          2, barriers);
    }
    const bool restore_color = restore && !target.clear_color_enabled &&
        !target.preserved_color.empty();
    const bool restore_depth = restore && !target.clear_depth_enabled &&
        !target.preserved_depth.empty();
    auto restore_image = [&](VkImage image, VkBuffer buffer,
                             VkImageAspectFlags aspect) {
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = image;
      barrier.subresourceRange = {aspect, 0, 1, 0, 1};
      vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
      VkBufferImageCopy copy{};
      copy.imageSubresource = {aspect, 0, 0, 1};
      copy.imageExtent = {targets.width, targets.height, 1};
      vkCmdCopyBufferToImage(command_, buffer, image,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
      barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    };
    if (restore_color)
      restore_image(targets.color_image, targets.color_readback,
          VK_IMAGE_ASPECT_COLOR_BIT);
    if (restore_depth)
      restore_image(targets.depth_image, targets.depth_readback,
          VK_IMAGE_ASPECT_DEPTH_BIT);
    if (trace) {
      // The radiance pass and the wavefront kernels read and write the
      // accumulation image; order that against the preceding submission's
      // accesses. Its contents are undefined until the first sample of an
      // accumulation writes them.
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.oldLayout = targets.initialized ? VK_IMAGE_LAYOUT_GENERAL
                                              : VK_IMAGE_LAYOUT_UNDEFINED;
      barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      barrier.dstAccessMask =
          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = targets.accumulation_image;
      barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      barrier.subresourceRange.levelCount = 1;
      barrier.subresourceRange.layerCount = 1;
      constexpr VkPipelineStageFlags kStages =
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      vkCmdPipelineBarrier(command_, kStages, kStages, 0, 0, nullptr, 0,
          nullptr, 1, &barrier);
    }
    VkRenderPassBeginInfo render_begin{
        VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    render_begin.renderPass = trace ? scene_render_pass_ : render_pass_;
    render_begin.framebuffer = targets.framebuffer;
    render_begin.renderArea.offset = {0, 0};
    render_begin.renderArea.extent = {targets.width, targets.height};
    if (trace && ray_timestamps_ != VK_NULL_HANDLE)
      vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, ray_timestamps_, 0);
    const VkViewport viewport = DisplayViewport(target);
    const VkRect2D scissor = DataScissor(target);
    const bool has_draw = trace ? scene_.Tlas() != VK_NULL_HANDLE : draw.triangle_count != 0;
    const bool drawn = has_draw && scissor.extent.width != 0 &&
                       scissor.extent.height != 0;
    // The wavefront kernels add the frame's sample before the render pass,
    // whose resolve pass writes the mean.
    recorded_wavefront_iterations_ = 0;
    if (drawn && wavefront_iterations != 0) {
      RecordWavefront(scissor, targets, wavefront_iterations);
      recorded_wavefront_iterations_ = wavefront_iterations;
    }
    vkCmdBeginRenderPass(command_, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
    VkClearAttachment clears[2]{};
    std::uint32_t clear_count = 0;
    if (target.clear_color_enabled || (!targets.initialized && !restore_color)) {
      auto& clear = clears[clear_count++];
      clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      std::copy(target.clear_color.begin(), target.clear_color.end(),
          clear.clearValue.color.float32);
    }
    if (target.clear_depth_enabled || (!targets.initialized && !restore_depth)) {
      auto& clear = clears[clear_count++];
      clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
      clear.clearValue.depthStencil = {target.clear_depth, 0};
    }
    if (clear_count != 0) {
      VkClearRect rect{};
      rect.rect.extent = {targets.width, targets.height};
      rect.layerCount = 1;
      vkCmdClearAttachments(command_, clear_count, clears, 1, &rect);
    }
    if (drawn) {
      const Matrix4 world_to_clip = VulkanWorldToClip(draw.world_to_clip);
      if (trace) {
        // The camera pass first: in Radiance output, for depth alone.
        vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_GRAPHICS,
            pass == FramePass::Barycentrics ? camera_pipeline_
            : pass == FramePass::Surface    ? surface_pipeline_
                                            : depth_pipeline_);
        vkCmdSetViewport(command_, 0, 1, &viewport);
        vkCmdSetScissor(command_, 0, 1, &scissor);
        vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_GRAPHICS,
            ray_layout_, 0, 1, &ray_descriptor_, 0, nullptr);
        vkCmdPushConstants(command_, ray_layout_, kRayStages, 0,
            sizeof(RayConstants), &ray_constants_);
        vkCmdDraw(command_, 3U, 1, 0, 0);
        if (pass == FramePass::Radiance) {
          vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_GRAPHICS,
              recorded_wavefront_iterations_ != 0 ? resolve_pipeline_
                                                  : radiance_pipeline_);
          vkCmdDraw(command_, 3U, 1, 0, 0);
        }
      } else {
        vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipeline_);
        vkCmdSetViewport(command_, 0, 1, &viewport);
        vkCmdSetScissor(command_, 0, 1, &scissor);
        vkCmdPushConstants(command_, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT,
            0, kFrameConstantsSize, world_to_clip.data());
        vkCmdDraw(command_, draw.triangle_count * 3U, 1, 0, 0);
      }
    }
    vkCmdEndRenderPass(command_);
    if (trace && ray_timestamps_ != VK_NULL_HANDLE)
      vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ray_timestamps_, 1);

    if (restore_color || restore_depth) {
      // Upload staging becomes readback storage in this submission.
      VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
    VkBufferImageCopy color_copy{};
    color_copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    color_copy.imageSubresource.layerCount = 1;
    color_copy.imageExtent = {targets.width, targets.height, 1};
    vkCmdCopyImageToBuffer(command_, targets.color_image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, targets.color_readback, 1,
        &color_copy);
    VkBufferImageCopy depth_copy{};
    depth_copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depth_copy.imageSubresource.layerCount = 1;
    depth_copy.imageExtent = {targets.width, targets.height, 1};
    vkCmdCopyImageToBuffer(command_, targets.depth_image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, targets.depth_readback, 1,
        &depth_copy);
    VkBufferMemoryBarrier host_barriers[2]{};
    for (VkBufferMemoryBarrier& barrier : host_barriers) {
      barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.offset = 0;
      barrier.size = VK_WHOLE_SIZE;
    }
    host_barriers[0].buffer = targets.color_readback;
    host_barriers[1].buffer = targets.depth_readback;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 2, host_barriers, 0,
        nullptr);
    return VulkanOk(vkEndCommandBuffer(command_), "vkEndCommandBuffer",
        detail);
  }

  // The wavefront integrator's sample: camera paths for the data window,
  // `iterations` bounces of intersect and shade, then the terminated paths
  // added to the accumulation. Every bounce is recorded; once the queues
  // are empty, its indirect dispatches have no groups. The queue state is
  // copied back for the path counts.
  void RecordWavefront(const VkRect2D& scissor, Targets& targets,
      std::uint32_t iterations) {
    const auto timestamp = [&](std::uint32_t query) {
      if (ray_timestamps_ != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            ray_timestamps_, kSceneTimestamps + query);
    };
    const auto barrier = [&](VkPipelineStageFlags source,
                             VkAccessFlags source_access,
                             VkPipelineStageFlags destination,
                             VkAccessFlags destination_access) {
      VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      memory.srcAccessMask = source_access;
      memory.dstAccessMask = destination_access;
      vkCmdPipelineBarrier(command_, source, destination, 0, 1, &memory, 0,
          nullptr, 0, nullptr);
    };
    constexpr VkPipelineStageFlags kKernels =
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
    constexpr VkAccessFlags kKernelAccess = VK_ACCESS_SHADER_READ_BIT |
                                            VK_ACCESS_SHADER_WRITE_BIT |
                                            VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    // Each kernel reads what the previous one wrote, its indirect dispatch
    // included.
    const auto kernel_barrier = [&] {
      barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
          kKernels, kKernelAccess);
    };
    const auto bind = [&](WavefrontKernel kernel) {
      vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_COMPUTE,
          wavefront_pipelines_[kernel]);
    };
    const auto dispatch_indirect = [&](WavefrontKernel kernel,
                                       std::uint32_t word) {
      bind(kernel);
      vkCmdDispatchIndirect(command_, targets.queue_state,
          VkDeviceSize{word} * sizeof(std::uint32_t));
    };
    const auto single = [&](WavefrontKernel kernel) {
      bind(kernel);
      vkCmdDispatch(command_, 1, 1, 1);
    };

    // The preceding submission's kernels and copy have finished with the
    // counters and paths this one resets and rewrites.
    barrier(kKernels | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
            VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdFillBuffer(command_, targets.queue_state, 0, VK_WHOLE_SIZE, 0);
    barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
        kKernels, kKernelAccess);
    vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_COMPUTE,
        ray_layout_, 0, 1, &ray_descriptor_, 0, nullptr);
    vkCmdPushConstants(command_, ray_layout_, kRayStages, 0,
        sizeof(RayConstants), &ray_constants_);

    timestamp(0);
    const std::uint64_t pixels =
        std::uint64_t{scissor.extent.width} * scissor.extent.height;
    const std::uint64_t groups =
        (pixels + kWavefrontGroupSize - 1) / kWavefrontGroupSize;
    bind(kGenerateKernel);
    vkCmdDispatch(command_,
        static_cast<std::uint32_t>(std::min<std::uint64_t>(groups,
            kWavefrontMaxGroupsX)),
        static_cast<std::uint32_t>(
            (groups + kWavefrontMaxGroupsX - 1) / kWavefrontMaxGroupsX),
        1);
    kernel_barrier();
    single(kPrepareKernel);
    kernel_barrier();
    timestamp(1);
    for (std::uint32_t bounce = 0; bounce < iterations; ++bounce) {
      dispatch_indirect(kIntersectKernel, kRayArgsWord);
      kernel_barrier();
      timestamp(2 + 2 * bounce);
      single(kPrepareKernel);
      kernel_barrier();
      dispatch_indirect(kShadeKernel, kHitArgsWord);
      kernel_barrier();
      single(kAdvanceKernel);
      kernel_barrier();
      timestamp(3 + 2 * bounce);
    }
    dispatch_indirect(kAccumulateKernel, kTerminatedArgsWord);
    timestamp(2 + 2 * iterations);
    // The resolve pass reads the accumulation; the host reads the counters.
    barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    const VkBufferCopy copy{0, 0, kQueueStateWords * sizeof(std::uint32_t)};
    vkCmdCopyBuffer(command_, targets.queue_state, targets.queue_readback, 1,
        &copy);
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = targets.queue_readback;
    host.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
  }

  // The messenger writes into `validation_`, so the renderer is never moved.
  InstanceState instance_state_;
  ValidationState validation_;
  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties device_properties_{};
  AccelerationSupport acceleration_;
  BackendCapability ray_query_;
  std::uint32_t queue_family_ = 0;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer command_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkRenderPass scene_render_pass_ = VK_NULL_HANDLE;
  VkPipeline camera_pipeline_ = VK_NULL_HANDLE;
  VkPipeline depth_pipeline_ = VK_NULL_HANDLE;
  VkPipeline surface_pipeline_ = VK_NULL_HANDLE;
  // Immutable samplers, one per pair of TextureWrap modes (s * 4 + t).
  std::array<VkSampler, kTextureSamplers> samplers_{};
  // The GpuScene texture generation the descriptor set holds.
  std::uint64_t texture_generation_ = 0;
  VkPipeline radiance_pipeline_ = VK_NULL_HANDLE;
  VkPipeline resolve_pipeline_ = VK_NULL_HANDLE;
  // One per WavefrontKernel; null without the wavefront shader.
  std::array<VkPipeline, kWavefrontKernels> wavefront_pipelines_{};
  // The bounces the command buffer's wavefront kernels were recorded for;
  // 0 when it has none.
  std::uint32_t recorded_wavefront_iterations_ = 0;
  VkPipelineLayout ray_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout ray_descriptor_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool ray_descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet ray_descriptor_ = VK_NULL_HANDLE;
  VkBuffer path_constants_ = VK_NULL_HANDLE;
  VkDeviceMemory path_constants_memory_ = VK_NULL_HANDLE;
  bool path_constants_coherent_ = false;
  void* path_constants_mapped_ = nullptr;
  VkQueryPool ray_timestamps_ = VK_NULL_HANDLE;
  std::uint32_t timestamp_bits_ = 0;
  RayConstants ray_constants_{};
  Targets targets_;
  Targets scene_targets_;
  GpuScene scene_;
  // Incremented by every UpdateScene with a nonempty plan.
  std::uint64_t scene_generation_ = 0;
  // The Radiance accumulation in scene_targets_: what it was rendered with,
  // and how many samples each pixel holds.
  std::optional<AccumulationKey> accumulation_key_;
  std::uint32_t accumulated_samples_ = 0;
  std::uint32_t target_creations_ = 0;
  std::uint64_t completion_ = 0;
  std::string failure_;
};

#endif

} // namespace

std::unique_ptr<OffscreenRenderer> CreateOffscreenRenderer(
    const std::string& vertex_shader, const std::string& fragment_shader,
    FrameStatus& status, std::string& error, const RayQueryShaders& ray_query_shaders) {
#if !defined(LOTUS_HAS_VULKAN)
  (void)vertex_shader;
  (void)fragment_shader;
  (void)ray_query_shaders;
  status = FrameStatus::Skip;
  error = "Vulkan backend was not compiled for this configuration";
  return nullptr;
#else
  auto renderer = std::make_unique<VulkanOffscreenRenderer>();
  if (!renderer->Initialize(vertex_shader, fragment_shader, status, error,
          ray_query_shaders)) {
    return nullptr;
  }
  return renderer;
#endif
}

GpuFrameEvidence RenderOffscreen(const DrawSummary& draw,
    const OffscreenTarget& target,
    const std::string& vertex_shader,
    const std::string& fragment_shader,
    std::uint32_t frame_count) {
  FrameStatus status = FrameStatus::Fail;
  std::string error;
  const std::unique_ptr<OffscreenRenderer> renderer =
      CreateOffscreenRenderer(vertex_shader, fragment_shader, status, error);
  if (!renderer) {
    GpuFrameEvidence evidence = Evidence(status, std::move(error));
#if !defined(LOTUS_HAS_VULKAN)
    evidence.validation_detail =
        "Vulkan validation capture is unavailable in the core-only "
        "configuration";
#endif
    return evidence;
  }
  return renderer->Render(draw, target, frame_count);
}

} // namespace Lotus
