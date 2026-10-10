// SPDX-License-Identifier: Apache-2.0
#include "benchmark.hpp"
#include "reference.hpp"

#include <lotus/extraction.hpp>
#include <lotus/vulkan_backend.hpp>

#include <algorithm>
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

struct Options {
  std::filesystem::path report;
  std::string label;
  std::uint32_t frames = 64;
  std::uint32_t warmup = 8;
  Lotus::Integrator integrator = Lotus::Integrator::Reference;
};

bool Wavefront(const Options& options) {
  return options.integrator == Lotus::Integrator::Wavefront;
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

Lotus::MeshGeometry Grid(std::uint32_t divisions, float extent, bool uv) {
  Lotus::MeshGeometry mesh;
  for (std::uint32_t y = 0; y <= divisions; ++y)
    for (std::uint32_t x = 0; x <= divisions; ++x)
      mesh.positions.push_back({extent * (2.0F * x / divisions - 1.0F),
          extent * (2.0F * y / divisions - 1.0F),
          0.002F * static_cast<float>((x * 17 + y * 13) % 11)});
  for (std::uint32_t y = 0; y < divisions; ++y) {
    for (std::uint32_t x = 0; x < divisions; ++x) {
      const auto a = y * (divisions + 1) + x;
      const auto b = a + 1;
      const auto d = a + divisions + 1;
      const auto c = d + 1;
      mesh.triangles.push_back({a, b, c});
      mesh.triangles.push_back({a, c, d});
      mesh.source_faces.insert(mesh.source_faces.end(), 2, y * divisions + x);
    }
  }
  if (uv) {
    auto& coordinates = mesh.texcoords["st"];
    for (const auto& triangle : mesh.triangles)
      for (const auto vertex : triangle) {
        const auto& p = mesh.positions[vertex];
        coordinates.push_back({(p[0] / extent + 1) * 0.5F,
            (p[1] / extent + 1) * 0.5F});
      }
  }
  return mesh;
}

struct Workload {
  Lotus::RenderWorld world;
  std::string edit_mesh;
};

Workload MakeWorkload(std::string_view id) {
  Workload work;
  SetCornellBox(work.world);
  if (id == "cornell-v1") {
    work.edit_mesh = "/cornell/back";
    return work;
  }
  const auto old = work.world.Commit();
  for (const auto& [key, mesh] : old.scene->meshes) work.world.RemoveMesh(key);
  for (const auto& [key, material] : old.scene->materials) work.world.RemoveMaterial(key);
  work.world.SetEnvironment({1.0F, 1.0F, 1.0F});
  if (id == "textured-v1") {
    // Sixteen independent textured patches, 32x32 quads each, filling the view.
    for (std::uint32_t i = 0; i < 16; ++i) {
      const std::string key = "/tiles/" + std::to_string(i);
      Lotus::MeshInstance placed;
      placed.world_from_object[12] = -0.9375F + 0.625F * (i % 4);
      placed.world_from_object[13] = -0.9375F + 0.625F * (i / 4);
      work.world.SetMesh(key, Grid(32, 0.3125F, true), placed);
      Lotus::Texture texture;
      texture.width = texture.height = 256;
      texture.format = Lotus::TextureFormat::Rgba8Srgb;
      for (std::uint32_t y = 0; y < 256; ++y)
        for (std::uint32_t x = 0; x < 256; ++x) {
          const std::uint8_t checker = ((x / 16 + y / 16) % 2) ? 220 : 80;
          texture.texels.insert(texture.texels.end(), {checker,
              static_cast<std::uint8_t>(40 + i * 12),
              static_cast<std::uint8_t>(60 + x / 2), 255});
        }
      work.world.SetTexture(key, std::move(texture));
      Lotus::Material material;
      material.texcoords = "st";
      material.base_color_texture = Lotus::TextureInput{};
      material.base_color_texture->texture = key;
      material.base_color_texture->wrap_s = material.base_color_texture->wrap_t =
          Lotus::TextureWrap::Repeat;
      material.roughness = 0.25F + 0.04F * i;
      work.world.SetMaterial(key, material);
      work.world.BindMaterial(key, key);
    }
    work.edit_mesh = "/tiles/0";
  } else if (id == "instanced-v1") {
    Lotus::MeshInstance placed;
    placed.instancer_transforms.emplace();
    for (std::uint32_t y = 0; y < 64; ++y)
      for (std::uint32_t x = 0; x < 64; ++x) {
        auto transform = Lotus::IdentityMatrix();
        transform[12] = -1.23046875F + 0.0390625F * x;
        transform[13] = -1.23046875F + 0.0390625F * y;
        placed.instancer_transforms->push_back(transform);
      }
    work.edit_mesh = "/prototype";
    work.world.SetMesh(work.edit_mesh, Grid(1, 0.01953125F, false), placed);
  } else {
    work.edit_mesh = "/grid";
    work.world.SetMesh(work.edit_mesh, Grid(512, 1.25F, false), {});
  }
  return work;
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

Result Measure(const std::string& id, const Options& options,
    const std::filesystem::path& executable) {
  Result result;
  result.id = id;
  try {
    const auto shaders = executable / "shaders";
    FrameStatus setup = FrameStatus::Fail;
    std::string why;
    auto renderer = Lotus::CreateOffscreenRenderer(
        (shaders / "triangle.vert.spv").string(), (shaders / "triangle.frag.spv").string(),
        setup, why, {(shaders / "path_trace.vert.spv").string(),
                       (shaders / "path_trace.frag.spv").string(),
                       (shaders / "wavefront.comp.spv").string()});
    if (!renderer) { result.status = setup; result.detail = why; return result; }
    const auto capability = renderer->RayQueryCapability();
    if (!capability.available) {
      result.status = FrameStatus::Skip;
      result.detail = capability.detail;
      return result;
    }
    auto work = MakeWorkload(id);
    Lotus::SceneExtraction extraction;
    const auto snapshot = work.world.Commit();
    const auto draw = Lotus::ExtractDrawSummary(snapshot);
    std::uint64_t triangles = 0;
    for (const auto& [key, mesh] : snapshot.scene->meshes)
      triangles += mesh.geometry->triangles.size();
    const auto initial_plan = extraction.Update(snapshot);
    auto start = Clock::now();
    const auto initial = renderer->UpdateScene(initial_plan);
    const double initial_wall = Milliseconds(start);
    Check(initial);
    Lotus::OffscreenTarget target;
    target.width = target.height = kReferenceSize;
    Lotus::PathTracingSettings settings;
    settings.integrator = options.integrator;
    settings.sample_index = 1; // Warmup has its own sequence, then reset to seed 0.
    for (std::uint32_t i = 0; i < options.warmup; ++i)
      Check(renderer->RenderScene(draw, target, 1, settings));
    const auto before = renderer->UpdateScene({});
    Check(before);
    std::vector<double> gpu, wall;
    // The wavefront integrator's rays and kernel durations per frame.
    std::vector<double> rays, generate, intersect, shade, accumulate;
    Lotus::GpuFrameEvidence frame;
    settings.sample_index = 0;
    for (std::uint32_t i = 0; i < options.frames; ++i) {
      start = Clock::now();
      frame = renderer->RenderScene(draw, target, 1, settings);
      wall.push_back(Milliseconds(start));
      Check(frame);
      Require(frame.samples_per_pixel == i + 1, "accumulation sample count mismatch");
      if (frame.primary_ray_timestamp_available) {
        Require(std::isfinite(frame.primary_ray_gpu_ms) && frame.primary_ray_gpu_ms > 0,
            "invalid GPU timestamp duration");
        gpu.push_back(frame.primary_ray_gpu_ms);
      }
      if (Wavefront(options)) {
        Require(!frame.wavefront_path_counts.empty(), "the wavefront frame queued no rays");
        rays.push_back(std::accumulate(frame.wavefront_path_counts.begin(),
            frame.wavefront_path_counts.end(), 0.0));
        const auto& timings = frame.wavefront_timings;
        Require(timings.available == frame.primary_ray_timestamp_available,
            "inconsistent wavefront timestamp availability");
        if (timings.available) {
          generate.push_back(timings.generate_gpu_ms);
          intersect.push_back(timings.intersect_gpu_ms);
          shade.push_back(timings.shade_gpu_ms);
          accumulate.push_back(timings.accumulate_gpu_ms);
        }
      }
    }
    Require(gpu.empty() || gpu.size() == options.frames, "inconsistent timestamp availability");
    const auto after = renderer->UpdateScene({});
    Check(after);
    Require(after.stats.memory == before.stats.memory &&
        after.stats.upload_submissions == before.stats.upload_submissions &&
        after.stats.blas_builds == before.stats.blas_builds &&
        after.stats.blas_updates == before.stats.blas_updates &&
        after.stats.tlas_builds == before.stats.tlas_builds &&
        after.stats.tlas_updates == before.stats.tlas_updates,
        "steady rendering changed scene allocation/upload/acceleration counters");

    RgbImage image{kReferenceSize, kReferenceSize, {}};
    Require(frame.color.pixel_format == "rgba32-sfloat" &&
        frame.color.payload.size() == std::size_t{kReferenceSize} * kReferenceSize * 16,
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
        (options.report.stem().string() + "-" + id + ".pfm");
    Require(WritePfm(image_path, image).empty(), "cannot write benchmark image");
    // This also verifies camera/transport compatibility with the established
    // reference, rather than treating a fast but incorrect image as a baseline.
    if (id == "cornell-v1") {
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
    std::vector<double> unchanged;
    constexpr std::uint32_t iterations = 1000;
    for (int batch = 0; batch < 16; ++batch) {
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
    const auto& edit = snapshot.scene->meshes.at(work.edit_mesh);
    auto geometry = *edit.geometry;
    for (auto& p : geometry.positions) p[2] += 0.01F * p[0];
    work.world.SetMesh(work.edit_mesh, std::move(geometry), edit.instance);
    auto plan = extraction.Update(work.world.Commit());
    start = Clock::now();
    const auto point = renderer->UpdateScene(plan);
    const double point_wall = Milliseconds(start);
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
    const auto transform = renderer->UpdateScene(plan);
    const double transform_wall = Milliseconds(start);
    Check(transform);
    Require(transform.stats.tlas_updates == point.stats.tlas_updates + 1 &&
        transform.stats.blas_updates == point.stats.blas_updates &&
        transform.stats.blas_builds == point.stats.blas_builds,
        "transform edit did not refit only TLAS");

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
        << "},\"gpu_frame_ms\":";
    Series(out, gpu);
    out << ",\"render_wall_ms\":"; Series(out, wall);
    out << ",\"pixel_samples_per_second_gpu\":";
    if (gpu.empty()) out << "null";
    else out << double{kReferenceSize * kReferenceSize} * options.frames * 1000 /
        std::accumulate(gpu.begin(), gpu.end(), 0.0);
    out << ",\"pixel_samples_per_second_wall\":"
        << double{kReferenceSize * kReferenceSize} * options.frames * 1000 /
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
          << total_rays / (double{kReferenceSize * kReferenceSize} * options.frames)
          << ",\"rays_per_second_gpu\":";
      if (gpu.empty()) out << "null";
      else out << total_rays * 1000 / std::accumulate(gpu.begin(), gpu.end(), 0.0);
      out << ",\"generate_gpu_ms\":"; Series(out, generate);
      out << ",\"intersect_gpu_ms\":"; Series(out, intersect);
      out << ",\"shade_gpu_ms\":"; Series(out, shade);
      out << ",\"accumulate_gpu_ms\":"; Series(out, accumulate);
      out << '}';
    }
    out << ",\"scene_pool_reserved_bytes\":" << after.stats.memory.reserved_bytes
        << ",\"scene_pool_used_bytes\":" << after.stats.memory.used_bytes
        << ",\"initial_update\":";
    UpdateJson(out, initial, initial_wall);
    out << ",\"point_refit\":"; UpdateJson(out, point, point_wall);
    out << ",\"transform_refit\":"; UpdateJson(out, transform, transform_wall);
    out << ",\"image\":" << Quote(image_path.filename().string())
        << ",\"mean_rgb\":" << radiance_sum / (kReferenceSize * kReferenceSize * 3)
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
                 "       [--benchmark-integrator <reference|wavefront>]\n";
    return 2;
  }
  options.report = argv[2];
  for (int i = 3; i < argc; ++i) {
    const std::string_view option = argv[i];
    if (i + 1 == argc) { std::cerr << "missing value for " << option << '\n'; return 2; }
    const std::string_view value = argv[++i];
    if (option == "--benchmark-label") options.label = value;
    else if (option == "--benchmark-integrator") {
      if (value == "reference") options.integrator = Lotus::Integrator::Reference;
      else if (value == "wavefront") options.integrator = Lotus::Integrator::Wavefront;
      else { std::cerr << "invalid integrator: " << value << '\n'; return 2; }
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
  try {
    if (!options.report.parent_path().empty())
      std::filesystem::create_directories(options.report.parent_path());
    const auto started = std::time(nullptr);
    const auto executable = std::filesystem::absolute(argv[0]).parent_path();
    std::vector<Result> results;
    FrameStatus status = FrameStatus::Pass;
    for (const char* id : {"cornell-v1", "textured-v1", "instanced-v1", "geometry-v1"}) {
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
        << ",\"width\":128,\"height\":128,\"sample_index\":0,\"max_bounces\":64,"
           "\"samples_per_frame\":1,\"measured_frames\":" << options.frames
        << ",\"warmup_frames\":" << options.warmup
        << ",\"warmup_sample_index\":1,\"unchanged_batches\":16,\"unchanged_iterations\":1000,"
           "\"fresh_renderer_per_scene\":true,\"update_measurements_per_kind\":1},"
           "\"metric_notes\":{"
           "\"gpu_frame_ms\":\"One sample pass, including clears, excluding readback; one call per sample.\","
           "\"render_wall_ms\":\"Blocking RenderScene, including submit, fence wait, readback and product copies.\","
           "\"pixel_samples_per_second\":\"Full-frame pixel samples divided by total measured GPU or wall time, including camera misses.\","
           "\"unchanged_cpu_ms\":\"Commit, extraction and empty UpdateScene plus invariant checks; batch wall time / 1000.\","
           "\"scene_pool_bytes\":\"Scene pools include device-local and host-visible memory; exclude renderer targets and constants.\","
           "\"update_ms\":\"One initial upload/build, one point refit and one transform refit; CPU wall covers blocking UpdateScene only. Counters are cumulative.\","
           "\"unavailable_gpu_timestamps\":\"Null GPU durations/rates indicate no timestamp support; wall measurements remain available.\","
           "\"wavefront\":\"Wavefront integrator only: rays traced per frame from its queue counts, "
           "rays per camera path, rays per GPU second and per-kernel GPU durations; null for the reference integrator.\"},"
           "\"unavailable\":{";
    // The wavefront integrator counts its rays; the reference shader does not.
    if (!Wavefront(options))
      out << "\"rays_per_second\":null,\"average_path_depth\":null,";
    out << "\"total_vram_bytes\":null,\"cpu_render_submit_ms\":null},"
           "\"unavailable_reasons\":{";
    if (!Wavefront(options))
      out << "\"rays_per_second\":\"Reference shader has no ray counter; pixel samples are not ray counts.\","
             "\"average_path_depth\":\"Reference shader has no path-depth counter.\",";
    out << "\"total_vram_bytes\":\"Scene pool statistics are not total renderer allocations or process VRAM.\","
           "\"cpu_render_submit_ms\":\"Blocking API does not separate CPU submission from GPU wait/readback.\"},\"scenes\":[";
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
