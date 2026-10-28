// Stall watchdog: the game sometimes stops submitting frames for good (the main game thread spins at
// ~90% of a core while the thread that runs the frame loop is gone quiet). A hang leaves no crash
// report, so this finds out where every thread is: after four seconds without a frame it sends each
// thread a real-time signal whose handler walks the stack, and logs the return addresses as
// module+offset, which a symbol table of the same build turns into function names.
#include "android/android_stall_watchdog.h"

#include <dirent.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <unwind.h>

#include <android/log.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "android/android_live_log.h"

namespace gta4::android {
namespace {

constexpr int kMaxFrames = 24;
constexpr int kMaxThreads = 48;

std::atomic<uint64_t> g_frames{0};
std::atomic<bool> g_started{false};

struct Capture {
  uintptr_t pcs[kMaxFrames];
  int count = 0;
  std::atomic<int> done{0};
};
Capture g_capture;

struct UnwindState {
  uintptr_t* pcs;
  int count;
};

_Unwind_Reason_Code UnwindStep(struct _Unwind_Context* context, void* argument) {
  auto* state = static_cast<UnwindState*>(argument);
  const uintptr_t pc = _Unwind_GetIP(context);
  if (!pc || state->count >= kMaxFrames) return _URC_END_OF_STACK;
  state->pcs[state->count++] = pc;
  return _URC_NO_REASON;
}

void StackSignalHandler(int, siginfo_t*, void*) {
  UnwindState state{g_capture.pcs, 0};
  _Unwind_Backtrace(UnwindStep, &state);
  g_capture.count = state.count;
  g_capture.done.store(1, std::memory_order_release);
}

void Emit(const char* text) {
  __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp", "%s", text);
  AppendLiveLog(text);
}

std::string ReadSmallFile(const std::string& path) {
  std::string out;
  if (FILE* file = fopen(path.c_str(), "r")) {
    char buffer[512];
    const size_t n = fread(buffer, 1, sizeof(buffer) - 1, file);
    buffer[n] = 0;
    out = buffer;
    fclose(file);
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out;
}

std::string Describe(uintptr_t pc) {
  Dl_info info{};
  char text[192];
  if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fname) {
    const char* name = strrchr(info.dli_fname, '/');
    name = name ? name + 1 : info.dli_fname;
    const uintptr_t offset = pc - reinterpret_cast<uintptr_t>(info.dli_fbase);
    if (info.dli_sname) {
      std::snprintf(text, sizeof(text), "%s+0x%zx(%s)", name, size_t(offset), info.dli_sname);
    } else {
      std::snprintf(text, sizeof(text), "%s+0x%zx", name, size_t(offset));
    }
  } else {
    std::snprintf(text, sizeof(text), "0x%zx", size_t(pc));
  }
  return text;
}

void DumpThreads(int stalled_seconds) {
  char head[96];
  std::snprintf(head, sizeof(head), "stall: no game frame for %d s, dumping threads", stalled_seconds);
  Emit(head);
  const int signal_number = SIGRTMIN + 5;
  struct sigaction action {};
  action.sa_sigaction = StackSignalHandler;
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&action.sa_mask);
  sigaction(signal_number, &action, nullptr);

  const pid_t self = gettid();
  const pid_t pid = getpid();
  DIR* directory = opendir("/proc/self/task");
  if (!directory) return;
  int dumped = 0;
  while (dirent* entry = readdir(directory)) {
    const int tid = atoi(entry->d_name);
    if (tid <= 0 || dumped >= kMaxThreads) continue;
    ++dumped;
    const std::string base = std::string("/proc/self/task/") + entry->d_name;
    const std::string name = ReadSmallFile(base + "/comm");
    const std::string stat = ReadSmallFile(base + "/stat");
    char state = '?';
    unsigned long utime = 0, stime = 0;
    // "tid (comm) S ... utime stime ..." : fields after the closing parenthesis.
    if (const size_t close = stat.rfind(')'); close != std::string::npos) {
      std::sscanf(stat.c_str() + close + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &state,
                  &utime, &stime);
    }
    const std::string wait_channel = ReadSmallFile(base + "/wchan");
    std::string line = "stall-thread tid=" + std::to_string(tid) + " name=" + name + " state=" + state +
                       " cpu-ticks=" + std::to_string(utime + stime) + " wchan=" + wait_channel;
    if (tid == self) {
      line += " (watchdog)";
    } else {
      g_capture.done.store(0, std::memory_order_release);
      g_capture.count = 0;
      if (syscall(SYS_tgkill, pid, tid, signal_number) == 0) {
        for (int wait = 0; wait < 30 && !g_capture.done.load(std::memory_order_acquire); ++wait) {
          timespec pause{0, 5 * 1000 * 1000};
          nanosleep(&pause, nullptr);
        }
      }
      if (g_capture.done.load(std::memory_order_acquire)) {
        line += " stack:";
        // The first frames are the handler and the signal return; the rest is where the thread was.
        for (int i = 0; i < g_capture.count; ++i) line += " " + Describe(g_capture.pcs[i]);
      } else {
        line += " stack: (no answer)";
      }
    }
    Emit(line.c_str());
  }
  closedir(directory);
  Emit("stall: thread dump done");
}

void WatchdogLoop() {
  uint64_t last = g_frames.load(std::memory_order_relaxed);
  int stalled = 0;
  for (;;) {
    timespec pause{1, 0};
    nanosleep(&pause, nullptr);
    const uint64_t now = g_frames.load(std::memory_order_relaxed);
    if (now != last) {
      last = now;
      stalled = 0;
      continue;
    }
    ++stalled;
    if (stalled == 4 || (stalled > 4 && (stalled - 4) % 30 == 0)) DumpThreads(stalled);
  }
}

}  // namespace

void NoteFrameForStallWatchdog() {
  g_frames.fetch_add(1, std::memory_order_relaxed);
  if (g_started.load(std::memory_order_relaxed)) return;
  if (g_started.exchange(true)) return;
  const char* log = getenv("LIBERTY_LIVE_LOG");
  if (!log || !*log) return;  // only in Play (Debug Log) sessions
  std::thread(WatchdogLoop).detach();
}

}  // namespace gta4::android
