# The seventh set of natives: leaf compute code (report)

Branch `game-natives-7` (from windows-release 1a13ca5), 2026-10-05. Built and tested against the snapshot
`E:\Github\Wind-Waker-Recomp-natives7-snap`: its `composite-src` and the `gGZLE01_recomp.dll` compiled from
it (RecompCore 11c1369, BlueWake 1a13ca5).

## Summary

- **19 bit-exact natives in six groups.** They cover libm's `fmod`, `sin`, `cos` and `tan`, and the game's
  random numbers and angle conversion built on `fmod`; the collision blocks' bounds; the Euler quaternions;
  the arc tangent; planes, polar coordinates and point winds; and JASystem's envelope oscillators and channel
  effector parameters. Together they cover **10.4 points of the game thread at Forest Haven and 3.2 at the
  sea**. These are the translated-module part of the functions' exclusive shares.
- Each native passed its randomized comparison with **zero mismatches**. Every function had 60,000 cases,
  and the native ran on 33,240 to 44,766 of them. Each run matched the module's translation in every
  register, flag, FPSCR bit, cycle count, cycle suffix, reservation and RAM byte. Each declined case left
  everything unchanged. Every case the native ran also ran through the hooked chunks, once with the natives
  on and once with them off, and matched the translation both times.
- **Speedup per call through the hooked chunks: 1.36x to 3.63x.** These are the lowest of three runs. Every
  kept native cleared 1.25x through the hooked chunks in every run. Through the dispatcher, the lowest
  speedups are 1.26x to 3.85x.
- **Estimated saving: about 3.8 points of the game thread at Forest Haven and 0.9 at the sea.** The estimate
  uses the lowest speedup measured for each native, through the dispatcher or the hooked chunks, and counts
  only the translated-module share. Most of the Forest Haven saving comes from `__ieee754_fmod` (1.09
  points), `MakeBlckMinMax` (0.53) and the audio channel's `updateEffectorParam` (0.41).
- **Dropped:** six more natives. They were exact but not 1.25x faster in every run: `cM3dGAab::SetMinMax`,
  `SetMin`, `SetMax`, `JASystem::Player::pitchToCent`, `cXyz::operator-`, `mDoLib_project` and `cM_atan2f`.
  `cLib_addCalc` and `cLib_addCalc2` were not made, because they are 60 Hz simulation sites.
- **Not attempted:** the spores (`dKyr_housi_move`, `dKyr_drawHousi`), `deformVtxPos_F32`,
  `GroundCrossGrpRp`, `fire_fly_move`, and JASystem's sequence and DSP updates. None of them is leaf code.
  The reasons are below.
- **Hooks:** `native_entries.py` certifies the 19 entries by hash, 201/201 with the earlier sets. I ran the
  builder's full step order twice on a copy of the snapshot's chunks. The first pass added 19 hooks in 8
  chunks. The second pass certified the same entries, added 0 hooks and left all 206 files byte-identical.

## What is native

`scripts/windows/native_leaf_gen.py` generates the natives from the translation. Each group has the same set
of files:

- `cmake/composite/native_<group>.c` and `.h`;
- `native_<group>_gen.inc`, the generated natives;
- `native_<group>_list.h`.

The shared runtime is `native_leaf_run.h`. The shared entry, gate and report code is `native_leaf_group.inc`.
Shares are the module part of the exclusive share, in points of the game thread, from `n7fh-guest.txt`
(Forest Haven, `Omori`) and `n7s41-guest.txt` (the sea, `sea:41`).

