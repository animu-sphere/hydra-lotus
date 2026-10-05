// SPDX-License-Identifier: Apache-2.0
// Diagnostic primary rays: no BSDF, lighting or accumulation yet.
struct RayConstants {
  float4x4 world_to_clip;
  float4x4 clip_to_world;
};
[[vk::push_constant]] ConstantBuffer<RayConstants> frame;
[[vk::binding(0, 0)]] RaytracingAccelerationStructure scene;

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
  ray.TMax = far_h.w == 0.0 ? 3.402823466e+38 :
      length(far_h.xyz / far_h.w - ray.Origin);
  RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
  query.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
  while (query.Proceed()) {}
  if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
    discard;
  }
  float2 bary = query.CommittedTriangleBarycentrics();
  float3 hit = ray.Origin + ray.Direction * query.CommittedRayT();
  float4 clip = mul(frame.world_to_clip, float4(hit, 1.0));
  FragmentOutput output;
  output.color = float4(1.0 - bary.x - bary.y, bary.x, bary.y, 1.0);
  output.depth = clip.z / clip.w;
  return output;
}
