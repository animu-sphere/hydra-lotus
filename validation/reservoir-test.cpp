// SPDX-License-Identifier: Apache-2.0
#include "../backend/vulkan/reservoir.hpp"

#include <algorithm>
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

void ReuseBoundaries() {
  const Reservoir source{17, 24, 3, 8};
  const double source_w = ReservoirNormalization(source); // one
  Reservoir merged;
  Check(MergeReevaluatedReservoir(merged, source, source_w, 2, 3, 0),
      "reevaluated source not selected");
  Check(merged.sample == 17 && merged.weight_sum == 6 &&
            merged.selected_target == 2 && merged.candidate_count == 3,
      "reevaluation/cap did not scale weight and count together");
  Near(ReservoirReuseNormalization(merged, 3), 1, 0, "cap changed source W");
  Near(ReservoirReuseNormalization(merged, 1), 3, 0, "support correction omitted");

  const auto retained = merged;
  Check(!MergeReevaluatedReservoir(merged, source, source_w, 0, 2, 0),
      "unsupported receiving candidate replaced sample");
  Check(merged.candidate_count == 5 && merged.weight_sum == retained.weight_sum &&
            merged.sample == retained.sample && merged.selected_target == retained.selected_target,
      "unsupported receiving candidate lost count or changed sample");
  const Reservoir zeros{kNoReservoirSample, 0, 0, 20};
  Check(!MergeReevaluatedReservoir(merged, zeros, 0, 0, 4, 0) &&
            merged.candidate_count == 9,
      "all-zero stream did not carry capped draws");
  Check(!MergeReevaluatedReservoir(merged, {}, 0, 0, 4, 0) &&
            merged.candidate_count == 9,
      "empty stream changed count");
  Near(ReservoirReuseNormalization(zeros, 0), 0, 0, "all-zero reuse is nonzero");
  Near(ReservoirReuseNormalization({}, 0), 0, 0, "empty reuse is nonzero");
  Reservoir unsupported;
  Check(!MergeReevaluatedReservoir(unsupported, source, source_w, 0, 8, 0) &&
            unsupported.candidate_count == 8 && unsupported.sample == kNoReservoirSample,
      "all-zero receiving merge selected a sample or lost represented draws");
  Near(ReservoirReuseNormalization(unsupported, 0), 0, 0,
      "all-zero receiving merge has nonzero normalization");

  // Reuse an already support-corrected record. Recomputing sum/(M*target)
  // here would discard its resolved weight of 3 and produce a third as much.
  Reservoir next;
  Check(MergeReevaluatedReservoir(next, retained,
            ReservoirReuseNormalization(retained, 1), 4, 2, 0),
      "resolved source not reusable");
  Check(next.weight_sum == 24 && next.candidate_count == 2,
      "reuse discarded the source support correction");
  Near(ReservoirReuseNormalization(next, 2), 3, 0, "resolved source W changed");

  // Correlated duplication carries count but is not new independent evidence.
  auto alias = source;
  Check(!MergeReevaluatedReservoir(alias, alias, source_w, 3, 8, 0.5F),
      "reevaluated replacement boundary must be strict");
  Check(alias.weight_sum == 48 && alias.candidate_count == 16,
      "alias merge changed inputs before reading them");
  Near(ReservoirReuseNormalization(alias, 16), 1, 0, "alias merge changed W");

  const float tiny = std::numeric_limits<float>::denorm_min();
  const Reservoir extreme{1, 1, tiny, 1};
  Reservoir scaled;
  Check(MergeReevaluatedReservoir(scaled, extreme, ReservoirNormalization(extreme), tiny, 1, 0),
      "large double W with representable receiving weight rejected");
  Check(scaled.weight_sum == 1, "source W prematurely rounded to float");
}

