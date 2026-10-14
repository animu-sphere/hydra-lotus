// SPDX-License-Identifier: Apache-2.0
#include <lotus/vulkan_backend.hpp>
#include "../backend/vulkan/reservoir.hpp"
#include "../adapters/headless/reference.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
using namespace Lotus;
constexpr double kPi = 3.14159265358979323846;
std::filesystem::path image_root;
void Check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
void Near(double value, double expected, double tolerance, const char* message) {
  if (!std::isfinite(value) || std::abs(value - expected) > tolerance) {
    std::cerr << message << ": actual=" << value << " expected=" << expected << '\n';
    throw std::runtime_error(message);
  }
}
Matrix4 Translation(float x, float y, float z) {
  auto m = IdentityMatrix();
  m[12] = x; m[13] = y; m[14] = z;
  return m;
}
Camera Perspective(float zoom = 1) {
  Camera camera;
  camera.projection = {zoom, 0, 0, 0, 0, zoom, 0, 0, 0, 0, -1.2F, -1, 0, 0, -2.2F, 0};
  return camera;
}
const MeshGeometry kPlane{
    {{-12, -12, -4}, {12, -12, -4}, {12, 12, -4}, {-12, 12, -4}},
    {{0, 1, 2}, {0, 2, 3}}, {0, 0}};
MeshGeometry Panel(float x0, float x1, float y0, float y1, float z) {
  return {{{x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z}},
      {{0, 1, 2}, {0, 2, 3}}, {0, 0}};
}
Material Diffuse() {
  Material m; m.ior = 1; m.base_color = {0.5F, 0.75F, 1};
  return m;
}
void PlaneWorld(RenderWorld& world, float zoom = 1) {
  world.SetCamera(Perspective(zoom));
  world.SetMesh("/plane", kPlane, {});
  world.SetMaterial("/diffuse", Diffuse());
  world.BindMaterial("/plane", "/diffuse");
}
std::uint32_t Hash(std::uint32_t value) {
  const auto state = value * 747796405U + 2891336453U;
  const auto word = ((state >> ((state >> 28U) + 4U)) ^ state) * 277803737U;
  return (word >> 22U) ^ word;
}
float Random(std::uint32_t& state) {
  state = state * 747796405U + 2891336453U;
  const auto word = ((state >> ((state >> 28U) + 4U)) ^ state) * 277803737U;
  return float(((word >> 22U) ^ word) >> 8U) * (1.0F / 16777216.0F);
}
std::array<double, 3> Mean(const GpuFrameEvidence& frame) {
  std::array<double, 3> result{};
  for (std::size_t i = 0; i < frame.color.payload.size() / 16; ++i) {
    float rgba[4];
    std::memcpy(rgba, frame.color.payload.data() + 16 * i, 16);
    for (int c = 0; c < 3; ++c) {
      Check(std::isfinite(rgba[c]), "nonfinite radiance");
      result[c] += rgba[c];
    }
  }
  for (auto& v : result) v /= frame.color.payload.size() / 16;
  return result;
}
void Save(const GpuFrameEvidence& frame, const std::string& name) {
  if (image_root.empty()) return;
  std::filesystem::create_directories(image_root);
  LotusHeadless::RgbImage image{frame.color.width, frame.color.height, {}};
  image.values.resize(std::size_t{image.width} * image.height * 3);
  for (std::size_t i = 0; i < image.values.size() / 3; ++i)
    std::memcpy(image.values.data() + 3 * i, frame.color.payload.data() + 16 * i, 12);
  const auto error = LotusHeadless::WritePfm(image_root / (name + ".pfm"), image);
  Check(error.empty(), error.c_str());
}
struct Fixture {
  OffscreenRenderer& renderer;
  RenderWorld world;
  SceneExtraction extraction;
  OffscreenTarget target;
  PathTracingSettings settings;
  std::uint64_t frame_index = 0;
  Fixture(OffscreenRenderer& r, Integrator integrator, unsigned size = 8) : renderer(r) {
    target.width = target.height = size;
    settings.integrator = integrator;
    settings.light_sampling = LightSampling::RestirInitial;
    settings.max_bounces = 1;
    settings.sample_index = 17;
  }
  GpuFrameEvidence Render(unsigned samples = 1) {
    auto snapshot = world.Commit();
    const auto upload = renderer.UpdateScene(extraction.Update(snapshot));
    Check(upload.status == FrameStatus::Pass, upload.detail.c_str());
    auto frame = renderer.RenderScene(ExtractDrawSummary(snapshot), target, samples, settings);
    Check(frame.status == FrameStatus::Pass, frame.detail.c_str());
    Check(frame.validation_message_count == 0, frame.validation_detail.c_str());
    return frame;
  }
  GpuFrameEvidence Temporal(TemporalFrameSettings temporal = {}) {
    settings.restir_temporal = true;
    temporal.frame_index = frame_index;
    auto snapshot = world.Commit();
    const auto upload = renderer.UpdateScene(extraction.Update(snapshot));
    Check(upload.status == FrameStatus::Pass, upload.detail.c_str());
    auto frame = renderer.RenderTemporalScene(snapshot, target, 1, temporal, settings);
    Check(frame.status == FrameStatus::Pass, frame.detail.c_str());
    Check(frame.validation_message_count == 0, frame.validation_detail.c_str());
    ++frame_index;
    return frame;
  }
};

