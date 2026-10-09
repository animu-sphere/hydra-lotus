// SPDX-License-Identifier: Apache-2.0
// Composed USD dome edits through UsdImaging; GPU mode checks exact mirror
// radiance and accumulation restarts, independently of a backend render.
#include "adapter.hpp"

#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/light.h>
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

#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {
constexpr int kSize = 8;
constexpr int kSamples = 4;
constexpr const char* kStage = R"usda(#usda 1.0
def Camera "Camera"
{
    double3 xformOp:translate = (0, 0, 4)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}
def Material "Mirror"
{
    token outputs:surface.connect = </Mirror/Surface.outputs:surface>
    def Shader "Surface"
    {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (1, 1, 1)
        float inputs:metallic = 1
        float inputs:roughness = 0
        token outputs:surface
    }
}
def Mesh "Panel" (
    prepend apiSchemas = ["MaterialBindingAPI"]
)
{
    uniform token subdivisionScheme = "none"
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-100, -100, 0), (100, -100, 0),
        (100, 100, 0), (-100, 100, 0)]
    rel material:binding = </Mirror>
}
def Xform "Lights" {}
)usda";

void Check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

class Task final : public HdTask {
public:
  Task(HdRenderPassSharedPtr pass, HdRenderPassStateSharedPtr state)
      : HdTask(SdfPath::EmptyPath()), pass_(std::move(pass)),
        state_(std::move(state)) {}
  void Sync(HdSceneDelegate*, HdTaskContext*, HdDirtyBits* bits) override {
    pass_->Sync();
    *bits = HdChangeTracker::Clean;
  }
  void Prepare(HdTaskContext*, HdRenderIndex*) override {}
  void Execute(HdTaskContext*) override {
    if (state_) pass_->Execute(state_, {});
  }
  const TfTokenVector& GetRenderTags() const override {
    static const TfTokenVector tags{HdRenderTagTokens->geometry};
    return tags;
  }
private:
  HdRenderPassSharedPtr pass_;
  HdRenderPassStateSharedPtr state_;
};

void Set(const UsdPrim& prim, const char* name, const VtValue& value) {
  Check(prim.GetAttribute(TfToken(name)).Set(value),
      std::string("could not set ") + name);
}

