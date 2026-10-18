// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <lotus/render_world.hpp>

namespace Lotus {

enum class TemporalResetReason {
  None,
  FirstFrame,
  ExplicitReset,
  CameraCut,
  FrameGap,
  TargetChanged,
  HistoryKeyChanged,
  RenderSettingsChanged,
  SceneChanged,
};

struct TemporalValidationSettings {
  float absolute_depth_tolerance = 0.01F;
  float relative_depth_tolerance = 0.01F;
  float minimum_normal_cosine = 0.9F;
  float max_motion_pixels = 32.0F;
};

struct TemporalFrameSettings {
  // A rendered frame's identity, independent of scene revisions and spp.
  std::uint64_t frame_index = 0;
  // Image extent. Empty windows mean the whole image, with a top-left origin.
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  // Caller-owned compatibility token: change it for lighting, estimator or
  // other edits that make every pixel's radiance history incompatible.
  std::uint64_t history_key = 0;
  bool camera_cut = false;
  std::array<float, 4> display_window{};
  std::array<std::int32_t, 4> data_window{};
  TemporalValidationSettings validation;
  // Opt-in radiance reuse. Each call traces a fresh batch rather than adding
  // the same progressive mean repeatedly. Metadata-only remains the default.
  bool accumulate_radiance = false;
  // Effective samples retained per pixel, including the current batch.
  // Must be in [1, 65536] when accumulation is enabled.
  std::uint32_t max_history_samples = 32;
  // Edge-aware a-trous iterations over the accumulated radiance
  // (FilterTemporalRadiance); zero leaves the colour unfiltered. Requires
  // accumulate_radiance and at most kTemporalFilterMaxIterations. The
  // history stays unfiltered, so changing it never resets the stream.
  std::uint32_t filter_iterations = 0;
};

struct TemporalFrame {
  FrameSnapshot current;
  // Present only when the immediately preceding frame is compatible.
  std::optional<FrameSnapshot> previous;
  TemporalFrameSettings settings;
  TemporalResetReason reset_reason = TemporalResetReason::FirstFrame;
};

// CPU metadata for one temporal stream, independent of integrator or GPU
// capability. Advance once per successfully rendered frame, never per sample
// or scene commit. Snapshots retain the previous resources without copying or
// traversing the scene. Failed/abandoned renders must not advance this state.
class TemporalHistory {
public:
  // Empty extents and non-increasing frame indices throw invalid_argument
  // without changing history. A gap accepts the frame but drops its history.
  [[nodiscard]] TemporalFrame Advance(
      const FrameSnapshot& snapshot, const TemporalFrameSettings& settings);
  // Releases the retained snapshot and permits the frame index to restart.
  void Reset();

private:
  std::optional<FrameSnapshot> last_;
  TemporalFrameSettings last_settings_;
  bool explicit_reset_ = false;
};

struct TemporalPlacement {
  Matrix4 current_world_from_object = IdentityMatrix();
  Matrix4 previous_world_from_object = IdentityMatrix();
  // Compares the binding, effective material values and its texture resources.
  bool material_unchanged = false;
};

// Correspondence is the stable mesh key and placement ordinal, never a GPU
// table slot. Hidden/new/deformed meshes and changed instancing mode/count
// return nothing. Same-count instancer reordering needs an explicit Reset.
[[nodiscard]] std::optional<TemporalPlacement> FindTemporalPlacement(
    const TemporalFrame& frame, const std::string& mesh,
    std::size_t placement = 0);

struct TemporalReprojection {
  // Continuous pixel coordinates from the top-left edge: a pixel centre is
  // (x + 0.5, y + 0.5). Backward motion is previous_pixel - current_pixel.
  std::array<float, 2> current_pixel{};
  std::array<float, 2> previous_pixel{};
  std::array<float, 2> motion{};
  // Positive linear view depth of this surface in the previous camera.
  float previous_depth = 0.0F;
};

// Projects the same object-space surface point through both transforms and
// unjittered OpenGL-convention cameras. Nothing for missing history, non-finite
// input, behind-camera points, clipping or positions outside either frame.
[[nodiscard]] std::optional<TemporalReprojection> ReprojectTemporalPoint(
    const TemporalFrame& frame, const TemporalPlacement& placement,
    const std::array<float, 3>& object_position);

struct TemporalSurface {
  bool hit = false;
  // Stable identities supplied by the consumer, never transient table slots.
  std::uint64_t instance_id = 0;
  std::uint64_t material_id = 0;
  float linear_depth = 0.0F;
  // World-space normal; normalized by the validity function.
  std::array<float, 3> normal{};
};

// Numeric values are also the GPU debug AOV's rejection codes.
enum class TemporalRejection : std::uint32_t {
  None,
  Miss,
  InvalidData,
  InstanceChanged,
  MaterialChanged,
  LargeMotion,
  DepthDiscontinuity,
  NormalDiscontinuity,
  HistoryUnavailable,
  CorrespondenceMissing,
  ReprojectionOutside,
};

// Shared history validity: compare the fetched previous surface's depth with
// the reprojected expected depth, not the current camera's depth. Material
// resource edits must also pass TemporalPlacement::material_unchanged before
// reuse. No radiance, reservoirs or denoiser-specific state enters this test.
[[nodiscard]] TemporalRejection ValidateTemporalHistory(
    const TemporalReprojection& reprojection, const TemporalSurface& current,
    const TemporalSurface& previous,
    const TemporalValidationSettings& settings = {});

// Tightly packed top-left primary-surface products, one per pixel.
struct TemporalPixel {
  // Unit world geometric normal facing the camera; w: positive view depth.
  std::array<float, 4> normal_depth{};
  // xy: backward motion; z: expected previous depth; w: valid projection.
  std::array<float, 4> motion_depth{};
  std::uint64_t instance_id = 0;
  std::uint64_t material_id = 0;
  // bit 0: hit; bit 1: rigid correspondence; bit 2: unchanged material.
  std::uint32_t flags = 0;
  // The material's base colour at the hit, after texture lookups, as
  // unorm8 R, G, B in bits 0-23 (UnpackTemporalAlbedo).
  std::uint32_t albedo = 0;
  // The shading normal facing the camera, octahedral snorm16 x in bits
  // 0-15 and y in bits 16-31 (UnpackTemporalNormal).
  std::uint32_t shading_normal = 0;
  std::uint32_t reserved = 0;
};

struct TemporalValidityPixel {
  std::uint32_t valid = 0;
  TemporalRejection rejection = TemporalRejection::Miss;
  // Row-major history sample, or UINT32_MAX when no fetch took place.
  std::uint32_t previous_index = UINT32_MAX;
  std::uint32_t reserved = 0;
};

// GPU temporal radiance/history-length debug product, 32 bytes per pixel.
struct TemporalRadiancePixel {
  std::array<float, 4> color{};
  // Zero on misses and outside the data window; such pixels never reuse.
  std::uint32_t sample_count = 0;
  // First and second moments of each fresh batch mean's illumination
  // luminance (TemporalLuminance of DemodulateTemporalColor), weighted as
  // the colour is.
  std::array<float, 2> moments{};
  // Estimated variance of one fresh batch mean's illumination luminance
  // (EstimateTemporalVariance); zero where sample_count is zero.
  float variance = 0.0F;
};

// Rec. 709 luminance of linear RGB, as the GPU moments use it.
[[nodiscard]] float TemporalLuminance(const std::array<float, 4>& color);

// TemporalPixel's packed material inputs. Normals are unit length.
[[nodiscard]] std::array<float, 3> UnpackTemporalAlbedo(std::uint32_t albedo);
[[nodiscard]] std::array<float, 3> UnpackTemporalNormal(std::uint32_t normal);

// Illumination: each colour channel over the albedo's, or unchanged where
// that albedo channel is zero. Alpha is kept. Moments, variance and the
// filter work on it; RemodulateTemporalColor undoes it.
[[nodiscard]] std::array<float, 4> DemodulateTemporalColor(
    const std::array<float, 4>& color, std::uint32_t albedo);
[[nodiscard]] std::array<float, 4> RemodulateTemporalColor(
    const std::array<float, 4>& illumination, std::uint32_t albedo);

// Histories this many batches long use their temporal variance; shorter ones
// estimate it from the surrounding pixels' moments.
inline constexpr std::uint32_t kTemporalVarianceBatches = 4;

// CPU oracle for the GPU variance pass at pixel (x, y). A history of
// b = sample_count / min(batch_samples, max_history_samples) batches has
// variance max(0, m2 - m1^2) when b >= kTemporalVarianceBatches. A shorter
// history uses SVGF's estimate: the moments of the 7x7 neighbourhood's
// histories on the same placement, weighted by geometric normal (cosine to
// the 128th power), linear depth (against three times the local depth
// gradient per pixel of distance) and first-moment luminance (difference
// over 10), times kTemporalVarianceBatches / b. Spans hold width*height
// records of one frame; batch_samples is the frame's fresh batch.
[[nodiscard]] float EstimateTemporalVariance(
    std::span<const TemporalPixel> surface,
    std::span<const TemporalRadiancePixel> radiance, std::uint32_t x,
    std::uint32_t y, std::uint32_t width, std::uint32_t height,
    std::uint32_t batch_samples, std::uint32_t max_history_samples);

// SVGF's iteration count; at most kTemporalFilterMaxIterations run.
inline constexpr std::uint32_t kTemporalFilterIterations = 5;
inline constexpr std::uint32_t kTemporalFilterMaxIterations = 8;

// CPU oracle for the GPU filter and composite, in its float arithmetic.
// Each pixel with a history starts from its illumination and the variance
// of its mean, variance / b for b batches of history. Iteration i averages
// a 5x5 B3-spline footprint with taps 2^i pixels apart over histories on
// the same placement, weighted by shading normal (cosine to the 128th
// power), linear depth (against the local depth gradient per pixel of
// distance) and illumination luminance (against four standard deviations
// of the 3x3 Gaussian-filtered variance); variances combine with squared
// weights. The composite remodulates by the albedo and keeps the
// accumulated alpha; pixels without a history keep the accumulated colour.
// Returns width*height RGBA values; empty for malformed input.
[[nodiscard]] std::vector<std::array<float, 4>> FilterTemporalRadiance(
    std::span<const TemporalPixel> surface,
    std::span<const TemporalRadiancePixel> radiance, std::uint32_t width,
    std::uint32_t height, std::uint32_t batch_samples,
    std::uint32_t max_history_samples, std::uint32_t iterations);

// CPU oracle for the GPU's nearest-pixel backward fetch (floor of the
// continuous previous pixel). History must contain width*height records,
// or be empty after a reset. No radiance/reservoir/denoiser state is needed.
[[nodiscard]] TemporalValidityPixel ReprojectTemporalHistory(
    const TemporalPixel& current, std::uint32_t x, std::uint32_t y,
    std::uint32_t width, std::uint32_t height,
    std::span<const TemporalPixel> previous,
    const TemporalValidationSettings& settings = {});

} // namespace Lotus
