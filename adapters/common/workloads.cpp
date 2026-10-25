// SPDX-License-Identifier: Apache-2.0
#include "workloads.hpp"
#include "reference.hpp"

#include <cmath>
#include <stdexcept>

namespace LotusHeadless {
namespace {
Lotus::MeshGeometry Grid(std::uint32_t divisions, float extent, bool uv) {
  Lotus::MeshGeometry mesh;
  for (std::uint32_t y = 0; y <= divisions; ++y)
    for (std::uint32_t x = 0; x <= divisions; ++x)
      mesh.positions.push_back({extent * (2.0F * x / divisions - 1.0F),
          extent * (2.0F * y / divisions - 1.0F),
          0.002F * static_cast<float>((x * 17 + y * 13) % 11)});
  for (std::uint32_t y = 0; y < divisions; ++y) {
    for (std::uint32_t x = 0; x < divisions; ++x) {
      const auto a = y * (divisions + 1) + x;
      const auto b = a + 1;
      const auto d = a + divisions + 1;
      const auto c = d + 1;
      mesh.triangles.push_back({a, b, c});
      mesh.triangles.push_back({a, c, d});
      mesh.source_faces.insert(mesh.source_faces.end(), 2, y * divisions + x);
    }
  }
  if (uv) {
    auto& coordinates = mesh.texcoords["st"];
    for (const auto& triangle : mesh.triangles)
      for (const auto vertex : triangle) {
        const auto& p = mesh.positions[vertex];
        coordinates.push_back({(p[0] / extent + 1) * 0.5F,
            (p[1] / extent + 1) * 0.5F});
      }
  }
  return mesh;
}

Lotus::Matrix4 Translation(float x, float y, float z) {
  auto matrix = Lotus::IdentityMatrix();
  matrix[12] = x;
  matrix[13] = y;
  matrix[14] = z;
  return matrix;
}

// Right-handed rotations by `degrees` about the X and Y axes.
Lotus::Matrix4 RotationX(double degrees) {
  const auto c = static_cast<float>(std::cos(degrees * 3.14159265358979323846 / 180));
  const auto s = static_cast<float>(std::sin(degrees * 3.14159265358979323846 / 180));
  auto matrix = Lotus::IdentityMatrix();
  matrix[5] = c;
  matrix[6] = s;
  matrix[9] = -s;
  matrix[10] = c;
  return matrix;
}

Lotus::Matrix4 RotationY(double degrees) {
  const auto c = static_cast<float>(std::cos(degrees * 3.14159265358979323846 / 180));
  const auto s = static_cast<float>(std::sin(degrees * 3.14159265358979323846 / 180));
  auto matrix = Lotus::IdentityMatrix();
  matrix[0] = c;
  matrix[2] = -s;
  matrix[8] = s;
  matrix[10] = c;
  return matrix;
}

} // namespace

void SetLightGrid(Lotus::RenderWorld& world, unsigned side) {
  if (side == 0 || side > kManyLightGrid)
    throw std::invalid_argument("light grid side must be 1.." + std::to_string(kManyLightGrid));
  for (unsigned i = 0; i < kManyLightGrid * kManyLightGrid; ++i)
    world.RemoveLight("/lights/" + std::to_string(i));
  // The full grid's spacing is exactly 0.12; one light sits at the centre.
  const float spacing = side == 1 ? 0.0F : 0.12F * (15.0F / float(side - 1));
  const float first = side == 1 ? 0.0F : -0.9F;
  const float scale = float(kManyLightGrid * kManyLightGrid) / float(side * side);
  for (unsigned y = 0; y < side; ++y) {
    for (unsigned x = 0; x < side; ++x) {
      Lotus::Light light;
      light.world_from_light = Translation(first + spacing * x,
          0.85F, first + spacing * y);
      light.radius = 0.025F;
      light.radiance = {12.0F * scale, 10.0F * scale, 8.0F * scale};
      world.SetLight("/lights/" + std::to_string(y * side + x), light);
    }
  }
}

// The lit workloads light the reference scene's geometry as its emissive
// panel does not: by analytic lights, and by an environment image.
Workload MakeWorkload(std::string_view id, const std::filesystem::path& assets) {
  if (!KnownWorkload(id)) throw std::invalid_argument("unknown workload: " + std::string(id));
  Workload work;
  SetCornellBox(work.world);
  if (id == "cornell-v1") {
    work.edit_mesh = "/cornell/back";
    return work;
  }
  if (id == "many-lights-v1") {
    work.world.RemoveMesh("/cornell/light");
    work.world.RemoveMaterial("/materials/light");
    SetLightGrid(work.world, kManyLightGrid);
    work.edit_mesh = "/cornell/back";
    return work;
  }
  if (id == "lights-v1") {
    // Without its ceiling and panel the box is lit by one light of each
    // analytic kind: a sun through the open top, a sphere, a rect where the
    // ceiling was and a disk on the green wall.
    work.world.RemoveMesh("/cornell/ceiling");
    work.world.RemoveMesh("/cornell/light");
    work.world.RemoveMaterial("/materials/light");
    Lotus::Light sun;
    sun.shape = Lotus::LightShape::Distant;
    sun.world_from_light = Lotus::Multiply(RotationY(20), RotationX(-60));
    sun.radiance = {2.5F, 2.3F, 2.0F};
    sun.normalize = true;
    work.world.SetLight("/lights/sun", sun);
    Lotus::Light sphere;
    sphere.world_from_light = Translation(-0.5F, 0.55F, 0.2F);
    sphere.radiance = {40.0F, 32.0F, 24.0F};
    sphere.radius = 0.12F;
    work.world.SetLight("/lights/sphere", sphere);
    Lotus::Light rect;
    rect.shape = Lotus::LightShape::Rect;
    rect.world_from_light =
        Lotus::Multiply(Translation(0.3F, 0.95F, -0.4F), RotationX(-90));
    rect.radiance = {12.0F, 12.0F, 12.0F};
    rect.width = 0.4F;
    rect.height = 0.25F;
    work.world.SetLight("/lights/rect", rect);
    Lotus::Light disk;
    disk.shape = Lotus::LightShape::Disk;
    disk.world_from_light =
        Lotus::Multiply(Translation(0.97F, 0.3F, -0.5F), RotationY(90));
    disk.radiance = {6.0F, 8.0F, 12.0F};
    disk.radius = 0.15F;
    work.world.SetLight("/lights/disk", disk);
    work.edit_mesh = "/cornell/back";
    return work;
  }
  if (id == "environment-v1") {
    // The blocks on a floor 120 units wide under OpenUSD's Stinson Beach
    // image, whose sun is behind the camera. The camera looks 25 degrees
    // down and sees only the floor and the blocks.
    for (const char* mesh : {"/cornell/ceiling", "/cornell/back", "/cornell/left",
             "/cornell/right", "/cornell/light"})
      work.world.RemoveMesh(mesh);
    for (const char* material : {"/materials/red", "/materials/green", "/materials/light"})
      work.world.RemoveMaterial(material);
    Lotus::MeshInstance ground;
    ground.world_from_object[0] = ground.world_from_object[10] = 60.0F;
    work.world.SetMeshInstance("/cornell/floor", ground);
    auto camera = work.world.Commit().camera;
    camera.view = Lotus::Multiply(RotationX(25), Translation(0.0F, -0.8F, -3.0F));
    work.world.SetCamera(camera);
    Lotus::Texture beach;
    const auto failure = ReadRadianceHdr(assets / "StinsonBeach.hdr", beach);
    if (!failure.empty()) throw std::runtime_error(failure);
    work.world.SetTexture("/beach", std::move(beach));
    Lotus::EnvironmentMap map;
    map.texture = "/beach";
    work.world.SetEnvironmentMap(map);
    work.edit_mesh = "/cornell/floor";
    return work;
  }
  const auto old = work.world.Commit();
  for (const auto& [key, mesh] : old.scene->meshes) work.world.RemoveMesh(key);
  for (const auto& [key, material] : old.scene->materials) work.world.RemoveMaterial(key);
  work.world.SetEnvironment({1.0F, 1.0F, 1.0F});
  if (id == "textured-v1") {
    // Sixteen independent textured patches, 32x32 quads each, filling the view.
    for (std::uint32_t i = 0; i < 16; ++i) {
      const std::string key = "/tiles/" + std::to_string(i);
      Lotus::MeshInstance placed;
      placed.world_from_object[12] = -0.9375F + 0.625F * (i % 4);
      placed.world_from_object[13] = -0.9375F + 0.625F * (i / 4);
      work.world.SetMesh(key, Grid(32, 0.3125F, true), placed);
      Lotus::Texture texture;
      texture.width = texture.height = 256;
      texture.format = Lotus::TextureFormat::Rgba8Srgb;
      for (std::uint32_t y = 0; y < 256; ++y)
        for (std::uint32_t x = 0; x < 256; ++x) {
          const std::uint8_t checker = ((x / 16 + y / 16) % 2) ? 220 : 80;
          texture.texels.insert(texture.texels.end(), {checker,
              static_cast<std::uint8_t>(40 + i * 12),
              static_cast<std::uint8_t>(60 + x / 2), 255});
        }
      work.world.SetTexture(key, std::move(texture));
      Lotus::Material material;
      material.texcoords = "st";
      material.base_color_texture = Lotus::TextureInput{};
      material.base_color_texture->texture = key;
      material.base_color_texture->wrap_s = material.base_color_texture->wrap_t =
          Lotus::TextureWrap::Repeat;
      material.roughness = 0.25F + 0.04F * i;
      work.world.SetMaterial(key, material);
      work.world.BindMaterial(key, key);
    }
    work.edit_mesh = "/tiles/0";
  } else if (id == "instanced-v1") {
    Lotus::MeshInstance placed;
    placed.instancer_transforms.emplace();
    for (std::uint32_t y = 0; y < 64; ++y)
      for (std::uint32_t x = 0; x < 64; ++x) {
        auto transform = Lotus::IdentityMatrix();
        transform[12] = -1.23046875F + 0.0390625F * x;
        transform[13] = -1.23046875F + 0.0390625F * y;
        placed.instancer_transforms->push_back(transform);
      }
    work.edit_mesh = "/prototype";
    work.world.SetMesh(work.edit_mesh, Grid(1, 0.01953125F, false), placed);
  } else {
    work.edit_mesh = "/grid";
    work.world.SetMesh(work.edit_mesh, Grid(512, 1.25F, false), {});
  }
  return work;
}

} // namespace LotusHeadless
