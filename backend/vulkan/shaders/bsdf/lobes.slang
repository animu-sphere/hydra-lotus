// SPDX-License-Identifier: Apache-2.0
// The primitive lobes composed closures are built from (docs/design/
// LAYERED_BSDF.md section 5.1): Lambert, GGX with Schlick's Fresnel, and a
// GGX dielectric interface that reflects, transmits or both. Each lobe has
// its own unit normal in the shading frame and evaluates in the frame about
// it (section 5.3); the functions here take directions already in that
// frame, z along the lobe's normal. Values are f * |cos(theta_i)| about the
// lobe's normal, in radiance mode, and exclude the lobe's delta components.
//
// The single-scattering functions reuse bsdf.slang's microfacet helpers. A
// lobe's multiple-scattering compensation is a factor its composed closure
// computes for its outgoing direction (section 4.2) and stores in the lobe.
module lobes;

import bsdf.bsdf;
import sampling.sampling;
import common.spectrum;

public static const uint kLambertLobe = 0;
public static const uint kGgxSchlickLobe = 1;
public static const uint kGgxDielectricLobe = 2;

// A dielectric's scatter mode, MaterialX's `R`, `T` and `RT`.
public static const uint kScatterReflect = 1;
public static const uint kScatterTransmit = 2;
public static const uint kScatterBoth = 3;

public struct Lobe {
  public uint type;
  // Unit, in the shading frame.
  public float3 normal;
  // Lambert's albedo, GgxSchlick's f0, or GgxDielectric's tint, which
  // scales what it reflects and transmits.
  public Spectrum color;
  // GgxSchlick's f90.
  public Spectrum f90;
  // GGX alpha, at least kMinAlpha.
  public float alpha;
  // GgxDielectric's relative index eta_t / eta_i from the outgoing
  // direction's side, or a thin wall's index against its surroundings.
  public float eta;
  public uint scatter;
  public bool thin_walled;
  // GgxDielectric's alpha is below the floor or its index is 1: it reflects
  // and transmits along Dirac deltas.
  public bool smooth;
  // GGX reflection is compensated for multiple scattering.
  public bool compensate;
  // The compensation factor for the closure's outgoing direction, 1 when
  // the lobe is not compensated. Set by the composed closure.
  public Spectrum compensation;
};

Lobe MakeLobe(uint type, float3 normal) {
  Lobe lobe;
  lobe.type = type;
  lobe.normal = normal;
  lobe.color = SpectrumConstant(1.0);
  lobe.f90 = SpectrumConstant(1.0);
  lobe.alpha = 1.0;
  lobe.eta = 1.0;
  lobe.scatter = kScatterReflect;
  lobe.thin_walled = false;
  lobe.smooth = false;
  lobe.compensate = false;
  lobe.compensation = SpectrumConstant(1.0);
  return lobe;
}

// MaterialX's oren_nayar_diffuse_bsdf at roughness 0.
public Lobe MakeLambertLobe(Spectrum color, float3 normal) {
  Lobe lobe = MakeLobe(kLambertLobe, normal);
  lobe.color = color;
  return lobe;
}

// MaterialX's generalized_schlick_bsdf in R mode with exponent 5. Its alpha
// is clamped at the floor rather than made a delta, as the preview
// closure's lobes are.
public Lobe MakeGgxSchlickLobe(Spectrum f0, Spectrum f90, float alpha,
    float3 normal, bool compensate) {
  Lobe lobe = MakeLobe(kGgxSchlickLobe, normal);
  lobe.color = f0;
  lobe.f90 = f90;
  lobe.alpha = max(alpha, kMinAlpha);
  lobe.compensate = compensate;
  return lobe;
}

// MaterialX's dielectric_bsdf. `compensate` applies to its reflection only
// (section 4.2).
public Lobe MakeGgxDielectricLobe(float eta, Spectrum tint, float alpha,
    uint scatter, bool thin_walled, float3 normal, bool compensate) {
  Lobe lobe = MakeLobe(kGgxDielectricLobe, normal);
  lobe.color = tint;
  lobe.alpha = max(alpha, kMinAlpha);
  lobe.eta = eta;
  lobe.scatter = scatter;
  lobe.thin_walled = thin_walled;
  lobe.smooth = alpha < kMinAlpha || eta == 1.0;
  lobe.compensate = compensate;
  return lobe;
}

