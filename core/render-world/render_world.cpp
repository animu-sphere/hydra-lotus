// SPDX-License-Identifier: Apache-2.0
#include <lotus/render_world.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Lotus {

namespace {

bool Finite(const Matrix4& matrix) {
  return std::all_of(matrix.begin(), matrix.end(),
      [](float value) { return std::isfinite(value); });
}

void ValidateInstance(const MeshInstance& instance) {
  if (!Finite(instance.world_from_object)) {
    throw std::invalid_argument("mesh transform must be finite");
  }
  if (instance.instancer_transforms &&
      !std::all_of(instance.instancer_transforms->begin(),
          instance.instancer_transforms->end(),
          [](const Matrix4& matrix) { return Finite(matrix); })) {
    throw std::invalid_argument("instancer transforms must be finite");
  }
}

bool Finite(const std::array<float, 3>& values) {
  return std::all_of(values.begin(), values.end(),
      [](float value) { return std::isfinite(value); });
}

bool UnitInterval(float value) {
  return value >= 0.0F && value <= 1.0F;
}

void ValidateMaterial(const Material& material) {
  if (!Finite(material.base_color) ||
      !std::all_of(material.base_color.begin(), material.base_color.end(),
          UnitInterval)) {
    throw std::invalid_argument("base colour components must be in [0, 1]");
  }
  if (!UnitInterval(material.roughness) || !UnitInterval(material.metallic)) {
    throw std::invalid_argument("roughness and metallic must be in [0, 1]");
  }
  if (!Finite(material.emission) ||
      !std::all_of(material.emission.begin(), material.emission.end(),
          [](float value) { return value >= 0.0F; })) {
    throw std::invalid_argument("emission must be finite and non-negative");
  }
}

void ValidateGeometry(const MeshGeometry& geometry) {
  if (geometry.triangles.size() != geometry.source_faces.size()) {
    throw std::invalid_argument("each triangle needs a source face");
  }
  for (const auto& position : geometry.positions) {
    for (float value : position) {
      if (!std::isfinite(value)) {
        throw std::invalid_argument("mesh positions must be finite");
      }
    }
  }
  for (const auto& triangle : geometry.triangles) {
    for (std::uint32_t index : triangle) {
      if (index >= geometry.positions.size()) {
        throw std::invalid_argument("mesh triangle index is out of range");
      }
    }
  }
  if (!geometry.normals.empty() &&
      geometry.normals.size() != 3 * geometry.triangles.size()) {
    throw std::invalid_argument("mesh normals need one per triangle corner");
  }
  if (!std::all_of(geometry.normals.begin(), geometry.normals.end(),
          [](const std::array<float, 3>& normal) { return Finite(normal); })) {
    throw std::invalid_argument("mesh normals must be finite");
  }
}

} // namespace

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

std::vector<Matrix4> PlacementTransforms(const MeshInstance& instance) {
  if (!instance.instancer_transforms) {
    return {instance.world_from_object};
  }
  std::vector<Matrix4> placements;
  placements.reserve(instance.instancer_transforms->size());
  for (const Matrix4& transform : *instance.instancer_transforms) {
    placements.push_back(Multiply(transform, instance.world_from_object));
  }
  return placements;
}

void RenderWorld::MakeSceneWritable() {
  if (scene_.use_count() != 1) {
    // Scene records are copied, while immutable geometry buffers are shared.
    scene_ = std::make_shared<LotusScene>(*scene_);
  }
  dirty_ = true;
  scene_dirty_ = true;
}

void RenderWorld::SetMesh(const std::string& id, MeshGeometry geometry,
    const MeshInstance& instance) {
  if (id.empty()) {
    throw std::invalid_argument("mesh identifier must not be empty");
  }
  ValidateGeometry(geometry);
  ValidateInstance(instance);
  const auto found = scene_->meshes.find(id);
  if (found != scene_->meshes.end() && *found->second.geometry == geometry) {
    SetMeshInstance(id, instance);
    return;
  }
  auto owned = std::make_shared<const MeshGeometry>(std::move(geometry));
  MakeSceneWritable();
  SceneMesh& mesh = scene_->meshes[id];
  mesh.geometry = std::move(owned);
  mesh.instance = instance;
}

