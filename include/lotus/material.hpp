// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Lotus {

// How a texture's texels are stored. Every format has four channels.
enum class TextureFormat : std::uint32_t {
  // 8-bit unsigned normalized channels, linear.
  Rgba8Unorm,
  // 8-bit channels whose red, green and blue are sRGB-encoded; alpha is
  // linear. Lookups decode them to linear values.
  Rgba8Srgb,
  // 32-bit floats in the host's byte order, linear.
  Rgba32Float,
};

[[nodiscard]] constexpr std::size_t TexelBytes(TextureFormat format) {
  return format == TextureFormat::Rgba32Float ? 16 : 4;
}

// A decoded image that material texture inputs read. Rows run from the top
// of the image: texture coordinates (s, t) = (0, 0) are its bottom-left
// corner and (1, 1) its top-right, as in USD.
struct Texture {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  TextureFormat format = TextureFormat::Rgba8Unorm;
  // width * height texels of TexelBytes(format) bytes each, row by row.
  std::vector<std::uint8_t> texels;

  bool operator==(const Texture&) const = default;
};

// What a lookup reads outside [0, 1] along one texture coordinate.
enum class TextureWrap : std::uint32_t {
  // Transparent black.
  Black,
  // The nearest edge texel.
  Clamp,
  Repeat,
  Mirror,
};

// A texture lookup that drives one material input, as UsdUVTexture does:
// the texture is filtered bilinearly at the material's texture coordinates
// and the result is texel * scale + bias. The input's value is then the
// lookup's red, green and blue for a colour, or its `channel` for a scalar,
// clamped into the input's range.
struct TextureInput {
  // The key of the texture in LotusScene::textures. A key with no texture
  // makes the lookup return `fallback`, unscaled.
  std::string texture;
  // 0 to 3 (red to alpha) for a scalar input; 0 for a colour input.
  std::uint32_t channel = 0;
  TextureWrap wrap_s = TextureWrap::Black;
  TextureWrap wrap_t = TextureWrap::Black;
  std::array<float, 4> scale{1.0F, 1.0F, 1.0F, 1.0F};
  std::array<float, 4> bias{0.0F, 0.0F, 0.0F, 0.0F};
  std::array<float, 4> fallback{0.0F, 0.0F, 0.0F, 1.0F};

  bool operator==(const TextureInput&) const = default;
};

// The Lotus material IR (design policy section 18): what a host's material
// translator produces and the GPU material table evaluates, independent of
// any authoring schema. Colours are linear RGB, and the defaults are
// UsdPreviewSurface's. Each input is a constant or, when it has a texture
// input, a texture lookup that replaces the constant.
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

  std::optional<TextureInput> base_color_texture;
  std::optional<TextureInput> roughness_texture;
  std::optional<TextureInput> metallic_texture;
  std::optional<TextureInput> emission_texture;
  // Where the texture inputs look up: the mesh's texture-coordinate set of
  // this name (MeshGeometry::texcoords), or `texcoord_fallback` on a mesh
  // without one. Empty names no set.
  std::string texcoords;
  std::array<float, 2> texcoord_fallback{0.0F, 0.0F};

  bool operator==(const Material&) const = default;
};

// A material's texture inputs, in the order the GPU material table keeps
// them: base colour, roughness, metallic, emission.
inline constexpr std::size_t kMaterialTextureInputs = 4;

[[nodiscard]] inline std::array<const std::optional<TextureInput>*,
    kMaterialTextureInputs>
TextureInputs(const Material& material) {
  return {&material.base_color_texture, &material.roughness_texture,
      &material.metallic_texture, &material.emission_texture};
}

// Whether any of the material's inputs is a texture lookup.
[[nodiscard]] inline bool HasTextureInputs(const Material& material) {
  for (const auto* input : TextureInputs(material)) {
    if (input->has_value()) {
      return true;
    }
  }
  return false;
}

} // namespace Lotus
