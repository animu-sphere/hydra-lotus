// SPDX-License-Identifier: Apache-2.0
// The UsdPreviewSurface material fixtures (LOTUS-MATERIAL-01) through
// UsdImaging. CPU mode syncs each committed fixture's USD file into the Lotus
// render delegate and requires its scene snapshot to be the headless mirror
// in adapters/common/material_fixtures.cpp: the same materials, bindings,
// triangles, texture coordinates, decoded textures, lights and environment
// exactly, with texture keys' resolved paths taken relative to the
// fixtures' directory, and the same positions, normals, transforms and
// camera to float rounding. GPU mode renders the
// stage through Hydra, under NEE/MIS (the default) and BSDF sampling, from
// the headless check's independent sample index, and requires each image to
// be the backend's image of the mirror bit for bit and to match the committed
// reference the headless check compares with by the DES-Q5 metric.
#include "adapter.hpp"
#include "common/material_fixtures.hpp"
#include "common/reference.hpp"

#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <lotus/extraction.hpp>
#include <lotus/vulkan_backend.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

const std::filesystem::path kFixtures = LOTUS_MATERIAL_FIXTURES_DIRECTORY;
const std::filesystem::path kReferences = LOTUS_REFERENCE_DIRECTORY;
// The headless check's: far from the reference's own samples.
constexpr int kSampleIndex = 1 << 20;
constexpr int kSamples = 256;
// The headless reference's sample count.
constexpr std::uint32_t kReferenceSamples = 4096;
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

// The fixture's stage synced into a render index with its own Lotus render
// delegate and seen by its camera at the reference's size; with `gpu`, a
// pass renders a float colour AOV, and without it syncs the scene only.
class Session {
public:
  Session(const LotusHeadless::MaterialFixture& fixture, const UsdStageRefPtr& stage,
      const HdRenderSettingsMap& settings, bool gpu)
      : size_(fixture.size), delegate_(settings),
        index_(HdRenderIndex::New(&delegate_, {})) {
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
    auto state = std::make_shared<HdRenderPassState>();
    const auto* camera = dynamic_cast<const HdCamera*>(index_->GetSprim(
        HdPrimTypeTokens->camera, SdfPath(std::string(fixture.root) + "/Camera")));
    Check(camera != nullptr, "the fixture's camera is not a Hydra camera");
    state->SetCamera(camera);
    state->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0), size_, size_)));
    if (gpu) {
      Check(color_.Allocate(GfVec3i(size_, size_, 1), HdFormatFloat32Vec4, false),
          "could not allocate the colour buffer");
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

  void Render() {
    scene_indices_.stageSceneIndex->ApplyPendingUpdates();
    engine_.Execute(index_.get(), &tasks_);
  }

  // Renders until the colour converges, at kSamples passes, and returns its
  // RGB with rows from the top, as the reference holds them.
  LotusHeadless::RgbImage Converge() {
    for (int pass = 1; pass <= kSamples; ++pass) {
      Render();
      const bool converged = pass_->IsConverged() && color_.IsConverged();
      Check(converged == (pass == kSamples),
          "convergence after pass " + std::to_string(pass) + " is " +
              (converged ? "early" : "missing"));
    }
    const auto* pixels = static_cast<const float*>(color_.Map());
    Check(pixels != nullptr, "the colour buffer is not allocated");
    LotusHeadless::RgbImage image;
    image.width = image.height = size_;
    for (std::uint32_t y = 0; y < size_; ++y) {
      for (std::uint32_t x = 0; x < size_; ++x) {
        const float* pixel = pixels + (std::size_t{size_ - 1 - y} * size_ + x) * 4;
        Check(pixel[3] == 1.0F, "alpha " + std::to_string(pixel[3]) +
                                    " at pixel " + std::to_string(x) + "," +
                                    std::to_string(y) + ": a sample missed the stage");
        image.values.insert(image.values.end(), pixel, pixel + 3);
      }
    }
    color_.Unmap();
    return image;
  }

private:
  std::uint32_t size_;
  HdLotusRenderDelegate delegate_;
  std::unique_ptr<HdRenderIndex> index_;
  UsdImagingSceneIndices scene_indices_;
  HdRenderPassSharedPtr pass_;
  HdLotusRenderBuffer color_{SdfPath("/color")};
  HdTaskSharedPtrVector tasks_;
  HdEngine engine_;
};

