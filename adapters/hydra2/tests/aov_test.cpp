// SPDX-License-Identifier: Apache-2.0
#include "adapter.hpp"

#include <pxr/base/tf/errorMark.h>
#include <pxr/imaging/hd/meshTopology.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/rprim.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/tokens.h>

#include <lotus/vulkan_backend.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

bool Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

class TriangleScene final : public HdSceneDelegate {
public:
  explicit TriangleScene(HdRenderIndex* index)
      : HdSceneDelegate(index, SdfPath("/scene")) {
  }

  bool visible = true;
  bool quad = false;
  GfMatrix4d transform{1.0};
  GfMatrix4d GetTransform(const SdfPath&) override {
    return transform;
  }
  bool GetVisible(const SdfPath&) override {
    return visible;
  }
  HdMeshTopology GetMeshTopology(const SdfPath&) override {
    if (quad) {
      return HdMeshTopology(TfToken("none"), HdTokens->rightHanded,
          VtIntArray{4}, VtIntArray{0, 1, 2, 3});
    }
    return HdMeshTopology(TfToken("none"), HdTokens->rightHanded,
        VtIntArray{3}, VtIntArray{0, 1, 2});
  }
  VtValue Get(const SdfPath&, const TfToken& key) override {
    if (key == HdTokens->points) {
      return VtValue(VtVec3fArray{GfVec3f(-0.5F, -0.5F, 0),
          GfVec3f(0.5F, -0.5F, 0), GfVec3f(0, 0.5F, 0),
          GfVec3f(-0.5F, 0.5F, 0)});
    }
    return {};
  }
  SdfPath GetMaterialId(const SdfPath&) override {
    return SdfPath("/scene/material");
  }
  VtValue GetMaterialResource(const SdfPath&) override {
    // Index matching gives uncoated Lambert so the AOV format and
    // convergence checks retain an exact radiance, independent of BSDF noise.
    HdMaterialNode surface;
    surface.path = SdfPath("/scene/material/surface");
    surface.identifier = TfToken("UsdPreviewSurface");
    surface.parameters[TfToken("ior")] = VtValue(1.0F);
    HdMaterialNetworkMap network;
    network.map[HdMaterialTerminalTokens->surface].nodes.push_back(surface);
    network.terminals.push_back(surface.path);
    return VtValue(network);
  }
};

HdRenderPassAovBinding Bind(const TfToken& name, HdLotusRenderBuffer& buffer,
    const VtValue& clear) {
  HdRenderPassAovBinding binding;
  binding.aovName = name;
  binding.renderBuffer = &buffer;
  binding.clearValue = clear;
  return binding;
}

} // namespace

