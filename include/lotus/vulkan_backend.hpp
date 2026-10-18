// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <lotus/extraction.hpp>
#include <lotus/temporal.hpp>
#include <lotus/vulkan_present.hpp>

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

// "rgba8-unorm" from the bootstrap draw; "rgba32-sfloat" from RenderScene.
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

// GPU durations of the wavefront integrator's kernels in the last sample a
// RenderScene call added, from timestamps between them. Each kernel's
// queue bookkeeping counts with it.
struct WavefrontTimings {
  // False for the reference integrator, when the call added no sample, or
  // when the queue has no timestamp support; the durations are then 0.
  bool available = false;
  // Camera paths for every pixel of the data window and the ray queries of
  // their camera rays: the first round's intersection.
  double camera_gpu_ms = 0.0;
  // Ray queries of the ray queues, every round after the first together.
  double intersect_gpu_ms = 0.0;
  // Hit reconstruction, emission, roulette and BSDF sampling of the hit
  // queues, every round together.
  double shade_gpu_ms = 0.0;
  // The paths still queued after the rounds, each followed to its end.
  // The resolve pass, which adds each ended path's sample to its pixel,
  // counts with the scene pass.
  double tail_gpu_ms = 0.0;
};

struct TemporalProducts {
  bool available = false;
  bool history_available = false;
  TemporalResetReason reset_reason = TemporalResetReason::FirstFrame;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t row_pitch = 0;
  std::vector<TemporalPixel> current;
  // Retained GPU metadata from the last successful temporal call. Empty
  // on a reset.
  std::vector<TemporalPixel> previous;
  // Shared GPU rejection/validity debug AOV; tightly packed top-left rows.
  std::vector<TemporalValidityPixel> validity;
  std::uint32_t validity_row_pitch = 0;
  bool timestamp_available = false;
  double primary_surface_gpu_ms = 0.0;
  double reprojection_gpu_ms = 0.0;
  // Two device surface buffers and their persistent host readback mirror.
  std::uint64_t surface_bytes = 0;
  // Persistent device validity buffer and host readback mirror.
  std::uint64_t validity_bytes = 0;
  std::uint64_t metadata_bytes = 0;
  bool radiance_available = false;
  // Accumulated colour, history length, luminance moments and variance.
  // A pixel's reprojected history is previous_radiance[validity.previous_index].
  std::vector<TemporalRadiancePixel> radiance;
  std::vector<TemporalRadiancePixel> previous_radiance;
  std::uint32_t radiance_row_pitch = 0;
  // The raw radiance: this frame's fresh RGBA batch mean, top-left rows.
  std::vector<std::array<float, 4>> batch;
  double accumulation_gpu_ms = 0.0;
  // The variance pass, after the accumulation pass.
  double variance_gpu_ms = 0.0;
  // Two history buffers, their readback mirror, and a fresh RGBA batch
  // buffer with its readback mirror.
  std::uint64_t radiance_bytes = 0;
  // RNG start and fresh samples in this frame, independent of history length.
  std::uint32_t batch_sample_index = 0;
  std::uint32_t batch_samples = 0;
  // A-trous iterations run this frame; with any, the colour attachment holds
  // the filtered composite and `radiance` the unfiltered history.
  std::uint32_t filter_iterations = 0;
  // Every iteration and the composite.
  double filter_gpu_ms = 0.0;
  // The filter's persistent ping-pong halves and guides, zero until a frame
  // filters.
  std::uint64_t filter_bytes = 0;
};

// Last fresh sample's initial direct-light reservoir, top-left rows.
// light is a slot in this scene generation's light table; uv reconstructs
// the sample in that light's unit-square domain. Never a persistent identity.
struct RestirPixel {
  std::uint32_t light = 0xFFFFFFFFU;
  float weight_sum = 0;
  float selected_target = 0;
  std::uint32_t candidate_count = 0;
  std::array<float, 2> uv{};
  float proposal = 0;
  // Bit 0: camera surface; bit 1: unrepresentable/invalid arithmetic.
  std::uint32_t flags = 0;
  // Selected, normalized RGB before BSDF partition; w is shadow transmittance.
  std::array<float, 4> contribution{};
};
static_assert(sizeof(RestirPixel) == 48);

