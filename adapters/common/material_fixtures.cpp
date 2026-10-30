// SPDX-License-Identifier: Apache-2.0
#include "material_fixtures.hpp"

#include <algorithm>
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

namespace {

// scripts/material_fixtures.py's coarse sphere.
constexpr std::uint32_t kCoarseSegments = 16;
constexpr std::uint32_t kCoarseRings = 8;

using Point = std::array<double, 3>;
using Normal = std::array<float, 3>;

std::array<float, 3> Fixed(const Point& point) {
  return {Fixed(point[0]), Fixed(point[1]), Fixed(point[2])};
}

// A mesh as the adapter triangulates it: fans from each face's first vertex,
// or, left-handed, (v0, v[k + 1], v[k]) and for a quad (v2, v1, v0),
// (v2, v0, v3), so that every triangle winds counter-clockwise seen from
// outside. Each corner remembers its vertex, face and face-vertex index, to
// expand primvars of any interpolation.
struct Polygons {
  std::vector<Point> points;
  std::vector<std::vector<std::uint32_t>> faces;
  bool left_handed = false;

  struct Corner {
    std::uint32_t vertex;
    std::uint32_t face;
    std::uint32_t face_vertex;
  };

  std::vector<Corner> Corners() const {
    std::vector<Corner> corners;
    std::uint32_t first = 0;
    for (std::uint32_t face = 0; face < faces.size(); ++face) {
      const auto& vertices = faces[face];
      std::vector<std::array<std::uint32_t, 3>> triangles;
      if (left_handed && vertices.size() == 4) {
        // HdMeshUtil starts a left-handed quad's triangles at its third vertex.
        triangles = {{2, 1, 0}, {2, 0, 3}};
      } else {
        for (std::uint32_t k = 1; k + 1 < vertices.size(); ++k)
          triangles.push_back(left_handed ? std::array{0U, k + 1, k} : std::array{0U, k, k + 1});
      }
      for (const auto& triangle : triangles)
        for (const std::uint32_t i : triangle) corners.push_back({vertices[i], face, first + i});
      first += static_cast<std::uint32_t>(vertices.size());
    }
    return corners;
  }

  Lotus::MeshGeometry Geometry() const {
    Lotus::MeshGeometry mesh;
    for (const Point& point : points) mesh.positions.push_back(Fixed(point));
    const auto corners = Corners();
    for (std::size_t i = 0; i < corners.size(); i += 3) {
      mesh.triangles.push_back(
          {corners[i].vertex, corners[i + 1].vertex, corners[i + 2].vertex});
      mesh.source_faces.push_back(corners[i].face);
    }
    return mesh;
  }

  // Storm's coarse smooth normals, as Hd_SmoothNormals computes them in
  // float: each vertex sums cross(next - vertex, previous - vertex) over its
  // faces' corners in face order, reversed for left-handed faces, then
  // divides by the float length as GfVec3f::Normalize does.
  std::vector<Normal> SmoothNormals() const {
    const auto positions = Geometry().positions;
    std::vector<Normal> sums(points.size(), Normal{0.0F, 0.0F, 0.0F});
    for (const auto& vertices : faces) {
      const std::size_t count = vertices.size();
      for (std::size_t i = 0; i < count; ++i) {
        const auto& curr = positions[vertices[i]];
        auto prev = positions[vertices[(i + count - 1) % count]];
        auto next = positions[vertices[(i + 1) % count]];
        if (left_handed) std::swap(prev, next);
        const Normal a{next[0] - curr[0], next[1] - curr[1], next[2] - curr[2]};
        const Normal b{prev[0] - curr[0], prev[1] - curr[1], prev[2] - curr[2]};
        Normal& sum = sums[vertices[i]];
        sum[0] += a[1] * b[2] - a[2] * b[1];
        sum[1] += a[2] * b[0] - a[0] * b[2];
        sum[2] += a[0] * b[1] - a[1] * b[0];
      }
    }
    for (Normal& sum : sums) {
      const float length = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
      const double scale = 1.0 / std::max(length, 1e-10F);
      for (float& c : sum) c = static_cast<float>(c * scale);
    }
    return sums;
  }
};

// The coarse unit sphere, faces counter-clockwise seen from outside; with
// `reversed`, each face's vertices in reverse under left-handed orientation.
Polygons CoarseSphere(bool reversed = false) {
  Polygons sphere;
  sphere.points.push_back({0.0, 1.0, 0.0});
  for (std::uint32_t i = 1; i < kCoarseRings; ++i) {
    const double theta = kPi * i / kCoarseRings;
    for (std::uint32_t j = 0; j < kCoarseSegments; ++j) {
      const double phi = 2 * kPi * j / kCoarseSegments;
      sphere.points.push_back({std::sin(theta) * std::cos(phi), std::cos(theta),
          -std::sin(theta) * std::sin(phi)});
    }
  }
  sphere.points.push_back({0.0, -1.0, 0.0});
  const auto bottom = static_cast<std::uint32_t>(sphere.points.size() - 1);
  const auto ring = [](std::uint32_t i, std::uint32_t j) {
    return 1 + (i - 1) * kCoarseSegments + j % kCoarseSegments;
  };
  for (std::uint32_t j = 0; j < kCoarseSegments; ++j)
    sphere.faces.push_back({0, ring(1, j), ring(1, j + 1)});
  for (std::uint32_t i = 1; i + 1 < kCoarseRings; ++i)
    for (std::uint32_t j = 0; j < kCoarseSegments; ++j)
      sphere.faces.push_back(
          {ring(i, j), ring(i + 1, j), ring(i + 1, j + 1), ring(i, j + 1)});
  for (std::uint32_t j = 0; j < kCoarseSegments; ++j)
    sphere.faces.push_back({ring(kCoarseRings - 1, j), bottom, ring(kCoarseRings - 1, j + 1)});
  if (reversed) {
    for (auto& face : sphere.faces) std::reverse(face.begin(), face.end());
    sphere.left_handed = true;
  }
  return sphere;
}

// The coarse sphere's (s, t) per face vertex, in face order, times `scale`:
// scripts/material_fixtures.py's latlong.
std::vector<std::array<float, 2>> LatLong(std::array<double, 2> scale) {
  const double segments = kCoarseSegments, rings = kCoarseRings;
  std::vector<std::array<float, 2>> values;
  const auto st = [&](double s, double t) {
    values.push_back({static_cast<float>(s * scale[0]), static_cast<float>(t * scale[1])});
  };
  for (std::uint32_t j = 0; j < kCoarseSegments; ++j) {
    st((j + 0.5) / segments, 1);
    st(j / segments, 1 - 1 / rings);
    st((j + 1) / segments, 1 - 1 / rings);
  }
  for (std::uint32_t i = 1; i + 1 < kCoarseRings; ++i)
    for (std::uint32_t j = 0; j < kCoarseSegments; ++j) {
      const double top = 1 - i / rings, bottom = 1 - (i + 1) / rings;
      st(j / segments, top);
      st(j / segments, bottom);
      st((j + 1) / segments, bottom);
      st((j + 1) / segments, top);
    }
  for (std::uint32_t j = 0; j < kCoarseSegments; ++j) {
    st(j / segments, 1 / rings);
    st((j + 0.5) / segments, 0);
    st((j + 1) / segments, 1 / rings);
  }
  return values;
}

Texel Tilts(std::uint32_t x, std::uint32_t y) {
  return {std::uint8_t(32 + 64 * x), std::uint8_t(224 - 64 * y), 200, 255};
}

Texel TiltsDirectX(std::uint32_t x, std::uint32_t y) {
  return {std::uint8_t(32 + 64 * x), std::uint8_t(31 + 64 * y), 200, 255};
}

// An unsigned normal map decoded to signed by scale 2 and bias -1.
Lotus::TextureInput NormalMap(const std::string& file, const std::string& space = "raw",
    Lotus::TextureWrap wrap = Lotus::TextureWrap::Clamp) {
  Lotus::TextureInput lookup = Lookup(file, space, wrap, wrap);
  lookup.scale = {2.0F, 2.0F, 2.0F, 1.0F};
  lookup.bias = {-1.0F, -1.0F, -1.0F, 0.0F};
  return lookup;
}

Lotus::Material Bumpy(std::optional<Lotus::TextureInput> normal) {
  Lotus::Material material = Dielectric({0.7F, 0.7F, 0.7F}, 0.4F);
  if (normal) material.texcoords = "st";
  material.normal_texture = std::move(normal);
  return material;
}

// As a DirectX/OpenGL green convention error would read the map.
void FlippedGreen(Lotus::RenderWorld& world) {
  const std::string key = "/MaterialNormals/Looks/Mapped";
  Lotus::Material material = world.Commit().scene->materials.at(key);
  material.normal_texture->scale[1] = -2.0F;
  material.normal_texture->bias[1] = 1.0F;
  world.SetMaterial(key, material);
}

} // namespace

