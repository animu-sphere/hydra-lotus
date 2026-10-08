// SPDX-License-Identifier: Apache-2.0
// The reference path tracer's surface BSDF (design policy section 17): a
// Fresnel-attenuated Lambert base and a GGX reflection lobe. Directions are in
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
  public Spectrum dielectric_f0;
  public float dielectric_f90;
  public Spectrum f0;
  public float f90;
};

public SurfaceBsdf MakeSurfaceBsdf(Spectrum base_color, float roughness,
    float metallic, float ior, bool specular_workflow, Spectrum specular) {
  SurfaceBsdf bsdf;
  bsdf.base_color = base_color;
  bsdf.alpha = max(roughness * roughness, kMinAlpha);
  bsdf.metallic = specular_workflow ? 0.0 : metallic;
  // This form avoids overflow for large finite indices.
  const float ratio = 1.0 - 2.0 / (ior + 1.0);
  bsdf.dielectric_f0 = specular_workflow ? specular : SpectrumConstant(ratio * ratio);
  bsdf.dielectric_f90 = specular_workflow || ior > 1.0 ? 1.0 : 0.0;
  bsdf.f0 = base_color * bsdf.metallic +
      bsdf.dielectric_f0 * (1.0 - bsdf.metallic);
  // Preserve the established metal lobe's white Schlick grazing limit.
  bsdf.f90 = bsdf.metallic + (1.0 - bsdf.metallic) * bsdf.dielectric_f90;
  return bsdf;
}

// Schlick's Fresnel approximation.
Spectrum FresnelSchlick(Spectrum f0, float f90, float cos_theta) {
  const float weight = pow(saturate(1.0 - cos_theta), 5.0);
  return f0 * (1.0 - weight) + SpectrumConstant(f90 * weight);
}

// A reciprocal, single-scattering coat: light crosses the dielectric on
// entry and exit. Internal diffuse reflection and GGX multiple scattering
// are not compensated. Index-matched materials recover uncoated Lambert.
Spectrum DielectricTransmission(SurfaceBsdf bsdf, float cos_theta) {
  const Spectrum fresnel = FresnelSchlick(bsdf.dielectric_f0,
      bsdf.dielectric_f90, cos_theta);
  return SpectrumConstant(1.0) + fresnel * -1.0;
}

float SpecularProbability(SurfaceBsdf bsdf, float3 wo) {
  if (bsdf.metallic == 1.0) return 1.0;
  if (bsdf.dielectric_f90 == 0.0) return bsdf.metallic;
  const float dielectric = clamp(MaxComponent(FresnelSchlick(
      bsdf.dielectric_f0, bsdf.dielectric_f90, wo.z)), 0.05, 0.95);
  return bsdf.metallic + (1.0 - bsdf.metallic) * dielectric;
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

// GGX reflection's f * cos(theta_i), with the height-correlated Smith
// masking-shadowing term.
Spectrum GgxEvalCos(SurfaceBsdf bsdf, float3 wo, float3 wi) {
  const float3 h = normalize(wo + wi);
  const float g2 = 1.0 /
      (1.0 + GgxLambda(wo, bsdf.alpha) + GgxLambda(wi, bsdf.alpha));
  return FresnelSchlick(bsdf.f0, bsdf.f90, dot(wo, h)) *
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
  value = value * DielectricTransmission(bsdf, wo.z) *
      DielectricTransmission(bsdf, wi.z);
  if (bsdf.f90 > 0.0) {
    value = value + GgxEvalCos(bsdf, wo, wi);
  }
  return value;
}

// The solid-angle density with which SampleBsdf chooses `wi`: the lobes'
// densities weighted by their selection probabilities.
public float BsdfPdf(SurfaceBsdf bsdf, float3 wo, float3 wi) {
  if (wo.z <= 0.0 || wi.z <= 0.0) {
    return 0.0;
  }
  const float specular = SpecularProbability(bsdf, wo);
  float pdf = (1.0 - specular) * wi.z / kPi;
  if (specular > 0.0) {
    pdf += specular * GgxPdf(bsdf, wo, wi);
  }
  return pdf;
}

public struct BsdfSample {
  public float3 wi;
  // f * cos(theta_i) / pdf.
  public Spectrum weight;
  public float pdf;
};

// Chooses GGX using the view Fresnel and metal fraction (u.z), then a direction
// from it (u.xy). Returns false when no direction above the surface results.
public bool SampleBsdf(SurfaceBsdf bsdf, float3 wo, float3 u,
    out BsdfSample sample) {
  sample.wi = float3(0.0);
  sample.weight = SpectrumConstant(0.0);
  sample.pdf = 0.0;
  if (wo.z <= 0.0) {
    return false;
  }
  if (u.z < SpecularProbability(bsdf, wo)) {
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
  // For a pure GGX surface D cancels analytically. Avoid dividing two
  // independently rounded, very large densities at near-mirror roughness.
  if (bsdf.metallic == 1.0) {
    const float3 h = normalize(wo + sample.wi);
    const float lambda_o = GgxLambda(wo, bsdf.alpha);
    sample.weight = FresnelSchlick(bsdf.f0, bsdf.f90, dot(wo, h)) *
        ((1.0 + lambda_o) / (1.0 + lambda_o + GgxLambda(sample.wi, bsdf.alpha)));
  }
  return IsFinite(sample.weight);
}
