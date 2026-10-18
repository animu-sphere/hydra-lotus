// SPDX-License-Identifier: Apache-2.0
#include <lotus/temporal.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Lotus {
namespace {

using Vector4 = std::array<double, 4>;

Vector4 Transform(const Matrix4& matrix, const Vector4& point) {
  Vector4 result{};
  for (std::size_t row = 0; row < 4; ++row) {
    for (std::size_t column = 0; column < 4; ++column) {
      result[row] += matrix[column * 4 + row] * point[column];
    }
  }
  return result;
}

template <class T, std::size_t Size>
bool Finite(const std::array<T, Size>& values) {
  for (const auto value : values) {
    if (!std::isfinite(value))
      return false;
  }
  return true;
}

struct Projection {
  std::array<float, 2> pixel;
  float depth;
};

std::optional<Projection> Project(const Camera& camera, const Matrix4& placement,
    const std::array<float, 3>& point, const TemporalFrameSettings& settings) {
  if (!Finite(point))
    return std::nullopt;
  const auto world = Transform(placement, {point[0], point[1], point[2], 1.0});
  const auto view = Transform(camera.view, world);
  const auto clip = Transform(camera.projection, view);
  if (!Finite(view) || !Finite(clip) || view[3] <= 0.0 || clip[3] <= 0.0) {
    return std::nullopt;
  }
  const double x = clip[0] / clip[3];
  const double y = clip[1] / clip[3];
  const double z = clip[2] / clip[3];
  const double depth = -view[2] / view[3];
  if (x < -1.0 || x >= 1.0 || y <= -1.0 || y > 1.0 ||
      z < -1.0 || z > 1.0 || depth <= 0.0) {
    return std::nullopt;
  }
  const auto& window = settings.display_window;
  const bool full = window[2] <= 0 || window[3] <= 0;
  const double px = (full ? 0 : window[0]) +
                    (x * 0.5 + 0.5) * (full ? settings.width : window[2]);
  const double py = (full ? 0 : window[1]) +
                    (0.5 - y * 0.5) * (full ? settings.height : window[3]);
  const auto& data = settings.data_window;
  const bool all = data[2] <= 0 || data[3] <= 0;
  const double left = all ? 0 : static_cast<double>(std::max<std::int64_t>(0, data[0]));
  const double top = all ? 0 : static_cast<double>(std::max<std::int64_t>(0, data[1]));
  const double right = all ? settings.width : static_cast<double>(std::min<std::int64_t>(settings.width, std::int64_t{data[0]} + data[2]));
  const double bottom = all ? settings.height : static_cast<double>(std::min<std::int64_t>(settings.height, std::int64_t{data[1]} + data[3]));
  if (px < left || py < top || px >= right || py >= bottom)
    return std::nullopt;
  Projection result{{static_cast<float>(px), static_cast<float>(py)},
      static_cast<float>(depth)};
  if (!Finite(result.pixel) || !std::isfinite(result.depth) ||
      result.pixel[0] >= settings.width || result.pixel[1] >= settings.height) {
    return std::nullopt;
  }
  return result;
}

const Material& BoundMaterial(const LotusScene& scene, const SceneMesh& mesh) {
  static const Material fallback;
  const auto found = scene.materials.find(mesh.material);
  return found == scene.materials.end() ? fallback : found->second;
}

bool SameMaterial(const LotusScene& current_scene, const SceneMesh& current,
    const LotusScene& previous_scene, const SceneMesh& previous) {
  if (current.material != previous.material)
    return false;
  const auto& material = BoundMaterial(current_scene, current);
  if (material != BoundMaterial(previous_scene, previous))
    return false;
  for (const auto* input : TextureInputs(material)) {
    if (!input->has_value())
      continue;
    const auto a = current_scene.textures.find((*input)->texture);
    const auto b = previous_scene.textures.find((*input)->texture);
    const auto* current_texture =
        a == current_scene.textures.end() ? nullptr : a->second.get();
    const auto* previous_texture =
        b == previous_scene.textures.end() ? nullptr : b->second.get();
    if (current_texture != previous_texture)
      return false;
  }
  return true;
}

double Length(const std::array<float, 3>& normal) {
  return std::hypot(static_cast<double>(normal[0]),
      static_cast<double>(normal[1]), static_cast<double>(normal[2]));
}

} // namespace

