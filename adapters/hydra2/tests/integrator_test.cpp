// SPDX-License-Identifier: Apache-2.0
// Integrator selection through Hydra. Syncs a USD stage with a camera
// through UsdImaging, renders it through Lotus render passes until the
// colour AOV converges under the lotus:wavefront render setting, and checks
// that it is a flag (usdview's checkable menu item); that each integrator's
// converged image is the backend's deterministic image for that integrator,
// bit for bit; that the frames came from the integrator the flag selects;
// that toggling it restarts the accumulation; and that the two integrators'
// images agree up to floating-point evaluation differences.
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

// A floor, a back wall and a tilted triangle between them, seen by a
// perspective camera. Under the adapter's white environment the grey
// surfaces light each other, so paths take several bounces.
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
constexpr int kSampleIndex = 4096;
const TfToken kSampleIndexSetting("lotus:sampleIndex");
const TfToken kWavefrontSetting("lotus:wavefront");

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
  // kSamples passes and that each pass's sample came from `integrator`:
  // only the wavefront integrator reports queue occupancy, with one camera
  // ray per pixel.
  Image Converge(const std::string& step, Lotus::Integrator integrator) {
    const bool wavefront = integrator == Lotus::Integrator::Wavefront;
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
      Check(frame.wavefront_path_counts.empty() != wavefront,
          at + ": the sample did not come from the " +
              (wavefront ? "wavefront" : "reference") + " integrator");
      Check(!wavefront || frame.wavefront_path_counts[0] == kSize * kSize,
          at + ": the wavefront integrator did not start a path per pixel");
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
// pass traced) under `integrator`, rendered in one call by a renderer of its
// own into the target the pass configured, with rows flipped as Hydra stores
// them.
Image BackendImage(const std::filesystem::path& shaders,
    const Lotus::FrameSnapshot& snapshot, Lotus::Integrator integrator) {
  Lotus::FrameStatus status = Lotus::FrameStatus::Fail;
  std::string error;
  const auto renderer = Lotus::CreateOffscreenRenderer(
      (shaders / "triangle.vert.spv").string(),
      (shaders / "triangle.frag.spv").string(), status, error,
      {(shaders / "path_trace.vert.spv").string(),
          (shaders / "path_trace.frag.spv").string(),
          (shaders / "wavefront.comp.spv").string()});
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
  settings.integrator = integrator;
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

// Both integrators trace the same samples, so their images may differ only
// by floating-point evaluation: each channel's image mean within 1e-3 of
// the larger mean, a few samples changing paths. Reports how many values
// differ.
void CheckAgreement(const Image& reference, const Image& wavefront) {
  std::size_t differing = 0;
  double max_difference = 0.0;
  std::array<double, 3> sums[2]{};
  for (std::size_t i = 0; i < reference.size(); ++i) {
    if (i % 4 != 3) {
      sums[0][i % 4] += reference[i];
      sums[1][i % 4] += wavefront[i];
    }
    if (std::memcmp(&reference[i], &wavefront[i], sizeof(float)) == 0)
      continue;
    ++differing;
    max_difference = std::max(
        max_difference, std::abs(double{reference[i]} - wavefront[i]));
  }
  std::cout << "the same " << kSamples << " spp from both integrators: ";
  if (differing == 0)
    std::cout << "bit for bit\n";
  else
    std::cout << differing << " of " << reference.size()
              << " values differ, by at most " << max_difference << '\n';
  for (std::size_t c = 0; c < 3; ++c) {
    const double scale = std::max({sums[0][c], sums[1][c], 1e-30});
    Check(std::abs(sums[0][c] - sums[1][c]) <= 1e-3 * scale,
        "channel " + std::to_string(c) + " of the integrators' images "
        "differs by more than floating-point evaluation");
  }
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

  HdRenderSettingsMap settings;
  settings[HdRenderSettingsTokens->convergedSamplesPerPixel] = VtValue(kSamples);
  settings[kSampleIndexSetting] = VtValue(kSampleIndex);
  Session session(stage, settings);
  const auto descriptors = session.Delegate().GetRenderSettingDescriptors();
  Check(std::any_of(descriptors.begin(), descriptors.end(),
            [](const HdRenderSettingDescriptor& descriptor) {
              return descriptor.key == kWavefrontSetting &&
                     descriptor.defaultValue == VtValue(false);
            }),
      "lotus:wavefront is not a flag that defaults to false");

  // Without the setting, the reference integrator traces the image.
  const Image reference =
      session.Converge("default integrator", Lotus::Integrator::Reference);
  const Lotus::FrameSnapshot snapshot = session.Delegate().GetSelectedSnapshot();
  Check(snapshot.scene != nullptr && snapshot.scene->meshes.size() == 3,
      "the pass did not trace the stage's three meshes");
  Check(Same(reference,
            BackendImage(shaders, snapshot, Lotus::Integrator::Reference)),
      "Hydra's reference image differs from the backend's");

  // Enabling the flag, as usdview's menu item does, restarts the
  // accumulation with the wavefront integrator: the backend's wavefront
  // image.
  session.Delegate().SetRenderSetting(kWavefrontSetting, VtValue(true));
  const Image wavefront =
      session.Converge("wavefront", Lotus::Integrator::Wavefront);
  Check(Same(wavefront,
            BackendImage(shaders, snapshot, Lotus::Integrator::Wavefront)),
      "Hydra's wavefront image differs from the backend's");
  CheckAgreement(reference, wavefront);

  // Toggling back and forth restarts each time and reproduces each image.
  session.Delegate().SetRenderSetting(kWavefrontSetting, VtValue(false));
  Check(Same(session.Converge("disabled", Lotus::Integrator::Reference),
            reference),
      "disabling the wavefront integrator did not reproduce the reference "
      "image");
  session.Delegate().SetRenderSetting(kWavefrontSetting, VtValue(true));
  Check(Same(session.Converge("enabled again", Lotus::Integrator::Wavefront),
            wavefront),
      "enabling the wavefront integrator again did not reproduce its image");

  // A new delegate created with the flag set reproduces the wavefront image.
  settings[kWavefrontSetting] = VtValue(true);
  Check(Same(Session(stage, settings).Converge(
                 "new delegate", Lotus::Integrator::Wavefront),
            wavefront),
      "a new render delegate's wavefront image differs");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
