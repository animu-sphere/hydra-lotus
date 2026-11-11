// SPDX-License-Identifier: Apache-2.0
// The BSDF API's functions (docs/design/BSDF_API.md section 3) and the
// composed closures' (docs/design/LAYERED_BSDF.md section 7), exported for
// lotus-bsdf-test and lotus-bsdf-tables: `slangc -target cpp` compiles this
// module and the real shader modules it imports to C++, which they run on
// the CPU (BSDF_API.md section 7.1). Arguments are float arrays so the C++
// names no generated type.
import bsdf.bsdf;
import bsdf.albedo;
import bsdf.composed;
import bsdf.lobes;
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

// --- Composed closures (docs/design/LAYERED_BSDF.md section 7) ---------------

// The albedo tables as bsdf-test.cpp and lotus-bsdf-tables bind them.
struct ProbeTables : IAlbedoTables {
  float* data;
  float Fetch(uint index) {
    return data[index];
  }
};

// A composed closure is described by a program of floats, read in order;
// each lobe pushes its range on a stack and each operation combines the
// ranges on top of it, so the lobes stay contiguous. Operations, with their
// operands (normals are unit, in the shading frame):
static const int kOpEnd = 0;
static const int kOpLambert = 1;     // colour rgb, normal xyz
static const int kOpSchlick = 2;     // f0 rgb, f90 rgb, alpha, normal xyz, compensate
static const int kOpDielectric = 3;  // eta, tint rgb, alpha, scatter, thin, normal xyz, compensate
static const int kOpMix = 4;         // m: pops fg, then bg
static const int kOpLayer = 5;       // pops base, then top
static const int kOpScale = 6;       // rgb: the range on top
static const int kOpAttenuate = 7;   // transmittance rgb, normal xyz, eta: the range on top
static const int kOpAdd = 8;         // pops two ranges

ComposedClosure ProbeComposed(float* program, float* table_data, float3 wo) {
  ProbeTables tables;
  tables.data = table_data;
  ComposedClosure closure = ComposedEmpty();
  uint2 stack[8];
  uint depth = 0;
  int at = 0;
  for (int step = 0; step < 64; ++step) {
    const int op = int(program[at]);
    if (op == kOpLambert) {
      const uint k = ComposedAdd(closure, MakeLambertLobe(
          SpectrumFromRgb(Load3(program + at + 1)), Load3(program + at + 4)),
          wo, tables);
      stack[depth++] = uint2(k, k + 1);
      at += 7;
    } else if (op == kOpSchlick) {
      const uint k = ComposedAdd(closure, MakeGgxSchlickLobe(
          SpectrumFromRgb(Load3(program + at + 1)),
          SpectrumFromRgb(Load3(program + at + 4)), program[at + 7],
          Load3(program + at + 8), program[at + 11] != 0.0), wo, tables);
      stack[depth++] = uint2(k, k + 1);
      at += 12;
    } else if (op == kOpDielectric) {
      const uint k = ComposedAdd(closure, MakeGgxDielectricLobe(
          program[at + 1], SpectrumFromRgb(Load3(program + at + 2)),
          program[at + 5], uint(program[at + 6]), program[at + 7] != 0.0,
          Load3(program + at + 8), program[at + 11] != 0.0), wo, tables);
      stack[depth++] = uint2(k, k + 1);
      at += 12;
    } else if (op == kOpMix) {
      const uint2 fg = stack[--depth];
      const uint2 bg = stack[--depth];
      ComposedMix(closure, bg.x, fg.x, fg.y, program[at + 1]);
      stack[depth++] = uint2(bg.x, fg.y);
      at += 2;
    } else if (op == kOpLayer) {
      const uint2 base = stack[--depth];
      const uint2 top = stack[--depth];
      ComposedLayer(closure, top.x, base.x, base.y, wo);
      stack[depth++] = uint2(top.x, base.y);
      at += 1;
    } else if (op == kOpScale) {
      const uint2 range = stack[depth - 1];
      ComposedScale(closure, range.x, range.y,
          SpectrumFromRgb(Load3(program + at + 1)));
      at += 4;
    } else if (op == kOpAttenuate) {
      const uint2 range = stack[depth - 1];
      ComposedAttenuate(closure, range.x, range.y,
          SpectrumFromRgb(Load3(program + at + 1)), Load3(program + at + 4),
          program[at + 7]);
      at += 8;
    } else if (op == kOpAdd) {
      const uint2 second = stack[--depth];
      const uint2 first = stack[--depth];
      stack[depth++] = uint2(first.x, second.y);
      at += 1;
    } else {
      break;
    }
  }
  ComposedFinalize(closure, wo);
  return closure;
}

