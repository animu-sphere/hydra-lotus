// SPDX-License-Identifier: Apache-2.0
module restir;

import common.scene;
import common.spectrum;
import bsdf.bsdf;
import sampling.sampling;
import sampling.reservoir;
import common.temporal;

// Mirrors RestirPixel. The first 16 bytes mirror the CPU Reservoir.
public struct RestirPixel {
  public Reservoir reservoir;
  public float2 uv;
  public float proposal;
  public uint flags;
  public float4 contribution;
};
[[vk::binding(16, 0)]] public RWStructuredBuffer<RestirPixel> initial_reservoirs;

// Mirrors RestirReusePixel: the resolved reuse record.
struct RestirReuse {
  RestirPixel pixel;
  float normalization;
  uint supporting_candidates;
  uint previous_index;
  uint age;
};
[[vk::binding(17, 0)]] RWStructuredBuffer<RestirReuse> temporal_reservoirs;
[[vk::binding(18, 0)]] StructuredBuffer<RestirReuse> previous_reservoirs;
[[vk::binding(19, 0)]] RWStructuredBuffer<RestirReuse> spatial_reservoirs;
[[vk::binding(20, 0)]] RWStructuredBuffer<uint4> spatial_sources;

// 128-byte exact jittered source surface: its target function for support
// evaluation. Written once per frame by the temporal pass; the spatial pass
// reads it and its output shares it, so it is stored apart from the records.
struct RestirSurface {
  float4 origin;
  float4 normal; // w: dielectric_f90
  // w: the transmitted share, negated for a thin wall.
  float4 shading_normal;
  // w: the interface's relative index, negated when it is smooth.
  float4 outgoing;
  float4 base_alpha;
  float4 dielectric_metallic;
  float4 fresnel;
  uint64_t instance_id;
  uint64_t material_id;
};
[[vk::binding(22, 0)]] RWStructuredBuffer<RestirSurface> reservoir_surfaces;
[[vk::binding(23, 0)]] StructuredBuffer<RestirSurface> previous_surfaces;

// f in the light's unit-square parameter measure. Reconstruction embeds the
// power selection p; its Jacobian is p / sample.pdf (area or solid angle).
// Candidate proposal q is separate and appears only in streaming weights.
// Max RGB covers every nonzero channel. Visibility is applied after selection.
Spectrum CandidateContribution(SurfaceHit surface, ShadingFrame basis,
    float3 wo, LightSample sample, float selection) {
  if (!(sample.pdf > 0.0) || dot(sample.direction, surface.normal) <= 0.0)
    return SpectrumConstant(0.0);
  return BsdfEvalCos(surface.bsdf, wo, basis.ToLocal(sample.direction)) *
      sample.radiance * (selection / sample.pdf);
}

