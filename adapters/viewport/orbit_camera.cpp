// SPDX-License-Identifier: Apache-2.0
#include "orbit_camera.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace Lotus::viewport {
namespace {

constexpr double kFrameFit = 1.1;

double Radians(double degrees) noexcept {
  return degrees * std::numbers::pi / 180.0;
}

double Degrees(double radians) noexcept {
  return radians * 180.0 / std::numbers::pi;
}

Matrix4d Identity() noexcept {
  return {1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
      0.0, 0.0, 1.0};
}

Matrix4d Multiply(const Matrix4d& a, const Matrix4d& b) noexcept {
  Matrix4d result{};
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 4; ++column) {
      double sum = 0.0;
      for (int k = 0; k < 4; ++k) {
        sum += a[row * 4 + k] * b[k * 4 + column];
      }
      result[row * 4 + column] = sum;
    }
  }
  return result;
}

Matrix4d Translation(const Vec3d& offset) noexcept {
  Matrix4d result = Identity();
  result[12] = offset[0];
  result[13] = offset[1];
  result[14] = offset[2];
  return result;
}

// GfRotation about a principal axis, for row vectors.
Matrix4d Rotation(int axis, double degrees) noexcept {
  const double c = std::cos(Radians(degrees));
  const double s = std::sin(Radians(degrees));
  const int u = (axis + 1) % 3;
  const int v = (axis + 2) % 3;
  Matrix4d result = Identity();
  result[u * 4 + u] = c;
  result[u * 4 + v] = s;
  result[v * 4 + u] = -s;
  result[v * 4 + v] = c;
  return result;
}

// The authored up axis as seen from a Y-up camera: usdview's
// _YZUpInverseMatrix.
Matrix4d UpMatrix(UpAxis up) noexcept {
  return up == UpAxis::Z ? Rotation(0, 90.0) : Identity();
}

Vec3d Row(const Matrix4d& matrix, int row) noexcept {
  return {matrix[row * 4], matrix[row * 4 + 1], matrix[row * 4 + 2]};
}

double Dot(const Vec3d& a, const Vec3d& b) noexcept {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3d Add(const Vec3d& a, const Vec3d& b, double scale = 1.0) noexcept {
  return {a[0] + b[0] * scale, a[1] + b[1] * scale, a[2] + b[2] * scale};
}

bool Normalize(Vec3d& value) noexcept {
  const double length = std::sqrt(Dot(value, value));
  if (!(length > 0.0) || !std::isfinite(length)) {
    return false;
  }
  for (double& component : value) {
    component /= length;
  }
  return true;
}

} // namespace

BoundsCorners BoxCorners(const Vec3d& minimum, const Vec3d& maximum) {
  BoundsCorners corners{};
  for (std::size_t corner = 0; corner < corners.size(); ++corner) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
      corners[corner][axis] =
          (corner >> axis) & 1U ? maximum[axis] : minimum[axis];
    }
  }
  return corners;
}

void OrbitCamera::SetBounds(const BoundsCorners& corners) {
  Vec3d minimum{std::numeric_limits<double>::max(),
      std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
  Vec3d maximum{std::numeric_limits<double>::lowest(),
      std::numeric_limits<double>::lowest(),
      std::numeric_limits<double>::lowest()};
  Vec3d sum{};
  for (const Vec3d& corner : corners) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
      if (!std::isfinite(corner[axis])) {
        throw std::invalid_argument("the scene bounds are not finite");
      }
      minimum[axis] = std::min(minimum[axis], corner[axis]);
      maximum[axis] = std::max(maximum[axis], corner[axis]);
    }
    sum = Add(sum, corner);
  }
  corners_ = corners;
  centroid_ = {sum[0] / 8.0, sum[1] / 8.0, sum[2] / 8.0};
  size_ = std::max({maximum[0] - minimum[0], maximum[1] - minimum[1],
      maximum[2] - minimum[2]});
  if (!std::isfinite(size_) || size_ <= 0.0) {
    size_ = 1.0;
  }
}

