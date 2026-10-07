// SPDX-License-Identifier: Apache-2.0
// Syncs a composed USD stage's meshes through UsdImaging's scene indices into
// the Lotus render delegate and compares each triangle corner's normal with
// the one read independently from the stage through UsdGeom, for every
// interpolation, indexed primvars, holes and left-handed faces, then through
// edits that add, change and remove normals. Computed smooth normals are
// checked against a double-precision polygon-corner oracle independent of Hd.
#include "adapter.hpp"

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// A quad and a triangle per mesh, so face-varying and uniform values differ
// between faces; each mesh lies apart from the others.
constexpr const char* kStage = R"usda(#usda 1.0
def Mesh "Vertex"
{
    int[] faceVertexCounts = [4, 3]
    int[] faceVertexIndices = [0, 1, 2, 3, 1, 4, 2]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0), (2, 0.5, 0)]
    normal3f[] normals = [(0, 0, 1), (0.5, 0, 1), (0, 0.5, 1), (-0.5, 0, 1), (0, -0.5, 1)]
}

def Mesh "FaceVarying"
{
    int[] faceVertexCounts = [4, 3]
    int[] faceVertexIndices = [0, 1, 2, 3, 1, 4, 2]
    point3f[] points = [(0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1), (2, 0.5, 1)]
    normal3f[] normals = [(1, 0, 0), (0, 1, 0), (0, 0, 1), (1, 1, 0),
        (0, 1, 1), (1, 0, 1), (1, 1, 1)] (
        interpolation = "faceVarying"
    )
}

def Mesh "Indexed"
{
    int[] faceVertexCounts = [4, 3]
    int[] faceVertexIndices = [0, 1, 2, 3, 1, 4, 2]
    point3f[] points = [(0, 0, 2), (1, 0, 2), (1, 1, 2), (0, 1, 2), (2, 0.5, 2)]
    normal3f[] normals = [(9, 9, 9), (9, 9, 9), (9, 9, 9), (9, 9, 9), (9, 9, 9)]
    normal3f[] primvars:normals = [(0, 0, 1), (0, 1, 0), (1, 0, 0)] (
        interpolation = "faceVarying"
    )
    int[] primvars:normals:indices = [0, 1, 2, 0, 2, 2, 1]
}

def Mesh "Uniform"
{
    int[] faceVertexCounts = [4, 3]
    int[] faceVertexIndices = [0, 1, 2, 3, 1, 4, 2]
    point3f[] points = [(0, 0, 3), (1, 0, 3), (1, 1, 3), (0, 1, 3), (2, 0.5, 3)]
    normal3f[] normals = [(0, 0.25, 1), (0.25, 0, -1)] (
        interpolation = "uniform"
    )
}

def Mesh "Constant"
{
    int[] faceVertexCounts = [4, 3]
    int[] faceVertexIndices = [0, 1, 2, 3, 1, 4, 2]
    point3f[] points = [(0, 0, 4), (1, 0, 4), (1, 1, 4), (0, 1, 4), (2, 0.5, 4)]
    normal3f[] primvars:normals = [(0.5, 0.5, 2)] (
        interpolation = "constant"
    )
}

def Mesh "LeftHandedHole"
{
    uniform token orientation = "leftHanded"
    int[] faceVertexCounts = [4, 3]
    int[] faceVertexIndices = [0, 1, 2, 3, 1, 4, 2]
    int[] holeIndices = [0]
    point3f[] points = [(0, 0, 5), (1, 0, 5), (1, 1, 5), (0, 1, 5), (2, 0.5, 5)]
    normal3f[] normals = [(1, 0, 0), (0, 1, 0), (0, 0, 1), (1, 1, 0),
        (0, 1, 1), (1, 0, 1), (1, 1, 1)] (
        interpolation = "faceVarying"
    )
}

def Mesh "Triangles"
{
    int[] faceVertexCounts = [3, 3]
    int[] faceVertexIndices = [0, 1, 2, 1, 3, 2]
    point3f[] points = [(0, 0, 8), (1, 0, 8), (0, 1, 8), (1, 1, 8)]
    normal3f[] normals = [(1, 0, 0), (0, 1, 0), (0, 0, 1), (1, 1, 0),
        (0, 1, 1), (1, 0, 1)] (
        interpolation = "faceVarying"
    )
}

def Mesh "Mismatched"
{
    int[] faceVertexCounts = [4, 3]
    int[] faceVertexIndices = [0, 1, 2, 3, 1, 4, 2]
    point3f[] points = [(0, 0, 6), (1, 0, 6), (1, 1, 6), (0, 1, 6), (2, 0.5, 6)]
    normal3f[] normals = [(0, 0, 1), (0, 0, 1)]
}

