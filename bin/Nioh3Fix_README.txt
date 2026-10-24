Nioh3Fix - stutter + CPU usage fix for Nioh 3 (PC, game version 2.02)
======================================================================

INSTALL
  1. Copy dinput8.dll and nioh3fix.ini into the Nioh 3 folder (the one with Nioh3.exe;
     in Steam: right-click Nioh 3 > Manage > Browse local files).
  2. Start the game and set Frame Rate Cap in the game's settings (see WHICH FRAME RATE CAP below;
     for most PCs it is "120 (fixed)").
  That's it. nioh3fix.log appears next to the game (or in %LOCALAPPDATA%\Nioh3Fix if the game
  folder is not writable) and shows what the fix found and is doing.

UNINSTALL
  Delete dinput8.dll and the nioh3fix* / Nioh3Fix_* files. No game file is changed on disk.

IN GAME
  F10 switches the stutter fix (variable timestep) on/off so you can compare.
  All settings are explained in nioh3fix.ini; edits apply while the game runs.


WHICH FRAME RATE CAP
--------------------
  Monitor 120 Hz or faster, PC 60+ fps    -> 120 (fixed), defaults: smooth at any fps up to 120
  Monitor 120 Hz or faster, PC 40-60 fps  -> 120 (fixed), defaults: steady 40 fps, full game speed
  Monitor 60 Hz, PC holds 60 fps          -> 60 (fixed), defaults: the game's own steady 60 fps
  Monitor 60 Hz, PC below 60 fps          -> 120 (fixed), defaults: steady 30 fps (or lower settings)
  G-Sync / FreeSync monitor               -> 120 (fixed) and PacerFitDisplay=0 in nioh3fix.ini
  PC below 40 fps                         -> 120 (fixed), defaults: steady 30 fps
  More detail and examples are at the top of nioh3fix.ini.


WHAT WAS WRONG
--------------
1. Micro-stutter below the frame cap
   The game moves in fixed 1/120 s steps. Below 120 fps each frame shows 1, 2 (sometimes 0)
   steps, so motion jumps about 15 times a second even at a steady ~100 fps.
2. Abnormal CPU usage
   Four game threads "wait" by spinning a core at 100% (window message loop, render work
   poll, GPU-fence poll, command-queue drain, and the 120 Hz update scheduler).

WHAT THE FIX DOES
-----------------
  Variable timestep  One game update per rendered frame, sized to the real time that passed:
                     smooth at any frame rate from VfrMinFps to 120. Ported from nioh3fps by
                     yogesh1239 (MIT). It also rescales the game systems that assume 120
                     updates/s. Off in menus, cutscenes, loading and online play.
  Even pacing        Below VfrMinFps (or when variable timestep is off or not allowed) frames
                     are paced at a steady 120/60/40/30 fps, so every frame shows the same
                     number of steps, at full game speed. The rate suits your monitor
                     (e.g. 60 or 30 fps on a 60 Hz screen, so frames are also shown evenly).
  CPU fixes          The spinning waits nap in 0.1 ms slices instead. They never wait past the
                     moment the game would have continued, so game speed is unchanged.

MEASURED (RTX 4090, 2560x1440, 120 cap, 240 Hz)
  unmodded:   86% of frames 1 step / 14% 2 steps; 5-18 hitches per minute; ~9.3 CPU cores busy
  Nioh3Fix:   100% of frames 1 update sized to real time; game speed exactly 120 steps/s;
              0 hitches in 1-5 minute runs; worst frame ~17-25 ms; ~6 CPU cores busy;
              Ki regeneration unchanged (252 vs 251 Ki/s); load times unchanged.


COMPATIBILITY
-------------
  - Any 64-bit Windows 10 (1803 or newer) / Windows 11 PC.
  - CPU vendor neutral: plain x86-64 code (SSE2 baseline, no AVX), Windows timers only.
  - GPU vendor neutral: it never touches Direct3D or the graphics driver. GPU load for the
    pacer comes from Windows' own per-process GPU statistics (as shown in Task Manager) for
    every adapter, so NVIDIA, AMD and Intel cards and laptops with two GPUs all work.
  - Game code is located by unique byte patterns once Steam's protection has decrypted it.
    If a game update changes that code, the affected part switches itself off ("PASSIVE" in
    nioh3fix.log) and the game runs unmodified; nothing is patched blindly.
  - Overlays (Steam, RTSS/Afterburner, Discord) are fine.
  - Other dinput8.dll mods cannot be used at the same time (only one file can have that name).

KNOWN LIMITS
  - Variable timestep makes physics take real-time sized steps (up to 2x at 60 fps). Not
    noticeable in play, but it is a change to how the simulation is stepped; F10 compares.
  - Online/co-op: variable timestep is off by default (VfrAllowOnline=0); even pacing still works.
  - At the "60 (fixed)" cap the game is already even, so only the CPU fixes apply.


SOURCE CODE AND CREDITS
-----------------------
  Source, build instructions and issue tracker: see the project's GitHub page.
  The variable-timestep core is derived from nioh3fps by yogesh1239
  (https://github.com/yogesh1239/nioh3fps, MIT licence). Hooks use MinHook by Tsuda Kageyu
  (BSD 2-Clause). Licences: Nioh3Fix_LICENSE.txt and Nioh3Fix_THIRD_PARTY_NOTICES.txt.