| Group (switch) | Native | Entry | Replayed with it | Share FH / sea |
|---|---|---|---|---:|
| libm (`BLUEWAKE_NATIVE_LIBM`) | `__ieee754_fmod` | 8032EC1C | - | 1.73 / 0.09 |
| | `fmod` | 80330E34 | `__ieee754_fmod` | 0.26 / 0.04 |
| | `cM_rad2s` | 80246044 | `fmod`, `__ieee754_fmod` | 0.42 / 0.04 |
| | `cM_rnd` | 802462C8 | `fmod`, `__ieee754_fmod` | 0.04 / 0.05 |
| | `cM_rndF` | 802463B0 | `cM_rnd`, `fmod`, `__ieee754_fmod` | 0.03 / 0.04 |
| | `cM_rndFX` | 802463E8 | `cM_rnd`, `fmod`, `__ieee754_fmod` | 0.00 / 0.00 |
| | `sin` | 80330C84 | `__ieee754_rem_pio2`, `__kernel_sin`, `__kernel_cos` | 0.20 / 0.40 |
| | `cos` | 8033071C | the same | 0.66 / 0.66 |
| | `tan` | 80330D5C | `__ieee754_rem_pio2`, `__kernel_tan` | 0.10 / 0.16 |
| bgblk (`BLUEWAKE_NATIVE_BGBLK`) | `cBgW::MakeBlckMinMax` | 80247C4C | - | 1.54 / 0.10 |
| | `cBgW::MakeBlckBnd` | 80247CD4 | `MakeBlckMinMax`, `MakeBlckTransMinMax`, `PSVECAdd` | 0.54 / 0.04 |
| rot (`BLUEWAKE_NATIVE_ROT`) | `JMAEulerToQuat` | 80301150 | - | 0.28 / 0.46 |
| calc (`BLUEWAKE_NATIVE_CALC`) | `cM_atan2s` | 802460D0 | `U_GetAtanTable` | 0.33 / 0.06 |
| geom (`BLUEWAKE_NATIVE_GEOM`) | `cM3d_CalcPla` | 8024A6F0 | `PSVECSubtract`, `PSVECCrossProduct`, `PSVECMag`, `PSVECScale`, `PSVECDotProduct` | 0.62 / 0.05 |
| | `cSPolar::Val` | 80254214 | `cM_atan2f`, `cM_atan2s`, `U_GetAtanTable`, `cSAngle` operators, `cSPolar::Formal` | 0.61 / 0.02 |
| | `dKyw_pntwind_get_info` | 8008A230 | `dKyr_get_vectle_calc`, `get_vectle_calc`, `vectle_calc`, `PSVECSquareDistance` | 0.51 / 0.06 |
| jas (`BLUEWAKE_NATIVE_JAS`) | `JASystem::TOscillator::getOffset` | 8028DF2C | `TOscillator::calc`, `Driver::getUpdateInterval`, `__cvt_fp2unsigned` | 0.18 / 0.10 |
| | `JASystem::TOscillator::calc` | 8028E238 | `Driver::getUpdateInterval`, `__cvt_fp2unsigned` | 0.37 / 0.13 |
| | `JASystem::TChannel::updateEffectorParam` | 8028C3A8 | `calcPan`, `calcEffect`, `updateMixer`, `updateAutoMixer`, `Calc::sinfT`, `Calc::sinfDolby2`, `DSPBuffer::setAutoMixer`, the Driver's getters | 2.01 / 0.66 |

Each native runs its function to the return, with every callee it reaches replayed with it. There are no
stops or resumes, and nothing in the natives calls back into translated code or the host.

### How a native stays exact

