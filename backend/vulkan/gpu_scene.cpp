// SPDX-License-Identifier: Apache-2.0
#include "gpu_scene.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <utility>

#include "vulkan_internal.hpp"

namespace Lotus::vulkan_internal {

namespace {

constexpr VkDeviceSize kVec3Bytes = 3 * sizeof(float);
static_assert(sizeof(std::array<float, 3>) == kVec3Bytes &&
              sizeof(std::array<std::uint32_t, 3>) == kVec3Bytes);
constexpr VkDeviceSize kInstanceBytes = sizeof(GpuInstanceRecord);

// Device allocations left for the renderer's targets and readback buffers.
constexpr std::uint32_t kReservedAllocations = 16;

VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment) {
  return (value + alignment - 1) / alignment * alignment;
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
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = memory_type;
  if (!VulkanOk(vkAllocateMemory(device, &allocate, nullptr, &buffer.memory),
          "vkAllocateMemory(scene)", detail) ||
      !VulkanOk(vkBindBufferMemory(device, buffer.buffer, buffer.memory, 0),
          "vkBindBufferMemory(scene)", detail)) {
    return false;
  }
  buffer.size = size;
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

} // namespace

bool GpuScene::Initialize(VkPhysicalDevice physical_device, VkDevice device,
    VkQueue queue, std::uint32_t queue_family, std::string& detail) {
  physical_device_ = physical_device;
  device_ = device;
  queue_ = queue;
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(physical_device, &properties);
  // Aligning the triangles lets a pass bind them as their own storage
  // buffer range.
  index_alignment_ = std::max<VkDeviceSize>(4,
      properties.limits.minStorageBufferOffsetAlignment);
  max_allocations_ = properties.limits.maxMemoryAllocationCount;

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
  VkFenceCreateInfo fence_create{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  return VulkanOk(vkCreateFence(device_, &fence_create, nullptr, &fence_),
      "vkCreateFence(scene)", detail);
}

void GpuScene::Destroy() {
  if (device_ == VK_NULL_HANDLE) {
    return;
  }
  for (Geometry& geometry : slots_) {
    DestroyBuffer(device_, geometry.buffer);
  }
  slots_.clear();
  free_slots_.clear();
  slot_of_.clear();
  DestroyBuffer(device_, instances_);
  DestroyBuffer(device_, staging_);
  vkDestroyFence(device_, fence_, nullptr);
  vkDestroyCommandPool(device_, command_pool_, nullptr);
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
  }
  if (!update.instances_changed && !update.instances.empty()) {
    detail = "the scene update lists instances without replacing them";
    return false;
  }
  if (update.instances.size() > std::numeric_limits<std::uint32_t>::max()) {
    detail = "the scene update has more instances than uint32 counts";
    return false;
  }
  for (const SceneInstance& instance : update.instances) {
    if (!resident(instance.geometry) && !uploaded.contains(instance.geometry)) {
      detail = "a scene instance references geometry that is not resident";
      return false;
    }
  }
  // One allocation per geometry, plus the instance and staging buffers.
  const std::uint64_t allocations = static_cast<std::uint64_t>(slot_of_.size()) -
                                    released.size() + uploaded.size() + 2 +
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
  // Render waits for its frame before returning, so no submission still
  // reads this buffer.
  DestroyBuffer(device_, slots_[slot].buffer);
  slots_[slot] = Geometry{};
  free_slots_.insert(slot);
  slot_of_.erase(found);
  ++stats_.geometry_releases;
}

bool GpuScene::CreateDeviceBuffer(VkDeviceSize size, DeviceBuffer& buffer,
    std::string& detail) {
  // Transfer source serves ReadBack; later passes add their own usage.
  if (!CreateBuffer(physical_device_, device_, size,
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
              VK_BUFFER_USAGE_TRANSFER_DST_BIT |
              VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, buffer, detail)) {
    DestroyBuffer(device_, buffer);
    return false;
  }
  return true;
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
  if (!Validate(update, detail)) {
    return false;
  }
  stats_.source_revision = update.source_revision;
  if (update.Empty()) {
    return true;
  }
  for (const MeshGeometry* geometry : update.geometry_releases) {
    Release(geometry);
  }

  // Staging holds the geometry uploads, then the instance records.
  VkDeviceSize staging_bytes = 0;
  for (const auto& geometry : update.geometry_uploads) {
    staging_bytes +=
        AlignUp(geometry->positions.size() * kVec3Bytes, index_alignment_) +
        geometry->triangles.size() * kVec3Bytes;
  }
  const VkDeviceSize instance_bytes =
      update.instances.size() * kInstanceBytes;
  staging_bytes += instance_bytes;
  if (staging_bytes != 0 && !EnsureStaging(staging_bytes, detail)) {
    return false;
  }

  auto* staging = static_cast<std::uint8_t*>(staging_.mapped);
  VkDeviceSize offset = 0;
  std::vector<std::pair<VkBuffer, VkBufferCopy>> copies;
  for (const auto& source : update.geometry_uploads) {
    Geometry geometry;
    geometry.source = source;
    geometry.vertex_count = static_cast<std::uint32_t>(source->positions.size());
    geometry.triangle_count =
        static_cast<std::uint32_t>(source->triangles.size());
    const VkDeviceSize position_bytes = geometry.vertex_count * kVec3Bytes;
    geometry.index_offset = AlignUp(position_bytes, index_alignment_);
    const VkDeviceSize size =
        geometry.index_offset + geometry.triangle_count * kVec3Bytes;
    if (!CreateDeviceBuffer(size, geometry.buffer, detail)) {
      return false;
    }
    std::memcpy(staging + offset, source->positions.data(), position_bytes);
    // Zeroed padding keeps the device contents deterministic.
    std::memset(staging + offset + position_bytes, 0,
        geometry.index_offset - position_bytes);
    std::memcpy(staging + offset + geometry.index_offset,
        source->triangles.data(), geometry.triangle_count * kVec3Bytes);
    copies.push_back({geometry.buffer.buffer, VkBufferCopy{offset, 0, size}});
    offset += size;

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

  if (update.instances_changed) {
    if (instance_bytes > instances_.size) {
      const VkDeviceSize capacity =
          std::max(instance_bytes, instances_.size * 2);
      DestroyBuffer(device_, instances_);
      if (!CreateDeviceBuffer(capacity, instances_, detail)) {
        return false;
      }
    }
    for (std::size_t index = 0; index < update.instances.size(); ++index) {
      const SceneInstance& instance = update.instances[index];
      GpuInstanceRecord record{};
      std::copy(instance.world_from_object.begin(),
          instance.world_from_object.end(), record.world_from_object);
      record.geometry_slot = slot_of_.at(instance.geometry);
      std::memcpy(staging + offset + index * kInstanceBytes, &record,
          kInstanceBytes);
    }
    if (instance_bytes != 0) {
      copies.push_back(
          {instances_.buffer, VkBufferCopy{offset, 0, instance_bytes}});
    }
    offset += instance_bytes;
    instance_count_ = static_cast<std::uint32_t>(update.instances.size());
    ++stats_.instance_writes;
  }

  if (!copies.empty()) {
    if (!Flush(staging_, detail) || !BeginCommands(detail)) {
      return false;
    }
    for (const auto& [buffer, region] : copies) {
      vkCmdCopyBuffer(command_, staging_.buffer, buffer, 1, &region);
    }
    // Later submissions read the scene in shaders, copy it back or
    // overwrite it.
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                            VK_ACCESS_TRANSFER_READ_BIT |
                            VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0,
        nullptr);
    if (!SubmitAndWait(detail)) {
      return false;
    }
    ++stats_.upload_submissions;
    stats_.uploaded_bytes += offset;
  }
  stats_.resident_geometries = static_cast<std::uint32_t>(slot_of_.size());
  stats_.instance_count = instance_count_;
  stats_.instance_bytes = instance_count_ * kInstanceBytes;
  return true;
}

bool GpuScene::ReadBack(GpuSceneContents& contents, std::string& detail) {
  contents = GpuSceneContents{};
  const VkDeviceSize instance_bytes = instance_count_ * kInstanceBytes;
  const VkDeviceSize total = stats_.geometry_bytes + instance_bytes;
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
  for (std::size_t slot = 0; slot < slots_.size(); ++slot) {
    const Geometry& geometry = slots_[slot];
    if (!geometry.source) {
      continue;
    }
    GpuGeometryContents copy;
    copy.slot = static_cast<std::uint32_t>(slot);
    copy.positions.resize(geometry.vertex_count);
    copy.triangles.resize(geometry.triangle_count);
    std::memcpy(copy.positions.data(), bytes + offset,
        geometry.vertex_count * kVec3Bytes);
    std::memcpy(copy.triangles.data(), bytes + offset + geometry.index_offset,
        geometry.triangle_count * kVec3Bytes);
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
    contents.instances.push_back(instance);
  }
  DestroyBuffer(device_, readback);
  contents.status = FrameStatus::Pass;
  return true;
}

} // namespace Lotus::vulkan_internal
