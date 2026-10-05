// SPDX-License-Identifier: Apache-2.0
#include "adapter.hpp"
#include "material_translator.hpp"

#include <pxr/pxr.h>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/quath.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3h.h>
#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/hashmap.h>
#include <pxr/base/vt/array.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/meshUtil.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/imaging/hd/rprimCollection.h>
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
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

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
    std::uint64_t scene_revision,
    std::uint64_t renderer_creations,
    const Lotus::GpuSceneStats& scene, std::uint32_t sample_index,
    bool converged) {
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
         << " renderer_creations=" << renderer_creations
         << " target_creations=" << frame.target_creations
         << " gpu_geometries=" << scene.resident_geometries
         << " gpu_instances=" << scene.instance_count
         << " geometry_uploads=" << scene.geometry_uploads
         << " acceleration=" << (scene.acceleration_available ? 1 : 0)
         << " blas=" << scene.blas_count
         << " tlas_instances=" << scene.tlas_instance_count
         << " blas_builds=" << scene.blas_builds
         << " ray_query=" << (frame.ray_query_used ? 1 : 0)
         << " sample_index=" << sample_index
         << " samples=" << frame.samples_per_pixel
         << " converged=" << (converged ? 1 : 0)
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

// Until render settings say otherwise, a path-traced frame converges at
// this many samples per pixel.
constexpr int kDefaultConvergedSamples = 64;

// The render setting that fixes an accumulation's first sample index, and so
// its random numbers: PathTracingSettings::sample_index.
const TfToken kSampleIndexSetting("lotus:sampleIndex");

HdAovDescriptor AovDescriptor(const TfToken& name) {
  if (name == HdAovTokens->color) {
    return {HdFormatFloat32Vec4, false, VtValue(GfVec4f(0.0F))};
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

// Validate the entire pass before rendering or writing any bound buffer.
bool ConfigureTarget(const HdRenderPassAovBindingVector& bindings,
    Lotus::OffscreenTarget& target) {
  TfTokenVector names;
  for (const HdRenderPassAovBinding& binding : bindings) {
    const auto* buffer =
        dynamic_cast<const HdLotusRenderBuffer*>(binding.renderBuffer);
    const HdAovDescriptor descriptor = AovDescriptor(binding.aovName);
    // Colour may also be 8-bit, as before HDR output.
    const bool format_ok =
        buffer != nullptr &&
        (buffer->GetFormat() == descriptor.format ||
            (binding.aovName == HdAovTokens->color &&
                buffer->GetFormat() == HdFormatUNorm8Vec4));
    if (buffer == nullptr || descriptor.format == HdFormatInvalid ||
        !format_ok || buffer->GetDepth() != 1 ||
        buffer->IsMultiSampled() || buffer->IsMapped() ||
        buffer->GetWidth() == 0 || buffer->GetHeight() == 0 ||
        std::find(names.begin(), names.end(), binding.aovName) != names.end()) {
      TF_RUNTIME_ERROR("Invalid Lotus AOV binding: %s", binding.aovName.GetText());
      return false;
    }
    names.push_back(binding.aovName);
    if (target.width == 0) {
      target.width = buffer->GetWidth();
      target.height = buffer->GetHeight();
    } else if (target.width != buffer->GetWidth() ||
               target.height != buffer->GetHeight()) {
      TF_RUNTIME_ERROR("Lotus AOV buffers must have the same extent");
      return false;
    }
    if (binding.clearValue.IsEmpty()) {
      if (binding.aovName == HdAovTokens->color) {
        target.clear_color_enabled = false;
      } else if (binding.aovName == HdAovTokens->depth) {
        target.clear_depth_enabled = false;
      }
      continue;
    }
    const VtValue& clear = binding.clearValue;
    if (binding.aovName == HdAovTokens->color && clear.IsHolding<GfVec4f>()) {
      const GfVec4f& value = clear.UncheckedGet<GfVec4f>();
      std::copy(value.data(), value.data() + 4, target.clear_color.begin());
      if (std::all_of(target.clear_color.begin(), target.clear_color.end(),
              [](float component) { return std::isfinite(component); })) {
        continue;
      }
    } else if (binding.aovName == HdAovTokens->depth && clear.IsHolding<float>()) {
      target.clear_depth = clear.UncheckedGet<float>();
      if (std::isfinite(target.clear_depth) && target.clear_depth >= 0.0F &&
          target.clear_depth <= 1.0F) {
        continue;
      }
    } else if (descriptor.format == HdFormatInt32 && clear.IsHolding<int>()) {
      continue;
    }
    TF_RUNTIME_ERROR("Invalid Lotus AOV clear value: %s", binding.aovName.GetText());
    return false;
  }
  return target.width != 0;
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

// Until dome lights are read, every surface is lit by a constant white
// environment, so an unlit surface shows its albedo.
constexpr std::array<float, 3> kFallbackEnvironment{1.0F, 1.0F, 1.0F};

// The scene keys of the meshes a render pass does not trace.
using MeshExclusion = std::unordered_set<std::string>;

class AdapterState {
public:
  AdapterState() {
    world_.SetEnvironment(kFallbackEnvironment);
  }

  // A mesh's material binding is the bound material's SdfPath string.
  void SyncMesh(const SdfPath& id, Lotus::MeshGeometry geometry,
      const Lotus::MeshInstance& instance, const SdfPath& material) {
    std::scoped_lock lock(mutex_);
    world_.SetMesh(id.GetString(), std::move(geometry), instance);
    world_.BindMaterial(id.GetString(), material.GetString());
  }

  void SyncInstance(const SdfPath& id, const Lotus::MeshInstance& instance,
      const SdfPath& material) {
    std::scoped_lock lock(mutex_);
    world_.SetMeshInstance(id.GetString(), instance);
    world_.BindMaterial(id.GetString(), material.GetString());
  }

  void SyncMaterial(const SdfPath& id, const Lotus::Material& material) {
    std::scoped_lock lock(mutex_);
    world_.SetMaterial(id.GetString(), material);
  }

  void RemoveMaterial(const SdfPath& id) {
    std::scoped_lock lock(mutex_);
    world_.RemoveMaterial(id.GetString());
  }

  void RemoveMesh(const SdfPath& id) {
    std::scoped_lock lock(mutex_);
    world_.RemoveMesh(id.GetString());
  }

  Lotus::FrameSnapshot GetFrameSnapshot() {
    std::scoped_lock lock(mutex_);
    return world_.Commit();
  }

  Lotus::GpuSceneStats GetGpuSceneStats() {
    std::scoped_lock lock(mutex_);
    return scene_stats_;
  }

  Lotus::FrameSnapshot GetSelectedSnapshot() {
    std::scoped_lock lock(mutex_);
    return selected_snapshot_;
  }

  // Whether the latest pass finished its image. A failed pass counts as
  // finished, so a host waiting for convergence does not wait forever.
  bool IsConverged() {
    std::scoped_lock lock(mutex_);
    return converged_;
  }

  // Each pass adds one radiance sample per pixel until the accumulation
  // holds `settings.max_samples`; the backend restarts it when the scene,
  // camera, framing or first sample index changes. The meshes in
  // `exclusion` are not traced.
  void Render(const HdRenderPassState& state,
      const Lotus::PathTracingSettings& settings,
      const std::shared_ptr<const MeshExclusion>& exclusion) {
    const HdRenderPassAovBindingVector& bindings = state.GetAovBindings();
    std::scoped_lock lock(mutex_);
    converged_ = true;
    for (const HdRenderPassAovBinding& binding : bindings) {
      if (auto* buffer = dynamic_cast<HdLotusRenderBuffer*>(binding.renderBuffer)) {
        buffer->SetConverged(false);
      }
    }
    world_.SetCamera(ToLotusCamera(state));
    const Lotus::FrameSnapshot snapshot = Select(world_.Commit(), exclusion);
    selected_snapshot_ = snapshot;
    const Lotus::DrawSummary draw = Lotus::ExtractDrawSummary(snapshot);
    Lotus::OffscreenTarget target;
    if (!ConfigureTarget(bindings, target)) {
      return;
    }
    ApplyFraming(state, target);

    if (!renderer_) {
      const std::filesystem::path shaders = PluginDirectory() / "shaders";
      Lotus::FrameStatus status = Lotus::FrameStatus::Fail;
      std::string error;
      renderer_ = Lotus::CreateOffscreenRenderer(
          (shaders / "triangle.vert.spv").string(),
          (shaders / "triangle.frag.spv").string(), status, error,
          {(shaders / "path_trace.vert.spv").string(),
              (shaders / "path_trace.frag.spv").string()});
      if (!renderer_) {
        TF_RUNTIME_ERROR("Lotus could not create its Vulkan renderer: %s",
            error.c_str());
        return;
      }
      ++renderer_creations_;
      // A new renderer starts with an empty GPU scene.
      extraction_.Reset();
    }
    const Lotus::GpuSceneEvidence scene =
        renderer_->UpdateScene(extraction_.Update(snapshot));
    scene_stats_ = scene.stats;
    if (scene.status != Lotus::FrameStatus::Pass) {
      TF_RUNTIME_ERROR("Lotus GPU scene update failed: %s",
          scene.detail.c_str());
      renderer_.reset();
      return;
    }
    const bool trace = renderer_->RayQueryCapability().available;
    const Lotus::GpuFrameEvidence frame =
        trace ? renderer_->RenderScene(draw, target, 1, settings)
              : renderer_->Render(draw, target, 1);
    if (frame.status != Lotus::FrameStatus::Pass) {
      TF_RUNTIME_ERROR("Lotus Hydra frame failed: %s", frame.detail.c_str());
      // A failed submission leaves the renderer unusable; the next frame
      // creates a new one.
      renderer_.reset();
      return;
    }

    converged_ = !trace || frame.samples_per_pixel >= settings.max_samples;
    std::size_t buffers_written{};
    for (const HdRenderPassAovBinding& binding : bindings) {
      auto* buffer =
          dynamic_cast<HdLotusRenderBuffer*>(binding.renderBuffer);
      if (buffer == nullptr) {
        continue;
      }
      bool wrote = false;
      // Depth and IDs are final after one pass; colour when its
      // accumulation is.
      bool complete = true;
      if (binding.aovName == HdAovTokens->color) {
        wrote = buffer->WriteColor(frame.color);
        complete = converged_;
      } else if (binding.aovName == HdAovTokens->depth) {
        wrote = buffer->WriteDepth(frame.depth.payload, frame.depth.width,
            frame.depth.height);
      } else if (binding.aovName == HdAovTokens->primId ||
                 binding.aovName == HdAovTokens->instanceId ||
                 binding.aovName == HdAovTokens->elementId) {
        // No geometry IDs exist yet. An empty clear preserves the buffer;
        // otherwise the requested sentinel is written across the image.
        wrote = binding.clearValue.IsEmpty() ||
                buffer->WriteIds(binding.clearValue.UncheckedGet<int>());
      }
      buffer->SetConverged(wrote && complete);
      if (wrote) {
        ++buffers_written;
      }
    }
    ++frame_index_;
    AppendHostEvidence(frame_index_, frame, target.width, target.height,
        buffers_written, snapshot.revision, renderer_creations_, scene_stats_,
        settings.sample_index, converged_);
  }

private:
  // The snapshot with every mesh in `exclusion` hidden. The hidden copy of
  // the scene is made only when the world's scene or the exclusion changes,
  // and not at all when the exclusion hides nothing, so an unchanged frame
  // plans no GPU work. A hidden mesh's geometry stays resident: a selection
  // change rewrites only the instances.
  Lotus::FrameSnapshot Select(Lotus::FrameSnapshot snapshot,
      const std::shared_ptr<const MeshExclusion>& exclusion) {
    if (snapshot.scene != selection_source_ || exclusion != exclusion_) {
      selection_source_ = snapshot.scene;
      exclusion_ = exclusion;
      selected_scene_ = snapshot.scene;
      hidden_triangles_ = 0;
      std::shared_ptr<Lotus::LotusScene> scene;
      for (const auto& [id, mesh] : snapshot.scene->meshes) {
        if (!mesh.instance.visible || !exclusion->contains(id)) {
          continue;
        }
        if (!scene) {
          scene = std::make_shared<Lotus::LotusScene>(*snapshot.scene);
          selected_scene_ = scene;
        }
        scene->meshes.at(id).instance.visible = false;
        hidden_triangles_ += static_cast<std::uint32_t>(
            mesh.geometry->triangles.size() *
            Lotus::PlacementTransforms(mesh.instance).size());
      }
    }
    snapshot.scene = selected_scene_;
    snapshot.triangle_count -= hidden_triangles_;
    return snapshot;
  }

  std::mutex mutex_;
  Lotus::RenderWorld world_;
  // The latest pass's exclusion, the scene it selected from and the result.
  std::shared_ptr<const MeshExclusion> exclusion_;
  std::shared_ptr<const Lotus::LotusScene> selection_source_;
  std::shared_ptr<const Lotus::LotusScene> selected_scene_;
  std::uint32_t hidden_triangles_{};
  Lotus::FrameSnapshot selected_snapshot_;
  // Created on the first frame and kept across frames (design policy
  // section 23).
  std::unique_ptr<Lotus::OffscreenRenderer> renderer_;
  // Plans the renderer's GPU scene updates; reset with each new renderer.
  Lotus::SceneExtraction extraction_;
  Lotus::GpuSceneStats scene_stats_;
  std::uint64_t renderer_creations_{};
  std::uint64_t frame_index_{};
  bool converged_ = true;
};

// Element `index` of an instance primvar holding a VtArray of one of Types,
// converted to T; nothing when the value holds none of them or the index is
// out of range.
template <typename T, typename... Types>
std::optional<T> InstanceValue(const VtValue& value, int index) {
  std::optional<T> result;
  const auto read = [&](const auto* array) {
    if (array != nullptr && index >= 0 &&
        static_cast<std::size_t>(index) < array->size()) {
      result = T((*array)[static_cast<std::size_t>(index)]);
    }
  };
  (read(value.IsHolding<VtArray<Types>>()
            ? &value.UncheckedGet<VtArray<Types>>()
            : nullptr),
      ...);
  return result;
}

// Places an instancer's prototypes. OpenUSD's hdEmbree instancer is the
// model: for column vectors, an instance's transform is
//   instancer transform * translation * rotation * scale * instance transform,
// each factor from its hydra:instance* primvar when present, and a nested
// instancer's instances repeat for each instance of its parent.
class HdLotusInstancer final : public HdInstancer {
public:
  HdLotusInstancer(HdSceneDelegate* delegate, const SdfPath& id)
      : HdInstancer(delegate, id) {
  }

  // _SyncInstancerAndParents serializes this and runs it before a
  // prototype reads the primvars.
  void Sync(HdSceneDelegate* delegate, HdRenderParam* render_param,
      HdDirtyBits* dirty_bits) override {
    (void)render_param;
    _UpdateInstancer(delegate, dirty_bits);
    if (!HdChangeTracker::IsAnyPrimvarDirty(*dirty_bits, GetId())) {
      return;
    }
    // Only the instance primvars the delegate still describes are kept.
    Primvars primvars;
    for (const HdPrimvarDescriptor& primvar :
        delegate->GetPrimvarDescriptors(GetId(), HdInterpolationInstance)) {
      const auto found = primvars_.find(primvar.name);
      primvars[primvar.name] =
          found == primvars_.end() ||
                  HdChangeTracker::IsPrimvarDirty(
                      *dirty_bits, GetId(), primvar.name)
              ? delegate->Get(GetId(), primvar.name)
              : found->second;
    }
    primvars_ = std::move(primvars);
  }

  // The world transforms of the prototype's instances, to be applied after
  // the prototype's own transform.
  VtMatrix4dArray ComputeInstanceTransforms(const SdfPath& prototype) const {
    HdSceneDelegate* delegate = GetDelegate();
    const VtIntArray indices = delegate->GetInstanceIndices(GetId(), prototype);
    VtMatrix4dArray transforms(
        indices.size(), delegate->GetInstancerTransform(GetId()));
    const VtValue* translations =
        Primvar(HdInstancerTokens->instanceTranslations);
    const VtValue* rotations = Primvar(HdInstancerTokens->instanceRotations);
    const VtValue* scales = Primvar(HdInstancerTokens->instanceScales);
    const VtValue* instance_transforms =
        Primvar(HdInstancerTokens->instanceTransforms);
    for (std::size_t i = 0; i < indices.size(); ++i) {
      // Gf matrices multiply row vectors: the left factor applies first.
      GfMatrix4d& transform = transforms[i];
      if (translations != nullptr) {
        if (const auto value = InstanceValue<GfVec3d, GfVec3f, GfVec3d,
                GfVec3h>(*translations, indices[i])) {
          transform = GfMatrix4d(1.0).SetTranslate(*value) * transform;
        }
      }
      if (rotations != nullptr) {
        if (const auto value = InstanceValue<GfQuatd, GfQuath, GfQuatf,
                GfQuatd>(*rotations, indices[i])) {
          transform = GfMatrix4d(1.0).SetRotate(*value) * transform;
        }
      }
      if (scales != nullptr) {
        if (const auto value = InstanceValue<GfVec3d, GfVec3f, GfVec3d,
                GfVec3h>(*scales, indices[i])) {
          transform = GfMatrix4d(1.0).SetScale(*value) * transform;
        }
      }
      if (instance_transforms != nullptr) {
        if (const auto value = InstanceValue<GfMatrix4d, GfMatrix4d,
                GfMatrix4f>(*instance_transforms, indices[i])) {
          transform = *value * transform;
        }
      }
    }
    if (GetParentId().IsEmpty()) {
      return transforms;
    }
    const auto* parent = dynamic_cast<const HdLotusInstancer*>(
        delegate->GetRenderIndex().GetInstancer(GetParentId()));
    if (parent == nullptr) {
      TF_CODING_ERROR("Lotus found no parent instancer %s for %s",
          GetParentId().GetText(), GetId().GetText());
      return {};
    }
    const VtMatrix4dArray parents = parent->ComputeInstanceTransforms(GetId());
    VtMatrix4dArray nested(parents.size() * transforms.size());
    for (std::size_t i = 0; i < parents.size(); ++i) {
      for (std::size_t j = 0; j < transforms.size(); ++j) {
        nested[i * transforms.size() + j] = transforms[j] * parents[i];
      }
    }
    return nested;
  }

private:
  using Primvars = TfHashMap<TfToken, VtValue, TfToken::HashFunctor>;

  const VtValue* Primvar(const TfToken& name) const {
    const auto found = primvars_.find(name);
    return found == primvars_.end() ? nullptr : &found->second;
  }

  Primvars primvars_;
};

// Translates its Hydra material network into the material IR, keyed by its
// path, whenever the network changes. Meshes bound to the path follow it.
class HdLotusMaterial final : public HdMaterial {
public:
  // A fallback material has no state and never syncs.
  HdLotusMaterial(const SdfPath& id, std::shared_ptr<AdapterState> state)
      : HdMaterial(id), state_(std::move(state)) {
  }

  ~HdLotusMaterial() override {
    if (state_) {
      state_->RemoveMaterial(GetId());
    }
  }

  HdDirtyBits GetInitialDirtyBitsMask() const override {
    return HdMaterial::AllDirty;
  }

  void Sync(HdSceneDelegate* delegate, HdRenderParam* render_param,
      HdDirtyBits* dirty_bits) override {
    (void)render_param;
    if (state_ && (*dirty_bits & (DirtyParams | DirtyResource))) {
      const VtValue resource = delegate->GetMaterialResource(GetId());
      const HdLotusMaterialTranslation translation = HdLotusTranslateMaterial(
          resource.IsHolding<HdMaterialNetworkMap>()
              ? resource.UncheckedGet<HdMaterialNetworkMap>()
              : HdMaterialNetworkMap{});
      if (!translation.unsupported.empty()) {
        TF_WARN("Lotus uses the default material for %s: %s",
            GetId().GetText(), translation.unsupported.c_str());
      }
      state_->SyncMaterial(GetId(), translation.material);
    }
    *dirty_bits = Clean;
  }

private:
  std::shared_ptr<AdapterState> state_;
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
           HdChangeTracker::DirtyRenderTag | HdChangeTracker::DirtyInstancer |
           HdChangeTracker::DirtyInstanceIndex |
           HdChangeTracker::DirtyMaterialId;
  }

  void Sync(HdSceneDelegate* delegate, HdRenderParam* render_param,
      HdDirtyBits* dirty_bits, const TfToken& repr_token) override {
    (void)render_param;
    (void)repr_token;
    const HdDirtyBits bits = *dirty_bits;
    if (bits == HdChangeTracker::Clean) {
      return;
    }
    const bool geometry_dirty = !initialized_ ||
                                (bits & HdChangeTracker::DirtyTopology) ||
                                HdChangeTracker::IsPrimvarDirty(bits, GetId(), HdTokens->points);
    if (!initialized_ || (bits & HdChangeTracker::DirtyTopology)) {
      topology_ = GetMeshTopology(delegate);
    }
    if (!initialized_ ||
        HdChangeTracker::IsPrimvarDirty(bits, GetId(), HdTokens->points)) {
      const VtValue value = GetPoints(delegate);
      points_ = value.IsHolding<VtVec3fArray>()
                    ? value.UncheckedGet<VtVec3fArray>()
                    : VtVec3fArray{};
    }
    if (!initialized_ || HdChangeTracker::IsTransformDirty(bits, GetId())) {
      instance_.world_from_object = ToLotusMatrix(delegate->GetTransform(GetId()));
    }
    if (!initialized_ || HdChangeTracker::IsVisibilityDirty(bits, GetId())) {
      instance_.visible = delegate->GetVisible(GetId());
    }
    if (!initialized_ || HdChangeTracker::IsInstancerDirty(bits, GetId()) ||
        HdChangeTracker::IsInstanceIndexDirty(bits, GetId())) {
      instance_.instancer_transforms = InstancerTransforms(delegate, bits);
    }
    if (!initialized_ || (bits & HdChangeTracker::DirtyMaterialId)) {
      SetMaterialId(delegate->GetMaterialId(GetId()));
    }
    try {
      if (geometry_dirty) {
        state_->SyncMesh(GetId(), ExtractGeometry(), instance_, GetMaterialId());
      } else {
        state_->SyncInstance(GetId(), instance_, GetMaterialId());
      }
      initialized_ = true;
    } catch (const std::invalid_argument& error) {
      // Reject the entire malformed mesh, including formerly valid geometry.
      TF_WARN("Lotus rejected mesh %s: %s", GetId().GetText(), error.what());
      state_->RemoveMesh(GetId());
      initialized_ = false;
    }
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
  // This mesh's placements as an instancer prototype, or nothing when no
  // instancer places it.
  std::optional<std::vector<Lotus::Matrix4>> InstancerTransforms(
      HdSceneDelegate* delegate, HdDirtyBits bits) {
    _UpdateInstancer(delegate, &bits);
    const SdfPath& id = GetInstancerId();
    if (id.IsEmpty()) {
      return std::nullopt;
    }
    HdRenderIndex& index = delegate->GetRenderIndex();
    HdInstancer::_SyncInstancerAndParents(index, id);
    const auto* instancer =
        dynamic_cast<const HdLotusInstancer*>(index.GetInstancer(id));
    if (instancer == nullptr) {
      TF_CODING_ERROR("Lotus found no instancer %s for %s", id.GetText(),
          GetId().GetText());
      return std::vector<Lotus::Matrix4>{};
    }
    std::vector<Lotus::Matrix4> transforms;
    for (const GfMatrix4d& transform :
        instancer->ComputeInstanceTransforms(GetId())) {
      transforms.push_back(ToLotusMatrix(transform));
    }
    return transforms;
  }

  Lotus::MeshGeometry ExtractGeometry() const {
    const auto& counts = topology_.GetFaceVertexCounts();
    const auto& indices = topology_.GetFaceVertexIndices();
    std::size_t corners = 0;
    for (int count : counts) {
      if (count < 0 || static_cast<std::size_t>(count) > indices.size() - corners) {
        throw std::invalid_argument("face counts do not match index storage");
      }
      corners += static_cast<std::size_t>(count);
    }
    if (corners != indices.size()) {
      throw std::invalid_argument("face counts do not match index storage");
    }
    for (int index : indices) {
      if (index < 0 || static_cast<std::size_t>(index) >= points_.size()) {
        throw std::invalid_argument("face vertex index is out of range");
      }
    }
    for (int hole : topology_.GetHoleIndices()) {
      if (hole < 0 || static_cast<std::size_t>(hole) >= counts.size()) {
        throw std::invalid_argument("hole face index is out of range");
      }
    }
    VtVec3iArray triangles;
    VtIntArray primitive_params;
    HdMeshUtil(&topology_, GetId()).ComputeTriangleIndices(&triangles, &primitive_params);
    Lotus::MeshGeometry geometry;
    geometry.positions.reserve(points_.size());
    for (const GfVec3f& point : points_) {
      geometry.positions.push_back({point[0], point[1], point[2]});
    }
    geometry.triangles.reserve(triangles.size());
    geometry.source_faces.reserve(primitive_params.size());
    for (const GfVec3i& triangle : triangles) {
      geometry.triangles.push_back({static_cast<std::uint32_t>(triangle[0]),
          static_cast<std::uint32_t>(triangle[1]),
          static_cast<std::uint32_t>(triangle[2])});
    }
    for (int param : primitive_params) {
      geometry.source_faces.push_back(static_cast<std::uint32_t>(
          HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(param)));
    }
    return geometry;
  }

  std::shared_ptr<AdapterState> state_;
  HdMeshTopology topology_;
  VtVec3fArray points_;
  Lotus::MeshInstance instance_;
  bool initialized_ = false;
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

public:
  bool IsConverged() const override {
    return state_->IsConverged();
  }

private:
  void _Execute(const HdRenderPassStateSharedPtr& render_pass_state,
      const TfTokenVector& render_tags) override {
    state_->Render(*render_pass_state, Settings(), Exclusion(render_tags));
  }

  // Deterministic mode: with the camera and the AOVs, these settings fix
  // the converged image. Negative values count as 0, and a sample count
  // below 1 as 1.
  Lotus::PathTracingSettings Settings() const {
    const HdRenderDelegate& delegate = *GetRenderIndex()->GetRenderDelegate();
    Lotus::PathTracingSettings settings;
    settings.max_samples = static_cast<std::uint32_t>(std::max(
        delegate.GetRenderSetting<int>(
            HdRenderSettingsTokens->convergedSamplesPerPixel,
            kDefaultConvergedSamples),
        1));
    settings.sample_index = static_cast<std::uint32_t>(
        std::max(delegate.GetRenderSetting<int>(kSampleIndexSetting, 0), 0));
    return settings;
  }

  void _MarkCollectionDirty() override {
    exclusion_.reset();
  }

  // The render index's rprims that the pass does not trace: those under none
  // of the collection's root paths or under one of its exclude paths, and
  // those whose render tag is not one of `render_tags` (when there are any).
  // The collection's material tag is ignored: a path tracer traces every
  // material in one pass. The exclusion is recomputed only when the
  // collection, the tags, the set of rprims or an rprim's render tag changes.
  std::shared_ptr<const MeshExclusion> Exclusion(
      const TfTokenVector& render_tags) {
    HdRenderIndex& index = *GetRenderIndex();
    const HdChangeTracker& tracker = index.GetChangeTracker();
    const unsigned rprim_version = tracker.GetRprimIndexVersion();
    const unsigned tag_version = tracker.GetRenderTagVersion();
    if (exclusion_ && render_tags == render_tags_ &&
        rprim_version == rprim_version_ && tag_version == tag_version_) {
      return exclusion_;
    }
    const HdRprimCollection& collection = GetRprimCollection();
    const auto under = [](const SdfPath& id, const SdfPathVector& roots) {
      return std::any_of(roots.begin(), roots.end(),
          [&](const SdfPath& root) { return id.HasPrefix(root); });
    };
    auto exclusion = std::make_shared<MeshExclusion>();
    for (const SdfPath& id : index.GetRprimIds()) {
      if (!under(id, collection.GetRootPaths()) ||
          under(id, collection.GetExcludePaths()) ||
          (!render_tags.empty() &&
              std::find(render_tags.begin(), render_tags.end(),
                  index.GetRenderTag(id)) == render_tags.end())) {
        exclusion->insert(id.GetString());
      }
    }
    exclusion_ = std::move(exclusion);
    render_tags_ = render_tags;
    rprim_version_ = rprim_version;
    tag_version_ = tag_version;
    return exclusion_;
  }

  std::shared_ptr<AdapterState> state_;
  // Cached until _MarkCollectionDirty or a change Exclusion detects.
  std::shared_ptr<const MeshExclusion> exclusion_;
  TfTokenVector render_tags_;
  unsigned rprim_version_{};
  unsigned tag_version_{};
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
      dimensions[2] != 1 || multi_sampled ||
      (format != HdFormatUNorm8Vec4 && format != HdFormatFloat32Vec4 &&
          format != HdFormatFloat32 && format != HdFormatInt32)) {
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

bool HdLotusRenderBuffer::WriteColor(const Lotus::ColorProduct& color) {
  std::scoped_lock lock(mutex_);
  HdFormat source_format = HdFormatInvalid;
  if (color.pixel_format == "rgba8-unorm") {
    source_format = HdFormatUNorm8Vec4;
  } else if (color.pixel_format == "rgba32-sfloat") {
    source_format = HdFormatFloat32Vec4;
  }
  const std::size_t pixel_count = std::size_t{color.width} * color.height;
  if (source_format == HdFormatInvalid ||
      color.row_pitch != color.width * HdDataSizeOfFormat(source_format) ||
      color.payload.size() != pixel_count * HdDataSizeOfFormat(source_format)) {
    return false;
  }
  if (source_format == format_) {
    return WriteRowsFlippedLocked(color.payload.data(), color.payload.size(),
        color.width, color.height, source_format);
  }
  // Convert one format to the other, then flip as usual.
  std::vector<std::uint8_t> converted;
  if (format_ == HdFormatUNorm8Vec4 && source_format == HdFormatFloat32Vec4) {
    converted.resize(pixel_count * 4);
    for (std::size_t index = 0; index < converted.size(); ++index) {
      float value = 0.0F;
      std::memcpy(&value, color.payload.data() + index * sizeof(float),
          sizeof(float));
      converted[index] = static_cast<std::uint8_t>(
          std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
    }
  } else if (format_ == HdFormatFloat32Vec4 &&
             source_format == HdFormatUNorm8Vec4) {
    converted.resize(pixel_count * 4 * sizeof(float));
    for (std::size_t index = 0; index < pixel_count * 4; ++index) {
      const float value = color.payload[index] / 255.0F;
      std::memcpy(converted.data() + index * sizeof(float), &value,
          sizeof(float));
    }
  } else {
    return false;
  }
  return WriteRowsFlippedLocked(converted.data(), converted.size(),
      color.width, color.height, format_);
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
  _PopulateDefaultSettings(GetRenderSettingDescriptors());
}

HdLotusRenderDelegate::~HdLotusRenderDelegate() = default;

const TfTokenVector& HdLotusRenderDelegate::GetSupportedRprimTypes() const {
  static const TfTokenVector types{HdPrimTypeTokens->mesh};
  return types;
}

const TfTokenVector& HdLotusRenderDelegate::GetSupportedSprimTypes() const {
  static const TfTokenVector types{
      HdPrimTypeTokens->camera, HdPrimTypeTokens->material};
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
  return new HdLotusInstancer(delegate, id);
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
  if (type_id == HdPrimTypeTokens->material) {
    return new HdLotusMaterial(sprim_id, impl_->state);
  }
  return nullptr;
}

HdSprim* HdLotusRenderDelegate::CreateFallbackSprim(
    const TfToken& type_id) {
  if (type_id == HdPrimTypeTokens->camera) {
    return new HdLotusCamera(SdfPath("/__lotusFallbackCamera"));
  }
  if (type_id == HdPrimTypeTokens->material) {
    return new HdLotusMaterial(SdfPath("/__lotusFallbackMaterial"), nullptr);
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
  return AovDescriptor(name);
}

HdRenderSettingDescriptorList
HdLotusRenderDelegate::GetRenderSettingDescriptors() const {
  return {{"Converged samples per pixel",
               HdRenderSettingsTokens->convergedSamplesPerPixel,
               VtValue(kDefaultConvergedSamples)},
      {"First sample index (random seed)", kSampleIndexSetting, VtValue(0)}};
}

Lotus::FrameSnapshot HdLotusRenderDelegate::GetFrameSnapshot() {
  return impl_->state->GetFrameSnapshot();
}

Lotus::GpuSceneStats HdLotusRenderDelegate::GetGpuSceneStats() {
  return impl_->state->GetGpuSceneStats();
}

Lotus::FrameSnapshot HdLotusRenderDelegate::GetSelectedSnapshot() {
  return impl_->state->GetSelectedSnapshot();
}

PXR_NAMESPACE_CLOSE_SCOPE
