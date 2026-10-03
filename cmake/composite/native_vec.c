/* The SDK's small vector leaves (GZLE01 PSVECAdd, PSVECSubtract, PSVECScale,
 * PSVECSquareMag, PSVECDotProduct, PSVECCrossProduct, PSVECSquareDistance,
 * PSVECNormalize, PSVECMag), native.
 *
 * Each is one block of six to fifteen paired-single instructions, and the
 * game calls them from everywhere - collision, actors, the camera: at native
 * 60 Hz on Outset they were 2.5 percent of the game thread, most of it the
 * call itself (the chunk's entry, its block machinery, the return). The
 * direct calls to them (scripts/windows/direct_calls.py) try these first.
 *
 * Each runs the leaf's operations in order on the same values with the
 * translation's own arithmetic (inline_fp.h: the interpreter's multiplier and
 * single rounding, NI's flush), its loads and stores in the same order, and
 * leaves the registers it writes, the FPRF of its last arithmetic, the cycles
 * and the last access's cycle suffix as the translation does. It declines,
 * changing nothing, unless that is certain: FP available, paired singles
 * unscaled, rounding to nearest, no write journal, every address plain RAM,
 * no store overlapping a later load, every float read finite and below 2^62
 * in magnitude (so no product, sum or single rounding reaches infinity, and
 * every operation takes the inline path the translation takes), the turn's
 * budget not spent, and the next deadline beyond the block (so the
 * translation prepays it and refunds nothing).
 *
 * tests/native_vec_test.c compares each against the personal module's
 * translation, every register and byte. No identifier here may be `ctx`. */
#include "native_vec.h"
#include "inline_fp.h"

#include <stdio.h>

enum {
    VEC_ADD, VEC_SUBTRACT, VEC_SCALE, VEC_SQUARE_MAG, VEC_DOT, VEC_CROSS, VEC_SQUARE_DISTANCE, VEC_NORMALIZE,
    VEC_MAG, VEC_COUNT
};
static unsigned long long s_vec_runs[VEC_COUNT], s_vec_declined[VEC_COUNT];

void bluewake_native_vec_report(void) {
    fprintf(stderr,
            "[native-vec] add=%llu/%llu sub=%llu/%llu scale=%llu/%llu sqmag=%llu/%llu dot=%llu/%llu "
            "cross=%llu/%llu sqdist=%llu/%llu normalize=%llu/%llu mag=%llu/%llu (native/declined)\n",
            s_vec_runs[0], s_vec_declined[0], s_vec_runs[1], s_vec_declined[1], s_vec_runs[2], s_vec_declined[2],
            s_vec_runs[3], s_vec_declined[3], s_vec_runs[4], s_vec_declined[4], s_vec_runs[5], s_vec_declined[5],
            s_vec_runs[6], s_vec_declined[6], s_vec_runs[7], s_vec_declined[7], s_vec_runs[8],
            s_vec_declined[8]);
}

typedef struct VecPair { f64 p0, p1; } VecPair;

static inline const u8* vec_ram(const CPUState* cpu, u32 address, u32 size) {
    if (!ppc_dispatch_poll_read_stable((CPUState*)cpu, address, size))
        return NULL;
    return cpu->ram + (address - GC_RAM_BASE);
}

/* Finite, and below 2^62 in magnitude (denormals and zeros included). */
static inline bool vec_bounded(u32 bits) {
    return (bits & 0x7F800000u) <= ((127u + 61u) << 23);
}

/* `count` floats at `address`, in RAM and bounded. */
static inline bool vec_floats(const CPUState* cpu, u32 address, unsigned count) {
    const u8* p = vec_ram(cpu, address, 4u * count);
    if (p == NULL)
        return false;
    bool ok = true;
    for (unsigned i = 0; i < count; ++i)
        ok &= vec_bounded(read_be32(p + 4u * i));
    return ok;
}

static inline bool vec_apart(u32 a, u32 a_size, u32 b, u32 b_size) {
    return (u64)a + a_size <= (u64)b || (u64)b + b_size <= (u64)a;
}

static inline f64 vec_single(u32 bits) {
    return f64_value(convert_to_double(bits));
}

static inline u32 vec_word(const CPUState* cpu, u32 address) {
    return read_be32(cpu->ram + (address - GC_RAM_BASE));
}

