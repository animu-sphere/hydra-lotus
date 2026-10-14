// SPDX-License-Identifier: Apache-2.0
// Reservoir controls, final diagnostics and moving USD stages through Hydra.
#include "adapter.hpp"

#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/base/tf/errorMark.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <lotus/extraction.hpp>
#include <lotus/vulkan_backend.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// A floor, a back wall and a tilted triangle between them, seen by a
// perspective camera. Under the adapter's white environment the grey
// surfaces light each other, so paths take several bounces.
constexpr const char* kStage = R"usda(#usda 1.0
def Camera "Camera"
{
    float focalLength = 18
    float horizontalAperture = 20.955
    float verticalAperture = 20.955
    double3 xformOp:translate = (0, 1, 4)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}

def DistantLight "Sun"
{
    float inputs:intensity = 2
    float inputs:angle = 0
    color3f inputs:color = (3, 2, 1)
}

def Xform "World"
{
    def Mesh "Floor"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-2, 0, 2), (2, 0, 2), (2, 0, -2), (-2, 0, -2)]
    }

    def Mesh "Wall"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-2, 0, -1), (2, 0, -1), (2, 3, -1), (-2, 3, -1)]
    }

    def Mesh "Panel"
    {
        double3 xformOp:translate = (0, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(-0.8, 0.1, 0.5), (0.8, 0.1, -0.3), (0, 1.6, 0)]
    }
}
)usda";

constexpr std::uint32_t kSize = 32;
constexpr int kSamples = 3;
constexpr int kSampleIndex = 4096;
const TfToken kSampleIndexSetting("lotus:sampleIndex");
const TfToken kWavefrontSetting("lotus:wavefront");

void Check(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}

class RenderTask final : public HdTask {
public:
  RenderTask(HdRenderPassSharedPtr pass, HdRenderPassStateSharedPtr state)
      : HdTask(SdfPath::EmptyPath()), pass_(std::move(pass)),
        state_(std::move(state)) {
  }
  void Sync(HdSceneDelegate*, HdTaskContext*, HdDirtyBits* dirty_bits) override {
    pass_->Sync();
    *dirty_bits = HdChangeTracker::Clean;
  }
  void Prepare(HdTaskContext*, HdRenderIndex*) override {
  }
  void Execute(HdTaskContext*) override {
    pass_->Execute(state_, {});
  }
  const TfTokenVector& GetRenderTags() const override {
    static const TfTokenVector tags{HdRenderTagTokens->geometry};
    return tags;
  }

private:
  HdRenderPassSharedPtr pass_;
  HdRenderPassStateSharedPtr state_;
};

const TfToken kTemporal("lotus:temporal");
const TfToken kCap("lotus:historySamples");
const TfToken kKey("lotus:historyKey");
const TfToken kReset("lotus:historyReset");
const TfToken kRestir("lotus:restir");
const TfToken kCandidates("lotus:restirCandidates");
const TfToken kTemporalReuse("lotus:restirTemporal");
const TfToken kSpatial("lotus:restirSpatial");
const TfToken kSourceCap("lotus:restirHistoryCandidates");
const TfToken kRadius("lotus:restirSpatialRadius");
const std::array<TfToken, 6> kAovs{TfToken("lotus:restirLight"), kCandidates,
    TfToken("lotus:restirSupport"), TfToken("lotus:restirAge"),
    TfToken("lotus:restirWeight"), TfToken("lotus:restirDirect")};