void TemporalArithmetic(OffscreenRenderer& renderer, Integrator integrator) {
  Fixture f(renderer, integrator, 64);
  PlaneWorld(f.world);
  const std::array<std::array<float, 3>, 3> colors{{{2, 1, 0.5F}, {1, 2, 0.5F}, {0.5F, 1, 2}}};
  for (unsigned i = 0; i < 3; ++i) {
    Light light; light.shape = LightShape::Distant; light.angle = 0; light.radiance = colors[i];
    if (i == 2) light.world_from_light[10] = -1;
    f.world.SetLight(std::to_string(i), light);
  }
  f.settings.restir_candidates = 2;
  f.settings.restir_max_history_candidates = 3;
  GpuFrameEvidence previous;
  std::size_t zero_sources = 0;
  for (unsigned frame = 0; frame < 12; ++frame) {
    const auto actual = f.Temporal();
    Check(actual.restir.available && actual.restir.temporal.size() == 4096 &&
        actual.restir.temporal_row_pitch == 64 * sizeof(RestirReusePixel) &&
        actual.restir.history_available == (frame != 0), "temporal reservoir publication");
    Check(actual.restir.reservoir_bytes == 864 * 4096, "temporal reservoir memory accounting");
    if (actual.restir.timestamp_available)
      Check(actual.restir.temporal_gpu_ms > 0, "temporal reservoir timestamp");
    if (frame != 0)
      Check(std::memcmp(actual.restir.previous.data(), previous.restir.temporal.data(),
          4096 * sizeof(RestirReusePixel)) == 0, "published history is not immutable preceding output");
    for (unsigned slot = 0; slot < 4096; ++slot) {
      const auto& initial = actual.restir.initial[slot];
      auto expected = vulkan_internal::Reservoir{initial.light, initial.weight_sum,
          initial.selected_target, initial.candidate_count};
      auto uv = initial.uv;
      unsigned supporting = initial.candidate_count;
      if (frame != 0) {
        const auto& old = previous.restir.temporal[slot];
        const auto& p = old.reservoir;
        const auto source = vulkan_internal::Reservoir{p.light, p.weight_sum, p.selected_target, p.candidate_count};
        auto rng = Hash(slot ^ Hash((f.settings.sample_index + frame) ^ 0x94D049BBU));
        const float target = p.light == 0 ? 1.0F / float(kPi)
                            : p.light == 1 ? 1.5F / float(kPi) : 0;
        if (p.weight_sum == 0) ++zero_sources;
        if (vulkan_internal::MergeReevaluatedReservoir(expected, source,
                old.normalization, target, 3, Random(rng)))
          uv = p.uv;
        if (expected.weight_sum > 0)
          supporting += std::min(p.candidate_count, 3U);
        else supporting = 0;
      }
      const auto& r = actual.restir.temporal[slot];
      if (!(r.reservoir.light == expected.sample && r.reservoir.uv == uv &&
          r.reservoir.candidate_count == expected.candidate_count &&
          r.supporting_candidates == supporting &&
          r.previous_index == (frame != 0 ? slot : UINT32_MAX)))
        std::cerr << "frame=" << frame << " slot=" << slot << " light=" << r.reservoir.light
                  << '/' << expected.sample << " M=" << r.reservoir.candidate_count << '/' << expected.candidate_count
                  << " support=" << r.supporting_candidates << '/' << supporting
                  << " prev=" << r.previous_index << " flags=" << r.reservoir.flags << '\n';
      Check(r.reservoir.light == expected.sample && r.reservoir.uv == uv &&
          r.reservoir.candidate_count == expected.candidate_count &&
          r.supporting_candidates == supporting &&
          r.previous_index == (frame != 0 ? slot : UINT32_MAX), "temporal CPU/GPU payload/count/support");
      Near(r.reservoir.weight_sum, expected.weight_sum, 4e-6, "temporal CPU/GPU sum");
      const auto w = frame == 0 ? vulkan_internal::ReservoirNormalization(expected)
          : vulkan_internal::ReservoirReuseNormalization(expected, supporting);
      Near(r.normalization, w, 4e-6, "temporal resolved normalization");
      if (expected.weight_sum > 0)
        for (int c = 0; c < 3; ++c)
          Near(r.reservoir.contribution[c], Diffuse().base_color[c] * colors[expected.sample][c] / kPi * w,
              5e-6, "temporal selected RGB against independent Lambert");
      Check(r.reservoir.candidate_count <= 5, "history candidate cap not applied to count");
    }
    previous = actual;
  }
  Check(zero_sources != 0, "zero observed history streams were not covered");
  const auto mean = Mean(previous);
  for (int c = 0; c < 3; ++c)
    Near(mean[c], Diffuse().base_color[c] * (colors[0][c] + colors[1][c]) / kPi,
        0.025, "temporal independent integral");
  std::cout << "temporal CPU/GPU oracle: 12 x 4096; capped M<=5; zero sources=" << zero_sources
            << " reuse_ms=" << previous.restir.temporal_gpu_ms << '\n';
}

