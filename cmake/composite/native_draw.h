#ifndef BLUEWAKE_NATIVE_DRAW_H
#define BLUEWAKE_NATIVE_DRAW_H

/* The particle draw code and the sea's waves natively, with stops
 * (native_draw.c). */

#include "core/cpu.h"

#include "native_draw_list.h"

extern int bluewake_native_draw_enabled;

/* The function from `address` (an entry, or a resume inside it) on: 1 with
 * every register, flag, cycle, byte of RAM and byte handed to the gather pipe
 * as the translation leaves them at the function's blr, pc the return
 * address; 2 with all of that as the translation has it just before the
 * instruction at pc - a call the native does not make, or a block it leaves
 * to the translation - the block prepaid where pc is inside one, for the
 * hook to go on with the translation there (cycle_block_prepaid set); 0,
 * with nothing changed, where neither is certain or the address is not one
 * of these. No identifier here may be `ctx`. */
int bluewake_native_draw(CPUState* cpu, u32 address);
void bluewake_native_draw_report(void);
/* [0] declined, [1] done, [2] stopped, over every entry and resume. */
void bluewake_native_draw_totals(unsigned long long totals[3]);

/* The same, each entry and resume on its own (bluewake_native_draw_80260D24
 * for JPADrawExecRotBillBoard::exec, ...): what its hook calls. */
#define BLUEWAKE_DRAW_DECLARE(entry, native, name, digits) int bluewake_native_draw_##digits(CPUState* cpu);
DRAW_NATIVES(BLUEWAKE_DRAW_DECLARE)
#undef BLUEWAKE_DRAW_DECLARE

#endif
