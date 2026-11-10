// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
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
  // Where this lookup reads, from the material's texture coordinates (s, t):
  // (m[0] s + m[1] t + m[2], m[3] s + m[4] t + m[5]). A normal map's tangent
  // frame follows the transformed coordinates.
  std::array<float, 6> st_transform{1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};

  bool operator==(const TextureInput&) const = default;
};

// The shading model a material's parameters belong to
// (docs/design/MATERIAL_IR_V2.md). Same-named parameters of different models
// stay distinct, and nothing converts one model's parameters into another's.
enum class ShadingModel : std::uint32_t {
  UsdPreviewSurface,
  // Tags reserved for LOTUS-MATERIALX-05, -07 and -08. A material with one
  // of them has no parameters yet, and the GPU scene rejects it.
  GltfPbr,
  StandardSurface,
  OpenPbr,
};

// What a material decides before or outside its BSDF, and means the same in
// every shading model: where its lookups read, what hides the surface and
// its base shading normal.
struct MaterialGeometry {
  // Where the texture inputs look up: the mesh's texture-coordinate set of
  // this name (MeshGeometry::texcoords), or `texcoord_fallback` on a mesh
  // without one. Empty names no set.
  std::string texcoords;
  std::array<float, 2> texcoord_fallback{0.0F, 0.0F};
  // Surface coverage in [0, 1]. With a positive threshold, coverage is
  // binary: opacity >= threshold accepts the surface. Otherwise a ray
  // accepts it with probability opacity (straight-through on rejection,
  // without a scattering event). This is presence, not refractive glass.
  float opacity = 1.0F;
  float opacity_threshold = 0.0F;
  std::optional<TextureInput> opacity_texture;
  // Tangent-space shading normal in [-1, 1], normalized after evaluation.
  // The identity leaves the mesh's shading normal unchanged. The tangent
  // follows increasing s and the bitangent increasing t of `texcoords`;
  // without usable coordinates a stable orthonormal frame is used.
  std::array<float, 3> normal{0.0F, 0.0F, 1.0F};
  // RGB already in tangent space: scale/bias belong to the texture input,
  // so an 8-bit normal map typically uses scale 2, bias -1 and raw texels.
  std::optional<TextureInput> normal_texture;
  // The model's colour input (UsdPreviewSurface's base colour) on a mesh
  // with the colour set of this name (MeshGeometry::colors), clamped into
  // [0, 1]; the input's own value applies on a mesh without one. Empty names
  // no set. A texture input on that input wins.
  std::string base_color_primvar;

  bool operator==(const MaterialGeometry&) const = default;
};

// UsdPreviewSurface's parameters, with its defaults. Colours are linear RGB.
// Each input is a constant or, when it has a texture input, a texture
// lookup that replaces the constant.
struct PreviewSurface {
  // Lambert albedo, and the GGX reflectance at normal incidence (F0) where
  // the surface is metallic. Each component in [0, 1].
  std::array<float, 3> base_color{0.18F, 0.18F, 0.18F};
  // GGX roughness in [0, 1]; the microfacet alpha is its square.
  float roughness = 0.5F;
  // The metal's share in [0, 1]; the rest is a dielectric over Lambert.
  float metallic = 0.0F;
  // Dielectric index relative to air, finite and >= 1. At 1 the dielectric
  // interface disappears, leaving uncoated Lambert. Constant-only today.
  float ior = 1.5F;
  // In this workflow metallic and ior are ignored: specular_color supplies
  // GGX F0 directly, with a white grazing limit, over the diffuse base.
  bool use_specular_workflow = false;
  std::array<float, 3> specular_color{0.0F, 0.0F, 0.0F};
  // Emitted radiance, non-negative, from both sides of the surface.
  std::array<float, 3> emission{0.0F, 0.0F, 0.0F};
  // The share in [0, 1] of the non-metal part that is a dielectric
  // interface transmitting light, with `roughness` and `ior`, instead of the
  // dielectric over Lambert (docs/design/TRANSMISSION.md). Positive values
  // require the metallic workflow. Constant-only.
  float transmission = 0.0F;
  // The tint of the light the interface transmits, applied at every
  // crossing; each component in [0, 1]. Constant-only.
  std::array<float, 3> transmission_color{1.0F, 1.0F, 1.0F};
  // The transmitting surface is a thin wall enclosing no medium. Otherwise
  // it bounds a closed solid whose triangles wind outwards.
  bool thin_walled = false;