void TemporalLifecycle(OffscreenRenderer& renderer, Integrator integrator) {
  Fixture f(renderer, integrator, 16); PlaneWorld(f.world);
  Light light; light.shape = LightShape::Distant; light.angle = 0; light.radiance = {2, 1, 0.5F};
  f.world.SetLight("/light", light);
  auto first = f.Temporal();
  Check(!first.restir.history_available, "first reservoir frame reused history");
  auto reused = f.Temporal();
  Check(reused.restir.history_available && reused.restir.temporal[128].reservoir.candidate_count == 16,
      "static history not reused");
  auto camera = Perspective(); camera.view = Translation(-0.25F, 0, 0);
  f.world.SetCamera(camera);
  const auto moving = f.Temporal();
  Check(moving.restir.history_available, "camera motion reset reservoir stream");
  std::size_t valid = 0, displaced = 0;
  for (unsigned i = 0; i < moving.restir.temporal.size(); ++i) {
    const auto& p = moving.restir.temporal[i];
    if (p.previous_index != UINT32_MAX) {
      ++valid; displaced += p.previous_index != i;
      Check(p.previous_index == moving.temporal.validity[i].previous_index,
          "reservoir did not use shared reprojection");
    }
  }
  Check(valid > 0 && displaced > 0, "camera motion did not reproject reservoirs");
  MeshInstance placement; placement.world_from_object = Translation(0.25F, 0, 0);
  f.world.SetMesh("/plane", kPlane, placement);
  Check(f.Temporal().restir.history_available, "nonemissive rigid motion cut reservoir history");
  auto reset = [&](TemporalResetReason reason, TemporalFrameSettings temporal = {}) {
    auto frame = f.Temporal(temporal);
    Check(!frame.restir.history_available && frame.restir.previous.empty() &&
        frame.temporal.reset_reason == reason, "reservoir reset reason/history");
    for (unsigned i = 0; i < frame.restir.initial.size(); ++i)
      Check(std::memcmp(&frame.restir.initial[i], &frame.restir.temporal[i].reservoir,
          sizeof(RestirPixel)) == 0, "reset did not resolve initial-only estimator");
    return frame;
  };
  light.radiance = {4, 2, 1}; f.world.SetLight("/light", light);
  reset(TemporalResetReason::SceneChanged);
  (void)f.Temporal();
  f.settings.restir_max_history_candidates = 1;
  reset(TemporalResetReason::RenderSettingsChanged);
  (void)f.Temporal();
  TemporalFrameSettings cut; cut.camera_cut = true;
  reset(TemporalResetReason::CameraCut, cut);
  (void)f.Temporal();
  f.frame_index += 2;
  reset(TemporalResetReason::FrameGap);
  (void)f.Temporal();
  f.target.width = f.target.height = 8;
  reset(TemporalResetReason::TargetChanged);
  (void)f.Temporal();
  f.settings.restir_candidates = 1;
  reset(TemporalResetReason::RenderSettingsChanged);
  (void)f.Temporal();
  auto snapshot = f.world.Commit();
  auto bad = f.settings; bad.restir_max_history_candidates = 0;
  TemporalFrameSettings t; t.frame_index = f.frame_index;
  auto failed = renderer.RenderTemporalScene(snapshot, f.target, 1, t, bad);
  Check(failed.status == FrameStatus::Fail && !failed.restir.available && !failed.temporal.available,
      "invalid reuse request published history");
  Check(f.Temporal().restir.history_available, "invalid request consumed preceding history");
  // Reject incompatible settings before reallocating a differently sized
  // target's metadata; returning to the last target must keep its history.
  t.frame_index = f.frame_index;
  auto other_target = f.target; other_target.width = other_target.height = 12;
  bad = f.settings; bad.integrator = static_cast<Integrator>(99);
  failed = renderer.RenderTemporalScene(snapshot, other_target, 1, t, bad);
  Check(failed.status == FrameStatus::Fail && !failed.restir.available,
      "invalid integrator with resized target was accepted");
  Check(f.Temporal().restir.history_available, "invalid resized request destroyed history");
  bad = f.settings; bad.restir_max_history_candidates = 65537;
  t.frame_index = f.frame_index;
  Check(renderer.RenderTemporalScene(snapshot, f.target, 1, t, bad).status == FrameStatus::Fail,
      "history cap above 65536 accepted");
  f.settings.restir_max_history_candidates = 65536;
  reset(TemporalResetReason::RenderSettingsChanged);
  Check(f.Temporal().restir.history_available, "maximum valid history cap rejected");
  f.settings.restir_temporal = false;
  (void)f.Render();
  reset(TemporalResetReason::ExplicitReset);
  // Reservoir and radiance histories can be enabled together.
  TemporalFrameSettings radiance; radiance.accumulate_radiance = true;
  auto both = f.Temporal(radiance);
  Check(both.restir.available && both.temporal.radiance_available, "combined history unavailable");
  std::cout << "temporal lifecycle: camera/rigid motion, settings/light/cut/gap/target/ordinary resets PASS\n";
}

