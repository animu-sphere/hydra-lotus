// SPDX-License-Identifier: Apache-2.0
// Syncs the skinned fixture through both of UsdImaging's front ends into the
// Lotus render delegate and compares each mesh's world points, at several
// times, with what UsdSkel computes independently. --computed-normals expects
// USDSKELIMAGING_ENABLE_NORMAL_COMPUTATIONS and also compares the normals. With
// --gpu the pass also renders, and a time change must refit the skinned
// geometry's acceleration structures and move it in the image.
#include "adapter.hpp"

#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/matrix3d.h>
#include <pxr/base/gf/matrix3f.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdSkel/animQuery.h>
#include <pxr/usd/usdSkel/binding.h>
#include <pxr/usd/usdSkel/bindingAPI.h>
#include <pxr/usd/usdSkel/blendShapeQuery.h>
#include <pxr/usd/usdSkel/cache.h>
#include <pxr/usd/usdSkel/root.h>
#include <pxr/usd/usdSkel/skeletonQuery.h>
#include <pxr/usd/usdSkel/skinningQuery.h>
#include <pxr/usd/usdSkel/utils.h>
#include <pxr/usdImaging/usdImaging/delegate.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <lotus/vulkan_backend.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

constexpr int kSize = 96;
constexpr int kSamples = 16;
// Hydra skins in floats; the oracle's results are floats in double transforms.
constexpr double kPointTolerance = 2e-4;
constexpr double kNormalTolerance = 2e-3;

void Check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string Text(const GfVec3d& point) {
  return "(" + std::to_string(point[0]) + ", " + std::to_string(point[1]) + ", " +
         std::to_string(point[2]) + ")";
}

// Syncs and, with AOV bindings, renders one render pass.
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
    pass_->Execute(state_, GetRenderTags());
  }
  const TfTokenVector& GetRenderTags() const override {
    static const TfTokenVector tags{HdRenderTagTokens->geometry};
    return tags;
  }

private:
  HdRenderPassSharedPtr pass_;
  HdRenderPassStateSharedPtr state_;
};

// One stage synced into one delegate, through scene indices or, as the
// viewport does, through UsdImagingDelegate.
class Host {
public:
  Host(const UsdStageRefPtr& stage, bool scene_indices, bool gpu)
      : color_(SdfPath("/color")), gpu_(gpu) {
    index_.reset(HdRenderIndex::New(&delegate_, {}));
    Check(index_ != nullptr, "could not create the render index");
    if (scene_indices) {
      UsdImagingCreateSceneIndicesInfo info;
      info.stage = stage;
      scene_indices_ = UsdImagingCreateSceneIndices(info);
      scene_indices_->stageSceneIndex->SetTime(UsdTimeCode(1));
      index_->InsertSceneIndex(
          scene_indices_->finalSceneIndex, SdfPath::AbsoluteRootPath());
    } else {
      legacy_ = std::make_unique<UsdImagingDelegate>(
          index_.get(), SdfPath::AbsoluteRootPath());
      legacy_->Populate(stage->GetPseudoRoot());
      legacy_->SetTime(UsdTimeCode(1));
    }
    auto state = std::make_shared<HdRenderPassState>();
    if (gpu_) {
      state->SetCamera(dynamic_cast<const HdCamera*>(
          index_->GetSprim(HdPrimTypeTokens->camera, SdfPath("/World/Camera"))));
      state->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0), kSize, kSize)));
      Check(color_.Allocate(GfVec3i(kSize, kSize, 1), HdFormatFloat32Vec4, false),
          "could not allocate the colour buffer");
      HdRenderPassAovBinding binding;
      binding.aovName = HdAovTokens->color;
      binding.renderBuffer = &color_;
      binding.clearValue = VtValue(GfVec4f(0.0F));
      state->SetAovBindings({binding});
    }
    tasks_.push_back(std::make_shared<RenderTask>(
        delegate_.CreateRenderPass(index_.get(),
            HdRprimCollection(HdTokens->geometry,
                HdReprSelector(HdReprTokens->smoothHull))),
        state));
  }

  ~Host() {
    legacy_.reset();
  }

  Lotus::FrameSnapshot SyncAt(double time) {
    if (scene_indices_) {
      scene_indices_->stageSceneIndex->SetTime(UsdTimeCode(time));
      scene_indices_->stageSceneIndex->ApplyPendingUpdates();
    } else {
      legacy_->SetTime(UsdTimeCode(time));
    }
    for (int i = 0; i < (gpu_ ? kSamples : 1); ++i) {
      engine_.Execute(index_.get(), &tasks_);
    }
    return delegate_.GetFrameSnapshot();
  }

  std::vector<float> Image() {
    const auto* data = static_cast<const float*>(color_.Map());
    Check(data != nullptr, "could not map the colour buffer");
    std::vector<float> image(data, data + kSize * kSize * 4);
    color_.Unmap();
    return image;
  }

  HdLotusRenderDelegate& delegate() {
    return delegate_;
  }

