// SPDX-License-Identifier: Apache-2.0
// The reference path tracer (design policy section 9) as one fullscreen
// fragment pass: one camera path per pixel centre, brute-force BSDF
// sampling, emissive surfaces, a constant environment and Russian roulette.
// It also writes the barycentric intersection diagnostic. No accumulation.
import bsdf.bsdf;
import sampling.sampling;
import common.spectrum;

struct RayConstants {
  float4x4 world_to_clip;
  float4x4 clip_to_world;
};
[[vk::push_constant]] ConstantBuffer<RayConstants> frame;

// Mirrors GpuInstanceRecord in gpu_scene.hpp.
struct InstanceRecord {
  float4x4 world_from_object;
  float3* positions;
  uint3* triangles;
  uint geometry_slot;
  uint reserved[3];
  float4 base_color_roughness;
  float4 emission_metallic;
};

// Mirrors PathConstants in vulkan_backend.cpp.
struct PathConstants {
  float4 environment;
  InstanceRecord* instances;
  uint sample_index;
  uint max_bounces;
  uint output;
  uint width;
};
[[vk::binding(0, 0)]] RaytracingAccelerationStructure scene;
[[vk::binding(1, 0)]] ConstantBuffer<PathConstants> path_constants;

static const uint kOutputBarycentrics = 1;
// Bounces before Russian roulette may end a path.
static const uint kRouletteBounces = 3;
static const float kMaxSurvival = 0.95;
static const float kFloatMax = 3.402823466e+38;
static const uint kPathDone = 1;

// Design policy section 8. Light-carrying values are Spectrum (DES-Q3).
struct PathState {
  RayDesc ray;
  Spectrum throughput;
  Spectrum radiance;
  uint pixelIndex;
  // Scattering events so far.
  uint depth;
  uint rngState;
  // Solid-angle density of the last sampled direction, for MIS later.
  float lastPdf;
  uint flags;
};

struct Hit {
  uint instance;
  uint primitive;
  float2 barycentrics;
  float t;
};