void TemporalSupport(OffscreenRenderer& renderer, Integrator integrator) {
  Fixture f(renderer, integrator, 64); PlaneWorld(f.world, 100);
  f.settings.restir_candidates = 1;
  f.settings.restir_max_history_candidates = 1;
  for (unsigned i = 0; i < 2; ++i) {
    Light light; light.shape = LightShape::Distant; light.angle = 0;
    light.radiance = {2, 1, 0.5F};
    light.world_from_light[8] = i == 0 ? -0.8F : 0.8F;
    light.world_from_light[10] = 0.6F;
    f.world.SetLight(std::to_string(i), light);
  }
  auto Tilt = [](float sine) {
    MeshInstance instance;
    instance.world_from_object = {0.5F, 0, -sine, 0, 0, 1, 0, 0,
        sine, 0, 0.5F, 0, 4 * sine, 0, -2, 1};
    return instance;
  };
  f.world.SetMeshInstance("/plane", Tilt(-0.8660254F));
  const auto source = f.Temporal();
  f.world.SetMeshInstance("/plane", Tilt(0.8660254F));
  TemporalFrameSettings temporal;
  temporal.validation.minimum_normal_cosine = -1;
  temporal.validation.relative_depth_tolerance = 1;
  const auto received = f.Temporal(temporal);
  unsigned covered = 0, zero_source = 0, receiving_zero = 0;
  for (unsigned i = 0; i < received.restir.temporal.size(); ++i) {
    const auto& r = received.restir.temporal[i];
    if (r.previous_index == UINT32_MAX) continue;
    ++covered;
    const auto& old = source.restir.temporal[r.previous_index];
    zero_source += old.reservoir.weight_sum == 0;
    receiving_zero += old.reservoir.weight_sum > 0;
    Check(r.reservoir.candidate_count == 2, "differing support lost zero-target history draws");
    if (r.reservoir.weight_sum > 0) {
      // Light 1 is supported only at the current tilted receiver. Source
      // draws must not enter the support denominator, even when observed zero.
      Check(r.reservoir.light == 1 && r.supporting_candidates == 1,
          "complementary support normalized by total represented count");
      Near(r.normalization, vulkan_internal::ReservoirReuseNormalization(
          {r.reservoir.light, r.reservoir.weight_sum, r.reservoir.selected_target, r.reservoir.candidate_count}, 1),
          2e-6, "GPU differing-support normalization");
    }
  }
  Check(covered > 1000 && zero_source > 0 && receiving_zero > 0,
      "complementary source support fixture did not exercise both streams");
  const auto estimate = Mean(received);
  f.settings.restir_temporal = false; f.settings.light_sampling = LightSampling::Mis;
  const auto baseline = Mean(f.Render(1024));
  for (int c = 0; c < 3; ++c)
    Near(estimate[c], baseline[c], 0.025, "moving differing-support temporal vs NEE/MIS");
  std::cout << "complementary support: accepted=" << covered << " zero streams=" << zero_source
            << " temporal_R=" << estimate[0] << " mis_R=" << baseline[0] << '\n';
}

void TemporalDisocclusion(OffscreenRenderer& renderer, Integrator integrator) {
  Fixture f(renderer, integrator, 32); PlaneWorld(f.world);
  f.world.SetMesh("/front", Panel(-1, 1, -1, 1, -3), {});
  auto front = Diffuse(); front.base_color = {0.9F, 0.2F, 0.1F};
  f.world.SetMaterial("/front", front); f.world.BindMaterial("/front", "/front");
  Light light; light.shape = LightShape::Distant; light.angle = 0;
  light.radiance = {2, 1, 0.5F};
  light.world_from_light[8] = 0.8F; light.world_from_light[10] = 0.6F;
  f.world.SetLight("/light", light);
  (void)f.Temporal(); (void)f.Temporal();
  MeshInstance moved; moved.world_from_object = Translation(1.5F, 0, 0);
  f.world.SetMeshInstance("/front", moved);
  const auto frame = f.Temporal();
  Check(frame.restir.history_available, "rigid occluder edit globally reset reservoirs");
  unsigned rejected = 0, reused = 0;
  for (unsigned i = 0; i < frame.restir.temporal.size(); ++i) {
    const auto& p = frame.restir.temporal[i];
    if (frame.temporal.validity[i].valid == 0) {
      ++rejected;
      Check(p.previous_index == UINT32_MAX && p.reservoir.candidate_count == 8,
          "disoccluded surface reused reservoir history");
    } else reused += p.previous_index != UINT32_MAX;
  }
  Check(rejected != 0 && reused != 0, "moving occluder did not exercise validity boundary");
  f.settings.restir_temporal = false; f.settings.light_sampling = LightSampling::Mis;
  f.settings.sample_index += 2;
  const auto baseline = f.Render();
  // One discrete parallel light makes both estimators exact, including the
  // newly cast shadow; use identical jitter and compare every RGB channel.
  for (unsigned i = 0; i < frame.color.payload.size() / sizeof(float); ++i) {
    float a, b;
    std::memcpy(&a, frame.color.payload.data() + sizeof(float) * i, sizeof(float));
    std::memcpy(&b, baseline.color.payload.data() + sizeof(float) * i, sizeof(float));
    Near(a, b, 3e-6, "moving visibility/disocclusion temporal vs NEE/MIS pixel");
  }
  Save(frame, integrator == Integrator::Reference ? "temporal-disocclusion-reference" : "temporal-disocclusion-wavefront");
  std::cout << "disocclusion/current visibility: rejected=" << rejected << " reused=" << reused
            << "; per-channel NEE/MIS delta <=3e-6\n";
}

