// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <lotus/material.hpp>

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
  // Authored object-space shading normals, one per triangle corner: corner k
  // of triangle t is normals[3 * t + k]. Empty means none, and the surface
  // shades with its geometric normal. They need not be unit length; a zero
  // normal shades with the geometric normal too.
  std::vector<std::array<float, 3>> normals;
  // Named texture-coordinate sets, one (s, t) per triangle corner, laid out
  // as `normals`. A material's texture inputs read the set it names.
  std::map<std::string, std::vector<std::array<float, 2>>> texcoords;

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

struct SceneMesh {
  std::shared_ptr<const MeshGeometry> geometry;
  MeshInstance instance;
  // The key of the material in LotusScene::materials that the mesh is bound
  // to. Empty, or a key with no material, means the default Material.
  std::string material;
};

struct LotusScene {
  // Stable host-supplied identifiers, in deterministic order.
  std::map<std::string, SceneMesh> meshes;
  // The material IR, keyed by stable host-supplied identifiers that meshes
  // bind to, in deterministic order.
  std::map<std::string, Material> materials;
  // The textures that materials' texture inputs name, keyed by stable
  // host-supplied identifiers, in deterministic order. Texels are shared
  // with retained snapshots.
  std::map<std::string, std::shared_ptr<const Texture>> textures;
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
  // A mesh keeps its material binding when its geometry or instance is
  // replaced. Any key may be bound, including one without a material yet.
  void BindMaterial(const std::string& mesh_id, const std::string& material_id);
  void RemoveMesh(const std::string& id);
  // Inserts or replaces a material; the meshes bound to its key follow it.
  void SetMaterial(const std::string& id, const Material& material);
  // The meshes bound to a removed material keep their binding and use the
  // default Material until it returns.
  void RemoveMaterial(const std::string& id);
  // Inserts or replaces a texture; the texture inputs that name its key read
  // it. Removing it makes them return their fallback until it returns.
  void SetTexture(const std::string& id, Texture texture);
  void RemoveTexture(const std::string& id);
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
