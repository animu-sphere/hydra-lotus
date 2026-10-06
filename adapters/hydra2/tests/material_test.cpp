// SPDX-License-Identifier: Apache-2.0
// Translates hand-built Hydra material networks, then syncs a composed USD
// stage's UsdPreviewSurface materials and bindings through UsdImaging's scene
// indices into the Lotus render delegate and checks the material IR and the
// bindings through edits. With --gpu a render pass also renders a colour AOV
// in which two Lambert materials must show their exact radiance.
#include "adapter.hpp"
#include "material_translator.hpp"

#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <lotus/vulkan_backend.hpp>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// Glow and the unbound quad fill the orthographic view's right and left
// halves; the other meshes lie beside it in the same plane, so no path
// leaving the plane can reach them.
constexpr const char* kStage = R"usda(#usda 1.0
def Scope "Looks"
{
    def Material "Red"
    {
        token outputs:surface.connect = </Looks/Red/Surface.outputs:surface>
        def Shader "Surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0.75, 0.125, 0.0625)
            color3f inputs:emissiveColor = (2, 1, 0.5)
            float inputs:metallic = 0.75
            float inputs:roughness = 0.25
            token outputs:surface
        }
    }

    def Material "Glow"
    {
        token outputs:surface.connect = </Looks/Glow/Surface.outputs:surface>
        def Shader "Surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0.5, 0.25, 0.125)
            color3f inputs:emissiveColor = (1, 2, 3)
            token outputs:surface
        }
    }

    def Material "Clamped"
    {
        token outputs:surface.connect = </Looks/Clamped/Surface.outputs:surface>
        def Shader "Surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (1.5, -0.25, 0.5)
            color3f inputs:emissiveColor = (-1, 3, 0)
            float inputs:metallic = -1
            float inputs:roughness = 2
            token outputs:surface
        }
    }

    def Material "Specular"
    {
        token outputs:surface.connect = </Looks/Specular/Surface.outputs:surface>
        def Shader "Surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0.5, 0.5, 0.5)
            color3f inputs:diffuseColor.connect = </Looks/Specular/Texture.outputs:rgb>
            int inputs:useSpecularWorkflow = 1
            float inputs:metallic = 1
            float inputs:roughness = 0.125
            token outputs:surface
        }
        def Shader "Texture"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @missing.png@
            float3 outputs:rgb
        }
    }

    def Material "Unknown"
    {
        token outputs:surface.connect = </Looks/Unknown/Surface.outputs:surface>
        def Shader "Surface"
        {
            uniform token info:id = "LotusTestUnknownSurface"
            color3f inputs:diffuseColor = (1, 0, 0)
            token outputs:surface
        }
    }
}

def Mesh "Unbound"
{
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-1, -1, 0), (0, -1, 0), (0, 1, 0), (-1, 1, 0)]
}

def Mesh "Glowing" (
    prepend apiSchemas = ["MaterialBindingAPI"]
)
{
    rel material:binding = </Looks/Glow>
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(0, -1, 0), (1, -1, 0), (1, 1, 0), (0, 1, 0)]
}

def Mesh "Red" (
    prepend apiSchemas = ["MaterialBindingAPI"]
)
{
    rel material:binding = </Looks/Red>
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(4, 0, 0), (5, 0, 0), (4, 1, 0)]
}

def Mesh "Clamped" (
    prepend apiSchemas = ["MaterialBindingAPI"]
)
{
    rel material:binding = </Looks/Clamped>
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(6, 0, 0), (7, 0, 0), (6, 1, 0)]
}
)usda";

void Check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string Describe(const Lotus::Material& material) {
  std::ostringstream text;
  text << "base (" << material.base_color[0] << ", " << material.base_color[1]
       << ", " << material.base_color[2] << ") roughness "
       << material.roughness << " metallic " << material.metallic
       << " emission (" << material.emission[0] << ", "
       << material.emission[1] << ", " << material.emission[2] << ")";
  return text.str();
}

