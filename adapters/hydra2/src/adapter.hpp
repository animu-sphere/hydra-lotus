// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <pxr/pxr.h>

#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/renderDelegate.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

#include <lotus/render_world.hpp>
#include <lotus/vulkan_backend.hpp>

PXR_NAMESPACE_OPEN_SCOPE

// CPU-readable AOV storage is the portable Hydra presentation baseline. A
// generated renderer may replace the copy with explicit Hgi/Vulkan interop,
// but that is an optional project-owned capability rather than this seam.
class HdLotusRenderBuffer final : public HdRenderBuffer {
public:
  explicit HdLotusRenderBuffer(const SdfPath& id);

  bool Allocate(const GfVec3i& dimensions, HdFormat format,
      bool multi_sampled) override;
  unsigned int GetWidth() const override;
  unsigned int GetHeight() const override;
  unsigned int GetDepth() const override;
  HdFormat GetFormat() const override;
  bool IsMultiSampled() const override;
  void* Map() override;
  void Unmap() override;
  bool IsMapped() const override;
  void Resolve() override;
  bool IsConverged() const override;

  // The source is a backend product: the buffer's size, rows top-down. Hydra
  // render buffers store rows bottom-up, so the rows are flipped on the way in.
  // An rgba8-unorm or rgba32-sfloat colour product converts to the buffer's
  // UNorm8Vec4 or Float32Vec4; conversion to UNorm8 clamps to [0, 1].
  bool WriteColor(const Lotus::ColorProduct& color);
  bool WriteDepth(const std::vector<float>& depth,
      std::uint32_t source_width, std::uint32_t source_height);
  bool WriteIds(std::int32_t value);
  bool WriteTemporal(const TfToken& name, const Lotus::TemporalProducts& products,
      std::uint32_t source_width, std::uint32_t source_height);
  bool WriteRestir(const TfToken& name, const Lotus::RestirProducts& products,
      std::uint32_t source_width, std::uint32_t source_height);
  // Copy unmapped CPU contents to a top-down backend restoration input.
  bool ReadColor(std::vector<float>& color) const;
  bool ReadDepth(std::vector<float>& depth) const;
  void SetConverged(bool converged);

protected:
  void _Deallocate() override;

private:
  bool WriteRowsFlippedLocked(const std::uint8_t* source,
      std::size_t source_bytes, std::uint32_t source_width,
      std::uint32_t source_height, HdFormat source_format);

  mutable std::mutex mutex_;
  GfVec3i dimensions_{0};
  HdFormat format_{HdFormatInvalid};
  bool multi_sampled_{};
  bool converged_{};
  std::size_t map_count_{};
  std::vector<std::uint8_t> data_;
};

class HdLotusRenderDelegate final : public HdRenderDelegate {
public:
  explicit HdLotusRenderDelegate(const HdRenderSettingsMap& settings = {});
  ~HdLotusRenderDelegate() override;

