// SPDX-License-Identifier: Apache-2.0
#include "reference.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

namespace LotusHeadless {
namespace {

std::uint32_t SwapBytes(std::uint32_t value) {
  return (value >> 24) | ((value >> 8) & 0xFF00U) | ((value << 8) & 0xFF0000U) |
         (value << 24);
}

// Converts between little-endian file order and native order, in place.
void ToFromLittleEndian(std::vector<float>& values) {
  if constexpr (std::endian::native == std::endian::big) {
    for (float& value : values)
      value = std::bit_cast<float>(SwapBytes(std::bit_cast<std::uint32_t>(value)));
  }
}

// Four corners in order around the quad.
Lotus::MeshGeometry Quad(std::vector<std::array<float, 3>> corners) {
  Lotus::MeshGeometry quad;
  quad.positions = std::move(corners);
  quad.triangles = {{{0, 1, 2}}, {{0, 2, 3}}};
  quad.source_faces = {0, 0};
  return quad;
}

Lotus::MeshGeometry UnitCube() {
  Lotus::MeshGeometry cube;
  cube.positions = {{{-0.5F, -0.5F, -0.5F}}, {{0.5F, -0.5F, -0.5F}},
      {{0.5F, 0.5F, -0.5F}}, {{-0.5F, 0.5F, -0.5F}}, {{-0.5F, -0.5F, 0.5F}},
      {{0.5F, -0.5F, 0.5F}}, {{0.5F, 0.5F, 0.5F}}, {{-0.5F, 0.5F, 0.5F}}};
  cube.triangles = {{{0, 2, 1}}, {{0, 3, 2}}, {{4, 5, 6}}, {{4, 6, 7}},
      {{0, 1, 5}}, {{0, 5, 4}}, {{3, 7, 6}}, {{3, 6, 2}},
      {{0, 4, 7}}, {{0, 7, 3}}, {{1, 2, 6}}, {{1, 6, 5}}};
  cube.source_faces = {0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5};
  return cube;
}

// Scales the unit cube, turns it about +Y by `degrees` and moves it.
Lotus::MeshInstance Block(const std::array<float, 3>& size, float degrees,
    const std::array<float, 3>& centre) {
  const double radians = degrees * 3.14159265358979323846 / 180.0;
  const auto c = static_cast<float>(std::cos(radians));
  const auto s = static_cast<float>(std::sin(radians));
  Lotus::MeshInstance instance;
  instance.world_from_object = {c * size[0], 0.0F, -s * size[0], 0.0F,
      0.0F, size[1], 0.0F, 0.0F, s * size[2], 0.0F, c * size[2], 0.0F,
      centre[0], centre[1], centre[2], 1.0F};
  return instance;
}

Lotus::Material CoatedDiffuse(const std::array<float, 3>& albedo) {
  Lotus::Material material;
  material.base_color = albedo;
  return material;
}

constexpr std::array<float, 3> kWhite{0.73F, 0.73F, 0.73F};
constexpr std::array<float, 3> kRed{0.63F, 0.065F, 0.05F};
constexpr std::array<float, 3> kGreen{0.14F, 0.45F, 0.091F};

} // namespace

std::string WritePfm(const std::filesystem::path& path, const RgbImage& image) {
  if (image.values.size() != std::size_t{image.width} * image.height * 3)
    return "the image has " + std::to_string(image.values.size()) +
           " values for " + std::to_string(image.width) + "x" +
           std::to_string(image.height) + " pixels";
  std::vector<float> rows;
  rows.reserve(image.values.size());
  const std::size_t row_values = std::size_t{image.width} * 3;
  for (std::uint32_t y = image.height; y-- > 0;) {
    const auto row = image.values.begin() + static_cast<std::ptrdiff_t>(y * row_values);
    rows.insert(rows.end(), row, row + static_cast<std::ptrdiff_t>(row_values));
  }
  ToFromLittleEndian(rows);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << "PF\n" << image.width << ' ' << image.height << "\n-1\n";
  output.write(reinterpret_cast<const char*>(rows.data()),
      static_cast<std::streamsize>(rows.size() * sizeof(float)));
  return output.good() ? "" : "cannot write " + path.string();
}

std::string ReadPfm(const std::filesystem::path& path, RgbImage& image) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return "cannot open " + path.string();
  std::string magic;
  long long width = 0;
  long long height = 0;
  double scale = 0;
  input >> magic >> width >> height >> scale;
  // One whitespace character separates the header from the data.
  input.get();
  if (!input || magic != "PF" || width <= 0 || height <= 0 || width > 65536 ||
      height > 65536 || scale == 0)
    return path.string() + " is not an RGB portable float map";
  if (scale > 0)
    return path.string() + " is big-endian; only little-endian maps are read";
  std::vector<float> rows(static_cast<std::size_t>(width * height * 3));
  input.read(reinterpret_cast<char*>(rows.data()),
      static_cast<std::streamsize>(rows.size() * sizeof(float)));
  if (input.gcount() != static_cast<std::streamsize>(rows.size() * sizeof(float)))
    return path.string() + " is truncated";
  ToFromLittleEndian(rows);
  image.width = static_cast<std::uint32_t>(width);
  image.height = static_cast<std::uint32_t>(height);
  image.values.clear();
  image.values.reserve(rows.size());
  const std::size_t row_values = static_cast<std::size_t>(width) * 3;
  for (std::uint32_t y = image.height; y-- > 0;) {
    const auto row = rows.begin() + static_cast<std::ptrdiff_t>(y * row_values);
    image.values.insert(image.values.end(), row,
        row + static_cast<std::ptrdiff_t>(row_values));
  }
  return {};
}

