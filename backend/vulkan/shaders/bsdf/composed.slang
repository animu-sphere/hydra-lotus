// SPDX-License-Identifier: Apache-2.0
// Composed closures (docs/design/LAYERED_BSDF.md section 5): a flat list of
// lobes.slang's lobes, each with an RGB weight and an optional attenuation,
// which the mix, add, multiply and layer operations produce while a shading
// model builds the closure for its outgoing direction `wo`:
//
//   f(wo, wi) = sum_k W_k * A_k(wo, wi) * f_k(wo, wi)
//
// Everything that depends on wo alone (each lobe's albedo, compensation and
// selection probability, and the layers' factors) is computed while the
// closure is built, so a closure is evaluated, given a density and sampled
// only for the wo it was built for, and those functions read no table.
//
// Directions are in the shading frame of the BSDF API (docs/design/
// BSDF_API.md section 4); each lobe evaluates in the frame about its own
// normal (section 5.3).
module composed;

import bsdf.bsdf;
import bsdf.lobes;
import bsdf.albedo;
import common.spectrum;

// The largest list a model needs: OpenPBR's fuzz, coat, specular, metal,
// transmission and diffuse. LOTUS-MATERIALX-05 measures its register cost.
public static const uint kMaxLobes = 6;

// A lobe with a positive estimate is chosen at least this often, before
// renormalization (section 5.5).
static const float kSelectionFloor = 0.05;

// The absorption of a layer above a lobe (section 5.4): its transmittance
// T raised to the path length through it, 1 / mu_o + 1 / mu_i, with mu the
// cosines refracted into the layer about its normal. Symmetric in wo and
// wi, and at most 1.
public struct Attenuation {
  public bool active;
  public Spectrum transmittance;
  public float3 normal;
  // The layer's index relative to its surroundings.
  public float eta;
};

public struct ComposedClosure {
  public uint count;
  public Lobe lobes[kMaxLobes];
  public Spectrum weights[kMaxLobes];
  public Attenuation attenuations[kMaxLobes];
  // The lobe's albedo toward wo as a layer above it sees it: the energy it
  // reflects, compensation included, before a dielectric's tint (as
  // MaterialX's dielectric_bsdf leaves its tint out of a layer's
  // throughput). Zero where wo lies below the lobe's normal.
  public Spectrum albedos[kMaxLobes];
  // Its estimated contribution toward wo, tint included, every scatter
  // mode counted: the basis of its selection probability.
  public Spectrum estimates[kMaxLobes];
  // Set by ComposedFinalize.
  public float probabilities[kMaxLobes];
};

public ComposedClosure ComposedEmpty() {
  ComposedClosure closure;
  closure.count = 0;
  for (uint k = 0; k < kMaxLobes; ++k) {
    closure.weights[k] = SpectrumConstant(0.0);
    closure.albedos[k] = SpectrumConstant(0.0);
    closure.estimates[k] = SpectrumConstant(0.0);
    closure.probabilities[k] = 0.0;
    closure.attenuations[k].active = false;
    closure.attenuations[k].transmittance = SpectrumConstant(1.0);
    closure.attenuations[k].normal = float3(0.0, 0.0, 1.0);
    closure.attenuations[k].eta = 1.0;
  }
  return closure;
}

// The path length through an attenuating layer along w.
float PathLength(Attenuation attenuation, float3 w) {
  const float c = abs(dot(w, attenuation.normal));
  const float refracted2 =
      1.0 - (1.0 - c * c) / (attenuation.eta * attenuation.eta);
  return 1.0 / sqrt(max(refracted2, 1.0e-8));
}

public Spectrum AttenuationFactor(Attenuation attenuation, float3 wo,
    float3 wi) {
  if (!attenuation.active) {
    return SpectrumConstant(1.0);
  }
  return SpectrumPow(attenuation.transmittance,
      PathLength(attenuation, wo) + PathLength(attenuation, wi));
}

