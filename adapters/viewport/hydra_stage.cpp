// SPDX-License-Identifier: Apache-2.0
// A USD stage through the Hydra adapter, driven in process as
// hydra-merlin's viewport does: the render delegate is created directly,
// UsdImaging populates its render index, and HdEngine runs one task whose
// render pass presents from the backend. Only the Hydra-enabled build
// (LOTUS_ENABLE_HYDRA2) compiles this file.
#include "frame_loop.hpp"
#include "scenes.hpp"

#include "adapter.hpp"

#include <pxr/base/gf/bbox3d.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/repr.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/bboxCache.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usdImaging/usdImaging/delegate.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

namespace Lotus::viewport {
namespace {

// usdview's initial free camera: the stage's render and proxy bounds, framed
// along -Z from the authored up axis with a 60 degree vertical field of view
// and a 1.1 fit margin, and clipping planes around the bounds.
class FramedPassState final : public HdRenderPassState {
public:
  GfMatrix4d GetWorldToViewMatrix() const override {
    return view_;
  }
  GfMatrix4d GetProjectionMatrix() const override {
    return projection_;
  }

  void Frame(const UsdStageRefPtr& stage) {
    const TfTokenVector purposes{UsdGeomTokens->default_,
        UsdGeomTokens->proxy, UsdGeomTokens->render};
    UsdGeomBBoxCache cache(UsdTimeCode::Default(), purposes, true);
    bounds_ = cache.ComputeWorldBound(stage->GetPseudoRoot());
    const GfRange3d range = bounds_.ComputeAlignedRange();
    if (range.IsEmpty()) {
      throw std::runtime_error("the USD stage has no renderable bounds");
    }
    const GfVec3d size = range.GetSize();
    size_ = std::max({size[0], size[1], size[2]});
    if (!std::isfinite(size_) || size_ <= 0.0) {
      size_ = 1.0;
    }
    const GfVec3d center = bounds_.ComputeCentroid();
    const double half_fov = kVerticalFovDegrees * std::numbers::pi / 360.0;
    const double fit = size_ * kFrameFit * 0.5;
    // usdview's FreeCamera.frameSelection, with its one-unit guard.
    double distance = fit / std::atan(half_fov);
    if (distance < 1.0 + size_ * 0.5) {
      distance = 1.0 + fit;
    }
    GfMatrix4d up(1.0);
    if (UsdGeomGetStageUpAxis(stage) == UsdGeomTokens->z) {
      up.SetRotate(GfRotation(GfVec3d(1.0, 0.0, 0.0), 90.0));
    }
    GfMatrix4d back(1.0);
    back.SetTranslate(GfVec3d(0.0, 0.0, distance));
    GfMatrix4d to_center(1.0);
    to_center.SetTranslate(center);
    camera_ = back * up * to_center;
    view_ = camera_.GetInverse();
    Clip();
  }

  void Resize(std::uint32_t width, std::uint32_t height) {
    SetViewport(GfVec4d(0.0, 0.0, width, height));
    GfFrustum frustum;
    frustum.SetPerspective(kVerticalFovDegrees,
        static_cast<double>(width) / static_cast<double>(height), near_, far_);
    projection_ = frustum.ComputeProjectionMatrix();
  }

private:
  static constexpr double kVerticalFovDegrees = 60.0;
  static constexpr double kFrameFit = 1.1;

  // The bounds' corners' depths along the view direction.
  void Clip() {
    const GfVec3d position = camera_.Transform(GfVec3d(0.0));
    GfVec3d direction = camera_.TransformDir(GfVec3d(0.0, 0.0, -1.0));
    direction.Normalize();
    const GfRange3d& range = bounds_.GetRange();
    const GfMatrix4d& matrix = bounds_.GetMatrix();
    double nearest = std::numeric_limits<double>::max();
    double farthest = 0.0;
    for (std::size_t corner = 0; corner < 8; ++corner) {
      const double depth = std::max(0.0,
          GfDot(matrix.Transform(range.GetCorner(corner)) - position,
              direction));
      nearest = std::min(nearest, depth);
      farthest = std::max(farthest, depth);
    }
    near_ = nearest == 0.0 ? std::min(1.0, farthest * 0.01) : nearest * 0.99;
    near_ = std::max(near_, size_ * 1.0e-7);
    far_ = std::max(near_ + 1.0, farthest * 1.01);
  }

