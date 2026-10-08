// SPDX-License-Identifier: Apache-2.0
// Translates hand-built UsdUVTexture networks, then syncs a USD stage whose
// UsdPreviewSurface inputs read PNG images written by the test through
// UsdImaging's scene indices into the Lotus render delegate. Checks the
// texture inputs, the decoded texels, the meshes' texture-coordinate sets and
// the textures' lifetimes through edits. With --gpu a render pass also
// renders a colour AOV in which a textured Lambert surface must show its
// texels' albedo.
#include "adapter.hpp"
#include "material_translator.hpp"

#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hio/image.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <lotus/vulkan_backend.hpp>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

void Check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

// A node of a hand-built network.
HdMaterialNode Node(const char* path, const char* identifier,
    std::map<TfToken, VtValue> parameters = {}) {
  HdMaterialNode node;
  node.path = SdfPath(path);
  node.identifier = TfToken(identifier);
  node.parameters = std::move(parameters);
  return node;
}

void TranslatorCases() {
  HdMaterialNetworkMap map;
  HdMaterialNetwork& network = map.map[HdMaterialTerminalTokens->surface];
  const SdfPath surface("/Material/Surface");
  network.nodes = {
      Node("/Material/Reader", "UsdPrimvarReader_float2",
          {{TfToken("varname"), VtValue(std::string("uv"))},
              {TfToken("fallback"), VtValue(GfVec2f(0.5F, 0.25F))}}),
      Node("/Material/Albedo", "UsdUVTexture",
          {{TfToken("file"),
               VtValue(SdfAssetPath("albedo.png", "/abs/albedo.png"))},
              {TfToken("wrapS"), VtValue(TfToken("repeat"))},
              {TfToken("wrapT"), VtValue(TfToken("mirror"))},
              {TfToken("scale"), VtValue(GfVec4f(0.5F, 1.0F, 1.0F, 1.0F))},
              {TfToken("bias"), VtValue(GfVec4f(0.0F, 0.125F, 0.0F, 0.0F))},
              {TfToken("fallback"), VtValue(GfVec4f(1.0F, 0.0F, 1.0F, 1.0F))},
              {TfToken("sourceColorSpace"), VtValue(TfToken("raw"))}}),
      Node("/Material/Unresolved", "UsdUVTexture",
          {{TfToken("file"), VtValue(SdfAssetPath("orm.png"))},
              {TfToken("colorSpace:file"), VtValue(TfToken("lin_rec709_scene"))},
              {TfToken("wrapS"), VtValue(TfToken("useMetadata"))},
              {TfToken("wrapT"), VtValue(TfToken("clamp"))}}),
      Node("/Material/Fileless", "UsdUVTexture"),
      Node("/Material/Elsewhere", "UsdUVTexture",
          {{TfToken("file"), VtValue(SdfAssetPath("glow.png", "/abs/glow.png"))},
              {TfToken("st"), VtValue(GfVec2f(0.25F, 0.75F))}}),
      Node("/Material/Surface", "UsdPreviewSurface",
          {{TfToken("diffuseColor"), VtValue(GfVec3f(0.5F))}})};
  const auto connect = [&](const char* upstream, const char* output,
                           const SdfPath& downstream, const char* input) {
    network.relationships.push_back(
        {SdfPath(upstream), TfToken(output), downstream, TfToken(input)});
  };
  for (const char* texture : {"/Material/Albedo", "/Material/Unresolved",
           "/Material/Fileless"}) {
    connect("/Material/Reader", "result", SdfPath(texture), "st");
  }
  connect("/Material/Albedo", "rgb", surface, "diffuseColor");
  connect("/Material/Unresolved", "g", surface, "roughness");
  connect("/Material/Albedo", "rgb", surface, "metallic");
  connect("/Material/Elsewhere", "rgb", surface, "emissiveColor");
  map.terminals.push_back(surface);
  HdLotusMaterialTranslation translation = HdLotusTranslateMaterial(map);
  const Lotus::Material& material = translation.material;
  Check(translation.unsupported.empty(), "the network was unsupported");

  // A connection wins over the authored constant.
  Lotus::TextureInput albedo;
  albedo.texture = HdLotusTextureKey("/abs/albedo.png", TfToken("raw"));
  albedo.wrap_s = Lotus::TextureWrap::Repeat;
  albedo.wrap_t = Lotus::TextureWrap::Mirror;
  albedo.scale = {0.5F, 1.0F, 1.0F, 1.0F};
  albedo.bias = {0.0F, 0.125F, 0.0F, 0.0F};
  albedo.fallback = {1.0F, 0.0F, 1.0F, 1.0F};
  Check(material.base_color_texture == albedo &&
            material.base_color == Lotus::Material{}.base_color,
      "the diffuse lookup was not translated");
  // An unresolved asset keeps its authored path; useMetadata is black. The
  // colour space UsdImaging hands over as colorSpace:file may be a linear
  // colour space's name, which is raw.
  Lotus::TextureInput orm;
  orm.texture = HdLotusTextureKey("orm.png", TfToken("raw"));
  orm.channel = 1;
  orm.wrap_t = Lotus::TextureWrap::Clamp;
  Check(material.roughness_texture == orm,
      "the roughness lookup of a single channel was not translated");
  Check(material.texcoords == "uv" &&
            material.texcoord_fallback == std::array<float, 2>{0.5F, 0.25F},
      "the primvar reader did not give the texture coordinates");
  // A colour output cannot drive a scalar, and a lookup with other texture
  // coordinates than the first is not translated.
  Check(!material.metallic_texture && !material.emission_texture &&
            translation.connected_inputs ==
                std::vector<TfToken>{TfToken("emissiveColor"), TfToken("metallic")},
      "untranslatable lookups were not reported as connected inputs");
  Check(translation.textures ==
            std::vector<HdLotusTextureRequest>{
                {albedo.texture, "/abs/albedo.png", TfToken("raw")},
                {orm.texture, "orm.png", TfToken("raw")}},
      "the texture requests differ");

  // A texture without a file returns its fallback: its key names no image.
  // Replaces Unresolved.g -> roughness, the fifth relationship.
  network.relationships.erase(network.relationships.begin() + 4);
  network.relationships.push_back({SdfPath("/Material/Fileless"), TfToken("a"),
      surface, TfToken("roughness")});
  translation = HdLotusTranslateMaterial(map);
  Check(translation.material.roughness_texture &&
            translation.material.roughness_texture->channel == 3 &&
            translation.material.roughness_texture->texture ==
                HdLotusTextureKey("", TfToken("auto")) &&
            translation.textures.size() == 1,
      "a texture without a file did not become a lookup of no image");

  // Tangent normal constants keep signed components and reject non-finite
  // values. RGB connections use the same authored scale/bias and colour
  // space as other lookups; no implicit unsigned-to-signed conversion.
  auto& parameters = network.nodes.back().parameters;
  parameters[TfToken("normal")] = VtValue(GfVec3f(-2, 0.25F, 3));
  translation = HdLotusTranslateMaterial(map);
  Check(translation.material.normal == std::array<float, 3>{-1, 0.25F, 1},
      "the tangent normal constant lost its sign or range");
  parameters[TfToken("normal")] = VtValue(GfVec3f(0,
      std::numeric_limits<float>::quiet_NaN(), 1));
  translation = HdLotusTranslateMaterial(map);
  Check(translation.material.normal == Lotus::Material{}.normal,
      "a non-finite tangent normal did not keep the default");
  connect("/Material/Albedo", "rgb", surface, "normal");
  translation = HdLotusTranslateMaterial(map);
  Check(translation.material.normal_texture == albedo &&
            translation.textures.size() == 1,
      "the normal lookup was not translated or decoded its shared image twice");
  network.relationships.back() = {SdfPath("/Material/Albedo"), TfToken("g"),
      surface, TfToken("normal")};
  translation = HdLotusTranslateMaterial(map);
  Check(!translation.material.normal_texture &&
            std::find(translation.connected_inputs.begin(),
                translation.connected_inputs.end(), TfToken("normal")) !=
                translation.connected_inputs.end(),
      "a scalar normal connection was not reported as untranslated");
}

