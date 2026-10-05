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
