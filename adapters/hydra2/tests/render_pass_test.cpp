// SPDX-License-Identifier: Apache-2.0
// Syncs a composed USD stage through UsdImaging's scene indices into the
// Lotus render delegate, renders it through a render pass whose collection
// and render tags change, and checks which meshes the pass traces. With
// --gpu the pass also renders a colour AOV and the GPU scene must hold one
// instance per traced mesh.
#include "adapter.hpp"

#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <lotus/vulkan_backend.hpp>

#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// One triangle per mesh: Geometry has the default purpose, Proxy and Guide
// their own, Inside is under the collection's exclude path and Outside under
// none of its root paths.
constexpr const char* kStage = R"usda(#usda 1.0
def Xform "World"
{
    def Mesh "Geometry"
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(-0.5, -0.5, 0), (0.5, -0.5, 0), (0, 0.5, 0)]
    }

    def Mesh "Proxy"
    {
        uniform token purpose = "proxy"
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(-0.5, -0.5, -1), (0.5, -0.5, -1), (0, 0.5, -1)]
    }

    def Mesh "Guide"
    {
        uniform token purpose = "guide"
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(-0.5, -0.5, -2), (0.5, -0.5, -2), (0, 0.5, -2)]
    }

    def Xform "Excluded"
    {
        def Mesh "Inside"
        {
            int[] faceVertexCounts = [3]
            int[] faceVertexIndices = [0, 1, 2]
            point3f[] points = [(-0.5, -0.5, -3), (0.5, -0.5, -3), (0, 0.5, -3)]
        }
    }
}

def Mesh "Outside"
{
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(-0.5, -0.5, -4), (0.5, -0.5, -4), (0, 0.5, -4)]
}
)usda";

void Check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

// Syncs and executes one render pass with the given render tags.
class RenderTask final : public HdTask {
public:
  RenderTask(HdRenderPassSharedPtr pass, HdRenderPassStateSharedPtr state)
      : HdTask(SdfPath::EmptyPath()), pass_(std::move(pass)),
        state_(std::move(state)) {
  }
  void Sync(HdSceneDelegate*, HdTaskContext*, HdDirtyBits* dirty_bits) override {
    pass_->Sync();
    *dirty_bits = HdChangeTracker::Clean;
  }
  void Prepare(HdTaskContext*, HdRenderIndex*) override {
  }
  void Execute(HdTaskContext*) override {
    pass_->Execute(state_, tags);
  }
  const TfTokenVector& GetRenderTags() const override {
    return tags;
  }

  TfTokenVector tags;

private:
  HdRenderPassSharedPtr pass_;
  HdRenderPassStateSharedPtr state_;
};

using Ids = std::set<std::string>;

Ids Visible(const Lotus::FrameSnapshot& snapshot) {
  Ids ids;
  for (const auto& [id, mesh] : snapshot.scene->meshes) {
    if (mesh.instance.visible) ids.insert(id);
  }
  return ids;
}

std::string Describe(const Ids& ids) {
  std::string text;
  for (const std::string& id : ids) text += " " + id;
  return text.empty() ? " (none)" : text;
}

} // namespace

