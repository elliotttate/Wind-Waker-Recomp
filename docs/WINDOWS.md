# BlueWake on Windows

BlueWake also builds as a native Windows x86-64 program. As on the Mac, you build it yourself from your own disc:
the game's code is translated from that disc during the build, so **the folder you build is yours alone: never
share or upload it.**

The Windows build is the same static recompilation as the iOS app: the same translator, the same pinned runtime
(RecompCore, GXRuntime, Aurora) and the same generated game source, verified against the same digest. Only the
host around it is different: Direct3D 12 through Dawn instead of Metal, SDL3 input and audio, and a Windows entry
point in place of the iOS app shell.

## Status

Verified on 2026-09-28 and 29 on one PC (Intel i9-13900KF, 64 GB, NVIDIA RTX 5090, a 3840x2160 120 Hz display,
Windows 11), Visual Studio 2026's clang 22, from a Redump-verified `.rvz`, at commit 79b7e16 plus the Windows
port:

- The builder runs end to end from the `.rvz`: the generated game source has the verified digest (`54f54434`,
  the same as the macOS builder's), and the mods match the Mac's counts (widescreen 22 chunks, Better Wind
  Waker's options 15, 40 with 16:10 and the options together).
- Boot to control on Outset through the title, file creation and the opening, headless and in the window
  (Direct3D 12), with the scripted route the Mac builder trains on; saves are written to the memory card.
- The keyboard reaches the game through SDL (key messages posted to the window read as A at the title).
- Widescreen 16:9 renders 1280x720 with the HUD at the edges; Better Wind Waker's options load (instant text
  patched 4,411 messages, as on the Mac); Smooth Motion draws the in-between frames.
- Speed, with the optimization profile, Smooth Motion and the fast scene changes on, paced in real time
  through the whole new-game opening (the title, the prologue, Outset from Link waking to the lookout; 461
  seconds): the game at full speed every second (lowest 59.8 retraces a second), including the view over
  Outset's village (about 10,000 draws a frame), where it had dropped to 24 to 28 FPS before the in-between
  work moved off the graphics thread; 60 frames a second shown in all but 9 seconds, each a scene change
  where the game itself skips a frame or briefly runs past 30. Standing on Outset: 59.2 to 60 every second.
  In-between frames sit halfway between their neighbours (`scripts/mac/frame_interp_report.py`: 30 of 30).
- The settings menu (F1 and Esc open and close it; its Display and Mods tabs; a setting changed with the
  mouse and saved), F11 and Alt+Enter fullscreen, moving the window by its title bar (the place is restored
  at the next launch) and F9's frame rate, driven with hardware-style mouse and keyboard input.

The Windows release of 2026-09-29 (branch `windows-release`: the Mac build's renderer and options from main
e5c6ae4, RecompCore 825f103) was checked on the same PC with its display at 60 Hz, windowed and paced, Link
running on Outset from a loaded save: Smooth Motion by default 59.8 frames a second shown (lowest 58.8) with the
game at its full 30; F10 off and on again (30, then 60); `--no-smooth` 29.9; `--120` and 120 in the settings
file 60 shown with the game at 30 (the display guard below); `--60hz` 59.9 game frames a second (lowest 59.4).
120 FPS on a 120 Hz display was not measured on Windows in this pass.

A slower CPU, 2026-09-30: a tester's Ryzen 5 5600X (with an RTX 3060) got about 25 FPS from that release. On
the same i9 pinned to 12 of its efficiency cores (slower than a Zen 3 core one thread at a time, so a harsher
stand-in), the release stood at Outset's spawn view (about 14,400 draws a frame) at 23 to 26 game frames a
second, the game waiting 190 to 250 ms a second for the GX translation worker; six performance cores with their
hyperthreads held 30. The worker's per-draw work is cheaper since (the derived pipeline state cached, lookups
remembered, a lighter hand-off to Smooth Motion; [status/SLOW_CPU_2026-09-30.md](status/SLOW_CPU_2026-09-30.md)):
29 to 30 there on the E-cores, every frame the same as before byte for byte.

