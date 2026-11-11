// SPDX-License-Identifier: Apache-2.0
// What lotus-bsdf-tables and lotus-bsdf-test share about the albedo tables
// (docs/design/LAYERED_BSDF.md section 5.6): the probe's composed-closure
// exports, the tables' layout and nodes as albedo.slang defines them, the
// programs that describe composed closures to the probe, and the quadrature
// that generates a node's value.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

extern "C" {
void LotusProbeComposedEvaluate(float* program, float* tables, float* wo,
    float* wi, float* result);
std::int32_t LotusProbeComposedSample(float* program, float* tables, float* wo,
    float* u, float* result);
void LotusProbeComposedAlbedo(float* program, float* tables, float* wo,
    float* result);
void LotusProbeAlbedoLayout(std::int32_t* result);
void LotusProbeAlbedoNode(std::int32_t table, std::int32_t cosine,
    std::int32_t alpha, std::int32_t layer, float* result);
void LotusProbeAlbedoPoint(std::int32_t table, float x_cosine, float x_alpha,
    std::int32_t sheet, float x_index, float* result);
float LotusProbeAlbedoLookup(float* tables, std::int32_t table, float cos_theta,
    float alpha, float eta, std::int32_t thin);
}

namespace lotus {

// albedo.slang's tables: E1, Eschlick and Edielectric.
constexpr int kAlbedoGgx = 0;
constexpr int kAlbedoSchlickShare = 1;
constexpr int kAlbedoDielectric = 2;
constexpr int kAlbedoTables = 3;

// The quadrature's initial panels per axis of the sampler's square, each
// refined adaptively (AlbedoQuadrature).
constexpr int kTablePanels = 4;

struct AlbedoLayout {
  int size = 0;
  std::array<int, kAlbedoTables> offset{};
  // Nodes along cosine, alpha, and sheet x index.
  std::array<std::array<int, 3>, kAlbedoTables> shape{};
};

inline AlbedoLayout ReadAlbedoLayout() {
  std::array<std::int32_t, 1 + 4 * kAlbedoTables> raw{};
  LotusProbeAlbedoLayout(raw.data());
  AlbedoLayout layout;
  layout.size = raw[0];
  for (int t = 0; t < kAlbedoTables; ++t) {
    layout.offset[t] = raw[1 + 4 * t];
    layout.shape[t] = {raw[2 + 4 * t], raw[3 + 4 * t], raw[4 + 4 * t]};
  }
  return layout;
}

struct AlbedoNode {
  float cos_theta = 1.0f;
  float alpha = 1.0f;
  float eta = 1.0f;
  bool thin = false;
};

inline AlbedoNode ReadAlbedoNode(int table, int cosine, int alpha, int layer) {
  std::array<float, 4> raw{};
  LotusProbeAlbedoNode(table, cosine, alpha, layer, raw.data());
  return {raw[0], raw[1], raw[2], raw[3] != 0.0f};
}

// --- Programs (bsdf-probe.slang's ProbeComposed) -----------------------------

enum Op : int {
  kOpEnd = 0,
  kOpLambert = 1,
  kOpSchlick = 2,
  kOpDielectric = 3,
  kOpMix = 4,
  kOpLayer = 5,
  kOpScale = 6,
  kOpAttenuate = 7,
  kOpAdd = 8,
};

constexpr int kScatterReflect = 1;
constexpr int kScatterTransmit = 2;
constexpr int kScatterBoth = 3;

using Rgb = std::array<float, 3>;
using Normal = std::array<float, 3>;
constexpr Normal kShadingNormal{0.0f, 0.0f, 1.0f};

struct Program {
  std::vector<float> code;

