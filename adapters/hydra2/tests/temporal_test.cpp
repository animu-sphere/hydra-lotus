// SPDX-License-Identifier: Apache-2.0
// Moving USD stages, temporal controls, CPU validity oracle and Hydra AOVs.
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
  if (!condition) throw std::runtime_error(message);
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
const std::array<TfToken, 4> kAovs{TfToken("lotus:motion"),
    TfToken("lotus:historyLength"), TfToken("lotus:historyValidity"),
    TfToken("lotus:historyRejection")};

void Contract() {
  HdLotusRenderDelegate delegate;
  const auto descriptors = delegate.GetRenderSettingDescriptors();
  for (const auto& [key, value] : std::array<std::pair<TfToken, VtValue>, 4>{{
           {kTemporal, VtValue(false)}, {kCap, VtValue(32)},
           {kKey, VtValue(0)}, {kReset, VtValue(0)}}}) {
    Check(std::any_of(descriptors.begin(), descriptors.end(),
              [&](const auto& d) { return d.key == key && d.defaultValue == value; }),
        "missing temporal setting/default: " + key.GetString());
  }
  Lotus::TemporalProducts products;
  products.available = true;
  products.radiance_available = true;
  products.width = 2;
  products.height = 2;
  products.current.resize(4);
  products.validity.resize(4);
  products.radiance.resize(4);
  for (std::size_t i = 0; i < 4; ++i) {
    products.current[i].motion_depth = {float(i), -float(i), 5, 1};
    products.radiance[i].sample_count = std::uint32_t(i + 1);
    products.validity[i].valid = std::uint32_t(i % 2);
    products.validity[i].rejection = Lotus::TemporalRejection(i + 1);
  }
  for (std::size_t a = 0; a < kAovs.size(); ++a) {
    const auto descriptor = delegate.GetDefaultAovDescriptor(kAovs[a]);
    const auto format = a == 0 ? HdFormatFloat32Vec4 : HdFormatInt32;
    Check(descriptor.format == format && !descriptor.multiSampled,
        "incorrect temporal AOV descriptor");
    HdLotusRenderBuffer buffer(SdfPath("/debug"));
    Check(buffer.Allocate(GfVec3i(2, 2, 1), format, false), "allocation");
    Check(buffer.WriteTemporal(kAovs[a], products, 2, 2), "write");
    const auto* data = static_cast<const std::uint8_t*>(buffer.Map());
    for (std::size_t i = 0; i < 4; ++i) {
      const auto flipped = (1 - i / 2) * 2 + i % 2;
      if (a == 0) {
        Check(std::memcmp(data + flipped * 16,
                  products.current[i].motion_depth.data(), 16) == 0,
            "motion row flip/layout");
      } else {
        const std::int32_t expected = a == 1 ? std::int32_t(i + 1)
            : a == 2 ? std::int32_t(i % 2) : std::int32_t(i + 1);
        std::int32_t actual;
        std::memcpy(&actual, data + flipped * 4, 4);
        Check(actual == expected, "history row flip/layout");
      }
    }
    Check(!buffer.WriteTemporal(kAovs[a], products, 2, 2), "mapped write accepted");
    buffer.Unmap();
    auto malformed = products;
    malformed.current.pop_back();
    Check(!buffer.WriteTemporal(kAovs[a], malformed, 2, 2), "short source accepted");
    Check(!buffer.WriteTemporal(kAovs[a], products, 1, 4), "wrong extent accepted");
    if (a == 1) {
      products.radiance_available = false;
      Check(buffer.WriteTemporal(kAovs[a], products, 2, 2), "metadata history write");
      const auto* zero = static_cast<const std::int32_t*>(buffer.Map());
      Check(std::all_of(zero, zero + 4, [](auto v) { return v == 0; }),
          "metadata-only history length must be zero");
      buffer.Unmap();
      products.radiance_available = true;
    }
  }
}

