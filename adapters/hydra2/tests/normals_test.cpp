// SPDX-License-Identifier: Apache-2.0
// Syncs a composed USD stage's meshes through UsdImaging's scene indices into
// the Lotus render delegate and compares each triangle corner's normal with
// the one read independently from the stage through UsdGeom, for every
// interpolation, indexed primvars, holes and left-handed faces, then through
// edits that add, change and remove normals.
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
#include <iostream>
#include <memory>
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
    return {};
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
    Check(geometry.normals == Expected(stage, path, geometry),
        step + ": the corner normals of " + path + " differ from the stage's");
  }
}

} // namespace

int main() try {
  const SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
  Check(layer->ImportFromString(kStage), "could not parse the test stage");
  const UsdStageRefPtr stage = UsdStage::Open(layer);
  Check(stage != nullptr, "could not open the test stage");

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
            meshes.at("/Plain").geometry->normals.empty(),
      "the stage's normals were not all read, or the primvar did not win");
  // Normals that do not match their interpolation are ignored; the mesh
  // stays.
  Check(meshes.at("/Mismatched").geometry->normals.empty() &&
            meshes.at("/Mismatched").geometry->triangles.size() == 3,
      "mismatched normals were used, or their mesh was dropped");

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
            changed.scene->meshes.at("/Vertex").geometry->normals.empty(),
      "an added primvar or a removed attribute was not synced");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