// Resolved reuse record. Keep normalization separately: raw sum/M cannot
// recover differing-support correction. previous_index is the receiver's
// temporal correspondence, UINT32_MAX if absent; age advances in temporal reuse.
struct RestirReusePixel {
  RestirPixel reservoir;
  float normalization = 0;
  std::uint32_t supporting_candidates = 0;
  std::uint32_t previous_index = 0xFFFFFFFFU;
  std::uint32_t age = 0;
};
static_assert(sizeof(RestirReusePixel) == 64);

struct RestirProducts {
  bool available = false;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t row_pitch = 0;
  std::uint32_t temporal_row_pitch = 0;
  std::uint64_t scene_generation = 0;
  std::vector<RestirPixel> initial;
  bool history_available = false;
  std::vector<RestirReusePixel> temporal;
  std::vector<RestirReusePixel> previous;
  std::uint32_t spatial_row_pitch = 0;
  std::vector<RestirReusePixel> spatial;
  // Left, right, up, down immutable source slots; UINT32_MAX if rejected.
  std::vector<std::array<std::uint32_t, 4>> spatial_sources;
  bool timestamp_available = false;
  // Primary ray, candidates and selected-sample visibility, excluding readback.
  double initial_gpu_ms = 0;
  double temporal_gpu_ms = 0;
  double spatial_gpu_ms = 0;
  // Allocated device reservoir storage plus its host mirror, outside scene stats.
  std::uint64_t reservoir_bytes = 0;
};

struct GpuFrameEvidence {
  FrameStatus status = FrameStatus::Skip;
  std::string detail;
  // Frames completed since the renderer was created, this call's included.
  std::uint64_t completion = 0;
  // Frames this call submitted.
  std::uint32_t frames_rendered = 0;
  // How many times the renderer has created its targets: once for the
  // bootstrap draw and once for the scene passes, plus once per change of
  // either's size.
  std::uint32_t target_creations = 0;
  // The Radiance output's samples per pixel in the colour product; 0 for
  // the other outputs.
  std::uint32_t samples_per_pixel = 0;
  bool validation_available = false;
  // Synchronization validation was explicitly enabled and its messages are
  // captured by the same messenger. False carries an unavailable explanation.
  bool synchronization_validation_available = false;
  std::string synchronization_validation_detail;
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
  // The wavefront integrator's queue occupancy in the last sample the call
  // added: per bounce, how many paths entered the ray queue, element 0
  // being the camera rays. It ends at the last bounce that traced a ray;
  // empty for the reference integrator or when no sample was added. The
  // tail's rays count at the bounce they leave from.
  std::vector<std::uint32_t> wavefront_path_counts;
  // The bounces that sample ran as intersect and shade rounds; the tail
  // kernel followed the paths still queued after them. 0 when
  // wavefront_path_counts is empty.
  std::uint32_t wavefront_rounds = 0;
  WavefrontTimings wavefront_timings;
  TemporalProducts temporal;
  RestirProducts restir;
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
  // False preserves the preceding attachment or restores supplied contents.
  // Fresh targets without supplied contents initialize to the clear values.
  bool clear_color_enabled = true;
  bool clear_depth_enabled = true;
  // Optional contents restored before the first frame of a Render call when
  // the channel's clear is disabled. Tightly packed, top-left rows, linear
  // RGBA floats and window-depth floats respectively. Empty keeps the GPU
  // attachment; nonempty arrays must match the target extent.
  std::vector<float> preserved_color;
  std::vector<float> preserved_depth;
  // False keeps the frame on the GPU, for Present: no product is read back
  // or checked on the host. The colour and depth products stay empty, and so
  // do the temporal and reservoir products' records; their availability,
  // reset reason, extents, byte counts and GPU times are still reported. The
  // host's check of the reservoirs' arithmetic flags is skipped with them.
  bool read_back = true;
};

