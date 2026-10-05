// SPDX-License-Identifier: Apache-2.0
// The reference path tracer's surface BSDF (design policy section 17): a
// Lambert lobe and a GGX metal lobe, mixed by `metallic`. Directions are in
// the shading frame, z along the normal facing the outgoing direction, and
// both point away from the surface.
module bsdf;

import sampling.sampling;
import common.spectrum;

// Roughness 0 is a near-mirror: below this alpha, GGX sampling and
// evaluation lose float precision.
static const float kMinAlpha = 1.0e-3;

public struct SurfaceBsdf {
  // Lambert albedo, and the metal's reflectance at normal incidence.
  public Spectrum base_color;
  // GGX alpha: roughness squared.
  public float alpha;
  public float metallic;
};

public SurfaceBsdf MakeSurfaceBsdf(Spectrum base_color, float roughness,
    float metallic) {
  SurfaceBsdf bsdf;
  bsdf.base_color = base_color;
  bsdf.alpha = max(roughness * roughness, kMinAlpha);
  bsdf.metallic = metallic;
  return bsdf;
}

// Schlick's Fresnel approximation.
Spectrum FresnelSchlick(Spectrum f0, float cos_theta) {
  const float weight = pow(saturate(1.0 - cos_theta), 5.0);
  return f0 * (1.0 - weight) + SpectrumConstant(weight);
}

// The isotropic GGX normal distribution.
float GgxD(float3 h, float alpha) {
  const float alpha2 = alpha * alpha;
  const float denominator = h.z * h.z * (alpha2 - 1.0) + 1.0;
  return alpha2 / (kPi * denominator * denominator);
}

// Smith's Lambda for GGX.
float GgxLambda(float3 w, float alpha) {
  const float tan2 = max(0.0, 1.0 - w.z * w.z) / (w.z * w.z);
  return 0.5 * (sqrt(1.0 + alpha * alpha * tan2) - 1.0);
}

// A visible normal for `wo`, by spherical caps (Dupuy and Benyoub,
// "Sampling Visible GGX Normals with Spherical Caps", 2023).
float3 SampleGgxVisibleNormal(float3 wo, float alpha, float2 u) {
  const float3 wo_std = normalize(float3(wo.xy * alpha, wo.z));
  const float phi = 2.0 * kPi * u.x;
  const float z = (1.0 - u.y) * (1.0 + wo_std.z) - wo_std.z;
  const float sin_theta = sqrt(saturate(1.0 - z * z));
  const float3 h_std =
      float3(sin_theta * cos(phi), sin_theta * sin(phi), z) + wo_std;
  return normalize(float3(h_std.xy * alpha, h_std.z));
}

// The GGX metal's f * cos(theta_i), with the height-correlated Smith
// masking-shadowing term.
Spectrum GgxEvalCos(SurfaceBsdf bsdf, float3 wo, float3 wi) {
  const float3 h = normalize(wo + wi);
  const float g2 = 1.0 /
      (1.0 + GgxLambda(wo, bsdf.alpha) + GgxLambda(wi, bsdf.alpha));
  return FresnelSchlick(bsdf.base_color, dot(wo, h)) *
         (GgxD(h, bsdf.alpha) * g2 / (4.0 * wo.z));
}

// The solid-angle density of visible-normal sampling followed by reflection.
float GgxPdf(SurfaceBsdf bsdf, float3 wo, float3 wi) {
  const float3 h = normalize(wo + wi);
  const float g1 = 1.0 / (1.0 + GgxLambda(wo, bsdf.alpha));
  return GgxD(h, bsdf.alpha) * g1 / (4.0 * wo.z);
}

// f * cos(theta_i) of the whole surface. Zero unless both directions are
// above the surface.
public Spectrum BsdfEvalCos(SurfaceBsdf bsdf, float3 wo, float3 wi) {
  if (wo.z <= 0.0 || wi.z <= 0.0) {
    return SpectrumConstant(0.0);
  }
  Spectrum value = bsdf.base_color * ((1.0 - bsdf.metallic) * wi.z / kPi);
  if (bsdf.metallic > 0.0) {
    value = value + GgxEvalCos(bsdf, wo, wi) * bsdf.metallic;
  }
  return value;
}

// The solid-angle density with which SampleBsdf chooses `wi`: the lobes'
// densities weighted by their selection probabilities.
public float BsdfPdf(SurfaceBsdf bsdf, float3 wo, float3 wi) {
  if (wo.z <= 0.0 || wi.z <= 0.0) {
    return 0.0;
  }
  float pdf = (1.0 - bsdf.metallic) * wi.z / kPi;
  if (bsdf.metallic > 0.0) {
    pdf += bsdf.metallic * GgxPdf(bsdf, wo, wi);
  }
  return pdf;
}

public struct BsdfSample {
  public float3 wi;
  // f * cos(theta_i) / pdf.
  public Spectrum weight;
  public float pdf;
};

// Chooses the GGX lobe with probability `metallic` (u.z), then a direction
// from it (u.xy). Returns false when no direction above the surface results.
public bool SampleBsdf(SurfaceBsdf bsdf, float3 wo, float3 u,
    out BsdfSample sample) {
  sample.wi = float3(0.0);
  sample.weight = SpectrumConstant(0.0);
  sample.pdf = 0.0;
  if (wo.z <= 0.0) {
    return false;
  }
  if (u.z < bsdf.metallic) {
    const float3 h = SampleGgxVisibleNormal(wo, bsdf.alpha, u.xy);
    sample.wi = 2.0 * dot(wo, h) * h - wo;
  } else {
    sample.wi = SampleCosineHemisphere(u.xy);
  }
  sample.pdf = BsdfPdf(bsdf, wo, sample.wi);
  if (!(sample.pdf > 0.0)) {
    return false;
  }
  sample.weight = BsdfEvalCos(bsdf, wo, sample.wi) / sample.pdf;
  return IsFinite(sample.weight);
}
