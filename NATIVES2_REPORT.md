# The second set of natives: report

Branch `game-natives-2`, based on `40953dd`. Bit-exact native C versions of nine GZLE01 functions.
That is the eight in the brief, with the J3DFifoLoad family counted as three. Each has a randomized
comparison test against the release module's translation, and each is hooked at its entry only where
the translated body is the one its test compared against. Every function is kept. Every test passes
with zero mismatches, and each function ran natively in at least 33,000 of its cases.

## What was converted

| Function | Address | Native | Shape |
| --- | --- | --- | --- |
| `J3DFifoLoadPosMtxImm` | 0x802D8BD8 | `native_fifo.c` | one block, 32 cycles: 3 header writes and 12 words to the gather pipe |
| `J3DFifoLoadNrmMtxImm` | 0x802D8C58 | `native_fifo.c` | one block, 29 cycles: 9 words |
| `J3DFifoLoadNrmMtxImm3x3` | 0x802D8CC4 | `native_fifo.c` | one block, 29 cycles: 9 words |
| `cBgS_Chk::ChkSameActorPid` | 0x8024734C | `native_bg.c` | four blocks of integer compares |
| `dBgW::ChkGrpThrough` | 0x800A9684 | `native_bg.c` | split across two chunks: blocks from 0x800A96E0 on are in the next chunk |
| `PSMTXMultVecSR` | 0x8030DB24 | `native_vec.c` (a separate, delimited section) | one block, 21 cycles, paired singles |
| `J3DMtxCalcBasic::calcTransform` | 0x802F5090 | `native_mtxcalc.c` | with its calls: J3DGetTranslateRotateMtx (info form), PSMTXConcat, PSMTXCopy, the inline GPR save/restore |
| `J3DMtxCalcSoftimage::calcTransform` | 0x802F52BC | `native_mtxcalc.c` | with J3DGetTranslateRotateMtx (angles form), PSMTXConcat, PSMTXCopy |
| `J3DMtxCalcMaya::calcTransform` | 0x802F5508 | `native_mtxcalc.c` | split across two chunks, with the parent scale compensation |

Each native either runs the whole function (and its calls) to the `blr` or declines, changing nothing,
so that the translation runs. Running to the `blr` leaves every GPR, both halves of every FPR, CR,
XER, LR, CTR, FPSCR (FPRF of the last arithmetic, FI/FR, the fcmpu codes), the reservation, cycles and
downcount, the last access's cycle suffix, pc at the return address, and every RAM byte and gather-pipe
byte exactly as the translation leaves them. The header comment of each file states its contract. In
summary:

- **FIFO loads.** Pipe bytes go through `bw_gather_pipe_put` store by store when the host takes words one
  at a time, or when the batch would reach its flush point inside the function. That way every host call,
  and every read of the matrix after one, happens where the translation makes it. Otherwise all the bytes
  are appended to the batch at once. The native declines with no pipe writer, a non-plain-RAM matrix, a
  pending exception, a write journal, a spent budget, or a deadline inside the block.
- **Collision checks.** Blocks run in order with the translation's CR0 forms, SO included. The native
  checks the turn's budget and deadline at every block entry. The path through 0x800A96DC (the one block
  without a prepaid copy) stores the cycle suffix 0, as the translation does. A path that crosses
  ChkGrpThrough's chunk boundary (0x800A96E0 or 0x800A96F0) runs natively only when the crossing would
  pass silently, as in play: the edge filter is on, the watch list is loaded, the host is quiet, and the
  address is unwatched.
- **PSMTXMultVecSR.** When all 15 floats are bounded (finite, below 2^62), it runs on values with
  always-inline helpers. Otherwise it runs the translation's own statements on the registers, using the
  inline_fp.h and interpreter helpers, so NaN, infinity, denormal and huge operands all run natively and
  exactly. It declines on FP off, quantised GQR0 or LSQE, a write journal, non-RAM ranges, a pending
  exception, a spent budget or a deadline. Overlapping input and output is fine, because every load comes
  before the first store.