void Expect(const std::string& step, const Lotus::Material& actual,
    const Lotus::Material& expected) {
  Check(actual == expected, step + ": got " + Describe(actual) +
                                ", expected " + Describe(expected));
}

Lotus::Material MakeMaterial(std::array<float, 3> base, float roughness,
    float metallic, std::array<float, 3> emission) {
  Lotus::Material material;
  material.base_color = base;
  material.roughness = roughness;
  material.metallic = metallic;
  material.emission = emission;
  return material;
}

// A surface network of one UsdPreviewSurface node with these parameters.
HdMaterialNetworkMap Surface(const TfToken& identifier,
    std::map<TfToken, VtValue> parameters) {
  HdMaterialNode node;
  node.path = SdfPath("/Material/Surface");
  node.identifier = identifier;
  node.parameters = std::move(parameters);
  HdMaterialNetworkMap map;
  map.map[HdMaterialTerminalTokens->surface].nodes.push_back(node);
  map.terminals.push_back(node.path);
  return map;
}

void TranslatorCases() {
  const TfToken preview("UsdPreviewSurface");
  HdLotusMaterialTranslation translation =
      HdLotusTranslateMaterial(HdMaterialNetworkMap{});
  Check(!translation.unsupported.empty() &&
            translation.material == Lotus::Material{},
      "an empty network was not reported as unsupported");

  translation = HdLotusTranslateMaterial(
      Surface(TfToken("ND_standard_surface_surfaceshader"), {}));
  Check(translation.unsupported.find("ND_standard_surface_surfaceshader") !=
            std::string::npos,
      "an unsupported surface shader was not named");

  constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
  constexpr float kInfinity = std::numeric_limits<float>::infinity();
  translation = HdLotusTranslateMaterial(Surface(preview,
      {{TfToken("diffuseColor"), VtValue(GfVec3f(kNaN, 0.5F, 0.5F))},
          {TfToken("emissiveColor"), VtValue(GfVec3d(1e39, -2.0, 0.25))},
          {TfToken("roughness"), VtValue(kInfinity)},
          {TfToken("metallic"), VtValue(0.5)}}));
  Check(translation.unsupported.empty(), "UsdPreviewSurface was unsupported");
  Expect("non-finite and double values", translation.material,
      MakeMaterial({0.18F, 0.18F, 0.18F}, 0.5F, 0.5F,
          {3.4e38F, 0.0F, 0.25F}));

  translation = HdLotusTranslateMaterial(Surface(preview,
      {{TfToken("diffuseColor"), VtValue(GfVec4f(1.0F))},
          {TfToken("roughness"), VtValue(std::string("rough"))}}));
  Expect("values of other types", translation.material, Lotus::Material{});

  // A connection wins over an authored value; the terminal node is last. A
  // texture lookup is translated (texture_test.cpp checks how); a texture
  // without a file is a lookup of no image, which returns its fallback. A
  // colour output cannot drive a scalar, which keeps its default.
  HdMaterialNetworkMap textured =
      Surface(preview, {{TfToken("diffuseColor"), VtValue(GfVec3f(0.5F))},
                           {TfToken("roughness"), VtValue(0.25F)}});
  HdMaterialNetwork& network = textured.map[HdMaterialTerminalTokens->surface];
  HdMaterialNode texture;
  texture.path = SdfPath("/Material/Texture");
  texture.identifier = TfToken("UsdUVTexture");
  network.nodes.insert(network.nodes.begin(), texture);
  network.relationships.push_back({texture.path, TfToken("rgb"),
      SdfPath("/Material/Surface"), TfToken("diffuseColor")});
  network.relationships.push_back({texture.path, TfToken("rgb"),
      SdfPath("/Material/Surface"), TfToken("roughness")});
  translation = HdLotusTranslateMaterial(textured);
  Lotus::Material lookup = Lotus::Material{};
  lookup.base_color_texture =
      Lotus::TextureInput{HdLotusTextureKey("", TfToken("auto"))};
  Expect("connected inputs", translation.material, lookup);
  Check(translation.connected_inputs == std::vector<TfToken>{
                                           TfToken("roughness")} &&
            translation.textures.empty(),
      "the untranslated connection was not reported");
}

