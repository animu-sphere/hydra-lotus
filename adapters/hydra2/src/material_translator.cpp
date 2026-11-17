// SPDX-License-Identifier: Apache-2.0
#include "material_translator.hpp"

#include "material_graph.hpp"

#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/staticTokens.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/assetPath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <utility>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

namespace Graph = HdLotusGraph;
using Graph::Connection;
using Graph::Parameter;
using Graph::ReadFile;
using Graph::ReadToken;

TF_DEFINE_PRIVATE_TOKENS(Tokens,
    (UsdPreviewSurface)
    (UsdUVTexture)
    (UsdPrimvarReader_float2)
    (UsdPrimvarReader_float3)
    (UsdTransform2d)
    (rotation)
    (translation)
    (result)
    (diffuseColor)
    (emissiveColor)
    (metallic)
    (roughness)
    (normal)
    (opacity)
    (opacityThreshold)
    (useSpecularWorkflow)
    (ior)
    (specularColor)
    (file)
    (st)
    (wrapS)
    (wrapT)
    (scale)
    (bias)
    (fallback)
    (sourceColorSpace)
    ((colorSpaceFile, "colorSpace:file"))
    (varname)
    (rgb)
    (r)
    (g)
    (b)
    (a)
    (black)
    (clamp)
    (repeat)
    (mirror)
    (raw)
    (sRGB)
    ((automatic, "auto"))
    (mtlx)
    ((mtlxPreviewSurface, "ND_UsdPreviewSurface_surfaceshader"))
    ((standardSurface, "ND_standard_surface_surfaceshader"))
    ((gltfPbr, "ND_gltf_pbr_surfaceshader"))
    (emissive)
    (emissive_strength)
    (alpha)
    (alpha_mode)
    (alpha_cutoff)
    (sheen_color)
    (sheen_roughness)
    (clearcoat)
    (clearcoat_roughness)
    (clearcoat_normal)
    (iridescence)
    (anisotropy_strength)
    (thickness)
    (attenuation_distance)
    (attenuation_color)
    (dispersion)
    ((texcoordVector2, "ND_texcoord_vector2"))
    ((geompropvalueVector2, "ND_geompropvalue_vector2"))
    (base)
    (base_color)
    (metalness)
    (specular)
    (specular_color)
    (specular_roughness)
    (specular_IOR)
    (emission)
    (emission_color)
    (coat)
    (diffuse_roughness)
    (sheen)
    (specular_anisotropy)
    (subsurface)
    (thin_film_thickness)
    (transmission)
    (transmission_color)
    (transmission_depth)
    (transmission_scatter)
    (transmission_scatter_anisotropy)
    (transmission_dispersion)
    (transmission_extra_roughness)
    (thin_walled)
    (texcoord)
    (uaddressmode)
    (vaddressmode)
    ((defaultValue, "default"))
    (uvtiling)
    (uvoffset)
    (realworldimagesize)
    (realworldtilesize)
    (index)
    (geomprop)
    (in)
    (space)
    (tangent)
    (bitangent)
    (constant)
    (periodic));

std::optional<double> ReadScalar(const VtValue& value) {
  if (value.IsHolding<float>()) return value.UncheckedGet<float>();
  if (value.IsHolding<double>()) return value.UncheckedGet<double>();
  if (value.IsHolding<int>()) return value.UncheckedGet<int>();
  return std::nullopt;
}

std::optional<std::array<double, 3>> ReadColor(const VtValue& value) {
  if (value.IsHolding<GfVec3f>()) {
    const GfVec3f& color = value.UncheckedGet<GfVec3f>();
    return std::array<double, 3>{color[0], color[1], color[2]};
  }
  if (value.IsHolding<GfVec3d>()) {
    const GfVec3d& color = value.UncheckedGet<GfVec3d>();
    return std::array<double, 3>{color[0], color[1], color[2]};
  }
  return std::nullopt;
}

// A float2 or float4 parameter, when it holds one of either precision with
// finite components.
template <std::size_t N, typename Single, typename Double>
std::optional<std::array<float, N>> ReadVector(const VtValue* value) {
  std::array<double, N> read{};
  if (value == nullptr) {
    return std::nullopt;
  }
  if (value->IsHolding<Single>()) {
    for (std::size_t c = 0; c < N; ++c) read[c] = value->UncheckedGet<Single>()[c];
  } else if (value->IsHolding<Double>()) {
    for (std::size_t c = 0; c < N; ++c) read[c] = value->UncheckedGet<Double>()[c];
  } else {
    return std::nullopt;
  }
  std::array<float, N> result{};
  for (std::size_t c = 0; c < N; ++c) {
    result[c] = static_cast<float>(read[c]);
    if (!std::isfinite(result[c])) {
      return std::nullopt;
    }
  }
  return result;
}

// UsdUVTexture's wrap modes. useMetadata, the default, would take the
// image's own wrap mode; Lotus reads none, so it is black, as Storm's is for
// an image without one.
Lotus::TextureWrap ReadWrap(const VtValue* value) {
  const TfToken wrap = ReadToken(value);
  if (wrap == Tokens->clamp) return Lotus::TextureWrap::Clamp;
  if (wrap == Tokens->repeat) return Lotus::TextureWrap::Repeat;
  if (wrap == Tokens->mirror) return Lotus::TextureWrap::Mirror;
  return Lotus::TextureWrap::Black;
}

