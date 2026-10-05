// SPDX-License-Identifier: Apache-2.0
// Syncs a composed USD stage through UsdImaging's scene indices into the
// Lotus render delegate and compares each mesh's placements with transforms
// computed independently by UsdGeom.
#include "adapter.hpp"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/quath.h>
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
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// Mesh A and B are a point instancer's prototypes, C is a prototype of a
// point instancer nested in another, and D is in a native instance
// prototype. Each mesh's first point's x identifies it.
constexpr const char* kStage = R"usda(#usda 1.0
def Xform "World"
{
    def PointInstancer "Points"
    {
        rel prototypes = [</World/Points/Prototypes/A>, </World/Points/Prototypes/B>]
        int[] protoIndices = [0, 1, 0, 1]
        point3f[] positions = [(1, 0, 0), (0, 2, 0), (0, 0, 3), (-1, -1, 0)]
        quath[] orientations = [(1, 0, 0, 0), (0.70710677, 0, 0, 0.70710677), (0.70710677, 0.70710677, 0, 0), (1, 0, 0, 0)]
        float3[] scales = [(1, 1, 1), (2, 1, 1), (1, 0.5, 1), (1, 1, 3)]
        double3 xformOp:translate = (10, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]

        def Scope "Prototypes"
        {
            def Mesh "A"
            {
                int[] faceVertexCounts = [3]
                int[] faceVertexIndices = [0, 1, 2]
                point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
                double3 xformOp:translate = (0, 0, 0.5)
                float xformOp:rotateZ = 30
                uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateZ"]
            }

            def Mesh "B"
            {
                int[] faceVertexCounts = [3]
                int[] faceVertexIndices = [0, 1, 2]
                point3f[] points = [(5, 0, 0), (6, 0, 0), (5, 1, 0)]
            }
        }
    }

    def PointInstancer "Outer"
    {
        rel prototypes = [</World/Outer/Prototypes/Inner>]
        int[] protoIndices = [0, 0]
        point3f[] positions = [(0, 0, -5), (0, 5, -5)]
        quath[] orientations = [(0.70710677, 0, 0, 0.70710677), (1, 0, 0, 0)]
        float3[] scales = [(1, 2, 1), (0.5, 1, 1)]

        def Scope "Prototypes"
        {
            def PointInstancer "Inner"
            {
                rel prototypes = [</World/Outer/Prototypes/Inner/Prototypes/C>]
                int[] protoIndices = [0, 0, 0]
                point3f[] positions = [(1, 0, 0), (2, 0, 0), (3, 0, 0)]
                double3 xformOp:translate = (0, 0, 1)
                uniform token[] xformOpOrder = ["xformOp:translate"]

                def Scope "Prototypes"
                {
                    def Mesh "C"
                    {
                        int[] faceVertexCounts = [3]
                        int[] faceVertexIndices = [0, 1, 2]
                        point3f[] points = [(7, 0, 0), (8, 0, 0), (7, 1, 0)]
                    }
                }
            }
        }
    }

    def Xform "Native"
    {
        def "One" (
            instanceable = true
            references = </Prototype>
        )
        {
            double3 xformOp:translate = (0, -4, 0)
            uniform token[] xformOpOrder = ["xformOp:translate"]
        }

        def "Two" (
            instanceable = true
            references = </Prototype>
        )
        {
            double3 xformOp:translate = (3, -4, 0)
            float xformOp:rotateX = 90
            uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateX"]
        }
    }
}

class Xform "Prototype"
{
    def Mesh "D"
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(9, 0, 0), (10, 0, 0), (9, 1, 0)]
        double3 xformOp:translate = (0, 0, 2)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
}
)usda";

void Check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

// Enqueues the render pass's collection, so the render index syncs its
// meshes. It renders nothing.
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

using Placements = std::map<float, std::vector<GfMatrix4d>>;

