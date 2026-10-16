// SPDX-License-Identifier: Apache-2.0
#include "restir_benchmark.hpp"
#include "reference.hpp"
#include "temporal_statistics.hpp"

#include <lotus/vulkan_backend.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace LotusHeadless {
namespace {
using namespace Lotus;
using Clock = std::chrono::steady_clock;
constexpr double kPi = 3.14159265358979323846;
constexpr unsigned kSeed = 17, kSequenceStride = 65537, kReferenceSeed = 1U << 24;

struct Options {
  std::filesystem::path report;
  std::string label;
  unsigned size = 64, frames = 32, warmup = 4, sequences = 4;
  unsigned lights = 128, reference_samples = 256, cap = 32, radius = 1;
  unsigned seed = kSeed, reference_seed = kReferenceSeed, bounces = 1;
  std::vector<unsigned> candidates{1, 2, 8};
  Integrator integrator = Integrator::Reference;
  RestirProposal proposal = RestirProposal::Power;
  std::string workload = "distant";
};

const char* ProposalName(RestirProposal proposal) {
  switch (proposal) {
  case RestirProposal::Power: return "power-alias";
  case RestirProposal::Uniform: return "uniform";
  case RestirProposal::PowerUniform: return "power-uniform";
  }
  throw std::invalid_argument("unknown reservoir proposal");
}

void Require(bool condition, const std::string& why) {
  if (!condition)
    throw std::runtime_error(why);
}

std::string Quote(std::string_view value) {
  std::ostringstream out;
  out << '"';
  for (unsigned char c : value) {
    if (c == '"' || c == '\\')
      out << '\\' << c;
    else if (c < 32)
      out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
          << static_cast<unsigned>(c) << std::dec;
    else
      out << c;
  }
  out << '"';
  return out.str();
}

void Array(std::ostream& out, const auto& values) {
  out << '[';
  bool first = true;
  for (auto value : values) {
    out << (first ? "" : ",") << value;
    first = false;
  }
  out << ']';
}

void Series(std::ostream& out, const std::vector<double>& values) {
  if (values.empty()) {
    out << "null";
    return;
  }
  auto sorted = values;
  std::sort(sorted.begin(), sorted.end());
  out << "{\"mean\":" << std::accumulate(values.begin(), values.end(), 0.0) / values.size()
      << ",\"median\":" << sorted[sorted.size() / 2]
      << ",\"p95\":" << sorted[static_cast<std::size_t>(std::ceil(sorted.size() * 0.95)) - 1]
      << ",\"raw\":";
  Array(out, values);
  out << '}';
}

unsigned Number(std::string_view value, unsigned low, unsigned high) {
  unsigned result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
      result < low || result > high)
    throw std::invalid_argument("invalid count: " + std::string(value));
  return result;
}

Options Parse(int argc, char** argv) {
  if (argc < 3 || std::string_view(argv[2]).starts_with("--"))
    throw std::invalid_argument("usage: lotus-headless --benchmark-restir <json-path> "
                                "[--benchmark-label <identity>] [--benchmark-integrator <reference|wavefront>] "
                                "[--benchmark-size <16..256>] [--benchmark-frames <2..256>] "
                                "[--benchmark-warmup <0..64>] [--restir-sequences <2..64>] "
                                "[--restir-seed <0..8388607>] "
                                "[--restir-reference-seed <16777216..1073741823>] [--restir-bounces <1..64>] "
                                "[--restir-workload <distant|mixed-boundaries|glossy-disocclusion|glossy-spheres>] "
                                "[--restir-lights <2..4096>] [--restir-reference-samples <16..4096>] "
                                "[--restir-candidates <comma-separated 1..64>] "
                                "[--restir-proposal <power-alias|uniform|power-uniform>] "
                                "[--restir-history-cap <1..65536>] [--restir-radius <1..32>]");
  Options options;
  options.report = argv[2];
  for (int i = 3; i < argc; ++i) {
    const std::string_view option = argv[i];
    if (i + 1 == argc)
      throw std::invalid_argument("missing value for " + std::string(option));
    const std::string_view value = argv[++i];
    if (option == "--benchmark-label")
      options.label = value;
    else if (option == "--benchmark-integrator") {
      if (value == "reference")
        options.integrator = Integrator::Reference;
      else if (value == "wavefront")
        options.integrator = Integrator::Wavefront;
      else
        throw std::invalid_argument("invalid integrator: " + std::string(value));
    } else if (option == "--restir-proposal") {
      if (value == "power-alias")
        options.proposal = RestirProposal::Power;
      else if (value == "uniform")
        options.proposal = RestirProposal::Uniform;
      else if (value == "power-uniform")
        options.proposal = RestirProposal::PowerUniform;
      else
        throw std::invalid_argument("invalid reservoir proposal: " + std::string(value));
    } else if (option == "--restir-workload") {
      if (value != "distant" && value != "mixed-boundaries" &&
          value != "glossy-disocclusion" && value != "glossy-spheres")
        throw std::invalid_argument("invalid reservoir workload: " + std::string(value));
      options.workload = value;
    } else if (option == "--benchmark-size")
      options.size = Number(value, 16, 256);
    else if (option == "--benchmark-frames")
      options.frames = Number(value, 2, 256);
    else if (option == "--benchmark-warmup")
      options.warmup = Number(value, 0, 64);
    else if (option == "--restir-sequences")
      options.sequences = Number(value, 2, 64);
    else if (option == "--restir-seed")
      options.seed = Number(value, 0, (1U << 23) - 1);
    else if (option == "--restir-reference-seed")
      options.reference_seed = Number(value, kReferenceSeed, (1U << 30) - 1);
    else if (option == "--restir-bounces")
      options.bounces = Number(value, 1, 64);
    else if (option == "--restir-lights")
      options.lights = Number(value, 2, 4096);
    else if (option == "--restir-reference-samples")
      options.reference_samples = Number(value, 16, 4096);
    else if (option == "--restir-history-cap")
      options.cap = Number(value, 1, 65536);
    else if (option == "--restir-radius")
      options.radius = Number(value, 1, 32);
    else if (option == "--restir-candidates") {
      options.candidates.clear();
      auto remaining = value;
      for (;;) {
        const auto comma = remaining.find(',');
        const auto candidate = Number(remaining.substr(0, comma), 1, 64);
        if (std::find(options.candidates.begin(), options.candidates.end(), candidate) != options.candidates.end())
          throw std::invalid_argument("duplicate candidate count");
        options.candidates.push_back(candidate);
        if (comma == std::string_view::npos)
          break;
        remaining.remove_prefix(comma + 1);
      }
    } else
      throw std::invalid_argument("unknown benchmark option: " + std::string(option));
  }
  // Measured samples must stay below the separate warmup/reference ranges.
  if (options.seed + (options.sequences - 1) * kSequenceStride + options.frames > (1U << 23))
    throw std::invalid_argument("measured sample range overlaps warmup range");
  return options;
}

