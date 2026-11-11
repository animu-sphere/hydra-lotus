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
#include <string>
#include <thread>
#include <vector>

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
  const Config& config;
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
            Evaluate(config, Direction(cy + hy * nodes[i], cp + hp * nodes[j]));
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
    const double resolve = config.alpha / 8.0;
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
  long long evaluations = 0;
  long long capped = 0;
};

bool MixtureRatio(const Config& c) {
  return c.share == 0.0 || (c.share == 1.0 && !c.smooth);
}

void Fail(Result& r, const Config& c, const std::string& detail) {
  if (r.failures.size() < 4) r.failures.push_back(c.Name() + ": " + detail);
}

// --only's diagnosis: print the bins whose count is far from expected.
bool g_dump_bins = false;

Result Run(const Config& c, std::uint64_t seed, double chi2_significance) {
  Result r;
  r.statistics = c.roughness >= kStatisticsRoughness;
  const Vec3 wo{c.wo[0], c.wo[1], c.wo[2]};
  Random random(seed);

  std::vector<long long> counts(kBins, 0);
  std::vector<std::array<double, 3>> sum(kBins), sum2(kBins);
  long long non_delta = 0, delta = 0, delta_reflected = 0;

  // Assertion 5's expected directions and split.
  const Vec3 mirror{-wo.x, -wo.y, wo.z};
  Vec3 refracted = -1.0 * wo;
  double reflectance = WallReflectance(c, wo.z);
  if (!c.thin) {
    const double sin2_t = (1.0 - wo.z * wo.z) / (c.eta * c.eta);
    if (sin2_t < 1.0) {
      refracted = Normalize(-1.0 / c.eta * wo +
          (wo.z / c.eta - std::sqrt(1.0 - sin2_t)) * Vec3{0.0, 0.0, 1.0});
    }
  }
  const double radiance_scale = c.thin ? 1.0 : 1.0 / (c.eta * c.eta);

  for (int n = 0; n < kSamples; ++n) {
    const std::array<float, 3> u{random.Uniform(), random.Uniform(), random.Uniform()};
    const Sample s = Draw(c, u);
    if (!s.success) continue;
    const double wmax = std::max({s.weight[0], s.weight[1], s.weight[2]});
    const double wmin = std::min({s.weight[0], s.weight[1], s.weight[2]});
    if (!std::isfinite(wmax) || wmin < 0.0) {
      Fail(r, c, "1: weight not finite and non-negative");
      continue;
    }
    if (s.transmitted != (s.wi.z < 0.0)) {
      Fail(r, c, "1: transmitted flag disagrees with wi.z");
    }
    if (s.delta) {
      ++delta;
      if (s.pdf != 0.0) Fail(r, c, "1: delta sample with nonzero pdf");
      if (!c.smooth || c.share == 0.0) {
        Fail(r, c, "5: delta sample from a closure without a delta component");
        continue;
      }
      // Assertion 5: the mirror or straight-through direction and its weight.
      const bool reflected = s.wi.z > 0.0;
      delta_reflected += reflected ? 1 : 0;
      const Vec3 expected = reflected ? mirror : refracted;
      const Vec3 d = s.wi - expected;
      if (std::sqrt(Dot(d, d)) > 1e-5) Fail(r, c, "5: delta direction is wrong");
      for (int k = 0; k < 3; ++k) {
        const double w = reflected ? 1.0 : kTransmissionColor[k] * radiance_scale;
        if (std::abs(s.weight[k] - w) > 1e-5 * std::max(1.0, w)) {
          Fail(r, c, "5: delta weight is wrong");
          break;
        }
      }
      continue;
    }
    ++non_delta;
    const Evaluation e = Evaluate(c, s.wi);
    if (!(s.pdf > 0.0) || !(e.pdf > 0.0)) {
      Fail(r, c, "1: non-delta sample where BsdfPdf is zero");
      continue;
    }
    const double pdf_error = std::abs(s.pdf - e.pdf) / e.pdf;
    r.max_pdf_error = std::max(r.max_pdf_error, pdf_error);
    if (pdf_error > 1e-5) Fail(r, c, "1: sample pdf differs from BsdfPdf");
    if (MixtureRatio(c)) {
      const double scale = std::max({e.value[0], e.value[1], e.value[2]}) / e.pdf;
      for (int k = 0; k < 3; ++k) {
        const double error = std::abs(s.weight[k] - e.value[k] / e.pdf) /
            std::max(scale, 1e-6);
        r.max_ratio_error = std::max(r.max_ratio_error, error);
        if (error > 1e-3) {
          Fail(r, c, "1: weight differs from the mixture ratio");
          break;
        }
      }
    }
    // Assertion 6: rough refraction and wall transmission.
    if (s.transmitted && c.share == 1.0 && r.statistics) {
      const Evaluation ref = ReferenceTransmission(c, wo, s.wi);
      const double pdf_ref_error = std::abs(e.pdf - ref.pdf) / ref.pdf;
      double value_error = 0.0;
      for (int k = 0; k < 3; ++k) {
        value_error = std::max(value_error,
            std::abs(e.value[k] - ref.value[k]) / ref.value[k]);
      }
      const double error = std::max(pdf_ref_error, value_error);
      r.max_transmission_error = std::max(r.max_transmission_error, error);
      if (!(error <= 1e-2)) Fail(r, c, "6: transmission differs from Walter et al.");
    }
    const int bin = BinOf(s.wi);
    ++counts[bin];
    for (int k = 0; k < 3; ++k) {
      sum[bin][k] += s.weight[k];
      sum2[bin][k] += s.weight[k] * s.weight[k];
    }
  }

  // Assertion 5: the delta split's probabilities.
  if (c.smooth && c.share > 0.0) {
    const double n = double(kSamples);
    const double p_delta = c.share;
    const double tolerance = 5.0 * std::sqrt(p_delta * (1.0 - p_delta) / n) + 1e-4;
    // Failed interface samples (none for a smooth interface) would count
    // against the share; there are none.
    if (std::abs(delta / n - p_delta) > tolerance) {
      Fail(r, c, "5: delta selection probability differs from the share");
    }
    if (delta > 0) {
      const double p = reflectance;
      const double se = std::sqrt(std::max(p * (1.0 - p), 1.0 / delta) / delta);
      if (std::abs(double(delta_reflected) / delta - p) > 5.0 * se + 1e-4) {
        Fail(r, c, "5: delta reflection probability differs from Fresnel");
      }
    }
  }

  if (!r.statistics) return r;

  // Quadrature of BsdfEvaluate and BsdfPdf over each bin.
  Integrator integrator{c, {}};
  integrator.peaks.push_back(std::atan2(wo.z, -wo.x));
  if (c.share > 0.0 && !c.smooth) {
    if (c.thin) {
      integrator.peaks.push_back(std::atan2(-wo.z, -wo.x));
    } else if ((1.0 - wo.z * wo.z) / (c.eta * c.eta) < 1.0) {
      integrator.peaks.push_back(std::atan2(refracted.z, refracted.x));
    }
  }
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
    Fail(r, c, "1: BsdfEvaluate positive where BsdfPdf is zero");
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
    Fail(r, c, text);
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
      Fail(r, c, "4: samples in a bin where BsdfPdf integrates to zero");
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
        Fail(r, c, text);
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
      Fail(r, c, text);
    }
  }
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<Config> full = Grid();
  int statistics_configs = 0;
  for (const Config& c : full) {
    statistics_configs += c.roughness >= kStatisticsRoughness ? 1 : 0;
  }
  // Section 7.3's significance, corrected for the number of tests.
  const double chi2_significance = 0.01 / statistics_configs;
  // Each configuration's sequence is seeded by its index in the grid.
  std::vector<std::size_t> indices(full.size());
  for (std::size_t i = 0; i < indices.size(); ++i) indices[i] = i;
  // --only <index>: one configuration with its bins, for diagnosis.
  if (argc == 3 && std::string(argv[1]) == "--only") {
    indices = {std::stoul(argv[2])};
    g_dump_bins = true;
  }
  std::vector<Config> grid;
  for (std::size_t i : indices) grid.push_back(full.at(i));
  if (grid.size() == 1) {
    std::printf("%s\n", grid[0].Name().c_str());
  }

  std::vector<Result> results(grid.size());
  std::atomic<std::size_t> next{0};
  const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
  std::vector<std::thread> pool;
  for (unsigned t = 0; t < threads; ++t) {
    pool.emplace_back([&] {
      for (std::size_t i; (i = next.fetch_add(1)) < grid.size();) {
        results[i] = Run(grid[i], 0x10705B5DFull * (indices[i] + 1), chi2_significance);
      }
    });
  }
  for (std::thread& thread : pool) thread.join();

  int failed = 0;
  double max_pdf_error = 0.0, max_ratio_error = 0.0, min_p = 1.0, max_sigma = 0.0;
  double max_transmission = 0.0, max_normalization = 0.0;
  long long evaluations = 0, capped = 0;
  for (const Result& r : results) {
    if (grid.size() == 1) {
      std::printf("%s: evaluations %lld, capped %lld\n", grid[0].Name().c_str(),
          r.evaluations, r.capped);
    }
    for (const std::string& failure : r.failures) std::printf("FAIL %s\n", failure.c_str());
    failed += r.failures.empty() ? 0 : 1;
    max_pdf_error = std::max(max_pdf_error, r.max_pdf_error);
    max_ratio_error = std::max(max_ratio_error, r.max_ratio_error);
    max_transmission = std::max(max_transmission, r.max_transmission_error);
    if (r.statistics) {
      min_p = std::min(min_p, r.chi2_p);
      max_sigma = std::max(max_sigma, r.max_estimator_sigma);
      max_normalization =
          std::max(max_normalization, std::abs(r.normalization - r.success_rate));
    }
    evaluations += r.evaluations;
    capped += r.capped;
  }
  std::printf("lotus-bsdf-test: %zu configurations (%d with statistics), %d samples each\n",
      grid.size(), statistics_configs, kSamples);
  std::printf("  1 pointwise: max relative pdf error %.3g, max mixture-ratio error %.3g\n",
      max_pdf_error, max_ratio_error);
  std::printf("  2 estimator: max |mean - integral| / se %.2f\n", max_sigma);
  std::printf("  3 normalization: max |integral - success rate| %.3g\n", max_normalization);
  std::printf("  4 chi-squared: min p %.3g (threshold %.3g)\n", min_p, chi2_significance);
  std::printf("  6 transmission: max relative error %.3g\n", max_transmission);
  std::printf("  quadrature: %lld evaluations, %lld cells at the depth limit\n",
      evaluations, capped);
  if (failed > 0) {
    std::printf("lotus-bsdf-test: %d configurations failed\n", failed);
    return 1;
  }
  std::printf("lotus-bsdf-test: passed\n");
  return 0;
}
