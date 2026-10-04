#ifndef BLUEWAKE_NATIVE_ANIM_H
#define BLUEWAKE_NATIVE_ANIM_H

/* J3D's key-frame animation natively (native_anim.c). */

#include "core/cpu.h"

#define BLUEWAKE_ANIM_HERMITE 0x803012D8u       /* JMAHermiteInterpolation__Ffffffff */
#define BLUEWAKE_ANIM_KEY_F 0x802F2DACu         /* J3DGetKeyFrameInterpolation<f>__FfP18J3DAnmKeyTableBasePf */
#define BLUEWAKE_ANIM_KEY_S 0x802F072Cu         /* J3DGetKeyFrameInterpolationS__FfP18J3DAnmKeyTableBasePs */
#define BLUEWAKE_ANIM_TRANSFORM 0x802F0954u     /* calcTransform__18J3DAnmTransformKeyCFfUsP16J3DTransformInfo */
#define BLUEWAKE_ANIM_INVERSE_TRANSPOSE 0x802DA584u /* J3DPSCalcInverseTranspose__FPA4_fPA3_f */

extern int bluewake_native_anim_enabled;

/* The function at `address`, entered with the return address in LR, through
 * its blr: nonzero with every register, flag, cycle and byte as the
 * translation leaves them; zero, with nothing changed, where that is not
 * certain or the address is not one of these. No identifier here may be
 * `ctx`. */
int bluewake_native_anim(CPUState* cpu, u32 address);
void bluewake_native_anim_report(void);
/* How many calls of entry `which` (0 hermite, 1 key-f, 2 key-s, 3 transform, 4 inverse transpose)
 * the general (double-precision, checked) replay ran. */
unsigned long long bluewake_native_anim_general(unsigned which);

#endif
