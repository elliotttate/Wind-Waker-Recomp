# The third set of natives: the actor search by name

Branch `game-natives-3`, based on `13355b8`. Bit-exact native C versions of the code on the path that a
survey by warp (2026-10-03) found hot at Dragon Roost Island: `fopAcM_searchFromName` walking every actor,
its judge `fopAcM_findObjectCB` calling `dStage_searchName` for the same name at every actor, and that
calling `strcmp` against each entry of the stage's object-name table. Every function was kept. Every test
passes with zero mismatches.

## What was converted

All of it lives in `cmake/composite/native_search.c` (header `native_search.h`).

| Function | Address | How it runs | Shape |
| --- | --- | --- | --- |
| `strcmp` | 0x8032DB44 | hooked at its entry | 27 blocks: the first byte, the byte loop to a word boundary, the word loop (the 0xFEFEFEFF/0x80808080 test), the byte tail |
| `dStage_searchName` | 0x80041544 | hooked at its entry | its frame (inline `_savegpr_29`/`_restgpr_29`), its loop over 825 entries, and every `strcmp` it calls in strcmp's chunk |
| `cTgIt_JudgeFilter` with `fopAcM_findObjectCB` as the judge | 0x80245640 | hooked at its entry | one call to its `blr`: both frames, `fopAcM_findObjectCB`'s calls and tests, `dStage_searchName` and its strcmps |
| the walk: `cNdIt_Judge` iterations whose judge answers NULL | 0x80245640 (a boundary) | `bluewake_native_search_judge`, exported for the host's edge service | whole iterations, `dStage_searchName` run once for the batch, handing the same boundary back nodes later |

Each runs the whole function (or iteration) to its `blr` (or back to the boundary), or declines and
changes nothing, so that the translation runs. When it runs, it leaves the same values as the
translation in all of these:

- every GPR, CR (CR0 from the last compare or record form, with SO), XER's CA, CTR and LR;
- cycles and downcount, charged block by block, and the last access's cycle suffix (the last non-lean
  observing instruction: strcmp's aligning `mtctr` 2, dStage_searchName's and JudgeFilter's `mtlr` 2,
  `cNdIt_Judge`'s call-block `mtctr` 1);
- the reservation, cleared as each frame store clears it;
- every frame word in RAM, and pc.

The header comments state the contracts. In short:

- **strcmp.** It returns what the translation returns: the byte difference, 0, or −1/+1 from the word
  compare. It also leaves r0, r3 to r8, CTR and CA as the last instruction to write each left them.
  - It declines on these: anything pending, aliases over MEM1, a load that is not plain RAM (checked at
    every load), a spent budget, or a deadline inside the work. The deadline test is conservative: a
    deadline before the last cycle, or nearer than 8 (the largest suffix on its paths is 7).
  - A write journal does not matter: strcmp stores nothing.
- **dStage_searchName.** An entry whose first byte differs from the name's costs the translation exactly
  15 cycles: the call block, strcmp's first two blocks, the result test and the loop step. The native
  scans such runs of entries in a tight loop and keeps only what their last strcmp leaves (r5 the byte,
  r4 the name). Only entries that share the first byte run strcmp's emulation. The registers strcmp
  writes only on some paths (r6, r7, r8, CTR, CA) are carried from the last call that wrote them.
  - It also declines on these: a write journal, a frame word outside RAM, a name or table read that lies
    under the frame's stores, or one of its calls into strcmp's chunk not passing silently.
  - "Passing silently" is the natives-2 rule: the edge filter is on, the watch list is loaded, the host is
    quiet, and neither 0x8032DB44 nor 0x80041578 is watched. Then the direct call, and the chassis loop
    it falls back to (at the depth limit, say), ask the host nothing.
- **cTgIt_JudgeFilter.** It is native only when the filter's judge is `fopAcM_findObjectCB`. It reads the
  judge word before anything else and declines on any other judge for a few nanoseconds, since every
  kind of actor search passes through JudgeFilter. It returns the actor or NULL.
  - It leaves r4 as the last test loaded it: dStage_searchName's r4, the entry's procname, or the mask.
  - CR0 comes from the last test. CTR is the judge, unless a strcmp aligned.
  - It also declines on these: a NULL search parameter (which goes to `OSPanic` in the translation), or
    a load under the eleven stack words a call stores.
  - Each of its six calls must pass silently: findObjectCB's entry and return, the call into
    dStage_searchName and its return, and strcmp's entry and return.