// A UsdUVTexture's sourceColorSpace: raw, sRGB or auto. UsdImaging hands it
// over as the file input's colour space, colorSpace:file, which may also be
// a colour space name; linear and data spaces are raw.
TfToken ReadColorSpace(const HdMaterialNode2& texture) {
  const auto read = [&](const TfToken& name) {
    const auto parameter = texture.parameters.find(name);
    return parameter == texture.parameters.end()
               ? TfToken()
               : ReadToken(&parameter->second);
  };
  TfToken color_space = read(Tokens->sourceColorSpace);
  if (color_space.IsEmpty()) {
    color_space = read(Tokens->colorSpaceFile);
  }
  const std::string& name = color_space.GetString();
  if (color_space == Tokens->raw || name.starts_with("lin_") ||
      name == "data") {
    return Tokens->raw;
  }
  if (color_space == Tokens->sRGB || name.starts_with("srgb_")) {
    return Tokens->sRGB;
  }
  return Tokens->automatic;
}

// The key of `file` under `color_space`, which the translation then
// requests once.
std::string RequestTexture(HdLotusMaterialTranslation& translation,
    const std::string& file, const TfToken& color_space) {
  std::string key = HdLotusTextureKey(file, color_space);
  // Without a file, the key names no image, so the lookup returns its
  // fallback.
  if (!file.empty()) {
    const HdLotusTextureRequest request{key, file, color_space};
    if (std::find(translation.textures.begin(), translation.textures.end(),
            request) == translation.textures.end()) {
      translation.textures.push_back(request);
    }
  }
  return key;
}

// Whether the material can take a lookup at these texture coordinates: a
// material has one texture-coordinate source, its first lookup's.
bool SameTexcoords(const Lotus::Material& material, const std::string& texcoords,
    const std::array<float, 2>& fallback) {
  return !Lotus::HasTextureInputs(material) ||
         (material.geometry.texcoords == texcoords && material.geometry.texcoord_fallback == fallback);
}

using Kind = HdLotusMaterialDiagnosticKind;

// Adds a diagnostic. An input has one: a substitution gives way to anything
// stronger, and otherwise the first stands.
void Report(HdLotusMaterialTranslation& translation,
    HdLotusMaterialDiagnostic diagnostic) {
  if (!diagnostic.input.IsEmpty()) {
    for (HdLotusMaterialDiagnostic& existing : translation.diagnostics) {
      if (existing.input == diagnostic.input) {
        if (existing.kind == Kind::Substituted && diagnostic.kind != Kind::Substituted) {
          existing = std::move(diagnostic);
        }
        return;
      }
    }
  }
  translation.diagnostics.push_back(std::move(diagnostic));
}

// What the MaterialX surface readers share (docs/design/
// MATERIALX_TRANSLATION.md section 6.3): reading an input of the surface's
// definition through the graph reader, placing a lookup in the material,
// the normal input, and the inputs the definition does not name.
class MaterialXReader {
public:
  MaterialXReader(const HdMaterialNetwork2& network, const SdfPath& path,
      const HdMaterialNode2& node, const Graph::NodeDef& def,
      HdLotusMaterialTranslation& translation)
      : path_(path), node_(node), def_(def), translation_(translation), graph_(network) {
  }

protected:
  // The input as the graph reader reads it, with its diagnostic reported.
  // A normal map reaches only the normal input.
  Graph::Reading Get(const TfToken& input,
      const std::optional<Graph::Range>& range = std::nullopt) {
    Graph::Reading reading = graph_.Read(path_, node_, def_, input, range);
    if (reading.diagnostic) {
      reading.diagnostic->input = input;
      Report(translation_, *reading.diagnostic);
    }
    if (reading.value.normal_map && input != Tokens->normal &&
        input != Tokens->clearcoat_normal) {
      Diagnose(Kind::UnsupportedGraph, input, reading,
          "a normal map on an input that is no normal");
      reading.value = Default(input);
      reading.failed = true;
    }
    return reading;
  }

  // The definition's default for a numeric input.
  Graph::Value Default(const TfToken& input) const {
    const Graph::InputDef* definition = def_.Find(input);
    return Graph::Constant(std::max<std::size_t>(Graph::Width(definition->type), 1),
        definition->value);
  }

  // A diagnostic about what the input reads: at the node its connection
  // names, or at the surface for an authored constant.
  void Diagnose(Kind kind, const TfToken& input, const Graph::Reading& reading,
      std::string detail) {
    Report(translation_,
        {kind, input, reading.connected ? reading.upstream : path_,
            reading.connected ? reading.upstream_type : node_.nodeTypeId,
            std::move(detail)});
  }

  // Whether a lookup at the value's coordinates fits the material, which
  // then reads there and requests its image.
  bool Place(const Graph::Value& value) {
    Lotus::Material& material = translation_.material;
    if (!SameTexcoords(material, value.texcoords, value.texcoord_fallback)) {
      return false;
    }
    material.geometry.texcoords = value.texcoords;
    material.geometry.texcoord_fallback = value.texcoord_fallback;
    RequestTexture(translation_, value.file, value.color_space);
    return true;
  }

  // The lookup `reading` drives the input with, placed in the material; or
  // nothing, with a diagnostic, when the IR cannot hold it.
  std::optional<Lotus::TextureInput> Lookup(const TfToken& input,
      const Graph::Reading& reading, const Graph::Value& value, bool colour) {
    std::string why;
    auto lookup = Graph::ToTextureInput(value, colour, why);
    if (!lookup) {
      Diagnose(Kind::UnsupportedGraph, input, reading, why);
    } else if (!Place(value)) {
      Diagnose(Kind::UnsupportedGraph, input, reading,
          "a second texture-coordinate source: a material reads one");
      lookup.reset();
    }
    return lookup;
  }

