// SPDX-License-Identifier: Apache-2.0
// The reference light transport (design policy section 9), as the steps of
// one camera path: start it, scatter it at each hit, and end it on a miss.
// Brute-force BSDF sampling, emissive surfaces, a constant environment and
// Russian roulette. The reference integrator runs the steps in one loop per
// pixel; the wavefront integrator runs each as a kernel over a queue of
// paths. Both consume a path's random numbers in the same order, so a pixel
// and sample index select the same path in either.
module transport;

import bsdf.bsdf;
import sampling.sampling;
import common.spectrum;
import common.scene;

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
  // Solid-angle density of the last sampled direction, for MIS later.
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

// One scattering event at the path's hit: adds the surface's emission, then
// either ends the path (false) or samples the BSDF and leaves the
// continuation ray in path.ray (true).
public bool ScatterPath(inout PathState path, Hit hit) {
  const SurfaceHit surface = ReconstructHit(hit, path.ray.Direction);
  path.radiance = path.radiance + path.throughput * surface.emission;
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
  float3 u;
  u.x = NextRandom(path.rngState);
  u.y = NextRandom(path.rngState);
  u.z = NextRandom(path.rngState);
  BsdfSample sample;
  if (!SampleBsdf(surface.bsdf, basis.ToLocal(-path.ray.Direction), u,
          sample)) {
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
// environment's radiance.
public void MissPath(inout PathState path) {
  path.radiance = path.radiance +
      path.throughput * SpectrumFromRgb(path_constants.environment.xyz);
}