/* psq_l with GQR0 (type 0): W=1 loads one single and a 1.0. */
static inline VecPair vec_psq_l(const CPUState* cpu, u32 address, bool w) {
    return (VecPair){vec_single(vec_word(cpu, address)), w ? 1.0 : vec_single(vec_word(cpu, address + 4u))};
}

/* lfs: the single in both halves. */
static inline VecPair vec_lfs(const CPUState* cpu, u32 address) {
    const f64 value = vec_single(vec_word(cpu, address));
    return (VecPair){value, value};
}

static inline void vec_store32(CPUState* cpu, u32 address, u32 value) {
    clear_matching_reservation(cpu, address);
    write_be32(cpu->ram + (address - GC_RAM_BASE), value);
}

/* psq_st with GQR0 (type 0): W=1 stores the first half only. */
static inline void vec_psq_st(CPUState* cpu, u32 address, VecPair value, bool w) {
    vec_store32(cpu, address, convert_to_single_ftz(f64_bits(value.p0)));
    if (!w)
        vec_store32(cpu, address + 4u, convert_to_single_ftz(f64_bits(value.p1)));
}

/* The inline paired-single operations (inline_fp.h) on finite operands, as
 * values; each records the class of its first half for the FPRF. */
static inline VecPair vec_result(const CPUState* cpu, f64 r0, f64 r1, u32* fprf) {
    const f32 s0 = bw_fp_single(cpu, r0), s1 = bw_fp_single(cpu, r1);
    *fprf = bw_fp_class32(s0);
    return (VecPair){(f64)s0, (f64)s1};
}

static inline VecPair vec_add(const CPUState* cpu, VecPair a, VecPair b, u32* fprf) {
    return vec_result(cpu, a.p0 + b.p0, a.p1 + b.p1, fprf);
}

static inline VecPair vec_sub(const CPUState* cpu, VecPair a, VecPair b, u32* fprf) {
    return vec_result(cpu, a.p0 - b.p0, a.p1 - b.p1, fprf);
}

static inline VecPair vec_mul(const CPUState* cpu, VecPair a, VecPair c, u32* fprf) {
    return vec_result(cpu, a.p0 * bw_fp_25bit(c.p0), a.p1 * bw_fp_25bit(c.p1), fprf);
}

static inline VecPair vec_muls0(const CPUState* cpu, VecPair a, f64 c0, u32* fprf) {
    const f64 c_round = bw_fp_25bit(c0);
    return vec_result(cpu, a.p0 * c_round, a.p1 * c_round, fprf);
}

static inline VecPair vec_madd(const CPUState* cpu, VecPair a, VecPair c, VecPair b, bool subtract, u32* fprf) {
    return vec_result(cpu, bw_fp_fma_single(a.p0, bw_fp_25bit(c.p0), subtract ? -b.p0 : b.p0),
                      bw_fp_fma_single(a.p1, bw_fp_25bit(c.p1), subtract ? -b.p1 : b.p1), fprf);
}

/* ps_sum0 d, a, c, b: a0 + b1 in the first half, c1 carried in the second. */
static inline VecPair vec_sum0(const CPUState* cpu, VecPair a, VecPair c, VecPair b, u32* fprf) {
    const f32 s0 = bw_fp_single(cpu, a.p0 + b.p1), s1 = bw_fp_single(cpu, c.p1);
    *fprf = bw_fp_class32(s0);
    return (VecPair){(f64)s0, (f64)s1};
}

/* ps_neg as the translation writes it: through the single's bits. */
static inline f64 vec_neg(f64 value) {
    return f64_value(convert_to_double(convert_to_single(f64_bits(value)) ^ 0x80000000u));
}

static inline void vec_set(CPUState* cpu, unsigned r, VecPair value) {
    cpu->fpr[r] = value.p0;
    cpu->ps1[r] = value.p1;
}

/* The one block's entry conditions (a translation prepaying it all, with no
 * refund), and its end. */