Matrix4 Translation(float x, float z = 0) {
  auto matrix = IdentityMatrix();
  matrix[12] = x;
  matrix[14] = z;
  return matrix;
}

struct Workload {
  std::string id;
  bool moving = false;
  bool exact_integral = true;
  std::vector<FrameSnapshot> snapshots;
  std::array<double, 3> integral{};
};

Workload MakeMixedWorkload(const Options& options, bool moving) {
  const bool narrow = options.workload == "glossy-disocclusion";
  const bool spheres = options.workload == "glossy-spheres";
  const bool glossy = narrow || spheres;
  Workload result;
  result.moving = moving;
  result.exact_integral = false;
  result.id = moving ? "mixed-area-environment-moving-boundaries-v1"
                     : "mixed-area-environment-boundaries-v1";
  if (glossy)
    result.id = moving ? "glossy-area-environment-camera-disocclusion-v1"
                       : "glossy-area-environment-static-v1";
  if (spheres)
    result.id = moving ? "glossy-sphere-environment-camera-disocclusion-v1"
                       : "glossy-sphere-environment-static-v1";
  RenderWorld world;
  Material diffuse;
  diffuse.ior = spheres ? 1.5F : 1;
  if (spheres)
    diffuse.roughness = 0.18F;
  diffuse.base_color = {0.8F, 0.12F, 0.04F};
  world.SetMaterial("/diffuse", diffuse);
  Material metal;
  metal.base_color = {0.06F, 0.3F, 0.85F};
  metal.metallic = 1;
  metal.roughness = narrow ? 0.08F : (spheres ? 0.5F : 0.35F);
  world.SetMaterial("/metal", metal);
  // Adjacent materials meet at X=0 with a depth and geometric-normal step.
  world.SetMesh("/left", {{{-12, -12, -4}, {0, -12, -4}, {0, 12, -4}, {-12, 12, -4}},
                            {{0, 1, 2}, {0, 2, 3}}, {0, 0}}, {});
  world.BindMaterial("/left", "/diffuse");
  const float right_far_z = spheres ? -13.2F : -6.6F;
  world.SetMesh("/right", {{{0, -12, -3.6F}, {12, -12, right_far_z}, {12, 12, right_far_z}, {0, 12, -3.6F}},
                             {{0, 1, 2}, {0, 2, 3}}, {0, 0}}, {});
  world.BindMaterial("/right", "/metal");
  if (moving || glossy) {
    world.SetMesh("/blocker", {{{-0.45F, -1.4F, -2.5F}, {0.45F, -1.4F, -2.5F},
                                 {0.45F, 1.4F, -2.5F}, {-0.45F, 1.4F, -2.5F}},
                                {{0, 1, 2}, {0, 2, 3}}, {0, 0}}, {});
    world.BindMaterial("/blocker", "/diffuse");
  }
  for (unsigned i = 0; i < options.lights; ++i) {
    const double phi = i * 2.399963229728653;
    const double radius = 3 * std::sqrt((i + 0.5) / options.lights);
    Light light;
    light.shape = i % 2 ? (spheres ? LightShape::Sphere : LightShape::Disk) : LightShape::Rect;
    light.radius = narrow ? 0.06F : (spheres ? 0.35F : 0.3F);
    light.width = light.height = narrow ? 0.12F : 0.6F;
    light.world_from_light = Translation(static_cast<float>(radius * std::cos(phi)), -1);
    light.world_from_light[13] = static_cast<float>(radius * std::sin(phi));
    // Preserve emitted power as the glossy fixture narrows the area lights.
    const float scale = (narrow ? 2400.0F : 96.0F) / options.lights;
    light.radiance = {scale * (0.2F + (i % 7) * 0.2F),
        scale * (0.1F + (i % 5) * 0.3F), scale * (0.3F + (i % 3) * 0.4F)};
    world.SetLight("/light/" + std::to_string(i), light);
  }
  // Procedural HDR lat-long image: broad colour variation plus a bright patch.
  // No external asset, filtering or licence dependency enters this fixture.
  Texture environment;
  environment.width = 16;
  environment.height = 8;
  environment.format = TextureFormat::Rgba32Float;
  environment.texels.resize(16 * 8 * 16);
  for (unsigned y = 0; y < 8; ++y)
    for (unsigned x = 0; x < 16; ++x) {
      const float bright = (x == 7 || x == 8) && (y == 2 || y == 3) ? 3.0F : 0.0F;
      const float rgba[4]{0.04F + bright, 0.08F + 0.01F * y + bright * 0.5F,
          0.16F + 0.005F * x + bright * 0.2F, 1};
      std::memcpy(environment.texels.data() + (y * 16 + x) * 16, rgba, 16);
    }
  world.SetTexture("/environment", std::move(environment));
  EnvironmentMap map;
  map.texture = "/environment";
  world.SetEnvironmentMap(map);
  for (unsigned f = 0; f < options.frames; ++f) {
    Camera camera;
    camera.projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1.2F, -1, 0, 0, -2.2F, 0};
    if (moving) {
      const float phase = static_cast<float>(2 * kPi * f / options.frames);
      if (glossy) {
        // Camera-only motion: the foreground stays fixed, exposing parallax
        // and newly visible glossy receivers without changing light domains.
        const float yaw = 0.16F * std::sin(phase);
        auto rotation = IdentityMatrix();
        rotation[0] = rotation[10] = std::cos(yaw);
        rotation[8] = std::sin(yaw);
        rotation[2] = -std::sin(yaw);
        camera.view = Multiply(rotation, Translation(1.25F * std::sin(phase)));
      } else {
        camera.view[12] = 0.12F * std::sin(phase);
        MeshInstance instance;
        instance.world_from_object = Translation(0.7F * std::sin(phase + 0.3F));
        world.SetMeshInstance("/blocker", instance);
      }
    }
    world.SetCamera(camera);
    result.snapshots.push_back(world.Commit());
  }
  return result;
}

