// SPDX-License-Identifier: Apache-2.0
// lotus-viewport's renderer controls and the frames they plan, without a
// window or a GPU.
#include "../adapters/viewport/controls.hpp"

#include <iostream>
#include <string>
#include <tuple>

namespace {

using Lotus::DebugView;
using Lotus::Integrator;
using Lotus::LightSampling;
using Lotus::viewport::Controls;
using Lotus::viewport::FramePlan;
using Lotus::viewport::Name;
using Lotus::viewport::PlanFrame;
using Lotus::viewport::Progressive;
using Lotus::viewport::Restarts;
using Lotus::viewport::RestirMode;
using Lotus::viewport::ViewAvailable;

int failures = 0;

void Check(bool condition, const std::string& what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
  }
}

FramePlan Plan(const Controls& controls, std::uint64_t frame = 0) {
  return PlanFrame(controls, frame, {0.5F, 40.0F});
}

} // namespace

int main() {
  const Controls defaults;
  const FramePlan base = Plan(defaults, 7);
  Check(base.settings.integrator == Integrator::Reference &&
            base.settings.light_sampling == LightSampling::Mis &&
            base.settings.max_samples == 1024 &&
            base.settings.sample_index == 0 && !base.settings.restir_temporal &&
            !base.settings.restir_spatial,
      "defaults plan a progressive NEE/MIS frame");
  Check(!base.temporal_frame && !base.temporal.accumulate_radiance &&
            base.present.view == DebugView::Beauty,
      "defaults render through RenderScene and present beauty");
  Check(Progressive(defaults), "defaults accumulate");

  Controls controls = defaults;
  controls.nee = false;
  Check(Plan(controls).settings.light_sampling == LightSampling::Bsdf,
      "NEE off samples the BSDF alone");
  controls.restir = RestirMode::Initial;
  const FramePlan initial = Plan(controls);
  Check(initial.settings.light_sampling == LightSampling::RestirInitial &&
            !initial.settings.restir_temporal &&
            !initial.settings.restir_spatial && !initial.temporal_frame,
      "initial-only reservoirs replace NEE and stay progressive");
  Check(Progressive(controls), "initial-only reservoirs accumulate");
  for (const auto [mode, temporal, spatial] :
      {std::tuple{RestirMode::Temporal, true, false},
          std::tuple{RestirMode::Spatial, false, true},
          std::tuple{RestirMode::Combined, true, true}}) {
    controls.restir = mode;
    const FramePlan reuse = Plan(controls, 9);
    Check(reuse.settings.restir_temporal == temporal &&
              reuse.settings.restir_spatial == spatial &&
              reuse.temporal_frame && reuse.settings.sample_index == 0 &&
              !Progressive(controls),
        std::string(Name(mode)) + " reuse renders fresh temporal frames");
  }

  controls = defaults;
  controls.restir_candidates = 200;
  Check(Plan(controls).settings.restir_candidates == 64,
      "candidates clamp to 64");
  controls.history_samples = 0;
  Check(Plan(controls).temporal.max_history_samples == 1,
      "history samples clamp to 1");
  controls = defaults;
  controls.temporal_radiance = true;
  const FramePlan radiance = Plan(controls, 3);
  Check(radiance.temporal_frame && radiance.temporal.accumulate_radiance &&
            radiance.settings.sample_index == 0 && !Progressive(controls),
      "temporal radiance renders fresh temporal frames");
  controls = defaults;
  controls.accumulate = false;
  Check(Plan(controls, 5).settings.sample_index == 5 &&
            Plan(controls, 6).settings.sample_index == 6 &&
            !Progressive(controls),
      "no accumulation draws a new sequence every frame");

  // Views: the temporal ones render temporal frames; the reservoir ones need
  // reservoirs and fall back to beauty without them.
  controls = defaults;
  for (const DebugView view : {DebugView::Depth, DebugView::Normal,
           DebugView::Motion, DebugView::TemporalValidity}) {
    controls.view = view;
    const FramePlan plan = Plan(controls);
    Check(ViewAvailable(controls, view) && plan.temporal_frame &&
              plan.present.view == view && Progressive(controls),
        std::string(Name(view)) + " renders a progressive temporal frame");
  }
  controls.view = DebugView::Depth;
  Check(Plan(controls).present.range[0] == 0.5F &&
            Plan(controls).present.range[1] == 40.0F,
      "depth spans the clipping range");
  controls.view = DebugView::Motion;
  Check(Plan(controls).present.range[1] == controls.motion_pixels,
      "motion spans its pixels");
  for (const DebugView view : {DebugView::ReservoirValidity,
           DebugView::ReservoirAge, DebugView::ReservoirWeight}) {
    controls.view = view;
    controls.restir = RestirMode::Off;
    Check(!ViewAvailable(controls, view) &&
              Plan(controls).present.view == DebugView::Beauty,
        std::string(Name(view)) + " falls back without reservoirs");
    controls.restir = RestirMode::Initial;
    const FramePlan plan = Plan(controls);
    Check(ViewAvailable(controls, view) && plan.present.view == view &&
              !plan.temporal_frame,
        std::string(Name(view)) + " presents initial reservoirs");
  }
  controls.view = DebugView::ReservoirAge;
  Check(Plan(controls).present.range[1] == controls.age_frames,
      "age spans its frames");
  controls.view = DebugView::ReservoirWeight;
  Check(Plan(controls).present.range == controls.weight_log10,
      "weight spans its decades");

  // The denoiser filters the temporal radiance, which it turns on; the
  // radiance views need one of the two.
  controls = defaults;
  controls.denoise = true;
  const FramePlan denoised = Plan(controls, 4);
  Check(denoised.temporal_frame && denoised.temporal.accumulate_radiance &&
            denoised.temporal.filter_iterations ==
                Lotus::kTemporalFilterIterations &&
            denoised.settings.sample_index == 0 && !Progressive(controls),
      "denoise renders filtered fresh temporal frames");
  controls.filter_iterations = 0;
  Check(Plan(controls).temporal.filter_iterations == 1,
      "filter iterations clamp to 1");
  controls.filter_iterations = 99;
  Check(Plan(controls).temporal.filter_iterations ==
            Lotus::kTemporalFilterMaxIterations,
      "filter iterations clamp to the maximum");
  controls = defaults;
  controls.temporal_radiance = true;
  Check(Plan(controls).temporal.filter_iterations == 0,
      "temporal radiance alone does not filter");
  for (const DebugView view : {DebugView::RawRadiance,
           DebugView::AccumulatedRadiance, DebugView::HistoryLength,
           DebugView::Variance}) {
    controls = defaults;
    controls.view = view;
    Check(!ViewAvailable(controls, view) &&
              Plan(controls).present.view == DebugView::Beauty,
        std::string(Name(view)) + " falls back without temporal radiance");
    controls.temporal_radiance = true;
    Check(ViewAvailable(controls, view) &&
              Plan(controls).present.view == view,
        std::string(Name(view)) + " presents temporal radiance");
    controls.temporal_radiance = false;
    controls.denoise = true;
    Check(ViewAvailable(controls, view) &&
              Plan(controls).present.view == view,
        std::string(Name(view)) + " presents the denoiser's input");
  }
  controls.view = DebugView::HistoryLength;
  controls.history_samples = 12;
  Check(Plan(controls).present.range[1] == 12.0F,
      "history length spans the history samples");
  controls.view = DebugView::Variance;
  Check(Plan(controls).present.range == controls.variance_log10,
      "variance spans its decades");
  Check(Plan(controls).present.split == 1.0F,
      "without a split the view fills the frame");
  controls.split = true;
  controls.split_position = 0.25F;
  Check(Plan(controls).present.split == 0.25F, "the split position");
  controls.split_position = 2.0F;
  Check(Plan(controls).present.split == 1.0F, "the split clamps");
  controls.view = DebugView::Beauty;
  Check(Plan(controls).present.split == 1.0F, "beauty is not split");

  // Restarts follow the backend's accumulation key.
  Controls after = defaults;
  after.integrator = Integrator::Wavefront;
  Check(Restarts(defaults, after), "the integrator restarts");
  after = defaults;
  after.nee = false;
  Check(Restarts(defaults, after), "the estimator restarts");
  after = defaults;
  after.restir = RestirMode::Initial;
  Check(Restarts(defaults, after), "initial reservoirs restart");
  Controls before = after;
  after.restir_candidates = 4;
  Check(Restarts(before, after), "the candidate count restarts");
  after = defaults;
  after.samples = 16;
  Check(!Restarts(defaults, after), "the sample limit continues");
  after = defaults;
  after.view = DebugView::Normal;
  Check(!Restarts(defaults, after) && !Restarts(after, defaults),
      "a temporal view continues");
  after = defaults;
  after.restir = RestirMode::Combined;
  Check(Restarts(defaults, after) && Restarts(after, defaults),
      "reuse restarts, and so does leaving it");
  after = defaults;
  after.temporal_radiance = true;
  Check(Restarts(defaults, after) && Restarts(after, defaults),
      "temporal radiance restarts, and so does leaving it");
  after = defaults;
  after.accumulate = false;
  Check(Restarts(defaults, after) && Restarts(after, defaults),
      "accumulation off restarts, and so does turning it on");
  after = defaults;
  after.denoise = true;
  Check(Restarts(defaults, after) && Restarts(after, defaults),
      "the denoiser leaves the progressive accumulation");

  for (const RestirMode mode : Lotus::viewport::kRestirModes) {
    Check(Lotus::viewport::ParseRestirMode(Name(mode)) == mode,
        "restir mode names round-trip");
  }
  for (const DebugView view : Lotus::viewport::kDebugViews) {
    Check(Lotus::viewport::ParseDebugView(Name(view)) == view,
        "view names round-trip");
  }
  for (const Integrator integrator :
      {Integrator::Reference, Integrator::Wavefront}) {
    Check(Lotus::viewport::ParseIntegrator(Name(integrator)) == integrator,
        "integrator names round-trip");
  }
  Check(!Lotus::viewport::ParseDebugView("Depth"), "names are lower case");
  return failures == 0 ? 0 : 1;
}
