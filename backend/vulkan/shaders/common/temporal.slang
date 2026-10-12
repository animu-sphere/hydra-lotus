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
