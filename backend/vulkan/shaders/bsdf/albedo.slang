// SPDX-License-Identifier: Apache-2.0
// Directional albedo tables (docs/design/LAYERED_BSDF.md section 5.6): the
// single-scattering albedos of lobes.slang's GGX lobes, which layering and
// multiple-scattering compensation need at a closure's outgoing direction.
// The data is generated from the lobes' own functions by lotus-bsdf-tables
// and read through IAlbedoTables, so the GPU (a storage buffer) and the CPU
// probe bind the same data the same way.
//
// Every table is a grid over coordinates in [0, 1], interpolated by
// Catmull-Rom splines along each axis: the albedos vary over widths that
// scale with alpha, the cosine and the index together, and a cubic reaches
// section 5.6's accuracy on grids a linear interpolation would need several
// times larger. The axes and their nodes are defined here only; the
// generator asks for the parameters at each node.
module albedo;

import bsdf.bsdf;

// The table data, one float per node.
public interface IAlbedoTables {
  float Fetch(uint index);
};

// The tables.
public static const uint kAlbedoGgx = 0;           // E1: a Fresnel of 1
public static const uint kAlbedoSchlickShare = 1;  // Eschlick: (1 - cos)^5
public static const uint kAlbedoDielectric = 2;    // Edielectric, reflected
public static const uint kAlbedoTableCount = 3;

// The dielectric table's sheets.
public static const uint kDielectricEntering = 0;
public static const uint kDielectricLeaving = 1;
public static const uint kDielectricThin = 2;
public static const uint kDielectricSheets = 3;

// Nodes per axis. The dielectric's cosine axis is longer: the leaving
// sheet's splits at the critical angle.
public static const uint kGgxCosines = 32;
public static const uint kDielectricCosines = 48;
public static const uint kAlbedoAlphas = 32;
public static const uint kAlbedoIndices = 16;

// The dielectric table's relative indices, eta_t / eta_i from the denser
// side's view; lookups outside them are clamped. An interface below the
// smallest reflects less than 3e-5 of the light at normal incidence.
public static const float kAlbedoMinIndex = 1.01;
public static const float kAlbedoMaxIndex = 3.0;

uint TableCosines(uint table) {
  return table == kAlbedoDielectric ? kDielectricCosines : kGgxCosines;
}

public uint AlbedoTableOffset(uint table) {
  const uint ggx = kGgxCosines * kAlbedoAlphas;
  if (table <= kAlbedoDielectric) {
    return table * ggx;
  }
  return 2 * ggx +
      kDielectricSheets * kAlbedoIndices * kDielectricCosines * kAlbedoAlphas;
}

// The number of floats the data holds.
public uint AlbedoTableSize() {
  return AlbedoTableOffset(kAlbedoTableCount);
}

// The number of nodes along each axis: cosine, alpha, and the dielectric's
// sheet and index together, in that order from fastest to slowest.
public uint3 AlbedoTableShape(uint table) {
  const uint layers = table == kAlbedoDielectric
      ? kDielectricSheets * kAlbedoIndices : 1;
  return uint3(TableCosines(table), kAlbedoAlphas, layers);
}

// --- Axes -----------------------------------------------------------------------

// The cosine, alpha and index axes use log(v) + k v, normalized: logarithmic
// where GGX's grazing features scale with alpha down to the alpha floor, and
// where grazing reflectance's features shrink with sqrt(eta - 1), and close
// to linear where Fresnel terms and wide lobes vary. A lobe has no frame at
// a cosine of 0, and the albedos tend to limits as the cosine falls, so the
// cosine axis starts at kMinCosine and a lookup below it takes its value.
static const float kMinCosine = 2.0e-4;
static const float kCosineSlope = 4.0;
static const float kAlphaSlope = 2.0;
static const float kIndexSlope = 0.5;

float LogLinearScale(float v, float slope) {
  return log(v) + slope * v;
}

// The coordinate of a value between `low` and `high`, in [0, 1].
float LogLinearCoordinate(float v, float low, float high, float slope) {
  const float clamped = clamp(v, low, high);
  return (LogLinearScale(clamped, slope) - LogLinearScale(low, slope)) /
      (LogLinearScale(high, slope) - LogLinearScale(low, slope));
}

// Its inverse, by Newton's method, which converges from below on the
// concave scale.
float LogLinearAt(float x, float low, float high, float slope) {
  const float target = lerp(LogLinearScale(low, slope),
      LogLinearScale(high, slope), saturate(x));
  float v = low;
  for (int i = 0; i < 60; ++i) {
    v = clamp(v - (LogLinearScale(v, slope) - target) / (1.0 / v + slope),
        low, high);
  }
  return v;
}

float CosineCoordinate(float cos_theta) {
  return LogLinearCoordinate(cos_theta, kMinCosine, 1.0, kCosineSlope);
}

float CosineAt(float x) {
  return LogLinearAt(x, kMinCosine, 1.0, kCosineSlope);
}

float AlphaCoordinate(float alpha) {
  return LogLinearCoordinate(alpha, kMinAlpha, 1.0, kAlphaSlope);
}