void SetMaterialNormals(Lotus::RenderWorld& world) {
  const std::string root = "/MaterialNormals";
  SetStudio(world, root);

  constexpr auto kUnorm = Lotus::TextureFormat::Rgba8Unorm;
  world.SetTexture("textures/tilts.png|raw", Image(4, kUnorm, Tilts));
  // Under auto the 8-bit three-channel map is sRGB.
  world.SetTexture("textures/tilts.png|auto", Image(4, Lotus::TextureFormat::Rgba8Srgb, Tilts));
  world.SetTexture("textures/tilts-dx.png|raw", Image(4, kUnorm, TiltsDirectX));

  Lotus::TextureInput unscaled = Lookup("tilts.png", "raw");
  Lotus::TextureInput directx = NormalMap("tilts-dx.png");
  directx.scale[1] = -2.0F;
  directx.bias[1] = 1.0F;
  // Without a lookup the constant names no texture-coordinate set, so it
  // takes the normal-only frame whether or not the mesh has st.
  Lotus::Material constant = Bumpy(std::nullopt);
  constant.normal = {0.4F, 0.3F, 0.866F};
  Lotus::Material absent = Bumpy(NormalMap("tilts.png"));
  absent.texcoords = "absent";
  absent.texcoord_fallback = {0.25F, 0.75F};
  Lotus::Material glossy = Metal({0.95F, 0.64F, 0.54F}, 0.25F, 1.0F);
  glossy.normal_texture = NormalMap("tilts.png", "raw", Lotus::TextureWrap::Repeat);
  glossy.texcoords = "st";
  const std::map<std::string, Lotus::Material> looks{
      {"Smooth", Dielectric({0.6F, 0.6F, 0.6F}, 0.3F)},
      {"Mapped", Bumpy(NormalMap("tilts.png"))},
      {"Auto", Bumpy(NormalMap("tilts.png", "auto"))},
      {"Unscaled", Bumpy(unscaled)},
      {"DirectX", Bumpy(directx)},
      {"Constant", constant},
      {"AbsentSet", absent},
      {"SphereMapped", Bumpy(NormalMap("tilts.png", "raw", Lotus::TextureWrap::Repeat))},
      {"SphereGlossy", glossy},
  };
  for (const auto& [name, material] : looks) world.SetMaterial(root + "/Looks/" + name, material);

  // Expands per-vertex, uniform or face-varying values to the corners.
  const Polygons sphere = CoarseSphere();
  const auto corners = sphere.Corners();
  const auto per_vertex = [&](const std::vector<Normal>& values) {
    std::vector<Normal> result;
    for (const auto& corner : corners) result.push_back(values[corner.vertex]);
    return result;
  };
  std::vector<Normal> points, inward, face_normals;
  for (const Point& point : sphere.points) {
    points.push_back(Fixed(point));
    inward.push_back(Fixed(Point{-point[0], -point[1], -point[2]}));
  }
  for (const auto& face : sphere.faces) {
    Point centre{0.0, 0.0, 0.0};
    for (const auto vertex : face)
      for (int k = 0; k < 3; ++k) centre[k] += sphere.points[vertex][k];
    const double length =
        std::sqrt(centre[0] * centre[0] + centre[1] * centre[1] + centre[2] * centre[2]);
    face_normals.push_back(Fixed(Point{centre[0] / length, centre[1] / length,
        centre[2] / length}));
  }
  std::vector<Normal> uniform;
  for (const auto& corner : corners) uniform.push_back(face_normals[corner.face]);
  const auto latlong = LatLong({4.0, 2.0});
  std::vector<std::array<float, 2>> mapped_st;
  for (const auto& corner : corners) mapped_st.push_back(latlong[corner.face_vertex]);

  const auto sphere_mesh = [&](std::vector<Normal> normals, bool mapped) {
    Lotus::MeshGeometry mesh = sphere.Geometry();
    mesh.normals = std::move(normals);
    if (mapped) mesh.texcoords["st"] = mapped_st;
    return mesh;
  };
  const std::vector<Normal> smooth = per_vertex(sphere.SmoothNormals());
  const Polygons reversed = CoarseSphere(true);
  Lotus::MeshGeometry left_handed = reversed.Geometry();
  const auto reversed_smooth = reversed.SmoothNormals();
  for (const auto& corner : reversed.Corners())
    left_handed.normals.push_back(reversed_smooth[corner.vertex]);
  const auto card = [](const Face& st) { return Card({{"st", Corners(st)}}); };

  struct Entry {
    const char* name;
    const char* material;
    Lotus::MeshGeometry geometry;
    std::array<float, 3> scale;
  };
  constexpr std::array<float, 3> kSphere{0.35F, 0.35F, 0.35F};
  constexpr std::array<float, 3> kCard{0.7F, 0.7F, 0.7F};
  const std::array<Entry, 25> objects{{
      {"Faceted", "Smooth", sphere_mesh({}, false), kSphere},
      {"Vertex", "Smooth", sphere_mesh(per_vertex(points), false), kSphere},
      // Indexed by the face-vertex indices: the vertex normals again.
      {"FaceVarying", "Smooth", sphere_mesh(per_vertex(points), false), kSphere},
      {"Uniform", "Smooth", sphere_mesh(uniform, false), kSphere},
      // primvars:normals wins over the normals attribute's (0, 0, 1).
      {"Primvar", "Smooth", sphere_mesh(per_vertex(points), false), kSphere},
      {"Computed", "Smooth", sphere_mesh(smooth, false), kSphere},
      {"LeftHanded", "Smooth", left_handed, kSphere},
      // Three normals for 114 points: ignored, so catmullClark computes them.
      {"Malformed", "Smooth", sphere_mesh(smooth, false), kSphere},
      {"Bilinear", "Smooth", sphere_mesh({}, false), kSphere},
      // The shader turns them to the geometric normal's side.
      {"Inward", "Smooth", sphere_mesh(per_vertex(inward), false), kSphere},
      {"Mapped", "Mapped", card(kUnit), kCard},
      {"Auto", "Auto", card(kUnit), kCard},
      {"Unscaled", "Unscaled", card(kUnit), kCard},
      {"DirectX", "DirectX", card(kUnit), kCard},
      {"Constant", "Constant", card(kUnit), kCard},
      {"MirroredS", "Mapped", card({{{1, 0}, {0, 0}, {0, 1}, {1, 1}}}), kCard},
      {"Rotated", "Mapped", card(kTurned), kCard},
      {"MirroredTransform", "Mapped", card(kUnit), {-0.7F, 0.7F, 0.7F}},
      {"Stretched", "Mapped", card(kUnit), {0.7F, 0.35F, 0.7F}},
      {"AbsentSet", "AbsentSet", card(kUnit), kCard},
      {"SphereVertex", "SphereMapped", sphere_mesh(per_vertex(points), true), kSphere},
      {"SphereFaceted", "SphereMapped", sphere_mesh({}, true), kSphere},
      {"SphereComputed", "SphereMapped", sphere_mesh(smooth, true), kSphere},
      {"SphereGlossy", "SphereGlossy", sphere_mesh(per_vertex(points), true), kSphere},
      {"SphereConstant", "Constant", sphere_mesh(per_vertex(points), true), kSphere},
  }};
  constexpr std::array<float, 5> kColumns{-1.6F, -0.8F, 0.0F, 0.8F, 1.6F};
  constexpr std::array<float, 5> kRows{1.6F, 0.8F, 0.0F, -0.8F, -1.6F};
  for (std::size_t i = 0; i < objects.size(); ++i) {
    const std::string mesh = root + "/Objects/" + objects[i].name;
    Lotus::MeshInstance placed;
    placed.world_from_object[0] = objects[i].scale[0];
    placed.world_from_object[5] = objects[i].scale[1];
    placed.world_from_object[10] = objects[i].scale[2];
    placed.world_from_object[12] = kColumns[i % 5];
    placed.world_from_object[13] = kRows[i / 5];
    world.SetMesh(mesh, objects[i].geometry, placed);
    world.BindMaterial(mesh, root + "/Looks/" + objects[i].material);
  }
}