static inline bool vec_ready(const CPUState* cpu, s64 cycles) {
    const u32 gqr = cpu->gqr[0];
    return cpu->exception == 0u && (cpu->msr & PPC_MSR_FP) != 0u && (cpu->hid2 & PPC_HID2_LSQE) != 0u &&
           ((gqr >> 16) & 7u) == 0u && (gqr & 7u) == 0u && (cpu->fpscr & 3u) == 0u &&
           g_mem_write_journal == NULL && cpu->cycle_budget > 0 && cpu->downcount > -cpu->cycle_budget &&
           (cpu->cycle_deadline_budget <= 0 ||
            (cpu->cycle_deadline_budget >= cycles && cpu->cycle_deadline_budget + cpu->downcount >= cycles));
}

static inline int vec_finish(CPUState* cpu, s64 cycles, u32 suffix, u32 fprf) {
    cpu->fpscr = (cpu->fpscr & ~(0x1Fu << 12)) | (fprf << 12);
    cpu->downcount -= cycles;
    cpu->cycle_observation_suffix = suffix;
    cpu->pc = cpu->lr & ~3u;
    return 1;
}

/* PSVECAdd, PSVECSubtract (r3 a, r4 b, r5 out): the store of x and y comes
 * before the loads of z. */
static int vec_add_sub(CPUState* cpu, bool subtract) {
    const u32 a = cpu->gpr[3], b = cpu->gpr[4], out = cpu->gpr[5];
    if (!vec_ready(cpu, 9) || !vec_floats(cpu, a, 3) || !vec_floats(cpu, b, 3) || !vec_ram(cpu, out, 12) ||
        !vec_apart(out, 8, a + 8u, 4) || !vec_apart(out, 8, b + 8u, 4))
        return 0;
    u32 fprf = 0;
    const VecPair f2 = vec_psq_l(cpu, a, false), f4 = vec_psq_l(cpu, b, false);
    const VecPair f6 = subtract ? vec_sub(cpu, f2, f4, &fprf) : vec_add(cpu, f2, f4, &fprf);
    vec_psq_st(cpu, out, f6, false);
    const VecPair f3 = vec_psq_l(cpu, a + 8u, true), f5 = vec_psq_l(cpu, b + 8u, true);
    const VecPair f7 = subtract ? vec_sub(cpu, f3, f5, &fprf) : vec_add(cpu, f3, f5, &fprf);
    vec_psq_st(cpu, out + 8u, f7, true);
    vec_set(cpu, 2, f2);
    vec_set(cpu, 3, f3);
    vec_set(cpu, 4, f4);
    vec_set(cpu, 5, f5);
    vec_set(cpu, 6, f6);
    vec_set(cpu, 7, f7);
    return vec_finish(cpu, 9, 1, fprf);
}

/* PSVECScale (r3 v, r4 out, f1 the scale). */
static int vec_scale(CPUState* cpu) {
    const u32 v = cpu->gpr[3], out = cpu->gpr[4];
    const f64 scale = cpu->fpr[1];
    const u64 scale_bits = f64_bits(scale) & 0x7FFFFFFFFFFFFFFFull;
    if (!vec_ready(cpu, 7) || scale_bits >= 0x43D0000000000000ull /* 2^62, and not finite */ ||
        !vec_floats(cpu, v, 3) || !vec_ram(cpu, out, 12))
        return 0;
    u32 fprf = 0;
    VecPair f0 = vec_psq_l(cpu, v, false);
    const VecPair f2 = vec_psq_l(cpu, v + 8u, true);
    f0 = vec_muls0(cpu, f0, scale, &fprf);
    vec_psq_st(cpu, out, f0, false);
    f0 = vec_muls0(cpu, f2, scale, &fprf);
    vec_psq_st(cpu, out + 8u, f0, true);
    vec_set(cpu, 0, f0);
    vec_set(cpu, 2, f2);
    return vec_finish(cpu, 7, 1, fprf);
}

/* PSVECSquareMag (r3 v): f1. */
static int vec_square_mag(CPUState* cpu) {
    const u32 v = cpu->gpr[3];
    if (!vec_ready(cpu, 6) || !vec_floats(cpu, v, 3))
        return 0;
    u32 fprf = 0;
    VecPair f0 = vec_psq_l(cpu, v, false);
    f0 = vec_mul(cpu, f0, f0, &fprf);
    VecPair f1 = vec_lfs(cpu, v + 8u);
    f1 = vec_madd(cpu, f1, f1, f0, false, &fprf);
    f1 = vec_sum0(cpu, f1, f0, f0, &fprf);
    vec_set(cpu, 0, f0);
    vec_set(cpu, 1, f1);
    return vec_finish(cpu, 6, 3, fprf);
}

