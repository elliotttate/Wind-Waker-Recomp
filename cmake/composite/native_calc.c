/* The game's arc tangents (GZLE01 cM_atan2s with U_GetAtanTable, cM_atan2f),
 * native.
 *
 * cM_atan2s takes a slope to a 16-bit angle by its octant and a 1025-entry
 * table (the quotient scaled and converted, fctiwz); cM_atan2f converts that
 * to radians. Actors, the camera and the wind call them throughout a frame
 * (0.5 percent of the game thread at Forest Haven with U_GetAtanTable).
 * cLib_addCalc and cLib_addCalc2, the easing beside them, are not here: they
 * are 60 Hz simulation sites (scripts/mods/prepare_simulation_60hz.py rewrites
 * them by frame-rate mode).
 *
 * Each native is its function's translation, callees included, replayed on
 * local registers (scripts/windows/native_leaf_gen.py wrote
 * native_calc_gen.inc from the blocks' prepaid copies; native_leaf_run.h runs
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
#include "native_calc.h"
#include "native_leaf_run.h"

#include <stdio.h>

int bluewake_native_calc_enabled;

/* A run's undo log (the game thread's: the natives run only there, and none
 * runs inside another). Static, so a native's frame stays small. */
static GxLog s_lf_log;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-label"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#include "native_calc_gen.inc"
#pragma clang diagnostic pop

#define LF_NATIVES CALC_NATIVES
#define LF_TAG "[native-calc]"
#define LF_ENTRY(id) bluewake_native_calc_##id
#define LF_REPORT bluewake_native_calc_report
#define LF_BY_ADDRESS bluewake_native_calc
#include "native_leaf_group.inc"
