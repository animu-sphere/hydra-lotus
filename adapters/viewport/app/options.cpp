// SPDX-License-Identifier: Apache-2.0
#include "app/options.hpp"
#include "common/workloads.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Lotus::viewport {
namespace {

std::uint64_t ReadUnsigned(std::string_view value, std::string_view name,
    std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max()) {
  const auto reject = [&]() {
    return std::invalid_argument(std::string(name) +
                                 " must be an integer in [0, " +
                                 std::to_string(maximum) + "]");
  };
  // std::stoull would accept and wrap a leading minus sign.
  if (value.empty() || value.front() < '0' || value.front() > '9') {
    throw reject();
  }
  std::size_t consumed = 0;
  std::uint64_t result = 0;
  try {
    result = std::stoull(std::string(value), &consumed);
  } catch (const std::exception&) {
    throw reject();
  }
  if (consumed != value.size() || result > maximum) {
    throw reject();
  }
  return result;
}

double ReadTime(std::string_view value) {
  const auto reject = []() {
    return std::invalid_argument("--time must be a finite time code");
  };
  std::size_t consumed = 0;
  double result = 0.0;
  try {
    result = std::stod(std::string(value), &consumed);
  } catch (const std::exception&) {
    throw reject();
  }
  if (consumed != value.size() || !std::isfinite(result)) {
    throw reject();
  }
  return result;
}

} // namespace

Options ParseOptions(int argc, const char* const* argv) {
  constexpr std::uint64_t kMaxExtent = 16384;
  Options result;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option(argv[index]);
    const auto next = [&]() -> std::string_view {
      if (++index >= argc) {
        throw std::invalid_argument(std::string(option) + " requires a value");
      }
      return argv[index];
    };
    if (option == "--width") {
      result.width =
          static_cast<std::uint32_t>(ReadUnsigned(next(), option, kMaxExtent));
    } else if (option == "--height") {
      result.height =
          static_cast<std::uint32_t>(ReadUnsigned(next(), option, kMaxExtent));
    } else if (option == "--frames") {
      result.frame_limit = ReadUnsigned(next(), option);
    } else if (option == "--samples") {
      result.controls.samples = static_cast<std::uint32_t>(ReadUnsigned(
          next(), option, std::numeric_limits<std::int32_t>::max()));
      if (result.controls.samples == 0) {
        throw std::invalid_argument("--samples must be at least 1");
      }
    } else if (option == "--integrator") {
      const auto integrator = ParseIntegrator(next());
      if (!integrator) {
        throw std::invalid_argument(
            "--integrator must be reference or wavefront");
      }
      result.controls.integrator = *integrator;
    } else if (option == "--nee") {
      const auto value = next();
      if (value != "on" && value != "off") {
        throw std::invalid_argument("--nee must be on or off");
      }
      result.controls.nee = value == "on";
    } else if (option == "--restir") {
      const auto mode = ParseRestirMode(next());
      if (!mode) {
        throw std::invalid_argument("--restir must be off, initial, "
                                    "temporal, spatial or combined");
      }
      result.controls.restir = *mode;
    } else if (option == "--temporal-radiance") {
      result.controls.temporal_radiance = true;
    } else if (option == "--denoise") {
      result.controls.denoise = true;
    } else if (option == "--filter-iterations") {
      result.controls.filter_iterations = static_cast<std::uint32_t>(
          ReadUnsigned(next(), option, kTemporalFilterMaxIterations));
      if (result.controls.filter_iterations == 0) {
        throw std::invalid_argument("--filter-iterations must be at least 1");
      }
    } else if (option == "--no-accumulation") {
      result.controls.accumulate = false;
    } else if (option == "--view") {
      const auto view = ParseDebugView(next());
      if (!view) {
        throw std::invalid_argument(
            "--view must be beauty, depth, normal, motion, "
            "temporal-validity, raw, accumulated, history-length, variance, "
            "reservoir-validity, reservoir-age or reservoir-weight");
      }
      result.controls.view = *view;
    } else if (option == "--split") {
      result.controls.split = true;
    } else if (option == "--hidden") {
      result.visible = false;
    } else if (option == "--fullscreen") {
      result.fullscreen = true;
    } else if (option == "--vsync") {
      const auto value = next();
      if (value != "on" && value != "off") {
        throw std::invalid_argument("--vsync must be on or off");
      }
      result.vsync = value == "on";
    } else if (option == "--validate") {
      result.validate = true;
    } else if (option == "--resize-test") {
      result.resize_test = true;
    } else if (option == "--allow-unavailable") {
      result.allow_unavailable = true;
    } else if (option == "--interaction-test") {
      result.interaction_test = true;
    } else if (option == "--camera") {
      result.camera = std::string(next());
      if (result.camera.empty() || result.camera.front() != '/') {
        throw std::invalid_argument("--camera must be an absolute prim path");
      }
    } else if (option == "--time") {
      result.time = ReadTime(next());
    } else if (option == "--usd") {
      result.stage = std::filesystem::path(std::string(next()));
    } else if (option == "--benchmark") {
      result.benchmark = std::filesystem::path(std::string(next()));
    } else if (option == "--workload") {
      result.workload = std::string(next());
      if (!LotusHeadless::KnownWorkload(result.workload)) {
        throw std::invalid_argument("unknown workload: " + result.workload);
      }
    } else if (option == "--benchmark-warmup") {
      result.benchmark_warmup = static_cast<std::uint32_t>(ReadUnsigned(next(), option, 4096));
    } else if (option == "--help") {
      result.help = true;
    } else {
      throw std::invalid_argument("unknown option: " + std::string(option));
    }
  }
  if (result.width == 0 || result.height == 0) {
    throw std::invalid_argument("viewport extent must be non-zero");
  }
  if (!result.workload.empty() && (!result.stage.empty() || result.interaction_test || result.resize_test)) {
    throw std::invalid_argument("--workload excludes --usd, --interaction-test and --resize-test");
  }
  if (result.benchmark_warmup != 0 && (result.benchmark.empty() ||
      result.frame_limit <= result.benchmark_warmup || result.interaction_test || result.resize_test)) {
    throw std::invalid_argument("--benchmark-warmup needs --benchmark and more --frames than warmup, without interaction/resize tests");
  }
  if (result.fullscreen && !result.visible) {
    throw std::invalid_argument("--fullscreen and --hidden exclude each other");
  }
  if (result.resize_test && (result.frame_limit < 3 || result.fullscreen)) {
    throw std::invalid_argument(
        "--resize-test needs --frames 3 or more and a windowed viewport");
  }
  if (result.interaction_test &&
      (result.frame_limit < 3 || result.resize_test)) {
    throw std::invalid_argument("--interaction-test needs --frames 3 or more "
                                "and excludes --resize-test");
  }
  if ((!result.camera.empty() || result.time) && result.stage.empty()) {
    throw std::invalid_argument("--camera and --time need --usd");
  }
  if (!ViewAvailable(result.controls, result.controls.view)) {
    throw std::invalid_argument(
        "the reservoir views need --restir; the radiance views need "
        "--temporal-radiance or --denoise");
  }
  if (result.controls.split && result.controls.view == DebugView::Beauty) {
    throw std::invalid_argument("--split needs a --view other than beauty");
  }
  return result;
}