def Mesh "Plain"
{
    int[] faceVertexCounts = [4, 3]
    int[] faceVertexIndices = [0, 1, 2, 3, 1, 4, 2]
    point3f[] points = [(0, 0, 7), (1, 0, 7), (1, 1, 7), (0, 1, 7), (2, 0.5, 7)]
}
)usda";

void Check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

class SyncTask final : public HdTask {
public:
  explicit SyncTask(HdRenderPassSharedPtr pass)
      : HdTask(SdfPath::EmptyPath()), pass_(std::move(pass)) {
  }
  void Sync(HdSceneDelegate*, HdTaskContext*, HdDirtyBits* dirty_bits) override {
    pass_->Sync();
    *dirty_bits = HdChangeTracker::Clean;
  }
  void Prepare(HdTaskContext*, HdRenderIndex*) override {
  }
  void Execute(HdTaskContext*) override {
  }
  const TfTokenVector& GetRenderTags() const override {
    static const TfTokenVector tags{HdRenderTagTokens->geometry};
    return tags;
  }

private:
  HdRenderPassSharedPtr pass_;
};

using Normals = std::vector<std::array<float, 3>>;

constexpr const char* kComputedPaths[] = {"/Smooth", "/Loop", "/Bilinear",
    "/None", "/Left", "/Hole", "/Degenerate"};

void AddComputedMeshes(const UsdStageRefPtr& stage) {
  for (const char* path : kComputedPaths) {
    const UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath(path));
    // A non-planar quad adjoining a tilted triangle. Triangulating before
    // averaging gives different normals, as does averaging unit face normals.
    // The unused trailing point exercises Hd's shorter normal result.
    mesh.CreatePointsAttr().Set(VtVec3fArray{GfVec3f(0, 0, 0),
        GfVec3f(2, 0, 0), GfVec3f(2, 1, 1), GfVec3f(0, 1, 0),
        GfVec3f(3, 0, 2), GfVec3f(90, 90, 90)});
    mesh.CreateFaceVertexCountsAttr().Set(VtIntArray{4, 3});
    mesh.CreateFaceVertexIndicesAttr().Set(VtIntArray{0, 1, 2, 3, 1, 4, 2});
    const std::string name(path);
    if (name == "/Smooth" || name == "/Left" || name == "/Hole") {
      mesh.CreateSubdivisionSchemeAttr().Set(UsdGeomTokens->catmullClark);
    } else if (name == "/Loop") {
      mesh.CreateSubdivisionSchemeAttr().Set(UsdGeomTokens->loop);
      mesh.GetFaceVertexCountsAttr().Set(VtIntArray{3, 3});
      mesh.GetFaceVertexIndicesAttr().Set(VtIntArray{0, 1, 2, 1, 4, 2});
    } else if (name == "/None") {
      mesh.CreateSubdivisionSchemeAttr().Set(UsdGeomTokens->none);
    } else if (name == "/Bilinear") {
      mesh.CreateSubdivisionSchemeAttr().Set(UsdGeomTokens->bilinear);
    }
    if (name == "/Left") {
      mesh.CreateOrientationAttr().Set(UsdGeomTokens->leftHanded);
    } else if (name == "/Hole") {
      mesh.CreateHoleIndicesAttr().Set(VtIntArray{1});
    } else if (name == "/Degenerate") {
      mesh.GetFaceVertexCountsAttr().Set(VtIntArray{3});
      mesh.GetFaceVertexIndicesAttr().Set(VtIntArray{0, 0, 0});
    }
  }
}

