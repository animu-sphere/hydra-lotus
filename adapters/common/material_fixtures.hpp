// SPDX-License-Identifier: Apache-2.0
// The UsdPreviewSurface material fixtures (LOTUS-MATERIAL-01) as Lotus
// scenes: what the Hydra adapter extracts from the committed USD files in
// validation/materials/, which scripts/material_fixtures.py writes. The
// headless check renders these against the committed reference images, and
// the Hydra fixture test requires the USD files to extract to them.
#pragma once

#include <lotus/render_world.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace LotusHeadless {

// Replaces the world's camera and scene with material-basics-v1: a 4x4 grid
// of unit spheres scaled to 0.4 in front of a grey displayColor backdrop,
// under a constant dome and a rect key light. Rows from the top: base
// colours, dielectric roughness 0 to 1, metal roughness 0 to 1, then
// partial metallic, a metallic-workflow ior and the specular workflow.
// Each material holds what the translator makes of its authored inputs,
// clamped and ignored values included. Every camera ray hits the backdrop
// or a ball.
void SetMaterialBasics(Lotus::RenderWorld& world);

// Replaces the world's camera and scene with material-textures-v1: a 5x5
// grid of textured cards under material-basics-v1's camera, lights and
// backdrop. Rows from the top: wrap modes; colour spaces and channel counts;
// scalar channels, scale and bias, and lookups without an image;
// texture-coordinate sources; then connections the translator does not
// read, which take their defaults, and a minified checker. Texture keys name
// files relative to validation/materials/, where the translator names their
// resolved paths; an unresolved file keeps its authored path.
void SetMaterialTextures(Lotus::RenderWorld& world);

// Replaces the world's camera and scene with material-normals-v1: a 5x5 grid
// of coarse spheres and cards under material-basics-v1's camera, lights and
// backdrop. Rows from the top: authored mesh-normal interpolations; meshes
// without usable normals (computed for catmullClark, geometric for bilinear)
// and inward normals; normal-map encodings and a constant tangent normal;
// tangent frames under mirrored, rotated, mirrored-transform and stretched
// mappings and an absent texture-coordinate set; then normal maps on spheres
// with lat-long coordinates. Computed normals are Storm's, in float.
void SetMaterialNormals(Lotus::RenderWorld& world);

struct MaterialFixture {
  // Its USD file's and reference images' file name stem.
  std::string_view id;
  // Its default prim, whose child Camera sees it.
  std::string_view root;
  // The reference images' size, in pixels, square.
  std::uint32_t size;
  void (*set)(Lotus::RenderWorld&);
  // The comparison's negative control: a plausible defect, which the metric
  // must reject at 256 spp, and its edit of the mirror.
  std::string_view control;
  void (*apply_control)(Lotus::RenderWorld&);
};

// Every committed fixture, in the audit order.
std::span<const MaterialFixture> MaterialFixtures();

} // namespace LotusHeadless