float AlphaAt(float x) {
  return LogLinearAt(x, kMinAlpha, 1.0, kAlphaSlope);
}

float IndexCoordinate(float index) {
  return LogLinearCoordinate(index - 1.0, kAlbedoMinIndex - 1.0,
      kAlbedoMaxIndex - 1.0, kIndexSlope);
}

float IndexAt(float x) {
  return 1.0 + LogLinearAt(x, kAlbedoMinIndex - 1.0, kAlbedoMaxIndex - 1.0,
      kIndexSlope);
}

// The leaving sheet's cosine axis keeps the critical angle on one node, so
// that no spline crosses the edge of total internal reflection: the nodes up
// to kCriticalNode cover cosines from kMinCosine to the critical cosine, and
// those above it the refracted cosine from kMinCosine to 1, logarithmic
// toward the critical angle, where a rough interface's albedo changes over
// a width of order alpha. Below the critical angle the coordinate is
// log(mu) - log(tau) + k mu, with tau = eta sqrt(mu_c^2 - mu^2) the
// evanescent cosine, logarithmic at both ends.
static const uint kCriticalNode = kDielectricCosines / 2;
static const float kCriticalCoordinate =
    float(kCriticalNode) / float(kDielectricCosines - 1);
static const float kMinEvanescent = 2.0e-3;

// `index` is the denser side's index relative to the outgoing direction's.
float CriticalCosine(float index) {
  return sqrt(max(1.0 - 1.0 / (index * index), 0.0));
}

float EvanescentScale(float mu, float index, float critical) {
  const float tau = max(index * sqrt(max(critical * critical - mu * mu, 0.0)),
      kMinEvanescent);
  return log(mu) - log(tau) + kCosineSlope * mu;
}

float LeavingCoordinate(float cos_theta, float index) {
  const float critical = CriticalCosine(index);
  if (cos_theta < critical) {
    const float low = EvanescentScale(kMinCosine, index, critical);
    const float high = EvanescentScale(critical, index, critical);
    return kCriticalCoordinate * saturate(
        (EvanescentScale(max(cos_theta, kMinCosine), index, critical) - low) /
        (high - low));
  }
  const float refracted =
      sqrt(max(1.0 - index * index * (1.0 - cos_theta * cos_theta), 0.0));
  return lerp(kCriticalCoordinate, 1.0, CosineCoordinate(refracted));
}

float LeavingAt(float x, float index) {
  const float critical = CriticalCosine(index);
  if (x <= kCriticalCoordinate) {
    // Bisection: the scale increases with the cosine.
    const float low = EvanescentScale(kMinCosine, index, critical);
    const float high = EvanescentScale(critical, index, critical);
    const float target = lerp(low, high, x / kCriticalCoordinate);
    float lo = kMinCosine;
    float hi = critical;
    for (int i = 0; i < 60; ++i) {
      const float mid = 0.5 * (lo + hi);
      if (EvanescentScale(mid, index, critical) < target) {
        lo = mid;
      } else {
        hi = mid;
      }
    }
    return 0.5 * (lo + hi);
  }
  const float refracted =
      CosineAt((x - kCriticalCoordinate) / (1.0 - kCriticalCoordinate));
  return sqrt(1.0 - (1.0 - refracted * refracted) / (index * index));
}

// The parameters at a point of a table's grid, its coordinates in [0, 1]:
// the outgoing direction's cosine, alpha, and the dielectric's relative
// index eta_t / eta_i from the outgoing direction's side, below 1 on the
// leaving sheet.
public float3 AlbedoTablePoint(uint table, float x_cosine, float x_alpha,
    uint sheet, float x_index) {
  if (table != kAlbedoDielectric) {
    return float3(CosineAt(x_cosine), AlphaAt(x_alpha), 1.0);
  }
  const float index = IndexAt(x_index);
  if (sheet == kDielectricLeaving) {
    return float3(LeavingAt(x_cosine, index), AlphaAt(x_alpha), 1.0 / index);
  }
  return float3(CosineAt(x_cosine), AlphaAt(x_alpha), index);
}

// A node's parameters. `layer` is sheet * kAlbedoIndices + index node.
public float3 AlbedoTableNode(uint table, uint cosine, uint alpha, uint layer) {
  return AlbedoTablePoint(table,
      float(cosine) / float(TableCosines(table) - 1),
      float(alpha) / float(kAlbedoAlphas - 1), layer / kAlbedoIndices,
      float(layer % kAlbedoIndices) / float(kAlbedoIndices - 1));
}

// Whether a node of the dielectric table is a thin wall's.
public bool AlbedoTableNodeThin(uint table, uint layer) {
  return table == kAlbedoDielectric && layer / kAlbedoIndices == kDielectricThin;
}

// --- Lookups ----------------------------------------------------------------------

// Catmull-Rom weights for the four nodes around a fraction t of a cell.
float4 CubicWeights(float t) {
  const float t2 = t * t;
  const float t3 = t2 * t;
  return float4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0,
      -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2);
}

