// SPDX-License-Identifier: Apache-2.0
// lotus-bsdf-test: the BSDF API's consistency tests (docs/design/BSDF_API.md
// section 7). bsdf-probe.slang exports BsdfEvaluate, BsdfPdf and BsdfSample
// from the real bsdf.slang, compiled to C++ by `slangc -target cpp`; this
// runs them on the CPU over section 7.2's grid of closures and outgoing
// directions and checks section 7.3's assertions:
//
//   1. pointwise consistency of the sampled density, support and, where the
//      closure documents it, the mixture-ratio weight;
//   2. the estimator: the mean weight in each bin of the sphere equals
//      BsdfEvaluate's integral over it;
//   3. the PDF's normalization against the non-delta success rate;
//   4. a chi-squared test of the sampled directions against BsdfPdf;
//   5. delta components' directions, Fresnel split and weights;
//   6. rough transmission's value and density against Walter et al.'s
//      formulas, written independently here.
//
// Every sequence is seeded per configuration, so the run is deterministic
// and its thresholds were fixed once.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "bsdf-tables.hpp"

extern "C" {
void LotusProbeEvaluate(float* material, float* wo, float* wi, float* result);
std::int32_t LotusProbeSample(float* material, float* wo, float* u, float* result);
}

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kMinAlpha = 1.0e-3f;  // bsdf.slang's alpha floor

// Samples per configuration.
constexpr int kSamples = 50000;
// The sphere's bins: polar axis y, normal to the plane of incidence, so
// that every lobe peak lies on the equator y = 0 rather than at a pole.
// psi runs around y from +x; z = sqrt(1 - y^2) sin(psi), so the reflection
// and transmission hemispheres are psi in [0, pi) and [-pi, 0), and both
// boundaries are bin edges.
constexpr int kBandsY = 16;
constexpr int kBandsPsi = 32;
constexpr int kBins = kBandsY * kBandsPsi;
// Assertions 2 to 4 resolve lobes from this roughness up (section 7.3).
constexpr float kStatisticsRoughness = 0.1f;

struct Vec3 {
  double x = 0.0, y = 0.0, z = 0.0;
};
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(double s, Vec3 a) { return {s * a.x, s * a.y, s * a.z}; }
double Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Normalize(Vec3 a) { return (1.0 / std::sqrt(Dot(a, a))) * a; }

// A closure's inputs and an outgoing direction: one point of section 7.2's
// grid.
struct Config {
  float roughness = 0.0f;
  float metallic = 0.0f;
  float transmission = 0.0f;
  float ior = 1.5f;
  float cos_o = 1.0f;
  bool specular_workflow = false;
  bool thin = false;
  bool entering = true;

  // Derived as bsdf.slang derives them.
  double share = 0.0;  // the transmitted share
  double alpha = 0.0;
  double eta = 1.0;    // eta_wi / eta_wo for transmission
  bool smooth = false; // the interface is a delta
  std::array<float, 16> material{};
  std::array<float, 3> wo{};

  std::string Name() const {
    char text[192];
    std::snprintf(text, sizeof(text),
        "roughness %.2f %s metallic %.1f transmission %.1f %s ior %.2f %s "
        "cos_o %.2f",
        roughness, specular_workflow ? "specular-workflow" : "metallic-workflow",
        metallic, transmission, thin ? "thin" : "solid", ior,
        entering ? "entering" : "leaving", cos_o);
    return text;
  }
};

constexpr std::array<float, 3> kBaseColor{0.8f, 0.5f, 0.2f};
constexpr std::array<float, 3> kSpecular{0.5f, 0.3f, 0.1f};
constexpr std::array<float, 3> kTransmissionColor{0.9f, 0.7f, 0.4f};

Config MakeConfig(float roughness, bool specular_workflow, float metallic,
    float transmission, float ior, bool thin, bool entering, float cos_o) {
  Config c;
  c.roughness = roughness;
  c.specular_workflow = specular_workflow;
  c.metallic = metallic;
  c.transmission = transmission;
  c.ior = ior;
  c.thin = thin;
  c.entering = entering;
  c.cos_o = cos_o;
  const double effective_metallic = specular_workflow ? 0.0 : metallic;
  c.share = (1.0 - effective_metallic) * transmission;
  c.alpha = std::max(roughness * roughness, kMinAlpha);
  c.eta = thin || entering ? ior : 1.0 / ior;
  c.smooth = roughness * roughness < kMinAlpha || ior == 1.0f;
  c.material = {kBaseColor[0], kBaseColor[1], kBaseColor[2], roughness,
      metallic, ior, specular_workflow ? 1.0f : 0.0f, kSpecular[0],
      kSpecular[1], kSpecular[2], transmission, kTransmissionColor[0],
      kTransmissionColor[1], kTransmissionColor[2], thin ? 1.0f : 0.0f,
      entering ? 1.0f : 0.0f};
  const float sin_o = std::sqrt(std::max(0.0f, 1.0f - cos_o * cos_o));
  c.wo = {sin_o, 0.0f, cos_o};
  return c;
}

// Section 7.2's grid, without points that differ only in an input the
// closure ignores: metallic in the specular workflow, the solid's side
// without transmission, the index without an interface or Fresnel term, and
// a metal's transmission.
std::vector<Config> Grid() {
  const float roughnesses[] = {0.0f, 0.02f, 0.1f, 0.3f, 0.6f, 1.0f};
  const float metallics[] = {0.0f, 0.5f, 1.0f};
  const float transmissions[] = {0.5f, 1.0f};
  const float iors[] = {1.0f, 1.33f, 1.5f, 2.4f};
  const float cosines[] = {1.0f, 0.7f, 0.3f, 0.05f};
  std::vector<Config> grid;
  for (float roughness : roughnesses) {
    for (float cos_o : cosines) {
      const auto interfaces = [&](bool specular_workflow, float metallic) {
        for (float transmission : transmissions) {
          for (float ior : iors) {
            grid.push_back(MakeConfig(roughness, specular_workflow, metallic,
                transmission, ior, false, true, cos_o));
            grid.push_back(MakeConfig(roughness, specular_workflow, metallic,
                transmission, ior, false, false, cos_o));
            grid.push_back(MakeConfig(roughness, specular_workflow, metallic,
                transmission, ior, true, true, cos_o));
          }
        }
      };
      for (float metallic : metallics) {
        for (float ior : iors) {
          grid.push_back(MakeConfig(roughness, false, metallic, 0.0f, ior,
              false, true, cos_o));
        }
        if (metallic < 1.0f) {
          interfaces(false, metallic);
        }
      }
      grid.push_back(MakeConfig(roughness, true, 0.0f, 0.0f, 1.5f, false, true,
          cos_o));
      interfaces(true, 0.0f);
    }
  }
  return grid;
}

// The probe's results in double.
struct Evaluation {
  std::array<double, 3> value{};
  double pdf = 0.0;
};

Evaluation Evaluate(const Config& c, Vec3 wi) {
  std::array<float, 16> material = c.material;
  std::array<float, 3> wo = c.wo;
  std::array<float, 3> direction{float(wi.x), float(wi.y), float(wi.z)};
  std::array<float, 4> result{};
  LotusProbeEvaluate(material.data(), wo.data(), direction.data(), result.data());
  return {{result[0], result[1], result[2]}, result[3]};
}

struct Sample {
  bool success = false;
  Vec3 wi;
  std::array<double, 3> weight{};
  double pdf = 0.0;
  bool delta = false;
  bool transmitted = false;
  // The sampled direction exactly as the probe returned it.
  std::array<float, 3> wi_float{};
};

Sample Draw(const Config& c, std::array<float, 3> u) {
  std::array<float, 16> material = c.material;
  std::array<float, 3> wo = c.wo;
  std::array<float, 9> result{};
  Sample s;
  s.success = LotusProbeSample(material.data(), wo.data(), u.data(), result.data()) != 0;
  s.wi_float = {result[0], result[1], result[2]};
  s.wi = {result[0], result[1], result[2]};
  s.weight = {result[3], result[4], result[5]};
  s.pdf = result[6];
  s.delta = result[7] != 0.0f;
  s.transmitted = result[8] != 0.0f;
  return s;
}

