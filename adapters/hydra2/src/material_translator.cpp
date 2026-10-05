// SPDX-License-Identifier: Apache-2.0
#include "material_translator.hpp"

#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/staticTokens.h>
#include <pxr/imaging/hd/tokens.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

TF_DEFINE_PRIVATE_TOKENS(Tokens,
    (UsdPreviewSurface)
    (diffuseColor)
    (emissiveColor)
    (metallic)
    (roughness)
    (useSpecularWorkflow));

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

class SurfaceReader {
public:
  SurfaceReader(const HdMaterialNode2& node,
      HdLotusMaterialTranslation& translation)
      : node_(node), translation_(translation) {
  }

  // The input's authored constant, or nothing when it is unauthored, not a
  // constant of a type this reads, or driven by a connection.
  const VtValue* Constant(const TfToken& input) {
    const auto connected = node_.inputConnections.find(input);
    if (connected != node_.inputConnections.end() &&
        !connected->second.empty()) {
      translation_.connected_inputs.push_back(input);
      return nullptr;
    }
    const auto parameter = node_.parameters.find(input);
    return parameter == node_.parameters.end() ? nullptr : &parameter->second;
  }

  // Clamped into [low, high]; a non-finite value keeps `value`.
  void Scalar(const TfToken& input, float& value, double low, double high) {
    if (const VtValue* constant = Constant(input)) {
      if (const auto read = ReadScalar(*constant); read && std::isfinite(*read)) {
        value = static_cast<float>(std::clamp(*read, low, high));
      }
    }
  }

  // Each component clamped into [low, high]; a non-finite component keeps
  // the whole of `value`.
  void Color(const TfToken& input, std::array<float, 3>& value, double low,
      double high) {
    if (const VtValue* constant = Constant(input)) {
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
  const HdMaterialNode2& node_;
  HdLotusMaterialTranslation& translation_;
};

} // namespace

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

  SurfaceReader surface(node->second, translation);
  Lotus::Material& material = translation.material;
  constexpr double kLargest = 3.4e38;
  surface.Color(Tokens->diffuseColor, material.base_color, 0.0, 1.0);
  surface.Color(Tokens->emissiveColor, material.emission, 0.0, kLargest);
  surface.Scalar(Tokens->roughness, material.roughness, 0.0, 1.0);
  float specular_workflow = 0.0F;
  surface.Scalar(Tokens->useSpecularWorkflow, specular_workflow, 0.0, 1.0);
  // The specular workflow ignores metallic.
  if (specular_workflow == 0.0F) {
    surface.Scalar(Tokens->metallic, material.metallic, 0.0, 1.0);
  }
  return translation;
}

PXR_NAMESPACE_CLOSE_SCOPE