Workload MakeWorkload(const Options& options, bool moving) {
  if (options.workload != "distant")
    return MakeMixedWorkload(options, moving);
  Workload result;
  result.moving = moving;
  result.id = moving ? "many-distant-moving-shadow-v1" : "many-distant-plane-v1";
  RenderWorld world;
  Material diffuse;
  diffuse.ior = 1;
  diffuse.base_color = {0.5F, 0.75F, 1};
  world.SetMaterial("/diffuse", diffuse);
  world.SetMesh("/plane", {{{-12, -12, -4}, {12, -12, -4}, {12, 12, -4}, {-12, 12, -4}}, {{0, 1, 2}, {0, 2, 3}}, {0, 0}}, {});
  world.BindMaterial("/plane", "/diffuse");
  if (moving) {
    world.SetMesh("/blocker", {{{-0.5F, -1.4F, -2.5F}, {0.5F, -1.4F, -2.5F}, {0.5F, 1.4F, -2.5F}, {-0.5F, 1.4F, -2.5F}}, {{0, 1, 2}, {0, 2, 3}}, {0, 0}}, {});
    world.BindMaterial("/blocker", "/diffuse");
  }
  for (unsigned i = 0; i < options.lights; ++i) {
    const double z = 0.15 + 0.8 * (i + 0.5) / options.lights;
    const double phi = i * 2.399963229728653; // Golden-angle azimuth.
    const double r = std::sqrt(1 - z * z);
    Light light;
    light.shape = LightShape::Distant;
    light.angle = 0;
    light.world_from_light[8] = static_cast<float>(r * std::cos(phi));
    light.world_from_light[9] = static_cast<float>(r * std::sin(phi));
    light.world_from_light[10] = static_cast<float>(z);
    const float scale = 32.0F / options.lights;
    light.radiance = {scale * (0.2F + (i % 7) * 0.2F),
        scale * (0.1F + (i % 5) * 0.3F), scale * (0.3F + (i % 3) * 0.4F)};
    world.SetLight("/light/" + std::to_string(i), light);
    const double length = std::sqrt(double(light.world_from_light[8]) * light.world_from_light[8] +
                                    double(light.world_from_light[9]) * light.world_from_light[9] +
                                    double(light.world_from_light[10]) * light.world_from_light[10]);
    for (unsigned c = 0; c < 3; ++c)
      result.integral[c] += diffuse.base_color[c] * light.radiance[c] * light.world_from_light[10] / (length * kPi);
  }
  for (unsigned f = 0; f < options.frames; ++f) {
    Camera camera;
    camera.projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1.2F, -1, 0, 0, -2.2F, 0};
    if (moving) {
      const float phase = static_cast<float>(2 * kPi * f / options.frames);
      camera.view[12] = 0.12F * std::sin(phase);
      MeshInstance instance;
      instance.world_from_object = Translation(0.7F * std::sin(phase + 0.3F));
      world.SetMeshInstance("/blocker", instance);
    }
    world.SetCamera(camera);
    result.snapshots.push_back(world.Commit());
  }
  return result;
}

