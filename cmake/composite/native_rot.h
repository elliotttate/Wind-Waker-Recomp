#ifndef BLUEWAKE_NATIVE_ROT_H
#define BLUEWAKE_NATIVE_ROT_H

/* JMAEulerToQuat, the key-frame animation's Euler angles to a quaternion,
 * natively (native_rot.c). */

#include "core/cpu.h"
#include "native_rot_list.h"

extern int bluewake_native_rot_enabled;

/* The function at `address`, entered with the return address in LR, through
 * its blr: nonzero with every register, flag, cycle and byte of RAM as the
 * translation leaves them; zero, with nothing changed, where that is not
 * certain or the address is not one of these. No identifier here may be
 * `ctx`. */
int bluewake_native_rot(CPUState* cpu, u32 address);
void bluewake_native_rot_report(void);

/* The same, each native on its own (bluewake_native_rot_<entry>): what its
 * hook calls, with no dispatch on the address. */
#define BLUEWAKE_ROT_DECLARE(entry, native, name, digits) int bluewake_native_rot_##digits(CPUState* cpu);
ROT_NATIVES(BLUEWAKE_ROT_DECLARE)
#undef BLUEWAKE_ROT_DECLARE

#endif
