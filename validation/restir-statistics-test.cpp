// SPDX-License-Identifier: Apache-2.0
#include "../adapters/headless/benchmarks/temporal_statistics.hpp"

#include <algorithm>
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

// Replication bias against a direct two-pass definition: a 3x2 image with
// 2-pixel tiles (one partial), two bands and a finite reference variance.
void CompareBiasDefinition() {
  constexpr unsigned width = 3, height = 2, replications = 4;
  std::array<std::array<double, width * height * 3>, replications> residuals{};
  std::array<double, width * height * 3> variance{};
  for (unsigned k = 0; k < replications; ++k)
    for (unsigned i = 0; i < width * height * 3; ++i) {
      residuals[k][i] = (int((i * 7 + k * 13) % 11) - 5) * 0.1 + 0.05 * k;
      variance[i] = 0.001 * (i % 4);
    }
  for (auto& residual : residuals)
    residual[0] += 2; // A persistent error in tile (0,0), red.
  LotusHeadless::ReplicationBiasStatistics stats(width, height, 2);
  for (const auto& residual : residuals)
    stats.Add(residual);
  auto mean = [&](unsigned i) {
    double sum = 0;
    for (const auto& residual : residuals)
      sum += residual[i];
    return sum / replications;
  };
  auto noise = [&](auto value) {
    double average = 0, squares = 0;
    for (unsigned k = 0; k < replications; ++k)
      average += value(k) / replications;
    for (unsigned k = 0; k < replications; ++k)
      squares += (value(k) - average) * (value(k) - average);
    return squares / (replications - 1) / replications;
  };
  auto region = [&](unsigned begin, unsigned end, bool reference) {
    double mse = 0, expected = 0, n = 0;
    for (unsigned y = 0; y < height; ++y)
      for (unsigned x = begin; x < end; ++x)
        for (unsigned c = 0; c < 3; ++c, ++n) {
          const unsigned i = (y * width + x) * 3 + c;
          mse += mean(i) * mean(i);
          expected += noise([&](unsigned k) { return residuals[k][i]; }) + (reference ? variance[i] : 0);
        }
    return std::array{mse / n, expected / n};
  };
  const std::array<LotusHeadless::ReplicationBiasStatistics::Band, 2> bands{{{0, 2}, {2, 3}}};
  const auto summary = stats.Summarize(variance, bands);
  Check(summary.replications == replications && summary.bands.size() == 2, "wrong replication population");
  Near(summary.image.mse, region(0, width, true)[0]);
  Near(summary.image.noise, region(0, width, true)[1]);
  for (unsigned b = 0; b < 2; ++b) {
    Near(summary.bands[b].mse, region(bands[b].begin, bands[b].end, true)[0]);
    Near(summary.bands[b].noise, region(bands[b].begin, bands[b].end, true)[1]);
  }
  unsigned beyond = 0;
  double max_z = 0;
  for (unsigned tx = 0; tx < 2; ++tx)
    for (unsigned c = 0; c < 3; ++c) {
      std::vector<unsigned> members;
      for (unsigned y = 0; y < height; ++y)
        for (unsigned x = tx * 2; x < std::min(width, tx * 2 + 2); ++x)
          members.push_back((y * width + x) * 3 + c);
      auto tile = [&](unsigned k) {
        double sum = 0;
        for (auto i : members)
          sum += residuals[k][i];
        return sum / members.size();
      };
      double shared = 0, average = 0;
      for (auto i : members)
        shared += variance[i];
      for (unsigned k = 0; k < replications; ++k)
        average += tile(k) / replications;
      const double z = average / std::sqrt(noise(tile) + shared / double(members.size() * members.size()));
      beyond += std::abs(z) > 3;
      if (std::abs(z) > std::abs(max_z))
        max_z = z;
    }
  Check(summary.tile_tests == 6 && summary.tiles_beyond_3 == beyond && beyond > 0, "wrong tile tests");
  Check(summary.max_tile && summary.max_tile->x == 0 && summary.max_tile->y == 0 &&
            summary.max_tile->channel == 0, "wrong largest tile");
  Near(summary.max_tile->z, max_z);
  // An exact reference adds no noise.
  Near(stats.Summarize({}, {}).image.noise, region(0, width, false)[1]);

  // Identical replications against an exact reference define no tile test.
  LotusHeadless::ReplicationBiasStatistics frozen(1, 1);
  for (unsigned k = 0; k < 2; ++k)
    frozen.Add(std::array{1., -2., 0.});
  const auto fixed = frozen.Summarize({}, {});
  Near(fixed.image.mse, 5. / 3);
  Near(fixed.image.noise, 0);
  Check(fixed.tile_tests == 0 && !fixed.max_tile, "zero noise must not define a tile test");

  // Invalid replications, bands, reference extents and populations reject.
  auto rejects = [](auto action) {
    try {
      action();
    } catch (const std::logic_error&) { // Includes std::invalid_argument.
      return true;
    }
    return false;
  };
  Check(rejects([&] { frozen.Add(std::array{1., 2.}); }), "wrong extent accepted");
  Check(rejects([&] { frozen.Add(std::array{1., std::numeric_limits<double>::quiet_NaN(), 0.}); }),
      "nonfinite replication accepted");
  Check(frozen.Replications() == 2, "rejected replication changed the population");
  const std::array<LotusHeadless::ReplicationBiasStatistics::Band, 1> empty_band{{{1, 1}}}, wide_band{{{0, 2}}};
  Check(rejects([&] { (void)frozen.Summarize({}, empty_band); }), "empty band accepted");
  Check(rejects([&] { (void)frozen.Summarize({}, wide_band); }), "band beyond the image accepted");
  Check(rejects([&] { (void)frozen.Summarize(std::array{1.}, {}); }), "wrong reference variance accepted");
  Check(rejects([] { LotusHeadless::ReplicationBiasStatistics(0, 1); }), "empty image accepted");
  LotusHeadless::ReplicationBiasStatistics lone(1, 1);
  lone.Add(std::array{1., 1., 1.});
  Check(rejects([&] { (void)lone.Summarize({}, {}); }), "one replication has no variance");
}

