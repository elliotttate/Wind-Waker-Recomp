/* libm's fmod, sin, cos and tan, and the game's random numbers and angle
 * conversion on fmod (GZLE01 __ieee754_fmod, fmod, cM_rad2s, cM_rnd, cM_rndF,
 * cM_rndFX, sin, cos, tan), native.
 *
 * __ieee754_fmod is fdlibm's software remainder: the operands' words through
 * the stack, the exponents found by bit loops (extracted by the translator), a
 * shift-and-subtract loop for the quotient's bits, the result rebuilt - 1.8
 * percent of the game thread at Forest Haven, where the spores' and the
 * fireflies' random numbers (cM_rnd: three seeds' quotients summed, fmodf
 * against 1.0) and the angles (cM_rad2s: fmod against 2 pi) call it hundreds
 * of times a frame. The natives for cM_rnd, cM_rndF, cM_rndFX and cM_rad2s
 * replay fmod with them, across the boundary between their chunks. sin, cos
 * and tan are fdlibm's too: __ieee754_rem_pio2's reduction and the kernels'
 * polynomials (1.0 percent at Forest Haven, 1.2 at the sea).
 *
 * Each native is its function's translation, callees included, replayed on
 * local registers (scripts/windows/native_leaf_gen.py wrote
 * native_libm_gen.inc from the blocks' prepaid copies; native_leaf_run.h runs
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
 * test); an argument past 2^20 pi/2, which takes __kernel_rem_pio2, declines.
 * The report counts the declines by reason.
 *
 * tests/native_leaf_test.c compares each with the translation, every register
 * and byte, directly and through the hooked chunks. No identifier here may be
 * `ctx`. */
#include "native_libm.h"
#include "native_leaf_run.h"

#include <stdio.h>

int bluewake_native_libm_enabled;

/* A run's undo log (the game thread's: the natives run only there, and none
 * runs inside another). Static, so a native's frame stays small. */
static GxLog s_lf_log;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-label"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#include "native_libm_gen.inc"
#pragma clang diagnostic pop

#define LF_NATIVES LIBM_NATIVES
#define LF_TAG "[native-libm]"
#define LF_ENTRY(id) bluewake_native_libm_##id
#define LF_REPORT bluewake_native_libm_report
#define LF_BY_ADDRESS bluewake_native_libm
#include "native_leaf_group.inc"
