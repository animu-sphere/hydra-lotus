// SPDX-License-Identifier: Apache-2.0
#include <lotus/extraction.hpp>
#include <lotus/render_world.hpp>
#include <lotus/vulkan_backend.hpp>

#include <algorithm>
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
  world.SetMesh("/triangle", {{{0, 0, 1}, {1, 0, 1}, {0, 1, 1}}, {{0, 1, 2}},
      {0}}, Lotus::MeshInstance{});
  world.SetMesh("/empty", {{{0, 0, 0}}, {}, {}}, Lotus::MeshInstance{});
  if (auto failure = apply("insertion"); !failure.empty()) return failure;
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
  if (auto failure = apply("hide"); !failure.empty()) return failure;
  if (before.resident_geometries != 2 || before.instance_count != 1 ||
      before.geometry_uploads != 2) {
    return "hide: geometry was released or uploaded";
  }
  expect(before.blas_builds == 2 && before.tlas_builds == 2 &&
             before.tlas_updates == 1,
      "hide", "expected a TLAS rebuild and no BLAS build");

  const std::uint32_t triangle_slot =
      renderer.ReadBackScene().instances.at(0).geometry_slot;
  world.SetMesh("/triangle", {{{0, 0, 2}, {1, 0, 2}, {0, 1, 2}}, {{0, 1, 2}},
      {0}}, Lotus::MeshInstance{});
  if (auto failure = apply("point edit"); !failure.empty()) return failure;
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
  if (auto failure = apply("removal"); !failure.empty()) return failure;
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
          setup_error);
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
