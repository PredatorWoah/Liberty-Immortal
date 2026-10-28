# Immortal-Fix: stutter and frame rate on Mali-G1 / Immortalis phones

Written for a vivo V2514 (Dimensity MT6993, Mali-G1-Ultra MC12, Android 17) running
`0.5.9.3-mali-alpha-1.5`. Nothing here was built or run on a device: it is a tuning profile plus
notes from reading the code. Treat it as a starting point and measure.

## What the settings you sent were doing

| Setting | Yours | Problem | Use |
|---|---|---|---|
| `gta4_frame_limit` | `60` | The game is CPU bound at roughly 25-40 FPS. A 60 limit on a vsynced panel makes frames land on 16 / 33 / 50 ms boundaries, which is exactly what stutter feels like. | `40` on a 120 Hz panel (3 vsyncs per frame, even), otherwise `30` |
| `gta4_motion_blur` | `true` | A full-screen composite per frame, and it makes frame times less even. | `false` |
| `android_performance_hint_target_ms` | `25` | Lets the big cores clock down between busy moments, then ramp back up. | `20` |
| `gta4_dynamic_draw_distance_min` | `0.80` | With `draw_distance_scale=0.8` the controller had almost no room to react. | `0.75` (do not go below 0.7, it breaks dynamic shadows) |
| `gta4_fps_guard` | `2` | Fine, but levels 1-3 behave the same today. | `3` |

Already correct in your files: 720p output cap, 0.4 population densities,
`native_max_queued_frames=3`, profiling scopes off, thermal guard on.

## What your gameplay log showed

`Liberty_Recompiled_004.log` (4 minutes of free roam, 2400x1080 panel, 1280x576 render):

- **About 2,900 draws per frame** (median 2,975, peak 4,864). The README measured 1,400-1,700 for
  the profile that reaches 30 FPS on an older chip. This is the CPU cost behind the stutter.
- **More than half of them are the sun shadow pass** (the 1280x1280 depth target: 1,400-1,700
  draws). Shadow range cannot be lowered (the engine raises it back to 1.0), so the profile skips
  tiny shadow draws instead with `gta4_native_shadow_min_indices=120`.
- **Frame pacing:** "one frame dropped every few frames" is what a CPU bound game looks like when
  the cap and the screen refresh rate do not divide evenly (for example a 40 cap on a 60 Hz
  screen, or a 60 cap the game cannot reach). The app now asks Android for a screen mode that is
  a whole multiple of the frame limit (30 -> 60 Hz, 40 -> 120 Hz) and logs
  `refresh rate: ... Hz for a ... FPS limit` to logcat. Use a **frame limit of 30** until the
  overlay shows the game holding well above 30.

## What not to change

- **`gta4_shadow_distance_scale` below 1.0.** The engine raises it back to 1.0 and vehicle and
  pedestrian shadows sink into the ground.
- **`gta4_thread_pinning`.** It could help on an 8 core chip, but it is off by default because
  changing the game's thread timing provokes a known race crash (PORTING.md, Stability). Try it
  only if you accept crashes, and never as a first fix.

## Apply it

1. Copy `os/android/android_args/mali_immortal.txt` over
   `Android/data/com.libertyrecomp/files/args.txt`.
2. In the launcher, Graphics & controls: set **Frame limit** to 40 (or 30), **Motion blur** off,
   **CPU boost** to Steady or Maximum. Save changes, then Play. These land in `graphics.txt`, which
   overrides `args.txt`, so the old `frame_limit=60` and `motion_blur=true` lines must go.
3. Fully close the app first (swipe it from recents) so the new args are read.

## Expectations

The README measured 20-30 FPS in the city on a Dimensity 8400, with the CPU as the limit, not the
GPU. A faster chip helps, but the recompiled game code and the render thread are single thread
heavy, so a steady 60 is not realistic with today's code. A locked 30, or 40 on a 120 Hz panel, in
the city is the achievable target. Anything these settings cannot fix needs profiling.

## Stutter sources that settings cannot fix

- **First time a pipeline is seen.** A pipeline missing from the cache compiles on the render
  thread (`graphics_system.cpp`, pipeline wait path). The cache is saved to disk and replayed at
  start, so stutter in a new area on the first drive-through is normal and fades on later visits.
  Do not clear the app data between tests.
- **DXT (BC) texture decode.** Mali has no BC support, so textures are decoded on the CPU as they
  stream in. Fast driving through unseen areas stutters from this.
- **Heat.** If the stutter starts after 10-15 minutes, it is throttling. Avoid charging while
  playing and turn on the game mode of the phone.

## Getting data that can actually be acted on

