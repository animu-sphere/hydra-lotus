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
  std::array<std::uint64_t, 11> rejections{};
  std::uint64_t compared_pixels = 0;
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
    const auto& products = result.temporal;
    Check(products.validity.size() == products.current.size() &&
              products.validity_row_pitch == target.width * sizeof(TemporalValidityPixel),
        "validity AOV layout");
    for (std::uint32_t y = 0; y < target.height; ++y) {
      for (std::uint32_t x = 0; x < target.width; ++x) {
        const auto i = y * target.width + x;
        const auto expected = ReprojectTemporalHistory(products.current[i], x, y,
            target.width, target.height, products.previous, temporal.validation);
        const auto& actual = products.validity[i];
        if (actual.valid != expected.valid || actual.rejection != expected.rejection ||
            actual.previous_index != expected.previous_index || actual.reserved != 0) {
          std::cerr << "frame=" << temporal.frame_index << " pixel=" << x << ',' << y
                    << " GPU rejection=" << static_cast<unsigned>(actual.rejection)
                    << " CPU rejection=" << static_cast<unsigned>(expected.rejection)
                    << " GPU fetch=" << actual.previous_index << " CPU fetch=" << expected.previous_index << '\n';
          throw std::runtime_error("GPU shared validity differs from CPU oracle");
        }
        ++rejections[static_cast<unsigned>(actual.rejection)];
        ++compared_pixels;
      }
    }
    ++temporal.frame_index;
    return result;
  };
  const auto first = Render(3);
  Check(!first.temporal.history_available && first.samples_per_pixel == 3, "first frame/sample count");
  const auto stationary = Render(2);
  for (const auto& p : stationary.temporal.validity) {
    Check(p.valid == 1 && p.rejection == TemporalRejection::None, "stationary history rejected");
  }
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
  temporal.validation.max_motion_pixels = 1;
  camera.view = Translation(-0.75F, 0, 1);
  world.SetCamera(camera);
  (void)Render();
  temporal.validation.max_motion_pixels = 32;
  camera.view = Translation(-0.5F, 0, 1);
  world.SetCamera(camera);
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

  // Self-disocclusion keeps the stable placement identity: one mesh contains
  // a background plane and a small foreground quad at half its depth.
  target.width = 64;
  target.height = 48;
  target.display_window = {};
  target.data_window = {};
  world.SetCamera(Perspective());
  auto occluded = kPlane;
  occluded.positions.insert(occluded.positions.end(),
      {{-0.5F, -0.5F, -2}, {0.5F, -0.5F, -2}, {0.5F, 0.5F, -2}, {-0.5F, 0.5F, -2}});
  occluded.triangles.insert(occluded.triangles.end(), {{4, 5, 6}, {4, 6, 7}});
  occluded.source_faces.insert(occluded.source_faces.end(), {1, 1});
  world.SetMesh("/plane", occluded, {});
  (void)Render();
  auto revealing = Perspective();
  revealing.view = Translation(-0.5F);
  world.SetCamera(revealing);
  (void)Render();

  // A different placement crossing a formerly visible background supplies
  // the independent identity rejection case.
  world.SetMesh("/plane", kPlane, {});
  MeshGeometry foreground{{{-0.5F, -0.5F, -2}, {0.5F, -0.5F, -2},
                              {0.5F, 0.5F, -2}, {-0.5F, 0.5F, -2}},
      {{0, 1, 2}, {0, 2, 3}}, {0, 0}};
  world.SetMesh("/occluder", foreground, {});
  (void)Render();
  MeshInstance leaving;
  leaving.world_from_object = Translation(1.5F);
  world.SetMeshInstance("/occluder", leaving);
  (void)Render();
  world.RemoveMesh("/occluder");

  // Invalid thresholds are classified by the shared pass and recover on the
  // next compatible frame; they do not invalidate the history lifecycle.
  temporal.validation.minimum_normal_cosine = 2;
  (void)Render();
  temporal.validation = {};
  world.RemoveMesh("/plane");
  world.RemoveMesh("/a");
  const auto removed = Render();
  for (const auto& p : removed.temporal.current)
    Check(p.flags == 0 && p.instance_id == 0, "empty scene retained surfaces");
  Check(removed.temporal.timestamp_available == stationary.temporal.timestamp_available,
      "empty scene lost timestamp");
  for (const auto reason : {TemporalRejection::None, TemporalRejection::Miss,
           TemporalRejection::InvalidData, TemporalRejection::InstanceChanged,
           TemporalRejection::MaterialChanged, TemporalRejection::LargeMotion,
           TemporalRejection::DepthDiscontinuity, TemporalRejection::NormalDiscontinuity,
           TemporalRejection::HistoryUnavailable, TemporalRejection::CorrespondenceMissing,
           TemporalRejection::ReprojectionOutside}) {
    if (rejections[static_cast<unsigned>(reason)] == 0) {
      std::cerr << "unexercised rejection code=" << static_cast<unsigned>(reason) << '\n';
      throw std::runtime_error("GPU rejection coverage incomplete");
    }
  }
  std::cout << (integrator == Integrator::Reference ? "reference" : "wavefront")
            << " temporal PASS; independent projected pixels=" << valid
            << " object-motion valid pixels=" << object_valid
            << " surface_gpu_ms=" << stationary.temporal.primary_surface_gpu_ms
            << " reprojection_gpu_ms=" << stationary.temporal.reprojection_gpu_ms
            << " compared_pixels=" << compared_pixels
            << " validity_bytes=" << stationary.temporal.validity_bytes
            << " surface_bytes=" << stationary.temporal.surface_bytes
            << " metadata_bytes=" << stationary.temporal.metadata_bytes << '\n';
  std::cout << "rejection histogram:";
  for (const auto count : rejections)
    std::cout << ' ' << count;
  std::cout << '\n';
}

