# The fourth set of natives: report

Branch `game-natives-4`, based on `99a20cc`, 2026-10-04. These are bit-exact native C versions of the game
functions that the 2026-10-04 game-thread profiles show hot at Forest Haven (`sea:41`) and Dragon Roost
Island (`sea:13`) and that no earlier set covered:

- the environment's colour blends;
- J3D's key-frame animation, with its inverse-transpose leaf;
- two collision-checker leaves.

All ten functions attempted were kept. Every test passes with zero mismatches. The natives cover 5.4
percent of the game thread's samples at Forest Haven and 4.3 percent at Dragon Roost. They should remove
about **3.2 points at Forest Haven and 2.6 at Dragon Roost**.

## What was converted

| Function | Address | Chunk | Source | Profile share, FH / DRI |
| --- | --- | --- | --- | --- |
| `s16_data_ratio_set` (d_kankyo) | 0x8018F894 | 0099 (and both widescreen variants) | `native_kankyo.c` | 0.34 / 0.72 (both copies) |
| `kankyo_color_ratio_set` | 0x8018F8E4 | 0099 (and both widescreen variants) | `native_kankyo.c` | 0.54 / 0.86 |
| `s16_data_ratio_set` (d_kyeff) | 0x8019803C | 0101 | `native_kankyo.c` | (with the above) |
| `JMAHermiteInterpolation` | 0x803012D8 | 0191 | `native_anim.c` | 0.56 / 0.43 |
| `J3DGetKeyFrameInterpolation<f32>` | 0x802F2DAC | 0188 | `native_anim.c` | 0.25 / 0.17 |
| `J3DGetKeyFrameInterpolationS` (with `J3DHermiteInterpolationS`, 0x802F06D8) | 0x802F072C | 0187 | `native_anim.c` | 0.32 / 0.28, and 0.28 / 0.17 for `J3DHermiteInterpolationS` |
| `J3DAnmTransformKey::calcTransform` | 0x802F0954 | 0187 | `native_anim.c` | 1.07 / 1.12 |
| `J3DPSCalcInverseTranspose` | 0x802DA584 | 0182 | `native_anim.c` | 0.67 / 0.16 |
| `cCcD_DivideArea::CalcDivideInfoOverArea` | 0x8024170C | 0144 | `native_cc.c` | 0.83 / 0.14 |
| `cM3dGCyl::SetC` | 0x80251D88 | 0148 | `native_cc.c` | 0.58 / 0.21 |

The shares are each function's exclusive share of the game thread's guest-pc samples, from
`gp4-fh-guest.txt` and `gp4-dri-guest.txt`. A later Forest Haven profile, `gp5`, shows the same functions
0.1 to 0.2 lower.

**How each one runs.** Each is hooked at its entry by `scripts/windows/native_entries.py`, only where the
translated body hashes to the certified value. The native either runs the whole function to its `blr` or
declines and changes nothing, so that the translation runs instead.

- `kankyo_color_ratio_set` covers its three `s16_data_ratio_set` calls.
- `J3DGetKeyFrameInterpolation<f32>` covers its `JMAHermiteInterpolation` calls, which sit in another
  chunk.
- `calcTransform` covers its whole tree: the nine channels, the binary searches, and both
  interpolations with their splines, across three chunks.

**Shared layer.** `cmake/composite/native_replay.h` holds the layer the three groups share:

- the translation's inline floating-point paths, applied to values on a local FPSCR: `inline_fp.h`'s
  single rounding, the fma tie correction, `fcmpo`'s code or'ed into the FPSCR, FPRF, NI flush,
  `fctiwz` as GXRuntime's `ppc_fctiw` leaves it, and the paired-single ops;
- the clock, charged block by block: prepaid blocks, the budget check, deadline refunds, and the inline
  `_savegpr`/`_restgpr`;
- a store log, so that nothing is committed until the native knows it will finish.

**What a run leaves.** When a native runs, it leaves the same values as the translation in all of these:

- every GPR and FPR (both halves) as the last instruction to write it left it;
- CR, XER's CA, LR, CTR and the FPSCR;
- the reservation;
- downcount, and the last access's cycle suffix (FP loads and stores, `mtlr`, and lean stores where a
  reservation is held);