// What Present shows. Beauty is the frame's colour; the others map one of
// the latest frame's GPU debug products to display colours, which reach the
// screen unchanged (the sRGB encoding is undone first). Pixels without a
// surface are black unless a view says otherwise.
enum class DebugView {
  Beauty,
  // Temporal primary-surface products (RenderTemporalScene):
  // positive linear view depth, logarithmically from white at range[0] to
  // dark grey at range[1];
  Depth,
  // the unit world geometric normal, as 0.5 + 0.5 * n;
  Normal,
  // backward motion, as red 0.5 + 0.5 * x / range[1] and green
  // 0.5 + 0.5 * y / range[1] over blue 0.5; black without a projection;
  Motion,
  // the shared history validity: green accepted, and per TemporalRejection
  // a colour of its own (DebugViewRejectionColor).
  TemporalValidity,
  // The final reservoir of the last fresh sample (RestirInitial): the
  // spatial or temporal one when reuse ran, the initial one otherwise.
  // Green with a selected sample, dark red on a surface where no candidate
  // had a target, magenta for invalid arithmetic (flag bit 1);
  ReservoirValidity,
  // the selected sample's age in frames, from blue at 0 to red at range[1];
  // initial-only reservoirs are 0;
  ReservoirAge,
  // the resolved contribution weight W, as log10(W) from blue at range[0]
  // to red at range[1]; dark red where no sample was selected.
  ReservoirWeight,
  // Temporal radiance products (accumulate_radiance), as linear colours
  // like Beauty, or black where a pixel has no history: the raw radiance,
  // this frame's fresh batch mean;
  RawRadiance,
  // the accumulated temporal mean, before the edge-aware filter;
  AccumulatedRadiance,
  // the history length (sample_count), from blue at 0 to red at range[1];
  HistoryLength,
  // the illumination luminance variance, as log10 from blue at range[0] to
  // red at range[1].
  Variance,
};

// Present's choice of view, and the values its colour map spans.
struct PresentView {
  DebugView view = DebugView::Beauty;
  std::array<float, 2> range{0.0F, 1.0F};
  // The fraction of the width, from the left, that shows the view; the
  // rest shows the frame's colour, as Beauty. 1 shows the view alone.
  // Clamped to [0, 1]; ignored by Beauty.
  float split = 1.0F;
};

// The display colour TemporalValidity shows for each TemporalRejection
// (None for an accepted history), for a host's legend.
[[nodiscard]] std::array<float, 3> DebugViewRejectionColor(
    TemporalRejection rejection);

// One OffscreenRenderer::Present call.
struct PresentEvidence {
  // Pass presented the frame. Skip presented nothing without an error: the
  // window has a zero extent, or its swapchain was out of date and has been
  // recreated for the next call.
  FrameStatus status = FrameStatus::Skip;
  std::string detail;
  // Presentations since the renderer was created, this call's included.
  std::uint64_t frames_presented = 0;
  // Swapchain creations after the first: resizes and out-of-date surfaces.
  std::uint32_t swapchain_recreates = 0;
  std::uint32_t swapchain_width = 0;
  std::uint32_t swapchain_height = 0;
  // The swapchain's image format and present mode, as Vulkan names them
  // without their prefixes ("B8G8R8A8_SRGB", "FIFO").
  std::string swapchain_format;
  std::string present_mode;
  // Messages since the renderer was created, as in GpuFrameEvidence.
  std::uint32_t validation_message_count = 0;
  std::string validation_detail;
};

// Scene-owned backing memory and fragmentation. Renderer attachments and
// accumulation/presentation memory are outside these totals.
struct GpuMemoryStats {
  std::uint32_t blocks = 0;
  std::uint32_t peak_blocks = 0;
  std::uint32_t dedicated_blocks = 0;
  std::uint64_t live_suballocations = 0;
  // Reserved backing memory, occupied requirements (including alignment
  // padding), total reusable space and largest individual free range.
  std::uint64_t reserved_bytes = 0;
  std::uint64_t used_bytes = 0;
  std::uint64_t free_bytes = 0;
  std::uint64_t largest_free_range = 0;
  // Lifetime calls, and successful allocations from existing blocks.
  std::uint64_t device_allocations = 0;
  std::uint64_t device_frees = 0;
  std::uint64_t suballocations = 0;
  std::uint64_t reused_suballocations = 0;
  bool operator==(const GpuMemoryStats&) const = default;
};

