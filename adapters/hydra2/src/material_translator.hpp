// SPDX-License-Identifier: Apache-2.0
// The Hydra material translator (design policy section 18): a Hydra
// material network to the Lotus material IR.
#pragma once

#include <pxr/pxr.h>

#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/material.h>

#include <string>
#include <vector>

#include <lotus/material.hpp>

PXR_NAMESPACE_OPEN_SCOPE

// An image that a translated texture lookup names: its key in the IR, and
// what to decode for it.
struct HdLotusTextureRequest {
  std::string key;
  // The resolved asset path, or the authored one when it does not resolve.
  std::string file;
  // UsdUVTexture's sourceColorSpace: raw, sRGB or auto.
  TfToken source_color_space;

  bool operator==(const HdLotusTextureRequest&) const = default;
};

struct HdLotusMaterialTranslation {
  Lotus::Material material;
  // Why the surface terminal is not a UsdPreviewSurface, which leaves the
  // default material; empty when it is one.
  std::string unsupported;
  // The UsdPreviewSurface inputs the translation reads that a shader graph
  // drives which it does not translate. They take their UsdPreviewSurface
  // default.
  std::vector<TfToken> connected_inputs;
  // The images the material's texture inputs name, once each, in input
  // order.
  std::vector<HdLotusTextureRequest> textures;
};

// The texture key of an image file decoded under a sourceColorSpace.
[[nodiscard]] std::string HdLotusTextureKey(const std::string& file,
    const TfToken& source_color_space);

// Reads the diffuseColor, emissiveColor, metallic and roughness of the
// network's UsdPreviewSurface surface: an authored constant, or a texture
// lookup where a UsdUVTexture drives the input, its texture coordinates a
// UsdPrimvarReader_float2 or its own st. Constants outside the IR's ranges
// are clamped into them, and a non-finite value takes its default. Under
// useSpecularWorkflow, metallic is 0. Other inputs are not read yet.
[[nodiscard]] HdLotusMaterialTranslation HdLotusTranslateMaterial(
    const HdMaterialNetworkMap& network);

PXR_NAMESPACE_CLOSE_SCOPE
