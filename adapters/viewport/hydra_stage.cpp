// SPDX-License-Identifier: Apache-2.0
// A USD stage through the Hydra adapter, driven in process as
// hydra-merlin's viewport does: the render delegate is created directly,
// UsdImaging populates its render index, and HdEngine runs one task whose
// render pass presents from the backend. The view is the free camera or an
// authored camera prim; every frame applies the stage's pending edits at the
// current time code, and the renderer controls through the delegate's
// render settings. Only the Hydra-enabled build (LOTUS_ENABLE_HYDRA2)
// compiles this file.
#include "frame_loop.hpp"
#include "orbit_camera.hpp"
#include "scenes.hpp"

#include "adapter.hpp"

#include <pxr/base/gf/bbox3d.h>
#include <pxr/base/gf/range1f.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/repr.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/bboxCache.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usdImaging/usdImaging/delegate.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <iterator>
#include <memory>
#include <numbers>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

PXR_NAMESPACE_USING_DIRECTIVE

namespace Lotus::viewport {
namespace {

GfMatrix4d ToGf(const Matrix4d& matrix) {
  GfMatrix4d result;
  std::copy(matrix.begin(), matrix.end(), result.data());
  return result;
}

Matrix4d FromGf(const GfMatrix4d& matrix) {
  Matrix4d result{};
  std::copy(matrix.data(), matrix.data() + 16, result.begin());
  return result;
}

// The free camera's matrices, or, while an authored camera is set, the base
// class's: its view, conformed to the viewport by its window policy.
class StagePassState final : public HdRenderPassState {
public:
  GfMatrix4d GetWorldToViewMatrix() const override {
    return GetCamera() ? HdRenderPassState::GetWorldToViewMatrix() : view_;
  }
  GfMatrix4d GetProjectionMatrix() const override {
    return GetCamera() ? HdRenderPassState::GetProjectionMatrix()
                       : projection_;
  }

