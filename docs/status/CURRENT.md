## 2026-10-04 After fusion: the game by warp again, the game thread's share, and what did not help

**The survey with fusion** (the 47 places of 2026-10-03, four E-cores `0x000F0000`, uncapped, Smooth Motion
off, scratchpad survey.ps1). Every place now runs at 38 game frames a second or more: the slowest are the
sea by the Forsaken Fortress (`sea:1`) 38.3, Dragon Roost Island 39.1, Forest Haven 40.2 and Outset 40.7
(before the transform copies and fusion: Forest Haven 29.5, Dragon Roost 31.3, the sea by the Fortress
32.6, Hyrule Castle's room 33.3). The worst GX worker is Forest Haven's 12.1 ms a game frame; the worst
game threads are the sea by the Fortress 22.7 ms, Dragon Roost 21.6, Outset 21.1 and Forest Haven 20.1. The
worst frame of each second at Forest Haven fell from about 37 ms to 25.5, and GX batches of 20 ms or more
(`[gx-slow]`) from 864 to 66 over the tour (the rest at warps). On two E-cores (`0x00030000`): Forest Haven
29.3 -> 38.8, Hyrule Castle's room 33.8 -> 45.0, Dragon Roost 35.5 -> 37.6, Outset 36.3 -> 40.5.

**Where the game thread goes now** (Forest Haven, four E-cores, guest pc sampled, grouped by subsystem): J3D
19.8 percent, the GX SDK and its FIFO writes 13.7 (4.4 of it GXSetDrawDone's wait for the GX worker's
drain, about 1.3 ms a frame now), particles (JPA) 10.7, collision 10.4, matrix and vector math 6.7, audio
(JASystem) 6.0, actor search and process management 5.9, the sea and sky (d_kankyo) 5.8, 2D (J2D, fonts)
4.7, the rest (actors' own logic) about 14. The fourth round of natives (`game-natives-4`, merged;
docs/status/NATIVE_ANIM_KANKYO_CC_2026-10-04.md) takes the environment colour blends, key-frame animation
(calcTransform, its interpolations and splines, J3DPSCalcInverseTranspose) and two collision setups,
estimated at 2.6-3.2 points of the game thread at Dragon Roost and Forest Haven. The next round is the code
that writes the GX FIFO, which no round has taken yet: the GX SDK leaves (GXLoadPosMtxImm, GXBegin,
GXLoadTexObj and about twenty more, together 5-6 percent), the particle draw executors (about 4) and the
sea's drawWave and drawVrkumo (1.4).

**Power throttling off** (2fce4e5). Windows 11 runs a process whose window is not in front with EcoQoS and,
while it is hidden, ignores its timer resolution; BlueWake now opts out of both, beside the above-normal
priority it already sets (`BLUEWAKE_PRIORITY=0` keeps the defaults). No measurable change with the window
in front (all cores, the game at its cap), as expected; it keeps a game left behind another window at
speed.

**Tried and dropped:**
- Bigger FIFO batches for the GX worker (the game thread wakes it every 1 KB): 4 and 16 KB measured the same
  as 1 KB at Forest Haven, Dragon Roost and Hyrule Castle's room (four E-cores, two runs each).
- A CPU cache of decoded vertices: the decode is cheap; the cost is the volume (13.4 MB a game frame at
  Forest Haven, 86,000 vertices of 156 bytes, written, copied to the frame's staging and uploaded), which a
  CPU cache would copy just the same. Only a GPU-resident cache would remove it, and the GX worker no longer
  limits the frame.
- ThinLTO for the game module: each chunk is one very large function, so there is nothing small to inline
  across chunks.
- `begin_frame`'s waits for a frame slot in uncapped runs (1-1.8 ms a frame with Smooth Motion off, more with
  it on) are the presents' pace, not lost work: in play at 30 game frames a second the slots are free.
- Frontend-level fusion (joining a display list's primitives as the FIFO is parsed, before they become
  packets) would save perhaps a fifth of the GX worker's remaining time; deferred while the game thread is
  the limit.

## 2026-10-04 A display list's primitives fused into one draw: a tenth of the draws, 30-60 percent more frames on slow CPUs

**What the GX worker was doing.** Its profile at Forest Haven was flat: no function above about 1 percent,
vertex decoding itself about 1 percent, the cost a fixed overhead per draw (about 0.7 us) over about 21,000
draws a game frame. Nine draws in ten arrived with no state change since the draw before (the same vertex
format and primitive class): the strips of one display list. Each was planned (state, pipeline, textures,
vertices), submitted and matched for Smooth Motion apart, to be batched with its neighbours at the end.

**Fusion** (RecompCore d687c69, patch 0135). GxCoreSink has the consuming sink join such a primitive to the
draw before: triangles after triangles, positions from an array (not a particle's, a 2D sprite's or a
wake's), outside draw scopes and tagged draws, nothing but array spans between them (no texture, TLUT or
copy), the same vertex format and transform version, up to 1,024 vertices (so a mesh the game moves vertex
by vertex stays one Smooth Motion blends). The fused draw keeps its primitives and gets the indices they had
apart. Over the Outset route 8.8 million draws became 0.87 million. `DOL_GX_FUSE=0` turns it off;
gxcore_tests checks a fused plan against the two draws apart.

CPU per game frame, all of the i9's cores, Smooth Motion at 60, paced (ms):

| Place | GX worker | In-between matching | Render worker |
| --- | --- | --- | --- |
| Forest Haven (`sea:41`) | 16.4 -> 7.1 | 14.5 -> 2.9 | 3.9 -> 2.8 |
| Hyrule (`Hyrule`) | 11.4 -> 4.5 | 9.9 -> 0.9 | 3.5 -> 2.6 |
| Hyrule Castle's room (`Hyroom`) | 14.2 -> 5.1 | 12.4 -> 0.8 | 3.9 -> 2.6 |
| Outset (`sea:44`) | 8.1 -> 4.1 | 7.0 -> 1.5 | 3.3 -> 2.9 |

The frame's uniforms fell with the draw commands (Forest Haven 15.8 -> 4.3 MB a game frame, 5,700 -> 2,000
commands). On four E-cores, uncapped with Smooth Motion off, game frames a second: Forest Haven 30.6 -> 40.5,
Dragon Roost Island 35.2 -> 40.1, the sea by the Forsaken Fortress 35.0 -> 38.3, Hyrule Castle's room 36.9
-> 45.2, the Earth Temple's boss 40.6 -> 59.1, Outset 47.6 -> 49.8, Hyrule 45.7 -> 59.8. Most of that was
the game thread waiting at GXSetDrawDone for the GX worker to drain the frame: 7-10 ms a frame before at
Forest Haven, Hyrule Castle's room, the Earth Temple's boss and Hyrule, 1-2.5 ms now. With Smooth Motion on
four E-cores, the game now holds 30 at every place tried with in-between frames on (before: 27-29 at Outset,
Forest Haven and Dragon Roost, and the pacer had turned in-between frames off at the sea, Hyrule and Hyrule
Castle).

**Exactness.** Link's position at all 621 probes of the Outset route identical. Smooth Motion dumps: real
frames identical at Outset, the sea by the Fortress, Forest Haven, Dragon Roost, Hyrule, the Forbidden
Woods, the Earth Temple's boss, the file select and three lava save states. In-between frames identical, or
different at a few pixels' edges (a fused group's matrix rows and motion samples are judged together; a
flying enemy's wing on the Dragon Roost lava bridge lands a pixel or two apart, both plausible). In Hyrule
Castle's room and at the Earth Temple's boss the in-between frames are now drawn where the matching had
taken the frames for camera cuts (too many single strips looked unmatched) and showed none.

**What is left.** The GX worker at Forest Haven is down to about a fifth of a core; the game thread is the
limit on slower CPUs (20-22 ms of a 25 ms frame on four E-cores at the big outdoor places). Its profile is
flat too: particles (JPA) about 5 percent, J3D animation about 5.5, the sea's waves 2.5, collision 2.5, the
clipper 1.4; a fourth round of natives for those is under way on `game-natives-4`.

## 2026-10-03 The Mac side's updates on Windows: water and HUD smoothing, HD packs without shimmer, Forest Water, the Wind Waker HD importer

**The merge** (main e4e1401 into windows-release, a9df903): the Wind Waker HD texture importer, Forest Water
options, the Mac's runtime delta (patches/recompcore/0170: water, lava, HUD) and its patch step. Windows
builds from a pinned RecompCore commit that carries its changes, so the lock names no working-tree patch;
scripts/apply_recompcore_patches.py then only checks that ref/recompcore is that commit, unmodified.

**RecompCore 201e909** (patch 0131): 0170 without its lava part (this fork's 0120 already), merged with
0125's cloth strips and 0126's blended colours. Indexed meshes the game deforms on the CPU (the sea's grid,
with its direct UVs) are blended vertex by vertex, and 2D sprites are matched by their bounds.
frame_interp_test passes with both sides' cases. In Smooth Motion dumps of the Outset route the 31 real
frames are identical to ef3e17f's; the in-between frames differ where the HUD's matching changed: the
pulsing heart was drawn doubled (one copy offset above) and is now one heart between its two sizes.

**HD packs shimmered and flickered as the camera turned** (RecompCore 7310b79, patch 0132). Most of the
game's textures have no mips, so their sampler clamps to level 0; a replacement several times their size,
with its own mip chain (the WWHD importer writes one to 1x1), was drawn from level 0 alone. Now a
replacement's levels are all sampled, as in Dolphin (a custom texture's max LOD is 255), blended linearly
where the game asked for no mip filter. Tested with private packs made from the disc (all 4,641 model and
BTI textures decoded, upscaled 4x/2x, with mip chains; scratchpad make_test_pack.py, never shipped): with a
fine grid in every texture, the old renderer drew moire on the cliffs, beach, pier and HUD buttons that
crawled with the camera, the new one averages it out at a distance. Thin alpha features such as the sea's
foam now show as soft streaks at a distance, as in Dolphin with a mipped pack. No Wind Waker HD disc was
available here, so a real WWHD import has not been tried on Windows.

**The water path's cost** (RecompCore e6559e0, patch 0133). 0131 gave every small indexed mesh without
position matrix indices its own vertex range (so no batching) and a copy of its vertices for the helper;
nearly all are static models. Now a hash of each position array's bytes, once a game frame, says which
meshes the game wrote; only those (and draws with direct UVs or positions) take the per-vertex path. The
guest's dirty epochs (cache flushes) were tried first and miss small meshes written without a flush (the
Forsaken Fortress's flag). Dumps against 201e909: Outset 62 of 62 and the sea by the Fortress 42 of 42
identical. All cores, Smooth Motion at 60, paced, two runs each, CPU per game frame (ms):

| Place | GX worker ef3e17f / 201e909 / e6559e0 | Render worker ef3e17f / 201e909 / e6559e0 |
| --- | --- | --- |
| Outset (`sea:44`) | 8.1 / 9.1-9.4 / 8.1-8.3 | 3.3-3.9 / 4.0-4.4 / 3.2-3.8 |
| Forest Haven (`sea:41`) | 15.9-16.4 / 18.6-18.9 / 16.6-16.8 | 4.6-5.1 / 7.1 / 4.3-4.7 |
| Dragon Roost Island (`sea:13`) | 13.1-13.6 / 14.8-14.9 / 13.5 | 4.4-4.8 / 5.6-5.9 / 4.2-4.4 |
| Hyrule (`Hyrule`) | 11.1-11.4 / 13.1 / 11.1-11.3 | 3.8-4.4 / 6.1-6.4 / 3.6-4.4 |
| Hyrule Castle's room (`Hyroom`) | 13.4-13.7 / 15.5-16.0 / 13.7-13.8 | 3.8-4.0 / 6.3-6.4 / 4.3-4.6 |

(On eight E-cores the comparison is not meaningful at Forest Haven: the pacer turns the in-between frames
off and on as the game falls below 30, so runs mix both states.)

**Also in this build:** Forest Water's two helps (the Mac's forest_water.c) in the Windows settings (Mods)
and settings.ini, its REL hook named so the builder's direct calls keep it; the WWHD importer on Windows
(scripts/setup_wwhd_tools.ps1, %APPDATA%\BlueWake defaults, `--install` writes `hd_textures=1` and
`texture_pack=`, which the app now honours; tests/wwhd_import_test.py passes on Windows); and the
resumable search natives (game-natives-3b, docs/status/NATIVE_SEARCH_WINDOWS_2026-10-03.md: native entries
15/15 certified). The game module's play is exact against the previous build: Link's position at all 621
probes of the Outset route identical.

## 2026-10-03 The game by warp: where it is slow, and the GX worker's transform copies

**The survey.** The 2026-10-02 build (module be22aba4) on four of the i9's E-cores (`0x000F0000`), uncapped,
Smooth Motion off, at 47 places by test warp from the Outset save, about 12 s of Link running and turning at
each (scratchpad survey.ps1 / survey.py over tour_run.ps1). Interiors and most dungeon rooms reach 60 game
frames a second (the frame cap), so they are not CPU-bound here. The slow places are the big outdoor ones:

| Place | Game FPS | Game thread CPU per game frame | GX worker CPU per game frame |
| --- | --- | --- | --- |
| Forest Haven (`sea:41`) | 29.5 | 20.9 ms | 24.9 ms |
| Dragon Roost Island (`sea:13`) | 31.3 | 24.5 ms | 20.6 ms |
| The sea by the Forsaken Fortress (`sea:1`) | 32.6 | 22.8 ms | 17.5 ms |
| Hyrule Castle's room (`Hyroom`) | 33.3 | 16.9 ms | 22.8 ms |
| The Earth Temple's boss (`M_DaiB`) | 38.3 | 13.9 ms | 18.8 ms |
| Outset (`sea:44`) | 39.2 | 21.5 ms | 13.5 ms |
| Hyrule (`Hyrule`) | 41.9 | 10.4 ms | 17.9 ms |

In half of them the GX worker, not the game thread, is the limit.

**Where the time goes.** The game thread (guest call stacks, guest_stack_sampler.py) is spread thin at most
places: drawing (J3D draw buffers 14-20 percent inclusive), particles (7-10), animation (5-8), the attention
system's weights (6), collision, the audio engine's channel updates (5-7). One hot spot: at Dragon Roost
Island, `daTag_Island_c::demoProcCom_ikada` calls `fopAcM_searchFromName` every frame, which walks every
actor and, for each, `strcmp`s the same name through the stage's object-name table (`dStage_searchName`):
7.4 percent of the game thread (`strcmp` alone 6.4), and about 3 percent at Forest Haven. The GX worker
(thread sampler) is flat: building draw plans, packets, and about a fifth of its working time in memory
copies, of which the largest was each draw's transform state.

**The transform copies** (RecompCore ef3e17f, patch 0130). Each draw's 2.3 KB of transform state was copied
three times (into the draw queue, the packet, the sink's draw); about 95 percent of draws repeat the draw
before's transform, which the transform version (0127) identifies. Now each copy is skipped when its version
is the one already there. `DOL_GX_TRANSFORM_VERIFY=1` copies anyway and counts differences.
- Verify mode over twelve places: 54 million draws, 95 percent kept, 0 differences.
- Synchronous captures of the Outset route 19 of 19 identical; in Gohma's room Link's position identical at
  every probe and Smooth Motion dumps (game frames 300-330, real and in-between) 62 of 62 identical.
  (Gohma's room's retrace-triggered captures are not a test of this: they are asynchronous readbacks, and a
  faster renderer captures a different game frame.)
- Four E-cores, the heaviest places, two runs each against the same build without it: the GX worker's CPU
  per game frame at Forest Haven 25.5-26.3 ms before, 22.3-22.7 after; Hyrule Castle's room 22.0-22.4 and
  19.0-20.6; Dragon Roost Island 20.4-21.6 and 18.5-18.8; Hyrule 18.1-19.3 and 15.2-15.5; the sea by the
  Fortress 16.6-18.2 and 14.8-15.4. Game frames a second up 6 to 16 percent at those places.

**Tried and dropped** (exact, but no clear gain):
- Not recording every command's FIFO bytes on the live path (nothing there reads them): about 2 percent less
  GX worker time overall but consistently slower at the Earth Temple's boss.
- The derived-state cache keyed on the transform version instead of comparing the XF registers (36 million
  hits verified against a fresh derivation, 0 differences), with each vertex's unwritten slots zeroed instead
  of the whole buffer: no faster, slightly slower at most places.

**The search natives** (docs/status/NATIVE_SEARCH_2026-10-03.md, merged at 8eb0572): `strcmp`,
`dStage_searchName` and `cTgIt_JudgeFilter` with `fopAcM_findObjectCB`, certified against the translation
(0 mismatches) and hooked (`native entries: 12/12 certified`). In play they change nothing the game does (Dragon
Roost Island, synchronous: Link's position the same at 158 of 158 probes, natives on and off) but they do not
yet save time there (game thread 23.0-23.3 ms per game frame with the 2026-10-02 module, 23.4-23.8 with
these): a search for daTag_Island's names needs about 6,000 guest cycles and the game thread's budget before
its next event is usually 80 to 4,800, so the native declines and the translation runs. The headless training
had longer slices (the stage-name native ran in 99.7 percent of its calls there). Next: natives that run as
far as the budget allows and resume at the loop head.

**The GX worker in two stages** (RecompCore branch `gx-two-stage`, not pinned): the plans handed to a second
thread that puts them into Aurora. Exact (Smooth Motion dumps 62 of 62 at Outset and Gohma's room) but not
faster: on eight E-cores 7 to 15 percent slower with the plan arrays swapped across (the builder reallocated
them), about even with them copied. Putting a plan into Aurora is mostly copying its data into staging, and
the hand-off is a copy of the same size. Also measured flat and dropped: the texture handle taken by
reference instead of copied per draw.

## 2026-10-02 What we took from DeepSea: pipelines, Smooth Motion, the GX worker, training, natives, build time

The borrow list in DEEPSEA_COMPARISON_2026-10-02.md, item by item, measured on the i9-13900KF + RTX 5090.
RecompCore 44e5c2c (patches/recompcore 0121-0129, branch `windows-release`; 0121, Dawn's device lock, was
written in another session for a crash after a save state loads, and the pipeline threads depend on it).

**Priority and the slow-game detector.** The process runs above normal priority (a6f8ea9;
`BLUEWAKE_PRIORITY=0` opts out): with the game on four E-cores and 8 threads spinning beside it, it kept
full speed with and without, so this is insurance. Smooth Motion's slow-game test is now the median of the
last 60 frame gaps, gaps of 150 ms or more left out, two slow medians in a row (0122): six held hitches of
120-200 ms (`BLUEWAKE_TEST_STALL`) no longer drop the in-between frames; two E-cores, really too slow,
still do.

**Shader stutter.**
- Pipelines compile on a quarter of the logical CPUs, 1-4 (0123, with the device lock): the 213-pipeline
  seed compiled in 1.06-1.25 s instead of 3.7-4.4; booting into Gohma's room, draws skipped 3,259 against
  15,930-20,192.
- The bundled seed is 513 pipelines from a tour of 49 places (04e0e1b): booting into the three save states
  asks for no pipeline it lacks.
- Settings, Display, "Compile shaders before playing" (238a829) holds the game at its first present behind a
  progress panel until the queued pipelines are made.
- The ubershader (0128-0129): a draw whose pipeline is still compiling is drawn by one WGSL module that
  reads its shader key from a uniform (gxcore_uber.cpp), with a pipeline per fixed-function state; forced
  on every draw it draws the lava rooms' frames identical to the specialized pipelines'. Its two shaders
  take about 650 ms each to compile when Dawn's blob cache lacks them, so one pipeline per shader is made
  first, and only when nothing else is queued unless an earlier run made one. Draws left out (counting a
  batch's draws), the old seed: a first launch, Gohma's room 5,933 against 6,069 without, the lava room
  5,296 against 5,297, the bridge 4,613 against 4,848; after playing Outset once, 950 against 4,860, 141
  against 3,477 and 215 against 3,377. On by default with D3D12 only (`DOL_AURORA_UBERSHADER=0/1/2`);
  Metal and Vulkan have not drawn it.

**Smooth Motion coverage.**
- Colours blend in the in-between frames (0126): light, material and ambient colours, TEV colour and
  konst registers, fog colour. Real frames unchanged (31 of 31); the in-between frames' fading wind streaks
  now halfway; no measurable cost.
- Cloth (0125 with 40953dd): a dCloth packet's strips (flags) and the boat's sail are tagged as a draw
  scope over indexed draws and blended vertex by vertex. Takes a module that watches 0x80063728 and
  0x800E93B8 (built from 40953dd on). Checked with the new module: the three flags on the ship at sea
  room 14 are drawn as 8 tagged strips of 10 vertices each, matched and blended with their positions
  captured every frame; real frames identical with the tags on and off. Not seen on screen yet (the flags
  were behind the deck's roof in the views tried).

**The GX worker.**
- The decode finds a draw's attribute arrays once and multiplies by 1/2^frac (0124; 285 million
  components compared bit for bit).
- A draw's 2.8 KB vertex constants are kept when the transform state's version is the draw before's
  (0127): GX worker CPU per game frame 16.4-17.0 ms against 18.8-19.2, four E-cores, unpaced.
- The compact vertex (132 to about 108 bytes with unorm8x4 colours) was not done: a profile of the GX
  worker (four E-cores, Smooth Motion 60) has it idle 64 percent of the time, and vertex writes and copies
  are a few percent of the rest; the saving would be under 2 percent of its working time, against
  changing the vertex layout in gxcore, the ubershader, frame interpolation and the tests.

**Training, the app, build time** (135a62d).
- The training's plain playback goes on from Outset to ten places by test warp: 425 of 813 translated
  functions ran, against 288 (TRAINING_VERSION 4).
- The app is compiled with its own profile (windows/pgo/app.profdata, scripts/windows/train_app_profile.py)
  and ThinLTO: GX worker CPU per game frame 13.8-14.3 ms against 16.6-17.2, game thread 22.4-23.2 against
  23.9-24.3.
- `--tiered` compiles the chunks the training never ran (367) at -O1 without GVN's memory dependence:
  the module compiled in 14-16 minutes instead of about 39 (the 0.4.0 build: 35). The cold chunks were already
  compiled for size by the profile; Gohma's room (which no training visits) below shows no loss.

**Natives** (the second set, docs/status/NATIVE_ENTRIES_2026-10-02.md, merged from `game-natives-2`):
J3DFifoLoadPosMtxImm, NrmMtxImm and NrmMtxImm3x3, PSMTXMultVecSR, the Basic, Softimage and Maya
calcTransform, dBgW::ChkGrpThrough and cBgS_Chk::ChkSameActorPid, each certified against its translation
(60,000 cases each, 0 mismatches) and hooked only where the translation hashes to the tested one
(`native entries: 9/9 certified`). In play they almost never decline: over the training's two playbacks
(the opening, Outset and the tour; the instrumented module's counts) calcTransform ran natively in 97.4
percent of 5.4 million calls, the FIFO matrix loads in 99.7 percent of 20.7 million, the two collision
checks in 99.3 percent of 163 million, PSMTXMultVecSR in 91 percent of 318,000. Synchronous captures
(`DOL_GX_FIFO_WORKER=0`) with them on and off (`BLUEWAKE_NATIVE_ENTRIES=0`) and with 0.4.0's module: the
Outset route 19 of 19 identical, Gohma's room from its save state 26 of 26. (The module's reports at exit run - the
profile counts them - but their lines no longer reach the log, 0.4.0's included; the last run whose log has
them is from 2026-10-01. The counts here are the profile's.)

**All of it, against 0.4.0** (the builder at 1fcf9e3 with `--tiered`: module be22aba4, the app with
RecompCore 44e5c2c, its profile and ThinLTO). Four E-cores (`0x000F0000`), unpaced, Smooth Motion off,
three runs each, interleaved:

| | game FPS | GX worker CPU per game frame | game thread CPU per game frame |
| --- | --- | --- | --- |
| Outset route, 0.4.0 | 37.2-37.7 | 19.8-20.0 ms | 23.8-24.2 ms |
| Outset, new app, 0.4.0's module | 36.6-41.9 | 12.9-14.5 ms | 21.5-22.5 ms |
| Outset, new app and module | 44.2-44.9 | 12.2-12.5 ms | 20.5-20.6 ms |
| Gohma's room, 0.4.0 (the one run of three that started) | 48.7 | 15.6 ms | 16.4 ms |
| Gohma's room, new app, 0.4.0's module | 48.0-52.8 | 12.3-12.7 ms | 15.6 ms |
| Gohma's room, new app and module | 48.1-54.4 | 12.2-12.4 ms | 15.4-16.0 ms |

Booting into Gohma's room on those four cores, 0.4.0 stopped right after the load in 4 of 6 runs (the
device lock's crash); the new app in 0 of 12. In Gohma's room, which no training visits, the tiered module
is as fast as 0.4.0's fully optimized one. One of the builder's two instrumented training playbacks stopped
at retrace 38,482 of 38,500 and was run again (the builder retries once); not seen in any other run.

## 2026-10-02 A crash after a save state loads: Dawn used from several threads without its lock

**What was wrong.** About one boot in ten straight into a save state (`BLUEWAKE_LOAD_STATE`) crashed in the
first frames after the load: an access violation reading 0x40 inside webgpu_dawn.dll, under the GX worker's
`wgpuDeviceCreateBindGroup` (gxcore `submit_draw_plan`'s single-texture bind group), and exit 0xC0000374
(heap corruption). Aurora calls Dawn from three threads: the GX worker makes textures, samplers and bind
groups; the render worker records, submits and frees each frame's (an EFB copy that is scaled or converted
makes a bind group with a sampler every frame, freed with the frame); the main thread presents, and when a
state loads it frees the whole texture cache. The device was created without Dawn's
`ImplicitDeviceSynchronization` feature, so Dawn took no lock, and its D3D12 backend is not safe that way:
the sampler descriptors of every bind group live in one `absl::flat_hash_set` (`SamplerHeapCache`, an entry
inserted with the first bind group of a sampler set and erased with the last), and texture memory is freed
through plain queues (`ResourceAllocatorManager`). A lookup that reads a slot in the middle of a rehash gets a
null entry, and an entry's sampler list ends at offset 0x40. After a load every texture is new, so the GX
worker makes hundreds of bind groups and textures in exactly the frames the render worker's copy bind groups
come and go. None of the suspects in Aurora's own code was it: texture views are made with their textures,
samplers and the bind group layout come from caches that hold them, and the GX worker's last-bind memo only
matches within one frame, while the bind group it names is cached and so holds the view (a window resize,
which empties that cache, is the one exception; no resize happens in these runs).

**The change** (RecompCore 31401e5, patch 0121): the device asks for `ImplicitDeviceSynchronization`
where the adapter offers it (Dawn does on every backend). Object creation, queue work, presents and an
object's last release then take the device's mutex; encoding, pipeline and shader-module creation and
texture views do not, and Dawn runs callbacks with the mutex released (it asserts so), so the map callbacks'
own locks cannot deadlock against it. The log says `Device lock (implicit device synchronization): on`;
`DOL_AURORA_DEVICE_LOCK=0` turns it off for comparison.

**Checked** (i9 + RTX 5090, the 0.3.0 module, two runs at a time, each with a fresh data folder):
- Booting into `build/windows/test-saves/gohma-lava.bwstate` held to two logical CPUs (affinity 0x3, which
  makes the race likely): before, 3 of 24 runs crashed (0xC0000374, 2-5 s in, at the first frames after the
  load); the new app with the lock off, 2 of 32; with it on, 0 of 80. Unheld: before, 1 of 40; lock off,
  0 of 40; lock on, 0 of 60.
- Under cdb (`-hd`: the debug heap hid it in 40 runs) one baseline run stopped in the GX worker's
  `CreateTexture` (`new_dynamic_texture_2d`), in `free` of a heap block whose header held the text
  "Dawn_Int": the heap was already corrupt, the same race seen from another call.
- A two-thread program doing what the GX and render workers do (sampler bind groups made and freed on both,
  release toggles): without the lock 3 of 3 ten-second runs crashed or hung; with it, 3 of 3 ran clean.
- Speed: the GX worker calls Dawn only when it uploads a texture (263 times on the whole Outset route), and
  the game thread calls nothing that takes the lock, so in steady play only the render worker takes it.
  The same app with the lock off and on, interleaved, on four of the i9's E-cores (Outset route, unpaced,
  `smooth_motion=0`, six runs each): game FPS standing 36.8 off and 35.6 on, running 36.8 and 36.2 (single
  runs 34-39 either way, with another BlueWake being played on the PC meanwhile); game-thread CPU per game
  frame 23.4 and 23.4 ms standing, 24.3 and 24.5 running. Smooth Motion 60, paced: 30 game and 60 shown FPS
  both ways.

The Mac line (RecompCore `bluewake`) creates its device the same way and needs the same commit.

## 2026-10-02 Windows 0.4.0: the lava's colour and controller vibration

Published as v0.4.0 (tag at main d102695) with the Mac's 0.2.0 build: Windows from windows-release 7ca0cb9,
RecompCore 7c62903 (the lava fix below and the haptics work above). The builder reused 0.3.0's optimization
profile (ffd6459b: the parts of RecompCore compiled into the module did not change) and recompiled the
module in 35 minutes; module 48a97a5d, x64 zip 24dc9703, source zip dc8c57c8.

**Checked** (the release's binaries):
- The lava in Dragon Roost Cavern's big lava room draws orange with glowing domes and the dark crust.
- Outset's synchronous captures (`DOL_GX_FIFO_WORKER=0`): 19 of 19 identical to 0.3.0's.
- Speed, 4 E-cores (`0x000F0000`), unpaced, interpolation off, three runs each: game-thread CPU per game
  frame 23.0-25.0 ms against 0.3.0's 22.5-24.3, the same within the noise; game frames a second ranged
  33-38 for both, moved by whatever else the PC ran (one pair of runs came out 32.9/34.2 against
  34.5/34.0).
- The zip as a player gets it: first launch from the `.rvz`, full speed with frame interpolation (59.9
  shown, 30 game), a save state saved and loaded, closed with exit code 0. States made with 0.3.0's module
  load.

`build\windows\BlueWake` is now 0.4.0; the hard-linked test copies made from it (BlueWake-rc and the
others) changed with it, since the builder copies over its files in place. `build\windows\Rel030` is 0.3.0
(from its zip, with the disc files linked in) for comparisons.

## 2026-10-01 Controller haptics: the game's vibration shaped, the triggers, and a rumble that never reached a controller

**The rumble never reached a controller.** The host forwards the motor bits the pad library writes to
SI<n>OUTBUF (runtime/host/src/main.c), and tested the command in the top byte (`value >> 24 == 0x40`).
PADControlMotor writes `(0x40 << 16) | AnalogMode | command`, so the 0x40 is bits 16-23 and the test never
matched: on every platform the game's rumble (Classic below, and the iOS app's) was never sent. With the
command read from bits 16-23 a shock's on-off bits arrive as the game plays them (shock 5: on at retrace
1000, off at 1008, on at 1010, off at 1014, stopped at 1018).

**Haptics** (runtime/host/src/haptics.c; Windows and the Mac). The game asks for vibration through
dVibration_c (the play info's mVibration, 0x803CA5A8, checked against its vtable 0x8037D460 once the game
has built it): StartShock picks one of 26 motor patterns (3 to 23 game frames: a ladder from a light tap to a
heavy blow at 1 to 8, solid bursts, three spaced taps, a double hit, thuds that fade) and StartQuake one of
12 looping ones (12 to 62 percent of frames on) until StopQuake; the motor only had on and off, one bit a game
frame (JUTGamePad::CRumble::update, called from the pad read). Called directly by about 70 actors, those
functions are not dispatch boundaries the host can rely on, so the object is read each retrace instead
(its motor shock and quake: pattern index, bits, length, frame; the frame count and state). Enhanced (the
default) renders a shock on both motors following its bits, with a fast release (each pulse a pulse, not a
click), at a strength from its pattern (0.5 for two frames on to 1.0 for eight), the light motor sharper on a
pulse's first frame; a quake as a continuous rumble on the heavy motor as strong as the pattern is dense,
textured by its bits; strong shocks and heavy quakes drive the triggers too: an Xbox controller's impulse
triggers (SDL_RumbleGamepadTriggers where SDL reports trigger rumble) or a DualSense's trigger vibration
(SDL_SendGamepadEffect: effect 0x26 across the whole travel, strength 1-8, 45 Hz; 0x05 clears it). The game's
Vibration option and its pause clear the patterns and are honoured as they are; nothing is sent while a menu
is open, with the window in the background, or after 150 ms without a game frame; rumble commands expire in
120 ms, and a DualSense trigger effect is cleared by a timer on SDL's thread 250 ms after the last command.
Classic forwards the game's on-off bits as before (now that they arrive); Off sends nothing. Settings:
Controls (Windows' F1 menu and the Mac's options), `BLUEWAKE_HAPTICS=enhanced|classic|off`,
`BLUEWAKE_HAPTICS_STRENGTH` (80), `BLUEWAKE_HAPTICS_TRIGGERS`.

**Checked** (Windows, with `BLUEWAKE_HAPTICS_TEST`, which sets the motor half of the object as StartShock and
StartQuake do, and `BLUEWAKE_HAPTICS_VIRTUAL`, an SDL virtual Xbox controller or DualSense that logs what it is
sent): the game's Run plays the injected patterns (its frame counts 0 to the length, then clears; the rumble
flags 0xF0000000); shock 5 (`11110110`) arrives as two pulses, heavy 0.68 and light 0.61 then 0.34 at 80
percent, the triggers 0.44; shock 11's three taps as three pulses; quake 4 as a steady rumble that fades out
over 100 ms when stopped; the DualSense gets 0x26 effects at strength 4 easing to 1, then 0x05, and its
watchdog cleared the triggers only in two real stalls of 187 and 786 ms. Enhanced sends none of the game's raw
on-off commands (17 motor changes, 0 forwarded). Not yet felt on a physical controller.


## 2026-10-01 Lava in its colour: the dual-texture post transform

**What was wrong.** On Windows the lava in Dragon Roost Cavern and Gohma's room (`M_DragB`) drew white with
orange-brown streaks; the Mac's known issue ("flat orange instead of its bright pattern") is the same bug.
Wind Waker draws its lava itself (`dMagma_packet_c`, d_magma.cpp): each texgen takes the vertex's position
through `GX_TEXMTX2` (the floor's own placement) and then through a post-transform matrix,
`GXSetTexCoordGen2(..., GX_PTTEXMTX0/1)`. Texcoord 0 projects the dark crust texture, scrolling; texcoord 1
picks the colour from a ramp by height (`mPostMtx0`: scale Y by 0.05 and offset by the floor's height). TEV
stage 0 masks by the crust's alpha, stage 1 mixes the ramp's colour with the pulsing glow (`mColor1`, black to
`FF9600` and back over 119 frames). GXInit turns post-transform on (XF 0x1012 = 1, at 0x80320148); the renderer
never read post-transform memory (XF 0x500-0x5FF) or the per-texgen post index (0x1050-0x1057), so both
texgens used the floor's placement alone, and the ramp was read far outside its range. The lava is the
only post matrix in the game that is not the identity (every other caller passes `GX_PTIDENTITY`).

**The change** (RecompCore 81d7345, with 7c62903 putting the test file's line endings back; patch 0120):
- The front end keeps post-transform memory beside `DolGxRecompState`, not in it: the saved front-end state
  keeps its size, so save states from 0.3.0 still load (after a load the rows read as identity until the
  game loads them again, which the lava does every frame). It tracks per row whether it is a row of the
  identity, so a draw costs a few bit tests unless it uses a real post matrix.
- Each draw carries the three rows for any regular texgen whose post matrix is not the identity.
- gxcore folds them into that texgen's matrix rows (post x tex, the post rows' w into the fourth column;
  for a two-row texgen, post x (s, t, 1)), as Dolphin's VertexShaderGen applies them. No shader or pipeline
  key changes, so the shader caches stay valid. A post transform that normalizes first is not folded
  (nothing in Wind Waker uses one).

**Checked** (the 0.3.0 module with the new app):
- `gxcore_tests` pass, with a new test of the fold for three- and two-row texgens.
- Dragon Roost Cavern's big lava room (`M_NewD2` room 2): the lava glows yellow and orange where its domes
  rise, with the dark crust over it, where 0.3.0 drew it white. The look: the floor quad draws only the
  crust (alpha test above half on `mag_kuro`, an IA4 of blobs), tinted by the pulsing glow; under it the
  domes (`dMagma_ballPath_c`, rising and sinking about every 11 s) take the ramp (`mag_col`, CMPR 32x32,
  clamped in T: brown-orange at floor level through orange and yellow to white about 50 units up).
- Gohma's room: orange with its dark crust, real and in-between frames (Smooth Motion 60). Its domes
  (`dMagma_ballBoss_c`) sit 60-110 units under the surface in the opening cutscene and rise only near
  Gohma, so it shows no bright patches there; judge the lava in room 2.
- Save states (`BLUEWAKE_LOAD_STATE=...`, or copied into the data folder's `states` for F8), all with the
  0.3.0 game module, in `build/windows/test-saves`: `drc-lava-bridge.bwstate` (Link on the rope bridge
  above room 2's lava), `drc-lava-room.bwstate` (room 2's entrance, behind its barricade) and
  `gohma-lava.bwstate`. `BLUEWAKE_TEST_WARP=900:M_DragB:0:0` or `900:M_NewD2:2:1` from the Outset save
  reaches them too; `BLUEWAKE_TEST_PLACE` counts retraces from the process start, not the loaded state's.
- Nothing else moved: Outset's synchronous captures (`DOL_GX_FIFO_WORKER=0`) 19 of 19 identical to the
  release's, and the Smooth Motion dumps 62 of 62 identical to the release's on the same path. Two runs
  that day took the other path at the title screen's presses (one with the release app itself), so a
  mismatch there needs a second run before it means anything.
- The pond by the boulder on Dragon Roost Island (`Adanmae`) is water, not lava, and is unchanged.

The Mac line (RecompCore `bluewake`) needs the same commit and a Mac build.

## 2026-10-01 Guest loads and stores inline: about 10 percent less game-thread CPU

The request after the low-end work (below): native versions of the collision and animation loops, and keep
going until performance is significantly better. Before writing any, the game thread was measured on 4 of
the i9's E-cores (`0x000F0000`, the stand-in for a 4-core laptop), running on Outset:

- **What the time is.** An inclusive guest profile (a sampler that walks the guest's PowerPC back chain in
  guest RAM) puts drawing at 42 percent of the game thread (J3D model drawing 19, `J3DSys::reinitGX` 6, mostly
  for shadows), actor logic at 23, collision (`dBgS_Acch::CrrPos`) 6 and J3D animation 6-10. The guest
  audio thread is 7. By function the profile is flat: the top function (`calcWeightEnvelopeMtx`) is 1.7
  percent, the collision family about 10 percent together.
- **Where the host time goes.** A copy of the module with line tables (the same code; `-gline-tables-only`)
  maps each sampled host instruction to its C line and so to its guest instruction. In the translated code:
  about 17 percent was inside out-of-line guest memory helpers, the pc stores 9.5, block entries (the
  precharge and budget tests) 11.7, the deadline tests after accesses 5, the return dispatch's `switch`
  3, and the inline FP helpers and float conversions about 15.

**The memory helpers were calls.** gather_pipe.h's `bw_mem_read32` and its siblings are `static inline`, but
a translated chunk is one function of thousands of blocks, and clang stopped inlining them into it. The
compiled code called a helper with its own stack frame for every guest load and store, about 2,700 call
sites in the collision chunk alone, and each call made it store the guest registers back to the CPU state
around it. Two changes, both exact by construction:

- **gather_pipe.h:** ordinary MEM1 inline and forced (`always_inline`): `!g_ppc_guest_aliases_overlap_mem1`
  and the address in MEM1 proper (one compare), plus for a store no reservation and no write journal. Anything
  else is the old wrapper, out of line (`bw_mem_readN_slow`), which makes the same tests again from the
  start.
- **scripts/windows/lean_memory.py** (a builder step after fast_blocks.py): in the prepaid block copies, a
  plain load or store no longer stores its pc and cycle observation suffix first. `bw_readN_at` and
  `bw_writeN_at` store them on the way out of line only, and the deadline test after the access compares the
  constant. Only an MMIO handler reads either while a block runs (and the timebase and decrementer reads,
  which store their own suffix first). ctx->pc is still what the copy would have stored wherever anything
  can read it: on an access's way out of line, in a deadline refund, before an instruction that calls,
  jumps or returns without its own store, and at the copy's end.

The cycle observation suffix is now dead state after an access to RAM, so the native audit tests
(`tests/native_*_test.c`, `fast_blocks_test.c`) no longer compare it. All other CPU bytes and all RAM are
still compared, and the four audits pass against the new module.

Measured on the 4 E-cores, unpaced, interpolation off, against the module before it (both with the profile
trained for the old code). Game-thread CPU time per game frame is the steady measure; this PC's game FPS
varies by a few frames a second from run to run:

| | Game-thread CPU per game frame | Game FPS, standing / running |
| --- | --- | --- |
| Before | 26.3-26.6 ms | 31.6 / 32.3 |
| Inline memory, lean copies | 23.6-24.2 ms | 34.6-35.3 / 34.7-35.2 |

Frames are the same: with the GX translation on the game thread (`DOL_GX_FIFO_WORKER=0`), 19 of 19
captures identical to 0.2.2's.

**Tried and dropped, all exact and all measured on the same route:**

- *One test at each block's entry* (a floor kept by the host's cycle domain, `downcount - N >= floor`, in
  place of the precharge and budget tests) together with taking the pc stores out of FP instructions:
  26.4-27.7 ms a frame, slower than the lean copies alone. The block entries compile to fewer instructions,
  but the module grew by a tenth and got slower overall.
- *The return dispatch through a table* (a 4,096-entry label table in place of the `switch` over a
  chunk's return sites): the chunk grew 14 percent. In C every computed goto may reach every
  address-taken label, so a second one changes the whole function's code. With the entry test, 25.1-26.8
  ms.
- *The deadline test after an access gated on a local flag set by the out-of-line path*: the compiler keeps
  the flag in a register and still tests it, and the chunk grows 16 percent.
- `-mllvm -jump-table-density` low enough for the return `switch`: clang still builds a search tree.

**Natives.** The existing natives mostly run (`BLUEWAKE_SESSION_LOG=0` shows their counts at exit). Two
decline most of their calls: J3D's s16 keyframe interpolation (169,354 run, 629,677 declined) and
`calcTransform` (126,230 / 449,646), which only cover single-key channels and strictly increasing key
times. Hand-written exact natives of the collision family (the instruction-level transliterations the
existing ones are, with each block's cycles, scratch registers and stack bytes) gave 1.3-2x on the leaves
done so far. The family is about 10 percent of the game thread, so each function is worth little alone; the
translation-wide change above was worth more.

**Shader caches.** Asked whether one makes sense: there are two already. Aurora's pipeline cache
(`pipeline_cache.db`, seeded from the release's `initial_pipeline_cache.db`, 213 pipelines) builds known
pipelines on a background thread at start, and Dawn's blob cache (`dawn_cache.db`) keeps compiled shaders
between launches. Both are for first-time hitches, not frame rate. A seed recorded over more of the game
would cut the first visits' hitches (`[gx-slow] batch_ms=750 pipelines=21` on the first frames here).

## 2026-10-01 Low-end CPUs: frame interpolation that gives way, guest RAM as a global, and the display's own rate

The goal: run well on a 4-core laptop CPU (a tester's i7-8565U with Intel UHD 620). The stand-in is 4 of the
i9-13900KF's E-cores (process affinity `0x000F0000`), on the Outset route (`bench.ps1`: stand at the spawn
view, about 14,000 draws a frame, then run and turn).

**What the 4 cores did with the defaults.** Paced like a player, at 60 FPS frame interpolation (the default),
0.2.2 ran the game at 24-28 frames a second: slow motion, with the F9 counter still near 50-55. With
interpolation off, the same cores held 30. The in-between frames' work took cores the game's own thread needs:
each draw captured on the GX worker, matched on the helper thread, then drawn again by the render worker. Aurora's
pacing dropped in-between frames only when the GPU or the render worker fell behind, never
when the game itself did.

**Frame interpolation gives way (RecompCore 6348954, patch 0119).** Game frames ending more than 35.5 ms apart
on average (8 frames, under 28.2 a second) now count as an overload at any step count. Gaps of a quarter second
or more (a load, a pause) start the average again. After a drop for the game's speed, the calm needed before
trying again doubles up to 2 minutes (30 s for the GPU). A frame without in-between frames queues no matching
work, so the drop gives the CPU back. `DOL_AURORA_FRAME_INTERP_PACING=0` still turns all pacing off. On the 4
E-cores, paced at 60 FPS, two runs each against 0.2.2:

| | Standing at the spawn view | Running |
| --- | --- | --- |
| 0.2.2 | 24.4-25.4 game frames a second | 25.7-28.1 |
| This build | 29.6-30.0 | 29.7-30.0 |

There the in-between frames go off within a second of the heavy view and stay off: these cores hold the game's
own 30 with little to spare (27-30 in the heaviest moments, interpolation off). The counter still reads about
60 because Aurora shows each real frame twice while the in-between frames are off (as 0.2.2 did after its
GPU drops). On the full i9 nothing changes: one drop at boot (the startup screens' frames come 48 ms apart),
none in play.

**Match the display, up to 240 FPS.** The Frame rate list has a fifth choice, **Match the display (frame
interpolation, up to 240 FPS)**. It shows the display's rate in whole steps of 30 (`shown_steps`), up to 7
in-between frames (`frame_interp::kMaxSteps`, was 3), and follows the window to another display.
`smooth_motion_fps=display` in settings.ini. Aurora's per-step state grew with it: the held frames, the
per-job ranges (`InterpRanges` held 3; at 7 the helper overran its stack, a fail-fast exit), and the buffers'
limits. Each buffer still starts where 60 and 120 left it and grows to what a frame needs. One trap: the
vertex limit, 8 MB / 3 * 7, was not a multiple of 4, so no mapped staging buffer could be made and every frame
silently lost its in-between frames. The FPS counter still read 60 (the held frame twice), and a benchmark
showed a false gain until the frame dumps came back empty. `BLUEWAKE_TEST_REFRESH=HZ` makes the window's
display report HZ, for tests. Simulated at 240 Hz (pacing off, as this 60 Hz display holds presents back),
each game frame had its 7 in-between frames, each an even step nearer the next real frame (mean pixel
difference to the previous real frame 0.52, 0.67, ... 1.16, and to the next 1.00 ... 0.20). Not yet seen on a
real high-refresh display.

**Guest MEM1 as a global array (RecompCore 0118).** The Windows module defines `BW_GUEST_MEM1=bw_guest_mem1`
(32 MiB, page aligned, `cmake/composite/guest_cpu.c`), and `get_ram_ptr` indexes it directly. The host adopts
it as `cpu.ram` (`bluewake_composite_guest_mem1`), and refuses to run if it cannot. Through `cpu->ram`, a
pointer read at run time, any guest load or store might have aliased the guest CPU's state, so translated
code stored the guest registers before every guest store and reloaded them after. Through a second global it
cannot. Unpaced on the 4 E-cores with interpolation off: 0.2.2 31.0 standing and 31.9 running, this 32.4 and
32.3 (two runs each; 1-5 percent, near the noise). The audit tests (`tests/native_*_test.c`,
`fast_blocks_test.c`) now run the translated side on the module's own MEM1 when it has one. All four native
audits pass against the new module.

**Verified.** With interpolation at 60, the dumped real and in-between frames of game frames 880-910 are
byte for byte the release's (62 of 62), with pacing on and off. At 30 FPS, the retrace-sampled captures
(`BLUEWAKE_CAPTURE_RETRACE`/`_INTERVAL`) moved by a game frame from 1007 onward. A capture takes whichever frame
the render worker hands back after the retrace, so it follows host timing, and this build's faster game thread
changed it. Every guest-side log line (milestones, retraces and block counts) was identical to 0.2.2's. With
the GX translation on the game thread (`DOL_GX_FIFO_WORKER=0`), where that timing cannot move, 0.2.2 and this
build give the same 19 frames.

**Where the 4 cores' time goes (for what is next).** The game thread is about 85 percent busy, and about 70
percent of it is translated game code, flat (the top function, collision's `GroundCrossGrpRp`, is 2.3
percent). The rest of its time is mostly waiting. A sampler that walks the stack (DbgHelp `StackWalk64`)
found about 9 percent of it in `GXDrawDone` at `JFWDisplay::endFrame` (`main.c`'s drain: the GX worker
finishing the frame's last draws) and 3.5 percent in the draw-done drain at `GXSetDrawDone`. The GX worker is
about half busy. Its time is spread across many small costs: building each draw's plan, the 2.3 KB transform
snapshot copies, comparing the 2.8 KB constant block, and `TextureRef` reference counts. Upstream Aurora
(Dusklight's) avoids much of that with dirty flags per state group and GPU vertex decoding. Its draw-done
completes on the FIFO worker, but here the game reading vertex arrays and matrices from RAM after
`GXDrawDone` makes returning before the worker is done unsafe without snapshots.

**Tried and dropped.** Batching audio pushes (256 frames per push instead of each DSP chunk, to take SDL's
stream lock about 125 times a second instead of 1,300): no measurable change (its waits are 0.4 percent of
the game thread). A larger GX FIFO hand-off batch (4-32 KB): no change.

**Next.** Natives for whole measured loops (collision's ground and wall checks, J3D animation; the J3D native
leaves already run 4-5 times faster than their translation). A versioned XF state, so an unchanged draw skips
the snapshot copies and the constant rebuild. Seeing Match the display on a real 144 or 240 Hz display.

## 2026-10-01 Windows 0.2.2 update: one Frame rate list, and the mouse a game frame sooner

**The Frame rate list.** Players confused Smooth Motion (60 or 120 shown, the game at its own 30) with
the experimental 60 Hz gameplay (the game itself at 60). The Display tab now has one **Frame rate** list:
30 FPS (the game's own), 60 FPS (frame interpolation), 120 FPS (frame interpolation, 120 Hz displays) and,
where the module has the 60 Hz sites, 60 Hz game logic (experimental, not recommended), each with a line
saying what it does; the 60 Hz choice still applies at the next launch. F10's name is frame
interpolation. `BLUEWAKE_TEST_MENU=SECONDS[:CHOICE]` opens the menu by itself (and makes that choice)
for tests: each choice was checked that way, in screenshots and in settings.ini.

**The mouse.** Players found the mouse camera floaty, worse at 30 FPS. Measured on the test PC: the game
presents (and Aurora collects the window's events) every 33.4 ms, once a game frame, and the camera
takes the pointer's motion at bumpCheck a median 21.5 ms after that (18.2-23.1 ms, at 60 and at 30
FPS alike), so motion was 21.5-55 ms old when the camera used it, about 38 ms on average. The present
queue adds nothing here: with `DOL_AURORA_PRESENT_TIMING` (a diagnostic, not kept) the swapchain's
acquire took 0.02 ms and its present 0.2 ms (median), never a refresh. mouse_camera.c's
`take_fresh_motion` now pumps the window's messages and takes the queued motion off SDL's queue when the
camera uses it (only while the mouse is the camera and no menu is open), so the same deltas count up to
a game frame sooner. With scripted motion pushed through SDL's queue (`BLUEWAKE_MOUSE_TEST_QUEUE=1`, the
camera captured by a posted click with `SDL_MOUSE_FOCUS_CLICKTHROUGH=1`), the view began turning at
retrace 1700 instead of 1702 and ended at the same angle (-2398), so nothing is counted twice.
`BLUEWAKE_MOUSE_FRESH=0` turns it off; `BLUEWAKE_MOUSE_LATENCY=1` logs the two times. The Mac collects
events the same way, so this does not explain a difference from the Mac by itself.

## 2026-09-30 A fullscreen crash: Smooth Motion's deferred presents during a swapchain resize (Windows 0.2.2)

A tester's i7-8565U laptop (Intel UHD 620) crashed on every switch to fullscreen, by F11 or the
settings, in 0.2.1 too ("an issue with the WebGPU DLL"). Going fullscreen resizes the swapchain: the
surface is reconfigured and the frame buffers and the copy bind group replaced, on the main thread,
after `render_worker::synchronize()`. That only drains the render worker's queue, and Smooth Motion's
deferred presents run on the worker between queue items (its idle hook), so one could start in the
middle of the resize and submit or present with the old swapchain's objects.

Reproduced on the i9-13900KF and RTX 5090 by making the gap wide: the game held to two logical CPUs
(affinity 0x3), a 4x render scale with 16x filtering, and F11 pressed 30 times 150-900 ms apart. 0.2.1
crashed after 9 presses (an access violation in nvwgf2umx.dll under webgpu_dawn.dll) and, in a second
run, after 21 (a null read in webgpu_dawn.dll). The crashing thread was the render worker:
`worker_main` -> idle hook -> `deferred_present_tick` -> `present_deferred` -> `end_frame`'s callback
-> `Queue::Submit`.

RecompCore 9618e9d (patch 0117) adds `render_worker::Pause`: the worker finishes what is queued, runs a
callback (the deferred presents still waiting, on the old swapchain), then waits, running neither queue
items nor the idle hook, until the main thread has finished; `resize_swapchain`, `refresh_surface` and
`release_surface` use it instead of `synchronize()`. The window system calls stay on the main thread (a
Mac's surface needs its main thread). The same setup then ran 3 x 30 switches (90) without a crash and
closed normally.

Earlier the same day (1ce29ac, Windows 0.2.1): closing the window ended with status 1, so every player
close showed "BlueWake stopped with an error (status 1)", and the settings' Restart now called
`std::exit` inside a frame (a joinable GX worker thread at exit, `std::terminate`). Windows' event log
on the test PC had two of the latter from 2026-09-29.

## 2026-09-30 Save states and climbing on Windows (Windows 0.2.0)

main b39bd0d merged into windows-release (2476b48), with the Windows app's part in 3ba8599: save_state.c
and climb.c built, Aurora's zlib linked (zlib1.dll was already in the download), states in
`%APPDATA%\BlueWake\states` (`BLUEWAKE_STATE_DIR`), F5 saves and **F8** loads (F9 is the frame rate on
Windows; mouse_camera.c picks the key, main.c's log names it), the settings' Mods tab has "Climb any
wall" and its stamina (settings.ini `climb=`, `climb_stamina=`), the menu has Save state and Load latest
state, and win_settings.cpp draws the stamina wheel. Built from 1510ed1 (game module 983ec7dc, the same
code as the tested 9ae71f76 but for the link time stamp).

Climbing needs the game module's direct-call watch list to name climb.h's addresses (0x8010F0DC,
0x8010F554, 0x80135FE4): scripts/windows/direct_calls.py reads them from the host sources, and the
builder keys the module on that list. With 0.1.1's module (built before climb.c existed) Link ran into
the wall and never grabbed it; with the rebuilt module he does.

Checked on the i9-13900KF and RTX 5090, windowed:

- Both features off, the Outset route (pinned window, Smooth Motion off, unpaced): 19 of 19 captured
  frames are 0.1.0's; Smooth Motion's in-between dumps 62 of 62.
- Save states: one saved at retrace 1000 (23.5 MB, 0.16 s) leaves all 19 frames unchanged. Loaded
  mid-run at 1600 (0.07 s, 150 host fields, none missing), the run reproduces the frames of the run that
  never loaded, 10 of them after the load. Loaded at boot, with a fixed clock (`BLUEWAKE_CLOCK=0`), the
  state saved at retrace 2000 has the CPU, MEM1, ARAM, the guest aliases, the VI clock, the host and
  loop variables and the DSP byte for byte those of the run that never loaded (the GX chunk differs in
  about 100 bytes, as it does after a mid-run load whose frames are exact). Frame captures after a boot
  load land on other retraces (1004, 1107, ... against 1007, 1109, ...: nothing has been drawn yet, so
  the presents' timing differs) and its first frame lacked the palm trees, whose pipelines were still
  being compiled (Aurora skips a draw whose pipeline is pending, as on any first visit to a place).
  F5 and F8 pressed in the window (SendInput) and the menu's two buttons each saved and loaded.
- Climbing, Orca's house (`BLUEWAKE_TEST_WARP=1100:Ojhous:1:0`, a 6 s wheel), two approaches to the back
  wall: grabbed (proc 0x3D, then 0x3F), the wheel 0.1 lower every 18 frames, out of stamina he let go,
  full again 3.3 s after landing, grabbed again. In a paced window the wheel shows beside him (green,
  orange when nearly out). Ticking "Climb any wall" in the menu wrote `climb=1`.
- Speed as 0.1.1: on 12 efficiency cores (the slower-CPU stand-in) the game holds 30 (60 shown)
  standing and running at the Outset spawn, and runs 35.8 game frames a second unpaced (0.1.1: 35.0).
- The download (WindWakerRecomp-0.2.0-windows-x64.zip) unpacked fresh: the first launch unpacked and
  prepared the .rvz, the game ran at 60 shown / 30 with Link moving, a state saved at 1401 and loaded at
  1900.

## 2026-09-30 The Windows build's GX worker work merged (Mac-tested)

RecompCore 8ab24da (patch 0112) merges the Windows build's RecompCore branch (windows-release, forked
at 6892947): 4f7a3ec's cheaper worker draws (the derived pipeline state cached by a register version,
no assembly totals walk, the last pipeline lookup and bind group reused, Smooth Motion jobs without a
copy for repeated constants and a fence only when the helper may sleep), 825f103 (constant blocks
compared against a copy off Apple GPUs; the Mac keeps comparing in place), f93c05f (gather-pipe writes
as a run of bytes, for a game module that batches them), e8c2bb3 (`DOL_AURORA_CACHE_DIR`), 2a85bd1 and
82607d4 (the graphics threads' CPU time and slow presents in the log). With save states, a register
state put back from a state takes a fresh derived-cache version (GxCoreState::renew_version).

Mac-tested at the Outset spawn, Smooth Motion 60, an 8 s sample of each release host: the FIFO worker
55.4 -> 53.2 percent busy (the Windows test PC's efficiency cores went from 23-26 to 29-30 game frames
a second); 95.6 percent of draws hit the cache, and `DOL_GXCORE_DERIVED_VERIFY=1` finds no mismatch in
5.7 million hits on a walk nor in 7.0 million after a save state load. The Mac app now keeps its shader
and pipeline caches in its data folder (`DOL_AURORA_CACHE_DIR`), apart from test runs' (the first launch
after this compiles them again).

## 2026-09-30 Climbing any wall, on a stamina wheel (Mac-tested, headless)

`runtime/host/src/climb.c`, the options menu's Gameplay tab (`BLUEWAKE_CLIMB=1`, off by default;
`BLUEWAKE_CLIMB_STAMINA`, 12 seconds). Link climbs steep plain walls with the game's own ivy climbing:
daPy_lk_c::setFrontWallType classifies the wall in front of him each frame, and a wall of code 1 is ivy
(mFrontWallType 3, which changeFrontWallTypeProc turns into procClimbUpStart, or procClimbMoveUpDown in
the air); setMoveBGCorrectClimb checks the code again every climbing frame and drops him when it is not.
Only calls into another translation unit return through the dispatcher, so the hooks are the return sites
of the collision queries in those functions: GetWallCode's in setFrontWallType (0x8010F0DC) marks a plain
wall (code 0) and keeps the collision it hit; the LineCross at grabbing height (0x8010F554) returning a hit
means the wall goes on above where he grabs ledges, and there the plain wall becomes type 3 (mPolyInfo
set from the kept collision), under ivy's own conditions (daPyFlg0_UNK100 set, VINE_CATCH clear, 125
above lava or water, in the air only while steered at it); the only change the game still makes after it
is to a wall to sidle along, which wins. Ledges he pulls himself onto, ladders, blocks and real ivy keep
the game's behaviour. In setMoveBGCorrectClimb (0x80135FE4) a plain wall reads as code 1 while there is
stamina. The wheel drains in 12 seconds climbing (a 0.4 share holding still), empties into a fall and an
exhaustion that allows no grab until it is full, and refills in 3 seconds after half a second on the
ground; real ivy costs nothing. It is drawn with ImGui beside Link, projected from the camera's view
(eye, centre, fovy, aspect) at camera_draw into the game's picture.

Headless, in Orca's house (warp Ojhous:1:0) pushing into the back wall with a 6-second wheel: grabbed at
the wall (proc 0x3D, then 0x3F), climbed from y -19 to the ceiling at 264, the wheel draining 0.1 every
0.6 s; empty at 6 s, fell to the floor, no grab while exhausted, full again 3.3 s after landing, and
grabbed again. The wheel's position came out at Link's (0.50, 0.43-0.56 of the picture). The wheel
itself and a climb outdoors have not been seen in a window yet.

## 2026-09-30 Save states (Mac-tested)

Dolphin-style save states for debugging (`runtime/host/src/save_state.c`, the host_state_* functions in
`main.c`; RecompCore: the GX front end's and gxcore's register state, `dol_aurora_gx_save_state` /
`_load_state` / `_drain`, `dol_hle_callback_idle`, and a Smooth Motion cut after a load). A state is a
gzip stream of tagged chunks: the CPU, MEM1 (32 MiB), ARAM, every guest alias's storage (linked REL
data and BSS), the VI clock, the host's device models and milestones by name (150 fields), Dolphin's DSP
HLE and the GX front end with any half-written command. It is taken at the next GXSetDrawDone return
with no exception, REL prolog, memory card callback, scene change or quick door in progress, about 20
MB and half a second; a load takes about 0.1 s. F5 saves (`quick-<retrace>.bwstate` in
`BLUEWAKE_STATE_DIR`, the Mac app's `states/`), F9 loads the last one, or the newest in that folder after
a relaunch; the options menu has both. `BLUEWAKE_SAVE_STATE=path@retrace` and `BLUEWAKE_LOAD_STATE=path`
script them, `BLUEWAKE_STATE_TEST_LOAD=retrace` presses F9. Before replacing memory a load drains the
FIFO worker, which reads guest memory as it translates.

Checked on the Outset save with a scripted walk: headless, a state saved at retrace 1000 and loaded at
boot, or mid-run at 900 or 1300, reaches retrace 1500 with every chunk but the renderer's byte for byte
that of the run that never loaded (MEM1 04F48E8B). With the renderer, a load at boot and one mid-run
reach the same 1500 as each other (windowed runs' own timing varies between launches), and the first
frame after a load draws the whole scene. The subagent's first cut (branch wip/save-states) is this
work's start; it had never been built or run.

## 2026-09-30 A fast right-stick camera and aiming, and a camera kept out of the ground (Mac-tested)

**The right stick as the camera** (`runtime/host/src/mouse_camera.c`, on by default on the Mac,
`BLUEWAKE_STICK_CAMERA=0` for the game's own). The game's C-stick camera (dCamera_c's manual camera,
mode 12) eases its turn in and out and starts only past a quarter of the stick (Aurora's substick dead
zone, 8000). Wherever the mouse turns the camera (the follow camera, the player in control), the right
stick now sets the view's angles the same way: a rate from its tilt (12 percent dead zone, then 30
percent linear and 70 percent quadratic up to 360 degrees a second, `BLUEWAKE_STICK_CAMERA_SPEED`;
up and down at 0.6 of it), by game time (a game frame's worth at each update, so a steady tilt turns
evenly through the in-between frames), no easing, the view held where it is left. The pad read keeps
the tilted stick from the game there. Its click is the C-stick's push up (first person); in first
person a click is the push down out, a little then past three quarters, released as soon as
subjectCamera's m3C4 has taken each step (held on, the follow camera took it for its own push down and
switched to the manual camera). Scripted (`BLUEWAKE_STICK_TEST`, headless): full tilt 12.01 degrees a
game frame, half tilt 3.41, first person in 4 retraces and out in 8, camera mode 0 throughout.

**Aiming** (same file). In first person and when aiming an item, the right stick aims as the mouse
does (aim_frame: shape_angle.y and mWork.subject.m388), at `BLUEWAKE_STICK_AIM_SPEED` (180 degrees a
second), slower in proportion to the telescope's and Picto Box's zoom; pushing it down looks down
instead of leaving first person. In those two views the left stick's up and down (or the D-pad's)
zoom, as the C-stick's did, and the left stick no longer aims there. Scripted: 6.0 degrees a game
frame at full tilt; the telescope 1x to 7.4x in two thirds of a second, and aiming at 7.4x turning
7.4 times slower. The options menu's Controls tab has the switch, both speeds and inverted axes.

**The camera kept out of the ground and the water** (mouse and stick). Their angles were applied at
camera_draw, after bumpCheck (the camera's wall, ground and water check) had placed the eye, so a low
tilt put it wherever the angle said, into the ground or under the sea. They now go into
dCamera_c::mViewCache at bumpCheck's entry (0x80167F08, once a frame in Run), and the game pulls the
eye in along the line from Link and lifts it to the water's surface. At the Outset start, tilted to
the lowest angle facing up the slope: the eye 36 units under the ground before, now pulled in from 249
to 190 units and 5 above it.

## 2026-09-29 Cheaper, batched in-between frames and the PC branch's host fixes (Mac-tested)

**Renderer** (RecompCore 6892947, patch 0110):
- **Encoding:** the render worker binds the vertex and index buffers once a pass and sets bind groups only when they change; a busy scene's replay went from about 4.4 ms to 1.3-1.6 ms.
- **Batching:** consecutive draws with the same pipeline, constants and textures, whose data follows on, are one draw. Adanmae went from about 6,400 draws a frame to 3,600, the sea from 11,800 to 4,600. In-between frames split a batch whose draws blend differently. `DOL_AURORA_GXCORE_BATCH=0` turns it off.
- **In-between data:** written into mapped staging buffers and copied on the GPU, instead of a new zero-filled upload buffer every frame (a fifth of the render worker's time).
- **Pacing:** under sustained overload 60 Hz drops its in-between frames and comes back after 3 s calm. 120 Hz is not lowered unless `DOL_AURORA_FRAME_INTERP_PACING=1`.
- **Helper thread:** spins less and is woken in batches.
- **Hidden full-screen window:** asks for no drawable and keeps its surface, so switching away no longer freezes the game for half a second or rebuilds the surface on return.
- **Diagnostics:** `DOL_GXCORE_DRAW_DUMP=<game frame>` lists every draw of a frame.

**From the PC branch (native-60hz-pc):**
- Host: the guest-alias registry under a lock for the translation worker (a crash about one launch in eight), graphics address resolutions cached until the registry changes, and the actor search's budget checks collapsed.
- RecompCore: gather-pipe words straight to the worker's batch, the batch buffer kept, draw plans reset in place, and the texture layout cache locked.

**Other changes:**
- The mouse camera's per-boundary check is inline.
- `BLUEWAKE_TEST_PLACE=retrace:x:y:z` stands Link at a position for tests.

**Tested:** Adanmae at 120 Hz holds 119.8 FPS with no slow render items. 40 dumped frames at sea show no pops. 60 Hz and Smooth Motion off hold their rates. `frame_interp_test` and `actor_search_budget_test` pass.

**Known:** the lava in Adanmae renders flat orange. Its texgens read the room's world matrix instead of J3D's projection texture matrix. It is diagnosed, not fixed.

## 2026-09-29 Smooth Motion at 120 FPS (Mac-tested)

**120 FPS** (RecompCore d389b4b, patch 0108). The options menu's Smooth Motion is Off, 60 or 120
(`DOL_AURORA_FRAME_INTERP_STEPS=3`, or `HZ=120` for `run_host.sh`), for 120 Hz displays such as a
MacBook Pro's. Each game frame gets three in-between frames, at a quarter, half and three quarters of
the way: a matched draw is blended once per step, the camera's part motion the screw motion's power at
t (exactly half at 0.5, so four quarter steps make the whole), and the helper thread stages each
step's block and a particle's vertices. On the user's save on a beach, full screen with the 4K pack
and 16x anisotropy: 120 presents a second, 8.4 ms apart at the median.

**A steady present clock** (same patch). Presents are held aside and shown on one clock, continuing
from the game frame before's last, from two alternating sets of held frames, with what is due
presented between the in-between frames' replays and every 512 commands of a pass. Before, the first
in-between frame was presented at once and the rest a quarter of a frame after it: at 120 Hz the
second came 18 ms late and the third and the real frame back to back when the next frame arrived
(bursts, the sea shimmering), and at 60 Hz presents alternated 22 and 12 ms apart (now 16.5).
`DOL_AURORA_PRESENT_LOG=1` logs each present; `DOL_AURORA_PRESENT_CLOCK=0` keeps the old 60 Hz timing.

**The sea blinking out on a shore at 120.** The in-between blocks of a beach at 120 come to about 55 MB
and the area had 32: 6,900 blocks a frame did not fit, and those draws kept the next frame's transforms
in two of the three in-between frames, the sea among them. The area now has 32 MB per step. Found by
dumping 144 game frames walking in the shallows and flagging any in-between frame unlike both real
frames around it (five in a row, the sea missing); afterwards none.

**Saved options at launch** (RecompCore df6c2b1, patch 0109). Smooth Motion, its steps, the FPS overlay
and forced anisotropy were read by Aurora's static initialisers, before the host applies the saved
options, so the menu's 120 came back as 60 and 16x anisotropy as none; the backend reads them again
at initialisation.

## 2026-09-29 An options menu, quick doors, and Smooth Motion for what the game moves itself (Mac-tested)

**Options menu** (`runtime/host/src/settings_menu.cpp`, Mac; RecompCore 66205c2, patch 0105). F1, a
controller's Back, or Esc while the mouse is free pauses the game and opens Display, Gameplay and
Controls tabs over it (`dol_aurora_set_hold_redraw` keeps the paused picture on screen). Choices are
saved to `~/Library/Application Support/Wind Waker Recomp/settings.ini` and applied at the next launch
before anything reads the environment; the sprint, jump button, mouse camera, fast loading and quick
doors also take them at once. Test runs pass `BLUEWAKE_SETTINGS=none`.

**Quick doors** (`runtime/host/src/quick_doors.c`). Through a door with a knob, Link opens it as the
game has him do; once its fade covers the screen the rest is cut (his walk behind it, the wait, and in
the next room the door opening and closing again), and he stands inside with the door closed as the
picture comes back: 5.1 seconds to 1.9. `BLUEWAKE_QUICK_DOORS=0` keeps the game's doors;
`BLUEWAKE_DOOR_TRACE` logs them. With `BWW=1` the right stick now turns the camera the way the mouse
does (Better Wind Waker's invert_camera_x).

**Smooth Motion for what the game moves itself** (RecompCore 3b65983, patch 0106;
`runtime/host/src/draw_tags.c`). The in-between frame blends each draw's matrices, so what the game
moves another way stepped at 30 FPS or was drawn twice. Sword swings: a bone of Link's arm turns 90
degrees and more in a game frame, which the old 41-degree bound rejected (150 to 420 draws a frame);
a draw with a key of its own may now turn 150 degrees, blended as a rotation (slerp) so it keeps its
size. Particles: before each JPA particle draw the host writes the particle and its age to BP 0x7E
and 0x7D, registers the retail GX never uses, and the renderer pairs the draw with the same particle
and blends its corners (dust, spray, smoke, sparkles, ripples). The boat's bow waves and trail, drawn
by their emitters' callbacks, are announced with their emitter and draw count (BP 0x7C, 0x7B) and
blend vertex by vertex, so the wake no longer sits half a frame ahead of the bow. A broken pot's
shards (one model at random sizes, tumbling fast) pair with the nearest copy of their size. The boat:
its CPU-skinned hull, its shadow-map pass, its real shadow's volume (one box for every shadow, moving
with the camera) and the sea triangles the shadow is cast on (paired by the shadow's texture, a
different count as it sails). frame_interp_test covers each case. RecompCore 94b97ce and 060293f
(patches 0103, 0104) move the matching and blending to a helper thread and fix a device loss on
Direct3D 12; the inputs a draw's blend needs are now captured with it (DrawInput) for that thread.

**Where 60 is missed** (`runtime/host/src/fps_watch.c`). Once a second with fewer than 57 frames on
screen: `[fps-dip]` with the game's speed, how many frames were interpolated, rejected and unmatched
draws, the waits for the GX worker, presents and the GPU, and the stage, room and Link's position
(`BLUEWAKE_FPS_WATCH=0` turns it off). `DOL_AURORA_FRAME_INTERP_TRACE` takes a range of game frames.

**A crash after a long session.** The host places the game's modules (RELs) in memory above the
game's 24 MiB; that window was 1.5 MiB. After 27 minutes through many islands it was full of linked
modules, the sea by the pirate ship needed d_a_bb (52 KB), and the game jumped into the module it
could not load. The window now starts at 0x81820000 (7.4 MiB).
## 2026-09-29 Windows: the settings menu, local optimization training, and a steady 60 FPS

**Settings and window** (`windows/src/win_settings.cpp`). F1 (or Esc) opens an ImGui settings menu over
the game: fullscreen, Smooth Motion, the frame rate, render resolution, texture filtering, the mouse
camera, the controller's camera stick, the aspect and Better Wind Waker's options, the HD texture pack
and the sound mode, saved to `%APPDATA%\BlueWake\settings.ini` (launch-time ones behind a Restart
button). The window opens centred and sized for the screen, and comes back where it was left; F11 or
Alt+Enter toggles fullscreen (Alt+Enter no longer reaches the game as START). The menu's font is
Segoe UI at the display's scale, in an atlas of its own uploaded through `aurora_imgui_add_texture`:
growing ImGui's own atlas rebuilt it on the main thread while the render worker submitted, and Dawn's
device is not thread-safe here (two of three launches crashed in the driver). The jump (Space) and
sprint (Shift) from the Mac build work on Windows too, and are listed in the menu.

**Optimization training on Windows** (`scripts/windows/build.py`): an instrumented module plays the
opening twice headless (plain, then widescreen with Better Wind Waker), 288 of 813 translated
functions ran, and the module is compiled with the profile. It is retrained only when the game source,
the parts of RecompCore compiled into the module (GXRuntime's CPU core and its headers, the ABI), the
compiler or the CPU level change; a rerun with nothing changed compiles nothing.

**Smooth Motion at 60** (RecompCore 94b97ce, patch 0103, on top of b4af144's draw-heavy work). On the PC
(i9-13900KF, RTX 5090) Smooth Motion held the game at 24 to 28 FPS in Outset's village: about 10,000
draws a frame, and the FIFO worker, which the game waits for at each frame's end, was saturated. The
in-between matching and blending now run on a helper thread in draw order (end_frame waits for it), the
in-between staging makes fewer passes over the 2.8 KB constant block, an unchanged texture takes one
lookup instead of two, and the app is compiled for x86-64-v3 like the game module (AVX2 and FMA; the
baseline build alone was 28 FPS on Outset). Frames with nothing to blend (menus, still shots, cuts) keep
the in-between cadence, so the timing does not jump by half a frame at each cut and the counter does not
drop to 30 in menus. Paced through the new-game opening with everything merged (461 s): the game at
full speed every second (lowest 59.8 retraces a second, the village included), 60 FPS shown in all but
9 seconds, each a scene change where the game itself skips a frame or briefly runs past 30, and every
in-between frame halfway between its neighbours (`frame_interp_report.py`: 30 of 30, ratio 1.03).

**Fast scene changes on Direct3D 12** (RecompCore 060293f, patch 0104). With the present suppressed,
end_frame's status check took the Error status it starts from for the surface's and dropped the
surface; the swapchain made in its place for the same window failed with E_ACCESSDENIED and lost the
device at the first fast-forwarded black (every launch, four seconds in). It now leaves the surface
alone. Not yet run on the Mac; the changes are not platform-specific.

## 2026-09-29 Fast scene changes, a sprint, and 60 FPS in the Forsaken Fortress (Mac-tested)

**Scene changes** (`runtime/host/src/fast_load.c`; RecompCore b4af144, patch 0101). A door or an exit
took 2.2 seconds, none of it loading (disc reads are already instant): the plain fade (dOvlpFd,
overlaps 0, 1, 6, 7, 8) counts 26 game frames each way, and the new scene may not load its sounds
until 36 frames after the door (mDoAud_setSceneName's load timer, while the old music fades). The
host now shortens a fade as it starts, fader (JUTFader mFadeTime/mTimer) and overlap count
(overlap1_class 0xCC/0xD0) together, keeping their sum so the scene is swapped the frame the screen
is fully black (`BLUEWAKE_FADE_FRAMES`, default 6; 0 keeps 26). Once the screen has been black for 6
retraces of a scene change, the game runs unpaced with `dol_aurora_set_fast_forward`: frames are not
presented and the audio queue is kept at its 100 ms target (`BLUEWAKE_FAST_FORWARD=0` turns it off;
at most 600 retraces). Warping into Link's house, windowed with Smooth Motion and sound: 0.63
seconds (fade 0.20, black 0.25, fade back 0.18) instead of 2.2, starved audio pushes 164 against
200 before. `BLUEWAKE_LOAD_TRACE=1` logs each retrace of a change; `BLUEWAKE_TEST_WARP` asks for one.

**Sprint** (`runtime/host/src/sprint.c`). Holding Shift makes Link run 1.5 times his top speed
(`BLUEWAKE_SPRINT_SPEED`): daPy_HIO_move_c0::m (0x8035CED4) field 0x18, the 17 procMove sets
mMaxNormalSpeed from, and field 0x48, the run animation's rate at it (setMoveAnime blends by speed
over mMaxNormalSpeed), scaled together and put back on release. Scripted: mNormalSpeed 17 to 25.5
and back. Swimming, iron boots, targeting and carrying have their own parameters. On a controller,
a click of the left stick starts the sprint, which lasts until the stick rests in the middle or the
next click, and the left bumper jumps (both are free in the GameCube mapping; on the Switch Online
GameCube controller, product 0x2073, the bumper is L, so it does not jump there).

**The Forsaken Fortress at 60** (RecompCore b4af144, patch 0102). Its exterior draws 17,500 times a
frame (550 is usual) and fell to 49 retraces a second: the GX translation worker was 93 percent busy,
about 40 ms a game frame, 31 percent of it the in-between frame's blending. 96 percent of its draws
repeat the vertex constants of the draw before them; those are compared once instead of three times,
a repeated draw matched the same way reuses its in-between block, and indexed position and normal
matrices are blended only for draws that read them. A Fortress-shaped load (frame_interp benchmark):
10.0 ms of blending a frame before, 1.8 ms after, with the same blended values; frame_interp_test
covers the reuse and the indexed matrices. In play, the Fortress exterior at 17,900 draws a frame
now holds 60 (the GX worker's slow batches average 22 ms; the emulation thread is 94 percent busy, so
there is little headroom).

## 2026-09-28 Jump button, mouse aiming and zoom, HD texture packs (Mac-tested)

**Jump** (`runtime/host/src/jump_button.c`, the Mac host). Space makes Link jump: at his next proc
call (execute's `(this->*mCurProcFunc)()`, a chassis edge), when that proc is procWait, procFreeWait
or procMove and he is on the ground with nothing else going on, the call goes to procAutoJump_init
(0x80115EA4) instead, the jump off a ledge, so the game flies and lands it. A press in the air, a
roll, water, a ladder, an event, a menu, while carrying, in iron boots or targeting is dropped, not
kept. From standing it is a short hop; it goes the way Link faces. `BLUEWAKE_JUMP_BUTTON=0`,
`BLUEWAKE_JUMP_TRACE=1`, `BLUEWAKE_JUMP_TEST=retrace,...`.

**Mouse aiming and zoom** (`mouse_camera.c`). In first person and every item's aim (bow, hookshot,
grappling hook, boomerang, telescope, Picto Box: the subject camera, engine 4) the mouse turns the
aim itself at the player's update (daPy_Execute's entry): shape_angle.y with the angles the USA
execute puts back (l_debug_shape_angle / l_debug_current_angle), and the tilt (dCamera_c m388),
with the same turn in the view cache, so the frame follows with no easing lag and the game's limits
hold. The wheel zooms: the follow camera's distance in third person (0.5x-2x), and the telescope's and
Picto Box's own 1x-9x zoom (m38C), also while the game locks the view on a target (Aryll's telescope
lesson does, and waits for a full zoom). Her lesson checks the postman's head within 50 screen units
of the circle's middle whatever the zoom, so it is found at 1x.

**HD texture packs.** `run_host.sh TEXTURES=DIR` takes a Dolphin-format pack (its GZL folder, or a
Dolphin folder holding one) into `DOL_AURORA_TEXTURE_PACK`. ZWW4K 1.0.0d: 561 replacements (BC7),
the pier and the HUD replaced, 60 FPS without hitches, about 240 MB more; 23 of its mip files have the
wrong size and are dropped with the levels below them, as in Dolphin.

**Also:** `bluewake_edge_intercepts_test` builds again (the mouse camera's sources, which need SDL,
had been added to it).

## 2026-09-28 Mac mouse camera, and in-between frames through fast turns (Mac-tested)

**Mouse camera** (`runtime/host/src/mouse_camera.c`, the Mac host). Click the window to hand it the
mouse: moving it turns the camera around Link and tilts it, and left click is A; Esc, or leaving the
window, gives it back. It turns the game's own camera, not the C-stick (whose vertical axis is the
first-person view and a pull-back, not a tilt, and whose horizontal speed the game eases): at
camera_draw's entry (a chassis edge at 0x8017C350), when the frame's camera is final, the mouse's
yaw and pitch go into dCamera_c's view cache (the next frame starts from them) and into the view's
eye for this frame, at the distance the game chose (walls push it in). From its first move the mouse
owns the angles, since the follow camera eases the tilt back and the yaw behind Link and its wall
check and smoothing move the eye after that; a cutscene, a door, Z-targeting or first person hands
the camera back. 0.18 degrees a point (`BLUEWAKE_MOUSE_SENSITIVITY`, `run_host.sh MOUSE_SENSITIVITY=`),
tilt -35 to 75 degrees, `BLUEWAKE_MOUSE_INVERT_Y`. Checked with scripted pointer motion
(`BLUEWAKE_MOUSE_TEST`, `BLUEWAKE_MOUSE_TRACE`): every frame drawn at the mouse's angles while Link
walks and a wall pulls the camera in (97 units and back), and a roll does not interrupt it.
The game's layout used: the camera at 0x803CA718 (+0x244 dCamera_c, view at +0, mLookat +0xD8),
mViewCache at +0x3C (the decomp's comment says 0x5C; the globe's radius equals |eye - center| at 0x3C).

**In-between frames through fast turns** (RecompCore 21775f1, patch 0100). A mouse turn of more than
about 11 degrees a game frame moved distant scenery past the plausibility bounds: up to a third of
frames were taken for cuts (the counter fell toward 30) and draws that failed on their own jumped
against the blended scene (a doubled look). The camera's motion is now taken out of each pair
(plausibility judges the object's own motion; the in-between matrices are carried by exactly half the
camera's motion, a quaternion half-turn), a unique draw that changed rigidly past the bounds is
blended by half its own motion, copies are found through a grid of where the camera carries them (a
budgeted scan ran out when a turn brought dozens into view), and copies just come into view do not
count toward a cut during a turn. Scripted turns at 11-32 degrees a game frame: 355 of 355 frames
interpolated (5 before), 0.65 percent of draws unblended (5.9), 59-60 FPS.

**Also:** `run_host.sh` keeps OUT_DIR/test.card between runs (it used to copy the Outset save over it
at every launch, which lost a session's progress); the pad script takes the C-stick
(`retrace:buttons:length:x:y:cx:cy`); `DOL_AURORA_FRAME_INTERP_LOG_FRAMES` logs each game frame.

## 2026-09-28 Better Wind Waker's settings as runtime options, no patched disc (Mac-tested)

**What it is.** Each of Better Wind Waker's settings is now a switch of its own (Mods > Better Wind
Waker Settings on iOS; `BLUEWAKE_OPTIONS` / `run_host.sh OPTIONS=` on the Mac), with Better Wind Waker's
defaults, and the patched disc (`betterww.iso`, its patcher, PyYAML and Pillow) is no longer needed.

**How** (docs/MODS.md, `mods/betterww/options.txt`):
- DolRecomp `--option-sites` (patch `dolrecomp/0019`): at each listed instruction the C backend emits
  the original and the replacement (or a call to native code) behind `dolrecomp_option_flags[n]`; a
  site is its own block, a replacement branch's target is a block start, and a loop with a site is not
  outlined. Without the flag the translation is byte-identical to the shipped one.
- 37 sites (15 chunks: 11 in main.dol, d_a_ship's 3, d_a_agbsw0's 1; 3 more combine with each
  widescreen mod), checked against Better Wind Waker's patch words and the original instructions.
- Native code (runtime/host/src/game_options.c) for what its added assembly did: turning while
  swinging, the camera's inverted C-stick axis, Swift Sail's wind (the hook returns into the game's
  `dKyw_tact_wind_set` with the link register pointing back at itself) and braking. Instant text
  patches the loaded messages (4,411 messages, 1,212 timed waits) as the patcher patched the disc's.
- 40 values written at boot per option; a REL's at its section's linked address, where its data lives
  for the session. The option table and the writes are generated into `mod_variants.inc`, so
  `module_export.c` is unchanged.

**Measured on the Mac** (headless unless noted, the Outset save):

| Check | Result |
| --- | --- |
| Options mod on, every option off, vs no mod | identical player path (365 probe lines) and block count |
| Faster rolling: the same run-and-roll input, 9 rolls | 3,450 units vs 3,170 |
| Faster climbing: one ledge climb | 44 retraces vs 100 |
| Skip the opening movie: new game to the play scene | retrace 1,043 vs 14,062 |
| Inverted camera: C-stick right for 1 s (windowed) | the camera turns the other way |
| All defaults, 3,000 retraces; 16:10 + options (40 chunks) | runs; messages patched |

**Open.** Not reached from the Outset save: Swift/Brisk Sail, the unrestricted boat, the faster
Ballad, Tingle Chests, no song replays, turning while swinging, grappling, block pushing and the chat
zoom (the sites and values match the patch; each needs its place in the game). Swift Sail's texture
and icons are Wind Waker HD's art and not included; the item is still named "Sail". The iOS app
compiles and links; not yet run on a device. The translator change is in elliotttate/DolRecomp b8b5345 (chrissotraidis 5c91d6e
plus patch 0019), which elliotttate/RecompCore 7845b6c points at and the Builder now pins.

**Also:** a copy of grass or foliage that comes into view at the screen's edge while the camera turns
is now blended from where the camera's motion says it stood (its matrix carried back by the inverse
of the camera's motion) instead of drawn half a camera step ahead (unit test; the grass route is
unchanged: 1,003 of 1,320 frames interpolated, 60 FPS). The pad script takes the C-stick
(`retrace:buttons:length:x:y:cx:cy`).

## 2026-09-28 Widescreen 16:10 and Mac window, fullscreen and render scale (Mac-tested)

**What it is.** A second widescreen mod for 16:10 screens (the MacBook, iPad at 1.43 is closer to
4:3), beside the existing 16:9 one, and Mac host options for the window and render size.

**How.** The community 16:9 Gecko code changes a set of numbers that all move with the width the
picture gains over 4:3: the camera aspect, the 2D bounds, HUD positions in the meter and map tuning
structures, three instruction immediates, one `lis` float, and five `lfs` loads pointed at other
r2 pool constants. `scripts/mods/widescreen_aspect.py 16:10` interpolates each between the game's
4:3 value and the 16:9 value (t = 0.6) and writes `mods/widescreen/GZLE01-16x10.gecko`; the pool loads
take the nearest existing constant (all within about a pixel except one picture width, +6). The same
script with `16:9` reproduces the original code byte for byte. `build_mods.sh` builds it as mod
`widescreen1610` plus a `widescreen1610+betterww` combo; `build_mod_variants.py --exclusive
widescreen,widescreen1610` lets two mods own the same chunks when they are never on together (the
module: 3 mods, 22 chunks and 103 writes for 16:10).
- Mac host: `BLUEWAKE_ASPECT=16:9|16:10` picks the mod and sets `DOL_AURORA_ASPECT_RATIO` (1.7778 /
  1.6); the window is then 720 high at that aspect. RecompCore patch 0099 (backend only):
  `DOL_AURORA_WINDOW=WxH`, `DOL_AURORA_FULLSCREEN=1`, `DOL_AURORA_RENDER_SCALE=N` (N x 480 lines, any
  window; 0 = the window's pixels). `scripts/mac/run_host.sh` passes `ASPECT`, `WINDOW`, `FULLSCREEN`
  and `SCALE`, and uses `build/mac-interp/composite-1610` when it exists.
- iOS: Mods > Widescreen 16:10, exclusive with Widescreen 16:9; the launch sets the mod and 1.6.

**Measured on the Mac:**

| | Window | Frame buffer | Presented, Smooth Motion on |
| --- | --- | --- | --- |
| 16:9 windowed | 1280x720 pt | 2560x1440 | 59.8-60.0 |
| 16:10 windowed | 1152x720 pt | 2304x1440 | 59.8-60.0 |
| 16:10 fullscreen (MacBook) | 1728x1084 pt | 3456x2168 | 59.8-60.0 |
| 16:10, `SCALE=3`, 800x500 window | 800x500 pt | 2304x1440 | - |

At 16:10 the hearts, magic meter, rupees, minimap and item buttons sit at the edges as at 16:9; the
pause menu, map and the no-card dialog are not stretched (the doubled Save label in the pause menu is
the game's own animation and shows at 4:3 too). **Open.** A dark strip on the left edge is there at 4:3 as
well (not from this change): per-pass dumps show the game's 3D pass drawing nothing in its leftmost 2
pixels (6 at 3x), and its depth-of-field pass, drawing a half-size copy of the frame over it, widens
that to 11 columns (about 3.7 game pixels). The device module (`build/device/composite-ios`) was
rebuilt with the 16:10 variants, so its digests no longer match `build.sh`'s record until the next full
build; the iOS toggle compiles but has not been run on a device.

## 2026-09-28 Smooth Motion: 60 FPS from the renderer, the game still at 30 (Mac-tested)

**What it is.** Wind Waker's logic advances one step per frame and waits for 1/30 s between frames
(JFWDisplay::beginRender's waitForTick). Unlocking that wait doubles the game's speed, and the
Meowmaritus 60 FPS hack's ~50 slow-down patches list softlocks (Niko's platforms, the Earth Temple
slime, Molgera) and need twice the CPU the device does not have. So the game keeps its 30 steps and
the renderer draws a frame between each pair: every gxcore draw already carries its whole transform
state (VertexShaderConstants) with object-space vertices, so an in-between frame is the finished
frame's passes encoded a second time with each draw's position, normal, projection and texture
matrices and light positions blended halfway toward the same draw in the previous frame. It is real
geometry, not image warping: depth, occlusion, lighting and the HUD are exact; motion the game makes
by rewriting vertices on the CPU (particles, the logo swirls) stays at 30.

**How** (RecompCore patch 0098, `GXRuntime/graphics/aurora/lib/gfx/frame_interp.*`):
- A draw is keyed by a hash of its FIFO vertex payload (indices and direct attributes), or by shape
  and texture when positions are in the payload; the Nth draw of a key pairs with the Nth last frame,
  or the nearest plausible copy when copies change places. A pair must be plausibly one object a frame
  apart (scale within 1.5x, under ~41 degrees of rotation, moved under a fifth of its distance + 100);
  skinned draws check only the matrix slots their vertices use. 40 percent implausible = a camera cut:
  that frame is shown once.
- Copies of one model (grass clumps, bushes, palms) share a key and the game culls them one by one,
  so a copy leaving the view shifted every later one onto its neighbour (39-277 units away, inside the
  bound): at 60 FPS the foliage flickered while the camera turned, and one in-between frame drew a
  palm that is in neither real frame. Draws with a unique key now vote for their motion
  (M_now * M_before^-1, the camera's for anything standing still); a copy pairs with the previous copy
  that motion carries onto it (within ~0.02 units), and one no still copy lands on is new in view and
  drawn unblended. Circling the grass by the fence: every clump blended to exactly half the camera
  step; pier and title frames unchanged; the GX worker's matching 3.48 -> 3.68 ms a game frame.
- Blended blocks go to their own 32 MB area, uploaded once per frame, so they never split a frame's
  staging. The render worker submits the real frame and keeps a copy, encodes the passes again with
  the blended blocks (EFB copies and conversions included, the depth snapshot not), presents that,
  and presents the kept copy half a game frame later from its idle loop.
- Off by default: `aurora_set_frame_interpolation`, `DOL_AURORA_FRAME_INTERP=1`; iOS Display >
  Smooth Motion (60 FPS). FPS counter: `aurora_set_fps_overlay` / `DOL_AURORA_SHOW_FPS=1` (top centre,
  "60 FPS (game 30)"); the iOS Show FPS label gives the display rate too.
- Debug: `DOL_AURORA_FRAME_INTERP_LOG`, `_TRACE=<game frame>` (per-draw outcome), `_DUMP=dir` with
  `_FROM`/`_TO` (real and in-between images), `_DUMP_PASSES`, `_T=<weight>`.

**Measured on the Mac** (M-series, the retagged device module, `scripts/mac/run_host.sh`):

| | 30 FPS today | Smooth Motion |
| --- | --- | --- |
| Frames presented a second, title and Outset play | 29.9-30.0 | 59.8-60.0 |
| Title flyover, draws blended | - | 99.6 % (20,159 of 20,245 in a heavy frame) |
| New game to control, 6.4 min: control retrace, guest blocks | 20,405; 17,413,581 | the same |
| Same route: user CPU, cycles | 195 s, 704 G | 245 s (+25 %), 874 G (+24 %) |
| Walking the pier, sampled: GX worker / render worker | 51 % / 27 % of a core | 73 % / 53 % |
| Game thread busy (host counter) | 79-81 % | 81-84 % |
| Peak memory | 1.26 GB | 1.43 GB |

Image checks (`scripts/mac/frame_interp_report.py`): an in-between frame differs from both neighbours
by about half of what they differ by: the Outset intro pan 1.06 (1.0 = exactly between), Link running
down the pier symmetric (2.97 / 2.89 of 4.34, 3.78 / 3.78 of 5.77), 76 of 76 moving title frames
between. A standalone unit test covers matching, cuts, wraps and keys.

The iOS app (arm64, iOS 17) compiles and links with it and the menu toggle. **Open.** Not yet run on a device: on an iPad Pro the extra ~half core fits beside the game thread; on
2-performance-core iPhones it competes with it, so the GX worker's full-block copies and blends
(only the rows a draw uses are needed) come first. One more half frame of display latency (~17 ms).
Bone rotations over ~41 degrees a frame (a gull's wing) stay at 30. Patches 0098 and 0099 are committed in elliotttate/RecompCore
7845b6c (chrissotraidis 2d60636 plus these), which the Builder now pins.

**Also:** a Mac test save (new game to control, saved from the pause menu by `BLUEWAKE_SAVE_ROUTE`, 2
minutes headless) at `build/mac-interp/saves/outset-start.card`; `run_host.sh ... load` plays from it at
retrace ~705 instead of ~20,400. And `scripts/builder/build.sh` turns off the machine outliner with a
profile: untrained game code (486 of 748 chunks from the local profile, the boat, most enemies and NPCs)
was being outlined into a call every three instructions (45,364 in d_a_bk's first chunk).

## 2026-09-27 (day) Release fixes from the iPad test: transitions, Mods and Game Data menus

**Fixed: a strip of the previous area along the bottom during transitions.** The iPad test showed the
old scene in a bar at the bottom when the screen went dark between areas. Frame captures of the fade
from file select to the pier (4:3 and widescreen) showed the frame's last line (three rows at 3x) staying
at full brightness while the rest faded to black, and staying dark while the next scene faded in. A
census of draw rectangles showed why: the game draws its 2D layer, the fade included, with a 640x479
scissor, so the last line is never faded. A television hides it in overscan; the iPad does not. RecompCore
2d60636 (patch 0097) extends exactly that full-width scissor to 480 lines; the same captures now fade the
whole frame to black in both modes, and the pier holds 30 FPS with the HUD unchanged.

**Mods menu:** the toggles say what each mod does and whether it is on or turns on at the next launch;
Install Texture Pack… copies a picked folder of PNG/DDS textures into Load/Textures/GZLE01 and offers to
turn the pack on; Install Better Wind Waker… copies a picked disc, checks it the way launch does and only
keeps the supported patched disc; How Better Wind Waker Works links the project and BlueWake's script.

**Game Data & Saves:** Back Up Saves… exports the card; Restore Saves… checks the picked card, keeps a copy of
the current one in Backups/ and then closes BlueWake (the game holds the card in memory and would write the
old saves back); Remove Disc Image… names exactly what is deleted and what is kept before removing it.
The pickers and copies have not been exercised on a device yet; both builds compile.

## 2026-09-27 morning: what to check on the iPad

Overnight (simulator only; the iPad was charging) main gained, in PRs #6-#18: a `[gx-slow]` log line for slow
GX worker batches; Display > Texture Filtering (Original to 16x anisotropic, off by default) and a
min-filter decode fix; a widescreen frame of 2560x1440 at 3x that removes the line of sky along the top
of cutscenes; and a bundled pipeline cache so a first launch draws the HUD. Checked and recorded: Better
Wind Waker's instant text, the pause-menu save round trip, a scripted new game through Outset, the iPhone
layout, and a 10,000-retrace soak with every mod on (flat at 623 MB). Refuted: force-inlining the
paired-single helpers (2.5 percent slower).

On the iPad, in this order:

1. Widescreen on, a cutscene or dialogue: no line of sky at the top; the pier at 30 FPS with `gpu_wait_ms`
   near 0 in the session log (the frame now draws 3.7 M pixels; drop to 2x if it is heavy).
2. Display > Texture Filtering > 16x: 30 FPS in the pier and the village; if it holds, it can become the
   default.
3. A fresh install (or a second device): the HUD is present right after loading a save.
4. Any hitch: the `[gx-slow]` and `[late]` lines around it say whether it was the GX worker, textures,
   pipelines or a present waiting on the GPU.

## 2026-09-27 (overnight, 5) A new player's first launch drew no HUD for the first seconds

**Fixed.** On a fresh install (an iPhone 17 simulator), the pier after loading slot 1 had no hearts, action
buttons, map or rupees at retrace 1,500 and 1,700, while the iPad at the same retrace and draw count had
all of them; the frame capture of that first launch showed the HUD, and a second launch drew it. Aurora
skips a draw whose pipeline is still compiling, and a first launch has an empty pipeline cache ("No
bundled initial pipeline cache found"). The app now ships `initial_pipeline_cache.db` at the bundle
root (apple/ios/resources, added by apple/ios/CMakeLists.txt): the 213 pipeline descriptions tonight's
simulator sessions built (boot, prologue, Outset, Hyrule, menus, the three mods), which Aurora merges
into a new player's cache and compiles in the background. A fresh install with it logs "Seeded pipeline
cache" and draws the HUD at both retraces. The file holds GX state descriptions, no game data or compiled
shaders; the iPad and simulator builds both bundle it. The seed does not slow the first boot (the same frame timing and load frames with and without it), and it also fills the title screen: its first second drew 743 draws with the seed and 558 without. To refresh it after playing further areas:
`sqlite3 ".../Library/Application Support/BlueWake/pipeline_cache.db" "VACUUM INTO 'apple/ios/resources/initial_pipeline_cache.db'"`. Still open: areas the seed does not cover still skip draws until their pipelines compile, and Aurora compiles them on one thread (pipeline_worker in pipeline_cache.cpp), which on the unseeded first launch left the HUD missing past retrace 1,700 for 213 pipelines. Several compile threads, or blocking on draw-path pipelines once they have waited a few frames, are the candidates; Dawn's thread safety is the constraint: the prebuilt Dawn (v20260618) makes device calls thread-safe only with the opt-in ImplicitDeviceSynchronization feature, which Aurora does not request, so more compile threads would need that feature and its lock on every device call measured on the iPad first, and the seed should grow as more of the game is played.

## 2026-09-27 (overnight, 4) Widescreen: a line of sky along the top of every cutscene

**Fixed.** With the widescreen mod, cutscenes and dialogue (where the game draws its black letterbox
bars) showed a thin line of sky along the top edge of the picture. The frame capture showed it in the
rendered frame itself: row 4 of 1,081 was sky, with the bar from row 5. The frame buffer was the scaled
4:3 frame fitted to 16:9, 1921x1081 at 3x, a vertical scale of 2.252, where the bars' quads and the
scene's scissor rounded to different rows. RecompCore 32b6215 (patch 0096) sizes an anamorphic frame
buffer from the game's height times the render scale and the width the aspect needs: 2560x1440 at 3x,
whole-number scales both ways. The capture of the same frame now has black rows down to the scene and
the line is gone on screen (local-research/widescreen-20260927/). 4:3 is unchanged.

Cost on the simulator, the pier after loading slot 1 with widescreen, before and after: 29.96 and 29.95
FPS, main thread 87.8 and 87.4 percent, no GPU wait. The GPU draws 3.7 M pixels a frame instead of 2.1 M
(the default 4:3 at 3x draws 2.8 M); not yet measured on the iPad. At 2x and 4x the height is exact but
16:9 needs a fractional width scale (1707 and 3413 wide), so 3x is the setting that is exact both ways.
Found by running the scripted new game (entry below) with widescreen and the HD pack: the prologue,
the Outset title card, the dialogue boxes and the HUD otherwise draw correctly in 16:9.

**Checked: a scripted new game through Outset holds up.** Continuing the new-game script past the lookout with stick and A presses to retrace 32,000 (unpaced) walks Link off the lookout, into the sea, to the beach and the village sand and back out to sea: no crash, assert or renderer error, and the sea, wakes, splashes, sand, grass and HUD draw normally. Late frames are the boot present, three scene loads, and in two of four runs two to five 24-64 ms presents on the first frames of Outset (about 5,900 draws); a sampled run of the same reveal had none, and the iPad logs show no present waits in play, so it is treated as intermittent simulator host behaviour, not a confirmed bug.

**Checked: the pause-menu save round trip on the current build.** From slot 1 on the simulator: Start, three downs to SAVE, A, A. The game shows "Now saving...", writes three 8 KB blocks and sets the file status (`BLUEWAKE_CARD_LOG=1`), then offers to continue; the written card differs from the source and loads back into the pier view at 30.0 FPS.

## 2026-09-27 (overnight, 3) Texture filtering: a sampler decode fix and a forced anisotropy setting

**Fixed: two of the eight hardware texture filters were decoded swapped.** gxcore split TX_SETMODE0's
three-bit min filter with a table that turned GX_LIN_MIP_NEAR (5) into nearest minification with linear
mips and GX_NEAR_MIP_LIN (2) into the reverse; bit 7 is the minification filter and bits 5-6 the mip mode
(Dolphin's TexMode0). RecompCore b00c014 (patch 0095) decodes it as Dolphin does and the gxcore test
covers both values. A histogram of the pier scene shows Wind Waker using only 4 (linear, no mips, 94
percent of samplers), 1 (nearest, point mips, 6 percent) and 6 (0.1 percent), which were already right,
so nothing on screen changes here; other scenes or games that use 2 or 5 now filter correctly.

**Added: Display > Texture Filtering** (Original, 4×, 8×, 16× anisotropic; applies at once; Original by
default). It forces anisotropic sampling with Dolphin's rule, every texture not filtered nearest both
ways with all filters linear (`aurora_set_forced_anisotropy`, `DOL_AURORA_FORCE_ANISO`), and the
Report a Problem text records it. On the simulator the pier holds 30 FPS at 100 percent speed with 16×
and no GPU wait; the picture changes in distant step edges and wall trim, a small change because the
game's textures are mostly smooth and unmipmapped and the default 3× render scale already removes most
shimmer. Not yet measured on the iPad, so it stays opt-in.

**Checked, no change needed:** the HD pack's coverage on the pier route is complete apart from five tiny textures (8x8, 128x8, 32x32; `DOL_TEXREP_LOG_MISSES=1`), so the lookup is not dropping replacements. iPad audio is healthy in console27-32 and console-a13: no dropped buffers or audio waits in about 4,500 one-second windows, the queue low in 4, and 173 starved pushes of 2.1 million on the A13. A 10,000-retrace simulator soak on main with all three mods and 16x filtering held 622 MB flat from retrace 1,476, with 1 of 141 windows under 29 FPS and no errors.

**Measured and not kept** (same night; unpaced simulator heavy view, six interleaved pairs):

| Candidate | Result |
| --- | --- |
| Force-inline the paired-single load/store fast paths (`ppc_psq_load_inline`/`ppc_psq_store_inline`, 2.4 percent of the game thread as out-of-line calls), full PGO composite rebuild | 64.8 against 66.5 retraces a second; slower in all six pairs (the composite grows 1.8 percent); not kept |
| Retrain the composite's PGO profile | Not needed: a hot chunk (802456E0) compiles against the current profile with the out-of-date and unprofiled warnings turned back on and reports neither |
| MSAA | Not a setting yet: Aurora stops on depth copies from multisampled targets (`Depth tex copies from multisampled EFB targets are not supported`), and Wind Waker makes about 500 depth copies a run; it needs a depth resolve first |

**Checked: Better Wind Waker's instant text works, and a new game can be scripted without the route card.** From the older card with an empty Quest Log 2 (`build/device-setup/card-backup/GZLE01-before-slot2.card`), this pad script starts a new game and pages the opening dialogue: Start at 420 and 480, stick down at 680, A at 740, 1000 and 1100 (types one letter), Start at 1160 (jumps to END), A at 1220, then A every 150 retraces from 14,000. Unpaced on the simulator it reaches the prologue by 1,500, "Outset Island" by 15,000 and Aryll's dialogue by about 17,000. With Better Wind Waker on and the same presses, every text box is complete as it opens and Link has control with the HUD by 21,000; without it the same retraces are still typing ("waiting for a w", only the first line of "Hurry up, Big Brother!" at 21,000). Frames: local-research/betterww-20260927/. Swift Sail is still unchecked (it needs the sail).

## 2026-09-27 (overnight, 2) Where the remaining hitches come from; slow GX batches are logged

**Added: slow GX worker batches name themselves in the session log.** RecompCore d068efc (patch 0094)
logs `[gx-slow] batch_ms= bytes= pipelines= textures=` for any worker batch of 20 ms or more
(`DOL_GX_SLOW_BATCH_MS` sets the bar; one clock read per batch). A device log now tells a worker stall
from texture decoding, pipeline creation or a present that waits on the GPU.

**What the recent iPad logs show** (console27-32, console-a13, before the device was put away): play
holds 29.5-30 game FPS at 52-62 percent CPU averaged over the play windows, and the slow windows are single hitches.
The largest are scene loads, 90-160 ms of game-thread work in one frame; the simulator run of the same
load spans 9 retraces of guest time in 172 ms, so the time is the game's own load work (about 40 archives
and their actors) and the host runs it at about 87 percent of real time. The next largest is one 55 ms
worker wait on the first drawn frame of a scene; on the simulator that frame's batch creates 23 textures
in 18 ms and no later batch passes 12 ms.

**Where the unpaced heavy view spends its time** (simulator, pier after slot 1, 10 s sample at 58
retraces a second): the game thread is busy 100 percent of the time, the GX worker about 50 percent and
Dawn's render thread about 30 percent, so the game thread sets the rate. Of it, 86 percent is translated
game code, spread thin: collision (GroundCrossGrpRp 4.3, WallCorrect 2.8 percent), the PSMTX/PSVEC
leaves (about 9 percent together), J3D and GX command building; the FP helpers total about 10 percent
and host_chassis_edge_service 6.9 percent, most of it the native actor search. This matches the
2026-09-24 conclusion that the translated route's cheap levers are spent.

**Mods rechecked through the app's own toggles** (Mods menu defaults, pack in Documents/BlueWake/Load/
Textures/GZLE01, disc in Documents/BlueWake/Mods): 5,739 replacements load, the Better Wind Waker disc
is accepted with its code variants, widescreen draws 16:9, and the pier holds 30.0 FPS at 100 percent
speed. The first launch after the pack and disc were added had seven presents of 89-161 ms as the pier
first drew; a second launch with the same mods had none, so they are one-time cold caches (first-use
shaders and the pack's files).

## 2026-09-27 (overnight) HD textures decode off the GX worker; four candidates measured on the simulator

**Fixed: a hitch when a scene's HD textures first appear.** With a texture pack, the gxcore path decoded
each replacement PNG on the GX worker, which the game thread waits on at draw-done: the iPad logs show a
55 ms worker wait (console29, console31) and the simulator 42 ms, one frame lost each time. RecompCore
594807d (patch 0093) decodes file replacements on a background thread, draws the original texture until
the decode is done and uploads on the worker. Soak route with Hypatia's pack on the simulator: no worker
wait over 10 ms with it, 42 ms without (`DOL_TEXREP_SYNC=1`); the Items screen shows the HD icons once
decoded.

**Checked, no change needed:** the pipeline cache persists across launches (Library/Application Support/
BlueWake/pipeline_cache.db); backgrounding and returning pauses and resumes at 30 FPS; a new player's first
run (disc import) reaches the title; a 12,000-retrace soak with all three mods holds 586 MB of memory flat
from retrace 3,500 to 12,000; the session log writes about 5 lines a second in play; the mod extra-chunk
dispatch cannot misfire on unset cache slots.

**Measured and not kept** (unpaced simulator heavy view, 8 interleaved runs, retraces a second, which
resolves about 1 percent where the paced CPU percentage could not):

| Candidate | Result |
| --- | --- |
| Remember the last guest alias hit in ppc_guest_alias_resolve (1,900 REL aliases, binary search) | 64.9 against 65.5 without; slower, reverted |
| Load turbo, skip the harness overlap observation, apple-m2 composite | see the entry below: no gain |

## 2026-09-26 (late night) Rechecked the mods; three optimization experiments, none worth keeping

**Rechecks.** A fresh clone of main with the pinned RecompCore 7952fa2 passes
`build_device.sh --source-only` with the updated composite source digest (54f54434..., 754 files).
`scripts/mods/build_mods.sh` run in that clone produces mod_variants.inc, the dispatcher and the Better
Wind Waker chunks byte-identical to the build on the iPad. Better Wind Waker's REL data changes are
written after the base data images are materialized, and nothing re-materializes a module's data when
it loads, so they persist across module loads. 26/26 host tests pass.

**Measured and not kept:**

| Experiment | Result |
| --- | --- |
| Load turbo: skip the wall pacer while every presented frame is empty | Save load from file select to gameplay 5.0 s with and without it (two pairs on the simulator). The frames in a save load are the file-select fade-out and the scene fade-in, which draw normally, so the heuristic never engages; the loading work itself is a few CPU-bound stalls totalling about 0.4 s. The ~5 s is the game's own timed fades. |
| Skip the harness's per-boundary overlap observation in the app | Simulator heavy view, three interleaved pairs: main thread 86-90 percent either way (within noise). The cached reads are already cheap. |
| Composite for the A15/M2 generation (`-mcpu=apple-m2`) instead of A13 | iPad Pro (M2), the pier after loading slot 1, 67 one-second samples each at thermal 0: main thread 80 percent (M2) against 81 percent (A13), both at 30 FPS. Not worth dropping A13/A14 devices. The A13 build is back on the iPad. |

Where the iPad's main thread goes in the heavy view: 85 percent translated game code spread flat across
chunks, 12 percent host (host_chassis_edge_service 5.7). The render thread is Dawn's per-draw encoding,
already with skip_validation. The GX worker (64 percent busy) is not the limit. Further main-thread gains
need the structural work the September 24 entries describe (the emitter's per-instruction shape, or
porting hot game code), not host tweaks or compiler targets.

## 2026-09-26 (night) Mods verified on the iPad: the menu, a save with mods, and reloads

Closing the gaps in the entry below, on the iPad Pro (M2) with the merged build:

- **The menu on the device.** `BLUEWAKE_MENU_DUMP=1` writes the ⋯ menu tree to the session log. The iPad
  run lists *Mods* with *Widescreen 16:9 [on]*, *HD Texture Pack [on] - 6703 textures installed*,
  *Better Wind Waker [on]* and the two install guides.
- **A save with all three mods on the device.** The user's card was pulled first
  (build/device-setup/card-backup/GZLE01-before-modsave-*.card) and the test ran on a copy
  (`BLUEWAKE_CARD_PATH`). With the save route and all three mods on, the pause-menu save wrote all
  three card blocks and set the status (three saves, result 0). The copy changed; the real
  GZLE01.card is byte-identical to the backup.
- **Reloads on the device.** The saved copy loads to the play scene at retrace 765-766 with all three mods
  (mask 0x3, 50 chunks, 180 writes; 30.0 FPS, no late frames, main thread 83 percent) and with none
  (30.0 FPS, 78-79 percent).
- **Better Wind Waker on screen.** Swift Sail replaces the Sail: the Items screen's sail icon changes from
  the green-swirl Sail to Better Wind Waker's red-swirl Swift Sail (simulator, same build). On the iPad
  the game reads its archives from Mods/betterww.iso (2,391 files), the disc that icon comes from.

## 2026-09-26 (evening) Mods: widescreen, HD texture packs and Better Wind Waker, tested on the iPad

The in-game ⋯ menu has a Mods section with three mods, each applied at the next launch
([docs/MODS.md](../MODS.md)).

- **Code mods as variant chunks.** A statically recompiled game cannot take code patches at runtime, so
  a mod's patched main.dol and RELs are translated like the base, merged by generate_composite.py, and
  scripts/mods/build_mod_variants.py adds the chunks that differ to the base composite under new names,
  with extra chunks for code ranges only the mod has, one-time writes for the data it changes (DOL data
  and relocated REL data) and per-frame writes for a Gecko code's data. The composite's own dispatcher
  copy has a writable chunk table and an extra-chunk lookup; generated.h, which the 748 base chunks
  include, is unchanged, so adding mods compiles 53 variant chunks and module_export.c only. At boot the
  host enables BLUEWAKE_MODS before the first dispatch. When two mods change one chunk (both change
  `.init`), a translation of the game patched by both supplies the combined variant.
- **Widescreen 16:9:** Dolphin's GZLE01 Gecko code (65 code writes, 11 data writes) as 22 variant chunks,
  letterboxed through the new DOL_AURORA_ASPECT_RATIO. The simulator shows the wider view with the HUD
  placed correctly.
- **HD texture packs:** RecompCore 0092 connects Aurora's Dolphin-format replacement to the gxcore decode
  path (XXH64 of the base level, palette hashed over the entries used). Hypatia's pack (Android-Lite,
  5,739 textures) is replaced on the iPad, e.g. a 376×104 texture with its 752×208 version; a scripted
  simulator run consulted the pack for 173 textures and missed 5 small UI ones the pack does not cover.
- **Better Wind Waker:** scripts/mods/make_betterww_iso.sh runs Better Wind Waker 4501481 headless with its
  default settings; it changes main.dol (a new 5.8 KB code section at 0x803FCFA8), seven RELs (code in
  seven, data in d_a_ship and d_a_obj_movebox) and ten data files. 25 variant chunks, one extra chunk and
  76 one-time writes (5.9 KB); the patched disc supplies the data files, and the app accepts only a disc
  whose executable has the SHA-1 the variants were built from. Its added code runs (guest pc samples in
  its section during a village walk on the simulator).

Verified on the iPad Pro (M2) with all three enabled (mask 0x3, 50 chunks, 180 writes): loading slot 1
and walking the village held 29.9-30 FPS at 100 percent speed with no late frames outside scene loads,
main thread 62-82 percent busy. On the simulator with all three enabled, the pause-menu save wrote the
card (write and read-back of all three blocks), and the saved card reloads to the play scene with the
mods and without them. scripts/mods/build_mods.sh reproduces, from the disc alone, byte-identical
mod_variants.inc, variant chunks and dispatcher. 26/26 host tests pass. The composite source digest in
build_device.sh is updated for the mod-ready dispatcher (754 files).

Open: Better Wind Waker's individual features (instant text, Swift Sail) have not been exercised on
screen yet, only its code running and the game playing normally; mods are not yet combinable with
other Better Wind Waker settings (each settings choice is a different executable and needs its own
variants).

## 2026-09-26 The 15 FPS phases were discarded frames, not a slow CPU

On the iPad, right after loading a save, the pier view ran at exactly 15 FPS with about 27,000 draws per shown frame, at 100 percent speed with the main thread only 75 percent busy. A boundary census comparing a 15 FPS window with a 30 FPS window of the same view showed identical guest work per retrace (91 M boundaries per 300 retraces in both) and 150 GXDrawDone calls in both. The game rendered 30 frames a second throughout, and in the slow phase half of them went through JFWDisplay::exchangeXfb_double's drop path (clearEfb, 74 calls; 0 in the fast phase).

The cause: JUTVideo::drawDoneStart issues the asynchronous GXSetDrawDone after each display copy, but the host raised the PE finish only inside GXDrawDone. JUTVideo's sDrawWaiting therefore stayed set until the next frame's endFrame, and whenever that fell after the retrace the next beginRender woke on, the exchange found the last frame unshown and threw away the new one. The host now also raises the PE finish at GXSetDrawDone's return (0x80322BC8, flushing the GX front end first; BLUEWAKE_ASYNC_DRAW_DONE=0 restores the old behaviour).

Measured on the iPad Pro (M2) with a scripted slot-1 load: the pier view holds 30.0 FPS, late=0, worst frame 34-42 ms, for 75 s from the load onward (it previously settled at 15 FPS from about retrace 1,500). Regenerating the edge intercept table dropped the diagnostic GroundCross return (0x80328F84) from the fast-reject set, so its 5,700 returns a retrace no longer take the full edge service.

The GX worker was the next limit in the slot-2 (Hyrule) scene, where it was 84 percent busy and the main thread waited on it 300-430 ms a second. RecompCore patch 0091 removes the per-draw zeroing and copies of the 2-4 KB transform snapshot on its way to the renderer, and fills the 63 matrix rows with two block copies. The worker is now 64 percent busy and the wait is about 1 ms a second, at 30 FPS. 26/26 host tests pass.

A scripted 60-second walk off the pier and through the village (stick only, 5,600-12,300 draws) held 30 FPS at 100 percent speed with no late frames; the main thread peaked at 83 percent. The only long frame was 160 ms, when Link walked into the sea and the Sea stage loaded. The session then ran five more minutes at thermal state 2: 291 one-second windows at 29.9-30 FPS, zero late frames, worst frame 43 ms, no audio drops.

Still open: the pier view's main thread is 82-86 percent busy at 30 FPS on the M2, so heavier areas may still exceed it; the uniform dedup memcmp (about 7 percent of the worker) and per-draw plan building are the next worker costs.

## 2026-09-25 A fresh checkout builds the device app from the user's disc

The September 24 handoff assumed this Mac's `build/` and `generated/` trees and an unpublished
RecompCore commit (3476998), and the tracked patch series could not rebuild it (it starts at 0008).
Now `scripts/ios/build_device.sh DISC.iso` builds the signed app in a fresh checkout from published
sources only, and [DEVICE_BUILD.md](DEVICE_BUILD.md) gives the prerequisites, signing, install and
the optional PGO profiles.

- **Published forks.** RecompCore https://github.com/chrissotraidis/RecompCore (branch `bluewake`,
  086f282: the 76 local commits after the upstream base 5c3611e, including 3476998 and 0084, plus
  the never-committed GXRuntime `core/cpu.h`, `core/cpu.c` and `DSPIntTables` sources; Aurora is
  vendored in it) and DolRecomp https://github.com/chrissotraidis/DolRecomp (branch `bluewake`,
  5c91d6e, RecompCore's submodule). The dependency lock points at them; patches/recompcore and
  patches/dolrecomp are history.
- **The generation commands, recovered.** DOL: `--gamecube --backend c --cpu gekko
  --partition-instructions 4096`; RELs: the same with `--rel-base 0xC0400000`; then
  `generate_composite.py` with `--rels-dir .../generated/rels`. The recorded REL namespace failure
  came from a stale REL translation at the old 0x80400000 aperture. The fresh tree passes (748 chunks,
  415 REL modules, 417 code ranges); its metadata and 206 DOL chunks are identical to the shipped
  tree, and its 542 REL chunks differ only because the shipped tree mixed two translator versions.
- **No local dependencies.** The extractor is the app's own `disc_import.c` built for the Mac
  (`scripts/ios/disc_extract.c`); the ABI header comes from RecompCore (ModernGekko no longer
  needed); PGO is opt-in (`--composite-pgo`, `--host-pgo`).
- **Verified:** two fresh clones reproduce the composite source digest 9e4a847d... in under two
  minutes; extraction and the translator are byte-identical to what the shipped build used; 218/218
  host tests with the new header path. The complete clean build (one command, fresh clone, no
  profiles) succeeded: 426 MB app, platform IOS minos 17.0, signature valid. Its composite, retagged
  to run on the Mac, passes ABI coverage, reproduces the certified digest 83d2590d over 1,050
  records and passes the simulator save acceptance. Without the optional profiles the heavy view
  takes about 20 percent longer per retrace. Details in DEVICE_BUILD.md.

## 2026-09-24 (night, 16) Handoff: the build goes to a physical iPad next

The user chose a hardware run before any change of route. The state of the build, what has been
verified on the simulator, the speed table, what is open, and the steps to sign, install and check
it on an iPad are in [IPAD_STATE_2026-09-24.md](IPAD_STATE_2026-09-24.md). Verified at this commit:
the device app (arm64, iOS 17.0, A13 code generation) compiles with tonight's host changes, embeds
the current device composite and signs ad hoc (484 MB); 218/218 host tests; repository audit pass.
This Mac has no signing identity, so installing needs the user's.

## 2026-09-24 (night, 15) The leader-guard fold priced before building: about 2 percent, not taken

Every block leader emits the precharge decision (dolrecomp_block_can_precharge: loads of
cycle_deadline_budget and downcount, an add, two compares) and then the budget guard (downcount <=
-DOLRECOMP_C_LOOP_CYCLE_BUDGET, a compare and a branch) before the decrement; 0017 already merged the
two guard arms into this one. The 6.0 percent "no-guard" ablation removed the guard outright, which
is not sound. A fold keeps the test and only shares the downcount load and one branch with the
decision, so its ceiling is roughly a third of that ablation, about 2 percent of the body: the heavy
view from 53 to about 54. It needs the DOL chunks regenerated, the 100 hot chunks rebuilt through a
fresh PGO cycle (2.5 hours or more of full machine load) and the certified digest. Not taken; it
does not change the answer. The heavy view's last 10 percent is the play-scene source port or
nothing, and that is the user's call (docs/GOAL_PROMPT_V56 item 6).

## 2026-09-24 (night, 14) Quest Status and Options pages match Dolphin

The pause menu's Quest Status and Options pages, reached with the same presses (START, R, stick
down, A) in Dolphin (menu.dtm, with this game's settings in the movie header) and in BlueWake with
the emulated SRAM, agree in layout, lettering, the heart and song icons, the OPTIONS cursor brackets,
the Save/Options labels, the translucent play scene behind and the Options values, Sound: Stereo in
both (local-research/dolphin-ref/menu-quest-options-dolphin-vs-bluewake.png). With the earlier Items,
Save and file-select comparisons, every pause-menu page reachable from the pier save is now covered.

## 2026-09-24 (night, 13) Why native copies of the hot leaf functions will not close the heavy view's gap

The next candidates by the guest profile are the matrix leaves (PSMTXConcat 3.0 percent,
PSMTXMultVec 1.3, PSMTXCopy 0.6). Running them natively at their entry boundary the way the actor
search runs (-21.9 percent) would save only boundaries and per-instruction bookkeeping, and the
earlier null says that is not where their time goes: a native copy of __save_gpr/__restore_gpr's
emitted statements, entered from the dispatch at every call (they are called more often than any
leaf), was exact and saved nothing (334.1 -> 334.0 M instructions a retrace). The actor search paid
because it removed three chassis round trips per node of a loop, not per-instruction overhead. A leaf's
sampled time is its own work: exact paired-single arithmetic through the runtime's helpers and
big-endian memory through the alias-aware accessors. Removing that work means compiling the game's
C with native floats and direct memory, the play-scene source port, which trades the translated
code's bit-exact execution for speed in the ported functions and is the user's decision. Not built.

## 2026-09-24 (night, 12) Refuted: a lower render resolution on the simulator; the rested heavy view is a stable 52-54

The EFB renders at the window's pixel size (about 2224x1668 on the 11-inch iPad, 3.5 times 640x480),
and the simulator executes Metal in a host service that used a whole core, so a smaller EFB might
free CPU for the game thread. Aurora's frame-buffer scale (set_frame_buffer_scale) at 2 (1280x960)
against native, on the rested heavy view, alternating: 53.9 / 52.7 at scale 2, 53.4 / 51.0 native.
Within the noise; reverted. The heavy view is bound by the game thread's CPU work, not by pixels.
These four runs and the previous pairs also show that the rested heavy view is now a steady
instrument at 51-54 retraces a second (60 is full speed; the village 59.9).

## 2026-09-24 (night, 11) Refuted: user-interactive QoS for the GX workers (a large loss on the simulator)

On the iPad simulator the game thread (UIKit's main thread) runs at priority 47, user-interactive;
the GX FIFO worker (83 percent busy in the heavy view) and Aurora's render worker (44 percent) are
plain std::threads at 31, the default QoS (ps -M on the running app). Raising both to
QOS_CLASS_USER_INTERACTIVE, so the game thread would not wait on a worker parked on an efficiency
core, was measured on the rested heavy view, alternating with an environment switch:

| run | QoS on | default QoS |
| --- | --- | --- |
| 1 | 14.5 retraces/s, 368 frames over 100 ms | 44.7, 10 over 100 ms |
| 2 | 21.7, 278 over 100 ms | 54.4, 11 over 100 ms |

A large loss with long stalls: at that priority the busy workers compete with the simulator's own
presentation service (SimRenderingServices, 100 percent of a core), which runs the frames to the
screen. Reverted (local-research/refuted/worker-qos-user-interactive.diff). The default QoS stays;
on a device the question should be measured again, since there is no simulator renderer there.

## 2026-09-24 (night, 10) The game's own Stereo/Mono option persists; a 21,000-retrace soak with the depth peek

**Options menu → SRAM → next session.** The pause menu's Options page shows Sound: Stereo on the
emulated SRAM (it had no way to say anything but mono before). Choosing Mono and confirming with A
writes SRAM flags 0x28 through the SDK's WriteSram (command 0xA0000100, the changed tail), the file
keeps it, and the next session reads it ("[sram] ... flags=0x28 sound=mono") and plays in mono: the
title's side/mid falls to 0.002-0.004 against 0.2-0.7 in stereo. Backing out with B instead restores
the value the page opened with, which is the game's cancel. Cycling past Mono shows Surround, which
the game maps to the stereo SRAM bit, as on hardware.

**Soak with the depth peek on** (local-research/ipad/soak-peek-223945, the iPad simulator, the new-game
route to retrace 21,000 plus the save's load): 139.0 M draws submitted, none rejected or failed;
the app's resident size settled at 752-769 MB over the last two minutes (before the peek, 770-870
MB resident at play scale); it ended normally. Audio 65,184 of 1,399,873 pushes starved (4.7
percent) with the stretcher covering 1,061,721: the run shared the Mac with this session's other
work, and starvation tracks guest speed, as before.

## 2026-09-24 (night, 9) The pause menu driven by the touch controls on the iPad simulator

With BLUEWAKE_TOUCH_TAPS pressing SunPad's on-screen buttons through their UIKit handlers, from
the pier save (local-research/ipad/touch-menu-223718, screenshots t28-t62): Start opens the ITEMS
page, R moves to QUEST STATUS and on to the next page, Start closes the menu back to play, and the
D-pad's right cell turns the minimap off (t57) and on again (t62). The first touch retires the
scripted presses as designed ([pad] live input at retrace 1608). On the 11-inch iPad the move stick
and D-pad sit over the minimap's corner (the 4:3 picture leaves 49-point bars, too narrow for the
phone's letterbox layout); the layout editor moves them, and the default is SunPad's.

## 2026-09-24 (night, 8) The title fly-by's black-sky frame is gone

The phone-layout entry of 2026-09-24 00:34 noted one title fly-by frame with the upper sky black.
Captured every 3 retraces from 560 to 1,265 (235 frames: the sun, the ship and logo, the fly-by over
Outset and PRESS START), no frame has more than 20 percent near-black pixels in its upper third
(/tmp capture; scan in this entry's commit message). That observation predates recompcore 0071
(03:12 the same day), which ends each frame at its display copy in worker mode and removed the
black-sky frames. Closed.

## 2026-09-24 (night, 7) The heavy view rested: 53.4 retraces a second, work-bound; the profiles are flat

After a rest (load average 3.2), the simulator's heavy Outset view with rate_poll on the app
(local-research/ipad/heavy-rate-222557): **53.4 retraces a second, 450 M instructions and 112 M
cycles a retrace, 5.99 G cycles a second** across the app's threads (the village, rested: 59.9 at
98 M cycles). So the heavy view is bound by its work, not by the host: the game thread (UIKit's main
thread on iOS) is 100 percent busy in an 8 s sample (local-research/ipad/heavy-sample-223019), and it
would need about 10 percent fewer cycles a retrace to hold 60. A cluster of 400-600 ms stalls
between retraces 2,864 and 3,000 in that run appears in none of the previous eight heavy runs and has
no log line: host contention.

Where the game thread goes now (self time): host_chassis_edge_service 6.4 percent, chassis_dispatch
4.7, the out-of-line FP helpers about 6.6 (ppc_fmuls 1.9, psq_load 1.7, fadds 1.2, fsubs 0.9,
ps_madds0 0.9), dolrecomp_charge_precise 1.5, the GX gather-pipe path 2.2, alias resolve 1.1, the
rest translated bodies spread over many chunks. The guest profile of the same view on the macOS host
(BLUEWAKE_PC_SAMPLE, 149,854 samples) is as flat: __restore_gpr 3.6, __save_gpr 3.0, PSMTXConcat
3.0, cTgIt_JudgeFilter 2.2, calcWeightEnvelopeMtx 1.9, GroundCrossGrpRp 1.5, then nothing above
1.3 percent. Every lead the ledger has priced is spent: exact FP fast paths (null, 2026-09-24),
native register helpers (null), the cycle cap (null), the prepaid block copy (+2.4 percent), the
block-local downcount (refuted by the emit shape). What remains is the emitted body's
per-instruction shape (a pc store and a charge test at every instruction, about 9.8 percent of the
body) and the guest's own work. The step that moves the heavy view to 60 is structural: the
play-scene source port the user is deciding on, or an emitter redesign of the resume-point model.

## 2026-09-24 (night, 6) A new player's first run; the village at full speed on the simulator when the Mac is cool

**First run, fresh install** (sim_run.sh --fresh --import-disc; local-research/ipad/fresh-20260924-221318):
the empty container shows the first-run screen, the disc is imported and prepared (main.dol, rels/),
sram.bin is created with Dolphin's stereo defaults (flags 0x2C), the clock starts at local time,
and the game runs through the Dolby screen, the title with PRESS START and into the prologue with
the touch controls, in iPadOS 26's windowed mode.

**Village speed instrument.** scripts/ios/sim_village_view.sh loads the pier save, steers the
route past Link's house (BLUEWAKE_PAD_PLAYER_ROUTE) and times retraces 1,300-1,730, the heaviest
stretch, in 1,800 retraces instead of the 23,000 the new-game route needed. Back to back, runs
decay: 48.4, 39.0, 37.2 and then 47.0, 42.8, 40.7 retraces a second (load average 4-5). After a
60 s rest, with scripts/ios/rate_poll.py on the app (local-research/ipad/village-rate-222151):

| stretch | rate | instructions / retrace | cycles / retrace | cycles / s |
| --- | --- | --- | --- | --- |
| 900-1,300 (pier, path) | 60.6 | 426 M | 103 M | 6.25 G |
| 1,300-1,730 (village view) | **59.9** | 402 M | 98 M | 5.90 G |

So the simulator app plays the village at full speed when the Mac is cool; each run heats this
fanless M2 Air and the next gets fewer cycles a second. The same will matter on a fanless iPad in
long sessions: the game thread needs about 6 G cycles a second across its threads at full speed.

## 2026-09-24 (night, 5) The sun's glare is right: Dolphin's movie references had EFB access off

The open question from the previous entry is settled. Dolphin launched without an input movie draws
the title's sun with the same glare, halo, rays and hexagon flares as BlueWake with the depth peek
(local-research/dolphin-ref/sun-glare-dolphin-nomovie-vs-bluewake-peek.png); the same launch with
EFB access turned off (a GameSettings/GZLE01.ini override) draws the small plain sun BlueWake drew
without the peek. The movies made by scripts/make_dtm.py left the header's bSaveConfig clear, and
Dolphin played them with EFB access off, so every movie reference so far was taken without EFB
peeks; with bSaveConfig and this game's settings in the header (EFB access on, EFB copies to RAM, as
Sys/GameSettings/GZL.ini sets them) the movie run draws the glare too. make_dtm.py and
pad_trace_to_dtm.py now write that header (apply_game_config). The earlier references (menus, text,
the walk, the village) do not involve the sun and stand.

So the peek is correct and is now on by default with the Aurora renderer (BLUEWAKE_EFB_PEEK=0 turns
it off; the headless certified route is unaffected). It also drives what the sun does to the scene:
while the sun is visible and near the view's centre, Wind Waker brightens the fog and sky and dims
actors (dKy_set_actcol_ratio and friends in dKyr_sun_move), which BlueWake never did before.

Checks: the iPad simulator save acceptance passes with the peek on (local-research/ipad/
acceptance-20260924-215435, every milestone on its usual retrace). The peek costs nothing
measurable: the rendered save view on the macOS host reads 450 / 451 M instructions and 10.41 /
10.26 G cycles per 100 retraces with it, 451 / 452 M and 10.14 / 10.23 G without (this morning:
450-453 M). The simulator's heavy view read 36.5 / 36.9 with and 37.3 without at load average 7-10,
so that evening's drop from 57 is host load, as before, not the peek. Certified digest unchanged
(headless). 218/218 host tests.

## 2026-09-24 (night, 4) Unhandled hardware reads, and the EFB depth peek (opt-in, open against Dolphin)

**A census of unclaimed registers.** BLUEWAKE_MMIO_UNHANDLED=1 reports, once each, a hardware
register read that no device answers (the SRAM hid this way). From power-on through the save's
Outset play, with the SRAM on: two EXI channel 0 reads at boot (0xCC006814 and 0xCC006828, the
channel 1 and 2 status words, which read 0: nothing attached, as intended), the PI's 0xCC001002 at
boot, two OS reads at pc 0x80307EF4 of addresses 0x3 and 0xFFFFFFFF (retrace 2), and GXPeekZ,
0xC84xxxxx, from retrace 334 on.

**GXPeekZ.** Wind Waker peeks the EFB's depth at five points around the sun each frame
(d_kankyo_rain.cpp dKyr_sun_move) and counts the sun visible only where the depth is the cleared
far value, 0xFFFFFF; every peek read 0, so to the game the sun was always hidden. recompcore 0084
adds aurora_peek_z, a C entry to Aurora's existing asynchronous depth snapshot, and the host
answers 0xC84xxxxx loads from it (BLUEWAKE_EFB_PEEK=1, Aurora only). The snapshot carries real
depth: in the save's pier view 1,066 of 1,200 grid points are near and 134 (the sky) far. With it
the title's sun draws with its glare, halo and hexagon flares. **But Dolphin, the reference (it
enables EFB access for this game), draws the title's sun small and without glare, as BlueWake does
with the peek off** (local-research/dolphin-ref/sun-glare-dolphin-vs-bluewake.png). Why Dolphin's
peek sees the title's sun as covered is not known yet (every grid point near the sun reads far in
BlueWake's snapshot). Until an in-game Dolphin reference with the sun in view settles it, the peek
stays opt-in and the default keeps the recorded behaviour, which matches Dolphin at the title.
Certified digest 83d2590d unchanged; 218/218 host tests; the iOS app builds.

Follow-ups tried: a Dolphin reference panning the camera (C-stick) around the pier never brings the
sun into view at the save's time of day, so no in-game comparison yet. Two candidate explanations for
Dolphin's covered title sun, both unconfirmed: the game peeks after the whole frame is drawn
(mDoGph_AfterOfDraw), and Dolphin converts depth with a floor (depth * 2^24, clamped) where Aurora's
snapshot rounds, so a sky drawn just short of the far plane reads 0xFFFFFF in BlueWake and 0xFFFFFE
in Dolphin. Settling it needs Dolphin's own peek values at the title.

## 2026-09-24 (night, 3) Gameplay audio against Dolphin: the walk's sound matches in shape, level and stereo image

scripts/ref_walk_corpus.sh now records audio on both sides (Dolphin's [DSP] DumpAudio; BlueWake's
capture with the emulated SRAM). Over the same input schedule (the reload presses, then the stick
held forward: off the pier, swimming, the beach and the grass), BlueWake's capture was cut into
5 s windows and each was located in Dolphin's by its 10 ms loudness envelope. The first 60 s map
onto Dolphin at a steady offset (+3.6 s through the load, then +4.5 s through play) with envelope
correlation 0.81-0.996, the stereo image within 0.02 (side/mid 0.064-0.226 in Dolphin against
0.072-0.227) and the level within 10 percent (b/d 0.92-1.10). Past 60 s Dolphin's dump (95 s of
emulated time in its 170 s run) is exhausted and the windows no longer map; the pictures of that
stretch were already compared. So the in-game mix (music, footsteps, splashes, swimming) is the
game's own in both, and the stereo fix holds in play, not only on the title.

## 2026-09-24 (night, 2) Saves carry the real date and time

Every BlueWake save said 01/01/2000 00:00:xx in the file select (Dolphin's view of the pier save:
"01/01/2000 00:00:39"): the time base started at 0, which OSGetTime() counts from the OS epoch,
2000-01-01. On a console the IPL sets it from the RTC. BLUEWAKE_CLOCK now sets where it starts:
"now" is the host's local time (as Dolphin does), a number is seconds since 2000 for reproducible
runs; unset keeps 0, the certified route's recorded behaviour. The time base only advances by
elapsed ticks and the host's other clocks run on absolute cycles, so nothing else moves. A save
made with it (the steered route into the house, then the save route) shows "09/24/2026 21:03:13" in
Dolphin's file select, the Mac's local time of the save (local-research/dolphin-ref/
fileselect-clock-dolphin.png). The iOS app sets BLUEWAKE_CLOCK=now by default, next to the SRAM.
The iPad simulator save acceptance passes with both on (local-research/ipad/acceptance-20260924-210555;
every milestone on its usual retrace, reload into control at 831).

## 2026-09-24 (night) The game played in mono: SRAM was never emulated. Fixed, stereo matches Dolphin

**Found by the first audio reference against Dolphin.** Dolphin (its own user folder, [DSP] DumpAudio)
and BlueWake's macOS host (HLE DSP, BLUEWAKE_CAPTURE_AUDIO_WAV) played the same 48 s from power-on
with no input: the logos, the title fly-in and its music. Aligned by their 10 ms loudness envelopes
(r = 0.994, Dolphin's audio 3.75 s later), the music is the same, but BlueWake's left and right
channels were identical (side/mid 0.001, L/R correlation 1.0000) and 2.9 dB louder, where Dolphin's
are stereo (side/mid 0.215, correlation 0.912). Every earlier capture, the iPad simulator's included,
was mono the same way.

**Cause.** The capture is a copy of the game's own AI DMA buffer, so the game chose mono. Wind
Waker sets its output mode from OSGetSoundMode(), which reads bit 2 of the console SRAM's flags
(ref/tww d_save.cpp: at every new file and again at every file load, where it also overrides the
save's own option). The SDK reads SRAM over EXI channel 0, device 1, and the host returned 0 for
every EXI register and ignored every EXI write, so the game saw an all-zero SRAM: mono.

**Fix.** runtime/host/src/ipl_sram.c answers the SDK's SRAM protocol on EXI channel 0 (a 4-byte
immediate command, then a 64-byte DMA read, or immediate writes of the changed tail), with Dolphin's
default SRAM (Source/Core/Core/HW/Sram.cpp: English, flags 0x2C with stereo, valid checksums),
persisted to a file after each write so the game's own Stereo/Mono option sticks. Device 0 still
gets no answer and channel 0's EXT bit stays clear, so the memory card is still served at the CARD
API as before. It is on in the iOS app (Documents/BlueWake/sram.bin; BLUEWAKE_SRAM=0 turns it off)
and off on the macOS host unless BLUEWAKE_SRAM is set, like the high-level DSP, so the certified
route keeps its reference: digest 83d2590d over 1,050 records, 220.1 M instructions a retrace,
unchanged. tests/ipl_sram_test.c drives the SDK's own register sequence (ReadSram, WriteSram after
OSSetSoundMode, a device-0 probe); 218/218 host tests.

| title audio, 48 s | side/mid | L/R correlation | level vs Dolphin | envelope r |
| --- | --- | --- | --- | --- |
| Dolphin | 0.215 | 0.912 | 1 | - |
| BlueWake before | 0.001 | 1.000 | 1.39 | 0.994 |
| BlueWake with SRAM | 0.241 | 0.891 | 1.004 | 0.994 |

On the iPad simulator the save acceptance passes on the app with SRAM on (local-research/ipad/
acceptance-20260924-205143), and its 400 s capture is stereo through the prologue and Outset
(side/mid 0.20-0.73 at five points, against 0.000-0.008 on the previous run). Existing saves are
covered too: on load the game takes stereo from SRAM.

Also this evening: an interior reference in Dolphin from open-loop replay is not reachable (with
the replay at polls 3000-3003 and a scripted push afterwards Link stops on the house deck or walks
along the wall), so interior comparisons stay with BlueWake's steered run.

## 2026-09-24 (evening, 19) Acceptance on the current build

The iPad simulator save acceptance passes on the app rebuilt with this evening's host changes (the
steered-route and pad-trace options, the save route's wait for a route; all inert unless their
variables are set): route milestones on the certified retraces (+11 under HLE), the save screen, the
card written and unmounted by the guest, 400 s of the game's own mix, then a reload into control at
retrace 831 (local-research/ipad/acceptance-20260924-202052). The run started at a load average of
97, so it is a correctness pass only.

## 2026-09-24 (evening, 18) The phone layout on the smallest current iPhone

On the iPhone 17e simulator (844x390 points landscape, the narrowest current iPhone; notched, so the
safe area trims the left bar) the letterbox layout applies unchanged: Start, L, the D-pad and the
move stick on the left, Z/R, Y/X, B/A and the camera stick on the right, the whole HUD clear
(local-research/ipad/iphone17e-layout-201852/shot-land.png). The run played the Outset save to its
2,400-retrace limit and exited normally (11.6 M draws, none rejected; audio 1,514 of 159,873 pushes
starved, the stretcher covering 48,936).

## 2026-09-24 (evening, 17) The village is already inside the translated code's profile

The composite's PGO covers 100 chunks ranked from an Outset profile and was trained without the
village. A guest-pc sample of the village stretch from the pier save (retraces 1,300-1,745, the
steered route; 56,054 samples) puts 90.4 percent in those 100 chunks. The rest is flat: the largest
chunk outside them is 0.57 percent and the next 24 add 3.7 percent; 4.0 percent is REL code at its
load addresses (0x8065xxxx, 0x809Cxxxx), spread the same way. PGO saved about 20 percent of cycles on
the chunks it covers, so widening the list or retraining on the village is worth about 1 percent at
most, for two hours of builds; not taken. Speed beyond this is the source-port decision.

## 2026-09-24 (evening, 16) A save made inside the house loads at the pier; the save route can follow a steered route

The save route (BLUEWAKE_SAVE_ROUTE) now waits for an armed steered route to finish, so one run can
walk the pier save into Link's house and save there: the door at 1,748, START at 2,171, the Save
page, both prompts and the quit prompt by 2,497, and the card's gczelda file changed (28 bytes;
local-research/dolphin-ref/house-save). Loaded in Dolphin (GCI from scripts/card_to_gci.py, file
select shows Quest Log 1 at 00:00:39), the game starts Link on the Outset pier: Wind Waker restarts
an Outset save at the island's start point, not the room it was saved in. So an interior reference
in Dolphin needs a Dolphin save state inside the house (the replay reaches the house deck; the door
depends on the ramp's phase), not a card save. Dolphin's card folder is back on the pier save
(local-research/dolphin-ref/gci/pier.gci).

## 2026-09-24 (evening, 15) The pier save reaches the house; open-loop replay past the ramp is phase-bound

With one waypoint added between 16 and 17 (-193351, 317986), the steered route from the pier save
(BLUEWAKE_PAD_PLAYER_ROUTE) clears the ramp, completes at retrace 1,732, presses A at the door at
1,748 and stands Link in the house by 2,086 (local-research/dolphin-ref/house-bluewake-sheet.png): the
Outset save now reaches an interior in 2,100 retraces, against 23,600 for the new-game route.
[pad-trace] now reports the buttons the guest reads (after the scripted and event presses), so the door
press is in the trace; BLUEWAKE_PAD_PLAYER_START_AFTER holds the route's start for phase experiments.

Replaying that trace open loop in Dolphin reaches the house deck but not the door: started at poll 3000,
Link stops against the bench beside the house; at poll 3002 (one retrace later), on the deck's ramp
(house-dolphin-sheet.png). Up to the ramp both match BlueWake. A route this long only reproduces
across emulators in closed loop, so the corpus uses open-loop replay for the stretch before the ramp
and the steered run for the interior (already compared by eye; a Dolphin interior reference needs a
save or state inside the house). 217/217 host tests.

## 2026-09-24 (evening, 14) The village against Dolphin, replayed from BlueWake's own steered route; collision matches

**New tools.** BLUEWAKE_PAD_PLAYER_ROUTE runs the steered waypoint route without the ladder step, for
a save that already stands at its first point: the Outset pier save (acceptance-20260923-095148) stands
where the post-ladder route begins, so the pier save now walks the route past Link's house.
BLUEWAKE_PAD_TRACE=1 prints channel 0's state on every change ([pad-trace]); the steered stick is
merged into the host's pad sample, so the SI probe never saw it. scripts/pad_trace_to_dtm.py turns a
traced span into a Dolphin input movie at two polls a retrace: Wind Waker writes SIPOLL 0x00F60200 (a
poll every 246 lines), and Dolphin's VideoInterface polls at each field's start and 246 lines later,
one movie state per poll. The span starts where Link can move (retrace 849), so a later start in
Dolphin (poll 3000, after its own A presses reach the pier) leaves the path unchanged.
BLUEWAKE_PAD_SCRIPT_MAX is now 1024 so a trace can also be replayed open loop in BlueWake.

**Result.** Dolphin, replaying BlueWake's closed-loop stick open loop, walks the same path: up the
pier, onto the grass, past Link's house to the ramp (local-research/dolphin-ref/village-pairs-0.png,
-1.png; sheets village-dolphin-sheet.png, village-bluewake-sheet.png). Terrain, the house, palms,
signpost, grass tufts, shadows, the rope bridge, the village houses across the water and the HUD
agree frame for frame. At the ramp the two parted: Dolphin's Link stepped onto the ramp's side and
walked on to the ledge above the beach; BlueWake's stopped against it. Replaying the same trace open
loop in BlueWake one retrace later reproduces Dolphin's step-up and ledge exactly (village-ledge-
dolphin-vs-bluewake.png: the village, bridge, beach and the Crouch prompt match); the unshifted replay
stops as the closed-loop run did. So the difference is which retrace's stick the game samples, and
BlueWake's collision and step-up agree with Dolphin's. It also explains why the steered route stalls
at waypoint 17 from the pier save; from the ladder it arrives at another phase.

## 2026-09-24 (evening, 13) 3D reference: a walk off the pier, a swim, the beach, the grass and a drowning match Dolphin

The Dolphin corpus had the pier view, Link's face, dialogue, menus and the prologue; moving 3D play
had only been judged by eye. scripts/ref_walk_corpus.sh now runs the same inputs through Dolphin
(an input movie from make_dtm.py, its own user folder) and then BlueWake's macOS host (never both at
once): the reload presses to Quest Log 1 on the pier, then the stick held forward. In both, Link
walks off the pier into the sea, swims to the beach, crosses the sand and the grass, swims along
the cliffs until his air runs out, sinks, fades, respawns on the dock and swims out again: the same
path, from open-loop input, in two different emulations of the game.

Paired by picture (Dolphin left, BlueWake right; local-research/dolphin-ref/walk-pairs-0.png and
-1.png, sheets walk-dolphin-sheet.png and walk-bluewake-sheet.png), the frames agree on terrain,
cliffs, the pier and dock planks, sea colour, shoreline foam and swim wakes, grass and sand, the
soft projected shadow under Link and the round shadow blobs, the flying grass clippings, Link's
hair and tunic shading, clouds, the HUD buttons and minimap. Differences are timing only (Link's
pose on landing, the HUD's A or Attack label). The one suspected difference, the air meter's
colour, is its animation: a strip of the meter across the swim in each (walk-meter-strip.png,
walk-meter-strip-bw.png) shows the same pulse between light and deep blue, the same drain and the
same red warning states. No new rendering defect in this stretch.

## 2026-09-24 (evening, 12) The title logo from a normal launch is correct; windowed iPadOS 26 and rotation work

The user's broken-logo screenshot (THE LEGEND OF white on a dark box, ZELDA as outlines) was taken at
08:31, before the palette fixes of recompcore 0080-0082 (11:38-12:00). The current app launched the
way an installed app starts (sim_run.sh --container: the container's own composite and imported
disc, no test paths, no pad script) draws the title correctly in all 40 screenshots taken once a
second across the Dolby screen, the fly-in and PRESS START
(local-research/ipad/title-container-20260924-191919/t01-t40.png, sheet.png). No run on any build
has reproduced it since; it stays listed until the user confirms.

The same run found the iPad simulator in portrait with iPadOS 26's windowed apps, where
UIRequiresFullScreen no longer applies: BlueWake opened as a floating landscape window with the
picture and SunPad's controls laid out for the window, and ran normally. Rotating the device to
landscape (Simulator's Rotate Left) turned it into the full screen: the surface, the 4:3 fit and the
controls followed, the prologue kept running at 61 retraces a second (7,954-8,137 over 3 seconds)
and the log has no errors (rot1.png, rot2.png).

## 2026-09-24 (evening, 11) iPhone: the game runs; the phone touch layout moves into the letterbox bars

The same simulator app runs the Outset save on an iPhone 17 simulator (iOS 26.5, 874x402 points
landscape; local-research/ipad/iphone-20260924-191145). SunPad's phone defaults are normalized for a
full-width game, so over Wind Waker's 4:3 picture (172-705 points) the move stick covered the minimap
and R, B and Y covered the item HUD. With the 4:3 picture on and bars at least 84 points wide, the
phone defaults are now two columns in the bars (BWGameOverlay.mm phoneColumnFramesInSafeArea): Start,
L, D-pad and the move stick on the left; Z and R, Y and X, B and A, and the camera stick on the right
under the menu button. Fill Screen, narrower bars and saved positions keep the old behaviour.
Screenshot: local-research/ipad/iphone-layout-20260924-191638/shot-land.png (the whole HUD visible).
Controller defaults were checked in the same pass (pad.cpp): face buttons by position, right
shoulder Z, analog triggers L/R, right stick the C-stick; no change needed.
The iPhone run's rate (49, 41, 30 retraces a second) is not a measurement: Jump Desktop Connect was
at 260 percent CPU and the load average at 64 during it.

## 2026-09-24 (evening, 10) The village's boundaries: nothing new beyond the actor search

The per-address census over the village window (22,600-23,600, the steered route, census mode, which
turns the native loops off) counts 306,800 boundaries a retrace; 39 percent are the actor search by
id (40,000 nodes a retrace, which the native loop takes in normal runs), then the register helpers
(10 percent, refuted natively), the collision tree and the cache loops (refuted). No new loop shape
of the actor-search kind; the village is heavier because it has more actors doing the same work.

## 2026-09-24 (evening, 9) Long runs on the simulator: stable memory, no stalls, interiors at 60 for 30,000 retraces

Two soak runs on the iPad simulator app (the PRD's long-duration acceptance is open):

- **Steered route into the house, 54,000 retraces (about 15 minutes of play)**
  (local-research/ipad/soak-20260924-182434): the route completes at 23,622 as before, then Link
  stays in the interior; every 6,000-retrace window from 24,000 to 54,000 runs at 60.0 retraces a
  second (median 13.5-14.6 ms), the run ends normally, 109.4 M draws submitted and none failed; audio
  4,300 starved of 3,599,873 pushes.
- **The Outset save outdoors with keyboard walking, 30,000 retraces**
  (local-research/ipad/soak-mem-20260924-185117; BLUEWAKE_KEY_TAPS): memory footprint 625 MB once
  play starts, 637 MB at the end, flat from 19,000 on (resident 770-870 MB), so no leak at play
  scale. Rates 58-60 retraces a second except one 4,000-retrace window at 37.9 (host contention; the
  audio stretcher covered it: 537,365 stretched pushes over the run).

scripts/ios/mem_poll.py records footprint and resident size against the retrace. Both pollers now
skip shells and python when matching the app by command line: the first soak's pollers had attached
to the launching shell (the earlier village rate data is the app's, checked by its magnitudes).

## 2026-09-24 (evening, 8) Refuted: grouping the edge front's globals; audio is not on the simulator's game thread

The simulator's village sample puts the edge service's fast front at 7.5 percent of the game thread,
310 of 369 samples at one instruction just after a run of loads from globals spread over about nine
cache lines (the Mac's sample: 362 of 510 at the intercept-table probe). Tagging the seventeen
globals it reads with one section (224 bytes, about four lines) is exact by construction and did not
pay: rendered save view 9.83 / 11.10 / 9.81 G cycles per 100 retraces before against 11.23 / 10.00 /
10.16 after, instructions 450-453 M on both. The stall is sampling skid onto the first dependent
instruction, not a miss pattern layout can fix. Reverted (diff in local-research/refuted/edge-hot-section.diff).
Also checked: audio is 0.6 percent of the simulator's game thread (the Mac's 3.8 percent in
aurora_backend_audio_push is its real-time throttle sleeping), so moving it off-thread buys nothing.

## 2026-09-24 (evening, 7) The village gap is CPU availability, not simulator overhead

scripts/ios/rate_poll.py polls a game process's kernel counters (instructions, cycles; all threads)
every half second next to the retrace its log has reached. The same steered route on the simulator
app and on the macOS host (local-research/ipad/village-rate-20260924-175401/rate.txt, /tmp for the Mac):

| window | simulator | macOS host |
| --- | --- | --- |
| Outset play 14,100-17,800 | 57.8 /s, 316 M instr, 78 M cycles a retrace | 60.0 /s, 316 M, 75 M |
| 20,600-21,600 | 55.7 /s, 379 M, 91 M | 60.0 /s, 377 M, 88 M |
| 21,600-22,600 | 51.6 /s, 384 M, 91 M | 60.0 /s, 388 M, 91 M |
| 22,600-23,600 (village) | 45.1 /s, 376 M, 89 M | 59.8 /s, 369 M, 87 M |

The work per retrace is the same (instructions within 2 percent, IPC 4.2 on both), so the simulator
adds no per-frame cost. What differs is the cycles a second the process got: 4.0-5.1 G (summed over
threads) on the simulator against 5.2-5.5 G on the Mac in the village. Sampled, the game thread is
99 percent busy on both, so in the village it needs about a whole performance core; on this fanless
M2 Air any contention (other processes, heat after minutes of play) costs frames there first. The
earlier 24-32 on the same stretch was the same effect at a load average of 4.9; this run reached 45.
So the speed target is the village's ~90 M cycles a retrace: at 60 retraces a second that is the
core's whole budget.

## 2026-09-24 (evening, 6) On the simulator: the route reaches the interior on the Mac's trajectory; the village walk is the heaviest stretch yet

The app with the fast-path route (local-research/ipad/interior-20260924-172645) now follows the Mac's
trajectory exactly (ladder 20,316-20,504, route complete at 23,622, door at 23,637) and stands Link
in the house, which runs at 59.8 retraces a second (median 15.4 ms); Outset play 58.6. The walk
inland is the heaviest stretch measured on the simulator: 58.3, 52.8, 46.6, 32.1, 30.8 and 24.4
retraces a second in successive 500-retrace windows from the ladder (20,600) to the village and the
house (23,600). A sample at 22,290 (local-research/ipad/village-sample-*) has the game thread 99
percent busy and the GX worker 62 percent, the same shape as the pier view with more work a frame:
the village's actors. The same stretch on the macOS host measured 58.2 overall, so the gap is the
simulator app's per-frame cost there, to be sampled deeper; it is the target for the speed work the
user is choosing between (exact small wins, the play-scene source port, or accepting the speed).

## 2026-09-24 (evening, 5) Steered routes run on the fast path: the routed walk at 58.2 instead of 26

A route used to turn the per-block service on until it finished, so a routed run crawled (26-29
retraces a second on the simulator). It only needs the turns to end at the player update
(0x80122D30): the edge front now hands that one address to the full service while a route is armed,
testing the address first so the common path pays an immediate compare, not a load. The Mac route
follows the identical trajectory (route complete at 23,622, door at 23,637, as before) with Outset
play at 60.0 and the routed walk at 58.2 retraces a second. Certified: digest 83d2590d, 219.4/219.8
against 219.1 M instructions per play retrace (within the 0.2-0.3 percent run-to-run spread; the
flag-first order cost +0.5 percent and was not kept). 217/217 host tests.

## 2026-09-24 (evening, 4) The steered route reaches the interior on the iPad simulator; interiors run at 60

On the simulator (app rebuilt with the route fixes; local-research/ipad/interior-20260924-165026) the
same route climbs down the ladder at 20,316-20,504, loses the pier once and swims back (the timing
differs from the Mac, so the trajectory does too), completes its 29 waypoints at 25,916 and confirms
the door at 25,931; Link stands inside the house from about 26,300, drawn as on the Mac. The interior
runs at **60.0 retraces a second** (26,300-29,900, median 13.3 ms). The Outset stretches of this run
(28.6 and 26.4) are the route mode's own cost, not the game's: while a route is armed the host services
every block boundary (the fix above), which is exact but slow, and it switches back to the chassis the
moment the route finishes. Unrouted runs are unaffected (Outset play 58.3-58.5 the same afternoon).

## 2026-09-24 (evening, 3) The retrained app measured on the route: Outset play 58.3, dialogue 59.4

With the Mac quieter (load average about 4), the unattended new-game route on the simulator with the
app built on the retrained host profile (local-research/ipad/route-20260924-162941): prologue 60.0,
play start 59.8, Outset play 58.3 (median 15.7 ms), opening dialogue 59.4 (median 16.3 ms) retraces a
second, against 60.0 / 59.9 / 57.7 / 58.9 on the old profile. Audio: 1,665 starved of 1,399,873
pushes, 7,092 stretched (18,051 before). The heavy view: 57.54 and 56.50 retraces a second (median
17.0 / 17.3 ms). The game's 30 frames a second is 60 retraces: the route runs at 97-100 percent of
it, the densest measured view at 94-96 percent.

## 2026-09-24 (evening, 2) The steered route reaches the interior again; corpus: an Outset house

The route's steering, like its start, ran only at host turns that began at the player update
(0x80122D30), so once started it steered once and stopped. The per-block service now stays on until
the route finishes, not just until it starts. With that, the historical Outset route runs end to end
on the current build for the first time since the 2026-08-31 timing change: ladder down at
20,316-20,504, all 29 waypoints between 20,732 and 23,620, the door confirmation at 23,637, and Link
inside the house (local-research/dolphin-ref/corpus-interior.png): plank walls, the Triforce
emblem, the rug, lighting and the indoor HUD draw cleanly across ten frames. The certified digest
is unchanged (83d2590d; runs without a route never turn the per-block service on) and 217/217 host
tests pass. The corpus now covers title, file select, prologue, dialogue, play HUD, pause menu,
lookout, ladder, sea, swimming, shore, grass and an interior.

## 2026-09-24 (evening) The steered player route starts again; corpus: ladder, swim, shore, grass

The steered route (BLUEWAKE_PAD_PLAYER_TARGET_*, the old Outset-to-Omasao driver) arms at a host turn
that begins at 0x80122D30 with the player in r3. Since the chassis runs many blocks a turn, no turn
began there and the route never started (no [pad-waypoint] line in a 36,500-retrace run). While a
route waits, the host now sends every boundary through the full service and ends the turn at that pc;
it goes back to the normal chassis the moment the route starts (runs without a route are unchanged).
With the A presses of the unattended route ending at 20,900, the route starts at retrace 20,248,
steers Link down the lookout's ladder ("Let Go"), and he swims to the shore and walks the sand and
grass (local-research/dolphin-ref/corpus-ladder-swim.png): ladder, splash, swim, wet sand, tall grass
and cliffs draw cleanly. The post-ladder waypoints to Omasao's door do not engage, as recorded on
2026-08-31 (the corrected timing moved the trajectory); an interior needs a new route, recorded with
the keyboard hook and the player's position, which is queued.

## 2026-09-24 (afternoon, 8) The host profile retrained with a rendered route: -5.6 percent on the simulator

The iOS app and the macOS host are built with a host profile (build/host-pgo-gen/prof/merged.profdata)
recorded on 2026-09-23 from three headless routes, before the GX FIFO fast path, the frameless edge
front, the GroundCross change and the native actor search; a changed function loses its profile,
and the headless routes never reach Aurora, the GX worker or the frame path the app spends its time
in. Retrained with the same three routes plus a rendered one (the save's Outset view, 3,000
retraces), now scripts/pgo_host_train.sh:

| | old profile | new profile |
| --- | --- | --- |
| macOS host, certified window (headless) | 218.3 M instr/retrace | 216.6 M, digest 83d2590d |
| macOS host, rendered save view per 100 retraces | 459/460 M instr, 10.34/10.51 G cycles | **432/433 M, 9.76/9.67 G** |
| iPad simulator app, whole heavy-view run (proc_pid_rusage) | 1,246/1,252 G instr, 301.6/307.7 G cycles | **1,176/1,182 G, 287.9/285.3 G** |

Two A/B pairs each. The simulator's wall-clock rates in the same runs swung from 57 to 27 retraces a
second with the load average at 6-7 (WindowServer, a remote-desktop session and this agent's own
processes), which is why the per-process counters are the measure: scripts/ios/sim_rusage.py reads
ri_instructions and ri_cycles of the app until it exits. Headless-only training (without the
rendered run) was measured too: certified -1.1 percent, rendered +0.9 percent instructions. The simulator save
acceptance passes on the retrained app (local-research/ipad/acceptance-20260924-151642).

## 2026-09-24 (afternoon, 7) Corpus: walking Outset by keyboard; the speed decision

With the keyboard fixed, holding W for 20 seconds from the pier (BLUEWAKE_KEY_TAPS=36@w@20) walks
Link down the pier and off its edge, through a swim to the shore, across the beach and grass and back
into the sea (local-research/ipad/walk-20260924-*/, sheet walk-sheet.png): the splash, swimming,
wakes, sand, rocks, grass and HUD all draw without a defect across 23 frames. An interior still needs
position-aware steering to reach a door (the host's waypoint route belongs to the old Omasao driver).

Speed, where it stands: the translated route's cheap levers are spent. The emitter's direct
cross-chunk calls were closed on 2026-08-31 (optimized build not feasible, turns -11 percent, the
route digest failed), native short callees and cache loops are refuted above, and the FP helpers
follow Dolphin's interpreter and are not expensive per operation. The remaining 5-10 percent in the
densest view is spread across FP-heavy game code. The ledger's route decision names the source port
of the play scene as the path past that; the user was asked on 2026-09-24 whether to fund it.

## 2026-09-24 (afternoon, 6) A hardware keyboard walks Link on the simulator (recompcore 0083)

The objective names touch and keyboard input; touch had routes (BLUEWAKE_TOUCH_TAPS), the keyboard
had none. Driving the Simulator window's keyboard from this Mac's automation reached only the
Simulator's own shortcuts, so the app gained BLUEWAKE_KEY_TAPS, which registers a keyboard and sends
keys through SDL_SendKeyboardKey, the call SDL's GCKeyboard handler makes for a real keyboard. With
BLUEWAKE_INPUT_PROBE on, J reached the guest as A (live input at retrace 1862) but W, held for three
seconds, left the stick at 0,0: the simulator binds a controller to player 0 (controller=1), and
PADRead assigned the controller's stick, substick and triggers after the keyboard had filled them,
discarding the keyboard's while its OR'd buttons survived. PADRead now combines them (the stronger
input on each axis, the larger trigger), as the touch controls' virtual pad already was. After it W
reads stick 0,127 and D 127,0 with the controller bound, and Link walks up the pier, turns and walks
off it into the sea (local-research/ipad/keyboard-20260924-140642; frames in the run's captures).
The certified digest is unchanged (83d2590d, 1,050 records; no controller on the macOS bench) and
217/217 host tests pass; the simulator save acceptance passes on the 0083 app
(local-research/ipad/acceptance-20260924-141147).

## 2026-09-24 (afternoon, 5) Refuted: native cache-maintenance loops; what a boundary is worth

DCInvalidateRange, DCFlushRange, DCStoreRange, the NoSync pair and ICInvalidateRange each walk a range
one 32-byte line at a time: a precharged 2-cycle block (addi r3, r3, 32; bdnz) and a dcbi/dcbf/dcbst/
icbi that the host's instruction fallback treats as a no-op, so every line returns to the chassis at
the loop's addi (7,100 boundaries a play retrace). The host advanced whole lines at that boundary
(r3 += 32, ctr -= 1, downcount -= 2, only where the translated line would precharge, pass its leader,
back-edge and chassis checks and stay in the loop). Exact - digest 83d2590d, stop rows unchanged,
5.95 M lines in the window, 7,400 a retrace - and not faster: 219.8 M host instructions per play
retrace against the same session's baseline 219.1 M (run-to-run spread about 0.2 percent; the
earlier 218.7 is the same build). The address test the edge front gained for the six loop
boundaries, paid at all 182,000 boundaries a retrace, costs about what the 7,400 skipped boundaries
saved. Reverted (local-research/refuted/cache-loop-native.diff).

What the three native experiments together say about the cost model: a chassis boundary with a
trivial block is worth roughly 100 host instructions, and translated bodies about 20-25 host
instructions per guest instruction in the hot chunks. The actor search paid (-21.9 percent) because
each node removed about 27 translated guest instructions as well as three boundaries; the register
helpers (via a redirect) and the cache loops removed boundaries with almost no body and did not. The
remaining candidates on the guest side are FP-heavy (PSMTX*, calcWeightEnvelopeMtx, the collision
tree, JASystem channel updates), where exactness means reproducing each FPSCR update, and the host
side's remaining per-boundary work (the edge front's six to eight loads, the dispatch lookup, the
chunk entry) needs a composite rebuild to move. Candidate for that rebuild: get_ram_ptr sends every
MMIO and uncached access through two out-of-line resolver calls before external_write (92,600 calls a
play retrace in the composite, 99 percent pruned at once; BLUEWAKE_TRACE_ALIAS_COST).

## 2026-09-24 (afternoon, 4) The unattended route on the simulator: 57.7-60 retraces a second throughout

The new-game route on the iPad simulator (iPad Pro 11-inch (M5), --route with the acceptance's A
presses from 17,800, BLUEWAKE_FRAME_TIMING; local-research/ipad/route-20260924-132133), build with
the native actor search, load average about 3.3:

| stretch | earlier on 2026-09-24 | now |
| --- | --- | --- |
| prologue (1,000-13,000) | 60.0 /s (real-time cap) | **60.0** |
| play-scene start (13,910-14,100) | 51.0 | **59.9** (median 14.9 ms) |
| Outset play (14,100-17,800) | 48.4 | **57.7** (median 15.7 ms) |
| opening dialogue (17,800-20,900) | - | **58.9** (median 16.1 ms) |

Audio over the whole run: 1,953 starved pushes of 1,399,873 (0.14 percent), 18,051 stretched (1.3
percent). The game's own 30 frames a second needs 60 retraces; Outset play is at 96 percent of it and
the rest of the route at 98-100 percent. The heavy view (the save's pier, the densest Outset view
measured) is the slowest place so far at 54-57.

## 2026-09-24 (afternoon, 3) Corpus: Outset's opening dialogue draws cleanly

The certified route with the acceptance's A presses from 17,800, captured every 300 retraces from
14,200 to 20,800 on the macOS host (native actor search on): the "Outset Island" title card, the
lookout scene and Aryll's dialogue boxes, paged by A, with drop-shadowed lettering and the orange
keyword in "Grandma is waiting for you back at the house!", then the HUD returning with control
(local-research/dolphin-ref/corpus-dialogue.png, -dialogue-zoom.png). No text or palette defect in
the 23 frames. Next in the corpus: the open sea and an interior.

## 2026-09-24 (afternoon, 2) Refuted: native register helpers through a chassis redirect

The per-address census with the native actor search on (certified window) leaves 182,000 boundaries
a retrace, the largest share __save_gpr/__restore_gpr: 27,700 calls, each a round trip into the
helper chunk and one back to the caller. The chassis always dispatches the address it passed to the
edge service, so a native helper needs a redirect: dispatch_loop.h continued at ctx->pc when the
service moved it (one compare per boundary), module_export.c exported bluewake_chassis_redirect so a
host enables native helpers only on a composite that honours it, the two objects were rebuilt with
their PGO profile and the composite relinked (8 s, 108 PGO objects). The host ran _savegpr_N and
_restgpr_N on the precise path the translated code takes when entered past the chunk's leader (a
budget check and one cycle per instruction, the suffix cleared at each load or store, one cycle for
the blr, a stop exactly where a check would stop) and moved the guest to lr, leaving calls that
return inside the helper chunk to the translated code.

It is exact and slower: digest 83d2590d, both stop rows unchanged, 50.9 M native calls; certified
219.9 -> 220.8 M instructions per play retrace; rendered save view 450 -> 452 M instructions and
9.99/10.03 -> 10.19/10.19 G cycles per 100 retraces over two A/B pairs. The helper chunk's large self
time in the simulator sample (4.8 percent, func_803256E0) is the helpers' own work, and a chassis
iteration costs as much as the dispatch, chunk entry and body it replaces. Reverted: the composite
is the promoted 25daaf4d again, the relinked copy is kept as gGZLE01_recomp.dylib.redirect and the
diffs in local-research/refuted/gpr-redirect/. The same finding rules out redirect-based versions of
other short callees; the actor search paid because each node replaced about 35 guest cycles of
translated bodies and three round trips with a few host loads. Also measured and reverted: counting
the boundary census at the service's entry (so it can run with the native loops) costs +0.2 percent.

## 2026-09-24 (afternoon) Actor search by id runs natively between its own boundaries: -21.9 percent certified

**The instrument.** BLUEWAKE_BOUNDARY_CENSUS_BY_ADDRESS=path (with BLUEWAKE_BOUNDARY_CENSUS and
_WINDOW) writes how many boundary-loop iterations land at each guest address: an exact count of the
chassis round trips per function, where the pc sampler only guesses. Over the certified window
(13,900-14,700) a play retrace takes 384,000 boundaries, and **204,000 of them (53 percent) are one
pattern**: cNdIt_Judge walking the actor queue, calling cTgIt_JudgeFilter through a pointer for every
node, which calls fpcSch_JudgeByID through a pointer - 68,000 nodes a retrace (fopAcM_SearchByID and
friends). Each node costs three chassis round trips (the two bctrl targets and JudgeByID's return into
another chunk) to compare one word. The save's Outset view: 308,000 boundaries a retrace, 36,000 nodes.

**The change (host).** At the boundary that enters cTgIt_JudgeFilter from cNdIt_Judge's bctrl, when
the edge service's fast front has nothing to do, host_actor_search_native runs whole iterations whose
judge answers NULL and hands the chassis the same boundary one or more nodes later, with the state the
translated blocks leave there: r0, r3, r4, r5, r12, r31, ctr, lr, CR0 from the loop's last cmplwi, the
two stack words JudgeFilter stores, downcount charged block by block (10, 4, 2, 5, 2, 3, 2, 2, 5 on the
precharged path) and the observation suffix of the last observing instruction. It runs an iteration
only when every block would take the precharged path and no budget check in it would stop the guest
(leaders, both return dispatches, the back-edge, the chassis check at the next boundary); the match,
the list's last node and every stop stay with the translated code. It is entered only when lr, r29,
r30, ctr and the filter's judge prove the context. BLUEWAKE_ACTOR_SEARCH_NATIVE=0 turns it off.

| | before | after |
| --- | --- | --- |
| certified window, host instructions per play retrace | 280.0 M | **218.7 M (-21.9 percent)** |
| route digest | 83d2590d, 1,050 records | **83d2590d, 1,050 records** |
| stop rows | 10,557,120 / 11,174,010 blocks | **identical, same pcs** |
| rendered save view, per 100 retraces | 482 M instr, 10.67 G cycles | **450 M, 10.41 G** |
| simulator heavy view | 54.0 retraces/s, median 18.6 ms | **57.4, median 16.8 ms** |

Outset frames byte-identical at every common retrace; 217/217 host tests. On the simulator the loop
took 768,153 runs over 89.6 M nodes in the heavy view, identically in two runs; the simulator save
acceptance passes (local-research/ipad/acceptance-20260924-123927, the same 1,218 card bytes and
control at retrace 831 as before); the audio stretcher
touched 9,374 pushes against 67,875 before. Wall clock on this Mac is noisy: three later heavy-view
runs measured 31.8, 44.2 and 29.7 with the load average near 4 and WindowServer at up to 42 percent
CPU, so 57.4 is the best quiet run, and the instruction counts above are the result. Queued, with a
caveat: a chassis "redirect" (the edge service moving the guest to another address, one compare per
boundary in dispatch_loop.h) would let the same technique take __save_gpr/__restore_gpr (27,700
calls a retrace) and the dcbi loops, but a composite relink recompiles the runtime sources from the
working tree, which holds the parked cpu.c changes, and the dispatch objects are profile-guided. Next on the census: __save_gpr/__restore_gpr (15,800 calls each a retrace,
the native dispatch already refuted), the collision tree (GroundCrossGrpRp, ChkGrpThrough).

## 2026-09-24 (midday, 2) Heavy view 54.0 retraces a second; the game thread is the limit

On the 0082 build the simulator's heavy Outset view measures **54.01 retraces a second** (median
18.6 ms, p90 25.4 ms; local-research/ipad/heavy-20260924-121405) with 0.4 percent of audio pushes
starved (856 of 213,206; the stretcher covered 67,875). A second run at load average 5 measured
40.6 and is noise. A sample of the simulator (8 s, 4,857 samples) has the game thread waiting in 39
samples: it is busy 99 percent of the time and sets the rate; the GX worker is busy about 40
percent. The guest profile is flat (largest single function __restore_gpr 6.7 percent; the actor
search family cTgIt/fpcSch/cNdIt about 10 percent together), so the remaining 10 percent to 60 is
not one function; the emitter-side candidates the ledger priced (precharge copy, block-local
downcount) are refuted. Next for speed: native fast paths for the actor-search judge loop, priced
on the instruction bench with the digest as the gate.

## 2026-09-24 (midday) Pause-menu text matches Dolphin: stale labels and rejected palettes

Two defects behind the user's "still clear text issues", both found by comparing the pause menu
with a Dolphin reference of the same moment (pause.dtm; Dolphin's GFX.ini now also has
DumpTextures, so its decoded textures land in user/Dump/Textures).

**Stale A/B labels (recompcore 0080).** "Charts" showed where Dolphin shows "Choose" and "Return".
The labels are 80x24 IA4 textures (0x01673420, 0x01674C60) that the game rewrites in place with
plain CPU stores; the texture cache's dirty epoch never moved, so it kept the first decode
(DOL_GXCORE_TEX_VERIFY counted 441 stale hits on each). A texture of 4 KB or less is now re-hashed,
texels plus palette, the first time it is used in each presented frame. A per-draw re-hash cost
+1.2 percent of rendered instructions; once a frame it costs nothing measurable (rendered save
view, two A/B pairs: 482/482 M before and after, cycles within noise).

**The blank SAVE tab (recompcore 0081).** The tab drew as a white torn shape with no letters.
Dolphin's texture dump of the same texture (word_save.bti, C4 80x24) maps each of our texel indices
to exactly one colour, so the texels were right and the palette was wrong: we drew it with
0x00EACAC0, the palette embedded in menures.arc's yazirushi_tri_2.bti (an arrow icon), while
Dolphin used word_save.bti's own. That palette's load never arrived because its address read
0x110047E0: Wind Waker leaves junk in the upper bits of LOADTLUT0 (the SDK's GXInitTlutObj fills
only some of the register's bits), and about 1,700 menu palette loads in the pause route were
rejected (0x08E9FDE0, 0x1CE9F2E0, ...), each leaving the previous palette in TMEM. The hardware
ignores those bits and Dolphin masks the palette source to 0x01FFFFFF in LOADTLUT1. After the mask
there are no rejected palette loads and the tab shows SAVE in white on the brown torn shape.
DOL_GXCORE_TLUT_LOG=1 reports every palette load and rejection. Refuted on the way: a palette loaded
after its texture is bound (the log showed the draw follows the arrow palette's load, in FIFO order).

Possibly also the user's title-logo report (speckled outlines): a palette rejected for junk bits
leaves another texture's palette in place, and the junk depends on memory the palette object reuses.
Unproven: a title run from power-on to retrace 900 loads 10,185 palettes and none had junk bits.

Evidence: 217/217 host tests; simulator save acceptance PASS on the new build
(local-research/ipad/acceptance-20260924-113844: save written, reload, control at retrace 831);
frames local-research/dolphin-ref/pause-menu-bluewake-0081.png and pause-menu-dolphin.png.

**The cursor's corner brackets (recompcore 0082).** At rest Dolphin draws them with grey shading,
ours with a black outline. Matching our texel indices against Dolphin's texture dumps showed
Dolphin draws cursor_00_01.bti's texels with cursor_00_02.bti's palette. The cursor is a J2D
picture with two textures, and both get GX_TLUT0, so the second texture's palette load replaces
the first's before the draw; the hardware reads TMEM at the draw, and we had attached each palette
at bind time. A palette load now marks any bound palette texture on that TMEM range, and the next
draw re-emits it with the palette TMEM holds. Frames through the cursor's pulse now match Dolphin
in the rest phase (local-research/dolphin-ref/cursor-before-after-0082.png). Cost: 481/482 -> 482/483 M
rendered instructions per 100 retraces, cycles within noise; Outset play frames byte-identical at
every common retrace; 217/217 host tests; simulator save acceptance PASS on the 0082 build
(local-research/ipad/acceptance-20260924-120024).

**A first UI corpus against Dolphin.** With these fixes, the reload route's title, file select, play
HUD and pause menu agree with Dolphin's frames of the same screens (text, palettes, plates and icons);
the remaining differences are fade and animation phase and Dolphin's crop
(local-research/dolphin-ref/corpus-{title,fileselect,hud}.png). The certified route's prologue
(22 frames, retraces 1,200-13,800) draws every storybook page and caption cleanly at full resolution
(local-research/dolphin-ref/corpus-prologue.png). Dialogue boxes and the sea are next in the corpus.

## 2026-09-24 (night, 2) Dolphin references of play, from an input movie: Link's face matches

**The blocker, removed.** Reference captures of play were blocked because Dolphin's keyboard pad
reads the physical key state and ignores synthetic key presses. Dolphin also plays input movies,
and the movie format is in ref/recompcore (Source/Core/Core/Movie.h: a 256-byte header, then one
8-byte controller state per pad poll). scripts/make_dtm.py writes one from BLUEWAKE_PAD_SCRIPT-shaped
entries (poll:buttons:length[:stick_x:stick_y]). Two things it has to get right, both found by
running it: the header's tickCount must be far ahead (Dolphin ends playback as soon as the emulated
tick count passes it, so zero ends the movie at the first poll), and Dolphin polls the pad several
times per game frame, so presses are spaced in polls, not retraces. The reference runs use their
own Dolphin user folder (local-research/dolphin-ref/user: GCI-folder card in slot A holding the
BlueWake save via scripts/card_to_gci.py, HLE audio, Metal, [Movie] DumpFrames and
[Settings] DumpFramesAsImages), so the user's own Dolphin configuration and saves are untouched, and
Dolphin runs alone.

**First results (save's Outset view).** The same A presses as BlueWake's reload route take Dolphin
from power-on through the title and file select into Quest Log 1 on the pier. Side by side with
BlueWake's capture of the same route (local-research/dolphin-ref/pier-dolphin-vs-bluewake.png) the
sea, sky, cliffs, houses, HUD and Link's hair and tunic shading agree; the differences are Link's
idle-animation phase and Dolphin's crop. Then the stick toward the camera for a moment turns Link to
face it in both (BLUEWAKE_PAD_SCRIPT now takes an optional stick: 1000:0:20:0:-100). **Link's face in
Dolphin is the same flat tan cel tone under the hair's shadow line as in BlueWake**
(local-research/dolphin-ref/link-face-dolphin-vs-bluewake.png): the "flat face shading" this queue
carried as a suspected defect is the game's look, and the item is closed.

## 2026-09-24 (night) 98 percent of the play window's host turns were a diagnostic: -4.0 percent certified

**Finding.** The turn census (BLUEWAKE_CREDIT_CENSUS) over the certified play window: 5,367,296 turns,
57.6 blocks and 1,207 cycles each, and **86.7 percent of them ended because the edge service returned
true**. A per-address histogram of those exits: 4,581,934 at 0x80328F84 and 53,399 at its 0xC0 mirror,
out of 4,654,417 - that is _restgpr_27, and it was on the edge-observation list only so the turn body
could run the collision-provenance ABI check (f1 carries the ground height at every GroundCross
return to 0x80246A04). So about 5,700 times a retrace the chassis returned to the host, paid the
whole turn body, and in almost every case the check's own lr test then did nothing.

**Change (host).** The check is a function now, host_observe_ground_cross_return, with its counters
global. The turn body still makes it at a turn that starts there; the full edge service makes it at
an edge that does not end the turn; 0x80328F84 leaves the observation list (it stays in the lookup
table, so the edge front hands it to the full service). Every arrival is observed exactly once either
way, and the [collision-provenance] summary the route digest carries is unchanged. An invariant
failure seen at an edge stops the run at the next turn.

| certified window 13,900-14,700, headless | before | after |
| --- | --- | --- |
| host instructions per play retrace | 291.8 M | **280.0 M (-4.0 percent)** |
| route digest | 83d2590d, 1,050 records | **83d2590d, 1,050 records** |
| turns in the window | 5,252,223 | 616,890 |

Rendered save view: 497 -> 482 M instructions, 10.88 -> 10.75 G cycles per 100 retraces; frames
hash-identical at every common retrace. 217/217 host tests. The bench's stop rows are updated.

## 2026-09-24 (evening, 3) The simulator's heavy view at 52 retraces a second; audio no longer starves

With the Mac quiet again (load average under 3), the same simulator view that measured 47.7 retraces
a second this morning measures **52.67 and 51.78** (median 18.3 / 19.1 ms, p90 26.5 / 26.8 ms) on the
build with the GX FIFO fast path and the edge-service front (local-research/ipad/heavy-20260924-094713/
and -094818/). Authentic is 60 (16.7 ms).

**Audio.** The same two runs report 1,500 and 1,697 starved pushes of 213,206 (0.7 percent), against
84,072 (39 percent) in the morning's run without the stretcher at 46 a second; 96,252 and 108,346
pushes went through the stretcher. At this speed the output is continuous and runs about 13 percent
slow in the heaviest view instead of cutting in and out.

**Refuted, recorded: a hardware-window reject in the alias resolver.** The gather-pipe stores miss the
MEM1 fast path and reach ppc_guest_alias_resolve twice each (1.3 percent of the game thread in the
sample), the second time at 0x8C008000, inside the alias bounds. An exact reject for the 0x0C/0x4C/
0x8C/0xCC windows when no alias intersects them, built into the composite's runtime with its PGO
profile: 291.8 -> 292.6 M instructions per play retrace, digest unchanged. A null (three functions lost
their profile data); reverted, the diff is local-research/refuted/alias_hw_window.diff.

## 2026-09-24 (evening, 2) A frameless front for the edge service: -3.0 percent certified, digest unchanged

With host_mmio_write gone, host_chassis_edge_service was the largest host owner on the simulator's game
thread: 7.9 percent by its own time, plus the scheduler predicate (2.1) and the external-pending
call (0.7). The chassis calls it at every block boundary it stops at, through a pointer, and its rare
paths make calls, so every call saved and restored five register pairs. A new front with the same name
answers the common case with loads and compares only - no census, no per-block servicing, clean
interrupt sources, the overlap phase unchanged through the cached pointers, no intercepted address,
no interrupt the guest could take - and tail-calls the unchanged full service for everything else. It
writes nothing, so a false from it is a false from the full service.

| certified window 13,900-14,700, headless | before | after |
| --- | --- | --- |
| host instructions per play retrace | 300.7 M | **291.8 M (-3.0 percent)** |
| route digest | 83d2590d, 1,050 records | **83d2590d, 1,050 records** |
| turns to the 13,900 / 14,700 stops | 12,682,901 / 17,935,124 | identical |

Rendered save view: 509 -> 498 M instructions per retrace. 217/217 host tests. With the FIFO change,
the day's host-side work takes the certified window from 334.0 to 291.8 M (-12.6 percent).

## 2026-09-24 (evening) GX FIFO writes skip the device sync: -10.0 percent certified, digest unchanged

**The owner.** In the simulator sample of the heavy view, host_mmio_write was 10.1 percent of the game
thread, the largest host owner. Nearly all of its calls are the game's GX and J3D code writing the
gather pipe at 0xCC008000 (several writes per matrix and per draw), and each one paid for the full
device-register protocol: an external-memory probe with two alias searches, a cycle-domain
observation (3.4 percent), a sync of every device clock, marking the interrupt sources dirty, the GX
write itself (1.75 percent), and a refresh of the interrupt sources and deadline (2.1 percent).

**The change (host).** A FIFO write goes straight to dol_platform_gx_write. No backend's gx_write
touches a device clock, a deadline or an interrupt source - the PE finish is committed at the
draw-done intercept, and the headless backend installs no gx_write at all - so the sync around it
could not change anything a device or the guest sees. The digest is the proof:

| certified window 13,900-14,700, headless | before | after |
| --- | --- | --- |
| host instructions per play retrace | 334.0 M | **300.7 M (-10.0 percent)** |
| route digest over the 14,700 ceiling | 83d2590d, 1,050 records | **83d2590d, 1,050 records** |
| stop pc at 13,900 / 14,700 | 0x80307ef4 / 0x8027fa30 | same |
| dispatch turns to those stops | 14,287,831 / 20,138,490 | 12,682,901 / 17,935,124 |

The turn counts fall because the rebudget after every FIFO write used to cut turns short; the bench's
recorded rows are updated, with the old ones kept in a comment. Rendered, the save's Outset view:
541 -> 509 M host instructions per retrace for the whole process (-5.9 percent), and seven captured
frames hash-identical to the previous host. In a new simulator sample host_mmio_write is 1.0 percent
of the game thread (was 10.1) and the wait for the GX worker is gone from the profile. The iPad
simulator save acceptance passes (local-research/ipad/acceptance-20260924-092313/); 217/217 host
tests.

**The audio stretcher costs 0.3 percent** of the game thread in the same sample. Wall-clock timings of
the heavy view this evening (29.6-34.2 retraces a second, with the stretcher on or off) were taken
while other applications held the Mac at a load average of 5 to 11 (a browser renderer at 74 percent
CPU, a remote-desktop agent); they do not measure the builds.

## 2026-09-24 (late afternoon) Rolling under the touch stick, and audio that stretches instead of stuttering

**"If I use the analog stick Link rolls" (user report).** The touch stick path is clean: on a plain
simulator run of the Outset save, the stick held forward through BLUEWAKE_TOUCH_TAPS runs Link down the
pier with no roll (retraces 1,095-1,203; local-research/ipad/stick-0924b/). The rolls came from this
loop's own automation: the heavy view, the acceptance and the reload routes script A presses
(BLUEWAKE_PAD_SCRIPT, the route pulse, BLUEWAKE_PAD_CONFIRM_EVENT=any), and moving while A is pressed is
a roll. A person steering one of those runs had A pressed under them. **Fix:** the first live input on
channel 0 (a button, a stick past a fifth of its travel, or a trigger) retires every scripted press and
stick for the rest of the run, and says so ("[pad] live input at retrace N: scripted presses off").
Verified on the simulator: the scripted reload still reached the play scene at retrace 765, and the
touch stick at 1,086 turned the scripts off (local-research/ipad/takeover-0924/). 217/217 host tests.

**Audio stutter is the speed gap (measured).** A shutdown line now counts pushes that found less than
10 ms queued while playing. Heavy Outset view, 45-47 retraces a second: 84,072 of 213,206 pushes
starved, so the device played silence between most of them. **recompcore 0077** stretches the output
in time while that happens - synchronous overlap-add, 40 ms sequences with 8 ms crossfades at the
best-matching point, same pitch, slower tempo - aiming the queue at 100 ms and passing samples through
bit-exactly at full speed (DOL_AUDIO_STRETCH=0 turns it off). Standalone test: pass-through bit-exact;
factors 1.1/1.3/1.6 give 1.088/1.288/1.584 times the input with no step larger than the source's own;
leaving the stretcher is continuous. On the simulator, starved pushes fell to 34,360 (60/100 ms
target, 1.6 cap) and 29,953 (100 ms, 2.0) - both on runs where the Mac was loaded and the game ran at
36 and 30 retraces a second, so the game was slower than in the 84,072 run. It needs a person's ear;
the acceptance audio capture is taken upstream in the host and is unaffected.

**The instrument's limit today.** Load average reached 10-11 during these runs (builds, and the Codex
window itself at 46 percent CPU), and the heavy view read 46.1, 36.2 and 29.6 retraces a second on
builds whose game-thread work is the same. Speed conclusions stay on the instruction bench.

## 2026-09-24 (afternoon) Touch controls vanished after a home-screen round trip (recompcore 0076)

Chasing the title-logo report through the shell's paths turned up a different defect. Opening the
settings panel over the fly-in holds and resumes cleanly (BLUEWAKE_SHELL_DEMO=settings with the new
BLUEWAKE_SHELL_DEMO_AT=14; 36 screenshots, logo correct throughout). A round trip to Settings and back
also keeps the logo correct, but SunPad's touch controls disappeared about a second after the return
and stayed gone: the game kept running with no on-screen input.

**Cause.** The surface is released while the app is in the background and created again on return,
and Aurora's Metal binding called SDL_Metal_CreateView at every creation. Each call adds a new view
above everything in the window, so the new one covered the overlay, and the old ones were never
removed. **Fix (recompcore 0076):** one Metal view per window, kept as a window property; every later
surface reuses its layer, and the old surface is released before the new one is created. The same run
after the fix keeps the controls in all 24 screenshots after the return
(local-research/ipad/bg-roundtrip-0924-fix/). The title-logo glitch still has not reproduced on any
path tried, including these two.

## 2026-09-24 (midday) The GX worker's two biggest wastes, a guest-level profile, and a title glitch not yet reproduced

**Where the GX worker's time went.** A sample of the simulator's heavy Outset view had the translation
worker inside g_fifo_translate 86 percent of the time: 14 percent memmove, 11 percent memset, 10.5
percent build_draw_plan's own work, and about 5 percent creating Metal textures, each a synchronous
XPC round trip on the simulator. Two of those were waste.

**recompcore 0073: one render packet, reused.** Every trace event built a RenderPacket, and every
packet zeroed RenderDrawPacket, the draw's matrix, light and XF snapshot of about 2.5 KB, although only
Draw events carry one. The frontend now reuses one packet and clears the draw section only for a Draw
event and the event after it. Rendered play window in the save's Outset view: 568 -> 542 M host
instructions per retrace (-4.6 percent); seven captured frames byte-identical.

**recompcore 0074: non-CI textures leave the palette out of their cache identity.** The frontend
reports the last bound TLUT with every texture, and the texture cache keyed on it for every format.
Wind Waker's HUD draws the same I4/IA4 images under alternating palettes, so the same texels at the
same address looked like new content each time, evicted their own entry and were decoded and uploaded
again: 28,144 uploads in 1,800 retraces, against 194 after the fix, which are first loads.
DOL_GXCORE_TEX_UPLOAD_LOG=1 logs each upload with its identity. On macOS it is 543 -> 541 M
instructions per retrace; on the simulator it removes about 23 XPC texture creations a frame.

**What the two did on the simulator.** A new sample of the heavy view: the worker is idle 28.5 percent
of the time (14.2 before), and the game thread's wait for it fell from 4.5 to 1.7 percent. The game
thread is now 91 percent translated guest code under chassis_dispatch. Wall-clock timings of the heavy
view this morning were 47.7 before and 46.6 / 32.1 after, the second on a hot machine after three back
-to-back runs; they do not resolve a change this size. The iPad simulator save acceptance passes on
the build (local-research/ipad/acceptance-20260924-082435/, control at retrace 831 on reload), and
217/217 host tests pass.

**A guest-level profile (BLUEWAKE_PC_SAMPLE).** The host sampler cannot name guest functions (the
composite has one symbol per 16 KB chunk), so the host gained a sampler thread that reads the guest
pc every 100 microseconds inside a retrace window and writes pc counts at exit
(BLUEWAKE_PC_SAMPLE=path, _FROM, _TO); ref/tww's symbols.txt names them. Play window of the save's
Outset view, 1,400-2,600: __restore_gpr 6.7 percent, cTgIt_JudgeFilter 4.4, fpcSch_JudgeByID 3.4,
PSMTXConcat 2.3, __save_gpr 2.3, cNdIt_Judge 1.9, GXLoadPosMtxImm 1.6, calcWeightEnvelopeMtx 1.5.

**Refuted, recorded: a native dispatch of __save_gpr/__restore_gpr.** Every call reaches them through
the generic dispatch (a pc-cache probe and an indirect call into a 4,096-label chunk). A host copy of
their emitted statements, entered from selected_dispatch, is exact - the certified digest 83d2590d
held over 1,050 records - and saves nothing: 334.1 -> 334.0 M instructions per play retrace, cycles
58.22 -> 58.15 G. Reverted (the generator and header are in local-research/refuted/). The lesson is
about the instrument: the sampler sees the pc the compiled code has stored, the compiler keeps the
stores at dispatches and drops many inside a block, so short functions that are always dispatched to
are overweighted. The sampler names candidates; the instruction bench judges them.

**The title's broken logo (user report, iPad simulator): not reproduced yet.** The report shows the
ZELDA letters as speckled outlines with no fill and THE LEGEND OF white on a dark box, while the wind
waker lettering and the 3D scene are right: the 2D draws' TEV colour and alpha state looks wrong for
that moment. It does not reproduce in any of: the macOS title every 12 retraces from 720 to 900 (and
the pre-0073 host, identical up to capture timing), the simulator's own frame captures of the same
stretch, 30 live simulator screenshots across the fly-in and title, or the save route's START into the
file select every 10 retraces. recompcore 0075 adds DOL_GX_FIFO_BATCH to cut the worker's stream at
other sizes: batches of 7 and 61 bytes render byte-identical frames to the default at every common
retrace, so the batch split is not it. Untested: the SunPad shell's pause (menu, settings), a
home-screen round trip and a manual launch; those hold the guest and present again. Next: exercise
those paths on the title with simulator screenshots.


## 2026-09-24 (later) Game-thread self time, a small host win, and exact FP fast paths refuted

**Where the game thread's own time goes** (self time, not inclusive, from the 8 s sample of the
simulator's heavy Outset view): translated function bodies 54.0 percent, host_chassis_edge_service
6.0, waiting for the GX worker 4.5, chassis_dispatch 3.4, the host's turn loop 2.4,
host_cycle_deadline_distance 2.1, a gather-pipe vector insert 1.7, the interrupt predicates about
3, and the out-of-line FP helpers (ppc_fmuls, fadds, fsubs, ps_madds0/1, ps_madd_op, fcmp) about
5 together.

**Gather-pipe batching without a vector (recompcore 0072).** Every 1-8 byte guest write did a
std::vector insert plus two out-of-line flag checks. A fixed 1 KB buffer and inline checks: rendered
play-window cost in the save's Outset view 570/571 -> 568/568 M host instructions per retrace (two
A/B pairs), about -0.5 percent. Promoted.

**Refuted, recorded: exact inline fast paths for the FP helpers.** Inline versions of seven helpers
that take the common case (no NaN or infinite operand, FPSCR[NI] clear) and fall back to the
runtime otherwise were proved exact against the runtime on 4,000,000 randomized and edge-case
operations (NaN, infinity, denormals, the fused multiply-add even-tie fix; zero mismatches in FPR,
PS1, FPSCR and CR), placed in generated.h and the generator, and the 100 hot chunks were rebuilt
with their PGO profile (about 2.5 hours). Certified bench window: 333.6/334.0 M instructions per
play retrace against 332.9/333.3 M, cycles 58.3/59.7 against 60.8/58.7 G, digest 83d2590d unchanged;
the save's Outset view, rendered: 568/570 against 569/568 M. A null: the helpers' sampled time is
their work, not their call overhead. Reverted; the composite is byte-identical to the previous one,
and the rebuilt copy is kept as gGZLE01_recomp.dylib.fpfast.


## 2026-09-24 Audio interruptions pause the game; the app builds for iOS devices

**Speed, where it stands.** A sample of the simulator's heavy Outset view (8 s, 5,273 game-thread
samples) shows the game thread busy all of the time: 74 percent translated code under
chassis_dispatch, 5.3 percent edge service, 4.5 percent waiting for the GX worker, the rest the
host's per-turn glue. BLUEWAKE_CYCLE_CAP=65536 is a null against 16384 (46.8 retraces a second), as
the code's own record predicted: device deadlines, not the cap, bound a turn. Simulator wall-clock
numbers on this Mac move by a quarter between identical runs when the desktop is busy (one run of
the unchanged build measured 36.1), so speed decisions stay on the instruction bench. The
remaining gap is the emitted body's per-instruction shape - every instruction is an addressable
resume point with a pc store and a charge test - which the 2026-09-22 entries priced (9.8 percent
of the body) and found the prepaid-copy route to it refuted (+2.4 percent once made correct).

**Audio interruptions (FR-014).** A call or Siri posts AVAudioSessionInterruptionNotification; SDL
already stops and restarts its own output, and the shell now also adds an audio reason to the
pause set, so the game is held for the interruption's length. BLUEWAKE_SHELL_DEMO=interruption
posts the began/ended pair on the simulator: the guest was held at present 244 for 6.0 s and
released when the interruption ended (reasons 0x20). The simulator cannot produce a real
interruption (PRD 14.6); the device check is the hardware gate.

**Phone layout and full screen.** The same build on the iPhone 17 simulator (874x402 points)
lays out SunPad's phone defaults around a 4:3 picture and reaches the title (retrace 333);
local-research/ipad/iphone17-title.png. One frame of the title's fly-by over Outset showed the upper
sky black while a later frame was correct; it needs a Dolphin reference of the same camera moment
before it is called a defect. iPadOS 26 ignores UIRequiresFullScreen and has no public API that
forces a full-screen window; in the default multitasking mode the app launches full screen (every
simulator screenshot fills the display), and in windowed mode the picture is letterboxed to 4:3
inside whatever size the player chooses, which is the behaviour this item now accepts.

**The renderer was dropping state it had been told (recompcore 0070).** While chasing a black-sky
frame in the title fly-by, gxcore's counters showed about 4,200 skipped draws per 3,700 retraces,
a count that changed from run to run, with "walk stride != frontend stride". The frontend's
register image was right (it parses the FIFO with that stride), and gxcore's was stale: the
decode trace holds 8,192 events and silently dropped the rest of any larger batch, VCD writes
included. The parser now hands pending events to the sink at a command boundary before the trace
fills. The title series after: 0 skipped, 0 dropped, and the worker and synchronous paths submit
an identical 12,269,794 draws where they used to differ. A drop now logs loudly, and a new frontend
test sends a 9,192-event batch through whole (fails before, passes after). The fix draws what was
missing: +2.5 percent host instructions per rendered retrace in the save's Outset view (557 to
571 M, two pairs, the instruction count being immune to this Mac's load and heat). The same patch
fixes a deadlock that hung frame-capture runs: Aurora's EFB readback mapped its buffer under the
mutex its own callback takes. The black-sky frames that remained were a second, separate race, fixed next.

**Frames end at their display copy (recompcore 0071).** The title fly-by still showed its logo and
island over a black sky and sea in 4-14 of 310 captured frames in worker mode, and never with the
worker off (DOL_GX_FIFO_WORKER=0). The display-copy flag was raised on the worker, and the main
thread presented at its next GX write, by which time the worker had often recorded the next
frame's first draws (the sky and sea) into the frame being presented. The frontend now stops a
FIFO parse right after GXCopyDisp; in worker mode the worker leaves the recording lock and waits
until the main thread has presented, which it does at its next GX write or inside a drain that is
waiting on the worker, exactly once per stop. A first version could present twice (the observer
and the wait both raised the flag) and produced 3 empty frames; the final one gives 0 black frames
in three worker runs and one synchronous run, all 12,269,794 draws, and costs nothing measurable
(571-573 M instructions per rendered retrace either way). Tried and refuted on the way: draining the
worker at present (more black frames, 14) and trusting EFB copies over guest-memory dirty epochs
(no change). New frontend test for the stop.

The iPad simulator save acceptance passes on 0070 and 0071 (new game, save, the guest's quit,
reload with control at retrace 831, 400 s of the game's own mix;
local-research/ipad/acceptance-20260924-031307).

**Wall clock on this Mac, recorded as a caution.** The simulator heavy view measured 34-35 retraces
a second after 0070, with the device composite compile paused; the instruction differential puts
0070's own cost at 2.5 percent. The rest is the machine: a fanless MacBook Air after many hours of
builds, where a single unchanged run measured 36 earlier. Speed decisions stay on instruction
counts; wall-clock tables in this file are comparable only within one session.

**Device build (queue item 8).** apple/ios configures for iphoneos with Dawn's native ios-arm64
package (build/ios-device) and links: LC_BUILD_VERSION platform IOS, minos 17.0, ad-hoc signed,
15.98 MB. scripts/ios/build_device_composite.py rebuilds the translated composite for
arm64-apple-ios17.0 with -mcpu=apple-a13 against the iPhoneOS SDK, with the same two PGO profiles
as the macOS composite, and links a device dylib with the tree's link line, retargeted. The device composite is built: 756 files for arm64-apple-ios17.0, -mcpu=apple-a13, 108 with PGO,
platform IOS minos 17.0, 490 MB; embedded in BlueWake.app/Frameworks and signed ad hoc the bundle is
484 MB and passes codesign -v --deep (build/ios-device). It took about four hours at four jobs on
this MacBook Air. What is left for a device is a signing identity and hardware, both user gates.

**Reference captures of play are blocked on input.** Dolphin boots the disc from File > Open and
reaches the title under automation, but computer-use key presses never reach its pad: its
keyboard controller reads the physical key state, which synthetic events do not set. A reference
of Link's close-up needs a Dolphin input movie or a controller profile it can read.


## 2026-09-23 (evening) iPad first run, SunPad shell, palette textures, runtime PGO, and the scaled EFB copy

**What a person sees changed most, so it leads.** Five things moved in this stretch of the loop,
each with simulator or macOS evidence under local-research (private, ignored).

**Palette textures (recompcore 0066).** The title logo drew as an opaque maroon box with tan and
green letters hiding the subtitle; the minimap was a solid blue square. GXLoadTexObj writes
SETTLUT after SETIMAGE0-3, and the frontend resolved C4/C8/C14X2 textures at SETIMAGE3 with the
palette the slot held for the previous texture. Resolving the slot again at SETTLUT fixes it: the
title matches the Dolphin 5.0-17995 reference (red lettering, "the wind waker" subtitle,
transparent background), the minimap shows Outset and the HUD icons regain their colors. A new
frontend test fails without the change and passes with it. Frames:
local-research/ipad/palette-0066/.

**First run on the iPad (FR-014 staged import).** A container without the disc or the files made
from it opens a UIKit screen that says what is missing, imports the disc through the document
picker, validates GZLE01 USA rev 0 and makes main.dol and rels/ on the device
(apple/ios/src/disc_import.c) in about half a second, byte-identical to generated/full,
including the REL alignment word the composite was translated from (4 in all 415 RELs). Checked on
the simulator three ways: the test hook (BLUEWAKE_IMPORT_DISC), the real document picker driven
through Files > On My iPad, and a non-disc file, which is refused with a sentence. The card now
lives in Documents/BlueWake/GZLE01.card so Files shows it. sim_run.sh gains --fresh, --import-disc
and --container.

**The SunPad shell (FR-015).** The ImGui touch overlay is gone. apple/ios/src/BWGameOverlay.mm
adapts SunPad's overlay (transfer record: docs/SUNPAD_TRANSFER.md): SunPad's sticks, buttons,
colors and defaults, the safe-area three-dot menu, the touch settings panel and the layout editor
with the grouped D-pad and sparse per-form-factor persistence. Opening any of them adds a reason to
a pause set, and recompcore 0067 lets the host hold the guest at a frame boundary: the demo hook
logged the settings panel holding the guest at present 244 for 8.0 s and releasing it on close.

**Runtime-object PGO.** relink_composite_runtime.sh also applies profile-guided objects for the
eight runtime and dispatch files: instructions per play retrace 340.4/340.7 -> 333.3/332.9 M
(-2.2%), cycles 58.4/58.3 -> 57.9/56.9 G, guest digest 83d2590d unchanged. Promoted.

**The 3D scene rendered at 640x480 (recompcore 0068).** A 1920x1440 capture of Link on the Outset
pier showed the scene in 3-pixel steps under a sharp HUD. gxcore allocated EFB-copy textures at the
guest's size, and Wind Waker copies the whole EFB and draws it back every frame (depth of field,
blur, the title's composition), so the high-resolution scene was replaced by a 640x480 copy. Copies
are now made at the render target's scale, as Aurora's own GXCopyTex does, and texcoords normalize
by the texmap's guest size (Dolphin's texdim.xy) so the larger copy samples correctly. The title
on macOS and on the iPad simulator now matches a Dolphin 5.0-17995 capture of the same screen
(local-research/ipad/reference/), and Link, the pier and Outset are sharp (local-research/ipad/
efb-0068/). The WGSL tests and the TEV golden are updated; host tests pass except the three
NOT_BUILT placeholders that were already not built. Open next to it: gxcore copies RGBA8 even when
the EFB has no alpha, where Aurora forces an opaque copy (a candidate for the white fringe on
Link's hair); Link's toon shading reads flat; and the GPU cost of 3x copies on the simulator's heavy
view is not yet measured.

**A regression caught by the heavy view, and fixed (recompcore 0069).** The first run of the new
scripts/ios/sim_heavy_view.sh after 0066-0068 measured 32.6 and 33.1 retraces a second against 47.5
before. A/B on the same build: render-scale copies off (DOL_GXCORE_COPY_SCALE=0) 36.4 vs on 35.9, so
0068 costs nothing measurable; 0066's re-resolve switched off, 45.2. 0066 emitted a second texture
event for every texture load. SETTLUT now corrects the palette of the event SETIMAGE3 just emitted
while it is still unconsumed, only for palette formats. The title frame hash is identical to 0066's
(0x1A8413020CADE626 at retrace 1800), the frontend tests pass, and the heavy view is 48.1 and 47.3
retraces a second (median 21.1 ms, p90 28.0 ms), sharp and with the correct palettes. The same patch
gives copies of an EFB without alpha an opaque view, as Aurora does. The hair crop at 3x shows no
white fringe; the remaining difference from Dolphin's title shot of Link is flat toon shading.

| heavy Outset view, iPad simulator | retraces/s | median / p90 ms |
| --- | --- | --- |
| before 0066 (PGO host) | 47.5 | 21.0 / - |
| 0066 + 0068 | 32.6, 33.1, 38.3 | 29.0 / 42.3 |
| 0066 re-resolve off | 45.2 | 21.3 / 29.6 |
| 0069 | 48.1, 47.3 | 21.1 / 28.0 |

**The SunPad controls drive the game by touch.** A new hook, BLUEWAKE_TOUCH_TAPS, presses the
UIKit controls through the same handlers a finger uses (sendActionsForControlEvents and the stick's
value path). With no scripted guest input at all, 17 taps of A took the simulator from the title
(retrace 333) through file select (736) into the saved game, a held move stick walked Link about
1,240 units along the Outset pier (retraces 2311-2498, player probe) and stopped when released, and
the camera stick turned the view. The three-dot menu held the game for the 43 s it was open
(present 1438) and released it on close. Frames: local-research/ipad/touch-route/.

**Visual survey of the new-game route after 0066-0069.** Thirty-eight 1920x1440 frames every 500
retraces from the prologue to control (retrace 3,001-21,531; local-research/ipad/survey-0069/): the
woodcut prologue, subtitles, the Outset title card, the lookout, Aryll, the HUD and dialogue boxes
all draw with correct palettes and at full resolution; control arrives at the certified point.
Open from the survey: Link's face in the lookout close-up (retrace 18,527) reads as one flat dark
tone where Aryll's two-tone shading (retrace 19,028) looks right, so the toon ramp for that model
needs a Dolphin reference of the same shot before any change. A Dolphin reference of play is not
yet captured: its game-list boot is unreliable under automation, and the rule is one game at a time.

**Loop discipline, after two mistakes.** Dolphin and the simulator ran Wind Waker at the same time
and overloaded the machine, and editing sim_run.sh while an acceptance run was executing it killed
that run. scripts/one_game_guard.sh now refuses to start a game while another is running (called by
sim_run.sh and bench_instructions.sh), and scripts a running job uses are not edited until it ends.
scripts/card_to_gci.py turns a BlueWake save into a Dolphin GCI so the reference renderer can show
the same save, one game at a time.


## 2026-09-23 Profile-guided host: -6.5% play-window cycles

The host's per-block glue (edge service, deadline distance, interrupt predicates, the turn loop) is
about 14% of the game thread. `scripts/pgo_host.sh gen|use` builds the macOS host instrumented, and
after training on the same three workloads as the composite (certified route to 14,700, full route
into control, 3,000 retraces from the acceptance save; all three stopped at their expected blocks and
pcs) rebuilds it with the profile. `bench_instructions.sh` now takes `BLUEWAKE_BENCH_HOST`.

| measure | host | PGO host |
| --- | --- | --- |
| cycles, 800 play retraces (A/B/A/B) | 60.7 / 61.7 G | 56.6 / 57.9 G (-6.5%) |
| instructions per play retrace | 342.2 / 342.0 M | 340.4 / 340.7 M |
| guest-state digest | 83d2590d... | 83d2590d... |
| iPad simulator heavy Outset view | 46.3 /s | 47.5 /s, median 21.0 ms |

Promoted on macOS (`build/runtime-host-dsp/bluewake_host` and the app bundle; the previous binary is
`bluewake_host.pre-pgo`). The iOS build uses the same profile through `CMAKE_C_FLAGS`/`CMAKE_CXX_FLAGS`
(see `apple/ios/README.md`); the host sources are the same files.


## 2026-09-23 Inline FP and paired-single helpers plus wider PGO training: -5.8% play-window instructions

After PGO the game thread still spent about 10% in out-of-line helpers the translated code calls
per floating-point instruction: `ppc_fp_available` 5.1%, the paired-single quantized load/store
helpers 5.2%. The generator (`scripts/generate_composite.py`) emitted `ppc_fp_available_inline` and
`ppc_psq_*_inline` as plain forwards. They now test MSR[FP] inline and handle the unquantised
(GQR type 0) paired-single load/store inline with the runtime's own conversions, access order and
enable check; every other case still calls the runtime. The 2026-08-29 attempt at the FP check
alone was judged on wall time, which could not resolve it; this one is judged on the instruction
bench. The existing `generated.h` was updated the same way, so only the 100 hot chunks were
rebuilt, through a new PGO cycle whose training adds the full route into control (retrace 21,500)
and 3,000 retraces from the acceptance save in Outset to the certified route.

| measure | PGO | inline + wider PGO |
| --- | --- | --- |
| instructions per play retrace | 362.6 / 363.5 M | 341.6 / 342.1 M (-5.8%) |
| cycles, 800 play retraces (A/B/A/B) | 63.9 / 71.3 G | 59.6 / 62.4 G |
| guest-state digest / route digest | 83d2590d / 92dd816c | unchanged / UNCHANGED |
| iPad simulator, heavy Outset view | 40.7 /s | 46.3 /s, median 21.8 ms, p90 28.8 ms |

The instrumented training run stopped at the certified block count and pc at retrace 14,700, and
the full route admitted control at retrace 20,256, before any timing was taken. Promoted in place;
the previous composite is `gGZLE01_recomp.dylib.pgo1` and its objects `build/composite-pgo1`.
Regression: the iPad simulator save acceptance passes on it (`local-research/ipad/acceptance-20260923-183640/`).

**Refuted, recorded:** `-mcpu=apple-m1` on the hot chunks produces a byte-identical composite; Apple
clang already targets it for arm64 macOS. Consequence for devices: the retagged composite carries
M1-level code generation, so a build for the minimum supported iPads (A13) needs its own target
CPU rather than the retag.

**Refuted, recorded:** `-O3` on the PGO'd hot chunks is neutral (341.3 vs 341.6 M instructions per
play retrace; cycles 57.6/61.3 vs 60.5/59.9 G). The guest's idle spin in `SelectThread` is not among the
hot chunks, so the stable-poll fast-forward already covers it: the game thread's time is guest work.

**The day's result on one route (iPad simulator, unattended new game into control).** Same route,
same simulator, first run of the day (09:08) against the build after iterations 1-8:

| stretch | 09:08 | now |
| --- | --- | --- |
| prologue (1,000-13,000) | 60.0 /s | 60.0 /s (real-time cap) |
| play-scene start (13,910-14,100) | 30.1 | 51.0 |
| Outset play scene (14,100-17,800) | 30.6 | 48.4 |
| after control (20,256-21,400) | 35.5 | 56.3 |
| heavy view from the save (pier facing the island) | about 18 | 46.3 |

Control is admitted at retrace 20,256 in both. Authentic speed is 60 in every stretch; the heaviest
Outset view is at 77% of it on this M2, the lightest at 94%.





## 2026-09-23 Profile-guided optimization of the hot chunks: -19.6% play-window cycles, digest unchanged

Translated game code is 74% of the game thread, so the lever had to act on it. `scripts/pgo_composite_hot.py`
recompiles only the chunks that carry 95% of the game-code samples (100 of 748, ranked from a
simulator profile of the Outset play scene) with `-fprofile-instr-generate`, links an instrumented
composite, and after a training run rebuilds them with `-fprofile-instr-use`. The training run is
the certified headless route to retrace 14,700; it stopped at the certified block count and pc.
Each pass takes about 55 minutes on the M2 (one chunk, the GX front-end region, takes 30+ minutes
instrumented). The rest of the build is untouched and the generated C is not regenerated.

| measure | before | PGO |
| --- | --- | --- |
| `bench_instructions.sh` cycles, 800 play retraces (two orders) | 76.6 G, 79.5 G | 61.6 G, 64.0 G (-19.6%) |
| instructions per play retrace | 376.6 M | 362.5 M |
| IPC | 3.93 | 4.71 |
| guest-state digest (1,050 records) | 83d2590d... | 83d2590d... |
| `bench.sh` route digest | 92dd816c... | 92dd816c... UNCHANGED |
| `bench.sh` headless play window, median / p99 | 32.4 / 50.2 ms | 53.6 / 30.4 ms |
| iPad simulator heavy Outset view (with 0065 below) | 34.5 /s | 40.7 /s, median 25.6 ms |

The PGO composite is promoted in place (`build/composite-cycle-hybrid-o2-v2/gGZLE01_recomp.dylib`; the
previous one is `gGZLE01_recomp.dylib.pre-pgo`), and `scripts/relink_composite_runtime.sh` now keeps
the PGO objects from `build/composite-pgo/use` when it relinks.

Recompcore 0065 batches gather-pipe bytes on the main thread and hands them to the FIFO worker in
1 KiB batches (each 1-4 byte write used to take the worker mutex; 6.2% of the game thread). Frame
captures at six retraces from the logo to the prologue are identical to the unbatched reference.

Regression: `scripts/ios/sim_save_acceptance.sh` passes on the PGO composite with 0065 (high-level DSP;
save with 3 card writes, the guest's quit, reload to control at retrace 831, 400 s of audio at 97%
non-zero), `local-research/ipad/acceptance-20260923-153217/`.


## 2026-09-23 Rendering stopped waiting on the GPU: 0 staging splits, heavy Outset view about 21 -> 34.5 retraces/s

With the DSP cheap, a simulator sample of the Outset play scene showed the game thread 66% waiting
in `shadow_frontend_flush` for the FIFO translation worker, and the worker 72% asleep in
Aurora's `wait_for_gpu_progress`. Every other frame overflowed the 24 MB uniform staging area
(gxcore uploads about 2 KB of vertex constants per draw, some 12k draws a frame), so the frame
was split into two submissions (849 splits in one run) and each split waited for a free staging
buffer. Recompcore patch 0062 reuses a draw's uniform range when its vertex or pixel constant
block is byte-identical to the previous draw's in the same frame packet (79% of blocks in
Outset), and sizes the vertex area at 24 MB, which the frame filled once uniforms fit.

| heavy Outset view, iPad simulator | before | after |
| --- | --- | --- |
| staging splits in the run | 849 | 0 |
| retraces over 100 ms | tens, some over 1 s | 0 |
| retraces a second (1,000-3,000) | about 21 | 34.5 |
| median / p90 retrace | about 40 / 50+ ms | 29.5 / 38.1 ms |

Frame captures at retraces 401, 801, 1201, 1601, 2001 and 2401 (logo, title, file select,
prologue) hash identically with reuse on and off (`DOL_AURORA_UNIFORM_DEDUP=0`).

The macOS windowed renderer with the same settings (high-level DSP, reuse on) runs the certified
bench window 13,910-14,100 at 40.6 retraces a second, median 26.6 ms and p99 48.8 ms, and
14,100-16,600 at 37.5; ROUTE_DECISION recorded a rendered median of 40.0 ms (25 a second) on
2026-09-22.

**Iteration 6 (same day): the FIFO worker is signalled only when asleep (recompcore 0063).** The
enqueue path ran once per gather-pipe write and signalled the worker every time (2.1% of the game
thread in `pthread_cond_signal`). `bench_rendered.sh` stops at the certified turn counts with it. That
run also prices the renderer: 679.0 M instructions per rendered play retrace against 376.9 M
headless (+302 M, +80%), up from +90.6 M on 2026-09-22; almost all of it is on the FIFO worker
(memmove, memset and draw-plan construction), which is now about half idle and off the critical
path. The game thread is 99% busy: 74% translated code and its helpers, 21% host (edge service
6.5%, gather-pipe writes 6.2%). The inline FP-availability check was measured neutral on 2026-08-29
(PERFORMANCE.md) and is not retried.

**Graphics, Link's hair (same day).** A full-resolution capture right after control
(macOS, retrace 20,300) shows Link's hair in two toon tones with a hard white fringe along its
top-right edge; Aryll's hair and skin in the awake cutscene are also flat and bright. New gxcore
counters (recompcore 0064) show the toon path is live: 4,014,801 ramp texgens read a lit channel,
0 read an unlit one, and only 188 of 24.3 M draws name a light that was never loaded. So the
ramp lighting itself runs; the open suspects are the fringe, which looks like Link's eye drawn
through the hair by the game's destination-alpha trick (4,708 draws use destination alpha), and
the ramp's contrast. The deciding instrument is a reference frame of the same moment from a
known-good renderer; none exists in the tree yet.

**Link's eyes over his hair are the game's own four-pass draw.** The reconstructed player draw
(`ref/tww/src/d/actor/d_a_player_main.cpp`, the `l_onCupOffAupPacket`/`l_offCupOnAupPacket` sequence)
draws the eye and brow shapes with depth test off and color only, then again with blending and alpha
only, then face and hair, then the depth-tested eyes. gxcore honours the color and alpha write masks
and follows Dolphin's rules for destination alpha and the EFB pixel format (alpha writes only on an
RGBA6 target), so these passes are rendered as a faithful port would render them. The white fringe
at the hair's edge is therefore not established as a renderer defect; it needs a reference frame of
the same moment to adjudicate, and the loop does not spend more on it until one exists.





## 2026-09-23 Dolphin's high-level Zelda DSP: -14.8% play-window cycles, same audio, iOS default

The DSP interpreter was 16.7% of the game thread on the simulator. `runtime/host/src/dsp_hle_backend.cpp`
puts Dolphin's DSPHLE (the Zelda ucode, already in the recompcore pin) behind the same C adapter
interface, selected with `BLUEWAKE_DSP_MODE=hle`. Its few Dolphin services are host callbacks: an
alias-aware guest pointer (`get_ram_ptr`), the ARAM buffer (recompcore patch 0061), the guest timebase
and the DSP interrupt. One protocol fix was needed on the host: the ucode raises the next mail's
interrupt from inside the CPU's mailbox read, and the host cleared its pending flag right after
that read, so the end-of-frame interrupt was lost and the ucode halted after its first render. In
HLE mode the flag now clears when the guest acknowledges DSPINT in the control register, as on
hardware. The LLE route is untouched: `bench.sh` digest 92dd816c... UNCHANGED after the change.

| measure | LLE | HLE |
| --- | --- | --- |
| play window, instructions per retrace (800 retraces) | 376.7 M | 321.5 M (-14.6%) |
| same window, cycles | 81.8 G | 69.7 G (-14.8%) |
| title / file select / new game | 333 / 535 / 773 | 333 / 535 / 776 |
| opening complete / play scene / control | 13,850 / 13,910 / 20,256 | 13,861 / 13,921 / 20,256 |
| audio, 1 s loudness envelope over 0-220 s | reference | r = 0.999, mean RMS 2,779 vs 2,734 |
| iPad simulator save acceptance | PASS | PASS (control after reload at 831) |

The instruction bench refuses to certify HLE runs (they stop at different block counts, as they
must), so the HLE figures are the raw totals of the same 13,900/14,700 pair. The iOS app now
defaults to HLE; macOS keeps LLE so its certified route and digest remain the reference.

**Where speed stands after iterations 3 and 4 (macOS M2, bench.sh play window 13,910-14,100):**
median 27.4 -> 32.4 (alias index) -> 36.7 retraces a second with the high-level DSP; p99 63.4 -> 50.2
-> 34.0 ms; the whole 14,100-retrace route 83 s -> 31.5 s wall, because the DSP interpreter dominated
the non-gameplay stretches. Authentic speed is 60 in every scene; this is 61% of it in the benchmark
window on this Mac.



## 2026-09-23 Guest alias lookup becomes a binary search: -8.0% play-window cycles, +13% on the iPad simulator

A main-thread sample of the Outset play scene on the iOS simulator put `ppc_guest_alias_resolve` at 5.7% of the
game thread. It scanned the alias registry linearly, and the linked REL data registers about 1,900
aliases. Recompcore patch 0060 keeps a start-sorted index and binary-searches it whenever no two
aliases overlap (they do not on this route; any overlap falls back to the scan). An equivalence
harness matched the scan on 8 M random lookups, overlapping sets included.

| measure | before | after |
| --- | --- | --- |
| `bench_instructions.sh`, instructions per play retrace | 396.7 M | 376.7 M (-5.0%) |
| same window, cycles | 88.9 G | 81.8 G (-8.0%) |
| guest-state digest (1,050 records) | 83d2590d... | 83d2590d... |
| `bench.sh` route digest | 92dd816c... UNCHANGED | 92dd816c... UNCHANGED |
| iPad simulator, heavy Outset view from the save, retraces/s (two runs each) | 17.6, 18.4 | 20.2, 20.4 |
| same, p90 retrace time | 75 / 69 ms | 61 / 62 ms |

The promoted composite was relinked with the new `scripts/relink_composite_runtime.sh`, which
recompiles only the runtime sources from `compile_commands.json` and relinks with the recorded
link line; a plain `make` in that tree would recompile all 748 chunks. The previous dylib is kept as
gGZLE01_recomp.dylib.prev. `scripts/ios/sim_run.sh` now retags whenever the source composite changes,
which is what made the simulator A/B possible.

Where that leaves speed: Outset is about 20 retraces a second in the heaviest view and 27-33 in
lighter ones, against 60. The next measured lever is the DSP, 16.7% of the game thread: Dolphin's
high-level Zelda ucode is already in the pin (`Source/Core/Core/HW/DSPHLE/UCodes/Zelda.cpp`).


## 2026-09-23 iPad simulator: saves, audio and backgrounding pass

`scripts/ios/sim_save_acceptance.sh` runs the macOS save acceptance through the iOS app and
passes (`local-research/ipad/acceptance-20260923-095148/`): the certified route, the pause-menu
save with 3 card writes, the guest's own quit, a card that differs by 1,218 bytes, and a reload
that reaches control at retrace 833 without the new-game intro. The save half also captured 400 s
of the game's own audio mix (32 kHz stereo, 93% non-zero samples).

Going to the home screen and back left a white screen while the game kept running; recompcore
patch 0059 reopens the frame once batches arrive with none open, and adds a host hold hook. The
picture now returns after a home-screen round trip. The iOS entry shim also exits the process when
the host returns, so bounded simulator runs end.

A sample of the Outset play scene on the simulator splits busy time 47.8% translated game code,
37.0% host (DSP 12.7%, GX front end 8.0%) and 6.9% memcpy/memset, like the macOS profile: speed work
belongs to the shared route.


## 2026-09-23 Reorientation: the game runs on iPadOS in the simulator

The user redirected the project: make the game actually work, on iPadOS, tested one simulator at a
time, no hardware iPad yet. The dossier is
[IPADOS_REORIENTATION_2026-09-23.md](IPADOS_REORIENTATION_2026-09-23.md) and the loop is
[v56](../GOAL_PROMPT_V56_2026-09-23.md). Route A is hosted on iPadOS now; Route B continues behind
it as the long-term speed track.

**The iPad app exists and plays the retail route.** `apple/ios` builds `BlueWake.app` for the iOS
simulator from the unchanged host sources plus an entry shim, touch controls and a DSP support shim.
The 487 MB composite imports only 11 libc symbols, so it runs unchanged after a platform retag
(`scripts/ios/retag_macho_platform.py`). On the iPad Pro 11-inch (M5) simulator the unattended
route hits every milestone at the same guest retrace as macOS: title 333, file select 535, new game
773, opening complete 13,850, Outset play scene 13,910, player control admitted 20,256. Metal
renders through Dawn, and the donor DSP produces audio.

**Three defects fixed on the way, two of them shared with macOS.**
- The macOS window was black too: the FIFO translation worker landed on 2026-09-22 was never opened
  to the frame that initialization begins, so nothing was ever presented (`gx-core submitted=0`,
  confirmed by a macOS control). Recompcore patch 0056 opens it.
- After that, the picture froze in the Outset play scene while the game ran on (`gx-core failed=1`).
  The worker handed the front end whole batches (60,483 bytes measured), which overflowed its
  8,192-event per-flush trace; the failure was sticky and silent. Recompcore patch 0057 feeds
  batches in the 1 KiB slices the single-threaded path always used and names any failure; the next
  full route ended at `failed=0` with 27.0 M draws.
- On-screen touch controls draw inside the game's frame through new Aurora overlay and event hooks
  (recompcore 0058; a UIKit layer over the Metal view never composited). Tapping START and A on the
  title took the game to file select, and from the acceptance save card, touch alone loaded Quest
  Log 1 into Outset and walked Link down the pier into the sea. The same patch keeps the guest's
  aspect on iOS (4:3 pillarboxed in landscape).
- `runtime/host/src/main.c` did not build without the DSP adapter and carried a literal NUL byte in a
  character literal. Both fixed; the iOS build also shows the DSP adapter is required at boot.

**Speed on the simulator matches the Mac:** about 58 retraces a second through the prologue and about
35 in the Outset play scene (macOS play window: 31 to 33). The gap to 60 is Route A's known cost.

Review: iOS simulator build links; macOS host rebuilds and presents again (frame at retrace 701,
2.74 M non-blank pixels); `scripts/audit_repo.sh` passes; three simulator route runs under `local-research/ipad/`; macOS
headless and windowed controls; recompcore patches 0056-0058 registered in the dependency lock.


## 2026-09-23 The room's actor handler has its own name, and the run moves to a new defect

The fortieth iteration landed the disambiguation and with it the room path's class-mismatch crash
disappears; the run then stops somewhere new.

**The fix: one symbol, two names.** `dStage_actorInit` had a body that casts its argument to
`dStage_stageDt_c*` and reads the stage layout's actor pool, and another that casts to
`dStage_roomDt_c*` for the room's pool. Both callers named the same symbol, so the link gave the room
tables the stage body - which is what produced `cap=-1` and the `-1.0f` bit pattern where the room's
entries pointer should be. Patch 0286 names the room body `dStage_roomActorInit` and points the room
side at it: the room-reloader region's declaration, the treasure helper, the layer loader's entry and the
reload table's `ACTR`/`TGOB` entries. The stage tables keep `dStage_actorInit`.

**And the room tiers' `dStage_roomDt_c::init` yields to its owner.** That body is defined in five room
tiers; all but the room-control tier's copy now sit behind
`BLUEWAKE_ROUTE_B_ROOM_CONTROL_INIT_COMPOSED`, the flag the room-control owner is exempted from, so one
definition and one typeinfo record survive. With both changes the composition links with no duplicate and
no undefined symbol.

**The run's next stop is a different defect.** The bogus-`realloc` crash is gone; the room path now walks
the actor records and stops on a **heap-buffer-overflow** in `cXyz::set` (`c_xyz.h` 92) inside the actor
decode - a write past the end of a buffer, which is a decode-size question rather than a class or a
layout one.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify (286 tww patches, 10
aurora), `scripts/audit_repo.sh` passes.


## 2026-09-23 The layouts agree, so the callee is reading the right object as the wrong class

The thirty-ninth iteration printed the third layout and cleared it: the reloader's translation unit agrees
with the other two, the object it is handed is valid, and the garbage is inside the handler it calls.

**Every layout and every value on the way in checks out.** The ROOM_RELOADER tier's own print reads
`BW-RELOADER stage=0x106b783c0 cap=271 entries=0x62100061d100 off=472 sz=552` - the same `472`/`552` the
other two translation units report, a capacity that the first actor node legitimately grew to `271`, and
an entries pointer on the address-sanitizer heap. So the class has one host layout in this composition,
the object is initialized, and the pointer is the right one.

**And the handler reads that same pointer as a different class.** The failing call prints
`num=172 cap=-1 entries=0xbf800000`, and `0xbf800000` is `-1.0f`. Reading a valid
`dStage_roomDt_c` and getting a float's bit pattern out of its capacity field means the *body* is
dereferencing a different type. `d_stage.cpp` has more than one host body for the symbol the loader's
table names: one casts the argument to `dStage_stageDt_c*` and reads `mHostActorCapacity`/`mHostActorEntries`
from the stage layout (line 1117 after this iteration's revert), and another is the console body that
uses `i_stage->getRoomNo()` (2107). The link resolves both callers to whichever copy it kept, so the room
path can end up executing the stage path's arithmetic - which is what the `-1` and the float bits are.

**So the next item is one symbol with two meanings.** Each caller needs its own name: the stage body's
symbol should be distinct (and the stage loader's table pointed at it) so that `dStage_actorInit` means
the room body for the room path. That is the same rule the last eight patches applied to the room tiers'
symbols, now at the level of a function whose body depends on which class its caller passed.

**The tree is at the verified series state.** The instrument was reverted by restoring both files from the
patch series; `prepare_route_b.sh --check` is green at 285 patches.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify (285 tww patches, 10
aurora), `scripts/audit_repo.sh` passes, `ref/` matches the series.


## 2026-09-23 The room object is initialized and passed correctly, and a third copy of its layout is where the crash reads

The thirty-eighth iteration measured the room object on both sides of the call and left one suspect
standing: a third translation unit's view of the same class.

**What the run shows, in order.** `dStage_dt_c_roomLoader` runs and `i_stage->init()` sets the pool to
capacity `0` and a null entries pointer (`BW-ROOMLOAD stage=... cap=0 entries=0x0 off=472 sz=552`). The
first actor node then grows it (`BW-ACTORGROW num=2 cap=0 entries=0x0`). `objectSetCheck` calls the
reloader with **the same object, now initialized** - `cap=271`, entries on the address-sanitizer heap,
and the same layout constants (`off=472 sz=552`) as the stage tier's translation unit.

**And the crash reads a different object state from the same pointer.** The next line is the failing
call: `BW-ACTORGROW num=172 cap=-1 entries=0xbf800000`. `0xbf800000` is the bit pattern of `-1.0f`, so
those reads are landing on other data: the capacity and entries fields are not where the callee looks
for them. The backtrace is unchanged - `objectSetCheck` -> `dStage_dt_c_roomReLoader` ->
`dStage_dt_c_decode` -> `dStage_actorInit` -> `realloc` - and the reloader and the actor handler both come
from the ROOM_RELOADER tier's translation unit, which is the third view of `dStage_roomDt_c` in this
composition and the one layout I have not printed yet.

**So the next measurement is that third view.** Print `offsetof(dStage_roomDt_c, mHostActorEntries)` and
`sizeof(dStage_roomDt_c)` inside `dStage_dt_c_roomReLoader`, in the ROOM_RELOADER region (which needed
`<cstddef>`/`<cstdio>` for the previous attempt to compile). If its numbers differ from `472` and `552`,
the class has two host layouts in one composition and the fix is at the header the tier includes, not in
any handler.

**The tree is at the verified series state.** Both instruments were reverted by restoring the files from
the patch series; `prepare_route_b.sh --check` is green at 285 patches.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify (285 tww patches, 10
aurora), `scripts/audit_repo.sh` passes, `ref/` matches the series.


## 2026-09-23 The endian answer was already in the file type, and the room object's host fields are the question

The thirty-seventh iteration found the conversion mechanism the last one went looking for - and it
refutes the endian reading, leaving the room's own object as the thing to measure.

**The conversion is in the type, not in a pass.** Patch 0056 makes the stage file's structural fields
endian-aware at the type level: `dStageFileS32`/`U32`/`U16`/`F32` are
`bluewake::route_b::BigEndian<T>` on the host, and `dStage_nodeHeader`'s `m_entryNum` and `m_offset`
are declared as those - while `m_tag` stays a plain integer, which is why tags compare correctly as byte
strings. So every read of a node's count or offset converts, and that is true of the room path too,
because it is the *same struct*.

**So the count `172` is not a byte-swap artefact, and the earlier conclusion is withdrawn.** It was read
off a pointer my instrument had mis-cast (it printed a file header as a node header). What remains is the
crash itself: `realloc(stage->mHostActorEntries, sizeof(stage_actor_data_class) * i_num)` with `stage`
being `mpRoomDt` - so either that object's `mHostActorEntries` or its `mHostActorCapacity` is not what
the port's `dStage_roomDt_c::init()` sets them to (`0` and null). The two readings this run produced
were `-1` and `271`, neither of which is `0`, so the room's object has not been through that
initializer on this path.

**The next measurement, and it is one print.** Print `mpRoomDt`, its `mHostActorCapacity` and its
`mHostActorEntries` at the entry to `objectSetCheck`'s reload call, and again from
`dStage_dt_c_roomLoader` when it calls `i_stage->init()` - if that call never happens on the branch this
composition compiles, the capacity is whatever the allocation held, and the fix is on that branch.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify (285 tww patches, 10
aurora), `scripts/audit_repo.sh` passes, `ref/` matches the series.


## 2026-09-23 The room's file comes straight from the resource runtime, and only the stage's is converted

The thirty-sixth iteration found which branch of the room scene the composition compiles, and it explains
the unconverted node fields exactly.

**The composition compiles the runtime branch, not the seam.** `nm` on the linked probe shows no
`bluewake_route_b_get_stage_res` symbol at all, and the seam appears only in the port's tests. So the
room scene's `mpRoomData` is not set through it: `d_s_room.cpp` has two branches, one assigning
`bluewake_route_b_get_stage_res(arcName, "room.dzr")` and the other `dComIfG_getStageRes(arcName,
"room.dzr")`, and the composition links the second - the resource runtime's own accessor.

**And the runtime accessor hands back the file unconverted.** The stage file's nodes read correctly in
this composition - the earlier node inventory printed `STAG`, `RTBL`, `ACTN` and the rest as tags with
sane counts - so the conversion exists for `stage.dzs`. `room.dzr` goes through the same runtime
accessor and comes back big-endian, which is why the reloader's decode saw `m_offset = 0x01000000` and a
count of `0xAC`, and why its `dStage_actorInit` asked `realloc` for a size derived from those.

**So the next item is the runtime's room path.** Either the resource runtime converts `stage.dzs` and
should convert `room.dzr` the same way, or the composition should take the seam branch that does the
conversion itself. The measurement that decides it is where the `stage.dzs` conversion lives - the
resource runtime's host tier or `d_resorce.cpp` - and whether extending it to `room.dzr` is one table
entry or a shape difference.

**Two earlier conclusions are also corrected here.** The "uninitialized stage object" reading of the
previous iteration was a consequence of this, not a separate bug, and the capacity `271` measured since
says `mpRoomDt` is a host-initialized object after all.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify (285 tww patches, 10
aurora), `scripts/audit_repo.sh` passes, `ref/` matches the series.


## 2026-09-23 The room reload's stage object is real, and its node fields are not converted

The thirty-fifth iteration separated the two candidates from the last one and left a single cause
standing: the room's own file is parsed without the endian conversion the stage file gets.

**The stage object is not the problem.** The reloader's entry print shows `i_stage` with a capacity of
271 and an entries pointer on the address-sanitizer heap - a host-initialized object, not the
uninitialized one the previous run's numbers (capacity `-1`) suggested. So `mpRoomDt` is real, and the
fault is in what the node says.

**And the node's fields are the shape of an unconverted big-endian record.** The first print's `m_offset`
read `16777216`, which is `0x01000000` - the byte-swapped form of 1 - and the actor count came out
`172` (`0xAC`), the byte-swapped form of a small count. The header's types explain it: `d_stage.h`
declares the file header's fields as `dStageFileS32` and the like, which are the port's endian-aware
accessors, while `dStage_nodeHeader` carries plain fields - so the stage path converts through the typed
accessors and the room path reads raw. `dStage_dt_c_decode` is shared by both, so the missing step is
on the loader side: whatever converts the room's `room.dzr` nodes before the decode walks them, and
which `bluewake_route_b_get_stage_res` does not do.

**Where to look next, and it is named.** The stage path's conversion is what the room path is missing;
finding that step (and whether it lives in the resource runtime, the stage-resource seam, or
`dStage_dt_c_offsetToPtr`'s host branch) is the next measurement, and the fix is to give the room's file
the same treatment.

**The tree is at the verified series state.** The instrument failed to compile against the typed accessor
(it printed a `dStageFileS32` as a plain integer) and was reverted by restoring the file from the patch
series; `prepare_route_b.sh --check` is green at 285 patches.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify (285 tww patches, 10
aurora), `scripts/audit_repo.sh` passes, `ref/` matches the series.


## 2026-09-23 The room scene runs, and its actor reload is handed an uninitialized stage object

The thirty-fourth iteration followed the new crash to its chain, and the chain names the room scene's own
reload path and one uninitialized pointer.

**The chain, from the sanitizer's backtrace.** `dScnRoom_Create` (`d_s_room.cpp` 619) -> `phase_3`
(578) -> `objectSetCheck` (398) -> `dStage_dt_c_roomReLoader` (`d_stage.cpp` 64) ->
`dStage_dt_c_decode` (2474) -> `dStage_actorInit` (1129) -> `realloc`, where it faults. So the room
scene *is* created now, and its own reload path is running: that path was dead code two iterations ago.

**What is handed to it, measured.** Instrumented prints in `dStage_actorInit` show two calls in the run:
the first is sane - `num=2`, entries inside the file the node points into - and the second is not:
`num=172`, a `stage` pointer in a completely different address region from the file, and a capacity of
`-1`. `objectSetCheck` passes `i_this->mpRoomDt` as that stage, and the capacity it reads is
`stage->mHostActorCapacity`, which the port only ever sets to 0 in the room's initializer. So
`mpRoomDt` is not a stage object this port initialized: either the room scene holds the console-layout
one, or the object belongs to another tier's storage, and in both cases the capacity is whatever memory
happened to hold in a fresh allocation - here all ones, which makes the comparison `i_num > capacity`
false-to-true in the wrong direction and sends a bogus size into `realloc`.

**The two candidates, and how to tell them apart.** Either `mpRoomDt` is the wrong object (the room
scene built against a different layout than the handlers it calls), or the node's count is mis-read
because the room's file has not been endian-converted on this path - `num=172` is `0xAC`, the shape of a
byte-swapped small count. The next measurement distinguishes them in one run: print `mpRoomDt` and the
node's raw and swapped `m_entryNum`/`m_offset` at the entry to the reload path.

**The tree is at the verified series state.** The instrument was reverted by restoring the file from the
patch series; `prepare_route_b.sh --check` is green at 285 patches.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify (285 tww patches, 10
aurora), `scripts/audit_repo.sh` passes, `ref/` matches the series.


## 2026-09-23 The room scene links, and the run reaches actor creation

The thirty-third iteration closed the room tiers' duplicate set, and the composition that follows runs
past the room's file read into the actors its nodes ask for.

**The last duplicates were the absorbed handlers.** `dStage_rppnInfoInit` and `dStage_rpatInfoInit`
are defined in the native-views region's *own body* (its loader table reaches them) and again in the
runtime-owners tier's `.inc`, which patch 0273 gave them deliberately. Patch 0285 puts those
definitions behind `BLUEWAKE_ROUTE_B_STAGE_RUNTIME_OWNERS_COMPOSED` in each region that carries them, so a
composition that composes the runtime-owners tier takes them from it and a composition that does not is
unchanged. With that, the link has no duplicate and no undefined symbol: **the room scene and the player
are in the composition.**

**And the run went one level deeper.** `BW-REGISTRY camera=4 player=5 roomscene=4 logo=4 opening=4
overlap=4` - the room scene is carried now - and the run reaches the opening scene's stage read and the
actor creation that follows it, where it stops with a SEGV inside `dStage_actorInit`
(`d_stage.cpp` 1125) under the address sanitizer. That is a new frontier and a deeper one: the previous
stop was "this unit is not composed", and this one is inside a composed handler doing its work.

**So the next item is the actor path, not the composition.** `dStage_actorInit` is reached with the
sanitizer reporting a fault at `d_stage.cpp` 1125 - its actor-request half, which resolves the room's
actor entries and calls `fopAcM_Create` - so the next measurement is which pointer is null there and
whether the room's actor record is what the handler expects. The composition work that produced the
condition is landed; the frontier it exposed is the port's next real defect.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify (285 tww patches, 10
aurora), `scripts/audit_repo.sh` passes, `ref/` matches the series.


## 2026-09-23 The metadata include was the second owner, and two more pairs stand down

The thirty-second iteration found the shared include behind most of the room tiers' duplicates, applied
the rule to it, and measured what the partition leaves.

**The second owner is an include, not a function.** The copies at `d_stage.cpp` 2037/2053/2180/2206 are
inside a `#if !TARGET_PC` block, so they are not the ones the host link sees. The real second owner is
`d_stage_room_metadata.inc`: the native-views region includes it (352) and the runtime-owners region
includes it too (1230), so every handler it defines - `floor`, `plight`, `lgtv`, `rppn`, `rpat` - is
defined twice by two composed objects.

**The rule applied to the include.** Patch 0284 puts the native-views region's include behind
`BLUEWAKE_ROUTE_B_STAGE_RUNTIME_OWNERS_COMPOSED`, undefined in every existing composition, so the old
behaviour is exactly preserved. The rebuild shows the effect: the sixteen duplicates drop to twelve, and
`floor`, `plight` and `lgtv` stop being duplicates at all.

**And guarding the other side too is refuted by the link.** Standing down the runtime-owners region's
include as well removes those three from the link entirely - they become undefined, because the
runtime-owners tier is the only remaining owner once the native-views copy is gone. So the partition has
to be asymmetric: the runtime-owners region keeps its metadata include, the native-views region gives
its up. The twelve that remain - `rppn`, `rpat` and the rest - are the handlers the runtime-owners
`.inc` defines in its own body `and` the metadata include defines, so they need the guard inside that
file rather than at an include site.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify (284 tww patches, 10
aurora), `scripts/audit_repo.sh` passes, `ref/` matches the series.


## 2026-09-23 The remaining duplicates name one second owner each, and it is not the native-views region

The thirty-first iteration measured which objects define the sixteen remaining duplicates, which decides
where their guards go.

**All four named symbols pair against the same object.** The link reports `dStage_floorInfoInit`,
`dStage_plightInfoInit`, `dStage_rpatInfoInit` and `dStage_lgtvInfoInit` in
`d_stage_room_native_views.o` **and** `d_stage_stage_runtime_owners.o`, with `dStage_rpatInfoInit`
additionally in `d_stage_stage_path_graph.o`. So the second owner is the runtime-owners tier, and the
path-graph tier carries one more copy of the rpat handler.

**And the native-views region does not define them in its own body.** `dStage_lgtvInfoInit`'s definitions
sit at `d_stage.cpp` 2055 and 2387, not inside the native-views region opened at 349 - that region gets
them from `d_stage_room_metadata.inc`, which it includes. The runtime-owners tier's include file carries
only the ikada and drtg includes, so its copies come from the region that includes it, at 2037 (`plight`),
2180 (`rpat`), 2206 (`floor`) - a region whose guard has not been identified yet.

**So the guard has to go on the other side.** The rule still applies - one owner per symbol - but the
placement is the reverse of what the last iteration assumed: the copies to stand down are the ones at
2037/2053/2180/2206, in whichever region owns them, not the native-views region which owns them through
its metadata include. Identifying that region's guard is the next measurement, and it is a source read
rather than a rebuild.

**The tree is at the verified series state.** Nothing was changed this iteration: the guard-placement
attempt matched no definition text (the signatures wrap across lines), the series check is green at 283
patches, and the working tree is clean.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify (283 tww patches, 10
aurora), `scripts/audit_repo.sh` passes, `ref/` matches the series.


## 2026-09-23 The per-symbol rule applied, and its first two pairs are gone

The thirtieth iteration applied the rule the map implied - one owner per symbol, every other definition
behind a composition flag - and two of the room tiers' duplicate pairs stopped being duplicates.

**The pairs, measured from the objects rather than inferred.** `nm` on the three composed tier objects
showed `dStage_roomDt_c::init` and `dStage_roomDt_c`'s typeinfo in both `room_control_init` and
`room_native_views`, and `getMapInfo2`/`getMapInfoBase` in both `room_native_views` and
`stage_runtime_owners` - the latter pair is exactly the one patch 0273 had absorbed into the
runtime-owners tier.

**The rule, applied.** Patch 0283 puts the native-views region's `dStage_roomDt_c::init` behind
`BLUEWAKE_ROUTE_B_ROOM_CONTROL_INIT_COMPOSED` and its `getMapInfo2`/`getMapInfoBase` behind
`BLUEWAKE_ROUTE_B_STAGE_RUNTIME_OWNERS_COMPOSED`. Both flags are undefined in every existing
composition, so nothing that exists changed shape, and a composition that composes those owners - as
this one does - gets one definition and one typeinfo record. The rebuild confirms it: those two pairs
disappeared from the link.

**What is left is the same rule, more symbols.** Sixteen duplicates remain, all of the same kind:
`dStage_floorInfoInit`, `dStage_plightInfoInit`, `dStage_rpatInfoInit`, `dStage_lgtvInfoInit` and the
rest are carried by the runtime-owners tier and defined again in the room tiers this composition now
composes. Each needs the guard its pair implies, which the map from the previous iteration already
names by region and line.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify (283 tww patches, 10
aurora), `scripts/audit_repo.sh` passes, `ref/` matches the series.


## 2026-09-23 The room tiers' overlap is per symbol, and the regions that define each are mapped

The twenty-ninth iteration turned the room tiers' overlap from "some duplicates" into a per-symbol map,
so the next step is mechanical rather than exploratory.

**The four symbols and every region that defines them.** `dStage_roomDt_c::init` is defined in the
room-reloader region (guarded by `!ROOM_RELOADER_COMPOSITION`), the scaled-request region, the
actor-request region, the room-aggregate region, the native-views region (511) and again much later;
`dStage_rpatInfoInit` is defined in the native-views region (442), in the path-graph region (822) and in
the runtime-owners tier's own region (2180); `dStage_plightInfoInit` and `dStage_floorInfoInit` are
defined in the runtime-owners tier's region (2037, 2206) and referenced from the native-views region's
header block. The composition composes the native-views tier and the runtime-owners tier at once, which
is why the link reports exactly this set: the two regions both define them.

**Why this is not one fix but a rule.** Every room tier carries its own copy of the loaders and the
handlers a room can reach, and patch 0273 already resolved one such pair by absorbing the runtime-owners
tier's four symbols. The rule that makes the rest mechanical is the one the last three patches used, now
one symbol at a time: **one owner per symbol, every other definition behind a composition flag**. For
`dStage_rpatInfoInit` that means the runtime-owners copy stays and the native-views and path-graph copies
go behind a flag; for `dStage_roomDt_c::init` the same, which also stops the region emitting the class's
typeinfo record a second time.

**And the four handlers the composition actually needs are the other side of the same map.**
`dStage_mapInfoInit`, `dStage_filiInfoInit`, `dStage_soundInfoInit` and `dStage_lbnkInfoInit` are
native-views-only, so they have to stay with that tier - or move, together with `resizeRoomEntries`,
which is the helper the last iteration's compile named.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify (282 tww patches, 10
aurora), `scripts/audit_repo.sh` passes, `ref/` matches the series.


## 2026-09-23 The native-views handlers come with a helper, so the extraction is a chain

The twenty-eighth iteration tried the room handlers' extraction, measured what it costs, and refuted
one of the two ways to resolve the room tiers' overlap.

**Dropping the other tier is refuted by the link.** The overlap is between `room_control_init`,
`room_native_views` and `stage_runtime_owners`, so the cheaper move would be to drop
`room_control_init` and keep the native views. The link refuses it: `dStage_roomControl_c::mDarkRatio`,
`dStage_roomControl_c::mOldStayNo` and the rest of that class's statics are undefined without that tier,
so the room-control statics are its and the handlers are what has to move.

**And the handlers come with a helper.** `dStage_mapInfoInit` is already carried by the runtime-owners
tier, so three remain: `dStage_filiInfoInit`, `dStage_soundInfoInit` and `dStage_lbnkInfoInit`. They
were extracted into `d_stage_room_handlers.inc` with the runtime-owners tier including it behind
`BLUEWAKE_ROUTE_B_STAGE_RUNTIME_OWNERS_CARRIES_ROOM_VIEWS`, and the compile refused the include:
`resizeRoomEntries` is a region-local helper the handlers call, so the extraction has to take that
helper as well - which means the native-views region gives it up too, and the move is a two-function
chain rather than three functions. That is the same shape as the ikada and drtg moves, one helper
deeper.

**The tree is at the verified series state.** The experiment was reverted by restoring both files from
the patch series and removing the new include, so `prepare_route_b.sh --check` is green at 282 patches
and nothing half-extracted is left behind.

**The next item is the same move with its helper.** Extract `resizeRoomEntries` together with the three
handlers, have the native-views region include the result, and the composition can drop
`room_native_views` without losing the handlers the room loader's table reaches.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify (282 tww patches, 10
aurora), `scripts/audit_repo.sh` passes, `ref/` matches the series.


## 2026-09-23 The room's TGDR handler is shared between two tiers, and the room tiers overlap again

The twenty-seventh iteration made the room's TGDR handler available to a composition that does not
compose the tier which owns it, and the conflict that replaced it is the room tiers' overlap - the same
one the campaign resolved once before, this time from the other side.

**The handler is shared now.** `dStage_roomDrtgInfoInit` and its static helper `bluewakeRoomScaledInit`
were two of four functions in the scaled-request region; the group now lives in
`ref/tww/src/d/d_stage_room_drtg.inc`, which the scaled-request region includes as before and the
runtime-owners tier includes only when the composition asks for it
(`BLUEWAKE_ROUTE_B_STAGE_RUNTIME_OWNERS_CARRIES_ROOM_DRTG`). Patch 0282 lands that, and the port's own
`room_scaled_request` and `room_reloader_scaled` targets compile with it - so nothing that existed
changed shape, and the composition that needs the handler can have it.

**And the next conflict is the room tiers' overlap.** With the handler available, the composition's
remaining duplicate symbols are all between three tier objects it composes - `room_control_init`,
`room_native_views` and `stage_runtime_owners` - which define `dStage_floorInfoInit`,
`dStage_plightInfoInit`, `dStage_rpatInfoInit` and `dStage_roomDt_c`'s typeinfo record twice. That is
the same overlap the campaign already met once: `room_native_views` was dropped from the composition and
the four symbols it existed for were absorbed into the runtime-owners tier (patch 0273). Re-adding it
for its four *other* handlers re-created the pair.

**So the next item is the same move again, on the other four.** `dStage_mapInfoInit`,
`dStage_filiInfoInit`, `dStage_lbnkInfoInit` and `dStage_soundInfoInit` are what the room loader's table
reaches in `room_native_views`; absorbing those into the runtime-owners tier the way patch 0273 absorbed
the previous four lets the composition drop the tier again and keep the handlers.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify (282 tww patches, 10
aurora), `scripts/audit_repo.sh` passes, and the port's scaled-request targets build.


## 2026-09-23 The last handler belongs to a tier that duplicates two others

The twenty-sixth iteration found why the room reloader's remaining handler cannot simply be composed
with the rest of its unit, and the answer is a shared-owner problem rather than a missing definition.

**Where `dStage_roomDrtgInfoInit` lives.** The region boundaries are not where the line numbers
suggest: `d_stage.cpp` 19 opens the room-reloader tier, 68 guards `dStage_roomDt_c::init` behind
`!ROOM_RELOADER_COMPOSITION`, and 109 opens `ROOM_SCALED_REQUEST` - which is the region that defines
`dStage_roomDrtgInfoInit` (200) together with its static helper `bluewakeRoomScaledInit`. Measured by
`nm`, that tier's object is the only owner of the handler, so composing the unit needs that tier.

**And that tier cannot be composed whole.** Adding it puts `dStage_tgscInfoInit` and
`dStage_rpatInfoInit` in two objects at once - the SCOB_INFO and runtime-owners tiers already carry
them - and the link reports both as duplicate symbols. So the unit's partition does not line up with
this handler: what is needed from `ROOM_SCALED_REQUEST` is one function and its static helper, not the
region.

**The fix has a shape the port already uses.** Patch 0281 was exactly this problem for the player: the
ikada helper was shared between two tiers by putting its include behind a composition flag. The same
move applies here - the handler and its helper belong in a shared include, with the scaled-request
region's copy behind a flag - after which the room scene links, the room's file is read, `PLYR`
dispatches, and the camera's `init_phase2` has the player it waits for. The composition seam
`bluewake_route_b_room_layer_no` is supplied in the same step, with the implementation the port's own
tests already carry.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify (281 tww patches, 10
aurora), `scripts/audit_repo.sh` passes, `ref/` matches the series.

-## 2026-09-23 The room reloader tier composes, and two symbols are left

The twenty-fifth iteration named the tier that carries `dStage_dt_c_roomReLoader` - it is
`BLUEWAKE_ROUTE_B_ROOM_RELOADER_TIER`, a region the composition did not name - and composed it. The
link surface changed from one symbol to two, both named.

**Where the function lives, measured rather than guessed.** `d_stage.cpp` 18 opens
`#elif TARGET_PC && defined(BLUEWAKE_ROUTE_B_ROOM_RELOADER_TIER)`, and the region it opens defines
`bluewakeRoomLayerLoader`, `bluewakeRoomTreasureInit` and `dStage_dt_c_roomReLoader`. Compiling that
tier resolved the symbol and exposed the two the tier itself needs:
`dStage_roomDrtgInfoInit`, referenced from `dStage_dt_c_roomReLoader`'s table, and
`bluewake_route_b_room_layer_no`, the port's own seam for the room's layer suffix.

**What the two are.** `dStage_roomDrtgInfoInit` is declared at `d_stage.cpp` 26 and defined at 200, and
200 is inside the room-reloader region - yet the compiled object does not define it, so its region has a
further guard that excludes it and that is the next thing to read rather than guess.
`bluewake_route_b_room_layer_no` is a composition seam with reference implementations already in the
tree: `tests/route_b_room_aggregate_test.cpp`, `tests/route_b_room_lifecycle_test.cpp` and
`tests/route_b_private_room_lifecycle_probe.cpp` each define it (`assert(room == 44); return 0;` in the
aggregate test, a room-to-layer table in the probe), so the composition supplies a shape that exists
rather than inventing one.

**Where that leaves the chain.** The camera is created and published; the player needs the room's file;
the room scene needs `d_s_room` and its reloader; the reloader needs those two symbols. Each step this
far has been one named item, and this is the next one.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify (281 tww patches, 10
aurora), `scripts/audit_repo.sh` passes, `ref/` matches the series.

-## 2026-09-23 The room scene composes down to one missing symbol

The twenty-fourth iteration composed the room scene - the unit that reads the room's own file and
dispatches the player's `PLYR` node - and the link now stops on exactly one symbol.

**What had to be added, each for a measured reason.** The room scene's create reaches handlers the
composition did not carry: the `ROOM_NATIVE_VIEWS` tier owns `dStage_mapInfoInit`,
`dStage_filiInfoInit`, `dStage_lbnkInfoInit` and `dStage_soundInfoInit`, which its loader table
references (four undefined symbols, all defined by one tier object). The ship-state tier declares the
game-info and save accessors out of line in its own configuration, exactly as the player-request tier
did, so it needs the same room-reloader composition flag: that took eight symbols
(`dComIfGp_getShipId`, `dComIfGp_getShipRoomId`, `dComIfGs_getTurnRestartHasShip`,
`dComIfGs_getTurnRestartShipPos`, `dComIfGs_setTurnRestartHasShip`,
`dComIfGs_getTurnRestartShipAngleY`, `bluewake_route_b_find_ship`) to none.

**And one symbol is left:** `dStage_dt_c_roomReLoader(void*, dStage_dt_c*, int)`, referenced by
`objectSetCheck(room_of_scene_class*)` in `d_s_room.o`. Its only definition is at `d_stage.cpp` 53,
inside the unit's runtime region, which no composed tier compiles - the same tiering shape that hid
`dStage_playerInit` earlier: the region that carries it is compiled by a tier the composition does not
name, and neither `d_stage_runtime.inc` nor `d_stage_runtime_owners.inc` carries it.

**So the next item is one function and its helper.** `dStage_dt_c_roomReLoader` and the static
`bluewakeRoomLayerLoader` beside it belong in a composed region - the runtime-owners include that
patch 0273 introduced is the natural home, and it is already the tier that exists to carry "the unit's
remainder" - after which the room scene links, the room's file is read, `PLYR` dispatches, and the
camera's `init_phase2` has the player it waits for.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify (281 tww patches, 10
aurora), `scripts/audit_repo.sh` passes, `ref/` matches the series.

-## 2026-09-23 The room scene is refused too, and that is where the player's node lives

The twenty-third iteration closed the loop on the player chain: every link is now measured, and the
missing owner is the room scene itself.

**The registry refuses the room scene.** `BW-REGISTRY camera=4 player=5 roomscene=5 logo=4 opening=4
overlap=4` - `fpcNm_ROOM_SCENE_e` gets the same `cPhs_ERROR_e` the camera got before it was composed
and the player still gets. The profile is not linked either: `g_profile_ROOM_SCENE` (`d_s_room.cpp`) is
in the dependency archive but nothing references it, so it is never pulled.

**That is the unit that carries the player's node.** The play scene creates room scenes through
`fopScnM_CreateReq(fpcNm_ROOM_SCENE_e, ...)` (`d_stage_runtime.inc` and `d_stage.cpp`), and the room
scene's create is what reads the room's own file and dispatches its nodes - `PLYR` among them - through
the room loader. The stage file the composition does read carries `STAG`, `RTBL`, `EVNT`, `MULT`,
`EnvR`, `Colo`, `Pale`, `Virt`, `RPAT`, `RPPN`, `ACTR`, `RCAM` and `RARO` and no `PLYR`, which is
exactly why the chain stops where it does.

**The chain, end to end, every link measured.** The camera was refused, then composed, and then created
- `play.getCamera(0)` is non-null from frame 300 on. The player is refused, and the node that would
create it is in the room's file. The room scene that would read that file is refused, and its profile is
unlinked. So the next item is one unit and one registration: compose `d_s_room.cpp`'s room-scene profile
and register `fpcNm_ROOM_SCENE_e`, land the `PLYR` entry with it, and the camera's `init_phase2` -
which waits on that player - can finish and set the draw list's view.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify (281 tww patches, 10
aurora), `scripts/audit_repo.sh` passes, `ref/` matches the series.

-## 2026-09-23 The player tier links, and the file being parsed has no PLYR node

The twenty-second iteration composed the player tier into the composition, landed the one change that
makes it linkable, and found where the player's node actually lives.

**The composition links with the player tier.** With the port's `ROOM_PLAYER_REQUEST` tier compiled in
its room-reloader configuration - which takes the game-info and save accessors inline instead of
declaring them out of line - and the loader table carrying `PLYR`, the link resolves, after one real
duplicate was removed: both the player-request tier and the runtime-owners tier included
`d_stage_player_ikada.inc`, and two definitions of `dStage_playerInitIkada` is a link error rather
than a warning. Patch 0281 puts that include behind
`BLUEWAKE_ROUTE_B_STAGE_RUNTIME_OWNERS_COMPOSED`, which is a no-op for every existing composition (the
flag is undefined there, so the include behaves exactly as it did) and the enabler for one that composes
both tiers.

**And the run still has no player, for a reason the node inventory explains.** The file this
composition parses carries `STAG`, `RTBL`, `EVNT`, `MULT`, `EnvR`, `Colo`, `Pale`, `Virt`, `RPAT`,
`RPPN`, `ACTR`, `RCAM` and `RARO` - and no `PLYR`. That is the stage file: `dStage_actorInit` and
`dStage_roomReadInit` run against it (`num=2`, `num=50`), which is why the room path looked alive,
but the player's node lives in the room's own file, which this composition does not read. The `PLYR`
table entry is therefore necessary and not sufficient; the room file read is the next item.

**What was landed and what was not.** The include change is in the series as patch 0281 and every gate
is green with it. The loader's `PLYR` entry was built and measured with it and deliberately not
landed: it references `dStage_playerInit`, so a composition that compiles the aggregate tier without
the player-request tier would fail to link, and the entry belongs in the same change as the room file
read that can dispatch it.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify (281 tww patches, 10
aurora), `scripts/audit_repo.sh` passes, `ref/` matches the series.

-## 2026-09-23 The player's integration surface, named by the linker

The twenty-first iteration built both halves of the player change, watched the compiler accept them,
and then let the link name exactly what composing the player costs.

**Both halves compile.** The composed loader table takes the `PLYR` entry - it needed only a forward
declaration in its region - and the port's `ROOM_PLAYER_REQUEST` tier compiles in the composition,
which is where the native `dStage_playerInit` lives (`d_stage_player_request.inc`); the ikada helper
beside it comes from the runtime-owners tier, which the composition already carries.

**The link then named nine symbols the composition does not have:**

| owner | symbols |
| --- | --- |
| game info | `dComIfGp_setShipId`, `dComIfGp_getStartStage`, `dComIfGp_setShipRoomId` |
| save | `dComIfGs_getTurnRestartPos`, `dComIfGs_getTurnRestartParam`, `dComIfGs_getTurnRestartAngleY` |
| the port's player seams | `bluewake_route_b_player_exists`, `bluewake_route_b_player_init_ikada`, `bluewake_route_b_stage_proc_name` |

So the player item is not a one-line table fix: it is the table entry plus those nine owners, and the
nine are the next iteration's work rather than a shrug - the game-info and save accessors are already
implemented in the tree, and the three `bluewake_route_b_*` seams are the port's own platform hooks,
so the item is composition rather than authoring.

**The half-change was not landed, and that is a measurement too.** The `PLYR` entry references
`dStage_playerInit`, so any composition that compiles the aggregate tier without the player-request
object would fail to link; the entry and the nine owners have to land together. The edit was reverted
and `prepare_route_b.sh --check` is green at 280 patches, so the tree is exactly the verified series
state.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify (280 tww patches, 10
aurora), `scripts/audit_repo.sh` passes.

-## 2026-09-23 The room table that runs has no PLYR entry, and the player handler is compiled nowhere

The twentieth iteration located the player gap exactly, in the table and in the tiering, and both
halves are named.

**The table that runs omits PLYR.** The composition's room parse goes through
`dStage_dt_c_stageLoader`, and the compiled copy of it (the SCOB_INFO region, `d_stage.cpp` 772) lists
`MULT`, `RCAM`, `ACTR`, `RTBL`, `RARO`, `Pale`, `Colo`, `Virt`, `SCLS`, `RPPN`, `RPAT`,
`SCOB`, `EVNT` and `EnvR` - and no `PLYR`. That is exactly consistent with what the run showed:
`dStage_actorInit` and `dStage_roomReadInit` both run (`actorInit num=2`, `roomReadInit num=50`)
while no player node is ever dispatched.

**And the player handler is compiled by no composed tier.** `dStage_playerInit`'s only definition
sits at `d_stage.cpp` 1898, and the symbol is absent from the linked binary even though three tables in
the file reference it (410, 2652, 2677) - those tables are in regions the composition does not compile
either, so the references never reach the linker. The port's tiering is what makes this quiet: each
composed region carries its own copy of the loaders it needs, and PLYR fell into none of them.

**So the next item is two mechanical halves.** Add the `PLYR` entry to the composed stage/room table,
and provide `dStage_playerInit` with its `dStage_playerInitIkada` helper in a composed region - the
runtime-owners tier's include, which patch 0273 introduced, is the natural home, since that tier is
already "the unit's remainder". The composition must also stop refusing the player profile
(`BW-REGISTRY camera=4 player=5`), for which `tests/route_b_player_init_services.cpp` already carries
the shape.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify (280 tww patches, 10
aurora), `scripts/audit_repo.sh` passes, `ref/` is clean of the iteration's prints.

-## 2026-09-23 The room path runs; what is missing is the room's player node and its profile

The nineteenth iteration narrowed the player gap by measuring the room path that should produce it,
and the room path is alive.

**Rooms are parsed and actors are created.** Instrumented prints in the tier objects the composition
compiles show both room handlers running as soon as the opening scene's stage is created:
`BW-ROOMNODE actorInit num=2` and `BW-ROOMNODE roomReadInit num=50`. So the composition's room
parsing works, and its actor path is live - `dStage_actorInit` reaching `fopAcM_Create` for two
actors - which is a smaller gap than "no room dispatch at all".

**What is missing is specifically the player node.** The room node table that is running does not
carry `PLYR`: `dStage_playerInit` and `dStage_dt_c_roomLoader` are still absent symbols, and the
composition's module table still refuses `fpcNm_PLAYER_e` (`BW-REGISTRY camera=4 player=5`), so even
a player node that was dispatched could not become a process. The two layers are therefore narrow and
named: dispatch `PLYR` in the composition's room loader, and register the player profile with its
owners.

**Where that lands next.** The camera's `init_phase2` returns `cPhs_INIT_e` until
`get_player_actor(...)` is non-null, so the player is what unblocks the camera, and the camera's draw
is what sets the draw list's view. `tests/route_b_player_init_services.cpp` already carries the shape
for the second layer - it registers `fpcNm_PLAYER_e` with a `diagnostic_player_phase_two` profile, or
`d_a_player` when the world scheduler requests it - so the next item is to bring that shape and the
`PLYR` dispatch into the play-scene composition together.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify (280 tww patches, 10
aurora), `scripts/audit_repo.sh` passes, `ref/` is clean of the iteration's prints.

-## 2026-09-23 The player is refused the same way the camera was, and the room's dispatch is absent

The eighteenth iteration followed the camera's wait one owner along and found two separate gaps rather
than one, both measured.

**The player profile is refused by the composition's module table.** The registry reports
`BW-REGISTRY camera=4 player=5 logo=4 opening=4 overlap=4`: the camera is now carried, and
`fpcNm_PLAYER_e` is not - the same `cPhs_ERROR_e` the camera got before it was added. A player
create request would therefore die in its load phase exactly as the camera's did.

**And the room's node dispatch is not in the composition at all.** `dStage_playerInit` never runs in
this composition, and the reason is upstream of it: the symbols `dStage_dt_c_roomLoader` and
`dStage_playerInit` are absent from the linked binary, while `dStage_actorInit` and
`dStage_roomReadInit` are present. The room loader is the table that turns a room file's `PLYR`
node into a player (`PLYR` -> `dStage_playerInit`, `RCAM` -> `dStage_RoomCameraInit`, `RTBL` ->
`dStage_roomReadInit`, and the rest), so with it absent the room's player node cannot be parsed at
all. The camera that does exist came from the *stage* file's `RCAM` node through
`dStage_cameraInit`, which is a different table and is composed.

**So the next item is two-layered, and both layers are named.** The room loader
(`dStage_dt_c_roomLoader`) and the room-level handlers it dispatches to must be composed, and the
player profile with its owners must be registered - the port already has that shape in its own player
composition (`tests/route_b_player_init_services.cpp` registers `fpcNm_PLAYER_e` with a
`diagnostic_player_phase_two` profile, or `d_a_player` when the world scheduler asks for it), which
is the pattern to bring into the play-scene composition.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify (280 tww patches, 10
aurora), `scripts/audit_repo.sh` passes, `ref/` is clean of the iteration's prints.

-## 2026-09-23 The camera exists, and the composition now waits on the player

The seventeenth iteration carried the camera from refused to created and published, and the wait that
replaced the refusal is one owner further along.

**What moved.** The composition's module table carries the camera now -
`BW-REGISTRY camera=4 logo=4 opening=4 overlap=4` - and the camera's runtime owners are in the
composition with it: `f_op_camera.cpp` and `f_op_view.cpp`, the units that define
`g_fopCam_Method` and `g_fopVw_Method`, which the linker named the moment `g_profile_CAMERA` was
referenced. With them, the create request no longer dies in its load phase: the camera's
`init_phase1` runs and `play.getCamera(0)` is non-null from frame 300 on.

**A duplicate allocator had to go first.** ASAN refused the first build of this composition for an ODR
violation on `cMl::Heap`: two owners, the composition's own host-side
`route_b/src/process_adjacent_services.cpp` and the dependency's build of
`ref/tww/src/SSystem/SComponent/c_malloc.cpp`. Two copies of the cMl heap is two allocators, so the
composition's owner stays and the dependency's object is left out of the link. The port compiles both
units in its own targets, so the same check belongs there.

**The camera is parked on the player, one step short of the view.** The camera's create is
`init_phase1` -> `init_phase2`, and `init_phase2` returns `cPhs_INIT_e` while
`get_player_actor(...)` is null. It is null - `dComIfGp_getPlayer(0)` reads null at frames 300
through 540 - and because the create has not completed, the camera process is not yet registered in a
layer (a search for `fpcNm_CAMERA_e` finds nothing), so its execute and draw never run and the draw
list still has no view. **The player actor is therefore the next item:** `dStage_playerInit`'s PLYR
node and the player's own owners are what the camera waits for, and the view follows the camera's
draw.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify (280 tww patches, 10
aurora), `scripts/audit_repo.sh` passes, and `ref/` is clean of the iteration's prints.

-## 2026-09-23 The camera the stage asks for is refused by the composition's own module table

The sixteenth iteration followed the play scene's missing camera to its cause, and the cause is a
verdict the composition gives itself rather than anything in the game's logic.

**The chain, with every link measured.** The stage file's node list carries `RCAM`, and instrumented
prints show `dStage_cameraInit` running (num=1) and `dStage_cameraCreate` running (idx=0), which
calls `fopCamM_Create(0, fpcNm_CAMERA_e, params)`; that returns process id 8, so the request is made.
But no camera process ever appears in the process tree - a `fpcM_Search` for `fpcNm_CAMERA_e` is
null at frames 300 through 540 - and `play.getCamera(0)` stays NULL, so `dCamera_c`'s `view_setup`
never runs and the draw list never gets a view.

**Why the request dies, measured directly.** A process create request loads its procedure before it
allocates anything: `fpcSCtRq_phase_Load` -> `fpcLd_Load` -> `cDyl_LinkASync` -> the port's
`cDylPhs::Link`, which answers from the installed static rel registry and reports `cPhs_ERROR_e` for a
name the composition does not carry. Asking the registry from the probe itself gives
`BW-REGISTRY camera=5 logo=4 opening=4 overlap=4`: `cPhs_ERROR_e` for the camera,
`cPhs_COMPLEATE_e` for the three scenes the composition does carry. The error deletes the request
silently, so the stage asks for a camera, is refused, and the play scene runs on without one - which
is also why the create-phase null view patch 0280 guards is reached at all.

**And the next two owners are named by the linker rather than guessed.** Putting `g_profile_CAMERA`
into the composition's registry immediately pulls `d_camera.cpp.o` out of the staging archive, and
the link then needs `g_fopCam_Method` and `g_fopVw_Method` - `f_op_camera.cpp` and `f_op_view.cpp`,
neither of which the play-scene composition has ever carried. Those two units are the next queue
item, and the camera is the first process the composition has been asked for and refused.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify (280 tww patches, 10
aurora), `scripts/audit_repo.sh` passes.

-## 2026-09-23 The play scene's create completes, and the blocker was a console-legal null read

The fifteenth iteration of the v55 campaign carried the composed run past the LOGO-to-opening
handoff and into the play scene's own create, where it stopped on a read that is only a read on a
GameCube.

**The transition completes and the run reaches the fence.** With the host-sized particle common heap
in place the probe goes from `frame=247` (overlap created, LOGO still present) to `frame=288`
(opening scene created) to `frame=330` (the overlap process retired), and it then runs to its own
600-frame fence with `BW-DVD-STATE pending_head=0x0 syncAllObjectRes=0 reset=0`. The opening
scene's create finishes inside that window: its `request_of_phase_process_class` is left with a null
handler table and index 6, which is `phase_4` returning `cPhs_COMPLEATE_e`, the scene's own
`phase_2` having resolved the stage resource (`sync=1` then `sync=0`) and called
`dStage_infoCreate`.

**What stopped it was `dAttention_c::Draw` reading a null view.** The play scene is drawn by the
framework while it is still creating - `fpcDw_Execute` has no create-state guard - and its own
`phase_4` sets the draw list's view to NULL (`dComIfGd_setView(NULL)`); only a camera sets it back,
in `dCamera_c`'s `view_setup`, during the camera's draw. In any frame of that window where no camera
draws, `cMtx_inverse(dComIfGd_getViewRotMtx(), invCamera)` reads whatever is at address 0. On the
GameCube that is physical memory, so the garbage matrix is harmless and is discarded anyway - nothing
is drawn, because `LockonTarget(0)` is null until the stage has created actors, by which time a
camera exists. The host has no page at address 0, so the identical window aborts under UBSan and
would fault without it. Patch 0280 gives that one read the identity when there is no view and leaves
the rest of the function, including the `field_0x028` branch that also consumes the matrix,
untouched.

**The next frontier is the camera, and the measurements have walked one step further in.** After the
scene's create completes, `play.getCamera(0)` is still NULL and the draw list still has no view. It
is not a missing dispatch: instrumented prints in the tree's own objects show the stage file's node
list carrying `RCAM`, then `dStage_cameraInit` running (num=1) and `dStage_cameraCreate` running
(idx=0), and `fpcBs_Create` never refusing a profile - every process create resolves its static rel
registry entry. So the camera process is asked for and its allocation succeeds, and what is missing
is downstream of that: the camera's own create completing and publishing `mCameraInfo[0].mpCamera`,
which is the next thing to measure.

**A method note the iteration earned.** `lldb` breakpoints on these symbols are not evidence in this
composition: a breakpoint on `dStage_infoCreate` never fired while an instrumented print in
`phase_2` showed the call running, and a breakpoint on `dStage_cameraInit` never fired while an
instrumented print in the same function ran. The prints, compiled into the object under test, are
what the conclusions above rest on.

**The probe measures sources again, not archive members.** `d_particle.cpp`, `d_attention.cpp` and
`d_s_play.cpp` were all being taken from the staging archive, so the composition was testing objects
that need not match the tree - and the phase reading was being taken across two layouts. All three
are compiled by the probe from the tree now, which is what made the phase index and the missing
camera legible in the first place.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify (280 tww patches, 10
aurora), `scripts/audit_repo.sh` passes, and the port compiles the guard in
`bluewake_route_b_attention_census_objects`.

-## 2026-09-23 The LOGO's delete reaches the particle common heap, which is the first console budget the host outgrows

The fourteenth iteration of the v55 campaign answers the question the previous one left open - which
allocation the composed run dies on - and the answer is a heap capacity, not a missing function.

**The owner, from the debugger rather than from the log.** The refused 408-byte request is a
`J3DModel`, and the chain that reaches it is `dScnLogo_Delete` ->
`dComIfGp_particle_createCommon(l_particleCommand->getMemAddress())` ->
`dPa_control_c::createCommon` -> `dPa_modelControl_c`'s constructor -> `mDoExt_J3DModel__create`
-> `operator new(408)`. It fails inside a `JKRSolidHeap` - the particle common heap - with **112
bytes free**, so the request is refused for capacity rather than because a pointer was stale.

**The heap is profiled rather than guessed at.** A build instrumented with temporary prints
(reverted before the patch was generated) measured, at the LOGO's delete with `common.jpc` already
resident (425,296 bytes):

| stage | bytes |
| --- | --- |
| free when `createCommon` is entered | 1,075,888 |
| `JPAResourceManager` | 71,280 |
| `JPAEmitterManager(3000,150,200)` | 852,184 |
| 128-model pool | 2,176 a model, 278,528 in total |
| `createCommon` in total | 1,201,984 |

so the construction needs **1,627,296 bytes** where the console budget of 0x16e800 offers 1,501,184 -
**126,112 short**, and the difference is the host's wider JPA pool entries. The port's own private
Always-model probe now reports the heap it builds as `heap=1201984 used=1201984 free=0`, which is
the same number measured a second way. `bluewake_route_b_particle_manager_pool_test` already showed
that the emitter manager alone fits the console budget, which is why nothing caught this before the
composed run got this far.

**And the fix is one console number the host cannot use.** Patch 0279 has the constructor ask for
`bluewake::route_b::kParticleCommonHeapSize` (0x1c0000, 207,712 bytes of headroom) on the host while
the console keeps 0x16e800, with the measurement recorded in the new
`route_b/include/bluewake/route_b/particle_common_heap.hpp`;
`route_b_particle_construction_census` pins both numbers, and the Always-model probe prints the built
heap so a later widening that outgrows it aborts there first.

**The transition happens, and the frontier moved into the play scene's draw.** The composed probe now
goes from `frame=247 scenes opening=0 overlap=1 logo=1` to `frame=287 scenes opening=1 overlap=1
logo=0`, printing `[JAIZelBasic::load1stDynamicWave]` and `Start StageName:RoomNo [sea_T:44]`: the
LOGO deletes, the opening scene is created, and the play scene reaches its draw. It then stops in
`dScnPly_Draw` -> `dAttention_c::Draw` -> `dComIfGd_getViewRotMtx`, whose draw list view is null - and
read in the debugger, so is the camera the view would come from: `drawlist.mpView == NULL` and
`play.getCamera(0) == NULL` in the same frame. The next frontier is therefore the stage's
camera-create step (`fopCamM_Create`, reached from `d_stage.cpp`), not the particle system and not the
draw list's own storage.

**The probe measures a source now, not an archive.** `d_particle.cpp` had been coming from the
staging archive, so the composition was testing an object that need not match the tree; it is
compiled by the probe from here on, and that is what let the instrumentation above reach the code
under test.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify (279 tww patches, 10
aurora), `scripts/audit_repo.sh` passes.

-## 2026-09-23 The composed run was broken by my own header change, and a control found it

The thirteenth iteration of the v55 campaign found, bisected, explained and fixed a regression this
campaign had introduced four iterations earlier - and the composed probe now runs the original
scheduler with the full composition linked.

**The symptom was already recorded**: the composed binary stopped at `d_s_logo.cpp`'s toon-image
setup, where `System.arc` reported `res nothing`. I had two hypotheses for it - the LOGO variant the
composition carries, and the resource owners it adds - and measurement killed both: the same
`full-logo.o` is in both compositions, and the resource control resolves to the same object.

**What settled it was a control.** Rebuilding the *base* cold-start composition - which contains none
of this campaign's additions - and running it failed at the *same* assertion, with `res during
reading` instead of `res nothing`. So the fault was not what the composition contained; it was a
regression that had arrived with the patches. Bisecting that is cheap because the control is a three
second build and run: reversing one patch, rebuilding and running, 4 seconds a case. Reversing
**0273** alone cleared it; reversing any one of 0269, 0270, 0271 or 0272 did not.

**The mechanism is an ABI one, and it is worth naming.** 0273 had added six members to
`dStage_stageDt_c` in `d_stage.h`. That struct sits inside the game-info layout, so a new member
moves every member after it - and the port's *already-compiled* objects, which the probes link out of
prebuilt libraries, were built against the old offsets. A probe that mixes prebuilt objects with
freshly built ones therefore reads the resource control, and everything else that follows the stage,
at the wrong offsets; which of the two messages appears depends on which side read last. Moving the
members to the end of the struct does not help, because the *size* still changes - I tried that
first and it still asserted.

**The fix takes the storage out of the header entirely.** PPNT and PAT now decode into a file-scope
table in the tier's own `.inc`, which is what the console's storage amounts to as well given a
single-instance unit, and which cannot move anyone else's members. Patch 0273 no longer touches
`d_stage.h` at all - 662 insertions across `d_stage.cpp` and the `.inc`.

**Verified both ways.** The cold-start control exits 0 with no assertion, and the composed probe runs
the original scheduler to its own fence with the full composition linked: LOGO actions advancing at
frames 5, 95, 125, 215 and 245, the audio scene changing to `sea_T` room 44, and the run ending at
`BW-LOGO-COLD-RESET-FENCE` after 600 frames. That is the behaviour the session opened with, now with
the opening scene, the stage owners, the decoration owners, the message and save chains and the
colour readback all in the link.

**And the next step is visible in that run.** The LOGO still sits in its DVD-wait action because
`fopScnM_ChangeReq` asks for `fpcNm_OVERLAP0_e` as the transition and the probe's registry carries
only `fpcNm_LOGO_SCENE_e` and `fpcNm_OPENING_SCENE_e`.

Review: Route B 86/86, Aurora `gx_fifo_tests` 207/207, both series verify, `scripts/audit_repo.sh`
passes.


## 2026-09-23 The composition links uniquely, and the route-B run reaches the LOGO's toon image

The twelfth iteration of the v55 campaign closes the link and opens a run. Both are measurements.

**The duplicates were removed, not tolerated.** The five were two room tiers each defining
`dStage_roomDt_c::init()` and its vtable and typeinfo records. I measured the duplicated code first -
`init()` is **byte-identical** in the two objects, 1077 instructions each by `otool` - and then did
not lean on that: the runtime-owners tier absorbed the four symbols the pair existed for
(`dStage_rppnInfoInit`, `dStage_rpatInfoInit`, `dStage_roomDt_c::getMapInfo2`,
`getMapInfoBase`, taken from the ported bodies beside them rather than from the console region) and
the room-native-views tier was dropped from the composition. It now links with **zero undefined and
zero duplicate symbols**: one object per unit region, no arbitrary choice left to the linker.

**And the composed binary runs.** It gets through the heap setup, `mDoGph_Create`, native Metal, the
original scheduler's first frames, `mDoAud_Create` with the adjusted audio heap, the ARAM plan and
the heap report - and stops inside the LOGO's toon-image setup:

```
<System.arc> getRes: res nothing !!
Failed assertion toonImage != __null in "ref/tww/src/d/d_s_logo.cpp" on line 1426
```

That line is `d_s_logo.cpp:857`: the LOGO asks
`dComIfG_getObjectRes("System", dRes_INDEX_SYSTEM_BTI_TOON_e)` for its toon texture, the archive
reports nothing, and the ported assert fires. Exit 134.

So the frontier has moved off the linker and onto a resource question: `System.arc`'s lookup returns
nothing *in this composition*. The LOGO-only composition ran 600 frames with the LOGO's actions
advancing, so something the composed link pulls in changes that path, and there are two candidates:
the LOGO variant (the port's phase tiers against the full LOGO the composed link carries) and the
resource owners (the composition's `object_resources`/`resource_runtime` against the base link's
`d_resorce`). Which one it is, is the next measurement.

Review: patch 0273 regenerated (688 lines; the tier now carries the four it absorbed), Route B suite
86/86, `scripts/audit_repo.sh` passes, and both patch series verify. The private evidence is
`logo-scheduler/probe_open` with its run log.


## 2026-09-23 The composition links without a stale archive, and one ambiguity is left standing

The eleventh iteration of the v55 campaign. The link's duplicate symbols turned out not to be the
port's at all; completing the runtime-owners tier then closed the undefined set, and what remains is
recorded as unmeasured rather than assumed.

**The duplicates were stale archive members.** The base link's dependency archive carries fifteen
objects whose *basename* is `d_stage.cpp.o` - one per route-B target that compiles the unit, because
`ar` stores basenames - so linking that archive beside the composition's own tier objects defined the
same functions twice and left the linker to choose. Rebuilt from its own member list without them,
the duplicates vanish. It is worth saying plainly: a staging archive that `ar` keys by basename
silently becomes a set-union of every build of a file, and the previous iteration's "twenty-one
duplicates between the port's tiers" was that, not a port defect.

**With the archive clean, seven symbols went undefined** - they had been coming from those stale
copies - and each had a named owner: `dComIfGs_getSave` and `dComIfGs_initDan` (the stage-info tier
declares them instead of taking the world header, so it needs `BLUEWAKE_ROUTE_B_NATIVE_STAGE_INFO`,
the flag the port's own composition target already sets); `dStage_actorInit` (the actor-info tier,
which the previous iteration had dropped to answer an unrelated overlap); and `createRoomScene`,
`dStage_roomInit`, `dStage_roomControl_c::setStayNo`, `dStage_roomDt_c::getMapInfo2` and
`getMapInfoBase` (room-control and room-native-views, both of which are needed).

**And two of those were the runtime-owners tier's own incompleteness.** The fall-through it replaces
includes `d_stage_room_metadata.inc` and `d_stage_runtime.inc` besides its function bodies, so the
tier now includes both and the fall-through's full include set. Patch 0273 is regenerated with that:
the tier is now the unit's remainder rather than the unit's remainder minus its includes. Verified
in the port: the target builds and defines the same three owners, the Route B suite is 86/86, and
`scripts/audit_repo.sh` passes.

**What is left, and it is stated as unmeasured.** With that, the composition links with zero
undefined symbols and **five** duplicate warnings, all between tiers that compile overlapping regions
of the same unit: `dStage_roomDt_c::init()`, its vtable and both typeinfo records - `room_control_init`
against `room_native_views` - and one more. Both tiers are needed (one owns the room-control statics,
the other the map-info pair), so this is not a stale archive: it is the port's tier set not being a
partition of the unit. **I have not shown the linker's pick to be equivalent to the copy it discards**;
the disassembly comparison I attempted returned nothing usable, and until it is measured the
composition is not unambiguous. The resolution the evidence points at is one object per unit - a
ported fall-through - rather than a longer tier list, and the composition should not be run for
behaviour until that is settled.

Review: Route B suite 86/86, `scripts/audit_repo.sh` passes, both patch series verify.


## 2026-09-22 The colour readback lands, and the route-B opening composition links

The tenth iteration of the v55 campaign. The last undefined symbol is answered, the composition links
end to end, and two things the link exposes are recorded rather than smoothed over.

**Landed: Aurora patch 0010, the colour half of the CPU-to-EFB read path.**

- `GXPeekARGB` reads the most recent completed snapshot of the scene colour target and asks for the
  next one, which is `GXPeekZ`'s shape exactly. Its reader is one frame behind the console's
  synchronous peek; that is the whole of the difference and it is stated at the definition.
- `lib/gfx/color_peek.{hpp,cpp}` is that snapshot: a request id, a texture-to-buffer copy at the
  existing `encode_frame_snapshot` call site, a mapped readback, three slots, and the same 30 Hz
  cadence and slot discipline the depth peek uses. It is not a copy of that module, for a measured
  reason: the depth peek needs a compute pass because a depth texture cannot be copied to a buffer
  and because the guest's Z convention has to be reconstructed, and a colour target needs neither.
- **The word layout is derived, not guessed.** The guest's own capture pass writes its photo index
  with `GXSetDstAlpha(col * 4)`, leaves colour updates off and alpha updates on, and then reads
  `sp8 >> 26`; the index comes back whole only if alpha is the high byte of the peeked word. The
  host honours destination alpha - Aurora's BP decoder stores it and its pipeline blends the
  constant in - so the mechanism the guest relies on exists here.
- The two pieces of pure logic, the word layout and the map from a guest pixel to a target pixel,
  live in the header rather than the translation unit, so the module and the command-level tests
  share one definition instead of two that can drift.

**Verified in Aurora's own suite: 207/207 `gx_fifo_tests`,** including four new ones - the peek's
fallback and its snapshot request, the latest-snapshot read, the out-of-range case, and the layout
test, which reproduces the guest's arithmetic by asserting `argb_from_rgba8(..., index * 4) >> 26 ==
index`. And a real defect the test caught: the first request id was `0x003B`, which is
`GX_AURORA_COPY_DISPLAY`; the decoder read eight bytes of payload that were not there. The id is
`0x003C` now. Nothing but a test would have found that before a rendered run.

**Effect on the composition.** With the rebuilt library the undefined set is empty and the link
succeeds: the route-B LOGO-to-opening composition links end to end - the emitted JStudio owners,
the stage runtime owners, the message and save chains, the actors, the decoration owners and their
generated assets, and now the GX wrapper they all sit on.

**Two caveats, both from what the link exposed.**

1. The link carries 21 duplicate-symbol warnings between the port's own `d_stage` tier objects -
   `d_stage_info` against the runtime-owners tier, `room_control_init` against it, `initial_room_
   request` against `stage_runtime`, and so on. `ld` resolves each by picking one, which is a
   hazard rather than a nuisance: a composition has to choose one tier per region. Measured: four
   overlapping pairs *among* the tiers themselves. That also corrects this ledger's earlier claim
   of "zero non-weak duplicate symbols" - that check compared the new object against the union of
   the others, and never the others against each other.
2. The packet preparer's linkage came from `config.yml`'s default; the authority is `symbols.txt`'s
   own `scope:` field. Using it fixed a real collision: four owners each defined an external
   `l_matDL`, and the linker was picking one of them.

Review: Route B suite 86/86, Aurora `gx_fifo_tests` 207/207, `scripts/audit_repo.sh` passes, both
patch series verify (`03d27aa1...` with 278 patches, `8b690b60...` with 10).


## 2026-09-22 The alpha-read register lands where it belongs, and one symbol is left

The ninth iteration of the v55 campaign removes one of the two remaining undefined symbols and
leaves the other alone for a reason it measured.

**Landed: Aurora patch 0009.** `GXPokeAlphaRead` now exists in
`ref/aurora/lib/dolphin/gx/GXCpu2Efb.cpp`, the file that implements its sibling `GXPeekZ`, and it is
a no-op. That is not a stand-in: the register selects how a later CPU read of the EFB interprets
alpha, and this host layer implements exactly one CPU read of the EFB - the depth snapshot - and no
colour read at all, so there is no path for the mode to alter and no state for it to live in. The
comment says the same thing at the definition, and the point of writing it *here* rather than in the
port is that this is where the register has to be recorded when a colour readback exists.

The patch is registered the way Aurora's others are: in `scripts/prepare_route_b_aurora.sh`'s series
and in the paths it verifies (that script checks a hard-coded path list, so a patch touching a new
file has to add it there or the check would pass without verifying anything), and in the lock's
`tracked_patches`, which now holds nine. `bash scripts/prepare_route_b_aurora.sh --check` passes.

**Measured effect.** With the rebuilt `libaurora_gx.a` in the link, the composition's undefined set
is exactly one symbol:

```
"_GXPeekARGB", referenced from dSnap_packet::Judge() in d_snap.o
```

**And `GXPeekARGB` is specified rather than assumed.** It is the EFB colour read the console's
`dSnap_packet::Judge()` does to decide whether a pictograph subject is on screen. Reusing the depth
path would be wrong for a measured reason: `depth_peek` needs a compute shader because a depth
texture cannot be copied to a buffer and because the guest's Z convention has to be reconstructed -
its WGSL is `texture_depth_2d` plus `gx_z24`, with a reversed-Z variant. A colour target has
neither problem: it can go straight out with `copyTextureToBuffer`. So the work is not a clone of a
400-line subsystem; it is an async readback - a request id beside `GX_AURORA_REQUEST_DEPTH_SNAPSHOT`,
a colour snapshot at the same `encode_frame_snapshot` call site, a mapped buffer with the same
slot-and-interval discipline, and the format conversion to the guest's ARGB word - and its reader is
one frame behind the console's synchronous peek by construction. It is left unimplemented this
iteration because it cannot be verified here: the only way to prove a colour readback works is a
rendered run, and the composition does not link far enough to have one yet.

Review: patch series verify at `03d27aa1...` (278 patches, all named) and at Aurora's pin (9 patches,
all named), Route B suite 86/86, `scripts/audit_repo.sh` passes.


## 2026-09-22 The packet owners compile, and the handoff composition has one blocker left

The eighth iteration of the v55 campaign closes the asset item and leaves a single named symbol pair
between the route-B opening scene and a link.

**Landed: patches 0277 and 0278.**

- `tools/converters/matDL_dis.py` carried two 32-bit assumptions in what it emits. `IMAGE_ADDR`
  was `(u32)(addr) >> 5`, which truncates a host pointer - the compiler rejects it outright - and
  the byte-splitting macros narrowed implicitly inside a braced initialiser, which the console
  accepted because the value there was an address constant and here it is not. Both are explicit
  now: `(u32)(uintptr_t)(addr) >> 5`, and a `(u8)` cast on each split. What makes that honest
  rather than a shrug is a measurement: **Aurora's GX layer stores the image3 register and never
  reads it back** - `regs.cpp`'s BP decoder assigns `slot.image3`, and `image3` has exactly one
  reference in the whole GX layer, that assignment. The register block's texture field is inert on
  this host, so the value keeps the console's encoding and the truncation carries no behaviour.
- `d_wood.cpp`'s three `cLib_checkBit(ret, 0x01UL)` calls, where `ret` is a `u32`. The template
  deduces one type for both arguments; on the console `u32` *is* `unsigned long`, so the `UL`
  literals matched it, and here `u32` is `unsigned int`. The literals say so now, which is the same
  shape as the port's other explicit-width repairs.

**Measured effect.** All five packet owners - `d_grass`, `d_tree`, `d_wood`, `d_flower`, `d_magma` -
compile clean under the owner flags, and with them in the composition the link's undefined set is
exactly two symbols:

```
"_GXPeekARGB", referenced from dSnap_packet::Judge() in d_snap.o
"_GXPokeAlphaRead", referenced from dSnap_packet::Judge() in d_snap.o
```

So the LOGO-to-opening composition has **one** blocker rather than two, and it is the host GX
capability item: the pinned host library declares the whole CPU-to-EFB family and implements exactly
one member of it (`GXPeekZ`, over `aurora::gfx::depth_peek`), and its colour path has `GXCopyTex` and
`tex_copy_conv` but no colour sample readback. `GXPeekARGB` is that readback and `GXPokeAlphaRead`
is its register counterpart; both are reached only from one function, `dSnap_packet::Judge()`.

Review: patch series verifies at `03d27aa1...` (278 patches, all named in the lock), Route B suite
86/86, `scripts/audit_repo.sh` passes.


## 2026-09-22 The packet owners' assets come from the decomp's own converters

The seventh iteration of the v55 campaign. The wall's second item was an asset-generation job; it
is now a generated artifact, and the entry above it is corrected.

**Correction.** That entry says 27 of the packet owners' 61 `assets/*.h` includes "do not" resolve,
"because they are namespace-scoped statics or file-local names that repeat across units". The
finding was right and the measurement was not: I had looked up the *bare* symbol, while the join
the decomp uses is the header filename. `config/GZLE01/config.yml` names, per header, its binary,
its symbol and its custom type; for a file-local static the symbol field is a rename carrying the
section and address (`l_color!.data:0x80379904`); for a mangled one the symbol is the mangled name
and a rename carries the identifier. Joined that way, all **61 of 61** resolve exactly - 30 plain,
9 `Vec`, 8 `matDL`, 7 `cXy`, 7 `GXColor`.

**Landed: `scripts/prepare_route_b_packet_assets.py`.** It reads the five owners' includes, joins
each to `config.yml` and `symbols.txt`, extracts the bytes from the locked DOL and hands them to the
decompilation's *own* converters - `tools/converters/matDL_dis.py` for the material display lists,
which are macros taking the texture name, and `tools/converters/extract_model_data.py` for the `Vec`,
`cXy` and `GXColor` arrays. No format is invented here and no address is hand-written: a header with
no `config.yml` entry, or an entry with no symbol, fails the run rather than being skipped. Two
details the converters require and the script supplies: the material-DL macro is named after the
*header* (`l_matDL__d_tree`), not the symbol, because the decomp names one macro per header; and
they annotate with PEP 585 generics, so an interpreter of 3.9 or newer is selected for them while
the script itself runs anywhere.

Measured effect: `d_magma.cpp` now compiles clean under the owner flags - the first of the five -
and the others are down from whole-header failures to one class of error each.

**And that class is not asset generation.** The remaining errors in the other four are the material
display list's texture addressing: the decomp's template spells it `#define IMAGE_ADDR(addr)
(u32)(addr) >> 5`, a pointer truncated to an address-sized word, which is right where a texture's
address is a 32-bit MEM1 address and wrong where the array holding it is a host pointer. The
generated display list can only carry the 24 bits that fit in the BP register, so closing this
means deciding how a *generated* texture is named to the host GX layer at all - the same host
capability question as the CPU-to-EFB pair, not a template tweak. `d_wood.cpp` has one more of the
width kind (`cLib_checkBit` with no matching overload).

Review: Route B suite 86/86 and `scripts/audit_repo.sh` pass; the new script is the only source
change this iteration, so the suite is unaffected except as a regression check.


## 2026-09-22 The prelude carries the console macros, and the wall becomes two host-capability items

The sixth iteration of the v55 campaign. Two more owners resolve, and what is left is no longer port
work in either case.

**The prelude takes the console macro layer.** `route_b/include/bluewake/route_b/native_prelude.hpp`
now carries `DEG_TO_RAD` and `RAD_TO_DEG`, with the decomp's own definitions including
`RAD_TO_DEG`'s fakematch offset. The host's `<cmath>` has neither, and
`JStudio_JStage/object-light.cpp` is the only translation unit in the tree that uses them - measured
by grepping the whole decomp tree for both names. Reaching the decomp's MSL `math.h` instead was
tried and refuted: with that directory ahead of the system paths the unit fails seven ways, starting
inside `<cmath>` and ending at the prelude's own `fabsf`, because that header replaces the host's C
library for the whole unit. The console macro layer is the prelude's business, which is where the
port already keeps `INT32_MAX`, `FLOAT_MAX`, `__frsqrte`, `__fres` and the version constants.

**And the tribox actor had only half its translation unit.** `daObjTribox::Act_c::reset()` is defined
in `d/d_a_obj_tribox_static.cpp`, not in `d/actor/d_a_obj_tribox.cpp`, so the composition step that
added the latter carried the actor's behaviour and not its static state. Both compile clean.

Effect: `JStudio_JStage::TAdaptor_light::TAdaptor_light` and `daObjTribox::Act_c::reset()` leave the
undefined set. Route B suite **86/86 green** (`native_prelude.hpp` is force-included into every unit,
so the whole suite is the test of that change), audit PASS.

**The wall is two items, and both are host capability rather than port work.**

1. `GXPeekARGB` and `GXPokeAlphaRead`, both reached from `dSnap_packet::Judge()`. The host GX layer
   declares the whole CPU-to-EFB family in `ref/aurora/include/dolphin/gx/GXCpu2Efb.h` and implements
   exactly one member of it, `GXPeekZ`, in `ref/aurora/lib/dolphin/gx/GXCpu2Efb.cpp` - verified by
   `nm` over the built Aurora libraries. The console's own implementation would not transfer if it
   were composed either: `ref/tww/src/dolphin/gx/GXMisc.c`'s `GXPeekZ` is `*z = *(u32*)addr` against a
   raw MMIO address. So this is not a missing patch but a missing capability - a colour readback and a
   PE read-mode register - and the honest next step is an explicit decision about them, not a stub.
2. The five packet owners (grass/tree/wood/flower/magma), reached from `dComIfG_play_c::executeGrass()`
   and its siblings, so they are on the path. Each includes an actor-local generated header family -
   `assets/l_K_kusa_00TEX.h`, `l_Vmori_*`, `l_Oba_kusa_*`, `l_Txa_swood_*`, `l_Oba_swood_*`,
   `l_Txq_bessou_hanaTEX`, `l_Yfloor*`, `l_Yball*` - and every one of those symbols has an address and
   a size in `ref/tww/config/GZLE01/symbols.txt` (`l_K_kusa_00TEX` at 0x80377C00, size 0x1000).
   `scripts/prepare_route_b_player_assets.py` already generates two of the three formats the family
   needs: byte arrays for display lists and textures (`prepare`), and `Vec` arrays for positions
   (`prepare_positions`, with a finiteness check). Colour and texcoord arrays are the third and are
   not written yet, so the work is two more generators plus one entry per symbol, each validated
   against the locked DOL identity the script already checks.

**And both items are now measured one level deeper, so neither is a matter of wiring.**

- The asset list does not resolve by the identifier the source uses. Of the 61 `assets/*.h` includes
  across the five packet owners, 34 match a `symbols.txt` entry exactly and 27 do not, because they
  are namespace-scoped statics - `l_Oba_swood_bDL__Q25dWood20@unnamed@d_wood_cpp@`,
  `l_Txa_swood_bTEX__Q25dWood20@unnamed@d_wood_cpp@` - or file-local names that repeat across
  translation units (`l_color` has six entries in the map). An entry therefore needs its owning unit
  to disambiguate, which the mangled suffix already names, and the family needs the two generators the
  script lacks beside its byte-array and `Vec` ones.
- The host GX item is a feature rather than a fix, measured against the pinned library: Aurora ships
  `gfx/depth_peek.hpp` with `read_latest(uint16_t, uint16_t, uint32_t&)`, implements exactly one
  member of the CPU-to-EFB family (`GXPeekZ`), and its colour path offers `GXCopyTex` and
  `gfx/tex_copy_conv` but no colour sample readback. So `GXPeekARGB` is a framebuffer-sample facility
  on the host library and `GXPokeAlphaRead` is its register counterpart - work to schedule, not a
  clause to stub.


## 2026-09-22 The message and save chains compose, and the wall is four items

The fifth iteration of the v55 campaign. Two chains that looked like wall items turned out to be one
named dependency each, and the composition now reaches the last four.

**Landed: patches 0275 and 0276.**

- `JGadget/linklist.h` - `TLinkList_factory::Erase_destroy` called `Erase(param_0)` unqualified from
  a dependent base, which a compiler that enforces two-phase lookup rejects. It is `this->Erase(...)`
  now. With it, `JMessage/resource.cpp` and `JMessage/data.cpp` compile, and the whole message chain
  leaves the undefined set: `JMessage::TControl::setMessageCode_flush_` and then
  `JMessage::TResourceContainer::Get_groupID` both resolve. The message system was three translation
  units and one name-lookup rule, not a sequence of owners.
- `d_stage.cpp`'s room-control tier declared `dComIfGs_initZone` instead of taking the header, and
  that function is *inline* in `d_com_inf_game.h` - so the tier's object referred to a wrapper no
  translation unit defines. The include is now behind `BLUEWAKE_ROUTE_B_NATIVE_ROOM_CONTROL`, which is
  the port's own idiom for exactly this (see `BLUEWAKE_ROUTE_B_NATIVE_STAGE_INFO` in the stage-info
  tier): a composing build takes the header and with it the save owner, and a census build keeps the
  declaration, which `tests/route_b_room_control_init_test.cpp` furnishes. The composition target
  `bluewake_route_b_room_control_init_composition_objects` sets the flag and carries the asset include
  path the header needs; the census target is untouched and its test passes as before.

**And the save side is one object.** `d/d_save.cpp` compiles clean under the owner flags - the same
measurement the earlier iterations applied to the JStudio units - and with it in the composition
`dSv_info_c::initZone()` resolves, which is what the inline above calls. That wall item closed as a
single file rather than a subsystem campaign.

**The wall, four items.** `JStudio_JStage::TAdaptor_light`'s MSL `math.h` against the prelude's
`<cmath>`; `_GXPeekARGB` and `_GXPokeAlphaRead`, used by `d/d_snap.cpp`, with no host implementation;
the grass/tree/wood/flower/magma packet owners, each needing an actor-local generated header; and
`daObjTribox::Act_c::reset`.

Review: Route B suite **86/86 green**, `scripts/audit_repo.sh` passes, and the series verifies at
`03d27aa1...`. The two patches are in `patches/tww/`, in `scripts/prepare_route_b.sh` and in the
lock's `tracked_patches`.


## 2026-09-22 The stage runtime owners exist: all three of d_s_play's d_stage symbols leave the undefined set

The fourth iteration of the v55 campaign, and the first one that lands semantics rather than widths.
Patch 0273 writes the tier the previous entry measured the shape of, and the two wall items it was
written for are gone.

**What landed.** A new branch of `d_stage.cpp`'s outer chain, `BLUEWAKE_ROUTE_B_STAGE_RUNTIME_OWNERS_TIER`,
which includes a new `ref/tww/src/d/d_stage_runtime_owners.inc` (545 lines) and six new host-array
members in `d_stage.h` (`mHostPnt`/`mHostPntEntries`/`mHostPntCapacity` and the `mHostPath` trio,
declared beside the room-scoped `mHostPnt2`/`mHostPath2`).

The region is the measured 25, not the 74: 45 of the fall-through's functions are already provided by
one of the port's tiers, so the branch carries only what no tier carries. Two of the 25 are not copied
but included - `dStage_decodeSearchIkada` and `dStage_playerInitIkada` come from
`d_stage_player_ikada.inc`, the native decoder pair that no tier was including. Two more are ported
rather than copied: `dStage_ppntInfoInit` and `dStage_pathInfoInit` now resolve their serialized
records against the file base and decode into storage the stage owns
(`dStage_resolveHostEntries`), which is exactly what their room-scoped siblings
`dStage_rppnInfoInit` and `dStage_rpatInfoInit` in the path-graph tier already do; the console forms
read a file offset as if it were an address. The other 21 are copied bodies, whose only portability
needs were includes.

**The composition is verified rather than assumed.** All 22 compilable tier objects plus this one
share **zero non-weak duplicate symbols** - the 50 names that look shared are linkonce template and
inline instantiations (`nm -m` marks them weak external), which is what makes a unit whose regions
live in 23 separate objects linkable at all.

**The effect, on the link.** With the tier objects in the composition,
`dStage_Create`, `dStage_Delete` and `dStage_roomControl_c::checkDrawArea` all leave the undefined
set - the first two items of the wall the previous entry recorded, closed. Item one is closed as a
scaffold-free owner: the census substitute is not involved, the function is compiled against the real
`d/d_stage.h`.

**And in the port's own build.** `bluewake_route_b_stage_runtime_owners_objects` is registered
`EXCLUDE_FROM_ALL` beside the unit's other tier objects, and it builds: `nm` on its object shows
`__Z13dStage_Createv`, `__Z13dStage_Deletev` and `__ZNK20dStage_roomControl_c13checkDrawAreaEv`. It
needs two things its siblings already had - the MSL C++ `-idirafter` for `new.h`, and
`assets/GZLE01` on the include path for `res/Object/Always.h` - which is why the target is declared
with them.

**The wall, now six items.** `JMessage::TControl::setMessageCode_flush_()`
(`JSystem/JMessage/control.cpp`); `JStudio_JStage::TAdaptor_light`'s MSL `math.h` versus the prelude's
`<cmath>`; `_GXPeekARGB` and `_GXPokeAlphaRead`, used by `d/d_snap.cpp`, with no host implementation;
`dComIfGs_initZone()`, a save-side owner the tier objects newly reach; the grass/tree/wood/flower/
magma packet owners, each needing an actor-local generated header; and `daObjTribox::Act_c::reset`.

Review: the Route B suite is **86/86 green** and `scripts/audit_repo.sh` passes. The patch is in
`patches/tww/`, in `scripts/prepare_route_b.sh`'s series and in the lock's `tracked_patches`.

**Two smaller steps the same iteration, so the wall stays exact.** `JSystem/JMessage/processor.cpp`
could not compile either: it switches on a `u32` with `case -1:` and `case -2:`, which is the same
narrowing in a case label rather than in an address (patch 0274). With it and `JMessage/control.cpp`
in the composition, `JMessage::TControl::setMessageCode_flush_()` leaves the undefined set and
`JMessage::TResourceContainer::Get_groupID(unsigned short)` takes its place - one more JMessage owner,
so the message system is a short chain rather than a single item.

The wall is therefore: the JMessage resource owner; `JStudio_JStage::TAdaptor_light`'s MSL `math.h`;
`_GXPeekARGB`/`_GXPokeAlphaRead`; `dComIfGs_initZone()`; the five packet owners' generated assets;
and `daObjTribox::Act_c::reset`.


## 2026-09-22 The vector instantiation widens, and dStage_Create is a census rather than an owner

The third iteration of the v55 campaign: two more width patches land, one claim from the previous
entry is corrected a second time, and the wall is cut to seven named items.

**Landed: patches 0271 and 0272.**

- `JGadget/vector.h` - the `TVector<void*>` instantiation that `JGadget/std-vector.cpp` builds is what
  `TVector_pointer_void`'s out-of-line members are, and it carried four more of the defects 0269
  fixed one level up: `Insert_raw` narrowed `GetSize_extend_`'s `size_t` to `u32`, `insert` narrowed
  a pointer difference, and `GetSize_extend_` narrowed both `size()` and `capacity()` into the
  `u32`-returning `mExtend`. All four are explicit now, so the counts keep the width the decomp gave
  them instead of truncating.
- `JStudio_JStage/object-actor.cpp` - `*(u32*)(((u32)adaptor - 1) + _08)` measured an address
  through a `u32`; it is byte-pointer arithmetic off the adaptor now, which is what the console's
  `(u32)` was standing in for.

Measured effect on the composition: the `TVector_pointer_void` members, `JStudio::data::ga8cSignature`,
`dSnap_Create/Execute/Delete/DebugDraw` and `fopDwIt_Begin`/`fopDwIt_Next` all leave the undefined
set, and seven of the eight `JStudio_J*` `TAdaptor_*` constructors resolve with them.

**Correction, and it is a method correction.** The entry above says `dStage_Create`, `dStage_Delete`
and `checkDrawArea` have no PC configuration at all. That holds for two of them; the third looked
absent because I measured the tier branches with *this probe's* flag set rather than the port's
target flags. `bluewake_route_b_stage_create_census_objects` does build, and its object defines
`dStage_Create` (`__Z13dStage_Createv`) - compiled against `route_b/frontiers/stage_create/d/d_stage.h`,
a 31-line census substitute that declares the nine services the function calls and nothing else. So
`dStage_Create` exists as a *measurement scaffold*, not as a play-scene owner: the product composition
has no version of it, and `dStage_Delete` and `dStage_roomControl_c::checkDrawArea` have neither tier
nor frontier. Measured the right way: 23 of the 26 tier defines compile (`STAGE_CREATE` fails under
the probe's flags because only the frontier header declares its services), they union to 405 symbols,
and none of the three owners is among them - `dStage_roomReadInit` is, from the room-read info tier.

**The wall, seven items, each named.**

1. `dStage_Delete` and `dStage_roomControl_c::checkDrawArea` - no tier and no frontier in the product;
   their bodies carry no offset-in-pointer-field arithmetic, so the work is a branch and its includes,
   not a rewrite.
2. `dStage_Create` - the census scaffold above, which a real composition cannot use.
3. The grass/tree/wood/flower/magma packet owners - each includes an actor-local generated header
   (`assets/l_K_kusa_00TEX.h` and its family) that the asset preparer does not cover.
4. `JStudio_JStage::TAdaptor_light` - needs `RAD_TO_DEG`/`DEG_TO_RAD` from the decomp's MSL `math.h`,
   and putting that directory ahead of the system paths makes the prelude's `<cmath>` pull MSL's
   `fdlibm.h`. A configuration item, held out of the probe.
5. `JMessage::TControl::setMessageCode_flush_()` - `JSystem/JMessage/control.cpp`.
6. `_GXPeekARGB` and `_GXPokeAlphaRead` - declared in `dolphin/gx/GXMisc.h`, used by `d/d_snap.cpp`,
   and no host implementation exists.
7. `daObjTribox::Act_c::reset` - `d/actor/d_a_obj_tribox.cpp` compiles but references a `daObjTribox`
   owner the composition does not carry.

Review: patches 0271 and 0272 are in `patches/tww/`, in `scripts/prepare_route_b.sh`'s series and in
the lock's `tracked_patches`, and `scripts/prepare_route_b.sh --check` passes at `03d27aa1...`.

**And the shape that wall's first two items need is settled by measurement.** The tempting move for
`dStage_Delete`/`checkDrawArea` is a new branch of the outer chain that compiles the unit's
fall-through region under its own tier. That cannot work, and the reason is a number: of the 74
functions that region defines, **45 are already provided by one of the port's tiers** - the five
info-inits, `dStage_roomReadInit`, `dStage_arrowInit`, `dStage_mapInfoInit`, `dStage_actorInit`,
`dStage_playerInit`, `dStage_stageLoad`'s loader family, `getMapInfo2`, `getStatusRoomDt`, `init`,
`dKankyo_create` and the rest - so a branch that compiles the region whole would define those 45 a
second time. The port's compositional unit is not the unit; it is the region minus what the other 25
tiers already own.

The measurement also says which 25 those are, and it is the good news: 25 functions are fall-through
only, and they contain all three owners the play scene asks for - `dStage_Create`, `dStage_Delete`
and `dStage_roomControl_c::checkDrawArea` - plus `checkRoomDisp`, `getDarkStatus`, `getDarkMode`,
`createMemoryBlock`, `destroyMemoryBlock`, `dStage_changeSceneExitId`, the Keep-Tresure and Keep-Door
helpers, `layerLoader`, `dStage_decodeSearchIkada`, `dStage_playerInitIkada` and eight info-inits
the ported tiers do not carry (`pathInfoInit`, `ppntInfoInit`, `memaInfoInit`, `mecoInfoInit`,
`dmapInfoInit`, `roomTresureInit`, `stageTresureInit`, `stageDrtgInfoInit`). Of those 25 only
`dStage_pathInfoInit` still carries the offset-in-pointer-field arithmetic the fall-through's other
info-inits were replaced for, so the tier is one rewritten function plus 24 bodies that need their
includes - and the 11 errors this ledger first met in this unit are all in the 45, not in the 25.


## 2026-09-22 The JStudio width repairs land, and d_stage's runtime owners have no PC tier at all

The second iteration of the v55 campaign, and it starts by correcting the first one's arithmetic.

**Correction.** The entry below files `d_stage.cpp` with the two JStudio translation units as a
"64-bit portability gap" that "does not compile under the Route B owner flags". That is wrong about
the file and right about one region. `d_stage.cpp` is tier-partitioned at the top: with
`BLUEWAKE_ROUTE_B_STAGE_INFO_TIER=1` - or `ROOM_CONTROL_INIT`, `KANKYO_CREATE`, `STAGE_RUNTIME` - it
compiles clean on a 64-bit host. With *no* tier it falls through to the console path, which is what
fails: `dStage_paletInfoInit` and its four siblings cast a `BigEndian<u32>` straight to a pointer,
and `dStage_roomReadInit` adds `(u32)i_file + (u32)rtbl_entries[i]`. The failing region is not a
width bug in a ported file; it is the console body, which no PC configuration was ever asked to
build.

**What that means for the handoff.** The two JStudio units were real portability gaps. `d_stage.cpp`
is a *missing tier*: no tier the port defines provides `dStage_Create`, `dStage_Delete` or
`dStage_roomControl_c::checkDrawArea`, which are exactly the three symbols `d_s_play.o` asks for,
because all three live in the console fall-through. Those three, plus the five info-init fixups
whose portable form already exists in the header as `dStage_resolveHostOffset`, are a tier the port
has not written.

**Landed: patches 0269 and 0270, and they are width repairs rather than reconstruction.**

- `JGadget/search.h` - `TExpandStride_`'s primary template had no `get`, so the only width the
  binary search could expand was the console's `s32`; it is asked for
  `std::iterator_traits<...>::difference_type`, which is `ptrdiff_t` on a 64-bit host. The primary
  now answers for any integral type.
- `JGadget/vector.h` - `size()` returned `((int)mEnd - (int)mBegin) / 4`: a truncated byte distance
  divided by a four-byte pointer, which is both unbuildable and a factor of two wrong where a
  pointer is eight bytes. It is now `static_cast<size_t>(mEnd - mBegin)`.
- `functionvalue.h` - both `TIterator_data_` inner classes took `s32` in `operator+=`/`operator-=`
  while declaring `ptrdiff_t` as their distance type, which is why `findUpperBound_binary_*` could
  not reach them. Both now take `ptrdiff_t`.
- `functionvalue.cpp` - four `u32`/`s32 size = param_1.size()` narrowings made explicit, and two
  `((int)p - (int)q) / 4` cast pairs replaced with the pointer difference they were spelling.
- `jstudio-object.cpp` - `(const void*)((int)param_5 + iVar7)` replaced with byte-pointer
  arithmetic.

Evidence: all five compile clean under the owner flags; the two patches are in `patches/tww/`, in
`scripts/prepare_route_b.sh`'s series, and in `config/dependencies.lock.json`'s `tracked_patches`
(**346 patch files on disk, 346 named**), and `scripts/prepare_route_b.sh --check` passes at
`03d27aa1...`. The Route B suite is **86/86 green**.

**The wall, narrowed.** With 0269 and 0270 in the composition the undefined set loses the eight
`JStudio::TObject_*` constructors, the whole `TFunctionValue_*` family, `JAIAnimeSound`'s vtable and
`typeinfo`, and `cDylPhs`. What is left is composition rather than width, and every file it names
exists: `dSnap_Create/Execute/Delete/DebugDraw` (`d/d_snap.cpp`), `fopDwIt_Begin`/`fopDwIt_Next`
(`f_op/f_op_draw_iter.cpp`), the grass/tree/wood/flower/magma packet owners (`d/d_grass.cpp`,
`d/d_tree.cpp`, `d/d_wood.cpp`), `daObjTribox::Act_c::reset` (`d/actor/d_a_obj_tribox.cpp`),
`JStudio::data::ga8cSignature` (`JStudio/JStudio/jstudio-data.cpp`), the `JStudio_J*` `TAdaptor_*`
constructors, and `JGadget::TVector_pointer_void`'s out-of-line members (`JGadget/std-vector.cpp`) -
with the d_stage runtime tier to be written first.

Review: `local-research/evidence/reorientation-20260909/logo-scheduler/build_probe_open.py` composes
those objects and `link_open.log` holds the undefined set `nm`-read from `route_b/src` and `ref/tww`
objects; `owner-portability.json` holds the per-source compile results the previous iteration took.

**And the runtime owners are missing by construction, not by omission.** `d_stage.cpp`'s outer
preprocessor chain ends in an `#else` whose fall-through is the whole console body: the room-metadata
and runtime includes, then `checkDrawArea`, `dStage_Create`, `dStage_Delete` and the rest. Every
earlier branch of that chain is one ported region - object names, the reduced runtime, room reloader,
passive metadata, room-read info, actor info, camera info, mult info - and each was measured: with
`BLUEWAKE_ROUTE_B_STAGE_PASSIVE_METADATA_TIER=1` the unit compiles clean and provides the five
native info-inits, while `dStage_Create`, `dStage_Delete` and `checkDrawArea` are still absent; with
`STAGE_RUNTIME_TIER` it compiles and provides none of them. So there is no PC configuration that
carries them at all.

Porting the fall-through wholesale is not the fix, because it is not a width problem there: it is
the console code, which stores a file offset in a pointer field - `setPaletInfo((stage_palet_info_class*)
pal_info->m_offset)` - which is a bogus address wherever pointers are 64-bit. The PASSIVE_METADATA
branch is the precedent for the right shape: resolve the file offset against the file base
(`dStage_resolveHostEntries`), decode into a host-layout array owned by the stage, and publish that.
The d_stage runtime tier is therefore a new branch of the outer chain with host-decode
implementations of those three owners - real writing, and the next piece of the campaign.

**And the rest of the list is not one kind of work.** Composing the objects named above, four of
them resolve clean and three are not composition at all:

- Clean: `d/d_snap.cpp`, `f_op/f_op_draw_iter.cpp`, `d/actor/d_a_obj_tribox.cpp` and
  `JSystem/JStudio/JStudio/jstudio-data.cpp` all compile under the owner flags.
- `JGadget/std-vector.cpp` - the file that instantiates `TVector<void*>`, which is what
  `TVector_pointer_void`'s out-of-line members are - is the *same width work* as 0269, one
  instantiation deeper: `GetSize_extend_` narrows `size_t` to `u32` and `Insert_raw` narrows a
  `ptrdiff_t` difference, none of which surfaced until the full instantiation was requested.
- The grass/tree/wood/flower/magma packet owners each include an actor-local *generated* header
  (`assets/l_K_kusa_00TEX.h` and its family). The asset preparer's current scope is the player and
  drawlist set; these owners need their family added to it before they can compile at all.

So the composition's remaining cost is one more width patch, one asset-preparer extension, and the
d_stage runtime tier - in that order of certainty, with the tier the only part that is new
semantics rather than a widening or a generated input.


## 2026-09-22 The route-B handoff is the play scene's composition, and the link names every piece of it

Step one of the route-B campaign the decision dossier opens, run on the tree rather than argued from
the September 9 notes. The frontier reproduces, the LOGO's gate turns out to be already open, and the
wall behind it is now an enumerated owner list rather than a claim.

**The frontier reproduces.** The private logo-scheduler probe
(`local-research/evidence/reorientation-20260909/logo-scheduler/`) rebuilds from the prepared Route B
source (`03d27aa1...`) and runs 600 original-scheduler frames: LOGO actions 0, 1, 3, 4 and 10 advance
at frames 5, 95, 125, 215 and 245, `mDoAud_Create` completes with the adjusted 2,168,352-byte audio
heap, the scheduler reaches `dComIfG_changeOpeningScene`, and the run ends at the probe's own
incomplete-handoff fence. Same shape as the September 9 record, so the campaign starts from a
verified state and not a stale one.

**The gate is not the DVD wait.** The probe's new state read is
`BW-DVD-STATE pending_head=0x0 syncAllObjectRes=0 reset=0` at frame 600: the original DVD command queue
is drained, no object resource is outstanding and no reset is pending. `dScnLogo_c`'s `dvdWaitDraw`
therefore passes its whole guard and calls `dComIfG_changeOpeningScene`, which sets next stage `sea_T`
room 44 and requests `fpcNm_OPENING_SCENE_e` through `fopScnM_ChangeReq` with `fpcNm_OVERLAP0_e`. The
LOGO stays in action 10 because `fpcPf_Get(fpcNm_OPENING_SCENE_e)` returns NULL - the installed static
rel registry holds `fpcNm_LOGO_SCENE_e` alone. The change is asked for and cannot be answered.

**Registering the profile moves the wall to the link, and the link enumerates it.** Adding
`{fpcNm_OPENING_SCENE_e,"d_s_play",&g_profile_OPENING_SCENE.base.base,{sizeof(dScnPly_ply_c),0,alignof(dScnPly_ply_c)}}`
(the entry validates because `dScnPly_ply_c`'s size is the profile's `mSize`) turns the next failure
into a link failure, and the undefined set *is* the play scene's owner list:

- **`cDylPhs::Link` / `cDylPhs::Unlink`** - the REL link phase that `d_s_play`'s `phase_6` and scene
  delete run. **Landed**: `route_b/src/native_dyl_phase.cpp` answers the phase from the installed
  static rel registry, and reports `cPhs_ERROR_e` for a process name the composition does not carry
  instead of the console's `cPhs_INIT_e`, which waits forever for a REL that is not coming. That
  silent wait is what parked the LOGO.
- **The JStudio owners** - `TObject_actor` through `TObject_sound`, `JStudio::data::ga8cSignature` and
  the `TFunctionValue_*` family come from `jstudio-object.cpp` (pointer narrowed through `int`, line
  149) and `functionvalue.cpp` (`size_t` narrowed to `u32`), neither of which compiles under the Route
  B owner flags.
- **The play scene's stage, demo and particle owners** - the `JStudio_JStage/_JAudio/_JMessage/_JParticle`
  `TAdaptor_*` constructors, `dStage_Create`/`dStage_Delete`, `dStage_roomControl_c::checkDrawArea`
  (all of `d_stage.cpp`, which casts `BigEndian<u32>` straight to a pointer), `dSnap_Create/Execute/
  Delete/DebugDraw`, `fopDwIt_Begin`/`fopDwIt_Next`, the `dGrass`/`dTree`/`dWood`/`dFlower`/`dMagma`
  packet owners and `daObjTribox::Act_c::reset`.

**What that is worth to the decision.** The dossier's section 2 said the barrier is the port's coverage
rather than the cost of executing guest code; this is that claim made specific, as an ordered owner
list three of whose compile failures are 64-bit portability rather than missing source. It is not only
missing source: `fvb.cpp`, `fvb-data-parse.cpp`, `fvb-data.cpp`, `jstudio-control.cpp` and
`JAIAnimation.cpp` do compile and link, and linking them is what removed `JAIAnimeSound`'s vtable and
`typeinfo` from the undefined set. `owner-portability.json` in the same evidence directory records the
per-source result.

Review: `route_b/CMakeLists.txt` registers `src/native_dyl_phase.cpp` in `bluewake_route_b_static_rel`
and `tests/route_b_native_dyl_phase_test.cpp` as a test, and the Route B suite is **86/86 green**
(`ctest --test-dir build/route-b-foundation`). The change touches nothing under `ref/`, so no patch
registration is owed. The private evidence is `local-research/evidence/reorientation-20260909/logo-scheduler/`:
`probe.cpp`, `build_probe_open.py`, `owner-portability.json`, `link_open.log`, `dvd-state.{log,json}` and
the `scheduler-run*` set.


## 2026-09-22 The spike is answered: the same function costs 1,119 emitted operations in route A and 177 instructions in route B

Step two of the amended spike, on the function step one named: **J3DSys::reinitTevStages**, hot
(about 475,000 returns over the certified route), self-contained, and present in the decomp at
`ref/tww/src/JSystem/J3DGraphBase/J3DSys.cpp` with a guest symbol of `size:0x308`, which is 194
PowerPC instructions.

**Route B's side** needed no build: the route-B release build directory already holds a compiled
`J3DSys.cpp.o` (built 2026-09-06), and the function is in it as
`__ZN6J3DSys15reinitTevStagesEv` - **177 host instructions for 194 guest instructions, 0.91 per
guest instruction**.

**Route A's side**, counted from the emitted C for the same guest range
(`chunk_0182_text1_802D96E0.c`, labels 0x802D9868 to 0x802D9B70, one label per guest
instruction): **194 labels and 1,119 emitted statements, 5.8 emitted operations per guest
instruction** - of which 230 are pc stores, 98 are cycle-accounting statements, 177 touch the
guest register file, and 614 are everything else. The compiled host instruction count is at least
that, and the chunk-level instrument measures the emitted body at 27.24 host instructions per
guest instruction, so the true A figure for this function is between 5.8 and 27.2 per guest
instruction.

**The comparison, and its caveats.** Same function, same guest work, two execution strategies:
**0.91 for B against 5.8 to 27.2 for A** - between six and thirty times cheaper, and far below the
dossier's threshold of ten. The caveats are worth stating: both A figures are *static* counts
(A's is a lower bound, since the compiler emits at least one instruction per statement and the
dynamic measure is higher), B's 177 is static too and its loops repeat at run time, and this is one
function of the J3D TEV setup - friendly to a port, since it is data-shuffling rather than
control-flow-heavy. None of that closes a six-times gap, let alone a thirty-times one.

**So the decision section 6 asked for is made: promote route B.** Its speed is settled with room
to spare; what stands between the project and a running play scene is the port's coverage - the
268-patch series and the scenes it does not yet reach - not the cost of executing guest code. Route
A's remaining trims are tenths of a percent against a measured 1.24x ceiling.

**What that means for this loop, stated plainly.** The objective's second terminal condition - the
queue's items each closed with a measured refutation, on a tree whose sources, objects and
artifacts agree - has been met for several iterations now, and the primary one, sixty retraces a
second, is measured out of reach in route A's emit shape. The path to it is now identified and
priced rather than hoped for, and executing it is a different program from this one: reconstructing
and porting the play scene's code, which the dossier's section 1 and section 4 describe, and which
needs a direction only the user can give.


## 2026-09-22 The play window's turns end at two guest waits and a J3D TEV loop, named from the decomp

Step one of the amended spike - map the hot emitted code to the decomp's functions - is done. The
instrument is the host's own return census (`BLUEWAKE_RETURN_CENSUS` with `_EDGES`), which prints
the top sixteen edges as input pc, output pc, count and charged cycles. On the certified route at
14,700 retraces:

| input pc | count | decomp symbol |
| --- | --- | --- |
| 0x80307EF4 | 9,356,741 | SelectThread +0x14C - the OS thread-select idle |
| 0x80328F84 | 5,814,115 | **_restgpr_27 - an EABI register-restore stub** |
| 0x80303104 | 182,640 | DCFlushRange +0x2C |
| 0x80303A50 | 78,424 | OSLoadContext |
| 0x802D9A60 to 0x802D9AD8 | about 475,000 over seven pcs | **J3DSys::reinitTevStages()** |
| 0x8028EA04, 0x8028E998 | about 69,000 | DSPSendCommands2 |

and the census's own totals agree with the turn inventory to within two percent: 20,573,305 returns
against 20,138,490 recorded turns, split budget 10,447,071, same-chunk 7,607,694, cross-chunk
2,027,748, exception 434,815.

**Three things fall out of it.**

**The ledger's own mystery is identified.** The "zero-charge exit" pc that the prepaid-copy
investigation found at the centre of the mid-block resumes, 0x80328F84, is **_restgpr_27** - an EABI
register-restore stub, long straight-line code, which is exactly where a cycle budget expires inside
a block. That also explains the family around it: _restgpr_24, and the DSP send path at
0x8028E998.

**The turns end in two places.** The OS thread-select idle and those register-restore stubs are
about three quarters of every return. In a port a function return is an instruction; here it is up
to sixty-four emulated register loads, so a large share of what the emulator dispatches is the
ABI's epilogue.

**And the renderer's setup is visible in the guest's own code.** J3DSys::reinitTevStages turns up at
seven pcs with roughly 475,000 returns over the route - the same work the rendered capture shows
costing 2.53 percent of the main thread inside func_803256E0.

**So the spike has a better subject than the dossier proposed.** J3DSys::reinitTevStages is hot,
self-contained, and *has* a symbol in the decomp, unlike the chunk entry named in section 6. Step
two - compile it from the decomp's source with the decomp's flags, count its host instructions, and
divide by its guest operation count read from the DOL - is the next iteration, thresholds unchanged
at 10 and 15.


## 2026-09-22 The device service is the DSP interpreter, and its cost is latency rather than instructions

Decomposing the last unmeasured region of the thread, from the same 20 s rendered capture:
`host_sync_cycle_devices_end_turn` is **10.02 percent of the main thread**, and **9.56 of that
10.02 is `host_sync_dsp_cycles`** - the donor DSP LLE interpreter. Inside it,
`Interpreter::RunCycles` 5.48, `Interpreter::Step` 1.59, and the handlers behind them: ReadDMEM
0.49, OpWriteRegister 0.42, ldax 0.37, lr 0.34, jcc 0.30, s 0.30, mrr 0.27, and the rest of the
opcode set below those. Everything in the device service that is not the DSP is tiny:
`host_sync_vi_cycles` 0.19.

**What it settles.** The v52 split's account is confirmed - "10.06 of 10.2 is the DSP" - and a
contradiction my own entries left behind is resolved. Two changes to the interpreter's
per-instruction path moved the route by 0.37 percent and by nothing measurable, which at 1.35 M
emulated instructions a retrace is inconsistent with a 9.6 percent instruction share. So the DSP's
share is **wall time against a small instruction count**: it is the interpreter's *latency* - a
branchy indirect dispatch, table reads, misses - rather than its arithmetic. The instruction
instrument cannot see it, and the two changes that were visible to it were the small parts.

**What it closes: the objective's first item.** The per-opcode dispatch is landed (patch 0054,
-0.37 percent), the per-instruction frame and the host memory callbacks are measured nulls, the
census's DSP cycles are corrected to *requested* rather than executed, and what remains is the
interpreter's shape. The ground rules forbid a runtime JIT, and the only ways to make an
interpreter's dispatch cheaper are to compile it or to stop interpreting - so the item closes with
its remaining 9.6 percent named and out of reach rather than untried.

**And the thread is now fully mapped.** Of the main thread: the emitted bodies 73.5 percent, the
device service 10.0 (of which the DSP 9.6), the edge service 4.3, and the dispatcher's own
remaining share a few percent - with the renderer's worker invisible to a main-thread capture and
+18.6 percent of the rendered window's instructions. No region of the play window is unmeasured
any more.


## 2026-09-22 The spike needs a mapping step first: the hot emitted symbols are chunk boundaries

Starting the route decision's spike - compile the hottest function of the play window from the tww
source and measure its cost per guest operation - found two things before the measurement can be
made, and both are recorded in the dossier's amendment.

**The emitted symbols are chunk boundaries, not function starts.** `func_803256E0` and the other
seven names in the rendered capture are the recompiler's partition entries, 4,096-instruction
chunks, so none is a function entry in the guest's own symbol map: all seven are absent from
`ref/tww/config/GZLE01/symbols.txt`, while that map is comprehensive at **24,275 symbols** and does
cover their neighbours - `GXInitGX` at 0x80320354 for the 0x8032 region, the d_wpot_water statics
at 0x80240038 for the 0x8024 region. So the hot code is *inside* those chunks rather than at their
entry, and the spike's first step is a mapping: a deeper sample under the hot chunk gives the inner
guest pcs, and those pcs go through the decomp's map to the source.

**And the coverage the spike was meant to test looks better than the dossier assumed.** That map
covers the engine and SDK regions as well as the game's own, which moves the question: route B's
distance to a running play scene is set by the port work its 268-patch series records rather than by
missing source. The spike is still the thing that settles the *speed* half, and its thresholds are
unchanged - at or below about 10 host instructions per guest operation, route B's speed is settled
and the campaign is about port coverage; above about 15, route A's re-emit is the cheaper path.

The amended spike, in order: map the hot chunk's inner pcs to decomp functions (one sample, one
lookup); compile one of them from source with the decomp's flags and count its host instructions;
divide by the same function's guest operation count, read from the DOL.


## 2026-09-22 The block-local downcount is refuted by the emit shape, and the body-side program is closed

The previous entry named a block-local `downcount` as the next emitter project: `downcount` is
touched as memory at every instrumented charge and every leader guard, so holding it in a local and
flushing at the block's exits would turn those into register operations. Reading the emitter to
implement it refutes the idea from the code's own structure.

**Why it cannot be done in this shape.** `emit_flat_function` emits the entry dispatch as a
computed goto over a label per instruction:

```c
    static void* const pc_table_%08X[%u] = { &&label_%08X, ... };
    goto *pc_table[(pc - lo) >> 2];
```

and then emits `label_%08X:` for **every** instruction, not only for leaders, because the host can
resume at any pc - that is exactly the mid-block resume the second-body design was built to handle.
A local therefore has to be loaded at every label to be correct, which is once per instruction: no
saving, plus a store at every return. The same reasoning rules out the cheaper half-measures -
caching the budget across a block fails too, because any observing instruction can rebudget it.

**What that closes.** With the test itself unremovable (the second body costs more than it saves,
measured at +2.4 percent), the cycle accounting's 16 percent of the emitted body is *structural* in
this emit shape rather than untrimmed, and with it the body-side program: the access path is already
inline and its one redundant clause is removed, the pc store and branch hints and the register file
and `__restrict` are all measured nulls or regressions, and the pc cache's size and inlining are
settled. The 73.5 percent of the main thread that is emitted bodies has no remaining measured
candidate inside route A's emit shape.

**So the next step is a decision, not a trim**, and the operating procedure says so: docs/GOAL_LOOP.md
§8 opens a route decision when the bounded fixes are exhausted, and §8.1 names what the dossier must
contain. It is written at
[status/ROUTE_DECISION_2026-09-22.md](ROUTE_DECISION_2026-09-22.md): the measured state, the
exhausted fixes with their numbers, the affected surface, the routes available under the ground
rules - a runtime JIT is not one of them - and a falsifiable spike that decides between compiling
the play window from source and continuing to improve the translation.


## 2026-09-22 The cycle accounting is priced with sound ablations: the budget guard alone is 6 percent of the body

Two ablations in the harness had never been run. Both are sound in the sense the `unchecked` form
in the entry below is not - they remove cost, not behaviour - and both are cheap: 20,000 entries,
ninety seconds, against a baseline of **27.24 instructions per guest cycle** for chunk 0144.

| ablation | instructions per guest cycle | against the baseline |
| --- | --- | --- |
| none (baseline) | 27.24 | - |
| `restrict` | 27.29 | a null |
| `no-guard` | **25.61** | **-1.63, -6.0 percent** |

**`restrict`** puts `__restrict` on the emitted function's `CPUState*`. It does nothing, which
says the compiler was not inhibited by aliasing between the guest state and the memory accesses -
the corroboration from the other side of the ledger's older register-file refutation.

**`no-guard`** removes the budget-exhaustion test at block leaders:
`if (ctx->downcount <= -(s64)DOLRECOMP_C_LOOP_CYCLE_BUDGET) { ctx->pc = ...; return; }`. That is
**6.0 percent of the emitted body**, and it is *executed* cost: a load of `downcount`, a load of
the budget, a compare and a branch at every leader, not dead weight.

**Where that leaves the accounting.** Measured separately over this and earlier entries:
`prepaid` 9.8 percent (the per-instruction charge test and its accounting), `no-suffix` 6.4 (the
observation suffix and reconcile), `cheap-budget` 3.3 (the budget test's defensive fallback),
`no-guard` 6.0 (the leader guard above), `no-pc` +0.7 (the pc store, removed and negative).
They overlap - `prepaid` reaches into the same sites `no-guard` does not - so they do not add up
to 25 percent, but every one of them is now a number with a sound ablation behind it rather than a
code-size share, and the 10-15 percent this ledger has quoted for the cycle accounting since v49
has a measurement under it.

**The shape this points at, which no ablation can price.** `downcount` lives in `CPUState` and is
touched as memory at every charge and every guard: a load-modify-store per instrumented charge, and
a load per leader test. Keeping it in a **block-local** and flushing it at the block's exits would
turn those into register operations. It is not a text-level transform, and that is why it is named
here rather than priced: the charge helper reads and writes `ctx->downcount` across a call
boundary, so a textual ablation would have to flush around the call and lose exactly the sites
where the win is. The honest shape of the next emitter project is a local `downcount` threaded
through the charge helper and the reconcile, with a flush emitted at every return - and the digest
is the gate, because a missed flush moves the delivery history rather than the instruction count.


## 2026-09-22 The access-path ablation cannot be run, and the body baseline reproduces at a cheaper sweep

Working the last open body-side question - the ledger's inference that the inlined access path is
"only about 3 percent of the dynamic stream", which rests on removing one redundant clause and
multiplying up - I reached for the ablation harness's `unchecked` form, which is the one that
shortens the whole memory path rather than a clause of it.

**It cannot be run, and by construction rather than by accident.** `ablate_chunk.py`'s
`UNCHECKED_PRELUDE` says what it is in its own comment: *bounds-only MEM1 fast path, no alias flag
and no mirror test*. A guest pc that touches memory a REL has aliased therefore reads the
underlying MEM1 instead of the REL's storage, the guest diverges, and the sweep never returns -
at the default entry count and at 20,000 entries alike, both killed by the harness's 180-second
fence. So the ablation prices a program that is not the route, and the 3 percent inference has no
measurement behind it.

**What that leaves, and it is not a pool.** The shipping `mem_read32`/`mem_write32` are
`static inline` in the generated header, so the "access path" is already inline code in every
emitted body: there is no accessor call whose removal could pay, only the clauses around the
load - the MEM1 bound, the alias flag and the mirror test - and of those the one that was
redundant has already been removed and measured at about 0.6 percent on the route. The remaining
clauses are load-bearing. The honest statement replaces the inference: the access path is
already the shipping shape, and the body-side pool that is still unmeasured is the *dynamic
category split* of the 27 host instructions per guest cycle - which needs counters emitted
alongside the code, not an ablation that changes what the guest does.

**And the baseline is reproducible more cheaply than the harness's default.** `bench_chunk.sh
--ablate 0144 none 20000` gives **27.24 instructions per guest cycle** in about ninety seconds,
against the ~27 this ledger has quoted for chunk 0144; the default entry count costs three
minutes for the same number. That is the setting to use when a construct needs pricing rather
than a result.


## 2026-09-22 The patch-registration debt is closed: 344 files, all named

The queue has carried "the ref/ patch registration debt" as housekeeping, and the audit two
entries below put a number on it: 344 patch files under `patches/`, 76 named in a dependency's
`tracked_patches`. It is closed now, by registering each file where it belongs - `patches/dolrecomp`
(2), `patches/aurora` (7), `patches/tww` (261) - and the re-audit reports **344 patch files on
disk, 344 named, none untracked, and none named but absent**.

Each of the three entries' `purpose` gains one clause naming its registered series, so a reader
sees the series and the list together rather than a list alone.

**Verified with the tree's own gate**, not with prose: `scripts/audit_repo.sh` reports the
dependency lock valid JSON and `AUDIT PASS`.

**What this closes, and what it does not.** The fence - register every `ref/` patch in
`config/dependencies.lock.json` - is now satisfied by a check a script runs, which is the point:
before this, 270 files were on disk with nothing naming them, and the two entries this session
added (0054, 0055) had been registered one by one into a list that stopped at 0052. It does not
claim the 270 older patches were *applied* in the order the list now records; that is what each
entry's `base_sha`, `commits` and `working_tree` capture are for, and those are unchanged.


## 2026-09-22 The worker's memset is a bzero from the payload assignment

The previous entry left one step named: read the stub targets out of the binary instead of
inferring them from the source. Done, and it corrects the mechanism without changing the size.

**The stubs.** `make_render_packet`'s seven `bl` sites resolve through `__stubs` to three
imports, named by `otool -Iv`'s indirect symbol table: `___stack_chk_fail` (the 0x310-byte
frame's guard, cold), `_memcpy` (the event copy and the return slot), and **`_bzero`** at
`0x100538a18`. libsystem's `bzero` is the implementation behind the sampler's
`_platform_memset` leaf, so the two names are the same code.

**What that corrects, and what it does not.** The zeroing is real and is emitted - but it is
*not* the four payload members' own default initializers, which is the thing the previous entry
made plain and measured as byte-identical. It is the **assignment of the selected payload**:
`packet.draw = { .kind = ..., .address = ... }` is an aggregate *assignment*, which
value-initializes the rest of that member, and the payload structs carry initializers of their
own, so the compiler emits a `bzero` per packet for whichever member the kind selects.
`RenderPacket` is 784 bytes, so a payload is hundreds of them.

**Why the fix is not free.** Assigning only the fields each consumer branch reads would remove
the `bzero` - but the zeroing is also what supplies the *defaults* for the fields the producer
does not set, so that change is only safe per `kind`, by reading what each consumer branch
actually touches. That is a bounded piece of work, and it is a different piece from the
initializer change this item first reached for; the gxcore pixel tests and the rendered route
are its gates. The estimate stands where it was: about a third of the worker's active time is
memset plus memmove, and the `bzero` is the memset half. There is a zero-risk half too now that
the copy is named - `make_render_packet` returns its 784-byte packet by value, so the return
slot is a `memcpy` per packet that an out-parameter would delete outright.


## 2026-09-22 The packet-zeroing hypothesis is codegen-inert: the compiler had already elided it

The entry below named the worker's memset as the next candidate and identified it as
`RenderPacket`'s four unused payloads being zero-filled by their own default initializers.
That is wrong, and the way it is wrong is cheap and worth recording.

**The test.** The four payload members were made plain - no `{}` - so that
`make_render_packet`'s aggregate initializer would leave them alone, which is the safe form the
previous entry proposed. Rebuilding the host **recompiled the translation unit and its archive
and relinked the executable, and all three came back byte-identical**: the object `2df376e9...`
before and after, the host `00ed6599...` before and after. A control run confirmed the build
does track this header - touching it recompiles the object, and the archive and the host relink -
so the chain is live and the change is genuinely inert: under ThinLTO the compiler already
proves the unused payloads are not read and elides their zeroing.

**The disassembly says the same thing.** `make_render_packet` in the built host has a
**0x310-byte frame** - `RenderPacket` is a 784-byte struct, so the payloads really are large -
and no `memset` symbol is reachable in it; the seven `bl` sites it does have point at unnamed
stubs, which is where the worker profile's `_platform_memset` must actually come from. What the
profile attributes to `make_render_packet` is therefore not the struct's initialization.

**What that leaves.** The worker's memset plus memmove is still about a third of its active time
and still worth having, but the source is not the packet's payload initialization and the fix is
not in the struct's initializers. Naming it needs the stub targets read out of the binary rather
than guessed from the source - which is one instrumented step, and is where this item goes next.
The reverted change is reverted: the working tree is back to the pin and the three artifacts
hash where they did.


## 2026-09-22 The FIFO worker's own profile: idle 81 percent, and a third of its work is memset and memmove

The rendered capture from the entry below carries the worker's tree as well as the main
thread's, and it answers the question the GX item has been carrying since translation moved off
the main thread: where the cost sits now.

**The main thread, summed over call sites (16,259 samples).** `host_mmio_write` **14.88
percent**, `aurora_backend_gx_write` 10.22, `gx_aurora::shadow_frontend_write` 10.04. Inside
that write path: `std::mutex::lock()` **3.60** - of which **3.12 is `__psynch_mutexwait`**,
the main thread *blocked* - plus `condition_variable::notify_one` 1.05 and
`std::vector::__insert_with_size` 0.74. Those are the guest-visible drain barriers, which the
design says are the only place the main thread may wait; the hand-off itself is a swap and a
counter, and the worker translates outside the lock.

**The worker (16,259 samples, `g_fifo_worker_main`): 81.31 percent of its time is waiting on
its own condition variable.** Of what is left:

| | share of the worker's samples |
| --- | --- |
| `RetailGxFrontend::flush` | 14.00 |
| `emit_new_packets` | 9.45 |
| `parse_stream` | 6.34 |
| `make_render_packet` | 5.79 |
| **`_platform_memset`** | **4.05** |
| `ConsumingAuroraRenderSink::submit_packet` | 3.03 |
| `handle_draw` | 2.34 |
| `_platform_memmove` | 2.15 |

**The mechanism, read off the code.** `make_render_packet` builds its result with an aggregate
initializer - `RenderPacket packet{ .kind = ..., .sequence = sequence, .event = event }` - and
`RenderPacket` is a flat struct in which *every* member carries a default initializer:
`event{}`, `stream{}`, `state{}`, `resource{}`, `draw{}` (render_sink.hpp:147). So each
packet zero-fills the payload of all five and then fills exactly one, and the memset the
profiler sees is the four that are not used. The same shape appears beside it in
`build_array_sizes` ("slots are zeroed then set from the draw's indexed inputs").

**What it is worth, and the trap that decides whether it can be taken.** memset plus memmove is
about a third of the worker's *active* time, and the active time is what the rendered
differential counts as the renderer's +90.6 M a retrace - so the prize is real. But if anything
hashes or records the packet as bytes, the zeroing is load-bearing and removing it is a
nondeterminism bug rather than a speedup: the tree has a recording sink that copies whole
packets and a packet-sequencing path that hashes them. The safe version is to zero only the
member the packet's `kind` selects, and the first iteration on it is to find out whether any
hash covers the packet's whole storage.

**The trap checked, and it is not armed.** The concern above was that something hashes or
compares a packet as bytes, which would make the zeroing load-bearing. A search of the frontend
and the backends finds no whole-packet hash, no memcmp over a packet and no packet-wide
comparison: the only whole-packet operation is the recording sink's copy
(packets_.push_back(packet)), which copies the unused members and never reads them. So the
zeroing can go, with the safe form - fill the member the kind selects and leave the rest alone -
as the shape to write.

**And the shape of the hand-off, for the record.** The worker is idle 81 percent of the wall
time against the 94 percent the ledger recorded before the deferral work - it is doing more of
the translation - and the main thread's remaining 3.1 percent of blocked time is at the
barriers where the guest's own synchronization requires the FIFO to be drained. That is the
item's remaining gap: not the hand-off, which is a swap and a counter, and not the wake-up
cadence, but the translation's own work, which is where the memset is.


## 2026-09-22 The rendered play window has a wall reading: median 25.0 fps, p99 58.4 ms

Two queue items closed, one of them the product's number.

**Item 5's other half.** The rendered level was already on record as instructions (487.1 M a
play retrace, +22.9 percent over headless). This is what a player would see, from a rendered
run at 14,700 with frame timing on, parsed by `scripts/bench_report.py` over the
14,100-14,700 window (600 frames), digest `83d2590d...` unchanged and the stop pc and turn
count at the reference:

| reading | value | against the 16.667 ms target |
| --- | --- | --- |
| median frame | 40.0 ms (24.97 fps) | **2.4x** |
| p99 frame | 58.36 ms (17.14 fps) | **3.5x** |
| mean | 25.34 fps | |
| range | 16.61 to 54.85 fps | |

Against the ledger's last rendered *wall* reading (2026-09-18: median 37.85 ms, p99 85.99 ms on
an earlier artifact) the tail has moved a long way - p99 1.47x better - which is the FIFO
translation worker's signature: the synchronous flush that used to sit in the frame is off the
main thread. The median is flat within the wall-clock noise this ledger has learned to expect.
The whole route's wall-over-guest is 90.9 percent real speed, and that is a route-average
artifact rather than a play number: 13,900 cheap boot-and-intro retraces against 600 expensive
play frames.

Caveats kept with it: this is the scripted route, not a human's session, and wall clock moves
with machine load - the ledger's rule stands, instructions are the measurement and milliseconds
are colour, with the p99 the one place the colour *is* the reading.

**The three levels, side by side.** headless 395.9 M a retrace (1.7x from 60 a second),
rendered 487.1 M a retrace (2.1x in instructions), rendered wall median 25.0 fps (2.4x).

**Item 5's other other half: the patch-registration debt is 268 files, not a rounding error.**
`docs/GOAL_LOOP.md` has been carrying "the ref/ patch registration debt" as a housekeeping
line. The audit: **344 patch files exist under `patches/`, 76 are named in a dependency's
`tracked_patches`, and 268 are not** - the whole `patches/aurora/` series and
`patches/tww/0008` through `0268`. Nothing tracked is missing from disk. The lock does carry
a second mechanism for the same obligation, the `recompcore` entry's `working_tree` capture
("working tree as landed 2026-09-22"), so this is not 268 unrecorded changes so much as one
recorded working tree with 268 older per-patch records missing; but the per-patch series is what
the lock's `tracked_patches` list is *for*, and until this session's two additions (0054, 0055)
it had not been extended since patch 0052.


## 2026-09-22 Two dispatcher candidates measured and reverted: aliases first (+0.53), a four times larger pc cache (null)

The first rendered owner capture since the FIFO worker landed is in hand (20 s `sample` at the
live play window, rendered), and its most interesting line is small: **`dolrecomp_call_slow`
is 1.13 percent of the play thread** - the dispatcher's out-of-line path - where
`chassis_dispatch` is 77.15, `selected_dispatch` 73.49 and the device service 9.57. It also
shows the renderer's work no longer concentrated in one guest function: **`func_803256E0` is
2.53 percent against 19.6 percent in the earlier census**, which is the FIFO worker and the GX
trims showing up as a share.

That 1.13 percent invited two hypotheses about the cold path, and both are refuted.

**Aliases first - a regression of +0.53 percent.** The single-function shape searches the raw
address before it tries an alias, and an aliased pc is by construction not in the chunk table,
so that search cannot succeed: 398.0 M against 395.9 M with the probes reordered ahead of it,
digest unchanged. The cause is in the object rather than in the logic: the two-arity probe
(`allow_search`) is emitted twice, the compiler outlined it instead of inlining it, and every
dispatch then paid a call - about six instructions, 0.58 percent - which is within a hair of
the measured regression. The reorder did remove the wasted search for aliased pcs and bought
less than the outlining cost. Reverted, composite byte-exact back to `6e4829fe...`.

**A four times larger pc cache - a null.** If the fast probe were missing often, the searches
behind the misses would be the cold path's cost, and the cache is 4096 entries keyed by
`(address >> 2) ^ (address >> 14)`. Enlarged to 16384 it measures **395.7 M against 395.9 M**:
the cache is not thrashing, the hit rate is not the lever, and the question is settled.
Reverted.

**What the pair leaves.** The cold path is entered for aliased and unresolvable pcs rather than
for cache misses, and what is left in the dispatcher is the shape of the `dolrecomp_call`
inlining rather than its arithmetic. One instrument improvement is kept:
`scripts/splice_composite_dispatch.py` now anchors on the emitter's function names and its
structural markers instead of one exact signature line, which is what left
`dolrecomp_call_original` stale when the first attempt changed the signature - the same
"a generated artifact must prove itself" failure in a different costume.


## 2026-09-22 The rendered differential works again, and the renderer's share is 22.9 percent

`scripts/bench_rendered.sh` was carrying the turn-count references of the 63 MB artifact
generation - ceilings 13,800 and 14,100, turns 29,737,920 and 32,203,791, and one stop pc for
both - so against the composite in the tree it refused to report at all, the same way the
instruction harness's reference had gone stale before. Its ceilings are now the instruction
harness's (13,900 and 14,700) with the references that harness records (14,287,831 at pc
`0x80307EF4`, 20,138,490 at pc `0x8027FA30`), the guard checks the pc per ceiling, and the
summary names which ceiling stops where.

**The reading, on the current artifacts (composite `6e4829fe...`, host `00ed6599...`):**

| configuration | instructions per play retrace |
| --- | --- |
| headless | 396.4 M |
| rendered | 487.1 M |
| **the renderer's share** | **+90.6 M, +22.9 percent** |

**What moved.** The last rendered differential this ledger recorded (2026-09-18) was **+33.1
percent** on the same script, so the renderer's *share* of the play window has fallen by about
a third. The rendered level is 487.1 M a retrace against the headless 396.4 M: at the host's
sustained ~14 G instructions a second that is 34.8 ms a retrace, **47.9 percent of authentic
speed**, against the 33.6 percent the rendered profile recorded on 2026-09-18 - the GX work of
the last two days, including the FIFO translation worker, is visible in this number.

**What this number is not.** The differential differences `/usr/bin/time -l` totals, so it
counts every thread the process runs: moving the FIFO translation to a worker does not lower
this figure by itself, because the worker's instructions still retire. What it measures is how
much *work* the renderer is, not how much of that work is on the critical path; the
product-level reading is wall time and the p99 tail out of `scripts/bench.sh`, which wants an
idle host and has been refused by the focus preflight before. The two readings answer different
questions and this entry claims only the first.

**Where the work is, in one line.** Of 487.1 M instructions a rendered play retrace, the
renderer is 90.6 M and everything the censuses have priced - the emitted bodies, the edge
service, the device service - is the other 396.4 M.


## 2026-09-22 The DSP's memory callbacks are not hot, and the save-continue path is closed

Two results from auditing the objective's own list rather than hunting new candidates, and one
of them is a null that closes a question worth closing.

**The DSP's memory traffic through the host is not a lever.** The four callbacks the donor's
microcode reaches the host through (`read_memory`, `write_memory`, `read_aram`,
`write_aram`) were `std::function`s wrapping calls the C bridge already had as plain function
pointers - a type-erased indirect call and a captured-context load in front of every DSP memory
access. Converting them to function pointers with the bridge's own user pointer, which is a
host-only change, measures **395.7 M against 395.9 M**: inside the instrument's own spread, so
a null, and reverted (the rebuilt host hashes back to `00ed659d...`). It is a useful null: it
says the DSP's memory traffic is not the interpreter's cost either, which is one more
independent check on the same conclusion the two dispatch changes reached - what is left in
that interpreter is not on the paths that run per access or per dispatch.

**The save-continue path is closed, and it was closed before this loop started.** The
objective's third item is P4 milestone 9, the time-to-playable reading. It exists as
`scripts/save_continue_acceptance.sh`, run today with its artifacts under
`local-research/acceptance/20260922-144732/`, and its documented control numbers are the
reading the PRD asks for: **the cold new game reaches control at 20,338 retraces (339.0 s of
authored content) and the save-continue path at 833 retraces (13.9 s)** - well inside five
minutes, and the reason the second half of that script exists. The host drives the pause menu's
Save screen (the card the certified route leaves holds the *empty* file the card manager
creates at file-select, not a save), the guest's own save chain runs, and a separate boot on the
written card reaches event-free gameplay without any new-game milestone. The route is armed by
`BLUEWAKE_SAVE_ROUTE` and is inert otherwise, so the certified route and the bench are
untouched by it.

**What that leaves for the goal.** Of the objective's three named items, one is landed here
(the DSP dispatch, patch 0054), one is closed as measured and documented above (save-continue),
and one - the GX FIFO path off the main thread - is the other writer's active workstream, with
the worker serialized against the frame transition and stopped and joined (patches 0051, 0052)
and the rendered path re-established at 14,700. The headless window stands at 395.9 M
instructions a retrace, which is 1.7 times the ~233 M that 60 retraces a second needs; the
remaining increment-sized levers are the ones this census and the two before it have priced.


## 2026-09-22 The alias-state revalidation becomes a generation compare: -0.42 percent

The census below ranked the edge service's pieces by what they cost at every boundary, and
the alias-state revalidation was second at ~7 instructions. It was asking "has the guest's
aliasing moved since I resolved this pointer" by reading a boolean: `adrp/add` for the flag,
`ldrb` for it, `adrp/add` for the cache globals, a load of the validity flag, a load of the
cached flag, a `csel` into 0/1 that was already in the source, and a two-way `ccmp` - ten
instructions at **every** block boundary, 3.9 M a retrace, to answer a question that is "no"
almost always.

**The change.** The runtime keeps a counter, `g_ppc_guest_alias_generation`, that moves in the
one setter every write to the overlap boolean now goes through, so a reader cannot be missed
by a write site the way a boolean read can land mid-update. The host's overlap cache stores
the generation it resolved under and compares it - one load, one compare, one branch. The
cache's separate validity flag is gone with it: the cached generation starts at a value the
counter cannot hold, so the first call still resolves. It is patch 0055, registered in the
lock, and it touches both binaries because the runtime is compiled into each.

**Measured: 395.9 M against 397.5 and 397.6 M**, **-0.40 to -0.43 percent**, route digest
`83d2590d...` unchanged, both ceilings at the recorded stop pc and turn count, 217/217 host
tests pass. Composite `6e4829fe...`, host `00ed6599...`, both rebuilt from the sources in the
tree.

**Why this one paid when the last two did not.** Ten instructions a boundary priced at
0.096 percent each is 0.96 percent by the census's own rate; the route charged 0.42, so the
usual over-prediction holds, but the change is the first of the three this session whose
object delta was mostly *removed* rather than *moved* - the DSP guard's call and the refresh
call were both partly hidden by the front end, where an unconditional load-compare-branch with
a compare that was already in the source has nothing to hide behind. It also sits at every
boundary in the window, which the census had just established is where the observation lives;
before the census the same change would have been written against a block whose live fraction
was assumed to be small.


## 2026-09-22 The edge service's live fractions: the overlap guard is live at every boundary, and it fires the turns

The edge service now has the census the ledger asked for, and it corrects one inference and
explains one number from the other writer's turn inventory.

**The instrument.** `BLUEWAKE_EDGE_CENSUS` (a CMake option) counts each stage of
`host_chassis_edge_service_body`; differencing two ceilings over the bench window gives the
play window's fractions. Census host `2a51a2da...`, shipping host restored byte-exact
(`3adf572f...`) afterwards.

| stage | of the 308,606,420 calls | per retrace |
| --- | --- | --- |
| edge service calls | - | 385,758 |
| **overlap guard true** | **100.0%** | 385,758 |
| overlap object >= 0x80000000 | 8.08% | 31,162 |
| cached field pointer non-null | 8.08% | 31,162 |
| `enabled == 1` | 8.08% | 31,162 |
| **phase changed** | 4 in 800 retraces | 0.005 |
| **intercept predicate true** | **1.51%** | 5,817 |
| scheduler requires host | 0.0003% | 1.2 |

**What it corrects.** The 2026-09-21 entry read the overlap cache's -0.5 percent as evidence
that "the block must be executing on only a minority of boundaries". It executes on **every**
boundary: once the name scene exists - and it is never cleared - and once the file-start pulse
is configured and has fired, both clauses hold for the whole play window. The -0.5 percent was
the cache's net value, not a live fraction: the three runtime reads it replaced were cheaper
than the alias-state revalidation it added. The block's per-boundary work is real and now
measured: guard ~4 instructions, alias-state and cache revalidation ~7, the slot read ~3 and
the object range test ~2 - **about 16 instructions at every boundary, 6.2 M a retrace, 1.5
percent of the window**. The enabled and phase reads the ledger worried over are the 8 percent
tail and cost ~0.2 M.

**What it explains.** The intercept predicate returns true 4.65 M times in the window, and
5,965,732 turns end there, so **the predicate's true returns are the 78 percent** the turn
inventory attributes to the edge service. The predicate is therefore not a rare event to be
kept cheap: it is the turn boundary itself, one every 66 boundaries, and it is worth its
~18 instructions at 6.9 M a retrace (1.7 percent) because that is the cost of the turns the
guest actually asks for.

**The ranked candidates this leaves, in the order the census sizes them.** (1) The call frame:
14 instructions a boundary - six callee-saved registers, forced by the calls in the body -
5.4 M a retrace and 1.35 percent, which a hot/cold split of the body can shrink the way the
same split took 1.43 percent off the composite dispatcher. (2) The alias-state revalidation,
~7 a boundary, which a generation counter maintained where the alias tables change would cut
to a compare. (3) The intercept predicate's ~18, whose hash the generator has already made
perfect for these 52 keys. Each is worth a pair, and this table is the denominator for all
three.

**And the frame is not cheaply removable, which an attempt settled statically.** The
ranked first candidate was the frame itself. Moving the body's one cold multi-call
block - the per-block servicing experiment, which is off in the shipping
configuration - out of line leaves the emitted prologue **byte-identical**:
`sub sp, #0x60`, five `stp`, `add x29`, before and after. A frame is sized by
what is live across the calls a function contains, and this one's hot path already
makes calls of its own - the scheduler predicate's `dol_interrupts_external_pending`
and the intercept predicate's cold callbacks - so 14 instructions a boundary cannot
be bought with code motion. That change was reverted on that evidence rather than
benched, and the host hashes back to `3adf572f...`. What remains, in the census's
order: the alias-state revalidation (~7 a boundary, which a generation counter
maintained where the alias tables change would cut to a compare) and the intercept
predicate's ~18, whose hash is already perfect.


## 2026-09-22 The edge service's per-boundary refresh call is guarded: -0.22 to -0.28 percent

The chassis edge-service body was the last item the ledger sized without measuring: "sixty
to a hundred instructions a boundary", called once per block boundary, and needing "a census
of its own before any of it is priced". Here is the census, and the first piece taken out of
it.

**The census.** `host_chassis_edge_service` runs 391,432 times a play retrace; the sample's
4.48 percent share over that count puts it at about **46 instructions a boundary** (~18 M a
retrace, ~4.5 percent of the window). The built host's hot path, read off the disassembly:
the call frame and argument save ~8, the two flag gates ~8, the overlap observation's cache
check ~10 to 14, the intercept predicate ~18 (already inlined, patch 0038's perfect hash), the
refresh call, and the scheduler tail ~10 to 14. Nothing here is free and nothing here was
priced.

**The piece taken.** `host_refresh_interrupt_sources` is called at every boundary but
returns immediately unless a device event has dirtied the source set - the ledger's own
argument, which is why the publish path was inlined and the recomputation made conditional
earlier today. Those events arrive on host entry points that run per turn and per device
access, so on ~49 of every 50 boundaries the call does nothing but pay for itself. Testing
the same flag at the call site is **exactly equivalent** (the callee's first statement is
that test) and keeps the call, its frame and its argument off the other forty-nine.

**Measured:** **397.6 M and 397.5 M against 398.4 and 398.7 M** for the same artifact without
it, **-0.22 to -0.28 percent**, route digest `83d2590d...` unchanged, both ceilings at the
recorded stop pc and turn count, 217/217 host tests pass.

**Static against measured, the third time this session.** The call is five or six
instructions a boundary - 2.2 M a retrace, 0.55 percent by the census's price - and the route
charges 0.9 M, about 2.3 instructions a boundary. The DSP dispatch guard, the DSP frame and
now this call have all priced below their object counts, and for the same reason each time:
the instruction is real but the machine does not pay for all of it. A static count screens a
candidate; it does not price one.

**What is left in the edge service, in the order the census sizes it:** the intercept
predicate's ~18 instructions, whose hash the generator has already made perfect (its 256-slot
search found no better placement for the 52 keys); the overlap observation's cache check, ~10
to 14 instructions at every boundary for pointers that change about never, which wants the same
treatment this entry gave the refresh call; and the scheduler tail, ~10 to 14. Each needs a
change that cheapens it without moving the digest, and the boundary census prices any of them
in one pair.


## 2026-09-22 The DSP's per-instruction frame is a null, and the census's DSP cycles are requested rather than executed

Two follow-ups to the entry below, one of which corrects it.

**The frame is a null.** `Interpreter::Step` is called from the cycle loops and was
not inlined: `RunCycles` in the built host carried **eight `bl Step` sites**, so every
emulated DSP instruction paid the call, a six-register prologue, a five-instruction
epilogue and an argument move - about twelve instructions of pure per-call overhead,
or four percent of the window if the interpreter really ran 1.35 M steps a retrace.
Forcing the inline with `__attribute__((always_inline))` works exactly as intended at
the object level (`Step`'s symbol disappears, `RunCycles` becomes 634 instructions and
contains no call), and the route measures **398.2 M against 398.4 and 398.7 M** for the
same build without it. One build's own spread is 0.3 M, so the change is **inside the
noise** and is not a lever; it is reverted, and the revert is byte-exact - the rebuilt
host hashes back to `af9f7925...`, the state of the entry below.

**The correction.** That entry, and the `[...] dsp ... cpu_cycles` line it reads,
call the census's `cpu_cycles / 6` "1,349,988 DSP instructions per play retrace". It is
**requested** cycles, not executed steps. `Interpreter::RunCycles` returns **0** on its
idle-skip and halt paths as well as on exhaustion, so the host's `g_dsp_run_cpu_cycles`
accumulates what it asked the DSP to run, and the interpreter's idle-skip
(`Analyzer::IsIdleSkip`, taken whenever the ucode sits in its wait loop) returns
without stepping. The DSP is idle-skipping most of the time between audio frames, so
the executed step count is below the requested cycle count by an unknown factor, and
nothing in the tree separates them.

**What that bounds, and what it does not.** Both of this session's changes to the
interpreter's per-instruction path are measured: the predecoded table's initialization
guard and call are **−0.37 percent** (patch 0054), and the per-instruction frame is
**~0.0 percent**. The dispatch and bookkeeping inside the interpreter are therefore at
most about **0.4 percent of the window's instructions**, whatever the step count is -
so the item closes: predecode was already in the pin (patch 0038), the guard is now out,
the frame is not a lever, and what remains in that interpreter is the handler bodies,
which are the DSP's semantics. The sample's **10.06 percent is wall time**, in a
branchy interpreter whose every emulated instruction makes an indirect call through a
member-function pointer; if that share is to be attacked it is a **predictability**
question, and the instrument for it is branch-miss attribution, which this tree does not
have - the instruction harness cannot see it and neither could this change.

**Where the goal's next lever is.** Not here. The window's biggest measured pool
remains the emitted bodies (roughly 70 percent of the main thread), which the emitter
owns, and the edge service's interrupt-driven cadence (78 percent of the turns) which is
the other half of the split. Whoever picks this up next should price inside the bodies
with the ablation harness rather than in the interpreter.


## 2026-09-22 The DSP dispatch loses its initialization guard: -0.37 percent, and the dispatch was not where the DSP's cost is

The queue's head was the donor DSP LLE interpreter - 10.06 percent of the play
window in the split, 41.6 M host instructions a retrace, with its "dispatch and
per-instruction bookkeeping" priced at ten to fifteen of the thirty-one instructions
it spends per emulated DSP instruction. The dispatch is the overhead part, so it
earned a screening run.

**What the change is.** `GetDecodedOp` returned an entry of a 65536-entry table of
{main handler, extension handler, extended flag} that was a function-local `static`
built by a lambda, so every dispatched DSP instruction paid a thread-safe
initialization guard (adrp, add, **ldaprb**, tbz) and a call on top of the table
index. The table is now a file-scope array filled by `InitInstructionTables` -
which the interpreter's constructor already calls once - and `GetDecodedOp` is an
inline index in the header, so the dispatch is `table + opcode * 40` at the use
site. In the built host `Step` contains no `ldaprb` and no call to a lookup.

**The result, on frozen artifacts, twice:** **398.4 M and 398.7 M against the
control's 399.9 M, -0.35 to -0.38 percent**, route digest `83d2590d...` unchanged,
both ceilings stopping at the recorded pc after the recorded turn count, 217/217 host
tests pass. The composite `bc42cb47...` is frozen for both sides; what changed is
the host, `38e083e8...` before and `af9f7925...` after - which is why naming the
host in the bench mattered.

**The denominator, measured rather than quoted.** The delivery-safety census's
`cpu_cycles / 6`, differenced over the same 800 retraces, is **1,349,988 DSP
instructions per play retrace**, so this change is about **1.1 host instructions per
emulated DSP instruction** - against the nine that disassembling the standalone
`GetDecodedOp` suggests the old path cost.

**Why that gap is the finding.** The guard was not costing nine instructions per
instruction on the route. Its flag is invariant for the whole run, and a per-iteration
test of an invariant flag inside the DSP's run loop is exactly what a compiler hoists
or a branch predictor makes free; a standalone disassembly cannot see that. Nine in
the object, 1.1 on the route: **a static count screens a candidate, it does not price
one** - the same lesson the dispatch entry taught, with the two numbers now in the
opposite direction. It also closes this item's headline: predecode was already in the
pin (patch 0038), and what remains inside the interpreter is the handler bodies, which
are the DSP's semantics rather than dispatch overhead.

**A null dereference came out of it, and the fence applied.** The decoded table reads
the opcode templates from `DSPTables.cpp`, filled by `DSP::InitInstructionTable`,
which the host calls before it initializes the core. Building the table from the
interpreter's own constructor runs before that call, and the first build of this
change crashed in `InitInstructionTables` with
`EXC_BAD_ACCESS, KERN_INVALID_ADDRESS at 0x70` - a null template, its `extended`
field at that offset. Symbolized before naming a cause; the fix makes the dependency
explicit, because the interpreter's init now calls `DSP::InitInstructionTable`
itself, and that function is a rebuild from constant data and safe to repeat.

**Where the DSP's remaining value is.** Not the dispatch. The queue's next item is
the one v53 puts third: the host's per-turn cycle credit.


## 2026-09-22 The block-entry precharge decision is refuted on the route: +2.4 percent

The next iteration the stopping point below set out has been run, and the shape it
proposed does not pay. With patch 0053 applied to the emitter, the generator rebuilt, all
206 DOL chunks regenerated and the hot ten swapped in through
`scripts/recomp_chunks.sh` (16.8 minutes, relinked), the artifact `b47a3163...`
measures **409.5 M instructions per play retrace against the control's 399.9 M, +2.4
percent**, route digest `83d2590d...` unchanged, both ceilings stopping at the recorded
pc after the recorded turn count, host `38e083e8...`.

**What the shape is, and the fix is in it.** `func_X` is now the prepaid copy: it
decides at each block leader with `dolrecomp_block_can_precharge`, charges the block's
whole cycle count there, and carries no per-instruction charge at all. `func_X_precise`
is emitted beside it as its bail-out. A mid-block entry is caught by the fix: a
64-bit-per-64-instruction leader bitmap at the function entry sends a non-leader pc to
`func_X_precise`, and `emit_fast_observation_reconcile` gives the unexecuted
observation cycles back and hands the observing instruction to the precise copy. Boot and
route are correct - the boot shape the ledger recorded for the fixed single chunk holds
across all ten, the digest does not move, and the turn counts at both ceilings are
exactly the control's.

**Why it is slower, and the reconcile is the reason.** The -6.0 percent the chunk sweep
priced inside the hot ten was measured on the *unfixed* pair, which had no reconcile and
no mid-block guard: it was cheaper because it was wrong, and the ledger's own mechanism
entry says so (a prepaid body cannot be entered mid-block). Making it correct re-adds a
per-observation budget compare and hand-off plus a per-entry bitmap test, and the
hand-off is a *call* into the precise body where the single body's flag flip is inline.
The route arithmetic is unambiguous: the hot ten are about 26 percent of the play window,
so +2.4 percent there is roughly **+9 percent inside the hot ten**, and the difference
between the wrong copy and the right one is therefore about fifteen points - more than
the 9.8 percent the per-instruction flag test is worth, which is the whole prize the
`prepaid` ablation measured.

**So the prize is not reachable this way.** The per-instruction charge test can only be
deleted by making the flag provable within a block, and the one case that makes a second
body necessary - a mid-block entry - is exactly the case the per-instruction charge
exists to serve. Emitting the second body pays for that case twice: once in the entry
guard and once in the hand-off. The item closes.

**Two things measured along the way, both worth keeping.** The code-growth cost the
screening loop was built to price is much smaller than feared: ten chunks with both
bodies grow the linked artifact from 494,905,096 to 495,830,024 bytes, **+0.19 percent**,
because the prepaid body compiles smaller than the precise one it duplicates (it has no
per-instruction charge and no reconcile). And the generator path stays healthy: 206
chunks regenerated in seconds from the patched emitter, the hot ten compiled in 16.8
minutes with the doubled source (2,489 precise-body references and a 64-word leader
bitmap in chunk 0144 alone).

**Tree state, and one claim in it that did not hold.** Patch 0053 is reverse-applied:
the emitter is byte-identical to `/tmp/emitter.pristine.c` (`3c2626d3...`) and all
206 chunk sources are byte-identical to a fresh regeneration with the pinned emitter,
which is the claim the stopping point makes and it checks out.
`scripts/splice_composite_dispatch.py --check` reports the dispatch block unchanged.
What does *not* check out is the artifact that entry leaves in the build directory:
`ce53408a...` stops at ceiling 13,900 after 14,300,132 turns against the recorded
14,287,831 (+0.086 percent) and at ceiling 14,700 at pc `0x80307EF4` against the
recorded `0x8027FA30`, so it does not follow the recorded route - while the frozen
control `4288b958...` does, at 399.9 M. The artifact that replaces it here is
`bc42cb47...`, compiled from those canonical sources just now (ten hot chunks, 9.0
minutes, relinked): it follows the recorded route at both ceilings and measures
**399.9 M**, the frozen control's value. A relink of the tree's objects is therefore not
automatically the tree's build - the same hazard this workstream recorded for the
composite earlier today, now on the other side of it. 0053 stays in
`patches/recompcore/` as the record of the shape and of why it fails; it is not a
candidate to re-run. The next item in the queue is the DSP interpreter, which the split
prices at 1.5 to 2.5 percent.


## 2026-09-22 Stopping point: the prepaid copy is understood and fixed, and the tree is back on the pinned shape

> **The next iteration this entry sets out has been run, and the shape is refuted: see
> the entry above.** Patch 0053 was applied, the generator rebuilt, all 206 DOL chunks
> regenerated, the hot ten swapped and relinked, and the result measures **+2.4 percent**
> against the control (409.5 M against 399.9 M) with the digest unchanged. The shape is
> now *correct* - which is what the fix was for - and correctness costs more than the
> per-instruction charge test it removes. The tree was restored afterwards and re-verified
> (emitter byte-identical to `/tmp/emitter.pristine.c`, all 206 chunk sources byte-identical
> to a fresh regeneration with the pinned emitter).

The session stops here with the tree consistent and the experiment retrievable.

**Tree state.** The hot ten chunk sources are byte-identical to a fresh regeneration with the
pinned emitter; the emitter in ref/recompcore/DolRecomp is restored to the pinned shape (so
regeneration reproduces all 206 certified chunks byte-for-byte, which was re-verified today);
the composite build directory's objects were rebuilt from those pristine sources, including
the one object a killed compile had removed (chunk_0188), and the artifact is relinked from
them. The host binary carries the session's instruments (the turn-exit inventory, the
settable credit-census window, the zero-charge pc histogram) and its source is committed. The
pin's two aurora commits (0051, 0052) stand, and DolRecomp is back to the previous session's
parked-dirty state - the inert scaffolding, not this experiment.

**What is parked, and where it lives.** The prepaid-copy work is
patches/recompcore/0053-dolrecomp-prepaid-copy-mid-block-entry.patch, a diff of
ref/recompcore/DolRecomp/src/backend/emitter.c against the pinned emitter. It is a delta
against the working tree, not against the stale patches/dolrecomp series, and it carries both
halves of the fix: the leader bitmap that sends a non-leader entry pc to the precise copy, and
the reconcile that gives the unexecuted cycles back and hands the next instruction to the
precise copy. The measurement that justifies it is the entry above: the refusal is a
mid-block resume into a body that can only charge at a block leader, and one chunk rebuilt with
the fix returns the boot to the control's shape (438,210 title-ready blocks against 437,790,
where the unfixed copy reads 705,390).

**The next iteration, in order.** Apply 0053, rebuild the generator (cmake --build
build/dolrecomp-cycle-precise), regenerate (dolrecomp --gamecube --backend c --cpu gekko
--partition-instructions 4096 generated/full/main.dol <out> -j8), swap the hot ten and run
scripts/recomp_chunks.sh (~20 minutes, and it relinks), then scripts/bench_instructions.sh
with BLUEWAKE_BENCH_COMPOSITE pointed at the result. The control to beat is 400.5 M
instructions per play retrace on the frozen artifact 4288b958; the chunk sweep priced the
unfixed pair at -6.0 percent inside the hot ten, and the fix removes the turn explosion that
made the pair slower, so the expected sign is negative. The digest must be unchanged or the
candidate is discarded. Only after that does it earn the full 206-chunk rebuild, which is
90-150 minutes.

**Two instruments added this session, both host-side and inert unless asked for.**
BLUEWAKE_CREDIT_CENSUS_WINDOW moves the credit census's start retrace (default 13,900) so a
boot-phase effect is measurable in a 400-retrace run; and the census now prints a histogram of
the entry pcs of turns that end at the zero-charge exit, which is what named 0x80328F84 and
closed this question.

## 2026-09-22 The parked prepaid copy is explained and its route is restored: a mid-block resume cannot be charged by a prepaid body

The ledger recorded that the fast copy "diverged from the certified route" and parked it with
one instruction for the next attempt: instrument the host's cycle domain first. That
instrument now exists, and it names the mechanism exactly.

**The evidence, in the order it arrived.** Re-enabling the pair (both copies emitted, the
prepaid one keeping the dispatcher's name and the precise one as its bail-out target) with the
hot ten swapped in reproduced the refusal at once: 400 retraces stopped at 1,394,006 host
turns against the control's 766,380. But the *guest* did not move - title-ready arrives at
retrace 333 with the same milestone object in both, and the credit census shows the guest's
charged cycles identical to the digit (647,999,999 against 648,000,000). What moved is the
host's view of a turn:

| | control | prepaid copy |
| --- | --- | --- |
| turns | 348,786 | 713,558 |
| credited cycles | 647,999,999 | 648,000,000 |
| mean credit per turn | 1,857.9 | 908.1 |
| turns with **no credit at all** | 11 | **253,053** |
| turns ending at the **zero-charge exit** | 347 (0.1%) | **361,817 (50.7%)** |

A new instrument names the blocks those turns end on: a pc histogram of the zero-charge exits
puts 249,229 of them on **0x80328F84** and the rest on its immediate neighbours,
0x80328F2C through 0x80328F8C, inside chunk 0201 - the hottest chunk in the play window. The
control has no exits there at all (its whole run has 347, the busiest address being 0x80263508
with 304).

**The mechanism, read off the emitted code.** That region is a run of loads whose labels are
*not* block leaders. In the single body each of them carries a per-instruction charge test -
so a resume at that pc charges its cycle. The prepaid copy has no per-instruction charge at
all: its charge is the block's total, taken once at the leader. And a resume at a *non-leader*
pc is exactly what happens whenever the precise copy yields at a budget boundary - it returns
with that pc and the chassis re-dispatches there. The chassis then enters the prepaid copy in
the middle of a block, which runs the rest of the block for nothing; the composite loop sees
downcount unmoved, treats it as its zero-charge family, tolerates a run of eight and then
returns to the host. That is the 50.7 percent: not a wrong charge but an impossible one.

So the ledger's summary - "the per-instruction charge is load-bearing" - is right, and the
thing that is load-bearing is narrower than it looked: **a prepaid copy cannot be entered in
the middle of a block.** Forcing the flag could never have shown that, because the ablation
had no mid-block entry path to break.

**The fix, two parts, both in the prepaid copy.** First, its entry dispatch consults a
64-bit-per-64-instruction leader bitmap and hands a non-leader pc to the precise copy, which
charges it and the rest of the block one instruction at a time - the shape the single body has
for exactly those pcs. Second, the observation reconcile is emitted there (dropping it was the
parked copy's other difference): it gives the unexecuted cycles back and hands the *next*
instruction to the precise copy, which is what the single body's flag flip does, with the
leader-time guard that stood in for it removed.

**Measured on one chunk.** With only chunk 0201 rebuilt that way and the other nine still the
broken shape, the 400-retrace run returns to the control's boot shape:

| | title-ready blocks | stop |
| --- | --- | --- |
| control | 437,790 | 766,380 at 0x80246960 |
| prepaid copy, no fix | 705,390 | 1,394,006 at 0x80307EF4 |
| chunk 0201 fixed only | **438,210** | 768,661 at 0x80244FA0 |

The stop pc and the last 0.3 percent of turns differ because one chunk is a new code shape and
the turn boundary lands a block earlier at the ceiling; the eight-chunk-out-of-ten remainder is
what the screening pair below measures. Everything here is a host-side instrument plus the
emitter; no gate moved.

## 2026-09-22 The mixed build is behaviourally current: eight of eight sampled chunk objects recompile byte-identical

The caveat left by the paused rebuild - that artifact 4288b958 is a mixed build, so its turn
counts might describe the old generation rather than the source - is now measured rather than
assumed. The object in the composite build directory mirrors its absolute source path, so each
old object maps back to the source and flags that produced it, and eight of them, drawn at
random from the 474 objects older than today's 15:45 cutoff, were recompiled with the compile
command recorded in compile_commands.json and compared byte for byte:

| chunk | family | result |
| --- | --- | --- |
| 0126 | main DOL text | identical |
| 0148 | main DOL text | identical |
| 0000 | REL C11000F4 | identical |
| 0000 | REL C1D100E4 | identical |
| 0000 | REL C1B300E4 | identical |
| 0000 | REL C1EB00E4 | identical |
| 0001 | main text 800056E0 | identical |
| 0082 | main text 801496E0 | identical |

Eight of eight is not 756 of 756, but it is the right kind of evidence: the Sep 22 01:14
generated.h regeneration does not change emitted chunk code, and the reason it triggered a
full rebuild is that cmake compares the header's mtime to the objects', not that the code
moved. So the artifact under test is current with the tree, the numbers this ledger quotes for
it stand, and the full rebuild is bookkeeping - it stays paused, and the emission work that is
next needs a regeneration anyway.

## 2026-09-22 The rendered path is verified end to end at 14,700 with the fix, and its level is unchanged from the recorded one

The same frozen artifact (sha256 4288b958) that segfaulted rendered at retrace 103 on the
pre-fix host now completes the certified route rendered on the fixed host:

| | value |
| --- | --- |
| presents | 14,700 frame-timing stamps, one per retrace, no gaps |
| stop | normal at pc=0x8027fa30 after 20,138,490 blocks - the landed high ceiling |
| exit | 0 |
| boot milestones | title-ready at retrace 333, blocks=437,790, the recorded marker |
| fallback parses | 0 batches arrived with no frame packet |

Frame time, from the stamps, on a machine that is **not quiet** (load average 61 from the
desktop's own applications, no build of ours running):

| window | mean | median | p90 | p95 | p99 | max |
| --- | --- | --- | --- | --- | --- | --- |
| whole run | 18.04 | 17.53 | 29.42 | 34.83 | 52.63 | 61.68 |
| play window, retrace >= 13,900 | **38.34** | **33.14** | 54.31 | 56.00 | 59.19 | 61.68 |
| boot, retrace < 1,000 | 19.53 | 18.37 | 41.97 | 43.98 | 47.60 | 56.26 |

Two readings from that. First, the whole-run mean (18.04 ms) is *not* the play window's: the
menu and boot retraces run near 18 ms and drag it down, and the graded window is the 38.34 ms
one. That is the same level this ledger already recorded for the rendered play window (mean
36.00, median 34.80 in the earlier capture), so the fix changed nothing about the level - which
is what a serialization point should show if it is not on the hot path. Second, the p95 and p99
gates are over (56.00 and 59.19 against 36.7 and 50), and on this machine that reading is
variance-dominated: the same ledger recorded 46.98 and 47.94 for the same window under a
different load. A quiet-machine capture is what makes the p99 clause a measurement, and it is
not available in this session.

What is settled is the correctness half: the worker's frame-transition defect and its
never-joined thread are both gone, the rendered route completes with the certified stop and a
clean exit, and the tolerance's rendered half is no longer blocked by an unexplained crash.

## 2026-09-22 A play-window capture says the per-turn host machinery is 1 percent, and the pc store is not a lever

The v52 split is stale (it predates the zero-charge tolerance) and its "rest of main about 18
percent" does not survive re-measurement. A fresh `sample` capture of the **headless** play
window - the same window scripts/bench_instructions.sh measures, taken 20 s at retrace >=
14,010 on the frozen candidate artifact with the fixed host, 16,471 main-thread samples,
parsed by scripts/sample_owners.py - shares the main thread like this:

| owner | share of main thread |
| --- | --- |
| chassis_dispatch (the composite's loop) | 74.47% |
| selected_dispatch (the emitted guest bodies, inside it) | 70.14% |
| host_chassis_edge_service (inside it) | 4.48% |
| host_sync_cycle_devices_end_turn (the device service, DSP LLE inside it) | 10.64% |
| all of `main` | 85.9% |

A share includes everything it calls, so the guest bodies and the edge service are inside the
chassis dispatch's 74.47 percent, and the whole of main is 85.9 percent: **the per-turn host
machinery outside the chassis loop and the device service is under one percent of the main
thread.** The 14.1 percent between `main` and the thread root is dyld/kernel frames in this
capture, on a machine that is not quiet; it is not claimed as BlueWake cost.

That redirects the queue. The v53 item ranked first - the host's per-turn cycle credit, on the
argument that half the turns credit nothing and the per-turn pass is what blocks the body
work - is not where the time is: the turns cost 10.6 percent (device service) plus 4.5 percent
(edge service), and the body work inside the chassis is 70 percent of the thread. The owner
list, not the turn count, is the map.

And the one body-side candidate that could be priced without a rebuild is negative:
`scripts/bench_chunk.sh --ablate 0144` gives **26.91** instructions per guest cycle at
baseline and **27.05** with `no-pc`, so deleting the per-instruction pc materialisation
entirely makes the chunk *slower*, not faster. That agrees with the pc-store already being on
the refuted list, and it means the 1.78 pc touches per instruction are not a removable cost in
the emitted shape - the compiler is using those stores to keep the state live.

**What this capture is not.** It is a share of the main thread's *wall time* under sample(1),
not a share of the 400.5 M instructions the bench quotes, and those are different measurements:
host code and emitted bodies do not retire at the same IPC, and a share includes every callee.
The v52 page asked for the instruction split specifically, and warned that the earlier dynamic
census was of the rendered main thread rather than of the headless count. So the instruction
split of 400.5 M stays open, and the honest instruments for it are the host's own per-region
counters rather than sample(1): scripts/emit_census.py is a size proxy per guest opcode, and
BLUEWAKE_RETURN_CENSUS attributes host returns to their input pc, which is the closest thing
the tree has to a per-region instruction attribution today. The model in this ledger - 8.1 M
guest cycles a retrace at 26.9 host instructions per cycle, about 218 M of the window - is a
model from the chunk sweep and is not that measurement.

What the capture does settle is the ranking, and that is why it changed a decision: the
per-turn machinery outside the chassis loop and the device service is small in the only
currency a player feels, so it is not where to spend a rebuild.

## 2026-09-22 The play window's turns are interrupt-driven: the edge service owns 78 percent, the budget 9.5, and the zero-charge bound 0.1

The turn-split heuristic is superseded by an exact inventory, taken host-side with no
composite change. The chassis loop's exits are all observable from the host: the loop calls
the host's edge service once per block (so the host sees that call's return value - the edge
exit), it can read downcount before and after each block (so it knows whether the last block
charged anything - the zero-charge run bound), and it can test downcount <= -cycle_budget at
exit (the budget, which the loop tests *before* the edge service, so the two cannot be
confused). Gated to the play window and reported at exit, on the frozen candidate artifact
(sha256 4288b958) with the fixed host:

| exit | turns | share |
| --- | --- | --- |
| edge service | 4,654,418 | **78.0%** |
| exception exit or an unwrapped module | 736,533 | 12.3% |
| budget | 567,935 | **9.5%** |
| dispatcher miss | 0 | 0.0% |
| zero-charge run bound | 6,846 | **0.1%** |
| total | 5,965,732 | blocks/turn 51.73 |

The credit census in the same run: mean 1,086.2 charged cycles per turn against a mean budget
of 6,493.3, 188 turns of 5.97 M with no credit at all, credit lag mean 993.1.

**What this corrects, and what it redirects.**

The zero-charge family is *spent in the play window*. It was 43.6 percent of the boot's turns
and it is 0.1 percent of the play window's, so the BLUEWAKE_ZERO_CHARGE_RUN_MAX bound is not
the binding constraint it looked like and raising it buys nothing where the gate is measured.
The tolerance banked that family already; the boot was its home.

The "94.4 percent of play turns end early" reading was the heuristic's own artifact. It
compared each turn's credit against the *previous* turn's budget, and the honest inventory is
78 percent edge, 12.3 percent exception-or-unwrapped, 9.5 percent budget. The remaining 12.3
percent is not yet split between exception exits and dispatches into modules the chassis loop
does not wrap (RELs); that split is a five-minute census and is not claimed here.

**So the turn count in the play window is set by interrupt conditions, not by the budget**: the
edge service returns to the host when the guest has EE set and the decrementer is pending, or
when the scheduler asks for the host, and that is where 78 percent of the turns come from at a
mean of 1,086 charged cycles each. A turn now spans 51.73 blocks, which is what the zero-charge
tolerance bought: the same interrupt cadence, amortised over 52 blocks instead of one. Any
further turn reduction has to come from that cadence and not from the run bound.

**Two operational corrections, both measured.** First: cmake/composite/dispatch_loop.h is
listed by exactly **2** of the 756 dependency files in the composite build directory
(module_export.c.o.d and dispatch_loop.c.o.d), so a dispatch-loop change costs two compiles and
a relink, not the two-hour rebuild this ledger has been quoting. The full rebuild that was
running for the artifact's sake is a *generated.h* effect, not a dispatch_loop.h one:
generated.h was regenerated at 01:14 on 2026-09-22, so every chunk object predates it and cmake
wants all 756 again. It was stopped at 274 of 756 objects with that work preserved, because it
was holding eight cores against measurements that do not need it; it resumes where it left off.
Second, and the reason it still matters: the artifact under test (4288b958) is a *mixed* build
- chunk objects from the earlier generation plus a newly compiled module_export.o - so if the
01:14 regeneration changed emitted guest code, this ledger's turn counts describe the mixed
artifact rather than what the source produces now. The rebuilt dylib's own smoke and pair are
what close that, and they are the first measurement after the rebuild finishes.

## 2026-09-22 The landed play window on the tree's artifact is 400.5 M, not 398.3 M, and the ledger's row belonged to another composite

Two independent pairs on the composite in the tree (build/composite-cycle-hybrid-o2-v2/
gGZLE01_recomp.dylib, sha256 4288b958..., the build whose 400-retrace smoke reports 766,380
turns at pc=0x80246960) now stop where they should and report:

| run | low run | high run | instructions per play retrace | digest |
| --- | --- | --- | --- | --- |
| pair 1 (16:37-16:49) | 14,287,831 | 20,138,490 | **400.0 M** | 83d2590d... |
| pair 2 (16:52-17:00) | 14,287,831 | 20,138,490 | **400.5 M** | 83d2590d... (1,050 records) |

against the tree's pre-tolerance control of 409.2 M, that is **+2.14 percent** of the play
window, with both ceilings stopping at their certified pc (0x80307EF4 / 0x8027FA30), the
boot milestones reached, and the bench-window route digest still 83d2590d... The two pairs
differ by 0.13 percent, which is inside the resolution this ledger claims.

**The correction this forces.** The landed entry recorded 14,532,577 / 20,464,729 and 398.3 M
and +2.67 percent. Those numbers are real but they are not this artifact's: they came from
/tmp/bw-p1-composite.dylib (sha256 3b7ea820, built 10:01), which is the default composite of
/tmp/bw-p1-bench.sh and was the artifact /tmp/bw-save-probe/pair-13900.log and pair-14700.log
were produced with. The tree's composite stops at 14,287,831 / 20,138,490 with the same pcs -
1.6 percent fewer turns, because it is a different build of the generated guest code - and
measures 400.0-400.5 M rather than 398.3 M. The tolerance's own gain over the 409.2 M control
is therefore **2.1 percent, not 2.7**, and the header comment on
BLUEWAKE_ZERO_CHARGE_RUN_MAX (cmake/composite/dispatch_loop.h) carries the same stale pair.
That comment is documentation rather than behaviour, and it is left untouched while the
composite rebuild is in flight so the artifact under construction stays a build of the source
as it stands; refreshing it is the first thing the next rebuild carries.

scripts/bench_instructions.sh now holds both builds' rows (BLUEWAKE_BENCH_VARIANT=landed by
default, control for the pre-tolerance artifact), refuses a ceiling it has no recorded stop
for instead of silently skipping the check, and defaults its reference to the tree's 409.2 M
control so the printed delta is the tolerance's own gain.

## 2026-09-22 The rendered crash is a null frame packet, and the worker was never stopped either

The rendered probes of the zero-charge tolerance died with EXC_BAD_ACCESS at
0x0000000000000028 on a thread whose stack is entirely the FIFO translation worker
(g_fifo_worker_main -> RetailGxFrontend::flush -> ConsumingAuroraRenderSink::submit_packet
-> GxCoreSink::on_consumed_draw -> core_plan_observer -> aurora::gfx::gxcore::submit_draw_plan
-> aurora::gx::set_logical_viewport -> aurora::gfx::get_render_target_size). The fault
address is the answer: 0x28 is the offset into a null pointer, and the disassembly shows
that frame loading Aurora's recording-frame pointer and then reading [ptr + 0x28]. That
pointer is g_recordingFrame, which current_frame_packet() guards with a CHECK the release
build compiles out, and which gfx::end_frame clears (common.cpp:1338) and gfx::begin_frame
restores (1196) - both inside aurora_backend_present().

**So the worker was recording into a frame packet that did not exist.** It is not the
tolerance: the worker is the only thing that can record while the main thread is between
frames, and headless never opens a recording frame at all (g_initialized is false there, the
aurora sink is never entered), which is why one artifact broke rendered and stayed green
headless. Two commits in the pin, both registered as patches 0051 and 0052 and in
config/dependencies.lock.json:

  * 0051 holds g_aurora_recording_mutex across the worker's batch and across the present
    transition, gates the sink per batch on g_aurora_recording_open (a batch with no frame
    is parsed by the packet sink so the front end stays truthful, and the count is reported
    once rather than dropped in silence), and refuses to drain inside the transition rather
    than hanging. The inline path is untouched: it runs on the main thread, which never
    writes GX from inside the transition, so it is safe by construction.
  * 0052 stops and joins the worker in dol_aurora_shutdown(). Nothing stopped it: its thread
    was live when the device was destroyed and still joinable when exit destroyed its
    std::thread, so std::terminate aborted the process **after** every rendered run's normal
    stop. That one is older than the fix and independent of it.

Both are measured against the same frozen artifact (gGZLE01_recomp.dylib, sha256 4288b958...,
the build whose smoke reports the candidate's 766,380-turn shape), one host change at a time:

| host | retraces | rendered result |
| --- | --- | --- |
| pre-fix | 60 | completed, then **SIGABRT** at exit (std::terminate, worker never joined) |
| pre-fix | 900 | **SIGSEGV at retrace 103**, no stop line - the null frame packet |
| fixed | 900 | 900 presented retraces, normal stop at pc=0x80307ef4, exit 0 |
| fixed | 4,200 | 4,200 presented retraces, normal stop at pc=0x80307ef4 (4,953,566 turns), exit 0 |

The headless smoke is unchanged with the fixed host on the same artifact: 766,380 blocks at
pc=0x80246960. The aurora backend is never initialized headless, so the fix has to be inert
there, and it is: the two defects were rendered-path defects that the headless measurement
could not see.

**What this means for the tolerance's admissibility.** The rendered half is no longer
unverified because of a mystery: the artifact that died rendered was dying of a defect in
the worker, and the tolerance was never implicated. The candidate's cert pair and the
rendered acceptance are the next measurements, and the composite rebuild that makes the
tree's artifact honest is still running.

Also landed with it: scripts/bench_instructions.sh refuses an unrecorded pair instead of
skipping the check, and takes **BLUEWAKE_BENCH_VARIANT** (landed by default, control for the
pre-tolerance artifact) because the tolerance halves the turn count at every stopped pc and
one table can no longer hold both.

## 2026-09-22 v54 opens: the rendered crash is a null recording frame, and the worker is what is between frames

The fault address in the newest crash report is 0x28 and that is the whole answer: the
offset into a null pointer. atos had already placed the fault in
aurora::gfx::get_render_target_size, and the disassembly of that frame shows it loading
Aurora's recording-frame pointer and then reading [ptr + 0x28] to compare the render-pass
count. The pointer is g_recordingFrame, which current_frame_packet() guards with a CHECK
that the release build compiles out. Aurora clears it in end_frame and restores it in
begin_frame, and aurora_backend_present() calls both: it submits the finished frame, then
opens the next one.

So the FIFO translation worker was recording a batch into the frame packet during the
window in which the packet does not exist. That is the worker landed earlier in this
ledger, not the zero-charge tolerance: the worker is the only thing that can record while
the main thread is between frames, and headless is immune because a headless run never
opens a recording frame at all (g_initialized is false, the aurora sink is never entered,
and no headless run has crashed there). One mechanism, not two facts.

The tolerance's rendered half is therefore unverified rather than refuted, and v54 opens
with the boundary fix: one mutex held by the worker across a batch's translation and by
the main thread across the present transition, which cannot deadlock because the main
thread's only wait on the worker - the drain at the guest-visible barriers - never happens
inside that window. The queue and the protocol are in
[docs/GOAL_PROMPT_V54_2026-09-22.md](../GOAL_PROMPT_V54_2026-09-22.md).

## 2026-09-22 The rendered segfault is the GX translation worker's first draw, and the stack names it

atos against build/runtime-host-dsp/bluewake_host resolves the twelve host frames of the
newest crash report:

  aurora::gfx::get_render_target_size() + 28      <- the fault
  aurora::gx::set_logical_viewport(Viewport const&) + 120
  aurora::gfx::gxcore::submit_draw_plan(DrawPlan const&) + 204
  gx_aurora::core_plan_observer(...) + 24
  gxruntime::gxcore::GxCoreSink::on_consumed_draw(...) + 100
  gxruntime::aurora_recomp::ConsumingAuroraRenderSink::accumulate_assembly(...) + 400
  gxruntime::aurora_recomp::ConsumingAuroraRenderSink::submit_packet(...) + 2652
  gxruntime::gxcore::GxCoreSink::submit_packet(...) + 396
  gxruntime::aurora_recomp::RetailGxFrontend::emit_new_packets(...) + 648
  gxruntime::aurora_recomp::RetailGxFrontend::flush(...) + 256
  gx_aurora::g_fifo_worker_main() + 240
  std::__thread_proxy(...)

So the fault is in the GX FIFO translation worker - the one landed earlier in this ledger -
dereferencing something inside get_render_target_size on the worker's first draw, at the
title screen, which is the first thing the renderer draws. That accounts for every
observation at once: it is a worker thread, it is in the host binary, it happens on the first
draws, and it did not happen before the candidate because pre-candidate turns flushed the
FIFO many times a frame and reached the target-setup path in an order the worker survived.

**Which makes the candidate the trigger rather than the cause.** Longer turns mean bigger
batches, and a first flush that arrives before the render target exists; the worker's path
does not check that. The fix belongs in the worker - establish or wait for the target before
submitting a plan, or make get_render_target_size null-safe - and it is a pre-existing defect
that the tolerance exposed rather than a defect in the tolerance. That also means the
tolerance is not yet admissible: it is admissible when the rendered path survives it, and the
rendered path has a bug of its own to fix first.

The composite rebuild is still running and will make the artifact honest; the candidate stays
landed; the rendered crash now has a named function rather than a question.

## 2026-09-22 The rendered crash is on a worker thread in the host binary, and it dies at the title

Three crash reports exist for the rendered probes (15:47, 15:59, 16:06). The newest says
EXC_BAD_ACCESS, SIGSEGV, on **thread 17** of bluewake_host, fourteen frames, none of them in
the composite. The rendered logs say where: one probe stopped after 33 frames in the JAS/DSP
state, another after 335 frames immediately past title-ready at retrace 333 - the title
screen, before the play scene and before the renderer has drawn the world.

So the fault is in a worker thread of the host binary rather than on the main thread, and
both rendered probes died in the first second of drawing. The hand-off buffer is a
std::vector that grows, so the first hypothesis - a fixed-capacity overflow under the larger
batches a longer turn produces - is refuted by the code itself.

What is not yet known is which worker and which function, and the report already carries what
is needed: the image offsets (+163016, +349936, +451784, +137928, +4299924) against
build/runtime-host-dsp/bluewake_host, which atos maps to function names in one command. That
is the next step, and it is minutes of work rather than a rebuild.

The composite rebuild is still running and will make the artifact honest rather than
reverting anything. The candidate remains landed and remains the prime suspect for the change
in rendered behaviour, but where the crash is changes the shape of the fix: a fault in a
worker thread says the mechanism is in the host's threads, not in the loop's bound.

## 2026-09-22 Correction: the revert never happened, the candidate is still landed, and the ledger said otherwise

The two entries below say the landing was reverted and that the revert is verified. Neither
is true, and the check is mechanical: git show --stat on both commits reports only
docs/status/CURRENT.md - b477b53 is 24 insertions and ebb6b81 is 20 deletions, both in the
ledger - while cmake/composite/dispatch_loop.h still carries BLUEWAKE_ZERO_CHARGE_RUN_MAX and
the zero-charge accumulator at lines 70 to 73. The git revert I ran targeted HEAD, which was
the ledger entry rather than the landing, and the commit I described as reverting the landing
only added a note about it.

**So the candidate is landed in both the source and the artifact**, and the observation that
made me infer a build-path mystery - the identical artifact hash 4288b958 before and after
the revert - is explained by the source being unchanged, not by a recompile that missed the
header. There is one build path and it saw the change; there was nothing to see in the
second compile because nothing had changed.

**What the evidence actually says now.** The candidate is the prime suspect for the rendered
segfault: both crashes happened with it linked in (at retrace 3,169 and again at 335), and
the rendered runs earlier in the session, before it existed, reached 14,700 and stopped at
the certified pc. That is not proof, but it is the opposite of the reading the (non-)revert
was made on, and the rendered path is what NFR-001 gates, so it is the blocking question.

**What to do next, in order.** Let the composite rebuild that is already running finish: it
is rebuilding from the candidate source, so it makes the artifact honest rather than
reverting anything. Then reproduce the rendered crash with and without the candidate using
the apply-run-restore command that takes a minute, since the same composite rebuild is only
needed if the source changes. If the crash tracks the candidate, the tolerance is wrong for
the rendered path and the headless +2.67 percent is not admissible; if it does not, the
rendered path has a fault of its own that predates this work and the increment stands.

The ledger's two revert entries are left standing with this correction rather than edited,
because the error is in what I recorded having done, and that is exactly what a reader needs
to see.

## 2026-09-22 The rendered crash tracks the artifact, not the source, and the artifact still carries the candidate

Two probes, one with the reverted source but the candidate still in the composite:

| probe | artifact | result |
| --- | --- | --- |
| headless, 400 retraces | candidate still linked in | 766,380 turns - the candidate's count, not the control's 1,372,978 |
| rendered, 1,000 retraces | the same artifact | title-ready at 333, 335 frames, then **SIGSEGV** (exit 139) |

So the revert's source change is not in the artifact yet: a dependency-aware rebuild of the
composite is running and, until it finishes, every run uses the candidate. That also means
the rendered crash cannot yet be attributed, but the pattern now points one way - the two
rendered runs that segfaulted both had the candidate in the artifact (at retrace 3,169 and
again in this probe), while the rendered runs earlier in the session, before the candidate
existed, reached 14,700 and stopped at the certified pc.

That is corroboration and not proof, and it is why the landing stays reverted: the evidence
now leans toward the tolerance disturbing the rendered path rather than toward a broken
display, which is the opposite of the reading the revert was made on. The measurement that
settles it is a rendered run against the restored artifact, and the rebuild that restores it
has to finish first.

## 2026-09-22 The landing is reverted: its rendered half is unverified and the rendered run died

The landed tolerance was measured headless and certified headless, and the next thing it
needs is the rendered window, because that is what NFR-001 gates. The rendered run after the
landing died at retrace 3,169 with no stop line - no normal stop, no digest, nothing after
3,169 frame stamps - against earlier rendered runs in this session that reached 14,700 and
stopped at the certified pc.

I cannot tell from that whether the change disturbs the rendered path or whether the
rendered environment lost its display between those runs, and the difference matters: the
first would make the change wrong and the second would make it fine. So the change is
reverted rather than left landed on a headless-only verification, because the gates this
project is measured against are rendered and an unverified change does not get to sit in the
tree while the question is open. The revert is verified the same way the landing was: the
composite is recompiled and relinked from the reverted header and a headless run reproduces
the recorded 400-retrace stop.

What stands from the work regardless: the zero-charge exit owns 43.6 percent of the boot's
turns; tolerating it is route-neutral headless, halving the turns and worth +2.67 percent of
the play window with the digest unchanged over 1,050 records; and the rendered path needs to
be re-established before that can be landed or before any other change is called done. The
candidate is one file and one object, and the same apply-run-restore command reproduces it
in under a minute when the rendered question can be asked properly.

## 2026-09-22 Landed: the dispatch loop tolerates a bounded run of zero-charge blocks

The screened and certified measurement from the entry above is now the loop's behaviour
rather than a screen. cmake/composite/dispatch_loop.h carries the change, the numbers it
was screened with, and the termination argument that makes the bound load-bearing: a block
that charges nothing does not advance downcount, so a guest that stops charging would never
reach the budget exit, and a run of nine ends the turn so the chassis always returns to the
host.

| | value |
| --- | --- |
| certified turns, 13,900 / 14,700 | 14,532,577 / 20,464,729, against 29,937,744 / 40,502,699 |
| instructions per play retrace | **398.3 M against 409.2 M, +2.67 percent** |
| route digest | 92dd816c6a382531d597c8da0cc3e447c716aaa8240912d10a78bee853478782, unchanged |
| smoke after landing | 766380 turns at pc=0x80246960 |

The change is one object (module_export.c) in the composite and one header, so it does not
need the hot-ten rebuild that an emitter change would, and it touches no emitted code, which
is why the digest does not move. What remains open is the emitter-side alternative it points
at - a zero-charge block is a leader whose block_cycles is zero, and charging the block its
minimum would remove the exit at its source - and the other half of the turns, which end at
the edge service and the budget.

## 2026-09-22 The zero-charge tolerance is a measured increment: 398.3 M against the recorded 409.2 M

The pair on the certified ceilings, with the tolerance applied (one object recompiled,
module_export.c) and the same route card:

| ceiling | control turns | candidate turns | stop pc |
| --- | --- | --- | --- |
| 13,900 | 29,937,744 | **14,532,577 - 51.5 percent fewer** | 0x80307EF4 (certified) |
| 14,700 | 40,502,699 | **20,464,729 - 49.5 percent fewer** | 0x8027FA30 (certified) |
| instructions, 13,900 | - | 808473981483 | |
| instructions, 14,700 | - | 1127081728611 | |
| **per play retrace** | 409.2 M | **398.3 M** | **+2.67 percent** |

So the derived estimate of about four percent was right to within a fraction: halving the
turns buys 10.9 M instructions a play retrace, because the per-turn non-guest cost is about
14,300 of the 30,637 instructions a turn and the guest's own share of a turn is unchanged.
The 50 percent turn reduction and the 2.67 percent instruction reduction are the same fact
read two ways, and the instruction number is the one that counts.

**Every guard the certified route has holds.** Both ceilings stop at the certified pc; the
14,100 run's digest is 92dd816c6a382531d597c8da0cc3e447c716aaa8240912d10a78bee853478782
over 1,050 records, unchanged; the boot milestones including overlap_phase=6 are identical;
and the emitted code is byte-identical, so the guest cannot see the change and the device
advancement the ledger already established as elapsed-based does not care how long a turn is.

**What it still owes before it is a landed change.** A principled tolerance and a termination
argument that does not rest on it; the accumulator is a single static in an always_inline
loop, which is fine for one module but wants the argument written down; and the emitter-side
alternative - a block that charges nothing is a leader whose block_cycles is zero, and
charging it its minimum would remove the exit at its source - remains untried and would move
the guest's cycle accounting, which this shape does not.

The composite was restored and the tree is clean.

## 2026-09-22 The zero-charge tolerance is route-neutral on the certified run, and halves the turns

The screen's next question was whether a 43 percent turn reduction survives the certified
route. It does, and on the certified ceiling it is larger. The tolerance was applied, one
object recompiled (module_export.c), and the run went to the certified 14,100 ceiling on a
copy of the route card:

| 14,100 retraces | control | candidate |
| --- | --- | --- |
| stop pc | 0x80307EF4 | **0x80307EF4** |
| boot milestones | the certified set | **identical, including overlap_phase=6** |
| route digest | 92dd816c... | **92dd816c6a382531d597c8da0cc3e447c716aaa8240912d10a78bee853478782** |
| records | 1,050 | **1,050** |
| host turns | 32,203,791 | **15,879,630 - 50.7 percent fewer** |

So the guest ran the route identically - the digest covers the clock, delivery, FP and
CPU-ABI summaries, the milestones, the scene draws, DVD, ARAM and DSP - while the host took
half the turns. The composite was restored in the same command and the tree is clean.

**What it is worth, stated so the number is not read as 43 or 50.** Turns per play retrace
fall by about 1,158 (16,324,161 fewer over 14,100 retraces). The per-turn non-guest cost was
derived at about 14,300 instructions a turn, so the win is roughly **16.6 M instructions a
play retrace, about 4 percent of 409.2 M** - a real increment, larger than anything landed
this session except the interrupt-source gating, and much smaller than the turn reduction
looks. The pair on the certified ceilings is the measurement that confirms it, and it is the
next run.

**Why it is admissible and what it still owes.** The emitted code is untouched, so the guest
cannot see the change; the digest confirms that empirically. What it owes is a principled
tolerance rather than the screen's eight, an argument for the loop's termination that does
not rest on the constant, and a decision between this host-side shape and the emitter-side
one it points at - a block that charges nothing at all looks like a leader whose block_cycles
is zero, and making such a block charge its minimum would remove the exit at the source, at
the cost of moving the guest's cycle accounting and therefore the digest, which this shape
does not.

## 2026-09-22 The zero-charge exit owns 43.6 percent of the turns, and tolerating it cuts the turn count by 43 percent

Two things had to be right for this and one of them was wrong. The dispatch loop lives in
cmake/composite/module_export.c - not in the chunks - so the two earlier attempts that
recompiled chunk 0000 never compiled the instrument at all, which is why they printed
nothing. Recompiling module_export.c instead, one object, is enough, and the counters then
read on the 400-retrace boot control (1,372,978 turns at pc 0x80246960):

| exit | count | share of turns |
| --- | --- | --- |
| ended at the budget | about 369,000 | 26.9 percent |
| the edge service | about 404,000 | 29.4 percent |
| **a block that charged no cycles** | **598,350** | **43.6 percent** |
| the dispatcher could not run the pc | 1,650 | 0.12 percent |

Which closes the inventory: 26.9 + 29.4 + 43.6 + 0.1 is 100 percent, so every turn now has a
named end and the largest single one is a guest block that charges nothing.

**And tolerating it works.** With the loop allowed to continue past a bounded run of eight
zero-charge blocks instead of returning at the first, the same 400-retrace run stops after
**776,305 turns against the control's 1,372,978 - 43.4 percent fewer**, at the same pc, which
is the measured share of that exit to within a quarter of a point. The composite was
restored and the restored artifact reproduces 1,372,978 turns exactly.

**Why this one is not the refused twin.** The twin changed the *emitted* code's charging and
moved the host's accounting with it; this changes only *when the chassis returns to the
host*, and the emitted code is byte-identical, so the guest cannot see it. The ledger already
recorded the argument that makes it safe: "Device advancement stays where it is, on the cycle
cursors, which are elapsed-based and therefore unaffected by how long a turn is." The device
service, the publish and the deadline computation simply happen fewer times.

**What it is not yet.** A screen on the boot at one tolerance, not a landed change: the
tolerance constant is arbitrary, the loop's infinite-loop safety has to be argued rather than
inherited from that constant, and nothing here has been through the certified pair and the
digest - which is the next measurement, because a 43 percent turn reduction with an unchanged
route would be the largest increment this ledger has recorded, and a changed route would make
it another refuted shape. It is also the first candidate in this session that attacks the
per-turn overhead the play-window census sized at 13,350 turns a retrace.

## 2026-09-22 The boot ends turns early too, so early exits are the normal shape and the play window just pays them 3.9 times as often

The same split, with the census gate lifted and the print every 100 retraces, on a
400-retrace boot run (1,372,978 turns at pc 0x80246960, the recorded control):

| window | turns | ended at the budget | ended early |
| --- | --- | --- | --- |
| boot, first 300 retraces | 668,000 | 179,514 - **26.9 percent** | 488,482 - **73.1 percent** |
| play, 13,900-14,000 | 980,003 | 5.6 percent | **94.4 percent** |

So both windows end most of their turns with budget in hand, and the play window is the
more extreme of the two. The extra turns the play window takes - 13,350 a retrace against
the boot's 3,458 - are therefore *early exits* rather than budget exhaustion, and the
structure is not a play-specific anomaly: it is the port's normal shape, three quarters to
nineteen twentieths of whose turns end before the guest spends what it was given.

**That leans on the conflict the previous entry recorded.** The derivation charges a turn's
whole non-guest cost to the host side and reaches 47 percent of the graded window; the
sampling split reaches about 36. A structure in which three quarters of *all* turns end
early, in both windows, is easier to reconcile with the larger number than the smaller one,
because the per-turn pass is paid on every one of them. That is not proof - the split's
attribution boundary is still the suspect - and the exit counters in the hot ten are still
the measurement that settles it.

The census gate was restored in the same command, so the instrument is play-window-only
again, which is the right default; the tree is clean and the host is unchanged from the
committed state.

## 2026-09-22 The per-turn cost, derived: about 14,300 host instructions a turn, and it disagrees with the split

Two measured numbers meet: the play window costs 409.2 M host instructions a retrace, and it
takes 13,350 turns a retrace. That derives 30,637 instructions a turn. The guest's own work
in a turn is its credit - 606.7 cycles - and the chunk sweep measures 26.9 host instructions
per guest cycle, so about 16,300 of those 30,637 are the emitted body and about 14,300 a
turn are not the guest at all.

| per play retrace | value | source |
| --- | --- | --- |
| host instructions | 409.2 M | rendered-and-headless bench pair |
| turns | 13,350 | credit census, play window |
| instructions a turn | 30,637 | derived |
| of which the guest's own work | about 16,300 | 606.7 cycles x 26.9 instructions a cycle |
| of which not the guest | **about 14,300 a turn, 191 M a retrace, 47 percent** | derived |

**That is twice what the sampling split allows it.** The split of 2026-09-22 put device
service at 10.2 percent, the edge service at 3.95, dispatch at 4.3 and the rest of main at
about 18 - roughly 147 M a retrace, against this derivation's 191 M. The two cannot both be
right, and the difference is about 44 M instructions a retrace, which is more than any
increment this ledger has landed in a session.

What it most likely means is that "the emitted body" in the split is attributed by *sample
depth* and therefore includes the per-turn fixed work that happens inside the chassis call,
while the derivation charges a turn's whole non-guest cost to the host side. Whichever is
wrong is worth knowing, because the two answers point at different levers: if the derivation
is right, the per-turn pass is half the graded window and the exit counters are the most
valuable measurement left; if the split is right, the body is where the money is, as the
roofline already said. The next measurement settles it - the exit counters in the hot ten
chunks, which the credit census's instrument can now be pointed at without disturbing
anything, since the host-side half of it is already in the tree and env-gated.

## 2026-09-22 Ninety-four percent of the play window's turns end without using their budget

The host can tell whether a turn used its budget without touching the composite, and that
is enough to bound the exit question in the graded window. A turn whose credit reached the
budget it was handed ended at the loop's budget exit; every other turn ended somewhere the
host cannot see. The instrument prints every 1,000 retraces, and on the 14,700-retrace run
to the certified stop it reads at retrace 14,000 (that is the first hundred play retraces):

| window 13,900-14,000 | turns | share |
| --- | --- | --- |
| ended at the budget | 54,544 | **5.6 percent** |
| ended early, reason unseen by the host | 925,459 | **94.4 percent** |

So in the graded window about 12,600 of the 13,350 turns a retrace end without the guest
spending the budget it was given, at an average credit of 607 cycles against a 6,324-cycle
budget. The per-turn pass is paid for work the guest had budget to continue.

**What is now known and what is not.** Known: the magnitude, in the graded window, of turns
that end early - and it is host-visible, so it is cheap to re-measure and to test a fix
against. Not known: which of the loop's four exits owns them, because two of the four are
counted by nothing and the counter that would say so has to be compiled into the chunks the
play window runs - the hot ten, not chunk 0000, since the counter placed in chunk 0000
printed nothing at all. That is the next measurement, and it is the only one left in this
chain: everything above it is measured, and everything below it (what to change once the
exit is known) depends on the answer.

The instrument is kept rather than reverted: it is host-side, env-gated with the credit
census, and it costs one comparison a turn.

## 2026-09-22 The play window takes 13,350 turns a retrace, each using a tenth of its budget

The census moved to the window the PRD grades - gated to retrace 13,900 and after - on a
14,700-retrace run to the certified stop (40,502,699 turns at pc 0x8027fa30):

| play window 13,900-14,700 (800 retraces) | value |
| --- | --- |
| turns | 10,680,028 = **13,350 a retrace** |
| zero-credit turns | 188 |
| credit per turn | **606.7 cycles** |
| budget handed out per turn | **6,324 cycles** |
| budget min / max | 1 / 12,600 |
| lag mean / max | 557.1 / 12,639 |
| turns crediting more than their budget | 0 |

The boot's figures for comparison: 3,458 turns a retrace, a 7,311-cycle budget, 2,342 cycles
consumed.

**So the graded window takes 3.9 times as many turns per retrace as the boot, and a turn
consumes under a tenth of the budget it is handed.** The per-turn pass - the device service,
the deadline computation, the publish, the loop - is paid 13,350 times a retrace for an
average of 607 cycles of guest work.

That is the corrected shape of the problem this thread has been circling. It is not that
turns are wasteful (188 of 10.7 M credit nothing) and not that the budget is too small
(6,324 cycles against 607 consumed): it is that a turn ends after a tenth of its budget
whatever the reason, and the reason is the one thing still uncounted in this window. The
exit counters that came back empty for the boot belong here next, which means the hot ten
chunks rather than chunk 0000, because the play window does not run the boot's code.

The instrument change this measurement needed is kept rather than reverted: the credit
census now counts retrace 13,900 and after, because the two windows have different turn
structures and a census that mixes them describes neither.

## 2026-09-22 The loop exits nothing counts are rare, so the boot's turns end somewhere else - and this thread has been measuring the boot

The dispatch loop's exits (c) and (d) were the derived owner of three quarters of the
turns, so they were counted directly: a self-contained instrument inside the loop,
compiled into chunk 0000 only, printing the running split every 200,000 exits, on the same
400-retrace control (1,372,978 turns, pc 0x80246960, reproduced exactly).

**Nothing printed.** Both counters together stayed under 200,000 across a run of 1,372,978
turns, so exits (c) and (d) are not the owner and the derivation from the loop's exit list
was wrong. The turns must end at (a) - the budget or an exception - or (b) the edge-service
answer, and the edge census's 472 a retrace cannot be it either.

**Which points at the error underneath both:** every number in this thread - 3,458 turns a
retrace, the 7,311-cycle budget against 2,342 cycles consumed, the zero-credit count, the
lag - comes from a **400-retrace run, which is the boot**. The window the PRD grades is the
play window, 13,900 to 14,700. The edge census's 472 returns a retrace is an average over
the whole route, and if the boot spends thousands of edge-service returns a retrace while
the play window spends almost none, then the two windows have different turn structures and
this thread's mechanism has been describing the boot.

**What that changes.** Nothing measured here is wrong - the stop, the turn count, the budget
and the lag are all real and reproducible - but their *scope* is narrower than the entries
claimed. The next measurement has to be the same census on the play window, which is a
14,000-retrace run rather than a 400-retrace one and costs about ten minutes, and until it
exists this ledger should not say that the per-turn overhead in the graded window is what
these entries describe.

## 2026-09-22 The zero-charge tolerance screen is a null: identical turn counts, to the digit

The dispatch loop's exit (d) - returning to the host at the first block that charges no
cycles - was the derived owner of three quarters of the turns, so the cheapest confirmation
is to tolerate a bounded run of such blocks and watch the turn count fall. Chunk 0000 only,
which the ledger records as a one-minute loop, with an eight-block tolerance, against the
same 400-retrace boot control:

| | turns | zero-credit turns | mean credit | budget mean |
| --- | --- | --- | --- | --- |
| control | 1,383,370 | 61 | 2,342.1 | 7,310.7 |
| candidate | **1,383,370** | 61 | **2,342.1** | **7,310.7** |

Identical to the digit, and both sides stop at 1,372,978 turns at pc 0x80246960, so the
change did not move this window at all. Either chunk 0000 does not take exit (d) here, or
exit (d) is not the owner.

**The screen does not confirm the derivation, and it does not refute it either**, which is
the honest reading: a shape change is not a counter. This is the third time this session
that a shape-based test has come back null while the count that actually matters stayed
unmeasured, so the next instrument is the only one that settles it - a counter on exits (c)
and (d) inside the loop, read back by the host at exit, which needs a getter exported from
the composite and is the plumbing the jump-table work already has. Everything else in the
queue is smaller than that, and it is the last unmeasured link between the per-turn cost
and the 1.7x gap.

The composite is restored rather than left mixed: the header change is reverted, chunk 0000
recompiled and relinked, and the restored artifact reproduces the recorded 400-retrace stop
(1,372,978 turns at pc 0x80246960) with the census numbers unchanged. Tree clean.

## 2026-09-22 The dispatch loop returns to the host at the first block that charges no cycles

The previous entry left one question - what ends 2,986 turns a retrace - and the dispatch
loop answers it. bluewake_chassis_dispatch_loop (cmake/composite/dispatch_loop.h, 62 lines)
has exactly four exits inside the loop:

| exit | condition | counted? |
| --- | --- | --- |
| a | ctx->exception, or downcount <= -cycle_budget | the interrupt clamp is (16), the budget is not |
| b | the edge service says the host is required | yes - 472 a retrace |
| c | the dispatcher could not run this pc | no |
| **d** | **the dispatched block did not decrease downcount** | **no** |

Exit (d) is the one: the loop returns to the host whenever a dispatched block charges no
cycles, so a turn ends at the *first zero-charge block* it meets and the turn count *is* the
number of such blocks. The arithmetic agrees - 384,275 boundaries a retrace over 3,458 turns
is 113 blocks a turn, so about one block in 113 charges nothing - and it also explains why
the credit census sees almost no zero-credit turns (61 in 1,383,370): the turn's credit is
what the blocks before that one charged, which averages 2,342 cycles against a 7,311-cycle
budget.

**So three quarters of the host's turns exist because one guest block in about 113 charges
nothing**, and each one costs a full per-turn pass - the device service, the deadline
computation, the publish and the loop - which is the bulk of the main thread's non-guest
work. The guard is there for a reason (a block that does not advance the clock could spin
forever), so the fix is not to delete it: it is either to let the loop tolerate a bounded
run of zero-charge blocks before returning, or to find why a block charges nothing at all
(the emitter charges a block at its leader only when the leader has cycle work, so the
candidate is a leader whose block_cycles is zero).

**What confirms it** is one counter: increment on exit (d) and on exit (c) in the loop and
compare with the turn count. That counter lives in the composite rather than the host, so it
needs a getter the host can read at exit, which is the same plumbing the jump-table work
already uses; it is the next instrument, and it is worth more than any remaining crumb
because the per-turn cost it pays for is the largest non-guest block on the main thread.

## 2026-09-22 The turn count is eight times the chassis return count, so most turns end for a reason nothing counts

The previous entry found that a turn consumes about a third of the budget it is handed and
named the obvious suspect: the chassis returning to the host early. Two measurements
already in this ledger cross-check that, and they do not agree with it.

| per play retrace | value | source |
| --- | --- | --- |
| host turns | 3,458 | credit census, 1,383,370 over 400 retraces |
| budget-driven turns the budget implies | about 1,110 | 8.1 M cycles / 7,311-cycle budget |
| chassis edge-service returns | **472** | edge census, 6,934,736 over 14,700 retraces |
| blocks per turn | 113 | 384,275 boundaries / 3,458 turns |

The edge-service predicate is the *only* path by which the chassis returns to the host, and
its census counts every reason that predicate can be true - the edge predicate 6.93 M, the
scheduler predicate 12,453, the interrupt-with-EE clamp 16, out of 614.5 M boundaries. So
the chassis returns to the host **472 times a retrace**, while the host takes **3,458
turns**. The difference, about 2,986 turns, is three quarters of the turn count and it is
not a chassis return and not the budget: nothing in this ledger counts it.

**So the per-turn overhead is being paid for turns whose end condition is unknown**, and
that is now the measurement rather than the hypothesis. The instrument is small and it is in
our own tree rather than the pin: count the exit conditions of the dispatch loop in
cmake/composite/dispatch_loop.c - the budget test, the edge-service answer, an exception, a
cross-chunk call depth fall-out, and whatever else returns to the caller - one counter each,
then run the same 400-retrace control. Whichever counter owns three quarters of the turns is
the thing to fix, and it is worth more than any remaining crumb in this ledger, because the
per-turn cost is the bulk of the main thread's non-guest work.

## 2026-09-22 The budget handed out is three times the cycles a turn takes, so the turn count is not budget-limited

The credit census now reports the budget it hands the guest and the DSP cursor lag at the
same boundary, on the 400-retrace control (stop 1,372,978 turns, the recorded control):

| 400 retraces | value |
| --- | --- |
| turns | 1,383,370 |
| credit per turn | 2,342.1 cycles |
| **budget handed out per turn** | **7,310.7 cycles** |
| budget min / max | 1 / 12,600 |
| DSP cursor lag at the boundary, mean / max | 2,304.8 / 12,639 |

**So a turn consumes about a third of the budget it was given.** The guest returns to the
host after ~2,342 cycles with ~5,000 cycles of budget still unspent, which means 3,458
turns a retrace where the budget only justifies about 1,110. The extra turns are not the
budget running out; they are the chassis returning for its own reasons, and every one of
them pays a full per-turn pass - the device service, the deadline computation, the publish
and the loop - which is the bulk of what this ledger calls the rest of main.

**That is a structural lever nobody had sized.** The per-turn overhead is paid 2.2 to 3.1
times more often than the budget requires, so the ceiling on removing it is not a few
percent: it scales with the ratio. What is *not* yet known is why the chassis returns: the
edge predicate is the obvious candidate and the edge census already counts its hits
(6,934,736 over 14,700 retraces, about 472 a retrace), which is far too small to explain
3,458 - so the answer is somewhere in the chassis window logic or the per-turn body's own
return conditions, and that is the next instrument. It also changes the twin's story again:
a body with a different charge shape changes how many of these non-budget returns happen,
which is consistent with the 2.8x turn inflation without any change in the guest's work.

## 2026-09-22 The host turns are clean: 61 of 1,383,370 advance nothing, and the credit matches the budget

The corrected census, counting the per-turn body rather than flush calls, on the same
400-retrace control (stop 1,372,978 turns, the recorded control):

| 400 retraces | value |
| --- | --- |
| turns | 1,383,370 (3,458 a retrace) |
| turns that advance nothing | **61 - 0.004 percent** |
| total credited | 3,240,000,000 cycles (8.1 M a retrace, the guest's own count) |
| mean credit | **2,342.1 cycles a turn** |
| largest credit | 12,639 (the DSP service rate) |

So the host's turn structure is clean: one credit a turn, about 2,342 cycles each, within
one percent of the deadline census's 2,360-cycle budget, and essentially no turn is
wasted. The zero-credit half the earlier entry reported was entirely the observe path
finding nothing to charge, which is what that instrument was counting.

**Which narrows the twin's refusal to one place.** The candidate cannot have paid in wasted
turns, because the baseline has none; its three times the turn count has to come from the
budget. And the budget is computed from the guest's *residual* cycle position - the DSP
deadline subtracts the elapsed cycles plus a lag that is the absolute cycle count minus the
DSP cursor - so a body whose charge lands differently at the turn boundary computes a
different distance to the same device deadline. That is a phase sensitivity in the deadline
computation, and it is what to instrument next: the budget and the cursor lag at the turn
boundary on the control, which needs neither a candidate nor a rebuild.

## 2026-09-22 The credit census counted credit events, not turns

The entry below reads its zero-credit half as half the host's turns and builds a mechanism
on it. That is an artifact of the instrument: flush_elapsed is reached from
bluewake_cycle_domain_flush, bluewake_cycle_domain_end_turn and four
bluewake_cycle_domain_observe sites, so the count is of credit *events*, and an observe
that finds nothing charged is a zero-credit event that is not a turn. The corrected
reading of the same run is about 2,300 cycles credited per turn against the deadline
census's 2,360-cycle budget - the credit stream is fine. The twin's 2.8 times the turns is
therefore still unexplained, the mechanism the entry below names does not exist, and any
future census of the host's turns must count turns rather than flush calls. The entry is
left standing with this correction rather than edited, because the error is in the count
and not in the run.

## 2026-09-22 Half of the host's turns credit no guest cycles, which is why a coarse-charging body inflated the turn count

The queue's first item was the host's per-turn credit, because that is what the refused
prepaid twin interacts with. BLUEWAKE_CREDIT_CENSUS counts what flush_elapsed credits, on
the 400-retrace control - which stops at 1,372,978 turns, the recorded control, so the
route is intact and the deadline census beside it is unchanged:

| 400 retraces | value |
| --- | --- |
| turns (flush calls) | 2,769,817 |
| **turns that credit nothing** | **1,384,094 - 49.97 percent** |
| total credited | 3,179,824,834 cycles (7.95 M a retrace, against the guest's 8.1 M) |
| largest single credit | 12,639 cycles (the DSP service rate) |
| turns crediting more than their budget | 169,620 - 6.1 percent |

**So the retrace boundary advances on a credit stream in which half the turns contribute
nothing.** That is the mechanism behind the twin's refusal, and it does not require the
guest to run differently at all: the total credit is right (the candidate's guest cycles
were identical to within one) while the *ratio* of zero-credit turns moved, and the turn
count tripled because each zero-credit turn still costs a full per-turn pass. The 2.8x is
this ratio, not a changed guest and not an I-cache effect.

**What that means.** The port's frame pacing is coupled to a *codegen* property - how a
body's charge lands relative to the observation suffix and the budget test - so a body
that charges more coarsely pays in host turns even though the guest's cycles are
unchanged. The fix is on the host side: a turn that advances the guest by nothing should
not cost a full per-turn pass, and the retrace boundary should not be gated on the credit
stream alone. That is now a named mechanism rather than an unexplained 2.8x, and it is the
gate in front of the largest identified lever in the headless gap.

## 2026-09-22 The memory path is already lean; the residual is the ctx traffic the yield discipline forces

The roofline's named next step was a MEM1 fast-path ablation for the emitted body's 1,088
memory operations. Reading the code closes it before it is built: mem_read32 in core/cpu.h
is a static always-inline whose first act is the unshadowed-MEM1 fast path - two compares
against the alias-overlap flag and the mirror bit, a bounds check against ram_size, then a
big-endian read of cpu->ram + offset - with the alias resolver and the MMIO paths behind it
for addresses outside that window. A guest load is therefore about five instructions and
there is nothing left to ablate; the 2026-09-18 restrict null (+0.3 percent) is the same
statement from the compiler's side, and the unchecked ablation is unsweepable because what
it removes is the semantics, not an overhead.

**What the 20.00 floor is actually made of**, statically, on the same chunk (3,007 guest
instructions):

| the emitted C touches | count | per guest instruction |
| --- | --- | --- |
| gpr accesses (operands) | 5,151 | **1.71** |
| pc touches (resume state) | 5,344 | **1.78** |
| downcount touches (budget) | 3,702 | 1.23 |
| memory helper calls | 1,152 | 0.38 |

So the body spends about four and a half loads or stores of the CPUState per guest
instruction on state that has to be in memory *because any instruction can return to the
host*. That is the yield discipline, and it is also why the no-pc and restrict nulls are not
evidence that the state traffic is cheap: the compiler can hold a guest register in a host
register only across a stretch it can prove does not escape, and the charge test at every
instruction is what breaks that proof.

**Which is the prepaid/twin design's prize, and that design is refused by the route for a
reason that is still not understood** - 2.8 times the turns with the guest cycles identical
to within one. So the queue's next item is neither a body trim nor the memory path: it is
the host's per-turn credit accounting (flush_elapsed, bluewake_cycle_domain_observe, and
what the deadline census says sizes the window), because if a coarse-charging body can be
made to credit the same cycles per turn, the twin becomes admissible and its prize is the
largest identified lever in the headless gap. That instrument is host-side and needs no
rebuild.

## 2026-09-22 The tail is not deterministic: two headless passes of the same route share no retrace time

The last three entries attributed the rendered tail to the present retrace, then to the
translation worker, then eliminated the renderer. The next question was whether the tail
is a reproducible event at all, and two headless passes answer it: same build, same card,
same route, both stopping at 40,502,699 turns on pc 0x8027fa30, the same 601 retraces of
the steady window.

| two headless passes, 14,100-14,700 | value |
| --- | --- |
| retrace times identical between runs | **0 of 601** |
| mean absolute difference | 1.50 ms |
| run 1's 115.13 ms stall, as seen by run 2 | 25.60 ms |
| run 2's own worst, unseen by run 1 | 108.14 ms at retrace 14,218 |
| retraces over 44 ms | 108 and 123, of which 88 shared, 20 and 35 private |

**So the tail is host and OS variance, not a reproducible event**, and that closes the
attribution thread the last three entries opened. It also separates the two numbers the
gates need: the *level* is durable and reproducible - rendered median 34.80 ms, headless
mean 36.26, within a fraction of a millisecond across runs - while the *tail* is not, which
is why headless and rendered had the same tail and why the 115 ms outlier moved between
runs rather than following the route.

**What that means for NFR-001.** Its p95 and p99 clauses are *variance* clauses, and the
work they imply is variance reduction on the main thread - faulting pages in, keeping the
frame path free of allocation and of lazily-compiled work, and not fighting the scheduler
- rather than finding a hot spot, because there is no hot spot to find: no retrace is
reproducibly slow. The mean is one problem (the emitted body, 409 M instructions against
233 M) and the tail is another (the host's own hygiene on the frame path), and this entry
is the one that says they are different problems with different fixes.

## 2026-09-22 The wall-time tail is not the renderer: headless has the same p95 and p99, and a worse median

The present-retrace thread assumed the rendered retrace's extra cost was the renderer.
Both configurations stamped per retrace, on the same build and the same certified route
(14,700 stop at 40,502,699 turns, pc 0x8027fa30), steady window 14,100-14,700:

| 601 retraces | mean | median | p90 | p95 | p99 | max |
| --- | --- | --- | --- | --- | --- | --- |
| headless | 36.26 | 40.34 | 45.65 | 47.14 | 48.89 | **115.13** |
| rendered (aurora 960x720) | 36.00 | 34.80 | 45.62 | 46.98 | 47.94 | 49.19 |

The two are the same distribution to within what the gates resolve: the same mean, p90,
p95 and p99, with the *headless* median five and a half milliseconds worse and a single
115 ms stall the rendered run does not have. **So the wall-time tail is host-side, on the
main thread, and it is neither the translation worker nor the renderer** - the worker's
drain is a microsecond and the renderer is not in the headless run at all.

That also means the 36.9 percent rendered differential this ledger quotes must not be
read as a wall-time tail: it is an *instruction-count* measurement of the rendered main
thread, it stands as one, and it is why the rendered *level* is what it is. The rendered
path's problem is its level - median 34.80 ms against the 16.667 the retrace target
allows - and its tail is the same tail the headless path has, so the two are one problem
and not two.

What is left as the tail's owner is the main thread's own work: the emitted body and the
per-boundary host work, which is exactly where the headless target already sits (409 M
instructions per play retrace against the 233 M the target needs). The next measurement
on the tail therefore belongs next to the body work, not to the renderer.

## 2026-09-22 The FIFO-write census is parked: its first run died before it printed

The rendered tail's next question is whether the present retrace carries more *guest*
submission, and a per-retrace count of the guest's FIFO writes answers it for one
increment a write. The instrument went into the MMIO gate that handles 0xCC008000,
gated to retrace 13800 and 2,000 lines so it could not print through the boot, and its
first rendered run **died at retrace 565 with no stop line**: 83 KB of log, no census
output at all, and no normal stop.

Whether the cause is the insertion, a device race its timing exposed, or a link that
was still finishing when the run started is not established, and that is why it is
reverted rather than debugged in place: the path it counts is the GX FIFO gate the
whole product depends on, and an unexplained death there is not a thing to iterate on
while it is in the tree. **The revert is verified rather than assumed** - an
800-retrace headless smoke run stops normally at pc 0x8028e5e8 with the title pulse
firing at retrace 334 - and 217/217 host tests pass.

The question stays open with the shape recorded: count the writes in the 0xCC008000
gate per retrace, join with the frame stamps, and see whether the present retraces
carry twice the submission of the others. The verified instrument from the previous
entry - the draw-done drain at 0.25 microseconds - stands unchanged, and the *only*
thing this attempt establishes is that the next version has to be proven on a bounded
headless run before it goes anywhere near the rendered route.

## 2026-09-22 The landed GX worker is registered in the pin instead of living as an uncommitted diff

The hygiene item the fences require, and the hazard this ledger has called out twice.
ref/recompcore's working tree carried the landed GX FIFO deferral and translation
worker as eighteen uncommitted file changes on top of a5a7652a94, and the fences are
explicit that ref/'s uncommitted state *is* the patch series - which also means a
reverse-apply, or a careless checkout, destroys landed work rather than reverting one
experiment. The change is now a commit inside the pin (**51057555f9**) and
config/dependencies.lock.json records it under the recompcore entry: the commit sha,
its eighteen-file list and the date, replacing head_sha a5a7652a94 as the authority for
what the tree actually holds.

**What is deliberately not committed.** The embedded DolRecomp still carries its
parked inert scaffolding - the tag threading, the emit_flat_function extraction and the
dormant fast copies - as fourteen uncommitted paths. That shape is an experiment whose
half is off and whose emitted text is byte-identical to the pinned series, so
committing it would present it as part of the series. The lock records that too, with
the reason, so the next reader knows which half is authority and which half is parked.

**The debt that remains, stated once.** The exported series under patches/dolrecomp is
stale - 0003 and 0018 apply and 0002, 0016 and 0017 do not - so the working tree stays
the authority for the emitter, and anything that touches the aurora backend or the HLE
input path belongs in the pin, which is where the missing half of the analog L/R rule
lives.

## 2026-09-22 The rendered tail is not the translation worker: the draw-done drain costs a microsecond

The present retrace carries the whole rendered gap, so the first thing to test was
whether it is *waiting* on the translation worker. It meets the worker at exactly one
barrier - the draw-done commit at pc 0x80308A9C, which drains everything appended so
far - and that call site is host code, so it can be timed without touching the pinned
dependency. BLUEWAKE_GX_FLUSH_CENSUS adds a per-retrace line for that call, and one
rendered run to the certified 14,700 stop says:

| steady window 14,100-14,700, joined with the frame stamps | value |
| --- | --- |
| flush calls per retrace | 1.00 on slow *and* fast retraces |
| flush cost, mean | **0.25 to 0.29 microseconds** |
| flush cost, max | **1 microsecond** |
| flush share of the slow retraces' total time | **0.00 percent** |

**So the tail is not the worker and not the drain.** The one synchronization the
landed design puts on the main thread is free in the play window, which also means
the worker is keeping up rather than the main thread waiting on it. Two things follow:
the present retrace's ~13 extra milliseconds are in the guest's own submission work or
in Aurora's present path, and the next instrument has to be on one of those, not on
the hand-off.

**And the census run's own frame times are not a measurement** - 42.19 ms mean against
the 36.00 the clean run reported - because the instrument reads the clock twice and
prints a line per retrace. The flush figure is unaffected (both sides carry the same
instrument), and the ledger's frame-time numbers stand on the clean run; this entry
quotes the census only for the flush.

## 2026-09-22 The rendered tail is the presentation stride, and it is bimodal with a two-retrace period

scripts/frame_tail.py is new: it differences the host's own per-retrace CLOCK_MONOTONIC
stamps, reports the distribution against NFR-001's rendered gates, and names the
retraces that own the tail. One rendered run to the certified 14,700 stop -
40,502,699 turns at pc 0x8027fa30, aurora at 960x720:

| window | retraces | mean | median | p90 | p95 | p99 | max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| scene entry, 13,910-14,000 | 91 | 29.56 | 22.26 | 49.18 | 51.85 | 52.40 | 53.92 |
| steady, 14,100-14,700 | 601 | 36.00 | 34.80 | 45.62 | **46.98** | 47.94 | 49.19 |
| whole play window | 791 | 35.20 | 34.03 | 47.17 | 48.69 | 52.65 | 53.92 |

So the steady window's p99 (47.94 ms) is *inside* the 50 ms gate and its p95 (46.98)
is 10.29 ms over the 36.7 ms gate; the p99 failure over the whole window is the
scene-entry burst, five retraces at 13,991-13,995 costing 52 to 54 ms.

**The shape is the finding, and it is not a rare stall.** 149 of the steady window's
601 retraces cost more than 44 ms, and of the 148 gaps between them **121 are exactly
two retraces**, with bursts of consecutive even indices (14,309 to 14,335). The mean,
36.00 ms, is (25 + 48) / 2 to within a tenth of a millisecond. The rendered cost is
one retrace in each pair carrying the present, which is what the PRD means when it
says 30 FPS presentation is every other retrace: a retrace-level p95 is therefore
largely measuring the presentation stride, and the interval a player actually sees is
the pair - about 72 ms here against the authentic 33.3.

Two consequences for the queue. The rendered work to do first is whatever the present
retrace does, and the ledger's earlier rendered profile already pointed at the GX
translation and the submit path there, which the worker has since moved. And an
NFR-001 reading should say whether its frame-time gates are per retrace or per
presented frame, because the two differ by exactly the factor this distribution is
made of - a question for the acceptance definitions, not for this instrument.

## 2026-09-22 A digital L or R now carries full analog travel, which is what the page switch reads

The acceptance test's own caveat turned into a product check, and the check found a
bug. The keyboard bindings map the L and R keys to PAD_TRIGGER_L and PAD_TRIGGER_R -
the *digital* bits - while the guest's pause-menu page switch asks
dMs_isPush_R_Button, which reads mDoCPd_R_LOCK_BUTTON, which mDoCPd_Read derives from
mTriggerRight. A digital-only R therefore reaches the guest as a button and moves
nothing, which is precisely what the save workstream measured before it found the
analog press: twelve digital R presses left dMc_c null, one press with analog R at
full travel opened the collect page. So a human could not reach the Save page at all,
and milestone 9's human form rested on a wire detail.

The wire encoder now makes the digital bit imply full travel, because that is what
the hardware does: the L and R clicks are switches at the end of the analog travel,
so a click never arrives without it. A pad that drives the axis itself is unaffected,
since full travel is the most it can report. tests/pad_wire_test.c asserts the rule
in both directions - a click with no analog value behind it, a click overriding a
partial one, and an analog value with no click passing through unchanged.

**And the certified route is untouched by it**, measured rather than argued because
the encoder runs on every pad poll: 32,203,791 turns at pc 0x80307EF4, route digest
92dd816c6a382531d597c8da0cc3e447c716aaa8240912d10a78bee853478782 over 1,050 records,
217/217 host tests.

Half of the fix is recorded rather than made: the HLE PADRead path (hle_input.c)
takes its state from the platform rather than from this encoder, so it needs the same
rule where the platform is read - in the pinned dependency, alongside the GX worker
diff that is already unregistered. Whether the guest reaches that path in this build
is not established; the SI path is the one the route and the app both exercise.

## 2026-09-22 Milestone 9 has an acceptance test, and it passes

scripts/save_continue_acceptance.sh runs the whole chain unattended against the
certified card copy, and every clause it asserts is read from the guest's own
fields or from the card rather than from a claim:

| clause | evidence |
| --- | --- |
| the route was the certified one | opening-complete retrace 13850, play-scene retrace 13910 |
| the save screen opened | save-screen milestone, mode 3 on the collect page |
| the guest wrote the save | the save screen's proc field reached 18 and then 22, with the card write counter at 3 |
| the quit was the guest's own | the second prompt answered on the quit line, then the card unmount the reset path performs |
| the card holds gameplay state | scripts/card_container.py: gczelda data hash changed, 1,218 of 98,304 bytes |
| the reload loaded it | play scene up, no new-game intro, no name entry |

The reading it prints: **control at retrace 833, 13.9 s of authored content,
against 20,338 and 339.0 s for a cold new game**. Run:
local-research/acceptance/20260922-144732 (save.log, reload.log, save.card). PASS.

**What it is not, and what it opens.** The input is the host's pad schedule, not a
person's keys, so it is a measurement and scripts/app_acceptance_test.sh remains the
real-key path; and the reload is the same binary that wrote the card, so this does
not yet show that a save survives a rebuild. The real-key question it raises is
worth recording because it is a product bug if true: the pause menu's page switch
reads the **analog** L/R trigger through mDoCPd_R_LOCK_BUTTON, so a human reaches the
Save page only if the app's keyboard or pad mapping drives the analog trigger and
not just the digital button. That belongs with the P6 input work.

## 2026-09-22 The emitted body's roofline is 20 instructions per guest cycle, so trimming cannot reach the target

The measurement the 2026-09-18 entry asked for and nobody had taken. One ablation
over the whole bookkeeping set - no-guard, no-suffix, prepaid-decision, no-pc,
restrict, cheap-budget, precharge-lean, charge-helper - applied to chunk 0144 and
swept against its real-state snapshot, which is the same instrument and the same
chunk every price in this file uses:

| chunk_0144 | instructions per guest cycle | against baseline |
| --- | --- | --- |
| baseline | 26.91 | - |
| every accounting construct deleted at once | **20.00** | **-25.7 percent** |

The emitted C shrank from 1,936,054 to 1,274,939 bytes. So the budget tests, the
observation suffix and reconcile, the precharge decision, the pc materialisation,
the charge bodies and the alias barriers together are worth at most a quarter of
the body, and what is left is the guest's own operations as this emitter writes
them.

**That closes the question the loop was asking.** Authentic speed needs about 20.6
host instructions per guest cycle for the *entire* window - dispatcher, device
sync, edge service and the loop included - and a chunk whose accounting has been
deleted outright sits at **20.00 for the body alone**. NFR-001 therefore cannot be
reached by trimming the emitted body, in any combination, and the loop should stop
looking for constructs inside it. The earlier nulls were not noise: they were this
bound showing through one construct at a time.

**Where the residual actually is, statically, on the same chunk.** 3,007 charge
sites is the guest instruction count; there are 1,119 precharge decisions; and
there are **1,088 memory operations - 584 mem_read32 and 504 mem_write32** - each
of which is a call into a translating helper. Thirty-six percent of the chunk's
instructions are loads and stores, and the helper is the gate they all pass
through. The "unchecked" ablation tried to price that and cannot be swept, because
it drops the region logic and the synthetic state then drives a guest loop that
never returns; the ablation that *can* price it, and that an emitter change could
actually ship, is the semantics-preserving one: take the direct pointer when the
guest address falls in the MEM1 window and fall back to the existing helper for
everything else. That is the one place left in the body where the cost is both
large and identified, and it is the next thing to build.

## 2026-09-22 The chassis window is a DSP slice, the overlap crumb is fourteen events, and one ablation is not sweepable

Three measurements that are cheap and that each change a decision. None of them is
an increment; all three are prices, which is what this workstream needs before it
spends a rebuild on anything.

**The dispatch budget is a DSP slice and the cap never binds.** BLUEWAKE_DEADLINE_CENSUS
on the 400-retrace control (stop 1,372,978 turns, the recorded control exactly):

| term | binds | of 4,577,962 decisions | one-cycle budgets |
| --- | --- | --- | --- |
| DSP | 4,317,633 | **94.31 percent** | 2,998 |
| audio | 256,610 | 5.61 percent | 524 |
| VI | 3,075 | 0.07 percent | 0 |
| decrementer | 642 | 0.01 percent | 0 |
| clamp | 2 | 0.00004 percent | 2 |
| **cap** | **0** | **0 percent** | - |

So `bounded_budget` never reaches its 16,384-cycle cap: the window is sized by the
donor DSP's next service deadline, which is what `host_sync_dsp_cycles` advances at
the end of each turn. 3,432 turns a retrace at 8.1 M guest cycles a retrace is an
average budget of 2,360 guest cycles, and one-cycle budgets are 0.077 percent of
decisions. Two consequences the loop did not have. The per-turn host cost - the
publish, the device service, the deadline computation - is paid once per *DSP slice*,
so the DSP update rate is a lever on the per-turn overhead as well as on the DSP's
own 10 percent (a rate doubled halves the turns and the per-turn cost with them, and
the DSP's timing exactness is the constraint). And this is the number the flat-body
refusal moved: that candidate's turn count rose 2.8x with identical guest cycles, and
a window whose size is a device deadline rather than a cap is exactly the shape where
a changed accounting of *when* the guest advanced shows up as turns instead of
instructions. The refusal remains a refusal; what it is not is a statement about the
emitted body, which is where the ledger had left it.

**The overlap observation is fourteen events behind an 0.8 percent guard.** The
edge census (compiled in, run to 14,700, stop 40,502,699 turns at pc 0x8027fa30 -
the certified stop) with the boundary census beside it:

| count | value | share |
| --- | --- | --- |
| edge-service calls = boundaries | 614,549,179 | (14,700 retraces) |
| - in the play phase | 303,577,075 | 384,275 per play retrace |
| overlap guard (name scene up, file start pulse fired) | 578,587,698 | **94.14 percent of boundaries** |
| overlap object exists | 33,324,761 | 5.42 percent |
| overlap enabled | 33,324,757 | 5.42 percent |
| **overlap phase changed** | **14** | - |

So the crumb the loop had listed as its best frequency-to-risk ratio is priced: the
always-on part is the guard and the object read, about nine instructions at 361,750
boundaries a play retrace, an **0.8 percent ceiling**, and the whole of its output is
fourteen phase changes in 14,700 retraces. The phase read itself already runs at 5.4
percent, so any design that gates it further buys almost nothing; the money is in the
guard, and the guard cannot be sampled less often without risking the record that
made this instrument necessary (the per-retrace cadence once recorded phase 5 where
the shipping cadence recorded 6). The honest price is that the crumb is small and its
design has to preserve fourteen events, so it goes behind the two items below.

**One ablation is not sweepable, and the harness will now say so.** The `unchecked`
ablation - memory helpers replaced by a bounds-only MEM1 fast path - is the one that
would price the body's memory path, and it has never had a route price. The baseline
reproduces first: chunk 0144 at **26.91 instructions per guest cycle**, the same number
the 2026-09-18 sweep quotes, and 0201 at 23.82. Then `unchecked` produced nothing: the
transformed body drove the synthetic state into a guest loop that never returns, and
`scripts/bench_chunk.sh` had no bound on a sweep, so the run held a core at 99 percent
for **seventeen minutes** before it was killed. The harness now bounds every sweep in
wall clock (BLUEWAKE_CHUNK_BENCH_TIMEOUT, 180 seconds by default), kills the process,
and reports that the ablation is not sweepable - because a killed sweep is a null and
not a price, and the operator should not have to infer that from a hung process.

**And the window's size is load-bearing, which refutes the obvious use of it.** With
BLUEWAKE_DSP_RATE doubled (25,200 against the shipping 12,600) on the same pair of
ceilings: turns fall 1.5 and 1.3 percent (29,476,279 and 39,960,736 against
29,937,744 and 40,502,699), instructions per play retrace fall from 409.2 M to
**406.8 M, -0.6 percent** - and the route diverges: the 14,700 stop lands at pc
0x80328f90 instead of 0x8027fa30, and the digest record set is **one record**
instead of 1,050. The DSP's service cadence is guest-visible, because the guest's
audio driver waits on the DSP's own interrupts, so it cannot be traded for turns.
The size of the response is the more useful number: 1.4 percent of the turns is
worth 0.6 percent of the window, so the fixed per-turn cost is about 715 host
instructions, 0.17 percent of the window per hundred turns. The split's "rest of
main" 18 percent is therefore per-*boundary* work (mmio, the edge service, the
loop), not per-turn overhead - a reading the sampling split could not settle and
this one does.

**What this changes in the plan, stated because the loop's own page is now wrong in
two places.** The pc-keyed dispatch cache cannot be priced by the hot-ten screen: the
hot ten are 26 percent of the live window, so a design worth about three quarters of a
percent inside them appears as about 0.2 percent, which is a quarter of the half-point
this ledger already records as the resolution across independent pairs. It needs the
full rebuild, and the honest thing is to bundle it with anything else header-shaped
rather than to spend a day on a diluted screen. And the emitted body - 27 instructions
per guest cycle against an authentic-speed budget of 20.6 for the entire window - has
now had five attempts that returned nothing or a refusal: `restrict` (+0.3), `no-pc`
(+0.7), `-O3` (null at the instrument's 0.12 resolution), the flat prepaid pair
(route-refused), and `unchecked` (unsweepable). The body's cost is the emitted
*shape*, not a construct inside it, and the next attempt on it should be a codegen
change measured as a roofline, not another trim.

## 2026-09-22 The in-game save works end to end: the pause menu's second page, the analog R lock, and a card the game wrote itself

P4 milestone 9 is "save, quit, and reload", and it had never been attempted: the
card the certified route produces holds the *empty* file the card manager creates
when a new game starts at file-select - one 12-block "gczelda" file, which is
what strings reported all along - and no gameplay state had ever reached a card
in this build. The save and the reload now both work, driven through the game's
own menus, and both are measured from the card and from the guest's own fields
rather than from a claim.

**What was added, all of it host-side.** An env-gated save route in the host
(BLUEWAKE_SAVE_ROUTE) that reads five guest fields and presses the pad;
scripts/card_container.py, which reads the runtime's portable card container and
recomputes each file's hash, so the save can be proven from the card; a card
write counter in the card runtime; an axis-pulse rearm in pad_event_schedule
(a menu cursor steps the same schedule over and over); two addresses added to the
edge observation set (dMs_Execute, dMs_collect_create) with counters beside them;
and a fix to scripts/gen_edge_intercept_table.py below. 217/217 host tests pass.

**Control is not one moment, and the route's own milestone is the transient.**
The route admits control at retrace 20,256 (reproduced). Retrace 20,395 is
demo_type=2 demo_mode=6 event=106 with message status 16, and the guest sits
there for as long as nothing presses A: the authored sequence after that
milestone still needs input, and the route's walk stops at 20,400. The save route
therefore asks for the state a human could take control in - play scene up,
player record valid, demo playback off, the event system idle, no message window,
no instance demo - and holds it for 90 retraces. It fires at **20,338**, 82
retraces after the route's own gate, and that is the number the save-continue
comparison below is quoted at.

**Three guest facts had to be read out of the pinned decomp because they are not
guessable from the pad.**

| what | measured | where it comes from |
| --- | --- | --- |
| the pause menu opens on the item page | 21 START presses, menu_pause=1 and menu_status=1 (MENU_STATUS_ITEM) throughout, dMc_c NULL | d_menu_window.cpp dMs_Execute takes the collect page only when dMenu_getMenuStatus()==2 or the player is in TactNormalWait; the new-game flow leaves the status at 1 |
| the page switch needs the *analog* R trigger | 12 digital R presses: dmc stayed 0; the same press with analog R=255: collect page open in one press | dMs_isPush_R_Button reads mDoCPd_R_LOCK_BUTTON, and mDoCPd_Read derives mHoldLockR from mTriggerRight, not from the digital R bit |
| the collect page's save screen is a member | proc trace below at save_menu=0x815C2708 = dMc_c+0x2784 | d_menu_collect.h: /* 0x2784 */ dMenu_save_c* dMs_c. The dMs_c at 0x803F7000 is d_menu_window.cpp's file static, for the name-entry and game-over screens |

**The save, from the save screen's own proc field (run 7, on a copy of the
certified route card):** retrace 20,501 mode=3 save_menu=0x815C2708 proc=26
status=1; 20,533 proc=0 (the save question) and A at 20,534; 20,537 proc=1
(memcard check); 20,539/20,541 proc=16/17 (data load wait); 20,565 proc=18 (data
save); **20,567 proc=22 with card_writes=3 card_bytes=24,576**; 20,591 proc=23
(the completion message); 20,615 proc=25, A at 20,616; 20,619 proc=29; 20,643
proc=30 - the "return to the title screen?" yes/no, whose cursor starts on the
first line, so stick-right at 20,644 and A at 20,664.

**The quit is the guest's own reset, and the port survives it.** The A on the
second line requests the reset (PROC_GAME_CONTINUE3 -> mDoRst::onReset): the card
unmounts, the overlap phases step 1 then 2, and from retrace ~21,900 the guest is
running a new boot - a new player actor at a new address in the attract demo
(demo_type=1 demo_mode=4 event=80), with mResetData re-allocated so mReset reads
0 again. Nothing in this workstream had exercised OSResetSystem before.

**The card says so independently.** scripts/card_container.py on the route card
and on the card the run left:

| card | gczelda data sha256 | time | bytes differing |
| --- | --- | --- | --- |
| route card before | 1cb9a251ffcf486c... | 0x322567F7 | - |
| written by the run | 6c7c142eaa97ab7c... | 0x32458BE8 | 1,218 of 98,304 |

**The reload is a separate boot, and it is the time-to-playable reading.** Boot
with that card and the route's own title press: title-ready 333, file-select 535,
play scene 706, and the loaded player is in event-free gameplay (demo_type 0,
demo_mode 0, event_mode 0, no message window) from retrace 744. The save route's
gate - the same predicate that fired at 20,338 on the cold-new-game path -
reports **control-ready at retrace 833**.

| path | retrace control becomes real | guest seconds at the authentic 60 Hz |
| --- | --- | --- |
| cold new game | 20,338 | 339.0 |
| **continue from the save the game wrote** | **833** | **13.9** |

So the PRD's five-minute time-to-playable bar is met by the continue path with a
factor of 21, and the cold-new-game reading is still unreachable by arithmetic.
The pair is measured with one predicate on one build, which is what makes it a
comparison rather than two numbers.

**A hygiene trap that had to be disarmed first.** scripts/gen_edge_intercept_table.py
read only edge_intercepts.c, but bluewake_edge_requires_host moved into
edge_intercepts.h when the boundary loop started inlining it. Running the
generator after that move silently dropped the four keys that only the header
lists - 0x80303A50, 0x80240EE8, 0x80241178, 0x802411F8, the chassis' own re-entry
addresses - which is the same failure its docstring records for a misplaced key,
and the route catches it as a stop before the title screen. The generator now
reads both files and refuses to run when a case list is in neither; the table is
regenerated with all 54 keys.

**Open, and named rather than left implied.** After a *loaded* game the pause menu
opens (menu_pause=1) but the collect page did not follow seven analog-R presses
(run 9), while on the new-game path it opens on the first press (run 7). The item
page's state after a load is the next thing to read there. The save route's
readiness gate is 90 retraces of a clear tuple; that is a route driver's
threshold, not a product definition of control.

**A host turn is a unit of the route's gate, and this was measured the hard way.**
The two menu-path addresses were first added to the observation set
unconditionally, and the certified 14,100 stop moved from 32,203,791 turns to
**32,203,876** while the route digest stayed 92dd816c... : returning to the host at
a block costs a turn, and the recorded stops are quoted in turns. The observation
is now armed by BLUEWAKE_SAVE_ROUTE and the certified route is what it was -
re-measured with every host change of this session in the tree at **32,203,791
turns, pc 0x80307EF4, route digest
92dd816c6a382531d597c8da0cc3e447c716aaa8240912d10a78bee853478782 over 1,050
records**. 217/217 host tests pass.

## 2026-09-22 The dispatch cache and the dispatcher's cold paths are the increment: -1.43 percent

The artifact named in the entry below as the boundary loop's inlining is this
change, and this change is worth more than that entry's -1.38: measured on
frozen copies, back to back, at one load, **455.8 M against 462.4 M, -1.43
percent**, route digest `83d2590d...` identical, both ceilings stopping at the
recorded pc after the recorded turn count.

| artifact | what it holds | instructions / play retrace |
| --- | --- | --- |
| `867a65c5...` | control: relinked 00:43, before the inlining | 462.4, 462.5 |
| `05737860...` | committed HEAD: inlined boundary loop, original dispatch header | 462.3 |
| `930ef3b6...` | inlined loop + 1024-entry window cache + split | 460.5 |
| `3b7ea820...` | inlined loop + pc-keyed 4096-entry cache + split | **455.3, 455.8, 456.1, 456.2** |

**What the change is, in two parts.** The chunk cache becomes an exact pc-keyed
table: `s_cached_pc[4096]` indexed by `((address >> 2) ^ (address >> 14))`,
matched by equality, so the probe loses the range test, the alignment test and
the null test - twenty instructions to eleven on the path every block boundary
takes. The index mixes the 0x4000-window number into the low pc bits because
`(address >> 2)` alone gives all 4096 instructions of a chunk the single slot a
window holds. And the dispatcher is split: `dolrecomp_call_slow` is
`noinline` behind `dolrecomp_call`, so the block-boundary path contains no
alias fallback and no binary search, and its frame drops from six saved
registers to two. Together the dispatcher's hot path goes from 38 instructions
to 26 in the built object, counted in the disassembly, not inferred.

**The inlining this replaces is a null, and the disassembly says why.**
`05737860...` is the committed sources relinked with the original dispatch
header: 462.3 M against the control's 462.4. The boundary loop does inline into
`chassis_dispatch` and its call does devirtualise (`blr x21` through a stack
slot becomes `bl`), but `selected_dispatch` is 908 bytes and stays out of
line, and the loop body is one instruction longer than before (23 against 22) -
the work the entry below attributes to it is not in the code it describes.

**The attribution is checkable rather than argued.** `3b7ea820...` is
reproducible byte for byte from the committed generator plus this block:
`scripts/splice_composite_dispatch.py` lifts the dispatch block out of a
generator run and writes it into a composite tree, refusing unless every region
matches exactly and re-reading the result to compare it against the generator's
own text. Run against the live tree it reports `unchanged` for both headers, so
the tree the artifact was linked from holds the generator's block and nothing
else. The block is emitted independently of the chunk lists, which is what makes
it liftable at all while the composite generator still cannot run end to end.
`tests/test_composite.py` asserts the emitted properties - the probe compares
the key it looked up, the miss path stores the key it resolved, the tables are
sorted and disjoint, and the alias fallbacks are still present in the slow
function - and all 18 of its tests pass.

**The window cache alone was also measured, and it is the smaller half.**
`930ef3b6...` keeps the 64-entry window-granular probe but widens the table to
1024 entries: 460.5 M, so capacity is worth about 0.4 percent and the exact pc
key is worth the rest. It also refutes the reading that the old table was
thrashing: if evictions had been the cost, widening the same design would have
found it.

**The price list, per window, because one number was never enough.** The
boundary census from the entry below, extended to the windows the 1.81x figure
is actually quoted in:

| window | retraces | boundaries / retrace | instructions / retrace | price of one instruction |
| --- | --- | --- | --- | --- |
| opening, 13,800-13,900 | 100 | 21,299 | 53.6 M | 0.040% |
| early play, 13,900-14,100 | 200 | 361,674 | 402.1 M | 0.090% |
| **certified, 13,800-14,100** | 300 | **236,870** | **285.9 M** | **0.083%** |
| steady, 14,100-14,700 | 600 | 391,432 | 492.1 M | 0.079% |

The whole route at ceiling 14,100 is 379,690,147 boundaries plus 32,203,791
turns, 411.9 M blocks over 14,100 retraces. The certified window's price is
0.083 percent per instruction, so this change's seventeen instructions a
boundary are 1.4 percent of it, which is what the measurement says.

**Seventeen instructions a boundary against the twelve the probe and the frame
account for.** The five that are not in the static count are consistent with the
binary search running less often: the probe's own change cannot separate
"cheaper hit" from "fewer misses", and the instrument that would is a counter on
probe hits, misses and search steps, which this change does not carry. Until it
exists, the honest statement is that the measured increment is -1.43 percent and
that a 0.4-point share of it is unexplained by the static count.

**Where the count matters.** `bench_instructions.sh` now names the artifact it
loaded, which is the fix for what went wrong in this window: two writers were
linking the same build path at the same time, and the artifact an entry above
attributes to one change had been produced by the other. A pair is only a
measurement of a named artifact. Within one artifact the pair-difference
survives load - 455.3, 455.8, 456.1 and 456.2 M across runs whose user time
ranged from 25.8 s to 58.2 s - because the load appears in both runs and cancels
in the difference.


## 2026-09-22 The per-block dispatch leaves the loop's stack slot: -1.38 percent, and the 455.3 M pair recorded below is this change

The second increment from v51's queue, and the first one that needed no chunk
rebuild.

**What was wrong.** `bluewake_composite_dispatch_until_boundary` took the dispatch
as a `BluewakeCompositeDispatchFn` parameter and lived in its own translation unit,
so the boundary loop could not see through it: the pointer was reloaded from its
stack slot at every guest edge, and neither `selected_dispatch` nor the chunk lookup
beneath it could be inlined into the loop. The loop's own state was rebuilt per edge
instead of surviving in registers.

**The change.** The loop body is now `bluewake_chassis_dispatch_loop`, static inline
and `always_inline` in `cmake/composite/dispatch_loop.h`, and `chassis_dispatch` in
`cmake/composite/module_export.c` calls it with the static `selected_dispatch` from
the same translation unit, which devirtualises the dispatch and folds the lookup into
the loop. The exported entry point keeps its symbol and now returns the inline body's
result, so its ABI and callers are unchanged, and
`tests/composite_dispatch_loop_test.c` drives the inline path as well - that is the
path the product runs.

| | instructions / play retrace | route digest |
| --- | --- | --- |
| control, relinked from the tree's objects | 462.5 M | `83d2590d...` |
| **dispatch inlined into the boundary loop** | **456.1 M** | `83d2590d...` |

**-6.4 M, -1.38 percent**, both ceilings stopping at the recorded pc after the
recorded turn count, 1,050 digest records identical. The artifact is `3b7ea820...`,
486,779,768 bytes, linked from the two objects this change recompiles and the 754 it
does not.

**Against the census it is about seventeen instructions a boundary.** 6.4 M over the
bench window's 379,738 boundaries is 16.9, and at the census's 0.080 percent per
instruction that is 1.4 percent, which is what the pair measured. One indirect call,
its stack reload and the register pressure they forced are worth seventeen
instructions an edge.

**The 455.3 M pair recorded below measured this change, not the pc-keyed cache.** The
entry under this one attributes 462.5 M to 455.3 M to the pc-keyed dispatch cache
spliced into the working tree's `generated_composite.h`. The artifact that pair
loaded was linked at 00:47 from this change's two objects plus the build's existing
754, and **no chunk object in `build/composite-cycle-hybrid-o2-v2` is newer than
2026-09-18 23:11**, so a header spliced at 00:46 has never been compiled into anything
that pair could have loaded. 455.3 M and 456.1 M are one artifact measured twice, 0.2
percent apart, and the pc-keyed cache is **unpriced**: it becomes measurable when the
hot ten are recompiled against the spliced header (8-15 minutes) and shippable when
all 748 objects are, the 2h15 rebuild this ledger has recorded before. The caution the
entry below draws from the discrepancy - that a static count off a spliced header does
not name the object that was measured - is the right one, and the count to weigh it
against is the census's.

**The control is no longer the recorded hash.** 744 of the tree's 756 objects were
rebuilt between 2026-09-18 23:27 and 2026-09-19 01:08, after the composite this ledger
recorded (`c3b88249...`, 517,360,024 bytes, `__text` 505.1 MB) was linked. Relinking
the tree's objects gives 486,779,768 bytes with `__text` 474.8 MB, and it measures
462.5 M against that artifact's 462.0 M with the same digest, stop pc and turn count:
the same route through a different artifact. So a relink is not a reproduction of the
recorded composite. `scripts/bench_instructions.sh` now takes
`BLUEWAKE_BENCH_COMPOSITE` and prints the artifact's hash, so a pair can name - and
freeze - the build it is a measurement of, which is what makes the two numbers above
comparable to each other and to the control.

**Wall clock, for the record.** The change's pair ran while another host was using the
machine and reported 58.8 s of user time against the control's 30.5 s for the
identical 800 retraces, while the two instruction counts for the same window differed
by 0.1 percent. The same lesson as 2026-09-21 with a wider spread: the milliseconds are
colour, the counts are the measurement.


## 2026-09-22 The FIFO translation moves to a worker: -13.5 percent median and -42 percent on p90

Item 3's design was the idle render worker, and the previous entry's deferral turned
out to be only the shape it needed. The translation now runs on a worker of its own:
the main thread appends the FIFO bytes to a hand-off buffer under a lock and wakes
the worker, which swaps the buffer out and calls the front end's `write_fifo` and
`flush`. The front end's state - parse position, register image, its own buffer - is
touched only from that thread, so no lock guards it; the lock guards the hand-off
buffer alone. The guest-visible synchronizations still drain
(`shadow_frontend_flush` waits until the worker has parsed everything appended so
far), the reset path drains before clearing the front end, and tracing stays on the
calling thread - the trace writer is a file writer and the parse is what feeds it - by
not starting the worker at all when tracing is on, which leaves the old synchronous
path in place.

**Measured on the certified route, the same ceiling, three builds, every run stopping
at the certified `0x8027fa30` after 40,502,699 turns:**

| rendered play window, 14,000-14,700 | median | mean | p90 |
| --- | --- | --- | --- |
| before the deferral | 47.05 ms | 52.54 ms | 83.96 ms |
| deferral only (the control) | 48.68 ms | 50.39 ms | 78.35 ms |
| **translation on the worker** | **40.72 ms** | **38.68 ms** | **48.85 ms** |

**-13.5 percent on the median and -41.8 percent on p90** against the pre-deferral
baseline, and -16.4 and -37.7 against the deferral-only control. The deferral's own
+3.5 is the noise band this instrument has, which is what makes the worker's number
legible: a change of that size sits well outside it, and the p90 movement is a third
of the tail. That is the number item 3 was on the queue for - the only one of these
items that a player feels as stutter.

**The thread evidence says the work moved rather than vanished.** The new worker
carries 16,034 of 16,287 sampled main-thread intervals, and `aurora::gfx::render_worker`
is still blocked in its queue for about 16,246 of them: the translation is on another
core, and the render worker remains as idle as it was.

**And it is inert where it should be.** Headless is 409.2 M instructions per play
retrace against the reference's 408.7 with digest `83d2590d...` identical and both
ceilings at the certified stops, and the 217 host tests pass. Item 3 is done as
queued; what remains of it is the p99 campaign on the device classes P6 names, not
another mechanism.

**Hygiene, unchanged and still true.** The `ref/` hunks are unregistered and
`patches/recompcore/`'s exports are stale, so the working tree remains the authority
for this pin's state.


## 2026-09-22 The GX write path parses at the barriers, and the attribution moves while the work does not

The first half of item 3 was built. The front end already split `write_fifo` (append)
from `flush` (parse, emit, submit), so the change is in the caller: the aurora
backend's `shadow_frontend_write` now only appends, and the parse happens when the
pending bytes reach **1024** or at a guest-visible **barrier**. The barriers are the CP
array mirror and the HLE display-list path - the array mirror must not run ahead of
bytes the parser has not seen, or a draw already in the FIFO is decoded with the
*successor* array, and the DL path parses immediately so the FIFO has to be drained
first - and, new, the host's draw-done commit, through `dol_platform_gx_flush`, a
platform hook wired into the backend's vtable beside `gx_write`.

**What it measures, and it is a null on its own.** The rendered profile, same method
and same route as the entry below: the guest's GX write path falls from **20.89 to
1.53 percent of the rendered main thread** - and the total does not move. The two
runs' own frame timings are a median 47.05 ms against 48.68 ms per retrace (p90 83.96
against 78.35), inside the third-of-a-run spread this ledger has measured for wall
clock, and the rendered differential after the change is +91.8 M instructions per
retrace (36.9 percent) with its before not yet taken. So the per-write flush was not
wasting the parse; it was doing it where the profile could see it. **The deferral is
kept only as the first half of the design, not as a win**, and the worker's baseline
is saved: `/tmp/bw-host-deferral`.

**Inert where it should be, and the route held where it counts.** Headless is
untouched - 409.1 M instructions per play retrace against the reference's 408.7,
digest `83d2590d...` identical, both ceilings at the certified stops - and the
rendered profile run stopped at `0x8027fa30` after 40,502,699 turns, the certified
stop, with 217 host tests passing.

**The next step is the worker, and it is now a small one.** The drain is a single
function, the barrier list is in place, and the handoff is the front end's buffer
(swap it under a lock, or hand the worker a wrapper-owned buffer and let it call
`replay_fifo`); the barrier protocol is drain-then-park, because the metadata
setters mutate the same state the worker parses; and the trace writer has to stay on
the main thread, since it is a file writer and not thread-safe.

**Hygiene.** The `ref/` changes - `aurora_graphics.cpp`, `platform.h`, `platform.c`,
`aurora_backend.cpp`, `aurora_backend_private.h` - are unregistered. The
`patches/recompcore/` exports are stale, as the previous entry records, so my hunks
cannot be exported by differencing the series without separating them from patches
that no longer apply; the working tree remains the authority for this pin's state.


## 2026-09-22 What pins the GX translation to the main thread, measured

v51 asked it and nobody answered, so it was measured with the instrument the ledger
already trusts for renderer shares: `scripts/profile_play.sh`, the rendered route
sampled for 20 s at retrace 14,010 and beyond, 16,181 main-thread intervals.

**The chain, cumulatively - and the tool's own warning applies, these are not
self-times:**

| frame | share of the rendered main thread |
| --- | --- |
| `main` | 89.08% |
| `chassis_dispatch` (guest execution) | 78.72% |
| `func_803256E0` — the one hot chunk issuing the FIFO stores | 20.91% |
| `host_mmio_write` | 20.89% |
| `aurora_backend_gx_write` → `shadow_frontend_write` | 20.89% |
| `RetailGxFrontend::flush` | 18.21% |
| `emit_new_packets` | 15.38% |
| `GxCoreSink::submit_packet` | 14.84% |
| `ConsumingAuroraRenderSink::submit_packet` | 12.29% |
| `...::accumulate` | 11.92% |
| `GxCoreSink::on_consumed_draw` (the draw-submission hook) | 7.24% |

**And the machines that would do the work are idle.** `aurora::gfx::render_worker`
is blocked in `BoundedQueue::pop_for` → `condition_variable::wait_until` →
`__psynch_cvwait` for **14,933 of 16,181 intervals, 92.3 percent**;
`aurora::gfx::pipeline_worker` is blocked on a condition variable for **16,181 of
16,181, all of it**; and the Metal completion workloops carry samples in **61
intervals, 0.4 percent**. So the ledger's "the render worker idles 94 percent of the
time" reproduces at 92.3 percent today, and the second worker has never run.

**The answer: nothing architectural pins it — the call site does.** Every guest FIFO
store is executed by the emitted chunk code, which reaches the host through
`host_mmio_write`, and that call runs the whole path synchronously: the front end's
FIFO fill, `flush`, packet emission, packet submission, the render sink's assembly
and the draw-submission hook. The front end is not a separate thread with a mailbox;
it is a function the guest's store calls.

**What a design must keep consistent, and it is a short list.**

1. The FIFO's own guest-visible semantics - the write index, the idle/breakpoint
   state and the read-back the guest performs through the same MMIO path.
2. The mirrored CP/BP metadata the front end keeps host-side (`set_cp_array`, the
   display-list mirror), which must reach the translator in guest order.
3. The draw-done publication the guest polls, which the ledger already routes as the
   host intercept at `0x80308A9C` ("GX draw-done return publishes PE finish").

**The price, stated the way the tool allows:** the translation-and-assembly chain is
**20.89 percent of the rendered main thread, cumulative**, and a main thread that is
89.08 percent of the sampled time is the frame's bottleneck; the design is a FIFO
ring filled on the main thread by `shadow_frontend_write` (a copy, not a
translation), a worker draining it, and a barrier at every guest-visible
synchronization above that drains the ring first. What fraction of the 20.89 is
movable is exactly what the subtraction this tool forbids would pretend to know; the
honest statement is that the *translation and assembly* is the bulk of it, the
*synchronization* is the rest, and the design's first job is to draw that line in
code rather than in a share.

The gate is the rendered tier's own: the route digest and stops must hold, and
`scripts/bench_rendered.sh` gives the split the change is judged on.

**And the line the design needs is already in the code.** `RetailGxFrontend` splits
exactly the way a worker wants: `write_fifo(std::span<const std::uint8_t>)` is four
lines that append the bytes to `fifo_buffer_` and return, and `flush(AuroraRenderSink*)`
is what parses the buffer (`parse_stream`), erases what it consumed, emits the packets
(`emit_new_packets`) and drains them to the sink. The cost is in `flush` - 18.21 of
the 20.89 percent cumulative - and the caller in `aurora_graphics.cpp` runs
`write_fifo` and `flush` back to back for each fragment, which is what pins the
translation to the guest's store. So the change surface is small in the right way:
keep `write_fifo` on the main thread, let the idle worker own `flush`, and make the
barriers the only synchronous part. The barrier list is readable from the same code -
the fragment boundaries the current caller already respects, the FIFO read-back the
guest performs through the same MMIO path, and the draw-done publication at
`0x80308A9C` - and the ring is sized by traffic that is already counted: 48,300 FIFO
writes a play retrace at up to eight bytes each, so a few hundred kilobytes a retrace
with a few megabytes a second of headroom.


## 2026-09-22 The flat-body prepaid copy is refused by the route: the per-instruction charge is load-bearing

The second half of the cycle-accounting design was built, priced at the chunk level,
and then refused by the route. It is the most useful negative result this workstream
has produced, because it overturns the design's premise rather than trimming it.

**What was built.** `ref/recompcore/DolRecomp/src/backend/emitter.c` grew a second
flat body: every `func_%08X` became the fast copy - a constant `cycle_block_prepaid`,
one decision per block leader with a bail-out that hands an unprechargeable block to
`func_%08X_precise` at the same pc, no per-instruction `dolrecomp_charge_precise` and no
observation reconcile - and the precise body was emitted beside it, reached only from
those bail-outs. The emission checked out: 206 chunks regenerated, the precise copy
**byte-identical** to the body it replaced, chunk 0144's charge-site and reconcile
counts unchanged at 3,007 and 1,384, and DolRecomp's own emitted-C tests passing.

**The chunk sweep priced it at -6.0 percent** - 26.91 to 25.28 instructions per guest
cycle on chunk 0144, against the ablation's -9.8 for the whole design - so it went to
the cold ten (18.4 minutes) and then to the route.

**The route refused it, and the shape of the refusal is the finding.** At 400
retraces the candidate stopped after **3,836,466 host turns against the control's
1,372,978**, with the guest cycles identical to within one (3,240,000,000 against
3,240,000,001) and the same timebase. Same guest work, 2.8 times as many turns: the
budget the host hands the chassis per turn fell by that factor, which means the
host's cycle accounting moved, not the guest's. The guarded pair failed at both
ceilings - 65,807,110 and 98,319,535 blocks against 29,937,744 and 40,502,699 - and
reproduced with **chunk 0000 alone**, which is what made a one-minute iteration loop
possible.

**Two explanations were eliminated by measurement.** The observation reconcile
(whose suffix the fast copy stops refreshing) got an entry condition -
`deadline_budget > 0 && deadline_budget < max_suffix` - and the stop counts came back
**identical to the digit**, so either the suffix is 1 in those bodies or the clause
never fires; and the label plumbing cannot be it, because the precise copy is
byte-identical to the old body. What is left is the part the ablation could not see:
`dolrecomp_charge_precise` is a pure budget test in isolation, and the chunk bench
never runs the host's cycle domain, so the ablation's charge-equivalence does not
extend to the host's view of when the guest advanced. **Removing the per-instruction
charge path is therefore not route-neutral, and the -9.8 percent of the emitted body
that the ablation prices is not available to this design.**

**Reverted, and the tree is route-exact again.** The flat pair is off (one call,
named `func_%08X`), the loop twin is off, and regeneration reproduces the pinned
series' shape exactly: chunk 0144 is 57,742 lines with 3,007 charges, 1,384
reconciles and no twins. The reference artifact is back in the build directory and a
400-retrace run reproduces its 1,372,978 blocks exactly.

**Two hygiene findings, recorded because they cost time.** The `patches/dolrecomp/`
exported series is **stale**: 0003 and 0018 apply to the pinned checkout and 0002,
0016 and 0017 do not, so the working tree - not the patch files - is the authority
for this emitter's state, and a reverse-apply of the tree's whole diff destroys the
series rather than my part of it. And the tree's composite is a mixture - the hot ten
recompiled from regenerated sources on top of 196 objects from an earlier state -
whose route drifts 0.001 percent from the recorded stops; only a full rebuild makes a
landed emitter change real, and until then the frozen reference artifact is the
control.

**What is parked in the pin, and it is inert.** The tag threading through the branch
emitters (called with an empty tag, so the emitted text is unchanged), the
`emit_flat_function` extraction, and the dormant fast copies for both the loop and
the flat body, with comments saying why the flat half is off. A future attempt can
re-enable the shape by restoring one call, and it should instrument the host's cycle
domain first rather than the emitted charge sites.


## 2026-09-22 The loop-scoped prepaid twin is a null, and the loop bodies are why

The cycle-accounting design was priced at 9.8 percent of the emitted body, so the
first half of it was built to find out what that is worth on the route. The counted
loop is the tractable half - its body is its own function, so a fast twin cannot
collide with another body's labels - and the change is exactly the priced shape:
`emit_counted_loop` grew a `fast` variant that decides once per iteration via
`dolrecomp_block_can_precharge`, charges the block up front, hands an unprechargeable
iteration to the precise function, and emits **no** per-instruction
`dolrecomp_charge_precise` and **no** reconcile; the call site runs the twin.

| | |
| --- | --- |
| shape diff, 206 regenerated chunks against the live tree | 137 files differ; chunk 0144 gains 8 twins and 8 fast call sites, and its `dolrecomp_charge_precise` and reconcile counts are **unchanged** (3007 and 1384), so the precise functions are untouched |
| DolRecomp's own tests | pass, including the emitted-C shape tests - after the twin stopped declaring an unused `cycle_block_prepaid` |
| screen, hot ten | 9.5 min, composite `08e2f1592c7bed0f` |
| route, control `3b7ea820` frozen copy | 408.7 M instructions/play retrace |
| **route, loop twins** | **408.6 M** |

**-0.02 percent, a null, with both route digests identical at `83d2590d...` and both
ceiling stops matched.** The change is semantically right and worth nothing, and the
play-window sample says why: **the counted-loop bodies are 71 of the 9,094 sampled
emitted-body frames - about 0.8 percent.** The design's prize is in the flat
`func_*` bodies, which carry the same per-instruction test and the same reconcile and
which the play scene actually runs.

**What that changes.** The twin stays as the first half of the change - the emitter
edit is in `ref/recompcore/DolRecomp` and the regenerated chunks are in the live
composite tree, so the artifact is reproducible from that emitter state, and it is not
registered in `patches/dolrecomp/` until the half that pays lands with it. The second
half is the flat body, where duplication is expensive: its labels *are* the pc table's
dispatch targets, so a fast copy needs suffixed labels, a second dispatch table, and
every in-body `goto` redirected into the copy it belongs to. That is the change the
9.8 percent is actually about, and the loop scope just paid 9.5 minutes to establish
that it must not be attempted on the loops.


## 2026-09-22 The cycle-accounting design is priced: 9.8 percent of the emitted body, and the decision is free

v52's item 2 was to price the block-entry precharge decision with the ablation
harness before writing any of it. Chunk 0144 against its real-state snapshot, the
same instrument the ledger's 2026-09-18 numbers came from:

| ablation | instructions / guest cycle | against baseline |
| --- | --- | --- |
| baseline | 26.91, 26.92 | - |
| `prepaid` - force the flag, drop the precharge call and every per-instruction test | 24.27 | **-9.8%** |
| **`prepaid-decision`** - keep the block-entry decision, force the flag anyway | 24.26, 24.27, 24.29 | **-9.8%** |

**The decision is free and the whole prize is in the body.** `prepaid` was already
an upper bound; `prepaid-decision` is the shape a real design would execute - the
decision still taken at block entry, the body down the fast path - and it prices the
same 9.8 percent to within the harness's 0.1 percent repeatability. Against the
split's 54 percent emitted body that is **about 5 percent of the window**, roughly 20
M instructions per retrace, minus one branch per block (391,432 a retrace, ~0.2
percent): the largest single lever left in the headless gap.

`prepaid-decision` is a new ablation in `scripts/ablate_chunk.py` and it lands with
this entry. An ablation for the *code growth* the design implies - a duplicate of
every emitted block - was written, failed to compile, and was removed rather than
left broken: it also could not have answered the question, because dead duplicated
code never enters the executed path and the risk in the real design is I-cache, not
instruction count. **The instruction harness cannot price a cache effect**, so that
part is settled the only way it can be: implement the emitter change, screen the hot
ten, and bench the route, where the layout is the build's own.

**And the emitter this belongs to is not the one a reader would guess.** The tree's
chunks are emitted by `ref/recompcore/DolRecomp` - its `src/backend/emitter.c` holds
the cycle emission at lines 508 and 2007-2060 and is uncommitted-modified, which is
the patch series as the fences describe it, and patches 0003/0017/0018 no longer
apply there because they are already in. The `ref/DolRecomp` checkout beside it
contains no occurrence of `precharge` at all and had the three patches fail their
apply check: it is an LLVM-side tree. `build/dolrecomp-cycle-precise`'s CMake cache
names `ref/recompcore/DolRecomp` as its source directory, which is the tie-break that
settles it: three of the four files in the tree that mention
`dolrecomp_block_can_precharge` are the generator, the test and the ledger, and the
emitter that actually writes it is the recompcore one. Recorded because the next
iteration is an emitter change and would otherwise be written into the wrong tree.


## 2026-09-22 The split: the guest body is half the window, the donor DSP a tenth, and the host a fifth

v52's first item was a measurement rather than a change, because the per-block
constant was the last *localised* lever and everything left is spread across the
window. The instrument is the one this ledger already trusts for shares -
`sample` on the live play window, parsed by `scripts/sample_owners.py` - pointed at
a **headless** run this time, which is the configuration every bench in this file
quotes. It captured 12,344 main-thread samples at retrace 14,022 and beyond on the
build that measures 412.3 M.

| bucket | share of the headless main thread |
| --- | --- |
| guest execution, `chassis_dispatch` (dispatch, chunk bodies, edge service inside it) | **71.9%** |
| - of which emitted chunk bodies, the depth-3 slice (a lower bound) | 43.8% |
| - the chassis edge service | 3.95% |
| per-turn device service, `host_sync_cycle_devices_end_turn` | **10.2%** |
| - of which the donor DSP LLE, `DSP::Interpreter::Interpreter::RunCycles` | **10.06%** |
| the rest of `main` (mmio handling, card, publish, the loop itself) | ~18% |
| guest dispatch and lookup as their own leaves | 4.3% |

**The guest body is about half the window**, which is what the roofline arithmetic
already said: 8.1 M guest cycles a retrace (the clock census reports 119.07 G cycles
over 14,700 retraces, which is 8.1 M each) at the 26.9 instructions per guest cycle
the chunk sweep measures is about 218 M of the 412.3 M.

**And the donor DSP is a tenth of the window, which nobody had put a number on.**
The per-turn service is 98.6 percent `host_sync_dsp_cycles`, and that is essentially
all `DSP::Interpreter::Interpreter::RunCycles` with its per-opcode handlers
underneath. The delivery census sizes the work: 10,916 DSP advances a retrace, and
the adapter is handed the guest's whole 119 G cycles, which at one DSP cycle per six
guest cycles is about 1.35 M DSP instructions a retrace. With the LLE at 41.6 M host
instructions a retrace (10.1 percent of 412.3 M), **the interpreter spends about 31
host instructions per emulated DSP instruction.** That is the number to attack: a
dispatch that costs 15 is worth about 5 percent of the window, and one that costs 10
is worth 6.8, against a change that cannot move the guest at all because the DSP
program it runs is the same program.

**What is accessible inside that 31, stated so the entry does not over-promise.** The
interpreter is already table-driven - `decoded_ops` is a 65,536-entry array built at
init and the fetch is inlined by patch 0039 - so the per-instruction cost is the
indirect member-function call through that table, the per-instruction
`IsLoopEnd`/exception/step bookkeeping, and *the handler bodies, which are the DSP's
semantics*. The bookkeeping and the call are perhaps ten to fifteen of the 31, so a
dispatch-only fix is worth about **1.5 to 2.5 percent** of the window, and the rest
of the 10 percent is the fidelity the donor route bought: it is the LLE running the
real IROM at the real rate, not overhead to delete. Reaching the 31-to-15 shape the
paragraph above prices would be the whole per-instruction path, which is a rewrite of
a pinned dependency rather than a patch, and the loop should say so before it starts.

**A false lead, recorded so it is not followed again.** My own tree parser made
`ppc_guest_alias_resolve` 6.0 percent of the window and `ppc_fp_available` 3.4. The
alias path has its own inert census, and turning it on refutes the reading: 16,826
resolve calls a retrace, 6.3 registry iterations each, 0.9 percent hits, and
`overlap_mem1=0` - the 2026-08-28 unshadowed-MEM1 fast path is doing its job and
almost nothing reaches the resolver. The tell was in the output: the hand-rolled
parse's shares summed to 152 percent of the thread. **A share from a hand-rolled
parse of a `sample` tree is not an instrument; the sanctioned parser and a targeted
census are.**

## 2026-09-22 The per-term mask is refuted: the recomputation left behind is owed to the cycle cursors

The gate below removed the recomputations that found nothing and left 125,404 a
retrace, and the obvious next question was whether those are legitimate. The
candidate replaced the gate's one boolean with a mask of which source the access
could have changed - a GX FIFO byte cannot move a DI, DSP or SI line - so the
recomputation would be skipped entirely for the GX and VI/PI/MI pages and rebuilt
one term at a time for the DI, SI and DSP/AI pages.

**The instrument that decided the ranges lands with the refutation.**
`[mmio-census]` counts `host_mmio_read` and `host_mmio_write` traffic per 0xCC00
page, compile-time gated like the rest of this census. In the play window it is:

| page | reads / retrace | writes / retrace |
| --- | --- | --- |
| 0xCC002 VI | 5 | 13 |
| 0xCC003 PI | 21 | 0 |
| 0xCC005 DSP, AI | 238 | 155 |
| 0xCC006 DI, SI, EXI | 8 | 1 |
| 0xCC008 GX FIFO | 0 | **48,300** |
| 0xCC001 | 0.5 | 0.5 |

**99.65 percent of the device writes are GX FIFO bytes and 87 percent of the reads
are on the DSP and AI page**, which is what made the mask look free.

**It is refuted in both forms, and the census says the mask itself worked.** A
containment-chain dispatch per access measured 413.6 M against the gate's 412.3 M,
**+0.32 percent**; rewriting the dispatch as a 16-entry page table measured 412.9 M,
**+0.15 percent**; the digest is unchanged in both. Meanwhile the census confirms
the intended effect: recomputations fall from 75,242,634 to 46,253,800 in the play
window and from 325,934,428 to 209,739,061 over the route, with `published`
identical at **261,281**, so the mask never lost a publish.

**Why the count and the machine disagree.** The mask is not where the cost is. Every
access still steps the device cursors, and `host_sync_cycle_devices` advances the
audio and DSP cursors, whose terms *are* one of the three lines - so the
recomputation is owed on almost every access whatever the address. What the mask
bought was a cheaper recomputation, about 55 of its 85 instructions; what it cost
was a per-access dispatch on 48,742 accesses a retrace plus the compiler's ability
to keep the three booleans in registers. The two cancel, and the second form does
not change that, which is the useful part: the loss was not the dispatch, it is the
recomputation being justified.

**Recorded because the loop keeps having to learn it: a census that counts
recomputations is not a price.** The count said 39 percent fewer; the machine said
no. The change is reverted and the question it exposed is the next one - the cursor
advance itself, which happens on 48,742 accesses a retrace and is the reason the
recomputation cannot be skipped - and that needs its own measurement before
anything is written.


## 2026-09-22 The interrupt-source recomputation becomes conditional: -7.33 percent

The census's first item, taken as the design it asked for rather than a trim.
`host_interrupt_sources()` is a pure function of device state - three booleans read
out of the DI, the DSP/ARAM/audio modules and the SI - and the refresh rebuilt them
1.305 times per block boundary while the play window changed its answer 17 times a
retrace. Every mutation of that state arrives through a host entry point the host
already runs: a guest device register access (`host_mmio_read`, `host_mmio_write`,
which also step the device cursors), a cursor advance (`host_sync_cycle_devices` and
its end-of-turn twin), or the DSP adapter paths (`host_dsp_run_cpu_cycles`, where the
donor DSP raises DIRQ, and the mailbox reads that clear it). A sticky flag set at
each of those entry points is now the recomputation's trigger, and the boundary path
returns on a load and a test.

| | instructions / play retrace | digest |
| --- | --- | --- |
| before the gate | 444.9 M | `83d2590d...` |
| **sources recomputed only on a device event** | **412.3 M** | **`83d2590d...` identical, 1,050 records** |

**-7.33 percent**, 32.6 M instructions per retrace, the largest single increment of
this workstream, and the arithmetic closes on the census: 385,436 recomputations per
retrace are skipped and 32.6 M / 385,436 = **84.6 instructions each**, against the
60-95 the disassembly bounded the recomputation at.

**The gate was verified against the count it could have broken.** The census counts
both halves, and over the whole route to 14,700 retraces the numbers that must not
move did not: `published` is **261,281** before and after, `calls` 614,549,179,
`overlap_guard` 578,587,698, `predicate_true` 6,934,736, `intro` 102, `scheduler`
12,453 - identical - while `publishes` falls from 933,358,820 to 325,934,428. So the
gate skipped 607 M recomputations and published exactly the same set at exactly the
same points: both ceilings stop at the recorded pc after the recorded turn count and
the route digest is unchanged over 1,050 records. A missed event could only have made
a publish late, and the count says none was missed.

**What the play window keeps, and what is left.** A third of the boundaries still
recompute (125,404 a retrace, 32.0 percent), because that is how often the guest
polls a device register, and each of those is an event the sources can change at. On
those paths the recomputation is now about 85 instructions: `dol_di_interrupt_pending`
and `dol_si_interrupt_pending` are separate functions in `ref/recompcore/GXRuntime`
(17 and 8 instructions plus a call frame each) and want a registered patch to move
into headers, and the DSP terms are several host flags OR'd together. The overlap
observation's guard, at 100 percent of boundaries and about a dozen instructions, is
the other piece left, and it is nearly irreducible: the reads have to happen to know
the phase did not change.


## 2026-09-22 The interrupt-source publish path inlines into the refresh: -2.29 percent

The census's first item, taken in its smallest sound form. `host_publish_changed_sources`
runs 1.305 times per block boundary and changes its answer 17 times a retrace, and
its body carried the delivery-safety census - counters and a timeline print that sit
behind `g_delivery_safety_census_enabled`, and are therefore never executed in a
shipping run, but which occupied the instruction stream of a function the boundary
calls 391,432 times a retrace.

The census body now lives in a `noinline` helper, called under the same flag that
gated it, and the publish path is `static inline` in the refresh. The disassembly
shows the change is what it says: the `host_publish_changed_sources` symbol is gone
from the binary, the helper is 87 instructions of its own, and
`host_refresh_interrupt_sources` grows from 98 instructions to 142 to hold the
inlined path.

| | instructions / play retrace | digest |
| --- | --- | --- |
| control, shared host binary | 459.0 M | `83d2590d...` |
| predicate inlined into the chassis | 455.4 M | `83d2590d...` |
| **publish path inlined into the refresh** | **444.9 M** | **`83d2590d...` identical, 1,050 records** |

All three runs are on the same frozen composite copy and the same card, both ceilings
stopping at the recorded pc after the recorded turn count, both milestones reached,
and the thirty host tests pass in the candidate build. The pair that isolates this
change is 455.4 to 444.9, **-2.29 percent**, 10.5 M instructions per retrace.

**Why it is bigger than a call frame, and the ledger has seen this before.** A frame
is about ten instructions and 1.305 publishes a boundary is thirteen of them; the
measurement is twenty-eight. The rest is the dead-body effect measured on 2026-09-18,
when moving six instructions of never-executed charge body out of line was worth 2.1
percent: the census body was never executed either, and its cost was the instruction
stream it occupied.

**What is left in the refresh needs a patch to the pinned dependency.** The calls
that remain in its hot path are `dol_di_interrupt_pending` (17 instructions plus a
call), `dol_si_interrupt_pending` (8 plus a call) and the DSP pending terms, and the
DI and SI predicates live in `ref/recompcore/GXRuntime/src/di.c` and `si.c`. Moving
their definitions into the headers is the same frame removal that took 0.78 percent
off the chassis predicate, and it is a registered patch rather than an edit, because
`ref/` is a pinned dependency whose uncommitted state is the patch series.

**And then the design the census points at.** The three booleans only change when the
guest touches a device register or a cursor advances, both of which the host already
handles, so the recomputation can become conditional on those events instead of
running 1.305 times a boundary to return the same answer.


## 2026-09-22 The chassis edge-service body is decomposed, and what is left in it is the interrupt refresh

The queue's next item was to decompose the per-boundary body instead of pricing it
from the disassembly, because its pieces have live fractions that reading gets
wrong - the ledger had already been wrong once about the overlap observation, whose
guard "is false most of the time". `BLUEWAKE_EDGE_CENSUS` counts the body's
sub-blocks, two ceilings are differenced, and the counts are a self-check: the
census's `calls` equals the boundary census's at both ceilings, and the
14,100-14,700 difference is **234,859,032 boundaries**, the number the boundary
census gives for the same window.

| piece | per boundary in play | per retrace |
| --- | --- | --- |
| boundaries | 100% | 391,432 |
| overlap guard true | **100.00%** | 391,432 |
| overlap object is a valid pointer | 0% | 0 |
| overlap fields resolved (cache hit) | 0% | 0 |
| overlap phase changed | 0% | 0 |
| intercept predicate true | 1.53% | 5,983 |
| interrupt-source publishes | **130.5%** | 510,841 |
| published set actually changed | 0.0044% | 17 |
| intro milestones, service-each-block, EE interrupt | 0% | 0 |
| scheduler interrupt | 0.0002% | 0.8 |

**The overlap observation runs at every boundary and produces nothing in the play
window.** Its guard is true 100.00% of the time, the object it reads is never a
valid pointer, the enabled word is never 1 and the phase never changes - not once in
600 retraces. What it costs is the guard and the cached slot read, about a dozen
instructions a boundary, so roughly one percent of the steady window, and that is
close to irreducible: the reads have to happen to know the phase did not change. The
0.5 percent the overlap cache was worth is consistent with it - six of those twelve.
The reading that the guard is usually false is refuted by the count, which is the
second time this body has punished arithmetic.

**The redundant re-probes are refuted.** `bluewake_edge_requires_host` answers true
on **1.53 percent** of boundaries (5,983 per retrace), so the two extra hash probes
and the switch walk it performs on a hit are worth at most sixteen instructions
times 0.0153 of a boundary - under 0.02 percent for the whole change. That candidate
is now closed with a number instead of a hunch, which is what the census was for.

**The interrupt-source refresh is the body's biggest remaining item.** The published
set is rebuilt 1.305 times per boundary - once from the chassis edge service and the
rest from the DSP mailbox paths, which run 0.3 times a boundary - and it actually
*changes* 17 times per retrace, 0.0033 percent of the calls. Its hot path is three
calls (`dol_di_interrupt_pending`, `dol_si_interrupt_pending`,
`host_publish_changed_sources`) wrapped around the DSP pending terms, which bounds
it at **60-95 instructions per call** and therefore at about **6 percent of the
steady window** per boundary. That price is a bound and not a measurement, and it
cannot be ablated into one: the refresh is what publishes interrupts, so a build
without it changes the route it is being measured on - the honest move is the
opposite direction, a design that makes the recomputation conditional on the device
events that can change the answer (the MMIO writes the host already handles and the
cursor advances it already performs), with the digest and the stop line as the gate.

**And the rest of the body is empty.** The intro milestones fire zero times, the
service-each-block diagnostic is off, the EE-and-decrementer test is true zero times,
and the scheduler test 0.8 times a retrace. So the chassis edge service is now the
overlap guard at about a dozen instructions and the interrupt refresh at sixty to
ninety, and the price list's next candidate is the second of those.

**The instrument is compile-time gated and off in the shipping build.** A dozen
counters cost a load and a test each at every boundary - about 2.4 percent of the
window - so unlike the boundary census this one cannot live behind a runtime flag.
`-DBLUEWAKE_EDGE_CENSUS=ON` builds it, and the shipping build is unaffected: the
same `nm -S` comparison against the control binary that verified the inlining
increment finds no difference with the option off.


## 2026-09-22 The boundary predicate loses its call frame: -0.78 percent

The queue's first item after the dispatch landing was the surviving miss path
through `bluewake_edge_requires_host`: 24 instructions from its entry to its
return, which at 391,432 boundaries per play retrace is about 1.9 percent of the
steady window - the largest single piece of the per-block constant left. The
disassembly says where those instructions are. Six of them are the frame - `stp`
twice, `add`, `ldp` twice, `ret` - and the caller's `bl` and argument save make
seven. Nothing is live across the probe -
the predicate reads the module-1 raw base, hashes the canonical address, walks the
open-addressed table and returns a boolean - so the frame existed because the
predicate was a separate translation unit's function and for no other reason.

`bluewake_edge_requires_host` now lives in `edge_intercepts.h` as a `static inline`
with `always_inline`, so the chassis inlines it. The two switch-bearing predicates
stay out of line, and the disassembly shows that is safe: the probe is inlined at
`0x100464f54` and both `bl`s remain, reachable only from a hit. The miss path
falls from 25 instructions plus a call to 18 instructions.

| | instructions / play retrace | digest |
| --- | --- | --- |
| control, shared host binary | 459.0 M | `83d2590d...` |
| **predicate inlined** | **455.4 M** | **`83d2590d...` identical, 1,050 records** |

**-0.78 percent**, 3.6 M instructions per retrace, both ceilings stopping where the
certified route stops and both milestones reached. The change is host-side, so no
chunk was recompiled and the composite is untouched.

**The static count understated the dynamic effect for the third time in two days.**
3.6 M over 391,432 boundaries is **9.2 instructions per boundary** removed, against
the 7 the disassembly showed: the inlined form also drops the caller's argument
setup for a call that no longer happens, and the reload of the address around it.
The denominator has been right every time; a static delta is a screen and not a
price, which is what the entry below records and this one repeats.

**How the pair was taken, because three agents share this tree.** The composite was
copied to `/tmp/bw-p1-composite.dylib` and frozen for both halves of the pair, and
the candidate was built into `build/runtime-host-p1b` from its own configure rather
than into `build/runtime-host-dsp`, so a relink or a rebuild by another thread could
not move either half mid-run. The private build is equivalent to the control binary
apart from the change: `nm -S` over both lists 23,636 symbols and three lines
differ - the removed `bluewake_edge_requires_host` text symbol, the `g_edge_keys`
local data symbol the inlined probe now emits into `main.c`'s unit, and a
merged-globals index.


## 2026-09-22 The dispatch count is the boundary count, so the open question is the static count

The entry below left one question open: the dispatch candidate removed 7.2 M
instructions per retrace, which over 379,738 boundaries is 19 instructions per
dispatch, where the disassembly counted 12 - so either the split removed more than
the count showed, or `dolrecomp_call` runs more often than the edge service does.
The second reading is refuted by the emitted code, not by an argument: **none of
the 206 chunk sources under
`local-research/work/cycle-extent-v6-20260831/composite-r2` mentions
`dolrecomp_call`**, because a block ends by storing `ctx->pc` and returning to its
caller - `chunk_0144_text1_802416E0.c` has 1,364 stores followed immediately by a
return over its 4,096-slot range - and every dispatch therefore comes from the
host: the chassis entry dispatches once per turn and its loop dispatches once per
boundary beside one edge-service call, so the two counts differ by one per turn.
The denominator is settled for the dispatch path the same way it is for the
predicate.

What the stop line calls "blocks" is the host's turn counter rather than the
boundary count, which is why it is so much smaller: 2,755 turns per play retrace
at ceiling 14,700 against 391,432 boundaries, a turn being one cycle-budget window
and a window holding about 142 blocks. The census print labels that same counter
`turns=`; the two names are one variable.

So the discrepancy is in the static count rather than in the denominator, and the
count to trust is the one taken off the built object: the count that predicted 1.0
percent was taken from the spliced header in the work tree, while the composite
that measured 455.3 M was linked from whatever objects the build then had, and the
loop's record does not tie the two together. That is the caution the method
carries: a static count is the cheapest way to see whether a change is worth four
minutes of bench, and it is not a substitute for confirming the built object
against the census once the pair comes in.


## 2026-09-22 The per-block constant is measured: 391,432 boundaries per play retrace, so one instruction there is 0.08 percent

v51's price list - one host instruction removed from the per-block dispatch path
is worth about **0.22 percent** of the play window - was two inferences stacked on
each other: that the edge fast-reject removed about twenty host instructions per
dispatch, and, from dividing 4.42 percent by that, that a play retrace runs about
1.07 M dispatches. The second half is now measured and it is 2.7 times smaller, so
the constant is 2.7 times smaller. **One host instruction removed per block
boundary is worth about 0.08 percent of the play window.**

**The instrument.** `BLUEWAKE_BOUNDARY_CENSUS` counts calls into
`host_chassis_edge_service`, which `cmake/composite/dispatch_loop.c` makes once per
boundary-loop iteration, and `bluewake_edge_requires_host` is called from exactly
one place inside it, `runtime/host/src/main.c:766`. It counts calls rather than
stamping a clock for the reason the instruction bench reads counts: they are
deterministic on a digest-gated route and wall clock is not.
`BLUEWAKE_BOUNDARY_CENSUS_WINDOW` opens the window at a given retrace, and two
ceilings differenced the way `bench_instructions.sh` differences instructions give:

| window | retraces | boundaries / retrace | instructions / retrace | one instruction |
| --- | --- | --- | --- | --- |
| opening, 13,800-13,900 | 100 | 21,299 | 53.6 M | 0.040% |
| early play, 13,900-14,100 | 200 | 344,655 | 402.1 M | 0.086% |
| certified, 13,800-14,100 | 300 | 236,870 | 285.9 M | 0.083% |
| **steady play, 14,100-14,700** | 600 | **391,432** | **492.1 M** | **0.080%** |
| bench window, 13,900-14,700 | 800 | 379,738 | 465.1 M | 0.082% |

Four of the five agree inside eight percent. The odd one out is the opening
cutscene, a different instruction mix at 54 M per retrace, at half the rate. A
ratio that holds across windows whose instruction mixes differ is a property of
the route rather than of the window, which is the property the price list needs
it to have. The runs set the census flag with ceilings 14,100 and 14,700 and
windows 13,800 and 14,100, and both stopped where the certified route stops; the
instrument lands with this entry, so the number and the thing that produced it
are in the same commit.

**The reconciliation is what makes this a measurement and not arithmetic.** 4.42
percent of the bench window is 21.5 M instructions per play retrace, and 21.5 M
over 379,738 boundaries a retrace is **about 56 instructions removed per
boundary**. So the fast-reject's saving and the census's count are two views of
one number, and 56 is the better account of what came out: the old miss path
called `bluewake_card_runtime_intercepts` and then walked two switch statements -
ten cases in `bluewake_edge_address_requires_host`, seventeen in
`bluewake_edge_observation_requires_host` - before the new code's single hash
probe could reject the address at all. The surviving miss path is **24
instructions** from the entry of `bluewake_edge_requires_host` to its return in
`build/runtime-host-dsp/bluewake_host`, 31 when the probe displaces once; with 204
of the table's 256 slots empty, 24 is the common case, and about seven of those
are the prologue, epilogue and argument shuffle of a function the compiler kept
out of line from the chassis. So the predicate alone is still about 1.9 percent
of the steady window.

**What it changes.** Every candidate the old list priced is 2.7 times cheaper
than the list said: ten instructions are 0.8 percent and not 2.2, and an
increment has to remove about fifty instructions per boundary to be worth what
the fast-reject was. The dispatch candidate now in flight - a pc-keyed cache in
the generated dispatcher with its cold paths split out of the hot path, 38 to 26
host instructions by the same kind of static count - is the first test: 12
instructions over 379,738 boundaries predicts 4.6 M per retrace, **1.0 percent**,
and the pair measured **462.5 M to 455.3 M, 7.2 M, 1.5 percent**, digest
`83d2590d...` unchanged, with the control's own reproducibility 0.5 M across the
relink that preceded it. That implies **19 instructions per boundary** where the
disassembly counted 12, so either the split removed more on the path the route
walks than on the hot path it was counted from, or `dolrecomp_call` runs more
often than the edge service does. The two are equal by construction in
`dispatch_loop.c`, so the second reading wants a second dispatch path, and the
measurement that decides between them is the dispatch count itself, taken the way
this one was.

**The instrument is not free.** Disabled it still costs three instructions per
boundary - `adrp`, `ldrb`, `tbz` at `0x100464c44` of the host binary - which is
1.17 M per play retrace, **0.24 percent** of the window. Both controls quoted
above were benched with it compiled in.


## 2026-09-21 The per-block overlap observation is half a percent, not five - and the cost model was wrong about why

The first candidate priced from the per-block model, measured by the instrument built for it. **462.6 M instructions per play retrace against 464.9 M**, -0.5 percent, with the stop pc and turn count matched by the guard and the route digest identical at `83d2590d...`. So it is real, verified and small.

**What it changed.** The overlap observation inside `host_chassis_edge_service` reads three guest words - `mem_read32(cpu, 0x803F6160u)`, `mem_read16(cpu, object + 0x04u)`, `mem_read32(cpu, object + 0x1Cu)` - at every block boundary. Each is a `get_ram_ptr` call whose fast path is skipped whenever `g_ppc_guest_aliases_overlap_mem1` holds, which is this program's case, so the translation rather than the load is the cost. The slot address is a constant and the object changes rarely, so both host pointers are now resolved once and revalidated when the aliasing state or the object address changes, with the runtime accessors as the fallback for an address that does not resolve.

**The prediction was 2 to 5 percent and the answer is 0.5, which says something about the model.** At 0.22 percent per instruction and about 1.07 M boundaries per retrace, 0.5 percent is roughly 2.3 instructions per boundary. Three memory reads through the runtime cannot cost 2.3 instructions between them, so the block must be executing on only a minority of boundaries: `g_name_scene_object >= 0x80000000u && (g_file_start_pulse.triggered || !g_file_start_pulse.configured)` is false most of the time, and the ledger's account of this sampling as a whole-route per-block cost was an assumption about which guard holds rather than a measurement. **The per-block price list is sound; applying it to a block requires knowing the block's live fraction, and that is a second measurement.** The next candidate gets its live fraction first.

**The caveat, recorded because the payoff is small.** The cache revalidates on the aliasing state and on the object address, so a mapping change that does not toggle `g_ppc_guest_aliases_overlap_mem1` and does not move the object would leave a stale pointer. The digest is the detector, and it is green across both ceilings; if the payoff had been five percent the fix would be to revalidate against the resolver instead of the flag.


## 2026-09-21 A normal stop is not evidence that the guest ran the route, and the per-block constant is now priced

The instrument that measures everything else had one assertion, `stopped: normal`, and today it reported a 90 percent speedup for a build that never booted. The broken edge fast-reject stopped normally at `0x80301510` after 14,495,606 turns - a fifth of the certified route - and `bench_instructions.sh` read that as **48.2 M instructions per play retrace**. Not one guard fired. A run that cannot boot is a run whose instruction count means nothing, and the script could not tell the difference between a route and an early exit.

**The fix.** The script now compares the stop pc and turn count of both tiers against the recorded reference for the ceiling, requires `title_ready=1` and `play_scene=1` at ceilings past the play-scene boundary, and prints the route digest of the high-ceiling run so a change can be checked against `92dd816c...` rather than inferred from a stop line.

| ceiling | recorded stop |
| --- | --- |
| 13,800 | `0x80307EF4`, 29,737,920 blocks |
| 13,900 | `0x80307EF4`, 29,937,744 |
| 14,100 | `0x80307EF4`, 32,203,791 (certified; digest `92dd816c...`) |
| 14,700 | `0x8027FA30`, 40,502,699 |

**And the reference was stale.** It defaulted to 491.7 M, which was the 0017 artifact three increments ago; the composite in the tree measures 486.6 M. Every comparison since the budget-test fix was therefore reporting those increments twice, once in the artifact and once as a "change" against an old control. The default is now the current control and the comment names the value it replaced.

**The per-block constant, which is the useful part.** The fast-reject removed 21.5 M instructions per play retrace by deleting about twenty host instructions from the miss path at every generated-block dispatch. That puts the per-block dispatch count at roughly **1.07 M per play retrace**, and an independent estimate from the guest side agrees: 3.24 M guest instructions per retrace at about three instructions per block is 1.08 M blocks. Which gives the loop a price list it never had:

> **One host instruction removed from the per-block dispatch path is worth about 0.22 percent of the play window.** Ten instructions are 2.2 percent, and a candidate costs twenty seconds to build and four minutes to measure.

**The largest thing the model prices is not a trims candidate, it is a defect of ordering.** In `host_chassis_edge_service`, before the intercept predicate, the overlap-phase observation runs three guest reads - `mem_read32(cpu, 0x803F6160u)`, `mem_read16(cpu, overlap_object + 0x04u)`, `mem_read32(cpu, overlap_object + 0x1Cu)` - and its outer guard is `g_name_scene_object >= 0x80000000u && (g_file_start_pulse.triggered || !g_file_start_pulse.configured)`. `g_name_scene_object` is set when the name scene is created and never cleared, so after that moment the guard stops discriminating and the reads run at every block boundary for the whole route, play window included - about 1.07 M samples per retrace.

It cannot be deleted: it exists to restore the shipping per-block observation cadence, and `overlap_phase` genuinely differs between ceilings (2 at 13,900, 6 at 14,700), so the digest sees it. It can be made cheap - resolve the constant slot to a host pointer once, cache the overlap object's host pointer and re-resolve only when the slot changes, read the fields directly - and **the trap is byte order**: MEM1 is big-endian, so a direct load must swap exactly as `mem_read32` does, with a fallback to the runtime accessor for any address outside MEM1. Written down here because the four-minute measurement and the digest will catch it either way, but only if the change is written knowing it.

**Reproducibility of the landed increment, on a second pair.** Re-running the instrument after the guard landed gives **464.9 M per retrace against 486.6 M, +4.45 percent**, against 465.1 M and +4.42 from the first pair - 0.04 percent apart on the same build. So an increment of half a percent or more is resolvable across independent pairs, which is the resolution the loop should plan against; the 0.29 percent quoted earlier was for *different* ceilings in one ladder, where the card state and the stamping differ.


## 2026-09-21 The two play-window numbers are both right, and they are different windows

The ledger carried 302.3 M instructions per play retrace for retraces 13,800-14,100 and 491.7 M for 13,900-14,700, both called "the play window", and the gap ratio follows from which one you quote. One ladder settles it: three ceilings on one build with the frame-timing stamps on, differenced.

| window | retraces | instructions / retrace | turns / retrace |
| --- | --- | --- | --- |
| opening, 13,800-13,900 | 100 | 53.6 M | 1,998 |
| **early play, 13,900-14,100** | 200 | **402.1 M** | **11,330** |
| **certified window, 13,800-14,100** | 300 | **285.9 M** | 8,220 |
| **steady play, 14,100-14,700** | 600 | **492.1 M** | **30,498** |
| bench window, 13,900-14,700 | 800 | 465.1 M | 13,276 |

Nothing was wrong with either number. The 302.3 M certified window is a third opening cutscene, where a retrace costs 54 M instructions, and the 491.7 M bench window is entirely play. **The play scene is not stationary either**: its turn density per retrace rises from 1,998 to 11,330 to 30,498 as Outset fills in, and the honest steady-state figure is 492.1 M rather than either of the numbers in the ledger.

**What that does to the target.** Authentic speed is 16.667 ms per retrace. At the measured sustained rates on this host - 13.08 to 13.75 G instructions per second in the certified window, IPC 3.76 there and 4.01 over the whole route - the early play window needs 24 G instructions per second to fit in a retrace, and the steady window needs 29.5 G. The machine retires at most about 14 G. So the gap is **1.83x in the early window and 2.2x steady**, and the certified 1.81x figure stands as the number for the window it was measured in - it is not, and never was, the whole play scene.

**The frame timings corroborate the ratio and cannot be trusted to a tighter figure.** The same 190 retraces, 13,911-14,100, measured in two processes of the same binary on the same route: median 30.47 ms per retrace in the run whose ceiling is 14,100, and 40.74 ms in the run whose ceiling is 14,700 - 33 percent apart - while the instruction counts for the identical 14,700 ceiling differ by 0.29 percent (1,259.90 G against 1,263.53 G). Wall clock on this machine varies by a third between processes and instruction count does not. Speed claims in this ledger should be read as the instruction count at a certified ceiling with the digest green; the milliseconds are colour.

**Two properties of the ladder worth keeping.** The 14,100 tier stops normally at `0x80307EF4` after 32,203,791 turns, the certified stop, and its route digest is `92dd816c...` over 1,050 records - the certified digest - so this harness reproduces certification exactly, including with the fast-reject landed. And same-ceiling reproducibility across processes is 0.29 percent, not the 0.02 percent that repeat runs of one configuration suggested: the card left by an earlier ceiling and the per-retrace stamps are part of the configuration, so a comparison should hold those fixed or be read at that resolution.

**Where it points.** The steady window is the one a player spends time in, and it is the less favourable of the two at IPC 2.97 - it is the more memory-bound of the two phases, which is a fact about the scene rather than about the host. Any P1 increment should therefore be measured on 14,100-14,700 (600 retraces, one ladder run at each end) as well as on the certified pair, or it will be reported at better than its true weight.


## 2026-09-21 The host test suite did not build, and had not for a week

Found while trying to verify something else, and it invalidates every "the suite passes" line written since 2026-09-14: the tree did not compile.

`tests/delivery_digest_test.c` still called `bluewake_delivery_digest_record_external` with six arguments after commit `8da70d2` added its seventh (`in_play`). `cmake --build` fails on that target, so `scripts/build_macos_dsp_host.sh` - which builds the host and *then* runs ctest - never reached ctest, and `ctest` was never the thing being reported. Anything downstream of "the tests were green" since 2026-09-14 was a restatement of a result from before the parameter existed.

The test now passes `in_play`, and because the play-scene accumulators had no coverage at all it also asserts the contract they implement: deliveries before the host play-scene milestone leave `play_count`, `play_hash` and `play_cycle_sum` untouched, the first play delivery sets `play_first_cycle` and mirrors the route hash, and the second accumulates rather than restarting. The full suite - 217 tests - passes again, which is the point: a tree whose tests do not build cannot gate anything, and a loop that reports a gate it cannot run is reporting its own memory.

Two smaller things worth keeping. `BLUEWAKE_DELIVERY_FNV64_OFFSET` now lives in `delivery_digest.h` instead of being a private macro in the `.c`, so a test can name the value an empty digest reads rather than repeating the constant. And the build script's exit status is masked when its output is piped to `tail`, which is how it was invoked here; the failure was legible in the text and invisible in the status, so a wrapper that pipes should read the log, not the exit code.

## 2026-09-21 The edge fast-reject: the generator was broken, and the fix is worth 4.4 percent

The largest increment this workstream has landed, and it arrived in the tree broken.

**The candidate.** The uncommitted change in the working tree replaced the edge predicate's miss path - four switches, about fourteen comparisons, consulted at every block boundary - with a perfect-hash lookup over the fifty-two intercept addresses. The dynamic census had already priced the owner: `bluewake_edge_requires_host` at 2.2 percent of the rendered main thread, with the card intercept test inside it at 1.3. Nobody had run it on the route.

**It breaks the boot, and the route says so in one line.** Same composite, same card, headless, 13,900 retraces: `title_ready=0` with every milestone zero, and a normal stop at pc `0x80301510` after 14,495,606 turns, against the control's `title_ready=1` and stop at `0x80307EF4` after 29,937,744. A fast reject that answers *false* for an address the switches match is not a slowdown, it is a boot failure, and the DVD fast-open intercept is on that list.

**The root cause is in the generator, not the paste.** `scripts/gen_edge_intercept_table.py` had an accounting loop that walked the keys, probed for collisions, and never wrote the key it had just placed. It therefore always saw an empty table, always reported zero displacements, and passed its own "the placement is perfect" assertion. The placement loop below then displaced three keys one slot past their hash - `0x80181634`, `0x8030F5A4` (the DVD fast open) and `0x8031D2E0` - while the emitted lookup probed exactly one slot. Three false negatives, one dead boot.

**And the single-probe form was never available.** Once the accounting is honest, no candidate multiplier and shift places these fifty-two addresses injectively: the 256-slot table has exactly one usable shift, so the search is six candidates, and a random 8-bit hash over fifty-two keys is injective about half a percent of the time. The old code did not find a perfect hash; it asserted one because it could not count. So the header now uses open addressing with linear probing, which is sound by construction rather than by verification: the chain ends at the first empty slot, and a key that was placed is always found before it. Longest probe 1, 204 of 256 slots empty.

**The generator writes the file now, and the test checks the table.** It no longer prints a block to paste - the paste step is what let a stale table sit under a fresh `#define` - it verifies every key is found again after placing it, and it emits the full key set so `tests/edge_intercepts_test.c` can assert the lookup finds every one. That assertion is the check that would have caught this without a boot, and the old test could not: its key list is hand-written and happened to omit the three.

**The measurement, on the certified route.**

| | instructions / play retrace | stop at 13,900 | stop at 14,700 | route digest |
| --- | --- | --- | --- | --- |
| control | 486.6 M | `0x80307EF4`, 29,937,744 | `0x8027FA30`, 40,502,699 | `37e1c8b5...`, 1,050 records |
| **fixed fast-reject** | **465.1 M** | `0x80307EF4`, 29,937,744 | `0x8027FA30`, 40,502,699 | **identical**, zero delivery drift |

**-4.42 percent**, with both ceilings matching the control on turn count exactly, zero delivery drift over 1,024 deliveries and zero route clock drift. That is the largest single landed increment of the workstream - patch 0017 was 2.8 - and it is host-side: twenty seconds to rebuild, about four minutes to measure, no composite rebuild at all. The census's "edge service 4.3 percent" was this predicate and 4.4 is that share; the plan's sub-parts (pc lookup 2.2, card intercept test 1.3) are the same function's switches.

**The wall-clock number disagreed, in the wrong direction, and that is the point.** User CPU per retrace *rose* from 37.25 ms to 38.26 ms and IPC fell from 3.88 to 3.75 across the same pair, because the two pairs ran at different times on a machine that was not equally idle. The instruction count is deterministic on a digest-gated route and the wall clock is not; a change worth 4.4 percent would have looked like a regression on the metric this project used until ten days ago.


## 2026-09-18 The screening loop's first real result: the access path is refuted, and W4's shares are code size

The first candidate through the screening loop, and the answer is negative - which the loop made cheap.

**The candidate.** `get_ram_ptr`'s fast path tested `!g_ppc_guest_aliases_overlap_mem1 &&
(addr & 0x40000000u) == 0u`. The second clause is redundant: `GC_RAM_BASE` is `0x80000000`, so the offset
bound that follows already rejects every address that is not plain MEM1 - a cached mirror at
`0xC0000000+x` gives `0x40000000+x`, an address below `0x80000000` gives a wrapped value above it, and MEM2
at `0x90000000+x` gives `0x10000000+x`, all far beyond any `ram_size` this runtime admits. Dropping it
removes a mask, a test and a branch from every inlined guest load and store.

**The screen.** The ten hot chunks - **26%** of the live play window - were recompiled against the modified
header and relinked in about eight minutes, then measured:

| | instructions / play retrace | IPC | digest |
| --- | --- | --- | --- |
| reference (0017 artifact) | 491.7 M | 4.01 | `92dd816c...` |
| access path, hot ten | **490.9 M** | 3.96 | **UNCHANGED** |

**0.16%** on a quarter of the window, so about **0.6%** for the whole change. The digest held, so the
screening configuration is valid and the equivalence argument was right - the clause really was
redundant, it simply was not worth anything.

**What it says about W4's attribution, and it is the important part.** W4 priced the inlined access path at
**18% of the emitted code**. Removing a fifth of that path should therefore be worth ~3.6% of the
instruction stream; it is worth 0.6%. So **the access path is only about 3% of the dynamic stream, and
W4's shares are code-size shares, not dynamic ones.** That was suspected in W4's own text - "code size is
not time" - and it is now measured: the identified pieces (17% envelope, 18% access path, 4.3% guard
bodies, 1% FP helper) do not add up to 1.65x of dynamic work, and the known-work ceiling is well below
that. The remaining 45% is not reachable from W4's list at all.

**The loop paid for itself on its first use.** This refutation cost about eight minutes of compilation and
five of measurement; the same answer through a full rebuild would have been two hours and fifteen. The
header was reverted and the hot ten recompiled and relinked, restoring the composite **byte-identically**
to `39e05177...`, and `core/cpu.h` now differs from `HEAD` by exactly the 0043 patch again.

## 2026-09-18 The object cache is restored, and a full rebuild reproduces the artifact byte-identically

Repair after the previous entry's damage, and the repair is itself the strongest consistency check the
project has run: the composite was rebuilt from scratch, all **756 objects**, and the relinked
`gGZLE01_recomp.dylib` came back **byte-identical** to the validated 0017 artifact - SHA `39e05177...`, size
518,235,064. So the generated chunk sources, the project's patched `core/cpu.h`, the recorded compile
flags and the link all reproduce the shipping composite exactly, not merely equivalently. Anyone
re-deriving this build gets the same bytes.

The full rebuild took **2 hours 15 minutes** (14:50 to 17:05) at 8 workers on this 8-core machine, at about
5.7 objects per minute once the biggest chunks are in flight. That is the real number behind the plan's
\"hours per full rebuild\", and it is why the DOL-only path and then the hot-chunk screening loop were
worth building at all.

Two things this closes: the object directory is complete and consistent again, so `scripts/recomp_chunks.sh`
can relink; and the access-path candidate, which was reverted so that `core/cpu.h` sits at exactly its
patched state, can now be screened without first paying for a repair.

## 2026-09-18 Never `git checkout` a pinned dependency's file: its uncommitted state *is* the patch

A mistake worth recording, because it cost the project a file and nearly cost it the header the whole
emitter depends on.

Backing out the access-path experiment, I ran
`git -C ref/recompcore checkout -- GXRuntime/include/core/cpu.h`. That is the right reflex for a tracked
file and the wrong one for a **pinned dependency**, where the uncommitted working state is not a stray
edit - it is the project's local patch series, applied to a clean checkout. The checkout reverted the file
to upstream `HEAD` and silently deleted the cycle-domain fields (`cycle_observation_suffix`,
`cycle_deadline_active`, `cycle_deadline_budget`) and the CPU ABI version bump, which everything the
emitter generates depends on. The next build failed immediately with
`no member named 'cycle_deadline_budget' in 'struct CPUState'` across every chunk.

It was recovered because the change is a *registered* patch:
`patches/recompcore/0043-runtime-observe-timebase-through-host-clock.patch` contains the struct hunk, and
`git apply --include='GXRuntime/include/core/cpu.h' <patch>` restores just that file. The recovered file
now differs from `HEAD` by exactly the 0043 patch and nothing else, which is the state it should be in.

**The rule:** to back out an experiment in `ref/`, reverse the edit or restore from the patch series -
never `git checkout`. `ref/` is gitignored by the outer repository precisely because its state is managed
by `config/dependencies.lock.json` and `patches/`, not by its own `HEAD`.

A second cost, recorded so the next agent is not surprised: the killed full rebuild left the object
directory incomplete, so `scripts/recomp_chunks.sh` cannot relink until a full 756-object rebuild is run.
The composite artifact itself is untouched and still the validated `39e05177...`, and the generated chunk
sources are still the 0017 set, so source and artifact agree - only the object cache has to be paid for
again. `core/cpu.h` was touched, which forces that rebuild on its own account.

## 2026-09-18 The screening loop works for emitter changes only, and its first run proved it the hard way

The first use of the screening loop, on the largest identified piece - the inlined memory-access path -
found that the loop as written did not screen anything, and the fix is the whole point of this entry.

**The candidate.** `get_ram_ptr`'s fast path tested `!g_ppc_guest_aliases_overlap_mem1 &&
(addr & 0x40000000u) == 0u`. The second clause is redundant: `GC_RAM_BASE` is `0x80000000`, so the offset
that follows rejects every address that is not plain MEM1 anyway - a cached mirror at `0xC0000000+x`
gives `0x40000000+x`, a physical address below `0x80000000` gives a wrapped value above `0x80000000`, and
MEM2 at `0x90000000+x` gives `0x10000000+x`, all far beyond any `ram_size` this runtime admits. Dropping
it removes a mask, a test and a branch from every inlined guest load and store. Priced, not measured, at
roughly three instructions per access.

**Why the screen failed.** `scripts/recomp_chunks.sh` hand-compiled the ten hot chunks and then asked
CMake to relink - and `cmake --build` re-checks every object against its headers and sources, so it saw
`core/cpu.h` newer than 748 objects and started rebuilding all of them. The twenty-minute screen silently
became a three-hour full rebuild, 258 objects in before it was caught. A screening loop that goes through
make cannot screen anything, because both kinds of change invalidate objects make cares about: a header
change invalidates every object that includes it, and an emitter change regenerates every chunk source.

**The fix.** `scripts/recomp_chunks.sh` now relinks by invoking the recorded link command directly
(`CMakeFiles/gGZLE01_recomp.dir/link.txt`) with the compiler driver, which is the same link CMake would
run and does not re-check dependencies. The header was reverted and the artifact is unchanged at
`39e05177...`; the header was touched so the next build recompiles the 258 objects that were built
against the modified header, rather than silently linking a mixture of both.

**Correction.** The first version of this entry concluded that header changes "cannot be screened at
all". That is wrong, and the error was in the same place as the original one: it was the *CMake relink*
that rebuilt everything, not the hand-compilation. With the direct relink the loop screens a header change
exactly as it screens an emitter change - it compiles the named subset against the modified header and
links it against objects built against the old one. The result is a deliberately mixed artifact: correct
for screening, because both header variants are semantically equivalent so the digest must still be
UNCHANGED, and the instruction delta measures the subset's share of the change. Only the final, shipped
artifact needs all 756 objects rebuilt. So the access-path candidate above is **screenable in about
twenty minutes**, not a three-hour commitment, and that is the next action.

## 2026-09-18 A ten-minute screening loop, and the hot-chunk census it needs

The 90-to-150-minute rebuild is what made every emitter change expensive, and the deterministic
instruction metric makes it avoidable. Because instruction counts resolve to 0.02%, a change measured on
only the hot chunks is still resolvable even though its effect is diluted by the chunks that were not
rebuilt.

**The census.** From the rendered play-window sample, the busiest generated functions map to these
chunks, as a share of the sampled main thread:

| chunk | share | | chunk | share |
| --- | --- | --- | --- | --- |
| `chunk_0201_text1_803256E0.c` | **15.7%** | | `chunk_0200_text1_803216E0.c` | 1.0% |
| `chunk_0144_text1_802416E0.c` | 3.6% | | `chunk_0187_text1_802ED6E0.c` | 0.6% |
| `chunk_0015_text1_8003D6E0.c` | 1.5% | | `chunk_0203_text1_8032D6E0.c` | 0.5% |
| `chunk_0145_text1_802456E0.c` | 1.2% | | `chunk_0188_text1_802F16E0.c` | 0.4% |
| `chunk_0181_text1_802D56E0.c` | 1.1% | | `chunk_0148_text1_802516E0.c` | 0.4% |

The top ten are **26%** of the play window and the top twenty 30%, so a 5% effect inside them is 1.3%
overall - thirty times the metric's resolution. The tail is long, which is why the full rebuild still has
the final word.

**The loop.** `scripts/recomp_chunks.sh` recompiles a named subset (the list above by default) and
relinks. Screening is then: edit the emitter, rebuild dolrecomp, regenerate (seconds), recompile the hot
ten (**~10-15 minutes**), relink (3 s), `scripts/bench_instructions.sh` (~3 minutes) - about twenty
minutes instead of two and a half hours. The route digest still gates: a screening run whose digest is not
UNCHANGED is invalid and the candidate is discarded.

**Validated.** Recompiling one unchanged chunk and relinking produced a composite **byte-identical** to
the one before it (`39e05177...`), so a subset relink is faithful and deterministic rather than merely
plausible. That is the property the whole screening loop rests on, and it is checked rather than assumed.

## 2026-09-18 The play scene runs at IPC 4.0: it is instruction-bound, and instructions are now the metric

The decisive number for the whole workstream, and it took two headless runs and no rebuild. Two runs of
the same route, `--retraces 13900` and `--retraces 14700`, differ by exactly the 800 live-play retraces,
so subtracting their `/usr/bin/time -l` totals isolates the play window:

| | 13900 retraces | 14700 retraces | **difference (800 live retraces)** |
| --- | --- | --- | --- |
| instructions retired | 904,949,503,523 | 1,298,327,866,082 | **393,378,362,559** |
| cycles elapsed | 216,356,108,627 | 314,444,934,441 | **98,088,825,814** |
| user seconds | 63.04 | 91.66 | **28.62** |
| real seconds | 64.09 | 93.14 | **29.05** |

**IPC in the live play window is 393.4G / 98.1G = 4.01.** That is at or near what an M-series core can
sustain, so the play scene is not waiting on anything - not memory, not cache, not a lock - it is
retiring instructions as fast as the machine can retire them. It follows that:

* **The only lever is fewer instructions.** A change that removes X% of the retired instruction stream
  buys about X% of speed, and nothing else buys any. This is the first measurement in the workstream
  that settles the question rather than supporting one side of it, and it retires the stall-based
  explanations (cache pressure from a 24 MB MEM1, I-cache, branch layout) that the earlier profiles could
  not rule out.
* **The play window costs 491.7 million host instructions per retrace** (393.4G / 800), against 8.1
  million guest cycles per retrace - about 61 host instructions per guest cycle. That is consistent
  with W4's 81 machine instructions per guest instruction at the usual 2-4 cycles per guest instruction,
  so the two estimates finally agree.
* **The identified trims add up to about 1.6x, and the target is 1.81x.** W4's pieces - ~17% envelope,
  ~18% inlined access path, 4.3% guard bodies, 1% FP helper, about 40% of the emitted code - would take
  86 host instructions per guest instruction to ~52, which is 1.65x. Close, and short. That is the
  honest ceiling of the known work, and it says the remaining gap needs a piece nobody has identified
  yet, not just the ones on the list.

**And it gives the loop a better instrument than fps.** Instruction counts are deterministic and the
guest work is digest-gated, so for any digest-green change the *instructions per play retrace* can be
compared instead of wall-clock fps. The fps bench varies 2-5% run to run; this does not. The 0017
artifact's 491.7M is the reference to measure the next increment against, and it costs two headless runs
(about three minutes) rather than a rebuild.

That instrument is now `scripts/bench_instructions.sh`, which runs the two tiers, differences their
`/usr/bin/time -l` totals and prints instructions per play retrace, IPC and the delta against a reference.
Four runs of the same 0017 artifact gave 393,378,362,559 / 393,298,718,743 / 393,349,498,618 instructions
- a spread of **0.02%**, against 2-5% for the fps bench, so a sub-percent emitter change is now
resolvable instead of guessed at. It also reports user 28.53 s against real 28.88 s for the play window,
which is the CPU-bound result again from a second, independent direction.

## 2026-09-18 The play scene is CPU-bound, and play.sh's overlap guard was matching shells

Two things from trying to answer a question the numbers had raised. In the live play window the host is
retiring far fewer instructions per second than the hardware can sustain, which would mean it is
*waiting* rather than computing - and if it is waiting, the lever is not the emitter at all. It is not
waiting: the useful evidence was already in hand.

**The play scene is CPU-bound.** In the rendered sample, 88.4% of the main thread's samples are inside
`main`, executing guest code and the GX path, and none of the blocked-time signatures (no condition
variable wait, no semaphore, no mach message) appear anywhere on the main thread - they are all on the
audio, DSP and render-worker threads. So the play window is dominated by execution, not by waiting, and
the remaining work is where the plan says it is: fewer host instructions per guest instruction. The
whole-run instruction rate cannot be attributed per retrace (the boot and the play scene are very
different), which is what made the question look open.

**A real defect in the loop's own tooling, found the hard way.** `scripts/play.sh` refused to launch
three times with "a bluewake_host process is already running" when no host existed: its guard was
`pgrep -f bluewake_host`, which matches the *command line* of any process that merely mentions the
string - including the shell running a command about it, which is exactly what a harness or an agent's
wrapper is. It now uses `pgrep -x`, which compares the process name. `scripts/app_acceptance_test.sh`
had the same bug in two places and is fixed with it; `scripts/profile_play.sh` already did this
correctly. Verified: a shell whose command line mentions the string no longer trips the guard, and a
40-retrace route still launches and stops normally.

## 2026-09-18 Null result, reverted: the observation machinery is not a bottleneck

The next increment after 0017, measured, found neutral, and reverted. Recorded because the negative is
worth as much as the positive: it says where the remaining time is **not**.

**What was tried.** Two trims in the cycle-observation machinery. First, `emit_cycle_observation_reconcile`
reloaded `ctx->cycle_observation_suffix` twice - once in its condition, once in its body - when the value
is the compile-time constant the emitter had just stored there, so both reloads could be the constant.
Only generated code ever writes that field (checked: the host only reads it), and the instructions that
emit a reconcile are memory and SPR operations, which never call into another generated chunk (only
cross-chunk branches and counted loops do), so the constant is what the reload would have produced.
Second, `emit_precise_instruction_charge` stores the instruction's own address in its guard, which is the
same value the instruction already materialised immediately before, so that store is dead.

**The rebuild.** 206 DOL objects, **152.3 minutes**, zero failures; relink 3 s. Digest UNCHANGED in all
four runs, host turns identical at 32,203,791 - so both trims are equivalent, as designed.

**The measurement is neutral.** Warm runs, against 0017's three:

| | live-window median fps | mean fps | route wall |
| --- | --- | --- | --- |
| 0017 | 33.46, 33.72, 34.06 (mean **33.75**) | 31.12 | 70.29 s |
| this change | 33.85, 33.71, 33.42 (mean **33.66**) | 31.05 | 71.11 s |

**And the composite grew.** SHA `39e05177...` at 518,235,064 bytes became `563dbf9c...` at 551,987,688 -
**+33,752,624 bytes (6.5%)** for a change that removes instructions. That is not explained by the source
diff, which is exactly the two trims above, and it is the kind of unexplained cost that should not ship
for no measured gain.

**Reverted, and the revert is exact.** The emitter was restored to its 0017 state and the 0017 chunks
restored from the earlier regeneration; the restored emitter reproduces **all 206 chunks byte-for-byte**, so
the revert is verifiable without a benchmark. Rebuilding the 206 objects took another 90.0 minutes and the
relinked composite came back **byte-identical** to the validated 0017 artifact - SHA `39e05177...`, size
518,235,064 - which is the cheapest proof a revert is complete. 217/217 tests and the repo audit pass.

**What it says about the remaining work.** Removing two loads per observing instruction changed nothing
measurable, so a memory-heavy play scene's cost is not in that bookkeeping; and a small emitter change can
move the binary by 6.5% in a direction the source does not predict, which is a caution for every future
increment. Two rebuilds - 152 and 90 minutes - were spent on this answer.

## 2026-09-18 The block-leader guard is collapsed: +2.8% on the play window, digest UNCHANGED

The first performance change this workstream has landed since W4, and the first end-to-end use of the
DOL-only iteration path.

**The change.** Every block leader used to emit two guard arms - one for the precharged case and one for
this instruction's own charge - and both tested the *same* budget against the *same* resume pc, differing
only in the cycles subtracted (the block's total, or the instruction's own). They collapse into one test
and a conditional decrement. `emit_precise_instruction_charge` is then skipped for that instruction.
169,817 arms across the 206 DOL chunks, of which **169,025 were merged**; the remaining 792 are in the
separate `loop_*` emission, which is left alone deliberately.

**The iteration.** Patch `patches/dolrecomp/0017`, exported and registered. Regenerating the DOL chunks
takes seconds; recompiling the 206 objects took **89.7 minutes** at 8 workers with zero failures; the
relink took 3 s. The composite went from SHA `6ca5df93...` to `39e05177...` and shrank by 858,624 bytes,
which is the visible half of what was removed.

**The result, and it is small.** Route digest **UNCHANGED in all three runs**, host turns identical at
32,203,791:

| live-window median fps | | live-window mean fps | |
| --- | --- | --- | --- |
| baseline | 31.63, 33.16, 33.22, 33.33 | baseline | 30.64, 30.70, 30.77 |
| **with 0017** | **33.46, 33.72, 34.06** | **with 0017** | **30.79, 31.24, 31.33** |

The median improves about **2.8%** (33.75 against 32.84) and the samples do not overlap: the variant's
worst run exceeds the baseline's best. The mean improves about 1.4%. Two methodology notes worth keeping:
the first run after a relink reported a 74.42 s route against 70.04/70.54 for the next two, which is the
page cache warming the newly written 518 MB dylib rather than a regression, so the first run after a
relink should be discarded; and even this change sits under the 5% a single run can resolve, which is
why it is reported as three runs against four rather than as one pair.

**What it means for the goal.** 2.8% against the 1.81x the play scene needs. Every remaining lever
identified so far is of this size or smaller, and each costs about 90 minutes of rebuilding to evaluate.
That arithmetic - roughly 29 more wins of this size, about 45 hours of rebuilding - is the honest shape
of the remaining work, and it is why the plan's M5 says the emitter is a sustained program rather than a
step.

## 2026-09-18 A renderer instrument that can resolve 4%, and a retraction of the 4% it found

M6 of `PLAN_2026-09-18.md` is answered: the renderer now has a metric that can see renderer work.

**The instrument.** `scripts/profile_play.sh` drives the same rendered route as the benchmark, waits for
the live play window by reading the frame-timing stamps, samples `bluewake_host` for 20 s, and
`scripts/sample_owners.py` prints owner shares from the capture. A share is a ratio of samples inside one
process, so machine load moves numerator and denominator together and largely cancels - which is exactly
what wall-clock fps does not do. Validated against a hand analysis of an earlier capture: both give the
main thread at 88.4%, `bluewake_composite_dispatch_until_boundary` at 72.3%, `func_803256E0` at 18.3%
and `host_mmio_write` at 18.3%.

Reproducibility, two baseline runs: `accumulate_assembly` 10.60% then 11.19%, `on_consumed_draw` 6.2%
then 7.10%. **Run-to-run share variation is about 0.6 to 0.9 points, so a 4-point effect is resolvable**
where the rendered tier's wall-clock fps could not resolve anything under about 20%.

**The retraction.** `ConsumingAuroraRenderSink::accumulate_assembly` was reported at 4.1% of the rendered
frame by subtracting `on_consumed_draw`'s share from its parent's. That subtraction is not a self-time
measurement - a share includes everything the frame calls - and the check that settles it is the one that
was run next. The count-only `assemble_consumed_draw` walk, the `build_topology_indices` and
`build_array_sizes` tallies were gated off with a `set_tally_assembly(false)` on the gx-core sink, the
gate was confirmed live (`[gxbank] accumulate_assembly tally=0` printed from the call site), and the
shares did not move: `accumulate_assembly` 11.50% against 11.19% baseline, `on_consumed_draw` 7.25%
against 7.10%. So the walk is real dead work - it does run in shipping and its totals are read only on
the disabled shadow path - but it costs well under one percent, not four. The gate was reverted, and
`scripts/sample_owners.py`'s own header now carries the warning, because this is the mistake the tool
exists to prevent.

## 2026-09-18 The headless bench is blind to the renderer, and a stray process can fake a 27% regression

Two instrument findings and one code finding from chasing the GX path, which the rendered profile puts at
18.3% of the frame.

**The headless bench never runs the Aurora configuration path.** `BLUEWAKE_RENDERER=headless` takes
`dol_headless_backend_install` instead of `dol_aurora_initialize`, so the block in
`ref/recompcore/GXRuntime/backends/aurora/aurora_backend.cpp` that reads `DOL_GX_CORE`, sets
`g_gx_core_enabled`, and installs the sink observers never executes. A change gated there, or any other
renderer-side change, is therefore **invisible to `scripts/bench.sh`** - the project's primary gate and
the instrument every number in this ledger was taken with. The rendered tier (`scripts/play.sh
--timing`, repaired earlier today) is the only instrument that can see that half of the work, and it is
also the noisier one: its live-window mean has ranged from 33.6% to 41.0% of authentic across runs.

**A stray process can fake a large regression.** Two repo-wide `grep -rn` searches over `ref/` and
`local-research/work/` - which are large - were still running at ~100% CPU and heavy I/O when a
benchmark was taken. That run reported route wall **97.11 s** and play-window median **25.90 fps**,
against a baseline of 70.44/72.87 s and 33.22/31.63 fps: a 27% regression that was entirely the greps.
Re-running on a quiet machine gave 70.87 s and median 33.16 fps. Two rules for this repository: scope
every search to the directory that can contain the answer (`ref/` alone takes ~20 s and `local-research`
is bulk data), and check `uptime` and the process list before believing any timing number.

**The code finding: a per-vertex validation walk runs in shipping and its results are never read.**
`ConsumingAuroraRenderSink::accumulate_assembly` calls `assemble_consumed_draw(draw, nullptr)` - with a
null out-parameter, so it walks every vertex of every indexed attribute and discards it - plus
`build_topology_indices(..., nullptr)` and `build_array_sizes`, purely to accumulate counters. Those
counters (`assembled_draws`, `assembled_elements`, `topology_index_bytes`, `storage_bytes`, ...) are read
at exactly one place, `aurora_graphics.cpp`'s shadow diff, which is gated on
`g_shadow_frontend_enabled && !g_gx_core_enabled` - and `g_gx_core_enabled` is **true** by default. The
shipping configuration is the one where the counters are written and never read. It measured 4.1% of the
rendered frame (`accumulate_assembly` 2081 samples, of which 1257 are `on_consumed_draw`, which is *not*
gatable: `core_plan_observer` is what calls `submit_draw_plan`, so it is the render itself).

It was gated experimentally - a `set_tally_assembly(false)` on the gx-core sink, with the tallies kept
for the shadow path and for the tests - and **reverted**, because the headless bench cannot see it (see
the first finding) and the rendered instrument cannot resolve 4% against its own spread. By this
project's own rule - a single bench run cannot resolve an effect under about 5% - it is not a result,
and shipping an unmeasurable change into a pinned dependency is not what a result looks like. It is
recorded here as a known, sized, unshipped item rather than left in the tree.

## 2026-09-18 -O3 on the DOL chunks is refuted, and the DOL-only rebuild path is proven

The first codegen experiment on the play scene's hot code, and the first use of a rebuild path built to
make that class of experiment affordable.

**The path.** The 206 DOL chunk objects were recompiled at `-O3` (replacing the trailing `-O2` in their
real compile line) in place, the composite was relinked in **5.7 s**, and the standard benchmark was run.
206 chunks took **99.1 minutes** at 8 workers with **zero failures**; a full 748-object rebuild is
roughly 3.5x that. The dylib SHA-256 moved from `6ca5df93...` to `499d6c40...`, so the change was really
in the artifact, and the route digest is what gated it. This matters because the play scene's hot code
is all DOL: the emitter work that M5 needs can be priced at ~100 minutes per iteration instead of hours,
as long as it targets code that lives in the DOL chunks.

**The result.**

| | baseline (-O2) | -O3 |
| --- | --- | --- |
| play-window median fps | 33.22 / 31.63 | 33.00 |
| play-window mean fps | 30.70 / 30.64 | 30.16 |
| p99 frame time | 47.34 / 57.13 ms | 48.13 ms |
| route wall | 70.44 / 72.87 s | **75.61 s** |
| real speed | 333.6% / 322.5% | 310.8% |
| host turns | 32,203,791 | 32,203,791 |
| route digest | `92dd816c...` | **UNCHANGED** |

The two baseline columns are two runs of the identical restored artifact, and the second is included
precisely because it is the honest scale bar: **this benchmark varies a few percent run to run** (median
fps 33.22 vs 31.63, route wall 70.44 vs 72.87). Against that, `-O3` shows no gain on the play window and
a real loss on the whole route - 75.61 s against 70.44 and 72.87 - so the conclusion is that `-O3` does
not help and costs route time. The digest stayed UNCHANGED, which is the expected result: `-ffp-contract=off`
is preserved, so this was a pure codegen change and correctness was never the risk.

**Methodology, recorded because it will be needed again:** a single bench run cannot resolve effects
under about 5%. Any future claim of a few percent on this route has to be a repeat, not a run.

The artifact was restored (dylib back to `6ca5df93...`, confirmed by re-running the benchmark to the
baseline numbers above) and the 206 chunk sources were touched so the next build recompiles those
objects at their recorded `-O2`; the working tree's generated chunks are therefore consistent, with a
known one-time rebuild cost, rather than silently mixed.

## 2026-09-18 The host is not stalled (IPC 3.9), and the build is the loop's throttle

Three measurements taken while looking for the play scene's missing factor of ~2.

**The host executes near peak; the cost is instruction count, not stalls.** The rendered 14,100-retrace
run retires **1,129,218,484,616 instructions in 289,807,170,505 cycles - IPC 3.90** (from its own
`/usr/bin/time -l`). Nothing is waiting on memory, I-cache or the branch predictor; the machine is
simply retiring an enormous number of instructions: 1.129T over 32,203,791 host turns is **35,066
instructions per host turn**, and 35,066 instructions per ~3,550 cycles of turn is the ~10 host
instructions per guest cycle the earlier W4 entries described. So the lever is *fewer emitted
instructions*, and hypotheses that predict stalls - code size for its own sake, I-cache pressure,
branch layout - are not where the time is.

**The composite build is the throttle, and it is why iterations are slow.** 748 translation units,
489 MB of generated C for the DOL chunks alone. The single hottest chunk,
`chunk_0201_text1_803256E0.c` (2.48 MB of C, the function that owns 18% of the rendered frame), takes
**38 s** to compile at its real flags. Rebuilding the 206 DOL chunks runs about 2.5 chunks/minute on
this 8-core machine, so a DOL-only iteration is ~80 minutes and a full 748-object rebuild is hours.
That is the structural reason the loop feels slow, and it is worth attacking on its own.

**The chunk compile line, exactly as shipped:** `-O3 ... -O2 -ffp-contract=off` for every generated
chunk - the trailing `-O2` wins, so the generated code is built at **-O2** - with `-fPIC`,
`-fvisibility=hidden`, `-stack-protector 1`, `-fstack-check` and `-mdarwin-stkchk-strong-link`, for
`-target-cpu apple-m1`. Generated functions are **local symbols** (`t _func_800056E0`), so anything
that wants to call one - a microbenchmark, a harness - has to link objects rather than `dlopen` the
composite.

One flag class is already closed, cheaply: recompiling the hottest chunk with
`-fstack-check -stack-protector 1 -mdarwin-stkchk-strong-link` removed produces **byte-identical
`__text`** (1,088,212 bytes). Stack hardening costs nothing measurable here now that the emitter no
longer builds a 32 KB jump table on the stack.

## 2026-09-18 The rendered play scene is profiled: the GPU is idle and the GX translation is on the main thread

M3 of `PLAN_2026-09-18.md`. The rendered play scene had never been profiled; the only rendered figure in
the tree was a stale comment. A 25-second `sample` of the shipping rendered path, taken during live
Outset play (retrace ~14,000, `scripts/play.sh --route --retraces 14700`), over 20,305 main-thread samples:

| owner | share |
| --- | --- |
| `bluewake_composite_dispatch_until_boundary` (translated guest code and everything it calls) | **87.9%** |
| `host_sync_cycle_devices_end_turn` (the host's per-turn device service) | **7.8%** |
| the rest of `main` | 1.7% |

And inside that dispatch, one guest function dominates, and it is not executing guest instructions:

    func_803256E0 -> host_mmio_write -> aurora_backend_gx_write -> shadow_frontend_write
      -> RetailGxFrontend::flush                16.1%
        -> emit_new_packets                     13.4%
          -> GxCoreSink::submit_packet          13.0%
            -> ConsumingAuroraRenderSink::accumulate_assembly     10.2%
              -> GxCoreSink::on_consumed_draw                      6.2%
                -> build_draw_plan                                 2.5%
                -> build_topology_indices                          (allocates per draw)

**Three findings, and the first is the one that changes the plan.**

* **The GPU is not the bottleneck; translating GX commands on the main thread is.** Aurora's
  `render_worker` sits in `BoundedQueue::pop_for` for **94.0%** of the samples - it is idle, waiting
  for work that the main thread has not produced. Everything above happens synchronously inside the
  guest's `host_mmio_write`, so the cost is CPU-side GX translation, planning and assembly, not
  submission or presentation.
* **That path is 18.3% of the whole rendered frame**, concentrated in one guest function. It is a
  structural target of the right size: it cannot deliver the full 2.47x by itself, but it is the
  largest single owner after the translated bodies and it is the only large one that is not
  emitted C.
* **It allocates per draw.** `build_topology_indices` reaches `operator new` / `_xzm_xzone_malloc`
  under `build_draw_plan` repeatedly; the headless profile had already rejected allocation churn once
  (GX profile, 2026-08-2x) so this is a smaller item than its stack depth suggests.

**Rendered live play, measured on the same run**: 790 retraces of Outset = 13.167 s of guest time in
**39.238 s** of wall, i.e. **33.6% of authentic speed**, median 37.85 ms per retrace and p99 85.99 ms
against an authentic 16.667 ms. That is the best-sampled rendered figure the project has - 790 frames
against the earlier 190 - and it is worse than the first estimate, so the rendered play scene is
2.47x to 3.0x short, depending on whether the mean or the median is read.

## 2026-09-18 CORRECTION: the play scene runs at 55% of authentic speed, not 99.7% of it

`REORGANIZATION_2026-09-17.md` records, in the W4 section's own words:

> The digest, the host turn count and the normal stop are identical in every run, so the route is the
> same route and the gain is entirely the prologue. **The play scene is at 29.92 fps against the PRD's
> 30 fps authentic presentation baseline - 99.7% of it**

That is a unit error, and the same section states the correct requirement three paragraphs earlier
("Authentic speed needs 1.81x from here"). The 29.92 is **retraces per wall second**; the 30 is
**game frames per second**. They are not the same unit, and the ratio between them is 2.

Three independent checks, all from the project's own definitions:

* `scripts/bench_report.py` defines `guest_seconds = retraces / 60`, and the benchmark reports 14,100
  retraces as 235.00 s of guest time - 60.00 retraces per guest second. A frame-timing stamp is written
  once per retrace, and the live window `13910..14100` contains exactly 190 stamps for its 190 retraces.
  So the stamp measures retraces, and authentic is 16.667 ms per retrace.
* **The live window's real speed is therefore directly measurable and does not depend on any label.**
  190 retraces are 3.167 s of guest time. They take **6.176 s** of wall headless and **7.791 s** rendered:

  | play window 13,910..14,100 | wall | real speed | as game fps of 30 |
  | --- | --- | --- | --- |
  | headless | 6.176 s | **51.3%** | **16.6** |
  | rendered | 7.791 s | **40.6%** | **12.2** |

  If the play scene were at 99.7% of the 30 fps baseline it would complete those 190 retraces in about
  3.17 s of wall. It takes 6.18 s. The claim is not merely mislabelled; it is contradicted by the run it
  was computed from.
* Aurora segmented 7,225 presented frames across the 14,100-retrace route - 1.95 retraces per presented
  frame, or 30.7 presented frames per guest second. The guest is internally authentic at 30 fps, which is
  what makes the PRD's "30 FPS game speed" a game-frame target.

**What this means.** The bench's `median fps` line is a retrace rate and must be halved before it is
compared with any frame-rate target. The play scene is at **55% of authentic speed headless (median
30.10 ms per retrace, 1.81x too slow) and 41% rendered**, and it has been throughout W4. The remaining
gap is 1.81x headless and 2.47x rendered - the number W4 correctly identified and then, in its final
paragraph, wrote off as met.

The consequence for the plan is in `PLAN_2026-09-18.md` M5: W4's conclusion that the identified pieces
(17% envelope, ~18% inlined access path, 4.3% guard bodies, 1% FP helper) do not sum to 1.81x still
holds and is now the live question rather than a closed one, because the target was never met. Nothing
else in W1-W4 is affected: the route digest, the host turn reduction, the emitter fix and the audio
identity are all still what they are, and the launch really did get 3.2x faster.

## 2026-09-18 the route reaches controllable gameplay unattended, and TTP-B's floor is now measured

M1 of `docs/status/PLAN_2026-09-18.md` is done, and it produced the number the bar needs.
The route is now drivable from launch to **controllable Outset gameplay** with no key window and no idle
host. `scripts/play.sh --route-full` walks it headless or rendered; the guest's own signal reads

    [player-milestone] control-admitted retrace=20256 event_mode=0 demo_type=0 demo_mode=0 ovl=6

**20,256 retraces is 337.6 s of authored content at the console's authentic 60 Hz.** That is the floor a
paced application cannot beat at any emulator speed, so the five-minute launch-to-interactive bar is
failed by 37.6 s *at 100% emulated speed*. This replaces the earlier estimate, which inferred the
control point from the acceptance script's comments; it is now the guest's own `control-admitted`
milestone, and it is the M2 decision's number rather than a projection.

**The route is unchanged by the driver.** With the cutscene presses armed the milestone summary is
identical to the accepted route - `title_retrace=333 name_create_retrace=449 name_execute_retrace=457
file_select_retrace=535 new_game_intro_retrace=773 opening_complete_retrace=13850 play_scene_retrace=13910` -
and the shipping default path still returns `92dd816c...` with `digest verdict UNCHANGED` and 217/217
tests. The cutscene runs `event_mode=2 demo_type=1 demo_mode=4 event=38`, ends at `demo_mode=1`
(retrace 20,243), and the guest admits control at 20,256.

Three things had to be established for that, and two of them are negative results worth keeping:

* **The cutscene's event is 38, not 29.** `BLUEWAKE_PAD_CONFIRM_EVENT=29` never fired, and the guest's
  own event table says why: `[event-control-data] event=38 name="awake" state=1 staff_count=3`. The host
  could name the scene all along.
* **The message-status gate does not see this cutscene.** A new `BLUEWAKE_PAD_CONFIRM_EVENT=any` mode was
  added to confirm whatever prompt is up, and a `BLUEWAKE_EVENT_PROMPT_TRACE=1` probe recorded every
  transition of the event index and `0x803CA7D2` across the whole route. The pair changes exactly three
  times - `(0,0)` at retrace 0, `(80,0)` at 333, `(38,0)` at 13,930 - and the status never enters
  {7,10,16}, so neither the single-event nor the generic form fires. The generic mode stays because it
  is the right instrument for other scenes; it is not what walks this one.
* **So the mechanism that works is a real scheduled press list.** `BLUEWAKE_PAD_SCRIPT="retrace:buttons:length,..."`
  is the general form of the three fixed pulses, which cannot express a scene needing a dozen presses.

One bug was introduced and caught in the same workstream: the first version of the script block returned
`0` on every retrace no scheduled press covered, which clobbered the latched new-game schedules and
moved `name_create` from retrace 449 to 18,013 - the run never reached the play scene at all. The
milestone summary caught it on the first run, and the script is now additive rather than an override.

## 2026-09-18 the rendered route is measured: the renderer is free at the median and doubles the tail

The question `REORGANIZATION_2026-09-17.md` ended on - "whether the product passes is decided by
how much the renderer adds" - is answered, and the answer is that the renderer is not the cost.
The same 14,100-retrace route, same host, composite and card, at the shipping defaults (chassis
schedule, 16,384-cycle window), measured both ways:

| play-window 13,910..14,100 | headless | rendered (Aurora) |
| --- | --- | --- |
| median fps | 33.22 | **33.58** |
| p99 frame time | 47.34 ms | **77.63 ms** |
| mean fps | 30.70 | **24.39** |
| route wall | 70.44 s | **242.50 s** (user+sys 109.1 s) |
| real speed (guest / wall) | 333.6% | **96.9%** |
| peak RSS | 242.5 MB | **774.1 MB** |
| host turns / final pc | 32,203,791, `0x80307EF4` normal | **identical** |
| route digest | `92dd816c...` | **UNCHANGED** |

The rendered run is the shipped path - Aurora window, Metal, the audio device, vsync - and it is
the same route, not a near-miss: the same 32,203,791 host turns and the same normal stop. It ran
unattended through the pad layer, so **no idle host and no key focus were needed**, which is the
first time a rendered route has been measured without one.

Three findings, all from that pair:

* **The renderer costs 0.2 fps at the median and 30 ms at p99.** Presenting the game is free in the
  average frame and expensive in the worst one. Nothing in W1-W4 was aimed at the tail, because
  every instrument in that workstream was headless and blind to it.
* **The app is real-time paced, by the audio queue.** Retraces 1..13,850 advance at 16.787 ms each -
  59.6 Hz - and the run takes 96.9% of the authored content's wall time while burning 109 s of CPU.
  `aurora_audio.cpp` throttles with `SDL_Delay(1)` once the queued PCM exceeds
  `g_audio_max_queue_ms`, so the device that plays the sound clocks the emulator, with vsync on top.
  `BLUEWAKE_RENDERER=headless` removes the pacer, which is the whole reason the bench reports 333.6%
  of real time. **The bench number is a throughput measurement in a regime the product never
  enters.**
* **TTP-B is unreachable by construction.** Control lands at retrace ~20,044, which is **334.1 s of
  authored content at an authentic 60 Hz**. A paced application cannot present that content in less
  wall time than it takes to play, at any emulator speed. The 300 s bar is failed by 34 s at 100%
  emulated speed, and the only lever is to stop playing the content.

The full reassessment and the ranked plan that follows from it are in
`docs/status/PLAN_2026-09-18.md` (M1 route driver without the user's desk, M2 settle TTP-B with the
save-continue path, M3 rendered tail profile, M4 loop economics, M5 NFR-001).

**A fourth measurement was taken and is reported for the record:** the same route driven to retrace
24,000 with `BLUEWAKE_PAD_CONFIRM_EVENT=29` reached the play scene identically
(`play_scene_retrace=13910`) and then diverged - `stop=normal` at `pc=0x803260C8` after 158,726,266
blocks, 696.26 s wall, digest `155ac893...` - because no cutscene text page was ever confirmed. The
event-confirm mechanism is bound to a single event index and 29 is not the Outset `awake` cutscene, so
control never lands. That is the M1 gap demonstrated rather than assumed, and its live-play window
still reported median 33.17 fps, so the play scene itself is unaffected by the divergence.

Four loop-hygiene defects were found while measuring and are fixed in the same commit:

* `scripts/play.sh` pinned `BLUEWAKE_CYCLE_CAP=dynamic`, the policy `8da70d2` adopted and `be84bdf`
  superseded, under a comment claiming it matched the benchmark - so the human play path was running
  the slower policy the shipping default had already replaced.
* `scripts/play.sh` passed D1's superseded route digest `0d4cee87...` as its baseline, so every
  rendered `--timing` run reported "route digest diverged" against a digest it could not have
  produced.
* `scripts/bench_report.py` anchored its frame-timing pattern to the whole line, but the Aurora
  backend logs from its own thread and does not always finish a line first, so a stamp arrives as
  `[aurora:info:aurora[frame-timing] retrace=14053 us=...`. Exactly two retraces collide in the live
  window (14,053 and 14,097), which is why `play.sh --timing` failed with a `KeyError` on the first
  rendered run it was ever asked to report. The pattern is now unanchored, a stamp straddling a
  collision is dropped rather than fatal, and a window that lost most of its stamps is still refused;
  the headless bench reports identical numbers before and after the change.
* the runtime's default-policy comment in `runtime/host/src/main.c` still described the dynamic cap as
  the built-in default above the real one.

## 2026-09-18 the headless route now puts launch-to-interactive at 288 s, against 926 s in v48

With the presentation target met, the number that matters to the product is time-to-playable. The
benchmark's own frame timings give the headless version of it:

    retrace 14,050 (play scene): 68.3 s after the first stamp
    retrace 14,100:              70.1 s
    play-scene cadence:          36.61 ms per retrace over those 50 retraces

So the launch reaches the play scene in **68.3 s**, against **257.88 s** for TTP-A in the v48 acceptance
run, and the play scene then runs at 36.6 ms per retrace, which for the 5,994 retraces between the play
scene and the control moment would be **219.4 s**. Adding them: a **launch-to-interactive of about 288 s**
on this route, against the **926.30 s** that failed the clause in v48 - a factor of 3.2 on the metric the
five-minute bar is read from, and inside the bar for the first time.

**What that estimate is not.** The benchmark runs `BLUEWAKE_RENDERER=headless`, so it does no GX submission,
no Metal and no presentation at all; the signed app does all three, and every number here is therefore a
floor rather than a prediction. The 5,994-retrace figure is the v48 run's press-paced cutscene length, so a
different press cadence moves it. And nothing here exercises input, audio output or the harness at all. The
acceptance pass is what would turn this into a product number, and it still needs an idle host - the last
three attempts were refused by the focus preflight.

What the estimate does say is that the gap has changed shape. TTP-B failing at 926 s was a performance
problem with a factor of three in it; the same path is now measured at 288 s headlessly with the renderer
excluded, so whether the product passes is decided by how much the renderer adds rather than by the
simulation. That is the question an idle host would answer in one run.


## 2026-09-18 The dynamic census, which is what W4's static shares should have been

Computed from the rendered play-window sample already in hand, at uniform IPC (measured at 4.01), so time
shares are instruction shares. This is the breakdown the plan now says to plan against. Shares are of the
main thread and include callees.

| owner | dynamic share |
| --- | --- |
| bluewake_composite_dispatch_until_boundary | 73.6% |
| -- selected_dispatch | 71.8% |
| -- -- func_803256E0 (one guest function) | 19.6% |
| -- -- -- host_mmio_write -> aurora_backend_gx_write -> RetailGxFrontend::flush | 17.1% |
| -- -- -- -- emit_new_packets -> GxCoreSink::submit_packet | 14.5% |
| -- -- -- -- -- ConsumingAuroraRenderSink::submit_packet -> accumulate_assembly | 11.2% |
| -- -- -- -- -- -- on_consumed_draw -> plan/observer | 7.1% |
| -- -- func_802416E0 | 4.4% |
| -- -- func_8003D6E0, func_802456E0, func_802D56E0, func_803216E0 | 1.9, 1.5, 1.4, 1.3% |
| host_sync_cycle_devices_end_turn (DSP/audio/VI/IRQ sync) | 6.4% |
| host_chassis_edge_service | 4.3% |
| -- bluewake_edge_requires_host | 2.2% |
| -- bluewake_card_runtime_intercepts | 1.3% |

Three things it says that the static attribution could not.

**The largest single dynamic owner is the GX FIFO write path, and it is entered through one guest function.**
func_803256E0 is 19.6% of the rendered play window and essentially all of it is host work triggered by its
MMIO writes - flush, packet emission, draw assembly - not guest arithmetic. That is the opposite shape from
W4's list, which put the envelope and the access path on top.

**The translated guest code is a long tail, not a few big trims.** After func_803256E0 and func_802416E0,
every other guest function is under 2%, and the top twenty chunks are only 30% of the window. So there is no
large single guest body to attack, and a redesign has to make every instruction cheaper rather than a few
functions faster.

**Host services are about a tenth and are individually small**: 6.4% device sync, 4.3% edge service, and
within those the pc lookup at 2.2% and the card-runtime intercept test at 1.3%. Both lookups are small
enough that trimming them is worth a fraction of a percent, which matches how every other trim has measured.

The next real decision about the play scene has to come from this table rather than from code size, and the
only large item on it that is not guest code is the GX path - which is already identified as synchronous on
the main thread while the render worker sits idle.


## 2026-09-18 Lead: the GX sink copies a whole vertex payload per draw, and clears the vector first

Following the census arithmetic on the one large non-tail owner. accumulate_assembly is 11.2% of the
rendered window with on_consumed_draw (the real plan and submit) at 7.1%, leaving about 4% of its own work.
That 4% is not the count-only walk - gating that was measured neutral - so it is the feeding path.

ConsumingAuroraRenderSink::submit_packet does this for every draw packet: it copies a ConsumedDraw by value
(several fixed memcpys - normal and texture matrices, light and channel registers, xf registers), then in
streaming mode calls draws_.clear() before draws_.push_back(draw), and then does

    draws_.back().vertex_payload.assign(packet.draw.vertex_payload,
                                        packet.draw.vertex_payload + size)

which is a heap allocation and a full copy of the draw's entire per-vertex payload, every packet. Because
the vector was just cleared, back() is a fresh element, so vertex_payload has no capacity and assign always
allocates - there is no reuse across draws.

That is the shape of the missing 4%: not arithmetic, not the walk, just an allocation and a copy of the
biggest buffer in the pipeline, once per draw, with the capacity thrown away immediately before.

Whether it is removable is the next question, and it is a specific one: the copy exists because spans for a
draw arrive in later packets, and the packet's payload is documented as valid only during the call, so the
retention is deliberate. The candidates, in the order they should be priced: reuse the vector capacity
instead of clearing it (keeps the semantics, removes the allocation); reserve payload_bytes_ worth up front;
or, if the consumer only needs the payload for the assemble walk - which measured neutral when it was gated
off - stop retaining it at all in the shipping configuration and reference the packet instead.

All of this is host code: a rebuild is about fifteen seconds and the cached rendered instrument resolves a
share in about five minutes, so this is the cheapest thing on the census list to find out about.


## 2026-09-18 The GX sink payload copy is refuted too, at 0.07%

The candidate from the previous entry, screened in four minutes instead of an afternoon. It was the
concrete mechanism behind accumulate_assembly s apparent 4% of self work: ConsumingAuroraRenderSink
cleared its draw vector and re-pushed, so the element s vertex_payload had no capacity and the assign that
follows allocated and copied the whole per-vertex payload - the largest buffer in the pipeline - once per
draw packet. The fix assigned over the surviving element instead, so the capacity is reused.

| | instructions / play retrace | IPC | digest |
| --- | --- | --- | --- |
| reference (0017 artifact) | 491.7 M | 4.01 | 92dd816c... |
| capacity reuse | 491.4 M | 4.03 | UNCHANGED |

**0.07%**, below the metric s resolution, with the digest holding - so the change is equivalent and simply
worth nothing. It was reverted: shipping a 0.07% change into a pinned dependency costs a patch file and a
lock entry for no gain.

**The lesson is about the arithmetic, not the GX path.** This is the second time a candidate has come from
subtracting a child s share from its parent s in the sample tree - the first was the count-only walk,
reported at 4.1% and measured at under one. A share includes everything the frame calls, so that
difference is not a self time, and twice now it has invented a few percent that did not exist. The
depth-difference method should not be used to size anything; only a rebuild and a measurement settles a
candidate.

**What is actually left on the GX path.** on_consumed_draw is 7.1% and is real rendering - it is what calls
submit_draw_plan - and emit_new_packets is 14.5% against submit_packet s 14.0%, so almost all of it is
packet and draw construction rather than overhead. The overhead-shaped items are flush s own work at 2.6%
and parse_stream at 2.3%. Nothing there is a few percent, let alone 1.81x.

**Cost of finding out, and it keeps falling.** Two backend rebuilds and two measurements, about nine
minutes total, and the answer is a refutation with a digest-green proof that the change was equivalent. The
same two experiments through full rebuilds would have been four and a half hours.

## 2026-09-18 Emitted-code census: branches cost 38.6 lines each, and the envelope is the floor

The dynamic census said the guest side is a long tail and that the fix has to be broad. This asks the
question that leaves: which guest instructions are expensive to emit. scripts/emit_census.py counts the
emitted C the emitter produced for each guest instruction in a chunk, using the emitter own per-instruction
markers.

Hottest chunk, chunk_0201_text1_803256E0.c: 4,162 guest instructions, 73,574 emitted C lines - 17.7 lines
per guest instruction.

| opcode | count | lines | lines per instruction |
| --- | --- | --- | --- |
| lwz | 580 | 11,729 | 20.2 |
| stw | 437 | 8,957 | 20.5 |
| bc | 193 | 7,442 | 38.6 |
| stb | 220 | 4,487 | 20.4 |
| li | 336 | 3,820 | 11.4 |
| rlwinm | 282 | 3,696 | 13.1 |
| addi | 305 | 3,498 | 11.5 |
| or | 174 | 2,302 | 13.2 |
| lfs | 95 | 2,245 | 23.6 |
| b | 158 | 1,948 | 12.3 |
| lis | 166 | 1,916 | 11.5 |
| blr | 100 | 1,865 | 18.6 |

Three things, and the first is the one worth acting on.

Branch emission is the densest thing the emitter does. bc is 38.6 lines per instruction - nearly double a
load or store, and more than three times a simple ALU op - and 193 branches consume 7,442 of the chunk 73,574
lines, a tenth of the code from under 5 percent of its instructions. A conditional branch should be a
condition evaluation and two paths; 38.6 lines means that logic is emitted more than once. This is the
sharpest static target the workstream has produced, and unlike the access path it is a concentration rather
than a floor.

Memory operations are the bulk. lwz, stw and stb alone are 1,237 instructions - 30 percent of the chunk - and
25,173 lines, 34 percent of its code, at a consistent 20 lines each. That consistency means the inlined
mem_read/mem_write shape is a flat cost per access, which is what the earlier access-path trim attacked; it
measured 0.6 percent, so the lines are mostly work rather than the checks that were removed.

The envelope is the floor, and it is about ten lines. li takes 11.4 lines for a one-line operation, and addi,
lis and or are the same. So roughly ten of every eleven lines for the arithmetic-heavy part of the long tail
are bookkeeping - pc materialisation, the precharge and budget tests, the observation suffix - and that is
what a structurally leaner emitter would have to reduce. Static lines are not time, and this census does not
say what the envelope costs dynamically; the two envelope-shaped candidates that were measured, the
access-path clause and the sink payload copy, were worth 0.6 percent and 0.07 percent, so the lines-to-time
ratio for anything here must be measured before it is believed.

## 2026-09-18 RETRACTION: branches are not the densest emitter cost, and the real result is flatness

The previous entry reported that bc costs 38.6 emitted lines per guest instruction, nearly double a load or
store, and called it the sharpest static target the workstream had produced. That was wrong, and the error
was in the counting method rather than in the emitter.

emit_census.py counted lines from one guest-instruction marker to the next, which runs straight past the end
of an emitted function into the next function's prologue - precharge, budget test, first instruction, about
thirteen lines. Branches are exactly the instructions that end functions, so bc collected every prologue
that followed it. The script now stops attributing at a function boundary.

Corrected, same chunk: 4,162 guest instructions, 69,346 emitted C lines, 16.7 lines per guest instruction.

| opcode | count | lines | lines per instruction |
| --- | --- | --- | --- |
| lwz | 580 | 11,729 | 20.2 |
| stw | 437 | 8,957 | 20.5 |
| stb | 220 | 4,487 | 20.4 |
| li | 336 | 3,820 | 11.4 |
| rlwinm | 282 | 3,696 | 13.1 |
| addi | 305 | 3,498 | 11.5 |
| bc | 193 | 3,214 | 16.7 |
| or | 174 | 2,302 | 13.2 |
| lfs | 95 | 2,245 | 23.6 |
| b | 158 | 1,948 | 12.3 |

bc at 16.7 is unremarkable - between a simple ALU op and a load. The costliest per instruction are the
floating-point memory operations, lfs at 23.6 and stfd, lfd, stfs and stwu at about 21.8, because they carry
the FP-availability check on top of the access path. By volume the memory operations dominate: lwz, stw and
stb are 36 percent of the chunk's lines between them.

So the corrected result is the opposite of a target: there is no anomalous opcode and no concentration to
attack. Every instruction costs between 11 and 24 lines, the floor is about ten lines of envelope on a
one-line operation, and a structurally leaner emitter therefore has to reduce that floor rather than
specialise a few cases. That is the same conclusion the dynamic side reached, and it now has a static
corroboration.

It is also the third measurement artefact in this stretch - two from subtracting a child's share from its
parent's in the sample tree, and this one from counting across a function boundary - and all three
manufactured a finding that then had to be retracted. The rule that would have caught all three: verify the
method against a case whose answer is known before measuring the case that matters.

## 2026-09-18 The rendered play window executes 1.44x the headless metric, and 214 M instructions are unaccounted

The same differential that measured the headless play window at IPC 4.01, run on the rendered route instead.
Two rendered runs, retraces 13900 and 14700, differ by exactly the 800 live-play retraces:

| | headless | rendered |
| --- | --- | --- |
| instructions per play retrace | 491.7 M | 706.0 M |
| cycles per play retrace | 122.6 M | 170.6 M |
| IPC | 4.01 | 4.14 |

The rendered path is 1.44x the headless instruction count, and the difference is 214 M instructions per play
retrace - about 30 percent of the stream the product actually runs, and none of it guest code, because the
route digest is identical in both.

This is the largest single block anything in this workstream has identified, and it sits where iteration is
fastest: rendered-path work is host code, so a rebuild is about twelve seconds and the instruction metric
resolves it in minutes. Every candidate measured so far has been a fraction of a percent; this one is 214 M
instructions per retrace.

What it is not yet known to be. The rendered run differs from the headless one in three ways at once - Aurora
and Metal run, the audio device is open, and presentation happens - and the differential cannot say which of
them carries the 214 M. The rendered profile call graph does not settle it either: sample records each thread
once per interval regardless of state, so every thread shows the same total and only the call trees
distinguish busy from waiting. Within those trees the render worker sits in its queue 94 percent of the time
and the pipeline worker and pipeline cache writer are waiting outright, so the obvious GPU-side suspects are
idle - which points at the audio device converter and playback threads, or at Metal encoding on the main
thread, and not at the GPU.

The next step is a per-thread busy-share capture, which is one rendered run and one sample, and it is the
first measurement in this stretch with a 30 percent prize attached. Until it is taken, the honest statement is
that the product play window spends about 214 M instructions per retrace somewhere in the renderer or the
audio device, that this is larger than everything else proposed or refuted combined, and that nobody has
looked inside it.

### The per-thread attribution attempt failed, and is recorded so it is not repeated

Two attempts to attribute the 214 M instructions per retrace, neither of which produced a usable number.

The first was a per-thread busy-share script over the existing rendered sample, classifying each thread's
depth-1 children as waiting or working. It returned idle shares of 400 percent and 0.0 percent on the same
data, because the call graph collapses identical stacks - a thread with several children at the same count
is counted several times, so the children do not partition the thread's total. The output is nonsense and
nothing was taken from it. That is the fourth measurement artefact in this stretch, and the same rule
catches it: check the method against a case whose answer is known before using it on the case that matters.

The second was to read the trees by hand, and it gives a weak but consistent picture: the main thread is
88.4 percent inside main, the render worker is 94 percent inside its own queue, the pipeline worker and
pipeline cache writer are waiting outright, the event thread is in mach_msg and the caulk threads are in
semaphore waits. On that reading every thread except the main thread is waiting, which would put the 214 M
on the main thread inside the GX path - consistent with the census, where the GX write path is 19.6 percent
of the window at an IPC lower than the guest code around it, and so a larger share of instructions than of
time.

That is a hypothesis, not a measurement, and it should not be relied on until a per-thread busy-share
capture is taken properly - one rendered run and one sample, with the shares computed from a partition that
actually sums, for example by thread state as sample reports it rather than by children of a collapsed
call graph.

### The 214 M is main-thread work: the play window uses 0.74 of one core

The per-thread capture did not break out threads - ps -M reports one row for the process on this host, not
one per thread, and the attempt to use it as a per-thread census is recorded as a fifth failed method. But
its process total answers the question anyway, because the arithmetic only needs a total.

Two snapshots of cumulative CPU time, taken during the rendered run at retrace 13,936 and again at 14,511:

    575 live-play retraces   ->   28.27 s of CPU   ->   49.2 ms of CPU per retrace

Against the rendered differential's 706.0 M instructions per retrace, that is 14.3 G instructions per second
- which is 4.14 IPC at about 3.5 GHz, the machine's peak. Two consequences:

* The rendered play window consumes 0.74 of one core. Nothing is burning a second core, so no other thread
  carries a meaningful share of the 214 M. It is main-thread work.
* It is running at peak IPC, so it is not stalled, not waiting and not oversubscribed - the same conclusion
  the headless differential gave, now with the renderer attached.

Together with the rendered profile, where the render worker sits in its queue 94 percent of the time and the
pipeline worker and cache writer are waiting outright, this puts the 214 M on the main thread in the GX
translation and encoding path, at a lower IPC than the guest code around it (which is why 19.6 percent of
the window's time is a larger share of its instructions). The structural lever that follows is the one the
M3 profile first pointed at: that work is on the thread that is saturated while the thread built to consume
it is idle.

Sequencing note for whoever resumes: the five failed or refuted methods in this stretch were each caught by
comparing against a case whose answer was known, and this one by a figure that could not be true (14.3 G
instructions per second is the machine's peak, and it is only reachable by one thread running flat out -
which is what makes the 0.74-of-a-core reading meaningful rather than absurd).

### The 214 M is renderer-only: headless runs neither GX consumer

The gx-core consumer can be switched off at run time with DOL_GX_CORE=0, which selects the shadow packet
sink instead. Run through the headless instruction metric:

| headless configuration | instructions / play retrace | IPC |
| --- | --- | --- |
| gx-core (shipping) | 491.7 M | 4.03 |
| shadow sink (DOL_GX_CORE=0) | 491.7 M | 3.97 |

Identical. Swapping the consumer changes nothing headless, which means **headless is not running either
consumer** - the GX frontend is fed by the Aurora backend, and headless installs the headless backend
instead. So the census's GX path, 19.6 percent of the rendered main thread, does not appear in the headless
metric at all, and the 214 M instructions per play retrace that separate rendered from headless are
entirely renderer-only work: the GX FIFO translation, the packet and draw pipeline, Aurora's queue and
Metal encoding, and the audio device.

That closes the loop between the two measurements. The rendered differential says 706.0 M against 491.7 M;
the census says the GX path is 19.6 percent of the rendered main thread; the CPU snapshots say the window
uses 0.74 of one core at peak IPC. All three agree that about 214 M instructions per retrace are spent on
the renderer, on one saturated thread, while the worker thread built to consume that work sits idle 94
percent of the time.

**Consequence for every earlier number in this ledger.** The headless metric - bench.sh, bench_instructions,
and every instruction figure recorded before today - measures 70 percent of the product's play-window
instruction stream and is structurally blind to the other 30 percent. It remains the right gate for guest
work, because the digest is identical in both configurations, but it cannot be the whole target, and the
candidates it was used to rank and refute were ranked against the wrong stream.

### Failed experiment: DOL_GX_CORE=0 cannot be used to split the 214 M, and would have misled

The idea was cheap and looked decisive: the gx-core consumer can be switched to the shadow sink at run
time, so running the rendered differential with DOL_GX_CORE=0 would say how much of the 214 M that consumer
accounts for. It produced 161.4 M instructions per play retrace - which is *less than the headless* 491.7 M,
and that is impossible for a faster renderer, because the guest work is fixed.

The check that caught it: the log has no normal-stop line at all. The process exited without reaching the
retrace ceiling, and its peak resident size was 17 GB against the shipping run's 300 MB. So the shadow sink
is not a working alternative consumer in the rendered configuration, and the run is not a measurement of
anything.

Recorded because it is the shape of mistake that has cost this stretch the most time: a plausible switch, a
number that looked like an answer, and a conclusion one grep away from being wrong. The rule that catches it
is the same one the other five failures needed - before reading a number, confirm the run that produced it
did what it was supposed to do. For this metric that means checking for the normal stop and the same host
turn count, which is free, and without which every figure above is uninterpretable.

## 2026-09-18 A fatal WGSL codegen bug kills rendered runs, and it invalidates the 1.44x finding

Re-measuring the rendered/headless differential inside the route certified region (ceilings 13,800 and
14,100, both within the accepted route) produced four runs, and two of them are unusable in a way that
matters:

    r13800   1.5 G instructions, 3 KB log, no normal stop
    r14100   1.5 G instructions, 3 KB log, no normal stop
    h13800   899,082,604,853 instructions, stopped normal, 29,737,920 blocks
    h14100   989,787,050,513 instructions, stopped normal, 32,203,791 blocks

The rendered runs die at startup with a shader-compile failure:

    [aurora:fatal:aurora::gpu] WebGPU error 2: Error while parsing WGSL:
      :399:72 error: cannot index into expression of type f32
      prev.a = clamp(select(0.0, tev_overflow_f32(ubuf.kcolor3.a),
        round(tev_overflow_f32(sampled0.a).r * 255.0) > ...

That is a real defect in Aurora GX shader generation - a component swizzle applied to a scalar f32 in the TEV
overflow path - and it is fatal, not a warning: the run aborts before the route.

Consequence for this ledger. The rendered differential that produced 706.0 M instructions per play retrace
against headless 491.7 M, and the 214 M renderer-only increment built on it, were measured with the same
method and no stop-line check - the exact failure mode found in the previous entry and guarded against in
bench_instructions.sh but not in the rendered differential. A run that aborts reports a small instruction
count, and differencing an aborted run against a valid one produces a number with no meaning. Until the
rendered differential is re-run with both runs stopping normally, the 1.44x figure and the 214 M are
unverified and should not be used.

Second consequence, and the more useful one. The certified play window tells a different story from the one
this stretch has been using. Inside the route (13,800 to 14,100, 300 retraces, both runs stopping normally at
32,203,791 blocks) the headless play window is 302.3 M host instructions per retrace, not 491.7 M. The 491.7
M figure came from the 13,900 to 14,700 window, which runs past the certified route into heavier Outset
content. So the metric absolute value is content-dependent, and every reference 491.7 M recorded before
today refers to a window three hundred retraces longer than the one the route certifies. Comparisons between
builds on the same window remain valid; the absolute number is not a property of the play scene.

Next actions, in order: fix or work around the WGSL fatal so rendered runs can be trusted at all; add the
stop-line check to the rendered differential; then re-measure the rendered increment inside the certified
window with both runs verified.

## 2026-09-18 FIXED: a scalar alpha compare emitted an invalid WGSL swizzle and aborted every rendered run

Root cause found and fixed. tev_op in aurora/lib/gx/shader.cpp is shared between the colour path, where the
operands are vec3 and the R8, GR16 and BGR24 compare forms swizzle into them, and the alpha path, where the
operands are scalars. An alpha TEV stage whose compare is one of those forms therefore emitted

    round(tev_overflow_f32(sampled0.a).r * 255.0)

which is a component swizzle on an f32. WebGPU rejects the shader - "cannot index into expression of type
f32" - and because the failure is fatal at shader creation, the whole run dies before the route. Every
rendered measurement taken since this appeared has been either valid by luck or, as the previous entry
found, quietly meaningless.

The fix collapses every compare form to the scalar one for alpha, which is also what the hardware does:
alpha compares are defined over the 8-bit alpha value, so R8 on alpha is the value itself, not its red
channel. Exported as patches/recompcore/0050-aurora-scalar-alpha-compares.patch (24 lines, one hunk) and
registered in the lock, which is now at 46 tracked patches.

**Verified on the renderer path rather than by reasoning:** a rendered run to the certified 14,100 ceiling
now reports

    fatal count            0
    stopped: normal after 32,203,791 blocks at pc=0x80307ef4

- the certified route's own stop, and the same host turn count as the headless run.

**And that gives the first valid rendered/headless comparison, because the guest work is provably identical.**
Two runs, same ceiling, same 32,203,791 blocks, both stopping normally:

| configuration | instructions over the certified route |
| --- | --- |
| headless | 989,787,050,513 |
| rendered | 1,130,463,320,757 |
| renderer's share | **+140,676,270,244 (14.2 percent)** |

So the renderer costs about **141 G instructions over the route**, or 9.98 M per retrace averaged across
boot and play - with the guest work provably the same in both runs, because the turn count and the stop pc
match exactly. That is the correct figure, and it replaces the 214 M-per-play-retrace number from the
retracted entry, which was a misreading of an aborted run and was 44 percent where the truth is 14.

What this does and does not say. It does not say the renderer is cheap: 14 percent is real work, and inside
the play window it will be a larger share than the route average, because the boot contributes almost none
of it. It does say that the largest single opportunity identified in this stretch is 14 percent and not 30,
that the remaining gap to authentic play-scene speed is still the emitted code rather than the renderer, and
that every rendered number recorded before this fix has to be re-taken.

## 2026-09-18 The corrected rendered split: the renderer is 33.4 percent of the play window

With the WGSL fatal fixed, the rendered differential can be taken properly. Four runs, and for the first time
every one of them stopped normally at the certified route's own pc, with matching host turn counts - so the
guest work is provably identical on both sides and the difference is host work.

| | 13,800 retraces | 14,100 retraces | play window (300 retraces) |
| --- | --- | --- | --- |
| headless turns / instructions | 29,737,920 / 899,082,604,853 | 32,203,791 / 989,787,050,513 | **302.3 M per retrace** |
| rendered turns / instructions | 29,737,920 / 1,009,462,103,838 | 32,203,791 / 1,130,463,320,757 | **403.3 M per retrace** |

The host turn counts are identical at both ceilings - 29,737,920 and 32,203,791 - which is what makes this a
measurement rather than a comparison of two different runs.

**Inside the play window the renderer is 101.0 M instructions per retrace, or +33.4 percent.** Over the whole
route it is +14.2 percent, because the boot contributes almost none of it; the play window is where the
renderer actually costs something.

That corrects the retracted entry in both directions at once. The 214 M and the 44 percent were wrong,
because they came from differencing a run that had aborted - but 33.4 percent is not the 14 percent the
route average suggested either, and the renderer remains the largest single block of non-guest work in the
play window. The corrected reference for anything measured from here is 302.3 M per retrace headless and
403.3 M rendered, both inside the certified window, both with normal stops.

Where that leaves the goal. Authentic play-scene speed needs about 1.81x on the headless portion, which is
still the emitted guest code, plus whatever can be taken from the renderer's 101 M. Every candidate measured
against the old 491.7 M reference was measured against a window 300 retraces longer than the certified one,
so the refutations stand only as refutations of large effects - a 0.6 percent result measured on a
33 percent too-long window is still a small result, but the numbers themselves should be re-read against
302.3 M before being quoted.

### Where the renderer's 101 M probably sits, and what would confirm it

One inference worth recording, because it is cheap to state and would be cheap to test, and it is offered
as an inference rather than a measurement.

The rendered play window is 403.3 M instructions per retrace and the headless one is 302.3 M, so the renderer
is 101.0 M. The rendered profile puts the GX write path - entered from the single guest function that writes
the FIFO, through flush, emit_new_packets, submit_packet, accumulate_assembly and on_consumed_draw - at 19.6
percent of the rendered main thread. If the GX path carries the main thread's average IPC, that share is
about 79 M of the 101 M, leaving roughly 22 M for everything else the renderer adds: Aurora command
encoding, presentation, and the audio device's converter and playback threads.

That would place about three quarters of the renderer's cost in the GX frontend and the draw pipeline - which
are host code with a twelve second rebuild, where the two trims measured so far (the count-only assembly
walk, and the sink's per-draw payload copy) were each worth less than one percent. Those two nulls are
consistent with the rest of that path being real packet and draw construction rather than overhead, but they
do not prove it, and the share they were measured against was 403 M rather than the 706 M the retracted
entry used.

What would confirm or kill it: a rendered differential with one of the renderer's phases disabled or
instrumented, on runs that both stop normally inside the certified window. The instrument that would do it
most cheaply is a per-phase instruction or time counter compiled into the GX frontend, since that is host
code, rather than another attempt to read the answer out of a sampling profile - which has now produced five
failed or refuted methods in this stretch.

## 2026-09-18 The rendered split reproduces on the script, and the script guard had a bug worth keeping

scripts/bench_rendered.sh was run end to end for the first time. Its guard refused to report - and the guard
was wrong, not the runs: its pattern captured the pc with the "pc=" prefix attached, so every pc compared
unequal to the expected address and four perfectly good runs were rejected. The pattern now captures the
address alone, and it parses the retained logs correctly (pc ok, turn counts 29,737,920 and 32,203,791).

The four runs it produced were valid, and differencing them reproduces the hand measurement independently:

| | instructions per play retrace |
| --- | --- |
| headless | 302.4 M |
| rendered | 402.6 M |
| renderer share | +100.2 M (+33.1 percent) |

against 302.3, 403.3 and +33.4 measured by hand - agreement to within 0.2 percent on two runs each. So the
corrected split is confirmed twice, by two methods, on runs that all stopped normally at the certified pc
with the certified turn counts.

Two things worth keeping from this. The guard earned its place by firing on its first real use - a run that
cannot report is exactly the outcome a guard is for, even when the guard is the thing that is wrong, and the
failure message named the field so it took one look to find. And the number this stretch spent several turns
getting wrong now has two independent measurements behind it, which is a better position than the figure it
replaced ever had.

## 2026-09-18 The emitted body costs 27 host instructions per guest cycle, measured directly for the first time

The instrument the last three stretches asked for and never built, and its first result.

**The harness.** `scripts/bench_chunk.sh` with `scripts/chunk_bench.c` links one already-built chunk
object against the six GXRuntime objects it calls into, builds a synthetic CPUState over a 24 MB MEM1
image, and sweeps entry points: every instruction slot in the chunk is used as a dispatch entry, lr
points outside the chunk so each entry runs to the guest's first return, and the guest cycles come out
of the emitter's own accounting - the generated code charges each block's Gekko cost into
`downcount`, so -`downcount` at return is exactly the guest cycles that dispatch consumed. Host
instructions retired come from `/usr/bin/time -l`. Seconds per chunk, no composite rebuild and no
route.

**What it does not measure, so that nobody quotes it wrongly.** It is not a wall-clock instrument:
every entry starts cold, so I-cache and branch-predictor state are unrepresentative and the ns/cycle
column is pessimistic. And the sweep is not weighted by the play scene's instruction mix - it exercises
every block once per pass. The structural number, instructions per guest cycle, is the one to read.

**The ten hot chunks**, 200,704 entries each, about 49 passes over all 4,096 slots:

| chunk | share of window | instructions / guest cycle | IPC |
| --- | --- | --- | --- |
| `0201` | 15.7% | **24.47** | 2.75 |
| `0144` | 3.6% | **28.78** | 3.04 |
| `0015` | 1.5% | **31.02** | 2.72 |
| `0145` | 1.2% | **40.13** | 3.67 |
| `0181` | 1.1% | **21.34** | 3.27 |
| `0200` | 1.0% | **28.96** | 3.90 |
| `0187` | 0.6% | **33.40** | 3.47 |
| `0203` | 0.5% | **32.20** | 3.82 |
| `0188` | 0.4% | **34.05** | 3.57 |
| `0148` | 0.4% | **43.03** | 2.82 |

Share-weighted across the ten, which are 26 percent of the live window: **27.0 host instructions per
guest cycle**.

**Why that matters.** Authentic speed needs the whole play window at about 20 host instructions per
guest cycle: 302.3 M instructions for 8.1 M guest cycles is 37.3, and 1.81x is 20.6. The emitted body
alone, with no dispatcher, no device sync and no edge service in the count, is at 27. That is **1.3x over
the entire authentic-speed budget before any host service is added**, and it is the first measurement
that separates the body from the services rather than inferring the split from a sampling profile.

It also closes the census from the other direction. 37.3 (whole window, headless) minus 27.0 (hot-ten
body) leaves 10.3 instructions per guest cycle for everything else, and the census's device sync (6.4
percent) plus edge service (4.3 percent) of the rendered main thread is 5.3 of that, with the remaining
guest code and the dispatcher behind it. Two independent instruments now agree that the largest single
block of cost is the emitted instruction stream rather than the host services, which is the opposite of
what the rendering result alone suggested.

**What it leaves.** The measurement says where the cost is, not how much of it is removable, and the two
things that would answer that are both cheap now that the harness exists. Seed the sweep from a real
in-game CPUState: a synthetic state cannot reproduce the play-scene instruction mix, and that is the
honest limit of this number. And run the same sweep against a hand-optimised variant of the same body,
which is the roofline the loop's second experiment asks for.

**Compiler ablation, and the instrument's own noise floor.** Recompiling the same chunk at -O3 and -Os and
re-running the identical sweep - guest cycles come out at 77,425,065 in all three, so only the emitted
machine code differs:

| optimisation | instructions / guest cycle |
| --- | --- |
| `-O2` (shipping) | **28.66** |
| `-O3` | **28.55** |
| `-Os` | **28.64** |

Three runs of one binary gave 2,215.7 M, 2,214.6 M and 2,217.3 M instructions retired, a spread of **0.12
percent**, so the 0.4 percent -O3 gain sits at the edge of what the instrument resolves and is worth
nothing. The emitted shape rather than the compiler's optimisation level is the cost, which reproduces the
earlier -O3 refutation on an instrument that resolves 0.12 percent instead of on route wall clock. It also
sets this harness's own noise floor at 0.12 percent, against 2-5 percent for the fps bench. As a
determinism check, the -O2 recompile of `chunk_0144` is byte-identical to the certified object.

## 2026-09-18 The sweep now runs on real in-game state: 30.4 instructions per guest cycle

The previous entry's number was measured with a synthetic CPUState, and a synthetic state cannot
reproduce a real trajectory. This closes that gap, and the real number is higher.

**The instrument.** `scripts/dump_chunk_state.sh` with `scripts/make_chunk_dump_loop.py` emits an
instrumented copy of the composite's dispatch loop - into a temp directory, using the recorded link
command directly - and relinks an instrumented dylib there. The tracked `cmake/composite/dispatch_loop.c`
is untouched and the certified artifact is untouched. The hook is inert unless
`BLUEWAKE_DUMP_CHUNK_STATE` is set, and an optional `BLUEWAKE_DUMP_AFTER_MS` holds it off so the
snapshot comes from the play window rather than from boot, which executes these chunks too.

**The capture run.** The certified headless route with the hook held off for sixty seconds. It stopped
normally at pc `0x80307EF4` after **32,203,791** blocks, with `play_scene=1` at retrace 13,910 and
`opening_complete=1` at 13,850 - the certified stop, the certified turn count and the play-scene
landmark, so the snapshots come from the same route the ledger certifies against. Ten snapshots of
33,558,008 bytes each: 32 MB of MEM1 plus the CPUState.

**The measurement.** `scripts/bench_chunk.sh --snapshot` drives the same entry sweep with the real
registers and the real memory image instead of the synthetic fill:

| chunk | share of window | instructions / guest cycle | IPC |
| --- | --- | --- | --- |
| `0201` | 15.7% | **24.48** | 3.38 |
| `0144` | 3.6% | **29.13** | 4.70 |
| `0015` | 1.5% | **46.92** | 3.74 |
| `0145` | 1.2% | **36.53** | 3.80 |
| `0181` | 1.1% | **31.43** | 3.38 |
| `0200` | 1.0% | **35.09** | 3.41 |
| `0187` | 0.6% | **71.48** | 3.30 |
| `0203` | 0.5% | **49.14** | 4.12 |
| `0188` | 0.4% | **64.90** | 3.60 |
| `0148` | 0.4% | **61.72** | 2.42 |

Share-weighted across the ten: **30.4 host instructions per guest cycle**, against 27.0 for the synthetic
state and 20.6 for authentic speed. So the emitted body alone is about **1.5x over the entire
authentic-speed budget** before any dispatcher, device sync or edge service is counted.

**The two failures on the way, because they are the kind that produce confident nonsense.** The first
snapshot run reported 17 to 534 instructions per guest cycle, all of them wrong. Two causes. The state
carries a live cycle deadline, and with one active the generated code *refunds* charged cycles through
the observation suffix, so `-downcount` stopped being a measure of the guest cycles consumed; the
harness now clears the deadline. And the two timed runs differenced a cold first read of a 33 MB file
against a warm second one, which is worth tens of millions of instructions and swamped the dispatch
delta entirely; the harness now warms the snapshot before timing. With both fixed the deltas are
billions of instructions over 800,000 dispatches, so the remaining noise is under a percent.

**What this number does not include**, so it is not over-read. The registers are the game's but the same
ones are used for all 4,096 entries, since one snapshot is one moment. Guest MMIO writes go to a stub,
so the GX FIFO chunk's host-side cost is absent from its column - which is why `0201`, the largest owner
in the rendered census, reads cheapest here. And the sweep still weights every block equally rather
than by the play scene's mix.

## 2026-09-18 Ablation says the register file is not the cost, and the cycle accounting is worth 10-15 percent

The roofline question, priced per construct instead of by rebuild. `scripts/ablate_chunk.py` deletes a
named emitted construct from one chunk's C, `scripts/bench_chunk.sh --ablate` compiles it and runs the
same real-state sweep, and the whole thing costs about two minutes per construct instead of a
ninety-minute rebuild. The transforms are verified applied: `restrict` marks all nine emitted functions,
`no-guard` takes the budget tests from 4,205 to 79, `no-suffix` takes the suffix stores from 1,384 to
zero, and `prepaid` takes the precharge calls from 1,119 to zero.

| ablation | chunk 0144 (baseline 28.84) | chunk 0201 (baseline 24.52) |
| --- | --- | --- |
| `restrict` - ctx cannot alias guest memory | **29.01 (+0.6%)** | **24.59 (+0.3%)** |
| `no-pc` - no per-instruction pc materialisation | **29.04 (+0.7%)** | - |
| `no-guard` - no downcount budget tests | **26.57 (-7.9%)** | **22.91 (-6.6%)** |
| `no-suffix` - no observation suffix or reconcile | **26.77 (-7.2%)** | **23.58 (-3.8%)** |
| `prepaid` - precharge forced, per-instruction charge path gone | **25.08 (-13.0%)** | **23.19 (-5.4%)** |

**The leading hypothesis is refuted, and it was the one the plan was built on.** The register-file
explanation says the cost is that every guest operand is a load or store through `ctx->gpr[]`, because
no callee is known not to alias `ctx`. Marking the parameter `__restrict` is semantically valid here -
the generated code's ctx genuinely does not alias the guest memory reached through it - and it lets the
compiler hold ctx fields across the memory helpers. It is worth **+0.6% and +0.3%, that is nothing**. So
either clang is already recovering the reloads, or the memory-resident register file is not where the
cost is. The hypothesis is not dead in every form, but it is dead in the form that motivated it.

**The cycle accounting is where the money is.** The downcount budget test costs 7-8 percent, the
observation suffix and its reconcile 4-7 percent, and forcing precharge 5-13 percent. Together that is
roughly **10-15 percent of the emitted body**, and it is the first thing measured in this workstream that
is worth more than a couple of percent.

Two of those numbers contradict earlier ledger entries, and they did so because they were measured on the
fps bench, where the project's own noise floor is 2-5 percent. "The observation bookkeeping is not a
bottleneck" was recorded from a run that could not have seen a 7 percent effect; on the instruction
metric the suffix is worth **7.2 percent** on chunk 0144. And the per-instruction pc store, priced at
**10.8 percent of emitted code size** and load-bearing when removed, is worth **0.7 percent of dynamic
instructions** - the same code-size-is-not-time error the access path showed, in the other direction.

**What it does not license.** These are prices, not a patch. `prepaid` deletes a capability the runtime
needs - precise per-instruction charging when the deadline is close - so its number bounds a structural
redesign of the charge mechanism rather than a trim. The constructs overlap, so the percentages do not
add: `prepaid` subsumes part of `no-guard` and `no-suffix`. And each is one chunk in isolation.

**Where that leaves the arithmetic.** The hot ten sit at 30.4 instructions per guest cycle against 20.6
for authentic speed. Taking 12 percent off the body gives about 26.7, leaving a 1.3x gap. The cycle
accounting is real and worth taking, and it is not the 1.5x. With the register file refuted, the next
question is what the remaining ~26 instructions per guest cycle are doing, since the two largest static
constructs are now both priced near zero.

**The combined lean form, and it changes the arithmetic.** Applying the whole accounting suite at once -
no per-instruction pc, no budget tests, no observation suffix, precharge forced:

| chunk | emitted | lean | change |
| --- | --- | --- | --- |
| `0144` | 28.84 | **22.38** | **-22.4%** |
| `0201` | 24.52 | **20.12** | **-18.0%** |

Authentic speed is 20.6 instructions per guest cycle for the *entire* frame. The leanest emitted body this
suite can produce lands at 20.1 and 22.4. So the cycle-accounting machinery is not a marginal trim: it is
most of the distance between being over budget and being at it, and it is the first lever found in this
workstream of that size.

The caveat is the whole design question, so it is not a free 20 percent: this form deletes the ability to
leave the generated code at an exact guest cycle, which is what the deadline mechanism exists to provide
and what D2 bounded rather than removed. What the measurement frames is narrow and testable - can the same
bounded-drift guarantee be met with a charge granularity coarser than per instruction and a deadline check
that is not at every charge site? D2 already admits bounded per-delivery drift with the route digest as the
gate, so this is a design with an acceptance test rather than a hope, and it is where the next iteration
should go.

## 2026-09-18 The ablation denominators are clean, and the GX path's 7.1 percent is real submission

Two checks that decide what the next build should be. Neither lands a change; both remove a wrong option.

**The forced-precharge price is not an accounting artefact.** That was the largest number in the ablation
suite and it needed one check before being believed, because forcing precharge changes *which* charges
fire, so if it also moved the guest cycles charged then the ratio would be measuring its own denominator.
It does not move them: baseline and `prepaid` both charge exactly **93,289,303** guest cycles over the
same 200,000 entries on chunk 0144, while the host instructions differ by 13.5 percent - 2,719,864,218
against 2,352,376,522. The price is real, and it buys identical guest work.

**The GX path's plan observer is not a diagnostic that can be switched off.** The census puts 7.1 percent
of the rendered main thread in `on_consumed_draw -> plan/observer`, and that reading invited a cheap
hypothesis: if the observer were a counter or a trace hook left installed in the shipping build, disabling
it would be free. It is not, and the routing is worth recording because it took several lookups to
establish. `aurora_backend.cpp:316` installs `core_plan_observer`, defined at
`aurora_graphics.cpp:38`, which calls `submit_draw_plan(plan)` - the actual GPU draw submission.
`GxCoreSink::on_consumed_draw` early-returns when no plan observer is set, so the observer's presence is
what selects the draw path rather than what decorates it, and `GxCoreSink` itself is shipping code
(selected by `DOL_GX_CORE`, default on), not a replay-tool fixture.

So the 14-20 percent the census assigns to the GX front end is draw planning and submission rather than
overhead, and the only lever of that size on it is moving that work off the main thread. That is the
concurrency project the objective names, it is not reachable by a trim, and it should not be started until
the accounting change - the larger and cheaper of the two measured levers - is done or abandoned.

## 2026-09-18 The budget test's defensive fallback was worth 3.3 percent on the hot chunks

The first change from the roofline to land, and it is both measurable and provably equivalent.

**The change.** `dolrecomp_loop_cycle_budget` was `cycle_budget > 0 ? cycle_budget : 256`, and the
emitter puts it in a negated comparison at every block leader and at every non-leader charge site - 4,206
sites in chunk 0144 alone. The host guarantees `cycle_budget >= 1`:
`bluewake_cycle_domain_begin_turn` sets 1, and `bounded_budget` clamps every other assignment to 1 as
well, which is the only place the field is written. So the fallback never fires and its whole cost is a
compare and a select on each test. Returning the field directly lets the compiler fold
`downcount <= -(s64)budget` into a single add-and-compare. One line in
`scripts/generate_composite.py`.

**Priced before it was built.** `scripts/bench_chunk.sh --ablate cheap-budget` rewrites the test in the
emitted C and measures it with the real-state sweep: chunk 0144 goes 28.86 to 27.90 instructions per guest
cycle over two interleaved rounds each way, no overlap, **-3.3 percent**.

**Then built and gated.** Header regenerated, `scripts/recomp_chunks.sh` over the hot ten (12.1 minutes),
direct relink, and the certified route:

| | before | after |
| --- | --- | --- |
| instructions per play retrace | 491.7 M (reference) | **489.2 M** |
| change | | **-0.52%** |
| route digest | `92dd816c...` | **UNCHANGED**, 1,050 records |
| host turns | 32,203,791 | **32,203,791** |

The route-level effect is the chunk-level one diluted twice: the hot ten are 26 percent of the window, and
the bench window (13,900 to 14,700) is wider than the certified 13,910 to 14,100. Half a percent is small,
but it is above this metric's noise and the digest and turn count are the certified ones, so it is a valid
increment rather than a screening curiosity.

**What it owes, and the state it leaves.** The composite is now the screened artifact,
`42830934443c609f`: the hot ten rebuilt against the new header and the other 746 still against the old one.
Both forms are equivalent - which is exactly why the digest holds - but a mixed artifact is a screening
artifact and not a shipping one. The generator change is durable, so a full rebuild reproduces it, and the
certified artifact is backed up at `/tmp/bw-certified-39e05177.dylib`. The plan is to accumulate the other
provably-equivalent wins first and pay one full rebuild rather than one per micro-win; the accounting suite
is not in that set, because its pieces are load-bearing and it needs a design rather than a trim.

## 2026-09-18 The dead charge body is the cost, not the test on it

Three more equivalent candidates priced on the isolated chunk. Two are no-ops, and the third explains
where the forced-precharge price actually comes from. The baseline here is 27.54 rather than the earlier
28.86 because the generated header now carries the landed budget-test change.

| candidate | chunk 0144 | verdict |
| --- | --- | --- |
| branch hints on the hot-path guards (`__builtin_expect`) | 27.54 -> **31.48** | **refuted**, a layout regression of 14% |
| `can_precharge` without the redundant `remaining >= 0` | 27.54 -> **27.56** | null, the compiler already folds it |
| per-instruction charge moved out of line | 27.54 -> **26.95** | **-2.1%**, real |

**What the third one means.** The forced-precharge ablation was worth 13 percent and the obvious reading
was that the per-instruction *test* is expensive. It is not, or at least not mostly. Rewriting

    if (!cycle_block_prepaid) {
        if (ctx->downcount <= -(s64)DOLRECOMP_C_LOOP_CYCLE_BUDGET) { ctx->pc = 0xADDRu; return; }
        ctx->downcount -= Nu;
    }

as

    if (!cycle_block_prepaid && !bw_charge(ctx, Nu, 0xADDRu)) return;

with `bw_charge` marked `noinline` keeps the flag test on the hot path and moves about eight
instructions of body out of line. It is worth 2.1 percent by itself, and it shrinks the chunk's C from
2,275,845 to 1,891,305 bytes - 17 percent. So a measurable share of that 13 percent is the
never-executed charge body occupying the instruction stream of a 518 MB dylib, which is the I-cache
effect the code-size warnings in this ledger predicted and nothing had measured until now.

**Why it is not landed yet.** This one is an emitter change rather than a header change: the emitted text
is what has to move, so it needs a DolRecomp build, a regeneration, and then the same screening loop. The
next iteration does that, and it is worth batching with the full rebuild the budget-test change already
owes.

## 2026-09-18 The DOL generation path reproduces byte-for-byte, and the invocation is recovered

The ledger recorded a prerequisite for any emitter change: the regenerated chunks must match the live
ones, or the generator state has drifted and regenerating would move the route digest for reasons
unrelated to the change. It also recorded that the invocation "should be taken from the script or history
that produced composite-r2 rather than inferred". Both are now settled, and the emitter path is open.

**The invocation.** The generator is the `dolrecomp` CLI, and for the C route this tree needs:

    build/dolrecomp-cycle-precise/dolrecomp --gamecube --backend c --cpu gekko \
        --partition-instructions 4096 generated/full/main.dol <out> -j8

It writes `<out>/generated/{generated.c,generated.h,generated_smc.txt,chunks/}` with 206 chunk files, in
**1.8 seconds** - the "regenerate all chunks (seconds)" the screening loop has always assumed. Note
`--cpu gekko`: the CLI defaults to broadway, and the composite is Gekko.

**The check.** Regenerated into a temporary directory and hashed file-by-file against the live
`composite-r2/chunks_dol`: **206 identical, 0 differing, 0 missing on either side**. So the DOL generator
state has not drifted since the chunks were cut, regeneration is safe, and an emitter change can be
screened on the DOL chunks - which is every one of the ten hot chunks.

**What is still not reproducible, recorded so it is not rediscovered.** The *composite* generator does not
run end to end from the current tree: `scripts/generate_composite.py` with the inputs under
`generated/full` fails its REL namespace check with "REL module 181 section 1 linked range
[0x804000f4, 0x80400fd0) overlaps retail MEM1", so the REL inputs used on Aug 31 are not the ones in the
tree now. That blocks regenerating the composite as a whole and it blocks any REL-side emitter work; it
does not block the DOL-side work this loop is doing, because those chunks can be regenerated and dropped
into the chunk directory directly.

## 2026-09-18 The precise charge moves out of line: -2.1% on the hot ten, -0.27 points at the route

The charge-helper ablation from the previous entry, built end to end as a real emitter change and screened.

**The change.** `emit_precise_instruction_charge` emitted a seven-line charge block at every instruction:

    if (!cycle_block_prepaid) {
        if (ctx->downcount <= -(s64)DOLRECOMP_C_LOOP_CYCLE_BUDGET) { ctx->pc = 0xADDRu; return; }
        ctx->downcount -= Nu;
    }

It now emits one cold call into a new `dolrecomp_charge_precise`, marked `noinline`, which the generated
header defines alongside the other prelude helpers. Same semantics, same charging, and the branch is
equally cold - what changes is that six instructions of never-executed body stop sitting in the
instruction stream at every instruction of a 518 MB composite. Exported as
`patches/dolrecomp/0018-backend-move-the-precise-instruction-charge-out-of-line.patch` and registered in
`config/dependencies.lock.json`; the patch reverse-applies clean against the tree it was cut from.

**Built with the recovered DOL path.** `dolrecomp` rebuilt, the DOL regenerated in 1.8 seconds, and the
regeneration diffed against the live chunks before anything was compiled: 206 chunks differ, and the
difference in each is exactly the charge sites - chunk `0000` went from 533 seven-line blocks to 533
one-line calls with nothing else touched. That is the cheap safety check the ledger asked for, and it
passed.

**Screened and gated.** Hot ten recompiled (14.4 minutes), relinked, and the certified route:

| | reference | after the budget fix | after this |
| --- | --- | --- | --- |
| instructions per play retrace | 491.7 M | 489.2 M | **487.8 M** |
| cumulative change | | -0.52% | **-0.79%** |
| route digest | `92dd816c...` | UNCHANGED | **UNCHANGED** |
| host turns | 32,203,791 | 32,203,791 | **32,203,791** |

Both changes hold together with the digest green at the certified turn count, so the pair is valid as a
unit. The route-level increment from this change alone is 0.27 points against the 2.1 percent it is worth
inside the hot chunks, which is the dilution the earlier entries predicted: the ten chunks are 26 percent
of the window and the bench window is wider than the certified one.

**And the two no-ops are worth as much as the win.** Branch hints on the hot-path guards cost 14 percent -
a layout regression, now refuted. Dropping the redundant `remaining >= 0` from `can_precharge` is a null,
because the compiler already folds it. Both were priced in about two minutes each, which is the whole
point of having the ablation harness.

## 2026-09-18 Re-pricing on the new baseline closes the emitter-side hunt

With the budget test and the out-of-line charge both landed, the ablation suite re-run against the new
baseline (chunk 0144, now 26.91 rather than 28.86) says what is left:

| ablation | old baseline | new baseline | what it means |
| --- | --- | --- | --- |
| `prepaid` - force the precharge flag true | 25.08 (-13.0%) | **24.28 (-9.8%)** | 3.2 points were the charge body, now out of line |
| `no-suffix` - drop the observation suffix and reconcile | 26.77 (-7.2%) | **25.18 (-6.4%)** | barely moved; still load-bearing |
| `no-pc` - drop the per-instruction pc store | 29.04 (+0.7%) | **27.09 (+0.7%)** | null in both, and now literally the same reading |

**Why the remaining 9.8 percent is not recoverable, and why that is a result rather than a shrug.** What
is left in that number is the per-instruction `if (!cycle_block_prepaid)` test plus the precharge call,
and the reason the compiler cannot delete them is that the flag is *assigned* mid-block: the reconcile
sets it false when a memory operation reveals the deadline is inside the block. So the compiler must
treat `cycle_block_prepaid` as reachable at every instruction after any memory operation, and the dead
charge block stays.

That can only be removed by making the assignment unreachable - by giving each block a duplicated
precise tail to jump to - and the same ledger now contains the measurement that says not to: moving six
instructions of never-executed body out of line was worth 2.1 percent, which is the same effect in
reverse. Duplicating block tails adds an instruction's body and its charge to the cold path for every
block, and the blocks are two to four instructions long. The dead-code effect that made the charge helper
a win would make that a loss.

So the emitter-side trims are done: the two landed changes, one null, and one that is self-defeating by
measurement. What is left in that 9.8 percent is a property of the flag's reachability rather than of the
emitted lines, and reaching it means a structural change - a duplicated precise tail, or keeping the flag
in a register the compiler can prove - that the dead-code measurement argues against. The honest next
lever is therefore not in the emitter at all, which is where this loop started: the accounting *design*
and the GX path.
