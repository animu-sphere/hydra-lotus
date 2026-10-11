// SPDX-License-Identifier: Apache-2.0
// Random numbers, shading frames and hemisphere warps.
module sampling;

public static const float kPi = 3.14159265358979323846;

// PCG's RXS-M-XS output permutation as a hash (Jarzynski and Olano, "Hash
// Functions for GPU Rendering", 2020).
public uint PcgHash(uint value) {
  const uint state = value * 747796405u + 2891336453u;
  const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}

// A pixel's random sequence for one sample index.
public uint SeedRandom(uint pixel_index, uint sample_index) {
  return PcgHash(pixel_index ^ PcgHash(sample_index));
}

// The next number in [0, 1) from a 32-bit PCG generator state.
public float NextRandom(inout uint state) {
  state = state * 747796405u + 2891336453u;
  const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return float(((word >> 22u) ^ word) >> 8u) * (1.0 / 16777216.0);
}

// An orthonormal frame whose z axis is a unit normal (Duff et al., "Building
// an Orthonormal Basis, Revisited", 2017).
public struct ShadingFrame {
  public float3 tangent;
  public float3 bitangent;
  public float3 normal;

  public float3 ToLocal(float3 direction) {
    return float3(dot(direction, tangent), dot(direction, bitangent),
        dot(direction, normal));
  }

  public float3 ToWorld(float3 direction) {
    return direction.x * tangent + direction.y * bitangent +
           direction.z * normal;
  }
};

public ShadingFrame MakeShadingFrame(float3 normal) {
  const float sign = normal.z >= 0.0 ? 1.0 : -1.0;
  const float a = -1.0 / (sign + normal.z);
  const float b = normal.x * normal.y * a;
  ShadingFrame frame;
  frame.tangent = float3(1.0 + sign * normal.x * normal.x * a, sign * b,
      -sign * normal.x);
  frame.bitangent = float3(b, sign + normal.y * normal.y * a, -normal.y);
  frame.normal = normal;
  return frame;
}

// A film sample's offset from its pixel centre, in pixels, for the 1-pixel
// box filter: uniform over the pixel's square. Every sample has weight 1.
public float2 SampleBoxFilter(float2 u) {
  return u - 0.5;
}

// The power heuristic with exponent 2 (Veach, "Robust Monte Carlo Methods
// for Light Transport Simulation", 1997, section 9.2.4): the weight of the
// strategy whose density is `chosen` against one of density `other`. The
// ratio form neither overflows nor divides zero by zero.
public float PowerHeuristic(float chosen, float other) {
  if (!(chosen > 0.0)) {
    return 0.0;
  }
  if (isinf(chosen)) {
    return isinf(other) ? 0.5 : 1.0;
  }
  const float ratio = other / chosen;
  return 1.0 / (1.0 + ratio * ratio);
}

// A direction about +z with density cos(theta) / pi.
public float3 SampleCosineHemisphere(float2 u) {
  const float radius = sqrt(u.x);
  const float phi = 2.0 * kPi * u.y;
  return float3(radius * cos(phi), radius * sin(phi),
      sqrt(max(0.0, 1.0 - u.x)));
}

// A point of the unit disk, uniform by area: Shirley and Chiu's concentric
// mapping of the square (Pharr et al., Physically Based Rendering, 4th
// edition, section A.5.1).
public float2 SampleConcentricDisk(float2 u) {
  const float2 offset = 2.0 * u - 1.0;
  if (offset.x == 0.0 && offset.y == 0.0) {
    return float2(0.0);
  }
  float radius;
  float theta;
  if (abs(offset.x) > abs(offset.y)) {
    radius = offset.x;
    theta = 0.25 * kPi * (offset.y / offset.x);
  } else {
    radius = offset.y;
    theta = 0.5 * kPi - 0.25 * kPi * (offset.x / offset.y);
  }
  return radius * float2(cos(theta), sin(theta));
}

// A direction about +z, uniform over the cone of directions within
// theta_max of it, given 1 - cos(theta_max): its density is
// 1 / (2 pi (1 - cos(theta_max))).
public float3 SampleUniformCone(float2 u, float one_minus_cos_max) {
  const float one_minus_cos = u.x * one_minus_cos_max;
  const float sin_theta =
      sqrt(max(0.0, one_minus_cos * (2.0 - one_minus_cos)));
  const float phi = 2.0 * kPi * u.y;
  return float3(sin_theta * cos(phi), sin_theta * sin(phi),
      1.0 - one_minus_cos);
}