void Contract() {
  HdLotusRenderDelegate delegate;
  const auto descriptors = delegate.GetRenderSettingDescriptors();
  for (const auto& [key, value] : std::array<std::pair<TfToken, VtValue>, 6>{{{kRestir, VtValue(false)}, {kCandidates, VtValue(8)},
           {kTemporalReuse, VtValue(false)}, {kSpatial, VtValue(false)},
           {kSourceCap, VtValue(32)}, {kRadius, VtValue(1)}}}) {
    Check(std::any_of(descriptors.begin(), descriptors.end(),
              [&](const auto& d) { return d.key == key && d.defaultValue == value; }),
        "missing reservoir setting/default: " + key.GetString());
  }
  Lotus::RestirProducts products;
  products.available = true;
  products.width = 2;
  products.height = 2;
  products.initial.resize(4);
  products.temporal.resize(4);
  products.spatial.resize(4);
  for (std::size_t i = 0; i < 4; ++i) {
    products.initial[i].light = std::uint32_t(i);
    products.initial[i].candidate_count = std::uint32_t(i + 1);
    products.initial[i].selected_target = 2;
    products.initial[i].weight_sum = float(i + 1) * 4;
    products.initial[i].proposal = 0.25F;
    products.initial[i].contribution = {float(i), 2, 3, 0.5F};
    products.temporal[i].reservoir = products.initial[i];
    products.temporal[i].normalization = 5;
    products.temporal[i].supporting_candidates = std::uint32_t(i + 1);
    products.temporal[i].age = std::uint32_t(i + 3);
    products.spatial[i] = products.temporal[i];
    products.spatial[i].normalization = 7;
  }
  for (std::size_t a = 0; a < kAovs.size(); ++a) {
    const auto descriptor = delegate.GetDefaultAovDescriptor(kAovs[a]);
    const auto format = a >= 4 ? HdFormatFloat32Vec4 : HdFormatInt32;
    Check(descriptor.format == format && !descriptor.multiSampled, "AOV descriptor");
    HdLotusRenderBuffer buffer(SdfPath("/debug"));
    Check(buffer.Allocate(GfVec3i(2, 2, 1), format, false), "allocation");
    for (int stage = 2; stage >= 0; --stage) {
      auto p = products;
      if (stage < 2)
        p.spatial.clear();
      if (stage < 1)
        p.temporal.clear();
      Check(buffer.WriteRestir(kAovs[a], p, 2, 2), "write");
      const auto* data = static_cast<const std::uint8_t*>(buffer.Map());
      for (std::size_t i = 0; i < 4; ++i) {
        const auto flipped = (1 - i / 2) * 2 + i % 2;
        if (a >= 4) {
          const std::array<float, 4> expected = a == 5 ? p.initial[i].contribution
                                                       : std::array<float, 4>{float(i + 1) * 4, 2,
                                                             stage == 2 ? 7.0F : stage == 1 ? 5.0F
                                                                                            : 2.0F,
                                                             0.25F};
          Check(std::memcmp(data + flipped * 16, expected.data(), 16) == 0, "vector row/layout");
        } else {
          const auto expected = a == 0 ? i : a == 3 ? (stage ? i + 3 : 0)
                                                    : i + 1;
          std::int32_t actual;
          std::memcpy(&actual, data + flipped * 4, 4);
          Check(actual == std::int32_t(expected), "integer row/layout");
        }
      }
      Check(!buffer.WriteRestir(kAovs[a], p, 2, 2), "mapped write accepted");
      buffer.Unmap();
      if (stage == 2)
        p.spatial.pop_back();
      else if (stage == 1)
        p.temporal.pop_back();
      else
        p.initial.pop_back();
      Check(!buffer.WriteRestir(kAovs[a], p, 2, 2), "short final source accepted");
    }
    Check(!buffer.WriteRestir(kAovs[a], products, 1, 4), "wrong extent accepted");
    Check(!buffer.WriteRestir(TfToken("bad"), products, 2, 2), "unknown AOV accepted");
    Check(buffer.WriteRestir(kAovs[a], {}, 2, 2), "disabled mode clear");
    const auto* data = static_cast<const std::uint8_t*>(buffer.Map());
    for (std::size_t i = 0; i < 4; ++i) {
      if (a >= 4) {
        const std::array<float, 4> zero{};
        Check(std::memcmp(data + i * 16, zero.data(), 16) == 0, "disabled vector");
      } else {
        std::int32_t actual;
        std::memcpy(&actual, data + i * 4, 4);
        Check(actual == (a == 0 ? -1 : 0), "disabled sentinel");
      }
    }
    buffer.Unmap();
  }
}

HdRenderSettingsMap InitialSettings(bool wavefront) {
  HdRenderSettingsMap settings;
  settings[kRestir] = VtValue(true);
  settings[kCandidates] = VtValue(2);
  settings[kSourceCap] = VtValue(3);
  settings[kCap] = VtValue(kSamples);
  settings[kWavefrontSetting] = VtValue(wavefront);
  settings[kSampleIndexSetting] = VtValue(kSampleIndex);
  settings[HdRenderSettingsTokens->convergedSamplesPerPixel] = VtValue(kSamples);
  return settings;
}

