// SPDX-License-Identifier: Apache-2.0
#include "frame_loop.hpp"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace Lotus::viewport {
namespace {

using Clock = std::chrono::steady_clock;

// Totals over the run. GPU time is the scene pass plus, under the wavefront
// integrator, its kernels.
struct RunTotals {
  std::uint64_t frames = 0;
  std::uint64_t present_skips = 0;
  std::uint64_t cpu_ns = 0;
  double gpu_ms = 0.0;
  std::uint64_t gpu_frames = 0;
  std::uint64_t readback_bytes = 0;
  FrameResult last;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

double FrameGpuMs(const GpuFrameEvidence& frame) {
  double total = frame.primary_ray_gpu_ms;
  const WavefrontTimings& wavefront = frame.wavefront_timings;
  if (wavefront.available) {
    total += wavefront.camera_gpu_ms + wavefront.intersect_gpu_ms +
             wavefront.shade_gpu_ms + wavefront.tail_gpu_ms;
  }
  return total;
}

std::string JsonString(std::string_view value) {
  std::ostringstream out;
  out << '"';
  for (const char c : value) {
    if (c == '"' || c == '\\') {
      out << '\\' << c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
          << static_cast<int>(c) << std::dec;
    } else {
      out << c;
    }
  }
  out << '"';
  return out.str();
}

void WriteBenchmark(const std::filesystem::path& path,
    std::string_view scene_source, const std::string& stage,
    const RunTotals& totals) {
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("could not write the benchmark: " +
                             path.string());
  }
  const GpuFrameEvidence& frame = totals.last.frame;
  const PresentEvidence& present = totals.last.present;
  const auto average = [&](std::uint64_t total) {
    return totals.frames == 0 ? 0 : total / totals.frames;
  };
  out << std::setprecision(6) << std::fixed;
  out << "{\n"
      << "  \"schema\": \"lotus.viewport-benchmark/v1\",\n"
      << "  \"scene_source\": " << JsonString(scene_source) << ",\n"
      << "  \"stage\": " << (stage.empty() ? "null" : JsonString(stage))
      << ",\n"
      << "  \"device\": " << JsonString(frame.device_name) << ",\n"
      << "  \"width\": " << totals.width << ",\n"
      << "  \"height\": " << totals.height << ",\n"
      << "  \"frames\": " << totals.frames << ",\n"
      << "  \"frames_presented\": " << present.frames_presented << ",\n"
      << "  \"present_skips\": " << totals.present_skips << ",\n"
      << "  \"swapchain_recreates\": " << present.swapchain_recreates
      << ",\n"
      << "  \"swapchain_format\": " << JsonString(present.swapchain_format)
      << ",\n"
      << "  \"present_mode\": " << JsonString(present.present_mode) << ",\n"
      << "  \"samples_per_pixel\": " << frame.samples_per_pixel << ",\n"
      << "  \"cpu_total_ns\": " << totals.cpu_ns << ",\n"
      << "  \"cpu_average_frame_ns\": " << average(totals.cpu_ns) << ",\n"
      << "  \"gpu_timestamps\": "
      << (totals.gpu_frames != 0 ? "true" : "false") << ",\n"
      << "  \"gpu_total_ms\": " << totals.gpu_ms << ",\n"
      << "  \"gpu_average_frame_ms\": "
      << (totals.gpu_frames == 0
                 ? 0.0
                 : totals.gpu_ms / static_cast<double>(totals.gpu_frames))
      << ",\n"
      << "  \"readback_bytes\": " << totals.readback_bytes << ",\n"
      << "  \"validation_available\": "
      << (frame.validation_available ? "true" : "false") << ",\n"
      << "  \"synchronization_validation_available\": "
      << (frame.synchronization_validation_available ? "true" : "false")
      << ",\n"
      << "  \"validation_messages\": " << present.validation_message_count
      << "\n"
      << "}\n";
}

std::string Title(std::string_view scene_source, const RunTotals& totals) {
  std::ostringstream title;
  title << "lotus-viewport | " << scene_source << " | "
        << totals.last.frame.device_name << " | " << totals.width << 'x'
        << totals.height << " | " << totals.last.frame.samples_per_pixel
        << " spp | " << std::setprecision(2) << std::fixed
        << FrameGpuMs(totals.last.frame) << " ms GPU";
  return title.str();
}

} // namespace

int Unavailable(const Options& options, std::string_view reason) {
  std::cout << "SKIP: " << reason << '\n';
  if (!options.allow_unavailable) {
    std::cerr << "lotus-viewport: this environment cannot present; "
                 "--allow-unavailable reports that as a skip (77)\n";
  }
  return options.allow_unavailable ? kExitSkip : 1;
}

