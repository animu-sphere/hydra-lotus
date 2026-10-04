// SPDX-License-Identifier: Apache-2.0
#include "adapter.hpp"

#include <pxr/pxr.h>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/tf/diagnostic.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/imaging/hd/tokens.h>

#include <lotus/extraction.hpp>
#include <lotus/render_world.hpp>
#include <lotus/vulkan_backend.hpp>

#ifdef _WIN32
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

std::filesystem::path PluginDirectory() {
#ifdef _WIN32
  static int module_anchor;
  HMODULE module{};
  const auto address = reinterpret_cast<LPCWSTR>(&module_anchor);
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          address, &module)) {
    throw std::runtime_error("could not locate the hdLotus module");
  }
  std::wstring path(32768, L'\0');
  const DWORD length =
      GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) {
    throw std::runtime_error("could not resolve the hdLotus module path");
  }
  path.resize(length);
  return std::filesystem::path(path).parent_path();
#else
  static int module_anchor;
  Dl_info info{};
  if (dladdr(&module_anchor, &info) == 0 || info.dli_fname == nullptr) {
    throw std::runtime_error("could not locate the hdLotus module");
  }
  return std::filesystem::path(info.dli_fname).parent_path();
#endif
}

void AppendHostEvidence(std::uint64_t frame_index,
    const Lotus::GpuFrameEvidence& frame,
    std::uint32_t width, std::uint32_t height,
    std::size_t buffers_written,
    std::uint64_t scene_revision) {
  const char* path = std::getenv("LOTUS_HYDRA_EVIDENCE");
  if (path == nullptr || *path == '\0') {
    return;
  }
  std::ofstream output(path, std::ios::binary | std::ios::app);
  if (!output) {
    TF_WARN("Could not append Lotus Hydra evidence to %s", path);
    return;
  }
  output << "frame=" << frame_index
         << " completion=" << frame.completion
         << " scene_revision=" << scene_revision
         << " width=" << width
         << " height=" << height
         << " buffers_written=" << buffers_written
         << " validation_messages=" << frame.validation_message_count << '\n';
}

// GfMatrix4d is row-major and multiplies row vectors, so its memory is the
// column-major layout of the same transform for column vectors.
Lotus::Matrix4 ToLotusMatrix(const GfMatrix4d& matrix) {
  Lotus::Matrix4 result{};
  const double* data = matrix.data();
  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index] = static_cast<float>(data[index]);
  }
  return result;
}

// The render pass state's matrices already conform the camera to the framing
// (or, without one, to the viewport), with OpenGL clip conventions.
Lotus::Camera ToLotusCamera(const HdRenderPassState& state) {
  return Lotus::Camera{ToLotusMatrix(state.GetWorldToViewMatrix()),
      ToLotusMatrix(state.GetProjectionMatrix())};
}

// The render buffers all bound to a pass share one size; render at the colour
// buffer's, or else the first bound buffer's.
bool TargetExtent(const HdRenderPassAovBindingVector& bindings,
    std::uint32_t& width, std::uint32_t& height) {
  const HdLotusRenderBuffer* chosen = nullptr;
  for (const HdRenderPassAovBinding& binding : bindings) {
    const auto* buffer =
        dynamic_cast<const HdLotusRenderBuffer*>(binding.renderBuffer);
    if (buffer == nullptr || buffer->GetWidth() == 0 ||
        buffer->GetHeight() == 0) {
      continue;
    }
    if (chosen == nullptr || binding.aovName == HdAovTokens->color) {
      chosen = buffer;
    }
  }
  if (chosen == nullptr) {
    return false;
  }
  width = chosen->GetWidth();
  height = chosen->GetHeight();
  return true;
}

