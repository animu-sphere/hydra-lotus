// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cmath>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace LotusHeadless {

// One independent sequence of RGB residuals, in fixed screen coordinates.
// Storage is O(image size), regardless of the number of frames.
class TemporalResidualStatistics {
public:
  explicit TemporalResidualStatistics(std::size_t components) : moments_(components) {
    if (!components)
      throw std::invalid_argument("empty temporal residual image");
  }

  void Add(std::span<const double> residual) {
    if (residual.size() != moments_.size())
      throw std::invalid_argument("temporal residual extent mismatch");
    for (double value : residual)
      if (!std::isfinite(value))
        throw std::invalid_argument("nonfinite temporal residual");
    ++frames_;
    for (std::size_t i = 0; i < residual.size(); ++i) {
      auto& m = moments_[i];
      const double value = residual[i];
      const double difference = value - m.mean;
      m.mean += difference / frames_;
      m.m2 += difference * (value - m.mean);
      if (frames_ == 1)
        m.first = value;
      else
        m.delta_squared += (value - m.previous) * (value - m.previous);
      m.previous = value;
    }
  }

  double MeanRmse() const {
    RequireFrames();
    double sum = 0;
    for (const auto& m : moments_)
      sum += m.mean * m.mean;
    return std::sqrt(sum / moments_.size());
  }

  double CenteredRmse() const {
    RequireFrames();
    return std::sqrt(CenteredSum() / (double(frames_) * moments_.size()));
  }

  std::optional<double> Lag1Autocorrelation() const {
    RequireFrames();
    const double denominator = CenteredSum();
    if (frames_ < 2 || denominator == 0)
      return std::nullopt;
    // sum (r[t]-mean)(r[t-1]-mean), using stable differences rather
    // than subtracting large raw second moments. Denominator uses all frames.
    double numerator = 0;
    for (const auto& m : moments_) {
      const double first = m.first - m.mean, last = m.previous - m.mean;
      numerator += m.m2 - 0.5 * (first * first + last * last + m.delta_squared);
    }
    return numerator / denominator;
  }

private:
  struct Moments {
    double mean = 0, m2 = 0, first = 0, previous = 0, delta_squared = 0;
  };
  void RequireFrames() const {
    if (!frames_)
      throw std::logic_error("no temporal residual frames");
  }
  double CenteredSum() const {
    double sum = 0;
    for (const auto& m : moments_)
      sum += m.m2;
    return sum;
  }
  std::vector<Moments> moments_;
  unsigned frames_ = 0;
};
} // namespace LotusHeadless
