// SPDX-License-Identifier: Apache-2.0
// Textured dome lights through UsdImaging. CPU mode checks the environment
// image a dome becomes: OpenUSD's Stinson Beach image decoded by Hio as the
// headless checks decode it, its scale, its frame (DomeLight_1's pole axis
// included), edits that keep the decoded image, the domes and formats that
// light uniformly instead, and the fallback image: the packaged Stinson Beach
// under a scene with no lighting, beside a host's light. GPU mode reflects
// the regions of a synthetic image that the OpenEXR latitude-longitude layout
// puts in each direction off a mirror.
#include "adapter.hpp"
#include "common/reference.hpp"

#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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

using Rgb = std::array<float, 3>;
// The synthetic image's regions, by the OpenEXR layout: its columns 3-4 face
// longitude 0 (+Z), 1-2 +pi/2 (+X), 5-6 -pi/2 (-X), 0 and 7 pi (-Z) in its
// middle rows; its top row is the +Y pole, its bottom row -Y. Every value
// is exact in RGBE.
const Rgb kTowardZ{0.25F, 0.5F, 1};
const Rgb kTowardX{2, 0.5F, 0.25F};
const Rgb kTowardMinusX{0.5F, 2, 0.5F};
const Rgb kTowardMinusZ{1, 1, 0.25F};
const Rgb kTop{4, 0.25F, 0.25F};
const Rgb kBottom{0.25F, 0.25F, 4};

void Check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

// Writes the 8x4 synthetic image as flat (uncompressed) Radiance RGBE.
void WriteRegions(const std::filesystem::path& path) {
  const std::array<Rgb, 8> columns{kTowardMinusZ, kTowardX, kTowardX,
      kTowardZ, kTowardZ, kTowardMinusX, kTowardMinusX, kTowardMinusZ};
  std::ofstream output(path, std::ios::binary);
  output << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 4 +X 8\n";
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 8; ++column) {
      const Rgb& rgb = row == 0 ? kTop : row == 3 ? kBottom : columns[column];
      const float largest = std::max({rgb[0], rgb[1], rgb[2]});
      int exponent = 0;
      std::frexp(largest, &exponent);
      // largest = m * 2^exponent, m in [0.5, 1): bytes m * 256.
      const float factor = std::ldexp(1.0F, 8 - exponent);
      for (const float value : rgb)
        output.put(static_cast<char>(static_cast<int>(value * factor)));
      output.put(static_cast<char>(exponent + 128));
    }
  }
  Check(bool(output), "could not write the synthetic image");
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

// Hands a light over as HdxTaskController does, with a value under `params`.
class HostLights final : public HdSceneDelegate {
public:
  using HdSceneDelegate::HdSceneDelegate;
  VtValue Get(const SdfPath&, const TfToken& key) override {
    return key == HdLightTokens->params ? VtValue(1) : VtValue();
  }
  bool GetVisible(const SdfPath&) override { return true; }
  GfMatrix4d GetTransform(const SdfPath&) override { return GfMatrix4d(1.0); }
};

void Set(const UsdPrim& prim, const char* name, const VtValue& value) {
  Check(prim.GetAttribute(TfToken(name)).Set(value),
      std::string("could not set ") + name);
}

// A dome light of `type` under /Lights reading `file`.
UsdPrim Dome(const UsdStagePtr& stage, const char* name, const char* type,
    const std::filesystem::path& file) {
  const UsdPrim dome =
      stage->DefinePrim(SdfPath("/Lights").AppendChild(TfToken(name)), TfToken(type));
  Check(bool(dome), std::string("could not define ") + name);
  Set(dome, "inputs:texture:file", VtValue(SdfAssetPath(file.generic_string())));
  return dome;
}

// The world-space direction the frame's image +Y axis (its top pole) has.
std::array<float, 3> Pole(const Lotus::Matrix4& frame) {
  const auto axes = *Lotus::EnvironmentFrame(frame);
  return {static_cast<float>(axes[1][0]), static_cast<float>(axes[1][1]),
      static_cast<float>(axes[1][2])};
}

