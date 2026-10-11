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

// Mirrors GpuLightRecord in gpu_scene.hpp: one emissive triangle of the
// light table, chosen by the alias method (Vose, "A Linear Algorithm for
// Generating Random Numbers with a Given Distribution", 1991).
public struct LightRecord {
  // The probability with which the table chooses this triangle.
  public float pdf;
  // The column's probability of keeping its own triangle, else its alias's.
  public float threshold;
  public uint alias;
  // The triangle's instance; its primitive is its entry's index less the
  // instance's first entry.
  public uint instance;
};
// An instance without entries in the light table.
public static const uint kNoLights = 0xFFFFFFFFu;
// A light table entry whose instance has this bit is an analytic light,
// which the other bits index.
public static const uint kAnalyticLight = 0x80000000u;

// Mirrors GpuAnalyticLightRecord in gpu_scene.hpp: one analytic light in
// world space.
public struct AnalyticLightRecord {
  // A sphere's, rect's or disk's centre; w: a sphere's radius.
  public float4 position;
  // A rect's or disk's half axes. A distant light's unit direction towards
  // it, and in w its cone's 1 - cos(theta_max).
  public float4 axis_u;
  public float4 axis_v;
  // A rect's or disk's unit normal on its emitting side; w: the light's
  // world-space area.
  public float4 normal;
  // The radiance, or a parallel light's irradiance; w: the probability
  // with which the light table chooses the light.
  public float4 radiance;
  // x: kSphereLight, kRectLight, kDiskLight, kDistantLight or
  // kParallelLight.
  public uint4 shape;
};
// The instance of the light table's entry for the environment image.
public static const uint kEnvironmentLight = 0xFFFFFFFFu;

// Mirrors GpuEnvironmentRecord in gpu_scene.hpp: the environment image,
// followed in its buffer by its sampling distribution over columns x rows
// cells, rows from the top: each cell's density per unit image area, the
// rows' cumulative distribution (rows + 1 floats), then each row's over its
// cells (columns + 1 floats each).
public struct EnvironmentRecord {
  // Rows of the rotation from world space to the image's frame.
  public float4 environment_from_world[3];
  public float4 scale;
  public uint texture;
  public uint sampler_index;
  public uint columns;
  public uint rows;
};
static const uint64_t kEnvironmentRecordBytes = 80;

public static const uint kSphereLight = 0;
public static const uint kRectLight = 1;
public static const uint kDiskLight = 2;
public static const uint kDistantLight = 3;
public static const uint kParallelLight = 4;

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
  // kBsdfSampling or kMisLightSampling.
  public uint light_sampling;
  // The pixels the scene passes render, {x, y, width, height}: the data
  // window clamped to the target.
  public uint4 data_rect;
  // Per instance record, its first entry in the light table or kNoLights.
  public uint* light_offsets;
  public LightRecord* lights;
  public uint light_count;
  public uint analytic_count;
  public AnalyticLightRecord* analytic_lights;
  // The environment buffer's address, or 0 without an environment image,
  // and the probability with which the light table chooses the image.
  public uint64_t environment_map;
  public float environment_selection;
  public uint reserved;
};
public static const uint kBsdfSampling = 0;
public static const uint kMisLightSampling = 1;
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
  // The triangle's world-space area.
  public float area;
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
// The probability that a ray accepts a candidate: its opacity, or 0 or 1
// under binary masking.
float Coverage(Hit hit) {
  const InstanceRecord instance = path_constants.instances[hit.instance];
  const MaterialRecord material = path_constants.materials[instance.material_slot];
  float opacity = material.opacity.x;
  const TextureInputRecord input = material.inputs[kOpacityInput];
  if (input.mode != kConstantInput) {
    opacity = Lookup(input, HitTexcoords(instance, material, hit))[input.channel];
    // Lookup arithmetic can overflow even with finite float texels.
    opacity = isfinite(opacity) ? saturate(opacity) : 0.0;
  }
  if (material.opacity.y > 0.0) return opacity >= material.opacity.y ? 1.0 : 0.0;
  return opacity;
}

