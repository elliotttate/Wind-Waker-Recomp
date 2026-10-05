#ifndef BLUEWAKE_NATIVE_GEOM_H
#define BLUEWAKE_NATIVE_GEOM_H

/* Geometry on the SDK's vector leaves (cM3d_CalcPla, cSPolar::Val,
 * dKyw_pntwind_get_info), natively (native_geom.c). */

#include "core/cpu.h"
#include "native_geom_list.h"

extern int bluewake_native_geom_enabled;

/* The function at `address`, entered with the return address in LR, through
 * its blr: nonzero with every register, flag, cycle and byte of RAM as the
 * translation leaves them; zero, with nothing changed, where that is not
 * certain or the address is not one of these. No identifier here may be
 * `ctx`. */
int bluewake_native_geom(CPUState* cpu, u32 address);
void bluewake_native_geom_report(void);

/* The same, each native on its own (bluewake_native_geom_<entry>): what its
 * hook calls, with no dispatch on the address. */
#define BLUEWAKE_GEOM_DECLARE(entry, native, name, digits) int bluewake_native_geom_##digits(CPUState* cpu);
GEOM_NATIVES(BLUEWAKE_GEOM_DECLARE)
#undef BLUEWAKE_GEOM_DECLARE

#endif
