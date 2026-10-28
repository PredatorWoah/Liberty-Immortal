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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <cstring>
#include <string>

#include <rex/assert.h>
#include <rex/audio/conversion.h>
#include <rex/audio/flags.h>
#include <rex/audio/sdl/sdl_audio_driver.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <SDL3/SDL.h>

REXCVAR_DEFINE_BOOL(audio_mute, false, "Audio", "Mute audio output");
REXCVAR_DEFINE_INT32(audio_capture_seconds, 40, "Audio",
                     "Android debug logs: write this many seconds of the final stereo output to "
                     "audio-capture.wav next to the live log, starting 25 s after audio starts "
                     "(0 = off, at most 120)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

// A 16-bit WAV of the audio actually handed to the device, so choppy sound can be told apart as
// "already in the game's output" or "damaged later". Memory only while recording, written once.
void WriteCaptureWav(std::vector<int16_t> samples, uint32_t rate, std::string path) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    REXAPU_WARN("audio-capture: cannot open {}", path);
    return;
  }
  const uint32_t data_bytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
  const auto put32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
  const auto put16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
  out.write("RIFF", 4);
  put32(36 + data_bytes);
  out.write("WAVEfmt ", 8);
  put32(16);
  put16(1);
  put16(2);
  put32(rate);
  put32(rate * 4);
  put16(4);
  put16(16);
  out.write("data", 4);
  put32(data_bytes);
  out.write(reinterpret_cast<const char*>(samples.data()), data_bytes);
  REXAPU_INFO("audio-capture: wrote {:.1f} s to {}", double(samples.size()) / 2.0 / rate, path);
}

}  // namespace
REXCVAR_DEFINE_BOOL(audio_low_latency, false, "Audio",
                    "Android: open AAudio in low-latency (MMAP) mode. Its buffer is a couple of "
                    "2 ms bursts, so any scheduling hiccup becomes a gap; the normal mode buffers "
                    "tens of milliseconds")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::audio::sdl {

namespace {

struct SDLSubsystemResult {
  bool success = false;
  std::string error;
};

void SDLCALL InitializeAudioSubsystem(void* userdata) {
  auto* result = static_cast<SDLSubsystemResult*>(userdata);
  result->success = SDL_InitSubSystem(SDL_INIT_AUDIO);
  if (!result->success) {
    result->error = SDL_GetError();
  }
}

void SDLCALL ShutdownAudioSubsystem(void*) {
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

}  // namespace

SDLAudioDriver::SDLAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

SDLAudioDriver::~SDLAudioDriver() {
  assert_true(frames_queued_.empty());
  assert_true(frames_unused_.empty());
}

bool SDLAudioDriver::Initialize() {
  // Prevent SDL from interfering with timer resolution (causes FPS drops)
  SDL_SetHintWithPriority(SDL_HINT_TIMER_RESOLUTION, "0", SDL_HINT_OVERRIDE);

  // Set audio category for proper OS audio handling
  SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");
  SDL_SetHint(SDL_HINT_ANDROID_LOW_LATENCY_AUDIO, REXCVAR_GET(audio_low_latency) ? "1" : "0");

  // Set app name for audio device identification
  SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, "rexglue");

  SDLSubsystemResult subsystem_result;
  if (SDL_IsMainThread()) {
    InitializeAudioSubsystem(&subsystem_result);
  } else if (!SDL_RunOnMainThread(InitializeAudioSubsystem, &subsystem_result, true)) {
    REXAPU_ERROR(
        "SDL_RunOnMainThread() for audio initialization failed: {}; using paced silent "
        "fallback",
        SDL_GetError());
    silent_fallback_ = true;
    return true;
  }
  if (!subsystem_result.success) {
    REXAPU_ERROR("SDL_InitSubSystem(SDL_INIT_AUDIO) failed: {}; using paced silent fallback",
                 subsystem_result.error);
    silent_fallback_ = true;
    return true;
  }
  sdl_initialized_ = true;

  SDL_AudioSpec desired_spec = {};
  SDL_AudioSpec obtained_spec = {};
  SDL_AudioSpec preferred_spec = {};
  desired_spec.freq = frame_frequency_;
  desired_spec.format = SDL_AUDIO_F32LE;
  if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &preferred_spec, nullptr)) {
    REXAPU_INFO("Default playback device '{}' prefers {} Hz, {} channels, format {}",
                SDL_GetAudioDeviceName(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK), preferred_spec.freq,
                preferred_spec.channels, uint32_t(preferred_spec.format));
  } else {
    REXAPU_WARN("SDL_GetAudioDeviceFormat() before open failed: {}", SDL_GetError());
  }

  // The callback can provide either the guest's six channels or its existing
  // stereo downmix. Select stereo before opening stereo-only devices so
  // CoreAudio never has to start an unsupported six-channel queue merely to
  // discover the physical layout.
  const bool prefer_stereo = preferred_spec.channels > 0 && preferred_spec.channels < 6;
  desired_spec.channels = prefer_stereo ? 2 : frame_channels_;
  sdl_device_channels_ = desired_spec.channels;
  sdl_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired_spec,
                                          SDLCallback, this);
  if (!sdl_stream_) {
    if (desired_spec.channels != 2) {
      REXAPU_WARN("SDL_OpenAudioDeviceStream() for {} channels failed: {}; retrying stereo",
                  desired_spec.channels, SDL_GetError());
      desired_spec.channels = 2;
      sdl_device_channels_ = 2;
      sdl_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired_spec,
                                              SDLCallback, this);
    }
    if (!sdl_stream_) {
      REXAPU_ERROR(
          "SDL_OpenAudioDeviceStream() stereo fallback failed: {}; using paced silent "
          "fallback",
          SDL_GetError());
      silent_fallback_ = true;
      return true;
    }
  }

  SDL_AudioDeviceID sdl_device = SDL_GetAudioStreamDevice(sdl_stream_);
  if (!sdl_device) {
    REXAPU_ERROR("SDL_GetAudioStreamDevice() failed: {}; using paced silent fallback",
                 SDL_GetError());
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
    silent_fallback_ = true;
    return true;
  }

  if (!SDL_GetAudioDeviceFormat(sdl_device, &obtained_spec, NULL)) {
    REXAPU_WARN("SDL_GetAudioDeviceFormat() failed: {}", SDL_GetError());
    obtained_spec = desired_spec;
  }

  REXAPU_INFO(
      "Opened playback stream with {} input channels; device is {} Hz, {} channels, "
      "format {}",
      desired_spec.channels, obtained_spec.freq, obtained_spec.channels,
      uint32_t(obtained_spec.format));

  if (!SDL_ResumeAudioStreamDevice(sdl_stream_)) {
    REXAPU_ERROR("SDL_ResumeAudioStreamDevice() failed: {}; using paced silent fallback",
                 SDL_GetError());
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
    silent_fallback_ = true;
    return true;
  }

  return true;
}