- every byte of RAM it writes, including the frames;
- pc at the return address.

**When a native declines.** A native declines whenever exactness is not certain:

- FP is off, the rounding mode is not nearest, an exception is pending, a write journal is set, or
  aliases overlap MEM1;
- an address is not plain RAM, or a read lies under one of the native's own stores;
- the conversion's magic double is not the usual one;
- a block would stop for the budget, or the deadline falls inside the work;
- a floating-point operation would leave the inline path (a NaN key time, for example);
- a chunk boundary the path crosses would not pass silently. Passing silently means the edge filter is
  on, the watch list is ready, the host is quiet, and none of the boundary addresses is watched.

**Two speeds per native.** The animation and collision natives try a fast single-precision replay first.
It is valid when all inputs are plain singles and every result is zero or at least 2^-125, so double
rounding is innocuous. The fast replay reads ranges it has validated beforehand. When it cannot finish,
a general replay runs, which follows every double-precision step. The tests count both paths.

**Plain joints.** A joint all of whose channels have at most one key is left to the second set's
`transform_simple` hook, which is faster on that shape (61 ns against about 87 here). That hook follows
this set's hook at the same label. Likewise, where this set's `J3DGetKeyFrameInterpolationS` declines,
the second set's key-s hook gets the call.

**`SetC` asserts.** `SetC`'s two asserts call `JUTAssertion` and `OSPanic`, and the host watches
`OSPanic`. The native declines both assert paths (a NaN component, or a component outside ±1e32). That
is why `native_entries.py` leaves 0x80006C4C out of the watch check for this one fragment, as the third
set did for `fopAcM_findObjectCB`.

## What was dropped or not attempted, and why

No native was dropped for inexactness. These candidates from the brief were left alone:

- **Already native from earlier sets.**
  - `J3DMtxCalcMaya::calcTransform` (`native_mtxcalc.c`).
  - `J3DModel::calcWeightEnvelopeMtx` (`native_skin.c`).
  - `J3DUClipper::clip` (`native_game_math.c`, sphere-clip and box-clip).
  - `dBgW::ChkGrpThrough` (`native_bg.c`).
  - `cM3d_Cross_MinMaxBoxLine` (`native_game_math.c`, box-line).
  - `PSMTXConcat` (`native_math.c`).
- **`JPABaseField::calcVel`, `JPABaseParticle::calcVelocity`.**
  - Both contain the 60 Hz simulation patch sites (0x8025A174 onward and 0x8025EBE8 onward).
  - `prepare_simulation_60hz.py` rewrites those sites around `bluewake_simulation_*` calls.
  - A native would have to reproduce the simulation's per-mode state. The hash would then certify a
    translation that differs by frame-rate mode.
- **The JPA draw execs (`JPADrawExecStripeCross`, `JPADrawExecStripe`, `JPADrawExecRotBillBoard`),
  `drawWave` and `drawVrkumo`.**
  - These are FIFO writers, and every GX call would have to reproduce the gather pipe's bytes in order.
  - They make virtual calls through the draw context, and some are large.
  - This was not cheap enough to make exact in this round.
- **`wave_move`.** It is large, it calls the sine tables and other functions across chunks, and its
  share is spread over many blocks. It was deferred.
- **The collision traversal (`cBgW::GroundCrossGrpRp`, `cBgW::GroundCrossRp`, `cBgS::GroundCross`,
  `dBgS::WallCorrect`, `dCcMassS_Mng::Chk`).** It is recursive, it calls through vtables, and it crosses
  many chunk boundaries. It would need the third set's resumable design.
- **`J3DMtxCalcBasic::recursiveCalc`, `J3DModel::calc`, `mDoExt_McaMorf::calc`.** These are virtual,
  deep call trees. Their leaves (`calcTransform`, `J3DMtxCalcMaya`, the PSMTX leaves) are now all
  native.
- **2D (`J2DPane::draw`, `J2DPrint::parse`, `JUTResFont::drawChar_scale`).** These are virtual draws
  and FIFO writers.