- **Generated from the translation, not written by hand.** `native_leaf_gen.py` builds on the fifth round's
  generator (`native_gx_gen.py`'s `Gen` and `Chunk`).
  - Each instruction's C comes from the blocks' prepaid copies in the snapshot's `composite-src`.
  - The guest registers become locals.
  - Calls inside the native's ranges become gotos, with LR kept in a local.
  - Jump tables become a `switch` over the function's own blocks.
- **The generator's additions:**
  - **Extracted loops.** The translator lifts some loops out into `static void loop_X` functions:
    `__ieee754_fmod`'s bit loops, `MakeBlckBnd`'s triangle loop, and the oscillator's segment walk. These
    become blocks of the native. This includes loops entered by fall-through, as in `updateEffectorParam`.
    `native_entries.py`'s hash for this set covers these loops' text as well as the fragments.
  - **Stores go only to plain RAM.** Each store is made in place, and its old bytes go to an undo log
    (`lf_st`). A store anywhere else marks the run bad, and the native declines. The set writes no gather-pipe
    bytes and hands the host nothing.
  - **FP follows the translation.**
    - Single loads use a hardware fast path (`lf_f32_from_bits`) that is bit-identical to the
      translation's conversion. It falls back to that conversion off the fast path.
    - `frsp` is inline and exact for non-NaN operands. Everything else goes to the interpreter.
    - `fctiwz` uses the translation's inline path (`nr_fctiwz`).
    - `fdiv` and `frsqrte` use the interpreter's own functions, run on a scratch CPU state that holds the
      run's FPSCR. The translation hands both to the interpreter too.
  - **One test of the clock per block.** `lf_floor` computes `floor = -min(budget, deadline)` once per run.
    Each block leader then subtracts its cycles and checks `downcount >= floor`.
    - That check equals the translation's deadline test.
    - At the very end of a turn's budget it is stricter by one block's cycles. The native then declines
      where the translation would still have run, which is always exact.
  - **The host is asked once per run.** At a boundary between chunks, `lf_silent` applies the edge filter's
    own test: filter on, host quiet, address unwatched.
    - The host's half of the test is asked once per run and kept. The address's half is checked at every
      crossing.
    - Nothing of the host's runs during a native. A flag raised meanwhile counts as raised a moment later,
      as for the earlier sets.
  - **`MakeBlckBnd` has a room test.** `lf_room_80247CD4` in `native_bgblk.c` runs before any replayed
    work. It reads the block's triangle count `n` and declines at once when `74 + 135·n` cycles do not fit
    above the floor. This spares a long replay that a deadline inside the loop would only undo.
- **The natives decline, changing nothing** (every stored byte is put back, latest first). Declines are
  counted by reason:
  - `state`: an exception pending, a write journal, an alias over MEM1, FP unavailable, a rounding mode
    other than nearest, or scaled paired singles;
  - `clock`: the budget is spent, or a deadline falls inside the work;
  - `memory`: a load or store that is not plain MEM1;
  - `fp`: an operand the translation would hand the interpreter, such as a NaN, an infinity or a zero
    divisor;
  - `host`: a boundary between chunks that would not pass silently;
  - `path`: a path the native does not replay.
    - `sin`, `cos` and `tan` decline on arguments past 2^20·π/2, which take `__kernel_rem_pio2`.
    - `getOffset` declines on the OSReport paths, where an oscillator has no data.
    - `updateEffectorParam` declines where a jump table leads outside its functions.
- **The decline gate.** Each native has its own `NativeGate` from `native_replay.h`. An entry that keeps
  declining (160 of 256 calls) is left to the translation for the next 16,384 calls.
  `bluewake_native_gate_enabled = 0` turns the gate off, and the tests use that. None of these natives is
  expected to decline often in play, so the gate is a guard.
- **Report at exit:** each group prints a line `[native-<group>] name=native/declined/skipped(reason
  counts)`.

## Tests

`tests/native_leaf_test.c`, with `tests/native7_harness.h` and `tests/native_leaf_hooked.h`. The build and run
commands are at the top of the test and the hooked header. Compiles and runs used affinity FFFF.

What each case randomizes:

- registers, CR, XER, FPSCR, the reservation (sometimes on a granule the function stores to) and cycle state;
- the arguments and the data the function reads: r2's constants, r13's seeds and table pointers, a
  background's vertices, triangles and blocks, the sine and arc tangent tables, the point winds, the
  oscillator's curves and tables, a channel and its mixer;
- input values, mostly as play has them. Sometimes zeros, denormals, huge values, infinities and NaNs, and
  every `fmod` path: exact multiples, equal magnitudes, subnormal operands and results, long exponent gaps;
- pointers, sometimes unaligned, a mirror, hardware, past MEM1, or aliasing each other;
- the budget spent or not, and deadlines near and inside the work;
- a write journal, aliases over MEM1, an exception pending;
- the host busy, the edge filter off, the boundaries watched.

The `s_clean` mode keeps enough cases free of those conditions that every native runs on more than 30,000.

Results, the final run (`native_leaf_test` and its hooked build, 60,000 cases per function):

| Native | Cases | Native ran, identical | Declined, unchanged | Mismatches | Declines by reason |
|---|---:|---:|---:|---:|---|
| `__ieee754_fmod` | 60,000 | 42,042 | 17,958 | 0 | state 7879, clock 6121, fp 3958 |
| `fmod` | 60,000 | 42,074 | 17,926 | 0 | state 7883, clock 6184, fp 3859 |
| `cM_rad2s` | 60,000 | 42,117 | 17,883 | 0 | state 7895, clock 4880, memory 170, fp 1114, host 3824 |
| `cM_rnd` | 60,000 | 41,893 | 18,107 | 0 | state 7880, clock 6098, memory 329, fp 287, host 3513 |
| `cM_rndF` | 60,000 | 41,638 | 18,362 | 0 | state 7887, clock 6171, memory 324, fp 472, host 3508 |
| `cM_rndFX` | 60,000 | 41,396 | 18,604 | 0 | state 7869, clock 6138, memory 329, fp 737, host 3531 |
| `sin` | 60,000 | 39,755 | 20,245 | 0 | state 7886, clock 4043, memory 342, fp 1765, path 6209 |
| `cos` | 60,000 | 39,742 | 20,258 | 0 | state 7857, clock 4080, memory 327, fp 1787, path 6207 |
| `tan` | 60,000 | 38,871 | 21,129 | 0 | state 7890, clock 4658, memory 317, fp 2049, path 6215 |
| `cBgW::MakeBlckMinMax` | 60,000 | 36,814 | 23,186 | 0 | state 7872, clock 5426, memory 6848, fp 3040 |
| `cBgW::MakeBlckBnd` | 60,000 | 33,240 | 26,760 | 0 | state 7873, clock 10406, memory 3260, fp 4277, host 944 |
| `JMAEulerToQuat` | 60,000 | 43,790 | 16,210 | 0 | state 7869, clock 5535, memory 2732, fp 74 |
| `cM_atan2s` | 60,000 | 44,766 | 15,234 | 0 | state 7885, clock 4175, memory 223, fp 2951 |
| `cM3d_CalcPla` | 60,000 | 33,446 | 26,554 | 0 | state 7848, clock 6041, memory 4938, fp 3658, host 4069 |
| `cSPolar::Val` | 60,000 | 36,383 | 23,617 | 0 | state 7882, clock 6838, memory 2574, fp 2823, host 3500 |
| `dKyw_pntwind_get_info` | 60,000 | 39,365 | 20,635 | 0 | state 7890, clock 8247, memory 2940, fp 720, host 838 |
| `TOscillator::getOffset` | 60,000 | 37,494 | 22,506 | 0 | state 7883, clock 4195, memory 434, fp 816, host 1016, path 8162 |
| `TOscillator::calc` | 60,000 | 41,759 | 18,241 | 0 | state 7860, clock 4981, memory 2225, fp 1342, host 1833 |
| `TChannel::updateEffectorParam` | 60,000 | 38,213 | 21,787 | 0 | state 7861, clock 6627, memory 583, fp 3051, host 2456, path 1209 |

**Total: 1,140,000 cases, 0 mismatches.**

- **The hooked build** links the eleven chunks the natives hook or reach. They were hooked on a copy by the
  builder's steps, along with every native the chunks name. Every case the native ran (the "identical"
  column) was also run through the hooked chunks from the function's entry, once with all natives on and
  once with all off. It matched byte for byte both times, for all 19 natives.