// What the renderer's GPU scene holds. Counts after "Lifetime" accumulate
// since the renderer was created.
struct GpuSceneStats {
  GpuMemoryStats memory;
  std::uint64_t source_revision = 0;
  std::uint32_t resident_geometries = 0;
  std::uint32_t instance_count = 0;
  // Entries in the material table, the default material included.
  std::uint32_t material_count = 0;
  // Triangles of emissive instances, which next event estimation samples:
  // the light table's entries besides the analytic lights' and the
  // environment image's.
  std::uint32_t emissive_triangles = 0;
  // Analytic lights that emit, which the scene passes sample and intersect.
  std::uint32_t analytic_lights = 0;
  // Whether the scene passes read an environment image (one that emits),
  // and whether the light table samples it, which needs instances to light.
  bool environment_map = false;
  bool environment_sampled = false;
  std::uint32_t resident_textures = 0;
  // Device bytes of the geometry buffers, of the instances, of the
  // material table in use and of the textures' texels.
  std::uint64_t geometry_bytes = 0;
  std::uint64_t instance_bytes = 0;
  std::uint64_t material_bytes = 0;
  std::uint64_t texture_bytes = 0;
  // Device bytes of the light table, its per-instance offsets and analytic
  // light records included.
  std::uint64_t light_bytes = 0;
  // Device bytes of the environment image's record and sampling
  // distribution; its texels count among the textures'.
  std::uint64_t environment_bytes = 0;
  // Lifetime: geometry residencies added/removed (compatible replacements
  // can retain GPU storage), textures created/destroyed, instance
  // buffer and material table rewrites, upload submissions and bytes copied
  // through staging. An empty update changes none of them.
  std::uint64_t geometry_uploads = 0;
  std::uint64_t geometry_releases = 0;
  std::uint64_t texture_uploads = 0;
  std::uint64_t texture_releases = 0;
  std::uint64_t instance_writes = 0;
  std::uint64_t material_writes = 0;
  // Lifetime light-table rewrites: after instance, material table, light or
  // environment image changes, never for a change of the constant
  // environment alone.
  std::uint64_t light_writes = 0;
  // Lifetime environment image changes, each of which rebuilds its
  // distribution.
  std::uint64_t environment_writes = 0;
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
  // Lifetime: BLAS builds/point refits and full TLAS builds/refits. A TLAS
  // refit follows either changed instance transforms or changed BLAS bounds.
  std::uint64_t blas_builds = 0;
  std::uint64_t blas_updates = 0;
  std::uint64_t tlas_builds = 0;
  std::uint64_t tlas_updates = 0;

  // Whether the queue writes timestamps, so that UpdateScene measures each
  // submission (GpuSceneEvidence::timings).
  bool timestamps_available = false;
};

// GPU durations of one UpdateScene submission, from timestamps between its
// phases. Each phase starts after the previous one has finished.
struct GpuSceneTimings {
  // False when the call submitted no GPU work or the queue has no timestamp
  // support; the durations are then 0.
  bool available = false;
  // Copying the staged geometry, textures, instance records, material table
  // and TLAS build input; 0 when there was nothing to copy.
  double upload_gpu_ms = 0.0;
  // Building or refitting uploaded geometries' BLASes; 0 when neither ran.
  double blas_build_gpu_ms = 0.0;
  // Building or refitting the TLAS; 0 when it was left as it was.
  double tlas_build_gpu_ms = 0.0;
};

struct GpuSceneEvidence {
  FrameStatus status = FrameStatus::Skip;
  std::string detail;
  GpuSceneStats stats;
  // This call's submission.
  GpuSceneTimings timings;
  // Messages since the renderer was created, as in GpuFrameEvidence.
  std::uint32_t validation_message_count = 0;
};

// A copy of the GPU scene's device buffers, for validation. A slot is the
// geometry's index in the GPU scene's geometry table.
struct GpuGeometryContents {
  std::uint32_t slot = 0;
  std::vector<std::array<float, 3>> positions;
  std::vector<std::array<std::uint32_t, 3>> triangles;
  // One per triangle corner, as MeshGeometry::normals; empty without them.
  std::vector<std::array<float, 3>> normals;
  // Each texture-coordinate set, in MeshGeometry::texcoords key order.
  std::vector<std::vector<std::array<float, 2>>> texcoords;
};

