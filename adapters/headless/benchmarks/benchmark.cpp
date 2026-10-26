// SPDX-License-Identifier: Apache-2.0
#include "benchmark.hpp"
#include "common/reference.hpp"
#include "common/workloads.hpp"

#include <lotus/extraction.hpp>
#include <lotus/vulkan_backend.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace LotusHeadless {
namespace {

using Clock = std::chrono::steady_clock;
using Lotus::FrameStatus;

// What changes before each measured frame. Static keeps one accumulation;
// the others edit the scene so that every measured frame restarts it.
enum class Motion { Static, Camera, Transform, Deform };

// The motion's offset after the last measured frame, growing linearly from
// the warmup pose so that consecutive frames always differ: a camera pan,
// the edited mesh's translation, or its shear of z by x.
constexpr float kMotionAmplitude = 0.05F;

const char* MotionName(Motion motion) {
  switch (motion) {
  case Motion::Camera: return "camera";
  case Motion::Transform: return "transform";
  case Motion::Deform: return "deform";
  default: return "static";
  }
}

struct Options {
  std::filesystem::path report;
  std::string label;
  std::string workload;
  std::uint32_t frames = 64;
  std::uint32_t warmup = 8;
  std::uint32_t sample_index = 0;
  // The square target's side in pixels.
  std::uint32_t size = kReferenceSize;
  Lotus::Integrator integrator = Lotus::Integrator::Reference;
  // BSDF sampling unless chosen, so that the fixed baseline's earlier
  // measurements stay comparable.
  Lotus::LightSampling light_sampling = Lotus::LightSampling::Bsdf;
  Motion motion = Motion::Static;
  // many-lights-v1's light grid side, 1..16; 0 keeps the fixture's.
  std::uint32_t light_grid = 0;
  Lotus::LightCrossing light_crossing = Lotus::LightCrossing::Hierarchy;
};

// The per-sample variance's batches: their count, their samples each, and
// the first one's sample index, apart from the timed frames' sequence.
constexpr std::uint32_t kVarianceBatches = 16;
constexpr std::uint32_t kVarianceBatch = 16;
constexpr std::uint32_t kVarianceSampleIndex = 2U << 20;

bool Mis(const Options& options) {
  return options.light_sampling == Lotus::LightSampling::Mis;
}

bool Wavefront(const Options& options) {
  return options.integrator == Lotus::Integrator::Wavefront;
}

std::uint32_t WarmupSampleIndex(const Options& options) {
  // A changed sequence key resets accumulation before measurement, even
  // when the requested measured sequence is the old warmup default.
  return options.sample_index == 1 ? 0 : 1;
}

double Milliseconds(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string Quote(std::string_view value) {
  std::ostringstream out;
  out << '"';
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') out << '\\' << c;
    else if (c < 32) out << "\\u" << std::hex << std::setw(4)
                         << std::setfill('0') << static_cast<unsigned>(c) << std::dec;
    else out << c;
  }
  out << '"';
  return out.str();
}

const char* Status(FrameStatus status) {
  return status == FrameStatus::Pass ? "pass" :
         status == FrameStatus::Skip ? "skip" : "fail";
}

void Require(bool condition, const std::string& why) {
  if (!condition) throw std::runtime_error(why);
}

void Check(const Lotus::GpuSceneEvidence& evidence) {
  Require(evidence.status == FrameStatus::Pass, evidence.detail);
  Require(evidence.validation_message_count == 0, "scene update emitted validation messages");
}

void Check(const Lotus::GpuFrameEvidence& evidence) {
  Require(evidence.status == FrameStatus::Pass, evidence.detail);
  Require(evidence.validation_message_count == 0, evidence.validation_detail);
  Require(evidence.ray_query_used && evidence.frames_rendered == 1,
      "measurement did not render exactly one ray-query sample");
}

// Raw samples preserve the population behind the summaries. Nearest-rank p95.
void Series(std::ostream& out, const std::vector<double>& values) {
  if (values.empty()) { out << "null"; return; }
  auto sorted = values;
  std::sort(sorted.begin(), sorted.end());
  const auto n = sorted.size();
  const double median = (sorted[(n - 1) / 2] + sorted[n / 2]) * 0.5;
  const double mean = std::accumulate(values.begin(), values.end(), 0.0) / n;
  out << "{\"mean\":" << mean << ",\"median\":" << median
      << ",\"min\":" << sorted.front() << ",\"max\":" << sorted.back()
      << ",\"p95\":" << sorted[static_cast<std::size_t>(std::ceil(0.95 * n)) - 1]
      << ",\"raw\":[";
  for (std::size_t i = 0; i < n; ++i) out << (i ? "," : "") << values[i];
  out << "]}";
}

void UpdateJson(std::ostream& out, const Lotus::GpuSceneEvidence& e, double wall) {
  out << "{\"cpu_update_wall_ms\":" << wall << ",\"upload_gpu_ms\":";
  if (e.timings.available) out << e.timings.upload_gpu_ms; else out << "null";
  out << ",\"blas_gpu_ms\":";
  if (e.timings.available) out << e.timings.blas_build_gpu_ms; else out << "null";
  out << ",\"tlas_gpu_ms\":";
  if (e.timings.available) out << e.timings.tlas_build_gpu_ms; else out << "null";
  out << ",\"blas_builds\":" << e.stats.blas_builds
      << ",\"blas_updates\":" << e.stats.blas_updates
      << ",\"tlas_builds\":" << e.stats.tlas_builds
      << ",\"tlas_updates\":" << e.stats.tlas_updates << '}';
}

struct Result {
  std::string id;
  FrameStatus status = FrameStatus::Fail;
  std::string detail;
  std::string measurements = "null";
};

Result Measure(std::string_view id, const Options& options,
    const std::filesystem::path& executable) {
  Result result;
  result.id = id;
  try {
    const auto shaders = Lotus::ShadersIn(executable / "shaders");
    FrameStatus setup = FrameStatus::Fail;
    std::string why;
    auto renderer = Lotus::CreateOffscreenRenderer(
        shaders.vertex, shaders.fragment, setup, why, shaders.ray_query);
    if (!renderer) { result.status = setup; result.detail = why; return result; }
    const auto capability = renderer->RayQueryCapability();
    if (!capability.available) {
      result.status = FrameStatus::Skip;
      result.detail = capability.detail;
      return result;
    }
    auto work = MakeWorkload(id, executable / "assets");
    if (options.light_grid != 0) SetLightGrid(work.world, options.light_grid);
    Lotus::SceneExtraction extraction;
    const auto snapshot = work.world.Commit();
    auto draw = Lotus::ExtractDrawSummary(snapshot);
    std::uint64_t triangles = 0;
    for (const auto& [key, mesh] : snapshot.scene->meshes)
      triangles += mesh.geometry->triangles.size();
    const auto initial_plan = extraction.Update(snapshot);
    auto start = Clock::now();
    const auto initial = renderer->UpdateScene(initial_plan);
    const double initial_wall = Milliseconds(start);
    Check(initial);
    Lotus::OffscreenTarget target;
    target.width = target.height = options.size;
    Lotus::PathTracingSettings settings;
    settings.integrator = options.integrator;
    settings.light_sampling = options.light_sampling;
    settings.light_crossing = options.light_crossing;
    settings.sample_index = WarmupSampleIndex(options);
    for (std::uint32_t i = 0; i < options.warmup; ++i)
      Check(renderer->RenderScene(draw, target, 1, settings));
    const auto before = renderer->UpdateScene({});
    Check(before);
    std::vector<double> gpu, wall;
    // The blocking RenderScene's host phases per frame, in FrameHostTimings order.
    std::array<std::vector<double>, 6> host;
    // The wavefront integrator's rays and kernel durations per frame.
    std::vector<double> rays, rounds, camera, intersect, shade, tail;
    // Each motion frame's world edit and Commit, extraction, blocking
    // UpdateScene, its submission's phases and the bytes it staged.
    std::vector<double> author, extract, update, upload_gpu, blas_gpu, tlas_gpu, uploaded;
    const bool moving = options.motion != Motion::Static;
    const auto& edit = snapshot.scene->meshes.at(work.edit_mesh);
    auto previous = before;
    Lotus::GpuFrameEvidence frame;
    settings.sample_index = options.sample_index;
    for (std::uint32_t i = 0; i < options.frames; ++i) {
      if (moving) {
        const float offset = kMotionAmplitude * float(i + 1) / float(options.frames);
        // The deformed copy is prepared outside the timed edit.
        Lotus::MeshGeometry geometry;
        if (options.motion == Motion::Deform) {
          geometry = *edit.geometry;
          for (auto& p : geometry.positions) p[2] += offset * p[0];
        }
        start = Clock::now();
        if (options.motion == Motion::Camera) {
          auto moved = snapshot.camera;
          moved.view[12] += offset;
          work.world.SetCamera(moved);
        } else if (options.motion == Motion::Transform) {
          auto moved = edit.instance;
          moved.world_from_object[12] += offset;
          work.world.SetMeshInstance(work.edit_mesh, moved);
        } else {
          work.world.SetMesh(work.edit_mesh, std::move(geometry), edit.instance);
        }
        const auto posed = work.world.Commit();
        author.push_back(Milliseconds(start));
        start = Clock::now();
        const auto plan = extraction.Update(posed);
        draw = Lotus::ExtractDrawSummary(posed);
        extract.push_back(Milliseconds(start));
        Require(plan.Empty() == (options.motion == Motion::Camera),
            "camera motion produced scene work, or object motion none");
        start = Clock::now();
        const auto evidence = renderer->UpdateScene(plan);
        update.push_back(Milliseconds(start));
        Check(evidence);
        const auto& now = evidence.stats;
        const auto& was = previous.stats;
        Require(now.blas_builds == was.blas_builds && now.tlas_builds == was.tlas_builds &&
            now.blas_updates == was.blas_updates + (options.motion == Motion::Deform ? 1 : 0) &&
            now.tlas_updates == was.tlas_updates + (options.motion != Motion::Camera ? 1 : 0),
            "motion did not refit exactly the edited BLAS and the TLAS");
        uploaded.push_back(double(now.uploaded_bytes - was.uploaded_bytes));
        if (evidence.timings.available) {
          upload_gpu.push_back(evidence.timings.upload_gpu_ms);
          blas_gpu.push_back(evidence.timings.blas_build_gpu_ms);
          tlas_gpu.push_back(evidence.timings.tlas_build_gpu_ms);
        }
        previous = evidence;
      }
      start = Clock::now();
      frame = renderer->RenderScene(draw, target, 1, settings);
      wall.push_back(Milliseconds(start));
      Check(frame);
      const auto& phases = frame.host_timings;
      const std::array<double, 6> split{phases.prepare_ms, phases.record_ms,
          phases.submit_ms, phases.wait_ms, phases.readback_ms, phases.results_ms};
      Require(std::all_of(split.begin(), split.end(),
                  [](double ms) { return std::isfinite(ms) && ms >= 0; }) &&
          std::accumulate(split.begin(), split.end(), 0.0) <= wall.back(),
          "host phases are invalid or exceed the frame's wall time");
      for (std::size_t phase = 0; phase < split.size(); ++phase)
        host[phase].push_back(split[phase]);
      Require(frame.samples_per_pixel == (moving ? 1 : i + 1),
          "accumulation sample count mismatch");
      if (frame.primary_ray_timestamp_available) {
        Require(std::isfinite(frame.primary_ray_gpu_ms) && frame.primary_ray_gpu_ms > 0,
            "invalid GPU timestamp duration");
        gpu.push_back(frame.primary_ray_gpu_ms);
      }
      if (Wavefront(options)) {
        Require(!frame.wavefront_path_counts.empty(), "the wavefront frame queued no rays");
        rays.push_back(std::accumulate(frame.wavefront_path_counts.begin(),
            frame.wavefront_path_counts.end(), 0.0));
        rounds.push_back(frame.wavefront_rounds);
        const auto& timings = frame.wavefront_timings;
        Require(timings.available == frame.primary_ray_timestamp_available,
            "inconsistent wavefront timestamp availability");
        if (timings.available) {
          camera.push_back(timings.camera_gpu_ms);
          intersect.push_back(timings.intersect_gpu_ms);
          shade.push_back(timings.shade_gpu_ms);
          tail.push_back(timings.tail_gpu_ms);
        }
      }
    }
    Require(gpu.empty() || gpu.size() == options.frames, "inconsistent timestamp availability");

    // Per-sample variance from fresh accumulations of kVarianceBatch samples,
    // their sequences apart from the timed frames': the mean over pixels and
    // channels of each value's variance across the batches, times the batch
    // size. The time to reach a given variance is proportional to it times
    // the GPU time per sample. A moving scene has no single pose to measure.
    const std::size_t pixels = std::size_t{options.size} * options.size;
    std::vector<double> sum(pixels * 3), squares(pixels * 3);
    for (std::uint32_t batch = 0; !moving && batch < kVarianceBatches; ++batch) {
      settings.sample_index = kVarianceSampleIndex + batch * kVarianceBatch;
      const auto accumulated = renderer->RenderScene(draw, target, kVarianceBatch, settings);
      Require(accumulated.status == FrameStatus::Pass, accumulated.detail);
      Require(accumulated.validation_message_count == 0, accumulated.validation_detail);
      Require(accumulated.samples_per_pixel == kVarianceBatch &&
          accumulated.color.payload.size() == pixels * 16,
          "unexpected variance batch accumulation or image");
      for (std::size_t p = 0; p < pixels; ++p) {
        std::array<float, 4> rgba;
        std::memcpy(rgba.data(), accumulated.color.payload.data() + p * 16, 16);
        for (std::size_t c = 0; c < 3; ++c) {
          Require(std::isfinite(rgba[c]), "non-finite variance batch");
          sum[p * 3 + c] += rgba[c];
          squares[p * 3 + c] += double{rgba[c]} * rgba[c];
        }
      }
    }
    double variance = 0;
    for (std::size_t i = 0; i < sum.size(); ++i) {
      const double mean = sum[i] / kVarianceBatches;
      variance += std::max(0.0,
          (squares[i] - kVarianceBatches * mean * mean) / (kVarianceBatches - 1));
    }
    variance *= double{kVarianceBatch} / static_cast<double>(sum.size());

    const auto after = renderer->UpdateScene({});
    Check(after);
    Require(after.stats.memory == previous.stats.memory &&
        after.stats.upload_submissions == previous.stats.upload_submissions &&
        after.stats.blas_builds == previous.stats.blas_builds &&
        after.stats.blas_updates == previous.stats.blas_updates &&
        after.stats.tlas_builds == previous.stats.tlas_builds &&
        after.stats.tlas_updates == previous.stats.tlas_updates,
        "steady rendering changed scene allocation/upload/acceleration counters");

    RgbImage image{options.size, options.size, {}};
    Require(frame.color.pixel_format == "rgba32-sfloat" &&
        frame.color.payload.size() == pixels * 16,
        "unexpected benchmark image format or size");
    double radiance_sum = 0;
    std::uint64_t hits = 0;
    for (std::size_t p = 0; p < frame.color.payload.size(); p += 16) {
      std::array<float, 4> rgba;
      std::memcpy(rgba.data(), frame.color.payload.data() + p, 16);
      for (float c : rgba) Require(std::isfinite(c), "non-finite benchmark image");
      radiance_sum += rgba[0] + rgba[1] + rgba[2];
      hits += rgba[3] > 0;
      image.values.insert(image.values.end(), rgba.begin(), rgba.begin() + 3);
    }
    Require(hits > 0 && radiance_sum > 0, "benchmark scene was invisible or black");
    const auto image_path = options.report.parent_path() /
        (options.report.stem().string() + "-" + std::string(id) + ".pfm");
    Require(WritePfm(image_path, image).empty(), "cannot write benchmark image");
    // This also verifies camera/transport compatibility with the established
    // reference, rather than treating a fast but incorrect image as a baseline.
    // The reference exists only at its own size.
    if (id == "cornell-v1" && options.size == kReferenceSize && !moving) {
      Reference reference;
      reference.samples = 1024;
      Require(ReadPfm(executable / "reference/cornell-box-mean.pfm", reference.mean).empty(),
          "cannot read reference mean");
      Require(ReadPfm(executable / "reference/cornell-box-variance.pfm", reference.variance).empty(),
          "cannot read reference variance");
      const auto comparison = Compare(image, options.frames, reference);
      Require(comparison.failure.empty(), comparison.failure);
    }

    // Time complete unchanged Commit -> extraction -> backend calls in batches
    // to keep the clock's overhead below the tiny per-call cost.
    // The single edits below measure the static pose only.
    std::vector<double> unchanged;
    constexpr std::uint32_t iterations = 1000;
    for (int batch = 0; !moving && batch < 16; ++batch) {
      start = Clock::now();
      for (std::uint32_t i = 0; i < iterations; ++i) {
        const auto plan = extraction.Update(work.world.Commit());
        Require(plan.Empty(), "unchanged scene produced work");
        const auto evidence = renderer->UpdateScene(plan);
        Check(evidence);
        Require(!evidence.timings.available && evidence.stats.memory == after.stats.memory &&
            evidence.stats.upload_submissions == after.stats.upload_submissions &&
            evidence.stats.blas_builds == after.stats.blas_builds &&
            evidence.stats.blas_updates == after.stats.blas_updates &&
            evidence.stats.tlas_builds == after.stats.tlas_builds &&
            evidence.stats.tlas_updates == after.stats.tlas_updates,
            "unchanged update submitted work or changed memory");
      }
      unchanged.push_back(Milliseconds(start) / iterations);
    }
    Lotus::GpuSceneEvidence point, transform;
    double point_wall = 0, transform_wall = 0;
    if (!moving) {
      auto geometry = *edit.geometry;
      for (auto& p : geometry.positions) p[2] += 0.01F * p[0];
      work.world.SetMesh(work.edit_mesh, std::move(geometry), edit.instance);
      auto plan = extraction.Update(work.world.Commit());
      start = Clock::now();
      point = renderer->UpdateScene(plan);
      point_wall = Milliseconds(start);
      Check(point);
      Require(point.stats.blas_updates == initial.stats.blas_updates + 1 &&
          point.stats.blas_builds == initial.stats.blas_builds &&
          point.stats.tlas_updates == initial.stats.tlas_updates + 1,
          "point edit did not refit BLAS and dependent TLAS");
      auto moved = edit.instance;
      moved.world_from_object[12] += 0.01F;
      work.world.SetMeshInstance(work.edit_mesh, moved);
      plan = extraction.Update(work.world.Commit());
      start = Clock::now();
      transform = renderer->UpdateScene(plan);
      transform_wall = Milliseconds(start);
      Check(transform);
      Require(transform.stats.tlas_updates == point.stats.tlas_updates + 1 &&
          transform.stats.blas_updates == point.stats.blas_updates &&
          transform.stats.blas_builds == point.stats.blas_builds,
          "transform edit did not refit only TLAS");
    }

    std::ostringstream out;
    out << std::setprecision(10) << "{\"device\":{\"name\":" << Quote(frame.device_name)
        << ",\"api_version\":" << Quote(frame.api_version)
        << ",\"driver_version_raw\":" << Quote(frame.driver_version)
        << ",\"vendor_id\":" << frame.vendor_id << ",\"device_id\":" << frame.device_id
        << "},\"validation\":{\"enabled\":" << (frame.validation_available ? "true" : "false")
        << ",\"synchronization\":" << (frame.synchronization_validation_available ? "true" : "false")
        << ",\"synchronization_detail\":" << Quote(frame.synchronization_validation_detail)
        << ",\"messages\":" << frame.validation_message_count << "},\"scene\":{\"geometries\":"
        << initial.stats.resident_geometries << ",\"unique_triangles\":" << triangles
        << ",\"instances\":" << initial.stats.instance_count << ",\"textures\":"
        << initial.stats.resident_textures << ",\"texture_bytes\":" << initial.stats.texture_bytes
        << ",\"emissive_triangles\":" << initial.stats.emissive_triangles
        << ",\"analytic_lights\":" << initial.stats.analytic_lights
        << ",\"light_hierarchy_bytes\":" << initial.stats.light_hierarchy_bytes
        << ",\"environment_sampled\":" << (initial.stats.environment_sampled ? "true" : "false")
        << "},\"gpu_frame_ms\":";
    Series(out, gpu);
    out << ",\"per_sample_variance\":";
    if (moving) out << "null"; else out << variance;
    out << ",\"variance_gpu_ms\":";
    if (gpu.empty() || moving) {
      out << "null";
    } else {
      auto sorted = gpu;
      std::sort(sorted.begin(), sorted.end());
      out << variance * (sorted[(sorted.size() - 1) / 2] + sorted[sorted.size() / 2]) * 0.5;
    }
    out << ",\"render_wall_ms\":"; Series(out, wall);
    out << ",\"render_host_ms\":{";
    constexpr const char* kHostPhases[] = {
        "prepare", "record", "submit", "wait", "readback", "results"};
    for (std::size_t phase = 0; phase < host.size(); ++phase) {
      out << (phase ? "," : "") << Quote(kHostPhases[phase]) << ':';
      Series(out, host[phase]);
    }
    out << '}';
    out << ",\"pixel_samples_per_second_gpu\":";
    if (gpu.empty()) out << "null";
    else out << double(pixels) * options.frames * 1000 /
        std::accumulate(gpu.begin(), gpu.end(), 0.0);
    out << ",\"pixel_samples_per_second_wall\":"
        << double(pixels) * options.frames * 1000 /
           std::accumulate(wall.begin(), wall.end(), 0.0)
        << ",\"unchanged_cpu_ms\":";
    Series(out, unchanged);
    out << ",\"wavefront\":";
    if (!Wavefront(options)) {
      out << "null";
    } else {
      const double total_rays = std::accumulate(rays.begin(), rays.end(), 0.0);
      out << "{\"rays\":";
      Series(out, rays);
      out << ",\"rays_per_path\":"
          << total_rays / (double(pixels) * options.frames)
          << ",\"rays_per_second_gpu\":";
      if (gpu.empty()) out << "null";
      else out << total_rays * 1000 / std::accumulate(gpu.begin(), gpu.end(), 0.0);
      out << ",\"rounds\":"; Series(out, rounds);
      out << ",\"camera_gpu_ms\":"; Series(out, camera);
      out << ",\"intersect_gpu_ms\":"; Series(out, intersect);
      out << ",\"shade_gpu_ms\":"; Series(out, shade);
      out << ",\"tail_gpu_ms\":"; Series(out, tail);
      out << '}';
    }
    out << ",\"scene_pool_reserved_bytes\":" << after.stats.memory.reserved_bytes
        << ",\"scene_pool_used_bytes\":" << after.stats.memory.used_bytes
        << ",\"initial_update\":";
    UpdateJson(out, initial, initial_wall);
    if (moving) {
      out << ",\"point_refit\":null,\"transform_refit\":null,\"motion\":{\"author_cpu_ms\":";
      Series(out, author);
      out << ",\"extract_cpu_ms\":"; Series(out, extract);
      out << ",\"update_wall_ms\":"; Series(out, update);
      out << ",\"upload_gpu_ms\":"; Series(out, upload_gpu);
      out << ",\"blas_gpu_ms\":"; Series(out, blas_gpu);
      out << ",\"tlas_gpu_ms\":"; Series(out, tlas_gpu);
      out << ",\"uploaded_bytes\":"; Series(out, uploaded);
      out << ",\"blas_updates\":" << after.stats.blas_updates - before.stats.blas_updates
          << ",\"tlas_updates\":" << after.stats.tlas_updates - before.stats.tlas_updates
          << ",\"upload_submissions\":"
          << after.stats.upload_submissions - before.stats.upload_submissions
          << ",\"scene_pool_reserved_bytes_before\":" << before.stats.memory.reserved_bytes
          << ",\"scene_pool_used_bytes_before\":" << before.stats.memory.used_bytes << '}';
    } else {
      out << ",\"point_refit\":"; UpdateJson(out, point, point_wall);
      out << ",\"transform_refit\":"; UpdateJson(out, transform, transform_wall);
      out << ",\"motion\":null";
    }
    out << ",\"image\":" << Quote(image_path.filename().string())
        << ",\"mean_rgb\":" << radiance_sum / (double(pixels) * 3)
        << ",\"hit_pixels\":" << hits << '}';
    result.measurements = out.str();
    result.status = FrameStatus::Pass;
  } catch (const std::exception& error) {
    result.detail = error.what();
  }
  return result;
}

} // namespace