- **Not attempted for lack of time, but good next candidates.** Both follow this round's patterns
  exactly:
  - `J3DGetKeyFrameInterpolation<s>` (0x802F2A88; 0.26 at DRI, 0.01 at FH), the same shape as `<f32>`
    over shorts;
  - `float_kankyo_color_ratio_set` (0x8018F9F8; 0.13 at DRI, 0.08 at FH), the float twin of
    `kankyo_color_ratio_set`.

## Tests

Each test is a randomized comparison of the native against the module's translation, run as in play:
direct calls and the edge filter on, the host quiet, and an edge service that fails a case if asked
anything before the return address. The module is
`E:\Github\Wind-Waker-Recomp\build\windows\W-final\gGZLE01_recomp.dll`.

**What a case compares.** Every byte of the CPU state, including the cycle suffix, and of the test's
writable pages must match. Where the native declines, nothing may have changed. RAM outside the test's
pages is read-only in both images.

**What the cases cover:**

- inputs: zeros, denormals, huge values, infinities, NaNs, both register halves, the FPSCR's enables, and
  NI with and without the host's FTZ/DAZ;
- addresses: aliasing, frames below RAM, and non-RAM addresses;
- the budget and deadline edges, including cases right at the edge, which must run;
- an unquiet host, an edge filter that is off, and watched boundaries;
- write journals and MEM1 aliases.

The shared harness is `tests/native4_harness.h`. Build commands are at the top of each test.

| Test | Function | Cases | Identical | Of those, by the general replay | Declined unchanged | Mismatches |
| --- | --- | --- | --- | --- | --- | --- |
| `native_kankyo_test` | s16_data_ratio_set (d_kankyo) | 60,000 | 44,958 | | 15,042 | 0 |
| | kankyo_color_ratio_set | 60,000 | 39,266 | | 20,734 | 0 |
| | s16_data_ratio_set (d_kyeff) | 60,000 | 45,016 | | 14,984 | 0 |
| `native_anim_test` | JMAHermiteInterpolation | 60,000 | 40,946 | 8,046 | 19,054 | 0 |
| | J3DGetKeyFrameInterpolation<f32> | 60,000 | 46,476 | 9,471 | 13,524 | 0 |
| | J3DGetKeyFrameInterpolationS | 60,000 | 45,921 | 9,195 | 14,079 | 0 |
| | J3DAnmTransformKey::calcTransform | 60,000 | 39,234 | 6,964 | 20,766 | 0 |
| | J3DPSCalcInverseTranspose | 60,000 | 40,769 | 5,817 | 19,231 | 0 |
| `native_cc_test` | CalcDivideInfoOverArea | 60,000 | 41,131 | | 18,869 | 0 |
| | cM3dGCyl::SetC | 60,000 | 34,753 | | 25,247 | 0 |

`native_anim_test ... --module-natives` also passes, with the module's own natives on: the second set's
`transform_simple` and key-s, and the PSMTX leaves.

**The hooks, end to end.** Built with `-DNATIVE4_HOOKED=1` and linked against the hooked chunks, every
case the native runs is run twice more through the hooked chunks from the function's entry:

1. with every native on (this set's, the earlier sets', and the leaves behind the direct calls);
2. with all of them off.

A small chassis loop continues in the next chunk where one leaves at a boundary. Both runs must match the
module's translation byte for byte. For `calcTransform` and `J3DGetKeyFrameInterpolationS`, the cases this
set's native declines (other than the ones where it must) also run through the chunks. That exercises the
second set's hooks behind this set's, and the inner hooks inside the translation.

The hooked chunks are the eight chunk files below. They were copied from the main checkout's
`composite-src`, hooked with `native_entries.py`, and compiled with the module's flags (no PGO). The
recipe is in `native4_harness.h`. At 30,000 cases per function, the results were:

| Entry | Also identical through the hooked chunks, natives on and off |
| --- | --- |
| 8018F894 / 8018F8E4 / 8019803C | 22,514 / 19,543 / 22,476 |
| 8024170C / 80251D88 | 20,559 / 17,322 |
| 803012D8 / 802F2DAC / 802DA584 | 20,451 / 23,193 / 20,201 |
| 802F072C | 23,894 (917 of them declined by this set, for the second set's hook or the translation) |
| 802F0954 | 22,943 (3,315 of them declined by this set) |

Each kankyo and cc native's counter came out at exactly twice its direct runs (for example
`[native-kankyo] s16-ratio=45028` for 22,514 cases), so each hook fired on every case it should have.
The anim counters came out at least twice, because the inner hooks fire inside the translations too. A 10,000-case run with
`--module-natives` also passes.

