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
  selected = weight > 0.0 && random < weight / sum;
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
