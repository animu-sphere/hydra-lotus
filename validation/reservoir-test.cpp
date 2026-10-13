// SPDX-License-Identifier: Apache-2.0
#include "../backend/vulkan/reservoir.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace Lotus::vulkan_internal;

void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

void Near(double actual, double expected, double tolerance, const char* message) {
  Check(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}

bool Same(const Reservoir& a, const Reservoir& b) {
  return a.sample == b.sample && a.weight_sum == b.weight_sum &&
         a.selected_target == b.selected_target && a.candidate_count == b.candidate_count;
}

template <class Exception, class Operation>
void RejectUnchanged(Reservoir& reservoir, Operation operation) {
  const Reservoir before = reservoir;
  bool rejected = false;
  try {
    operation();
  } catch (const Exception&) {
    rejected = true;
  }
  Check(rejected, "invalid reservoir operation was accepted");
  Check(Same(reservoir, before), "failed reservoir operation changed state");
}

void Boundaries() {
  Reservoir reservoir;
  Check(reservoir.sample == kNoReservoirSample && ReservoirNormalization(reservoir) == 0,
      "empty reservoir has a sample or nonzero normalization");
  Check(!StreamReservoirCandidate(reservoir, kNoReservoirSample, 0, 0.25F, 0),
      "zero target selected a sample at u=0");
  Check(reservoir.candidate_count == 1 && reservoir.weight_sum == 0 &&
            ReservoirNormalization(reservoir) == 0,
      "zero target was not counted");
  Check(StreamReservoirCandidate(reservoir, 17, 2, 0.5F, std::nextafter(1.0F, 0.0F)),
      "first positive weight was not selected");
  Check(reservoir.sample == 17 && reservoir.selected_target == 2 &&
            reservoir.weight_sum == 4 && reservoir.candidate_count == 2,
      "candidate state mismatch");
  Near(ReservoirNormalization(reservoir), 1, 0, "zero draw omitted from normalization");

  // Equal weights: equality retains the old sample, immediately below replaces.
  const Reservoir first = reservoir;
  Check(!StreamReservoirCandidate(reservoir, 99, 1, 0.25F, 0.5F),
      "replacement boundary must be strict");
  Check(reservoir.sample == 17 && reservoir.selected_target == 2 &&
            reservoir.candidate_count == 3 && reservoir.weight_sum == 8,
      "retention lost target or count");
  reservoir = first;
  Check(StreamReservoirCandidate(reservoir, 99, 1, 0.25F, std::nextafter(0.5F, 0.0F)),
      "candidate below replacement boundary was retained");
  Check(reservoir.sample == 99 && reservoir.selected_target == 1,
      "selected target did not follow replacement");
  Near(ReservoirNormalization(reservoir), 8.0 / 3, 1e-15, "wrong normalization");
  const auto positive = reservoir;
  Check(!StreamReservoirCandidate(reservoir, 23, 0, 0.5F, 0),
      "zero target replaced a positive sample");
  Check(reservoir.sample == positive.sample &&
            reservoir.selected_target == positive.selected_target &&
            reservoir.weight_sum == positive.weight_sum && reservoir.candidate_count == 4,
      "zero target changed selected sample or lost count");

  Reservoir zeros;
  for (int i = 0; i < 4; ++i)
    (void)StreamReservoirCandidate(zeros, 3, 0, 1, 0);
  Check(!MergeSameTargetReservoir(reservoir, zeros, 0), "zero stream selected a sample");
  Check(reservoir.candidate_count == 8 && reservoir.weight_sum == 8,
      "zero stream's full M was not merged");
  Check(!MergeSameTargetReservoir(reservoir, {}, 0), "empty stream selected a sample");
  Check(reservoir.candidate_count == 8, "empty stream changed count");
  Reservoir destination;
  Check(MergeSameTargetReservoir(destination, reservoir, 0), "nonempty source lost");
  Check(Same(destination, reservoir), "merge into empty destination changed source");
  const auto normalization = ReservoirNormalization(destination);
  const auto count = destination.candidate_count;
  (void)MergeSameTargetReservoir(destination, destination, 0);
  Check(destination.candidate_count == 2 * count, "self merge lost count");
  Near(ReservoirNormalization(destination), normalization, 0, "self merge changed W");

  const float maximum = std::numeric_limits<float>::max();
  Reservoir large{3, maximum, maximum, 2};
  Near(ReservoirNormalization(large), 0.5, 0, "normalization denominator overflowed");
  const float tiny = std::numeric_limits<float>::denorm_min();
  Reservoir small;
  Check(StreamReservoirCandidate(small, 4, tiny, 1, 0), "representable subnormal lost");
  Near(ReservoirNormalization(small), 1, 0, "subnormal normalization failed");
  Reservoir full{3, 1, 1, std::numeric_limits<std::uint32_t>::max() - 1};
  (void)StreamReservoirCandidate(full, 3, 0, 1, 0);
  Check(full.candidate_count == std::numeric_limits<std::uint32_t>::max(),
      "maximum representable count rejected");
}

void InvalidInputs() {
  Reservoir reservoir;
  (void)StreamReservoirCandidate(reservoir, 7, 2, 0.25F, 0);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float infinity = std::numeric_limits<float>::infinity();
  for (const float target : {-1.0F, nan, infinity, -infinity})
    RejectUnchanged<std::invalid_argument>(reservoir, [&] {
      (void)StreamReservoirCandidate(reservoir, 1, target, 1, 0);
    });
  for (const float pdf : {0.0F, -1.0F, nan, infinity, -infinity})
    RejectUnchanged<std::invalid_argument>(reservoir, [&] {
      (void)StreamReservoirCandidate(reservoir, 1, 1, pdf, 0);
    });
  const Reservoir source = reservoir;
  for (const float random : {-1.0F, 1.0F, nan, infinity, -infinity}) {
    RejectUnchanged<std::invalid_argument>(reservoir, [&] {
      (void)StreamReservoirCandidate(reservoir, 1, 1, 1, random);
    });
    RejectUnchanged<std::invalid_argument>(reservoir, [&] {
      (void)MergeSameTargetReservoir(reservoir, source, random);
    });
  }
  RejectUnchanged<std::invalid_argument>(reservoir, [&] {
    (void)StreamReservoirCandidate(reservoir, kNoReservoirSample, 1, 1, 0);
  });
  const float maximum = std::numeric_limits<float>::max();
  RejectUnchanged<std::overflow_error>(reservoir, [&] {
    (void)StreamReservoirCandidate(reservoir, 1, maximum, 0.5F, 0);
  });
  RejectUnchanged<std::overflow_error>(reservoir, [&] {
    (void)StreamReservoirCandidate(reservoir, 1,
        std::numeric_limits<float>::denorm_min(), maximum, 0);
  });
  Reservoir large{1, maximum, 1, 1};
  RejectUnchanged<std::overflow_error>(large, [&] {
    (void)StreamReservoirCandidate(large, 1, maximum, 1, 0);
  });
  RejectUnchanged<std::overflow_error>(large, [&] {
    (void)MergeSameTargetReservoir(large, large, 0);
  });
  Reservoir full{1, 1, 1, std::numeric_limits<std::uint32_t>::max()};
  RejectUnchanged<std::overflow_error>(full, [&] {
    (void)StreamReservoirCandidate(full, 1, 0, 1, 0);
  });
  RejectUnchanged<std::overflow_error>(full, [&] {
    (void)MergeSameTargetReservoir(full, source, 0);
  });

  const std::array<Reservoir, 10> malformed{{{1, 0, 0, 1}, {kNoReservoirSample, 0, 1, 1},
      {kNoReservoirSample, 1, 1, 1}, {1, 1, 0, 1}, {1, 1, 1, 0},
      {1, -1, 1, 1}, {1, nan, 1, 1}, {1, infinity, 1, 1},
      {1, 1, nan, 1}, {1, 1, -1, 1}}};
  for (auto invalid : malformed) {
    RejectUnchanged<std::invalid_argument>(reservoir, [&] {
      (void)MergeSameTargetReservoir(reservoir, invalid, 0);
    });
    bool rejected = false;
    try {
      (void)ReservoirNormalization(invalid);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    Check(rejected, "malformed reservoir normalized");
    // Bitwise comparison is required to check preservation of NaN records.
    const auto before = std::bit_cast<std::array<std::uint32_t, 4>>(invalid);
    rejected = false;
    try {
      (void)StreamReservoirCandidate(invalid, 1, 1, 1, 0);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    Check(rejected && std::bit_cast<std::array<std::uint32_t, 4>>(invalid) == before,
        "malformed destination was accepted or changed");
  }
}

using Triple = std::array<double, 3>;
struct Outcome {
  Reservoir reservoir;
  double probability = 0;
};

// Enumerate each proposal draw and each retain/replace branch. Probabilities
// come from an independent double-precision prefix sum, never from the record
// under test. Fixed inputs have binary-exact weights, allowing tight checks.
std::vector<Outcome> Enumerate(const Triple& target, const Triple& proposal,
    int draws) {
  std::vector<Outcome> outcomes;
  const auto visit = [&](auto&& self, Reservoir reservoir, double prefix,
                         double probability, int remaining) -> void {
    if (remaining == 0) {
      Near(reservoir.weight_sum, prefix, 0, "stream sum disagrees with oracle");
      Check(reservoir.candidate_count == static_cast<std::uint32_t>(draws),
          "enumerated stream lost a candidate");
      if (prefix > 0)
        Near(reservoir.selected_target, target[reservoir.sample], 0,
            "selected target disagrees with sample");
      outcomes.push_back({reservoir, probability});
      return;
    }
    for (std::uint32_t sample = 0; sample < 3; ++sample) {
      const double weight = target[sample] / proposal[sample];
      const double next_sum = prefix + weight;
      const double replace = weight > 0 ? weight / next_sum : 0;
      const auto step = [&](bool select, double chance) {
        auto next = reservoir;
        Check(StreamReservoirCandidate(next, sample, static_cast<float>(target[sample]),
                  static_cast<float>(proposal[sample]),
                  select ? 0.0F : std::nextafter(1.0F, 0.0F)) == select,
            "enumerated replacement branch disagrees with oracle");
        self(self, next, next_sum, probability * proposal[sample] * chance,
            remaining - 1);
      };
      if (replace > 0)
        step(true, replace);
      if (replace < 1)
        step(false, 1 - replace);
    }
  };
  visit(visit, {}, 0, 1, draws);
  return outcomes;
}

using Contributions = std::array<Triple, 3>;
void ExpectIntegral(const std::vector<Outcome>& outcomes,
    const Contributions& contribution) {
  Triple actual{};
  double probability = 0;
  for (const auto& outcome : outcomes) {
    probability += outcome.probability;
    const double normalization = ReservoirNormalization(outcome.reservoir);
    if (outcome.reservoir.sample == kNoReservoirSample) {
      Check(normalization == 0, "empty outcome contributes radiance");
      continue;
    }
    for (std::size_t channel = 0; channel < 3; ++channel)
      actual[channel] += outcome.probability * normalization *
                         contribution[outcome.reservoir.sample][channel];
  }
  Near(probability, 1, 2e-13, "outcome probabilities do not sum to one");
  for (std::size_t channel = 0; channel < 3; ++channel) {
    const double expected = contribution[0][channel] + contribution[1][channel] +
                            contribution[2][channel];
    Near(actual[channel], expected, 2e-12,
        "reservoir expectation differs from independent discrete integral");
  }
}

void ExactExpectations() {
  const Triple target{1, 3, 0};
  const Triple proposal_a{0.25, 0.25, 0.5};
  const Triple proposal_b{0.5, 0.25, 0.25};
  const Contributions visible{{{2, 1, 0.5}, {1, 4, 2}, {0, 0, 0}}};
  // An unshadowed target can select an occluded/partially covered sample.
  // Visibility multiplies the final RGB integrand while leaving W unchanged.
  const Contributions shadowed{{{0.5, 0.25, 0.125}, {0, 0, 0}, {0, 0, 0}}};
  for (int draws = 1; draws <= 4; ++draws) {
    const auto outcomes = Enumerate(target, proposal_a, draws);
    ExpectIntegral(outcomes, visible);
    ExpectIntegral(outcomes, shadowed);
  }
  const auto left = Enumerate(target, proposal_a, 2);
  const auto right = Enumerate(target, proposal_b, 3);
  std::vector<Outcome> merged;
  for (const auto& a : left) {
    for (const auto& b : right) {
      const double sum = a.reservoir.weight_sum + b.reservoir.weight_sum;
      const double replace = b.reservoir.weight_sum > 0 ? b.reservoir.weight_sum / sum : 0;
      const auto step = [&](bool select, double chance) {
        auto next = a.reservoir;
        Check(MergeSameTargetReservoir(next, b.reservoir,
                  select ? 0.0F : std::nextafter(1.0F, 0.0F)) == select,
            "merged selection disagrees with oracle");
        Check(next.candidate_count == 5, "merge normalized by reservoir count instead of M");
        Near(next.weight_sum, sum, 0, "merge lost source weight sum");
        merged.push_back({next, a.probability * b.probability * chance});
      };
      if (replace > 0)
        step(true, replace);
      if (replace < 1)
        step(false, 1 - replace);
    }
  }
  ExpectIntegral(merged, visible);
  ExpectIntegral(merged, shadowed);
  ExpectIntegral(Enumerate({0, 0, 0}, proposal_a, 3), {});
  std::cout << "exact RGB integrals, visibility and unequal-stream merges PASS\n";
}

struct Random {
  std::uint32_t state = 0x6A09E667U;
  float Next() {
    state = state * 747796405U + 2891336453U;
    const std::uint32_t word = ((state >> ((state >> 28U) + 4U)) ^ state) * 277803737U;
    return static_cast<float>(((word >> 22U) ^ word) >> 8U) / 16777216.0F;
  }
};

void SelectionFrequencies() {
  Random random;
  constexpr std::uint32_t trials = 200000;
  constexpr std::array<float, 3> weights{1, 2, 7};
  std::array<std::uint32_t, 3> selected{};
  for (std::uint32_t trial = 0; trial < trials; ++trial) {
    Reservoir reservoir;
    for (std::uint32_t sample = 0; sample < 3; ++sample)
      (void)StreamReservoirCandidate(reservoir, sample, weights[sample], 1, random.Next());
    Check(reservoir.sample < 3, "positive stream did not select a sample");
    ++selected[reservoir.sample];
  }
  std::cout << "fixed-seed selection frequencies:";
  for (std::size_t i = 0; i < selected.size(); ++i) {
    const double frequency = static_cast<double>(selected[i]) / trials;
    Near(frequency, weights[i] / 10.0, 0.005, "weighted selection frequency mismatch");
    std::cout << ' ' << frequency;
  }
  std::cout << " PASS\n";
}

} // namespace

int main() try {
  Boundaries();
  InvalidInputs();
  ExactExpectations();
  SelectionFrequencies();
  std::cout << "reservoir boundaries, normalization and transactional rejection PASS\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