int main(int argc, char** argv) try {
  const bool gpu = argc > 1 && std::string(argv[1]) == "--gpu";
  if (gpu) {
    // Missing GPU capability is a CTest SKIP, never a passing GPU check.
    const auto shaders =
        std::filesystem::absolute(argv[0]).parent_path() / "shaders";
    Lotus::FrameStatus status;
    std::string error;
    if (!Lotus::CreateOffscreenRenderer(
            (shaders / "triangle.vert.spv").string(),
            (shaders / "triangle.frag.spv").string(), status, error)) {
      std::cerr << error << '\n';
      return status == Lotus::FrameStatus::Skip ? 77 : 1;
    }
  }

  const SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
  Check(layer->ImportFromString(kStage), "could not parse the test stage");
  const UsdStageRefPtr stage = UsdStage::Open(layer);
  Check(stage != nullptr, "could not open the test stage");

  HdRenderSettingsMap settings;
  settings[HdRenderSettingsTokens->convergedSamplesPerPixel] = VtValue(1);
  HdLotusRenderDelegate delegate(settings);
  std::unique_ptr<HdRenderIndex> index(HdRenderIndex::New(&delegate, {}));
  Check(index != nullptr, "could not create the render index");
  UsdImagingCreateSceneIndicesInfo info;
  info.stage = stage;
  const UsdImagingSceneIndices scene_indices = UsdImagingCreateSceneIndices(info);
  scene_indices.stageSceneIndex->SetTime(UsdTimeCode::Default());
  index->InsertSceneIndex(scene_indices.finalSceneIndex, SdfPath::AbsoluteRootPath());

  HdRprimCollection collection(HdTokens->geometry,
      HdReprSelector(HdReprTokens->smoothHull), SdfPath("/World"));
  collection.SetExcludePaths({SdfPath("/World/Excluded")});
  const HdRenderPassSharedPtr pass =
      delegate.CreateRenderPass(index.get(), collection);
  // Without AOV bindings the pass selects its meshes and renders nothing.
  auto state = std::make_shared<HdRenderPassState>();
  HdLotusRenderBuffer color(SdfPath("/color"));
  if (gpu) {
    Check(color.Allocate(GfVec3i(8, 8, 1), HdFormatFloat32Vec4, false),
        "could not allocate the colour buffer");
    HdRenderPassAovBinding binding;
    binding.aovName = HdAovTokens->color;
    binding.renderBuffer = &color;
    binding.clearValue = VtValue(GfVec4f(0.0F));
    state->SetAovBindings({binding});
  }
  const auto task = std::make_shared<RenderTask>(pass, state);
  HdTaskSharedPtrVector tasks{task};
  HdEngine engine;

  std::uint64_t uploads = 0;
  // Renders once and checks the meshes the pass traced.
  const auto render = [&](const char* step, const Ids& expected) {
    scene_indices.stageSceneIndex->ApplyPendingUpdates();
    engine.Execute(index.get(), &tasks);
    const Lotus::FrameSnapshot selected = delegate.GetSelectedSnapshot();
    Check(selected.scene != nullptr, std::string(step) + ": nothing was selected");
    const Ids visible = Visible(selected);
    Check(visible == expected, std::string(step) + ": traced" +
                                   Describe(visible) + ", expected" +
                                   Describe(expected));
    Check(selected.triangle_count == expected.size(),
        std::string(step) + ": the selected snapshot counts " +
            std::to_string(selected.triangle_count) + " triangles");
    if (gpu) {
      const Lotus::GpuSceneStats stats = delegate.GetGpuSceneStats();
      Check(stats.instance_count == expected.size(),
          std::string(step) + ": the GPU scene holds " +
              std::to_string(stats.instance_count) + " instances");
      uploads = stats.geometry_uploads;
    }
    return selected;
  };

  task->tags = {HdRenderTagTokens->geometry};
  render("geometry tag", {"/World/Geometry"});
  Check(delegate.GetFrameSnapshot().scene->meshes.contains("/Outside"),
      "a mesh outside the collection was not synced, so the test proves nothing");

  task->tags = {HdRenderTagTokens->geometry, HdRenderTagTokens->proxy};
  const Lotus::FrameSnapshot with_proxy =
      render("proxy tag", {"/World/Geometry", "/World/Proxy"});
  const std::uint64_t uploads_with_proxy = uploads;

  // The proxy mesh stays in the scene, synced, but is no longer traced.
  task->tags = {HdRenderTagTokens->geometry};
  const Lotus::FrameSnapshot without_proxy =
      render("proxy tag removed", {"/World/Geometry"});
  Check(without_proxy.scene->meshes.at("/World/Proxy").geometry ==
            with_proxy.scene->meshes.at("/World/Proxy").geometry,
      "deselecting the proxy mesh replaced its geometry");
  Check(!gpu || uploads == uploads_with_proxy,
      "deselecting the proxy mesh uploaded geometry");
  Check(render("unchanged", {"/World/Geometry"}).scene == without_proxy.scene,
      "an unchanged frame copied the selected scene");

  UsdGeomImageable(stage->GetPrimAtPath(SdfPath("/World/Proxy")))
      .GetPurposeAttr()
      .Set(UsdGeomTokens->default_);
  render("purpose edit", {"/World/Geometry", "/World/Proxy"});

  UsdGeomImageable(stage->GetPrimAtPath(SdfPath("/World/Geometry"))).MakeInvisible();
  render("invisible mesh", {"/World/Proxy"});

  pass->SetRprimCollection(HdRprimCollection(HdTokens->geometry,
      HdReprSelector(HdReprTokens->smoothHull)));
  render("whole-stage collection",
      {"/World/Proxy", "/World/Excluded/Inside", "/Outside"});

  // Hydra syncs only meshes whose render tag a task requests, so the guide
  // mesh is synced here, then deselected, then traced again without tags.
  task->tags = {HdRenderTagTokens->geometry, HdRenderTagTokens->guide};
  render("guide tag",
      {"/World/Proxy", "/World/Guide", "/World/Excluded/Inside", "/Outside"});
  task->tags = {HdRenderTagTokens->geometry};
  render("guide tag removed",
      {"/World/Proxy", "/World/Excluded/Inside", "/Outside"});
  task->tags.clear();
  render("no render tags",
      {"/World/Proxy", "/World/Guide", "/World/Excluded/Inside", "/Outside"});
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
