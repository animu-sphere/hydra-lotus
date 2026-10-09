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
#include "gpu_memory.hpp"

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
  // Device address of the geometry's corner normals, three floats per
  // triangle corner; zero when it has none or without acceleration
  // structures.
  std::uint64_t normals;
  // Device address of the texture-coordinate set the material's lookups
  // read, two floats per triangle corner; zero when there is none or
  // without acceleration structures.
  std::uint64_t texcoords;
  // That set's index in the geometry's sets, or kNoTexcoords.
  std::uint32_t texcoord_set;
  std::uint32_t reserved;
};
static_assert(sizeof(GpuInstanceRecord) == 112);

// GpuTextureInputRecord::mode.
inline constexpr std::uint32_t kConstantInput = 0;
inline constexpr std::uint32_t kTextureLookup = 1;
inline constexpr std::uint32_t kFallbackLookup = 2;

// One material input's texture lookup. Its mirror is `TextureInputRecord`
// in shaders/path_trace.slang.
struct GpuTextureInputRecord {
  // The texture table slot a kTextureLookup samples.
  std::uint32_t texture;
  // The sampler of its wrap modes: wrap_s * 4 + wrap_t.
  std::uint32_t sampler;
  std::uint32_t channel;
  // kConstantInput, kTextureLookup or kFallbackLookup (the key names no
  // texture).
  std::uint32_t mode;
  float scale[4];
  float bias[4];
  float fallback[4];
};
static_assert(sizeof(GpuTextureInputRecord) == 64);

// One material table element: the material IR as the scene pass evaluates
// it. Its mirror is `MaterialRecord` in shaders/path_trace.slang. The inputs
// are in TextureInputs order.
struct GpuMaterialRecord {
  float base_color_roughness[4];
  float emission_metallic[4];
  // The texture coordinates without a set, then two unused floats.
  float texcoord_fallback[4];
  float normal[4];
  // Opacity, opacity threshold, then two unused floats.
  float opacity[4];
  // Specular F0 RGB and ior; workflow flag uses normal.w.
  float specular_ior[4];
  GpuTextureInputRecord inputs[kMaterialTextureInputs];
};
static_assert(sizeof(GpuMaterialRecord) == 544);

// The texture table: the size of the scene passes' sampled-image array, and
// so the most textures a GPU scene holds.
inline constexpr std::uint32_t kMaxTextures = 1024;
// One sampler per pair of TextureWrap modes.
inline constexpr std::uint32_t kTextureSamplers = 16;

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
  MemorySlice allocation;
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
  [[nodiscard]] GpuSceneStats Stats() const {
    auto stats = stats_;
    stats.memory = memory_.Stats();
    return stats;
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
  // The texture table: one view per slot, VK_NULL_HANDLE for a free slot.
  // Views are in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL. The generation
  // changes whenever a view is created or destroyed.
  [[nodiscard]] std::vector<VkImageView> TextureViews() const;
  [[nodiscard]] std::uint64_t TextureGeneration() const {
    return texture_generation_;
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
  // `index_offset`, by the triangles (three uint32 each), at
  // `normal_offset`, by the corner normals (three floats each), if any, and
  // at each of `texcoord_offsets`, by a texture-coordinate set (two floats
  // per corner).
  struct Geometry {
    // Keeps the address that identifies this buffer unique while resident.
    std::shared_ptr<const MeshGeometry> source;
    DeviceBuffer buffer;
    VkDeviceSize index_offset = 0;
    // Zero without normals: the positions start the buffer.
    VkDeviceSize normal_offset = 0;
    std::vector<VkDeviceSize> texcoord_offsets;
    std::uint32_t vertex_count = 0;
    std::uint32_t triangle_count = 0;
    AccelerationStructure blas;
    // Unique for the scene's lifetime, unlike handles and addresses, which
    // a later BLAS may reuse.
    std::uint64_t blas_id = 0;
  };

  // A texture's image, in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL once
  // uploaded, and its view.
  struct GpuTexture {
    // Keeps the address that identifies this texture unique while resident.
    std::shared_ptr<const Texture> source;
    VkImage image = VK_NULL_HANDLE;
    MemorySlice allocation;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceSize bytes = 0;
  };

  bool Validate(const SceneUpdate& update, std::string& detail) const;
  void Release(const MeshGeometry* geometry);
  void Release(const Texture* texture);
  bool CreateTexture(const Texture& source, GpuTexture& texture,
      std::string& detail);
  void DestroyTexture(GpuTexture& texture);
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
  SceneMemory memory_;
  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer command_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;

  // Indexed by slot; a released slot is reused, lowest first, so slot
  // assignment is deterministic for a deterministic sequence of plans.
  std::vector<Geometry> slots_;
  std::set<std::uint32_t> free_slots_;
  std::unordered_map<const MeshGeometry*, std::uint32_t> slot_of_;
  // The texture table, slots assigned as the geometry's are.
  std::vector<GpuTexture> textures_;
  std::set<std::uint32_t> free_texture_slots_;
  std::unordered_map<const Texture*, std::uint32_t> texture_slot_of_;
  std::uint64_t texture_generation_ = 0;
  VkDeviceSize max_texture_size_ = 0;
  DeviceBuffer instances_;
  std::uint32_t instance_count_ = 0;
  // The plans' material table. It reaches the device with the first
  // instances and with each change after that; until then the device holds
  // no entries.
  std::vector<SceneMaterial> material_table_{SceneMaterial{}};
  bool materials_current_ = false;
  DeviceBuffer materials_;
  std::uint32_t material_count_ = 0;
  // One more than the highest material slot an instance uses.
  std::uint32_t materials_used_ = 0;
  std::array<float, 3> environment_{};
  DeviceBuffer staging_;
  DeviceBuffer readback_;

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