void Arithmetic(OffscreenRenderer& renderer, Integrator integrator) {
  Fixture f(renderer, integrator, 64);
  PlaneWorld(f.world);
  const std::array<std::array<float, 3>, 3> colors{{{2, 1, 0.5F}, {1, 2, 0.5F}, {0.5F, 1, 2}}};
  // Equal estimated powers give a uniform three-column alias table. Third
  // light is below the receiver, so its target is zero but its draws count.
  for (unsigned i = 0; i < 3; ++i) {
    Light light; light.shape = LightShape::Distant; light.angle = 0; light.radiance = colors[i];
    if (i == 2) light.world_from_light[10] = -1;
    f.world.SetLight(std::to_string(i), light);
  }
  const auto frame = f.Render();
  Check(frame.restir.available && frame.restir.initial.size() == 4096 &&
      frame.restir.row_pitch == 64 * sizeof(RestirPixel), "reservoir product extent");
  Check(frame.restir.reservoir_bytes == 2 * 4096 * sizeof(RestirPixel), "reservoir memory accounting");
  if (frame.restir.timestamp_available)
    Check(frame.restir.initial_gpu_ms > 0, "reservoir timestamp");
  std::size_t all_zero = 0;
  for (unsigned slot = 0; slot < 4096; ++slot) {
    auto rng = Hash(slot ^ Hash(f.settings.sample_index ^ 0xD1B54A35U));
    vulkan_internal::Reservoir expected;
    std::array<float, 2> selected_uv{};
    for (unsigned draw = 0; draw < f.settings.restir_candidates; ++draw) {
      const auto light = std::min(unsigned(Random(rng) * 3.0F), 2U);
      (void)Random(rng); // alias threshold is one
      const std::array<float, 2> uv{Random(rng), Random(rng)};
      const float random = Random(rng);
      const float target = light == 2 ? 0.0F : (light == 0 ? 1.0F : 1.5F) / float(kPi);
      if (vulkan_internal::StreamReservoirCandidate(expected, light, target, 1.0F / 3.0F, random))
        selected_uv = uv;
    }
    const auto& actual = frame.restir.initial[slot];
    Check(actual.flags == 1 && actual.light == expected.sample &&
        actual.candidate_count == expected.candidate_count && actual.uv == selected_uv,
        "GPU selection/payload/count differs from CPU oracle");
    Near(actual.weight_sum, expected.weight_sum, 2e-6, "GPU weight sum");
    Near(actual.selected_target, expected.selected_target, 2e-7, "GPU target");
    if (expected.weight_sum == 0) { ++all_zero; continue; }
    const auto normalization = vulkan_internal::ReservoirNormalization(expected);
    for (int c = 0; c < 3; ++c) {
      const double contribution = Diffuse().base_color[c] * colors[expected.sample][c] / kPi;
      Near(actual.contribution[c], contribution * normalization, 2e-6, "selected RGB resolve");
    }
    Near(actual.contribution[3], 1, 0, "unshadowed visibility");
  }
  std::cout << "CPU/GPU oracle: 4096 reservoirs; all-zero streams=" << all_zero
            << " initial_ms=" << frame.restir.initial_gpu_ms << '\n';
  // Deterministic integrator equivalence including selected payloads.
  f.settings.integrator = integrator == Integrator::Reference ? Integrator::Wavefront : Integrator::Reference;
  const auto other = f.Render();
  Check(std::memcmp(frame.restir.initial.data(), other.restir.initial.data(),
      frame.restir.initial.size() * sizeof(RestirPixel)) == 0, "integrators changed reservoirs");
  Check(frame.color.payload == other.color.payload, "parallel-light integrator mismatch");
  f.settings.integrator = integrator;
  // All-zero material still represents all candidate draws.
  auto black = Diffuse(); black.base_color = {0, 0, 0}; f.world.SetMaterial("/diffuse", black);
  for (const auto& p : f.Render().restir.initial)
    Check(p.light == UINT32_MAX && p.weight_sum == 0 && p.candidate_count == 8 &&
        p.contribution == std::array<float, 4>{}, "all-zero target normalization");
}

void Visibility(OffscreenRenderer& renderer, Integrator integrator) {
  Fixture f(renderer, integrator, 16);
  PlaneWorld(f.world);
  Light light; light.shape = LightShape::Distant; light.angle = 0;
  const float x = std::sqrt(25.0F / 26.0F), z = std::sqrt(1.0F / 26.0F);
  light.world_from_light[8] = x; light.world_from_light[10] = z;
  light.radiance = {2, 1, 0.5F}; f.world.SetLight("/light", light);
  f.world.SetMesh("/blocker", Panel(5, 15, -6, 6, -2), {});
  Material blocker; blocker.ior = 1; blocker.base_color = {0, 0, 0};
  f.world.BindMaterial("/blocker", "/blocker");
  for (float opacity : {0.0F, 0.75F, 1.0F}) {
    blocker.opacity = opacity; f.world.SetMaterial("/blocker", blocker);
    const auto frame = f.Render();
    for (const auto& p : frame.restir.initial) {
      Check(p.flags == 1 && p.candidate_count == 8 && p.light == 0, "shadow affected candidate stream");
      Near(p.contribution[3], 1 - opacity, 1e-6, "selected transmittance");
    }
    const auto mean = Mean(frame);
    for (int c = 0; c < 3; ++c)
      Near(mean[c], Diffuse().base_color[c] * light.radiance[c] * z / kPi * (1 - opacity),
          2e-6, "shadowed independent irradiance");
  }
  std::cout << "visibility: transparent, fractional and opaque PASS\n";
}

