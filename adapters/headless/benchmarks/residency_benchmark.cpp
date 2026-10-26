// SPDX-License-Identifier: Apache-2.0
#include "residency_benchmark.hpp"
#include "common/workloads.hpp"

#include <lotus/extraction.hpp>
#include <lotus/vulkan_backend.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace LotusHeadless {
namespace {
using namespace Lotus;

// What each mode renders: the renderer paths that own frame-sized state.
struct Mode {
  const char* id;
  Integrator integrator;
  bool temporal;
  bool accumulate;
  std::uint32_t filter_iterations;
  bool restir;
};
constexpr std::array<Mode, 7> kModes{{
    {"progressive-reference", Integrator::Reference, false, false, 0, false},
    {"progressive-wavefront", Integrator::Wavefront, false, false, 0, false},
    {"temporal-metadata", Integrator::Reference, true, false, 0, false},
    {"temporal-radiance", Integrator::Reference, true, true, 0, false},
    {"svgf", Integrator::Reference, true, true, kTemporalFilterIterations, false},
    {"restir-reuse", Integrator::Reference, true, false, 0, true},
    {"svgf-restir", Integrator::Reference, true, true, kTemporalFilterIterations, true},
}};

struct Options {
  std::filesystem::path report;
  std::string label;
  std::string workload = "cornell-v1";
  std::string mode;
  unsigned width = 1920, height = 1080, frames = 3;
  bool read_back = false;
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
    throw std::invalid_argument("usage: lotus-headless --benchmark-residency <json-path> "
                                "[--benchmark-label <identity>] [--residency-workload <id>] "
                                "[--residency-mode <id>] [--residency-width <16..8192>] "
                                "[--residency-height <16..8192>] [--residency-frames <1..16>] "
                                "[--residency-read-back <0|1>]");
  Options options;
  options.report = argv[2];
  for (int i = 3; i < argc; ++i) {
    const std::string_view option = argv[i];
    if (i + 1 == argc)
      throw std::invalid_argument("missing value for " + std::string(option));
    const std::string_view value = argv[++i];
    if (option == "--benchmark-label")
      options.label = value;
    else if (option == "--residency-workload") {
      if (!KnownWorkload(value))
        throw std::invalid_argument("unknown workload: " + std::string(value));
      options.workload = value;
    } else if (option == "--residency-mode") {
      if (std::none_of(kModes.begin(), kModes.end(), [&](const Mode& mode) { return mode.id == value; }))
        throw std::invalid_argument("unknown mode: " + std::string(value));
      options.mode = value;
    } else if (option == "--residency-width")
      options.width = Number(value, 16, 8192);
    else if (option == "--residency-height")
      options.height = Number(value, 16, 8192);
    else if (option == "--residency-frames")
      options.frames = Number(value, 1, 16);
    else if (option == "--residency-read-back")
      options.read_back = Number(value, 0, 1) != 0;
    else
      throw std::invalid_argument("unknown benchmark option: " + std::string(option));
  }
  return options;
}

std::string Json(const GpuResidency& residency) {
  std::ostringstream out;
  out << "{\"device_local_bytes\":" << residency.device_local_bytes
      << ",\"host_bytes\":" << residency.host_bytes
      << ",\"swapchain_images\":" << residency.swapchain_images << ",\"groups\":[";
  for (std::size_t i = 0; i < residency.groups.size(); ++i) {
    const auto& group = residency.groups[i];
    out << (i ? "," : "") << "{\"owner\":" << Quote(group.owner)
        << ",\"device_local\":" << (group.device_local ? "true" : "false")
        << ",\"allocations\":" << group.allocations << ",\"bytes\":" << group.bytes << '}';
  }
  out << "],\"heaps\":[";
  for (std::size_t i = 0; i < residency.heaps.size(); ++i) {
    const auto& heap = residency.heaps[i];
    out << (i ? "," : "") << "{\"device_local\":" << (heap.device_local ? "true" : "false")
        << ",\"size\":" << heap.size << ",\"process_usage\":";
    if (residency.budget_available)
      out << heap.process_usage << ",\"budget\":" << heap.budget << '}';
    else
      out << "null,\"budget\":null}";
  }
  out << "]}";
  return out.str();
}

struct Result {
  std::string id;
  FrameStatus status = FrameStatus::Fail;
  std::string detail;
  std::string device = "null";
  std::string measurements = "null";
};

// The largest of each sampled quantity: the renderer's sums and each heap's
// process usage.
struct Peak {
  std::uint64_t device_local_bytes = 0, host_bytes = 0;
  std::vector<std::uint64_t> heap_usage;
  void Sample(const GpuResidency& residency) {
    device_local_bytes = std::max(device_local_bytes, residency.device_local_bytes);
    host_bytes = std::max(host_bytes, residency.host_bytes);
    heap_usage.resize(residency.heaps.size());
    for (std::size_t i = 0; i < residency.heaps.size(); ++i)
      heap_usage[i] = std::max(heap_usage[i], residency.heaps[i].process_usage);
  }
};

void RequireFrame(const GpuFrameEvidence& frame) {
  Require(frame.status == FrameStatus::Pass, frame.detail);
  Require(frame.validation_message_count == 0, frame.validation_detail);
}

// One mode on a fresh renderer, so no other mode's buffers persist in it.
Result Measure(const Mode& mode, const Options& options, const std::filesystem::path& executable) {
  Result result;
  result.id = mode.id;
  try {
    const auto shaders = ShadersIn(executable / "shaders");
    FrameStatus setup = FrameStatus::Fail;
    std::string why;
    auto renderer = CreateOffscreenRenderer(shaders.vertex, shaders.fragment, setup, why, shaders.ray_query);
    if (!renderer) {
      result.status = setup;
      result.detail = why;
      return result;
    }
    const auto capability = renderer->RayQueryCapability();
    if (!capability.available) {
      result.status = FrameStatus::Skip;
      result.detail = capability.detail;
      return result;
    }
    Peak peak;
    const auto created = renderer->Residency();
    peak.Sample(created);
    auto work = MakeWorkload(options.workload, executable / "assets");
    SceneExtraction extraction;
    const auto snapshot = work.world.Commit();
    const auto update = renderer->UpdateScene(extraction.Update(snapshot));
    Require(update.status == FrameStatus::Pass, update.detail);
    Require(update.validation_message_count == 0, "scene upload emitted validation messages");
    const auto uploaded = renderer->Residency();
    peak.Sample(uploaded);
    OffscreenTarget target;
    target.width = options.width;
    target.height = options.height;
    // By default as the viewport and Hydra render: the products stay on
    // the GPU. Reading back allocates the host mirrors.
    target.read_back = options.read_back;
    PathTracingSettings settings;
    settings.integrator = mode.integrator;
    if (mode.restir) {
      settings.light_sampling = LightSampling::RestirInitial;
      settings.restir_temporal = settings.restir_spatial = true;
    }
    TemporalFrameSettings temporal;
    temporal.accumulate_radiance = mode.accumulate;
    temporal.filter_iterations = mode.filter_iterations;
    const auto draw = ExtractDrawSummary(snapshot);
    GpuResidency last;
    for (unsigned f = 0; f < options.frames; ++f) {
      temporal.frame_index = f;
      const auto frame = mode.temporal ? renderer->RenderTemporalScene(snapshot, target, 1, temporal, settings)
                                       : renderer->RenderScene(draw, target, 1, settings);
      RequireFrame(frame);
      Require(!mode.temporal || frame.temporal.radiance_available == mode.accumulate, "unexpected radiance mode");
      Require(!mode.restir || frame.restir.available, "missing reservoirs");
      Require(!options.read_back ||
                  (!frame.color.payload.empty() && (!mode.temporal || !frame.temporal.validity.empty()) &&
                      (!mode.accumulate || !frame.temporal.radiance.empty()) &&
                      (!mode.restir || !frame.restir.spatial.empty())),
          "missing read-back products");
      result.device = "{\"name\":" + Quote(frame.device_name) + ",\"api\":" + Quote(frame.api_version) +
                      ",\"driver\":" + Quote(frame.driver_version) + ",\"validation_available\":" +
                      (frame.validation_available ? "true" : "false") + '}';
      last = renderer->Residency();
      peak.Sample(last);
    }
    std::ostringstream out;
    out << "{\"settings\":{\"integrator\":" << Quote(mode.integrator == Integrator::Wavefront ? "wavefront" : "reference")
        << ",\"light_sampling\":" << Quote(mode.restir ? "restir-initial" : "mis")
        << ",\"temporal\":" << (mode.temporal ? "true" : "false")
        << ",\"accumulate_radiance\":" << (mode.accumulate ? "true" : "false")
        << ",\"filter_iterations\":" << mode.filter_iterations
        << ",\"restir_temporal\":" << (mode.restir ? "true" : "false")
        << ",\"restir_spatial\":" << (mode.restir ? "true" : "false")
        << "},\"budget_available\":" << (last.budget_available ? "true" : "false")
        << ",\"budget_detail\":" << Quote(last.budget_detail)
        << ",\"created\":" << Json(created) << ",\"uploaded\":" << Json(uploaded)
        << ",\"last_frame\":" << Json(last)
        << ",\"sampled_peak\":{\"device_local_bytes\":" << peak.device_local_bytes
        << ",\"host_bytes\":" << peak.host_bytes << ",\"heap_process_usage\":";
    if (last.budget_available) {
      out << '[';
      for (std::size_t i = 0; i < peak.heap_usage.size(); ++i)
        out << (i ? "," : "") << peak.heap_usage[i];
      out << ']';
    } else {
      out << "null";
    }
    out << "}}";
    result.measurements = out.str();
    result.status = FrameStatus::Pass;
  } catch (const std::exception& error) {
    result.status = FrameStatus::Fail;
    result.detail = error.what();
  }
  return result;
}

const char* Status(FrameStatus status) {
  return status == FrameStatus::Pass ? "pass" : status == FrameStatus::Skip ? "skip" : "fail";
}

} // namespace