TemporalFrame TemporalHistory::Advance(
    const FrameSnapshot& snapshot, const TemporalFrameSettings& settings) {
  if (settings.width == 0 || settings.height == 0 ||
      !Finite(settings.display_window)) {
    throw std::invalid_argument("temporal frame extent must be nonempty");
  }
  if (last_ && settings.frame_index <= last_settings_.frame_index) {
    throw std::invalid_argument("temporal frame indices must increase");
  }
  TemporalFrame frame{snapshot, std::nullopt, settings};
  if (!last_) {
    frame.reset_reason = explicit_reset_ ? TemporalResetReason::ExplicitReset
                                         : TemporalResetReason::FirstFrame;
  } else if (settings.camera_cut) {
    frame.reset_reason = TemporalResetReason::CameraCut;
  } else if (settings.frame_index - last_settings_.frame_index != 1) {
    frame.reset_reason = TemporalResetReason::FrameGap;
  } else if (settings.width != last_settings_.width ||
             settings.height != last_settings_.height ||
             settings.display_window != last_settings_.display_window ||
             settings.data_window != last_settings_.data_window) {
    frame.reset_reason = TemporalResetReason::TargetChanged;
  } else if (settings.history_key != last_settings_.history_key) {
    frame.reset_reason = TemporalResetReason::HistoryKeyChanged;
  } else {
    frame.reset_reason = TemporalResetReason::None;
    frame.previous = last_;
  }
  last_ = snapshot;
  last_settings_ = settings;
  explicit_reset_ = false;
  return frame;
}

void TemporalHistory::Reset() {
  last_.reset();
  last_settings_ = {};
  explicit_reset_ = true;
}

std::optional<TemporalPlacement> FindTemporalPlacement(
    const TemporalFrame& frame, const std::string& mesh, std::size_t placement) {
  if (!frame.previous || !frame.current.scene || !frame.previous->scene) {
    return std::nullopt;
  }
  const auto a = frame.current.scene->meshes.find(mesh);
  const auto b = frame.previous->scene->meshes.find(mesh);
  if (a == frame.current.scene->meshes.end() ||
      b == frame.previous->scene->meshes.end())
    return std::nullopt;
  const auto& current = a->second;
  const auto& previous = b->second;
  if (!current.geometry || current.geometry->triangles.empty() ||
      current.geometry != previous.geometry || !current.instance.visible ||
      !previous.instance.visible ||
      current.instance.instancer_transforms.has_value() !=
          previous.instance.instancer_transforms.has_value()) {
    return std::nullopt;
  }
  const auto count = [](const MeshInstance& instance) {
    return instance.instancer_transforms ? instance.instancer_transforms->size() : std::size_t{1};
  };
  if (count(current.instance) != count(previous.instance) ||
      placement >= count(current.instance))
    return std::nullopt;
  const auto transform = [placement](const MeshInstance& instance) {
    return instance.instancer_transforms
               ? Multiply((*instance.instancer_transforms)[placement], instance.world_from_object)
               : instance.world_from_object;
  };
  return TemporalPlacement{transform(current.instance),
      transform(previous.instance), SameMaterial(*frame.current.scene, current, *frame.previous->scene, previous)};
}

std::optional<TemporalReprojection> ReprojectTemporalPoint(
    const TemporalFrame& frame, const TemporalPlacement& placement,
    const std::array<float, 3>& object_position) {
  if (!frame.previous || frame.settings.width == 0 || frame.settings.height == 0) {
    return std::nullopt;
  }
  const auto current = Project(frame.current.camera,
      placement.current_world_from_object, object_position, frame.settings);
  const auto previous = Project(frame.previous->camera,
      placement.previous_world_from_object, object_position, frame.settings);
  if (!current || !previous)
    return std::nullopt;
  return TemporalReprojection{current->pixel, previous->pixel,
      {previous->pixel[0] - current->pixel[0],
          previous->pixel[1] - current->pixel[1]},
      previous->depth};
}