// CameraUtilFraming's windows are in pixels with y down, which is the
// backend's product origin. Without a valid framing the whole image is the
// display and data window, as for HdEmbree.
void ApplyFraming(const HdRenderPassState& state,
    Lotus::OffscreenTarget& target) {
  const CameraUtilFraming& framing = state.GetFraming();
  if (!framing.IsValid()) {
    return;
  }
  const GfRange2f& display = framing.displayWindow;
  target.display_window = {display.GetMin()[0], display.GetMin()[1],
      display.GetSize()[0], display.GetSize()[1]};
  const GfRect2i& data = framing.dataWindow;
  target.data_window = {data.GetMinX(), data.GetMinY(), data.GetWidth(),
      data.GetHeight()};
}

class AdapterState {
public:
  void SyncMesh(const SdfPath& id, bool renderable) {
    std::scoped_lock lock(mutex_);
    meshes_[id.GetString()] = renderable;
    world_.MarkChanged();
    UpdateWorldLocked();
  }

  void RemoveMesh(const SdfPath& id) {
    std::scoped_lock lock(mutex_);
    meshes_.erase(id.GetString());
    UpdateWorldLocked();
  }

  void Render(const HdRenderPassState& state) {
    const HdRenderPassAovBindingVector& bindings = state.GetAovBindings();
    std::scoped_lock lock(mutex_);
    world_.SetCamera(ToLotusCamera(state));
    const Lotus::FrameSnapshot snapshot = world_.Commit();
    const Lotus::DrawSummary draw = Lotus::ExtractDrawSummary(snapshot);
    if (draw.triangle_count == 0) {
      for (const HdRenderPassAovBinding& binding : bindings) {
        if (auto* buffer =
                dynamic_cast<HdLotusRenderBuffer*>(binding.renderBuffer)) {
          buffer->SetConverged(false);
        }
      }
      return;
    }

    Lotus::OffscreenTarget target;
    if (!TargetExtent(bindings, target.width, target.height)) {
      return;
    }
    ApplyFraming(state, target);

    const std::filesystem::path shaders = PluginDirectory() / "shaders";
    const Lotus::GpuFrameEvidence frame = Lotus::RenderOffscreen(
        draw, target, (shaders / "triangle.vert.spv").string(),
        (shaders / "triangle.frag.spv").string(), 1);
    if (frame.status != Lotus::FrameStatus::Pass) {
      TF_RUNTIME_ERROR("Lotus Hydra frame failed: %s", frame.detail.c_str());
      return;
    }

    std::size_t buffers_written{};
    for (const HdRenderPassAovBinding& binding : bindings) {
      auto* buffer =
          dynamic_cast<HdLotusRenderBuffer*>(binding.renderBuffer);
      if (buffer == nullptr) {
        continue;
      }
      bool wrote = false;
      if (binding.aovName == HdAovTokens->color) {
        wrote = buffer->WriteColor(frame.color.payload, frame.color.width,
            frame.color.height);
      } else if (binding.aovName == HdAovTokens->depth) {
        wrote = buffer->WriteDepth(frame.depth.payload, frame.depth.width,
            frame.depth.height);
      } else if (binding.aovName == HdAovTokens->primId ||
                 binding.aovName == HdAovTokens->instanceId ||
                 binding.aovName == HdAovTokens->elementId) {
        wrote = buffer->WriteIds(-1);
      }
      buffer->SetConverged(wrote);
      if (wrote) {
        ++buffers_written;
      }
    }
    ++frame_index_;
    AppendHostEvidence(frame_index_, frame, target.width, target.height,
        buffers_written, snapshot.revision);
  }

private:
  void UpdateWorldLocked() {
    const bool any_renderable =
        std::any_of(meshes_.begin(), meshes_.end(),
            [](const auto& entry) { return entry.second; });
    world_.SetTriangleCount(any_renderable ? 1U : 0U);
  }

  std::mutex mutex_;
  std::unordered_map<std::string, bool> meshes_;
  Lotus::RenderWorld world_;
  std::uint64_t frame_index_{};
};

class HdLotusMesh final : public HdMesh {
public:
  HdLotusMesh(const SdfPath& id, std::shared_ptr<AdapterState> state)
      : HdMesh(id), state_(std::move(state)) {
  }

