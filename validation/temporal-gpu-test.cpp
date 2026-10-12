// SPDX-License-Identifier: Apache-2.0
#include <lotus/vulkan_backend.hpp>

#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
void Near(float value, double expected, const char* message) {
  if (std::abs(value - expected) > 0.0002 || !std::isfinite(value)) {
    std::cerr << message << ": actual=" << value << " expected=" << expected << '\n';
    throw std::runtime_error(message);
  }
}
Lotus::Matrix4 Translation(float x, float y = 0, float z = 0) {
  auto matrix = Lotus::IdentityMatrix();
  matrix[12] = x;
  matrix[13] = y;
  matrix[14] = z;
  return matrix;
}
Lotus::Camera Perspective() {
  Lotus::Camera camera;
  camera.projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1.2F, -1, 0, 0, -2.2F, 0};
  return camera;
}
const Lotus::MeshGeometry kPlane{
    {{-12, -12, -4}, {12, -12, -4}, {12, 12, -4}, {-12, 12, -4}},
    {{0, 1, 2}, {0, 2, 3}}, {0, 0}};

bool SamePixels(const std::vector<Lotus::TemporalPixel>& a,
    const std::vector<Lotus::TemporalPixel>& b) {
  return a.size() == b.size() &&
         std::memcmp(a.data(), b.data(), a.size() * sizeof(a[0])) == 0;
}