## Per-call speedups

Times are in ns per call, best of five or six rounds. The columns are:

- **translation**: the module's PGO-built translation through its dispatcher;
- **native**: the native called directly;
- **hooked, on / off**: the hooked chunk entered at the function's label (its entry and return dispatch
  included, no dispatcher), with this set's natives on, then off. The other sets' natives are on in both,
  as in play. The test's chunks have no PGO, so the off column is not directly comparable with the
  translation column.

| Function, case | Guest cycles | Translation | Native | Hooked, on | Hooked, off |
| --- | --- | --- | --- | --- | --- |
| s16_data_ratio_set (d_kankyo) | 20 | 26.5 | 8.5 | 9.2 | 24.8 |
| kankyo_color_ratio_set | ~135 | 136.6 | 41.6 | 45.0 | 133.6 |
| s16_data_ratio_set (d_kyeff) | 20 | 27.3 | 8.4 | 9.3 | 24.8 |
| CalcDivideInfoOverArea, three axes converting | | 108.7 | 56.2 | 58.6 | 115.9 |
| cM3dGCyl::SetC | | 63.3 | 28.2 | 30.3 | 68.3 |
| JMAHermiteInterpolation, a spline | 46 | 69.2 | 25.3 | 27.8 | 98.4 |
| J3DGetKeyFrameInterpolation<f32>, 20 keys | 129 | 122.3 | 61.6 | 64.3 | 159.2 |
| J3DGetKeyFrameInterpolationS, 20 keys | 153 | 156.7 | 46.3 | 48.9 | 124.0 (the second set's key-s native) |
| calcTransform, rotation keyed (20 keys), scale and translation one key | 629 | 596.3 | 243.1 | 246.8 | 625.4 |
| calcTransform, all nine channels keyed | 1457 | 1468.4 | 624.5 | 636.8 | 1637.8 |
| J3DPSCalcInverseTranspose | 48 | 167.2 | 53.1 | 55.2 | 189.2 |

The natives run 2 to 3.5 times faster than the translation. Through the hook, a native costs about 1 to
12 ns more than a direct call. For `J3DGetKeyFrameInterpolationS`, the second set's key-s native is
faster than the translation where it runs: 106.6 ns through the dispatcher with module natives on. It
takes only word-aligned tables, about a fifth of the calls, and declines the rest.

## Expected share of the game thread saved

Each function's saving is its share times (1 − hooked-on time ÷ module translation time), using the
representative cases above.

- The profiles sample guest pcs in translated code, so the shares are translation time, and the
  translation is the right baseline. A native's time lands on its caller's last guest pc.
- For `calcTransform`, the two cases are averaged (58 percent).
- `J3DHermiteInterpolationS` is covered through `J3DGetKeyFrameInterpolationS`, its only caller in the
  decomp. It is counted at the key-s rate.

| Function | Saved | FH share | FH saved | DRI share | DRI saved |
| --- | --- | --- | --- | --- | --- |
| calcTransform | 58% | 1.07 | 0.62 | 1.12 | 0.65 |
| JMAHermiteInterpolation | 60% | 0.56 | 0.34 | 0.43 | 0.26 |
| J3DGetKeyFrameInterpolationS | 69% | 0.32 | 0.22 | 0.28 | 0.19 |
| J3DHermiteInterpolationS (inside key-s) | 69% | 0.28 | 0.19 | 0.17 | 0.12 |
| J3DGetKeyFrameInterpolation<f32> | 47% | 0.25 | 0.12 | 0.17 | 0.08 |
| J3DPSCalcInverseTranspose | 67% | 0.67 | 0.45 | 0.16 | 0.11 |
| kankyo_color_ratio_set | 67% | 0.54 | 0.36 | 0.86 | 0.58 |
| s16_data_ratio_set (both) | 65% | 0.34 | 0.22 | 0.72 | 0.47 |
| CalcDivideInfoOverArea | 46% | 0.83 | 0.38 | 0.14 | 0.06 |
| cM3dGCyl::SetC | 52% | 0.58 | 0.30 | 0.21 | 0.11 |
| **Total** | | **5.44** | **≈ 3.2** | **4.26** | **≈ 2.6** |

These are estimates, for three reasons:

- **Declines in play.** The fraction of calls that decline in play is not known. Declines come at budget
  or deadline edges, with the host busy, and on plain joints, which stay with the second set. The exit
  report lines give the real native/declined counts.
- **Where samples land.** A native's time is sampled at the caller's last guest pc. Some of the time
  sampled under `calcTransform` is therefore the second set's natives (`transform_simple`, key-s)
  running for it, and the saving there may be a little lower.
- **Different inputs.** Real key counts differ from the benchmark's 20.

## The hooks

`scripts/windows/native_entries.py` gains three delimited blocks of additions; no existing line is
removed or moved:

1. **Groups and entries.** The groups `kankyo`, `anim` and `cc`, and ten entries with their certified
   hashes. Each hash covers the callees its native stands in for.
2. **Mod variants.** A `main_function` override that also finds a mod variant's function
   (`func_8018D6E0__mod_widescreen...`). Chunk 0099 has two widescreen variants, whose fragments hash
   the same as the base's and are hooked the same way.
3. **The `OSPanic` exemption.** `FOURTH_HOST_OWN` leaves `OSPanic` out of `SetC`'s watch check, as
   described above.

**On a copy of the main checkout's tree** (`build/natives4-hooks`: 18 base chunk files, the two variants,
and `generated.h`), the step printed:

