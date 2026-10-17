// SPDX-License-Identifier: Apache-2.0
// Dear ImGui over the presented frame. The backend draws the overlay in a
// render pass of its own over each swapchain image (PresentOverlay); this
// file initializes ImGui's Vulkan backend from the handles it is offered
// and records the latest ImGui frame there. The swapchain is sRGB, so the
// style's colours, which are display values, are decoded first.
#include "developer_ui.hpp"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>

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
  }
  return "?";
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
  explicit ImGuiDeveloperUi(Window& window) {
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
    if (!ImGui_ImplGlfw_InitForVulkan(
            static_cast<GLFWwindow*>(window.native_handle()), true)) {
      ImGui::DestroyContext(context_);
      throw std::runtime_error("Dear ImGui could not use the GLFW window");
    }
  }

  ~ImGuiDeveloperUi() override {
    ImGui::SetCurrentContext(context_);
    if (vulkan_) {
      ImGui_ImplVulkan_Shutdown();
    }
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext(context_);
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
    }
    ImGui::Render();
    return !(controls == before);
  }

  void ToggleVisible() override {
    visible_ = !visible_;
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
      if (vulkan_) {
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
      break;
    }
  }

  void DrawRenderer(Controls& controls, const UiStatus& status) {
    ImGui::SetNextWindowPos(ImVec2(12.0F, 12.0F), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360.0F, 0.0F), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Renderer")) {
      ImGui::End();
      return;
    }
    ImGui::Text("%.*s | %s", static_cast<int>(status.source.size()),
        status.source.data(), status.scene.c_str());
    ImGui::TextDisabled("%.*s", static_cast<int>(status.device.size()),
        status.device.data());
    ImGui::Text("%ux%u  %u / %u spp", status.width, status.height,
        status.samples_per_pixel, controls.samples);
    if (status.gpu_ms >= 0.0) {
      ImGui::Text("frame %.2f ms  GPU %.2f ms", status.frame_ms,
          status.gpu_ms);
    } else {
      ImGui::Text("frame %.2f ms", status.frame_ms);
    }

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
                       controls.temporal_radiance;
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
    ImGui::Checkbox("Reuse history", &controls.temporal_radiance);
    ImGui::BeginDisabled(!controls.temporal_radiance);
    SliderUnsigned("History samples", controls.history_samples, 1, 1024,
        ImGuiSliderFlags_Logarithmic);
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
    ImGui::Spacing();
    ImGui::TextDisabled("F1 hides this window");
    ImGui::End();
  }

  static void DrawViewDetail(Controls& controls, const UiStatus& status) {
    if (!ViewAvailable(controls, controls.view)) {
      ImGui::TextColored(ImVec4(1.0F, 0.4F, 0.1F, 1.0F),
          "Needs reservoirs; showing beauty");
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
    default:
      break;
    }
  }

  ImGuiContext* context_ = nullptr;
  bool vulkan_ = false;
  bool visible_ = true;
};

} // namespace

std::unique_ptr<DeveloperUi> DeveloperUi::Create(Window& window) {
  return std::make_unique<ImGuiDeveloperUi>(window);
}

} // namespace Lotus::viewport
