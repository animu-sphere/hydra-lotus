// SPDX-License-Identifier: Apache-2.0
#include "adapter.hpp"

#include <pxr/pxr.h>
#include <pxr/imaging/hd/tokens.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

bool Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

Lotus::ColorProduct Product(std::uint32_t width, std::uint32_t height,
    const char* format, std::vector<std::uint8_t> payload) {
  Lotus::ColorProduct product;
  product.width = width;
  product.height = height;
  product.pixel_format = format;
  product.row_pitch = width * (product.pixel_format == "rgba8-unorm" ? 4U : 16U);
  product.payload = std::move(payload);
  return product;
}

std::vector<std::uint8_t> FloatBytes(const std::vector<float>& values) {
  std::vector<std::uint8_t> bytes(values.size() * sizeof(float));
  std::memcpy(bytes.data(), values.data(), bytes.size());
  return bytes;
}

} // namespace

int main() {
  HdLotusRenderDelegate delegate;
  const auto color_descriptor = delegate.GetDefaultAovDescriptor(HdAovTokens->color);
  const auto depth_descriptor = delegate.GetDefaultAovDescriptor(HdAovTokens->depth);
  const auto id_descriptor = delegate.GetDefaultAovDescriptor(HdAovTokens->primId);
  if (!Check(color_descriptor.format == HdFormatFloat32Vec4 &&
                 !color_descriptor.multiSampled &&
                 color_descriptor.clearValue == VtValue(GfVec4f(0.0F)),
          "color descriptor mismatch") ||
      !Check(depth_descriptor.format == HdFormatFloat32 &&
                 !depth_descriptor.multiSampled &&
                 depth_descriptor.clearValue == VtValue(1.0F),
          "depth descriptor mismatch") ||
      !Check(id_descriptor.format == HdFormatInt32 &&
                 !id_descriptor.multiSampled &&
                 id_descriptor.clearValue == VtValue(-1),
          "id descriptor mismatch") ||
      !Check(delegate.GetDefaultAovDescriptor(TfToken("normal")).format ==
                 HdFormatInvalid,
          "unimplemented AOV was advertised")) {
    return 1;
  }
  HdLotusRenderBuffer color(SdfPath("/color"));
  if (!Check(!color.Allocate(GfVec3i(2, 2, 1), HdFormatFloat16Vec4, false),
          "unsupported color format was accepted") ||
      !Check(!color.Allocate(GfVec3i(2, 2, 2), HdFormatUNorm8Vec4, false),
          "volume AOV was accepted") ||
      !Check(!color.Allocate(GfVec3i(2, 2, 1), HdFormatUNorm8Vec4, true),
          "multisampled AOV was accepted")) {
    return 1;
  }
  if (!Check(color.Allocate(GfVec3i(2, 2, 1), HdFormatUNorm8Vec4, false),
          "color allocation failed")) {
    return 1;
  }
  // Top-down source rows: red, green over blue, white.
  const auto source = Product(2, 2, "rgba8-unorm",
      {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255});
  auto unknown = source;
  unknown.pixel_format = "rgba16-sfloat";
  if (!Check(!color.WriteColor(Product(1, 4, "rgba8-unorm", source.payload)),
          "a color source of another size was accepted") ||
      !Check(!color.WriteColor(unknown), "an unknown color format was accepted") ||
      !Check(color.WriteColor(source), "color write failed")) {
    return 1;
  }
  color.SetConverged(true);
  const auto* pixels = static_cast<const std::uint8_t*>(color.Map());
  if (!Check(!color.WriteColor(source), "mapped color was overwritten") ||
      !Check(!color.Allocate(GfVec3i(1, 1, 1), HdFormatUNorm8Vec4, false),
          "mapped color was reallocated")) {
    return 1;
  }
  // Hydra's rows are bottom-up: blue, white first, then red, green.
  if (!Check(pixels != nullptr && pixels[0] == 0 && pixels[2] == 255 &&
                 pixels[4] == 255 && pixels[8] == 255 && pixels[9] == 0 &&
                 pixels[13] == 255 && pixels[15] == 255,
          "color rows were not flipped") ||
      !Check(color.IsMapped(), "color map state is incorrect") ||
      !Check(color.IsConverged(), "color did not converge")) {
    return 1;
  }
  color.Unmap();

  // HDR radiance keeps its values in a Float32Vec4 buffer and is clamped
  // and rounded into a UNorm8Vec4 one; 8-bit colour widens to floats.
  const auto radiance = Product(1, 2, "rgba32-sfloat",
      FloatBytes({2.5F, 0.5F, -1.0F, 1.0F, 0.25F, 0.0F, 1.0F, 0.0F}));
  HdLotusRenderBuffer hdr(SdfPath("/hdr"));
  if (!Check(hdr.Allocate(GfVec3i(1, 2, 1), HdFormatFloat32Vec4, false) &&
                 hdr.WriteColor(radiance),
          "float color write failed")) {
    return 1;
  }
  const auto* floats = static_cast<const float*>(hdr.Map());
  const bool kept = floats != nullptr && floats[0] == 0.25F && floats[3] == 0.0F &&
                    floats[4] == 2.5F && floats[6] == -1.0F && floats[7] == 1.0F;
  hdr.Unmap();
  if (!Check(kept, "float color was not kept and flipped") ||
      !Check(hdr.WriteColor(Product(1, 2, "rgba8-unorm",
                 {255, 51, 0, 255, 0, 0, 0, 0})),
          "8-bit color did not widen")) {
    return 1;
  }
  floats = static_cast<const float*>(hdr.Map());
  const bool widened = floats != nullptr && floats[4] == 1.0F && floats[5] == 0.2F &&
                       floats[7] == 1.0F && floats[0] == 0.0F;
  hdr.Unmap();
  if (!Check(widened, "8-bit color widened incorrectly") ||
      !Check(color.Allocate(GfVec3i(1, 2, 1), HdFormatUNorm8Vec4, false) &&
                 color.WriteColor(radiance),
          "float color did not narrow")) {
    return 1;
  }
  pixels = static_cast<const std::uint8_t*>(color.Map());
  const bool narrowed = pixels != nullptr && pixels[0] == 64 && pixels[2] == 255 &&
                        pixels[3] == 0 && pixels[4] == 255 && pixels[5] == 128 &&
                        pixels[6] == 0 && pixels[7] == 255;
  color.Unmap();
  if (!Check(narrowed, "float color was not clamped into 8 bits")) {
    return 1;
  }

  HdLotusRenderBuffer depth(SdfPath("/depth"));
  if (!Check(depth.Allocate(GfVec3i(1, 2, 1), HdFormatFloat32, false),
          "depth allocation failed") ||
      !Check(!depth.WriteDepth({0.25F}, 1, 1),
          "a depth source of another size was accepted") ||
      !Check(depth.WriteDepth({0.25F, 0.75F}, 1, 2), "depth write failed")) {
    return 1;
  }
  const auto* depths = static_cast<const float*>(depth.Map());
  if (!Check(depths != nullptr && depths[0] == 0.75F && depths[1] == 0.25F,
          "depth rows were not flipped")) {
    return 1;
  }
  depth.Unmap();

  HdLotusRenderBuffer ids(SdfPath("/primId"));
  if (!Check(ids.Allocate(GfVec3i(2, 1, 1), HdFormatInt32, false),
          "id allocation failed") ||
      !Check(ids.WriteIds(-1), "id write failed")) {
    return 1;
  }
  const auto* id_values = static_cast<const std::int32_t*>(ids.Map());
  if (!Check(id_values != nullptr && id_values[0] == -1 && id_values[1] == -1,
          "id payload is incorrect")) {
    return 1;
  }
  ids.Unmap();
  return 0;
}
