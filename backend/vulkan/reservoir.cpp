// SPDX-License-Identifier: Apache-2.0
#include "reservoir.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace Lotus::vulkan_internal {
namespace {

void Validate(const Reservoir& reservoir) {
  if (!std::isfinite(reservoir.weight_sum) || reservoir.weight_sum < 0 ||
      !std::isfinite(reservoir.selected_target) || reservoir.selected_target < 0)
    throw std::invalid_argument("reservoir weights must be finite and nonnegative");
  if (reservoir.weight_sum == 0) {
    if (reservoir.sample != kNoReservoirSample || reservoir.selected_target != 0)
      throw std::invalid_argument("zero-weight reservoir must have no sample");
  } else if (reservoir.sample == kNoReservoirSample ||
             reservoir.selected_target == 0 || reservoir.candidate_count == 0) {
    throw std::invalid_argument("positive-weight reservoir needs a sample, target and count");
  }
}

void ValidateRandom(float random) {
  if (!std::isfinite(random) || random < 0 || random >= 1)
    throw std::invalid_argument("reservoir random variate must be in [0, 1)");
}

float RepresentableWeight(double weight) {
  if (!std::isfinite(weight) || weight > std::numeric_limits<float>::max())
    throw std::overflow_error("reservoir weight exceeds float storage");
  const auto result = static_cast<float>(weight);
  if (weight > 0 && result == 0)
    throw std::overflow_error("positive reservoir weight underflows float storage");
  return result;
}

bool StreamWeighted(Reservoir& destination, std::uint32_t sample,
    float target, float weight, std::uint32_t count, float random) {
  if (count > std::numeric_limits<std::uint32_t>::max() - destination.candidate_count)
    throw std::overflow_error("reservoir candidate count overflow");
  const double sum = static_cast<double>(destination.weight_sum) + weight;
  const float stored_sum = RepresentableWeight(sum);
  // A strict comparison makes a zero-weight candidate unselectable at u=0.
  const bool selected = weight > 0 && random < static_cast<double>(weight) / sum;
  Reservoir next = destination;
  next.weight_sum = stored_sum;
  next.candidate_count += count;
  if (selected) {
    next.sample = sample;
    next.selected_target = target;
  }
  destination = next;
  return selected;
}

} // namespace

bool StreamReservoirCandidate(Reservoir& reservoir, std::uint32_t sample,
    float target, float proposal_pdf, float random) {
  Validate(reservoir);
  ValidateRandom(random);
  if (!std::isfinite(target) || target < 0 ||
      !std::isfinite(proposal_pdf) || proposal_pdf <= 0 ||
      (target > 0 && sample == kNoReservoirSample))
    throw std::invalid_argument("invalid reservoir candidate target, PDF or sample");
  const float weight = RepresentableWeight(static_cast<double>(target) / proposal_pdf);
  return StreamWeighted(reservoir, sample, target, weight, 1, random);
}

bool MergeSameTargetReservoir(Reservoir& destination,
    const Reservoir& source, float random) {
  Validate(destination);
  Validate(source);
  ValidateRandom(random);
  // Copy the arguments before publishing the update, including for self-merge.
  return StreamWeighted(destination, source.sample, source.selected_target,
      source.weight_sum, source.candidate_count, random);
}

double ReservoirNormalization(const Reservoir& reservoir) {
  Validate(reservoir);
  if (reservoir.weight_sum == 0)
    return 0;
  return static_cast<double>(reservoir.weight_sum) /
         reservoir.candidate_count / reservoir.selected_target;
}

} // namespace Lotus::vulkan_internal
