// SPDX-License-Identifier: Apache-2.0
#include <lotus/extraction.hpp>
#include <lotus/render_world.hpp>
#include <lotus/vulkan_backend.hpp>

#include "reference.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

// Gives a mesh a material of its own, keyed by the mesh's key.
void Paint(Lotus::RenderWorld& world, const std::string& mesh,
    const Lotus::Material& material) {
  world.SetMaterial(mesh, material);
  world.BindMaterial(mesh, mesh);
}

// Process id, for a session identifier that is unique among concurrent runs.
long long CurrentProcessId() {
#if defined(_WIN32)
  return static_cast<long long>(_getpid());
#else
  return static_cast<long long>(getpid());
#endif
}

struct Check {
  std::string id;
  std::string status;
  std::string detail;
};

std::string Escape(std::string_view value) {
  std::string escaped;
  for (const char character : value) {
    switch (character) {
    case '\\':
      escaped += "\\\\";
      break;
    case '"':
      escaped += "\\\"";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      escaped += character;
      break;
    }
  }
  return escaped;
}

// The producing invocation behind this report. OpenStrata refuses a PASS that
// no completed producer stands behind, so the report records who wrote it,
// against what, and whether the run actually finished.
struct Session {
  std::string id;
  std::string target;
  long long started = 0;
  long long completed = 0;
  // Whether this harness reached its verdicts, not whether the verdicts were
  // PASS. A run that decided every check succeeded, however many of them
  // failed; only a run that could not produce checks at all is a failure.
  bool succeeded = false;
};

