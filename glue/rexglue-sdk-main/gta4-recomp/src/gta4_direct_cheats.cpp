#include "gta4_direct_cheats.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>

#include "gta4_init.h"

namespace gta4::directcheats {
namespace {

std::mutex g_mutex;
std::string g_dial_fallback;  // a phone number the overlay should dial, set when a cheat has no table match
std::map<std::string, uint32_t> g_natives;  // native name -> original handler address
std::vector<std::string> g_pending;
std::atomic<bool> g_has_pending{false};
std::atomic<uint64_t> g_last_registration_ms{0};
std::atomic<bool> g_dumped{false};
uint32_t g_weather = 0;

uint64_t NowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

bool Span(uint8_t* base, uint32_t address, size_t size, bool write = false) {
  if (!base || !address || !size || uint64_t(address) + size > uint64_t(UINT32_MAX) + 1) return false;
  auto* kernel = REX_KERNEL_STATE();
  auto* memory = kernel ? kernel->memory() : nullptr;
  auto* heap = memory ? memory->LookupHeap(address) : nullptr;
  const uint32_t last = uint32_t(uint64_t(address) + size - 1);
  if (!heap || heap != memory->LookupHeap(last)) return false;
  const auto access = heap->QueryRangeAccess(address, last);
  using rex::memory::PageAccess;
  return access == PageAccess::kReadWrite || access == PageAccess::kExecuteReadWrite ||
         (!write && (access == PageAccess::kReadOnly || access == PageAccess::kExecuteReadOnly));
}

uint32_t Read(uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, rex::memory::GuestPtr(base, address), sizeof(v));
  return __builtin_bswap32(v);
}

void Write(uint8_t* base, uint32_t address, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(rex::memory::GuestPtr(base, address), &v, sizeof(v));
}

std::string NativeName(uint8_t* base, uint32_t address) {
  std::string out;
  for (size_t i = 0; i < 80; ++i) {
    if (uint64_t(address) + i > UINT32_MAX || !Span(base, address + uint32_t(i), 1)) return {};
    const char c = char(*rex::memory::GuestPtr(base, address + uint32_t(i)));
    if (!c) return out;
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return {};
    out.push_back(c);
  }
  return {};
}

::PPCFunc* GuestFunction(uint32_t address) {
  auto* runtime = rex::Runtime::instance();
  auto* dispatcher = runtime ? runtime->function_dispatcher() : nullptr;
  return dispatcher && address ? dispatcher->GetFunction(address) : nullptr;
}

constexpr uint32_t kSlot = 0xFFFF0000u;  // kSlot + k as an argument = address of scratch slot k (k < 8)
constexpr uint32_t kText = 0xFFFE0000u;  // as an argument = address of the scratch string

uint32_t F(float value) { return std::bit_cast<uint32_t>(value); }

// Calls a script native with the game's native context layout in a separate
// stack frame: [0] return storage, [4] argument count, [8] argument array. An
// output argument gets the address of the return storage and its value is
// returned (for natives that write a result through a pointer). Scratch slots
// give natives with several output pointers (or in/out handles) somewhere to
// write: pass kSlot + k and read the values back from `slots`.
std::optional<uint32_t> Call(PPCContext& parent, uint8_t* base, std::string_view name,
                             std::initializer_list<uint32_t> values = {}, int output_argument = -1,
                             std::array<uint32_t, 8>* slots = nullptr, const char* text = nullptr) {
  uint32_t address = 0;
  {
    std::lock_guard lock(g_mutex);
    const auto it = g_natives.find(std::string(name));
    if (it != g_natives.end()) address = it->second;
  }
  auto* function = GuestFunction(address);
  if (!function || values.size() > 8 || parent.r1.u32 < 0x200) {
    REXLOG_WARN("liberty-direct: native {} is not available (address {:08X})", name, address);
    return std::nullopt;
  }
  const uint32_t frame = (parent.r1.u32 - 0x200u) & ~15u;
  if (!Span(base, frame, 0x200, true)) return std::nullopt;
  const uint32_t context = frame + 0x80, result = frame + 0xC0, arguments = frame + 0xD0;
  const uint32_t scratch = frame + 0x100, text_address = frame + 0x120;
  Write(base, frame, parent.r1.u32);
  Write(base, context, result);
  Write(base, context + 4, uint32_t(values.size()));
  Write(base, context + 8, arguments);
  Write(base, context + 12, 0);
  Write(base, result, 0);
  if (slots) {
    for (uint32_t k = 0; k < 8; ++k) Write(base, scratch + k * 4, (*slots)[k]);
  }
  if (text) {
    const size_t length = std::min<size_t>(std::strlen(text), 63);
    for (size_t i = 0; i < length; ++i) *rex::memory::GuestPtr(base, text_address + uint32_t(i)) = uint8_t(text[i]);
    *rex::memory::GuestPtr(base, text_address + uint32_t(length)) = 0;
  }
  size_t i = 0;
  for (const auto value : values) {
    uint32_t argument = value;
    if (int(i) == output_argument) argument = result;
    else if (value >= kSlot && value < kSlot + 8) argument = scratch + (value - kSlot) * 4;
    else if (value == kText) argument = text_address;
    Write(base, arguments + uint32_t(i) * 4, argument);
    ++i;
  }
  PPCContext call = parent;
  call.r1.u32 = frame;
  call.r3.u32 = context;
  call.lr = 0x82844BA0;
  function(call, base);
  if (slots) {
    for (uint32_t k = 0; k < 8; ++k) (*slots)[k] = Read(base, scratch + k * 4);
  }
  const uint32_t value = Read(base, result);
  REXLOG_WARN("liberty-direct: {}({}) -> {}", name, values.size(), int32_t(value));
  return value;
}

void DumpTable() {
  const auto path = rex::filesystem::GetUserFolder() / "liberty_natives.txt";
  std::ofstream out(path);
  std::lock_guard lock(g_mutex);
  for (const auto& [name, handler] : g_natives) {
    char line[128];
    std::snprintf(line, sizeof(line), "%s %08X\n", name.c_str(), handler);
    out << line;
  }
  REXLOG_WARN("liberty-direct: {} natives written to {}", g_natives.size(), path.string());
}

// Weapon ids of the game's eWeaponType and the ammo given for each.
struct WeaponGrant {
  uint32_t weapon;
  uint32_t ammo;
};
constexpr std::array<WeaponGrant, 7> kWeaponsOne = {{{1, 1}, {4, 25}, {7, 250}, {10, 250}, {12, 250}, {14, 250}, {16, 250}}};
constexpr std::array<WeaponGrant, 7> kWeaponsTwo = {{{5, 25}, {9, 250}, {11, 250}, {13, 250}, {15, 250}, {17, 250}, {18, 10}}};

uint32_t Joaat(const std::string& text) {
  uint32_t hash = 0;
  for (char c : text) {
    if (c >= 'A' && c <= 'Z') c = char(c + 32);
    hash += uint8_t(c);
    hash += hash << 10;
    hash ^= hash >> 6;
  }
  hash += hash << 3;
  hash ^= hash >> 11;
  hash += hash << 15;
  return hash;
}

// The game's own hash of a model name; the same function is the fallback.
uint32_t ModelHash(PPCContext& parent, uint8_t* base, const std::string& model) {
  const auto hash = Call(parent, base, "GET_HASH_KEY", {kText}, -1, nullptr, model.c_str());
  const uint32_t ours = Joaat(model);
  if (hash && *hash != ours) {
    REXLOG_WARN("liberty-direct: hash of '{}': game {:08X}, ours {:08X}", model, *hash, ours);
  }
  return hash && *hash ? *hash : ours;
}

// Which episode a model belongs to, 0 when it is part of the base game (or shared). A model of an
// episode that is not running can be present in the model table without the rest of its data
// (handling, assets): spawning one was followed by a frozen game, so those are refused.
int EpisodeOfModel(const std::string& name) {
  static const char* const kLostAndDamned[] = {"innovation", "hexer", "slamvan", "gburrito", "gangburrito", "hakuchou2"};
  static const char* const kGayTony[] = {"floater", "buzzard", "akuma", "vader"};
  for (const char* model : kLostAndDamned) {
    if (name == model) return 1;
  }
  for (const char* model : kGayTony) {
    if (name == model) return 2;
  }
  return 0;
}

bool IsBoat(const std::string& name) {
  static const char* const kBoats[] = {"jetmax", "floater", "squalo", "marquis", "dinghy", "predator",
                                       "reefer", "tuga", "tug", "tropic", "launch"};
  for (const char* boat : kBoats) {
    if (name == boat) return true;
  }
  return false;
}

// Water to put a boat on: the first point in front of the player (then to the sides, then farther)
// where the game finds water. A boat created on dry land hangs in the air.
bool FindWaterAhead(PPCContext& parent, uint8_t* base, uint32_t ped, uint32_t position[3]) {
  static const float kForward[] = {6.0f, 12.0f, 20.0f, 30.0f, 45.0f, 65.0f, 90.0f};
  static const float kSide[] = {0.0f, -0.6f, 0.6f};
  for (const float forward : kForward) {
    for (const float side : kSide) {
      std::array<uint32_t, 8> slots{};
      Call(parent, base, "GET_OFFSET_FROM_CHAR_IN_WORLD_COORDS",
           {ped, F(side * forward), F(forward), F(0.0f), kSlot, kSlot + 1, kSlot + 2}, -1, &slots);
      const uint32_t x = slots[0], y = slots[1], z = slots[2];
      std::array<uint32_t, 8> water{};
      const auto found = Call(parent, base, "GET_WATER_HEIGHT", {x, y, z, kSlot + 3}, -1, &water);
      if (!found || !*found) continue;
      const float height = std::bit_cast<float>(water[3]);
      if (!(height > -200.0f && height < 500.0f)) continue;
      position[0] = x;
      position[1] = y;
      position[2] = F(height + 0.5f);
      REXLOG_WARN("liberty-direct: water {} m ahead ({} to the side), surface {:.1f}", forward, side, height);
      return true;
    }
  }
  return false;
}

// The game's cheat table, as sub_82247D78 (what ACTIVATE_CHEAT runs) reads it: the action function
// of cheat `id` at 0x82A95BC8 + 4 * id and its name at 0x82A95C48 + 4 * id.
constexpr uint32_t kCheatFunctions = 0x82A95BC8;
constexpr uint32_t kCheatNames = 0x82A95C48;
constexpr uint32_t kCheatCount = 32;

// Reads a guest string: plain bytes, or UTF-16 big-endian (the console's wide strings have a zero
// first byte, which a byte reader takes for an empty string).
std::string GuestText(uint8_t* base, uint32_t address) {
  std::string out;
  if (!Span(base, address, 2)) return out;
  const bool wide = *rex::memory::GuestPtr(base, address) == 0 && *rex::memory::GuestPtr(base, address + 1) != 0;
  const uint32_t step = wide ? 2 : 1;
  for (uint32_t i = 0; i < 80; ++i) {
    const uint32_t at = address + i * step + (wide ? 1 : 0);
    if (!Span(base, at, 1)) return out;
    const char c = char(*rex::memory::GuestPtr(base, at));
    if (!c) return out;
    out.push_back(c >= 32 && c < 127 ? c : '?');
  }
  return out;
}

std::vector<std::string> ReadCheatNames(uint8_t* base) {
  std::vector<std::string> names;
  for (uint32_t id = 0; id < kCheatCount; ++id) {
    if (!Span(base, kCheatNames + id * 4, 4)) {
      names.emplace_back();
      continue;
    }
    names.push_back(GuestText(base, Read(base, kCheatNames + id * 4)));
  }
  return names;
}

void DumpCheatTable(uint8_t* base) {
  const auto names = ReadCheatNames(base);
  const auto path = rex::filesystem::GetUserFolder() / "liberty_cheat_table.txt";
  std::ofstream out(path);
  for (uint32_t id = 0; id < names.size(); ++id) {
    const uint32_t function = Span(base, kCheatFunctions + id * 4, 4) ? Read(base, kCheatFunctions + id * 4) : 0;
    const uint32_t name_pointer = Span(base, kCheatNames + id * 4, 4) ? Read(base, kCheatNames + id * 4) : 0;
    char raw[40] = "";
    if (name_pointer && Span(base, name_pointer, 16)) {
      for (uint32_t i = 0; i < 16; ++i)
        std::snprintf(raw + i * 2, sizeof(raw) - i * 2, "%02X", *rex::memory::GuestPtr(base, name_pointer + i));
    }
    char line[240];
    std::snprintf(line, sizeof(line), "%2u function=%08X name-at=%08X bytes=%s name='%s'", id, function,
                  name_pointer, raw, names[id].c_str());
    REXLOG_WARN("liberty-direct: cheat table {}", line);
    out << line << "\n";
  }
}

std::string Normalize(const std::string& text) {
  std::string out;
  for (char c : text) {
    if (c >= 'A' && c <= 'Z') c = char(c + 32);
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out.push_back(c);
  }
  return out;
}

// Runs one of the game's own cheats through ACTIVATE_CHEAT. `spec` is a cheat number or words that
// appear in its name; several alternatives are separated by '|' (the first that matches is used).
void ActivateCheat(PPCContext& parent, uint8_t* base, const std::string& spec) {
  const auto names = ReadCheatNames(base);
  for (size_t start = 0; start <= spec.size();) {
    const size_t end = spec.find('|', start);
    const std::string token = spec.substr(start, end == std::string::npos ? end : end - start);
    start = end == std::string::npos ? spec.size() + 1 : end + 1;
    if (token.empty()) continue;
    int id = -1;
    if (token.find_first_not_of("0123456789") == std::string::npos) {
      id = std::stoi(token);
    } else {
      const std::string wanted = Normalize(token);
      for (uint32_t i = 0; i < names.size(); ++i) {
        if (!wanted.empty() && Normalize(names[i]).find(wanted) != std::string::npos) {
          id = int(i);
          break;
        }
      }
    }
    if (id < 0 || id >= int(kCheatCount)) continue;
    REXLOG_WARN("liberty-direct: cheat {} '{}' for '{}'", id, names[size_t(id)], token);
    Call(parent, base, "ACTIVATE_CHEAT", {uint32_t(id)});
    return;
  }
  REXLOG_WARN("liberty-direct: no cheat matches '{}'; see liberty_cheat_table.txt", spec);
  // A phone number in the list is the fallback: hand it to the overlay, which dials it.
  for (size_t start = 0; start <= spec.size();) {
    const size_t end = spec.find('|', start);
    const std::string token = spec.substr(start, end == std::string::npos ? end : end - start);
    start = end == std::string::npos ? spec.size() + 1 : end + 1;
    if (Normalize(token).size() >= 10 && token.find_first_not_of("0123456789-*# ") == std::string::npos) {
      std::lock_guard lock(g_mutex);
      g_dial_fallback = token;
      return;
    }
  }
}

// Spawns a vehicle a few metres in front of the player. `models` lists candidate
// model names separated by '|'; the first one the game knows is used.
void SpawnVehicle(PPCContext& parent, uint8_t* base, uint32_t ped, const std::string& models) {
  uint32_t hash = 0;
  std::string used;
  for (size_t start = 0; start <= models.size();) {
    const size_t end = models.find('|', start);
    const std::string name = models.substr(start, end == std::string::npos ? end : end - start);
    start = end == std::string::npos ? models.size() + 1 : end + 1;
    if (name.empty()) continue;
    if (const int needed = EpisodeOfModel(name); needed != 0) {
      const auto episode = Call(parent, base, "GET_CURRENT_EPISODE");
      REXLOG_WARN("liberty-direct: '{}' needs episode {}, current episode {}", name, needed,
                  episode ? int(*episode) : -1);
      if (!episode || int(*episode) != needed) continue;
    }
    const uint32_t candidate = ModelHash(parent, base, name);
    const auto known = Call(parent, base, "IS_MODEL_IN_CDIMAGE", {candidate});
    if (known && !*known) {
      REXLOG_WARN("liberty-direct: model '{}' is not in the game", name);
      continue;
    }
    hash = candidate;
    used = name;
    break;
  }
  if (!hash) {
    REXLOG_WARN("liberty-direct: no valid model in '{}'", models);
    return;
  }
  Call(parent, base, "REQUEST_MODEL", {hash});
  Call(parent, base, "LOAD_ALL_OBJECTS_NOW");
  const auto loaded = Call(parent, base, "HAS_MODEL_LOADED", {hash});
  if (!loaded || !*loaded) {
    REXLOG_WARN("liberty-direct: model '{}' did not load", used);
    Call(parent, base, "MARK_MODEL_AS_NO_LONGER_NEEDED", {hash});
    return;
  }
  std::array<uint32_t, 8> slots{};
  const bool boat = IsBoat(used);
  uint32_t water[3] = {};
  if (boat) {
    if (!FindWaterAhead(parent, base, ped, water)) {
      REXLOG_WARN("liberty-direct: no water within 90 m ahead: '{}' not spawned", used);
      Call(parent, base, "MARK_MODEL_AS_NO_LONGER_NEEDED", {hash});
      return;
    }
  } else {
    Call(parent, base, "GET_OFFSET_FROM_CHAR_IN_WORLD_COORDS",
         {ped, F(0.0f), F(6.0f), F(0.5f), kSlot, kSlot + 1, kSlot + 2}, -1, &slots);
  }
  Call(parent, base, "GET_CHAR_HEADING", {ped, kSlot + 3}, -1, &slots);
  if (boat) {
    slots[0] = water[0];
    slots[1] = water[1];
    slots[2] = water[2];
  }
  slots[4] = 0;
  Call(parent, base, "CREATE_CAR", {hash, slots[0], slots[1], slots[2], kSlot + 4, 1}, -1, &slots);
  const uint32_t car = slots[4];
  if (!car) {
    REXLOG_WARN("liberty-direct: '{}' was not created", used);
    Call(parent, base, "MARK_MODEL_AS_NO_LONGER_NEEDED", {hash});
    return;
  }
  Call(parent, base, "SET_CAR_HEADING", {car, slots[3]});
  if (!boat) Call(parent, base, "SET_CAR_ON_GROUND_PROPERLY", {car});
  Call(parent, base, "MARK_MODEL_AS_NO_LONGER_NEEDED", {hash});
  Call(parent, base, "MARK_CAR_AS_NO_LONGER_NEEDED", {kSlot + 4}, -1, &slots);
  REXLOG_WARN("liberty-direct: spawned '{}' as vehicle {}", used, car);
}

void GiveWeapons(PPCContext& parent, uint8_t* base, uint32_t ped, const std::array<WeaponGrant, 7>& list) {
  for (const auto& grant : list) Call(parent, base, "GIVE_WEAPON_TO_CHAR", {ped, grant.weapon, grant.ammo, 0});
}

void Execute(PPCContext& parent, uint8_t* base, const std::string& action) {
  REXLOG_WARN("liberty-direct: running '{}'", action);
  const auto player = Call(parent, base, "GET_PLAYER_ID");
  if (!player) return;
  const auto ped = Call(parent, base, "GET_PLAYER_CHAR", {*player, 0}, 1);
  if (!ped || !*ped) {
    REXLOG_WARN("liberty-direct: no player character yet");
    return;
  }
  if (action == "health_armor" || action == "health_armor_ammo") {
    Call(parent, base, "SET_CHAR_HEALTH", {*ped, 200});
    Call(parent, base, "ADD_ARMOUR_TO_CHAR", {*ped, 100});
    if (action == "health_armor_ammo") {
      const auto weapon = Call(parent, base, "GET_CURRENT_CHAR_WEAPON", {*ped, 0}, 1);
      if (weapon && *weapon) Call(parent, base, "ADD_AMMO_TO_CHAR", {*ped, *weapon, 250});
    }
  } else if (action == "weapons_1") {
    GiveWeapons(parent, base, *ped, kWeaponsOne);
  } else if (action == "weapons_2") {
    GiveWeapons(parent, base, *ped, kWeaponsTwo);
  } else if (action == "wanted_clear") {
    if (!Call(parent, base, "CLEAR_WANTED_LEVEL", {*player})) {
      Call(parent, base, "ALTER_WANTED_LEVEL", {*player, 0});
      Call(parent, base, "APPLY_WANTED_LEVEL_CHANGE_NOW", {*player});
    }
  } else if (action == "wanted_raise") {
    const auto level = Call(parent, base, "STORE_WANTED_LEVEL", {*player, 0}, 1);
    const uint32_t next = std::min<uint32_t>((level ? *level : 0) + 1, 6);
    Call(parent, base, "ALTER_WANTED_LEVEL", {*player, next});
    Call(parent, base, "APPLY_WANTED_LEVEL_CHANGE_NOW", {*player});
  } else if (action == "weather_next") {
    Call(parent, base, "FORCE_WEATHER_NOW", {g_weather % 8});
    ++g_weather;
  } else if (action.rfind("car:", 0) == 0) {
    SpawnVehicle(parent, base, *ped, action.substr(4));
  } else if (action.rfind("cheat:", 0) == 0) {
    ActivateCheat(parent, base, action.substr(6));
  } else {
    REXLOG_WARN("liberty-direct: unknown action '{}'", action);
  }
}

void RunPending(PPCContext& parent, uint8_t* base) {
  if (!g_dumped.load(std::memory_order_relaxed) && g_last_registration_ms.load() &&
      NowMs() - g_last_registration_ms.load() > 4000 && !g_dumped.exchange(true)) {
    DumpTable();
    DumpCheatTable(base);
  }
  if (!g_has_pending.load(std::memory_order_acquire)) return;
  thread_local bool busy = false;
  if (busy) return;
  busy = true;
  std::vector<std::string> actions;
  {
    std::lock_guard lock(g_mutex);
    actions.swap(g_pending);
    g_has_pending.store(false, std::memory_order_release);
  }
  for (const auto& action : actions) Execute(parent, base, action);
  busy = false;
}

struct Gateway {
  const char* name;
  uint32_t original;
  uint32_t thunk;
  ::PPCFunc* replacement;
};

template <size_t N>
void GatewayNative(PPCContext& ctx, uint8_t* base);

std::array<Gateway, 3> g_gateways = {{
    {"GET_PLAYER_ID", 0, 0, GatewayNative<0>},
    {"IS_PLAYER_PLAYING", 0, 0, GatewayNative<1>},
    {"GET_PLAYER_CHAR", 0, 0, GatewayNative<2>},
}};

// Runs the real native first, then any queued cheat, inside the calling script.
template <size_t N>
void GatewayNative(PPCContext& ctx, uint8_t* base) {
  if (auto* original = GuestFunction(g_gateways[N].original)) original(ctx, base);
  RunPending(ctx, base);
}

}  // namespace