std::unique_ptr<OffscreenRenderer> Renderer(const std::filesystem::path& executable,
    FrameStatus& status, std::string& detail) {
  const auto shaders = executable / "shaders";
  auto renderer = CreateOffscreenRenderer((shaders / "triangle.vert.spv").string(),
      (shaders / "triangle.frag.spv").string(), status, detail,
      {(shaders / "path_trace.vert.spv").string(), (shaders / "path_trace.frag.spv").string(),
          (shaders / "wavefront.comp.spv").string()});
  if (renderer) {
    const auto capability = renderer->RayQueryCapability();
    if (!capability.available) {
      status = FrameStatus::Skip;
      detail = capability.detail;
      renderer.reset();
    }
  }
  return renderer;
}

void Upload(OffscreenRenderer& renderer, SceneExtraction& extraction, const FrameSnapshot& snapshot) {
  const auto update = renderer.UpdateScene(extraction.Update(snapshot));
  Require(update.status == FrameStatus::Pass, update.detail);
  Require(update.validation_message_count == 0, "scene upload emitted validation messages");
}

RgbImage Image(const GpuFrameEvidence& frame, unsigned size, unsigned samples) {
  Require(frame.status == FrameStatus::Pass, frame.detail);
  Require(frame.validation_message_count == 0, frame.validation_detail);
  Require(frame.ray_query_used && frame.samples_per_pixel == samples, "wrong fresh sample count");
  Require(frame.color.width == size && frame.color.height == size && frame.color.row_pitch == size * 16 &&
              frame.color.pixel_format == "rgba32-sfloat" && frame.color.origin == "top-left" &&
              frame.color.payload.size() == std::size_t{size} * size * 16,
      "wrong colour product layout");
  RgbImage image{size, size, std::vector<float>(std::size_t{size} * size * 3)};
  for (std::size_t i = 0; i < image.values.size() / 3; ++i)
    std::memcpy(image.values.data() + i * 3, frame.color.payload.data() + i * 16, 12);
  for (auto value : image.values)
    Require(std::isfinite(value), "nonfinite radiance");
  return image;
}

double Mse(const auto& a, const auto& b) {
  Require(a.size() == b.size() && !a.empty(), "image extent mismatch");
  double sum = 0;
  for (std::size_t i = 0; i < a.size(); ++i)
    sum += std::pow(double(a[i]) - b[i], 2);
  return sum / a.size();
}

struct Baseline {
  std::vector<RgbImage> images;
  std::vector<double> split_rmse;
  std::string device;
  double integral_rmse = 0;
};

Baseline MakeReference(OffscreenRenderer& renderer, const Workload& work, const Options& options,
    const std::filesystem::path& images) {
  Baseline result;
  SceneExtraction extraction;
  OffscreenTarget target;
  target.width = target.height = options.size;
  // Use the reference integrator even when measuring wavefront, so it remains
  // an independent transport check. Each half has a disjoint sample range.
  PathTracingSettings settings;
  settings.max_bounces = options.bounces;
  for (unsigned f = 0; f < (work.moving ? options.frames : 1); ++f) {
    Upload(renderer, extraction, work.snapshots[f]);
    std::array<RgbImage, 2> halves;
    for (unsigned half = 0; half < 2; ++half) {
      settings.sample_index = options.reference_seed + f * 8192 + half * 4096;
      const auto frame = renderer.RenderScene(ExtractDrawSummary(work.snapshots[f]), target,
          options.reference_samples, settings);
      halves[half] = Image(frame, options.size, options.reference_samples);
      result.device = "{\"name\":" + Quote(frame.device_name) + ",\"api\":" + Quote(frame.api_version) +
                      ",\"driver\":" + Quote(frame.driver_version) + ",\"validation_available\":" +
                      (frame.validation_available ? "true" : "false") + ",\"synchronization_validation_available\":" +
                      (frame.synchronization_validation_available ? "true" : "false") + '}';
    }
    result.split_rmse.push_back(std::sqrt(Mse(halves[0].values, halves[1].values)));
    auto mean = halves[0];
    for (std::size_t i = 0; i < mean.values.size(); ++i)
      mean.values[i] = (mean.values[i] + halves[1].values[i]) * 0.5F;
    Require(WritePfm(images / (work.id + "-mis-reference-" + std::to_string(f) + ".pfm"), mean).empty(),
        "cannot save reference image");
    result.images.push_back(std::move(mean));
  }
  if (work.exact_integral && !work.moving) {
    std::vector<double> exact(result.images[0].values.size());
    for (std::size_t i = 0; i < exact.size(); ++i)
      exact[i] = work.integral[i % 3];
    result.integral_rmse = std::sqrt(Mse(result.images[0].values, exact));
  }
  if (!work.moving)
    result.images.resize(options.frames, result.images.front());
  return result;
}

struct Mode {
  std::string name;
  unsigned candidates = 0;
  bool temporal = false, spatial = false;
};

