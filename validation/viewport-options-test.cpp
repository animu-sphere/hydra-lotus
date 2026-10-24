// SPDX-License-Identifier: Apache-2.0
// lotus-viewport's command line, without a window or a GPU.
#include "../adapters/viewport/options.hpp"

#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

Lotus::viewport::Options Parse(std::initializer_list<const char*> arguments) {
  std::vector<const char*> argv{"lotus-viewport"};
  argv.insert(argv.end(), arguments);
  return Lotus::viewport::ParseOptions(static_cast<int>(argv.size()),
      argv.data());
}

bool Rejects(std::initializer_list<const char*> arguments) {
  try {
    (void)Parse(arguments);
  } catch (const std::invalid_argument&) {
    return true;
  }
  return false;
}

int failures = 0;

void Check(bool condition, const std::string& what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
  }
}

} // namespace

int main() {
  const auto defaults = Parse({});
  Check(defaults.width == 1280 && defaults.height == 720, "default extent");
  Check(defaults.frame_limit == 0 && defaults.controls.samples == 1024,
      "default frame limit and samples");
  Check(defaults.visible && defaults.vsync && !defaults.fullscreen &&
            !defaults.validate && !defaults.resize_test &&
            !defaults.allow_unavailable && defaults.stage.empty() &&
            defaults.benchmark.empty() && !defaults.help &&
            !defaults.interaction_test && defaults.camera.empty() &&
            !defaults.time,
      "default flags");

  const auto smoke = Parse({"--hidden", "--frames", "8", "--vsync", "off",
      "--validate", "--resize-test", "--allow-unavailable", "--usd",
      "stage.usda", "--benchmark", "out/run.json", "--samples", "16",
      "--width", "320", "--height", "200"});
  Check(!smoke.visible && smoke.frame_limit == 8 && !smoke.vsync &&
            smoke.validate && smoke.resize_test && smoke.allow_unavailable,
      "smoke-test flags");
  Check(smoke.stage == "stage.usda" && smoke.benchmark == "out/run.json",
      "stage and benchmark paths");
  Check(smoke.controls.samples == 16 && smoke.width == 320 && smoke.height == 200,
      "samples and extent");
  const auto stage = Parse({"--usd", "stage.usda", "--camera", "/World/Cam",
      "--time", "12.5", "--frames", "16", "--interaction-test"});
  Check(stage.camera == "/World/Cam" && stage.time == 12.5 &&
            stage.interaction_test && stage.frame_limit == 16,
      "camera, time and interaction test");
  Check(Parse({"--usd", "s.usda", "--time", "-3"}).time == -3.0,
      "negative time code");
  using Lotus::DebugView;
  using Lotus::viewport::RestirMode;
  const Lotus::viewport::Controls& initial = defaults.controls;
  Check(initial.integrator == Lotus::Integrator::Reference && initial.nee &&
            initial.restir == RestirMode::Off && initial.accumulate &&
            !initial.temporal_radiance && initial.view == DebugView::Beauty,
      "default renderer controls");
  const auto controls = Parse({"--integrator", "wavefront", "--nee", "off",
      "--restir", "combined", "--temporal-radiance", "--no-accumulation",
      "--view", "reservoir-age"});
  Check(controls.controls.integrator == Lotus::Integrator::Wavefront &&
            !controls.controls.nee &&
            controls.controls.restir == RestirMode::Combined &&
            controls.controls.temporal_radiance &&
            !controls.controls.accumulate &&
            controls.controls.view == DebugView::ReservoirAge,
      "renderer controls");
  Check(Parse({"--view", "temporal-validity"}).controls.view ==
            DebugView::TemporalValidity,
      "a temporal view without reuse");
  Check(!initial.denoise && !initial.split &&
            initial.filter_iterations == Lotus::kTemporalFilterIterations,
      "the denoiser is off by default");
  const auto denoise = Parse({"--denoise", "--filter-iterations", "3",
      "--view", "variance", "--split"});
  Check(denoise.controls.denoise && denoise.controls.filter_iterations == 3 &&
            denoise.controls.view == DebugView::Variance &&
            denoise.controls.split,
      "denoiser controls");
  Check(Parse({"--temporal-radiance", "--view", "raw"}).controls.view ==
            DebugView::RawRadiance,
      "a radiance view with temporal radiance");
  Check(Parse({"--fullscreen"}).fullscreen, "fullscreen");
  Check(Parse({"--help"}).help, "help");
  const auto fixture = Parse({"--workload", "many-lights-v1", "--benchmark", "out.json",
      "--benchmark-warmup", "8", "--frames", "24"});
  Check(fixture.workload == "many-lights-v1" && fixture.benchmark_warmup == 8,
      "shared fixture and measured warmup");
  Check(Rejects({"--workload", "unknown"}), "unknown fixture");
  Check(Rejects({"--workload", "cornell-v1", "--usd", "s.usda"}), "fixture with USD");
  Check(Rejects({"--benchmark-warmup", "8", "--frames", "16"}), "warmup without report");
  Check(Rejects({"--benchmark-warmup", "8", "--frames", "8", "--benchmark", "out.json"}),
      "warmup without measured frames");

  Check(Rejects({"--frames"}), "missing value");
  Check(Rejects({"--frames", "-1"}), "negative frame count");
  Check(Rejects({"--frames", "8x"}), "trailing characters");
  Check(Rejects({"--width", "0"}), "zero width");
  Check(Rejects({"--height", "16385"}), "oversized height");
  Check(Rejects({"--samples", "0"}), "zero samples");
  Check(Rejects({"--vsync", "maybe"}), "vsync value");
  Check(Rejects({"--fullscreen", "--hidden"}), "hidden fullscreen");
  Check(Rejects({"--resize-test", "--frames", "2"}),
      "resize test without a third frame");
  Check(Rejects({"--resize-test"}), "resize test without a frame limit");
  Check(Rejects({"--interaction-test", "--frames", "2"}),
      "interaction test without a third frame");
  Check(Rejects({"--interaction-test", "--resize-test", "--frames", "8"}),
      "interaction and resize tests together");
  Check(Rejects({"--camera", "/Cam"}), "camera without a stage");
  Check(Rejects({"--time", "1"}), "time without a stage");
  Check(Rejects({"--usd", "s.usda", "--camera", "Cam"}),
      "relative camera path");
  Check(Rejects({"--usd", "s.usda", "--time", "1x"}),
      "time with trailing characters");
  Check(Rejects({"--usd", "s.usda", "--time", "inf"}), "infinite time");
  Check(Rejects({"--integrator", "megakernel"}), "integrator name");
  Check(Rejects({"--nee", "maybe"}), "nee value");
  Check(Rejects({"--restir", "sometimes"}), "restir mode");
  Check(Rejects({"--view", "albedo"}), "view name");
  Check(Rejects({"--view", "reservoir-weight"}),
      "a reservoir view without reservoirs");
  Check(Rejects({"--view", "history-length"}),
      "a radiance view without temporal radiance");
  Check(Rejects({"--filter-iterations", "0"}), "zero filter iterations");
  Check(Rejects({"--filter-iterations", "9"}), "too many filter iterations");
  Check(Rejects({"--split"}), "a split without a view");
  Check(Rejects({"--bogus"}), "unknown option");
  Check(Lotus::viewport::Usage(true).find("--usd <stage>") !=
            std::string::npos,
      "usage names --usd");
  return failures == 0 ? 0 : 1;
}