  ~HdLotusMesh() override {
    state_->RemoveMesh(GetId());
  }

  HdDirtyBits GetInitialDirtyBitsMask() const override {
    return HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyTopology |
           HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyVisibility |
           HdChangeTracker::DirtyRenderTag;
  }

  void Sync(HdSceneDelegate* delegate, HdRenderParam* render_param,
      HdDirtyBits* dirty_bits, const TfToken& repr_token) override {
    (void)render_param;
    (void)repr_token;
    const bool visible = delegate->GetVisible(GetId());
    const HdMeshTopology topology = GetMeshTopology(delegate);
    const bool has_face =
        std::any_of(topology.GetFaceVertexCounts().begin(),
            topology.GetFaceVertexCounts().end(),
            [](int count) { return count >= 3; });
    const bool has_points = !GetPoints(delegate).IsEmpty();
    state_->SyncMesh(GetId(), visible && has_face && has_points);
    *dirty_bits = HdChangeTracker::Clean;
  }

protected:
  HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override {
    return bits;
  }

  void _InitRepr(const TfToken& repr_token, HdDirtyBits* dirty_bits) override {
    (void)repr_token;
    *dirty_bits |= GetInitialDirtyBitsMask();
  }

private:
  std::shared_ptr<AdapterState> state_;
};

class HdLotusCamera final : public HdCamera {
public:
  explicit HdLotusCamera(const SdfPath& id) : HdCamera(id) {
  }
};

class HdLotusRenderPass final : public HdRenderPass {
public:
  HdLotusRenderPass(HdRenderIndex* index,
      const HdRprimCollection& collection,
      std::shared_ptr<AdapterState> state)
      : HdRenderPass(index, collection), state_(std::move(state)) {
  }

private:
  void _Execute(const HdRenderPassStateSharedPtr& render_pass_state,
      const TfTokenVector& render_tags) override {
    (void)render_tags;
    state_->Render(*render_pass_state);
  }

  std::shared_ptr<AdapterState> state_;
};

} // namespace

HdLotusRenderBuffer::HdLotusRenderBuffer(const SdfPath& id)
    : HdRenderBuffer(id) {
}

bool HdLotusRenderBuffer::Allocate(const GfVec3i& dimensions,
    HdFormat format,
    bool multi_sampled) {
  std::scoped_lock lock(mutex_);
  if (map_count_ != 0 || dimensions[0] < 0 || dimensions[1] < 0 ||
      dimensions[2] < 0 || multi_sampled || format == HdFormatInvalid) {
    return false;
  }
  const std::size_t pixel_size = HdDataSizeOfFormat(format);
  const std::size_t width = static_cast<std::size_t>(dimensions[0]);
  const std::size_t height = static_cast<std::size_t>(dimensions[1]);
  const std::size_t depth = static_cast<std::size_t>(dimensions[2]);
  if (pixel_size == 0 ||
      (width != 0 && height > std::numeric_limits<std::size_t>::max() / width) ||
      (width * height != 0 &&
          depth > std::numeric_limits<std::size_t>::max() / (width * height)) ||
      (width * height * depth != 0 &&
          pixel_size > std::numeric_limits<std::size_t>::max() /
                           (width * height * depth))) {
    return false;
  }
  dimensions_ = dimensions;
  format_ = format;
  multi_sampled_ = multi_sampled;
  converged_ = false;
  data_.assign(width * height * depth * pixel_size, 0);
  return true;
}

unsigned int HdLotusRenderBuffer::GetWidth() const {
  std::scoped_lock lock(mutex_);
  return static_cast<unsigned int>(dimensions_[0]);
}

unsigned int HdLotusRenderBuffer::GetHeight() const {
  std::scoped_lock lock(mutex_);
  return static_cast<unsigned int>(dimensions_[1]);
}

unsigned int HdLotusRenderBuffer::GetDepth() const {
  std::scoped_lock lock(mutex_);
  return static_cast<unsigned int>(dimensions_[2]);
}

