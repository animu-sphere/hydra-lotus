// SPDX-License-Identifier: Apache-2.0
#include <lotus/extraction.hpp>

#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template <typename F> void Reject(F operation) {
  bool rejected = false;
  try { operation(); }
  catch (const std::invalid_argument&) { rejected = true; }
  Check(rejected, "invalid geometry or instance was accepted");
}

} // namespace

int main() try {
  Lotus::RenderWorld world;
  const auto empty = world.Commit();
  Lotus::MeshGeometry geometry{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}},
      {{0, 1, 2}}, {4}};
  Lotus::MeshInstance instance;
  world.SetMesh("/mesh", geometry, instance);
  const auto first = world.Commit();
  const auto original_geometry = first.scene->meshes.at("/mesh").geometry;
  Check(empty.scene->meshes.empty() && first.revision == 1 &&
      first.triangle_count == 1 && *original_geometry == geometry,
      "mesh insertion changed a retained empty snapshot");

  world.SetMesh("/mesh", geometry, instance);
  auto stable = world.Commit();
  Check(stable.revision == first.revision && stable.scene == first.scene,
      "unchanged geometry copied or invalidated the scene");
  instance.world_from_object[12] = 7;
  world.SetMeshInstance("/mesh", instance);
  const auto moved = world.Commit();
  Check(moved.revision == 2 && moved.scene != first.scene &&
      moved.scene->meshes.at("/mesh").geometry == original_geometry &&
      first.scene->meshes.at("/mesh").instance.world_from_object[12] == 0,
      "transform edit copied geometry or changed a retained snapshot");

  instance.visible = false;
  world.SetMeshInstance("/mesh", instance);
  const auto hidden = world.Commit();
  Check(hidden.triangle_count == 0 &&
      Lotus::ExtractDrawSummary(hidden).draw_count == 0 &&
      hidden.scene->meshes.at("/mesh").geometry == original_geometry,
      "hidden mesh lost geometry or remained drawable");
  instance.visible = true;
  geometry.positions[0][2] = 2;
  world.SetMesh("/mesh", geometry, instance);
  const auto edited = world.Commit();
  Check(edited.scene->meshes.at("/mesh").geometry != original_geometry &&
      original_geometry->positions[0][2] == 0,
      "point edit changed a retained geometry buffer");

  auto invalid = geometry;
  invalid.triangles[0][2] = 3;
  Reject([&] { world.SetMesh("/mesh", invalid, instance); });
  invalid = geometry;
  invalid.positions[0][0] = std::numeric_limits<float>::quiet_NaN();
  Reject([&] { world.SetMesh("/mesh", invalid, instance); });
  invalid = geometry;
  invalid.source_faces.clear();
  Reject([&] { world.SetMesh("/mesh", invalid, instance); });
  invalid = geometry;
  invalid.normals = {{0, 0, 1}, {0, 0, 1}};
  Reject([&] { world.SetMesh("/mesh", invalid, instance); });
  invalid.normals = {{0, 0, 1}, {0, 0, 1},
      {0, std::numeric_limits<float>::infinity(), 1}};
  Reject([&] { world.SetMesh("/mesh", invalid, instance); });
  Reject([&] { world.SetMesh("", geometry, instance); });
  auto invalid_instance = instance;
  invalid_instance.world_from_object[0] = std::numeric_limits<float>::infinity();
  Reject([&] { world.SetMeshInstance("/mesh", invalid_instance); });
  Reject([&] { world.SetMeshInstance("/missing", instance); });
  Lotus::Material material;
  Check(edited.scene->meshes.at("/mesh").material.empty() &&
      edited.scene->materials.empty() &&
      edited.scene->environment == std::array<float, 3>{},
      "a new mesh or scene did not start unbound, without materials and "
      "with a black environment");
  auto invalid_material = material;
  invalid_material.base_color[1] = 1.5F;
  Reject([&] { world.SetMaterial("/paint", invalid_material); });
  invalid_material = material;
  invalid_material.roughness = -0.25F;
  Reject([&] { world.SetMaterial("/paint", invalid_material); });
  invalid_material = material;
  invalid_material.metallic = std::numeric_limits<float>::quiet_NaN();
  Reject([&] { world.SetMaterial("/paint", invalid_material); });
  invalid_material = material;
  invalid_material.emission[2] = -1.0F;
  Reject([&] { world.SetMaterial("/paint", invalid_material); });
  Reject([&] { world.SetMaterial("", material); });
  Reject([&] { world.BindMaterial("/missing", "/paint"); });
  Reject([&] { world.SetEnvironment({1.0F, -1.0F, 0.0F}); });
  Reject([&] {
    world.SetEnvironment({std::numeric_limits<float>::infinity(), 0.0F, 0.0F});
  });
  world.BindMaterial("/mesh", "");
  world.RemoveMaterial("/missing");
  world.SetEnvironment({});
  Check(world.Commit().revision == edited.revision,
      "rejected input or unchanged materials changed the scene revision");

  // A binding may name a key before its material exists.
  world.BindMaterial("/mesh", "/paint");
  const auto bound = world.Commit();
  Check(bound.revision == edited.revision + 1 &&
      bound.scene->meshes.at("/mesh").geometry ==
          edited.scene->meshes.at("/mesh").geometry &&
      bound.scene->meshes.at("/mesh").material == "/paint" &&
      edited.scene->meshes.at("/mesh").material.empty(),
      "a material binding copied geometry or changed a retained snapshot");
  material.base_color = {0.5F, 0.25F, 1.0F};
  material.emission = {2.0F, 0.0F, 0.0F};
  world.SetMaterial("/paint", material);
  const auto painted = world.Commit();
  Check(painted.revision == bound.revision + 1 &&
      painted.scene->materials.at("/paint") == material &&
      bound.scene->materials.empty(),
      "a material insertion was lost or changed a retained snapshot");
  world.SetMaterial("/paint", material);
  Check(world.Commit().revision == painted.revision,
      "an unchanged material changed the scene revision");
  geometry.positions[1][2] = 3;
  world.SetMesh("/mesh", geometry, instance);
  Check(world.Commit().scene->meshes.at("/mesh").material == "/paint",
      "replacing a mesh's geometry lost its material binding");
  world.RemoveMaterial("/paint");
  const auto unpainted = world.Commit();
  Check(unpainted.scene->materials.empty() &&
      unpainted.scene->meshes.at("/mesh").material == "/paint" &&
      painted.scene->materials.at("/paint") == material,
      "removing a material dropped its binding or changed a retained "
      "snapshot");
  world.SetMaterial("/paint", material);
  world.SetEnvironment({0.25F, 0.5F, 4.0F});
  const auto lit = world.Commit();
  Check(lit.scene->environment == std::array<float, 3>{0.25F, 0.5F, 4.0F} &&
      painted.scene->environment == std::array<float, 3>{},
      "an environment edit was lost or changed a retained snapshot");

  // An instancer prototype is placed once per instancer transform, applied
  // after its own transform; an empty list places it nowhere.
  auto prototype = instance;
  prototype.world_from_object = Lotus::IdentityMatrix();
  prototype.world_from_object[0] = 2;
  prototype.world_from_object[12] = 1;
  Lotus::Matrix4 shifted = Lotus::IdentityMatrix();
  shifted[13] = 5;
  Lotus::Matrix4 turned{0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  prototype.instancer_transforms = std::vector<Lotus::Matrix4>{shifted, turned};
  world.SetMeshInstance("/mesh", prototype);
  const auto instanced = world.Commit();
  const auto placements = Lotus::PlacementTransforms(
      instanced.scene->meshes.at("/mesh").instance);
  Check(instanced.revision == lit.revision + 1 && instanced.triangle_count == 2 &&
      placements.size() == 2 &&
      placements[0] == Lotus::Matrix4{2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0,
          1, 5, 0, 1} &&
      placements[1] == Lotus::Matrix4{0, 2, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0,
          0, 1, 0, 1} &&
      Lotus::PlacementTransforms(instance) ==
          std::vector<Lotus::Matrix4>{instance.world_from_object},
      "instancer placements were not composed after the mesh transform");
  prototype.instancer_transforms->clear();
  world.SetMeshInstance("/mesh", prototype);
  Check(world.Commit().triangle_count == 0,
      "an instancer prototype without instances was placed");
  invalid_instance = prototype;
  invalid_instance.instancer_transforms->push_back(shifted);
  invalid_instance.instancer_transforms->back()[3] =
      std::numeric_limits<float>::quiet_NaN();
  Reject([&] { world.SetMeshInstance("/mesh", invalid_instance); });
  world.SetMeshInstance("/mesh", instance);
  Check(world.Commit().triangle_count == 1 &&
      instanced.scene->meshes.at("/mesh").instance.instancer_transforms->size() == 2,
      "an ordinary placement was not restored or changed a retained snapshot");

  // Normals are geometry: adding or changing them replaces the buffer, and
  // a zero normal is valid.
  geometry.normals = {{0, 0, 1}, {0.5F, 0, 2}, {0, 0, 0}};
  const auto plain = world.Commit();
  world.SetMesh("/mesh", geometry, instance);
  const auto normal = world.Commit();
  Check(normal.revision == plain.revision + 1 &&
      normal.scene->meshes.at("/mesh").geometry->normals == geometry.normals &&
      plain.scene->meshes.at("/mesh").geometry->normals.empty() &&
      normal.scene->meshes.at("/mesh").material ==
          plain.scene->meshes.at("/mesh").material,
      "adding normals was lost or changed a retained geometry buffer");
  world.SetMesh("/mesh", geometry, instance);
  Check(world.Commit().scene == normal.scene,
      "unchanged normals replaced the geometry");
  geometry.normals.clear();
  world.SetMesh("/mesh", geometry, instance);
  Check(world.Commit().scene->meshes.at("/mesh").geometry->normals.empty(),
      "removing normals was lost");

  // Texture-coordinate sets are geometry too, one (s, t) per corner.
  invalid = geometry;
  invalid.texcoords["st"] = {{{0, 0}}, {{1, 0}}};
  Reject([&] { world.SetMesh("/mesh", invalid, instance); });
  invalid.texcoords["st"] = {{{0, 0}}, {{1, 0}},
      {{0, std::numeric_limits<float>::quiet_NaN()}}};
  Reject([&] { world.SetMesh("/mesh", invalid, instance); });
  invalid.texcoords = {{"", {{{0, 0}}, {{1, 0}}, {{0, 1}}}}};
  Reject([&] { world.SetMesh("/mesh", invalid, instance); });
  const auto untextured = world.Commit();
  geometry.texcoords["st"] = {{{0, 0}}, {{1, 0}}, {{0, -2}}};
  world.SetMesh("/mesh", geometry, instance);
  const auto uv = world.Commit();
  Check(uv.revision == untextured.revision + 1 &&
      uv.scene->meshes.at("/mesh").geometry->texcoords == geometry.texcoords &&
      untextured.scene->meshes.at("/mesh").geometry->texcoords.empty(),
      "adding texture coordinates was lost or changed a retained geometry "
      "buffer");

  // Textures are keyed like materials, and a texture input names one by
  // key, which need not hold a texture yet.
  Lotus::Texture texture;
  texture.width = 2;
  texture.height = 1;
  texture.format = Lotus::TextureFormat::Rgba32Float;
  texture.texels.resize(2 * 16);
  auto invalid_texture = texture;
  invalid_texture.texels.pop_back();
  Reject([&] { world.SetTexture("/tex", invalid_texture); });
  invalid_texture = texture;
  invalid_texture.width = 0;
  invalid_texture.texels.clear();
  Reject([&] { world.SetTexture("/tex", invalid_texture); });
  invalid_texture = texture;
  const float infinity = std::numeric_limits<float>::infinity();
  std::memcpy(invalid_texture.texels.data() + 4, &infinity, sizeof(float));
  Reject([&] { world.SetTexture("/tex", invalid_texture); });
  Reject([&] { world.SetTexture("", texture); });
  invalid_material = material;
  invalid_material.base_color_texture = Lotus::TextureInput{};
  Reject([&] { world.SetMaterial("/paint", invalid_material); });
  invalid_material.base_color_texture = Lotus::TextureInput{"/tex", 1};
  Reject([&] { world.SetMaterial("/paint", invalid_material); });
  invalid_material = material;
  invalid_material.roughness_texture = Lotus::TextureInput{"/tex", 4};
  Reject([&] { world.SetMaterial("/paint", invalid_material); });
  invalid_material.roughness_texture = Lotus::TextureInput{"/tex", 3};
  invalid_material.roughness_texture->bias[2] =
      std::numeric_limits<float>::quiet_NaN();
  Reject([&] { world.SetMaterial("/paint", invalid_material); });
  invalid_material = material;
  invalid_material.texcoord_fallback[1] = infinity;
  Reject([&] { world.SetMaterial("/paint", invalid_material); });
  Check(world.Commit().revision == uv.revision,
      "rejected textures or texture inputs changed the scene revision");
  material.metallic_texture = Lotus::TextureInput{"/tex", 3};
  material.texcoords = "st";
  world.SetMaterial("/paint", material);
  world.SetTexture("/tex", texture);
  const auto textured = world.Commit();
  const auto texels = textured.scene->textures.at("/tex");
  Check(textured.revision == uv.revision + 1 && *texels == texture &&
      textured.scene->materials.at("/paint") == material,
      "a texture or a textured material was lost");
  world.SetTexture("/tex", texture);
  Check(world.Commit().scene == textured.scene,
      "an unchanged texture replaced the scene");
  world.RemoveTexture("/tex");
  world.RemoveTexture("/missing");
  const auto untextured_scene = world.Commit();
  Check(untextured_scene.scene->textures.empty() &&
      textured.scene->textures.at("/tex") == texels &&
      untextured_scene.scene->materials.at("/paint") == material,
      "removing a texture changed a retained snapshot or its material");
  geometry.texcoords.clear();
  world.SetMesh("/mesh", geometry, instance);

  world.SetMesh("/other", geometry, instance);
  const auto multiple = world.Commit();
  Check(multiple.triangle_count == 2 &&
      Lotus::ExtractDrawSummary(multiple).triangle_count == 1,
      "scene triangle count leaked into the bootstrap draw");
  world.RemoveMesh("/mesh");
  const auto removed = world.Commit();
  Check(removed.triangle_count == 1 && !removed.scene->meshes.contains("/mesh") &&
      edited.scene->meshes.contains("/mesh"), "removal changed an old snapshot");
  world.RemoveMesh("/mesh");
  Check(world.Commit().revision == removed.revision,
      "removing a missing mesh invalidated the scene");
  world.RemoveMesh("/other");
  Check(world.Commit().triangle_count == 0, "last mesh removal left a draw");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
