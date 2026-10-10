// SPDX-License-Identifier: Apache-2.0
// What every scene pass reads: the GPU scene's records, the scene descriptor
// set, the camera, and ray traversal and hit reconstruction against them.
// The reference and wavefront integrators share it, so they trace and shade
// with the same code.
module scene;

import bsdf.bsdf;
import sampling.sampling;
import common.spectrum;

public struct RayConstants {
  public float4x4 world_to_clip;
  public float4x4 clip_to_world;
};
[[vk::push_constant]] public ConstantBuffer<RayConstants> frame;

// Mirrors GpuInstanceRecord in gpu_scene.hpp.
public struct InstanceRecord {
  public float4x4 world_from_object;
  public float3* positions;
  public uint3* triangles;
  public uint geometry_slot;
  public uint material_slot;
  // The address of three float3 normals per triangle, or 0 without authored
  // normals. An address, not a pointer: comparing a pointer with null makes
  // slangc emit a pointer variable that the SPIR-V validator of Vulkan SDK
  // 1.3.290 rejects (no AliasedPointer decoration).
  public uint64_t normals;
  // The address of the three float2 texture coordinates per triangle that
  // the material's lookups read, or 0 without them.
  public uint64_t texcoords;
  public uint texcoord_set;
  public uint reserved;
};

// Mirrors GpuTextureInputRecord in gpu_scene.hpp: one material input's
// texture lookup.
public struct TextureInputRecord {
  public uint texture;
  public uint sampler_index;
  public uint channel;
  public uint mode;
  public float4 scale;
  public float4 bias;
  public float4 fallback;
};
static const uint kConstantInput = 0;
static const uint kTextureLookup = 1;
static const uint kFallbackLookup = 2;
static const uint kBaseColorInput = 0;
static const uint kRoughnessInput = 1;
static const uint kMetallicInput = 2;
static const uint kEmissionInput = 3;
static const uint kNormalInput = 4;
static const uint kOpacityInput = 5;
static const uint kSpecularInput = 6;

// Mirrors GpuMaterialRecord in gpu_scene.hpp: one entry of the material
// table, the Lotus material IR.
public struct MaterialRecord {
  public float4 base_color_roughness;
  public float4 emission_metallic;
  public float4 texcoord_fallback;
  public float4 normal;
  public float4 opacity;
  public float4 specular_ior;
  public TextureInputRecord inputs[7];
};

// Mirrors PathConstants in vulkan_backend.cpp.
public struct PathConstants {
  public float4 environment;
  // What a camera sample that misses the scene contributes to its pixel:
  // the target's clear colour.
  public float4 background;
  // Framebuffer pixel coordinates to normalized device coordinates:
  // ndc = pixel * xy + zw, from the display window.
  public float4 ndc_from_pixel;
  public InstanceRecord* instances;
  public MaterialRecord* materials;
  // The random sequence of the sample this frame adds.
  public uint sample_index;
  // Samples already in the accumulation image; 0 restarts it.
  public uint accumulated;
  // 0 writes the accumulated mean again without adding a sample.
  public uint add_sample;
  public uint max_bounces;
  public uint width;
  // What the surface pass writes: kShadingNormalOutput, kAlbedoOutput or
  // kRoughnessMetallicOutput.
  public uint surface_output;
  // The wavefront integrator's path slots: one per target pixel.
  public uint path_capacity;
  public uint reserved;
  // The pixels the scene passes render, {x, y, width, height}: the data
  // window clamped to the target.
  public uint4 data_rect;
};
[[vk::binding(0, 0)]] public RaytracingAccelerationStructure scene;
[[vk::binding(1, 0)]] public ConstantBuffer<PathConstants> path_constants;
// Per pixel: the sum of the radiance of the samples that hit the scene, and
// how many did.
[[vk::binding(2, 0)]] [[vk::image_format("rgba32f")]]
public RWTexture2D<float4> accumulation;
// The GPU scene's texture table (kMaxTextures in gpu_scene.hpp), written
// only where textures are resident, and one sampler per pair of wrap modes.
[[vk::binding(3, 0)]] public Texture2D<float4> textures[1024];
[[vk::binding(4, 0)]] public SamplerState samplers[16];