struct GpuInstanceContents {
  Matrix4 world_from_object = IdentityMatrix();
  std::uint32_t geometry_slot = 0;
  // The instance's index in GpuSceneContents::materials.
  std::uint32_t material_slot = 0;
  // The texture-coordinate set its lookups read, as SceneInstance::texcoords.
  std::uint32_t texcoords = kNoTexcoords;
};

// A texture as the device holds it. A slot is its index in the GPU scene's
// texture table, the array the scene passes sample.
struct GpuTextureContents {
  std::uint32_t slot = 0;
  Texture texture;
};

// GpuMaterialContents::texture_slots for an input that reads no resident
// texture: a constant, or a lookup that returns its fallback.
inline constexpr std::uint32_t kNoTextureSlot =
    std::numeric_limits<std::uint32_t>::max();

// A material table entry as the device holds it. `material` has the
// constants, the texture inputs with empty keys and no texture-coordinate
// set name; `texture_slots` has, per TextureInputs entry, the slot of the
// texture it samples.
struct GpuMaterialContents {
  Material material;
  std::array<std::uint32_t, kMaterialTextureInputs> texture_slots{
      kNoTextureSlot, kNoTextureSlot, kNoTextureSlot, kNoTextureSlot,
      kNoTextureSlot, kNoTextureSlot, kNoTextureSlot};
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
  // The material table the instances index; empty until the first instances.
  std::vector<GpuMaterialContents> materials;
  // Resident textures in slot order.
  std::vector<GpuTextureContents> textures;
  // The TLAS build input in instance order; empty without acceleration
  // structures.
  std::vector<GpuTlasInstanceContents> tlas_instances;
  // The environment radiance the scene passes read.
  std::array<float, 3> environment{};
};

// What RenderScene writes into the colour product.
enum class SceneOutput {
  // The mean of the accumulated path-traced radiance samples, unclamped.
  Radiance,
  // The pixel centre's closest triangle's barycentric weights: the
  // intersection diagnostic. Not accumulated.
  Barycentrics,
  // The world-space shading normal, unit length, that the path tracer
  // evaluates the BSDF around at the pixel centre's closest hit (design
  // policy section 25). Not accumulated.
  ShadingNormal,
  // The base colour, after texture lookups, at the pixel centre's closest
  // hit, with alpha 1: the albedo diagnostic. Not accumulated.
  Albedo,
  // The roughness and metallic, after texture lookups, at the pixel
  // centre's closest hit, as (roughness, metallic, 0, 1). Not accumulated.
  RoughnessMetallic,
};

// How Radiance traces its paths. Both integrators run the same transport
// and draw each path's random numbers in the same order, so they render the
// same image up to floating-point evaluation differences.
enum class Integrator {
  // The reference path tracer: each pixel's path followed to its end in
  // one fragment-shader invocation (design policy section 9).
  Reference,
  // Wavefront path tracing (design policy section 7): compute kernels pass
  // persistent path state between ray and hit queues, one bounce at a
  // time, and each ended path leaves its sample in its slot for the
  // resolve pass. Traces at most kMaxWavefrontBounces bounces.
  Wavefront,
};

// How Radiance finds the scene's lights. The estimators converge to the same
// image and preserve surface emission. Reservoir reuse is a separate opt-in.
enum class LightSampling {
  // Brute force: emission counts only where a camera ray or a sampled BSDF
  // direction happens to find it. Parallel light, which no direction finds,
  // is added at every scattering event through a shadow ray per light,
  // drawing no random numbers. The estimator of the committed reference
  // images.
  Bsdf,
  // Next event estimation as well: at each scattering event a shadow ray
  // goes to one point on an emissive triangle or analytic light, or to one
  // direction of the environment image, chosen in proportion to its
  // estimated power. Each contribution is weighed against BSDF sampling's
  // density for the same direction by the power heuristic, and so is the
  // emission BSDF sampling finds. The constant environment is left to BSDF
  // sampling alone.
  Mis,
  // Initial-only reservoir direct lighting at the jittered camera surface.
  // Later bounces retain NEE/MIS. Constant environment remains BSDF sampled.
  RestirInitial,
};

// Light-table selection for fresh reservoir candidates. The payload mapping
// and target measure stay fixed; only the proposal probability changes.
enum class RestirProposal {
  Power = 0,
  Uniform = 1,
  PowerUniform = 2, // Equal mixture of power-alias and uniform selection.
};

