// SPDX-License-Identifier: Apache-2.0
#include <lotus/extraction.hpp>

namespace Lotus {

namespace {

Matrix4 Multiply(const Matrix4& left, const Matrix4& right) {
  Matrix4 result{};
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      float sum = 0.0F;
      for (int index = 0; index < 4; ++index) {
        sum += left[index * 4 + row] * right[column * 4 + index];
      }
      result[column * 4 + row] = sum;
    }
  }
  return result;
}

} // namespace

DrawSummary ExtractDrawSummary(const FrameSnapshot& snapshot) {
  return DrawSummary{
      snapshot.revision,
      snapshot.triangle_count == 0 ? 0U : 1U,
      snapshot.triangle_count == 0 ? 0U : 1U,
      Multiply(snapshot.camera.projection, snapshot.camera.view),
  };
}

} // namespace Lotus
