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

// 192-byte private history record. The first 64 bytes are RestirReusePixel;
// the remaining float4s retain the exact jittered source target function.
struct RestirHistory {
  RestirPixel pixel;
  float normalization;
  uint supporting_candidates;
  uint previous_index;
  uint age;
  float4 origin;
  float4 normal; // w: dielectric_f90
  float4 shading_normal;
  float4 outgoing;
  float4 base_alpha;
  float4 dielectric_metallic;
  float4 fresnel;
  uint64_t instance_id;
  uint64_t material_id;
};
[[vk::binding(17, 0)]] RWStructuredBuffer<RestirHistory> temporal_reservoirs;
[[vk::binding(18, 0)]] StructuredBuffer<RestirHistory> previous_reservoirs;

// f in the light's unit-square parameter measure. The conditional mapping's
// Jacobian is selection / sample.pdf (area or solid angle); q is selection.
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
        const float proposal = path_constants.lights[light].pdf;
        const LightSample sample = ReconstructReservoirLight(light, uv, origin);
        const Spectrum f = CandidateContribution(surface, basis, wo, sample, proposal);
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
          sample, result.proposal) * (visibility * normalization);
      if (!isfinite(normalization) || !IsFinite(estimate))
        result.flags |= 2;
      else
        result.contribution = float4(SpectrumToRgb(estimate), visibility);
    }
  }
  initial_reservoirs[slot] = result;
}

public Spectrum InitialDirectLight(uint pixel) {
  if (path_constants.restir_temporal != 0)
    return SpectrumFromRgb(temporal_reservoirs[pixel].pixel.contribution.xyz);
  return SpectrumFromRgb(initial_reservoirs[pixel].contribution.xyz);
}

Spectrum HistoryContribution(RestirHistory source, uint light, float2 uv) {
  SurfaceBsdf bsdf;
  bsdf.base_color = SpectrumFromRgb(source.base_alpha.xyz);
  bsdf.alpha = source.base_alpha.w;
  bsdf.metallic = source.dielectric_metallic.w;
  bsdf.dielectric_f0 = SpectrumFromRgb(source.dielectric_metallic.xyz);
  bsdf.dielectric_f90 = source.normal.w;
  bsdf.f0 = SpectrumFromRgb(source.fresnel.xyz);
  bsdf.f90 = source.fresnel.w;
  const LightSample sample = ReconstructReservoirLight(light, uv, source.origin.xyz);
  if (!(sample.pdf > 0.0) || dot(sample.direction, source.normal.xyz) <= 0.0)
    return SpectrumConstant(0.0);
  const ShadingFrame basis = MakeShadingFrame(source.shading_normal.xyz);
  return BsdfEvalCos(bsdf, source.outgoing.xyz, basis.ToLocal(sample.direction)) *
      sample.radiance * (path_constants.lights[light].pdf / sample.pdf);
}

public void WriteTemporalReservoir(uint2 pixel, RayDesc ray, inout uint camera_rng,
    bool active) {
  const uint slot = pixel.y * path_constants.width + pixel.x;
  RestirHistory result = {};
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
    result.origin = float4(origin, 0.0);
    result.normal = float4(surface.normal, surface.bsdf.dielectric_f90);
    result.shading_normal = float4(surface.shading_normal, 0.0);
    result.outgoing = float4(wo, 0.0);
    result.base_alpha = float4(SpectrumToRgb(surface.bsdf.base_color), surface.bsdf.alpha);
    result.dielectric_metallic = float4(SpectrumToRgb(surface.bsdf.dielectric_f0), surface.bsdf.metallic);
    result.fresnel = float4(SpectrumToRgb(surface.bsdf.f0), surface.bsdf.f90);
    TemporalInstanceIdentity(hit.instance, result.instance_id, result.material_id);
    uint previous_index;
    if (path_constants.restir_history != 0 && (result.pixel.flags & 2) == 0 &&
        TemporalSampleHistory(slot, result.instance_id, result.material_id, previous_index)) {
      const RestirHistory source = previous_reservoirs[previous_index];
      // A jittered source can hit a different surface than its centre AOV.
      // Require its own identity too. Failed records are never published.
      if ((source.pixel.flags & 1) != 0 && (source.pixel.flags & 2) == 0 &&
          source.instance_id == result.instance_id && source.material_id == result.material_id) {
        const uint count = min(source.pixel.reservoir.candidate_count,
            path_constants.restir_max_history_candidates);
        Spectrum received = SpectrumConstant(0.0);
        if (source.pixel.reservoir.weight_sum > 0.0) {
          if (source.pixel.reservoir.sample >= path_constants.light_count)
            result.pixel.flags |= 2;
          else
            received = HistoryContribution(result, source.pixel.reservoir.sample, source.pixel.uv);
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
          // Reevaluate the FINAL selection at both participating surfaces.
          // Even an all-zero observed source contributes its support count.
          if (result.pixel.reservoir.weight_sum > 0.0) {
            const Spectrum support = HistoryContribution(source,
                result.pixel.reservoir.sample, result.pixel.uv);
            if (!IsFinite(support))
              result.pixel.flags |= 2;
            result.supporting_candidates = initial_reservoirs[slot].reservoir.candidate_count +
                (MaxComponent(support) > 0.0 ? count : 0u);
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
      RayDesc shadow;
      shadow.Origin = origin;
      shadow.Direction = sample.direction;
      shadow.TMin = 0.0;
      shadow.TMax = sample.distance;
      const float visibility = ShadowTransmittance(shadow);
      const Spectrum estimate = HistoryContribution(result,
          result.pixel.reservoir.sample, result.pixel.uv) * (visibility * result.normalization);
      if (!isfinite(result.normalization) || result.normalization <= 0.0 || !IsFinite(estimate))
        result.pixel.flags |= 2;
      else
        result.pixel.contribution = float4(SpectrumToRgb(estimate), visibility);
    }
  }
  temporal_reservoirs[slot] = result;
}