public void WriteInitialReservoir(uint2 pixel, RayDesc ray, inout uint camera_rng,
    bool active) {
  RestirPixel result;
  result.reservoir = EmptyReservoir();
  result.uv = float2(0.0);
  result.proposal = 0.0;
  result.flags = 0;
  result.contribution = float4(0.0);
  const uint slot = pixel.y * path_constants.width + pixel.x;
  // Preserve failures across all submissions of a progressive batch. The
  // final readback must also reject an overflow from an earlier sample.
  if (path_constants.accumulated != 0)
    result.flags = initial_reservoirs[slot].flags & 2;
  Hit hit;
  if (active && Trace(ray, camera_rng, hit)) {
    result.flags |= 1;
    const SurfaceHit surface = ReconstructHit(hit, ray.Direction);
    const ShadingFrame basis = MakeShadingFrame(surface.shading_normal);
    const float3 wo = basis.ToLocal(-ray.Direction);
    const float3 origin = OffsetRayOrigin(surface.position, surface.normal);
    // Separate candidate RNG stream: primary coverage and BSDF continuation
    // keep exactly the same draws under either integrator.
    uint rng = SeedRandom(slot, path_constants.sample_index ^ 0xD1B54A35u);
    if (path_constants.max_bounces != 0 && path_constants.light_count != 0) {
      for (uint i = 0; i < path_constants.restir_candidates; ++i) {
        float2 choice;
        choice.x = NextRandom(rng);
        choice.y = NextRandom(rng);
        float2 uv;
        uv.x = NextRandom(rng);
        uv.y = NextRandom(rng);
        const float random = NextRandom(rng);
        const uint light = ChooseReservoirLight(choice);
        const float proposal = ReservoirProposalPdf(light);
        const LightSample sample = ReconstructReservoirLight(light, uv, origin);
        // Reconstruction PDFs include the fixed power-table selection. Cancel
        // that factor for the target; q belongs only in the streaming weight.
        const Spectrum f = CandidateContribution(surface, basis, wo, sample,
            path_constants.lights[light].pdf);
        bool selected;
        if (!isfinite(sample.pdf) || !IsFinite(f) || !StreamReservoirCandidate(result.reservoir,
                light, MaxComponent(f), proposal, random, selected)) {
          result.flags |= 2;
          break;
        }
        if (selected) {
          result.uv = uv;
          result.proposal = proposal;
        }
      }
    }
    if ((result.flags & 2) == 0 && result.reservoir.weight_sum > 0.0) {
      const LightSample sample = ReconstructReservoirLight(
          result.reservoir.sample, result.uv, origin);
      RayDesc shadow;
      shadow.Origin = origin;
      shadow.Direction = sample.direction;
      shadow.TMin = 0.0;
      shadow.TMax = sample.distance;
      const float visibility = ShadowTransmittance(shadow);
      const float normalization = ReservoirNormalization(result.reservoir);
      const Spectrum estimate = CandidateContribution(surface, basis, wo,
          sample, path_constants.lights[result.reservoir.sample].pdf) * (visibility * normalization);
      if (!isfinite(normalization) || !IsFinite(estimate))
        result.flags |= 2;
      else
        result.contribution = float4(SpectrumToRgb(estimate), visibility);
    }
  }
  initial_reservoirs[slot] = result;
}

public Spectrum InitialDirectLight(uint pixel, SurfaceHit surface, ShadingFrame basis, float3 wo) {
  RestirPixel selected = initial_reservoirs[pixel];
  if (path_constants.restir_spatial != 0)
    selected = spatial_reservoirs[pixel].pixel;
  else if (path_constants.restir_temporal != 0)
    selected = temporal_reservoirs[pixel].pixel;
  // Failed streams are diagnosed by readback; never dereference their key.
  if ((selected.flags & 2) != 0 || !(selected.reservoir.weight_sum > 0.0) ||
      selected.reservoir.sample >= path_constants.light_count)
    return SpectrumConstant(0.0);
  const float3 origin = OffsetRayOrigin(surface.position, surface.normal);
  const LightSample sample = ReconstructReservoirLight(selected.reservoir.sample, selected.uv, origin);
  // Split the integrand, not the selected reservoir's unknown distribution.
  // The BSDF continuation uses the complementary power-table/BSDF weight.
  // Targets, support correction and diagnostic RGB remain unpartitioned.
  const float weight = sample.delta ? 1.0 : PowerHeuristic(
      ReservoirPartitionPdf(selected.reservoir.sample, sample, origin),
      BsdfPdf(surface.bsdf, wo, basis.ToLocal(sample.direction)));
  return SpectrumFromRgb(selected.contribution.xyz) * weight;
}

Spectrum HistoryContribution(RestirSurface source, uint light, LightSample sample) {
  SurfaceBsdf bsdf;
  bsdf.base_color = SpectrumFromRgb(source.base_alpha.xyz);
  bsdf.alpha = source.base_alpha.w;
  bsdf.metallic = source.dielectric_metallic.w;
  bsdf.dielectric_f0 = SpectrumFromRgb(source.dielectric_metallic.xyz);
  bsdf.dielectric_f90 = source.normal.w;
  bsdf.f0 = SpectrumFromRgb(source.fresnel.xyz);
  bsdf.f90 = source.fresnel.w;
  // Targets are reflection-side, which the transmission colour never tints.
  bsdf.transmission = abs(source.shading_normal.w);
  bsdf.transmission_color = SpectrumConstant(1.0);
  bsdf.eta = abs(source.outgoing.w);
  bsdf.thin_walled = source.shading_normal.w < 0.0;
  bsdf.smooth = source.outgoing.w < 0.0;
  if (!(sample.pdf > 0.0) || dot(sample.direction, source.normal.xyz) <= 0.0)
    return SpectrumConstant(0.0);
  const ShadingFrame basis = MakeShadingFrame(source.shading_normal.xyz);
  return BsdfEvalCos(bsdf, source.outgoing.xyz, basis.ToLocal(sample.direction)) *
      sample.radiance * (path_constants.lights[light].pdf / sample.pdf);
}