// Syncs and executes one render pass.
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
    pass_->Execute(state_, GetRenderTags());
  }
  const TfTokenVector& GetRenderTags() const override {
    static const TfTokenVector tags{HdRenderTagTokens->geometry};
    return tags;
  }

private:
  HdRenderPassSharedPtr pass_;
  HdRenderPassStateSharedPtr state_;
};

constexpr int kSize = 8;

// The colour of column `x` of the middle row.
std::array<float, 3> Pixel(HdLotusRenderBuffer& buffer, int x) {
  const auto* data = static_cast<const float*>(buffer.Map());
  Check(data != nullptr, "could not map the colour buffer");
  std::array<float, 3> rgb{};
  std::memcpy(rgb.data(), data + ((kSize / 2) * kSize + x) * 4, sizeof(rgb));
  buffer.Unmap();
  return rgb;
}

// A Lambert surface alone in the plane, under the adapter's white
// environment: every path scatters once and escapes, so each sample is
// albedo + emission up to rounding.
void ExpectRadiance(const std::string& step, HdLotusRenderBuffer& buffer,
    int x, const std::array<float, 3>& expected) {
  const std::array<float, 3> actual = Pixel(buffer, x);
  for (int c = 0; c < 3; ++c) {
    Check(std::abs(actual[c] - expected[c]) <= 1e-5F * (1.0F + expected[c]),
        step + ": column " + std::to_string(x) + " is (" +
            std::to_string(actual[0]) + ", " + std::to_string(actual[1]) +
            ", " + std::to_string(actual[2]) + "), expected (" +
            std::to_string(expected[0]) + ", " + std::to_string(expected[1]) +
            ", " + std::to_string(expected[2]) + ")");
  }
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
    const auto renderer = Lotus::CreateOffscreenRenderer(
        (shaders / "triangle.vert.spv").string(),
        (shaders / "triangle.frag.spv").string(), status, error);
    if (!renderer) {
      std::cerr << error << '\n';
      return status == Lotus::FrameStatus::Skip ? 77 : 1;
    }
    if (!renderer->RayQueryCapability().available) {
      std::cerr << "SKIP: " << renderer->RayQueryCapability().detail << '\n';
      return 77;
    }
  } else {
    TranslatorCases();
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

  // Without AOV bindings the pass syncs the scene and renders nothing. The
  // identity camera sees x and y in [-1, 1].
  auto state = std::make_shared<HdRenderPassState>();
  HdLotusRenderBuffer color(SdfPath("/color"));
  if (gpu) {
    Check(color.Allocate(GfVec3i(kSize, kSize, 1), HdFormatFloat32Vec4, false),
        "could not allocate the colour buffer");
    HdRenderPassAovBinding binding;
    binding.aovName = HdAovTokens->color;
    binding.renderBuffer = &color;
    binding.clearValue = VtValue(GfVec4f(0.0F));
    state->SetAovBindings({binding});
  }
  HdTaskSharedPtrVector tasks{std::make_shared<RenderTask>(
      delegate.CreateRenderPass(index.get(),
          HdRprimCollection(HdTokens->geometry,
              HdReprSelector(HdReprTokens->smoothHull))),
      state)};
  HdEngine engine;
  const auto sync = [&] {
    scene_indices.stageSceneIndex->ApplyPendingUpdates();
    engine.Execute(index.get(), &tasks);
    return delegate.GetFrameSnapshot();
  };

  const Lotus::FrameSnapshot first = sync();
  const auto& materials = first.scene->materials;
  const auto binding = [](const Lotus::FrameSnapshot& snapshot,
                           const char* mesh) {
    return snapshot.scene->meshes.at(mesh).material;
  };
  Check(materials.size() == 5, "expected five materials, got " +
                                   std::to_string(materials.size()));
  Check(binding(first, "/Unbound").empty() &&
            binding(first, "/Glowing") == "/Looks/Glow" &&
            binding(first, "/Red") == "/Looks/Red" &&
            binding(first, "/Clamped") == "/Looks/Clamped",
      "the mesh bindings differ from the stage's");
  Expect("Red", materials.at("/Looks/Red"),
      MakeMaterial({0.75F, 0.125F, 0.0625F}, 0.25F, 0.75F, {2, 1, 0.5F}));
  Expect("Clamped", materials.at("/Looks/Clamped"),
      MakeMaterial({1, 0, 0.5F}, 1, 0, {0, 3, 0}));
  // The diffuse colour is a lookup of an image that cannot be read, which
  // returns its fallback; the specular workflow ignores metallic.
  Lotus::Material specular = materials.at("/Looks/Specular");
  Check(specular.base_color_texture &&
            specular.base_color_texture->texture.ends_with("missing.png|auto") &&
            !first.scene->textures.contains(specular.base_color_texture->texture),
      "the Specular lookup does not name its missing image");
  specular.base_color_texture.reset();
  Expect("Specular", specular,
      MakeMaterial({0.18F, 0.18F, 0.18F}, 0.125F, 0, {0, 0, 0}));
  Expect("Unknown", materials.at("/Looks/Unknown"), Lotus::Material{});
  if (gpu) {
    Check(delegate.GetGpuSceneStats().material_count == 6,
        "the GPU material table does not hold the default and five materials");
    ExpectRadiance("first frame", color, 1, {0.18F, 0.18F, 0.18F});
    ExpectRadiance("first frame", color, 6, {1.5F, 2.25F, 3.125F});
  }

  // A value edit changes the material, not the meshes.
  stage->GetPrimAtPath(SdfPath("/Looks/Glow/Surface"))
      .GetAttribute(TfToken("inputs:emissiveColor"))
      .Set(GfVec3f(0.5F, 0.25F, 0.0F));
  const Lotus::FrameSnapshot edited = sync();
  Expect("Glow after its edit", edited.scene->materials.at("/Looks/Glow"),
      MakeMaterial({0.5F, 0.25F, 0.125F}, 0.5F, 0, {0.5F, 0.25F, 0}));
  for (const auto& [id, mesh] : edited.scene->meshes) {
    Check(mesh.geometry == first.scene->meshes.at(id).geometry &&
              mesh.material == first.scene->meshes.at(id).material,
        "a material edit changed the mesh " + id);
  }
  if (gpu) {
    ExpectRadiance("material edit", color, 6, {1.0F, 0.5F, 0.125F});
  }

  // A new binding.
  const UsdPrim unbound = stage->GetPrimAtPath(SdfPath("/Unbound"));
  unbound.AddAppliedSchema(TfToken("MaterialBindingAPI"));
  unbound.CreateRelationship(TfToken("material:binding"))
      .SetTargets({SdfPath("/Looks/Glow")});
  const Lotus::FrameSnapshot rebound = sync();
  Check(binding(rebound, "/Unbound") == "/Looks/Glow" &&
            rebound.scene->meshes.at("/Unbound").geometry ==
                first.scene->meshes.at("/Unbound").geometry,
      "a new binding was not synced, or replaced the geometry");
  if (gpu) {
    ExpectRadiance("binding", color, 1, {1.0F, 0.5F, 0.125F});
  }

  // A removed material leaves its meshes to the default.
  stage->RemovePrim(SdfPath("/Looks/Clamped"));
  const Lotus::FrameSnapshot removed = sync();
  Check(!removed.scene->materials.contains("/Looks/Clamped") &&
            !removed.scene->materials.contains(binding(removed, "/Clamped")),
      "a removed material is still in the scene, or still bound");
  if (gpu) {
    Check(delegate.GetGpuSceneStats().material_count == 5,
        "the GPU material table kept the removed material");
  }
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