private:
  HdLotusRenderDelegate delegate_;
  HdLotusRenderBuffer color_;
  std::unique_ptr<HdRenderIndex> index_;
  std::optional<UsdImagingSceneIndices> scene_indices_;
  std::unique_ptr<UsdImagingDelegate> legacy_;
  HdTaskSharedPtrVector tasks_;
  HdEngine engine_;
  bool gpu_;
};

GfVec3d TransformNormal(const GfMatrix3d& inverse_transpose, const GfVec3f& normal) {
  return (GfVec3d(normal) * inverse_transpose).GetNormalized();
}

GfMatrix3d InverseTranspose(const GfMatrix4d& matrix) {
  return matrix.ExtractRotationMatrix().GetInverse().GetTranspose();
}

// What UsdSkel computes for one mesh, in world space.
struct Skinned {
  std::vector<GfVec3d> points;
  // Per point, or per face vertex when `face_varying`; empty for none.
  std::vector<GfVec3d> normals;
  bool face_varying = false;
};

class Oracle {
public:
  explicit Oracle(const UsdStageRefPtr& stage) {
    const UsdSkelRoot root(stage->GetPrimAtPath(SdfPath("/World/Rig")));
    Check(cache_.Populate(root, UsdTraverseInstanceProxies()),
        "UsdSkel could not populate the rig");
    Check(cache_.ComputeSkelBindings(root, &bindings_, UsdTraverseInstanceProxies()),
        "UsdSkel could not compute the rig's bindings");
  }

  std::map<std::string, Skinned> At(double time_value) const {
    const UsdTimeCode time(time_value);
    UsdGeomXformCache xforms(time);
    std::map<std::string, Skinned> meshes;
    for (const UsdSkelBinding& binding : bindings_) {
      const UsdSkelSkeletonQuery skeleton = cache_.GetSkelQuery(binding.GetSkeleton());
      VtMatrix4dArray skinning;
      Check(skeleton.ComputeSkinningTransforms(&skinning, time),
          "UsdSkel could not compute skinning transforms");
      VtMatrix3fArray skinning_normals(skinning.size());
      for (std::size_t i = 0; i < skinning.size(); ++i) {
        skinning_normals[i] = GfMatrix3f(InverseTranspose(skinning[i]));
      }
      const GfMatrix4d world =
          xforms.GetLocalToWorldTransform(binding.GetSkeleton().GetPrim());
      const GfMatrix3d world_normals = InverseTranspose(world);
      for (const UsdSkelSkinningQuery& query : binding.GetSkinningTargets()) {
        const UsdPrim prim = query.GetPrim();
        if (prim.GetName() == "Malformed") {
          continue;
        }
        const UsdGeomMesh mesh(prim);
        VtVec3fArray points;
        Check(mesh.GetPointsAttr().Get(&points, time), "the mesh has no points");
        if (query.HasBlendShapes()) {
          Deform(skeleton, query, time, &points);
        }
        Check(query.ComputeSkinnedPoints(skinning, &points, time),
            "UsdSkel could not skin " + prim.GetPath().GetString());
        Skinned& skinned = meshes[prim.GetPath().GetString()];
        for (const GfVec3f& point : points) {
          skinned.points.push_back(world.Transform(GfVec3d(point)));
        }
        SkinNormals(query, mesh, skinning_normals, world_normals, time, &skinned);
      }
    }
    return meshes;
  }

private:
  static void Deform(const UsdSkelSkeletonQuery& skeleton,
      const UsdSkelSkinningQuery& query, UsdTimeCode time, VtVec3fArray* points) {
    const UsdSkelBlendShapeQuery shapes{UsdSkelBindingAPI(query.GetPrim())};
    Check(static_cast<bool>(shapes), "no blend shape query");
    VtFloatArray animated, weights;
    Check(skeleton.GetAnimQuery().ComputeBlendShapeWeights(&animated, time),
        "UsdSkel could not compute blend shape weights");
    Check(query.GetBlendShapeMapper()->Remap(animated, &weights),
        "UsdSkel could not map blend shape weights");
    VtFloatArray sub_shape_weights;
    VtUIntArray shape_indices, sub_shape_indices;
    Check(shapes.ComputeSubShapeWeights(
              weights, &sub_shape_weights, &shape_indices, &sub_shape_indices),
        "UsdSkel could not compute sub-shape weights");
    Check(shapes.ComputeDeformedPoints(sub_shape_weights, shape_indices,
              sub_shape_indices, shapes.ComputeBlendShapePointIndices(),
              shapes.ComputeSubShapePointOffsets(), *points),
        "UsdSkel could not apply the blend shapes");
  }