// Adds a lobe of weight 1 and returns its position. Computes its
// compensation, albedo and estimate for wo from the tables.
public uint ComposedAdd<T : IAlbedoTables>(inout ComposedClosure closure,
    Lobe added, float3 wo, T tables) {
  Lobe lobe = added;
  const uint k = closure.count;
  if (k >= kMaxLobes) {
    return k;
  }
  closure.count = k + 1;
  closure.weights[k] = SpectrumConstant(1.0);
  const float mu = dot(wo, lobe.normal);
  Spectrum albedo = SpectrumConstant(0.0);
  Spectrum estimate = SpectrumConstant(0.0);
  if (mu > 0.0) {
    switch (lobe.type) {
    case kLambertLobe:
      albedo = lobe.color;
      estimate = lobe.color;
      break;
    case kGgxSchlickLobe: {
      const float e1 = AlbedoGgx(tables, mu, lobe.alpha);
      const float share = AlbedoSchlickShare(tables, mu, lobe.alpha);
      if (lobe.compensate && e1 > 0.0) {
        lobe.compensation =
            SpectrumConstant(1.0) + lobe.color * ((1.0 - e1) / e1);
      }
      albedo = (lobe.color * (e1 - share) + lobe.f90 * share) *
          lobe.compensation;
      estimate = albedo;
      break;
    }
    default: {
      const bool reflects = (lobe.scatter & kScatterReflect) != 0;
      const bool transmits = (lobe.scatter & kScatterTransmit) != 0;
      float reflected = 0.0;
      if (lobe.smooth) {
        reflected = DielectricReflectance(lobe, mu);
      } else {
        reflected = AlbedoDielectric(tables, mu, lobe.alpha, lobe.eta,
            lobe.thin_walled);
        const float e1 = AlbedoGgx(tables, mu, lobe.alpha);
        if (lobe.compensate && e1 > 0.0) {
          const float f0 = DielectricReflectance(lobe, 1.0);
          lobe.compensation = SpectrumConstant(1.0 + f0 * (1.0 - e1) / e1);
        }
      }
      const Spectrum reflected_albedo = lobe.compensation * reflected;
      if (reflects) {
        albedo = reflected_albedo;
        estimate = reflected_albedo;
      }
      if (transmits) {
        estimate = estimate + SpectrumConstant(max(1.0 - reflected, 0.0));
      }
      estimate = estimate * lobe.color;
      break;
    }
    }
  }
  closure.lobes[k] = lobe;
  closure.albedos[k] = albedo;
  closure.estimates[k] = estimate;
  return k;
}

// MaterialX's multiply: scales the weights of lobes [begin, end).
public void ComposedScale(inout ComposedClosure closure, uint begin, uint end,
    Spectrum scale) {
  for (uint k = begin; k < min(end, closure.count); ++k) {
    closure.weights[k] = closure.weights[k] * scale;
  }
}

// MaterialX's mix, bg * (1 - m) + fg * m, of the background lobes
// [begin, split) and the foreground lobes [split, end). Concatenating two
// ranges is MaterialX's add.
public void ComposedMix(inout ComposedClosure closure, uint begin, uint split,
    uint end, float mix) {
  ComposedScale(closure, begin, split, SpectrumConstant(1.0 - mix));
  ComposedScale(closure, split, end, SpectrumConstant(mix));
}

// The total albedo toward wo of lobes [begin, end), as a layer above them
// sees it: sum_k W_k * A_k(wo, wo) * E_k(wo).
public Spectrum ComposedAlbedo(ComposedClosure closure, uint begin, uint end,
    float3 wo) {
  Spectrum albedo = SpectrumConstant(0.0);
  for (uint k = begin; k < min(end, closure.count); ++k) {
    albedo = albedo + closure.weights[k] *
        AttenuationFactor(closure.attenuations[k], wo, wo) * closure.albedos[k];
  }
  return albedo;
}

// MaterialX's layer of the top lobes [begin, split) over the base lobes
// [split, end) (section 4.1): every base weight is scaled by
// 1 - E_top(wo).
public void ComposedLayer(inout ComposedClosure closure, uint begin,
    uint split, uint end, float3 wo) {
  const Spectrum top = ComposedAlbedo(closure, begin, split, wo);
  ComposedScale(closure, split, end,
      SpectrumSaturate(SpectrumConstant(1.0) - top));
}

// Attaches a layer's absorption (section 5.4) to lobes [begin, end),
// replacing any they had: a lobe has at most one.
public void ComposedAttenuate(inout ComposedClosure closure, uint begin,
    uint end, Spectrum transmittance, float3 normal, float eta) {
  for (uint k = begin; k < min(end, closure.count); ++k) {
    closure.attenuations[k].active = true;
    closure.attenuations[k].transmittance = SpectrumSaturate(transmittance);
    closure.attenuations[k].normal = normal;
    closure.attenuations[k].eta = eta;
  }
}

