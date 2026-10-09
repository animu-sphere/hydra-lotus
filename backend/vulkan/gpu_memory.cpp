// SPDX-License-Identifier: Apache-2.0
#include "gpu_memory.hpp"

#include <algorithm>
#include <limits>

#include "vulkan_internal.hpp"

namespace Lotus::vulkan_internal {
namespace {
constexpr VkDeviceSize kDeviceBlock = 16 * 1024 * 1024;
constexpr VkDeviceSize kHostBlock = 4 * 1024 * 1024;
// Offscreen/presentation resources remain outside the scene allocator.
constexpr std::uint32_t kReservedAllocations = 16;

std::optional<VkDeviceSize> Align(VkDeviceSize size, VkDeviceSize alignment) {
  if (alignment == 0)
    return std::nullopt;
  const VkDeviceSize padding = (alignment - size % alignment) % alignment;
  if (size > std::numeric_limits<VkDeviceSize>::max() - padding) {
    return std::nullopt;
  }
  return size + padding;
}
} // namespace

std::optional<MemoryRange> MemoryRanges::Allocate(
    VkDeviceSize size, VkDeviceSize alignment) {
  if (size == 0)
    return std::nullopt;
  for (std::size_t i = 0; i < free_.size(); ++i) {
    const MemoryRange free = free_[i];
    const auto offset = Align(free.offset, alignment);
    if (!offset || *offset - free.offset > free.size ||
        size > free.size - (*offset - free.offset)) {
      continue;
    }
    const VkDeviceSize prefix = *offset - free.offset;
    const VkDeviceSize suffix = free.size - prefix - size;
    free_.erase(free_.begin() + i);
    if (suffix != 0)
      free_.insert(free_.begin() + i, {*offset + size, suffix});
    if (prefix != 0)
      free_.insert(free_.begin() + i, {free.offset, prefix});
    return MemoryRange{*offset, size};
  }
  return std::nullopt;
}

void MemoryRanges::Release(MemoryRange range) {
  const auto pos = std::lower_bound(free_.begin(), free_.end(), range.offset,
      [](MemoryRange free, VkDeviceSize offset) { return free.offset < offset; });
  auto current = free_.insert(pos, range);
  if (current != free_.begin()) {
    auto previous = current - 1;
    if (previous->offset + previous->size == current->offset) {
      previous->size += current->size;
      current = free_.erase(current) - 1;
    }
  }
  const auto next = current + 1;
  if (next != free_.end() && current->offset + current->size == next->offset) {
    current->size += next->size;
    free_.erase(next);
  }
}

VkDeviceSize MemoryRanges::FreeBytes() const {
  VkDeviceSize bytes = 0;
  for (const auto range : free_)
    bytes += range.size;
  return bytes;
}

VkDeviceSize MemoryRanges::LargestFreeRange() const {
  VkDeviceSize bytes = 0;
  for (const auto range : free_)
    bytes = std::max(bytes, range.size);
  return bytes;
}

void SceneMemory::Initialize(VkPhysicalDevice physical_device, VkDevice device) {
  physical_device_ = physical_device;
  device_ = device;
  vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties_);
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(physical_device, &properties);
  atom_size_ = properties.limits.nonCoherentAtomSize;
  max_blocks_ = properties.limits.maxMemoryAllocationCount > kReservedAllocations
                    ? properties.limits.maxMemoryAllocationCount - kReservedAllocations
                    : 0;
}

bool SceneMemory::SamePool(const Block& a, const Block& b) {
  return a.memory_type == b.memory_type && a.addressable == b.addressable &&
         a.optimal_image == b.optimal_image && !a.dedicated && !b.dedicated;
}

