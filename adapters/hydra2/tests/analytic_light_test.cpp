// SPDX-License-Identifier: Apache-2.0
// Analytic UsdLux lights through Hydra. Syncs sphere, rect, disk and
// distant lights through UsdImaging and checks their Lotus lights: shape,
// transform, radiance and size parameters, edits, visibility, invalid
// values, and that any analytic light replaces the white fallback
// environment. GPU mode renders them under both estimators and requires the
// backend's deterministic images bit for bit, the two estimators' images
// to agree statistically, and a light edit to restart the accumulation
// without uploading geometry or building acceleration structures.
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
#include <pxr/usd/sdf/copyUtils.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/xformable.h>
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
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// A floor and a back wall under four lights: a normalized sphere, a rect
// and a normalized disk facing down, and a 2-degree distant light at 60
// degrees, all of them in /Lights.
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
}

def Xform "Lights"
{
    def SphereLight "Sphere"
    {
        color3f inputs:color = (1, 0.5, 0.25)
        float inputs:exposure = 1
        float inputs:intensity = 3
        bool inputs:normalize = 1
        float inputs:radius = 0.25
        double3 xformOp:translate = (0.2, 1.5, 0.3)
        float3 xformOp:scale = (2, 2, 2)
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]
    }

    def RectLight "Rect"
    {
        float inputs:intensity = 4
        float inputs:width = 0.5
        float inputs:height = 0.25
        double3 xformOp:translate = (-0.5, 1.8, 0)
        float xformOp:rotateX = -90
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateX"]
    }

    def DiskLight "Disk"
    {
        color3f inputs:color = (0.5, 1, 1)
        float inputs:intensity = 2
        bool inputs:normalize = 1
        float inputs:radius = 0.3
        double3 xformOp:translate = (0.6, 1.6, 0.2)
        float xformOp:rotateX = -90
        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateX"]
    }

    def DistantLight "Sun"
    {
        float inputs:angle = 2
        color3f inputs:color = (1, 0.9, 0.8)
        float inputs:intensity = 0.5
        float xformOp:rotateX = -60
        uniform token[] xformOpOrder = ["xformOp:rotateX"]
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

void Set(const UsdPrim& prim, const char* name, const VtValue& value) {
  Check(prim.GetAttribute(TfToken(name)).Set(value),
      std::string("could not set ") + name);
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

using Image = std::vector<float>;

// Bitwise, so that -0 and NaN count as differences too.
bool Same(const Image& a, const Image& b) {
  return a.size() == b.size() &&
         std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

// The stage synced into a render index with its own Lotus render delegate,
// rendered through one pass, into a kSize x kSize float colour AOV when
// `gpu`.
class Session {
public:
  Session(const UsdStageRefPtr& stage, const HdRenderSettingsMap& settings,
      bool gpu)
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
    HdRenderPassStateSharedPtr state;
    if (gpu) {
      Check(color_.Allocate(GfVec3i(kSize, kSize, 1), HdFormatFloat32Vec4, false),
          "could not allocate the colour buffer");
      state = std::make_shared<HdRenderPassState>();
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
    }
    tasks_.push_back(std::make_shared<RenderTask>(pass_, state));
  }

  HdLotusRenderDelegate& Delegate() {
    return delegate_;
  }

  HdRenderIndex& Index() {
    return *index_;
  }

  void Tick() {
    scene_indices_.stageSceneIndex->ApplyPendingUpdates();
    engine_.Execute(index_.get(), &tasks_);
  }

  // Renders until the colour converges, and checks that it took exactly
  // kSamples passes, the first of which started a fresh accumulation.
  Image Converge(const std::string& step) {
    for (int pass = 1; pass <= kSamples; ++pass) {
      Tick();
      const std::string at = step + ": pass " + std::to_string(pass);
      const bool converged = pass_->IsConverged() && color_.IsConverged();
      Check(converged == (pass == kSamples),
          at + ": convergence is " + (converged ? "early" : "missing"));
      const Lotus::GpuFrameEvidence frame = delegate_.GetFrameEvidence();
      Check(frame.status == Lotus::FrameStatus::Pass &&
                frame.samples_per_pixel == std::uint32_t(pass),
          at + ": the frame holds " +
              std::to_string(frame.samples_per_pixel) + " samples");
    }
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

// The backend's deterministic image of `snapshot` under `light_sampling`,
// rendered in one call by a renderer of its own, rows flipped as Hydra
// stores them.
Image BackendImage(const std::filesystem::path& shaders,
    const Lotus::FrameSnapshot& snapshot, Lotus::LightSampling light_sampling) {
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
  settings.light_sampling = light_sampling;
  settings.sample_index = kSampleIndex;
  settings.max_samples = kSamples;
  const Lotus::GpuFrameEvidence frame = renderer->RenderScene(
      Lotus::ExtractDrawSummary(snapshot), target, kSamples, settings);
  Check(frame.status == Lotus::FrameStatus::Pass &&
            frame.samples_per_pixel == kSamples &&
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
// agrees within five standard errors of the pixel differences' spread.
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

// The light the stage's prim should become: its composed transform as the
// adapter converts it, and the given parameters.
Lotus::Light Expected(const UsdStageRefPtr& stage, const char* path,
    Lotus::LightShape shape, const std::array<float, 3>& radiance) {
  const UsdPrim prim = stage->GetPrimAtPath(SdfPath(path));
  const GfMatrix4d transform =
      UsdGeomXformable(prim).ComputeLocalToWorldTransform(UsdTimeCode::Default());
  Lotus::Light light;
  light.shape = shape;
  for (std::size_t index = 0; index < 16; ++index)
    light.world_from_light[index] = static_cast<float>(transform.data()[index]);
  light.radiance = radiance;
  return light;
}

// The scene's light at `path` equals `expected` to float rounding of the
// radiance; nothing at `path` when `expected` is empty.
void CheckLight(Session& session, const std::string& step, const char* path,
    const std::optional<Lotus::Light>& expected) {
  const auto scene = session.Delegate().GetFrameSnapshot().scene;
  const auto found = scene->lights.find(path);
  if (!expected) {
    Check(found == scene->lights.end(), step + ": " + path + " is still a light");
    return;
  }
  Check(found != scene->lights.end(), step + ": " + path + " is not a light");
  const Lotus::Light& light = found->second;
  bool radiance = true;
  for (std::size_t c = 0; c < 3; ++c)
    radiance = radiance && std::abs(light.radiance[c] - expected->radiance[c]) <=
                               1e-6F * std::abs(expected->radiance[c]);
  Check(radiance, step + ": " + path + " has the wrong radiance");
  Lotus::Light exact = light;
  exact.radiance = expected->radiance;
  Check(exact == *expected, step + ": " + path + " has the wrong parameters");
}

void CheckEnvironment(Session& session, const std::string& step,
    const std::array<float, 3>& expected) {
  Check(session.Delegate().GetFrameSnapshot().scene->environment == expected,
      step + ": wrong environment radiance");
}

int RunCpu(const UsdStageRefPtr& stage) {
  // The lights' declarations, removed for the first frame.
  const SdfLayerRefPtr lights_layer = SdfLayer::CreateAnonymous(".usda");
  Check(lights_layer->ImportFromString(kStage), "could not parse the stage");
  stage->RemovePrim(SdfPath("/Lights"));
  Session session(stage, {}, false);
  session.Tick();
  Check(session.Delegate().GetFrameSnapshot().scene->lights.empty(),
      "no lights: the scene has lights");
  CheckEnvironment(session, "no lights", {1, 1, 1});

  Check(SdfCopySpec(lights_layer, SdfPath("/Lights"), stage->GetRootLayer(),
            SdfPath("/Lights")),
      "could not add the lights");
  session.Tick();
  for (const auto& [type, path] :
      {std::pair{HdPrimTypeTokens->sphereLight, "/Lights/Sphere"},
          std::pair{HdPrimTypeTokens->rectLight, "/Lights/Rect"},
          std::pair{HdPrimTypeTokens->diskLight, "/Lights/Disk"},
          std::pair{HdPrimTypeTokens->distantLight, "/Lights/Sun"}})
    Check(session.Index().GetSprim(type, SdfPath(path)) != nullptr,
        std::string(path) + " was not populated through UsdImaging");
  Lotus::Light sphere = Expected(stage, "/Lights/Sphere",
      Lotus::LightShape::Sphere, {6, 3, 1.5F});
  sphere.normalize = true;
  sphere.radius = 0.25F;
  Lotus::Light rect = Expected(stage, "/Lights/Rect", Lotus::LightShape::Rect,
      {4, 4, 4});
  rect.width = 0.5F;
  rect.height = 0.25F;
  Lotus::Light disk = Expected(stage, "/Lights/Disk", Lotus::LightShape::Disk,
      {1, 2, 2});
  disk.normalize = true;
  disk.radius = 0.3F;
  Lotus::Light sun = Expected(stage, "/Lights/Sun", Lotus::LightShape::Distant,
      {0.5F, 0.45F, 0.4F});
  sun.angle = 2;
  CheckLight(session, "four lights", "/Lights/Sphere", sphere);
  CheckLight(session, "four lights", "/Lights/Rect", rect);
  CheckLight(session, "four lights", "/Lights/Disk", disk);
  CheckLight(session, "four lights", "/Lights/Sun", sun);
  CheckEnvironment(session, "four lights", {0, 0, 0});
  const auto first = session.Delegate().GetFrameSnapshot();

  // Fallback sprims add nothing.
  session.Delegate().DestroySprim(
      session.Delegate().CreateFallbackSprim(HdPrimTypeTokens->sphereLight));
  session.Tick();
  Check(session.Delegate().GetFrameSnapshot().scene == first.scene,
      "a fallback sprim or an unchanged sync mutated the scene");

  // Edits, with the earlier snapshot untouched.
  const UsdPrim sphere_prim = stage->GetPrimAtPath(SdfPath("/Lights/Sphere"));
  Set(sphere_prim, "inputs:intensity", VtValue(6.0F));
  const UsdPrim rect_prim = stage->GetPrimAtPath(SdfPath("/Lights/Rect"));
  Set(rect_prim, "xformOp:translate", VtValue(GfVec3d(-0.25, 1.7, 0.1)));
  const UsdPrim sun_prim = stage->GetPrimAtPath(SdfPath("/Lights/Sun"));
  Check(sun_prim.GetAttribute(TfToken("inputs:intensity")).Clear(),
      "could not clear the sun's intensity");
  session.Tick();
  sphere.radiance = {12, 6, 3};
  CheckLight(session, "edits", "/Lights/Sphere", sphere);
  const Lotus::Light moved = Expected(stage, "/Lights/Rect",
      Lotus::LightShape::Rect, rect.radiance);
  rect.world_from_light = moved.world_from_light;
  CheckLight(session, "edits", "/Lights/Rect", rect);
  // DistantLight's own default intensity.
  sun.radiance = {50000, 45000, 40000};
  CheckLight(session, "edits", "/Lights/Sun", sun);
  Check(first.scene->lights.at("/Lights/Sphere").radiance ==
            std::array<float, 3>{6, 3, 1.5F},
      "a light edit modified an immutable snapshot");

  // Sizes outside UsdLux's ranges, and values Lotus cannot use.
  Set(rect_prim, "inputs:width", VtValue(-1.0F));
  Set(sun_prim, "inputs:angle", VtValue(400.0F));
  session.Tick();
  rect.width = 0;
  sun.angle = 360;
  CheckLight(session, "clipped sizes", "/Lights/Rect", rect);
  CheckLight(session, "clipped sizes", "/Lights/Sun", sun);
  const UsdPrim disk_prim = stage->GetPrimAtPath(SdfPath("/Lights/Disk"));
  Set(disk_prim, "inputs:intensity",
      VtValue(std::numeric_limits<float>::quiet_NaN()));
  session.Tick();
  CheckLight(session, "non-finite intensity", "/Lights/Disk", std::nullopt);
  Set(disk_prim, "inputs:intensity", VtValue(2.0F));
  Set(disk_prim, "inputs:exposure", VtValue(200.0F));
  session.Tick();
  CheckLight(session, "overflowing radiance", "/Lights/Disk", std::nullopt);
  Set(disk_prim, "inputs:exposure", VtValue(0.0F));
  session.Tick();
  CheckLight(session, "valid again", "/Lights/Disk", disk);

  // Visibility; hidden lights still replace the fallback environment.
  Set(sphere_prim, "visibility", VtValue(TfToken("invisible")));
  session.Tick();
  CheckLight(session, "hidden sphere", "/Lights/Sphere", std::nullopt);
  const UsdPrim lights = stage->GetPrimAtPath(SdfPath("/Lights"));
  Set(lights, "visibility", VtValue(TfToken("invisible")));
  session.Tick();
  Check(session.Delegate().GetFrameSnapshot().scene->lights.empty(),
      "inherited invisibility left lights");
  CheckEnvironment(session, "hidden lights", {0, 0, 0});
  Set(lights, "visibility", VtValue(TfToken("inherited")));
  Set(sphere_prim, "visibility", VtValue(TfToken("inherited")));
  session.Tick();
  CheckLight(session, "visible again", "/Lights/Sphere", sphere);
  CheckLight(session, "visible again", "/Lights/Sun", sun);

  // A dome supplies the environment beside them; removing every analytic
  // light leaves it, and removing it restores the fallback.
  const UsdPrim dome = stage->DefinePrim(SdfPath("/Dome"), TfToken("DomeLight"));
  Set(dome, "inputs:intensity", VtValue(0.5F));
  session.Tick();
  CheckEnvironment(session, "dome and lights", {0.5F, 0.5F, 0.5F});
  stage->RemovePrim(SdfPath("/Lights"));
  session.Tick();
  Check(session.Delegate().GetFrameSnapshot().scene->lights.empty(),
      "removed lights remain");
  CheckEnvironment(session, "dome alone", {0.5F, 0.5F, 0.5F});
  stage->RemovePrim(SdfPath("/Dome"));
  session.Tick();
  CheckEnvironment(session, "no lights again", {1, 1, 1});
  std::cout << "Hydra analytic light CPU checks passed\n";
  return 0;
}

int RunGpu(const UsdStageRefPtr& stage, const std::filesystem::path& shaders) {
  HdRenderSettingsMap settings;
  settings[HdRenderSettingsTokens->convergedSamplesPerPixel] = VtValue(kSamples);
  settings[kSampleIndexSetting] = VtValue(kSampleIndex);
  Session session(stage, settings, true);
  const Image sampled = session.Converge("light sampling");
  const Lotus::FrameSnapshot snapshot = session.Delegate().GetSelectedSnapshot();
  Check(snapshot.scene != nullptr && snapshot.scene->lights.size() == 4,
      "the pass did not trace the stage's four lights");
  const Lotus::GpuSceneStats stats = session.Delegate().GetGpuSceneStats();
  Check(stats.analytic_lights == 4 && stats.emissive_triangles == 0,
      "the GPU scene holds " + std::to_string(stats.analytic_lights) +
          " analytic lights and " + std::to_string(stats.emissive_triangles) +
          " emissive triangles");
  Check(Same(sampled, BackendImage(shaders, snapshot, Lotus::LightSampling::Mis)),
      "Hydra's light-sampled image differs from the backend's");
  session.Delegate().SetRenderSetting(kLightSamplingSetting, VtValue(false));
  const Image brute_force = session.Converge("bsdf sampling");
  Check(Same(brute_force,
            BackendImage(shaders, snapshot, Lotus::LightSampling::Bsdf)),
      "Hydra's BSDF-sampled image differs from the backend's");
  CheckAgreement(sampled, brute_force);

  // A light edit restarts the accumulation and rewrites only the light
  // table.
  session.Delegate().SetRenderSetting(kLightSamplingSetting, VtValue(true));
  session.Converge("light sampling again");
  const Lotus::GpuSceneStats before = session.Delegate().GetGpuSceneStats();
  Set(stage->GetPrimAtPath(SdfPath("/Lights/Rect")), "inputs:intensity",
      VtValue(8.0F));
  const Image brighter = session.Converge("light edit");
  const Lotus::GpuSceneStats after = session.Delegate().GetGpuSceneStats();
  Check(!Same(brighter, sampled), "the light edit did not change the image");
  Check(after.light_writes == before.light_writes + 1 &&
            after.geometry_uploads == before.geometry_uploads &&
            after.instance_writes == before.instance_writes &&
            after.blas_builds == before.blas_builds &&
            after.tlas_builds == before.tlas_builds &&
            after.tlas_updates == before.tlas_updates,
      "a light edit did more than rewrite the light table");
  std::cout << "Hydra analytic light GPU checks passed\n";
  return 0;
}

} // namespace

int main(int argc, char** argv) try {
  const bool gpu = argc == 2 && std::string_view(argv[1]) == "--gpu";
  const auto shaders =
      std::filesystem::absolute(argv[0]).parent_path() / "shaders";
  if (gpu) {
    // Missing GPU capability is a CTest SKIP, never a passing GPU check.
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
  return gpu ? RunGpu(stage, shaders) : RunCpu(stage);
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
