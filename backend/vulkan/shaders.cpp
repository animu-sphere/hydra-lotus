// SPDX-License-Identifier: Apache-2.0
#include <lotus/vulkan_backend.hpp>

namespace Lotus {

// The names lotus_add_slang_shader gives backend/vulkan/shaders' outputs.
RendererShaders ShadersIn(const std::filesystem::path& directory) {
  const auto file = [&](const char* name) { return (directory / name).string(); };
  return {file("triangle.vert.spv"), file("triangle.frag.spv"),
      {file("path_trace.vert.spv"), file("path_trace.frag.spv"),
          file("wavefront.comp.spv"), file("present_view.comp.spv")}};
}

// Mirrors RejectionColor in shaders/present/present_view.slang.
std::array<float, 3> DebugViewRejectionColor(TemporalRejection rejection) {
  static constexpr std::array<std::array<float, 3>, 11> kColors{{
      {0.10F, 0.75F, 0.15F}, // None: accepted
      {0.0F, 0.0F, 0.0F}, // Miss
      {1.0F, 0.0F, 1.0F}, // InvalidData
      {0.95F, 0.55F, 0.10F}, // InstanceChanged
      {0.95F, 0.85F, 0.10F}, // MaterialChanged
      {0.15F, 0.45F, 0.95F}, // LargeMotion
      {0.90F, 0.15F, 0.15F}, // DepthDiscontinuity
      {0.60F, 0.25F, 0.85F}, // NormalDiscontinuity
      {0.45F, 0.45F, 0.45F}, // HistoryUnavailable
      {0.10F, 0.80F, 0.85F}, // CorrespondenceMissing
      {0.85F, 0.85F, 0.85F}, // ReprojectionOutside
  }};
  const auto index = static_cast<std::size_t>(rejection);
  return index < kColors.size() ? kColors[index]
                                : std::array<float, 3>{1.0F, 0.0F, 1.0F};
}

} // namespace Lotus