bool SceneMemory::Allocate(const VkMemoryRequirements& requirements,
    VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
    bool addressable, VkBuffer dedicated_buffer, VkImage dedicated_image,
    bool optimal_image, MemorySlice& slice, std::string& detail) {
  const std::uint32_t memory_type = FindMemoryType(physical_device_,
      requirements.memoryTypeBits, required, preferred);
  if (memory_type == std::numeric_limits<std::uint32_t>::max()) {
    detail = "no suitable memory type for a GPU scene resource is available";
    return false;
  }
  const auto flags = memory_properties_.memoryTypes[memory_type].propertyFlags;
  const bool host = (required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
  const bool coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
  const bool dedicated = dedicated_buffer != VK_NULL_HANDLE ||
                         dedicated_image != VK_NULL_HANDLE;
  const VkDeviceSize alignment = host && !coherent
                                     ? std::max(requirements.alignment, atom_size_)
                                     : requirements.alignment;
  const auto padded = Align(requirements.size, host && !coherent ? atom_size_ : 1);
  if (!padded || *padded == 0) {
    detail = "GPU scene memory requirement size overflow or zero size";
    return false;
  }
  // Dedicated allocations must have exactly requirements.size bytes.
  const VkDeviceSize size = dedicated ? requirements.size : *padded;
  const auto use = [&](Block& block, MemoryRange range) {
    ++block.live;
    slice = {block.memory, range.offset, range.size, block.id, block.coherent,
        block.mapped ? static_cast<char*>(block.mapped) + range.offset : nullptr};
    ++lifetime_.suballocations;
  };
  if (!dedicated) {
    for (auto& block : blocks_) {
      if (block->memory_type == memory_type && block->addressable == addressable &&
          block->optimal_image == optimal_image && !block->dedicated &&
          // Host-visible device-local memory can serve both uses, but only
          // host pools are mapped; keep their lifetimes separate.
          (block->mapped != nullptr) == host) {
        if (auto range = block->ranges.Allocate(size, alignment)) {
          use(*block, *range);
          ++lifetime_.reused_suballocations;
          return true;
        }
      }
    }
  }
  // Retire cached empty blocks before enforcing the actual allocation bound.
  // Live ranges are never relocated.
  if (blocks_.size() >= max_blocks_) {
    for (auto it = blocks_.begin(); it != blocks_.end();) {
      if ((*it)->live == 0) {
        DestroyBlock(**it);
        it = blocks_.erase(it);
      } else
        ++it;
    }
  }
  if (blocks_.size() >= max_blocks_) {
    detail = "GPU scene memory pool reached the device allocation limit "
             "(16 allocations reserved for renderer targets)";
    return false;
  }
  const VkDeviceSize capacity = dedicated ? size : std::max(size, host ? kHostBlock : kDeviceBlock);
  auto block = std::make_unique<Block>(capacity);
  block->id = next_id_++;
  block->memory_type = memory_type;
  block->addressable = addressable;
  block->optimal_image = optimal_image;
  block->dedicated = dedicated;
  block->coherent = coherent;
  VkMemoryDedicatedAllocateInfo specific{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  specific.buffer = dedicated_buffer;
  specific.image = dedicated_image;
  VkMemoryAllocateFlagsInfo address{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  address.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  address.pNext = dedicated ? &specific : nullptr;
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.allocationSize = capacity;
  allocate.memoryTypeIndex = memory_type;
  allocate.pNext = addressable ? static_cast<const void*>(&address)
                   : dedicated ? &specific
                               : nullptr;
  if (!VulkanOk(vkAllocateMemory(device_, &allocate, nullptr, &block->memory),
          "vkAllocateMemory(scene pool)", detail))
    return false;
  ++lifetime_.device_allocations;
  if (host && !VulkanOk(vkMapMemory(device_, block->memory, 0, VK_WHOLE_SIZE,
                            0, &block->mapped),
                  "vkMapMemory(scene pool)", detail)) {
    DestroyBlock(*block);
    return false;
  }
  use(*block, *block->ranges.Allocate(size, alignment));
  blocks_.push_back(std::move(block));
  lifetime_.peak_blocks = std::max(lifetime_.peak_blocks,
      static_cast<std::uint32_t>(blocks_.size()));
  return true;
}

void SceneMemory::DestroyBlock(Block& block) {
  if (block.mapped)
    vkUnmapMemory(device_, block.memory);
  vkFreeMemory(device_, block.memory, nullptr);
  ++lifetime_.device_frees;
}

void SceneMemory::Release(MemorySlice& slice) {
  if (slice.memory == VK_NULL_HANDLE)
    return;
  for (auto it = blocks_.begin(); it != blocks_.end(); ++it) {
    Block& block = **it;
    if (block.id != slice.block_id)
      continue;
    block.ranges.Release({slice.offset, slice.size});
    --block.live;
    if (block.live == 0 && (block.dedicated ||
                               std::any_of(blocks_.begin(), blocks_.end(), [&](const auto& other) {
                                 return other.get() != &block && other->live == 0 &&
                                        SamePool(block, *other) && (block.mapped != nullptr) == (other->mapped != nullptr);
                               }))) {
      DestroyBlock(block);
      blocks_.erase(it);
    }
    break;
  }
  slice = {};
}

GpuMemoryStats SceneMemory::Stats() const {
  GpuMemoryStats stats = lifetime_;
  stats.blocks = static_cast<std::uint32_t>(blocks_.size());
  for (const auto& block : blocks_) {
    stats.reserved_bytes += block->size;
    const auto free = block->ranges.FreeBytes();
    stats.free_bytes += free;
    stats.used_bytes += block->size - free;
    stats.largest_free_range = std::max(stats.largest_free_range,
        block->ranges.LargestFreeRange());
    stats.live_suballocations += block->live;
    if (block->dedicated)
      ++stats.dedicated_blocks;
  }
  return stats;
}

void SceneMemory::Destroy() {
  for (auto& block : blocks_)
    DestroyBlock(*block);
  blocks_.clear();
  device_ = VK_NULL_HANDLE;
}
} // namespace Lotus::vulkan_internal
