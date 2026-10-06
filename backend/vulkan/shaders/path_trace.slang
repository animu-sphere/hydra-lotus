// SPDX-License-Identifier: Apache-2.0
// The reference path tracer (design policy section 9) as fullscreen fragment
// passes. The camera pass traces each pixel centre for depth and the
// barycentric intersection diagnostic, or in the surface pass a diagnostic
// of the hit's surface: its shading normal, albedo, or roughness and
// metallic. The radiance pass adds one camera
// path per pixel, through a box-filtered point of the pixel, to a
// floating-point accumulation image and writes the accumulated mean: brute-
// force BSDF sampling, emissive surfaces, a constant environment and Russian
// roulette.
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
  uint material_slot;
  // The address of three float3 normals per triangle, or 0 without authored
  // normals. An address, not a pointer: comparing a pointer with null makes
  // slangc emit a pointer variable that the SPIR-V validator of Vulkan SDK
  // 1.3.290 rejects (no AliasedPointer decoration).
  uint64_t normals;
  // The address of the three float2 texture coordinates per triangle that
  // the material's lookups read, or 0 without them.
  uint64_t texcoords;
  uint texcoord_set;
  uint reserved;
};

// Mirrors GpuTextureInputRecord in gpu_scene.hpp: one material input's
// texture lookup.
struct TextureInputRecord {
  uint texture;
  uint sampler_index;
  uint channel;
  uint mode;
  float4 scale;
  float4 bias;
  float4 fallback;
};
static const uint kConstantInput = 0;
static const uint kTextureLookup = 1;
static const uint kFallbackLookup = 2;
static const uint kBaseColorInput = 0;
static const uint kRoughnessInput = 1;
static const uint kMetallicInput = 2;
static const uint kEmissionInput = 3;

// Mirrors GpuMaterialRecord in gpu_scene.hpp: one entry of the material
// table, the Lotus material IR.
struct MaterialRecord {
  float4 base_color_roughness;
  float4 emission_metallic;
  float4 texcoord_fallback;
  TextureInputRecord inputs[4];
};

// Mirrors PathConstants in vulkan_backend.cpp.
struct PathConstants {
  float4 environment;
  // What a camera sample that misses the scene contributes to its pixel:
  // the target's clear colour.
  float4 background;
  // Framebuffer pixel coordinates to normalized device coordinates:
  // ndc = pixel * xy + zw, from the display window.
  float4 ndc_from_pixel;
  InstanceRecord* instances;
  MaterialRecord* materials;
  // The random sequence of the sample this frame adds.
  uint sample_index;
  // Samples already in the accumulation image; 0 restarts it.
  uint accumulated;
  // 0 writes the accumulated mean again without adding a sample.
  uint add_sample;
  uint max_bounces;
  uint width;
  // What the surface pass writes: kShadingNormalOutput, kAlbedoOutput or
  // kRoughnessMetallicOutput.
  uint surface_output;
  uint reserved0;
  uint reserved1;
};
[[vk::binding(0, 0)]] RaytracingAccelerationStructure scene;
[[vk::binding(1, 0)]] ConstantBuffer<PathConstants> path_constants;
// Per pixel: the sum of the radiance of the samples that hit the scene, and
// how many did.
[[vk::binding(2, 0)]] [[vk::image_format("rgba32f")]]
RWTexture2D<float4> accumulation;
// The GPU scene's texture table (kMaxTextures in gpu_scene.hpp), written
// only where textures are resident, and one sampler per pair of wrap modes.
[[vk::binding(3, 0)]] Texture2D<float4> textures[1024];
[[vk::binding(4, 0)]] SamplerState samplers[16];

// Which pass a pipeline runs: kCameraPass, kRadiancePass or kSurfacePass.
[vk::constant_id(0)] const uint kPass = 0;
static const uint kCameraPass = 0;
static const uint kRadiancePass = 1;
static const uint kSurfacePass = 2;
static const uint kShadingNormalOutput = 0;
static const uint kAlbedoOutput = 1;
static const uint kRoughnessMetallicOutput = 2;
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

// The camera ray through a point in normalized device coordinates. Starting
// at the unprojected Vulkan z=0 handles perspective and orthographic
// cameras, and clips geometry behind the near plane; z=1 bounds it.
RayDesc CameraRay(float2 ndc) {
  float4 near_h = mul(frame.clip_to_world, float4(ndc, 0.0, 1.0));
  float4 far_h = mul(frame.clip_to_world, float4(ndc, 1.0, 1.0));
  RayDesc ray;
  ray.Origin = near_h.xyz / near_h.w;
  ray.Direction = normalize(far_h.xyz * near_h.w - near_h.xyz * far_h.w);
  ray.TMin = 0.0;
  ray.TMax = far_h.w == 0.0 ? kFloatMax :
      length(far_h.xyz / far_h.w - ray.Origin);
  return ray;
}