bool AcceptSurface(Hit hit, inout uint rng) {
  const float coverage = Coverage(hit);
  if (coverage <= 0.0) return false;
  if (coverage >= 1.0) return true;
  return NextRandom(rng) < coverage;
}

Hit CandidateHit<let Flags : int>(RayQuery<Flags> query) {
  Hit candidate;
  candidate.instance = query.CandidateInstanceIndex();
  candidate.primitive = query.CandidatePrimitiveIndex();
  candidate.barycentrics = query.CandidateTriangleBarycentrics();
  candidate.t = query.CandidateTriangleRayT();
  return candidate;
}

public bool Trace(RayDesc ray, inout uint rng, out Hit hit) {
  RayQuery<RAY_FLAG_NONE> query;
  query.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
  while (query.Proceed()) {
    if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
      if (AcceptSurface(CandidateHit(query), rng)) query.CommitNonOpaqueTriangleHit();
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

// The fraction of light a shadow ray carries through the surfaces on it:
// the product of their transparency, 1 - coverage, with no random numbers.
// Its expectation is the probability that a traced ray passes them all, as
// the coverage tests draw it.
public float ShadowTransmittance(RayDesc ray) {
  RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
  query.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
  float transmittance = 1.0;
  while (query.Proceed()) {
    if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
      const float coverage = Coverage(CandidateHit(query));
      if (coverage >= 1.0) {
        query.CommitNonOpaqueTriangleHit();
      } else if (coverage > 0.0) {
        transmittance *= 1.0 - coverage;
      }
    }
  }
  return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : transmittance;
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

float TriangleArea(float3 p0, float3 p1, float3 p2) {
  return 0.5 * length(cross(p1 - p0, p2 - p0));
}

// The material's emission at texture coordinates `st`.
float3 Emission(MaterialRecord material, float2 st) {
  const TextureInputRecord input = material.inputs[kEmissionInput];
  if (input.mode == kConstantInput) {
    return material.emission_metallic.xyz;
  }
  return max(Lookup(input, st).xyz, 0.0);
}

// A direction next event estimation chose towards a light.
public struct LightSample {
  // Unit length, from the shading point.
  public float3 direction;
  // Where the shadow ray ends.
  public float distance;
  // The radiance arriving along the direction, before shadowing: an
  // emissive triangle's emission times its coverage, the probability that
  // a ray reaching it stops there; a parallel light's irradiance.
  public Spectrum radiance;
  // The density of the direction per unit solid angle, the light's
  // selection included, or for a parallel light its selection's
  // probability; 0 when the light sends nothing towards the point.
  public float pdf;
  // A parallel light, which only next event estimation finds.
  public bool delta;
};

LightSample NoLight() {
  LightSample sample;
  sample.direction = float3(0.0, 0.0, 1.0);
  sample.distance = 0.0;
  sample.radiance = SpectrumConstant(0.0);
  sample.pdf = 0.0;
  sample.delta = false;
  return sample;
}

// A point uniformly by area on emissive triangle `light` of the table,
// seen from `origin`.
LightSample SampleTriangle(uint light, LightRecord chosen, float2 u,
    float3 origin) {
  Hit hit;
  hit.instance = chosen.instance;
  hit.primitive = light - path_constants.light_offsets[chosen.instance];
  // Uniform barycentrics (Pharr et al., Physically Based Rendering, 4th
  // edition, section A.5.5).
  const float root = sqrt(u.x);
  hit.barycentrics = float2(root * (1.0 - u.y), root * u.y);
  hit.t = 0.0;
  const InstanceRecord instance = path_constants.instances[hit.instance];
  const uint3 triangle = instance.triangles[hit.primitive];
  const float3 p0 = mul(instance.world_from_object,
      float4(instance.positions[triangle.x], 1.0)).xyz;
  const float3 p1 = mul(instance.world_from_object,
      float4(instance.positions[triangle.y], 1.0)).xyz;
  const float3 p2 = mul(instance.world_from_object,
      float4(instance.positions[triangle.z], 1.0)).xyz;
  const MaterialRecord material =
      path_constants.materials[instance.material_slot];
  const float3 position = p0 + hit.barycentrics.x * (p1 - p0) +
                          hit.barycentrics.y * (p2 - p0);
  const float3 normal = normalize(cross(p1 - p0, p2 - p0));
  const float area = TriangleArea(p0, p1, p2);
  const Spectrum radiance = SpectrumFromRgb(
      Emission(material, HitTexcoords(instance, material, hit)) * Coverage(hit));
  const float3 toward = position - origin;
  const float distance2 = dot(toward, toward);
  if (!(area > 0.0) || !(distance2 > 0.0) || MaxComponent(radiance) <= 0.0) {
    return NoLight();
  }
  LightSample sample;
  sample.direction = toward * rsqrt(distance2);
  const float cosine = abs(dot(normal, sample.direction));
  if (!(cosine > 0.0)) {
    return NoLight();
  }
  // End the shadow ray off the light's surface, on the receiver's side.
  const float3 end = OffsetRayOrigin(position,
      dot(normal, sample.direction) > 0.0 ? -normal : normal);
  sample.distance = max(0.0, dot(end - origin, sample.direction));
  sample.radiance = radiance;
  sample.pdf = chosen.pdf / area * distance2 / cosine;
  sample.delta = false;
  return sample;
}

// The density per unit solid angle with which a sphere light's directions
// are sampled from outside it, given sin^2 of the angle it subtends: uniform
// over its cone.
float SphereConePdf(float sin2_max) {
  // 1 - cos(theta_max) without cancellation for small spheres.
  const float one_minus_cos = sin2_max < 0.00068523
      ? 0.5 * sin2_max
      : 1.0 - sqrt(max(0.0, 1.0 - sin2_max));
  return 1.0 / (2.0 * kPi * one_minus_cos);
}

// A direction towards an analytic light, chosen with probability
// `selection`: by solid angle over a sphere's cone or a distant light's,
// by area on a rect or disk.
LightSample SampleAnalytic(AnalyticLightRecord light, float selection,
    float2 u, float3 origin) {
  LightSample sample = NoLight();
  sample.radiance = SpectrumFromRgb(light.radiance.xyz);
  const uint shape = light.shape.x;
  if (shape == kSphereLight) {
    // Uniform over the sphere's cone, then the cone's direction's point on
    // the sphere's near side (Pharr et al., Physically Based Rendering, 4th
    // edition, section 6.2.4).
    const float3 centre = light.position.xyz;
    const float radius = light.position.w;
    const float3 to_centre = centre - origin;
    const float distance2 = dot(to_centre, to_centre);
    if (!(distance2 > radius * radius)) {
      return NoLight();
    }
    const float sin2_max = radius * radius / distance2;
    const float sin_max = sqrt(sin2_max);
    const float cos_max = sqrt(max(0.0, 1.0 - sin2_max));
    float cos_theta = (cos_max - 1.0) * u.x + 1.0;
    float sin2_theta = 1.0 - cos_theta * cos_theta;
    if (sin2_max < 0.00068523) {
      sin2_theta = sin2_max * u.x;
      cos_theta = sqrt(1.0 - sin2_theta);
    }
    // The angle at the centre between the origin and the point.
    const float cos_alpha = sin2_theta / sin_max +
        cos_theta * sqrt(max(0.0, 1.0 - sin2_theta / sin2_max));
    const float sin_alpha = sqrt(max(0.0, 1.0 - cos_alpha * cos_alpha));
    const float phi = 2.0 * kPi * u.y;
    const ShadingFrame frame = MakeShadingFrame(normalize(to_centre));
    const float3 outward = frame.ToWorld(
        -float3(sin_alpha * cos(phi), sin_alpha * sin(phi), cos_alpha));
    const float3 toward = centre + radius * outward - origin;
    sample.distance = length(toward);
    sample.direction = toward / sample.distance;
    sample.pdf = selection * SphereConePdf(sin2_max);
  } else if (shape == kRectLight || shape == kDiskLight) {
    const float2 ab = shape == kRectLight ? 2.0 * u - 1.0
                                          : SampleConcentricDisk(u);
    const float3 toward = light.position.xyz + ab.x * light.axis_u.xyz +
                          ab.y * light.axis_v.xyz - origin;
    const float distance2 = dot(toward, toward);
    if (!(distance2 > 0.0)) {
      return NoLight();
    }
    sample.distance = sqrt(distance2);
    sample.direction = toward / sample.distance;
    // Light leaves only the emitting side.
    const float cosine = -dot(light.normal.xyz, sample.direction);
    if (!(cosine > 0.0)) {
      return NoLight();
    }
    sample.pdf = selection * distance2 / (cosine * light.normal.w);
  } else if (shape == kDistantLight) {
    const ShadingFrame frame = MakeShadingFrame(light.axis_u.xyz);
    sample.direction =
        normalize(frame.ToWorld(SampleUniformCone(u, light.axis_u.w)));
    sample.distance = kFloatMax;
    sample.pdf = selection / (2.0 * kPi * light.axis_u.w);
  } else {
    sample.direction = light.axis_u.xyz;
    sample.distance = kFloatMax;
    sample.pdf = selection;
    sample.delta = true;
  }
  return sample;
}

EnvironmentRecord LoadEnvironment() {
  return ((EnvironmentRecord*)path_constants.environment_map)[0];
}

// The distribution's floats after the record: densities, then the rows'
// cumulative distribution, then each row's.
float EnvironmentFloat(uint64_t index) {
  return ((float*)(path_constants.environment_map +
      kEnvironmentRecordBytes))[index];
}

// The largest i in [0, count) with cdf[i] <= u, for a cumulative
// distribution of count + 1 floats from `first`.
uint FindInterval(uint64_t first, uint count, float u) {
  uint low = 0;
  uint length = count + 1;
  while (length > 0) {
    const uint half = length >> 1;
    const uint middle = low + half;
    if (EnvironmentFloat(first + middle) <= u) {
      low = middle + 1;
      length -= half + 1;
    } else {
      length = half;
    }
  }
  return clamp(low, 1u, count) - 1;
}

// A point of [0, 1) distributed as the cumulative distribution of count
// cells from `first`, and the cell it falls in (Pharr et al., Physically
// Based Rendering, 3rd edition, section 13.6.7).
float SampleCumulative(uint64_t first, uint count, float u, out uint cell) {
  cell = FindInterval(first, count, u);
  const float low = EnvironmentFloat(first + cell);
  const float width = EnvironmentFloat(first + cell + 1) - low;
  const float offset = width > 0.0 ? (u - low) / width : 0.5;
  return min((float(cell) + offset) / float(count), 0.99999994);
}

// The image coordinates of a direction in the image's frame, as OpenEXR
// lays out latitude-longitude maps: u from longitude +pi at the left edge to
// -pi at the right, v from +Y at the top to -Y at the bottom.
float2 EnvironmentUv(float3 local) {
  const float u = 0.5 - atan2(local.x, local.z) * (0.5 / kPi);
  const float v = acos(clamp(local.y, -1.0, 1.0)) * (1.0 / kPi);
  return float2(u - floor(u), v);
}

float3 EnvironmentDirection(float2 uv) {
  const float theta = kPi * uv.y;
  const float phi = kPi * (1.0 - 2.0 * uv.x);
  const float sin_theta = sin(theta);
  return float3(sin_theta * sin(phi), cos(theta), sin_theta * cos(phi));
}

// The image's radiance at `uv`: its bilinear texel times the scale.
Spectrum EnvironmentLookup(EnvironmentRecord environment, float2 uv) {
  const float4 texel =
      textures[NonUniformResourceIndex(environment.texture)].SampleLevel(
          samplers[NonUniformResourceIndex(environment.sampler_index)], uv, 0.0);
  return SpectrumFromRgb(max(texel.xyz, 0.0) * environment.scale.xyz);
}

// The density per unit solid angle with which the distribution chooses the
// direction at `uv`, before the image's selection: the cell's density per
// unit image area over the area's 2 pi^2 sin(theta) steradians.
float EnvironmentDirectionPdf(EnvironmentRecord environment, float2 uv) {
  const uint column = min(uint(uv.x * float(environment.columns)),
      environment.columns - 1);
  const uint row = min(uint(uv.y * float(environment.rows)),
      environment.rows - 1);
  const float sin_theta = sin(kPi * uv.y);
  if (!(sin_theta > 0.0)) {
    return 0.0;
  }
  return EnvironmentFloat(uint64_t(row) * environment.columns + column) /
         (2.0 * kPi * kPi * sin_theta);
}

// A direction of the environment image chosen with probability
// `selection`, then by its distribution: a row by u.x, a cell of the row by
// u.y, uniformly within the cell.
LightSample SampleEnvironment(float selection, float2 u) {
  const EnvironmentRecord environment = LoadEnvironment();
  const uint64_t cells = uint64_t(environment.columns) * environment.rows;
  uint row;
  uint column;
  float2 uv;
  uv.y = SampleCumulative(cells, environment.rows, u.x, row);
  uv.x = SampleCumulative(cells + environment.rows + 1 +
          uint64_t(row) * (environment.columns + 1),
      environment.columns, u.y, column);
  const float sin_theta = sin(kPi * uv.y);
  const float density =
      EnvironmentFloat(uint64_t(row) * environment.columns + column);
  if (!(sin_theta > 0.0) || !(density > 0.0)) {
    return NoLight();
  }
  const float3 local = EnvironmentDirection(uv);
  LightSample sample;
  sample.direction = normalize(
      local.x * environment.environment_from_world[0].xyz +
      local.y * environment.environment_from_world[1].xyz +
      local.z * environment.environment_from_world[2].xyz);
  sample.distance = kFloatMax;
  sample.radiance = EnvironmentLookup(environment, uv);
  sample.pdf = selection * density / (2.0 * kPi * kPi * sin_theta);
  sample.delta = false;
  return sample;
}

// The environment image's radiance arriving along unit world `direction`.
// With `weigh`, it is weighed against light sampling's density for the
// direction by the power heuristic, the direction's own density being `pdf`.
public Spectrum EnvironmentRadiance(float3 direction, bool weigh, float pdf) {
  const EnvironmentRecord environment = LoadEnvironment();
  const float3 local = float3(
      dot(environment.environment_from_world[0].xyz, direction),
      dot(environment.environment_from_world[1].xyz, direction),
      dot(environment.environment_from_world[2].xyz, direction));
  const float2 uv = EnvironmentUv(local);
  const Spectrum radiance = EnvironmentLookup(environment, uv);
  if (!weigh || !(path_constants.environment_selection > 0.0)) {
    return radiance;
  }
  return radiance * PowerHeuristic(pdf, path_constants.environment_selection *
      EnvironmentDirectionPdf(environment, uv));
}

// Chooses a light from the light table in proportion to its estimated
// power (u.x, u.y), then a direction towards it from `origin` (u.zw).
public LightSample SampleLight(float4 u, float3 origin) {
  const uint count = path_constants.light_count;
  const uint column = min(uint(u.x * float(count)), count - 1);
  const LightRecord record = path_constants.lights[column];
  const uint light = u.y < record.threshold ? column : record.alias;
  const LightRecord chosen = path_constants.lights[light];
  if (chosen.instance == kEnvironmentLight) {
    return SampleEnvironment(chosen.pdf, u.zw);
  }
  if ((chosen.instance & kAnalyticLight) != 0) {
    return SampleAnalytic(
        path_constants.analytic_lights[chosen.instance & ~kAnalyticLight],
        chosen.pdf, u.zw, origin);
  }
  return SampleTriangle(light, chosen, u.zw, origin);
}

// The parallel light `index` of the analytic lights: its direction and
// irradiance, as a delta light of density 1.
public LightSample ParallelLight(uint index) {
  const AnalyticLightRecord light = path_constants.analytic_lights[index];
  if (light.shape.x != kParallelLight) {
    return NoLight();
  }
  LightSample sample;
  sample.direction = light.axis_u.xyz;
  sample.distance = kFloatMax;
  sample.radiance = SpectrumFromRgb(light.radiance.xyz);
  sample.pdf = 1.0;
  sample.delta = true;
  return sample;
}

// The probability density, per unit area, with which SampleLight chooses
// a point of the hit's emissive triangle.
public float EmitterPdf(Hit hit, float area) {
  const uint first = path_constants.light_offsets[hit.instance];
  if (first == kNoLights || !(area > 0.0)) {
    return 0.0;
  }
  return path_constants.lights[first + hit.primitive].pdf / area;
}

// The emission of the analytic lights that a ray from `origin` along unit
// `direction` crosses before `distance`, as the ray sees it: a sphere's near
// side from outside it, a rect's or disk's emitting side, and, for a ray
// that escapes the scene, the distant lights whose cone holds its direction.
// Lights neither block rays nor each other. With `weigh`, each is weighed
// against light sampling's density for the direction by the power
// heuristic, the ray's own density being `pdf`.
public Spectrum CrossedLights(float3 origin, float3 direction, float distance,
    bool escaped, bool weigh, float pdf) {
  Spectrum sum = SpectrumConstant(0.0);
  for (uint index = 0; index < path_constants.analytic_count; ++index) {
    const AnalyticLightRecord light = path_constants.analytic_lights[index];
    const uint shape = light.shape.x;
    // The density with which light sampling finds this direction.
    float light_pdf = 0.0;
    if (shape == kSphereLight) {
      // The nearer root without cancellation (Haines et al., "Precision
      // Improvements for Ray/Sphere Intersection", Ray Tracing Gems, 2019).
      const float radius = light.position.w;
      const float3 offset = origin - light.position.xyz;
      const float c = dot(offset, offset) - radius * radius;
      const float b = -dot(offset, direction);
      const float3 perpendicular = offset + b * direction;
      const float discriminant =
          radius * radius - dot(perpendicular, perpendicular);
      // Only from outside, towards the sphere.
      if (!(c > 0.0) || !(b > 0.0) || discriminant < 0.0) {
        continue;
      }
      const float t = c / (b + sqrt(discriminant));
      if (!(t < distance)) {
        continue;
      }
      light_pdf = SphereConePdf(radius * radius / dot(offset, offset));
    } else if (shape == kRectLight || shape == kDiskLight) {
      const float facing = dot(direction, light.normal.xyz);
      if (!(facing < 0.0)) {
        continue;
      }
      const float t = dot(light.position.xyz - origin, light.normal.xyz) / facing;
      if (!(t > 0.0) || !(t < distance)) {
        continue;
      }
      // The point's coordinates along the half axes.
      const float3 q = origin + t * direction - light.position.xyz;
      const float3 u = light.axis_u.xyz;
      const float3 v = light.axis_v.xyz;
      const float uu = dot(u, u);
      const float uv = dot(u, v);
      const float vv = dot(v, v);
      const float qu = dot(q, u);
      const float qv = dot(q, v);
      const float determinant = uu * vv - uv * uv;
      const float a = (qu * vv - qv * uv) / determinant;
      const float b = (qv * uu - qu * uv) / determinant;
      if (shape == kRectLight ? !(abs(a) <= 1.0 && abs(b) <= 1.0)
                              : !(a * a + b * b <= 1.0)) {
        continue;
      }
      light_pdf = t * t / (-facing * light.normal.w);
    } else if (shape == kDistantLight && escaped) {
      const float3 toward = light.axis_u.xyz;
      const float one_minus_cos = light.axis_u.w;
      const float cosine = dot(direction, toward);
      // Within the cone, by its sine where the cosine is too coarse.
      if (one_minus_cos <= 1.0) {
        const float3 across = cross(direction, toward);
        if (!(cosine > 0.0) ||
            !(dot(across, across) <= one_minus_cos * (2.0 - one_minus_cos))) {
          continue;
        }
      } else if (!(cosine >= 1.0 - one_minus_cos)) {
        continue;
      }
      light_pdf = 1.0 / (2.0 * kPi * one_minus_cos);
    } else {
      continue;
    }
    const float weight =
        weigh ? PowerHeuristic(pdf, light.radiance.w * light_pdf) : 1.0;
    sum = sum + SpectrumFromRgb(light.radiance.xyz) * weight;
  }
  return sum;
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
  surface.area = TriangleArea(p0, p1, p2);
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
    emission = Emission(material, st);
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
