// SPDX-License-Identifier: Apache-2.0
#include <lotus/extraction.hpp>

#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>

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
  Reject([&] { world.SetMesh("", geometry, instance); });
  auto invalid_instance = instance;
  invalid_instance.world_from_object[0] = std::numeric_limits<float>::infinity();
  Reject([&] { world.SetMeshInstance("/mesh", invalid_instance); });
  Reject([&] { world.SetMeshInstance("/missing", instance); });
  Lotus::SurfaceMaterial material;
  Check(edited.scene->meshes.at("/mesh").material == material &&
      edited.scene->environment == std::array<float, 3>{},
      "a new mesh or scene did not start with the default material and a "
      "black environment");
  auto invalid_material = material;
  invalid_material.base_color[1] = 1.5F;
  Reject([&] { world.SetMeshMaterial("/mesh", invalid_material); });
  invalid_material = material;
  invalid_material.roughness = -0.25F;
  Reject([&] { world.SetMeshMaterial("/mesh", invalid_material); });
  invalid_material = material;
  invalid_material.metallic = std::numeric_limits<float>::quiet_NaN();
  Reject([&] { world.SetMeshMaterial("/mesh", invalid_material); });
  invalid_material = material;
  invalid_material.emission[2] = -1.0F;
  Reject([&] { world.SetMeshMaterial("/mesh", invalid_material); });
  Reject([&] { world.SetMeshMaterial("/missing", material); });
  Reject([&] { world.SetEnvironment({1.0F, -1.0F, 0.0F}); });
  Reject([&] {
    world.SetEnvironment({std::numeric_limits<float>::infinity(), 0.0F, 0.0F});
  });
  world.SetMeshMaterial("/mesh", material);
  world.SetEnvironment({});
  Check(world.Commit().revision == edited.revision,
      "rejected input or unchanged materials changed the scene revision");

  material.base_color = {0.5F, 0.25F, 1.0F};
  material.emission = {2.0F, 0.0F, 0.0F};
  world.SetMeshMaterial("/mesh", material);
  const auto painted = world.Commit();
  Check(painted.revision == edited.revision + 1 &&
      painted.scene->meshes.at("/mesh").geometry ==
          edited.scene->meshes.at("/mesh").geometry &&
      painted.scene->meshes.at("/mesh").material == material &&
      edited.scene->meshes.at("/mesh").material == Lotus::SurfaceMaterial{},
      "a material edit copied geometry or changed a retained snapshot");
  geometry.positions[1][2] = 3;
  world.SetMesh("/mesh", geometry, instance);
  Check(world.Commit().scene->meshes.at("/mesh").material == material,
      "replacing a mesh's geometry lost its material");
  world.SetEnvironment({0.25F, 0.5F, 4.0F});
  const auto lit = world.Commit();
  Check(lit.scene->environment == std::array<float, 3>{0.25F, 0.5F, 4.0F} &&
      painted.scene->environment == std::array<float, 3>{},
      "an environment edit was lost or changed a retained snapshot");

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