// The bounces the wavefront integrator records per sample; a larger
// `max_bounces` fails.
inline constexpr std::uint32_t kMaxWavefrontBounces = 1024;

// The path tracer's settings.
struct PathTracingSettings {
  SceneOutput output = SceneOutput::Radiance;
  // Changing it restarts the accumulation.
  Integrator integrator = Integrator::Reference;
  // Changing it restarts the accumulation.
  LightSampling light_sampling = LightSampling::Mis;
  // The random sequence of an accumulation's first sample; its k-th sample
  // uses sample_index + k. With the pixel's coordinates it selects each
  // sample's random numbers: the same index, scene, camera and target give
  // the same image.
  std::uint32_t sample_index = 0;
  // Scattering events after the camera ray's hit; 0 keeps only the emission
  // the camera sees directly.
  std::uint32_t max_bounces = 64;
  // Radiance stops adding samples once each pixel holds this many; later
  // frames write the same image. 0 means no limit. Changing it does not
  // restart the accumulation.
  std::uint32_t max_samples = 0;
  // Initial light draws per primary surface, 1..64; changing it resets
  // progressive and temporal radiance.
  std::uint32_t restir_candidates = 8;
  // Opt-in reservoir reuse through RenderTemporalScene, one fresh sample
  // per call. Requires RestirInitial; independent of radiance accumulation.
  bool restir_temporal = false;
  // Cap preceding/neighbor sources' represented draws, 1..65536. Both count
  // and merge weight are capped; resolved normalization is preserved.
  std::uint32_t restir_max_history_candidates = 32;
  // One spatial pass over immutable current temporal/initial reservoirs.
  // Requires RenderTemporalScene and one fresh sample; temporal is optional.
  bool restir_spatial = false;
  // Four axis neighbours at this pixel distance, 1..32. Neighbour counts
  // use restir_max_history_candidates; geometry/material rejection is fixed.
  std::uint32_t restir_spatial_radius = 1;
  // Changing the proposal resets progressive and temporal history.
  RestirProposal restir_proposal = RestirProposal::Power;
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
// per resident geometry, one image per resident texture, an instance buffer,
// a material table and a TLAS, changed only by UpdateScene. RenderScene path-traces them; Render is the bootstrap.
class OffscreenRenderer {
public:
  virtual ~OffscreenRenderer() = default;

  // Render the bootstrap draw `frame_count` times into `target` and read the
  // products back. After a failed submission the renderer stays failed;
  // create a new one.
  [[nodiscard]] virtual GpuFrameEvidence Render(const DrawSummary& draw,
      const OffscreenTarget& target, std::uint32_t frame_count) = 0;

  // Ray queries, plus what the scene passes need besides: fragment-stage
  // storage writes, 64-bit shader integers, RGBA32F colour attachments,
  // a partially bound array of sampled textures indexed non-uniformly, and
  // linear filtering of RGBA32F textures.
  [[nodiscard]] virtual BackendCapability RayQueryCapability() const = 0;
  // Trace the uploaded scene through draw.world_to_clip as the camera (the
  // bootstrap counts are ignored), into RGBA32F colour and D32 depth.
  // Depth is the pixel centre's closest alpha-accepted hit; misses retain
  // clears. Fractional coverage uses a fixed per-pixel seed (sample index 0)
  // for depth and diagnostics, independent of progressive radiance samples.
  //
  // Radiance adds `frame_count` path-traced samples per pixel, at most up
  // to `settings.max_samples`, to a floating-point accumulation and writes
  // its mean: each sample's camera ray goes through a uniformly distributed
  // point of the pixel (a 1-pixel box filter), a hit adds its radiance with
  // alpha 1, and a miss adds the target's clear colour. Pixels no sample
  // has hit retain the colour attachment. The accumulation restarts when
  // the camera, the target's size or windows, `integrator`,
  // `sample_index`, `max_bounces` or the scene (any nonempty UpdateScene)
  // changes.
  // The other outputs write the pixel centre's hit with alpha 1
  // and leave the accumulation as it is.
  //
  // Missing ray-query support returns Skip. A singular camera returns Fail,
  // as does a Wavefront Radiance frame without the wavefront kernels or
  // with `max_bounces` above kMaxWavefrontBounces.
  [[nodiscard]] virtual GpuFrameEvidence RenderScene(const DrawSummary& draw,
      const OffscreenTarget& target, std::uint32_t frame_count,
      const PathTracingSettings& settings = {}) = 0;