void SetCornellBox(Lotus::RenderWorld& world) {
  // The bootstrap camera: 3 units in front of the box's centre, looking down
  // -Z with a 45 degree vertical field of view. The open front at z = 1 is
  // 2 units away, where the view is 1.66 units across, so every camera ray
  // enters the box.
  constexpr float kFocal = 2.41421356F; // 1 / tan(22.5 degrees)
  constexpr float kNear = 1.0F;
  constexpr float kFar = 10.0F;
  Lotus::Camera camera;
  camera.view[14] = -3.0F;
  camera.projection = {kFocal, 0.0F, 0.0F, 0.0F, 0.0F, kFocal, 0.0F, 0.0F,
      0.0F, 0.0F, (kFar + kNear) / (kNear - kFar), -1.0F,
      0.0F, 0.0F, 2.0F * kFar * kNear / (kNear - kFar), 0.0F};
  world.SetCamera(camera);
  world.SetEnvironment({0.0F, 0.0F, 0.0F});

  // The box spans [-1, 1] on each axis and is open towards the camera.
  const Lotus::MeshInstance placed;
  world.SetMesh("/cornell/floor",
      Quad({{{-1, -1, 1}}, {{1, -1, 1}}, {{1, -1, -1}}, {{-1, -1, -1}}}), placed);
  world.SetMesh("/cornell/ceiling",
      Quad({{{-1, 1, 1}}, {{-1, 1, -1}}, {{1, 1, -1}}, {{1, 1, 1}}}), placed);
  world.SetMesh("/cornell/back",
      Quad({{{-1, -1, -1}}, {{1, -1, -1}}, {{1, 1, -1}}, {{-1, 1, -1}}}), placed);
  world.SetMesh("/cornell/left",
      Quad({{{-1, -1, 1}}, {{-1, -1, -1}}, {{-1, 1, -1}}, {{-1, 1, 1}}}), placed);
  world.SetMesh("/cornell/right",
      Quad({{{1, -1, -1}}, {{1, -1, 1}}, {{1, 1, 1}}, {{1, 1, -1}}}), placed);
  world.SetMaterial("/materials/white", CoatedDiffuse(kWhite));
  world.SetMaterial("/materials/red", CoatedDiffuse(kRed));
  world.SetMaterial("/materials/green", CoatedDiffuse(kGreen));
  for (const char* wall : {"/cornell/floor", "/cornell/ceiling", "/cornell/back"})
    world.BindMaterial(wall, "/materials/white");
  world.BindMaterial("/cornell/left", "/materials/red");
  world.BindMaterial("/cornell/right", "/materials/green");

  // A black emitter just under the ceiling, a quarter of its width.
  world.SetMesh("/cornell/light",
      Quad({{{-0.35F, 0.98F, 0.35F}}, {{-0.35F, 0.98F, -0.35F}},
          {{0.35F, 0.98F, -0.35F}}, {{0.35F, 0.98F, 0.35F}}}),
      placed);
  Lotus::Material light = CoatedDiffuse({0.0F, 0.0F, 0.0F});
  light.emission = {15.0F, 13.0F, 10.0F};
  world.SetMaterial("/materials/light", light);
  world.BindMaterial("/cornell/light", "/materials/light");

  // The blocks stand a hair above the floor, so no face is coplanar with it.
  world.SetMesh("/cornell/tall", UnitCube(),
      Block({0.6F, 1.2F, 0.6F}, 17.0F, {-0.35F, -0.399F, -0.3F}));
  Lotus::Material metal;
  metal.base_color = {0.95F, 0.85F, 0.6F};
  metal.roughness = 0.35F;
  metal.metallic = 1.0F;
  world.SetMaterial("/materials/metal", metal);
  world.BindMaterial("/cornell/tall", "/materials/metal");
  world.SetMesh("/cornell/short", UnitCube(),
      Block({0.6F, 0.6F, 0.6F}, -18.0F, {0.38F, -0.699F, 0.3F}));
  world.BindMaterial("/cornell/short", "/materials/white");
}