  const TfTokenVector& GetSupportedRprimTypes() const override;
  const TfTokenVector& GetSupportedSprimTypes() const override;
  const TfTokenVector& GetSupportedBprimTypes() const override;
  HdResourceRegistrySharedPtr GetResourceRegistry() const override;
  HdRenderPassSharedPtr CreateRenderPass(
      HdRenderIndex* index, const HdRprimCollection& collection) override;
  HdInstancer* CreateInstancer(HdSceneDelegate* delegate,
      const SdfPath& id) override;
  void DestroyInstancer(HdInstancer* instancer) override;
  HdRprim* CreateRprim(const TfToken& type_id,
      const SdfPath& rprim_id) override;
  void DestroyRprim(HdRprim* rprim) override;
  HdSprim* CreateSprim(const TfToken& type_id,
      const SdfPath& sprim_id) override;
  HdSprim* CreateFallbackSprim(const TfToken& type_id) override;
  void DestroySprim(HdSprim* sprim) override;
  HdBprim* CreateBprim(const TfToken& type_id,
      const SdfPath& bprim_id) override;
  HdBprim* CreateFallbackBprim(const TfToken& type_id) override;
  void DestroyBprim(HdBprim* bprim) override;
  void CommitResources(HdChangeTracker* tracker) override;
  HdAovDescriptor GetDefaultAovDescriptor(const TfToken& name) const override;
  // A material's MaterialX surface wins over its universal one.
  TfTokenVector GetMaterialRenderContexts() const override;
  // convergedSamplesPerPixel: the radiance samples per pixel at which a
  // path-traced frame converges. lotus:sampleIndex: the sample index of an
  // accumulation's first sample, which selects its random numbers (0 by
  // default). lotus:wavefront: a flag selecting the wavefront integrator to
  // trace the radiance instead of the reference integrator (false by
  // default). lotus:lightSampling: a flag adding next event estimation with
  // multiple importance sampling to BSDF sampling (true by default).
  // lotus:temporal selects continuous bounded radiance reuse (false by
  // default); lotus:historySamples bounds its effective sample count.
  // lotus:historyKey changes compatibility; changing lotus:historyReset
  // cuts history once, on the next successful temporal frame.
  // lotus:filterIterations (0 by default, clamped to [0, 8]) filters
  // lotus:temporal's colour with SVGF's edge-aware a-trous filter; the
  // history stays unfiltered, so changing it never resets the history.
  // lotus:restir selects primary direct-light reservoirs, overriding
  // lotus:lightSampling. Candidate/count/radius settings are bounded;
  // lotus:restirTemporal and lotus:restirSpatial select continuous reuse
  // independently of radiance accumulation, only while restir is enabled.
  HdRenderSettingDescriptorList GetRenderSettingDescriptors() const override;
  // CPU scene inspection uses the same immutable snapshot as rendering.
  [[nodiscard]] Lotus::FrameSnapshot GetFrameSnapshot();
  // The GPU scene after the latest render pass; empty before the first.
  [[nodiscard]] Lotus::GpuSceneStats GetGpuSceneStats();
  // The latest render pass's GPU scene update; unavailable when it submitted
  // no GPU work, and before the first pass.
  [[nodiscard]] Lotus::GpuSceneTimings GetGpuSceneTimings();
  // The snapshot the latest render pass traced: the CPU scene with every mesh
  // outside the pass's collection or render tags hidden. Recorded before the
  // pass validates its AOVs or creates a renderer; empty before the first.
  [[nodiscard]] Lotus::FrameSnapshot GetSelectedSnapshot();
  // The latest render pass's frame evidence, without the colour and depth
  // products; empty before the first pass that rendered.
  [[nodiscard]] Lotus::GpuFrameEvidence GetFrameEvidence();
  // The image beside the hdLotus module.
  [[nodiscard]] static std::filesystem::path PackagedFallbackEnvironment();
  // A scene with no dome and no scene light is lit by this latitude-longitude
  // image, unturned, instead of a white environment. The Hydra plugin sets
  // PackagedFallbackEnvironment(), as Storm's hosts default to the same image.
  void SetFallbackEnvironment(const std::filesystem::path& file);
  // For a host that owns a window (lotus-viewport): the renderer is created
  // for this surface, and a render pass whose one AOV binding is colour
  // without a render buffer presents its frame there, at the render pass
  // state's viewport extent, without reading it back. Call before the first
  // render pass.
  void SetPresentation(const Lotus::PresentOptions& presentation);
  // What a presenting pass shows (Lotus::OffscreenRenderer::Present). A
  // temporal debug view renders the pass through RenderTemporalScene, for
  // its primary-surface products; a radiance view needs lotus:temporal and
  // a reservoir view lotus:restir.
  void SetPresentView(const Lotus::PresentView& view);
  // The latest presenting pass's presentation; empty before the first.
  [[nodiscard]] Lotus::PresentEvidence GetPresentEvidence();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  HdResourceRegistrySharedPtr resources_;
};

PXR_NAMESPACE_CLOSE_SCOPE
