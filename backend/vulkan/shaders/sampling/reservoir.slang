// SPDX-License-Identifier: Apache-2.0
// GPU float counterpart of backend/vulkan/reservoir.cpp. Invalid or
// unrepresentable updates return false without publishing any state.
module reservoir;

public static const uint kNoReservoirSample = 0xFFFFFFFFu;
public struct Reservoir {
  public uint sample;
  public float weight_sum;
  public float selected_target;
  public uint candidate_count;
};

public Reservoir EmptyReservoir() {
  Reservoir r;
  r.sample = kNoReservoirSample;
  r.weight_sum = 0.0;
  r.selected_target = 0.0;
  r.candidate_count = 0;
  return r;
}

public bool StreamReservoirCandidate(inout Reservoir r, uint sample,
    float target, float proposal, float random, out bool selected) {
  selected = false;
  const bool valid_zero = r.weight_sum == 0.0 && r.sample == kNoReservoirSample &&
      r.selected_target == 0.0;
  const bool valid_positive = r.weight_sum > 0.0 && r.sample != kNoReservoirSample &&
      r.selected_target > 0.0 && r.candidate_count != 0;
  if (!isfinite(r.weight_sum) || !isfinite(r.selected_target) ||
      !(valid_zero || valid_positive) ||
      !isfinite(target) || target < 0.0 || !isfinite(proposal) ||
      proposal <= 0.0 || !isfinite(random) || random < 0.0 || random >= 1.0 ||
      (target > 0.0 && sample == kNoReservoirSample) ||
      r.candidate_count == 0xFFFFFFFFu) {
    return false;
  }
  const float weight = target / proposal;
  const float sum = r.weight_sum + weight;
  if (!isfinite(weight) || !isfinite(sum) || (target > 0.0 && weight == 0.0)) {
    return false;
  }
  // The first positive weight is selected with probability one: a
  // reciprocal-based float quotient can fall just below 1 and leave a
  // positive sum without a sample when random is close to 1.
  selected = weight > 0.0 && (r.weight_sum == 0.0 || random < weight / sum);
  r.weight_sum = sum;
  ++r.candidate_count;
  if (selected) {
    r.sample = sample;
    r.selected_target = target;
  }
  return true;
}

public float ReservoirNormalization(Reservoir r) {
  // Divide sequentially, avoiding M * target overflowing in float.
  return r.weight_sum > 0.0
      ? (r.weight_sum / float(r.candidate_count)) / r.selected_target : 0.0;
}

public bool MergeReevaluatedReservoir(inout Reservoir destination,
    Reservoir source, float normalization, float target, uint cap,
    float random, out bool selected) {
  selected = false;
  const uint count = min(source.candidate_count, cap);
  const bool zero = source.weight_sum == 0.0 &&
      source.sample == kNoReservoirSample && source.selected_target == 0.0 &&
      normalization == 0.0 && target == 0.0;
  const bool positive = source.weight_sum > 0.0 && source.selected_target > 0.0 &&
      source.sample != kNoReservoirSample && source.candidate_count != 0 && normalization > 0.0;
  const bool valid_destination = destination.weight_sum == 0.0
      ? destination.sample == kNoReservoirSample && destination.selected_target == 0.0
      : destination.weight_sum > 0.0 && destination.selected_target > 0.0 &&
        destination.sample != kNoReservoirSample && destination.candidate_count != 0;
  if (!valid_destination || !isfinite(destination.weight_sum) ||
      !isfinite(destination.selected_target) || !isfinite(source.weight_sum) ||
      !isfinite(source.selected_target) || !(zero || positive) ||
      !isfinite(normalization) || !isfinite(target) || target < 0.0 || cap == 0 ||
      !isfinite(random) || random < 0.0 || random >= 1.0 ||
      count > 0xFFFFFFFFu - destination.candidate_count)
    return false;
  // Match the CPU oracle's receiver_target * W * capped M contract.
  const float weight = (target * normalization) * float(count);
  const float sum = destination.weight_sum + weight;
  if (!isfinite(weight) || !isfinite(sum) || (target > 0.0 && weight == 0.0))
    return false;
  // As in StreamReservoirCandidate: an empty destination takes the source.
  selected = weight > 0.0 &&
             (destination.weight_sum == 0.0 || random < weight / sum);
  destination.weight_sum = sum;
  destination.candidate_count += count;
  if (selected) {
    destination.sample = source.sample;
    destination.selected_target = target;
  }
  return true;
}

public float ReservoirReuseNormalization(Reservoir r, uint supporting_count) {
  return r.weight_sum > 0.0
      ? (r.weight_sum / float(supporting_count)) / r.selected_target : 0.0;
}