int main(int argc, char** argv) {
  (void)argc;
  // Missing GPU capability is a CTest SKIP, never a passing GPU check.
  const auto shaders = std::filesystem::absolute(argv[0]).parent_path() / "shaders";
  Lotus::FrameStatus status;
  std::string error;
  auto probe = Lotus::CreateOffscreenRenderer(
      (shaders / "triangle.vert.spv").string(),
      (shaders / "triangle.frag.spv").string(), status, error);
  if (!probe) {
    std::cerr << error << '\n';
    return status == Lotus::FrameStatus::Skip ? 77 : 1;
  }
  const bool ray_query = probe->RayQueryCapability().available;
  probe.reset();

  // One sample converges, so each pass below finishes its image.
  HdRenderSettingsMap settings;
  settings[HdRenderSettingsTokens->convergedSamplesPerPixel] = VtValue(1);
  HdLotusRenderDelegate delegate(settings);
  std::unique_ptr<HdRenderIndex> index(HdRenderIndex::New(&delegate, {}));
  if (!Check(index != nullptr, "render index creation failed")) {
    return 1;
  }
  auto pass = delegate.CreateRenderPass(index.get(), HdRprimCollection());
  auto state = std::make_shared<HdRenderPassState>();
  HdLotusRenderBuffer color(SdfPath("/color"));
  HdLotusRenderBuffer depth(SdfPath("/depth"));
  HdLotusRenderBuffer ids(SdfPath("/primId"));
  if (!color.Allocate(GfVec3i(16, 16, 1), HdFormatUNorm8Vec4, false) ||
      !depth.Allocate(GfVec3i(16, 16, 1), HdFormatFloat32, false) ||
      !ids.Allocate(GfVec3i(16, 16, 1), HdFormatInt32, false)) {
    return 1;
  }
  HdRenderPassAovBindingVector bindings{
      Bind(HdAovTokens->color, color, VtValue(GfVec4f(0.0F))),
      Bind(HdAovTokens->depth, depth, VtValue(1.0F)),
      Bind(HdAovTokens->primId, ids, VtValue(-1))};
  TriangleScene scene(index.get());
  std::unique_ptr<HdSprim> material(delegate.CreateSprim(
      HdPrimTypeTokens->material, SdfPath("/scene/material")));
  HdDirtyBits material_dirty = material->GetInitialDirtyBitsMask();
  material->Sync(&scene, nullptr, &material_dirty);
  std::unique_ptr<HdRprim> mesh(delegate.CreateRprim(
      HdPrimTypeTokens->mesh, SdfPath("/scene/triangle")));
  HdDirtyBits dirty = mesh->GetInitialDirtyBitsMask();
  mesh->Sync(&scene, nullptr, &dirty, HdReprTokens->smoothHull);
  state->SetAovBindings(bindings);
  pass->Execute(state, {});
  const auto* pixels = static_cast<const std::uint8_t*>(color.Map());
  const auto center = (8 * 16 + 8) * 4;
  // Path-traced, index-matched grey under the adapter's
  // white fallback environment reflects 0.18 (46 of 255).
  const bool triangle = pixels != nullptr && pixels[center + 3] == 255 &&
                        (!ray_query || std::all_of(pixels + center, pixels + center + 3,
                                           [](std::uint8_t value) { return value >= 45 && value <= 47; }));
  color.Unmap();
  if (!Check(triangle && color.IsConverged() && depth.IsConverged() &&
                 ids.IsConverged(),
          "triangle AOVs did not converge")) {
    return 1;
  }
  auto gpu = delegate.GetGpuSceneStats();
  if (!Check(gpu.resident_geometries == 1 && gpu.instance_count == 1 &&
                 gpu.geometry_uploads == 1,
          "the extracted triangle did not reach the GPU scene")) {
    return 1;
  }
  // Acceleration structures follow the buffers when the device has them.
  if (!Check(!gpu.acceleration_available ||
                 (gpu.blas_count == 1 && gpu.tlas_instance_count == 1 &&
                     gpu.blas_builds == 1 && gpu.tlas_builds == 1),
          "the triangle did not get a BLAS and a TLAS instance")) {
    return 1;
  }

  // A placement edit must move the actual silhouette, without a new BLAS.
  if (ray_query) {
    scene.transform.SetTranslate(GfVec3d(0.6, 0, 0));
    dirty = HdChangeTracker::DirtyTransform;
    mesh->Sync(&scene, nullptr, &dirty, HdReprTokens->smoothHull);
    pass->Execute(state, {});
    pixels = static_cast<const std::uint8_t*>(color.Map());
    const bool moved = pixels && pixels[center + 3] == 0 &&
                       pixels[(8 * 16 + 12) * 4 + 3] == 255;
    color.Unmap();
    if (!Check(moved, "Hydra transform did not move the ray-traced silhouette"))
      return 1;
    scene.transform.SetIdentity();
    dirty = HdChangeTracker::DirtyTransform;
    mesh->Sync(&scene, nullptr, &dirty, HdReprTokens->smoothHull);
  }
  // A topology edit replaces the BLAS/TLAS used by the next ray query.
  scene.quad = true;
  dirty = HdChangeTracker::DirtyTopology;
  mesh->Sync(&scene, nullptr, &dirty, HdReprTokens->smoothHull);
  pass->Execute(state, {});
  if (!Check(delegate.GetFrameSnapshot().triangle_count == 2 &&
                 color.IsConverged() && depth.IsConverged(),
          "multi-triangle scene broke the AOV path")) {
    return 1;
  }
  gpu = delegate.GetGpuSceneStats();
  if (!Check(gpu.resident_geometries == 1 && gpu.instance_count == 1 &&
                 gpu.geometry_uploads == 2 && gpu.geometry_releases == 1,
          "a topology edit did not replace the GPU geometry")) {
    return 1;
  }
  if (!Check(!gpu.acceleration_available ||
                 (gpu.blas_count == 1 && gpu.tlas_instance_count == 1 &&
                     gpu.blas_builds == 2 && gpu.tlas_builds == 2),
          "a topology edit did not rebuild the BLAS and the TLAS")) {
    return 1;
  }

  // Hiding the only mesh must clear the old triangle and still converge.
  scene.visible = false;
  dirty = mesh->GetInitialDirtyBitsMask();
  mesh->Sync(&scene, nullptr, &dirty, HdReprTokens->smoothHull);
  auto no_clear = bindings;
  for (auto& binding : no_clear) {
    binding.clearValue = VtValue();
  }
  state->SetAovBindings(no_clear);
  pass->Execute(state, {});
  pixels = static_cast<const std::uint8_t*>(color.Map());
  const auto* retained_depth = static_cast<const float*>(depth.Map());
  const auto* retained_ids = static_cast<const std::int32_t*>(ids.Map());
  const bool preserved = pixels && retained_depth && retained_ids &&
                         pixels[(8 * 16 + 8) * 4 + 3] == 255 &&
                         retained_depth[8 * 16 + 8] < 1.0F &&
                         retained_ids[8 * 16 + 8] == -1;
  color.Unmap();
  depth.Unmap();
  ids.Unmap();
  if (!Check(preserved, "empty clear did not preserve preceding AOVs"))
    return 1;
  gpu = delegate.GetGpuSceneStats();
  if (!Check(gpu.resident_geometries == 1 && gpu.instance_count == 0 &&
                 gpu.geometry_uploads == 2,
          "hiding the mesh released or re-uploaded its GPU geometry")) {
    return 1;
  }
  if (!Check(!gpu.acceleration_available ||
                 (gpu.blas_count == 1 && gpu.tlas_instance_count == 0 &&
                     gpu.blas_builds == 2),
          "hiding the mesh rebuilt its BLAS or kept its TLAS instance")) {
    return 1;
  }
  bindings[0].clearValue = VtValue(GfVec4f(0.25F, 0.5F, 0.75F, 1.0F));
  bindings[1].clearValue = VtValue(0.375F);
  bindings[2].clearValue = VtValue(-7);
  state->SetAovBindings(bindings);
  pass->Execute(state, {});
  pixels = static_cast<const std::uint8_t*>(color.Map());
  const auto* depths = static_cast<const float*>(depth.Map());
  const auto* values = static_cast<const std::int32_t*>(ids.Map());
  bool cleared = pixels && depths && values;
  for (int pixel = 0; cleared && pixel < 16 * 16; ++pixel) {
    cleared = pixels[pixel * 4] == 64 && pixels[pixel * 4 + 1] >= 127 &&
              pixels[pixel * 4 + 1] <= 128 &&
              pixels[pixel * 4 + 2] == 191 && pixels[pixel * 4 + 3] == 255 &&
              depths[pixel] == 0.375F && values[pixel] == -7;
  }
  color.Unmap();
  depth.Unmap();
  ids.Unmap();
  if (!Check(cleared && color.IsConverged() && depth.IsConverged() &&
                 ids.IsConverged(),
          "empty scene retained old AOV pixels")) {
    return 1;
  }

  // Malformed bindings invalidate the whole pass without partial writes.
  auto rejected = [&](const HdRenderPassAovBindingVector& invalid) {
    state->SetAovBindings(invalid);
    TfErrorMark mark;
    pass->Execute(state, {});
    const bool error_reported = !mark.IsClean();
    mark.Clear();
    const auto* retained = static_cast<const std::uint8_t*>(color.Map());
    const bool unchanged = retained != nullptr && retained[0] == 64;
    color.Unmap();
    return error_reported && !color.IsConverged() && unchanged;
  };
  auto invalid = bindings;
  invalid[1].aovName = TfToken("normal");
  if (!Check(rejected(invalid), "unknown AOV was accepted"))
    return 1;
  invalid = bindings;
  invalid[1].renderBuffer = &ids;
  if (!Check(rejected(invalid), "wrong AOV format was accepted"))
    return 1;
  invalid = bindings;
  invalid[1].clearValue = VtValue(1);
  if (!Check(rejected(invalid), "wrong clear type was accepted"))
    return 1;
  invalid[1].clearValue = VtValue(std::numeric_limits<float>::quiet_NaN());
  if (!Check(rejected(invalid), "non-finite depth clear was accepted"))
    return 1;
  invalid[1].clearValue = VtValue(1.5F);
  if (!Check(rejected(invalid), "out-of-range depth clear was accepted"))
    return 1;
  invalid = bindings;
  invalid.push_back(bindings[0]);
  if (!Check(rejected(invalid), "duplicate AOV was accepted"))
    return 1;
  depth.Allocate(GfVec3i(8, 16, 1), HdFormatFloat32, false);
  if (!Check(rejected(bindings), "mismatched AOV extent was accepted"))
    return 1;
  depth.Allocate(GfVec3i(16, 16, 1), HdFormatFloat32, false);
  color.Map();
  if (!Check(rejected(bindings), "mapped AOV was accepted"))
    return 1;
  color.Unmap();

  // Recovery after a rejected binding, including a depth-only pass.
  state->SetAovBindings({bindings[1]});
  pass->Execute(state, {});
  if (!Check(depth.IsConverged(), "depth-only pass did not converge"))
    return 1;

  // The default Float32Vec4 colour accumulates one sample per pass until
  // convergedSamplesPerPixel; further passes keep the converged image.
  if (!ray_query)
    return 0;
  scene.visible = true;
  dirty = HdChangeTracker::DirtyVisibility;
  mesh->Sync(&scene, nullptr, &dirty, HdReprTokens->smoothHull);
  delegate.SetRenderSetting(HdRenderSettingsTokens->convergedSamplesPerPixel, VtValue(4));
  HdLotusRenderBuffer hdr(SdfPath("/hdr"));
  if (!Check(hdr.Allocate(GfVec3i(16, 16, 1), HdFormatFloat32Vec4, false),
          "float colour allocation failed"))
    return 1;
  bindings[0] = Bind(HdAovTokens->color, hdr, VtValue(GfVec4f(0.0F)));
  bindings[1].clearValue = VtValue(1.0F);
  state->SetAovBindings(bindings);
  std::vector<float> converged;
  for (int frame = 1; frame <= 5; ++frame) {
    pass->Execute(state, {});
    const bool done = frame >= 4;
    if (!Check(pass->IsConverged() == done && hdr.IsConverged() == done &&
                   depth.IsConverged(),
            "progressive convergence does not follow the sample count"))
      return 1;
    const auto* mapped = static_cast<const float*>(hdr.Map());
    std::vector<float> image(mapped, mapped + 16 * 16 * 4);
    hdr.Unmap();
    // An interior pixel's samples all hit: 0.18 albedo, white environment.
    const float* interior = &image[(8 * 16 + 8) * 4];
    if (!Check(std::abs(interior[0] - 0.18F) < 1e-4F && interior[3] == 1.0F,
            "the float colour's interior is not the 0.18 radiance"))
      return 1;
    if (frame == 4)
      converged = image;
    if (frame == 5 && !Check(image == converged,
                          "a pass after convergence changed the image"))
      return 1;
  }
  // A scene edit restarts the accumulation.
  scene.transform.SetTranslate(GfVec3d(0.05, 0, 0));
  dirty = HdChangeTracker::DirtyTransform;
  mesh->Sync(&scene, nullptr, &dirty, HdReprTokens->smoothHull);
  pass->Execute(state, {});
  if (!Check(!pass->IsConverged() && !hdr.IsConverged(),
          "a transform edit did not restart the accumulation"))
    return 1;
  return 0;
}