bool WriteReport(const std::string& path,
    const std::vector<Check>& checks,
    const Lotus::GpuFrameEvidence& frame,
    const Session& session) {
  // Write to a sibling temp file and rename into place, so a run killed
  // mid-write leaves no partial overlay for `ost renderer merge` to pick up.
  const std::filesystem::path final_path(path);
  std::filesystem::path temp_path = final_path;
  temp_path += ".tmp";
  {
    std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
    if (!output) {
      return false;
    }
    output << "{\n"
           << "  \"schema\": \"openstrata.renderer-report/v1alpha1\",\n"
           << "  \"renderer\": {\"name\": \"lotus\"},\n"
           << "  \"producer\": {\"id\":\"" << Escape(session.id)
           << "\",\"kind\":\"renderer-harness\",\"target\":\""
           << Escape(session.target) << "\",\"started_unix\":" << session.started
           << ",\"completed_unix\":" << session.completed << ",\"outcome\":\""
           << (session.succeeded ? "success" : "failure") << "\"},\n";
    if (!frame.device_name.empty()) {
      output << "  \"device\": {\"backend\":\"vulkan\",\"name\":\""
             << Escape(frame.device_name) << "\",\"api_version\":\""
             << Escape(frame.api_version) << "\",\"driver_version\":\""
             << Escape(frame.driver_version) << "\",\"vendor_id\":"
             << frame.vendor_id << ",\"device_id\":" << frame.device_id << "},\n";
    }
    output << "  \"checks\": [\n";
    for (std::size_t index = 0; index < checks.size(); ++index) {
      const Check& check = checks[index];
      output << "    {\"id\":\"" << Escape(check.id) << "\",\"status\":\""
             << Escape(check.status) << "\"";
      if (!check.detail.empty()) {
        output << ",\"detail\":\"" << Escape(check.detail) << "\"";
      }
      output << '}' << (index + 1 == checks.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
    if (!output.good()) {
      return false;
    }
  }
  std::error_code error;
  std::filesystem::rename(temp_path, final_path, error);
  if (error) {
    std::filesystem::remove(temp_path, error);
    return false;
  }
  return true;
}

// A perspective camera 3 units in front of the bootstrap triangle, looking
// down -Z: 45 degree vertical field of view, square aspect, clipping range
// [1, 10]. A transposed or misapplied matrix moves the triangle off the
// centre pixel or its depth out of range, so the product checks catch it.
Lotus::Camera BootstrapCamera() {
  constexpr float kFocal = 2.41421356F; // 1 / tan(22.5 degrees)
  constexpr float kNear = 1.0F;
  constexpr float kFar = 10.0F;
  Lotus::Camera camera;
  camera.view[14] = -3.0F;
  camera.projection = {kFocal, 0.0F, 0.0F, 0.0F, 0.0F, kFocal, 0.0F, 0.0F,
      0.0F, 0.0F, (kFar + kNear) / (kNear - kFar), -1.0F,
      0.0F, 0.0F, 2.0F * kFar * kNear / (kNear - kFar), 0.0F};
  return camera;
}

// Whether a GPU material table entry holds `material`: its constants and
// lookup parameters, and for each lookup whose key names a scene texture,
// the slot of a resident texture with the same texels. A lookup without one
// samples no slot.
bool MaterialMatches(const Lotus::GpuMaterialContents& gpu,
    const Lotus::Material& material, const Lotus::LotusScene& scene,
    const Lotus::GpuSceneContents& contents) {
  Lotus::Material expected = material;
  expected.texcoords.clear();
  const std::array<std::optional<Lotus::TextureInput>*, Lotus::kMaterialTextureInputs> inputs{
      &expected.base_color_texture, &expected.roughness_texture,
      &expected.metallic_texture, &expected.emission_texture,
      &expected.normal_texture, &expected.opacity_texture,
      &expected.specular_color_texture};
  for (std::size_t index = 0; index < inputs.size(); ++index) {
    const std::uint32_t slot = gpu.texture_slots[index];
    if (!inputs[index]->has_value()) {
      if (slot != Lotus::kNoTextureSlot)
        return false;
      continue;
    }
    const auto texture = scene.textures.find((*inputs[index])->texture);
    (*inputs[index])->texture.clear();
    if (texture == scene.textures.end()) {
      if (slot != Lotus::kNoTextureSlot)
        return false;
      continue;
    }
    const auto resident = std::find_if(contents.textures.begin(),
        contents.textures.end(),
        [&](const Lotus::GpuTextureContents& t) { return t.slot == slot; });
    if (resident == contents.textures.end() ||
        resident->texture != *texture->second)
      return false;
  }
  return gpu.material == expected;
}

// Whether the GPU scene's buffers hold exactly the scene's resident geometry,
// normals and texture coordinates included, its textures, and each placement
// of its visible meshes, in the update plan's order, with its material and
// the texture-coordinate set the material reads.
bool SceneMatches(const Lotus::GpuSceneContents& contents,
    const Lotus::LotusScene& scene) {
  if (contents.status != Lotus::FrameStatus::Pass ||
      (!contents.instances.empty() &&
          contents.materials.size() != scene.materials.size() + 1) ||
      contents.textures.size() != scene.textures.size()) {
    return false;
  }
  for (const auto& [id, texture] : scene.textures) {
    (void)id;
    if (std::none_of(contents.textures.begin(), contents.textures.end(),
            [&](const Lotus::GpuTextureContents& t) {
              return t.texture == *texture;
            }))
      return false;
  }
  std::set<const Lotus::MeshGeometry*> resident;
  std::size_t instance = 0;
  for (const auto& [id, mesh] : scene.meshes) {
    (void)id;
    if (mesh.geometry->triangles.empty()) {
      continue;
    }
    resident.insert(mesh.geometry.get());
    if (!mesh.instance.visible) {
      continue;
    }
    for (const Lotus::Matrix4& placement :
        Lotus::PlacementTransforms(mesh.instance)) {
      if (instance >= contents.instances.size()) {
        return false;
      }
      const Lotus::GpuInstanceContents& gpu = contents.instances[instance++];
      const auto found = scene.materials.find(mesh.material);
      const Lotus::Material material =
          found == scene.materials.end() ? Lotus::Material{} : found->second;
      const auto geometry = std::find_if(contents.geometries.begin(),
          contents.geometries.end(), [&](const Lotus::GpuGeometryContents& g) {
            return g.slot == gpu.geometry_slot;
          });
      std::vector<std::vector<std::array<float, 2>>> texcoords;
      std::uint32_t set = Lotus::kNoTexcoords;
      for (const auto& [name, values] : mesh.geometry->texcoords) {
        if (name == material.texcoords &&
            (Lotus::HasTextureInputs(material) || material.normal != Lotus::Material{}.normal))
          set = static_cast<std::uint32_t>(texcoords.size());
        texcoords.push_back(values);
      }
      if (geometry == contents.geometries.end() ||
          geometry->positions != mesh.geometry->positions ||
          geometry->triangles != mesh.geometry->triangles ||
          geometry->normals != mesh.geometry->normals ||
          geometry->texcoords != texcoords ||
          gpu.world_from_object != placement || gpu.texcoords != set ||
          gpu.material_slot >= contents.materials.size() ||
          !MaterialMatches(contents.materials[gpu.material_slot], material,
              scene, contents)) {
        return false;
      }
    }
  }
  return instance == contents.instances.size() &&
         resident.size() == contents.geometries.size() &&
         contents.environment == scene.environment;
}

// Whether the TLAS build input holds one instance per instance record, in
// the same order, referencing the record's geometry's BLAS with the same
// transform.
bool TlasMatches(const Lotus::GpuSceneContents& contents) {
  if (contents.tlas_instances.size() != contents.instances.size()) {
    return false;
  }
  for (std::size_t index = 0; index < contents.instances.size(); ++index) {
    const Lotus::GpuInstanceContents& instance = contents.instances[index];
    const Lotus::GpuTlasInstanceContents& tlas = contents.tlas_instances[index];
    if (tlas.geometry_slot != instance.geometry_slot || tlas.mask != 0xFFU) {
      return false;
    }
    for (int row = 0; row < 3; ++row) {
      for (int column = 0; column < 4; ++column) {
        if (tlas.object_to_world[row * 4 + column] !=
            instance.world_from_object[column * 4 + row]) {
          return false;
        }
      }
    }
  }
  return true;
}

// The BLAS and TLAS verdict of the scene walk below. Without acceleration
// structures on the device it is a SKIP that says why.
struct AccelerationVerdict {
  bool available = false;
  std::string detail;
  std::string failure;
};

// Drives the renderer's GPU scene through insertion, an unchanged commit, a
// transform edit, a material binding, a material edit, a material removal,
// an environment edit, a hide, a point and normal edit,
// instancer placements, texture insertion, edit and removal, and removal,
// comparing the device buffers and images with the CPU scene after each.
// Returns an empty string on success.
// Acceleration-structure mismatches go to `acceleration` instead.
std::string SceneUploadFailure(Lotus::OffscreenRenderer& renderer,
    AccelerationVerdict& acceleration) {
  Lotus::RenderWorld world;
  Lotus::SceneExtraction extraction;
  Lotus::GpuSceneStats before;
  // Records the first acceleration-structure mismatch; the walk goes on so
  // the buffer checks still run.
  const auto expect = [&](bool ok, const char* step, const char* what) {
    if (before.acceleration_available && !ok &&
        acceleration.failure.empty()) {
      acceleration.failure = std::string(step) + ": " + what;
    }
  };
  const auto apply = [&](const char* step) -> std::string {
    const Lotus::FrameSnapshot snapshot = world.Commit();
    const Lotus::GpuSceneEvidence evidence =
        renderer.UpdateScene(extraction.Update(snapshot));
    if (evidence.status != Lotus::FrameStatus::Pass) {
      return std::string(step) + ": " + evidence.detail;
    }
    if (evidence.validation_message_count != 0) {
      return std::string(step) + ": Vulkan validation reported messages";
    }
    const Lotus::GpuSceneContents contents = renderer.ReadBackScene();
    if (!SceneMatches(contents, *snapshot.scene)) {
      return std::string(step) + ": device buffers differ from the scene";
    }
    before = evidence.stats;
    acceleration.available = before.acceleration_available;
    acceleration.detail = before.acceleration_detail;
    expect(TlasMatches(contents), step,
        "the TLAS build input differs from the instances");
    expect(before.blas_count == before.resident_geometries &&
               before.tlas_instance_count == before.instance_count,
        step, "BLAS or TLAS instance count differs from the scene");
    return {};
  };
  const auto stats = [&] { return renderer.UpdateScene({}).stats; };

  Lotus::MeshInstance instance;
  instance.world_from_object[12] = 0.25F;
  world.SetMesh("/quad",
      {{{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}},
          {{0, 1, 2}, {0, 2, 3}}, {0, 0}},
      instance);
  world.SetMesh("/triangle", {{{0, 0, 1}, {1, 0, 1}, {0, 1, 1}}, {{0, 1, 2}}, {0}}, Lotus::MeshInstance{});
  world.SetMesh("/empty", {{{0, 0, 0}}, {}, {}}, Lotus::MeshInstance{});
  if (auto failure = apply("insertion"); !failure.empty())
    return failure;
  if (before.resident_geometries != 2 || before.instance_count != 2 ||
      before.geometry_uploads != 2 || before.upload_submissions != 1) {
    return "insertion: unexpected resident geometry or upload counts";
  }
  expect(before.blas_builds == 2 && before.tlas_builds == 1 &&
             before.tlas_updates == 0 && before.acceleration_bytes != 0,
      "insertion", "expected two BLAS builds and one TLAS build");

  const Lotus::GpuSceneStats inserted = before;
  if (auto failure = apply("unchanged commit"); !failure.empty()) {
    return failure;
  }
  if (before.upload_submissions != inserted.upload_submissions ||
      before.instance_writes != inserted.instance_writes) {
    return "unchanged commit: the GPU scene recorded work";
  }
  expect(before.blas_builds == inserted.blas_builds &&
             before.tlas_builds == inserted.tlas_builds &&
             before.tlas_updates == inserted.tlas_updates,
      "unchanged commit", "acceleration structures were built");

  instance.world_from_object[13] = -0.5F;
  world.SetMeshInstance("/quad", instance);
  if (auto failure = apply("transform edit"); !failure.empty()) {
    return failure;
  }
  if (before.geometry_uploads != 2 ||
      before.instance_writes != inserted.instance_writes + 1) {
    return "transform edit: geometry was uploaded again";
  }
  expect(before.blas_builds == 2 && before.tlas_builds == 1 &&
             before.tlas_updates == 1,
      "transform edit", "expected a TLAS update and no build");

  const Lotus::GpuSceneStats moved = before;
  Lotus::Material material;
  material.base_color = {0.25F, 0.5F, 0.75F};
  material.roughness = 0.125F;
  material.metallic = 0.5F;
  material.emission = {1.0F, 2.0F, 3.0F};
  world.SetMaterial("/paint", material);
  world.BindMaterial("/quad", "/paint");
  if (auto failure = apply("material binding"); !failure.empty()) {
    return failure;
  }
  if (before.geometry_uploads != 2 || before.material_count != 2 ||
      before.instance_writes != moved.instance_writes + 1 ||
      before.material_writes != moved.material_writes + 1) {
    return "material binding: geometry was uploaded, or the instances or "
           "the material table were not rewritten";
  }
  expect(before.tlas_builds == 1 && before.tlas_updates == 1,
      "material binding", "the TLAS was built or updated");

  const Lotus::GpuSceneStats bound = before;
  material.base_color = {0.75F, 0.5F, 0.25F};
  world.SetMaterial("/paint", material);
  if (auto failure = apply("material edit"); !failure.empty()) {
    return failure;
  }
  if (before.instance_writes != bound.instance_writes ||
      before.material_writes != bound.material_writes + 1 ||
      before.upload_submissions != bound.upload_submissions + 1) {
    return "material edit: the instances were rewritten or the material "
           "table was not";
  }
  expect(before.tlas_builds == 1 && before.tlas_updates == 1,
      "material edit", "the TLAS was built or updated");

  // The binding stays; the mesh falls back to the default material.
  const Lotus::GpuSceneStats edited = before;
  world.RemoveMaterial("/paint");
  if (auto failure = apply("material removal"); !failure.empty()) {
    return failure;
  }
  if (before.material_count != 1 ||
      before.instance_writes != edited.instance_writes + 1 ||
      before.material_writes != edited.material_writes + 1) {
    return "material removal: the instances or the material table were not "
           "rewritten";
  }

  const Lotus::GpuSceneStats painted = before;
  world.SetEnvironment({0.5F, 1.0F, 2.0F});
  if (auto failure = apply("environment edit"); !failure.empty()) {
    return failure;
  }
  if (before.upload_submissions != painted.upload_submissions ||
      before.instance_writes != painted.instance_writes) {
    return "environment edit: the GPU scene recorded work";
  }
  world.SetEnvironment({});

  Lotus::MeshInstance hidden;
  hidden.visible = false;
  world.SetMeshInstance("/quad", hidden);
  if (auto failure = apply("hide"); !failure.empty())
    return failure;
  if (before.resident_geometries != 2 || before.instance_count != 1 ||
      before.geometry_uploads != 2) {
    return "hide: geometry was released or uploaded";
  }
  expect(before.blas_builds == 2 && before.tlas_builds == 2 &&
             before.tlas_updates == 1,
      "hide", "expected a TLAS rebuild and no BLAS build");

  // The new geometry also carries corner normals, which follow its
  // triangles in the same buffer.
  const std::uint32_t triangle_slot =
      renderer.ReadBackScene().instances.at(0).geometry_slot;
  world.SetMesh("/triangle",
      {{{0, 0, 2}, {1, 0, 2}, {0, 1, 2}}, {{0, 1, 2}}, {0},
          {{0, 0, 1}, {0.5F, 0, 1}, {0, -0.25F, 2}}},
      Lotus::MeshInstance{});
  if (auto failure = apply("point and normal edit"); !failure.empty())
    return failure;
  if (before.resident_geometries != 2 || before.geometry_uploads != 3 ||
      before.geometry_releases != 1 ||
      renderer.ReadBackScene().instances.at(0).geometry_slot != triangle_slot) {
    return "point and normal edit: the geometry was not replaced in its slot";
  }
  expect(before.blas_builds == 3 && before.tlas_builds == 3 &&
             before.tlas_updates == 1,
      "point and normal edit", "expected one BLAS build and a TLAS rebuild");

  // An instancer places one resident geometry, with one BLAS, several times.
  Lotus::MeshInstance prototype;
  Lotus::Matrix4 placement = Lotus::IdentityMatrix();
  prototype.instancer_transforms.emplace();
  for (float offset : {-0.5F, 0.0F, 0.5F}) {
    placement[12] = offset;
    prototype.instancer_transforms->push_back(placement);
  }
  world.SetMeshInstance("/triangle", prototype);
  if (auto failure = apply("instancer expansion"); !failure.empty())
    return failure;
  if (before.resident_geometries != 2 || before.instance_count != 3 ||
      before.geometry_uploads != 3) {
    return "instancer expansion: expected three instances of resident "
           "geometry and no upload";
  }
  expect(before.blas_count == 2 && before.blas_builds == 3 &&
             before.tlas_builds == 4 && before.tlas_updates == 1,
      "instancer expansion", "expected a TLAS rebuild and no BLAS build");

  const Lotus::GpuSceneStats expanded = before;
  (*prototype.instancer_transforms)[1][13] = 0.75F;
  world.SetMeshInstance("/triangle", prototype);
  if (auto failure = apply("instancer transform edit"); !failure.empty())
    return failure;
  if (before.geometry_uploads != 3 ||
      before.instance_writes != expanded.instance_writes + 1) {
    return "instancer transform edit: geometry was uploaded or the "
           "instances were not rewritten";
  }
  expect(before.blas_builds == 3 && before.tlas_builds == 4 &&
             before.tlas_updates == 2,
      "instancer transform edit", "expected a TLAS update and no build");

  prototype.instancer_transforms->clear();
  world.SetMeshInstance("/triangle", prototype);
  if (auto failure = apply("instancer without instances"); !failure.empty())
    return failure;
  if (before.resident_geometries != 2 || before.instance_count != 0 ||
      before.geometry_releases != 1) {
    return "instancer without instances: geometry was released or an "
           "instance remained";
  }
  expect(before.blas_count == 2 && before.tlas_builds == 5,
      "instancer without instances", "expected an empty TLAS rebuild");

  world.SetMeshInstance("/triangle", Lotus::MeshInstance{});
  if (auto failure = apply("ordinary placement"); !failure.empty())
    return failure;
  if (before.instance_count != 1 || before.geometry_uploads != 3) {
    return "ordinary placement: expected one instance and no upload";
  }
  expect(before.blas_builds == 3 && before.tlas_builds == 6 &&
             before.tlas_updates == 2,
      "ordinary placement", "expected a TLAS rebuild and no BLAS build");

  // Textures become images in the texture table; the geometry carries two
  // texture-coordinate sets, of which the material reads the second. One
  // lookup's key names no texture, so it samples none.
  const Lotus::GpuSceneStats placed = before;
  Lotus::Texture checker;
  checker.width = 2;
  checker.height = 2;
  checker.format = Lotus::TextureFormat::Rgba8Srgb;
  checker.texels = {255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0, 255, 255,
      255, 255};
  Lotus::Texture radiance;
  radiance.width = 1;
  radiance.height = 2;
  radiance.format = Lotus::TextureFormat::Rgba32Float;
  const float hdr[8] = {4.0F, 2.0F, 1.0F, 1.0F, 0.5F, 0.25F, 8.0F, 0.0F};
  radiance.texels.resize(sizeof(hdr));
  std::memcpy(radiance.texels.data(), hdr, sizeof(hdr));
  world.SetTexture("/checker", checker);
  world.SetTexture("/radiance", radiance);
  Lotus::MeshGeometry mapped{{{0, 0, 2}, {1, 0, 2}, {0, 1, 2}}, {{0, 1, 2}}, {0}};
  mapped.texcoords["map1"] = {{{0, 0}}, {{1, 0}}, {{0, 1}}};
  mapped.texcoords["st"] = {{{0.25F, 0}}, {{2, 0}}, {{0, -1}}};
  world.SetMesh("/triangle", mapped, Lotus::MeshInstance{});
  Lotus::Material textured;
  textured.base_color_texture = Lotus::TextureInput{"/checker"};
  textured.base_color_texture->wrap_s = Lotus::TextureWrap::Repeat;
  textured.base_color_texture->wrap_t = Lotus::TextureWrap::Mirror;
  textured.roughness_texture = Lotus::TextureInput{"/missing", 2};
  textured.roughness_texture->fallback = {0.1F, 0.2F, 0.3F, 0.4F};
  textured.emission_texture = Lotus::TextureInput{"/radiance"};
  textured.emission_texture->wrap_s = Lotus::TextureWrap::Clamp;
  textured.emission_texture->scale = {2.0F, 2.0F, 2.0F, 1.0F};
  textured.emission_texture->bias = {0.5F, 0.0F, 0.0F, 0.0F};
  textured.texcoords = "st";
  textured.texcoord_fallback = {0.5F, 0.75F};
  textured.normal = {0.25F, -0.5F, 0.75F};
  textured.normal_texture = Lotus::TextureInput{"/checker"};
  textured.normal_texture->scale = {2, 2, 2, 1};
  textured.normal_texture->bias = {-1, -1, -1, 0};
  textured.opacity = 0.75F;
  textured.opacity_threshold = 0.5F;
  textured.opacity_texture = Lotus::TextureInput{"/checker"};
  textured.opacity_texture->channel = 3;
  textured.ior = 2.0F;
  textured.use_specular_workflow = true;
  textured.specular_color = {0.1F, 0.3F, 0.5F};
  textured.specular_color_texture = Lotus::TextureInput{"/checker"};
  textured.specular_color_texture->scale = {0.5F, 0.75F, 1, 1};
  world.SetMaterial("/textured", textured);
  world.BindMaterial("/triangle", "/textured");
  if (auto failure = apply("texture insertion"); !failure.empty())
    return failure;
  if (before.resident_textures != 2 || before.texture_uploads != 2 ||
      before.texture_bytes != 2 * 2 * 4 + 1 * 2 * 16 ||
      before.material_writes != placed.material_writes + 1 ||
      before.upload_submissions != placed.upload_submissions + 1) {
    return "texture insertion: unexpected texture or material table counts";
  }

  const Lotus::GpuSceneStats inserted_textures = before;
  checker.texels[0] = 128;
  world.SetTexture("/checker", checker);
  if (auto failure = apply("texture edit"); !failure.empty())
    return failure;
  if (before.resident_textures != 2 || before.texture_uploads != 3 ||
      before.texture_releases != 1 ||
      before.geometry_uploads != inserted_textures.geometry_uploads ||
      before.instance_writes != inserted_textures.instance_writes ||
      before.material_writes != inserted_textures.material_writes + 1) {
    return "texture edit: the texture was not replaced alone with the table";
  }

  world.RemoveTexture("/radiance");
  if (auto failure = apply("texture removal"); !failure.empty())
    return failure;
  if (before.resident_textures != 1 || before.texture_releases != 2 ||
      before.texture_bytes != 2 * 2 * 4) {
    return "texture removal: the texture stayed resident";
  }

  world.RemoveMesh("/quad");
  world.RemoveMesh("/triangle");
  world.RemoveMesh("/empty");
  world.RemoveMaterial("/textured");
  world.RemoveTexture("/checker");
  if (auto failure = apply("removal"); !failure.empty())
    return failure;
  if (before.resident_geometries != 0 || before.instance_count != 0 ||
      before.geometry_bytes != 0 || before.geometry_releases != 4 ||
      before.resident_textures != 0 || before.texture_bytes != 0 ||
      stats().upload_submissions != before.upload_submissions) {
    return "removal: the GPU scene kept geometry, textures or instances";
  }
  expect(before.blas_builds == 4 && before.tlas_builds == 8 &&
             before.tlas_updates == 2,
      "removal", "expected an empty TLAS rebuild");
  return {};
}

// The scene-update timestamps verdict: a SKIP without timestamp support,
// otherwise the measured durations or the first mismatch.
struct SceneTimestampVerdict {
  bool available = false;
  std::string detail;
  std::string failure;
};

// Times an instanced grid's insertion, a transform edit, a material edit, an
// unchanged commit and removal. A phase a step runs measures a positive
// duration and one it skips reports 0; the empty TLAS of the removal is only
// required to be finite.
SceneTimestampVerdict SceneTimestamps(Lotus::OffscreenRenderer& renderer) {
  constexpr std::uint32_t kQuads = 256;
  constexpr std::uint32_t kPlacements = 32;
  Lotus::MeshGeometry grid;
  for (std::uint32_t y = 0; y <= kQuads; ++y) {
    for (std::uint32_t x = 0; x <= kQuads; ++x) {
      grid.positions.push_back({float(x) / kQuads, float(y) / kQuads, 0.0F});
    }
  }
  for (std::uint32_t y = 0; y < kQuads; ++y) {
    for (std::uint32_t x = 0; x < kQuads; ++x) {
      const std::uint32_t corner = y * (kQuads + 1) + x;
      grid.triangles.push_back({corner, corner + 1, corner + kQuads + 2});
      grid.triangles.push_back({corner, corner + kQuads + 2, corner + kQuads + 1});
      grid.source_faces.push_back(y * kQuads + x);
      grid.source_faces.push_back(y * kQuads + x);
    }
  }
  const std::size_t triangle_count = grid.triangles.size();
  Lotus::MeshInstance placements;
  placements.instancer_transforms.emplace();
  for (std::uint32_t y = 0; y < kPlacements; ++y) {
    for (std::uint32_t x = 0; x < kPlacements; ++x) {
      Lotus::Matrix4 placement = Lotus::IdentityMatrix();
      placement[12] = float(x);
      placement[13] = float(y);
      placements.instancer_transforms->push_back(placement);
    }
  }

  SceneTimestampVerdict verdict;
  Lotus::RenderWorld world;
  Lotus::SceneExtraction extraction;
  std::ostringstream summary;
  summary << std::fixed << std::setprecision(4) << triangle_count
          << " triangles x " << kPlacements * kPlacements << " placements:";
  enum class Phase { Skipped, Runs, Finite };
  // Returns the step's failure, or an empty string.
  const auto step = [&](const char* name, Phase upload, Phase blas,
                        Phase tlas) -> std::string {
    const Lotus::GpuSceneEvidence evidence =
        renderer.UpdateScene(extraction.Update(world.Commit()));
    const std::string prefix = std::string(name) + ": ";
    if (evidence.status != Lotus::FrameStatus::Pass) {
      return prefix + evidence.detail;
    }
    verdict.available = evidence.stats.timestamps_available;
    if (!verdict.available) {
      return {};
    }
    // Without acceleration structures only the copies run.
    if (!evidence.stats.acceleration_available) {
      blas = Phase::Skipped;
      tlas = Phase::Skipped;
    }
    const Lotus::GpuSceneTimings& timings = evidence.timings;
    const bool submitted =
        upload != Phase::Skipped || blas != Phase::Skipped || tlas != Phase::Skipped;
    if (timings.available != submitted) {
      return prefix + (submitted ? "the submission was not measured"
                                 : "timings were reported without a submission");
    }
    const auto check = [&](Phase phase, double milliseconds,
                           const char* field) -> std::string {
      const bool ok = phase == Phase::Skipped ? milliseconds == 0.0
                      : phase == Phase::Runs  ? std::isfinite(milliseconds) && milliseconds > 0.0
                                              : std::isfinite(milliseconds) && milliseconds >= 0.0;
      if (ok) {
        return {};
      }
      return prefix + field + "=" + std::to_string(milliseconds) +
             (phase == Phase::Skipped ? " for a phase that did not run"
                                      : " is not a positive duration");
    };
    for (auto failure : {check(upload, timings.upload_gpu_ms, "upload_gpu_ms"),
             check(blas, timings.blas_build_gpu_ms, "blas_build_gpu_ms"),
             check(tlas, timings.tlas_build_gpu_ms, "tlas_build_gpu_ms")}) {
      if (!failure.empty()) {
        return failure;
      }
    }
    if (submitted) {
      summary << ' ' << name << " upload_gpu_ms=" << timings.upload_gpu_ms
              << " blas_build_gpu_ms=" << timings.blas_build_gpu_ms
              << " tlas_build_gpu_ms=" << timings.tlas_build_gpu_ms << ';';
    }
    return {};
  };

  world.SetMesh("/grid", std::move(grid), placements);
  verdict.failure = step("insertion", Phase::Runs, Phase::Runs, Phase::Runs);
  if (verdict.failure.empty() && verdict.available) {
    // Moving every placement refits the TLAS.
    for (Lotus::Matrix4& placement : *placements.instancer_transforms) {
      placement[14] = 0.5F;
    }
    world.SetMeshInstance("/grid", placements);
    verdict.failure =
        step("transform edit", Phase::Runs, Phase::Skipped, Phase::Runs);
  }
  if (verdict.failure.empty() && verdict.available) {
    Lotus::Material material;
    material.base_color = {0.25F, 0.5F, 0.75F};
    world.SetMaterial("/grid/paint", material);
    world.BindMaterial("/grid", "/grid/paint");
    verdict.failure =
        step("material binding", Phase::Runs, Phase::Skipped, Phase::Skipped);
  }
  if (verdict.failure.empty() && verdict.available) {
    Lotus::Material material;
    material.base_color = {0.75F, 0.5F, 0.25F};
    world.SetMaterial("/grid/paint", material);
    verdict.failure =
        step("material edit", Phase::Runs, Phase::Skipped, Phase::Skipped);
  }
  if (verdict.failure.empty() && verdict.available) {
    verdict.failure = step("unchanged commit", Phase::Skipped, Phase::Skipped,
        Phase::Skipped);
  }
  // Leaves the GPU scene empty for the checks after this one.
  world.RemoveMesh("/grid");
  if (const std::string removal = step("removal", Phase::Skipped,
          Phase::Skipped, Phase::Finite);
      verdict.failure.empty()) {
    verdict.failure = removal;
  }
  verdict.detail = verdict.available
                       ? summary.str()
                       : "the graphics queue has no timestamp support";
  return verdict;
}

// The scene passes' RGBA32F colour product, four floats per pixel.
std::vector<float> ColorValues(const Lotus::ColorProduct& color) {
  std::vector<float> values(color.payload.size() / sizeof(float));
  std::memcpy(values.data(), color.payload.data(), values.size() * sizeof(float));
  return values;
}

std::array<double, 4> Transform(const Lotus::Matrix4& matrix,
    const std::array<double, 4>& point) {
  std::array<double, 4> result{};
  for (int row = 0; row < 4; ++row)
    for (int column = 0; column < 4; ++column)
      result[row] += matrix[column * 4 + row] * point[column];
  return result;
}

// Independent oracle: project CPU triangles, intersect in screen space,
// perspective-correct the barycentrics and select the nearest window depth.
// This uses neither GPU readback geometry nor the shader's ray construction.
std::string PrimaryImageFailure(const Lotus::FrameSnapshot& snapshot,
    const Lotus::OffscreenTarget& target, const Lotus::GpuFrameEvidence& frame) {
  if (frame.status != Lotus::FrameStatus::Pass)
    return frame.detail;
  if (frame.color.pixel_format != "rgba32-sfloat" ||
      frame.color.row_pitch != target.width * 16U ||
      frame.color.payload.size() != std::size_t{target.width} * target.height * 16 ||
      frame.depth.payload.size() != std::size_t{target.width} * target.height)
    return "incorrect primary-ray product format or size";
  const std::vector<float> values = ColorValues(frame.color);
  const auto camera = Lotus::ExtractDrawSummary(snapshot).world_to_clip;
  struct Triangle {
    std::array<std::array<double, 4>, 3> clip;
  };
  std::vector<Triangle> triangles;
  for (const auto& [key, mesh] : snapshot.scene->meshes) {
    (void)key;
    if (!mesh.instance.visible)
      continue;
    // Instancer transforms apply after the mesh's own, in double precision
    // rather than through the core's composed matrices.
    std::vector<const Lotus::Matrix4*> instancer;
    if (mesh.instance.instancer_transforms) {
      for (const auto& transform : *mesh.instance.instancer_transforms)
        instancer.push_back(&transform);
    } else {
      instancer.push_back(nullptr);
    }
    for (const Lotus::Matrix4* placement : instancer)
      for (const auto& indices : mesh.geometry->triangles) {
        Triangle triangle;
        bool clipped = false;
        for (int corner = 0; corner < 3; ++corner) {
          const auto& position = mesh.geometry->positions[indices[corner]];
          auto world = Transform(mesh.instance.world_from_object,
              {position[0], position[1], position[2], 1.0});
          if (placement != nullptr)
            world = Transform(*placement, world);
          auto clip = Transform(camera, world);
          // Fixtures use triangles wholly inside or outside the depth range.
          if (clip[3] <= 0 || clip[2] < -clip[3] || clip[2] > clip[3])
            clipped = true;
          triangle.clip[corner] = clip;
        }
        if (!clipped)
          triangles.push_back(triangle);
      }
  }
  const auto& display = target.display_window;
  const double dx = display[2] > 0 ? display[0] : 0;
  const double dy = display[3] > 0 ? display[1] : 0;
  const double dw = display[2] > 0 ? display[2] : target.width;
  const double dh = display[3] > 0 ? display[3] : target.height;
  for (std::uint32_t y = 0; y < target.height; ++y) {
    for (std::uint32_t x = 0; x < target.width; ++x) {
      const double px = 2 * (x + 0.5 - dx) / dw - 1;
      const double py = 1 - 2 * (y + 0.5 - dy) / dh;
      double depth = target.clear_depth;
      std::array<double, 4> color{target.clear_color[0], target.clear_color[1],
          target.clear_color[2], target.clear_color[3]};
      bool boundary = false;
      const auto& data = target.data_window;
      const bool inside = (data[2] <= 0 || data[3] <= 0) ||
                          (static_cast<std::int64_t>(x) >= data[0] && static_cast<std::int64_t>(y) >= data[1] &&
                              static_cast<std::int64_t>(x) < std::int64_t{data[0]} + data[2] &&
                              static_cast<std::int64_t>(y) < std::int64_t{data[1]} + data[3]);
      if (inside)
        for (const auto& triangle : triangles) {
          double sx[3], sy[3];
          for (int corner = 0; corner < 3; ++corner) {
            sx[corner] = triangle.clip[corner][0] / triangle.clip[corner][3];
            sy[corner] = triangle.clip[corner][1] / triangle.clip[corner][3];
          }
          const double det = (sy[1] - sy[2]) * (sx[0] - sx[2]) +
                             (sx[2] - sx[1]) * (sy[0] - sy[2]);
          if (std::abs(det) < 1e-12)
            continue;
          double weights[3];
          weights[0] = ((sy[1] - sy[2]) * (px - sx[2]) +
                           (sx[2] - sx[1]) * (py - sy[2])) /
                       det;
          weights[1] = ((sy[2] - sy[0]) * (px - sx[2]) +
                           (sx[0] - sx[2]) * (py - sy[2])) /
                       det;
          weights[2] = 1 - weights[0] - weights[1];
          const double minimum = *std::min_element(weights, weights + 3);
          if (minimum > -0.015 && minimum < 0.015)
            boundary = true;
          if (minimum < 0)
            continue;
          double hit_depth = 0;
          double sum = 0;
          for (int corner = 0; corner < 3; ++corner) {
            hit_depth += weights[corner] *
                         (triangle.clip[corner][2] / triangle.clip[corner][3] + 1) * 0.5;
            sum += weights[corner] / triangle.clip[corner][3];
          }
          if (hit_depth >= depth)
            continue;
          depth = hit_depth;
          for (int corner = 0; corner < 3; ++corner)
            color[corner] = weights[corner] / triangle.clip[corner][3] / sum;
          color[3] = 1;
        }
      if (boundary)
        continue; // Edge coverage precision is device-dependent.
      const std::size_t pixel = std::size_t{y} * target.width + x;
      for (int channel = 0; channel < 4; ++channel) {
        if (std::abs(color[channel] - values[pixel * 4 + channel]) > 2.0 / 255.0)
          return "barycentric/miss mismatch at " + std::to_string(x) + "," + std::to_string(y);
      }
      if (!std::isfinite(frame.depth.payload[pixel]) ||
          std::abs(frame.depth.payload[pixel] - depth) > 2e-5)
        return "projected depth mismatch at " + std::to_string(x) + "," + std::to_string(y);
    }
  }
  return {};
}

std::string PrimaryRayFailure(Lotus::OffscreenRenderer& renderer,
    Lotus::GpuFrameEvidence& last, Lotus::GpuFrameEvidence& measured) {
  Lotus::RenderWorld world;
  world.SetCamera(BootstrapCamera());
  Lotus::MeshGeometry geometry;
  geometry.positions = {{{-0.65F, -0.35F, 0}}, {{0.55F, -0.45F, 0}}, {{0.15F, 0.70F, 0}}};
  geometry.triangles = {{{0, 1, 2}}};
  geometry.source_faces = {0};
  Lotus::MeshInstance instance;
  world.SetMesh("triangle", geometry, instance);
  Lotus::SceneExtraction extraction;
  Lotus::OffscreenTarget target;
  target.width = 64;
  target.height = 64;
  target.clear_color = {0.05F, 0.1F, 0.15F, 0.0F};
  Lotus::PathTracingSettings barycentrics;
  barycentrics.output = Lotus::SceneOutput::Barycentrics;
  const auto check = [&](const char* step) -> std::string {
    const auto snapshot = world.Commit();
    const auto upload = renderer.UpdateScene(extraction.Update(snapshot));
    if (upload.status != Lotus::FrameStatus::Pass)
      return std::string(step) + ": " + upload.detail;
    last = renderer.RenderScene(Lotus::ExtractDrawSummary(snapshot), target, 1,
        barycentrics);
    const auto failure = PrimaryImageFailure(snapshot, target, last);
    if (!failure.empty())
      return std::string(step) + ": " + failure;
    if (last.validation_message_count != 0)
      return std::string(step) + ": " + last.validation_detail;
    return {};
  };
  if (auto error = check("perspective insertion"); !error.empty())
    return error;
  const auto first = last;
  measured = first;
  if (auto error = check("unchanged frame"); !error.empty())
    return error;
  if (first.color.payload != last.color.payload || first.depth.payload != last.depth.payload ||
      first.target_creations != last.target_creations)
    return "unchanged primary frame differs";
  instance.world_from_object[0] = 0.8F;
  instance.world_from_object[5] = 1.2F;
  instance.world_from_object[4] = 0.2F;
  instance.world_from_object[12] = 0.3F;
  instance.world_from_object[13] = -0.15F;
  world.SetMeshInstance("triangle", instance);
  if (auto error = check("transform refit"); !error.empty())
    return error;
  instance.visible = false;
  world.SetMeshInstance("triangle", instance);
  if (auto error = check("hidden mesh"); !error.empty())
    return error;
  instance.visible = true;
  geometry.positions[2][0] = -0.25F;
  geometry.positions[2][2] = 0.3F;
  world.SetMesh("triangle", geometry, instance);
  if (auto error = check("point edit and show"); !error.empty())
    return error;
  Lotus::MeshInstance closer;
  closer.world_from_object[14] = 0.65F;
  world.SetMesh("occluder", geometry, closer);
  if (auto error = check("closest of overlapping instances"); !error.empty())
    return error;
  closer.world_from_object[14] = 2.5F;
  world.SetMeshInstance("occluder", closer);
  if (auto error = check("near-plane exclusion"); !error.empty())
    return error;
  closer.world_from_object[14] = -8.0F;
  world.SetMeshInstance("occluder", closer);
  if (auto error = check("far-plane exclusion"); !error.empty())
    return error;
  world.RemoveMesh("occluder");
  // Instancer placements of one geometry, one of them rotated and scaled
  // and one in front of another.
  Lotus::MeshInstance instanced = instance;
  Lotus::Matrix4 left = Lotus::IdentityMatrix();
  left[12] = -0.45F;
  left[13] = 0.3F;
  Lotus::Matrix4 turned{0, 0.6F, 0, 0, -0.6F, 0, 0, 0, 0, 0, 0.6F, 0,
      0.5F, -0.2F, 0, 1};
  Lotus::Matrix4 front = Lotus::IdentityMatrix();
  front[12] = -0.2F;
  front[14] = 0.4F;
  instanced.instancer_transforms = std::vector<Lotus::Matrix4>{left, turned, front};
  world.SetMeshInstance("triangle", instanced);
  if (auto error = check("instancer placements"); !error.empty())
    return error;
  world.SetMeshInstance("triangle", instance);
  auto infinite = BootstrapCamera();
  infinite.projection[10] = -1.0F;
  infinite.projection[14] = -2.0F;
  world.SetCamera(infinite);
  if (auto error = check("infinite far-plane camera"); !error.empty())
    return error;
  // Orthographic projection with the same view and clipping range.
  auto camera = BootstrapCamera();
  camera.projection = Lotus::IdentityMatrix();
  camera.projection[0] = 0.8F;
  camera.projection[5] = 0.8F;
  camera.projection[10] = -2.0F / 9.0F;
  camera.projection[14] = -11.0F / 9.0F;
  world.SetCamera(camera);
  if (auto error = check("orthographic camera"); !error.empty())
    return error;
  target.display_window = {-8.0F, 5.0F, 80.0F, 50.0F};
  target.data_window = {16, 8, 32, 40};
  if (auto error = check("display and data windows"); !error.empty())
    return error;
  target.width = 96;
  target.height = 48;
  target.display_window = {};
  target.data_window = {};
  if (auto error = check("target resize"); !error.empty())
    return error;
  // A rejected camera must not poison the renderer.
  auto singular = Lotus::ExtractDrawSummary(world.Commit());
  singular.world_to_clip = {};
  if (renderer.RenderScene(singular, target, 1, barycentrics).status !=
      Lotus::FrameStatus::Fail)
    return "singular camera was accepted";
  world.RemoveMesh("triangle");
  if (auto error = check("removal and empty scene"); !error.empty())
    return error;
  target.clear_color_enabled = false;
  target.clear_depth_enabled = false;
  const auto preserved = renderer.RenderScene(
      Lotus::ExtractDrawSummary(world.Commit()), target, 1, barycentrics);
  if (preserved.status != Lotus::FrameStatus::Pass || preserved.color.payload != last.color.payload ||
      preserved.depth.payload != last.depth.payload)
    return "empty-scene no-clear preservation differs";
  last = preserved;
  return {};
}

// A perspective camera at the origin looking down -Z: 90 degree vertical
// field of view, square aspect, clipping range [0.1, 10].
Lotus::Camera WideCamera() {
  constexpr float kNear = 0.1F;
  constexpr float kFar = 10.0F;
  Lotus::Camera camera;
  camera.projection = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
      0.0F, 0.0F, (kFar + kNear) / (kNear - kFar), -1.0F,
      0.0F, 0.0F, 2.0F * kFar * kNear / (kNear - kFar), 0.0F};
  return camera;
}

// Linear RGB radiance.
using Rgb = std::array<double, 3>;

// The GGX metal's directional albedo for view angle `cos_view`, the integral
// of f * cos over the hemisphere. A midpoint rule over the GGX distribution's
// inverse CDF in half-vector space, independent of the shader's visible-
// normal sampling: with h distributed as D(h) cos(theta_h),
// E = integral of F G2 (wo.h) / (cos_view cos(theta_h)).
Rgb GgxAlbedo(const Rgb& f0, double roughness, double cos_view, double f90 = 1.0) {
  constexpr int kSteps = 1024;
  constexpr double kPi = 3.14159265358979323846;
  const double alpha = std::max(roughness * roughness, 1.0e-3);
  const double wo[3] = {std::sqrt(1.0 - cos_view * cos_view), 0.0, cos_view};
  const auto lambda = [&](const double* w) {
    const double tan2 = (1.0 - w[2] * w[2]) / (w[2] * w[2]);
    return 0.5 * (std::sqrt(1.0 + alpha * alpha * tan2) - 1.0);
  };
  Rgb sum{};
  for (int i = 0; i < kSteps; ++i) {
    const double xi = (i + 0.5) / kSteps;
    const double theta = std::atan(alpha * std::sqrt(xi / (1.0 - xi)));
    for (int j = 0; j < kSteps; ++j) {
      const double phi = 2.0 * kPi * (j + 0.5) / kSteps;
      const double h[3] = {std::sin(theta) * std::cos(phi),
          std::sin(theta) * std::sin(phi), std::cos(theta)};
      const double cos_oh = wo[0] * h[0] + wo[1] * h[1] + wo[2] * h[2];
      const double wi[3] = {2 * cos_oh * h[0] - wo[0], 2 * cos_oh * h[1] - wo[1],
          2 * cos_oh * h[2] - wo[2]};
      if (cos_oh <= 0.0 || wi[2] <= 0.0)
        continue;
      const double g2 = 1.0 / (1.0 + lambda(wo) + lambda(wi));
      const double schlick = std::pow(1.0 - cos_oh, 5.0);
      const double common = g2 * cos_oh / (cos_view * h[2]);
      for (int c = 0; c < 3; ++c)
        sum[c] += (f0[c] + (f90 - f0[c]) * schlick) * common;
    }
  }
  for (double& value : sum)
    value /= double{kSteps} * kSteps;
  return sum;
}

// Independent hemispherical integral: GGX uses half-vector quadrature;
// the separable diffuse coat integrates analytically, since the cosine-
// weighted average of (1-cos(theta))^5 is 1/21.
Rgb SurfaceAlbedo(const Lotus::Material& material, double cos_view) {
  const double m = material.use_specular_workflow ? 0.0 : material.metallic;
  const double ratio = (double{material.ior} - 1) / (double{material.ior} + 1);
  const double edge = material.use_specular_workflow || material.ior > 1 ? 1 : 0;
  Rgb dielectric{}, f0{};
  for (int c = 0; c < 3; ++c) {
    dielectric[c] = material.use_specular_workflow ? material.specular_color[c] : ratio * ratio;
    f0[c] = m * material.base_color[c] + (1 - m) * dielectric[c];
  }
  Rgb result = GgxAlbedo(f0, material.roughness, cos_view, m + (1 - m) * edge);
  for (int c = 0; c < 3; ++c) {
    const double exit = 1 - dielectric[c] - (edge - dielectric[c]) * std::pow(1 - cos_view, 5);
    const double entry = 1 - dielectric[c] - (edge - dielectric[c]) / 21;
    result[c] += material.base_color[c] * (1 - m) * exit * entry;
  }
  return result;
}

// The path-tracing scenes' world, camera and target, and the GPU scene
// they are uploaded to.
class PathScenes {
public:
  explicit PathScenes(Lotus::OffscreenRenderer& renderer)
      : renderer_(renderer), target_(DefaultTarget()) {
  }

  Lotus::RenderWorld& World() {
    return world_;
  }

  Lotus::OffscreenTarget& Target() {
    return target_;
  }

  // Applies the world's update plan and renders it, adding `frames`
  // samples to a Radiance accumulation.
  std::string Render(const Lotus::PathTracingSettings& settings,
      Lotus::GpuFrameEvidence& frame, std::uint32_t frames = 1) {
    const auto snapshot = world_.Commit();
    const auto upload = renderer_.UpdateScene(extraction_.Update(snapshot));
    if (upload.status != Lotus::FrameStatus::Pass)
      return upload.detail;
    frame = renderer_.RenderScene(Lotus::ExtractDrawSummary(snapshot), target_,
        frames, settings);
    if (frame.status != Lotus::FrameStatus::Pass)
      return frame.detail;
    if (frame.validation_message_count != 0)
      return frame.validation_detail;
    if (frame.color.pixel_format != "rgba32-sfloat" ||
        frame.color.payload.size() != std::size_t{target_.width} * target_.height * 16)
      return "the scene colour product is not RGBA32F at the target's size";
    return {};
  }

  // Renders a fresh accumulation of `samples` per pixel, in which a pixel
  // whose samples all hit has `expected` radiance. A pixel a fraction `a` of
  // whose samples hit is a * expected + (1 - a) * clear with alpha a, and
  // one none of whose samples hit keeps the clear colour. Depth is the
  // barycentric pass's.
  std::string ExpectExact(std::uint32_t max_bounces, const Rgb& expected) {
    constexpr std::uint32_t kSamples = 4;
    Lotus::PathTracingSettings settings;
    settings.output = Lotus::SceneOutput::Barycentrics;
    Lotus::GpuFrameEvidence coverage;
    Lotus::GpuFrameEvidence radiance;
    if (auto failure = Render(settings, coverage); !failure.empty())
      return failure;
    settings.output = Lotus::SceneOutput::Radiance;
    settings.max_bounces = max_bounces;
    if (auto failure = Render(settings, radiance, kSamples); !failure.empty())
      return failure;
    if (radiance.samples_per_pixel != kSamples)
      return "the accumulation holds " + std::to_string(radiance.samples_per_pixel) +
             " samples instead of " + std::to_string(kSamples);
    if (radiance.depth.payload != coverage.depth.payload)
      return "radiance depth differs from the barycentric pass";
    const std::vector<float> values = ColorValues(radiance.color);
    std::size_t hits = 0;
    for (std::size_t pixel = 0; pixel * 4 < values.size(); ++pixel) {
      const float* actual = &values[pixel * 4];
      const double alpha = actual[3];
      const double hit_samples = alpha * kSamples;
      if (std::abs(hit_samples - std::round(hit_samples)) > 1e-4 || alpha < 0 || alpha > 1)
        return "alpha " + std::to_string(alpha) + " is not a fraction of " +
               std::to_string(kSamples) + " samples at pixel " + std::to_string(pixel);
      if (alpha == 0) {
        if (!std::equal(actual, actual + 4, target_.clear_color.begin()))
          return "a pixel no sample hit changed at " + std::to_string(pixel);
        continue;
      }
      hits += alpha == 1 ? 1 : 0;
      for (int c = 0; c < 3; ++c) {
        const double want = alpha * expected[c] + (1 - alpha) * target_.clear_color[c];
        if (std::abs(want - actual[c]) > 1e-6 + 1e-4 * std::abs(want))
          return "radiance " + std::to_string(actual[c]) + " instead of " +
                 std::to_string(want) + " in channel " + std::to_string(c) +
                 " at pixel " + std::to_string(pixel);
      }
    }
    return hits == 0 ? "no pixel's samples all hit" : "";
  }

  // Renders a fresh accumulation of `count` samples per pixel. The mean
  // radiance over every pixel whose samples all hit is `expected` within
  // five standard errors, estimated from the pixels' means. Each comparison
  // is added to Summary().
  std::string ExpectMean(const char* name, std::uint32_t count,
      const Rgb& expected) {
    Lotus::PathTracingSettings settings;
    Lotus::GpuFrameEvidence frame;
    if (auto failure = Render(settings, frame, count); !failure.empty())
      return failure;
    if (frame.samples_per_pixel != count)
      return "the accumulation holds " + std::to_string(frame.samples_per_pixel) +
             " samples instead of " + std::to_string(count);
    const std::vector<float> values = ColorValues(frame.color);
    Rgb sum{};
    Rgb squares{};
    double pixels = 0;
    for (std::size_t pixel = 0; pixel * 4 < values.size(); ++pixel) {
      if (values[pixel * 4 + 3] != 1.0F)
        continue;
      pixels += 1;
      for (int c = 0; c < 3; ++c) {
        const double value = values[pixel * 4 + c];
        if (!std::isfinite(value)) return "non-finite radiance in a mean comparison";
        sum[c] += value;
        squares[c] += value * value;
      }
    }
    if (pixels < 2)
      return "no pixel's samples all hit";
    std::ostringstream line;
    line << std::fixed << std::setprecision(4) << name << ": "
         << static_cast<long long>(pixels) * count << " samples";
    for (int c = 0; c < 3; ++c) {
      const double mean = sum[c] / pixels;
      const double variance =
          std::max(0.0, (squares[c] - pixels * mean * mean) / (pixels - 1));
      const double tolerance = 5.0 * std::sqrt(variance / pixels) + 1e-6;
      line << (c == 0 ? ", mean " : " ") << mean << '/' << expected[c] << "+-"
           << tolerance;
      if (std::abs(mean - expected[c]) > tolerance)
        return "mean " + std::to_string(mean) + " instead of " +
               std::to_string(expected[c]) + " +- " + std::to_string(tolerance) +
               " in channel " + std::to_string(c);
    }
    Summarize(line.str());
    return {};
  }

  void Summarize(const std::string& line) {
    summary_ += (summary_.empty() ? "" : "; ") + line;
  }

  // The mean comparisons since the last call: measured/expected+-tolerance.
  std::string TakeSummary() {
    return std::exchange(summary_, {});
  }

  // Removes every mesh, material and texture and the environment from the
  // world and the GPU scene, and restores the target.
  std::string Clear() {
    const auto snapshot = world_.Commit();
    for (const auto& [key, mesh] : snapshot.scene->meshes) {
      (void)mesh;
      world_.RemoveMesh(key);
    }
    for (const auto& [key, material] : snapshot.scene->materials) {
      (void)material;
      world_.RemoveMaterial(key);
    }
    for (const auto& [key, texture] : snapshot.scene->textures) {
      (void)texture;
      world_.RemoveTexture(key);
    }
    world_.SetEnvironment({});
    target_ = DefaultTarget();
    const auto upload = renderer_.UpdateScene(extraction_.Update(world_.Commit()));
    return upload.status == Lotus::FrameStatus::Pass ? "" : upload.detail;
  }

private:
  static Lotus::OffscreenTarget DefaultTarget() {
    Lotus::OffscreenTarget target;
    target.width = 64;
    target.height = 64;
    target.clear_color = {0.05F, 0.1F, 0.15F, 0.0F};
    return target;
  }

  Lotus::OffscreenRenderer& renderer_;
  Lotus::RenderWorld world_;
  Lotus::SceneExtraction extraction_;
  Lotus::OffscreenTarget target_;
  std::string summary_;
};

// Single-scattering scenes whose radiance is known: a lone triangle, which
// no reflected ray can hit again, and a large tilted plane seen by an
// orthographic camera, so every pixel has the same view angle.
std::string BsdfFailure(PathScenes& scenes) {
  Lotus::RenderWorld& world = scenes.World();
  world.SetCamera(BootstrapCamera());
  Lotus::MeshGeometry triangle;
  triangle.positions = {{{-0.65F, -0.35F, 0}}, {{0.55F, -0.45F, 0}}, {{0.15F, 0.70F, 0}}};
  triangle.triangles = {{{0, 1, 2}}};
  triangle.source_faces = {0};
  Lotus::MeshInstance front;
  world.SetMesh("/panel", triangle, front);
  Lotus::Material lambert;
  lambert.ior = 1;
  lambert.base_color = {0.5F, 0.25F, 0.75F};
  lambert.emission = {0.1F, 0.05F, 0.0F};
  Paint(world, "/panel", lambert);
  world.SetEnvironment({0.8F, 0.6F, 0.4F});
  // Lambert under a constant environment reflects albedo * environment for
  // any sampled direction; emission adds to it.
  const Rgb lit{0.5 * 0.8 + 0.1, 0.25 * 0.6 + 0.05, 0.75 * 0.4};
  if (auto failure = scenes.ExpectExact(64, lit); !failure.empty())
    return "Lambert front face: " + failure;
  if (auto failure = scenes.ExpectExact(0, {0.1, 0.05, 0.0}); !failure.empty())
    return "emission without bounces: " + failure;
  // Turned half a revolution about Y, the camera sees the other side.
  Lotus::MeshInstance back;
  back.world_from_object[0] = -1.0F;
  back.world_from_object[10] = -1.0F;
  world.SetMeshInstance("/panel", back);
  if (auto failure = scenes.ExpectExact(64, lit); !failure.empty())
    return "Lambert back face: " + failure;
  world.RemoveMesh("/panel");

  // An orthographic camera looking down -Z at a plane whose normal is
  // tilted 60 degrees towards -Y, so cos(theta_o) = 0.5 at every pixel.
  Lotus::Camera ortho = BootstrapCamera();
  ortho.projection = Lotus::IdentityMatrix();
  ortho.projection[10] = -2.0F / 9.0F;
  ortho.projection[14] = -11.0F / 9.0F;
  world.SetCamera(ortho);
  world.SetEnvironment({1.0F, 1.0F, 1.0F});
  Lotus::MeshGeometry plane;
  plane.positions = {{{-4, -4, 0}}, {{4, -4, 0}}, {{4, 4, 0}}, {{-4, 4, 0}}};
  plane.triangles = {{{0, 1, 2}}, {{0, 2, 3}}};
  plane.source_faces = {0, 0};
  Lotus::MeshInstance tilted;
  const float cos_tilt = 0.5F;
  const float sin_tilt = 0.866025404F;
  tilted.world_from_object[5] = cos_tilt;
  tilted.world_from_object[6] = sin_tilt;
  tilted.world_from_object[9] = -sin_tilt;
  tilted.world_from_object[10] = cos_tilt;
  world.SetMesh("/plane", plane, tilted);
  struct Case {
    const char* name;
    Rgb base;
    float roughness;
    float metallic;
  };
  const Case cases[] = {{"GGX metal, roughness 0.5", {1.0, 0.6, 0.2}, 0.5F, 1.0F},
      {"GGX metal, roughness 0.25", {0.9, 0.5, 0.1}, 0.25F, 1.0F},
      {"half-metallic mixture", {0.9, 0.6, 0.3}, 0.5F, 0.5F}};
  for (const Case& entry : cases) {
    Lotus::Material material;
    for (int c = 0; c < 3; ++c)
      material.base_color[c] = static_cast<float>(entry.base[c]);
    material.roughness = entry.roughness;
    material.metallic = entry.metallic;
    Paint(world, "/plane", material);
    const Rgb expected = SurfaceAlbedo(material, cos_tilt);
    if (auto failure = scenes.ExpectMean(entry.name, 16, expected); !failure.empty())
      return std::string(entry.name) + ": " + failure;
  }
  // Normal and grazing views, dark and white bases, two indices, rough
  // and near-mirror lobes, coloured F0 and metallic ignored by the specular
  // workflow. White furnace values are also bounded by incident radiance.
  for (const float cos_view : {1.0F, 0.5F, 0.125F}) {
    tilted.world_from_object[5] = cos_view;
    tilted.world_from_object[6] = std::sqrt(1 - cos_view * cos_view);
    tilted.world_from_object[9] = -tilted.world_from_object[6];
    tilted.world_from_object[10] = cos_view;
    // Enlarge so the grazing plane still covers the whole view.
    auto large = plane;
    for (auto& p : large.positions)
      for (float& v : p)
        v *= 4;
    world.SetMesh("/plane", large, tilted);
    for (int mode = 0; mode < 6; ++mode) {
      Lotus::Material material;
      material.base_color = mode % 2 == 0 ? std::array<float, 3>{0, 0, 0} : std::array<float, 3>{1, 1, 1};
      material.ior = mode == 2 ? 2.5F : 1.5F;
      material.roughness = mode == 0 ? 0 : (mode == 1 ? 1 : 0.5F);
      material.use_specular_workflow = mode >= 3;
      material.specular_color = mode == 4 ? std::array<float, 3>{0, 0, 0} :
          (mode == 5 ? std::array<float, 3>{1, 1, 1} : std::array<float, 3>{0.7F, 0.2F, 0.05F});
      material.metallic = mode >= 3 ? 1.0F : 0.0F;
      Paint(world, "/plane", material);
      const Rgb expected = SurfaceAlbedo(material, cos_view);
      for (double value : expected)
        if (value > 1.00001)
          return "white furnace integral exceeds one";
      const std::string name = "dielectric mode " + std::to_string(mode) + " cos " + std::to_string(cos_view);
      if (auto failure = scenes.ExpectMean(name.c_str(), 32, expected); !failure.empty())
        return name + ": " + failure;
    }
  }
  return {};
}

using Vec3 = std::array<double, 3>;

Vec3 Subtract(const Vec3& a, const Vec3& b) {
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

double Dot(const Vec3& a, const Vec3& b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3 Cross(const Vec3& a, const Vec3& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
      a[0] * b[1] - a[1] * b[0]};
}

Vec3 Normalize(const Vec3& a) {
  const double length = std::sqrt(Dot(a, a));
  return {a[0] / length, a[1] / length, a[2] / length};
}

// `matrix` applied to a point (w = 1) or a direction (w = 0).
Vec3 TransformVec3(const Lotus::Matrix4& matrix, const Vec3& value, double w) {
  const auto result = Transform(matrix, {value[0], value[1], value[2], w});
  return {result[0], result[1], result[2]};
}

Lotus::Matrix4 Translation(float x, float y, float z) {
  Lotus::Matrix4 matrix = Lotus::IdentityMatrix();
  matrix[12] = x;
  matrix[13] = y;
  matrix[14] = z;
  return matrix;
}

Lotus::Matrix4 Scaling(float x, float y, float z) {
  Lotus::Matrix4 matrix = Lotus::IdentityMatrix();
  matrix[0] = x;
  matrix[5] = y;
  matrix[10] = z;
  return matrix;
}

// Right-handed rotations by `degrees` about the X and Y axes.
Lotus::Matrix4 RotationX(double degrees) {
  const auto c = static_cast<float>(std::cos(degrees * 3.14159265358979323846 / 180));
  const auto s = static_cast<float>(std::sin(degrees * 3.14159265358979323846 / 180));
  Lotus::Matrix4 matrix = Lotus::IdentityMatrix();
  matrix[5] = c;
  matrix[6] = s;
  matrix[9] = -s;
  matrix[10] = c;
  return matrix;
}

Lotus::Matrix4 RotationY(double degrees) {
  const auto c = static_cast<float>(std::cos(degrees * 3.14159265358979323846 / 180));
  const auto s = static_cast<float>(std::sin(degrees * 3.14159265358979323846 / 180));
  Lotus::Matrix4 matrix = Lotus::IdentityMatrix();
  matrix[0] = c;
  matrix[2] = -s;
  matrix[8] = s;
  matrix[10] = c;
  return matrix;
}

// A square of side 2 in the XY plane, as two triangles.
Lotus::MeshGeometry Square() {
  Lotus::MeshGeometry square;
  square.positions = {{{-1, -1, 0}}, {{1, -1, 0}}, {{1, 1, 0}}, {{-1, 1, 0}}};
  square.triangles = {{{0, 1, 2}}, {{0, 2, 3}}};
  square.source_faces = {0, 0};
  return square;
}

// Each triangle corner takes its vertex's normal.
void SetVertexNormals(Lotus::MeshGeometry& geometry,
    const std::vector<std::array<float, 3>>& vertex_normals) {
  geometry.normals.clear();
  for (const auto& triangle : geometry.triangles)
    for (const std::uint32_t index : triangle)
      geometry.normals.push_back(vertex_normals[index]);
}

// An orthographic camera looking down -Z from z = 3, seeing x and y in
// [-1, 1] and z in [-7, 2].
Lotus::Camera OrthographicCamera() {
  Lotus::Camera camera = BootstrapCamera();
  camera.projection = Lotus::IdentityMatrix();
  camera.projection[10] = -2.0F / 9.0F;
  camera.projection[14] = -11.0F / 9.0F;
  return camera;
}

// Independent oracle for the ShadingNormal output of OrthographicCamera's
// view: intersects each pixel centre's ray with the world-space triangles in
// double precision and applies the shading-normal rules to the hit. Pixels
// whose outcome a rounding error could change are skipped: near triangle
// edges and where the interpolated normal is nearly tangent to the
// geometric surface or to the ray. Counts the pixels of each rule.
std::string ShadingNormalFailure(const Lotus::FrameSnapshot& snapshot,
    const Lotus::OffscreenTarget& target, const Lotus::GpuFrameEvidence& frame,
    std::string& summary) {
  const std::vector<float> values = ColorValues(frame.color);
  const Vec3 direction{0, 0, -1};
  std::size_t geometric = 0;
  std::size_t interpolated = 0;
  std::size_t flipped = 0;
  std::size_t fallback = 0;
  for (std::uint32_t y = 0; y < target.height; ++y) {
    for (std::uint32_t x = 0; x < target.width; ++x) {
      const Vec3 origin{2.0 * (x + 0.5) / target.width - 1.0,
          1.0 - 2.0 * (y + 0.5) / target.height, 2.0};
      struct Closest {
        double t = 9.0;
        double u = 0;
        double v = 0;
        std::array<Vec3, 3> corners{};
        const Lotus::SceneMesh* mesh = nullptr;
        std::size_t triangle = 0;
      } closest;
      for (const auto& [key, mesh] : snapshot.scene->meshes) {
        (void)key;
        const auto& geometry = *mesh.geometry;
        for (std::size_t index = 0; index < geometry.triangles.size(); ++index) {
          std::array<Vec3, 3> corners;
          for (int corner = 0; corner < 3; ++corner) {
            const auto& position = geometry.positions[geometry.triangles[index][corner]];
            corners[corner] = TransformVec3(mesh.instance.world_from_object,
                {position[0], position[1], position[2]}, 1.0);
          }
          // Moeller-Trumbore.
          const Vec3 edge1 = Subtract(corners[1], corners[0]);
          const Vec3 edge2 = Subtract(corners[2], corners[0]);
          const Vec3 p = Cross(direction, edge2);
          const double determinant = Dot(edge1, p);
          if (std::abs(determinant) < 1e-12)
            continue;
          const Vec3 s = Subtract(origin, corners[0]);
          const double u = Dot(s, p) / determinant;
          const Vec3 q = Cross(s, edge1);
          const double v = Dot(direction, q) / determinant;
          const double t = Dot(edge2, q) / determinant;
          if (u < 0 || v < 0 || u + v > 1 || t <= 0 || t >= closest.t)
            continue;
          closest = {t, u, v, corners, &mesh, index};
        }
      }
      const std::size_t pixel = std::size_t{y} * target.width + x;
      const float* actual = &values[pixel * 4];
      if (closest.mesh == nullptr) {
        if (!std::equal(actual, actual + 4, target.clear_color.begin()))
          return "a missed pixel changed at " + std::to_string(x) + "," + std::to_string(y);
        continue;
      }
      if (std::min({closest.u, closest.v, 1 - closest.u - closest.v}) < 0.02)
        continue;
      Vec3 normal = Normalize(Cross(Subtract(closest.corners[1], closest.corners[0]),
          Subtract(closest.corners[2], closest.corners[0])));
      if (Dot(normal, direction) > 0)
        normal = {-normal[0], -normal[1], -normal[2]};
      Vec3 expected = normal;
      const auto& normals = closest.mesh->geometry->normals;
      if (normals.empty()) {
        ++geometric;
      } else {
        Vec3 object{};
        for (int axis = 0; axis < 3; ++axis) {
          const auto corner = [&](std::size_t k) {
            return double{normals[3 * closest.triangle + k][axis]};
          };
          object[axis] = corner(0) + closest.u * (corner(1) - corner(0)) +
                         closest.v * (corner(2) - corner(0));
        }
        // The inverse transpose of the linear part, up to a scale.
        const Lotus::Matrix4& m = closest.mesh->instance.world_from_object;
        const Vec3 c0 = TransformVec3(m, {1, 0, 0}, 0.0);
        const Vec3 c1 = TransformVec3(m, {0, 1, 0}, 0.0);
        const Vec3 c2 = TransformVec3(m, {0, 0, 1}, 0.0);
        Vec3 world{};
        for (int axis = 0; axis < 3; ++axis)
          world[axis] = Cross(c1, c2)[axis] * object[0] +
                        Cross(c2, c0)[axis] * object[1] +
                        Cross(c0, c1)[axis] * object[2];
        Vec3 shading = Normalize(world);
        const double side = Dot(shading, normal);
        if (std::abs(side) < 0.02)
          continue;
        if (side < 0)
          shading = {-shading[0], -shading[1], -shading[2]};
        const double facing = Dot(shading, direction);
        if (std::abs(facing) < 0.02)
          continue;
        if (facing > 0) {
          ++fallback;
        } else {
          expected = shading;
          ++(side < 0 ? flipped : interpolated);
        }
      }
      for (int c = 0; c < 3; ++c) {
        if (std::abs(actual[c] - expected[c]) > 2e-3 || actual[3] != 1.0F)
          return "shading normal (" + std::to_string(actual[0]) + ", " +
                 std::to_string(actual[1]) + ", " + std::to_string(actual[2]) +
                 ") instead of (" + std::to_string(expected[0]) + ", " +
                 std::to_string(expected[1]) + ", " + std::to_string(expected[2]) +
                 ") at " + std::to_string(x) + "," + std::to_string(y);
      }
    }
  }
  summary = "shading normals: " + std::to_string(interpolated) +
            " interpolated, " + std::to_string(flipped) + " flipped, " +
            std::to_string(fallback) + " facing away, " +
            std::to_string(geometric) + " geometric pixels";
  if (interpolated == 0 || flipped == 0 || fallback == 0 || geometric == 0)
    return "the scene does not exercise every rule: " + summary;
  return {};
}

// Authored normals through the GpuScene to the path tracer. The shading
// normal diagnostic of interpolated corner normals under non-uniform scales
// and rotations, against an independent oracle; then single-scattering
// scenes whose radiance the shading normal decides.
std::string NormalsFailure(PathScenes& scenes) {
  Lotus::RenderWorld& world = scenes.World();
  world.SetCamera(OrthographicCamera());
  // A tilted, non-uniformly scaled square whose vertex normals vary enough
  // that some point below its surface and some away from the camera; a
  // triangle without normals; and a square turned to show its back.
  Lotus::MeshGeometry curved = Square();
  SetVertexNormals(curved,
      {{-0.6F, -0.6F, 1}, {0.9F, -0.3F, 0.6F}, {0.3F, 0.9F, -0.2F}, {-1.2F, 0.5F, 0.2F}});
  Lotus::MeshInstance curved_instance;
  curved_instance.world_from_object = Lotus::Multiply(Translation(-0.45F, 0, 0),
      Lotus::Multiply(RotationX(40), Scaling(0.5F, 0.8F, 3)));
  world.SetMesh("/curved", curved, curved_instance);
  world.SetMesh("/flat",
      {{{0.2F, -0.9F, 0}, {0.9F, -0.9F, 0}, {0.55F, -0.1F, -0.5F}}, {{0, 1, 2}}, {0}},
      Lotus::MeshInstance{});
  Lotus::MeshGeometry behind = Square();
  SetVertexNormals(behind,
      {{0.3F, 0.2F, 1}, {-0.2F, 0.4F, 1}, {0, -0.5F, 1}, {0.6F, 0, 1}});
  Lotus::MeshInstance behind_instance;
  behind_instance.world_from_object = Lotus::Multiply(Translation(0.55F, 0.5F, 0),
      Lotus::Multiply(RotationY(180), Scaling(0.35F, 0.4F, 1)));
  world.SetMesh("/behind", behind, behind_instance);
  Lotus::PathTracingSettings diagnostic;
  diagnostic.output = Lotus::SceneOutput::ShadingNormal;
  Lotus::GpuFrameEvidence frame;
  if (auto failure = scenes.Render(diagnostic, frame); !failure.empty())
    return "shading normal output: " + failure;
  std::string summary;
  if (auto failure = ShadingNormalFailure(world.Commit(), scenes.Target(), frame, summary);
      !failure.empty())
    return failure;
  scenes.Summarize(summary);
  world.RemoveMesh("/curved");
  world.RemoveMesh("/flat");
  world.RemoveMesh("/behind");

  // A Lambert plane facing the camera under a white environment. With its
  // geometric normal, or authored normals of any length along it, each
  // sample reflects albedo exactly.
  world.SetEnvironment({1.0F, 1.0F, 1.0F});
  Lotus::MeshGeometry plane = Square();
  for (auto& position : plane.positions) {
    position[0] *= 4.0F;
    position[1] *= 4.0F;
  }
  Lotus::Material lambert;
  lambert.base_color = {0.5F, 0.25F, 0.75F};
  lambert.ior = 1;
  const Rgb albedo{0.5, 0.25, 0.75};
  plane.normals.assign(6, {0, 0, 3});
  world.SetMesh("/plane", plane, Lotus::MeshInstance{});
  Paint(world, "/plane", lambert);
  if (auto failure = scenes.ExpectExact(64, albedo); !failure.empty())
    return "Lambert, normals along the geometric normal: " + failure;
  // Tilted by 60 degrees, the cosine-weighted directions around the shading
  // normal that leave above the plane are the tilted plane's sky view
  // factor, (1 + cos 60) / 2; the rest end the path.
  plane.normals.assign(6, {0.866025404F, 0, 0.5F});
  world.SetMesh("/plane", plane, Lotus::MeshInstance{});
  if (auto failure = scenes.ExpectMean("Lambert, shading normal tilted 60 degrees",
          64, {0.75 * albedo[0], 0.75 * albedo[1], 0.75 * albedo[2]});
      !failure.empty())
    return "Lambert, shading normal tilted 60 degrees: " + failure;

  // A white near-mirror under a black environment, beside an emissive wall
  // outside the view. Its geometric normal reflects the camera's rays back
  // up into the dark; shading normals tilted 30 degrees towards the wall
  // reflect them 60 degrees from the vertical onto it.
  world.SetEnvironment({});
  Lotus::Material mirror;
  mirror.base_color = {1.0F, 1.0F, 1.0F};
  mirror.roughness = 0.0F;
  mirror.metallic = 1.0F;
  plane.normals.clear();
  world.SetMesh("/plane", plane, Lotus::MeshInstance{});
  Paint(world, "/plane", mirror);
  Lotus::Material wall;
  wall.base_color = {0.0F, 0.0F, 0.0F};
  wall.ior = 1;
  wall.emission = {2.0F, 1.0F, 0.5F};
  world.SetMesh("/wall",
      {{{6, -20, 0.5F}, {6, 20, 0.5F}, {6, 20, 10}, {6, -20, 10}},
          {{0, 1, 2}, {0, 2, 3}}, {0, 0}},
      Lotus::MeshInstance{});
  Paint(world, "/wall", wall);
  if (auto failure = scenes.ExpectExact(64, {0.0, 0.0, 0.0}); !failure.empty())
    return "mirror, geometric normal: " + failure;
  plane.normals.assign(6, {0.5F, 0, 0.866025404F});
  world.SetMesh("/plane", plane, Lotus::MeshInstance{});
  if (auto failure = scenes.ExpectExact(64, {2.0, 1.0, 0.5}); !failure.empty())
    return "mirror, shading normals tilted 30 degrees: " + failure;
  return {};
}

using Rgba = std::array<double, 4>;

// Texel (i, j) of a texture, row j from the top, in linear values: sRGB
// colour channels are decoded first, as Vulkan does before filtering.
Rgba Texel(const Lotus::Texture& texture, int i, int j) {
  const std::size_t index = std::size_t(j) * texture.width + std::size_t(i);
  Rgba texel{};
  if (texture.format == Lotus::TextureFormat::Rgba32Float) {
    float values[4];
    std::memcpy(values, texture.texels.data() + index * 16, sizeof(values));
    for (int c = 0; c < 4; ++c)
      texel[c] = values[c];
    return texel;
  }
  for (int c = 0; c < 4; ++c) {
    const double value = texture.texels[index * 4 + c] / 255.0;
    texel[c] = texture.format == Lotus::TextureFormat::Rgba8Srgb && c < 3
                   ? (value <= 0.04045 ? value / 12.92
                                       : std::pow((value + 0.055) / 1.055, 2.4))
                   : value;
  }
  return texel;
}

// Vulkan's wrapping of integer texel coordinate `i`, or nothing for the
// transparent black border.
std::optional<int> WrapTexel(int i, int size, Lotus::TextureWrap wrap) {
  switch (wrap) {
  case Lotus::TextureWrap::Repeat:
    return ((i % size) + size) % size;
  case Lotus::TextureWrap::Mirror: {
    const int m = ((i % (2 * size)) + 2 * size) % (2 * size) - size;
    return size - 1 - (m >= 0 ? m : -(1 + m));
  }
  case Lotus::TextureWrap::Clamp:
    return std::clamp(i, 0, size - 1);
  case Lotus::TextureWrap::Black:
    break;
  }
  if (i < 0 || i >= size)
    return std::nullopt;
  return i;
}

// A value the device computes, and how far it may be from it.
struct Expected {
  Rgba value{};
  Rgba tolerance{1e-6, 1e-6, 1e-6, 1e-6};
};

// Independent oracle of a lookup: Vulkan's bilinear filter at level 0 at
// texture coordinates (s, t), whose t runs up from the image's bottom row,
// times scale plus bias; the unscaled fallback without a texture. A device
// with 8 bits of subtexel precision rounds each filter weight to 1/256 of a
// texel, which moves the value by up to 2^-7 of the four texels' spread.
// Devices decode sRGB less precisely than this oracle: up to 0.7% of the
// value on an RTX A5000, so sRGB colour channels allow 1% of the larger
// texel.
Expected LookupOracle(const Lotus::LotusScene& scene,
    const Lotus::TextureInput& input, double s, double t) {
  const auto found = scene.textures.find(input.texture);
  if (found == scene.textures.end())
    return {{input.fallback[0], input.fallback[1], input.fallback[2],
        input.fallback[3]}};
  const Lotus::Texture& texture = *found->second;
  const double u = s * texture.width - 0.5;
  const double v = (1.0 - t) * texture.height - 0.5;
  const int i0 = static_cast<int>(std::floor(u));
  const int j0 = static_cast<int>(std::floor(v));
  const double a = u - i0;
  const double b = v - j0;
  Rgba result{};
  Rgba low{1e30, 1e30, 1e30, 1e30};
  Rgba high{-1e30, -1e30, -1e30, -1e30};
  for (int dj = 0; dj < 2; ++dj) {
    for (int di = 0; di < 2; ++di) {
      const auto i = WrapTexel(i0 + di, static_cast<int>(texture.width), input.wrap_s);
      const auto j = WrapTexel(j0 + dj, static_cast<int>(texture.height), input.wrap_t);
      const Rgba texel = i && j ? Texel(texture, *i, *j) : Rgba{};
      const double weight = (di == 0 ? 1 - a : a) * (dj == 0 ? 1 - b : b);
      for (int c = 0; c < 4; ++c) {
        result[c] += weight * texel[c];
        low[c] = std::min(low[c], texel[c]);
        high[c] = std::max(high[c], texel[c]);
      }
    }
  }
  Expected expected;
  for (int c = 0; c < 4; ++c) {
    expected.value[c] = result[c] * input.scale[c] + input.bias[c];
    const double decoding =
        texture.format == Lotus::TextureFormat::Rgba8Srgb && c < 3
            ? 0.01 * high[c]
            : 0.0;
    expected.tolerance[c] =
        ((high[c] - low[c]) / 128.0 + decoding + 2e-4) *
            std::abs(input.scale[c]) +
        1e-6;
  }
  return expected;
}

// The closest hit of OrthographicCamera's ray through a pixel centre, in
// double precision: the mesh, its triangle and the hit's barycentrics.
struct OrthographicHit {
  const Lotus::SceneMesh* mesh = nullptr;
  std::size_t triangle = 0;
  double u = 0;
  double v = 0;
};

OrthographicHit TraceOrthographic(const Lotus::LotusScene& scene,
    const Lotus::OffscreenTarget& target, std::uint32_t x, std::uint32_t y) {
  const Vec3 origin{2.0 * (x + 0.5) / target.width - 1.0,
      1.0 - 2.0 * (y + 0.5) / target.height, 2.0};
  const Vec3 direction{0, 0, -1};
  OrthographicHit closest;
  double closest_t = 9.0;
  for (const auto& [key, mesh] : scene.meshes) {
    (void)key;
    if (!mesh.instance.visible)
      continue;
    const auto& geometry = *mesh.geometry;
    for (std::size_t index = 0; index < geometry.triangles.size(); ++index) {
      std::array<Vec3, 3> corners;
      for (int corner = 0; corner < 3; ++corner) {
        const auto& position = geometry.positions[geometry.triangles[index][corner]];
        corners[corner] = TransformVec3(mesh.instance.world_from_object,
            {position[0], position[1], position[2]}, 1.0);
      }
      // Moeller-Trumbore.
      const Vec3 edge1 = Subtract(corners[1], corners[0]);
      const Vec3 edge2 = Subtract(corners[2], corners[0]);
      const Vec3 p = Cross(direction, edge2);
      const double determinant = Dot(edge1, p);
      if (std::abs(determinant) < 1e-12)
        continue;
      const Vec3 s = Subtract(origin, corners[0]);
      const double u = Dot(s, p) / determinant;
      const Vec3 q = Cross(s, edge1);
      const double v = Dot(direction, q) / determinant;
      const double t = Dot(edge2, q) / determinant;
      if (u < 0 || v < 0 || u + v > 1 || t <= 0 || t >= closest_t)
        continue;
      closest_t = t;
      closest = {&mesh, index, u, v};
    }
  }
  return closest;
}

// What the Albedo or RoughnessMetallic output shows at a hit, by the texture
// rules applied in double precision.
Expected SurfaceOracle(const Lotus::LotusScene& scene, const OrthographicHit& hit,
    Lotus::SceneOutput output) {
  const auto found = scene.materials.find(hit.mesh->material);
  const Lotus::Material material =
      found == scene.materials.end() ? Lotus::Material{} : found->second;
  std::array<double, 2> st{material.texcoord_fallback[0],
      material.texcoord_fallback[1]};
  const auto set = hit.mesh->geometry->texcoords.find(material.texcoords);
  if (set != hit.mesh->geometry->texcoords.end()) {
    for (int axis = 0; axis < 2; ++axis) {
      const auto corner = [&](std::size_t k) {
        return double{set->second[3 * hit.triangle + k][axis]};
      };
      st[axis] = corner(0) + hit.u * (corner(1) - corner(0)) +
                 hit.v * (corner(2) - corner(0));
    }
  }
  const auto lookup = [&](const Lotus::TextureInput& input) {
    return LookupOracle(scene, input, st[0], st[1]);
  };
  const auto unit = [](double value) { return std::clamp(value, 0.0, 1.0); };
  Expected expected;
  if (output == Lotus::SceneOutput::Albedo) {
    expected.value = {material.base_color[0], material.base_color[1],
        material.base_color[2], 1.0};
    if (material.base_color_texture) {
      const Expected value = lookup(*material.base_color_texture);
      for (int c = 0; c < 3; ++c) {
        expected.value[c] = unit(value.value[c]);
        expected.tolerance[c] = value.tolerance[c];
      }
    }
    return expected;
  }
  expected.value = {material.roughness, material.metallic, 0.0, 1.0};
  const std::array<const std::optional<Lotus::TextureInput>*, 2> inputs{
      &material.roughness_texture, &material.metallic_texture};
  for (int c = 0; c < 2; ++c) {
    if (*inputs[c]) {
      const Expected value = lookup(**inputs[c]);
      const std::uint32_t channel = (*inputs[c])->channel;
      expected.value[c] = unit(value.value[channel]);
      expected.tolerance[c] = value.tolerance[channel];
    }
  }
  return expected;
}

// Renders a surface diagnostic of OrthographicCamera's view and compares
// each pixel centre with SurfaceOracle, skipping pixels near triangle edges.
// Counts the pixels per mesh.
std::string SurfaceOutputFailure(PathScenes& scenes, Lotus::SceneOutput output,
    std::map<std::string, std::size_t>& pixels) {
  Lotus::PathTracingSettings settings;
  settings.output = output;
  Lotus::GpuFrameEvidence frame;
  if (auto failure = scenes.Render(settings, frame); !failure.empty())
    return failure;
  const auto snapshot = scenes.World().Commit();
  const Lotus::OffscreenTarget& target = scenes.Target();
  const std::vector<float> values = ColorValues(frame.color);
  for (std::uint32_t y = 0; y < target.height; ++y) {
    for (std::uint32_t x = 0; x < target.width; ++x) {
      const OrthographicHit hit = TraceOrthographic(*snapshot.scene, target, x, y);
      const float* actual = &values[(std::size_t{y} * target.width + x) * 4];
      if (hit.mesh == nullptr) {
        if (!std::equal(actual, actual + 4, target.clear_color.begin()))
          return "a missed pixel changed at " + std::to_string(x) + "," + std::to_string(y);
        continue;
      }
      if (std::min({hit.u, hit.v, 1 - hit.u - hit.v}) < 0.02)
        continue;
      const Expected expected = SurfaceOracle(*snapshot.scene, hit, output);
      for (int c = 0; c < 4; ++c) {
        if (std::abs(actual[c] - expected.value[c]) > expected.tolerance[c]) {
          const Rgba& want = expected.value;
          return "(" + std::to_string(actual[0]) + ", " + std::to_string(actual[1]) +
                 ", " + std::to_string(actual[2]) + ", " + std::to_string(actual[3]) +
                 ") instead of (" + std::to_string(want[0]) + ", " +
                 std::to_string(want[1]) + ", " + std::to_string(want[2]) + ", " +
                 std::to_string(want[3]) + ") +- " +
                 std::to_string(expected.tolerance[c]) + " at " +
                 std::to_string(x) + "," + std::to_string(y);
        }
      }
      for (const auto& [key, mesh] : snapshot.scene->meshes)
        if (&mesh == hit.mesh)
          ++pixels[key];
    }
  }
  return {};
}

// A Texture whose texel (i, j), row j from the top, is texel(i, j).
Lotus::Texture MakeTexture(std::uint32_t width, std::uint32_t height,
    Lotus::TextureFormat format,
    const std::function<std::array<double, 4>(std::uint32_t, std::uint32_t)>& texel) {
  Lotus::Texture texture;
  texture.width = width;
  texture.height = height;
  texture.format = format;
  texture.texels.resize(std::size_t{width} * height * Lotus::TexelBytes(format));
  for (std::uint32_t j = 0; j < height; ++j) {
    for (std::uint32_t i = 0; i < width; ++i) {
      const std::array<double, 4> value = texel(i, j);
      const std::size_t index = std::size_t{j} * width + i;
      for (int c = 0; c < 4; ++c) {
        if (format == Lotus::TextureFormat::Rgba32Float) {
          const auto component = static_cast<float>(value[c]);
          std::memcpy(texture.texels.data() + index * 16 + c * 4, &component, 4);
        } else {
          texture.texels[index * 4 + c] =
              static_cast<std::uint8_t>(std::lround(std::clamp(value[c], 0.0, 1.0) * 255));
        }
      }
    }
  }
  return texture;
}

// A square of side 2 placed at `world_from_object`, with one
// texture-coordinate set per entry of `sets`, each of four corner (s, t)
// in the order of its positions.
Lotus::MeshGeometry MappedSquare(
    const std::map<std::string, std::array<std::array<float, 2>, 4>>& sets) {
  Lotus::MeshGeometry square = Square();
  for (const auto& [name, corners] : sets) {
    auto& set = square.texcoords[name];
    for (const auto& triangle : square.triangles)
      for (const std::uint32_t index : triangle)
        set.push_back(corners[index]);
  }
  return square;
}

// A normal-map oracle: solve the world-space UV Jacobian directly in double
// precision, then orthogonalize against the mesh's interpolated normal.
// Unlike the shader this divides by the determinant without rescaling it.
std::string NormalMapDiagnosticFailure(const Lotus::FrameSnapshot& snapshot,
    const Lotus::OffscreenTarget& target, const Lotus::GpuFrameEvidence& frame,
    std::map<std::string, std::size_t>& pixels) {
  const auto values = ColorValues(frame.color);
  for (std::uint32_t y = 0; y < target.height; ++y) {
    for (std::uint32_t x = 0; x < target.width; ++x) {
      const auto hit = TraceOrthographic(*snapshot.scene, target, x, y);
      const float* actual = &values[(std::size_t{y} * target.width + x) * 4];
      if (!hit.mesh) {
        if (!std::equal(actual, actual + 4, target.clear_color.begin()))
          return "normal-map diagnostic changed a missed pixel";
        continue;
      }
      if (std::min({hit.u, hit.v, 1 - hit.u - hit.v}) < 0.02)
        continue;
      const auto& geometry = *hit.mesh->geometry;
      const auto& transform = hit.mesh->instance.world_from_object;
      const auto& triangle = geometry.triangles[hit.triangle];
      std::array<Vec3, 3> positions;
      for (int c = 0; c < 3; ++c) {
        const auto& p = geometry.positions[triangle[c]];
        positions[c] = TransformVec3(transform, {p[0], p[1], p[2]}, 1);
      }
      const Vec3 e1 = Subtract(positions[1], positions[0]);
      const Vec3 e2 = Subtract(positions[2], positions[0]);
      Vec3 geometric = Normalize(Cross(e1, e2));
      if (geometric[2] < 0)
        for (double& c : geometric)
          c = -c;
      Vec3 base = geometric;
      if (!geometry.normals.empty()) {
        Vec3 object{};
        for (int c = 0; c < 3; ++c) {
          const auto n = [&](std::size_t k) {
            return geometry.normals[hit.triangle * 3 + k][c];
          };
          object[c] = n(0) + hit.u * (n(1) - n(0)) + hit.v * (n(2) - n(0));
        }
        const Vec3 a = TransformVec3(transform, {1, 0, 0}, 0);
        const Vec3 b = TransformVec3(transform, {0, 1, 0}, 0);
        const Vec3 c = TransformVec3(transform, {0, 0, 1}, 0);
        Vec3 world{};
        for (int axis = 0; axis < 3; ++axis)
          world[axis] = Cross(b, c)[axis] * object[0] +
                        Cross(c, a)[axis] * object[1] + Cross(a, b)[axis] * object[2];
        if (Dot(world, world) > 0) {
          base = Normalize(world);
          if (Dot(base, geometric) < 0)
            for (double& v : base)
              v = -v;
          if (base[2] <= 0)
            base = geometric;
        }
      }
      const auto& material = snapshot.scene->materials.at(hit.mesh->material);
      // Duff's normal-only frame; fallback cases below have base = +Z,
      // where this is simply the world X/Y frame.
      const double a = -1 / (1 + base[2]);
      Vec3 tangent{1 + base[0] * base[0] * a, base[0] * base[1] * a, -base[0]};
      Vec3 bitangent{base[0] * base[1] * a, 1 + base[1] * base[1] * a, -base[1]};
      std::array<double, 2> st{material.texcoord_fallback[0], material.texcoord_fallback[1]};
      if (const auto set = geometry.texcoords.find(material.texcoords);
          set != geometry.texcoords.end()) {
        const auto& uv = set->second;
        const auto& uv0 = uv[hit.triangle * 3];
        const auto& uv1 = uv[hit.triangle * 3 + 1];
        const auto& uv2 = uv[hit.triangle * 3 + 2];
        const double ds1 = double(uv1[0]) - uv0[0], dt1 = double(uv1[1]) - uv0[1];
        const double ds2 = double(uv2[0]) - uv0[0], dt2 = double(uv2[1]) - uv0[1];
        st = {uv0[0] + hit.u * ds1 + hit.v * ds2,
            uv0[1] + hit.u * dt1 + hit.v * dt2};
        const double determinant = ds1 * dt2 - ds2 * dt1;
        if (determinant != 0) {
          Vec3 dpds{}, dpdt{};
          for (int c = 0; c < 3; ++c) {
            dpds[c] = (e1[c] * dt2 - e2[c] * dt1) / determinant;
            dpdt[c] = (e2[c] * ds1 - e1[c] * ds2) / determinant;
          }
          const double projection = Dot(dpds, base);
          for (int c = 0; c < 3; ++c)
            dpds[c] -= projection * base[c];
          tangent = Normalize(dpds);
          bitangent = Cross(base, tangent);
          if (Dot(bitangent, dpdt) < 0)
            for (double& c : bitangent)
              c = -c;
        }
      }
      Vec3 mapped{material.normal[0], material.normal[1], material.normal[2]};
      if (material.normal_texture) {
        const auto lookup = LookupOracle(*snapshot.scene, *material.normal_texture, st[0], st[1]);
        for (int c = 0; c < 3; ++c)
          mapped[c] = std::clamp(lookup.value[c], -1.0, 1.0);
      }
      Vec3 expected = base;
      if (Dot(mapped, mapped) > 0) {
        for (int c = 0; c < 3; ++c)
          expected[c] = mapped[0] * tangent[c] + mapped[1] * bitangent[c] + mapped[2] * base[c];
        expected = Normalize(expected);
        if (Dot(expected, geometric) < 0)
          for (double& c : expected)
            c = -c;
        if (expected[2] <= 0)
          expected = base;
      }
      for (int c = 0; c < 3; ++c) {
        if (!std::isfinite(actual[c]) || std::abs(actual[c] - expected[c]) > 0.012 || actual[3] != 1)
          return "normal map at " + std::to_string(x) + "," + std::to_string(y) +
                 ": component " + std::to_string(c) + " is " + std::to_string(actual[c]) +
                 " instead of " + std::to_string(expected[c]);
      }
      for (const auto& [key, mesh] : snapshot.scene->meshes)
        if (&mesh == hit.mesh)
          ++pixels[key];
    }
  }
  return {};
}

std::string NormalMapsFailure(PathScenes& scenes) {
  auto& world = scenes.World();
  world.SetCamera(OrthographicCamera());
  world.SetTexture("/normal", MakeTexture(3, 2, Lotus::TextureFormat::Rgba8Unorm,
                                  [](std::uint32_t i, std::uint32_t j) -> Rgba {
                                    return {0.3 + 0.2 * i, 0.35 + 0.3 * j, 0.9, 1};
                                  }));
  world.SetTexture("/signed", MakeTexture(2, 2, Lotus::TextureFormat::Rgba32Float,
                                  [](std::uint32_t i, std::uint32_t j) -> Rgba {
                                    return {-0.3 + 0.5 * i, -0.25 + 0.4 * j, 0.8, 1};
                                  }));
  using Uvs = std::array<std::array<float, 2>, 4>;
  const Uvs regular{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
  const Uvs mirrored{{{1, 0}, {0, 0}, {0, 1}, {1, 1}}};
  const Uvs rotated{{{0, 1}, {0, 0}, {1, 0}, {1, 1}}};
  const Uvs degenerate{{{0.5F, 0.5F}, {0.5F, 0.5F}, {0.5F, 0.5F}, {0.5F, 0.5F}}};
  for (int index = 0; index < 8; ++index) {
    auto panel = MappedSquare({{"st", index == 1 ? mirrored : index == 2 ? rotated
                                                          : index == 5   ? degenerate
                                                                         : regular},
        {"unused", rotated}});
    Lotus::MeshInstance instance;
    const float x = -0.75F + 0.5F * (index % 4);
    const float y = index < 4 ? 0.5F : -0.5F;
    instance.world_from_object = Lotus::Multiply(Translation(x, y, 0), Scaling(0.22F, 0.4F, 1));
    if (index == 3) {
      SetVertexNormals(panel, {{-0.2F, 0.1F, 1}, {0.4F, -0.2F, 1},
                                  {0.3F, 0.3F, 1}, {-0.3F, 0.2F, 1}});
      auto shear = Scaling(-0.22F, 0.4F, 1.8F);
      shear[4] = 0.08F;
      instance.world_from_object = Lotus::Multiply(Translation(x, y, 0),
          Lotus::Multiply(RotationX(25), shear));
    }
    if (index == 4)
      instance.world_from_object = Lotus::Multiply(instance.world_from_object, RotationY(180));
    if (index == 6)
      panel.texcoords.erase("st");
    Lotus::Material material;
    material.texcoords = "st";
    material.texcoord_fallback = {0.5F, 0.5F};
    material.normal_texture = Lotus::TextureInput{index == 2 ? "/signed" : "/normal"};
    material.normal_texture->wrap_s = Lotus::TextureWrap::Clamp;
    material.normal_texture->wrap_t = Lotus::TextureWrap::Clamp;
    if (index != 2) {
      material.normal_texture->scale = {2, 2, 2, 1};
      material.normal_texture->bias = {-1, -1, -1, 0};
    }
    if (index == 7) {
      material.normal_texture->texture = "/missing";
      material.normal_texture->fallback = {-0.3F, 0.4F, 0.8F, 1};
    }
    const std::string key = "/panel" + std::to_string(index);
    world.SetMesh(key, panel, instance);
    Paint(world, key, material);
  }
  Lotus::PathTracingSettings diagnostic;
  diagnostic.output = Lotus::SceneOutput::ShadingNormal;
  const auto check = [&](const std::string& label) -> std::string {
    Lotus::GpuFrameEvidence frame;
    if (auto failure = scenes.Render(diagnostic, frame); !failure.empty())
      return failure;
    std::map<std::string, std::size_t> pixels;
    if (auto failure = NormalMapDiagnosticFailure(world.Commit(), scenes.Target(), frame, pixels);
        !failure.empty())
      return label + ": " + failure;
    for (const auto& [key, mesh] : world.Commit().scene->meshes) {
      (void)mesh;
      if (pixels[key] < 40)
        return label + ": insufficient coverage of " + key;
    }
    scenes.Summarize(label + ": " + std::to_string(pixels.size()) + " mapped panels");
    return {};
  };
  if (auto failure = check("UV frames, smooth normals, mirrors, backfaces, fallbacks"); !failure.empty())
    return failure;
  world.SetTexture("/normal", MakeTexture(1, 1, Lotus::TextureFormat::Rgba32Float,
                                  [](std::uint32_t, std::uint32_t) -> Rgba { return {0.75, 0.25, 1, 1}; }));
  if (auto failure = check("texture edit"); !failure.empty())
    return failure;
  auto material = world.Commit().scene->materials.at("/panel7");
  material.normal_texture->fallback = {0, 0, 0, 1};
  world.SetMaterial("/panel7", material);
  if (auto failure = check("zero normal fallback"); !failure.empty())
    return failure;
  material.normal_texture.reset();
  material.normal = {0.3F, -0.4F, -0.8F};
  world.SetMaterial("/panel7", material);
  if (auto failure = check("constant signed normal"); !failure.empty())
    return failure;
  material.normal = {1, 0, 0};
  world.SetMaterial("/panel7", material);
  if (auto failure = check("tangent normal fallback"); !failure.empty())
    return failure;
  if (auto failure = scenes.Clear(); !failure.empty())
    return failure;

  // Exact mirror radiance proves the mapped normal reaches BSDF sampling,
  // not just its diagnostic: only the tilted mirror can see the bright wall.
  world.SetCamera(OrthographicCamera());
  auto plane = MappedSquare({{"st", regular}});
  for (auto& p : plane.positions) {
    p[0] *= 4;
    p[1] *= 4;
  }
  world.SetMesh("/plane", plane, Lotus::MeshInstance{});
  Lotus::Material mirror;
  mirror.base_color = {1, 1, 1};
  mirror.roughness = 0;
  mirror.metallic = 1;
  Paint(world, "/plane", mirror);
  Lotus::Material wall;
  wall.base_color = {0, 0, 0};
  wall.ior = 1;
  wall.emission = {2, 1, 0.5F};
  world.SetMesh("/wall", {{{6, -20, 0.5F}, {6, 20, 0.5F}, {6, 20, 10}, {6, -20, 10}}, {{0, 1, 2}, {0, 2, 3}}, {0, 0}}, Lotus::MeshInstance{});
  Paint(world, "/wall", wall);
  if (auto failure = scenes.ExpectExact(4, {0, 0, 0}); !failure.empty())
    return failure;
  world.SetTexture("/tilt", MakeTexture(1, 1, Lotus::TextureFormat::Rgba32Float,
                                [](std::uint32_t, std::uint32_t) -> Rgba { return {0.5, 0, 0.866025404, 1}; }));
  mirror.normal_texture = Lotus::TextureInput{"/tilt"};
  mirror.normal_texture->wrap_s = Lotus::TextureWrap::Clamp;
  mirror.normal_texture->wrap_t = Lotus::TextureWrap::Clamp;
  mirror.texcoords = "st";
  Paint(world, "/plane", mirror);
  if (auto failure = scenes.ExpectExact(4, {2, 1, 0.5}); !failure.empty())
    return "mapped mirror: " + failure;
  scenes.Summarize("mapped mirror: exact emissive-wall radiance at 4 spp");
  return {};
}

// Coverage is tested against analytic layer mixtures, and the alpha mask
// against the independent CPU bilinear oracle. No shader RNG is reproduced.
std::string OpacityFailure(PathScenes& scenes) {
  auto& world = scenes.World();
  world.SetCamera(OrthographicCamera());
  const auto square = MappedSquare({{"st", {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}}}});
  world.SetMesh("/front", square, Lotus::MeshInstance{});
  Lotus::MeshInstance behind;
  behind.world_from_object = Translation(0, 0, -2);
  world.SetMesh("/back", square, behind);
  Lotus::Material front;
  front.ior = 1;
  front.base_color = {1, 0, 0};
  front.emission = {4, 0, 0};
  front.texcoords = "st";
  Lotus::Material back;
  back.ior = 1;
  back.base_color = {0, 1, 0};
  back.emission = {0, 2, 1};
  Paint(world, "/front", front);
  Paint(world, "/back", back);
  if (auto failure = scenes.ExpectExact(0, {4, 0, 0}); !failure.empty()) return failure;
  front.opacity = 0;
  Paint(world, "/front", front);
  if (auto failure = scenes.ExpectExact(0, {0, 2, 1}); !failure.empty()) return failure;
  front.opacity = front.opacity_threshold = 0.5F;
  Paint(world, "/front", front);
  if (auto failure = scenes.ExpectExact(0, {4, 0, 0}); !failure.empty())
    return "threshold equality: " + failure;
  front.opacity = 0.499F;
  Paint(world, "/front", front);
  if (auto failure = scenes.ExpectExact(0, {0, 2, 1}); !failure.empty())
    return "below threshold: " + failure;

  Lotus::PathTracingSettings settings;
  settings.output = Lotus::SceneOutput::Albedo;
  Lotus::GpuFrameEvidence background;
  if (auto failure = scenes.Render(settings, background); !failure.empty()) return failure;
  front.opacity = 1;
  Paint(world, "/front", front);
  Lotus::GpuFrameEvidence foreground;
  if (auto failure = scenes.Render(settings, foreground); !failure.empty()) return failure;
  world.SetTexture("/alpha", MakeTexture(2, 1, Lotus::TextureFormat::Rgba8Srgb,
      [](std::uint32_t i, std::uint32_t) -> Rgba { return {1, 1, 1, i == 0 ? 0.25 : 0.75}; }));
  front.opacity_texture = Lotus::TextureInput{"/alpha", 3};
  front.opacity_texture->wrap_s = Lotus::TextureWrap::Clamp;
  front.opacity_texture->wrap_t = Lotus::TextureWrap::Clamp;
  // Alpha must remain linear even in an sRGB image; exercise scale and bias.
  front.opacity_texture->scale[3] = 0.8F;
  front.opacity_texture->bias[3] = 0.1F;
  Paint(world, "/front", front);
  const auto mask_check = [&](bool mirrored) -> std::string {
    Lotus::GpuFrameEvidence frame;
    if (auto failure = scenes.Render(settings, frame); !failure.empty()) return failure;
    const auto snapshot = world.Commit();
    const auto values = ColorValues(frame.color);
    const auto fg = ColorValues(foreground.color);
    const auto bg = ColorValues(background.color);
    const auto& depths = frame.depth.payload;
    const auto& fg_depth = foreground.depth.payload;
    const auto& bg_depth = background.depth.payload;
    const auto& lookup = *front.opacity_texture;
    std::size_t accepted = 0, rejected = 0;
    for (std::uint32_t y = 0; y < scenes.Target().height; ++y) {
      for (std::uint32_t x = 0; x < scenes.Target().width; ++x) {
        const double s = (x + 0.5) / scenes.Target().width;
        const auto texel = LookupOracle(*snapshot.scene, lookup, mirrored ? 1 - s : s, 0.5);
        const bool hit = std::clamp(texel.value[3], 0.0, 1.0) >= front.opacity_threshold;
        hit ? ++accepted : ++rejected;
        const std::size_t p = y * scenes.Target().width + x;
        const auto& expected = hit ? fg : bg;
        for (int c = 0; c < 4; ++c)
          if (values[4 * p + c] != expected[4 * p + c])
            return "alpha-mask albedo differs at pixel " + std::to_string(p);
        if (std::abs(depths[p] - (hit ? fg_depth[p] : bg_depth[p])) > 1e-6)
          return "alpha-mask depth differs at pixel " + std::to_string(p);
      }
    }
    return accepted == 0 || rejected == 0 ? "alpha mask did not exercise both outcomes" : "";
  };
  if (auto failure = mask_check(false); !failure.empty()) return failure;
  world.SetMesh("/front", MappedSquare({{"st", {{{1, 0}, {0, 0}, {0, 1}, {1, 1}}}}}), Lotus::MeshInstance{});
  if (auto failure = mask_check(true); !failure.empty()) return "mirrored UVs: " + failure;
  world.SetTexture("/alpha", MakeTexture(2, 1, Lotus::TextureFormat::Rgba8Srgb,
      [](std::uint32_t i, std::uint32_t) -> Rgba { return {1, 1, 1, i == 0 ? 0.75 : 0.25}; }));
  if (auto failure = mask_check(true); !failure.empty()) return "texture replacement: " + failure;
  world.RemoveTexture("/alpha");
  front.opacity_texture->fallback = {1, 1, 1, 0.5F};
  front.opacity_texture->scale[3] = 0;
  Paint(world, "/front", front);
  if (auto failure = scenes.ExpectExact(0, {4, 0, 0}); !failure.empty())
    return "unscaled missing-image fallback: " + failure;
  world.SetTexture("/alpha", MakeTexture(1, 1, Lotus::TextureFormat::Rgba32Float,
      [](std::uint32_t, std::uint32_t) -> Rgba {
        return {-0.25, 2, 1, std::numeric_limits<float>::max()};
      }));
  front.opacity_texture->scale = {1, 1, 1, 1};
  front.opacity_texture->scale[3] = 2;
  front.opacity_texture->bias = {0, 0, 0, 0};
  front.texcoords = "absent";
  front.texcoord_fallback = {0.5F, 0.5F};
  Paint(world, "/front", front);
  if (auto failure = scenes.ExpectExact(0, {0, 2, 1}); !failure.empty())
    return "non-finite float alpha: " + failure;
  front.opacity_texture->channel = 0;
  Paint(world, "/front", front);
  if (auto failure = scenes.ExpectExact(0, {0, 2, 1}); !failure.empty())
    return "negative red coverage: " + failure;
  front.opacity_texture->channel = 1;
  Paint(world, "/front", front);
  if (auto failure = scenes.ExpectExact(0, {4, 0, 0}); !failure.empty())
    return "clamped green coverage without a UV set: " + failure;
  front.opacity_texture.reset();
  front.base_color = {0, 0, 0};
  back.base_color = {0, 0, 0};
  Paint(world, "/back", back);
  front.opacity = 0.25F;
  front.opacity_threshold = 0;
  Paint(world, "/front", front);
  if (auto failure = scenes.ExpectMean("one coverage layer", 32, {1, 1.5, 0.75}); !failure.empty()) return failure;
  behind.world_from_object = Translation(0, 0, -1);
  world.SetMesh("/middle", square, behind);
  Lotus::Material middle;
  middle.ior = 1;
  middle.base_color = {0, 0, 0};
  middle.emission = {0, 2, 0};
  middle.opacity = 0.5F;
  Paint(world, "/middle", middle);
  back.emission = {0, 0, 2};
  Paint(world, "/back", back);
  if (auto failure = scenes.ExpectMean("two independent coverage layers", 32, {1, 0.75, 0.75}); !failure.empty()) return failure;
  world.RemoveMesh("/middle");
  world.RemoveMesh("/back");
  settings.output = Lotus::SceneOutput::Radiance;
  settings.max_bounces = 0;
  Lotus::GpuFrameEvidence frame;
  if (auto failure = scenes.Render(settings, frame, 32); !failure.empty()) return failure;
  const auto values = ColorValues(frame.color);
  double alpha = 0;
  for (std::size_t p = 0; p * 4 < values.size(); ++p) {
    const double a = values[4 * p + 3];
    alpha += a;
    for (int c = 0; c < 3; ++c) {
      const double expected = a * front.emission[c] + (1 - a) * scenes.Target().clear_color[c];
      if (std::abs(values[4 * p + c] - expected) > 1e-5)
        return "background compositing differs from coverage alpha";
    }
  }
  const double pixels = static_cast<double>(values.size() / 4);
  const double tolerance = 5 * std::sqrt(0.25 * 0.75 / (pixels * 32));
  if (std::abs(alpha / pixels - 0.25) > tolerance)
    return "primary alpha differs from analytic 0.25 coverage";
  Lotus::GpuFrameEvidence repeated;
  settings.sample_index = 99;
  if (auto failure = scenes.Render(settings, repeated); !failure.empty()) return failure;
  if (frame.depth.payload != repeated.depth.payload)
    return "progressive sample index changed fixed coverage depth";
  settings.sample_index = 0;
  if (auto failure = scenes.Render(settings, repeated, 32); !failure.empty()) return failure;
  if (frame.color.payload != repeated.color.payload || frame.depth.payload != repeated.depth.payload)
    return "fixed-seed alpha rendering was not repeatable";
  if (auto failure = scenes.Clear(); !failure.empty()) return failure;

  // A mirror's secondary ray crosses a cut-out outside the camera view.
  // With max_bounces=1, rejected candidates must not consume another bounce.
  world.SetCamera(OrthographicCamera());
  auto plane = Square();
  for (auto& p : plane.positions) { p[0] *= 4; p[1] *= 4; }
  world.SetMesh("/mirror", plane, Lotus::MeshInstance{});
  Lotus::Material mirror;
  mirror.base_color = {1, 1, 1};
  mirror.roughness = 0;
  mirror.metallic = 1;
  mirror.normal = {0.5F, 0, 0.866025404F};
  Paint(world, "/mirror", mirror);
  const auto wall = [](float x) -> Lotus::MeshGeometry {
    return {{{x, -20, 0.5F}, {x, 20, 0.5F}, {x, 20, 10}, {x, -20, 10}}, {{0, 1, 2}, {0, 2, 3}}, {0, 0}};
  };
  world.SetMesh("/cutout", wall(3), Lotus::MeshInstance{});
  front.opacity = 0;
  front.opacity_threshold = 0.5F;
  Paint(world, "/cutout", front);
  world.SetMesh("/light", wall(6), Lotus::MeshInstance{});
  Paint(world, "/light", back);
  if (auto failure = scenes.ExpectExact(1, {0, 0, 2}); !failure.empty()) return "secondary cut-out: " + failure;
  front.opacity = 1;
  Paint(world, "/cutout", front);
  if (auto failure = scenes.ExpectExact(1, {4, 0, 0}); !failure.empty()) return "secondary accepted surface: " + failure;
  front.opacity = 0.25F;
  front.opacity_threshold = 0;
  Paint(world, "/cutout", front);
  if (auto failure = scenes.ExpectMean("secondary coverage", 32, {1, 0, 1.5}); !failure.empty()) return failure;
  scenes.Summarize("cut-outs, threshold equality, linear texture alpha, mirrored UVs, replacement, fallback, primary alpha and deterministic repeat passed");
  return {};
}

// Texture lookups through the GpuScene to the path tracer. The Albedo and
// RoughnessMetallic diagnostics of sRGB, linear and float textures under
// every wrap mode, scale and bias, channel selection, a missing texture's
// fallback and a mesh without the material's texture-coordinate set, against
// an independent oracle of Vulkan's bilinear filter, before and after a
// texture edit; then the radiance of textured Lambert and emissive surfaces.
std::string TexturesFailure(PathScenes& scenes) {
  Lotus::RenderWorld& world = scenes.World();
  world.SetCamera(OrthographicCamera());
  const auto checker = [](std::uint32_t i, std::uint32_t j) {
    return std::array<double, 4>{((37 * i + 91 * j) % 256) / 255.0,
        ((113 * i + 29 * j + 64) % 256) / 255.0,
        ((71 * i * j + 17 * i + 200) % 256) / 255.0, ((i + j) % 2) ? 0.25 : 1.0};
  };
  world.SetTexture("/checker",
      MakeTexture(4, 4, Lotus::TextureFormat::Rgba8Srgb, checker));
  world.SetTexture("/linear",
      MakeTexture(3, 2, Lotus::TextureFormat::Rgba8Unorm,
          [](std::uint32_t i, std::uint32_t j) {
            return std::array<double, 4>{0.1 * i, 0.3 + 0.2 * j + 0.1 * i,
                0.5, 1.0 - 0.4 * j - 0.15 * i};
          }));
  world.SetTexture("/float",
      MakeTexture(2, 3, Lotus::TextureFormat::Rgba32Float,
          [](std::uint32_t i, std::uint32_t j) {
            return std::array<double, 4>{-0.5 + 0.75 * i + 0.5 * j,
                1.5 - 0.6 * j, 0.25 + 0.3 * i, 1.0};
          }));
  const auto place = [](float x, float y) {
    return Lotus::Multiply(Translation(x, y, 0), Scaling(0.45F, 0.45F, 1));
  };
  // Upper left: repeat along s, mirror along t, reading "st".
  Lotus::MeshInstance instance;
  instance.world_from_object = place(-0.5F, 0.5F);
  world.SetMesh("/repeat",
      MappedSquare({{"st", {{{-0.6F, -0.4F}, {1.7F, -0.4F}, {1.7F, 1.3F}, {-0.6F, 1.3F}}}}}),
      instance);
  Lotus::Material repeat;
  repeat.base_color_texture = Lotus::TextureInput{"/checker"};
  repeat.base_color_texture->wrap_s = Lotus::TextureWrap::Repeat;
  repeat.base_color_texture->wrap_t = Lotus::TextureWrap::Mirror;
  repeat.texcoords = "st";
  Paint(world, "/repeat", repeat);
  // Upper right: clamp along s, the black border along t, scaled and biased.
  instance.world_from_object = place(0.5F, 0.5F);
  world.SetMesh("/clamp",
      MappedSquare({{"st", {{{-0.5F, -0.5F}, {1.5F, -0.5F}, {1.5F, 1.5F}, {-0.5F, 1.5F}}}}}),
      instance);
  Lotus::Material clamp;
  clamp.base_color_texture = Lotus::TextureInput{"/checker"};
  clamp.base_color_texture->wrap_s = Lotus::TextureWrap::Clamp;
  clamp.base_color_texture->wrap_t = Lotus::TextureWrap::Black;
  clamp.base_color_texture->scale = {1.0F, 0.5F, 1.0F, 1.0F};
  clamp.base_color_texture->bias = {0.0F, 0.25F, 0.0F, 0.0F};
  clamp.texcoords = "st";
  Paint(world, "/clamp", clamp);
  // Lower left: a float texture, read through the second of two sets and
  // pushed out of [0, 1], which the base colour clamps.
  instance.world_from_object = place(-0.5F, -0.5F);
  world.SetMesh("/float",
      MappedSquare({{"map1", {{{9, 9}, {9, 9}, {9, 9}, {9, 9}}}},
          {"uv", {{{0.9F, -0.2F}, {1.1F, 1.2F}, {-0.3F, 1.0F}, {0.1F, 0.0F}}}}}),
      instance);
  Lotus::Material floating;
  floating.base_color_texture = Lotus::TextureInput{"/float"};
  floating.base_color_texture->wrap_s = Lotus::TextureWrap::Repeat;
  floating.base_color_texture->wrap_t = Lotus::TextureWrap::Clamp;
  floating.base_color_texture->scale = {0.5F, 1.0F, 1.0F, 1.0F};
  floating.base_color_texture->bias = {0.1F, 0.0F, -0.2F, 0.0F};
  floating.texcoords = "uv";
  Paint(world, "/float", floating);
  // Lower right: a lookup whose texture is missing, and one on a mesh
  // without the set its material reads.
  world.SetMesh("/missing",
      {{{0.05F, -0.95F, 0}, {0.95F, -0.95F, 0}, {0.05F, -0.05F, 0}},
          {{0, 1, 2}}, {0}},
      Lotus::MeshInstance{});
  Lotus::Material missing;
  missing.base_color_texture = Lotus::TextureInput{"/absent"};
  missing.base_color_texture->fallback = {0.2F, 0.4F, 0.6F, 1.0F};
  missing.roughness_texture = Lotus::TextureInput{"/absent", 3};
  missing.roughness_texture->fallback = {0.0F, 0.0F, 0.0F, 0.375F};
  Paint(world, "/missing", missing);
  world.SetMesh("/unmapped",
      {{{0.95F, -0.95F, 0}, {0.95F, -0.05F, 0}, {0.1F, -0.05F, 0}},
          {{0, 1, 2}}, {0}},
      Lotus::MeshInstance{});
  Lotus::Material unmapped = repeat;
  unmapped.texcoord_fallback = {0.3F, 0.8F};
  Paint(world, "/unmapped", unmapped);

  std::map<std::string, std::size_t> albedo;
  if (auto failure = SurfaceOutputFailure(scenes, Lotus::SceneOutput::Albedo, albedo);
      !failure.empty())
    return "albedo: " + failure;
  // Roughness from green and metallic from alpha of a linear texture.
  repeat.roughness_texture = Lotus::TextureInput{"/linear", 1};
  repeat.roughness_texture->wrap_s = Lotus::TextureWrap::Mirror;
  repeat.roughness_texture->wrap_t = Lotus::TextureWrap::Repeat;
  repeat.roughness_texture->scale = {1.0F, 1.5F, 1.0F, 1.0F};
  repeat.metallic_texture = Lotus::TextureInput{"/linear", 3};
  repeat.metallic_texture->wrap_t = Lotus::TextureWrap::Clamp;
  repeat.metallic_texture->bias = {0.0F, 0.0F, 0.0F, -0.125F};
  Paint(world, "/repeat", repeat);
  clamp.metallic_texture = Lotus::TextureInput{"/checker", 0};
  Paint(world, "/clamp", clamp);
  std::map<std::string, std::size_t> parameters;
  if (auto failure = SurfaceOutputFailure(scenes,
          Lotus::SceneOutput::RoughnessMetallic, parameters);
      !failure.empty())
    return "roughness and metallic: " + failure;
  // A texture edit replaces the image that the descriptors name.
  world.SetTexture("/checker",
      MakeTexture(4, 4, Lotus::TextureFormat::Rgba8Srgb,
          [&](std::uint32_t i, std::uint32_t j) { return checker(3 - i, j); }));
  std::map<std::string, std::size_t> edited;
  if (auto failure = SurfaceOutputFailure(scenes, Lotus::SceneOutput::Albedo, edited);
      !failure.empty())
    return "albedo after a texture edit: " + failure;
  std::ostringstream line;
  line << "albedo and roughness/metallic pixels against the bilinear oracle:";
  for (const auto& [key, count] : albedo)
    line << ' ' << key << ' ' << count << '/' << parameters[key];
  scenes.Summarize(line.str());
  for (const char* key : {"/repeat", "/clamp", "/float", "/missing", "/unmapped"})
    if (albedo[key] == 0 || parameters[key] == 0 || edited[key] == 0)
      return std::string("no pixel of ") + key + " was compared";
  for (const char* key : {"/repeat", "/clamp", "/float", "/missing", "/unmapped"})
    world.RemoveMesh(key);

  // A Lambert square filling the view under a white environment, with an
  // 8x8 albedo texture and an 8x8 float emission texture of 2x2 blocks. The
  // bilinear filter is constant within each block, half a texel from its
  // edges, so a pixel there reflects its block's albedo exactly and adds its
  // emission; with no bounces it shows the emission alone.
  const std::array<Rgb, 4> block_albedo{
      {{0.75, 0.25, 0.5}, {0.125, 0.5, 1.0}, {1.0, 1.0, 0.25}, {0.375, 0.0, 0.625}}};
  const std::array<Rgb, 4> block_emission{
      {{0.5, 0.0, 0.0}, {0.0, 2.0, 0.25}, {0.0, 0.0, 0.0}, {4.0, 1.0, 3.0}}};
  const auto block = [](std::uint32_t i, std::uint32_t j) { return (j / 4) * 2 + i / 4; };
  world.SetTexture("/blocks",
      MakeTexture(8, 8, Lotus::TextureFormat::Rgba8Unorm,
          [&](std::uint32_t i, std::uint32_t j) {
            const Rgb& value = block_albedo[block(i, j)];
            return std::array<double, 4>{value[0], value[1], value[2], 1.0};
          }));
  world.SetTexture("/glow",
      MakeTexture(8, 8, Lotus::TextureFormat::Rgba32Float,
          [&](std::uint32_t i, std::uint32_t j) {
            const Rgb& value = block_emission[block(i, j)];
            return std::array<double, 4>{value[0], value[1], value[2], 1.0};
          }));
  world.SetMesh("/wall",
      MappedSquare({{"st", {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}}}}),
      Lotus::MeshInstance{});
  Lotus::Material wall;
  wall.base_color_texture = Lotus::TextureInput{"/blocks"};
  wall.ior = 1;
  wall.emission_texture = Lotus::TextureInput{"/glow"};
  wall.texcoords = "st";
  Paint(world, "/wall", wall);
  world.SetEnvironment({1.0F, 1.0F, 1.0F});
  const Lotus::OffscreenTarget& target = scenes.Target();
  // 64 pixels span 8 texels, so a block's constant interior is pixels
  // [4, 28) from the image's block edges.
  for (const std::uint32_t bounces : {64U, 0U}) {
    Lotus::PathTracingSettings settings;
    settings.max_bounces = bounces;
    Lotus::GpuFrameEvidence frame;
    if (auto failure = scenes.Render(settings, frame, 4); !failure.empty())
      return "textured radiance: " + failure;
    const std::vector<float> values = ColorValues(frame.color);
    std::size_t checked = 0;
    for (std::uint32_t y = 0; y < target.height; ++y) {
      for (std::uint32_t x = 0; x < target.width; ++x) {
        if (x % 32 < 4 || x % 32 >= 28 || y % 32 < 4 || y % 32 >= 28)
          continue;
        // Texture rows run from the top, as image rows do.
        const std::uint32_t index = block(x / 8, y / 8);
        for (int c = 0; c < 3; ++c) {
          // The albedo as the 8-bit texels hold it.
          const double quantized =
              std::lround(block_albedo[index][c] * 255) / 255.0;
          const double want =
              block_emission[index][c] + (bounces == 0 ? 0.0 : quantized);
          const float actual = values[(std::size_t{y} * target.width + x) * 4 + c];
          if (std::abs(actual - want) > 1e-6 + 1e-4 * want)
            return "textured radiance with " + std::to_string(bounces) +
                   " bounces: " + std::to_string(actual) + " instead of " +
                   std::to_string(want) + " at " + std::to_string(x) + "," +
                   std::to_string(y);
        }
        ++checked;
      }
    }
    scenes.Summarize("textured radiance with " + std::to_string(bounces) +
                     " bounces: " + std::to_string(checked) + " block-interior pixels exact");
  }
  // A specular-only image drives the coat on a diffuse plane. Independent
  // texel decoding and analytic/half-vector integrals predict its furnace
  // radiance. Replacement, missing-image fallback and lookup overflow must
  // all update transport without replacing the plane.
  wall = Lotus::Material{};
  wall.use_specular_workflow = true;
  wall.base_color = {0.4F, 0.2F, 0.1F};
  wall.specular_color = {0.3F, 0.1F, 0.05F};
  wall.specular_color_texture = Lotus::TextureInput{"/specular"};
  wall.specular_color_texture->wrap_s = Lotus::TextureWrap::Clamp;
  wall.specular_color_texture->wrap_t = Lotus::TextureWrap::Clamp;
  wall.specular_color_texture->scale = {2, 0.5F, 1, 1};
  wall.specular_color_texture->bias = {-0.1F, 0.1F, 0.05F, 0};
  wall.specular_color_texture->fallback = {0.8F, 0.3F, 0.1F, 1};
  wall.texcoords = "st";
  for (int mode = 0; mode < 5; ++mode) {
    if (mode < 3) {
      const auto format = mode == 1 ? Lotus::TextureFormat::Rgba8Srgb : Lotus::TextureFormat::Rgba32Float;
      world.SetTexture("/specular", MakeTexture(1, 1, format,
                                        [mode](std::uint32_t, std::uint32_t) -> Rgba {
                                          return mode == 2 ? Rgba{0.9, 0.4, 0.2, 1} : Rgba{0.2, 0.6, 0.3, 1};
                                        }));
    } else if (mode == 3) {
      world.RemoveTexture("/specular");
    } else {
      world.SetTexture("/specular", MakeTexture(1, 1, Lotus::TextureFormat::Rgba32Float,
                                        [](std::uint32_t, std::uint32_t) -> Rgba { return {2, 1, 1, 1}; }));
      wall.specular_color_texture->scale[0] = std::numeric_limits<float>::max();
    }
    Paint(world, "/wall", wall);
    const auto snapshot = world.Commit();
    const auto sampled = LookupOracle(*snapshot.scene, *wall.specular_color_texture, 0.5, 0.5);
    Lotus::Material evaluated = wall;
    bool finite = true;
    for (int c = 0; c < 3; ++c)
      finite &= std::isfinite(static_cast<float>(sampled.value[c]));
    if (finite)
      for (int c = 0; c < 3; ++c)
        evaluated.specular_color[c] = static_cast<float>(std::clamp(sampled.value[c], 0.0, 1.0));
    const std::string name = "specular texture mode " + std::to_string(mode);
    if (auto failure = scenes.ExpectMean(name.c_str(), 32, SurfaceAlbedo(evaluated, 1)); !failure.empty())
      return name + ": " + failure;
  }
  return {};
}

// A closed Lambert box seen from inside: every path bounces until it ends,
// and the environment must never reach it. Bounce-limited radiance is exact,
// L = Le (1 + a + ... + a^n); the unlimited mean is Le / (1 - a).
std::string MultibounceFailure(PathScenes& scenes) {
  Lotus::RenderWorld& world = scenes.World();
  world.SetCamera(WideCamera());
  Lotus::MeshGeometry box;
  box.positions = {{{-1, -1, -1}}, {{1, -1, -1}}, {{1, 1, -1}}, {{-1, 1, -1}},
      {{-1, -1, 1}}, {{1, -1, 1}}, {{1, 1, 1}}, {{-1, 1, 1}}};
  box.triangles = {{{0, 2, 1}}, {{0, 3, 2}}, {{4, 5, 6}}, {{4, 6, 7}},
      {{0, 1, 5}}, {{0, 5, 4}}, {{3, 7, 6}}, {{3, 6, 2}},
      {{0, 4, 7}}, {{0, 7, 3}}, {{1, 2, 6}}, {{1, 6, 5}}};
  box.source_faces = {0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5};
  world.SetMesh("/box", box, Lotus::MeshInstance{});
  const Rgb albedo{0.5, 0.25, 0.75};
  const Rgb emission{0.1, 0.15, 0.025};
  Lotus::Material material;
  material.base_color = {0.5F, 0.25F, 0.75F};
  material.emission = {0.1F, 0.15F, 0.025F};
  material.ior = 1;
  Paint(world, "/box", material);
  world.SetEnvironment({1.0F, 1.0F, 1.0F});
  for (const std::uint32_t bounces : {0U, 1U, 3U}) {
    Rgb expected{};
    for (int c = 0; c < 3; ++c) {
      double term = emission[c];
      for (std::uint32_t k = 0; k <= bounces; ++k, term *= albedo[c])
        expected[c] += term;
    }
    if (auto failure = scenes.ExpectExact(bounces, expected); !failure.empty())
      return std::to_string(bounces) + " bounces: " + failure;
  }
  Rgb converged{};
  for (int c = 0; c < 3; ++c)
    converged[c] = emission[c] / (1.0 - albedo[c]);
  if (auto failure = scenes.ExpectMean("Russian roulette", 16, converged);
      !failure.empty())
    return "Russian roulette mean: " + failure;

  // A restarted accumulation with the same first sample index repeats its
  // image; another index does not. Changing the index restarts it.
  Lotus::PathTracingSettings settings;
  settings.sample_index = 1;
  Lotus::GpuFrameEvidence first;
  Lotus::GpuFrameEvidence again;
  Lotus::GpuFrameEvidence other;
  if (auto failure = scenes.Render(settings, first); !failure.empty())
    return failure;
  settings.sample_index = 2;
  if (auto failure = scenes.Render(settings, other); !failure.empty())
    return failure;
  settings.sample_index = 1;
  if (auto failure = scenes.Render(settings, again); !failure.empty())
    return failure;
  if (first.samples_per_pixel != 1 || other.samples_per_pixel != 1 ||
      again.samples_per_pixel != 1)
    return "a sample index change did not restart the accumulation";
  if (first.color.payload != again.color.payload)
    return "a repeated sample index changed the image";
  if (first.color.payload == other.color.payload)
    return "another sample index repeated the image";
  return {};
}

// The area of the part of a screen-space polygon inside the pixel square
// [x, x + 1] x [y, y + 1]: Sutherland-Hodgman clipping, then the shoelace
// formula.
double PixelCoverage(std::vector<std::array<double, 2>> polygon, double x,
    double y) {
  const auto clip = [&](int axis, double bound, bool below) {
    std::vector<std::array<double, 2>> kept;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
      const auto& a = polygon[i];
      const auto& b = polygon[(i + 1) % polygon.size()];
      const bool a_in = below ? a[axis] <= bound : a[axis] >= bound;
      const bool b_in = below ? b[axis] <= bound : b[axis] >= bound;
      if (a_in)
        kept.push_back(a);
      if (a_in != b_in) {
        const double t = (bound - a[axis]) / (b[axis] - a[axis]);
        kept.push_back({a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1])});
      }
    }
    polygon = std::move(kept);
  };
  clip(0, x, false);
  clip(0, x + 1, true);
  clip(1, y, false);
  clip(1, y + 1, true);
  double area = 0;
  for (std::size_t i = 0; i < polygon.size(); ++i) {
    const auto& a = polygon[i];
    const auto& b = polygon[(i + 1) % polygon.size()];
    area += a[0] * b[1] - b[0] * a[1];
  }
  return std::abs(area) / 2;
}

