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
    std::cout << "Temporal residual statistics: independent definition, frozen, alternating, trend and precision passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
