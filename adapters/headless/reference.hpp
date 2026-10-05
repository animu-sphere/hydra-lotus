// SPDX-License-Identifier: Apache-2.0
// The reference scene, its images, and the statistical comparison that
// decides whether a render matches the reference (design policy section 53,
// DES-Q5).
#pragma once

#include <lotus/render_world.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace LotusHeadless {

// The reference images' size, in pixels.
inline constexpr std::uint32_t kReferenceSize = 128;

// Linear RGB, three floats per pixel, rows from the top.
struct RgbImage {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<float> values;
};

// Portable float maps: "PF", little-endian, rows from the bottom. Each
// returns an empty string on success, or why it failed.
[[nodiscard]] std::string WritePfm(
    const std::filesystem::path& path, const RgbImage& image);
[[nodiscard]] std::string ReadPfm(
    const std::filesystem::path& path, RgbImage& image);

// Replaces the world's camera and scene with the reference scene: a Cornell
// box lit only by an emissive panel under its ceiling, with red and green
// side walls, a GGX metal block and a Lambert block. Every camera ray enters
// the box, so every sample hits.
void SetCornellBox(Lotus::RenderWorld& world);

// The same scene with the red wall's albedo scaled by `scale`: the
// comparison's negative control.
void SetCornellRedWall(Lotus::RenderWorld& world, float scale);

// The reference: the mean of `samples` per pixel and each pixel's per-sample
// variance, estimated from independent batches.
struct Reference {
  RgbImage mean;
  RgbImage variance;
  std::uint32_t samples = 0;
};

// One image's comparison with the reference. z values are differences in
// standard errors; tiles are square, of `tile` pixels a side.
struct Comparison {
  std::uint32_t samples = 0;
  std::uint32_t tile = 0;
  std::array<double, 3> image_z{};
  double max_tile_z = 0;
  double mean_tile_z2 = 0;
  // Empty when the image statistically matches the reference.
  std::string failure;
};

// The DES-Q5 metric. An image of `samples` per pixel matches the reference
// when, in each channel, the difference between its mean and the
// reference's is within kMatchZ standard errors over the whole image and
// over every tile. A tile is 8x8 pixels, doubled until it holds at least
// 1024 samples. A pixel's difference has variance s2 (1/N + 1/M), where s2
// is the reference's per-sample variance and N and M are the sample counts:
// exact for the reference estimator, conservative for one with less
// variance.
inline constexpr double kMatchZ = 5.0;
[[nodiscard]] Comparison Compare(
    const RgbImage& image, std::uint32_t samples, const Reference& reference);

} // namespace LotusHeadless
