// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
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

// Interleaved RGB residual images of independent replications that share one
// reference: sequence means (static) or one pose's frame from each sequence
// (moving). Compares the replications' mean error with its expected noise,
// pooled over the image, within column bands and for each tile's mean.
// Tile means are replicated directly, so correlation between neighbouring
// pixels enters their variance. Storage is O(image size).
class ReplicationBiasStatistics {
public:
  struct Band {
    unsigned begin = 0, end = 0; // Columns [begin, end), all rows.
  };
  struct Error {
    double mse = 0, noise = 0; // Means over components.
  };
  struct Tile {
    unsigned x = 0, y = 0, channel = 0; // Tile column/row, RGB channel.
    double z = 0;
  };
  struct Summary {
    unsigned replications = 0;
    Error image;
    std::vector<Error> bands;
    unsigned tile_tests = 0, tiles_beyond_3 = 0;
    std::optional<Tile> max_tile; // Largest |z|; null without a defined test.
  };

  ReplicationBiasStatistics(unsigned width, unsigned height, unsigned tile = 8)
      : width_(width), height_(height), tile_(tile),
        tiles_x_(tile ? (width + tile - 1) / tile : 0), tiles_y_(tile ? (height + tile - 1) / tile : 0),
        pixels_(std::size_t{width} * height * 3), tiles_(std::size_t{tiles_x_} * tiles_y_ * 3) {
    if (!width || !height || !tile)
      throw std::invalid_argument("empty replication image");
  }

  void Add(std::span<const double> residual) {
    if (residual.size() != pixels_.size())
      throw std::invalid_argument("replication extent mismatch");
    for (double value : residual)
      if (!std::isfinite(value))
        throw std::invalid_argument("nonfinite replication residual");
    std::vector<double> tiles(tiles_.size());
    for (unsigned y = 0; y < height_; ++y)
      for (unsigned x = 0; x < width_; ++x)
        for (unsigned c = 0; c < 3; ++c)
          tiles[TileIndex(x / tile_, y / tile_, c)] += residual[(std::size_t{y} * width_ + x) * 3 + c];
    ++replications_;
    for (std::size_t i = 0; i < pixels_.size(); ++i)
      pixels_[i].Add(residual[i], replications_);
    for (unsigned ty = 0; ty < tiles_y_; ++ty)
      for (unsigned tx = 0; tx < tiles_x_; ++tx)
        for (unsigned c = 0; c < 3; ++c) {
          const auto i = TileIndex(tx, ty, c);
          tiles_[i].Add(tiles[i] / TilePixels(tx, ty), replications_);
        }
  }

  // reference_variance holds each component's variance in the shared
  // reference; empty means exact. Reference pixels are assumed independent.
  Summary Summarize(std::span<const double> reference_variance, std::span<const Band> bands) const {
    if (replications_ < 2)
      throw std::logic_error("replication bias needs two replications");
    if (!reference_variance.empty() && reference_variance.size() != pixels_.size())
      throw std::invalid_argument("reference variance extent mismatch");
    auto reference = [&](std::size_t i) { return reference_variance.empty() ? 0.0 : reference_variance[i]; };
    auto noise = [&](const Moments& m) { return m.m2 / (replications_ - 1) / replications_; };
    Summary result;
    result.replications = replications_;
    for (std::size_t i = 0; i < pixels_.size(); ++i) {
      result.image.mse += pixels_[i].mean * pixels_[i].mean;
      result.image.noise += noise(pixels_[i]) + reference(i);
    }
    result.image.mse /= pixels_.size();
    result.image.noise /= pixels_.size();
    for (const auto& band : bands) {
      if (band.begin >= band.end || band.end > width_)
        throw std::invalid_argument("invalid replication band");
      Error error;
      for (unsigned y = 0; y < height_; ++y)
        for (unsigned x = band.begin; x < band.end; ++x)
          for (unsigned c = 0; c < 3; ++c) {
            const auto i = (std::size_t{y} * width_ + x) * 3 + c;
            error.mse += pixels_[i].mean * pixels_[i].mean;
            error.noise += noise(pixels_[i]) + reference(i);
          }
      const double components = 3.0 * height_ * (band.end - band.begin);
      error.mse /= components;
      error.noise /= components;
      result.bands.push_back(error);
    }
    for (unsigned ty = 0; ty < tiles_y_; ++ty)
      for (unsigned tx = 0; tx < tiles_x_; ++tx)
        for (unsigned c = 0; c < 3; ++c) {
          double shared = 0;
          for (unsigned y = ty * tile_; y < std::min(height_, (ty + 1) * tile_); ++y)
            for (unsigned x = tx * tile_; x < std::min(width_, (tx + 1) * tile_); ++x)
              shared += reference((std::size_t{y} * width_ + x) * 3 + c);
          const double pixels = TilePixels(tx, ty);
          const auto& m = tiles_[TileIndex(tx, ty, c)];
          const double variance = noise(m) + shared / (pixels * pixels);
          if (variance <= 0)
            continue;
          const double z = m.mean / std::sqrt(variance);
          ++result.tile_tests;
          result.tiles_beyond_3 += std::abs(z) > 3;
          if (!result.max_tile || std::abs(z) > std::abs(result.max_tile->z))
            result.max_tile = Tile{tx, ty, c, z};
        }
    return result;
  }

