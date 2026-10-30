#include "android/android_performance_hint.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <dirent.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>

#include <android/log.h>

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(android_performance_hint, true, "GTA IV/Performance",
                    "Report frame times to Android's Performance Hint API so the system "
                    "raises the CPU clocks of the game's busiest threads when frames run late");
REXCVAR_DEFINE_UINT32(android_performance_hint_target_ms, 25, "GTA IV/Performance",
                      "Frame time the performance hint asks Android for. Below the 33 ms of "
                      "30 FPS so the clocks stay up between busy moments instead of dropping "
                      "and recovering (steadier, a little warmer); 33 = the old behaviour")
    .range(10, 50);
REXCVAR_DEFINE_BOOL(android_performance_hint_report_work, false, "GTA IV/Performance",
                    "Experimental, off by default: reporting the real CPU work let Android drop the clocks until "
                    "the game ran at 14 FPS. Report the CPU time the busiest thread spends per frame as the work "
                    "duration, which is what Android's Performance Hint expects. false reports the frame "
                    "interval (33 ms at 30 FPS), which always looks late next to the target and keeps "
                    "the CPU clocks high (hot)");

REXCVAR_DEFINE_DOUBLE(android_performance_hint_report_scale, 0.8, "GTA IV/Performance",
                      "Share of the frame interval reported to Android's Performance Hint as the work duration "
                      "(0.8 = 26 ms at 30 FPS). 1.0 always looks late and keeps the clocks high (hot); real "
                      "work (about 0.65) let Android drop the clocks until the game ran at 14 FPS")
    .range(0.5, 1.0);

namespace gta4::android {
namespace {

constexpr int64_t kMaxFrameNs = 4 * 33'333'333;  // clamp for pauses and loading stalls

int64_t TargetFrameNs() {
  return int64_t(std::clamp<uint32_t>(REXCVAR_GET(android_performance_hint_target_ms), 10, 50)) * 1'000'000;
}
constexpr size_t kMaxHintThreads = 6;
// CPU ticks (100 per second) a thread needs in one second to count as busy.
constexpr uint64_t kBusyTicks = 10;

std::mutex g_frames_mutex;
std::vector<int64_t> g_frame_durations;  // filled by NoteTitleFrame
int64_t g_last_frame_ns = 0;

int64_t NowNs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

// NDK API (libandroid.so, API 33+; setThreads is API 34+), loaded at runtime
// because the app's minSdk is lower.
struct HintApi {
  using GetManagerFn = void* (*)();
  using CreateSessionFn = void* (*)(void*, const int32_t*, size_t, int64_t);
  using ReportFn = int (*)(void*, int64_t);
  using SetThreadsFn = int (*)(void*, const pid_t*, size_t);
  using CloseFn = void (*)(void*);
  using UpdateTargetFn = int (*)(void*, int64_t);
  using PreferPowerFn = int (*)(void*, bool);
  GetManagerFn get_manager = nullptr;
  CreateSessionFn create_session = nullptr;
  ReportFn report = nullptr;
  SetThreadsFn set_threads = nullptr;
  CloseFn close = nullptr;
  UpdateTargetFn update_target = nullptr;
  PreferPowerFn prefer_power = nullptr;

  bool Load() {
    void* library = dlopen("libandroid.so", RTLD_NOW);
    if (!library) return false;
    get_manager = reinterpret_cast<GetManagerFn>(dlsym(library, "APerformanceHint_getManager"));
    create_session =
        reinterpret_cast<CreateSessionFn>(dlsym(library, "APerformanceHint_createSession"));
    report =
        reinterpret_cast<ReportFn>(dlsym(library, "APerformanceHint_reportActualWorkDuration"));
    set_threads = reinterpret_cast<SetThreadsFn>(dlsym(library, "APerformanceHint_setThreads"));
    close = reinterpret_cast<CloseFn>(dlsym(library, "APerformanceHint_closeSession"));
    update_target = reinterpret_cast<UpdateTargetFn>(
        dlsym(library, "APerformanceHint_updateTargetWorkDuration"));
    prefer_power = reinterpret_cast<PreferPowerFn>(
        dlsym(library, "APerformanceHint_setPreferPowerEfficiency"));
    return get_manager && create_session && report && close;
  }
};

// The busiest threads of this process over the last second, busiest first.
class BusyThreads {
 public:
  // CPU ticks (100 per second) the busiest thread used in the last second (0 when none was busy).
  uint64_t max_delta() const { return max_delta_; }

