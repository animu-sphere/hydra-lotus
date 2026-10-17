// SPDX-License-Identifier: Apache-2.0
// lotus-viewport's timing HUD figures from synthetic frame evidence, without
// a window or a GPU.
#include "../adapters/viewport/timings.hpp"

#include <cmath>
#include <iostream>
#include <string>

namespace {

using Lotus::GpuFrameEvidence;
using Lotus::GpuSceneStats;
using Lotus::GpuSceneTimings;
using Lotus::viewport::FrameGpuMs;
using Lotus::viewport::FrameTimes;
using Lotus::viewport::GpuPass;
using Lotus::viewport::kGpuPassCount;
using Lotus::viewport::MeasureFrame;
using Lotus::viewport::PartOfPathTrace;
using Lotus::viewport::TimingRecord;

int failures = 0;

void Check(bool condition, const std::string& what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
  }
}

bool Near(double a, double b) {
  return std::abs(a - b) < 1e-9;
}

// The passes that are not part of the path trace add up to the frame's GPU
// time.
double PassSum(const FrameTimes& times) {
  double sum = 0.0;
  for (std::size_t index = 0; index < kGpuPassCount; ++index) {
    const auto pass = static_cast<GpuPass>(index);
    if (times.ran(pass) && !PartOfPathTrace(pass)) {
      sum += times.pass(pass);
    }
  }
  return sum;
}

GpuFrameEvidence SceneFrame(double scene_ms) {
  GpuFrameEvidence frame;
  frame.primary_ray_timestamp_available = true;
  frame.primary_ray_gpu_ms = scene_ms;
  return frame;
}

// Combined reservoir reuse: the reservoir passes and the temporal surfaces
// run inside the scene pass.
GpuFrameEvidence CombinedFrame() {
  GpuFrameEvidence frame = SceneFrame(2.0);
  frame.restir.available = true;
  frame.restir.timestamp_available = true;
  frame.restir.temporal_row_pitch = 64;
  frame.restir.spatial_row_pitch = 64;
  frame.restir.initial_gpu_ms = 0.3;
  frame.restir.temporal_gpu_ms = 0.2;
  frame.restir.spatial_gpu_ms = 0.25;
  frame.restir.reservoir_bytes = 4096;
  frame.temporal.available = true;
  frame.temporal.timestamp_available = true;
  frame.temporal.primary_surface_gpu_ms = 0.1;
  frame.temporal.reprojection_gpu_ms = 0.05;
  frame.temporal.surface_bytes = 1000;
  frame.temporal.validity_bytes = 200;
  frame.temporal.metadata_bytes = 30;
  return frame;
}

} // namespace