int BenchmarkMain(int argc, char** argv) {
  Options options;
  if (argc < 3 || std::string_view(argv[2]).starts_with("--")) {
    std::cerr << "usage: lotus-headless --benchmark <json-path> [--benchmark-label <identity>]\n"
                 "       [--benchmark-frames <1..4096>] [--benchmark-warmup <1..4096>]\n"
                 "       [--benchmark-integrator <reference|wavefront>] [--benchmark-size <16..4096>]\n"
                 "       [--benchmark-light-sampling <bsdf|mis>] [--benchmark-workload <id>]\n"
                 "       [--benchmark-sample-index <start below 2097152>]\n"
                 "       [--benchmark-motion <static|camera|transform|deform>]\n"
                 "       [--benchmark-light-grid <1..16>, with many-lights-v1]\n"
                 "       [--benchmark-light-crossing <hierarchy|linear>]\n";
    return 2;
  }
  options.report = argv[2];
  for (int i = 3; i < argc; ++i) {
    const std::string_view option = argv[i];
    if (i + 1 == argc) { std::cerr << "missing value for " << option << '\n'; return 2; }
    const std::string_view value = argv[++i];
    if (option == "--benchmark-label") options.label = value;
    else if (option == "--benchmark-workload") {
      if (!KnownWorkload(value)) {
        std::cerr << "invalid workload: " << value << '\n'; return 2;
      }
      options.workload = value;
    }
    else if (option == "--benchmark-integrator") {
      if (value == "reference") options.integrator = Lotus::Integrator::Reference;
      else if (value == "wavefront") options.integrator = Lotus::Integrator::Wavefront;
      else { std::cerr << "invalid integrator: " << value << '\n'; return 2; }
    }
    else if (option == "--benchmark-light-sampling") {
      if (value == "bsdf") options.light_sampling = Lotus::LightSampling::Bsdf;
      else if (value == "mis") options.light_sampling = Lotus::LightSampling::Mis;
      else { std::cerr << "invalid light sampling: " << value << '\n'; return 2; }
    }
    else if (option == "--benchmark-light-crossing") {
      if (value == "hierarchy") options.light_crossing = Lotus::LightCrossing::Hierarchy;
      else if (value == "linear") options.light_crossing = Lotus::LightCrossing::Linear;
      else { std::cerr << "invalid light crossing: " << value << '\n'; return 2; }
    }
    else if (option == "--benchmark-motion") {
      if (value == "static") options.motion = Motion::Static;
      else if (value == "camera") options.motion = Motion::Camera;
      else if (value == "transform") options.motion = Motion::Transform;
      else if (value == "deform") options.motion = Motion::Deform;
      else { std::cerr << "invalid motion: " << value << '\n'; return 2; }
    }
    else if (option == "--benchmark-size") {
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), options.size);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
          options.size < 16 || options.size > 4096) {
        std::cerr << "invalid size: " << value << '\n'; return 2;
      }
    }
    else if (option == "--benchmark-light-grid") {
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), options.light_grid);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
          options.light_grid == 0 || options.light_grid > kManyLightGrid) {
        std::cerr << "invalid light grid: " << value << '\n'; return 2;
      }
    }
    else if (option == "--benchmark-sample-index") {
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), options.sample_index);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        std::cerr << "invalid sample index: " << value << '\n'; return 2;
      }
    }
    else if (option == "--benchmark-frames" || option == "--benchmark-warmup") {
      std::uint32_t count = 0;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), count);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
          count == 0 || count > 4096) {
        std::cerr << "invalid count for " << option << '\n'; return 2;
      }
      (option == "--benchmark-frames" ? options.frames : options.warmup) = count;
    } else { std::cerr << "unknown benchmark option: " << option << '\n'; return 2; }
  }
  if (options.light_grid != 0 && options.workload != "many-lights-v1") {
    std::cerr << "--benchmark-light-grid requires --benchmark-workload many-lights-v1\n";
    return 2;
  }
  // Keep timed samples below, and disjoint from, the variance batches.
  if (options.sample_index >= kVarianceSampleIndex ||
      options.frames > kVarianceSampleIndex - options.sample_index) {
    std::cerr << "measured sample range must end before variance sample index "
              << kVarianceSampleIndex << '\n';
    return 2;
  }
  try {
    if (!options.report.parent_path().empty())
      std::filesystem::create_directories(options.report.parent_path());
    const auto started = std::time(nullptr);
    const auto executable = std::filesystem::absolute(argv[0]).parent_path();
    std::vector<Result> results;
    FrameStatus status = FrameStatus::Pass;
    for (const auto id : kWorkloadIds) {
      if (options.workload.empty() ? id == "many-lights-v1" : id != options.workload) continue;
      auto result = Measure(id, options, executable);
      if (result.status == FrameStatus::Fail) status = FrameStatus::Fail;
      else if (result.status == FrameStatus::Skip && status == FrameStatus::Pass)
        status = FrameStatus::Skip;
      std::cout << id << ": " << Status(result.status) << ' ' << result.detail << '\n';
      results.push_back(std::move(result));
    }
    // Keep this schema distinct from OpenStrata's renderer correctness report.
    std::ofstream out(options.report, std::ios::trunc);
    out << "{\"schema\":\"lotus-benchmark-v1\",\"status\":" << Quote(Status(status))
        << ",\"started_unix\":" << started << ",\"completed_unix\":" << std::time(nullptr)
        << ",\"build\":{\"label\":" << Quote(options.label)
        << ",\"compiler\":" << Quote(LOTUS_BENCHMARK_COMPILER)
        << ",\"configuration\":" << Quote(LOTUS_BENCHMARK_CONFIG)
        << ",\"system\":" << Quote(LOTUS_BENCHMARK_SYSTEM)
        << ",\"cpu_logical_processors\":" << std::thread::hardware_concurrency()
        << "},\"procedure\":{\"integrator\":"
        << Quote(Wavefront(options) ? "wavefront" : "reference")
        << ",\"light_sampling\":" << Quote(Mis(options) ? "mis" : "bsdf")
        << ",\"light_crossing\":"
        << Quote(options.light_crossing == Lotus::LightCrossing::Linear ? "linear" : "hierarchy")
        << ",\"width\":" << options.size << ",\"height\":" << options.size
        << ",\"cornell_reference_compared\":"
        << (options.size == kReferenceSize && options.motion == Motion::Static &&
            (options.workload.empty() || options.workload == "cornell-v1") ? "true" : "false")
        << ",\"motion\":" << Quote(MotionName(options.motion))
        << ",\"light_grid\":";
    if (options.light_grid == 0) out << "null"; else out << options.light_grid;
    out << ",\"motion_amplitude\":";
    if (options.motion == Motion::Static) out << "null"; else out << kMotionAmplitude;
    out << ",\"sample_index\":" << options.sample_index << ",\"max_bounces\":64,"
           "\"samples_per_frame\":1,\"measured_frames\":" << options.frames
        << ",\"warmup_frames\":" << options.warmup
        << ",\"variance_batches\":" << kVarianceBatches
        << ",\"variance_batch_samples\":" << kVarianceBatch
        << ",\"variance_sample_index\":" << kVarianceSampleIndex
        << ",\"warmup_sample_index\":" << WarmupSampleIndex(options)
        << ",\"unchanged_batches\":16,\"unchanged_iterations\":1000,"
           "\"fresh_renderer_per_scene\":true,\"update_measurements_per_kind\":1},"
           "\"metric_notes\":{"
           "\"gpu_frame_ms\":\"One sample pass, including clears, excluding readback; one call per sample.\","
           "\"render_wall_ms\":\"Blocking RenderScene, including submit, fence wait, readback and product copies.\","
           "\"render_host_ms\":\"The same calls' host phases: target/descriptor preparation and constant writes, command recording, "
           "fence reset and queue submission, fence wait, readback invalidation and product copies, and results/evidence assembly. "
           "Their sum excludes RenderScene's argument checks; enabled validation layers add to the phases they intercept.\","
           "\"pixel_samples_per_second\":\"Full-frame pixel samples divided by total measured GPU or wall time, including camera misses.\","
           "\"per_sample_variance\":\"Mean over pixels and channels of each value's variance across the variance batches, times their samples each.\","
           "\"variance_gpu_ms\":\"Per-sample variance times the median GPU frame time: proportional to the time to equal quality; null without timestamps.\","
           "\"unchanged_cpu_ms\":\"Commit, extraction and empty UpdateScene plus invariant checks; batch wall time / 1000.\","
           "\"scene_pool_bytes\":\"Scene pools include device-local and host-visible memory; exclude renderer targets and constants.\","
           "\"update_ms\":\"One initial upload/build, one point refit and one transform refit; CPU wall covers blocking UpdateScene only. Counters are cumulative.\","
           "\"motion\":\"Non-static motion only: before every measured frame, a camera pan, the edited mesh's translation or its shear of z by x, "
           "growing linearly to motion_amplitude, restarts the accumulation (one sample per frame). Author covers the world edit and Commit, "
           "excluding the deformed copy's preparation; extract covers SceneExtraction and the draw summary; update is blocking UpdateScene wall; "
           "GPU phases come from its timestamps; counters are totals over the measured frames. Variance, the Cornell comparison, unchanged "
           "batches and single refits are static only and null.\","
           "\"light_grid\":\"When set, many-lights-v1's 16 x 16 sphere lights are replaced by a square grid of this side "
           "over the same footprint, radius and total power; null is the fixture.\","
           "\"light_crossing\":\"How rays find the analytic lights they cross: the GPU scene's hierarchy over the bounded "
           "lights' bounds, or every light in order (the evaluation baseline). light_hierarchy_bytes counts the hierarchy's "
           "nodes and distant-light indices.\","
           "\"unavailable_gpu_timestamps\":\"Null GPU durations/rates indicate no timestamp support; wall measurements remain available.\","
           "\"wavefront\":\"Wavefront integrator only: rays traced per frame from its queue counts, "
           "rays per camera path, rays per GPU second, bounces run as rounds and per-kernel GPU durations; null for the reference integrator.\"},"
           "\"unavailable\":{";
    // The wavefront integrator counts its rays; the reference shader does not.
    if (!Wavefront(options))
      out << "\"rays_per_second\":null,\"average_path_depth\":null,";
    out << "\"total_vram_bytes\":null},"
           "\"unavailable_reasons\":{";
    if (!Wavefront(options))
      out << "\"rays_per_second\":\"Reference shader has no ray counter; pixel samples are not ray counts.\","
             "\"average_path_depth\":\"Reference shader has no path-depth counter.\",";
    out << "\"total_vram_bytes\":\"Scene pool statistics are not total renderer allocations or process VRAM; "
           "lotus-headless --benchmark-residency reports both.\"},"
           "\"scenes\":[";
    for (std::size_t i = 0; i < results.size(); ++i) {
      const auto& result = results[i];
      out << (i ? "," : "") << "{\"id\":" << Quote(result.id) << ",\"status\":"
          << Quote(Status(result.status)) << ",\"detail\":" << Quote(result.detail)
          << ",\"measurements\":" << result.measurements << '}';
    }
    out << "]}\n";
    out.close();
    Require(!out.fail(), "cannot write benchmark report: " + options.report.string());
    return status == FrameStatus::Pass ? 0 : status == FrameStatus::Skip ? 77 : 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

} // namespace LotusHeadless
