#pragma once

namespace gta4::android {

// Called once per game frame the title submits. In a "Play (Debug Log)" session the first call starts
// a watchdog thread: when no frame has been submitted for four seconds it writes every thread's
// state and call stack (module + offset) into the debug log, once, then every 30 seconds while the
// game stays stuck. Costs one wake-up a second and nothing outside debug-log sessions.
void NoteFrameForStallWatchdog();

}  // namespace gta4::android
