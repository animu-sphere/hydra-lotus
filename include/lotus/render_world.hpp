// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>

namespace Lotus {

// A 4x4 matrix in column-major order that multiplies column vectors: element
// (row, column) is at [column * 4 + row].
using Matrix4 = std::array<float, 16>;

[[nodiscard]] constexpr Matrix4 IdentityMatrix() {
  return {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
      0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
}

// The camera as a host supplies it. `view` maps world space to a right-handed
// view space looking down -Z; `projection` maps view space to OpenGL clip
// space (y up, z in [-w, w]). A backend converts to its own clip convention.
struct Camera {
  Matrix4 view = IdentityMatrix();
  Matrix4 projection = IdentityMatrix();

  bool operator==(const Camera&) const = default;
};

struct FrameSnapshot {
  std::uint64_t revision = 0;
  std::uint32_t triangle_count = 0;
  Camera camera;
};

class RenderWorld {
public:
  void SetTriangleCount(std::uint32_t triangle_count);
  void SetCamera(const Camera& camera);
  void MarkChanged();
  void SetBootstrapTriangle();
  [[nodiscard]] FrameSnapshot Commit();

private:
  std::uint64_t revision_ = 0;
  std::uint32_t triangle_count_ = 0;
  Camera camera_;
  bool dirty_ = false;
};

} // namespace Lotus
