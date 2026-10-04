// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include <lotus/render_world.hpp>

namespace Lotus {

struct DrawSummary {
  // Bootstrap raster pass only: visible scene geometry selects a single
  // scaffold triangle. GPU scene upload will consume FrameSnapshot::scene.
  std::uint64_t source_revision = 0;
  std::uint32_t draw_count = 0;
  std::uint32_t triangle_count = 0;
  // camera.projection * camera.view: world space to OpenGL clip space.
  Matrix4 world_to_clip = IdentityMatrix();
};

[[nodiscard]] DrawSummary ExtractDrawSummary(const FrameSnapshot& snapshot);

} // namespace Lotus