TemporalRejection ValidateTemporalHistory(const TemporalReprojection& reprojection,
    const TemporalSurface& current, const TemporalSurface& previous,
    const TemporalValidationSettings& settings) {
  if (!current.hit || !previous.hit)
    return TemporalRejection::Miss;
  if (!Finite(current.normal) || !Finite(previous.normal) ||
      !Finite(reprojection.current_pixel) || !Finite(reprojection.previous_pixel) ||
      !Finite(reprojection.motion) || !std::isfinite(reprojection.previous_depth) ||
      reprojection.previous_depth <= 0.0F ||
      !std::isfinite(current.linear_depth) || current.linear_depth <= 0.0F ||
      !std::isfinite(previous.linear_depth) || previous.linear_depth <= 0.0F ||
      !std::isfinite(settings.absolute_depth_tolerance) ||
      settings.absolute_depth_tolerance < 0.0F ||
      !std::isfinite(settings.relative_depth_tolerance) ||
      settings.relative_depth_tolerance < 0.0F ||
      !std::isfinite(settings.minimum_normal_cosine) ||
      settings.minimum_normal_cosine < -1.0F ||
      settings.minimum_normal_cosine > 1.0F ||
      !std::isfinite(settings.max_motion_pixels) || settings.max_motion_pixels < 0.0F) {
    return TemporalRejection::InvalidData;
  }
  const double current_length = Length(current.normal);
  const double previous_length = Length(previous.normal);
  if (current_length == 0.0 || previous_length == 0.0) {
    return TemporalRejection::InvalidData;
  }
  if (current.instance_id != previous.instance_id) {
    return TemporalRejection::InstanceChanged;
  }
  if (current.material_id != previous.material_id) {
    return TemporalRejection::MaterialChanged;
  }
  if (std::hypot(static_cast<double>(reprojection.motion[0]),
          static_cast<double>(reprojection.motion[1])) > settings.max_motion_pixels) {
    return TemporalRejection::LargeMotion;
  }
  const double tolerance = settings.absolute_depth_tolerance +
                           static_cast<double>(settings.relative_depth_tolerance) * reprojection.previous_depth;
  if (std::abs(static_cast<double>(previous.linear_depth) - reprojection.previous_depth) >
      tolerance)
    return TemporalRejection::DepthDiscontinuity;
  double cosine = 0.0;
  for (std::size_t axis = 0; axis < 3; ++axis) {
    cosine += (current.normal[axis] / current_length) *
              (previous.normal[axis] / previous_length);
  }
  if (cosine < settings.minimum_normal_cosine) {
    return TemporalRejection::NormalDiscontinuity;
  }
  return TemporalRejection::None;
}

TemporalValidityPixel ReprojectTemporalHistory(const TemporalPixel& current,
    std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height,
    std::span<const TemporalPixel> previous, const TemporalValidationSettings& settings) {
  TemporalValidityPixel output;
  auto Reject = [&](TemporalRejection reason) {
    output.rejection = reason;
    return output;
  };
  if ((current.flags & 1u) == 0)
    return Reject(TemporalRejection::Miss);
  if (previous.empty())
    return Reject(TemporalRejection::HistoryUnavailable);
  if ((current.flags & 2u) == 0)
    return Reject(TemporalRejection::CorrespondenceMissing);
  if ((current.flags & 4u) == 0)
    return Reject(TemporalRejection::MaterialChanged);
  if (current.motion_depth[3] != 1.0F)
    return Reject(TemporalRejection::ReprojectionOutside);
  // Float pixel arithmetic matches the GPU fetch before the double-precision
  // geometry oracle runs. Never convert non-finite/out-of-range floats to uint.
  const std::array<float, 2> pixel{static_cast<float>(x) + 0.5F,
      static_cast<float>(y) + 0.5F};
  const std::array<float, 2> old_pixel{pixel[0] + current.motion_depth[0],
      pixel[1] + current.motion_depth[1]};
  if (!Finite(old_pixel))
    return Reject(TemporalRejection::InvalidData);
  if (x >= width || y >= height || previous.size() != std::uint64_t{width} * height)
    return Reject(TemporalRejection::InvalidData);
  if (old_pixel[0] < 0 || old_pixel[1] < 0 ||
      old_pixel[0] >= width || old_pixel[1] >= height)
    return Reject(TemporalRejection::ReprojectionOutside);
  output.previous_index = static_cast<std::uint32_t>(old_pixel[1]) * width +
                          static_cast<std::uint32_t>(old_pixel[0]);
  const auto& old = previous[output.previous_index];
  const TemporalSurface surface{true, current.instance_id, current.material_id,
      current.normal_depth[3], {current.normal_depth[0], current.normal_depth[1], current.normal_depth[2]}};
  const TemporalSurface old_surface{(old.flags & 1u) != 0, old.instance_id, old.material_id,
      old.normal_depth[3], {old.normal_depth[0], old.normal_depth[1], old.normal_depth[2]}};
  const TemporalReprojection reprojection{pixel, old_pixel,
      {current.motion_depth[0], current.motion_depth[1]}, current.motion_depth[2]};
  output.rejection = ValidateTemporalHistory(reprojection, surface, old_surface, settings);
  output.valid = output.rejection == TemporalRejection::None ? 1u : 0u;
  return output;
}

