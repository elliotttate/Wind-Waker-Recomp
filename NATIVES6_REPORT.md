# Natives, sixth set: the particle draw code and the sea's waves, with stops

Branch `game-natives-6` (from windows-release 56dcd70), 2026-10-04. Generated, certified and tested
against W-n5 (`build\windows\W-n5\gGZLE01_recomp.dll`, windows-release a8e179f), using my own snapshot of
its composite-src taken before the experimental `straight_copies.py` build started. The hooked tests link
that snapshot's chunks.

## Summary

- **Stops and resumes.** The generator (`native_gx_gen.py`, `stops=True`) can now stop a native just
  before a call it does not replay, instead of declining. The stop leaves exactly what the translation
  has at that point: registers, cycles with the block prepaid, RAM and pipe bytes committed, and pc at the
  call. The hook then hands over to the translation at that instruction through the chunk's pc table. A
  hook at the call's return address (a resume) picks up the rest natively. Loop heads take checkpoints, so
  long strips never run out of undo log. The fifth set's generated output is byte-identical with the new
  generator.
- **23 natives, 122 hooked entries** (23 entries and 99 resumes):
  - the JPA particle draw visitors: 4 billboards, 4 directionals, 2 rotations, point, line;
  - both stripes;
  - three colour-register visitors;
  - the four calc visitors;
  - `JPADraw::calcParticle`, with every calc visitor;
  - `drawWave`'s loop over the waves.
- **Exactness: zero mismatches.**
  - Each native ran 60,000 random cases (72,000 for `CalcScaleXBySpeed`). Between 32,851 and 47,052 cases
    per native matched the translation end to end through the hooked chunks with a native running. Of
    those, 802 to 19,996 stopped and resumed at least once.
  - Each comparison covered every register, flag, cycle count, RAM byte and pipe byte, with all sets'
    natives on and again with them off.
  - The test now writes a tag into the pipe at every boundary the module's chassis asks the host about.
    It does this in the reference and in the hooked runs, the way the particle draw tags do. So the stream
    comparison also checks where each native's bytes fall relative to the host's tags. Between 512 and
    1,283 compared cases per native carried such tags.
- **Speed through the hooked chunks, with the fifth set on in both runs:** 1.05x to 1.73x per call.
  Highlights: `StripeCross` 1.46x, `Stripe` 1.34x, `RotBillBoard` 1.57x, `calcParticle` 1.30x, the calc
  visitors 1.40x to 1.45x, `drawWave` 1.26x. My rough estimate is **about 1.3 points of the game thread**
  at Forest Haven, from the listed functions only.
- **Dropped: five colour-register visitors.** They are exact, but they are not faster than their
  translation now that the fifth set's `GXSetTevColor` native runs inside it (0.92x to 1.05x).
  `JPADrawExecRegisterPrmAlphaAnm`, which was on the target list, is one of them.
- **Not done:**
  - `drawVrkumo`. Its cloud loop calls `mDoLib_project`, which reads the view matrices at absolute
    addresses, plus `atan2`, `fmod` (through `cM_rad2s`), `sin`, `cos` and `PSVECAdd`. It would need its
    own test layout for an estimated saving of about 0.15 point.
  - `JPADrawExecCallBack::exec`. Its time is the host's tag work plus the game's callback.
  - `JPADraw::drawParticle`. Every visitor call it makes is a watched draw-tag boundary, so it would stop
    once per particle.
- **Separate commits taken earlier in the round:**
  - `eed80b5` fixes a real exactness bug in `psq_st`, which the fifth set's `GXLoadPosMtxImm` and
    `GXLoadNrmMtxImm` also use; details below.
  - `f4f99b1` recertifies `GXCallDisplayList` against the watch-list fix.
- **Two passes:** on a copy of W-n5's DOL chunks the full step order gives native entries 182/182 with
  123 new hooks, then 0 new hooks. The second pass is byte-identical over all 207 files.

## Stops, resumes and checkpoints

The fifth set's natives decline at any call they don't replay. A particle draw calls its shape's vtable
getters, the draw clipboard's function pointers, the matrix leaves, `GXBegin` and `GXSetTevColor`. On
real data there is usually something it can't replay. The sixth set's natives stop there instead.