// Replication correlation against a direct two-pass definition: two pose
// pairs of a two-component image, pooled by summing their moments.
void CompareCorrelationDefinition() {
  constexpr unsigned replications = 5;
  std::array<std::array<std::array<double, 2>, replications>, 4> images{};
  for (unsigned k = 0; k < replications; ++k)
    for (unsigned c = 0; c < 2; ++c) {
      const double noise = (int((k * 7 + c * 3) % 5) - 2) * 0.3;
      images[0][k][c] = noise + 4 + c;               // Persistent offset.
      images[1][k][c] = 0.5 * noise + 0.1 * k * k;   // Partly shared noise.
      images[2][k][c] = (int((k * 3 + c) % 4) - 1.5) * 0.2 - 3;
      images[3][k][c] = -images[2][k][c] + 0.05 * c;  // Anticorrelated.
    }
  double cross = 0, first = 0, second = 0;
  std::array<LotusHeadless::ReplicationCorrelationStatistics::Sums, 2> sums;
  for (unsigned pair = 0; pair < 2; ++pair) {
    LotusHeadless::ReplicationCorrelationStatistics stats(2);
    for (unsigned k = 0; k < replications; ++k)
      stats.Add(images[pair * 2][k], images[pair * 2 + 1][k]);
    Check(stats.Replications() == replications, "wrong correlation population");
    sums[pair] = stats.Summarize();
    double pair_cross = 0, pair_first = 0, pair_second = 0;
    for (unsigned c = 0; c < 2; ++c) {
      double a = 0, b = 0;
      for (unsigned k = 0; k < replications; ++k) {
        a += images[pair * 2][k][c] / replications;
        b += images[pair * 2 + 1][k][c] / replications;
      }
      for (unsigned k = 0; k < replications; ++k) {
        const double da = images[pair * 2][k][c] - a, db = images[pair * 2 + 1][k][c] - b;
        pair_cross += da * db;
        pair_first += da * da;
        pair_second += db * db;
      }
    }
    Near(sums[pair].cross, pair_cross);
    Near(sums[pair].first, pair_first);
    Near(sums[pair].second, pair_second);
    cross += pair_cross;
    first += pair_first;
    second += pair_second;
  }
  Check(*LotusHeadless::ReplicationCorrelationStatistics::Correlation(sums[1]) < -0.99,
      "anticorrelated pair must be near -1");
  const LotusHeadless::ReplicationCorrelationStatistics::Sums pooled{
      sums[0].cross + sums[1].cross, sums[0].first + sums[1].first, sums[0].second + sums[1].second};
  Near(*LotusHeadless::ReplicationCorrelationStatistics::Correlation(pooled), cross / std::sqrt(first * second));

  // Identical replications have no variance, so correlation is undefined.
  LotusHeadless::ReplicationCorrelationStatistics frozen(1);
  for (unsigned k = 0; k < 2; ++k)
    frozen.Add(std::array{1.}, std::array{double(k)});
  Check(!LotusHeadless::ReplicationCorrelationStatistics::Correlation(frozen.Summarize()),
      "frozen side must have undefined correlation");
  auto rejects = [](auto action) {
    try {
      action();
    } catch (const std::logic_error&) {
      return true;
    }
    return false;
  };
  Check(rejects([&] { frozen.Add(std::array{1., 2.}, std::array{1.}); }), "wrong extent accepted");
  Check(rejects([&] { frozen.Add(std::array{1.}, std::array{std::numeric_limits<double>::infinity()}); }),
      "nonfinite correlation residual accepted");
  Check(frozen.Replications() == 2, "rejected pair changed the population");
  Check(rejects([] { LotusHeadless::ReplicationCorrelationStatistics(0); }), "empty image accepted");
  LotusHeadless::ReplicationCorrelationStatistics lone(1);
  lone.Add(std::array{1.}, std::array{2.});
  Check(rejects([&] { (void)lone.Summarize(); }), "one replication has no variance");
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
    CompareBiasDefinition();
    CompareCorrelationDefinition();
    std::cout << "Temporal residual statistics: independent definition, frozen, alternating, trend, precision, sequence ensemble, replication bias and correlation passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