void RenderWorld::SetMeshInstance(const std::string& id,
    const MeshInstance& instance) {
  ValidateInstance(instance);
  const auto found = scene_->meshes.find(id);
  if (found == scene_->meshes.end()) {
    throw std::invalid_argument("mesh instance needs existing geometry");
  }
  if (found->second.instance != instance) {
    MakeSceneWritable();
    scene_->meshes.at(id).instance = instance;
  }
}

void RenderWorld::BindMaterial(const std::string& mesh_id,
    const std::string& material_id) {
  const auto found = scene_->meshes.find(mesh_id);
  if (found == scene_->meshes.end()) {
    throw std::invalid_argument("material binding needs existing geometry");
  }
  if (found->second.material != material_id) {
    MakeSceneWritable();
    scene_->meshes.at(mesh_id).material = material_id;
  }
}

void RenderWorld::SetMaterial(const std::string& id,
    const Material& material) {
  if (id.empty()) {
    throw std::invalid_argument("material identifier must not be empty");
  }
  ValidateMaterial(material);
  const auto found = scene_->materials.find(id);
  if (found == scene_->materials.end() || found->second != material) {
    MakeSceneWritable();
    scene_->materials[id] = material;
  }
}

void RenderWorld::RemoveMaterial(const std::string& id) {
  if (scene_->materials.contains(id)) {
    MakeSceneWritable();
    scene_->materials.erase(id);
  }
}

void RenderWorld::SetEnvironment(const std::array<float, 3>& radiance) {
  if (!Finite(radiance) ||
      !std::all_of(radiance.begin(), radiance.end(),
          [](float value) { return value >= 0.0F; })) {
    throw std::invalid_argument(
        "environment radiance must be finite and non-negative");
  }
  if (scene_->environment != radiance) {
    MakeSceneWritable();
    scene_->environment = radiance;
  }
}

void RenderWorld::RemoveMesh(const std::string& id) {
  if (scene_->meshes.contains(id)) {
    MakeSceneWritable();
    scene_->meshes.erase(id);
  }
}

void RenderWorld::SetTriangleCount(std::uint32_t triangle_count) {
  if (triangle_count_ != triangle_count) {
    triangle_count_ = triangle_count;
    dirty_ = true;
  }
}

void RenderWorld::SetCamera(const Camera& camera) {
  if (camera_ != camera) {
    camera_ = camera;
    dirty_ = true;
  }
}

void RenderWorld::MarkChanged() {
  dirty_ = true;
}

void RenderWorld::SetBootstrapTriangle() {
  SetTriangleCount(1);
}

FrameSnapshot RenderWorld::Commit() {
  if (scene_dirty_) {
    std::uint64_t count = 0;
    for (const auto& [id, mesh] : scene_->meshes) {
      (void)id;
      if (mesh.instance.visible) {
        const std::uint64_t placements =
            mesh.instance.instancer_transforms
                ? mesh.instance.instancer_transforms->size()
                : 1;
        count += mesh.geometry->triangles.size() * placements;
      }
    }
    if (count > std::numeric_limits<std::uint32_t>::max()) {
      throw std::overflow_error("scene triangle count exceeds uint32");
    }
    scene_triangle_count_ = static_cast<std::uint32_t>(count);
    scene_dirty_ = false;
  }
  const std::uint64_t count =
      static_cast<std::uint64_t>(triangle_count_) + scene_triangle_count_;
  if (count > std::numeric_limits<std::uint32_t>::max()) {
    throw std::overflow_error("frame triangle count exceeds uint32");
  }
  if (dirty_) {
    ++revision_;
    dirty_ = false;
  }
  return FrameSnapshot{revision_, static_cast<std::uint32_t>(count), camera_, scene_};
}

} // namespace Lotus
