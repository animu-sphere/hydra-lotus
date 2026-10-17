// SPDX-License-Identifier: Apache-2.0
#include "../adapters/headless/temporal_statistics.hpp"

#include <array>
#include <iostream>
#include <limits>

namespace {
void Check(bool condition, const char* detail) {
  if (!condition)
    throw std::runtime_error(detail);
}
void Near(double actual, double expected) {
  Check(std::abs(actual - expected) < 1e-10, "incorrect temporal statistic");
}
// Independent two-pass definition, including per-component centering and
// endpoint treatment. Compare a nonsymmetric, multichannel sequence.
void CompareDefinition(double offset) {
  const std::array<std::array<double, 3>, 5> frames{{{1, 4, -2}, {3, 4, 7}, {-1, 4, 3}, {6, 4, -5}, {2, 4, 1}}};
  LotusHeadless::TemporalResidualStatistics stats(3);
  for (auto frame : frames) {
    for (auto& value : frame)
      value += offset;
    stats.Add(frame);
  }
  double numerator = 0, denominator = 0, mean_squared = 0;
  for (unsigned c = 0; c < 3; ++c) {
    double mean = 0;
    for (const auto& frame : frames)
      mean += frame[c] / double(frames.size());
    mean_squared += (mean + offset) * (mean + offset);
    for (std::size_t f = 0; f < frames.size(); ++f) {
      const double centered = frames[f][c] - mean;
      denominator += centered * centered;
      if (f)
        numerator += centered * (frames[f - 1][c] - mean);
    }
  }
  // Large offsets have the ordinary Welford rounding error, far below float
  // radiance precision. Do not demand exact decimal arithmetic at that scale.
  const double tolerance = offset ? 1e-4 : 1e-10;
  Check(std::abs(*stats.Lag1Autocorrelation() - numerator / denominator) < tolerance,
      "lag-one disagrees with independent definition");
  Check(std::abs(stats.CenteredRmse() - std::sqrt(denominator / 15)) < tolerance,
      "centered error disagrees with independent definition");
  Check(std::abs(stats.MeanRmse() - std::sqrt(mean_squared / 3)) < (offset ? 1e-3 : 1e-10),
      "mean error disagrees with independent definition");
}

// Across-sequence decomposition, against a direct two-pass definition over
// three unequal two-component sequences.
void CompareEnsembleDefinition() {
  const std::array<std::array<std::array<double, 2>, 3>, 3> sequences{{
      {{{1, 2}, {3, -2}, {2, 6}}}, {{{-1, 0}, {0, 1}, {4, 5}}}, {{{2, 2}, {2, 2}, {5, -1}}}}};
  LotusHeadless::SequenceEnsembleStatistics ensemble(2);
  for (const auto& sequence : sequences) {
    LotusHeadless::TemporalResidualStatistics stats(2);
    for (const auto& frame : sequence)
      stats.Add(frame);
    ensemble.Add(stats);
  }
  double frame_variance = 0, mean_variance = 0, grand = 0;
  for (unsigned c = 0; c < 2; ++c) {
    std::array<double, 3> means{};
    for (unsigned k = 0; k < 3; ++k) {
      for (const auto& frame : sequences[k])
        means[k] += frame[c] / 3;
      for (const auto& frame : sequences[k])
        frame_variance += (frame[c] - means[k]) * (frame[c] - means[k]) / (2 * 3 * 2);
    }
    const double mean = (means[0] + means[1] + means[2]) / 3;
    for (double m : means)
      mean_variance += (m - mean) * (m - mean) / (2 * 2);
    grand += mean * mean / 2;
  }
  const auto summary = ensemble.Summarize();
  Check(summary.sequences == 3 && summary.frames == 3, "wrong ensemble population");
  Near(summary.frame_variance, frame_variance);
  Near(summary.sequence_mean_variance, mean_variance);
  Near(summary.grand_mean_mse, grand);
  Near(*summary.variance_inflation, 3 * mean_variance / frame_variance);

  // Frozen sequences have no frame variance, so no inflation is defined.
  LotusHeadless::SequenceEnsembleStatistics frozen(1);
  for (double value : {1., 3.}) {
    LotusHeadless::TemporalResidualStatistics stats(1);
    stats.Add(std::array{value});
    stats.Add(std::array{value});
    frozen.Add(stats);
  }
  const auto fixed = frozen.Summarize();
  Near(fixed.sequence_mean_variance, 2);
  Near(fixed.grand_mean_mse, 4);
  Check(!fixed.variance_inflation, "frozen sequences must have undefined inflation");

  // Mismatched extents, one-frame and unequal-length sequences reject.
  LotusHeadless::TemporalResidualStatistics wide(2), single(1), longer(1);
  wide.Add(std::array{1., 2.});
  wide.Add(std::array{1., 2.});
  single.Add(std::array{1.});
  for (double value : {1., 2., 3.})
    longer.Add(std::array{value});
  for (const auto* invalid : {&wide, &single, &longer}) {
    bool rejected = false;
    try {
      frozen.Add(*invalid);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    Check(rejected, "invalid sequence accepted");
  }
  Near(frozen.Summarize().grand_mean_mse, 4);
  LotusHeadless::SequenceEnsembleStatistics lone(1);
  LotusHeadless::TemporalResidualStatistics two(1);
  two.Add(std::array{1.});
  two.Add(std::array{2.});
  lone.Add(two);
  bool rejected = false;
  try {
    (void)lone.Summarize();
  } catch (const std::logic_error&) {
    rejected = true;
  }
  Check(rejected, "one sequence has no across-sequence variance");
}
} // namespace

int main() {
  try {
    CompareDefinition(0);
    CompareDefinition(1e12);
    LotusHeadless::TemporalResidualStatistics frozen(2);
    const std::array<double, 2> fixed{3, -3};
    frozen.Add(fixed);
    Check(!frozen.Lag1Autocorrelation(), "single frame has no correlation");
    for (unsigned i = 0; i < 3; ++i)
      frozen.Add(fixed);
    Near(frozen.MeanRmse(), 3);
    Near(frozen.CenteredRmse(), 0);
    Check(!frozen.Lag1Autocorrelation(), "frozen residual must be undefined, not zero");
    LotusHeadless::TemporalResidualStatistics alternating(1);
    for (double value : {1., -1., 1., -1.})
      alternating.Add(std::array{value});
    Near(alternating.MeanRmse(), 0);
    Near(alternating.CenteredRmse(), 1);
    Near(*alternating.Lag1Autocorrelation(), -0.75);
    LotusHeadless::TemporalResidualStatistics ramp(1);
    for (double value : {1., 2., 3., 4.})
      ramp.Add(std::array{value});
    Near(ramp.MeanRmse(), 2.5);
    Near(ramp.CenteredRmse(), std::sqrt(1.25));
    Near(*ramp.Lag1Autocorrelation(), 0.25);
    // Failed input must not alter the sequence.
    for (const auto& invalid : {std::vector<double>{}, std::vector<double>{1, 2},
             std::vector<double>{std::numeric_limits<double>::infinity()}}) {
      bool rejected = false;
      try {
        ramp.Add(invalid);
      } catch (const std::invalid_argument&) {
        rejected = true;
      }
      Check(rejected, "invalid residual accepted");
    }
    Near(*ramp.Lag1Autocorrelation(), 0.25);
    CompareEnsembleDefinition();
    std::cout << "Temporal residual statistics: independent definition, frozen, alternating, trend, precision and sequence ensemble passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