- **The walk.** It is `host_actor_search_native`'s idea (runtime/host/src/main.c) applied to the
  `fopAcM_findObjectCB` judge. It is entered at the boundary into JudgeFilter from cNdIt_Judge's `bctrl`,
  where the host's edge service has found nothing to do. It runs whole iterations whose judge answers
  NULL, and leaves the state the translated blocks leave at that boundary nodes later.
  - The name and the table are the same at every node, and nothing the walk stores lies under them. So
    dStage_searchName runs once for the whole batch.
  - It leaves to the translated code the node the judge matches, the list's last node, a node with a load
    outside RAM or under the stores, and any node that would reach the budget or a deadline.
  - It runs only once the certified JudgeFilter hook has run (`bluewake_native_search_judge_ready`). So
    the translation it stands in for is the one its test compared against.

## What was dropped, and why

No function was dropped. Two things are deliberately not done in this branch:

- **The walk is not hooked here.** Its caller must be the host's edge service at 0x80245640, the address
  the host watches for `host_actor_search_native`. Only the host can say its service has nothing to do
  at the boundaries the walk skips. The brief rules out editing `main.c`, so the native is in the module,
  exported and tested; [the host's hook](#the-hosts-call-of-the-walk) shows how to hook it. Without that
  hook the module's three entry hooks already remove about 97 percent of the path's time (below).
- **No direct-call shortcut for strcmp.** `direct_calls.py` could route other chunks' calls to strcmp
  straight to the native (like `VEC_LEAVES`). I did not do this:
  - The hot caller, dStage_searchName, no longer calls strcmp at all.
  - The entry hook already catches every other call.
  - For the common first-byte difference, the hooked chunk costs the same with the native as without
    (7.3 ns both); the gain is in longer compares (2.9–3.6×).

## Tests

All tests are under `tests/`. Each has its exact build and run command at the top. They are built with
the brief's recipe:

- VS clang `-O2 -march=x86-64-v3 -ffp-contract=off`;
- the includes from the main checkout's `ref/recompcore`;
- `E:\Github\Wind-Waker-Recomp\build\windows-exp\app\gxruntime_build\gxruntime.lib`.

They run against the release module, `E:\Github\Wind-Waker-Recomp\build\windows\BlueWake\gGZLE01_recomp.dll`
(SHA-256 `48a97a5d08f5369e0f0f0e8e3e826b3e5fa175e06dbd0b4c5d5b2ff201e750a2`, read-only). That module's
chunks 0009, 0015, 0144 and 0203 are byte-identical to `build\windows-exp\composite-src`'s.

"Identical" means the native ran and every CPU byte and every byte of the test areas matched the
translation run as in play: direct calls and the edge filter on, the host quiet. "Declined" means
nothing changed. The cases include declining inputs on purpose. The randomized inputs cover these:

- strings at every pair of alignments: equal, differing at any byte, prefixes, bytes of 0x80 and up
  (which the word test cannot tell from a zero), runs of 0x01, long strings, and random bytes after
  each terminator;
- tables of 825 random names, from wide and narrow alphabets, including eight-letter names that run into
  the procname bytes;
- names found at any index, absent, prefixes, extensions, empty, the table entry itself, or under the
  frame;
- stacks at any alignment and at the edges of RAM, and reservations on the frame words;
- pointers outside RAM at each load;
- budgets spent before, inside and just after the work, deadlines inside it and before each suffix;
- exceptions pending, aliases over MEM1, write journals;
- the host not quiet (sources dirty, a decrementer or PI interrupt with EE), the edge filter off, and
  each call address watched, in both mirror forms.

The judge tests add these: actor lists of 1 to 40 nodes, actors matching or failing each of findObjectCB's
tests, a state that is not cNdIt_Judge's at its `bctrl` (LR, r29, CTR, r30, pc), the judge
`fpcSch_JudgeByID` instead, and a NULL parameter.

Build (`native_search_judge_test.c` the same way; `native_search_entries_test.c` is linked with the
hooked chunks, as its header says):

```
clang -O2 -march=x86-64-v3 -ffp-contract=off -Icmake/composite
  -IE:\Github\Wind-Waker-Recomp\ref\recompcore\GXRuntime\include
  -IE:\Github\Wind-Waker-Recomp\ref\recompcore\Source\Core\Core\PowerPC\StaticRecomp
  tests/native_search_test.c cmake/composite/native_search.c cmake/composite/direct_calls.c
  E:\Github\Wind-Waker-Recomp\build\windows-exp\app\gxruntime_build\gxruntime.lib -o native_search_test.exe
```

| Test (run) | Function | Cases | Identical | Declined | Mismatches |
| --- | --- | --- | --- | --- | --- |
| `native_search_test MODULE.dll` (60000 per function) | strcmp | 60,000 | 44,301 (14,425 returning 0; 12,588 through the aligning loop, 16,500 through the word loop) | 15,699 | **0** |
| | dStage_searchName | 60,000 | 36,413 (20,411 found) | 23,587 (3,158 with a call in trouble) | **0** |
| `native_search_judge_test MODULE.dll` (60000 each) | cTgIt_JudgeFilter, one call | 60,000 | 40,205 (33,868 with the name in the table, 3,430 returning the actor) | 19,795 (3,158 with a call in trouble) | **0** |
| | the walk | 60,000 | 30,716 runs, 339,647 nodes (4,102 runs of 1 node, 11,168 of 2–7, 10,686 of 8–23, 4,760 of 24 or more; 12,995 stopping at the list's last node) | 29,284 ran no node, unchanged (3,158 with a call in trouble) | **0** |

The walk is compared like this. The translation runs from the boundary with an edge service that has
nothing to do at 0x80245640 until the translation has arrived there as many times as the native ran
nodes. It then ends the run, and the two states are compared. Any other request to the service fails the
case, such as a return out of cNdIt_Judge after a match. The test's cNdIt_Judge frame sends that return
to 0xFFFFFFFC.

End to end, `tests/native_search_entries_test.c` checks the hooks themselves. It compiles the hooked
chunks 0009, 0015, 0144 and 0203 (a copy of the exp tree's, after `native_entries.py`) with the module's
flags: `-fno-slp-vectorize -mllvm -large-interval-freq-threshold=10` and the module's defines. It runs each
function three ways: the module's translation, the hooked chunks with the natives on, and with them off. A
small chassis loop runs between chunks. Every CPU byte and the test areas must match across all three, and
the full images every 256 cases.

Result with `native_search_entries_test MODULE.dll` (20,000 cases per function by default; 0 mismatches):
strcmp, dStage_searchName and cTgIt_JudgeFilter were 20,000 cases each, identical through the hooked chunks
with the natives on and off. These include 775, 789 and 759 cases stopped by the budget inside the work.
With 6,000 cases, one JudgeFilter case also stopped where the module asked its edge service: at 0x80328F40,
the register-save routine, called out of line when the budget is too short for the inline form. The test
stops the hooked run at the same boundary.

## Per-call speedups

Standalone (in the tests; translation through the module's dispatcher, where an empty dispatch costs
7.1–7.7 ns, against the native):

| Call | Translation | Native |
| --- | --- | --- |
| strcmp, first byte differs | 9.0 ns | 3.5 ns |
| strcmp, equal 7-letter names | 34.7 ns | 7.8 ns |
| strcmp, 40 letters, differing at the last | 58.5 ns | 12.0 ns |
| dStage_searchName("ikada_h"), entry 383, 3 earlier entries share its first letter (as in the stage's table) | 4,516 ns | 116 ns (39×) |
| the same, every 16th entry sharing it | 4,911 ns | 177 ns |
| dStage_searchName of a name the table lacks | 9,577 ns | 204 ns |
| cTgIt_JudgeFilter, one node, "ikada_h" | 4,552 ns | 127 ns (36×) |
| cTgIt_JudgeFilter, one node, the name at entry 0 | 81 ns | 28 ns |
| the walk, per node, "ikada_h" | 4,548 ns | 5.1 ns |
| the walk, per node, the name at entry 0 | 93 ns | 3.1 ns |

Through the hooked chunks, natives off and on (`native_search_entries_test`: the chunk's entry and return
dispatch included, the dispatcher not; the closest measure to a call in play):

| Call | Off | On | Speedup |
| --- | --- | --- | --- |
| strcmp, first byte differs | 7.3 ns | 7.3 ns | 0.99× |
| strcmp, equal 7-letter names | 34.4 ns | 11.7 ns | 2.95× |
| strcmp, 40 letters | 58.1 ns | 16.1 ns | 3.61× |
| dStage_searchName("ikada_h") | 4,588 ns | 119 ns | 39× |
| dStage_searchName (absent) | 9,751 ns | 209 ns | 47× |
| cTgIt_JudgeFilter, one node | 4,614 ns | 132 ns | 35× |
| cNdIt_Judge's walk, per node, 64 actors | 4,609 ns | 138 ns | 33× |
| the same, with the walk batched where the host's service would call it | 4,609 ns | 6.8 ns | 683× |

## Expected share of the game thread saved

At Dragon Roost Island (`sea:13`), `fopAcM_searchFromName` is 7.4 percent of the game thread, inclusive.
Almost all of that is the per-actor search. The module's translation walks a node in 4,548 ns when the name
is entry 383, as "ikada_h" is, and in 93 ns when it is entry 0. So about 98 percent of a node is
dStage_searchName's scan and its strcmp calls. Through the hooked chunks a node costs 4,609 ns with the
natives off.

- **With this branch alone** (the three entry hooks), a node costs 138 ns plus the host's edge service
  at JudgeFilter's boundary. That boundary is still crossed once per node, and the test's loop does not
  include its time; a few tens of ns is a fair guess. That is about 3 to 4 percent of the translation's
  time. **Expected saving: about 7.1 percent of the game thread** (7.0–7.2).
- **With the host's call of the walk** as well, a node costs about 7 ns, one service call per walk.
  **Expected saving: about 7.4 percent**, essentially the whole path.
- **At Forest Haven** (`sea:41`), about 3 percent, the saving is about 2.9 and 3.0 percent respectively.

Calls of strcmp and dStage_searchName from elsewhere also speed up, by the per-call figures above: the
event manager, `fopAcM_create` by name, and the other callers of `strcmp`. The survey's exclusive shares
for strcmp and dStage_searchName (6.4 and 2.5 percent) add up to more than the path's 7.4 inclusive. So
some of their time at Dragon Roost comes from other callers, and the saving there may be a little higher.
The module's `[native-search]` lines at exit (below) give the counts that show how often the natives
decline in play.

## The hooks

`scripts/windows/native_entries.py`, the second set's script, now also hooks the third set. It uses the
same machinery, in clearly delimited additions:

- a `search` group;
- three entries with their certified hashes;
- a scoped exemption from the watch check (below).

Each hook is two lines after the entry label, as before:

```c
    if (bluewake_native_search_enabled && bluewake_native_search(ctx, 0x80245640u))
        goto return_dispatch_802416E0;
```

| Entry | Fragments hashed | Certified hash |
| --- | --- | --- |
| 8032DB44 strcmp | strcmp (chunk 8032D6E0) | `0a578813114c0553f81a447d7d0c6ef7d36b22afc5e5f6465ae01517bb5a2375` |
| 80041544 dStage_searchName | dStage_searchName (8003D6E0), strcmp | `bdf91cee67046b81e7b60bb4669129749a912db84cd9fd96028b74a4cac01dd7` |
| 80245640 cTgIt_JudgeFilter | JudgeFilter and cNdIt_Judge's loop 80244F78–80244FB4 (802416E0), fopAcM_findObjectCB (800256E0), dStage_searchName, strcmp | `795c6cb80232a10a8e2ceca570ee310e52f37a4ca4931a70920261c6f75385c2` |

The JudgeFilter entry's hash covers cNdIt_Judge's loop because the walk runs it. The hook arms the walk,
so the walk runs only where that translation is certified.

**The watch check, and its one exemption.** As before, an entry is not hooked if its fragments name an
address the host watches (`direct_calls.watched_addresses()`). The JudgeFilter entry's fragments name
four such addresses, and none is a boundary the hooks run past:

- The host names 0x80245640 (the entry), 0x80244F84 (cNdIt_Judge's `bctrl`) and 0x80244F88 (its return)
  for its own actor search, `BW_SEARCH_JUDGE_FILTER` and `BW_SEARCH_NDIT_RETURN`.
  - The entry is reached only by a dispatch after the host's service, or by a goto inside the chunk.
    cNdIt_Judge's `bctrl` always goes round the loop, because its return is watched; that is also why
    `direct_calls.py` leaves it alone.
  - 0x80244F84 is the middle of a block.
  - The return to 0x80244F88 goes through the chunk's own return dispatch, natively and translated alike.
- 0x80006C4C is `OSPanic`, which the host reports. findObjectCB calls it only for a NULL search
  parameter, where the native declines.

So `SEARCH_HOST_OWN` leaves these out, in those fragments only: the ones starting at 80245640, 80244F78
and 8002833C.

Run on a copy of the exp build's prepared chunks, which already carry the second set's hooks, it printed
`native entries: 12/12 certified, 3 new hooks in 3 chunks`, and `0 new hooks` on a second run. On a copy
of the release tree's chunks, which have no hooks yet, it printed `12/12 certified, 12 new hooks in 8
chunks`. The diff it produced (exp copy, without context):

```diff
chunk_0015_text1_8003D6E0.c
@@ -4,0 +5,2 @@
+/* bluewake: certified native entries (scripts/windows/native_entries.py) */
+#include "native_search.h"
@@ -62421,0 +62424,2 @@
+    if (bluewake_native_search_enabled && bluewake_native_search(ctx, 0x80041544u))
+        goto return_dispatch_8003D6E0;
chunk_0144_text1_802416E0.c
@@ -6,0 +7,2 @@   (marker, #include "native_search.h")
@@ -62222,0 +62225,2 @@
+    if (bluewake_native_search_enabled && bluewake_native_search(ctx, 0x80245640u))
+        goto return_dispatch_802416E0;
chunk_0203_text1_8032D6E0.c
@@ -4,0 +5,2 @@   (marker, #include "native_search.h")
@@ -11132,0 +11135,2 @@
+    if (bluewake_native_search_enabled && bluewake_native_search(ctx, 0x8032DB44u))
+        goto return_dispatch_8032D6E0;
```

Checked on the hooked copy:

- `prepare_simulation_60hz.py`'s two sites in chunk 0144 (0x80244894, 0x802449AC) lie outside every hashed
  fragment.
- No site lies in chunks 0009, 0015 or 0203.
- `prepare_native_math.py` hashes only chunks 803096E0 and 8030D6E0.
- No mod variant replaces any of the four chunks today. A future variant must hash the same, or the
  entry is not hooked.

## Integration

1. **Merge** `game-natives-3` into `windows-release`. The branch has 3,017 insertions and 0 deletions since
   `13355b8`.
   - The shared files take only clearly delimited additions: `CMakeLists.txt` (+2 lines), `module_export.c`
     (+9), `build.py` (+5, in `training_fingerprint` only) and `native_entries.py` (+58).
   - `direct_calls.py` is untouched.
2. **Source step order** (`finish_tree` in `scripts/windows/build.py`): no new step. The third set rides on
   the existing `native-entries` step, `scripts/windows/native_entries.py`. It runs after `native-j3d` and
   before `simulation-prepare` and `native-math`:
   guest-cpu → gpr-inline → chunk-headers → direct-calls → native-skin → native-game-math → fast-blocks →
   lean-memory → native-j3d → **native-entries** → simulation-prepare → native-math.
   - Its hashes include the prepaid copies that `fast_blocks.py` and `lean_memory.py` make, and the
     game-math hook text that `native_game_math.py` put in chunk 0144's copy. So it must stay after those
     steps.
   - Its last line in `native-entries.log` should read `native entries: 12/12 certified, ...`.
3. **Module sources**:
   - `CMakeLists.txt` adds `native_search.c` after `native_mtxcalc.c`.
   - `module_export.c` turns the set on with the other certified entries: `BLUEWAKE_NATIVE_MATH=1`, the
     Windows default; `BLUEWAKE_NATIVE_ENTRIES=0` turns them off.
   - At exit the module prints `[native-search] strcmp=…/… stage-name=…/… judge-filter=…/…` (native/declined;
     JudgeFilter calls with other judges are counted apart). Once the host calls the walk, it also prints
     `[native-search] judge walk=runs/declined (nodes)`.
4. **What recompiles.**
   - The hooked chunks: `chunk_0015_text1_8003D6E0.c`, `chunk_0144_text1_802416E0.c` and
     `chunk_0203_text1_8032D6E0.c` (and their mod variants, should any appear). Chunk 0009 is hashed but not
     hooked.
   - With them, `module_export.c` and the new `native_search.c`.
   - `training_fingerprint` now lists `native_search.c` and `.h`, and already lists `native_entries.py`. So
     the PGO profile is retrained, and with the new profile every chunk compiles again.
5. **After a build**:
   - Check `native-entries.log` for 12/12 certified.
   - Play at Dragon Roost and Forest Haven, and read the `[native-search]` line. stage-name and judge-filter
     should run natively almost always. strcmp's declines include those inside dStage_searchName's
     translation where its own native declined.
   - A step message like `cTgIt_JudgeFilter: not the certified translation` means a translator or
     earlier-step change altered a fragment, and the function is left to the translation. To re-certify:
     rebuild the module from that tree; rerun `native_search_test`, `native_search_judge_test` and
     `native_search_entries_test` against it with zero mismatches; only then replace the hash with what
     `native_entries.py --hashes` prints.

### The host's call of the walk

This is a separate change, in `runtime/host/src/main.c`. It is not made in this branch. With the other
module lookups after the module is loaded (next to `bluewake_composite_guest_mem1`):

```c
static unsigned (*g_module_search_judge)(CPUState*);
...
g_module_search_judge = (unsigned (*)(CPUState*))dlsym(lib, "bluewake_native_search_judge");
```

In `host_chassis_edge_service`, where the host batches its own actor search:

```c
    if (address == BW_SEARCH_JUDGE_FILTER && g_actor_search_native) {
        host_actor_search_native(cpu);         /* the judge fpcSch_JudgeByID */
        if (g_module_search_judge != NULL)
            g_module_search_judge(cpu);        /* the judge fopAcM_findObjectCB */
    }
```

The two are exclusive by the filter's judge word, and each changes nothing otherwise. The module's walk
leaves pc at 0x80245640 like the host's, and the loop then dispatches the next node.
`BLUEWAKE_ACTOR_SEARCH_NATIVE=0` turns both off. So does `BLUEWAKE_NATIVE_ENTRIES=0` for the module's walk,
which also stays off until the module's certified JudgeFilter hook has run once. The host's service makes
the same promise it makes for its own native: it found nothing to do at the boundary, and would find
nothing at the later arrivals the walk skips.

## Notes

- **Host flags between check and crossing.** As in the second set, the natives check once, at entry, that
  every call inside passes silently. A host flag raised in the nanoseconds between that check and the
  moment the translation would have crossed looks the same as the flag arriving a little later.
- **RAM size.** The natives test loads against `cpu->ram_size`. In play that is the module's MEM1,
  32 MiB (`BW_GUEST_MEM1_SIZE`), the same bound the translation's fast loads use. Above the GameCube's
  24 MiB it holds the REL modules' linked data, where a REL actor's name strings may live. The tests give
  the native side 24 MiB, so their reads past it decline.
- **The JudgeFilter hook's cost on other searches.** JudgeFilter runs for every kind of actor search. The
  hook reads the filter's judge word first and declines at once when it is not `fopAcM_findObjectCB`.
- **Other builds.** The hooks come only from this Windows source step. A tree without them never calls the
  natives, and the walk stays unarmed.
