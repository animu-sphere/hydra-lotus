// SPDX-License-Identifier: Apache-2.0
#include <lotus/vulkan_backend.hpp>

namespace Lotus {

// The names lotus_add_slang_shader gives backend/vulkan/shaders' outputs.
RendererShaders ShadersIn(const std::filesystem::path& directory) {
  const auto file = [&](const char* name) { return (directory / name).string(); };
  return {file("triangle.vert.spv"), file("triangle.frag.spv"),
      {file("path_trace.vert.spv"), file("path_trace.frag.spv"),
          file("wavefront.comp.spv")}};
}

} // namespace Lotus