std::string Usage(bool hydra) {
  std::string usage =
      "Usage: lotus-viewport [options]\n"
      "  --usd <stage>            open a USD stage through Hydra";
  usage += hydra ? "\n" : " (not in this build)\n";
  usage +=
      "  --camera <prim>          look through the stage's camera at first\n"
      "  --time T                 the stage time code to open at\n"
      "  --width N --height N     window size (default 1280x720)\n"
      "  --fullscreen             start on the primary monitor\n"
      "  --samples N              samples per pixel to accumulate (default "
      "1024)\n"
      "  --integrator reference|wavefront\n"
      "                           the path tracer to start with\n"
      "  --nee on|off             next event estimation with MIS (default on)\n"
      "  --restir off|initial|temporal|spatial|combined\n"
      "                           reservoir direct lighting (default off)\n"
      "  --temporal-radiance      reuse each pixel's radiance history\n"
      "  --denoise                SVGF-filter the temporal radiance (turns\n"
      "                           it on)\n"
      "  --filter-iterations N    a-trous iterations, 1-8 (default 5)\n"
      "  --no-accumulation        one fresh sample per pixel every frame\n"
      "  --view <name>            beauty, depth, normal, motion,\n"
      "                           temporal-validity, raw, accumulated,\n"
      "                           history-length, variance,\n"
      "                           reservoir-validity, reservoir-age or\n"
      "                           reservoir-weight\n"
      "  --split                  the view left, beauty right\n"
      "  --vsync on|off           FIFO, or immediate/mailbox present\n"
      "  --frames N               exit after N presented frames\n"
      "  --hidden                 do not show the window\n"
      "  --validate               fail unless Vulkan validation is active\n"
      "  --resize-test            resize once and require a new swapchain\n"
      "  --interaction-test       script inputs and edits; check each restart\n"
      "  --allow-unavailable      exit 77, explained, without a device\n"
      "  --benchmark <file>       write the run's timings as JSON\n"
      "  --workload <id>          fixed headless fixture and authored camera\n"
      "  --benchmark-warmup N     discard N frames, reset the sample sequence\n"
      "Without --usd the viewport renders the built-in Cornell box.\n"
      "Left drag tumbles, middle (or Ctrl+left) drag tracks, right drag and\n"
      "the wheel dolly, F restores the initial framing. With a stage, C\n"
      "selects the next authored camera, Left/Right step the time code and\n"
      "R reloads the layers. F11 toggles fullscreen; Esc or closing the\n"
      "window exits. The Renderer window changes the settings above while\n"
      "the viewport runs; F1 hides and shows it, F2 switches the denoiser.\n";
  return usage;
}

} // namespace Lotus::viewport
