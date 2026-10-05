#ifndef BLUEWAKE_NATIVE_LIBM_H
#define BLUEWAKE_NATIVE_LIBM_H

/* libm's fmod (__ieee754_fmod, fmod), the random numbers and the angle
 * conversion built on it (cM_rnd, cM_rndF, cM_rndFX, cM_rad2s), and its sin,
 * cos and tan, natively (native_libm.c). */

#include "core/cpu.h"
#include "native_libm_list.h"

extern int bluewake_native_libm_enabled;

/* The function at `address`, entered with the return address in LR, through
 * its blr: nonzero with every register, flag, cycle and byte of RAM as the
 * translation leaves them; zero, with nothing changed, where that is not
 * certain or the address is not one of these. No identifier here may be
 * `ctx`. */
int bluewake_native_libm(CPUState* cpu, u32 address);
void bluewake_native_libm_report(void);

/* The same, each native on its own (bluewake_native_libm_<entry>): what its
 * hook calls, with no dispatch on the address. */
#define BLUEWAKE_LIBM_DECLARE(entry, native, name, digits) int bluewake_native_libm_##digits(CPUState* cpu);
LIBM_NATIVES(BLUEWAKE_LIBM_DECLARE)
#undef BLUEWAKE_LIBM_DECLARE

#endif
