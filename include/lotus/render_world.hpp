// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Lotus {

// A 4x4 matrix in column-major order that multiplies column vectors: element
// (row, column) is at [column * 4 + row].
using Matrix4 = std::array<float, 16>;

[[nodiscard]] constexpr Matrix4 IdentityMatrix() {
  return {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
      0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
}

// The camera as a host supplies it. `view` maps world space to a right-handed
// view space looking down -Z; `projection` maps view space to OpenGL clip
// space (y up, z in [-w, w]). A backend converts to its own clip convention.
struct Camera {
  Matrix4 view = IdentityMatrix();
  Matrix4 projection = IdentityMatrix();

  bool operator==(const Camera&) const = default;
};

// Object-space triangles; source_faces maps each triangle to its coarse face.
struct MeshGeometry {
  std::vector<std::array<float, 3>> positions;
  std::vector<std::array<std::uint32_t, 3>> triangles;
  std::vector<std::uint32_t> source_faces;

  bool operator==(const MeshGeometry&) const = default;
};

// One ordinary mesh placement. Hydra instancer expansion is a later step.
struct MeshInstance {
  Matrix4 world_from_object = IdentityMatrix();
  bool visible = true;

  bool operator==(const MeshInstance&) const = default;
};

struct SceneMesh {
  std::shared_ptr<const MeshGeometry> geometry;
  MeshInstance instance;
};

struct LotusScene {
  // Stable host-supplied identifiers, in deterministic order.
  std::map<std::string, SceneMesh> meshes;
};

struct FrameSnapshot {
  std::uint64_t revision = 0;
  std::uint32_t triangle_count = 0;
  Camera camera;
  std::shared_ptr<const LotusScene> scene;
};

class RenderWorld {
public:
  void SetTriangleCount(std::uint32_t triangle_count);
  void SetCamera(const Camera& camera);
  void MarkChanged();
  void SetBootstrapTriangle();
  // Invalid input throws without changing the world. Geometry is owned here;
  // a retained snapshot remains immutable across edits and removals.
  void SetMesh(const std::string& id, MeshGeometry geometry,
      const MeshInstance& instance);
  void SetMeshInstance(const std::string& id, const MeshInstance& instance);
  void RemoveMesh(const std::string& id);
  [[nodiscard]] FrameSnapshot Commit();

private:
  void MakeSceneWritable();
  std::uint64_t revision_ = 0;
  std::uint32_t triangle_count_ = 0;
  Camera camera_;
  bool dirty_ = false;
  bool scene_dirty_ = false;
  std::uint32_t scene_triangle_count_ = 0;
  std::shared_ptr<LotusScene> scene_ = std::make_shared<LotusScene>();
};

} // namespace Lotus
