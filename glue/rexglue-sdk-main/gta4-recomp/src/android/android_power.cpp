// Power and heat: Android's performance hint (ADPF) and thermal headroom.
//
// Left alone, the CPU governor sees busy threads and raises the clocks of
// their cores toward the maximum, although a frame only has to be ready every
// 33 ms: the extra speed turns into idle time and heat. A performance hint
// session tells the system how long each frame's work took against its
// target, so it can pick the lowest clocks that still meet the target (and
// boost early when a frame runs long). The session holds the busiest threads
// of the process, re-ranked every two seconds like the thread pinning, and
// once per frame period it reports the largest CPU time any of them used.
//
// Thermal headroom (0 = cool, 1 = the device starts throttling) is read every
// two seconds and logged. With android_thermal_guard the draw distance is
// lowered before the device throttles and restored once it has cooled down,
// trading distant detail for steady clocks instead of a throttled frame rate.
// Devices without a thermal HAL report no headroom (the Retroid Pocket 5, for
// one); the guard then follows the GPU temperature instead.
//
// Both APIs are looked up at run time (performance hints need Android 13,
// thermal headroom Android 11) and need the device's power and thermal HALs;
// without them the hints are skipped.
#include <dirent.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <android/log.h>

#include "android/android_live_log.h"

#include <rex/cvar.h>

// Mali Edition: off by default. The Mali Edition's own hint
// (android_performance_hint.cpp, "CPU boost" in the launcher) keeps the game's
// threads on fast clocks; this one asks for the lowest clocks that still meet
// 30 FPS. Running both would send Android opposite signals for the same threads.
REXCVAR_DEFINE_BOOL(android_perf_hint, false, "GTA IV/Performance",
                    "Report each frame's work to Android's performance hint API so the CPU "
                    "clocks follow the frame budget instead of running at the maximum");
REXCVAR_DEFINE_DOUBLE(android_perf_hint_target_ms, 30.0, "GTA IV/Performance",
                      "Work time per frame the performance hint asks for (a margin below the "
                      "33.3 ms of 30 FPS)")
    .range(8.0, 33.0);
REXCVAR_DEFINE_BOOL(android_thermal_guard, false, "GTA IV/Performance",
                    "Lower the draw distance when Android forecasts thermal throttling and "
                    "restore it after the device has cooled down");
REXCVAR_DEFINE_DOUBLE(android_thermal_guard_hot_c, 78.0, "GTA IV/Performance",
                      "Thermal guard without thermal headroom: GPU temperature (C) that lowers "
                      "the draw distance")
    .range(50.0, 110.0);
REXCVAR_DEFINE_DOUBLE(android_thermal_guard_cool_c, 68.0, "GTA IV/Performance",
                      "Thermal guard without thermal headroom: GPU temperature (C) that restores "
                      "the draw distance")
    .range(40.0, 100.0);

REXCVAR_DECLARE(double, gta4_draw_distance_scale);