// Float values the adapter composes in double precision and the mirror in
// float, or that the USD file spells in decimal: equal to rounding.
template <std::size_t N>
bool Near(const std::array<float, N>& a, const std::array<float, N>& b) {
  for (std::size_t i = 0; i < N; ++i)
    if (!(std::abs(a[i] - b[i]) <= 2e-6F * std::max(1.0F, std::abs(b[i]))))
      return false;
  return true;
}

template <std::size_t N>
bool Near(const std::vector<std::array<float, N>>& a,
    const std::vector<std::array<float, N>>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (!Near(a[i], b[i])) return false;
  return true;
}

// A texture key as the mirror names it: an existing resolved path relative
// to the fixtures' directory. An unresolved path, or none, is kept.
std::string MirrorKey(const std::string& key) {
  const auto bar = key.rfind('|');
  if (bar == std::string::npos) return key;
  const std::filesystem::path file(key.substr(0, bar));
  std::error_code error;
  if (!file.is_absolute() || !std::filesystem::exists(file, error)) return key;
  const std::filesystem::path relative = std::filesystem::relative(file, kFixtures, error);
  return error ? key : relative.generic_string() + key.substr(bar);
}

Lotus::Material MirrorKeys(Lotus::Material material) {
  for (auto* input : {&material.base_color_texture, &material.roughness_texture,
           &material.metallic_texture, &material.emission_texture,
           &material.normal_texture, &material.opacity_texture,
           &material.specular_color_texture})
    if (*input) (*input)->texture = MirrorKey((*input)->texture);
  return material;
}

// What a mismatched material's lookups name, for the failure message.
std::string Lookups(const Lotus::Material& material) {
  std::string text = "texcoords '" + material.texcoords + "'";
  const char* names[] = {"base colour", "roughness", "metallic", "emission",
      "normal", "opacity", "specular colour"};
  const auto inputs = Lotus::TextureInputs(material);
  for (std::size_t i = 0; i < inputs.size(); ++i)
    if (*inputs[i])
      text += std::string(", ") + names[i] + " " + (*inputs[i])->texture + " channel " +
              std::to_string((*inputs[i])->channel);
  return text;
}