// Double midpoint quadrature, independent of shader sampling and densities.
double PanelIntegral(bool disk) {
  double sum = 0;
  constexpr unsigned steps = 512;
  for (unsigned i = 0; i < steps; ++i) for (unsigned j = 0; j < steps; ++j) {
    double x, y, area;
    if (disk) {
      const double radius = 0.5 * std::sqrt((i + 0.5) / steps);
      const double phi = 2 * kPi * (j + 0.5) / steps;
      x = 1 + radius * std::cos(phi); y = radius * std::sin(phi); area = 0.25 * kPi;
    } else {
      x = 0.5 + (i + 0.5) / steps; y = -0.5 + (j + 0.5) / steps; area = 1;
    }
    const double distance2 = x * x + y * y + 4;
    sum += 4 / (distance2 * distance2) * area;
  }
  return sum / (steps * steps * kPi);
}
void Lighting(OffscreenRenderer& renderer, Integrator integrator) {
  SceneExtraction extraction;
  for (const char* name : {"rect", "disk", "sphere", "distant", "emissive", "environment", "constant"}) {
    Fixture f(renderer, integrator);
    f.extraction = std::move(extraction);
    PlaneWorld(f.world, 100);
    const std::array<float, 3> radiance{4, 2, 1};
    double factor = 0;
    const std::string kind = name;
    if (kind == "emissive") {
      f.world.SetMesh("/emitter", Panel(0.5F, 1.5F, -0.5F, 0.5F, -2), {});
      Material emitter; emitter.ior = 1; emitter.base_color = {0, 0, 0}; emitter.emission = radiance;
      f.world.SetMaterial("/emitter", emitter); f.world.BindMaterial("/emitter", "/emitter");
      factor = PanelIntegral(false);
    } else if (kind == "environment") {
      Texture texture; texture.width = 4; texture.height = 2; texture.format = TextureFormat::Rgba32Float;
      const float rgba[4]{4, 2, 1, 1};
      texture.texels.resize(8 * 16);
      for (unsigned i = 0; i < 8; ++i) std::memcpy(texture.texels.data() + i * 16, rgba, 16);
      f.world.SetTexture("/env", std::move(texture));
      EnvironmentMap map; map.texture = "/env"; f.world.SetEnvironmentMap(map); factor = 1;
    } else if (kind == "constant") {
      f.world.SetEnvironment(radiance); factor = 1;
    } else {
      Light light; light.radiance = radiance; light.world_from_light = Translation(1, 0, -2);
      if (kind == "rect") { light.shape = LightShape::Rect; factor = PanelIntegral(false); }
      if (kind == "disk") { light.shape = LightShape::Disk; factor = PanelIntegral(true); }
      if (kind == "sphere") { light.shape = LightShape::Sphere; factor = 0.25 / 5 * (2 / std::sqrt(5.0)); }
      if (kind == "distant") { light.shape = LightShape::Distant; light.angle = 60; factor = 0.25; }
      f.world.SetLight("/light", light);
    }
    const auto initial = f.Render(1024);
    const auto mean = Mean(initial);
    f.settings.light_sampling = LightSampling::Mis;
    const auto mis = f.Render(1024);
    const auto baseline = Mean(mis);
    const std::string prefix = integrator == Integrator::Reference ? "reference-" : "wavefront-";
    Save(initial, prefix + kind + "-initial");
    Save(mis, prefix + kind + "-mis");
    for (int c = 0; c < 3; ++c) {
      const double expected = Diffuse().base_color[c] * radiance[c] * factor;
      const double tolerance = expected * 0.02 + 0.0001;
      Near(mean[c], expected, tolerance, "initial-only independent lighting integral");
      Near(baseline[c], expected, tolerance, "NEE/MIS independent lighting integral");
      Near(mean[c], baseline[c], tolerance, "initial-only versus converged NEE/MIS");
    }
    std::cout << name << ": expected_R=" << Diffuse().base_color[0] * radiance[0] * factor
              << " initial_R=" << mean[0] << " mis_R=" << baseline[0]
              << " initial_ms=" << initial.restir.initial_gpu_ms << '\n';
    extraction = std::move(f.extraction);
  }
}