OrbitView OrbitCamera::Framed(double vertical_fov) const {
  OrbitView view;
  view.center = centroid_;
  view.vertical_fov = vertical_fov;
  const double fit = size_ * kFrameFit * 0.5;
  // usdview takes the arctangent of the half angle, and keeps the camera a
  // unit beyond the bounds.
  view.distance = fit / std::atan(Radians(vertical_fov) * 0.5);
  if (view.distance < 1.0 + size_ * 0.5) {
    view.distance = 1.0 + fit;
  }
  return view;
}

void OrbitCamera::Tumble(double delta_x, double delta_y) noexcept {
  view_.theta += 0.25 * delta_x;
  view_.phi += 0.25 * delta_y;
}

void OrbitCamera::Track(
    double delta_x, double delta_y, std::uint32_t height) noexcept {
  const Matrix4d camera = CameraToWorld();
  const double pixel = 2.0 * std::tan(Radians(view_.vertical_fov) * 0.5) *
                       view_.distance /
                       static_cast<double>(std::max(1U, height));
  view_.center = Add(view_.center, Row(camera, 0), -delta_x * pixel);
  view_.center = Add(view_.center, Row(camera, 1), delta_y * pixel);
}

void OrbitCamera::Dolly(double scale) noexcept {
  scale = std::max(0.01, scale);
  if (scale > 1.0 && view_.distance < 2.0) {
    view_.distance += std::min(size_ / 25.0, scale - 1.0);
  } else {
    view_.distance *= scale;
  }
  view_.distance = std::max(view_.distance, size_ * 1.0e-6);
}

void OrbitCamera::Adopt(const Matrix4d& camera_to_world, double vertical_fov) {
  // Gram-Schmidt keeps the orientation and drops scale and shear.
  Vec3d x = Row(camera_to_world, 0);
  Vec3d y = Row(camera_to_world, 1);
  Vec3d z = Row(camera_to_world, 2);
  if (!Normalize(z)) {
    throw std::invalid_argument("the camera transform is degenerate");
  }
  y = Add(y, z, -Dot(y, z));
  if (!Normalize(y)) {
    throw std::invalid_argument("the camera transform is degenerate");
  }
  x = Add(Add(x, z, -Dot(x, z)), y, -Dot(x, y));
  if (!Normalize(x)) {
    throw std::invalid_argument("the camera transform is degenerate");
  }
  Matrix4d rotation = Identity();
  for (int axis = 0; axis < 3; ++axis) {
    rotation[axis] = x[axis];
    rotation[4 + axis] = y[axis];
    rotation[8 + axis] = z[axis];
  }
  // The up matrix is a rotation, so its inverse is its transpose.
  Matrix4d up_inverse = UpMatrix(up_);
  std::swap(up_inverse[6], up_inverse[9]);
  // rotation * up^-1 = Rz(a) Rx(b) Ry(g) with a = -psi, b = -phi,
  // g = -theta; its third row is (cos b sin g, -sin b, cos b cos g).
  const Matrix4d r = Multiply(rotation, up_inverse);
  const double b = std::asin(std::clamp(-r[9], -1.0, 1.0));
  double a = 0.0;
  double g = 0.0;
  if (std::abs(std::cos(b)) > 1.0e-9) {
    a = std::atan2(r[1], r[5]);
    g = std::atan2(r[8], r[10]);
  } else {
    // Looking straight along up: roll and yaw coincide; keep yaw.
    g = std::atan2(-r[2], r[0]);
  }
  const Vec3d position = Row(camera_to_world, 3);
  const Vec3d forward{-z[0], -z[1], -z[2]};
  double distance = Dot(Add(centroid_, position, -1.0), forward);
  if (!(distance > size_ * 1.0e-3)) {
    distance = size_;
  }
  view_.center = Add(position, forward, distance);
  view_.distance = distance;
  view_.theta = -Degrees(g);
  view_.phi = -Degrees(b);
  view_.psi = -Degrees(a);
  view_.vertical_fov = vertical_fov;
}

