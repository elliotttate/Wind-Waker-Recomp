#ifndef BLUEWAKE_NATIVE_VEC_H
#define BLUEWAKE_NATIVE_VEC_H

/* The SDK's small vector leaves natively (native_vec.c). */

#include "core/cpu.h"

#define BLUEWAKE_PSVEC_ADD 0x8030DCE0u
#define BLUEWAKE_PSVEC_SUBTRACT 0x8030DD04u
#define BLUEWAKE_PSVEC_SCALE 0x8030DD28u
#define BLUEWAKE_PSVEC_SQUARE_MAG 0x8030DE50u
#define BLUEWAKE_PSVEC_DOT_PRODUCT 0x8030DEACu
#define BLUEWAKE_PSVEC_CROSS_PRODUCT 0x8030DECCu
#define BLUEWAKE_PSVEC_SQUARE_DISTANCE 0x8030E0B4u
#define BLUEWAKE_PSVEC_NORMALIZE 0x8030DE0Cu
#define BLUEWAKE_PSVEC_MAG 0x8030DE68u

/* The leaf at `address`, entered with the return address in LR, through its
 * blr: nonzero with every register, flag, cycle and byte as the translation
 * leaves them; zero, with nothing changed, where that is not certain or the
 * address is not one of these leaves. No identifier here may be `ctx`. */
int bluewake_native_vec(CPUState* cpu, u32 address);
void bluewake_native_vec_report(void);

/* PSMTXMultVecSR, added with the second set of natives (hooked at its entry
 * by scripts/windows/native_entries.py): the same contract, for the leaf at
 * BLUEWAKE_PSMTX_MULT_VEC_SR only. */
#define BLUEWAKE_PSMTX_MULT_VEC_SR 0x8030DB24u
extern int bluewake_native_vec_sr_enabled;
int bluewake_native_vec_sr(CPUState* cpu);
void bluewake_native_vec_sr_report(void);

#endif
