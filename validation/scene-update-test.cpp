// SPDX-License-Identifier: Apache-2.0
#include <lotus/extraction.hpp>

#include <iostream>
#include <stdexcept>

namespace {

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

} // namespace

int main() try {
  Lotus::RenderWorld world;
  Lotus::SceneExtraction extraction;
  Check(extraction.Update(world.Commit()).Empty(),
      "an empty scene produced GPU work");

  const Lotus::MeshGeometry triangle{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}},
      {{0, 1, 2}}, {0}};
  Lotus::MeshInstance instance;
  world.SetMesh("/b", triangle, instance);
  world.SetMesh("/a", triangle, instance);
  world.SetMesh("/empty", Lotus::MeshGeometry{{{0, 0, 0}}, {}, {}}, instance);
  const auto first = world.Commit();
  const auto* a = first.scene->meshes.at("/a").geometry.get();
  const auto* b = first.scene->meshes.at("/b").geometry.get();
  auto update = extraction.Update(first);
  Check(update.source_revision == first.revision &&
      update.geometry_releases.empty() && update.geometry_uploads.size() == 2 &&
      update.geometry_uploads[0].get() == a &&
      update.geometry_uploads[1].get() == b,
      "insertion did not upload non-empty geometry in key order");
  Check(update.instances_changed && update.instances.size() == 2 &&
      update.instances[0].geometry == a && update.instances[1].geometry == b,
      "insertion did not list visible instances in key order");

  Check(extraction.Update(world.Commit()).Empty(),
      "an unchanged commit produced GPU work");
  Lotus::Camera camera;
  camera.view[14] = -3;
  world.SetCamera(camera);
  Check(extraction.Update(world.Commit()).Empty(),
      "a camera change produced scene work");

  instance.world_from_object[12] = 5;
  world.SetMeshInstance("/a", instance);
  update = extraction.Update(world.Commit());
  Check(update.geometry_uploads.empty() && update.geometry_releases.empty() &&
      update.instances_changed && update.instances.size() == 2 &&
      update.instances[0].world_from_object[12] == 5 &&
      update.instances[1].world_from_object[12] == 0,
      "a transform edit uploaded geometry or lost the new transform");

  Lotus::MeshInstance hidden;
  hidden.visible = false;
  world.SetMeshInstance("/b", hidden);
  update = extraction.Update(world.Commit());
  Check(update.geometry_uploads.empty() && update.geometry_releases.empty() &&
      update.instances_changed && update.instances.size() == 1 &&
      update.instances[0].geometry == a,
      "hiding a mesh released its geometry or kept its instance");

  auto edited = triangle;
  edited.positions[0][2] = 2;
  world.SetMesh("/a", edited, instance);
  const auto moved = world.Commit();
  const auto* edited_a = moved.scene->meshes.at("/a").geometry.get();
  update = extraction.Update(moved);
  Check(update.geometry_releases.size() == 1 &&
      update.geometry_releases[0] == a && update.geometry_uploads.size() == 1 &&
      update.geometry_uploads[0].get() == edited_a &&
      update.instances_changed && update.instances[0].geometry == edited_a,
      "a point edit did not replace exactly one geometry buffer");

  // A hidden mesh has no instance, so removing it changes no instance.
  world.RemoveMesh("/b");
  update = extraction.Update(world.Commit());
  Check(update.geometry_releases.size() == 1 &&
      update.geometry_releases[0] == b && update.geometry_uploads.empty() &&
      !update.instances_changed && update.instances.empty(),
      "removing a hidden mesh did not release only its geometry");

  // A lost GPU scene gets everything again, without releases.
  extraction.Reset();
  update = extraction.Update(world.Commit());
  Check(update.geometry_releases.empty() &&
      update.geometry_uploads.size() == 1 &&
      update.geometry_uploads[0].get() == edited_a &&
      update.instances_changed && update.instances.size() == 1,
      "reset did not re-upload the resident scene");

  world.RemoveMesh("/a");
  world.RemoveMesh("/empty");
  update = extraction.Update(world.Commit());
  Check(update.geometry_releases.size() == 1 &&
      update.geometry_releases[0] == edited_a &&
      update.instances_changed && update.instances.empty(),
      "removing the last mesh left resident geometry or instances");
  Check(extraction.Update(Lotus::FrameSnapshot{}).Empty(),
      "a snapshot without a scene was not treated as empty");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