// Writes an 8-bit PNG of `channels` channels, rows from the top.
void WritePng(const std::filesystem::path& path, int width, int height,
    int channels, std::vector<std::uint8_t> texels) {
  const HioImageSharedPtr image = HioImage::OpenForWriting(path.string());
  Check(image != nullptr, "cannot write " + path.string());
  HioImage::StorageSpec storage;
  storage.width = width;
  storage.height = height;
  storage.depth = 1;
  storage.format = channels == 1   ? HioFormatUNorm8
                   : channels == 3 ? HioFormatUNorm8Vec3
                                   : HioFormatUNorm8Vec4;
  storage.flipped = false;
  storage.data = texels.data();
  Check(image->Write(storage), "cannot write " + path.string());
}

// The quad's st are face-varying and indexed, map1 per vertex. Painted reads
// st through a primvar reader; Gray reads a constant st.
constexpr const char* kStage = R"usda(#usda 1.0
def Scope "Looks"
{
    def Material "Painted"
    {
        token outputs:surface.connect = </Looks/Painted/Surface.outputs:surface>
        def Shader "Surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor.connect = </Looks/Painted/Albedo.outputs:rgb>
            float inputs:roughness.connect = </Looks/Painted/Linear.outputs:g>
            color3f inputs:emissiveColor.connect = </Looks/Painted/Missing.outputs:rgb>
            token outputs:surface
        }
        def Shader "Reader"
        {
            uniform token info:id = "UsdPrimvarReader_float2"
            string inputs:varname = "st"
            float2 outputs:result
        }
        def Shader "Albedo"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @rgb.png@
            float2 inputs:st.connect = </Looks/Painted/Reader.outputs:result>
            token inputs:wrapS = "clamp"
            token inputs:wrapT = "clamp"
            float3 outputs:rgb
        }
        def Shader "Linear"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @rgb.png@
            float2 inputs:st.connect = </Looks/Painted/Reader.outputs:result>
            token inputs:sourceColorSpace = "raw"
            float outputs:g
        }
        def Shader "Missing"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @missing.png@
            float2 inputs:st.connect = </Looks/Painted/Reader.outputs:result>
            float4 inputs:fallback = (0.25, 0.5, 1, 1)
            float3 outputs:rgb
        }
    }

    def Material "Gray"
    {
        token outputs:surface.connect = </Looks/Gray/Surface.outputs:surface>
        def Shader "Surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor.connect = </Looks/Gray/Texture.outputs:rgb>
            token outputs:surface
        }
        def Shader "Texture"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @gray.png@
            float2 inputs:st = (0.25, 0.75)
            token inputs:sourceColorSpace = "raw"
            float3 outputs:rgb
        }
    }
}

