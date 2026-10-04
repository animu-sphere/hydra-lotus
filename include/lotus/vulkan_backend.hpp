// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <lotus/extraction.hpp>

namespace Lotus {

struct BackendCapability {
  bool available = false;
  std::string detail;
};

enum class FrameStatus {
  Pass,
  Fail,
  Skip,
};

struct ColorProduct {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t row_pitch = 0;
  std::string pixel_format;
  std::string origin;
  std::string color_space;
  std::vector<std::uint8_t> payload;
};

struct DepthProduct {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t row_pitch = 0;
  std::string pixel_format;
  std::string origin;
  std::vector<float> payload;
};

struct GpuFrameEvidence {
  FrameStatus status = FrameStatus::Skip;
  std::string detail;
  std::uint64_t completion = 0;
  std::uint32_t frames_rendered = 0;
  bool validation_available = false;
  std::uint32_t validation_message_count = 0;
  std::string validation_detail;
  std::string device_name;
  std::string api_version;
  std::string driver_version;
  std::uint32_t vendor_id = 0;
  std::uint32_t device_id = 0;
  ColorProduct color;
  DepthProduct depth;
};

// The image an offscreen frame renders into. Rectangles are {x, y, width,
// height} in pixels with the products' top-left origin.
struct OffscreenTarget {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  // Where normalized device coordinates [-1, 1]^2 land: a host's display
  // window. It may extend past the image. Empty means the whole image.
  std::array<float, 4> display_window{};
  // The pixels that are rendered, clamped to the image; the rest keep the
  // clear values. Empty means the whole image.
  std::array<std::int32_t, 4> data_window{};
};

// This is a capability probe, not a renderer implementation. Generated source
// never reports a GPU frame until the project implements and validates one.
[[nodiscard]] BackendCapability ProbeVulkanBackend();

// Render a deterministic bootstrap draw through the project extraction output.
// Shader paths are explicit so build-tree and install-tree layouts exercise the
// same backend code without source-tree fallbacks.
[[nodiscard]] GpuFrameEvidence RenderOffscreen(
    const DrawSummary& draw,
    const OffscreenTarget& target,
    const std::string& vertex_shader,
    const std::string& fragment_shader,
    std::uint32_t frame_count);

} // namespace Lotus