- **The hook counters** came out at exactly twice the direct runs. This shows the hooks reached the natives.
- **These decline rates come from the test's design.** About a third of the cases deliberately set a
  condition the native must decline on. In play, the expected declines are deadlines inside the work
  (rare), a busy host at the boundaries, and NaN or infinite operands. The `[native-<group>]` lines at exit
  show the real proportion.

## Speed

Measured by the benchmark in the test. Each function is called 1,000,000 times on a typical input. The best
round is kept: of five through the dispatcher, and of three each way through the hooked chunks.

- **Through the dispatcher:** the module's translation through its dispatcher, against the native called
  directly.
- **Through the hooked chunks:** the hooked chunks entered at the function's entry and run to the return,
  with this set's natives off and on. The earlier sets and the direct-call leaves are on in both, as in play.
- **Variant rows** use longer inputs:
  - `fmod`: 1234.5678 by 2π, a long quotient;
  - `sin`, `cos` and `tan`: 1234.5, a full argument reduction;
  - `MakeBlckBnd`: 30 triangles instead of 6;
  - point winds: one wind present.

Three runs (`last_hooked`, `bench3`, `bench4`). The ns columns are from the last run. The ratio columns are
the lowest of the three runs.

| Native | Translation, dispatcher (ns) | Native (ns) | Hooked, off (ns) | Hooked, on (ns) | Lowest, dispatcher | Lowest, hooked |
|---|---:|---:|---:|---:|---:|---:|
| `__ieee754_fmod` | 63.9 | 21.9 | 64.3 | 23.8 | 2.89x | 2.70x |
| `__ieee754_fmod` (variant) | 125.6 | 32.6 | 126.4 | 34.6 | 3.85x | 3.63x |
| `fmod` | 69.1 | 24.5 | 68.7 | 25.0 | 2.82x | 2.74x |
| `fmod` (variant) | 131.8 | 34.8 | 132.7 | 37.3 | 3.63x | 3.56x |
| `cM_rad2s` | 40.4 | 27.8 | 41.4 | 29.0 | 1.42x | 1.42x |
| `cM_rnd` | 125.4 | 86.9 | 131.3 | 87.6 | 1.43x | 1.49x |
| `cM_rndF` | 144.5 | 95.7 | 151.3 | 97.9 | 1.48x | 1.55x |
| `cM_rndFX` | 151.3 | 104.2 | 160.8 | 107.8 | 1.45x | 1.49x |
| `sin` | 35.9 | 26.9 | 40.5 | 28.8 | 1.33x | 1.41x |
| `sin` (variant) | 92.4 | 61.2 | 109.8 | 63.7 | 1.51x | 1.72x |
| `cos` | 48.1 | 34.4 | 61.8 | 36.7 | 1.39x | 1.68x |
| `cos` (variant) | 97.5 | 58.8 | 115.9 | 60.6 | 1.61x | 1.88x |
| `tan` | 104.3 | 68.3 | 123.9 | 68.2 | 1.53x | 1.78x |
| `tan` (variant) | 133.2 | 72.3 | 163.2 | 74.7 | 1.80x | 2.17x |
| `cBgW::MakeBlckMinMax` | 39.2 | 24.7 | 40.8 | 26.2 | 1.58x | 1.52x |
| `cBgW::MakeBlckBnd` | 596.7 | 343.9 | 630.0 | 347.9 | 1.73x | 1.81x |
| `cBgW::MakeBlckBnd` (variant) | 2685.0 | 1507.7 | 2905.4 | 1499.0 | 1.78x | 1.92x |
| `JMAEulerToQuat` | 64.2 | 49.6 | 105.7 | 52.1 | 1.29x | 2.03x |
| `cM_atan2s` | 43.5 | 29.7 | 42.7 | 30.9 | 1.46x | 1.36x |
| `cM3d_CalcPla` | 265.6 | 157.6 | 232.5 | 163.0 | 1.68x | 1.41x |
| `cSPolar::Val` | 335.5 | 220.8 | 397.2 | 228.4 | 1.51x | 1.74x |
| `dKyw_pntwind_get_info` | 169.1 | 79.8 | 173.5 | 79.8 | 2.12x | 2.09x |
| `dKyw_pntwind_get_info` (variant) | 458.5 | 285.2 | 465.9 | 286.6 | 1.61x | 1.62x |
| `TOscillator::getOffset` | 208.1 | 114.6 | 230.1 | 118.4 | 1.79x | 1.93x |
| `TOscillator::calc` | 164.1 | 99.9 | 180.6 | 98.8 | 1.60x | 1.80x |
| `TChannel::updateEffectorParam` | 647.4 | 512.6 | 700.5 | 509.9 | 1.26x | 1.37x |