  Program& Lambert(Rgb color, Normal n = kShadingNormal) {
    Push(kOpLambert, {color[0], color[1], color[2], n[0], n[1], n[2]});
    return *this;
  }
  Program& Schlick(Rgb f0, Rgb f90, float alpha, bool compensate,
      Normal n = kShadingNormal) {
    Push(kOpSchlick, {f0[0], f0[1], f0[2], f90[0], f90[1], f90[2], alpha, n[0],
        n[1], n[2], compensate ? 1.0f : 0.0f});
    return *this;
  }
  Program& Dielectric(float eta, Rgb tint, float alpha, int scatter, bool thin,
      bool compensate, Normal n = kShadingNormal) {
    Push(kOpDielectric, {eta, tint[0], tint[1], tint[2], alpha, float(scatter),
        thin ? 1.0f : 0.0f, n[0], n[1], n[2], compensate ? 1.0f : 0.0f});
    return *this;
  }
  Program& Mix(float m) {
    Push(kOpMix, {m});
    return *this;
  }
  Program& Layer() {
    Push(kOpLayer, {});
    return *this;
  }
  Program& Scale(Rgb s) {
    Push(kOpScale, {s[0], s[1], s[2]});
    return *this;
  }
  Program& Attenuate(Rgb t, Normal n, float eta) {
    Push(kOpAttenuate, {t[0], t[1], t[2], n[0], n[1], n[2], eta});
    return *this;
  }
  Program& Add() {
    Push(kOpAdd, {});
    return *this;
  }
  // The code with its terminator.
  std::vector<float> Code() const {
    std::vector<float> out = code;
    out.push_back(float(kOpEnd));
    return out;
  }

