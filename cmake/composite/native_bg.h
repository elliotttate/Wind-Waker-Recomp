#ifndef BLUEWAKE_NATIVE_BG_H
#define BLUEWAKE_NATIVE_BG_H

/* Two collision checks natively (native_bg.c). */

#include "core/cpu.h"

#define BLUEWAKE_BG_CHK_SAME_ACTOR_PID 0x8024734Cu /* ChkSameActorPid__8cBgS_ChkCFUi */
#define BLUEWAKE_BG_CHK_GRP_THROUGH 0x800A9684u    /* ChkGrpThrough__4dBgWFiP15cBgS_GrpPassChki */

extern int bluewake_native_bg_enabled;

/* The function at `address`, entered with the return address in LR, through
 * its blr: nonzero with every register, flag, cycle and byte as the
 * translation leaves them; zero, with nothing changed, where that is not
 * certain or the address is not one of these. No identifier here may be
 * `ctx`. */
int bluewake_native_bg(CPUState* cpu, u32 address);
void bluewake_native_bg_report(void);

#endif
