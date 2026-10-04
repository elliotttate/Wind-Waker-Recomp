#ifndef BLUEWAKE_NATIVE_MTXCALC_H
#define BLUEWAKE_NATIVE_MTXCALC_H

/* J3D's joint matrix calculations natively (native_mtxcalc.c). */

#include "core/cpu.h"

#define BLUEWAKE_MTXCALC_BASIC 0x802F5090u     /* calcTransform__15J3DMtxCalcBasicFUsRC16J3DTransformInfo */
#define BLUEWAKE_MTXCALC_SOFTIMAGE 0x802F52BCu /* calcTransform__19J3DMtxCalcSoftimageFUsRC16J3DTransformInfo */
#define BLUEWAKE_MTXCALC_MAYA 0x802F5508u      /* calcTransform__14J3DMtxCalcMayaFUsRC16J3DTransformInfo */

extern int bluewake_native_mtxcalc_enabled;

/* The function at `address`, entered with the return address in LR, through
 * its blr - its calls included: nonzero with every register, flag, cycle and
 * byte as the translation leaves them; zero, with nothing changed, where that
 * is not certain or the address is not one of these. No identifier here may
 * be `ctx`. */
int bluewake_native_mtxcalc(CPUState* cpu, u32 address);
void bluewake_native_mtxcalc_report(void);

#endif
