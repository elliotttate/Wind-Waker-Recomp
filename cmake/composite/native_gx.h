#ifndef BLUEWAKE_NATIVE_GX_H
#define BLUEWAKE_NATIVE_GX_H

/* The GX SDK's FIFO writers natively (native_gx.c). */

#include "core/cpu.h"

#define BLUEWAKE_GX_LOAD_POS_MTX_IMM 0x80326F38u  /* GXLoadPosMtxImm */
#define BLUEWAKE_GX_LOAD_NRM_MTX_IMM 0x80326F88u  /* GXLoadNrmMtxImm */
#define BLUEWAKE_GX_SET_TEV_COLOR 0x80325FA8u     /* GXSetTevColor */
#define BLUEWAKE_GX_SET_TEV_COLOR_S10 0x8032601Cu /* GXSetTevColorS10 */
#define BLUEWAKE_GX_SET_TEV_KCOLOR 0x80326090u    /* GXSetTevKColor */
#define BLUEWAKE_GX_SET_ARRAY 0x80322568u         /* GXSetArray */
#define BLUEWAKE_GX_SET_TEV_ORDER 0x803263A0u     /* GXSetTevOrder */
#define BLUEWAKE_GX_CALL_DISPLAY_LIST 0x80326B80u /* GXCallDisplayList */
#define BLUEWAKE_GX_BEGIN 0x803230C4u             /* GXBegin */
#define BLUEWAKE_GX_LOAD_TEX_OBJ_PRELOADED 0x80324D50u /* GXLoadTexObjPreLoaded */
#define BLUEWAKE_GX_LOAD_TEX_OBJ 0x80324EE8u      /* GXLoadTexObj */
#define BLUEWAKE_GX_SET_SU_TEX_REGS 0x803253B8u   /* __GXSetSUTexRegs */
#define BLUEWAKE_GX_SET_VAT 0x803221D8u           /* __GXSetVAT */
#define BLUEWAKE_GX_SET_MATRIX_INDEX 0x80327364u  /* __GXSetMatrixIndex */

#include "native_gx_list.h"

extern int bluewake_native_gx_enabled;

/* The function at `address`, entered with the return address in LR, through
 * its blr: nonzero with every register, flag, cycle, byte of RAM and byte
 * handed to the gather pipe as the translation leaves them; zero, with
 * nothing changed, where that is not certain or the address is not one of
 * these. No identifier here may be `ctx`. */
int bluewake_native_gx(CPUState* cpu, u32 address);
void bluewake_native_gx_report(void);

/* The same, each native on its own (bluewake_native_gx_80326F38 for
 * GXLoadPosMtxImm, ...): what its hook calls, with no dispatch on the
 * address. */
#define BLUEWAKE_GX_DECLARE(entry, native, name, digits) int bluewake_native_gx_##digits(CPUState* cpu);
GX_NATIVES(BLUEWAKE_GX_DECLARE)
#undef BLUEWAKE_GX_DECLARE

#endif