- **A stop** happens at a call (direct or through a register) in the hook's own chunk whose target the
  native doesn't replay or whose boundary the host would be asked about. It also happens when the turn's
  budget is spent there.
  - The native commits everything as the translation's prepaid copy has it just before that
    instruction. That includes the block's cycles, the suffix, RAM, and the pipe bytes appended to the
    batch.
  - It sets pc to the call and returns 2. The hook sets `cycle_block_prepaid` and jumps through
    `pc_table_CHUNK` to that instruction. The translation then makes the call exactly as before, edge
    service included.
  - If the call is its block's first instruction, the native hands the block's cycles back and stops
    before the block is entered.
  - A branch to a block of the hook's chunk that the native doesn't replay also stops, at that block's
    leader.
  - Outside the hook's chunk (in a callee's chunk), the native still declines, because it has no pc table
    to go on with there.
- **A resume** is a hook at the return address of each stop in the native's own function. When the call
  returns into the translation, whether by its return dispatch or by falling through after a direct
  call, the resume runs the rest natively. Resumes are entries of their own in `native_entries.py`, with
  the same fragments and hash as their native.
  - The generator makes two passes: the second adds the resumes the first one found. That is why the
    builder's native-entries step reports them as extra entries.
- **A checkpoint** is taken at a loop head in the hook's chunk. If the undo log is past 128 stores or the
  pipe bytes are past 512, the native commits as a stop there would and goes on with both empty. A later
  decline becomes a stop at that head (`gx_decline` returns 2).