HdRenderSettingsMap InitialSettings(bool wavefront) {
  HdRenderSettingsMap settings;
  settings[kTemporal] = VtValue(true);
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
    Check(stage_->GetPrimAtPath(SdfPath(path)).GetAttribute(
              TfToken("xformOp:translate")).Set(translation), "edit transform");
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
    state_->SetAovBindings(enabled ? bindings_ :
        HdRenderPassAovBindingVector{bindings_.front()});
  }
  Lotus::GpuFrameEvidence Frame() {
    indices_.stageSceneIndex->ApplyPendingUpdates();
    engine_.Execute(index_.get(), &tasks_);
    return delegate_.GetFrameEvidence();
  }
  Lotus::GpuFrameEvidence Render(Lotus::TemporalResetReason reset,
      bool radiance = true) {
    TfErrorMark errors;
    auto frame = Frame();
    Check(errors.IsClean(), "Hydra diagnostics");
    Check(frame.status == Lotus::FrameStatus::Pass && frame.temporal.available,
        "temporal render: " + frame.detail);
    const auto& p = frame.temporal;
    Check(p.reset_reason == reset, "unexpected reset: " +
        std::to_string(int(p.reset_reason)) + " expected " + std::to_string(int(reset)));
    Check(p.history_available == (reset == Lotus::TemporalResetReason::None),
        "history availability");
    Check(p.radiance_available == radiance, "radiance selection");
    Check(pass_->IsConverged() == (!radiance && frame.samples_per_pixel >= kSamples),
        "temporal/progressive convergence");
    Check(color_.IsConverged() == pass_->IsConverged(), "color convergence");
    std::size_t valid = 0, moved = 0, rejected = 0;
    for (std::uint32_t y = 0; y < kSize; ++y) {
      for (std::uint32_t x = 0; x < kSize; ++x) {
        const auto i = std::size_t(y) * kSize + x;
        const auto oracle = Lotus::ReprojectTemporalHistory(p.current[i], x, y,
            kSize, kSize, p.previous);
        Check(oracle.valid == p.validity[i].valid &&
                  oracle.rejection == p.validity[i].rejection &&
                  oracle.previous_index == p.validity[i].previous_index,
            "Hydra GPU validity differs from CPU oracle");
        valid += p.validity[i].valid != 0;
        moved += std::abs(p.current[i].motion_depth[0]) > 0.01F &&
            p.current[i].motion_depth[3] != 0;
        rejected += (p.current[i].flags & 1) && !p.validity[i].valid;
      }
    }
    const auto* color = static_cast<const float*>(color_.Map());
    for (std::size_t a = 0; a < debug_.size(); ++a) {
      Check(debug_[a]->IsConverged(), "debug convergence");
      const auto* data = static_cast<const std::uint8_t*>(debug_[a]->Map());
      for (std::size_t i = 0; i < kSize * kSize; ++i) {
        const auto flipped = (kSize - 1 - i / kSize) * kSize + i % kSize;
        if (a == 0) {
          Check(std::memcmp(data + flipped * 16,
                    p.current[i].motion_depth.data(), 16) == 0, "motion AOV");
        } else {
          const auto expected = a == 1 ? (radiance ? p.radiance[i].sample_count : 0)
              : a == 2 ? p.validity[i].valid : std::uint32_t(p.validity[i].rejection);
          std::int32_t actual;
          std::memcpy(&actual, data + flipped * 4, 4);
          Check(actual == std::int32_t(expected), "history AOV");
        }
        if (radiance && a == 0)
          Check(std::memcmp(color + flipped * 4, p.radiance[i].color.data(), 16) == 0,
              "temporal color AOV");
      }
      debug_[a]->Unmap();
    }
    color_.Unmap();
    std::cout << "reset=" << int(reset) << " valid=" << valid << " moved=" << moved
              << " rejected_hits=" << rejected << " batch=" << p.batch_sample_index
              << " surface_ms=" << p.primary_surface_gpu_ms
              << " validity_ms=" << p.reprojection_gpu_ms
              << " accumulation_ms=" << p.accumulation_gpu_ms << '\n';
    valid_ = valid; moved_ = moved; rejected_ = rejected;
    return frame;
  }
  std::size_t valid_{}, moved_{}, rejected_{};
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
  for (int n = 1; n <= 5; ++n) {
    const auto frame = session.Render(n == 1 ? R::FirstFrame : R::None);
    Check(frame.samples_per_pixel == 1 &&
              frame.temporal.batch_sample_index == std::uint32_t(kSampleIndex + n - 1),
        "fresh batch/RNG progression");
    for (const auto& pixel : frame.temporal.radiance)
      Check(pixel.sample_count == 0 || pixel.sample_count == std::uint32_t(std::min(n, kSamples)),
          "static history cap");
    if (n > 1) Check(session.valid_ > 0, "static scene reused no history");
  }
  session.Move("/Camera", GfVec3d(0.12, 1, 4));
  session.Render(R::None);
  Check(session.valid_ > 0 && session.moved_ > 0, "camera motion not reused");
  session.Move("/World/Panel", GfVec3d(0.3, 0, 0));
  session.Render(R::None);
  Check(session.valid_ > 0 && session.moved_ > 0 && session.rejected_ > 0,
      "object motion/disocclusion not exercised");
  session.Setting(kReset, VtValue(1));
  session.Render(R::CameraCut);
  session.Render(R::None);
  session.Setting(kKey, VtValue(7));
  session.Render(R::HistoryKeyChanged);
  session.Setting(kCap, VtValue(2));
  session.Render(R::RenderSettingsChanged);
  session.Setting(kWavefrontSetting, VtValue(!wavefront));
  session.Render(R::RenderSettingsChanged);
  session.Setting(kWavefrontSetting, VtValue(wavefront));
  session.Render(R::RenderSettingsChanged);

  // Rejected bindings consume neither the reset counter nor the RNG range.
  session.Setting(kReset, VtValue(2));
  session.ColorClear(false);
  {
    TfErrorMark errors;
    session.Frame();
    Check(!errors.IsClean(), "preserved temporal color accepted");
    errors.Clear();
  }
  session.ColorClear(true);
  Check(session.Render(R::CameraCut).temporal.batch_sample_index == kSampleIndex,
      "rejected pass consumed reset/RNG");
  session.Render(R::None);

  // Debug AOVs request metadata while progressive transport remains selectable.
  session.Setting(kTemporal, VtValue(false));
  session.Render(R::None, false);
  session.Render(R::None, false);
  session.Render(R::None, false);
  session.Setting(kTemporal, VtValue(true));
  session.Render(R::RenderSettingsChanged);
  session.Move("/World/Panel", GfVec3d(0.4, 0, 0));
  session.Render(R::None);
  session.SwitchPass();
  session.Render(R::CameraCut);
  session.Render(R::None);
  session.DebugBindings(false);
  const auto color_only = session.Frame();
  Check(color_only.temporal.radiance_available && color_only.samples_per_pixel == 1,
      "temporal selection depends on debug AOV bindings");
  session.DebugBindings(true);
  session.Render(R::None);
}
} // namespace

int main(int argc, char** argv) try {
  Contract();
  if (argc > 1 && std::string(argv[1]) == "--contract") return 0;
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
