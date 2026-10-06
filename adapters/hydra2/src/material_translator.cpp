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
#include <optional>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

TF_DEFINE_PRIVATE_TOKENS(Tokens,
    (UsdPreviewSurface)
    (UsdUVTexture)
    (UsdPrimvarReader_float2)
    (diffuseColor)
    (emissiveColor)
    (metallic)
    (roughness)
    (useSpecularWorkflow)
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
    ((automatic, "auto")));

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
  static const VtValue* Parameter(const HdMaterialNode2& node,
      const TfToken& name) {
    const auto parameter = node.parameters.find(name);
    return parameter == node.parameters.end() ? nullptr : &parameter->second;
  }

  static const HdMaterialConnection2* Connection(const HdMaterialNode2& node,
      const TfToken& input) {
    const auto connected = node.inputConnections.find(input);
    return connected == node.inputConnections.end() || connected->second.empty()
               ? nullptr
               : &connected->second.front();
  }

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

  // A UsdUVTexture output, read as a colour (rgb) or a scalar (r, g, b or
  // a), whose st is constant or a UsdPrimvarReader_float2's. A material has
  // one texture-coordinate source, its first lookup's; a lookup with
  // another is not translated.
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

    std::string texcoords;
    std::array<float, 2> texcoord_fallback{0.0F, 0.0F};
    if (const HdMaterialConnection2* st = Connection(texture, Tokens->st)) {
      const auto reader = network_.nodes.find(st->upstreamNode);
      if (reader == network_.nodes.end() ||
          reader->second.nodeTypeId != Tokens->UsdPrimvarReader_float2) {
        return false;
      }
      texcoords = ReadToken(Parameter(reader->second, Tokens->varname)).GetString();
      texcoord_fallback =
          ReadVector<2, GfVec2f, GfVec2d>(Parameter(reader->second, Tokens->fallback))
              .value_or(texcoord_fallback);
    } else {
      texcoord_fallback =
          ReadVector<2, GfVec2f, GfVec2d>(Parameter(texture, Tokens->st))
              .value_or(texcoord_fallback);
    }
    Lotus::Material& material = translation_.material;
    if (Lotus::HasTextureInputs(material) &&
        (material.texcoords != texcoords ||
            material.texcoord_fallback != texcoord_fallback)) {
      return false;
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
    const TfToken color_space = ReadColorSpace(texture);
    std::string file;
    if (const VtValue* asset = Parameter(texture, Tokens->file)) {
      if (asset->IsHolding<SdfAssetPath>()) {
        const SdfAssetPath& path = asset->UncheckedGet<SdfAssetPath>();
        file = path.GetResolvedPath().empty() ? path.GetAssetPath()
                                              : path.GetResolvedPath();
      } else if (asset->IsHolding<std::string>()) {
        file = asset->UncheckedGet<std::string>();
      }
    }
    // Without a file, the key names no image, so the lookup returns its
    // fallback.
    result.texture = HdLotusTextureKey(file, color_space);
    if (!file.empty()) {
      const HdLotusTextureRequest request{result.texture, file, color_space};
      if (std::find(translation_.textures.begin(), translation_.textures.end(),
              request) == translation_.textures.end()) {
        translation_.textures.push_back(request);
      }
    }
    material.texcoords = texcoords;
    material.texcoord_fallback = texcoord_fallback;
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
  if (node->second.nodeTypeId != Tokens->UsdPreviewSurface) {
    translation.unsupported = "the surface shader is " +
                              node->second.nodeTypeId.GetString() +
                              ", not UsdPreviewSurface";
    return translation;
  }

  SurfaceReader surface(converted, node->second, translation);
  Lotus::Material& material = translation.material;
  constexpr double kLargest = 3.4e38;
  surface.Color(Tokens->diffuseColor, material.base_color,
      material.base_color_texture, 0.0, 1.0);
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
  }
  return translation;
}

PXR_NAMESPACE_CLOSE_SCOPE