Spectrum HistoryContribution(RestirSurface source, uint light, float2 uv) {
  return HistoryContribution(source, light,
      ReconstructReservoirLight(light, uv, source.origin.xyz));
}

bool SameSample(RestirPixel a, RestirPixel b) {
  return a.reservoir.sample == b.reservoir.sample && all(a.uv == b.uv);
}

// The source stores its unshadowed target at its own exact jittered surface.
// It is valid for this selection only when the full light-slot/UV key matches.
float HistoryTarget(RestirReuse source, RestirSurface surface, RestirPixel selected,
    out bool valid) {
  if (SameSample(source.pixel, selected)) {
    valid = isfinite(source.pixel.reservoir.selected_target);
    return source.pixel.reservoir.selected_target;
  }
  const Spectrum contribution = HistoryContribution(surface, selected.reservoir.sample, selected.uv);
  valid = IsFinite(contribution);
  return MaxComponent(contribution);
}

// The input was resolved at this exact receiver earlier in the SAME frame.
// Light identity alone is insufficient for area/environment samples: their
// unit-square variates must match too. Never pass a neighbour or old history.
float ReceiverVisibility(RestirPixel selected, RestirPixel current, float3 origin,
    LightSample sample) {
  if (SameSample(selected, current))
    return current.contribution.w;
  RayDesc shadow;
  shadow.Origin = origin;
  shadow.Direction = sample.direction;
  shadow.TMin = 0.0;
  shadow.TMax = sample.distance;
  return ShadowTransmittance(shadow);
}

