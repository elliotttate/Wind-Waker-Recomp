/* Geometry on the SDK's vector leaves (GZLE01 cM3d_CalcPla with PSVECSubtract,
 * PSVECCrossProduct, PSVECMag, PSVECScale and PSVECDotProduct; cSPolar::Val
 * with cM_atan2f, cM_atan2s, U_GetAtanTable, cSPolar::Formal and its cSAngle
 * operations; dKyw_pntwind_get_info with dKyr_get_vectle_calc,
 * get_vectle_calc, vectle_calc and PSVECSquareDistance), native.
 *
 * cM3d_CalcPla is a triangle's plane (normal and distance), a moving
 * background's every triangle every frame (0.6 percent of the game thread at
 * Forest Haven, and its vector leaves' calls); cSPolar::Val a vector's polar
 * coordinates (two square roots by frsqrte and Newton steps, two arc tangents;
 * 0.6 percent); dKyw_pntwind_get_info the point winds' pull on a position, 30
 * slots looked at for every spore and leaf (0.5 percent).
 *
 * Each native is its function's translation, callees included, replayed on
 * local registers (scripts/windows/native_leaf_gen.py wrote
 * native_geom_gen.inc from the blocks' prepaid copies; native_leaf_run.h runs
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
#include "native_geom.h"
#include "native_leaf_run.h"

#include <stdio.h>

int bluewake_native_geom_enabled;

/* A run's undo log (the game thread's: the natives run only there, and none
 * runs inside another). Static, so a native's frame stays small. */
static GxLog s_lf_log;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-label"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#include "native_geom_gen.inc"
#pragma clang diagnostic pop

#define LF_NATIVES GEOM_NATIVES
#define LF_TAG "[native-geom]"
#define LF_ENTRY(id) bluewake_native_geom_##id
#define LF_REPORT bluewake_native_geom_report
#define LF_BY_ADDRESS bluewake_native_geom
#include "native_leaf_group.inc"
