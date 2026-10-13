// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Lotus::vulkan_internal {

inline constexpr std::uint32_t kNoReservoirSample = 0xFFFFFFFFU;

// CPU reference for the reservoir arithmetic. A sample is an opaque key in
// a caller-owned candidate domain, not a persistent scene/light-table slot.
// No visibility, temporal validity or renderer state belongs in this record.
struct Reservoir {
  std::uint32_t sample = kNoReservoirSample;
  float weight_sum = 0;
  float selected_target = 0;
  std::uint32_t candidate_count = 0;
};
static_assert(std::is_standard_layout_v<Reservoir>);
static_assert(std::is_trivially_copyable_v<Reservoir>);
static_assert(sizeof(Reservoir) == 16);
static_assert(offsetof(Reservoir, sample) == 0);
static_assert(offsetof(Reservoir, weight_sum) == 4);
static_assert(offsetof(Reservoir, selected_target) == 8);
static_assert(offsetof(Reservoir, candidate_count) == 12);

// Stream one draw with weight target / proposal_pdf. Both densities must use
// the same measure, and target must cover every nonzero contribution. Zero
// targets count toward M but cannot replace the selected sample. proposal_pdf
// is finite and positive; target is finite and nonnegative; random is [0, 1).
// Returns whether the candidate was selected. Invalid input/state throws
// invalid_argument; unrepresentable positive weights/sums or count overflow
// throw overflow_error. Failure leaves the destination unchanged.
bool StreamReservoirCandidate(Reservoir& reservoir, std::uint32_t sample,
    float target, float proposal_pdf, float random);

// Combine streams evaluated with EXACTLY THE SAME target function and sample
// domain. Carries the source's raw weight sum and its full M, including zeros.
// This is not cross-surface temporal/spatial reuse: that needs reevaluation
// and support-aware normalization. Shares StreamReservoirCandidate's failure
// contract. Source and destination may alias.
bool MergeSameTargetReservoir(Reservoir& destination,
    const Reservoir& source, float random);

// W = weight_sum / (M * selected_target), evaluated in double precision so
// the denominator cannot overflow in float. The estimated contribution is
// W * f(sample), with visibility applied to f, not to W. Empty/all-zero
// streams return zero. Invalid records throw invalid_argument.
[[nodiscard]] double ReservoirNormalization(const Reservoir& reservoir);

} // namespace Lotus::vulkan_internal
