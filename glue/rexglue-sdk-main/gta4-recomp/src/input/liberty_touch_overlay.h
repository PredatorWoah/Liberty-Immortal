#pragma once
// Liberty touch layout (Mali Edition): icon buttons that switch between on
// foot, driving, phone and menu screens, sent to the title as an Xbox 360 pad.
// Selected with gta4_touch_layout=liberty. Hold Pause for 1.5 s to move the
// buttons; positions are saved in liberty_touch_layout.txt.
#include <cstdint>
#include <string_view>

#include <imgui.h>

namespace rex::input {
struct X_INPUT_GAMEPAD;
}

namespace gta4::input {

bool LibertyTouchLayoutActive(std::string_view layout) noexcept;
bool UpdateAndDrawLibertyTouchOverlay(ImDrawList* draw, ImFont* font, float font_size, float width,
                                      float height) noexcept;
bool LibertyTouchOverlayVisible() noexcept;
bool ReadLibertyTouchOverlay(uint32_t user, rex::input::X_INPUT_GAMEPAD* output) noexcept;

}  // namespace gta4::input