// The accumulation as an estimator, then as state. A Lambert triangle under
// a constant environment has one HDR radiance at every hit, so with the
// 1-pixel box filter each pixel's alpha estimates the triangle's area
// fraction of the pixel, computed here by clipping the projected triangle,
// and its colour is the coverage-weighted mix of that radiance and the
// clear colour. Then: split frames continue one accumulation, and what
// restarts it.
std::string AccumulationFailure(PathScenes& scenes) {
  Lotus::RenderWorld& world = scenes.World();
  Lotus::OffscreenTarget& target = scenes.Target();
  world.SetCamera(BootstrapCamera());
  Lotus::MeshGeometry triangle;
  triangle.positions = {{{-0.65F, -0.35F, 0}}, {{0.55F, -0.45F, 0}}, {{0.15F, 0.70F, 0}}};
  triangle.triangles = {{{0, 1, 2}}};
  triangle.source_faces = {0};
  world.SetMesh("/panel", triangle, Lotus::MeshInstance{});
  Lotus::Material material;
  material.base_color = {0.5F, 0.25F, 0.75F};
  material.emission = {2.5F, 1.25F, 0.5F};
  material.ior = 1;
  Paint(world, "/panel", material);
  world.SetEnvironment({2.0F, 1.0F, 0.5F});
  const Rgb radiance{0.5 * 2.0 + 2.5, 0.25 * 1.0 + 1.25, 0.75 * 0.5 + 0.5};

  constexpr std::uint32_t kSamples = 256;
  Lotus::PathTracingSettings settings;
  Lotus::GpuFrameEvidence frame;
  if (auto failure = scenes.Render(settings, frame, kSamples); !failure.empty())
    return failure;
  if (frame.samples_per_pixel != kSamples || frame.frames_rendered != kSamples)
    return "the accumulation holds " + std::to_string(frame.samples_per_pixel) +
           " samples instead of " + std::to_string(kSamples);
  const auto camera = Lotus::ExtractDrawSummary(world.Commit()).world_to_clip;
  std::vector<std::array<double, 2>> projected;
  for (const auto& position : triangle.positions) {
    const auto clip = Transform(camera, {position[0], position[1], position[2], 1.0});
    projected.push_back({(clip[0] / clip[3] + 1) / 2 * target.width,
        (1 - clip[1] / clip[3]) / 2 * target.height});
  }
  const std::vector<float> values = ColorValues(frame.color);
  double measured_total = 0;
  double expected_total = 0;
  double variance_total = 0;
  double brightest = 0;
  std::size_t partial = 0;
  for (std::uint32_t y = 0; y < target.height; ++y) {
    for (std::uint32_t x = 0; x < target.width; ++x) {
      const std::string at = " at " + std::to_string(x) + "," + std::to_string(y);
      const float* actual = &values[(std::size_t{y} * target.width + x) * 4];
      const double area = PixelCoverage(projected, x, y);
      const double alpha = actual[3];
      // Binomial: five standard deviations, plus one sample for edges that
      // graze a pixel's border.
      const double tolerance =
          5 * std::sqrt(area * (1 - area) / kSamples) + 1.0 / kSamples;
      if (std::abs(alpha - area) > tolerance)
        return "coverage " + std::to_string(alpha) + " instead of " +
               std::to_string(area) + " +- " + std::to_string(tolerance) + at;
      measured_total += alpha;
      expected_total += area;
      variance_total += area * (1 - area) / kSamples;
      partial += alpha > 0 && alpha < 1 ? 1 : 0;
      if (alpha == 0) {
        if (!std::equal(actual, actual + 4, target.clear_color.begin()))
          return "a pixel no sample hit changed" + at;
        continue;
      }
      for (int c = 0; c < 3; ++c) {
        const double want = alpha * radiance[c] + (1 - alpha) * target.clear_color[c];
        if (std::abs(want - actual[c]) > 1e-6 + 1e-4 * std::abs(want))
          return "radiance " + std::to_string(actual[c]) + " instead of " +
                 std::to_string(want) + " in channel " + std::to_string(c) + at;
        brightest = std::max(brightest, double{actual[c]});
      }
    }
  }
  if (brightest <= 1)
    return "no radiance above 1: the colour product is clamped";
  if (partial == 0)
    return "no pixel is partly covered";
  const double total_tolerance = 5 * std::sqrt(variance_total) + 1e-3;
  if (std::abs(measured_total - expected_total) > total_tolerance)
    return "total coverage " + std::to_string(measured_total) + " instead of " +
           std::to_string(expected_total) + " +- " + std::to_string(total_tolerance);
  std::ostringstream line;
  line << std::fixed << std::setprecision(3) << "box filter: " << partial
       << " partly covered pixels, total coverage " << measured_total << '/'
       << expected_total << "+-" << total_tolerance << " px at " << kSamples
       << " spp, peak radiance " << brightest;
  scenes.Summarize(line.str());

  // A GGX-Lambert mixture makes every sample's radiance random. A material
  // change restarts the accumulation; 1 + 3 + 12 samples in three frames
  // are the same image as 16 in one.
  material.metallic = 0.5F;
  material.roughness = 0.4F;
  Paint(world, "/panel", material);
  Lotus::GpuFrameEvidence whole;
  settings.sample_index = 1000;
  if (auto failure = scenes.Render(settings, whole, 16); !failure.empty())
    return failure;
  settings.sample_index = 2000;
  if (auto failure = scenes.Render(settings, frame, 1); !failure.empty())
    return failure;
  settings.sample_index = 1000;
  for (const std::uint32_t frames : {1U, 3U, 12U}) {
    if (auto failure = scenes.Render(settings, frame, frames); !failure.empty())
      return failure;
  }
  if (whole.samples_per_pixel != 16 || frame.samples_per_pixel != 16 ||
      frame.color.payload != whole.color.payload)
    return "1 + 3 + 12 samples differ from 16 samples in one frame";

  // How frames continue or restart the accumulation.
  const auto expect = [&](const char* step, std::uint32_t frames,
                          std::uint32_t samples) -> std::string {
    if (auto failure = scenes.Render(settings, frame, frames); !failure.empty())
      return std::string(step) + ": " + failure;
    if (frame.samples_per_pixel != samples)
      return std::string(step) + ": " + std::to_string(frame.samples_per_pixel) +
             " samples instead of " + std::to_string(samples);
    return {};
  };
  if (auto failure = expect("an unchanged frame", 1, 17); !failure.empty())
    return failure;
  settings.output = Lotus::SceneOutput::Barycentrics;
  if (auto failure = expect("a barycentric frame", 1, 0); !failure.empty())
    return failure;
  settings.output = Lotus::SceneOutput::Radiance;
  if (auto failure = expect("after a barycentric frame", 1, 18); !failure.empty())
    return failure;
  target.clear_color = {0.2F, 0.3F, 0.4F, 1.0F};
  if (auto failure = expect("a clear colour change", 1, 19); !failure.empty())
    return failure;
  Lotus::Camera moved = BootstrapCamera();
  moved.view[12] = 0.1F;
  world.SetCamera(moved);
  if (auto failure = expect("a camera change", 1, 1); !failure.empty())
    return failure;
  world.SetCamera(BootstrapCamera());
  if (auto failure = expect("the camera restored", 2, 2); !failure.empty())
    return failure;
  target.data_window = {8, 8, 48, 48};
  if (auto failure = expect("a data window change", 1, 1); !failure.empty())
    return failure;
  target.display_window = {-8.0F, 5.0F, 80.0F, 50.0F};
  if (auto failure = expect("a display window change", 1, 1); !failure.empty())
    return failure;
  settings.max_bounces = 8;
  if (auto failure = expect("a bounce limit change", 1, 1); !failure.empty())
    return failure;
  world.SetEnvironment({1.0F, 1.0F, 1.0F});
  if (auto failure = expect("an environment change", 1, 1); !failure.empty())
    return failure;
  target.width = 48;
  if (auto failure = expect("a target resize", 1, 1); !failure.empty())
    return failure;

  // max_samples stops the accumulation; later frames write the same image
  // with one submission. Raising it continues.
  settings.max_samples = 4;
  if (auto failure = expect("a limit", 10, 4); !failure.empty())
    return failure;
  if (frame.frames_rendered != 3)
    return "a limit submitted " + std::to_string(frame.frames_rendered) +
           " frames instead of 3";
  const std::vector<std::uint8_t> limited = frame.color.payload;
  if (auto failure = expect("at the limit", 3, 4); !failure.empty())
    return failure;
  if (frame.frames_rendered != 1 || frame.color.payload != limited)
    return "a frame at the limit did not write the same image once";
  settings.max_samples = 6;
  return expect("a raised limit", 5, 6);
}

