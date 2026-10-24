# Nioh3Fix

A drop-in `dinput8.dll` for **Nioh 3** (PC, Steam, game version 2.02) that fixes the game's constant
micro-stutter and its abnormally high CPU usage, without changing gameplay.

- **Smooth motion at any frame rate**: one game update per rendered frame, sized to the real time that
  passed (variable timestep), so 75, 90 or 110 fps all look smooth instead of juddering.
- **Even pacing fallback**: on slower PCs it switches to a steady 60/40/30 fps that keeps the game at full speed,
  chosen to suit the monitor's refresh rate (for example 30 rather than 40 fps on a 60 Hz screen).
- **About a third less CPU**: five busy-waiting threads now rest instead of spinning cores at 100%.
- **No game files changed**: delete the DLL and the game is back to stock.
- **Fails safe**: if a game update changes the code it looks for, the affected part switches itself off.

> Not affiliated with or endorsed by KOEI TECMO GAMES or Team NINJA. "Nioh" is a trademark of its owner.

---

## The problem

Nioh 3 simulates the world in fixed steps of 1/120 s and draws frames whenever it can. Below 120 fps a
frame therefore shows one step, two steps, or sometimes none. Even at a steady ~100 fps, motion
visibly jumps about 15 times a second. This happens on every PC and isn't a driver or hardware
problem.

On top of that, several game threads "wait" by checking the same condition as fast as they can. Each one
keeps a processor core at 100% while doing nothing useful. The title screen alone kept about 4 cores busy.

## Results