void Sequence(Lotus::OffscreenRenderer& renderer, Lotus::Integrator integrator) {
  using namespace Lotus;
  RenderWorld world;
  SceneExtraction extraction;
  world.SetCamera(Perspective());
  world.SetMesh("/plane", kPlane, {});
  Material material;
  material.emission = {0.25F, 0.5F, 1.0F};
  world.SetMaterial("/material", material);
  world.BindMaterial("/plane", "/material");
  OffscreenTarget target;
  target.width = 64;
  target.height = 48;
  PathTracingSettings settings;
  settings.integrator = integrator;
  settings.max_bounces = 0;
  TemporalFrameSettings temporal;
  auto snapshot = world.Commit();
  auto Render = [&](std::uint32_t samples = 1) {
    snapshot = world.Commit();
    Check(renderer.UpdateScene(extraction.Update(snapshot)).status == FrameStatus::Pass,
        "scene upload failed");
    const auto result = renderer.RenderTemporalScene(snapshot, target, samples, temporal, settings);
    Check(result.status == FrameStatus::Pass, result.detail.c_str());
    Check(result.validation_message_count == 0, result.validation_detail.c_str());
    Check(result.temporal.available && result.temporal.width == target.width &&
              result.temporal.height == target.height &&
              result.temporal.row_pitch == target.width * sizeof(TemporalPixel),
        "product layout");
    ++temporal.frame_index;
    return result;
  };
  const auto first = Render(3);
  Check(!first.temporal.history_available && first.samples_per_pixel == 3, "first frame/sample count");
  const auto stationary = Render(2);
  Check(stationary.temporal.history_available && stationary.samples_per_pixel == 5,
      "spp advanced frame history incorrectly");
  Check(SamePixels(first.temporal.current, stationary.temporal.previous), "GPU previous buffer not retained");
  for (const auto& p : stationary.temporal.current) {
    Check(p.flags == 7 && p.instance_id != 0 && p.material_id != 0, "surface identity/correspondence");
    Near(p.normal_depth[2], 1, "stationary normal");
    Near(p.normal_depth[3], 4, "stationary linear depth");
    Near(p.motion_depth[0], 0, "stationary motion x");
    Near(p.motion_depth[1], 0, "stationary motion y");
    Near(p.motion_depth[2], 4, "stationary expected depth");
    Near(p.motion_depth[3], 1, "stationary reprojection");
  }
  // Ordinary transport produces byte-identical colour/depth at the same spp.
  const auto ordinary = renderer.RenderScene(ExtractDrawSummary(snapshot), target, 1, settings);
  Check(ordinary.status == FrameStatus::Pass && ordinary.color.payload == stationary.color.payload &&
            ordinary.depth.payload == stationary.depth.payload,
      "temporal metadata changed progressive output");
  Check(!Render().temporal.history_available, "ordinary frame did not break temporal stream");
  (void)Render();

  auto camera_only = Perspective();
  camera_only.view = Translation(-0.25F);
  world.SetCamera(camera_only);
  const auto camera_move = Render();
  for (std::size_t i = 0; i < camera_move.temporal.current.size(); ++i) {
    const auto& p = camera_move.temporal.current[i];
    const bool inside = i % target.width < target.width - 2;
    Check((p.motion_depth[3] != 0) == inside, "camera-only previous-frame clipping");
    if (inside) {
      Near(p.motion_depth[0], 2, "camera-only backward motion x");
      Near(p.motion_depth[1], 0, "camera-only backward motion y");
      Near(p.motion_depth[2], 4, "camera-only expected depth");
    }
  }
  MeshInstance object_only;
  object_only.world_from_object = Translation(0.5F, 0.25F);
  world.SetMeshInstance("/plane", object_only);
  const auto object_move = Render();
  std::size_t object_valid = 0;
  for (std::uint32_t y = 0; y < target.height; ++y) {
    for (std::uint32_t x = 0; x < target.width; ++x) {
      const auto& p = object_move.temporal.current[y * target.width + x];
      // Same camera; this rigid translation moves pixels +4 in X, -1.5 in Y.
      const bool inside = x + 0.5 >= 4 && y + 0.5 + 1.5 < target.height;
      Check((p.motion_depth[3] != 0) == inside, "object-only previous-frame clipping");
      if (inside) {
        Near(p.motion_depth[0], -4, "object-only backward motion x");
        Near(p.motion_depth[1], 1.5, "object-only backward motion y");
        ++object_valid;
      }
    }
  }
  world.SetCamera(Perspective());
  world.SetMeshInstance("/plane", {});
  (void)Render();

  // Independent pinhole oracle: plane Z=-4, object translation +(.5,.25,0),
  // camera eye +(.25,0,-1) (view translation -(.25,0,-1)).
  MeshInstance moved;
  moved.world_from_object = Translation(0.5F, 0.25F);
  world.SetMeshInstance("/plane", moved);
  auto camera = Perspective();
  camera.view = Translation(-0.25F, 0, 1);
  world.SetCamera(camera);
  const auto moving = Render();
  Check(moving.samples_per_pixel == 1, "progressive camera/scene reset changed");
  std::size_t valid = 0;
  for (std::uint32_t y = 0; y < target.height; ++y) {
    for (std::uint32_t x = 0; x < target.width; ++x) {
      const auto& p = moving.temporal.current[y * target.width + x];
      const double nx = 2.0 * (x + 0.5) / target.width - 1.0;
      const double ny = 1.0 - 2.0 * (y + 0.5) / target.height;
      const double old_x = (0.5 + (3 * nx - 0.25) / 8) * target.width;
      const double old_y = (0.5 - (3 * ny - 0.25) / 8) * target.height;
      Check(p.motion_depth[3] == 1, "moving in-frame projection unavailable");
      Near(p.motion_depth[0], old_x - (x + 0.5), "pinhole backward motion x");
      Near(p.motion_depth[1], old_y - (y + 0.5), "pinhole backward motion y");
      Near(p.normal_depth[3], 3, "current camera depth");
      Near(p.motion_depth[2], 4, "previous camera depth");
      Check(p.instance_id == stationary.temporal.current[0].instance_id &&
                p.material_id == stationary.temporal.current[0].material_id,
          "motion changed stable identity");
      ++valid;
    }
  }
  // Insert earlier geometry/material entries; the surface's GPU slots shift.
  MeshInstance outside;
  outside.world_from_object = Translation(1000);
  world.SetMesh("/a", kPlane, outside);
  world.SetMaterial("/a", {});
  const auto churn = Render();
  Check(churn.temporal.current[0].instance_id == moving.temporal.current[0].instance_id &&
            churn.temporal.current[0].material_id == moving.temporal.current[0].material_id &&
            churn.temporal.current[0].flags == 7,
      "GPU slot churn changed stable identity");
  material.base_color[0] = 0.1F;
  world.SetMaterial("/material", material);
  const auto edit = Render();
  Check(edit.temporal.current[0].instance_id == churn.temporal.current[0].instance_id &&
            edit.temporal.current[0].material_id != churn.temporal.current[0].material_id &&
            edit.temporal.current[0].flags == 3,
      "material edit retained material identity");

  // Failed calls must leave the preceding successful history untouched.
  auto duplicate = temporal;
  --duplicate.frame_index;
  Check(renderer.RenderTemporalScene(snapshot, target, 1, duplicate, settings).status == FrameStatus::Fail,
      "duplicate frame accepted");
  Check(renderer.RenderTemporalScene(snapshot, target, 0, temporal, settings).status == FrameStatus::Fail,
      "zero samples accepted");
  auto empty = target;
  empty.width = 0;
  Check(renderer.RenderTemporalScene(snapshot, empty, 1, temporal, settings).status == FrameStatus::Fail,
      "empty target accepted");
  const auto after_failure = Render();
  Check(SamePixels(after_failure.temporal.previous, edit.temporal.current), "failed call consumed history");
  settings.max_samples = after_failure.samples_per_pixel;
  const auto frozen = Render(5);
  Check(frozen.samples_per_pixel == after_failure.samples_per_pixel &&
      SamePixels(frozen.temporal.previous, after_failure.temporal.current),
      "converged transport did not advance a temporal frame");
  settings.max_samples = 0;
  target.clear_depth = 0;
  const auto rejected_depth = Render();
  Check(rejected_depth.temporal.current[0].flags == 7 && rejected_depth.depth.payload[0] == 0,
      "temporal surfaces depended on the depth attachment test");
  target.clear_depth = 1;

  world.SetEnvironment({0.1F, 0.1F, 0.1F});
  const auto unuploaded = world.Commit();
  Check(renderer.RenderTemporalScene(unuploaded, target, 1, temporal, settings).status == FrameStatus::Fail,
      "unuploaded snapshot accepted");
  const auto after_mismatch = Render();
  Check(SamePixels(after_mismatch.temporal.previous, rejected_depth.temporal.current),
      "snapshot mismatch consumed history");

  // Texture identity changes follow the resources the bound material reads.
  material.base_color_texture = TextureInput{};
  material.base_color_texture->texture = "/used";
  world.SetMaterial("/material", material);
  const auto textured = Render();
  Texture texel{1, 1, TextureFormat::Rgba8Unorm, {128, 128, 128, 255}};
  world.SetTexture("/unused", texel);
  const auto unused = Render();
  Check(unused.temporal.current[0].material_id == textured.temporal.current[0].material_id &&
            unused.temporal.current[0].flags == 7,
      "unused texture changed GPU material identity");
  world.SetTexture("/used", texel);
  const auto used = Render();
  Check(used.temporal.current[0].material_id != unused.temporal.current[0].material_id &&
            used.temporal.current[0].flags == 3,
      "used texture retained GPU material identity");

  // Offset display window and cropped data window, plus target resize.
  target.width = 80;
  target.height = 60;
  target.display_window = {-8, 4, 96, 48};
  target.data_window = {5, 7, 60, 40};
  const auto resized = Render();
  Check(!resized.temporal.history_available &&
            resized.temporal.reset_reason == TemporalResetReason::TargetChanged,
      "resize did not reset");
  const auto cropped = Render();
  for (std::uint32_t y = 0; y < target.height; ++y) {
    for (std::uint32_t x = 0; x < target.width; ++x) {
      const auto& p = cropped.temporal.current[y * target.width + x];
      const bool inside = x >= 5 && x < 65 && y >= 7 && y < 47;
      Check((p.flags != 0) == inside, "cropped data window contains stale surfaces");
      if (inside) {
        Near(p.motion_depth[0], 0, "cropped stationary motion x");
        Near(p.motion_depth[1], 0, "cropped stationary motion y");
        Near(p.motion_depth[3], 1, "cropped stationary reprojection");
      }
    }
  }
  target.data_window = {6, 7, 60, 40};
  Check(!Render().temporal.history_available, "framing edit did not reset history");
  (void)Render();
  camera.view = Translation(-0.5F, 0, 1);
  world.SetCamera(camera);
  const auto crop_move = Render();
  for (std::uint32_t y = 7; y < 47; ++y) {
    for (std::uint32_t x = 6; x < 66; ++x) {
      const auto& p = crop_move.temporal.current[y * target.width + x];
      // Backward +0.25/3 * half-display-width = 4 pixels.
      Check((p.motion_depth[3] != 0) == (x < 62), "cropped history bounds ignored");
      if (x < 62)
        Near(p.motion_depth[0], 4, "offset-display camera motion");
    }
  }
  temporal.camera_cut = true;
  Check(Render().temporal.reset_reason == TemporalResetReason::CameraCut, "camera cut ignored");
  temporal.camera_cut = false;
  ++temporal.history_key;
  Check(Render().temporal.reset_reason == TemporalResetReason::HistoryKeyChanged, "history key ignored");
  ++temporal.frame_index;
  Check(Render().temporal.reset_reason == TemporalResetReason::FrameGap, "frame gap ignored");

  // A rotated plane checks that normals come from transformed geometry.
  MeshInstance tilted;
  tilted.world_from_object = {0.5F, 0, -0.8660254F, 0, 0, 1, 0, 0,
      0.8660254F, 0, 0.5F, 0, 3.4641016F, 0, -2, 1};
  world.SetMeshInstance("/plane", tilted);
  const auto rotated = Render();
  std::size_t normal_hits = 0;
  for (const auto& p : rotated.temporal.current) {
    if ((p.flags & 1) == 0)
      continue;
    Near(p.normal_depth[0], 0.8660254, "rotated geometric normal x");
    Near(p.normal_depth[1], 0, "rotated geometric normal y");
    Near(p.normal_depth[2], 0.5, "rotated geometric normal z");
    ++normal_hits;
  }
  Check(normal_hits > 100, "rotated normal oracle exercised no surface");
  world.SetMeshInstance("/plane", moved);
  auto orthographic = camera;
  orthographic.projection = {0.25F, 0, 0, 0, 0, 0.25F, 0, 0,
      0, 0, -0.2F, 0, 0, 0, -1.2F, 1};
  temporal.camera_cut = true;
  world.SetCamera(orthographic);
  (void)Render();
  temporal.camera_cut = false;
  orthographic.view = Translation(-0.25F, 0, 1);
  world.SetCamera(orthographic);
  const auto ortho_move = Render();
  for (std::uint32_t y = 7; y < 47; ++y) {
    for (std::uint32_t x = 6; x < 66; ++x) {
      const auto& p = ortho_move.temporal.current[y * target.width + x];
      // Eye moved -0.25: backward -0.25/8 * display width = -3 pixels.
      Check((p.motion_depth[3] != 0) == (x >= 9), "orthographic history bounds ignored");
      if (x >= 9) {
        Near(p.motion_depth[0], -3, "orthographic backward motion");
        Near(p.motion_depth[2], 3, "orthographic previous depth");
      }
    }
  }

  // Instancer count churn invalidates correspondence even when slot 0 stays.
  moved.instancer_transforms = std::vector<Matrix4>{IdentityMatrix()};
  world.SetMeshInstance("/plane", moved);
  const auto instanced = Render();
  Check(instanced.temporal.current[7 * target.width + 6].flags == 1, "instancing mode paired history");
  (void)Render();
  moved.instancer_transforms->push_back(Translation(1000));
  world.SetMeshInstance("/plane", moved);
  const auto count_change = Render();
  Check(count_change.temporal.current[7 * target.width + 6].flags == 1 &&
            count_change.temporal.current[7 * target.width + 6].instance_id !=
                instanced.temporal.current[7 * target.width + 6].instance_id,
      "instancer count paired history");
  world.RemoveMesh("/plane");
  world.RemoveMesh("/a");
  const auto removed = Render();
  for (const auto& p : removed.temporal.current)
    Check(p.flags == 0 && p.instance_id == 0, "empty scene retained surfaces");
  Check(removed.temporal.timestamp_available == stationary.temporal.timestamp_available,
      "empty scene lost timestamp");
  std::cout << (integrator == Integrator::Reference ? "reference" : "wavefront")
            << " temporal PASS; independent projected pixels=" << valid
            << " object-motion valid pixels=" << object_valid
            << " surface_gpu_ms=" << stationary.temporal.primary_surface_gpu_ms
            << " surface_bytes=" << stationary.temporal.surface_bytes
            << " metadata_bytes=" << stationary.temporal.metadata_bytes << '\n';
}
} // namespace

