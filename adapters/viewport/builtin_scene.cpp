// SPDX-License-Identifier: Apache-2.0
// The built-in scene: the reference Cornell box, path-traced as the
// renderer controls plan it and presented from the backend, seen through
// the free camera.
#include "frame_loop.hpp"
#include "orbit_camera.hpp"
#include "reference.hpp"
#include "../headless/workloads.hpp"
#include "scenes.hpp"

#include <lotus/extraction.hpp>
#include <lotus/render_world.hpp>
#include <lotus/vulkan_backend.hpp>

#include <array>
#include <iostream>
#include <memory>
#include <optional>

namespace Lotus::viewport {
namespace {

Matrix4 ToFloat(const Matrix4d& matrix) {
  Matrix4 result{};
  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index] = static_cast<float>(matrix[index]);
  }
  return result;
}

class BuiltinScene final : public Scene {
public:
  explicit BuiltinScene(OffscreenRenderer& renderer, const Options& options,
      const std::filesystem::path& assets) : renderer_(renderer), workload_(options.workload) {
    world_ = LotusHeadless::MakeWorkload(workload_.empty() ? "cornell-v1" : workload_, assets).world;
    // The box spans [-1, 1]; the initial view is the headless bootstrap
    // camera's, three units in front of it with a 45 degree field of view.
    camera_.SetBounds(BoxCorners({-1.0, -1.0, -1.0}, {1.0, 1.0, 1.0}));
    home_.distance = 3.0;
    home_.vertical_fov = 45.0;
    camera_.SetView(home_);
  }

  std::string_view source() const override {
    return workload_.empty() ? std::string_view("Cornell box") : std::string_view(workload_);
  }

  std::string Status() const override {
    return workload_.empty() ? "free camera" : "fixed benchmark camera";
  }

  // The scene has no authored cameras, time samples or layers.
  void Handle(const Event& event) override {
    if (!workload_.empty()) return;
    if (event.type == EventType::KeyDown && event.key == Key::Frame) {
      camera_.SetView(home_);
    } else {
      (void)navigator_.Handle(event, camera_, height_);
    }
  }

  std::array<float, 2> DepthRange() const override {
    const std::array<double, 2> range = camera_.ClippingRange();
    return {static_cast<float>(range[0]), static_cast<float>(range[1])};
  }

  bool Render(std::uint32_t width, std::uint32_t height,
      const FramePlan& plan, FrameResult& result,
      std::string& error) override {
    height_ = height;
    const double aspect =
        static_cast<double>(width) / static_cast<double>(height);
    if (workload_.empty()) {
      world_.SetCamera({ToFloat(camera_.WorldToView()),
          ToFloat(camera_.Projection(aspect))});
    }
    const FrameSnapshot snapshot = world_.Commit();
    const GpuSceneEvidence scene =
        renderer_.UpdateScene(extraction_.Update(snapshot));
    if (scene.status != FrameStatus::Pass) {
      error = "GPU scene update failed: " + scene.detail;
      return false;
    }
    result.scene_update = scene.timings;
    result.scene = scene.stats;
    OffscreenTarget target;
    target.width = width;
    target.height = height;
    target.clear_color = {0.0F, 0.0F, 0.0F, 1.0F};
    target.read_back = false;
    if (plan.temporal_frame) {
      // A frame index after a gap starts the history again, as an ordinary
      // frame in between does.
      TemporalFrameSettings temporal = plan.temporal;
      temporal.frame_index = ++frame_index_;
      result.frame = renderer_.RenderTemporalScene(snapshot, target, 1,
          temporal, plan.settings);
    } else {
      ++frame_index_;
      result.frame = renderer_.RenderScene(
          ExtractDrawSummary(snapshot), target, 1, plan.settings);
    }
    if (result.frame.status != FrameStatus::Pass) {
      error = "scene frame failed: " + result.frame.detail;
      return false;
    }
    result.present = renderer_.Present(width, height, plan.present);
    return true;
  }

  std::vector<InteractionStep> InteractionSteps() override {
    std::vector<InteractionStep> steps = NavigationSteps(*this);
    steps.push_back({"next camera (C) without authored cameras", false,
        [this](bool&, std::string&) {
          Press(*this, Key::NextCamera);
          return true;
        }});
    return steps;
  }

private:
  OffscreenRenderer& renderer_;
  RenderWorld world_;
  std::string workload_;
  SceneExtraction extraction_;
  std::uint64_t frame_index_ = 0;
  OrbitCamera camera_;
  OrbitView home_;
  Navigator navigator_;
  std::uint32_t height_ = 1;
};

} // namespace

int RunBuiltinScene(const Options& options, ViewportWindow& window,
    const std::filesystem::path& shader_directory) {
  // Destroyed before the window and its developer UI.
  std::string error;
  FrameStatus status = FrameStatus::Fail;
  const RendererShaders shaders = ShadersIn(shader_directory);
  const std::unique_ptr<OffscreenRenderer> renderer = CreateOffscreenRenderer(
      shaders.vertex, shaders.fragment, status, error, shaders.ray_query,
      &window.presentation);
  if (!renderer) {
    if (status == FrameStatus::Skip) {
      return Unavailable(options, error);
    }
    std::cerr << "lotus-viewport: " << error << '\n';
    return 1;
  }
  if (const BackendCapability ray_query = renderer->RayQueryCapability();
      !ray_query.available) {
    return Unavailable(options, ray_query.detail);
  }

  BuiltinScene scene(*renderer, options, shader_directory.parent_path() / "assets");
  return RunFrameLoop(options, window, scene, {});
}

} // namespace Lotus::viewport