  static void SkinNormals(const UsdSkelSkinningQuery& query, const UsdGeomMesh& mesh,
      const VtMatrix3fArray& skinning, const GfMatrix3d& world, UsdTimeCode time,
      Skinned* skinned) {
    VtVec3fArray normals;
    TfToken interpolation = mesh.GetNormalsInterpolation();
    const UsdGeomPrimvar primvar = UsdGeomPrimvarsAPI(mesh).GetPrimvar(HdTokens->normals);
    if (primvar.HasAuthoredValue()) {
      primvar.Get(&normals, time);
      interpolation = primvar.GetInterpolation();
    } else if (!mesh.GetNormalsAttr().Get(&normals, time) || normals.empty()) {
      return;
    }
    VtIntArray indices, face_vertex_indices;
    VtFloatArray weights;
    Check(query.ComputeVaryingJointInfluences(skinned->points.size(), &indices, &weights, time),
        "UsdSkel could not compute joint influences");
    const GfMatrix3f bind(InverseTranspose(query.GetGeomBindTransform(time)));
    // The skeleton's joint order is the influences' here, so no remapping.
    if (interpolation == UsdGeomTokens->faceVarying) {
      mesh.GetFaceVertexIndicesAttr().Get(&face_vertex_indices, time);
      Check(UsdSkelSkinFaceVaryingNormals(query.GetSkinningMethod(), bind,
                TfSpan<const GfMatrix3f>(skinning), TfSpan<const int>(indices),
                TfSpan<const float>(weights), query.GetNumInfluencesPerComponent(),
                TfSpan<const int>(face_vertex_indices), TfSpan<GfVec3f>(normals)),
          "UsdSkel could not skin face-varying normals");
      skinned->face_varying = true;
    } else {
      Check(UsdSkelSkinNormals(query.GetSkinningMethod(), bind,
                TfSpan<const GfMatrix3f>(skinning), TfSpan<const int>(indices),
                TfSpan<const float>(weights), query.GetNumInfluencesPerComponent(),
                TfSpan<GfVec3f>(normals)),
          "UsdSkel could not skin normals");
    }
    for (const GfVec3f& normal : normals) {
      skinned->normals.push_back(TransformNormal(world, normal));
    }
  }

  UsdSkelCache cache_;
  std::vector<UsdSkelBinding> bindings_;
};

GfMatrix4d WorldFromObject(const Lotus::SceneMesh& mesh) {
  GfMatrix4d matrix;
  double* data = matrix.data();
  for (std::size_t i = 0; i < mesh.instance.world_from_object.size(); ++i) {
    data[i] = mesh.instance.world_from_object[i];
  }
  return matrix;
}

std::vector<GfVec3d> WorldPoints(const Lotus::SceneMesh& mesh) {
  const GfMatrix4d world = WorldFromObject(mesh);
  std::vector<GfVec3d> points;
  for (const auto& position : mesh.geometry->positions) {
    points.push_back(world.Transform(GfVec3d(position[0], position[1], position[2])));
  }
  return points;
}