std::vector<float> Colors(const Lotus::GpuFrameEvidence& frame) {
  std::vector<float> values(frame.color.payload.size() / sizeof(float));
  std::memcpy(values.data(), frame.color.payload.data(), frame.color.payload.size());
  return values;
}

void AccumulationSequence(Lotus::OffscreenRenderer& renderer,
    Lotus::OffscreenRenderer& reference, Lotus::Integrator integrator) {
  using namespace Lotus;
  RenderWorld world;
  SceneExtraction extraction, reference_extraction;
  world.SetCamera(Perspective());
  world.SetMesh("/plane", kPlane, {});
  Material material;
  material.base_color = {0.6F, 0.4F, 0.2F};
  material.metallic = 1;
  material.roughness = 0.5F;
  world.SetMaterial("/material", material);
  world.BindMaterial("/plane", "/material");
  world.SetEnvironment({1, 2, 3});
  OffscreenTarget target;
  target.width = 64;
  target.height = 48;
  PathTracingSettings settings;
  settings.integrator = integrator;
  settings.max_bounces = 1;
  settings.sample_index = 31;
  TemporalFrameSettings temporal;
  temporal.accumulate_radiance = true;
  temporal.max_history_samples = 32;
  FrameSnapshot snapshot;
  std::optional<GpuFrameEvidence> preceding;
  std::optional<std::uint32_t> reference_index;
  std::uint32_t next_index = settings.sample_index;
  std::uint64_t compared = 0, reused = 0, rejected_hits = 0;
  double max_blend_error = 0, moving_mse = 0, fresh_moving_mse = 0;
  std::uint32_t moving_comparisons = 0;
  auto Fresh = [&](std::uint32_t index, std::uint32_t samples) {
    auto path = settings;
    path.sample_index = index;
    if (reference_index == index) {
      auto prime = path;
      prime.sample_index ^= 0x80000000u;
      Check(reference.RenderScene(ExtractDrawSummary(snapshot), target, 1, prime).status == FrameStatus::Pass,
          "reference batch restart failed");
    }
    auto frame = reference.RenderScene(ExtractDrawSummary(snapshot), target, samples, path);
    reference_index = index;
    Check(frame.status == FrameStatus::Pass, frame.detail.c_str());
    Check(frame.validation_message_count == 0, frame.validation_detail.c_str());
    return frame;
  };
  auto Render = [&](std::uint32_t samples = 1) {
    snapshot = world.Commit();
    Check(renderer.UpdateScene(extraction.Update(snapshot)).status == FrameStatus::Pass, "temporal upload");
    Check(reference.UpdateScene(reference_extraction.Update(snapshot)).status == FrameStatus::Pass, "reference upload");
    auto frame = renderer.RenderTemporalScene(snapshot, target, samples, temporal, settings);
    Check(frame.status == FrameStatus::Pass, frame.detail.c_str());
    Check(frame.validation_message_count == 0, frame.validation_detail.c_str());
    const auto& product = frame.temporal;
    Check(product.radiance_available && product.radiance.size() == target.width * target.height &&
              product.radiance_row_pitch == target.width * sizeof(TemporalRadiancePixel),
        "radiance debug layout");
    Check(product.radiance_bytes == std::uint64_t{target.width} * target.height * 144, "radiance memory");
    Check(product.batch_samples == std::min(samples, settings.max_samples == 0 ? samples : settings.max_samples),
        "fresh batch sample count");
    if (!product.history_available)
      next_index = settings.sample_index;
    Check(product.batch_sample_index == next_index, "RNG repeated or skipped samples");
    next_index += product.batch_samples;
    const auto fresh = Fresh(product.batch_sample_index, samples);
    const auto batch = Colors(fresh), output = Colors(frame);
    if (product.history_available) {
      Check(preceding.has_value() && product.previous_radiance.size() == preceding->temporal.radiance.size(),
          "missing radiance history");
      Check(std::memcmp(product.previous_radiance.data(), preceding->temporal.radiance.data(),
                product.previous_radiance.size() * sizeof(TemporalRadiancePixel)) == 0,
          "radiance history was overwritten");
    } else
      Check(product.previous_radiance.empty(), "reset exposed radiance history");
    for (std::size_t i = 0; i < product.radiance.size(); ++i) {
      const auto& actual = product.radiance[i];
      const bool hit = (product.current[i].flags & 1u) != 0;
      const std::uint32_t fresh_count = hit ? std::min(product.batch_samples, temporal.max_history_samples) : 0u;
      std::uint32_t kept = 0;
      const TemporalRadiancePixel* old = nullptr;
      if (product.validity[i].valid) {
        old = &preceding->temporal.radiance.at(product.validity[i].previous_index);
        kept = std::min(old->sample_count, temporal.max_history_samples - fresh_count);
        ++reused;
      } else if (hit)
        ++rejected_hits;
      Check(actual.sample_count == fresh_count + kept && actual.reserved == std::array<std::uint32_t, 3>{},
          "bounded history sample count");
      for (unsigned c = 0; c < 4; ++c) {
        const double expected = kept == 0 ? batch[i * 4 + c] : (double(batch[i * 4 + c]) * fresh_count + double(old->color[c]) * kept) / (fresh_count + kept);
        const double error = std::abs(actual.color[c] - expected);
        max_blend_error = std::max(max_blend_error, error);
        Check(std::isfinite(actual.color[c]) && error < 0.00002, "temporal blend differs from independent fresh batch");
        Near(output[i * 4 + c], actual.color[c], "colour attachment differs from temporal history");
      }
      ++compared;
    }
    ++temporal.frame_index;
    preceding = frame;
    return frame;
  };
  auto Reset = [&](TemporalResetReason reason) {
    const auto frame = Render(2);
    Check(!frame.temporal.history_available && frame.temporal.reset_reason == reason, "radiance reset reason");
    const auto recovery = Render(2);
    Check(recovery.temporal.history_available, "radiance reset did not recover");
  };
  auto MeasureMotion = [&] {
    const auto frame = Render(2);
    Check(frame.temporal.history_available, "motion reset radiance globally");
    const auto truth = Colors(Fresh(8192, 256)), image = Colors(frame);
    const auto fresh = Colors(Fresh(frame.temporal.batch_sample_index, 2));
    double squared = 0, fresh_squared = 0;
    for (std::size_t i = 0; i < image.size(); ++i)
      if (i % 4 != 3) {
        squared += std::pow(double(image[i]) - truth[i], 2);
        fresh_squared += std::pow(double(fresh[i]) - truth[i], 2);
      }
    moving_mse += squared / (target.width * target.height * 3);
    fresh_moving_mse += fresh_squared / (target.width * target.height * 3);
    ++moving_comparisons;
  };

  // Unbounded-by-cap static phase: fresh disjoint RNG batches must converge
  // to the same mean as ordinary progressive transport of all 16 samples.
  GpuFrameEvidence stationary;
  for (unsigned i = 0; i < 8; ++i)
    stationary = Render(2);
  const auto progressive = Colors(Fresh(settings.sample_index, 16));
  const auto image = Colors(stationary);
  double static_error = 0;
  for (std::size_t i = 0; i < image.size(); ++i)
    static_error = std::max(static_error, std::abs(double(image[i]) - progressive[i]));
  Check(static_error < 0.00002, "static temporal convergence differs from progressive reference");
  for (const auto& p : stationary.temporal.radiance)
    Check(p.sample_count == 16, "static counts did not grow");
  for (unsigned i = 0; i < 10; ++i)
    (void)Render(2); // Reach and exceed the cap; the oracle checks each blend.
  (void)Render(64);  // A batch larger than the cap replaces the old mean.

  auto camera = Perspective();
  camera.view = Translation(-0.25F);
  world.SetCamera(camera);
  MeasureMotion();
  MeshInstance instance;
  instance.world_from_object = Translation(0.5F, 0.25F);
  world.SetMeshInstance("/plane", instance);
  MeasureMotion();
  camera.view = Translation(-0.5F, 0, -0.1F);
  world.SetCamera(camera);
  MeasureMotion();
  // All declared global incompatibilities reset and then recover.
  temporal.camera_cut = true;
  const auto cut = Render();
  Check(cut.temporal.reset_reason == TemporalResetReason::CameraCut, "radiance camera cut");
  temporal.camera_cut = false;
  (void)Render();
  ++temporal.history_key;
  Reset(TemporalResetReason::HistoryKeyChanged);
  ++temporal.frame_index;
  Reset(TemporalResetReason::FrameGap);
  target.display_window = {0, 0, 60, 48};
  Reset(TemporalResetReason::TargetChanged);
  target.data_window = {4, 2, 56, 44};
  Reset(TemporalResetReason::TargetChanged);
  target.width = 80;
  Reset(TemporalResetReason::TargetChanged);
  target.display_window = {};
  target.data_window = {};
  Reset(TemporalResetReason::TargetChanged);
  world.SetEnvironment({2, 1, 3});
  Reset(TemporalResetReason::SceneChanged);
  Light light;
  world.SetLight("/light", light);
  Reset(TemporalResetReason::SceneChanged);
  world.RemoveLight("/light");
  Reset(TemporalResetReason::SceneChanged);
  material.emission = {0.2F, 0.4F, 0.8F};
  world.SetMaterial("/material", material);
  Reset(TemporalResetReason::SceneChanged);
  Texture unused;
  unused.width = unused.height = 1;
  unused.texels = {255, 0, 0, 255};
  world.SetTexture("/unused", unused);
  Reset(TemporalResetReason::SceneChanged);
  auto deformed = kPlane;
  deformed.positions[0][0] -= 0.1F;
  world.SetMesh("/plane", deformed, instance);
  Reset(TemporalResetReason::SceneChanged);
  settings.light_sampling = LightSampling::Bsdf;
  Reset(TemporalResetReason::RenderSettingsChanged);
  settings.integrator = integrator == Integrator::Reference ? Integrator::Wavefront : Integrator::Reference;
  Reset(TemporalResetReason::RenderSettingsChanged);
  settings.integrator = integrator;
  Reset(TemporalResetReason::RenderSettingsChanged);
  ++settings.sample_index;
  Reset(TemporalResetReason::RenderSettingsChanged);
  settings.max_bounces = 0;
  Reset(TemporalResetReason::RenderSettingsChanged);
  settings.max_samples = 1;
  Reset(TemporalResetReason::RenderSettingsChanged);
  temporal.max_history_samples = 4;
  Reset(TemporalResetReason::RenderSettingsChanged);
  target.clear_color = {0.1F, 0.2F, 0.3F, 1};
  Reset(TemporalResetReason::RenderSettingsChanged);

  // Invalid calls do not consume the preceding radiance/RNG stream.
  auto bad = temporal;
  --bad.frame_index;
  Check(renderer.RenderTemporalScene(snapshot, target, 1, bad, settings).status == FrameStatus::Fail, "duplicate radiance frame");
  bad = temporal;
  bad.max_history_samples = 0;
  Check(renderer.RenderTemporalScene(snapshot, target, 1, bad, settings).status == FrameStatus::Fail, "zero history cap");
  bad.max_history_samples = 65537;
  Check(renderer.RenderTemporalScene(snapshot, target, 1, bad, settings).status == FrameStatus::Fail, "oversized history cap");
  auto diagnostic = settings;
  diagnostic.output = SceneOutput::Albedo;
  Check(renderer.RenderTemporalScene(snapshot, target, 1, temporal, diagnostic).status == FrameStatus::Fail, "diagnostic radiance accumulation");
  auto preserve = target;
  preserve.clear_color_enabled = false;
  Check(renderer.RenderTemporalScene(snapshot, preserve, 1, temporal, settings).status == FrameStatus::Fail, "undefined fresh batch accepted");
  Check(renderer.RenderTemporalScene(snapshot, target, 0, temporal, settings).status == FrameStatus::Fail, "empty radiance batch");
  Check(Render().temporal.history_available, "failed radiance call consumed history");
  // Mode changes and ordinary rendering break radiance reuse explicitly.
  temporal.accumulate_radiance = false;
  const auto metadata = renderer.RenderTemporalScene(snapshot, target, 1, temporal, settings);
  Check(metadata.status == FrameStatus::Pass && !metadata.temporal.radiance_available, "metadata-only mode");
  ++temporal.frame_index;
  temporal.accumulate_radiance = true;
  Reset(TemporalResetReason::RenderSettingsChanged);
  Check(renderer.RenderScene(ExtractDrawSummary(snapshot), target, 1, settings).status == FrameStatus::Pass,
      "ordinary frame after radiance");
  Reset(TemporalResetReason::ExplicitReset);

  // Disocclusion: a foreground leaves a background of another colour. Old
  // foreground samples must not survive on a newly exposed background hit.
  auto foreground = kPlane;
  foreground.positions = {{-1, -1, -2}, {1, -1, -2}, {1, 1, -2}, {-1, 1, -2}};
  world.SetMesh("/foreground", foreground, {});
  Material red;
  red.emission = {8, 0, 0};
  world.SetMaterial("/red", red);
  world.BindMaterial("/foreground", "/red");
  Reset(TemporalResetReason::SceneChanged);
  MeshInstance leaving;
  leaving.world_from_object = Translation(1.5F);
  world.SetMeshInstance("/foreground", leaving);
  const auto disocclusion = Render();
  std::uint32_t exposed = 0;
  for (std::size_t i = 0; i < disocclusion.temporal.radiance.size(); ++i) {
    if (disocclusion.temporal.validity[i].rejection == TemporalRejection::InstanceChanged) {
      ++exposed;
      Check(disocclusion.temporal.radiance[i].sample_count == 1, "ghost foreground history on exposed surface");
    }
  }
  Check(exposed > 0, "moving foreground exercised no disocclusion");
  world.RemoveMesh("/foreground");
  world.RemoveMesh("/plane");
  Reset(TemporalResetReason::SceneChanged);
  for (const auto& p : preceding->temporal.radiance)
    Check(p.sample_count == 0, "empty scene retained radiance history");
  Check(preceding->temporal.accumulation_gpu_ms >= 0, "invalid accumulation timestamp");
  std::cout << (integrator == Integrator::Reference ? "reference" : "wavefront")
            << " radiance PASS; compared_pixels=" << compared << " reused=" << reused
            << " rejected_hits=" << rejected_hits << " static_max_error=" << static_error
            << " max_blend_error=" << max_blend_error << " moving_mse=" << moving_mse / moving_comparisons
            << " fresh_moving_mse=" << fresh_moving_mse / moving_comparisons
            << " disoccluded_pixels=" << exposed << " accumulation_gpu_ms=" << stationary.temporal.accumulation_gpu_ms
            << " radiance_bytes=" << stationary.temporal.radiance_bytes << '\n';
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
  // The validity pass must run before any TLAS/scene descriptor exists.
  Lotus::RenderWorld empty_world;
  Lotus::SceneExtraction empty_extraction;
  const auto empty_snapshot = empty_world.Commit();
  Check(renderer->UpdateScene(empty_extraction.Update(empty_snapshot)).status == Lotus::FrameStatus::Pass,
      "initial empty upload failed");
  Lotus::OffscreenTarget empty_target;
  empty_target.width = 4;
  empty_target.height = 3;
  const auto empty_frame = renderer->RenderTemporalScene(empty_snapshot, empty_target, 1, {});
  Check(empty_frame.status == Lotus::FrameStatus::Pass, empty_frame.detail.c_str());
  Check(empty_frame.validation_message_count == 0, empty_frame.validation_detail.c_str());
  Check(empty_frame.temporal.validity.size() == 12, "initial empty validity extent");
  for (const auto& p : empty_frame.temporal.validity)
    Check(p.valid == 0 && p.rejection == Lotus::TemporalRejection::Miss &&
              p.previous_index == UINT32_MAX,
        "initial empty scene fetched history");
  Lotus::TemporalFrameSettings empty_radiance;
  empty_radiance.frame_index = 1;
  empty_radiance.accumulate_radiance = true;
  const auto empty_accumulation = renderer->RenderTemporalScene(empty_snapshot, empty_target, 1, empty_radiance);
  Check(empty_accumulation.status == Lotus::FrameStatus::Pass, empty_accumulation.detail.c_str());
  Check(empty_accumulation.validation_message_count == 0, empty_accumulation.validation_detail.c_str());
  Check(empty_accumulation.temporal.radiance_available && !empty_accumulation.temporal.history_available,
      "initial empty radiance stream");
  for (const auto& p : empty_accumulation.temporal.radiance)
    Check(p.sample_count == 0 && p.color == std::array<float, 4>{}, "initial empty radiance retained samples");
  Check(renderer->RenderScene(Lotus::ExtractDrawSummary(empty_snapshot), empty_target, 1).status ==
            Lotus::FrameStatus::Pass,
      "initial empty stream reset failed");
  Sequence(*renderer, Lotus::Integrator::Reference);
  // A fresh renderer supplies an independent history/identity stream.
  renderer = Lotus::CreateOffscreenRenderer(vertex, fragment, status, detail, shaders);
  Check(renderer != nullptr, detail.c_str());
  Sequence(*renderer, Lotus::Integrator::Wavefront);
  for (auto integrator : {Lotus::Integrator::Reference, Lotus::Integrator::Wavefront}) {
    renderer = Lotus::CreateOffscreenRenderer(vertex, fragment, status, detail, shaders);
    Check(renderer != nullptr, detail.c_str());
    auto reference = Lotus::CreateOffscreenRenderer(vertex, fragment, status, detail, shaders);
    Check(reference != nullptr, detail.c_str());
    AccumulationSequence(*renderer, *reference, integrator);
  }
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