namespace {

// scripts/material_fixtures.py's GLOW, as stb decodes Radiance RGBE:
// mantissa * 2^(exponent - 136) in float, alpha 1. Under sRGB the adapter
// decodes each colour component in double, as for any float image Hio
// reports sRGB.
Lotus::Texture Glow(bool srgb) {
  Lotus::Texture texture;
  texture.width = texture.height = 4;
  texture.format = Lotus::TextureFormat::Rgba32Float;
  for (std::uint32_t y = 0; y < 4; ++y)
    for (std::uint32_t x = 0; x < 4; ++x) {
      const float unit = std::ldexp(1.0F, static_cast<int>(127 + y) - 136);
      std::array<double, 4> value{(240 - 50 * x) * unit, (80 + 50 * x) * unit, 160 * unit, 1.0};
      for (int c = 0; c < 4; ++c) {
        if (srgb && c < 3)
          value[c] = value[c] <= 0.04045 ? value[c] / 12.92
                                         : std::pow((value[c] + 0.055) / 1.055, 2.4);
        const auto texel = static_cast<float>(value[c]);
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&texel);
        texture.texels.insert(texture.texels.end(), bytes, bytes + sizeof(texel));
      }
    }
  return texture;
}

Lotus::Material Emissive(const std::array<float, 3>& emission) {
  Lotus::Material material;
  material.emission = emission;
  return material;
}

// A material whose emission is `lookup`, at st.
Lotus::Material Glowing(Lotus::TextureInput lookup) {
  Lotus::Material material;
  material.emission_texture = std::move(lookup);
  material.texcoords = "st";
  return material;
}

// As a colour-space error would decode the sRGB emission map.
void RawEmission(Lotus::RenderWorld& world) {
  const std::string key = "textures/palette.png|sRGB";
  Lotus::Texture texture = *world.Commit().scene->textures.at(key);
  texture.format = Lotus::TextureFormat::Rgba8Unorm;
  world.SetTexture(key, std::move(texture));
}

} // namespace