public bool LobeIsDelta(Lobe lobe) {
  return lobe.type == kGgxDielectricLobe && lobe.smooth;
}

// --- Frames ------------------------------------------------------------------

// An orthonormal basis about a unit normal (Duff et al., "Building an
// Orthonormal Basis, Revisited", 2017). For the shading normal (0, 0, 1)
// it is the identity, exactly.
public struct LobeFrame {
  public float3 tangent;
  public float3 bitangent;
  public float3 normal;
};

public LobeFrame MakeLobeFrame(float3 normal) {
  const float sign = normal.z >= 0.0 ? 1.0 : -1.0;
  const float a = -1.0 / (sign + normal.z);
  const float b = normal.x * normal.y * a;
  LobeFrame frame;
  frame.tangent = float3(1.0 + sign * normal.x * normal.x * a, sign * b,
      -sign * normal.x);
  frame.bitangent = float3(b, sign + normal.y * normal.y * a, -normal.y);
  frame.normal = normal;
  return frame;
}

public float3 ToLobe(LobeFrame frame, float3 w) {
  return float3(dot(w, frame.tangent), dot(w, frame.bitangent),
      dot(w, frame.normal));
}

public float3 FromLobe(LobeFrame frame, float3 w) {
  return w.x * frame.tangent + w.y * frame.bitangent + w.z * frame.normal;
}

// --- Shared terms --------------------------------------------------------------

// Schlick's Fresnel with exponent 5 and colour f90.
Spectrum SchlickFresnel(Spectrum f0, Spectrum f90, float cos_theta) {
  const float weight = pow(saturate(1.0 - cos_theta), 5.0);
  return f0 * (1.0 - weight) + f90 * weight;
}

// The share of light the dielectric reflects about a microfacet at
// cos_theta from the outgoing direction. A thin wall sums the
// interreflections inside it, 2R / (1 + R).
public float DielectricReflectance(Lobe lobe, float cos_theta) {
  const float r = FresnelDielectric(cos_theta, lobe.eta);
  return lobe.thin_walled ? 2.0 * r / (1.0 + r) : r;
}

// GGX reflection's D * G2 / (4 cos(theta_o)), the f * cos(theta_i) of a
// Fresnel of 1, with the height-correlated Smith term.
float GgxReflection(float3 wo, float3 wi, float3 h, float alpha) {
  const float g2 = 1.0 / (1.0 + GgxLambda(wo, alpha) + GgxLambda(wi, alpha));
  return GgxD(h, alpha) * g2 / (4.0 * wo.z);
}

// The density of visible-normal sampling followed by reflection.
float GgxReflectionPdf(float3 wo, float3 h, float alpha) {
  const float g1 = 1.0 / (1.0 + GgxLambda(wo, alpha));
  return GgxD(h, alpha) * g1 / (4.0 * wo.z);
}

// A solid interface's refraction toward wi.z < 0 about the generalized half
// vector (Walter et al., 2007): the visible-normal density with its
// Jacobian, and f * |cos(theta_i)| in radiance mode without the Fresnel
// share. Zero where no microfacet that faces wo refracts into wi.
struct Refraction {
  float value;
  float pdf;
  float cos_oh;
};

Refraction GgxRefraction(float3 wo, float3 wi, float alpha, float eta) {
  Refraction result;
  result.value = 0.0;
  result.pdf = 0.0;
  result.cos_oh = 0.0;
  float3 h = normalize(wo + eta * wi);
  if (h.z < 0.0) {
    h = -h;
  }
  const float cos_oh = dot(wo, h);
  const float cos_ih = dot(wi, h);
  if (!(cos_oh > 0.0) || !(cos_ih < 0.0)) {
    return result;
  }
  const float denominator = cos_ih + cos_oh / eta;
  const float jacobian = -cos_ih / (denominator * denominator);
  const float d = GgxD(h, alpha);
  const float lambda_o = GgxLambda(wo, alpha);
  const float g1 = 1.0 / (1.0 + lambda_o);
  const float g2 = 1.0 / (1.0 + lambda_o + GgxLambda(wi, alpha));
  result.pdf = (g1 * cos_oh * d / wo.z) * jacobian;
  result.value = (g2 * cos_oh * d / wo.z) * jacobian / (eta * eta);
  result.cos_oh = cos_oh;
  return result;
}

