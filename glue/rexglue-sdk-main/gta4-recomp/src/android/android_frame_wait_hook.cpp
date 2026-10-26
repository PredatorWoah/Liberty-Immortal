// sub_82A46D70(device r3, out r4) writes {0, submitted frame, submitted -
// completed} for the device. Its only caller, sub_828BF420, polls it in a
// loop without sleeping while two frames are pending, i.e. while the title
// waits for the renderer to submit a frame: a busy-wait that held a big core
// at ~7% of the main game thread's samples. Sleep briefly when the title is
// about to spin; the game's own loop still decides when to stop waiting.
//
// The same two-frame limit caps throughput: one frame passes the command
// worker, the recorder and the GPU in sequence (~75 ms at the bridge), so two
// in flight give ~38 ms per frame while every stage has slack. Reporting
// fewer pending frames lets the title run that many frames further ahead; the
// renderer's own queue still bounds the total.
#include <time.h>

#include <algorithm>
#include <cstdio>

#include <android/log.h>

#include "android/android_performance_hint.h"

#include <atomic>

#include <rex/cvar.h>

#include "gta4_init.h"

// No frame rate change, but the spin held the main game thread at ~97% of a
// big core while it waited (17% with the sleep): the whole process used 2.16
// cores at the save point with the spin and 1.37 without (Retroid Pocket 5).
// It was off for a while because the game-code crash in sub_8296E480 (stale
// object pointer) first appeared in builds that had it; that crash has its
// own guard now.
REXCVAR_DEFINE_BOOL(gta4_frame_wait_sleep, true, "GTA IV/Performance",
                    "Sleep instead of spinning while the title waits for a frame slot");
REXCVAR_DEFINE_UINT32(gta4_extra_frames_in_flight, 0, "GTA IV/Performance",
                      "Frames the title may run ahead beyond its own limit of two pending "
                      "frames (more throughput, one frame of input latency each)")
    .range(0, 2);

// Latest game update rate, shown by the in-game "Show FPS" overlay.
std::atomic<float> g_liberty_title_fps{0.0f};

extern "C" void sub_82A46D70(PPCContext& ctx, uint8_t* base) {
  const uint32_t out = ctx.r4.u32;
  __imp__sub_82A46D70(ctx, base);
  uint32_t pending = REX_LOAD_U32(out + 8);
  if (pending >= 0x80000000u) return;
  // Title frame rate: how often the game submits a new frame (world update),
  // as opposed to how often frames are presented. Logged every 2 seconds.
  {
    static uint32_t last_submitted = 0;
    static uint32_t submitted_count = 0;
    static timespec window_start{};
    // Frame pacing: the time between consecutive submitted frames. One clock
    // read per frame; a summary every 2 seconds shows whether the stutter is
    // a steady rhythm (16/33/16/33 ms) or random spikes.
    static int64_t last_frame_ns = 0;
    static float intervals[512];
    static uint32_t interval_count = 0;
    static float recent[24];
    static uint32_t recent_next = 0;
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const int64_t now_ns = int64_t(now.tv_sec) * 1000000000 + now.tv_nsec;
    const uint32_t submitted = REX_LOAD_U32(out + 4);
    if (submitted != last_submitted) {
      last_submitted = submitted;
      ++submitted_count;
      gta4::android::NoteTitleFrame();
      if (last_frame_ns && now_ns - last_frame_ns < 1000000000) {
        const float ms = float(now_ns - last_frame_ns) / 1e6f;
        if (interval_count < 512) intervals[interval_count++] = ms;
        recent[recent_next++ % 24] = ms;
      }
      last_frame_ns = now_ns;
    }
    if (window_start.tv_sec == 0) window_start = now;
    const double elapsed = double(now.tv_sec - window_start.tv_sec) +
                           double(now.tv_nsec - window_start.tv_nsec) / 1e9;
    if (elapsed >= 0.5) {
      g_liberty_title_fps.store(float(submitted_count / elapsed), std::memory_order_relaxed);
    }
    if (elapsed >= 2.0) {
      __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp",
                          "title-fps: %.1f (game frames submitted per second)",
                          submitted_count / elapsed);
      if (interval_count >= 8) {
        float sorted[512];
        std::copy(intervals, intervals + interval_count, sorted);
        std::sort(sorted, sorted + interval_count);
        const auto percentile = [&](double q) {
          return sorted[std::min<uint32_t>(interval_count - 1, uint32_t(q * interval_count))];
        };
        const float median = percentile(0.5);
        float sum = 0;
        uint32_t spikes = 0, over50 = 0, over100 = 0;
        for (uint32_t i = 0; i < interval_count; ++i) {
          sum += intervals[i];
          if (intervals[i] > median * 1.5f) ++spikes;
          if (intervals[i] > 50.0f) ++over50;
          if (intervals[i] > 100.0f) ++over100;
        }
        // The last frames in order, oldest first.
        char pattern[24 * 6 + 1];
        size_t used = 0;
        const uint32_t shown = std::min<uint32_t>(recent_next, 24);
        for (uint32_t i = 0; i < shown; ++i) {
          const uint32_t index = (recent_next - shown + i) % 24;
          used += std::snprintf(pattern + used, sizeof(pattern) - used, "%.0f ", recent[index]);
        }
        pattern[used] = 0;
        __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp",
                            "frame-times: n=%u avg=%.1f p50=%.1f p95=%.1f p99=%.1f max=%.1f ms "
                            "spikes>1.5x=%u over50ms=%u over100ms=%u recent=[%s]",
                            interval_count, sum / interval_count, median, percentile(0.95),
                            percentile(0.99), sorted[interval_count - 1], spikes, over50, over100,
                            pattern);
      }
      interval_count = 0;
      submitted_count = 0;
      window_start = now;
    }
  }
  const uint32_t extra = REXCVAR_GET(gta4_extra_frames_in_flight);
  if (extra) {
    pending = pending > extra ? pending - extra : 0;
    REX_STORE_U32(out + 8, pending);
  }
  if (pending >= 2 && REXCVAR_GET(gta4_frame_wait_sleep)) {
    const timespec delay{0, 250 * 1000};
    nanosleep(&delay, nullptr);
  }
}