void Lifecycle(OffscreenRenderer& renderer, Integrator integrator) {
  Fixture f(renderer, integrator, 4); PlaneWorld(f.world);
  Light light; light.shape = LightShape::Distant; light.angle = 0; f.world.SetLight("/light", light);
  f.settings.max_samples = 2;
  const auto first = f.Render(2);
  Check(first.samples_per_pixel == 2 && first.restir.available, "fresh sample publication");
  const auto frozen = f.Render();
  Check(frozen.samples_per_pixel == 2 && !frozen.restir.available &&
        frozen.color.payload == first.color.payload, "sample cap stale publication");
  auto snapshot = f.world.Commit();
  for (unsigned count : {0U, 65U}) {
    auto invalid = f.settings; invalid.restir_candidates = count;
    Check(renderer.RenderScene(ExtractDrawSummary(snapshot), f.target, 1, invalid).status == FrameStatus::Fail,
        "invalid candidate count accepted");
  }
  f.settings.restir_candidates = 3;
  Check(f.Render().samples_per_pixel == 1, "candidate count did not reset accumulation");
  for (unsigned count : {1U, 64U}) {
    f.settings.restir_candidates = count;
    const auto changed = f.Render();
    Check(changed.samples_per_pixel == 1, "candidate endpoint did not reset accumulation");
    for (const auto& p : changed.restir.initial)
      Check(p.candidate_count == count, "candidate endpoint count");
  }
  f.settings.restir_candidates = 3;
  f.target.width = 3; f.target.height = 2; f.target.data_window = {1, 0, 1, 2};
  const auto crop = f.Render();
  for (unsigned i = 0; i < 6; ++i) {
    const auto& p = crop.restir.initial[i];
    if (i % 3 == 1) Check(p.flags == 1 && p.candidate_count == 3, "cropped surface");
    else Check(p.flags == 0 && p.light == UINT32_MAX && p.candidate_count == 0, "stale out-of-window reservoir");
  }
  light.radiance[0] = 2; f.world.SetLight("/light", light);
  const auto edited = f.Render();
  Check(edited.samples_per_pixel == 1 && edited.restir.scene_generation > crop.restir.scene_generation,
      "light edit retained accumulation/domain identity");
  f.settings.max_bounces = 0;
  const auto emission_only = f.Render();
  for (unsigned i = 0; i < emission_only.restir.initial.size(); ++i) {
    const auto& p = emission_only.restir.initial[i];
    Check(p.flags == (i % 3 == 1 ? 1U : 0U) && p.candidate_count == 0 && p.light == UINT32_MAX,
        "zero scattering limit generated candidates");
  }
  f.settings.max_bounces = 1;
  // Camera rays missing an existing TLAS must overwrite stale products.
  auto moved_camera = Perspective(); moved_camera.view = Translation(-100, 0, 0);
  f.world.SetCamera(moved_camera);
  for (const auto& p : f.Render().restir.initial)
    Check(p.flags == 0 && p.light == UINT32_MAX && p.candidate_count == 0, "camera miss stale reservoir");
  f.world.SetCamera(Perspective());
  f.world.RemoveMesh("/plane");
  const auto empty = f.Render();
  for (const auto& p : empty.restir.initial)
    Check(p.flags == 0 && p.light == UINT32_MAX && p.candidate_count == 0, "empty scene stale reservoir");

  // Reservoir state stays outside shared temporal validity/radiance buffers.
  Fixture t(renderer, integrator, 4); PlaneWorld(t.world); t.world.SetLight("/light", light);
  t.extraction = std::move(f.extraction);
  snapshot = t.world.Commit();
  Check(renderer.UpdateScene(t.extraction.Update(snapshot)).status == FrameStatus::Pass, "temporal upload");
  TemporalFrameSettings temporal; temporal.accumulate_radiance = true;
  const auto Render = [&]() {
    auto frame = renderer.RenderTemporalScene(snapshot, t.target, 1, temporal, t.settings);
    Check(frame.status == FrameStatus::Pass, frame.detail.c_str());
    Check(frame.validation_message_count == 0, frame.validation_detail.c_str());
    ++temporal.frame_index;
    return frame;
  };
  (void)Render();
  Check(Render().temporal.history_available, "stationary temporal radiance missing");
  t.settings.restir_candidates = 4;
  const auto changed = Render();
  Check(!changed.temporal.history_available &&
      changed.temporal.reset_reason == TemporalResetReason::RenderSettingsChanged,
      "candidate count retained incompatible temporal radiance");
  std::cout << "lifecycle: limits, resets, crop, resize, empty scene, temporal key PASS\n";
}
void Cornell(OffscreenRenderer& renderer, Integrator integrator) {
  Fixture f(renderer, integrator, 32);
  LotusHeadless::SetCornellBox(f.world); f.settings.max_bounces = 16;
  const auto initial_frame = f.Render(1024);
  const auto initial = Mean(initial_frame);
  f.settings.light_sampling = LightSampling::Mis;
  const auto baseline_frame = f.Render(1024);
  const auto baseline = Mean(baseline_frame);
  for (int c = 0; c < 3; ++c)
    Near(initial[c], baseline[c], 0.02 * baseline[c] + 0.001, "multibounce NEE/MIS comparison");
  // Compare image regions as well as the global mean.
  for (unsigned y = 0; y < 32; y += 8) for (unsigned x = 0; x < 32; x += 8) {
    std::array<double, 3> a{}, b{};
    for (unsigned dy = 0; dy < 8; ++dy) for (unsigned dx = 0; dx < 8; ++dx) {
      const auto offset = ((y + dy) * 32 + x + dx) * 16;
      float av[4], bv[4];
      std::memcpy(av, initial_frame.color.payload.data() + offset, 16);
      std::memcpy(bv, baseline_frame.color.payload.data() + offset, 16);
      for (int c = 0; c < 3; ++c) { a[c] += av[c] / 64; b[c] += bv[c] / 64; }
    }
    for (int c = 0; c < 3; ++c)
      Near(a[c], b[c], 0.05 * b[c] + 0.005, "Cornell image tile comparison");
  }
  const std::string prefix = integrator == Integrator::Reference ? "reference-" : "wavefront-";
  Save(initial_frame, prefix + "cornell-initial");
  Save(baseline_frame, prefix + "cornell-mis");
  std::cout << "Cornell mean RGB: initial=" << initial[0] << ',' << initial[1] << ',' << initial[2]
            << " mis=" << baseline[0] << ',' << baseline[1] << ',' << baseline[2] << '\n';
}
void Overflow(OffscreenRenderer& renderer, Integrator integrator) {
  Fixture f(renderer, integrator, 4); PlaneWorld(f.world);
  Light light; light.shape = LightShape::Distant; light.angle = 0;
  light.radiance.fill(std::numeric_limits<float>::max());
  f.world.SetLight("/light", light);
  auto snapshot = f.world.Commit();
  const auto upload = renderer.UpdateScene(f.extraction.Update(snapshot));
  Check(upload.status == FrameStatus::Pass, upload.detail.c_str());
  const auto failed = renderer.RenderScene(ExtractDrawSummary(snapshot), f.target, 2, f.settings);
  Check(failed.status == FrameStatus::Fail && !failed.restir.available &&
      failed.detail.find("reservoir arithmetic") != std::string::npos,
      "GPU reservoir overflow published a successful frame");
  Check(renderer.RenderScene(ExtractDrawSummary(snapshot), f.target, 1, f.settings).status == FrameStatus::Fail,
      "failed renderer resumed accumulation");
  std::cout << "overflow: batch failure rejected, no products published PASS\n";
}

