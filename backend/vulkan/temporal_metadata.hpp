// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
#include <lotus/temporal.hpp>
#include <lotus/extraction.hpp>

namespace Lotus::vulkan_internal {

// Mirrors TemporalInstance in shaders/common/temporal.slang.
struct TemporalInstanceRecord {
  Matrix4 previous_world_from_object = IdentityMatrix();
  std::uint64_t instance_id = 0;
  std::uint64_t material_id = 0;
  std::uint32_t flags = 0;
  std::array<std::uint32_t, 3> reserved{};
};
static_assert(sizeof(TemporalInstanceRecord) == 96);

// A transaction copies this small table and publishes it only after GPU
// completion. IDs never depend on GPU slots, hashes or resource addresses.
class TemporalIdentities {
public:
  std::vector<TemporalInstanceRecord> Build(const TemporalFrame& frame);

private:
  using PlacementKey = std::pair<std::string, std::size_t>;
  struct MaterialIdentity {
    SceneMaterial material;
    std::uint64_t id = 0;
  };
  std::optional<FrameSnapshot> last_;
  std::map<PlacementKey, std::uint64_t> placements_;
  std::map<std::string, MaterialIdentity> materials_;
  std::uint64_t next_id_ = 1;
};

} // namespace Lotus::vulkan_internal
