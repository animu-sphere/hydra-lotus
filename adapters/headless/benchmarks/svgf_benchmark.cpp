// SPDX-License-Identifier: Apache-2.0
#include "svgf_benchmark.hpp"
#include "common/reference.hpp"

#include <lotus/vulkan_backend.hpp>

#include <algorithm>
#include <charconv>
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
#include <tuple>

namespace LotusHeadless {
namespace {
using namespace Lotus;
constexpr double kPi = 3.14159265358979323846;
constexpr unsigned kSeed = 17, kSequenceStride = 65537, kReferenceSeed = 1U << 24;
// What each frame is compared as: the fresh batch, the temporal mean and the
// filtered composite of the same render.
constexpr std::array<const char*, 3> kOutputs{"raw", "temporal", "filtered"};
// History length in batches: 1, 2-3, 4-7 and 8 or more.
constexpr std::array<const char*, 4> kBuckets{"1", "2-3", "4-7", "8+"};

struct Options {
  std::filesystem::path report;
  std::string label;
  unsigned size = 128, frames = 32, sequences = 4, batch = 1;
  unsigned reference_samples = 1024, cap = 32, iterations = kTemporalFilterIterations;
  unsigned seed = kSeed, reference_seed = kReferenceSeed, bounces = 4;
  Integrator integrator = Integrator::Reference;
};

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

double Median(std::vector<double> values) {
  if (values.empty())
    return 0;
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
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
    throw std::invalid_argument("usage: lotus-headless --benchmark-svgf <json-path> "
                                "[--benchmark-label <identity>] [--benchmark-integrator <reference|wavefront>] "
                                "[--benchmark-size <16..512>] [--benchmark-frames <4..256>] "
                                "[--svgf-sequences <2..64>] [--svgf-batch <1..64>] "
                                "[--svgf-seed <0..8388607>] [--svgf-reference-seed <16777216..1073741823>] "
                                "[--svgf-reference-samples <16..8192>] [--svgf-bounces <1..64>] "
                                "[--svgf-history-cap <1..65536>] [--svgf-iterations <1..8>]");
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
    } else if (option == "--benchmark-size")
      options.size = Number(value, 16, 512);
    else if (option == "--benchmark-frames")
      options.frames = Number(value, 4, 256);
    else if (option == "--svgf-sequences")
      options.sequences = Number(value, 2, 64);
    else if (option == "--svgf-batch")
      options.batch = Number(value, 1, 64);
    else if (option == "--svgf-seed")
      options.seed = Number(value, 0, (1U << 23) - 1);
    else if (option == "--svgf-reference-seed")
      options.reference_seed = Number(value, kReferenceSeed, (1U << 30) - 1);
    else if (option == "--svgf-reference-samples")
      options.reference_samples = Number(value, 16, 8192);
    else if (option == "--svgf-bounces")
      options.bounces = Number(value, 1, 64);
    else if (option == "--svgf-history-cap")
      options.cap = Number(value, 1, 65536);
    else if (option == "--svgf-iterations")
      options.iterations = Number(value, 1, kTemporalFilterMaxIterations);
    else
      throw std::invalid_argument("unknown benchmark option: " + std::string(option));
  }
  // Each sequence's batches stay inside its stride, below the references.
  if (options.frames * options.batch > kSequenceStride ||
      options.seed + std::uint64_t{options.sequences} * kSequenceStride > (1U << 23))
    throw std::invalid_argument("measured sample ranges overlap");
  return options;
}

Matrix4 Translation(float x, float y = 0, float z = 0) {
  auto matrix = IdentityMatrix();
  matrix[12] = x;
  matrix[13] = y;
  matrix[14] = z;
  return matrix;
}

MeshGeometry Quad(std::array<float, 3> a, std::array<float, 3> b, std::array<float, 3> c, std::array<float, 3> d) {
  return {{a, b, c, d}, {{0, 1, 2}, {0, 2, 3}}, {0, 0}};
}