// The reference images: the Cornell box at 1, 16, 64, 256 and 1024 spp.
constexpr std::array<std::uint32_t, 5> kReferenceLevels{1, 16, 64, 256, 1024};
constexpr std::uint32_t kReferenceSamples = 1024;
// The committed reference's variance comes from this many batches of
// kReferenceSamples / kReferenceBatches samples, the same samples as its mean.
constexpr std::uint32_t kReferenceBatches = 64;
// The first sample index of the images compared with the reference, far
// from the reference's own [0, kReferenceSamples), so the two are
// independent estimates.
constexpr std::uint32_t kComparedSampleIndex = 1U << 20;

std::filesystem::path ReferenceImagePath(
    const std::filesystem::path& directory, std::uint32_t samples) {
  std::ostringstream name;
  name << "cornell-box-" << std::setw(4) << std::setfill('0') << samples
       << "spp.pfm";
  return directory / name.str();
}

// The scene colour product's RGB. Every sample of the reference scene hits,
// so every pixel's alpha must be 1.
std::string ToRgb(const Lotus::ColorProduct& color, LotusHeadless::RgbImage& image) {
  const std::vector<float> values = ColorValues(color);
  image.width = color.width;
  image.height = color.height;
  image.values.clear();
  for (std::size_t pixel = 0; pixel * 4 < values.size(); ++pixel) {
    if (values[pixel * 4 + 3] != 1.0F)
      return "alpha " + std::to_string(values[pixel * 4 + 3]) + " at pixel " +
             std::to_string(pixel) + ": a sample missed the Cornell box";
    image.values.insert(image.values.end(), &values[pixel * 4], &values[pixel * 4 + 3]);
  }
  return {};
}

