// SPDX-License-Identifier: Apache-2.0
// The built-in scene: the reference Cornell box, path-traced progressively
// and presented from the backend.
#include "camera.hpp"
#include "frame_loop.hpp"
#include "reference.hpp"
#include "scenes.hpp"

#include <lotus/extraction.hpp>
#include <lotus/render_world.hpp>
#include <lotus/vulkan_backend.hpp>

#include <iostream>
#include <memory>

namespace Lotus::viewport {

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
  const auto shader = [&](const char* name) {
    return (shader_directory / name).string();
  };
  const std::unique_ptr<OffscreenRenderer> renderer = CreateOffscreenRenderer(
      shader("triangle.vert.spv"), shader("triangle.frag.spv"), status, error,
      {shader("path_trace.vert.spv"), shader("path_trace.frag.spv"),
          shader("wavefront.comp.spv")},
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

  RenderWorld world;
  LotusHeadless::SetCornellBox(world);
  SceneExtraction extraction;
  PathTracingSettings settings;
  settings.max_samples = options.samples;
  return RunFrameLoop(options, *window, "Cornell box", {},
      [&](std::uint32_t width, std::uint32_t height, FrameResult& result,
          std::string& frame_error) {
        world.SetCamera(BootstrapCamera(width, height));
        const FrameSnapshot snapshot = world.Commit();
        const GpuSceneEvidence scene =
            renderer->UpdateScene(extraction.Update(snapshot));
        if (scene.status != FrameStatus::Pass) {
          frame_error = "GPU scene update failed: " + scene.detail;
          return false;
        }
        OffscreenTarget target;
        target.width = width;
        target.height = height;
        target.clear_color = {0.0F, 0.0F, 0.0F, 1.0F};
        target.read_back = false;
        result.frame = renderer->RenderScene(
            ExtractDrawSummary(snapshot), target, 1, settings);
        if (result.frame.status != FrameStatus::Pass) {
          frame_error = "scene frame failed: " + result.frame.detail;
          return false;
        }
        result.present = renderer->Present(width, height);
        return true;
      });
}

} // namespace Lotus::viewport