class Session {
public:
  explicit Session(bool wavefront)
      : delegate_(InitialSettings(wavefront)),
        index_(HdRenderIndex::New(&delegate_, {})) {
    const auto layer = SdfLayer::CreateAnonymous(".usda");
    Check(layer->ImportFromString(kStage), "parse stage");
    stage_ = UsdStage::Open(layer);
    Check(stage_ && index_, "stage/index");
    UsdImagingCreateSceneIndicesInfo info;
    info.stage = stage_;
    indices_ = UsdImagingCreateSceneIndices(info);
    indices_.stageSceneIndex->SetTime(UsdTimeCode::Default());
    index_->InsertSceneIndex(indices_.finalSceneIndex, SdfPath::AbsoluteRootPath());
    pass_ = delegate_.CreateRenderPass(index_.get(), HdRprimCollection(
                                                         HdTokens->geometry, HdReprSelector(HdReprTokens->smoothHull),
                                                         SdfPath::AbsoluteRootPath()));
    state_ = std::make_shared<HdRenderPassState>();
    state_->SetCamera(dynamic_cast<const HdCamera*>(
        index_->GetSprim(HdPrimTypeTokens->camera, SdfPath("/Camera"))));
    state_->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0), kSize, kSize)));
    Check(color_.Allocate(GfVec3i(kSize, kSize, 1), HdFormatFloat32Vec4, false),
        "color allocation");
    HdRenderPassAovBinding color;
    color.aovName = HdAovTokens->color;
    color.renderBuffer = &color_;
    color.clearValue = VtValue(GfVec4f(0));
    bindings_.push_back(color);
    for (const auto& name : kAovs) {
      auto buffer = std::make_unique<HdLotusRenderBuffer>(SdfPath("/debug"));
      const auto descriptor = delegate_.GetDefaultAovDescriptor(name);
      Check(buffer->Allocate(GfVec3i(kSize, kSize, 1), descriptor.format, false),
          "debug allocation");
      HdRenderPassAovBinding binding;
      binding.aovName = name;
      binding.renderBuffer = buffer.get();
      binding.clearValue = descriptor.clearValue;
      bindings_.push_back(binding);
      debug_.push_back(std::move(buffer));
    }
    state_->SetAovBindings(bindings_);
    tasks_.push_back(std::make_shared<RenderTask>(pass_, state_));
  }

  void Setting(const TfToken& key, const VtValue& value) {
    delegate_.SetRenderSetting(key, value);
  }
  void Move(const char* path, GfVec3d translation) {
    Check(stage_->GetPrimAtPath(SdfPath(path)).GetAttribute(TfToken("xformOp:translate")).Set(translation), "edit transform");
  }
  void ColorClear(bool enabled) {
    bindings_[0].clearValue = enabled ? VtValue(GfVec4f(0)) : VtValue();
    state_->SetAovBindings(bindings_);
  }
  void SwitchPass() {
    pass_ = delegate_.CreateRenderPass(index_.get(), HdRprimCollection(
                                                         HdTokens->geometry, HdReprSelector(HdReprTokens->smoothHull),
                                                         SdfPath::AbsoluteRootPath()));
    tasks_.clear();
    tasks_.push_back(std::make_shared<RenderTask>(pass_, state_));
  }
  void DebugBindings(bool enabled) {
    state_->SetAovBindings(enabled ? bindings_ : HdRenderPassAovBindingVector{bindings_.front()});
  }
  std::vector<std::uint8_t> DebugData() {
    std::vector<std::uint8_t> result;
    for (const auto& buffer : debug_) {
      const auto* data = static_cast<const std::uint8_t*>(buffer->Map());
      const auto bytes = kSize * kSize * (buffer->GetFormat() == HdFormatInt32 ? 4 : 16);
      result.insert(result.end(), data, data + bytes);
      buffer->Unmap();
    }
    return result;
  }
  void ReallocateDebug() {
    for (const auto& buffer : debug_)
      Check(buffer->Allocate(GfVec3i(kSize, kSize, 1), buffer->GetFormat(), false),
          "debug reallocation");
  }
  Lotus::GpuFrameEvidence Frame() {
    indices_.stageSceneIndex->ApplyPendingUpdates();
    engine_.Execute(index_.get(), &tasks_);
    return delegate_.GetFrameEvidence();
  }
  std::vector<float> Color() {
    const auto* data = static_cast<const float*>(color_.Map());
    std::vector<float> result(data, data + kSize * kSize * 4);
    color_.Unmap();
    return result;
  }
  Lotus::GpuFrameEvidence Render(bool reuse, Lotus::TemporalResetReason reset = Lotus::TemporalResetReason::None, std::uint32_t candidates = 2) {
    TfErrorMark errors;
    auto frame = Frame();
    Check(errors.IsClean(), "Hydra diagnostics");
    Check(frame.status == Lotus::FrameStatus::Pass && frame.restir.available,
        "reservoir render: " + frame.detail);
    Check(!pass_->IsConverged() || (!reuse && frame.samples_per_pixel == kSamples),
        "reuse converged/progressive prematurely converged");
    if (reuse) {
      Check(frame.temporal.available && frame.temporal.reset_reason == reset,
          "temporal reset actual=" + std::to_string(int(frame.temporal.reset_reason)) +
              " expected=" + std::to_string(int(reset)));
      Check(frame.samples_per_pixel == 1, "reuse accumulated stale radiance");
    }
    const auto& p = frame.restir;
    const auto& resolved = p.spatial.empty() ? p.temporal : p.spatial;
    std::size_t hits = 0, history = 0;
    for (std::size_t a = 0; a < debug_.size(); ++a) {
      Check(debug_[a]->IsConverged(), "debug convergence");
      const auto* data = static_cast<const std::uint8_t*>(debug_[a]->Map());
      for (std::size_t i = 0; i < kSize * kSize; ++i) {
        const auto flipped = (kSize - 1 - i / kSize) * kSize + i % kSize;
        const auto& r = resolved.empty() ? p.initial[i] : resolved[i].reservoir;
        const auto* reuse_pixel = resolved.empty() ? nullptr : &resolved[i];
        if (a == 0) {
          hits += r.light != 0xFFFFFFFFU;
          history += reuse_pixel && reuse_pixel->previous_index != 0xFFFFFFFFU;
          Check(p.initial[i].candidate_count == 0 ||
                    p.initial[i].candidate_count == candidates,
              "candidate control");
        }
        if (a >= 4) {
          const float w = reuse_pixel             ? reuse_pixel->normalization
                          : r.selected_target > 0 ? (r.weight_sum / float(r.candidate_count)) / r.selected_target
                                                  : 0;
          const auto expected = a == 5 ? r.contribution
                                       : std::array<float, 4>{r.weight_sum, r.selected_target, w, r.proposal};
          Check(std::memcmp(data + flipped * 16, expected.data(), 16) == 0, "GPU vector AOV");
        } else {
          const auto expected = a == 0 ? r.light : a == 1    ? r.candidate_count
                                               : a == 2      ? (reuse_pixel ? reuse_pixel->supporting_candidates : r.candidate_count)
                                               : reuse_pixel ? reuse_pixel->age
                                                             : 0;
          std::int32_t actual;
          std::memcpy(&actual, data + flipped * 4, 4);
          Check(actual == static_cast<std::int32_t>(expected), "GPU integer AOV");
        }
      }
      debug_[a]->Unmap();
    }
    Check(hits > 0, "stage exercised no positive light reservoirs");
    std::cout << "reset=" << int(reset) << " hits=" << hits << " history=" << history
              << " bytes=" << p.reservoir_bytes << '\n';
    return frame;
  }

