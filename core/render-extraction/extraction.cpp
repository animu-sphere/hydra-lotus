// SPDX-License-Identifier: Apache-2.0
#include <lotus/extraction.hpp>

#include <iterator>
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
         texture_releases.empty() && texture_uploads.empty() &&
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
  std::unordered_map<const Texture*, std::shared_ptr<const Texture>>
      resident_textures;
  std::vector<SceneInstance> instances;
  std::vector<SceneMaterial> materials{SceneMaterial{}};
  if (snapshot.scene) {
    for (const auto& [id, texture] : snapshot.scene->textures) {
      (void)id;
      resident_textures.emplace(texture.get(), texture);
      if (!resident_textures_.contains(texture.get())) {
        update.texture_uploads.push_back(texture);
      }
    }
    // Slot 0 is the default material, for unbound meshes and bindings to a
    // key without a material.
    std::unordered_map<std::string_view, std::uint32_t> slots;
    for (const auto& [id, material] : snapshot.scene->materials) {
      slots.emplace(id, static_cast<std::uint32_t>(materials.size()));
      SceneMaterial& entry = materials.emplace_back();
      entry.material = material;
      const auto inputs = TextureInputs(material);
      for (std::size_t index = 0; index < inputs.size(); ++index) {
        if (!inputs[index]->has_value()) {
          continue;
        }
        const auto found =
            snapshot.scene->textures.find((*inputs[index])->texture);
        if (found != snapshot.scene->textures.end()) {
          entry.textures[index] = found->second.get();
        }
      }
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
        // The set the material's lookups read, by its position in key order.
        std::uint32_t texcoords = kNoTexcoords;
        const Material& bound = materials[material].material;
        if (HasTextureInputs(bound) || bound.normal != Material{}.normal) {
          const auto set = geometry->texcoords.find(bound.texcoords);
          if (set != geometry->texcoords.end()) {
            texcoords = static_cast<std::uint32_t>(
                std::distance(geometry->texcoords.begin(), set));
          }
        }
        for (const Matrix4& placement : PlacementTransforms(mesh.instance)) {
          instances.push_back({geometry, placement, material, texcoords});
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
    for (const auto& [id, texture] : scene_->textures) {
      (void)id;
      // Erasing skips a texture held under several keys.
      if (!resident_textures.contains(texture.get()) &&
          resident_textures_.erase(texture.get()) != 0) {
        update.texture_releases.push_back(texture.get());
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
  resident_textures_ = std::move(resident_textures);
  scene_ = snapshot.scene;
  reset_ = false;
  return update;
}

void SceneExtraction::Reset() {
  scene_.reset();
  resident_.clear();
  resident_textures_.clear();
  instances_.clear();
  materials_ = {SceneMaterial{}};
  environment_ = {};
  reset_ = true;
}

} // namespace Lotus