Normals ComputedExpected(const UsdGeomMesh& mesh,
    const Lotus::MeshGeometry& geometry) {
  TfToken scheme;
  mesh.GetSubdivisionSchemeAttr().Get(&scheme);
  if (scheme == UsdGeomTokens->none || scheme == UsdGeomTokens->bilinear) {
    return {};
  }
  VtVec3fArray points;
  VtIntArray counts, indices;
  TfToken orientation;
  mesh.GetPointsAttr().Get(&points);
  mesh.GetFaceVertexCountsAttr().Get(&counts);
  mesh.GetFaceVertexIndicesAttr().Get(&indices);
  mesh.GetOrientationAttr().Get(&orientation);
  std::vector<GfVec3d> sums(points.size(), GfVec3d(0));
  std::size_t offset = 0;
  // All polygon corners contribute, including holes, as in Storm's coarse
  // adjacency. Use double precision and no Hd normal or adjacency utilities.
  for (const int count : counts) {
    for (int corner = 0; corner < count; ++corner) {
      const int vertex = indices[offset + corner];
      const GfVec3d centre(points[vertex]);
      const GfVec3d before(points[indices[offset + (corner + count - 1) % count]]);
      const GfVec3d after(points[indices[offset + (corner + 1) % count]]);
      const GfVec3d contribution = GfCross(after - centre, before - centre);
      sums[vertex] += orientation == UsdGeomTokens->leftHanded
                          ? -contribution : contribution;
    }
    offset += static_cast<std::size_t>(count);
  }
  for (GfVec3d& sum : sums) {
    const double length = sum.GetLength();
    if (length > 0) sum /= length;
  }
  Normals corners;
  for (const auto& triangle : geometry.triangles) {
    for (const auto vertex : triangle) {
      const auto& value = sums[vertex];
      corners.push_back({static_cast<float>(value[0]),
          static_cast<float>(value[1]), static_cast<float>(value[2])});
    }
  }
  return corners;
}

bool Near(const Normals& a, const Normals& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    for (std::size_t c = 0; c < 3; ++c) {
      if (!std::isfinite(a[i][c]) || std::abs(a[i][c] - b[i][c]) > 2e-6F)
        return false;
    }
  }
  return true;
}

// Each triangle corner's normal as UsdGeom reads the stage: `primvars:normals`
// over `normals`, flattened, and looked up by the corner's point, its face
// or its face vertex. The triangles are the delegate's; a corner's face
// vertex is the one in its source face that names its point.
Normals Expected(const UsdStageRefPtr& stage, const std::string& path,
    const Lotus::MeshGeometry& geometry) {
  const UsdGeomMesh mesh(stage->GetPrimAtPath(SdfPath(path)));
  VtVec3fArray values;
  TfToken interpolation;
  const UsdGeomPrimvar primvar =
      UsdGeomPrimvarsAPI(mesh.GetPrim()).GetPrimvar(UsdGeomTokens->normals);
  if (primvar && primvar.HasAuthoredValue()) {
    Check(primvar.ComputeFlattened(&values), "could not flatten " + path);
    interpolation = primvar.GetInterpolation();
  } else if (mesh.GetNormalsAttr().HasAuthoredValue()) {
    mesh.GetNormalsAttr().Get(&values);
    interpolation = mesh.GetNormalsInterpolation();
  } else {
    return ComputedExpected(mesh, geometry);
  }
  VtIntArray counts;
  VtIntArray indices;
  mesh.GetFaceVertexCountsAttr().Get(&counts);
  mesh.GetFaceVertexIndicesAttr().Get(&indices);
  std::vector<int> face_offsets;
  int offset = 0;
  for (const int count : counts) {
    face_offsets.push_back(offset);
    offset += count;
  }
  Normals corners;
  for (std::size_t triangle = 0; triangle < geometry.triangles.size(); ++triangle) {
    const auto face = static_cast<int>(geometry.source_faces[triangle]);
    for (const std::uint32_t point : geometry.triangles[triangle]) {
      std::size_t index = 0;
      if (interpolation == UsdGeomTokens->constant) {
        index = 0;
      } else if (interpolation == UsdGeomTokens->uniform) {
        index = static_cast<std::size_t>(face);
      } else if (interpolation == UsdGeomTokens->vertex ||
                 interpolation == UsdGeomTokens->varying) {
        index = point;
      } else {
        int corner = 0;
        while (indices[face_offsets[face] + corner] != static_cast<int>(point))
          ++corner;
        index = static_cast<std::size_t>(face_offsets[face] + corner);
      }
      const GfVec3f& value = values[index];
      corners.push_back({value[0], value[1], value[2]});
    }
  }
  return corners;
}

void Compare(const std::string& step, const Lotus::FrameSnapshot& snapshot,
    const UsdStageRefPtr& stage) {
  for (const char* path : {"/Vertex", "/FaceVarying", "/Indexed", "/Uniform",
           "/Constant", "/LeftHandedHole", "/Triangles", "/Plain"}) {
    const auto found = snapshot.scene->meshes.find(path);
    Check(found != snapshot.scene->meshes.end(),
        step + ": the delegate has no mesh " + path);
    const Lotus::MeshGeometry& geometry = *found->second.geometry;
    Check(Near(geometry.normals, Expected(stage, path, geometry)),
        step + ": the corner normals of " + path + " differ from the stage's");
  }
  for (const char* path : kComputedPaths) {
    const auto found = snapshot.scene->meshes.find(path);
    Check(found != snapshot.scene->meshes.end(),
        step + ": the delegate has no mesh " + path);
    const auto& geometry = *found->second.geometry;
    Check(Near(geometry.normals, Expected(stage, path, geometry)),
        step + ": computed normals of " + path + " differ from the oracle");
  }
}

} // namespace

