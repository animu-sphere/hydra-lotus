// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lotus/render_world.hpp>

#include <cstdint>
#include <stdexcept>

namespace Lotus::viewport {

// Match the headless bootstrap view: 45 degree vertical field of view,
// looking down -Z from three units away, with clipping range [1, 10].
// Framebuffer pixels determine the aspect, including on high-DPI displays.
[[nodiscard]] inline Camera BootstrapCamera(
    std::uint32_t width, std::uint32_t height) {
  if (width == 0 || height == 0) {
    throw std::invalid_argument("camera extent must be non-zero");
  }
  constexpr float kFocal = 2.41421356F;
  constexpr float kNear = 1.0F;
  constexpr float kFar = 10.0F;
  const float aspect = static_cast<float>(width) / static_cast<float>(height);
  Camera camera;
  camera.view[14] = -3.0F;
  camera.projection = {kFocal / aspect, 0.0F, 0.0F, 0.0F,
      0.0F, kFocal, 0.0F, 0.0F,
      0.0F, 0.0F, (kFar + kNear) / (kNear - kFar), -1.0F,
      0.0F, 0.0F, 2.0F * kFar * kNear / (kNear - kFar), 0.0F};
  return camera;
}

} // namespace Lotus::viewport
