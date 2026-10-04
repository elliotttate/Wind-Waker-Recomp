#ifndef BLUEWAKE_NATIVE_FIFO_H
#define BLUEWAKE_NATIVE_FIFO_H

/* J3D's immediate matrix loads to the GX FIFO natively (native_fifo.c). */

#include "core/cpu.h"

#define BLUEWAKE_J3D_FIFO_POS_MTX 0x802D8BD8u    /* J3DFifoLoadPosMtxImm__FPA4_fUl */
#define BLUEWAKE_J3D_FIFO_NRM_MTX 0x802D8C58u    /* J3DFifoLoadNrmMtxImm__FPA4_fUl */
#define BLUEWAKE_J3D_FIFO_NRM_MTX33 0x802D8CC4u  /* J3DFifoLoadNrmMtxImm3x3__FPA3_fUl */

extern int bluewake_native_fifo_enabled;

/* The function at `address`, entered with the return address in LR, through
 * its blr: nonzero with every register, flag, cycle, byte of RAM and byte
 * handed to the gather pipe as the translation leaves them; zero, with
 * nothing changed, where that is not certain or the address is not one of
 * these. No identifier here may be `ctx`. */
int bluewake_native_fifo(CPUState* cpu, u32 address);
void bluewake_native_fifo_report(void);

#endif
