// SPDX-License-Identifier: Apache-2.0
// The GPU scene (design policy section 4.2): persistent device buffers and
// acceleration structures that SceneExtraction plans change incrementally.
// Private to backend/vulkan.
#pragma once

#include <array>
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

// One instance buffer element. The scene pass reads it through the buffer's
// device address; its mirror is `InstanceRecord` in shaders/path_trace.slang.
struct GpuInstanceRecord {
  // Column-major, multiplying column vectors (Lotus::Matrix4).
  float world_from_object[16];
  // Device addresses of the geometry's positions and triangles, or zero
  // without acceleration structures.
  std::uint64_t positions;
  std::uint64_t triangles;
  std::uint32_t geometry_slot;
  // The instance's entry in the material table.
  std::uint32_t material_slot;
  std::uint32_t reserved[2];
};
static_assert(sizeof(GpuInstanceRecord) == 96);

// One material table element: the material IR as the scene pass evaluates
// it. Its mirror is `MaterialRecord` in shaders/path_trace.slang.
struct GpuMaterialRecord {
  float base_color_roughness[4];
  float emission_metallic[4];
};
static_assert(sizeof(GpuMaterialRecord) == 32);

// Whether a physical device can build acceleration structures, decided
// before the device is created. When `available`, the device must be
// created with `kAccelerationExtensions`, the accelerationStructure feature
// and the bufferDeviceAddress feature.
struct AccelerationSupport {
  bool available = false;
  std::string detail;
};

inline constexpr std::array<const char*, 2> kAccelerationExtensions = {
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};

[[nodiscard]] AccelerationSupport ProbeAccelerationStructures(
    VkPhysicalDevice device);
[[nodiscard]] BackendCapability ProbeRayQueries(VkPhysicalDevice device,
    const AccelerationSupport& acceleration);

struct DeviceBuffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
  bool coherent = false;
  void* mapped = nullptr;
  // Set when the buffer was created with shader device address usage.
  VkDeviceAddress address = 0;
};

class GpuScene {
public:
  GpuScene() = default;
  GpuScene(const GpuScene&) = delete;
  GpuScene& operator=(const GpuScene&) = delete;

  bool Initialize(VkPhysicalDevice physical_device, VkDevice device,
      VkQueue queue, std::uint32_t queue_family,
      const AccelerationSupport& acceleration, std::string& detail);
  // Requires an idle device. Safe to call more than once.
  void Destroy();

  // Validates the whole plan before changing anything, then applies it and
  // waits for its uploads and builds. After a failure the scene's contents
  // are undefined.
  bool Apply(const SceneUpdate& update, std::string& detail);
  bool ReadBack(GpuSceneContents& contents, std::string& detail);
  [[nodiscard]] const GpuSceneStats& Stats() const {
    return stats_;
  }
  // The last Apply's submission; unavailable when it submitted nothing.
  [[nodiscard]] const GpuSceneTimings& Timings() const {
    return timings_;
  }
  [[nodiscard]] VkAccelerationStructureKHR Tlas() const {
    return tlas_built_ ? tlas_.handle : VK_NULL_HANDLE;
  }
  // The instance records' device address, valid until the next update; zero
  // without acceleration structures or before the first instance.
  [[nodiscard]] VkDeviceAddress InstanceAddress() const {
    return instances_.address;
  }
  // The material table's device address, under the same conditions.
  [[nodiscard]] VkDeviceAddress MaterialAddress() const {
    return materials_.address;
  }
  [[nodiscard]] const std::array<float, 3>& Environment() const {
    return environment_;
  }

private:
  struct AccelerationStructure {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    DeviceBuffer buffer;
    VkDeviceAddress address = 0;
  };

  // A geometry buffer holds the positions (three floats each) followed, at
  // `index_offset`, by the triangles (three uint32 each).
  struct Geometry {
    // Keeps the address that identifies this buffer unique while resident.
    std::shared_ptr<const MeshGeometry> source;
    DeviceBuffer buffer;
    VkDeviceSize index_offset = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t triangle_count = 0;
    AccelerationStructure blas;
    // Unique for the scene's lifetime, unlike handles and addresses, which
    // a later BLAS may reuse.
    std::uint64_t blas_id = 0;
  };

  bool Validate(const SceneUpdate& update, std::string& detail) const;
  void Release(const MeshGeometry* geometry);
  bool CreateDeviceBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
      DeviceBuffer& buffer, std::string& detail);
  bool CreateAccelerationStructure(VkAccelerationStructureTypeKHR type,
      VkDeviceSize size, AccelerationStructure& structure,
      std::string& detail);
  void DestroyAccelerationStructure(AccelerationStructure& structure);
  bool EnsureStaging(VkDeviceSize size, std::string& detail);
  bool EnsureScratch(VkDeviceSize size, std::string& detail);
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
  // The plans' material table. It reaches the device with the first
  // instances and with each change after that; until then the device holds
  // no entries.
  std::vector<Material> material_table_{Material{}};
  bool materials_current_ = false;
  DeviceBuffer materials_;
  std::uint32_t material_count_ = 0;
  // One more than the highest material slot an instance uses.
  std::uint32_t materials_used_ = 0;
  std::array<float, 3> environment_{};
  DeviceBuffer staging_;

  // Acceleration structures, when the device supports them.
  bool acceleration_ = false;
  PFN_vkCreateAccelerationStructureKHR create_acceleration_ = nullptr;
  PFN_vkDestroyAccelerationStructureKHR destroy_acceleration_ = nullptr;
  PFN_vkGetAccelerationStructureBuildSizesKHR build_sizes_ = nullptr;
  PFN_vkCmdBuildAccelerationStructuresKHR build_acceleration_ = nullptr;
  PFN_vkGetAccelerationStructureDeviceAddressKHR acceleration_address_ =
      nullptr;
  VkDeviceSize scratch_alignment_ = 1;
  std::uint64_t max_instances_ = 0;
  std::uint64_t max_primitives_ = 0;
  std::uint64_t next_blas_id_ = 1;
  AccelerationStructure tlas_;
  // The TLAS build input: one VkAccelerationStructureInstanceKHR per
  // instance record.
  DeviceBuffer tlas_instances_;
  // The BLAS of each TLAS instance when the TLAS was last built; an
  // identical list lets the next rewrite update the TLAS instead.
  std::vector<std::uint64_t> tlas_blas_ids_;
  // Their transforms, so a rewrite that keeps both skips the TLAS.
  std::vector<Matrix4> tlas_transforms_;
  bool tlas_built_ = false;
  DeviceBuffer scratch_;

  // Timestamps before the copies and after the copies, the BLAS builds and
  // the TLAS build; absent when the queue has no timestamp support.
  VkQueryPool timestamps_ = VK_NULL_HANDLE;
  std::uint32_t timestamp_bits_ = 0;
  float timestamp_period_ = 0.0F;

  GpuSceneStats stats_;
  GpuSceneTimings timings_;
};

} // namespace Lotus::vulkan_internal