int main() {
  {
    GpuFrameEvidence untimed;
    untimed.primary_ray_gpu_ms = 3.0;
    const FrameTimes times = MeasureFrame(untimed, 16.0);
    bool any = false;
    for (std::size_t index = 0; index < kGpuPassCount; ++index) {
      any = any || times.ran(static_cast<GpuPass>(index));
    }
    Check(times.frame_ms == 16.0 && times.gpu_ms < 0.0 && !any,
        "a frame without timestamps has a host time and no GPU passes");
  }
  {
    const FrameTimes times = MeasureFrame(SceneFrame(0.4), 1.0);
    Check(Near(times.gpu_ms, 0.4) && Near(times.pass(GpuPass::PathTrace), 0.4),
        "NEE/MIS: the scene pass is the path trace");
    Check(!times.ran(GpuPass::RestirInitial) &&
              !times.ran(GpuPass::TemporalSurfaces) &&
              !times.ran(GpuPass::WavefrontCamera),
        "NEE/MIS runs no other pass");
  }
  {
    GpuFrameEvidence frame = SceneFrame(1.0);
    frame.wavefront_timings = {true, 0.1, 0.4, 0.3, 0.05};
    const FrameTimes times = MeasureFrame(frame, 2.0);
    Check(Near(times.pass(GpuPass::PathTrace), 1.0) &&
              Near(times.pass(GpuPass::WavefrontIntersect), 0.4) &&
              Near(times.pass(GpuPass::WavefrontTail), 0.05),
        "wavefront kernels are parts of the path trace");
    Check(Near(PassSum(times), times.gpu_ms),
        "wavefront kernels are not counted twice");
  }
  {
    const FrameTimes times = MeasureFrame(CombinedFrame(), 3.0);
    Check(Near(times.gpu_ms, 2.0), "combined reuse: one scene submission");
    Check(Near(times.pass(GpuPass::PathTrace), 2.0 - 0.75 - 0.15),
        "combined reuse: the path trace excludes the reservoir passes and "
        "the temporal surfaces");
    Check(Near(times.pass(GpuPass::RestirSpatial), 0.25) &&
              Near(times.pass(GpuPass::TemporalSurfaces), 0.15) &&
              !times.ran(GpuPass::TemporalAccumulation),
        "combined reuse reports its passes");
    Check(Near(PassSum(times), times.gpu_ms),
        "combined reuse: the passes add up to the GPU time");
  }
  {
    GpuFrameEvidence frame = CombinedFrame();
    frame.restir.temporal_row_pitch = 0;
    frame.restir.spatial_row_pitch = 0;
    frame.temporal = {};
    const FrameTimes times = MeasureFrame(frame, 3.0);
    Check(times.ran(GpuPass::RestirInitial) &&
              !times.ran(GpuPass::RestirTemporal) &&
              !times.ran(GpuPass::RestirSpatial) &&
              Near(times.pass(GpuPass::PathTrace), 1.7),
        "initial-only reservoirs run one reservoir pass");
  }
  {
    GpuFrameEvidence frame = SceneFrame(0.5);
    frame.temporal.available = true;
    frame.temporal.timestamp_available = true;
    frame.temporal.radiance_available = true;
    frame.temporal.primary_surface_gpu_ms = 0.1;
    frame.temporal.reprojection_gpu_ms = 0.05;
    frame.temporal.accumulation_gpu_ms = 0.2;
    const FrameTimes times = MeasureFrame(frame, 3.0);
    Check(Near(times.gpu_ms, FrameGpuMs(frame)) && Near(times.gpu_ms, 0.85),
        "temporal radiance: the temporal submission follows the scene pass");
    Check(Near(times.pass(GpuPass::PathTrace), 0.5) &&
              Near(times.pass(GpuPass::TemporalAccumulation), 0.2) &&
              Near(PassSum(times), times.gpu_ms),
        "temporal radiance: the passes add up to the GPU time");
  }

  {
    TimingRecord record;
    Check(record.frames() == 0 && !record.scene_update().available &&
              record.FrameHistory().empty(),
        "an empty record");
    GpuSceneTimings upload;
    upload.available = true;
    upload.upload_gpu_ms = 1.5;
    upload.blas_build_gpu_ms = 0.5;
    upload.tlas_build_gpu_ms = 0.1;
    GpuSceneStats scene;
    scene.memory.used_bytes = 900;
    scene.memory.reserved_bytes = 1024;
    scene.acceleration_bytes = 300;
    record.Add(SceneFrame(0.2), upload, scene, 10.0);
    record.Add(SceneFrame(0.4), {}, scene, 20.0);
    Check(record.mean_frames() == 2 && Near(record.Mean().frame_ms, 15.0) &&
              Near(record.Mean().pass(GpuPass::PathTrace), 0.3),
        "the mean covers frames with the same passes");
    Check(record.scene_update().available && record.scene_update().frame == 1 &&
              record.scene_update().count == 1 &&
              Near(record.scene_update().upload_ms, 1.5),
        "an update without GPU work keeps the latest one");
    Check(record.memory().scene_used_bytes == 900 &&
              record.memory().acceleration_bytes == 300 &&
              record.memory().reservoir_bytes == 0,
        "scene memory");

    record.Add(CombinedFrame(), {}, scene, 30.0);
    Check(record.mean_frames() == 1 && Near(record.Mean().frame_ms, 30.0),
        "a change of passes starts the mean again");
    Check(record.memory().reservoir_bytes == 4096 &&
              record.memory().reservoir_current &&
              record.memory().surface_history_bytes == 1230 &&
              record.memory().surface_history_current &&
              !record.memory().radiance_history_current,
        "reuse memory");
    record.Add(SceneFrame(0.2), {}, scene, 10.0);
    Check(record.memory().reservoir_bytes == 4096 &&
              !record.memory().reservoir_current &&
              !record.memory().surface_history_current,
        "unused reuse memory is retained, not current");
    Check(record.pass_frames()[static_cast<std::size_t>(GpuPass::PathTrace)] ==
                  4 &&
              record.pass_frames()[static_cast<std::size_t>(
                  GpuPass::RestirInitial)] == 1 &&
              Near(record.pass_totals()[static_cast<std::size_t>(
                       GpuPass::RestirInitial)],
                  0.3),
        "run totals count each pass over the frames that ran it");

    for (int frame = 0; frame < 200; ++frame) {
      record.Add(SceneFrame(0.2), {}, scene, static_cast<double>(frame));
    }
    const std::vector<float> history = record.FrameHistory();
    Check(history.size() == TimingRecord::kHistoryFrames &&
              history.front() == 80.0F && history.back() == 199.0F,
        "the history keeps the latest frames, oldest first");
    Check(record.GpuHistory().size() == TimingRecord::kHistoryFrames &&
              record.GpuHistory().back() == 0.2F,
        "the GPU history follows the frame history");
    Check(record.mean_frames() == TimingRecord::kAverageFrames &&
              Near(record.Mean().frame_ms, (168.0 + 199.0) / 2.0),
        "the mean covers the latest frames");
  }

  if (failures != 0) {
    std::cerr << failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "viewport timings: all checks passed\n";
  return 0;
}
