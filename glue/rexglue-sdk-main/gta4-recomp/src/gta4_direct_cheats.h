#pragma once
// Cheats applied directly by calling the game's own script natives, without the
// phone. The native table is learned from the registration hook; the cheats run
// inside a frequently called native so they execute in a real script thread
// (the same approach as the quicksave feature's native calls).
#include <cstdint>
#include <string>

struct PPCContext;

namespace gta4::directcheats {

// Called from the single native registrar (sub_82845600) for every native. May
// replace ctx.r4 with a wrapper for a few frequently called natives.
void ObserveNativeRegistration(PPCContext& ctx, uint8_t* base);

// Queues a cheat by id (health_armor, wanted_clear, wanted_raise, weather_next).
// Safe from any thread; it runs on the next script native call.
bool Request(const std::string& action);

// True once when a cheat run through the game's cheat table found no match and listed a phone number:
// *number is that number, for the caller to dial.
bool TakeDialFallback(std::string* number);

}  // namespace gta4::directcheats
