// SPDX-License-Identifier: Apache-2.0
// The BSDF API (docs/design/BSDF_API.md): the closure a shading model builds
// at a hit, and the three functions every integrator calls on it,
// BsdfEvaluate, BsdfPdf and BsdfSample. Directions are in the shading frame,
// z along the shading normal facing the outgoing direction `wo`, and point
// away from the surface; wi.z > 0 is reflection and wi.z < 0 transmission.
// Every function is in radiance mode.
//
// A closure has one of two representations (docs/design/GLTF_PBR.md section
// 6.2): UsdPreviewSurface's own (preview.slang), or the composed closure
// (composed.slang) that every MaterialX model builds from shared lobes.
module bsdf;

__exported import bsdf.preview;
__exported import bsdf.albedo;
__exported import bsdf.composed;
__exported import bsdf.gltf_pbr;
import common.spectrum;

// The closure's model tag: the hit's ShadingModel (gpu_scene.hpp).
public static const uint kPreviewSurfaceClosure = 0;
public static const uint kGltfPbrClosure = 1;

// The closure of a hit, whatever its shading model: the model tag and the
// representation that model uses. The functions below dispatch on the tag.
public struct BsdfClosure {
  public uint model;
  public PreviewClosure preview;
  public ComposedClosure composed;
};

public BsdfClosure MakeBsdfClosure(PreviewClosure preview) {
  BsdfClosure closure;
  closure.model = kPreviewSurfaceClosure;
  closure.preview = preview;
  closure.composed = ComposedEmpty();
  return closure;
}

// A MaterialX model's closure. Its mixture-ratio weights and never-delta
// GGX lobes are the composed closure's (docs/design/LAYERED_BSDF.md section
// 5.5).
public BsdfClosure MakeBsdfClosure(uint model, ComposedClosure composed) {
  BsdfClosure closure;
  closure.model = model;
  closure.preview = MakePreviewClosure(SpectrumConstant(0.0), 1.0, 0.0, 1.5,
      false, SpectrumConstant(0.0), 0.0, SpectrumConstant(1.0), false, true);
  closure.composed = composed;
  return closure;
}

// f(wo, wi) * |cos(theta_i)| of the closure's non-delta components, over
// the whole sphere (BSDF_API.md section 5.1).
public Spectrum BsdfEvaluate(BsdfClosure closure, float3 wo, float3 wi) {
  switch (closure.model) {
  case kGltfPbrClosure:
    return ComposedEvaluate(closure.composed, wo, wi);
  case kPreviewSurfaceClosure:
  default:
    return PreviewEvaluate(closure.preview, wo, wi);
  }
}

// The solid-angle density with which BsdfSample returns `wi` as a non-delta
// sample, every selection probability included (BSDF_API.md section 5.2).
// Its integral over the sphere is the probability that a sample is
// non-delta and succeeds.
public float BsdfPdf(BsdfClosure closure, float3 wo, float3 wi) {
  switch (closure.model) {
  case kGltfPbrClosure:
    return ComposedPdf(closure.composed, wo, wi);
  case kPreviewSurfaceClosure:
  default:
    return PreviewPdf(closure.preview, wo, wi);
  }
}

// Samples a direction from three uniform numbers in [0, 1) (BSDF_API.md
// section 5.3). Returns false when the procedure produces no direction:
// the path ends, its energy absorbed.
public bool BsdfSample(BsdfClosure closure, float3 wo, float3 u,
    out BsdfSampleResult sample) {
  switch (closure.model) {
  case kGltfPbrClosure:
    return ComposedSample(closure.composed, wo, u, sample);
  case kPreviewSurfaceClosure:
  default:
    return PreviewSample(closure.preview, wo, u, sample);
  }
}