int RunFrameLoop(const Options& options, Window& window,
    std::string_view scene_source, const std::string& stage,
    const RenderFrameFunction& render) {
  RunTotals totals;
  // Out-of-date swapchains skip a present without an error; a run that
  // keeps skipping is still a failure.
  const std::uint64_t frame_cap =
      options.frame_limit == 0 ? 0 : options.frame_limit * 2 + 16;
  bool resized_for_test = false;
  bool running = true;
  auto title_update = Clock::now();
  std::string error;
  while (running && (options.frame_limit == 0 ||
                        totals.last.present.frames_presented <
                            options.frame_limit)) {
    Event event;
    while (window.PollEvent(event)) {
      if (event.type == EventType::Close ||
          (event.type == EventType::KeyDown && event.key == Key::Escape)) {
        running = false;
      } else if (event.type == EventType::KeyDown &&
                 event.key == Key::Fullscreen) {
        window.SetFullscreen(!window.fullscreen());
      }
    }
    if (!running) {
      break;
    }
    const std::uint32_t width = window.width();
    const std::uint32_t height = window.height();
    if (width == 0 || height == 0) {
      window.WaitForEvent();
      continue;
    }
    if (frame_cap != 0 && totals.frames >= frame_cap) {
      std::cerr << "lotus-viewport: presented "
                << totals.last.present.frames_presented << " of "
                << options.frame_limit << " frames in " << totals.frames
                << " attempts\n";
      return 1;
    }
    FrameResult result;
    const auto start = Clock::now();
    if (!render(width, height, result, error)) {
      std::cerr << "lotus-viewport: " << error << '\n';
      return 1;
    }
    totals.cpu_ns += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - start)
            .count());
    if (result.present.status == FrameStatus::Fail) {
      std::cerr << "lotus-viewport: presentation failed: "
                << result.present.detail << '\n';
      return 1;
    }
    ++totals.frames;
    totals.present_skips += result.present.status == FrameStatus::Skip;
    if (result.frame.primary_ray_timestamp_available) {
      totals.gpu_ms += FrameGpuMs(result.frame);
      ++totals.gpu_frames;
    }
    totals.readback_bytes +=
        std::uint64_t{result.frame.color.row_pitch} * result.frame.color.height +
        std::uint64_t{result.frame.depth.row_pitch} * result.frame.depth.height;
    totals.width = width;
    totals.height = height;
    totals.last = std::move(result);
    if (options.resize_test && !resized_for_test &&
        totals.last.present.frames_presented == 2) {
      window.SetSize(width + 64U, height + 32U);
      resized_for_test = true;
    }
    const auto now = Clock::now();
    if (now - title_update >= std::chrono::milliseconds(250)) {
      window.SetTitle(Title(scene_source, totals));
      title_update = now;
    }
  }

  if (!options.benchmark.empty()) {
    WriteBenchmark(options.benchmark, scene_source, stage, totals);
  }
  const GpuFrameEvidence& frame = totals.last.frame;
  const PresentEvidence& present = totals.last.present;
  int exit_code = 0;
  if (present.validation_message_count != 0) {
    std::cerr << "lotus-viewport: Vulkan validation reported "
              << present.validation_message_count
              << " message(s): " << present.validation_detail << '\n';
    exit_code = 1;
  }
  if (options.validate && totals.frames != 0 && !frame.validation_available) {
    std::cerr << "lotus-viewport: --validate, but Vulkan validation was not "
                 "active: "
              << frame.validation_detail << '\n';
    exit_code = 1;
  }
  if (totals.readback_bytes != 0) {
    std::cerr << "lotus-viewport: presented frames read back "
              << totals.readback_bytes << " bytes\n";
    exit_code = 1;
  }
  if (options.resize_test && present.swapchain_recreates == 0) {
    std::cerr << "lotus-viewport: the swapchain did not follow the resize\n";
    exit_code = 1;
  }
  if (options.frame_limit != 0 && running &&
      present.frames_presented < options.frame_limit) {
    std::cerr << "lotus-viewport: presented " << present.frames_presented
              << " of " << options.frame_limit << " requested frames\n";
    exit_code = 1;
  }
  if (exit_code == 0) {
    std::cout << "Presented " << present.frames_presented << " frames of "
              << scene_source << " on " << frame.device_name << " ("
              << frame.samples_per_pixel << " spp, "
              << present.swapchain_format << ' ' << present.present_mode
              << ", swapchain recreates: " << present.swapchain_recreates
              << ")\n";
  }
  return exit_code;
}

} // namespace Lotus::viewport
