// SPDX-License-Identifier: Apache-2.0
// The GPU scene (design policy section 4.2): persistent device buffers and
// acceleration structures that SceneExtraction plans change incrementally.
// Private to backend/vulkan.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
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

// One light table element: an emissive triangle, chosen by the alias
// method. Its mirror is `LightRecord` in shaders/common/scene.slang. The
// table follows one uint32 per instance record, padded to 16 bytes: the
// instance's first entry, or kNoLights. An instance's entries are its
// triangles in order.
struct GpuLightRecord {
  float pdf;
  float threshold;
  std::uint32_t alias;
  std::uint32_t instance;
};
static_assert(sizeof(GpuLightRecord) == 16);
inline constexpr std::uint32_t kNoLights = 0xFFFFFFFFU;
// A light table entry whose `instance` has this bit is an analytic light:
// the other bits index the analytic light records, which follow the
// entries, 16-byte aligned. The emissive triangles' entries come first.
inline constexpr std::uint32_t kAnalyticLight = 0x80000000U;
// The `instance` of the light table's last entry when it is the environment
// image's.
inline constexpr std::uint32_t kEnvironmentLight = 0xFFFFFFFFU;

// GpuAnalyticLightRecord::shape. A distant light of angle 0 is a parallel
// light.
inline constexpr std::uint32_t kSphereLight = 0;
inline constexpr std::uint32_t kRectLight = 1;
inline constexpr std::uint32_t kDiskLight = 2;
inline constexpr std::uint32_t kDistantLight = 3;
inline constexpr std::uint32_t kParallelLight = 4;

// One analytic light in world space, as the scene passes sample and
// intersect it. Its mirror is `AnalyticLightRecord` in
// shaders/common/scene.slang.
struct GpuAnalyticLightRecord {
  // A sphere's, rect's or disk's centre; w: a sphere's radius.
  float position[4];
  // A rect's or disk's half axes: its points are position + a * axis_u +
  // b * axis_v for |a|, |b| <= 1, or a^2 + b^2 <= 1. A distant light's
  // axis_u is the unit direction towards it, and axis_u.w its cone's
  // 1 - cos(theta_max).
  float axis_u[4];
  float axis_v[4];
  // A rect's or disk's unit normal on its emitting side; w: the world-space
  // area of a sphere, rect or disk.
  float normal[4];
  // The radiance after normalization, or a parallel light's irradiance at
  // normal incidence; w: the probability with which the light table chooses
  // the light, 0 without an entry.
  float radiance[4];
  std::uint32_t shape;
  std::uint32_t reserved[3];
};
static_assert(sizeof(GpuAnalyticLightRecord) == 96);

// The environment image as the scene passes look it up and sample it. Its
// mirror is `EnvironmentRecord` in shaders/common/scene.slang. The record
// starts the environment buffer; its sampling distribution follows, as
// floats over `columns` x `rows` cells of the image, rows from the top: each
// cell's density per unit image area, then the rows' cumulative
// distribution (rows + 1 entries from 0 to 1), then each row's cumulative
// distribution over its cells (columns + 1 entries each).
struct GpuEnvironmentRecord {
  // The rows of the rotation from world space to the image's frame: its
  // unit axes; w unused.
  float environment_from_world[3][4];
  // The texels' RGB scale; w unused.
  float scale[4];
  // The image's texture table slot and the sampler that repeats it across
  // longitudes and clamps it at the poles.
  std::uint32_t texture;
  std::uint32_t sampler;
  std::uint32_t columns;
  std::uint32_t rows;
};
static_assert(sizeof(GpuEnvironmentRecord) == 80);
// The most cells of an environment image's distribution along each axis.
// A larger image's texels are averaged into them.
inline constexpr std::uint32_t kMaxEnvironmentColumns = 2048;
inline constexpr std::uint32_t kMaxEnvironmentRows = 1024;

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
  // The light table's per-instance offsets and entries, under the same
  // conditions; the offsets cover every instance record.
  [[nodiscard]] VkDeviceAddress LightOffsetAddress() const {
    return lights_.address;
  }
  [[nodiscard]] VkDeviceAddress LightAddress() const {
    return lights_.address == 0 ? 0 : lights_.address + light_entries_offset_;
  }
  [[nodiscard]] std::uint32_t LightCount() const {
    return light_count_;
  }
  // The analytic light records, under the same conditions.
  [[nodiscard]] VkDeviceAddress AnalyticLightAddress() const {
    return lights_.address == 0 ? 0 : lights_.address + analytic_offset_;
  }
  [[nodiscard]] std::uint32_t AnalyticLightCount() const {
    return analytic_count_;
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
  // The environment buffer's address, zero without an image that emits or
  // without acceleration structures.
  [[nodiscard]] VkDeviceAddress EnvironmentAddress() const {
    return environment_lit_ ? environment_buffer_.address : 0;
  }
  // The probability with which the light table chooses the environment
  // image, 0 without its entry.
  [[nodiscard]] float EnvironmentSelection() const {
    return environment_selection_;
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
  // The last instance plan, from which the light table is built.
  std::vector<SceneInstance> scene_instances_;
  // The last light plan, from which the analytic records are built.
  std::vector<Light> scene_lights_;
  DeviceBuffer lights_;
  VkDeviceSize light_entries_offset_ = 0;
  std::uint32_t light_count_ = 0;
  VkDeviceSize analytic_offset_ = 0;
  std::uint32_t analytic_count_ = 0;
  std::uint32_t emissive_triangles_ = 0;
  std::array<float, 3> environment_{};
  // The last plan's environment image; its buffer, which holds its record
  // and distribution when `environment_lit_`; and the mean of the
  // distribution's unnormalized cell values, which the light table weighs
  // it by.
  std::optional<SceneEnvironmentMap> environment_map_;
  DeviceBuffer environment_buffer_;
  bool environment_lit_ = false;
  double environment_mean_ = 0.0;
  float environment_selection_ = 0.0F;
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