struct SurfaceHit {
  float3 position;
  // The geometric normal, facing the incoming ray. Surfaces are two-sided.
  float3 normal;
  // The normal the BSDF is evaluated around: on the geometric normal's side
  // and facing the incoming ray.
  float3 shading_normal;
  // The material's inputs at the hit, after texture lookups.
  float3 base_color;
  float roughness;
  float metallic;
  SurfaceBsdf bsdf;
  Spectrum emission;
};

// A texture lookup as UsdUVTexture makes it: the bilinear texel at (s, t),
// whose rows run from the image's top, times scale plus bias; or the
// unscaled fallback when the input's texture is missing.
float4 Lookup(TextureInputRecord input, float2 st) {
  if (input.mode == kFallbackLookup) {
    return input.fallback;
  }
  const float4 texel =
      textures[NonUniformResourceIndex(input.texture)].SampleLevel(
          samplers[NonUniformResourceIndex(input.sampler_index)],
          float2(st.x, 1.0 - st.y), 0.0);
  return texel * input.scale + input.bias;
}

// The texture coordinates at a hit: the instance's set interpolated with the
// barycentrics, or the material's fallback.
float2 HitTexcoords(InstanceRecord instance, MaterialRecord material,
    Hit hit) {
  if (instance.texcoords == 0) {
    return material.texcoord_fallback.xy;
  }
  const uint corner = hit.primitive * 3;
  const float2 t0 = ((float2*)instance.texcoords)[corner];
  const float2 t1 = ((float2*)instance.texcoords)[corner + 1];
  const float2 t2 = ((float2*)instance.texcoords)[corner + 2];
  return t0 + hit.barycentrics.x * (t1 - t0) + hit.barycentrics.y * (t2 - t0);
}

// A hit's object-space corner normals `n` (one per row) interpolated at the
// hit, in world space and turned to the geometric normal's side. The
// geometric normal stands in where the interpolated normal vanishes and
// where it faces away from the incoming ray, which the BSDF could not
// reflect.
float3 ShadingNormal(float3x3 n, float2 barycentrics,
    float4x4 world_from_object, float3 geometric, float3 direction) {
  const float3 object = n[0] + barycentrics.x * (n[1] - n[0]) +
                        barycentrics.y * (n[2] - n[0]);
  // Normals transform by the inverse transpose of the linear part. Its
  // cofactor matrix is that times the determinant, whose sign the
  // orientation below discards, and needs no inverse.
  const float3 c0 = mul(world_from_object, float4(1.0, 0.0, 0.0, 0.0)).xyz;
  const float3 c1 = mul(world_from_object, float4(0.0, 1.0, 0.0, 0.0)).xyz;
  const float3 c2 = mul(world_from_object, float4(0.0, 0.0, 1.0, 0.0)).xyz;
  const float3 world = cross(c1, c2) * object.x + cross(c2, c0) * object.y +
                       cross(c0, c1) * object.z;
  const float length2 = dot(world, world);
  if (!(length2 > 0.0) || isinf(length2)) {
    return geometric;
  }
  float3 shading = world * rsqrt(length2);
  if (dot(shading, geometric) < 0.0) {
    shading = -shading;
  }
  return dot(shading, direction) < 0.0 ? shading : geometric;
}

// Surface hit reconstruction from the instance record and the geometry and
// material it addresses. Interpolating the world-space corners keeps the position on
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
  surface.shading_normal = surface.normal;
  if (instance.normals != 0) {
    const uint corner = hit.primitive * 3;
    const float3x3 normals = float3x3(((float3*)instance.normals)[corner],
        ((float3*)instance.normals)[corner + 1],
        ((float3*)instance.normals)[corner + 2]);
    surface.shading_normal = ShadingNormal(normals, hit.barycentrics,
        instance.world_from_object, surface.normal, direction);
  }
  const MaterialRecord material =
      path_constants.materials[instance.material_slot];
  surface.base_color = material.base_color_roughness.xyz;
  surface.roughness = material.base_color_roughness.w;
  surface.metallic = material.emission_metallic.w;
  float3 emission = material.emission_metallic.xyz;
  // A texture lookup replaces its input's constant, clamped into the
  // input's range.
  if (material.inputs[kBaseColorInput].mode != kConstantInput ||
      material.inputs[kRoughnessInput].mode != kConstantInput ||
      material.inputs[kMetallicInput].mode != kConstantInput ||
      material.inputs[kEmissionInput].mode != kConstantInput) {
    const float2 st = HitTexcoords(instance, material, hit);
    TextureInputRecord input = material.inputs[kBaseColorInput];
    if (input.mode != kConstantInput) {
      surface.base_color = saturate(Lookup(input, st).xyz);
    }
    input = material.inputs[kRoughnessInput];
    if (input.mode != kConstantInput) {
      surface.roughness = saturate(Lookup(input, st)[input.channel]);
    }
    input = material.inputs[kMetallicInput];
    if (input.mode != kConstantInput) {
      surface.metallic = saturate(Lookup(input, st)[input.channel]);
    }
    input = material.inputs[kEmissionInput];
    if (input.mode != kConstantInput) {
      emission = max(Lookup(input, st).xyz, 0.0);
    }
  }
  surface.bsdf = MakeSurfaceBsdf(SpectrumFromRgb(surface.base_color),
      surface.roughness, surface.metallic);
  surface.emission = SpectrumFromRgb(emission);
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