// result: ComposedEvaluate's RGB, then ComposedPdf.
__extern_cpp export void LotusProbeComposedEvaluate(float* program,
    float* tables, float* wo, float* wi, float* result) {
  const ComposedClosure closure = ProbeComposed(program, tables, Load3(wo));
  Store3(SpectrumToRgb(ComposedEvaluate(closure, Load3(wo), Load3(wi))),
      result);
  result[3] = ComposedPdf(closure, Load3(wo), Load3(wi));
}

// result: as LotusProbeSample's. Returns 1 on success.
__extern_cpp export int LotusProbeComposedSample(float* program, float* tables,
    float* wo, float* u, float* result) {
  BsdfSampleResult sample;
  const bool success = ComposedSample(
      ProbeComposed(program, tables, Load3(wo)), Load3(wo), Load3(u), sample);
  Store3(sample.wi, result);
  Store3(SpectrumToRgb(sample.weight), result + 3);
  result[6] = sample.pdf;
  result[7] = sample.delta ? 1.0 : 0.0;
  result[8] = sample.transmitted ? 1.0 : 0.0;
  return success ? 1 : 0;
}

// result: the whole closure's albedo toward wo as a layer above it sees it.
__extern_cpp export void LotusProbeComposedAlbedo(float* program,
    float* tables, float* wo, float* result) {
  const ComposedClosure closure = ProbeComposed(program, tables, Load3(wo));
  Store3(SpectrumToRgb(ComposedAlbedo(closure, 0, closure.count, Load3(wo))),
      result);
}

// result: the data's size, then each table's offset and node counts.
__extern_cpp export void LotusProbeAlbedoLayout(int* result) {
  result[0] = int(AlbedoTableSize());
  for (uint table = 0; table < kAlbedoTableCount; ++table) {
    const uint3 shape = AlbedoTableShape(table);
    result[1 + 4 * table] = int(AlbedoTableOffset(table));
    result[2 + 4 * table] = int(shape.x);
    result[3 + 4 * table] = int(shape.y);
    result[4 + 4 * table] = int(shape.z);
  }
}

// result: a node's cosine, alpha, relative index and thin-wall flag.
__extern_cpp export void LotusProbeAlbedoNode(int table, int cosine, int alpha,
    int layer, float* result) {
  Store3(AlbedoTableNode(uint(table), uint(cosine), uint(alpha), uint(layer)),
      result);
  result[3] = AlbedoTableNodeThin(uint(table), uint(layer)) ? 1.0 : 0.0;
}

// result: the parameters at a point of a table's grid, as for a node.
__extern_cpp export void LotusProbeAlbedoPoint(int table, float x_cosine,
    float x_alpha, int sheet, float x_index, float* result) {
  Store3(AlbedoTablePoint(uint(table), x_cosine, x_alpha, uint(sheet), x_index),
      result);
  result[3] = table == int(kAlbedoDielectric) && sheet == int(kDielectricThin)
      ? 1.0 : 0.0;
}

// A table's interpolated value.
__extern_cpp export float LotusProbeAlbedoLookup(float* table_data, int table,
    float cos_theta, float alpha, float eta, int thin) {
  ProbeTables tables;
  tables.data = table_data;
  if (table == int(kAlbedoGgx)) {
    return AlbedoGgx(tables, cos_theta, alpha);
  }
  if (table == int(kAlbedoSchlickShare)) {
    return AlbedoSchlickShare(tables, cos_theta, alpha);
  }
  return AlbedoDielectric(tables, cos_theta, alpha, eta, thin != 0);
}
