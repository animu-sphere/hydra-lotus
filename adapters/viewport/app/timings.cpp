// SPDX-License-Identifier: Apache-2.0
#include "app/timings.hpp"

#include <algorithm>

namespace Lotus::viewport {
namespace {

bool SamePasses(const FrameTimes& a, const FrameTimes& b) {
  for (std::size_t pass = 0; pass < kGpuPassCount; ++pass) {
    if ((a.passes[pass] >= 0.0) != (b.passes[pass] >= 0.0)) {
      return false;
    }
  }
  return (a.gpu_ms >= 0.0) == (b.gpu_ms >= 0.0);
}

// Appends to a ring of `capacity` entries, `next` being the oldest once it
// is full.
template <typename Value>
void Push(std::vector<Value>& ring, std::size_t& next, std::size_t capacity,
    const Value& value) {
  if (ring.size() < capacity) {
    ring.push_back(value);
    return;
  }
  ring[next] = value;
  next = (next + 1) % capacity;
}

// One component of the history, oldest first.
std::vector<float> Ordered(const std::vector<std::array<float, 2>>& ring,
    std::size_t next, std::size_t component) {
  std::vector<float> ordered;
  ordered.reserve(ring.size());
  for (std::size_t index = 0; index < ring.size(); ++index) {
    ordered.push_back(ring[(next + index) % ring.size()][component]);
  }
  return ordered;
}

} // namespace

// The scene pass's time covers the wavefront kernels, the reservoir passes
// and, under reuse, the temporal surfaces that precede them; the temporal
// passes otherwise, and radiance accumulation, variance and the filter
// always, are a later submission.
double FrameGpuMs(const GpuFrameEvidence& frame) {
  double total = frame.primary_ray_gpu_ms;
  const TemporalProducts& temporal = frame.temporal;
  if (temporal.timestamp_available) {
    const bool reuse = frame.restir.temporal_row_pitch != 0;
    if (!reuse) {
      total += temporal.primary_surface_gpu_ms + temporal.reprojection_gpu_ms;
    }
    total += temporal.accumulation_gpu_ms + temporal.variance_gpu_ms +
             temporal.filter_gpu_ms;
  }
  return total;
}

bool PartOfPathTrace(GpuPass pass) noexcept {
  return pass == GpuPass::WavefrontCamera ||
         pass == GpuPass::WavefrontIntersect ||
         pass == GpuPass::WavefrontShade || pass == GpuPass::WavefrontTail;
}

std::string_view Name(GpuPass pass) {
  switch (pass) {
  case GpuPass::PathTrace:
    return "path_trace";
  case GpuPass::WavefrontCamera:
    return "wavefront_camera";
  case GpuPass::WavefrontIntersect:
    return "wavefront_intersect";
  case GpuPass::WavefrontShade:
    return "wavefront_shade";
  case GpuPass::WavefrontTail:
    return "wavefront_tail";
  case GpuPass::RestirInitial:
    return "restir_initial";
  case GpuPass::RestirTemporal:
    return "restir_temporal";
  case GpuPass::RestirSpatial:
    return "restir_spatial";
  case GpuPass::TemporalSurfaces:
    return "temporal_surfaces";
  case GpuPass::TemporalAccumulation:
    return "temporal_accumulation";
  case GpuPass::TemporalVariance:
    return "temporal_variance";
  case GpuPass::TemporalFilter:
    return "temporal_filter";
  }
  return "?";
}

FrameTimes MeasureFrame(const GpuFrameEvidence& frame, double frame_ms) {
  FrameTimes times;
  times.frame_ms = frame_ms;
  times.passes.fill(kNotRun);
  if (!frame.primary_ray_timestamp_available) {
    return times;
  }
  times.gpu_ms = FrameGpuMs(frame);
  const auto set = [&times](GpuPass pass, double ms) {
    times.passes[static_cast<std::size_t>(pass)] = std::max(ms, 0.0);
  };
  double path_trace = frame.primary_ray_gpu_ms;
  const RestirProducts& restir = frame.restir;
  const bool reuse = restir.temporal_row_pitch != 0;
  if (restir.available && restir.timestamp_available) {
    set(GpuPass::RestirInitial, restir.initial_gpu_ms);
    path_trace -= restir.initial_gpu_ms;
    // Spatial reuse alone resolves the records through the temporal pass.
    if (reuse) {
      set(GpuPass::RestirTemporal, restir.temporal_gpu_ms);
      path_trace -= restir.temporal_gpu_ms;
    }
    if (restir.spatial_row_pitch != 0) {
      set(GpuPass::RestirSpatial, restir.spatial_gpu_ms);
      path_trace -= restir.spatial_gpu_ms;
    }
  }
  const TemporalProducts& temporal = frame.temporal;
  if (temporal.available && temporal.timestamp_available) {
    const double surfaces =
        temporal.primary_surface_gpu_ms + temporal.reprojection_gpu_ms;
    set(GpuPass::TemporalSurfaces, surfaces);
    if (reuse) {
      path_trace -= surfaces;
    }
    if (temporal.radiance_available) {
      set(GpuPass::TemporalAccumulation, temporal.accumulation_gpu_ms);
      set(GpuPass::TemporalVariance, temporal.variance_gpu_ms);
    }
    if (temporal.filter_iterations != 0) {
      set(GpuPass::TemporalFilter, temporal.filter_gpu_ms);
    }
  }
  set(GpuPass::PathTrace, path_trace);
  const WavefrontTimings& wavefront = frame.wavefront_timings;
  if (wavefront.available) {
    set(GpuPass::WavefrontCamera, wavefront.camera_gpu_ms);
    set(GpuPass::WavefrontIntersect, wavefront.intersect_gpu_ms);
    set(GpuPass::WavefrontShade, wavefront.shade_gpu_ms);
    set(GpuPass::WavefrontTail, wavefront.tail_gpu_ms);
  }
  return times;
}

void TimingRecord::Add(const GpuFrameEvidence& frame,
    const GpuSceneTimings& update, const GpuSceneStats& scene,
    double frame_ms) {
  ++frames_;
  latest_ = MeasureFrame(frame, frame_ms);
  if (!window_.empty() && !SamePasses(window_.front(), latest_)) {
    window_.clear();
    window_next_ = 0;
  }
  Push(window_, window_next_, kAverageFrames, latest_);
  Push(history_, history_next_, kHistoryFrames,
      std::array<float, 2>{static_cast<float>(frame_ms),
          static_cast<float>(std::max(latest_.gpu_ms, 0.0))});
  for (std::size_t pass = 0; pass < kGpuPassCount; ++pass) {
    if (latest_.passes[pass] >= 0.0) {
      pass_totals_[pass] += latest_.passes[pass];
      ++pass_frames_[pass];
    }
  }

  if (update.available) {
    scene_update_.available = true;
    scene_update_.frame = frames_;
    scene_update_.upload_ms = update.upload_gpu_ms;
    scene_update_.blas_ms = update.blas_build_gpu_ms;
    scene_update_.tlas_ms = update.tlas_build_gpu_ms;
    ++scene_update_.count;
  }

  const RestirProducts& restir = frame.restir;
  memory_.reservoir_current = restir.available;
  if (restir.available) {
    memory_.reservoir_bytes = restir.reservoir_bytes;
  }
  const TemporalProducts& temporal = frame.temporal;
  memory_.surface_history_current = temporal.available;
  if (temporal.available) {
    memory_.surface_history_bytes = temporal.surface_bytes +
                                    temporal.validity_bytes +
                                    temporal.metadata_bytes;
  }
  memory_.radiance_history_current = temporal.radiance_available;
  if (temporal.radiance_available) {
    memory_.radiance_history_bytes = temporal.radiance_bytes;
  }
  memory_.filter_current = temporal.filter_iterations != 0;
  if (temporal.filter_bytes != 0) {
    memory_.filter_bytes = temporal.filter_bytes;
  }
  memory_.scene_used_bytes = scene.memory.used_bytes;
  memory_.scene_reserved_bytes = scene.memory.reserved_bytes;
  memory_.acceleration_bytes = scene.acceleration_bytes;
}

FrameTimes TimingRecord::Mean() const {
  FrameTimes mean = latest_;
  if (window_.empty()) {
    return mean;
  }
  const double count = static_cast<double>(window_.size());
  mean.frame_ms = 0.0;
  for (const FrameTimes& frame : window_) {
    mean.frame_ms += frame.frame_ms / count;
  }
  if (latest_.gpu_ms >= 0.0) {
    mean.gpu_ms = 0.0;
    for (const FrameTimes& frame : window_) {
      mean.gpu_ms += frame.gpu_ms / count;
    }
  }
  for (std::size_t pass = 0; pass < kGpuPassCount; ++pass) {
    if (latest_.passes[pass] < 0.0) {
      continue;
    }
    mean.passes[pass] = 0.0;
    for (const FrameTimes& frame : window_) {
      mean.passes[pass] += frame.passes[pass] / count;
    }
  }
  return mean;
}

std::vector<float> TimingRecord::FrameHistory() const {
  return Ordered(history_, history_next_, 0);
}

std::vector<float> TimingRecord::GpuHistory() const {
  return Ordered(history_, history_next_, 1);
}

} // namespace Lotus::viewport