int Run(bool gpu) {
  const auto layer = SdfLayer::CreateAnonymous(".usda");
  Check(layer->ImportFromString(kStage), "could not parse the stage");
  const auto stage = UsdStage::Open(layer);
  Check(bool(stage), "could not open the stage");
  HdRenderSettingsMap settings;
  settings[HdRenderSettingsTokens->convergedSamplesPerPixel] = VtValue(kSamples);
  settings[TfToken("lotus:sampleIndex")] = VtValue(123);
  HdLotusRenderDelegate delegate(settings);
  std::unique_ptr<HdRenderIndex> index(HdRenderIndex::New(&delegate, {}));
  Check(bool(index), "could not create the render index");
  UsdImagingCreateSceneIndicesInfo info;
  info.stage = stage;
  auto indices = UsdImagingCreateSceneIndices(info);
  indices.stageSceneIndex->SetTime(UsdTimeCode::Default());
  index->InsertSceneIndex(indices.finalSceneIndex, SdfPath::AbsoluteRootPath());
  auto pass = delegate.CreateRenderPass(index.get(),
      HdRprimCollection(HdTokens->geometry,
          HdReprSelector(HdReprTokens->smoothHull), SdfPath::AbsoluteRootPath()));
  HdLotusRenderBuffer color(SdfPath("/color"));
  HdRenderPassStateSharedPtr state;
  if (gpu) {
    Check(color.Allocate(GfVec3i(kSize, kSize, 1), HdFormatFloat32Vec4, false),
        "could not allocate colour");
    state = std::make_shared<HdRenderPassState>();
    const auto* camera = dynamic_cast<const HdCamera*>(
        index->GetSprim(HdPrimTypeTokens->camera, SdfPath("/Camera")));
    Check(camera != nullptr, "no stage camera");
    state->SetCamera(camera);
    state->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0), kSize, kSize)));
    HdRenderPassAovBinding binding;
    binding.aovName = HdAovTokens->color;
    binding.renderBuffer = &color;
    binding.clearValue = VtValue(GfVec4f(0));
    state->SetAovBindings({binding});
  }
  HdTaskSharedPtrVector tasks{std::make_shared<Task>(pass, state)};
  HdEngine engine;
  const auto tick = [&] {
    indices.stageSceneIndex->ApplyPendingUpdates();
    engine.Execute(index.get(), &tasks);
  };
  const auto verify = [&](const std::string& step,
                          const std::array<float, 3>& expected,
                          bool restart = true) {
    for (int sample = 1; sample <= (gpu && restart ? kSamples : 1); ++sample) {
      tick();
      if (gpu) {
        const bool converged = !restart || sample == kSamples;
        Check(pass->IsConverged() == converged &&
                  color.IsConverged() == converged,
            step + ": accumulation did not restart/converge as expected");
      }
    }
    const auto snapshot = delegate.GetFrameSnapshot();
    Check(snapshot.scene->environment == expected,
        step + ": wrong environment radiance");
    if (gpu) {
      const auto* pixels = static_cast<const float*>(color.Map());
      bool matches = pixels != nullptr;
      if (pixels) {
        for (int p = 0; p < kSize * kSize; ++p) {
          for (int c = 0; c < 3; ++c) {
            matches = matches && std::isfinite(pixels[p * 4 + c]) &&
                std::abs(pixels[p * 4 + c] - expected[c]) < 1.0e-5F;
          }
          matches = matches && pixels[p * 4 + 3] == 1.0F;
        }
      }
      color.Unmap();
      Check(matches, step + ": mirror image differs from analytic radiance");
    }
  };
  verify("no domes", {1, 1, 1});
  const auto first = delegate.GetFrameSnapshot();
  const auto geometry = first.scene->meshes.at("/Panel").geometry;
  const auto stats = delegate.GetGpuSceneStats();
  // Creating or destroying fallback sprims must not add a light.
  delegate.DestroySprim(delegate.CreateFallbackSprim(HdPrimTypeTokens->domeLight));
  verify("fallback sprim", {1, 1, 1}, false);
  Check(delegate.GetFrameSnapshot().scene == first.scene,
      "fallback sprim or unchanged sync mutated the scene");

  const auto a = stage->DefinePrim(SdfPath("/Lights/A"), TfToken("DomeLight"));
  verify("default dome", {1, 1, 1}, false);
  Check(index->GetSprim(HdPrimTypeTokens->domeLight, a.GetPath()) != nullptr,
      "DomeLight was not populated through UsdImaging");
  Set(a, "inputs:color", VtValue(GfVec3f(0.25F, 0.5F, 1.0F)));
  Set(a, "inputs:intensity", VtValue(2.0F));
  Set(a, "inputs:exposure", VtValue(1.0F));
  verify("coloured HDR dome", {1, 2, 4});
  Check(first.scene->environment == std::array<float, 3>{1, 1, 1},
      "a light edit modified an immutable snapshot");
  Set(a, "inputs:exposure", VtValue(-1.0F));
  verify("negative exposure", {0.25F, 0.5F, 1});
  const auto b = stage->DefinePrim(SdfPath("/Lights/B"), TfToken("DomeLight"));
  Set(b, "inputs:color", VtValue(GfVec3f(0.5F, 0.25F, 0.125F)));
  verify("additive domes", {0.75F, 0.75F, 1.125F});
  const auto lights = stage->GetPrimAtPath(SdfPath("/Lights"));
  Set(lights, "visibility", VtValue(TfToken("invisible")));
  verify("inherited invisibility", {0, 0, 0});
  Set(lights, "visibility", VtValue(TfToken("inherited")));
  verify("restore parent visibility", {0.75F, 0.75F, 1.125F});
  Set(a, "visibility", VtValue(TfToken("invisible")));
  verify("individual invisibility", {0.5F, 0.25F, 0.125F});
  stage->RemovePrim(b.GetPath());
  verify("remove visible dome", {0, 0, 0});
  Set(a, "visibility", VtValue(TfToken("inherited")));
  Set(a, "inputs:intensity", VtValue(0.0F));
  verify("zero intensity", {0, 0, 0}, false);
  Set(a, "inputs:intensity", VtValue(-1.0F));
  verify("negative intensity", {0, 0, 0}, false);
  Set(a, "inputs:intensity", VtValue(2.0F));
  Set(a, "inputs:color", VtValue(GfVec3f(-1, 0.5F, 1)));
  verify("negative channel", {0, 0.5F, 1});
  if (!gpu) {
    Set(a, "inputs:intensity", VtValue(std::numeric_limits<float>::infinity()));
    verify("non-finite intensity", {0, 0, 0});
    Set(a, "inputs:intensity", VtValue(1.0F));
    Set(a, "inputs:exposure", VtValue(std::numeric_limits<float>::quiet_NaN()));
    verify("non-finite exposure", {0, 0, 0});
    Set(a, "inputs:exposure", VtValue(0.0F));
    Set(a, "inputs:color", VtValue(GfVec3f(0, 0, std::numeric_limits<float>::quiet_NaN())));
    verify("non-finite colour", {0, 0, 0});
    Set(a, "inputs:color", VtValue(GfVec3f(1)));
    Set(a, "inputs:exposure", VtValue(200.0F));
    verify("overflowing radiance", {0, 0, 0});
    Set(a, "inputs:color", VtValue(GfVec3f(0)));
    Set(a, "inputs:exposure", VtValue(2000.0F));
    verify("zero times overflowing exposure", {0, 0, 0});
    Set(a, "inputs:color", VtValue(GfVec3f(1)));
    Set(a, "inputs:exposure", VtValue(-2000.0F));
    verify("underflowing radiance", {0, 0, 0});
  }
  stage->RemovePrim(a.GetPath());
  verify("remove last dome", {1, 1, 1});
  verify("unchanged frame", {1, 1, 1}, false);
  Check(delegate.GetFrameSnapshot().scene->meshes.at("/Panel").geometry == geometry,
      "dome edits replaced mesh geometry");
  if (gpu) {
    const auto final_stats = delegate.GetGpuSceneStats();
    Check(final_stats.geometry_uploads == stats.geometry_uploads &&
              final_stats.blas_builds == stats.blas_builds &&
              final_stats.blas_updates == stats.blas_updates &&
              final_stats.tlas_builds == stats.tlas_builds &&
              final_stats.tlas_updates == stats.tlas_updates,
        "environment edits uploaded geometry or rebuilt acceleration structures");
  }
  std::cout << "Hydra dome " << (gpu ? "GPU" : "CPU") << " checks passed\n";
  return 0;
}
} // namespace

int main(int argc, char** argv) try {
  const bool gpu = argc == 2 && std::string_view(argv[1]) == "--gpu";
  if (gpu) {
    const auto shaders = std::filesystem::absolute(argv[0]).parent_path() / "shaders";
    Lotus::FrameStatus status = Lotus::FrameStatus::Fail;
    std::string error;
    const auto probe = Lotus::CreateOffscreenRenderer(
        (shaders / "triangle.vert.spv").string(),
        (shaders / "triangle.frag.spv").string(), status, error);
    if (!probe) {
      std::cerr << error << '\n';
      return status == Lotus::FrameStatus::Skip ? 77 : 1;
    }
    if (const auto capability = probe->RayQueryCapability(); !capability.available) {
      std::cerr << "ray queries are unavailable: " << capability.detail << '\n';
      return 77;
    }
  }
  return Run(gpu);
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
