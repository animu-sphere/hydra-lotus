// SPDX-License-Identifier: Apache-2.0
// The light-transport value (design policy section 8, DES-Q3). Throughput,
// radiance, BSDF values and emission are all a Spectrum. It holds linear RGB
// today; spectral rendering (Renderer Phase 10) replaces its contents and
// the conversions at the edges, not the code that transports it. Its
// components are internal, so other modules cannot depend on RGB.
module spectrum;

public struct Spectrum {
  internal float3 rgb;
};

public Spectrum SpectrumConstant(float value) {
  Spectrum spectrum;
  spectrum.rgb = float3(value);
  return spectrum;
}

// Material and environment inputs enter the transport here.
public Spectrum SpectrumFromRgb(float3 rgb) {
  Spectrum spectrum;
  spectrum.rgb = rgb;
  return spectrum;
}

// The renderer's linear RGB output leaves the transport here.
public float3 SpectrumToRgb(Spectrum spectrum) {
  return spectrum.rgb;
}

// A Spectrum as storage buffers hold it, such as a section of the wavefront
// integrator's path state: one 16-byte entry whose last word is the
// caller's. Entries are words rather than a struct with a float3: slangc
// 1.4.350 gives every float3 pointer of a module the 16-byte array stride
// of a float3 in a std430 buffer, which misreads the scene's tightly packed
// positions.
public uint4 PackSpectrum(Spectrum spectrum, uint extra) {
  return uint4(asuint(spectrum.rgb), extra);
}

public Spectrum UnpackSpectrum(uint4 packed) {
  return SpectrumFromRgb(asfloat(packed.xyz));
}

public Spectrum operator+(Spectrum left, Spectrum right) {
  return SpectrumFromRgb(left.rgb + right.rgb);
}

public Spectrum operator*(Spectrum left, Spectrum right) {
  return SpectrumFromRgb(left.rgb * right.rgb);
}

public Spectrum operator*(Spectrum spectrum, float scale) {
  return SpectrumFromRgb(spectrum.rgb * scale);
}

public Spectrum operator*(float scale, Spectrum spectrum) {
  return SpectrumFromRgb(scale * spectrum.rgb);
}

public Spectrum operator/(Spectrum spectrum, float divisor) {
  return SpectrumFromRgb(spectrum.rgb / divisor);
}

public float MaxComponent(Spectrum spectrum) {
  return max(spectrum.rgb.x, max(spectrum.rgb.y, spectrum.rgb.z));
}

public bool IsFinite(Spectrum spectrum) {
  return all(isfinite(spectrum.rgb));
}
