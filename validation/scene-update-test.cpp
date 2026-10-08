// SPDX-License-Identifier: Apache-2.0
#include <lotus/extraction.hpp>

#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

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
      update.instances[0].geometry == a && update.instances[1].geometry == b &&
      update.instances[0].material == 0 && update.instances[1].material == 0,
      "insertion did not list visible instances in key order");
  Check(!update.materials_changed && update.materials.empty(),
      "a scene without materials planned a material table");

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

  // The table is the default material, then the materials in key order.
  Lotus::Material metal;
  metal.metallic = 1.0F;
  Lotus::Material glow;
  glow.emission = {1.0F, 2.0F, 3.0F};
  world.SetMaterial("/m/metal", metal);
  world.SetMaterial("/m/glow", glow);
  world.BindMaterial("/b", "/m/metal");
  update = extraction.Update(world.Commit());
  Check(update.geometry_uploads.empty() && update.geometry_releases.empty() &&
      !update.environment_changed && update.instances_changed &&
      update.instances.size() == 2 && update.instances[0].material == 0 &&
      update.instances[1].material == 2 && update.materials_changed &&
      update.materials == std::vector<Lotus::SceneMaterial>{
                              {Lotus::Material{}}, {glow}, {metal}},
      "a material binding uploaded geometry or planned the wrong table");

  // A value edit rewrites the table and leaves the instances.
  metal.roughness = 0.25F;
  world.SetMaterial("/m/metal", metal);
  update = extraction.Update(world.Commit());
  Check(!update.instances_changed && update.materials_changed &&
      update.materials.size() == 3 &&
      update.materials[2] == Lotus::SceneMaterial{metal},
      "a material edit rewrote the instances or lost the new value");
  world.BindMaterial("/a", "/m/glow");
  update = extraction.Update(world.Commit());
  Check(update.instances_changed && !update.materials_changed &&
      update.instances[0].material == 1,
      "a binding to an existing material rewrote the table");

  // Removing a material shifts the later slots; its meshes fall back to
  // the default until it returns.
  world.RemoveMaterial("/m/glow");
  update = extraction.Update(world.Commit());
  Check(update.instances_changed && update.materials_changed &&
      update.materials == std::vector<Lotus::SceneMaterial>{
                              {Lotus::Material{}}, {metal}} &&
      update.instances[0].material == 0 && update.instances[1].material == 1,
      "a material removal did not fall back to the default material");
  world.SetMaterial("/m/glow", glow);
  update = extraction.Update(world.Commit());
  Check(update.instances_changed && update.materials_changed &&
      update.instances[0].material == 1 && update.instances[1].material == 2,
      "a returning material did not resume its binding");
  world.BindMaterial("/a", "");
  world.RemoveMaterial("/m/glow");
  update = extraction.Update(world.Commit());
  Check(update.instances_changed && update.materials_changed &&
      update.instances[0].material == 0 && update.instances[1].material == 1,
      "unbinding a mesh did not return it to the default material");

  world.SetEnvironment({1.0F, 0.5F, 0.25F});
  update = extraction.Update(world.Commit());
  Check(update.geometry_uploads.empty() && update.geometry_releases.empty() &&
      !update.instances_changed && update.environment_changed &&
      update.environment == std::array<float, 3>{1.0F, 0.5F, 0.25F} &&
      !update.Empty(),
      "an environment edit was not planned on its own");

  // Each instancer placement is an instance of the same resident geometry,
  // in placement order after the meshes before it.
  auto prototype = instance;
  Lotus::Matrix4 up = Lotus::IdentityMatrix();
  up[13] = 2;
  Lotus::Matrix4 down = Lotus::IdentityMatrix();
  down[13] = -2;
  prototype.instancer_transforms = std::vector<Lotus::Matrix4>{up, down, up};
  world.SetMeshInstance("/a", prototype);
  update = extraction.Update(world.Commit());
  Check(update.geometry_uploads.empty() && update.geometry_releases.empty() &&
      update.instances_changed && update.instances.size() == 4 &&
      update.instances[0].geometry == a && update.instances[1].geometry == a &&
      update.instances[2].geometry == a && update.instances[3].geometry == b &&
      update.instances[0].world_from_object[12] == 5 &&
      update.instances[0].world_from_object[13] == 2 &&
      update.instances[1].world_from_object[13] == -2 &&
      update.instances[2] == update.instances[0],
      "instancer placements were not expanded in order over one geometry");
  prototype.instancer_transforms->pop_back();
  world.SetMeshInstance("/a", prototype);
  update = extraction.Update(world.Commit());
  Check(update.geometry_uploads.empty() && update.instances_changed &&
      update.instances.size() == 3,
      "removing an instancer placement did not remove one instance");
  prototype.instancer_transforms->clear();
  world.SetMeshInstance("/a", prototype);
  update = extraction.Update(world.Commit());
  Check(update.geometry_uploads.empty() && update.geometry_releases.empty() &&
      update.instances_changed && update.instances.size() == 1 &&
      update.instances[0].geometry == b,
      "a prototype without placements released its geometry or kept an "
      "instance");
  world.SetMeshInstance("/a", instance);
  update = extraction.Update(world.Commit());
  Check(update.instances_changed && update.instances.size() == 2 &&
      update.instances[0].geometry == a,
      "an ordinary placement was not restored");

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
  Check(update.environment_changed &&
      update.environment == std::array<float, 3>{1.0F, 0.5F, 0.25F} &&
      update.geometry_releases.empty() &&
      update.geometry_uploads.size() == 1 &&
      update.geometry_uploads[0].get() == edited_a &&
      update.instances_changed && update.instances.size() == 1 &&
      update.materials_changed && update.materials.size() == 2,
      "reset did not re-upload the resident scene");

  // Every texture in the scene is resident. A material's lookups resolve
  // their keys to textures, and its instances read the texture-coordinate
  // set it names, by the set's position in key order.
  Lotus::Texture checker;
  checker.width = 2;
  checker.height = 1;
  checker.format = Lotus::TextureFormat::Rgba8Srgb;
  checker.texels = {255, 0, 0, 255, 0, 0, 255, 255};
  world.SetTexture("/t/checker", checker);
  auto mapped = edited;
  mapped.texcoords["map1"] = {{{0, 0}}, {{1, 0}}, {{0, 1}}};
  mapped.texcoords["st"] = {{{0, 0}}, {{2, 0}}, {{0, 2}}};
  world.SetMesh("/a", mapped, instance);
  Lotus::Material painted;
  painted.base_color_texture = Lotus::TextureInput{"/t/checker"};
  Lotus::TextureInput missing;
  missing.texture = "/t/missing";
  missing.channel = 1;
  painted.roughness_texture = missing;
  painted.normal_texture = Lotus::TextureInput{"/t/checker"};
  painted.texcoords = "st";
  world.SetMaterial("/m/painted", painted);
  world.BindMaterial("/a", "/m/painted");
  const auto textured = world.Commit();
  const auto* checker_texture = textured.scene->textures.at("/t/checker").get();
  const auto* mapped_a = textured.scene->meshes.at("/a").geometry.get();
  update = extraction.Update(textured);
  Check(update.texture_releases.empty() && update.texture_uploads.size() == 1 &&
            update.texture_uploads[0].get() == checker_texture &&
            update.materials_changed && update.materials.size() == 3 &&
            update.materials[2].material == painted &&
            update.materials[2].textures ==
                std::array<const Lotus::Texture*, Lotus::kMaterialTextureInputs>{
                    checker_texture, nullptr, nullptr, nullptr, checker_texture} &&
            update.instances_changed && update.instances.size() == 1 &&
            update.instances[0].geometry == mapped_a &&
            update.instances[0].material == 2 && update.instances[0].texcoords == 1,
      "a textured material did not upload its texture, resolve its keys or "
      "select its texture-coordinate set");

  // Replacing a texture's texels replaces the texture, and the table that
  // samples it, but no instance.
  checker.texels[0] = 128;
  world.SetTexture("/t/checker", checker);
  const auto retextured = world.Commit();
  const auto* replaced = retextured.scene->textures.at("/t/checker").get();
  update = extraction.Update(retextured);
  Check(update.texture_releases == std::vector<const Lotus::Texture*>{
                                       checker_texture} &&
      update.texture_uploads.size() == 1 &&
      update.texture_uploads[0].get() == replaced &&
      update.materials_changed && update.materials[2].textures[0] == replaced &&
      update.materials[2].textures[4] == replaced &&
      !update.instances_changed && update.geometry_uploads.empty(),
      "a texture edit did not replace exactly the texture and the table");

  // A set the mesh lacks reads none; a removed texture leaves its lookup to
  // the fallback.
  painted.texcoords = "uv";
  world.SetMaterial("/m/painted", painted);
  update = extraction.Update(world.Commit());
  Check(update.instances_changed &&
      update.instances[0].texcoords == Lotus::kNoTexcoords,
      "a missing texture-coordinate set was selected");
  world.RemoveTexture("/t/checker");
  update = extraction.Update(world.Commit());
  Check(update.texture_releases == std::vector<const Lotus::Texture*>{
                                       replaced} &&
      update.texture_uploads.empty() && update.materials_changed &&
      update.materials[2].textures ==
          std::array<const Lotus::Texture*, Lotus::kMaterialTextureInputs>{} &&
      !update.instances_changed,
      "a texture removal did not release it and leave its lookup without one");
  world.SetTexture("/t/unused", checker);
  update = extraction.Update(world.Commit());
  Check(update.texture_uploads.size() == 1 && !update.materials_changed &&
      !update.instances_changed,
      "a texture no material names was not resident on its own");

  // A constant tangent normal still needs its named UV frame even without
  // a texture lookup. Restoring the identity stops reading the set.
  painted.base_color_texture.reset();
  painted.roughness_texture.reset();
  painted.normal_texture.reset();
  painted.normal = {0.5F, 0, 0.866025404F};
  painted.texcoords = "st";
  world.SetMaterial("/m/painted", painted);
  update = extraction.Update(world.Commit());
  Check(update.materials_changed && update.instances_changed &&
            update.instances[0].texcoords == 1 &&
            update.geometry_uploads.empty() && update.texture_uploads.empty(),
      "a constant tangent normal did not select its UV frame");
  painted.normal = Lotus::Material{}.normal;
  world.SetMaterial("/m/painted", painted);
  update = extraction.Update(world.Commit());
  Check(update.materials_changed && update.instances_changed &&
            update.instances[0].texcoords == Lotus::kNoTexcoords,
      "the identity normal kept reading a UV frame without lookups");

  // An opacity-only lookup selects UVs and resolves its sixth texture slot.
  // Changing coverage without changing the lookup touches only the table.
  painted.opacity_texture = Lotus::TextureInput{"/t/unused", 3};
  world.SetMaterial("/m/painted", painted);
  update = extraction.Update(world.Commit());
  Check(update.materials_changed && update.instances_changed &&
            update.instances[0].texcoords == 1 &&
            update.materials[2].textures[5] != nullptr &&
            update.geometry_uploads.empty(),
      "an opacity-only texture did not select its coordinates and image");
  painted.opacity = 0.25F;
  painted.opacity_threshold = 0.5F;
  world.SetMaterial("/m/painted", painted);
  update = extraction.Update(world.Commit());
  Check(update.materials_changed && !update.instances_changed &&
            update.geometry_uploads.empty() && update.texture_uploads.empty(),
      "a coverage edit changed geometry or instances");

  painted.opacity_texture.reset();
  painted.specular_color_texture = Lotus::TextureInput{"/t/unused"};
  painted.use_specular_workflow = true;
  world.SetMaterial("/m/painted", painted);
  update = extraction.Update(world.Commit());
  Check(update.materials_changed && !update.instances_changed &&
            update.materials[2].textures[6] != nullptr &&
            update.materials[2].textures[5] == nullptr && update.geometry_uploads.empty(),
      "a specular-only lookup did not resolve the seventh texture slot");
  painted.ior = 2.0F;
  painted.specular_color = {0.1F, 0.3F, 0.5F};
  world.SetMaterial("/m/painted", painted);
  update = extraction.Update(world.Commit());
  Check(update.materials_changed && !update.instances_changed &&
            update.geometry_uploads.empty() && update.texture_uploads.empty(),
      "a specular parameter edit changed geometry or instances");

  world.RemoveMesh("/a");
  world.RemoveMesh("/empty");
  world.RemoveMaterial("/m/metal");
  world.RemoveMaterial("/m/painted");
  world.RemoveTexture("/t/unused");
  world.SetEnvironment({});
  update = extraction.Update(world.Commit());
  Check(update.geometry_releases.size() == 1 &&
      update.geometry_releases[0] == mapped_a &&
      update.texture_releases.size() == 1 &&
      update.instances_changed && update.instances.empty() &&
      update.materials_changed && update.materials.size() == 1 &&
      update.environment_changed && update.environment == std::array<float, 3>{},
      "removing the last mesh left resident geometry, textures, instances, "
      "materials or light");
  Check(extraction.Update(Lotus::FrameSnapshot{}).Empty(),
      "a snapshot without a scene was not treated as empty");
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
