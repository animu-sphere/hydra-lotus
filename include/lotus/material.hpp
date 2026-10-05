// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>

namespace Lotus {

// The Lotus material IR (design policy section 18): what a host's material
// translator produces and the GPU material table evaluates, independent of
// any authoring schema. Renderer Phase 1.5 starts it with constant values;
// colours are linear RGB, and the defaults are UsdPreviewSurface's.
struct Material {
  // Lambert albedo, and the GGX reflectance at normal incidence (F0) where
  // the surface is metallic. Each component in [0, 1].
  std::array<float, 3> base_color{0.18F, 0.18F, 0.18F};
  // GGX roughness in [0, 1]; the microfacet alpha is its square.
  float roughness = 0.5F;
  // The GGX metal's share of the reflectance in [0, 1]; the rest is Lambert.
  float metallic = 0.0F;
  // Emitted radiance, non-negative, from both sides of the surface.
  std::array<float, 3> emission{0.0F, 0.0F, 0.0F};

  bool operator==(const Material&) const = default;
};

} // namespace Lotus
