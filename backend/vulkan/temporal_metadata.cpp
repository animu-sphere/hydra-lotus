// SPDX-License-Identifier: Apache-2.0
#include "temporal_metadata.hpp"

namespace Lotus::vulkan_internal {

std::vector<TemporalInstanceRecord> TemporalIdentities::Build(
    const TemporalFrame& frame) {
  std::vector<TemporalInstanceRecord> records;
  std::map<PlacementKey, std::uint64_t> placements;
  std::map<std::string, MaterialIdentity> materials;
  // Identity lifetime is independent of camera cuts and framing resets.
  TemporalFrame identity_frame = frame;
  identity_frame.previous = last_;
  if (frame.current.scene) {
    const auto& scene = *frame.current.scene;
    for (const auto& [key, mesh] : scene.meshes) {
      if (!mesh.instance.visible || !mesh.geometry ||
          mesh.geometry->triangles.empty())
        continue;
      auto found = materials.find(mesh.material);
      if (found == materials.end()) {
        SceneMaterial bound;
        if (const auto it = scene.materials.find(mesh.material);
            it != scene.materials.end())
          bound.material = it->second;
        const auto inputs = TextureInputs(bound.material);
        for (std::size_t i = 0; i < inputs.size(); ++i) {
          if (!inputs[i]->has_value())
            continue;
          const auto texture = scene.textures.find((*inputs[i])->texture);
          if (texture != scene.textures.end())
            bound.textures[i] = texture->second.get();
        }
        const auto old = materials_.find(mesh.material);
        const auto id = old != materials_.end() && old->second.material == bound
                            ? old->second.id
                            : next_id_++;
        found = materials.emplace(mesh.material, MaterialIdentity{bound, id}).first;
      }
      const auto transforms = PlacementTransforms(mesh.instance);
      for (std::size_t ordinal = 0; ordinal < transforms.size(); ++ordinal) {
        const PlacementKey placement_key{key, ordinal};
        const auto old = placements_.find(placement_key);
        const bool same = old != placements_.end() &&
                          FindTemporalPlacement(identity_frame, key, ordinal).has_value();
        const auto id = same ? old->second : next_id_++;
        placements.emplace(placement_key, id);
        TemporalInstanceRecord record;
        record.instance_id = id;
        record.material_id = found->second.id;
        if (const auto pair = FindTemporalPlacement(frame, key, ordinal)) {
          record.previous_world_from_object = pair->previous_world_from_object;
          record.flags = 2U | (pair->material_unchanged ? 4U : 0U);
        }
        records.push_back(record);
      }
    }
  }
  placements_ = std::move(placements);
  materials_ = std::move(materials);
  last_ = frame.current;
  return records;
}

} // namespace Lotus::vulkan_internal
