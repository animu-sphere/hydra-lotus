// SPDX-License-Identifier: Apache-2.0
// The Hydra material translator (design policy section 18): a Hydra
// material network to the Lotus material IR.
#pragma once

#include <pxr/pxr.h>

#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/usd/sdf/path.h>

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

// What a translation could not represent exactly
// (docs/design/MATERIALX_TRANSLATION.md section 6).
enum class HdLotusMaterialDiagnosticKind {
  // The terminal's shader has no model: the default material.
  UnsupportedSurface,
  // A displacement or volume terminal is connected: it is not rendered.
  UnsupportedTerminal,
  // A node identifier the node table does not hold.
  UnsupportedNode,
  // Supported nodes in a shape a constant or one lookup cannot hold.
  UnsupportedGraph,
  // A parameter value outside a node's rule.
  UnsupportedValue,
  // The model has no place for the input's value.
  UnrepresentedInput,
  // An authored input the model's table does not name.
  UnknownInput,
  // Translated with a stated difference.
  Substituted,
};

struct HdLotusMaterialDiagnostic {
  HdLotusMaterialDiagnosticKind kind = HdLotusMaterialDiagnosticKind::UnsupportedSurface;
  // The surface input affected; empty for a terminal.
  TfToken input;
  // Where translation stopped, and that node's identifier.
  SdfPath node;
  TfToken node_type;
  // The rule, in one clause.
  std::string detail;

  bool operator==(const HdLotusMaterialDiagnostic&) const = default;
};

// The kind's name as the adapter's warnings spell it.
[[nodiscard]] const char* HdLotusMaterialDiagnosticKindName(
    HdLotusMaterialDiagnosticKind kind);

struct HdLotusMaterialTranslation {
  Lotus::Material material;
  // Everything the translation did not represent exactly, in the order the
  // surface's inputs are read; one per input. Empty when every authored
  // input, connection and terminal was read and is represented exactly.
  std::vector<HdLotusMaterialDiagnostic> diagnostics;
  // The images the material's texture inputs name, once each, in input
  // order.
  std::vector<HdLotusTextureRequest> textures;
};

// One input of a MaterialX node definition as the translator holds it.
struct HdLotusNodeInputDefinition {
  TfToken name;
  // The MaterialX type: float, color3, color4, vector2, vector3, vector4,
  // integer, boolean, string or filename.
  TfToken type;
  // Whether the definition gives the input a value; an input bound to a
  // geometric property by default has none.
  bool has_value = true;
  // A numeric type's default components.
  std::vector<double> value;
  // A string's default.
  std::string text;
};

struct HdLotusNodeDefinition {
  TfToken identifier;
  std::vector<HdLotusNodeInputDefinition> inputs;
};

// Every MaterialX node definition the translator reads, surfaces included,
// with the inputs and defaults it assumes. A test holds them to the
// runtime's definitions.
[[nodiscard]] const std::vector<HdLotusNodeDefinition>&
HdLotusMaterialNodeDefinitions();

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
// interface (ior 1). Each input reads a constant or a MaterialX graph that
// folds exactly into a constant or one lookup with a per-channel scale and
// bias (docs/reference/SCENE.md lists the nodes); anything else leaves the
// input at its definition's default and adds a diagnostic.
//
// Constants outside the IR's ranges are clamped into them.
[[nodiscard]] HdLotusMaterialTranslation HdLotusTranslateMaterial(
    const HdMaterialNetworkMap& network);

PXR_NAMESPACE_CLOSE_SCOPE