void SetMaterialEmission(Lotus::RenderWorld& world) {
  const std::string root = "/MaterialEmission";
  SetStudio(world, root);

  constexpr auto kSrgb = Lotus::TextureFormat::Rgba8Srgb;
  constexpr auto kUnorm = Lotus::TextureFormat::Rgba8Unorm;
  world.SetTexture("textures/palette.png|sRGB", Image(4, kSrgb, Palette));
  world.SetTexture("textures/palette.png|raw", Image(4, kUnorm, Palette));
  world.SetTexture("textures/palette.png|auto", Image(4, kSrgb, Palette));
  world.SetTexture("textures/ramp.png|auto", Image(4, kUnorm, Ramp));
  // Under raw and auto the float image stays linear.
  world.SetTexture("textures/glow.hdr|raw", Glow(false));
  world.SetTexture("textures/glow.hdr|auto", Glow(false));
  world.SetTexture("textures/glow.hdr|sRGB", Glow(true));

  using Lotus::TextureWrap;
  Lotus::Material metal = Metal({0.95F, 0.64F, 0.54F}, 0.25F, 1.0F);
  metal.emission = {0.6F, 0.2F, 0.1F};
  Lotus::TextureInput scale_bias = Lookup("palette.png", "raw");
  scale_bias.scale = {3.0F, 3.0F, 3.0F, 1.0F};
  scale_bias.bias = {0.2F, 0.1F, 0.0F, 0.0F};
  // The unresolved file keeps its authored path and returns its fallback.
  Lotus::TextureInput missing = Lookup("palette.png", "sRGB");
  missing.texture = "./textures/missing.png|sRGB";
  missing.fallback = {2.0F, 1.0F, 0.5F, 1.0F};
  Lotus::TextureInput inverted = Lookup("palette.png", "raw");
  inverted.scale = {-1.0F, -1.0F, -1.0F, 1.0F};
  inverted.bias = {1.0F, 1.0F, 1.0F, 0.0F};
  Lotus::Material shared = Glowing(Lookup("palette.png", "sRGB"));
  shared.base_color_texture = Lookup("palette.png", "sRGB");
  Lotus::Material unlit = Glowing(Lookup("palette.png", "sRGB"));
  unlit.base_color = {0.0F, 0.0F, 0.0F};
  unlit.ior = 1.0F;
  const std::map<std::string, Lotus::Material> looks{
      {"Warm", Emissive({1.0F, 0.6F, 0.2F})},
      {"Bright", Emissive({6.0F, 6.0F, 6.0F})},
      {"Dim", Emissive({0.05F, 0.1F, 0.2F})},
      // Authored (2, -1, 0.5).
      {"Clamped", Emissive({2.0F, 0.0F, 0.5F})},
      // Authored (inf, 1, 1): a non-finite component keeps the default.
      {"NonFinite", Lotus::Material{}},
      {"Metal", metal},
      {"Cool", Emissive({0.4F, 0.8F, 1.0F})},
      {"Srgb", Glowing(Lookup("palette.png", "sRGB"))},
      {"Raw", Glowing(Lookup("palette.png", "raw"))},
      {"Auto", Glowing(Lookup("palette.png", "auto"))},
      {"Grey", Glowing(Lookup("ramp.png", "auto"))},
      {"ScaleBias", Glowing(scale_bias)},
      {"Missing", Glowing(missing)},
      {"Inverted", Glowing(inverted)},
      {"Hdr", Glowing(Lookup("glow.hdr", "raw"))},
      {"HdrAuto", Glowing(Lookup("glow.hdr", "auto"))},
      {"HdrSrgb", Glowing(Lookup("glow.hdr", "sRGB"))},
      {"SphereTextured",
          Glowing(Lookup("palette.png", "sRGB", TextureWrap::Repeat, TextureWrap::Repeat))},
      {"Shared", shared},
      {"Unlit", unlit},
      {"Base", Dielectric({0.6F, 0.6F, 0.6F}, 0.3F)},
  };
  for (const auto& [name, material] : looks) world.SetMaterial(root + "/Looks/" + name, material);

  const Polygons sphere = CoarseSphere();
  const auto corners = sphere.Corners();
  Lotus::MeshGeometry faceted = sphere.Geometry();
  Lotus::MeshGeometry smooth = faceted;
  for (const auto& corner : corners) smooth.normals.push_back(Fixed(sphere.points[corner.vertex]));
  Lotus::MeshGeometry textured = faceted;
  const auto latlong = LatLong({4.0, 2.0});
  for (const auto& corner : corners) textured.texcoords["st"].push_back(latlong[corner.face_vertex]);
  const Lotus::MeshGeometry plain = Card({});
  const Lotus::MeshGeometry mapped = Card({{"st", Corners(kUnit)}});
  // Wound clockwise seen from the camera: (0, 3, 2), (0, 2, 1).
  Lotus::MeshGeometry back = plain;
  back.triangles = {{0, 3, 2}, {0, 2, 1}};
  // Two quads; the GeomSubset that binds the right one is not read, so the
  // whole card takes the mesh's binding.
  Lotus::MeshGeometry split;
  split.positions = {{-0.5F, -0.5F, 0.0F}, {0.0F, -0.5F, 0.0F}, {0.5F, -0.5F, 0.0F},
      {0.5F, 0.5F, 0.0F}, {0.0F, 0.5F, 0.0F}, {-0.5F, 0.5F, 0.0F}};
  split.triangles = {{0, 1, 4}, {0, 4, 5}, {1, 2, 3}, {1, 3, 4}};
  split.source_faces = {0, 0, 1, 1};

  struct Entry {
    const char* name;
    const char* material;
    const Lotus::MeshGeometry& geometry;
    std::array<float, 3> scale;
    double rotate_x;
  };
  constexpr std::array<float, 3> kSphere{0.35F, 0.35F, 0.35F};
  constexpr std::array<float, 3> kCard{0.7F, 0.7F, 0.7F};
  const std::array<Entry, 25> objects{{
      {"Warm", "Warm", plain, kCard, 0},
      {"Bright", "Bright", plain, kCard, 0},
      {"Dim", "Dim", plain, kCard, 0},
      {"Clamped", "Clamped", plain, kCard, 0},
      {"NonFinite", "NonFinite", plain, kCard, 0},
      {"Metal", "Metal", smooth, kSphere, 0},
      {"Back", "Warm", back, kCard, 0},
      {"Mirrored", "Warm", plain, {-0.7F, 0.7F, 0.7F}, 0},
      {"Stretched", "Warm", plain, {0.7F, 0.35F, 0.7F}, 0},
      {"Sphere", "Cool", faceted, kSphere, 0},
      {"Srgb", "Srgb", mapped, kCard, 0},
      {"Raw", "Raw", mapped, kCard, 0},
      {"Auto", "Auto", mapped, kCard, 0},
      {"Grey", "Grey", mapped, kCard, 0},
      {"ScaleBias", "ScaleBias", mapped, kCard, 0},
      {"Missing", "Missing", mapped, kCard, 0},
      {"Inverted", "Inverted", mapped, kCard, 0},
      {"Hdr", "Hdr", mapped, kCard, 0},
      {"HdrAuto", "HdrAuto", mapped, kCard, 0},
      {"HdrSrgb", "HdrSrgb", mapped, kCard, 0},
      {"Tilted", "Warm", plain, kCard, 60},
      {"TexturedSphere", "SphereTextured", textured, kSphere, 0},
      {"Shared", "Shared", mapped, kCard, 0},
      {"Unlit", "Unlit", mapped, kCard, 0},
      {"Subset", "Base", split, kCard, 0},
  }};
  constexpr std::array<double, 5> kColumns{-1.6, -0.8, 0.0, 0.8, 1.6};
  constexpr std::array<double, 5> kRows{1.6, 0.8, 0.0, -0.8, -1.6};
  for (std::size_t i = 0; i < objects.size(); ++i) {
    const Entry& object = objects[i];
    const std::string mesh = root + "/Objects/" + object.name;
    const Matrix4d translation{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0,
        kColumns[i % 5], kRows[i / 5], 0, 1};
    const Matrix4d scale{object.scale[0], 0, 0, 0, 0, object.scale[1], 0, 0,
        0, 0, object.scale[2], 0, 0, 0, 0, 1};
    Lotus::MeshInstance placed;
    placed.world_from_object =
        ToFloat(Multiply(translation, Multiply(RotationX(object.rotate_x), scale)));
    world.SetMesh(mesh, object.geometry, placed);
    world.BindMaterial(mesh, root + "/Looks/" + object.material);
  }
}