- **calcTransform.** The native reads all inputs, computes in host floats, checks, and only then writes:
  the callees' stack frames, J3DSys's current matrix and scales, the joint's scale flag and animation
  matrix, and every register. Host single arithmetic equals the translation's inline_fp.h paths while
  inputs are zero or normal, every result is zero or at least 2^-125 and finite, and the rounding mode is
  round to nearest. The fmuls rounding to a 25-bit multiplier leaves singles unchanged, the fused
  multiply-add tie correction matches fmaf, and double rounding is innocuous because 53 >= 2·24 + 2.

  The native declines otherwise. It also declines when any address it or a callee touches is not plain
  RAM, when a store overlaps another store or an input (checked branch-free over 4 store ranges × 16
  input ranges), when the budget or deadline falls inside the longest path (240, 256 or 320 cycles), or
  when any chunk boundary the translation crosses would not pass silently. Basic and Softimage each cross
  6 or 7 boundaries for the calls and returns. Maya also crosses its own chunk boundary.

## What was dropped, and why

No function was dropped. One approach was replaced:

- **calcTransform, register-emulating version.** The first exact calcTransform natives ran the
  translation's statements on the guest registers block by block, including the callees. They passed
  with zero mismatches, but they were slower than what the game runs today. In play, the module's
  existing natives already answer PSMTXConcat, PSMTXCopy and J3DGetTranslateRotateMtx. The version kept
  is the compute-then-commit form above. It passes the same test with zero mismatches, and it is
  1.5–3.0× faster than the translation with those module natives on.

Coverage limits by design: everything listed under the declines above runs the translation instead.
Examples are non-RAM or watched addresses, aliasing stores in calcTransform, denormal, huge or
non-finite values in calcTransform, rounding other than nearest, a budget or deadline inside the
function, a write journal, a boundary the host would act at, and a quantised paired-single setup.

## Tests

All tests are under `tests/`. Each has its exact build command and its run command at the top. They are
built with the brief's recipe: VS clang `-O2 -march=x86-64-v3 -ffp-contract=off`, the includes from the
main checkout's `ref/recompcore`, and `gxruntime.lib`. They run against the current release's module,
`E:\Github\Wind-Waker-Recomp\build\windows\BlueWake\gGZLE01_recomp.dll` (SHA-256 `48a97a5d08f5369e…`,
read-only and tested as a byte-identical copy). "Identical" means the native ran and every CPU byte,
RAM byte and pipe byte matched. "Declined" means the native declined and the state was verified
unchanged. The cases deliberately include declining inputs: non-RAM and alias addresses at each level,
spent budgets, deadlines inside the function, write journals, pending exceptions, rounding mode not
nearest, and overlaps. They also include edge values: zeros, denormals, huge values, NaN and infinity,
pipe batches filled so that a flush lands inside the function, a word-at-a-time pipe, and no writer.

Build (example; the others replace the test and native source as their headers say):

```
clang -O2 -march=x86-64-v3 -ffp-contract=off -Icmake/composite
  -IE:\Github\Wind-Waker-Recomp\ref\recompcore\GXRuntime\include
  -IE:\Github\Wind-Waker-Recomp\ref\recompcore\Source\Core\Core\PowerPC\StaticRecomp
  tests/native_fifo_test.c cmake/composite/native_fifo.c cmake/composite/gather_pipe.c
  E:\Github\Wind-Waker-Recomp\build\windows\app\gxruntime_build\gxruntime.lib -o native_fifo_test.exe
```

| Test (run) | Function | Cases | Identical | Declined | Mismatches |
| --- | --- | --- | --- | --- | --- |
| `native_fifo_test MODULE.dll` (60000 per function) | J3DFifoLoadPosMtxImm | 60,000 | 35,610 (4,329 with a flush inside) | 24,390 | **0** |
| | J3DFifoLoadNrmMtxImm | 60,000 | 35,767 (3,327 with a flush inside) | 24,233 | **0** |
| | J3DFifoLoadNrmMtxImm3x3 | 60,000 | 36,000 (3,366 with a flush inside) | 24,000 | **0** |
| `native_bg_test MODULE.dll` | ChkSameActorPid | 60,000 | 43,612 (10,852 returning true) | 16,388 | **0** |
| | ChkGrpThrough | 60,000 | 40,888 (8,892 true; 16,330 across the chunk boundary) | 19,112 (2,083 with the boundary in trouble) | **0** |
| `native_vec_sr_test MODULE.dll` | PSMTXMultVecSR | 60,000 | 37,292 (36,011 with non-finite results) | 22,708 | **0** |
| `native_mtxcalc_test MODULE.dll` (module natives off) | calcTransform Basic | 60,000 | 37,493 (20,765 with scale one; ≤233 cycles) | 22,507 | **0** |
| | calcTransform Softimage | 60,000 | 37,490 (20,698 with scale one; ≤249 cycles) | 22,510 | **0** |
| | calcTransform Maya | 60,000 | 33,565 (9,292 through the scale compensation; ≤311 cycles) | 26,435 | **0** |
| `native_mtxcalc_test MODULE.dll 60000 500000 --module-natives` (as in play) | all three | same counts as above | | | **0** |

