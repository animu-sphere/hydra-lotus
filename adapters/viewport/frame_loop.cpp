// SPDX-License-Identifier: Apache-2.0
#include "frame_loop.hpp"
#include "timings.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace Lotus::viewport {
namespace {

using Clock = std::chrono::steady_clock;

// Totals over the run. GPU time is every submission the frame timed: the
// scene pass, and the temporal passes submitted after it.
struct RunTotals {
  std::uint64_t frames = 0;
  std::uint64_t present_skips = 0;
  std::uint64_t cpu_ns = 0;
  double gpu_ms = 0.0;
  std::uint64_t gpu_frames = 0;
  std::uint64_t readback_bytes = 0;
  FrameResult last;
  std::uint64_t last_cpu_ns = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  TimingRecord timings;
};

template <typename Record>
std::uint64_t Bytes(const std::vector<Record>& records) {
  return std::uint64_t{records.size()} * sizeof(Record);
}

// Everything the frame copied to the host: a presented frame copies
// nothing.
std::uint64_t ReadbackBytes(const GpuFrameEvidence& frame) {
  const TemporalProducts& temporal = frame.temporal;
  const RestirProducts& restir = frame.restir;
  return Bytes(frame.color.payload) + Bytes(frame.depth.payload) +
         Bytes(temporal.current) + Bytes(temporal.previous) +
         Bytes(temporal.validity) + Bytes(temporal.radiance) +
         Bytes(temporal.previous_radiance) + Bytes(restir.initial) +
         Bytes(restir.temporal) + Bytes(restir.previous) +
         Bytes(restir.spatial) + Bytes(restir.spatial_sources);
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
    const RunTotals& totals, const Controls& controls) {
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
      << "  \"controls\": {\"integrator\": "
      << JsonString(Name(controls.integrator))
      << ", \"nee\": " << (controls.nee ? "true" : "false")
      << ", \"restir\": " << JsonString(Name(controls.restir))
      << ", \"restir_candidates\": " << controls.restir_candidates
      << ", \"temporal_radiance\": "
      << (controls.temporal_radiance ? "true" : "false")
      << ", \"denoise\": " << (controls.denoise ? "true" : "false")
      << ", \"filter_iterations\": " << controls.filter_iterations
      << ", \"accumulate\": " << (controls.accumulate ? "true" : "false")
      << ", \"view\": " << JsonString(Name(controls.view)) << "},\n"
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
      << "  \"gpu_pass_average_ms\": {";
  // Each pass over the frames that ran it; null when none did.
  const TimingRecord& timings = totals.timings;
  for (std::size_t pass = 0; pass < kGpuPassCount; ++pass) {
    out << (pass == 0 ? "" : ", ")
        << JsonString(Name(static_cast<GpuPass>(pass))) << ": ";
    if (timings.pass_frames()[pass] == 0) {
      out << "null";
    } else {
      out << timings.pass_totals()[pass] /
                 static_cast<double>(timings.pass_frames()[pass]);
    }
  }
  const SceneUpdateTimes& update = timings.scene_update();
  const MemoryUse& memory = timings.memory();
  out << "},\n"
      << "  \"scene_updates\": " << update.count << ",\n"
      << "  \"last_scene_update\": ";
  if (update.available) {
    out << "{\"frame\": " << update.frame
        << ", \"upload_gpu_ms\": " << update.upload_ms
        << ", \"blas_gpu_ms\": " << update.blas_ms
        << ", \"tlas_gpu_ms\": " << update.tlas_ms << "}";
  } else {
    out << "null";
  }
  out << ",\n"
      << "  \"memory_bytes\": {\"reservoirs\": " << memory.reservoir_bytes
      << ", \"surface_history\": " << memory.surface_history_bytes
      << ", \"radiance_history\": " << memory.radiance_history_bytes
      << ", \"filter\": " << memory.filter_bytes
      << ", \"scene_used\": " << memory.scene_used_bytes
      << ", \"scene_reserved\": " << memory.scene_reserved_bytes
      << ", \"acceleration\": " << memory.acceleration_bytes << "},\n"
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

std::string Title(const Scene& scene, const RunTotals& totals,
    const Controls& controls) {
  std::ostringstream title;
  title << "lotus-viewport | " << scene.source() << " | " << scene.Status()
        << " | " << Name(controls.integrator) << ' '
        << (controls.restir != RestirMode::Off
                   ? "restir-" + std::string(Name(controls.restir))
               : controls.nee ? std::string("nee")
                              : std::string("bsdf"))
        << (controls.temporal_radiance ? " temporal-radiance" : "")
        << (controls.denoise ? " denoise" : "")
        << (controls.view != DebugView::Beauty
                   ? " | " + std::string(Name(controls.view))
                   : std::string())
        << " | " << totals.last.frame.device_name << " | " << totals.width
        << 'x' << totals.height << " | "
        << totals.last.frame.samples_per_pixel << " spp | "
        << std::setprecision(2) << std::fixed
        << FrameGpuMs(totals.last.frame) << " ms GPU";
  return title.str();
}

// The interaction test: each step is applied after a frame, and the next
// frame says whether it restarted the accumulation.
class InteractionTest {
public:
  InteractionTest(std::vector<InteractionStep> steps, const Controls& controls)
      : steps_(std::move(steps)), controls_(controls) {
  }

  // After each frame: checks the pending step, then applies the next one
  // from the second frame on. False, with `error`, fails the run.
  bool AfterFrame(std::uint64_t frames, std::uint32_t samples_per_pixel,
      std::string& error) {
    if (pending_) {
      pending_ = false;
      const std::uint32_t expected =
          restarts_ ? 1U : std::min(before_ + 1U, controls_.samples);
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
  const Controls& controls_;
  std::size_t verified_ = 0;
  bool pending_ = false;
  bool restarts_ = true;
  std::uint32_t before_ = 0;
};

// Every renderer control and debug view in turn, from the run's controls,
// each restarting or continuing the accumulation as Restarts says, and
// back to where it began.
std::vector<InteractionStep> ControlSteps(Controls& controls) {
  std::vector<InteractionStep> steps;
  Controls planned = controls;
  const Controls initial = controls;
  const auto step = [&](std::string name, auto edit) {
    Controls after = planned;
    edit(after);
    steps.push_back({std::move(name), Restarts(planned, after),
        [&controls, after](bool&, std::string&) {
          controls = after;
          return true;
        }});
    planned = after;
  };
  const auto view = [&](DebugView shown) {
    step("view " + std::string(Name(shown)),
        [shown](Controls& c) { c.view = shown; });
  };
  step("wavefront integrator",
      [](Controls& c) { c.integrator = Integrator::Wavefront; });
  step("sample limit 512", [](Controls& c) { c.samples = 512; });
  for (const DebugView shown : {DebugView::Depth, DebugView::Normal,
           DebugView::Motion, DebugView::TemporalValidity}) {
    view(shown);
  }
  step("NEE/MIS off", [](Controls& c) { c.nee = false; });
  step("reference integrator",
      [](Controls& c) { c.integrator = Integrator::Reference; });
  step("ReSTIR initial", [](Controls& c) { c.restir = RestirMode::Initial; });
  for (const DebugView shown : {DebugView::ReservoirValidity,
           DebugView::ReservoirAge, DebugView::ReservoirWeight}) {
    view(shown);
  }
  step("4 candidates", [](Controls& c) { c.restir_candidates = 4; });
  for (const RestirMode mode :
      {RestirMode::Temporal, RestirMode::Spatial, RestirMode::Combined}) {
    step("ReSTIR " + std::string(Name(mode)),
        [mode](Controls& c) { c.restir = mode; });
  }
  view(DebugView::ReservoirAge);
  view(DebugView::TemporalValidity);
  step("wavefront ReSTIR combined",
      [](Controls& c) { c.integrator = Integrator::Wavefront; });
  step("ReSTIR off", [](Controls& c) { c.restir = RestirMode::Off; });
  step("temporal radiance", [](Controls& c) { c.temporal_radiance = true; });
  view(DebugView::Motion);
  step("denoise", [](Controls& c) { c.denoise = true; });
  for (const DebugView shown : {DebugView::RawRadiance,
           DebugView::AccumulatedRadiance, DebugView::HistoryLength,
           DebugView::Variance}) {
    view(shown);
  }
  step("split view", [](Controls& c) { c.split = true; });
  step("1 filter iteration", [](Controls& c) { c.filter_iterations = 1; });
  step("temporal radiance off, still denoised",
      [](Controls& c) { c.temporal_radiance = false; });
  step("denoise off", [](Controls& c) {
    c.denoise = false;
    c.view = DebugView::Beauty;
    c.split = false;
  });
  step("accumulation off", [](Controls& c) { c.accumulate = false; });
  step("the run's controls", [initial](Controls& c) { c = initial; });
  return steps;
}

UiStatus Status(const Scene& scene, const RunTotals& totals,
    std::string_view stage, bool can_open_stage) {
  UiStatus status;
  status.source = scene.source();
  status.scene = scene.Status();
  status.device = totals.last.frame.device_name;
  status.width = totals.width;
  status.height = totals.height;
  status.samples_per_pixel = totals.last.frame.samples_per_pixel;
  status.timings = &totals.timings;
  status.depth_range = scene.DepthRange();
  status.stage = stage;
  status.can_open_stage = can_open_stage;
  return status;
}

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

std::optional<ViewportWindow> OpenWindow(const Options& options,
    std::string& reason) {
  ViewportWindow opened;
  opened.presentation.vsync = options.vsync;
  opened.controls = options.controls;
  try {
    opened.window = Window::Create("lotus-viewport", options.width,
        options.height, options.visible, options.fullscreen);
    opened.presentation.surface = MakeSurfaceProvider(*opened.window);
  } catch (const std::exception& environment) {
    reason = environment.what();
    return std::nullopt;
  }
  opened.ui = DeveloperUi::Create(*opened.window);
  opened.ui->ConfigurePresentation(opened.presentation);
  return opened;
}

int RunFrameLoop(const Options& options, ViewportWindow& viewport,
    Scene& scene, const std::string& stage) {
  Window& window = *viewport.window;
  DeveloperUi& ui = *viewport.ui;
  Controls& controls = viewport.controls;
  RunTotals totals;
  std::vector<InteractionStep> steps;
  if (options.interaction_test) {
    steps = scene.InteractionSteps();
    for (InteractionStep& step : ControlSteps(controls)) {
      steps.push_back(std::move(step));
    }
  }
  InteractionTest interaction(std::move(steps), controls);
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
      const bool key = event.type == EventType::KeyDown;
      const bool pointer = event.type == EventType::PointerDown ||
                           event.type == EventType::PointerMove ||
                           event.type == EventType::Scroll;
      if (event.type == EventType::Close ||
          (key && event.key == Key::Escape && !ui.WantsKeyboard())) {
        running = false;
      } else if (key && event.key == Key::Fullscreen) {
        window.SetFullscreen(!window.fullscreen());
      } else if (key && event.key == Key::ToggleUi) {
        ui.ToggleVisible();
      } else if (key && event.key == Key::ToggleDenoise) {
        controls.denoise = !controls.denoise;
      } else if ((key && ui.WantsKeyboard()) || (pointer && ui.WantsPointer())) {
        // The developer UI has it; a release still ends a scene drag.
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
    const auto start = Clock::now();
    (void)ui.Draw(controls,
        Status(scene, totals, stage, static_cast<bool>(viewport.open_stage)));
    if (std::optional<std::filesystem::path> picked = ui.TakeStageRequest()) {
      std::string open_error = "this build cannot open USD stages";
      if (viewport.open_stage && viewport.open_stage(*picked, open_error)) {
        viewport.next_stage = std::move(picked);
        running = false;
        break;
      }
      std::cerr << "lotus-viewport: " << open_error << '\n';
      ui.SetStageError(open_error);
    }
    const FramePlan plan = PlanFrame(controls, totals.frames,
        scene.DepthRange());
    FrameResult result;
    if (!scene.Render(width, height, plan, result, error)) {
      std::cerr << "lotus-viewport: frame " << totals.frames + 1 << ": "
                << error << '\n';
      return 1;
    }
    totals.last_cpu_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - start)
            .count());
    totals.cpu_ns += totals.last_cpu_ns;
    if (result.present.status == FrameStatus::Fail) {
      std::cerr << "lotus-viewport: presentation failed: "
                << result.present.detail << '\n';
      return 1;
    }
    // The scene, through Hydra too, must filter as the controls ask.
    if (result.frame.temporal.filter_iterations !=
        plan.temporal.filter_iterations) {
      std::cerr << "lotus-viewport: frame " << totals.frames + 1 << " ran "
                << result.frame.temporal.filter_iterations
                << " filter iterations, the controls asked for "
                << plan.temporal.filter_iterations << '\n';
      return 1;
    }
    ++totals.frames;
    totals.present_skips += result.present.status == FrameStatus::Skip;
    if (result.frame.primary_ray_timestamp_available) {
      totals.gpu_ms += FrameGpuMs(result.frame);
      ++totals.gpu_frames;
    }
    totals.readback_bytes += ReadbackBytes(result.frame);
    totals.timings.Add(result.frame, result.scene_update, result.scene,
        static_cast<double>(totals.last_cpu_ns) / 1.0e6);
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
      window.SetTitle(Title(scene, totals, controls));
      title_update = now;
    }
  }

  if (!options.benchmark.empty()) {
    WriteBenchmark(options.benchmark, scene.source(), stage, totals,
        controls);
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
