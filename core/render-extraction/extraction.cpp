// SPDX-License-Identifier: Apache-2.0
#include <lotus/extraction.hpp>

#include <string_view>
#include <unordered_map>
#include <utility>

namespace Lotus {

DrawSummary ExtractDrawSummary(const FrameSnapshot& snapshot) {
  return DrawSummary{
      snapshot.revision,
      snapshot.triangle_count == 0 ? 0U : 1U,
      snapshot.triangle_count == 0 ? 0U : 1U,
      Multiply(snapshot.camera.projection, snapshot.camera.view),
  };
}

bool SceneUpdate::Empty() const {
  return geometry_releases.empty() && geometry_uploads.empty() &&
         !instances_changed && !materials_changed && !environment_changed;
}

SceneUpdate SceneExtraction::Update(const FrameSnapshot& snapshot) {
  SceneUpdate update;
  update.source_revision = snapshot.revision;
  if (!reset_ && snapshot.scene == scene_) {
    return update;
  }

  std::unordered_map<const MeshGeometry*, std::shared_ptr<const MeshGeometry>>
      resident;
  std::vector<SceneInstance> instances;
  std::vector<Material> materials{Material{}};
  if (snapshot.scene) {
    // Slot 0 is the default material, for unbound meshes and bindings to a
    // key without a material.
    std::unordered_map<std::string_view, std::uint32_t> slots;
    for (const auto& [id, material] : snapshot.scene->materials) {
      slots.emplace(id, static_cast<std::uint32_t>(materials.size()));
      materials.push_back(material);
    }
    for (const auto& [id, mesh] : snapshot.scene->meshes) {
      (void)id;
      const MeshGeometry* geometry = mesh.geometry.get();
      if (geometry->triangles.empty()) {
        continue;
      }
      if (resident.emplace(geometry, mesh.geometry).second &&
          !resident_.contains(geometry)) {
        update.geometry_uploads.push_back(mesh.geometry);
      }
      if (mesh.instance.visible) {
        const auto slot = slots.find(mesh.material);
        const std::uint32_t material = slot == slots.end() ? 0 : slot->second;
        for (const Matrix4& placement : PlacementTransforms(mesh.instance)) {
          instances.push_back({geometry, placement, material});
        }
      }
    }
  }
  if (scene_) {
    for (const auto& [id, mesh] : scene_->meshes) {
      (void)id;
      const MeshGeometry* geometry = mesh.geometry.get();
      // Erasing from resident_ skips a buffer shared by several meshes.
      if (!resident.contains(geometry) && resident_.erase(geometry) != 0) {
        update.geometry_releases.push_back(geometry);
      }
    }
  }

  if (instances != instances_) {
    update.instances_changed = true;
    update.instances = instances;
    instances_ = std::move(instances);
  }
  if (materials != materials_) {
    update.materials_changed = true;
    update.materials = materials;
    materials_ = std::move(materials);
  }
  const std::array<float, 3> environment =
      snapshot.scene ? snapshot.scene->environment : std::array<float, 3>{};
  if (environment != environment_) {
    update.environment_changed = true;
    update.environment = environment;
    environment_ = environment;
  }
  resident_ = std::move(resident);
  scene_ = snapshot.scene;
  reset_ = false;
  return update;
}

void SceneExtraction::Reset() {
  scene_.reset();
  resident_.clear();
  instances_.clear();
  materials_ = {Material{}};
  environment_ = {};
  reset_ = true;
}

} // namespace Lotus