// Storm's coarse smooth normals of the strip's quads, from the posed points.
std::vector<GfVec3d> SmoothNormals(const std::vector<GfVec3d>& points,
    const VtIntArray& counts, const VtIntArray& indices) {
  std::vector<GfVec3d> normals(points.size(), GfVec3d(0));
  std::size_t base = 0;
  for (const int count : counts) {
    for (int corner = 0; corner < count; ++corner) {
      const auto at = [&](int offset) {
        return indices[base + static_cast<std::size_t>((corner + offset) % count)];
      };
      const GfVec3d& point = points[static_cast<std::size_t>(at(0))];
      normals[static_cast<std::size_t>(at(0))] +=
          GfCross(points[static_cast<std::size_t>(at(1))] - point,
              points[static_cast<std::size_t>(at(count - 1))] - point);
    }
    base += static_cast<std::size_t>(count);
  }
  for (GfVec3d& normal : normals) normal.Normalize();
  return normals;
}

void CheckClose(const GfVec3d& actual, const GfVec3d& expected, double tolerance,
    const std::string& what) {
  Check((actual - expected).GetLength() <= tolerance,
      what + " is " + Text(actual) + ", expected " + Text(expected));
}

void CheckNormals(const std::string& step, const Lotus::SceneMesh& mesh,
    const Skinned& skinned) {
  const auto& geometry = *mesh.geometry;
  Check(geometry.normals.size() == 3 * geometry.triangles.size(),
      step + ": expected corner normals, got " +
          std::to_string(geometry.normals.size()));
  const GfMatrix3d world = InverseTranspose(WorldFromObject(mesh));
  std::map<std::uint32_t, int> face_triangles;
  for (std::size_t t = 0; t < geometry.triangles.size(); ++t) {
    // The strip's quads triangulate as (0, 1, 2) and (0, 2, 3).
    const int fan = face_triangles[geometry.source_faces[t]]++;
    for (std::size_t c = 0; c < 3; ++c) {
      const std::size_t index =
          skinned.face_varying
              ? 4 * std::size_t{geometry.source_faces[t]} +
                    (c == 0 ? 0 : static_cast<std::size_t>(fan) + c)
              : geometry.triangles[t][c];
      const auto& normal = geometry.normals[3 * t + c];
      CheckClose(TransformNormal(world, GfVec3f(normal[0], normal[1], normal[2])),
          skinned.normals.at(index), kNormalTolerance,
          step + ": corner " + std::to_string(3 * t + c) + "'s normal");
    }
  }
}

