// SPDX-License-Identifier: Apache-2.0
#include "material_fixtures.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <map>
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

// Clears the world, then sets material-basics-v1's camera, dome, key light
// and displayColor backdrop under `root`, which every fixture shares.
void SetStudio(Lotus::RenderWorld& world, const std::string& root) {
  const auto old = world.Commit();
  for (const auto& [key, mesh] : old.scene->meshes) world.RemoveMesh(key);
  for (const auto& [key, material] : old.scene->materials) world.RemoveMaterial(key);
  for (const auto& [key, light] : old.scene->lights) world.RemoveLight(key);
  for (const auto& [key, texture] : old.scene->textures) world.RemoveTexture(key);
  world.SetEnvironmentMap(std::nullopt);

  // The camera 6.5 units in front of the subjects, looking down -Z: focal
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
  world.SetLight(root + "/Key", key);

  world.SetMesh(root + "/Backdrop", Backdrop(), {});
  const std::string backdrop = root + "/Backdrop.lotus:displayColor";
  world.SetMaterial(backdrop, Dielectric({0.5F, 0.5F, 0.5F}));
  world.BindMaterial(root + "/Backdrop", backdrop);
}

} // namespace

void SetMaterialBasics(Lotus::RenderWorld& world) {
  SetStudio(world, "/MaterialBasics");

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

namespace {

using Texel = std::array<std::uint8_t, 4>;

// scripts/material_fixtures.py's IMAGES, as the adapter decodes them: one
// channel becomes (r, r, r, 1), two (r, r, r, g) and three (r, g, b, 1).
Texel Palette(std::uint32_t x, std::uint32_t y) {
  return {std::uint8_t(40 + 60 * x), std::uint8_t(40 + 60 * y),
      std::uint8_t((x + y) % 2 == 0 ? 200 : 60), 255};
}

Texel Ramp(std::uint32_t x, std::uint32_t y) {
  const auto v = std::uint8_t(15 + 15 * (x + 4 * y));
  return {v, v, v, 255};
}

Texel GreyAlpha(std::uint32_t x, std::uint32_t y) {
  const auto v = std::uint8_t(50 + 60 * x);
  return {v, v, v, std::uint8_t(230 - 60 * y)};
}

Texel Rgba(std::uint32_t x, std::uint32_t y) {
  return {std::uint8_t(220 - 60 * x), std::uint8_t(40 + 60 * y),
      std::uint8_t(40 + 60 * ((x + y) % 4)), std::uint8_t(30 + 65 * x)};
}

Texel Checker(std::uint32_t x, std::uint32_t y) {
  const auto v = std::uint8_t((x + y) % 2 == 0 ? 255 : 0);
  return {v, v, v, 255};
}

// A square image's texels, rows from the top.
Lotus::Texture Image(std::uint32_t size, Lotus::TextureFormat format,
    Texel (*texel)(std::uint32_t, std::uint32_t)) {
  Lotus::Texture texture;
  texture.width = texture.height = size;
  texture.format = format;
  for (std::uint32_t y = 0; y < size; ++y)
    for (std::uint32_t x = 0; x < size; ++x) {
      const Texel value = texel(x, y);
      texture.texels.insert(texture.texels.end(), value.begin(), value.end());
    }
  return texture;
}

// A lookup of validation/materials/textures/<file> under a colour space,
// keyed relative to validation/materials/.
Lotus::TextureInput Lookup(const std::string& file, const std::string& space,
    Lotus::TextureWrap wrap_s = Lotus::TextureWrap::Clamp,
    Lotus::TextureWrap wrap_t = Lotus::TextureWrap::Clamp) {
  Lotus::TextureInput lookup;
  lookup.texture = "textures/" + file + "|" + space;
  lookup.wrap_s = wrap_s;
  lookup.wrap_t = wrap_t;
  return lookup;
}

Lotus::TextureInput Channel(Lotus::TextureInput lookup, std::uint32_t channel) {
  lookup.channel = channel;
  return lookup;
}

// A material whose base colour is `lookup`, at the set `texcoords`.
Lotus::Material Textured(Lotus::TextureInput lookup, std::string texcoords = "st") {
  Lotus::Material material;
  material.base_color_texture = std::move(lookup);
  material.texcoords = std::move(texcoords);
  return material;
}

using Texcoords = std::vector<std::array<float, 2>>;
using Face = std::array<std::array<float, 2>, 4>;

// A card's texture coordinates per triangle corner, from its four face
// vertices counter-clockwise from the bottom left: the fan (0, 1, 2),
// (0, 2, 3).
Texcoords Corners(const Face& face) {
  return {face[0], face[1], face[2], face[0], face[2], face[3]};
}

constexpr Face kUnit{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
constexpr Face kWide{{{-0.5F, -0.5F}, {1.5F, -0.5F}, {1.5F, 1.5F}, {-0.5F, 1.5F}}};
// uv1, where a card has it: st turned a quarter.
constexpr Face kTurned{{{1, 0}, {1, 1}, {0, 1}, {0, 0}}};

Lotus::MeshGeometry Card(std::map<std::string, Texcoords> texcoords) {
  Lotus::MeshGeometry mesh;
  mesh.positions = {{-0.5F, -0.5F, 0.0F}, {0.5F, -0.5F, 0.0F},
      {0.5F, 0.5F, 0.0F}, {-0.5F, 0.5F, 0.0F}};
  mesh.triangles = {{0, 1, 2}, {0, 2, 3}};
  mesh.source_faces = {0, 0};
  mesh.texcoords = std::move(texcoords);
  return mesh;
}

void DarkerMetal(Lotus::RenderWorld& world) {
  const std::string key = "/MaterialBasics/Looks/Metal50";
  Lotus::Material metal = world.Commit().scene->materials.at(key);
  for (float& channel : metal.base_color) channel *= 0.9F;
  world.SetMaterial(key, metal);
}

// As a texel-centre convention error would place the coordinates.
void ShiftedRepeat(Lotus::RenderWorld& world) {
  const std::string key = "/MaterialTextures/Cards/Repeat";
  const Lotus::SceneMesh mesh = world.Commit().scene->meshes.at(key);
  Lotus::MeshGeometry geometry = *mesh.geometry;
  for (auto& st : geometry.texcoords.at("st")) st[0] += 0.125F;
  world.SetMesh(key, std::move(geometry), mesh.instance);
}

} // namespace

void SetMaterialTextures(Lotus::RenderWorld& world) {
  const std::string root = "/MaterialTextures";
  SetStudio(world, root);

  // Every image a translated lookup names. Under auto, the 8-bit
  // three-channel palette is sRGB and the one- and two-channel images are
  // not.
  constexpr auto kSrgb = Lotus::TextureFormat::Rgba8Srgb;
  constexpr auto kUnorm = Lotus::TextureFormat::Rgba8Unorm;
  world.SetTexture("textures/palette.png|sRGB", Image(4, kSrgb, Palette));
  world.SetTexture("textures/palette.png|raw", Image(4, kUnorm, Palette));
  world.SetTexture("textures/palette.png|auto", Image(4, kSrgb, Palette));
  world.SetTexture("textures/ramp.png|auto", Image(4, kUnorm, Ramp));
  world.SetTexture("textures/grey-alpha.png|auto", Image(4, kUnorm, GreyAlpha));
  world.SetTexture("textures/rgba.png|sRGB", Image(4, kSrgb, Rgba));
  world.SetTexture("textures/rgba.png|raw", Image(4, kUnorm, Rgba));
  world.SetTexture("textures/checker.png|raw", Image(64, kUnorm, Checker));

  using Lotus::TextureWrap;
  const auto palette = [](TextureWrap s = TextureWrap::Clamp,
                           TextureWrap t = TextureWrap::Clamp) {
    return Lookup("palette.png", "sRGB", s, t);
  };
  Lotus::Material grey_alpha = Textured(Lookup("grey-alpha.png", "auto"));
  grey_alpha.roughness_texture = Channel(Lookup("grey-alpha.png", "auto"), 3);
  Lotus::Material alpha = Textured(Lookup("rgba.png", "sRGB"));
  alpha.roughness_texture = Channel(Lookup("rgba.png", "sRGB"), 3);
  Lotus::Material orm;
  orm.base_color = {0.95F, 0.64F, 0.54F};
  orm.metallic_texture = Channel(Lookup("rgba.png", "raw"), 2);
  orm.roughness_texture = Channel(Lookup("rgba.png", "raw"), 1);
  orm.texcoords = "st";
  Lotus::TextureInput scale_bias = Lookup("palette.png", "raw");
  scale_bias.scale = {2.0F, 2.0F, 2.0F, 1.0F};
  scale_bias.bias = {-0.5F, -0.5F, -0.5F, 0.0F};
  // The unresolved file keeps its authored path. Neither lookup names an
  // image, so both return their fallbacks.
  Lotus::TextureInput missing = palette();
  missing.texture = "./textures/missing.png|sRGB";
  missing.scale = {0.5F, 0.5F, 0.5F, 1.0F};
  missing.fallback = {0.9F, 0.2F, 0.6F, 1.0F};
  Lotus::TextureInput no_file = palette();
  no_file.texture = "|auto";
  no_file.fallback = {0.2F, 0.6F, 0.9F, 1.0F};
  Lotus::Material constant = Textured(palette(), "");
  constant.texcoord_fallback = {0.6F, 0.3F};
  Lotus::Material absent = Textured(palette(), "absent");
  absent.texcoord_fallback = {0.1F, 0.9F};
  // Untranslated connections leave their inputs' defaults: a UsdTransform2d
  // between the reader and the texture, the ramp's metallic at a second
  // texture-coordinate source, a colour output on roughness and a scalar
  // output on the base colour.
  Lotus::Material colour_on_scalar;
  colour_on_scalar.base_color = {0.3F, 0.5F, 0.7F};

  using Sets = std::map<std::string, Texcoords>;
  const Sets unit{{"st", Corners(kUnit)}};
  const Sets wide{{"st", Corners(kWide)}};
  const Sets two{{"st", Corners(kUnit)}, {"uv1", Corners(kTurned)}};
  struct Entry {
    const char* name;
    Lotus::Material material;
    Sets texcoords;
  };
  const std::array<Entry, 25> cards{{
      {"UseMetadata", Textured(palette(TextureWrap::Black, TextureWrap::Black)), wide},
      {"Clamp", Textured(palette()), wide},
      {"Repeat", Textured(palette(TextureWrap::Repeat, TextureWrap::Repeat)), wide},
      {"Mirror", Textured(palette(TextureWrap::Mirror, TextureWrap::Mirror)), wide},
      {"Mixed", Textured(palette(TextureWrap::Repeat, TextureWrap::Black)), wide},
      {"Srgb", Textured(palette()), unit},
      {"Raw", Textured(Lookup("palette.png", "raw")), unit},
      {"Auto", Textured(Lookup("palette.png", "auto")), unit},
      {"Grey", Textured(Lookup("ramp.png", "auto")), unit},
      {"GreyAlpha", grey_alpha, unit},
      {"Alpha", alpha, unit},
      {"Orm", orm, unit},
      {"ScaleBias", Textured(scale_bias), unit},
      {"Missing", Textured(missing), unit},
      {"NoFile", Textured(no_file), unit},
      // Per vertex, which reaches the corners as per face vertex would.
      {"Vertex", Textured(palette()), unit},
      // (0, 0), (1, 0), (0, 1), (1, 1) indexed by 1, 0, 2, 3: mirrored in s.
      {"Indexed", Textured(palette()), {{"st", Corners({{{1, 0}, {0, 0}, {0, 1}, {1, 1}}})}}},
      {"Named", Textured(palette(), "uv1"), two},
      {"Constant", constant, unit},
      {"Absent", absent, unit},
      {"Transform2d", Lotus::Material{}, unit},
      {"SecondSource", Textured(palette()), two},
      {"ColourOnScalar", colour_on_scalar, unit},
      {"ScalarOnColour", Lotus::Material{}, unit},
      {"Minified",
          Textured(Lookup("checker.png", "raw", TextureWrap::Repeat, TextureWrap::Repeat)),
          unit},
  }};
  constexpr std::array<float, 5> kColumns{-1.6F, -0.8F, 0.0F, 0.8F, 1.6F};
  constexpr std::array<float, 5> kRows{1.6F, 0.8F, 0.0F, -0.8F, -1.6F};
  for (std::size_t i = 0; i < cards.size(); ++i) {
    const std::string name = cards[i].name;
    const std::string mesh = root + "/Cards/" + name;
    const std::string material = root + "/Looks/" + name;
    Lotus::MeshInstance placed;
    placed.world_from_object[0] = placed.world_from_object[5] =
        placed.world_from_object[10] = 0.7F;
    placed.world_from_object[12] = kColumns[i % 5];
    placed.world_from_object[13] = kRows[i / 5];
    world.SetMesh(mesh, Card(cards[i].texcoords), placed);
    world.SetMaterial(material, cards[i].material);
    world.BindMaterial(mesh, material);
  }
}

std::span<const MaterialFixture> MaterialFixtures() {
  static constexpr MaterialFixture kFixtures[] = {
      {"material-basics-v1", "/MaterialBasics", 128, SetMaterialBasics,
          "the half-rough metal 10% darker", DarkerMetal},
      {"material-textures-v1", "/MaterialTextures", 160, SetMaterialTextures,
          "the repeating card half a texel off", ShiftedRepeat},
  };
  return kFixtures;
}

} // namespace LotusHeadless
