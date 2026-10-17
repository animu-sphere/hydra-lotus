// SPDX-License-Identifier: Apache-2.0
// Light sampling through Hydra. Syncs a USD stage with a camera and an
// emissive UsdPreviewSurface panel through UsdImaging, renders it through
// Lotus render passes until the colour AOV converges under the
// lotus:lightSampling render setting, and checks that it is a flag
// (usdview's checkable menu item) that defaults to true; that the panel's
// triangles fill the GPU scene's light table; that each estimator's
// converged image is the backend's deterministic image for it, bit for
// bit; that toggling the flag restarts the accumulation and reproduces each
// image; and that the two estimators' images agree statistically.
#include "adapter.hpp"

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
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <lotus/extraction.hpp>
#include <lotus/vulkan_backend.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// A floor and a back wall lit by a small emissive panel above the floor,
// seen by a perspective camera under the adapter's white environment.
constexpr const char* kStage = R"usda(#usda 1.0
def Camera "Camera"
{
    float focalLength = 18
    float horizontalAperture = 20.955
    float verticalAperture = 20.955
    double3 xformOp:translate = (0, 1, 4)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}

def Scope "Looks"
{
    def Material "Lamp"
    {
        token outputs:surface.connect = </Looks/Lamp/Surface.outputs:surface>
        def Shader "Surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0, 0, 0)
            color3f inputs:emissiveColor = (8, 6, 4)
            token outputs:surface
        }
    }
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

    def Mesh "Lamp" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-0.3, 1.5, -0.3), (0.3, 1.5, -0.3), (0.3, 1.5, 0.3), (-0.3, 1.5, 0.3)]
        rel material:binding = </Looks/Lamp>
    }
}
)usda";

