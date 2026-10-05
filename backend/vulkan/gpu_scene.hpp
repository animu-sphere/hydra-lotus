// SPDX-License-Identifier: Apache-2.0
// The GPU scene (design policy section 4.2): persistent device buffers that
// SceneExtraction plans change incrementally. Private to backend/vulkan.
#pragma once

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include <lotus/extraction.hpp>
#include <lotus/vulkan_backend.hpp>

namespace Lotus::vulkan_internal {

// One instance buffer element, laid out for std430 storage buffers. The
// shader-side mirror arrives with the first pass that reads it.
struct GpuInstanceRecord {
  // Column-major, multiplying column vectors (Lotus::Matrix4).
  float world_from_object[16];
  std::uint32_t geometry_slot;
  std::uint32_t reserved[3];
};
static_assert(sizeof(GpuInstanceRecord) == 80);

struct DeviceBuffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
  bool coherent = false;
  void* mapped = nullptr;
};

class GpuScene {
public:
  GpuScene() = default;
  GpuScene(const GpuScene&) = delete;
  GpuScene& operator=(const GpuScene&) = delete;

  bool Initialize(VkPhysicalDevice physical_device, VkDevice device,
      VkQueue queue, std::uint32_t queue_family, std::string& detail);
  // Requires an idle device. Safe to call more than once.
  void Destroy();

  // Validates the whole plan before changing anything, then applies it and
  // waits for its uploads. After a failure the scene's contents are
  // undefined.
  bool Apply(const SceneUpdate& update, std::string& detail);
  bool ReadBack(GpuSceneContents& contents, std::string& detail);
  [[nodiscard]] const GpuSceneStats& Stats() const { return stats_; }

private:
  // A geometry buffer holds the positions (three floats each) followed, at
  // `index_offset`, by the triangles (three uint32 each).
  struct Geometry {
    // Keeps the address that identifies this buffer unique while resident.
    std::shared_ptr<const MeshGeometry> source;
    DeviceBuffer buffer;
    VkDeviceSize index_offset = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t triangle_count = 0;
  };

  bool Validate(const SceneUpdate& update, std::string& detail) const;
  void Release(const MeshGeometry* geometry);
  bool CreateDeviceBuffer(VkDeviceSize size, DeviceBuffer& buffer,
      std::string& detail);
  bool EnsureStaging(VkDeviceSize size, std::string& detail);
  bool Flush(const DeviceBuffer& buffer, std::string& detail);
  bool BeginCommands(std::string& detail);
  bool SubmitAndWait(std::string& detail);

  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkDeviceSize index_alignment_ = 4;
  std::uint32_t max_allocations_ = 0;
  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer command_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;

  // Indexed by slot; a released slot is reused, lowest first, so slot
  // assignment is deterministic for a deterministic sequence of plans.
  std::vector<Geometry> slots_;
  std::set<std::uint32_t> free_slots_;
  std::unordered_map<const MeshGeometry*, std::uint32_t> slot_of_;
  DeviceBuffer instances_;
  std::uint32_t instance_count_ = 0;
  DeviceBuffer staging_;
  GpuSceneStats stats_;
};

} // namespace Lotus::vulkan_internal
