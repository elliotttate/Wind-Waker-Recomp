#ifndef BLUEWAKE_NATIVE_KANKYO_H
#define BLUEWAKE_NATIVE_KANKYO_H

/* The environment's colour blends natively (native_kankyo.c). */

#include "core/cpu.h"

#define BLUEWAKE_KANKYO_S16_RATIO 0x8018F894u      /* s16_data_ratio_set__Fssf (d_kankyo) */
#define BLUEWAKE_KANKYO_COLOR_RATIO 0x8018F8E4u    /* kankyo_color_ratio_set__FUcUcfUcUcfsf */
#define BLUEWAKE_KYEFF_S16_RATIO 0x8019803Cu       /* s16_data_ratio_set__Fssf (d_kyeff) */

extern int bluewake_native_kankyo_enabled;

/* The function at `address`, entered with the return address in LR, through
 * its blr: nonzero with every register, flag, cycle and byte as the
 * translation leaves them; zero, with nothing changed, where that is not
 * certain or the address is not one of these. No identifier here may be
 * `ctx`. */
int bluewake_native_kankyo(CPUState* cpu, u32 address);
void bluewake_native_kankyo_report(void);

#endif
