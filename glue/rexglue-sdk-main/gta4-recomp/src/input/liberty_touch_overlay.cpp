// Liberty touch layout (Mali Edition): icon buttons that follow what the
// player is doing (on foot, driving, phone, menus), drawn in a GTA-style HUD
// look, sent to the title as an Xbox 360 pad. Every control can be moved:
// hold Pause for 1.5 s to edit, drag, tap Pause to save.
#include "input/liberty_touch_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_set>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_touch.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/absolute_pointer.h>
#include <rex/logging.h>
#include <rex/input/input.h>

#include "gta4_direct_cheats.h"
#include "input/context_touch_context.h"
#include "input/context_touch_draw.h"

REXCVAR_DECLARE(double, gta4_touch_gamepad_scale);
REXCVAR_DECLARE(std::string, gta4_touch_camera);
REXCVAR_DECLARE(double, gta4_touch_swipe_sensitivity);
REXCVAR_DECLARE(double, gta4_touch_gamepad_opacity);
REXCVAR_DEFINE_BOOL(gta4_touch_labels, false, "GTA IV/Input/Touch",
                    "Show text labels under the Liberty layout's buttons");

namespace gta4::input {
namespace {

using rex::input::X_INPUT_GAMEPAD_A;
using rex::input::X_INPUT_GAMEPAD_B;
using rex::input::X_INPUT_GAMEPAD_BACK;
using rex::input::X_INPUT_GAMEPAD_DPAD_DOWN;
using rex::input::X_INPUT_GAMEPAD_DPAD_LEFT;
using rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT;
using rex::input::X_INPUT_GAMEPAD_DPAD_UP;
using rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER;
using rex::input::X_INPUT_GAMEPAD_LEFT_THUMB;
using rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER;
using rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB;
using rex::input::X_INPUT_GAMEPAD_START;
using rex::input::X_INPUT_GAMEPAD_X;
using rex::input::X_INPUT_GAMEPAD_Y;

enum class Screen : uint8_t { kHidden, kOnFoot, kDriving, kPhone, kMenu, kCutscene };
enum class Kind : uint8_t { kButton, kStick, kTrigger, kSteer, kReset };
enum class Anchor : uint8_t { kLeft, kRight, kCenter };
enum class Icon : uint8_t {
  kNone, kMenu, kPhone, kView, kCrouch, kCover, kPistol, kDoor, kReload, kFist, kAim, kBullet,
  kJump, kRun, kSteerLeft, kSteerRight, kRadio, kHorn, kEye, kHandbrake, kGas, kBrake,
  kUp, kDown, kLeft, kRight, kCheck, kBack, kHangUp, kLB, kRB, kReset, kSkip, kHouse, kGamepad, kCheats
};

// Icon image files (assets/touch-icons/<name>.png, copied to the user folder by
// the launcher). Drawn tinted; the vector icon is the fallback when a file is
// missing.
const char* IconFile(Icon icon) {
  switch (icon) {
    case Icon::kMenu: return "house";
    case Icon::kHouse: return "house";
    case Icon::kPhone: return "phone";
    case Icon::kView: return "view";
    case Icon::kGamepad: return "gamepad";
    case Icon::kCrouch: return "crouch";
    case Icon::kCover: return "cover";
    case Icon::kPistol: return "pistol";
    case Icon::kDoor: return "door";
    case Icon::kReload: return "reload";
    case Icon::kFist: return "fist";
    case Icon::kAim: return "aim";
    case Icon::kBullet: return "bullet";
    case Icon::kJump: return "jump";
    case Icon::kRun: return "run";
    case Icon::kSteerLeft: return "steer_left";
    case Icon::kSteerRight: return "steer_right";
    case Icon::kRadio: return "radio";
    case Icon::kHorn: return "horn";
    case Icon::kEye: return "eye";
    case Icon::kHandbrake: return "handbrake";
    case Icon::kGas: return "gas";
    case Icon::kBrake: return "brake";
    case Icon::kUp: return "up";
    case Icon::kDown: return "down";
    case Icon::kLeft: return "left";
    case Icon::kRight: return "right";
    case Icon::kCheck: return "check";
    case Icon::kBack: return "back";
    case Icon::kHangUp: return "hang_up";
    case Icon::kLB: return "lb";
    case Icon::kRB: return "rb";
    case Icon::kReset: return "reset";
    case Icon::kSkip: return "skip";
    default: return nullptr;
  }
}

std::atomic<uint32_t> g_pause_flag{0};
std::atomic<uint32_t> g_menu_screen{0};

LibertyIconResolver g_icon_resolver = nullptr;
void* g_icon_context = nullptr;

// Accent colours (ring and some icons).
constexpr ImU32 kWhite = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kGreen = IM_COL32(125, 228, 140, 255);
constexpr ImU32 kRed = IM_COL32(246, 104, 98, 255);
constexpr ImU32 kYellow = IM_COL32(246, 207, 92, 255);
constexpr ImU32 kOrange = IM_COL32(240, 132, 86, 255);

// A control as designed on a 1600x737 reference screen. Positions are in
// "design units" (1/737 of the screen height) from the anchor edge, so the
// layout keeps its shape on any aspect ratio.
struct Spec {
  const char* id;
  Kind kind;
  Anchor anchor;
  float x, y, r;
  Icon icon;
  const char* label;
  uint16_t button;  // pad button; trigger: 0 = left, 1 = right; stick: 0/1; steer: -1/+1 via sign
  ImU32 accent;
  ImU32 icon_color;
};

constexpr uint16_t kLT = 0, kRT = 1;

// ---------------- layouts (from the approved v3 mockups) ----------------
// Mapping from the Xbox 360 GTA IV defaults: A sprint/select, B reload/melee/
// back, X jump, Y enter/exit vehicle, RB cover/handbrake, LB drive-by, L3
// crouch/horn, R3 look back, LT aim/brake, RT fire/gas, D-pad weapons/phone.
const std::vector<Spec>& Layout(Screen screen) {
  static const std::vector<Spec> on_foot = {
      {"pause", Kind::kButton, Anchor::kLeft, 62, 58, 32, Icon::kHouse, "PAUSE", X_INPUT_GAMEPAD_START, 0, kWhite},
      {"cheats", Kind::kButton, Anchor::kCenter, 200, 58, 30, Icon::kCheats, "CHEATS", 0, 0, kWhite},
      {"phone", Kind::kButton, Anchor::kCenter, 280, 58, 30, Icon::kPhone, "PHONE", X_INPUT_GAMEPAD_DPAD_UP, 0, kWhite},
      {"view", Kind::kButton, Anchor::kCenter, 360, 58, 30, Icon::kView, "VIEW", X_INPUT_GAMEPAD_BACK, 0, kWhite},
      {"switch", Kind::kButton, Anchor::kCenter, 440, 58, 30, Icon::kGamepad, "XBOX", 0, 0, kWhite},
      {"move", Kind::kStick, Anchor::kLeft, 400, 540, 112, Icon::kNone, "MOVE", 0, 0, kWhite},
      {"crouch", Kind::kButton, Anchor::kLeft, 92, 260, 34, Icon::kCrouch, "CROUCH", X_INPUT_GAMEPAD_LEFT_THUMB, 0, kWhite},
      {"cover", Kind::kButton, Anchor::kLeft, 92, 375, 34, Icon::kCover, "COVER", X_INPUT_GAMEPAD_RIGHT_SHOULDER, 0, kWhite},
      {"weapon_prev", Kind::kButton, Anchor::kRight, 251, 140, 22, Icon::kLeft, nullptr, X_INPUT_GAMEPAD_DPAD_LEFT, 0, kWhite},
      {"weapon", Kind::kButton, Anchor::kRight, 175, 140, 44, Icon::kPistol, "WEAPON", X_INPUT_GAMEPAD_DPAD_RIGHT, 0, kWhite},
      {"weapon_next", Kind::kButton, Anchor::kRight, 99, 140, 22, Icon::kRight, nullptr, X_INPUT_GAMEPAD_DPAD_RIGHT, 0, kWhite},
      {"enter", Kind::kButton, Anchor::kRight, 80, 262, 38, Icon::kDoor, "ENTER CAR", X_INPUT_GAMEPAD_Y, 0, kWhite},
      {"melee", Kind::kButton, Anchor::kRight, 255, 262, 34, Icon::kFist, "MELEE", X_INPUT_GAMEPAD_B, 0, kWhite},
      {"reload", Kind::kButton, Anchor::kRight, 395, 375, 32, Icon::kReload, "RELOAD", X_INPUT_GAMEPAD_B, 0, kWhite},
      {"aim", Kind::kTrigger, Anchor::kRight, 265, 395, 48, Icon::kAim, "AIM", kLT, 0, kWhite},
      {"shoot", Kind::kTrigger, Anchor::kRight, 95, 385, 56, Icon::kBullet, "SHOOT", kRT, kOrange, kWhite},
      {"jump", Kind::kButton, Anchor::kRight, 365, 505, 42, Icon::kJump, "JUMP", X_INPUT_GAMEPAD_X, 0, kWhite},
      {"sprint", Kind::kButton, Anchor::kRight, 365, 650, 40, Icon::kRun, "SPRINT", X_INPUT_GAMEPAD_A, 0, kWhite},
      {"camera", Kind::kStick, Anchor::kRight, 170, 590, 88, Icon::kView, "CAMERA", 1, 0, kWhite},
  };
  static const std::vector<Spec> driving = {
      {"pause", Kind::kButton, Anchor::kLeft, 62, 58, 32, Icon::kHouse, "PAUSE", X_INPUT_GAMEPAD_START, 0, kWhite},
      {"cheats", Kind::kButton, Anchor::kCenter, 200, 58, 30, Icon::kCheats, "CHEATS", 0, 0, kWhite},
      {"phone", Kind::kButton, Anchor::kCenter, 280, 58, 30, Icon::kPhone, "PHONE", X_INPUT_GAMEPAD_DPAD_UP, 0, kWhite},
      {"view", Kind::kButton, Anchor::kCenter, 360, 58, 30, Icon::kView, "VIEW", X_INPUT_GAMEPAD_BACK, 0, kWhite},
      {"switch", Kind::kButton, Anchor::kCenter, 440, 58, 30, Icon::kGamepad, "XBOX", 0, 0, kWhite},
      {"steer_left", Kind::kSteer, Anchor::kLeft, 330, 585, 64, Icon::kSteerLeft, "STEER LEFT", 0, 0, kWhite},
      {"steer_right", Kind::kSteer, Anchor::kLeft, 505, 585, 64, Icon::kSteerRight, "STEER RIGHT", 1, 0, kWhite},
      {"radio", Kind::kButton, Anchor::kLeft, 92, 260, 34, Icon::kRadio, "RADIO", X_INPUT_GAMEPAD_DPAD_RIGHT, 0, kWhite},
      {"horn", Kind::kButton, Anchor::kLeft, 92, 375, 34, Icon::kHorn, "HORN", X_INPUT_GAMEPAD_LEFT_THUMB, 0, kWhite},
      {"weapon", Kind::kButton, Anchor::kRight, 175, 140, 44, Icon::kPistol, "WEAPON", X_INPUT_GAMEPAD_X, 0, kWhite},
      {"exit", Kind::kButton, Anchor::kRight, 80, 262, 38, Icon::kDoor, "EXIT CAR", X_INPUT_GAMEPAD_Y, 0, kWhite},
      {"look_back", Kind::kButton, Anchor::kRight, 410, 460, 36, Icon::kEye, "LOOK BACK", X_INPUT_GAMEPAD_RIGHT_THUMB, 0, kWhite},
      {"drive_by", Kind::kButton, Anchor::kRight, 100, 405, 38, Icon::kBullet, "DRIVE-BY", X_INPUT_GAMEPAD_LEFT_SHOULDER, kOrange, kWhite},
      {"handbrake", Kind::kButton, Anchor::kRight, 260, 455, 44, Icon::kHandbrake, "HANDBRAKE", X_INPUT_GAMEPAD_RIGHT_SHOULDER, kYellow, kYellow},
      {"brake", Kind::kTrigger, Anchor::kRight, 270, 620, 56, Icon::kBrake, "BRAKE", kLT, kRed, kRed},
      {"gas", Kind::kTrigger, Anchor::kRight, 100, 575, 70, Icon::kGas, "GAS", kRT, kGreen, kGreen},
      {"camera", Kind::kStick, Anchor::kRight, 440, 615, 72, Icon::kView, "CAMERA", 1, 0, kWhite},
  };
  static const std::vector<Spec> phone = {
      {"pause", Kind::kButton, Anchor::kLeft, 62, 58, 32, Icon::kHouse, "PAUSE", X_INPUT_GAMEPAD_START, 0, kWhite},
      {"up", Kind::kButton, Anchor::kLeft, 260, 400, 44, Icon::kUp, "UP", X_INPUT_GAMEPAD_DPAD_UP, 0, kWhite},
      {"down", Kind::kButton, Anchor::kLeft, 260, 600, 44, Icon::kDown, "DOWN", X_INPUT_GAMEPAD_DPAD_DOWN, 0, kWhite},
      {"left", Kind::kButton, Anchor::kLeft, 160, 500, 44, Icon::kLeft, "LEFT", X_INPUT_GAMEPAD_DPAD_LEFT, 0, kWhite},
      {"right", Kind::kButton, Anchor::kLeft, 360, 500, 44, Icon::kRight, "RIGHT", X_INPUT_GAMEPAD_DPAD_RIGHT, 0, kWhite},
      {"select", Kind::kButton, Anchor::kRight, 130, 560, 62, Icon::kCheck, "SELECT", X_INPUT_GAMEPAD_A, kGreen, kGreen},
      {"back", Kind::kButton, Anchor::kRight, 295, 620, 48, Icon::kBack, "BACK", X_INPUT_GAMEPAD_B, kRed, kRed},
      {"hang_up", Kind::kButton, Anchor::kRight, 270, 455, 40, Icon::kHangUp, "HANG UP", X_INPUT_GAMEPAD_B, kRed, kRed},
  };
  static const std::vector<Spec> menu = {
      {"prev_tab", Kind::kButton, Anchor::kLeft, 215, 290, 38, Icon::kLB, "PREV TAB", X_INPUT_GAMEPAD_LEFT_SHOULDER, 0, kWhite},
      {"next_tab", Kind::kButton, Anchor::kRight, 215, 290, 38, Icon::kRB, "NEXT TAB", X_INPUT_GAMEPAD_RIGHT_SHOULDER, 0, kWhite},
      {"up", Kind::kButton, Anchor::kLeft, 200, 425, 42, Icon::kUp, "UP", X_INPUT_GAMEPAD_DPAD_UP, 0, kWhite},
      {"down", Kind::kButton, Anchor::kLeft, 200, 615, 42, Icon::kDown, "DOWN", X_INPUT_GAMEPAD_DPAD_DOWN, 0, kWhite},
      {"left", Kind::kButton, Anchor::kLeft, 105, 520, 42, Icon::kLeft, "LEFT", X_INPUT_GAMEPAD_DPAD_LEFT, 0, kWhite},
      {"right", Kind::kButton, Anchor::kLeft, 295, 520, 42, Icon::kRight, "RIGHT", X_INPUT_GAMEPAD_DPAD_RIGHT, 0, kWhite},
      {"select", Kind::kButton, Anchor::kRight, 140, 560, 62, Icon::kCheck, "SELECT", X_INPUT_GAMEPAD_A, kGreen, kGreen},
      {"back", Kind::kButton, Anchor::kRight, 300, 620, 48, Icon::kBack, "BACK", X_INPUT_GAMEPAD_B, kRed, kRed},
      {"resume", Kind::kButton, Anchor::kRight, 290, 460, 38, Icon::kMenu, "RESUME", X_INPUT_GAMEPAD_START, 0, kWhite},
  };
  // Skip sends A, B and X together in one short tap (see ReadLibertyTouchOverlay):
  // whichever of them the title uses to skip, the others do nothing in a cutscene.
  static const std::vector<Spec> cutscene = {
      {"pause", Kind::kButton, Anchor::kLeft, 62, 58, 32, Icon::kHouse, "PAUSE", X_INPUT_GAMEPAD_START, 0, kWhite},
      {"skip", Kind::kButton, Anchor::kRight, 140, 600, 60, Icon::kSkip, "SKIP", 0, 0, kWhite},
  };
  static const std::vector<Spec> none;
  switch (screen) {
    case Screen::kOnFoot: return on_foot;
    case Screen::kDriving: return driving;
    case Screen::kPhone: return phone;
    case Screen::kMenu: return menu;
    case Screen::kCutscene: return cutscene;
    default: return none;
  }
}

const char* ScreenName(Screen screen) {
  switch (screen) {
    case Screen::kOnFoot: return "on_foot";
    case Screen::kDriving: return "driving";
    case Screen::kPhone: return "phone";
    case Screen::kMenu: return "menu";
    case Screen::kCutscene: return "cutscene";
    default: return "hidden";
  }
}

// Which screen the game is on right now.
Screen CurrentScreen(bool forced_menu, bool pause_flag_verified, uint32_t* logged) {
  const TouchContextSnapshot context = GetTouchContextSnapshot();
  const uint32_t pause_flag = g_pause_flag.load(std::memory_order_relaxed);
  const bool pause_menu = false;  // flag logged only: it does not track the pause menu reliably
  const uint32_t facts = uint32_t(pause_menu) << 9 | uint32_t(context.valid) | uint32_t(context.frontend) << 1 |
                         uint32_t(context.map) << 2 | uint32_t(context.loading) << 3 |
                         uint32_t(context.cutscene) << 4 | uint32_t(context.phone_visible) << 5 |
                         uint32_t(context.playing) << 6 | uint32_t(context.gameplay_allowed) << 7 |
                         uint32_t(forced_menu) << 8 | uint32_t(context.base_mode) << 12;
  if (logged && *logged != facts) {
    *logged = facts;
    REXLOG_WARN("liberty-touch: pause-flag={:08X} menu-screen={} valid={} frontend={} map={} "
                "loading={} cutscene={} phone={} playing={} gameplay={} forced-menu={} mode={} verified={}",
                pause_flag, g_menu_screen.load(std::memory_order_relaxed), context.valid,
                context.frontend, context.map, context.loading, context.cutscene,
                context.phone_visible, context.playing, context.gameplay_allowed, forced_menu,
                int(context.base_mode), pause_flag_verified);
  }
  if (forced_menu || pause_menu) return Screen::kMenu;
  // Menus first: while a menu is open the player facts are often not
  // readable, so "not valid" must not hide (or replace) the menu controls.
  if (context.frontend || context.map) return Screen::kMenu;
  // Boot, legal and loading screens: no controls until the game is running.
  if (!context.valid || context.loading) return Screen::kHidden;
  if (context.cutscene) return Screen::kCutscene;
  if (context.phone_visible) return Screen::kPhone;
  switch (context.base_mode) {
    case ContextTouchMode::kVehicleAutomobile:
    case ContextTouchMode::kVehicleBike:
    case ContextTouchMode::kVehicleBoat:
    case ContextTouchMode::kVehicleHelicopter:
    case ContextTouchMode::kVehicleDriverUnknown:
    case ContextTouchMode::kVehiclePassenger:
      return Screen::kDriving;
    case ContextTouchMode::kPhone:
      return Screen::kPhone;
    case ContextTouchMode::kFrontend:
    case ContextTouchMode::kMap:
      return Screen::kMenu;
    default:
      return Screen::kOnFoot;
  }
}

// A placed control for this frame (screen pixels).
struct Placed {
  const Spec* spec;
  ImVec2 center;
  float r;
};

struct Finger {
  int control = -1;  // index into placed, -1 = nothing
  ImVec2 grab_offset{0, 0};
};

struct State {
  std::mutex mutex;
  bool visible = false;
  bool editing = false;
  bool layout_loaded = false;
  Screen screen = Screen::kHidden;
  std::vector<Placed> placed;
  std::unordered_map<uint64_t, Finger> fingers;
  std::vector<bool> held;
  std::array<ImVec2, 2> stick{ImVec2(0, 0), ImVec2(0, 0)};  // -1..1, screen axes
  float steer = 0.0f;
  // Custom offsets: "screen/id" -> offset in design units.
  std::map<std::string, ImVec2> offsets;
  // Custom sizes: "screen/id" -> scale (1 = default). Pinch in edit mode.
  std::map<std::string, float> sizes;
  struct Pinch {
    int control = -1;
    float start_distance = 0.0f;
    float start_scale = 1.0f;
  } pinch;
  std::chrono::steady_clock::time_point pause_down{};
  bool pause_long_handled = false;
  bool pause_was_held = false;
  // A short tap on Pause sends Start for a moment after the finger lifts
  // (holding it would open the pause menu before edit mode could start).
  std::chrono::steady_clock::time_point start_pulse_until{};
  // Skip: A + B + X for a moment after each new press.
  std::chrono::steady_clock::time_point skip_pulse_until{};
  bool skip_was_held = false;
  bool switch_was_held = false;
  // Pause-menu fallback: the title's frontend flag does not cover every menu,
  // so tapping PAUSE also switches to the menu controls until RESUME.
  bool forced_menu = false;
  std::chrono::steady_clock::time_point hidden_since{};  // when the game last started to report no screen
  bool forced_saw_pause = false;  // gameplay stopped after the Pause tap (menu opened)
  std::chrono::steady_clock::time_point gameplay_since{};
  // The pause flag is only trusted once it has been seen to rise (0 -> non-zero)
  // right after a Pause tap; until then a wrong flag cannot pin the menu.
  bool pause_flag_verified = false;
  uint32_t pause_flag_before_tap = 0;
  std::chrono::steady_clock::time_point pause_check_until{};
  bool resume_was_held = false;
  // Fallback when the pause flag cannot be verified: Select goes one level
  // into the menu, Back one level out; Back at the top level closes it.
  int menu_depth = 0;
  bool select_was_held = false;
  bool back_was_held = false;
  // Diagnostics: last logged context facts.
  uint32_t logged_context = 0xFFFFFFFFu;
  // Swipe camera: one finger on empty screen drives the right stick by its speed.
  bool look_active = false;
  uint64_t look_key = 0;
  ImVec2 look_last{0, 0};
  ImVec2 look_pending{0, 0};  // finger travel not yet delivered, in seconds of full stick
  ImVec2 look_out{0, 0};      // the axis sent to the title this frame, -1..1, screen axes
  std::chrono::steady_clock::time_point look_time{};
  float swipe_sens = 1.0f;    // camera speed, set with the slider in edit mode
  // Cheat buttons: a panel of cheats and the phone-dialing macro they start.
  bool cheat_panel = false;
  bool cheats_was_held = false;
  std::unordered_set<uint64_t> panel_fingers;  // fingers already down when the panel opened/handled
  struct MacroStep {
    uint16_t buttons = 0;
    int hold_ms = 90;
    int gap_ms = 130;
    bool wait_phone = false;  // wait (up to hold_ms) for the phone to be on screen
    std::string note;
  };
  struct Macro {
    bool active = false;
    bool step_started = false;
    size_t index = 0;
    std::string name;
    std::vector<MacroStep> steps;
    std::chrono::steady_clock::time_point step_start{};
  } macro;
};

State& state() {
  static State s;
  return s;
}

std::filesystem::path LayoutPath() {
  return rex::filesystem::GetUserFolder() / "liberty_touch_layout.txt";
}

std::filesystem::path SwipePath() { return rex::filesystem::GetUserFolder() / "liberty_swipe.txt"; }

void SaveSwipe(const State& s) {
  std::ofstream file(SwipePath(), std::ios::trunc);
  file << s.swipe_sens << '\n';
}

void LoadOffsets(State& s) {
  s.layout_loaded = true;
  s.swipe_sens = float(std::clamp(REXCVAR_GET(gta4_touch_swipe_sensitivity), 0.1, 5.0));
  {
    std::ifstream sens_file(SwipePath());
    float value = 0.0f;
    if (sens_file >> value && std::isfinite(value)) s.swipe_sens = std::clamp(value, 0.1f, 5.0f);
  }
  std::ifstream file(LayoutPath());
  std::string line;
  while (std::getline(file, line)) {
    std::istringstream in(line);
    std::string key;
    float dx = 0, dy = 0;
    if (in >> key >> dx >> dy && std::isfinite(dx) && std::isfinite(dy)) {
      s.offsets[key] = ImVec2(std::clamp(dx, -2000.0f, 2000.0f), std::clamp(dy, -800.0f, 800.0f));
      float scale = 1.0f;
      if (in >> scale && std::isfinite(scale)) s.sizes[key] = std::clamp(scale, 0.5f, 2.0f);
    }
  }
}

void SaveOffsets(const State& s) {
  std::ofstream file(LayoutPath(), std::ios::trunc);
  file << "# Liberty touch layout: <screen>/<control> <dx> <dy> <scale> (design units)\n";
  std::map<std::string, bool> keys;
  for (const auto& [key, offset] : s.offsets) keys[key] = true;
  for (const auto& [key, scale] : s.sizes) keys[key] = true;
  for (const auto& [key, unused] : keys) {
    (void)unused;
    const auto offset_it = s.offsets.find(key);
    const ImVec2 offset = offset_it != s.offsets.end() ? offset_it->second : ImVec2(0, 0);
    const auto size_it = s.sizes.find(key);
    const float scale = size_it != s.sizes.end() ? size_it->second : 1.0f;
    if (offset.x == 0.0f && offset.y == 0.0f && scale == 1.0f) continue;
    file << key << ' ' << offset.x << ' ' << offset.y << ' ' << scale << '\n';
  }
}

float Scale() { return float(std::clamp(REXCVAR_GET(gta4_touch_gamepad_scale), 0.75, 1.35)); }
float Opacity() { return float(std::clamp(REXCVAR_GET(gta4_touch_gamepad_opacity), 0.3, 1.6)); }

ImU32 Alpha(ImU32 color, float alpha) {
  const float a = float((color >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f);
  return (color & ~IM_COL32_A_MASK) | (uint32_t(a) << IM_COL32_A_SHIFT);
}

void Place(State& s, float w, float h) {
  s.placed.clear();
  const float unit = h / 737.0f, k = Scale();
  const bool swipe_camera = REXCVAR_GET(gta4_touch_camera) != "stick";
  for (const Spec& spec : Layout(s.screen)) {
    if (swipe_camera && std::string_view(spec.id) == "camera") continue;  // the swipe area replaces it
    ImVec2 offset(0, 0);
    if (auto it = s.offsets.find(std::string(ScreenName(s.screen)) + "/" + spec.id); it != s.offsets.end()) {
      offset = it->second;
    }
    float x = spec.x + offset.x;
    float px = spec.anchor == Anchor::kLeft    ? x * unit
               : spec.anchor == Anchor::kRight ? w - x * unit
                                               : w * 0.5f + x * unit;
    float custom = 1.0f;
    if (auto it = s.sizes.find(std::string(ScreenName(s.screen)) + "/" + spec.id); it != s.sizes.end()) {
      custom = it->second;
    }
    const float r = spec.r * unit * k * custom;
    px = std::clamp(px, r, w - r);
    const float py = std::clamp((spec.y + offset.y) * unit, r, h - r);
    s.placed.push_back({&spec, ImVec2(px, py), r});
  }
  if (s.editing) {  // Reset button for the current screen, centre of the screen
    static const Spec reset{"reset", Kind::kReset, Anchor::kCenter, 0, 300, 40, Icon::kReset, "RESET", 0, kYellow, kYellow};
    s.placed.push_back({&reset, ImVec2(w * 0.5f, 300 * unit), 40 * unit});
  }
  s.held.assign(s.placed.size(), false);
}

int HitTest(const State& s, ImVec2 p) {
  int best = -1;
  float best_score = FLT_MAX;
  for (size_t i = 0; i < s.placed.size(); ++i) {
    const auto& c = s.placed[i];
    const float d = std::hypot(p.x - c.center.x, p.y - c.center.y);
    const float capture = c.r * (c.spec->kind == Kind::kStick ? 1.5f : 1.35f);
    if (d <= capture && d / c.r < best_score) {
      best_score = d / c.r;
      best = int(i);
    }
  }
  return best;
}

// ---------------- drawing ----------------
// Each icon is drawn twice: a dark, slightly thicker pass (outline) and the
// coloured pass, like the mockups' outlined white icons.
struct Pen {
  ImDrawList* d;
  ImU32 col;
  float grow;  // extra half-thickness for the outline pass
  void Line(ImVec2 a, ImVec2 b, float w) const { d->AddLine(a, b, col, w + 2 * grow); }
  void Disc(ImVec2 c, float r) const { d->AddCircleFilled(c, r + grow, col, 24); }
  void Ring(ImVec2 c, float r, float w) const { d->AddCircle(c, r, col, 32, w + 2 * grow); }
  void Rect(ImVec2 a, ImVec2 b, float rounding = 0) const {
    d->AddRectFilled(ImVec2(a.x - grow, a.y - grow), ImVec2(b.x + grow, b.y + grow), col, rounding);
  }
  void Tri(ImVec2 a, ImVec2 b, ImVec2 c) const {
    d->AddTriangleFilled(a, b, c, col);
    if (grow > 0) {
      ImVec2 pts[3] = {a, b, c};
      d->AddPolyline(pts, 3, col, ImDrawFlags_Closed, 2 * grow);
    }
  }
  void Poly(const ImVec2* pts, int n) const {
    d->AddConvexPolyFilled(pts, n, col);
    if (grow > 0) d->AddPolyline(pts, n, col, ImDrawFlags_Closed, 2 * grow);
  }
  void Arc(ImVec2 c, float r, float a0, float a1, float w) const {
    d->PathArcTo(c, r, a0, a1, 20);
    d->PathStroke(col, 0, w + 2 * grow);
  }
};

ImVec2 P(ImVec2 c, float r, float x, float y) { return ImVec2(c.x + x * r, c.y + y * r); }

void Limb(const Pen& p, std::initializer_list<ImVec2> pts, float w) {
  const ImVec2* prev = nullptr;
  for (const ImVec2& pt : pts) {
    if (prev) p.Line(*prev, pt, w);
    p.Disc(pt, w * 0.5f);
    prev = &pt;
  }
}

void DrawIcon(const Pen& p, Icon icon, ImVec2 c, float r, ImU32 hole) {
  const float w = r * 0.2f;
  switch (icon) {
    case Icon::kMenu:
      for (int i = -1; i <= 1; ++i) p.Rect(P(c, r, -0.6f, i * 0.38f - 0.1f), P(c, r, 0.6f, i * 0.38f + 0.1f), r * 0.08f);
      break;
    case Icon::kPhone:
      p.Rect(P(c, r, -0.42f, -0.82f), P(c, r, 0.42f, 0.82f), r * 0.14f);
      if (p.grow == 0) p.d->AddRectFilled(P(c, r, -0.28f, -0.6f), P(c, r, 0.28f, 0.25f), hole);
      break;
    case Icon::kView:
      p.Rect(P(c, r, -0.75f, -0.3f), P(c, r, 0.22f, 0.48f), r * 0.1f);
      p.Tri(P(c, r, 0.28f, 0.09f), P(c, r, 0.78f, -0.25f), P(c, r, 0.78f, 0.43f));
      p.Disc(P(c, r, -0.5f, -0.58f), r * 0.2f);
      p.Disc(P(c, r, -0.08f, -0.58f), r * 0.2f);
      break;
    case Icon::kCrouch:
      p.Disc(P(c, r, 0.12f, -0.53f), r * 0.17f);
      Limb(p, {P(c, r, 0.05f, -0.25f), P(c, r, -0.3f, 0.18f)}, w * 1.25f);
      Limb(p, {P(c, r, -0.3f, 0.18f), P(c, r, 0.22f, 0.32f), P(c, r, 0.15f, 0.82f)}, w);
      Limb(p, {P(c, r, -0.3f, 0.18f), P(c, r, -0.55f, 0.82f)}, w);
      Limb(p, {P(c, r, -0.05f, -0.12f), P(c, r, 0.45f, 0.02f)}, w * 0.85f);
      break;
    case Icon::kCover:
      p.Rect(P(c, r, 0.3f, -0.75f), P(c, r, 0.75f, 0.85f));
      p.Disc(P(c, r, -0.16f, -0.39f), r * 0.16f);
      Limb(p, {P(c, r, -0.12f, -0.15f), P(c, r, -0.2f, 0.3f)}, w * 1.2f);
      Limb(p, {P(c, r, -0.2f, 0.3f), P(c, r, -0.55f, 0.45f), P(c, r, -0.45f, 0.82f)}, w);
      Limb(p, {P(c, r, -0.2f, 0.3f), P(c, r, 0.1f, 0.82f)}, w);
      Limb(p, {P(c, r, -0.12f, -0.05f), P(c, r, 0.22f, -0.05f)}, w * 0.85f);
      break;
    case Icon::kPistol: {
      p.Rect(P(c, r, -0.88f, -0.46f), P(c, r, 0.88f, -0.08f), r * 0.04f);
      const ImVec2 grip[4] = {P(c, r, -0.62f, -0.08f), P(c, r, -0.12f, -0.08f), P(c, r, -0.32f, 0.82f), P(c, r, -0.78f, 0.82f)};
      p.Poly(grip, 4);
      p.Arc(P(c, r, 0.06f, 0.06f), r * 0.24f, 0.0f, 3.14159f, r * 0.1f);
      break;
    }
    case Icon::kDoor: {
      const ImVec2 body[5] = {P(c, r, -0.78f, -0.05f), P(c, r, -0.3f, -0.72f), P(c, r, 0.7f, -0.72f), P(c, r, 0.7f, 0.75f), P(c, r, -0.78f, 0.75f)};
      p.Poly(body, 5);
      if (p.grow == 0) {
        const ImVec2 win[4] = {P(c, r, -0.52f, -0.1f), P(c, r, -0.18f, -0.55f), P(c, r, 0.55f, -0.55f), P(c, r, 0.55f, -0.1f)};
        p.d->AddConvexPolyFilled(win, 4, hole);
        p.d->AddRectFilled(P(c, r, 0.18f, 0.12f), P(c, r, 0.52f, 0.25f), hole, r * 0.05f);
      }
      break;
    }
    case Icon::kReload:
      p.Arc(c, r * 0.62f, -0.87f, 4.1f, w);
      p.Tri(P(c, r, 0.33f, -0.93f), P(c, r, 0.78f, -0.38f), P(c, r, 0.1f, -0.3f));
      break;
    case Icon::kFist:
      p.Rect(P(c, r, -0.62f, -0.45f), P(c, r, 0.5f, 0.55f), r * 0.22f);
      if (p.grow == 0)
        for (int i = 0; i < 3; ++i) p.d->AddLine(P(c, r, -0.3f + i * 0.28f, -0.45f), P(c, r, -0.3f + i * 0.28f, -0.02f), hole, r * 0.08f);
      break;
    case Icon::kAim:
      p.Ring(c, r * 0.58f, w * 0.8f);
      for (int i = 0; i < 4; ++i) {
        const float a = i * 1.5708f, ca = std::cos(a), sa = std::sin(a);
        p.Line(ImVec2(c.x + ca * r * 0.3f, c.y + sa * r * 0.3f), ImVec2(c.x + ca * r * 0.98f, c.y + sa * r * 0.98f), w * 0.9f);
      }
      p.Disc(c, w * 0.7f);
      break;
    case Icon::kBullet: {
      // Tilted cartridge: body line with a round nose.
      const float a = -0.9f, ca = std::cos(a), sa = std::sin(a);
      const ImVec2 base(c.x - ca * r * 0.62f, c.y - sa * r * 0.62f), nose(c.x + ca * r * 0.35f, c.y + sa * r * 0.35f);
      p.Line(base, nose, r * 0.52f);
      p.Disc(nose, r * 0.26f);
      p.Disc(ImVec2(c.x + ca * r * 0.5f, c.y + sa * r * 0.5f), r * 0.2f);
      break;
    }
    case Icon::kJump:
      p.Disc(P(c, r, 0.0f, -0.81f), r * 0.17f);
      Limb(p, {P(c, r, 0, -0.55f), P(c, r, 0, 0.05f)}, w * 1.25f);
      Limb(p, {P(c, r, 0, -0.42f), P(c, r, -0.42f, -0.78f)}, w * 0.85f);
      Limb(p, {P(c, r, 0, -0.42f), P(c, r, 0.42f, -0.78f)}, w * 0.85f);
      Limb(p, {P(c, r, 0, 0.05f), P(c, r, -0.35f, 0.3f), P(c, r, -0.25f, 0.62f)}, w);
      Limb(p, {P(c, r, 0, 0.05f), P(c, r, 0.35f, 0.3f), P(c, r, 0.25f, 0.62f)}, w);
      p.Rect(P(c, r, -0.8f, 0.8f), P(c, r, 0.8f, 0.93f), r * 0.06f);
      break;
    case Icon::kRun:
      p.Disc(P(c, r, 0.25f, -0.78f), r * 0.17f);
      Limb(p, {P(c, r, 0.18f, -0.5f), P(c, r, -0.05f, 0.08f)}, w * 1.25f);
      Limb(p, {P(c, r, -0.05f, 0.08f), P(c, r, 0.35f, 0.32f), P(c, r, 0.25f, 0.82f)}, w);
      Limb(p, {P(c, r, -0.05f, 0.08f), P(c, r, -0.35f, 0.45f), P(c, r, -0.72f, 0.4f)}, w);
      Limb(p, {P(c, r, 0.15f, -0.38f), P(c, r, 0.5f, -0.12f), P(c, r, 0.72f, -0.4f)}, w * 0.85f);
      Limb(p, {P(c, r, 0.15f, -0.38f), P(c, r, -0.22f, -0.28f), P(c, r, -0.45f, -0.02f)}, w * 0.85f);
      break;
    case Icon::kSteerLeft:
    case Icon::kSteerRight: {
      const float s = icon == Icon::kSteerLeft ? -1.0f : 1.0f;
      p.Tri(P(c, r, -0.1f * s, -0.75f), P(c, r, 0.62f * s, 0.0f), P(c, r, -0.1f * s, 0.75f));
      p.Rect(P(c, r, std::min(-0.7f * s, -0.1f * s), -0.32f), P(c, r, std::max(-0.7f * s, -0.1f * s), 0.32f));
      break;
    }
    case Icon::kRadio:
      p.Rect(P(c, r, -0.78f, -0.35f), P(c, r, 0.78f, 0.65f), r * 0.14f);
      p.Line(P(c, r, -0.45f, -0.38f), P(c, r, 0.4f, -0.85f), r * 0.11f);
      if (p.grow == 0) {
        p.d->AddCircleFilled(P(c, r, 0.32f, 0.12f), r * 0.24f, hole, 20);
        for (int i = 0; i < 3; ++i) p.d->AddRectFilled(P(c, r, -0.6f, -0.12f + i * 0.17f), P(c, r, -0.1f, -0.05f + i * 0.17f), hole);
      }
      break;
    case Icon::kHorn: {
      const ImVec2 cone[4] = {P(c, r, -0.35f, -0.25f), P(c, r, 0.08f, -0.65f), P(c, r, 0.08f, 0.65f), P(c, r, -0.35f, 0.25f)};
      p.Rect(P(c, r, -0.7f, -0.25f), P(c, r, -0.32f, 0.25f));
      p.Poly(cone, 4);
      p.Arc(P(c, r, 0.08f, 0), r * 0.4f, -0.78f, 0.78f, r * 0.15f);
      p.Arc(P(c, r, 0.08f, 0), r * 0.68f, -0.78f, 0.78f, r * 0.15f);
      break;
    }
    case Icon::kEye: {
      // Almond: two arcs; iris and pupil.
      p.Arc(P(c, r, 0, 0.55f), r * 0.95f, -2.6f, -0.54f, r * 0.14f);
      p.Arc(P(c, r, 0, -0.55f), r * 0.95f, 0.54f, 2.6f, r * 0.14f);
      p.Disc(c, r * 0.3f);
      if (p.grow == 0) p.d->AddCircleFilled(c, r * 0.13f, hole, 16);
      break;
    }
    case Icon::kHandbrake:
      p.Ring(c, r * 0.5f, r * 0.16f);
      p.Arc(c, r * 0.82f, 2.2f, 4.1f, r * 0.14f);
      p.Arc(c, r * 0.82f, -0.95f, 0.95f, r * 0.14f);
      break;
    case Icon::kGas:
    case Icon::kBrake: {
      const bool wide = icon == Icon::kBrake;
      const float hw = wide ? 0.58f : 0.42f, hh = wide ? 0.62f : 0.85f;
      p.Rect(P(c, r, -hw, -hh), P(c, r, hw, hh), r * 0.18f);
      if (p.grow == 0) {
        const int n = wide ? 4 : 6;
        for (int i = 0; i < n; ++i) {
          const float y = -hh + 0.25f + i * (2 * hh - 0.5f) / (n - 1);
          p.d->AddLine(P(c, r, -hw * 0.6f, y + 0.08f), P(c, r, hw * 0.6f, y - 0.08f), hole, r * 0.09f);
        }
      }
      break;
    }
    case Icon::kUp: p.Tri(P(c, r, 0, -0.6f), P(c, r, 0.6f, 0.42f), P(c, r, -0.6f, 0.42f)); break;
    case Icon::kDown: p.Tri(P(c, r, 0, 0.6f), P(c, r, -0.6f, -0.42f), P(c, r, 0.6f, -0.42f)); break;
    case Icon::kLeft: p.Tri(P(c, r, -0.6f, 0), P(c, r, 0.42f, -0.6f), P(c, r, 0.42f, 0.6f)); break;
    case Icon::kRight: p.Tri(P(c, r, 0.6f, 0), P(c, r, -0.42f, 0.6f), P(c, r, -0.42f, -0.6f)); break;
    case Icon::kCheck: Limb(p, {P(c, r, -0.58f, 0.02f), P(c, r, -0.15f, 0.45f), P(c, r, 0.62f, -0.48f)}, r * 0.26f); break;
    case Icon::kBack:
      p.Arc(P(c, r, 0.1f, 0.13f), r * 0.55f, -1.57f, 2.2f, w * 1.2f);
      p.Tri(P(c, r, -0.78f, -0.42f), P(c, r, -0.08f, -0.85f), P(c, r, -0.08f, 0.0f));
      break;
    case Icon::kHangUp:
      p.Arc(P(c, r, 0, 0.32f), r * 0.75f, 3.5f, 5.93f, r * 0.3f);
      p.Rect(P(c, r, -0.85f, -0.02f), P(c, r, -0.42f, 0.35f), r * 0.1f);
      p.Rect(P(c, r, 0.42f, -0.02f), P(c, r, 0.85f, 0.35f), r * 0.1f);
      break;
    case Icon::kHouse: {
      const ImVec2 roof[3] = {P(c, r, 0.0f, -0.82f), P(c, r, 0.85f, -0.05f), P(c, r, -0.85f, -0.05f)};
      p.Poly(roof, 3);
      p.Rect(P(c, r, -0.58f, -0.1f), P(c, r, 0.58f, 0.75f));
      if (p.grow == 0) p.d->AddRectFilled(P(c, r, -0.16f, 0.25f), P(c, r, 0.16f, 0.75f), hole);
      break;
    }
    case Icon::kGamepad:
      p.Rect(P(c, r, -0.85f, -0.42f), P(c, r, 0.85f, 0.42f), r * 0.4f);
      p.Disc(P(c, r, -0.55f, 0.35f), r * 0.32f);
      p.Disc(P(c, r, 0.55f, 0.35f), r * 0.32f);
      if (p.grow == 0) {
        p.d->AddRectFilled(P(c, r, -0.62f, -0.07f), P(c, r, -0.28f, 0.07f), hole);
        p.d->AddRectFilled(P(c, r, -0.52f, -0.17f), P(c, r, -0.38f, 0.17f), hole);
        p.d->AddCircleFilled(P(c, r, 0.38f, -0.05f), r * 0.09f, hole, 12);
        p.d->AddCircleFilled(P(c, r, 0.58f, -0.18f), r * 0.09f, hole, 12);
        p.d->AddCircleFilled(P(c, r, 0.58f, 0.08f), r * 0.09f, hole, 12);
      }
      break;
    case Icon::kSkip:
      p.Tri(P(c, r, -0.78f, -0.6f), P(c, r, 0.02f, 0.0f), P(c, r, -0.78f, 0.6f));
      p.Tri(P(c, r, -0.08f, -0.6f), P(c, r, 0.72f, 0.0f), P(c, r, -0.08f, 0.6f));
      p.Rect(P(c, r, 0.72f, -0.6f), P(c, r, 0.88f, 0.6f));
      break;
    case Icon::kReset:
      p.Arc(c, r * 0.58f, -0.5f, 4.4f, w);
      p.Tri(P(c, r, 0.62f, -0.6f), P(c, r, 0.75f, 0.0f), P(c, r, 0.2f, -0.25f));
      break;
    default:
      break;
  }
}

void DrawText(ImDrawList* d, ImFont* font, float size, ImVec2 top_center, const char* text, float alpha) {
  const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  const ImVec2 pos(top_center.x - extent.x * 0.5f, top_center.y);
  const ImU32 dark = Alpha(IM_COL32(8, 8, 10, 230), alpha);
  for (int dx = -1; dx <= 1; ++dx)
    for (int dy = -1; dy <= 1; ++dy)
      if (dx || dy) d->AddText(font, size, ImVec2(pos.x + dx * 1.5f, pos.y + dy * 1.5f), dark, text);
  d->AddText(font, size, pos, Alpha(kWhite, alpha * 0.95f), text);
}

// Reference style: an almost transparent disc, a thin light-grey ring, and a
// large light-grey icon. Held controls brighten.
constexpr ImU32 kRing = IM_COL32(225, 225, 225, 170);
constexpr ImU32 kIconGrey = IM_COL32(222, 222, 222, 205);

void DrawRing(ImDrawList* d, ImVec2 c, float r, float alpha, bool held) {
  d->AddCircleFilled(c, r, Alpha(IM_COL32(0, 0, 0, held ? 70 : 28), alpha), 48);
  d->AddCircle(c, r, Alpha(IM_COL32(0, 0, 0, 60), alpha), 48, std::max(3.0f, r * 0.06f));
  d->AddCircle(c, r, Alpha(held ? IM_COL32(255, 255, 255, 235) : kRing, alpha), 48, std::max(2.0f, r * 0.035f));
}

void DrawControl(ImDrawList* d, ImFont* font, const State& s, size_t i, float unit) {
  const Placed& c = s.placed[i];
  const Spec& spec = *c.spec;
  const float alpha = std::clamp(Opacity(), 0.0f, 1.0f);
  const bool held = i < s.held.size() && s.held[i];
  const ImU32 hole = IM_COL32(0, 0, 0, 0);
  if (spec.kind == Kind::kStick) {
    DrawRing(d, c.center, c.r, alpha * 0.9f, false);
    const ImVec2 v = s.stick[spec.button];
    const ImVec2 knob(c.center.x + v.x * c.r * 0.55f, c.center.y + v.y * c.r * 0.55f);
    d->AddCircleFilled(knob, c.r * 0.34f, Alpha(IM_COL32(215, 215, 215, held ? 200 : 150), alpha), 40);
    d->AddCircle(knob, c.r * 0.34f, Alpha(IM_COL32(0, 0, 0, 50), alpha), 40, 2.0f);
  } else {
    DrawRing(d, c.center, c.r, alpha, held);
    const float ir = c.r * 0.66f;
    const ImU32 tint = Alpha(held ? IM_COL32(255, 255, 255, 240) : kIconGrey, alpha);
    ContextTouchIcon image;
    const char* file = IconFile(spec.icon);
    const bool drew_image =
        file && g_icon_resolver && g_icon_resolver(g_icon_context, file, &image) && image.texture;
    if (drew_image) {
      // Icon PNG: square, about 82% of the button's diameter.
      const float half = c.r * 0.82f;
      d->AddImage(image.texture, ImVec2(c.center.x - half, c.center.y - half),
                  ImVec2(c.center.x + half, c.center.y + half), image.uv_min, image.uv_max, tint);
    } else {
      const Pen fill{d, tint, 0.0f};
      // Cut-outs (windows, grooves) show the dark disc behind the icon.
      DrawIcon(fill, spec.icon, c.center, ir, Alpha(IM_COL32(20, 20, 22, 120), alpha));
    }
    if (!drew_image && (spec.icon == Icon::kHandbrake || spec.icon == Icon::kLB ||
                        spec.icon == Icon::kRB || spec.icon == Icon::kCheats)) {
      const char* text = spec.icon == Icon::kHandbrake ? "P"
                         : spec.icon == Icon::kLB      ? "LB"
                         : spec.icon == Icon::kCheats  ? "CH"
                                                       : "RB";
      const float size = ir * (spec.icon == Icon::kHandbrake ? 1.0f : 0.9f);
      const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
      d->AddText(font, size, ImVec2(c.center.x - extent.x * 0.5f, c.center.y - extent.y * 0.5f),
                 Alpha(kIconGrey, alpha), text);
    }
  }
  (void)hole;
  if (spec.label && REXCVAR_GET(gta4_touch_labels)) {
    const float size = std::max(11.0f, std::min(c.r * 0.3f, 17.0f * unit * 1.2f));
    DrawText(d, font, size, ImVec2(c.center.x, c.center.y + c.r + 4.0f), spec.label, alpha);
  }
  if (s.editing && spec.kind != Kind::kReset) {
    d->AddCircle(c.center, c.r * 1.12f, IM_COL32(246, 207, 92, 220), 40, 2.5f);
  }
}

void DrawAmmo(ImDrawList* d, ImFont* font, const State& s, float unit) {
  const TouchContextSnapshot context = GetTouchContextSnapshot();
  if (!context.clip_known || !context.armed) return;
  for (const auto& c : s.placed) {
    if (std::string_view(c.spec->id) == "weapon") {
      char text[24];
      std::snprintf(text, sizeof(text), "%u", unsigned(context.clip_ammo));
      DrawText(d, font, 17.0f * unit * 1.2f, ImVec2(c.center.x, c.center.y - c.r - 26.0f * unit), text, 1.0f);
    }
  }
}


// ---------------- cheat buttons ----------------
// GTA IV cheats are phone numbers. A cheat button takes the phone out, opens the
// keypad, moves the keypad cursor with D-pad presses to each digit and presses
// select, as a player would. The cursor movement is an assumption about the
// phone's keypad, so everything that could differ is a setting in
// liberty_cheats.txt (in the user folder), and every step is logged.
struct CheatConfig {
  std::vector<std::string> rows{"123", "456", "789", "*0#"};
  bool home = true;   // press Up and Left first to reach the top-left key (grid does not wrap)
  bool wrap = false;  // the cursor wraps around the edges of the grid
  char start = '5';   // key under the cursor when the keypad opens (used when home = false)
  std::string open_phone = "up";
  std::string open_keypad = "up";
  std::string select = "a";
  std::string tail;  // buttons pressed after the last digit, e.g. "a" or "down a" (empty = none)
  int hold_ms = 90;
  int gap_ms = 130;
  int open_wait_ms = 1500;
  std::vector<std::pair<std::string, std::string>> cheats;
};

const char* kDefaultCheatFile = R"(# liberty_cheats v4
# Liberty cheat buttons (the CH button at the top of the screen in game).
# Every cheat below is instant: the app calls the game's own script functions.
# Save your game first. Single player only.
#
#   Name = @action          instant cheat
#   Name = 362-555-0100     dial a phone number instead (takes the phone out,
#                           opens the keypad and presses digits with the D-pad)
#
# Actions: @health_armor  @health_armor_ammo  @weapons_1  @weapons_2
#          @wanted_clear  @wanted_raise  @weather_next
#          @car:<model>   spawns a vehicle in front of you; <model> is its game
#                         name (infernus, comet, ...). "a|b" tries a then b.
#
# Phone dialing settings (only used for lines with a number):
#   home    true: press Up and Left first so the cursor is on the top-left key.
#   wrap    true when moving past an edge jumps to the other side.
#   tail    buttons pressed after the last digit to make the call ("a", "down a").
#   Buttons: up down left right a b x y start back lb rb
home = true
wrap = false
start = 5
rows = 123 456 789 *0#
open_phone = up
open_keypad = up
select = a
tail =
hold_ms = 90
gap_ms = 130
open_wait_ms = 1500

Full health and armor = @health_armor
Health, armor and ammo = @health_armor_ammo
Weapons set 1 = @weapons_1
Weapons set 2 = @weapons_2
Remove wanted level = @wanted_clear
Raise wanted level = @wanted_raise
Change weather = @weather_next
Annihilator (helicopter) = @car:annihilator
FIB Buffalo = @car:fbi|fib
Cognoscenti = @car:cognoscenti
Turismo = @car:turismo
Super GT = @car:supergt
Comet = @car:comet
NRG-900 = @car:nrg900
Sanchez = @car:sanchez
Jetmax (boat) = @car:jetmax
Identify radio song (ZiT) = 948-555-0100
# The Lost and Damned (only spawn inside that episode)
TLAD Innovation = @car:innovation
TLAD Hexer = @car:hexer
TLAD Double T Custom = @car:double|doublet|doublecustom
TLAD Hakuchou Custom = @car:hakuchou2|hakuchoucustom|hakuchou
TLAD Slamvan = @car:slamvan
TLAD Gang Burrito = @car:gburrito|gangburrito|burrito2
# The Ballad of Gay Tony (only spawn inside that episode)
TBoGT Bullet GT = @car:bullet
TBoGT APC = @car:apc
TBoGT Buzzard = @car:buzzard
TBoGT Akuma = @car:akuma
TBoGT Vader = @car:vader
TBoGT Floater = @car:floater
# No script function found for these: the app dials the phone code for you
TBoGT Parachute = 359-555-7272
TBoGT Explosive punches = 276-555-2666
TBoGT Explosive sniper = 486-555-2526
TBoGT Weapons = 486-555-0100
)";

std::filesystem::path CheatPath() { return rex::filesystem::GetUserFolder() / "liberty_cheats.txt"; }

std::string Trim(std::string text) {
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
  text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
  return text;
}

const CheatConfig& Cheats() {
  static CheatConfig config;
  static bool loaded = false;
  if (loaded) return config;
  loaded = true;
  const auto path = CheatPath();
  std::error_code ignored;
  bool write_defaults = !std::filesystem::exists(path, ignored);
  if (!write_defaults) {
    // A file from before every cheat was instant: keep it as liberty_cheats.old.txt.
    std::ifstream existing(path);
    const std::string content((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
    if (content.find("liberty_cheats v4") == std::string::npos) {
      existing.close();
      std::filesystem::rename(path, path.parent_path() / "liberty_cheats.old.txt", ignored);
      write_defaults = true;
    }
  }
  if (write_defaults) {
    std::ofstream out(path);
    out << kDefaultCheatFile;
  }
  std::ifstream in(path);
  std::string line;
  bool any_cheat = false;
  while (std::getline(in, line)) {
    line = Trim(line);
    if (line.empty() || line[0] == '#') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
    const auto number = [&](int fallback) {
      try { return std::stoi(value); } catch (...) { return fallback; }
    };
    if (key == "home") config.home = value == "true";
    else if (key == "wrap") config.wrap = value == "true";
    else if (key == "start") { if (!value.empty()) config.start = value[0]; }
    else if (key == "rows") {
      std::istringstream ss(value);
      std::vector<std::string> rows;
      for (std::string row; ss >> row;) rows.push_back(row);
      if (!rows.empty()) config.rows = rows;
    }
    else if (key == "open_phone") config.open_phone = value;
    else if (key == "open_keypad") config.open_keypad = value;
    else if (key == "select") config.select = value;
    else if (key == "tail") config.tail = value;
    else if (key == "hold_ms") config.hold_ms = std::clamp(number(90), 30, 500);
    else if (key == "gap_ms") config.gap_ms = std::clamp(number(130), 30, 1000);
    else if (key == "open_wait_ms") config.open_wait_ms = std::clamp(number(1500), 200, 5000);
    else if (!value.empty() && (value[0] == '@' || value.find_first_not_of("0123456789-*# ") == std::string::npos)) {
      config.cheats.emplace_back(key, value);
      any_cheat = true;
    }
  }
  if (!any_cheat) {  // unreadable or empty file: fall back to the built-in list
    std::istringstream defaults(kDefaultCheatFile);
    while (std::getline(defaults, line)) {
      const size_t eq = line.find('=');
      if (eq == std::string::npos || line[0] == '#') continue;
      const std::string key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
      if (!value.empty() && (value[0] == '@' || value.find_first_not_of("0123456789-*# ") == std::string::npos)) {
        config.cheats.emplace_back(key, value);
      }
    }
  }
  REXLOG_WARN("liberty-cheat: {} cheats loaded from {} (home={} wrap={} tail='{}')",
              config.cheats.size(), path.string(), config.home, config.wrap, config.tail);
  return config;
}

uint16_t ButtonByName(const std::string& name) {
  static const std::unordered_map<std::string, uint16_t> map = {
      {"up", X_INPUT_GAMEPAD_DPAD_UP},       {"down", X_INPUT_GAMEPAD_DPAD_DOWN},
      {"left", X_INPUT_GAMEPAD_DPAD_LEFT},   {"right", X_INPUT_GAMEPAD_DPAD_RIGHT},
      {"a", X_INPUT_GAMEPAD_A},              {"b", X_INPUT_GAMEPAD_B},
      {"x", X_INPUT_GAMEPAD_X},              {"y", X_INPUT_GAMEPAD_Y},
      {"start", X_INPUT_GAMEPAD_START},      {"back", X_INPUT_GAMEPAD_BACK},
      {"lb", X_INPUT_GAMEPAD_LEFT_SHOULDER}, {"rb", X_INPUT_GAMEPAD_RIGHT_SHOULDER},
  };
  const auto it = map.find(name);
  return it == map.end() ? 0 : it->second;
}

void StartCheat(State& s, const std::string& name, const std::string& number) {
  if (!number.empty() && number[0] == '@') {  // instant: the game's own natives, no phone
    const bool ready = gta4::directcheats::Request(number.substr(1));
    REXLOG_WARN("liberty-cheat: instant '{}' ({}) requested, gateways ready: {}", name, number, ready);
    return;
  }
  const CheatConfig& c = Cheats();
  auto press = [&](std::vector<State::MacroStep>& steps, const std::string& button,
                   const std::string& note, int gap) {
    State::MacroStep step;
    step.buttons = ButtonByName(button);
    step.hold_ms = c.hold_ms;
    step.gap_ms = gap;
    step.note = note;
    steps.push_back(step);
  };
  std::vector<State::MacroStep> steps;
  const bool phone_open = GetTouchContextSnapshot().phone_visible;
  if (!phone_open) {
    press(steps, c.open_phone, "take the phone out", 200);
    State::MacroStep wait;
    wait.wait_phone = true;
    wait.hold_ms = c.open_wait_ms;
    wait.gap_ms = 350;
    wait.note = "wait for the phone";
    steps.push_back(wait);
  }
  press(steps, c.open_keypad, "open the keypad", 450);
  const int rows = int(c.rows.size()), cols = int(c.rows[0].size());
  int row = 0, col = 0;
  if (c.home) {
    for (int i = 0; i < rows; ++i) press(steps, "up", "home: up", c.gap_ms);
    for (int i = 0; i < cols; ++i) press(steps, "left", "home: left", c.gap_ms);
  } else {
    for (int r = 0; r < rows; ++r) {
      const size_t at = c.rows[r].find(c.start);
      if (at != std::string::npos) { row = r; col = int(at); }
    }
  }
  const auto move = [&](int from, int to, int size, const char* less, const char* more) {
    std::vector<std::string> out;
    int delta = to - from;
    if (c.wrap) {
      delta = ((delta % size) + size) % size;
      if (delta > size / 2) delta -= size;
    }
    for (int i = 0; i < std::abs(delta); ++i) out.push_back(delta < 0 ? less : more);
    return out;
  };
  for (char digit : number) {
    int target_row = -1, target_col = -1;
    for (int r = 0; r < rows; ++r) {
      const size_t at = c.rows[r].find(digit);
      if (at != std::string::npos) { target_row = r; target_col = int(at); }
    }
    if (target_row < 0) continue;  // dashes and spaces
    for (const auto& b : move(col, target_col, cols, "left", "right")) press(steps, b, std::string("move ") + b, c.gap_ms);
    for (const auto& b : move(row, target_row, rows, "up", "down")) press(steps, b, std::string("move ") + b, c.gap_ms);
    row = target_row;
    col = target_col;
    press(steps, c.select, std::string("press ") + digit, c.gap_ms);
  }
  std::istringstream tail(c.tail);
  for (std::string button; tail >> button;) press(steps, button, "tail: " + button, c.gap_ms + 100);
  s.macro = {};
  s.macro.active = true;
  s.macro.name = name;
  s.macro.steps = std::move(steps);
  REXLOG_WARN("liberty-cheat: start '{}' {} ({} steps, phone already open: {})", name, number,
              s.macro.steps.size(), phone_open);
}

void AdvanceMacro(State& s, std::chrono::steady_clock::time_point now, uint16_t* buttons) {
  auto& m = s.macro;
  if (!m.active) return;
  // Touching any control, or the stick, hands control back to the player.
  bool user_input = std::fabs(s.stick[0].x) + std::fabs(s.stick[0].y) + std::fabs(s.stick[1].x) +
                        std::fabs(s.stick[1].y) > 0.2f;
  for (size_t i = 0; i < s.held.size() && i < s.placed.size(); ++i) {
    if (s.held[i] && std::string_view(s.placed[i].spec->id) != "cheats") user_input = true;
  }
  if (s.look_active) user_input = true;
  if (user_input) {
    REXLOG_WARN("liberty-cheat: '{}' cancelled by touch at step {}/{}", m.name, m.index, m.steps.size());
    m = {};
    return;
  }
  if (m.index >= m.steps.size()) {
    REXLOG_WARN("liberty-cheat: '{}' finished", m.name);
    m = {};
    return;
  }
  const State::MacroStep& step = m.steps[m.index];
  if (!m.step_started) {
    m.step_started = true;
    m.step_start = now;
    REXLOG_WARN("liberty-cheat: step {}/{}: {} (phone visible: {})", m.index + 1, m.steps.size(),
                step.note, GetTouchContextSnapshot().phone_visible);
  }
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - m.step_start).count();
  bool next = false;
  if (step.wait_phone) {
    if (GetTouchContextSnapshot().phone_visible) {
      next = elapsed >= step.gap_ms;  // let the phone settle
    } else if (elapsed > step.hold_ms) {
      REXLOG_WARN("liberty-cheat: '{}' stopped, the phone did not appear", m.name);
      m = {};
      return;
    }
  } else if (elapsed < step.hold_ms) {
    *buttons |= step.buttons;
  } else if (elapsed >= step.hold_ms + step.gap_ms) {
    next = true;
  }
  if (next) {
    ++m.index;
    m.step_started = false;
  }
}

// Word-wraps `text` into lines no wider than max_w at `size`.
std::vector<std::string> WrapLines(ImFont* font, float size, const std::string& text, float max_w) {
  std::vector<std::string> lines;
  std::istringstream words(text);
  std::string line, word;
  while (words >> word) {
    const std::string candidate = line.empty() ? word : line + " " + word;
    if (line.empty() || font->CalcTextSizeA(size, FLT_MAX, 0.0f, candidate.c_str()).x <= max_w) {
      line = candidate;
    } else {
      lines.push_back(line);
      line = word;
    }
  }
  if (!line.empty()) lines.push_back(line);
  return lines;
}

// Draws `text` centred in a box: wrapped at the box width and shrunk until it fits.
void DrawFittedText(ImDrawList* d, ImFont* font, float size, ImVec2 center, float max_w, float max_h,
                    const std::string& text, float alpha) {
  std::vector<std::string> lines;
  for (;;) {
    lines = WrapLines(font, size, text, max_w);
    float widest = 0.0f;
    for (const auto& l : lines) widest = std::max(widest, font->CalcTextSizeA(size, FLT_MAX, 0.0f, l.c_str()).x);
    if ((widest <= max_w && float(lines.size()) * size * 1.15f <= max_h) || size <= 9.0f) break;
    size *= 0.92f;
  }
  const float line_h = size * 1.15f;
  float y = center.y - float(lines.size()) * line_h * 0.5f;
  for (const auto& l : lines) {
    DrawText(d, font, size, ImVec2(center.x, y), l.c_str(), alpha);
    y += line_h;
  }
}

// The cheat list: a dark panel with one button per cheat. A tap starts the cheat.
void HandleCheatPanel(State& s, const std::unordered_map<uint64_t, ImVec2>& touches, ImDrawList* d,
                      ImFont* font, float width, float height, float unit) {
  const CheatConfig& c = Cheats();
  const int count = int(c.cheats.size());
  const int cols = count > 20 ? 6 : 4;
  const int rows = std::max(1, (count + cols - 1) / cols);
  const float panel_w = std::min(width * 0.9f, 1100.0f * unit);
  const float panel_h = height * 0.84f;
  const ImVec2 origin((width - panel_w) * 0.5f, (height - panel_h) * 0.5f);
  const float title_h = 56.0f * unit, close_h = 62.0f * unit, pad = 14.0f * unit;
  const float cell_w = (panel_w - pad * (cols + 1)) / cols;
  const float cell_h = std::min(110.0f * unit, (panel_h - title_h - close_h - pad * (rows + 2)) / rows);
  const ImVec2 close_a(origin.x + panel_w * 0.5f - 120.0f * unit, origin.y + panel_h - close_h - pad * 0.5f);
  const ImVec2 close_b(close_a.x + 240.0f * unit, close_a.y + close_h);
  const auto inside = [](ImVec2 p, ImVec2 a, ImVec2 b) { return p.x >= a.x && p.x <= b.x && p.y >= a.y && p.y <= b.y; };
  const auto cell_origin = [&](int i) {
    return ImVec2(origin.x + pad + (i % cols) * (cell_w + pad), origin.y + title_h + pad + (i / cols) * (cell_h + pad));
  };

  // New fingers only: a finger already down when the panel opened is ignored.
  for (auto it = s.panel_fingers.begin(); it != s.panel_fingers.end();) {
    it = touches.count(*it) ? std::next(it) : s.panel_fingers.erase(it);
  }
  for (const auto& [key, position] : touches) {
    if (!s.panel_fingers.insert(key).second) continue;
    if (!inside(position, origin, ImVec2(origin.x + panel_w, origin.y + panel_h)) ||
        inside(position, close_a, close_b)) {
      s.cheat_panel = false;
      break;
    }
    for (int i = 0; i < count; ++i) {
      const ImVec2 a = cell_origin(i);
      if (inside(position, a, ImVec2(a.x + cell_w, a.y + cell_h))) {
        StartCheat(s, c.cheats[i].first, c.cheats[i].second);
        s.cheat_panel = false;
        break;
      }
    }
    if (!s.cheat_panel) break;
  }

  if (!d || !font) return;
  d->AddRectFilled(ImVec2(0, 0), ImVec2(width, height), IM_COL32(0, 0, 0, 120));
  d->AddRectFilled(origin, ImVec2(origin.x + panel_w, origin.y + panel_h), IM_COL32(18, 18, 22, 235), 14.0f * unit);
  d->AddRect(origin, ImVec2(origin.x + panel_w, origin.y + panel_h), IM_COL32(225, 225, 225, 170), 14.0f * unit, 0, 2.0f);
  DrawText(d, font, 24.0f * unit * 1.2f, ImVec2(origin.x + panel_w * 0.5f, origin.y + 12.0f * unit),
           "CHEATS - tap one", 1.0f);
  for (int i = 0; i < count; ++i) {
    const ImVec2 a = cell_origin(i), b(a.x + cell_w, a.y + cell_h);
    d->AddRectFilled(a, b, IM_COL32(60, 62, 70, 230), 8.0f * unit);
    d->AddRect(a, b, IM_COL32(225, 225, 225, 150), 8.0f * unit, 0, 1.5f);
    const bool instant = !c.cheats[i].second.empty() && c.cheats[i].second[0] == '@';
    const float size = std::max(11.0f, std::min(cell_h * 0.26f, 20.0f * unit * 1.2f));
    const float inner_w = cell_w - 16.0f * unit;
    if (instant) {
      DrawFittedText(d, font, size, ImVec2(a.x + cell_w * 0.5f, a.y + cell_h * 0.5f), inner_w, cell_h - 12.0f * unit,
                     c.cheats[i].first, 1.0f);
    } else {
      DrawFittedText(d, font, size, ImVec2(a.x + cell_w * 0.5f, a.y + cell_h * 0.38f), inner_w, cell_h * 0.5f,
                     c.cheats[i].first, 1.0f);
      DrawText(d, font, size * 0.8f, ImVec2(a.x + cell_w * 0.5f, a.y + cell_h * 0.62f), c.cheats[i].second.c_str(), 0.7f);
    }
  }
  d->AddRectFilled(close_a, close_b, IM_COL32(170, 60, 55, 230), 8.0f * unit);
  DrawText(d, font, 22.0f * unit * 1.2f, ImVec2((close_a.x + close_b.x) * 0.5f, close_a.y + close_h * 0.5f - 13.0f * unit), "CLOSE", 1.0f);
}

}  // namespace

bool LibertyTouchLayoutActive(std::string_view layout) noexcept { return layout == "liberty"; }

void PublishLibertyPauseMenu(uint32_t pause_flag, uint32_t screen) noexcept {
  g_pause_flag.store(pause_flag, std::memory_order_relaxed);
  g_menu_screen.store(screen, std::memory_order_relaxed);
}

void SetLibertyIconResolver(LibertyIconResolver resolver, void* context) noexcept {
  g_icon_resolver = resolver;
  g_icon_context = context;
}

bool UpdateAndDrawLibertyTouchOverlay(ImDrawList* draw, ImFont* font, float font_size, float width,
                                      float height) noexcept {
  (void)font_size;
  auto& s = state();
  std::lock_guard lock(s.mutex);
  if (!s.layout_loaded) LoadOffsets(s);
  // Leave the forced menu when the game is clearly back in play: a cutscene,
  // loading, or the title reporting the menu closed after it had reported it open.
  {
    const TouchContextSnapshot context = GetTouchContextSnapshot();
    if (s.forced_menu && (context.loading || context.cutscene)) s.forced_menu = false;
    // The pause menu stops gameplay (gameplay_allowed false). Once that has been
    // seen after the Pause tap, the menu has opened; when gameplay stays allowed
    // again for 0.4 s the player is back in the game, however the menu was
    // closed (Back, Resume, ...).
    const auto now_tick = std::chrono::steady_clock::now();
    if (s.forced_menu) {
      if (!context.gameplay_allowed) {
        s.forced_saw_pause = true;
        s.gameplay_since = {};
      } else if (s.forced_saw_pause) {
        if (s.gameplay_since.time_since_epoch().count() == 0) s.gameplay_since = now_tick;
        if (now_tick - s.gameplay_since > std::chrono::milliseconds(400)) s.forced_menu = false;
      }
    }
    if (!s.forced_menu) {
      s.forced_saw_pause = false;
      s.gameplay_since = {};
    }
  }
  Screen screen = rex::input::TouchControlsVisible() && width > 0 && height > 0
                      ? CurrentScreen(s.forced_menu, s.pause_flag_verified, &s.logged_context)
                      : Screen::kHidden;
  if (screen == Screen::kHidden && rex::input::TouchControlsVisible() && width > 0 && height > 0) {
    // Screens drawn before the game reports a player (the episode picker, for one) look like a
    // loading screen to the context, so the controls never appeared and the picker could not be
    // used. After a few seconds with nothing reported, show the menu controls (up, down, accept,
    // back) so the game can be navigated. They go away again once the game reports a real screen.
    const auto hidden_now = std::chrono::steady_clock::now();
    if (s.hidden_since.time_since_epoch().count() == 0) s.hidden_since = hidden_now;
    if (hidden_now - s.hidden_since > std::chrono::seconds(6)) screen = Screen::kMenu;
  } else {
    s.hidden_since = {};
  }
  if (screen != s.screen) {
    s.screen = screen;
    s.fingers.clear();
    s.stick = {ImVec2(0, 0), ImVec2(0, 0)};
    s.steer = 0.0f;
    s.look_active = false;
    s.look_pending = s.look_out = ImVec2(0, 0);
  }
  if (screen == Screen::kHidden) {
    s.visible = false;
    s.placed.clear();
    s.held.clear();
    return false;
  }
  s.visible = true;
  Place(s, width, height);
  const float unit = height / 737.0f;

  // Every finger on the screen.
  std::unordered_map<uint64_t, ImVec2> touches;
  int device_count = 0;
  if (SDL_TouchID* devices = SDL_GetTouchDevices(&device_count)) {
    for (int dev = 0; dev < device_count; ++dev) {
      int finger_count = 0;
      if (SDL_Finger** fingers = SDL_GetTouchFingers(devices[dev], &finger_count)) {
        for (int f = 0; f < finger_count; ++f) {
          const uint64_t key = (uint64_t(devices[dev]) << 32) ^ uint64_t(fingers[f]->id);
          touches[key] = ImVec2(fingers[f]->x * width, fingers[f]->y * height);
        }
        SDL_free(fingers);
      }
    }
    SDL_free(devices);
  }
  for (auto it = s.fingers.begin(); it != s.fingers.end();) {
    it = touches.count(it->first) ? std::next(it) : s.fingers.erase(it);
  }

  // Cheat list open: it takes all touches, and nothing else reaches the game.
  if (s.cheat_panel && !s.editing) {
    s.fingers.clear();
    s.stick = {ImVec2(0, 0), ImVec2(0, 0)};
    s.steer = 0.0f;
    s.look_active = false;
    s.look_pending = s.look_out = ImVec2(0, 0);
    HandleCheatPanel(s, touches, draw, font, width, height, unit);
    s.held.assign(s.placed.size(), false);
    s.cheats_was_held = false;
    return true;
  }

  // Edit mode: the CAMERA SPEED slider (0.3x to 3x, logarithmic) under the title.
  const float slider_x0 = width * 0.5f - 260.0f * unit, slider_x1 = width * 0.5f + 260.0f * unit;
  const float slider_y = 215.0f * unit;
  if (s.editing) {
    for (const auto& [key, position] : touches) {
      if (std::fabs(position.y - slider_y) <= 48.0f * unit && position.x >= slider_x0 - 30.0f * unit &&
          position.x <= slider_x1 + 30.0f * unit) {
        const float t = std::clamp((position.x - slider_x0) / (slider_x1 - slider_x0), 0.0f, 1.0f);
        s.swipe_sens = 0.3f * std::pow(10.0f, t);
      }
    }
  }

  bool stick_owned[2] = {false, false};
  bool layout_changed = false;
  const bool swipe_camera = REXCVAR_GET(gta4_touch_camera) != "stick";
  bool look_seen = false;
  s.steer = 0.0f;
  for (const auto& [key, position] : touches) {
    auto found = s.fingers.find(key);
    if (found == s.fingers.end()) {
      Finger finger;
      finger.control = HitTest(s, position);
      if (s.editing) {
        // A second finger joins the control the first finger holds, so two
        // fingers on (or around) a button resize it.
        int held_control = -1, holders = 0;
        for (const auto& [other_key, other] : s.fingers) {
          if (other.control >= 0) {
            held_control = other.control;
            ++holders;
          }
        }
        if (holders == 1 && held_control >= 0 &&
            s.placed[held_control].spec->kind != Kind::kReset &&
            std::string_view(s.placed[held_control].spec->id) != "pause" &&
            (finger.control < 0 || finger.control == held_control)) {
          finger.control = held_control;
        }
      }
      if (finger.control >= 0) {
        const auto& c = s.placed[finger.control];
        finger.grab_offset = ImVec2(position.x - c.center.x, position.y - c.center.y);
        if (s.editing && c.spec->kind == Kind::kReset) {
          for (auto it = s.offsets.begin(); it != s.offsets.end();) {
            it = it->first.rfind(std::string(ScreenName(s.screen)) + "/", 0) == 0 ? s.offsets.erase(it)
                                                                                 : std::next(it);
          }
          for (auto it = s.sizes.begin(); it != s.sizes.end();) {
            it = it->first.rfind(std::string(ScreenName(s.screen)) + "/", 0) == 0 ? s.sizes.erase(it)
                                                                                : std::next(it);
          }
          SaveOffsets(s);
          layout_changed = true;
        }
      }
      found = s.fingers.emplace(key, finger).first;
    }
    const int index = found->second.control;
    if (index < 0 && swipe_camera && !s.editing && (screen == Screen::kOnFoot || screen == Screen::kDriving)) {
      // A finger on empty screen: the first one (right of the move area) becomes the look finger.
      if (!s.look_active && position.x > width * 0.3f) {
        s.look_active = true;
        s.look_key = key;
        s.look_last = position;
        s.look_time = std::chrono::steady_clock::now();
      }
      if (s.look_active && s.look_key == key) {
        look_seen = true;
        // Finger travel is kept as a budget (seconds of full stick) and delivered below, so a
        // fast flick turns as far as the same distance dragged slowly. Full speed when the
        // finger crosses about 1.5 screen heights per second at sensitivity 1.
        const float full_speed = height * 1.5f / s.swipe_sens;
        // Fast flicks are compressed so they do not whip the camera round: the faster the finger,
        // the smaller the share of its travel that is kept (a slow drag keeps nearly all of it).
        const ImVec2 step(position.x - s.look_last.x, position.y - s.look_last.y);
        const float frame_dt = std::clamp(
            std::chrono::duration<float>(std::chrono::steady_clock::now() - s.look_time).count(), 1.0f / 240.0f, 0.1f);
        const float heights_per_second = std::hypot(step.x, step.y) / frame_dt / height;
        const float keep = 1.0f / (1.0f + 0.35f * heights_per_second);
        s.look_pending.x += step.x * keep / full_speed;
        s.look_pending.y += step.y * keep / full_speed;
        s.look_last = position;
      }
      continue;
    }
    if (index < 0 || index >= int(s.placed.size())) continue;
    const Placed& c = s.placed[index];
    const Spec& spec = *c.spec;

    if (s.editing && spec.kind != Kind::kReset && std::string_view(spec.id) != "pause") {
      int fingers_on_control = 0;
      for (const auto& [other_key, other] : s.fingers)
        if (other.control == index) ++fingers_on_control;
      if (fingers_on_control >= 2) continue;  // pinching: handled below
      // Drag to move: store the new centre as an offset in design units.
      const ImVec2 target(position.x - found->second.grab_offset.x, position.y - found->second.grab_offset.y);
      const float dx = (target.x - c.center.x) / unit, dy = (target.y - c.center.y) / unit;
      if (std::fabs(dx) > 0.01f || std::fabs(dy) > 0.01f) {
        ImVec2& offset = s.offsets[std::string(ScreenName(s.screen)) + "/" + spec.id];
        offset.x += spec.anchor == Anchor::kRight ? -dx : dx;
        offset.y += dy;
        layout_changed = true;
      }
      continue;
    }
    s.held[index] = true;
    if (spec.kind == Kind::kStick && !stick_owned[spec.button]) {
      stick_owned[spec.button] = true;
      const float range = c.r * 0.55f;
      ImVec2 v((position.x - c.center.x) / range, (position.y - c.center.y) / range);
      const float length = std::hypot(v.x, v.y);
      if (length > 1.0f) v = ImVec2(v.x / length, v.y / length);
      s.stick[spec.button] = v;
    } else if (spec.kind == Kind::kSteer) {
      s.steer += spec.button ? 1.0f : -1.0f;
    }
  }
  for (int i = 0; i < 2; ++i)
    if (!stick_owned[i]) s.stick[i] = ImVec2(0, 0);
  {
    // Deliver the finger's travel as stick deflection. The title ignores small stick values (its
    // own dead zone), so a slow drag used to turn nothing. Deflection is therefore either zero or at
    // least kMinAxis: small travel waits until it adds up to one minimum step. Travel that is more
    // than one frame can deliver at full stick carries on over the next frames, also after lift-off.
    const auto look_now = std::chrono::steady_clock::now();
    const float dt = std::clamp(std::chrono::duration<float>(look_now - s.look_time).count(), 1.0f / 240.0f, 0.1f);
    s.look_time = look_now;
    if (!look_seen) s.look_active = false;
    constexpr float kMinAxis = 0.30f;
    constexpr float kMaxBudget = 0.35f;  // at most about a third of a second of full stick is queued
    constexpr float kSmoothing = 0.06f;  // seconds; the output eases towards its target
    float* pending[2] = {&s.look_pending.x, &s.look_pending.y};
    float* out[2] = {&s.look_out.x, &s.look_out.y};
    for (int i = 0; i < 2; ++i) {
      float& p = *pending[i];
      p = std::clamp(p, -kMaxBudget, kMaxBudget);
      const float need = p / dt;
      float target = 0.0f;
      if (std::fabs(need) >= kMinAxis) {
        target = std::clamp(need, -1.0f, 1.0f);
      } else if (!look_seen && std::fabs(p) < kMinAxis * dt) {
        p = 0.0f;  // finger up: drop what is too small to deliver
      }
      // Ease towards the target so the camera turns smoothly instead of in steps, but never sit in
      // the title's dead zone: a non-zero output is at least kMinAxis.
      float value = *out[i] + (target - *out[i]) * std::min(1.0f, dt / kSmoothing);
      if (std::fabs(value) < kMinAxis) value = target == 0.0f ? 0.0f : (target > 0.0f ? kMinAxis : -kMinAxis);
      *out[i] = value;
      p -= value * dt;
    }
  }
  // Pinch to resize (edit mode): the scale follows the distance between the
  // two fingers on a control, relative to when the second finger landed.
  if (s.editing) {
    int pinch_control = -1;
    ImVec2 points[2];
    int count = 0;
    for (const auto& [key, finger] : s.fingers) {
      if (finger.control < 0 || finger.control >= int(s.placed.size())) continue;
      if (pinch_control >= 0 && finger.control != pinch_control) continue;
      int same = 0;
      for (const auto& [k2, f2] : s.fingers) if (f2.control == finger.control) ++same;
      if (same < 2) continue;
      pinch_control = finger.control;
      if (count < 2 && touches.count(key)) points[count++] = touches[key];
    }
    if (pinch_control >= 0 && count == 2) {
      const float distance = std::hypot(points[0].x - points[1].x, points[0].y - points[1].y);
      const std::string key = std::string(ScreenName(s.screen)) + "/" + s.placed[pinch_control].spec->id;
      if (s.pinch.control != pinch_control || s.pinch.start_distance <= 1.0f) {
        s.pinch.control = pinch_control;
        s.pinch.start_distance = std::max(distance, 1.0f);
        const auto it = s.sizes.find(key);
        s.pinch.start_scale = it != s.sizes.end() ? it->second : 1.0f;
      } else {
        const float scale = std::clamp(s.pinch.start_scale * distance / s.pinch.start_distance, 0.5f, 2.0f);
        float& current = s.sizes[key];
        if (current == 0.0f) current = 1.0f;
        if (std::fabs(scale - current) > 0.005f) {
          current = scale;
          layout_changed = true;
        }
      }
    } else {
      s.pinch = {};
    }
  }
  if (layout_changed) Place(s, width, height);

  // Hold Pause 1.5 s: toggle edit mode. In edit mode a tap on Pause saves.
  bool pause_held = false;
  for (size_t i = 0; i < s.placed.size(); ++i)
    if (std::string_view(s.placed[i].spec->id) == "pause" && s.held[i]) pause_held = true;
  if (s.editing) {
    for (const auto& [key, finger] : s.fingers) {
      if (finger.control >= 0 && finger.control < int(s.placed.size()) &&
          std::string_view(s.placed[finger.control].spec->id) == "pause") {
        pause_held = true;
      }
    }
  }
  const auto now = std::chrono::steady_clock::now();
  if (pause_held && !s.pause_was_held) {
    s.pause_down = now;
    s.pause_long_handled = false;
  }
  if (pause_held && !s.pause_long_handled && !s.editing && now - s.pause_down > std::chrono::milliseconds(1500)) {
    s.editing = true;
    s.pause_long_handled = true;
    s.fingers.clear();
  } else if (!pause_held && s.pause_was_held && !s.pause_long_handled) {
    if (s.editing) {
      s.editing = false;  // tap Pause in edit mode: save and leave
      SaveOffsets(s);
      SaveSwipe(s);
    } else {
      s.start_pulse_until = now + std::chrono::milliseconds(120);  // short tap: pause the game
      s.forced_menu = true;

    }
  }
  s.pause_was_held = pause_held;
  bool skip_held = false;
  for (size_t i = 0; i < s.placed.size(); ++i)
    if (std::string_view(s.placed[i].spec->id) == "skip" && s.held[i]) skip_held = true;
  if (skip_held && !s.skip_was_held && !s.editing) s.skip_pulse_until = now + std::chrono::milliseconds(150);
  s.skip_was_held = skip_held;
  bool cheats_held = false;
  for (size_t i = 0; i < s.placed.size(); ++i)
    if (std::string_view(s.placed[i].spec->id) == "cheats" && s.held[i]) cheats_held = true;
  if (cheats_held && !s.cheats_was_held && !s.editing) {
    if (s.macro.active) {
      REXLOG_WARN("liberty-cheat: '{}' cancelled by the CH button", s.macro.name);
      s.macro = {};
    } else {
      s.cheat_panel = true;
      s.panel_fingers.clear();
      for (const auto& [key, position] : touches) s.panel_fingers.insert(key);
    }
  }
  s.cheats_was_held = cheats_held;
  // RESUME (menu screen): Start closes the pause menu; leave the forced menu.
  bool resume_held = false;
  for (size_t i = 0; i < s.placed.size(); ++i)
    if (std::string_view(s.placed[i].spec->id) == "resume" && s.held[i]) resume_held = true;
  if (!resume_held && s.resume_was_held) s.forced_menu = false;
  s.resume_was_held = resume_held;

  // Switch button: change to the classic Xbox 360 gamepad for this session.
  bool switch_held = false;
  for (size_t i = 0; i < s.placed.size(); ++i)
    if (std::string_view(s.placed[i].spec->id) == "switch" && s.held[i]) switch_held = true;
  if (!switch_held && s.switch_was_held && !s.editing) {
    s.switch_was_held = false;
    s.visible = false;
    s.fingers.clear();
    rex::cvar::SetFlagByName("gta4_touch_layout", "gamepad");
    return false;
  }
  s.switch_was_held = switch_held;
  if (s.editing) {  // Pause never reaches the game while editing
    for (size_t i = 0; i < s.placed.size(); ++i)
      if (std::string_view(s.placed[i].spec->id) == "pause") s.held[i] = false;
  }

  if (draw && font) {
    for (size_t i = 0; i < s.placed.size(); ++i) DrawControl(draw, font, s, i, unit);
    if (!s.editing) DrawAmmo(draw, font, s, unit);
    if (s.editing) {
      DrawText(draw, font, 20.0f * unit * 1.2f, ImVec2(width * 0.5f, 120.0f * unit),
               "EDIT LAYOUT - drag to move, pinch to resize, tap PAUSE to save", 1.0f);
      // Camera speed slider.
      char label[48];
      std::snprintf(label, sizeof(label), "CAMERA SPEED  %.2fx", s.swipe_sens);
      DrawText(draw, font, 19.0f * unit * 1.2f, ImVec2(width * 0.5f, 158.0f * unit), label, 1.0f);
      const float t = std::clamp(std::log10(s.swipe_sens / 0.3f), 0.0f, 1.0f);
      const float knob_x = slider_x0 + t * (slider_x1 - slider_x0);
      draw->AddRectFilled(ImVec2(slider_x0, slider_y - 5.0f * unit), ImVec2(slider_x1, slider_y + 5.0f * unit),
                          IM_COL32(20, 20, 24, 200), 5.0f * unit);
      draw->AddRectFilled(ImVec2(slider_x0, slider_y - 5.0f * unit), ImVec2(knob_x, slider_y + 5.0f * unit),
                          IM_COL32(246, 207, 92, 230), 5.0f * unit);
      draw->AddCircleFilled(ImVec2(knob_x, slider_y), 17.0f * unit, IM_COL32(246, 207, 92, 255), 24);
      draw->AddCircle(ImVec2(knob_x, slider_y), 17.0f * unit, IM_COL32(20, 20, 24, 255), 24, 2.5f);
    }
  }
  return true;
}

bool LibertyTouchOverlayVisible() noexcept {
  std::lock_guard lock(state().mutex);
  return state().visible;
}

bool ReadLibertyTouchOverlay(uint32_t user, rex::input::X_INPUT_GAMEPAD* output) noexcept {
  if (!output || user != 0) return false;
  auto& s = state();
  std::lock_guard lock(s.mutex);
  if (!s.visible) return false;
  *output = {};
  if (s.editing) return true;  // nothing reaches the game while editing
  uint16_t buttons = 0;
  uint8_t triggers[2] = {0, 0};
  for (size_t i = 0; i < s.placed.size() && i < s.held.size(); ++i) {
    if (!s.held[i]) continue;
    const Spec& spec = *s.placed[i].spec;
    if (std::string_view(spec.id) == "pause") continue;  // sent on release, below
    if (spec.kind == Kind::kButton) buttons |= spec.button;
    if (spec.kind == Kind::kTrigger) triggers[spec.button & 1] = 255;
  }
  constexpr float kDeadZone = 0.12f;
  const auto axis = [&](ImVec2 v, int16_t* x_out, int16_t* y_out) {
    const float length = std::hypot(v.x, v.y);
    if (length < kDeadZone) return;
    const float k = std::min((length - kDeadZone) / (1.0f - kDeadZone), 1.0f) / length;
    *x_out = int16_t(std::clamp(v.x * k, -1.0f, 1.0f) * 32767.0f);
    *y_out = int16_t(std::clamp(-v.y * k, -1.0f, 1.0f) * 32767.0f);
  };
  // The pad fields are big-endian (rex::be); fill locals, then assign.
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  axis(s.stick[0], &lx, &ly);
  axis(s.stick[1], &rx, &ry);
  if (s.look_out.x != 0.0f || s.look_out.y != 0.0f) {  // swipe camera: the title applies its own dead zone
    rx = int16_t(std::clamp(s.look_out.x, -1.0f, 1.0f) * 32767.0f);
    ry = int16_t(std::clamp(-s.look_out.y, -1.0f, 1.0f) * 32767.0f);
  }
  if (s.steer != 0.0f) lx = int16_t(std::clamp(s.steer, -1.0f, 1.0f) * 32767.0f);
  const auto now = std::chrono::steady_clock::now();
  if (now < s.start_pulse_until) buttons |= X_INPUT_GAMEPAD_START;
  if (now < s.skip_pulse_until) buttons |= X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_B | X_INPUT_GAMEPAD_X;
  AdvanceMacro(s, now, &buttons);
  output->thumb_lx = lx;
  output->thumb_ly = ly;
  output->thumb_rx = rx;
  output->thumb_ry = ry;
  output->buttons = buttons;
  output->left_trigger = triggers[0];
  output->right_trigger = triggers[1];
  return true;
}

}  // namespace gta4::input
