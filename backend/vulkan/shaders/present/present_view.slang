// SPDX-License-Identifier: Apache-2.0
// Present's debug views (DebugView in include/lotus/vulkan_backend.hpp): one
// invocation per pixel maps the latest frame's GPU debug product to a
// display colour in an RGBA16F image, which the presentation blits to the
// swapchain as it blits the scene colour. The product is read as raw uint4
// words at its record stride, so one binding serves every record layout;
// the layouts mirror TemporalPixel, TemporalValidityPixel, RestirPixel and
// RestirHistoryRecord in the backend.

// Mirrors DebugView.
static const uint kDepth = 1;
static const uint kNormal = 2;
static const uint kMotion = 3;
static const uint kTemporalValidity = 4;
static const uint kReservoirValidity = 5;
static const uint kReservoirAge = 6;
static const uint kReservoirWeight = 7;

struct ViewConstants {
  uint view;
  // Record stride in uint4 words: 4 for TemporalPixel, 1 for
  // TemporalValidityPixel, 3 for RestirPixel, 12 for RestirHistoryRecord.
  uint stride;
  uint width;
  uint height;
  float2 range;
  uint2 reserved;
};

[[vk::push_constant]] ConstantBuffer<ViewConstants> constants;
[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]]
RWTexture2D<float4> output;
[[vk::binding(1, 0)]] StructuredBuffer<uint4> source;

// The display colours reach the screen unchanged: the blit to the sRGB
// swapchain encodes what this decodes.
float3 Decode(float3 display) {
  const float3 c = saturate(display);
  return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}

float3 Heat(float t) {
  t = saturate(t);
  return saturate(float3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0),
      1.5 - abs(4.0 * t - 1.0)));
}

// Mirrors DebugViewRejectionColor.
float3 RejectionColor(uint rejection) {
  switch (rejection) {
  case 0: return float3(0.10, 0.75, 0.15);  // None: accepted
  case 1: return float3(0.0, 0.0, 0.0);     // Miss
  case 2: return float3(1.0, 0.0, 1.0);     // InvalidData
  case 3: return float3(0.95, 0.55, 0.10);  // InstanceChanged
  case 4: return float3(0.95, 0.85, 0.10);  // MaterialChanged
  case 5: return float3(0.15, 0.45, 0.95);  // LargeMotion
  case 6: return float3(0.90, 0.15, 0.15);  // DepthDiscontinuity
  case 7: return float3(0.60, 0.25, 0.85);  // NormalDiscontinuity
  case 8: return float3(0.45, 0.45, 0.45);  // HistoryUnavailable
  case 9: return float3(0.10, 0.80, 0.85);  // CorrespondenceMissing
  case 10: return float3(0.85, 0.85, 0.85); // ReprojectionOutside
  default: return float3(1.0, 0.0, 1.0);
  }
}

float3 Surface(uint base) {
  const float4 normal_depth = asfloat(source[base]);
  const float4 motion_depth = asfloat(source[base + 1]);
  const bool hit = (source[base + 3].x & 1u) != 0;
  if (!hit)
    return float3(0.0);
  if (constants.view == kDepth) {
    const float near = max(constants.range.x, 1.0e-6);
    const float far = max(constants.range.y, near * 1.0001);
    const float t = log(max(normal_depth.w, near) / near) / log(far / near);
    return float3(1.0 - 0.9 * saturate(t));
  }
  if (constants.view == kNormal)
    return 0.5 + 0.5 * normal_depth.xyz;
  if (motion_depth.w < 0.5)
    return float3(0.0);
  const float2 motion = clamp(motion_depth.xy / max(constants.range.y, 1.0e-6),
      -1.0, 1.0);
  return float3(0.5 + 0.5 * motion, 0.5);
}

float3 Reservoir(uint base) {
  const uint4 head = source[base];
  const uint flags = source[base + 1].w;
  if ((flags & 2u) != 0)
    return float3(1.0, 0.0, 1.0);
  if ((flags & 1u) == 0)
    return float3(0.0);
  const bool selected = head.x != 0xFFFFFFFFu && asfloat(head.z) > 0.0;
  const float3 no_sample = float3(0.35, 0.05, 0.05);
  if (constants.view == kReservoirValidity)
    return selected ? float3(0.10, 0.75, 0.15) : no_sample;
  const bool reuse = constants.stride == 12;
  if (constants.view == kReservoirAge) {
    const float age = reuse ? float(source[base + 3].w) : 0.0;
    return Heat(age / max(constants.range.y, 1.0));
  }
  if (!selected)
    return no_sample;
  // Initial reservoirs keep their raw sum; W = sum / (M * target).
  const float weight = reuse
      ? asfloat(source[base + 3].x)
      : asfloat(head.y) / (float(max(head.w, 1u)) * asfloat(head.z));
  const float span = max(constants.range.y - constants.range.x, 1.0e-6);
  return Heat((log10(max(weight, 1.0e-30)) - constants.range.x) / span);
}

[numthreads(8, 8, 1)]
void present_main(uint3 id: SV_DispatchThreadID) {
  if (id.x >= constants.width || id.y >= constants.height)
    return;
  const uint base = (id.y * constants.width + id.x) * constants.stride;
  float3 display;
  if (constants.view <= kMotion)
    display = Surface(base);
  else if (constants.view == kTemporalValidity) {
    const uint4 validity = source[base];
    display = validity.x != 0 ? RejectionColor(0) : RejectionColor(validity.y);
  } else
    display = Reservoir(base);
  output[id.xy] = float4(Decode(display), 1.0);
}