int main(int argc, char** argv) try {
  Lotus::FrameStatus status;
  std::string detail;
  const std::string vertex = argc == 6 ? argv[1] : "";
  const std::string fragment = argc == 6 ? argv[2] : "";
  Lotus::RayQueryShaders shaders;
  if (argc == 6)
    shaders = {argv[3], argv[4], argv[5]};
  // The Vulkan-disabled build has no shader artifacts.
  if (argc != 6 && !Lotus::ProbeVulkanBackend().available) {
    std::cout << "SKIP: " << Lotus::ProbeVulkanBackend().detail << '\n';
    return 77;
  }
  auto renderer = Lotus::CreateOffscreenRenderer(vertex, fragment, status, detail, shaders);
  if (!renderer) {
    std::cout << (status == Lotus::FrameStatus::Skip ? "SKIP: " : "FAIL: ") << detail << '\n';
    return status == Lotus::FrameStatus::Skip ? 77 : 1;
  }
  if (!renderer->RayQueryCapability().available) {
    std::cout << "SKIP: " << renderer->RayQueryCapability().detail << '\n';
    return 77;
  }
  Sequence(*renderer, Lotus::Integrator::Reference);
  // A fresh renderer supplies an independent history/identity stream.
  renderer = Lotus::CreateOffscreenRenderer(vertex, fragment, status, detail, shaders);
  Check(renderer != nullptr, detail.c_str());
  Sequence(*renderer, Lotus::Integrator::Wavefront);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
