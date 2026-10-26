// SPDX-License-Identifier: Apache-2.0
// The UsdPreviewSurface material fixtures (LOTUS-MATERIAL-01) as Lotus
// scenes: what the Hydra adapter extracts from the committed USD files in
// validation/materials/, which scripts/material_fixtures.py writes. The
// headless check renders these against the committed reference images, and
// the Hydra fixture test requires the USD files to extract to them.
#pragma once

#include <lotus/render_world.hpp>

#include <cstdint>
#include <string_view>

namespace LotusHeadless {

// The fixture's id: its USD file's and reference images' file name stem.
inline constexpr std::string_view kMaterialBasics = "material-basics-v1";

// The reference images' size, in pixels, square.
inline constexpr std::uint32_t kMaterialFixtureSize = 128;

// Replaces the world's camera and scene with material-basics-v1: a 4x4 grid
// of unit spheres scaled to 0.4 in front of a grey displayColor backdrop,
// under a constant dome and a rect key light. Rows from the top: base
// colours, dielectric roughness 0 to 1, metal roughness 0 to 1, then
// partial metallic, a metallic-workflow ior and the specular workflow.
// Each material holds what the translator makes of its authored inputs,
// clamped and ignored values included. Every camera ray hits the backdrop
// or a ball.
void SetMaterialBasics(Lotus::RenderWorld& world);

// The key of the material whose base colour the comparison's negative
// control scales: the half-rough metal.
inline constexpr std::string_view kMaterialBasicsControl =
    "/MaterialBasics/Looks/Metal50";

} // namespace LotusHeadless