HdFormat HdLotusRenderBuffer::GetFormat() const {
  std::scoped_lock lock(mutex_);
  return format_;
}

bool HdLotusRenderBuffer::IsMultiSampled() const {
  std::scoped_lock lock(mutex_);
  return multi_sampled_;
}

void* HdLotusRenderBuffer::Map() {
  std::scoped_lock lock(mutex_);
  if (data_.empty()) {
    return nullptr;
  }
  ++map_count_;
  return data_.data();
}

void HdLotusRenderBuffer::Unmap() {
  std::scoped_lock lock(mutex_);
  if (map_count_ != 0) {
    --map_count_;
  }
}

bool HdLotusRenderBuffer::IsMapped() const {
  std::scoped_lock lock(mutex_);
  return map_count_ != 0;
}

void HdLotusRenderBuffer::Resolve() {
}

bool HdLotusRenderBuffer::IsConverged() const {
  std::scoped_lock lock(mutex_);
  return converged_;
}

bool HdLotusRenderBuffer::WriteColor(
    const std::vector<std::uint8_t>& rgba8, std::uint32_t source_width,
    std::uint32_t source_height) {
  std::scoped_lock lock(mutex_);
  return WriteRowsFlippedLocked(rgba8.data(), rgba8.size(), source_width,
      source_height, HdFormatUNorm8Vec4);
}

bool HdLotusRenderBuffer::WriteDepth(const std::vector<float>& depth,
    std::uint32_t source_width,
    std::uint32_t source_height) {
  std::scoped_lock lock(mutex_);
  return WriteRowsFlippedLocked(
      reinterpret_cast<const std::uint8_t*>(depth.data()),
      depth.size() * sizeof(float), source_width, source_height,
      HdFormatFloat32);
}

bool HdLotusRenderBuffer::WriteRowsFlippedLocked(const std::uint8_t* source,
    std::size_t source_bytes, std::uint32_t source_width,
    std::uint32_t source_height, HdFormat source_format) {
  const std::size_t pixel_size = HdDataSizeOfFormat(source_format);
  const std::size_t width = static_cast<std::size_t>(dimensions_[0]);
  const std::size_t height = static_cast<std::size_t>(dimensions_[1]);
  if (map_count_ != 0 || format_ != source_format || dimensions_[2] != 1 ||
      source_width != width || source_height != height ||
      source_bytes != width * height * pixel_size ||
      data_.size() != source_bytes) {
    return false;
  }
  const std::size_t row_bytes = width * pixel_size;
  for (std::size_t y = 0; y < height; ++y) {
    std::memcpy(data_.data() + (height - 1U - y) * row_bytes,
        source + y * row_bytes, row_bytes);
  }
  return true;
}

bool HdLotusRenderBuffer::WriteIds(std::int32_t value) {
  std::scoped_lock lock(mutex_);
  if (map_count_ != 0 || format_ != HdFormatInt32 || dimensions_[2] != 1 ||
      data_.size() % sizeof(value) != 0) {
    return false;
  }
  for (std::size_t offset = 0; offset < data_.size(); offset += sizeof(value)) {
    std::memcpy(data_.data() + offset, &value, sizeof(value));
  }
  return true;
}

void HdLotusRenderBuffer::SetConverged(bool converged) {
  std::scoped_lock lock(mutex_);
  converged_ = converged;
}

void HdLotusRenderBuffer::_Deallocate() {
  std::scoped_lock lock(mutex_);
  if (map_count_ != 0) {
    return;
  }
  dimensions_ = GfVec3i(0);
  format_ = HdFormatInvalid;
  multi_sampled_ = false;
  converged_ = false;
  data_.clear();
}

class HdLotusRenderDelegate::Impl {
public:
  std::shared_ptr<AdapterState> state = std::make_shared<AdapterState>();
};

HdLotusRenderDelegate::HdLotusRenderDelegate(
    const HdRenderSettingsMap& settings)
    : HdRenderDelegate(settings),
      impl_(std::make_unique<Impl>()),
      resources_(std::make_shared<HdResourceRegistry>()) {
}