namespace {

using APerformanceHintManager = struct APerformanceHintManager;
using APerformanceHintSession = struct APerformanceHintSession;
using AThermalManager = struct AThermalManager;

struct Api {
  APerformanceHintManager* (*get_manager)() = nullptr;
  APerformanceHintSession* (*create_session)(APerformanceHintManager*, const int32_t*, size_t,
                                             int64_t) = nullptr;
  int (*update_target)(APerformanceHintSession*, int64_t) = nullptr;
  int (*report_actual)(APerformanceHintSession*, int64_t) = nullptr;
  void (*close_session)(APerformanceHintSession*) = nullptr;
  AThermalManager* (*thermal_acquire)() = nullptr;
  float (*thermal_headroom)(AThermalManager*, int) = nullptr;
};

Api LoadApi() {
  Api api;
  void* library = dlopen("libandroid.so", RTLD_NOW);
  if (!library) return api;
  const auto find = [library](auto& function, const char* name) {
    function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(dlsym(library, name));
  };
  find(api.get_manager, "APerformanceHint_getManager");
  find(api.create_session, "APerformanceHint_createSession");
  find(api.update_target, "APerformanceHint_updateTargetWorkDuration");
  find(api.report_actual, "APerformanceHint_reportActualWorkDuration");
  find(api.close_session, "APerformanceHint_closeSession");
  find(api.thermal_acquire, "AThermal_acquireManager");
  find(api.thermal_headroom, "AThermal_getThermalHeadroom");
  return api;
}

// CPU time a thread of this process has used (CPUCLOCK_SCHED, per thread).
int64_t ThreadCpuNanoseconds(pid_t tid) {
  const clockid_t clock = clockid_t((~unsigned(tid) << 3) | 6u);
  timespec time{};
  if (clock_gettime(clock, &time) != 0) return -1;
  return int64_t(time.tv_sec) * 1000000000 + time.tv_nsec;
}

// The busiest threads over the last interval, by CPU time.
// Adreno GPU temperature in degrees C (kgsl reports millidegrees); NaN if absent.
float GpuTemperature() {
  float value = __builtin_nanf("");
  if (FILE* file = std::fopen("/sys/class/kgsl/kgsl-3d0/temp", "r")) {
    long raw = 0;
    if (std::fscanf(file, "%ld", &raw) == 1) value = raw > 1000 ? float(raw) / 1000.0f : float(raw);
    std::fclose(file);
  }
  return value;
}

std::vector<pid_t> BusiestThreads(std::unordered_map<pid_t, int64_t>& previous, size_t count) {
  std::vector<std::pair<int64_t, pid_t>> usage;
  std::unordered_map<pid_t, int64_t> current;
  if (DIR* directory = opendir("/proc/self/task")) {
    while (dirent* entry = readdir(directory)) {
      const pid_t tid = pid_t(std::atoi(entry->d_name));
      if (tid <= 0) continue;
      const int64_t time = ThreadCpuNanoseconds(tid);
      if (time < 0) continue;
      current[tid] = time;
      const auto old = previous.find(tid);
      if (old != previous.end() && time > old->second) usage.emplace_back(time - old->second, tid);
    }
    closedir(directory);
  }
  previous.swap(current);
  std::sort(usage.rbegin(), usage.rend());
  std::vector<pid_t> result;
  for (size_t i = 0; i < usage.size() && i < count; ++i) {
    // Threads that used under 5% of the interval carry no frame work.
    if (usage[i].first < 100'000'000) break;
    result.push_back(usage[i].second);
  }
  std::sort(result.begin(), result.end());
  return result;
}

void PowerLoop() {
  const Api api = LoadApi();
  const bool hints = api.get_manager && api.create_session && api.update_target &&
                     api.report_actual && api.close_session;
  // AThermal_acquireManager is API 30, AThermal_getThermalHeadroom API 31:
  // Android 11 has the manager without the headroom (issue #58).
  AThermalManager* thermal =
      api.thermal_acquire && api.thermal_headroom ? api.thermal_acquire() : nullptr;
  APerformanceHintManager* manager = hints ? api.get_manager() : nullptr;
  __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp", "power: performance hints %s, thermal headroom %s",
                      manager ? "available" : "unavailable", thermal ? "available" : "unavailable");

  using Clock = std::chrono::steady_clock;
  constexpr auto kPeriod = std::chrono::microseconds(33333);
  constexpr auto kRankInterval = std::chrono::seconds(2);
  APerformanceHintSession* session = nullptr;
  std::vector<pid_t> threads;
  std::vector<int64_t> thread_times;
  std::unordered_map<pid_t, int64_t> rank_times;
  int64_t target_ns = 0;
  auto next_rank = Clock::now();
  auto next_tick = Clock::now() + kPeriod;
  auto next_log = Clock::now() + std::chrono::seconds(10);
  float headroom = __builtin_nanf("");
  float gpu_temperature = __builtin_nanf("");
  double guard_saved_scale = 0.0;  // the draw distance the guard lowered; 0 = not lowered
  int guard_stage = 0;
  std::string guard_saved_shadow, guard_saved_env, guard_saved_water;
  double reported_sum_ms = 0.0;
  uint32_t reported = 0;