  unsigned Replications() const { return replications_; }

private:
  struct Moments {
    double mean = 0, m2 = 0;
    void Add(double value, unsigned count) {
      const double difference = value - mean;
      mean += difference / count;
      m2 += difference * (value - mean);
    }
  };
  std::size_t TileIndex(unsigned tx, unsigned ty, unsigned c) const {
    return (std::size_t{ty} * tiles_x_ + tx) * 3 + c;
  }
  double TilePixels(unsigned tx, unsigned ty) const {
    return double(std::min(width_, (tx + 1) * tile_) - tx * tile_) *
           (std::min(height_, (ty + 1) * tile_) - ty * tile_);
  }
  unsigned width_, height_, tile_, tiles_x_, tiles_y_;
  std::vector<Moments> pixels_, tiles_;
  unsigned replications_ = 0;
};

// Two residual images per independent replication: one sequence's frames at a
// fixed pair of poses. Centering each component across replications removes
// persistent error and the poses' shared reference noise, so what remains
// correlates only their sampling noise. Storage is O(image size).
class ReplicationCorrelationStatistics {
public:
  // Co-moment and second moments about the replication means, summed over
  // components. Sums from several pose pairs pool by addition.
  struct Sums {
    double cross = 0, first = 0, second = 0;
  };

  explicit ReplicationCorrelationStatistics(std::size_t components) : moments_(components) {
    if (!components)
      throw std::invalid_argument("empty replication correlation image");
  }

  void Add(std::span<const double> first, std::span<const double> second) {
    if (first.size() != moments_.size() || second.size() != moments_.size())
      throw std::invalid_argument("replication correlation extent mismatch");
    for (std::size_t i = 0; i < moments_.size(); ++i)
      if (!std::isfinite(first[i]) || !std::isfinite(second[i]))
        throw std::invalid_argument("nonfinite replication correlation residual");
    ++replications_;
    for (std::size_t i = 0; i < moments_.size(); ++i) {
      auto& m = moments_[i];
      const double a = first[i] - m.first, b = second[i] - m.second;
      m.first += a / replications_;
      m.second += b / replications_;
      m.cross += a * (second[i] - m.second);
      m.first_m2 += a * (first[i] - m.first);
      m.second_m2 += b * (second[i] - m.second);
    }
  }

  Sums Summarize() const {
    if (replications_ < 2)
      throw std::logic_error("replication correlation needs two replications");
    Sums result;
    for (const auto& m : moments_) {
      result.cross += m.cross;
      result.first += m.first_m2;
      result.second += m.second_m2;
    }
    return result;
  }

  // Pooled correlation; null when either side has no variance.
  static std::optional<double> Correlation(const Sums& sums) {
    if (sums.first <= 0 || sums.second <= 0)
      return std::nullopt;
    return sums.cross / std::sqrt(sums.first * sums.second);
  }

  unsigned Replications() const { return replications_; }

private:
  struct Moments {
    double first = 0, second = 0, cross = 0, first_m2 = 0, second_m2 = 0;
  };
  std::vector<Moments> moments_;
  unsigned replications_ = 0;
};
} // namespace LotusHeadless