void SDLAudioDriver::SubmitFrame(uint32_t frame_ptr) {
  if (silent_fallback_) {
    rex::thread::Sleep(std::chrono::microseconds(silent_frame_duration_microseconds_));
    const bool released = semaphore_->Release(1, nullptr);
    assert_true(released);
    return;
  }

  const auto input_frame = memory_->TranslateVirtual<float*>(frame_ptr);
  float* output_frame;
  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    if (frames_unused_.empty()) {
      output_frame = new float[frame_samples_];
    } else {
      output_frame = frames_unused_.top();
      frames_unused_.pop();
    }
  }

  std::memcpy(output_frame, input_frame, frame_samples_ * sizeof(float));

  // Is the block the game hands over digital silence? Runs of silent blocks that the game itself
  // produced (an audio thread or decoder that fell behind) sound like dropouts yet never show up
  // as output underruns, so they are counted here: separate from the device side below.
  bool block_silent = true;
  {
    const auto* words = reinterpret_cast<const uint32_t*>(output_frame);
    uint32_t any = 0;
    for (size_t i = 0; i < frame_samples_; ++i) any |= words[i];
    block_silent = any == 0;
  }

  static uint32_t sdl_submit_count = 0;
  if (sdl_submit_count < 10) {
    REXAPU_DEBUG("SDLAudioDriver::SubmitFrame: frame_ptr={:08X} queued_count={}", frame_ptr,
                 frames_queued_.size() + 1);
    sdl_submit_count++;
  }

  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    frames_queued_.push(output_frame);
    PROFILE_BUFFER_QUEUE_DEPTH(static_cast<int64_t>(frames_queued_.size()));
    // Longest wait between two guest frames in this report period: a producer that stalls for
    // longer than the queue holds is what turns into a dropout.
    const auto now = std::chrono::steady_clock::now();
    if (last_submit_.time_since_epoch().count() != 0) {
      const double gap = std::chrono::duration<double, std::milli>(now - last_submit_).count();
      if (gap > max_submit_gap_ms_) max_submit_gap_ms_ = gap;
      ++submitted_blocks_;
    }
    last_submit_ = now;
    if (block_silent) {
      ++guest_silent_blocks_;
      ++guest_silent_run_;
      if (guest_silent_run_ > guest_silent_longest_) guest_silent_longest_ = guest_silent_run_;
    } else {
      if (guest_silent_run_ >= 2) ++guest_silent_runs_;
      guest_silent_run_ = 0;
    }
  }
}