namespace {

// scripts/material_fixtures.py's cutout.png: a green gradient whose alpha is
// an opaque disc, a ring of 96 and transparent corners.
Texel Cutout(std::uint32_t x, std::uint32_t y) {
  const int dx = 2 * static_cast<int>(x) - 7, dy = 2 * static_cast<int>(y) - 7;
  const int distance = dx * dx + dy * dy;
  return {std::uint8_t(60 + 20 * x), std::uint8_t(200 - 15 * y), 60,
      std::uint8_t(distance <= 26 ? 255 : distance <= 50 ? 96 : 0)};
}

Lotus::Material Covered(float opacity, float threshold = 0.0F) {
  Lotus::Material material;
  material.base_color = {0.2F, 0.6F, 0.7F};
  material.opacity = opacity;
  material.opacity_threshold = threshold;
  return material;
}

// A material whose base colour and coverage are `lookup`'s RGB and alpha,
// at st.
Lotus::Material Masked(const Lotus::TextureInput& lookup, float threshold = 0.5F) {
  Lotus::Material material = Textured(lookup);
  material.opacity_texture = Channel(lookup, 3);
  material.opacity_threshold = threshold;
  return material;
}

// As a decoder that treats alpha as colour would read the sRGB images.
void SrgbAlpha(Lotus::RenderWorld& world) {
  for (const auto& [key, texture] : world.Commit().scene->textures) {
    if (texture->format != Lotus::TextureFormat::Rgba8Srgb) continue;
    Lotus::Texture decoded = *texture;
    for (std::size_t i = 3; i < decoded.texels.size(); i += 4) {
      const double value = decoded.texels[i] / 255.0;
      const double linear =
          value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
      decoded.texels[i] = static_cast<std::uint8_t>(std::lround(linear * 255.0));
    }
    world.SetTexture(key, std::move(decoded));
  }
}

} // namespace

void SetMaterialAlpha(Lotus::RenderWorld& world) {
  const std::string root = "/MaterialAlpha";
  SetStudio(world, root);

  constexpr auto kSrgb = Lotus::TextureFormat::Rgba8Srgb;
  constexpr auto kUnorm = Lotus::TextureFormat::Rgba8Unorm;
  // Under auto the 8-bit two-channel image stays linear.
  world.SetTexture("textures/rgba.png|sRGB", Image(4, kSrgb, Rgba));
  world.SetTexture("textures/cutout.png|sRGB", Image(8, kSrgb, Cutout));
  world.SetTexture("textures/grey-alpha.png|auto", Image(4, kUnorm, GreyAlpha));
  world.SetTexture("textures/ramp.png|raw", Image(4, kUnorm, Ramp));
  world.SetTexture("textures/palette.png|sRGB", Image(4, kSrgb, Palette));
  world.SetTexture("textures/checker.png|raw", Image(64, kUnorm, Checker));

  using Lotus::TextureWrap;
  Lotus::Material ramp = Covered(1.0F, 0.5F);
  ramp.opacity_texture = Channel(Lookup("ramp.png", "raw"), 0);
  ramp.texcoords = "st";
  Lotus::TextureInput inverse = Channel(Lookup("cutout.png", "sRGB"), 3);
  inverse.scale = {1.0F, 1.0F, 1.0F, -1.0F};
  inverse.bias = {0.0F, 0.0F, 0.0F, 1.0F};
  Lotus::Material inverted = Covered(1.0F, 0.5F);
  inverted.opacity_texture = inverse;
  inverted.texcoords = "st";
  // The unresolved file keeps its authored path and returns its fallback.
  Lotus::TextureInput missing = Lookup("palette.png", "sRGB");
  missing.texture = "./textures/missing.png|sRGB";
  missing.fallback = {0.9F, 0.2F, 0.6F, 0.3F};
  Lotus::Material minified = Covered(1.0F, 0.5F);
  minified.opacity_texture =
      Channel(Lookup("checker.png", "raw", TextureWrap::Repeat, TextureWrap::Repeat), 0);
  minified.texcoords = "st";
  Lotus::Material glowing_mask = Masked(Lookup("cutout.png", "sRGB"));
  glowing_mask.base_color_texture.reset();
  glowing_mask.emission = {1.0F, 0.6F, 0.2F};
  Lotus::Material glowing_half = Emissive({1.0F, 0.6F, 0.2F});
  glowing_half.opacity = 0.5F;
  const std::map<std::string, Lotus::Material> looks{
      {"Half", Covered(0.5F)},
      {"Quarter", Covered(0.25F)},
      {"Zero", Covered(0.0F)},
      // opacityMode is not read: presence as for every material.
      {"Presence", Covered(0.5F)},
      // Authored opacity 1.5 and threshold 2: equal once clamped.
      {"Clamped", Covered(1.0F, 1.0F)},
      {"Below", Covered(0.4F, 0.5F)},
      {"Equal", Covered(0.5F, 0.5F)},
      {"Above", Covered(0.6F, 0.5F)},
      // Authored -inf and inf, which keep the defaults.
      {"NonFinite", Covered(1.0F)},
      // The threshold's texture connection is not translated.
      {"ThresholdMap", Covered(0.5F)},
      {"Alpha", Masked(Lookup("rgba.png", "sRGB"))},
      {"Cutout", Masked(Lookup("cutout.png", "sRGB"))},
      {"GreyAlpha", Masked(Lookup("grey-alpha.png", "auto"))},
      {"Ramp", ramp},
      {"NoAlpha", Masked(Lookup("palette.png", "sRGB"))},
      {"Fractional", Masked(Lookup("cutout.png", "sRGB"), 0.0F)},
      {"Inverted", inverted},
      {"BlackBorder",
          Masked(Lookup("palette.png", "sRGB", TextureWrap::Black, TextureWrap::Black))},
      {"Missing", Masked(missing, 0.0F)},
      {"Minified", minified},
      {"SphereCutout",
          Masked(Lookup("cutout.png", "sRGB", TextureWrap::Repeat, TextureWrap::Repeat))},
      {"GlowingMask", glowing_mask},
      {"GlowingHalf", glowing_half},
  };
  for (const auto& [name, material] : looks) world.SetMaterial(root + "/Looks/" + name, material);

  const Polygons sphere = CoarseSphere();
  const auto latlong = LatLong({4.0, 2.0});
  Lotus::MeshGeometry masked_sphere = sphere.Geometry();
  for (const auto& corner : sphere.Corners()) {
    masked_sphere.normals.push_back(Fixed(sphere.points[corner.vertex]));
    masked_sphere.texcoords["st"].push_back(latlong[corner.face_vertex]);
  }
  const Lotus::MeshGeometry plain = Card({});
  const Lotus::MeshGeometry mapped = Card({{"st", Corners(kUnit)}});
  const Lotus::MeshGeometry wide = Card({{"st", Corners(kWide)}});
  Lotus::MeshGeometry stacked;
  stacked.positions = {{-0.5F, -0.5F, 0.0F}, {0.5F, -0.5F, 0.0F}, {0.5F, 0.5F, 0.0F},
      {-0.5F, 0.5F, 0.0F}, {-0.15F, -0.15F, 0.4F}, {0.45F, -0.15F, 0.4F},
      {0.45F, 0.45F, 0.4F}, {-0.15F, 0.45F, 0.4F}};
  stacked.triangles = {{0, 1, 2}, {0, 2, 3}, {4, 5, 6}, {4, 6, 7}};
  stacked.source_faces = {0, 0, 1, 1};

  struct Entry {
    const char* name;
    // Unbound when null.
    const char* material;
    const Lotus::MeshGeometry& geometry;
    float scale;
  };
  constexpr float kSphere = 0.35F;
  constexpr float kCard = 0.7F;
  const std::array<Entry, 25> objects{{
      {"Half", "Half", plain, kCard},
      {"Quarter", "Quarter", plain, kCard},
      {"Zero", "Zero", plain, kCard},
      {"Presence", "Presence", plain, kCard},
      {"Clamped", "Clamped", plain, kCard},
      {"Below", "Below", plain, kCard},
      {"Equal", "Equal", plain, kCard},
      {"Above", "Above", plain, kCard},
      {"NonFinite", "NonFinite", plain, kCard},
      {"ThresholdMap", "ThresholdMap", mapped, kCard},
      {"Alpha", "Alpha", mapped, kCard},
      {"Cutout", "Cutout", mapped, kCard},
      {"GreyAlpha", "GreyAlpha", mapped, kCard},
      {"Ramp", "Ramp", mapped, kCard},
      {"NoAlpha", "NoAlpha", mapped, kCard},
      {"Fractional", "Fractional", mapped, kCard},
      {"Inverted", "Inverted", mapped, kCard},
      {"BlackBorder", "BlackBorder", wide, kCard},
      {"Missing", "Missing", mapped, kCard},
      {"Minified", "Minified", mapped, kCard},
      {"Stacked", "Half", stacked, kCard},
      {"SphereCutout", "SphereCutout", masked_sphere, kSphere},
      {"GlowingMask", "GlowingMask", mapped, kCard},
      {"GlowingHalf", "GlowingHalf", plain, kCard},
      // displayOpacity 0.3 is not read.
      {"DisplayOpacity", nullptr, plain, kCard},
  }};
  constexpr std::array<float, 5> kColumns{-1.6F, -0.8F, 0.0F, 0.8F, 1.6F};
  constexpr std::array<float, 5> kRows{1.6F, 0.8F, 0.0F, -0.8F, -1.6F};
  for (std::size_t i = 0; i < objects.size(); ++i) {
    const Entry& object = objects[i];
    const std::string mesh = root + "/Objects/" + object.name;
    Lotus::MeshInstance placed;
    placed.world_from_object[0] = placed.world_from_object[5] =
        placed.world_from_object[10] = object.scale;
    placed.world_from_object[12] = kColumns[i % 5];
    placed.world_from_object[13] = kRows[i / 5];
    world.SetMesh(mesh, object.geometry, placed);
    if (object.material != nullptr) {
      world.BindMaterial(mesh, root + "/Looks/" + object.material);
    } else {
      const std::string display = mesh + ".lotus:displayColor";
      world.SetMaterial(display, Dielectric({0.2F, 0.6F, 0.7F}));
      world.BindMaterial(mesh, display);
    }
  }
}

