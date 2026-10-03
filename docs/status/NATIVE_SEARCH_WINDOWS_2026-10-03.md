# The third set of natives under real windows

Branch `game-natives-3b`, from `windows-release` at `74cb4c3` (the merged third set). In play at Dragon Roost
Island the third set's natives almost never ran: `dStage_searchName` declined about 93,000 times against 804
runs, and the strcmp hook declined 81 percent of its calls. This branch makes them run under the windows the
host actually gives. Every test passes with zero mismatches.

## Why they declined

Two causes, both fixed here:

1. **The name is a REL literal, outside MEM1.** daTag_Island is a REL actor (`ActorRel("d_a_tag_island")` in
   the decomp). It calls `fopAcM_searchFromName("ikada_h", 0, 0)`, and the literal lives in its module's linked
   data. The host keeps REL linked data in guest aliases from 0xC0400000 up (`main.c`, near
   `host_graphics_guest_resolve_uncached`), not in MEM1. The translation reads these through the out-of-line
   path (`bw_mem_read8_slow` → `get_ram_ptr` → the alias registry). The third set accepted only plain MEM1
   loads. So it declined every one of these searches at the name's first byte, whatever the window, and every
   strcmp the translated search then made (one per table entry, about 400 a search). That alone accounts for
   the strcmp hook's 81 percent: a strcmp needs only 6 to 60 cycles, so the window can stop at most the last
   call of each window. (The third set's report wrongly said that REL data lies in MEM1 above 24 MiB.)
2. **The window is shorter than the search.** A search for "ikada_h" takes about 6,000 cycles (6,043 in your
   log). The room a call sees is the rest of its window, which the third set's natives needed whole.

**The host's slicing.** This is read-only from `runtime/host/src/main.c` and `cycle_domain.c`.
- Each dispatch turn starts at `downcount = 0`. `cycle_budget` is the smaller of two values:
  - the cap, 16,384 (`BLUEWAKE_CYCLE_CAP`);
  - the distance to the next device deadline. That is the nearest of the VI event, the DSP step every 12,600
    cycles, the audio DMA chunk and the decrementer, or 1 with an interrupt pending under EE.
- `cycle_deadline_budget` is that distance itself.
- In the 60 Hz simulation mode both are doubled, counted in instruction cycles.
- An observing access (a device register, or the timebase and decrementer SPRs) flushes the cycles and
  recomputes the budget mid-turn.
- By the 2026-09-17 census, the DSP cadence ends 94.5 percent of windows, and the cap never binds.

So windows are at most about 12,600 cycles, often less. A call made at a random point sees anything from 0 to
the window's length. That matches the logged rooms of 80 to 4,800. Half or more of the "ikada_h" searches
cannot fit, and the declines mainly reflect the host's slicing, not a defect in it.

## What changed

All of it is in `cmake/composite/native_search.c` and `.h`, `scripts/windows/native_entries.py` (+10 lines), and
the three tests.

- **dStage_searchName runs as much as the window holds.**
  - It stops only at a strcmp's return (0x80041578, the block after the call). There, pc, every register (r0,
    r3 to r8, r30, r31, LR, CTR, CA, CR0), the frame words, the cycles and the cycle suffix are as the
    translated blocks leave them. That return is in the function's own chunk, and its return dispatch already
    lists the address.
  - Each stretch the native runs is the step, the call block, strcmp, and the result test (15 cycles for an
    entry whose first byte differs). It runs one only if it fits whole:
    - every block's budget check passes;
    - the stretch ends no later than the deadline;
    - the deadline is at least 8 cycles out.

    So the translation would have prepaid every one of those blocks.
  - The rest of the window, at most one stretch, is the translation's. It stops exactly where it would have
    stopped anyway.
  - This is the "equivalent design" the request allows. The state at the window's end is the translation's;
    the end-to-end test checks it after every turn.
- **Three new hooks, with certified hashes.** They resume the search natively from the loop's block leaders:
  the call block 0x8004156C, the strcmp return 0x80041578, and the step 0x80041588.
  - A search the native stopped, or one the translation began (in a window too short even for a stretch),
    goes on natively from the next block the scheduler runs.
  - The hooks hash the same fragments as the entry hook (the whole function and strcmp), so they carry the
    same certified hash, `bdf91cee…dd7`.
  - From a resumption, the native checks that r30 is below 825 and r31 equals the table plus 12 × r30. At the
    end it reads the epilogue's words back from the frame.
  - After a stop, the hook at the return fires once more and declines at once, because the window has less
    left than the shortest stretch (a per-point minimum).
- **Loads out of line, exactly.** strcmp and the search now read anything that is not plain MEM1 the way the
  prepaid copies do: through `get_ram_ptr`, that is the alias registry or the uncached mirror.
  - The cycle suffix of the last such load is left as the copy stores it: 3 or 2 in the byte blocks, 7 or 4
    and then 5 or 4 in the word loop, and 2 for every name byte the scan reads.
  - The hardware (0xC8000000 up) and addresses nothing backs still decline. They are the only loads that
    drain the gather pipe or reach a handler.
  - The extents checked against the frame's stores cover both forms of a MEM1 address.