  for (;;) {
    std::this_thread::sleep_until(next_tick);
    const auto now = Clock::now();
    next_tick += kPeriod;
    if (next_tick < now) next_tick = now + kPeriod;  // fell behind (suspended): resynchronize

    const bool enabled = manager && REXCVAR_GET(android_perf_hint);
    if (!enabled && session) {
      api.close_session(session);
      session = nullptr;
    }

    if (now >= next_rank) {
      next_rank = now + kRankInterval;
      const std::vector<pid_t> busiest = BusiestThreads(rank_times, 4);
      if (enabled && busiest != threads) {
        if (session) api.close_session(session);
        session = nullptr;
        threads = busiest;
        thread_times.assign(threads.size(), -1);
        if (!threads.empty()) {
          std::vector<int32_t> ids(threads.begin(), threads.end());
          target_ns = int64_t(REXCVAR_GET(android_perf_hint_target_ms) * 1e6);
          session = api.create_session(manager, ids.data(), ids.size(), target_ns);
        }
      }
      headroom = thermal ? api.thermal_headroom(thermal, 10) : __builtin_nanf("");  // 10 s ahead
      gpu_temperature = GpuTemperature();
      // Staged heat guard. One step was not enough on a hot phone (headroom kept rising to
      // 1.07 after a single draw distance cut), so each stage trims more work, and the
      // stages start earlier and come back one at a time as the phone cools.
      //   1 (0.88): draw distance x0.85        2 (0.95): x0.70
      //   3 (1.00): + shadow map every other frame
      //   4 (1.05): x0.55, + environment reflections every 3rd frame, water every other frame
      static constexpr float kUp[4] = {0.88f, 0.95f, 1.00f, 1.05f};
      static constexpr float kDown[4] = {0.78f, 0.85f, 0.92f, 0.98f};
      static constexpr double kDistance[5] = {1.0, 0.85, 0.70, 0.70, 0.55};
      int target = guard_stage;
      if (!REXCVAR_GET(android_thermal_guard)) {
        target = 0;
      } else if (headroom == headroom) {
        while (target < 4 && headroom >= kUp[target]) ++target;
        while (target > 0 && headroom <= kDown[target - 1]) --target;
      } else if (gpu_temperature == gpu_temperature) {
        if (gpu_temperature >= REXCVAR_GET(android_thermal_guard_hot_c)) target = std::max(target, 2);
        else if (gpu_temperature <= REXCVAR_GET(android_thermal_guard_cool_c)) target = 0;
      }
      if (target != guard_stage) {
        if (target > 0 && guard_stage == 0) {
          guard_saved_scale = REXCVAR_GET(gta4_draw_distance_scale);
          guard_saved_shadow = rex::cvar::GetFlagByName("gta4_native_shadow_interval");
          guard_saved_env = rex::cvar::GetFlagByName("gta4_native_environment_reflection_interval");
          guard_saved_water = rex::cvar::GetFlagByName("gta4_native_water_reflection_interval");
        }
        const double scale = target == 0 ? guard_saved_scale
                                         : std::max(0.45, guard_saved_scale * kDistance[target]);
        rex::cvar::SetFlagByName("gta4_draw_distance_scale", std::to_string(scale));
        rex::cvar::SetFlagByName("gta4_native_shadow_interval",
                                 target >= 3 ? "2" : guard_saved_shadow.c_str());
        rex::cvar::SetFlagByName("gta4_native_environment_reflection_interval",
                                 target >= 4 ? "3" : guard_saved_env.c_str());
        rex::cvar::SetFlagByName("gta4_native_water_reflection_interval",
                                 target >= 4 ? "2" : guard_saved_water.c_str());
        char line[200];
        std::snprintf(line, sizeof(line),
                      "thermal-guard: stage %d -> %d, headroom %.2f, draw distance %.2f%s%s",
                      guard_stage, target, headroom, scale, target >= 3 ? ", shadows every 2nd frame" : "",
                      target >= 4 ? ", reflections reduced" : "");
        __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp", "%s", line);
        gta4::android::AppendLiveLog(line);
        guard_stage = target;
        if (target == 0) guard_saved_scale = 0.0;
      }
    }

    if (session) {
      const int64_t wanted = int64_t(REXCVAR_GET(android_perf_hint_target_ms) * 1e6);
      if (wanted != target_ns) {
        target_ns = wanted;
        api.update_target(session, target_ns);
      }
      // The frame's critical path is the busiest of the session's threads.
      int64_t work = 0;
      for (size_t i = 0; i < threads.size(); ++i) {
        const int64_t time = ThreadCpuNanoseconds(threads[i]);
        if (time >= 0 && thread_times[i] >= 0) work = std::max(work, time - thread_times[i]);
        thread_times[i] = time;
      }
      if (work > 0) {
        api.report_actual(session, work);
        reported_sum_ms += double(work) / 1e6;
        ++reported;
      }
    }

    if (now >= next_log) {
      next_log = now + std::chrono::seconds(10);
      std::string ids;
      for (pid_t tid : threads) ids += (ids.empty() ? "" : ",") + std::to_string(tid);
      __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp",
                          "power: headroom=%.2f gpu=%.1fC hint=%s work=%.1fms/target %.1fms "
                          "threads=%s%s",
                          headroom, gpu_temperature, session ? "on" : "off",
                          reported ? reported_sum_ms / reported : 0.0, double(target_ns) / 1e6,
                          ids.c_str(), guard_saved_scale ? " guard=lowered" : "");
      reported_sum_ms = 0.0;
      reported = 0;
    }
  }
}

const bool g_power_started = [] {
  std::thread(PowerLoop).detach();
  return true;
}();

}  // namespace
