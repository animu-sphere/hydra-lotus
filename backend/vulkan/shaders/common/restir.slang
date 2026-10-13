// SPDX-License-Identifier: Apache-2.0
module restir;

import common.scene;
import common.spectrum;
import bsdf.bsdf;
import sampling.sampling;
import sampling.reservoir;

// Mirrors RestirPixel. The first 16 bytes mirror the CPU Reservoir.
public struct RestirPixel {
  public Reservoir reservoir;
  public float2 uv;
  public float proposal;
  public uint flags;
  public float4 contribution;
};
[[vk::binding(16, 0)]] public RWStructuredBuffer<RestirPixel> initial_reservoirs;

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
  return SpectrumFromRgb(initial_reservoirs[pixel].contribution.xyz);
}