def Mesh "Quad" (
    prepend apiSchemas = ["MaterialBindingAPI"]
)
{
    rel material:binding = </Looks/Painted>
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]
    texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (
        interpolation = "faceVarying"
    )
    int[] primvars:st:indices = [0, 1, 2, 3]
    float2[] primvars:map1 = [(0, 0), (2, 0), (2, 2), (0, 2)] (
        interpolation = "vertex"
    )
}

def Mesh "Plain" (
    prepend apiSchemas = ["MaterialBindingAPI"]
)
{
    rel material:binding = </Looks/Gray>
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(4, 0, 0), (5, 0, 0), (4, 1, 0)]
}
)usda";

// A raw 8-bit tangent normal turns a white mirror towards an emissive wall
// outside the camera view. The unperturbed mirror sees the white environment.
constexpr const char* kNormalStage = R"usda(#usda 1.0
def Scope "Looks"
{
    def Material "Mirror"
    {
        token outputs:surface.connect = </Looks/Mirror/Surface.outputs:surface>
        def Shader "Surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (1, 1, 1)
            float inputs:roughness = 0
            float inputs:metallic = 1
            normal3f inputs:normal.connect = </Looks/Mirror/Normal.outputs:rgb>
            token outputs:surface
        }
        def Shader "Normal"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @normal.png@
            token inputs:sourceColorSpace = "raw"
            float2 inputs:st.connect = </Looks/Mirror/Reader.outputs:result>
            float4 inputs:scale = (2, 2, 2, 1)
            float4 inputs:bias = (-1, -1, -1, 0)
            token inputs:wrapS = "clamp"
            token inputs:wrapT = "clamp"
            float3 outputs:rgb
        }
        def Shader "Reader"
        {
            uniform token info:id = "UsdPrimvarReader_float2"
            string inputs:varname = "st"
            float2 outputs:result
        }
    }
    def Material "Glow"
    {
        token outputs:surface.connect = </Looks/Glow/Surface.outputs:surface>
        def Shader "Surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0, 0, 0)
            color3f inputs:emissiveColor = (2, 1, 0.5)
            token outputs:surface
        }
    }
}
def Mesh "Quad" (prepend apiSchemas = ["MaterialBindingAPI"])
{
    rel material:binding = </Looks/Mirror>
    uniform token subdivisionScheme = "none"
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-4, -4, 0), (4, -4, 0), (4, 4, 0), (-4, 4, 0)]
    texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (
        interpolation = "faceVarying"
    )
}
def Mesh "Wall" (prepend apiSchemas = ["MaterialBindingAPI"])
{
    rel material:binding = </Looks/Glow>
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(6, -20, -0.5), (6, 20, -0.5), (6, 20, -10), (6, -20, -10)]
}
)usda";

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