  void Scalar(const TfToken& input, float& value,
      std::optional<Lotus::TextureInput>* lookup, double low, double high) {
    const Graph::Reading reading = Get(input, Graph::Range{low, high});
    if (reading.value.IsLookup()) {
      if (lookup == nullptr) {
        Diagnose(Kind::UnrepresentedInput, input, reading,
            "a lookup on an input that takes a constant");
      } else if (const auto placed = Lookup(input, reading, reading.value, false)) {
        *lookup = placed;
      }
      return;
    }
    value = static_cast<float>(std::clamp(reading.value.constant[0], low, high));
  }

  // The input when it is a constant; a lookup is reported, and leaves
  // nothing.
  std::optional<Graph::Value> ConstantOnly(const TfToken& input) {
    const Graph::Reading reading = Get(input);
    if (reading.value.IsLookup()) {
      Diagnose(Kind::UnrepresentedInput, input, reading,
          "a lookup on an input that takes a constant");
      return std::nullopt;
    }
    return reading.value;
  }

  // An input the material does not represent: reported unless it is the
  // constant `neutral`.
  void Neutral(const TfToken& input, double neutral, const char* detail) {
    const Graph::Reading reading = Get(input);
    bool differs = reading.value.IsLookup();
    for (std::size_t c = 0; !differs && c < reading.value.width; ++c) {
      differs = reading.value.constant[c] != neutral;
    }
    if (differs) {
      Diagnose(Kind::UnrepresentedInput, input, reading, detail);
    }
  }

  // A normal map, or the constant of one without an image; an authored
  // world-space normal is not represented.
  void Normal() {
    Lotus::Material& material = translation_.material;
    NormalInput(Tokens->normal, material.geometry.normal_texture,
        &material.geometry.normal);
  }

  // The same for any normal input: its lookup goes to `texture`, and a
  // normal map's constant to `constant`. Without a `constant` to hold it,
  // one that tilts the normal is reported.
  void NormalInput(const TfToken& input,
      std::optional<Lotus::TextureInput>& texture,
      std::array<float, 3>* constant) {
    if (Connection(node_, input) == nullptr) {
      if (Parameter(node_, input) != nullptr) {
        Report(translation_, {Kind::UnrepresentedInput, input, path_,
                                 node_.nodeTypeId,
                                 "a constant normal: only a normal map is read"});
      }
      return;
    }
    const Graph::Reading reading = Get(input);
    if (reading.failed) {
      return;
    }
    if (!reading.value.normal_map) {
      Diagnose(Kind::UnsupportedGraph, input, reading,
          "a normal that is not a normal map");
      return;
    }
    if (reading.value.IsLookup()) {
      if (const auto placed = Lookup(input, reading, reading.value, true)) {
        texture = placed;
      }
      return;
    }
    if (constant == nullptr) {
      if (reading.value.constant[0] != 0.0 || reading.value.constant[1] != 0.0) {
        Diagnose(Kind::UnrepresentedInput, input, reading,
            "a normal map without an image that tilts the normal");
      }
      return;
    }
    for (std::size_t c = 0; c < 3; ++c) {
      (*constant)[c] =
          static_cast<float>(std::clamp(reading.value.constant[c], -1.0, 1.0));
    }
  }

  // Authored parameters and connections the definition does not name. A
  // parameter with a colon is Hydra's note about another (colorSpace:file).
  void UnknownInputs() {
    for (const auto& [name, value] : node_.parameters) {
      if (name.GetString().find(':') == std::string::npos && def_.Find(name) == nullptr) {
        Report(translation_, {Kind::UnknownInput, name, path_, node_.nodeTypeId,
                                 "the definition names no such input"});
      }
    }
    for (const auto& [name, connections] : node_.inputConnections) {
      if (def_.Find(name) == nullptr) {
        Report(translation_, {Kind::UnknownInput, name, path_, node_.nodeTypeId,
                                 "the definition names no such input"});
      }
    }
  }

  const SdfPath& path_;
  const HdMaterialNode2& node_;
  const Graph::NodeDef& def_;
  HdLotusMaterialTranslation& translation_;
  Graph::GraphReader graph_;
};

// Translates an ND_standard_surface_surfaceshader node
// (docs/design/MATERIALX_TRANSLATION.md section 6.3). Every input of the
// definition is translated, reported as unrepresented, or without effect:
// the inputs of a lobe the material lacks do nothing while the lobe's weight
// is 0, and the weight's diagnostic stands for them otherwise.
class StandardSurfaceReader : public MaterialXReader {
public:
  using MaterialXReader::MaterialXReader;