 private:
  void Push(Op op, std::vector<float> operands) {
    code.push_back(float(op));
    code.insert(code.end(), operands.begin(), operands.end());
  }
};

// The one-lobe closure whose albedo a table node holds: uncompensated, about
// the shading normal, untinted, reflecting only.
inline Program TableLobe(int table, const AlbedoNode& node) {
  Program p;
  if (table == kAlbedoGgx) {
    p.Schlick({1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, node.alpha, false);
  } else if (table == kAlbedoSchlickShare) {
    p.Schlick({0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, node.alpha, false);
  } else {
    p.Dielectric(node.eta, {1.0f, 1.0f, 1.0f}, node.alpha, kScatterReflect,
        node.thin, false);
  }
  return p;
}

// The albedo of a node's lobe by quadrature of its sample weight over the
// sampler's square. The closure is built with tables of ones, which only
// its selection estimate reads: one lobe is always chosen.
//
// The weight falls from about 1 to 0 within a band of the square whose
// width is of order alpha^2, where the reflected direction crosses the
// horizon, so a fixed rule converges only as 1 / panels there. Each panel
// is refined until its 4 x 4 Gauss-Legendre rule and its four children's
// agree to a share of `tolerance` proportional to its area; every panel is
// split once first, since a rule and its children can agree by coincidence.
struct AlbedoQuadrature {
  std::vector<float> code;
  std::array<float, 3> wo{};
  const std::vector<float>* ones = nullptr;
  double tolerance_density = 0.0;
  long long evaluations = 0;

  double Weight(double ux, double uy) {
    std::array<float, 3> u{float(ux), float(uy), 0.5f};
    std::array<float, 9> result{};
    std::vector<float>& tables = const_cast<std::vector<float>&>(*ones);
    ++evaluations;
    if (LotusProbeComposedSample(code.data(), tables.data(), wo.data(), u.data(),
            result.data()) == 0) {
      return 0.0;
    }
    // Reflected samples only; the lobes reflect in R mode. Channels are
    // equal for an untinted lobe.
    return result[2] > 0.0f ? double(result[3]) : 0.0;
  }

  double Rule(double x0, double x1, double y0, double y1) {
    static constexpr double kNodes[4] = {-0.8611363115940526,
        -0.3399810435848563, 0.3399810435848563, 0.8611363115940526};
    static constexpr double kWeights[4] = {0.3478548451374538,
        0.6521451548625461, 0.6521451548625461, 0.3478548451374538};
    const double cx = 0.5 * (x0 + x1), hx = 0.5 * (x1 - x0);
    const double cy = 0.5 * (y0 + y1), hy = 0.5 * (y1 - y0);
    double sum = 0.0;
    for (int a = 0; a < 4; ++a) {
      for (int b = 0; b < 4; ++b) {
        sum += kWeights[a] * kWeights[b] *
            Weight(cx + hx * kNodes[a], cy + hy * kNodes[b]);
      }
    }
    return sum * hx * hy;
  }

  double Adaptive(double x0, double x1, double y0, double y1, double estimate,
      int depth) {
    constexpr int kMinDepth = 1;
    constexpr int kMaxDepth = 7;
    const double xm = 0.5 * (x0 + x1), ym = 0.5 * (y0 + y1);
    const double parts[4] = {Rule(x0, xm, y0, ym), Rule(xm, x1, y0, ym),
        Rule(x0, xm, ym, y1), Rule(xm, x1, ym, y1)};
    const double fine = parts[0] + parts[1] + parts[2] + parts[3];
    const double area = (x1 - x0) * (y1 - y0);
    if ((depth >= kMinDepth &&
            std::abs(fine - estimate) <= tolerance_density * area) ||
        depth >= kMaxDepth) {
      return fine;
    }
    return Adaptive(x0, xm, y0, ym, parts[0], depth + 1) +
        Adaptive(xm, x1, y0, ym, parts[1], depth + 1) +
        Adaptive(x0, xm, ym, y1, parts[2], depth + 1) +
        Adaptive(xm, x1, ym, y1, parts[3], depth + 1);
  }
};

// The tables' quadrature tolerance, absolute over the whole square.
constexpr double kTableTolerance = 2.0e-5;

inline double IntegrateAlbedo(int table, const AlbedoNode& node,
    double tolerance = kTableTolerance, long long* evaluations = nullptr) {
  const int panels = kTablePanels;
  static const std::vector<float> ones = [] {
    return std::vector<float>(std::size_t(ReadAlbedoLayout().size), 1.0f);
  }();
  AlbedoQuadrature q;
  q.code = TableLobe(table, node).Code();
  const float sin_o =
      std::sqrt(std::max(0.0f, 1.0f - node.cos_theta * node.cos_theta));
  q.wo = {sin_o, 0.0f, node.cos_theta};
  q.ones = &ones;
  q.tolerance_density = tolerance;
  const double h = 1.0 / panels;
  double sum = 0.0;
  for (int i = 0; i < panels; ++i) {
    for (int j = 0; j < panels; ++j) {
      const double x0 = i * h, x1 = (i + 1) * h, y0 = j * h, y1 = (j + 1) * h;
      sum += q.Adaptive(x0, x1, y0, y1, q.Rule(x0, x1, y0, y1), 0);
    }
  }
  if (evaluations) *evaluations += q.evaluations;
  return sum;
}

// --- Checking a table against its lobe (section 7, assertion 6) ----------------

constexpr int kDielectricSheets = 3;
constexpr int kDielectricThin = 2;

inline AlbedoNode ReadAlbedoPoint(int table, float x_cosine, float x_alpha,
    int sheet, float x_index) {
  std::array<float, 4> raw{};
  LotusProbeAlbedoPoint(table, x_cosine, x_alpha, sheet, x_index, raw.data());
  return {raw[0], raw[1], raw[2], raw[3] != 0.0f};
}

// The parts whose accuracy is reported separately: E1, Eschlick, and the
// dielectric table's entering, leaving and thin-wall sheets.
constexpr int kAlbedoParts = 5;
constexpr const char* kAlbedoPartNames[kAlbedoParts] = {"E1", "Eschlick",
    "Edielectric entering", "Edielectric leaving", "Edielectric thin"};

inline int AlbedoPart(int table, int sheet) {
  return table == kAlbedoDielectric ? kAlbedoDielectric + sheet : table;
}

struct TableError {
  double error = 0.0;  // the largest |interpolated - fresh quadrature|
  std::string where;
  long long points = 0;
};

// Compares a table's interpolated value with a fresh quadrature of its lobe:
// at the nodes (`at_nodes`), which finds data the lobes no longer produce,
// or at the centres of the grid's cells, every axis halfway between nodes,
// where interpolation is least accurate; at every strides[table]-th point.
inline std::array<TableError, kAlbedoParts> CheckTables(const float* data,
    bool at_nodes, double tolerance = kTableTolerance,
    std::array<int, kAlbedoTables> strides = {1, 1, 1}) {
  const AlbedoLayout layout = ReadAlbedoLayout();
  struct Point {
    int table, sheet;
    float x_cosine, x_alpha, x_index;
  };
  std::vector<Point> points;
  for (int table = 0; table < kAlbedoTables; ++table) {
    const auto& shape = layout.shape[table];
    const int indices = table == kAlbedoDielectric ? shape[2] / kDielectricSheets : 1;
    const int sheets = table == kAlbedoDielectric ? kDielectricSheets : 1;
    const int shift = at_nodes ? 0 : 1;
    const float half = at_nodes ? 0.0f : 0.5f;
    for (int sheet = 0; sheet < sheets; ++sheet) {
      for (int i = 0; i < std::max(1, indices - (indices > 1 ? shift : 0)); ++i) {
        for (int a = 0; a < shape[1] - shift; ++a) {
          for (int c = 0; c < shape[0] - shift; ++c) {
            points.push_back({table, sheet, (c + half) / float(shape[0] - 1),
                (a + half) / float(shape[1] - 1),
                indices > 1 ? (i + half) / float(indices - 1) : 0.0f});
          }
        }
      }
    }
  }
  // A deterministic subset: every strides[table]-th point of each table.
  {
    std::vector<Point> kept;
    std::array<int, kAlbedoTables> seen{};
    for (const Point& p : points) {
      if (seen[p.table]++ % strides[p.table] == 0) kept.push_back(p);
    }
    points.swap(kept);
  }
  std::vector<double> errors(points.size(), 0.0);
  std::vector<float> table_data(data, data + layout.size);
  std::atomic<std::size_t> next{0};
  std::vector<std::thread> pool;
  const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
  for (unsigned t = 0; t < threads; ++t) {
    pool.emplace_back([&] {
      for (std::size_t i; (i = next.fetch_add(1)) < points.size();) {
        const Point& p = points[i];
        const AlbedoNode node =
            ReadAlbedoPoint(p.table, p.x_cosine, p.x_alpha, p.sheet, p.x_index);
        const double fresh = IntegrateAlbedo(p.table, node, tolerance);
        const double lookup = LotusProbeAlbedoLookup(table_data.data(), p.table,
            node.cos_theta, node.alpha, node.eta, node.thin ? 1 : 0);
        errors[i] = std::abs(lookup - fresh);
      }
    });
  }
  for (std::thread& thread : pool) thread.join();
  std::array<TableError, kAlbedoParts> result{};
  for (std::size_t i = 0; i < points.size(); ++i) {
    TableError& r = result[AlbedoPart(points[i].table, points[i].sheet)];
    ++r.points;
    if (errors[i] > r.error) {
      const Point& p = points[i];
      const AlbedoNode node =
          ReadAlbedoPoint(p.table, p.x_cosine, p.x_alpha, p.sheet, p.x_index);
      char text[160];
      std::snprintf(text, sizeof(text),
          "sheet %d, cos %.5f, alpha %.5f, eta %.4f (x %.3f %.3f %.3f)", p.sheet,
          node.cos_theta, node.alpha, node.eta, p.x_cosine, p.x_alpha, p.x_index);
      r.error = errors[i];
      r.where = text;
    }
  }
  return result;
}

}  // namespace lotus