std::string Measure(const Workload& work, const Baseline& baseline, const Mode& mode,
    const Options& options, const std::filesystem::path& executable, const std::filesystem::path& images) {
  FrameStatus setup;
  std::string detail;
  auto renderer = Renderer(executable, setup, detail);
  Require(renderer != nullptr, detail);
  SceneExtraction extraction;
  OffscreenTarget target;
  target.width = target.height = options.size;
  PathTracingSettings settings;
  settings.integrator = options.integrator;
  settings.max_bounces = options.bounces;
  settings.light_sampling = mode.candidates ? LightSampling::RestirInitial : LightSampling::Mis;
  settings.restir_candidates = mode.candidates ? mode.candidates : 8;
  settings.restir_temporal = mode.temporal;
  settings.restir_spatial = mode.spatial;
  settings.restir_max_history_candidates = options.cap;
  settings.restir_spatial_radius = options.radius;
  settings.restir_proposal = options.proposal;
  const bool reuse = mode.temporal || mode.spatial;
  std::uint64_t frame_index = 0;
  auto render = [&](const FrameSnapshot& snapshot, unsigned seed, unsigned f, bool cut) {
    Upload(*renderer, extraction, snapshot);
    settings.sample_index = seed + (reuse ? 0 : f);
    if (reuse) {
      TemporalFrameSettings temporal;
      temporal.frame_index = frame_index++;
      temporal.camera_cut = cut;
      return renderer->RenderTemporalScene(snapshot, target, 1, temporal, settings);
    }
    return renderer->RenderScene(ExtractDrawSummary(snapshot), target, 1, settings);
  };
  for (unsigned f = 0; f < options.warmup; ++f)
    (void)Image(render(work.snapshots[0], 1U << 23, f, f == 0), options.size, 1);

  std::vector<double> error, delta, wall, gpu, initial, temporal, spatial, surface, reprojection;
  std::vector<double> residual_mean, residual_centered;
  std::vector<std::optional<double>> residual_lag1;
  std::vector<double> convergence_mse(options.frames), convergence_gpu(options.frames);
  std::vector<std::vector<double>> convergence_rmse_raw(options.frames), convergence_gpu_raw(options.frames);
  std::vector<double> ensemble(std::size_t{options.size} * options.size * 3), previous;
  std::uint64_t reservoir_bytes = 0, metadata_bytes = 0, scene_pool_bytes = 0;
  std::uint64_t reused_pixels = 0, surface_pixels = 0;
  RgbImage last;
  bool timestamps = false;
  for (unsigned sequence = 0; sequence < options.sequences; ++sequence) {
    std::vector<double> accumulated(ensemble.size());
    TemporalResidualStatistics residual_statistics(ensemble.size());
    double elapsed_gpu = 0;
    for (unsigned f = 0; f < options.frames; ++f) {
      const auto start = Clock::now();
      auto frame = render(work.snapshots[f], options.seed + sequence * kSequenceStride, f, f == 0);
      wall.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
      last = Image(frame, options.size, 1);
      Require(frame.frames_rendered == 1, "measurement did not submit one sample");
      Require(frame.restir.available == (mode.candidates != 0), "wrong reservoir availability");
      if (reuse) {
        Require(frame.temporal.history_available == (f != 0) && !frame.temporal.radiance_available,
            "unexpected reset or radiance accumulation during sequence");
        Require(frame.restir.history_available == (mode.temporal && f != 0), "unexpected reservoir history reset");
        for (const auto& record : frame.restir.temporal) {
          surface_pixels += (record.reservoir.flags & 1) != 0;
          reused_pixels += record.previous_index != UINT32_MAX;
        }
      }
      // Only the unoccluded distant Lambert fixture has an exact integral.
      // All other workloads use the matched finite-spp NEE/MIS image.
      std::vector<double> reference(ensemble.size());
      for (std::size_t i = 0; i < reference.size(); ++i)
        reference[i] = work.exact_integral && !work.moving ? work.integral[i % 3] : baseline.images[f].values[i];
      error.push_back(std::sqrt(Mse(last.values, reference)));
      std::vector<double> residual(ensemble.size());
      for (std::size_t i = 0; i < residual.size(); ++i)
        residual[i] = double(last.values[i]) - reference[i];
      residual_statistics.Add(residual);
      if (f != 0) {
        std::vector<double> residual_delta(ensemble.size()), zero(ensemble.size());
        for (std::size_t i = 0; i < ensemble.size(); ++i)
          residual_delta[i] = double(last.values[i]) - previous[i] -
                              (work.moving ? double(baseline.images[f].values[i]) - baseline.images[f - 1].values[i] : 0);
        delta.push_back(std::sqrt(Mse(residual_delta, zero)));
      }
      previous.assign(last.values.begin(), last.values.end());
      for (std::size_t i = 0; i < ensemble.size(); ++i) {
        if (!work.moving)
          accumulated[i] += last.values[i];
        if (work.moving ? f + 1 == options.frames : true)
          ensemble[i] += last.values[i] / double(options.sequences * (work.moving ? 1 : options.frames));
      }
      if (!work.moving) {
        auto progressive = accumulated;
        for (auto& value : progressive)
          value /= f + 1;
        const auto mse = Mse(progressive, reference);
        convergence_mse[f] += mse / options.sequences;
        convergence_rmse_raw[f].push_back(std::sqrt(mse));
      }
      if (sequence == 0 && f == 0)
        timestamps = frame.primary_ray_timestamp_available;
      Require(frame.primary_ray_timestamp_available == timestamps, "inconsistent timestamp availability");
      if (timestamps) {
        Require(std::isfinite(frame.primary_ray_gpu_ms) && frame.primary_ray_gpu_ms > 0, "invalid scene timestamp");
        gpu.push_back(frame.primary_ray_gpu_ms);
        elapsed_gpu += frame.primary_ray_gpu_ms;
        convergence_gpu[f] += elapsed_gpu / options.sequences;
        convergence_gpu_raw[f].push_back(elapsed_gpu);
        if (mode.candidates) {
          Require(frame.restir.timestamp_available, "missing reservoir timestamps");
          initial.push_back(frame.restir.initial_gpu_ms);
          temporal.push_back(frame.restir.temporal_gpu_ms);
          spatial.push_back(frame.restir.spatial_gpu_ms);
        }
        if (reuse) {
          Require(frame.temporal.timestamp_available, "missing metadata timestamps");
          surface.push_back(frame.temporal.primary_surface_gpu_ms);
          reprojection.push_back(frame.temporal.reprojection_gpu_ms);
        }
      }
      reservoir_bytes = std::max(reservoir_bytes, frame.restir.reservoir_bytes);
      metadata_bytes = std::max(metadata_bytes, frame.temporal.surface_bytes +
                                                    frame.temporal.validity_bytes + frame.temporal.metadata_bytes);
    }
    residual_mean.push_back(residual_statistics.MeanRmse());
    residual_centered.push_back(residual_statistics.CenteredRmse());
    residual_lag1.push_back(residual_statistics.Lag1Autocorrelation());
  }
  const auto stats = renderer->UpdateScene({});
  Require(stats.status == FrameStatus::Pass && stats.validation_message_count == 0, "final scene/validation check failed");
  Require(stats.stats.analytic_lights == options.lights, "wrong analytic light count");
  scene_pool_bytes = stats.stats.memory.reserved_bytes;
  std::vector<double> reference(ensemble.size());
  std::array<double, 3> mean{}, bias{};
  for (std::size_t i = 0; i < ensemble.size(); ++i) {
    reference[i] = work.exact_integral && !work.moving ? work.integral[i % 3] : baseline.images.back().values[i];
    mean[i % 3] += ensemble[i] / (ensemble.size() / 3);
    bias[i % 3] += (ensemble[i] - reference[i]) / (ensemble.size() / 3);
  }
  double max_tile_bias = 0;
  for (unsigned y = 0; y < options.size; y += 8)
    for (unsigned x = 0; x < options.size; x += 8) {
      std::array<double, 3> tile{};
      unsigned pixels = 0;
      for (unsigned dy = y; dy < std::min(y + 8, options.size); ++dy)
        for (unsigned dx = x; dx < std::min(x + 8, options.size); ++dx) {
          ++pixels;
          for (unsigned c = 0; c < 3; ++c) {
            const auto i = (std::size_t{dy} * options.size + dx) * 3 + c;
            tile[c] += ensemble[i] - reference[i];
          }
        }
      for (auto value : tile)
        max_tile_bias = std::max(max_tile_bias, std::abs(value / pixels));
    }
  const auto prefix = work.id + '-' + mode.name + "-c" + std::to_string(mode.candidates);
  Require(WritePfm(images / (prefix + "-last.pfm"), last).empty(), "cannot save measured image");
  auto average = last;
  for (std::size_t i = 0; i < ensemble.size(); ++i)
    average.values[i] = static_cast<float>(ensemble[i]);
  Require(WritePfm(images / (prefix + "-ensemble.pfm"), average).empty(), "cannot save ensemble image");
  std::ostringstream out;
  out << std::setprecision(12);
  out << "{\"mode\":" << Quote(mode.name) << ",\"candidates\":" << mode.candidates
      << ",\"mean_rgb\":";
  Array(out, mean);
  out << ",\"signed_mean_error_rgb\":";
  Array(out, bias);
  out << ",\"ensemble_rmse\":" << std::sqrt(Mse(ensemble, reference))
      << ",\"max_abs_tile_mean_error\":" << max_tile_bias
      << ",\"regions\":[";
  if (!work.exact_integral) {
    const bool glossy = options.workload == "glossy-disocclusion" || options.workload == "glossy-spheres";
    const unsigned regions = options.workload == "glossy-spheres" ? 3U : (glossy ? 2U : 1U);
    for (unsigned region = 0; region < regions; ++region) {
      double squared = 0;
      std::array<double, 3> signed_error{};
      unsigned pixels = 0;
      // Fixed screen bands: center boundaries, right metal, left dielectric.
      const unsigned band = region == 2 ? 1U : (region ? 5U : 3U);
      const unsigned begin = band * options.size / 8;
      const unsigned end = (band + 2) * options.size / 8;
      for (unsigned y = 0; y < options.size; ++y)
        for (unsigned x = begin; x < end; ++x) {
          ++pixels;
          for (unsigned c = 0; c < 3; ++c) {
            const auto i = (std::size_t{y} * options.size + x) * 3 + c;
            const double residual = ensemble[i] - reference[i];
            squared += residual * residual;
            signed_error[c] += residual;
          }
        }
      for (auto& value : signed_error)
        value /= pixels;
      out << (region ? "," : "") << "{\"id\":"
          << Quote(region == 2 ? "left-dielectric-band-v1" :
              (region ? "right-glossy-band-v1" : "center-boundary-band-v1")) << ",\"pixels\":" << pixels
          << ",\"ensemble_rmse\":" << std::sqrt(squared / (3 * pixels))
          << ",\"signed_mean_error_rgb\":";
      Array(out, signed_error);
      out << '}';
    }
  }
  out << "],\"fresh_rmse\":";
  Series(out, error);
  out << ",\"residual_frame_delta_rmse\":";
  Series(out, delta);
  out << ",\"temporal_residuals\":{\"sequence_mean_rmse\":";
  Series(out, residual_mean);
  out << ",\"centered_rmse\":";
  Series(out, residual_centered);
  out << ",\"lag1_autocorrelation_raw\":[";
  for (std::size_t i = 0; i < residual_lag1.size(); ++i) {
    out << (i ? "," : "");
    if (residual_lag1[i])
      out << *residual_lag1[i];
    else
      out << "null";
  }
  out << "]}";
  out << ",\"render_update_wall_ms\":";
  Series(out, wall);
  out << ",\"scene_gpu_ms\":";
  Series(out, gpu);
  out << ",\"initial_gpu_ms\":";
  Series(out, initial);
  out << ",\"temporal_gpu_ms\":";
  Series(out, temporal);
  out << ",\"spatial_gpu_ms\":";
  Series(out, spatial);
  out << ",\"primary_surface_gpu_ms\":";
  Series(out, surface);
  out << ",\"reprojection_gpu_ms\":";
  Series(out, reprojection);
  out << ",\"memory\":{\"reservoir_device_and_host_bytes\":" << reservoir_bytes
      << ",\"metadata_device_and_host_bytes\":" << metadata_bytes
      << ",\"scene_pool_reserved_bytes\":" << scene_pool_bytes << ",\"total_vram_bytes\":null}"
      << ",\"history_receiver_pixels\":" << reused_pixels << ",\"surface_pixels\":" << surface_pixels
      << ",\"last_image\":" << Quote(prefix + "-last.pfm")
      << ",\"ensemble_image\":" << Quote(prefix + "-ensemble.pfm") << ",\"convergence\":";
  if (work.moving)
    out << "null";
  else {
    out << '[';
    bool first = true;
    for (unsigned f = 0; f < options.frames; ++f)
      if (((f + 1) & f) == 0 || f + 1 == options.frames) {
        out << (first ? "" : ",") << "{\"samples\":" << f + 1
            << ",\"rmse\":" << std::sqrt(convergence_mse[f]) << ",\"cumulative_gpu_ms\":";
        if (timestamps)
          out << convergence_gpu[f];
        else
          out << "null";
        out << ",\"rmse_raw\":";
        Array(out, convergence_rmse_raw[f]);
        out << ",\"cumulative_gpu_ms_raw\":";
        if (timestamps)
          Array(out, convergence_gpu_raw[f]);
        else
          out << "null";
        out << '}';
        first = false;
      }
    out << ']';
  }
  out << '}';
  std::cout << work.id << ' ' << mode.name << " candidates=" << mode.candidates
            << " ensemble_rmse=" << std::sqrt(Mse(ensemble, reference)) << '\n';
  return out.str();
}
} // namespace