constexpr std::uint32_t kSize = 32;
constexpr int kSamples = 16;
constexpr int kSampleIndex = 4096;
const TfToken kSampleIndexSetting("lotus:sampleIndex");
const TfToken kLightSamplingSetting("lotus:lightSampling");

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
// rendered through one pass into a kSize x kSize float colour AOV.
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

  bool Converged() const {
    return pass_->IsConverged() && color_.IsConverged();
  }

  // Renders until the colour converges, and checks that it took exactly
  // kSamples passes, the first of which started a fresh accumulation.
  Image Converge(const std::string& step) {
    for (int pass = 1; pass <= kSamples; ++pass) {
      scene_indices_.stageSceneIndex->ApplyPendingUpdates();
      engine_.Execute(index_.get(), &tasks_);
      const std::string at =
          step + ": pass " + std::to_string(pass) + " of " +
          std::to_string(kSamples);
      Check(Converged() == (pass == kSamples),
          at + ": convergence is " + (Converged() ? "early" : "missing"));
      const Lotus::GpuFrameEvidence frame = delegate_.GetFrameEvidence();
      Check(frame.status == Lotus::FrameStatus::Pass &&
                frame.samples_per_pixel == std::uint32_t(pass),
          at + ": the frame holds " +
              std::to_string(frame.samples_per_pixel) + " samples");
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
// pass traced) under `light_sampling`, rendered in one call by a renderer
// of its own into the target the pass configured, with rows flipped as
// Hydra stores them.
Image BackendImage(const Lotus::RendererShaders& shaders,
    const Lotus::FrameSnapshot& snapshot, Lotus::LightSampling light_sampling) {
  Lotus::FrameStatus status = Lotus::FrameStatus::Fail;
  std::string error;
  const auto renderer = Lotus::CreateOffscreenRenderer(
      shaders.vertex, shaders.fragment, status, error, shaders.ray_query);
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
  settings.light_sampling = light_sampling;
  settings.sample_index = kSampleIndex;
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

// Both estimators converge to the same image: each channel's image mean
// agrees within five standard errors, estimated from the spread of the two
// images' pixel differences. Reports the means.
void CheckAgreement(const Image& sampled, const Image& brute_force) {
  const std::size_t pixels = sampled.size() / 4;
  for (std::size_t c = 0; c < 3; ++c) {
    double sum = 0;
    double squares = 0;
    std::array<double, 2> means{};
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
      const double a = sampled[pixel * 4 + c];
      const double b = brute_force[pixel * 4 + c];
      Check(std::isfinite(a) && std::isfinite(b), "a non-finite pixel");
      means[0] += a / pixels;
      means[1] += b / pixels;
      sum += a - b;
      squares += (a - b) * (a - b);
    }
    const double mean = sum / pixels;
    const double variance =
        std::max(0.0, (squares - pixels * mean * mean) / (pixels - 1));
    const double tolerance = 5.0 * std::sqrt(variance / pixels) + 1e-6;
    std::cout << "channel " << c << ": mis " << means[0] << ", bsdf "
              << means[1] << ", difference " << mean << " +- " << tolerance
              << '\n';
    Check(std::abs(mean) <= tolerance,
        "channel " + std::to_string(c) +
            " of the estimators' images differs by more than their noise");
  }
}

} // namespace

int main(int, char** argv) try {
  // Missing GPU capability is a CTest SKIP, never a passing GPU check.
  // Without ray queries Hydra draws the bootstrap, which has no samples.
  const auto shaders =
      Lotus::ShadersIn(std::filesystem::absolute(argv[0]).parent_path() / "shaders");
  {
    Lotus::FrameStatus status = Lotus::FrameStatus::Fail;
    std::string error;
    const auto probe = Lotus::CreateOffscreenRenderer(
        shaders.vertex, shaders.fragment, status, error);
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

  HdRenderSettingsMap settings;
  settings[HdRenderSettingsTokens->convergedSamplesPerPixel] = VtValue(kSamples);
  settings[kSampleIndexSetting] = VtValue(kSampleIndex);
  Session session(stage, settings);
  const auto descriptors = session.Delegate().GetRenderSettingDescriptors();
  Check(std::any_of(descriptors.begin(), descriptors.end(),
            [](const HdRenderSettingDescriptor& descriptor) {
              return descriptor.key == kLightSamplingSetting &&
                     descriptor.defaultValue == VtValue(true);
            }),
      "lotus:lightSampling is not a flag that defaults to true");

  // Without the setting, light sampling traces the image.
  const Image sampled = session.Converge("default light sampling");
  const Lotus::FrameSnapshot snapshot = session.Delegate().GetSelectedSnapshot();
  Check(snapshot.scene != nullptr && snapshot.scene->meshes.size() == 3,
      "the pass did not trace the stage's three meshes");
  const std::uint32_t emissive =
      session.Delegate().GetGpuSceneStats().emissive_triangles;
  Check(emissive == 2, "the light table holds " + std::to_string(emissive) +
                           " triangles instead of the lamp's 2");
  Check(Same(sampled,
            BackendImage(shaders, snapshot, Lotus::LightSampling::Mis)),
      "Hydra's light-sampled image differs from the backend's");

  // Clearing the flag, as usdview's menu item does, restarts the
  // accumulation with BSDF sampling alone: the backend's brute-force image.
  session.Delegate().SetRenderSetting(kLightSamplingSetting, VtValue(false));
  const Image brute_force = session.Converge("bsdf sampling");
  Check(Same(brute_force,
            BackendImage(shaders, snapshot, Lotus::LightSampling::Bsdf)),
      "Hydra's BSDF-sampled image differs from the backend's");
  Check(!Same(sampled, brute_force),
      "the flag did not change the estimator");
  CheckAgreement(sampled, brute_force);

  // Toggling back and forth restarts each time and reproduces each image.
  session.Delegate().SetRenderSetting(kLightSamplingSetting, VtValue(true));
  Check(Same(session.Converge("enabled again"), sampled),
      "enabling light sampling again did not reproduce its image");
  session.Delegate().SetRenderSetting(kLightSamplingSetting, VtValue(false));
  Check(Same(session.Converge("disabled again"), brute_force),
      "disabling light sampling again did not reproduce the BSDF image");

  // A new delegate created with the flag cleared reproduces the BSDF image.
  settings[kLightSamplingSetting] = VtValue(false);
  Check(Same(Session(stage, settings).Converge("new delegate"), brute_force),
      "a new render delegate's BSDF-sampled image differs");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