  void Translate() {
    Lotus::Material& material = translation_.material;
    Lotus::PreviewSurface& preview = Lotus::Preview(material);
    constexpr double kLargest = 3.4e38;
    // Standard Surface's defaults where the IR's differ.
    preview.base_color = {0.8F, 0.8F, 0.8F};
    preview.roughness = 0.2F;

    Color(Tokens->base, Tokens->base_color, preview.base_color,
        preview.base_color_texture, 0.0, 1.0);
    Color(Tokens->emission, Tokens->emission_color, preview.emission,
        preview.emission_texture, 0.0, kLargest);
    Scalar(Tokens->metalness, preview.metallic, &preview.metallic_texture,
        0.0, 1.0);
    Scalar(Tokens->specular_roughness, preview.roughness,
        &preview.roughness_texture, 0.0, 1.0);
    Scalar(Tokens->specular_IOR, preview.ior, nullptr, 1.0, kLargest);
    Transmission();
    Specular();
    Opacity();
    Normal();
    for (const TfToken& lobe : {Tokens->coat, Tokens->diffuse_roughness,
             Tokens->sheen, Tokens->specular_anisotropy, Tokens->subsurface,
             Tokens->thin_film_thickness}) {
      const Graph::Reading weight = Get(lobe);
      if (weight.value.IsLookup() || weight.value.constant[0] != 0.0) {
        Diagnose(Kind::UnrepresentedInput, lobe, weight, "the material has no such lobe");
      }
    }
    UnknownInputs();
  }

private:
  // A colour times its scalar weight, as base and emission are. A product
  // the IR cannot hold leaves the input that stops it at its default.
  void Color(const TfToken& weight, const TfToken& color, std::array<float, 3>& value,
      std::optional<Lotus::TextureInput>& lookup, double low, double high) {
    const Graph::Reading scale = Get(weight);
    // A clamp on the colour is the input's own only under a weight of 1.
    const bool unit = scale.value.IsConstant() && scale.value.constant[0] == 1.0;
    const Graph::Reading tint = Get(color,
        unit ? std::optional<Graph::Range>(Graph::Range{low, high}) : std::nullopt);
    std::string why;
    Graph::Value product = Graph::Product(tint.value, scale.value, why);
    if (product.kind == Graph::Value::Kind::Failed) {
      // Two lookups, or a scalar lookup over a colour: the weight's.
      Diagnose(Kind::UnsupportedGraph, weight, scale,
          why + ": " + weight.GetString() + " times " + color.GetString());
      product = Graph::Product(tint.value, Default(weight), why);
    }
    if (product.IsLookup()) {
      const bool from_color = tint.value.IsLookup();
      if (const auto placed = Lookup(from_color ? color : weight,
              from_color ? tint : scale, product, true)) {
        lookup = placed;
        return;
      }
      product = Graph::Product(Default(color), Default(weight), why);
    }
    for (std::size_t c = 0; c < 3; ++c) {
      value[c] = static_cast<float>(std::clamp(product.constant[c], low, high));
    }
  }

  // A constant transmission weight and colour, and whether the surface is
  // a thin wall (docs/design/TRANSMISSION.md section 9). Under a positive
  // transmission_depth the colour is absorption at that depth inside the
  // medium, which is not represented: the transmitted light stays untinted.
  // The medium's scattering, dispersion and extra roughness are ignored.
  // Without transmission none of them has an effect.
  void Transmission() {
    Lotus::PreviewSurface& preview = Lotus::Preview(translation_.material);
    if (const auto weight = ConstantOnly(Tokens->transmission)) {
      preview.transmission =
          static_cast<float>(std::clamp(weight->constant[0], 0.0, 1.0));
    }
    if (Connection(node_, Tokens->thin_walled) != nullptr) {
      // No node of the table gives a boolean: the read reports why.
      (void)Get(Tokens->thin_walled);
    } else if (const VtValue* thin = Parameter(node_, Tokens->thin_walled)) {
      if (thin->IsHolding<bool>()) {
        preview.thin_walled = thin->UncheckedGet<bool>();
      } else if (const auto flag = ReadScalar(*thin)) {
        preview.thin_walled = *flag != 0.0;
      } else {
        Report(translation_, {Kind::UnsupportedValue, Tokens->thin_walled, path_,
                                 node_.nodeTypeId, "input thin_walled holds no boolean"});
      }
    }
    if (!(preview.transmission > 0.0F)) {
      return;
    }
    if (const auto color = ConstantOnly(Tokens->transmission_color)) {
      for (std::size_t c = 0; c < 3; ++c) {
        preview.transmission_color[c] =
            static_cast<float>(std::clamp(color->constant[c], 0.0, 1.0));
      }
    }
    const Graph::Reading depth = Get(Tokens->transmission_depth);
    if (depth.value.IsLookup() || depth.value.constant[0] != 0.0) {
      Diagnose(Kind::UnrepresentedInput, Tokens->transmission_depth, depth,
          "absorption at a depth is not represented: the transmitted light is untinted");
      preview.transmission_color = {1.0F, 1.0F, 1.0F};
    }
    for (const TfToken& input : {Tokens->transmission_scatter,
             Tokens->transmission_scatter_anisotropy, Tokens->transmission_dispersion,
             Tokens->transmission_extra_roughness}) {
      Neutral(input, 0.0, "the transmitting medium is not represented");
    }
  }

  // specular 0 removes the dielectric interface; specular 1 with a white
  // specular_color is the IR's. Other weights and tints are not represented.
  // Transmission refracts with specular_IOR whatever the specular weight,
  // so the interface's reflection stays too.
  void Specular() {
    const Graph::Reading weight = Get(Tokens->specular);
    const bool constant = weight.value.IsConstant();
    if (constant && weight.value.constant[0] == 0.0 &&
        !(Lotus::Preview(translation_.material).transmission > 0.0F)) {
      Lotus::Preview(translation_.material).ior = 1.0F;
      return;
    }
    if (!constant || weight.value.constant[0] != 1.0) {
      Diagnose(Kind::UnrepresentedInput, Tokens->specular, weight,
          "a specular weight other than 0 and 1");
    }
    Neutral(Tokens->specular_color, 1.0, "a specular tint");
  }

  // A grey constant opacity is coverage; a coloured one is read as its
  // mean.
  void Opacity() {
    const auto opacity = ConstantOnly(Tokens->opacity);
    if (!opacity) {
      return;
    }
    const auto& [r, g, b, a] = opacity->constant;
    if (r != g || g != b) {
      Report(translation_, {Kind::Substituted, Tokens->opacity, path_, node_.nodeTypeId,
                               "a coloured opacity is read as its mean"});
    }
    translation_.material.geometry.opacity =
        static_cast<float>(std::clamp((r + g + b) / 3.0, 0.0, 1.0));
  }
};

// Translates an ND_gltf_pbr_surfaceshader node into the glTF PBR model
// (docs/design/GLTF_PBR.md section 5). Every input of the definition is
// translated, reported as unrepresented, or without effect: occlusion in a
// path tracer, alpha under OPAQUE, and the inputs of an extension the
// material lacks while the extension's weight is at its default.
class GltfPbrReader : public MaterialXReader {
public:
  using MaterialXReader::MaterialXReader;

