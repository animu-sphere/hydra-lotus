// SPDX-License-Identifier: Apache-2.0
// lotus-viewport's free camera and pointer bindings, without a window, a GPU
// or OpenUSD.
#include "../adapters/viewport/orbit_camera.hpp"

#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>
#include <iostream>
#include <string>

namespace {

using Lotus::viewport::BoxCorners;
using Lotus::viewport::Button;
using Lotus::viewport::Event;
using Lotus::viewport::EventType;
using Lotus::viewport::Matrix4d;
using Lotus::viewport::Navigator;
using Lotus::viewport::OrbitCamera;
using Lotus::viewport::OrbitView;
using Lotus::viewport::UpAxis;
using Lotus::viewport::Vec3d;

int failures = 0;

void Check(bool condition, const std::string& what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
  }
}

bool Near(double a, double b, double tolerance = 1e-9) {
  return std::abs(a - b) <= tolerance * std::max(1.0, std::abs(b));
}

bool Near(const Matrix4d& a, const Matrix4d& b, double tolerance = 1e-9) {
  for (std::size_t index = 0; index < a.size(); ++index) {
    if (!Near(a[index], b[index], tolerance)) {
      return false;
    }
  }
  return true;
}

// A world point through view and projection, to pixels from the image
// centre (x right, y up) and the clip-space depth.
std::array<double, 3> Project(const OrbitCamera& camera, const Vec3d& point,
    double width, double height) {
  const Matrix4d view = camera.WorldToView();
  const Matrix4d projection = camera.Projection(width / height);
  std::array<double, 4> v{};
  for (int column = 0; column < 4; ++column) {
    v[column] = point[0] * view[column] + point[1] * view[4 + column] +
                point[2] * view[8 + column] + view[12 + column];
  }
  std::array<double, 4> clip{};
  for (int column = 0; column < 4; ++column) {
    for (int k = 0; k < 4; ++k) {
      clip[column] += v[k] * projection[k * 4 + column];
    }
  }
  return {clip[0] / clip[3] * width * 0.5, clip[1] / clip[3] * height * 0.5,
      clip[2] / clip[3]};
}

Event Pointer(EventType type, double x, double y,
    Button button = Button::Left, bool control = false) {
  Event event{type};
  event.x = x;
  event.y = y;
  event.button = button;
  event.modifiers.control = control;
  return event;
}

void Framing() {
  // usdview's frameSelection over a 2-unit cube, 60 degree field of view.
  OrbitCamera camera;
  camera.SetBounds(BoxCorners({-1.0, -1.0, -1.0}, {1.0, 1.0, 1.0}));
  const OrbitView framed = camera.Framed();
  Check(Near(framed.distance, 1.1 / std::atan(std::numbers::pi / 6.0)),
      "framing distance");
  Check(framed.center == Vec3d{} && framed.theta == 0.0 && framed.phi == 0.0,
      "framing centre and orientation");
  camera.SetView(framed);
  const Matrix4d view = camera.WorldToView();
  Check(Near(view[14], -framed.distance) && Near(view[0], 1.0) &&
            Near(view[5], 1.0) && Near(view[10], 1.0),
      "framed view looks down -Z");
  const auto [near_plane, far_plane] = camera.ClippingRange();
  Check(Near(near_plane, (framed.distance - 1.0) * 0.99) &&
            Near(far_plane, (framed.distance + 1.0) * 1.01),
      "clipping range around the bounds");

  // A unit-sized scene keeps the camera a unit beyond its bounds.
  OrbitCamera small;
  small.SetBounds(BoxCorners({0.0, 0.0, 0.0}, {0.1, 0.1, 0.1}));
  Check(Near(small.Framed().distance, 1.0 + 0.055), "framing guard");
  Check(Near(small.Framed().center[0], 0.05), "framing centroid");

  // Z up: the framed camera looks along -Y with Z up the image.
  OrbitCamera z_up;
  z_up.SetUpAxis(UpAxis::Z);
  z_up.SetBounds(BoxCorners({-1.0, -1.0, -1.0}, {1.0, 1.0, 1.0}));
  z_up.SetView(z_up.Framed());
  const Matrix4d camera_to_world = z_up.CameraToWorld();
  Check(Near(camera_to_world[4 + 2], 1.0) && Near(camera_to_world[8 + 1], -1.0) &&
            Near(camera_to_world[13], -z_up.view().distance),
      "Z-up framing");
}