void SetCornellRedWall(Lotus::RenderWorld& world, float scale) {
  world.SetMaterial("/materials/red",
      CoatedDiffuse({kRed[0] * scale, kRed[1] * scale, kRed[2] * scale}));
}

Comparison Compare(
    const RgbImage& image, std::uint32_t samples, const Reference& reference) {
  Comparison result;
  result.samples = samples;
  const std::uint32_t width = reference.mean.width;
  const std::uint32_t height = reference.mean.height;
  const std::size_t count = std::size_t{width} * height * 3;
  if (samples == 0 || reference.samples == 0 || reference.mean.values.size() != count ||
      reference.variance.width != width || reference.variance.height != height ||
      reference.variance.values.size() != count) {
    result.failure = "the reference is incomplete";
    return result;
  }
  if (image.width != width || image.height != height || image.values.size() != count) {
    result.failure = "the image is " + std::to_string(image.width) + "x" +
                     std::to_string(image.height) + " instead of the reference's " +
                     std::to_string(width) + "x" + std::to_string(height);
    return result;
  }
  for (std::size_t i = 0; i < count; ++i) {
    if (!std::isfinite(image.values[i])) {
      result.failure = "a non-finite value at pixel " + std::to_string(i / 3);
      return result;
    }
  }
  const double weight = 1.0 / samples + 1.0 / reference.samples;
  // The difference's mean over a set of pixels, in standard errors. Float
  // rounding of the means sets a floor under the standard error, for pixels
  // whose every sample has the same radiance.
  struct Sum {
    double difference = 0;
    double variance = 0;
    double magnitude = 0;
    double pixels = 0;
    double Z() const {
      const double floor = 1e-6 + 1e-5 * magnitude / pixels;
      return difference / pixels /
             std::sqrt(variance / (pixels * pixels) + floor * floor);
    }
  };
  const auto add = [&](Sum& sum, std::size_t pixel, int channel) {
    const std::size_t i = pixel * 3 + static_cast<std::size_t>(channel);
    sum.difference += double{image.values[i]} - reference.mean.values[i];
    sum.variance += reference.variance.values[i] * weight;
    sum.magnitude += std::abs(double{reference.mean.values[i]});
    sum.pixels += 1;
  };
  const auto describe = [&](const char* where, const Sum& sum, int channel) {
    std::ostringstream message;
    message << samples << " spp: " << where << ", channel " << channel
            << ": mean difference " << sum.difference / sum.pixels << " is "
            << sum.Z() << " standard errors";
    return message.str();
  };

  for (int c = 0; c < 3; ++c) {
    Sum sum;
    for (std::size_t pixel = 0; pixel < count / 3; ++pixel)
      add(sum, pixel, c);
    result.image_z[c] = sum.Z();
    if (!(std::abs(result.image_z[c]) <= kMatchZ) && result.failure.empty())
      result.failure = describe("the image", sum, c);
  }

  result.tile = 8;
  while (std::uint64_t{result.tile} * result.tile * samples < 1024 &&
         result.tile < std::max(width, height))
    result.tile *= 2;
  double z2 = 0;
  double tests = 0;
  for (std::uint32_t y0 = 0; y0 < height; y0 += result.tile) {
    for (std::uint32_t x0 = 0; x0 < width; x0 += result.tile) {
      for (int c = 0; c < 3; ++c) {
        Sum sum;
        for (std::uint32_t y = y0; y < std::min(height, y0 + result.tile); ++y)
          for (std::uint32_t x = x0; x < std::min(width, x0 + result.tile); ++x)
            add(sum, std::size_t{y} * width + x, c);
        const double z = sum.Z();
        z2 += z * z;
        tests += 1;
        result.max_tile_z = std::max(result.max_tile_z, std::abs(z));
        if (!(std::abs(z) <= kMatchZ) && result.failure.empty()) {
          const std::string where =
              "the tile at " + std::to_string(x0) + "," + std::to_string(y0);
          result.failure = describe(where.c_str(), sum, c);
        }
      }
    }
  }
  result.mean_tile_z2 = z2 / tests;
  return result;
}

} // namespace LotusHeadless
