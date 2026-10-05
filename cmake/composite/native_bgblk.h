#ifndef BLUEWAKE_NATIVE_BGBLK_H
#define BLUEWAKE_NATIVE_BGBLK_H

/* The collision blocks' bounds (cBgW::MakeBlckMinMax, cBgW::MakeBlckBnd with
 * MakeBlckTransMinMax and PSVECAdd), natively (native_bgblk.c). */

#include "core/cpu.h"
#include "native_bgblk_list.h"

extern int bluewake_native_bgblk_enabled;

/* The function at `address`, entered with the return address in LR, through
 * its blr: nonzero with every register, flag, cycle and byte of RAM as the
 * translation leaves them; zero, with nothing changed, where that is not
 * certain or the address is not one of these. No identifier here may be
 * `ctx`. */
int bluewake_native_bgblk(CPUState* cpu, u32 address);
void bluewake_native_bgblk_report(void);

/* The same, each native on its own (bluewake_native_bgblk_<entry>): what its
 * hook calls, with no dispatch on the address. */
#define BLUEWAKE_BGBLK_DECLARE(entry, native, name, digits) int bluewake_native_bgblk_##digits(CPUState* cpu);
BGBLK_NATIVES(BLUEWAKE_BGBLK_DECLARE)
#undef BLUEWAKE_BGBLK_DECLARE

#endif