// SplitMix64: uniform floats in [0, 1).
struct Random {
  std::uint64_t state;
  explicit Random(std::uint64_t seed) : state(seed) {}
  std::uint64_t Next() {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  float Uniform() { return float(Next() >> 40) * 0x1.0p-24f; }
};

int BinOf(Vec3 w) {
  const double y = std::clamp(w.y, -1.0, 1.0);
  const double psi = std::atan2(w.z, w.x);
  const int iy = std::min(int((y + 1.0) * 0.5 * kBandsY), kBandsY - 1);
  const int ip = std::min(int((psi + kPi) / (2.0 * kPi) * kBandsPsi), kBandsPsi - 1);
  return iy * kBandsPsi + std::max(ip, 0);
}

Vec3 Direction(double y, double psi) {
  const double s = std::sqrt(std::max(0.0, 1.0 - y * y));
  return {s * std::cos(psi), y, s * std::sin(psi)};
}

// --- Independent reference formulas (assertions 5 and 6) -------------------

double FresnelExact(double cos_i, double eta) {
  cos_i = std::clamp(cos_i, 0.0, 1.0);
  const double sin2_t = (1.0 - cos_i * cos_i) / (eta * eta);
  if (sin2_t >= 1.0) return 1.0;
  const double cos_t = std::sqrt(1.0 - sin2_t);
  const double rs = (cos_i - eta * cos_t) / (cos_i + eta * cos_t);
  const double rp = (eta * cos_i - cos_t) / (eta * cos_i + cos_t);
  return 0.5 * (rs * rs + rp * rp);
}

double WallReflectance(const Config& c, double cos_i) {
  const double r = FresnelExact(cos_i, c.eta);
  return c.thin ? 2.0 * r / (1.0 + r) : r;
}

double GgxDistribution(Vec3 m, double alpha) {
  if (m.z <= 0.0) return 0.0;
  const double cos2 = m.z * m.z;
  const double tan2 = (1.0 - cos2) / cos2;
  const double a2 = alpha * alpha;
  const double t = a2 + tan2;
  return a2 / (kPi * cos2 * cos2 * t * t);
}

double SmithLambda(Vec3 w, double alpha) {
  const double cos2 = w.z * w.z;
  const double tan2 = std::max(0.0, 1.0 - cos2) / cos2;
  return 0.5 * (std::sqrt(1.0 + alpha * alpha * tan2) - 1.0);
}

// Rough transmission toward wi.z < 0: f * |cos(theta_i)| in radiance mode and
// the density of visible-normal sampling. A solid's BTDF is Walter et al.'s
// equation 21 with eta_o = eta_wo = 1 and eta_i = eta_wi = eta, which is the
// radiance BTDF; the Jacobian is their equation 17 for the sampled side. A
// thin wall's is the reflection mirrored through the tangent plane.
Evaluation ReferenceTransmission(const Config& c, Vec3 wo, Vec3 wi) {
  Evaluation e;
  const double alpha = c.alpha;
  const double lambda_o = SmithLambda(wo, alpha);
  if (c.thin) {
    const Vec3 r{wi.x, wi.y, -wi.z};
    const Vec3 h = Normalize(wo + r);
    const double share = c.share * (1.0 - WallReflectance(c, Dot(wo, h)));
    const double d = GgxDistribution(h, alpha);
    const double g2 = 1.0 / (1.0 + lambda_o + SmithLambda(r, alpha));
    const double g1 = 1.0 / (1.0 + lambda_o);
    const double f = share * d * g2 / (4.0 * wo.z * r.z);
    for (int k = 0; k < 3; ++k) e.value[k] = kTransmissionColor[k] * f * r.z;
    e.pdf = share * (g1 * Dot(wo, h) * d / wo.z) / (4.0 * Dot(wo, h));
    return e;
  }
  const double eta_i = c.eta;  // wi's side
  const double eta_o = 1.0;    // wo's side
  Vec3 h = Normalize(eta_i * wi + eta_o * wo);
  if (h.z < 0.0) h = -1.0 * h;
  const double ih = Dot(wi, h);
  const double oh = Dot(wo, h);
  if (!(oh > 0.0) || !(ih < 0.0)) return e;
  const double d = GgxDistribution(h, alpha);
  const double g2 = 1.0 / (1.0 + lambda_o + SmithLambda(wi, alpha));
  const double g1 = 1.0 / (1.0 + lambda_o);
  const double fresnel = WallReflectance(c, oh);
  const double denominator = eta_i * ih + eta_o * oh;
  const double btdf = std::abs(ih) * std::abs(oh) / (std::abs(wi.z) * wo.z) *
      eta_o * eta_o * (1.0 - fresnel) * g2 * d / (denominator * denominator);
  const double f = c.share * btdf;
  for (int k = 0; k < 3; ++k) e.value[k] = kTransmissionColor[k] * f * std::abs(wi.z);
  const double visible = g1 * oh * d / wo.z;
  const double jacobian = eta_i * eta_i * std::abs(ih) / (denominator * denominator);
  e.pdf = c.share * (1.0 - fresnel) * visible * jacobian;
  return e;
}

// --- Chi-squared --------------------------------------------------------------

// The regularized upper incomplete gamma function Q(a, x) (Press et al.,
// Numerical Recipes, section 6.2).
double GammaQ(double a, double x) {
  if (x <= 0.0) return 1.0;
  const double log_prefix = -x + a * std::log(x) - std::lgamma(a);
  if (x < a + 1.0) {
    double term = 1.0 / a, sum = term;
    for (int n = 1; n < 1000; ++n) {
      term *= x / (a + n);
      sum += term;
      if (std::abs(term) < std::abs(sum) * 1e-15) break;
    }
    return 1.0 - sum * std::exp(log_prefix);
  }
  double b = x + 1.0 - a, cc = 1.0 / 1e-300, d = 1.0 / b, h = d;
  for (int i = 1; i < 1000; ++i) {
    const double an = -i * (i - a);
    b += 2.0;
    d = an * d + b;
    if (std::abs(d) < 1e-300) d = 1e-300;
    cc = b + an / cc;
    if (std::abs(cc) < 1e-300) cc = 1e-300;
    d = 1.0 / d;
    const double delta = d * cc;
    h *= delta;
    if (std::abs(delta - 1.0) < 1e-15) break;
  }
  return std::exp(log_prefix) * h;
}

// --- Quadrature -------------------------------------------------------------

using Quad = std::array<double, 4>;  // BsdfEvaluate's RGB, then BsdfPdf

struct Cell {
  double y0, y1, p0, p1;
};

// A cell is refined until its integrals agree with its four children's to a
// bin's share of an absolute tolerance, in probability (and value) units,
// plus a relative one, which keeps a narrow lobe's peak from refining to
// the limit. Both are far below the samples' standard errors. Every bin is
// first split twice, to 4 x 4 cells: a bin's rule and its children's can
// agree by coincidence (a rough refraction lobe's bin did, 2.6% off).
constexpr double kBinTolerance = 1.0e-5;
constexpr double kRelativeTolerance = 3.0e-4;
constexpr int kMinDepth = 1;
constexpr int kMaxDepth = 8;

struct Integrator {
  const std::function<Evaluation(Vec3)>& evaluate;
  double alpha;               // the narrowest rough lobe's
  std::vector<double> peaks;  // lobe centres' psi, all at y = 0
  long long support_violations = 0;
  long long evaluations = 0;
  long long capped = 0;

  Quad Rule(const Cell& cell) {
    static constexpr double nodes[4] = {-0.8611363115940526, -0.3399810435848563,
        0.3399810435848563, 0.8611363115940526};
    static constexpr double weights[4] = {0.3478548451374538, 0.6521451548625461,
        0.6521451548625461, 0.3478548451374538};
    const double cy = 0.5 * (cell.y0 + cell.y1), hy = 0.5 * (cell.y1 - cell.y0);
    const double cp = 0.5 * (cell.p0 + cell.p1), hp = 0.5 * (cell.p1 - cell.p0);
    Quad sum{};
    for (int i = 0; i < 4; ++i) {
      for (int j = 0; j < 4; ++j) {
        const Evaluation e =
            evaluate(Direction(cy + hy * nodes[i], cp + hp * nodes[j]));
        ++evaluations;
        const double w = weights[i] * weights[j] * hy * hp;
        if (std::max({e.value[0], e.value[1], e.value[2]}) > 0.0 && !(e.pdf > 0.0)) {
          ++support_violations;
        }
        for (int k = 0; k < 3; ++k) sum[k] += w * e.value[k];
        sum[3] += w * e.pdf;
      }
    }
    return sum;
  }

  bool ContainsPeak(const Cell& cell) const {
    const double resolve = alpha / 8.0;
    if (cell.y1 - cell.y0 <= resolve && cell.p1 - cell.p0 <= resolve) return false;
    const double margin = 1e-9;
    if (!(cell.y0 - margin <= 0.0 && 0.0 <= cell.y1 + margin)) return false;
    for (double peak : peaks) {
      if (cell.p0 - margin <= peak && peak <= cell.p1 + margin) return true;
    }
    return false;
  }

