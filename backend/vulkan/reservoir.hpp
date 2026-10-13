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

// Stream a source sample reevaluated at the receiver, with weight
// receiver_target * source_normalization * min(source.M, max_source_candidates).
// source_normalization is the source's resolved W, including any earlier
// support correction; it must NOT be reconstructed from a reused raw sum/M.
// The cap changes represented count and merge weight together, preserving W.
// Zero-weight sources/reevaluations still carry the capped count. A source
// without a sample requires zero receiver_target and zero normalization.
// Shares the streaming failure/alias contract. Positive source weights require
// finite positive normalization; the cap must be nonzero.
bool MergeReevaluatedReservoir(Reservoir& destination, const Reservoir& source,
    double source_normalization, float receiver_target,
    std::uint32_t max_source_candidates, float random);

// Resolve a differing-target merge after evaluating the FINAL selected sample
// at every participating source surface. supporting_candidate_count is the
// sum of their capped M where that source's target at the sample is positive,
// including all-zero observed streams. It is not the number of reservoirs or
// the sum of counts whose selected samples survived receiver reevaluation.
// Returns sum/(supporting M * receiver target). Positive records require
// supporting M in [1, record.M]; zero records return zero. Keep this W beside
// the raw record when reusing again. Targets/proposals must share a fixed
// candidate domain/measure, cover their respective contributions, and the
// source support union must cover the receiver integral.
[[nodiscard]] double ReservoirReuseNormalization(const Reservoir& reservoir,
    std::uint32_t supporting_candidate_count);

// W = weight_sum / (M * selected_target), evaluated in double precision so
// the denominator cannot overflow in float. The estimated contribution is
// W * f(sample), with visibility applied to f, not to W. Empty/all-zero
// streams return zero. Invalid records throw invalid_argument.
// Differing-support reuse must use ReservoirReuseNormalization instead.
[[nodiscard]] double ReservoirNormalization(const Reservoir& reservoir);

} // namespace Lotus::vulkan_internal