// Sets up the Cornell box in a fresh GPU scene, at the reference's size.
void SetReferenceScene(PathScenes& scenes) {
  LotusHeadless::SetCornellBox(scenes.World());
  scenes.Target().width = LotusHeadless::kReferenceSize;
  scenes.Target().height = LotusHeadless::kReferenceSize;
  scenes.Target().clear_color = {0.0F, 0.0F, 0.0F, 0.0F};
}

// Renders a fresh accumulation of `samples` from `sample_index` on.
std::string RenderReferenceImage(PathScenes& scenes, std::uint32_t sample_index,
    std::uint32_t samples, LotusHeadless::RgbImage& image) {
  Lotus::PathTracingSettings settings;
  settings.sample_index = sample_index;
  Lotus::GpuFrameEvidence frame;
  if (auto failure = scenes.Render(settings, frame, samples); !failure.empty())
    return failure;
  if (frame.samples_per_pixel != samples)
    return "the accumulation holds " + std::to_string(frame.samples_per_pixel) +
           " samples instead of " + std::to_string(samples);
  return ToRgb(frame.color, image);
}

// Deterministic mode: the reference's mean is kReferenceSamples from sample
// index 0 in one accumulation, and its per-sample variance is estimated from
// kReferenceBatches accumulations of the same samples. The batches come
// first: the first one starts at sample index 0 too, and would continue a
// preceding accumulation from there.
std::string WriteReference(PathScenes& scenes, const std::filesystem::path& directory) {
  SetReferenceScene(scenes);
  constexpr std::uint32_t kBatch = kReferenceSamples / kReferenceBatches;
  std::vector<double> sum;
  std::vector<double> squares;
  for (std::uint32_t batch = 0; batch < kReferenceBatches; ++batch) {
    LotusHeadless::RgbImage image;
    if (auto failure = RenderReferenceImage(scenes, batch * kBatch, kBatch, image);
        !failure.empty())
      return "batch " + std::to_string(batch) + ": " + failure;
    sum.resize(image.values.size());
    squares.resize(image.values.size());
    for (std::size_t i = 0; i < image.values.size(); ++i) {
      sum[i] += image.values[i];
      squares[i] += double{image.values[i]} * image.values[i];
    }
  }
  LotusHeadless::Reference reference;
  reference.samples = kReferenceSamples;
  if (auto failure = RenderReferenceImage(scenes, 0, kReferenceSamples, reference.mean);
      !failure.empty())
    return failure;
  const std::size_t count = reference.mean.values.size();
  if (sum.size() != count)
    return "the batches and the mean differ in size";
  reference.variance = reference.mean;
  for (std::size_t i = 0; i < count; ++i) {
    const double mean = sum[i] / kReferenceBatches;
    // The batches hold the mean's samples; only the summation order differs.
    if (std::abs(mean - reference.mean.values[i]) > 1e-6 + 1e-4 * std::abs(mean))
      return "the batches' mean " + std::to_string(mean) + " differs from " +
             std::to_string(reference.mean.values[i]) + " at value " +
             std::to_string(i);
    const double batch_variance = std::max(0.0,
        (squares[i] - kReferenceBatches * mean * mean) / (kReferenceBatches - 1));
    reference.variance.values[i] = static_cast<float>(batch_variance * kBatch);
  }
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (auto failure = LotusHeadless::WritePfm(
          directory / "cornell-box-mean.pfm", reference.mean);
      !failure.empty())
    return failure;
  return LotusHeadless::WritePfm(
      directory / "cornell-box-variance.pfm", reference.variance);
}