void TemporalOverflow(OffscreenRenderer& renderer, Integrator integrator) {
  Fixture f(renderer, integrator, 4); PlaneWorld(f.world);
  auto material = Diffuse(); material.base_color = {1, 1, 1};
  f.world.SetMaterial("/diffuse", material);
  Light light; light.shape = LightShape::Distant; light.angle = 0;
  light.radiance.fill(1.6e38F); f.world.SetLight("/light", light);
  f.settings.restir_candidates = 1;
  f.settings.restir_max_history_candidates = 64;
  f.settings.restir_temporal = true;
  auto snapshot = f.world.Commit();
  Check(renderer.UpdateScene(f.extraction.Update(snapshot)).status == FrameStatus::Pass,
      "temporal overflow upload");
  bool failed = false;
  for (unsigned i = 0; i < 12; ++i) {
    TemporalFrameSettings temporal; temporal.frame_index = i;
    const auto frame = renderer.RenderTemporalScene(snapshot, f.target, 1, temporal, f.settings);
    if (frame.status == FrameStatus::Fail) {
      Check(i > 0 && !frame.restir.available && !frame.temporal.available &&
          frame.detail.find("temporal reservoir arithmetic") != std::string::npos,
          "GPU merge overflow published history or failed in initial-only pass");
      Check(renderer.RenderTemporalScene(snapshot, f.target, 1, temporal, f.settings).status == FrameStatus::Fail,
          "GPU merge failure did not latch");
      failed = true; break;
    }
    Check(frame.status == FrameStatus::Pass && frame.restir.available, frame.detail.c_str());
  }
  Check(failed, "temporal merge overflow fixture stayed finite");
  std::cout << "temporal overflow: valid initial draws, failed merge, no history publication PASS\n";
}
} // namespace

int main(int argc, char** argv) try {
  const std::string vertex = argc > 1 ? argv[1] : "";
  const std::string fragment = argc > 2 ? argv[2] : "";
  RayQueryShaders shaders;
  if (argc > 4) { shaders.vertex = argv[3]; shaders.fragment = argv[4]; }
  if (argc > 5) shaders.wavefront = argv[5];
  if (argc > 6) image_root = argv[6];
  FrameStatus status; std::string detail;
  auto renderer = CreateOffscreenRenderer(vertex, fragment, status, detail, shaders);
  if (!renderer) {
    std::cout << (status == FrameStatus::Skip ? "SKIP: " : "FAIL: ") << detail << '\n';
    return status == FrameStatus::Skip ? 77 : 1;
  }
  if (!renderer->RayQueryCapability().available) {
    std::cout << "SKIP: " << renderer->RayQueryCapability().detail << '\n'; return 77;
  }
  for (auto integrator : {Integrator::Reference, Integrator::Wavefront}) {
    // Fresh scene extraction and renderer ownership for each independent fixture.
    for (unsigned test = 0; test < 11; ++test) {
      renderer = CreateOffscreenRenderer(vertex, fragment, status, detail, shaders);
      Check(renderer != nullptr, detail.c_str());
      if (test == 0) Arithmetic(*renderer, integrator);
      if (test == 1) Visibility(*renderer, integrator);
      if (test == 2) Lighting(*renderer, integrator);
      if (test == 3) Lifecycle(*renderer, integrator);
      if (test == 4) Cornell(*renderer, integrator);
      if (test == 5) Overflow(*renderer, integrator);
      if (test == 6) TemporalArithmetic(*renderer, integrator);
      if (test == 7) TemporalLifecycle(*renderer, integrator);
      if (test == 8) TemporalSupport(*renderer, integrator);
      if (test == 9) TemporalOverflow(*renderer, integrator);
      if (test == 10) TemporalDisocclusion(*renderer, integrator);
    }
  }
  std::cout << "PASS: initial and temporal reservoirs under both integrators\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "FAIL: " << error.what() << '\n'; return 1;
}