// The world transforms UsdGeom computes for each mesh's placements.
Placements Expected(const UsdStageRefPtr& stage) {
  const UsdTimeCode time = UsdTimeCode::Default();
  UsdGeomXformCache cache(time);
  Placements expected;
  const auto instances = [&](const char* path, VtMatrix4dArray& transforms,
                             VtIntArray& prototypes) {
    const UsdGeomPointInstancer instancer(stage->GetPrimAtPath(SdfPath(path)));
    Check(instancer.ComputeInstanceTransformsAtTime(&transforms, time, time,
              UsdGeomPointInstancer::IncludeProtoXform,
              UsdGeomPointInstancer::IgnoreMask),
        std::string("UsdGeom could not compute the instances of ") + path);
    instancer.GetProtoIndicesAttr().Get(&prototypes, time);
    const std::vector<bool> mask = instancer.ComputeMaskAtTime(time);
    for (std::size_t i = 0; i < mask.size(); ++i) {
      if (!mask[i]) prototypes[i] = -1;
    }
    return cache.GetLocalToWorldTransform(instancer.GetPrim());
  };

  VtMatrix4dArray points;
  VtIntArray point_prototypes;
  const GfMatrix4d points_world = instances("/World/Points", points, point_prototypes);
  for (std::size_t i = 0; i < points.size(); ++i) {
    if (point_prototypes[i] >= 0) {
      expected[point_prototypes[i] == 0 ? 0.0F : 5.0F].push_back(
          points[i] * points_world);
    }
  }

  VtMatrix4dArray outer;
  VtIntArray outer_prototypes;
  const GfMatrix4d outer_world = instances("/World/Outer", outer, outer_prototypes);
  VtMatrix4dArray inner;
  VtIntArray inner_prototypes;
  instances("/World/Outer/Prototypes/Inner", inner, inner_prototypes);
  for (std::size_t i = 0; i < outer.size(); ++i) {
    for (std::size_t j = 0; j < inner.size(); ++j) {
      if (outer_prototypes[i] >= 0 && inner_prototypes[j] >= 0) {
        expected[7.0F].push_back(inner[j] * outer[i] * outer_world);
      }
    }
  }

  for (const UsdPrim& instance :
      stage->GetPrimAtPath(SdfPath("/World/Native")).GetChildren()) {
    expected[9.0F].push_back(cache.GetLocalToWorldTransform(
        stage->GetPrimAtPath(instance.GetPath().AppendChild(TfToken("D")))));
  }
  return expected;
}

// Every visible mesh's placements, by its first point's x. Each mesh must
// be placed by an instancer.
Placements Actual(const Lotus::FrameSnapshot& snapshot) {
  Placements actual;
  for (const auto& [id, mesh] : snapshot.scene->meshes) {
    Check(mesh.instance.instancer_transforms.has_value(),
        id + " is not placed by an instancer");
    if (!mesh.instance.visible) continue;
    auto& placements = actual[mesh.geometry->positions.at(0)[0]];
    for (const Lotus::Matrix4& placement :
        Lotus::PlacementTransforms(mesh.instance)) {
      GfMatrix4d matrix;
      double* data = matrix.data();
      for (std::size_t index = 0; index < placement.size(); ++index) {
        data[index] = placement[index];
      }
      placements.push_back(matrix);
    }
  }
  return actual;
}

std::string Describe(const Placements& placements) {
  std::ostringstream stream;
  for (const auto& [mesh, matrices] : placements) {
    stream << "\n  mesh " << mesh << ":";
    for (const GfMatrix4d& matrix : matrices) stream << "\n    " << matrix;
  }
  return stream.str();
}

