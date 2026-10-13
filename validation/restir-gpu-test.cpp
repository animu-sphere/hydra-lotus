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
};

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
    for (unsigned test = 0; test < 6; ++test) {
      renderer = CreateOffscreenRenderer(vertex, fragment, status, detail, shaders);
      Check(renderer != nullptr, detail.c_str());
      if (test == 0) Arithmetic(*renderer, integrator);
      if (test == 1) Visibility(*renderer, integrator);
      if (test == 2) Lighting(*renderer, integrator);
      if (test == 3) Lifecycle(*renderer, integrator);
      if (test == 4) Cornell(*renderer, integrator);
      if (test == 5) Overflow(*renderer, integrator);
    }
  }
  std::cout << "PASS: initial-only reservoirs under both integrators\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "FAIL: " << error.what() << '\n'; return 1;
}