// Axis-aligned box from its minimum and maximum corners, twelve triangles.
MeshGeometry Box(std::array<float, 3> low, std::array<float, 3> high) {
  MeshGeometry box;
  for (unsigned i = 0; i < 8; ++i)
    box.positions.push_back({i & 1 ? high[0] : low[0], i & 2 ? high[1] : low[1], i & 4 ? high[2] : low[2]});
  box.triangles = {{0, 1, 3}, {0, 3, 2}, {4, 6, 7}, {4, 7, 5}, {0, 4, 5}, {0, 5, 1},
      {2, 3, 7}, {2, 7, 6}, {0, 2, 6}, {0, 6, 4}, {1, 5, 7}, {1, 7, 3}};
  box.source_faces = {0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5};
  return box;
}

struct Workload {
  std::string id;
  bool moving = false;
  std::vector<FrameSnapshot> snapshots;
};

// A closed room lit by an emissive ceiling panel, so most light is indirect.
// The floor is a textured checker, which demodulation must keep sharp; a
// rough metal block shows glossy reflection. The moving variant pans the
// camera and slides the block, exposing floor and wall behind it.
Workload MakeRoom(const Options& options, bool moving) {
  Workload result;
  result.moving = moving;
  result.id = moving ? "svgf-room-moving-camera-block-v1" : "svgf-room-static-v1";
  RenderWorld world;
  auto lambert = [&](const char* id, std::array<float, 3> albedo) {
    Material material;
    material.ior = 1;
    material.base_color = albedo;
    world.SetMaterial(id, material);
  };
  lambert("/white", {0.8F, 0.8F, 0.8F});
  lambert("/red", {0.8F, 0.15F, 0.1F});
  lambert("/green", {0.15F, 0.7F, 0.2F});
  Texture checker;
  checker.width = checker.height = 8;
  for (unsigned y = 0; y < 8; ++y)
    for (unsigned x = 0; x < 8; ++x) {
      const bool light = (x + y) % 2 == 0;
      const std::uint8_t texel[4]{static_cast<std::uint8_t>(light ? 220 : 40),
          static_cast<std::uint8_t>(light ? 200 : 50), static_cast<std::uint8_t>(light ? 170 : 80), 255};
      checker.texels.insert(checker.texels.end(), texel, texel + 4);
    }
  world.SetTexture("/checker", std::move(checker));
  Material floor;
  floor.ior = 1;
  floor.base_color_texture = TextureInput{"/checker", 0, TextureWrap::Repeat, TextureWrap::Repeat};
  floor.texcoords = "st";
  world.SetMaterial("/floor", floor);
  Material metal;
  metal.base_color = {0.9F, 0.75F, 0.5F};
  metal.metallic = 1;
  metal.roughness = 0.3F;
  world.SetMaterial("/metal", metal);
  Material panel;
  panel.base_color = {0.8F, 0.8F, 0.8F};
  panel.emission = {15, 14, 12};
  world.SetMaterial("/panel", panel);
  constexpr float x = 2, y = 2, back = -7, front = 1;
  auto floor_quad = Quad({-x, -y, back}, {x, -y, back}, {x, -y, front}, {-x, -y, front});
  floor_quad.texcoords["st"] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
  const std::array<std::tuple<const char*, MeshGeometry, const char*>, 7> meshes{{
      {"/floor", floor_quad, "/floor"},
      {"/ceiling", Quad({-x, y, back}, {-x, y, front}, {x, y, front}, {x, y, back}), "/white"},
      {"/back", Quad({-x, -y, back}, {-x, y, back}, {x, y, back}, {x, -y, back}), "/white"},
      {"/front", Quad({-x, -y, front}, {x, -y, front}, {x, y, front}, {-x, y, front}), "/white"},
      {"/left", Quad({-x, -y, back}, {-x, -y, front}, {-x, y, front}, {-x, y, back}), "/red"},
      {"/right", Quad({x, -y, back}, {x, y, back}, {x, y, front}, {x, -y, front}), "/green"},
      {"/panel", Quad({-0.6F, 1.95F, -4.6F}, {-0.6F, 1.95F, -3.4F}, {0.6F, 1.95F, -3.4F}, {0.6F, 1.95F, -4.6F}),
          "/panel"}}};
  for (const auto& [id, geometry, material] : meshes) {
    world.SetMesh(id, geometry, {});
    world.BindMaterial(id, material);
  }
  world.SetMesh("/tall", Box({-1.3F, -y, -5.5F}, {-0.3F, 0.4F, -4.5F}), {});
  world.BindMaterial("/tall", "/white");
  world.SetMesh("/block", Box({0.4F, -y, -4.2F}, {1.4F, -1, -3.2F}), {});
  world.BindMaterial("/block", "/metal");
  for (unsigned f = 0; f < options.frames; ++f) {
    Camera camera;
    camera.projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1.2F, -1, 0, 0, -2.2F, 0};
    if (moving) {
      const float phase = static_cast<float>(2 * kPi * f / options.frames);
      camera.view = Translation(0.4F * std::sin(phase));
      MeshInstance instance;
      instance.world_from_object = Translation(-0.8F * std::sin(phase + 0.3F) + 0.8F * std::sin(0.3F));
      world.SetMeshInstance("/block", instance);
    }
    world.SetCamera(camera);
    result.snapshots.push_back(world.Commit());
  }
  return result;
}