// --- Evaluation ----------------------------------------------------------------

Spectrum DielectricValue(Lobe lobe, float3 wo, float3 wi) {
  if (lobe.smooth) {
    return SpectrumConstant(0.0);
  }
  if (wi.z > 0.0) {
    if ((lobe.scatter & kScatterReflect) == 0) {
      return SpectrumConstant(0.0);
    }
    const float3 h = normalize(wo + wi);
    return lobe.color * lobe.compensation *
        (DielectricReflectance(lobe, dot(wo, h)) *
         GgxReflection(wo, wi, h, lobe.alpha));
  }
  if (!(wi.z < 0.0) || (lobe.scatter & kScatterTransmit) == 0) {
    return SpectrumConstant(0.0);
  }
  if (lobe.thin_walled) {
    // The reflection mirrored through the tangent plane.
    const float3 mirrored = float3(wi.xy, -wi.z);
    const float3 h = normalize(wo + mirrored);
    return lobe.color * ((1.0 - DielectricReflectance(lobe, dot(wo, h))) *
        GgxReflection(wo, mirrored, h, lobe.alpha));
  }
  const Refraction refraction = GgxRefraction(wo, wi, lobe.alpha, lobe.eta);
  if (!(refraction.value > 0.0)) {
    return SpectrumConstant(0.0);
  }
  return lobe.color * ((1.0 - DielectricReflectance(lobe, refraction.cos_oh)) *
      refraction.value);
}

// f * |cos(theta_i)| of the lobe's non-delta components, in its frame.
public Spectrum LobeValue(Lobe lobe, float3 wo, float3 wi) {
  if (!(wo.z > 0.0)) {
    return SpectrumConstant(0.0);
  }
  switch (lobe.type) {
  case kLambertLobe:
    return wi.z > 0.0 ? lobe.color * (wi.z / kPi) : SpectrumConstant(0.0);
  case kGgxSchlickLobe: {
    if (!(wi.z > 0.0)) {
      return SpectrumConstant(0.0);
    }
    const float3 h = normalize(wo + wi);
    return SchlickFresnel(lobe.color, lobe.f90, dot(wo, h)) *
        lobe.compensation * GgxReflection(wo, wi, h, lobe.alpha);
  }
  default:
    return DielectricValue(lobe, wo, wi);
  }
}

float DielectricPdf(Lobe lobe, float3 wo, float3 wi) {
  if (lobe.smooth) {
    return 0.0;
  }
  const bool both = lobe.scatter == kScatterBoth;
  if (wi.z > 0.0) {
    if ((lobe.scatter & kScatterReflect) == 0) {
      return 0.0;
    }
    const float3 h = normalize(wo + wi);
    const float choice = both ? DielectricReflectance(lobe, dot(wo, h)) : 1.0;
    return choice * GgxReflectionPdf(wo, h, lobe.alpha);
  }
  if (!(wi.z < 0.0) || (lobe.scatter & kScatterTransmit) == 0) {
    return 0.0;
  }
  if (lobe.thin_walled) {
    const float3 mirrored = float3(wi.xy, -wi.z);
    const float3 h = normalize(wo + mirrored);
    const float choice =
        both ? 1.0 - DielectricReflectance(lobe, dot(wo, h)) : 1.0;
    return choice * GgxReflectionPdf(wo, h, lobe.alpha);
  }
  const Refraction refraction = GgxRefraction(wo, wi, lobe.alpha, lobe.eta);
  if (!(refraction.pdf > 0.0)) {
    return 0.0;
  }
  const float choice =
      both ? 1.0 - DielectricReflectance(lobe, refraction.cos_oh) : 1.0;
  return choice * refraction.pdf;
}

// The solid-angle density with which LobeSample returns wi as a non-delta
// direction, in the lobe's frame.
public float LobePdf(Lobe lobe, float3 wo, float3 wi) {
  if (!(wo.z > 0.0)) {
    return 0.0;
  }
  switch (lobe.type) {
  case kLambertLobe:
    return wi.z > 0.0 ? wi.z / kPi : 0.0;
  case kGgxSchlickLobe:
    return wi.z > 0.0 ? GgxReflectionPdf(wo, normalize(wo + wi), lobe.alpha)
                      : 0.0;
  default:
    return DielectricPdf(lobe, wo, wi);
  }
}

