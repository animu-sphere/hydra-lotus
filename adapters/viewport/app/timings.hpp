// SPDX-License-Identifier: Apache-2.0
// The timing HUD's figures: what each frame cost on the host and on the GPU,
// pass by pass, what the latest scene update cost, and the renderer memory
// the reuse paths and the scene hold. GPU-, window- and USD-free, so that a
// unit test covers them; the developer UI only draws a TimingRecord, and the
// benchmark JSON writes its averages.
#pragma once

#include <lotus/vulkan_backend.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace Lotus::viewport {

// GPU time of every submission the frame timed: the scene pass, which
// covers the wavefront kernels, the reservoir passes and, under reservoir
// reuse, the temporal surfaces that precede them; and the temporal passes
// submitted after it.
[[nodiscard]] double FrameGpuMs(const GpuFrameEvidence& frame);

// The GPU passes the HUD names. The path trace is the scene pass without
// the reservoir passes and the temporal surfaces recorded in it; the
// wavefront kernels are part of it. Passes after the path trace add up to
// FrameGpuMs with it.
enum class GpuPass : std::size_t {
  PathTrace,
  WavefrontCamera,
  WavefrontIntersect,
  WavefrontShade,
  WavefrontTail,
  RestirInitial,
  RestirTemporal,
  RestirSpatial,
  // Primary surfaces and reprojection.
  TemporalSurfaces,
  TemporalAccumulation,
  TemporalVariance,
  // The denoiser's a-trous iterations and composite.
  TemporalFilter,
};
inline constexpr std::size_t kGpuPassCount = 12;

// Whether the pass is a part of the path trace rather than a pass of its own.
[[nodiscard]] bool PartOfPathTrace(GpuPass pass) noexcept;
[[nodiscard]] std::string_view Name(GpuPass pass);

inline constexpr double kNotRun = -1.0;

// One frame. A pass that did not run, or ran without timestamps, holds
// kNotRun; gpu_ms is kNotRun without scene-pass timestamps.
struct FrameTimes {
  // The host's wall time to render and present the frame, the developer
  // UI included.
  double frame_ms = 0.0;
  double gpu_ms = kNotRun;
  std::array<double, kGpuPassCount> passes{};

  [[nodiscard]] double pass(GpuPass which) const noexcept {
    return passes[static_cast<std::size_t>(which)];
  }
  [[nodiscard]] bool ran(GpuPass which) const noexcept {
    return pass(which) >= 0.0;
  }
};

[[nodiscard]] FrameTimes MeasureFrame(const GpuFrameEvidence& frame,
    double frame_ms);

// The latest scene update that submitted GPU work: its upload, BLAS and
// TLAS phases, and the frame of the run (counted from 1) it preceded.
struct SceneUpdateTimes {
  bool available = false;
  std::uint64_t frame = 0;
  double upload_ms = 0.0;
  double blas_ms = 0.0;
  double tlas_ms = 0.0;
  // Updates with GPU work in the run.
  std::uint64_t count = 0;
};

// Renderer memory, as the latest frame that used it reported it. The
// backend keeps reuse buffers allocated after the controls stop using them,
// so a category the latest frame did not use keeps its last size and reads
// as retained.
struct MemoryUse {
  // Device reservoirs and their host mirrors (RestirProducts).
  std::uint64_t reservoir_bytes = 0;
  bool reservoir_current = false;
  // Temporal surfaces, validity and metadata, with their host mirrors.
  std::uint64_t surface_history_bytes = 0;
  bool surface_history_current = false;
  // Radiance history, its host mirror and the fresh batch.
  std::uint64_t radiance_history_bytes = 0;
  bool radiance_history_current = false;
  // The denoiser's ping-pong buffer, allocated by the first filtered frame.
  std::uint64_t filter_bytes = 0;
  bool filter_current = false;
  // The scene's backing memory (GpuMemoryStats) and its BLASes and TLAS.
  std::uint64_t scene_used_bytes = 0;
  std::uint64_t scene_reserved_bytes = 0;
  std::uint64_t acceleration_bytes = 0;
};

// What the HUD shows: the latest frame, the mean of the recent frames that
// ran the same passes, a history of frame and GPU times for the graph, the
// latest scene update and the memory.
class TimingRecord {
public:
  // Frames in the mean, and in the graph.
  static constexpr std::size_t kAverageFrames = 32;
  static constexpr std::size_t kHistoryFrames = 120;

  // After each frame: its evidence, the scene update before it and the
  // scene's state after it.
  void Add(const GpuFrameEvidence& frame, const GpuSceneTimings& update,
      const GpuSceneStats& scene, double frame_ms);

  [[nodiscard]] std::uint64_t frames() const noexcept {
    return frames_;
  }
  [[nodiscard]] const FrameTimes& latest() const noexcept {
    return latest_;
  }
  // Each figure's mean over the latest frames, up to kAverageFrames, since
  // the set of passes last changed; a mode switch starts it again.
  [[nodiscard]] FrameTimes Mean() const;
  [[nodiscard]] std::size_t mean_frames() const noexcept {
    return window_.size();
  }
  // Frame and GPU times, oldest first, at most kHistoryFrames; GPU entries
  // are 0 for frames without timestamps.
  [[nodiscard]] std::vector<float> FrameHistory() const;
  [[nodiscard]] std::vector<float> GpuHistory() const;
  [[nodiscard]] const SceneUpdateTimes& scene_update() const noexcept {
    return scene_update_;
  }
  [[nodiscard]] const MemoryUse& memory() const noexcept {
    return memory_;
  }
  // Means over every frame of the run, for the benchmark: each pass over
  // the frames that ran it, with how many did.
  [[nodiscard]] const std::array<double, kGpuPassCount>& pass_totals()
      const noexcept {
    return pass_totals_;
  }
  [[nodiscard]] const std::array<std::uint64_t, kGpuPassCount>& pass_frames()
      const noexcept {
    return pass_frames_;
  }

private:
  std::uint64_t frames_ = 0;
  FrameTimes latest_;
  std::vector<FrameTimes> window_;
  std::size_t window_next_ = 0;
  // Frame and GPU milliseconds.
  std::vector<std::array<float, 2>> history_;
  std::size_t history_next_ = 0;
  SceneUpdateTimes scene_update_;
  MemoryUse memory_;
  std::array<double, kGpuPassCount> pass_totals_{};
  std::array<std::uint64_t, kGpuPassCount> pass_frames_{};
};

} // namespace Lotus::viewport
