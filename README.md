<p align="center">
    <img src="docs/android/images/banner_android_mali_edition.png" alt="Liberty Immortal" width="800"/>
</p>

# Liberty Immortal

**GTA IV on Android, optimized for a stable 30 FPS on Immortalis GPUs.**

Latest: **[0.8 beta](../../releases/tag/v0.8-beta)**. It is signed with the same key as every earlier build, so
it installs over them as an update.

Liberty Immortal is a performance-focused fork of the Mali Edition of
[LibertyRecompAndroid](https://github.com/vaduur/LibertyRecompAndroid). It runs the static
recompilation of the Xbox 360 version of Grand Theft Auto IV, with *The Lost and Damned* and
*The Ballad of Gay Tony* ([Liberty Recompiled](https://github.com/OZORDI/LibertyRecomp)), natively on arm64
Android, on the phone's own Mali / Immortalis Vulkan driver. There is no emulator in between.

The goal of this fork is simple: a locked, even 30 FPS instead of a number that jumps around, a world
that does not pop in right next to you, less memory and heat, and touch controls and tools that make the
game a real touch screen game instead of a gamepad drawn on glass.

> [!WARNING]
> Beta. Expect rough edges and save often. Builds between the named releases are published as
> **pre-releases** (`immortal-fix-N`) on the [Releases](../../releases) page.

## What this fork adds

### A steady 30 FPS

The game is limited by the CPU, not the GPU, so most of the work goes into keeping frames **even**:

- **Locked 30 FPS with a matching screen mode.** The frame limit is sent to Android so the panel runs at a
  whole multiple of it (30 on 60 Hz, 40 on 120 Hz). A free-running frame rate on a fixed-refresh screen is
  what makes a game look like it skips a frame every few frames.
- **Tuned profile for Mali-G1 / Immortalis class chips** (`os/android/android_args/mali_immortal.txt`).
  The app applies its values once on first launch, so nothing has to be copied by hand. Why each value was
  chosen is in [IMMORTAL_FIX.md](docs/android/IMMORTAL_FIX.md).
- **Shadow draw budget.** More than a third of the draws in a busy street are the sun shadow pass. When a
  frame has more draws than the budget (3,200), only the smallest shadow casters are skipped, slowly, so
  shadows stay steady and big casters (cars, buildings) are never touched.
- **A farther world without pop-in.** Draw distance 0.8 with a floor of 0.9 for the dynamic controller. The
  earlier multiplication of distance, controller and heat guard left only a third of the normal distance.
- **Heat guard.** As Android forecasts throttling, the guard shrinks the shadow draw budget in four stages
  (x0.95 to x0.80) instead of shortening the draw distance, so the CPU has less to do while the picture
  changes little. It restores one stage at a time as the phone cools.
- **Clean sound.** A bigger audio device buffer and a shorter game audio queue fixed the choppy audio that
  came from late audio callbacks.
- **Sharper picture.** The base resolution follows the output setting (it used to render 720p and stretch
  it), with SMAA to clean up edges.

### Less memory and bandwidth: ASTC textures

Mali cannot sample the Xbox's DXT formats, so the stock port decoded every texture to 16 or 32 bits a
texel. This fork transcodes them to ASTC 4x4 (8 bits a texel) block by block, without a slow encoder:

- **Opaque DXT1** (most of the world): half the memory, with an error of at most 2/255.
- **DXT3 / DXT5 with simple alpha** (cut-outs such as fences and leaves, short gradients): a quarter of the
  RGBA8 size. The alpha is fitted by least squares, cut-out alpha is exact, and a texture whose alpha is too
  detailed (alpha used as data) stays RGBA8 automatically.
- **DXN normal maps**: stored as ASTC luminance + alpha with 8-bit endpoints, half the memory of R8G8.
- The transcoders are tested against ARM's reference decoder (`os/android/tools/`). In a long session over
  20,000 textures were converted (the game streams them in and out, so that counts re-streams).

### Touch controls built for GTA

- **Swipe to look.** Drag a finger on empty screen to turn the camera, like the GTA mobile ports. Fast flicks
  are compressed so they do not whip the camera round, slow drags still register, and the camera turns
  faster while you walk or steer, to offset the game's own camera drifting back behind the player. A
  **camera speed** slider lives in layout edit mode (hold **Pause** for 1.5 s).
- **Steering that behaves.** The finger that landed last decides the side (hold left, tap right: right).
  Steering builds up while a button is held, so a tap steers a little and a hold goes to full lock, and a
  thumb resting on a button's edge does not flip to the other one. A finger slid from one button to the
  other follows it.
- **Fewer accidental presses.** Drive-by needs a finger down for 0.15 s (it sits right above the gas), and
  touches at the very edge of the screen (palm, bezel) are ignored.
- **Layouts that follow what you do:** on foot, driving, phone, pause menu and cutscenes. Drag to move a
  button, pinch to resize, **Reset** restores the default.
- **Menu controls when the game has none on screen,** such as the episode picker, and on the map and menu
  screens a **left stick** for the cursor, **LT / RT** zoom and a **Y** legend button.
- **Switch any time** to a classic on-screen Xbox controller with the gamepad icon.

### Episodes

- *The Lost and Damned* and *The Ballad of Gay Tony* go in the `dlc` folder and start from the episode
  picker, which now has touch controls.
- Episode vehicles only spawn inside their own episode, and boats are placed on water in front of you.

### Instant cheats

The **CH** button opens a panel of 32 cheats that apply on the spot, with no phone and no keypad: they call
the game's own script functions.

- **Base game:** health and armor, ammo, two weapon sets, wanted level up and down, weather, and vehicles
  (Annihilator, FIB Buffalo, Cognoscenti, Turismo, Super GT, Comet, NRG-900, Sanchez, Jetmax).
- **The Lost and Damned:** Innovation, Hexer, Double T Custom, Hakuchou Custom, Slamvan, Gang Burrito.
- **The Ballad of Gay Tony:** Bullet GT, APC, Buzzard, Akuma, Vader, Floater, and Parachute, Explosive
  punches, Explosive sniper and the advanced weapon set. These four dial the phone code the first time and
  the app learns the game's cheat number from it (`liberty_cheat_ids.txt`), so every press after that is
  instant.

The list lives in an editable file, `liberty_cheats.txt`. Details and phone codes:
[CHEATS.md](docs/android/CHEATS.md). Single player only; some cheats switch off achievements.

### Logs that find problems

*Play (Debug Log)* copies the logs to `Downloads/LibertyRecomp` as you play:

- frame-time percentiles every 2 seconds, thread load, battery temperature, thermal headroom and the heat
  guard's stage;
- the draw census, shadow budget and texture statistics (`draw-budget:`, `astc-dxt1:`, `astc-alpha:`,
  `astc-dxn:`, `texture-capture:`);
- a **CPU sampler** (`cpu-thread:`, `cpu-hot:`) that shows which threads and which code use the CPU, which
  is how heat gets measured and reduced;
- a **freeze watchdog** that dumps every thread's stack when the game stops drawing for 4 seconds;
- every finger down and up (`liberty-touch-finger:`), to track down ghost touches;
- a 40 second recording of the final audio output.

See what to look for in [IMMORTAL_FIX.md](docs/android/IMMORTAL_FIX.md#reading-the-new-logs-play-debug-log).

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
4. Nothing else to set up: the app applies the tuned profile once on first launch. Set **Frame limit 30**,
   **Motion blur off** and **CPU boost: Steady** in the launcher's Graphics & controls if they are not already.
   The full list of settings is in [IMMORTAL_FIX.md](docs/android/IMMORTAL_FIX.md#apply-it).

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
- ASTC textures are a quality trade: the fits are tested, but report any texture that looks wrong (the switches
  are `gta4_native_astc_dxt1`, `gta4_native_astc_alpha` and `gta4_native_astc_dxn` in `args.txt`).
- Busy city areas, the first drive through an area (shader and texture work) and long hot sessions
  are where frames get uneven.
- Some reflections or geometry may look wrong (Mali has no clip distances), and memory use is
  higher than on Adreno (decoded textures).
- Long, hot sessions still warm the phone up and it throttles at thermal status 3; the CPU sampler in the debug
  log is how that is being measured and reduced.
- Occasional crashes or freezes can still happen. **Save often**, and report them with a log.

## Reporting problems

Open an [Issue](../../issues) and attach `Android/data/com.libertyrecomp/files/last_launch.txt`
and the two files a *Play (Debug Log)* run leaves in `Downloads/LibertyRecomp`
(`debug-*.log`, `runtime-debug-*.log` and the audio recording). Say which GPU you have.

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