- first run: `native entries: 25/25 certified, 14 new hooks in 10 chunks`;
- second run: `0 new hooks`.

**On a fresh tree** (the same copy with every `native_entries.py` hook and include taken out), it
printed `25/25 certified, 29 new hooks in 16 chunks`: the earlier sets' 15 hooks and this set's 14. A
second run printed `0 new hooks`. The result matches the incremental copy apart from the order of two
`#include` lines.

A representative diff:

```diff
--- build/natives4-hooks-before/chunks_dol/chunk_0187_text1_802ED6E0.c
+++ build/natives4-hooks/chunks_dol/chunk_0187_text1_802ED6E0.c
@@ -5,4 +5,6 @@
 #include "gather_pipe.h"
 #include "../generated.h"
+/* bluewake: certified native entries (scripts/windows/native_entries.py) */
+#include "native_anim.h"
 /* bluewake: prepaid block copies (scripts/windows/fast_blocks.py) */
@@ -49088,4 +49090,6 @@
 label_802F072C:
+    if (bluewake_native_anim_enabled && bluewake_native_anim(ctx, 0x802F072Cu))
+        goto return_dispatch_802ED6E0;
     if (bluewake_native_game_math_enabled &&
         bluewake_native_game_math(ctx, 0x802F072Cu))
@@ -50866,4 +50870,6 @@
 label_802F0954:
+    if (bluewake_native_anim_enabled && bluewake_native_anim(ctx, 0x802F0954u))
+        goto return_dispatch_802ED6E0;
     if (bluewake_native_game_math_enabled &&
         bluewake_native_game_math(ctx, 0x802F0954u))
--- build/natives4-hooks-before/chunks_dol/chunk_0148_text1_802516E0.c
+++ build/natives4-hooks/chunks_dol/chunk_0148_text1_802516E0.c
@@ -10220,6 +10222,8 @@
 label_80251D88:
+    if (bluewake_native_cc_enabled && bluewake_native_cc(ctx, 0x80251D88u))
+        goto return_dispatch_802516E0;
     ctx->pc = 0x80251D88u;
     cycle_block_prepaid = dolrecomp_block_can_precharge(ctx, 14u);
```

The full set of added lines:

- 7 kankyo hooks: 0x8018F894 and 0x8018F8E4 in chunk 0099 and in each of its two widescreen variants,
  plus 0x8019803C in chunk 0101;
- 5 anim hooks: 0x803012D8 in 0191, 0x802F2DAC in 0188, 0x802F072C and 0x802F0954 in 0187, and
  0x802DA584 in 0182;
