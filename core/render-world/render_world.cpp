// SPDX-License-Identifier: Apache-2.0
#include <lotus/render_world.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Lotus {

namespace {

void ValidateInstance(const MeshInstance& instance) {
  if (!std::all_of(instance.world_from_object.begin(),
          instance.world_from_object.end(),
          [](float value) { return std::isfinite(value); })) {
    throw std::invalid_argument("mesh transform must be finite");
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
}

} // namespace

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
  scene_->meshes[id] = SceneMesh{std::move(owned), instance};
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
        count += mesh.geometry->triangles.size();
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
