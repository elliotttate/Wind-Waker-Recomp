#ifndef BLUEWAKE_NATIVE_JAS_H
#define BLUEWAKE_NATIVE_JAS_H

/* JASystem's leaves: an envelope oscillator's value (TOscillator::getOffset,
 * TOscillator::calc), natively (native_jas.c). */

#include "core/cpu.h"
#include "native_jas_list.h"

extern int bluewake_native_jas_enabled;

/* The function at `address`, entered with the return address in LR, through
 * its blr: nonzero with every register, flag, cycle and byte of RAM as the
 * translation leaves them; zero, with nothing changed, where that is not
 * certain or the address is not one of these. No identifier here may be
 * `ctx`. */
int bluewake_native_jas(CPUState* cpu, u32 address);
void bluewake_native_jas_report(void);

/* The same, each native on its own (bluewake_native_jas_<entry>): what its
 * hook calls, with no dispatch on the address. */
#define BLUEWAKE_JAS_DECLARE(entry, native, name, digits) int bluewake_native_jas_##digits(CPUState* cpu);
JAS_NATIVES(BLUEWAKE_JAS_DECLARE)
#undef BLUEWAKE_JAS_DECLARE

#endif
