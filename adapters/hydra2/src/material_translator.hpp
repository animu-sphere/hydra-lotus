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

struct HdLotusMaterialTranslation {
  Lotus::Material material;
  // Why the surface terminal is not a UsdPreviewSurface, which leaves the
  // default material; empty when it is one.
  std::string unsupported;
  // The UsdPreviewSurface inputs the translation reads that a shader graph
  // drives. They take their UsdPreviewSurface default.
  std::vector<TfToken> connected_inputs;
};

// Reads the constant diffuseColor, emissiveColor, metallic and roughness of
// the network's UsdPreviewSurface surface. Values outside the IR's ranges are
// clamped into them, and a non-finite value takes its default. Under
// useSpecularWorkflow, metallic is 0. Other inputs are not read yet.
[[nodiscard]] HdLotusMaterialTranslation HdLotusTranslateMaterial(
    const HdMaterialNetworkMap& network);

PXR_NAMESPACE_CLOSE_SCOPE
