# Natives, fifth set: the code that writes the GX FIFO

Branch `game-natives-5` (from windows-release 70b33dd), 2026-10-04. Tested against the W-n4 game module
(`build\windows\W-n4\gGZLE01_recomp.dll`, windows-release 70b33dd with the fourth round's natives).

## Summary

- **35 bit-exact natives** for the GX SDK's FIFO writers and their callees, J3D's GD fog and TEV-order
  writers and the GF forms. They cover about 7.6 percent of the game thread at Forest Haven and 7.3 at
  Dragon Roost. The translated-module part of that is 6.2 and 5.9 percent; the rest is host time at the
  host-watched GX entries.
- Each native passed its randomized comparison with **zero mismatches**. Every function had 60,000 cases,
  and the native ran on 31,499 to 45,617 of them. Each run matched the module's translation in every
  register, flag, cycle count, RAM byte and gather-pipe byte. Each declined case left everything unchanged.
  Every case also ran through the hooked chunks, once with the natives on and once with them off, and
  matched the translation both times.
- **Speedup through the hooked chunks: 1.15x to 4.06x per call.** I estimate the set removes **about 2.5
  points of the game thread at Forest Haven and 2.4 at Dragon Roost**. The estimate uses the lowest speedup
  measured for each function and counts only the translated-module share.
- **Dropped: 32 more natives.** I built them and they passed the same comparison, but they were not faster
  than their translation through the hooks. They are the setters that store mostly to RAM (`__GXData` or a
  GD list).
- **Not attempted:** the JPA draw executors, `drawWave` and `drawVrkumo`. They need calls into code the
  native does not replay, and the host has to see them at their boundaries. Reasons are below.
- **Hooks:** the 35 entries are certified by hash in `native_entries.py` (60/60 with the earlier groups).
  I ran the full step order twice on a copy of the builder's finished chunks. The second pass added 0 hooks
  and left all 207 files byte-identical.

## What is native

All 35 live in `cmake/composite/native_gx.c`, generated as `native_gx_gen.inc` from the translation by
`scripts/windows/native_gx_gen.py`. Their runtime support is in `native_gx_run.h`, and
`native_gx_list.h` lists them.

| Group | Natives (callees replayed with them) |
|---|---|
| Matrix loads | `GXLoadPosMtxImm`, `GXLoadNrmMtxImm`, `GXSetCurrentMtx` (+`__GXSetMatrixIndex`), `__GXSetMatrixIndex`, `GXSetIndTexMtx` |
| Textures | `GXLoadTexObj` (+`__GXDefaultTexRegionCallback`, `GXGetTexObjFmt`, `GXLoadTexObjPreLoaded`, `__GXDefaultTlutRegionCallback`), `GXLoadTexObjPreLoaded` (+TLUT callback), `GXInitTexObjLOD` |
| TEV | `GXSetTevColor`, `GXSetTevColorS10`, `GXSetTevKColor`, `GXSetTevOrder` |
| Vertex arrays, texture coordinates, channels | `GXSetArray`, `GXSetTexCoordGen2` (+`__GXSetMatrixIndex`), `GXSetNumTexGens`, `GXSetChanAmbColor`, `GXSetChanMatColor`, `GXSetNumChans`, `GXSetChanCtrl` |
| Fog | `GXSetFog` (+`__cvt_fp2unsigned`), `GXSetFogRangeAdj` |
| Draw start and its dirty state | `GXBegin` (its own blocks), `GXCallDisplayList`, `__GXSetSUTexRegs` (+`__SetSURegs`), `__GXSetVAT`, `__GXUpdateBPMask`, `__GXSetGenMode`, `__GXSetVCD` (+`__GXXfVtxSpecs`), `__GXXfVtxSpecs`, `__GXCalculateVLim` |
| J3D | `J3DGDSetFog` (+`__cvt_fp2unsigned`), `J3DGDSetTevOrder`, `GFSetTevColor`, `GFSetTevColorS10`, `GFSetFog` (+`__cvt_fp2unsigned`) |

### How a native stays exact

- **Generated from the translation, not written by hand.** `native_gx_gen.py` takes each instruction's C
  from the blocks' prepaid copies in the builder's `composite-src`. The guest registers become locals. Each
  block leader becomes a check that the block is prepaid and that neither the budget nor a deadline inside
  the block stops it. Calls inside the native's ranges become gotos with LR in a local. Jump tables become
  a `switch` over the function's own blocks. Indirect calls are replayed only when they reach the SDK's
  default callbacks. The FP arithmetic uses `native_replay.h`'s inline paths and the interpreter's own
  `fctiw`/`frsqrte`/`frsp`, on the run's FPSCR. The cycles, the downcount and the last access's cycle
  suffix (pipe store, FP/paired access, `mflr`/`mtlr`, inline register save, a RAM store under a
  reservation) are kept exactly as the copies keep them.
- **RAM stores go through an undo log.** Stores happen in place and their old bytes are logged. A decline
  restores them in reverse order, so nothing has changed when the translation then runs.
- **Gather-pipe stores are collected.** At the end, the bytes are appended to the module's batch
  (`bw_gather_pipe_buffer`) through the batch entry point. Where they would take the batch past its flush
  point, the batch is handed over first and the bytes follow it, a batch at a time. The host gets the
  same bytes in the same order and only the call boundaries differ (the "batch handed over first" column
  below).
- **The natives decline (no change)** when:
  - an exception is pending, a write journal is active, or an alias lies over MEM1;
  - the module is not batching the pipe (word mode, the FIFO trace);
  - FP is unavailable, or the rounding mode is not round-to-nearest where they do arithmetic;
  - a paired single is scaled;
  - the budget is spent, or a deadline falls inside the work;
  - a load or store is not plain MEM1 or the pipe;
  - a callback pointer is not the SDK's;
  - any boundary between chunks on the path is one the host would be asked about. This is the edge
    filter's own test: filter on, host quiet, address unwatched.
- **Host hooks still work.** The host's watch list includes the entries of `GXBegin`, `GXLoadTexObj`,
  `GXCallDisplayList`, `__GXSetDirtyState` and `__GXSendFlushPrim`, because their literals appear in
  `runtime/host/src/main.c`'s GX entry trace.
  - Every call into these functions from another chunk still goes round the chassis loop to the host's
    edge service, before it reaches the entry label where the hook sits. The host sees what it saw before.
  - `native_entries.py` normally refuses to hook a fragment that names a watched address.
    `FIFTH_HOST_OWN` exempts exactly these addresses, and only in those three functions' fragments.
  - The natives decline before any path that would reach `__GXSetDirtyState` or `__GXSendFlushPrim`. The
    translation then makes the call, and each dirty-state callee native runs at its own hook.
  - Any other watched address on a native's path makes it decline.
  - No native touches the JPA executors, so `draw_tags.c`'s particle tags are untouched.
- **Each native has its own entry point.** The hook calls `bluewake_native_gx_<entry>(ctx)` directly, with
  no dispatch on the address. `bluewake_native_gx(cpu, address)` remains for the tests.
- **Runtime cost was cut in `native_gx_run.h`.** The log counts live in the run, not the static log
  (every byte-pointer store would make the compiler reload them). Pipe bytes reach the batch 8 at a time;
  the log and the batch both have 8 spare bytes.

## Tests

`tests/native_gx_test.c` covers the 35 natives. Its build and run command is at the top of the file;
`native5_harness.h` holds the shared harness. The hooked mode (`-DNATIVE5_HOOKED=1`) links
`tests/native_gx_hooked.h` and the five hooked chunks; that file gives the recipe. Build commands, from
the worktree root (clang `-O2 -march=x86-64-v3 -ffp-contract=off`, includes as in the brief):

```
clang -O2 -march=x86-64-v3 -ffp-contract=off -Icmake/composite -Itests -I...\GXRuntime\include
  -I...\StaticRecomp tests/native_gx_test.c cmake/composite/native_gx.c cmake/composite/direct_calls.c
  cmake/composite/gather_pipe.c ...\gxruntime.lib -o native_gx_test.exe
native_gx_test E:\Github\Wind-Waker-Recomp\build\windows\W-n4\gGZLE01_recomp.dll 60000 1000000 [PREFIX,...]
```

What the cases vary:

- random registers, flags, FPSCR, reservation (sometimes on a granule the function stores to) and cycle
  state, with deadlines near and inside the work and the budget spent or not;
- `__GXData`, texture objects, colours, matrices, the small-data areas, the GD list and the TEV order
  table, filled randomly but biased toward the paths play takes;
- pointers that are unaligned, mirrored, hardware, past MEM1 or anything else;
- a write journal, aliases, a pending exception;
- the host busy (any boundary the native crossed would then mismatch), the edge filter off, and the
  callees' boundaries watched;
- the pipe batched, in word mode and unset, with the batch pre-filled at random so that runs cross its
  flush point.

The runs below are the final ones: the hooked build, 60,000 cases per function, pinned to the P-cores
(affinity 0xFF). Logs are in the scratch directory as `finalA.txt` and `finalB.txt`. There were zero
mismatches everywhere, and every count is at least 30,000. The last column comes from the hooked build.

| Function | Entry | Native ran (identical) | Of them, batch handed over first | Declined, unchanged | Also identical through the hooked chunks |
|---|---|---:|---:|---:|---:|
| `GXLoadPosMtxImm` | 80326F38 | 43,428 | 7,592 | 16,572 | 43,428 |
| `GXLoadNrmMtxImm` | 80326F88 | 41,701 | 5,659 | 18,299 | 41,701 |
| `GXSetTevColor` | 80325FA8 | 42,718 | 2,848 | 17,282 | 42,718 |
| `GXSetTevColorS10` | 8032601C | 42,741 | 2,765 | 17,259 | 42,741 |
| `GXSetTevKColor` | 80326090 | 42,819 | 1,447 | 17,181 | 42,819 |
| `GXSetArray` | 80322568 | 45,212 | 1,827 | 14,788 | 45,212 |
| `GXSetTevOrder` | 803263A0 | 40,222 | 702 | 19,778 | 40,222 |
| `GXCallDisplayList` | 80326B80 | 37,554 | 1,101 | 22,446 | 37,554 |
| `GXBegin` | 803230C4 | 37,523 | 383 | 22,477 | 37,523 |
| `GXLoadTexObjPreLoaded` | 80324D50 | 31,815 | 3,512 | 28,185 | 31,815 |
| `GXLoadTexObj` | 80324EE8 | 31,499 | 3,552 | 28,501 | 31,499 |
| `__GXSetSUTexRegs` | 803253B8 | 39,013 | 4,282 | 20,987 | 39,013 |
| `__GXSetVAT` | 803221D8 | 41,508 | 9,911 | 18,492 | 41,508 |
| `__GXSetMatrixIndex` | 80327364 | 44,270 | 2,143 | 15,730 | 44,270 |
| `__GXUpdateBPMask` | 80325CD4 | 42,441 | 678 | 17,559 | 42,441 |
| `__GXSetGenMode` | 803233B0 | 45,617 | 716 | 14,383 | 45,617 |
| `__GXSetVCD` | 80321958 | 38,595 | 2,751 | 21,405 | 38,595 |
| `__GXXfVtxSpecs` | 803214B0 | 42,314 | 1,290 | 17,686 | 42,314 |
| `__GXCalculateVLim` | 803219AC | 42,457 | 0 | 17,543 | 42,457 |
| `GXSetTexCoordGen2` | 80322604 | 36,527 | 3,262 | 23,473 | 36,527 |
| `GXSetNumTexGens` | 803228D4 | 44,944 | 1,352 | 15,056 | 44,944 |
| `GXSetChanAmbColor` | 80324390 | 41,903 | 944 | 18,097 | 41,903 |
| `GXSetChanMatColor` | 80324484 | 41,884 | 902 | 18,116 | 41,884 |
| `GXSetNumChans` | 80324578 | 44,930 | 1,382 | 15,070 | 44,930 |
| `GXSetChanCtrl` | 803245BC | 42,771 | 1,353 | 17,229 | 42,771 |
| `GXSetCurrentMtx` | 80326FD8 | 42,967 | 2,104 | 17,033 | 42,967 |
| `GXSetFog` | 803265A8 | 34,608 | 2,953 | 25,392 | 34,608 |
| `GXSetFogRangeAdj` | 80326758 | 39,971 | 3,809 | 20,029 | 39,971 |
| `GXInitTexObjLOD` | 80324B68 | 35,784 | 0 | 24,216 | 35,784 |
| `GXSetIndTexMtx` | 80325810 | 37,170 | 1,877 | 22,830 | 37,170 |
| `J3DGDSetFog` | 802D85F8 | 32,205 | 0 | 27,795 | 32,205 |
| `J3DGDSetTevOrder` | 802D80D0 | 41,333 | 0 | 18,667 | 41,333 |
| `GFSetTevColor` | 802AFDDC | 42,781 | 2,842 | 17,219 | 42,781 |
| `GFSetTevColorS10` | 802AFE38 | 42,674 | 2,784 | 17,326 | 42,674 |
| `GFSetFog` | 802AFBD4 | 33,123 | 2,812 | 26,877 | 33,123 |

The counters at exit came out at exactly twice the direct runs (for example `GXLoadPosMtxImm=86856/16572`).
Each case's native ran once directly and once from its hook.

## Speed

Times are ns per call: one ordinary case per function, 1,000,000 calls, the best of 5 or 6 rounds,
pinned to the P-cores. The "hooked chunks" columns are what the game sees. Calls enter the chunk at the
function's entry and leave through its return dispatch, natives off and then on. The FP arguments and the
GD list are reset before every call, on both sides.

| Function | Translation via the dispatcher | Native | Hooked chunks, natives off | Hooked chunks, natives on | Speedup through the hooks |
|---|---:|---:|---:|---:|---:|
| `GXLoadPosMtxImm` | 86.2 | 24.1 | 65.1 | 26.1 | 2.49x |
| `GXLoadNrmMtxImm` | 70.1 | 19.8 | 63.4 | 21.1 | 3.00x |
| `GXSetTevColor` | 35.8 | 9.2 | 28.9 | 10.4 | 2.78x |
| `GXSetTevColorS10` | 38.3 | 9.3 | 28.5 | 10.7 | 2.66x |
| `GXSetTevKColor` | 27.4 | 8.8 | 17.6 | 10.2 | 1.73x |
| `GXSetArray` | 43.9 | 11.7 | 28.8 | 13.7 | 2.10x |
| `GXSetTevOrder` | 57.3 | 19.8 | 33.1 | 21.8 | 1.52x |
| `GXCallDisplayList` | 31.8 | 12.9 | 18.1 | 13.9 | 1.30x |
| `GXBegin` | 29.4 | 13.1 | 17.1 | 14.9 | 1.15x |
| `GXLoadTexObjPreLoaded` | 74.2 | 27.9 | 62.2 | 29.6 | 2.10x |
| `GXLoadTexObj` | 126.4 | 53.9 | 93.1 | 56.6 | 1.64x |
| `__GXSetSUTexRegs` | 625.8 | 211.0 | 501.0 | 213.4 | 2.35x |
| `__GXSetVAT` | 71.2 | 18.3 | 62.2 | 20.7 | 3.00x |
| `__GXSetMatrixIndex` | 34.2 | 9.4 | 23.6 | 11.1 | 2.13x |
| `__GXUpdateBPMask` | 71.4 | 20.8 | 60.9 | 22.1 | 2.76x |
| `__GXSetGenMode` | 18.3 | 7.4 | 11.4 | 8.4 | 1.36x |
| `__GXSetVCD` | 99.9 | 18.3 | 80.8 | 19.9 | 4.06x |
| `__GXXfVtxSpecs` | 69.1 | 13.2 | 51.6 | 15.1 | 3.42x |
| `__GXCalculateVLim` | 29.9 | 14.4 | 20.9 | 17.5 | 1.19x |
| `GXSetTexCoordGen2` | 64.7 | 28.4 | 41.9 | 30.2 | 1.39x |
| `GXSetNumTexGens` | 23.6 | 9.4 | 18.3 | 10.7 | 1.71x |
| `GXSetChanAmbColor` | 30.8 | 11.2 | 20.7 | 14.3 | 1.45x |
| `GXSetChanMatColor` | 38.1 | 14.1 | 24.3 | 14.4 | 1.69x |
| `GXSetNumChans` | 25.6 | 9.6 | 20.7 | 10.8 | 1.92x |
| `GXSetChanCtrl` | 34.0 | 11.9 | 19.7 | 14.4 | 1.37x |
| `GXSetCurrentMtx` | 49.4 | 14.1 | 32.4 | 16.5 | 1.96x |
| `GXSetFog` | 160.5 | 74.2 | 147.0 | 74.4 | 1.98x |
| `GXSetFogRangeAdj` | 63.0 | 14.4 | 46.6 | 15.7 | 2.97x |
| `GXInitTexObjLOD` | 94.3 | 49.1 | 67.5 | 51.7 | 1.31x |
| `GXSetIndTexMtx` | 115.2 | 60.6 | 81.9 | 64.7 | 1.27x |
| `J3DGDSetFog` | 182.9 | 165.2 | 209.2 | 161.8 | 1.29x |
| `J3DGDSetTevOrder` | 44.7 | 33.1 | 44.3 | 35.3 | 1.25x |
| `GFSetTevColor` | 28.8 | 8.4 | 29.4 | 9.4 | 3.13x |
| `GFSetTevColorS10` | 29.5 | 8.6 | 29.1 | 9.8 | 2.97x |
| `GFSetFog` | 122.5 | 73.9 | 161.4 | 78.2 | 2.06x |

- **Variation.** The measurements vary by about ±10 percent from run to run. They also depend on the
  case: `__GXUpdateBPMask` measured 1.36x to 2.76x depending on which of its paths the benchmark case takes.
- **Where the gain comes from.** It comes from the pipe stores. In the translation each pipe store is an
  out-of-line call, followed by a deadline check and a block-leader check per block. A native that makes
  several pipe stores gains 2-4x. A native that makes one pipe store and a RAM store gains little.

## Share of the game thread removed (estimate)

From the 2026-10-04 guest-pc profiles behind the brief (`gp5-fh-guest.txt`, Forest Haven `sea:41`, and
`gp4-dri-guest.txt`, Dragon Roost `sea:13`; four E-cores), with the host-module split each line carries. For
each native: the module part of its functions' exclusive share × (1 − 1/speedup), with the lowest hooked
speedup measured for it in any of the hooked benchmark runs.

| Place | Share of the functions covered | Of it in the module | Removed (estimate) | The dropped natives' functions |
|---|---:|---:|---:|---:|
| Forest Haven | 7.61 | 6.24 | **about 2.5** | 2.74 |
| Dragon Roost | 7.25 | 5.92 | **about 2.4** | 3.16 |

The largest gains at Forest Haven are:

| Native | Points removed |
|---|---:|
| `GXLoadPosMtxImm` | 0.35 |
| `__GXSetSUTexRegs` with `__SetSURegs` | 0.26 |
| `GXLoadTexObj` with its callback | 0.24 |
| `GXLoadTexObjPreLoaded` | 0.23 |
| `GXSetArray` | 0.20 |
| `GXSetTevColor`, `__GXSetVAT` | 0.12 each |
| `GFSetTevColor`, `GXSetTexCoordGen2`, `GXSetFogRangeAdj` | about 0.08 each |

At Dragon Roost: `GXLoadTexObj` 0.30, `__GXSetSUTexRegs` 0.21, `GXLoadPosMtxImm` 0.21, `GXSetArray` and
`GXLoadTexObjPreLoaded` 0.16, `__GXXfVtxSpecs` 0.15.

The estimate assumes the natives run on most calls. They decline where `GXBegin` and
`GXCallDisplayList` find dirty state, and where a deadline or the turn's budget falls inside the work.
The `[native-gx] name=runs/declined` line at exit shows the real proportion. The script is
`estimate.py` in the scratch directory.

## Dropped, and why

**32 natives, made and tested, then dropped as no faster.** I generated, tested (60,000 cases each, zero
mismatches) and hooked these, but through the hooked chunks none was 15 percent faster than its
translation in every measurement. Most measured 0.75x to 1.10x:

- `GXSetVtxDesc`, `GXClearVtxDesc`, `GXSetVtxAttrFmt`, `GXSetCullMode`, `GXGetTexObjFmt` (as its own entry;
  it is still replayed inside `GXLoadTexObj`);
- `GXSetTevIndirect`, `GXSetNumIndStages`, `GXSetTevDirect`, `GXSetTevColorIn`, `GXSetTevAlphaIn`,
  `GXSetTevColorOp`, `GXSetTevAlphaOp`, `GXSetTevKColorSel`, `GXSetTevKAlphaSel`, `GXSetTevSwapMode`,
  `GXSetAlphaCompare`, `GXSetNumTevStages`;
- `GXSetBlendMode`, `GXSetColorUpdate`, `GXSetAlphaUpdate`, `GXSetZMode`, `GXSetZCompLoc`, `GXSetDstAlpha`;
- `J3DGDSetTevKColor`, `J3DGDSetTevColorS10`, `J3DGDSetLightColor`, `J3DGDSetLightPos`, `J3DGDSetLightDir`,
  `J3DGDSetLightAttn`, `J3DGDSetTexLookupMode`, `J3DGDSetTexImgPtr`, `J3DGDLoadTexMtxImm`.

These setters store mostly to RAM (`__GXData`'s shadow state, dirty flags, a GD list's bytes and write
pointer) and make one pipe store or none. The translation's inline RAM stores are as fast as the native's,
and the native pays an undo-log entry per store plus its entry and write-back. `native_gx_gen.py` keeps
them in `DROPPED` (not generated). Their functions are 2.7 percent of the game thread at Forest Haven and
3.2 at Dragon Roost.

A faster form would skip the undo entry for stores that no decline can follow, or keep a store's old value
only until the last check that could decline. That needs the generator to prove which declines can still
happen after each store; I left it for a later round.

**Not attempted, or the generator does not support them:**

- **The JPA draw executors** (`JPADrawExecStripeCross` 1.34, `JPADrawExecStripe` 0.87,
  `JPADrawExecRotBillBoard` 0.87 at Forest Haven).
  - `StripeCross` and `Stripe` call through the draw context's function pointers (`ctx->pc = target`)
    into code outside their own.
  - `RotBillBoard` calls `PSMTXMultVec` and `GXBegin` in other chunks; `GXBegin` is a host-watched entry.
  - Each executor runs per particle, and `draw_tags.c`'s Smooth Motion tags depend on the host seeing
    those boundaries.
  - To make these exact, a native must stop at a call into code it does not replay and resume after it.
    The generator has a first form of this (`Native(..., stops=True, resumes=...)`), but it is unused and
    untested, so I did not ship anything that relies on it.
- **`drawWave` (0.87) and `drawVrkumo` (0.55).** `drawWave` calls 38 functions in other chunks: the GX
  setters, matrix code and `GXBegin`. `drawVrkumo`'s budget checks inside its loops take a form the
  generator does not handle. Both would need the stops above.
- **`J3DGDSetFogRangeAdj` and `GDPadCurr32`.** Their loops are extracted by the translator into
  `loop_XXXXXXXX` functions, which the generator does not replay.
- **`GXInitTexObj`.** It calls `memset` (`.init`), which is not replayed.
- **`__GXSendFlushPrim`.** Its entry (0x803231B4) is a host-watched address; `GXBegin` declines where it
  would call it.
- **`J3DGetKeyFrameInterpolation<s>` and `float_kankyo_color_ratio_set`.** These are not FIFO code and are
  outside this round.
- **Word-mode pipe.** With no batch writer (the FIFO trace, `BLUEWAKE_GATHER_PIPE_BATCH=0`), every native
  declines.

**A finding outside this set's code.** `GXCallDisplayList`, `GXBegin` and `GXLoadTexObj` spend about 1.2
points of the game thread at Forest Haven in BlueWake.exe and ntdll (about 0.55, 0.21 and 0.41).
- Why: their entry addresses are literals in `runtime/host/src/main.c`'s GX entry trace (line ~13206,
  `g_gx_entry_trace`, off by default). `direct_calls.py`'s `watched_addresses()` takes every literal in
  the host sources, so each call to these functions goes round the chassis loop: the batch is drained and
  the edge service runs.