std::unique_ptr<OffscreenRenderer> Renderer(const std::filesystem::path& executable,
    FrameStatus& status, std::string& detail) {
  const auto shaders = ShadersIn(executable / "shaders");
  auto renderer = CreateOffscreenRenderer(
      shaders.vertex, shaders.fragment, status, detail, shaders.ray_query);
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

void RequireFrame(const GpuFrameEvidence& frame, unsigned size) {
  Require(frame.status == FrameStatus::Pass, frame.detail);
  Require(frame.validation_message_count == 0, frame.validation_detail);
  Require(frame.color.width == size && frame.color.height == size && frame.color.row_pitch == size * 16 &&
              frame.color.pixel_format == "rgba32-sfloat" && frame.color.origin == "top-left" &&
              frame.color.payload.size() == std::size_t{size} * size * 16,
      "wrong colour product layout");
}

// RGB of top-left RGBA float rows.
std::vector<float> Rgb(const float* rgba, std::size_t pixels) {
  std::vector<float> values(pixels * 3);
  for (std::size_t i = 0; i < pixels; ++i)
    std::memcpy(values.data() + i * 3, rgba + i * 4, 12);
  for (auto value : values)
    Require(std::isfinite(value), "nonfinite radiance");
  return values;
}

struct Baseline {
  // Per pose: the mean of two halves and each component's variance in it.
  std::vector<std::vector<float>> images;
  std::vector<std::vector<double>> variance;
  std::vector<double> split_rmse;
  std::string device = "null";
};

Baseline MakeReference(OffscreenRenderer& renderer, const Workload& work, const Options& options,
    const std::filesystem::path& images) {
  Baseline result;
  SceneExtraction extraction;
  OffscreenTarget target;
  target.width = target.height = options.size;
  const std::size_t pixels = std::size_t{options.size} * options.size;
  // The reference integrator for both measured integrators, an independent
  // transport check. Each half has a disjoint sample range.
  PathTracingSettings settings;
  settings.max_bounces = options.bounces;
  for (unsigned f = 0; f < (work.moving ? options.frames : 1); ++f) {
    Upload(renderer, extraction, work.snapshots[f]);
    std::array<std::vector<float>, 2> halves;
    for (unsigned half = 0; half < 2; ++half) {
      settings.sample_index = options.reference_seed + (2 * f + half) * options.reference_samples;
      const auto frame = renderer.RenderScene(ExtractDrawSummary(work.snapshots[f]), target,
          options.reference_samples, settings);
      RequireFrame(frame, options.size);
      Require(frame.samples_per_pixel == options.reference_samples, "wrong reference sample count");
      halves[half] = Rgb(reinterpret_cast<const float*>(frame.color.payload.data()), pixels);
      result.device = "{\"name\":" + Quote(frame.device_name) + ",\"api\":" + Quote(frame.api_version) +
                      ",\"driver\":" + Quote(frame.driver_version) + ",\"validation_available\":" +
                      (frame.validation_available ? "true" : "false") + '}';
    }
    auto& variance = result.variance.emplace_back(halves[0].size());
    auto& mean = result.images.emplace_back(halves[0].size());
    double squared = 0;
    for (std::size_t i = 0; i < mean.size(); ++i) {
      const double difference = double(halves[0][i]) - halves[1][i];
      squared += difference * difference;
      variance[i] = difference * difference / 4;
      mean[i] = (halves[0][i] + halves[1][i]) * 0.5F;
    }
    result.split_rmse.push_back(std::sqrt(squared / mean.size()));
  }
  const RgbImage last{options.size, options.size, result.images.back()};
  Require(WritePfm(images / (work.id + "-reference.pfm"), last).empty(), "cannot save reference image");
  return result;
}

std::uint64_t Hash(std::uint64_t hash, const void* data, std::size_t bytes) {
  for (std::size_t i = 0; i < bytes; ++i) {
    hash ^= static_cast<const std::uint8_t*>(data)[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

struct OutputStatistics {
  // Per frame, summed over sequences.
  std::vector<double> mse, delta;
  std::array<double, 4> bucket_squared{};
  std::array<std::uint64_t, 4> bucket_components{};
  // The last frame across sequences, per component.
  std::vector<double> last_sum, last_squares;
};

struct Run {
  std::array<OutputStatistics, 3> outputs;
  std::vector<double> filter_ms, accumulation_ms, variance_ms, surface_ms, reprojection_ms;
  std::vector<std::uint64_t> hashes;
  std::uint64_t filter_bytes = 0, radiance_bytes = 0;
};

// One temporal sequence from a camera cut: the raw batch, the temporal mean
// and the filtered composite of every frame, against the pose's reference.
void Sequence(OffscreenRenderer& renderer, SceneExtraction& extraction, const Workload& work,
    const Baseline& baseline, const Options& options, unsigned sequence, std::uint64_t& frame_index,
    Run& run, const std::filesystem::path& images, bool save) {
  OffscreenTarget target;
  target.width = target.height = options.size;
  const std::size_t pixels = std::size_t{options.size} * options.size;
  PathTracingSettings settings;
  settings.integrator = options.integrator;
  settings.max_bounces = options.bounces;
  settings.sample_index = options.seed + sequence * kSequenceStride;
  TemporalFrameSettings temporal;
  temporal.accumulate_radiance = true;
  temporal.max_history_samples = options.cap;
  temporal.filter_iterations = options.iterations;
  std::array<std::vector<double>, 3> previous;
  for (unsigned f = 0; f < options.frames; ++f) {
    Upload(renderer, extraction, work.snapshots[f]);
    temporal.frame_index = frame_index++;
    temporal.camera_cut = f == 0;
    const auto frame = renderer.RenderTemporalScene(work.snapshots[f], target, options.batch, temporal, settings);
    RequireFrame(frame, options.size);
    const auto& products = frame.temporal;
    Require(products.radiance_available && products.filter_iterations == options.iterations &&
                products.radiance.size() == pixels && products.batch.size() == pixels,
        "missing temporal products");
    Require(products.history_available == (f != 0), "unexpected history reset");
    std::vector<float> temporal_rgba(pixels * 4);
    for (std::size_t i = 0; i < pixels; ++i)
      std::memcpy(temporal_rgba.data() + i * 4, products.radiance[i].color.data(), 16);
    const std::array<std::vector<float>, 3> values{
        Rgb(products.batch.front().data(), pixels), Rgb(temporal_rgba.data(), pixels),
        Rgb(reinterpret_cast<const float*>(frame.color.payload.data()), pixels)};
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto& value : values)
      hash = Hash(hash, value.data(), value.size() * sizeof(float));
    run.hashes.push_back(hash);
    if (products.timestamp_available) {
      run.filter_ms.push_back(products.filter_gpu_ms);
      run.accumulation_ms.push_back(products.accumulation_gpu_ms);
      run.variance_ms.push_back(products.variance_gpu_ms);
      run.surface_ms.push_back(products.primary_surface_gpu_ms);
      run.reprojection_ms.push_back(products.reprojection_gpu_ms);
    }
    run.filter_bytes = products.filter_bytes;
    run.radiance_bytes = products.radiance_bytes;
    const auto& reference = baseline.images[work.moving ? f : 0];
    const float batch = static_cast<float>(std::min(options.batch, options.cap));
    for (std::size_t o = 0; o < values.size(); ++o) {
      auto& statistics = run.outputs[o];
      std::vector<double> error(values[o].size());
      double squared = 0;
      for (std::size_t k = 0; k < error.size(); ++k) {
        error[k] = double(values[o][k]) - reference[k];
        squared += error[k] * error[k];
      }
      statistics.mse[f] += squared / error.size();
      if (f != 0) {
        double delta = 0;
        for (std::size_t k = 0; k < error.size(); ++k)
          delta += std::pow(error[k] - previous[o][k], 2);
        statistics.delta[f] += delta / error.size();
        // The cut frame would fill the first bucket with every pixel.
        for (std::size_t i = 0; i < pixels; ++i) {
          const auto count = products.radiance[i].sample_count;
          if (count == 0)
            continue;
          const float batches = static_cast<float>(count) / batch;
          const std::size_t bucket = batches < 2 ? 0 : batches < 4 ? 1 : batches < 8 ? 2 : 3;
          for (std::size_t c = 0; c < 3; ++c)
            statistics.bucket_squared[bucket] += error[i * 3 + c] * error[i * 3 + c];
          statistics.bucket_components[bucket] += 3;
        }
      }
      if (f + 1 == options.frames) {
        for (std::size_t k = 0; k < error.size(); ++k) {
          statistics.last_sum[k] += values[o][k];
          statistics.last_squares[k] += double(values[o][k]) * values[o][k];
        }
        if (save) {
          const RgbImage image{options.size, options.size, values[o]};
          Require(WritePfm(images / (work.id + "-" + kOutputs[o] + ".pfm"), image).empty(),
              "cannot save measured image");
        }
      }
      previous[o] = std::move(error);
    }
  }
}

// A fresh renderer per workload; its sequences share one extraction.
std::string Measure(const Workload& work, const Baseline& baseline, const Options& options,
    const std::filesystem::path& executable, const std::filesystem::path& images) {
  FrameStatus setup;
  std::string detail;
  auto renderer = Renderer(executable, setup, detail);
  Require(renderer != nullptr, detail);
  SceneExtraction extraction;
  std::uint64_t frame_index = 0;
  Run run;
  const std::size_t components = std::size_t{options.size} * options.size * 3;
  for (auto& statistics : run.outputs) {
    statistics.mse.assign(options.frames, 0);
    statistics.delta.assign(options.frames, 0);
    statistics.last_sum.assign(components, 0);
    statistics.last_squares.assign(components, 0);
  }
  for (unsigned sequence = 0; sequence < options.sequences; ++sequence)
    Sequence(*renderer, extraction, work, baseline, options, sequence, frame_index, run, images, sequence == 0);
  // Deterministic validation: the first sequence again, from a new cut, must
  // reproduce every output bit for bit.
  Run repeat;
  for (auto& statistics : repeat.outputs) {
    statistics.mse.assign(options.frames, 0);
    statistics.delta.assign(options.frames, 0);
    statistics.last_sum.assign(components, 0);
    statistics.last_squares.assign(components, 0);
  }
  Sequence(*renderer, extraction, work, baseline, options, 0, frame_index, repeat, images, false);
  const bool deterministic = std::equal(repeat.hashes.begin(), repeat.hashes.end(), run.hashes.begin());

  const double sequences = options.sequences;
  const auto& last_variance = baseline.variance.back();
  const auto& last_reference = baseline.images.back();
  double reference_noise = 0;
  for (const auto& pose : baseline.variance)
    reference_noise += std::accumulate(pose.begin(), pose.end(), 0.0) / pose.size();
  reference_noise /= baseline.variance.size();
  std::ostringstream out;
  out << std::setprecision(9);
  out << "{\"id\":" << Quote(work.id) << ",\"moving\":" << (work.moving ? "true" : "false")
      << ",\"reference_split_rmse\":";
  Array(out, baseline.split_rmse);
  out << ",\"reference_noise_mse\":" << reference_noise << ",\"deterministic\":"
      << (deterministic ? "true" : "false") << ",\"outputs\":{";
  for (std::size_t o = 0; o < kOutputs.size(); ++o) {
    const auto& statistics = run.outputs[o];
    std::vector<double> mse(options.frames), delta(options.frames - 1);
    for (unsigned f = 0; f < options.frames; ++f) {
      mse[f] = statistics.mse[f] / sequences;
      if (f != 0)
        delta[f - 1] = statistics.delta[f] / sequences;
    }
    // The last frame across independent sequences: the squared error of their
    // mean, less its expected noise, estimates the squared bias.
    double mean_squared = 0, noise = 0, last_reference_noise = 0;
    for (std::size_t k = 0; k < components; ++k) {
      const double mean = statistics.last_sum[k] / sequences;
      const double spread = (statistics.last_squares[k] - sequences * mean * mean) / (sequences - 1);
      mean_squared += std::pow(mean - last_reference[k], 2);
      noise += std::max(spread, 0.0) / sequences;
      last_reference_noise += last_variance[k];
    }
    mean_squared /= components;
    noise /= components;
    last_reference_noise /= components;
    const auto Mean = [](const std::vector<double>& values) {
      return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
    };
    out << (o ? "," : "") << Quote(kOutputs[o]) << ":{\"mse\":" << Mean(mse)
        << ",\"rmse\":" << std::sqrt(Mean(mse)) << ",\"mse_by_frame\":";
    Array(out, mse);
    out << ",\"frame_delta_mse\":" << Mean(delta) << ",\"frame_delta_mse_by_frame\":";
    Array(out, delta);
    out << ",\"history_buckets\":[";
    for (std::size_t b = 0; b < kBuckets.size(); ++b) {
      out << (b ? "," : "") << "{\"batches\":" << Quote(kBuckets[b])
          << ",\"pixels\":" << statistics.bucket_components[b] / 3 << ",\"mse\":";
      if (statistics.bucket_components[b] == 0)
        out << "null";
      else
        out << statistics.bucket_squared[b] / statistics.bucket_components[b];
      out << '}';
    }
    out << "],\"last_frame_ensemble\":{\"mean_mse\":" << mean_squared << ",\"noise_mse\":" << noise
        << ",\"reference_noise_mse\":" << last_reference_noise
        << ",\"excess_mse\":" << mean_squared - noise - last_reference_noise << "}}";
  }
  out << "},\"gpu_ms_median\":";
  if (run.filter_ms.empty())
    out << "null";
  else
    out << "{\"primary_surface\":" << Median(run.surface_ms) << ",\"reprojection\":" << Median(run.reprojection_ms)
        << ",\"accumulation\":" << Median(run.accumulation_ms) << ",\"variance\":" << Median(run.variance_ms)
        << ",\"filter\":" << Median(run.filter_ms) << '}';
  out << ",\"memory\":{\"radiance_bytes\":" << run.radiance_bytes << ",\"filter_bytes\":" << run.filter_bytes
      << "}}";
  return out.str();
}

} // namespace

int SvgfBenchmarkMain(int argc, char** argv) {
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
      for (bool moving : {false, true}) {
        // Each reference has its own renderer too: both workloads upload the
        // same geometry.
        auto renderer = Renderer(executable, status, detail);
        if (!renderer)
          break;
        const auto work = MakeRoom(options, moving);
        const auto baseline = MakeReference(*renderer, work, options, image_root);
        device = baseline.device;
        scenes.push_back(Measure(work, baseline, options, executable, image_root));
      }
      if (scenes.size() == 2) {
        status = FrameStatus::Pass;
        detail = "measurement completed; no quality or speed threshold asserted";
      }
    } catch (const std::exception& error) {
      status = FrameStatus::Fail;
      detail = error.what();
    }
    std::ostringstream out;
    out << "{\"schema\":\"lotus-svgf-benchmark-v1\",\"status\":"
        << Quote(status == FrameStatus::Pass ? "pass" : status == FrameStatus::Skip ? "skip"
                                                                                    : "fail")
        << ",\"detail\":" << Quote(detail) << ",\"started_unix\":" << started
        << ",\"completed_unix\":" << std::time(nullptr) << ",\"build\":{\"label\":" << Quote(options.label)
        << ",\"compiler\":" << Quote(LOTUS_BENCHMARK_COMPILER)
        << ",\"configuration\":" << Quote(LOTUS_BENCHMARK_CONFIG)
        << ",\"system\":" << Quote(LOTUS_BENCHMARK_SYSTEM) << "},\"device\":" << device
        << ",\"procedure\":{\"integrator\":" << Quote(options.integrator == Integrator::Reference ? "reference" : "wavefront")
        << ",\"reference_integrator\":\"reference\",\"light_sampling\":\"nee-mis\",\"workload\":\"svgf-room\""
        << ",\"width\":" << options.size << ",\"height\":" << options.size << ",\"frames\":" << options.frames
        << ",\"sequences\":" << options.sequences << ",\"fresh_samples_per_frame\":" << options.batch
        << ",\"history_cap\":" << options.cap << ",\"filter_iterations\":" << options.iterations
        << ",\"max_bounces\":" << options.bounces << ",\"sample_index\":" << options.seed
        << ",\"sequence_stride\":" << kSequenceStride << ",\"reference_seed\":" << options.reference_seed
        << ",\"reference_samples_per_half\":" << options.reference_samples << ",\"reference_halves\":2}"
        << ",\"image_directory\":" << Quote(image_root.filename().string())
        << ",\"metric_notes\":{"
           "\"outputs\":\"One render per frame: raw is its fresh batch, temporal the bounded temporal mean (the history), filtered the a-trous composite of that mean. The filter never feeds back into history.\","
           "\"mse\":\"RGB squared error against the pose's reference, every pixel, averaged over sequences and then frames. Finite reference noise (reference_noise_mse) is included, not subtracted.\","
           "\"frame_delta_mse\":\"Temporal residual: each frame's error minus the previous frame's at the same pixel, squared. It measures flicker and lag; the cut frame has none.\","
           "\"history_buckets\":\"Squared error by the pixel's history length in batches, frames after the cut pooled: the response after disocclusion or rejection.\","
           "\"last_frame_ensemble\":\"The last frame across independent sequences. Mean MSE is the squared error of their mean; noise is its expected sampling part; excess estimates the squared bias, and can be negative.\","
           "\"deterministic\":\"The first sequence rendered again from a new cut reproduced every raw, temporal and filtered value bit for bit.\","
           "\"gpu_ms_median\":\"Per frame, with whatever validation layers are active; filter covers every iteration and the composite.\"},\"scenes\":[";
    for (std::size_t i = 0; i < scenes.size(); ++i)
      out << (i ? "," : "") << scenes[i];
    out << "]}\n";
    // Publish only a complete JSON document.
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