  std::optional<TextureInput> base_color_texture;
  std::optional<TextureInput> roughness_texture;
  std::optional<TextureInput> metallic_texture;
  std::optional<TextureInput> emission_texture;
  std::optional<TextureInput> specular_color_texture;

  bool operator==(const PreviewSurface&) const = default;
};

// Reserved: their parameters arrive with LOTUS-MATERIALX-05, -07 and -08.
struct GltfPbr {
  bool operator==(const GltfPbr&) const = default;
};
struct StandardSurface {
  bool operator==(const StandardSurface&) const = default;
};
struct OpenPbr {
  bool operator==(const OpenPbr&) const = default;
};

// The Lotus material IR (design policy section 18,
// docs/design/MATERIAL_IR_V2.md): what a host's material translator produces
// and the GPU material table evaluates, independent of any authoring schema.
// The variant's alternatives are in ShadingModel order, so the tag and the
// parameters cannot disagree. A default material is a default
// UsdPreviewSurface.
struct Material {
  MaterialGeometry geometry;
  std::variant<PreviewSurface, GltfPbr, StandardSurface, OpenPbr> surface;

  [[nodiscard]] ShadingModel Model() const {
    return static_cast<ShadingModel>(surface.index());
  }

  bool operator==(const Material&) const = default;
};

// The UsdPreviewSurface parameters of a material that has that model.
[[nodiscard]] inline PreviewSurface& Preview(Material& material) {
  return std::get<PreviewSurface>(material.surface);
}
[[nodiscard]] inline const PreviewSurface& Preview(const Material& material) {
  return std::get<PreviewSurface>(material.surface);
}

// A material's texture-input slots, the numbering of the GPU material
// table's binding mask. Slots 0 and 1 are the geometry's in every model, so
// the any-hit coverage test never asks for the model; the model's own
// inputs follow. A slot the model does not have reads no lookup.
inline constexpr std::size_t kMaterialTextureInputs = 7;
inline constexpr std::size_t kOpacitySlot = 0;
inline constexpr std::size_t kNormalSlot = 1;
// UsdPreviewSurface's slots.
inline constexpr std::size_t kPreviewBaseColorSlot = 2;
inline constexpr std::size_t kPreviewRoughnessSlot = 3;
inline constexpr std::size_t kPreviewMetallicSlot = 4;
inline constexpr std::size_t kPreviewEmissionSlot = 5;
inline constexpr std::size_t kPreviewSpecularColorSlot = 6;

// A material's texture inputs in slot order. A slot the model does not have
// is an empty input.
[[nodiscard]] inline std::array<const std::optional<TextureInput>*,
    kMaterialTextureInputs>
TextureInputs(const Material& material) {
  static const std::optional<TextureInput> kNone;
  std::array<const std::optional<TextureInput>*, kMaterialTextureInputs>
      inputs{&material.geometry.opacity_texture,
          &material.geometry.normal_texture, &kNone, &kNone, &kNone, &kNone,
          &kNone};
  if (const auto* preview = std::get_if<PreviewSurface>(&material.surface)) {
    inputs[kPreviewBaseColorSlot] = &preview->base_color_texture;
    inputs[kPreviewRoughnessSlot] = &preview->roughness_texture;
    inputs[kPreviewMetallicSlot] = &preview->metallic_texture;
    inputs[kPreviewEmissionSlot] = &preview->emission_texture;
    inputs[kPreviewSpecularColorSlot] = &preview->specular_color_texture;
  }
  return inputs;
}

// The same, writable, for a UsdPreviewSurface material.
[[nodiscard]] inline std::array<std::optional<TextureInput>*,
    kMaterialTextureInputs>
PreviewTextureInputs(Material& material) {
  PreviewSurface& preview = Preview(material);
  return {&material.geometry.opacity_texture,
      &material.geometry.normal_texture, &preview.base_color_texture,
      &preview.roughness_texture, &preview.metallic_texture,
      &preview.emission_texture, &preview.specular_color_texture};
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