/* PSVECDotProduct (r3 a, r4 b): f1. */
static int vec_dot(CPUState* cpu) {
    const u32 a = cpu->gpr[3], b = cpu->gpr[4];
    if (!vec_ready(cpu, 8) || !vec_floats(cpu, a, 3) || !vec_floats(cpu, b, 3))
        return 0;
    u32 fprf = 0;
    VecPair f2 = vec_psq_l(cpu, a + 4u, false);
    VecPair f3 = vec_psq_l(cpu, b + 4u, false);
    f2 = vec_mul(cpu, f2, f3, &fprf);
    const VecPair f5 = vec_psq_l(cpu, a, false), f4 = vec_psq_l(cpu, b, false);
    f3 = vec_madd(cpu, f5, f4, f2, false, &fprf);
    const VecPair f1 = vec_sum0(cpu, f3, f2, f2, &fprf);
    vec_set(cpu, 1, f1);
    vec_set(cpu, 2, f2);
    vec_set(cpu, 3, f3);
    vec_set(cpu, 4, f4);
    vec_set(cpu, 5, f5);
    return vec_finish(cpu, 8, 3, fprf);
}

/* PSVECCrossProduct (r3 a, r4 b, r5 out): every load before the stores. */
static int vec_cross(CPUState* cpu) {
    const u32 a = cpu->gpr[3], b = cpu->gpr[4], out = cpu->gpr[5];
    if (!vec_ready(cpu, 15) || !vec_floats(cpu, a, 3) || !vec_floats(cpu, b, 3) || !vec_ram(cpu, out, 12))
        return 0;
    u32 fprf = 0;
    const VecPair f1 = vec_psq_l(cpu, b, false);
    const VecPair f2 = vec_lfs(cpu, a + 8u);
    const VecPair f0 = vec_psq_l(cpu, a, false);
    const VecPair f6 = {f1.p1, f1.p0}; /* ps_merge10 */
    const VecPair f3 = vec_lfs(cpu, b + 8u);
    const VecPair f4 = vec_mul(cpu, f1, f2, &fprf);
    const VecPair f7 = vec_muls0(cpu, f1, f0.p0, &fprf);
    const VecPair f5 = vec_madd(cpu, f0, f3, f4, true, &fprf);
    const VecPair f8 = vec_madd(cpu, f0, f6, f7, true, &fprf);
    const VecPair f9 = {f5.p1, f5.p1};     /* ps_merge11 */
    VecPair f10 = {f5.p0, f8.p1};          /* ps_merge01 */
    vec_psq_st(cpu, out, f9, true);
    f10 = (VecPair){vec_neg(f10.p0), vec_neg(f10.p1)};
    vec_psq_st(cpu, out + 4u, f10, false);
    vec_set(cpu, 0, f0);
    vec_set(cpu, 1, f1);
    vec_set(cpu, 2, f2);
    vec_set(cpu, 3, f3);
    vec_set(cpu, 4, f4);
    vec_set(cpu, 5, f5);
    vec_set(cpu, 6, f6);
    vec_set(cpu, 7, f7);
    vec_set(cpu, 8, f8);
    vec_set(cpu, 9, f9);
    vec_set(cpu, 10, f10);
    return vec_finish(cpu, 15, 1, fprf);
}

/* PSVECSquareDistance (r3 a, r4 b): f1. */
static int vec_square_distance(CPUState* cpu) {
    const u32 a = cpu->gpr[3], b = cpu->gpr[4];
    if (!vec_ready(cpu, 10) || !vec_floats(cpu, a, 3) || !vec_floats(cpu, b, 3))
        return 0;
    u32 fprf = 0;
    VecPair f0 = vec_psq_l(cpu, a + 4u, false);
    VecPair f1 = vec_psq_l(cpu, b + 4u, false);
    VecPair f2 = vec_sub(cpu, f0, f1, &fprf);
    f0 = vec_psq_l(cpu, a, false);
    f1 = vec_psq_l(cpu, b, false);
    f2 = vec_mul(cpu, f2, f2, &fprf);
    f0 = vec_sub(cpu, f0, f1, &fprf);
    f1 = vec_madd(cpu, f0, f0, f2, false, &fprf);
    f1 = vec_sum0(cpu, f1, f2, f2, &fprf);
    vec_set(cpu, 0, f0);
    vec_set(cpu, 1, f1);
    vec_set(cpu, 2, f2);
    return vec_finish(cpu, 10, 5, fprf);
}