int main() try {
  const SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
  Check(layer->ImportFromString(kStage), "could not parse the test stage");
  const UsdStageRefPtr stage = UsdStage::Open(layer);
  Check(stage != nullptr, "could not open the test stage");
  AddComputedMeshes(stage);

  HdLotusRenderDelegate delegate;
  std::unique_ptr<HdRenderIndex> index(HdRenderIndex::New(&delegate, {}));
  Check(index != nullptr, "could not create the render index");
  UsdImagingCreateSceneIndicesInfo info;
  info.stage = stage;
  const UsdImagingSceneIndices scene_indices = UsdImagingCreateSceneIndices(info);
  scene_indices.stageSceneIndex->SetTime(UsdTimeCode::Default());
  index->InsertSceneIndex(scene_indices.finalSceneIndex, SdfPath::AbsoluteRootPath());
  HdTaskSharedPtrVector tasks{std::make_shared<SyncTask>(delegate.CreateRenderPass(
      index.get(), HdRprimCollection(HdTokens->geometry,
                       HdReprSelector(HdReprTokens->smoothHull))))};
  HdEngine engine;
  const auto sync = [&] {
    scene_indices.stageSceneIndex->ApplyPendingUpdates();
    engine.Execute(index.get(), &tasks);
    return delegate.GetFrameSnapshot();
  };

  const Lotus::FrameSnapshot first = sync();
  Compare("first sync", first, stage);
  const auto& meshes = first.scene->meshes;
  // The cases the expectation alone would not notice going wrong.
  Check(meshes.at("/Vertex").geometry->normals.size() == 9 &&
            meshes.at("/LeftHandedHole").geometry->triangles.size() == 1 &&
            meshes.at("/LeftHandedHole").geometry->normals.size() == 3 &&
            meshes.at("/Indexed").geometry->normals[0] ==
                std::array<float, 3>{0, 0, 1} &&
            meshes.at("/Triangles").geometry->normals.size() == 6 &&
            meshes.at("/Plain").geometry->normals.size() == 9,
      "the stage's normals were not all read, or the primvar did not win");
  // Normals that do not match their interpolation are ignored; this smooth
  // mesh gets computed normals instead and stays in the scene.
  Check(Near(meshes.at("/Mismatched").geometry->normals,
            ComputedExpected(UsdGeomMesh(stage->GetPrimAtPath(SdfPath("/Mismatched"))),
                *meshes.at("/Mismatched").geometry)) &&
            meshes.at("/Mismatched").geometry->triangles.size() == 3,
      "mismatched normals did not fall back to computed normals");
  Check(meshes.at("/Smooth").geometry->normals.size() == 9 &&
            meshes.at("/None").geometry->normals.empty() &&
            meshes.at("/Bilinear").geometry->normals.empty() &&
            meshes.at("/Hole").geometry->triangles.size() == 2 &&
            meshes.at("/Degenerate").geometry->normals ==
                Normals(3, std::array<float, 3>{0, 0, 0}),
      "smooth/flat scheme, holes or degenerate normals were mishandled");

  // A value edit replaces the geometry of that mesh alone.
  const UsdGeomMesh vertex(stage->GetPrimAtPath(SdfPath("/Vertex")));
  vertex.GetNormalsAttr().Set(VtVec3fArray{GfVec3f(1, 0, 0), GfVec3f(0, 1, 0),
      GfVec3f(0, 0, 1), GfVec3f(1, 1, 0), GfVec3f(0, 1, 1)});
  const Lotus::FrameSnapshot edited = sync();
  Compare("normal edit", edited, stage);
  for (const auto& [id, mesh] : edited.scene->meshes) {
    Check((mesh.geometry == meshes.at(id).geometry) == (id != "/Vertex"),
        "a normal edit of /Vertex replaced the geometry of " + id);
  }

  // Adding a primvar, changing an interpolation, and removing normals.
  const UsdGeomMesh plain(stage->GetPrimAtPath(SdfPath("/Plain")));
  UsdGeomPrimvarsAPI(plain.GetPrim())
      .CreatePrimvar(UsdGeomTokens->normals, SdfValueTypeNames->Normal3fArray,
          UsdGeomTokens->constant)
      .Set(VtVec3fArray{GfVec3f(0, 0, -1)});
  UsdGeomMesh uniform(stage->GetPrimAtPath(SdfPath("/Uniform")));
  uniform.GetNormalsAttr().Set(VtVec3fArray{GfVec3f(1, 0, 0),
      GfVec3f(0, 1, 0), GfVec3f(0, 0, 1), GfVec3f(1, 1, 1), GfVec3f(0, 0, 2)});
  uniform.SetNormalsInterpolation(UsdGeomTokens->vertex);
  vertex.GetPrim().RemoveProperty(TfToken("normals"));
  const Lotus::FrameSnapshot changed = sync();
  Compare("added, reinterpolated and removed normals", changed, stage);
  Check(!changed.scene->meshes.at("/Plain").geometry->normals.empty() &&
            changed.scene->meshes.at("/Vertex").geometry->normals.size() == 9,
      "an added primvar or a removed attribute was not synced");

  const UsdGeomMesh smooth(stage->GetPrimAtPath(SdfPath("/Smooth")));
  VtVec3fArray points;
  smooth.GetPointsAttr().Get(&points);
  points[2][2] = 2;
  smooth.GetPointsAttr().Set(points);
  const auto deformed = sync();
  Compare("point edit", deformed, stage);
  Check(deformed.scene->meshes.at("/Smooth").geometry !=
            changed.scene->meshes.at("/Smooth").geometry &&
            !Near(deformed.scene->meshes.at("/Smooth").geometry->normals,
                changed.scene->meshes.at("/Smooth").geometry->normals),
      "a point edit kept stale computed normals");
  for (const auto& [id, mesh] : deformed.scene->meshes) {
    Check(id == "/Smooth" || mesh.geometry == changed.scene->meshes.at(id).geometry,
        "a point edit replaced unrelated geometry " + id);
  }

  smooth.GetFaceVertexCountsAttr().Set(VtIntArray{3, 3});
  smooth.GetFaceVertexIndicesAttr().Set(VtIntArray{0, 1, 2, 1, 4, 2});
  const auto retriangulated = sync();
  Compare("topology edit", retriangulated, stage);
  Check(!Near(retriangulated.scene->meshes.at("/Smooth").geometry->normals,
            deformed.scene->meshes.at("/Smooth").geometry->normals),
      "a topology edit kept stale computed normals");

  smooth.GetSubdivisionSchemeAttr().Set(UsdGeomTokens->none);
  const auto flat = sync();
  Compare("smooth to flat", flat, stage);
  Check(flat.scene->meshes.at("/Smooth").geometry->normals.empty(),
      "a scheme edit kept smooth normals on a flat mesh");
  smooth.GetSubdivisionSchemeAttr().Set(UsdGeomTokens->catmullClark);
  smooth.CreateNormalsAttr().Set(VtVec3fArray(6, GfVec3f(1, 0, 0)));
  const auto authored = sync();
  Compare("authored replaces computed", authored, stage);
  Check(authored.scene->meshes.at("/Smooth").geometry->normals ==
            Normals(6, std::array<float, 3>{1, 0, 0}),
      "computed normals overrode authored normals");
  smooth.GetPrim().RemoveProperty(TfToken("normals"));
  const auto restored = sync();
  Compare("computed restored", restored, stage);
  Check(Near(restored.scene->meshes.at("/Smooth").geometry->normals,
            retriangulated.scene->meshes.at("/Smooth").geometry->normals),
      "removing authored normals did not restore computed normals");

  smooth.CreateOrientationAttr().Set(UsdGeomTokens->leftHanded);
  Compare("orientation edit", sync(), stage);
  smooth.CreateHoleIndicesAttr().Set(VtIntArray{1});
  const auto holed = sync();
  Compare("hole edit", holed, stage);
  Check(holed.scene->meshes.at("/Smooth").geometry->triangles.size() == 1,
      "a hole edit did not remove its rendered face");

  // Authored normals also win on a faceted subdivision scheme.
  const UsdGeomMesh none(stage->GetPrimAtPath(SdfPath("/None")));
  none.CreateNormalsAttr().Set(VtVec3fArray(6, GfVec3f(1, 0, 0)));
  Compare("authored normals on a flat scheme", sync(), stage);

  points[0][0] = std::numeric_limits<float>::quiet_NaN();
  smooth.GetPointsAttr().Set(points);
  Check(!sync().scene->meshes.contains("/Smooth"),
      "a smooth mesh with non-finite points was retained");
  points[0][0] = 0;
  smooth.GetPointsAttr().Set(points);
  const auto recovered = sync();
  Compare("recovered finite points", recovered, stage);

  const auto unchanged = sync();
  Check(unchanged.scene == recovered.scene && unchanged.revision == recovered.revision,
      "a clean sync invalidated computed normals");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
