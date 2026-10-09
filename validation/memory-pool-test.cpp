// SPDX-License-Identifier: Apache-2.0
#include "../backend/vulkan/gpu_memory.hpp"
#include "../backend/vulkan/vulkan_internal.hpp"

#include <cstring>
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
  Require(VulkanOk(result, operation, detail), detail);
}

struct Buffer {
  VkBuffer handle = VK_NULL_HANDLE;
  MemorySlice slice;
};
struct Resources {
  ValidationState validation;
  InstanceState instance;
  VkDevice device = VK_NULL_HANDLE;
  SceneMemory memory;
  std::vector<Buffer> buffers;
  void Release(unsigned index) {
    vkDestroyBuffer(device, buffers[index].handle, nullptr);
    buffers[index].handle = VK_NULL_HANDLE;
    memory.Release(buffers[index].slice);
  }
  ~Resources() {
    if (device != VK_NULL_HANDLE) {
      for (unsigned i = 0; i < buffers.size(); ++i)
        Release(i);
      memory.Destroy();
      vkDestroyDevice(device, nullptr);
    }
    DestroyInstance(instance);
  }
  unsigned Add(VkDeviceSize size, bool dedicated = false) {
    Buffer& buffer = buffers.emplace_back();
    VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    create.size = size;
    create.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    Check(vkCreateBuffer(device, &create, nullptr, &buffer.handle), "create test buffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer.handle, &requirements);
    std::string detail;
    const bool allocated = memory.Allocate(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false,
        dedicated ? buffer.handle : VK_NULL_HANDLE, VK_NULL_HANDLE, false, buffer.slice, detail);
    Require(allocated, detail);
    Check(vkBindBufferMemory(device, buffer.handle, buffer.slice.memory, buffer.slice.offset), "bind test buffer");
    return static_cast<unsigned>(buffers.size() - 1);
  }
};

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
  const bool created = CreateInstanceWithValidation("lotus-memory-pool-test", {}, &r.validation, r.instance, detail);
  Require(created, detail);
  std::uint32_t count = 0;
  Check(vkEnumeratePhysicalDevices(r.instance.instance, &count, nullptr), "enumerate devices");
  std::vector<VkPhysicalDevice> devices(count);
  Check(vkEnumeratePhysicalDevices(r.instance.instance, &count, devices.data()), "enumerate devices");
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  std::uint32_t family = 0;
  for (auto device : devices) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_3)
      continue;
    std::uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_count, queues.data());
    for (std::uint32_t i = 0; i < queue_count; ++i) {
      if (queues[i].queueCount != 0) {
        physical = device;
        family = i;
        break;
      }
    }
    if (physical != VK_NULL_HANDLE)
      break;
  }
  if (physical == VK_NULL_HANDLE) {
    std::cout << "SKIP: no Vulkan 1.3 device with a queue\n";
    return 77;
  }
  const float priority = 1;
  VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue.queueFamilyIndex = family;
  queue.queueCount = 1;
  queue.pQueuePriorities = &priority;
  VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  create.queueCreateInfoCount = 1;
  create.pQueueCreateInfos = &queue;
  Check(vkCreateDevice(physical, &create, nullptr, &r.device), "create device");
  r.memory.Initialize(physical, r.device);
  const auto a = r.Add(1024);
  const auto b = r.Add(1024);
  Require(r.buffers[a].slice.memory == r.buffers[b].slice.memory &&
              r.buffers[a].slice.offset != r.buffers[b].slice.offset,
      "host buffers did not share memory");
  std::memset(r.buffers[a].slice.mapped, 0x31, 1024);
  std::memset(r.buffers[b].slice.mapped, 0x72, 1024);
  Require(static_cast<unsigned char*>(r.buffers[a].slice.mapped)[0] == 0x31,
      "offset mapping overlapped another slice");
  const auto offset = r.buffers[a].slice.offset;
  const auto allocations = r.memory.Stats().device_allocations;
  r.Release(a);
  const auto c = r.Add(1024);
  Require(r.buffers[c].slice.offset == offset && r.memory.Stats().device_allocations == allocations,
      "released host range was not reused");
  const auto d = r.Add(1024, true);
  Require(r.memory.Stats().dedicated_blocks == 1 && r.buffers[d].slice.offset == 0,
      "dedicated allocation was pooled");
  r.Release(d);
  Require(r.memory.Stats().dedicated_blocks == 0, "dedicated memory was cached");
  // Oversized ranges force separate blocks. At most one empty block of this
  // pool remains cached after arbitrary release order.
  const auto big_a = r.Add(5 * 1024 * 1024);
  const auto big_b = r.Add(5 * 1024 * 1024);
  r.Release(big_b);
  r.Release(big_a);
  const auto before = r.memory.Stats();
  VkMemoryRequirements impossible{1024, 256, 0};
  MemorySlice failed;
  const bool allocated = r.memory.Allocate(impossible, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
      0, false, VK_NULL_HANDLE, VK_NULL_HANDLE, false, failed, detail);
  Require(!allocated && !detail.empty() && r.memory.Stats() == before,
      "incompatible request mutated pool ownership");
  r.Release(b);
  r.Release(c);
  Require(r.memory.Stats().live_suballocations == 0 && r.memory.Stats().blocks == 1,
      "empty-block cache did not retire excess memory");
  r.memory.Destroy();
  Require(r.memory.Stats().blocks == 0 && r.memory.Stats().device_allocations == r.memory.Stats().device_frees,
      "pool destruction leaked backing memory");
  Require(r.validation.message_count == 0, r.validation.first_message);
  std::cout << "PASS: mapped offsets, reuse, dedicated fallback, oversized blocks, failure and teardown\n";
  return 0;
}
} // namespace

int main() {
  try {
    return Run();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