  Quad Integrate(const Cell& cell, const Quad& estimate, int depth,
      double tolerance_density) {
    const double ym = 0.5 * (cell.y0 + cell.y1), pm = 0.5 * (cell.p0 + cell.p1);
    const Cell children[4] = {{cell.y0, ym, cell.p0, pm}, {cell.y0, ym, pm, cell.p1},
        {ym, cell.y1, cell.p0, pm}, {ym, cell.y1, pm, cell.p1}};
    Quad parts[4];
    Quad fine{};
    for (int i = 0; i < 4; ++i) {
      parts[i] = Rule(children[i]);
      for (int k = 0; k < 4; ++k) fine[k] += parts[i][k];
    }
    double error = 0.0;
    for (int k = 0; k < 4; ++k) error = std::max(error, std::abs(fine[k] - estimate[k]));
    const double area = (cell.y1 - cell.y0) * (cell.p1 - cell.p0);
    double magnitude = 0.0;
    for (int k = 0; k < 4; ++k) magnitude = std::max(magnitude, std::abs(fine[k]));
    const bool refine = depth < kMinDepth || ContainsPeak(cell) ||
        error > tolerance_density * area + kRelativeTolerance * magnitude;
    if (!refine) return fine;
    if (depth >= kMaxDepth) {
      ++capped;
      return fine;
    }
    Quad sum{};
    for (int i = 0; i < 4; ++i) {
      const Quad part = Integrate(children[i], parts[i], depth + 1, tolerance_density);
      for (int k = 0; k < 4; ++k) sum[k] += part[k];
    }
    return sum;
  }
};

// --- One configuration --------------------------------------------------------

struct Result {
  std::vector<std::string> failures;
  bool statistics = false;
  // Diagnostics for the summary.
  double max_pdf_error = 0.0;
  double max_ratio_error = 0.0;
  double normalization = 0.0;
  double success_rate = 0.0;
  double chi2_p = 1.0;
  double max_estimator_sigma = 0.0;
  double max_transmission_error = 0.0;
  // The albedo, the mean energy weight per channel, and its standard error.
  std::array<double, 3> albedo{}, albedo_se{};
  long long evaluations = 0;
  long long capped = 0;
};

bool MixtureRatio(const Config& c) {
  return c.share == 0.0 || (c.share == 1.0 && !c.smooth);
}

// What the statistics need to know of a closure under test: its functions
// at one outgoing direction, and where its lobes peak.
struct Target {
  std::string name;
  Vec3 wo;
  std::function<Evaluation(Vec3)> evaluate;
  std::function<Sample(std::array<float, 3>)> draw;
  bool statistics = false;     // assertions 2 to 4 apply
  bool mixture_ratio = false;  // every non-delta weight is the mixture ratio
  double alpha = 1.0;          // the narrowest rough lobe's, for quadrature
  std::vector<double> peaks;   // lobe centres' psi at y = 0
  // UsdPreviewSurface's closure: assertion 5 as BSDF_API.md states it.
  const Config* preview = nullptr;
  // A closure that is one rough transmitting interface: assertion 6.
  const Config* transmission_reference = nullptr;
  // A composed closure's delta directions (assertion 5), and the factor that
  // turns a transmitted sample's radiance weight into energy (eta^2 across a
  // solid's interface).
  std::vector<Vec3> delta_directions;
  double transmit_energy = 1.0;
};

void Fail(Result& r, const Target& t, const std::string& detail) {
  if (r.failures.size() < 4) r.failures.push_back(t.name + ": " + detail);
}

// --only's diagnosis: print the bins whose count is far from expected.
bool g_dump_bins = false;

Result Run(const Target& t, std::uint64_t seed, double chi2_significance) {
  Result r;
  r.statistics = t.statistics;
  const Vec3 wo = t.wo;
  const Config* preview = t.preview;
  Random random(seed);

  std::vector<long long> counts(kBins, 0);
  std::vector<std::array<double, 3>> sum(kBins), sum2(kBins);
  long long non_delta = 0, delta = 0, delta_reflected = 0;

  // Assertion 5's expected directions and split.
  const Vec3 mirror{-wo.x, -wo.y, wo.z};
  Vec3 refracted = -1.0 * wo;
  double reflectance = 0.0;
  double radiance_scale = 1.0;
  if (preview) {
    const Config& c = *preview;
    reflectance = WallReflectance(c, wo.z);
    if (!c.thin) {
      const double sin2_t = (1.0 - wo.z * wo.z) / (c.eta * c.eta);
      if (sin2_t < 1.0) {
        refracted = Normalize(-1.0 / c.eta * wo +
            (wo.z / c.eta - std::sqrt(1.0 - sin2_t)) * Vec3{0.0, 0.0, 1.0});
      }
    }
    radiance_scale = c.thin ? 1.0 : 1.0 / (c.eta * c.eta);
  }
  // The albedo: the mean energy weight, failures and delta samples included.
  std::array<double, 3> energy{}, energy2{};

  for (int n = 0; n < kSamples; ++n) {
    const std::array<float, 3> u{random.Uniform(), random.Uniform(), random.Uniform()};
    const Sample s = t.draw(u);
    if (!s.success) continue;
    const double wmax = std::max({s.weight[0], s.weight[1], s.weight[2]});
    const double wmin = std::min({s.weight[0], s.weight[1], s.weight[2]});
    if (!std::isfinite(wmax) || wmin < 0.0) {
      Fail(r, t, "1: weight not finite and non-negative");
      continue;
    }
    for (int k = 0; k < 3; ++k) {
      const double e = s.weight[k] * (s.transmitted ? t.transmit_energy : 1.0);
      energy[k] += e;
      energy2[k] += e * e;
    }
    if (s.transmitted != (s.wi.z < 0.0)) {
      Fail(r, t, "1: transmitted flag disagrees with wi.z");
    }
    if (s.delta) {
      ++delta;
      if (s.pdf != 0.0) Fail(r, t, "1: delta sample with nonzero pdf");
      if (!preview) {
        // Assertion 5: a delta direction of one of the closure's lobes.
        bool found = false;
        for (const Vec3& expected : t.delta_directions) {
          const Vec3 d = s.wi - expected;
          found = found || std::sqrt(Dot(d, d)) <= 1e-5;
        }
        if (!found) Fail(r, t, "5: delta direction is none of the lobes'");
        continue;
      }
      const Config& c = *preview;
      if (!c.smooth || c.share == 0.0) {
        Fail(r, t, "5: delta sample from a closure without a delta component");
        continue;
      }
      // Assertion 5: the mirror or straight-through direction and its weight.
      const bool reflected = s.wi.z > 0.0;
      delta_reflected += reflected ? 1 : 0;
      const Vec3 expected = reflected ? mirror : refracted;
      const Vec3 d = s.wi - expected;
      if (std::sqrt(Dot(d, d)) > 1e-5) Fail(r, t, "5: delta direction is wrong");
      for (int k = 0; k < 3; ++k) {
        const double w = reflected ? 1.0 : kTransmissionColor[k] * radiance_scale;
        if (std::abs(s.weight[k] - w) > 1e-5 * std::max(1.0, w)) {
          Fail(r, t, "5: delta weight is wrong");
          break;
        }
      }
      continue;
    }
    ++non_delta;
    const Evaluation e = t.evaluate(s.wi);
    if (!(s.pdf > 0.0) || !(e.pdf > 0.0)) {
      Fail(r, t, "1: non-delta sample where BsdfPdf is zero");
      continue;
    }
    const double pdf_error = std::abs(s.pdf - e.pdf) / e.pdf;
    r.max_pdf_error = std::max(r.max_pdf_error, pdf_error);
    if (pdf_error > 1e-5) Fail(r, t, "1: sample pdf differs from BsdfPdf");
    if (t.mixture_ratio) {
      const double scale = std::max({e.value[0], e.value[1], e.value[2]}) / e.pdf;
      for (int k = 0; k < 3; ++k) {
        const double error = std::abs(s.weight[k] - e.value[k] / e.pdf) /
            std::max(scale, 1e-6);
        r.max_ratio_error = std::max(r.max_ratio_error, error);
        if (error > 1e-3) {
          Fail(r, t, "1: weight differs from the mixture ratio");
          break;
        }
      }
    }
    // Assertion 6: rough refraction and wall transmission.
    if (t.transmission_reference && s.transmitted && r.statistics) {
      const Evaluation ref = ReferenceTransmission(*t.transmission_reference, wo, s.wi);
      const double pdf_ref_error = std::abs(e.pdf - ref.pdf) / ref.pdf;
      double value_error = 0.0;
      for (int k = 0; k < 3; ++k) {
        value_error = std::max(value_error,
            std::abs(e.value[k] - ref.value[k]) / ref.value[k]);
      }
      const double error = std::max(pdf_ref_error, value_error);
      r.max_transmission_error = std::max(r.max_transmission_error, error);
      if (!(error <= 1e-2)) Fail(r, t, "6: transmission differs from Walter et al.");
    }
    const int bin = BinOf(s.wi);
    ++counts[bin];
    for (int k = 0; k < 3; ++k) {
      sum[bin][k] += s.weight[k];
      sum2[bin][k] += s.weight[k] * s.weight[k];
    }
  }

  const double samples = double(kSamples);
  for (int k = 0; k < 3; ++k) {
    r.albedo[k] = energy[k] / samples;
    r.albedo_se[k] = std::sqrt(
        std::max(0.0, energy2[k] / samples - r.albedo[k] * r.albedo[k]) / samples);
  }

  // Assertion 5: the delta split's probabilities.
  if (preview && preview->smooth && preview->share > 0.0) {
    const Config& c = *preview;
    const double n = double(kSamples);
    const double p_delta = c.share;
    const double tolerance = 5.0 * std::sqrt(p_delta * (1.0 - p_delta) / n) + 1e-4;
    // Failed interface samples (none for a smooth interface) would count
    // against the share; there are none.
    if (std::abs(delta / n - p_delta) > tolerance) {
      Fail(r, t, "5: delta selection probability differs from the share");
    }
    if (delta > 0) {
      const double p = reflectance;
      const double se = std::sqrt(std::max(p * (1.0 - p), 1.0 / delta) / delta);
      if (std::abs(double(delta_reflected) / delta - p) > 5.0 * se + 1e-4) {
        Fail(r, t, "5: delta reflection probability differs from Fresnel");
      }
    }
  }

  if (!r.statistics) return r;

  // Quadrature of BsdfEvaluate and BsdfPdf over each bin.
  Integrator integrator{t.evaluate, t.alpha, t.peaks};
  const double band_y = 2.0 / kBandsY, band_psi = 2.0 * kPi / kBandsPsi;
  const double tolerance_density = kBinTolerance / (band_y * band_psi);
  std::vector<Quad> integral(kBins);
  double total_pdf = 0.0;
  for (int iy = 0; iy < kBandsY; ++iy) {
    for (int ip = 0; ip < kBandsPsi; ++ip) {
      const Cell cell{-1.0 + iy * band_y, -1.0 + (iy + 1) * band_y,
          -kPi + ip * band_psi, -kPi + (ip + 1) * band_psi};
      const Quad q = integrator.Integrate(cell, integrator.Rule(cell), 0,
          tolerance_density);
      integral[iy * kBandsPsi + ip] = q;
      total_pdf += q[3];
    }
  }
  r.evaluations = integrator.evaluations;
  r.capped = integrator.capped;
  if (integrator.support_violations > 0) {
    Fail(r, t, "1: BsdfEvaluate positive where BsdfPdf is zero");
  }

  // Assertion 3: normalization.
  const double n = double(kSamples);
  const double rate = non_delta / n;
  r.normalization = total_pdf;
  r.success_rate = rate;
  const double rate_se = std::sqrt(std::max(rate * (1.0 - rate), 1.0 / n) / n);
  // Besides the quadrature, float rounding limits the integral: at alpha
  // 0.01, GGX's h.z^2 (alpha^2 - 1) + 1 loses digits near the peak, which
  // moves the integral by about 1e-4 (a float and a double evaluation of a
  // pure metal's lobe differ by 9e-5).
  const double precision = 5e-4;
  if (std::abs(total_pdf - rate) > 5.0 * rate_se + precision ||
      total_pdf > 1.0 + precision) {
    char text[128];
    std::snprintf(text, sizeof(text),
        "3: integral of BsdfPdf %.6f, non-delta success rate %.6f", total_pdf, rate);
    Fail(r, t, text);
  }

  // Cells for assertions 2 and 4: each bin with an expected count of at least
  // 5, the others pooled into one, and the delta-or-failed outcome.
  struct Pooled {
    double expected = 0.0;
    long long observed = 0;
    Quad integral{};
    std::array<double, 3> sum{}, sum2{};
  };
  std::vector<Pooled> cells;
  Pooled low;
  for (int b = 0; b < kBins; ++b) {
    Pooled p;
    p.expected = n * integral[b][3];
    p.observed = counts[b];
    p.integral = integral[b];
    p.sum = sum[b];
    p.sum2 = sum2[b];
    if (g_dump_bins) {
      const int iy = b / kBandsPsi, ip = b % kBandsPsi;
      std::printf("bin y %d psi %d (y %.3f..%.3f, psi %.3f..%.3f): observed %lld expected %.1f\n",
          iy, ip, -1.0 + iy * band_y, -1.0 + (iy + 1) * band_y, -kPi + ip * band_psi,
          -kPi + (ip + 1) * band_psi, p.observed, p.expected);
    }
    if (p.expected == 0.0 && p.observed > 0) {
      Fail(r, t, "4: samples in a bin where BsdfPdf integrates to zero");
    }
    Pooled& target = p.expected >= 5.0 ? cells.emplace_back() : low;
    target.expected += p.expected;
    target.observed += p.observed;
    for (int k = 0; k < 4; ++k) target.integral[k] += p.integral[k];
    for (int k = 0; k < 3; ++k) {
      target.sum[k] += p.sum[k];
      target.sum2[k] += p.sum2[k];
    }
  }
  if (low.expected > 0.0 || low.observed > 0) cells.push_back(low);

  // Assertion 2: the estimator per cell and channel.
  for (const Pooled& p : cells) {
    for (int k = 0; k < 3; ++k) {
      const double mean = p.sum[k] / n;
      // The sample variance, or, where a cell holds few samples, the
      // variance its integrals predict for a weight constant over it.
      const double sampled = std::max(0.0, p.sum2[k] / n - mean * mean) / n;
      const double predicted = p.integral[3] > 0.0
          ? p.integral[k] * p.integral[k] / (p.integral[3] * n)
          : 0.0;
      const double se = std::sqrt(std::max(sampled, predicted));
      const double difference = std::abs(mean - p.integral[k]);
      const double allowed = 6.0 * se + 1e-5 + 1e-3 * std::abs(p.integral[k]);
      if (se > 0.0) {
        r.max_estimator_sigma = std::max(r.max_estimator_sigma, difference / se);
      }
      if (difference > allowed) {
        char text[160];
        std::snprintf(text, sizeof(text),
            "2: mean weight %.6g differs from the integral %.6g (se %.3g)", mean,
            p.integral[k], se);
        Fail(r, t, text);
        break;
      }
    }
  }

  // Assertion 4: chi-squared of where the non-delta samples fall, given how
  // many there are: the expected counts are BsdfPdf's integrals scaled to
  // the observed total. How many there are is assertion 3's, which allows
  // for the integral's precision; a delta-or-failed cell here would turn a
  // 1e-4 rounding of the integral into a significant count.
  double chi2 = 0.0;
  int dof = -1;
  std::vector<std::pair<double, long long>> terms;
  for (const Pooled& p : cells) {
    terms.push_back({total_pdf > 0.0 ? p.expected * non_delta / (n * total_pdf) : 0.0,
        p.observed});
  }
  // Fold a remaining low-expectation term into the smallest other term.
  std::sort(terms.begin(), terms.end());
  while (terms.size() > 1 && terms.front().first < 5.0) {
    terms[1].first += terms[0].first;
    terms[1].second += terms[0].second;
    terms.erase(terms.begin());
    std::sort(terms.begin(), terms.end());
  }
  for (const auto& [expected, observed] : terms) {
    if (expected <= 0.0) continue;
    const double d = observed - expected;
    chi2 += d * d / expected;
    ++dof;
  }
  if (dof > 0) {
    r.chi2_p = GammaQ(0.5 * dof, 0.5 * chi2);
    if (r.chi2_p < chi2_significance) {
      char text[128];
      std::snprintf(text, sizeof(text), "4: chi-squared %.1f, %d dof, p = %.3g", chi2,
          dof, r.chi2_p);
      Fail(r, t, text);
    }
  }
  return r;
}

// UsdPreviewSurface's closure as a Target, with BSDF_API.md section 7.3's
// expectations.
Target MakePreviewTarget(const Config& c) {
  Target t;
  t.name = c.Name();
  t.wo = {c.wo[0], c.wo[1], c.wo[2]};
  t.evaluate = [&c](Vec3 wi) { return Evaluate(c, wi); };
  t.draw = [&c](std::array<float, 3> u) { return Draw(c, u); };
  t.statistics = c.roughness >= kStatisticsRoughness;
  t.mixture_ratio = MixtureRatio(c);
  t.alpha = c.alpha;
  t.preview = &c;
  if (c.share == 1.0) t.transmission_reference = &c;
  const Vec3 wo = t.wo;
  t.peaks.push_back(std::atan2(wo.z, -wo.x));
  if (c.share > 0.0 && !c.smooth) {
    if (c.thin) {
      t.peaks.push_back(std::atan2(-wo.z, -wo.x));
    } else {
      const double sin2_t = (1.0 - wo.z * wo.z) / (c.eta * c.eta);
      if (sin2_t < 1.0) {
        const Vec3 refracted = Normalize(-1.0 / c.eta * wo +
            (wo.z / c.eta - std::sqrt(1.0 - sin2_t)) * Vec3{0.0, 0.0, 1.0});
        t.peaks.push_back(std::atan2(refracted.z, refracted.x));
      }
    }
  }
  return t;
}

// --- Composed closures (docs/design/LAYERED_BSDF.md section 7) ---------------

// Assertion 6 of section 7: the tables against fresh quadratures of their
// lobes, at their nodes, where a difference means the data no longer
// matches the lobes, and at the centres of their cells, where interpolation
// is least accurate. The interpolation tolerances are section 5.6's 1e-3
// where the tables reach it, and the measured maxima over every centre where
// they do not (the report records them): the thin wall's at an index of
// 1.01, and the leaving sheet's at small alpha just inside the critical
// angle.
//
// A deterministic subset keeps the fast tier fast: about 64 nodes and 200
// centres per part, the nodes integrated as the generator integrates them,
// so that unchanged data reproduces, and the centres to a looser quadrature
// tolerance, still far below the interpolation tolerances.
// `lotus-bsdf-tables --check` compares every point.
constexpr double kNodeTolerance = 1.0e-4;
constexpr double kInterpolationTolerance[lotus::kAlbedoParts] = {1.0e-3, 1.0e-3,
    1.0e-3, 1.2e-2, 1.2e-3};
constexpr std::array<int, lotus::kAlbedoTables> kNodeStrides = {17, 17, 383};
constexpr std::array<int, lotus::kAlbedoTables> kCentreStrides = {5, 5, 109};
constexpr double kCentreQuadrature = 1.0e-4;

// The committed albedo tables, as the probe binds them. The literals are
// doubles; the probe reads floats.
const double kAlbedoData[] = {
#include "bsdf-albedo-tables.inc"
};

float* AlbedoData() {
  static std::vector<float> data = [] {
    std::vector<float> values;
    for (double v : kAlbedoData) values.push_back(float(v));
    return values;
  }();
  return data.data();
}

using lotus::Normal;
using lotus::Rgb;

constexpr Rgb kWhite{1.0f, 1.0f, 1.0f};
constexpr Rgb kTint{0.8f, 0.5f, 0.2f};
constexpr Rgb kTransmissionTint{0.9f, 0.7f, 0.4f};

Vec3 ToVec(Normal n) { return {n[0], n[1], n[2]}; }

// A normal tilted in the plane of incidence, toward wo for a positive angle.
Normal Tilted(double degrees) {
  const double t = degrees * kPi / 180.0;
  return {float(std::sin(t)), 0.0f, float(std::cos(t))};
}

// What the test needs to know of a lobe besides its program.
struct LobeInfo {
  int type = lotus::kOpLambert;
  double alpha = 1.0;
  double eta = 1.0;
  int scatter = lotus::kScatterReflect;
  bool thin = false;
  bool smooth = false;
  Vec3 n{0.0, 0.0, 1.0};
};

// A composed closure under construction: its program and its lobes, in
// order.
struct Piece {
  lotus::Program program;
  std::vector<LobeInfo> lobes;
};

Piece Lambert(Rgb color, Normal n = lotus::kShadingNormal) {
  Piece p;
  p.program.Lambert(color, n);
  p.lobes.push_back({lotus::kOpLambert, 1.0, 1.0, 0, false, false, ToVec(n)});
  return p;
}

Piece Schlick(Rgb f0, Rgb f90, float roughness, bool compensate,
    Normal n = lotus::kShadingNormal) {
  Piece p;
  const float alpha = roughness * roughness;
  p.program.Schlick(f0, f90, alpha, compensate, n);
  p.lobes.push_back({lotus::kOpSchlick,
      std::max(double(alpha), double(kMinAlpha)), 1.0, lotus::kScatterReflect,
      false, false, ToVec(n)});
  return p;
}

Piece Dielectric(float eta, Rgb tint, float roughness, int scatter, bool thin,
    bool compensate, Normal n = lotus::kShadingNormal) {
  Piece p;
  const float alpha = roughness * roughness;
  p.program.Dielectric(eta, tint, alpha, scatter, thin, compensate, n);
  p.lobes.push_back({lotus::kOpDielectric,
      std::max(double(alpha), double(kMinAlpha)), eta, scatter, thin,
      alpha < kMinAlpha || eta == 1.0f, ToVec(n)});
  return p;
}

Piece Join(const Piece& a, const Piece& b) {
  Piece p = a;
  p.program.code.insert(p.program.code.end(), b.program.code.begin(),
      b.program.code.end());
  p.lobes.insert(p.lobes.end(), b.lobes.begin(), b.lobes.end());
  return p;
}

Piece Mix(const Piece& bg, const Piece& fg, float m) {
  Piece p = Join(bg, fg);
  p.program.Mix(m);
  return p;
}

Piece LayerOf(const Piece& top, const Piece& base) {
  Piece p = Join(top, base);
  p.program.Layer();
  return p;
}

Piece Attenuated(Piece p, Rgb transmittance, Normal n, float eta) {
  p.program.Attenuate(transmittance, n, eta);
  return p;
}

// The largest energy an uncompensated or transmitting configuration loses,
// by the roughness of its rough lobe, fixed from the deterministic run (the
// report records the measured values). Single scattering loses most at
// normal incidence on the roughest surfaces.
double LossBound(float roughness) {
  if (roughness < 0.05f) return 0.003;  // measured 0.0011
  if (roughness < 0.2f) return 0.03;    // 0.024
  if (roughness < 0.45f) return 0.16;   // 0.133
  if (roughness < 0.8f) return 0.37;    // 0.306
  return 0.84;                          // 0.697
}

// What a configuration's albedo must satisfy (section 7, assertions 2, 3).
enum class Energy {
  kBound,    // at most 1 + kBoundTolerance
  kFurnace,  // 1 within the tolerance
  kLoss,     // at most 1 + kBoundTolerance, at least 1 - the tolerance
};

// Section 7's albedo tolerances, fixed once from the deterministic run (the
// report records the measured values): a layer's furnace deviation is the
// table error of its top's albedo, and a compensated lobe's is E1's table
// error over E1.
constexpr double kExactFurnace = 1.0e-5;
constexpr double kLayerFurnace = 1.5e-3;
constexpr double kCompensatedFurnace = 5.0e-3;
constexpr double kBoundTolerance = 5.0e-3;

struct Composed {
  std::string family;
  std::string label;
  Piece piece;
  float cos_o = 1.0f;
  Energy energy = Energy::kBound;
  double tolerance = kBoundTolerance;
  // Assertion 4 applies: every lobe uncompensated, about one normal.
  bool reciprocal = false;
  // The roughness a loss bound is stated for.
  float roughness = 0.0f;
  // Assertion 5 applies: the closure is layer(top, base).
  bool layered = false;
  Piece top, base;
  // A single rough transmitting interface: BSDF API assertion 6.
  bool has_reference = false;
  Config reference;

