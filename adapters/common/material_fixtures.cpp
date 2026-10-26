// SPDX-License-Identifier: Apache-2.0
#include "material_fixtures.hpp"

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace LotusHeadless {
namespace {

constexpr double kPi = 3.14159265358979323846;
// scripts/material_fixtures.py's sphere tessellation.
constexpr std::uint32_t kSegments = 32;
constexpr std::uint32_t kRings = 16;

// A coordinate as the USD file spells it: rounded to a multiple of 1e-6.
// Adding 0 turns a rounded -0 into the +0 the file's "0.000000" parses as.
float Fixed(double value) {
  return static_cast<float>(std::round(value * 1e6) / 1e6 + 0.0);
}

// The unit sphere: its points double as its authored vertex normals. The
// adapter triangulates each face as a fan from its first vertex.
Lotus::MeshGeometry Ball() {
  std::vector<std::array<float, 3>> points{{0.0F, 1.0F, 0.0F}};
  for (std::uint32_t i = 1; i < kRings; ++i) {
    const double theta = kPi * i / kRings;
    for (std::uint32_t j = 0; j < kSegments; ++j) {
      const double phi = 2 * kPi * j / kSegments;
      points.push_back({Fixed(std::sin(theta) * std::cos(phi)),
          Fixed(std::cos(theta)), Fixed(-std::sin(theta) * std::sin(phi))});
    }
  }
  points.push_back({0.0F, -1.0F, 0.0F});
  const auto bottom = static_cast<std::uint32_t>(points.size() - 1);
  const auto ring = [](std::uint32_t i, std::uint32_t j) {
    return 1 + (i - 1) * kSegments + j % kSegments;
  };
  std::vector<std::vector<std::uint32_t>> faces;
  for (std::uint32_t j = 0; j < kSegments; ++j)
    faces.push_back({0, ring(1, j), ring(1, j + 1)});
  for (std::uint32_t i = 1; i + 1 < kRings; ++i)
    for (std::uint32_t j = 0; j < kSegments; ++j)
      faces.push_back({ring(i, j), ring(i + 1, j), ring(i + 1, j + 1), ring(i, j + 1)});
  for (std::uint32_t j = 0; j < kSegments; ++j)
    faces.push_back({ring(kRings - 1, j), bottom, ring(kRings - 1, j + 1)});

  Lotus::MeshGeometry mesh;
  mesh.positions = points;
  for (std::uint32_t face = 0; face < faces.size(); ++face) {
    const auto& vertices = faces[face];
    for (std::size_t k = 1; k + 1 < vertices.size(); ++k) {
      const std::array<std::uint32_t, 3> triangle{vertices[0], vertices[k], vertices[k + 1]};
      mesh.triangles.push_back(triangle);
      mesh.source_faces.push_back(face);
      for (const auto vertex : triangle) mesh.normals.push_back(points[vertex]);
    }
  }
  return mesh;
}

Lotus::MeshGeometry Backdrop() {
  Lotus::MeshGeometry mesh;
  mesh.positions = {{-3.0F, -3.0F, -1.0F}, {3.0F, -3.0F, -1.0F},
      {3.0F, 3.0F, -1.0F}, {-3.0F, 3.0F, -1.0F}};
  mesh.triangles = {{0, 1, 2}, {0, 2, 3}};
  mesh.source_faces = {0, 0};
  return mesh;
}

using Matrix4d = std::array<double, 16>;

// Column-major products and right-handed rotations by `degrees`, in double
// precision as the adapter composes USD transforms, rounded to float once.
Matrix4d Multiply(const Matrix4d& a, const Matrix4d& b) {
  Matrix4d result{};
  for (int column = 0; column < 4; ++column)
    for (int row = 0; row < 4; ++row)
      for (int k = 0; k < 4; ++k)
        result[column * 4 + row] += a[k * 4 + row] * b[column * 4 + k];
  return result;
}

Matrix4d RotationX(double degrees) {
  const double c = std::cos(degrees * kPi / 180), s = std::sin(degrees * kPi / 180);
  return {1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, 1};
}

Matrix4d RotationY(double degrees) {
  const double c = std::cos(degrees * kPi / 180), s = std::sin(degrees * kPi / 180);
  return {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1};
}

Lotus::Matrix4 ToFloat(const Matrix4d& matrix) {
  Lotus::Matrix4 result{};
  for (int i = 0; i < 16; ++i) result[i] = static_cast<float>(matrix[i]);
  return result;
}

Lotus::Material Dielectric(const std::array<float, 3>& base, float roughness = 0.5F) {
  Lotus::Material material;
  material.base_color = base;
  material.roughness = roughness;
  return material;
}

Lotus::Material Metal(const std::array<float, 3>& base, float roughness, float metallic) {
  Lotus::Material material = Dielectric(base, roughness);
  material.metallic = metallic;
  return material;
}

} // namespace

void SetMaterialBasics(Lotus::RenderWorld& world) {
  const auto old = world.Commit();
  for (const auto& [key, mesh] : old.scene->meshes) world.RemoveMesh(key);
  for (const auto& [key, material] : old.scene->materials) world.RemoveMaterial(key);
  for (const auto& [key, light] : old.scene->lights) world.RemoveLight(key);
  for (const auto& [key, texture] : old.scene->textures) world.RemoveTexture(key);
  world.SetEnvironmentMap(std::nullopt);

  // The camera 6.5 units in front of the balls, looking down -Z: focal
  // length 50 over a 32 aperture, so tan(half field of view) = 0.32; its
  // clipping range is 1 to 100.
  constexpr float kFocal = 3.125F;
  constexpr float kNear = 1.0F;
  constexpr float kFar = 100.0F;
  Lotus::Camera camera;
  camera.view[14] = -6.5F;
  camera.projection = {kFocal, 0.0F, 0.0F, 0.0F, 0.0F, kFocal, 0.0F, 0.0F,
      0.0F, 0.0F, (kFar + kNear) / (kNear - kFar), -1.0F,
      0.0F, 0.0F, 2.0F * kFar * kNear / (kNear - kFar), 0.0F};
  world.SetCamera(camera);

  world.SetEnvironment({0.5F, 0.5F, 0.5F});
  Lotus::Light key;
  key.shape = Lotus::LightShape::Rect;
  Matrix4d translation{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1.5, 2.5, 4, 1};
  key.world_from_light =
      ToFloat(Multiply(translation, Multiply(RotationY(20), RotationX(-30))));
  key.radiance = {4.0F, 4.0F, 4.0F};
  key.width = 3.0F;
  key.height = 3.0F;
  world.SetLight("/MaterialBasics/Key", key);

  world.SetMesh("/MaterialBasics/Backdrop", Backdrop(), {});
  const std::string backdrop = "/MaterialBasics/Backdrop.lotus:displayColor";
  world.SetMaterial(backdrop, Dielectric({0.5F, 0.5F, 0.5F}));
  world.BindMaterial("/MaterialBasics/Backdrop", backdrop);

  Lotus::Material specular = Dielectric({0.05F, 0.05F, 0.05F}, 0.2F);
  specular.use_specular_workflow = true;
  specular.specular_color = {0.9F, 0.6F, 0.3F};
  Lotus::Material ior = Dielectric({0.1F, 0.2F, 0.6F}, 0.1F);
  ior.ior = 2.5F;
  constexpr std::array<float, 3> kGrey{0.5F, 0.5F, 0.5F};
  constexpr std::array<float, 3> kCopper{0.95F, 0.64F, 0.54F};
  constexpr std::array<float, 3> kGold{0.9F, 0.6F, 0.2F};
  const std::array<std::pair<const char*, Lotus::Material>, 16> balls{{
      {"White", Dielectric({0.8F, 0.8F, 0.8F})},
      {"Red", Dielectric({0.8F, 0.1F, 0.05F})},
      {"Dark", Dielectric({0.04F, 0.04F, 0.04F})},
      // Authored (1.25, 0.5, -0.25) and roughness 1.5.
      {"Clamped", Dielectric({1.0F, 0.5F, 0.0F}, 1.0F)},
      {"Gloss0", Dielectric(kGrey, 0.0F)},
      {"Gloss25", Dielectric(kGrey, 0.25F)},
      {"Gloss50", Dielectric(kGrey, 0.5F)},
      {"Gloss100", Dielectric(kGrey, 1.0F)},
      {"Metal0", Metal(kCopper, 0.0F, 1.0F)},
      {"Metal25", Metal(kCopper, 0.25F, 1.0F)},
      {"Metal50", Metal(kCopper, 0.5F, 1.0F)},
      {"Metal100", Metal(kCopper, 1.0F, 1.0F)},
      {"Metallic25", Metal(kGold, 0.3F, 0.25F)},
      {"Metallic75", Metal(kGold, 0.3F, 0.75F)},
      {"Ior25", ior},
      // Authored metallic 1 and ior 3, which the specular workflow ignores.
      {"Specular", specular},
  }};
  constexpr std::array<float, 4> kColumns{-1.35F, -0.45F, 0.45F, 1.35F};
  constexpr std::array<float, 4> kRows{1.35F, 0.45F, -0.45F, -1.35F};
  const Lotus::MeshGeometry ball = Ball();
  for (std::size_t i = 0; i < balls.size(); ++i) {
    const std::string name = balls[i].first;
    const std::string mesh = "/MaterialBasics/Balls/" + name;
    const std::string material = "/MaterialBasics/Looks/" + name;
    Lotus::MeshInstance placed;
    placed.world_from_object[0] = placed.world_from_object[5] =
        placed.world_from_object[10] = 0.4F;
    placed.world_from_object[12] = kColumns[i % 4];
    placed.world_from_object[13] = kRows[i / 4];
    world.SetMesh(mesh, ball, placed);
    world.SetMaterial(material, balls[i].second);
    world.BindMaterial(mesh, material);
  }
}

} // namespace LotusHeadless
