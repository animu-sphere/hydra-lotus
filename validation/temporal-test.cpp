// SPDX-License-Identifier: Apache-2.0
#include <lotus/temporal.hpp>
#include "../backend/vulkan/temporal_metadata.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

void Near(float actual, double expected, const char* message) {
  Check(std::abs(actual - expected) < 0.00002, message);
}

Lotus::Matrix4 Translation(float x, float y = 0, float z = 0) {
  auto result = Lotus::IdentityMatrix();
  result[12] = x;
  result[13] = y;
  result[14] = z;
  return result;
}

Lotus::Camera Perspective() {
  Lotus::Camera camera;
  // 90-degree vertical FOV, aspect 1, near 1, far 11, OpenGL clip space.
  camera.projection = {1, 0, 0, 0, 0, 1, 0, 0,
      0, 0, -1.2F, -1, 0, 0, -2.2F, 0};
  return camera;
}

const Lotus::MeshGeometry kTriangle{
    {{0, 0, -4}, {1, 0, -4}, {0, 1, -4}}, {{0, 1, 2}}, {0}};

void Lifecycle() {
  using Reason = Lotus::TemporalResetReason;
  Lotus::RenderWorld world;
  Lotus::TemporalHistory history;
  Lotus::TemporalFrameSettings settings{0, 128, 64};
  const auto snapshot = world.Commit();
  auto frame = history.Advance(snapshot, settings);
  Check(!frame.previous && frame.reset_reason == Reason::FirstFrame,
      "first frame had history");
  ++settings.frame_index;
  frame = history.Advance(snapshot, settings);
  Check(frame.previous && frame.reset_reason == Reason::None &&
            frame.previous->revision == frame.current.revision &&
            frame.previous->scene == frame.current.scene,
      "unchanged scene revision did not advance temporal frames");

  bool threw = false;
  try {
    (void)history.Advance(snapshot, settings);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  Check(threw, "duplicate frame was accepted");
  --settings.frame_index;
  threw = false;
  try {
    (void)history.Advance(snapshot, settings);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  Check(threw, "backwards frame index was accepted");
  settings.frame_index = 2;
  settings.width = 0;
  threw = false;
  try {
    (void)history.Advance(snapshot, settings);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  Check(threw, "empty frame was accepted");
  settings.width = 128;
  Check(history.Advance(snapshot, settings).previous.has_value(),
      "invalid input consumed the preceding frame");

  auto ExpectReset = [&](Reason reason) {
    frame = history.Advance(snapshot, settings);
    Check(!frame.previous && frame.reset_reason == reason, "wrong reset reason");
    ++settings.frame_index;
    settings.camera_cut = false;
    Check(history.Advance(snapshot, settings).previous.has_value(),
        "reset persisted beyond its frame");
    ++settings.frame_index;
  };
  ++settings.frame_index;
  settings.width = 256;
  ExpectReset(Reason::TargetChanged);
  settings.height = 128;
  ExpectReset(Reason::TargetChanged);
  ++settings.history_key;
  ExpectReset(Reason::HistoryKeyChanged);
  settings.frame_index += 2;
  ExpectReset(Reason::FrameGap);
  settings.camera_cut = true;
  ExpectReset(Reason::CameraCut);
  history.Reset();
  settings.frame_index = 0;
  ExpectReset(Reason::ExplicitReset);

  // Resource retention ends when the history and returned frames release it.
  std::weak_ptr<const Lotus::LotusScene> retained;
  {
    Lotus::RenderWorld temporary;
    const auto owned = temporary.Commit();
    retained = owned.scene;
    settings.frame_index = 0;
    history.Reset();
    (void)history.Advance(owned, settings);
  }
  Check(!retained.expired(), "history lost its immutable scene snapshot");
  history.Reset();
  Check(retained.expired(), "reset kept old scene resources alive");

  // Subtraction-based gap detection remains valid at the index limit.
  settings.frame_index = std::numeric_limits<std::uint64_t>::max() - 1;
  (void)history.Advance(snapshot, settings);
  ++settings.frame_index;
  Check(history.Advance(snapshot, settings).previous.has_value(),
      "last representable frame index lost history");
}

void Correspondence() {
  Lotus::RenderWorld world;
  Lotus::TemporalHistory history;
  Lotus::TemporalFrameSettings settings{0, 128, 64};
  Lotus::MeshInstance instance;
  world.SetMesh("/mesh", kTriangle, instance);
  auto Next = [&] {
    auto frame = history.Advance(world.Commit(), settings);
    ++settings.frame_index;
    return frame;
  };
  Check(!Lotus::FindTemporalPlacement(Next(), "/mesh"),
      "first frame had a previous placement");
  world.SetMesh("/a", kTriangle, {}); // Changes transient GPU table indices.
  instance.world_from_object = Translation(0.5F);
  world.SetMeshInstance("/mesh", instance);
  auto frame = Next();
  auto placement = Lotus::FindTemporalPlacement(frame, "/mesh");
  Check(placement && placement->material_unchanged &&
            placement->previous_world_from_object[12] == 0 &&
            placement->current_world_from_object[12] == 0.5F,
      "stable key correspondence depended on table order or lost old transform");
  Check(!Lotus::FindTemporalPlacement(frame, "/a") &&
            !Lotus::FindTemporalPlacement(frame, "/missing") &&
            !Lotus::FindTemporalPlacement(frame, "/mesh", 1),
      "a new/missing/out-of-range placement had correspondence");
  // Later commits must leave both of the returned frame's snapshots intact.
  instance.world_from_object = Translation(0.75F);
  world.SetMeshInstance("/mesh", instance);
  Check(Lotus::FindTemporalPlacement(frame, "/mesh")->current_world_from_object[12] == 0.5F,
      "later edits mutated a retained temporal frame");
  (void)Next();

  instance.instancer_transforms = std::vector<Lotus::Matrix4>{Translation(-0.25F), Translation(0.25F)};
  world.SetMeshInstance("/mesh", instance);
  Check(!Lotus::FindTemporalPlacement(Next(), "/mesh"),
      "instancing mode change retained history");
  instance.instancer_transforms->at(1) = Translation(0.5F);
  world.SetMeshInstance("/mesh", instance);
  placement = Lotus::FindTemporalPlacement(Next(), "/mesh", 1);
  Check(placement && placement->previous_world_from_object[12] == 1.0F &&
            placement->current_world_from_object[12] == 1.25F,
      "instancer and prototype transforms were not composed");
  instance.instancer_transforms->pop_back();
  world.SetMeshInstance("/mesh", instance);
  Check(!Lotus::FindTemporalPlacement(Next(), "/mesh"),
      "instancer count change reused ordinal history");
  instance.visible = false;
  world.SetMeshInstance("/mesh", instance);
  Check(!Lotus::FindTemporalPlacement(Next(), "/mesh"), "hidden mesh reused history");
  instance.visible = true;
  world.SetMeshInstance("/mesh", instance);
  Check(!Lotus::FindTemporalPlacement(Next(), "/mesh"), "revealed mesh reused hidden history");
  auto deformed = kTriangle;
  deformed.positions[0][0] = 0.1F;
  world.SetMesh("/mesh", deformed, instance);
  Check(!Lotus::FindTemporalPlacement(Next(), "/mesh"), "deformation reused rigid history");

  Lotus::Material material;
  world.SetMaterial("/material", material);
  world.BindMaterial("/mesh", "/material");
  placement = Lotus::FindTemporalPlacement(Next(), "/mesh");
  Check(placement && !placement->material_unchanged, "binding change retained material history");
  material.roughness = 0.25F;
  world.SetMaterial("/material", material);
  Check(!Lotus::FindTemporalPlacement(Next(), "/mesh")->material_unchanged,
      "material value change retained history");
  material.base_color_texture = Lotus::TextureInput{};
  material.base_color_texture->texture = "/texture";
  world.SetMaterial("/material", material);
  (void)Next();
  Lotus::Texture texture{1, 1, Lotus::TextureFormat::Rgba8Unorm, {64, 64, 64, 255}};
  world.SetTexture("/texture", texture);
  Check(!Lotus::FindTemporalPlacement(Next(), "/mesh")->material_unchanged,
      "missing-to-present texture retained material history");
  texture.texels[0] = 128;
  world.SetTexture("/texture", texture);
  Check(!Lotus::FindTemporalPlacement(Next(), "/mesh")->material_unchanged,
      "texture edit retained material history");
  world.SetTexture("/unrelated", texture);
  Check(Lotus::FindTemporalPlacement(Next(), "/mesh")->material_unchanged,
      "unrelated texture rejected material history");
  world.RemoveTexture("/texture");
  Check(!Lotus::FindTemporalPlacement(Next(), "/mesh")->material_unchanged,
      "texture removal retained material history");
  world.RemoveMaterial("/material");
  Check(!Lotus::FindTemporalPlacement(Next(), "/mesh")->material_unchanged,
      "material removal retained history");
  world.RemoveMesh("/mesh");
  Check(!Lotus::FindTemporalPlacement(Next(), "/mesh"), "removed mesh reused history");
  world.SetMesh("/mesh", kTriangle, {});
  Check(!Lotus::FindTemporalPlacement(Next(), "/mesh"), "reinserted mesh reused removed history");
}

Lotus::TemporalReprojection Reprojection() {
  Lotus::RenderWorld world;
  Lotus::TemporalHistory history;
  Lotus::TemporalFrameSettings settings{0, 128, 64};
  auto camera = Perspective();
  world.SetCamera(camera);
  world.SetMesh("/mesh", kTriangle, {});
  (void)history.Advance(world.Commit(), settings);
  ++settings.frame_index;
  auto frame = history.Advance(world.Commit(), settings);
  auto placement = Lotus::FindTemporalPlacement(frame, "/mesh");
  const auto stationary = Lotus::ReprojectTemporalPoint(frame, *placement, {1, 0.5F, -4});
  Check(stationary.has_value(), "static point failed reprojection");
  Near(stationary->current_pixel[0], 80, "wrong perspective X");
  Near(stationary->current_pixel[1], 28, "wrong top-left perspective Y");
  Check(stationary->motion == std::array<float, 2>{0, 0}, "static point had motion");
  camera.view = Translation(-0.25F, 0, 1);
  world.SetCamera(camera);
  Lotus::MeshInstance instance;
  instance.world_from_object = Translation(0.5F);
  world.SetMeshInstance("/mesh", instance);
  ++settings.frame_index;
  frame = history.Advance(world.Commit(), settings);
  Check(frame.previous.has_value(), "ordinary camera motion reset history");
  placement = Lotus::FindTemporalPlacement(frame, "/mesh");
  const auto moving = Lotus::ReprojectTemporalPoint(frame, *placement, {1, 0.5F, -4});
  Check(moving.has_value(), "moving surface failed reprojection");
  // Independent pinhole calculation: previous x/z = 1/4, current = 1.25/3.
  Near(moving->previous_pixel[0], 80, "wrong previous X");
  Near(moving->previous_pixel[1], 28, "wrong previous Y");
  Near(moving->current_pixel[0], (0.5 + 1.25 / 6) * 128, "wrong moving X");
  Near(moving->current_pixel[1], (0.5 - 0.5 / 6) * 64, "wrong moving Y");
  Near(moving->motion[0], -32.0 / 3, "wrong backward X motion");
  Near(moving->motion[1], 4.0 / 3, "wrong backward Y motion");
  Near(moving->previous_depth, 4, "expected depth used the current camera");

  camera = Perspective();
  frame.current.camera = camera;
  frame.previous->camera = camera;
  const Lotus::TemporalPlacement identity;
  auto Project = [&](std::array<float, 3> point) {
    return Lotus::ReprojectTemporalPoint(frame, identity, point);
  };
  Check(Project({-4, 4, -4}).has_value(), "top-left edge was rejected");
  Check(!Project({4, 0, -4}) && !Project({0, -4, -4}), "bottom/right edge was accepted");
  Check(!Project({0, 0, 4}) && !Project({0, 0, 0}) &&
            !Project({0, 0, -0.5F}) && !Project({0, 0, -12}),
      "clipped point was accepted");
  Check(!Project({std::numeric_limits<float>::quiet_NaN(), 0, -4}),
      "NaN point was accepted");
  frame.previous->camera.view[0] = std::numeric_limits<float>::infinity();
  Check(!Project({0, 0, -4}), "non-finite previous transform was accepted");
  frame.previous->camera = camera;
  frame.previous->camera.view = Translation(20);
  Check(!Project({0, 0, -4}), "point outside previous frame was accepted");
  frame.previous->camera = camera;
  frame.current.camera.projection = {};
  Check(!Project({0, 0, -4}), "zero clip W was accepted");
  // Orthographic camera: X/Y in [-2, 2], depth 1..11, translation in pixels
  // is independent of the surface's depth.
  camera.projection = {0.5F, 0, 0, 0, 0, 0.5F, 0, 0,
      0, 0, -0.2F, 0, 0, 0, -1.2F, 1};
  frame.previous->camera = camera;
  frame.current.camera = camera;
  frame.current.camera.view = Translation(-0.5F);
  const auto orthographic = Project({1, 0.5F, -4});
  Check(orthographic.has_value(), "orthographic point failed reprojection");
  Near(orthographic->previous_pixel[0], 96, "wrong orthographic previous X");
  Near(orthographic->current_pixel[0], 80, "wrong orthographic current X");
  Near(orthographic->motion[0], 16, "wrong orthographic backward motion");
  Near(orthographic->current_pixel[1], 24, "wrong orthographic Y");
  frame.previous.reset();
  Check(!Project({0, 0, -4}), "missing history was reprojected");
  return *moving;
}

void Validity(const Lotus::TemporalReprojection& reprojection) {
  using Rejection = Lotus::TemporalRejection;
  const Lotus::TemporalSurface current{true, 7, 3, 3, {0, 0, 2}};
  const Lotus::TemporalSurface previous{true, 7, 3, 4, {0, 0, 5}};
  auto Validate = [&](Lotus::TemporalSurface old) {
    return Lotus::ValidateTemporalHistory(reprojection, current, old);
  };
  Check(Validate(previous) == Rejection::None,
      "camera depth change rejected the same surface or normals were not normalized");
  auto old = previous;
  old.hit = false;
  Check(Validate(old) == Rejection::Miss, "miss reused history");
  old = previous;
  ++old.instance_id;
  Check(Validate(old) == Rejection::InstanceChanged, "instance change reused history");
  old = previous;
  ++old.material_id;
  Check(Validate(old) == Rejection::MaterialChanged, "material change reused history");
  old = previous;
  old.linear_depth = 3;
  Check(Validate(old) == Rejection::DepthDiscontinuity, "foreground disocclusion reused history");
  old.linear_depth = 5;
  Check(Validate(old) == Rejection::DepthDiscontinuity, "background disocclusion reused history");
  old = previous;
  old.normal = {0, 2, 0};
  Check(Validate(old) == Rejection::NormalDiscontinuity, "normal discontinuity reused history");
  old.normal = {0, 0, -5};
  Check(Validate(old) == Rejection::NormalDiscontinuity, "reversed normal reused history");
  old.normal = {};
  Check(Validate(old) == Rejection::InvalidData, "zero normal reused history");
  old = previous;
  old.linear_depth = std::numeric_limits<float>::quiet_NaN();
  Check(Validate(old) == Rejection::InvalidData, "NaN depth reused history");
  old = previous;
  old.normal[0] = std::numeric_limits<float>::infinity();
  Check(Validate(old) == Rejection::InvalidData, "infinite normal reused history");

  auto motion = reprojection;
  motion.motion = {24, 24}; // Euclidean length > 32, each component < 32.
  Check(Lotus::ValidateTemporalHistory(motion, current, previous) == Rejection::LargeMotion,
      "diagonal large motion reused history");
  motion.motion = {32, 0};
  Check(Lotus::ValidateTemporalHistory(motion, current, previous) == Rejection::None,
      "inclusive motion threshold rejected history");
  motion.motion[0] = std::numeric_limits<float>::quiet_NaN();
  Check(Lotus::ValidateTemporalHistory(motion, current, previous) == Rejection::InvalidData,
      "NaN motion reused history");
  Lotus::TemporalValidationSettings settings;
  settings.absolute_depth_tolerance = 0.125F;
  settings.relative_depth_tolerance = 0;
  old = previous;
  old.linear_depth = 4.125F;
  Check(Lotus::ValidateTemporalHistory(reprojection, current, old, settings) == Rejection::None,
      "inclusive absolute depth tolerance rejected history");
  settings.absolute_depth_tolerance = 0;
  settings.relative_depth_tolerance = 0.03125F;
  Check(Lotus::ValidateTemporalHistory(reprojection, current, old, settings) == Rejection::None,
      "relative depth tolerance used the wrong camera depth");
  old.linear_depth = 4.25F;
  Check(Lotus::ValidateTemporalHistory(reprojection, current, old, settings) == Rejection::DepthDiscontinuity,
      "depth outside tolerance reused history");
  settings.max_motion_pixels = -1;
  Check(Lotus::ValidateTemporalHistory(reprojection, current, previous, settings) == Rejection::InvalidData,
      "invalid validation settings reused history");
}

void WindowsAndIdentities() {
  using namespace Lotus;
  RenderWorld world;
  world.SetCamera(Perspective());
  world.SetMesh("/mesh", kTriangle, {});
  TemporalHistory history;
  TemporalFrameSettings settings{0, 128, 64};
  settings.display_window = {-16, 8, 160, 48};
  settings.data_window = {10, 12, 100, 40};
  auto frame = history.Advance(world.Commit(), settings);
  vulkan_internal::TemporalIdentities identities;
  const auto first = identities.Build(frame);
  ++settings.frame_index;
  frame = history.Advance(world.Commit(), settings);
  const auto placement = FindTemporalPlacement(frame, "/mesh");
  Check(placement.has_value(), "window frame placement missing");
  const auto point = ReprojectTemporalPoint(frame, *placement, {1, 0.5F, -4});
  Check(point.has_value(), "window projection missing");
  Near(point->current_pixel[0], 84, "offset display projection x");
  Near(point->current_pixel[1], 29, "offset display projection y");
  Check(!ReprojectTemporalPoint(frame, *placement, {-4, 0, -4}),
      "point outside cropped data window accepted");
  world.SetMesh("/a", kTriangle, {});
  world.SetMaterial("/a", {});
  ++settings.frame_index;
  frame = history.Advance(world.Commit(), settings);
  const auto shifted = identities.Build(frame);
  Check(shifted.size() == 2 && shifted[1].instance_id == first[0].instance_id &&
            shifted[1].material_id == first[0].material_id,
      "table insertion changed identities");
  settings.camera_cut = true;
  ++settings.frame_index;
  frame = history.Advance(world.Commit(), settings);
  const auto cut = identities.Build(frame);
  Check(cut[1].instance_id == shifted[1].instance_id && cut[1].flags == 0,
      "camera cut changed identity or retained correspondence");
  settings.camera_cut = false;
  settings.display_window[0] += 1;
  ++settings.frame_index;
  Check(history.Advance(world.Commit(), settings).reset_reason == TemporalResetReason::TargetChanged,
      "display-window change retained history");

  Material material;
  material.base_color_texture = TextureInput{};
  material.base_color_texture->texture = "/used";
  world.SetMaterial("/bound", material);
  world.BindMaterial("/mesh", "/bound");
  ++settings.frame_index;
  frame = history.Advance(world.Commit(), settings);
  const auto bound = identities.Build(frame);
  Check(bound[1].material_id != cut[1].material_id, "material binding reused identity");
  Texture texture{1, 1, TextureFormat::Rgba8Unorm, {64, 64, 64, 255}};
  world.SetTexture("/unused", texture);
  ++settings.frame_index;
  const auto unused = identities.Build(history.Advance(world.Commit(), settings));
  Check(unused[1].material_id == bound[1].material_id, "unused texture invalidated identity");
  world.SetTexture("/used", texture);
  ++settings.frame_index;
  const auto inserted = identities.Build(history.Advance(world.Commit(), settings));
  Check(inserted[1].material_id != unused[1].material_id && inserted[1].flags == 2,
      "used texture insertion retained identity");
  texture.texels[0] = 128;
  world.SetTexture("/used", texture);
  ++settings.frame_index;
  const auto replaced = identities.Build(history.Advance(world.Commit(), settings));
  Check(replaced[1].material_id != inserted[1].material_id, "texture replacement retained identity");
  world.RemoveMesh("/mesh");
  ++settings.frame_index;
  (void)identities.Build(history.Advance(world.Commit(), settings));
  world.SetMesh("/mesh", kTriangle, {});
  ++settings.frame_index;
  const auto reinserted = identities.Build(history.Advance(world.Commit(), settings));
  Check(reinserted[1].instance_id != first[0].instance_id && reinserted[1].flags == 0,
      "mesh reinsertion reused instance incarnation");
}

void PixelFetch() {
  using namespace Lotus;
  TemporalPixel current;
  current.flags = 7;
  current.instance_id = 7;
  current.material_id = 3;
  current.normal_depth = {0, 0, 1, 3};
  current.motion_depth = {0.75F, 0, 4, 1};
  std::array<TemporalPixel, 4> previous;
  previous.fill(current);
  for (auto& p : previous)
    p.normal_depth[3] = 4;
  auto Fetch = [&] {
    return ReprojectTemporalHistory(current, 0, 0, 2, 2, previous);
  };
  Check(Fetch().valid == 1 && Fetch().previous_index == 1,
      "nearest fetch or expected previous-camera depth");
  current.motion_depth[0] = 0.5F;
  Check(Fetch().previous_index == 1, "integer pixel boundary did not select right pixel");
  current.motion_depth[0] = -0.5F;
  Check(Fetch().valid == 1 && Fetch().previous_index == 0, "top-left edge excluded");
  current.motion_depth[0] = 1.5F;
  Check(Fetch().rejection == TemporalRejection::ReprojectionOutside &&
            Fetch().previous_index == UINT32_MAX,
      "right edge fetched out of bounds");
  current.motion_depth[0] = std::numeric_limits<float>::quiet_NaN();
  Check(Fetch().rejection == TemporalRejection::InvalidData, "nonfinite fetch accepted");
  current.motion_depth[0] = 0;
  Check(ReprojectTemporalHistory(current, 0, 0, 2, 2, {}).rejection ==
            TemporalRejection::HistoryUnavailable,
      "missing history fetched");
  current.flags = 1;
  Check(Fetch().rejection == TemporalRejection::CorrespondenceMissing, "missing rigid pair accepted");
  current.flags = 3;
  Check(Fetch().rejection == TemporalRejection::MaterialChanged, "changed material accepted");
  current.flags = 7;
  current.motion_depth[3] = 0;
  Check(Fetch().rejection == TemporalRejection::ReprojectionOutside, "failed projection fetched");
  current.motion_depth[3] = 1;
  previous[0].flags = 0;
  Check(Fetch().rejection == TemporalRejection::Miss && Fetch().previous_index == 0,
      "previous miss reused history");
  current.flags = 0;
  Check(ReprojectTemporalHistory(current, 0, 0, 2, 2, {}).rejection == TemporalRejection::Miss,
      "miss lost precedence over missing history");
  current.flags = 7;
  Check(ReprojectTemporalHistory(current, 0, 0, 2, 3, previous).rejection == TemporalRejection::InvalidData,
      "malformed history extent accepted");
}

} // namespace

int main() try {
  Lifecycle();
  Correspondence();
  Validity(Reprojection());
  WindowsAndIdentities();
  PixelFetch();
  std::cout << "temporal lifecycle, correspondence, reprojection and rejection PASS\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
