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
  // Object-space shading normals, authored or computed by the host, one per
  // triangle corner: normals[3 * t + k] is corner k of triangle t.
  // Empty means none, and the surface
  // shades with its geometric normal. They need not be unit length; a zero
  // normal shades with the geometric normal too.
  std::vector<std::array<float, 3>> normals;
  // Named texture-coordinate sets, one (s, t) per triangle corner, laid out
  // as `normals`. A material's texture inputs read the set it names.
  std::map<std::string, std::vector<std::array<float, 2>>> texcoords;
  // Named linear RGB sets, finite but not clamped, for the materials that
  // read them: one per triangle corner, laid out as `normals`, or a single
  // value for the whole mesh.
  std::map<std::string, std::vector<std::array<float, 3>>> colors;

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

// The kinds of analytic light: UsdLux's SphereLight, RectLight, DiskLight
// and DistantLight.
enum class LightShape : std::uint32_t {
  Sphere,
  Rect,
  Disk,
  Distant,
};

// A light with a shape of its own rather than an emissive mesh. It only
// emits: it neither reflects nor blocks light, and camera rays do not see
// it, as they do not see the environment.
struct Light {
  LightShape shape = LightShape::Sphere;
  // The light's frame. A sphere is centred on its origin; a rect or disk is
  // centred in its XY plane and emits from its -Z side; a distant light
  // shines along its -Z axis from beyond the scene. A sphere's radius scales
  // by the cube root of the frame's absolute determinant (its scale when
  // uniform), so it stays a sphere; a rect or disk takes the full transform.
  Matrix4 world_from_light = IdentityMatrix();
  // Emitted radiance, linear RGB, non-negative: UsdLux's
  // color * intensity * 2^exposure. A distant light of angle 0 has no
  // radiance; this is then its irradiance at normal incidence.
  std::array<float, 3> radiance{1.0F, 1.0F, 1.0F};
  // UsdLux normalize: divides the radiance by the light's world-space area,
  // or for a distant light of positive angle by its UsdLux size factor, so
  // that its power does not change with its size.
  bool normalize = false;
  // Non-negative sizes: a sphere's or disk's radius, a rect's extent along
  // its X and Y axes, and a distant light's angular diameter in degrees,
  // 0 for parallel light; angles above 360 count as 360.
  float radius = 0.5F;
  float width = 1.0F;
  float height = 1.0F;
  float angle = 0.53F;

  bool operator==(const Light&) const = default;
};

// A latitude-longitude environment image: UsdLux DomeLight's texture. In
// its frame it follows the OpenEXR convention: image column 0 is longitude
// +pi and the last column -pi, the top row latitude +pi/2 (+Y) and the
// bottom row -pi/2 (-Y); latitude 0 is +Z at longitude 0 and +X at
// longitude pi/2. Its radiance arrives from every direction a path escapes
// in, like the constant environment's, which it adds to.
struct EnvironmentMap {
  // The key in LotusScene::textures of the image. A key with no texture
  // contributes nothing.
  std::string texture;
  // Multiplies the texels' RGB, linear and non-negative: UsdLux's
  // color * intensity * 2^exposure.
  std::array<float, 3> scale{1.0F, 1.0F, 1.0F};
  // The image's frame. Only its orientation counts: see EnvironmentFrame.
  Matrix4 world_from_environment = IdentityMatrix();

  bool operator==(const EnvironmentMap&) const = default;
};

// The world-space unit X, Y and Z axes of an environment map's frame: the
// transform's linear part's columns orthonormalized in that order
// (Gram-Schmidt), so that a scale or shear does not distort the image and a
// mirroring transform mirrors it. Nothing when they are not finite or not
// independent.
[[nodiscard]] std::optional<std::array<std::array<double, 3>, 3>>
EnvironmentFrame(const Matrix4& world_from_environment);

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
  // An environment image, whose radiance adds to `environment`'s.
  std::optional<EnvironmentMap> environment_map;
  // Analytic lights, keyed by stable host-supplied identifiers, in
  // deterministic order.
  std::map<std::string, Light> lights;
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
  // Sets or, with nothing, removes the environment image. Its key may name
  // a texture that does not exist yet.
  void SetEnvironmentMap(const std::optional<EnvironmentMap>& map);
  // Inserts or replaces an analytic light.
  void SetLight(const std::string& id, const Light& light);
  void RemoveLight(const std::string& id);
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