// Every mesh of `snapshot` against UsdSkel's result for `time`.
void Compare(const std::string& step, const Lotus::FrameSnapshot& snapshot,
    const UsdStageRefPtr& stage, const Oracle& oracle, double time,
    bool computed_normals) {
  const auto expected = oracle.At(time);
  const auto& meshes = snapshot.scene->meshes;
  // The skeletons' guide meshes have the guide render tag.
  const std::set<std::string> names{"/World/Fixed", "/World/Plain",
      "/World/Rig/Blend", "/World/Rig/Dual", "/World/Rig/FaceVarying",
      "/World/Rig/Linear", "/World/Rig/Malformed", "/World/Rig/Rest",
      "/World/Rig/Rigid", "/World/Rig/Smooth",
      "/World/Rig/Parts.lotus:subset:Lower", "/World/Rig/Parts.lotus:subset:Upper"};
  std::set<std::string> actual_names;
  for (const auto& [name, mesh] : meshes) actual_names.insert(name);
  std::string listed;
  for (const std::string& name : actual_names) listed += "\n  " + name;
  Check(actual_names == names, step + ": unexpected meshes:" + listed);

  for (const auto& [name, skinned] : expected) {
    if (name == "/World/Rig/Parts") continue;
    const Lotus::SceneMesh& mesh = meshes.at(name);
    const std::vector<GfVec3d> points = WorldPoints(mesh);
    Check(points.size() == skinned.points.size(),
        step + ": " + name + " has " + std::to_string(points.size()) + " points");
    for (std::size_t i = 0; i < points.size(); ++i) {
      CheckClose(points[i], skinned.points[i], kPointTolerance,
          step + ": " + name + "'s point " + std::to_string(i));
    }
    if (computed_normals && !skinned.normals.empty()) {
      CheckNormals(step + ": " + name, mesh, skinned);
    } else if (!computed_normals && skinned.normals.size() == skinned.points.size()) {
      // UsdImaging withholds authored normals: they would not fit the pose.
      Check(mesh.geometry->normals.empty(),
          step + ": " + name + " kept normals UsdImaging did not hand over");
    }
  }

  // Normals computed from the posed points, in object space.
  const Lotus::SceneMesh& smooth = meshes.at("/World/Rig/Smooth");
  const UsdGeomMesh strip(stage->GetPrimAtPath(SdfPath("/World/Rig/Smooth")));
  VtIntArray counts, indices;
  strip.GetFaceVertexCountsAttr().Get(&counts);
  strip.GetFaceVertexIndicesAttr().Get(&indices);
  std::vector<GfVec3d> object_points;
  for (const auto& position : smooth.geometry->positions) {
    object_points.emplace_back(position[0], position[1], position[2]);
  }
  Skinned smooth_normals;
  smooth_normals.normals = SmoothNormals(object_points, counts, indices);
  Lotus::SceneMesh object_space = smooth;
  object_space.instance.world_from_object = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  CheckNormals(step + ": /World/Rig/Smooth", object_space, smooth_normals);

  // Each part of the split mesh holds its faces' posed points.
  const std::vector<GfVec3d>& parts = expected.at("/World/Rig/Parts").points;
  const auto check_part = [&](const char* suffix, std::size_t first) {
    const std::vector<GfVec3d> points =
        WorldPoints(meshes.at(std::string("/World/Rig/Parts.lotus:subset:") + suffix));
    Check(points.size() == 6, step + ": part " + suffix + " has " +
                                  std::to_string(points.size()) + " points");
    for (std::size_t i = first; i < first + 6; ++i) {
      bool found = false;
      for (const GfVec3d& point : points) {
        found = found || (point - parts[i]).GetLength() <= kPointTolerance;
      }
      Check(found, step + ": part " + suffix + " lacks point " + std::to_string(i));
    }
  };
  check_part("Lower", 0);
  check_part("Upper", 4);

  // UsdSkelImaging's computation warns about influences that do not match the
  // points and passes them on unskinned, as if they were in skeleton space.
  const UsdGeomMesh malformed(stage->GetPrimAtPath(SdfPath("/World/Rig/Malformed")));
  VtVec3fArray rest;
  malformed.GetPointsAttr().Get(&rest);
  const std::vector<GfVec3d> fallback = WorldPoints(meshes.at("/World/Rig/Malformed"));
  Check(fallback.size() == rest.size(), step + ": Malformed lost points");
  UsdGeomXformCache xforms((UsdTimeCode(time)));
  const GfMatrix4d malformed_world =
      xforms.GetLocalToWorldTransform(stage->GetPrimAtPath(SdfPath("/World/Rig/Skel")));
  for (std::size_t i = 0; i < rest.size(); ++i) {
    CheckClose(fallback[i], malformed_world.Transform(GfVec3d(rest[i])), kPointTolerance,
        step + ": Malformed's point " + std::to_string(i));
  }

  // Time-sampled points go on arriving through the points primvar.
  const UsdGeomMesh plain(stage->GetPrimAtPath(SdfPath("/World/Plain")));
  VtVec3fArray plain_points;
  plain.GetPointsAttr().Get(&plain_points, UsdTimeCode(time));
  const auto& plain_actual = meshes.at("/World/Plain").geometry->positions;
  for (std::size_t i = 0; i < plain_points.size(); ++i) {
    CheckClose(GfVec3d(plain_actual[i][0], plain_actual[i][1], plain_actual[i][2]),
        GfVec3d(plain_points[i]), 1e-6, step + ": Plain's point " + std::to_string(i));
  }
}

// The pixel a world point projects to.
std::array<int, 2> PixelOf(const UsdStageRefPtr& stage, const GfVec3d& point, double time) {
  const GfCamera camera =
      UsdGeomCamera(stage->GetPrimAtPath(SdfPath("/World/Camera"))).GetCamera(UsdTimeCode(time));
  const GfFrustum frustum = camera.GetFrustum();
  const GfVec3d clip =
      (frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix()).Transform(point);
  return {static_cast<int>((clip[0] * 0.5 + 0.5) * kSize),
      static_cast<int>((clip[1] * 0.5 + 0.5) * kSize)};
}

