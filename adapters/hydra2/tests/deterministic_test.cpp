// SPDX-License-Identifier: Apache-2.0
// Deterministic mode through Hydra. Syncs a USD stage with a camera through
// UsdImaging, renders it through Lotus render passes until the colour AOV
// converges, and checks that the converged image is the backend's
// deterministic image for the lotus:sampleIndex and convergedSamplesPerPixel
// render settings, the camera and the AOV: bit for bit the same as the
// backend's own render, across render delegates and whatever the
// accumulation went through before.
#include "adapter.hpp"

#include <pxr/base/gf/vec3d.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <lotus/extraction.hpp>
#include <lotus/vulkan_backend.hpp>

#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// A floor, a back wall and a tilted triangle between them, seen by a
// perspective camera. Under the adapter's white environment the grey
// surfaces light each other, so every sample index gives different noise.
constexpr const char* kStage = R"usda(#usda 1.0
def Camera "Camera"
{
    float focalLength = 18
    float horizontalAperture = 20.955
    float verticalAperture = 20.955
    double3 xformOp:translate = (0, 1, 4)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}

def Xform "World"
{
    def Mesh "Floor"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-2, 0, 2), (2, 0, 2), (2, 0, -2), (-2, 0, -2)]
    }

    def Mesh "Wall"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-2, 0, -1), (2, 0, -1), (2, 3, -1), (-2, 3, -1)]
    }

    def Mesh "Panel"
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(-0.8, 0.1, 0.5), (0.8, 0.1, -0.3), (0, 1.6, 0)]
    }
}
)usda";

constexpr std::uint32_t kSize = 32;
constexpr int kSamples = 8;
constexpr int kSampleIndexA = 4096;
constexpr int kSampleIndexB = 8192;
const TfToken kSampleIndexSetting("lotus:sampleIndex");

void Check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

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
    pass_->Execute(state_, {});
  }
  const TfTokenVector& GetRenderTags() const override {
    static const TfTokenVector tags{HdRenderTagTokens->geometry};
    return tags;
  }

private:
  HdRenderPassSharedPtr pass_;
  HdRenderPassStateSharedPtr state_;
};

using Image = std::vector<float>;

// Bitwise, so that -0 and NaN count as differences too.
bool Same(const Image& a, const Image& b) {
  return a.size() == b.size() &&
         std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

// The stage synced into a render index with its own Lotus render delegate,
// rendered through one pass into a kSize² float colour AOV.
class Session {
public:
  Session(const UsdStageRefPtr& stage, const HdRenderSettingsMap& settings)
      : delegate_(settings), index_(HdRenderIndex::New(&delegate_, {})) {
    Check(index_ != nullptr, "could not create the render index");
    UsdImagingCreateSceneIndicesInfo info;
    info.stage = stage;
    scene_indices_ = UsdImagingCreateSceneIndices(info);
    scene_indices_.stageSceneIndex->SetTime(UsdTimeCode::Default());
    index_->InsertSceneIndex(
        scene_indices_.finalSceneIndex, SdfPath::AbsoluteRootPath());
    pass_ = delegate_.CreateRenderPass(index_.get(),
        HdRprimCollection(HdTokens->geometry,
            HdReprSelector(HdReprTokens->smoothHull),
            SdfPath::AbsoluteRootPath()));
    Check(color_.Allocate(GfVec3i(kSize, kSize, 1), HdFormatFloat32Vec4, false),
        "could not allocate the colour buffer");
    auto state = std::make_shared<HdRenderPassState>();
    const auto* camera = dynamic_cast<const HdCamera*>(
        index_->GetSprim(HdPrimTypeTokens->camera, SdfPath("/Camera")));
    Check(camera != nullptr, "the stage's camera is not a Hydra camera");
    state->SetCamera(camera);
    state->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0), kSize, kSize)));
    HdRenderPassAovBinding binding;
    binding.aovName = HdAovTokens->color;
    binding.renderBuffer = &color_;
    binding.clearValue = VtValue(GfVec4f(0.0F));
    state->SetAovBindings({binding});
    tasks_.push_back(std::make_shared<RenderTask>(pass_, state));
  }

  HdLotusRenderDelegate& Delegate() {
    return delegate_;
  }

  // One pass: one more sample, or the converged image again.
  void Render() {
    scene_indices_.stageSceneIndex->ApplyPendingUpdates();
    engine_.Execute(index_.get(), &tasks_);
  }

  bool Converged() const {
    return pass_->IsConverged() && color_.IsConverged();
  }

  // Renders until the colour converges and checks that it took exactly
  // `passes`.
  Image Converge(const std::string& step, int passes) {
    for (int pass = 1; pass <= passes; ++pass) {
      Render();
      Check(Converged() == (pass == passes),
          step + ": convergence after pass " + std::to_string(pass) + " of " +
              std::to_string(passes) + " is " +
              (Converged() ? "early" : "missing"));
    }
    return Read();
  }

  // The colour AOV, rows bottom-up as Hydra stores them.
  Image Read() {
    const auto* pixels = static_cast<const float*>(color_.Map());
    Check(pixels != nullptr, "the colour buffer is not allocated");
    Image image(pixels, pixels + std::size_t{kSize} * kSize * 4);
    color_.Unmap();
    return image;
  }

