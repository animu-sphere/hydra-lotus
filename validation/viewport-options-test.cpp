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
  Check(defaults.frame_limit == 0 && defaults.samples == 1024,
      "default frame limit and samples");
  Check(defaults.visible && defaults.vsync && !defaults.fullscreen &&
            !defaults.validate && !defaults.resize_test &&
            !defaults.allow_unavailable && defaults.stage.empty() &&
            defaults.benchmark.empty() && !defaults.help,
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
  Check(smoke.samples == 16 && smoke.width == 320 && smoke.height == 200,
      "samples and extent");
  Check(Parse({"--fullscreen"}).fullscreen, "fullscreen");
  Check(Parse({"--help"}).help, "help");

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
  Check(Rejects({"--bogus"}), "unknown option");
  Check(Lotus::viewport::Usage(true).find("--usd <stage>") !=
            std::string::npos,
      "usage names --usd");
  return failures == 0 ? 0 : 1;
}
