// SPDX-License-Identifier: Apache-2.0
#include "adapter.hpp"

#include <pxr/pxr.h>
#include <pxr/imaging/hd/tokens.h>

#include <cstdint>
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

} // namespace

int main() {
  HdLotusRenderDelegate delegate;
  const auto color_descriptor = delegate.GetDefaultAovDescriptor(HdAovTokens->color);
  const auto depth_descriptor = delegate.GetDefaultAovDescriptor(HdAovTokens->depth);
  const auto id_descriptor = delegate.GetDefaultAovDescriptor(HdAovTokens->primId);
  if (!Check(color_descriptor.format == HdFormatUNorm8Vec4 &&
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
  if (!Check(!color.Allocate(GfVec3i(2, 2, 1), HdFormatFloat32Vec4, false),
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
  const std::vector<std::uint8_t> source{
      255, 0, 0, 255, 0, 255, 0, 255,
      0, 0, 255, 255, 255, 255, 255, 255};
  if (!Check(!color.WriteColor(source, 1, 4),
          "a color source of another size was accepted") ||
      !Check(color.WriteColor(source, 2, 2), "color write failed")) {
    return 1;
  }
  color.SetConverged(true);
  const auto* pixels = static_cast<const std::uint8_t*>(color.Map());
  if (!Check(!color.WriteColor(source, 2, 2), "mapped color was overwritten") ||
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