void Projection() {
  // The built-in scene's home view is the headless bootstrap camera.
  OrbitCamera camera;
  OrbitView home;
  home.distance = 3.0;
  home.vertical_fov = 45.0;
  camera.SetView(home);
  const Matrix4d projection = camera.Projection(1.0);
  const auto [near_plane, far_plane] = camera.ClippingRange();
  Check(Near(projection[0], 2.41421356, 1e-8) &&
            Near(projection[5], 2.41421356, 1e-8) &&
            Near(projection[10],
                (far_plane + near_plane) / (near_plane - far_plane)) &&
            projection[11] == -1.0 &&
            Near(projection[14],
                2.0 * far_plane * near_plane / (near_plane - far_plane)) &&
            projection[15] == 0.0,
      "projection matches GfFrustum's");
  // Equal world offsets cover equal pixels on any aspect.
  for (const auto& extent : {std::array<double, 2>{720.0, 720.0},
           std::array<double, 2>{1280.0, 720.0},
           std::array<double, 2>{720.0, 1280.0}}) {
    const auto p = Project(camera, {0.5, 0.5, 0.0}, extent[0], extent[1]);
    Check(p[0] > 0.0 && Near(p[0], p[1], 1e-9), "square pixels");
  }
  bool rejected = false;
  try {
    (void)camera.Projection(0.0);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  Check(rejected, "zero aspect rejected");
}

void Navigation() {
  OrbitCamera camera;
  camera.SetBounds(BoxCorners({-1.0, -1.0, -1.0}, {1.0, 1.0, 1.0}));
  camera.SetView(camera.Framed());
  Navigator navigator;
  const std::uint32_t height = 600;

  // Left drag: a quarter degree per pixel.
  Check(Navigator::Begins(Pointer(EventType::PointerDown, 100, 100)),
      "pointer down begins a navigation");
  Check(!navigator.Handle(Pointer(EventType::PointerDown, 100, 100), camera,
            height),
      "pointer down alone does not move");
  Check(navigator.Handle(Pointer(EventType::PointerMove, 108, 96), camera,
            height),
      "tumble moves");
  Check(Near(camera.view().theta, 2.0) && Near(camera.view().phi, -1.0),
      "tumble angles");
  (void)navigator.Handle(Pointer(EventType::PointerUp, 108, 96), camera,
      height);
  Check(!navigator.Handle(Pointer(EventType::PointerMove, 200, 200), camera,
            height),
      "moving without a button does not navigate");

  // Track: the centre follows the cursor, pixel for pixel.
  camera.SetView(camera.Framed());
  for (const auto& [button, control] :
      {std::pair{Button::Middle, false}, std::pair{Button::Left, true}}) {
    camera.SetView(camera.Framed());
    (void)navigator.Handle(Pointer(EventType::PointerDown, 0, 0, button,
                               control),
        camera, height);
    (void)navigator.Handle(Pointer(EventType::PointerMove, 30, 20), camera,
        height);
    (void)navigator.Handle(Pointer(EventType::PointerUp, 30, 20), camera,
        height);
    const auto p = Project(camera, {0.0, 0.0, 0.0}, 800.0, height);
    // y grows down the framebuffer, up in the image.
    Check(Near(p[0], 30.0, 1e-9) && Near(p[1], -20.0, 1e-9),
        "track follows the cursor");
  }

  // Right drag and the wheel dolly.
  camera.SetView(camera.Framed());
  const double framed = camera.view().distance;
  (void)navigator.Handle(Pointer(EventType::PointerDown, 0, 0, Button::Right),
      camera, height);
  (void)navigator.Handle(Pointer(EventType::PointerMove, 50, 50), camera,
      height);
  (void)navigator.Handle(Pointer(EventType::PointerUp, 50, 50), camera,
      height);
  Check(Near(camera.view().distance, framed * 0.8), "dolly drag");
  Event wheel{EventType::Scroll};
  wheel.scroll = 1.0;
  Check(Navigator::Begins(wheel), "scroll begins a navigation");
  Check(navigator.Handle(wheel, camera, height) &&
            Near(camera.view().distance, framed * 0.8 * 0.88),
      "wheel dollies in");
  // Close in, stepping away grows by at most a 25th of the bounds.
  wheel.scroll = -10.0;
  (void)navigator.Handle(wheel, camera, height);
  Check(Near(camera.view().distance, framed * 0.8 * 0.88 + 2.0 / 25.0),
      "close dolly step");
  // Further out, one wheel event scales the distance by at most 1.5.
  OrbitView far = camera.Framed();
  far.distance = 10.0;
  camera.SetView(far);
  (void)navigator.Handle(wheel, camera, height);
  Check(Near(camera.view().distance, 15.0), "wheel step is bounded");
  camera.Dolly(0.0);
  Check(camera.view().distance > 0.0, "dolly keeps a positive distance");
}

void Adoption() {
  for (const UpAxis up : {UpAxis::Y, UpAxis::Z}) {
    for (const double phi : {-20.0, 89.0, 90.0}) {
      OrbitCamera source;
      source.SetUpAxis(up);
      source.SetBounds(BoxCorners({-2.0, -1.0, -3.0}, {4.0, 2.0, 1.0}));
      OrbitView view = source.Framed();
      view.theta = 30.0;
      view.phi = phi;
      view.psi = 10.0;
      view.vertical_fov = 17.4;
      source.SetView(view);
      Matrix4d authored = source.CameraToWorld();

      OrbitCamera camera;
      camera.SetUpAxis(up);
      camera.SetBounds(BoxCorners({-2.0, -1.0, -3.0}, {4.0, 2.0, 1.0}));
      // An authored scale does not reach the free camera.
      Matrix4d scaled = authored;
      for (int index = 0; index < 12; ++index) {
        scaled[index] *= 2.0;
      }
      camera.Adopt(scaled, 17.4);
      Check(Near(camera.CameraToWorld(), authored, 1e-9),
          "adopted camera keeps the authored view");
      Check(camera.view().vertical_fov == 17.4,
          "adopted camera keeps the field of view");
      Check(camera.view().distance > 0.0, "adopted camera orbits ahead");
    }
  }
  // Looking away from the bounds: orbit at the bounds' size.
  OrbitCamera camera;
  camera.SetBounds(BoxCorners({-1.0, -1.0, -1.0}, {1.0, 1.0, 1.0}));
  // Turned 180 degrees about Y, at z = 5: looking down +Z.
  const Matrix4d away{-1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0,
      -1.0, 0.0, 0.0, 0.0, 5.0, 1.0};
  camera.Adopt(away, 60.0);
  Check(Near(camera.view().distance, 2.0) &&
            Near(camera.view().center[2], 7.0),
      "adopted camera facing away orbits at the bounds' size");
  bool rejected = false;
  try {
    camera.Adopt(Matrix4d{}, 60.0);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  Check(rejected, "degenerate camera rejected");
}

} // namespace

int main() {
  Framing();
  Projection();
  Navigation();
  Adoption();
  return failures == 0 ? 0 : 1;
}
