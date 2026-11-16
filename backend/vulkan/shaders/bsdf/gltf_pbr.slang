// SPDX-License-Identifier: Apache-2.0
// MaterialX's glTF PBR surface, its core (docs/design/GLTF_PBR.md section
// 6.1): the closure ND_gltf_pbr_surfaceshader 2.0.1's graph reduces to with
// its extensions at their defaults,
//
//   mix(layer(generalized_schlick(0.04, 1), oren_nayar_diffuse(base_color)),
//       generalized_schlick(base_color, 1), metallic)
//
// built from the composed closure's lobes and operations. Both reflection
// lobes are compensated for multiple scattering, and share alpha and the
// shading normal, so one pair of table values serves both.
module gltf_pbr;

import bsdf.preview;
import bsdf.lobes;
import bsdf.albedo;
import bsdf.composed;
import common.spectrum;

// The dielectric's reflectance at normal incidence at the definition's
// default index, ((1 - 1.5) / (1 + 1.5))^2.
public static const float kGltfDielectricF0 = 0.04;

// What a glTF PBR closure is built from: the model's inputs at the hit and
// the two table values at its outgoing direction. A stored reservoir surface
// keeps exactly these (section 6.4).
public struct GltfPbrTerms {
  public Spectrum base_color;
  // GGX alpha: roughness squared, at least kMinAlpha.
  public float alpha;
  public float metallic;
  // AlbedoGgx and AlbedoSchlickShare at alpha and wo.z; 0 for wo.z <= 0.
  public float e1;
  public float share;
};

public GltfPbrTerms MakeGltfPbrTerms<T : IAlbedoTables>(Spectrum base_color,
    float roughness, float metallic, float3 wo, T tables) {
  GltfPbrTerms terms;
  terms.base_color = base_color;
  terms.alpha = max(roughness * roughness, kMinAlpha);
  terms.metallic = metallic;
  terms.e1 = 0.0;
  terms.share = 0.0;
  if (wo.z > 0.0) {
    terms.e1 = AlbedoGgx(tables, wo.z, terms.alpha);
    terms.share = AlbedoSchlickShare(tables, wo.z, terms.alpha);
  }
  return terms;
}

// The terms a closure MakeGltfPbrClosure built for `wo` came from, so that
// a stored surface can keep them: its inputs are read back from its lobes
// and weights, and the table values looked up again.
public GltfPbrTerms GltfPbrTermsOf<T : IAlbedoTables>(ComposedClosure closure,
    float3 wo, T tables) {
  GltfPbrTerms terms;
  terms.base_color = closure.lobes[1].color;
  terms.alpha = closure.lobes[0].alpha;
  terms.metallic = SpectrumToRgb(closure.weights[2]).x;
  terms.e1 = 0.0;
  terms.share = 0.0;
  if (wo.z > 0.0) {
    terms.e1 = AlbedoGgx(tables, wo.z, terms.alpha);
    terms.share = AlbedoSchlickShare(tables, wo.z, terms.alpha);
  }
  return terms;
}

// The closure for the outgoing direction the terms were made for: the
// dielectric's reflection, the diffuse base under it, and the metal.
public ComposedClosure MakeGltfPbrClosure(GltfPbrTerms terms, float3 wo) {
  const float3 normal = float3(0.0, 0.0, 1.0);
  const Spectrum white = SpectrumConstant(1.0);
  ComposedBuilder builder = ComposedBegin();
  const uint top = ComposedAddSchlick(builder,
      MakeGgxSchlickLobe(SpectrumConstant(kGltfDielectricF0), white,
          terms.alpha, normal, true),
      wo, terms.e1, terms.share);
  const uint base = ComposedAddLambert(builder,
      MakeLambertLobe(terms.base_color, normal), wo);
  ComposedLayer(builder, top, base, base + 1, wo);
  const uint metal = ComposedAddSchlick(builder,
      MakeGgxSchlickLobe(terms.base_color, white, terms.alpha, normal, true),
      wo, terms.e1, terms.share);
  ComposedMix(builder, top, metal, metal + 1, terms.metallic);
  return ComposedFinalize(builder, wo);
}