void InvalidReuseInputs() {
  const Reservoir source{7, 8, 2, 4};
  const Reservoir zeros{kNoReservoirSample, 0, 0, 4};
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float infinity = std::numeric_limits<float>::infinity();
  Reservoir destination = source;
  for (double w : {-1.0, 0.0, std::numeric_limits<double>::quiet_NaN(),
           std::numeric_limits<double>::infinity()})
    RejectUnchanged<std::invalid_argument>(destination, [&] {
      (void)MergeReevaluatedReservoir(destination, source, w, 2, 4, 0);
    });
  for (float target : {-1.0F, nan, infinity})
    RejectUnchanged<std::invalid_argument>(destination, [&] {
      (void)MergeReevaluatedReservoir(destination, source, 1, target, 4, 0);
    });
  for (float random : {-1.0F, 1.0F, nan, infinity})
    RejectUnchanged<std::invalid_argument>(destination, [&] {
      (void)MergeReevaluatedReservoir(destination, source, 1, 2, 4, random);
    });
  RejectUnchanged<std::invalid_argument>(destination, [&] {
    (void)MergeReevaluatedReservoir(destination, source, 1, 2, 0, 0);
  });
  RejectUnchanged<std::invalid_argument>(destination, [&] {
    (void)MergeReevaluatedReservoir(destination, zeros, 1, 0, 4, 0);
  });
  RejectUnchanged<std::invalid_argument>(destination, [&] {
    (void)MergeReevaluatedReservoir(destination, zeros, 0, 1, 4, 0);
  });
  for (const Reservoir malformed : {Reservoir{1, 0, 0, 1}, Reservoir{1, 1, 0, 1},
           Reservoir{kNoReservoirSample, 1, 1, 1}, Reservoir{1, 1, 1, 0}}) {
    RejectUnchanged<std::invalid_argument>(destination, [&] {
      (void)MergeReevaluatedReservoir(destination, malformed, 1, 2, 4, 0);
    });
    auto invalid = malformed;
    RejectUnchanged<std::invalid_argument>(invalid, [&] {
      (void)MergeReevaluatedReservoir(invalid, source, 1, 2, 4, 0);
    });
  }
  const float maximum = std::numeric_limits<float>::max();
  for (const auto [w, target] : std::array<std::array<double, 2>, 3>{{
           {1, maximum}, {std::numeric_limits<double>::max(), 1},
           {std::numeric_limits<double>::denorm_min(), 1}}})
    RejectUnchanged<std::overflow_error>(destination, [&] {
      (void)MergeReevaluatedReservoir(destination, source, w,
          static_cast<float>(target), 4, 0);
    });
  Reservoir large{1, maximum, 1, 1};
  RejectUnchanged<std::overflow_error>(large, [&] {
    (void)MergeReevaluatedReservoir(large, large, maximum, 1, 1, 0);
  });
  Reservoir full{1, 1, 1, std::numeric_limits<std::uint32_t>::max()};
  RejectUnchanged<std::overflow_error>(full, [&] {
    (void)MergeReevaluatedReservoir(full, zeros, 0, 0, 1, 0);
  });
  for (std::uint32_t count : {0U, 5U}) {
    bool rejected = false;
    try {
      (void)ReservoirReuseNormalization(source, count);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    Check(rejected, "invalid support count accepted");
  }
  bool rejected = false;
  try {
    (void)ReservoirReuseNormalization(zeros, 5);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  Check(rejected, "all-zero record accepted support beyond represented count");
}

// Exhaust the draw/selection outcomes of two initial source streams, then
// exhaust both branches of each receiving merge. The oracle computes branch
// probabilities from the original draws' sums and source targets, not the
// helper's merged weights or normalization. Zero observed streams remain
// participants when evaluating support at the final selected sample.
void ReuseExpectations() {
  const Triple proposal_a{0.25, 0.25, 0.5};
  const Triple proposal_b{0.5, 0.25, 0.25};
  std::size_t cases = 0;
  double naive_bias = 0;
  for (const Triple receiving : {Triple{2, 1, 4}, Triple{2, 0, 4}}) {
    Contributions visible{{{2, 1, 0.5}, {1, 4, 2}, {3, 0.5, 1}}};
    Contributions shadowed{{{0, 0, 0}, {0.25, 1, 0.5}, {3, 0.5, 1}}};
    if (receiving[1] == 0) {
      visible[1] = {};
      shadowed[1] = {};
    }
    for (const auto& targets : std::array<std::array<Triple, 2>, 4>{{
             {{{1, 3, 2}, {4, 2, 1}}}, // full, different targets
             {{{1, 0, 2}, {0, 2, 1}}}, // complementary support
             {{{1, 3, 2}, {0, 0, 0}}}, // whole zero-support source
             {{{1, 3, 2}, {0, 2, 0}}}}}) { // one source can observe all zeros
      const std::array streams{Enumerate(targets[0], proposal_a, 2),
          Enumerate(targets[1], proposal_b, 3)};
      for (std::uint32_t cap : {1U, 2U, 3U, 8U}) {
        for (bool reverse : {false, true}) {
          Triple actual{}, shadow_actual{};
          double probability = 0, naive_red = 0;
          const std::array<std::uint32_t, 2> count{std::min(2U, cap), std::min(3U, cap)};
          for (const auto& a : streams[0]) {
            for (const auto& b : streams[1]) {
              const std::array sources{a.reservoir, b.reservoir};
              const auto visit = [&](auto&& self, Reservoir output, double prefix,
                                     double chance, int step) -> void {
                if (step == 2) {
                  probability += chance;
                  Check(output.candidate_count == count[0] + count[1],
                      "reuse lost capped source counts");
                  if (output.sample == kNoReservoirSample) {
                    Near(ReservoirReuseNormalization(output, 0), 0, 0,
                        "all-zero receiving merge has nonzero W");
                    return;
                  }
                  const auto sample = output.sample;
                  std::uint32_t support = 0;
                  for (std::size_t i = 0; i < 2; ++i)
                    if (targets[i][sample] > 0)
                      support += count[i];
                  const double expected_w = prefix / support / receiving[sample];
                  const double w = ReservoirReuseNormalization(output, support);
                  Near(w, expected_w, 2e-7 * std::max(1.0, expected_w),
                      "support normalization disagrees with oracle");
                  for (std::size_t channel = 0; channel < 3; ++channel) {
                    actual[channel] += chance * w * visible[sample][channel];
                    shadow_actual[channel] += chance * w * shadowed[sample][channel];
                  }
                  naive_red += chance * ReservoirNormalization(output) * visible[sample][0];
                  return;
                }
                const std::size_t i = reverse ? 1 - step : step;
                const Reservoir& source = sources[i];
                const bool positive = source.sample != kNoReservoirSample;
                const double w = positive ? double(source.weight_sum) /
                    source.candidate_count / targets[i][source.sample] : 0;
                const float target = positive ? static_cast<float>(receiving[source.sample]) : 0;
                const double weight = target * w * count[i];
                const double sum = prefix + weight;
                const double replace = weight > 0 ? weight / sum : 0;
                const auto branch = [&](bool selected, double branch_chance) {
                  auto next = output;
                  Check(MergeReevaluatedReservoir(next, source, w, target, cap,
                            selected ? 0.0F : std::nextafter(1.0F, 0.0F)) == selected,
                      "reevaluated selection disagrees with independent oracle");
                  self(self, next, sum, chance * branch_chance, step + 1);
                };
                if (replace > 0)
                  branch(true, replace);
                if (replace < 1)
                  branch(false, 1 - replace);
              };
              visit(visit, {}, 0, a.probability * b.probability, 0);
            }
          }
          Near(probability, 1, 2e-12, "reuse branch probabilities do not sum to one");
          for (std::size_t channel = 0; channel < 3; ++channel) {
            double integral = 0, shadow_integral = 0;
            for (std::size_t sample = 0; sample < 3; ++sample) {
              integral += visible[sample][channel];
              shadow_integral += shadowed[sample][channel];
            }
            Near(actual[channel], integral, 2e-6, "differing-target RGB integral is biased");
            Near(shadow_actual[channel], shadow_integral, 2e-6, "reuse visibility integral is biased");
          }
          const double red_integral = visible[0][0] + visible[1][0] + visible[2][0];
          naive_bias = std::max(naive_bias, std::abs(naive_red - red_integral));
          ++cases;
        }
      }
    }
  }
  Check(naive_bias > 1, "fixtures cannot detect total-M normalization bias");
  std::cout << "support-aware RGB/visibility integrals: " << cases
            << " cases PASS; naive total-M red bias up to " << naive_bias << '\n';
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
  ReuseBoundaries();
  InvalidReuseInputs();
  ReuseExpectations();
  SelectionFrequencies();
  std::cout << "reservoir boundaries, normalization and transactional rejection PASS\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
