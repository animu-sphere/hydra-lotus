// SPDX-License-Identifier: Apache-2.0
#include "material_translator.hpp"

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

// A token parameter, authored as a token or a string.
TfToken ReadToken(const VtValue* value) {
  if (value != nullptr && value->IsHolding<TfToken>()) {
    return value->UncheckedGet<TfToken>();
  }
  if (value != nullptr && value->IsHolding<std::string>()) {
    return TfToken(value->UncheckedGet<std::string>());
  }
  return {};
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

const VtValue* Parameter(const HdMaterialNode2& node, const TfToken& name) {
  const auto parameter = node.parameters.find(name);
  return parameter == node.parameters.end() ? nullptr : &parameter->second;
}

const HdMaterialConnection2* Connection(const HdMaterialNode2& node,
    const TfToken& input) {
  const auto connected = node.inputConnections.find(input);
  return connected == node.inputConnections.end() || connected->second.empty()
             ? nullptr
             : &connected->second.front();
}

// A file input's image path: resolved, or as authored when it does not
// resolve; empty when there is none.
std::string ReadFile(const HdMaterialNode2& node) {
  if (const VtValue* asset = Parameter(node, Tokens->file)) {
    if (asset->IsHolding<SdfAssetPath>()) {
      const SdfAssetPath& path = asset->UncheckedGet<SdfAssetPath>();
      return path.GetResolvedPath().empty() ? path.GetAssetPath()
                                            : path.GetResolvedPath();
    }
    if (asset->IsHolding<std::string>()) {
      return asset->UncheckedGet<std::string>();
    }
  }
  return {};
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
         (material.texcoords == texcoords && material.texcoord_fallback == fallback);
}

// One output of a MaterialX node graph: constant components, or a texture
// lookup whose channels hold them.
struct GraphValue {
  // 1 (float) to 4 (color4 and vector4).
  std::size_t width = 1;
  std::array<float, 4> constant{};
  std::optional<Lotus::TextureInput> lookup;
  // Where the lookup reads, or, for a texture-coordinate node, the mesh's
  // coordinate set and the value on a mesh without it.
  std::string texcoords;
  std::array<float, 2> texcoord_fallback{0.0F, 0.0F};
  // A texture-coordinate node's output, which only an image's texcoord
  // input reads.
  bool coordinates = false;
  // An ND_normalmap's output: a tangent-space normal.
  bool normal_map = false;
  // The lookup's image, which the translation requests once a material
  // input takes the lookup.
  std::string file;
  TfToken color_space;
};

GraphValue ConstantValue(std::initializer_list<float> components) {
  GraphValue value;
  value.width = components.size();
  std::copy(components.begin(), components.end(), value.constant.begin());
  return value;
}

// A MaterialX value of one to four finite components: a scalar, a vector or
// a colour of either precision.
std::optional<GraphValue> ReadComponents(const VtValue& value) {
  GraphValue result;
  std::array<double, 4> read{};
  if (const auto scalar = ReadScalar(value)) {
    read[0] = *scalar;
  } else if (value.IsHolding<GfVec2f>() || value.IsHolding<GfVec2d>()) {
    result.width = 2;
    const GfVec2d vector = value.IsHolding<GfVec2f>()
                               ? GfVec2d(value.UncheckedGet<GfVec2f>())
                               : value.UncheckedGet<GfVec2d>();
    read = {vector[0], vector[1], 0.0, 0.0};
  } else if (const auto color = ReadColor(value)) {
    result.width = 3;
    read = {(*color)[0], (*color)[1], (*color)[2], 0.0};
  } else if (value.IsHolding<GfVec4f>() || value.IsHolding<GfVec4d>()) {
    result.width = 4;
    const GfVec4d vector = value.IsHolding<GfVec4f>()
                               ? GfVec4d(value.UncheckedGet<GfVec4f>())
                               : value.UncheckedGet<GfVec4d>();
    read = {vector[0], vector[1], vector[2], vector[3]};
  } else {
    return std::nullopt;
  }
  for (std::size_t c = 0; c < result.width; ++c) {
    result.constant[c] = static_cast<float>(read[c]);
    if (!std::isfinite(result.constant[c])) {
      return std::nullopt;
    }
  }
  return result;
}

// The component count of a MaterialX type named at the end of `id`, as in
// ND_image_color3; 0 for other types.
std::size_t TypeWidth(std::string_view id) {
  constexpr std::pair<std::string_view, std::size_t> kTypes[] = {{"_float", 1},
      {"_vector2", 2}, {"_color3", 3}, {"_vector3", 3}, {"_color4", 4},
      {"_vector4", 4}};
  for (const auto& [suffix, width] : kTypes) {
    if (id.ends_with(suffix)) return width;
  }
  return 0;
}

// An ND_image address mode; constant reads transparent black outside the
// image, which is its default value only when that is zero.
Lotus::TextureWrap ReadAddressMode(const VtValue* value) {
  const TfToken mode = ReadToken(value);
  if (mode == Tokens->constant) return Lotus::TextureWrap::Black;
  if (mode == Tokens->clamp) return Lotus::TextureWrap::Clamp;
  if (mode == Tokens->mirror) return Lotus::TextureWrap::Mirror;
  return Lotus::TextureWrap::Repeat;
}

// Evaluates MaterialX node graphs into constants and texture lookups.
class GraphReader {
public:
  explicit GraphReader(const HdMaterialNetwork2& network) : network_(network) {
  }

  // An input of `node`: its connection's value, its authored constant, or
  // `fallback` when it has neither or the constant is not a finite value.
  // Nothing when the connection is a graph this does not translate.
  std::optional<GraphValue> Input(const HdMaterialNode2& node,
      const TfToken& input, const GraphValue& fallback) {
    if (const HdMaterialConnection2* connection = Connection(node, input)) {
      return Output(*connection, 0);
    }
    if (const VtValue* constant = Parameter(node, input)) {
      if (auto read = ReadComponents(*constant)) {
        return read;
      }
    }
    return fallback;
  }

private:
  // A graph this deep is cyclic or beyond any authored network.
  static constexpr int kMaxDepth = 64;

  std::optional<GraphValue> Output(const HdMaterialConnection2& connection,
      int depth) {
    const auto upstream = network_.nodes.find(connection.upstreamNode);
    if (depth > kMaxDepth || upstream == network_.nodes.end()) {
      return std::nullopt;
    }
    const HdMaterialNode2& node = upstream->second;
    const std::string& id = node.nodeTypeId.GetString();
    if (id.starts_with("ND_image_") || id.starts_with("ND_tiledimage_")) {
      return Image(node, id.starts_with("ND_tiledimage_"), depth);
    }
    if (node.nodeTypeId == Tokens->texcoordVector2) {
      if (const VtValue* index = Parameter(node, Tokens->index);
          index != nullptr && ReadScalar(*index).value_or(-1.0) != 0.0) {
        return std::nullopt;
      }
      GraphValue value;
      value.coordinates = true;
      value.texcoords = "st";
      return value;
    }
    if (node.nodeTypeId == Tokens->geompropvalueVector2) {
      GraphValue value;
      value.coordinates = true;
      value.texcoords = ReadToken(Parameter(node, Tokens->geomprop)).GetString();
      value.texcoord_fallback =
          ReadVector<2, GfVec2f, GfVec2d>(Parameter(node, Tokens->defaultValue))
              .value_or(value.texcoord_fallback);
      return value;
    }
    if (id.starts_with("ND_extract_")) {
      return Extract(node, depth);
    }
    if (id.starts_with("ND_convert_")) {
      return Convert(node, id, depth);
    }
    if (id.starts_with("ND_normalmap")) {
      return NormalMap(node, depth);
    }
    return std::nullopt;
  }

  // An input inside the graph, as Input reads it.
  std::optional<GraphValue> Upstream(const HdMaterialNode2& node,
      const TfToken& input, const GraphValue& fallback, int depth) {
    if (const HdMaterialConnection2* connection = Connection(node, input)) {
      return Output(*connection, depth + 1);
    }
    if (const VtValue* constant = Parameter(node, input)) {
      if (auto read = ReadComponents(*constant)) {
        return read;
      }
    }
    return fallback;
  }

  // ND_image and untiled ND_tiledimage: a lookup of the file at the
  // texcoord input (the st primvar by default), or, without a file, the
  // default value. Colour spaces other than sRGB's are read as raw.
  std::optional<GraphValue> Image(const HdMaterialNode2& node, bool tiled,
      int depth) {
    const std::size_t width = TypeWidth(node.nodeTypeId.GetString());
    if (width == 0 || width == 2) {
      return std::nullopt;
    }
    if (tiled) {
      const auto identity = [&](const TfToken& name, float component) {
        const VtValue* value = Parameter(node, name);
        const auto read = value != nullptr
                              ? ReadVector<2, GfVec2f, GfVec2d>(value)
                              : std::optional<std::array<float, 2>>(
                                    std::array<float, 2>{component, component});
        return read == std::array<float, 2>{component, component};
      };
      if (!identity(Tokens->uvtiling, 1.0F) || !identity(Tokens->uvoffset, 0.0F) ||
          Connection(node, Tokens->uvtiling) || Connection(node, Tokens->uvoffset) ||
          Parameter(node, Tokens->realworldimagesize) ||
          Parameter(node, Tokens->realworldtilesize) ||
          Connection(node, Tokens->realworldimagesize) ||
          Connection(node, Tokens->realworldtilesize)) {
        return std::nullopt;
      }
    }
    GraphValue value;
    value.width = width;
    value.constant = {0.0F, 0.0F, 0.0F, width == 4 ? 0.0F : 1.0F};
    if (const VtValue* fallback = Parameter(node, Tokens->defaultValue)) {
      if (const auto read = ReadComponents(*fallback); read && read->width == width) {
        for (std::size_t c = 0; c < width; ++c) value.constant[c] = read->constant[c];
      }
    }
    if (width == 1) {
      value.constant = {value.constant[0], value.constant[0], value.constant[0], 1.0F};
    }
    const std::string file = ReadFile(node);
    if (file.empty()) {
      return value;
    }

    value.texcoords = "st";
    if (const HdMaterialConnection2* texcoord = Connection(node, Tokens->texcoord)) {
      const auto coordinates = Output(*texcoord, depth + 1);
      if (!coordinates || !coordinates->coordinates) {
        return std::nullopt;
      }
      value.texcoords = coordinates->texcoords;
      value.texcoord_fallback = coordinates->texcoord_fallback;
    } else if (const auto constant = ReadVector<2, GfVec2f, GfVec2d>(
                   Parameter(node, Tokens->texcoord))) {
      value.texcoords.clear();
      value.texcoord_fallback = *constant;
    }
    const TfToken color_space = ReadColorSpace(node) == Tokens->sRGB
                                    ? Tokens->sRGB
                                    : Tokens->raw;
    Lotus::TextureInput lookup;
    lookup.texture = HdLotusTextureKey(file, color_space);
    value.file = file;
    value.color_space = color_space;
    lookup.wrap_s = ReadAddressMode(Parameter(node, Tokens->uaddressmode));
    lookup.wrap_t = ReadAddressMode(Parameter(node, Tokens->vaddressmode));
    lookup.fallback = value.constant;
    value.lookup = std::move(lookup);
    return value;
  }

  // ND_extract: one component, a lookup's channel.
  std::optional<GraphValue> Extract(const HdMaterialNode2& node, int depth) {
    auto value = Upstream(node, Tokens->in, ConstantValue({0, 0, 0}), depth);
    const auto index = ReadScalar(
        Parameter(node, Tokens->index) ? *Parameter(node, Tokens->index) : VtValue(0));
    if (!value || value->coordinates || value->normal_map || !index ||
        *index < 0 || *index >= static_cast<double>(value->width) ||
        Connection(node, Tokens->index)) {
      return std::nullopt;
    }
    const auto channel = static_cast<std::uint32_t>(*index);
    GraphValue result = *value;
    result.width = 1;
    result.constant = {value->constant[channel], value->constant[channel],
        value->constant[channel], 1.0F};
    if (result.lookup) {
      if (result.lookup->channel != 0) {
        return std::nullopt;
      }
      result.lookup->channel = channel;
    }
    return result;
  }

  // ND_convert between scalars, vectors and colours: a scalar constant
  // spreads to every component; a lookup keeps its channels, so it converts
  // only between types of three or four components.
  std::optional<GraphValue> Convert(const HdMaterialNode2& node,
      const std::string& id, int depth) {
    const std::size_t to = TypeWidth(id);
    const std::size_t from = TypeWidth(id.substr(0, id.rfind('_')));
    auto value = Upstream(node, Tokens->in, GraphValue{from}, depth);
    if (to == 0 || from == 0 || !value || value->coordinates || value->normal_map ||
        value->width != from) {
      return std::nullopt;
    }
    if (value->lookup) {
      if (from < 3 || to < 3) {
        return std::nullopt;
      }
    } else if (from == 1) {
      value->constant.fill(value->constant[0]);
    } else if (to == 4 && from < 4) {
      value->constant[3] = 1.0F;
    }
    value->width = to;
    return value;
  }

  // ND_normalmap: the image's unsigned tangent-space normal, signed and its
  // x and y scaled, in the default tangent frame.
  std::optional<GraphValue> NormalMap(const HdMaterialNode2& node, int depth) {
    for (const TfToken& frame : {Tokens->normal, Tokens->tangent, Tokens->bitangent}) {
      if (Parameter(node, frame) != nullptr || Connection(node, frame) != nullptr) {
        return std::nullopt;
      }
    }
    if (const VtValue* space = Parameter(node, Tokens->space);
        space != nullptr && ReadToken(space) != TfToken("tangent")) {
      return std::nullopt;
    }
    auto value = Upstream(node, Tokens->in, ConstantValue({0.5F, 0.5F, 1.0F}), depth);
    auto scale = Upstream(node, Tokens->scale, ConstantValue({1.0F}), depth);
    if (!value || value->coordinates || value->normal_map || value->width != 3 ||
        (value->lookup && value->lookup->channel != 0) || !scale ||
        scale->lookup || scale->coordinates || scale->width > 2) {
      return std::nullopt;
    }
    const std::array<float, 3> factor{scale->constant[0],
        scale->width == 2 ? scale->constant[1] : scale->constant[0], 1.0F};
    value->normal_map = true;
    for (std::size_t c = 0; c < 3; ++c) {
      value->constant[c] = (2.0F * value->constant[c] - 1.0F) * factor[c];
      if (value->lookup) {
        Lotus::TextureInput& lookup = *value->lookup;
        lookup.scale[c] = 2.0F * lookup.scale[c] * factor[c];
        lookup.bias[c] = (2.0F * lookup.bias[c] - 1.0F) * factor[c];
        lookup.fallback[c] = (2.0F * lookup.fallback[c] - 1.0F) * factor[c];
      }
    }
    return value;
  }

  const HdMaterialNetwork2& network_;
};

// `value` times `weight`, component by component, where a scalar scales
// every component. Nothing when both are lookups, or a scalar lookup would
// have to spread over a colour.
std::optional<GraphValue> Multiply(GraphValue value, GraphValue weight) {
  if (value.lookup && weight.lookup) {
    return std::nullopt;
  }
  if (weight.lookup) {
    std::swap(value, weight);
  }
  if (value.lookup && value.width == 1 && weight.width > 1) {
    return std::nullopt;
  }
  const auto factor = [&](std::size_t c) {
    return weight.width == 1 ? weight.constant[0] : weight.constant[c];
  };
  if (value.lookup) {
    Lotus::TextureInput& lookup = *value.lookup;
    const std::size_t first = value.width == 1 ? lookup.channel : 0;
    const std::size_t last = value.width == 1 ? lookup.channel + 1 : 3;
    for (std::size_t c = first; c < last; ++c) {
      const float f = factor(value.width == 1 ? 0 : c);
      lookup.scale[c] *= f;
      lookup.bias[c] *= f;
      lookup.fallback[c] *= f;
    }
    return value;
  }
  const std::size_t width = std::max(value.width, weight.width);
  if (value.width == 1) {
    value.constant.fill(value.constant[0]);
  }
  for (std::size_t c = 0; c < width; ++c) {
    value.constant[c] *= factor(c);
  }
  value.width = width;
  return value;
}

// Translates an ND_standard_surface_surfaceshader node.
class StandardSurfaceReader {
public:
  StandardSurfaceReader(const HdMaterialNetwork2& network,
      const HdMaterialNode2& node, HdLotusMaterialTranslation& translation)
      : node_(node), translation_(translation), graph_(network) {
  }

  void Translate() {
    Lotus::Material& material = translation_.material;
    constexpr double kLargest = 3.4e38;
    // Standard Surface's defaults where the IR's differ.
    material.base_color = {0.8F, 0.8F, 0.8F};
    material.roughness = 0.2F;

    Color(Weighted(Tokens->base, Tokens->base_color, ConstantValue({0.8F, 0.8F, 0.8F})),
        material.base_color, material.base_color_texture, 0.0, 1.0);
    Color(Weighted(Tokens->emission, Tokens->emission_color,
              ConstantValue({1.0F, 1.0F, 1.0F}), 0.0F),
        material.emission, material.emission_texture, 0.0, kLargest);
    Scalar(Tokens->metalness, material.metallic, &material.metallic_texture,
        0.0, 1.0);
    Scalar(Tokens->specular_roughness, material.roughness,
        &material.roughness_texture, 0.0, 1.0);
    Scalar(Tokens->specular_IOR, material.ior, nullptr, 1.0, kLargest);
    Transmission();
    Specular();
    Opacity();
    Normal();
    for (const TfToken& lobe : {Tokens->coat, Tokens->diffuse_roughness,
             Tokens->sheen, Tokens->specular_anisotropy, Tokens->subsurface,
             Tokens->thin_film_thickness}) {
      const auto weight = graph_.Input(node_, lobe, ConstantValue({0.0F}));
      if (!weight || weight->lookup || weight->constant[0] != 0.0F) {
        translation_.unrepresented_inputs.push_back(lobe);
      }
    }
  }

private:
  // A value read from one or two inputs, nothing when it is a graph this
  // does not translate, and which of those inputs are connected.
  struct Reading {
    std::optional<GraphValue> value;
    std::vector<TfToken> connected;
  };

  Reading Input(const TfToken& input, const GraphValue& fallback) {
    Reading reading{graph_.Input(node_, input, fallback), {}};
    if (Connection(node_, input) != nullptr) {
      reading.connected.push_back(input);
    }
    if (reading.value && (reading.value->coordinates || reading.value->normal_map ||
                             reading.value->width != fallback.width)) {
      reading.value.reset();
    }
    return reading;
  }

  // A colour times its scalar weight, as base and emission are.
  Reading Weighted(const TfToken& weight, const TfToken& color,
      const GraphValue& color_fallback, float weight_fallback = 1.0F) {
    const Reading scale = Input(weight, ConstantValue({weight_fallback}));
    Reading reading = Input(color, color_fallback);
    reading.connected.insert(
        reading.connected.begin(), scale.connected.begin(), scale.connected.end());
    reading.value = scale.value && reading.value
                        ? Multiply(*reading.value, *scale.value)
                        : std::nullopt;
    return reading;
  }

  // Whether a lookup at the value's coordinates fits the material, which
  // then reads there and requests its image.
  bool Place(const GraphValue& value) {
    Lotus::Material& material = translation_.material;
    if (!SameTexcoords(material, value.texcoords, value.texcoord_fallback)) {
      return false;
    }
    material.texcoords = value.texcoords;
    material.texcoord_fallback = value.texcoord_fallback;
    RequestTexture(translation_, value.file, value.color_space);
    return true;
  }

  // A reading the material cannot take leaves the input's default and
  // reports its connected inputs.
  void Untranslated(const Reading& reading) {
    translation_.connected_inputs.insert(translation_.connected_inputs.end(),
        reading.connected.begin(), reading.connected.end());
  }

  void Color(const Reading& reading, std::array<float, 3>& value,
      std::optional<Lotus::TextureInput>& lookup, double low, double high) {
    const auto& result = reading.value;
    if (!result || (result->lookup &&
                       (result->lookup->channel != 0 || !Place(*result)))) {
      Untranslated(reading);
      return;
    }
    if (result->lookup) {
      lookup = result->lookup;
      return;
    }
    for (std::size_t c = 0; c < 3; ++c) {
      value[c] = static_cast<float>(std::clamp<double>(result->constant[c], low, high));
    }
  }

  void Scalar(const TfToken& input, float& value,
      std::optional<Lotus::TextureInput>* lookup, double low, double high) {
    const Reading reading = Input(input, ConstantValue({value}));
    const auto& result = reading.value;
    if (!result || (result->lookup && (lookup == nullptr || !Place(*result)))) {
      Untranslated(reading);
      return;
    }
    if (result->lookup) {
      *lookup = result->lookup;
      return;
    }
    value = static_cast<float>(std::clamp<double>(result->constant[0], low, high));
  }

  // A constant transmission weight and colour, and whether the surface is
  // a thin wall (docs/design/TRANSMISSION.md section 9). A graph leaves the
  // input's default. Under a positive transmission_depth the colour is
  // absorption at that depth inside the medium, which is not represented:
  // the transmitted light stays untinted. The medium's scattering,
  // dispersion and extra roughness are ignored.
  void Transmission() {
    Lotus::Material& material = translation_.material;
    const auto weight = graph_.Input(node_, Tokens->transmission, ConstantValue({0.0F}));
    if (!weight || weight->lookup || weight->width != 1) {
      translation_.unrepresented_inputs.push_back(Tokens->transmission);
    } else {
      material.transmission =
          static_cast<float>(std::clamp<double>(weight->constant[0], 0.0, 1.0));
    }
    if (Connection(node_, Tokens->thin_walled) != nullptr) {
      translation_.unrepresented_inputs.push_back(Tokens->thin_walled);
    } else if (const VtValue* thin = Parameter(node_, Tokens->thin_walled)) {
      if (thin->IsHolding<bool>()) {
        material.thin_walled = thin->UncheckedGet<bool>();
      } else if (const auto flag = ReadScalar(*thin)) {
        material.thin_walled = *flag != 0.0;
      }
    }
    if (!(material.transmission > 0.0F)) {
      return;
    }
    const auto color = graph_.Input(node_, Tokens->transmission_color,
        ConstantValue({1.0F, 1.0F, 1.0F}));
    if (!color || color->lookup || color->width != 3) {
      translation_.unrepresented_inputs.push_back(Tokens->transmission_color);
    } else {
      for (std::size_t c = 0; c < 3; ++c) {
        material.transmission_color[c] =
            static_cast<float>(std::clamp<double>(color->constant[c], 0.0, 1.0));
      }
    }
    const auto depth = graph_.Input(node_, Tokens->transmission_depth, ConstantValue({0.0F}));
    if (!depth || depth->lookup || depth->constant[0] != 0.0F) {
      translation_.unrepresented_inputs.push_back(Tokens->transmission_depth);
      material.transmission_color = {1.0F, 1.0F, 1.0F};
    }
    const auto scatter = graph_.Input(node_, Tokens->transmission_scatter,
        ConstantValue({0.0F, 0.0F, 0.0F}));
    if (!scatter || scatter->lookup || scatter->constant[0] != 0.0F ||
        scatter->constant[1] != 0.0F || scatter->constant[2] != 0.0F) {
      translation_.unrepresented_inputs.push_back(Tokens->transmission_scatter);
    }
    for (const TfToken& input : {Tokens->transmission_scatter_anisotropy,
             Tokens->transmission_dispersion, Tokens->transmission_extra_roughness}) {
      const auto value = graph_.Input(node_, input, ConstantValue({0.0F}));
      if (!value || value->lookup || value->constant[0] != 0.0F) {
        translation_.unrepresented_inputs.push_back(input);
      }
    }
  }

  // specular 0 removes the dielectric interface; specular 1 with a white
  // specular_color is the IR's. Other weights and tints are not represented.
  // Transmission refracts with specular_IOR whatever the specular weight,
  // so the interface's reflection stays too.
  void Specular() {
    const auto weight = graph_.Input(node_, Tokens->specular, ConstantValue({1.0F}));
    if (weight && !weight->lookup && weight->constant[0] == 0.0F &&
        !(translation_.material.transmission > 0.0F)) {
      translation_.material.ior = 1.0F;
      return;
    }
    if (!weight || weight->lookup || weight->constant[0] != 1.0F) {
      translation_.unrepresented_inputs.push_back(Tokens->specular);
    }
    const auto tint = graph_.Input(node_, Tokens->specular_color,
        ConstantValue({1.0F, 1.0F, 1.0F}));
    if (!tint || tint->lookup || tint->constant[0] != 1.0F ||
        tint->constant[1] != 1.0F || tint->constant[2] != 1.0F) {
      translation_.unrepresented_inputs.push_back(Tokens->specular_color);
    }
  }

  // A grey constant opacity is coverage; a coloured one is not represented,
  // and its mean is used.
  void Opacity() {
    const Reading reading = Input(Tokens->opacity, ConstantValue({1.0F, 1.0F, 1.0F}));
    const auto& result = reading.value;
    if (!result || result->lookup) {
      Untranslated(reading);
      return;
    }
    const auto& [r, g, b, a] = result->constant;
    if (r != g || g != b) {
      translation_.unrepresented_inputs.push_back(Tokens->opacity);
    }
    translation_.material.opacity = static_cast<float>(
        std::clamp((double(r) + double(g) + double(b)) / 3.0, 0.0, 1.0));
  }

  // A normal map, or the constant of one without an image; an authored
  // world-space normal is not represented.
  void Normal() {
    Lotus::Material& material = translation_.material;
    if (Connection(node_, Tokens->normal) == nullptr) {
      if (Parameter(node_, Tokens->normal) != nullptr) {
        translation_.unrepresented_inputs.push_back(Tokens->normal);
      }
      return;
    }
    const auto value = graph_.Input(node_, Tokens->normal, GraphValue{});
    if (!value || !value->normal_map || (value->lookup && !Place(*value))) {
      translation_.connected_inputs.push_back(Tokens->normal);
      return;
    }
    if (value->lookup) {
      material.normal_texture = value->lookup;
      return;
    }
    for (std::size_t c = 0; c < 3; ++c) {
      material.normal[c] = std::clamp(value->constant[c], -1.0F, 1.0F);
    }
  }

  const HdMaterialNode2& node_;
  HdLotusMaterialTranslation& translation_;
  GraphReader graph_;
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
        translation_.connected_inputs.push_back(input);
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
    material.texcoords = std::move(coordinates.texcoords);
    material.texcoord_fallback = coordinates.fallback;
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

const TfTokenVector& HdLotusMaterialRenderContexts() {
  static const TfTokenVector contexts{Tokens->mtlx};
  return contexts;
}

HdLotusMaterialTranslation HdLotusTranslateMaterial(
    const HdMaterialNetworkMap& network) {
  HdLotusMaterialTranslation translation;
  const HdMaterialNetwork2 converted = HdConvertToHdMaterialNetwork2(network);
  const auto terminal =
      converted.terminals.find(HdMaterialTerminalTokens->surface);
  if (terminal == converted.terminals.end()) {
    translation.unsupported = "the material has no surface terminal";
    return translation;
  }
  const auto node = converted.nodes.find(terminal->second.upstreamNode);
  if (node == converted.nodes.end()) {
    translation.unsupported = "the surface terminal names no shader node";
    return translation;
  }
  if (node->second.nodeTypeId == Tokens->standardSurface) {
    StandardSurfaceReader(converted, node->second, translation).Translate();
    return translation;
  }
  if (node->second.nodeTypeId != Tokens->UsdPreviewSurface &&
      node->second.nodeTypeId != Tokens->mtlxPreviewSurface) {
    translation.unsupported = "the surface shader is " +
                              node->second.nodeTypeId.GetString() +
                              ", neither UsdPreviewSurface nor "
                              "ND_standard_surface_surfaceshader";
    return translation;
  }

  SurfaceReader surface(converted, node->second, translation);
  Lotus::Material& material = translation.material;
  constexpr double kLargest = 3.4e38;
  if (!surface.ColorPrimvar(Tokens->diffuseColor, material.base_color,
          material.base_color_primvar)) {
    surface.Color(Tokens->diffuseColor, material.base_color,
        material.base_color_texture, 0.0, 1.0);
  }
  surface.Color(Tokens->emissiveColor, material.emission,
      material.emission_texture, 0.0, kLargest);
  surface.Scalar(Tokens->roughness, material.roughness,
      &material.roughness_texture, 0.0, 1.0);
  float specular_workflow = 0.0F;
  surface.Scalar(Tokens->useSpecularWorkflow, specular_workflow, nullptr, 0.0,
      1.0);
  // The specular workflow ignores metallic.
  if (specular_workflow == 0.0F) {
    surface.Scalar(Tokens->metallic, material.metallic,
        &material.metallic_texture, 0.0, 1.0);
    surface.Scalar(Tokens->ior, material.ior, nullptr, 1.0, kLargest);
  }
  material.use_specular_workflow = specular_workflow != 0.0F;
  surface.Color(Tokens->normal, material.normal,
      material.normal_texture, -1.0, 1.0);
  surface.Scalar(Tokens->opacity, material.opacity,
      &material.opacity_texture, 0.0, 1.0);
  surface.Scalar(Tokens->opacityThreshold, material.opacity_threshold,
      nullptr, 0.0, 1.0);
  if (material.use_specular_workflow) {
    surface.Color(Tokens->specularColor, material.specular_color,
        material.specular_color_texture, 0.0, 1.0);
  }
  return translation;
}

PXR_NAMESPACE_CLOSE_SCOPE
