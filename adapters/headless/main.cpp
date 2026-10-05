// SPDX-License-Identifier: Apache-2.0
#include <lotus/extraction.hpp>
#include <lotus/render_world.hpp>
#include <lotus/vulkan_backend.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

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

// Whether the GPU scene's buffers hold exactly the scene's resident geometry
// and its visible instances, in the update plan's order.
bool SceneMatches(const Lotus::GpuSceneContents& contents,
    const Lotus::LotusScene& scene) {
  if (contents.status != Lotus::FrameStatus::Pass) {
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
    if (instance >= contents.instances.size()) {
      return false;
    }
    const Lotus::GpuInstanceContents& gpu = contents.instances[instance++];
    const auto geometry = std::find_if(contents.geometries.begin(),
        contents.geometries.end(), [&](const Lotus::GpuGeometryContents& g) {
          return g.slot == gpu.geometry_slot;
        });
    if (geometry == contents.geometries.end() ||
        geometry->positions != mesh.geometry->positions ||
        geometry->triangles != mesh.geometry->triangles ||
        gpu.world_from_object != mesh.instance.world_from_object) {
      return false;
    }
  }
  return instance == contents.instances.size() &&
         resident.size() == contents.geometries.size();
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
// transform edit, a hide, a point edit and removal, comparing the device
// buffers with the CPU scene after each. Returns an empty string on success.
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

  const std::uint32_t triangle_slot =
      renderer.ReadBackScene().instances.at(0).geometry_slot;
  world.SetMesh("/triangle", {{{0, 0, 2}, {1, 0, 2}, {0, 1, 2}}, {{0, 1, 2}}, {0}}, Lotus::MeshInstance{});
  if (auto failure = apply("point edit"); !failure.empty())
    return failure;
  if (before.resident_geometries != 2 || before.geometry_uploads != 3 ||
      before.geometry_releases != 1 ||
      renderer.ReadBackScene().instances.at(0).geometry_slot != triangle_slot) {
    return "point edit: the geometry was not replaced in its slot";
  }
  expect(before.blas_builds == 3 && before.tlas_builds == 3 &&
             before.tlas_updates == 1,
      "point edit", "expected one BLAS build and a TLAS rebuild");

  world.RemoveMesh("/quad");
  world.RemoveMesh("/triangle");
  world.RemoveMesh("/empty");
  if (auto failure = apply("removal"); !failure.empty())
    return failure;
  if (before.resident_geometries != 0 || before.instance_count != 0 ||
      before.geometry_bytes != 0 || before.geometry_releases != 3 ||
      stats().upload_submissions != before.upload_submissions) {
    return "removal: the GPU scene kept geometry or instances";
  }
  expect(before.blas_builds == 3 && before.tlas_builds == 4 &&
             before.tlas_updates == 1,
      "removal", "expected an empty TLAS rebuild");
  return {};
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
  if (frame.color.payload.size() != std::size_t{target.width} * target.height * 4 ||
      frame.depth.payload.size() != std::size_t{target.width} * target.height)
    return "incorrect primary-ray product size";
  const auto camera = Lotus::ExtractDrawSummary(snapshot).world_to_clip;
  struct Triangle {
    std::array<std::array<double, 4>, 3> clip;
  };
  std::vector<Triangle> triangles;
  for (const auto& [key, mesh] : snapshot.scene->meshes) {
    (void)key;
    if (!mesh.instance.visible)
      continue;
    for (const auto& indices : mesh.geometry->triangles) {
      Triangle triangle;
      bool clipped = false;
      for (int corner = 0; corner < 3; ++corner) {
        const auto& position = mesh.geometry->positions[indices[corner]];
        const auto world = Transform(mesh.instance.world_from_object,
            {position[0], position[1], position[2], 1.0});
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
        const int expected = static_cast<int>(std::lround(std::clamp(color[channel], 0.0, 1.0) * 255));
        if (std::abs(expected - int(frame.color.payload[pixel * 4 + channel])) > 2)
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
  const auto check = [&](const char* step) -> std::string {
    const auto snapshot = world.Commit();
    const auto upload = renderer.UpdateScene(extraction.Update(snapshot));
    if (upload.status != Lotus::FrameStatus::Pass)
      return std::string(step) + ": " + upload.detail;
    last = renderer.RenderScene(Lotus::ExtractDrawSummary(snapshot), target, 1);
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
  if (renderer.RenderScene(singular, target, 1).status != Lotus::FrameStatus::Fail)
    return "singular camera was accepted";
  world.RemoveMesh("triangle");
  if (auto error = check("removal and empty scene"); !error.empty())
    return error;
  target.clear_color_enabled = false;
  target.clear_depth_enabled = false;
  const auto preserved = renderer.RenderScene(Lotus::ExtractDrawSummary(world.Commit()), target, 1);
  if (preserved.status != Lotus::FrameStatus::Pass || preserved.color.payload != last.color.payload ||
      preserved.depth.payload != last.depth.payload)
    return "empty-scene no-clear preservation differs";
  last = preserved;
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

  std::string report_path = "renderer-report.json";
  bool install_tree = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--report" && index + 1 < argc) {
      report_path = argv[++index];
    } else if (argument == "--install-tree") {
      install_tree = true;
    } else {
      std::cerr << "usage: lotus-headless [--report <path>] [--install-tree]\n";
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
  const std::filesystem::path shader_directory =
      std::filesystem::absolute(argv[0]).parent_path() / "shaders";
  Lotus::OffscreenTarget target;
  target.width = 64;
  target.height = 64;
  Lotus::FrameStatus setup_status = Lotus::FrameStatus::Fail;
  std::string setup_error;
  const std::unique_ptr<Lotus::OffscreenRenderer> renderer =
      Lotus::CreateOffscreenRenderer(
          (shader_directory / "triangle.vert.spv").string(),
          (shader_directory / "triangle.frag.spv").string(), setup_status,
          setup_error, {(shader_directory / "primary_ray.vert.spv").string(), (shader_directory / "primary_ray.frag.spv").string()});
  // The scene runs first, so the frames' validation count covers it too.
  std::string scene_failure;
  AccelerationVerdict acceleration;
  if (renderer) {
    scene_failure = SceneUploadFailure(*renderer, acceleration);
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
    } else {
      checks.push_back({"renderer.ray_query.triangle", "skip", ray_query.detail});
      checks.push_back({"renderer.ray_query.timestamp", "skip", ray_query.detail});
    }
  } else {
    checks.push_back({"renderer.ray_query.capability", Status(setup_status), setup_error});
    checks.push_back({"renderer.ray_query.triangle", Status(setup_status), setup_error});
    checks.push_back({"renderer.ray_query.timestamp", Status(setup_status), setup_error});
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
  } else {
    checks.push_back({"renderer.scene.upload", Status(setup_status),
        setup_error});
    checks.push_back({"renderer.scene.acceleration", Status(setup_status),
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
