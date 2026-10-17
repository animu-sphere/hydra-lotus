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

  unsigned Frames() const { return frames_; }
  std::size_t Components() const { return moments_.size(); }
  double Mean(std::size_t component) const { return moments_.at(component).mean; }
  // Sample variance about this sequence's own mean (T - 1 denominator).
  double Variance(std::size_t component) const {
    if (frames_ < 2)
      throw std::logic_error("temporal residual variance needs two frames");
    return moments_.at(component).m2 / (frames_ - 1);
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

// Static residuals of independent, equal-length sequences. Variation of the
// sequence means across sequences is free of within-sequence correlation, so
// it measures both the variance inflation that correlation causes and the
// noise expected in the grand mean. Storage is O(image size).
class SequenceEnsembleStatistics {
public:
  struct Summary {
    unsigned sequences = 0, frames = 0;
    double frame_variance = 0, sequence_mean_variance = 0, grand_mean_mse = 0;
    // T * Var(sequence mean) / Var(frame); one for uncorrelated frames.
    std::optional<double> variance_inflation;
  };

  explicit SequenceEnsembleStatistics(std::size_t components) : moments_(components) {
    if (!components)
      throw std::invalid_argument("empty sequence ensemble image");
  }

  void Add(const TemporalResidualStatistics& sequence) {
    if (sequence.Components() != moments_.size())
      throw std::invalid_argument("sequence ensemble extent mismatch");
    if (sequence.Frames() < 2 || (sequences_ && sequence.Frames() != frames_))
      throw std::invalid_argument("sequence ensemble needs equal sequences of at least two frames");
    frames_ = sequence.Frames();
    ++sequences_;
    for (std::size_t i = 0; i < moments_.size(); ++i) {
      auto& m = moments_[i];
      const double value = sequence.Mean(i);
      const double difference = value - m.mean;
      m.mean += difference / sequences_;
      m.m2 += difference * (value - m.mean);
      m.frame_variance += sequence.Variance(i);
    }
  }

  Summary Summarize() const {
    if (sequences_ < 2)
      throw std::logic_error("sequence ensemble needs two sequences");
    Summary result;
    result.sequences = sequences_;
    result.frames = frames_;
    for (const auto& m : moments_) {
      result.frame_variance += m.frame_variance / sequences_;
      result.sequence_mean_variance += m.m2 / (sequences_ - 1);
      result.grand_mean_mse += m.mean * m.mean;
    }
    const double n = double(moments_.size());
    result.frame_variance /= n;
    result.sequence_mean_variance /= n;
    result.grand_mean_mse /= n;
    if (result.frame_variance > 0)
      result.variance_inflation = frames_ * result.sequence_mean_variance / result.frame_variance;
    return result;
  }

private:
  struct Moments {
    double mean = 0, m2 = 0, frame_variance = 0;
  };
  std::vector<Moments> moments_;
  unsigned sequences_ = 0, frames_ = 0;
};
} // namespace LotusHeadless
