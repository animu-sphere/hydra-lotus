// SPDX-License-Identifier: Apache-2.0
#include "../backend/vulkan/gpu_memory.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

using Lotus::vulkan_internal::MemoryRange;
using Lotus::vulkan_internal::MemoryRanges;

int main() {
  // Independent byte-occupancy oracle: varied alignments, holes, failed
  // requests and arbitrary release order must never overlap a live range.
  constexpr std::size_t capacity = 4096;
  MemoryRanges ranges(capacity);
  std::vector<bool> occupied(capacity);
  std::vector<MemoryRange> live;
  std::mt19937 random(701);
  for (int step = 0; step < 10000; ++step) {
    if (!live.empty() && random() % 3 == 0) {
      const auto index = random() % live.size();
      const auto range = live[index];
      for (auto byte = range.offset; byte < range.offset + range.size; ++byte) {
        occupied[byte] = false;
      }
      ranges.Release(range);
      live.erase(live.begin() + index);
    } else {
      const auto size = 1 + random() % 257;
      const auto alignment = VkDeviceSize{1} << (random() % 9);
      std::optional<VkDeviceSize> expected;
      for (VkDeviceSize offset = 0; offset + size <= capacity; offset += alignment) {
        if (std::none_of(occupied.begin() + offset, occupied.begin() + offset + size,
                [](bool value) { return value; })) {
          expected = offset;
          break;
        }
      }
      const auto allocated = ranges.Allocate(size, alignment);
      if (bool(allocated) != bool(expected) ||
          (allocated && (allocated->offset != *expected || allocated->size != size))) {
        std::cerr << "allocation disagrees with byte-occupancy oracle at " << step << '\n';
        return 1;
      }
      if (allocated) {
        live.push_back(*allocated);
        for (auto byte = allocated->offset; byte < allocated->offset + size; ++byte) {
          occupied[byte] = true;
        }
      }
    }
    std::size_t free = 0, largest = 0, run = 0;
    for (bool byte : occupied) {
      if (!byte) {
        ++free;
        largest = std::max(largest, ++run);
      } else
        run = 0;
    }
    if (ranges.FreeBytes() != free || ranges.LargestFreeRange() != largest) {
      std::cerr << "fragmentation statistics disagree with occupancy\n";
      return 1;
    }
  }
  for (auto range : live)
    ranges.Release(range);
  if (ranges.FreeBytes() != capacity || ranges.LargestFreeRange() != capacity ||
      !ranges.Allocate(capacity, 256) || ranges.Allocate(1, 1))
    return 1;
  MemoryRanges overflow(std::numeric_limits<VkDeviceSize>::max());
  auto huge = overflow.Allocate(std::numeric_limits<VkDeviceSize>::max() - 3, 1);
  if (!huge || overflow.Allocate(1, 8) || overflow.Allocate(0, 1) ||
      overflow.Allocate(1, 0))
    return 1;
  overflow.Release(*huge);
  if (overflow.LargestFreeRange() != std::numeric_limits<VkDeviceSize>::max())
    return 1;
  std::cout << "10,000 allocation/release steps matched independent byte occupancy\n";
  return 0;
}
