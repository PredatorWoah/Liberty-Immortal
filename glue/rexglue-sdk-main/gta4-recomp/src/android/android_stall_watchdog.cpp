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
#include <ucontext.h>
#include <unwind.h>

#include <android/log.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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

// ---------------------------------------------------------------------------------------------
// CPU sampler (debug-log sessions): ten times a second, every thread that is running at that moment
// gets a real-time signal whose handler stores the interrupted program counter and the link register
// (no unwinding: safe in a signal handler). Every minute the log gets the CPU share of each thread name
// and the hottest code addresses, which a symbol table of the same build turns into function names:
// where the CPU time (and so the heat) goes.
// ---------------------------------------------------------------------------------------------
struct PcSample {
  uintptr_t pc = 0, lr = 0;
  std::atomic<int> done{0};
};
PcSample g_pc_sample;

void PcSignalHandler(int, siginfo_t*, void* context) {
#if defined(__aarch64__)
  const auto* uc = static_cast<const ucontext_t*>(context);
  g_pc_sample.pc = uintptr_t(uc->uc_mcontext.pc);
  g_pc_sample.lr = uintptr_t(uc->uc_mcontext.regs[30]);
#else
  (void)context;
  g_pc_sample.pc = g_pc_sample.lr = 0;
#endif
  g_pc_sample.done.store(1, std::memory_order_release);
}

void SamplerLoop() {
  const int signal_number = SIGRTMIN + 6;
  struct sigaction action {};
  action.sa_sigaction = PcSignalHandler;
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&action.sa_mask);
  sigaction(signal_number, &action, nullptr);
  const pid_t self = gettid();
  const pid_t pid = getpid();
  std::unordered_map<pid_t, std::string> names;
  std::map<std::string, uint32_t> per_thread;                 // thread name -> samples
  std::map<std::pair<uintptr_t, uintptr_t>, uint32_t> per_pc;  // (pc bucket, lr bucket) -> samples
  std::map<std::pair<uintptr_t, uintptr_t>, std::string> bucket_thread;
  std::unordered_map<pid_t, std::pair<unsigned long, unsigned long>> last_ticks;  // utime, stime at the last round
  std::map<std::string, std::pair<unsigned long, unsigned long>> cpu_ticks;      // per thread name, this window
  uint32_t total = 0, ticks = 0;
  auto next_dump = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  for (;;) {
    timespec pause{0, 100 * 1000 * 1000};
    nanosleep(&pause, nullptr);
    ++ticks;
    if (DIR* directory = opendir("/proc/self/task")) {
      while (dirent* entry = readdir(directory)) {
        const int tid = atoi(entry->d_name);
        if (tid <= 0 || tid == self) continue;
        const std::string base = std::string("/proc/self/task/") + entry->d_name;
        const std::string stat = ReadSmallFile(base + "/stat");
        const size_t close = stat.rfind(')');
        if (close == std::string::npos || close + 2 >= stat.size()) continue;
        auto name = names.find(tid);
        if (name == names.end()) name = names.emplace(tid, ReadSmallFile(base + "/comm")).first;
        {
          // CPU time of every thread, user and kernel, in 10 ms ticks since the last round.
          char state = '?';
          unsigned long utime = 0, stime = 0;
          std::sscanf(stat.c_str() + close + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &state, &utime, &stime);
          auto& last = last_ticks[tid];
          if (last.first || last.second) {
            auto& sum = cpu_ticks[name->second];
            if (utime >= last.first) sum.first += utime - last.first;
            if (stime >= last.second) sum.second += stime - last.second;
          }
          last = {utime, stime};
        }
        if (stat[close + 2] != 'R') continue;
        g_pc_sample.done.store(0, std::memory_order_release);
        if (syscall(SYS_tgkill, pid, tid, signal_number) != 0) continue;
        for (int wait = 0; wait < 20 && !g_pc_sample.done.load(std::memory_order_acquire); ++wait) {
          timespec tiny{0, 500 * 1000};
          nanosleep(&tiny, nullptr);
        }
        if (!g_pc_sample.done.load(std::memory_order_acquire)) continue;
        ++total;
        ++per_thread[name->second];
        const std::pair<uintptr_t, uintptr_t> key{g_pc_sample.pc >> 6, g_pc_sample.lr >> 6};
        ++per_pc[key];
        auto& owner = bucket_thread[key];
        if (owner.find(name->second) == std::string::npos && owner.size() < 60) owner += (owner.empty() ? "" : ",") + name->second;
      }
      closedir(directory);
    }
    if (std::chrono::steady_clock::now() < next_dump) continue;
    next_dump += std::chrono::seconds(60);
    if (total) {
      char head[120];
      std::snprintf(head, sizeof(head), "cpu-profile: %u samples of running threads in 60 s (%u sample rounds)", total, ticks);
      Emit(head);
      {
        // CPU time per thread name over the minute, in percent of one core (user + kernel).
        std::vector<std::pair<unsigned long, std::string>> by_cpu;
        for (const auto& [thread_name, sum] : cpu_ticks) by_cpu.emplace_back(sum.first + sum.second, thread_name);
        std::sort(by_cpu.rbegin(), by_cpu.rend());
        unsigned long all = 0;
        for (const auto& entry : by_cpu) all += entry.first;
        char summary[100];
        std::snprintf(summary, sizeof(summary), "cpu-time: all threads %.0f%% of one core in the last minute", all / 60.0);
        Emit(summary);
        for (size_t i = 0; i < by_cpu.size() && i < 14; ++i) {
          const auto& sum = cpu_ticks[by_cpu[i].second];
          char line[160];
          std::snprintf(line, sizeof(line), "cpu-time: %5.1f%% (user %.1f%%, kernel %.1f%%) %s", by_cpu[i].first / 60.0,
                        sum.first / 60.0, sum.second / 60.0, by_cpu[i].second.c_str());
          Emit(line);
        }
      }
      std::vector<std::pair<uint32_t, std::string>> threads;
      for (const auto& [thread_name, count] : per_thread) threads.emplace_back(count, thread_name);
      std::sort(threads.rbegin(), threads.rend());
      for (size_t i = 0; i < threads.size() && i < 10; ++i) {
        char line[160];
        std::snprintf(line, sizeof(line), "cpu-thread: %5.1f%% %s", 100.0 * threads[i].first / total, threads[i].second.c_str());
        Emit(line);
      }
      std::vector<std::pair<uint32_t, std::pair<uintptr_t, uintptr_t>>> hot;
      for (const auto& [key, count] : per_pc) hot.emplace_back(count, key);
      std::sort(hot.rbegin(), hot.rend());
      for (size_t i = 0; i < hot.size() && i < 40; ++i) {
        const auto key = hot[i].second;
        std::string line = "cpu-hot: " + std::to_string(hot[i].first) + " pc=" + Describe(key.first << 6) +
                           " lr=" + Describe(key.second << 6) + " threads=" + bucket_thread[key];
        Emit(line.c_str());
      }
    }
    cpu_ticks.clear();
    per_thread.clear();
    per_pc.clear();
    bucket_thread.clear();
    total = 0;
    ticks = 0;
  }
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
  std::thread(SamplerLoop).detach();
}

}  // namespace gta4::android
