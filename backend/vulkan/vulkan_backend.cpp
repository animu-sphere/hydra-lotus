// SPDX-License-Identifier: Apache-2.0
#include <lotus/vulkan_backend.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#if defined(LOTUS_HAS_VULKAN)
#include <vulkan/vulkan.h>

#include "gpu_scene.hpp"
#include "presentation.hpp"
#include "temporal_metadata.hpp"
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

// Adds the host time since the previous lap to one frame phase.
class HostLaps {
public:
  void Lap(double& phase) {
    const auto now = std::chrono::steady_clock::now();
    phase += std::chrono::duration<double, std::milli>(now - mark_).count();
    mark_ = now;
  }

private:
  std::chrono::steady_clock::time_point mark_ = std::chrono::steady_clock::now();
};

constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
// The scene pass's colour target and its radiance accumulation image.
constexpr VkFormat kSceneColorFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

struct RayConstants {
  Matrix4 world_to_clip;
  Matrix4 clip_to_world;
};
static_assert(sizeof(RayConstants) == 128);

struct TemporalConstants {
  Matrix4 current_view;
  Matrix4 previous_view;
  Matrix4 previous_world_to_clip;
  std::array<float, 4> previous_display;
  std::array<std::uint32_t, 4> previous_data;
  std::array<float, 4> validation;
  std::array<std::uint32_t, 4> history;
  std::array<std::uint32_t, 4> radiance;
};
static_assert(sizeof(TemporalConstants) == 272);
static_assert(sizeof(TemporalRadiancePixel) == 32 &&
              offsetof(TemporalRadiancePixel, sample_count) == 16 &&
              offsetof(TemporalRadiancePixel, moments) == 20 &&
              offsetof(TemporalRadiancePixel, variance) == 28);
static_assert(sizeof(TemporalValidityPixel) == 16 &&
              offsetof(TemporalValidityPixel, previous_index) == 8);
static_assert(sizeof(TemporalPixel) == 64 &&
              offsetof(TemporalPixel, instance_id) == 32 &&
              offsetof(TemporalPixel, flags) == 48 &&
              offsetof(TemporalPixel, albedo) == 52 &&
              offsetof(TemporalPixel, shading_normal) == 56);

struct TemporalBuffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  bool coherent = false;
  void* mapped = nullptr;
  VkDeviceSize bytes = 0;
};

// Private exact jittered source surface beside each frame's reuse records;
// mirrored in restir.slang. Never read back.
struct RestirSurfaceRecord {
  std::array<float, 4> origin, normal, shading_normal, outgoing;
  std::array<float, 4> base_alpha, dielectric_metallic, fresnel;
  std::uint64_t instance_id, material_id;
};
static_assert(sizeof(RestirSurfaceRecord) == 128);

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
  // kBsdfSampling or kMisLightSampling.
  std::uint32_t light_sampling;
  // DataScissor: {x, y, width, height}.
  std::uint32_t data_rect[4];
  // The GPU scene's light table: per-instance offsets and entries.
  std::uint64_t light_offsets;
  std::uint64_t lights;
  std::uint32_t light_count;
  // The GPU scene's analytic light records.
  std::uint32_t analytic_count;
  std::uint64_t analytic_lights;
  // The GPU scene's environment buffer, or 0 without an image, and the
  // probability with which the light table chooses the image.
  std::uint64_t environment_map;
  float environment_selection;
  std::uint32_t restir_candidates;
  std::uint32_t restir_temporal;
  std::uint32_t restir_history;
  std::uint32_t restir_max_history_candidates;
  std::uint32_t restir_spatial;
  std::uint32_t restir_spatial_radius;
  std::uint32_t restir_proposal;
  // The GPU scene's crossing hierarchy, followed by the distant lights'
  // indices, or 0 to test every analytic light (LightCrossing::Linear).
  std::uint64_t light_nodes;
  std::uint32_t light_node_count;
  std::uint32_t distant_count;
  // PathTracingSettings::environment_background.
  std::uint32_t environment_background;
};
static_assert(sizeof(PathConstants) == 208 &&
              offsetof(PathConstants, restir_proposal) == 180 &&
              offsetof(PathConstants, light_nodes) == 184 &&
              offsetof(PathConstants, analytic_lights) == 136 &&
              offsetof(PathConstants, environment_map) == 144 &&
              offsetof(PathConstants, instances) == 48 &&
              offsetof(PathConstants, materials) == 56 &&
              offsetof(PathConstants, width) == 80 &&
              offsetof(PathConstants, data_rect) == 96 &&
              offsetof(PathConstants, light_offsets) == 112 &&
              offsetof(PathConstants, light_count) == 128);
constexpr std::uint32_t kBsdfSampling = 0;
constexpr std::uint32_t kMisLightSampling = 1;
constexpr std::uint32_t kRestirInitialSampling = 2;

// The path tracer's kPass specialization constant. The resolve pass writes
// the mean of an accumulation the wavefront kernels added to.
constexpr std::uint32_t kCameraPass = 0;
constexpr std::uint32_t kRadiancePass = 1;
constexpr std::uint32_t kSurfacePass = 2;
constexpr std::uint32_t kResolvePass = 3;
constexpr std::uint32_t kTemporalPass = 4;
constexpr std::uint32_t kTemporalValidityPass = 5;
constexpr std::uint32_t kTemporalAccumulationPass = 6;
constexpr std::uint32_t kRestirInitialPass = 7;
constexpr std::uint32_t kRestirTemporalPass = 8;
constexpr std::uint32_t kRestirSpatialPass = 9;
constexpr std::uint32_t kTemporalVariancePass = 10;
// A-trous iterations, and the last one with the composite; the draw's first
// instance is the iteration.
constexpr std::uint32_t kTemporalFilterPass = 11;
constexpr std::uint32_t kTemporalCompositePass = 12;
// Surface, validity, accumulation, variance and filter timestamp pairs.
constexpr std::uint32_t kTemporalTimestampQueries = 10;

// Rigid placements and cameras may move; edits that can alter illumination
// anywhere reset radiance globally, including indirect/emissive changes.
bool TemporalSceneCompatible(const LotusScene& a, const LotusScene& b) {
  if (&a == &b)
    return true;
  if (a.environment != b.environment || a.environment_map != b.environment_map ||
      a.lights != b.lights || a.materials != b.materials || a.textures != b.textures ||
      a.meshes.size() != b.meshes.size())
    return false;
  auto old = b.meshes.begin();
  for (const auto& [key, mesh] : a.meshes) {
    const auto& previous = old->second;
    if (key != old->first || mesh.geometry != previous.geometry ||
        mesh.material != previous.material || mesh.instance.visible != previous.instance.visible ||
        mesh.instance.instancer_transforms.has_value() != previous.instance.instancer_transforms.has_value() ||
        (mesh.instance.instancer_transforms &&
            mesh.instance.instancer_transforms->size() != previous.instance.instancer_transforms->size()))
      return false;
    ++old;
  }
  return true;
}

struct TemporalTransportKey {
  Integrator integrator;
  LightSampling light_sampling;
  RestirProposal restir_proposal;
  std::uint32_t restir_candidates;
  bool restir_temporal;
  std::uint32_t restir_max_history_candidates;
  bool restir_spatial;
  std::uint32_t restir_spatial_radius;
  bool accumulate_radiance;
  std::uint32_t sample_index;
  std::uint32_t max_bounces;
  std::uint32_t max_samples;
  std::uint32_t max_history_samples;
  std::array<float, 4> background;
  bool environment_background;
  bool operator==(const TemporalTransportKey&) const = default;
};

// The wavefront kernels' kKernel specialization constant, in
// shaders/wavefront/wavefront.slang.
enum WavefrontKernel : std::uint32_t {
  kCameraKernel,
  kIntersectKernel,
  kShadeKernel,
  kTailKernel,
  kWavefrontKernels,
};
// The camera kernel covers its invocations in groups of kWavefrontGroupSize,
// in rows of at most kWavefrontMaxGroupsX groups; a queue's dispatch is one
// row.
constexpr std::uint32_t kWavefrontGroupSize = 64;
constexpr std::uint32_t kWavefrontMaxGroupsX = 65535;
// The words of wavefront.slang's queue_state: the two kernels' bounces,
// then per bounce the ray and hit queues' descriptors (each a
// VkDispatchIndirectCommand and the queue's length). Bounce 0's ray count
// is the camera rays; bounce kMaxWavefrontBounces + 1 holds the rays no
// path continues with.
constexpr std::uint32_t kQueueDescriptorWords = 4;
constexpr std::uint32_t kQueueCountWord = 3;
constexpr std::uint32_t kBouncesWord = 4;
constexpr std::uint32_t kBounceWords = 2 * kQueueDescriptorWords;
constexpr std::uint32_t RayQueueWord(std::uint32_t bounce) {
  return kBouncesWord + bounce * kBounceWords;
}
constexpr std::uint32_t HitQueueWord(std::uint32_t bounce) {
  return RayQueueWord(bounce) + kQueueDescriptorWords;
}
constexpr std::uint32_t kQueueStateWords =
    RayQueueWord(kMaxWavefrontBounces + 2);
// A frame's queue state is written with vkCmdUpdateBuffer.
static_assert(kQueueStateWords * sizeof(std::uint32_t) <= 65536);
// Two ray queues and the hit queue.
constexpr VkDeviceSize kWavefrontQueues = 3;
// A path slot's state in common/path_state.slang: five sections of one
// 16-byte entry per slot.
constexpr VkDeviceSize kPathStateBytes = 5 * 16;
// Timestamps: the scene pass's first and last (kSceneTimestamps), then the
// wavefront kernels': before the camera kernel, after it, after the first
// round's shade, after each later round's intersect and shade, and after
// the tail.
constexpr std::uint32_t kSceneTimestamps = 2;
constexpr std::uint32_t WavefrontTimestamps(std::uint32_t rounds) {
  return 2 + 2 * rounds;
}
constexpr std::uint32_t kRayTimestampQueries =
    kSceneTimestamps + WavefrontTimestamps(kMaxWavefrontBounces + 1);

// What a frame's wavefront kernels are recorded for: `bounces` ray-queue
// bounces (max_bounces + 1; 0 records no kernels), the first `rounds` of
// them as intersect and shade rounds and the rest in the tail kernel.
struct WavefrontSchedule {
  std::uint32_t bounces = 0;
  std::uint32_t rounds = 0;
};

// A round costs two dispatches and two barriers however few paths its
// queue holds. Bounces whose queue holds at least 1/kWavefrontTailDivisor
// of the camera rays run as rounds; the tail follows the rest.
constexpr std::uint32_t kWavefrontTailDivisor = 16;

// The rounds to record after a sample whose ray queues held `counts` paths
// per bounce: every bounce before the first that held fewer than the
// threshold, and at least the camera rays' bounce.
std::uint32_t WavefrontRounds(const std::vector<std::uint32_t>& counts) {
  const std::uint32_t camera = counts.empty() ? 0 : counts.front();
  const std::uint32_t threshold = std::max(1U,
      (camera + kWavefrontTailDivisor - 1) / kWavefrontTailDivisor);
  std::uint32_t rounds = 1;
  while (rounds < counts.size() && counts[rounds] >= threshold)
    ++rounds;
  return rounds;
}

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
  LightSampling light_sampling = LightSampling::Mis;
  RestirProposal restir_proposal = RestirProposal::Power;
  std::uint32_t restir_candidates = 8;
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

