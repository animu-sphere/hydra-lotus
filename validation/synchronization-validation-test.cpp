// SPDX-License-Identifier: Apache-2.0
// Prove the shared instance setup actually enables synchronization checking:
// synchronized writes are clean, while removing their barrier reports WAW.
// The invalid command buffer is recorded only, never submitted to the GPU.
#include "../backend/vulkan/vulkan_internal.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using namespace Lotus::vulkan_internal;

struct Resources {
  ValidationState validation;
  InstanceState instance;
  VkDevice device = VK_NULL_HANDLE;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;

  ~Resources() {
    if (device != VK_NULL_HANDLE) {
      if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, pool, nullptr);
      if (buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, buffer, nullptr);
      if (memory != VK_NULL_HANDLE) vkFreeMemory(device, memory, nullptr);
      vkDestroyDevice(device, nullptr);
    }
    DestroyInstance(instance);
  }
};

void Check(VkResult result, const char* operation) {
  std::string detail;
  if (!VulkanOk(result, operation, detail)) throw std::runtime_error(detail);
}

int Run() {
  Resources resources;
  std::string detail;
  // Establish Vulkan 1.3 driver support before enabling validation. Only an
  // incompatible driver is a capability SKIP; other setup errors still fail.
  VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  application.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo probe{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  probe.pApplicationInfo = &application;
  const VkResult probe_result = vkCreateInstance(&probe, nullptr, &resources.instance.instance);
  if (probe_result == VK_ERROR_INCOMPATIBLE_DRIVER) {
    std::cout << "SKIP: Vulkan 1.3 driver unavailable (VK_ERROR_INCOMPATIBLE_DRIVER)\n";
    return 77;
  }
  Check(probe_result, "probe Vulkan driver");
  DestroyInstance(resources.instance);
  if (!CreateInstanceWithValidation("lotus-synchronization-test", {},
          &resources.validation, resources.instance, detail)) {
    throw std::runtime_error(detail);
  }
  if (!resources.instance.synchronization_validation_available) {
    std::cout << "SKIP: "
              << resources.instance.synchronization_validation_detail << '\n';
    return 77;
  }
  std::uint32_t count = 0;
  Check(vkEnumeratePhysicalDevices(resources.instance.instance, &count, nullptr),
      "enumerate devices");
  std::vector<VkPhysicalDevice> devices(count);
  Check(vkEnumeratePhysicalDevices(resources.instance.instance, &count, devices.data()),
      "enumerate devices");
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  std::uint32_t family = 0;
  for (VkPhysicalDevice device : devices) {
    std::uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_count, queues.data());
    for (std::uint32_t index = 0; index < queue_count; ++index) {
      if (queues[index].queueCount != 0 &&
          (queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
        physical = device;
        family = index;
        break;
      }
    }
    if (physical != VK_NULL_HANDLE) break;
  }
  if (physical == VK_NULL_HANDLE) {
    std::cout << "SKIP: no Vulkan device with a graphics queue\n";
    return 77;
  }
  const float priority = 1.0F;
  VkDeviceQueueCreateInfo queue_create{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue_create.queueFamilyIndex = family;
  queue_create.queueCount = 1;
  queue_create.pQueuePriorities = &priority;
  VkDeviceCreateInfo device_create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  device_create.queueCreateInfoCount = 1;
  device_create.pQueueCreateInfos = &queue_create;
  Check(vkCreateDevice(physical, &device_create, nullptr, &resources.device),
      "create device");
  VkBufferCreateInfo buffer_create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_create.size = 256;
  buffer_create.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  Check(vkCreateBuffer(resources.device, &buffer_create, nullptr, &resources.buffer),
      "create buffer");
  VkMemoryRequirements requirements;
  vkGetBufferMemoryRequirements(resources.device, resources.buffer, &requirements);
  VkPhysicalDeviceMemoryProperties properties;
  vkGetPhysicalDeviceMemoryProperties(physical, &properties);
  std::uint32_t memory_type = 0;
  while (memory_type < properties.memoryTypeCount &&
         (requirements.memoryTypeBits & (1U << memory_type)) == 0) ++memory_type;
  if (memory_type == properties.memoryTypeCount)
    throw std::runtime_error("no compatible memory type");
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = memory_type;
  Check(vkAllocateMemory(resources.device, &allocate, nullptr, &resources.memory),
      "allocate memory");
  Check(vkBindBufferMemory(resources.device, resources.buffer, resources.memory, 0),
      "bind buffer memory");
  VkCommandPoolCreateInfo pool_create{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_create.queueFamilyIndex = family;
  pool_create.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  Check(vkCreateCommandPool(resources.device, &pool_create, nullptr, &resources.pool),
      "create command pool");
  VkCommandBufferAllocateInfo command_allocate{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  command_allocate.commandPool = resources.pool;
  command_allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_allocate.commandBufferCount = 1;
  VkCommandBuffer command;
  Check(vkAllocateCommandBuffers(resources.device, &command_allocate, &command),
      "allocate command buffer");
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  Check(vkBeginCommandBuffer(command, &begin), "begin synchronized writes");
  vkCmdFillBuffer(command, resources.buffer, 0, 256, 0);
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
  vkCmdFillBuffer(command, resources.buffer, 0, 256, 1);
  Check(vkEndCommandBuffer(command), "end synchronized writes");
  if (resources.validation.message_count != 0)
    throw std::runtime_error("synchronized writes: " + resources.validation.first_message);

  Check(vkResetCommandBuffer(command, 0), "reset command buffer");
  Check(vkBeginCommandBuffer(command, &begin), "begin unsynchronized writes");
  vkCmdFillBuffer(command, resources.buffer, 0, 256, 0);
  vkCmdFillBuffer(command, resources.buffer, 0, 256, 1);
  Check(vkEndCommandBuffer(command), "end unsynchronized writes");
  if (resources.validation.message_count != 1 ||
      resources.validation.first_message.find("SYNC-HAZARD-WRITE-AFTER-WRITE") ==
          std::string::npos) {
    throw std::runtime_error("missing expected synchronization hazard: " +
                            resources.validation.first_message);
  }
  std::cout << "PASS: synchronized writes clean; missing barrier captured as "
               "SYNC-HAZARD-WRITE-AFTER-WRITE\n";
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