float TemporalLuminance(const std::array<float, 4>& color) {
  return 0.2126F * color[0] + 0.7152F * color[1] + 0.0722F * color[2];
}

std::array<float, 3> UnpackTemporalAlbedo(std::uint32_t albedo) {
  return {static_cast<float>(albedo & 0xFFU) / 255.0F,
      static_cast<float>((albedo >> 8) & 0xFFU) / 255.0F,
      static_cast<float>((albedo >> 16) & 0xFFU) / 255.0F};
}

// Octahedral decoding, as temporal.slang's UnpackNormal.
std::array<float, 3> UnpackTemporalNormal(std::uint32_t normal) {
  const float x = std::max(static_cast<float>(static_cast<std::int16_t>(normal & 0xFFFFU)) / 32767.0F, -1.0F);
  const float y = std::max(static_cast<float>(static_cast<std::int16_t>(normal >> 16)) / 32767.0F, -1.0F);
  std::array<float, 3> n{x, y, 1.0F - std::abs(x) - std::abs(y)};
  const float fold = std::max(-n[2], 0.0F);
  n[0] += n[0] >= 0.0F ? -fold : fold;
  n[1] += n[1] >= 0.0F ? -fold : fold;
  const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  return {n[0] / length, n[1] / length, n[2] / length};
}

std::array<float, 4> DemodulateTemporalColor(const std::array<float, 4>& color, std::uint32_t albedo) {
  const auto factor = UnpackTemporalAlbedo(albedo);
  auto result = color;
  for (std::size_t c = 0; c < 3; ++c)
    if (factor[c] > 0.0F)
      result[c] = color[c] / factor[c];
  return result;
}

std::array<float, 4> RemodulateTemporalColor(const std::array<float, 4>& illumination, std::uint32_t albedo) {
  const auto factor = UnpackTemporalAlbedo(albedo);
  auto result = illumination;
  for (std::size_t c = 0; c < 3; ++c)
    if (factor[c] > 0.0F)
      result[c] = illumination[c] * factor[c];
  return result;
}

namespace {

// One frame's records as the variance and filter passes see them: a pixel is
// usable when it has a history on the given placement.
struct Neighbourhood {
  std::span<const TemporalPixel> surface;
  std::span<const TemporalRadiancePixel> radiance;
  std::int64_t width = 0, height = 0;

  bool Usable(std::int64_t x, std::int64_t y, std::uint64_t instance_id) const {
    return x >= 0 && y >= 0 && x < width && y < height &&
           radiance[y * width + x].sample_count != 0 &&
           surface[y * width + x].instance_id == instance_id;
  }

