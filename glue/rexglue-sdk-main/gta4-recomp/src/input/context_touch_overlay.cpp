#include "input/context_touch_overlay.h"

#if defined(__ANDROID__)
#include "android/android_perf_overlay.h"
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <vector>
#include <rex/input/absolute_pointer.h>
#include <rex/ui/image_decode.h>
#include <rex/logging.h>

#include "input/context_touch_settings.h"
#include "input/gamepad_touch_overlay.h"
#include "input/liberty_touch_overlay.h"

namespace gta4::input {

ContextTouchOverlay::ContextTouchOverlay(rex::ui::ImGuiDrawer* drawer,
                                         rex::ui::ImmediateDrawer* immediate,
                                         std::filesystem::path icon_directory,
                                         rex::ui::Window* window)
    : ImGuiDialog(drawer), immediate_(immediate), icon_directory_(std::move(icon_directory)) {
  if (window) editor_mouse_ = std::make_unique<ContextTouchEditorMouse>(window);
}

void ContextTouchOverlay::SetIconResolver(ContextTouchIconResolver resolver, void* context) noexcept {
  icon_resolver_ = resolver;
  icon_context_ = context;
}

bool ContextTouchOverlay::ResolveIcon(void* context, std::string_view id, ContextTouchIcon* icon) {
  auto& self = *static_cast<ContextTouchOverlay*>(context);
  if (!self.immediate_) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_WARN("touch-icon: no immediate drawer, icons cannot be loaded");
    }
    return false;
  }
  if (!icon || self.icon_directory_.empty() || id.empty() ||
      !std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; })) return false;
  const std::string key(id);
  auto found = self.icons_.find(key);
  // A failed icon is retried every 2 s (up to 5 times) instead of being
  // remembered as missing forever, and each failure is logged with its cause.
  if (found != self.icons_.end() && !found->second) {
    auto& retry = self.icon_retries_[key];
    const auto now = std::chrono::steady_clock::now();
    if (retry.attempts < 5 && now - retry.last >= std::chrono::seconds(2)) {
      self.icons_.erase(found);
      found = self.icons_.end();
    }
  }
  if (found == self.icons_.end()) {
    const char* failure = nullptr;
    if (self.icons_.size() >= 256) return false;
    std::unique_ptr<rex::ui::ImmediateTexture> texture;
    const std::filesystem::path icon_path = self.icon_directory_ / (key + ".png");
    std::ifstream file(icon_path, std::ios::binary | std::ios::ate);
    if (!file) failure = "file not found";
    if (file) {
      const auto length = file.tellg();
      if (length > 0 && length <= 4194304) {
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        file.seekg(0);
        if (file.read(reinterpret_cast<char*>(bytes.data()), length)) {
          int width = 0, height = 0;
          const auto rgba = rex::ui::DecodeImageRGBA(bytes.data(), bytes.size(), width, height, 1024, 1024);
          if (rgba.empty()) failure = "PNG decode failed";
          if (!rgba.empty() && width > 0 && height > 0 &&
              rgba.size() <= 33554432 - self.decoded_icon_bytes_) {
            texture = self.immediate_->CreateTexture(static_cast<uint32_t>(width),
                static_cast<uint32_t>(height), rex::ui::ImmediateTextureFilter::kLinear, false, rgba.data());
            if (texture) self.decoded_icon_bytes_ += rgba.size();
            else failure = "texture creation failed";
          }
        }
      }
    }
    if (!texture) {
      auto& retry = self.icon_retries_[key];
      ++retry.attempts;
      retry.last = std::chrono::steady_clock::now();
      REXLOG_WARN("touch-icon: '{}' not loaded ({}), attempt {} - {}", key,
                  failure ? failure : "unknown", retry.attempts, icon_path.string());
    }
    found = self.icons_.emplace(key, std::move(texture)).first;
  }
  if (!found->second) return false;
  icon->texture = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(found->second.get()));
  icon->aspect = float(found->second->width) / float(found->second->height);
  return true;
}

void ContextTouchOverlay::OnDraw(ImGuiIO& io) {
  (void)FlushContextTouchSettings();
#if defined(__ANDROID__)
  // The performance overlay draws whether or not the touch controls are
  // visible (also with a physical controller) and keeps the layer repainting.
  gta4::android::DrawPerfOverlay(ImGui::GetForegroundDrawList(), ImGui::GetFont(),
                                 io.DisplaySize.x, io.DisplaySize.y);
  const bool perf_overlay = gta4::android::PerfOverlayEnabled();
#else
  const bool perf_overlay = false;
#endif
  if (GamepadTouchLayoutSelected()) {
    // The Liberty layout draws its icons from the same touch-icons cache.
    SetLibertyIconResolver(&ContextTouchOverlay::ResolveIcon, this);
    repaint_needed_ = UpdateAndDrawGamepadTouchOverlay(
        ImGui::GetForegroundDrawList(), ImGui::GetFont(), ImGui::GetFontSize(), io.DisplaySize.x,
        io.DisplaySize.y) || perf_overlay;
    return;
  }
  const auto snapshot = GetContextTouchDrawableOverlaySnapshot();
  repaint_needed_ = (snapshot.visible && snapshot.layout.mode != ContextTouchMode::kFrontend &&
                     snapshot.layout.mode != ContextTouchMode::kMap) || perf_overlay;
  DrawContextTouchOverlay(ImGui::GetForegroundDrawList(), ImGui::GetFont(), ImGui::GetFontSize(),
                          snapshot, io.DisplaySize.x, io.DisplaySize.y,
                          icon_resolver_ ? icon_resolver_ : ResolveIcon,
                          icon_resolver_ ? icon_context_ : this);
}

}  // namespace gta4::input