bool Trace(RayDesc ray, out Hit hit) {
  RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
  query.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
  while (query.Proceed()) {}
  hit.instance = query.CommittedInstanceIndex();
  hit.primitive = query.CommittedPrimitiveIndex();
  hit.barycentrics = query.CommittedTriangleBarycentrics();
  hit.t = query.CommittedRayT();
  return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

struct SurfaceHit {
  float3 position;
  // The geometric normal, facing the incoming ray. Surfaces are two-sided.
  float3 normal;
  SurfaceBsdf bsdf;
  Spectrum emission;
};

// Surface hit reconstruction from the instance record and the geometry it
// addresses. Interpolating the world-space corners keeps the position on
// the triangle's plane.
SurfaceHit ReconstructHit(Hit hit, float3 direction) {
  const InstanceRecord instance = path_constants.instances[hit.instance];
  const uint3 triangle = instance.triangles[hit.primitive];
  const float3 p0 = mul(instance.world_from_object,
      float4(instance.positions[triangle.x], 1.0)).xyz;
  const float3 p1 = mul(instance.world_from_object,
      float4(instance.positions[triangle.y], 1.0)).xyz;
  const float3 p2 = mul(instance.world_from_object,
      float4(instance.positions[triangle.z], 1.0)).xyz;
  SurfaceHit surface;
  surface.position = p0 + hit.barycentrics.x * (p1 - p0) +
                     hit.barycentrics.y * (p2 - p0);
  const float3 normal = normalize(cross(p1 - p0, p2 - p0));
  surface.normal = dot(normal, direction) > 0.0 ? -normal : normal;
  surface.bsdf = MakeSurfaceBsdf(
      SpectrumFromRgb(instance.base_color_roughness.xyz),
      instance.base_color_roughness.w, instance.emission_metallic.w);
  surface.emission = SpectrumFromRgb(instance.emission_metallic.xyz);
  return surface;
}

// Moves a ray origin off its surface along the normal, by an amount that
// scales with the position's magnitude (Waechter and Binder, "A Fast and
// Robust Method for Avoiding Self-Intersection", Ray Tracing Gems, 2019).
float3 OffsetRayOrigin(float3 position, float3 normal) {
  const float kOrigin = 1.0 / 32.0;
  const float kFloatScale = 1.0 / 65536.0;
  const float kIntScale = 256.0;
  float3 result;
  [unroll]
  for (int axis = 0; axis < 3; ++axis) {
    const int offset = int(kIntScale * normal[axis]);
    const float moved = asfloat(
        asint(position[axis]) + (position[axis] < 0.0 ? -offset : offset));
    result[axis] = abs(position[axis]) < kOrigin
                       ? position[axis] + kFloatScale * normal[axis]
                       : moved;
  }
  return result;
}

struct VertexOutput {
  float4 position : SV_Position;
  float2 ndc : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertex_main(uint index : SV_VertexID) {
  VertexOutput output;
  float2 ndc = float2(index == 1 ? 3.0 : -1.0,
                     index == 2 ? 3.0 : -1.0);
  output.position = float4(ndc, 0.0, 1.0);
  output.ndc = ndc;
  return output;
}

struct FragmentOutput {
  float4 color : SV_Target;
  float depth : SV_Depth;
};

[shader("fragment")]
FragmentOutput fragment_main(VertexOutput input) {
  // Unproject Vulkan z=0 and z=1. Starting at the near plane handles
  // perspective and orthographic cameras, and clips geometry behind it.
  float4 near_h = mul(frame.clip_to_world, float4(input.ndc, 0.0, 1.0));
  float4 far_h = mul(frame.clip_to_world, float4(input.ndc, 1.0, 1.0));
  RayDesc ray;
  ray.Origin = near_h.xyz / near_h.w;
  ray.Direction = normalize(far_h.xyz * near_h.w - near_h.xyz * far_h.w);
  ray.TMin = 0.0;
  ray.TMax = far_h.w == 0.0 ? kFloatMax :
      length(far_h.xyz / far_h.w - ray.Origin);
  Hit hit;
  if (!Trace(ray, hit)) {
    discard;
  }
  float3 position = ray.Origin + ray.Direction * hit.t;
  float4 clip = mul(frame.world_to_clip, float4(position, 1.0));
  FragmentOutput output;
  output.depth = clip.z / clip.w;
  if (path_constants.output == kOutputBarycentrics) {
    output.color = float4(1.0 - hit.barycentrics.x - hit.barycentrics.y,
        hit.barycentrics.x, hit.barycentrics.y, 1.0);
    return output;
  }

  const uint2 pixel = uint2(input.position.xy);
  PathState path;
  path.ray = ray;
  path.throughput = SpectrumConstant(1.0);
  path.radiance = SpectrumConstant(0.0);
  path.pixelIndex = pixel.y * path_constants.width + pixel.x;
  path.depth = 0;
  path.rngState = SeedRandom(path.pixelIndex, path_constants.sample_index);
  path.lastPdf = 0.0;
  path.flags = 0;
  const Spectrum environment =
      SpectrumFromRgb(path_constants.environment.xyz);
  while ((path.flags & kPathDone) == 0) {
    const SurfaceHit surface = ReconstructHit(hit, path.ray.Direction);
    path.radiance = path.radiance + path.throughput * surface.emission;
    if (path.depth >= path_constants.max_bounces) {
      break;
    }
    if (path.depth >= kRouletteBounces) {
      const float survival =
          min(MaxComponent(path.throughput), kMaxSurvival);
      if (NextRandom(path.rngState) >= survival) {
        break;
      }
      path.throughput = path.throughput / survival;
    }
    const ShadingFrame basis = MakeShadingFrame(surface.normal);
    float3 u;
    u.x = NextRandom(path.rngState);
    u.y = NextRandom(path.rngState);
    u.z = NextRandom(path.rngState);
    BsdfSample sample;
    if (!SampleBsdf(surface.bsdf, basis.ToLocal(-path.ray.Direction), u,
            sample)) {
      break;
    }
    path.throughput = path.throughput * sample.weight;
    path.lastPdf = sample.pdf;
    path.ray.Origin = OffsetRayOrigin(surface.position, surface.normal);
    path.ray.Direction = normalize(basis.ToWorld(sample.wi));
    path.ray.TMin = 0.0;
    path.ray.TMax = kFloatMax;
    ++path.depth;
    if (!Trace(path.ray, hit)) {
      path.radiance = path.radiance + path.throughput * environment;
      path.flags |= kPathDone;
    }
  }
  output.color = float4(saturate(SpectrumToRgb(path.radiance)), 1.0);
  return output;
}
