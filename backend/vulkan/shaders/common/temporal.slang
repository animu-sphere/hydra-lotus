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

public float CurrentViewDepth(float3 position) {
  const float4 view = mul(temporal_frame.current_view, float4(position, 1.0));
  return -view.z / view.w;
}

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

// Mirrors TemporalRadiancePixel; scalar moments keep the 32-byte layout
// (a float2 would align to offset 24).
struct TemporalRadiancePixel {
  float4 color;
  uint sample_count;
  float first_moment;
  float second_moment;
  float variance;
};
[[vk::binding(13, 0)]] StructuredBuffer<float4> temporal_batch;
[[vk::binding(14, 0)]] RWStructuredBuffer<TemporalRadiancePixel> temporal_radiance;
[[vk::binding(15, 0)]] StructuredBuffer<TemporalRadiancePixel> temporal_old_radiance;

// Mirrors TemporalLuminance.
float Luminance(float4 color) {
  return 0.2126 * color.x + 0.7152 * color.y + 0.0722 * color.z;
}

// The batch's luminance moments are blended with the colour's weights. The
// variance pass, which needs every pixel's moments, writes the variance.
public float4 AccumulateTemporalRadiance(float2 pixel) {
  const uint index = uint(pixel.y) * temporal_frame.history.y + uint(pixel.x);
  TemporalRadiancePixel output = {};
  output.color = temporal_batch[index];
  const uint cap = temporal_frame.radiance.z;
  if ((temporal_surface[index].flags & 1u) != 0) {
    output.sample_count = min(temporal_frame.radiance.y, cap);
    const float luminance = Luminance(output.color);
    float2 moments = float2(luminance, luminance * luminance);
    const TemporalValidityPixel validity = temporal_validity[index];
    if (validity.valid != 0) {
      const TemporalRadiancePixel old = temporal_old_radiance[validity.previous_index];
      const uint kept = min(old.sample_count, cap - output.sample_count);
      const float2 old_moments = float2(old.first_moment, old.second_moment);
      if (kept != 0 && all(isfinite(old.color)) && all(isfinite(old_moments))) {
        const uint count = kept + output.sample_count;
        const float blend = float(kept) / float(count);
        output.color += (old.color - output.color) * blend;
        moments += (old_moments - moments) * blend;
        output.sample_count = count;
      }
    }
    output.first_moment = moments.x;
    output.second_moment = moments.y;
  }
  temporal_radiance[index] = output;
  return output.color;
}

// Usable neighbours for the variance estimate: a history on the same
// placement. Histories exist only on primary hits.
bool VarianceNeighbour(int2 q, uint64_t instance_id) {
  const uint2 extent = temporal_frame.history.yz;
  if (any(q < 0) || q.x >= int(extent.x) || q.y >= int(extent.y))
    return false;
  const uint index = uint(q.y) * extent.x + uint(q.x);
  return temporal_radiance[index].sample_count != 0 &&
      temporal_surface[index].instance_id == instance_id;
}

float VarianceDepthStep(int2 a, int2 b, float depth, uint64_t instance_id) {
  const uint width = temporal_frame.history.y;
  float step = -1.0; // No usable neighbour yet.
  if (VarianceNeighbour(a, instance_id))
    step = abs(depth - temporal_surface[uint(a.y) * width + uint(a.x)].normal_depth.w);
  if (VarianceNeighbour(b, instance_id)) {
    const float other = abs(depth - temporal_surface[uint(b.y) * width + uint(b.x)].normal_depth.w);
    step = step < 0.0 ? other : min(step, other);
  }
  return max(step, 0.0);
}

// Mirrors EstimateTemporalVariance: temporal variance from a long history,
// SVGF's edge-aware 7x7 moments estimate, scaled up, from a short one. Only
// the variance word is written; neighbours read the other words.
public void EstimateTemporalVariance(float2 pixel) {
  const uint width = temporal_frame.history.y;
  const int2 p = int2(pixel);
  const uint index = uint(p.y) * width + uint(p.x);
  const TemporalRadiancePixel center = temporal_radiance[index];
  float variance = 0.0;
  if (center.sample_count != 0) {
    const float batches = float(center.sample_count) /
        float(min(temporal_frame.radiance.y, temporal_frame.radiance.z));
    if (batches >= 4.0) {
      variance = max(0.0, center.second_moment - center.first_moment * center.first_moment);
    } else {
      const TemporalPixel s = temporal_surface[index];
      const float depth = s.normal_depth.w;
      const float phi = max(VarianceDepthStep(p - int2(1, 0), p + int2(1, 0), depth, s.instance_id) +
          VarianceDepthStep(p - int2(0, 1), p + int2(0, 1), depth, s.instance_id), 1.0e-8) * 3.0;
      float weight_sum = 0.0;
      float2 sum = float2(0.0);
      for (int dy = -3; dy <= 3; ++dy) {
        for (int dx = -3; dx <= 3; ++dx) {
          const int2 q = p + int2(dx, dy);
          if (!VarianceNeighbour(q, s.instance_id))
            continue;
          const uint qi = uint(q.y) * width + uint(q.x);
          const float4 n = temporal_surface[qi].normal_depth;
          const TemporalRadiancePixel neighbour = temporal_radiance[qi];
          const float distance = sqrt(float(dx * dx + dy * dy));
          const float wz = dx == 0 && dy == 0 ? 0.0 : abs(depth - n.w) / (phi * distance);
          const float wn = pow(saturate(dot(s.normal_depth.xyz, n.xyz)), 128.0);
          const float wl = abs(center.first_moment - neighbour.first_moment) / 10.0;
          const float w = exp(-(wl + wz)) * wn;
          sum += w * float2(neighbour.first_moment, neighbour.second_moment);
          weight_sum += w;
        }
      }
      const float2 m = sum / max(weight_sum, 1.0e-6);
      variance = max(0.0, m.y - m.x * m.x) * (4.0 / batches);
    }
  }
  temporal_radiance[index].variance = variance;
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