The debug log you sent is 31 lines of Android process start and ends before the game runs, so it
shows nothing about frame times. To capture something useful:

1. Launcher, Overlay card: turn on **FPS, frame time, CPU, GPU, temperature**.
2. Play 5 minutes in the same spot, for example driving across the bridge, and note the FPS and
   temperature at the start and end.
3. Use **Play (Debug Log)** (it sets `--diagnostics=true` and `--log_level=info`), then send the
   whole file from `files/debug_logs/` after the stutter happens.
4. Add `--gta4_pipeline_stats=true` to `args.txt` for one run to see pipeline cache misses.

## Reading the new logs (Play (Debug Log))

Copies land in `Downloads/LibertyRecomp/`: `debug-DATE.log` (the whole process log) and
`runtime-debug-DATE.log` (the game's own log). Low overhead lines to look for:

- `frame-times:` every 2 s. `p50/p95/p99/max` in ms, how many frames were slower than 1.5x the
  median, and `recent=[...]`, the last 24 frame times in order. `16 33 16 33` is a rhythm problem
  (refresh rate and cap do not match); isolated large numbers are real hitches (loading, shader
  compile, texture decode).
- `LibertyPerf: perf:` every 5 s. Screen refresh rate, thermal status and headroom, battery
  temperature, memory, and `threads=[name:percent ...]`: which threads are saturated (a thread
  near 100% is the bottleneck).
- `pipeline: 120 frames ...` handoff and sync waits, `gta4-draw-census` draw counts per render
  target, `io:` read rates, `thermal:` and `power:` lines.

### Lines written without logcat

On some phones logcat stops delivering the game's lines partway through a session. These lines
are written straight into the same file, so they always arrive:

- `LibertyNative: frame-times: ...` and `title-fps: ...` from the game process.
- `LibertyNative: thermal-guard: ...` when the heat guard lowers or restores the draw distance.
- `LibertyApp: session:`, `settings:` (the `--gta4_*` and `--android_*` launch settings actually
  in effect, so the frame limit is visible) and `refresh rate:` (what screen mode was requested).
- `LibertyPerf:` every 5 s, and `LibertyPoll:` only when the logcat copy has a problem.

## Performance extras in build 6

- The frame limit is also sent to Android as a fixed content frame rate, so it can pick a matching
  refresh rate and save power (less heat). It is logged as `frame rate hint:`.
- The launcher's Thermal guard now defaults to on and Other reflections to every second frame
  (only for a launcher that has not saved those settings yet; tick them on the Performance card
  otherwise).
- `gta4_native_water_reflection_min_indices=90` in the profile skips the tiniest draws in the
  water and mirror reflection captures.

## Swipe camera

On foot and driving, drag one finger on empty screen (right of the move area) to turn the camera,
like the GTA mobile ports. The right camera stick is hidden in this mode.

Build 7 turned the finger's speed into stick deflection each frame, which failed in two ways: a quick
flick saturated the stick for only a few frames (about half of the travel was thrown away), and a slow
drag produced values below the game's own stick dead zone, so it turned nothing. Swiping several
times was the workaround. Now the finger's travel is kept as a budget and delivered as stick
deflection that is either zero or at least 0.30 (above the game's dead zone): small travel adds up
to one minimum step, and large travel that one frame cannot deliver carries on over the next frames,
also after the finger lifts. A simulation delivers 99-100% of the travel for a slow drag, a normal
drag, a fast flick, a long swipe and a tiny nudge.

**Camera speed slider:** hold PAUSE for 1.5 s (layout edit mode) and drag the CAMERA SPEED slider
under the title (0.3x to 3x). Tap PAUSE to save. It is stored in `liberty_swipe.txt` in the app's
user folder. Without a saved value it starts from `--gta4_touch_swipe_sensitivity` (default 1.0).
`--gta4_touch_camera=stick` brings the on-screen camera stick back.

## Heat and stability (28 minute log)

A 28 minute session held 30.1 FPS the whole way (median frame 33.3 ms), but the phone went from
thermal status 0 to 3 (severe) after 14 minutes and 4 (critical) at the end, battery 44 C. The old
guard cut the draw distance once (0.80 -> 0.56 at headroom 0.95) and could do nothing more while
headroom kept rising to 1.07. The guard is now staged, starts earlier, and steps back one stage at a
time as the phone cools:

| Stage | Starts at headroom | Adds |
|---|---|---|
| 1 | 0.88 | draw distance x0.85 |
| 2 | 0.95 | draw distance x0.70 |
| 3 | 1.00 | shadow map redrawn every other frame |
| 4 | 1.05 | draw distance x0.55, environment reflections every 3rd frame, water every other frame |

It steps down at 0.98, 0.92, 0.85 and 0.78. Every change is logged as `thermal-guard: stage a -> b`.
The first 10 minutes also had more hitches (21-29% of 2 s windows against 6-19% later): streaming and
shader warm-up, and the draw distance cut after 18:41 lightened the frames.

## Build 11: smoother swipe, lighter heat guard

A 14 minute session at thermal status 3 held a locked 30 FPS (p50 33.3 ms) and then the game froze
at 20:47:45 (the main guest thread at 92-96% CPU, no more frames; the phone was not slowing down
before it). The stage 3 step (shadow map every other frame) had been active for two minutes and
the draw distance was at 0.45, below the 0.7 the docs warn about. The cause is not proven, so the
guard now only trims draw distance (x0.90, then x0.80 of the saved value) and no longer flips the
shadow or reflection intervals at run time.

Swipe: build 10 delivered slow drags as bursts of at least 0.30 stick, which felt like flicks, and
raising the speed only made the bursts bigger. The output now eases towards its target (60 ms), fast
finger speed is compressed (share kept = 1 / (1 + 0.35 x screen heights per second)), and at most
0.35 s of full stick is queued, so a flick no longer carries on.

## City stutter (log of 17 minutes in the city)

Frame pacing was fine in quiet areas and bad in the dense ones. Per minute, against the average number
of draws a frame:

| Draws a frame | Windows with a frame over 50 ms |
|---|---|
| about 1,200 | 1 of 29 |
| about 2,500 | 16 of 31 |
| 3,000 to 3,900 | 9 to 18 of 30 |

So the CPU runs out above roughly 3,000 draws: a frame that misses its 33 ms slot shows up as 66 ms.
In those minutes the 1280x1280 shadow pass is 1,100 to 2,000 draws and the main pass about 2,000.
Cutting draws is the fix, not the heat guard (thermal status stayed at 0 for 13 minutes):

- `--gta4_native_shadow_min_indices=250` skips more of the tiny shadow draws.
- Population densities 0.5 to 0.55 in the launcher (the launcher's value wins over `args.txt`).
- Draw distance 0.5 to 0.55 in the launcher.

Also: one texture failed to bind on every frame for four minutes (`required-binding-failed`,
27,000 identical error lines written on the render thread). The log now keeps the first 24 and every
600th.

## Audio statistics

`audio-stats:` lines now appear in the runtime log every 10 seconds: blocks played, blocks of silence,
the lowest and current queue depth (the queue holds up to `audio_maxqframes` blocks of about 5.3 ms)
and the longest gap between two blocks from the game. A gap near or above the queue's length means the
game's audio thread stalled and the sound drops out; a clean queue with a choppy sound points at the
decoded audio itself.

## Profile applied by the app (build 19)

The first launch of this build writes these values into `args.txt` and into the launcher's
`graphics.txt` (which wins over `args.txt`): draw distance 0.5, all four population densities 0.5,
`gta4_native_shadow_min_indices=250` and `audio_maxqframes=16`. A marker file `.immortal_profile_2`
keeps it from running again, so anything you change in the launcher afterwards stays.

The `audio-stats:` line also reports `guest-silent` (blocks the game itself handed over as digital
silence), `silent-runs` (runs of two or more in a row) and `longest-run`. Alternating dropouts that are
not output underruns (queue-min stays above 0) show up there if the game's own audio is the one with gaps.

## Audio capture (build 22)

A debug log session now also records 40 seconds of the final stereo output (what is handed to the
device) as a 16-bit WAV, starting 25 seconds after the audio starts, and copies it to
`Downloads/LibertyRecomp/audio-debug-DATE.wav` (a phone may add `.txt`). It is written once, from memory,
after the 40 seconds. `--audio_capture_seconds=0` turns it off. The session line of the debug log now
also shows `build=N` (20 plus the CI run number), so the build a log came from can be told at a glance.

## Audio: what the 40 s recording showed (build 42)

The WAV is clean: no zero-length gaps, no clicks at block edges, no repeated blocks, no 15 or 30 Hz gating,
a smooth level curve. What the game hands to the audio device is intact, so the choppy sound happens on
the device side. Build 23 adds an `audio-device:` line (callbacks, late callbacks over 40 ms, longest
gap, biggest request) and `--audio_device_frames=2048` (or 4096) to try a bigger device buffer.

The app now also writes `--audio_device_frames=2048` into `args.txt` once (marker `.immortal_audio_1`), leaving your
other values as they are. Try 4096 by editing the line if it is still choppy.