The Windows release 0.1.1 (1e97dca: RecompCore 82607d4, the certified native game math and recovered J3D
matrices, [status/NATIVE_GAME_MATH_2026-09-30.md](status/NATIVE_GAME_MATH_2026-09-30.md)): the unpacked zip's
first launch prepared the disc from the `.rvz`; paced on Outset with Link running, 30 game frames a second
(60 shown) on all cores and 29.9 on the E-cores; the Outset route's frames 0.1.0's byte for byte.

Not yet tried on Windows: a game controller, audio on other output devices, the HD texture packs, the later
game, and other PCs (AMD CPUs and GPUs, Vulkan; slower CPUs only through the E-core stand-in above).

## Download and play

A release's `WindWakerRecomp-VERSION-windows-x64.zip` is ready to play and has no game in it. Unpack it and run
`BlueWake.exe`. The first launch asks for your disc image (the GameCube USA disc, `GZLE01` revision 0, as an
`.iso`, `.gcm` or Dolphin `.rvz`), checks it, prepares it once and remembers it:

- an `.iso` or `.gcm` is used where it is; an `.rvz` (or `.wia`, `.gcz`, `.ciso`, `.nfs`) is unpacked once, with
  the bundled `nodtool.exe`, to `%APPDATA%\BlueWake\GZLE01.iso` (about 1.4 GB)
- the disc id and `main.dol`'s SHA-1 (revision 0) are checked, and `main.dol` and the 415 RELs are prepared into
  `%APPDATA%\BlueWake\game` by the iOS app's own importer (`apple/ios/src/disc_import.c`), byte for byte what the
  builder prepares
- `%APPDATA%\BlueWake\disc.txt` remembers the disc: later launches start straight away, and if the file has gone
  BlueWake asks again. The settings' *Sound and files* tab has *Choose another disc image*.

A folder the builder made (below) has the disc and prepared files beside the app and never asks. `--disc FILE`
chooses a disc for one session.

The zip is made from a builder's folder by `python scripts/windows/package_release.py VERSION`: it takes the
app, the game module, the runtime DLLs, `nodtool.exe` and the licenses, leaves out the disc, `main.dol`, the
RELs and anything else from the disc, and refuses to write the zip if any such file would be in it.

## What you need to build it

- Windows 10 or 11 on an x86-64 PC. The game module is compiled for `x86-64-v3` by default (AVX2, FMA, BMI2,
  MOVBE: Intel Haswell, AMD Zen or newer); the builder drops to an older level on older CPUs.
