// SPDX-License-Identifier: Apache-2.0
// The renderer controls the viewport changes while it runs, and the frame
// they ask the renderer for. GPU-, window- and USD-free, so that a unit test
// covers them in every configuration; the developer UI and the command line
// only edit a Controls value.
#pragma once

#include <lotus/vulkan_backend.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace Lotus::viewport {

// Reservoir direct lighting at the camera surface (reference RESTIR.md).
enum class RestirMode {
  Off,
  // Initial-only reservoirs, accumulated progressively.
  Initial,
  // Reuse through RenderTemporalScene, one fresh sample per frame.
  Temporal,
  Spatial,
  Combined,
};

struct Controls {
  Integrator integrator = Integrator::Reference;
  // The samples per pixel at which a progressive accumulation stops.
  std::uint32_t samples = 1024;
  // False traces one fresh sample per pixel every frame instead of adding
  // to the accumulation.
  bool accumulate = true;
  // Next event estimation with MIS; off leaves BSDF sampling alone.
  // Reservoirs replace it at the camera surface.
  bool nee = true;
  RestirMode restir = RestirMode::Off;
  // Initial light draws per reservoir, 1..64.
  std::uint32_t restir_candidates = 8;
  // Temporal radiance reuse: bounded per-pixel history, one fresh sample
  // per frame.
  bool temporal_radiance = false;
  // Effective samples a pixel's radiance history keeps, 1..65536.
  std::uint32_t history_samples = 32;
  DebugView view = DebugView::Beauty;
  // The colour maps' spans: motion in pixels at full saturation, age in
  // frames at full heat, and log10 of the contribution weight.
  float motion_pixels = 8.0F;
  float age_frames = 32.0F;
  std::array<float, 2> weight_log10{-2.0F, 2.0F};

  bool operator==(const Controls&) const = default;
};

// What one frame asks of the renderer.
struct FramePlan {
  PathTracingSettings settings;
  // Render through RenderTemporalScene with `temporal`, rather than
  // RenderScene: for reuse, and for the temporal debug views' products.
  bool temporal_frame = false;
  TemporalFrameSettings temporal;
  // A view the controls cannot show (a reservoir view without reservoirs)
  // presents Beauty instead.
  PresentView present;
};

// Whether `view` has its product under `controls`: the reservoir views
// need reservoirs; every other view is always available.
[[nodiscard]] bool ViewAvailable(const Controls& controls, DebugView view);

// Whether the renderer adds each frame's sample to the accumulation
// (false: every frame is one fresh sample per pixel).
[[nodiscard]] bool Progressive(const Controls& controls);

// The frame `frame` (counted from 0 for the run) under `controls`.
// `depth_range` is the depth view's span, the scene camera's clipping range.
[[nodiscard]] FramePlan PlanFrame(const Controls& controls,
    std::uint64_t frame, std::array<float, 2> depth_range);

// Whether the frame after a change from `before` to `after` starts the
// accumulation again (1 sample per pixel) rather than adding to it: always
// without a progressive accumulation, and when the change reaches the
// backend's accumulation key (integrator, estimator, candidates).
[[nodiscard]] bool Restarts(const Controls& before, const Controls& after);

// Names, as the command line and the developer UI spell them.
[[nodiscard]] std::string_view Name(RestirMode mode);
[[nodiscard]] std::string_view Name(DebugView view);
[[nodiscard]] std::string_view Name(Integrator integrator);
[[nodiscard]] std::optional<RestirMode> ParseRestirMode(std::string_view name);
[[nodiscard]] std::optional<DebugView> ParseDebugView(std::string_view name);
[[nodiscard]] std::optional<Integrator> ParseIntegrator(std::string_view name);

inline constexpr std::array<RestirMode, 5> kRestirModes{RestirMode::Off,
    RestirMode::Initial, RestirMode::Temporal, RestirMode::Spatial,
    RestirMode::Combined};
inline constexpr std::array<DebugView, 8> kDebugViews{DebugView::Beauty,
    DebugView::Depth, DebugView::Normal, DebugView::Motion,
    DebugView::TemporalValidity, DebugView::ReservoirValidity,
    DebugView::ReservoirAge, DebugView::ReservoirWeight};

} // namespace Lotus::viewport
