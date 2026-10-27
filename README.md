<p align="center">
    <img src="docs/android/images/banner_android_mali_edition.png" alt="Liberty Immortal" width="800"/>
</p>

# Liberty Immortal

**GTA IV on Android, optimized for a stable 30 FPS on Immortalis GPUs.**

Liberty Immortal is a performance-focused fork of the Mali Edition of
[LibertyRecompAndroid](https://github.com/vaduur/LibertyRecompAndroid). It runs the static
recompilation of the Xbox 360 version of Grand Theft Auto IV
([Liberty Recompiled](https://github.com/OZORDI/LibertyRecomp)) natively on arm64 Android, on the
phone's own Mali / Immortalis Vulkan driver. There is no emulator in between.

The goal of this fork is simple: a locked, even 30 FPS instead of a number that jumps around, plus
the controls and tools that make the game comfortable on a touch screen.

> [!WARNING]
> Experimental. Expect rough edges and save often. Test builds are published as
> **pre-releases** on the [Releases](../../releases) page (`immortal-fix-N`).

## Why it feels smoother

The game is limited by the CPU, not the GPU, so this fork spends its effort on keeping frames
**even**:

- **Locked 30 FPS with a matching screen mode.** The frame limit is sent to Android so the panel
  runs at a whole multiple of it (30 on 60 Hz, 40 on 120 Hz). A free-running frame rate on a
  fixed-refresh screen is what makes a game look like it skips a frame every few frames.
- **Tuned profile for Mali-G1 / Immortalis class chips**, `os/android/android_args/mali_immortal.txt`:
  skips tiny shadow and reflection draws, caps reflection work, keeps the big cores clocked up and
  turns motion blur off. Why each value was chosen is in
  [IMMORTAL_FIX.md](docs/android/IMMORTAL_FIX.md).
- **Heat guard.** Lowers the draw distance a little as the phone heats up and restores it when it
  cools, so the clocks stay high instead of throttling in the middle of a drive.
- **Sharper picture.** The base resolution now follows the output setting (it used to render 720p
  and stretch it), with SMAA to clean up edges.
- **Detailed logs, cheap to record.** *Play (Debug Log)* copies the log to
  `Downloads/LibertyRecomp` as you play: frame-time percentiles every 2 seconds, thread load,
  temperature and thermal headroom every 5. See what to look for in
  [IMMORTAL_FIX.md](docs/android/IMMORTAL_FIX.md#reading-the-new-logs-play-debug-log).

## Touch controls built for GTA

- **Swipe to look.** Drag a finger on empty screen to turn the camera, like the GTA mobile ports.
  A **camera speed** slider lives in layout edit mode (hold **Pause** for 1.5 s).
- **Layouts that follow what you do:** on foot, driving, phone, pause menu and cutscenes. Drag to
  move a button, pinch to resize, **Reset** restores the default.
- **Menu controls when the game has none on screen,** such as the episode picker.
- **Switch any time** to a classic on-screen Xbox controller with the gamepad icon.

## Instant cheats

The **CH** button opens a panel of cheats that apply on the spot, with no phone and no keypad: they
call the game's own script functions. 28 buttons:

- **Base game:** health and armor, ammo, weapon sets, wanted level, weather, and vehicles
  (Annihilator, FIB Buffalo, Cognoscenti, Turismo, Super GT, Comet, NRG-900, Sanchez, Jetmax).
- **The Lost and Damned:** Innovation, Hexer, Double T Custom, Hakuchou Custom, Slamvan, Gang Burrito.
- **The Ballad of Gay Tony:** Bullet GT, APC, Buzzard, Akuma, Vader, Floater, and Parachute,
  Explosive punches, Explosive sniper and the episode weapon set (these four dial the phone code
  for you).

The list lives in an editable file, `liberty_cheats.txt`. Details and phone codes:
[CHEATS.md](docs/android/CHEATS.md). Single player only; some cheats switch off achievements.

## Requirements

| | |
|---|---|
| **GPU** | ARM Mali / Immortalis with Vulkan 1.2 and `bufferDeviceAddress`. Tuned for Immortalis-class parts (Mali-G720 and the newer Mali-G1 series). Older Mali GPUs without 64-bit shader integers work through a fallback. |
| **CPU** | arm64. A recent flagship-class chip is recommended, since the game is CPU bound. |
| **Android** | 10 or newer, 64-bit. |
| **RAM and storage** | 8 GB RAM recommended. About 7 GB for the installed game and about 15 GB free during install. |
| **Controls** | Touch works out of the box. An XInput (Xbox layout) gamepad also works. |

Snapdragon (Adreno) phones should use the [original project](https://github.com/vaduur/LibertyRecompAndroid).

## Game files

This project contains no game assets. You need your own legally obtained copy of the **Xbox 360
GTA IV disc** (`.iso`, USA / NTSC-U) and **Title Update 8** (version 0.0.8.5). The episodes are
optional and go in `Android/data/com.libertyrecomp/files/LibertyRecomp/dlc/`.

The tested files:

| File | MD5 |
|---|---|
| `Grand Theft Auto IV (USA) (En,Fr,De,Es,It).iso` | `f0a046aed1520a913b2125f0a69ee7d0` |
| `TU_1A581VI_000000K000000.0000000000205` (Title Update 8) | `f041b5f6721d7ee578560bac21668826` |

## Quick start

1. Install the latest APK from [Releases](../../releases) and open it. Builds are signed with the
   same key as earlier Mali Edition builds, so they install over them as an update.
2. In the launcher's **Game** panel pick your disc image and the Title Update 8 file. The app asks
   once for *All files access*. (Or copy both into `Android/data/com.libertyrecomp/files/install/`.)
3. Press **Install**, then **Play**.
4. For the tuned profile, copy `os/android/android_args/mali_immortal.txt` to
   `Android/data/com.libertyrecomp/files/args.txt`, then set **Frame limit 30**, **Motion blur off**
   and **CPU boost: Steady or Maximum** in the launcher's Graphics & controls. Close the app fully
   before launching. The full list of settings is in
   [IMMORTAL_FIX.md](docs/android/IMMORTAL_FIX.md#apply-it).

The complete walkthrough is in [INSTALL.md](docs/android/INSTALL.md).

## Getting the best out of it

- Start the session cool, avoid charging while playing, and use the phone's game mode.
- Lower **Population density** and **Draw distance** first. Resolution and FSR cost the GPU,
  which is not the bottleneck, and FSR can look worse than native.
- Keep the shadow distance at 1.0. The engine raises it back, and shadows sink into the ground
  below that.
- Send a *Play (Debug Log)* session if something stutters. The log shows which of CPU, heat,
  loading or the screen mode is responsible.

## Known issues

- A steady 60 FPS is not realistic with today's code. The goal is an even 30 (or 40 on a 120 Hz screen).
- Busy city areas, the first drive through an area (shader and texture work) and long hot sessions
  are where frames get uneven.
- Some reflections or geometry may look wrong (Mali has no clip distances), and memory use is
  higher than on Adreno (decoded textures).
- Occasional crashes or freezes can still happen. **Save often**, and report them with a log.

## Reporting problems

Open an [Issue](../../issues) and attach `Android/data/com.libertyrecomp/files/last_launch.txt`
and the two files a *Play (Debug Log)* run leaves in `Downloads/LibertyRecomp`
(`debug-*.log` and `runtime-debug-*.log`). Say which GPU you have.

## Documentation

| Document | Contents |
|---|---|
| [Immortal-Fix notes](docs/android/IMMORTAL_FIX.md) | What the tuning does, how to apply it, how to read the logs |
| [Cheats](docs/android/CHEATS.md) | Every cheat button, phone code and the editable file |
| [Installation](docs/android/INSTALL.md) | Installing the APK, game files, first run, upgrading |
| [Advanced configuration](docs/android/ADVANCED.md) | `args.txt` settings, driver selection, logs |
| [Mali notes](docs/android/MALI.md) | Mali specific compatibility work |
| [Porting write-up](docs/android/PORTING.md) | Technical notes on the Android port |
| [Building from source](docs/android/BUILDING.md) | Building the APK yourself |

## Credits

- [Liberty Recompiled](https://github.com/OZORDI/LibertyRecomp) by OZORDI and contributors, and the
  [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) it is built on. Nearly all of the hard work
  of recompiling GTA IV is theirs.
- [LibertyRecompAndroid](https://github.com/vaduur/LibertyRecompAndroid) by vaduur, the Android
  port this fork builds on, and the Mali Edition by
  [prollynotzjay](https://github.com/prollynotzjay/LibertyRecompAndroidMaliVer), which made it run
  on Mali GPUs.
- [libadrenotools](https://github.com/bylaws/libadrenotools), [SDL3](https://github.com/libsdl-org/SDL),
  and the Vulkan driver proxy from [skate3-android](https://github.com/andrewnakas/skate3-android)
  and [skate3-pocket](https://github.com/AlanConstantino/skate3-pocket).
- [nfsmw-nx](https://github.com/StevensND/nfsmw-nx) by StevensND, whose work on busy-waits and
  thread placement informed the frame-wait hook.

Grand Theft Auto IV is a trademark of Take-Two Interactive Software. This project is not
affiliated with or endorsed by Rockstar Games or Take-Two Interactive.
