// SPDX-License-Identifier: Apache-2.0
// Isolated size/copy experiment. Compacted structures are never installed in
// the renderer's TLAS; this does not claim traversal or image equivalence.
#include "../backend/vulkan/gpu_scene.hpp"
#include "../backend/vulkan/vulkan_internal.hpp"

#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace {
using namespace Lotus::vulkan_internal;
void Require(bool condition, const std::string& detail) {
  if (!condition)
    throw std::runtime_error(detail);
}
void Check(VkResult result, const char* operation) {
  std::string detail;
  const bool ok = VulkanOk(result, operation, detail);
  Require(ok, detail);
}

struct Resources {
  ValidationState validation;
  InstanceState instance;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  SceneMemory memory;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer command = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkQueryPool timestamps = VK_NULL_HANDLE;
  VkQueryPool sizes = VK_NULL_HANDLE;
  std::uint32_t timestamp_bits = 0;
  float timestamp_period = 0;
  std::vector<DeviceBuffer> buffers;
  std::vector<VkAccelerationStructureKHR> structures;
  PFN_vkDestroyAccelerationStructureKHR destroy = nullptr;
  ~Resources() {
    if (device != VK_NULL_HANDLE) {
      vkDeviceWaitIdle(device);
      for (auto structure : structures)
        destroy(device, structure, nullptr);
      for (auto& buffer : buffers) {
        vkDestroyBuffer(device, buffer.buffer, nullptr);
        memory.Release(buffer.allocation);
      }
      memory.Destroy();
      vkDestroyQueryPool(device, sizes, nullptr);
      vkDestroyQueryPool(device, timestamps, nullptr);
      vkDestroyFence(device, fence, nullptr);
      vkDestroyCommandPool(device, pool, nullptr);
      vkDestroyDevice(device, nullptr);
    }
    DestroyInstance(instance);
  }
  DeviceBuffer Buffer(VkDeviceSize bytes, VkBufferUsageFlags usage, bool host = false) {
    auto& buffer = buffers.emplace_back();
    VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    create.size = bytes;
    create.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    Check(vkCreateBuffer(device, &create, nullptr, &buffer.buffer), "create buffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer.buffer, &requirements);
    std::string detail;
    const bool allocated = memory.Allocate(requirements,
        host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        host ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0, true,
        VK_NULL_HANDLE, VK_NULL_HANDLE, false, buffer.allocation, detail);
    Require(allocated, detail);
    Check(vkBindBufferMemory(device, buffer.buffer, buffer.allocation.memory,
              buffer.allocation.offset),
        "bind buffer");
    VkBufferDeviceAddressInfo address{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    address.buffer = buffer.buffer;
    buffer.address = vkGetBufferDeviceAddress(device, &address);
    buffer.size = bytes;
    buffer.mapped = buffer.allocation.mapped;
    return buffer;
  }
  VkAccelerationStructureKHR Structure(VkDeviceSize bytes) {
    const auto buffer = Buffer(bytes, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
    VkAccelerationStructureCreateInfoKHR create{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    create.buffer = buffer.buffer;
    create.size = bytes;
    create.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    const auto make = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
        vkGetDeviceProcAddr(device, "vkCreateAccelerationStructureKHR"));
    auto& structure = structures.emplace_back(VK_NULL_HANDLE);
    Check(make(device, &create, nullptr, &structure), "create BLAS");
    return structure;
  }
  void Begin() {
    Check(vkResetCommandBuffer(command, 0), "reset command");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(command, &begin), "begin command");
    if (timestamps) {
      vkCmdResetQueryPool(command, timestamps, 0, 2);
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, 0);
    }
  }
  double Finish() {
    if (timestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, 1);
    Check(vkEndCommandBuffer(command), "end command");
    Check(vkResetFences(device, 1, &fence), "reset fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    Check(vkQueueSubmit(queue, 1, &submit, fence), "submit");
    Check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "wait");
    if (!timestamps)
      return 0;
    std::uint64_t ticks[2]{};
    Check(vkGetQueryPoolResults(device, timestamps, 0, 2, sizeof(ticks), ticks,
              sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
        "timestamps");
    const auto mask = timestamp_bits == 64 ? ~std::uint64_t{0}
                                           : (std::uint64_t{1} << timestamp_bits) - 1;
    return double((ticks[1] - ticks[0]) & mask) * timestamp_period / 1e6;
  }
};

void CheckReplacementPlans(Resources& r, VkPhysicalDevice physical, std::uint32_t family) {
  struct SceneOwner {
    GpuScene scene;
    ~SceneOwner() {
      scene.Destroy();
    }
  } owner;
  auto& scene = owner.scene;
  std::string detail;
  const bool initialized = scene.Initialize(physical, r.device, r.queue, family,
      ProbeAccelerationStructures(physical), detail);
  Require(initialized, detail);
  auto a = std::make_shared<Lotus::MeshGeometry>(Lotus::MeshGeometry{
      {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, {{0, 1, 2}}, {0}});
  auto b = std::make_shared<Lotus::MeshGeometry>(*a);
  b->positions[0][2] = 1;
  Lotus::SceneUpdate update;
  update.geometry_uploads = {a, b};
  update.instances_changed = true;
  update.instances = {{a.get()}, {b.get()}};
  const bool applied = scene.Apply(update, detail);
  Require(applied, detail);
  const auto before = scene.Stats();
  // Bad correspondence and non-finite refit input must fail before mutation.
  const auto rejects = [&](const Lotus::SceneUpdate& invalid) {
    Require(!scene.Apply(invalid, detail), "invalid replacement plan was accepted");
    const auto after = scene.Stats();
    Require(after.memory == before.memory && after.blas_builds == before.blas_builds &&
                after.blas_updates == before.blas_updates &&
                after.geometry_releases == before.geometry_releases,
        "invalid replacement plan changed scene ownership");
  };
  Lotus::SceneUpdate invalid;
  invalid.geometry_replacements = {{a.get(), b.get()}};
  rejects(invalid);
  auto bad = std::make_shared<Lotus::MeshGeometry>(*a);
  bad->positions[0][0] = std::numeric_limits<float>::quiet_NaN();
  invalid.geometry_releases = {a.get()};
  invalid.geometry_uploads = {bad};
  invalid.geometry_replacements = {{a.get(), bad.get()}};
  rejects(invalid);
  bad->positions[0][0] = 0;
  invalid.geometry_replacements.push_back({a.get(), bad.get()});
  rejects(invalid);
  // A valid direct plan can reuse released source addresses in a permutation.
  // Resolve every old slot before changing the source-to-slot map.
  update.geometry_releases = {a.get(), b.get()};
  update.geometry_replacements = {{a.get(), b.get()}, {b.get(), a.get()}};
  const bool swapped = scene.Apply(update, detail);
  Require(swapped, detail);
  Lotus::GpuSceneContents contents;
  const bool read = scene.ReadBack(contents, detail);
  Require(read, detail);
  Require(contents.geometries.size() == 2 &&
              contents.geometries[0].positions == b->positions &&
              contents.geometries[1].positions == a->positions &&
              scene.Stats().blas_builds == before.blas_builds &&
              scene.Stats().blas_updates == before.blas_updates + 2,
      "replacement permutation lost geometry or rebuilt compatible BLASes");
  std::cout << "PASS: replacement rejection is atomic; source permutations refit and read back correctly\n";
}

int Run() {
  Resources r;
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo probe{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  probe.pApplicationInfo = &app;
  const auto status = vkCreateInstance(&probe, nullptr, &r.instance.instance);
  if (status == VK_ERROR_INCOMPATIBLE_DRIVER) {
    std::cout << "SKIP: Vulkan 1.3 driver unavailable (VK_ERROR_INCOMPATIBLE_DRIVER)\n";
    return 77;
  }
  Check(status, "probe driver");
  DestroyInstance(r.instance);
  std::string detail;
  const bool created = CreateInstanceWithValidation("lotus-compaction-test", {},
      &r.validation, r.instance, detail);
  Require(created, detail);
  std::uint32_t count = 0;
  Check(vkEnumeratePhysicalDevices(r.instance.instance, &count, nullptr), "enumerate devices");
  std::vector<VkPhysicalDevice> devices(count);
  Check(vkEnumeratePhysicalDevices(r.instance.instance, &count, devices.data()), "enumerate devices");
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  std::uint32_t family = 0;
  for (auto device : devices) {
    if (!ProbeAccelerationStructures(device).available)
      continue;
    std::uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_count, queues.data());
    for (std::uint32_t i = 0; i < queue_count; ++i) {
      if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
        physical = device;
        family = i;
        r.timestamp_bits = queues[i].timestampValidBits;
        break;
      }
    }
    if (physical)
      break;
  }
  if (!physical) {
    std::cout << "SKIP: no acceleration-structure device with a compute queue\n";
    return 77;
  }
  VkPhysicalDeviceAccelerationStructurePropertiesKHR as_properties{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
  VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  properties.pNext = &as_properties;
  vkGetPhysicalDeviceProperties2(physical, &properties);
  r.timestamp_period = properties.properties.limits.timestampPeriod;
  const float priority = 1;
  VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue.queueFamilyIndex = family;
  queue.queueCount = 1;
  queue.pQueuePriorities = &priority;
  VkPhysicalDeviceAccelerationStructureFeaturesKHR as_features{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
  as_features.accelerationStructure = VK_TRUE;
  VkPhysicalDeviceVulkan12Features features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  features.bufferDeviceAddress = VK_TRUE;
  features.pNext = &as_features;
  VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  create.pNext = &features;
  create.queueCreateInfoCount = 1;
  create.pQueueCreateInfos = &queue;
  create.enabledExtensionCount = static_cast<std::uint32_t>(kAccelerationExtensions.size());
  create.ppEnabledExtensionNames = kAccelerationExtensions.data();
  Check(vkCreateDevice(physical, &create, nullptr, &r.device), "create device");
  r.destroy = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
      vkGetDeviceProcAddr(r.device, "vkDestroyAccelerationStructureKHR"));
  vkGetDeviceQueue(r.device, family, 0, &r.queue);
  r.memory.Initialize(physical, r.device);
  CheckReplacementPlans(r, physical, family);
  VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool.queueFamilyIndex = family;
  pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  Check(vkCreateCommandPool(r.device, &pool, nullptr, &r.pool), "create pool");
  VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocate.commandPool = r.pool;
  allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate.commandBufferCount = 1;
  Check(vkAllocateCommandBuffers(r.device, &allocate, &r.command), "allocate command");
  VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  Check(vkCreateFence(r.device, &fence, nullptr, &r.fence), "create fence");
  VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  query.queryCount = 2;
  query.queryType = VK_QUERY_TYPE_TIMESTAMP;
  if (r.timestamp_bits)
    Check(vkCreateQueryPool(r.device, &query, nullptr, &r.timestamps), "create timestamps");
  query.queryCount = 1;
  query.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
  Check(vkCreateQueryPool(r.device, &query, nullptr, &r.sizes), "create size query");

  constexpr std::uint32_t quads = 256;
  constexpr std::uint32_t triangles = 2 * quads * quads;
  std::vector<std::array<float, 3>> positions;
  for (unsigned y = 0; y < quads; ++y) {
    for (unsigned x = 0; x < quads; ++x) {
      const float a = float(x) / quads, b = float(y) / quads;
      const float c = float(x + 1) / quads, d = float(y + 1) / quads;
      for (auto point : {std::array{a, b, 0.0F}, std::array{c, b, 0.0F},
               std::array{c, d, 0.0F}, std::array{a, b, 0.0F},
               std::array{c, d, 0.0F}, std::array{a, d, 0.0F}})
        positions.push_back(point);
    }
  }
  const auto input = r.Buffer(positions.size() * sizeof(positions[0]),
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true);
  std::memcpy(input.mapped, positions.data(), input.size);
  if (!input.allocation.coherent) {
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = input.allocation.memory;
    range.offset = input.allocation.offset;
    range.size = input.allocation.size;
    Check(vkFlushMappedMemoryRanges(r.device, 1, &range), "flush input");
  }
  VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
  geometry.flags = VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
  auto& data = geometry.geometry.triangles;
  data.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
  data.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
  data.vertexData.deviceAddress = input.address;
  data.vertexStride = sizeof(positions[0]);
  data.maxVertex = static_cast<std::uint32_t>(positions.size() - 1);
  data.indexType = VK_INDEX_TYPE_NONE_KHR;
  VkAccelerationStructureBuildGeometryInfoKHR build{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build.geometryCount = 1;
  build.pGeometries = &geometry;
  VkAccelerationStructureBuildRangeInfoKHR range{};
  range.primitiveCount = triangles;
  const auto* range_pointer = &range;
  const auto get_sizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
      vkGetDeviceProcAddr(r.device, "vkGetAccelerationStructureBuildSizesKHR"));
  const auto execute = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
      vkGetDeviceProcAddr(r.device, "vkCmdBuildAccelerationStructuresKHR"));
  const auto write_sizes = reinterpret_cast<PFN_vkCmdWriteAccelerationStructuresPropertiesKHR>(
      vkGetDeviceProcAddr(r.device, "vkCmdWriteAccelerationStructuresPropertiesKHR"));
  const auto copy = reinterpret_cast<PFN_vkCmdCopyAccelerationStructureKHR>(
      vkGetDeviceProcAddr(r.device, "vkCmdCopyAccelerationStructureKHR"));
  std::cout << std::fixed << std::setprecision(6) << properties.properties.deviceName
            << ": " << triangles << " triangles; 1 warmup + 8 measured builds per mode\n";
  for (unsigned mode = 0; mode < 3; ++mode) {
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    if (mode > 0)
      build.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    if (mode == 2)
      build.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    VkAccelerationStructureBuildSizesInfoKHR size_info{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    get_sizes(r.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &build, &triangles, &size_info);
    build.dstAccelerationStructure = r.Structure(size_info.accelerationStructureSize);
    const auto alignment = as_properties.minAccelerationStructureScratchOffsetAlignment;
    const auto scratch = r.Buffer(size_info.buildScratchSize + alignment,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    build.scratchData.deviceAddress = (scratch.address + alignment - 1) / alignment * alignment;
    double gpu_sum = 0, cpu_sum = 0;
    for (unsigned iteration = 0; iteration < 9; ++iteration) {
      const auto begin = std::chrono::steady_clock::now();
      r.Begin();
      execute(r.command, 1, &build, &range_pointer);
      VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
      barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                              VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
      vkCmdPipelineBarrier(r.command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
          VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &barrier, 0, nullptr, 0, nullptr);
      const double gpu = r.Finish();
      const double cpu = std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - begin)
                             .count();
      if (iteration) {
        gpu_sum += gpu;
        cpu_sum += cpu;
      }
    }
    std::cout << "mode=" << mode << " storage_bytes=" << size_info.accelerationStructureSize
              << " build_gpu_ms=" << (r.timestamps ? std::to_string(gpu_sum / 8) : "unavailable")
              << " build_cpu_wall_ms=" << cpu_sum / 8;
    if (mode == 2) {
      const auto begin = std::chrono::steady_clock::now();
      r.Begin();
      vkCmdResetQueryPool(r.command, r.sizes, 0, 1);
      write_sizes(r.command, 1, &build.dstAccelerationStructure,
          VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, r.sizes, 0);
      const auto query_ms = r.Finish();
      std::uint64_t compacted = 0;
      Check(vkGetQueryPoolResults(r.device, r.sizes, 0, 1, sizeof(compacted), &compacted,
                sizeof(compacted), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
          "compacted size");
      Require(compacted > 0, "compacted size is zero");
      const auto destination = r.Structure(compacted);
      VkCopyAccelerationStructureInfoKHR compact{VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
      compact.src = build.dstAccelerationStructure;
      compact.dst = destination;
      compact.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
      r.Begin();
      copy(r.command, &compact);
      const auto copy_ms = r.Finish();
      std::cout << " compacted_bytes=" << compacted << " size_query_gpu_ms=" << query_ms
                << " compact_copy_gpu_ms=" << copy_ms << " compact_cpu_wall_ms="
                << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    }
    std::cout << '\n';
  }
  Require(r.validation.message_count == 0, r.validation.first_message);
  std::cout << "PASS: BLAS build flags, compacted-size query and compact copy; validation_messages=0\n";
  return 0;
}
} // namespace
int main() try { return Run(); } catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