- **strcmp.**
  - It now handles alias and mirror strings.
  - Its usual call has a first-byte fast path ahead of the rest's frame: both first bytes in plain MEM1 and
    different, 6 cycles, r0, r3, r5 and CR0 written. That makes the hooked call break even for the commonest
    case. Without it, this branch's general strcmp lost 8 percent there.
- **JudgeFilter, and the batched walk.** Each still runs a call or node only whole.
  - A node includes a whole search, so at Dragon Roost (6,000 cycles) a call rarely fits. It then declines,
    and the translation runs JudgeFilter and fopAcM_findObjectCB. Inside them, dStage_searchName's entry hook
    runs the search natively, window by window. Only those two functions' few dozen cycles of blocks stay
    translated.
  - Making JudgeFilter itself resumable would save only those cycles, so I did not.
  - A failed attempt still scanned the table up to the window's end. A one-name hint now records the most room
    a search for that name failed in, and declines at once with no more room. It only ever declines, so a stale
    hint costs time, never exactness. This took the JudgeFilter call at 5,000-cycle windows from 388 to 277 ns
    (MEM1 name).
- **The exit line** gains `resume=native/declined` and two counts: searches stopped for the window, and runs
  that read out of line.

## Tests and results

All tests use the same recipe as before. They are built with VS clang `-O2 -march=x86-64-v3 -ffp-contract=off`
and run against the release module `build\windows\BlueWake\gGZLE01_recomp.dll`, which is read-only. Each test's
header gives its command. A guest alias at 0xC1F00000 is registered in the module (through its exported
`ppc_guest_alias_add_shared`) and in the test, with the same bytes, as a REL module's data is.

- **`native_search_test`** covers strcmp and dStage_searchName from its entry and from each resumption point.
  - Windows are as the host gives them: budgets of 1 to 16,000 with the deadline at the end, or none, or
    beyond.
  - Names and strings are in the alias and through the mirror.
  - Resumption frames are as a search leaves them, and loop registers that are not the loop's own must decline.
  - The comparison has three parts:
    - Where the native ran whole, everything must equal the translation run with the same budget (T).
    - Where it stopped at a strcmp's return, the translation given a budget ending exactly there must equal the
      native's state, except for the budget.
    - The translation run on from the native's state must equal T, including where T stopped.
- **`native_search_judge_test`** is as before, with names also in the alias and through the mirror.
- **`native_search_entries_test`** runs the hooked chunks, compiled with the module's flags, against the
  module's translation, natives on and off.
  - One call at a time, as before, with the alias.
  - Turn by turn: a search, a JudgeFilter call, and cNdIt_Judge's walk over 1 to 24 actors (with the host's
    walk call on). Each turn is a window of 300 to 5,000 cycles ending at a deadline, or none. All three states
    must be equal after every turn, and the RAM and alias at the end.

| Test (default counts) | Function | Cases | Identical | Declined | Mismatches |
| --- | --- | --- | --- | --- | --- |
| `native_search_test` | strcmp | 60,000 | 44,283 (14,249 returning 0; 18,083 reading out of line) | 15,717 | **0** |
| | dStage_searchName, entry | 60,000 | 42,862: 27,504 whole (17,636 found), 15,358 stopped for the window (T itself stopped in 14,778); 11,653 reading the name out of line | 17,138 | **0** |
| | from 8004156C | 60,000 | 46,854: 38,739 whole, 8,115 stopped | 13,146 | **0** |
| | from 80041578 | 60,000 | 47,184: 41,122 whole, 6,062 stopped | 12,816 | **0** |
| | from 80041588 | 60,000 | 46,934: 38,972 whole, 7,962 stopped | 13,066 | **0** |
| `native_search_judge_test` | JudgeFilter, one call | 60,000 | 40,205 | 19,795 | **0** |
| | the walk | 60,000 | 30,616 runs, 338,017 nodes | 29,384 | **0** |
| `native_search_entries_test` | strcmp, searchName, JudgeFilter, one call | 20,000 each | all; 754, 8,168 and 8,225 stopped by the budget inside the work | n/a | **0** |
| | turns: search / JudgeFilter / walk | 3,000 each | all, after each of 11,382 / 11,484 / 96,813 turns | n/a | **0** |

## Speed

Through the hooked chunks, natives off and on (`native_search_entries_test`; the closest measure to play):

| Call | Off | On | Speedup |
| --- | --- | --- | --- |
| strcmp, first byte differs | 7.3 ns | 6.9 ns | 1.06× |
| strcmp, equal 7-letter names | 34.3 ns | 13.8 ns | 2.5× |
| strcmp, 40 letters | 58.6 ns | 19.1 ns | 3.1× |

In fixed windows of W cycles, "ikada_h" at entry 383 with the stage's table shape, and the name in the alias as
in play (ns per search, call or node; MEM1 names are 20–30 percent faster on both sides):