/* PSVECNormalize (r3 v, r4 out; the SDA's 0.5 and 3.0 at r2-12884): the
 * translation's own statements with the block machinery taken out, on the
 * registers themselves - its inline helpers and the interpreter's frsqrte
 * (inline_fp.h renames the ppc_ operations as it does in the chunks), so
 * every value and flag is theirs whatever the operands; the interpreter only
 * sets FPSCR flags, so nothing here can stop the block part of the way. Only
 * the accesses need plain RAM, and every load comes before the stores. */
static int vec_normalize(CPUState* cpu) {
    const u32 v = cpu->gpr[3], out = cpu->gpr[4], constants = cpu->gpr[2] + (u32)(s32)-12884;
    const u32 gqr = cpu->gqr[0];
    if (cpu->exception != 0u || (cpu->msr & PPC_MSR_FP) == 0u || (cpu->hid2 & PPC_HID2_LSQE) == 0u ||
        ((gqr >> 16) & 7u) != 0u || (gqr & 7u) != 0u || g_mem_write_journal != NULL || cpu->cycle_budget <= 0 ||
        cpu->downcount <= -cpu->cycle_budget ||
        (cpu->cycle_deadline_budget > 0 &&
         (cpu->cycle_deadline_budget < 17 || cpu->cycle_deadline_budget + cpu->downcount < 17)) ||
        !vec_ram(cpu, constants, 8) || !vec_ram(cpu, v, 12) || !vec_ram(cpu, out, 12))
        return 0;
    cpu->fpr[0] = cpu->ps1[0] = vec_single(vec_word(cpu, constants));
    cpu->fpr[1] = cpu->ps1[1] = vec_single(vec_word(cpu, constants + 4u));
    vec_set(cpu, 2, vec_psq_l(cpu, v, false));
    ppc_ps_mul_op(cpu, 5, 2, 2);
    vec_set(cpu, 3, vec_psq_l(cpu, v + 8u, true));
    ppc_ps_madd_op(cpu, 4, 3, 3, 5, false, false);
    ppc_ps_sum0(cpu, 4, 4, 3, 5);
    {
        f64 result;
        if (ppc_frsqrte(cpu, cpu->fpr[4], &result))
            cpu->fpr[5] = result;
    }
    ppc_fmuls(cpu, 6, 5, 5);
    ppc_fmuls(cpu, 0, 5, 0);
    {
        f64 result;
        if (ppc_fma(cpu, cpu->fpr[6], cpu->fpr[4], cpu->fpr[1], true, true, true, &result))
            cpu->fpr[6] = cpu->ps1[6] = result;
    }
    ppc_fmuls(cpu, 5, 6, 0);
    ppc_ps_muls0(cpu, 2, 2, 5);
    vec_psq_st(cpu, out, (VecPair){cpu->fpr[2], cpu->ps1[2]}, false);
    ppc_ps_muls0(cpu, 3, 3, 5);
    vec_psq_st(cpu, out + 8u, (VecPair){cpu->fpr[3], cpu->ps1[3]}, true);
    cpu->downcount -= 17;
    cpu->cycle_observation_suffix = 1u;
    cpu->pc = cpu->lr & ~3u;
    return 1;
}

/* PSVECMag (r3 v; the SDA's 0.5 and 3.0 at r2-12884): as PSVECNormalize, the
 * translation's statements on the registers. Three blocks - 9 cycles, then
 * the root's 7 unless the square is zero, then the return's 1 - so the turn's
 * budget must reach past the longer path's last block start. */
