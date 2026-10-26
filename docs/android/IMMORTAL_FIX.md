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