namespace {

// A Standard Surface's material: the IR with Standard Surface's base colour
// and roughness defaults, then `base` and `roughness` where given.
Lotus::Material Standard(const std::array<float, 3>& base = {0.8F, 0.8F, 0.8F},
    float roughness = 0.2F) {
  return Dielectric(base, roughness);
}

// A MaterialX image lookup: its address modes default to periodic.
Lotus::TextureInput Periodic(const std::string& file, const std::string& space) {
  return Lookup(file, space, Lotus::TextureWrap::Repeat, Lotus::TextureWrap::Repeat);
}

// A Standard Surface whose base_color is `lookup`, at `texcoords`.
Lotus::Material StandardTextured(Lotus::TextureInput lookup, std::string texcoords = "st") {
  Lotus::Material material = Standard();
  material.base_color_texture = std::move(lookup);
  material.texcoords = std::move(texcoords);
  return material;
}

// An ND_normalmap of tilts.png with scale `scale`.
Lotus::Material StandardBumpy(float scale) {
  Lotus::Material material = Standard({0.7F, 0.7F, 0.7F}, 0.4F);
  Lotus::TextureInput lookup = Periodic("tilts.png", "raw");
  lookup.scale = {2.0F * scale, 2.0F * scale, 2.0F, 1.0F};
  lookup.bias = {-scale, -scale, -1.0F, 0.0F};
  lookup.fallback = {-scale, -scale, -1.0F, 1.0F};
  material.normal_texture = lookup;
  material.texcoords = "st";
  return material;
}

// As a translator that ignored the base weight would read the image.
void UnweightedImage(Lotus::RenderWorld& world) {
  const std::string key = "/MaterialMtlx/Looks/WeightedImage";
  Lotus::Material material = world.Commit().scene->materials.at(key);
  material.base_color_texture->scale = {1.0F, 1.0F, 1.0F, 1.0F};
  world.SetMaterial(key, material);
}

} // namespace

