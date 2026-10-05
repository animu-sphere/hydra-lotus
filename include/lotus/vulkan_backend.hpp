// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <memory>
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
  // Frames completed since the renderer was created, this call's included.
  std::uint64_t completion = 0;
  // Frames this call rendered.
  std::uint32_t frames_rendered = 0;
  // How many times the renderer has created its targets: once, plus once per
  // change of target size.
  std::uint32_t target_creations = 0;
  bool validation_available = false;
  // Messages since the renderer was created.
  std::uint32_t validation_message_count = 0;
  std::string validation_detail;
  std::string device_name;
  std::string api_version;
  std::string driver_version;
  std::uint32_t vendor_id = 0;
  std::uint32_t device_id = 0;
  ColorProduct color;
  DepthProduct depth;
  bool ray_query_used = false;
  // GPU duration of the RenderScene pass, including clears, excluding readback.
  bool primary_ray_timestamp_available = false;
  double primary_ray_gpu_ms = 0.0;
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
  // Full-frame clears, including pixels outside the data window. Color is
  // linear RGBA; depth is Vulkan window depth in [0, 1].
  std::array<float, 4> clear_color{};
  float clear_depth = 1.0F;
  // False preserves the preceding frame's attachment, until a size change.
  // Newly created targets are initialized to the clear values either way.
  bool clear_color_enabled = true;
  bool clear_depth_enabled = true;
};

// What the renderer's GPU scene holds. Counts after "Lifetime" accumulate
// since the renderer was created.
struct GpuSceneStats {
  std::uint64_t source_revision = 0;
  std::uint32_t resident_geometries = 0;
  std::uint32_t instance_count = 0;
  // Device bytes of the geometry buffers and of the instances in use.
  std::uint64_t geometry_bytes = 0;
  std::uint64_t instance_bytes = 0;
  // Lifetime: geometry buffers created and destroyed, instance buffer
  // rewrites, upload submissions and bytes copied through staging. An empty
  // update changes none of them.
  std::uint64_t geometry_uploads = 0;
  std::uint64_t geometry_releases = 0;
  std::uint64_t instance_writes = 0;
  std::uint64_t upload_submissions = 0;
  std::uint64_t uploaded_bytes = 0;

  // Whether the device builds acceleration structures. Without them the
  // buffers are still uploaded but no BLAS or TLAS exists, and
  // `acceleration_detail` says why.
  bool acceleration_available = false;
  std::string acceleration_detail;
  // One BLAS per resident geometry; the TLAS holds one instance per
  // instance record. Bytes are the BLAS and TLAS storage.
  std::uint32_t blas_count = 0;
  std::uint32_t tlas_instance_count = 0;
  std::uint64_t acceleration_bytes = 0;
  // Lifetime: BLAS builds, full TLAS builds, and TLAS updates (refits) for
  // instance rewrites that change only transforms.
  std::uint64_t blas_builds = 0;
  std::uint64_t tlas_builds = 0;
  std::uint64_t tlas_updates = 0;
};

struct GpuSceneEvidence {
  FrameStatus status = FrameStatus::Skip;
  std::string detail;
  GpuSceneStats stats;
  // Messages since the renderer was created, as in GpuFrameEvidence.
  std::uint32_t validation_message_count = 0;
};

// A copy of the GPU scene's device buffers, for validation. A slot is the
// geometry's index in the GPU scene's geometry table.
struct GpuGeometryContents {
  std::uint32_t slot = 0;
  std::vector<std::array<float, 3>> positions;
  std::vector<std::array<std::uint32_t, 3>> triangles;
};

struct GpuInstanceContents {
  Matrix4 world_from_object = IdentityMatrix();
  std::uint32_t geometry_slot = 0;
  SurfaceMaterial material;
};

// One TLAS build input, decoded. `object_to_world` holds the first three
// rows of the transform, row-major, as Vulkan takes it. `geometry_slot` is
// the slot whose BLAS the instance references, or UINT32_MAX when it
// references no resident BLAS.
struct GpuTlasInstanceContents {
  std::array<float, 12> object_to_world{};
  std::uint32_t geometry_slot = 0;
  std::uint32_t mask = 0;
};

