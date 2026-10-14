// SPDX-License-Identifier: Apache-2.0
module temporal;

import common.scene;
import sampling.sampling;

// Mirrors TemporalConstants in vulkan_backend.cpp. Clip matrix uses Vulkan
// convention; pixel products still have a top-left origin.
struct TemporalConstants {
  float4x4 current_view;
  float4x4 previous_view;
  float4x4 previous_world_to_clip;
  float4 previous_display;
  uint4 previous_data;
  // absolute/relative depth tolerance, normal cosine, max pixel motion.
  float4 validation;
  uint4 history; // x: preceding frame available; y/z: full target extent.
  uint4 radiance; // enabled, fresh samples, bounded history samples, reserved
};
[[vk::binding(8, 0)]] ConstantBuffer<TemporalConstants> temporal_frame;

struct TemporalInstance {
  float4x4 previous_world_from_object;
  uint64_t instance_id;
  uint64_t material_id;
  uint flags;
  uint reserved0;
  uint reserved1;
  uint reserved2;
};
[[vk::binding(9, 0)]] StructuredBuffer<TemporalInstance> temporal_instances;

// Mirrors the plain-C++ TemporalPixel (64 bytes).
public struct TemporalPixel {
  public float4 normal_depth;
  public float4 motion_depth;
  public uint64_t instance_id;
  public uint64_t material_id;
  public uint flags;
  public uint reserved0;
  public uint reserved1;
  public uint reserved2;
};
[[vk::binding(10, 0)]] RWStructuredBuffer<TemporalPixel> temporal_surface;
[[vk::binding(11, 0)]] StructuredBuffer<TemporalPixel> temporal_previous;

struct TemporalValidityPixel {
  uint valid;
  uint rejection;
  uint previous_index;
  uint reserved;
};
[[vk::binding(12, 0)]] RWStructuredBuffer<TemporalValidityPixel> temporal_validity;

// Shared correspondence for consumers with jittered primary samples. The
// sample must belong to the centre metadata's instance/material too.
public void TemporalInstanceIdentity(uint instance, out uint64_t instance_id,
    out uint64_t material_id) {
  instance_id = temporal_instances[instance].instance_id;
  material_id = temporal_instances[instance].material_id;
}

public bool TemporalSampleHistory(uint index, uint64_t instance_id,
    uint64_t material_id, out uint previous_index) {
  previous_index = temporal_validity[index].previous_index;
  return temporal_validity[index].valid != 0 &&
      temporal_surface[index].instance_id == instance_id &&
      temporal_surface[index].material_id == material_id;
}

struct TemporalRadiancePixel {
  float4 color;
  uint sample_count;
  uint reserved0;
  uint reserved1;
  uint reserved2;
};
[[vk::binding(13, 0)]] StructuredBuffer<float4> temporal_batch;
[[vk::binding(14, 0)]] RWStructuredBuffer<TemporalRadiancePixel> temporal_radiance;
[[vk::binding(15, 0)]] StructuredBuffer<TemporalRadiancePixel> temporal_old_radiance;

public float4 AccumulateTemporalRadiance(float2 pixel) {
  const uint index = uint(pixel.y) * temporal_frame.history.y + uint(pixel.x);
  TemporalRadiancePixel output = {};
  output.color = temporal_batch[index];
  const uint cap = temporal_frame.radiance.z;
  if ((temporal_surface[index].flags & 1u) != 0) {
    output.sample_count = min(temporal_frame.radiance.y, cap);
    const TemporalValidityPixel validity = temporal_validity[index];
    if (validity.valid != 0) {
      const TemporalRadiancePixel old = temporal_old_radiance[validity.previous_index];
      const uint kept = min(old.sample_count, cap - output.sample_count);
      if (kept != 0 && all(isfinite(old.color))) {
        const uint count = kept + output.sample_count;
        output.color += (old.color - output.color) * (float(kept) / float(count));
        output.sample_count = count;
      }
    }
  }
  temporal_radiance[index] = output;
  return output.color;
}

// TemporalRejection's stable numeric codes. This contract contains no
// integrator, radiance, reservoir or denoising state.
uint ValidateHistory(TemporalPixel current, TemporalPixel previous) {
  if ((previous.flags & 1u) == 0)
    return 1u; // Miss
  const float4 thresholds = temporal_frame.validation;
  const float3 normal = current.normal_depth.xyz;
  const float3 old_normal = previous.normal_depth.xyz;
  if (!all(isfinite(current.normal_depth)) || !all(isfinite(previous.normal_depth)) ||
      !all(isfinite(current.motion_depth)) || !all(isfinite(thresholds)) ||
      current.normal_depth.w <= 0.0 || previous.normal_depth.w <= 0.0 ||
      current.motion_depth.z <= 0.0 || thresholds.x < 0.0 || thresholds.y < 0.0 ||
      thresholds.z < -1.0 || thresholds.z > 1.0 || thresholds.w < 0.0 ||
      all(normal == 0.0) || all(old_normal == 0.0))
    return 2u; // InvalidData
  if (current.instance_id != previous.instance_id)
    return 3u;
  if (current.material_id != previous.material_id)
    return 4u;
  if (length(current.motion_depth.xy) > thresholds.w)
    return 5u;
  if (abs(previous.normal_depth.w - current.motion_depth.z) >
      thresholds.x + thresholds.y * current.motion_depth.z)
    return 6u;
  if (dot(normalize(normal), normalize(old_normal)) < thresholds.z)
    return 7u;
  return 0u;
}

