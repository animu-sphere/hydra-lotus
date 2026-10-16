// SPDX-License-Identifier: Apache-2.0
// The reference light transport (design policy section 9), as the steps of
// one camera path: start it, scatter it at each hit, and end it on a miss.
// BSDF sampling, emissive surfaces, analytic lights, a constant environment,
// an environment image and Russian roulette, with emissive surfaces,
// analytic lights and the environment image found either by BSDF sampling
// alone (the brute-force reference estimator) or also by next event
// estimation, combined by multiple importance sampling (Renderer Phase 3). The reference integrator runs the steps in one loop
// per pixel; the wavefront integrator runs each as a kernel over a queue of
// paths. Both consume a path's random numbers in the same order, so a pixel
// and sample index select the same path in either.
module transport;

import bsdf.bsdf;
import sampling.sampling;
import common.spectrum;
import common.scene;
import common.restir;

// Bounces before Russian roulette may end a path.
static const uint kRouletteBounces = 3;
static const float kMaxSurvival = 0.95;

// Design policy section 8. Light-carrying values are Spectrum (DES-Q3).
public struct PathState {
  public RayDesc ray;
  public Spectrum throughput;
  public Spectrum radiance;
  public uint pixelIndex;
  // Scattering events so far.
  public uint depth;
  public uint rngState;
  // Solid-angle density of the last sampled direction, which weighs the
  // emission it finds against next event estimation's.
  public float lastPdf;
  public uint flags;
};

// A pixel's camera path for the frame's sample index: its camera ray goes
// through a uniformly distributed point of the pixel (the 1-pixel box
// filter).
public PathState StartPath(uint2 pixel) {
  PathState path;
  path.throughput = SpectrumConstant(1.0);
  path.radiance = SpectrumConstant(0.0);
  path.pixelIndex = pixel.y * path_constants.width + pixel.x;
  path.depth = 0;
  path.rngState = SeedRandom(path.pixelIndex, path_constants.sample_index);
  path.lastPdf = 0.0;
  path.flags = 0;
  float2 u;
  u.x = NextRandom(path.rngState);
  u.y = NextRandom(path.rngState);
  const float2 film = float2(pixel) + 0.5 + SampleBoxFilter(u);
  path.ray = CameraRay(film * path_constants.ndc_from_pixel.xy +
                       path_constants.ndc_from_pixel.zw);
  return path;
}

public bool SampleLights() {
  return path_constants.light_sampling != kBsdfSampling;
}

public bool ReservoirPrimary(uint depth) {
  return path_constants.light_sampling == kRestirInitialSampling && depth == 0;
}

// The light a sample carries to a surface through a shadow ray, weighed
// against BSDF sampling's density for its direction by the power heuristic
// unless only next event estimation finds it.
Spectrum ShadowedLight(SurfaceHit surface, ShadingFrame basis, float3 wo,
    float3 origin, LightSample sample) {
  // As for a sampled direction, light below the geometric surface does not
  // reach it.
  if (!(sample.pdf > 0.0) || MaxComponent(sample.radiance) <= 0.0 ||
      dot(sample.direction, surface.normal) <= 0.0) {
    return SpectrumConstant(0.0);
  }
  const float3 wi = basis.ToLocal(sample.direction);
  const Spectrum f = BsdfEvalCos(surface.bsdf, wo, wi);
  if (MaxComponent(f) <= 0.0) {
    return SpectrumConstant(0.0);
  }
  RayDesc shadow;
  shadow.Origin = origin;
  shadow.TMin = 0.0;
  shadow.Direction = sample.direction;
  shadow.TMax = sample.distance;
  const float transmittance = ShadowTransmittance(shadow);
  if (transmittance <= 0.0) {
    return SpectrumConstant(0.0);
  }
  const float weight = sample.delta
      ? 1.0
      : PowerHeuristic(sample.pdf, BsdfPdf(surface.bsdf, wo, wi));
  const Spectrum contribution =
      f * sample.radiance * (transmittance * weight / sample.pdf);
  return IsFinite(contribution) ? contribution : SpectrumConstant(0.0);
}

// Next event estimation at a surface: one emissive triangle, analytic light
// or environment image direction, chosen from the light table, its light
// carried through a shadow ray. Draws four random numbers when the table has
// entries. The constant environment is left to BSDF sampling, whose
// cosine-weighted directions sample it better than uniform directions
// would.
Spectrum DirectLight(SurfaceHit surface, ShadingFrame basis, float3 wo,
    inout uint rng) {
  if (path_constants.light_count == 0) {
    return SpectrumConstant(0.0);
  }
  float4 u;
  u.x = NextRandom(rng);
  u.y = NextRandom(rng);
  u.z = NextRandom(rng);
  u.w = NextRandom(rng);
  const float3 origin = OffsetRayOrigin(surface.position, surface.normal);
  return ShadowedLight(surface, basis, wo, origin, SampleLight(u, origin));
}

