#ifndef BLUEWAKE_NATIVE_CC_H
#define BLUEWAKE_NATIVE_CC_H

/* The collision checker's area division natively (native_cc.c). */

#include "core/cpu.h"

#define BLUEWAKE_CC_DIVIDE_OVER_AREA 0x8024170Cu /* CalcDivideInfoOverArea__15cCcD_DivideAreaFP15cCcD_DivideInfoRC8cM3dGAab */

extern int bluewake_native_cc_enabled;

/* The function at `address`, entered with the return address in LR, through
 * its blr: nonzero with every register, flag, cycle and byte as the
 * translation leaves them; zero, with nothing changed, where that is not
 * certain or the address is not this one. No identifier here may be `ctx`. */
int bluewake_native_cc(CPUState* cpu, u32 address);
void bluewake_native_cc_report(void);

#endif
