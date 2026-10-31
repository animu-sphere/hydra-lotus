// SPDX-License-Identifier: Apache-2.0
#include <lotus/extraction.hpp>

#include <iterator>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
         geometry_replacements.empty() &&
         texture_releases.empty() && texture_uploads.empty() &&
         !instances_changed && !materials_changed && !environment_changed &&
         !lights_changed;
}

SceneUpdate SceneExtraction::Update(const FrameSnapshot& snapshot) {
  SceneUpdate update;
  update.source_revision = snapshot.revision;
  update.source_scene = snapshot.scene;
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
        std::uint32_t colors = kNoColors;
        if (!bound.base_color_texture) {
          const auto set = geometry->colors.find(bound.base_color_primvar);
          if (set != geometry->colors.end()) {
            colors = static_cast<std::uint32_t>(
                std::distance(geometry->colors.begin(), set));
          }
        }
        for (const Matrix4& placement : PlacementTransforms(mesh.instance)) {
          instances.push_back({geometry, placement, material, texcoords, colors});
        }
      }
    }
  }
  if (scene_) {
    std::unordered_set<const MeshGeometry*> replaced;
    std::unordered_set<const MeshGeometry*> uploaded;
    for (const auto& geometry : update.geometry_uploads) {
      uploaded.insert(geometry.get());
    }
    for (const auto& [id, mesh] : scene_->meshes) {
      const MeshGeometry* geometry = mesh.geometry.get();
      // Erasing from resident_ skips a buffer shared by several meshes.
      if (!resident.contains(geometry) && resident_.erase(geometry) != 0) {
        update.geometry_releases.push_back(geometry);
        if (snapshot.scene) {
          const auto next = snapshot.scene->meshes.find(id);
          if (next != snapshot.scene->meshes.end()) {
            const auto* replacement = next->second.geometry.get();
            if (uploaded.contains(replacement) &&
                replaced.insert(replacement).second) {
              update.geometry_replacements.push_back({geometry, replacement});
            }
          }
        }
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
  std::optional<SceneEnvironmentMap> environment_map;
  if (snapshot.scene && snapshot.scene->environment_map) {
    const EnvironmentMap& map = *snapshot.scene->environment_map;
    const auto texture = snapshot.scene->textures.find(map.texture);
    if (texture != snapshot.scene->textures.end() &&
        map.scale != std::array<float, 3>{}) {
      environment_map = SceneEnvironmentMap{
          texture->second.get(), map.scale, map.world_from_environment};
    }
  }
  if (environment != environment_ || environment_map != environment_map_) {
    update.environment_changed = true;
    update.environment = environment;
    update.environment_map = environment_map;
    environment_ = environment;
    environment_map_ = environment_map;
  }
  std::vector<Light> lights;
  if (snapshot.scene) {
    for (const auto& [id, light] : snapshot.scene->lights) {
      (void)id;
      lights.push_back(light);
    }
  }
  if (lights != lights_) {
    update.lights_changed = true;
    update.lights = lights;
    lights_ = std::move(lights);
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
  environment_map_.reset();
  lights_.clear();
  reset_ = true;
}

} // namespace Lotus
