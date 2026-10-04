// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
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

// One placement of a resident geometry buffer.
struct SceneInstance {
  // A buffer from this or an earlier update's geometry_uploads.
  const MeshGeometry* geometry = nullptr;
  Matrix4 world_from_object = IdentityMatrix();

  bool operator==(const SceneInstance&) const = default;
};

// What a GPU scene changes to match one snapshot's LotusScene (design policy
// section 4.2). A geometry buffer is identified by its address: SceneExtraction
// keeps every resident buffer alive, so an address is not reused for another
// buffer before its release. Geometry without triangles is never resident.
struct SceneUpdate {
  std::uint64_t source_revision = 0;
  // Resident buffers the scene no longer references, in the previous scene's
  // key order. Applied before the uploads.
  std::vector<const MeshGeometry*> geometry_releases;
  // Buffers that become resident, in scene key order. A hidden mesh's
  // geometry is resident too, so a visibility change uploads nothing.
  std::vector<std::shared_ptr<const MeshGeometry>> geometry_uploads;
  // When set, `instances` replaces every instance: the visible meshes with
  // triangles, in scene key order. Otherwise `instances` is empty.
  bool instances_changed = false;
  std::vector<SceneInstance> instances;

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
  std::vector<SceneInstance> instances_;
};

} // namespace Lotus