static int vec_mag(CPUState* cpu) {
    const u32 v = cpu->gpr[3], constants = cpu->gpr[2] + (u32)(s32)-12884;
    const u32 gqr = cpu->gqr[0];
    if (cpu->exception != 0u || (cpu->msr & PPC_MSR_FP) == 0u || (cpu->hid2 & PPC_HID2_LSQE) == 0u ||
        ((gqr >> 16) & 7u) != 0u || g_mem_write_journal != NULL || cpu->cycle_budget <= 0 ||
        cpu->downcount - 17 <= -cpu->cycle_budget ||
        (cpu->cycle_deadline_budget > 0 &&
         (cpu->cycle_deadline_budget < 17 || cpu->cycle_deadline_budget + cpu->downcount < 17)) ||
        !vec_ram(cpu, constants, 8) || !vec_ram(cpu, v, 12))
        return 0;
    cpu->fpr[4] = cpu->ps1[4] = vec_single(vec_word(cpu, constants));
    vec_set(cpu, 0, vec_psq_l(cpu, v, false));
    ppc_ps_mul_op(cpu, 0, 0, 0);
    cpu->fpr[1] = cpu->ps1[1] = vec_single(vec_word(cpu, v + 8u));
    ppc_fsubs(cpu, 2, 4, 4);
    ppc_ps_madd_op(cpu, 1, 1, 1, 0, false, false);
    ppc_ps_sum0(cpu, 1, 1, 0, 0);
    ppc_fcmp(cpu, 0, cpu->fpr[1], cpu->fpr[2], false);
    s64 cycles = 9 + 1;
    if ((cpu->cr & 0x20000000u) == 0u) {
        {
            f64 result;
            if (ppc_frsqrte(cpu, cpu->fpr[1], &result))
                cpu->fpr[0] = result;
        }
        cpu->fpr[3] = cpu->ps1[3] = vec_single(vec_word(cpu, constants + 4u));
        ppc_fmuls(cpu, 2, 0, 0);
        ppc_fmuls(cpu, 0, 0, 4);
        {
            f64 result;
            if (ppc_fma(cpu, cpu->fpr[2], cpu->fpr[1], cpu->fpr[3], true, true, true, &result))
                cpu->fpr[2] = cpu->ps1[2] = result;
        }
        ppc_fmuls(cpu, 0, 2, 0);
        ppc_fmuls(cpu, 1, 1, 0);
        cycles += 7;
    }
    cpu->downcount -= cycles;
    cpu->cycle_observation_suffix = 5u;
    cpu->pc = cpu->lr & ~3u;
    return 1;
}

int bluewake_native_vec(CPUState* cpu, u32 address) {
    int which, done;
    switch (address) {
    case BLUEWAKE_PSVEC_ADD: which = VEC_ADD; done = vec_add_sub(cpu, false); break;
    case BLUEWAKE_PSVEC_SUBTRACT: which = VEC_SUBTRACT; done = vec_add_sub(cpu, true); break;
    case BLUEWAKE_PSVEC_SCALE: which = VEC_SCALE; done = vec_scale(cpu); break;
    case BLUEWAKE_PSVEC_SQUARE_MAG: which = VEC_SQUARE_MAG; done = vec_square_mag(cpu); break;
    case BLUEWAKE_PSVEC_DOT_PRODUCT: which = VEC_DOT; done = vec_dot(cpu); break;
    case BLUEWAKE_PSVEC_CROSS_PRODUCT: which = VEC_CROSS; done = vec_cross(cpu); break;
    case BLUEWAKE_PSVEC_SQUARE_DISTANCE: which = VEC_SQUARE_DISTANCE; done = vec_square_distance(cpu); break;
    case BLUEWAKE_PSVEC_NORMALIZE: which = VEC_NORMALIZE; done = vec_normalize(cpu); break;
    case BLUEWAKE_PSVEC_MAG: which = VEC_MAG; done = vec_mag(cpu); break;
    default: return 0;
    }
    if (done)
        s_vec_runs[which]++;
    else
        s_vec_declined[which]++;
    return done;
}

