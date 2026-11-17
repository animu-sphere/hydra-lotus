// SPDX-License-Identifier: Apache-2.0
// MaterialX's glTF PBR surface (docs/design/GLTF_PBR.md section 6.1 and
// GLTF_PBR_EXTENSIONS.md section 6.1): the closure
// ND_gltf_pbr_surfaceshader 2.0.1's graph reduces to with iridescence and
// anisotropy at their defaults,
//
//   layer(clearcoat,
//     layer(sheen,
//       mix(layer(generalized_schlick(f0, f90),
//                 mix(oren_nayar_diffuse(base_color), transmission,
//                     transmission)),
//           generalized_schlick(base_color, 1), metallic)))
//
// built from the composed closure's lobes and operations. The GGX lobes are
// compensated for multiple scattering; the three of the base share alpha and
// the shading normal, so one pair of table values serves them. Transmission
// is glTF's thin surface: the reflection lobe mirrored through the tangent
// plane, without a Fresnel term of its own (section 6.2). A lobe whose
// weight is 0 is not added, so a material with the extensions at their
// defaults builds the core's three lobes with the core's weights.
module gltf_pbr;

import bsdf.preview;
import bsdf.lobes;
import bsdf.albedo;
import bsdf.composed;
import common.spectrum;

// The clearcoat's index, a constant of the definition's graph.
public static const float kGltfClearcoatIndex = 1.5;

// The model's inputs at a hit, as the scene pass evaluates them.
public struct GltfPbrInputs {
  public Spectrum base_color;
  public float roughness;
  public float metallic;
  // The dielectric's reflectance at normal incidence,
  // ((1 - ior) / (1 + ior))^2, which the host computes from the uniform ior.
  public float dielectric_f0;
  public float specular;
  public Spectrum specular_color;
  public float transmission;
  public float clearcoat;
  public float clearcoat_roughness;
  // The coat's unit normal in the shading frame.
  public float3 clearcoat_normal;
  public Spectrum sheen_color;
  public float sheen_roughness;
};

// The core's inputs with every extension at the definition's default.
public GltfPbrInputs MakeGltfPbrInputs(Spectrum base_color, float roughness,
    float metallic) {
  GltfPbrInputs inputs;
  inputs.base_color = base_color;
  inputs.roughness = roughness;
  inputs.metallic = metallic;
  inputs.dielectric_f0 = 0.04;
  inputs.specular = 1.0;
  inputs.specular_color = SpectrumConstant(1.0);
  inputs.transmission = 0.0;
  inputs.clearcoat = 0.0;
  inputs.clearcoat_roughness = 0.0;
  inputs.clearcoat_normal = float3(0.0, 0.0, 1.0);
  inputs.sheen_color = SpectrumConstant(0.0);
  inputs.sheen_roughness = 0.0;
  return inputs;
}

// What a glTF PBR closure is built from: the model's inputs at the hit,
// reduced to what its lobes read, and every table value at its outgoing
// direction. A stored reservoir surface keeps exactly these, kGltfPbrLanes
// floats (GLTF_PBR_EXTENSIONS.md section 6.7), so a closure is rebuilt from
// them without a table.
public struct GltfPbrTerms {
  public Spectrum base_color;
  // GGX alpha of the base's lobes: roughness squared, at least kMinAlpha.
  public float alpha;
  public float metallic;
  // AlbedoGgx and AlbedoSchlickShare at alpha and wo.z; 0 for wo.z <= 0.
  public float e1;
  public float share;
  // The dielectric reflection's Schlick colours:
  // min(specular_color * dielectric_f0, 1) * specular, and specular.
  public Spectrum f0;
  public float f90;
  public float transmission;
  // The coat's weight times its compensation, and that times its reflected
  // albedo toward wo, which is what it takes from the layers below; 0 and 0
  // without a coat or where wo lies below its normal.
  public float coat_scale;
  public float coat_dim;
  public float coat_alpha;
  // Unit, in the shading frame, in its upper hemisphere.
  public float3 coat_normal;
  // The sheen's colour, normalized where the lobe's albedo exceeds 1, its
  // roughness, and its weight times its albedo toward wo; 0 without a
  // sheen.
  public Spectrum sheen_color;
  public float sheen_roughness;
  public float sheen_dim;
};