bool Near(const std::array<float, 3>& a, const std::array<float, 3>& b) {
  for (int c = 0; c < 3; ++c)
    if (!(std::abs(a[c] - b[c]) < 1e-6F)) return false;
  return true;
}

int Run(bool gpu, const std::filesystem::path& assets,
    const std::filesystem::path& scratch) {
  const auto layer = SdfLayer::CreateAnonymous(".usda");
  Check(layer->ImportFromString(kStage), "could not parse the stage");
  const auto stage = UsdStage::Open(layer);
  Check(bool(stage), "could not open the stage");
  HdRenderSettingsMap settings;
  settings[HdRenderSettingsTokens->convergedSamplesPerPixel] = VtValue(kSamples);
  settings[TfToken("lotus:sampleIndex")] = VtValue(123);
  // BSDF sampling reflects the mirror exactly in each sample; light
  // sampling shares the near-mirror's lobe tails with light samples.
  settings[TfToken("lotus:lightSampling")] = VtValue(false);
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

  if (!gpu) {
    // A textured dome becomes the environment image: Hio's decoding of the
    // committed image equals the headless checks', texel for texel.
    const std::filesystem::path beach_file = assets / "StinsonBeach.hdr";
    Lotus::Texture beach;
    const std::string read = LotusHeadless::ReadRadianceHdr(beach_file, beach);
    Check(read.empty(), read);
    const UsdPrim a = Dome(stage, "A", "DomeLight", beach_file);
    Set(a, "inputs:intensity", VtValue(2.0F));
    tick();
    auto snapshot = delegate.GetFrameSnapshot();
    Check(snapshot.scene->environment == Rgb{0, 0, 0},
        "a textured dome added constant radiance");
    Check(snapshot.scene->environment_map.has_value(),
        "a textured dome made no environment image");
    const Lotus::EnvironmentMap map = *snapshot.scene->environment_map;
    Check(map.scale == Rgb{2, 2, 2}, "the image's scale is not the radiance");
    Check(map.world_from_environment == Lotus::IdentityMatrix(),
        "an untransformed dome has a frame");
    const auto texture = snapshot.scene->textures.at(map.texture);
    Check(texture->width == 1024 && texture->height == 512 &&
              texture->format == Lotus::TextureFormat::Rgba32Float,
        "the dome texture has the wrong size or format");
    Check(texture->texels == beach.texels,
        "Hio and the headless reader decode Stinson Beach differently");

    // Edits keep the decoded image; the transform orients it.
    Set(a, "inputs:color", VtValue(GfVec3f(1, 0.5F, 0.25F)));
    UsdGeomXformable(a).AddRotateXOp().Set(90.0F);
    tick();
    snapshot = delegate.GetFrameSnapshot();
    Check(snapshot.scene->textures.at(map.texture) == texture,
        "a colour or transform edit decoded the image again");
    Check(snapshot.scene->environment_map->scale == Rgb{2, 1, 0.5F},
        "a colour edit did not scale the image");
    Check(Near(Pole(snapshot.scene->environment_map->world_from_environment),
              {0, 0, 1}),
        "rotating the dome 90 degrees about X did not turn its pole to +Z");

    // A hidden dome keeps its image resident but contributes nothing.
    Set(a, "visibility", VtValue(TfToken("invisible")));
    tick();
    snapshot = delegate.GetFrameSnapshot();
    Check(!snapshot.scene->environment_map &&
              snapshot.scene->textures.at(map.texture) == texture,
        "a hidden dome lit the scene or lost its image");
    Set(a, "visibility", VtValue(TfToken("inherited")));

    // Only the first textured dome in path order is the image; a second
    // one is left out with a warning, and an untextured one adds its
    // constant radiance.
    const UsdPrim b = Dome(stage, "B", "DomeLight", beach_file);
    const UsdPrim c = stage->DefinePrim(SdfPath("/Lights/C"), TfToken("DomeLight"));
    Set(c, "inputs:color", VtValue(GfVec3f(0.125F, 0.25F, 0.5F)));
    tick();
    snapshot = delegate.GetFrameSnapshot();
    Check(snapshot.scene->environment_map &&
              snapshot.scene->environment_map->texture == map.texture,
        "the first textured dome is not the image");
    Check(snapshot.scene->environment == Rgb{0.125F, 0.25F, 0.5F},
        "the second textured dome or the untextured one changed the constant");
    stage->RemovePrim(a.GetPath());
    tick();
    snapshot = delegate.GetFrameSnapshot();
    Check(snapshot.scene->environment_map &&
              snapshot.scene->environment_map->texture != map.texture &&
              !snapshot.scene->textures.contains(map.texture),
        "removing the first textured dome did not hand the image to the next");
    stage->RemovePrim(b.GetPath());
    stage->RemovePrim(c.GetPath());

    // DomeLight_1's pole axis turns the image's pole to Z through
    // domeOffset, which Lotus applies before the dome's transform.
    const UsdPrim d = Dome(stage, "D", "DomeLight_1", beach_file);
    Set(d, "poleAxis", VtValue(TfToken("Z")));
    tick();
    snapshot = delegate.GetFrameSnapshot();
    Check(snapshot.scene->environment_map &&
              Near(Pole(snapshot.scene->environment_map->world_from_environment),
                  {0, 0, 1}),
        "DomeLight_1 with poleAxis Z did not turn the image's pole to +Z");
    stage->RemovePrim(d.GetPath());

    // Formats Lotus does not read, and files it cannot, light uniformly
    // with the dome's radiance, with a warning.
    const UsdPrim e = Dome(stage, "E", "DomeLight", beach_file);
    Set(e, "inputs:texture:format", VtValue(TfToken("mirroredBall")));
    tick();
    snapshot = delegate.GetFrameSnapshot();
    Check(!snapshot.scene->environment_map &&
              snapshot.scene->environment == Rgb{1, 1, 1} &&
              snapshot.scene->textures.empty(),
        "a mirroredBall texture was read");
    Set(e, "inputs:texture:format", VtValue(TfToken("latlong")));
    tick();
    Check(delegate.GetFrameSnapshot().scene->environment_map.has_value(),
        "an explicit latlong texture was not read");
    Set(e, "inputs:texture:file",
        VtValue(SdfAssetPath((scratch / "missing.hdr").generic_string())));
    tick();
    snapshot = delegate.GetFrameSnapshot();
    Check(!snapshot.scene->environment_map &&
              snapshot.scene->environment == Rgb{1, 1, 1} &&
              snapshot.scene->textures.empty(),
        "a missing texture did not leave the dome uniform");
    stage->RemovePrim(e.GetPath());
    tick();
    snapshot = delegate.GetFrameSnapshot();
    Check(!snapshot.scene->environment_map &&
              snapshot.scene->environment == Rgb{1, 1, 1},
        "removing the last dome did not restore the fallback environment");

    // The packaged image is the committed validation image, and it replaces
    // the white fallback environment, unturned and unscaled.
    const std::filesystem::path packaged =
        HdLotusRenderDelegate::PackagedFallbackEnvironment();
    Lotus::Texture packaged_texels;
    const std::string packaged_read =
        LotusHeadless::ReadRadianceHdr(packaged, packaged_texels);
    Check(packaged_read.empty(), packaged_read);
    Check(packaged_texels.texels == beach.texels,
        "the packaged fallback environment is not Stinson Beach");
    delegate.SetFallbackEnvironment(packaged);
    snapshot = delegate.GetFrameSnapshot();
    Check(snapshot.scene->environment == Rgb{0, 0, 0} &&
              snapshot.scene->environment_map.has_value(),
        "the fallback image did not replace the white environment");
    const Lotus::EnvironmentMap fallback = *snapshot.scene->environment_map;
    Check(fallback.scale == Rgb{1, 1, 1} &&
              fallback.world_from_environment == Lotus::IdentityMatrix() &&
              snapshot.scene->textures.at(fallback.texture)->texels == beach.texels,
        "the fallback image is scaled, turned or differently decoded");

    // An authored dome replaces it, and its image leaves the scene.
    const UsdPrim f = stage->DefinePrim(SdfPath("/Lights/F"), TfToken("DomeLight"));
    tick();
    snapshot = delegate.GetFrameSnapshot();
    Check(!snapshot.scene->environment_map &&
              snapshot.scene->environment == Rgb{1, 1, 1} &&
              !snapshot.scene->textures.contains(fallback.texture),
        "an untextured dome did not replace the fallback image");
    stage->RemovePrim(f.GetPath());
    tick();
    Check(delegate.GetFrameSnapshot().scene->environment_map.has_value(),
        "removing the dome did not restore the fallback image");

    // So does an authored analytic light, even a hidden one.
    const UsdPrim g = stage->DefinePrim(SdfPath("/Lights/G"), TfToken("SphereLight"));
    Set(g, "visibility", VtValue(TfToken("invisible")));
    tick();
    snapshot = delegate.GetFrameSnapshot();
    Check(!snapshot.scene->environment_map &&
              snapshot.scene->environment == Rgb{0, 0, 0} &&
              !snapshot.scene->textures.contains(fallback.texture),
        "a scene light did not replace the fallback image");
    stage->RemovePrim(g.GetPath());
    tick();
    Check(delegate.GetFrameSnapshot().scene->environment_map.has_value(),
        "removing the scene light did not restore the fallback image");

    // A host's light adds to the image.
    HostLights host(index.get(), SdfPath("/Host"));
    const SdfPath host_light("/Host/CameraLight");
    index->InsertSprim(HdPrimTypeTokens->distantLight, &host, host_light);
    tick();
    snapshot = delegate.GetFrameSnapshot();
    Check(snapshot.scene->lights.size() == 1,
        "the host's light did not become a light");
    Check(snapshot.scene->environment_map &&
              snapshot.scene->environment_map->texture == fallback.texture,
        "the host's light replaced the fallback image");
    index->RemoveSprim(HdPrimTypeTokens->distantLight, host_light);
    tick();
    snapshot = delegate.GetFrameSnapshot();
    Check(snapshot.scene->lights.empty() && snapshot.scene->environment_map,
        "removing the host's light left it or lost the fallback image");

    // An image that cannot be read leaves the fallback white.
    delegate.SetFallbackEnvironment(scratch / "missing.hdr");
    snapshot = delegate.GetFrameSnapshot();
    Check(!snapshot.scene->environment_map &&
              snapshot.scene->environment == Rgb{1, 1, 1} &&
              snapshot.scene->textures.empty(),
        "an unreadable fallback image did not leave the fallback white");
    std::cout << "Hydra dome texture CPU checks passed\n";
    return 0;
  }

  // GPU: the mirror reflects the camera's rays about +Z, into the region of
  // the image each frame turns there.
  const std::filesystem::path regions = scratch / "lotus-dome-regions.hdr";
  WriteRegions(regions);
  const auto verify = [&](const std::string& step, const Rgb& expected) {
    for (int sample = 1; sample <= kSamples; ++sample) {
      tick();
      const bool converged = sample == kSamples;
      Check(pass->IsConverged() == converged && color.IsConverged() == converged,
          step + ": accumulation did not restart/converge as expected");
    }
    const auto* pixels = static_cast<const float*>(color.Map());
    bool matches = pixels != nullptr;
    if (pixels) {
      for (int p = 0; p < kSize * kSize; ++p) {
        for (int c = 0; c < 3; ++c) {
          matches = matches && std::isfinite(pixels[p * 4 + c]) &&
              std::abs(pixels[p * 4 + c] - expected[c]) < 1.0e-5F * (1 + expected[c]);
        }
        matches = matches && pixels[p * 4 + 3] == 1.0F;
      }
    }
    color.Unmap();
    Check(matches, step + ": the mirror does not reflect the expected region");
  };
  const auto scaled = [](const Rgb& rgb, const Rgb& scale) {
    return Rgb{rgb[0] * scale[0], rgb[1] * scale[1], rgb[2] * scale[2]};
  };
  verify("no domes", {1, 1, 1});
  const auto stats = delegate.GetGpuSceneStats();
  const UsdPrim a = Dome(stage, "A", "DomeLight", regions);
  verify("untransformed dome", kTowardZ);
  Set(a, "inputs:color", VtValue(GfVec3f(1, 0.5F, 2)));
  verify("coloured dome", scaled(kTowardZ, {1, 0.5F, 2}));
  auto rotate = UsdGeomXformable(a).AddRotateYOp();
  rotate.Set(90.0F);
  verify("dome turned 90 degrees about Y", scaled(kTowardMinusX, {1, 0.5F, 2}));
  rotate.Set(-90.0F);
  verify("dome turned -90 degrees about Y", scaled(kTowardX, {1, 0.5F, 2}));
  rotate.Set(180.0F);
  verify("dome turned 180 degrees about Y", scaled(kTowardMinusZ, {1, 0.5F, 2}));
  rotate.Set(0.0F);
  const UsdPrim c = stage->DefinePrim(SdfPath("/Lights/C"), TfToken("DomeLight"));
  Set(c, "inputs:color", VtValue(GfVec3f(0.5F, 0.25F, 0.125F)));
  verify("textured and uniform domes", {0.25F + 0.5F, 0.25F + 0.25F, 2 + 0.125F});
  stage->RemovePrim(c.GetPath());
  stage->RemovePrim(a.GetPath());
  const UsdPrim d = Dome(stage, "D", "DomeLight_1", regions);
  Set(d, "poleAxis", VtValue(TfToken("Z")));
  verify("DomeLight_1 with its pole on Z", kTop);
  Set(d, "poleAxis", VtValue(TfToken("Y")));
  verify("DomeLight_1 with its pole on Y", kTowardZ);
  stage->RemovePrim(d.GetPath());
  verify("no domes again", {1, 1, 1});
  const auto final_stats = delegate.GetGpuSceneStats();
  Check(final_stats.geometry_uploads == stats.geometry_uploads &&
            final_stats.blas_builds == stats.blas_builds &&
            final_stats.tlas_builds == stats.tlas_builds &&
            final_stats.tlas_updates == stats.tlas_updates,
      "dome edits uploaded geometry or rebuilt acceleration structures");
  Check(final_stats.environment_writes > stats.environment_writes,
      "dome images never reached the GPU scene");
  std::cout << "Hydra dome texture GPU checks passed\n";
  return 0;
}
} // namespace

int main(int argc, char** argv) try {
  const bool gpu = argc == 2 && std::string_view(argv[1]) == "--gpu";
  const auto directory = std::filesystem::absolute(argv[0]).parent_path();
  if (gpu) {
    const auto shaders = Lotus::ShadersIn(directory / "shaders");
    Lotus::FrameStatus status = Lotus::FrameStatus::Fail;
    std::string error;
    const auto probe = Lotus::CreateOffscreenRenderer(
        shaders.vertex, shaders.fragment, status, error);
    if (!probe) {
      std::cerr << error << '\n';
      return status == Lotus::FrameStatus::Skip ? 77 : 1;
    }
    if (const auto capability = probe->RayQueryCapability(); !capability.available) {
      std::cerr << "ray queries are unavailable: " << capability.detail << '\n';
      return 77;
    }
  }
  return Run(gpu, LOTUS_ASSETS_DIRECTORY, directory);
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
