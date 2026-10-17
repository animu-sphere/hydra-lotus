// SPDX-License-Identifier: Apache-2.0
// The built-in scene: the reference Cornell box, path-traced progressively
// and presented from the backend, seen through the free camera.
#include "frame_loop.hpp"
#include "orbit_camera.hpp"
#include "reference.hpp"
#include "scenes.hpp"

#include <lotus/extraction.hpp>
#include <lotus/render_world.hpp>
#include <lotus/vulkan_backend.hpp>

#include <iostream>
#include <memory>

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
  BuiltinScene(OffscreenRenderer& renderer, std::uint32_t samples)
      : renderer_(renderer) {
    LotusHeadless::SetCornellBox(world_);
    settings_.max_samples = samples;
    // The box spans [-1, 1]; the initial view is the headless bootstrap
    // camera's, three units in front of it with a 45 degree field of view.
    camera_.SetBounds(BoxCorners({-1.0, -1.0, -1.0}, {1.0, 1.0, 1.0}));
    home_.distance = 3.0;
    home_.vertical_fov = 45.0;
    camera_.SetView(home_);
  }

  std::string_view source() const override {
    return "Cornell box";
  }

  std::string Status() const override {
    return "free camera";
  }

  // The scene has no authored cameras, time samples or layers.
  void Handle(const Event& event) override {
    if (event.type == EventType::KeyDown && event.key == Key::Frame) {
      camera_.SetView(home_);
    } else {
      (void)navigator_.Handle(event, camera_, height_);
    }
  }

  bool Render(std::uint32_t width, std::uint32_t height, FrameResult& result,
      std::string& error) override {
    height_ = height;
    const double aspect =
        static_cast<double>(width) / static_cast<double>(height);
    world_.SetCamera({ToFloat(camera_.WorldToView()),
        ToFloat(camera_.Projection(aspect))});
    const FrameSnapshot snapshot = world_.Commit();
    const GpuSceneEvidence scene =
        renderer_.UpdateScene(extraction_.Update(snapshot));
    if (scene.status != FrameStatus::Pass) {
      error = "GPU scene update failed: " + scene.detail;
      return false;
    }
    OffscreenTarget target;
    target.width = width;
    target.height = height;
    target.clear_color = {0.0F, 0.0F, 0.0F, 1.0F};
    target.read_back = false;
    result.frame = renderer_.RenderScene(
        ExtractDrawSummary(snapshot), target, 1, settings_);
    if (result.frame.status != FrameStatus::Pass) {
      error = "scene frame failed: " + result.frame.detail;
      return false;
    }
    result.present = renderer_.Present(width, height);
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
  SceneExtraction extraction_;
  PathTracingSettings settings_;
  OrbitCamera camera_;
  OrbitView home_;
  Navigator navigator_;
  std::uint32_t height_ = 1;
};

} // namespace

int RunBuiltinScene(const Options& options,
    const std::filesystem::path& shader_directory) {
  std::unique_ptr<Window> window;
  PresentOptions presentation;
  presentation.vsync = options.vsync;
  try {
    window = Window::Create("lotus-viewport", options.width, options.height,
        options.visible, options.fullscreen);
    presentation.surface = MakeSurfaceProvider(*window);
  } catch (const std::exception& environment) {
    return Unavailable(options, environment.what());
  }

  FrameStatus status = FrameStatus::Fail;
  std::string error;
  const RendererShaders shaders = ShadersIn(shader_directory);
  const std::unique_ptr<OffscreenRenderer> renderer = CreateOffscreenRenderer(
      shaders.vertex, shaders.fragment, status, error, shaders.ray_query,
      &presentation);
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

  BuiltinScene scene(*renderer, options.samples);
  return RunFrameLoop(options, *window, scene, {});
}

} // namespace Lotus::viewport