public void WriteTemporalReservoir(uint2 pixel, RayDesc ray, inout uint camera_rng,
    bool active) {
  const uint slot = pixel.y * path_constants.width + pixel.x;
  RestirReuse result = {};
  RestirSurface receiver = {};
  result.pixel = initial_reservoirs[slot];
  result.previous_index = 0xffffffffu;
  result.normalization = ReservoirNormalization(result.pixel.reservoir);
  result.supporting_candidates = result.pixel.reservoir.candidate_count;
  Hit hit;
  if (active && Trace(ray, camera_rng, hit)) {
    const SurfaceHit surface = ReconstructHit(hit, ray.Direction);
    const ShadingFrame basis = MakeShadingFrame(surface.shading_normal);
    const float3 wo = basis.ToLocal(-ray.Direction);
    const float3 origin = OffsetRayOrigin(surface.position, surface.normal);
    receiver.origin = float4(origin, CurrentViewDepth(surface.position));
    receiver.normal = float4(surface.normal, surface.bsdf.dielectric_f90);
    receiver.shading_normal = float4(surface.shading_normal,
        surface.bsdf.thin_walled ? -surface.bsdf.transmission : surface.bsdf.transmission);
    receiver.outgoing = float4(wo, surface.bsdf.smooth ? -surface.bsdf.eta : surface.bsdf.eta);

    receiver.base_alpha = float4(SpectrumToRgb(surface.bsdf.base_color), surface.bsdf.alpha);
    receiver.dielectric_metallic = float4(SpectrumToRgb(surface.bsdf.dielectric_f0), surface.bsdf.metallic);
    receiver.fresnel = float4(SpectrumToRgb(surface.bsdf.f0), surface.bsdf.f90);
    TemporalInstanceIdentity(hit.instance, receiver.instance_id, receiver.material_id);
    uint previous_index;
    if (path_constants.restir_history != 0 && (result.pixel.flags & 2) == 0 &&
        TemporalSampleHistory(slot, receiver.instance_id, receiver.material_id, previous_index)) {
      const RestirReuse source = previous_reservoirs[previous_index];
      // A jittered source can hit a different surface than its centre AOV.
      // Require its own identity too. Failed records are never published.
      // Its shading data is read only if support evaluation needs it.
      if ((source.pixel.flags & 1) != 0 && (source.pixel.flags & 2) == 0 &&
          previous_surfaces[previous_index].instance_id == receiver.instance_id &&
          previous_surfaces[previous_index].material_id == receiver.material_id) {
        const uint count = min(source.pixel.reservoir.candidate_count,
            path_constants.restir_max_history_candidates);
        Spectrum received = SpectrumConstant(0.0);
        if (source.pixel.reservoir.weight_sum > 0.0) {
          if (source.pixel.reservoir.sample >= path_constants.light_count)
            result.pixel.flags |= 2;
          else
            received = HistoryContribution(receiver, source.pixel.reservoir.sample, source.pixel.uv);
        }
        uint rng = SeedRandom(slot, path_constants.sample_index ^ 0x94D049BBu);
        bool selected;
        if (!IsFinite(received) || (result.pixel.flags & 2) != 0 ||
            !MergeReevaluatedReservoir(result.pixel.reservoir, source.pixel.reservoir,
                source.normalization, MaxComponent(received),
                path_constants.restir_max_history_candidates, NextRandom(rng), selected))
          result.pixel.flags |= 2;
        else {
          result.previous_index = previous_index;
          if (selected) {
            result.pixel.uv = source.pixel.uv;
            result.pixel.proposal = source.pixel.proposal;
            result.age = min(source.age, 65535u) + 1u;
          }
          // Resolve the FINAL selection's target at both participating surfaces.
          // Even an all-zero observed source contributes its support count.
          if (result.pixel.reservoir.weight_sum > 0.0) {
            bool valid_support;
            const float support = HistoryTarget(source, previous_surfaces[previous_index],
                result.pixel, valid_support);
            if (!valid_support)
              result.pixel.flags |= 2;
            result.supporting_candidates = initial_reservoirs[slot].reservoir.candidate_count +
                (support > 0.0 ? count : 0u);
          } else
            result.supporting_candidates = 0;
          result.normalization = ReservoirReuseNormalization(
              result.pixel.reservoir, result.supporting_candidates);
        }
      }
    }
    if ((result.pixel.flags & 2) == 0 && result.pixel.reservoir.weight_sum > 0.0) {
      const LightSample sample = ReconstructReservoirLight(
          result.pixel.reservoir.sample, result.pixel.uv, origin);
      const float visibility = ReceiverVisibility(result.pixel, initial_reservoirs[slot], origin, sample);
      const Spectrum estimate = HistoryContribution(receiver,
          result.pixel.reservoir.sample, sample) * (visibility * result.normalization);
      if (!isfinite(result.normalization) || result.normalization <= 0.0 || !IsFinite(estimate))
        result.pixel.flags |= 2;
      else
        result.pixel.contribution = float4(SpectrumToRgb(estimate), visibility);
    }
  }
  temporal_reservoirs[slot] = result;
  reservoir_surfaces[slot] = receiver;
}

// Geometry-only acceptance is independent of the random light selection.
// Do not gate on selected visibility or target: even all-zero observed
// streams can support the final selection and belong in its denominator.
bool SpatialCompatible(RestirSurface receiver, RestirReuse record, RestirSurface source) {
  return (record.pixel.flags & 3u) == 1u &&
      receiver.origin.w > 0.0 && source.origin.w > 0.0 &&
      receiver.material_id == source.material_id &&
      abs(receiver.origin.w - source.origin.w) <=
          0.01 + 0.02 * max(receiver.origin.w, source.origin.w) &&
      dot(receiver.normal.xyz, source.normal.xyz) >= 0.9 &&
      dot(receiver.shading_normal.xyz, source.shading_normal.xyz) >= 0.9;
}