  std::vector<float> code;
  std::string Name() const {
    char text[64];
    std::snprintf(text, sizeof(text), " cos_o %.2f", cos_o);
    return family + " " + label + text;
  }
};

std::array<float, 3> WoOf(float cos_o) {
  const float sin_o = std::sqrt(std::max(0.0f, 1.0f - cos_o * cos_o));
  return {sin_o, 0.0f, cos_o};
}

Evaluation EvaluateCode(const std::vector<float>& code, std::array<float, 3> wo,
    Vec3 wi) {
  std::array<float, 3> direction{float(wi.x), float(wi.y), float(wi.z)};
  std::array<float, 4> result{};
  LotusProbeComposedEvaluate(const_cast<float*>(code.data()), AlbedoData(),
      wo.data(), direction.data(), result.data());
  return {{result[0], result[1], result[2]}, result[3]};
}

Sample DrawCode(const std::vector<float>& code, std::array<float, 3> wo,
    std::array<float, 3> u) {
  std::array<float, 9> result{};
  Sample s;
  s.success = LotusProbeComposedSample(const_cast<float*>(code.data()),
      AlbedoData(), wo.data(), u.data(), result.data()) != 0;
  s.wi_float = {result[0], result[1], result[2]};
  s.wi = {result[0], result[1], result[2]};
  s.weight = {result[3], result[4], result[5]};
  s.pdf = result[6];
  s.delta = result[7] != 0.0f;
  s.transmitted = result[8] != 0.0f;
  return s;
}

std::array<double, 3> AlbedoOf(const std::vector<float>& code,
    std::array<float, 3> wo) {
  std::array<float, 3> result{};
  LotusProbeComposedAlbedo(const_cast<float*>(code.data()), AlbedoData(),
      wo.data(), result.data());
  return {result[0], result[1], result[2]};
}

// Refraction of wo about n with relative index eta; false under total
// internal reflection.
bool Refract(Vec3 wo, Vec3 n, double eta, Vec3& out) {
  const double c = Dot(wo, n);
  const double sin2_t = (1.0 - c * c) / (eta * eta);
  if (sin2_t >= 1.0) return false;
  out = Normalize(-1.0 / eta * wo + (c / eta - std::sqrt(1.0 - sin2_t)) * n);
  return true;
}

double Psi(Vec3 w) { return std::atan2(w.z, w.x); }

Target MakeComposedTarget(const Composed& c) {
  Target t;
  t.name = c.Name();
  const std::array<float, 3> wo = WoOf(c.cos_o);
  t.wo = {wo[0], wo[1], wo[2]};
  const std::vector<float>* code = &c.code;
  t.evaluate = [code, wo](Vec3 wi) { return EvaluateCode(*code, wo, wi); };
  t.draw = [code, wo](std::array<float, 3> u) { return DrawCode(*code, wo, u); };
  t.mixture_ratio = true;
  t.statistics = true;
  t.alpha = 1.0;
  for (const LobeInfo& lobe : c.piece.lobes) {
    const Vec3 n = lobe.n;
    if (lobe.type == lotus::kOpLambert || Dot(t.wo, n) <= 0.0) continue;
    const Vec3 mirror = 2.0 * Dot(t.wo, n) * n - t.wo;
    const bool reflects = (lobe.scatter & lotus::kScatterReflect) != 0;
    const bool transmits = (lobe.scatter & lotus::kScatterTransmit) != 0;
    Vec3 through{};
    bool has_through = false;
    if (transmits) {
      if (lobe.thin) {
        through = mirror - 2.0 * Dot(mirror, n) * n;
        has_through = true;
      } else {
        has_through = Refract(t.wo, n, lobe.eta, through);
        t.transmit_energy = lobe.eta * lobe.eta;
      }
    }
    if (lobe.smooth) {
      if (reflects) t.delta_directions.push_back(mirror);
      if (has_through) t.delta_directions.push_back(through);
      continue;
    }
    t.alpha = std::min(t.alpha, lobe.alpha);
    if (lobe.alpha < 0.01) t.statistics = false;
    t.peaks.push_back(Psi(mirror));
    if (has_through) t.peaks.push_back(Psi(through));
  }
  if (c.has_reference) t.transmission_reference = &c.reference;
  return t;
}

// Section 7's configurations, each at every outgoing cosine.
std::vector<Composed> ComposedGrid() {
  const float cosines[] = {1.0f, 0.7f, 0.3f, 0.05f};
  const float roughnesses[] = {0.0f, 0.1f, 0.3f, 0.6f, 1.0f};
  std::vector<Composed> base;
  float loss_roughness = 0.0f;
  const auto add = [&](std::string family, std::string label, Piece piece,
                       Energy energy, double tolerance, bool reciprocal) {
    Composed c;
    c.family = std::move(family);
    c.label = std::move(label);
    c.piece = std::move(piece);
    c.energy = energy;
    c.tolerance = tolerance;
    c.reciprocal = reciprocal;
    c.roughness = loss_roughness;
    base.push_back(std::move(c));
    return base.size() - 1;
  };
  const auto layered = [&](std::string family, std::string label,
                           const Piece& top, const Piece& under, Energy energy,
                           double tolerance) {
    const std::size_t i = add(std::move(family), std::move(label),
        LayerOf(top, under), energy, tolerance, false);
    base[i].layered = true;
    base[i].top = top;
    base[i].base = under;
  };
  char text[160];

  // Each primitive lobe alone.
  add("lambert", "white", Lambert(kWhite), Energy::kFurnace, kExactFurnace, true);
  add("lambert", "tinted", Lambert(kTint), Energy::kBound, kBoundTolerance, true);
  for (float r : roughnesses) {
    loss_roughness = r;
    std::snprintf(text, sizeof(text), "roughness %.1f", r);
    const std::string rough = text;
    add("schlick", rough + " white compensated",
        Schlick(kWhite, kWhite, r, true), Energy::kFurnace,
        kCompensatedFurnace, false);
    add("schlick-uncompensated", rough + " white",
        Schlick(kWhite, kWhite, r, false), Energy::kLoss, LossBound(r), true);
    add("schlick", rough + " tinted metal compensated",
        Schlick(kTint, kWhite, r, true), Energy::kBound, kBoundTolerance, false);
    add("schlick", rough + " f0 0.04",
        Schlick({0.04f, 0.04f, 0.04f}, kWhite, r, false), Energy::kBound,
        kBoundTolerance, true);
    for (float eta : {1.5f, 1.0f / 1.5f}) {
      std::snprintf(text, sizeof(text), "%s eta %.3f", rough.c_str(), eta);
      add("dielectric-r", std::string(text) + " compensated",
          Dielectric(eta, kWhite, r, lotus::kScatterReflect, false, true),
          Energy::kBound, kBoundTolerance, false);
      add("dielectric-r", text,
          Dielectric(eta, kWhite, r, lotus::kScatterReflect, false, false),
          Energy::kBound, kBoundTolerance, true);
    }
    for (float ior : {1.33f, 1.5f, 2.4f}) {
      for (bool entering : {true, false}) {
        const float eta = entering ? ior : 1.0f / ior;
        std::snprintf(text, sizeof(text), "%s eta %.3f", rough.c_str(), eta);
        add("dielectric-rt", std::string(text) + " white",
            Dielectric(eta, kWhite, r, lotus::kScatterBoth, false, false),
            Energy::kLoss, LossBound(r), true);
        const std::size_t tinted = add("dielectric-rt",
            std::string(text) + " tinted",
            Dielectric(eta, kTransmissionTint, r, lotus::kScatterBoth, false,
                false),
            Energy::kBound, kBoundTolerance, false);
        if (r * r >= kMinAlpha) {
          base[tinted].has_reference = true;
          base[tinted].reference =
              MakeConfig(r, false, 0.0f, 1.0f, ior, false, entering, 1.0f);
        }
      }
    }
    add("dielectric-rt", rough + " thin eta 1.5 white",
        Dielectric(1.5f, kWhite, r, lotus::kScatterBoth, true, false),
        Energy::kLoss, LossBound(r), true);
    add("dielectric-t", rough + " eta 1.5",
        Dielectric(1.5f, kWhite, r, lotus::kScatterTransmit, false, false),
        Energy::kBound, kBoundTolerance, false);
  }

  // mix(Lambert, GgxSchlick).
  for (float m : {0.0f, 0.3f, 1.0f}) {
    for (float r : {0.1f, 0.3f, 1.0f}) {
      std::snprintf(text, sizeof(text), "m %.1f roughness %.1f", m, r);
      add("mix", std::string(text) + " white compensated",
          Mix(Lambert(kWhite), Schlick(kWhite, kWhite, r, true), m),
          Energy::kFurnace, kCompensatedFurnace, false);
      add("mix", std::string(text) + " tinted",
          Mix(Lambert(kTint), Schlick({0.04f, 0.04f, 0.04f}, kWhite, r, false),
              m),
          Energy::kBound, kBoundTolerance, true);
    }
  }

  // layer(GgxDielectric R, Lambert).
  for (float r : roughnesses) {
    for (float eta : {1.0f, 1.5f, 2.4f}) {
      for (bool compensate : {false, true}) {
        std::snprintf(text, sizeof(text), "coat roughness %.1f eta %.1f%s", r,
            eta, compensate ? " compensated" : "");
        layered("coat-lambert", text,
            Dielectric(eta, kWhite, r, lotus::kScatterReflect, false,
                compensate),
            Lambert(kWhite), Energy::kFurnace, kLayerFurnace);
      }
    }
  }

  // layer(GgxDielectric R, GgxSchlick).
  for (float coat : {0.0f, 0.3f, 1.0f}) {
    for (float r : {0.3f, 1.0f}) {
      std::snprintf(text, sizeof(text), "coat roughness %.1f metal %.1f", coat,
          r);
      const Piece top =
          Dielectric(1.5f, kWhite, coat, lotus::kScatterReflect, false, true);
      layered("coat-metal", std::string(text) + " white", top,
          Schlick(kWhite, kWhite, r, true), Energy::kFurnace,
          kLayerFurnace + kCompensatedFurnace);
      layered("coat-metal", std::string(text) + " tinted", top,
          Schlick(kTint, kWhite, r, true), Energy::kBound, kBoundTolerance);
    }
  }

  // A coat with its own normal.
  for (double tilt : {0.0, 15.0, 45.0, -45.0}) {
    for (float coat : {0.0f, 0.3f}) {
      std::snprintf(text, sizeof(text), "tilt %.0f coat roughness %.1f", tilt,
          coat);
      layered("coat-normal", text,
          Dielectric(1.5f, kWhite, coat, lotus::kScatterReflect, false, true,
              Tilted(tilt)),
          Lambert(kWhite), Energy::kFurnace, kLayerFurnace);
    }
  }

  // A coat with absorption.
  for (float coat : {0.0f, 0.3f}) {
    const Piece top =
        Dielectric(1.5f, kWhite, coat, lotus::kScatterReflect, false, true);
    std::snprintf(text, sizeof(text), "coat roughness %.1f", coat);
    layered("coat-absorption", std::string(text) + " white", top,
        Attenuated(Lambert(kWhite), kWhite, lotus::kShadingNormal, 1.5f),
        Energy::kFurnace, kLayerFurnace);
    layered("coat-absorption", std::string(text) + " tinted", top,
        Attenuated(Lambert(kWhite), {0.6f, 0.8f, 0.9f}, lotus::kShadingNormal,
            1.5f),
        Energy::kBound, kBoundTolerance);
  }

  // layer(coat, mix(layer(GgxDielectric R, Lambert), GgxSchlick)): the
  // shape glTF PBR and Standard Surface share.
  for (float metallic : {0.0f, 0.5f, 1.0f}) {
    for (float r : {0.3f, 1.0f}) {
      for (float coat : {0.0f, 0.3f}) {
        std::snprintf(text, sizeof(text),
            "metallic %.1f roughness %.1f coat %.1f", metallic, r, coat);
        const std::string label = text;
        const Piece top =
            Dielectric(1.5f, kWhite, coat, lotus::kScatterReflect, false, true);
        const auto shape = [&](Rgb color, Energy energy, double tolerance,
                               const char* suffix) {
          const Piece specular =
              Dielectric(1.5f, kWhite, r, lotus::kScatterReflect, false, true);
          const Piece under = Mix(LayerOf(specular, Lambert(color)),
              Schlick(color, kWhite, r, true), metallic);
          layered("gltf-shape", label + suffix, top, under, energy, tolerance);
        };
        shape(kWhite, Energy::kFurnace,
            2.0 * kLayerFurnace + kCompensatedFurnace, " white");
        shape(kTint, Energy::kBound, kBoundTolerance, " tinted");
      }
    }
  }

  // GgxDielectric RT under a coat.
  for (float coat : {0.0f, 0.3f}) {
    for (float r : {0.0f, 0.3f}) {
      loss_roughness = r;
      for (bool thin : {false, true}) {
        for (float eta : {1.5f, 1.0f / 1.5f}) {
          if (thin && eta < 1.0f) continue;
          std::snprintf(text, sizeof(text),
              "coat %.1f roughness %.1f %s eta %.3f", coat, r,
              thin ? "thin" : "solid", eta);
          layered("coat-transmission", text,
              Dielectric(1.5f, kWhite, coat, lotus::kScatterReflect, false,
                  true),
              Dielectric(eta, kWhite, r, lotus::kScatterBoth, thin, false),
              Energy::kLoss, LossBound(r));
        }
      }
    }
  }

  std::vector<Composed> grid;
  for (const Composed& c : base) {
    for (float cos_o : cosines) {
      Composed at = c;
      at.cos_o = cos_o;
      at.code = at.piece.program.Code();
      grid.push_back(std::move(at));
    }
  }
  return grid;
}

// Assertions 2 and 3 on a configuration's albedo. Returns the worst
// deviation from 1 (furnace), loss (loss) or excess over 1 (bound).
double CheckEnergy(const Composed& c, Result& r, const Target& t) {
  double worst = 0.0;
  for (int k = 0; k < 3; ++k) {
    const double a = r.albedo[k];
    const double noise = 5.0 * r.albedo_se[k];
    char text[160];
    if (c.energy == Energy::kFurnace) {
      worst = std::max(worst, std::abs(a - 1.0));
      if (std::abs(a - 1.0) > c.tolerance + noise) {
        std::snprintf(text, sizeof(text), "furnace: albedo %.6f (se %.2g)", a,
            r.albedo_se[k]);
        Fail(r, t, text);
        break;
      }
      continue;
    }
    worst = std::max(worst, c.energy == Energy::kLoss ? 1.0 - a : a - 1.0);
    if (a > 1.0 + kBoundTolerance + noise) {
      std::snprintf(text, sizeof(text), "energy bound: albedo %.6f (se %.2g)",
          a, r.albedo_se[k]);
      Fail(r, t, text);
      break;
    }
    if (c.energy == Energy::kLoss && 1.0 - a > c.tolerance + noise) {
      std::snprintf(text, sizeof(text), "loss %.6f above its bound %.3g",
          1.0 - a, c.tolerance);
      Fail(r, t, text);
      break;
    }
  }
  return worst;
}

Vec3 UniformSphere(Random& random) {
  const double z = 1.0 - 2.0 * random.Uniform();
  const double phi = 2.0 * kPi * random.Uniform();
  const double s = std::sqrt(std::max(0.0, 1.0 - z * z));
  return {s * std::cos(phi), s * std::sin(phi), z};
}

double MaxOf(const std::array<double, 3>& v) {
  return std::max({v[0], v[1], v[2]});
}

// The program with every dielectric's index inverted: the closure seen from
// the other side of its interface.
std::vector<float> Reversed(std::vector<float> code) {
  static constexpr std::size_t kLength[] = {1, 7, 12, 12, 2, 1, 4, 8, 1};
  for (std::size_t at = 0; at < code.size();) {
    const int op = int(code[at]);
    if (op <= 0 || op > 8) break;
    if (op == lotus::kOpDielectric) code[at + 1] = 1.0f / code[at + 1];
    at += kLength[op];
  }
  return code;
}

// Assertion 4: reciprocity of the reciprocal parts (section 4.3). f is the
// value over the cosine about the lobes' common normal, the shading normal.
// Across a solid's interface the reverse closure is seen from the other
// side, with every index inverted, in the frame flipped through the tangent
// plane, and f(wo, wi) / eta_o^2 = f(wi, wo) / eta_i^2 with eta_o = 1 and
// eta_i = eta (Veach, 1997, section 5.2).
double CheckReciprocity(const Composed& c, Result& r, const Target& t,
    std::uint64_t seed) {
  if (!c.reciprocal) return 0.0;
  const std::array<float, 3> wo_f = WoOf(c.cos_o);
  const Vec3 wo{wo_f[0], wo_f[1], wo_f[2]};
  bool solid = false;
  double eta = 1.0;
  for (const LobeInfo& lobe : c.piece.lobes) {
    if (lobe.type == lotus::kOpDielectric && !lobe.thin &&
        (lobe.scatter & lotus::kScatterTransmit) != 0) {
      solid = true;
      eta = lobe.eta;
    }
  }
  const std::vector<float> reverse = solid ? Reversed(c.code) : c.code;
  Random random(seed);
  double worst = 0.0;
  for (int i = 0; i < 64; ++i) {
    const Vec3 wi = UniformSphere(random);
    if (std::abs(wi.z) < 1e-3) continue;
    const double f1 = MaxOf(EvaluateCode(c.code, wo_f, wi).value) / std::abs(wi.z);
    double f2 = 0.0;
    if (wi.z > 0.0) {
      const std::array<float, 3> wi_f{float(wi.x), float(wi.y), float(wi.z)};
      f2 = MaxOf(EvaluateCode(c.code, wi_f, wo).value) / wo.z;
    } else {
      const std::array<float, 3> wo_r{float(wi.x), float(wi.y), float(-wi.z)};
      const Vec3 wi_r{wo.x, wo.y, -wo.z};
      f2 = MaxOf(EvaluateCode(reverse, wo_r, wi_r).value) / wo.z;
      if (solid) f2 /= eta * eta;
    }
    const double scale = std::max(f1, f2);
    if (scale < 1e-6) continue;
    const double error = std::abs(f1 - f2) / scale;
    worst = std::max(worst, error);
    // Float rounding limits a rough refraction's value to about 1e-2, as
    // BSDF API assertion 6 measures it.
    if (error > (wi.z > 0.0 ? 1e-4 : 1e-2)) {
      char text[192];
      std::snprintf(text, sizeof(text),
          "4: reciprocity f(wo, wi) %.6g, f(wi, wo) %.6g at wi (%.3f %.3f %.3f)",
          f1, f2, wi.x, wi.y, wi.z);
      Fail(r, t, text);
      break;
    }
  }
  return worst;
}

// Assertion 5: a layer's value is section 4.1's formula, the top's value
// plus the base's scaled by 1 - E_top(wo), at random direction pairs.
double CheckStructure(const Composed& c, Result& r, const Target& t,
    std::uint64_t seed) {
  if (!c.layered) return 0.0;
  const std::array<float, 3> wo = WoOf(c.cos_o);
  const std::vector<float> top = c.top.program.Code();
  const std::vector<float> base = c.base.program.Code();
  const std::array<double, 3> e_top = AlbedoOf(top, wo);
  Random random(seed);
  double worst = 0.0;
  for (int i = 0; i < 64; ++i) {
    const Vec3 wi = UniformSphere(random);
    const Evaluation whole = EvaluateCode(c.code, wo, wi);
    const Evaluation f_top = EvaluateCode(top, wo, wi);
    const Evaluation f_base = EvaluateCode(base, wo, wi);
    for (int k = 0; k < 3; ++k) {
      const double expected = f_top.value[k] +
          std::clamp(1.0 - e_top[k], 0.0, 1.0) * f_base.value[k];
      const double error = std::abs(whole.value[k] - expected) /
          std::max(std::abs(expected), 1e-3);
      worst = std::max(worst, error);
      if (error > 1e-4) {
        char text[192];
        std::snprintf(text, sizeof(text),
            "5: layer value %.6g, formula %.6g at wi (%.3f %.3f %.3f)",
            whole.value[k], expected, wi.x, wi.y, wi.z);
        Fail(r, t, text);
        return worst;
      }
    }
  }
  return worst;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<Config> full = Grid();
  const std::vector<Composed> composed_full = ComposedGrid();
  // Targets refer to their configurations, which stay in place from here.
  std::vector<std::size_t> indices(full.size());
  for (std::size_t i = 0; i < indices.size(); ++i) indices[i] = i;
  std::vector<std::size_t> composed(composed_full.size());
  for (std::size_t i = 0; i < composed.size(); ++i) composed[i] = i;
  bool tables = true;
  // --only <index>: one preview configuration with its bins, for diagnosis.
  // --composed <family>: the composed configurations of one family.
  if (argc == 3 && std::string(argv[1]) == "--only") {
    indices = {std::stoul(argv[2])};
    composed.clear();
    tables = false;
    g_dump_bins = true;
  }
  if (argc == 3 && std::string(argv[1]) == "--composed") {
    indices.clear();
    composed.clear();
    tables = false;
    for (std::size_t i = 0; i < composed_full.size(); ++i) {
      if (composed_full[i].family == argv[2]) composed.push_back(i);
    }
  }
  if (argc == 2 && std::string(argv[1]) == "--tables") {
    indices.clear();
    composed.clear();
  }
  // The significance is section 7.3's, corrected for every configuration
  // with statistics in the full run, so a partial run tests the same.
  int statistics_configs = 0;
  for (const Config& c : full) {
    statistics_configs += c.roughness >= kStatisticsRoughness ? 1 : 0;
  }
  for (const Composed& c : composed_full) {
    statistics_configs += MakeComposedTarget(c).statistics ? 1 : 0;
  }
  const double chi2_significance = 0.01 / statistics_configs;
  // Each configuration's sequence is seeded by its index: the preview grid's
  // first, then the composed configurations'.
  std::vector<Target> targets;
  std::vector<std::uint64_t> seeds;
  for (std::size_t i : indices) {
    targets.push_back(MakePreviewTarget(full.at(i)));
    seeds.push_back(0x10705B5DFull * (i + 1));
  }
  const std::size_t first_composed = targets.size();
  for (std::size_t i : composed) {
    targets.push_back(MakeComposedTarget(composed_full[i]));
    seeds.push_back(0x10705B5DFull * (full.size() + i + 1));
  }
  if (targets.size() == 1) {
    std::printf("%s\n", targets[0].name.c_str());
  }

  std::vector<Result> results(targets.size());
  struct Measures {
    double energy = 0.0, reciprocity = 0.0, structure = 0.0;
  };
  std::vector<Measures> measures(targets.size());
  std::atomic<std::size_t> next{0};
  const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
  std::vector<std::thread> pool;
  for (unsigned th = 0; th < threads; ++th) {
    pool.emplace_back([&] {
      for (std::size_t i; (i = next.fetch_add(1)) < targets.size();) {
        Result r = Run(targets[i], seeds[i], chi2_significance);
        if (i >= first_composed) {
          const Composed& c = composed_full[composed[i - first_composed]];
          measures[i].energy = CheckEnergy(c, r, targets[i]);
          measures[i].reciprocity =
              CheckReciprocity(c, r, targets[i], seeds[i] + 1);
          measures[i].structure = CheckStructure(c, r, targets[i], seeds[i] + 2);
        }
        results[i] = std::move(r);
      }
    });
  }
  for (std::thread& thread : pool) thread.join();

  int failed = 0;
  double max_pdf_error = 0.0, max_ratio_error = 0.0, min_p = 1.0, max_sigma = 0.0;
  double max_transmission = 0.0, max_normalization = 0.0;
  long long evaluations = 0, capped = 0;
  int with_statistics = 0;
  for (const Result& r : results) {
    if (targets.size() == 1) {
      std::printf("%s: evaluations %lld, capped %lld\n", targets[0].name.c_str(),
          r.evaluations, r.capped);
    }
    for (const std::string& failure : r.failures) std::printf("FAIL %s\n", failure.c_str());
    failed += r.failures.empty() ? 0 : 1;
    max_pdf_error = std::max(max_pdf_error, r.max_pdf_error);
    max_ratio_error = std::max(max_ratio_error, r.max_ratio_error);
    max_transmission = std::max(max_transmission, r.max_transmission_error);
    if (r.statistics) {
      ++with_statistics;
      min_p = std::min(min_p, r.chi2_p);
      max_sigma = std::max(max_sigma, r.max_estimator_sigma);
      max_normalization =
          std::max(max_normalization, std::abs(r.normalization - r.success_rate));
    }
    evaluations += r.evaluations;
    capped += r.capped;
  }
  std::printf("lotus-bsdf-test: %zu configurations (%zu preview, %zu composed; "
      "%d with statistics), %d samples each\n",
      targets.size(), first_composed, targets.size() - first_composed,
      with_statistics, kSamples);
  std::printf("  1 pointwise: max relative pdf error %.3g, max mixture-ratio error %.3g\n",
      max_pdf_error, max_ratio_error);
  std::printf("  2 estimator: max |mean - integral| / se %.2f\n", max_sigma);
  std::printf("  3 normalization: max |integral - success rate| %.3g\n", max_normalization);
  std::printf("  4 chi-squared: min p %.3g (threshold %.3g)\n", min_p, chi2_significance);
  std::printf("  6 transmission: max relative error %.3g\n", max_transmission);
  std::printf("  quadrature: %lld evaluations, %lld cells at the depth limit\n",
      evaluations, capped);

  // Section 7's composed measures per family.
  struct Family {
    std::string name;
    double energy = 0.0, reciprocity = 0.0, structure = 0.0;
    int count = 0;
    Energy kind = Energy::kBound;
  };
  std::vector<Family> families;
  for (std::size_t i = first_composed; i < targets.size(); ++i) {
    const Composed& c = composed_full[composed[i - first_composed]];
    auto it = std::find_if(families.begin(), families.end(),
        [&](const Family& f) { return f.name == c.family; });
    if (it == families.end()) {
      families.push_back({c.family});
      it = families.end() - 1;
    }
    ++it->count;
    if (c.energy != Energy::kBound) it->kind = c.energy;
    it->energy = std::max(it->energy, measures[i].energy);
    it->reciprocity = std::max(it->reciprocity, measures[i].reciprocity);
    it->structure = std::max(it->structure, measures[i].structure);
  }
  // The measured losses by family and roughness.
  for (std::size_t i = first_composed; i < targets.size(); ++i) {
    const Composed& c = composed_full[composed[i - first_composed]];
    if (c.energy != Energy::kLoss) continue;
    std::printf("    loss %-22s roughness %.1f cos_o %.2f: %.4f (%s)\n",
        c.family.c_str(), c.roughness, c.cos_o, measures[i].energy, c.label.c_str());
  }
  for (const Family& f : families) {
    const char* kind = f.kind == Energy::kFurnace ? "|albedo - 1|"
        : f.kind == Energy::kLoss ? "loss" : "albedo - 1";
    std::printf("  %-22s %4d: max %s %.3g, reciprocity %.2g, structure %.2g\n",
        f.name.c_str(), f.count, kind, f.energy, f.reciprocity, f.structure);
  }

  // Section 7's assertion 6: the tables.
  if (tables) {
    const lotus::AlbedoLayout layout = lotus::ReadAlbedoLayout();
    if (int(std::size(kAlbedoData)) != layout.size) {
      std::printf("FAIL tables: bsdf-albedo-tables.inc holds %zu floats, albedo.slang %d\n",
          std::size(kAlbedoData), layout.size);
      ++failed;
    } else {
      for (bool at_nodes : {true, false}) {
        const auto errors = lotus::CheckTables(AlbedoData(), at_nodes,
            at_nodes ? lotus::kTableTolerance : kCentreQuadrature,
            at_nodes ? kNodeStrides : kCentreStrides);
        for (int part = 0; part < lotus::kAlbedoParts; ++part) {
          const char* name = lotus::kAlbedoPartNames[part];
          const double allowed =
              at_nodes ? kNodeTolerance : kInterpolationTolerance[part];
          std::printf("  6 tables %s %-20s %5lld points: max error %.3g at %s\n",
              at_nodes ? "nodes  " : "centres", name, errors[part].points,
              errors[part].error, errors[part].where.c_str());
          if (!(errors[part].error <= allowed)) {
            std::printf("FAIL tables: %s %s error %.3g above %.3g\n", name,
                at_nodes ? "node" : "centre", errors[part].error, allowed);
            ++failed;
          }
        }
      }
    }
  }
  if (failed > 0) {
    std::printf("lotus-bsdf-test: %d configurations failed\n", failed);
    return 1;
  }
  std::printf("lotus-bsdf-test: passed\n");
  return 0;
}