  void Translate() {
    Lotus::GltfPbr& gltf =
        translation_.material.surface.emplace<Lotus::GltfPbr>();
    constexpr double kLargest = 3.4e38;
    Color(Tokens->base_color, gltf.base_color, 0.0, 1.0);
    Scalar(Tokens->metallic, gltf.metallic.value, &gltf.metallic.texture, 0.0,
        1.0);
    Scalar(Tokens->roughness, gltf.roughness.value, &gltf.roughness.texture,
        0.0, 1.0);
    Color(Tokens->emissive, gltf.emissive, 0.0, kLargest);
    if (const auto strength = ConstantOnly(Tokens->emissive_strength)) {
      gltf.emissive_strength =
          static_cast<float>(std::clamp(strength->constant[0], 0.0, kLargest));
    }
    Normal();
    Alpha();
    // The extensions (docs/design/GLTF_PBR_EXTENSIONS.md section 5). The
    // index is a uniform of the definition.
    if (const auto ior = ConstantOnly(Tokens->ior)) {
      gltf.ior = static_cast<float>(std::clamp(ior->constant[0], 1.0, kLargest));
    }
    Scalar(Tokens->specular, gltf.specular.value, &gltf.specular.texture, 0.0,
        1.0);
    Color(Tokens->specular_color, gltf.specular_color, 0.0, kLargest);
    Scalar(Tokens->transmission, gltf.transmission.value,
        &gltf.transmission.texture, 0.0, 1.0);
    Scalar(Tokens->clearcoat, gltf.clearcoat.value, &gltf.clearcoat.texture,
        0.0, 1.0);
    // A lobe's other inputs do nothing while its weight is a constant 0, and
    // are not read.
    if (gltf.clearcoat.value > 0.0F || gltf.clearcoat.texture) {
      Scalar(Tokens->clearcoat_roughness, gltf.clearcoat_roughness.value,
          &gltf.clearcoat_roughness.texture, 0.0, 1.0);
      NormalInput(Tokens->clearcoat_normal, gltf.clearcoat_normal_texture,
          nullptr);
    }
    Color(Tokens->sheen_color, gltf.sheen_color, 0.0, 1.0);
    if (gltf.sheen_color.texture ||
        gltf.sheen_color.value != std::array<float, 3>{0.0F, 0.0F, 0.0F}) {
      Scalar(Tokens->sheen_roughness, gltf.sheen_roughness.value,
          &gltf.sheen_roughness.texture, 0.0, 1.0);
    }
    Neutral(Tokens->iridescence, 0.0, "the material has no thin film");
    Neutral(Tokens->anisotropy_strength, 0.0,
        "the material's lobes are isotropic");
    // The definition's own graph reads none of these; glTF gives them a
    // meaning, so they are not dropped in silence. Transmission renders as
    // a thin surface whatever the thickness.
    Neutral(Tokens->thickness, 0.0, "the material encloses no medium");
    Neutral(Tokens->attenuation_color, 1.0, "the material encloses no medium");
    Neutral(Tokens->dispersion, 0.0, "the material has no dispersion");
    if (Connection(node_, Tokens->attenuation_distance) != nullptr ||
        Parameter(node_, Tokens->attenuation_distance) != nullptr) {
      Report(translation_, {Kind::UnrepresentedInput,
                               Tokens->attenuation_distance, path_,
                               node_.nodeTypeId,
                               "the material encloses no medium"});
    }
    UnknownInputs();
  }

private:
  // A colour input: a constant clamped into [low, high], or a lookup. A
  // graph the IR cannot hold leaves the definition's default.
  void Color(const TfToken& input,
      Lotus::MaterialInput<std::array<float, 3>>& value, double low,
      double high) {
    const Graph::Reading reading = Get(input, Graph::Range{low, high});
    if (reading.value.IsLookup()) {
      if (const auto placed = Lookup(input, reading, reading.value, true)) {
        value.texture = placed;
      }
      return;
    }
    for (std::size_t c = 0; c < 3; ++c) {
      value.value[c] =
          static_cast<float>(std::clamp(reading.value.constant[c], low, high));
    }
  }

  // A uniform input's authored number, or its definition's default; nothing,
  // reported, for a connection or a value that is no number.
  std::optional<double> Uniform(const TfToken& input) {
    if (Connection(node_, input) != nullptr) {
      Report(translation_, {Kind::UnsupportedValue, input, path_,
                               node_.nodeTypeId,
                               "input " + input.GetString() +
                                   " is uniform: a connection is not read"});
      return std::nullopt;
    }
    const VtValue* authored = Parameter(node_, input);
    if (authored == nullptr) {
      return def_.Find(input)->value[0];
    }
    const auto read = ReadScalar(*authored);
    if (!read || !std::isfinite(*read)) {
      Report(translation_, {Kind::UnsupportedValue, input, path_,
                               node_.nodeTypeId,
                               "input " + input.GetString() +
                                   " holds no finite number"});
      return std::nullopt;
    }
    return read;
  }