void RenderChecks(const std::string& step, Host& host, const UsdStageRefPtr& stage,
    const Oracle& oracle) {
  const auto alpha = [](const std::vector<float>& image, const std::array<int, 2>& at) {
    return image.at((static_cast<std::size_t>(at[1]) * kSize +
                        static_cast<std::size_t>(at[0])) * 4 + 3);
  };
  host.SyncAt(1);
  const std::vector<float> rest = host.Image();
  const Lotus::GpuSceneStats posed_from = host.delegate().GetGpuSceneStats();
  Check(posed_from.resident_geometries == 12 && posed_from.blas_count == 12,
      step + ": expected 12 resident geometries, got " +
          std::to_string(posed_from.resident_geometries));
  host.SyncAt(3);
  const std::vector<float> posed = host.Image();
  const Lotus::GpuSceneStats posed_to = host.delegate().GetGpuSceneStats();
  Check(posed_to.blas_builds == posed_from.blas_builds &&
            posed_to.resident_geometries == posed_from.resident_geometries,
      step + ": a time change rebuilt geometry instead of refitting it");
  Check(posed_to.blas_updates > posed_from.blas_updates &&
            posed_to.tlas_updates + posed_to.tlas_builds >
                posed_from.tlas_updates + posed_from.tlas_builds,
      step + ": a time change refit no acceleration structure");

  // The rigid mesh's far corner sweeps away from where it rested: samples of
  // the posed image hit the pixel it reaches, which the rest image leaves empty.
  const Skinned rigid = oracle.At(3).at("/World/Rig/Rigid");
  const auto pixel = PixelOf(stage, rigid.points[9], 3);
  Check(pixel[0] >= 0 && pixel[0] < kSize && pixel[1] >= 0 && pixel[1] < kSize,
      step + ": the posed corner is outside the image");
  Check(alpha(rest, pixel) == 0.0F && alpha(posed, pixel) > 0.5F,
      step + ": pixel (" + std::to_string(pixel[0]) + ", " + std::to_string(pixel[1]) +
          ") has alpha " + std::to_string(alpha(rest, pixel)) + " at rest and " +
          std::to_string(alpha(posed, pixel)) + " posed");
}

} // namespace

int main(int argc, char** argv) try {
  const std::string mode = argc > 1 ? argv[1] : "";
  const bool gpu = mode == "--gpu";
  const bool computed_normals = mode == "--computed-normals";
  if (gpu) {
    // Missing GPU capability is a CTest SKIP, never a passing GPU check.
    const auto shaders =
        Lotus::ShadersIn(std::filesystem::absolute(argv[0]).parent_path() / "shaders");
    Lotus::FrameStatus status;
    std::string error;
    const auto renderer = Lotus::CreateOffscreenRenderer(
        shaders.vertex, shaders.fragment, status, error);
    if (!renderer) {
      std::cerr << error << '\n';
      return status == Lotus::FrameStatus::Skip ? 77 : 1;
    }
    if (!renderer->RayQueryCapability().available) {
      std::cerr << "SKIP: " << renderer->RayQueryCapability().detail << '\n';
      return 77;
    }
  }
  const UsdStageRefPtr stage = UsdStage::Open(LOTUS_SKEL_FIXTURE);
  Check(stage != nullptr, "could not open the skinned fixture");
  const Oracle oracle(stage);

  for (const bool scene_indices : {true, false}) {
    const std::string front_end = scene_indices ? "scene indices" : "UsdImagingDelegate";
    Host host(stage, scene_indices, gpu);
    if (gpu) {
      RenderChecks(front_end, host, stage, oracle);
      continue;
    }
    Lotus::FrameSnapshot previous = host.SyncAt(1);
    const auto fixed = previous.scene->meshes.at("/World/Fixed").geometry;
    Compare(front_end + " at 1", previous, stage, oracle, 1, computed_normals);
    for (const double time : {2.0, 3.0, 2.5, 1.0}) {
      const std::string step = front_end + " at " + std::to_string(time);
      const Lotus::FrameSnapshot snapshot = host.SyncAt(time);
      Compare(step, snapshot, stage, oracle, time, computed_normals);
      Check(snapshot.revision != previous.revision,
          step + ": the time change left the scene as it was");
      Check(snapshot.scene->meshes.at("/World/Rig/Linear").geometry !=
                previous.scene->meshes.at("/World/Rig/Linear").geometry,
          step + ": the time change kept the posed geometry");
      Check(snapshot.scene->meshes.at("/World/Fixed").geometry == fixed,
          step + ": the time change replaced geometry that does not vary");
      Check(host.SyncAt(time).revision == snapshot.revision,
          step + ": a second sync at the same time changed the scene");
      previous = snapshot;
    }
  }
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
