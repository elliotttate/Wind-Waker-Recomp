/* A collision background's blocks' bounds (GZLE01 cBgW::MakeBlckMinMax,
 * cBgW::MakeBlckBnd with MakeBlckTransMinMax and PSVECAdd), native.
 *
 * A moving background (Forest Haven's water wheels and platforms) rebuilds its
 * node tree every frame it moves: each block's box from its triangles'
 * vertices, three MakeBlckMinMax calls a triangle (six compares and stores of
 * the bounds), or the box moved by the background's velocity where its
 * vertices are not transformed. 2.1 percent of the game thread at Forest
 * Haven.
 *
 * Each native is its function's translation, callees included, replayed on
 * local registers (scripts/windows/native_leaf_gen.py wrote
 * native_bgblk_gen.inc from the blocks' prepaid copies; native_leaf_run.h runs
 * it): every instruction's own C, the block leaders' cycles, the FP operations
 * on the translation's inline paths and the interpreter's own functions, RAM
 * stores in place with their old bytes kept. A run leaves what the translation
 * leaves: every register as the last instruction to write it left it (both
 * halves of the FPRs), CR, XER, LR, CTR, the FPSCR, the reservation, the
 * cycles block by block, the last access's cycle suffix, every byte of RAM, pc
 * at the return address.
 *
 * It declines, changing nothing (every byte it stored put back), unless that
 * is certain: no exception pending, no write journal, no alias over MEM1, FP
 * available and rounding to nearest where the code does FP arithmetic, paired
 * singles unscaled where it uses them, every block prepaid and none stopped
 * for the turn's budget or a deadline inside it, every load and store plain
 * RAM, every FP operand on the translation's inline path, and every boundary
 * between chunks on its path passing without the host (the edge filter's own
 * test). The report counts the declines by reason.
 *
 * tests/native_leaf_test.c compares each with the translation, every register
 * and byte, directly and through the hooked chunks. No identifier here may be
 * `ctx`. */
#include "native_bgblk.h"
#include "native_leaf_run.h"

#include <stdio.h>

int bluewake_native_bgblk_enabled;

/* A run's undo log (the game thread's: the natives run only there, and none
 * runs inside another). Static, so a native's frame stays small. */
static GxLog s_lf_log;

/* MakeBlckBnd's room: a block of n triangles (where its vertices are
 * transformed: MakeBlckTransMinMax's short path otherwise) takes at most
 * 74 + 135 n cycles - its own blocks, the inline register save and restore,
 * and per triangle three MakeBlckMinMax calls of at most 36. Where the clock's
 * floor would come first, the native declines before it starts, and the
 * translation, with MakeBlckMinMax's own native at each call, runs the block
 * as before. Anything not plain RAM here is left to the replay, which
 * declines on it itself. */
static inline bool lf_room_80247CD4(const CPUState* cpu, s64 floor) {
    const u32 self = cpu->gpr[3];
    if (!nr_ram(cpu, self + 109u, 1u) || !nr_ram(cpu, self + 148u, 4u) || nr_at(cpu, self + 109u)[0] == 0u)
        return true;
    const u32 bgd = nr_word(cpu, self + 148u), i = cpu->gpr[4];
    if (!nr_ram(cpu, bgd + 8u, 16u))
        return true;
    const u32 blocks = nr_word(cpu, bgd + 20u);
    if (!nr_ram(cpu, blocks + 2u * i, 4u))
        return true;
    const s32 first = (s32)nr_half(cpu, blocks + 2u * i);
    const s32 last = i != nr_word(cpu, bgd + 16u) - 1u ? (s32)nr_half(cpu, blocks + 2u * i + 2u) - 1
                                                         : (s32)nr_word(cpu, bgd + 8u) - 1;
    const s64 count = last >= first ? (s64)last - first + 1 : 0;
    return cpu->downcount - (74 + 135 * count) >= floor;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-label"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#include "native_bgblk_gen.inc"
#pragma clang diagnostic pop

#define LF_NATIVES BGBLK_NATIVES
#define LF_TAG "[native-bgblk]"
#define LF_ENTRY(id) bluewake_native_bgblk_##id
#define LF_REPORT bluewake_native_bgblk_report
#define LF_BY_ADDRESS bluewake_native_bgblk
#include "native_leaf_group.inc"