  void SetFreeCamera(const GfMatrix4d& view, const GfMatrix4d& projection) {
    view_ = view;
    projection_ = projection;
  }

private:
  GfMatrix4d view_{1.0};
  GfMatrix4d projection_{1.0};
};

class ViewportTask final : public HdTask {
public:
  ViewportTask(HdRenderPassSharedPtr pass,
      std::shared_ptr<StagePassState> state,
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
  std::shared_ptr<StagePassState> state_;
  HdResourceRegistrySharedPtr resources_;
  TfTokenVector render_tags_{HdRenderTagTokens->geometry};
};

std::string Utf8(const std::filesystem::path& path) {
  const auto utf8 = path.generic_u8string();
  return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

std::string TimeText(UsdTimeCode time) {
  if (time.IsDefault()) {
    return "default";
  }
  std::ostringstream text;
  text << time.GetValue();
  return text.str();
}

// The stage's camera prims, in traversal order.
SdfPathVector StageCameras(const UsdStageRefPtr& stage) {
  SdfPathVector cameras;
  for (const UsdPrim& prim : stage->Traverse()) {
    if (prim.IsA<UsdGeomCamera>()) {
      cameras.push_back(prim.GetPath());
    }
  }
  return cameras;
}

bool MightBeTimeVarying(const UsdStageRefPtr& stage) {
  for (const UsdPrim& prim : stage->Traverse()) {
    for (const UsdAttribute& attribute : prim.GetAttributes()) {
      if (attribute.ValueMightBeTimeVarying()) {
        return true;
      }
    }
  }
  return false;
}

// The interaction test's session-layer prim.
const SdfPath kInteractionPrim("/__lotusViewportInteraction");

class HydraScene final : public Scene {
public:
  HydraScene(const Options& options, UsdStageRefPtr stage,
      const PresentOptions& presentation)
      : stage_(std::move(stage)), render_delegate_(Settings(options)) {
    render_delegate_.SetPresentation(presentation);
    render_index_.reset(
        HdRenderIndex::New(&render_delegate_, HdDriverVector{}));
    if (!render_index_) {
      throw std::runtime_error("could not create the Hydra render index");
    }
    scene_delegate_ = std::make_unique<UsdImagingDelegate>(
        render_index_.get(), SdfPath::AbsoluteRootPath());
    scene_delegate_->Populate(stage_->GetPseudoRoot());
    time_ = options.time ? UsdTimeCode(*options.time)
            : stage_->HasAuthoredTimeCodeRange()
                ? UsdTimeCode(stage_->GetStartTimeCode())
                : UsdTimeCode::Default();
    scene_delegate_->SetTime(time_);

    const HdRprimCollection collection(HdTokens->geometry,
        HdReprSelector(HdReprTokens->smoothHull));
    const HdRenderPassSharedPtr pass =
        render_delegate_.CreateRenderPass(render_index_.get(), collection);
    state_ = std::make_shared<StagePassState>();
    // One colour binding without a render buffer: the delegate presents.
    HdRenderPassAovBinding color;
    color.aovName = HdAovTokens->color;
    color.clearValue = VtValue(GfVec4f(0.0F, 0.0F, 0.0F, 1.0F));
    state_->SetAovBindings({color});
    tasks_ = {std::make_shared<ViewportTask>(
        pass, state_, render_delegate_.GetResourceRegistry())};

    camera_.SetUpAxis(UsdGeomGetStageUpAxis(stage_) == UsdGeomTokens->z
                          ? UpAxis::Z
                          : UpAxis::Y);
    if (!UpdateBounds()) {
      throw std::runtime_error("the USD stage has no renderable bounds");
    }
    camera_.SetView(camera_.Framed());
    if (!options.camera.empty()) {
      const SdfPath path(options.camera);
      const UsdPrim prim = stage_->GetPrimAtPath(path);
      if (!prim || !prim.IsA<UsdGeomCamera>()) {
        throw std::runtime_error(options.camera + " is not a camera prim");
      }
      selected_ = path;
    }
  }

  std::string_view source() const override {
    return "Hydra";
  }

  std::string Status() const override {
    return (selected_.IsEmpty() ? std::string("free camera")
                                : selected_.GetString()) +
           " | time " + TimeText(time_);
  }

  void Handle(const Event& event) override {
    if (event.type == EventType::KeyDown) {
      HandleKey(event.key);
      return;
    }
    if (Navigator::Begins(event) && !selected_.IsEmpty()) {
      std::string error;
      if (!Adopt(error)) {
        std::cerr << "lotus-viewport: " << error << '\n';
      }
    }
    (void)navigator_.Handle(event, camera_, height_);
  }

  std::array<float, 2> DepthRange() const override {
    if (const HdCamera* camera = SelectedCamera()) {
      const GfRange1f range = camera->GetClippingRange();
      return {range.GetMin(), range.GetMax()};
    }
    const std::array<double, 2> range = camera_.ClippingRange();
    return {static_cast<float>(range[0]), static_cast<float>(range[1])};
  }

  bool Render(std::uint32_t width, std::uint32_t height,
      const FramePlan& plan, FrameResult& frame,
      std::string& error) override {
    ApplyPlan(plan);
    // Applies the stage's pending edits, then moves to the time code.
    scene_delegate_->SetTime(time_);
    if (width != width_ || height != height_) {
      state_->SetViewport(GfVec4d(0.0, 0.0, width, height));
      width_ = width;
      height_ = height;
    }
    const HdCamera* camera = SelectedCamera();
    if (!selected_.IsEmpty() && camera == nullptr) {
      std::cout << "Camera " << selected_ << " is gone; using the free "
                << "camera\n";
      selected_ = SdfPath();
    }
    state_->SetCamera(camera);
    if (camera == nullptr) {
      state_->SetFreeCamera(ToGf(camera_.WorldToView()),
          ToGf(camera_.Projection(Aspect())));
    }
    engine_.Execute(render_index_.get(), &tasks_);
    frame.frame = render_delegate_.GetFrameEvidence();
    frame.present = render_delegate_.GetPresentEvidence();
    // The delegate reports why it could not render through the evidence;
    // its Tf diagnostics say the same. A failed frame is a failure, not an
    // environment that cannot render.
    if (frame.frame.status == FrameStatus::Fail) {
      error = "Hydra frame failed: " + frame.frame.detail;
      return false;
    }
    if (frame.frame.device_name.empty() &&
        frame.frame.status == FrameStatus::Skip) {
      unavailable_ = true;
      unavailable_reason_ = frame.frame.detail;
      error = "the renderer is unavailable: " + frame.frame.detail;
      return false;
    }
    if (!frame.frame.ray_query_used) {
      unavailable_ = true;
      unavailable_reason_ = frame.present.detail;
      error = frame.present.detail;
      return false;
    }
    if (frame.frame.status != FrameStatus::Pass) {
      error = "Hydra frame failed: " + frame.frame.detail;
      return false;
    }
    return true;
  }

  std::vector<InteractionStep> InteractionSteps() override {
    std::vector<InteractionStep> steps;
    if (!MightBeTimeVarying(stage_)) {
      steps.push_back({"time forward (Right) on a static stage", false,
          [this](bool&, std::string&) {
            Press(*this, Key::TimeForward);
            return true;
          }});
    }
    steps.push_back({"add a time-sampled mesh in the session layer", true,
        [this](bool&, std::string& error) { return AddInteractionPrim(error); }});
    steps.push_back({"time forward (Right) with the time-sampled mesh", true,
        [this](bool& restarts, std::string&) {
          const UsdTimeCode before = time_;
          Press(*this, Key::TimeForward);
          // A time range that ends here clamps the step.
          restarts = time_ != before;
          return true;
        }});
    steps.push_back({"remove the session-layer mesh", true,
        [this](bool&, std::string& error) {
          const UsdEditContext session(stage_, stage_->GetSessionLayer());
          if (!stage_->RemovePrim(kInteractionPrim)) {
            error = "could not remove " + kInteractionPrim.GetString();
            return false;
          }
          return true;
        }});
    if (const SdfPathVector cameras = StageCameras(stage_); !cameras.empty()) {
      steps.push_back({"next camera (C): " + cameras.front().GetString(), true,
          [this, path = cameras.front()](bool&, std::string& error) {
            Press(*this, Key::NextCamera);
            if (selected_ != path) {
              error = "selected " + Status() + " instead";
              return false;
            }
            return true;
          }});
      steps.push_back({"tumble from the authored camera", true,
          [this](bool&, std::string& error) {
            return TumbleFromAuthoredCamera(error);
          }});
    }
    for (InteractionStep& step : NavigationSteps(*this)) {
      steps.push_back(std::move(step));
    }
    return steps;
  }

  // Whether the run ended because the renderer could not render, and why.
  [[nodiscard]] bool unavailable() const noexcept {
    return unavailable_;
  }
  [[nodiscard]] const std::string& unavailable_reason() const noexcept {
    return unavailable_reason_;
  }

private:
  static HdRenderSettingsMap Settings(const Options& options) {
    HdRenderSettingsMap settings;
    settings[HdRenderSettingsTokens->convergedSamplesPerPixel] =
        VtValue(static_cast<int>(options.controls.samples));
    return settings;
  }

  // The plan as the delegate's render settings (adapter.hpp), each set only
  // when it changes, and the view the presenting pass shows.
  void ApplyPlan(const FramePlan& plan) {
    const auto set = [this](const char* name, const VtValue& value) {
      const TfToken key(name);
      if (render_delegate_.GetRenderSetting(key) != value) {
        render_delegate_.SetRenderSetting(key, value);
      }
    };
    const PathTracingSettings& settings = plan.settings;
    const bool restir =
        settings.light_sampling == LightSampling::RestirInitial;
    set(HdRenderSettingsTokens->convergedSamplesPerPixel.GetText(),
        VtValue(static_cast<int>(settings.max_samples)));
    set("lotus:sampleIndex", VtValue(static_cast<int>(settings.sample_index)));
    set("lotus:wavefront",
        VtValue(settings.integrator == Integrator::Wavefront));
    set("lotus:lightSampling",
        VtValue(settings.light_sampling != LightSampling::Bsdf));
    set("lotus:restir", VtValue(restir));
    set("lotus:restirCandidates",
        VtValue(static_cast<int>(settings.restir_candidates)));
    set("lotus:restirTemporal", VtValue(settings.restir_temporal));
    set("lotus:restirSpatial", VtValue(settings.restir_spatial));
    set("lotus:temporal", VtValue(plan.temporal.accumulate_radiance));
    set("lotus:historySamples",
        VtValue(static_cast<int>(plan.temporal.max_history_samples)));
    render_delegate_.SetPresentView(plan.present);
  }

  double Aspect() const noexcept {
    return static_cast<double>(std::max(1U, width_)) /
           static_cast<double>(std::max(1U, height_));
  }

  const HdCamera* SelectedCamera() const {
    if (selected_.IsEmpty()) {
      return nullptr;
    }
    return dynamic_cast<const HdCamera*>(
        render_index_->GetSprim(HdPrimTypeTokens->camera, selected_));
  }

  // The render and proxy bounds at the current time code, as usdview frames
  // them. False, keeping the previous bounds, when they are empty.
  bool UpdateBounds() {
    const TfTokenVector purposes{UsdGeomTokens->default_,
        UsdGeomTokens->proxy, UsdGeomTokens->render};
    UsdGeomBBoxCache cache(time_, purposes, true);
    const GfBBox3d bounds = cache.ComputeWorldBound(stage_->GetPseudoRoot());
    const GfRange3d aligned = bounds.ComputeAlignedRange();
    if (aligned.IsEmpty()) {
      return false;
    }
    BoundsCorners corners{};
    for (std::size_t corner = 0; corner < corners.size(); ++corner) {
      const GfVec3d point =
          bounds.GetMatrix().Transform(bounds.GetRange().GetCorner(corner));
      corners[corner] = {point[0], point[1], point[2]};
    }
    camera_.SetBounds(corners);
    bounds_center_ = aligned.GetMidpoint();
    bounds_size_ = aligned.GetSize().GetLength();
    return true;
  }

  void HandleKey(Key key) {
    switch (key) {
    case Key::Frame:
      (void)UpdateBounds();
      camera_.SetView(camera_.Framed());
      selected_ = SdfPath();
      break;
    case Key::NextCamera: {
      const SdfPathVector cameras = StageCameras(stage_);
      const auto current =
          std::find(cameras.begin(), cameras.end(), selected_);
      // Free, each camera in turn, then free again.
      if (current == cameras.end()) {
        selected_ = cameras.empty() ? SdfPath() : cameras.front();
      } else {
        selected_ = std::next(current) == cameras.end() ? SdfPath()
                                                        : *std::next(current);
      }
      std::cout << "Camera: "
                << (selected_.IsEmpty() ? "free" : selected_.GetString())
                << '\n';
      break;
    }
    case Key::Reload:
      stage_->Reload();
      (void)UpdateBounds();
      std::cout << "Reloaded the stage's layers\n";
      break;
    case Key::TimeBack:
    case Key::TimeForward: {
      const double step = key == Key::TimeForward ? 1.0 : -1.0;
      double time = time_.IsDefault() ? stage_->GetStartTimeCode()
                                      : time_.GetValue() + step;
      if (stage_->HasAuthoredTimeCodeRange()) {
        time = std::clamp(time, stage_->GetStartTimeCode(),
            stage_->GetEndTimeCode());
      }
      time_ = UsdTimeCode(time);
      (void)UpdateBounds();
      break;
    }
    default:
      break;
    }
  }

  // Hands the authored camera's view, conformed to the viewport, to the
  // free camera, as usdview does when one navigates from a camera.
  bool Adopt(std::string& error) {
    const HdCamera* camera = SelectedCamera();
    selected_ = SdfPath();
    if (camera == nullptr) {
      return true;
    }
    state_->SetCamera(camera);
    const GfMatrix4d projection =
        state_->HdRenderPassState::GetProjectionMatrix();
    // An orthographic camera keeps the free camera's field of view.
    const double fov =
        projection[3][3] != 0.0 || !(projection[1][1] > 0.0)
            ? camera_.view().vertical_fov
            : 2.0 * std::atan(1.0 / projection[1][1]) * 180.0 /
                  std::numbers::pi;
    try {
      camera_.Adopt(FromGf(camera->GetTransform()), fov);
    } catch (const std::exception& degenerate) {
      error = degenerate.what();
      return false;
    }
    return true;
  }

  bool AddInteractionPrim(std::string& error) {
    const UsdEditContext session(stage_, stage_->GetSessionLayer());
    UsdGeomMesh mesh = UsdGeomMesh::Define(stage_, kInteractionPrim);
    if (!mesh) {
      error = "could not define " + kInteractionPrim.GetString();
      return false;
    }
    // A triangle a tenth of the bounds across at their centre, moving by a
    // twentieth of them over one time code.
    const float half = static_cast<float>(bounds_size_ * 0.05);
    mesh.CreatePointsAttr(VtValue(VtVec3fArray{GfVec3f(-half, -half, 0.0F),
        GfVec3f(half, -half, 0.0F), GfVec3f(0.0F, half, 0.0F)}));
    mesh.CreateFaceVertexCountsAttr(VtValue(VtIntArray{3}));
    mesh.CreateFaceVertexIndicesAttr(VtValue(VtIntArray{0, 1, 2}));
    const double start =
        time_.IsDefault() ? stage_->GetStartTimeCode() : time_.GetValue();
    const UsdGeomXformOp translate = mesh.AddTranslateOp();
    translate.Set(bounds_center_, UsdTimeCode(start));
    translate.Set(bounds_center_ + GfVec3d(bounds_size_ * 0.05, 0.0, 0.0),
        UsdTimeCode(start + 1.0));
    return true;
  }

  // Navigating from an authored camera must start from its exact view.
  bool TumbleFromAuthoredCamera(std::string& error) {
    if (SelectedCamera() == nullptr) {
      error = "no authored camera is selected";
      return false;
    }
    const GfMatrix4d view = state_->GetWorldToViewMatrix();
    const GfMatrix4d projection = state_->GetProjectionMatrix();
    Event down{EventType::PointerDown};
    Handle(down);
    if (!selected_.IsEmpty()) {
      error = "the authored camera is still selected";
      return false;
    }
    const GfMatrix4d adopted = ToGf(camera_.WorldToView());
    const GfMatrix4d adopted_projection = ToGf(camera_.Projection(Aspect()));
    double difference = 0.0;
    for (int row = 0; row < 4; ++row) {
      for (int column = 0; column < 4; ++column) {
        difference = std::max(difference,
            std::abs(adopted[row][column] - view[row][column]) /
                (row == 3 ? std::max(1.0, bounds_size_) : 1.0));
      }
    }
    for (const auto [row, column] : {std::pair{0, 0}, std::pair{1, 1}}) {
      difference = std::max(difference,
          std::abs(adopted_projection[row][column] -
                   projection[row][column]));
    }
    if (difference > 1.0e-6) {
      error = "the free camera left the authored view by " +
              std::to_string(difference);
      return false;
    }
    Event move{EventType::PointerMove};
    move.x = 40.0;
    move.y = 10.0;
    Handle(move);
    Event up{EventType::PointerUp};
    up.x = move.x;
    up.y = move.y;
    Handle(up);
    return true;
  }

  UsdStageRefPtr stage_;
  HdLotusRenderDelegate render_delegate_;
  std::unique_ptr<HdRenderIndex> render_index_;
  std::unique_ptr<UsdImagingDelegate> scene_delegate_;
  std::shared_ptr<StagePassState> state_;
  HdTaskSharedPtrVector tasks_;
  HdEngine engine_;
  OrbitCamera camera_;
  Navigator navigator_;
  // Empty for the free camera.
  SdfPath selected_;
  UsdTimeCode time_ = UsdTimeCode::Default();
  GfVec3d bounds_center_{0.0};
  double bounds_size_ = 1.0;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  bool unavailable_ = false;
  std::string unavailable_reason_;
};

} // namespace

int RunHydraStage(const Options& options) {
  const std::string stage_path = Utf8(options.stage);
  const UsdStageRefPtr stage = UsdStage::Open(stage_path);
  if (!stage) {
    std::cerr << "lotus-viewport: could not open the USD stage " << stage_path
              << '\n';
    return 1;
  }
  std::string reason;
  std::optional<ViewportWindow> window = OpenWindow(options, reason);
  if (!window) {
    return Unavailable(options, reason);
  }

  // The scene owns the renderer, which goes before the window's UI.
  std::unique_ptr<HydraScene> scene;
  try {
    scene = std::make_unique<HydraScene>(options, stage, window->presentation);
  } catch (const std::exception& setup) {
    std::cerr << "lotus-viewport: " << setup.what() << '\n';
    return 1;
  }
  std::cout << "USD stage: " << stage_path << '\n';
  const int result = RunFrameLoop(options, *window, *scene, stage_path);
  if (scene->unavailable()) {
    return Unavailable(options, scene->unavailable_reason());
  }
  return result;
}

} // namespace Lotus::viewport
