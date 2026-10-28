/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <vector>
#include <stack>

#include <rex/audio/audio_driver.h>
#include <rex/thread.h>

#include <SDL3/SDL.h>

namespace rex::audio::sdl {

class SDLAudioDriver : public AudioDriver {
 public:
  SDLAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore);
  ~SDLAudioDriver() override;

  bool Initialize();
  void SubmitFrame(uint32_t frame_ptr) override;
  void Pause() override;
  void Resume() override;
  void Flush() override;
  void Shutdown();

 protected:
  static void SDLCallback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                          int total_amount);
  void FlushQueuedFrames();

  rex::thread::Semaphore* semaphore_ = nullptr;

  SDL_AudioStream* sdl_stream_ = nullptr;
  bool sdl_initialized_ = false;
  bool silent_fallback_ = false;
  uint8_t sdl_device_channels_ = 0;

  static const uint32_t frame_frequency_ = 48000;
  static const uint32_t frame_channels_ = 6;
  static const uint32_t channel_samples_ = 256;
  static const uint32_t frame_samples_ = frame_channels_ * channel_samples_;
  static const uint32_t frame_size_ = sizeof(float) * frame_samples_;
  static const uint32_t silent_frame_duration_microseconds_ = 5333;
  std::queue<float*> frames_queued_ = {};
  std::stack<float*> frames_unused_ = {};
  std::mutex frames_mutex_ = {};
  // Report-period statistics, guarded by frames_mutex_.
  size_t min_queue_seen_ = SIZE_MAX;
  double max_submit_gap_ms_ = 0.0;
  uint64_t submitted_blocks_ = 0;
  void CaptureBlock(const float* interleaved, uint32_t channels);
  int capture_state_ = 0;  // 0 not started, 1 recording, 2 finished or off
  int capture_seconds_ = 0;
  std::string capture_path_;
  std::vector<int16_t> capture_;
  std::chrono::steady_clock::time_point capture_started_{};
  uint32_t guest_silent_blocks_ = 0;
  uint32_t guest_silent_run_ = 0;
  uint32_t guest_silent_runs_ = 0;
  uint32_t guest_silent_longest_ = 0;
  std::chrono::steady_clock::time_point last_submit_{};
};

}  // namespace rex::audio::sdl