public void WriteSpatialReservoir(uint2 pixel) {
  const uint slot = pixel.y * path_constants.width + pixel.x;
  const RestirReuse centre = temporal_reservoirs[slot];
  const RestirSurface receiver = reservoir_surfaces[slot];
  RestirReuse result = centre;
  uint4 sources = uint4(0xffffffffu);
  const int radius = int(path_constants.restir_spatial_radius);
  const int2 offsets[4] = {int2(-radius, 0), int2(radius, 0),
      int2(0, -radius), int2(0, radius)};
  if ((centre.pixel.flags & 3u) == 1u) {
    uint accepted = 0;
    for (uint i = 0; i < 4; ++i) {
      const int2 neighbour = int2(pixel) + offsets[i];
      if (any(neighbour < int2(0)) ||
          any(neighbour >= int2(path_constants.width,
              path_constants.path_capacity / path_constants.width)))
        continue;
      const uint index = uint(neighbour.y) * path_constants.width + uint(neighbour.x);
      if (SpatialCompatible(receiver, temporal_reservoirs[index], reservoir_surfaces[index])) {
        sources[i] = index;
        ++accepted;
      }
    }
    if (accepted != 0) {
      // Re-stream the centre's RESOLVED estimator; its raw sum can contain
      // a different support correction from the preceding temporal pass.
      result.pixel.reservoir = EmptyReservoir();
      bool selected;
      if (!MergeReevaluatedReservoir(result.pixel.reservoir, centre.pixel.reservoir,
              centre.normalization, centre.pixel.reservoir.selected_target,
              0xffffffffu, 0.0, selected))
        result.pixel.flags |= 2u;
      uint rng = SeedRandom(slot, path_constants.sample_index ^ 0x369DEA0Fu);
      for (uint i = 0; i < 4; ++i) {
        if (sources[i] == 0xffffffffu)
          continue;
        const RestirReuse source = temporal_reservoirs[sources[i]];
        Spectrum received = SpectrumConstant(0.0);
        if (source.pixel.reservoir.weight_sum > 0.0)
          received = HistoryContribution(receiver, source.pixel.reservoir.sample, source.pixel.uv);
        if (!IsFinite(received) || !MergeReevaluatedReservoir(result.pixel.reservoir,
                source.pixel.reservoir, source.normalization, MaxComponent(received),
                path_constants.restir_max_history_candidates, NextRandom(rng), selected)) {
          result.pixel.flags |= 2u;
          break;
        }
        if (selected) {
          result.pixel.uv = source.pixel.uv;
          result.pixel.proposal = source.pixel.proposal;
          result.age = source.age; // spatial reuse does not advance frame age
        }
      }
      result.supporting_candidates = 0;
      result.pixel.contribution = float4(0.0);
      if ((result.pixel.flags & 2u) == 0 && result.pixel.reservoir.weight_sum > 0.0) {
        result.supporting_candidates = centre.pixel.reservoir.candidate_count;
        for (uint i = 0; i < 4; ++i) {
          if (sources[i] == 0xffffffffu)
            continue;
          const RestirReuse source = temporal_reservoirs[sources[i]];
          bool valid_support;
          const float support = HistoryTarget(source, reservoir_surfaces[sources[i]],
              result.pixel, valid_support);
          if (!valid_support)
            result.pixel.flags |= 2u;
          if (support > 0.0)
            result.supporting_candidates += min(source.pixel.reservoir.candidate_count,
                path_constants.restir_max_history_candidates);
        }
      }
      result.normalization = ReservoirReuseNormalization(
          result.pixel.reservoir, result.supporting_candidates);
      if ((result.pixel.flags & 2u) == 0 && result.pixel.reservoir.weight_sum > 0.0) {
        const LightSample sample = ReconstructReservoirLight(
            result.pixel.reservoir.sample, result.pixel.uv, receiver.origin.xyz);
        const float visibility = ReceiverVisibility(result.pixel, centre.pixel, receiver.origin.xyz, sample);
        const Spectrum estimate = HistoryContribution(receiver,
            result.pixel.reservoir.sample, sample) * (visibility * result.normalization);
        if (!isfinite(result.normalization) || result.normalization <= 0.0 || !IsFinite(estimate))
          result.pixel.flags |= 2u;
        else
          result.pixel.contribution = float4(SpectrumToRgb(estimate), visibility);
      }
    }
  }
  // Destination is never read by another pixel in this pass. The output
  // keeps the receiver's surface, which the temporal pass already stored.
  spatial_reservoirs[slot] = result;
  spatial_sources[slot] = sources;
}