Matrix4d OrbitCamera::CameraToWorld() const noexcept {
  Matrix4d result = Translation({0.0, 0.0, view_.distance});
  result = Multiply(result, Rotation(2, -view_.psi));
  result = Multiply(result, Rotation(0, -view_.phi));
  result = Multiply(result, Rotation(1, -view_.theta));
  result = Multiply(result, UpMatrix(up_));
  return Multiply(result, Translation(view_.center));
}

Matrix4d OrbitCamera::WorldToView() const noexcept {
  // A rigid inverse: the transposed rotation, then the negated translation
  // rotated into the view.
  const Matrix4d camera = CameraToWorld();
  Matrix4d result = Identity();
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column) {
      result[row * 4 + column] = camera[column * 4 + row];
    }
  }
  const Vec3d position = Row(camera, 3);
  for (int column = 0; column < 3; ++column) {
    result[12 + column] = -Dot(position, Row(camera, column));
  }
  return result;
}

Matrix4d OrbitCamera::Projection(double aspect) const {
  if (!(aspect > 0.0) || !std::isfinite(aspect)) {
    throw std::invalid_argument("the viewport aspect must be positive");
  }
  const auto [near_plane, far_plane] = ClippingRange();
  const double focal = 1.0 / std::tan(Radians(view_.vertical_fov) * 0.5);
  Matrix4d result{};
  result[0] = focal / aspect;
  result[5] = focal;
  result[10] = -(far_plane + near_plane) / (far_plane - near_plane);
  result[11] = -1.0;
  result[14] = -2.0 * far_plane * near_plane / (far_plane - near_plane);
  return result;
}

std::array<double, 2> OrbitCamera::ClippingRange() const noexcept {
  const Matrix4d camera = CameraToWorld();
  const Vec3d position = Row(camera, 3);
  Vec3d direction = Row(camera, 2);
  direction = {-direction[0], -direction[1], -direction[2]};
  double nearest = std::numeric_limits<double>::max();
  double farthest = 0.0;
  for (const Vec3d& corner : corners_) {
    const double depth =
        std::max(0.0, Dot(Add(corner, position, -1.0), direction));
    nearest = std::min(nearest, depth);
    farthest = std::max(farthest, depth);
  }
  double near_plane =
      nearest == 0.0 ? std::min(1.0, farthest * 0.01) : nearest * 0.99;
  near_plane = std::max(near_plane, size_ * 1.0e-7);
  return {near_plane, std::max(near_plane + 1.0, farthest * 1.01)};
}

bool Navigator::Begins(const Event& event) noexcept {
  return event.type == EventType::PointerDown ||
         event.type == EventType::Scroll;
}

bool Navigator::Handle(
    const Event& event, OrbitCamera& camera, std::uint32_t height) {
  switch (event.type) {
  case EventType::PointerDown:
    mode_ = event.button == Button::Right ? Mode::Dolly
            : event.button == Button::Middle ||
                    (event.button == Button::Left && event.modifiers.control)
                ? Mode::Track
                : Mode::Tumble;
    x_ = event.x;
    y_ = event.y;
    return false;
  case EventType::PointerUp:
    mode_ = Mode::None;
    return false;
  case EventType::PointerMove: {
    const double delta_x = event.x - x_;
    const double delta_y = event.y - y_;
    x_ = event.x;
    y_ = event.y;
    if (mode_ == Mode::None || (delta_x == 0.0 && delta_y == 0.0)) {
      return false;
    }
    if (mode_ == Mode::Tumble) {
      camera.Tumble(delta_x, delta_y);
    } else if (mode_ == Mode::Track) {
      camera.Track(delta_x, delta_y, height);
    } else {
      camera.Dolly(1.0 - 0.002 * (delta_x + delta_y));
    }
    return true;
  }
  case EventType::Scroll:
    if (event.scroll == 0.0) {
      return false;
    }
    camera.Dolly(1.0 - std::clamp(event.scroll * 0.12, -0.5, 0.5));
    return true;
  default:
    return false;
  }
}

} // namespace Lotus::viewport