- 2 cc hooks: 0x8024170C in 0144 and 0x80251D88 in 0148;
- each chunk's `#include` of its group's header under the mark.

## Integration

1. **Merge.** Merge `game-natives-4` into `windows-release`.
   - Against `99a20cc`, the branch's changes are additions only: 16 files including this report, 0
     deletions.
   - `windows-release` has moved on by two commits, `8eedff6` (RecompCore d687c69) and `2fce4e5` (power
     throttling). Neither touches these files, and `git merge-tree` reports no conflicts.
2. **No new source step.** The existing `native-entries` step (`native_entries.py`) now carries this
   set. After a build, the last line of `build/windows/logs/native-entries.log` should read
   `native entries: 25/25 certified, 29 new hooks in 16 chunks`, or `14 new hooks in 10 chunks` where
   the earlier sets' hooks are already in place.
3. **Module sources.**
   - `cmake/composite/CMakeLists.txt` adds `native_kankyo.c`, `native_anim.c` and `native_cc.c` after
     `native_search.c`. `native_replay.h` is header-only.
   - `module_export.c` turns the three groups on with the other certified entries: `BLUEWAKE_NATIVE_MATH=1`
     (the Windows default), unless `BLUEWAKE_NATIVE_ENTRIES=0`.
   - At exit, the module prints `[native-kankyo]`, `[native-anim]` (with the general-replay counts) and
     `[native-cc]` with native/declined counts.
4. **What recompiles.**
   - The scripts changed, so the source tree is regenerated. `sync_tree` keeps unchanged files, so only
     the hooked chunks change:
     - `chunk_0099_text1_8018D6E0.c`, and its variants in `chunks_mod_widescreen` and
       `chunks_mod_widescreen1610` when those mods are on;
     - `chunk_0101_text1_801956E0.c`, `chunk_0144_text1_802416E0.c`, `chunk_0148_text1_802516E0.c`,
       `chunk_0182_text1_802D96E0.c`, `chunk_0187_text1_802ED6E0.c`, `chunk_0188_text1_802F16E0.c` and
       `chunk_0191_text1_802FD6E0.c`.
   - Those recompile, together with `module_export.c` and the three new sources.
   - `training_fingerprint` in `build.py` now lists `native_replay.h` and the six new sources, so the PGO
     profile is retrained and every chunk recompiles with the new profile. `native_entries.py` was
     already listed.
5. **After a build, check the step and play.**
   - Check `native-entries.log` for 25/25 certified.
   - Play Forest Haven and Dragon Roost, then read the three report lines.
   - A step message such as `kankyo_color_ratio_set: not the certified translation (...); not hooked`
     means a translator or earlier-step change has altered a fragment. That function is then left to the
     translation.
6. **To re-certify a function:**
   1. Rebuild the module from that tree.
   2. Rerun the function's test against the new module (60,000 cases, zero mismatches).
   3. Rerun the test in hooked mode against the newly hooked chunks.
   4. Only then replace the hash with the one `native_entries.py --hashes` prints.

## Notes

- **Host flags between check and crossing.** The animation natives check once, at entry, that each
  boundary on their path would pass silently. A host flag raised between that check and the crossing
  looks the same as one raised a few nanoseconds later. That is a timing difference, as with any host
  event, not a different computation. The second set's natives work the same way.
- **Host floating-point mode.** The natives follow NI: with NI set, the host runs with FTZ/DAZ, as
  `ppc_fpscr_control_updated` arms it. The tests run both ways. Bit tests on values derived from
  floating point go through `nr_opaque` barriers, because clang otherwise folds them into FP compares,
  which are wrong under DAZ.
- **Callee natives.** `calcTransform` and the key-frame natives match whether the module's own natives
  are on or off (`--module-natives`), and through the hooked chunks with every native on or off.
- **Other builds.** The hooks come only from this Windows source step. A tree without them never calls
  these natives. The Mac build is unaffected.
- **Scratch.** The `build/natives4-*` directories, which hold the hooked copies, the objects and the
  hooked test executables, are git-ignored and are not part of the branch. No chunk source or game data
  is committed.
