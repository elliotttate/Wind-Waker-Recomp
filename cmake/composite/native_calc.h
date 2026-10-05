#ifndef BLUEWAKE_NATIVE_CALC_H
#define BLUEWAKE_NATIVE_CALC_H

/* The game's own small calculations - easing a value toward a target
 * (cLib_addCalc, cLib_addCalc2), a slope's arc tangent as an angle
 * (cM_atan2s, cM_atan2f) - natively (native_calc.c). */

#include "core/cpu.h"
#include "native_calc_list.h"

extern int bluewake_native_calc_enabled;

/* The function at `address`, entered with the return address in LR, through
 * its blr: nonzero with every register, flag, cycle and byte of RAM as the
 * translation leaves them; zero, with nothing changed, where that is not
 * certain or the address is not one of these. No identifier here may be
 * `ctx`. */
int bluewake_native_calc(CPUState* cpu, u32 address);
void bluewake_native_calc_report(void);

/* The same, each native on its own (bluewake_native_calc_<entry>): what its
 * hook calls, with no dispatch on the address. */
#define BLUEWAKE_CALC_DECLARE(entry, native, name, digits) int bluewake_native_calc_##digits(CPUState* cpu);
CALC_NATIVES(BLUEWAKE_CALC_DECLARE)
#undef BLUEWAKE_CALC_DECLARE

#endif