  // glTF's alpha is coverage (docs/design/GLTF_PBR.md section 5.3): none
  // under OPAQUE, where alpha is not read; alpha against the cutoff under
  // MASK; stochastic under BLEND. A cutoff the IR's threshold cannot state
  // is the constant it amounts to.
  void Alpha() {
    Lotus::MaterialGeometry& geometry = translation_.material.geometry;
    const auto mode = Uniform(Tokens->alpha_mode);
    if (!mode) {
      return;
    }
    if (*mode != 0.0 && *mode != 1.0 && *mode != 2.0) {
      Report(translation_, {Kind::UnsupportedValue, Tokens->alpha_mode, path_,
                               node_.nodeTypeId,
                               "an alpha_mode that is none of 0, 1 and 2"});
      return;
    }
    if (*mode == 0.0) {
      return;
    }
    if (*mode == 1.0) {
      const auto cutoff = Uniform(Tokens->alpha_cutoff);
      if (!cutoff || !(*cutoff > 0.0)) {
        // Every alpha passes a cutoff of 0.
        return;
      }
      if (*cutoff > 1.0) {
        // No alpha passes.
        geometry.opacity = 0.0F;
        return;
      }
      geometry.opacity_threshold = static_cast<float>(*cutoff);
    }
    Scalar(Tokens->alpha, geometry.opacity, &geometry.opacity_texture, 0.0,
        1.0);
  }
};

class SurfaceReader {
public:
  SurfaceReader(const HdMaterialNetwork2& network, const HdMaterialNode2& node,
      HdLotusMaterialTranslation& translation)
      : network_(network), node_(node), translation_(translation) {
  }

  // Clamped into [low, high]; a non-finite value keeps `value`. A texture
  // lookup that drives the input goes to `lookup`, when the input takes one.
  void Scalar(const TfToken& input, float& value,
      std::optional<Lotus::TextureInput>* lookup, double low, double high) {
    if (const VtValue* constant = Constant(input, lookup, false)) {
      if (const auto read = ReadScalar(*constant); read && std::isfinite(*read)) {
        value = static_cast<float>(std::clamp(*read, low, high));
      }
    }
  }

  // Each component clamped into [low, high]; a non-finite component keeps
  // the whole of `value`.
  // A UsdPrimvarReader_float3 on `input` names the colour set, and its
  // fallback, clamped, is the constant on a mesh without that set. Whether
  // the connection was one.
  bool ColorPrimvar(const TfToken& input, std::array<float, 3>& value,
      std::string& primvar) {
    const HdMaterialConnection2* connection = Connection(node_, input);
    if (connection == nullptr || connection->upstreamOutputName != Tokens->result) {
      return false;
    }
    const auto reader = network_.nodes.find(connection->upstreamNode);
    if (reader == network_.nodes.end() ||
        reader->second.nodeTypeId != Tokens->UsdPrimvarReader_float3) {
      return false;
    }
    primvar = ReadToken(Parameter(reader->second, Tokens->varname)).GetString();
    value = {0.0F, 0.0F, 0.0F};
    if (const VtValue* fallback = Parameter(reader->second, Tokens->fallback)) {
      const auto read = ReadColor(*fallback);
      if (read && std::all_of(read->begin(), read->end(),
                      [](double component) { return std::isfinite(component); })) {
        for (std::size_t c = 0; c < 3; ++c) {
          value[c] = static_cast<float>(std::clamp((*read)[c], 0.0, 1.0));
        }
      }
    }
    return true;
  }

  void Color(const TfToken& input, std::array<float, 3>& value,
      std::optional<Lotus::TextureInput>& lookup, double low, double high) {
    if (const VtValue* constant = Constant(input, &lookup, true)) {
      const auto read = ReadColor(*constant);
      if (read && std::all_of(read->begin(), read->end(),
                      [](double component) { return std::isfinite(component); })) {
        for (std::size_t c = 0; c < 3; ++c) {
          value[c] = static_cast<float>(std::clamp((*read)[c], low, high));
        }
      }
    }
  }

private:
  // The input's authored constant, or nothing when it is unauthored or
  // driven by a connection. A connection is translated into `lookup` when
  // it is a lookup this reads, and recorded as a connected input otherwise.
  const VtValue* Constant(const TfToken& input,
      std::optional<Lotus::TextureInput>* lookup, bool colour) {
    if (const HdMaterialConnection2* connection = Connection(node_, input)) {
      if (lookup == nullptr || !Lookup(*connection, colour, *lookup)) {
        const auto upstream = network_.nodes.find(connection->upstreamNode);
        Report(translation_,
            {Kind::UnsupportedGraph, input, connection->upstreamNode,
                upstream == network_.nodes.end() ? TfToken() : upstream->second.nodeTypeId,
                "a connection the UsdPreviewSurface reader does not translate"});
      }
      return nullptr;
    }
    return Parameter(node_, input);
  }

  // Where a lookup reads: a material's texture-coordinate source, and the
  // lookup's transform from it.
  struct Coordinates {
    std::string texcoords;
    std::array<float, 2> fallback{0.0F, 0.0F};
    std::array<double, 6> transform{1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
  };

  // A chain this long is cyclic or beyond any authored network.
  static constexpr int kMaxTransforms = 64;