Measured with [PresentMon](https://github.com/GameTechDev/PresentMon) and the game's own update counter
(test PC: RTX 4090, 2560×1440, "120 (fixed)" cap, VSync on, 240 Hz monitor).

| | Unmodded | Nioh3Fix |
|---|---|---|
| Steps shown per frame | 86% one / 14% two / 1% none | 100% one update, sized to real time |
| Game speed | 120 steps/s | 120 steps/s (checked every 10 s) |
| Hitches (frame > 2× median) | 4.7–18 per minute | 0 in 1–5 minute runs |
| Worst frame | 69–180 ms | ~17–25 ms |
| CPU (whole game, gameplay) | ~9.3 cores | ~6 cores |

Gameplay checks: Ki (stamina) regeneration measured 252 Ki/s with variable timestep and 251 Ki/s
without it, and load times stayed at 14–16 s with the CPU fixes on.

## Installation

1. Download `Nioh3Fix.zip` from [Releases](../../releases).
2. Extract it into the Nioh 3 folder, the one containing `Nioh3.exe`. In Steam, right-click Nioh 3 and
   choose **Manage → Browse local files**.
3. In the game, set **Frame Rate Cap** as shown in [Recommended setup](#recommended-setup). For most PCs
   that's **120 (fixed)**.

That's all. A `nioh3fix.log` file appears next to the game showing what the fix found and is doing. If
the game folder isn't writable, it goes to `%LOCALAPPDATA%\Nioh3Fix` instead.

**Uninstall:** delete `dinput8.dll` and the `nioh3fix*` / `Nioh3Fix_*` files.

**In game:** press **F10** to switch the variable timestep on and off and compare.

## Recommended setup

Pick the in-game **Frame Rate Cap** to match your monitor and the frame rate your PC usually reaches:

| Your monitor | Your PC's frame rate | In-game Frame Rate Cap | `nioh3fix.ini` | Result |
|---|---|---|---|---|
| 120 Hz or faster | 60–120+ fps | 120 (fixed) | defaults | Smooth at whatever fps you get (75, 90, 110…) |
| 120 Hz or faster | ~40–60 fps | 120 (fixed) | defaults | Steady 40 fps at full game speed ¹ |
| 60 Hz, no G-Sync/FreeSync | holds 60 fps | 60 (fixed) | defaults | The game's own steady 60 fps, less CPU use |
| 60 Hz, no G-Sync/FreeSync | below 60 fps | 120 (fixed) | defaults | Steady 30 fps at full game speed (or lower graphics settings to hold 60) |
| G-Sync / FreeSync (VRR) on | any | 120 (fixed) | `PacerFitDisplay=0` | Smooth 60–120 fps; steady 40 or 30 below that |
| any | below 40 fps | 120 (fixed) | defaults | Steady 30 fps at full game speed |

¹ On 144/165 Hz monitors without VRR, 40 fps can't be shown perfectly evenly because a frame lasts 3.6 or
4.1 refreshes. Running the monitor at 120 Hz for this game fixes that.

Why: at **120 (fixed)** the game moves in 120 small steps per second, and Nioh3Fix can make any frame rate
from 60 to 120 smooth. At **60 (fixed)** the game moves in 60 steps per second and paces itself evenly,
as long as the PC holds 60 fps; Nioh3Fix then only reduces CPU usage. A 60 Hz monitor can't show more than
60 fps, so "120 (fixed)" adds nothing there when the PC holds 60.

Below 60 fps, variable timestep isn't used: one frame would have to cover a bigger time step than the game
ever takes on its own (1/60 s), which the game wasn't built for. Even pacing at 40 or 30 fps keeps the game
at full speed instead.

## Settings

Everything is in `nioh3fix.ini`, with plain-language comments. You can edit it while the game runs,
and changes apply within 2 seconds. The defaults are meant for every PC.

| Setting | Default | What it does |
|---|---|---|
| `VariableTimestep` | 1 | The main stutter fix: one update per frame, sized to real time |
| `VfrMinFps` | 58 | Below this fps (for 2 s), use even 40/30 fps pacing instead; switches back automatically |
| `VfrAllowOnline` | 0 | Also use variable timestep in co-op/online play (untested, so off) |
| `Pacer` | 1 | Even pacing when variable timestep isn't active: 1 = auto, 0 = off, or 60/40/30 |
| `PacerFitDisplay` | 1 | Auto pacing only uses rates the monitor shows evenly (60 Hz: 60/30; 120/240 Hz: all). Set 0 for 60 Hz VRR monitors |
| `PumpFix` | 1 | Window thread waits for messages instead of polling non-stop |
| `GatePollUs` | 100 | Render thread naps 0.1 ms between work checks instead of 1 ms busy loops |
| `UpdateNap` | 1 | Game-logic thread naps until 1 ms before its next update is due |
| `FenceNap` | 1 | Render thread naps while waiting for the GPU to finish a frame |
| `DrainNap` | 1 | Render thread naps while its command queue empties |

## Compatibility

- **Windows:** 64-bit Windows 10 (1803 or newer) or Windows 11.
- **CPU:** works on any Intel or AMD processor. It's plain x86-64 code (SSE2 baseline, no AVX) and uses
  only standard Windows timers.
- **GPU:** works on NVIDIA, AMD and Intel cards, and on laptops with two GPUs. It never touches Direct3D
  or the graphics driver. GPU load for the pacer comes from Windows' own per-process GPU statistics
  (what Task Manager shows), for every adapter.
- **Overlays:** Steam, RTSS/Afterburner and Discord overlays are fine.
- **Game version:** built and tested on 2.02. The game's code is located by unique byte patterns after
  Steam's DRM has decrypted it. If an update changes that code, the affected feature logs `PASSIVE` and
  leaves the game untouched; nothing is patched blindly.
- **Other mods:** only one `dinput8.dll` can be installed, so other mods that use that file name conflict.

## How it works

| Part | Where | What |
|---|---|---|
| Proxy | `src/dllmain.cpp` | Forwards the real `dinput8.dll` exports from the System32 folder; installs everything else from background threads |
| Variable timestep | `src/vfr/` | The nioh3fps core. A render "heartbeat" at the packet take gates the update thread so exactly one update runs per rendered frame. That update's step is scaled by measured real time. The game systems that assume 120 updates/s (character movement steps, frame-based timers, input-hold timers, turning speed) receive a matching rate. It disarms automatically in menus, cutscenes, loading, pause and online sessions. |
| Pacer / hybrid | `src/pacer.cpp` | At the render thread's once-per-frame clock read, holds the frame until the next slot of an even 120/60/40/30 fps schedule, phase-locked to the display's vblank and clear of update boundaries. Auto mode prefers rates that last a
whole number of refreshes of the monitor the game is on. It also decides between variable timestep and even pacing from the measured fps and GPU/CPU headroom. |
| CPU fixes | `src/dllmain.cpp` | Hooks `Sleep`, `QueryPerformanceCounter`, `PeekMessageA` and one queue-check leaf function. A wait is changed only when its caller matches the exact game code signature. Waits nap in 0.1 ms slices on high-resolution timers and never past the moment the game would have continued. |

Every change was A/B tested for game speed (update counter at exactly 120/s, stamina regeneration,
load times). Ideas that cost game time were rejected, for example a generic `Sleep(0)` backoff that
slowed the game to 116 updates/s.

## Building

Requirements:
- Visual Studio 2022 or its Build Tools, with **Desktop development with C++**
- Clang: [LLVM for Windows](https://github.com/llvm/llvm-project/releases), or the Visual Studio
  component **C++ Clang tools for Windows**

Then run:

```bat
build.bat
```

Output is `bin\dinput8.dll` and `bin\Nioh3Fix.zip`, the release package. Set `CLANG=path\to\clang.exe`
if Clang isn't found automatically.

```
src/                  Nioh3Fix: proxy, CPU fixes, pacer/hybrid (C++ / MASM)
src/vfr/              variable-timestep core ported from nioh3fps (GNU C / asm, built with Clang)
third_party/minhook/  MinHook 1.3.4 (x64 parts)
nioh3fix.ini          default settings shipped with the DLL
Nioh3Fix_README.txt   plain-text readme shipped with the DLL
```

## Troubleshooting

- **Nothing changes / no log file:** make sure `dinput8.dll` sits next to `Nioh3.exe`. Then check
  `%LOCALAPPDATA%\Nioh3Fix` for the log.
- **`PASSIVE` in the log:** that part didn't recognise the game's code (usually after a game update), so it
  stays off and the game runs unmodified.
- **Something feels faster or slower:** press F10 to compare, set `VariableTimestep=0` to use only even
  pacing, and please open an issue with your `nioh3fix.log`.
- **At "60 (fixed)":** the game is already even at that cap, so only the CPU fixes apply.

## Credits

- **[yogesh1239](https://github.com/yogesh1239)** for [nioh3fps](https://github.com/yogesh1239/nioh3fps),
  the variable-timestep core in `src/vfr/`. It covers the reverse engineering of the game's update loop,
  the lockstep gate and the consumer fixes. Used under the MIT licence.
- **[Tsuda Kageyu](https://github.com/TsudaKageyu)** for [MinHook](https://github.com/TsudaKageyu/minhook),
  used for all function hooks (BSD 2-Clause).
- **Vyacheslav Patkov** for Hacker Disassembler Engine 64, part of MinHook (BSD 2-Clause).
- **Intel** for [PresentMon](https://github.com/GameTechDev/PresentMon), used to measure frame pacing
  during development (not included).
- **Team NINJA / KOEI TECMO GAMES** for Nioh 3.

Full licence texts are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

Nioh3Fix is released under the [MIT License](LICENSE). Third-party parts keep their own licences; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
