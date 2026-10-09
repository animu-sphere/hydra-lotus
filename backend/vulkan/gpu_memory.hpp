// SPDX-License-Identifier: Apache-2.0
// Scene-owned memory. Buffers and optimal images use separate pools, so
// bufferImageGranularity never separates two resources in the same block.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>
#include <lotus/vulkan_backend.hpp>

namespace Lotus::vulkan_internal {

struct MemoryRange {
  VkDeviceSize offset = 0;
  VkDeviceSize size = 0;
};

// Deterministic first-fit with coalescing; no live resources move.
class MemoryRanges {
public:
  explicit MemoryRanges(VkDeviceSize size) : free_{{0, size}} {
  }
  std::optional<MemoryRange> Allocate(VkDeviceSize size, VkDeviceSize alignment);
  void Release(MemoryRange range);
  [[nodiscard]] VkDeviceSize FreeBytes() const;
  [[nodiscard]] VkDeviceSize LargestFreeRange() const;

private:
  std::vector<MemoryRange> free_;
};

struct MemorySlice {
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize offset = 0;
  // Atom-aligned reserved range, including padding for noncoherent hosts.
  VkDeviceSize size = 0;
  std::uint64_t block_id = 0;
  bool coherent = false;
  void* mapped = nullptr;
};

class SceneMemory {
public:
  void Initialize(VkPhysicalDevice physical_device, VkDevice device);
  // All bound resources must have been destroyed; device must be idle.
  void Destroy();
  bool Allocate(const VkMemoryRequirements& requirements,
      VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
      bool addressable, VkBuffer dedicated_buffer, VkImage dedicated_image,
      bool optimal_image, MemorySlice& slice, std::string& detail);
  // Bound buffer/image must be destroyed and all uses complete.
  void Release(MemorySlice& slice);
  [[nodiscard]] GpuMemoryStats Stats() const;

private:
  struct Block {
    explicit Block(VkDeviceSize capacity) : size(capacity), ranges(capacity) {
    }
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size;
    MemoryRanges ranges;
    std::uint64_t id = 0;
    std::uint32_t memory_type = 0;
    std::uint32_t live = 0;
    bool addressable = false;
    bool optimal_image = false;
    bool dedicated = false;
    bool coherent = false;
    void* mapped = nullptr;
  };
  void DestroyBlock(Block& block);
  static bool SamePool(const Block& a, const Block& b);
  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memory_properties_{};
  VkDeviceSize atom_size_ = 1;
  std::uint32_t max_blocks_ = 0;
  std::uint64_t next_id_ = 1;
  std::vector<std::unique_ptr<Block>> blocks_;
  GpuMemoryStats lifetime_;
};

} // namespace Lotus::vulkan_internal