  // The coordinates of a UsdPrimvarReader_float2's output, or of a
  // UsdTransform2d's: in * scale * rotate + translation, with the rotation
  // counter-clockwise in degrees. Nothing for another node or a connected
  // transform parameter. As for other parameters, a non-finite one keeps its
  // default.
  std::optional<Coordinates> ReadCoordinates(
      const HdMaterialConnection2& connection, int depth = 0) const {
    const auto upstream = network_.nodes.find(connection.upstreamNode);
    if (upstream == network_.nodes.end() || depth > kMaxTransforms) {
      return std::nullopt;
    }
    const HdMaterialNode2& node = upstream->second;
    Coordinates coordinates;
    if (node.nodeTypeId == Tokens->UsdPrimvarReader_float2) {
      coordinates.texcoords = ReadToken(Parameter(node, Tokens->varname)).GetString();
      coordinates.fallback = ReadVector<2, GfVec2f, GfVec2d>(Parameter(node, Tokens->fallback))
                                 .value_or(coordinates.fallback);
      return coordinates;
    }
    if (node.nodeTypeId != Tokens->UsdTransform2d || Connection(node, Tokens->rotation) ||
        Connection(node, Tokens->scale) || Connection(node, Tokens->translation)) {
      return std::nullopt;
    }
    if (const HdMaterialConnection2* in = Connection(node, Tokens->in)) {
      const auto read = ReadCoordinates(*in, depth + 1);
      if (!read) {
        return std::nullopt;
      }
      coordinates = *read;
    } else {
      coordinates.fallback = ReadVector<2, GfVec2f, GfVec2d>(Parameter(node, Tokens->in))
                                 .value_or(coordinates.fallback);
    }
    constexpr double kDegrees = 3.14159265358979323846 / 180.0;
    const VtValue* authored_rotation = Parameter(node, Tokens->rotation);
    const double read_rotation =
        authored_rotation != nullptr ? ReadScalar(*authored_rotation).value_or(0.0) : 0.0;
    const double radians = std::isfinite(read_rotation) ? read_rotation * kDegrees : 0.0;
    const double c = std::cos(radians), s = std::sin(radians);
    const auto scale = ReadVector<2, GfVec2f, GfVec2d>(Parameter(node, Tokens->scale))
                           .value_or(std::array<float, 2>{1.0F, 1.0F});
    const auto translation = ReadVector<2, GfVec2f, GfVec2d>(Parameter(node, Tokens->translation))
                                 .value_or(std::array<float, 2>{0.0F, 0.0F});
    const std::array<double, 6> local{c * scale[0], -s * scale[1], translation[0],
        s * scale[0], c * scale[1], translation[1]};
    const std::array<double, 6>& inner = coordinates.transform;
    coordinates.transform = {local[0] * inner[0] + local[1] * inner[3],
        local[0] * inner[1] + local[1] * inner[4],
        local[0] * inner[2] + local[1] * inner[5] + local[2],
        local[3] * inner[0] + local[4] * inner[3],
        local[3] * inner[1] + local[4] * inner[4],
        local[3] * inner[2] + local[4] * inner[5] + local[5]};
    return coordinates;
  }

  // A UsdUVTexture output, read as a colour (rgb) or a scalar (r, g, b or
  // a), whose st is constant, a UsdPrimvarReader_float2's or a UsdTransform2d
  // chain's from either. A material has one texture-coordinate source, its
  // first lookup's; a lookup with another is not translated. Each lookup has
  // its own transform.
  bool Lookup(const HdMaterialConnection2& connection, bool colour,
      std::optional<Lotus::TextureInput>& lookup) {
    const auto upstream = network_.nodes.find(connection.upstreamNode);
    if (upstream == network_.nodes.end() ||
        upstream->second.nodeTypeId != Tokens->UsdUVTexture) {
      return false;
    }
    const HdMaterialNode2& texture = upstream->second;
    const TfToken& output = connection.upstreamOutputName;
    Lotus::TextureInput result;
    if (colour) {
      if (output != Tokens->rgb) {
        return false;
      }
    } else if (output == Tokens->r) {
      result.channel = 0;
    } else if (output == Tokens->g) {
      result.channel = 1;
    } else if (output == Tokens->b) {
      result.channel = 2;
    } else if (output == Tokens->a) {
      result.channel = 3;
    } else {
      return false;
    }

    Coordinates coordinates;
    if (const HdMaterialConnection2* st = Connection(texture, Tokens->st)) {
      const auto read = ReadCoordinates(*st);
      if (!read) {
        return false;
      }
      coordinates = *read;
    } else {
      coordinates.fallback =
          ReadVector<2, GfVec2f, GfVec2d>(Parameter(texture, Tokens->st))
              .value_or(coordinates.fallback);
    }
    Lotus::Material& material = translation_.material;
    if (!SameTexcoords(material, coordinates.texcoords, coordinates.fallback)) {
      return false;
    }
    for (std::size_t i = 0; i < coordinates.transform.size(); ++i) {
      result.st_transform[i] = static_cast<float>(coordinates.transform[i]);
      if (!std::isfinite(result.st_transform[i])) {
        return false;
      }
    }

    result.wrap_s = ReadWrap(Parameter(texture, Tokens->wrapS));
    result.wrap_t = ReadWrap(Parameter(texture, Tokens->wrapT));
    result.scale = ReadVector<4, GfVec4f, GfVec4d>(Parameter(texture, Tokens->scale))
                       .value_or(result.scale);
    result.bias = ReadVector<4, GfVec4f, GfVec4d>(Parameter(texture, Tokens->bias))
                      .value_or(result.bias);
    result.fallback =
        ReadVector<4, GfVec4f, GfVec4d>(Parameter(texture, Tokens->fallback))
            .value_or(result.fallback);
    result.texture =
        RequestTexture(translation_, ReadFile(texture), ReadColorSpace(texture));
    material.geometry.texcoords = std::move(coordinates.texcoords);
    material.geometry.texcoord_fallback = coordinates.fallback;
    lookup = std::move(result);
    return true;
  }

  const HdMaterialNetwork2& network_;
  const HdMaterialNode2& node_;
  HdLotusMaterialTranslation& translation_;
};

} // namespace

std::string HdLotusTextureKey(const std::string& file,
    const TfToken& source_color_space) {
  return file + "|" + source_color_space.GetString();
}

const char* HdLotusMaterialDiagnosticKindName(HdLotusMaterialDiagnosticKind kind) {
  switch (kind) {
  case Kind::UnsupportedSurface: return "unsupported surface";
  case Kind::UnsupportedTerminal: return "unsupported terminal";
  case Kind::UnsupportedNode: return "unsupported node";
  case Kind::UnsupportedGraph: return "unsupported graph";
  case Kind::UnsupportedValue: return "unsupported value";
  case Kind::UnrepresentedInput: return "unrepresented input";
  case Kind::UnknownInput: return "unknown input";
  case Kind::Substituted: return "substituted";
  }
  return "";
}