public GltfPbrTerms MakeGltfPbrTerms<T : IAlbedoTables>(GltfPbrInputs inputs,
    float3 wo, T tables) {
  GltfPbrTerms terms;
  terms.base_color = inputs.base_color;
  terms.alpha = max(inputs.roughness * inputs.roughness, kMinAlpha);
  terms.metallic = inputs.metallic;
  terms.e1 = 0.0;
  terms.share = 0.0;
  if (wo.z > 0.0) {
    terms.e1 = AlbedoGgx(tables, wo.z, terms.alpha);
    terms.share = AlbedoSchlickShare(tables, wo.z, terms.alpha);
  }
  terms.f0 = SpectrumSaturate(inputs.specular_color * inputs.dielectric_f0) *
      inputs.specular;
  terms.f90 = inputs.specular;
  terms.transmission = inputs.transmission;

  terms.coat_scale = 0.0;
  terms.coat_dim = 0.0;
  terms.coat_alpha = max(
      inputs.clearcoat_roughness * inputs.clearcoat_roughness, kMinAlpha);
  // A coat normal in the shading frame's lower hemisphere is not stored
  // (section 6.7): the coat then lies about the base normal.
  terms.coat_normal = inputs.clearcoat_normal.z > 0.0
      ? inputs.clearcoat_normal : float3(0.0, 0.0, 1.0);
  const float coat_cos = dot(wo, terms.coat_normal);
  if (inputs.clearcoat > 0.0 && coat_cos > 0.0) {
    const float reflected = AlbedoDielectric(tables, coat_cos,
        terms.coat_alpha, kGltfClearcoatIndex, false);
    const float coat_e1 = AlbedoGgx(tables, coat_cos, terms.coat_alpha);
    const float coat_root =
        (1.0 - kGltfClearcoatIndex) / (1.0 + kGltfClearcoatIndex);
    float compensation = 1.0;
    if (coat_e1 > 0.0) {
      compensation += coat_root * coat_root * (1.0 - coat_e1) / coat_e1;
    }
    terms.coat_scale = inputs.clearcoat * compensation;
    terms.coat_dim = saturate(terms.coat_scale * reflected);
  }

  terms.sheen_color = inputs.sheen_color;
  terms.sheen_roughness =
      max(inputs.sheen_roughness * inputs.sheen_roughness, kMinSheenRoughness);
  terms.sheen_dim = 0.0;
  const float sheen = MaxComponent(inputs.sheen_color);
  if (sheen > 0.0 && wo.z > 0.0) {
    const float e = AlbedoSheen(tables, wo.z, terms.sheen_roughness);
    if (e > 1.0) {
      terms.sheen_color = inputs.sheen_color * (1.0 / e);
    }
    terms.sheen_dim = saturate(sheen * min(e, 1.0));
  }
  return terms;
}

// The number of floats a stored surface keeps of the terms.
public static const uint kGltfPbrLanes = 22;

// The terms as a stored reservoir surface keeps them, and back. The coat's
// normal is stored by its tangent components.
public void PackGltfPbrTerms(GltfPbrTerms terms, out float lanes[kGltfPbrLanes]) {
  const float3 base = SpectrumToRgb(terms.base_color);
  const float3 f0 = SpectrumToRgb(terms.f0);
  const float3 sheen = SpectrumToRgb(terms.sheen_color);
  lanes[0] = base.x;
  lanes[1] = base.y;
  lanes[2] = base.z;
  lanes[3] = terms.alpha;
  lanes[4] = terms.metallic;
  lanes[5] = terms.e1;
  lanes[6] = terms.share;
  lanes[7] = f0.x;
  lanes[8] = f0.y;
  lanes[9] = f0.z;
  lanes[10] = terms.f90;
  lanes[11] = terms.transmission;
  lanes[12] = terms.coat_scale;
  lanes[13] = terms.coat_dim;
  lanes[14] = terms.coat_alpha;
  lanes[15] = terms.coat_normal.x;
  lanes[16] = terms.coat_normal.y;
  lanes[17] = sheen.x;
  lanes[18] = sheen.y;
  lanes[19] = sheen.z;
  lanes[20] = terms.sheen_roughness;
  lanes[21] = terms.sheen_dim;
}

