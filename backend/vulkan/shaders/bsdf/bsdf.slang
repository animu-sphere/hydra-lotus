// SPDX-License-Identifier: Apache-2.0
// The reference path tracer's surface BSDF (design policy section 17): a
// Fresnel-attenuated Lambert base and a GGX reflection lobe, and a share of
// dielectric interface that reflects and transmits (docs/design/
// TRANSMISSION.md). Directions are in the shading frame, z along the normal
// facing the outgoing direction, and point away from the surface.
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
  // The metal's share of the layered lobes, the metal and the dielectric
  // over Lambert, which share what the transmitted share leaves.
  public float metallic;
  public Spectrum dielectric_f0;
  public float dielectric_f90;
  public Spectrum f0;
  public float f90;
  // The transmitted share, (1 - metallic) * transmission: the dielectric
  // interface's weight and the probability of sampling it. At 0 the
  // layered lobes are the whole surface.
  public float transmission;
  public Spectrum transmission_color;
  // The interface's relative index eta_t / eta_i from the outgoing
  // direction's side, or a thin wall's index against air.
  public float eta;
  public bool thin_walled;
  // The interface's roughness is below the alpha floor, or its index is 1:
  // it reflects and transmits along Dirac deltas.
  public bool smooth;
};

// `entering`: the outgoing direction lies outside the solid the surface
// bounds, so light arriving along it enters the solid.
public SurfaceBsdf MakeSurfaceBsdf(Spectrum base_color, float roughness,
    float metallic, float ior, bool specular_workflow, Spectrum specular,
    float transmission, Spectrum transmission_color, bool thin_walled,
    bool entering) {
  SurfaceBsdf bsdf;
  bsdf.base_color = base_color;
  bsdf.alpha = max(roughness * roughness, kMinAlpha);
  bsdf.metallic = specular_workflow ? 0.0 : metallic;
  // Materials with transmission use the metallic workflow.
  bsdf.transmission = (1.0 - bsdf.metallic) * transmission;
  bsdf.transmission_color = transmission_color;
  bsdf.eta = thin_walled || entering ? ior : 1.0 / ior;
  bsdf.thin_walled = thin_walled;
  bsdf.smooth = roughness * roughness < kMinAlpha || ior == 1.0;
  if (bsdf.transmission > 0.0) {
    // The metal's share of what the interface leaves.
    bsdf.metallic = bsdf.transmission < 1.0
        ? min(bsdf.metallic / (1.0 - bsdf.transmission), 1.0)
        : 0.0;
  }
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

// The exact unpolarized reflectance of a dielectric interface for light
// arriving at cos_theta from its normal, with relative index eta = eta_t /
// eta_i (Pharr et al., Physically Based Rendering, 4th edition, section
// 9.3): 1 beyond the critical angle.
float FresnelDielectric(float cos_theta, float eta) {
  cos_theta = saturate(cos_theta);
  const float sin2_t = (1.0 - cos_theta * cos_theta) / (eta * eta);
  if (sin2_t >= 1.0) return 1.0;
  const float cos_t = sqrt(1.0 - sin2_t);
  const float parallel = (eta * cos_theta - cos_t) / (eta * cos_theta + cos_t);
  const float perpendicular =
      (cos_theta - eta * cos_t) / (cos_theta + eta * cos_t);
  return 0.5 * (parallel * parallel + perpendicular * perpendicular);
}

// The share of light the interface reflects about a normal at cos_theta from
// the outgoing direction; it transmits the rest. A thin wall sums the
// interreflections inside it, 2R / (1 + R) (Pharr et al., section 9.5).
float InterfaceReflectance(SurfaceBsdf bsdf, float cos_theta) {
  const float r = FresnelDielectric(cos_theta, bsdf.eta);
  return bsdf.thin_walled ? 2.0 * r / (1.0 + r) : r;
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

// G2 / G1 for a visible normal sampled for `wo` that sends light along `wi`.
float SmithRatio(SurfaceBsdf bsdf, float3 wo, float3 wi) {
  const float lambda_o = GgxLambda(wo, bsdf.alpha);
  return (1.0 + lambda_o) / (1.0 + lambda_o + GgxLambda(wi, bsdf.alpha));
}

// f * cos(theta_i) of the layered lobes. Zero unless both directions are
// above the surface.
Spectrum LayeredEvalCos(SurfaceBsdf bsdf, float3 wo, float3 wi) {
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

// The solid-angle density with which the layered lobes are sampled: their
// densities weighted by their selection probabilities.
float LayeredPdf(SurfaceBsdf bsdf, float3 wo, float3 wi) {
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

// The transmitted share's rough reflection, f * cos(theta_i), for both
// directions above the surface.
float InterfaceReflectionEvalCos(SurfaceBsdf bsdf, float3 wo, float3 wi) {
  const float3 h = normalize(wo + wi);
  const float g2 = 1.0 /
      (1.0 + GgxLambda(wo, bsdf.alpha) + GgxLambda(wi, bsdf.alpha));
  return InterfaceReflectance(bsdf, dot(wo, h)) * GgxD(h, bsdf.alpha) * g2 /
         (4.0 * wo.z);
}

// f * cos(theta_i) of the surface's reflection: the layered lobes and the
// transmitted share's rough reflection. Zero unless both directions are
// above the surface. Delta lobes and transmission have no value here; the
// light that reaches a path through them is found by sampling alone.
public Spectrum BsdfEvalCos(SurfaceBsdf bsdf, float3 wo, float3 wi) {
  const Spectrum layered = LayeredEvalCos(bsdf, wo, wi);
  if (bsdf.transmission == 0.0) {
    return layered;
  }
  Spectrum value = layered * (1.0 - bsdf.transmission);
  if (!bsdf.smooth && wo.z > 0.0 && wi.z > 0.0) {
    value = value + SpectrumConstant(
        bsdf.transmission * InterfaceReflectionEvalCos(bsdf, wo, wi));
  }
  return value;
}

// The solid-angle density with which SampleBsdf chooses a reflected `wi`:
// the non-delta reflection lobes' densities weighted by their selection
// probabilities.
public float BsdfPdf(SurfaceBsdf bsdf, float3 wo, float3 wi) {
  const float layered = LayeredPdf(bsdf, wo, wi);
  if (bsdf.transmission == 0.0) {
    return layered;
  }
  float pdf = (1.0 - bsdf.transmission) * layered;
  if (!bsdf.smooth && wo.z > 0.0 && wi.z > 0.0) {
    const float3 h = normalize(wo + wi);
    pdf += bsdf.transmission * InterfaceReflectance(bsdf, dot(wo, h)) *
           GgxPdf(bsdf, wo, wi);
  }
  return pdf;
}

public struct BsdfSample {
  public float3 wi;
  // f * cos(theta_i) / pdf.
  public Spectrum weight;
  // BsdfPdf(wi) for a non-delta reflection, else 0.
  public float pdf;
  // Along a Dirac delta.
  public bool delta;
  // Below the surface, through the interface.
  public bool transmitted;
};

// The transmitted share's direction: about the shading normal if smooth,
// else a visible normal for `wo` (u.xy), the interface reflects with its
// reflectance and transmits otherwise (u.z). Each weight is the share's
// f * cos(theta_i) over its density, from which D and the selection
// probabilities cancel.
bool SampleInterface(SurfaceBsdf bsdf, float3 wo, float3 u,
    inout BsdfSample sample) {
  const float3 h = bsdf.smooth ? float3(0.0, 0.0, 1.0)
                               : SampleGgxVisibleNormal(wo, bsdf.alpha, u.xy);
  const float cos_oh = dot(wo, h);
  if (!(cos_oh > 0.0)) {
    return false;
  }
  const float3 reflected = 2.0 * cos_oh * h - wo;
  sample.delta = bsdf.smooth;
  if (u.z < InterfaceReflectance(bsdf, cos_oh)) {
    sample.wi = reflected;
    if (bsdf.smooth) {
      sample.weight = SpectrumConstant(1.0);
      return true;
    }
    if (reflected.z <= 0.0) {
      return false;
    }
    sample.weight = SpectrumConstant(SmithRatio(bsdf, wo, reflected));
    sample.pdf = BsdfPdf(bsdf, wo, reflected);
    return sample.pdf > 0.0 && IsFinite(sample.weight);
  }
  sample.transmitted = true;
  if (bsdf.thin_walled) {
    // Straight through, or the reflection mirrored through the tangent
    // plane: a rough wall blurs what is behind it without displacing it.
    if (!bsdf.smooth && reflected.z <= 0.0) {
      return false;
    }
    sample.wi = float3(reflected.xy, -reflected.z);
    sample.weight = bsdf.transmission_color *
        (bsdf.smooth ? 1.0 : SmithRatio(bsdf, wo, reflected));
    return IsFinite(sample.weight);
  }
  // Refraction about h by Snell's law; total internal reflection always
  // reflects above.
  const float sin2_t = (1.0 - cos_oh * cos_oh) / (bsdf.eta * bsdf.eta);
  if (sin2_t >= 1.0) {
    return false;
  }
  const float cos_t = sqrt(1.0 - sin2_t);
  sample.wi = -wo / bsdf.eta + (cos_oh / bsdf.eta - cos_t) * h;
  if (sample.wi.z >= 0.0) {
    return false;
  }
  // Radiance scales by 1 / eta^2 across the interface.
  sample.weight = bsdf.transmission_color *
      ((bsdf.smooth ? 1.0 : SmithRatio(bsdf, wo, sample.wi)) /
       (bsdf.eta * bsdf.eta));
  return IsFinite(sample.weight);
}

// Chooses the transmitted share with its probability (u.z), or else the
// layered lobes with u.z rescaled. Among those, chooses GGX using the view
// Fresnel and metal fraction (u.z), then a direction from it (u.xy).
// Returns false when no direction on the chosen side results.
public bool SampleBsdf(SurfaceBsdf bsdf, float3 wo, float3 u,
    out BsdfSample sample) {
  sample.wi = float3(0.0);
  sample.weight = SpectrumConstant(0.0);
  sample.pdf = 0.0;
  sample.delta = false;
  sample.transmitted = false;
  if (wo.z <= 0.0) {
    return false;
  }
  if (bsdf.transmission > 0.0) {
    if (u.z < bsdf.transmission) {
      return SampleInterface(bsdf, wo,
          float3(u.xy, u.z / bsdf.transmission), sample);
    }
    u.z = (u.z - bsdf.transmission) / (1.0 - bsdf.transmission);
  }
  if (u.z < SpecularProbability(bsdf, wo)) {
    const float3 h = SampleGgxVisibleNormal(wo, bsdf.alpha, u.xy);
    sample.wi = 2.0 * dot(wo, h) * h - wo;
  } else {
    sample.wi = SampleCosineHemisphere(u.xy);
  }
  const float layered_pdf = LayeredPdf(bsdf, wo, sample.wi);
  if (!(layered_pdf > 0.0)) {
    return false;
  }
  // The layered lobes' own estimate: their share's weight cancels its
  // selection probability. Weighing against light sampling takes the
  // density of every lobe that could have chosen wi.
  sample.pdf = bsdf.transmission > 0.0 ? BsdfPdf(bsdf, wo, sample.wi)
                                       : layered_pdf;
  sample.weight = LayeredEvalCos(bsdf, wo, sample.wi) / layered_pdf;
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