  GfBBox3d bounds_;
  GfMatrix4d camera_{1.0};
  GfMatrix4d view_{1.0};
  GfMatrix4d projection_{1.0};
  double size_ = 1.0;
  double near_ = 0.1;
  double far_ = 1000.0;
};

class ViewportTask final : public HdTask {
public:
  ViewportTask(HdRenderPassSharedPtr pass,
      std::shared_ptr<FramedPassState> state,
      HdResourceRegistrySharedPtr resources)
      : HdTask(SdfPath("/__lotusViewportTask")), pass_(std::move(pass)),
        state_(std::move(state)), resources_(std::move(resources)) {
  }

  void Sync(HdSceneDelegate*, HdTaskContext*, HdDirtyBits* dirty) override {
    pass_->Sync();
    *dirty = HdChangeTracker::Clean;
  }

  void Prepare(HdTaskContext*, HdRenderIndex*) override {
    state_->Prepare(resources_);
  }

  void Execute(HdTaskContext*) override {
    pass_->Execute(state_, render_tags_);
  }

  const TfTokenVector& GetRenderTags() const override {
    return render_tags_;
  }

private:
  HdRenderPassSharedPtr pass_;
  std::shared_ptr<FramedPassState> state_;
  HdResourceRegistrySharedPtr resources_;
  TfTokenVector render_tags_{HdRenderTagTokens->geometry};
};

std::string Utf8(const std::filesystem::path& path) {
  const auto utf8 = path.generic_u8string();
  return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

} // namespace

int RunHydraStage(const Options& options) {
  const std::string stage_path = Utf8(options.stage);
  const UsdStageRefPtr stage = UsdStage::Open(stage_path);
  if (!stage) {
    std::cerr << "lotus-viewport: could not open the USD stage " << stage_path
              << '\n';
    return 1;
  }
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

  HdRenderSettingsMap settings;
  settings[HdRenderSettingsTokens->convergedSamplesPerPixel] =
      VtValue(static_cast<int>(options.samples));
  HdLotusRenderDelegate render_delegate(settings);
  render_delegate.SetPresentation(presentation);
  const std::unique_ptr<HdRenderIndex> render_index(
      HdRenderIndex::New(&render_delegate, HdDriverVector{}));
  if (!render_index) {
    std::cerr << "lotus-viewport: could not create the Hydra render index\n";
    return 1;
  }
  UsdImagingDelegate scene_delegate(render_index.get(),
      SdfPath::AbsoluteRootPath());
  scene_delegate.Populate(stage->GetPseudoRoot());
  scene_delegate.SetTime(UsdTimeCode::Default());

  const HdRprimCollection collection(HdTokens->geometry,
      HdReprSelector(HdReprTokens->smoothHull));
  const HdRenderPassSharedPtr pass =
      render_delegate.CreateRenderPass(render_index.get(), collection);
  auto state = std::make_shared<FramedPassState>();
  state->Frame(stage);
  // One colour binding without a render buffer: the delegate presents.
  HdRenderPassAovBinding color;
  color.aovName = HdAovTokens->color;
  color.clearValue = VtValue(GfVec4f(0.0F, 0.0F, 0.0F, 1.0F));
  state->SetAovBindings({color});
  HdTaskSharedPtrVector tasks{std::make_shared<ViewportTask>(
      pass, state, render_delegate.GetResourceRegistry())};
  HdEngine engine;

  std::cout << "USD stage: " << stage_path << '\n';
  std::uint32_t framed_width = 0;
  std::uint32_t framed_height = 0;
  bool unavailable = false;
  std::string unavailable_reason;
  const int result = RunFrameLoop(options, *window, "Hydra", stage_path,
      [&](std::uint32_t width, std::uint32_t height, FrameResult& frame,
          std::string& error) {
        if (width != framed_width || height != framed_height) {
          state->Resize(width, height);
          framed_width = width;
          framed_height = height;
        }
        engine.Execute(render_index.get(), &tasks);
        frame.frame = render_delegate.GetFrameEvidence();
        frame.present = render_delegate.GetPresentEvidence();
        // The delegate reports why it could not render through the evidence;
        // its Tf diagnostics say the same.
        if (frame.frame.device_name.empty() &&
            frame.frame.status == FrameStatus::Skip) {
          unavailable = true;
          unavailable_reason = frame.frame.detail;
          error = "the renderer is unavailable: " + frame.frame.detail;
          return false;
        }
        if (!frame.frame.ray_query_used) {
          unavailable = true;
          unavailable_reason = frame.present.detail;
          error = frame.present.detail;
          return false;
        }
        if (frame.frame.status != FrameStatus::Pass) {
          error = "Hydra frame failed: " + frame.frame.detail;
          return false;
        }
        return true;
      });
  if (unavailable) {
    return Unavailable(options, unavailable_reason);
  }
  return result;
}

} // namespace Lotus::viewport
