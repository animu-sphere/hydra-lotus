// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

#include <lotus/render_world.hpp>

namespace Lotus {

struct DrawSummary {
  // Bootstrap raster pass only: visible scene geometry selects a single
  // scaffold triangle. The scene itself reaches the GPU through SceneUpdate.
  std::uint64_t source_revision = 0;
  std::uint32_t draw_count = 0;
  std::uint32_t triangle_count = 0;
  // camera.projection * camera.view: world space to OpenGL clip space.
  Matrix4 world_to_clip = IdentityMatrix();
};

[[nodiscard]] DrawSummary ExtractDrawSummary(const FrameSnapshot& snapshot);

// SceneInstance::texcoords when the instance's texture lookups read no
// texture-coordinate set of its geometry.
inline constexpr std::uint32_t kNoTexcoords =
    std::numeric_limits<std::uint32_t>::max();

// One placement of a resident geometry buffer.
struct SceneInstance {
  // A buffer from this or an earlier update's geometry_uploads.
  const MeshGeometry* geometry = nullptr;
  Matrix4 world_from_object = IdentityMatrix();
  // The instance's material: its index in the GPU scene's material table.
  std::uint32_t material = 0;
  // The texture-coordinate set its material's lookups read: an index into
  // geometry->texcoords in key order, or kNoTexcoords when the material has
  // no texture input or the geometry no set of the name it reads.
  std::uint32_t texcoords = kNoTexcoords;

  bool operator==(const SceneInstance&) const = default;
};

// One material table entry: the material and, for each of its
// TextureInputs, the resident texture the input's key names. An entry is
// null when the input is a constant or its key names no texture, so that
// the lookup returns its fallback.
struct SceneMaterial {
  Material material;
  std::array<const Texture*, kMaterialTextureInputs> textures{};

  bool operator==(const SceneMaterial&) const = default;
};

// Optional correspondence for a mesh edited under the same stable key.
// Both addresses also occur in this plan's releases and uploads. The backend
// may retain storage and refit a BLAS when the topology is compatible.
struct SceneGeometryReplacement {
  const MeshGeometry* previous = nullptr;
  const MeshGeometry* replacement = nullptr;
  bool operator==(const SceneGeometryReplacement&) const = default;
};

// What a GPU scene changes to match one snapshot's LotusScene (design policy
// section 4.2). A geometry buffer or a texture is identified by its address:
// SceneExtraction keeps every resident one alive, so an address is not
// reused for another before its release. Geometry without triangles is never
// resident; every texture in the scene is.
struct SceneUpdate {
  std::uint64_t source_revision = 0;
  // Resident buffers the scene no longer references, in the previous scene's
  // key order. Applied before the uploads.
  std::vector<const MeshGeometry*> geometry_releases;
  // Buffers that become resident, in scene key order. A hidden mesh's
  // geometry is resident too, so a visibility change uploads nothing.
  std::vector<std::shared_ptr<const MeshGeometry>> geometry_uploads;
  std::vector<SceneGeometryReplacement> geometry_replacements;
  // Textures the scene no longer holds, in the previous scene's key order,
  // and textures that become resident, in key order. Releases are applied
  // before uploads.
  std::vector<const Texture*> texture_releases;
  std::vector<std::shared_ptr<const Texture>> texture_uploads;
  // When set, `instances` replaces every instance: each placement of the
  // visible meshes with triangles, in scene key order and then placement
  // order. Otherwise `instances` is empty.
  bool instances_changed = false;
  std::vector<SceneInstance> instances;
  // When set, `materials` replaces the material table: the default Material
  // at slot 0, then the scene's materials in key order, each with the
  // textures its inputs read. A GPU scene starts with the default alone.
  // Otherwise `materials` is empty.
  bool materials_changed = false;
  std::vector<SceneMaterial> materials;
  // When set, the scene's environment radiance replaces the GPU scene's. A
  // GPU scene starts with a black environment.
  bool environment_changed = false;
  std::array<float, 3> environment{};

  // True when the GPU scene has nothing to do.
  [[nodiscard]] bool Empty() const;
};

// Turns successive snapshots into the update plans of one GPU scene. Every
// plan is applied, in order. When the GPU scene is lost, Reset makes the next
// plan upload everything again.
class SceneExtraction {
public:
  // An unchanged scene pointer yields an empty plan without traversal.
  [[nodiscard]] SceneUpdate Update(const FrameSnapshot& snapshot);
  void Reset();

private:
  std::shared_ptr<const LotusScene> scene_;
  bool reset_ = true;
  std::unordered_map<const MeshGeometry*, std::shared_ptr<const MeshGeometry>>
      resident_;
  std::unordered_map<const Texture*, std::shared_ptr<const Texture>>
      resident_textures_;
  std::vector<SceneInstance> instances_;
  std::vector<SceneMaterial> materials_{SceneMaterial{}};
  std::array<float, 3> environment_{};
};

} // namespace Lotus