// The radiance a camera path carries back, given its first hit.
Spectrum TracePath(inout PathState path, Hit hit) {
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
    const ShadingFrame basis = MakeShadingFrame(surface.shading_normal);
    float3 u;
    u.x = NextRandom(path.rngState);
    u.y = NextRandom(path.rngState);
    u.z = NextRandom(path.rngState);
    BsdfSample sample;
    if (!SampleBsdf(surface.bsdf, basis.ToLocal(-path.ray.Direction), u,
            sample)) {
      break;
    }
    const float3 wi = normalize(basis.ToWorld(sample.wi));
    // Above the shading normal but below the geometric surface, a direction
    // would leave through the surface, which only reflects: the path ends.
    if (dot(wi, surface.normal) <= 0.0) {
      break;
    }
    path.throughput = path.throughput * sample.weight;
    path.lastPdf = sample.pdf;
    path.ray.Origin = OffsetRayOrigin(surface.position, surface.normal);
    path.ray.Direction = wi;
    path.ray.TMin = 0.0;
    path.ray.TMax = kFloatMax;
    ++path.depth;
    if (!Trace(path.ray, hit)) {
      path.radiance = path.radiance + path.throughput * environment;
      path.flags |= kPathDone;
    }
  }
  return path.radiance;
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

// The camera pass: the pixel centre's closest hit, as its projected depth
// and barycentric weights, or in the surface pass a diagnostic of its
// surface. Misses keep the attachments.
FragmentOutput CameraPass(VertexOutput input) {
  const RayDesc ray = CameraRay(input.ndc);
  Hit hit;
  if (!Trace(ray, hit)) {
    discard;
  }
  float3 position = ray.Origin + ray.Direction * hit.t;
  float4 clip = mul(frame.world_to_clip, float4(position, 1.0));
  FragmentOutput output;
  output.depth = clip.z / clip.w;
  if (kPass == kSurfacePass) {
    const SurfaceHit surface = ReconstructHit(hit, ray.Direction);
    if (path_constants.surface_output == kAlbedoOutput) {
      output.color = float4(surface.base_color, 1.0);
    } else if (path_constants.surface_output == kRoughnessMetallicOutput) {
      output.color = float4(surface.roughness, surface.metallic, 0.0, 1.0);
    } else {
      output.color = float4(surface.shading_normal, 1.0);
    }
  } else {
    output.color = float4(1.0 - hit.barycentrics.x - hit.barycentrics.y,
        hit.barycentrics.x, hit.barycentrics.y, 1.0);
  }
  return output;
}

// The radiance pass: adds one sample to the pixel's accumulation and writes
// the mean of its samples, a missed sample counting as the background. A
// pixel no sample has hit keeps the colour attachment. Depth is the camera
// pass's; this pipeline neither tests nor writes it.
FragmentOutput RadiancePass(VertexOutput input) {
  const uint2 pixel = uint2(input.position.xy);
  float4 sum = path_constants.accumulated == 0 ? float4(0.0)
                                               : accumulation[pixel];
  if (path_constants.add_sample != 0) {
    PathState path;
    path.throughput = SpectrumConstant(1.0);
    path.radiance = SpectrumConstant(0.0);
    path.pixelIndex = pixel.y * path_constants.width + pixel.x;
    path.depth = 0;
    path.rngState =
        SeedRandom(path.pixelIndex, path_constants.sample_index);
    path.lastPdf = 0.0;
    path.flags = 0;
    float2 u;
    u.x = NextRandom(path.rngState);
    u.y = NextRandom(path.rngState);
    const float2 film = float2(pixel) + 0.5 + SampleBoxFilter(u);
    path.ray = CameraRay(film * path_constants.ndc_from_pixel.xy +
                         path_constants.ndc_from_pixel.zw);
    Hit hit;
    if (Trace(path.ray, hit)) {
      sum += float4(SpectrumToRgb(TracePath(path, hit)), 1.0);
    }
    accumulation[pixel] = sum;
  }
  if (sum.w == 0.0) {
    discard;
  }
  const float count =
      float(path_constants.accumulated + path_constants.add_sample);
  const float4 background = path_constants.background;
  FragmentOutput output;
  output.color = float4(sum.xyz + (count - sum.w) * background.xyz,
                     sum.w + (count - sum.w) * background.w) / count;
  output.depth = 1.0;
  return output;
}

[shader("fragment")]
FragmentOutput fragment_main(VertexOutput input) {
  if (kPass == kRadiancePass) {
    return RadiancePass(input);
  }
  return CameraPass(input);
}