// The reference images: the Cornell box rendered at each of
// kReferenceLevels in one accumulation, from an independent sample index,
// written to `images` when it is not empty, and each compared with the
// committed reference by the DES-Q5 metric. Then the metric must reject a
// scene whose red wall is brighter, and the reference's own samples are
// compared bitwise with it, for the record.
std::string ReferenceFailure(PathScenes& scenes,
    const std::filesystem::path& reference_directory,
    const std::filesystem::path& images) {
  LotusHeadless::Reference reference;
  reference.samples = kReferenceSamples;
  if (auto failure = LotusHeadless::ReadPfm(
          reference_directory / "cornell-box-mean.pfm", reference.mean);
      !failure.empty())
    return failure;
  if (auto failure = LotusHeadless::ReadPfm(
          reference_directory / "cornell-box-variance.pfm", reference.variance);
      !failure.empty())
    return failure;
  if (reference.mean.width != LotusHeadless::kReferenceSize ||
      reference.mean.height != LotusHeadless::kReferenceSize)
    return "the reference is not " +
           std::to_string(LotusHeadless::kReferenceSize) + " pixels square";
  if (!images.empty()) {
    std::error_code error;
    std::filesystem::create_directories(images, error);
    if (error)
      return "cannot create " + images.string() + ": " + error.message();
  }
  SetReferenceScene(scenes);

  Lotus::PathTracingSettings settings;
  settings.sample_index = kComparedSampleIndex;
  std::uint32_t rendered = 0;
  for (const std::uint32_t level : kReferenceLevels) {
    Lotus::GpuFrameEvidence frame;
    if (auto failure = scenes.Render(settings, frame, level - rendered);
        !failure.empty())
      return failure;
    rendered = level;
    if (frame.samples_per_pixel != level)
      return "the accumulation holds " + std::to_string(frame.samples_per_pixel) +
             " samples instead of " + std::to_string(level);
    LotusHeadless::RgbImage image;
    if (auto failure = ToRgb(frame.color, image); !failure.empty())
      return failure;
    if (!images.empty()) {
      if (auto failure =
              LotusHeadless::WritePfm(ReferenceImagePath(images, level), image);
          !failure.empty())
        return failure;
    }
    const LotusHeadless::Comparison comparison =
        LotusHeadless::Compare(image, level, reference);
    if (!comparison.failure.empty())
      return comparison.failure;
    std::ostringstream line;
    line << std::fixed << std::setprecision(2) << level << " spp: image z "
         << comparison.image_z[0] << ' ' << comparison.image_z[1] << ' '
         << comparison.image_z[2] << ", " << comparison.tile
         << "px tiles max |z| " << comparison.max_tile_z << " mean z^2 "
         << comparison.mean_tile_z2;
    scenes.Summarize(line.str());
  }

  // The metric's power: a red wall reflecting 10% more must not match.
  LotusHeadless::SetCornellRedWall(scenes.World(), 1.1F);
  LotusHeadless::RgbImage brighter;
  if (auto failure = RenderReferenceImage(
          scenes, kComparedSampleIndex, kReferenceSamples, brighter);
      !failure.empty())
    return "red wall 10% brighter: " + failure;
  const LotusHeadless::Comparison rejected =
      LotusHeadless::Compare(brighter, kReferenceSamples, reference);
  if (rejected.failure.empty())
    return "the comparison accepted a red wall reflecting 10% more: max |z| " +
           std::to_string(rejected.max_tile_z);
  std::ostringstream line;
  line << std::fixed << std::setprecision(2)
       << "red wall 10% brighter rejected at max |z| " << rejected.max_tile_z;
  scenes.Summarize(line.str());
  LotusHeadless::SetCornellRedWall(scenes.World(), 1.0F);

  // On the device and build that wrote the reference, deterministic mode
  // reproduces it bit for bit; elsewhere it need not.
  LotusHeadless::RgbImage own;
  if (auto failure = RenderReferenceImage(scenes, 0, kReferenceSamples, own);
      !failure.empty())
    return "the reference's samples: " + failure;
  std::size_t differing = 0;
  for (std::size_t i = 0; i < own.values.size(); ++i)
    differing +=
        std::memcmp(&own.values[i], &reference.mean.values[i], sizeof(float)) != 0;
  scenes.Summarize(differing == 0
                       ? std::string("the reference's samples reproduce it bit for bit")
                       : "the reference's samples differ from it in " +
                             std::to_string(differing) + " values");
  return {};
}

