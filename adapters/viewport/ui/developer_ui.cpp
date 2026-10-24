// SPDX-License-Identifier: Apache-2.0
// Dear ImGui over the presented frame. The backend draws the overlay in a
// render pass of its own over each swapchain image (PresentOverlay); this
// file initializes ImGui's Vulkan backend from the handles it is offered
// and records the latest ImGui frame there. The swapchain is sRGB, so the
// style's colours, which are display values, are decoded first. A
// Hydra-enabled build opens USD stages through the native file dialog
// (nativefiledialog-extended), as hydra-merlin's viewport does.
#include "ui/developer_ui.hpp"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#if defined(LOTUS_VIEWPORT_FILE_DIALOG)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define GLFW_INCLUDE_NONE
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#endif
#include <GLFW/glfw3.h>
#include <nfd.h>
#include <nfd_glfw3.h>
#endif

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace Lotus::viewport {
namespace {

template <typename Handle>
Handle Decode(std::uintptr_t handle) noexcept {
  if constexpr (std::is_pointer_v<Handle>) {
    return reinterpret_cast<Handle>(handle);
  } else {
    return static_cast<Handle>(handle);
  }
}

float Linear(float display) {
  return display <= 0.04045F ? display / 12.92F
                             : std::pow((display + 0.055F) / 1.055F, 2.4F);
}

ImVec4 Linear(const std::array<float, 3>& display) {
  return {Linear(display[0]), Linear(display[1]), Linear(display[2]), 1.0F};
}

void ReportVulkan(VkResult result) {
  if (result < 0) {
    std::cerr << "lotus-viewport: Dear ImGui's Vulkan backend: VkResult "
              << static_cast<int>(result) << '\n';
  }
}

constexpr const char* kRejections[] = {"accepted", "miss", "invalid data",
    "instance changed", "material changed", "large motion",
    "depth discontinuity", "normal discontinuity", "history unavailable",
    "correspondence missing", "reprojection outside"};

const char* Label(RestirMode mode) {
  switch (mode) {
  case RestirMode::Off:
    return "Off";
  case RestirMode::Initial:
    return "Initial only";
  case RestirMode::Temporal:
    return "Temporal";
  case RestirMode::Spatial:
    return "Spatial";
  case RestirMode::Combined:
    return "Temporal + spatial";
  }
  return "?";
}

const char* Label(DebugView view) {
  switch (view) {
  case DebugView::Beauty:
    return "Beauty";
  case DebugView::Depth:
    return "Depth";
  case DebugView::Normal:
    return "Normal";
  case DebugView::Motion:
    return "Motion vectors";
  case DebugView::TemporalValidity:
    return "Temporal validity";
  case DebugView::ReservoirValidity:
    return "Reservoir validity";
  case DebugView::ReservoirAge:
    return "Reservoir age";
  case DebugView::ReservoirWeight:
    return "Reservoir weight";
  case DebugView::RawRadiance:
    return "Raw radiance";
  case DebugView::AccumulatedRadiance:
    return "Accumulated (unfiltered)";
  case DebugView::HistoryLength:
    return "History length";
  case DebugView::Variance:
    return "Variance";
  }
  return "?";
}

const char* Label(GpuPass pass) {
  switch (pass) {
  case GpuPass::PathTrace:
    return "Path trace";
  case GpuPass::WavefrontCamera:
    return "camera";
  case GpuPass::WavefrontIntersect:
    return "intersect";
  case GpuPass::WavefrontShade:
    return "shade";
  case GpuPass::WavefrontTail:
    return "tail";
  case GpuPass::RestirInitial:
    return "ReSTIR initial";
  case GpuPass::RestirTemporal:
    return "ReSTIR temporal";
  case GpuPass::RestirSpatial:
    return "ReSTIR spatial";
  case GpuPass::TemporalSurfaces:
    return "Temporal surfaces";
  case GpuPass::TemporalAccumulation:
    return "Temporal accumulation";
  case GpuPass::TemporalVariance:
    return "Variance";
  case GpuPass::TemporalFilter:
    return "Denoise filter";
  }
  return "?";
}

double Mebibytes(std::uint64_t bytes) {
  return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

// A table of the HUD's rows: the label takes the room the right-hand columns
// leave, and they fit their contents.
bool BeginRows(const char* id) {
  if (!ImGui::BeginTable(id, 3, ImGuiTableFlags_SizingFixedFit)) {
    return false;
  }
  ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed);
  ImGui::TableSetupColumn("detail", ImGuiTableColumnFlags_WidthFixed);
  return true;
}

// A row of the HUD's tables: a label and up to two right-hand columns.
void Row(const char* label, const char* value, const char* detail = "") {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::TextUnformatted(label);
  ImGui::TableNextColumn();
  ImGui::TextUnformatted(value);
  ImGui::TableNextColumn();
  ImGui::TextDisabled("%s", detail);
}

// A memory row: the bytes the latest frame that used them reported, or
// nothing before any frame did.
void MemoryRow(const char* label, std::uint64_t bytes, bool current) {
  char value[32] = "-";
  if (bytes != 0) {
    std::snprintf(value, sizeof(value), "%.2f MiB", Mebibytes(bytes));
  }
  Row(label, value, bytes != 0 && !current ? "retained" : "");
}

void Graph(const char* id, const std::vector<float>& values,
    const char* overlay) {
  ImGui::PlotLines(id, values.data(), static_cast<int>(values.size()), 0,
      overlay, 0.0F, FLT_MAX, ImVec2(-FLT_MIN, 40.0F));
}

void Swatch(const char* id, const std::array<float, 3>& display,
    const char* label) {
  ImGui::ColorButton(id, Linear(display),
      ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
      ImVec2(12.0F, 12.0F));
  ImGui::SameLine();
  ImGui::TextUnformatted(label);
}

bool SliderUnsigned(const char* label, std::uint32_t& value, int minimum,
    int maximum, ImGuiSliderFlags flags = ImGuiSliderFlags_None) {
  int edited = static_cast<int>(value);
  if (!ImGui::SliderInt(label, &edited, minimum, maximum, "%d",
          flags | ImGuiSliderFlags_AlwaysClamp)) {
    return false;
  }
  value = static_cast<std::uint32_t>(edited);
  return true;
}

class ImGuiDeveloperUi final : public DeveloperUi {
public:
  explicit ImGuiDeveloperUi(Window& window)
      : window_(static_cast<GLFWwindow*>(window.native_handle())) {
    IMGUI_CHECKVERSION();
    context_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // A developer tool launched from any directory leaves no files there.
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImGui::StyleColorsDark();
    for (ImVec4& color : ImGui::GetStyle().Colors) {
      color = ImVec4(Linear(color.x), Linear(color.y), Linear(color.z),
          color.w);
    }
    // Chains the window's own callbacks, installed when it was created.
    if (!ImGui_ImplGlfw_InitForVulkan(window_, true)) {
      ImGui::DestroyContext(context_);
      throw std::runtime_error("Dear ImGui could not use the GLFW window");
    }
#if defined(LOTUS_VIEWPORT_FILE_DIALOG)
    if (NFD_Init() == NFD_OKAY) {
      file_dialog_ = true;
      NFD_SetDisplayPropertiesFromGLFW();
    } else {
      const char* error = NFD_GetError();
      stage_error_ = error != nullptr
                         ? error
                         : "the native file dialog is unavailable";
    }
#endif
  }

  ~ImGuiDeveloperUi() override {
    ImGui::SetCurrentContext(context_);
    if (vulkan_) {
      ImGui_ImplVulkan_Shutdown();
    }
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext(context_);
#if defined(LOTUS_VIEWPORT_FILE_DIALOG)
    if (file_dialog_) {
      NFD_Quit();
    }
#endif
  }

  void ConfigurePresentation(PresentOptions& presentation) override {
    presentation.overlay.record = &Record;
    presentation.overlay.user_data = this;
  }

  bool Draw(Controls& controls, const UiStatus& status) override {
    ImGui::SetCurrentContext(context_);
    // Until the renderer offers its device, ImGui has no font texture.
    if (!vulkan_) {
      return false;
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    const Controls before = controls;
    if (visible_) {
      DrawRenderer(controls, status);
      DrawTiming(controls, status);
    }
    DrawSplit(controls);
    ImGui::Render();
    drawn_ = true;
    return !(controls == before);
  }

  void ToggleVisible() override {
    visible_ = !visible_;
  }

  std::optional<std::filesystem::path> TakeStageRequest() override {
    return std::exchange(stage_request_, std::nullopt);
  }

  void SetStageError(std::string error) override {
    stage_error_ = std::move(error);
  }

  bool WantsPointer() const noexcept override {
    ImGui::SetCurrentContext(context_);
    return vulkan_ && ImGui::GetIO().WantCaptureMouse;
  }

  bool WantsKeyboard() const noexcept override {
    ImGui::SetCurrentContext(context_);
    return vulkan_ && ImGui::GetIO().WantCaptureKeyboard;
  }

private:
  static void Record(void* user_data, const PresentOverlayContext& context) {
    static_cast<ImGuiDeveloperUi*>(user_data)->OnRecord(context);
  }

  void OnRecord(const PresentOverlayContext& context) {
    ImGui::SetCurrentContext(context_);
    switch (context.phase) {
    case PresentOverlayPhase::Initialize: {
      ImGui_ImplVulkan_InitInfo info{};
      info.ApiVersion = context.api_version;
      info.Instance = Decode<VkInstance>(context.instance);
      info.PhysicalDevice = Decode<VkPhysicalDevice>(context.physical_device);
      info.Device = Decode<VkDevice>(context.device);
      info.QueueFamily = context.queue_family;
      info.Queue = Decode<VkQueue>(context.queue);
      info.DescriptorPoolSize = 16;
      info.MinImageCount = std::max(context.image_count, 2U);
      info.ImageCount = std::max(context.image_count, 2U);
      info.PipelineInfoMain.RenderPass =
          Decode<VkRenderPass>(context.render_pass);
      info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
      info.CheckVkResultFn = &ReportVulkan;
      vulkan_ = ImGui_ImplVulkan_Init(&info);
      if (!vulkan_) {
        std::cerr << "lotus-viewport: Dear ImGui could not initialize its "
                     "Vulkan backend; the developer UI is off\n";
      }
      break;
    }
    case PresentOverlayPhase::Render:
      if (vulkan_ && drawn_) {
        if (ImDrawData* draw = ImGui::GetDrawData(); draw != nullptr) {
          ImGui_ImplVulkan_RenderDrawData(draw,
              Decode<VkCommandBuffer>(context.command_buffer));
        }
      }
      break;
    case PresentOverlayPhase::Shutdown:
      if (vulkan_) {
        ImGui_ImplVulkan_Shutdown();
        vulkan_ = false;
      }
      // The latest frame names this backend's textures; the next renderer
      // draws nothing until the UI builds a frame for it.
      drawn_ = false;
      break;
    }
  }

  void DrawRenderer(Controls& controls, const UiStatus& status) {
    ImGui::SetNextWindowPos(ImVec2(12.0F, 12.0F), ImGuiCond_FirstUseEver);
    // A fixed width; the height follows the controls shown.
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(360.0F, 0.0F), ImVec2(360.0F, FLT_MAX));
    if (!ImGui::Begin(
            "Renderer", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::End();
      return;
    }
    ImGui::Text("%.*s | %s", static_cast<int>(status.source.size()),
        status.source.data(), status.scene.c_str());
    ImGui::TextDisabled("%.*s", static_cast<int>(status.device.size()),
        status.device.data());
    if (status.can_open_stage) {
      DrawOpenStage(status.stage);
    }
    ImGui::Text("%ux%u  %u / %u spp", status.width, status.height,
        status.samples_per_pixel, controls.samples);

    ImGui::SeparatorText("Integration");
    int integrator = controls.integrator == Integrator::Wavefront ? 1 : 0;
    if (ImGui::Combo("Integrator", &integrator, "Reference\0Wavefront\0")) {
      controls.integrator =
          integrator == 1 ? Integrator::Wavefront : Integrator::Reference;
    }
    SliderUnsigned("Sample limit", controls.samples, 1, 65536,
        ImGuiSliderFlags_Logarithmic);
    const bool fresh = controls.restir == RestirMode::Temporal ||
                       controls.restir == RestirMode::Spatial ||
                       controls.restir == RestirMode::Combined ||
                       controls.temporal_radiance || controls.denoise;
    ImGui::BeginDisabled(fresh);
    ImGui::Checkbox("Accumulate", &controls.accumulate);
    ImGui::EndDisabled();
    if (fresh) {
      ImGui::SameLine();
      ImGui::TextDisabled("(reuse: one fresh sample a frame)");
    }

    ImGui::SeparatorText("Direct light");
    ImGui::BeginDisabled(controls.restir != RestirMode::Off);
    ImGui::Checkbox("NEE / MIS", &controls.nee);
    ImGui::EndDisabled();
    if (controls.restir != RestirMode::Off) {
      ImGui::SameLine();
      ImGui::TextDisabled("(reservoirs at the camera surface)");
    }
    if (ImGui::BeginCombo("ReSTIR", Label(controls.restir))) {
      for (const RestirMode mode : kRestirModes) {
        if (ImGui::Selectable(Label(mode), mode == controls.restir)) {
          controls.restir = mode;
        }
      }
      ImGui::EndCombo();
    }
    ImGui::BeginDisabled(controls.restir == RestirMode::Off);
    SliderUnsigned("Candidates", controls.restir_candidates, 1, 64);
    ImGui::EndDisabled();

    ImGui::SeparatorText("Temporal radiance");
    // The denoiser needs the history; the checkbox shows that it is on.
    bool reuse = controls.temporal_radiance || controls.denoise;
    ImGui::BeginDisabled(controls.denoise);
    if (ImGui::Checkbox("Reuse history", &reuse)) {
      controls.temporal_radiance = reuse;
    }
    ImGui::EndDisabled();
    if (controls.denoise) {
      ImGui::SameLine();
      ImGui::TextDisabled("(the denoiser's input)");
    }
    ImGui::BeginDisabled(!reuse);
    SliderUnsigned("History samples", controls.history_samples, 1, 1024,
        ImGuiSliderFlags_Logarithmic);
    ImGui::EndDisabled();

    ImGui::SeparatorText("Denoise (SVGF)");
    ImGui::Checkbox("Filter", &controls.denoise);
    ImGui::SameLine();
    ImGui::TextDisabled("(F2; keeps the history)");
    ImGui::BeginDisabled(!controls.denoise);
    SliderUnsigned("Iterations", controls.filter_iterations, 1,
        static_cast<int>(kTemporalFilterMaxIterations));
    ImGui::EndDisabled();

    ImGui::SeparatorText("View");
    if (ImGui::BeginCombo("Product", Label(controls.view))) {
      for (const DebugView view : kDebugViews) {
        const ImGuiSelectableFlags flags = ViewAvailable(controls, view)
                                               ? ImGuiSelectableFlags_None
                                               : ImGuiSelectableFlags_Disabled;
        if (ImGui::Selectable(Label(view), view == controls.view, flags)) {
          controls.view = view;
        }
      }
      ImGui::EndCombo();
    }
    DrawViewDetail(controls, status);
    ImGui::BeginDisabled(controls.view == DebugView::Beauty);
    ImGui::Checkbox("Split", &controls.split);
    ImGui::SameLine();
    ImGui::TextDisabled("(view left, beauty right)");
    ImGui::BeginDisabled(!controls.split);
    ImGui::SliderFloat("Position", &controls.split_position, 0.0F, 1.0F,
        "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::TextDisabled("F1 hides the developer UI");
    ImGui::End();
  }

  // The split view's divider, under the windows.
  static void DrawSplit(const Controls& controls) {
    const FramePlan plan = PlanFrame(controls, 0, {});
    if (plan.present.view == DebugView::Beauty || plan.present.split >= 1.0F) {
      return;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float x = std::round(display.x * plan.present.split);
    ImGui::GetBackgroundDrawList()->AddLine(ImVec2(x, 0.0F),
        ImVec2(x, display.y), IM_COL32(255, 255, 255, 160), 1.0F);
  }

  // The timing HUD: the recent frames' mean host and GPU times, pass by
  // pass, the latest scene update and the renderer memory.
  static void DrawTiming(const Controls& controls, const UiStatus& status) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x - 12.0F, 12.0F),
        ImGuiCond_FirstUseEver, ImVec2(1.0F, 0.0F));
    // A fixed width; the height follows the passes that ran.
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(340.0F, 0.0F), ImVec2(340.0F, FLT_MAX));
    if (!ImGui::Begin(
            "Timing", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::End();
      return;
    }
    if (status.timings == nullptr || status.timings->frames() == 0) {
      ImGui::TextDisabled("no frame yet");
      ImGui::End();
      return;
    }
    const TimingRecord& record = *status.timings;
    const FrameTimes mean = record.Mean();
    char value[32];
    char detail[48];

    ImGui::Text("%u / %u spp", status.samples_per_pixel, controls.samples);
    ImGui::TextDisabled("mean of the last %zu frames with these passes",
        record.mean_frames());
    if (BeginRows("##frame")) {
      std::snprintf(value, sizeof(value), "%.2f ms", mean.frame_ms);
      std::snprintf(detail, sizeof(detail), "%.0f fps",
          mean.frame_ms > 0.0 ? 1000.0 / mean.frame_ms : 0.0);
      Row("Frame (host)", value, detail);
      if (mean.gpu_ms >= 0.0) {
        std::snprintf(value, sizeof(value), "%.3f ms", mean.gpu_ms);
        Row("GPU", value);
      }
      ImGui::EndTable();
    }
    Graph("##frame-history", record.FrameHistory(), "frame ms");

    ImGui::SeparatorText("GPU passes");
    if (mean.gpu_ms < 0.0) {
      ImGui::TextDisabled("the queue writes no timestamps");
    } else {
      if (BeginRows("##passes")) {
        for (std::size_t index = 0; index < kGpuPassCount; ++index) {
          const auto pass = static_cast<GpuPass>(index);
          if (!mean.ran(pass)) {
            continue;
          }
          char label[40];
          std::snprintf(label, sizeof(label), "%s%s",
              PartOfPathTrace(pass) ? "  wavefront " : "", Label(pass));
          std::snprintf(value, sizeof(value), "%.3f ms", mean.pass(pass));
          detail[0] = '\0';
          if (mean.gpu_ms > 0.0) {
            std::snprintf(detail, sizeof(detail), "%.0f%%",
                100.0 * mean.pass(pass) / mean.gpu_ms);
          }
          Row(label, value, detail);
        }
        ImGui::EndTable();
      }
      Graph("##gpu-history", record.GpuHistory(), "GPU ms");
    }

    ImGui::SeparatorText("Scene update");
    const SceneUpdateTimes& update = record.scene_update();
    if (!update.available) {
      ImGui::TextDisabled("none with GPU work yet");
    } else {
      ImGui::TextDisabled("before frame %llu; %llu with GPU work",
          static_cast<unsigned long long>(update.frame),
          static_cast<unsigned long long>(update.count));
      if (BeginRows("##update")) {
        std::snprintf(value, sizeof(value), "%.3f ms", update.upload_ms);
        Row("Upload", value);
        std::snprintf(value, sizeof(value), "%.3f ms", update.blas_ms);
        Row("BLAS build/refit", value);
        std::snprintf(value, sizeof(value), "%.3f ms", update.tlas_ms);
        Row("TLAS build/refit", value);
        ImGui::EndTable();
      }
    }

    ImGui::SeparatorText("Memory");
    const MemoryUse& memory = record.memory();
    if (BeginRows("##memory")) {
      MemoryRow("Reservoirs", memory.reservoir_bytes,
          memory.reservoir_current);
      MemoryRow("Surface history", memory.surface_history_bytes,
          memory.surface_history_current);
      MemoryRow("Radiance history", memory.radiance_history_bytes,
          memory.radiance_history_current);
      MemoryRow("Denoise filter", memory.filter_bytes, memory.filter_current);
      std::snprintf(value, sizeof(value), "%.2f MiB",
          Mebibytes(memory.scene_used_bytes));
      std::snprintf(detail, sizeof(detail), "of %.2f reserved",
          Mebibytes(memory.scene_reserved_bytes));
      Row("Scene", value, detail);
      MemoryRow("BLAS + TLAS", memory.acceleration_bytes, true);
      ImGui::EndTable();
    }
    ImGui::End();
  }

  static void DrawViewDetail(Controls& controls, const UiStatus& status) {
    if (!ViewAvailable(controls, controls.view)) {
      ImGui::TextColored(ImVec4(1.0F, 0.4F, 0.1F, 1.0F),
          controls.view < DebugView::RawRadiance
              ? "Needs reservoirs; showing beauty"
              : "Needs temporal radiance; showing beauty");
      return;
    }
    switch (controls.view) {
    case DebugView::Depth:
      ImGui::Text("white %.3g to grey %.3g (clipping range)",
          static_cast<double>(status.depth_range[0]),
          static_cast<double>(status.depth_range[1]));
      break;
    case DebugView::Normal:
      ImGui::TextUnformatted("world geometric normal, 0.5 + 0.5 n");
      break;
    case DebugView::Motion:
      ImGui::SliderFloat("Full scale (px)", &controls.motion_pixels, 0.25F,
          256.0F, "%.2f",
          ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
      break;
    case DebugView::TemporalValidity:
      for (std::size_t rejection = 0; rejection < std::size(kRejections);
          ++rejection) {
        char id[16];
        std::snprintf(id, sizeof(id), "##rejection%zu", rejection);
        Swatch(id,
            DebugViewRejectionColor(static_cast<TemporalRejection>(rejection)),
            kRejections[rejection]);
        if (rejection % 2 == 0 && rejection + 1 < std::size(kRejections)) {
          ImGui::SameLine(180.0F);
        }
      }
      break;
    case DebugView::ReservoirValidity:
      Swatch("##selected", {0.10F, 0.75F, 0.15F}, "selected sample");
      ImGui::SameLine(180.0F);
      Swatch("##empty", {0.35F, 0.05F, 0.05F}, "no candidate target");
      Swatch("##invalid", {1.0F, 0.0F, 1.0F}, "invalid arithmetic");
      break;
    case DebugView::ReservoirAge:
      ImGui::SliderFloat("Full scale (frames)", &controls.age_frames, 1.0F,
          1024.0F, "%.0f",
          ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
      break;
    case DebugView::ReservoirWeight:
      ImGui::DragFloatRange2("log10 W", &controls.weight_log10[0],
          &controls.weight_log10[1], 0.05F, -12.0F, 12.0F, "%.2f", "%.2f",
          ImGuiSliderFlags_AlwaysClamp);
      break;
    case DebugView::RawRadiance:
      ImGui::TextUnformatted("this frame's fresh batch, before any reuse");
      break;
    case DebugView::AccumulatedRadiance:
      ImGui::TextUnformatted("the temporal mean, before the filter");
      break;
    case DebugView::HistoryLength:
      ImGui::Text("samples, blue 0 to red %u (history samples)",
          std::clamp(controls.history_samples, 1U, 65536U));
      break;
    case DebugView::Variance:
      ImGui::DragFloatRange2("log10 variance", &controls.variance_log10[0],
          &controls.variance_log10[1], 0.05F, -12.0F, 12.0F, "%.2f", "%.2f",
          ImGuiSliderFlags_AlwaysClamp);
      ImGui::TextDisabled("illumination luminance; black without history");
      break;
    default:
      break;
    }
  }

  // The button that opens a USD stage, the open stage's file name beside it
  // (its path in a tooltip), and why the last pick did not open.
  void DrawOpenStage(std::string_view stage) {
#if defined(LOTUS_VIEWPORT_FILE_DIALOG)
    ImGui::BeginDisabled(!file_dialog_);
    if (ImGui::Button("Open USD...")) {
      OpenStage();
    }
    ImGui::EndDisabled();
#endif
    if (!stage.empty()) {
      const std::size_t slash = stage.find_last_of("/\\");
      const std::string_view name =
          slash == std::string_view::npos ? stage : stage.substr(slash + 1);
      ImGui::SameLine();
      ImGui::TextDisabled("%.*s", static_cast<int>(name.size()), name.data());
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%.*s", static_cast<int>(stage.size()),
            stage.data());
      }
    }
    if (!stage_error_.empty()) {
      ImGui::PushTextWrapPos(0.0F);
      ImGui::TextColored(ImVec4(1.0F, 0.4F, 0.1F, 1.0F), "%s",
          stage_error_.c_str());
      ImGui::PopTextWrapPos();
    }
  }

#if defined(LOTUS_VIEWPORT_FILE_DIALOG)
  // Blocks the frame while the dialog is open, as hydra-merlin's does.
  void OpenStage() {
    stage_error_.clear();
    const nfdu8filteritem_t filters[] = {
        {"OpenUSD stage", "usd,usda,usdc,usdz"}};
    nfdopendialogu8args_t arguments{};
    arguments.filterList = filters;
    arguments.filterCount = static_cast<nfdfiltersize_t>(std::size(filters));
    NFD_GetNativeWindowFromGLFWWindow(window_, &arguments.parentWindow);
    nfdu8char_t* selected = nullptr;
    const nfdresult_t result = NFD_OpenDialogU8_With(&selected, &arguments);
    if (result == NFD_OKAY) {
      stage_request_ = std::filesystem::path(
          std::u8string(reinterpret_cast<const char8_t*>(selected)));
      NFD_FreePathU8(selected);
    } else if (result == NFD_ERROR) {
      const char* error = NFD_GetError();
      stage_error_ =
          error != nullptr ? error : "the native file dialog failed";
    }
  }
#endif

  GLFWwindow* window_ = nullptr;
  ImGuiContext* context_ = nullptr;
  bool vulkan_ = false;
  // Whether ImGui's draw data is a frame built since the backend initialized.
  bool drawn_ = false;
  bool visible_ = true;
  bool file_dialog_ = false;
  std::optional<std::filesystem::path> stage_request_;
  std::string stage_error_;
};

} // namespace

std::unique_ptr<DeveloperUi> DeveloperUi::Create(Window& window) {
  return std::make_unique<ImGuiDeveloperUi>(window);
}

} // namespace Lotus::viewport