public void ReprojectHistory(float2 pixel) {
  const uint index = uint(pixel.y) * temporal_frame.history.y + uint(pixel.x);
  const TemporalPixel current = temporal_surface[index];
  TemporalValidityPixel output = {};
  output.previous_index = 0xffffffffu;
  if ((current.flags & 1u) == 0)
    output.rejection = 1u;
  else if (temporal_frame.history.x == 0)
    output.rejection = 8u; // HistoryUnavailable
  else if ((current.flags & 2u) == 0)
    output.rejection = 9u; // CorrespondenceMissing
  else if ((current.flags & 4u) == 0)
    output.rejection = 4u;
  else if (current.motion_depth.w != 1.0)
    output.rejection = 10u; // ReprojectionOutside
  else {
    const float2 old_pixel = pixel + current.motion_depth.xy;
    if (!all(isfinite(old_pixel)))
      output.rejection = 2u;
    else if (any(old_pixel < 0.0) || any(old_pixel >= float2(temporal_frame.history.yz)))
      output.rejection = 10u;
    else {
      // Nearest centre: floor of edge-origin continuous pixel coordinates.
      const uint2 fetched = uint2(old_pixel);
      output.previous_index = fetched.y * temporal_frame.history.y + fetched.x;
      output.rejection = ValidateHistory(current, temporal_previous[output.previous_index]);
    }
  }
  output.valid = output.rejection == 0u ? 1u : 0u;
  temporal_validity[index] = output;
}

// Separate from the depth attachment's test and radiance's jitter/RNG.
public void WriteTemporalSurface(float2 ndc, float2 pixel) {
  const uint index = uint(pixel.y) * path_constants.width + uint(pixel.x);
  uint rng = SeedRandom(index, 0);
  const RayDesc ray = CameraRay(ndc);
  Hit hit;
  if (!Trace(ray, rng, hit))
    return; // The whole buffer was cleared, including outside the data window.
  const InstanceRecord instance = path_constants.instances[hit.instance];
  const TemporalInstance identity = temporal_instances[hit.instance];
  const uint3 triangle = instance.triangles[hit.primitive];
  const float3 p0 = instance.positions[triangle.x];
  const float3 p1 = instance.positions[triangle.y];
  const float3 p2 = instance.positions[triangle.z];
  const float3 point = p0 + hit.barycentrics.x * (p1 - p0) +
      hit.barycentrics.y * (p2 - p0);
  const float4 world = mul(instance.world_from_object, float4(point, 1.0));
  const float4 view = mul(temporal_frame.current_view, world);
  const float3 a = mul(instance.world_from_object, float4(p0, 1.0)).xyz;
  const float3 b = mul(instance.world_from_object, float4(p1, 1.0)).xyz;
  const float3 c = mul(instance.world_from_object, float4(p2, 1.0)).xyz;
  float3 normal = normalize(cross(b - a, c - a));
  if (dot(normal, ray.Direction) > 0.0)
    normal = -normal;
  if (!all(isfinite(view)) || !all(isfinite(normal)) || view.w <= 0.0 || -view.z / view.w <= 0.0)
    return;
  TemporalPixel output = {};
  output.normal_depth = float4(normal, -view.z / view.w);
  output.instance_id = identity.instance_id;
  output.material_id = identity.material_id;
  output.flags = 1u | identity.flags;
  if ((identity.flags & 2u) != 0) {
    const float4 previous_world = mul(identity.previous_world_from_object,
        float4(point, 1.0));
    const float4 previous_view = mul(temporal_frame.previous_view, previous_world);
    const float4 clip = mul(temporal_frame.previous_world_to_clip, previous_world);
    const float3 projected = clip.xyz / clip.w;
    const float depth = -previous_view.z / previous_view.w;
    const float2 previous_pixel = temporal_frame.previous_display.xy +
        (projected.xy * 0.5 + 0.5) * temporal_frame.previous_display.zw;
    const float2 begin = float2(temporal_frame.previous_data.xy);
    const float2 end = begin + float2(temporal_frame.previous_data.zw);
    if (all(isfinite(previous_view)) && all(isfinite(clip)) &&
        all(isfinite(previous_pixel)) && isfinite(depth) &&
        previous_view.w > 0.0 && clip.w > 0.0 && depth > 0.0 &&
        projected.z >= 0.0 && projected.z <= 1.0 &&
        all(projected.xy >= -1.0) && all(projected.xy < 1.0) &&
        all(previous_pixel >= begin) && all(previous_pixel < end)) {
      output.motion_depth = float4(previous_pixel - pixel, depth, 1.0);
    }
  }
  temporal_surface[index] = output;
}