void SDLAudioDriver::CaptureBlock(const float* interleaved, uint32_t channels) {
#if defined(__ANDROID__)
  if (capture_state_ == 2 || channels == 0) return;
  const auto now = std::chrono::steady_clock::now();
  if (capture_state_ == 0) {
    const int seconds = std::min(REXCVAR_GET(audio_capture_seconds), 120);
    const char* live = std::getenv("LIBERTY_LIVE_LOG");
    if (seconds <= 0 || !live || !*live) {
      capture_state_ = 2;
      return;
    }
    capture_path_ = std::string(live);
    const size_t slash = capture_path_.find_last_of('/');
    capture_path_ = (slash == std::string::npos ? std::string() : capture_path_.substr(0, slash + 1)) +
                    "audio-capture.wav";
    capture_started_ = now + std::chrono::seconds(25);
    capture_.reserve(size_t(seconds) * frame_frequency_ * 2);
    capture_seconds_ = seconds;
    capture_state_ = 1;
  }
  if (now < capture_started_) return;
  for (uint32_t i = 0; i < channel_samples_; ++i) {
    for (uint32_t c = 0; c < 2; ++c) {
      const float v = std::clamp(interleaved[i * channels + std::min<uint32_t>(c, channels - 1)], -1.0f, 1.0f);
      capture_.push_back(static_cast<int16_t>(v * 32767.0f));
    }
  }
  if (capture_.size() >= size_t(capture_seconds_) * frame_frequency_ * 2) {
    capture_state_ = 2;
    std::thread(WriteCaptureWav, std::move(capture_), uint32_t(frame_frequency_), capture_path_).detach();
  }
#else
  (void)interleaved;
  (void)channels;
#endif
}

void SDLAudioDriver::Pause() {
  if (!sdl_stream_) {
    FlushQueuedFrames();
    return;
  }
  if (!SDL_PauseAudioStreamDevice(sdl_stream_)) {
    REXAPU_WARN("SDL_PauseAudioStreamDevice() failed: {}", SDL_GetError());
  }
  Flush();
}

void SDLAudioDriver::Resume() {
  if (sdl_stream_ && !SDL_ResumeAudioStreamDevice(sdl_stream_)) {
    REXAPU_WARN("SDL_ResumeAudioStreamDevice() failed: {}", SDL_GetError());
  }
}

void SDLAudioDriver::Flush() {
  if (!sdl_stream_) {
    FlushQueuedFrames();
    return;
  }
  if (!SDL_LockAudioStream(sdl_stream_)) {
    REXAPU_WARN("SDL_LockAudioStream() failed while flushing: {}", SDL_GetError());
    return;
  }
  FlushQueuedFrames();
  if (!SDL_ClearAudioStream(sdl_stream_)) {
    REXAPU_WARN("SDL_ClearAudioStream() failed: {}", SDL_GetError());
  }
  SDL_UnlockAudioStream(sdl_stream_);
}

void SDLAudioDriver::FlushQueuedFrames() {
  std::unique_lock<std::mutex> guard(frames_mutex_);
  while (!frames_queued_.empty()) {
    frames_unused_.push(frames_queued_.front());
    frames_queued_.pop();
  }
}

void SDLAudioDriver::Shutdown() {
  silent_fallback_ = false;
  if (sdl_stream_) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
  }
  if (sdl_initialized_) {
    if (SDL_IsMainThread()) {
      ShutdownAudioSubsystem(nullptr);
    } else if (!SDL_RunOnMainThread(ShutdownAudioSubsystem, nullptr, true)) {
      REXAPU_ERROR("SDL_RunOnMainThread() for audio shutdown failed: {}", SDL_GetError());
    }
    sdl_initialized_ = false;
  }
  std::unique_lock<std::mutex> guard(frames_mutex_);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  }
  while (!frames_queued_.empty()) {
    delete[] frames_queued_.front();
    frames_queued_.pop();
  }
}

