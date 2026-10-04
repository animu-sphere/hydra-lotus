// SPDX-License-Identifier: Apache-2.0
#include "adapter.hpp"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/imaging/hd/meshTopology.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/rprim.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/tokens.h>

#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

class MeshScene final : public HdSceneDelegate {
public:
  explicit MeshScene(HdRenderIndex* index)
      : HdSceneDelegate(index, SdfPath("/scene")) {}

  VtVec3fArray points{GfVec3f(0, 0, 0), GfVec3f(1, 0, 0),
      GfVec3f(1, 1, 0), GfVec3f(0, 1, 0), GfVec3f(0, 0, 1)};
  VtIntArray counts{4, 3};
  VtIntArray indices{0, 1, 2, 3, 0, 1, 4};
  VtIntArray holes{1};
  TfToken orientation = HdTokens->rightHanded;
  GfMatrix4d transform{1.0};
  bool visible = true;
  int point_reads = 0;
  int topology_reads = 0;
  int transform_reads = 0;
  int visibility_reads = 0;

  bool GetVisible(const SdfPath&) override {
    ++visibility_reads;
    return visible;
  }
  GfMatrix4d GetTransform(const SdfPath&) override {
    ++transform_reads;
    return transform;
  }
  HdMeshTopology GetMeshTopology(const SdfPath&) override {
    ++topology_reads;
    return HdMeshTopology(TfToken("none"), orientation, counts, indices, holes);
  }
  VtValue Get(const SdfPath&, const TfToken& key) override {
    if (key == HdTokens->points) {
      ++point_reads;
      return VtValue(points);
    }
    return {};
  }
};

} // namespace

int main() try {
  HdLotusRenderDelegate delegate;
  std::unique_ptr<HdRenderIndex> index(HdRenderIndex::New(&delegate, {}));
  Check(index != nullptr, "could not create render index");
  MeshScene scene(index.get());
  const SdfPath id("/scene/mesh");
  std::unique_ptr<HdRprim> mesh(delegate.CreateRprim(HdPrimTypeTokens->mesh, id));
  Check(mesh != nullptr, "could not create mesh");
  auto sync = [&](HdDirtyBits bits) {
    mesh->Sync(&scene, nullptr, &bits, HdReprTokens->smoothHull);
    Check(bits == HdChangeTracker::Clean, "sync did not consume dirty bits");
    return delegate.GetFrameSnapshot();
  };
  const auto first = sync(mesh->GetInitialDirtyBitsMask());
  const auto geometry = first.scene->meshes.at(id.GetString()).geometry;
  Check(first.triangle_count == 2 && geometry->positions.size() == 5 &&
      geometry->triangles == std::vector<std::array<std::uint32_t, 3>>{
          {0, 1, 2}, {0, 2, 3}} &&
      geometry->source_faces == std::vector<std::uint32_t>{0, 0},
      "quad triangulation or hole removal failed");

  const auto clean = sync(HdChangeTracker::Clean);
  Check(clean.revision == first.revision && clean.scene == first.scene &&
      scene.point_reads == 1 && scene.topology_reads == 1,
      "clean mesh fetched or changed geometry");
  const auto unchanged = sync(mesh->GetInitialDirtyBitsMask());
  Check(unchanged.revision == first.revision,
      "identical dirty input invalidated the scene");

  // Non-symmetric rotation and translation expose matrix transposition bugs.
  scene.transform.SetRotate(GfRotation(GfVec3d(0, 0, 1), 90));
  scene.transform.SetTranslateOnly(GfVec3d(2, 3, 4));
  const auto moved = sync(HdChangeTracker::DirtyTransform);
  const auto& matrix = moved.scene->meshes.at(id.GetString()).instance.world_from_object;
  Check(matrix[12] == 2 && matrix[13] == 3 && matrix[14] == 4 &&
      std::abs(matrix[0]) < 1e-5F && std::abs(matrix[1] - 1) < 1e-5F &&
      std::abs(matrix[4] + 1) < 1e-5F &&
      moved.scene->meshes.at(id.GetString()).geometry == geometry &&
      scene.point_reads == 2 && scene.topology_reads == 2,
      "transform update changed geometry or matrix convention");
  scene.visible = false;
  const auto hidden = sync(HdChangeTracker::DirtyVisibility);
  Check(hidden.triangle_count == 0 && hidden.scene->meshes.size() == 1 &&
      scene.point_reads == 2 && scene.topology_reads == 2 &&
      scene.transform_reads == 3, "visibility update fetched geometry");
  scene.visible = true;
  Check(sync(HdChangeTracker::DirtyVisibility).triangle_count == 2,
      "visibility restoration failed");

  scene.points[0][2] = 5;
  const auto edited = sync(HdChangeTracker::DirtyPoints);
  Check(edited.scene->meshes.at(id.GetString()).geometry->positions[0][2] == 5 &&
      geometry->positions[0][2] == 0 && scene.topology_reads == 2,
      "point update changed old snapshot or fetched topology");
  scene.orientation = HdTokens->leftHanded;
  const auto flipped = sync(HdChangeTracker::DirtyTopology);
  const auto& flipped_geometry = *flipped.scene->meshes.at(id.GetString()).geometry;
  Check(flipped_geometry.triangles.size() == 2, "left-handed quad lost triangles");
  for (const auto& triangle : flipped_geometry.triangles) {
    const auto& a = flipped_geometry.positions[triangle[0]];
    const auto& b = flipped_geometry.positions[triangle[1]];
    const auto& c = flipped_geometry.positions[triangle[2]];
    const float normal_z =
        (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    Check(normal_z < 0, "left-handed winding was not normalized");
  }
  scene.holes.clear();
  const auto filled = sync(HdChangeTracker::DirtyTopology);
  Check(filled.triangle_count == 3 &&
      filled.scene->meshes.at(id.GetString()).geometry->source_faces.back() == 1,
      "topology update did not restore hole face");

  // Malformed edits must remove formerly valid geometry, then recover.
  scene.indices[0] = 99;
  const auto rejected = sync(HdChangeTracker::DirtyTopology);
  Check(rejected.scene->meshes.empty() && rejected.triangle_count == 0,
      "invalid indices retained stale geometry");
  scene.indices[0] = 0;
  Check(sync(HdChangeTracker::DirtyTopology).triangle_count == 3,
      "valid topology did not recover after rejection");
  scene.counts[0] = -1;
  Check(sync(HdChangeTracker::DirtyTopology).scene->meshes.empty(),
      "negative face count was accepted");
  scene.counts[0] = 4;
  scene.points[0][0] = std::numeric_limits<float>::quiet_NaN();
  Check(sync(HdChangeTracker::DirtyPoints).scene->meshes.empty(),
      "non-finite position was accepted");
  scene.points[0][0] = 0;
  const auto recovered = sync(HdChangeTracker::DirtyPoints);
  Check(recovered.triangle_count == 3, "valid points did not recover");
  mesh.reset();
  const auto removed = delegate.GetFrameSnapshot();
  Check(removed.scene->meshes.empty() && removed.triangle_count == 0 &&
      recovered.scene->meshes.size() == 1,
      "mesh destruction did not remove geometry or mutated old snapshot");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
