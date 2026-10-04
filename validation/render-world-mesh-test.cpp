// SPDX-License-Identifier: Apache-2.0
#include <lotus/extraction.hpp>

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
  Check(world.Commit().revision == edited.revision,
      "rejected input changed the scene revision");

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