void SDLAudioDriver::SDLCallback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                                 [[maybe_unused]] int total_amount) {
  SCOPE_profile_cpu_f("apu");
  if (!userdata || !stream) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback called with nullptr.");
    return;
  }
  const auto driver = static_cast<SDLAudioDriver*>(userdata);
  const int sample_count =
      static_cast<int>(channel_samples_ * std::max<uint8_t>(driver->sdl_device_channels_, 1));
  const int len = static_cast<int>(sizeof(float) * sample_count);
  float* data = SDL_stack_alloc(float, sample_count);
  if (!data) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback failed to allocate {} samples", sample_count);
    return;
  }
  // Gaps (no guest frame ready, silence played) counted and logged every
  // 10 s with the queue depth, so stutter reports can tell a starved guest
  // producer from an output-side problem.
  static uint32_t silent_fills = 0;
  static uint32_t filled = 0;
  static auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (additional_amount > 0) {
    static uint32_t sdl_callback_count = 0;
    std::unique_lock<std::mutex> guard(driver->frames_mutex_);
    if (const auto now = std::chrono::steady_clock::now(); now >= next_report) {
      if (silent_fills) {
        REXAPU_WARN("audio: {} of {} blocks were silence (no guest frame ready), {} queued",
                    silent_fills, silent_fills + filled, driver->frames_queued_.size());
      }
      // Every period, so a clean session shows its numbers too: the queue seen at the start of a
      // callback (lowest and now), and the longest gap between guest frames.
      REXAPU_INFO(
          "audio-stats: played={} silence={} queue-min={} queue-now={} guest-gap-max={:.0f}ms "
          "guest-silent={} silent-runs={} longest-run={}",
          filled, silent_fills, driver->min_queue_seen_, driver->frames_queued_.size(),
          driver->max_submit_gap_ms_, driver->guest_silent_blocks_, driver->guest_silent_runs_,
          driver->guest_silent_longest_);
      driver->guest_silent_blocks_ = driver->guest_silent_runs_ = driver->guest_silent_longest_ = 0;
      driver->min_queue_seen_ = SIZE_MAX;
      driver->max_submit_gap_ms_ = 0.0;
      silent_fills = filled = 0;
      next_report = now + std::chrono::seconds(10);
    }
    driver->min_queue_seen_ = std::min(driver->min_queue_seen_, driver->frames_queued_.size());
    if (driver->frames_queued_.empty()) {
      ++silent_fills;
      if (sdl_callback_count < 10) {
        REXAPU_DEBUG("SDLCallback: no frames queued (silence)");
        sdl_callback_count++;
      }
      std::memset(data, 0, len);
      driver->CaptureBlock(data, driver->sdl_device_channels_);
      if (!SDL_PutAudioStreamData(stream, data, len)) {
        REXAPU_ERROR("SDL_PutAudioStreamData() failed while filling silence: {}", SDL_GetError());
        break;
      }
      additional_amount -= len;
    } else {
      ++filled;
      auto buffer = driver->frames_queued_.front();
      driver->frames_queued_.pop();
      if (REXCVAR_GET(audio_mute)) {
        std::memset(data, 0, len);
      } else {
        switch (driver->sdl_device_channels_) {
          case 2:
            conversion::sequential_6_BE_to_interleaved_2_LE(data, buffer, channel_samples_);
            break;
          case 6:
            conversion::sequential_6_BE_to_interleaved_6_LE(data, buffer, channel_samples_);
            break;
          default:
            assert_unhandled_case(driver->sdl_device_channels_);
            break;
        }
      }
      driver->CaptureBlock(data, driver->sdl_device_channels_);
      if (!SDL_PutAudioStreamData(stream, data, len)) {
        REXAPU_ERROR("SDL_PutAudioStreamData() failed: {}", SDL_GetError());
        driver->frames_unused_.push(buffer);
        break;
      }
      driver->frames_unused_.push(buffer);

      auto ret = driver->semaphore_->Release(1, nullptr);
      assert_true(ret);
      additional_amount -= len;
    }
  }
  SDL_stack_free(data);
}

}  // namespace rex::audio::sdl