  // Along one axis, the smaller depth step to a usable neighbour; zero
  // without one.
  float Step(std::int64_t x, std::int64_t y, std::int64_t dx, std::int64_t dy) const {
    const auto& s = surface[y * width + x];
    float step = std::numeric_limits<float>::infinity();
    for (const std::int64_t sign : {-1, 1}) {
      const std::int64_t qx = x + sign * dx, qy = y + sign * dy;
      if (Usable(qx, qy, s.instance_id))
        step = std::min(step, std::abs(s.normal_depth[3] - surface[qy * width + qx].normal_depth[3]));
    }
    return std::isinf(step) ? 0.0F : step;
  }
};

} // namespace

// Float arithmetic in the GPU pass's order; see temporal.slang.
float EstimateTemporalVariance(std::span<const TemporalPixel> surface,
    std::span<const TemporalRadiancePixel> radiance, std::uint32_t x,
    std::uint32_t y, std::uint32_t width, std::uint32_t height,
    std::uint32_t batch_samples, std::uint32_t max_history_samples) {
  const std::uint64_t pixels = std::uint64_t{width} * height;
  if (x >= width || y >= height || surface.size() != pixels ||
      radiance.size() != pixels || batch_samples == 0 || max_history_samples == 0)
    return 0.0F;
  const std::size_t index = std::size_t{y} * width + x;
  const auto& center = radiance[index];
  if (center.sample_count == 0)
    return 0.0F;
  const float batches = static_cast<float>(center.sample_count) /
                        static_cast<float>(std::min(batch_samples, max_history_samples));
  if (batches >= static_cast<float>(kTemporalVarianceBatches))
    return std::max(0.0F, center.moments[1] - center.moments[0] * center.moments[0]);
  const auto& s = surface[index];
  const float depth = s.normal_depth[3];
  const Neighbourhood around{surface, radiance, width, height};
  const std::int64_t px = x, py = y;
  const float phi = std::max(around.Step(px, py, 1, 0) + around.Step(px, py, 0, 1), 1.0e-8F) * 3.0F;
  float weight_sum = 0.0F;
  std::array<float, 2> sum{};
  for (std::int64_t dy = -3; dy <= 3; ++dy) {
    for (std::int64_t dx = -3; dx <= 3; ++dx) {
      if (!around.Usable(px + dx, py + dy, s.instance_id))
        continue;
      const std::size_t q = static_cast<std::size_t>((py + dy) * width + px + dx);
      const auto& n = surface[q].normal_depth;
      const float distance = std::sqrt(static_cast<float>(dx * dx + dy * dy));
      const float wz = dx == 0 && dy == 0 ? 0.0F : std::abs(depth - n[3]) / (phi * distance);
      const float cosine = s.normal_depth[0] * n[0] + s.normal_depth[1] * n[1] +
                           s.normal_depth[2] * n[2];
      const float wn = std::pow(std::clamp(cosine, 0.0F, 1.0F), 128.0F);
      const float wl = std::abs(center.moments[0] - radiance[q].moments[0]) / 10.0F;
      const float w = std::exp(-(wl + wz)) * wn;
      sum[0] += w * radiance[q].moments[0];
      sum[1] += w * radiance[q].moments[1];
      weight_sum += w;
    }
  }
  weight_sum = std::max(weight_sum, 1.0e-6F);
  const float m1 = sum[0] / weight_sum;
  const float m2 = sum[1] / weight_sum;
  return std::max(0.0F, m2 - m1 * m1) *
         (static_cast<float>(kTemporalVarianceBatches) / batches);
}

