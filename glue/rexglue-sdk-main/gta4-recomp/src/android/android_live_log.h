#pragma once

// Appends a line to the debug-log file named by LIBERTY_LIVE_LOG (set by the
// app for "Play (Debug Log)"). It does not depend on logcat, which on some
// phones stops delivering the game's lines mid-session.
#include <stdio.h>
#include <cstdio>
#include <stdlib.h>
#include <time.h>

namespace gta4::android {

inline void AppendLiveLog(const char* text) {
  static FILE* file = nullptr;
  static bool tried = false;
  if (!tried) {
    tried = true;
    const char* path = getenv("LIBERTY_LIVE_LOG");
    if (path && *path) file = fopen(path, "a");
  }
  if (!file) return;
  timespec now{};
  clock_gettime(CLOCK_REALTIME, &now);
  tm local{};
  localtime_r(&now.tv_sec, &local);
  char stamp[24];
  strftime(stamp, sizeof(stamp), "%m-%d %H:%M:%S", &local);
  fprintf(file, "%s.%03ld LibertyNative: %s\n", stamp, now.tv_nsec / 1000000, text);
  fflush(file);
}

}  // namespace gta4::android
