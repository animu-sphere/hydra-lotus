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
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace LotusHeadless {
namespace {
using namespace Lotus;
constexpr double kPi = 3.14159265358979323846;
constexpr unsigned kSeed = 17, kSequenceStride = 65537, kReferenceSeed = 1U << 24;
// What each frame is compared as: the fresh batch, the temporal mean and the
// filtered composite of the same render, and a separate progressive render
// restarted on every change, which is what the Reference preset shows.
constexpr std::array<const char*, 4> kOutputs{"raw", "temporal", "filtered", "progressive"};
constexpr std::size_t kMeasuredOutputs = 3;
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

Matrix4 RotationX(float radians) {
  auto matrix = IdentityMatrix();
  matrix[5] = matrix[10] = std::cos(radians);
  matrix[6] = std::sin(radians);
  matrix[9] = -std::sin(radians);
  return matrix;
}

// Where a fixture's lag shows: no region, the pixels whose primary surface has
// one material's base colour, or the pixels whose reference changes between
// poses of a fixture whose visible surfaces never move.
enum class LagRegion { None, Albedo, Swept };

struct Workload {
  std::string id;
  // The contract's fixture this workload stands for.
  std::string fixture;
  // A reference per frame; otherwise every frame shares the first pose's.
  bool moving = false;
  LagRegion lag = LagRegion::None;
  std::array<float, 3> lag_albedo{};
  std::vector<FrameSnapshot> snapshots;
};

void Lambert(RenderWorld& world, const char* id, std::array<float, 3> albedo) {
  Material material;
  material.ior = 1;
  material.base_color = albedo;
  world.SetMaterial(id, material);
}

void Place(RenderWorld& world, const char* id, MeshGeometry geometry, const char* material) {
  world.SetMesh(id, std::move(geometry), {});
  world.BindMaterial(id, material);
}

// svgf-room's shell: X and Y in [-2, 2], Z in [-7, 1], red left and green
// right walls, white elsewhere, and an emissive ceiling panel. The floor's
// material is the caller's.
void RoomShell(RenderWorld& world, const char* floor_material) {
  Lambert(world, "/white", {0.8F, 0.8F, 0.8F});
  Lambert(world, "/red", {0.8F, 0.15F, 0.1F});
  Lambert(world, "/green", {0.15F, 0.7F, 0.2F});
  Material panel;
  panel.base_color = {0.8F, 0.8F, 0.8F};
  panel.emission = {15, 14, 12};
  world.SetMaterial("/panel", panel);
  constexpr float x = 2, y = 2, back = -7, front = 1;
  auto floor_quad = Quad({-x, -y, back}, {x, -y, back}, {x, -y, front}, {-x, -y, front});
  floor_quad.texcoords["st"] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
  Place(world, "/floor", floor_quad, floor_material);
  Place(world, "/ceiling", Quad({-x, y, back}, {-x, y, front}, {x, y, front}, {x, y, back}), "/white");
  Place(world, "/back", Quad({-x, -y, back}, {-x, y, back}, {x, y, back}, {x, -y, back}), "/white");
  Place(world, "/front", Quad({-x, -y, front}, {x, -y, front}, {x, y, front}, {-x, y, front}), "/white");
  Place(world, "/left", Quad({-x, -y, back}, {-x, -y, front}, {-x, y, front}, {-x, y, back}), "/red");
  Place(world, "/right", Quad({x, -y, back}, {x, y, back}, {x, y, front}, {x, -y, front}), "/green");
  Place(world, "/panel",
      Quad({-0.6F, 1.95F, -4.6F}, {-0.6F, 1.95F, -3.4F}, {0.6F, 1.95F, -3.4F}, {0.6F, 1.95F, -4.6F}), "/panel");
}

Camera Pinhole(Matrix4 view = IdentityMatrix()) {
  Camera camera;
  camera.view = view;
  camera.projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1.2F, -1, 0, 0, -2.2F, 0};
  return camera;
}

float Phase(const Options& options, unsigned frame) {
  return static_cast<float>(2 * kPi * frame / options.frames);
}

// A closed room lit by an emissive ceiling panel, so most light is indirect.
// The floor is a textured checker, which demodulation must keep sharp; a
// rough metal block shows glossy reflection. The moving variant pans the
// camera and slides the block, exposing floor and wall behind it.
Workload MakeRoom(const Options& options, bool moving) {
  Workload result;
  result.moving = moving;
  result.fixture = "svgf-room";
  result.id = moving ? "svgf-room-moving-camera-block-v1" : "svgf-room-static-v1";
  RenderWorld world;
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
  RoomShell(world, "/floor");
  Place(world, "/tall", Box({-1.3F, -2, -5.5F}, {-0.3F, 0.4F, -4.5F}), "/white");
  Place(world, "/block", Box({0.4F, -2, -4.2F}, {1.4F, -1, -3.2F}), "/metal");
  for (unsigned f = 0; f < options.frames; ++f) {
    Camera camera = Pinhole();
    if (moving) {
      const float phase = Phase(options, f);
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

// Shadow lag. A still camera three units up looks 60 degrees down at a floor
// and sees nothing above its own height. A small panel above and behind it
// lights the floor through an occluder that, just above the camera, slides in
// X and sweeps a shadow about 2.2 times its size across the floor. Every
// visible surface is still and passes validity, so any change is lighting the
// history must catch up with; its region is where the reference changes.
Workload MakeShadowSweep(const Options& options) {
  Workload result;
  result.id = "occluder-shadow-sweep-v1";
  result.fixture = "moving-occluder-still-receiver";
  result.moving = true;
  result.lag = LagRegion::Swept;
  RenderWorld world;
  Lambert(world, "/receiver", {0.7F, 0.68F, 0.62F});
  Lambert(world, "/wall", {0.8F, 0.8F, 0.8F});
  Lambert(world, "/occluder", {0.5F, 0.5F, 0.5F});
  Material panel;
  panel.base_color = {0.8F, 0.8F, 0.8F};
  panel.emission = {300, 290, 270};
  world.SetMaterial("/panel", panel);
  Place(world, "/floor", Quad({-10, 0, -12}, {10, 0, -12}, {10, 0, 3}, {-10, 0, 3}), "/receiver");
  Place(world, "/back", Quad({-10, 0, -12}, {-10, 6, -12}, {10, 6, -12}, {10, 0, -12}), "/wall");
  Place(world, "/panel", Quad({-0.25F, 6, 1.75F}, {-0.25F, 6, 2.25F}, {0.25F, 6, 2.25F}, {0.25F, 6, 1.75F}), "/panel");
  Place(world, "/occluder", Box({-0.4F, 3.25F, -1.1F}, {0.4F, 3.35F, -0.3F}), "/occluder");
  world.SetCamera(Pinhole(Multiply(RotationX(static_cast<float>(kPi / 3)), Translation(0, -3))));
  for (unsigned f = 0; f < options.frames; ++f) {
    MeshInstance instance;
    instance.world_from_object = Translation(0.9F * std::sin(Phase(options, f)));
    world.SetMeshInstance("/occluder", instance);
    result.snapshots.push_back(world.Commit());
  }
  return result;
}

// Reflection lag. svgf-room's shell with a low-roughness metal floor and three
// coloured posts; the camera, tilted down, pans across. The reflected posts
// move with their mirrored depth, not with the floor's motion vectors.
Workload MakeGlossyPan(const Options& options) {
  Workload result;
  result.id = "glossy-floor-camera-pan-v1";
  result.fixture = "camera-over-low-roughness-surface";
  result.moving = true;
  result.lag = LagRegion::Albedo;
  RenderWorld world;
  Material mirror;
  mirror.base_color = result.lag_albedo = {0.94F, 0.92F, 0.88F};
  mirror.metallic = 1;
  mirror.roughness = 0.1F;
  world.SetMaterial("/mirror", mirror);
  Lambert(world, "/post-red", {0.85F, 0.2F, 0.15F});
  Lambert(world, "/post-yellow", {0.85F, 0.75F, 0.2F});
  Lambert(world, "/post-blue", {0.2F, 0.35F, 0.85F});
  RoomShell(world, "/mirror");
  Place(world, "/post-a", Box({-1.4F, -2, -5.6F}, {-0.9F, 0.2F, -5.1F}), "/post-red");
  Place(world, "/post-b", Box({-0.25F, -2, -6.2F}, {0.25F, 0.8F, -5.7F}), "/post-yellow");
  Place(world, "/post-c", Box({0.9F, -2, -5.0F}, {1.4F, -0.4F, -4.5F}), "/post-blue");
  for (unsigned f = 0; f < options.frames; ++f) {
    const float phase = Phase(options, f);
    world.SetCamera(Pinhole(Multiply(RotationX(0.3F), Translation(-0.6F * std::sin(phase), -0.4F))));
    result.snapshots.push_back(world.Commit());
  }
  return result;
}

// Deformation. svgf-room's shell with a white floor and a still camera
// facing a waving sheet of fixed topology. Today every pixel resets on its
// every frame; the contract keeps the history of the pixels it leaves.
Workload MakeDeformingSheet(const Options& options) {
  Workload result;
  result.id = "deforming-sheet-v1";
  result.fixture = "deforming-mesh";
  result.moving = true;
  result.lag = LagRegion::Albedo;
  RenderWorld world;
  Lambert(world, "/sheet", result.lag_albedo = {0.25F, 0.45F, 0.8F});
  RoomShell(world, "/white");
  world.SetCamera(Pinhole());
  constexpr unsigned columns = 32, rows = 16;
  MeshGeometry sheet;
  for (unsigned r = 0; r < rows; ++r)
    for (unsigned c = 0; c < columns; ++c) {
      const auto at = [](unsigned v, unsigned w) { return v + w * (columns + 1); };
      sheet.triangles.push_back({at(c, r), at(c + 1, r), at(c + 1, r + 1)});
      sheet.triangles.push_back({at(c, r), at(c + 1, r + 1), at(c, r + 1)});
      sheet.source_faces.push_back(r * columns + c);
      sheet.source_faces.push_back(r * columns + c);
    }
  for (unsigned f = 0; f < options.frames; ++f) {
    const float phase = Phase(options, f);
    sheet.positions.clear();
    for (unsigned r = 0; r <= rows; ++r)
      for (unsigned c = 0; c <= columns; ++c) {
        const float u = static_cast<float>(c) / columns, v = static_cast<float>(r) / rows;
        // Pinned along its bottom edge, the wave grows towards the top.
        const float wave = 0.35F * v * std::sin(static_cast<float>(2 * kPi) * u * 1.5F - phase);
        sheet.positions.push_back({-1.3F + 2.6F * u, -1.6F + 2.4F * v, -4.4F + wave});
      }
    Place(world, "/sheet", sheet, "/sheet");
    result.snapshots.push_back(world.Commit());
  }
  return result;
}

// The contract's fixed set, in report order.
std::vector<Workload> Workloads(const Options& options) {
  std::vector<Workload> result;
  result.push_back(MakeRoom(options, false));
  result.push_back(MakeRoom(options, true));
  result.push_back(MakeShadowSweep(options));
  result.push_back(MakeGlossyPan(options));
  result.push_back(MakeDeformingSheet(options));
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
  // LagRegion::Swept: the pixels whose reference changes between poses.
  std::vector<std::uint8_t> swept;
  std::string device = "null";
};

double Luminance(const std::vector<float>& rgb, std::size_t pixel) {
  return 0.2126 * rgb[pixel * 3] + 0.7152 * rgb[pixel * 3 + 1] + 0.0722 * rgb[pixel * 3 + 2];
}

// A pixel is swept when its reference luminance ranges over more than a tenth
// of its brightest pose and six times the references' mean noise, so finite
// reference noise alone never marks it.
std::vector<std::uint8_t> Swept(const Baseline& baseline) {
  const std::size_t pixels = baseline.images.front().size() / 3;
  double noise = 0;
  for (const auto& pose : baseline.variance)
    noise += std::accumulate(pose.begin(), pose.end(), 0.0) / pose.size();
  noise = std::sqrt(noise / baseline.variance.size());
  std::vector<std::uint8_t> swept(pixels);
  for (std::size_t i = 0; i < pixels; ++i) {
    double low = INFINITY, high = 0;
    for (const auto& image : baseline.images) {
      low = std::min(low, Luminance(image, i));
      high = std::max(high, Luminance(image, i));
    }
    swept[i] = high - low > std::max(0.1 * high, 6 * noise);
  }
  return swept;
}

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
  if (work.lag == LagRegion::Swept)
    result.swept = Swept(result);
  return result;
}

const char* ResetName(TemporalResetReason reason) {
  switch (reason) {
  case TemporalResetReason::None:
    return "none";
  case TemporalResetReason::FirstFrame:
    return "first-frame";
  case TemporalResetReason::ExplicitReset:
    return "explicit-reset";
  case TemporalResetReason::CameraCut:
    return "camera-cut";
  case TemporalResetReason::FrameGap:
    return "frame-gap";
  case TemporalResetReason::TargetChanged:
    return "target-changed";
  case TemporalResetReason::HistoryKeyChanged:
    return "history-key-changed";
  case TemporalResetReason::RenderSettingsChanged:
    return "render-settings-changed";
  case TemporalResetReason::SceneChanged:
    return "scene-changed";
  }
  return "unknown";
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
  // The lag region per frame, summed over sequences: squared error and
  // components.
  std::vector<double> lag_squared;
  std::vector<std::uint64_t> lag_components;
  // The last frame across sequences, per component.
  std::vector<double> last_sum, last_squares;
};

struct Run {
  std::array<OutputStatistics, kOutputs.size()> outputs;
  std::vector<double> filter_ms, accumulation_ms, variance_ms, surface_ms, reprojection_ms;
  std::vector<std::uint64_t> hashes;
  std::uint64_t filter_bytes = 0, radiance_bytes = 0;
  // Frames after a sequence's cut that the renderer rendered without history,
  // by reason, and the lag region's pixels summed over frames.
  std::map<std::string, unsigned> resets;
  std::uint64_t lag_pixels = 0;
  // The first sequence's lag region per frame.
  std::vector<std::vector<std::uint8_t>> lag_regions;

  explicit Run(std::size_t frames, std::size_t components) {
    for (auto& statistics : outputs) {
      statistics.mse.assign(frames, 0);
      statistics.delta.assign(frames, 0);
      statistics.lag_squared.assign(frames, 0);
      statistics.lag_components.assign(frames, 0);
      statistics.last_sum.assign(components, 0);
      statistics.last_squares.assign(components, 0);
    }
  }
};

// Whether a pixel's primary surface has the base colour, to unorm8.
bool AlbedoMatches(const TemporalPixel& pixel, const std::array<float, 3>& albedo) {
  if ((pixel.flags & 1U) == 0)
    return false;
  const auto found = UnpackTemporalAlbedo(pixel.albedo);
  for (std::size_t c = 0; c < 3; ++c)
    if (std::abs(found[c] - albedo[c]) > 0.6F / 255)
      return false;
  return true;
}

// One output's frame against the pose's reference. batches gives a pixel's
// history length in batches, or a negative value to leave it out of the
// buckets.
template <typename Batches>
void Score(Run& run, std::size_t o, const Workload& work, const Baseline& baseline, const Options& options,
    unsigned f, const std::vector<float>& value, const std::vector<std::uint8_t>& lag, Batches batches,
    std::vector<double>& previous, const std::filesystem::path& images, bool save) {
  const std::size_t pixels = lag.size();
  const auto& reference = baseline.images[work.moving ? f : 0];
  auto& statistics = run.outputs[o];
  std::vector<double> error(value.size());
  double squared = 0;
  for (std::size_t k = 0; k < error.size(); ++k) {
    error[k] = double(value[k]) - reference[k];
    squared += error[k] * error[k];
  }
  statistics.mse[f] += squared / error.size();
  for (std::size_t i = 0; i < pixels; ++i) {
    if (!lag[i])
      continue;
    for (std::size_t c = 0; c < 3; ++c)
      statistics.lag_squared[f] += error[i * 3 + c] * error[i * 3 + c];
    statistics.lag_components[f] += 3;
  }
  if (f != 0) {
    double delta = 0;
    for (std::size_t k = 0; k < error.size(); ++k)
      delta += std::pow(error[k] - previous[k], 2);
    statistics.delta[f] += delta / error.size();
    // The cut frame would fill the first bucket with every pixel.
    for (std::size_t i = 0; i < pixels; ++i) {
      const float length = batches(i);
      if (length < 0)
        continue;
      const std::size_t bucket = length < 2 ? 0 : length < 4 ? 1 : length < 8 ? 2 : 3;
      for (std::size_t c = 0; c < 3; ++c)
        statistics.bucket_squared[bucket] += error[i * 3 + c] * error[i * 3 + c];
      statistics.bucket_components[bucket] += 3;
    }
  }
  if (f + 1 == options.frames) {
    for (std::size_t k = 0; k < error.size(); ++k) {
      statistics.last_sum[k] += value[k];
      statistics.last_squares[k] += double(value[k]) * value[k];
    }
    if (save) {
      const RgbImage image{options.size, options.size, value};
      Require(WritePfm(images / (work.id + "-" + kOutputs[o] + ".pfm"), image).empty(),
          "cannot save measured image");
    }
  }
  previous = std::move(error);
}

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
  std::array<std::vector<double>, kMeasuredOutputs> previous;
  for (unsigned f = 0; f < options.frames; ++f) {
    Upload(renderer, extraction, work.snapshots[f]);
    temporal.frame_index = frame_index++;
    temporal.camera_cut = f == 0;
    const auto frame = renderer.RenderTemporalScene(work.snapshots[f], target, options.batch, temporal, settings);
    RequireFrame(frame, options.size);
    const auto& products = frame.temporal;
    Require(products.radiance_available && products.filter_iterations == options.iterations &&
                products.radiance.size() == pixels && products.batch.size() == pixels &&
                products.current.size() == pixels,
        "missing temporal products");
    Require(f != 0 || !products.history_available, "history survived a camera cut");
    // Today's behaviour is the measurement: a reset after the cut is counted,
    // not rejected.
    if (f != 0 && !products.history_available)
      ++run.resets[ResetName(products.reset_reason)];
    std::vector<float> temporal_rgba(pixels * 4);
    for (std::size_t i = 0; i < pixels; ++i)
      std::memcpy(temporal_rgba.data() + i * 4, products.radiance[i].color.data(), 16);
    const std::array<std::vector<float>, kMeasuredOutputs> values{
        Rgb(products.batch.front().data(), pixels), Rgb(temporal_rgba.data(), pixels),
        Rgb(reinterpret_cast<const float*>(frame.color.payload.data()), pixels)};
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto& value : values)
      hash = Hash(hash, value.data(), value.size() * sizeof(float));
    run.hashes.push_back(hash);
    // The primary surface is the unjittered pixel centre's, the same in every
    // sequence; the progressive renders reuse the first sequence's regions.
    std::vector<std::uint8_t> lag(pixels);
    for (std::size_t i = 0; i < pixels; ++i)
      lag[i] = work.lag == LagRegion::Swept    ? baseline.swept[i]
               : work.lag == LagRegion::Albedo ? AlbedoMatches(products.current[i], work.lag_albedo)
                                               : 0;
    run.lag_pixels += std::accumulate(lag.begin(), lag.end(), std::uint64_t{0});
    if (run.lag_regions.size() == f)
      run.lag_regions.push_back(lag);
    if (products.timestamp_available) {
      run.filter_ms.push_back(products.filter_gpu_ms);
      run.accumulation_ms.push_back(products.accumulation_gpu_ms);
      run.variance_ms.push_back(products.variance_gpu_ms);
      run.surface_ms.push_back(products.primary_surface_gpu_ms);
      run.reprojection_ms.push_back(products.reprojection_gpu_ms);
    }
    run.filter_bytes = products.filter_bytes;
    run.radiance_bytes = products.radiance_bytes;
    const float batch = static_cast<float>(std::min(options.batch, options.cap));
    const auto history = [&](std::size_t i) {
      const auto count = products.radiance[i].sample_count;
      return count == 0 ? -1.0F : static_cast<float>(count) / batch;
    };
    for (std::size_t o = 0; o < values.size(); ++o)
      Score(run, o, work, baseline, options, f, values[o], lag, history, previous[o], images, save);
  }
}

// What the Reference preset shows over the same frames: the progressive
// render, a batch per frame as hosts draw it, restarting at the sequence's
// sample index on every change. A moving workload therefore shows one batch
// per frame with the same random numbers each time (DES-Q7); the still one
// accumulates.
void Progressive(OffscreenRenderer& renderer, SceneExtraction& extraction, const Workload& work,
    const Baseline& baseline, const Options& options, unsigned sequence, Run& run,
    const std::filesystem::path& images, bool save) {
  OffscreenTarget target;
  target.width = target.height = options.size;
  const std::size_t pixels = std::size_t{options.size} * options.size;
  PathTracingSettings settings;
  settings.integrator = options.integrator;
  settings.max_bounces = options.bounces;
  settings.sample_index = options.seed + sequence * kSequenceStride;
  std::vector<double> previous;
  for (unsigned f = 0; f < options.frames; ++f) {
    Upload(renderer, extraction, work.snapshots[f]);
    const unsigned batches = work.moving ? 1 : f + 1;
    const auto frame =
        renderer.RenderScene(ExtractDrawSummary(work.snapshots[f]), target, options.batch, settings);
    RequireFrame(frame, options.size);
    Require(frame.samples_per_pixel == batches * options.batch, "wrong progressive sample count");
    const auto value = Rgb(reinterpret_cast<const float*>(frame.color.payload.data()), pixels);
    Score(run, kOutputs.size() - 1, work, baseline, options, f, value, run.lag_regions[f],
        [batches](std::size_t) { return static_cast<float>(batches); }, previous, images, save);
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
  const std::size_t components = std::size_t{options.size} * options.size * 3;
  Run run(options.frames, components);
  for (unsigned sequence = 0; sequence < options.sequences; ++sequence)
    Sequence(*renderer, extraction, work, baseline, options, sequence, frame_index, run, images, sequence == 0);
  // Deterministic validation: the first sequence again, from a new cut, must
  // reproduce every output bit for bit.
  Run repeat(options.frames, components);
  Sequence(*renderer, extraction, work, baseline, options, 0, frame_index, repeat, images, false);
  const bool deterministic = std::equal(repeat.hashes.begin(), repeat.hashes.end(), run.hashes.begin());
  // After every temporal frame, so no progressive render sits between two.
  for (unsigned sequence = 0; sequence < options.sequences; ++sequence)
    Progressive(*renderer, extraction, work, baseline, options, sequence, run, images, sequence == 0);
  constexpr std::array<const char*, 3> kRegions{"none", "albedo", "swept"};

  const double sequences = options.sequences;
  const auto& last_variance = baseline.variance.back();
  const auto& last_reference = baseline.images.back();
  double reference_noise = 0;
  for (const auto& pose : baseline.variance)
    reference_noise += std::accumulate(pose.begin(), pose.end(), 0.0) / pose.size();
  reference_noise /= baseline.variance.size();
  std::ostringstream out;
  out << std::setprecision(9);
  out << "{\"id\":" << Quote(work.id) << ",\"fixture\":" << Quote(work.fixture)
      << ",\"moving\":" << (work.moving ? "true" : "false") << ",\"reference_split_rmse\":";
  Array(out, baseline.split_rmse);
  out << ",\"reference_noise_mse\":" << reference_noise << ",\"deterministic\":"
      << (deterministic ? "true" : "false") << ",\"history_resets_after_cut\":{";
  bool first_reset = true;
  for (const auto& [reason, count] : run.resets) {
    out << (first_reset ? "" : ",") << Quote(reason) << ':' << count;
    first_reset = false;
  }
  out << "},\"lag_region\":{\"kind\":" << Quote(kRegions[static_cast<std::size_t>(work.lag)])
      << ",\"mean_pixels\":" << double(run.lag_pixels) / (sequences * options.frames) << "},\"outputs\":{";
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
    // The lag region after the cut, pooled; per frame, null where it is empty.
    out << "],\"lag_region_mse\":";
    double lag_squared = 0;
    std::uint64_t lag_components = 0;
    for (unsigned f = 1; f < options.frames; ++f) {
      lag_squared += statistics.lag_squared[f];
      lag_components += statistics.lag_components[f];
    }
    if (lag_components == 0)
      out << "null";
    else
      out << lag_squared / lag_components;
    out << ",\"lag_region_mse_by_frame\":[";
    for (unsigned f = 0; f < options.frames; ++f) {
      out << (f ? "," : "");
      if (statistics.lag_components[f] == 0)
        out << "null";
      else
        out << statistics.lag_squared[f] / statistics.lag_components[f];
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
      const auto workloads = Workloads(options);
      for (const auto& work : workloads) {
        // Each reference has its own renderer too: workloads upload the same
        // geometry.
        auto renderer = Renderer(executable, status, detail);
        if (!renderer)
          break;
        const auto baseline = MakeReference(*renderer, work, options, image_root);
        device = baseline.device;
        scenes.push_back(Measure(work, baseline, options, executable, image_root));
      }
      if (scenes.size() == workloads.size()) {
        status = FrameStatus::Pass;
        detail = "measurement completed; no quality or speed threshold asserted";
      }
    } catch (const std::exception& error) {
      status = FrameStatus::Fail;
      detail = error.what();
    }
    std::ostringstream out;
    out << "{\"schema\":\"lotus-svgf-benchmark-v2\",\"status\":"
        << Quote(status == FrameStatus::Pass ? "pass" : status == FrameStatus::Skip ? "skip"
                                                                                    : "fail")
        << ",\"detail\":" << Quote(detail) << ",\"started_unix\":" << started
        << ",\"completed_unix\":" << std::time(nullptr) << ",\"build\":{\"label\":" << Quote(options.label)
        << ",\"compiler\":" << Quote(LOTUS_BENCHMARK_COMPILER)
        << ",\"configuration\":" << Quote(LOTUS_BENCHMARK_CONFIG)
        << ",\"system\":" << Quote(LOTUS_BENCHMARK_SYSTEM) << "},\"device\":" << device
        << ",\"procedure\":{\"integrator\":" << Quote(options.integrator == Integrator::Reference ? "reference" : "wavefront")
        << ",\"reference_integrator\":\"reference\",\"light_sampling\":\"nee-mis\",\"workloads\":[\"svgf-room\",\"moving-occluder-still-receiver\",\"camera-over-low-roughness-surface\",\"deforming-mesh\"]"
        << ",\"width\":" << options.size << ",\"height\":" << options.size << ",\"frames\":" << options.frames
        << ",\"sequences\":" << options.sequences << ",\"fresh_samples_per_frame\":" << options.batch
        << ",\"history_cap\":" << options.cap << ",\"filter_iterations\":" << options.iterations
        << ",\"max_bounces\":" << options.bounces << ",\"sample_index\":" << options.seed
        << ",\"sequence_stride\":" << kSequenceStride << ",\"reference_seed\":" << options.reference_seed
        << ",\"reference_samples_per_half\":" << options.reference_samples << ",\"reference_halves\":2}"
        << ",\"image_directory\":" << Quote(image_root.filename().string())
        << ",\"metric_notes\":{"
           "\"outputs\":\"One render per frame: raw is its fresh batch, temporal the bounded temporal mean (the history), filtered the a-trous composite of that mean. The filter never feeds back into history. progressive is a separate progressive render restarted at the sequence's sample index on every change: the Reference preset's image. A moving workload therefore repeats its random numbers every frame (DES-Q7), which lowers its frame-delta MSE.\","
           "\"mse\":\"RGB squared error against the pose's reference, every pixel, averaged over sequences and then frames. Finite reference noise (reference_noise_mse) is included, not subtracted.\","
           "\"frame_delta_mse\":\"Temporal residual: each frame's error minus the previous frame's at the same pixel, squared. It measures flicker and lag; the cut frame has none.\","
           "\"history_buckets\":\"Squared error by the pixel's history length in batches, frames after the cut pooled: the response after disocclusion or rejection.\","
           "\"last_frame_ensemble\":\"The last frame across independent sequences. Mean MSE is the squared error of their mean; noise is its expected sampling part; excess estimates the squared bias, and can be negative.\","
           "\"history_resets_after_cut\":\"Frames after each sequence's cut rendered without history, by reset reason, summed over sequences: today's invalidation, recorded rather than required.\","
           "\"lag_region\":\"Where the fixture's lag shows: albedo is every pixel whose primary surface has the fixture's lagging material, per frame; swept is every pixel whose reference luminance ranges over more than a tenth of its brightest pose and six times the mean reference noise; none has no region. mean_pixels averages its size over frames.\","
           "\"lag_region_mse\":\"RGB squared error against the pose's reference in the lag region, frames after the cut pooled; by frame, null where the region is empty.\","
           "\"deterministic\":\"The first sequence rendered again from a new cut reproduced every raw, temporal and filtered value bit for bit; progressive is not repeated.\","
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