  std::vector<pid_t> Sample(pid_t exclude) {
    std::unordered_map<pid_t, uint64_t> ticks;
    std::vector<std::pair<uint64_t, pid_t>> busy;
    if (DIR* tasks = opendir("/proc/self/task")) {
      while (const dirent* entry = readdir(tasks)) {
        if (!std::isdigit(static_cast<unsigned char>(entry->d_name[0]))) continue;
        const pid_t tid = pid_t(std::atoi(entry->d_name));
        if (tid == exclude) continue;
        std::ifstream stat_file(std::string("/proc/self/task/") + entry->d_name + "/stat");
        std::string stat((std::istreambuf_iterator<char>(stat_file)), {});
        const size_t name_end = stat.rfind(')');
        if (name_end == std::string::npos) continue;
        // Fields after the name start at 3 (state); utime is 14, stime 15.
        std::istringstream fields(stat.substr(name_end + 2));
        std::string field;
        uint64_t utime = 0, stime = 0;
        for (int index = 3; index <= 15 && fields >> field; ++index) {
          if (index == 14) utime = std::strtoull(field.c_str(), nullptr, 10);
          if (index == 15) stime = std::strtoull(field.c_str(), nullptr, 10);
        }
        ticks[tid] = utime + stime;
        const auto last = last_ticks_.find(tid);
        if (last != last_ticks_.end() && utime + stime - last->second >= kBusyTicks) {
          busy.emplace_back(utime + stime - last->second, tid);
        }
      }
      closedir(tasks);
    }
    last_ticks_ = std::move(ticks);
    std::sort(busy.begin(), busy.end(), std::greater<>());
    max_delta_ = busy.empty() ? 0 : busy.front().first;
    std::vector<pid_t> result;
    for (const auto& [delta, tid] : busy) {
      if (result.size() == kMaxHintThreads) break;
      result.push_back(tid);
    }
    std::sort(result.begin(), result.end());
    return result;
  }

 private:
  std::unordered_map<pid_t, uint64_t> last_ticks_;
  uint64_t max_delta_ = 0;
};

std::string ThreadName(pid_t tid) {
  std::ifstream comm("/proc/self/task/" + std::to_string(tid) + "/comm");
  std::string name;
  std::getline(comm, name);
  return name.empty() ? "?" : name;
}

void LogThreads(const char* what, const std::vector<pid_t>& threads) {
  std::string list;
  for (pid_t tid : threads) {
    if (!list.empty()) list += ", ";
    list += ThreadName(tid) + ":" + std::to_string(tid);
  }
  __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp", "performance-hint: %s %zu threads [%s]", what,
                      threads.size(), list.c_str());
}

void Log(const char* message, size_t count) {
  __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp", "performance-hint: %s (%zu threads)",
                      message, count);
}

}  // namespace

void NoteTitleFrame() noexcept {
  const int64_t now = NowNs();
  std::lock_guard lock(g_frames_mutex);
  if (g_last_frame_ns != 0) {
    // Clamp pauses and loading stalls so one long gap does not dominate.
    g_frame_durations.push_back(std::min<int64_t>(now - g_last_frame_ns, kMaxFrameNs));
    if (g_frame_durations.size() > 256) g_frame_durations.erase(g_frame_durations.begin());
  }
  g_last_frame_ns = now;
}