| W | dStage_searchName | JudgeFilter call | walk, per node (batched) |
| --- | --- | --- | --- |
| 300 (20 turns) | 6,242 → 1,315 (4.7×) | 6,237 → 1,233 (5.1×) | 6,191 → 1,430 (4.3×; 4.3×) |
| 1,000 (6) | 5,909 → 497 (11.9×) | 6,006 → 597 (10.1×) | 6,034 → 688 (8.8×; 8.6×) |
| 2,000 (3) | 5,854 → 276 (21.2×) | 5,938 → 404 (14.7×) | 5,979 → 521 (11.5×; 11.2×) |
| 5,000 (2) | 5,874 → 277 (21.2×) | 5,910 → 360 (16.4×) | 5,924 → 371 (16.0×; 15.6×) |
| 16,384 (1) | 5,820 → 202 (28.9×) | 5,922 → 219 (27.0×) | 5,892 → 271 (21.7×; 27.8×) |

Each extra window costs the native about 40–60 ns: the turn's entry into the chunk, the resumption's checks, and
the hook's immediate decline after a stop. That is the whole difference from the one-window time. Standalone,
without the chunks, a whole search is 135 ns (MEM1 name) or 193 ns (alias), against 4.5 and 5.4 µs.

## Expected effect under 300–5,000-cycle windows

At Dragon Roost the path is `fopAcM_searchFromName` over every actor. Its share of the game thread was 7.4
percent inclusive by the 2026-10-03 survey, and dStage_searchName alone was 9 percent inclusive in your
measurement. Nearly all of it is daTag_Island's search for an alias name, which until now ran entirely
translated. With this branch, each search runs natively across the windows it spans, with only JudgeFilter's
and findObjectCB's blocks and the partial last stretch of each window translated.

- In windows of 1,000 to 5,000 cycles (the logged rooms), the search costs 12–21× less. The JudgeFilter call
  and the walk per node, which is what the game actually runs, cost 9–16× less. **That removes about 89–94
  percent of the path: roughly 6.5 to 8.5 percent of the game thread**, from about nothing today.
- In the worst case, 300-cycle windows throughout, it is 4.3–5×, about 77–80 percent of the path.
- Calls of strcmp from elsewhere: a first-byte difference breaks even, and longer compares are 2.5–3× faster.
- **The host's walk call** (the third set's report shows how to add it) adds little at Dragon Roost. A node there
  rarely fits a window, and the walk then declines cheaply through the hint. It pays off only for short
  searches, such as names near the table's start or windows of 16,384.

These are test-loop figures. In play the windows' own host work is the same with the natives off and on, and
the first build with this branch should confirm them with the `[native-search]` line. Expect these readings at
Dragon Roost:
- stage-name and resume mostly native;
- "searches stopped for the window" about once per window a search crosses;
- "runs read out of line" about one per daTag_Island search;
- judge-filter mostly declined. That is expected: the search runs natively inside the translated call.

## Integration

1. **Merge** `game-natives-3b` into `windows-release`. The changes are `native_search.c`/`.h`, `native_entries.py`
   (+10 lines, in its own delimited block after the third set's entries), the three tests, and this report.
   `CMakeLists.txt`, `module_export.c`, `build.py` and `direct_calls.py` are unchanged.
2. **No new step.** The `native-entries` step (`scripts/windows/native_entries.py`, same place in `finish_tree`)
   now certifies 15 entries. `native-entries.log` should end with `native entries: 15/15 certified, ...`.
   - On a copy of the release tree with the merged third set's hooks it printed `15/15 certified, 3 new hooks
     in 1 chunks`, then `0 new hooks` on a rerun.
   - The result was byte-identical to the chunks the end-to-end test compiled.
   - Each new hook is the usual two lines after the label:
     ```c
         if (bluewake_native_search_enabled && bluewake_native_search(ctx, 0x80041578u))
             goto return_dispatch_8003D6E0;
     ```
3. **What recompiles.**
   - `chunk_0015_text1_8003D6E0.c` (hooks at 8004156C, 80041578 and 80041588) and `native_search.c`.
   - `training_fingerprint` already lists `native_search.c`/`.h` and `native_entries.py`. So the profile is
     retrained, and every chunk recompiles with it.
4. **After a build**:
   - check the 15/15 line;
   - play at Dragon Roost and read the `[native-search]` line, as above;
   - `BLUEWAKE_NATIVE_ENTRIES=0` still turns all of it off.
   - If a fragment's translation changes, all four dStage_searchName hooks fall back together, because they
     share one hash. To re-certify, follow the same steps as in the third set's report.

## Notes

- **When the native declines persistently**, the translated loop calls a hook at each of its three block
  leaders per entry (a few ns each). That happens when the host is not quiet, or a call boundary is watched.
  In play those states are transient: the host's service clears them at the next boundary, after which the
  native takes over. Near each window's end it is at most one stretch.
- **The judge hint** is module-global state, touched only on the game thread (the hook, and the host's call
  of the walk). It changes only which calls decline. The judge test's identical counts moved by 0.1 percent
  with it.
- **Per-call costs of the general path.** A strcmp whose strings are in the alias gains little on its own:
  41 ns → 34 ns standalone, since `get_ram_ptr` dominates. Such strcmps are now rare, because the search that
  made them runs natively.