- **Host boundaries.** At every boundary where the translation would go back to the chassis (a call,
  return or branch into another chunk, or an indirect target outside the chunk's dispatch switch), the
  native asks the edge filter's own question at run time (`gx_silent`). Where the host would be asked,
  it stops or declines, so the host's edge service, including the particle draw tags, sees the same pipe
  bytes before it as with the translation. In-chunk transfers ask nothing in either form. For this reason
  `native_entries.py` leaves the static watched-address check out for the draw group. A watched entry,
  such as a tagged particle draw, is reached through the chassis, so its tag is written first. The test's
  tags check this end to end.
- **New arithmetic in `native_gx_run.h`.** `ps_sum0`, `ps_sum1`, paired moves and `fmadd`/`fmsub`/
  `fnmadd`/`fnmsub` (single and double) now follow `inline_fp.h`'s inline paths. Anything off those paths
  declines.

### The natives

| Native | Blocks | Resumes | Replayed with it |
|---|---:|---:|---|
| `JPADrawExec{,Rot}{,Y}BillBoard`, `Rot{,Directional}Cross`, `RotDirectional`, `DirBillBoard`, `Rotation`, `Point`, `Line` (12) | 21-67 | 1-6 | vtable getters of `JPABaseShapeArc`/`JPAExtraShapeArc`, the clipboard's direction, rotation and plane functions, `PSMTXMultVec{,Array,SR}`, `PSMTXConcat`, `GXBegin`'s own blocks, `GXSetTevColor` |
| `JPADrawExecStripe`, `StripeCross` | 66, 90 | 6, 8 | getters, clipboard functions, `stripeGetNext`/`Prev`, `GXBegin`'s own blocks |
| `JPADrawExecRegisterPrmCEnv`, `PrmAEnv`, `ColorEmitterPE` | 4 each | 2 each | getters, `GXSetTevColor` |
| `JPADrawCalcScaleX`, `ScaleY`, `ScaleXBySpeed`, `Alpha` | 22-25 | 6-8 | getters |
| `JPADraw::calcParticle` | 197 | 3 | every calc visitor (28), getters |
| `drawWave` (loop head 8009A094) | 118 | 13 | `sin`, `__kernel_sin`, `__kernel_cos`, `__ieee754_rem_pio2`, `GXLoadTexObj` with `GXLoadTexObjPreLoaded` and the region callbacks, `GXSetTevKColor`, `PSMTXMultVec`, `__register_global_object` |

- **`GXBegin`.** Only its own blocks are replayed. Where its dirty state needs calls (a draw's first after
  state changes), the native declines and the translation makes them, with the fifth set's natives at
  their hooks.
- **`drawWave` starts at its loop's head**, not its entry. It stops before every quad's `GXBegin`, because
  the waves load a texture before each quad, so the dirty state always needs calls. It resumes after
  `GXBegin` with the quad's four vertices and the next wave.
- **`__kernel_rem_pio2`** (arguments past 2^20 pi/2) is not replayed and declines.

## Tests

`tests/native_draw_test.c` builds as its header describes, linking a hooked copy of 11 chunks of W-n5's
composite-src (0038, 0149, 0151-0153, 0194, 0195, 0199-0201, 0203).

**What each random case covers:**
- registers, flags, FPSCR, reservation, and cycle state (deadlines and budgets near and inside the work);
- 256 heap objects whose first word is a vtable, with slots holding the classes' functions mostly and
  sometimes other code or an object;
- the clipboard's function pointers;
- `__GXData` and r2's and r13's small data;
- MEM1's first page, which is new: a store through a small pointer lands there in the module's
  translation. A store at address 3 made the reference fault before it was a compared region;
- write journal, aliases, a pending exception;
- the host quiet, busy, with the filter off, or with the natives' boundaries watched;
- the pipe batched (the flush point inside the run or not), word by word, or unset.

**drawWave's cases** start at the loop head. They lay out:
- the waves, the scales and count, the statics and their guard, and the frame (matrix, texture object,
  saved registers, return address);
- fdlibm's constants where `sin`'s code loads them, mostly.

**Plain run:** the native alone. Where it runs to the blr, everything is compared with the module's
translation. Where it declines, nothing may change.

**Hooked runs:** every case goes through the hooked chunks from the entry, natives on and then off, each
compared byte for byte with the translation.
- Tags go into the pipe at the boundaries the module's chassis asks about. The hooked runs mirror the
  chassis: the module's own watch list, and the first-dispatch rule.
- A tag repeated at the same boundary with nothing between is counted once. The chassis's zero-charge
  retries make those repeats.

Results (W-n5, the final test binary):

| Native | Cases | Plain: ran to blr | Stopped | Declined | End to end with a native | of them with stops | with host tags |
|---|---:|---:|---:|---:|---:|---:|---:|
| RotBillBoard | 60,000 | 26,834 | 3,079 | 30,087 | 42,232 | 3,480 | 988 |
| BillBoard | 60,000 | 28,867 | 3,401 | 27,732 | 42,519 | 3,645 | 991 |
| YBillBoard | 60,000 | 28,830 | 3,005 | 28,165 | 42,023 | 3,266 | 1,010 |
| RotYBillBoard | 60,000 | 26,730 | 2,990 | 30,280 | 41,932 | 3,405 | 971 |
| Point | 60,000 | 31,870 | 2,441 | 25,689 | 44,421 | 2,502 | 1,029 |
| Line | 60,000 | 33,629 | 779 | 25,592 | 43,902 | 802 | 512 |
| drawWave | 60,000 | 15,913 | 15,653 | 28,434 | 44,629 | 19,996 | 1,283 |
| RotDirectional | 60,000 | 24,465 | 6,568 | 28,967 | 39,182 | 7,377 | 944 |
| DirectionalCross | 60,000 | 28,206 | 5,044 | 26,750 | 39,560 | 5,147 | 931 |
| RotDirectionalCross | 60,000 | 25,172 | 5,534 | 29,294 | 39,128 | 6,139 | 943 |
| DirBillBoard | 60,000 | 27,579 | 4,335 | 28,086 | 39,499 | 4,491 | 939 |
| Rotation | 60,000 | 25,322 | 5,067 | 29,611 | 40,946 | 5,564 | 945 |
| RotationCross | 60,000 | 25,797 | 4,042 | 30,161 | 40,505 | 4,334 | 932 |
| Stripe | 60,000 | 14,322 | 7,576 | 38,102 | 40,778 | 6,715 | 736 |
| StripeCross | 60,000 | 14,012 | 7,732 | 38,256 | 40,724 | 6,924 | 721 |
| RegisterPrmCEnv | 60,000 | 31,668 | 5,181 | 23,151 | 46,837 | 6,021 | 1,212 |
| RegisterPrmAEnv | 60,000 | 30,325 | 4,830 | 24,845 | 46,661 | 5,825 | 1,205 |
| RegisterColorEmitterPE | 60,000 | 34,825 | 5,616 | 19,559 | 47,052 | 6,166 | 1,213 |
| CalcScaleX | 60,000 | 26,864 | 12,302 | 20,834 | 35,311 | 4,835 | 960 |
| CalcScaleY | 60,000 | 26,850 | 12,326 | 20,824 | 35,211 | 4,756 | 973 |
| CalcAlpha | 60,000 | 27,161 | 13,920 | 18,919 | 35,152 | 4,987 | 932 |
| calcParticle | 60,000 | 15,176 | 23,488 | 21,336 | 37,677 | 19,536 | 805 |
| CalcScaleXBySpeed | 72,000 | 23,563 | 14,949 | 33,488 | 32,851 | 6,010 | 1,141 |

**Every native: 0 mismatches.** The rest of the 60,000 cases either left the linked chunks or had no native
run. Runs leave the linked chunks where a random vtable slot or callback sends the translation into an
object or address 0; the test does not follow those.

Before they were dropped, the five colour-register visitors also passed 60,000 cases each with 0
mismatches, at 46,981 to 47,261 end to end.

Bugs the tests found on the way:
- `psq_st` stored a denormal half unflushed when NI is set (`eed80b5`, committed separately). Clang
  turned `convert_to_single_ftz`'s zero test into a floating-point compare on bits it could see came from
  a double, and the host's denormals-are-zero mode answered it differently. The fifth set's
  `GXLoadPosMtxImm` and `GXLoadNrmMtxImm` take the same path, so this fix belongs in windows-release
  whether or not the sixth set goes in.
- Everything else was in the test: stray float words used as pointers, non-code callback targets, the
  low page, the deadline and zero-charge repeats.

The fifth set, rechecked on W-n5 (with `f4f99b1`):
- `native_gx_test` passes all 35 natives at 60,000 cases with 0 mismatches, and each is identical through
  W-n5's hooked chunks.
- `native_gx_gen.py` (with this round's changes) writes `native_gx_gen.inc`, `native_gx_list.h` and
  `native_gx_leaders.h` byte-identical from W-n5's tree.

## Speed

How the benchmark is set up:
- ns per call through the hooked chunks, with the sixth set off and then on. **The fifth set is on in both
  runs**, as the module has it without this set; round 5's benchmark toggled both sets.
- Each native uses eight of 400 cases with ordinary singles where the native runs and the run reaches the
  return. Cases where the native runs to the blr come first, then the most work. drawWave is the
  exception: it uses its heaviest cases, which all stop before each quad's `GXBegin`.
- Each case is timed alone (best of three rounds). The table averages three benchmark runs.

The experimental build was using most cores during these runs. The off/on rounds are interleaved, so the
ratios hold, but absolute times are noisy.

| Native | Off ns | On ns | Speedup (3 runs) |
|---|---:|---:|---|
| BillBoard | 379.8 | 219.2 | 1.73x (1.73 1.73 1.74) |
| RotBillBoard | 485.8 | 310.0 | 1.57x (1.56 1.56 1.58) |
| Line | 285.3 | 181.3 | 1.57x |
| Point | 81.0 | 52.0 | 1.56x |
| StripeCross | 10,089 | 6,920 | 1.46x |
| RotationCross | 1,238 | 852.5 | 1.45x |
| CalcScaleX | 78.0 | 53.9 | 1.45x |
| CalcScaleXBySpeed | 204.0 | 141.4 | 1.44x |
| CalcScaleY | 79.2 | 56.2 | 1.41x |
| CalcAlpha | 91.5 | 65.2 | 1.40x |
| Stripe | 6,003 | 4,470 | 1.34x |
| DirectionalCross | 1,700 | 1,288 | 1.32x |
| DirBillBoard | 968.7 | 742.2 | 1.31x |
| calcParticle | 575.1 | 440.7 | 1.30x |
| RotDirectionalCross | 1,885 | 1,453 | 1.30x |
| drawWave (stopping at every GXBegin) | 33,145 | 26,388 | 1.26x |
| RotDirectional | 1,585 | 1,304 | 1.22x |
| Rotation | 754.2 | 644.2 | 1.17x |
| YBillBoard | 671.4 | 599.2 | 1.12x |
| RotYBillBoard | 761.5 | 701.6 | 1.09x |
| RegisterColorEmitterPE | 45.2 | 41.8 | 1.08x |
| RegisterPrmCEnv | 61.6 | 57.5 | 1.07x |
| RegisterPrmAEnv | 62.8 | 59.8 | 1.05x |

- **Choosing cases where the native runs to the blr** matters for the stripes and `calcParticle`. When
  the benchmark took the heaviest cases whatever they did, its synthetic strips read stray pointers
  (small or out-of-RAM addresses from the random heap) partway through. Those reads make the native
  decline, and the following resumes try and decline again (see below). `StripeCross` then measured only
  1.01x, `Stripe` 1.16x and `calcParticle` 1.07x to 1.22x. The other natives measured the same either way.
  I don't know how often the game's own strips stop; real particle lists don't hold stray pointers.
- **A cost to know about.** A resume hook sits at its label, so the translation also passes it when it
  falls through after a direct call. If a native declines partway through a function, every later resume
  in that run tries again and, for the same reason, usually declines again. That is wasted work only;
  results stay exact. It is what made the stray-pointer cases slow.

## Share of the game thread removed (estimate)

This uses the profile shares from the brief (Forest Haven, self time) and the speedups above, as
share x (1 - 1/speedup):
- StripeCross 1.34 -> 0.42
- Stripe 0.87 -> 0.22
- RotBillBoard 0.87 -> 0.32
- CalcScaleX 0.32 -> 0.10
- calcParticle 0.27 -> 0.06
- drawWave 0.87 -> 0.18

That is **about 1.3 points**. The natives without a listed share (the other billboards, directionals,
rotations, the calc visitors other than ScaleX) add a little more.

This is a microbenchmark estimate. The self-time shares leave out callees the natives also replay, such
as `sin`, `GXLoadTexObj` and the matrix leaves. The cases are synthetic. A profile of the built module
should replace it.

## Dropped, and why

- **`JPADrawExecRegisterPrmColorAnm`, `PrmAlphaAnm`, `EnvColorAnm`, `ColorEmitterP`, `ColorEmitterE`.**
  They are exact, but over five benchmark runs they measured 0.92x to 1.05x. Each is one `GXSetTevColor`
  plus a few loads, and the fifth set's native already runs that call. They stay listed in
  `native_draw_gen.py` (`NOT_FASTER`) but are not built. `PrmAlphaAnm` (0.26 in the brief) is one of them.
- **`drawVrkumo` (0.55).** Its cloud loop (8009B04C to 8009B8E8) calls:
  - `mDoLib_project`, which reads the projection through absolute addresses (`lis r3, 0x803C`);
  - `atan2` twice;
  - `cM_rad2s` (through `fmod`, a bit loop);
  - 15 `sin`/`cos`, four `PSVECAdd` and `cXyz::operator+`;
  - `GXLoadTexObj`, `GXSetTevColor` and `GXBegin`.

  It also keeps a dozen registers and r13 statics live across the loop. The generator could take it with
  stops, but testing it needs another case layout, including a region at the view matrices' addresses.
  For an estimated 0.15 point I left it.
- **`JPADrawExecCallBack::exec` (0.36).** In the profile most of its samples are in BlueWake: the host's
  tag work at this watched entry. The rest is the game's own callback, called through the emitter.
- **`JPADraw::drawParticle` (0.34).** Its loop calls a visitor through a vtable for every particle, and
  every visitor entry is a watched draw-tag boundary. A native would stop and resume once per particle per
  visitor, which costs more than its loop saves. The visitors themselves are native now.
- **`GXBegin` inside `drawWave`.** Each wave's `GXSetTevKColor` writes a BP register, so `GXBegin` would
  need `__GXSendFlushPrim`, whose loop the translator extracts and the generator doesn't replay. Replaying
  `GXBegin` would make the native decline every wave, so it stops there instead.

## Hooks, and the two passes

- `native_entries.py` certifies all 122 draw entries against W-n5's composite-src (my snapshot): 182/182
  with the earlier sets.
- `tests/native_draw_hooks.diff` shows the hooks as the step writes them: chunks 0038, 0151, 0152 and
  0153, 122 hooks and 4 includes.
- The full step order (native game math, fast blocks, lean memory, native J3D, native entries), run twice
  over a copy of W-n5's DOL chunks:
  - native game math 12/12, native J3D 2, native entries 182/182 with 123 new hooks (122 draw plus
    `GXCallDisplayList`), then 0;
  - the second pass is byte-identical over all 207 files.
- Note for the `straight_copies.py` experiment: the draw entries' fragments span chunks 0038, 0149,
  0151-0153, 0194, 0195, 0199-0201 and 0203. If that step reads `native_entries.py`'s `ENTRIES` to decide
  which copies to leave alone, the new entries are covered. After a build with both, the native-entries
  line should still read 182/182.

## The separate commit (watched addresses, `GXCallDisplayList`)

As asked mid-round, these are their own commits.
- **`f4f99b1`** recertifies `GXCallDisplayList` against W-n5. With the GX entry trace no longer watched,
  its call to `__GXSetDirtyState` became a direct call, which changed only its hash. The generated native
  is identical. W-n5's builder logged 59/60; with this commit it is 60/60.
- **The formerly watched addresses** in the fifth set:
  - `FIFTH_HOST_OWN` is now inert, and its comment says so.
  - The declines around `__GXSendFlushPrim` stay. Its loop is extracted, and the generator doesn't replay
    it.
  - `GXCallDisplayList`'s and `GXBegin`'s dirty paths are still left to the translation. Now that the
    callees are direct calls, a later round could replay `GXCallDisplayList`'s dirty path.
- **`eed80b5`** is the `psq_st` fix above.

## Integrating

1. Merge `game-natives-6`. It is based on windows-release 56dcd70, and `f4f99b1` and `eed80b5` are its
   first two commits.
2. Rebuild. `native_draw.c` is in the module's sources: about 20 s to compile, a 1.7 MB object.
   - `module_export.c` turns the set on with the same switch as the others and prints `[native-draw]`
     counts at exit (done/stopped/declined per entry and resume).
   - `build.py`'s training fingerprint now includes `native_draw.c`, `native_draw.h`,
     `native_draw_list.h` and `native_draw_gen.inc`, so the PGO training reruns.
3. Expect the builder's native-entries step to log **182/182 certified on both passes**. If the
   composite-src differs from W-n5's in any of the chunks listed above, the affected entries log "not the
   certified translation" and stay unhooked. To fix that, regenerate and recertify:
   - `python scripts/windows/native_draw_gen.py <composite-src>`;
   - `native_entries.py --hashes` for the new hashes;
   - then rerun `tests/native_draw_test.c` against the new module.
4. A change worth checking in play is Smooth Motion around particles, since the tags must be unchanged.
   The test's tag comparison covers this. A Smooth Motion dump compare at a particle-heavy place, such as
   Forest Haven or Dragon Roost, would confirm it on real frames.

## Commits

- `eed80b5` native_gx_run.h: psq_st takes the halves' bits opaque
- `f4f99b1` native_entries.py: GXCallDisplayList certified against the translation with the watch-list fix
- `db4f314` native_gx_gen.py: stops before the calls a native does not make, resumes after them,
  checkpoints at loop heads
- `dfd3a1a` Natives, sixth set: the particle draw code and the sea's waves, with stops
- `b77e033` native_entries.py: the sixth set's 122 entries, hooked with their stops
- `a9c4117` Build the sixth set's natives into the module, switch and fingerprint them
- `a243564` module_export.c, CMakeLists.txt: the sixth set's comments name drawWave too
- `3946338` tests/native_draw_test.c: the sixth set against the module's translation, end to end through
  the hooked chunks
- `6ca1d59` tests/native_draw_hooks.diff: the sixth set's hooks, as native_entries.py writes them
- this report