// The memory types of the renderer's own allocations, outside the scene's
// pools; Residency classifies them with the same choice.
std::uint32_t DeviceMemoryType(VkPhysicalDevice physical_device,
    std::uint32_t allowed) {
  return FindMemoryType(physical_device, allowed,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
}

// The host reads what the GPU copies into a buffer. Uncached host-visible
// memory makes those reads bus reads, about a millisecond for a 128x128
// frame's products; cached memory needs an invalidation when not coherent.
std::uint32_t HostMemoryType(VkPhysicalDevice physical_device,
    std::uint32_t allowed, VkBufferUsageFlags usage, bool* coherent) {
  std::uint32_t memory_type = std::numeric_limits<std::uint32_t>::max();
  if ((usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) != 0) {
    memory_type = FindMemoryType(physical_device, allowed,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, coherent);
  }
  if (memory_type == std::numeric_limits<std::uint32_t>::max()) {
    memory_type = FindMemoryType(physical_device, allowed,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        coherent);
  }
  return memory_type;
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
      DeviceMemoryType(physical_device, requirements.memoryTypeBits);
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
      DeviceMemoryType(physical_device, requirements.memoryTypeBits);
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
  const std::uint32_t memory_type = HostMemoryType(physical_device,
      requirements.memoryTypeBits, usage, &coherent);
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

// The images and framebuffer of one target size, for one colour format, and
// the persistently mapped colour/depth mirrors once a frame needs them. The
// scene targets also hold the radiance accumulation image.
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
      DestroyTemporalBuffers();
      DestroyTemporalBuffer(restir_initial_);
      DestroyTemporalBuffer(restir_readback_);
      for (auto& buffer : restir_history_buffers_)
        DestroyTemporalBuffer(buffer);
      DestroyTemporalBuffer(restir_history_readback_);
      for (auto& buffer : restir_surfaces_)
        DestroyTemporalBuffer(buffer);
      DestroyTemporalBuffer(restir_spatial_input_);
      DestroyTemporalBuffer(restir_spatial_input_readback_);
      DestroyTemporalBuffer(restir_spatial_sources_);
      DestroyTemporalBuffer(restir_spatial_sources_readback_);
      scene_.Destroy();
      vkDestroyFence(device_, fence_, nullptr);
      vkDestroyPipeline(device_, pipeline_, nullptr);
      vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
      vkDestroyPipeline(device_, camera_pipeline_, nullptr);
      vkDestroyPipeline(device_, depth_pipeline_, nullptr);
      vkDestroyPipeline(device_, surface_pipeline_, nullptr);
      vkDestroyPipeline(device_, radiance_pipeline_, nullptr);
      vkDestroyPipeline(device_, resolve_pipeline_, nullptr);
      vkDestroyPipeline(device_, temporal_pipeline_, nullptr);
      vkDestroyPipeline(device_, temporal_validity_pipeline_, nullptr);
      vkDestroyPipeline(device_, temporal_accumulation_pipeline_, nullptr);
      vkDestroyPipeline(device_, temporal_variance_pipeline_, nullptr);
      vkDestroyPipeline(device_, temporal_filter_pipeline_, nullptr);
      vkDestroyPipeline(device_, temporal_composite_pipeline_, nullptr);
      vkDestroyPipeline(device_, restir_pipeline_, nullptr);
      vkDestroyPipeline(device_, restir_temporal_pipeline_, nullptr);
      vkDestroyPipeline(device_, restir_spatial_pipeline_, nullptr);
      vkDestroyQueryPool(device_, restir_timestamps_, nullptr);
      vkDestroyQueryPool(device_, temporal_timestamps_, nullptr);
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
      if (presentation_)
        presentation_->Destroy(instance_state_.instance);
      vkDestroyDevice(device_, nullptr);
    } else if (presentation_) {
      presentation_->Destroy(instance_state_.instance);
    }
    DestroyInstance(instance_state_);
  }

  bool Initialize(const std::string& vertex_shader,
      const std::string& fragment_shader, FrameStatus& status,
      std::string& detail, const RayQueryShaders& ray_shaders,
      const PresentOptions* presentation) {
    status = FrameStatus::Fail;
    std::vector<std::uint32_t> vertex_words;
    std::vector<std::uint32_t> fragment_words;
    if (!LoadSpirv(vertex_shader, vertex_words, detail) ||
        !LoadSpirv(fragment_shader, fragment_words, detail)) {
      return false;
    }
    std::vector<const char*> instance_extensions;
    if (presentation != nullptr) {
      for (const std::string& extension :
          presentation->surface.instance_extensions)
        instance_extensions.push_back(extension.c_str());
    }
    if (!CreateInstanceWithValidation(
            presentation != nullptr ? "lotus-viewport" : "lotus-headless",
            instance_extensions, &validation_, instance_state_, detail)) {
      status = FrameStatus::Skip;
      return false;
    }
    if (presentation != nullptr) {
      presentation_ = std::make_unique<vulkan_internal::Presentation>();
      if (!presentation_->CreateSurface(instance_state_.instance,
              presentation->surface, detail)) {
        status = FrameStatus::Skip;
        return false;
      }
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
    if (presentation_) {
      std::vector<std::uint32_t> view_words;
      if (!ray_shaders.present_view.empty() &&
          !LoadSpirv(ray_shaders.present_view, view_words, detail))
        return false;
      bool unsupported = false;
      if (!presentation_->Initialize(instance_state_.instance,
              physical_device_, device_, queue_, queue_family_,
              kSceneColorFormat, *presentation, view_words, unsupported,
              detail)) {
        if (unsupported)
          status = FrameStatus::Skip;
        return false;
      }
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
              resolve_pipeline_, detail) ||
          !CreatePipeline(vertex_words, fragment_words, ray_layout_,
              {scene_render_pass_, kTemporalPass, false, false},
              temporal_pipeline_, detail) ||
          !CreatePipeline(vertex_words, fragment_words, ray_layout_,
              {scene_render_pass_, kTemporalValidityPass, false, false},
              temporal_validity_pipeline_, detail) ||
          !CreatePipeline(vertex_words, fragment_words, ray_layout_,
              {scene_render_pass_, kTemporalAccumulationPass, true, false},
              temporal_accumulation_pipeline_, detail) ||
          !CreatePipeline(vertex_words, fragment_words, ray_layout_,
              {scene_render_pass_, kTemporalVariancePass, false, false},
              temporal_variance_pipeline_, detail) ||
          !CreatePipeline(vertex_words, fragment_words, ray_layout_,
              {scene_render_pass_, kTemporalFilterPass, false, false},
              temporal_filter_pipeline_, detail) ||
          !CreatePipeline(vertex_words, fragment_words, ray_layout_,
              {scene_render_pass_, kTemporalCompositePass, true, false},
              temporal_composite_pipeline_, detail))
        return false;
      if (!CreatePipeline(vertex_words, fragment_words, ray_layout_,
              {scene_render_pass_, kRestirInitialPass, false, false},
              restir_pipeline_, detail) ||
          !CreatePipeline(vertex_words, fragment_words, ray_layout_,
              {scene_render_pass_, kRestirTemporalPass, false, false},
              restir_temporal_pipeline_, detail) ||
          !CreatePipeline(vertex_words, fragment_words, ray_layout_,
              {scene_render_pass_, kRestirSpatialPass, false, false},
              restir_spatial_pipeline_, detail))
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

  GpuResidency Residency() const override {
    GpuResidency residency;
    if (device_ == VK_NULL_HANDLE)
      return residency;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 memory{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    if (memory_budget_)
      memory.pNext = &budget;
    vkGetPhysicalDeviceMemoryProperties2(physical_device_, &memory);
    const auto& properties = memory.memoryProperties;
    const auto group = [&](const char* owner, bool device_local) -> GpuAllocationGroup& {
      for (auto& existing : residency.groups)
        if (existing.owner == owner && existing.device_local == device_local)
          return existing;
      return residency.groups.emplace_back(GpuAllocationGroup{owner, device_local});
    };
    const auto add = [&](const char* owner, const VkMemoryRequirements& requirements,
                         std::uint32_t type) {
      auto& entry = group(owner, (properties.memoryTypes[type].propertyFlags &
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0);
      ++entry.allocations;
      entry.bytes += requirements.size;
    };
    // Each is a dedicated allocation of its requirements, of the memory type
    // its creation helper chose.
    const auto device_buffer = [&](const char* owner, VkBuffer buffer) {
      if (buffer == VK_NULL_HANDLE)
        return;
      VkMemoryRequirements requirements{};
      vkGetBufferMemoryRequirements(device_, buffer, &requirements);
      add(owner, requirements, DeviceMemoryType(physical_device_, requirements.memoryTypeBits));
    };
    const auto host_buffer = [&](const char* owner, VkBuffer buffer, VkBufferUsageFlags usage) {
      if (buffer == VK_NULL_HANDLE)
        return;
      VkMemoryRequirements requirements{};
      vkGetBufferMemoryRequirements(device_, buffer, &requirements);
      add(owner, requirements,
          HostMemoryType(physical_device_, requirements.memoryTypeBits, usage, nullptr));
    };
    const auto image = [&](const char* owner, VkImage image) {
      if (image == VK_NULL_HANDLE)
        return;
      VkMemoryRequirements requirements{};
      vkGetImageMemoryRequirements(device_, image, &requirements);
      add(owner, requirements, DeviceMemoryType(physical_device_, requirements.memoryTypeBits));
    };
    constexpr VkBufferUsageFlags readback = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    GpuAllocationGroup pools_device{"scene.pools", true}, pools_host{"scene.pools", false};
    scene_.AddResidency(pools_device, pools_host);
    for (const auto* pools : {&pools_device, &pools_host})
      if (pools->allocations != 0)
        residency.groups.push_back(*pools);
    host_buffer("constants.path", path_constants_, 0);
    const auto targets = [&](const Targets& t, const char* attachments, const char* mirrors) {
      image(attachments, t.accumulation_image);
      image(attachments, t.color_image);
      image(attachments, t.depth_image);
      host_buffer(mirrors, t.color_readback, readback);
      host_buffer(mirrors, t.depth_readback, readback);
    };
    targets(targets_, "targets.bootstrap", "targets.bootstrap.readback");
    targets(scene_targets_, "targets.scene", "targets.scene.readback");
    for (const Targets* t : {&targets_, &scene_targets_}) {
      device_buffer("wavefront.state", t->queue_state);
      device_buffer("wavefront.state", t->queue_items);
      device_buffer("wavefront.state", t->paths);
      host_buffer("wavefront.readback", t->queue_readback, readback);
    }
    for (const auto& buffer : temporal_surfaces_)
      device_buffer("temporal.surfaces", buffer.buffer);
    host_buffer("temporal.surfaces.readback", temporal_readback_.buffer, readback);
    device_buffer("temporal.validity", temporal_validity_.buffer);
    host_buffer("temporal.validity.readback", temporal_validity_readback_.buffer, readback);
    host_buffer("temporal.metadata", temporal_constants_.buffer, 0);
    host_buffer("temporal.metadata", temporal_instances_.buffer, 0);
    for (const auto& buffer : temporal_radiance_)
      device_buffer("temporal.radiance", buffer.buffer);
    device_buffer("temporal.radiance", temporal_batch_.buffer);
    host_buffer("temporal.radiance.readback", temporal_radiance_readback_.buffer, readback);
    device_buffer("temporal.filter", temporal_filter_.buffer);
    device_buffer("restir.initial", restir_initial_.buffer);
    host_buffer("restir.initial.readback", restir_readback_.buffer, readback);
    for (const auto& buffer : restir_history_buffers_)
      device_buffer("restir.history", buffer.buffer);
    host_buffer("restir.history.readback", restir_history_readback_.buffer, readback);
    for (const auto& buffer : restir_surfaces_)
      device_buffer("restir.surfaces", buffer.buffer);
    device_buffer("restir.spatial", restir_spatial_input_.buffer);
    device_buffer("restir.spatial", restir_spatial_sources_.buffer);
    host_buffer("restir.spatial.readback", restir_spatial_input_readback_.buffer, readback);
    host_buffer("restir.spatial.readback", restir_spatial_sources_readback_.buffer, readback);
    if (presentation_) {
      image("present.view", presentation_->view_image());
      residency.swapchain_images = presentation_->swapchain_images();
    }
    for (const auto& entry : residency.groups)
      (entry.device_local ? residency.device_local_bytes : residency.host_bytes) += entry.bytes;
    residency.budget_available = memory_budget_;
    if (!memory_budget_)
      residency.budget_detail = "the device does not expose VK_EXT_memory_budget";
    for (std::uint32_t i = 0; i < properties.memoryHeapCount; ++i)
      residency.heaps.push_back({(properties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0,
          properties.memoryHeaps[i].size, memory_budget_ ? budget.heapUsage[i] : 0,
          memory_budget_ ? budget.heapBudget[i] : 0});
    return residency;
  }

  bool ValidateSceneSettings(const PathTracingSettings& settings,
      std::uint32_t frame_count, bool temporal_call, FramePass& pass,
      std::string& detail) const {
    auto Reject = [&](std::string message) { detail = std::move(message); return false; };
    if (camera_pipeline_ == VK_NULL_HANDLE)
      return Reject("ray-query shader paths were not supplied");
    pass = FramePass::Radiance;
    switch (settings.output) {
    case SceneOutput::Radiance:
      if (settings.integrator == Integrator::Wavefront) {
        if (wavefront_pipelines_[0] == VK_NULL_HANDLE)
          return Reject("the wavefront shader path was not supplied");
        if (settings.max_bounces > kMaxWavefrontBounces)
          return Reject("the wavefront integrator traces at most " +
                  std::to_string(kMaxWavefrontBounces) + " bounces");
      } else if (settings.integrator != Integrator::Reference) {
        return Reject("unknown integrator");
      }
      if (settings.light_sampling != LightSampling::Bsdf &&
          settings.light_sampling != LightSampling::Mis &&
          settings.light_sampling != LightSampling::RestirInitial)
        return Reject("unknown light sampling");
      if (settings.light_crossing != LightCrossing::Hierarchy &&
          settings.light_crossing != LightCrossing::Linear)
        return Reject("unknown light crossing");
      if (settings.light_sampling == LightSampling::RestirInitial &&
          (settings.restir_candidates == 0 || settings.restir_candidates > 64))
        return Reject("initial reservoir candidates must be in [1, 64]");
      if (settings.restir_proposal != RestirProposal::Power &&
          settings.restir_proposal != RestirProposal::Uniform &&
          settings.restir_proposal != RestirProposal::PowerUniform)
        return Reject("unknown reservoir proposal");
      if (settings.restir_temporal &&
          (settings.light_sampling != LightSampling::RestirInitial || !(temporal_call || rendering_temporal_) ||
              frame_count != 1 || settings.restir_max_history_candidates == 0 ||
              settings.restir_max_history_candidates > 65536))
        return Reject("temporal reservoirs require RenderTemporalScene, RestirInitial, one sample and history candidates in [1, 65536]");
      if (settings.restir_spatial &&
          (settings.light_sampling != LightSampling::RestirInitial || !(temporal_call || rendering_temporal_) ||
              frame_count != 1 || settings.restir_max_history_candidates == 0 ||
              settings.restir_max_history_candidates > 65536 ||
              settings.restir_spatial_radius == 0 || settings.restir_spatial_radius > 32))
        return Reject("spatial reservoirs require RenderTemporalScene, RestirInitial, one sample, history candidates in [1, 65536] and radius in [1, 32]");
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
      return Reject("unknown scene output");
    }
    return true;
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
    FramePass pass;
    std::string settings_error;
    if (!ValidateSceneSettings(settings, frame_count, false, pass, settings_error))
      return Evidence(FrameStatus::Fail, settings_error);
    ray_constants_.world_to_clip = VulkanWorldToClip(draw.world_to_clip);
    if (!Invert(ray_constants_.world_to_clip, ray_constants_.clip_to_world))
      return Evidence(FrameStatus::Fail, "ray-query camera is singular or non-finite");
    return RenderFrame(draw, target, frame_count, pass, settings);
  }

  bool ValidateFrameTarget(const OffscreenTarget& target, std::uint32_t frame_count,
      std::string& detail) const {
    if (!std::isfinite(target.clear_depth) || target.clear_depth < 0.0F ||
        target.clear_depth > 1.0F ||
        !std::all_of(target.clear_color.begin(), target.clear_color.end(),
            [](float value) { return std::isfinite(value); })) {
      detail = "invalid offscreen clear values";
      return false;
    }
    if (frame_count == 0) {
      detail = "frame_count must be at least 1";
      return false;
    }
    if (target.width == 0 || target.height == 0) {
      detail = "the offscreen target is empty";
      return false;
    }
    const VkPhysicalDeviceLimits& limits = device_properties_.limits;
    if (target.width > std::min(limits.maxImageDimension2D,
                           limits.maxFramebufferWidth) ||
        target.height > std::min(limits.maxImageDimension2D,
                            limits.maxFramebufferHeight)) {
      std::ostringstream message;
      message << "the offscreen target " << target.width << 'x'
              << target.height << " exceeds the device's framebuffer limits";
      detail = message.str();
      return false;
    }

    const std::size_t pixels = std::size_t{target.width} * target.height;
    if ((!target.preserved_color.empty() &&
            target.preserved_color.size() != pixels * 4) ||
        (!target.preserved_depth.empty() &&
            target.preserved_depth.size() != pixels)) {
      detail = "invalid preserved attachment extent";
      return false;
    }
    return true;
  }

  GpuFrameEvidence RenderFrame(const DrawSummary& draw,
      const OffscreenTarget& target, std::uint32_t frame_count,
      FramePass pass, const PathTracingSettings& settings) {
    if (!failure_.empty()) {
      return Evidence(FrameStatus::Fail,
          "the renderer failed earlier: " + failure_);
    }
    FrameHostTimings host;
    HostLaps laps;
    const bool trace = pass != FramePass::Bootstrap;
    if (!trace && !((draw.draw_count == 1 && draw.triangle_count == 1) ||
                      (draw.draw_count == 0 && draw.triangle_count == 0))) {
      return Evidence(FrameStatus::Fail,
          "bootstrap extraction must be empty or produce one triangle draw");
    }
    std::string target_error;
    if (!ValidateFrameTarget(target, frame_count, target_error))
      return Evidence(FrameStatus::Fail, target_error);
    Targets& targets = trace ? scene_targets_ : targets_;
    std::string detail;
    if (!EnsureTargets(targets, target.width, target.height, detail)) {
      return Evidence(FrameStatus::Fail, detail);
    }
    // The mirrors receive read-back products and stage preserved contents;
    // frames that do neither leave them unallocated.
    if ((target.read_back || RestoresContents(target)) &&
        !EnsureTargetMirrors(targets, detail)) {
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
          settings.integrator, settings.light_sampling, settings.restir_proposal,
          settings.restir_candidates, settings.sample_index,
          settings.max_bounces, scene_generation_};
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
    // A path's last scattering event is at depth max_bounces. Without a
    // previous sample to go by, every bounce is a round.
    WavefrontSchedule wavefront;
    if (pass == FramePass::Radiance &&
        settings.integrator == Integrator::Wavefront) {
      wavefront.bounces = settings.max_bounces + 1;
      wavefront.rounds = wavefront_rounds_hint_ == 0
                             ? wavefront.bounces
                             : std::min(wavefront_rounds_hint_,
                                   wavefront.bounces);
    }
    if (wavefront.bounces != 0 && !EnsureWavefront(targets, detail)) {
      return Evidence(FrameStatus::Fail, detail);
    }
    const bool restir = pass == FramePass::Radiance &&
                        settings.light_sampling == LightSampling::RestirInitial;
    restir_spatial_active_ = restir && settings.restir_spatial;
    restir_temporal_active_ = restir && (settings.restir_temporal || settings.restir_spatial);
    if (restir && !EnsureRestir(target, detail))
      return Evidence(FrameStatus::Fail, detail);
    if (trace) {
      UpdateSceneDescriptors(targets);
    }
    bool initializing = !targets.initialized;
    laps.Lap(host.prepare_ms);
    if (!Record(draw, target, targets, pass, wavefront, restir, true, detail)) {
      return Evidence(FrameStatus::Fail, detail);
    }
    laps.Lap(host.record_ms);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_;
    for (std::uint32_t frame = 0; frame < submissions; ++frame) {
      const bool add_sample = frame < samples_to_add;
      if (trace && !WritePathConstants(target, settings, add_sample, detail)) {
        return Evidence(FrameStatus::Fail, detail);
      }
      laps.Lap(host.prepare_ms);
      // A failure here may leave the command buffer pending, so it can never
      // be recorded again.
      if (!VulkanOk(vkResetFences(device_, 1, &fence_), "vkResetFences",
              detail) ||
          !VulkanOk(vkQueueSubmit(queue_, 1, &submit, fence_),
              "vkQueueSubmit", detail)) {
        failure_ = detail;
        return Evidence(FrameStatus::Fail, detail);
      }
      laps.Lap(host.submit_ms);
      if (!VulkanOk(vkWaitForFences(device_, 1, &fence_, VK_TRUE,
                        10'000'000'000ULL),
              "vkWaitForFences", detail)) {
        failure_ = detail;
        return Evidence(FrameStatus::Fail, detail);
      }
      laps.Lap(host.wait_ms);
      ++completion_;
      if (add_sample) {
        ++accumulated_samples_;
      }
      targets.initialized = true;
      // The first recording gives fresh images their layouts, which would
      // discard the accumulation if it were submitted again.
      const bool restored = frame == 0 && RestoresContents(target);
      if ((initializing || restored) && frame + 1 < submissions) {
        initializing = false;
        if (!Record(draw, target, targets, pass, wavefront, restir, false, detail)) {
          return Evidence(FrameStatus::Fail, detail);
        }
        laps.Lap(host.record_ms);
      }
    }
    laps.Lap(host.results_ms);
    if (target.read_back) {
      if (!InvalidateIfNeeded(device_, targets.color_readback_memory,
              targets.color_readback_coherent, detail) ||
          !InvalidateIfNeeded(device_, targets.depth_readback_memory,
              targets.depth_readback_coherent, detail)) {
        return Evidence(FrameStatus::Fail, detail);
      }
      laps.Lap(host.readback_ms);
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
    if (target.read_back) {
      laps.Lap(host.results_ms);
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
      laps.Lap(host.readback_ms);
    }
    // Present copies this frame's scene colour; a bootstrap frame leaves
    // nothing to present. Its reservoirs hold the last fresh sample's, and
    // RenderTemporalScene marks its temporal products once they exist.
    presentable_ = trace;
    temporal_presentable_ = false;
    radiance_presentable_ = false;
    if (!restir)
      restir_presentable_ = RestirPresentable::None;
    else if (samples_to_add != 0)
      restir_presentable_ = restir_temporal_active_ ? RestirPresentable::Reuse
                                                    : RestirPresentable::Initial;

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
    if (recorded_wavefront_.bounces != 0 && samples_to_add != 0) {
      if (!InvalidateIfNeeded(device_, targets.queue_readback_memory,
              targets.queue_readback_coherent, detail)) {
        return Evidence(FrameStatus::Fail, detail);
      }
      const auto* words = static_cast<const std::uint32_t*>(targets.queue_mapped);
      std::vector<std::uint32_t>& counts = evidence.wavefront_path_counts;
      for (std::uint32_t bounce = 0; bounce < recorded_wavefront_.bounces;
          ++bounce)
        counts.push_back(words[RayQueueWord(bounce) + kQueueCountWord]);
      while (!counts.empty() && counts.back() == 0)
        counts.pop_back();
      evidence.wavefront_rounds = recorded_wavefront_.rounds;
      wavefront_rounds_hint_ = WavefrontRounds(counts);
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
    if (restir && samples_to_add != 0 && !ReadRestir(target, evidence.restir, detail)) {
      failure_ = detail;
      return Evidence(FrameStatus::Fail, detail);
    }
    std::ostringstream success;
    success << "rendered " << submissions << " deterministic frames on "
            << device_properties_.deviceName;
    evidence.detail = success.str();
    if (!rendering_temporal_)
      temporal_history_.Reset();
    laps.Lap(host.results_ms);
    evidence.host_timings = host;
    return evidence;
  }

  void DestroyTemporalBuffer(TemporalBuffer& buffer) {
    if (buffer.mapped)
      vkUnmapMemory(device_, buffer.memory);
    vkDestroyBuffer(device_, buffer.buffer, nullptr);
    vkFreeMemory(device_, buffer.memory, nullptr);
    buffer = {};
  }

  void DestroyTemporalBuffers() {
    for (auto& buffer : temporal_surfaces_)
      DestroyTemporalBuffer(buffer);
    DestroyTemporalBuffer(temporal_readback_);
    DestroyTemporalBuffer(temporal_validity_);
    DestroyTemporalBuffer(temporal_validity_readback_);
    for (auto& buffer : temporal_radiance_)
      DestroyTemporalBuffer(buffer);
    DestroyTemporalBuffer(temporal_radiance_readback_);
    DestroyTemporalBuffer(temporal_batch_);
    DestroyTemporalBuffer(temporal_filter_);
    DestroyTemporalBuffer(temporal_constants_);
    DestroyTemporalBuffer(temporal_instances_);
    temporal_width_ = temporal_height_ = 0;
  }

  bool EnsureTemporalBuffer(TemporalBuffer& buffer, VkDeviceSize bytes,
      VkBufferUsageFlags usage, bool host, std::string& detail) {
    if (buffer.bytes >= bytes)
      return true;
    DestroyTemporalBuffer(buffer);
    const bool created = host
                             ? CreateHostBuffer(physical_device_, device_, bytes, usage,
                                   buffer.buffer, buffer.memory, buffer.coherent, detail)
                             : CreateDeviceBuffer(physical_device_, device_, bytes, usage,
                                   buffer.buffer, buffer.memory, detail);
    if (!created)
      return false;
    buffer.bytes = bytes;
    return !host || VulkanOk(vkMapMemory(device_, buffer.memory, 0,
                                 VK_WHOLE_SIZE, 0, &buffer.mapped),
                        "vkMapMemory(temporal)", detail);
  }

  bool WriteTemporalBuffer(TemporalBuffer& buffer, const void* data,
      std::size_t bytes, std::string& detail) {
    std::memcpy(buffer.mapped, data, bytes);
    if (buffer.coherent)
      return true;
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = buffer.memory;
    range.size = VK_WHOLE_SIZE;
    return VulkanOk(vkFlushMappedMemoryRanges(device_, 1, &range),
        "vkFlushMappedMemoryRanges(temporal)", detail);
  }

  bool EnsureRestir(const OffscreenTarget& target, std::string& detail) {
    const VkDeviceSize bytes = VkDeviceSize{target.width} * target.height * sizeof(RestirPixel);
    if (bytes > device_properties_.limits.maxStorageBufferRange) {
      detail = "initial reservoir exceeds the device's storage buffer range";
      return false;
    }
    // Mirrors are allocated by the first frame that reads them back.
    if (!EnsureTemporalBuffer(restir_initial_, bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            false, detail) ||
        (target.read_back &&
            !EnsureTemporalBuffer(restir_readback_, bytes,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, detail)))
      return false;
    const VkDescriptorBufferInfo info{restir_initial_.buffer, 0, bytes};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = ray_descriptor_;
    write.dstBinding = 16;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    return true;
  }

  bool EnsureRestirHistory(const OffscreenTarget& target, std::string& detail) {
    const VkDeviceSize bytes = VkDeviceSize{target.width} * target.height * sizeof(RestirReusePixel);
    const VkDeviceSize surface_bytes =
        VkDeviceSize{target.width} * target.height * sizeof(RestirSurfaceRecord);
    if (surface_bytes > device_properties_.limits.maxStorageBufferRange) {
      detail = "temporal reservoir exceeds the device's storage buffer range";
      return false;
    }
    constexpr auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    for (auto& buffer : restir_history_buffers_)
      if (!EnsureTemporalBuffer(buffer, bytes, usage, false, detail))
        return false;
    for (auto& buffer : restir_surfaces_)
      if (!EnsureTemporalBuffer(buffer, surface_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, detail))
        return false;
    const bool mirrors = target.read_back;
    if (mirrors && !EnsureTemporalBuffer(restir_history_readback_, bytes * 2,
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, detail))
      return false;
    const VkDeviceSize source_bytes = VkDeviceSize{target.width} * target.height * 16;
    if (restir_spatial_active_ &&
        (!EnsureTemporalBuffer(restir_spatial_input_, bytes, usage, false, detail) ||
            !EnsureTemporalBuffer(restir_spatial_sources_, source_bytes, usage, false, detail) ||
            (mirrors &&
                (!EnsureTemporalBuffer(restir_spatial_input_readback_, bytes,
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, detail) ||
                    !EnsureTemporalBuffer(restir_spatial_sources_readback_, source_bytes,
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, detail)))))
      return false;
    // Bindings 17-20 and the current/previous surfaces at 22-23; the spatial
    // pair is written only when spatial reuse is active.
    const VkDescriptorBufferInfo buffers[6] = {
        {restir_surfaces_[temporal_write_].buffer, 0, surface_bytes},
        {restir_surfaces_[1 - temporal_write_].buffer, 0, surface_bytes},
        {restir_spatial_active_ ? restir_spatial_input_.buffer : restir_history_buffers_[temporal_write_].buffer, 0, bytes},
        {restir_history_buffers_[1 - temporal_write_].buffer, 0, bytes},
        {restir_history_buffers_[temporal_write_].buffer, 0, bytes},
        {restir_spatial_sources_.buffer, 0, source_bytes}};
    VkWriteDescriptorSet writes[6]{};
    const unsigned count = restir_spatial_active_ ? 6u : 4u;
    for (unsigned i = 0; i < count; ++i) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = ray_descriptor_;
      writes[i].dstBinding = i < 2 ? 22 + i : 15 + i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[i].pBufferInfo = &buffers[i];
    }
    vkUpdateDescriptorSets(device_, count, writes, 0, nullptr);
    return true;
  }

  void RecordRestir(const OffscreenTarget& target,
      const VkRenderPassBeginInfo& render) {
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                           VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    if (restir_timestamps_ != VK_NULL_HANDLE) {
      vkCmdResetQueryPool(command_, restir_timestamps_, 0, 6);
      vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, restir_timestamps_, 0);
    }
    vkCmdBeginRenderPass(command_, &render, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0, 0, static_cast<float>(target.width), static_cast<float>(target.height), 0, 1};
    const VkRect2D scissor{{0, 0}, {target.width, target.height}};
    vkCmdSetViewport(command_, 0, 1, &viewport);
    vkCmdSetScissor(command_, 0, 1, &scissor);
    vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_GRAPHICS, restir_pipeline_);
    vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_GRAPHICS,
        ray_layout_, 0, 1, &ray_descriptor_, 0, nullptr);
    vkCmdPushConstants(command_, ray_layout_, kRayStages, 0, sizeof(RayConstants), &ray_constants_);
    vkCmdDraw(command_, 3, 1, 0, 0);
    vkCmdEndRenderPass(command_);
    if (restir_timestamps_ != VK_NULL_HANDLE)
      vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, restir_timestamps_, 1);
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);
    if (restir_temporal_active_) {
      if (restir_timestamps_ != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, restir_timestamps_, 2);
      vkCmdBeginRenderPass(command_, &render, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdSetViewport(command_, 0, 1, &viewport);
      vkCmdSetScissor(command_, 0, 1, &scissor);
      vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_GRAPHICS, restir_temporal_pipeline_);
      vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_GRAPHICS,
          ray_layout_, 0, 1, &ray_descriptor_, 0, nullptr);
      vkCmdPushConstants(command_, ray_layout_, kRayStages, 0, sizeof(RayConstants), &ray_constants_);
      vkCmdDraw(command_, 3, 1, 0, 0);
      vkCmdEndRenderPass(command_);
      if (restir_timestamps_ != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, restir_timestamps_, 3);
      vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
    if (restir_spatial_active_) {
      if (restir_timestamps_ != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, restir_timestamps_, 4);
      vkCmdBeginRenderPass(command_, &render, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdSetViewport(command_, 0, 1, &viewport);
      vkCmdSetScissor(command_, 0, 1, &scissor);
      vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_GRAPHICS, restir_spatial_pipeline_);
      vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_GRAPHICS,
          ray_layout_, 0, 1, &ray_descriptor_, 0, nullptr);
      vkCmdPushConstants(command_, ray_layout_, kRayStages, 0, sizeof(RayConstants), &ray_constants_);
      vkCmdDraw(command_, 3, 1, 0, 0);
      vkCmdEndRenderPass(command_);
      if (restir_timestamps_ != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, restir_timestamps_, 5);
      vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
  }

  // Without read_back, the records stay on the GPU and the host checks of
  // their arithmetic flags are skipped; the rest is reported as usual.
  bool ReadRestir(const OffscreenTarget& target, RestirProducts& products,
      std::string& detail) {
    products.width = target.width;
    products.height = target.height;
    products.row_pitch = target.width * sizeof(RestirPixel);
    products.scene_generation = scene_generation_;
    products.reservoir_bytes = restir_initial_.bytes + restir_readback_.bytes;
    if (restir_temporal_active_) {
      products.history_available = restir_history_available_;
      products.temporal_row_pitch = target.width * sizeof(RestirReusePixel);
      if (restir_spatial_active_)
        products.spatial_row_pitch = products.temporal_row_pitch;
    }
    if (target.read_back && !ReadRestirRecords(target, products, detail))
      return false;
    for (const auto& buffer : restir_history_buffers_)
      products.reservoir_bytes += buffer.bytes;
    for (const auto& buffer : restir_surfaces_)
      products.reservoir_bytes += buffer.bytes;
    products.reservoir_bytes += restir_history_readback_.bytes;
    products.reservoir_bytes += restir_spatial_input_.bytes + restir_spatial_input_readback_.bytes +
        restir_spatial_sources_.bytes + restir_spatial_sources_readback_.bytes;
    if (restir_timestamps_ != VK_NULL_HANDLE) {
      std::uint64_t ticks[6]{};
      const unsigned queries = restir_spatial_active_ ? 6u : restir_temporal_active_ ? 4u : 2u;
      if (!VulkanOk(vkGetQueryPoolResults(device_, restir_timestamps_, 0, queries,
              sizeof(ticks), ticks, sizeof(std::uint64_t),
              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
              "vkGetQueryPoolResults(initial reservoir)", detail))
        return false;
      const std::uint64_t mask = timestamp_bits_ == 64 ? ~std::uint64_t{0}
          : (std::uint64_t{1} << timestamp_bits_) - 1;
      products.timestamp_available = true;
      products.initial_gpu_ms = static_cast<double>((ticks[1] - ticks[0]) & mask) *
          device_properties_.limits.timestampPeriod / 1'000'000.0;
      if (restir_temporal_active_)
        products.temporal_gpu_ms = static_cast<double>((ticks[3] - ticks[2]) & mask) *
            device_properties_.limits.timestampPeriod / 1'000'000.0;
      if (restir_spatial_active_)
        products.spatial_gpu_ms = static_cast<double>((ticks[5] - ticks[4]) & mask) *
            device_properties_.limits.timestampPeriod / 1'000'000.0;
    }
    products.available = true;
    return true;
  }

  bool ReadRestirRecords(const OffscreenTarget& target,
      RestirProducts& products, std::string& detail) {
    if (!InvalidateIfNeeded(device_, restir_readback_.memory,
            restir_readback_.coherent, detail))
      return false;
    products.initial.resize(std::size_t{target.width} * target.height);
    std::memcpy(products.initial.data(), restir_readback_.mapped,
        products.initial.size() * sizeof(RestirPixel));
    if (const auto invalid = std::find_if(products.initial.begin(), products.initial.end(),
            [](const RestirPixel& p) { return (p.flags & 2) != 0; });
        invalid != products.initial.end()) {
      const auto index = static_cast<std::size_t>(invalid - products.initial.begin());
      std::ostringstream where;
      where << "initial reservoir arithmetic is invalid or exceeds GPU float storage at pixel ("
            << index % target.width << ", " << index / target.width << "): light " << invalid->light
            << ", weight sum " << invalid->weight_sum << ", target " << invalid->selected_target
            << ", candidates " << invalid->candidate_count << ", proposal " << invalid->proposal;
      detail = where.str();
      return false;
    }
    if (restir_temporal_active_) {
      if (!InvalidateIfNeeded(device_, restir_history_readback_.memory,
              restir_history_readback_.coherent, detail))
        return false;
      products.temporal.resize(products.initial.size());
      if (restir_history_available_)
        products.previous.resize(products.initial.size());
      const auto* records = static_cast<const RestirReusePixel*>(restir_history_readback_.mapped);
      const auto* temporal_records = records;
      if (restir_spatial_active_) {
        if (!InvalidateIfNeeded(device_, restir_spatial_input_readback_.memory,
                restir_spatial_input_readback_.coherent, detail) ||
            !InvalidateIfNeeded(device_, restir_spatial_sources_readback_.memory,
                restir_spatial_sources_readback_.coherent, detail))
          return false;
        temporal_records = static_cast<const RestirReusePixel*>(restir_spatial_input_readback_.mapped);
        products.spatial.resize(products.initial.size());
        products.spatial_sources.resize(products.initial.size());
        std::memcpy(products.spatial_sources.data(), restir_spatial_sources_readback_.mapped,
            products.spatial_sources.size() * sizeof(products.spatial_sources[0]));
      }
      for (std::size_t i = 0; i < products.initial.size(); ++i) {
        products.temporal[i] = temporal_records[i];
        if ((products.temporal[i].reservoir.flags & 2) != 0) {
          detail = "temporal reservoir arithmetic is invalid or exceeds GPU float storage";
          return false;
        }
        if (restir_spatial_active_) {
          products.spatial[i] = records[i];
          if ((products.spatial[i].reservoir.flags & 2) != 0) {
            detail = "spatial reservoir arithmetic is invalid or exceeds GPU float storage";
            return false;
          }
        }
        if (restir_history_available_)
          products.previous[i] = records[products.initial.size() + i];
      }
    }
    return true;
  }

  GpuFrameEvidence RenderTemporalScene(const FrameSnapshot& snapshot,
      const OffscreenTarget& target, std::uint32_t frame_count,
      const TemporalFrameSettings& requested,
      const PathTracingSettings& settings) override {
    if (!failure_.empty())
      return Evidence(FrameStatus::Fail, failure_);
    if (!ray_query_.available)
      return Evidence(FrameStatus::Skip, ray_query_.detail);
    if (snapshot.scene != uploaded_scene_)
      return Evidence(FrameStatus::Fail,
          "apply this snapshot's SceneExtraction update before temporal rendering");
    std::string request_error;
    if (!ValidateFrameTarget(target, frame_count, request_error))
      return Evidence(FrameStatus::Fail, request_error);
    FramePass requested_pass;
    if (!ValidateSceneSettings(settings, frame_count, true, requested_pass, request_error))
      return Evidence(FrameStatus::Fail, request_error);
    Matrix4 inverse;
    if (!Invert(VulkanWorldToClip(ExtractDrawSummary(snapshot).world_to_clip), inverse))
      return Evidence(FrameStatus::Fail, "ray-query camera is singular or non-finite");
    const bool accumulate = requested.accumulate_radiance;
    const bool reuse = settings.restir_temporal;
    const bool reservoirs = reuse || settings.restir_spatial;
    const bool fresh_batch = accumulate || reservoirs;
    if (reservoirs && (frame_count != 1 || settings.output != SceneOutput::Radiance ||
                     settings.light_sampling != LightSampling::RestirInitial ||
                     settings.restir_candidates == 0 || settings.restir_candidates > 64 ||
                     settings.restir_max_history_candidates == 0 ||
                     settings.restir_max_history_candidates > 65536))
      return Evidence(FrameStatus::Fail,
          "temporal reservoirs require Radiance, RestirInitial, one fresh sample, candidates in [1, 64] and history candidates in [1, 65536]");
    if (accumulate && (requested.max_history_samples == 0 || requested.max_history_samples > 65536 ||
                          settings.output != SceneOutput::Radiance || !target.clear_color_enabled))
      return Evidence(FrameStatus::Fail,
          "temporal radiance requires Radiance, colour clears and history samples in [1, 65536]");
    const std::uint32_t filter_iterations = requested.filter_iterations;
    if (filter_iterations > kTemporalFilterMaxIterations || (filter_iterations != 0 && !accumulate))
      return Evidence(FrameStatus::Fail,
          "temporal filtering requires radiance accumulation and at most 8 iterations");
    auto next_history = temporal_history_;
    auto next_identities = temporal_identities_;
    auto frame_settings = requested;
    frame_settings.width = target.width;
    frame_settings.height = target.height;
    frame_settings.display_window = target.display_window;
    frame_settings.data_window = target.data_window;
    TemporalFrame frame;
    try {
      frame = next_history.Advance(snapshot, frame_settings);
    } catch (const std::invalid_argument& error) {
      return Evidence(FrameStatus::Fail, error.what());
    }
    const TemporalTransportKey radiance_key{settings.integrator, settings.light_sampling,
        settings.restir_proposal,
        settings.restir_candidates, settings.restir_temporal, settings.restir_max_history_candidates,
        settings.restir_spatial, settings.restir_spatial_radius,
        accumulate, settings.sample_index, settings.max_bounces, settings.max_samples,
        requested.max_history_samples, target.clear_color,
        settings.environment_background};
    if (fresh_batch && frame.previous) {
      if (!temporal_transport_key_ || *temporal_transport_key_ != radiance_key) {
        frame.previous.reset();
        frame.reset_reason = TemporalResetReason::RenderSettingsChanged;
      } else if (snapshot.scene != frame.previous->scene &&
                 (!snapshot.scene || !frame.previous->scene ||
                     !TemporalSceneCompatible(*snapshot.scene, *frame.previous->scene) ||
                     (reuse && scene_.Stats().emissive_triangles != 0))) {
        frame.previous.reset();
        frame.reset_reason = TemporalResetReason::SceneChanged;
      }
    }
    const auto instances = next_identities.Build(frame);
    if (instances.size() != scene_.Stats().instance_count)
      return Evidence(FrameStatus::Fail, "temporal instance order/scene mismatch");
    const auto max_range = device_properties_.limits.maxStorageBufferRange;
    if (std::uint64_t{target.width} * target.height > max_range / sizeof(TemporalPixel) ||
        instances.size() > max_range / sizeof(vulkan_internal::TemporalInstanceRecord) ||
        (reservoirs && std::uint64_t{target.width} * target.height > max_range / sizeof(RestirSurfaceRecord)))
      return Evidence(FrameStatus::Fail, "temporal metadata exceeds the device's storage buffer range");
    auto batch_settings = settings;
    const auto sample_offset = frame.previous ? temporal_sample_offset_ : 0u;
    if (fresh_batch) {
      // A fresh estimator batch: never feed a cumulative progressive mean
      // back into history. Advance the RNG even on a moving view.
      accumulated_samples_ = 0;
      batch_settings.sample_index += sample_offset;
    }
    std::string detail;
    const VkDeviceSize bytes = VkDeviceSize{target.width} * target.height * sizeof(TemporalPixel);
    const VkDeviceSize validity_bytes = VkDeviceSize{target.width} * target.height * sizeof(TemporalValidityPixel);
    const VkDeviceSize radiance_bytes = VkDeviceSize{target.width} * target.height * sizeof(TemporalRadiancePixel);
    if (temporal_width_ != target.width || temporal_height_ != target.height) {
      DestroyTemporalBuffers();
      temporal_write_ = 0;
      temporal_width_ = target.width;
      temporal_height_ = target.height;
    }
    constexpr VkBufferUsageFlags surface_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    // The host mirrors are allocated by the first frame that reads back.
    const bool mirrors = target.read_back;
    if (!EnsureTemporalBuffer(temporal_surfaces_[0], bytes, surface_usage, false, detail) ||
        !EnsureTemporalBuffer(temporal_surfaces_[1], bytes, surface_usage, false, detail) ||
        (mirrors && !EnsureTemporalBuffer(temporal_readback_, bytes * 2,
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, detail)) ||
        !EnsureTemporalBuffer(temporal_validity_, validity_bytes,
            surface_usage, false, detail) ||
        (mirrors && !EnsureTemporalBuffer(temporal_validity_readback_, validity_bytes,
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, detail)) ||
        !EnsureTemporalBuffer(temporal_constants_, sizeof(TemporalConstants),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, detail) ||
        !EnsureTemporalBuffer(temporal_instances_,
            std::max<std::size_t>(instances.size(), 1) * sizeof(vulkan_internal::TemporalInstanceRecord),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, detail)) {
      failure_ = detail;
      return Evidence(FrameStatus::Fail, detail);
    }
    if (accumulate &&
        (!EnsureTemporalBuffer(temporal_radiance_[0], radiance_bytes, surface_usage, false, detail) ||
            !EnsureTemporalBuffer(temporal_radiance_[1], radiance_bytes, surface_usage, false, detail) ||
            (mirrors && !EnsureTemporalBuffer(temporal_radiance_readback_,
                            radiance_bytes * 2 + radiance_bytes / 2,
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, detail)) ||
            !EnsureTemporalBuffer(temporal_batch_, radiance_bytes / 2, surface_usage, false, detail))) {
      failure_ = detail;
      return Evidence(FrameStatus::Fail, detail);
    }
    // Two ping-pong halves of illumination and variance and the guides, 16
    // bytes per pixel each.
    const VkDeviceSize filter_bytes = radiance_bytes / 2 * 3;
    if (filter_iterations != 0 &&
        !EnsureTemporalBuffer(temporal_filter_, filter_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, detail)) {
      failure_ = detail;
      return Evidence(FrameStatus::Fail, detail);
    }
    const auto viewport = DisplayViewport(target);
    const auto scissor = DataScissor(target);
    TemporalConstants constants{};
    constants.current_view = snapshot.camera.view;
    if (frame.previous) {
      constants.previous_view = frame.previous->camera.view;
      constants.previous_world_to_clip = VulkanWorldToClip(Multiply(
          frame.previous->camera.projection, frame.previous->camera.view));
    }
    constants.previous_display = {viewport.x, viewport.y, viewport.width, viewport.height};
    constants.previous_data = {static_cast<std::uint32_t>(scissor.offset.x),
        static_cast<std::uint32_t>(scissor.offset.y), scissor.extent.width, scissor.extent.height};
    constants.validation = {requested.validation.absolute_depth_tolerance,
        requested.validation.relative_depth_tolerance, requested.validation.minimum_normal_cosine,
        requested.validation.max_motion_pixels};
    constants.history = {frame.previous ? 1u : 0u, target.width, target.height, 0u};
    constants.radiance = {accumulate ? 1u : 0u, frame_count,
        requested.max_history_samples, filter_iterations};
    if (!WriteTemporalBuffer(temporal_constants_, &constants, sizeof(constants), detail) ||
        (!instances.empty() && !WriteTemporalBuffer(temporal_instances_, instances.data(),
                                   instances.size() * sizeof(instances[0]), detail))) {
      failure_ = detail;
      return Evidence(FrameStatus::Fail, detail);
    }
    const VkDescriptorBufferInfo buffers[5] = {
        {temporal_constants_.buffer, 0, sizeof(constants)},
        {temporal_instances_.buffer, 0, VK_WHOLE_SIZE},
        {temporal_surfaces_[temporal_write_].buffer, 0, bytes},
        {temporal_surfaces_[1 - temporal_write_].buffer, 0, bytes},
        {temporal_validity_.buffer, 0, validity_bytes}};
    VkWriteDescriptorSet writes[5]{};
    for (std::uint32_t i = 0; i < 5; ++i) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = ray_descriptor_;
      writes[i].dstBinding = 8 + i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                        : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[i].pBufferInfo = &buffers[i];
    }
    vkUpdateDescriptorSets(device_, 5, writes, 0, nullptr);
    if (accumulate) {
      const VkDescriptorBufferInfo radiance_buffers[3] = {
          {temporal_batch_.buffer, 0, VK_WHOLE_SIZE},
          {temporal_radiance_[temporal_write_].buffer, 0, radiance_bytes},
          {temporal_radiance_[1 - temporal_write_].buffer, 0, radiance_bytes}};
      VkWriteDescriptorSet radiance_writes[3]{};
      for (std::uint32_t i = 0; i < 3; ++i) {
        radiance_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        radiance_writes[i].dstSet = ray_descriptor_;
        radiance_writes[i].dstBinding = 13 + i;
        radiance_writes[i].descriptorCount = 1;
        radiance_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        radiance_writes[i].pBufferInfo = &radiance_buffers[i];
      }
      vkUpdateDescriptorSets(device_, 3, radiance_writes, 0, nullptr);
    }
    if (filter_iterations != 0) {
      const VkDescriptorBufferInfo filter{temporal_filter_.buffer, 0, filter_bytes};
      VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      write.dstSet = ray_descriptor_;
      write.dstBinding = 21;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      write.pBufferInfo = &filter;
      vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }
    restir_history_available_ = reuse && frame.previous.has_value();
    restir_spatial_active_ = settings.restir_spatial;
    if (reservoirs && !EnsureRestirHistory(target, detail)) {
      failure_ = detail;
      return Evidence(FrameStatus::Fail, detail);
    }
    rendering_temporal_ = true;
    auto evidence = RenderScene(ExtractDrawSummary(snapshot), target, frame_count, batch_settings);
    rendering_temporal_ = false;
    if (evidence.status != FrameStatus::Pass) {
      // Input was validated before histories were prepared. A later failure
      // may have replaced GPU buffers; do not permit reuse of old snapshots.
      failure_ = evidence.detail;
      return evidence;
    }

    constants.radiance[1] = evidence.samples_per_pixel;
    if (!WriteTemporalBuffer(temporal_constants_, &constants, sizeof(constants), detail)) {
      failure_ = detail;
      return Evidence(FrameStatus::Fail, detail);
    }
    if (!RecordTemporal(target, frame.previous.has_value(), accumulate, filter_iterations,
            target.read_back, bytes, detail)) {
      failure_ = detail;
      return Evidence(FrameStatus::Fail, detail);
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_;
    if (!VulkanOk(vkResetFences(device_, 1, &fence_), "vkResetFences(temporal)", detail) ||
        !VulkanOk(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit(temporal)", detail) ||
        !VulkanOk(vkWaitForFences(device_, 1, &fence_, VK_TRUE, 10'000'000'000ULL),
            "vkWaitForFences(temporal)", detail) ||
        (target.read_back &&
            (!InvalidateIfNeeded(device_, temporal_readback_.memory,
                 temporal_readback_.coherent, detail) ||
                !InvalidateIfNeeded(device_, temporal_validity_readback_.memory,
                    temporal_validity_readback_.coherent, detail) ||
                (accumulate &&
                    (!InvalidateIfNeeded(device_, temporal_radiance_readback_.memory,
                         temporal_radiance_readback_.coherent, detail) ||
                        !InvalidateIfNeeded(device_, scene_targets_.color_readback_memory,
                            scene_targets_.color_readback_coherent, detail)))))) {
      failure_ = detail;
      return Evidence(FrameStatus::Fail, detail);
    }
    auto& products = evidence.temporal;
    products.available = true;
    products.history_available = frame.previous.has_value();
    products.reset_reason = frame.reset_reason;
    products.width = target.width;
    products.height = target.height;
    products.row_pitch = target.width * sizeof(TemporalPixel);
    const std::size_t pixels = bytes / sizeof(TemporalPixel);
    if (target.read_back) {
      products.current.resize(pixels);
      std::memcpy(products.current.data(), temporal_readback_.mapped, bytes);
      if (frame.previous) {
        products.previous.resize(pixels);
        std::memcpy(products.previous.data(),
            static_cast<const std::uint8_t*>(temporal_readback_.mapped) + bytes, bytes);
      }
      products.validity.resize(pixels);
      std::memcpy(products.validity.data(), temporal_validity_readback_.mapped, validity_bytes);
    }
    // Device storage and whichever mirrors a read-back frame allocated.
    products.surface_bytes = bytes * 2 + temporal_readback_.bytes;
    products.validity_row_pitch = target.width * sizeof(TemporalValidityPixel);
    products.validity_bytes = validity_bytes + temporal_validity_readback_.bytes;
    products.metadata_bytes = temporal_constants_.bytes + temporal_instances_.bytes;
    if (accumulate) {
      products.radiance_available = true;
      products.radiance_row_pitch = target.width * sizeof(TemporalRadiancePixel);
      if (target.read_back) {
        products.radiance.resize(pixels);
        std::memcpy(products.radiance.data(), temporal_radiance_readback_.mapped, radiance_bytes);
        if (frame.previous) {
          products.previous_radiance.resize(pixels);
          std::memcpy(products.previous_radiance.data(),
              static_cast<const std::uint8_t*>(temporal_radiance_readback_.mapped) + radiance_bytes,
              radiance_bytes);
        }
        products.batch.resize(pixels);
        std::memcpy(products.batch.data(),
            static_cast<const std::uint8_t*>(temporal_radiance_readback_.mapped) + radiance_bytes * 2,
            radiance_bytes / 2);
        std::memcpy(evidence.color.payload.data(), scene_targets_.color_mapped,
            evidence.color.payload.size());
      }
      products.radiance_bytes = radiance_bytes * 2 + temporal_radiance_readback_.bytes + temporal_batch_.bytes;
      products.batch_samples = evidence.samples_per_pixel;
      products.batch_sample_index = batch_settings.sample_index;
      products.filter_iterations = filter_iterations;
      products.filter_bytes = temporal_filter_.bytes;
    }
    if (temporal_timestamps_ != VK_NULL_HANDLE) {
      std::uint64_t ticks[kTemporalTimestampQueries]{};
      // Only the passes recorded this frame wrote their queries.
      const std::uint32_t queries = filter_iterations != 0 ? kTemporalTimestampQueries
                                    : accumulate           ? 8u
                                                           : 4u;
      if (!VulkanOk(vkGetQueryPoolResults(device_, temporal_timestamps_, 0, queries,
                        sizeof(ticks), ticks, sizeof(ticks[0]),
                        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
              "vkGetQueryPoolResults(temporal)", detail)) {
        failure_ = detail;
        return Evidence(FrameStatus::Fail, detail);
      }
      const std::uint64_t mask = timestamp_bits_ == 64 ? ~std::uint64_t{0}
                                                       : (std::uint64_t{1} << timestamp_bits_) - 1;
      products.timestamp_available = true;
      products.primary_surface_gpu_ms = static_cast<double>((ticks[1] - ticks[0]) & mask) *
                                        device_properties_.limits.timestampPeriod / 1'000'000.0;
      products.reprojection_gpu_ms = static_cast<double>((ticks[3] - ticks[2]) & mask) *
                                     device_properties_.limits.timestampPeriod / 1'000'000.0;
      if (accumulate) {
        products.accumulation_gpu_ms = static_cast<double>((ticks[5] - ticks[4]) & mask) *
                                       device_properties_.limits.timestampPeriod / 1'000'000.0;
        products.variance_gpu_ms = static_cast<double>((ticks[7] - ticks[6]) & mask) *
                                   device_properties_.limits.timestampPeriod / 1'000'000.0;
      }
      if (filter_iterations != 0)
        products.filter_gpu_ms = static_cast<double>((ticks[9] - ticks[8]) & mask) *
                                 device_properties_.limits.timestampPeriod / 1'000'000.0;
    }
    // Publish only after submission, readback and timestamps all succeeded.
    temporal_presentable_ = true;
    radiance_presentable_ = accumulate;
    temporal_history_ = std::move(next_history);
    temporal_identities_ = std::move(next_identities);
    temporal_transport_key_ = fresh_batch ? std::optional{radiance_key} : std::nullopt;
    temporal_sample_offset_ = fresh_batch ? sample_offset + evidence.samples_per_pixel : 0u;
    // A fresh batch is not a progressive mean: the next progressive frame
    // starts its accumulation again rather than adding to this sample.
    if (fresh_batch)
      accumulation_key_.reset();
    temporal_write_ = 1 - temporal_write_;
    evidence.validation_message_count = validation_.message_count;
    evidence.validation_detail = validation_.first_message.empty()
                                     ? instance_state_.validation_detail
                                     : validation_.first_message;
    return evidence;
  }

  void RecordTemporalSurfaces(const OffscreenTarget& target, bool accumulate,
      VkDeviceSize bytes, VkMemoryBarrier& barrier, VkRenderPassBeginInfo& render) {
    if (temporal_timestamps_ != VK_NULL_HANDLE) {
      vkCmdResetQueryPool(command_, temporal_timestamps_, 0, kTemporalTimestampQueries);
      vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, temporal_timestamps_, 0);
    }
    barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);
    vkCmdFillBuffer(command_, temporal_surfaces_[temporal_write_].buffer, 0, bytes, 0);
    if (accumulate) {
      VkBufferImageCopy batch{};
      batch.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      batch.imageExtent = {target.width, target.height, 1};
      vkCmdCopyImageToBuffer(command_, scene_targets_.color_image,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, temporal_batch_.buffer, 1, &batch);
    }
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    render = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    render.renderPass = scene_render_pass_;
    render.framebuffer = scene_targets_.framebuffer;
    render.renderArea.extent = {target.width, target.height};
    vkCmdBeginRenderPass(command_, &render, VK_SUBPASS_CONTENTS_INLINE);
    const auto viewport = DisplayViewport(target);
    const auto scissor = DataScissor(target);
    if (scene_.Tlas() != VK_NULL_HANDLE && scissor.extent.width && scissor.extent.height) {
      vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_GRAPHICS, temporal_pipeline_);
      vkCmdSetViewport(command_, 0, 1, &viewport);
      vkCmdSetScissor(command_, 0, 1, &scissor);
      vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_GRAPHICS,
          ray_layout_, 0, 1, &ray_descriptor_, 0, nullptr);
      vkCmdPushConstants(command_, ray_layout_, kRayStages, 0, sizeof(RayConstants), &ray_constants_);
      vkCmdDraw(command_, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(command_);
    if (temporal_timestamps_ != VK_NULL_HANDLE)
      vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, temporal_timestamps_, 1);
    // Make this frame's writes and the preceding frame's retained surface
    // visible to a separate fullscreen validity pass. It covers misses and
    // pixels outside the data window too; every debug record is overwritten.
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    if (temporal_timestamps_ != VK_NULL_HANDLE)
      vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, temporal_timestamps_, 2);
    vkCmdBeginRenderPass(command_, &render, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport full_viewport{0, 0, static_cast<float>(target.width), static_cast<float>(target.height), 0, 1};
    const VkRect2D full_scissor{{0, 0}, {target.width, target.height}};
    vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_GRAPHICS, temporal_validity_pipeline_);
    vkCmdSetViewport(command_, 0, 1, &full_viewport);
    vkCmdSetScissor(command_, 0, 1, &full_scissor);
    vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_GRAPHICS,
        ray_layout_, 0, 1, &ray_descriptor_, 0, nullptr);
    // The shared shader statically declares this range even when the
    // specialized validity pass does not read the camera. Initialize it
    // independently of a primary draw (which an empty scene skips).
    vkCmdPushConstants(command_, ray_layout_, kRayStages, 0, sizeof(RayConstants), &ray_constants_);
    vkCmdDraw(command_, 3, 1, 0, 0);
    vkCmdEndRenderPass(command_);
    if (temporal_timestamps_ != VK_NULL_HANDLE)
      vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, temporal_timestamps_, 3);
  }

  // Without `read_back` the products stay on the GPU: the surfaces,
  // validity and radiance are not copied to their host mirrors.
  bool RecordTemporal(const OffscreenTarget& target, bool previous,
      bool accumulate, std::uint32_t filter_iterations, bool read_back,
      VkDeviceSize bytes, std::string& detail) {
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (!VulkanOk(vkBeginCommandBuffer(command_, &begin),
            "vkBeginCommandBuffer(temporal)", detail))
      return false;
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    VkRenderPassBeginInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    if (!restir_temporal_active_)
      RecordTemporalSurfaces(target, accumulate, bytes, barrier, render);
    else {
      // Reuse already ran the shared metadata before transport. Retain its
      // timestamps and validity; only stage the fresh radiance batch here.
      render.renderPass = scene_render_pass_;
      render.framebuffer = scene_targets_.framebuffer;
      render.renderArea.extent = {target.width, target.height};
      if (accumulate) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        VkBufferImageCopy batch{};
        batch.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        batch.imageExtent = {target.width, target.height, 1};
        vkCmdCopyImageToBuffer(command_, scene_targets_.color_image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, temporal_batch_.buffer, 1, &batch);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
      }
    }
    const VkViewport full_viewport{0, 0, static_cast<float>(target.width), static_cast<float>(target.height), 0, 1};
    const VkRect2D full_scissor{{0, 0}, {target.width, target.height}};
    if (accumulate) {
      barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
      if (temporal_timestamps_ != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, temporal_timestamps_, 4);
      vkCmdBeginRenderPass(command_, &render, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_GRAPHICS, temporal_accumulation_pipeline_);
      vkCmdSetViewport(command_, 0, 1, &full_viewport);
      vkCmdSetScissor(command_, 0, 1, &full_scissor);
      vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_GRAPHICS,
          ray_layout_, 0, 1, &ray_descriptor_, 0, nullptr);
      vkCmdPushConstants(command_, ray_layout_, kRayStages, 0, sizeof(RayConstants), &ray_constants_);
      vkCmdDraw(command_, 3, 1, 0, 0);
      vkCmdEndRenderPass(command_);
      if (temporal_timestamps_ != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, temporal_timestamps_, 5);
      // The variance pass reads every neighbour's accumulated moments.
      barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
      if (temporal_timestamps_ != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, temporal_timestamps_, 6);
      vkCmdBeginRenderPass(command_, &render, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_GRAPHICS, temporal_variance_pipeline_);
      vkCmdSetViewport(command_, 0, 1, &full_viewport);
      vkCmdSetScissor(command_, 0, 1, &full_scissor);
      vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_GRAPHICS,
          ray_layout_, 0, 1, &ray_descriptor_, 0, nullptr);
      vkCmdPushConstants(command_, ray_layout_, kRayStages, 0, sizeof(RayConstants), &ray_constants_);
      vkCmdDraw(command_, 3, 1, 0, 0);
      vkCmdEndRenderPass(command_);
      if (temporal_timestamps_ != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, temporal_timestamps_, 7);
    }
    if (filter_iterations != 0) {
      if (temporal_timestamps_ != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, temporal_timestamps_, 8);
      // Each iteration reads its neighbours' variance and the preceding
      // iteration's output; the last one writes the colour attachment.
      for (std::uint32_t iteration = 0; iteration < filter_iterations; ++iteration) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        vkCmdBeginRenderPass(command_, &render, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_GRAPHICS,
            iteration + 1 == filter_iterations ? temporal_composite_pipeline_ : temporal_filter_pipeline_);
        vkCmdSetViewport(command_, 0, 1, &full_viewport);
        vkCmdSetScissor(command_, 0, 1, &full_scissor);
        vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_GRAPHICS,
            ray_layout_, 0, 1, &ray_descriptor_, 0, nullptr);
        vkCmdPushConstants(command_, ray_layout_, kRayStages, 0, sizeof(RayConstants), &ray_constants_);
        vkCmdDraw(command_, 3, 1, 0, iteration);
        vkCmdEndRenderPass(command_);
      }
      if (temporal_timestamps_ != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, temporal_timestamps_, 9);
    }
    if (!read_back)
      return VulkanOk(vkEndCommandBuffer(command_), "vkEndCommandBuffer(temporal)", detail);
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);
    VkBufferCopy copy{0, 0, bytes};
    vkCmdCopyBuffer(command_, temporal_surfaces_[temporal_write_].buffer,
        temporal_readback_.buffer, 1, &copy);
    if (previous) {
      copy.dstOffset = bytes;
      vkCmdCopyBuffer(command_, temporal_surfaces_[1 - temporal_write_].buffer,
          temporal_readback_.buffer, 1, &copy);
    }
    copy = {0, 0, VkDeviceSize{target.width} * target.height * sizeof(TemporalValidityPixel)};
    vkCmdCopyBuffer(command_, temporal_validity_.buffer,
        temporal_validity_readback_.buffer, 1, &copy);
    if (accumulate) {
      const VkDeviceSize radiance_bytes = VkDeviceSize{target.width} * target.height * sizeof(TemporalRadiancePixel);
      copy = {0, 0, radiance_bytes};
      vkCmdCopyBuffer(command_, temporal_radiance_[temporal_write_].buffer,
          temporal_radiance_readback_.buffer, 1, &copy);
      if (previous) {
        copy.dstOffset = radiance_bytes;
        vkCmdCopyBuffer(command_, temporal_radiance_[1 - temporal_write_].buffer,
            temporal_radiance_readback_.buffer, 1, &copy);
      }
      copy = {0, radiance_bytes * 2, radiance_bytes / 2};
      vkCmdCopyBuffer(command_, temporal_batch_.buffer, temporal_radiance_readback_.buffer, 1, &copy);
      VkBufferImageCopy color{};
      color.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      color.imageExtent = {target.width, target.height, 1};
      vkCmdCopyImageToBuffer(command_, scene_targets_.color_image,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scene_targets_.color_readback, 1, &color);
    }
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    return VulkanOk(vkEndCommandBuffer(command_), "vkEndCommandBuffer(temporal)", detail);
  }

  static bool RestoresContents(const OffscreenTarget& target) {
    return (!target.clear_color_enabled && !target.preserved_color.empty()) ||
           (!target.clear_depth_enabled && !target.preserved_depth.empty());
  }

  // Reuse the readback mirrors as upload staging while no frame is in
  // flight. The restoration copy finishes before readback overwrites them.
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
                                                std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F))
                                          : 0;
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
    constants.light_sampling = settings.light_sampling == LightSampling::Mis
                                   ? kMisLightSampling
                               : settings.light_sampling == LightSampling::RestirInitial
                                   ? kRestirInitialSampling : kBsdfSampling;
    constants.restir_candidates = scene_.Tlas() == VK_NULL_HANDLE ? 0 : settings.restir_candidates;
    constants.restir_temporal = settings.restir_temporal || settings.restir_spatial ? 1u : 0u;
    constants.restir_history = settings.restir_temporal && restir_history_available_ ? 1u : 0u;
    constants.restir_max_history_candidates = settings.restir_max_history_candidates;
    constants.restir_spatial = settings.restir_spatial ? 1u : 0u;
    constants.restir_spatial_radius = settings.restir_spatial_radius;
    constants.restir_proposal = static_cast<std::uint32_t>(settings.restir_proposal);
    constants.light_offsets = scene_.LightOffsetAddress();
    constants.lights = scene_.LightAddress();
    constants.light_count = scene_.LightCount();
    constants.analytic_lights = scene_.AnalyticLightAddress();
    constants.analytic_count = scene_.AnalyticLightCount();
    const bool hierarchy = settings.light_crossing == LightCrossing::Hierarchy;
    constants.light_nodes = hierarchy ? scene_.LightNodeAddress() : 0;
    constants.light_node_count = hierarchy ? scene_.LightNodeCount() : 0;
    constants.distant_count = hierarchy ? scene_.DistantLightCount() : 0;
    constants.environment_map = scene_.EnvironmentAddress();
    constants.environment_selection = scene_.EnvironmentSelection();
    constants.environment_background = settings.environment_background ? 1U : 0U;
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
        CreateDeviceBuffer(physical_device_, device_, slots * kPathStateBytes,
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
    const std::uint32_t rounds = recorded_wavefront_.rounds;
    const std::uint32_t queries = WavefrontTimestamps(rounds);
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
    timings.camera_gpu_ms = ms(0);
    timings.shade_gpu_ms = ms(1);
    for (std::uint32_t round = 1; round < rounds; ++round) {
      timings.intersect_gpu_ms += ms(2 * round);
      timings.shade_gpu_ms += ms(1 + 2 * round);
    }
    timings.tail_gpu_ms = ms(queries - 2);
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
      uploaded_scene_ = update.source_scene;
      evidence.timings = scene_.Timings();
    }
    evidence.stats = scene_.Stats();
    evidence.validation_message_count = validation_.message_count;
    return evidence;
  }

  PresentEvidence Present(std::uint32_t width, std::uint32_t height,
      const PresentView& view) override {
    auto Fail = [](std::string detail) {
      PresentEvidence evidence;
      evidence.status = FrameStatus::Fail;
      evidence.detail = std::move(detail);
      return evidence;
    };
    if (!failure_.empty())
      return Fail("the renderer failed earlier: " + failure_);
    if (!presentation_)
      return Fail("the renderer was created without a presentation surface");
    if (!presentable_)
      return Fail("no scene frame has been rendered to present");
    vulkan_internal::PresentSource source;
    source.color = scene_targets_.color_image;
    source.extent = {scene_targets_.width, scene_targets_.height};
    source.view = view;
    if (view.view != DebugView::Beauty) {
      if (!presentation_->has_view_kernel())
        return Fail("debug views need the present_view kernel");
      // The current records: RenderTemporalScene has flipped the write index.
      switch (view.view) {
      case DebugView::Depth:
      case DebugView::Normal:
      case DebugView::Motion:
      case DebugView::TemporalValidity:
        if (!temporal_presentable_)
          return Fail("the latest frame wrote no temporal products; render "
                      "it with RenderTemporalScene");
        source.product = view.view == DebugView::TemporalValidity
                             ? temporal_validity_.buffer
                             : temporal_surfaces_[1 - temporal_write_].buffer;
        source.stride = view.view == DebugView::TemporalValidity
                            ? sizeof(TemporalValidityPixel) / 16
                            : sizeof(TemporalPixel) / 16;
        break;
      case DebugView::RawRadiance:
      case DebugView::AccumulatedRadiance:
      case DebugView::HistoryLength:
      case DebugView::Variance:
        if (!temporal_presentable_ || !radiance_presentable_)
          return Fail("the latest frame wrote no temporal radiance; render "
                      "it with accumulate_radiance");
        if (view.view == DebugView::RawRadiance) {
          source.product = temporal_batch_.buffer;
          source.stride = sizeof(std::array<float, 4>) / 16;
        } else {
          source.product = temporal_radiance_[1 - temporal_write_].buffer;
          source.stride = sizeof(TemporalRadiancePixel) / 16;
        }
        break;
      default:
        if (restir_presentable_ == RestirPresentable::None)
          return Fail("the latest frame wrote no reservoirs; render it with "
                      "RestirInitial");
        if (restir_presentable_ == RestirPresentable::Reuse) {
          source.product = restir_history_buffers_[1 - temporal_write_].buffer;
          source.stride = sizeof(RestirReusePixel) / 16;
        } else {
          source.product = restir_initial_.buffer;
          source.stride = sizeof(RestirPixel) / 16;
        }
        break;
      }
    }
    PresentEvidence evidence = presentation_->Present(source, width, height);
    if (evidence.status == FrameStatus::Fail)
      failure_ = evidence.detail;
    evidence.validation_message_count = validation_.message_count;
    evidence.validation_detail = validation_.first_message.empty()
                                     ? instance_state_.validation_detail
                                     : validation_.first_message;
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
      if (candidate &&
          (!presentation_ || presentation_->Supports(physical, *candidate))) {
        physical_device_ = physical;
        queue_family_ = *candidate;
        break;
      }
    }
    if (physical_device_ == VK_NULL_HANDLE) {
      status = FrameStatus::Skip;
      detail = presentation_
                   ? "no Vulkan physical device presents to the window "
                     "from a graphics queue with VK_KHR_swapchain"
                   : "no Vulkan physical device exposes a graphics queue";
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
    memory_budget_ = vulkan_internal::HasDeviceExtension(physical_device_,
        VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
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
    }
    if (presentation_)
      extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (memory_budget_)
      extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    device_create.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    device_create.ppEnabledExtensionNames = extensions.data();
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
    VkDescriptorSetLayoutBinding bindings[24]{};
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
    // The wavefront kernels' queue state, queues and paths; the resolve
    // pass reads the paths. They exist only after the first wavefront
    // frame, and no other pass reads them.
    for (std::uint32_t index = 5; index < 8; ++index) {
      bindings[index].binding = index;
      bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      bindings[index].descriptorCount = 1;
      bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    bindings[7].stageFlags |= VK_SHADER_STAGE_FRAGMENT_BIT;
    for (std::uint32_t index = 8; index < 24; ++index) {
      bindings[index].binding = index;
      bindings[index].descriptorType = index == 8
                                           ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                           : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      bindings[index].descriptorCount = 1;
      bindings[index].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    for (unsigned index = 16; index < 24; ++index)
      if (index != 21)
        bindings[index].stageFlags |= VK_SHADER_STAGE_COMPUTE_BIT;
    std::array<VkDescriptorBindingFlags, 24> binding_flags{};
    for (unsigned index = 0; index < binding_flags.size(); ++index)
      if (index == 0 || index == 3 || index >= 7)
        binding_flags[index] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flags.bindingCount = 24;
    flags.pBindingFlags = binding_flags.data();
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.pNext = &flags;
    layout.bindingCount = 24;
    layout.pBindings = bindings;
    if (!VulkanOk(vkCreateDescriptorSetLayout(device_, &layout, nullptr,
                      &ray_descriptor_layout_),
            "vkCreateDescriptorSetLayout(ray)", detail))
      return false;
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxTextures},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kTextureSamplers},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 18}};
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
      query.queryCount = kTemporalTimestampQueries;
      if (!VulkanOk(vkCreateQueryPool(device_, &query, nullptr, &temporal_timestamps_),
              "vkCreateQueryPool(temporal surface)", detail))
        return false;
      query.queryCount = 6;
      if (!VulkanOk(vkCreateQueryPool(device_, &query, nullptr, &restir_timestamps_),
              "vkCreateQueryPool(initial reservoir)", detail))
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
    // A presentation's blit may still read the scene colour.
    if (presentation_ && targets.framebuffer != VK_NULL_HANDLE &&
        !VulkanOk(vkQueueWaitIdle(queue_), "vkQueueWaitIdle", detail))
      return false;
    DestroyTargets(targets);
    if (&targets == &scene_targets_) {
      accumulation_key_.reset();
    }
    targets.width = width;
    targets.height = height;
    if (!CreateTargets(targets, &targets == &scene_targets_ ? scene_render_pass_ : render_pass_,
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
    return true;
  }

  // Created by the first frame at this size that reads back or restores;
  // they persist with the targets. A failure leaves neither.
  bool EnsureTargetMirrors(Targets& t, std::string& detail) {
    if (t.color_mapped != nullptr && t.depth_mapped != nullptr)
      return true;
    DestroyTargetMirrors(t);
    const VkDeviceSize pixel_count = VkDeviceSize{t.width} * t.height;
    const VkDeviceSize color_bytes = pixel_count * t.color_pixel_bytes;
    const VkDeviceSize depth_bytes = pixel_count * sizeof(float);
    if (CreateHostBuffer(physical_device_, device_, color_bytes,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            t.color_readback,
            t.color_readback_memory, t.color_readback_coherent, detail) &&
        CreateHostBuffer(physical_device_, device_, depth_bytes,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            t.depth_readback,
            t.depth_readback_memory, t.depth_readback_coherent, detail) &&
        VulkanOk(vkMapMemory(device_, t.color_readback_memory, 0,
                     VK_WHOLE_SIZE, 0, &t.color_mapped),
            "vkMapMemory(color)", detail) &&
        VulkanOk(vkMapMemory(device_, t.depth_readback_memory, 0,
                     VK_WHOLE_SIZE, 0, &t.depth_mapped),
            "vkMapMemory(depth)", detail))
      return true;
    DestroyTargetMirrors(t);
    return false;
  }

  void DestroyTargetMirrors(Targets& t) {
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
    t.color_readback = t.depth_readback = VK_NULL_HANDLE;
    t.color_readback_memory = t.depth_readback_memory = VK_NULL_HANDLE;
    t.color_mapped = t.depth_mapped = nullptr;
  }

  // Only called while no frame is in flight: Render waits for every
  // submission before it returns. Keeps the colour format.
  void DestroyTargets(Targets& t) {
    DestroyWavefront(t);
    DestroyTargetMirrors(t);
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
      Targets& targets, FramePass pass, const WavefrontSchedule& wavefront,
      bool restir, bool restore, std::string& detail) {
    const bool trace = pass != FramePass::Bootstrap;
    VkCommandBufferBeginInfo command_begin{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (!VulkanOk(vkBeginCommandBuffer(command_, &command_begin),
            "vkBeginCommandBuffer", detail)) {
      return false;
    }
    if (trace && ray_timestamps_ != VK_NULL_HANDLE)
      vkCmdResetQueryPool(command_, ray_timestamps_, 0,
          wavefront.bounces == 0
              ? kSceneTimestamps
              : kSceneTimestamps + WavefrontTimestamps(wavefront.rounds));
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
    recorded_wavefront_ = {};
    if (restir_temporal_active_) {
      VkMemoryBarrier metadata_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      VkRenderPassBeginInfo metadata_render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      RecordTemporalSurfaces(target, false,
          VkDeviceSize{target.width} * target.height * sizeof(TemporalPixel),
          metadata_barrier, metadata_render);
    }
    if (restir)
      RecordRestir(target, render_begin);
    if (drawn && wavefront.bounces != 0) {
      RecordWavefront(scissor, targets, wavefront);
      recorded_wavefront_ = wavefront;
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
              recorded_wavefront_.bounces != 0 ? resolve_pipeline_
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

    if (restir && target.read_back) {
      VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          0, 1, &barrier, 0, nullptr, 0, nullptr);
      const VkBufferCopy copy{0, 0, VkDeviceSize{target.width} * target.height * sizeof(RestirPixel)};
      vkCmdCopyBuffer(command_, restir_initial_.buffer, restir_readback_.buffer, 1, &copy);
      if (restir_temporal_active_) {
        VkBufferCopy history_copy{0, 0,
            VkDeviceSize{target.width} * target.height * sizeof(RestirReusePixel)};
        vkCmdCopyBuffer(command_, restir_history_buffers_[temporal_write_].buffer,
            restir_history_readback_.buffer, 1, &history_copy);
        if (restir_spatial_active_) {
          vkCmdCopyBuffer(command_, restir_spatial_input_.buffer,
              restir_spatial_input_readback_.buffer, 1, &history_copy);
          const VkBufferCopy sources_copy{0, 0, VkDeviceSize{target.width} * target.height * 16};
          vkCmdCopyBuffer(command_, restir_spatial_sources_.buffer,
              restir_spatial_sources_readback_.buffer, 1, &sources_copy);
        }
        if (restir_history_available_) {
          history_copy.dstOffset = history_copy.size;
          vkCmdCopyBuffer(command_, restir_history_buffers_[1 - temporal_write_].buffer,
              restir_history_readback_.buffer, 1, &history_copy);
        }
      }
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    if (restore_color || restore_depth) {
      // Upload staging becomes readback storage in this submission.
      VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
    if (!target.read_back)
      return VulkanOk(vkEndCommandBuffer(command_), "vkEndCommandBuffer",
          detail);
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

  // The wavefront integrator's sample: camera paths for the data window
  // and their camera rays, the schedule's rounds of shade and (after the
  // first) intersect, and the tail for paths still queued after them. Each
  // path leaves its sample in its slot for the resolve pass. A round after
  // the queues empty dispatches no groups. The queue state is copied back
  // for the path counts.
  void RecordWavefront(const VkRect2D& scissor, Targets& targets,
      const WavefrontSchedule& schedule) {
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
    const auto dispatch_indirect = [&](WavefrontKernel kernel,
                                       std::uint32_t word) {
      vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_COMPUTE,
          wavefront_pipelines_[kernel]);
      vkCmdDispatchIndirect(command_, targets.queue_state,
          VkDeviceSize{word} * sizeof(std::uint32_t));
    };

    // The preceding submission's kernels, resolve pass and copy have
    // finished with the counters and paths this one resets and rewrites.
    barrier(kKernels | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
            VK_ACCESS_SHADER_WRITE_BIT);
    // Every queue the frame can reach starts empty, with a one-row
    // dispatch of no groups, and both kernels' bounces at 0.
    std::vector<std::uint32_t> state(RayQueueWord(schedule.bounces + 1), 0);
    const auto empty = [&](std::uint32_t descriptor) {
      state[descriptor + 1] = state[descriptor + 2] = 1;
    };
    for (std::uint32_t word = kBouncesWord; word < state.size();
        word += kQueueDescriptorWords)
      empty(word);
    vkCmdUpdateBuffer(command_, targets.queue_state, 0,
        state.size() * sizeof(std::uint32_t), state.data());
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
    vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_COMPUTE,
        wavefront_pipelines_[kCameraKernel]);
    vkCmdDispatch(command_,
        static_cast<std::uint32_t>(std::min<std::uint64_t>(groups,
            kWavefrontMaxGroupsX)),
        static_cast<std::uint32_t>(
            (groups + kWavefrontMaxGroupsX - 1) / kWavefrontMaxGroupsX),
        1);
    kernel_barrier();
    timestamp(1);
    for (std::uint32_t round = 0; round < schedule.rounds; ++round) {
      // The camera kernel traced the first round's rays.
      if (round != 0) {
        dispatch_indirect(kIntersectKernel, RayQueueWord(round));
        kernel_barrier();
        timestamp(1 + 2 * round);
      }
      dispatch_indirect(kShadeKernel, HitQueueWord(round));
      kernel_barrier();
      timestamp(2 + 2 * round);
    }
    // No path continues past the last bounce.
    if (schedule.rounds < schedule.bounces)
      dispatch_indirect(kTailKernel, RayQueueWord(schedule.rounds));
    timestamp(1 + 2 * schedule.rounds);
    // The resolve pass reads the paths' samples; the host reads the
    // counters.
    barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    const VkBufferCopy copy{
        0, 0, state.size() * sizeof(std::uint32_t)};
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
  // VK_EXT_memory_budget, enabled when the device exposes it.
  bool memory_budget_ = false;
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
  VkPipeline temporal_pipeline_ = VK_NULL_HANDLE;
  VkPipeline temporal_validity_pipeline_ = VK_NULL_HANDLE;
  VkPipeline temporal_accumulation_pipeline_ = VK_NULL_HANDLE;
  VkPipeline temporal_variance_pipeline_ = VK_NULL_HANDLE;
  VkPipeline temporal_filter_pipeline_ = VK_NULL_HANDLE;
  VkPipeline temporal_composite_pipeline_ = VK_NULL_HANDLE;
  VkQueryPool temporal_timestamps_ = VK_NULL_HANDLE;
  VkPipeline restir_pipeline_ = VK_NULL_HANDLE;
  VkPipeline restir_temporal_pipeline_ = VK_NULL_HANDLE;
  VkPipeline restir_spatial_pipeline_ = VK_NULL_HANDLE;
  TemporalBuffer restir_spatial_input_;
  TemporalBuffer restir_spatial_input_readback_;
  TemporalBuffer restir_spatial_sources_;
  TemporalBuffer restir_spatial_sources_readback_;
  std::array<TemporalBuffer, 2> restir_history_buffers_{};
  TemporalBuffer restir_history_readback_;
  std::array<TemporalBuffer, 2> restir_surfaces_{};
  bool restir_history_available_ = false;
  bool restir_temporal_active_ = false;
  bool restir_spatial_active_ = false;
  VkQueryPool restir_timestamps_ = VK_NULL_HANDLE;
  TemporalBuffer restir_initial_;
  TemporalBuffer restir_readback_;
  TemporalHistory temporal_history_;
  vulkan_internal::TemporalIdentities temporal_identities_;
  std::optional<TemporalTransportKey> temporal_transport_key_;
  std::uint32_t temporal_sample_offset_ = 0;
  std::shared_ptr<const LotusScene> uploaded_scene_;
  std::array<TemporalBuffer, 2> temporal_surfaces_{};
  TemporalBuffer temporal_readback_;
  TemporalBuffer temporal_validity_;
  TemporalBuffer temporal_validity_readback_;
  std::array<TemporalBuffer, 2> temporal_radiance_{};
  TemporalBuffer temporal_radiance_readback_;
  TemporalBuffer temporal_batch_;
  TemporalBuffer temporal_filter_;
  TemporalBuffer temporal_constants_;
  TemporalBuffer temporal_instances_;
  std::uint32_t temporal_width_ = 0;
  std::uint32_t temporal_height_ = 0;
  std::uint32_t temporal_write_ = 0;
  bool rendering_temporal_ = false;
  // One per WavefrontKernel; null without the wavefront shader.
  std::array<VkPipeline, kWavefrontKernels> wavefront_pipelines_{};
  // The schedule the command buffer's wavefront kernels were recorded with;
  // no bounces when it has none.
  WavefrontSchedule recorded_wavefront_{};
  // The rounds the next wavefront call records, from the last sample's
  // queue occupancy; 0 before the first.
  std::uint32_t wavefront_rounds_hint_ = 0;
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
  // The window's swapchain; null for an offscreen renderer.
  std::unique_ptr<vulkan_internal::Presentation> presentation_;
  // Whether the latest frame was a scene frame, which Present copies.
  bool presentable_ = false;
  // Which debug products Present may show: the temporal surfaces and
  // validity after a RenderTemporalScene frame, and its radiance products
  // when it accumulated radiance; the initial or the reused reservoirs after
  // a RestirInitial frame.
  bool temporal_presentable_ = false;
  bool radiance_presentable_ = false;
  enum class RestirPresentable { None, Initial, Reuse };
  RestirPresentable restir_presentable_ = RestirPresentable::None;
};

#endif

} // namespace

std::unique_ptr<OffscreenRenderer> CreateOffscreenRenderer(
    const std::string& vertex_shader, const std::string& fragment_shader,
    FrameStatus& status, std::string& error, const RayQueryShaders& ray_query_shaders,
    const PresentOptions* presentation) {
#if !defined(LOTUS_HAS_VULKAN)
  (void)vertex_shader;
  (void)fragment_shader;
  (void)ray_query_shaders;
  (void)presentation;
  status = FrameStatus::Skip;
  error = "Vulkan backend was not compiled for this configuration";
  return nullptr;
#else
  auto renderer = std::make_unique<VulkanOffscreenRenderer>();
  if (!renderer->Initialize(vertex_shader, fragment_shader, status, error,
          ray_query_shaders, presentation)) {
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
