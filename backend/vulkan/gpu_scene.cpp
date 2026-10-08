// SPDX-License-Identifier: Apache-2.0
#include "gpu_scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "vulkan_internal.hpp"

namespace Lotus::vulkan_internal {

namespace {

constexpr VkDeviceSize kVec3Bytes = 3 * sizeof(float);
static_assert(sizeof(std::array<float, 3>) == kVec3Bytes &&
              sizeof(std::array<std::uint32_t, 3>) == kVec3Bytes);
constexpr VkDeviceSize kVec2Bytes = 2 * sizeof(float);
static_assert(sizeof(std::array<float, 2>) == kVec2Bytes);
// Texel copies start at multiples of the largest texel.
constexpr VkDeviceSize kTexelAlignment = 16;
constexpr VkDeviceSize kInstanceBytes = sizeof(GpuInstanceRecord);
constexpr VkDeviceSize kMaterialBytes = sizeof(GpuMaterialRecord);
constexpr VkDeviceSize kTlasInstanceBytes =
    sizeof(VkAccelerationStructureInstanceKHR);
static_assert(kTlasInstanceBytes == 64);

// Device allocations left for the renderer's targets and readback buffers.
constexpr std::uint32_t kReservedAllocations = 16;

constexpr std::uint32_t kNoSlot = std::numeric_limits<std::uint32_t>::max();

// One timestamp before Apply's copies and one after each of its phases.
constexpr std::uint32_t kTimestampCount = 4;

VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

// Where a geometry buffer's triangles, corner normals and texture-coordinate
// sets start, and its size. Each section is aligned so a pass could bind it
// as its own storage buffer range; `normal_offset` is zero without normals.
struct GeometryLayout {
  VkDeviceSize index_offset = 0;
  VkDeviceSize normal_offset = 0;
  std::vector<VkDeviceSize> texcoord_offsets;
  VkDeviceSize size = 0;
};

GeometryLayout LayOut(const MeshGeometry& geometry, VkDeviceSize alignment) {
  GeometryLayout layout;
  layout.index_offset =
      AlignUp(geometry.positions.size() * kVec3Bytes, alignment);
  layout.size = layout.index_offset + geometry.triangles.size() * kVec3Bytes;
  if (!geometry.normals.empty()) {
    layout.normal_offset = AlignUp(layout.size, alignment);
    layout.size = layout.normal_offset + geometry.normals.size() * kVec3Bytes;
  }
  for (const auto& [name, texcoords] : geometry.texcoords) {
    (void)name;
    layout.texcoord_offsets.push_back(AlignUp(layout.size, alignment));
    layout.size = layout.texcoord_offsets.back() + texcoords.size() * kVec2Bytes;
  }
  return layout;
}

VkFormat TextureVkFormat(TextureFormat format) {
  switch (format) {
  case TextureFormat::Rgba8Unorm:
    return VK_FORMAT_R8G8B8A8_UNORM;
  case TextureFormat::Rgba8Srgb:
    return VK_FORMAT_R8G8B8A8_SRGB;
  case TextureFormat::Rgba32Float:
    return VK_FORMAT_R32G32B32A32_SFLOAT;
  }
  return VK_FORMAT_UNDEFINED;
}

VkDeviceSize TextureBytes(const Texture& texture) {
  return VkDeviceSize{texture.width} * texture.height *
         TexelBytes(texture.format);
}

bool CreateBuffer(VkPhysicalDevice physical_device, VkDevice device,
    VkDeviceSize size, VkBufferUsageFlags usage,
    VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
    DeviceBuffer& buffer, std::string& detail) {
  VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  create.size = size;
  create.usage = usage;
  create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (!VulkanOk(vkCreateBuffer(device, &create, nullptr, &buffer.buffer),
          "vkCreateBuffer(scene)", detail)) {
    return false;
  }
  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(device, buffer.buffer, &requirements);
  const std::uint32_t memory_type =
      FindMemoryType(physical_device, requirements.memoryTypeBits, required,
          preferred, &buffer.coherent);
  if (memory_type == std::numeric_limits<std::uint32_t>::max()) {
    detail = "no suitable memory type for a GPU scene buffer is available";
    return false;
  }
  const bool addressable =
      (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;
  VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.pNext = addressable ? &flags : nullptr;
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = memory_type;
  if (!VulkanOk(vkAllocateMemory(device, &allocate, nullptr, &buffer.memory),
          "vkAllocateMemory(scene)", detail) ||
      !VulkanOk(vkBindBufferMemory(device, buffer.buffer, buffer.memory, 0),
          "vkBindBufferMemory(scene)", detail)) {
    return false;
  }
  buffer.size = size;
  if (addressable) {
    VkBufferDeviceAddressInfo address{
        VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    address.buffer = buffer.buffer;
    buffer.address = vkGetBufferDeviceAddress(device, &address);
  }
  if ((required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
    return VulkanOk(vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0,
                        &buffer.mapped),
        "vkMapMemory(scene)", detail);
  }
  return true;
}

void DestroyBuffer(VkDevice device, DeviceBuffer& buffer) {
  if (buffer.mapped != nullptr) {
    vkUnmapMemory(device, buffer.memory);
  }
  vkDestroyBuffer(device, buffer.buffer, nullptr);
  vkFreeMemory(device, buffer.memory, nullptr);
  buffer = DeviceBuffer{};
}

bool InvalidateBuffer(VkDevice device, const DeviceBuffer& buffer,
    std::string& detail) {
  if (buffer.coherent) {
    return true;
  }
  VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
  range.memory = buffer.memory;
  range.size = VK_WHOLE_SIZE;
  return VulkanOk(vkInvalidateMappedMemoryRanges(device, 1, &range),
      "vkInvalidateMappedMemoryRanges(scene)", detail);
}

bool HasDeviceExtension(VkPhysicalDevice device, std::string_view name) {
  std::uint32_t count = 0;
  vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> extensions(count);
  vkEnumerateDeviceExtensionProperties(device, nullptr, &count,
      extensions.data());
  return std::any_of(extensions.begin(), extensions.end(),
      [&](const VkExtensionProperties& extension) {
        return name == extension.extensionName;
      });
}

// The column-major 4x4 transform's first three rows, row-major, which is
// how VkTransformMatrixKHR stores it.
bool ValidRadiance(const std::array<float, 3>& values) {
  return std::all_of(values.begin(), values.end(),
      [](float value) { return std::isfinite(value) && value >= 0.0F; });
}

bool ValidTextureInput(const std::optional<TextureInput>& input,
    bool colour) {
  if (!input) {
    return true;
  }
  const auto finite = [](const std::array<float, 4>& values) {
    return std::all_of(values.begin(), values.end(),
        [](float value) { return std::isfinite(value); });
  };
  return input->channel <= (colour ? 0U : 3U) &&
         static_cast<std::uint32_t>(input->wrap_s) < 4 &&
         static_cast<std::uint32_t>(input->wrap_t) < 4 &&
         finite(input->scale) && finite(input->bias) &&
         finite(input->fallback);
}

bool ValidMaterial(const SceneMaterial& entry) {
  const Material& material = entry.material;
  const auto unit = [](float value) { return value >= 0.0F && value <= 1.0F; };
  return std::all_of(material.base_color.begin(), material.base_color.end(),
             unit) &&
         unit(material.roughness) && unit(material.metallic) &&
         ValidRadiance(material.emission) &&
         ValidTextureInput(material.base_color_texture, true) &&
         ValidTextureInput(material.roughness_texture, false) &&
         ValidTextureInput(material.metallic_texture, false) &&
         ValidTextureInput(material.emission_texture, true) &&
         std::all_of(material.normal.begin(), material.normal.end(),
             [](float value) { return value >= -1.0F && value <= 1.0F; }) &&
         ValidTextureInput(material.normal_texture, true) &&
         std::isfinite(material.texcoord_fallback[0]) &&
         std::isfinite(material.texcoord_fallback[1]);
}

VkTransformMatrixKHR TlasTransform(const Matrix4& world_from_object) {
  VkTransformMatrixKHR transform{};
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 4; ++column) {
      transform.matrix[row][column] = world_from_object[column * 4 + row];
    }
  }
  return transform;
}

} // namespace

AccelerationSupport ProbeAccelerationStructures(VkPhysicalDevice device) {
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(device, &properties);
  if (properties.apiVersion < VK_API_VERSION_1_2) {
    return {false, "the device's Vulkan API is older than 1.2, so it has no "
                   "buffer device addresses for acceleration structures"};
  }
  for (const char* extension : kAccelerationExtensions) {
    if (!HasDeviceExtension(device, extension)) {
      return {false, std::string("the device does not expose ") + extension};
    }
  }
  VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
  VkPhysicalDeviceVulkan12Features vulkan12{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  vulkan12.pNext = &acceleration;
  VkPhysicalDeviceFeatures2 features{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  features.pNext = &vulkan12;
  vkGetPhysicalDeviceFeatures2(device, &features);
  if (vulkan12.bufferDeviceAddress != VK_TRUE) {
    return {false, "the device does not support bufferDeviceAddress"};
  }
  if (acceleration.accelerationStructure != VK_TRUE) {
    return {false, "the device does not support the accelerationStructure "
                   "feature"};
  }
  return {true, "VK_KHR_acceleration_structure on " +
                    std::string(properties.deviceName)};
}

BackendCapability ProbeRayQueries(VkPhysicalDevice device,
    const AccelerationSupport& acceleration) {
  if (!acceleration.available)
    return {false, acceleration.detail};
  if (!HasDeviceExtension(device, VK_KHR_RAY_QUERY_EXTENSION_NAME))
    return {false, "the device does not expose VK_KHR_ray_query"};
  VkPhysicalDeviceRayQueryFeaturesKHR query{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
  VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  features.pNext = &query;
  vkGetPhysicalDeviceFeatures2(device, &features);
  return {query.rayQuery == VK_TRUE, query.rayQuery == VK_TRUE
                                         ? "VK_KHR_ray_query and rayQuery are supported"
                                         : "the device does not support the rayQuery feature"};
}

bool GpuScene::Initialize(VkPhysicalDevice physical_device, VkDevice device,
    VkQueue queue, std::uint32_t queue_family,
    const AccelerationSupport& acceleration, std::string& detail) {
  physical_device_ = physical_device;
  device_ = device;
  queue_ = queue;
  VkPhysicalDeviceAccelerationStructurePropertiesKHR acceleration_properties{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
  VkPhysicalDeviceProperties2 properties2{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  if (acceleration.available) {
    properties2.pNext = &acceleration_properties;
  }
  vkGetPhysicalDeviceProperties2(physical_device, &properties2);
  const VkPhysicalDeviceProperties& properties = properties2.properties;
  // Aligning the triangles lets a pass bind them as their own storage
  // buffer range.
  index_alignment_ = std::max<VkDeviceSize>(4,
      properties.limits.minStorageBufferOffsetAlignment);
  max_allocations_ = properties.limits.maxMemoryAllocationCount;
  max_texture_size_ = properties.limits.maxImageDimension2D;
  timestamp_period_ = properties.limits.timestampPeriod;
  std::uint32_t queue_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_count,
      nullptr);
  std::vector<VkQueueFamilyProperties> queues(queue_count);
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_count,
      queues.data());
  timestamp_bits_ = queues.at(queue_family).timestampValidBits;

  acceleration_ = acceleration.available;
  stats_.acceleration_available = acceleration.available;
  stats_.acceleration_detail = acceleration.detail;
  if (acceleration_) {
    create_acceleration_ = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
        vkGetDeviceProcAddr(device_, "vkCreateAccelerationStructureKHR"));
    destroy_acceleration_ =
        reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
            vkGetDeviceProcAddr(device_, "vkDestroyAccelerationStructureKHR"));
    build_sizes_ = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
        vkGetDeviceProcAddr(device_,
            "vkGetAccelerationStructureBuildSizesKHR"));
    build_acceleration_ =
        reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
            vkGetDeviceProcAddr(device_,
                "vkCmdBuildAccelerationStructuresKHR"));
    acceleration_address_ =
        reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
            vkGetDeviceProcAddr(device_,
                "vkGetAccelerationStructureDeviceAddressKHR"));
    if (create_acceleration_ == nullptr || destroy_acceleration_ == nullptr ||
        build_sizes_ == nullptr || build_acceleration_ == nullptr ||
        acceleration_address_ == nullptr) {
      detail = "the device enabled VK_KHR_acceleration_structure but does "
               "not provide its commands";
      return false;
    }
    scratch_alignment_ = std::max<VkDeviceSize>(1,
        acceleration_properties.minAccelerationStructureScratchOffsetAlignment);
    max_instances_ = acceleration_properties.maxInstanceCount;
    max_primitives_ = acceleration_properties.maxPrimitiveCount;
  }

  VkCommandPoolCreateInfo pool_create{
      VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_create.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_create.queueFamilyIndex = queue_family;
  if (!VulkanOk(vkCreateCommandPool(device_, &pool_create, nullptr,
                    &command_pool_),
          "vkCreateCommandPool(scene)", detail)) {
    return false;
  }
  VkCommandBufferAllocateInfo command_allocate{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  command_allocate.commandPool = command_pool_;
  command_allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_allocate.commandBufferCount = 1;
  if (!VulkanOk(vkAllocateCommandBuffers(device_, &command_allocate,
                    &command_),
          "vkAllocateCommandBuffers(scene)", detail)) {
    return false;
  }
  if (timestamp_bits_ != 0) {
    VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    query.queryType = VK_QUERY_TYPE_TIMESTAMP;
    query.queryCount = kTimestampCount;
    if (!VulkanOk(vkCreateQueryPool(device_, &query, nullptr, &timestamps_),
            "vkCreateQueryPool(scene)", detail)) {
      return false;
    }
  }
  stats_.timestamps_available = timestamps_ != VK_NULL_HANDLE;
  VkFenceCreateInfo fence_create{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  return VulkanOk(vkCreateFence(device_, &fence_create, nullptr, &fence_),
      "vkCreateFence(scene)", detail);
}

void GpuScene::Destroy() {
  if (device_ == VK_NULL_HANDLE) {
    return;
  }
  DestroyAccelerationStructure(tlas_);
  for (Geometry& geometry : slots_) {
    DestroyAccelerationStructure(geometry.blas);
    DestroyBuffer(device_, geometry.buffer);
  }
  slots_.clear();
  free_slots_.clear();
  slot_of_.clear();
  for (GpuTexture& texture : textures_) {
    DestroyTexture(texture);
  }
  textures_.clear();
  free_texture_slots_.clear();
  texture_slot_of_.clear();
  tlas_blas_ids_.clear();
  tlas_transforms_.clear();
  tlas_built_ = false;
  DestroyBuffer(device_, instances_);
  DestroyBuffer(device_, materials_);
  DestroyBuffer(device_, tlas_instances_);
  DestroyBuffer(device_, scratch_);
  DestroyBuffer(device_, staging_);
  vkDestroyQueryPool(device_, timestamps_, nullptr);
  vkDestroyFence(device_, fence_, nullptr);
  vkDestroyCommandPool(device_, command_pool_, nullptr);
  timestamps_ = VK_NULL_HANDLE;
  fence_ = VK_NULL_HANDLE;
  command_pool_ = VK_NULL_HANDLE;
  command_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
}

bool GpuScene::Validate(const SceneUpdate& update, std::string& detail) const {
  std::unordered_set<const MeshGeometry*> released;
  for (const MeshGeometry* geometry : update.geometry_releases) {
    if (!slot_of_.contains(geometry) || !released.insert(geometry).second) {
      detail = "the scene update releases geometry that is not resident";
      return false;
    }
  }
  const auto resident = [&](const MeshGeometry* geometry) {
    return slot_of_.contains(geometry) && !released.contains(geometry);
  };
  std::unordered_set<const MeshGeometry*> uploaded;
  for (const auto& geometry : update.geometry_uploads) {
    if (!geometry || geometry->triangles.empty()) {
      detail = "the scene update uploads geometry without triangles";
      return false;
    }
    if (resident(geometry.get()) || !uploaded.insert(geometry.get()).second) {
      detail = "the scene update uploads geometry that is already resident";
      return false;
    }
    if (geometry->positions.size() > std::numeric_limits<std::uint32_t>::max() ||
        geometry->triangles.size() > std::numeric_limits<std::uint32_t>::max()) {
      detail = "the scene update uploads geometry larger than uint32 counts";
      return false;
    }
    for (const auto& triangle : geometry->triangles) {
      for (const std::uint32_t index : triangle) {
        if (index >= geometry->positions.size()) {
          detail = "the scene update uploads a triangle index out of range";
          return false;
        }
      }
    }
    if (!geometry->normals.empty() &&
        geometry->normals.size() != 3 * geometry->triangles.size()) {
      detail = "the scene update uploads normals that are not one per "
               "triangle corner";
      return false;
    }
    if (!std::all_of(geometry->texcoords.begin(), geometry->texcoords.end(),
            [&](const auto& set) {
              return set.second.size() == 3 * geometry->triangles.size();
            })) {
      detail = "the scene update uploads texture coordinates that are not "
               "one per triangle corner";
      return false;
    }
    if (acceleration_ && geometry->triangles.size() > max_primitives_) {
      std::ostringstream message;
      message << "the scene update uploads geometry with "
              << geometry->triangles.size()
              << " triangles but a BLAS on this device holds at most "
              << max_primitives_;
      detail = message.str();
      return false;
    }
  }
  std::unordered_set<const Texture*> released_textures;
  for (const Texture* texture : update.texture_releases) {
    if (!texture_slot_of_.contains(texture) ||
        !released_textures.insert(texture).second) {
      detail = "the scene update releases a texture that is not resident";
      return false;
    }
  }
  std::unordered_set<const Texture*> uploaded_textures;
  for (const auto& texture : update.texture_uploads) {
    if (!texture || (texture_slot_of_.contains(texture.get()) &&
                        !released_textures.contains(texture.get())) ||
        !uploaded_textures.insert(texture.get()).second) {
      detail = "the scene update uploads a texture that is already resident";
      return false;
    }
    if (TextureVkFormat(texture->format) == VK_FORMAT_UNDEFINED ||
        texture->width == 0 || texture->height == 0 ||
        texture->texels.size() != TextureBytes(*texture)) {
      detail = "the scene update uploads a texture whose texels do not "
               "match its size and format";
      return false;
    }
    if (texture->width > max_texture_size_ ||
        texture->height > max_texture_size_) {
      std::ostringstream message;
      message << "the scene update uploads a " << texture->width << 'x'
              << texture->height << " texture but this device's images are "
              << "at most " << max_texture_size_ << " texels on a side";
      detail = message.str();
      return false;
    }
  }
  const std::uint64_t textures =
      static_cast<std::uint64_t>(texture_slot_of_.size()) -
      released_textures.size() + uploaded_textures.size();
  if (textures > kMaxTextures) {
    std::ostringstream message;
    message << "the scene update makes " << textures
            << " textures resident but the GPU scene holds at most "
            << kMaxTextures;
    detail = message.str();
    return false;
  }
  const auto texture_resident = [&](const Texture* texture) {
    return (texture_slot_of_.contains(texture) &&
               !released_textures.contains(texture)) ||
           uploaded_textures.contains(texture);
  };
  if (!update.instances_changed && !update.instances.empty()) {
    detail = "the scene update lists instances without replacing them";
    return false;
  }
  if (update.instances.size() > std::numeric_limits<std::uint32_t>::max()) {
    detail = "the scene update has more instances than uint32 counts";
    return false;
  }
  if (update.materials_changed == update.materials.empty()) {
    detail = update.materials_changed
                 ? "the scene update's material table has no default material"
                 : "the scene update lists materials without replacing them";
    return false;
  }
  if (update.materials.size() > std::numeric_limits<std::uint32_t>::max()) {
    detail = "the scene update has more materials than uint32 counts";
    return false;
  }
  if (!std::all_of(update.materials.begin(), update.materials.end(),
          ValidMaterial)) {
    detail = "the scene update has a material value out of range";
    return false;
  }
  // The table after this plan must sample only resident textures, whether
  // the plan replaces it or not.
  const std::vector<SceneMaterial>& table =
      update.materials_changed ? update.materials : material_table_;
  for (const SceneMaterial& entry : table) {
    const auto inputs = TextureInputs(entry.material);
    for (std::size_t index = 0; index < inputs.size(); ++index) {
      const Texture* texture = entry.textures[index];
      if (texture != nullptr &&
          (!inputs[index]->has_value() || !texture_resident(texture))) {
        detail = "a material samples a texture that is not resident";
        return false;
      }
    }
  }
  const std::size_t material_count = table.size();
  for (const SceneInstance& instance : update.instances) {
    if (!resident(instance.geometry) && !uploaded.contains(instance.geometry)) {
      detail = "a scene instance references geometry that is not resident";
      return false;
    }
    if (instance.material >= material_count) {
      detail = "a scene instance references a material outside the table";
      return false;
    }
    if (instance.texcoords != kNoTexcoords &&
        instance.texcoords >= instance.geometry->texcoords.size()) {
      detail = "a scene instance reads a texture-coordinate set its geometry "
               "does not have";
      return false;
    }
  }
  if (!update.instances_changed && material_count < materials_used_) {
    detail = "the scene update's material table drops materials that the "
             "instances use";
    return false;
  }
  if (update.environment_changed && !ValidRadiance(update.environment)) {
    detail = "the scene update's environment radiance is negative or not "
             "finite";
    return false;
  }
  if (acceleration_ && update.instances.size() > max_instances_) {
    std::ostringstream message;
    message << "the scene update has " << update.instances.size()
            << " instances but a TLAS on this device holds at most "
            << max_instances_;
    detail = message.str();
    return false;
  }
  // One allocation per geometry, one per BLAS and one per texture, plus the
  // instance, material and staging buffers and, with acceleration
  // structures, the TLAS, its build input and the scratch buffer.
  const std::uint64_t geometries = static_cast<std::uint64_t>(slot_of_.size()) -
                                   released.size() + uploaded.size();
  const std::uint64_t allocations = geometries * (acceleration_ ? 2 : 1) +
                                    textures + 3 +
                                    (acceleration_ ? 3 : 0) +
                                    kReservedAllocations;
  if (allocations > max_allocations_) {
    std::ostringstream message;
    message << "the GPU scene needs " << allocations
            << " device allocations but the device allows "
            << max_allocations_ << "; scenes this large need a suballocator";
    detail = message.str();
    return false;
  }
  return true;
}

void GpuScene::Release(const MeshGeometry* geometry) {
  const auto found = slot_of_.find(geometry);
  const std::uint32_t slot = found->second;
  stats_.geometry_bytes -= slots_[slot].buffer.size;
  stats_.acceleration_bytes -= slots_[slot].blas.buffer.size;
  // Render waits for its frame before returning, so no submission still
  // reads this buffer. A TLAS instance that referenced the BLAS is gone from
  // the plan's instances, so this plan rebuilds the TLAS before any use.
  DestroyAccelerationStructure(slots_[slot].blas);
  DestroyBuffer(device_, slots_[slot].buffer);
  slots_[slot] = Geometry{};
  free_slots_.insert(slot);
  slot_of_.erase(found);
  ++stats_.geometry_releases;
}

void GpuScene::Release(const Texture* texture) {
  const auto found = texture_slot_of_.find(texture);
  const std::uint32_t slot = found->second;
  stats_.texture_bytes -= textures_[slot].bytes;
  // As for geometry, no submission still samples it. A material that did is
  // rewritten by this plan, and the scene passes leave its descriptor
  // unread.
  DestroyTexture(textures_[slot]);
  free_texture_slots_.insert(slot);
  texture_slot_of_.erase(found);
  ++texture_generation_;
  ++stats_.texture_releases;
}

bool GpuScene::CreateTexture(const Texture& source, GpuTexture& texture,
    std::string& detail) {
  const VkFormat format = TextureVkFormat(source.format);
  VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  create.imageType = VK_IMAGE_TYPE_2D;
  create.format = format;
  create.extent = {source.width, source.height, 1};
  create.mipLevels = 1;
  create.arrayLayers = 1;
  create.samples = VK_SAMPLE_COUNT_1_BIT;
  create.tiling = VK_IMAGE_TILING_OPTIMAL;
  // Transfer source serves ReadBack.
  create.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!VulkanOk(vkCreateImage(device_, &create, nullptr, &texture.image),
          "vkCreateImage(texture)", detail)) {
    DestroyTexture(texture);
    return false;
  }
  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(device_, texture.image, &requirements);
  const std::uint32_t memory_type =
      FindMemoryType(physical_device_, requirements.memoryTypeBits,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
  if (memory_type == std::numeric_limits<std::uint32_t>::max()) {
    detail = "no suitable memory type for a texture is available";
    DestroyTexture(texture);
    return false;
  }
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = memory_type;
  if (!VulkanOk(vkAllocateMemory(device_, &allocate, nullptr, &texture.memory),
          "vkAllocateMemory(texture)", detail) ||
      !VulkanOk(vkBindImageMemory(device_, texture.image, texture.memory, 0),
          "vkBindImageMemory(texture)", detail)) {
    DestroyTexture(texture);
    return false;
  }
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = texture.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format;
  view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  view.subresourceRange.levelCount = 1;
  view.subresourceRange.layerCount = 1;
  if (!VulkanOk(vkCreateImageView(device_, &view, nullptr, &texture.view),
          "vkCreateImageView(texture)", detail)) {
    DestroyTexture(texture);
    return false;
  }
  texture.bytes = TextureBytes(source);
  return true;
}

void GpuScene::DestroyTexture(GpuTexture& texture) {
  vkDestroyImageView(device_, texture.view, nullptr);
  vkDestroyImage(device_, texture.image, nullptr);
  vkFreeMemory(device_, texture.memory, nullptr);
  texture = GpuTexture{};
}

std::vector<VkImageView> GpuScene::TextureViews() const {
  std::vector<VkImageView> views;
  views.reserve(textures_.size());
  for (const GpuTexture& texture : textures_) {
    views.push_back(texture.view);
  }
  return views;
}

bool GpuScene::CreateDeviceBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
    DeviceBuffer& buffer, std::string& detail) {
  if (!CreateBuffer(physical_device_, device_, size, usage,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, buffer, detail)) {
    DestroyBuffer(device_, buffer);
    return false;
  }
  return true;
}

bool GpuScene::CreateAccelerationStructure(VkAccelerationStructureTypeKHR type,
    VkDeviceSize size, AccelerationStructure& structure, std::string& detail) {
  if (!CreateDeviceBuffer(size,
          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
          structure.buffer, detail)) {
    return false;
  }
  VkAccelerationStructureCreateInfoKHR create{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
  create.buffer = structure.buffer.buffer;
  create.size = size;
  create.type = type;
  if (!VulkanOk(create_acceleration_(device_, &create, nullptr,
                    &structure.handle),
          "vkCreateAccelerationStructureKHR", detail)) {
    DestroyAccelerationStructure(structure);
    return false;
  }
  VkAccelerationStructureDeviceAddressInfoKHR address{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
  address.accelerationStructure = structure.handle;
  structure.address = acceleration_address_(device_, &address);
  return true;
}

void GpuScene::DestroyAccelerationStructure(AccelerationStructure& structure) {
  if (structure.handle != VK_NULL_HANDLE) {
    destroy_acceleration_(device_, structure.handle, nullptr);
  }
  DestroyBuffer(device_, structure.buffer);
  structure = AccelerationStructure{};
}

bool GpuScene::EnsureStaging(VkDeviceSize size, std::string& detail) {
  if (staging_.size >= size) {
    return true;
  }
  const VkDeviceSize capacity = std::max(size, staging_.size * 2);
  DestroyBuffer(device_, staging_);
  if (!CreateBuffer(physical_device_, device_, capacity,
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging_, detail)) {
    DestroyBuffer(device_, staging_);
    return false;
  }
  return true;
}

// Grow-only. The buffer carries one alignment of slack, so the aligned base
// address still has `size` bytes behind it.
bool GpuScene::EnsureScratch(VkDeviceSize size, std::string& detail) {
  const VkDeviceSize needed = size + scratch_alignment_;
  if (scratch_.size >= needed) {
    return true;
  }
  const VkDeviceSize capacity = std::max(needed, scratch_.size * 2);
  DestroyBuffer(device_, scratch_);
  return CreateDeviceBuffer(capacity,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      scratch_, detail);
}

bool GpuScene::Flush(const DeviceBuffer& buffer, std::string& detail) {
  if (buffer.coherent) {
    return true;
  }
  VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
  range.memory = buffer.memory;
  range.size = VK_WHOLE_SIZE;
  return VulkanOk(vkFlushMappedMemoryRanges(device_, 1, &range),
      "vkFlushMappedMemoryRanges(scene)", detail);
}

bool GpuScene::BeginCommands(std::string& detail) {
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  return VulkanOk(vkBeginCommandBuffer(command_, &begin),
      "vkBeginCommandBuffer(scene)", detail);
}

bool GpuScene::SubmitAndWait(std::string& detail) {
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command_;
  return VulkanOk(vkEndCommandBuffer(command_), "vkEndCommandBuffer(scene)",
             detail) &&
         VulkanOk(vkResetFences(device_, 1, &fence_), "vkResetFences(scene)",
             detail) &&
         VulkanOk(vkQueueSubmit(queue_, 1, &submit, fence_),
             "vkQueueSubmit(scene)", detail) &&
         VulkanOk(vkWaitForFences(device_, 1, &fence_, VK_TRUE,
                      10'000'000'000ULL),
             "vkWaitForFences(scene)", detail);
}

bool GpuScene::Apply(const SceneUpdate& update, std::string& detail) {
  timings_ = GpuSceneTimings{};
  if (!Validate(update, detail)) {
    return false;
  }
  stats_.source_revision = update.source_revision;
  if (update.environment_changed) {
    // Host state: the scene pass reads it with each frame's constants.
    environment_ = update.environment;
  }
  if (update.materials_changed) {
    material_table_ = update.materials;
    materials_current_ = false;
  }
  // Only instances read the table, so it waits for the first of them.
  const bool has_instances = update.instances_changed
                                 ? !update.instances.empty()
                                 : instance_count_ != 0;
  const bool write_materials = !materials_current_ && has_instances;
  if (update.geometry_releases.empty() && update.geometry_uploads.empty() &&
      update.texture_releases.empty() && update.texture_uploads.empty() &&
      !update.instances_changed && !write_materials) {
    return true;
  }
  for (const MeshGeometry* geometry : update.geometry_releases) {
    Release(geometry);
  }
  for (const Texture* texture : update.texture_releases) {
    Release(texture);
  }

  // Staging holds the texture uploads, the geometry uploads, the instance
  // records, the material table, then the TLAS build input.
  VkDeviceSize staging_bytes = 0;
  for (const auto& texture : update.texture_uploads) {
    staging_bytes = AlignUp(staging_bytes, kTexelAlignment) +
                    TextureBytes(*texture);
  }
  staging_bytes = AlignUp(staging_bytes, kTexelAlignment);
  for (const auto& geometry : update.geometry_uploads) {
    staging_bytes += LayOut(*geometry, index_alignment_).size;
  }
  const VkDeviceSize instance_bytes =
      update.instances.size() * kInstanceBytes;
  const VkDeviceSize material_bytes =
      write_materials ? material_table_.size() * kMaterialBytes : 0;
  // A rewrite that keeps every instance's BLAS and transform, such as a
  // material binding change, leaves the TLAS as it is.
  bool tlas_current =
      tlas_built_ && update.instances.size() == tlas_blas_ids_.size();
  for (std::size_t index = 0; tlas_current && index < update.instances.size();
      ++index) {
    const SceneInstance& instance = update.instances[index];
    const auto found = slot_of_.find(instance.geometry);
    tlas_current = found != slot_of_.end() &&
                   slots_[found->second].blas_id == tlas_blas_ids_[index] &&
                   instance.world_from_object == tlas_transforms_[index];
  }
  const bool build_tlas =
      acceleration_ && update.instances_changed && !tlas_current;
  const VkDeviceSize tlas_input_bytes =
      build_tlas ? update.instances.size() * kTlasInstanceBytes : 0;
  staging_bytes += instance_bytes + material_bytes + tlas_input_bytes;
  if (staging_bytes != 0 && !EnsureStaging(staging_bytes, detail)) {
    return false;
  }

  // Transfer source serves ReadBack.
  VkBufferUsageFlags geometry_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if (acceleration_) {
    geometry_usage |=
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
  }
  // Each new geometry gets a BLAS, built from its buffer. Every build has
  // its own scratch range, so one command records them all.
  struct BlasBuild {
    VkAccelerationStructureGeometryKHR geometry{};
    VkAccelerationStructureBuildGeometryInfoKHR info{};
    VkAccelerationStructureBuildRangeInfoKHR range{};
    VkDeviceSize scratch_offset = 0;
  };
  std::vector<BlasBuild> blas_builds;
  blas_builds.reserve(update.geometry_uploads.size());
  VkDeviceSize blas_scratch = 0;

  auto* staging = static_cast<std::uint8_t*>(staging_.mapped);
  VkDeviceSize offset = 0;
  std::vector<std::pair<VkImage, VkBufferImageCopy>> texture_copies;
  for (const auto& source : update.texture_uploads) {
    GpuTexture texture;
    if (!CreateTexture(*source, texture, detail)) {
      return false;
    }
    texture.source = source;
    offset = AlignUp(offset, kTexelAlignment);
    std::memcpy(staging + offset, source->texels.data(), texture.bytes);
    VkBufferImageCopy region{};
    region.bufferOffset = offset;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {source->width, source->height, 1};
    texture_copies.push_back({texture.image, region});
    offset += texture.bytes;
    stats_.texture_bytes += texture.bytes;
    std::uint32_t slot = 0;
    if (free_texture_slots_.empty()) {
      slot = static_cast<std::uint32_t>(textures_.size());
      textures_.push_back(std::move(texture));
    } else {
      slot = *free_texture_slots_.begin();
      free_texture_slots_.erase(free_texture_slots_.begin());
      textures_[slot] = std::move(texture);
    }
    texture_slot_of_[source.get()] = slot;
    ++texture_generation_;
    ++stats_.texture_uploads;
  }
  offset = AlignUp(offset, kTexelAlignment);
  std::vector<std::pair<VkBuffer, VkBufferCopy>> copies;
  for (const auto& source : update.geometry_uploads) {
    Geometry geometry;
    geometry.source = source;
    geometry.vertex_count = static_cast<std::uint32_t>(source->positions.size());
    geometry.triangle_count =
        static_cast<std::uint32_t>(source->triangles.size());
    const VkDeviceSize position_bytes = geometry.vertex_count * kVec3Bytes;
    const VkDeviceSize triangle_bytes = geometry.triangle_count * kVec3Bytes;
    const GeometryLayout layout = LayOut(*source, index_alignment_);
    geometry.index_offset = layout.index_offset;
    geometry.normal_offset = layout.normal_offset;
    geometry.texcoord_offsets = layout.texcoord_offsets;
    const VkDeviceSize size = layout.size;
    if (!CreateDeviceBuffer(size, geometry_usage, geometry.buffer, detail)) {
      return false;
    }
    // Zeroed padding keeps the device contents deterministic.
    std::memset(staging + offset, 0, size);
    std::memcpy(staging + offset, source->positions.data(), position_bytes);
    std::memcpy(staging + offset + geometry.index_offset,
        source->triangles.data(), triangle_bytes);
    if (geometry.normal_offset != 0) {
      std::memcpy(staging + offset + geometry.normal_offset,
          source->normals.data(), source->normals.size() * kVec3Bytes);
    }
    std::size_t set = 0;
    for (const auto& [name, texcoords] : source->texcoords) {
      (void)name;
      std::memcpy(staging + offset + geometry.texcoord_offsets[set++],
          texcoords.data(), texcoords.size() * kVec2Bytes);
    }
    copies.push_back({geometry.buffer.buffer, VkBufferCopy{offset, 0, size}});
    offset += size;

    if (acceleration_) {
      BlasBuild& build = blas_builds.emplace_back();
      build.geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
      build.geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
      build.geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
      VkAccelerationStructureGeometryTrianglesDataKHR& triangles =
          build.geometry.geometry.triangles;
      triangles.sType =
          VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
      triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
      triangles.vertexData.deviceAddress = geometry.buffer.address;
      triangles.vertexStride = kVec3Bytes;
      triangles.maxVertex = geometry.vertex_count - 1;
      triangles.indexType = VK_INDEX_TYPE_UINT32;
      triangles.indexData.deviceAddress =
          geometry.buffer.address + geometry.index_offset;
      build.info.sType =
          VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
      build.info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
      build.info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
      build.info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
      build.info.geometryCount = 1;
      // Stable: `blas_builds` never reallocates.
      build.info.pGeometries = &build.geometry;
      build.range.primitiveCount = geometry.triangle_count;
      VkAccelerationStructureBuildSizesInfoKHR sizes{
          VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
      build_sizes_(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
          &build.info, &geometry.triangle_count, &sizes);
      if (!CreateAccelerationStructure(
              VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
              sizes.accelerationStructureSize, geometry.blas, detail)) {
        DestroyBuffer(device_, geometry.buffer);
        return false;
      }
      build.info.dstAccelerationStructure = geometry.blas.handle;
      build.scratch_offset = blas_scratch;
      blas_scratch += AlignUp(sizes.buildScratchSize, scratch_alignment_);
      geometry.blas_id = next_blas_id_++;
      stats_.acceleration_bytes += geometry.blas.buffer.size;
      ++stats_.blas_builds;
    }

    std::uint32_t slot = 0;
    if (free_slots_.empty()) {
      slot = static_cast<std::uint32_t>(slots_.size());
      slots_.push_back(std::move(geometry));
    } else {
      slot = *free_slots_.begin();
      free_slots_.erase(free_slots_.begin());
      slots_[slot] = std::move(geometry);
    }
    slot_of_[source.get()] = slot;
    stats_.geometry_bytes += size;
    ++stats_.geometry_uploads;
  }

  VkAccelerationStructureGeometryKHR tlas_geometry{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  VkAccelerationStructureBuildGeometryInfoKHR tlas_info{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  VkAccelerationStructureBuildRangeInfoKHR tlas_range{};
  VkDeviceSize tlas_scratch = 0;
  if (update.instances_changed) {
    if (instance_bytes > instances_.size) {
      const VkDeviceSize capacity =
          std::max(instance_bytes, instances_.size * 2);
      DestroyBuffer(device_, instances_);
      // The scene pass reads the records through their device address.
      VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
      if (acceleration_) {
        usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
      }
      if (!CreateDeviceBuffer(capacity, usage, instances_, detail)) {
        return false;
      }
    }
    for (std::size_t index = 0; index < update.instances.size(); ++index) {
      const SceneInstance& instance = update.instances[index];
      GpuInstanceRecord record{};
      std::copy(instance.world_from_object.begin(),
          instance.world_from_object.end(), record.world_from_object);
      record.geometry_slot = slot_of_.at(instance.geometry);
      const Geometry& geometry = slots_[record.geometry_slot];
      if (geometry.buffer.address != 0) {
        record.positions = geometry.buffer.address;
        record.triangles = geometry.buffer.address + geometry.index_offset;
        if (geometry.normal_offset != 0) {
          record.normals = geometry.buffer.address + geometry.normal_offset;
        }
        if (instance.texcoords != kNoTexcoords) {
          record.texcoords = geometry.buffer.address +
                             geometry.texcoord_offsets[instance.texcoords];
        }
      }
      record.material_slot = instance.material;
      record.texcoord_set = instance.texcoords;
      std::memcpy(staging + offset + index * kInstanceBytes, &record,
          kInstanceBytes);
    }
    if (instance_bytes != 0) {
      copies.push_back(
          {instances_.buffer, VkBufferCopy{offset, 0, instance_bytes}});
    }
    offset += instance_bytes;
    instance_count_ = static_cast<std::uint32_t>(update.instances.size());
    materials_used_ = 0;
    for (const SceneInstance& instance : update.instances) {
      materials_used_ = std::max(materials_used_, instance.material + 1);
    }
    ++stats_.instance_writes;
  }

  if (write_materials) {
    if (material_bytes > materials_.size) {
      const VkDeviceSize capacity =
          std::max(material_bytes, materials_.size * 2);
      DestroyBuffer(device_, materials_);
      // The scene pass reads the table through its device address.
      VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
      if (acceleration_) {
        usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
      }
      if (!CreateDeviceBuffer(capacity, usage, materials_, detail)) {
        return false;
      }
    }
    for (std::size_t index = 0; index < material_table_.size(); ++index) {
      const SceneMaterial& entry = material_table_[index];
      const Material& material = entry.material;
      GpuMaterialRecord record{};
      std::copy(material.base_color.begin(), material.base_color.end(),
          record.base_color_roughness);
      record.base_color_roughness[3] = material.roughness;
      std::copy(material.emission.begin(), material.emission.end(),
          record.emission_metallic);
      record.emission_metallic[3] = material.metallic;
      record.texcoord_fallback[0] = material.texcoord_fallback[0];
      record.texcoord_fallback[1] = material.texcoord_fallback[1];
      std::copy(material.normal.begin(), material.normal.end(), record.normal);
      const auto inputs = TextureInputs(material);
      for (std::size_t input = 0; input < inputs.size(); ++input) {
        GpuTextureInputRecord& lookup = record.inputs[input];
        if (!inputs[input]->has_value()) {
          continue;
        }
        const TextureInput& source = **inputs[input];
        lookup.channel = source.channel;
        lookup.sampler = static_cast<std::uint32_t>(source.wrap_s) * 4 +
                         static_cast<std::uint32_t>(source.wrap_t);
        std::copy(source.scale.begin(), source.scale.end(), lookup.scale);
        std::copy(source.bias.begin(), source.bias.end(), lookup.bias);
        std::copy(source.fallback.begin(), source.fallback.end(),
            lookup.fallback);
        if (entry.textures[input] == nullptr) {
          lookup.mode = kFallbackLookup;
        } else {
          lookup.mode = kTextureLookup;
          lookup.texture = texture_slot_of_.at(entry.textures[input]);
        }
      }
      std::memcpy(staging + offset + index * kMaterialBytes, &record,
          kMaterialBytes);
    }
    copies.push_back(
        {materials_.buffer, VkBufferCopy{offset, 0, material_bytes}});
    offset += material_bytes;
    material_count_ = static_cast<std::uint32_t>(material_table_.size());
    materials_current_ = true;
    ++stats_.material_writes;
  }

  if (build_tlas) {
    // At least one record, so an empty TLAS still has a valid input address.
    const VkDeviceSize input_bytes =
        std::max(tlas_input_bytes, kTlasInstanceBytes);
    if (input_bytes > tlas_instances_.size) {
      const VkDeviceSize capacity =
          std::max(input_bytes, tlas_instances_.size * 2);
      DestroyBuffer(device_, tlas_instances_);
      if (!CreateDeviceBuffer(capacity,
              VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                  VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                  VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
              tlas_instances_, detail)) {
        return false;
      }
    }
    std::vector<std::uint64_t> blas_ids;
    blas_ids.reserve(update.instances.size());
    for (std::size_t index = 0; index < update.instances.size(); ++index) {
      const SceneInstance& instance = update.instances[index];
      const Geometry& geometry = slots_[slot_of_.at(instance.geometry)];
      // The instance index (InstanceId in a ray query) is the instance
      // record's index; the custom index is unused.
      VkAccelerationStructureInstanceKHR record{};
      record.transform = TlasTransform(instance.world_from_object);
      record.instanceCustomIndex = 0;
      record.mask = 0xFF;
      record.instanceShaderBindingTableRecordOffset = 0;
      record.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
      record.accelerationStructureReference = geometry.blas.address;
      std::memcpy(staging + offset + index * kTlasInstanceBytes, &record,
          kTlasInstanceBytes);
      blas_ids.push_back(geometry.blas_id);
    }
    if (tlas_input_bytes != 0) {
      copies.push_back(
          {tlas_instances_.buffer, VkBufferCopy{offset, 0, tlas_input_bytes}});
    }
    offset += tlas_input_bytes;

    tlas_geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlas_geometry.geometry.instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tlas_geometry.geometry.instances.arrayOfPointers = VK_FALSE;
    tlas_geometry.geometry.instances.data.deviceAddress =
        tlas_instances_.address;
    tlas_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlas_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                      VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    tlas_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlas_info.geometryCount = 1;
    tlas_info.pGeometries = &tlas_geometry;
    const auto count = static_cast<std::uint32_t>(update.instances.size());
    VkAccelerationStructureBuildSizesInfoKHR sizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    build_sizes_(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &tlas_info, &count, &sizes);
    if (tlas_.handle == VK_NULL_HANDLE ||
        tlas_.buffer.size < sizes.accelerationStructureSize) {
      stats_.acceleration_bytes -= tlas_.buffer.size;
      DestroyAccelerationStructure(tlas_);
      tlas_built_ = false;
      if (!CreateAccelerationStructure(
              VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
              sizes.accelerationStructureSize, tlas_, detail)) {
        return false;
      }
      stats_.acceleration_bytes += tlas_.buffer.size;
    }
    // Rewrites that keep every instance on the same BLAS change only
    // transforms, so they refit the TLAS in place (design policy section
    // 4.2: update and rebuild are distinct). Anything else rebuilds it.
    const bool refit = tlas_built_ && count != 0 && blas_ids == tlas_blas_ids_;
    if (refit) {
      tlas_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
      tlas_info.srcAccelerationStructure = tlas_.handle;
      tlas_scratch = sizes.updateScratchSize;
      ++stats_.tlas_updates;
    } else {
      tlas_scratch = sizes.buildScratchSize;
      ++stats_.tlas_builds;
    }
    tlas_info.dstAccelerationStructure = tlas_.handle;
    tlas_range.primitiveCount = count;
    tlas_blas_ids_ = std::move(blas_ids);
    tlas_transforms_.clear();
    for (const SceneInstance& instance : update.instances) {
      tlas_transforms_.push_back(instance.world_from_object);
    }
    tlas_built_ = true;
  }

  // The TLAS build runs after the BLAS builds, so it reuses their scratch.
  const bool builds = !blas_builds.empty() || build_tlas;
  if (builds) {
    if (!EnsureScratch(std::max(blas_scratch, tlas_scratch), detail)) {
      return false;
    }
    const VkDeviceAddress scratch = AlignUp(scratch_.address, scratch_alignment_);
    for (BlasBuild& build : blas_builds) {
      build.info.scratchData.deviceAddress = scratch + build.scratch_offset;
    }
    tlas_info.scratchData.deviceAddress = scratch;
  }

  const bool copying = !copies.empty() || !texture_copies.empty();
  if (copying || builds) {
    if (!Flush(staging_, detail) || !BeginCommands(detail)) {
      return false;
    }
    // New images go to the transfer layout for their copies, then to the
    // layout the scene passes sample.
    const auto transition = [&](VkImageLayout from, VkImageLayout to,
                                VkAccessFlags source_access,
                                VkAccessFlags destination_access,
                                VkPipelineStageFlags source_stage,
                                VkPipelineStageFlags destination_stage) {
      if (texture_copies.empty()) {
        return;
      }
      std::vector<VkImageMemoryBarrier> barriers;
      for (const auto& [image, region] : texture_copies) {
        (void)region;
        VkImageMemoryBarrier& barrier = barriers.emplace_back();
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = source_access;
        barrier.dstAccessMask = destination_access;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
      }
      vkCmdPipelineBarrier(command_, source_stage, destination_stage, 0, 0,
          nullptr, 0, nullptr, static_cast<std::uint32_t>(barriers.size()),
          barriers.data());
    };
    // Each phase's closing timestamp waits for the phase's commands; the
    // barriers between phases keep the next phase from starting earlier.
    const bool timed = timestamps_ != VK_NULL_HANDLE;
    const auto stamp = [&](VkPipelineStageFlagBits stage, std::uint32_t query) {
      if (timed) {
        vkCmdWriteTimestamp(command_, stage, timestamps_, query);
      }
    };
    if (timed) {
      vkCmdResetQueryPool(command_, timestamps_, 0, kTimestampCount);
    }
    stamp(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);
    transition(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);
    for (const auto& [image, region] : texture_copies) {
      vkCmdCopyBufferToImage(command_, staging_.buffer, image,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    for (const auto& [buffer, region] : copies) {
      vkCmdCopyBuffer(command_, staging_.buffer, buffer, 1, &region);
    }
    // Later submissions sample the textures or copy them back.
    transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    // Acceleration-structure builds and later submissions read the scene in
    // shaders, copy it back or overwrite it.
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                            VK_ACCESS_TRANSFER_READ_BIT |
                            VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0,
        nullptr);
    stamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 1);
    if (!blas_builds.empty()) {
      std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
      std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> ranges;
      for (const BlasBuild& build : blas_builds) {
        infos.push_back(build.info);
        ranges.push_back(&build.range);
      }
      build_acceleration_(command_, static_cast<std::uint32_t>(infos.size()),
          infos.data(), ranges.data());
      // The TLAS build reads the new BLASes and reuses their scratch.
      VkMemoryBarrier built{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      built.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
      built.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                            VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
      vkCmdPipelineBarrier(command_,
          VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
          VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &built,
          0, nullptr, 0, nullptr);
    }
    stamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 2);
    if (build_tlas) {
      const VkAccelerationStructureBuildRangeInfoKHR* range = &tlas_range;
      build_acceleration_(command_, 1, &tlas_info, &range);
    }
    stamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 3);
    if (builds) {
      // Later submissions trace the structures, update the TLAS and reuse
      // the scratch buffer.
      VkMemoryBarrier built{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      built.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
      built.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                            VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
      vkCmdPipelineBarrier(command_,
          VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &built, 0, nullptr, 0,
          nullptr);
    }
    if (!SubmitAndWait(detail)) {
      return false;
    }
    ++stats_.upload_submissions;
    stats_.uploaded_bytes += offset;
    if (timed) {
      std::array<std::uint64_t, kTimestampCount> ticks{};
      if (!VulkanOk(vkGetQueryPoolResults(device_, timestamps_, 0,
                        kTimestampCount, sizeof(ticks), ticks.data(),
                        sizeof(std::uint64_t),
                        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
              "vkGetQueryPoolResults(scene)", detail)) {
        return false;
      }
      const std::uint64_t mask = timestamp_bits_ == 64
                                     ? ~std::uint64_t{0}
                                     : (std::uint64_t{1} << timestamp_bits_) - 1;
      const auto milliseconds = [&](std::size_t first) {
        return static_cast<double>((ticks[first + 1] - ticks[first]) & mask) *
               timestamp_period_ / 1'000'000.0;
      };
      timings_.available = true;
      // A phase with no commands measures only the gap between two
      // timestamps, so it reports 0.
      timings_.upload_gpu_ms = copying ? milliseconds(0) : 0.0;
      timings_.blas_build_gpu_ms = blas_builds.empty() ? 0.0 : milliseconds(1);
      timings_.tlas_build_gpu_ms = build_tlas ? milliseconds(2) : 0.0;
    }
  }
  stats_.resident_geometries = static_cast<std::uint32_t>(slot_of_.size());
  stats_.instance_count = instance_count_;
  stats_.instance_bytes = instance_count_ * kInstanceBytes;
  stats_.material_count = material_count_;
  stats_.material_bytes = material_count_ * kMaterialBytes;
  stats_.resident_textures =
      static_cast<std::uint32_t>(texture_slot_of_.size());
  if (acceleration_) {
    stats_.blas_count = stats_.resident_geometries;
    stats_.tlas_instance_count =
        static_cast<std::uint32_t>(tlas_blas_ids_.size());
  }
  return true;
}

bool GpuScene::ReadBack(GpuSceneContents& contents, std::string& detail) {
  contents = GpuSceneContents{};
  contents.environment = environment_;
  const VkDeviceSize instance_bytes = instance_count_ * kInstanceBytes;
  const VkDeviceSize material_bytes = material_count_ * kMaterialBytes;
  const VkDeviceSize tlas_input_bytes =
      tlas_built_ ? tlas_blas_ids_.size() * kTlasInstanceBytes : 0;
  VkDeviceSize total = stats_.geometry_bytes + instance_bytes +
                       material_bytes + tlas_input_bytes;
  // The textures follow, each at an aligned offset.
  std::vector<VkDeviceSize> texture_offsets(textures_.size());
  for (std::size_t slot = 0; slot < textures_.size(); ++slot) {
    if (textures_[slot].source) {
      texture_offsets[slot] = AlignUp(total, kTexelAlignment);
      total = texture_offsets[slot] + textures_[slot].bytes;
    }
  }
  if (total == 0) {
    contents.status = FrameStatus::Pass;
    return true;
  }
  DeviceBuffer readback;
  const auto fail = [&] {
    DestroyBuffer(device_, readback);
    return false;
  };
  if (!CreateBuffer(physical_device_, device_, total,
          VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, readback, detail) ||
      !BeginCommands(detail)) {
    return fail();
  }
  VkDeviceSize offset = 0;
  for (const Geometry& geometry : slots_) {
    if (geometry.source) {
      const VkBufferCopy region{0, offset, geometry.buffer.size};
      vkCmdCopyBuffer(command_, geometry.buffer.buffer, readback.buffer, 1,
          &region);
      offset += geometry.buffer.size;
    }
  }
  if (instance_bytes != 0) {
    const VkBufferCopy region{0, offset, instance_bytes};
    vkCmdCopyBuffer(command_, instances_.buffer, readback.buffer, 1, &region);
    offset += instance_bytes;
  }
  if (material_bytes != 0) {
    const VkBufferCopy region{0, offset, material_bytes};
    vkCmdCopyBuffer(command_, materials_.buffer, readback.buffer, 1, &region);
    offset += material_bytes;
  }
  if (tlas_input_bytes != 0) {
    const VkBufferCopy region{0, offset, tlas_input_bytes};
    vkCmdCopyBuffer(command_, tlas_instances_.buffer, readback.buffer, 1,
        &region);
  }
  // Each texture leaves the sampled layout for its copy and returns to it.
  std::vector<VkImageMemoryBarrier> to_source;
  for (const GpuTexture& texture : textures_) {
    if (!texture.source) {
      continue;
    }
    VkImageMemoryBarrier& image = to_source.emplace_back();
    image.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    image.srcAccessMask = 0;
    image.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    image.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    image.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image.image = texture.image;
    image.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    image.subresourceRange.levelCount = 1;
    image.subresourceRange.layerCount = 1;
  }
  if (!to_source.empty()) {
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
        static_cast<std::uint32_t>(to_source.size()), to_source.data());
    for (std::size_t slot = 0; slot < textures_.size(); ++slot) {
      const GpuTexture& texture = textures_[slot];
      if (!texture.source) {
        continue;
      }
      VkBufferImageCopy region{};
      region.bufferOffset = texture_offsets[slot];
      region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      region.imageSubresource.layerCount = 1;
      region.imageExtent = {texture.source->width, texture.source->height, 1};
      vkCmdCopyImageToBuffer(command_, texture.image,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
    }
    std::vector<VkImageMemoryBarrier> to_sampled = to_source;
    for (VkImageMemoryBarrier& image : to_sampled) {
      image.srcAccessMask = 0;
      image.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      image.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      image.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
        static_cast<std::uint32_t>(to_sampled.size()), to_sampled.data());
  }
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
  if (!SubmitAndWait(detail) || !InvalidateBuffer(device_, readback, detail)) {
    return fail();
  }

  const auto* bytes = static_cast<const std::uint8_t*>(readback.mapped);
  offset = 0;
  std::unordered_map<VkDeviceAddress, std::uint32_t> slot_of_blas;
  for (std::size_t slot = 0; slot < slots_.size(); ++slot) {
    const Geometry& geometry = slots_[slot];
    if (!geometry.source) {
      continue;
    }
    if (geometry.blas.address != 0) {
      slot_of_blas[geometry.blas.address] = static_cast<std::uint32_t>(slot);
    }
    GpuGeometryContents copy;
    copy.slot = static_cast<std::uint32_t>(slot);
    copy.positions.resize(geometry.vertex_count);
    copy.triangles.resize(geometry.triangle_count);
    std::memcpy(copy.positions.data(), bytes + offset,
        geometry.vertex_count * kVec3Bytes);
    std::memcpy(copy.triangles.data(), bytes + offset + geometry.index_offset,
        geometry.triangle_count * kVec3Bytes);
    if (geometry.normal_offset != 0) {
      copy.normals.resize(3 * std::size_t{geometry.triangle_count});
      std::memcpy(copy.normals.data(), bytes + offset + geometry.normal_offset,
          copy.normals.size() * kVec3Bytes);
    }
    for (const VkDeviceSize texcoord_offset : geometry.texcoord_offsets) {
      auto& set = copy.texcoords.emplace_back(
          3 * std::size_t{geometry.triangle_count});
      std::memcpy(set.data(), bytes + offset + texcoord_offset,
          set.size() * kVec2Bytes);
    }
    contents.geometries.push_back(std::move(copy));
    offset += geometry.buffer.size;
  }
  for (std::uint32_t index = 0; index < instance_count_; ++index) {
    GpuInstanceRecord record{};
    std::memcpy(&record, bytes + offset + index * kInstanceBytes,
        kInstanceBytes);
    GpuInstanceContents instance;
    std::copy(std::begin(record.world_from_object),
        std::end(record.world_from_object), instance.world_from_object.begin());
    instance.geometry_slot = record.geometry_slot;
    instance.material_slot = record.material_slot;
    instance.texcoords = record.texcoord_set;
    contents.instances.push_back(instance);
  }
  offset += instance_bytes;
  for (std::uint32_t index = 0; index < material_count_; ++index) {
    GpuMaterialRecord record{};
    std::memcpy(&record, bytes + offset + index * kMaterialBytes,
        kMaterialBytes);
    GpuMaterialContents& entry = contents.materials.emplace_back();
    Material& material = entry.material;
    std::copy(record.base_color_roughness, record.base_color_roughness + 3,
        material.base_color.begin());
    material.roughness = record.base_color_roughness[3];
    std::copy(record.emission_metallic, record.emission_metallic + 3,
        material.emission.begin());
    material.metallic = record.emission_metallic[3];
    material.texcoord_fallback = {record.texcoord_fallback[0],
        record.texcoord_fallback[1]};
    std::copy(record.normal, record.normal + 3, material.normal.begin());
    std::array<std::optional<TextureInput>*, kMaterialTextureInputs> inputs{
        &material.base_color_texture, &material.roughness_texture,
        &material.metallic_texture, &material.emission_texture,
        &material.normal_texture};
    for (std::size_t input = 0; input < inputs.size(); ++input) {
      const GpuTextureInputRecord& lookup = record.inputs[input];
      if (lookup.mode == kConstantInput) {
        continue;
      }
      TextureInput& decoded = inputs[input]->emplace();
      decoded.channel = lookup.channel;
      decoded.wrap_s = static_cast<TextureWrap>(lookup.sampler / 4);
      decoded.wrap_t = static_cast<TextureWrap>(lookup.sampler % 4);
      std::copy(lookup.scale, lookup.scale + 4, decoded.scale.begin());
      std::copy(lookup.bias, lookup.bias + 4, decoded.bias.begin());
      std::copy(lookup.fallback, lookup.fallback + 4, decoded.fallback.begin());
      if (lookup.mode == kTextureLookup) {
        entry.texture_slots[input] = lookup.texture;
      }
    }
  }
  offset += material_bytes;
  for (VkDeviceSize index = 0; index * kTlasInstanceBytes < tlas_input_bytes;
      ++index) {
    VkAccelerationStructureInstanceKHR record{};
    std::memcpy(&record, bytes + offset + index * kTlasInstanceBytes,
        kTlasInstanceBytes);
    GpuTlasInstanceContents instance;
    for (int row = 0; row < 3; ++row) {
      for (int column = 0; column < 4; ++column) {
        instance.object_to_world[row * 4 + column] =
            record.transform.matrix[row][column];
      }
    }
    const auto found = slot_of_blas.find(record.accelerationStructureReference);
    instance.geometry_slot =
        found == slot_of_blas.end() ? kNoSlot : found->second;
    instance.mask = record.mask;
    contents.tlas_instances.push_back(instance);
  }
  for (std::size_t slot = 0; slot < textures_.size(); ++slot) {
    const GpuTexture& texture = textures_[slot];
    if (!texture.source) {
      continue;
    }
    GpuTextureContents& copy = contents.textures.emplace_back();
    copy.slot = static_cast<std::uint32_t>(slot);
    copy.texture.width = texture.source->width;
    copy.texture.height = texture.source->height;
    copy.texture.format = texture.source->format;
    copy.texture.texels.assign(bytes + texture_offsets[slot],
        bytes + texture_offsets[slot] + texture.bytes);
  }
  DestroyBuffer(device_, readback);
  contents.status = FrameStatus::Pass;
  return true;
}

} // namespace Lotus::vulkan_internal
