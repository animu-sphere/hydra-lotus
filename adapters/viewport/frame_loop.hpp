// SPDX-License-Identifier: Apache-2.0
// The present loop that the built-in scene and USD stages share: window
// events, fullscreen, the developer UI and the renderer controls, the resize
// and interaction tests, the frame limit, the title, the timing HUD's record,
// the run's checks and its benchmark JSON.
#pragma once

#include "controls.hpp"
#include "developer_ui.hpp"
#include "options.hpp"
#include "window.hpp"

#include <lotus/vulkan_backend.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Lotus::viewport {

inline constexpr int kExitSkip = 77;

// One rendered frame. `frame` may lack its colour and depth payloads; its
// product extents say whether they were read back. `scene_update` times the
// GPU scene update before the frame; `scene` is the GPU scene after it.
struct FrameResult {
  GpuFrameEvidence frame;
  PresentEvidence present;
  GpuSceneTimings scene_update;
  GpuSceneStats scene;
};

// One step of the scripted interaction test: an input, a scene edit or a
// change of the renderer controls, and whether the next frame must restart
// the accumulation (1 sample per pixel) or continue it (one sample more, up
// to the controls' sample limit). `apply` may revise `restarts` when the
// scene decides it, and fails the run by returning false with `error`.
struct InteractionStep {
  std::string name;
  bool restarts = true;
  std::function<bool(bool& restarts, std::string& error)> apply;
};

class Scene {
public:
  virtual ~Scene() = default;

  // What the run renders, for the title, the summary and the benchmark.
  [[nodiscard]] virtual std::string_view source() const = 0;
  // The camera and stage time, for the title.
  [[nodiscard]] virtual std::string Status() const = 0;
  // An input the loop does not handle itself (navigation and scene keys).
  virtual void Handle(const Event& event) = 0;
  // The camera's clipping range, which the depth view spans.
  [[nodiscard]] virtual std::array<float, 2> DepthRange() const = 0;
  // Renders `plan` and presents it at the window's framebuffer extent.
  // False, with `error`, ends the run as a failure.
  virtual bool Render(std::uint32_t width, std::uint32_t height,
      const FramePlan& plan, FrameResult& result, std::string& error) = 0;
  // The interaction test's steps, in order.
  [[nodiscard]] virtual std::vector<InteractionStep> InteractionSteps() = 0;
};

// Synthetic inputs through Scene::Handle, as the window delivers them: a
// drag by a pixel offset, wheel notches, a key press.
void Drag(Scene& scene, Button button, double delta_x, double delta_y);
void Wheel(Scene& scene, double notches);
void Press(Scene& scene, Key key);

// Tumble, track, dolly and frame, each of which restarts the accumulation.
[[nodiscard]] std::vector<InteractionStep> NavigationSteps(Scene& scene);

// The window, its developer UI and the presentation to create the renderer
// with. Members go in reverse: the UI before the window it uses.
//
// A Hydra-enabled build sets `open_stage`: it opens a stage the developer UI
// picked, or says why it cannot. Once it has, the frame loop ends with
// `next_stage` set, and the caller renders that stage in the same window,
// with the renderer controls as the previous scene left them.
struct ViewportWindow {
  std::unique_ptr<Window> window;
  std::unique_ptr<DeveloperUi> ui;
  PresentOptions presentation;
  Controls controls;
  std::function<bool(const std::filesystem::path& stage, std::string& error)>
      open_stage;
  std::optional<std::filesystem::path> next_stage;
};

// Nothing, with `reason`, when this environment has no window to present
// to.
[[nodiscard]] std::optional<ViewportWindow> OpenWindow(const Options& options,
    std::string& reason);

// Returns the process exit code. `stage` is empty for the built-in scene.
// The run starts with the window's controls; its UI edits them.
[[nodiscard]] int RunFrameLoop(const Options& options, ViewportWindow& window,
    Scene& scene, const std::string& stage);

// Reports an environment that cannot present: exit 77 with
// --allow-unavailable, otherwise 1. The explanation always goes to stdout as
// "SKIP: <reason>", as the renderer's other entry points print it.
[[nodiscard]] int Unavailable(const Options& options, std::string_view reason);

} // namespace Lotus::viewport