void ObserveNativeRegistration(PPCContext& ctx, uint8_t* base) {
  const std::string name = NativeName(base, ctx.r3.u32);
  if (name.empty()) return;
  {
    std::lock_guard lock(g_mutex);
    g_natives[name] = ctx.r4.u32;
  }
  g_last_registration_ms.store(NowMs());
  for (auto& gateway : g_gateways) {
    if (name != gateway.name) continue;
    if (!gateway.thunk) {
      auto* runtime = rex::Runtime::instance();
      auto* dispatcher = runtime ? runtime->function_dispatcher() : nullptr;
      gateway.thunk = dispatcher ? dispatcher->AllocateThunk(gateway.replacement, uint32_t(ctx.lr)) : 0;
      REXLOG_WARN("liberty-direct: gateway {} thunk {:08X}", gateway.name, gateway.thunk);
    }
    if (gateway.thunk) {
      gateway.original = ctx.r4.u32;
      ctx.r4.u32 = gateway.thunk;
    }
  }
}

bool TakeDialFallback(std::string* number) {
  std::lock_guard lock(g_mutex);
  if (g_dial_fallback.empty()) return false;
  *number = std::move(g_dial_fallback);
  g_dial_fallback.clear();
  return true;
}

bool Request(const std::string& action) {
  size_t gateways = 0, natives = 0;
  {
    std::lock_guard lock(g_mutex);
    g_pending.push_back(action);
    g_has_pending.store(true, std::memory_order_release);
    natives = g_natives.size();
  }
  for (const auto& gateway : g_gateways) gateways += gateway.thunk ? 1 : 0;
  REXLOG_WARN("liberty-direct: queued '{}' ({} natives known, {} gateways)", action, natives, gateways);
  return gateways > 0;
}

}  // namespace gta4::directcheats