std::string Status(Lotus::FrameStatus status) {
  switch (status) {
  case Lotus::FrameStatus::Pass:
    return "pass";
  case Lotus::FrameStatus::Fail:
    return "fail";
  case Lotus::FrameStatus::Skip:
    return "skip";
  }
  return "fail";
}

} // namespace

int main(int argc, char** argv) {
  Session session;
  session.started = static_cast<long long>(std::time(nullptr));
  session.target = "lotus-headless";
  // Start time plus pid: unique per invocation, so a later session supersedes
  // an earlier one, and a portable identifier the report model accepts.
  session.id = "headless-" + std::to_string(session.started) + "-" +
               std::to_string(CurrentProcessId());

  const std::filesystem::path executable_directory =
      std::filesystem::absolute(argv[0]).parent_path();
  std::string report_path = "renderer-report.json";
  bool install_tree = false;
  std::filesystem::path reference_directory = executable_directory / "reference";
  std::filesystem::path images;
  std::filesystem::path write_reference;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--report" && index + 1 < argc) {
      report_path = argv[++index];
    } else if (argument == "--install-tree") {
      install_tree = true;
    } else if (argument == "--reference" && index + 1 < argc) {
      reference_directory = argv[++index];
    } else if (argument == "--images" && index + 1 < argc) {
      images = argv[++index];
    } else if (argument == "--write-reference" && index + 1 < argc) {
      write_reference = argv[++index];
    } else {
      std::cerr << "usage: lotus-headless [--report <path>] [--install-tree]\n"
                   "           [--reference <directory>] [--images <directory>]\n"
                   "       lotus-headless --write-reference <directory>\n";
      return 2;
    }
  }

  Lotus::RenderWorld world;
  world.SetBootstrapTriangle();
  world.SetCamera(BootstrapCamera());
  const Lotus::FrameSnapshot first = world.Commit();
  const Lotus::FrameSnapshot unchanged = world.Commit();
  const Lotus::DrawSummary draw = Lotus::ExtractDrawSummary(first);
  const bool core_ok = first.revision == 1 && unchanged.revision == first.revision &&
                       draw.draw_count == 1 && draw.triangle_count == 1;

  const Lotus::BackendCapability capability = Lotus::ProbeVulkanBackend();
  const std::filesystem::path shader_directory = executable_directory / "shaders";
  Lotus::OffscreenTarget target;
  target.width = 64;
  target.height = 64;
  Lotus::FrameStatus setup_status = Lotus::FrameStatus::Fail;
  std::string setup_error;
  const std::unique_ptr<Lotus::OffscreenRenderer> renderer =
      Lotus::CreateOffscreenRenderer(
          (shader_directory / "triangle.vert.spv").string(),
          (shader_directory / "triangle.frag.spv").string(), setup_status,
          setup_error, {(shader_directory / "path_trace.vert.spv").string(), (shader_directory / "path_trace.frag.spv").string()});
  if (!write_reference.empty()) {
    if (!renderer) {
      std::cerr << "cannot create the renderer: " << setup_error << '\n';
      return 1;
    }
    if (const auto ray_query = renderer->RayQueryCapability(); !ray_query.available) {
      std::cerr << "the reference needs ray queries: " << ray_query.detail << '\n';
      return 1;
    }
    PathScenes scenes(*renderer);
    if (const auto failure = WriteReference(scenes, write_reference); !failure.empty()) {
      std::cerr << "cannot write the reference: " << failure << '\n';
      return 1;
    }
    std::cout << "wrote the reference to " << write_reference.string() << '\n';
    return 0;
  }
  // The scene runs first, so the frames' validation count covers it too.
  std::string scene_failure;
  AccelerationVerdict acceleration;
  SceneTimestampVerdict scene_timestamps;
  if (renderer) {
    scene_failure = SceneUploadFailure(*renderer, acceleration);
    if (scene_failure.empty()) {
      scene_timestamps = SceneTimestamps(*renderer);
    }
  }
  Lotus::GpuFrameEvidence frame;
  if (renderer) {
    frame = renderer->Render(draw, target, 1000);
  } else {
    frame.status = setup_status;
    frame.detail = setup_error;
  }

  bool color_ok = false;
  bool depth_ok = false;
  bool persistence_ok = false;
  bool clears_ok = false;
  // The renderer's validation count covers every frame it rendered.
  Lotus::GpuFrameEvidence last = frame;
  if (frame.status == Lotus::FrameStatus::Pass) {
    const std::size_t center =
        (frame.color.height / 2U) * frame.color.row_pitch +
        (frame.color.width / 2U) * 4U;
    color_ok = frame.color.width == 64 && frame.color.height == 64 &&
               frame.color.row_pitch == 64U * 4U &&
               frame.color.pixel_format == "rgba8-unorm" &&
               frame.color.origin == "top-left" &&
               frame.color.color_space == "linear" &&
               frame.color.payload.size() == 64U * 64U * 4U &&
               center + 3U < frame.color.payload.size() &&
               frame.color.payload[center] > 150U &&
               frame.color.payload[center + 1U] < 100U &&
               frame.color.payload[center + 2U] < 80U &&
               frame.color.payload[center + 3U] > 240U;

    const std::size_t depth_center =
        (frame.depth.height / 2U) * frame.depth.width + frame.depth.width / 2U;
    depth_ok = frame.depth.width == 64 && frame.depth.height == 64 &&
               frame.depth.row_pitch == 64U * sizeof(float) &&
               frame.depth.pixel_format == "d32-sfloat" &&
               frame.depth.origin == "top-left" &&
               frame.depth.payload.size() == 64U * 64U &&
               depth_center < frame.depth.payload.size() &&
               frame.depth.payload[depth_center] > 0.0F &&
               frame.depth.payload[depth_center] < 0.9F &&
               frame.depth.payload.front() > 0.99F;
    // Another frame at the same size reuses the targets; a new size
    // recreates them once.
    const Lotus::GpuFrameEvidence again = renderer->Render(draw, target, 1);
    Lotus::OffscreenTarget resized = target;
    resized.width = 96;
    resized.height = 48;
    const Lotus::GpuFrameEvidence resized_frame =
        renderer->Render(draw, resized, 1);
    if (resized_frame.status == Lotus::FrameStatus::Pass) {
      last = resized_frame;
    }
    persistence_ok =
        frame.frames_rendered == 1000 && frame.completion == 1000 &&
        frame.target_creations == 1 &&
        again.status == Lotus::FrameStatus::Pass &&
        again.completion == 1001 && again.target_creations == 1 &&
        resized_frame.status == Lotus::FrameStatus::Pass &&
        resized_frame.completion == 1002 &&
        resized_frame.target_creations == 2 &&
        resized_frame.color.width == 96 && resized_frame.color.height == 48 &&
        resized_frame.color.payload.size() == 96U * 48U * 4U &&
        resized_frame.depth.payload.size() == 96U * 48U;
    // A cropped frame clears the entire target, and an empty scene replaces
    // the previous triangle. Clear changes must not recreate the targets.
    Lotus::OffscreenTarget cropped = target;
    cropped.clear_color = {0.25F, 0.5F, 0.75F, 1.0F};
    cropped.data_window = {0, 0, 8, 8};
    const auto crop = renderer->Render(draw, cropped, 1);
    Lotus::OffscreenTarget empty_target = cropped;
    empty_target.clear_depth = 0.375F;
    const auto empty = renderer->Render(Lotus::DrawSummary{}, empty_target, 1);
    if (empty.status == Lotus::FrameStatus::Pass) {
      last = empty;
    }
    const std::vector<std::uint8_t> clear_pixel{64, 128, 191, 255};
    // UNORM conversion may round a half-integer either way (0.5 -> 127/128).
    const auto near_byte = [](std::uint8_t expected, std::uint8_t actual) {
      return std::abs(int(expected) - int(actual)) <= 1;
    };
    clears_ok = crop.status == Lotus::FrameStatus::Pass &&
                crop.color.payload.size() == 64U * 64U * 4U &&
                std::equal(clear_pixel.begin(), clear_pixel.end(),
                    crop.color.payload.begin() + center, near_byte) &&
                crop.depth.payload[depth_center] == 1.0F &&
                empty.status == Lotus::FrameStatus::Pass &&
                empty.target_creations == crop.target_creations &&
                empty.color.payload.size() == 64U * 64U * 4U &&
                empty.depth.payload.size() == 64U * 64U;
    for (std::size_t pixel = 0; clears_ok && pixel < 64U * 64U; ++pixel) {
      clears_ok = std::equal(clear_pixel.begin(), clear_pixel.end(),
                      empty.color.payload.begin() + pixel * 4U, near_byte) &&
                  empty.depth.payload[pixel] == 0.375F;
    }
    if (!clears_ok) {
      std::cerr << "crop: " << crop.detail << "; empty: " << empty.detail << '\n';
      if (empty.color.payload.size() >= 4) {
        std::cerr << "empty clear: " << int(empty.color.payload[0]) << ','
                  << int(empty.color.payload[1]) << ','
                  << int(empty.color.payload[2]) << ','
                  << int(empty.color.payload[3]) << '\n';
      }
    }
    empty_target.clear_color_enabled = false;
    empty_target.clear_depth_enabled = false;
    empty_target.clear_color = {};
    empty_target.clear_depth = 1.0F;
    const auto preserved = renderer->Render(Lotus::DrawSummary{}, empty_target, 1);
    clears_ok = clears_ok && preserved.status == Lotus::FrameStatus::Pass &&
                preserved.color.payload == empty.color.payload &&
                preserved.depth.payload == empty.depth.payload &&
                preserved.target_creations == empty.target_creations;
    if (preserved.status == Lotus::FrameStatus::Pass) {
      last = preserved;
    }
  }

  std::vector<Check> checks;
  if (renderer) {
    const auto ray_query = renderer->RayQueryCapability();
    checks.push_back({"renderer.ray_query.capability",
        ray_query.available ? "pass" : "skip", ray_query.detail});
    if (ray_query.available) {
      Lotus::GpuFrameEvidence measured;
      const auto ray_failure = PrimaryRayFailure(*renderer, last, measured);
      checks.push_back({"renderer.ray_query.triangle", ray_failure.empty() ? "pass" : "fail", ray_failure});
      checks.push_back({"renderer.ray_query.timestamp",
          measured.primary_ray_timestamp_available ? "pass" : "skip",
          measured.primary_ray_timestamp_available
              ? "perspective insertion primary_ray_gpu_ms=" + std::to_string(measured.primary_ray_gpu_ms)
              : "the graphics queue has no timestamp support"});
      // Each scene starts from, and leaves, an empty GPU scene.
      PathScenes scenes(*renderer);
      const auto run = [&](const char* id, const auto& scenario) {
        std::string failure = scenario(scenes);
        const std::string summary = scenes.TakeSummary();
        if (auto cleared = scenes.Clear(); failure.empty())
          failure = std::move(cleared);
        checks.push_back({id, failure.empty() ? "pass" : "fail",
            failure.empty() ? summary : failure});
      };
      run("renderer.path.bsdf", BsdfFailure);
      run("renderer.path.normals", NormalsFailure);
      run("renderer.path.textures", TexturesFailure);
      run("renderer.path.normal_maps", NormalMapsFailure);
      run("renderer.path.opacity", OpacityFailure);
      run("renderer.path.multibounce", MultibounceFailure);
      run("renderer.path.accumulation", AccumulationFailure);
      run("renderer.path.reference", [&](PathScenes& reference_scenes) {
        return ReferenceFailure(reference_scenes, reference_directory, images);
      });
    } else {
      checks.push_back({"renderer.ray_query.triangle", "skip", ray_query.detail});
      checks.push_back({"renderer.ray_query.timestamp", "skip", ray_query.detail});
      checks.push_back({"renderer.path.bsdf", "skip", ray_query.detail});
      checks.push_back({"renderer.path.normals", "skip", ray_query.detail});
      checks.push_back({"renderer.path.textures", "skip", ray_query.detail});
      checks.push_back({"renderer.path.normal_maps", "skip", ray_query.detail});
      checks.push_back({"renderer.path.opacity", "skip", ray_query.detail});
      checks.push_back({"renderer.path.multibounce", "skip", ray_query.detail});
      checks.push_back({"renderer.path.accumulation", "skip", ray_query.detail});
      checks.push_back({"renderer.path.reference", "skip", ray_query.detail});
    }
  } else {
    checks.push_back({"renderer.ray_query.capability", Status(setup_status), setup_error});
    checks.push_back({"renderer.ray_query.triangle", Status(setup_status), setup_error});
    checks.push_back({"renderer.ray_query.timestamp", Status(setup_status), setup_error});
    checks.push_back({"renderer.path.bsdf", Status(setup_status), setup_error});
    checks.push_back({"renderer.path.normals", Status(setup_status), setup_error});
    checks.push_back({"renderer.path.textures", Status(setup_status), setup_error});
    checks.push_back({"renderer.path.normal_maps", Status(setup_status), setup_error});
    checks.push_back({"renderer.path.opacity", Status(setup_status), setup_error});
    checks.push_back({"renderer.path.multibounce", Status(setup_status), setup_error});
    checks.push_back({"renderer.path.accumulation", Status(setup_status), setup_error});
    checks.push_back({"renderer.path.reference", Status(setup_status), setup_error});
  }
  checks.push_back({"renderer.core.boundary", core_ok ? "pass" : "fail",
      core_ok ? "" : "commit/extraction contract mismatch"});
  checks.push_back({"renderer.backend.capability",
      capability.available ? "pass" : "skip", capability.detail});
  checks.push_back({"renderer.gpu.frame", Status(frame.status), frame.detail});
  if (renderer) {
    checks.push_back({"renderer.scene.upload",
        scene_failure.empty() ? "pass" : "fail", scene_failure});
    if (!scene_failure.empty()) {
      checks.push_back({"renderer.scene.acceleration", "skip",
          "renderer.scene.upload did not pass: " + scene_failure});
    } else if (!acceleration.available) {
      checks.push_back({"renderer.scene.acceleration", "skip",
          acceleration.detail});
    } else {
      checks.push_back({"renderer.scene.acceleration",
          acceleration.failure.empty() ? "pass" : "fail",
          acceleration.failure});
    }
    if (!scene_failure.empty()) {
      checks.push_back({"renderer.scene.timestamp", "skip",
          "renderer.scene.upload did not pass: " + scene_failure});
    } else if (!scene_timestamps.failure.empty()) {
      checks.push_back({"renderer.scene.timestamp", "fail",
          scene_timestamps.failure});
    } else {
      checks.push_back({"renderer.scene.timestamp",
          scene_timestamps.available ? "pass" : "skip",
          scene_timestamps.detail});
    }
  } else {
    checks.push_back({"renderer.scene.upload", Status(setup_status),
        setup_error});
    checks.push_back({"renderer.scene.acceleration", Status(setup_status),
        setup_error});
    checks.push_back({"renderer.scene.timestamp", Status(setup_status),
        setup_error});
  }
  if (last.validation_available) {
    checks.push_back({"renderer.validation.messages",
        last.validation_message_count == 0 ? "pass" : "fail",
        last.validation_message_count == 0
            ? ""
            : last.validation_detail});
  } else {
    checks.push_back({"renderer.validation.messages", "skip",
        last.validation_detail.empty()
            ? "Vulkan validation capture was unavailable"
            : last.validation_detail});
  }
  if (frame.status == Lotus::FrameStatus::Pass) {
    checks.push_back({"renderer.render_product.color", color_ok ? "pass" : "fail",
        color_ok ? "" : "RGBA8 metadata or center pixel mismatch"});
    checks.push_back({"renderer.render_product.depth", depth_ok ? "pass" : "fail",
        depth_ok ? "" : "depth metadata or numeric payload mismatch"});
    checks.push_back({"renderer.frame.persistence",
        persistence_ok ? "pass" : "fail",
        persistence_ok ? ""
                       : "1,000-frame completion count, target reuse or "
                         "resize mismatch"});
    checks.push_back({"renderer.aov.clears", clears_ok ? "pass" : "fail",
        clears_ok ? "" : "crop, empty-scene clear or target reuse mismatch"});
  } else {
    const std::string dependent = "renderer.gpu.frame did not pass: " + frame.detail;
    checks.push_back({"renderer.render_product.color", "skip", dependent});
    checks.push_back({"renderer.render_product.depth", "skip", dependent});
    checks.push_back({"renderer.frame.persistence", "skip", dependent});
    checks.push_back({"renderer.aov.clears", "skip", dependent});
  }
  checks.push_back({"renderer.install_tree", install_tree ? "pass" : "skip",
      install_tree ? "" : "run the renderer install-tree CTest"});