public GltfPbrTerms UnpackGltfPbrTerms(float lanes[kGltfPbrLanes]) {
  GltfPbrTerms terms;
  terms.base_color = SpectrumFromRgb(float3(lanes[0], lanes[1], lanes[2]));
  terms.alpha = lanes[3];
  terms.metallic = lanes[4];
  terms.e1 = lanes[5];
  terms.share = lanes[6];
  terms.f0 = SpectrumFromRgb(float3(lanes[7], lanes[8], lanes[9]));
  terms.f90 = lanes[10];
  terms.transmission = lanes[11];
  terms.coat_scale = lanes[12];
  terms.coat_dim = lanes[13];
  terms.coat_alpha = lanes[14];
  const float2 tilt = float2(lanes[15], lanes[16]);
  terms.coat_normal =
      float3(tilt, sqrt(max(1.0 - dot(tilt, tilt), 0.0)));
  terms.sheen_color = SpectrumFromRgb(float3(lanes[17], lanes[18], lanes[19]));
  terms.sheen_roughness = lanes[20];
  terms.sheen_dim = lanes[21];
  return terms;
}

// The closure for the outgoing direction the terms were made for: the coat,
// the sheen, the dielectric's reflection, the diffuse and transmitted base
// under it, and the metal.
public ComposedClosure MakeGltfPbrClosure(GltfPbrTerms terms, float3 wo) {
  const float3 normal = float3(0.0, 0.0, 1.0);
  const Spectrum white = SpectrumConstant(1.0);
  ComposedBuilder builder = ComposedBegin();

  const uint coat = builder.closure.count;
  if (terms.coat_scale > 0.0) {
    ComposedAddWeighted(builder,
        MakeGgxDielectricLobe(kGltfClearcoatIndex, white, terms.coat_alpha,
            kScatterReflect, false, terms.coat_normal, false),
        SpectrumConstant(terms.coat_scale),
        SpectrumConstant(terms.coat_dim / terms.coat_scale));
  }
  const uint sheen = builder.closure.count;
  if (terms.sheen_dim > 0.0) {
    // The lobe carries the colour; its largest channel is the sheen's
    // weight, by which the layer below is dimmed.
    ComposedAddWeighted(builder,
        MakeSheenLobe(white, terms.sheen_roughness, normal),
        terms.sheen_color,
        SpectrumConstant(terms.sheen_dim / MaxComponent(terms.sheen_color)));
  }

  const bool dielectric = terms.metallic < 1.0;
  const uint top = builder.closure.count;
  if (dielectric && terms.f90 > 0.0) {
    ComposedAddSchlick(builder,
        MakeGgxSchlickLobe(terms.f0, SpectrumConstant(terms.f90), terms.alpha,
            normal, true),
        wo, terms.e1, terms.share);
  }
  const uint diffuse = builder.closure.count;
  if (dielectric && terms.transmission < 1.0) {
    ComposedAddLambert(builder, MakeLambertLobe(terms.base_color, normal), wo);
  }
  const uint transmitted = builder.closure.count;
  if (dielectric && terms.transmission > 0.0) {
    Lobe lobe = MakeGgxSchlickLobe(white, white, terms.alpha, normal, true);
    lobe.transmit = true;
    const uint k = ComposedAddSchlick(builder, lobe, wo, terms.e1, terms.share);
    ComposedScale(builder, k, k + 1, terms.base_color);
  }
  const uint metal = builder.closure.count;
  if (terms.transmission > 0.0) {
    ComposedMix(builder, diffuse, transmitted, metal, terms.transmission);
  }
  ComposedLayerScalar(builder, top, diffuse, metal, wo);
  if (terms.metallic > 0.0) {
    ComposedAddSchlick(builder,
        MakeGgxSchlickLobe(terms.base_color, white, terms.alpha, normal, true),
        wo, terms.e1, terms.share);
  }
  const uint end = builder.closure.count;
  ComposedMix(builder, top, metal, end, terms.metallic);
  // The sheen dims what lies below by its weight times its albedo,
  // uncoloured.
  ComposedLayerScalar(builder, sheen, top, end, wo);
  ComposedLayer(builder, coat, sheen, end, wo);
  return ComposedFinalize(builder, wo);
}