private:
  HdLotusRenderDelegate delegate_;
  std::unique_ptr<HdRenderIndex> index_;
  UsdStageRefPtr stage_;
  UsdImagingSceneIndices indices_;
  HdRenderPassSharedPtr pass_;
  HdRenderPassStateSharedPtr state_;
  HdLotusRenderBuffer color_{SdfPath("/color")};
  std::vector<std::unique_ptr<HdLotusRenderBuffer>> debug_;
  HdRenderPassAovBindingVector bindings_;
  HdTaskSharedPtrVector tasks_;
  HdEngine engine_;
};

void Sequence(bool wavefront) {
  using R = Lotus::TemporalResetReason;
  std::cout << (wavefront ? "wavefront" : "reference") << '\n';
  Session session(wavefront);
  for (int n = 1; n <= kSamples; ++n)
    session.Render(false);
  const auto initial = session.Color();
  const auto diagnostics = session.DebugData();
  session.DebugBindings(false);
  session.Frame();
  session.ReallocateDebug();
  session.DebugBindings(true);
  Check(!session.Frame().restir.available, "converged call generated reservoirs");
  Check(session.Color() == initial, "converged diagnostic binding changed colour");
  Check(session.DebugData() == diagnostics, "converged fresh binding lost last diagnostic");
  session.Setting(kTemporalReuse, VtValue(true));
  session.Render(true, R::ExplicitReset);
  Check(session.Render(true).restir.history_available, "no static reservoir history");
  session.Move("/Camera", GfVec3d(0.12, 1, 4));
  Check(session.Render(true).restir.history_available, "camera reset reservoir domain");
  session.Move("/World/Panel", GfVec3d(0.3, 0, 0));
  Check(session.Render(true).restir.history_available, "rigid placement reset reservoir domain");
  session.Setting(kSpatial, VtValue(true));
  Check(!session.Render(true, R::RenderSettingsChanged).restir.spatial.empty(), "no spatial output");
  session.Render(true);
  session.Setting(kTemporal, VtValue(true));
  Check(session.Render(true, R::RenderSettingsChanged).temporal.radiance_available,
      "independent temporal radiance selection");
  session.Setting(kTemporal, VtValue(false));
  session.Render(true, R::RenderSettingsChanged);
  session.Setting(kTemporalReuse, VtValue(false));
  Check(!session.Render(true, R::RenderSettingsChanged).restir.history_available, "spatial-only read history");
  session.Setting(kCandidates, VtValue(0));
  session.Setting(kSourceCap, VtValue(0));
  session.Setting(kRadius, VtValue(0));
  session.Render(true, R::RenderSettingsChanged, 1);
  session.Setting(kCandidates, VtValue(999));
  session.Setting(kSourceCap, VtValue(999999));
  session.Setting(kRadius, VtValue(999));
  session.Render(true, R::RenderSettingsChanged, 64);
  session.Setting(kCandidates, VtValue(2));
  session.Setting(kSourceCap, VtValue(3));
  session.Setting(kRadius, VtValue(1));
  session.Setting(kTemporalReuse, VtValue(true));
  session.Render(true, R::RenderSettingsChanged);
  session.Setting(kReset, VtValue(1));
  session.ColorClear(false);
  {
    TfErrorMark errors;
    session.Frame();
    Check(!errors.IsClean(), "preserved colour accepted for reservoir reuse");
    errors.Clear();
  }
  session.ColorClear(true);
  session.Render(true, R::CameraCut);
  session.Render(true);
  session.SwitchPass();
  session.Render(true, R::CameraCut);
  session.Setting(kRestir, VtValue(false));
  for (int n = 1; n <= kSamples; ++n) {
    const auto f = session.Frame();
    Check(f.status == Lotus::FrameStatus::Pass && !f.restir.available &&
              f.samples_per_pixel == std::uint32_t(n),
        "return to NEE/MIS progressive");
  }
  const auto mis = session.Color();
  session.Setting(kRestir, VtValue(true));
  session.Setting(TfToken("lotus:lightSampling"), VtValue(false));
  session.Setting(kSpatial, VtValue(false));
  session.Setting(kTemporalReuse, VtValue(false));
  for (int n = 1; n <= kSamples; ++n)
    session.Render(false);
  session.Setting(kRestir, VtValue(false));
  session.Setting(TfToken("lotus:lightSampling"), VtValue(true));
  for (int n = 1; n <= kSamples; ++n)
    session.Frame();
  Check(session.Color() == mis, "restoring NEE/MIS changed deterministic image");
}
} // namespace

int main(int argc, char** argv) try {
  Contract();
  if (argc > 1 && std::string(argv[1]) == "--contract")
    return 0;
  const auto shaders = std::filesystem::absolute(argv[0]).parent_path() / "shaders";
  Lotus::FrameStatus status = Lotus::FrameStatus::Fail;
  std::string error;
  const auto probe = Lotus::CreateOffscreenRenderer(
      (shaders / "triangle.vert.spv").string(),
      (shaders / "triangle.frag.spv").string(), status, error);
  if (!probe) {
    std::cerr << error << '\n';
    return status == Lotus::FrameStatus::Skip ? 77 : 1;
  }
  if (!probe->RayQueryCapability().available) {
    std::cerr << "ray queries unavailable: " << probe->RayQueryCapability().detail << '\n';
    return 77;
  }
  Sequence(false);
  Sequence(true);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