// A node of a slice of `cosines` x kAlbedoAlphas nodes. Cosine nodes outside
// [first, last] and alpha nodes outside the axis are extrapolated linearly
// from the two inside it.
float FetchNode<T : IAlbedoTables>(T tables, uint base, uint cosines,
    int cosine, int alpha, int first, int last) {
  const int last_alpha = int(kAlbedoAlphas) - 1;
  const int c = clamp(cosine, first, last);
  const int a = clamp(alpha, 0, last_alpha);
  const int dc = cosine < first ? 1 : (cosine > last ? -1 : 0);
  const int da = alpha < 0 ? 1 : (alpha > last_alpha ? -1 : 0);
  const float at = tables.Fetch(base + uint(a * int(cosines) + c));
  if (dc == 0 && da == 0) {
    return at;
  }
  const float inner =
      tables.Fetch(base + uint((a + da) * int(cosines) + c + dc));
  return 2.0 * at - inner;
}

// A slice's value at coordinates in [0, 1]. With `split`, the cosine nodes
// below and above kCriticalNode are separate pieces, so no spline crosses
// the critical angle.
float FetchSlice<T : IAlbedoTables>(T tables, uint base, uint cosines,
    float x_cosine, float x_alpha, bool split) {
  const float fc = saturate(x_cosine) * float(cosines - 1);
  const float fa = saturate(x_alpha) * float(kAlbedoAlphas - 1);
  const int c0 = int(min(uint(fc), cosines - 2));
  const int a0 = int(min(uint(fa), kAlbedoAlphas - 2));
  int first = 0;
  int last = int(cosines) - 1;
  if (split) {
    if (c0 < int(kCriticalNode)) {
      last = int(kCriticalNode);
    } else {
      first = int(kCriticalNode);
    }
  }
  const float4 wc = CubicWeights(fc - float(c0));
  const float4 wa = CubicWeights(fa - float(a0));
  float value = 0.0;
  for (int j = 0; j < 4; ++j) {
    float row = 0.0;
    for (int i = 0; i < 4; ++i) {
      row += wc[i] * FetchNode(tables, base, cosines, c0 + i - 1, a0 + j - 1,
          first, last);
    }
    value += wa[j] * row;
  }
  return value;
}

// The single-scattering albedo of GGX reflection with a Fresnel of 1, at the
// outgoing cosine `cos_theta` and alpha.
public float AlbedoGgx<T : IAlbedoTables>(T tables, float cos_theta,
    float alpha) {
  return FetchSlice(tables, AlbedoTableOffset(kAlbedoGgx), kGgxCosines,
      CosineCoordinate(cos_theta), AlphaCoordinate(alpha), false);
}

// The albedo of GGX reflection with a Fresnel of (1 - cos_theta_h)^5:
// Schlick's f90 - f0 term per unit difference.
public float AlbedoSchlickShare<T : IAlbedoTables>(T tables, float cos_theta,
    float alpha) {
  return FetchSlice(tables, AlbedoTableOffset(kAlbedoSchlickShare),
      kGgxCosines, CosineCoordinate(cos_theta), AlphaCoordinate(alpha), false);
}

// A rough dielectric's reflected albedo, with relative index eta from the
// outgoing direction's side, total internal reflection included. A thin
// wall's index below 1 is looked up as its reciprocal.
public float AlbedoDielectric<T : IAlbedoTables>(T tables, float cos_theta,
    float alpha, float eta, bool thin_walled) {
  uint sheet = thin_walled ? kDielectricThin : kDielectricEntering;
  float index = eta;
  if (eta < 1.0) {
    index = 1.0 / eta;
    sheet = thin_walled ? kDielectricThin : kDielectricLeaving;
  }
  index = clamp(index, kAlbedoMinIndex, kAlbedoMaxIndex);
  const float fi = IndexCoordinate(index) * float(kAlbedoIndices - 1);
  const int i0 = int(min(uint(fi), kAlbedoIndices - 2));
  // Every index slice is read at the query's own cosine coordinate, which
  // on the leaving sheet follows its critical angle.
  const bool split = sheet == kDielectricLeaving;
  const float x_cosine = split ? LeavingCoordinate(cos_theta, index)
                               : CosineCoordinate(cos_theta);
  const float x_alpha = AlphaCoordinate(alpha);
  const uint slice_size = kDielectricCosines * kAlbedoAlphas;
  const uint base = AlbedoTableOffset(kAlbedoDielectric) +
      sheet * kAlbedoIndices * slice_size;
  // Catmull-Rom across index slices; a slice beyond an edge is
  // extrapolated linearly.
  float slices[4];
  for (int j = 0; j < 4; ++j) {
    const int slice = clamp(i0 + j - 1, 0, int(kAlbedoIndices) - 1);
    slices[j] = FetchSlice(tables, base + uint(slice) * slice_size,
        kDielectricCosines, x_cosine, x_alpha, split);
  }
  if (i0 == 0) {
    slices[0] = 2.0 * slices[1] - slices[2];
  }
  if (i0 == int(kAlbedoIndices) - 2) {
    slices[3] = 2.0 * slices[2] - slices[1];
  }
  const float4 w = CubicWeights(fi - float(i0));
  return w.x * slices[0] + w.y * slices[1] + w.z * slices[2] + w.w * slices[3];
}