// Under BSDF sampling, the parallel lights no sampled direction finds: each
// through its own shadow ray, drawing no random numbers.
Spectrum ParallelLights(SurfaceHit surface, ShadingFrame basis, float3 wo) {
  const float3 origin = OffsetRayOrigin(surface.position, surface.normal);
  Spectrum sum = SpectrumConstant(0.0);
  for (uint index = 0; index < path_constants.analytic_count; ++index) {
    sum = sum + ShadowedLight(surface, basis, wo, origin, ParallelLight(index));
  }
  return sum;
}

// The weight of emission a sampled direction found at a surface, against
// next event estimation's density for the same point.
float EmissionWeight(PathState path, Hit hit, SurfaceHit surface) {
  const float area_pdf = EmitterPdf(hit, surface.area);
  if (!(area_pdf > 0.0)) {
    return 1.0;
  }
  const float3 toward = surface.position - path.ray.Origin;
  const float cosine = abs(dot(surface.normal, path.ray.Direction));
  return PowerHeuristic(path.lastPdf, area_pdf * dot(toward, toward) / cosine);
}

// One scattering event at the path's hit: adds the surface's emission and,
// with light sampling, the direct light at it, then either ends the path
// (false) or samples the BSDF and leaves the continuation ray in path.ray
// (true).
public bool ScatterPath(inout PathState path, Hit hit) {
  const SurfaceHit surface = ReconstructHit(hit, path.ray.Direction);
  Spectrum emission = surface.emission;
  if (SampleLights() && path.depth != 0 && MaxComponent(emission) > 0.0) {
    emission = emission * EmissionWeight(path, hit, surface);
  }
  path.radiance = path.radiance + path.throughput * emission;
  // The analytic lights the continuation ray crossed before the hit. Camera
  // rays do not see them.
  if (path.depth != 0 && path_constants.analytic_count != 0) {
    path.radiance = path.radiance + path.throughput *
        CrossedLights(path.ray.Origin, path.ray.Direction,
            dot(surface.position - path.ray.Origin, path.ray.Direction),
            false, SampleLights(), path.lastPdf);
  }
  if (path.depth >= path_constants.max_bounces) {
    return false;
  }
  if (path.depth >= kRouletteBounces) {
    const float survival = min(MaxComponent(path.throughput), kMaxSurvival);
    if (NextRandom(path.rngState) >= survival) {
      return false;
    }
    path.throughput = path.throughput / survival;
  }
  const ShadingFrame basis = MakeShadingFrame(surface.shading_normal);
  const float3 wo = basis.ToLocal(-path.ray.Direction);
  if (ReservoirPrimary(path.depth)) {
    path.radiance = path.radiance + path.throughput * InitialDirectLight(path.pixelIndex, surface, basis, wo);
  } else if (SampleLights()) {
    path.radiance = path.radiance +
        path.throughput * DirectLight(surface, basis, wo, path.rngState);
  } else if (path_constants.analytic_count != 0) {
    path.radiance = path.radiance +
        path.throughput * ParallelLights(surface, basis, wo);
  }
  float3 u;
  u.x = NextRandom(path.rngState);
  u.y = NextRandom(path.rngState);
  u.z = NextRandom(path.rngState);
  BsdfSample sample;
  if (!SampleBsdf(surface.bsdf, wo, u, sample)) {
    return false;
  }
  const float3 wi = normalize(basis.ToWorld(sample.wi));
  // Above the shading normal but below the geometric surface, a direction
  // would leave through the surface, which only reflects: the path ends.
  if (dot(wi, surface.normal) <= 0.0) {
    return false;
  }
  path.throughput = path.throughput * sample.weight;
  path.lastPdf = sample.pdf;
  path.ray.Origin = OffsetRayOrigin(surface.position, surface.normal);
  path.ray.Direction = wi;
  path.ray.TMin = 0.0;
  path.ray.TMax = kFloatMax;
  ++path.depth;
  return true;
}

// A continuation ray that leaves the scene ends its path with the
// environment's radiance, the image's weighed against light sampling, and
// that of the analytic lights it crossed.
public void MissPath(inout PathState path) {
  Spectrum environment = SpectrumFromRgb(path_constants.environment.xyz);
  if (path_constants.environment_map != 0) {
    environment = environment + EnvironmentRadiance(
        path.ray.Direction, SampleLights(), path.lastPdf);
  }
  path.radiance = path.radiance + path.throughput * environment;
  if (path_constants.analytic_count != 0) {
    path.radiance = path.radiance + path.throughput *
        CrossedLights(path.ray.Origin, path.ray.Direction, kFloatMax, true,
            SampleLights(), path.lastPdf);
  }
}