  // Apply the snapshot's SceneExtraction update first. One call is one
  // temporal frame; frame_count controls progressive samples. Extent/windows
  // come from target; frame_index/history_key/camera_cut are caller-owned.
  // Only success advances history. Ordinary Render/RenderScene calls break
  // this stream. Same-count instancer reordering requires a cut/key change.
  // accumulate_radiance traces a fresh batch per call and reuses validated
  // per-pixel radiance with bounded sample counts. It automatically resets
  // for transport settings and non-rigid scene edits. Camera/rigid motion
  // preserves compatible history; history_key/camera_cut reset explicitly.
  // A progressive frame after a fresh batch (radiance or reservoir reuse)
  // restarts the accumulation.
  [[nodiscard]] virtual GpuFrameEvidence RenderTemporalScene(
      const FrameSnapshot& snapshot, const OffscreenTarget& target,
      std::uint32_t frame_count, const TemporalFrameSettings& temporal,
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

  // Copy the colour of the latest RenderScene or RenderTemporalScene frame,
  // or the debug product `view` selects, into the window's next swapchain
  // image, draw the overlay over it and present it. The copy encodes the
  // linear colour as sRGB, clamped to [0, 1], with no tone mapping. The
  // swapchain follows `width` x `height`, the window's framebuffer extent; a
  // frame of another extent is scaled to it. Returns once the copy is
  // submitted; the next frame is ordered after it on the queue. Fails
  // without a presentation surface or before a scene frame; a failed
  // presentation leaves the renderer failed, as a failed frame does. A debug
  // view whose product the latest frame did not write, or without the
  // presentation kernel, fails before any GPU work and leaves the renderer
  // usable.
  [[nodiscard]] virtual PresentEvidence Present(std::uint32_t width,
      std::uint32_t height, const PresentView& view = {}) = 0;
};

struct RayQueryShaders {
  std::string vertex;
  std::string fragment;
  // The wavefront integrator's compute kernels; without them, Wavefront
  // frames fail.
  std::string wavefront;
  // Present's debug-view kernel; without it, debug views fail.
  std::string present_view;
};

// The compiled shaders a renderer loads. The build stages them into a
// `shaders/` directory beside each host and the install tree ships the same
// layout, so a host names only that directory and never a shader file.
struct RendererShaders {
  std::string vertex;
  std::string fragment;
  RayQueryShaders ray_query;
};

// The backend's shader files in `directory`. It does not check that they
// exist; CreateOffscreenRenderer reports a missing one.
[[nodiscard]] RendererShaders ShadersIn(const std::filesystem::path& directory);

// Creates the device and pipeline, enabling Vulkan and synchronization
// validation capture whenever the loader/layer offers it. Returns nullptr
// with `status`/`error` describing why: the core-only configuration and missing
// device capability report Skip, real failures Fail. Shader paths are explicit so build-tree and
// install-tree layouts exercise the same backend code without source-tree
// fallbacks.
//
// With `presentation`, the renderer also owns the window's surface and
// swapchain, for Present: the device must present to the surface from its
// graphics queue, and the surface must offer an sRGB format the scene colour
// can be copied to. A surface the window layer cannot create, or a device
// without either, reports Skip.
[[nodiscard]] std::unique_ptr<OffscreenRenderer> CreateOffscreenRenderer(
    const std::string& vertex_shader, const std::string& fragment_shader,
    FrameStatus& status, std::string& error,
    const RayQueryShaders& ray_query_shaders = {},
    const PresentOptions* presentation = nullptr);

// One-shot convenience: create a renderer, render `frame_count` frames and
// destroy it.
[[nodiscard]] GpuFrameEvidence RenderOffscreen(
    const DrawSummary& draw,
    const OffscreenTarget& target,
    const std::string& vertex_shader,
    const std::string& fragment_shader,
    std::uint32_t frame_count);

} // namespace Lotus