**Where the time goes.**

- `fmod` gains the most. Its time is in integer bit loops, which the native runs on local registers
  instead of the guest CPU state in memory, with one clock test per block.
- In the FP-heavy natives, such as `CalcPla`, `Val`, the oscillator and `updateEffectorParam`, the replayed
  FP operations and the undo log are most of what is left:
  - the operations follow the translation's inline paths, with FPSCR flags computed exactly;
  - the undo log costs about 10 percent in experiments.

## Share of the game thread removed (estimate)

For each native, the estimate is the module part of the exclusive share of the functions it covers, times
(1 − 1/speedup). The speedup is the lowest measured for that native in any of the three runs, through the
dispatcher or the hooked chunks, variants included. The profiles are the brief's `n7fh-guest.txt` and
`n7s41-guest.txt` (four E-cores, Smooth Motion off; those runs had the sixth set on).

| Place | Share of the functions covered (module part) | Removed (estimate) |
|---|---:|---:|
| Forest Haven (`Omori`) | 10.44 | **about 3.8** |
| The sea (`sea:41`) | 3.15 | **about 0.9** |

**Largest gains at Forest Haven (points):**

| Native | Points removed |
|---|---:|
| `__ieee754_fmod` | 1.09 |
| `MakeBlckMinMax` | 0.53 |
| `updateEffectorParam` (with `updateMixer`, `calcPan` and the others) | 0.41 |
| `MakeBlckBnd` | 0.23 |
| `cSPolar::Val` | 0.21 |
| `dKyw_pntwind_get_info` | 0.19 |
| `cos`, `cM3d_CalcPla` | 0.18 each |
| `fmod` | 0.16 |
| `TOscillator::calc` | 0.14 |
| `cM_rad2s` | 0.13 |