End to end, `tests/native_entries_test.c` checks the hooks themselves. It compiles the nine hooked
chunks (from a copy of the player's prepared sources, after `native_entries.py`) with the module's
flags, and links them with the natives and the module runtime they call. For each function it runs
4,000 cases three ways: the module's translation (direct calls and edge filter on, host quiet), the
hooked chunks with the natives on, and the hooked chunks with the natives off. Every CPU byte, the
touched RAM and the pipe bytes must match across all three, and the full images are compared every 256
cases. Result: all 9 functions identical through the hooked chunks, natives on and off.

## Per-call speedups

Hooked chunk, natives off and on. This is the closest measure to a call in play: the chunk's entry and
return dispatch are included, the module's other natives are on, and the dispatcher is not included.

| Function | Off (ns) | On (ns) | Speedup | Time saved |
| --- | --- | --- | --- | --- |
| J3DFifoLoadPosMtxImm | 52.6 | 21.8 | 2.41× | 59% |
| J3DFifoLoadNrmMtxImm | 42.0 | 19.3 | 2.18× | 54% |
| J3DFifoLoadNrmMtxImm3x3 | 42.1 | 18.9 | 2.23× | 55% |
| ChkSameActorPid | 13.1 | 8.5 | 1.53× | 35% |
| ChkGrpThrough | 16.5 | 9.3 | 1.76× | 44% |
| PSMTXMultVecSR | 73.2 | 31.6 | 2.31× | 57% |
| calcTransform Basic | 164.5 | 96.6 | 1.70× | 41% |
| calcTransform Softimage | 193.1 | 95.2 | 2.03× | 51% |
| calcTransform Maya | 255.7 | 107.6 | 2.38× | 58% |

Standalone microbenchmarks (in each test; translation through the module's dispatcher vs the native):

- FIFO: 46.8 → 7.8, 38.3 → 6.8 and 38.3 → 6.7 ns. An empty dispatch costs 4.7 ns.
- ChkSameActorPid: 13.0 → 4.8 ns.
- ChkGrpThrough: a ground group 21.4 → 6.4 ns; a water group, through the chunk boundary, 17.4 → 5.7 ns.
- PSMTXMultVecSR: 64.9 → 27.1 ns.
- calcTransform with the module natives on (as in play):
  - Basic: 149.1 → 89.3 ns (scale one) and 222.4 → 94.6 ns (scaled).
  - Softimage: 185.4 → 90.2 ns and 265.9 → 88.8 ns.
  - Maya: 135.0 → 90.9 ns and 292.8 → 100.8 ns (compensated).
- calcTransform with the module natives off: 487.6, 580.7, 505.1, 627.2, 462.5 and 693.5 ns, against the
  same native times.

## Expected share of the game thread saved

Each function's exclusive share (Link running on Outset, from the brief) times its time saved per call:

| Function | Share | Saved |
| --- | --- | --- |
| ChkGrpThrough | 0.64% | 0.28% |
| calcTransform Maya | 0.46% | 0.27% |
| J3DFifoLoadPosMtxImm | 0.16% | 0.09% |
| ChkSameActorPid | 0.24% | 0.08% |
| calcTransform Basic | 0.14% | 0.06% |
| J3DFifoLoadNrmMtxImm | 0.09% | 0.05% |
| J3DFifoLoadNrmMtxImm3x3 | 0.03% | 0.02% |
| PSMTXMultVecSR | 0.03% | 0.02% |
| calcTransform Softimage | small | small |
| **Total** | **≈1.8%** | **≈0.86%** |

This is a lower bound. The calcTransform natives also replace their callees' time, and ChkGrpThrough's
native also replaces the trip through the chassis loop at its chunk boundary. Neither is in the
exclusive shares above. Counting them, roughly 0.9–1.2% of the game thread is expected, if the natives
rarely decline in play. The module prints native/declined counts at exit (below) to confirm that.

## The hooks

`scripts/windows/native_entries.py COMPOSITE_SRC` inserts two lines after each certified entry label:

```c
    if (bluewake_native_fifo_enabled && bluewake_native_fifo(ctx, 0x802D8BD8u))
        goto return_dispatch_802D56E0;
```

It also adds the native's header include under a marker line. An entry is hooked only if every fragment
of the function hashes to the certified SHA-256, in the base chunk and in every mod variant. The hash
covers the blocks from its entry label in each chunk it spans, every prepaid `bwfast` copy those blocks
jump to, and, for calcTransform, its callees' fragments. Whitespace is collapsed, and this script's and
`prepare_native_j3d.py`'s hooks are removed before hashing. No address of the function may be one the
host watches (`direct_calls.watched_addresses()`). Otherwise the step prints why and leaves the entry
alone. A rerun is idempotent and removes a hook it no longer certifies. `--hashes` prints the current
hashes.

| Entry | Certified hash |
| --- | --- |
| 802D8BD8 | `c1f2664aa12f9dd8c195fcf4a862e77793b889902ca390018f1b1b5090c4dbdf` |
| 802D8C58 | `6cee7486d365cb81b8d1b9e5b63dd0720e271af0d4bb323be726d4333e3ec9ad` |
| 802D8CC4 | `2affe02fcb1211515e55f97b2eab1c7f685fe661c30259af77361385fec49ee9` |
| 8024734C | `a3cf44b28b18dfc3b21022150567e7e5223ea3ff3ffdc777e374f7fdd806d1ee` |
| 800A9684 | `d491b8f1f60e6ad9ae20524d879bb8b1d74d9608e919e6c07601e8dcfa69227f` |
| 8030DB24 | `600139a8e4e0caabf4a3006b3eb737577c0934568e1741186eeec1b9565b7db9` |
| 802F5090 | `3e442f9bac67c268093ed4abdbcb9c4659fad59339a700282a7eb9e023b11d67` |
| 802F52BC | `7ad20637ab7fb125cfee5c1aa81bd982abfc8bbe5f26747bae359fa320fd56d2` |
| 802F5508 | `8df3c54d0620dc3152b7b640038b7637d9514d3e51cd6133f4dfb4b836f9c324` |

Run on a copy of the release build's prepared chunks (`build\windows\composite-src`), it printed
`native entries: 9/9 certified, 9 new hooks in 5 chunks`. A second run printed `0 new hooks in 0 chunks`.
The diff it produced, without context:

```diff
chunk_0041_text1_800A56E0.c
@@ -4,0 +5,2 @@
+/* bluewake: certified native entries (scripts/windows/native_entries.py) */
+#include "native_bg.h"
@@ -58738,0 +58741,2 @@
+    if (bluewake_native_bg_enabled && bluewake_native_bg(ctx, 0x800A9684u))
+        goto return_dispatch_800A56E0;
chunk_0145_text1_802456E0.c
@@ -4,0 +5,2 @@   (marker, #include "native_bg.h")
@@ -29419,0 +29422,2 @@
+    if (bluewake_native_bg_enabled && bluewake_native_bg(ctx, 0x8024734Cu))
+        goto return_dispatch_802456E0;
chunk_0181_text1_802D56E0.c
@@ -4,0 +5,2 @@   (marker, #include "native_fifo.h")
@@ -51510,0 +51513,2 @@   bluewake_native_fifo(ctx, 0x802D8BD8u) -> return_dispatch_802D56E0
@@ -52016,0 +52021,2 @@   bluewake_native_fifo(ctx, 0x802D8C58u) -> return_dispatch_802D56E0
@@ -52424,0 +52431,2 @@   bluewake_native_fifo(ctx, 0x802D8CC4u) -> return_dispatch_802D56E0
chunk_0188_text1_802F16E0.c
@@ -4,0 +5,2 @@   (marker, #include "native_mtxcalc.h")
@@ -58066,0 +58069,2 @@   bluewake_native_mtxcalc(ctx, 0x802F5090u) -> return_dispatch_802F16E0
@@ -60046,0 +60051,2 @@   bluewake_native_mtxcalc(ctx, 0x802F52BCu) -> return_dispatch_802F16E0
@@ -62110,0 +62117,2 @@   bluewake_native_mtxcalc(ctx, 0x802F5508u) -> return_dispatch_802F16E0
chunk_0195_text1_8030D6E0.c
@@ -4,0 +5,2 @@   (marker, #include "native_vec.h")
@@ -7845,0 +7848,2 @@
+    if (bluewake_native_vec_sr_enabled && bluewake_native_vec_sr(ctx))
+        goto return_dispatch_8030D6E0;
```

Checked on the hooked copy:

- `prepare_native_math.py` still certifies its four SDK leaves. The PSMTXMultVecSR hook sits after
  label 0x8030DB24, outside PSMTXMultVecArray's hashed range.
- None of `prepare_simulation_60hz.py`'s 57 site addresses lies inside a hooked function.

## Integration

1. **Merge** `game-natives-2` into `windows-release`. Its shared-file edits are additions only (17 files,
   4,099 insertions, 0 deletions since `40953dd`). Windows-release has moved on by `297c88d`, `238a829`
   and `04e0e1b`, which touch none of these files. The hunks to `build.py`, `cmake/composite/CMakeLists.txt`
   and `module_export.c` also apply, with offsets only, to the main checkout's current uncommitted
   `build.py` and CMakeLists changes (training tour, `--tiered` cold sources).
2. **Source step order** (`finish_tree` in `scripts/windows/build.py`). One new step goes after
   `native-j3d` and before `simulation-prepare`:
   guest-cpu → gpr-inline → chunk-headers → direct-calls → native-skin → native-game-math → fast-blocks →
   lean-memory → native-j3d → **native-entries** (`scripts/windows/native_entries.py`) → simulation-prepare →
   native-math.
   - It must run after `fast_blocks.py` and `lean_memory.py`, because its hashes include the prepaid
     copies they make.
   - It must run before `prepare_simulation_60hz.py` and `prepare_native_math.py`, whose manifests hash
     whole chunk files as they end up.
   - Its log is `build/windows/logs/native-entries.log`, and its last line is printed with the others. On a fresh tree
     it should read `native entries: 9/9 certified, 9 new hooks in 5 chunks`.
3. **Module sources**:
   - `CMakeLists.txt` adds `native_fifo.c`, `native_bg.c` and `native_mtxcalc.c` after
     `native_game_math.c`. `native_vec.c` gains the PSMTXMultVecSR section.
   - `module_export.c` enables the four groups with native math. On Windows that is
     `BLUEWAKE_NATIVE_MATH=1`, the default set by `win_entry.c`. `BLUEWAKE_NATIVE_ENTRIES=0` turns just
     this set off.
   - At exit, the module prints `[native-fifo]`, `[native-bg]`, `[native-vec] multvec-sr=` and
     `[native-mtxcalc]` with native/declined counts.
4. **What recompiles.**
   - `scripts/windows/*.py` and `build.py` are composite inputs, so the source tree is regenerated.
     `sync_tree` keeps unchanged files, so only the five hooked chunks change (and their mod variants
     when mods are on): `chunk_0041_text1_800A56E0.c`, `chunk_0145_text1_802456E0.c`,
     `chunk_0181_text1_802D56E0.c`, `chunk_0188_text1_802F16E0.c` and `chunk_0195_text1_8030D6E0.c`.
     Those compile again together with `module_export.c`, `native_vec.c` and the three new sources.
   - `training_fingerprint` now lists the new sources and the script, and already lists
     `module_export.c` and `native_vec.c`. The PGO profile is therefore retrained, and with the new
     profile every chunk compiles again. The main branch's `TRAINING_VERSION = "4"` retrains anyway.
5. **After a build**, check `native-entries.log` for 9/9 certified, then play and read the four report
   lines. A step message such as `not the certified translation; not hooked` means a translator or
   earlier-step change altered a fragment, and the function is then left to the translation. To
   re-certify:
   - Rebuild the module from that tree.
   - Rerun the function's test against it with zero mismatches, and `native_entries_test` against the
     hooked chunks.
   - Only then replace the hash with the one `native_entries.py --hashes` prints.

## Notes

- **Host flags between check and crossing.** The ChkGrpThrough and calcTransform natives check once,
  at entry, that each chunk boundary would pass silently. The host quiet flags (sources dirty, a
  decrementer or PI interrupt pending with EE on) are set by other threads. A flag raised between that
  check and the moment the translation would have reached the boundary looks the same as the flag
  arriving a few nanoseconds later. It is a timing difference of the kind any host event has, not a
  different computation.
- **Host floating-point mode.** Like inline_fp.h's inline paths, the natives assume the host's default
  floating-point mode (MXCSR round to nearest, no FTZ or DAZ).
- **Callee natives.** The calcTransform natives match the translation whether the module's own
  PSMTXConcat, PSMTXCopy and J3DGetTranslateRotateMtx natives are off or on. Those natives are exact
  themselves, and the test was run both ways.
- **Other builds.** The hooks come only from this Windows source step. A tree without them never calls
  the natives.
