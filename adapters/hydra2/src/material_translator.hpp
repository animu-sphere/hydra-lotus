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
  // Why the surface terminal is neither a UsdPreviewSurface nor a MaterialX
  // Standard Surface, which leaves the default material; empty when it is
  // one.
  std::string unsupported;
  // The surface inputs the translation reads that a shader graph drives
  // which it does not translate. They take their shader's default.
  std::vector<TfToken> connected_inputs;
  // The Standard Surface inputs whose authored values the IR cannot
  // represent, so that the translation ignores them: lobes it lacks and
  // specular weights other than 0 and 1, for example.
  std::vector<TfToken> unrepresented_inputs;
  // The images the material's texture inputs name, once each, in input
  // order.
  std::vector<HdLotusTextureRequest> textures;
};

// The texture key of an image file decoded under a sourceColorSpace.
[[nodiscard]] std::string HdLotusTextureKey(const std::string& file,
    const TfToken& source_color_space);

// The render contexts whose material networks the translator reads, in
// order of preference: a material's MaterialX (mtlx) surface wins over its
// universal one, as in Storm.
[[nodiscard]] const TfTokenVector& HdLotusMaterialRenderContexts();

// Translates the network's surface shader into the material IR.
//
// A UsdPreviewSurface (or MaterialX's ND_UsdPreviewSurface_surfaceshader)
// supplies diffuseColor, emissiveColor, metallic, roughness, ior, normal,
// opacity, opacityThreshold and, under useSpecularWorkflow, specularColor
// (metallic is then 0): an authored constant, or a texture lookup where a
// UsdUVTexture drives the input, its texture coordinates a
// UsdPrimvarReader_float2 or its own st.
//
// A MaterialX ND_standard_surface_surfaceshader supplies base x base_color,
// metalness, specular_roughness, specular_IOR, emission x emission_color,
// opacity and a tangent-space normal map; specular 0 removes the dielectric
// interface (ior 1). Its inputs read constants and graphs of ND_image,
// ND_tiledimage (untiled), ND_texcoord (index 0, the st primvar),
// ND_geompropvalue_vector2, ND_extract, ND_convert and ND_normalmap nodes;
// an image without a file is its default constant. Its other lobes and
// specular tints are reported in unrepresented_inputs.
//
// Constants outside the IR's ranges are clamped into them, and a non-finite
// value takes its default.
[[nodiscard]] HdLotusMaterialTranslation HdLotusTranslateMaterial(
    const HdMaterialNetworkMap& network);

PXR_NAMESPACE_CLOSE_SCOPE