- A GPU with Direct3D 12
- [Visual Studio 2022 or newer](https://visualstudio.microsoft.com/) (Community is fine) with the
  **Desktop development with C++** workload and the **C++ Clang Compiler for Windows** component
- [Python 3.10+](https://www.python.org/), [Git](https://git-scm.com/), and CMake 3.25+ and Ninja
  (`pip install cmake ninja` works)
- Your disc image of *The Legend of Zelda: The Wind Waker*, GameCube USA (`GZLE01`, revision 0). An `.iso` or
  `.gcm` works directly. A Dolphin `.rvz` (or `.wia`, `.gcz`, `.ciso`, `.nfs`) is converted to an ISO with
  [nodtool](https://github.com/encounter/nod), which the builder compiles from crates.io the first time; that needs
  [Rust](https://rustup.rs). You can instead convert it in Dolphin (right-click the game, **Convert File...**,
  format ISO).
- About 15 GB of free disk space (the converted disc, the generated source and the compiled module)

(Playing a release zip needs none of this: Windows 10 or 11, a Direct3D 12 GPU, an AVX2 CPU and your disc image.)

## Build

From a normal terminal in the checkout:

```bash
python scripts/windows/build.py "D:\Games\The Legend of Zelda - The Wind Waker (USA).rvz"
```

The builder finds Visual Studio itself (no developer prompt needed), fetches the pinned RecompCore and DolRecomp
into `ref/`, checks and converts the disc, extracts and translates the game, checks the generated source against
the verified digest, adds the mods, compiles the game module and the app, and writes the app folder
`build\windows\BlueWake`. Each stage prints its progress; full logs are in `build\windows\logs`. Rerunning the
same command reuses finished work.

The first build takes about an hour on the i9-13900KF: training the optimization profile (below, about 25
minutes) and compiling the game module with it (about 15 minutes: the code the training ran at `-O2`, the rest,
which the profile marks cold, at `-O1`; `--no-tiered` compiles all of it at `-O2`, about 25 minutes longer, and
measured no faster in play). Later builds reuse all of it and take minutes, unless the game source, RecompCore's
runtime, the compiler or `--march` changed.

**Optimization training.** Like the Mac builder's local training, the Windows builder makes an optimization
profile from your own game: it compiles an instrumented game module (a couple of minutes), plays the opening
through to player control on Outset twice without a window (once plain, going on to tour ten places by warp -
Windfall, Dragon Roost and its cavern, Forest Haven, the Forbidden Woods, the Tower of the Gods, the Forsaken
Fortress, the sea, Hyrule - in about 17 minutes; once with widescreen and Better Wind Waker's options, about 7),
and compiles the real module with the counts (clang's `-fprofile-instr-use`). The app itself is compiled with a
profile of its own code that comes with the source (`windows/pgo/app.profdata`) and ThinLTO. The profile is made from your disc, so it stays in `build\windows\pgo-local` and is
never shared. `--no-train` skips it (a faster first build, a slower game).

Options (`--help` lists all):

| Option | |
| --- | --- |
| `--source-only` | Stop after generating the source: checks your tools, disc and translation in a few minutes |
| `--no-mods` | Skip the mods (widescreen 16:9 and 16:10, Better Wind Waker's options) |
| `--no-train` | Skip the optimization training (see above) |
| `--no-tiered` | Compile all of the game module at `-O2` (see above) |
| `--no-app-pgo` | Compile the app without its profile and ThinLTO |
| `--retrain` | Train again although nothing the profile depends on changed |
| `--jobs N` | Parallel compile jobs (default: the cores, as far as free memory allows) |
| `--march LEVEL` | CPU level for the game module and the app (default `x86-64-v3`) |
| `--console` | Build `BlueWake.exe` as a console program |
| `--out DIR` | Build directory (default `build\windows`) |

## Play

Run `build\windows\BlueWake\BlueWake.exe`. The window opens in the middle of the screen, sized for it (the
tallest multiple of 240 lines within 80 percent of the screen); drag its title bar to move it and an edge to
size it.
BlueWake remembers where you left it, its size, and whether it was fullscreen.

| | |
| --- | --- |
| Control stick | W A S D |
| C-stick | T F G H |
| D-pad | arrow keys |
| A, B, X, Y | J, K, U, I |
| L, R, Z | E, R, Q |
| START | Return |
| Jump | Space (a controller's left bumper) |
| Sprint | Shift (a controller: click the left stick) |
| Camera | Click the game, then move the mouse (Esc releases it); the wheel zooms |
| Controller camera | The right stick turns it and aims; its click is first person (and back out) |
| Settings | F1, or Esc when the mouse is not the camera |
| Fullscreen | F11 or Alt+Enter |
| Frame interpolation on or off | F10 |
| Frame rate | F9 |
| Save state, load the latest | F5, F8 |

Game controllers work through SDL (Xbox, PlayStation, Switch Pro and others). The title screen wants A to reach
the file menu. The right stick turns the camera directly, like the mouse (360 degrees a second at full tilt, no
easing), instead of the game's eased C-stick camera; in first person and when aiming an item it aims (180 degrees
a second), and in the telescope and the Picto Box the left stick (or the D-pad) zooms. The camera, by mouse or
stick, stays out of the ground and the water: its angles go in before the game's own wall and ground check. The mouse turns the game's own camera around Link and tilts it, and a left click is A; in first
person and when aiming an item it aims instead; a cutscene, door or Z-target takes the camera back. Scene
changes are quick: the fades are short and the black between them runs as fast as the PC can, and through a door
with a knob Link skips the walk-in and the door closing behind him (both can be turned off in the Mods tab).

**Climbing** (off by default; Mods, *Climb any wall*): Link climbs a plain wall the way he climbs ivy, Breath
of the Wild style. Walk or jump into a steep wall to grab it; climbing and hanging use up the stamina wheel
beside him (hanging still, less), and when it runs out he lets go and cannot climb again until it has refilled
on the ground. The wheel lasts 12 seconds of climbing by default (4 to 30 in the menu). Ivy, ladders, ledges he
pulls himself onto, walls he sidles along and blocks he pushes behave as in the game.

**Save states.** F5 saves the whole running game to `%APPDATA%\BlueWake\states` and F8 loads the
latest one (the menu has buttons for both). They are taken at the next moment the game is between frames with
no scene change, door or memory card write in progress, so one can wait a moment. A state is meant for the
build that made it: another build refuses one made by a different translation of the game or a different
machine layout. The memory card is not part of a state; use the game's own save for anything you want to
keep. (On a Mac F9 loads; on Windows F9 is the frame rate.)

**Settings.** F1 opens the settings over the game (it keeps running underneath; the keyboard and mouse work the
menu until you close it):

- *Display*: fullscreen, the frame rate (30 FPS; 60 FPS, 120 FPS or the display's own rate up to 240 with
  frame interpolation; or the experimental 60 Hz game logic; see below), the frame rate counter, the render resolution (the window's own pixels, or 1x to 4x the GameCube's 480 lines),
  texture filtering (up to 16x anisotropic), keeping the picture's shape, pausing while the window is in the
  background, and putting the window back in the middle.
- *Controls*: controller vibration (Enhanced, Classic or Off, its strength, and trigger feedback), the mouse
  camera, its sensitivity and vertical direction, the fast right-stick camera (or the
  game's own) and its turn and aim speeds, the controller's camera stick directions (for either), and the
  keyboard layout.
- *Mods*: 4:3, 16:10 or 16:9, Better Wind Waker and each of its options, quick doors, skipping through the
  black while loading, climbing any wall and its stamina, the Forest Water challenge's two helps (keeping
  watered trees when the water runs out, and a 30-minute timer), and an HD texture pack (a Dolphin-format
  pack for GZLE01, in the folder the menu opens, or the folder the Wind Waker HD importer made: see
  [WWHD_TEXTURES.md](WWHD_TEXTURES.md)).
- *Sound and files*: fast (Dolphin's high-level) or exact (the DSP's own program) sound, and your files.

Display and control settings apply at once. The mods and the sound mode are compiled paths chosen when the game
starts, so those marked `*` apply when BlueWake starts again; **Restart now** does that. Everything is saved to
`%APPDATA%\BlueWake\settings.ini`.

**Frame rate.** The Display tab's **Frame rate** list has five choices. **60 FPS (frame interpolation)** is the
default (Smooth Motion): the renderer draws a blended frame between each of the game's 30, so the game shows 60
frames a second (F9's counter reads `60 FPS (game 30)`). **120 FPS (frame interpolation)** draws three in-between
frames each, for a 120 Hz display, and **30 FPS** is the game's own, with none; F10 turns frame interpolation off
and back on. 120 FPS is
used only while the window is on a display of 100 Hz or more: on a 60 Hz display the game would wait for
presents the display cannot show and run at half speed, so it shows 60 there (the menu says so). Scenes with
nothing to blend (menus, the title, still shots) keep the same rhythm, so the picture's timing does not change
when they begin or end.

**Match the display (frame interpolation, up to 240 FPS)** shows as many frames as the window's display does,
in whole steps of 30: 240 on a 240 Hz display, 180 on 200 Hz, 150 on 165 Hz, 120 on 144 Hz, 90 on 100 Hz and
60 on a 60 or 75 Hz one (the menu shows which). It follows the window to another display.

Frame interpolation gives way when the computer cannot keep up. If the GPU or the render thread falls a frame
behind, or the game itself drops below full speed (game frames more than 35.5 ms apart on average: a CPU with
few cores, where the in-between frames' work takes time the game's own thread needs), the in-between frames
stop until things are calm again, rather than the game running in slow motion. On 4 of an i9-13900KF's slower
cores, 60 FPS used to hold the game at 24-28 frames a second; now the game keeps its 30. After a stop for the
game's speed, it waits a little longer each time before trying again (up to 2 minutes). The session log says
when it happens (`[interp-pace]` lines).

**60 Hz game logic (experimental, not recommended)**, the list's last choice, runs the game itself 60 times a
second instead of blending frames, so parts of it (movement, cutscenes, some timers) still run too fast; frame
interpolation is off while it runs. It needs a fast CPU (see
[status/WINDOWS_NATIVE_60HZ_2026-09-29.md](status/WINDOWS_NATIVE_60HZ_2026-09-29.md)), and some timing is not
converted yet ([SIMULATION_60HZ.md](SIMULATION_60HZ.md)). It applies when BlueWake starts again.

Command-line options (`BlueWake.exe --help`) choose for one session; they win over the settings file:

| Option | |
| --- | --- |
| `--widescreen` | 16:9: the widescreen mod (a wider camera, culling and HUD) with a 16:9 picture |
| `--aspect 16:10` | 16:10 instead (`4:3` is the game's own) |
| `--smooth`, `--no-smooth` | 60 FPS with frame interpolation (the default), or the game's own 30 FPS |
| `--120` | 120 FPS with frame interpolation, for a 120 Hz display |
| `--60hz`, `--30hz` | The experimental 60 Hz game logic on or off (not recommended) |
| `--betterww` | Better Wind Waker's settings at their defaults (Swift Sail, instant text, faster climbing...) |
| `--options LIST` | Change them: `name,-name,...`, or `none,name,...` (names in `mods/betterww/options.txt`) |
| `--fullscreen` | Start in fullscreen |
| `--window WxH` | The window's size |
| `--scale N` | Render at N x 480 lines (0: the window's own pixels) |
| `--fps` | Show the frame rate |
| `--stretch` | Fill the window instead of keeping the game's aspect ratio |
| `--no-mouse-camera` | Keep the mouse out of the camera |
| `--lle-audio` | Run the DSP's own microcode instead of the high-level Zelda ucode |
| `--disc FILE` | Read another copy of the disc |

The mods need no extra files: they are compiled into your game module from your disc (see [MODS.md](MODS.md)).
The environment variables `scripts/mac/run_host.sh` documents (`BLUEWAKE_*`, `DOL_*`) work the same way, for
example `BLUEWAKE_MOUSE_SENSITIVITY` and `BLUEWAKE_MOUSE_INVERT_Y`.

## Your saves and logs

Everything that is yours lives in `%APPDATA%\BlueWake`, outside the build, so rebuilding or deleting the build
never touches it:

- `GZLE01.card`: the memory card with your saves
- `sram.bin`: the console's settings (sound mode and the like)
- `settings.ini`: the settings menu's choices and the window's place
- `Load\Textures\GZLE01`: where an HD texture pack goes
- `Load\Textures\WWHD`: the pack made from your own Wind Waker HD disc, if you import one
  ([WWHD_TEXTURES.md](WWHD_TEXTURES.md))
- `states\quick-*.bwstate`: save states (F5); delete any you no longer want
- `logs\session-*.log`: the newest eight sessions, one line a second of speed and timing plus anything that went
  wrong. Attach the relevant one to a bug report. If BlueWake crashes, the log says where. It starts with the
  `CPU model`, `CPU cores` and GPU lines. Each second has a `[perf]` line (the game's retraces: 60 is full
  speed) and an `[fps]` line (frames shown, the game's own frames, how long the game waited for the graphics
  thread, and how busy the GX worker, Smooth Motion's helper and the render worker were: one near 100 percent
  is the thread the PC runs out of). A second under 57 frames adds an `[fps-dip]` line with the reason, and a
  present that held the game 100 ms or more a `[present-slow]` line saying which part took the time.
- Aurora's pipeline cache, so later launches start drawing sooner

## How the port works

The Windows host is `windows/`: a CMake project that compiles the unchanged host (`runtime/host/src`), GXRuntime
with Aurora (prebuilt Dawn and SDL3 packages, as Aurora fetches them), and Dolphin's DSP from RecompCore, with
clang (GNU driver, MSVC ABI) from Visual Studio.

- **POSIX calls.** The host uses a handful: threads, clocks and sleeps, the environment, `dlopen`, directory
  listing. `windows/compat` provides them on Win32 (sleeps use a high-resolution waitable timer, since `Sleep`
  rounds up to the 15.6 ms tick). The header is force-included into BlueWake's own sources and GXRuntime's C
  runtime only, never into third-party code.
- **The game module** is `gGZLE01_recomp.dll`, built by the same `cmake/composite` project as the iOS dylib, and
  loaded the same way. On Windows it exports its entry points explicitly.
- **The same source, byte for byte.** Windows' C runtime and Python write text files with CRLF line endings,
  which would change the generated game source and its verified digest. The translator is linked with MSVC's
  `binmode.obj` (binary file mode by default), and the generators write `\n` explicitly. The source this builder
  generates has the same digest as the macOS builder's.
- **Compile time.** Each translated chunk is one very large function, and two LLVM passes are superlinear on
  them with clang 22 for x86-64 (measured with `-ftime-report`). The SLP vectorizer took 92 percent of a
  typical large chunk's time, and the largest chunks took over half an hour each; `-fno-slp-vectorize` brings
  them to a minute or two. The register coalescer took 95 percent of the worst remaining chunk's 44 minutes,
  joining copies into the context pointer's function-long live range again and again; capping that per range
  (`-mllvm -large-interval-freq-threshold=10`) brings it to about two minutes.
- **Memory.** A large chunk takes 1 to 3 GB in clang, so the builder runs as many compile jobs as free memory
  allows, not one per core, and retries a chunk that ran out of memory with fewer jobs.
- **The DSP** runs Dolphin's interpreter and its high-level Zelda ucode, as on iOS; the x64 DSP JIT is not built.
- **Floating point.** GXRuntime maps the guest's rounding and non-IEEE modes onto the x86 MXCSR, as it does onto
  the arm64 FPCR.
- **Stack.** Translated code recurses on the host stack as the game does on the GameCube's; the executable
  reserves 64 MB for the main thread (Windows' default is 1 MB).

- **Better Wind Waker's REL sites.** DolRecomp names a REL's option sites by its file name after the last `/`,
  and on Windows it joins a folder and a file with `\`, so the sites in `d_a_ship` and `d_a_agbsw0` would not
  match. The builder names the RELs' folder with `/` and a trailing `/` for that translation, which gives the
  15 option chunks the Mac build has, not 11.
- **Windows' own costs.** The C runtime's `getenv` locks and scans the whole environment, and GXRuntime reads
  a trace switch on every guest exception (4 percent of the game thread in a profile): its C sources and the
  module's runtime remember each call site's answer (`windows/compat/bw_getenv_cache.h`). Aurora paces frames
  with short sleeps, which Windows' default 15.6 ms timer tick would stretch; the app asks for 1 ms.
- **Profiling.** `BLUEWAKE_HOST_PROFILE=FILE` samples the game thread every millisecond and writes where it
  was, by module and offset, charging time in system code to the BlueWake function that called it.
- **Smooth Motion's cost.** GXRuntime translates the game's graphics commands on a worker thread (the FIFO
  worker), and the game waits for it at each frame's end. On Outset that is about 10,000 draws a frame, and
  matching each to the frame before and blending its matrices for the in-between frame took a fifth of that
  thread, enough to hold the game at 24 to 28 FPS in the village. Three changes (RecompCore, Aurora's
  `frame_interp.cpp` and `gxcore_draw.cpp`) bring it back under: the matching and blending run on a helper
  thread, in draw order, and the frame's end waits for it; a draw's matrix bank is carried by the camera's
  motion only for the matrices its vertices name; and the app is compiled for the same CPU level as the game
  module (AVX2 and FMA for that matrix work; the baseline build alone dropped the game to 28 FPS on Outset).
- **The worker on a slower CPU.** The game waits for the worker at every frame's end, so the worker's work per
  draw sets how slow a core can be. At Outset's spawn view (about 14,400 draws a frame) on the i9's efficiency
  cores, standing in for a Ryzen 5600X, it held the game at 23 to 26 FPS. The pipeline state derived from the
  GX registers is cached by a register version (96 percent of draws reuse it), a draw's pipeline and texture
  bind group lookups are remembered for the next draw, the assembly totals only a validation sink reads are no
  longer computed, and a draw that repeats its constants is handed to Smooth Motion's helper without a copy:
  29 to 30 FPS there, the frames byte for byte the same
  ([status/SLOW_CPU_2026-09-30.md](status/SLOW_CPU_2026-09-30.md)).
- **The optimization profile's key.** The builder retrains when the game source (mods included), the parts of
  RecompCore compiled into the module (GXRuntime's CPU core and headers, the recompiler ABI), the compiler, the
  CPU level or the recipe change, not when the app or host code around the module does.

Not done on Windows: the bundled Apple-silicon PGO profiles (the local training replaces them), and the iOS
overlay menus (touch controls, controller remapping, save management). The settings menu covers the rest.