private:
  HdLotusRenderDelegate delegate_;
  std::unique_ptr<HdRenderIndex> index_;
  UsdImagingSceneIndices scene_indices_;
  HdRenderPassSharedPtr pass_;
  HdLotusRenderBuffer color_{SdfPath("/color")};
  HdTaskSharedPtrVector tasks_;
  HdEngine engine_;
};

// The backend's deterministic image of `snapshot` (the scene and camera a
// pass traced) from `sample_index`, rendered in one call by a renderer of its
// own into the target the pass configured, with rows flipped as Hydra stores
// them.
Image BackendImage(const std::filesystem::path& shaders,
    const Lotus::FrameSnapshot& snapshot, std::uint32_t sample_index) {
  Lotus::FrameStatus status = Lotus::FrameStatus::Fail;
  std::string error;
  const auto renderer = Lotus::CreateOffscreenRenderer(
      (shaders / "triangle.vert.spv").string(),
      (shaders / "triangle.frag.spv").string(), status, error,
      {(shaders / "path_trace.vert.spv").string(),
          (shaders / "path_trace.frag.spv").string()});
  Check(renderer != nullptr, "could not create the backend renderer: " + error);
  Lotus::SceneExtraction extraction;
  const Lotus::GpuSceneEvidence scene =
      renderer->UpdateScene(extraction.Update(snapshot));
  Check(scene.status == Lotus::FrameStatus::Pass,
      "the backend scene update failed: " + scene.detail);
  Lotus::OffscreenTarget target;
  target.width = kSize;
  target.height = kSize;
  target.display_window = {0.0F, 0.0F, float(kSize), float(kSize)};
  target.data_window = {0, 0, std::int32_t(kSize), std::int32_t(kSize)};
  Lotus::PathTracingSettings settings;
  settings.sample_index = sample_index;
  settings.max_samples = kSamples;
  const Lotus::GpuFrameEvidence frame = renderer->RenderScene(
      Lotus::ExtractDrawSummary(snapshot), target, kSamples, settings);
  Check(frame.status == Lotus::FrameStatus::Pass &&
            frame.samples_per_pixel == kSamples &&
            frame.color.pixel_format == "rgba32-sfloat" &&
            frame.color.payload.size() == std::size_t{kSize} * kSize * 16,
      "the backend render failed: " + frame.detail);
  Image image(std::size_t{kSize} * kSize * 4);
  const std::size_t row = std::size_t{kSize} * 4;
  for (std::size_t y = 0; y < kSize; ++y) {
    std::memcpy(image.data() + (kSize - 1 - y) * row,
        frame.color.payload.data() + y * row * sizeof(float),
        row * sizeof(float));
  }
  return image;
}

} // namespace