// Placements are compared as sets: Hydra's instance order is not UsdGeom's.
void Compare(const char* step, const Lotus::FrameSnapshot& snapshot,
    const UsdStageRefPtr& stage) {
  const Placements expected = Expected(stage);
  const Placements actual = Actual(snapshot);
  bool match = expected.size() == actual.size();
  for (auto e = expected.begin(), a = actual.begin();
       match && e != expected.end(); ++e, ++a) {
    match = e->first == a->first && e->second.size() == a->second.size();
    std::vector<bool> used(a->second.size());
    for (const GfMatrix4d& matrix : e->second) {
      bool found = false;
      for (std::size_t i = 0; match && !found && i < a->second.size(); ++i) {
        found = !used[i] && GfIsClose(matrix, a->second[i], 1e-5);
        if (found) used[i] = true;
      }
      match = match && found;
    }
  }
  Check(match, std::string(step) + ": placements differ from UsdGeom\nexpected:" +
                   Describe(expected) + "\nactual:" + Describe(actual));
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
  Check(first.scene->meshes.size() == 4,
      "expected one mesh per prototype, got " +
          std::to_string(first.scene->meshes.size()));

  const auto points = UsdGeomPointInstancer::Get(stage, SdfPath("/World/Points"));
  points.GetPositionsAttr().Set(VtVec3fArray{GfVec3f(1, 1, 0), GfVec3f(0, 2, 0),
      GfVec3f(0, 0, 3), GfVec3f(-1, -1, 4)});
  const Lotus::FrameSnapshot moved = sync();
  Compare("instance positions", moved, stage);
  for (const auto& [id, mesh] : moved.scene->meshes) {
    Check(mesh.geometry == first.scene->meshes.at(id).geometry,
        "an instance edit replaced the geometry of " + id);
  }

  points.GetInvisibleIdsAttr().Set(VtInt64Array{1});
  Compare("invisible instance", sync(), stage);
  points.GetProtoIndicesAttr().Set(VtIntArray{0, 0, 1, 0});
  Compare("prototype indices", sync(), stage);
  stage->GetPrimAtPath(SdfPath("/World/Points"))
      .GetAttribute(TfToken("xformOp:translate"))
      .Set(GfVec3d(-2, 3, 1));
  Compare("instancer transform", sync(), stage);
  stage->GetPrimAtPath(SdfPath("/World/Points/Prototypes/A"))
      .GetAttribute(TfToken("xformOp:rotateZ"))
      .Set(-45.0F);
  Compare("prototype transform", sync(), stage);

  UsdGeomPointInstancer::Get(stage, SdfPath("/World/Outer/Prototypes/Inner"))
      .GetPositionsAttr()
      .Set(VtVec3fArray{GfVec3f(0, 1, 0), GfVec3f(0, 2, 0), GfVec3f(0, 3, 0)});
  Compare("nested instance positions", sync(), stage);
  const auto outer = UsdGeomPointInstancer::Get(stage, SdfPath("/World/Outer"));
  outer.GetProtoIndicesAttr().Set(VtIntArray{0, 0, 0});
  outer.GetPositionsAttr().Set(
      VtVec3fArray{GfVec3f(0, 0, -5), GfVec3f(0, 5, -5), GfVec3f(5, 0, -5)});
  outer.GetOrientationsAttr().Set(VtQuathArray{GfQuath(0.70710677F, 0, 0, 0.70710677F),
      GfQuath(1), GfQuath(0.70710677F, 0.70710677F, 0, 0)});
  outer.GetScalesAttr().Set(
      VtVec3fArray{GfVec3f(1, 2, 1), GfVec3f(0.5F, 1, 1), GfVec3f(1, 1, 3)});
  Compare("nested parent instances", sync(), stage);

  stage->GetPrimAtPath(SdfPath("/World/Native/Two"))
      .GetAttribute(TfToken("xformOp:translate"))
      .Set(GfVec3d(-3, 4, 0));
  Compare("native instance transform", sync(), stage);
  stage->DefinePrim(SdfPath("/World/Native/Three"))
      .GetReferences()
      .AddInternalReference(SdfPath("/Prototype"));
  stage->GetPrimAtPath(SdfPath("/World/Native/Three")).SetInstanceable(true);
  Compare("native instance insertion", sync(), stage);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