public static const float kFloatMax = 3.402823466e+38;

public struct Hit {
  public uint instance;
  public uint primitive;
  public float2 barycentrics;
  public float t;
};

// The camera ray through a point in normalized device coordinates. Starting
// at the unprojected Vulkan z=0 handles perspective and orthographic
// cameras, and clips geometry behind the near plane; z=1 bounds it.
public RayDesc CameraRay(float2 ndc) {
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

public struct SurfaceHit {
  public float3 position;
  // The geometric normal, facing the incoming ray. Surfaces are two-sided.
  public float3 normal;
  // The normal the BSDF is evaluated around: on the geometric normal's side
  // and facing the incoming ray.
  public float3 shading_normal;
  // The material's inputs at the hit, after texture lookups.
  public float3 base_color;
  public float roughness;
  public float metallic;
  public SurfaceBsdf bsdf;
  public Spectrum emission;
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

// Alpha is surface coverage. Rejected candidates leave the ray's interval
// intact, with no offset, throughput change or scattering event. Geometry
// forbids duplicate candidates so a fractional test cannot be retried.
bool AcceptSurface(Hit hit, inout uint rng) {
  const InstanceRecord instance = path_constants.instances[hit.instance];
  const MaterialRecord material = path_constants.materials[instance.material_slot];
  float opacity = material.opacity.x;
  const TextureInputRecord input = material.inputs[kOpacityInput];
  if (input.mode != kConstantInput) {
    opacity = Lookup(input, HitTexcoords(instance, material, hit))[input.channel];
    // Lookup arithmetic can overflow even with finite float texels.
    opacity = isfinite(opacity) ? saturate(opacity) : 0.0;
  }
  if (material.opacity.y > 0.0) return opacity >= material.opacity.y;
  if (opacity <= 0.0) return false;
  if (opacity >= 1.0) return true;
  return NextRandom(rng) < opacity;
}

public bool Trace(RayDesc ray, inout uint rng, out Hit hit) {
  RayQuery<RAY_FLAG_NONE> query;
  query.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
  while (query.Proceed()) {
    if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
      Hit candidate;
      candidate.instance = query.CandidateInstanceIndex();
      candidate.primitive = query.CandidatePrimitiveIndex();
      candidate.barycentrics = query.CandidateTriangleBarycentrics();
      candidate.t = query.CandidateTriangleRayT();
      if (AcceptSurface(candidate, rng)) query.CommitNonOpaqueTriangleHit();
    }
  }
  hit = (Hit)0;
  if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return false;
  hit.instance = query.CommittedInstanceIndex();
  hit.primitive = query.CommittedPrimitiveIndex();
  hit.barycentrics = query.CommittedTriangleBarycentrics();
  hit.t = query.CommittedRayT();
  return true;
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

// UV differentials in world space, orthonormalized about the interpolated
// shading normal. Preserve the handedness of mirrored UVs and transforms.
// Missing, degenerate or ill-conditioned UVs use a stable normal-only frame.
ShadingFrame TangentFrame(InstanceRecord instance, Hit hit, float3 edge1,
    float3 edge2, float3 normal) {
  ShadingFrame basis = MakeShadingFrame(normal);
  if (instance.texcoords == 0) {
    return basis;
  }
  const uint corner = hit.primitive * 3;
  const float2 st0 = ((float2*)instance.texcoords)[corner];
  float2 uv1 = ((float2*)instance.texcoords)[corner + 1] - st0;
  float2 uv2 = ((float2*)instance.texcoords)[corner + 2] - st0;
  const float uv_scale = max(max(abs(uv1.x), abs(uv1.y)),
      max(abs(uv2.x), abs(uv2.y)));
  const float edge_scale = max(max(max(abs(edge1.x), abs(edge1.y)), abs(edge1.z)),
      max(max(abs(edge2.x), abs(edge2.y)), abs(edge2.z)));
  if (!(uv_scale > 0.0) || isinf(uv_scale) ||
      !(edge_scale > 0.0) || isinf(edge_scale)) {
    return basis;
  }
  uv1 /= uv_scale;
  uv2 /= uv_scale;
  edge1 /= edge_scale;
  edge2 /= edge_scale;
  const float determinant = uv1.x * uv2.y - uv1.y * uv2.x;
  if (!(abs(determinant) > 1e-8)) {
    return basis;
  }
  // Only the determinant's sign matters after normalization.
  const float orientation = determinant < 0.0 ? -1.0 : 1.0;
  float3 tangent = orientation * (edge1 * uv2.y - edge2 * uv1.y);
  const float3 bitangent = orientation * (edge2 * uv1.x - edge1 * uv2.x);
  tangent -= normal * dot(normal, tangent);
  const float length2 = dot(tangent, tangent);
  if (!(length2 > 1e-16) || isinf(length2)) {
    return basis;
  }
  basis.tangent = tangent * rsqrt(length2);
  basis.bitangent = cross(normal, basis.tangent);
  if (dot(basis.bitangent, bitangent) < 0.0) {
    basis.bitangent = -basis.bitangent;
  }
  return basis;
}

// Normal inputs are already signed tangent-space data. A zero or non-finite
// vector leaves the mesh normal. Keep reflection on the geometric side and
// fall back to the mesh normal if the mapped normal faces away from the ray.
float3 MappedNormal(float3 tangent_normal, ShadingFrame basis,
    float3 geometric, float3 direction) {
  const float magnitude = max(max(abs(tangent_normal.x), abs(tangent_normal.y)),
      abs(tangent_normal.z));
  if (!(magnitude > 0.0) || !all(isfinite(tangent_normal))) {
    return basis.normal;
  }
  float3 shading = normalize(basis.ToWorld(tangent_normal / magnitude));
  if (dot(shading, geometric) < 0.0) {
    shading = -shading;
  }
  return dot(shading, direction) < 0.0 ? shading : basis.normal;
}

// Surface hit reconstruction from the instance record and the geometry and
// material it addresses. Interpolating the world-space corners keeps the
// position on the triangle's plane.
public SurfaceHit ReconstructHit(Hit hit, float3 direction) {
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
  float3 tangent_normal = material.normal.xyz;
  float3 specular = material.specular_ior.xyz;
  // A texture lookup replaces its input's constant, clamped into the
  // input's range.
  if (material.inputs[kBaseColorInput].mode != kConstantInput ||
      material.inputs[kRoughnessInput].mode != kConstantInput ||
      material.inputs[kMetallicInput].mode != kConstantInput ||
      material.inputs[kEmissionInput].mode != kConstantInput ||
      material.inputs[kNormalInput].mode != kConstantInput ||
      material.inputs[kSpecularInput].mode != kConstantInput) {
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
    input = material.inputs[kNormalInput];
    if (input.mode != kConstantInput) {
      tangent_normal = clamp(Lookup(input, st).xyz, -1.0, 1.0);
    }
    input = material.inputs[kSpecularInput];
    if (input.mode != kConstantInput) {
      const float3 value = Lookup(input, st).xyz;
      specular = all(isfinite(value)) ? saturate(value) : material.specular_ior.xyz;
    }
  }
  // Preserve the constant-material reference path exactly, including the
  // interpolated normal's rounding, when the input is the identity.
  if (any(tangent_normal != float3(0.0, 0.0, 1.0))) {
    surface.shading_normal = MappedNormal(tangent_normal,
        TangentFrame(instance, hit, p1 - p0, p2 - p0, surface.shading_normal),
        surface.normal, direction);
  }
  surface.bsdf = MakeSurfaceBsdf(SpectrumFromRgb(surface.base_color),
      surface.roughness, surface.metallic, material.specular_ior.w,
      material.normal.w != 0.0, SpectrumFromRgb(specular));
  surface.emission = SpectrumFromRgb(emission);
  return surface;
}

// Moves a ray origin off its surface along the normal, by an amount that
// scales with the position's magnitude (Waechter and Binder, "A Fast and
// Robust Method for Avoiding Self-Intersection", Ray Tracing Gems, 2019).
public float3 OffsetRayOrigin(float3 position, float3 normal) {
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
