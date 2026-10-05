// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
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

// left * right: right's transform is applied first.
[[nodiscard]] Matrix4 Multiply(const Matrix4& left, const Matrix4& right);

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

// Where a mesh is placed. Without `instancer_transforms` it is one ordinary
// placement at `world_from_object`. With them it is an instancer prototype,
// placed once per entry, in order, at entry * world_from_object; an empty
// list places it nowhere.
struct MeshInstance {
  Matrix4 world_from_object = IdentityMatrix();
  bool visible = true;
  std::optional<std::vector<Matrix4>> instancer_transforms;

  bool operator==(const MeshInstance&) const = default;
};

// The world_from_object of each of an instance's placements, in order,
// whether or not it is visible.
[[nodiscard]] std::vector<Matrix4> PlacementTransforms(
    const MeshInstance& instance);

// How a mesh's surface scatters and emits light: the reference path tracer's
// parameters until the material IR (Renderer Phase 1.5) replaces them. Colours
// are linear RGB. The defaults are UsdPreviewSurface's.
struct SurfaceMaterial {
  // Lambert albedo, and the GGX reflectance at normal incidence (F0) where
  // the surface is metallic. Each component in [0, 1].
  std::array<float, 3> base_color{0.18F, 0.18F, 0.18F};
  // GGX roughness in [0, 1]; the microfacet alpha is its square.
  float roughness = 0.5F;
  // The GGX metal's share of the reflectance in [0, 1]; the rest is Lambert.
  float metallic = 0.0F;
  // Emitted radiance, non-negative, from both sides of the surface.
  std::array<float, 3> emission{0.0F, 0.0F, 0.0F};

  bool operator==(const SurfaceMaterial&) const = default;
};

struct SceneMesh {
  std::shared_ptr<const MeshGeometry> geometry;
  MeshInstance instance;
  SurfaceMaterial material;
};

struct LotusScene {
  // Stable host-supplied identifiers, in deterministic order.
  std::map<std::string, SceneMesh> meshes;
  // Constant environment radiance, linear RGB, arriving from every direction
  // a path escapes in. Camera rays that miss do not see it.
  std::array<float, 3> environment{0.0F, 0.0F, 0.0F};
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
  // A mesh keeps its material when its geometry or instance is replaced.
  void SetMeshMaterial(const std::string& id, const SurfaceMaterial& material);
  void RemoveMesh(const std::string& id);
  void SetEnvironment(const std::array<float, 3>& radiance);
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