struct GpuSceneContents {
  FrameStatus status = FrameStatus::Skip;
  std::string detail;
  // Resident geometry in slot order, and the instances in update order.
  std::vector<GpuGeometryContents> geometries;
  std::vector<GpuInstanceContents> instances;
  // The TLAS build input in instance order; empty without acceleration
  // structures.
  std::vector<GpuTlasInstanceContents> tlas_instances;
  // The environment radiance the scene passes read.
  std::array<float, 3> environment{};
};

// What RenderScene writes into the colour product.
enum class SceneOutput {
  // One path-traced sample of radiance per pixel, clamped to [0, 1].
  Radiance,
  // The closest triangle's barycentric weights: the intersection diagnostic.
  Barycentrics,
};

// The reference path tracer's per-frame settings.
struct PathTracingSettings {
  SceneOutput output = SceneOutput::Radiance;
  // Selects each pixel's random sequence together with the pixel's
  // coordinates: the same index, scene and camera give the same image.
  std::uint32_t sample_index = 0;
  // Scattering events after the camera ray's hit; 0 keeps only the emission
  // the camera sees directly.
  std::uint32_t max_bounces = 64;
};

// This is a capability probe, not a renderer implementation. Generated source
// never reports a GPU frame until the project implements and validates one.
[[nodiscard]] BackendCapability ProbeVulkanBackend();

// A Vulkan instance, device and pipeline that outlive frames. The targets and
// their readback buffers are created on the first frame and recreated only
// when the target size changes (design policy section 23). One frame is in
// flight: Render returns after the GPU has finished and the products are read
// back.
//
// The renderer also owns the GPU scene: one device-local buffer and one BLAS
// per resident geometry, an instance buffer and a TLAS, changed only by
// UpdateScene. RenderScene path-traces them; Render is the bootstrap.
class OffscreenRenderer {
public:
  virtual ~OffscreenRenderer() = default;

  // Render the bootstrap draw `frame_count` times into `target` and read the
  // products back. After a failed submission the renderer stays failed;
  // create a new one.
  [[nodiscard]] virtual GpuFrameEvidence Render(const DrawSummary& draw,
      const OffscreenTarget& target, std::uint32_t frame_count) = 0;

  [[nodiscard]] virtual BackendCapability RayQueryCapability() const = 0;
  // Trace one camera path per pixel centre through the uploaded scene, using
  // draw.world_to_clip as the camera (the bootstrap counts are ignored).
  // Pixels whose camera ray hits output `settings.output` as linear RGB with
  // alpha 1, and the hit's projected depth; misses retain clears. Missing
  // ray-query support returns Skip. A singular camera returns Fail.
  [[nodiscard]] virtual GpuFrameEvidence RenderScene(const DrawSummary& draw,
      const OffscreenTarget& target, std::uint32_t frame_count,
      const PathTracingSettings& settings = {}) = 0;

  // Apply one SceneExtraction plan; plans are applied in order. Returns after
  // the uploads have completed. An empty plan records no GPU work. After a
  // failure the renderer stays failed, as after a failed frame: create a new
  // renderer and reset the extraction.
  [[nodiscard]] virtual GpuSceneEvidence UpdateScene(
      const SceneUpdate& update) = 0;

  // Copy the GPU scene's buffers back to the host. Validation only: it
  // allocates a readback buffer and waits for the copy.
  [[nodiscard]] virtual GpuSceneContents ReadBackScene() = 0;
};

struct RayQueryShaders {
  std::string vertex;
  std::string fragment;
};

// Creates the device and pipeline, enabling Vulkan validation capture
// whenever the loader offers it. Returns nullptr with `status`/`error`
// describing why: the core-only configuration and missing device capability
// report Skip, real failures Fail. Shader paths are explicit so build-tree and
// install-tree layouts exercise the same backend code without source-tree
// fallbacks.
[[nodiscard]] std::unique_ptr<OffscreenRenderer> CreateOffscreenRenderer(
    const std::string& vertex_shader, const std::string& fragment_shader,
    FrameStatus& status, std::string& error,
    const RayQueryShaders& ray_query_shaders = {});

// One-shot convenience: create a renderer, render `frame_count` frames and
// destroy it.
[[nodiscard]] GpuFrameEvidence RenderOffscreen(
    const DrawSummary& draw,
    const OffscreenTarget& target,
    const std::string& vertex_shader,
    const std::string& fragment_shader,
    std::uint32_t frame_count);

} // namespace Lotus
