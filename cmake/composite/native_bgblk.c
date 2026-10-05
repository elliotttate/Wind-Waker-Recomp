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
