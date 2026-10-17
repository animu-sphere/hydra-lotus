// SPDX-License-Identifier: Apache-2.0
// The viewport's free camera, usdview's FreeCamera without OpenUSD: an orbit
// around a centre with yaw, pitch and roll, framed and clipped by the scene's
// bounds. GPU-, window- and USD-free, so that a unit test covers it in every
// configuration; the built-in scene and USD stages share it.
#pragma once

#include "input.hpp"

#include <array>
#include <cstdint>

namespace Lotus::viewport {

// Row vectors, as GfMatrix4d: element (row, column) at row * 4 + column,
// translation in the last row. The flat layout equals Lotus::Matrix4's.
using Matrix4d = std::array<double, 16>;
using Vec3d = std::array<double, 3>;
// World-space corners of the scene's bounds.
using BoundsCorners = std::array<Vec3d, 8>;

enum class UpAxis {
  Y,
  Z,
};

// Angles in degrees, as usdview's FreeCamera: the camera sits `distance`
// from `center`, turned by yaw (theta, about up), pitch (phi) and roll (psi).
struct OrbitView {
  Vec3d center{};
  double distance = 1.0;
  double theta = 0.0;
  double phi = 0.0;
  double psi = 0.0;
  double vertical_fov = 60.0;
};

[[nodiscard]] BoundsCorners BoxCorners(const Vec3d& minimum,
    const Vec3d& maximum);

class OrbitCamera {
public:
  // usdview's default free-camera field of view.
  static constexpr double kDefaultFov = 60.0;

  // The bounds place the clipping planes and scale framing and dollying.
  void SetBounds(const BoundsCorners& corners);
  void SetUpAxis(UpAxis up) noexcept {
    up_ = up;
  }
  [[nodiscard]] UpAxis up_axis() const noexcept {
    return up_;
  }

  // usdview's initial framing (FreeCamera.frameSelection): the bounds'
  // centroid, at the distance that fits their largest dimension with a 1.1
  // margin, looking down -Z from the up axis.
  [[nodiscard]] OrbitView Framed(double vertical_fov = kDefaultFov) const;

  void SetView(const OrbitView& view) noexcept {
    view_ = view;
  }
  [[nodiscard]] const OrbitView& view() const noexcept {
    return view_;
  }

  // 0.25 degrees per pixel, as usdview.
  void Tumble(double delta_x, double delta_y) noexcept;
  // Moves the centre so that it follows the cursor across a viewport
  // `height` pixels tall.
  void Track(double delta_x, double delta_y, std::uint32_t height) noexcept;
  // usdview's FreeCamera.AdjustDistance: scales the distance, but steps
  // away by at most a 25th of the bounds when close.
  void Dolly(double scale) noexcept;

  // Takes over an authored camera's view, as usdview copies one into its
  // free camera: the same position, orientation and vertical field of view,
  // orbiting the point in front of it at the bounds' centroid's depth (or
  // the bounds' size, when the centroid is not in front). Scale and shear
  // in `camera_to_world` are dropped.
  void Adopt(const Matrix4d& camera_to_world, double vertical_fov);

  [[nodiscard]] Matrix4d CameraToWorld() const noexcept;
  [[nodiscard]] Matrix4d WorldToView() const noexcept;
  // A symmetric perspective with OpenGL clip conventions, as
  // GfFrustum::ComputeProjectionMatrix, between the clipping planes.
  [[nodiscard]] Matrix4d Projection(double aspect) const;

  // usdview's clipping: just inside the nearest and beyond the farthest
  // bounds corner along the view direction.
  [[nodiscard]] std::array<double, 2> ClippingRange() const noexcept;

private:
  BoundsCorners corners_ = BoxCorners({-1.0, -1.0, -1.0}, {1.0, 1.0, 1.0});
  Vec3d centroid_{};
  double size_ = 2.0;
  UpAxis up_ = UpAxis::Y;
  OrbitView view_;
};

// usdview's pointer bindings: left drag tumbles, middle (or control + left)
// drag tracks, right drag and the wheel dolly. Alt, which usdview requires,
// is accepted and not required.
class Navigator {
public:
  // Whether `event` begins a navigation, so that a scene that shows an
  // authored camera hands it to the free camera first.
  [[nodiscard]] static bool Begins(const Event& event) noexcept;

  // Applies a pointer or scroll event; true when the camera moved.
  bool Handle(const Event& event, OrbitCamera& camera, std::uint32_t height);

private:
  enum class Mode {
    None,
    Tumble,
    Track,
    Dolly,
  };

  Mode mode_ = Mode::None;
  double x_ = 0.0;
  double y_ = 0.0;
};

} // namespace Lotus::viewport
