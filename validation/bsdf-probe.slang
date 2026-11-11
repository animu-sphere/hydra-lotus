// SPDX-License-Identifier: Apache-2.0
// The BSDF API's functions (docs/design/BSDF_API.md section 3), exported for
// lotus-bsdf-test: `slangc -target cpp` compiles this module and the real
// bsdf.slang it imports to C++, which the test runs on the CPU (section
// 7.1). Arguments are float arrays so the test names no generated type.
import bsdf.bsdf;
import common.spectrum;

// UsdPreviewSurface's closure inputs, as kMaterialFloats floats in the
// order of ProbeMaterial in bsdf-test.cpp.
BsdfClosure ProbeClosure(float* m) {
  return MakeBsdfClosure(MakePreviewClosure(
      SpectrumFromRgb(float3(m[0], m[1], m[2])), m[3], m[4], m[5],
      m[6] != 0.0, SpectrumFromRgb(float3(m[7], m[8], m[9])), m[10],
      SpectrumFromRgb(float3(m[11], m[12], m[13])), m[14] != 0.0,
      m[15] != 0.0));
}

float3 Load3(float* v) {
  return float3(v[0], v[1], v[2]);
}

void Store3(float3 value, float* v) {
  v[0] = value.x;
  v[1] = value.y;
  v[2] = value.z;
}

// result: BsdfEvaluate's RGB, then BsdfPdf.
__extern_cpp export void LotusProbeEvaluate(float* material, float* wo,
    float* wi, float* result) {
  const BsdfClosure closure = ProbeClosure(material);
  Store3(SpectrumToRgb(BsdfEvaluate(closure, Load3(wo), Load3(wi))), result);
  result[3] = BsdfPdf(closure, Load3(wo), Load3(wi));
}

// result: wi, weight, pdf, delta, transmitted. Returns 1 on success.
__extern_cpp export int LotusProbeSample(float* material, float* wo, float* u,
    float* result) {
  BsdfSampleResult sample;
  const bool success = BsdfSample(ProbeClosure(material), Load3(wo), Load3(u),
      sample);
  Store3(sample.wi, result);
  Store3(SpectrumToRgb(sample.weight), result + 3);
  result[6] = sample.pdf;
  result[7] = sample.delta ? 1.0 : 0.0;
  result[8] = sample.transmitted ? 1.0 : 0.0;
  return success ? 1 : 0;
}