void StartPerformanceHint() {
  std::thread([] {
    HintApi api;
    if (!api.Load()) {
      Log("not available on this Android version", 0);
      return;
    }
    void* manager = api.get_manager();
    if (!manager) {
      Log("no hint manager", 0);
      return;
    }
    const pid_t self = pid_t(gettid());
    BusyThreads busy;
    std::vector<pid_t> threads;
    void* session = nullptr;
    int64_t session_target = 0;
    int tick = 0;
    uint64_t frames_this_second = 0;
    int64_t work_ns = 0;  // CPU time of the busiest thread per frame, refreshed every second
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (!REXCVAR_GET(android_performance_hint)) {
        if (session) {
          api.close(session);
          session = nullptr;
          threads.clear();
          Log("turned off", 0);
        }
        continue;
      }
      // Once a second: follow the busiest threads.
      if (++tick % 10 == 0) {
        std::vector<pid_t> hot = busy.Sample(self);
        // Work per frame = the busiest thread's CPU time over the second / frames in the second.
        if (frames_this_second >= 5 && busy.max_delta() > 0) {
          work_ns = std::clamp<int64_t>(int64_t(busy.max_delta()) * 10'000'000 / int64_t(frames_this_second),
                                        6'000'000, 40'000'000);
        }
        if (tick % 100 == 0) {  // every ten seconds
          __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp",
                              "performance-hint: reporting %s x%.2f, real work per frame %.1f ms (target %.1f ms)",
                              REXCVAR_GET(android_performance_hint_report_work) ? "work" : "the frame interval",
                              REXCVAR_GET(android_performance_hint_report_scale), double(work_ns) / 1e6, double(TargetFrameNs()) / 1e6);
        }
        frames_this_second = 0;
        // Keep every thread that was busy before and is still alive, so a
        // briefly idle main thread is not dropped; add the new busy ones.
        if (!hot.empty()) {
          std::vector<pid_t> merged = hot;
          for (pid_t tid : threads) {
            if (merged.size() >= kMaxHintThreads) break;
            if (std::find(merged.begin(), merged.end(), tid) == merged.end() &&
                access(("/proc/self/task/" + std::to_string(tid)).c_str(), F_OK) == 0) {
              merged.push_back(tid);
            }
          }
          std::sort(merged.begin(), merged.end());
          hot = merged;
        }
        if (!hot.empty() && hot != threads) {
          if (session && api.set_threads &&
              api.set_threads(session, hot.data(), hot.size()) == 0) {
            threads = hot;
            LogThreads("now boosting", threads);
          } else {
            if (session) api.close(session);
            session = api.create_session(manager, reinterpret_cast<const int32_t*>(hot.data()),
                                         hot.size(), TargetFrameNs());
            session_target = TargetFrameNs();
            threads = session ? hot : std::vector<pid_t>{};
            if (session) {
              // Android 15+: ask for performance over power efficiency.
              if (api.prefer_power) api.prefer_power(session, false);
              LogThreads("session started,", threads);
            } else {
              Log("could not create a session", hot.size());
            }
          }
        }
      }
      // Follow a changed target (live tuning) without recreating the session.
      if (session && api.update_target && TargetFrameNs() != session_target) {
        session_target = TargetFrameNs();
        api.update_target(session, session_target);
      }
      // Report the frames finished since the last pass.
      std::vector<int64_t> durations;
      {
        std::lock_guard lock(g_frames_mutex);
        durations.swap(g_frame_durations);
      }
      frames_this_second += durations.size();
      if (session) {
        const bool work = REXCVAR_GET(android_performance_hint_report_work) && work_ns > 0;
        for (int64_t duration : durations) {
          if (duration > 0) api.report(session, work ? work_ns : int64_t(double(duration) * REXCVAR_GET(android_performance_hint_report_scale)));
        }
      }
    }
  }).detach();
}

}  // namespace gta4::android