// The scene's texture whose key names a file of this name decoded in this
// colour space, or null.
std::shared_ptr<const Lotus::Texture> Find(const Lotus::FrameSnapshot& snapshot,
    const std::string& name, const char* color_space) {
  for (const auto& [key, texture] : snapshot.scene->textures) {
    const std::string suffix = name + "|" + color_space;
    if (key.size() >= suffix.size() &&
        key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0) {
      return texture;
    }
  }
  return nullptr;
}

constexpr int kSize = 8;

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

  // The stage and its images in a directory of their own.
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("lotus-texture-test-" + std::string(gpu ? "gpu" : "cpu"));
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  // Red, green, blue and white over black, grey, yellow and magenta.
  const std::vector<std::uint8_t> rgb{255, 0, 0, 0, 255, 0, 0, 0, 255, 255,
      255, 255, 0, 0, 0, 128, 128, 128, 255, 255, 0, 255, 0, 255};
  WritePng(directory / "rgb.png", 4, 2, 3, rgb);
  WritePng(directory / "gray.png", 2, 1, 1, {64, 192});
  {
    std::ofstream stage_file(directory / "stage.usda", std::ios::binary);
    stage_file << kStage;
  }
  const UsdStageRefPtr stage = UsdStage::Open((directory / "stage.usda").string());
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

  // The identity camera sees x and y in [-1, 1]: the quad fills the view.
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
  const Lotus::Material& painted = first.scene->materials.at("/Looks/Painted");
  Check(painted.base_color_texture && painted.roughness_texture &&
            painted.emission_texture && !painted.metallic_texture &&
            painted.texcoords == "st" &&
            painted.base_color_texture->wrap_s == Lotus::TextureWrap::Clamp &&
            painted.roughness_texture->channel == 1 &&
            painted.emission_texture->fallback ==
                std::array<float, 4>{0.25F, 0.5F, 1.0F, 1.0F},
      "the Painted material's lookups differ from the stage's");
  // The 8-bit RGB image is sRGB under auto and linear under raw; either way
  // it gains an opaque alpha. The missing image is absent, so its lookup
  // returns the fallback; the one-channel image is grey.
  const auto albedo = Find(first, "rgb.png", "auto");
  const auto linear = Find(first, "rgb.png", "raw");
  const auto gray = Find(first, "gray.png", "raw");
  std::string keys;
  for (const auto& [key, texture] : first.scene->textures) {
    keys += " " + key;
  }
  Check(albedo && linear && gray && first.scene->textures.size() == 3,
      "the scene does not hold exactly the three readable images:" + keys);
  std::vector<std::uint8_t> rgba;
  for (std::size_t texel = 0; texel < 8; ++texel) {
    rgba.insert(rgba.end(), rgb.begin() + 3 * texel, rgb.begin() + 3 * texel + 3);
    rgba.push_back(255);
  }
  Check(albedo->width == 4 && albedo->height == 2 &&
            albedo->format == Lotus::TextureFormat::Rgba8Srgb &&
            albedo->texels == rgba,
      "the sRGB image's texels differ from the file's");
  Check(linear->format == Lotus::TextureFormat::Rgba8Unorm &&
            linear->texels == rgba,
      "the raw image's texels differ from the file's");
  Check(gray->format == Lotus::TextureFormat::Rgba8Unorm &&
            gray->texels == std::vector<std::uint8_t>{64, 64, 64, 255, 192,
                                192, 192, 255},
      "the one-channel image was not expanded to grey");
  Check(painted.base_color_texture->texture.ends_with("rgb.png|auto") &&
            first.scene->textures.contains(painted.base_color_texture->texture) &&
            !first.scene->textures.contains(painted.emission_texture->texture),
      "the lookups do not name the scene's textures");
  const Lotus::Material& plain = first.scene->materials.at("/Looks/Gray");
  Check(plain.texcoords.empty() &&
            plain.texcoord_fallback == std::array<float, 2>{0.25F, 0.75F},
      "a constant st did not become the texture-coordinate fallback");

  // st is face-varying and indexed, map1 per vertex; both reach every
  // triangle corner. The quad is the fan (0, 1, 2), (0, 2, 3).
  const Lotus::MeshGeometry& quad = *first.scene->meshes.at("/Quad").geometry;
  using St = std::vector<std::array<float, 2>>;
  Check(quad.texcoords.size() == 2 &&
            quad.texcoords.at("st") == St{{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}} &&
            quad.texcoords.at("map1") ==
                St{{0, 0}, {2, 0}, {2, 2}, {0, 0}, {2, 2}, {0, 2}},
      "the quad's texture-coordinate sets differ from the stage's");
  Check(first.scene->meshes.at("/Plain").geometry->texcoords.empty(),
      "a mesh without float-pair primvars has texture coordinates");

  if (gpu) {
    // Under the white environment a Lambert pixel shows its albedo plus the
    // missing emission image's fallback (0.25, 0.5, 1). The clamped lookup
    // is constant over the corner pixels, two rows by one column each: the
    // image's corner texels, red, white, black and magenta, whose sRGB
    // values 0 and 255 decode exactly. Hydra rows run from the bottom.
    const auto* data = static_cast<const float*>(color.Map());
    Check(data != nullptr, "could not map the colour buffer");
    struct Corner {
      int x;
      int y;
      std::array<float, 3> albedo;
    };
    for (const Corner& corner : {Corner{0, kSize - 1, {1, 0, 0}},
             Corner{0, kSize - 2, {1, 0, 0}}, Corner{kSize - 1, kSize - 1, {1, 1, 1}},
             Corner{0, 0, {0, 0, 0}}, Corner{0, 1, {0, 0, 0}},
             Corner{kSize - 1, 0, {1, 0, 1}}}) {
      const float* pixel = data + (corner.y * kSize + corner.x) * 4;
      const std::array<float, 3> emission{0.25F, 0.5F, 1.0F};
      for (int c = 0; c < 3; ++c) {
        Check(std::abs(pixel[c] - (corner.albedo[c] + emission[c])) <= 1e-4F,
            "the textured radiance at " + std::to_string(corner.x) + "," +
                std::to_string(corner.y) + " is (" + std::to_string(pixel[0]) +
                ", " + std::to_string(pixel[1]) + ", " +
                std::to_string(pixel[2]) + ")");
      }
    }
    color.Unmap();
  }

  // A wrap edit re-translates the material and keeps its decoded images; a
  // file edit decodes the new image and releases the old one.
  stage->GetPrimAtPath(SdfPath("/Looks/Painted/Albedo"))
      .GetAttribute(TfToken("inputs:wrapS"))
      .Set(TfToken("repeat"));
  const Lotus::FrameSnapshot wrapped = sync();
  Check(wrapped.scene->materials.at("/Looks/Painted").base_color_texture->wrap_s ==
                Lotus::TextureWrap::Repeat &&
            Find(wrapped, "rgb.png", "auto") == albedo,
      "a wrap edit was lost or decoded its image again");
  stage->GetPrimAtPath(SdfPath("/Looks/Painted/Albedo"))
      .GetAttribute(TfToken("inputs:file"))
      .Set(SdfAssetPath("gray.png"));
  const Lotus::FrameSnapshot refiled = sync();
  Check(!Find(refiled, "rgb.png", "auto") &&
            Find(refiled, "gray.png", "auto") &&
            Find(refiled, "rgb.png", "raw") == linear &&
            refiled.scene->textures.size() == 3,
      "a file edit did not swap exactly the edited lookup's image");

  // A primvar edit replaces the geometry's texture coordinates.
  stage->GetPrimAtPath(SdfPath("/Quad"))
      .GetAttribute(TfToken("primvars:map1"))
      .Set(VtVec2fArray{{0, 0}, {3, 0}, {3, 3}, {0, 3}});
  const Lotus::FrameSnapshot moved = sync();
  Check(moved.scene->meshes.at("/Quad").geometry->texcoords.at("map1")[1] ==
            std::array<float, 2>{3, 0},
      "a texture-coordinate edit was lost");

  // Removing a material releases the images only it names.
  stage->RemovePrim(SdfPath("/Looks/Painted"));
  const Lotus::FrameSnapshot removed = sync();
  Check(removed.scene->textures.size() == 1 &&
            Find(removed, "gray.png", "raw") == gray,
      "a removed material's images were kept, or another material's lost");

  WritePng(directory / "normal.png", 1, 1, 3, {191, 128, 238});
  WritePng(directory / "normal-left.png", 1, 1, 3, {64, 128, 238});
  Check(stage->GetRootLayer()->ImportFromString(kNormalStage),
      "could not replace the stage with the normal-map scene");
  const auto mapped_normal = sync();
  const auto normal_image = Find(mapped_normal, "normal.png", "raw");
  const auto& mirror = mapped_normal.scene->materials.at("/Looks/Mirror");
  Check(mirror.normal_texture && mirror.texcoords == "st" &&
            mirror.normal_texture->scale == std::array<float, 4>{2, 2, 2, 1} &&
            mirror.normal_texture->bias == std::array<float, 4>{-1, -1, -1, 0} &&
            normal_image && normal_image->format == Lotus::TextureFormat::Rgba8Unorm &&
            normal_image->texels == std::vector<std::uint8_t>{191, 128, 238, 255} &&
            mapped_normal.scene->textures.size() == 1,
      "the stage's signed raw normal lookup did not reach the IR");
  const auto expect_normal_radiance = [&](const std::array<float, 3>& expected) {
    if (!gpu)
      return;
    const auto* data = static_cast<const float*>(color.Map());
    Check(data != nullptr, "could not map the normal-map colour buffer");
    for (int p = 0; p < kSize * kSize; ++p)
      for (int c = 0; c < 3; ++c)
        Check(std::isfinite(data[p * 4 + c]) &&
                  std::abs(data[p * 4 + c] - expected[c]) <= 1e-4F,
            "mapped mirror radiance differs: " + std::to_string(data[p * 4 + c]) +
                " instead of " + std::to_string(expected[c]));
    color.Unmap();
  };
  expect_normal_radiance({2, 1, 0.5F});
  // Mirroring s turns the normal towards -X and away from the wall.
  const auto st_attribute = stage->GetPrimAtPath(SdfPath("/Quad"))
                                .GetAttribute(TfToken("primvars:st"));
  st_attribute.Set(VtVec2fArray{{1, 0}, {0, 0}, {0, 1}, {1, 1}});
  const auto mirrored_normal = sync();
  Check(mirrored_normal.scene->meshes.at("/Quad").geometry !=
                mapped_normal.scene->meshes.at("/Quad").geometry &&
            Find(mirrored_normal, "normal.png", "raw") == normal_image,
      "a normal-map UV edit did not replace geometry alone");
  expect_normal_radiance({1, 1, 1});
  st_attribute.Set(VtVec2fArray{{0, 0}, {1, 0}, {1, 1}, {0, 1}});
  sync();
  const auto normal_shader = stage->GetPrimAtPath(SdfPath("/Looks/Mirror/Normal"));
  normal_shader.GetAttribute(TfToken("inputs:file")).Set(SdfAssetPath("normal-left.png"));
  const auto edited_normal = sync();
  Check(!Find(edited_normal, "normal.png", "raw") &&
            Find(edited_normal, "normal-left.png", "raw") &&
            edited_normal.scene->textures.size() == 1,
      "a normal-map file edit kept the old image");
  expect_normal_radiance({1, 1, 1});
  normal_shader.GetAttribute(TfToken("inputs:scale")).Set(GfVec4f(0, 0, 0, 1));
  normal_shader.GetAttribute(TfToken("inputs:bias")).Set(GfVec4f(0));
  const auto zero_normal = sync();
  Check(Find(zero_normal, "normal-left.png", "raw") ==
            Find(edited_normal, "normal-left.png", "raw"),
      "a normal scale edit decoded its image again");
  expect_normal_radiance({1, 1, 1});
  stage->GetPrimAtPath(SdfPath("/Looks/Mirror/Surface"))
      .GetAttribute(TfToken("inputs:normal"))
      .ClearConnections();
  const auto disconnected = sync();
  Check(!disconnected.scene->materials.at("/Looks/Mirror").normal_texture &&
            disconnected.scene->textures.empty(),
      "disconnecting the normal input kept its image");
  expect_normal_radiance({1, 1, 1});
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
