// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <lotus/render_world.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

namespace LotusHeadless {

// Versioned procedural fixtures shared with the viewport. The first six
// retain the original benchmark's order and geometry.
inline constexpr std::array<std::string_view, 7> kWorkloadIds{
    "cornell-v1", "textured-v1", "instanced-v1", "geometry-v1",
    "lights-v1", "environment-v1", "many-lights-v1"};

inline bool KnownWorkload(std::string_view id) {
  for (const auto known : kWorkloadIds) if (known == id) return true;
  return false;
}

struct Workload {
  Lotus::RenderWorld world;
  std::string edit_mesh;
};

// many-lights-v1's sphere lights: a grid of side x side over the fixture's
// footprint at its total power. The fixture is the full grid, 16 x 16.
inline constexpr unsigned kManyLightGrid = 16;
void SetLightGrid(Lotus::RenderWorld& world, unsigned side);

// Environment images are read relative to the executable's licensed assets.
Workload MakeWorkload(std::string_view id, const std::filesystem::path& assets);

} // namespace LotusHeadless