- The same applies to 0x80323D50 `GXCopyDisp`, 0x80323EAC `GXCopyTex`, 0x80324F3C `GXInitTlutObj` and
  0x80326BF0 `GXProject`.
- I did not change this; the host and its watch list belong to the main branch.
- Suggested fix: keep the watch list from depending on a trace that is off, for example by moving the
  trace's ranges out of the scanned sources. It would remove most of that 1.2 points, with no native
  needed.

## Hooks, and the two passes

`scripts/windows/native_entries.py` defines the fifth set inside its delimited blocks ("The fifth set ...
end of the fifth set's entries", and "The fifth set: watched GX entries"):
- `GROUPS["gx"]`, which calls `bluewake_native_gx_<entry>(ctx)`;
- 35 `ENTRIES`, each hashing every fragment its native replays, callees included;
- `FIFTH_HOST_OWN`, for the labels the host watches.

The hooks went onto a copy of W-n4's finished `composite-src` (all 206 DOL chunks and `generated.h`).
The full step order ran twice: `native_game_math.py`, `fast_blocks.py`, `lean_memory.py`,
`prepare_native_j3d.py`, `native_entries.py`.

```
pass 1: native game math: 12/12 certified entries, 0 new hooks in 0 chunks
        prepaid block copies: 0 blocks in 0 chunks
        lean memory accesses: 0 in 0 chunks
        native J3D: 2 recovered matrix functions certified in 1 chunks
        native entries: 60/60 certified, 35 new hooks in 5 chunks
pass 2: (the same four lines)
        native entries: 60/60 certified, 0 new hooks in 0 chunks
pass 1 changed only chunks 0171, 0181, 0199, 0200, 0201; pass 2 against pass 1: byte-identical (207 files)
```

The diff pass 1 made is `tests/native_gx_hooks.diff`, in the form of the game-math round's:
- a `#include "native_gx.h"` under the native-entries mark;
- before each entry's first `ctx->pc` store:

```
label_80326F38:
+    if (bluewake_native_gx_enabled && bluewake_native_gx_80326F38(ctx))
+        goto return_dispatch_803256E0;
```

Both passes certify everything:
- game math's 12 entries;
- `prepare_native_j3d.py`'s 2 matrix functions;
- all 60 native entries, including the second set's FIFO hooks, which share chunk 0181 with two of this
  set's.

Each step leaves the others' hooks as they are.

## Integrating

1. **Merge `game-natives-5` into windows-release.** Additions to shared files are self-contained and
   delimited:
   - `cmake/composite/CMakeLists.txt` (`native_gx.c`);
   - `cmake/composite/module_export.c`: the include, plus `bluewake_native_gx_enabled` set from the same
     switch as the other native entries (`BLUEWAKE_NATIVE_ENTRIES=0` turns them off) and the exit report;
   - `scripts/windows/native_entries.py` (the fifth set's blocks);
   - `scripts/windows/build.py` (`training_fingerprint` adds `native_gx_run.h`, `native_gx.c`,
     `native_gx.h`, `native_gx_list.h`, `native_gx_gen.inc`).

   New files: `cmake/composite/native_gx.c`, `native_gx.h`, `native_gx_list.h`, `native_gx_run.h`,
   `native_gx_gen.inc` (generated, committed), `scripts/windows/native_gx_gen.py` (developer tool, not run
   by the builder), and `tests/native5_harness.h`, `native_gx_test.c`, `native_gx_hooked.h`,
   `native_gx_leaders.h`, `native_gx_hooks.diff`.
2. **Build.** The builder's `native-entries` step should log `native entries: 60/60 certified` on both
   passes, with the 35 new hooks on the first and 0 new on the second. If a gx entry fails to certify,
   its translation changed: that native stays unhooked (the translation runs) until it is regenerated
   with `native_gx_gen.py COMPOSITE_SRC`, retested and its hash updated.
   - The tree now being built adds `return_ranges.py` (one line in each return dispatch) and a fast
     `dolrecomp_f32_from_bits`. Neither is inside a certified fragment, and the natives use their own
     conversions (`native_gx_run.h`), so the hashes should hold.
   - Run `native_gx_test` once against the new module to confirm.
3. **What gets recompiled.**
   - The five hooked chunks: `chunk_0171_text1_802AD6E0` (the GF writers), `chunk_0181_text1_802D56E0`
     (J3DGD), `chunk_0199_text1_8031D6E0` (`__GXXfVtxSpecs`), `chunk_0200_text1_803216E0` and
     `chunk_0201_text1_803256E0` (the SDK);
   - `native_gx.c` (new);
   - `module_export.c`.

   The training fingerprint changes, so the builder retrains the speed profile.
4. **In play,** read `[native-gx] name=runs/declined` at exit (Forest Haven, Dragon Roost). With
   `BLUEWAKE_NATIVE_ENTRIES=0` the translation runs everywhere, which gives an A/B check.

## Commits

```
af050f3 module_export.c: the fifth set's comment names the functions it keeps
a869533 tests/native_gx_hooks.diff: the fifth set's hooks, as native_entries.py writes them
c003311 tests/native_gx_test.c: the measured set, its GD list and fog inputs, five hooked chunks
c4057d0 build.py: native_gx_list.h in the training fingerprint
1fb6cce native_entries.py: the fifth set's 35 entries, hooked by their own entry points
717feed Natives, fifth set: the measured 35, each with its own entry point
0363e1e native_gx_run.h: the log's counts kept in the run, the pipe's bytes appended eight at a time
07bf0d5 Build the fifth set's natives into the module, switch and fingerprint them
e5ad8f5 native_entries.py: hook the fifth set's GX natives where certified
a238833 tests/native_gx_test.c: the fifth set against the module's translation
929e2e7 Natives, fifth set: the GX SDK's FIFO writers, replayed from their translation
```