**At the sea (points):**

| Native | Points removed |
|---|---:|
| `cos` | 0.18 |
| `updateEffectorParam` | 0.14 |
| `sin`, `JMAEulerToQuat` | 0.10 each |
| `__ieee754_fmod`, `tan` | 0.06 each |
| the oscillator | 0.10 together |

**What the estimate assumes:**

- The natives run on most calls in play.
- A callee's exclusive share is credited to the native that covers it. For example, `__ieee754_rem_pio2`
  is credited to `cos`, and `updateMixer` to `updateEffectorParam`, its only caller.
- The `__ieee754_fmod` saving is credited to that native's own speedup, although in play much of it runs
  inside the `cM_rnd` and `cM_rad2s` natives.

`estimate.py` and `speedtable.py`, in the scratch directory, compute these figures from the logs.

## Dropped, and why

**Made, tested exact (0 mismatches), and dropped as too slow.** The rule was at least 1.25x through the hooked
chunks in every run.

- **`cM3dGAab::SetMinMax`:** 1.26x through the dispatcher, too close to the bar to clear 1.25x in every run.
- **`SetMin` and `SetMax`:** about 1.0x. They are a dozen instructions each, and the native's entry and exit
  cost what the blocks save.
- **`JASystem::Player::pitchToCent`:** 0.94x.
- **`cXyz::operator-`** (with `PSVECSubtract`) **and `mDoLib_project`** (with `PSMTXMultVec`): 1.6-1.8x
  against a fully translated call, but only 1.22-1.24x through the hooked chunks. In play their leaf is
  already native (`native_vec.c` and `native_math.c`). `cXyz::operator-` also spans two chunks.
- **`cM_atan2f`** (`cM_atan2s` and the conversion to radians): 1.14-1.26x through the hooked chunks.
  `cSPolar::Val` still replays it inside its own native.

**Not made:**

- **`cLib_addCalc` and `cLib_addCalc2`.** They are 60 Hz simulation sites: their translations contain the
  `bluewake60` sites that `prepare_simulation_60hz.py` rewrites by frame-rate mode, so no single body can be
  certified.
- **`dKyw_pntwind_get_vecpow`.** Its `cXyz::operator*` is a game-math entry whose hook sits before the block
  leader.

**Not attempted (not leaf code):**

