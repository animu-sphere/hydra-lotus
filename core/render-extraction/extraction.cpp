// SPDX-License-Identifier: Apache-2.0
#include <lotus/extraction.hpp>

#include <utility>

namespace Lotus {

namespace {

Matrix4 Multiply(const Matrix4& left, const Matrix4& right) {
  Matrix4 result{};
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      float sum = 0.0F;
      for (int index = 0; index < 4; ++index) {
        sum += left[index * 4 + row] * right[column * 4 + index];
      }
      result[column * 4 + row] = sum;
    }
  }
  return result;
}

} // namespace

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
         !instances_changed;
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
  if (snapshot.scene) {
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
        instances.push_back({geometry, mesh.instance.world_from_object});
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
  resident_ = std::move(resident);
  scene_ = snapshot.scene;
  reset_ = false;
  return update;
}

void SceneExtraction::Reset() {
  scene_.reset();
  resident_.clear();
  instances_.clear();
  reset_ = true;
}

} // namespace Lotus