int ResidencyBenchmarkMain(int argc, char** argv) {
  Options options;
  try {
    options = Parse(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 2;
  }
  try {
    if (!options.report.parent_path().empty())
      std::filesystem::create_directories(options.report.parent_path());
    const auto started = std::time(nullptr);
    const auto executable = std::filesystem::absolute(argv[0]).parent_path();
    std::vector<Result> results;
    FrameStatus status = FrameStatus::Pass;
    std::string device = "null";
    for (const auto& mode : kModes) {
      if (!options.mode.empty() && options.mode != mode.id)
        continue;
      auto result = Measure(mode, options, executable);
      std::cout << mode.id << ": " << Status(result.status) << ' ' << result.detail << '\n';
      if (result.status == FrameStatus::Fail)
        status = FrameStatus::Fail;
      else if (result.status == FrameStatus::Skip && status == FrameStatus::Pass)
        status = FrameStatus::Skip;
      if (result.device != "null")
        device = result.device;
      results.push_back(std::move(result));
      // Without a device or ray queries no other mode can run either.
      if (results.back().status == FrameStatus::Skip)
        break;
    }
    std::ofstream out(options.report, std::ios::trunc);
    out << "{\"schema\":\"lotus-residency-benchmark-v1\",\"status\":" << Quote(Status(status))
        << ",\"started_unix\":" << started << ",\"completed_unix\":" << std::time(nullptr)
        << ",\"build\":{\"label\":" << Quote(options.label)
        << ",\"compiler\":" << Quote(LOTUS_BENCHMARK_COMPILER)
        << ",\"configuration\":" << Quote(LOTUS_BENCHMARK_CONFIG)
        << ",\"system\":" << Quote(LOTUS_BENCHMARK_SYSTEM) << "},\"device\":" << device
        << ",\"procedure\":{\"workload\":" << Quote(options.workload) << ",\"width\":" << options.width
        << ",\"height\":" << options.height << ",\"pixels\":" << std::uint64_t{options.width} * options.height
        << ",\"frames\":" << options.frames << ",\"samples_per_frame\":1,\"read_back\":"
        << (options.read_back ? "true" : "false") << ",\"fresh_renderer_per_mode\":true},"
           "\"metric_notes\":{"
           "\"groups\":\"OffscreenRenderer::Residency: the renderer's own allocations by owner and memory kind, "
           "allocation sizes with alignment; scene pools count reserved blocks. Driver objects (pipelines, descriptor and "
           "query pools, command buffers) and swapchain images are outside them.\","
           "\"created\":\"After CreateOffscreenRenderer, before any scene or frame.\","
           "\"uploaded\":\"After the workload's initial UpdateScene.\","
           "\"last_frame\":\"After the mode's last frame; allocations persist between frames.\","
           "\"heaps\":\"VK_EXT_memory_budget per heap: the driver's estimate of this process's usage and budget; "
           "null without the extension.\","
           "\"sampled_peak\":\"The largest value among the created, uploaded and per-frame samples: not a "
           "continuous peak, and transient allocations inside a call (staging, scratch) are only seen if they persist.\"},"
           "\"modes\":[";
    for (std::size_t i = 0; i < results.size(); ++i) {
      const auto& result = results[i];
      out << (i ? "," : "") << "{\"id\":" << Quote(result.id) << ",\"status\":" << Quote(Status(result.status))
          << ",\"detail\":" << Quote(result.detail) << ",\"measurements\":" << result.measurements << '}';
    }
    out << "]}\n";
    out.close();
    Require(!out.fail(), "cannot write residency report: " + options.report.string());
    return status == FrameStatus::Pass ? 0 : status == FrameStatus::Skip ? 77 : 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

} // namespace LotusHeadless
