// SPDX-License-Identifier: Apache-2.0
#include "adapter.hpp"

#include <pxr/base/tf/errorMark.h>
#include <pxr/imaging/hd/meshTopology.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/rprim.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/tokens.h>

#include <lotus/vulkan_backend.hpp>

#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>

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
      : HdSceneDelegate(index, SdfPath("/scene")) {}

  bool visible = true;
  bool quad = false;
  bool GetVisible(const SdfPath&) override { return visible; }
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
  probe.reset();

  HdLotusRenderDelegate delegate;
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
  std::unique_ptr<HdRprim> mesh(delegate.CreateRprim(
      HdPrimTypeTokens->mesh, SdfPath("/scene/triangle")));
  HdDirtyBits dirty = mesh->GetInitialDirtyBitsMask();
  mesh->Sync(&scene, nullptr, &dirty, HdReprTokens->smoothHull);
  state->SetAovBindings(bindings);
  pass->Execute(state, {});
  const auto* pixels = static_cast<const std::uint8_t*>(color.Map());
  const bool triangle = pixels != nullptr && pixels[(8 * 16 + 8) * 4] > 150;
  color.Unmap();
  if (!Check(triangle && color.IsConverged() && depth.IsConverged() &&
                 ids.IsConverged(), "triangle AOVs did not converge")) {
    return 1;
  }

  // A multi-triangle scene must also work with the bootstrap raster backend.
  scene.quad = true;
  dirty = HdChangeTracker::DirtyTopology;
  mesh->Sync(&scene, nullptr, &dirty, HdReprTokens->smoothHull);
  pass->Execute(state, {});
  if (!Check(delegate.GetFrameSnapshot().triangle_count == 2 &&
      color.IsConverged() && depth.IsConverged(),
      "multi-triangle scene broke the bootstrap AOV path")) {
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
                         pixels[(8 * 16 + 8) * 4] > 150 &&
                         retained_depth[8 * 16 + 8] < 1.0F &&
                         retained_ids[8 * 16 + 8] == -1;
  color.Unmap();
  depth.Unmap();
  ids.Unmap();
  if (!Check(preserved, "empty clear did not preserve preceding AOVs")) return 1;
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
                 ids.IsConverged(), "empty scene retained old AOV pixels")) {
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
  if (!Check(rejected(invalid), "unknown AOV was accepted")) return 1;
  invalid = bindings;
  invalid[1].renderBuffer = &ids;
  if (!Check(rejected(invalid), "wrong AOV format was accepted")) return 1;
  invalid = bindings;
  invalid[1].clearValue = VtValue(1);
  if (!Check(rejected(invalid), "wrong clear type was accepted")) return 1;
  invalid[1].clearValue = VtValue(std::numeric_limits<float>::quiet_NaN());
  if (!Check(rejected(invalid), "non-finite depth clear was accepted")) return 1;
  invalid[1].clearValue = VtValue(1.5F);
  if (!Check(rejected(invalid), "out-of-range depth clear was accepted")) return 1;
  invalid = bindings;
  invalid.push_back(bindings[0]);
  if (!Check(rejected(invalid), "duplicate AOV was accepted")) return 1;
  depth.Allocate(GfVec3i(8, 16, 1), HdFormatFloat32, false);
  if (!Check(rejected(bindings), "mismatched AOV extent was accepted")) return 1;
  depth.Allocate(GfVec3i(16, 16, 1), HdFormatFloat32, false);
  color.Map();
  if (!Check(rejected(bindings), "mapped AOV was accepted")) return 1;
  color.Unmap();

  // Recovery after a rejected binding, including a depth-only pass.
  state->SetAovBindings({bindings[1]});
  pass->Execute(state, {});
  if (!Check(depth.IsConverged(), "depth-only pass did not converge")) return 1;
  return 0;
}
