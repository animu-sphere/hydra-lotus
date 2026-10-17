// SPDX-License-Identifier: Apache-2.0
#include "frame_loop.hpp"

#include <algorithm>
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

std::string Title(const Scene& scene, const RunTotals& totals) {
  std::ostringstream title;
  title << "lotus-viewport | " << scene.source() << " | " << scene.Status()
        << " | " << totals.last.frame.device_name << " | " << totals.width << 'x'
        << totals.height << " | " << totals.last.frame.samples_per_pixel
        << " spp | " << std::setprecision(2) << std::fixed
        << FrameGpuMs(totals.last.frame) << " ms GPU";
  return title.str();
}

// The interaction test: each step is applied after a frame, and the next
// frame says whether it restarted the accumulation.
class InteractionTest {
public:
  InteractionTest(std::vector<InteractionStep> steps, std::uint32_t samples)
      : steps_(std::move(steps)), samples_(samples) {
  }

  // After each frame: checks the pending step, then applies the next one
  // from the second frame on. False, with `error`, fails the run.
  bool AfterFrame(std::uint64_t frames, std::uint32_t samples_per_pixel,
      std::string& error) {
    if (pending_) {
      pending_ = false;
      const std::uint32_t expected =
          restarts_ ? 1U : std::min(before_ + 1U, samples_);
      const InteractionStep& step = steps_[verified_];
      if (samples_per_pixel != expected) {
        error = "interaction test: " + step.name + " left " +
                std::to_string(samples_per_pixel) +
                " samples per pixel, expected " + std::to_string(expected) +
                (restarts_ ? " (a restarted accumulation)"
                           : " (a continued accumulation)");
        return false;
      }
      std::cout << "Interaction: " << step.name << ": "
                << (restarts_ ? "restarted" : "continued")
                << " the accumulation (" << samples_per_pixel << " spp)\n";
      ++verified_;
    }
    if (frames >= 2 && verified_ < steps_.size()) {
      const InteractionStep& step = steps_[verified_];
      restarts_ = step.restarts;
      before_ = samples_per_pixel;
      if (!step.apply(restarts_, error)) {
        error = "interaction test: " + step.name + ": " + error;
        return false;
      }
      pending_ = true;
    }
    return true;
  }

  [[nodiscard]] std::size_t verified() const noexcept {
    return verified_;
  }
  [[nodiscard]] std::size_t size() const noexcept {
    return steps_.size();
  }

private:
  std::vector<InteractionStep> steps_;
  std::uint32_t samples_ = 0;
  std::size_t verified_ = 0;
  bool pending_ = false;
  bool restarts_ = true;
  std::uint32_t before_ = 0;
};

} // namespace

void Drag(Scene& scene, Button button, double delta_x, double delta_y) {
  Event event{EventType::PointerDown};
  event.button = button;
  scene.Handle(event);
  event.type = EventType::PointerMove;
  event.x = delta_x;
  event.y = delta_y;
  scene.Handle(event);
  event.type = EventType::PointerUp;
  scene.Handle(event);
}

void Wheel(Scene& scene, double notches) {
  Event event{EventType::Scroll};
  event.scroll = notches;
  scene.Handle(event);
}

void Press(Scene& scene, Key key) {
  Event event{EventType::KeyDown};
  event.key = key;
  scene.Handle(event);
}

std::vector<InteractionStep> NavigationSteps(Scene& scene) {
  const auto step = [&scene](std::string name, std::function<void()> input) {
    return InteractionStep{std::move(name), true,
        [input = std::move(input)](bool&, std::string&) {
          input();
          return true;
        }};
  };
  return {step("tumble (left drag)",
              [&scene] { Drag(scene, Button::Left, 40.0, -20.0); }),
      step("track (middle drag)",
          [&scene] { Drag(scene, Button::Middle, -30.0, 15.0); }),
      step("dolly (wheel)", [&scene] { Wheel(scene, 1.0); }),
      step("frame (F)", [&scene] { Press(scene, Key::Frame); })};
}

int Unavailable(const Options& options, std::string_view reason) {
  std::cout << "SKIP: " << reason << '\n';
  if (!options.allow_unavailable) {
    std::cerr << "lotus-viewport: this environment cannot present; "
                 "--allow-unavailable reports that as a skip (77)\n";
  }
  return options.allow_unavailable ? kExitSkip : 1;
}

int RunFrameLoop(const Options& options, Window& window, Scene& scene,
    const std::string& stage) {
  RunTotals totals;
  InteractionTest interaction(options.interaction_test
                                  ? scene.InteractionSteps()
                                  : std::vector<InteractionStep>{},
      options.samples);
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
      } else if (event.type != EventType::Resize) {
        scene.Handle(event);
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
    if (!scene.Render(width, height, result, error)) {
      std::cerr << "lotus-viewport: frame " << totals.frames + 1 << ": "
                << error << '\n';
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
    if (!interaction.AfterFrame(totals.frames,
            totals.last.frame.samples_per_pixel, error)) {
      std::cerr << "lotus-viewport: " << error << '\n';
      return 1;
    }
    if (options.resize_test && !resized_for_test &&
        totals.last.present.frames_presented == 2) {
      window.SetSize(width + 64U, height + 32U);
      resized_for_test = true;
    }
    const auto now = Clock::now();
    if (now - title_update >= std::chrono::milliseconds(250)) {
      window.SetTitle(Title(scene, totals));
      title_update = now;
    }
  }

  if (!options.benchmark.empty()) {
    WriteBenchmark(options.benchmark, scene.source(), stage, totals);
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
  if (interaction.verified() != interaction.size()) {
    std::cerr << "lotus-viewport: the interaction test verified "
              << interaction.verified() << " of " << interaction.size()
              << " steps; it needs --frames " << interaction.size() + 2
              << " or more\n";
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
              << scene.source() << " on " << frame.device_name << " ("
              << frame.samples_per_pixel << " spp, "
              << present.swapchain_format << ' ' << present.present_mode
              << ", swapchain recreates: " << present.swapchain_recreates
              << ")\n";
  }
  return exit_code;
}

} // namespace Lotus::viewport