const std::vector<HdLotusNodeDefinition>& HdLotusMaterialNodeDefinitions() {
  static const std::vector<HdLotusNodeDefinition> definitions = [] {
    std::vector<HdLotusNodeDefinition> built;
    for (const Graph::NodeDef& def : Graph::NodeDefs()) {
      HdLotusNodeDefinition definition{def.id, {}};
      for (const Graph::InputDef& input : def.inputs) {
        const std::size_t width =
            input.type == Graph::Type::Integer || input.type == Graph::Type::Boolean
                ? 1
                : Graph::Width(input.type);
        definition.inputs.push_back({input.name, TfToken(Graph::Name(input.type)),
            input.has_value,
            std::vector<double>(input.value.begin(),
                input.value.begin() + static_cast<std::ptrdiff_t>(width)),
            input.text});
      }
      built.push_back(std::move(definition));
    }
    return built;
  }();
  return definitions;
}

const TfTokenVector& HdLotusMaterialRenderContexts() {
  static const TfTokenVector contexts{Tokens->mtlx};
  return contexts;
}

HdLotusMaterialTranslation HdLotusTranslateMaterial(
    const HdMaterialNetworkMap& network) {
  HdLotusMaterialTranslation translation;
  const HdMaterialNetwork2 converted = HdConvertToHdMaterialNetwork2(network);
  // Lotus renders a surface: a displacement or a volume does nothing.
  for (const TfToken& terminal :
      {HdMaterialTerminalTokens->displacement, HdMaterialTerminalTokens->volume}) {
    const auto connected = converted.terminals.find(terminal);
    if (connected == converted.terminals.end()) {
      continue;
    }
    const auto upstream = converted.nodes.find(connected->second.upstreamNode);
    translation.diagnostics.push_back({Kind::UnsupportedTerminal, {},
        connected->second.upstreamNode,
        upstream == converted.nodes.end() ? TfToken() : upstream->second.nodeTypeId,
        "the " + terminal.GetString() + " terminal is not rendered"});
  }
  const auto unsupported = [&](const SdfPath& path, const TfToken& type,
                               std::string detail) {
    translation.diagnostics.push_back(
        {Kind::UnsupportedSurface, {}, path, type, std::move(detail)});
    return translation;
  };
  const auto terminal =
      converted.terminals.find(HdMaterialTerminalTokens->surface);
  if (terminal == converted.terminals.end()) {
    return unsupported({}, {}, "the material has no surface terminal");
  }
  const auto node = converted.nodes.find(terminal->second.upstreamNode);
  if (node == converted.nodes.end()) {
    return unsupported(terminal->second.upstreamNode, {},
        "the surface terminal names no shader node");
  }
  if (node->second.nodeTypeId == Tokens->standardSurface) {
    StandardSurfaceReader(converted, node->first, node->second,
        *Graph::FindNodeDef(Tokens->standardSurface), translation)
        .Translate();
    return translation;
  }
  if (node->second.nodeTypeId == Tokens->gltfPbr) {
    GltfPbrReader(converted, node->first, node->second,
        *Graph::FindNodeDef(Tokens->gltfPbr), translation)
        .Translate();
    return translation;
  }
  if (node->second.nodeTypeId != Tokens->UsdPreviewSurface &&
      node->second.nodeTypeId != Tokens->mtlxPreviewSurface) {
    return unsupported(node->first, node->second.nodeTypeId,
        "the surface shader is " + node->second.nodeTypeId.GetString() +
            ", none of UsdPreviewSurface, "
            "ND_standard_surface_surfaceshader and "
            "ND_gltf_pbr_surfaceshader");
  }

  SurfaceReader surface(converted, node->second, translation);
  Lotus::Material& material = translation.material;
  Lotus::PreviewSurface& preview = Lotus::Preview(material);
  constexpr double kLargest = 3.4e38;
  if (!surface.ColorPrimvar(Tokens->diffuseColor, preview.base_color,
          material.geometry.base_color_primvar)) {
    surface.Color(Tokens->diffuseColor, preview.base_color,
        preview.base_color_texture, 0.0, 1.0);
  }
  surface.Color(Tokens->emissiveColor, preview.emission,
      preview.emission_texture, 0.0, kLargest);
  surface.Scalar(Tokens->roughness, preview.roughness,
      &preview.roughness_texture, 0.0, 1.0);
  float specular_workflow = 0.0F;
  surface.Scalar(Tokens->useSpecularWorkflow, specular_workflow, nullptr, 0.0,
      1.0);
  // The specular workflow ignores metallic.
  if (specular_workflow == 0.0F) {
    surface.Scalar(Tokens->metallic, preview.metallic,
        &preview.metallic_texture, 0.0, 1.0);
    surface.Scalar(Tokens->ior, preview.ior, nullptr, 1.0, kLargest);
  }
  preview.use_specular_workflow = specular_workflow != 0.0F;
  surface.Color(Tokens->normal, material.geometry.normal,
      material.geometry.normal_texture, -1.0, 1.0);
  surface.Scalar(Tokens->opacity, material.geometry.opacity,
      &material.geometry.opacity_texture, 0.0, 1.0);
  surface.Scalar(Tokens->opacityThreshold, material.geometry.opacity_threshold,
      nullptr, 0.0, 1.0);
  if (preview.use_specular_workflow) {
    surface.Color(Tokens->specularColor, preview.specular_color,
        preview.specular_color_texture, 0.0, 1.0);
  }
  return translation;
}

PXR_NAMESPACE_CLOSE_SCOPE