// --- Sampling ------------------------------------------------------------------

public struct LobeSampleResult {
  // In the lobe's frame.
  public float3 wi;
  public bool delta;
  // A delta direction's weight: its share of the lobe's delta albedo over
  // the probability of choosing it.
  public Spectrum delta_weight;
};

// A smooth dielectric: the mirror direction or the refracted one, chosen
// with u.z by the reflectance in RT mode.
bool SampleSmoothDielectric(Lobe lobe, float3 wo, float u,
    inout LobeSampleResult sample) {
  sample.delta = true;
  const float reflectance = DielectricReflectance(lobe, wo.z);
  bool reflect = lobe.scatter == kScatterReflect;
  if (lobe.scatter == kScatterBoth) {
    reflect = u < reflectance;
  }
  if (reflect) {
    sample.wi = float3(-wo.xy, wo.z);
    sample.delta_weight = lobe.color *
        (lobe.scatter == kScatterBoth ? 1.0 : reflectance);
    return reflectance > 0.0;
  }
  const float share = lobe.scatter == kScatterBoth ? 1.0 : 1.0 - reflectance;
  if (lobe.thin_walled) {
    sample.wi = -wo;
    sample.delta_weight = lobe.color * share;
    return share > 0.0;
  }
  const float sin2_t = (1.0 - wo.z * wo.z) / (lobe.eta * lobe.eta);
  if (sin2_t >= 1.0) {
    return false;
  }
  sample.wi = -wo / lobe.eta +
      float3(0.0, 0.0, wo.z / lobe.eta - sqrt(1.0 - sin2_t));
  // Radiance scales by 1 / eta^2 across the interface.
  sample.delta_weight = lobe.color * (share / (lobe.eta * lobe.eta));
  return share > 0.0;
}

bool SampleDielectric(Lobe lobe, float3 wo, float3 u,
    inout LobeSampleResult sample) {
  if (lobe.smooth) {
    return SampleSmoothDielectric(lobe, wo, u.z, sample);
  }
  const float3 h = SampleGgxVisibleNormal(wo, lobe.alpha, u.xy);
  const float cos_oh = dot(wo, h);
  if (!(cos_oh > 0.0)) {
    return false;
  }
  const float3 reflected = 2.0 * cos_oh * h - wo;
  bool reflect = lobe.scatter == kScatterReflect;
  if (lobe.scatter == kScatterBoth) {
    reflect = u.z < DielectricReflectance(lobe, cos_oh);
  }
  if (reflect) {
    sample.wi = reflected;
    return reflected.z > 0.0;
  }
  if (lobe.thin_walled) {
    sample.wi = float3(reflected.xy, -reflected.z);
    return reflected.z > 0.0;
  }
  // Refraction about h; total internal reflection produces no direction.
  const float sin2_t = (1.0 - cos_oh * cos_oh) / (lobe.eta * lobe.eta);
  if (sin2_t >= 1.0) {
    return false;
  }
  sample.wi = -wo / lobe.eta + (cos_oh / lobe.eta - sqrt(1.0 - sin2_t)) * h;
  return sample.wi.z < 0.0;
}

// Draws a direction in the lobe's frame from u.xy, and from u.z where the
// lobe chooses between reflection and transmission. Returns false when no
// direction results.
public bool LobeSample(Lobe lobe, float3 wo, float3 u,
    out LobeSampleResult sample) {
  sample.wi = float3(0.0, 0.0, 1.0);
  sample.delta = false;
  sample.delta_weight = SpectrumConstant(0.0);
  if (!(wo.z > 0.0)) {
    return false;
  }
  switch (lobe.type) {
  case kLambertLobe:
    sample.wi = SampleCosineHemisphere(u.xy);
    return sample.wi.z > 0.0;
  case kGgxSchlickLobe: {
    const float3 h = SampleGgxVisibleNormal(wo, lobe.alpha, u.xy);
    sample.wi = 2.0 * dot(wo, h) * h - wo;
    return sample.wi.z > 0.0;
  }
  default:
    return SampleDielectric(lobe, wo, u, sample);
  }
}