// Sets the selection probabilities once the closure is complete (section
// 5.5): proportional to each lobe's estimated contribution toward wo, at
// least the floor for every lobe with a positive estimate, renormalized.
public void ComposedFinalize(inout ComposedClosure closure, float3 wo) {
  float total = 0.0;
  for (uint k = 0; k < closure.count; ++k) {
    const float estimate = MaxComponent(closure.weights[k] *
        AttenuationFactor(closure.attenuations[k], wo, wo) *
        closure.estimates[k]);
    closure.probabilities[k] = estimate > 0.0 ? estimate : 0.0;
    total += closure.probabilities[k];
  }
  if (!(total > 0.0)) {
    return;
  }
  float floored = 0.0;
  for (uint k = 0; k < closure.count; ++k) {
    if (closure.probabilities[k] > 0.0) {
      closure.probabilities[k] =
          max(closure.probabilities[k] / total, kSelectionFloor);
      floored += closure.probabilities[k];
    }
  }
  for (uint k = 0; k < closure.count; ++k) {
    closure.probabilities[k] /= floored;
  }
}

// f(wo, wi) * |cos(theta_i)| of the non-delta lobes, each about its normal.
public Spectrum ComposedEvaluate(ComposedClosure closure, float3 wo,
    float3 wi) {
  Spectrum value = SpectrumConstant(0.0);
  for (uint k = 0; k < closure.count; ++k) {
    const Lobe lobe = closure.lobes[k];
    if (LobeIsDelta(lobe)) {
      continue;
    }
    const LobeFrame frame = MakeLobeFrame(lobe.normal);
    value = value + closure.weights[k] *
        AttenuationFactor(closure.attenuations[k], wo, wi) *
        LobeValue(lobe, ToLobe(frame, wo), ToLobe(frame, wi));
  }
  return value;
}

// The density with which ComposedSample returns wi as a non-delta sample:
// sum_k p_k * pdf_k(wo, wi).
public float ComposedPdf(ComposedClosure closure, float3 wo, float3 wi) {
  float pdf = 0.0;
  for (uint k = 0; k < closure.count; ++k) {
    const Lobe lobe = closure.lobes[k];
    if (LobeIsDelta(lobe) || !(closure.probabilities[k] > 0.0)) {
      continue;
    }
    const LobeFrame frame = MakeLobeFrame(lobe.normal);
    pdf += closure.probabilities[k] *
        LobePdf(lobe, ToLobe(frame, wo), ToLobe(frame, wi));
  }
  return pdf;
}

// One choice among the lobes with u.z, then the chosen lobe's direction
// from u.xy and u.z rescaled (section 5.5). A non-delta sample's weight is
// the mixture ratio; a delta sample's is the chosen lobe's delta weight
// times its weight and attenuation, over its selection probability.
public bool ComposedSample(ComposedClosure closure, float3 wo, float3 u,
    out BsdfSampleResult sample) {
  sample.wi = float3(0.0, 0.0, 1.0);
  sample.weight = SpectrumConstant(0.0);
  sample.pdf = 0.0;
  sample.delta = false;
  sample.transmitted = false;
  // Rounding can leave u.z above the last cumulative probability; the last
  // candidate takes it.
  uint chosen = kMaxLobes;
  float start = 0.0;
  float end = 0.0;
  for (uint k = 0; k < closure.count; ++k) {
    const float p = closure.probabilities[k];
    if (!(p > 0.0)) {
      continue;
    }
    chosen = k;
    start = end;
    end += p;
    if (u.z < end) {
      break;
    }
  }
  if (chosen == kMaxLobes) {
    return false;
  }
  const float probability = closure.probabilities[chosen];
  u.z = clamp((u.z - start) / probability, 0.0, 0x1.fffffep-1);
  const Lobe lobe = closure.lobes[chosen];
  const LobeFrame frame = MakeLobeFrame(lobe.normal);
  LobeSampleResult local;
  if (!LobeSample(lobe, ToLobe(frame, wo), u, local)) {
    return false;
  }
  sample.wi = FromLobe(frame, local.wi);
  sample.transmitted = sample.wi.z < 0.0;
  if (local.delta) {
    sample.delta = true;
    sample.weight = closure.weights[chosen] *
        AttenuationFactor(closure.attenuations[chosen], wo, sample.wi) *
        local.delta_weight / probability;
    return IsFinite(sample.weight);
  }
  sample.pdf = ComposedPdf(closure, wo, sample.wi);
  if (!(sample.pdf > 0.0)) {
    return false;
  }
  sample.weight = ComposedEvaluate(closure, wo, sample.wi) / sample.pdf;
  return IsFinite(sample.weight);
}