void SetMaterialMtlx(Lotus::RenderWorld& world) {
  const std::string root = "/MaterialMtlx";
  SetStudio(world, root);

  // MaterialX images are raw unless their colour space is an sRGB one.
  constexpr auto kSrgb = Lotus::TextureFormat::Rgba8Srgb;
  constexpr auto kUnorm = Lotus::TextureFormat::Rgba8Unorm;
  world.SetTexture("textures/palette.png|sRGB", Image(4, kSrgb, Palette));
  world.SetTexture("textures/palette.png|raw", Image(4, kUnorm, Palette));
  world.SetTexture("textures/rgba.png|raw", Image(4, kUnorm, Rgba));
  world.SetTexture("textures/ramp.png|raw", Image(4, kUnorm, Ramp));
  world.SetTexture("textures/tilts.png|raw", Image(4, kUnorm, Tilts));

  using Lotus::TextureWrap;
  constexpr std::array<float, 3> kTeal{0.2F, 0.6F, 0.7F};
  constexpr std::array<float, 3> kCopper{0.95F, 0.64F, 0.54F};
  constexpr std::array<float, 3> kBlack{0.0F, 0.0F, 0.0F};
  Lotus::Material metal = Standard(kCopper, 0.3F);
  metal.metallic = 1.0F;
  Lotus::Material ior = Standard({0.1F, 0.2F, 0.6F}, 0.1F);
  ior.ior = 2.5F;
  Lotus::Material no_specular = Standard({0.6F, 0.6F, 0.6F}, 0.1F);
  no_specular.ior = 1.0F;
  // emission x emission_color, in float.
  Lotus::Material emissive = Standard(kBlack);
  emissive.emission = {2.0F * 1.0F, 2.0F * 0.6F, 2.0F * 0.2F};
  Lotus::Material opacity = Standard(kTeal);
  opacity.opacity = 0.5F;
  // base 1.5 x (1, 0.5, -0.25), metalness -1 and roughness 1.5, clamped.
  Lotus::Material clamped = Standard({1.0F, 1.5F * 0.5F, 0.0F}, 1.0F);
  Lotus::TextureInput weighted = Periodic("palette.png", "sRGB");
  weighted.scale = {0.5F, 0.5F, 0.5F, 1.0F};
  weighted.fallback = {0.0F, 0.0F, 0.0F, 1.0F};
  Lotus::Material channels = Standard(kCopper);
  channels.metallic_texture = Channel(Periodic("rgba.png", "raw"), 2);
  channels.roughness_texture = Periodic("ramp.png", "raw");
  channels.texcoords = "st";
  Lotus::Material emission_map = Standard(kBlack);
  emission_map.emission_texture = Periodic("palette.png", "raw");
  emission_map.texcoords = "st";
  const std::map<std::string, Lotus::Material> looks{
      {"Defaults", Standard()},
      {"Weighted", Standard({0.5F * 0.9F, 0.5F * 0.3F, 0.5F * 0.1F})},
      {"Metal", metal},
      {"Ior", ior},
      {"NoSpecular", no_specular},
      {"Emissive", emissive},
      {"Opacity", opacity},
      {"Clamped", clamped},
      // coat, sheen and subsurface are not represented.
      {"Lobes", Standard(kTeal)},
      // The MaterialX surface wins over the red UsdPreviewSurface.
      {"Precedence", Standard({0.1F, 0.2F, 0.8F})},
      {"Srgb", StandardTextured(Periodic("palette.png", "sRGB"))},
      {"Raw", StandardTextured(Periodic("palette.png", "raw"))},
      {"WeightedImage", StandardTextured(weighted)},
      {"Channels", channels},
      {"EmissionMap", emission_map},
      {"Clamp", StandardTextured(Lookup("palette.png", "sRGB"))},
      {"Mirror", StandardTextured(Lookup("palette.png", "sRGB", TextureWrap::Mirror,
                     TextureWrap::Mirror))},
      {"Constant", StandardTextured(Lookup("palette.png", "sRGB", TextureWrap::Black,
                       TextureWrap::Black))},
      {"Named", StandardTextured(Periodic("palette.png", "sRGB"), "uv1")},
      {"NormalMap", StandardBumpy(1.0F)},
      {"NormalScale", StandardBumpy(0.5F)},
      {"Tiled", StandardTextured(Periodic("palette.png", "sRGB"))},
      // An image without a file is its default.
      {"Fileless", Standard({0.2F, 0.6F, 0.9F})},
      // ND_multiply is not read: base_color keeps its default.
      {"Untranslated", Standard()},
  };
  for (const auto& [name, material] : looks) world.SetMaterial(root + "/Looks/" + name, material);

  const Polygons sphere = CoarseSphere();
  const auto corners = sphere.Corners();
  Lotus::MeshGeometry smooth = sphere.Geometry();
  for (const auto& corner : corners) smooth.normals.push_back(Fixed(sphere.points[corner.vertex]));
  Lotus::MeshGeometry bumpy = smooth;
  const auto latlong = LatLong({4.0, 2.0});
  for (const auto& corner : corners) bumpy.texcoords["st"].push_back(latlong[corner.face_vertex]);
  const Lotus::MeshGeometry plain = Card({});
  const Lotus::MeshGeometry mapped = Card({{"st", Corners(kUnit)}});
  const Lotus::MeshGeometry wide = Card({{"st", Corners(kWide)}});
  const Lotus::MeshGeometry named =
      Card({{"st", Corners(kUnit)}, {"uv1", Corners(kTurned)}});

  struct Entry {
    const char* name;
    const char* material;
    const Lotus::MeshGeometry& geometry;
    float scale;
  };
  constexpr float kSphere = 0.35F;
  constexpr float kCard = 0.7F;
  const std::array<Entry, 25> objects{{
      {"Defaults", "Defaults", smooth, kSphere},
      {"Weighted", "Weighted", smooth, kSphere},
      {"Metal", "Metal", smooth, kSphere},
      {"Ior", "Ior", smooth, kSphere},
      {"NoSpecular", "NoSpecular", smooth, kSphere},
      {"Emissive", "Emissive", plain, kCard},
      {"Opacity", "Opacity", plain, kCard},
      {"Clamped", "Clamped", plain, kCard},
      {"Lobes", "Lobes", plain, kCard},
      {"Precedence", "Precedence", plain, kCard},
      {"Srgb", "Srgb", mapped, kCard},
      {"Raw", "Raw", mapped, kCard},
      {"WeightedImage", "WeightedImage", mapped, kCard},
      {"Channels", "Channels", mapped, kCard},
      {"EmissionMap", "EmissionMap", mapped, kCard},
      {"Periodic", "Srgb", wide, kCard},
      {"Clamp", "Clamp", wide, kCard},
      {"Mirror", "Mirror", wide, kCard},
      {"Constant", "Constant", wide, kCard},
      {"Named", "Named", named, kCard},
      {"NormalMap", "NormalMap", bumpy, kSphere},
      {"NormalScale", "NormalScale", mapped, kCard},
      {"Tiled", "Tiled", mapped, kCard},
      {"Fileless", "Fileless", plain, kCard},
      {"Untranslated", "Untranslated", mapped, kCard},
  }};
  constexpr std::array<float, 5> kColumns{-1.6F, -0.8F, 0.0F, 0.8F, 1.6F};
  constexpr std::array<float, 5> kRows{1.6F, 0.8F, 0.0F, -0.8F, -1.6F};
  for (std::size_t i = 0; i < objects.size(); ++i) {
    const Entry& object = objects[i];
    const std::string mesh = root + "/Objects/" + object.name;
    Lotus::MeshInstance placed;
    placed.world_from_object[0] = placed.world_from_object[5] =
        placed.world_from_object[10] = object.scale;
    placed.world_from_object[12] = kColumns[i % 5];
    placed.world_from_object[13] = kRows[i / 5];
    world.SetMesh(mesh, object.geometry, placed);
    world.BindMaterial(mesh, root + "/Looks/" + object.material);
  }
}

