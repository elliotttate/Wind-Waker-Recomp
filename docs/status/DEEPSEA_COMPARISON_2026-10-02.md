# BlueWake against DeepSea — 2026-10-02

DeepSea (https://github.com/AH64-dll/DeepSea, checkout `E:\Github\DeepSea` at a343c13, the source of its
1.0.0-beta.4 release) is another Windows recompilation of Wind Waker GZLE01: DolRecomp translates the game
as here, but it runs on ModernGekko, a Dolphin fork, with Dolphin's own CPU-side chassis, CoreTiming and its
Vulkan renderer, and gets 60 fps by running the game's own drawing code a second time per game frame.

## Measured on this PC (i9-13900KF, RTX 5090), same save, same route

DeepSea was built here from that checkout: its runtime with Visual Studio's clang 22 targeting MinGW over
MSYS2's UCRT64 libraries, `-O3` and ThinLTO like its `windows-mingw-release-opt` preset; its module with its own
`scripts/disc_builder/build.py` (tiered `-O2`/`-O1`/`-O0`, `-march=native`). The release kit's PGO profile is
not in the repository, so its module ran without one (their own notes measure module PGO at +5 to +9
percent). Our Outset save was exported as a `.gci`, loaded by scripted A presses, and saved as a Dolphin state
at the spawn; the route (stand, then run and turn) is BlueWake's bench route converted from retraces to its
30 Hz pad ticks. Its speed is guest video fields a second (`[vi]`), ours retraces a second: the same quantity,
59.94 is full speed.

| Run | BlueWake 0.4.0 | DeepSea |
| --- | --- | --- |
| All 32 threads, unthrottled, interpolation off | 120 fields/s (60 game fps), held there by the 60 Hz display's vsync (945 ms a second waiting on presents) | 88-91 fields/s (44-45 game fps) |
| 4 E-cores, unthrottled, interpolation off (two quiet runs each) | 64-68 fields/s (32.5-34 game fps) | 39-41 fields/s (19.4-20.6 game fps) |
| 4 E-cores, paced, each one's 60 fps interpolation on | full speed: 30 game fps, 60 shown | 47-49 fields/s (about 0.7x: slow motion); its governor turned the in-between frames off ("host at 0.70x speed") |

So on a 4-core CPU BlueWake runs the game about 1.65 times as fast, and with interpolation on it is the
difference between full speed at 60 shown and 70 percent speed at 30 shown. Even granting DeepSea its missing
PGO profile, the gap stays above 1.5 times.

## Why (from reading both codebases)

- **Translated code.** Theirs returns to a chassis loop every 256 guest cycles (`finish_segment`), calls every
  main-executable function through the dispatcher (their direct-call module crashed and was not shipped),
  calls out of line for FP and paired-single arithmetic, and reloads `ctx->ram` with a second EXRAM probe and a
  journal check on every store. Ours: direct calls, inline FP, guest CPU and MEM1 as globals, inline lean loads
  and stores, prepaid block copies, turns of up to 16,384 cycles bounded by device deadlines.
- **60 fps.** Their in-between frames re-run `mDoGph_Painter` on the game's own thread (about 7 ms of every
  33 ms pair in their heaviest scene), so they cost the slowest thread; ours are built from the recorded draws
  on the graphics threads and cost the game thread nothing.

## Worth borrowing (none measured here yet)

1. **Game-thread priority:** they raise the emulation thread to ABOVE_NORMAL while 60 fps is on
   (`mods/frame60-accum/mod.c:281-319`) and measured fewer governor drops under a CPU hog; we never call
   `SetThreadPriority` on the game thread.
2. **A sturdier slow-game detector:** their governor takes the median of about 60 game frames, ignores gaps
   over 150 ms and acts after two slow windows (`mod.c:1753-1849`). Our `slow_game` is an 8-frame average that a
   single 100-249 ms hitch can push over 35.5 ms; check with a scripted stall and `[interp-pace]`.
3. **Shader stutter:** Dolphin draws with an ubershader while a pipeline compiles (we skip the draw),
   compiles with several threads (we use one), precompiles the known pipelines before play with a progress
   screen, and compiles a new shader under up to 32 sibling blend/depth/raster states already seen
   (`vendor/dolphin/Source/Core/VideoCommon/ShaderCache.cpp:1472-1515`).
4. **Smooth Motion coverage:** they interpolate cloth, sails and flags per vertex (`mod.c:6303-6900`; hook
   `dCloth_packet_c::draw` 0x80063728) and animated material and particle colours (`mod.c:4764+, 5743-6300`);
   we leave non-rigid meshes unblended and blend vertex constants only.
5. **GX worker per draw:** Dolphin's vertex loaders are specialised per format and write a compact vertex
   straight into the stream buffer; ours walk attributes generically into a 132-byte vertex (about 84 with
   `unorm8x4` colours and N/B/T only where emboss needs them), and the 2.3 KB transform snapshot is copied about
   three times a draw where a version number would do.
6. **Wider PGO training:** they merge boot, title, attract flyovers, Outset, sailing and save/load
   (`scripts/disc_builder/make_hot_list.py`); ours trains on boot to Outset only, so sea and dungeon code
   compiles without counts.
7. **Host app PGO and ThinLTO:** their runner PGO measured +2 percent.
8. **Natives we lack:** `J3DFifoLoadPosMtxImm`/`NrmMtxImm`/`NrmMtxImm3x3`, `PSMTXMultVecSR`, the fused
   `J3DMtxCalc*::calcTransform` bodies, `dBgW::ChkGrpThrough`, `cBgS_Chk::ChkSameActorPid` (their `sin`/`cos`
   use host libm and are not exact).
9. **Build time:** their tiered compile (`-O2` with PGO for main.dol, the runtime and 62 hot actor files;
   `-O1`/`-O0` for the rest; `-mllvm -enable-gvn-memdep=false`) builds the module in about 5 minutes here against
   our 35 to 75; they measured sailing unchanged. Cold actors at `-O0` would need checking in enemy-heavy
   scenes.

Not worth taking: their 256-cycle segments, `-O3`, ThinLTO on the module, `x86-64-v3` versus native,
per-call-site caches on indirect calls (all measured flat or worse by them), and re-running the game's
renderer for in-between frames.

## Reproducing

`build/deepsea/` holds the toolchain (`tc/`: clang copies named `x86_64-w64-mingw32-clang` with an
`x86_64-w64-windows-gnu.cfg`, and a windres wrapper), the runtime build (`rt/`), the package (`pkg/`: the
runner, `Mods/`, Dolphin's `Sys/` and `libwinpthread-1.dll`), the extracted disc (`disc/`, checked against
their 238 hashes; the apploader needs its trailer), the module (`modules/`), the user folder with our save as
`GC/USA/Card A/01-GZLE-gczelda.gci`, the Outset state (`outset.sav`) and the route (`route.txt`). The
scratchpad scripts `deepsea_run.ps1`, `deepsea_vi.py`, `compare_deepsea.ps1` and `compare_paced.ps1` ran the
tables above. Their `--uncapped` keeps 1.0x guest time and turns frame60 back on; unthrottled runs set
`EmulationSpeed = 0` in the user folder's `Dolphin.ini` instead.