HdLotusRenderDelegate::~HdLotusRenderDelegate() = default;

const TfTokenVector& HdLotusRenderDelegate::GetSupportedRprimTypes() const {
  static const TfTokenVector types{HdPrimTypeTokens->mesh};
  return types;
}

const TfTokenVector& HdLotusRenderDelegate::GetSupportedSprimTypes() const {
  static const TfTokenVector types{HdPrimTypeTokens->camera};
  return types;
}

const TfTokenVector& HdLotusRenderDelegate::GetSupportedBprimTypes() const {
  static const TfTokenVector types{HdPrimTypeTokens->renderBuffer};
  return types;
}

HdResourceRegistrySharedPtr HdLotusRenderDelegate::GetResourceRegistry() const {
  return resources_;
}

HdRenderPassSharedPtr HdLotusRenderDelegate::CreateRenderPass(
    HdRenderIndex* index, const HdRprimCollection& collection) {
  return std::make_shared<HdLotusRenderPass>(index, collection,
      impl_->state);
}

HdInstancer* HdLotusRenderDelegate::CreateInstancer(
    HdSceneDelegate* delegate, const SdfPath& id) {
  (void)delegate;
  (void)id;
  return nullptr;
}

void HdLotusRenderDelegate::DestroyInstancer(HdInstancer* instancer) {
  delete instancer;
}

HdRprim* HdLotusRenderDelegate::CreateRprim(const TfToken& type_id,
    const SdfPath& rprim_id) {
  if (type_id == HdPrimTypeTokens->mesh) {
    return new HdLotusMesh(rprim_id, impl_->state);
  }
  return nullptr;
}

void HdLotusRenderDelegate::DestroyRprim(HdRprim* rprim) {
  delete rprim;
}

HdSprim* HdLotusRenderDelegate::CreateSprim(const TfToken& type_id,
    const SdfPath& sprim_id) {
  if (type_id == HdPrimTypeTokens->camera) {
    return new HdLotusCamera(sprim_id);
  }
  return nullptr;
}

HdSprim* HdLotusRenderDelegate::CreateFallbackSprim(
    const TfToken& type_id) {
  if (type_id == HdPrimTypeTokens->camera) {
    return new HdLotusCamera(SdfPath("/__lotusFallbackCamera"));
  }
  return nullptr;
}

void HdLotusRenderDelegate::DestroySprim(HdSprim* sprim) {
  delete sprim;
}

HdBprim* HdLotusRenderDelegate::CreateBprim(const TfToken& type_id,
    const SdfPath& bprim_id) {
  if (type_id == HdPrimTypeTokens->renderBuffer) {
    return new HdLotusRenderBuffer(bprim_id);
  }
  return nullptr;
}

HdBprim* HdLotusRenderDelegate::CreateFallbackBprim(
    const TfToken& type_id) {
  if (type_id == HdPrimTypeTokens->renderBuffer) {
    return new HdLotusRenderBuffer(
        SdfPath("/__lotusFallbackRenderBuffer"));
  }
  return nullptr;
}

void HdLotusRenderDelegate::DestroyBprim(HdBprim* bprim) {
  delete bprim;
}

void HdLotusRenderDelegate::CommitResources(HdChangeTracker* tracker) {
  (void)tracker;
}

HdAovDescriptor HdLotusRenderDelegate::GetDefaultAovDescriptor(
    const TfToken& name) const {
  if (name == HdAovTokens->color) {
    return {HdFormatUNorm8Vec4, false, VtValue(GfVec4f(0.0F))};
  }
  if (name == HdAovTokens->depth) {
    return {HdFormatFloat32, false, VtValue(1.0F)};
  }
  if (name == HdAovTokens->primId || name == HdAovTokens->instanceId ||
      name == HdAovTokens->elementId) {
    return {HdFormatInt32, false, VtValue(-1)};
  }
  return {};
}

PXR_NAMESPACE_CLOSE_SCOPE