namespace {

// A Standard Surface transmitting with `roughness`.
Lotus::Material Glass(float roughness, const std::array<float, 3>& tint = {1.0F, 1.0F, 1.0F},
    bool thin_walled = false) {
  Lotus::Material material = Standard({0.8F, 0.8F, 0.8F}, roughness);
  material.transmission = 1.0F;
  material.transmission_color = tint;
  material.thin_walled = thin_walled;
  return material;
}

// As a translator that ignored transmission_color would read it.
void UntintedGlass(Lotus::RenderWorld& world) {
  const std::string key = "/MaterialTransmission/Looks/Tinted";
  Lotus::Material material = world.Commit().scene->materials.at(key);
  material.transmission_color = {1.0F, 1.0F, 1.0F};
  world.SetMaterial(key, material);
}

} // namespace

void SetMaterialTransmission(Lotus::RenderWorld& world) {
  const std::string root = "/MaterialTransmission";
  SetStudio(world, root);

  constexpr std::array<float, 3> kTeal{0.2F, 0.6F, 0.7F};
  constexpr std::array<float, 3> kAmber{0.9F, 0.45F, 0.15F};
  Lotus::Material matched = Glass(0.4F, {0.5F, 0.8F, 0.9F});
  matched.ior = 1.0F;
  Lotus::Material partial = Standard(kTeal, 0.15F);
  partial.transmission = 0.5F;
  partial.metallic = 0.25F;
  Lotus::Material thin_matched = Glass(0.2F, {0.9F, 0.6F, 0.3F}, true);
  thin_matched.ior = 1.0F;
  Lotus::Material thin_dense = Glass(0.0F, {1.0F, 1.0F, 1.0F}, true);
  thin_dense.ior = 2.42F;
  const std::map<std::string, Lotus::Material> looks{
      {"Smooth", Glass(0.0F)},
      {"Rough", Glass(0.35F)},
      {"Tinted", Glass(0.0F, kAmber)},
      {"Matched", matched},
      {"Partial", partial},
      {"ThinSmooth", Glass(0.0F, {1.0F, 1.0F, 1.0F}, true)},
      {"ThinRough", Glass(0.4F, {1.0F, 1.0F, 1.0F}, true)},
      {"ThinTinted", Glass(0.1F, {0.3F, 0.7F, 0.9F}, true)},
      {"ThinMatched", thin_matched},
      {"ThinDense", thin_dense},
      // A positive depth leaves the transmitted light untinted.
      {"Depth", Glass(0.0F)},
      // specular 0 keeps specular_IOR to refract with.
      {"NoSpecular", Glass(0.2F)},
      // The medium's scattering and dispersion are ignored.
      {"Ignored", Glass(0.1F)},
      // A graph-driven transmission keeps its default, 0.
      {"Graph", Standard(kTeal, 0.3F)},
  };
  for (const auto& [name, material] : looks) world.SetMaterial(root + "/Looks/" + name, material);

  const Polygons sphere = CoarseSphere();
  Lotus::MeshGeometry smooth = sphere.Geometry();
  for (const auto& corner : sphere.Corners())
    smooth.normals.push_back(Fixed(sphere.points[corner.vertex]));
  const Lotus::MeshGeometry plain = Card({});

  struct Entry {
    const char* name;
    const char* material;
    const Lotus::MeshGeometry& geometry;
    float scale;
    // A mirroring transform: x scaled by -scale.
    bool mirrored = false;
  };
  constexpr float kSphere = 0.35F;
  constexpr float kCard = 0.7F;
  const std::array<Entry, 15> objects{{
      {"Smooth", "Smooth", smooth, kSphere},
      {"Rough", "Rough", smooth, kSphere},
      {"Tinted", "Tinted", smooth, kSphere},
      {"Matched", "Matched", smooth, kSphere},
      {"Partial", "Partial", smooth, kSphere},
      {"ThinSmooth", "ThinSmooth", plain, kCard},
      {"ThinRough", "ThinRough", plain, kCard},
      {"ThinTinted", "ThinTinted", plain, kCard},
      {"ThinMatched", "ThinMatched", plain, kCard},
      {"ThinDense", "ThinDense", plain, kCard},
      {"Mirrored", "Smooth", smooth, kSphere, true},
      {"Depth", "Depth", smooth, kSphere},
      {"NoSpecular", "NoSpecular", smooth, kSphere},
      {"Ignored", "Ignored", smooth, kSphere},
      {"Graph", "Graph", smooth, kSphere},
  }};
  constexpr std::array<float, 5> kColumns{-1.6F, -0.8F, 0.0F, 0.8F, 1.6F};
  constexpr std::array<float, 3> kRows{0.8F, 0.0F, -0.8F};
  for (std::size_t i = 0; i < objects.size(); ++i) {
    const Entry& object = objects[i];
    const std::string mesh = root + "/Objects/" + object.name;
    Lotus::MeshInstance placed;
    placed.world_from_object[0] = object.mirrored ? -object.scale : object.scale;
    placed.world_from_object[5] = placed.world_from_object[10] = object.scale;
    placed.world_from_object[12] = kColumns[i % 5];
    placed.world_from_object[13] = kRows[i / 5];
    world.SetMesh(mesh, object.geometry, placed);
    world.BindMaterial(mesh, root + "/Looks/" + object.material);
  }
}

std::span<const MaterialFixture> MaterialFixtures() {
  static constexpr MaterialFixture kFixtures[] = {
      {"material-basics-v1", "/MaterialBasics", 128, SetMaterialBasics,
          "the half-rough metal 10% darker", DarkerMetal},
      {"material-textures-v1", "/MaterialTextures", 160, SetMaterialTextures,
          "the repeating card half a texel off", ShiftedRepeat},
      {"material-normals-v1", "/MaterialNormals", 160, SetMaterialNormals,
          "the normal map's green read flipped", FlippedGreen},
      {"material-emission-v1", "/MaterialEmission", 160, SetMaterialEmission,
          "the sRGB emission map decoded as raw", RawEmission},
      {"material-alpha-v1", "/MaterialAlpha", 160, SetMaterialAlpha,
          "the sRGB images' alpha decoded as sRGB", SrgbAlpha},
      {"material-mtlx-v1", "/MaterialMtlx", 160, SetMaterialMtlx,
          "the weighted image's base weight ignored", UnweightedImage},
      {"material-transmission-v1", "/MaterialTransmission", 160, SetMaterialTransmission,
          "the tinted sphere's transmission colour ignored", UntintedGlass},

  };
  return kFixtures;
}

} // namespace LotusHeadless
