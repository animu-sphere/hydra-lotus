// SPDX-License-Identifier: Apache-2.0
// Single-source bootstrap triangle. slangc compiles one entry point per SPIR-V
// artifact from this module, so the vertex/fragment interface lives in one
// file and the same source can later add a Metal target without a rewrite.

// The triangle sits in world space where the usdview smoke scene's mesh does,
// so the camera places it in both hosts alike.
static const float3 kPositions[3] = {
    float3(-0.5, -0.5, 0.0),
    float3( 0.5, -0.5, 0.0),
    float3( 0.0,  0.5, 0.0),
};

// The backend has already converted the camera to Vulkan clip space.
struct FrameConstants {
  float4x4 world_to_clip;
};

[[vk::push_constant]] ConstantBuffer<FrameConstants> frame;

struct VertexStageOutput {
  float4 position : SV_Position;
};

[shader("vertex")]
VertexStageOutput vertex_main(uint vertex_index : SV_VertexID) {
  VertexStageOutput output;
  output.position =
      mul(frame.world_to_clip, float4(kPositions[vertex_index], 1.0));
  return output;
}

[shader("fragment")]
float4 fragment_main() : SV_Target {
  return float4(0.80, 0.20, 0.10, 1.0);
}