int RestirBenchmarkMain(int argc, char** argv) {
  Options options;
  try {
    options = Parse(argc, argv);
  } catch (const std::invalid_argument& error) {
    std::cerr << error.what() << '\n';
    return 2;
  }
  try {
    const auto started = std::time(nullptr);
    const auto executable = std::filesystem::absolute(argv[0]).parent_path();
    auto image_root = options.report;
    image_root.replace_extension();
    image_root += "-images";
    std::filesystem::create_directories(image_root);
    FrameStatus status = FrameStatus::Fail;
    std::string detail, device = "null";
    std::vector<std::string> scenes;
    try {
      auto renderer = Renderer(executable, status, detail);
      if (renderer) {
        for (bool moving : {false, true}) {
          const auto work = MakeWorkload(options, moving);
          const auto baseline = MakeReference(*renderer, work, options, image_root);
          // Baselines share a renderer, but measured cases each own a fresh
          // one: retained capacity from another mode cannot inflate memory.
          device = baseline.device;
          std::ostringstream scene;
          scene << std::setprecision(12);
          scene << "{\"id\":" << Quote(work.id) << ",\"moving\":" << (moving ? "true" : "false")
                << ",\"reference_split_rmse\":";
          Array(scene, baseline.split_rmse);
          scene << ",\"reference_integral_rmse\":";
          if (moving || !work.exact_integral)
            scene << "null";
          else
            scene << baseline.integral_rmse;
          scene << ",\"independent_integral_rgb\":";
          if (moving || !work.exact_integral)
            scene << "null";
          else
            Array(scene, work.integral);
          scene << ",\"error_reference\":"
                << Quote(work.exact_integral && !moving ? "independent-lambert-integral" : "finite-spp-nee-mis")
                << ",\"cases\":[" << Measure(work, baseline, {"mis"}, options, executable, image_root);
          for (auto candidates : options.candidates)
            for (unsigned mode = 0; mode < 4; ++mode) {
              const char* name = mode == 0 ? "initial" : mode == 1 ? "temporal"
                                                     : mode == 2   ? "spatial"
                                                                   : "spatiotemporal";
              scene << ',' << Measure(work, baseline, {name, candidates, (mode & 1) != 0, (mode & 2) != 0}, options, executable, image_root);
            }
          scene << "]}";
          scenes.push_back(scene.str());
        }
        status = FrameStatus::Pass;
        detail = "measurement completed; no quality or speed threshold asserted";
      }
    } catch (const std::exception& error) {
      status = FrameStatus::Fail;
      detail = error.what();
    }
    std::ostringstream out;
    out << "{\"schema\":\"lotus-restir-benchmark-v1\",\"status\":"
        << Quote(status == FrameStatus::Pass ? "pass" : status == FrameStatus::Skip ? "skip"
                                                                                    : "fail")
        << ",\"detail\":" << Quote(detail) << ",\"started_unix\":" << started
        << ",\"completed_unix\":" << std::time(nullptr) << ",\"build\":{\"label\":" << Quote(options.label)
        << ",\"compiler\":" << Quote(LOTUS_BENCHMARK_COMPILER)
        << ",\"configuration\":" << Quote(LOTUS_BENCHMARK_CONFIG)
        << ",\"system\":" << Quote(LOTUS_BENCHMARK_SYSTEM) << "},\"device\":" << device
        << ",\"procedure\":{\"integrator\":" << Quote(options.integrator == Integrator::Reference ? "reference" : "wavefront")
        << ",\"reference_integrator\":\"reference\",\"candidate_proposal\":" << Quote(ProposalName(options.proposal))
        << ",\"workload_suite\":" << Quote(options.workload)
        << ",\"width\":" << options.size
        << ",\"height\":" << options.size << ",\"frames\":" << options.frames << ",\"warmup_frames\":" << options.warmup
        << ",\"sequences\":" << options.sequences << ",\"analytic_lights\":" << options.lights
        << ",\"reference_samples_per_half\":" << options.reference_samples
        << ",\"reference_halves\":2,\"reference_seed\":" << options.reference_seed
        << ",\"sample_index\":" << options.seed << ",\"sequence_stride\":" << kSequenceStride
        << ",\"max_bounces\":" << options.bounces
        << ",\"fresh_samples_per_frame\":1,\"radiance_reuse\":false,\"history_cap\":" << options.cap
        << ",\"spatial_radius\":" << options.radius << ",\"candidate_counts\":";
    Array(out, options.candidates);
    out << "},\"image_directory\":" << Quote(image_root.filename().string())
        << ",\"metric_notes\":{"
           "\"scene_gpu_ms\":\"Includes metadata and reservoir passes; excludes readback and scene updates. Do not add component times.\","
           "\"render_update_wall_ms\":\"Blocking update and render, including waits, readback and products.\","
           "\"convergence\":\"Static: RMS across sequences of each prefix mean's error to the scene error_reference. CPU averaging only. Moving: null. Finite-spp reference noise is retained.\","
           "\"ensemble\":\"Static averages all frames and sequences; moving averages only the final frame across independent sequences. Finite-sample errors are not proof of bias.\","
           "\"residual_frame_delta_rmse\":\"Fresh frame difference minus matching reference difference; excludes sequence boundaries. Temporal correlation can reduce it without improving convergence.\","
           "\"temporal_residuals\":\"One entry per independent sequence. Residual is fresh RGB minus the matched reference at fixed screen coordinates. Sequence mean RMS retains persistent error; centered RMS removes each component's sequence mean. Lag-one sample autocorrelation uses the full centered sum of squares; null for zero variance. Short sequences, motion and finite reference noise limit interpretation; no effective sample count is inferred.\","
           "\"reference_split_rmse\":\"Difference of disjoint equal-spp NEE/MIS halves; the stored reference is their mean. Reference noise is not subtracted.\","
           "\"memory\":\"Fresh renderer per case. Reservoir and metadata bytes include host mirrors; scene pool backing excludes targets. Total VRAM is unavailable.\","
           "\"timestamps\":\"Unavailable or inapplicable GPU series and cumulative times are null.\"},\"scenes\":[";
    for (std::size_t i = 0; i < scenes.size(); ++i)
      out << (i ? "," : "") << scenes[i];
    out << "]}\n";
    // Publish only a complete JSON document. Invalid CLI arguments never reach
    // output creation; failed measurements carry an explicit failure status.
    auto temporary = options.report;
    temporary += ".tmp";
    {
      std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
      file << out.str();
      file.close();
      Require(!file.fail(), "cannot write benchmark report");
    }
    std::filesystem::rename(temporary, options.report);
    std::cout << detail << '\n';
    return status == FrameStatus::Pass ? 0 : status == FrameStatus::Skip ? 77
                                                                         : 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
} // namespace LotusHeadless