/* --- PSMTXMultVecSR (0x8030DB24): the scale and rotation of a matrix times a
 * vector, added with the second set of natives. Its entry is hooked by
 * scripts/windows/native_entries.py where its translation is the one
 * tests/native_vec_sr_test.c compared it against; the direct calls above do
 * not route to it. Separate from bluewake_native_vec and its report.
 *
 * One block of 21 cycles: nine psq_l, three ps_mul, three ps_sum0, three
 * ps_madd, three single psq_st. When all fifteen floats are bounded (finite,
 * below 2^62: every product, sum and single rounding stays finite, so each
 * operation takes the inline path), the operations run on values, as the
 * leaves above do. Otherwise, as PSVECNormalize above, the translation's own
 * statements with the block machinery taken out, on the registers themselves
 * (inline_fp.h's helpers, or the interpreter's where they hand over), so
 * every value, FPRF and flag is theirs whatever the operands: the
 * interpreter only sets FPSCR flags, so nothing can stop the block part of
 * the way. Every load comes before the first store, so the output may
 * overlap the inputs. It declines, changing nothing, unless FP is available,
 * paired singles unscaled (GQR0 type 0, HID2 LSQE), no write journal, all
 * three ranges plain RAM, no exception pending, the turn's budget not spent
 * and the next deadline beyond the block. The last access (the third store)
 * leaves its cycle suffix, 1. */
int bluewake_native_vec_sr_enabled;
static unsigned long long s_vec_sr_runs, s_vec_sr_declined;

void bluewake_native_vec_sr_report(void) {
    fprintf(stderr, "[native-vec] multvec-sr=%llu/%llu (native/declined)\n", s_vec_sr_runs, s_vec_sr_declined);
}

/* The value path's helpers, always inline: the leaves above share theirs
 * (vec_*), which clang keeps out of line here, and a call per operation, with
 * its pair returned through memory, cost this leaf most of its time. Each is
 * the vec_ helper of the same name, on a pointer into RAM. */
#define SR_INLINE static inline __attribute__((always_inline))

SR_INLINE bool sr_bounded(const u8* p, unsigned count) {
    bool ok = true;
    for (unsigned i = 0; i < count; ++i)
        ok &= vec_bounded(read_be32(p + 4u * i));
    return ok;
}

SR_INLINE VecPair sr_psq_l(const u8* p, bool w) {
    return (VecPair){vec_single(read_be32(p)), w ? 1.0 : vec_single(read_be32(p + 4u))};
}

SR_INLINE VecPair sr_result(const CPUState* cpu, f64 r0, f64 r1, u32* fprf) {
    const f32 s0 = bw_fp_single(cpu, r0), s1 = bw_fp_single(cpu, r1);
    *fprf = bw_fp_class32(s0);
    return (VecPair){(f64)s0, (f64)s1};
}

SR_INLINE VecPair sr_mul(const CPUState* cpu, VecPair a, VecPair c, u32* fprf) {
    return sr_result(cpu, a.p0 * bw_fp_25bit(c.p0), a.p1 * bw_fp_25bit(c.p1), fprf);
}

/* ps_sum0 d, a, a, a: a0 + a1 in the first half, a1 carried. */
SR_INLINE VecPair sr_sum0(const CPUState* cpu, VecPair a, u32* fprf) {
    const f32 s0 = bw_fp_single(cpu, a.p0 + a.p1), s1 = bw_fp_single(cpu, a.p1);
    *fprf = bw_fp_class32(s0);
    return (VecPair){(f64)s0, (f64)s1};
}

SR_INLINE VecPair sr_madd(const CPUState* cpu, VecPair a, VecPair c, VecPair b, u32* fprf) {
    return sr_result(cpu, bw_fp_fma_single(a.p0, bw_fp_25bit(c.p0), b.p0),
                     bw_fp_fma_single(a.p1, bw_fp_25bit(c.p1), b.p1), fprf);
}

/* The operations on values, every operand bounded. */
static int sr_values(CPUState* cpu, const u8* m, const u8* v, u32 out) {
    u32 fprf = 0;
    const VecPair f0 = sr_psq_l(m, false), f6 = sr_psq_l(v, false), f2 = sr_psq_l(m + 16, false);
    VecPair f8 = sr_mul(cpu, f0, f6, &fprf);
    const VecPair f4 = sr_psq_l(m + 32, false);
    VecPair f10 = sr_mul(cpu, f2, f6, &fprf);
    const VecPair f7 = sr_psq_l(v + 8, true);
    VecPair f12 = sr_mul(cpu, f4, f6, &fprf);
    const VecPair f3 = sr_psq_l(m + 24, false);
    f8 = sr_sum0(cpu, f8, &fprf);
    const VecPair f5 = sr_psq_l(m + 40, false);
    f10 = sr_sum0(cpu, f10, &fprf);
    const VecPair f1 = sr_psq_l(m + 8, false);
    f12 = sr_sum0(cpu, f12, &fprf);
    const VecPair f9 = sr_madd(cpu, f1, f7, f8, &fprf);
    const VecPair f11 = sr_madd(cpu, f3, f7, f10, &fprf);
    const VecPair f13 = sr_madd(cpu, f5, f7, f12, &fprf);
    /* The stores in order, after every load. */
    vec_store32(cpu, out, convert_to_single_ftz(f64_bits(f9.p0)));
    vec_store32(cpu, out + 4u, convert_to_single_ftz(f64_bits(f11.p0)));
    vec_store32(cpu, out + 8u, convert_to_single_ftz(f64_bits(f13.p0)));
    const VecPair results[14] = {f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13};
    for (unsigned r = 0; r < 14; ++r) {
        cpu->fpr[r] = results[r].p0;
        cpu->ps1[r] = results[r].p1;
    }
    return vec_finish(cpu, 21, 1, fprf);
}