- **`dKyr_drawHousi`** (1.90, the spores' draw). It writes the FIFO through many GX calls, which needs the
  sixth round's stops and resumes, and that round lost time in play.
- **`dKyr_housi_move`** (1.48). It calls collision checks and other non-leaf code.
- **`deformVtxPos_F32`** (0.88). It is a long loop over every vertex: a native would often meet a deadline
  inside it and decline after doing most of the work. It also ends in `DCStoreRange`.
- **`GroundCrossGrpRp`** (0.45). It is recursive and makes virtual calls into the polygon checks.
- **`fire_fly_move`** (0.60). It lives in d_a_ff's REL, not in the DOL chunks the generator reads.
- **`updateSeq`, `updateTrack` and `updatecallDSPChannel`.** They call into the sequence interpreter, the
  DSP, `OSDisableInterrupts`/`OSRestoreInterrupts` and `OSReport`. Their leaves, the oscillator and the
  channel's effector and mixer, are in this set.

## Hooks, and the two passes

`scripts/windows/native_entries.py` defines the set inside one delimited block, from `# --- The seventh set`
to `# --- end of the seventh set's entries ---`:

- **`SEVENTH_GROUPS`**, the six groups. Each hook has the form
  `if (bluewake_native_<group>_enabled && bluewake_native_<group>_XXXXXXXX(ctx)) goto return_dispatch_...;`
  at the entry's label.
- **19 `ENTRIES`** with their fragments, covering every function each native replays, and the SHA-256
  hashes the test verified.
- **An `entry_hash` override** that also hashes the extracted `loop_X` functions those fragments call. It
  applies to this set's groups only.

**Two passes.** I ran the full step order twice on a copy of the snapshot's finished `chunks_dol` and
headers (`build/natives7/steps`, each step with affinity FFFF):

```
pass 1:
  native_game_math.py: native game math: 12/12 certified entries, 0 new hooks in 0 chunks
  fast_blocks.py: prepaid block copies: 0 blocks in 0 chunks
  lean_memory.py: lean memory accesses: 0 in 0 chunks
  return_ranges.py: return dispatch range tests: 0 in 0 chunks
  prepare_native_j3d.py: native J3D: 2 recovered matrix functions certified in 1 chunks
  native_entries.py: native entries: 201/201 certified, 19 new hooks in 8 chunks
pass 2:
  native_game_math.py: native game math: 12/12 certified entries, 0 new hooks in 0 chunks
  fast_blocks.py: prepaid block copies: 0 blocks in 0 chunks
  lean_memory.py: lean memory accesses: 0 in 0 chunks
  return_ranges.py: return dispatch range tests: 0 in 0 chunks
  prepare_native_j3d.py: native J3D: 2 recovered matrix functions certified in 1 chunks
  native_entries.py: native entries: 201/201 certified, 0 new hooks in 0 chunks
pass 1 changed 8 files: chunk_0034_text1_800896E0.c, chunk_0145_text1_802456E0.c, chunk_0146_text1_802496E0.c,
  chunk_0148_text1_802516E0.c, chunk_0162_text1_802896E0.c, chunk_0163_text1_8028D6E0.c,
  chunk_0191_text1_802FD6E0.c, chunk_0203_text1_8032D6E0.c
pass 2 against pass 1: byte-identical (206 files)
```

The full diff is in `tests/native_leaf_hooks.diff` (19 hooks, plus each chunk's includes). A representative
hunk:

```diff
 /* bluewake: certified native entries (scripts/windows/native_entries.py) */
+#include "native_bgblk.h"
+#include "native_calc.h"
+#include "native_libm.h"
 #include "native_bg.h"
...
 label_8032EC1C:
+    if (bluewake_native_libm_enabled && bluewake_native_libm_8032EC1C(ctx))
+        goto return_dispatch_8032D6E0;
     ctx->pc = 0x8032EC1Cu;
     cycle_block_prepaid = dolrecomp_block_can_precharge(ctx, 12u);
```

**The hook sits on the entry's label**, before `ctx->pc` is set. A call from another chunk still reaches it
through the chassis loop and the host's edge service, so the host sees what it saw before. The native's
`lf_silent` test covers the boundaries inside its own path. `native_entries.py` refuses a fragment that
names a host-watched address, and all 19 certified without one.

**The generator is reproducible.** Rerunning `native_leaf_gen.py` on the snapshot's `composite-src` gives
byte-identical `native_<group>_gen.inc` and `native_<group>_list.h` for all six groups.

## Integrating

1. **Merge `game-natives-7` into windows-release.** `git merge-tree` against windows-release 6d3e073 is
   clean. The additions to shared files are self-contained and delimited, with no lines removed or
   reordered:
   - `cmake/composite/CMakeLists.txt`: the six `native_<group>.c`, after `native_draw.c`;
   - `cmake/composite/module_export.c`:
     - the six includes;
     - inside the existing `on` (`BLUEWAKE_NATIVE_ENTRIES`), a loop that sets each group's
       `bluewake_native_<group>_enabled` (off with `BLUEWAKE_NATIVE_<GROUP>=0`) and registers its exit
       report;
   - `scripts/windows/native_entries.py`: the seventh set's block;
   - `scripts/windows/build.py`: `training_fingerprint` adds `native_leaf_run.h`, `native_leaf_group.inc`
     and each group's `.c`, `.h`, `_list.h` and `_gen.inc`.

   New files:
   - `cmake/composite/native_{libm,bgblk,rot,calc,geom,jas}.{c,h}`, `native_*_gen.inc` (generated,
     committed), `native_*_list.h`, `native_leaf_run.h`, `native_leaf_group.inc`;
   - `scripts/windows/native_leaf_gen.py`, a developer tool the builder does not run;
   - `tests/native7_harness.h`, `native_leaf_test.c`, `native_leaf_hooked.h`, `native_leaf_hooks.diff`.
2. **The set is on by default**, under `BLUEWAKE_NATIVE_ENTRIES`, unlike the sixth set. If you prefer it
   opt-in until it has been measured in play, change only the loop's condition in `module_export.c`.
3. **Build.** On the first pass, the builder's native-entries step should log `native entries: 201/201
   certified, 19 new hooks in 8 chunks`. On the second, `0 new hooks`.
   - windows-release now carries RecompCore 39dcacb (feaacac). That change is the GX worker's vertex upload,
     not the translator, so the hashes should hold.
   - If a seventh-set entry fails to certify, its translation changed, and that native stays unhooked (the
     translation runs). To restore it:
     1. Run `python scripts/windows/native_leaf_gen.py <build>\composite-src`. It rewrites the groups'
        `_gen.inc` and `_list.h`.
     2. Rebuild `native_leaf_test` against the new module and run it, plain and hooked.
     3. Take the new hashes from `python scripts/windows/native_entries.py --hashes <build>\composite-src`.
4. **What gets recompiled:**
   - the eight hooked chunks: 0034 (`dKyw_pntwind_get_info`), 0145 (`cM_*`, `MakeBlck*`), 0146
     (`cM3d_CalcPla`), 0148 (`cSPolar::Val`), 0162 (`updateEffectorParam`), 0163 (the oscillator), 0191
     (`JMAEulerToQuat`) and 0203 (libm);
   - the six `native_<group>.c`;
   - `module_export.c`.

   The training fingerprint changes, so the builder retrains the speed profile.
5. **In play,** read the six `[native-libm]`, `[native-bgblk]`, `[native-rot]`, `[native-calc]`,
   `[native-geom]` and `[native-jas]` lines at exit, at Forest Haven and the sea. They show runs, declines
   by reason, and the calls the gate skipped.
   - For an A/B check, `BLUEWAKE_NATIVE_<GROUP>=0` leaves one group to the translation, and
     `BLUEWAKE_NATIVE_ENTRIES=0` leaves all natives to it.
   - Measure CPU ms per frame with Link moving.
6. **Run `native_leaf_test` once against the integrated module** to confirm exactness: the plain build and
   the hooked build, as recorded at the top of the test and in `native_leaf_hooked.h`.

## Commits

```
5ff78f5 Natives, seventh set: comments naming all six groups
8afe695 Natives, seventh set: the comments naming its groups, all six
e06e334 Natives, seventh set: cM_atan2f dropped
2e060ba Natives, seventh set: MakeBlckBnd's room test; the hooks native_entries.py writes
c8e0044 tests/native_leaf_test.c: the seventh set against the module's translation
fba89fc Natives, seventh set: each group's header comment
ab55804 Natives, seventh set: JASystem::TChannel::updateEffectorParam, the host asked once a run
9f6bcbf Natives, seventh set: JASystem's envelope oscillator, and what was tried and dropped
f042e77 Natives, seventh set: hooks, module switches and the training fingerprint
da0dfc2 Natives, seventh set: fast single loads, inline frsp and fctiwz, loops entered by fall-through
a8099be Natives, seventh set: leaf compute code replayed from its translation
```
