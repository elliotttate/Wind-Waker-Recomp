# Windows performance and exactness harness

The scripts behind the measurements in `docs/status/CURRENT.md` (2026-10-03 onward). They drive a built
BlueWake (`build\windows\<copy>\BlueWake.exe`) by scripted input and test warps, read its stderr log, and
compare or profile runs. Run them from PowerShell (the `.ps1` files) and Python 3 (the `.py` files; the image
tools need `numpy` and `Pillow`). Nothing here downloads anything or ships with a release.

Paths are relative to this folder: runs write `build\windows\test-<tag>\` (a data folder with its own
settings.ini, card and stderr.txt) and, for dumps, `build\windows\test-<tag>-dump\`.

## Before you start

- **A copy to test**, never the play folder: copy `build\windows\BlueWake` to e.g. `build\windows\W-test`
  and point `-App` at it; variants of one module with another `BlueWake.exe` are how renderer changes were
  compared.
- **Saves the routes start from**, in `build\windows\test-saves\` (private, made from your own play, not in
  the repository): `outset-start.card` (a memory card whose first file stands on Outset; every route loads
  it), and for `state_dumps.ps1` the save states `gohma-lava.bwstate`, `drc-lava-room.bwstate`,
  `drc-lava-bridge.bwstate`.
- **Don't take focus from someone using the PC**: the test windows open in front. And never stop a BlueWake
  you did not start.

## Runs

| Script | What it does |
| --- | --- |
| `tour_run.ps1` | Loads the Outset save, then visits `-Places` (stage:room:point) by test warp (`BLUEWAKE_TEST_WARP`) every `-Stop` retraces from retrace 1200, Link running and turning (`-MovesKind survey/long/tour`). `-Unpaced` runs uncapped, `-Mask` sets CPU affinity (`0x000F0000` = four E-cores of an i9-13900K, the slow-CPU stand-in), `-Settings` writes settings.ini, `-Env` adds environment variables. |
| `survey.ps1` + `survey.py` | The 47-place survey: per place, game FPS and the game thread's, GX worker's and render worker's CPU per game frame. |
| `survey_sm.py` | The same for Smooth Motion runs, with the in-between frames' matching thread. |
| `survey_drain.py` | The same with the draw-done drain per frame (`drain_ms` of the `[fps]` lines). |
| `compare_apps_sm.ps1` | Several app copies at six heavy places, Smooth Motion at 60, paced (`-Mask`, `-Apps`). |
| `appab.ps1`, `envab.ps1` | A/B of app copies, or of one environment variable's values, uncapped (`survey_drain.py`). |
| `capture_run.ps1` | The Outset route with frame captures (`BLUEWAKE_CAPTURE_*`) and Link's position probes (`BLUEWAKE_PLAYER_PROBE`). Captures are asynchronous readbacks: compare the `[player-scene-state] ... pos=` lines between builds, not the capture hashes. |
| `state_run.ps1` | Boots into a save state (`-State`). |

## Exactness of what is drawn

Smooth Motion dumps (`DOL_AURORA_FRAME_INTERP_DUMP`, game frames `_FROM` to `_TO`) are keyed by game frame,
so paced runs of two builds dump the same frames. Real frames must be identical; in-between frames may
differ where the change is in the in-between path.

| Script | What it does |
| --- | --- |
| `sm_compare.ps1` + `dump_compare.py` | The Outset route, game frames 880-910 (override with `-Env`), compared with a reference dump folder. |
| `place_dumps.ps1` + `dump_maxdiff.py` | Per place, frames 900-904 of two apps; real frames identical or not, and each in-between frame's differing pixels and largest difference. |
| `state_dumps.ps1` | The same booting into the lava save states (frames 300-304). |
| `diff_pair.py`, `triptych.py` | Images: two frames and their difference; one frame across several dump folders, optionally a zoomed crop. |

## Profiles

| Script | What it does |
| --- | --- |
| `placeprof.ps1` | One place, uncapped on four E-cores, Smooth Motion off: samples the game thread's guest pc (`guest_sampler.py`, or call stacks with `-Stacks`, `guest_stack_sampler.py`) and the busiest host threads' instruction pointers (`thread_sampler_w32.py`). Guest functions are named from the zeldaret decompilation's symbols (a `tww` checkout beside this repository, or `BLUEWAKE_TWW`). |
| `smprof.ps1` | The host threads only, Smooth Motion on, paced, all cores. |
| `symprof.py` | Symbolizes a host thread profile with the exe's PDB (`SYMPROF_APP` = the exe that ran; a rebuilt exe gives wrong names). |
| `threads_at.ps1` + `thread_cpu.py` | Every thread's CPU over 15 s at one place. |

## HD texture packs without a Wind Waker HD disc

`make_test_pack.py ISO OUT [PROCESSES] [--mark] [--no-mips]` makes a private Dolphin-format pack from the
GameCube disc: every model and BTI texture decoded, upscaled 4x (2x above 256 texels), named with the WWHD
importer's key (`scripts/wwhd/formats.py`, `GCTexture.filename()`), with mip chains; `--mark` draws a fine grid
into every texture so aliasing shows. `check_decode.py` draws a sheet of each GameCube format's decode. The
packs are game-derived: keep them in `build\` and never commit or share them. Use one with
`DOL_AURORA_TEXTURE_PACK=<folder>`.