#if defined(LOTUS_HAS_HYDRA2)
  const std::string hydra_detail =
      "the co-built Hydra adapter is exercised by its OpenUSD CTests";
#else
  const std::string hydra_detail =
      "configure with LOTUS_ENABLE_HYDRA2=ON and a matching OpenUSD SDK";
#endif
  checks.push_back({"renderer.plugin.discovery", "skip", hydra_detail});
  checks.push_back({"renderer.delegate.creation", "skip", hydra_detail});
  checks.push_back({"renderer.render_buffer.cpu", "skip", hydra_detail});
  checks.push_back({"renderer.host.first_frame", "skip", hydra_detail});
  checks.push_back({"renderer.host.stable_update", "skip", hydra_detail});

  const bool failed = std::any_of(checks.begin(), checks.end(),
      [](const Check& check) {
        return check.status == "fail";
      });

  // The session concludes here, with every check already decided -- the report
  // is published only once this run has actually finished, and it says so.
  //
  // The outcome describes *this harness*, not the verdicts it reached: it ran
  // every check to a decision, so it succeeded even when some of those checks
  // failed. Conflating the two would mark the session a failure and make its
  // own PASSes unmergeable, so a run with any FAIL could not report the
  // checks that passed alongside it. Failing checks are carried by `checks`;
  // a failed session is one that could not produce them at all.
  session.completed = static_cast<long long>(std::time(nullptr));
  session.succeeded = true;
  if (!WriteReport(report_path, checks, frame, session)) {
    std::cerr << "cannot write renderer report: " << report_path << '\n';
    return 1;
  }
  return failed ? 1 : 0;
}