static int vec_mult_sr(CPUState* cpu) {
    const u32 m = cpu->gpr[3], v = cpu->gpr[4], out = cpu->gpr[5];
    const u32 gqr = cpu->gqr[0];
    if (cpu->exception != 0u || (cpu->msr & PPC_MSR_FP) == 0u || (cpu->hid2 & PPC_HID2_LSQE) == 0u ||
        ((gqr >> 16) & 7u) != 0u || (gqr & 7u) != 0u || g_mem_write_journal != NULL || cpu->cycle_budget <= 0 ||
        cpu->downcount <= -cpu->cycle_budget ||
        (cpu->cycle_deadline_budget > 0 &&
         (cpu->cycle_deadline_budget < 21 || cpu->cycle_deadline_budget + cpu->downcount < 21)) ||
        !vec_ram(cpu, m, 48) || !vec_ram(cpu, v, 12) || !vec_ram(cpu, out, 12))
        return 0;
    const u8* matrix = cpu->ram + (m - GC_RAM_BASE);
    const u8* vector = cpu->ram + (v - GC_RAM_BASE);
    if (sr_bounded(matrix, 12) && sr_bounded(vector, 3))
        return sr_values(cpu, matrix, vector, out);
    vec_set(cpu, 0, vec_psq_l(cpu, m, false));
    vec_set(cpu, 6, vec_psq_l(cpu, v, false));
    vec_set(cpu, 2, vec_psq_l(cpu, m + 16u, false));
    ppc_ps_mul_op(cpu, 8, 0, 6);
    vec_set(cpu, 4, vec_psq_l(cpu, m + 32u, false));
    ppc_ps_mul_op(cpu, 10, 2, 6);
    vec_set(cpu, 7, vec_psq_l(cpu, v + 8u, true));
    ppc_ps_mul_op(cpu, 12, 4, 6);
    vec_set(cpu, 3, vec_psq_l(cpu, m + 24u, false));
    ppc_ps_sum0(cpu, 8, 8, 8, 8);
    vec_set(cpu, 5, vec_psq_l(cpu, m + 40u, false));
    ppc_ps_sum0(cpu, 10, 10, 10, 10);
    vec_set(cpu, 1, vec_psq_l(cpu, m + 8u, false));
    ppc_ps_sum0(cpu, 12, 12, 12, 12);
    ppc_ps_madd_op(cpu, 9, 1, 7, 8, false, false);
    vec_psq_st(cpu, out, (VecPair){cpu->fpr[9], cpu->ps1[9]}, true);
    ppc_ps_madd_op(cpu, 11, 3, 7, 10, false, false);
    vec_psq_st(cpu, out + 4u, (VecPair){cpu->fpr[11], cpu->ps1[11]}, true);
    ppc_ps_madd_op(cpu, 13, 5, 7, 12, false, false);
    vec_psq_st(cpu, out + 8u, (VecPair){cpu->fpr[13], cpu->ps1[13]}, true);
    cpu->downcount -= 21;
    cpu->cycle_observation_suffix = 1u;
    cpu->pc = cpu->lr & ~3u;
    return 1;
}

int bluewake_native_vec_sr(CPUState* cpu) {
    const int done = vec_mult_sr(cpu);
    if (done)
        s_vec_sr_runs++;
    else
        s_vec_sr_declined++;
    return done;
}