// Float arithmetic in the GPU passes' order; see temporal.slang.
std::vector<std::array<float, 4>> FilterTemporalRadiance(
    std::span<const TemporalPixel> surface,
    std::span<const TemporalRadiancePixel> radiance, std::uint32_t width,
    std::uint32_t height, std::uint32_t batch_samples,
    std::uint32_t max_history_samples, std::uint32_t iterations) {
  const std::uint64_t pixels = std::uint64_t{width} * height;
  if (surface.size() != pixels || radiance.size() != pixels || batch_samples == 0 ||
      max_history_samples == 0 || iterations == 0 || iterations > kTemporalFilterMaxIterations)
    return {};
  const Neighbourhood around{surface, radiance, width, height};
  const float batch = static_cast<float>(std::min(batch_samples, max_history_samples));
  // Illumination and the variance of its mean's luminance.
  std::vector<std::array<float, 4>> input(pixels), output(pixels);
  for (std::size_t i = 0; i < pixels; ++i) {
    if (radiance[i].sample_count == 0)
      continue;
    const float batches = static_cast<float>(radiance[i].sample_count) / batch;
    input[i] = DemodulateTemporalColor(radiance[i].color, surface[i].albedo);
    input[i][3] = radiance[i].variance / batches;
  }
  constexpr float kKernel[3] = {0.375F, 0.25F, 0.0625F};
  const auto Luminance = [](const std::array<float, 4>& value) {
    return TemporalLuminance({value[0], value[1], value[2], 0.0F});
  };
  for (std::uint32_t iteration = 0; iteration < iterations; ++iteration) {
    const std::int64_t step = std::int64_t{1} << iteration;
    for (std::int64_t py = 0; py < height; ++py) {
      for (std::int64_t px = 0; px < width; ++px) {
        const std::size_t index = static_cast<std::size_t>(py * width + px);
        output[index] = {};
        if (radiance[index].sample_count == 0)
          continue;
        const auto& s = surface[index];
        float gaussian = 0.0F, gaussian_weight = 0.0F;
        for (std::int64_t dy = -1; dy <= 1; ++dy) {
          for (std::int64_t dx = -1; dx <= 1; ++dx) {
            if (!around.Usable(px + dx, py + dy, s.instance_id))
              continue;
            const float k = (dx == 0 ? 0.5F : 0.25F) * (dy == 0 ? 0.5F : 0.25F);
            gaussian += k * input[static_cast<std::size_t>((py + dy) * width + px + dx)][3];
            gaussian_weight += k;
          }
        }
        const float phi_l = 4.0F * std::sqrt(std::max(gaussian / gaussian_weight, 0.0F) + 1.0e-10F);
        const float phi_z = std::max(around.Step(px, py, 1, 0) + around.Step(px, py, 0, 1), 1.0e-8F);
        const auto normal = UnpackTemporalNormal(s.shading_normal);
        const float luminance = Luminance(input[index]);
        std::array<float, 3> sum{};
        float variance = 0.0F, weight_sum = 0.0F;
        for (std::int64_t ty = -2; ty <= 2; ++ty) {
          for (std::int64_t tx = -2; tx <= 2; ++tx) {
            const std::int64_t qx = px + tx * step, qy = py + ty * step;
            if (!around.Usable(qx, qy, s.instance_id))
              continue;
            const std::size_t q = static_cast<std::size_t>(qy * width + qx);
            const auto& value = input[q];
            float w = kKernel[std::abs(tx)] * kKernel[std::abs(ty)];
            if (tx != 0 || ty != 0) {
              const float distance = static_cast<float>(step) * std::sqrt(static_cast<float>(tx * tx + ty * ty));
              const float wz = std::abs(s.normal_depth[3] - surface[q].normal_depth[3]) / (phi_z * distance);
              const float wl = std::abs(luminance - Luminance(value)) / phi_l;
              const auto other = UnpackTemporalNormal(surface[q].shading_normal);
              const float cosine = normal[0] * other[0] + normal[1] * other[1] + normal[2] * other[2];
              const float wn = std::pow(std::clamp(cosine, 0.0F, 1.0F), 128.0F);
              w = w * std::exp(-(wl + wz)) * wn;
            }
            for (std::size_t c = 0; c < 3; ++c)
              sum[c] += w * value[c];
            variance += w * w * value[3];
            weight_sum += w;
          }
        }
        output[index] = {sum[0] / weight_sum, sum[1] / weight_sum, sum[2] / weight_sum,
            variance / (weight_sum * weight_sum)};
      }
    }
    input.swap(output);
  }
  // The composite: the last iteration's illumination, remodulated.
  for (std::size_t i = 0; i < pixels; ++i) {
    if (radiance[i].sample_count == 0) {
      output[i] = radiance[i].color;
      continue;
    }
    output[i] = RemodulateTemporalColor({input[i][0], input[i][1], input[i][2], radiance[i].color[3]},
        surface[i].albedo);
  }
  return output;
}

} // namespace Lotus