// The stage's snapshot against the mirror's. Returns how many of the float
// values compared to rounding differ at all, for the record.
std::size_t ExpectMirror(const Lotus::FrameSnapshot& stage,
    const Lotus::FrameSnapshot& mirror) {
  const Lotus::LotusScene& got = *stage.scene;
  const Lotus::LotusScene& want = *mirror.scene;
  std::size_t inexact = 0;
  const auto rounded = [&](const auto& a, const auto& b, const std::string& what) {
    Check(Near(a, b), what + " differs from the mirror's beyond rounding");
    inexact += !(a == b);
  };
  rounded(stage.camera.view, mirror.camera.view, "the camera's view");
  rounded(stage.camera.projection, mirror.camera.projection, "the camera's projection");
  Check(got.environment == want.environment && !got.environment_map,
      "the environment differs from the mirror's");
  std::map<std::string, std::shared_ptr<const Lotus::Texture>> textures;
  for (const auto& [key, texture] : got.textures) textures[MirrorKey(key)] = texture;
  std::string keys;
  for (const auto& [key, texture] : textures) keys += " " + key;
  Check(textures.size() == want.textures.size(), "the stage has textures" + keys);
  for (const auto& [key, texture] : want.textures) {
    const auto found = textures.find(key);
    Check(found != textures.end(), "the stage has no texture " + key + "; it has" + keys);
    const Lotus::Texture& decoded = *found->second;
    Check(decoded.width == texture->width && decoded.height == texture->height &&
              decoded.format == texture->format,
        "texture " + key + " is " + std::to_string(decoded.width) + "x" +
            std::to_string(decoded.height) + " in format " +
            std::to_string(static_cast<int>(decoded.format)) + ", the mirror's " +
            std::to_string(static_cast<int>(texture->format)));
    Check(decoded.texels == texture->texels,
        "texture " + key + "'s texels differ from the mirror's");
  }
  Check(got.lights.size() == want.lights.size(), "the lights differ from the mirror's");
  for (const auto& [key, light] : want.lights) {
    const auto found = got.lights.find(key);
    Check(found != got.lights.end(), "the stage has no light " + key);
    Lotus::Light placed = light;
    placed.world_from_light = found->second.world_from_light;
    Check(found->second == placed, "light " + key + " differs from the mirror's");
    rounded(found->second.world_from_light, light.world_from_light,
        "light " + key + "'s transform");
  }
  Check(got.materials.size() == want.materials.size(),
      "the stage has " + std::to_string(got.materials.size()) +
          " materials, the mirror " + std::to_string(want.materials.size()));
  for (const auto& [key, material] : want.materials) {
    const auto found = got.materials.find(key);
    Check(found != got.materials.end(), "the stage has no material " + key);
    Check(MirrorKeys(found->second) == material,
        "material " + key + " differs from the mirror's: " +
            Lookups(MirrorKeys(found->second)) + " against " + Lookups(material));
  }
  Check(got.meshes.size() == want.meshes.size(),
      "the stage has " + std::to_string(got.meshes.size()) +
          " meshes, the mirror " + std::to_string(want.meshes.size()));
  for (const auto& [key, mesh] : want.meshes) {
    const auto found = got.meshes.find(key);
    Check(found != got.meshes.end(), "the stage has no mesh " + key);
    const Lotus::SceneMesh& synced = found->second;
    Check(synced.material == mesh.material, "mesh " + key + " is bound to " +
                                                synced.material + ", not " + mesh.material);
    Check(synced.instance.visible && !synced.instance.instancer_transforms,
        "mesh " + key + " is hidden or instanced");
    rounded(synced.instance.world_from_object, mesh.instance.world_from_object,
        "mesh " + key + "'s transform");
    const Lotus::MeshGeometry& a = *synced.geometry;
    const Lotus::MeshGeometry& b = *mesh.geometry;
    Check(a.triangles == b.triangles && a.source_faces == b.source_faces,
        "mesh " + key + "'s triangles differ from the mirror's");
    Check(a.texcoords == b.texcoords,
        "mesh " + key + "'s texture coordinates differ from the mirror's");
    rounded(a.positions, b.positions, "mesh " + key + "'s positions");
    rounded(a.normals, b.normals, "mesh " + key + "'s normals");
  }
  return inexact;
}

// The backend's deterministic image of the mirror, rendered in one call by a
// renderer of its own as the headless check renders it, RGB with rows from
// the top.
LotusHeadless::RgbImage MirrorImage(const LotusHeadless::MaterialFixture& fixture,
    const Lotus::RendererShaders& shaders, bool mis) {
  const std::uint32_t size = fixture.size;
  Lotus::FrameStatus status = Lotus::FrameStatus::Fail;
  std::string error;
  const auto renderer = Lotus::CreateOffscreenRenderer(
      shaders.vertex, shaders.fragment, status, error, shaders.ray_query);
  Check(renderer != nullptr, "could not create the backend renderer: " + error);
  Lotus::RenderWorld world;
  fixture.set(world);
  const Lotus::FrameSnapshot snapshot = world.Commit();
  Lotus::SceneExtraction extraction;
  const Lotus::GpuSceneEvidence scene =
      renderer->UpdateScene(extraction.Update(snapshot));
  Check(scene.status == Lotus::FrameStatus::Pass,
      "the backend scene update failed: " + scene.detail);
  Lotus::OffscreenTarget target;
  target.width = size;
  target.height = size;
  target.display_window = {0.0F, 0.0F, float(size), float(size)};
  target.data_window = {0, 0, std::int32_t(size), std::int32_t(size)};
  Lotus::PathTracingSettings settings;
  settings.sample_index = kSampleIndex;
  settings.max_samples = kSamples;
  settings.light_sampling = mis ? Lotus::LightSampling::Mis : Lotus::LightSampling::Bsdf;
  const Lotus::GpuFrameEvidence frame = renderer->RenderScene(
      Lotus::ExtractDrawSummary(snapshot), target, kSamples, settings);
  Check(frame.status == Lotus::FrameStatus::Pass &&
            frame.samples_per_pixel == kSamples &&
            frame.color.pixel_format == "rgba32-sfloat" &&
            frame.color.payload.size() == std::size_t{size} * size * 16,
      "the backend render failed: " + frame.detail);
  LotusHeadless::RgbImage image;
  image.width = image.height = size;
  for (std::size_t pixel = 0; pixel < std::size_t{size} * size; ++pixel) {
    float rgba[4];
    std::memcpy(rgba, frame.color.payload.data() + pixel * sizeof(rgba), sizeof(rgba));
    image.values.insert(image.values.end(), rgba, rgba + 3);
  }
  return image;
}

} // namespace

