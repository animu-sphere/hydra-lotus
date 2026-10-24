// SPDX-License-Identifier: Apache-2.0
#include "app/controls.hpp"

#include <algorithm>

namespace Lotus::viewport {
namespace {

bool Reuses(const Controls& controls) noexcept {
  return controls.restir == RestirMode::Temporal ||
         controls.restir == RestirMode::Spatial ||
         controls.restir == RestirMode::Combined;
}

// The denoiser filters the temporal radiance, which it needs.
bool AccumulatesRadiance(const Controls& controls) noexcept {
  return controls.temporal_radiance || controls.denoise;
}

// Reuse traces one fresh sample per frame, whatever `accumulate` says.
bool FreshBatch(const Controls& controls) noexcept {
  return Reuses(controls) || AccumulatesRadiance(controls);
}

bool TemporalView(DebugView view) noexcept {
  return view == DebugView::Depth || view == DebugView::Normal ||
         view == DebugView::Motion || view == DebugView::TemporalValidity;
}

bool RadianceView(DebugView view) noexcept {
  return view == DebugView::RawRadiance ||
         view == DebugView::AccumulatedRadiance ||
         view == DebugView::HistoryLength || view == DebugView::Variance;
}

bool ReservoirView(DebugView view) noexcept {
  return view == DebugView::ReservoirValidity ||
         view == DebugView::ReservoirAge || view == DebugView::ReservoirWeight;
}

struct Named {
  std::string_view name;
  int value;
};

constexpr Named kRestirNames[] = {{"off", 0}, {"initial", 1},
    {"temporal", 2}, {"spatial", 3}, {"combined", 4}};
constexpr Named kViewNames[] = {{"beauty", 0}, {"depth", 1}, {"normal", 2},
    {"motion", 3}, {"temporal-validity", 4}, {"reservoir-validity", 5},
    {"reservoir-age", 6}, {"reservoir-weight", 7}, {"raw", 8},
    {"accumulated", 9}, {"history-length", 10}, {"variance", 11}};
constexpr Named kIntegratorNames[] = {{"reference", 0}, {"wavefront", 1}};

template <std::size_t N>
std::string_view NameOf(const Named (&names)[N], int value) {
  for (const Named& named : names) {
    if (named.value == value) {
      return named.name;
    }
  }
  return "unknown";
}

template <typename Enum, std::size_t N>
std::optional<Enum> Parse(const Named (&names)[N], std::string_view name) {
  for (const Named& named : names) {
    if (named.name == name) {
      return static_cast<Enum>(named.value);
    }
  }
  return std::nullopt;
}

} // namespace

bool ViewAvailable(const Controls& controls, DebugView view) {
  if (ReservoirView(view)) {
    return controls.restir != RestirMode::Off;
  }
  return !RadianceView(view) || AccumulatesRadiance(controls);
}

bool Progressive(const Controls& controls) {
  return controls.accumulate && !FreshBatch(controls);
}

FramePlan PlanFrame(const Controls& controls, std::uint64_t frame,
    std::array<float, 2> depth_range) {
  FramePlan plan;
  PathTracingSettings& settings = plan.settings;
  settings.integrator = controls.integrator;
  settings.max_samples = std::max(controls.samples, 1U);
  settings.light_sampling =
      controls.nee ? LightSampling::Mis : LightSampling::Bsdf;
  settings.restir_candidates = std::clamp(controls.restir_candidates, 1U, 64U);
  if (controls.restir != RestirMode::Off) {
    settings.light_sampling = LightSampling::RestirInitial;
    settings.restir_temporal = controls.restir == RestirMode::Temporal ||
                               controls.restir == RestirMode::Combined;
    settings.restir_spatial = controls.restir == RestirMode::Spatial ||
                              controls.restir == RestirMode::Combined;
  }
  // Without accumulation, each frame's sample sequence differs, which
  // restarts the accumulation. A fresh batch advances its own sequence; a
  // changing sample index would reset its history instead.
  if (!controls.accumulate && !FreshBatch(controls)) {
    settings.sample_index = static_cast<std::uint32_t>(frame);
  }
  plan.temporal.accumulate_radiance = AccumulatesRadiance(controls);
  plan.temporal.max_history_samples =
      std::clamp(controls.history_samples, 1U, 65536U);
  if (controls.denoise) {
    plan.temporal.filter_iterations = std::clamp(
        controls.filter_iterations, 1U, kTemporalFilterMaxIterations);
  }
  plan.temporal_frame = FreshBatch(controls) || TemporalView(controls.view);

  PresentView& present = plan.present;
  present.view =
      ViewAvailable(controls, controls.view) ? controls.view : DebugView::Beauty;
  switch (present.view) {
  case DebugView::Depth:
    present.range = depth_range;
    break;
  case DebugView::Motion:
    present.range = {0.0F, controls.motion_pixels};
    break;
  case DebugView::ReservoirAge:
    present.range = {0.0F, controls.age_frames};
    break;
  case DebugView::ReservoirWeight:
    present.range = controls.weight_log10;
    break;
  case DebugView::HistoryLength:
    present.range = {0.0F,
        static_cast<float>(plan.temporal.max_history_samples)};
    break;
  case DebugView::Variance:
    present.range = controls.variance_log10;
    break;
  default:
    break;
  }
  if (controls.split && present.view != DebugView::Beauty) {
    present.split = std::clamp(controls.split_position, 0.0F, 1.0F);
  }
  return plan;
}

bool Restarts(const Controls& before, const Controls& after) {
  if (!Progressive(before) || !Progressive(after)) {
    return true;
  }
  const PathTracingSettings a = PlanFrame(before, 0, {}).settings;
  const PathTracingSettings b = PlanFrame(after, 0, {}).settings;
  return a.integrator != b.integrator ||
         a.light_sampling != b.light_sampling ||
         a.restir_candidates != b.restir_candidates;
}

std::string_view Name(RestirMode mode) {
  return NameOf(kRestirNames, static_cast<int>(mode));
}

std::string_view Name(DebugView view) {
  return NameOf(kViewNames, static_cast<int>(view));
}

std::string_view Name(Integrator integrator) {
  return NameOf(kIntegratorNames, static_cast<int>(integrator));
}

std::optional<RestirMode> ParseRestirMode(std::string_view name) {
  return Parse<RestirMode>(kRestirNames, name);
}

std::optional<DebugView> ParseDebugView(std::string_view name) {
  return Parse<DebugView>(kViewNames, name);
}

std::optional<Integrator> ParseIntegrator(std::string_view name) {
  return Parse<Integrator>(kIntegratorNames, name);
}

} // namespace Lotus::viewport