int main(int, char** argv) try {
  // Missing GPU capability is a CTest SKIP, never a passing GPU check.
  // Without ray queries Hydra draws the bootstrap, which has no samples.
  const auto shaders =
      std::filesystem::absolute(argv[0]).parent_path() / "shaders";
  {
    Lotus::FrameStatus status = Lotus::FrameStatus::Fail;
    std::string error;
    const auto probe = Lotus::CreateOffscreenRenderer(
        (shaders / "triangle.vert.spv").string(),
        (shaders / "triangle.frag.spv").string(), status, error);
    if (!probe) {
      std::cerr << error << '\n';
      return status == Lotus::FrameStatus::Skip ? 77 : 1;
    }
    if (const auto ray_query = probe->RayQueryCapability(); !ray_query.available) {
      std::cerr << "ray queries are unavailable: " << ray_query.detail << '\n';
      return 77;
    }
  }

  const SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
  Check(layer->ImportFromString(kStage), "could not parse the test stage");
  const UsdStageRefPtr stage = UsdStage::Open(layer);
  Check(stage != nullptr, "could not open the test stage");
  const UsdAttribute camera_position =
      stage->GetPrimAtPath(SdfPath("/Camera")).GetAttribute(TfToken("xformOp:translate"));

  HdRenderSettingsMap settings;
  settings[HdRenderSettingsTokens->convergedSamplesPerPixel] = VtValue(kSamples);
  settings[kSampleIndexSetting] = VtValue(kSampleIndexA);
  Session session(stage, settings);

  // The setting fixes the converged image: the backend's image from the
  // same first sample index, and the same image on further passes.
  const Image a = session.Converge("sample index A", kSamples);
  const Lotus::FrameSnapshot snapshot = session.Delegate().GetSelectedSnapshot();
  Check(snapshot.scene != nullptr && snapshot.scene->meshes.size() == 3,
      "the pass did not trace the stage's three meshes");
  Check(Same(a, BackendImage(shaders, snapshot, kSampleIndexA)),
      "Hydra's image differs from the backend's for sample index A");
  Check(!Same(a, BackendImage(shaders, snapshot, kSampleIndexA + 1)),
      "a neighbouring sample index gives the same image: the comparison "
      "proves nothing");
  session.Render();
  Check(session.Converged() && Same(session.Read(), a),
      "a pass after convergence changed the image");

  // Changing the setting restarts the accumulation from the new index.
  session.Delegate().SetRenderSetting(kSampleIndexSetting, VtValue(kSampleIndexB));
  const Image b = session.Converge("sample index B", kSamples);
  Check(Same(b, BackendImage(shaders, snapshot, kSampleIndexB)),
      "Hydra's image differs from the backend's for sample index B");
  Check(!Same(a, b), "sample indices A and B gave the same image");

  // Whatever the accumulation went through, returning to the same setting
  // and camera converges to the same image.
  session.Delegate().SetRenderSetting(kSampleIndexSetting, VtValue(kSampleIndexA));
  for (int pass = 0; pass < 3; ++pass) session.Render();
  camera_position.Set(GfVec3d(0.3, 1.2, 4.0));
  for (int pass = 0; pass < 2; ++pass) session.Render();
  Check(!Same(session.Read(), a), "moving the camera did not change the image");
  camera_position.Set(GfVec3d(0.0, 1.0, 4.0));
  Check(Same(session.Converge("camera restored", kSamples), a),
      "after a camera move and back, the image differs from sample index A's");

  // A new delegate, with a new Vulkan renderer, reproduces it.
  Check(Same(Session(stage, settings).Converge("new delegate", kSamples), a),
      "a new render delegate's image differs from sample index A's");

  // Without the setting, the first sample index is 0.
  HdRenderSettingsMap defaults;
  defaults[HdRenderSettingsTokens->convergedSamplesPerPixel] = VtValue(kSamples);
  Session unset(stage, defaults);
  Check(Same(unset.Converge("default sample index", kSamples),
            BackendImage(shaders, snapshot, 0)),
      "without lotus:sampleIndex, Hydra's image is not sample index 0's");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