int main(int argc, char** argv) try {
  const bool gpu = argc > 1 && std::string(argv[1]) == "--gpu";
  const auto shaders =
      Lotus::ShadersIn(std::filesystem::absolute(argv[0]).parent_path() / "shaders");
  if (gpu) {
    // Missing GPU capability is a CTest SKIP, never a passing GPU check.
    Lotus::FrameStatus status = Lotus::FrameStatus::Fail;
    std::string error;
    const auto probe = Lotus::CreateOffscreenRenderer(
        shaders.vertex, shaders.fragment, status, error);
    if (!probe) {
      std::cerr << error << '\n';
      return status == Lotus::FrameStatus::Skip ? 77 : 1;
    }
    if (const auto ray_query = probe->RayQueryCapability(); !ray_query.available) {
      std::cerr << "SKIP: ray queries are unavailable: " << ray_query.detail << '\n';
      return 77;
    }
  }

  for (const LotusHeadless::MaterialFixture& fixture : LotusHeadless::MaterialFixtures()) {
    const std::string stem(fixture.id);
    const std::filesystem::path file = kFixtures / (stem + ".usda");
    const UsdStageRefPtr stage = UsdStage::Open(file.string());
    Check(stage != nullptr, "could not open " + file.string());

    if (!gpu) {
      Session session(fixture, stage, {}, false);
      session.Render();
      Lotus::RenderWorld world;
      fixture.set(world);
      const std::size_t inexact =
          ExpectMirror(session.Delegate().GetFrameSnapshot(), world.Commit());
      std::cout << stem << " extracts to its mirror; " << inexact
                << " transform, camera, position or normal sets differ by rounding\n";
      continue;
    }

    LotusHeadless::Reference reference;
    reference.samples = kReferenceSamples;
    for (const auto& [suffix, image] : {std::pair{"-mean.pfm", &reference.mean},
             std::pair{"-variance.pfm", &reference.variance}}) {
      const auto failure = LotusHeadless::ReadPfm(kReferences / (stem + suffix), *image);
      Check(failure.empty(), failure);
    }
    for (const bool mis : {true, false}) {
      HdRenderSettingsMap settings;
      settings[HdRenderSettingsTokens->convergedSamplesPerPixel] = VtValue(kSamples);
      settings[kSampleIndexSetting] = VtValue(kSampleIndex);
      settings[kLightSamplingSetting] = VtValue(mis);
      Session session(fixture, stage, settings, true);
      const LotusHeadless::RgbImage image = session.Converge();
      const std::string name = stem + (mis ? " NEE/MIS" : " BSDF sampling");
      const LotusHeadless::RgbImage mirror = MirrorImage(fixture, shaders, mis);
      Check(std::memcmp(image.values.data(), mirror.values.data(),
                image.values.size() * sizeof(float)) == 0,
          name + ": Hydra's image differs from the backend's image of the mirror");
      const LotusHeadless::Comparison comparison =
          LotusHeadless::Compare(image, kSamples, reference);
      Check(comparison.failure.empty(), name + ": " + comparison.failure);
      std::cout << std::fixed << std::setprecision(2) << name << ' ' << kSamples
                << " spp: the mirror's image bit for bit; image z "
                << comparison.image_z[0] << ' ' << comparison.image_z[1] << ' '
                << comparison.image_z[2] << ", " << comparison.tile
                << "px tiles max |z| " << comparison.max_tile_z << " mean z^2 "
                << comparison.mean_tile_z2 << '\n';
    }
  }
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
