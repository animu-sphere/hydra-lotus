// SPDX-License-Identifier: Apache-2.0
#include "../adapters/viewport/camera.hpp"

#include <lotus/extraction.hpp>

#include <cmath>
#include <iostream>

namespace {

bool CheckProjection(const Lotus::Matrix4& matrix, float width, float height) {
  // Project (0.5, 0.5, 0, 1). Equal world-space offsets must cover equal
  // pixel distances on square, landscape and portrait framebuffers.
  const float w = matrix[3] * 0.5F + matrix[7] * 0.5F + matrix[15];
  const float x = (matrix[0] * 0.5F + matrix[4] * 0.5F + matrix[12]) / w;
  const float y = (matrix[1] * 0.5F + matrix[5] * 0.5F + matrix[13]) / w;
  const float z = (matrix[2] * 0.5F + matrix[6] * 0.5F + matrix[14]) / w;
  return std::abs(w - 3.0F) < 1e-5F && x > 0.0F && y > 0.0F &&
      std::abs(x * width - y * height) < 1e-3F &&
      std::abs(z - 13.0F / 27.0F) < 1e-5F;
}

} // namespace

int main() {
  Lotus::RenderWorld world;
  world.SetBootstrapTriangle();
  std::uint64_t revision = 0;
  for (const auto extent : {std::array<std::uint32_t, 2>{720, 720},
           std::array<std::uint32_t, 2>{1280, 720},
           std::array<std::uint32_t, 2>{720, 1280}}) {
    const auto camera = Lotus::viewport::BootstrapCamera(extent[0], extent[1]);
    world.SetCamera(camera);
    const auto snapshot = world.Commit();
    const auto draw = Lotus::ExtractDrawSummary(snapshot);
    if (snapshot.revision != ++revision || draw.draw_count != 1 ||
        !CheckProjection(draw.world_to_clip,
            static_cast<float>(extent[0]), static_cast<float>(extent[1]))) {
      std::cerr << "viewport camera projection or resize revision failed\n";
      return 1;
    }
    world.SetCamera(camera);
    if (world.Commit().revision != revision) {
      std::cerr << "unchanged viewport camera invalidated the scene\n";
      return 1;
    }
  }
  for (const auto extent : {std::array<std::uint32_t, 2>{0, 720},
           std::array<std::uint32_t, 2>{720, 0}}) {
    try {
      (void)Lotus::viewport::BootstrapCamera(extent[0], extent[1]);
      return 1;
    } catch (const std::invalid_argument&) {
    }
  }
  return 0;
}
