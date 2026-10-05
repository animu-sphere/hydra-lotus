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

// A direction about +z with density cos(theta) / pi.
public float3 SampleCosineHemisphere(float2 u) {
  const float radius = sqrt(u.x);
  const float phi = 2.0 * kPi * u.y;
  return float3(radius * cos(phi), radius * sin(phi),
      sqrt(max(0.0, 1.0 - u.x)));
}
