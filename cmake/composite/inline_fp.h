#ifndef BLUEWAKE_COMPOSITE_INLINE_FP_H
#define BLUEWAKE_COMPOSITE_INLINE_FP_H

/* The common floating-point instructions, inline in the translated chunks
 * (scripts/windows/chunk_headers.py includes this in every chunk).
 *
 * The translator emits each of these as a call into GXRuntime's interpreter
 * (cpu_interpreter_float.c) with register numbers as arguments: fadds, fsubs,
 * fmuls, fmul, fadd, fsub, fdivs and fcmp are about 90,000 call sites, and the
 * collision, animation and matrix code the game spends its frame in is mostly
 * made of them. The fused multiply-adds (ppc_fma) and the paired-single
 * arithmetic the matrix library (PSMTX*, PSVEC*) is written in are here too. A call costs its frame, the indexed register accesses, and the
 * nested classify and FPRF calls; inline, with the register numbers constant,
 * each is a handful of instructions on fixed fields.
 *
 * Each fast path is the interpreter function for operands it handles without
 * a special case - finite values (and a nonzero divisor, and no NaN compare) -
 * written out: the same arithmetic, the same rounding (force_single, the
 * 25-bit multiplier), the same writes to the registers and the FPSCR. Anything
 * else calls the interpreter function itself, which starts from the same
 * unmodified state. So every result, flag and register is the interpreter's.
 *
 * No identifier here may be `ctx`: the chunks define it as a macro. */

#include "core/cpu.h"

#include <math.h>
#include <string.h>

#define BW_FP_FPSCR_FR 0x00040000u  /* cpu_interpreter_private.h */
#define BW_FP_FPSCR_FI 0x00020000u
#define BW_FP_FPSCR_NI 0x00000004u

static inline u64 bw_fp_bits(f64 value) {
    u64 bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static inline bool bw_fp_finite(f64 value) {
    return (bw_fp_bits(value) & 0x7FF0000000000000ull) != 0x7FF0000000000000ull;
}

/* force_single */
static inline f32 bw_fp_single(const CPUState* cpu, f64 value) {
    if (cpu->fpscr & BW_FP_FPSCR_NI) {
        const u64 bits = bw_fp_bits(value);
        if ((bits & 0x7FFFFFFFFFFFFFFFull) < 0x3810000000000000ull) {
            const u32 flushed = (u32)((bits & 0x8000000000000000ull) >> 32);
            f32 zero;
            memcpy(&zero, &flushed, sizeof zero);
            return zero;
        }
    }
    return (f32)value;
}

/* force_25bit_c */
static inline f64 bw_fp_25bit(f64 d) {
    u64 integral = bw_fp_bits(d);
    const u64 exponent = integral & 0x7FF0000000000000ull;
    const u64 fraction = integral & 0x000FFFFFFFFFFFFFull;
    if (exponent == 0 && fraction != 0) {
        s64 keep_mask = (s64)0xFFFFFFFFF8000000ll;
        u64 round = 0x8000000u;
        const unsigned shift = (unsigned)__builtin_clzll(fraction) - 11u;
        keep_mask >>= shift;
        round >>= shift;
        integral = (integral & (u64)keep_mask) + (integral & round);
    } else {
        integral = (integral & 0xFFFFFFFFF8000000ull) + (integral & 0x8000000ull);
    }
    f64 value;
    memcpy(&value, &integral, sizeof value);
    return value;
}

/* classify_f32, classify_f64 */
static inline u32 bw_fp_class32(f32 value) {
    u32 bits;
    memcpy(&bits, &value, sizeof bits);
    const u32 sign = bits >> 31;
    const u32 exponent = bits & 0x7F800000u;
    const u32 fraction = bits & 0x007FFFFFu;
    if (exponent == 0x7F800000u)
        return fraction ? 0x11u : (sign ? 0x09u : 0x05u);
    if (exponent == 0)
        return fraction ? (sign ? 0x18u : 0x14u) : (sign ? 0x12u : 0x02u);
    return sign ? 0x08u : 0x04u;
}

static inline u32 bw_fp_class64(f64 value) {
    const u64 bits = bw_fp_bits(value);
    const u64 sign = bits >> 63;
    const u64 exponent = bits & 0x7FF0000000000000ull;
    const u64 fraction = bits & 0x000FFFFFFFFFFFFFull;
    if (exponent == 0x7FF0000000000000ull)
        return fraction ? 0x11u : (sign ? 0x09u : 0x05u);
    if (exponent == 0)
        return fraction ? (sign ? 0x18u : 0x14u) : (sign ? 0x12u : 0x02u);
    return sign ? 0x08u : 0x04u;
}

/* set_fprf */
static inline void bw_fp_fprf(CPUState* cpu, u32 value) {
    cpu->fpscr = (cpu->fpscr & ~(0x1Fu << 12)) | ((value & 0x1Fu) << 12);
}

/* fp_write_single, fp_write_double */
static inline void bw_fp_write_single(CPUState* cpu, u8 d, f32 rounded) {
    cpu->fpr[d] = (f64)rounded;
    cpu->ps1[d] = (f64)rounded;
    bw_fp_fprf(cpu, bw_fp_class32(rounded));
}

static inline void bw_fp_write_double(CPUState* cpu, u8 d, f64 value) {
    cpu->fpr[d] = value;
    bw_fp_fprf(cpu, bw_fp_class64(value));
}

/* ppc_fadds, ppc_fsubs: ni_add/ni_sub of finite operands is the plain sum,
 * with no exception and no FI/FR change. */
static inline void bw_fp_fadds(CPUState* cpu, u8 d, u8 a, u8 b) {
    const f64 x = cpu->fpr[a], y = cpu->fpr[b];
    if (__builtin_expect(!(bw_fp_finite(x) && bw_fp_finite(y)), 0)) {
        ppc_fadds(cpu, d, a, b);
        return;
    }
    bw_fp_write_single(cpu, d, bw_fp_single(cpu, x + y));
}

static inline void bw_fp_fsubs(CPUState* cpu, u8 d, u8 a, u8 b) {
    const f64 x = cpu->fpr[a], y = cpu->fpr[b];
    if (__builtin_expect(!(bw_fp_finite(x) && bw_fp_finite(y)), 0)) {
        ppc_fsubs(cpu, d, a, b);
        return;
    }
    bw_fp_write_single(cpu, d, bw_fp_single(cpu, x - y));
}

/* ppc_fadd, ppc_fsub (double results; force_double is the identity). */
static inline void bw_fp_fadd(CPUState* cpu, u8 d, u8 a, u8 b) {
    const f64 x = cpu->fpr[a], y = cpu->fpr[b];
    if (__builtin_expect(!(bw_fp_finite(x) && bw_fp_finite(y)), 0)) {
        ppc_fadd(cpu, d, a, b);
        return;
    }
    bw_fp_write_double(cpu, d, x + y);
}

static inline void bw_fp_fsub(CPUState* cpu, u8 d, u8 a, u8 b) {
    const f64 x = cpu->fpr[a], y = cpu->fpr[b];
    if (__builtin_expect(!(bw_fp_finite(x) && bw_fp_finite(y)), 0)) {
        ppc_fsub(cpu, d, a, b);
        return;
    }
    bw_fp_write_double(cpu, d, x - y);
}

/* ppc_fmuls: the multiplier rounded to 25 bits, then ni_mul. A NaN product
 * (which finite operands give only through a rounded multiplier of infinity
 * times zero) is ni_mul's case, so it goes to the interpreter. */
static inline void bw_fp_fmuls(CPUState* cpu, u8 d, u8 a, u8 c) {
    const f64 x = cpu->fpr[a], y = cpu->fpr[c];
    if (__builtin_expect(bw_fp_finite(x) && bw_fp_finite(y), 1)) {
        const f64 product = x * bw_fp_25bit(y);
        if (__builtin_expect(product == product, 1)) {
            bw_fp_write_single(cpu, d, bw_fp_single(cpu, product));
            cpu->fpscr &= ~(BW_FP_FPSCR_FI | BW_FP_FPSCR_FR);
            return;
        }
    }
    ppc_fmuls(cpu, d, a, c);
}

/* ppc_fmul */
static inline void bw_fp_fmul(CPUState* cpu, u8 d, u8 a, u8 c) {
    const f64 x = cpu->fpr[a], y = cpu->fpr[c];
    if (__builtin_expect(!(bw_fp_finite(x) && bw_fp_finite(y)), 0)) {
        ppc_fmul(cpu, d, a, c);
        return;
    }
    bw_fp_write_double(cpu, d, x * y);
    cpu->fpscr &= ~(BW_FP_FPSCR_FI | BW_FP_FPSCR_FR);
}

/* ppc_fdivs: ni_div of finite operands with a nonzero divisor is the plain
 * quotient (an overflow to infinity included), with no exception. */
static inline void bw_fp_fdivs(CPUState* cpu, u8 d, u8 a, u8 b) {
    const f64 x = cpu->fpr[a], y = cpu->fpr[b];
    if (__builtin_expect(!(bw_fp_finite(x) && bw_fp_finite(y)) || y == 0.0, 0)) {
        ppc_fdivs(cpu, d, a, b);
        return;
    }
    bw_fp_write_single(cpu, d, bw_fp_single(cpu, x / y));
}

/* ppc_fcmp without a NaN operand: no exception, the FPCC bits or'ed into the
 * FPSCR as the interpreter does, and the CR field. */
static inline void bw_fp_fcmp(CPUState* cpu, u8 crfd, f64 a, f64 b, bool ordered) {
    if (__builtin_expect(a != a || b != b, 0)) {
        ppc_fcmp(cpu, crfd, a, b, ordered);
        return;
    }
    const u32 compare = a < b ? 0x8u : (a > b ? 0x4u : 0x2u);
    cpu->fpscr |= compare << 12;
    const u32 shift = 4u * (7u - crfd);
    cpu->cr = (cpu->cr & ~(0xFu << shift)) | (compare << shift);
}

/* force_25_bit (ppc_fma's multiplier rounding; not force_25bit_c) */
static inline f64 bw_fp_25bit_fma(f64 value) {
    u64 bits = bw_fp_bits(value);
    const u64 fraction = bits & 0x000FFFFFFFFFFFFFull;
    u64 keep_mask = 0xFFFFFFFFF8000000ull;
    u64 round = 0x0000000008000000ull;
    if ((bits & 0x7FF0000000000000ull) == 0 && fraction != 0) {
        const unsigned shift = (unsigned)__builtin_clzll(fraction) - 11u;
        if (shift < 28) {
            keep_mask = ~((1ull << (27 - shift)) - 1);
            round >>= shift;
        } else {
            keep_mask = ~0ull;
            round = 0;
        }
    }
    bits = (bits & keep_mask) + (bits & round);
    f64 result;
    memcpy(&result, &bits, sizeof result);
    return result;
}

/* The single-precision fused multiply-add's tie correction, as ppc_fma and
 * ni_madd_msub both write it. */
static inline f64 bw_fp_fma_single(f64 a, f64 c_round, f64 addend) {
    f64 result = fma(a, c_round, addend);
    u64 bits = bw_fp_bits(result);
    if ((bits & 0x000000001FFFFFFFull) == 0x0000000010000000ull) {
        const f64 a_prime = addend - result;
        const f64 b_prime = result + a_prime;
        const f64 delta_a = fma(a, c_round, a_prime);
        const f64 delta_b = addend - b_prime;
        const f64 error = delta_a + delta_b;
        if (error != 0.0) {
            if ((error > 0.0) == (result > 0.0))
                bits++;
            else
                bits--;
            memcpy(&result, &bits, sizeof result);
        }
    }
    return result;
}

/* ppc_fma (fmadd, fmsub, fnmadd, fnmsub and their single forms) for finite
 * operands: finite operands never give a NaN except through a multiplier
 * rounded up to infinity, which the interpreter handles. */
static inline bool bw_fp_fma(CPUState* cpu, f64 a, f64 c, f64 b, bool single,
                             bool subtract, bool negative, f64* output) {
    if (__builtin_expect(bw_fp_finite(a) && bw_fp_finite(c) && bw_fp_finite(b), 1)) {
        const f64 addend = subtract ? -b : b;
        f64 result = single ? bw_fp_fma_single(a, bw_fp_25bit_fma(c), addend) : fma(a, c, addend);
        if (single)
            result = (f64)(f32)result;
        if (__builtin_expect(result == result, 1)) {
            if (negative)
                result = -result;
            bw_fp_fprf(cpu, single ? bw_fp_class32((f32)result) : bw_fp_class64(result));
            *output = result;
            return true;
        }
    }
    return ppc_fma(cpu, a, c, b, single, subtract, negative, output);
}

/* Paired singles. ni_madd_msub(single) of finite operands, when it is not a
 * NaN: false sends the instruction to the interpreter. */
static inline bool bw_fp_ps_madd(f64 a, f64 c, f64 b, bool subtract, f64* out) {
    *out = bw_fp_fma_single(a, bw_fp_25bit(c), subtract ? -b : b);
    return *out == *out;
}

/* ps_write_both and the FPRF of the half it names. */
static inline void bw_fp_ps_write(CPUState* cpu, u8 d, f32 ps0, f32 ps1, f32 fprf_of) {
    cpu->fpr[d] = (f64)ps0;
    cpu->ps1[d] = (f64)ps1;
    bw_fp_fprf(cpu, bw_fp_class32(fprf_of));
}

#define BW_FP_FINITE4(w, x, y, z) \
    (bw_fp_finite(w) && bw_fp_finite(x) && bw_fp_finite(y) && bw_fp_finite(z))
#define BW_FP_FINITE6(u, v, w, x, y, z) (BW_FP_FINITE4(u, v, w, x) && bw_fp_finite(y) && bw_fp_finite(z))

static inline void bw_fp_ps_madds0(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    const f64 a0 = cpu->fpr[a], a1 = cpu->ps1[a], c0 = cpu->fpr[c], b0 = cpu->fpr[b], b1 = cpu->ps1[b];
    f64 r0, r1;
    if (__builtin_expect(BW_FP_FINITE4(a0, a1, c0, b0) && bw_fp_finite(b1) &&
                             bw_fp_ps_madd(a0, c0, b0, false, &r0) && bw_fp_ps_madd(a1, c0, b1, false, &r1), 1)) {
        const f32 s0 = bw_fp_single(cpu, r0), s1 = bw_fp_single(cpu, r1);
        bw_fp_ps_write(cpu, d, s0, s1, s0);
        return;
    }
    ppc_ps_madds0(cpu, d, a, c, b);
}

static inline void bw_fp_ps_madds1(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    const f64 a0 = cpu->fpr[a], a1 = cpu->ps1[a], c1 = cpu->ps1[c], b0 = cpu->fpr[b], b1 = cpu->ps1[b];
    f64 r0, r1;
    if (__builtin_expect(BW_FP_FINITE4(a0, a1, c1, b0) && bw_fp_finite(b1) &&
                             bw_fp_ps_madd(a0, c1, b0, false, &r0) && bw_fp_ps_madd(a1, c1, b1, false, &r1), 1)) {
        const f32 s0 = bw_fp_single(cpu, r0), s1 = bw_fp_single(cpu, r1);
        bw_fp_ps_write(cpu, d, s0, s1, s0);
        return;
    }
    ppc_ps_madds1(cpu, d, a, c, b);
}

/* ppc_ps_madd_op: ps_madd, ps_msub, ps_nmadd, ps_nmsub. */
static inline void bw_fp_ps_madd_op(CPUState* cpu, u8 d, u8 a, u8 c, u8 b, bool subtract, bool negative) {
    const f64 a0 = cpu->fpr[a], a1 = cpu->ps1[a], c0 = cpu->fpr[c], c1 = cpu->ps1[c];
    const f64 b0 = cpu->fpr[b], b1 = cpu->ps1[b];
    f64 r0, r1;
    if (__builtin_expect(BW_FP_FINITE6(a0, a1, c0, c1, b0, b1) && bw_fp_ps_madd(a0, c0, b0, subtract, &r0) &&
                             bw_fp_ps_madd(a1, c1, b1, subtract, &r1), 1)) {
        f32 s0 = bw_fp_single(cpu, r0), s1 = bw_fp_single(cpu, r1);
        if (negative) {
            s0 = -s0;
            s1 = -s1;
        }
        bw_fp_ps_write(cpu, d, s0, s1, s0);
        return;
    }
    ppc_ps_madd_op(cpu, d, a, c, b, subtract, negative);
}

static inline void bw_fp_ps_mul_op(CPUState* cpu, u8 d, u8 a, u8 c) {
    const f64 a0 = cpu->fpr[a], a1 = cpu->ps1[a], c0 = cpu->fpr[c], c1 = cpu->ps1[c];
    if (__builtin_expect(BW_FP_FINITE4(a0, a1, c0, c1), 1)) {
        const f64 p0 = a0 * bw_fp_25bit(c0), p1 = a1 * bw_fp_25bit(c1);
        if (__builtin_expect(p0 == p0 && p1 == p1, 1)) {
            const f32 s0 = bw_fp_single(cpu, p0), s1 = bw_fp_single(cpu, p1);
            bw_fp_ps_write(cpu, d, s0, s1, s0);
            return;
        }
    }
    ppc_ps_mul_op(cpu, d, a, c);
}

static inline void bw_fp_ps_muls0(CPUState* cpu, u8 d, u8 a, u8 c) {
    const f64 a0 = cpu->fpr[a], a1 = cpu->ps1[a], c0 = cpu->fpr[c];
    if (__builtin_expect(bw_fp_finite(a0) && bw_fp_finite(a1) && bw_fp_finite(c0), 1)) {
        const f64 c_round = bw_fp_25bit(c0);
        const f64 p0 = a0 * c_round, p1 = a1 * c_round;
        if (__builtin_expect(p0 == p0 && p1 == p1, 1)) {
            const f32 s0 = bw_fp_single(cpu, p0), s1 = bw_fp_single(cpu, p1);
            bw_fp_ps_write(cpu, d, s0, s1, s0);
            return;
        }
    }
    ppc_ps_muls0(cpu, d, a, c);
}

static inline void bw_fp_ps_muls1(CPUState* cpu, u8 d, u8 a, u8 c) {
    const f64 a0 = cpu->fpr[a], a1 = cpu->ps1[a], c1 = cpu->ps1[c];
    if (__builtin_expect(bw_fp_finite(a0) && bw_fp_finite(a1) && bw_fp_finite(c1), 1)) {
        const f64 c_round = bw_fp_25bit(c1);
        const f64 p0 = a0 * c_round, p1 = a1 * c_round;
        if (__builtin_expect(p0 == p0 && p1 == p1, 1)) {
            const f32 s0 = bw_fp_single(cpu, p0), s1 = bw_fp_single(cpu, p1);
            bw_fp_ps_write(cpu, d, s0, s1, s0);
            return;
        }
    }
    ppc_ps_muls1(cpu, d, a, c);
}

static inline void bw_fp_ps_add_op(CPUState* cpu, u8 d, u8 a, u8 b) {
    const f64 a0 = cpu->fpr[a], a1 = cpu->ps1[a], b0 = cpu->fpr[b], b1 = cpu->ps1[b];
    if (__builtin_expect(!BW_FP_FINITE4(a0, a1, b0, b1), 0)) {
        ppc_ps_add_op(cpu, d, a, b);
        return;
    }
    const f32 s0 = bw_fp_single(cpu, a0 + b0), s1 = bw_fp_single(cpu, a1 + b1);
    bw_fp_ps_write(cpu, d, s0, s1, s0);
}

static inline void bw_fp_ps_sub_op(CPUState* cpu, u8 d, u8 a, u8 b) {
    const f64 a0 = cpu->fpr[a], a1 = cpu->ps1[a], b0 = cpu->fpr[b], b1 = cpu->ps1[b];
    if (__builtin_expect(!BW_FP_FINITE4(a0, a1, b0, b1), 0)) {
        ppc_ps_sub_op(cpu, d, a, b);
        return;
    }
    const f32 s0 = bw_fp_single(cpu, a0 - b0), s1 = bw_fp_single(cpu, a1 - b1);
    bw_fp_ps_write(cpu, d, s0, s1, s0);
}

/* ps_sum0: the sum in the first half, c's second half carried; ps_sum1 the
 * other way round, with the FPRF of the sum's half. */
static inline void bw_fp_ps_sum0(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    const f64 a0 = cpu->fpr[a], b1 = cpu->ps1[b], c1 = cpu->ps1[c];
    if (__builtin_expect(!(bw_fp_finite(a0) && bw_fp_finite(b1)), 0)) {
        ppc_ps_sum0(cpu, d, a, c, b);
        return;
    }
    const f32 s0 = bw_fp_single(cpu, a0 + b1), s1 = bw_fp_single(cpu, c1);
    bw_fp_ps_write(cpu, d, s0, s1, s0);
}

static inline void bw_fp_ps_sum1(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    const f64 a0 = cpu->fpr[a], b1 = cpu->ps1[b], c0 = cpu->fpr[c];
    if (__builtin_expect(!(bw_fp_finite(a0) && bw_fp_finite(b1)), 0)) {
        ppc_ps_sum1(cpu, d, a, c, b);
        return;
    }
    const f32 s0 = bw_fp_single(cpu, c0), s1 = bw_fp_single(cpu, a0 + b1);
    bw_fp_ps_write(cpu, d, s0, s1, s1);
}

/* lfs, lfsx, lfsu and the other single loads: the generated header widens the
 * single's bits by hand so that a signalling NaN keeps its payload and stays
 * signalling. For a normal single (exponent 1-254) the hardware's widening is
 * the same double: the exponent rebiased, the fraction shifted, the sign kept.
 * Zeros, denormals, infinities and NaNs take the generated path
 * (gather_pipe.h renames it ahead of the generated header; tests/f32_from_bits_test.c
 * compares the two over every bit pattern). */
#if defined(RECOMP_COMPOSITE_H) && defined(dolrecomp_f32_from_bits)
#undef dolrecomp_f32_from_bits
static inline f64 dolrecomp_f32_from_bits(u32 bits) {
    if (__builtin_expect(((bits >> 23) & 0xFFu) - 1u < 254u, 1)) {
        f32 single;
        memcpy(&single, &bits, sizeof single);
        return (f64)single;
    }
    return bw_generated_f32_from_bits(bits);
}
#endif

#define ppc_fadds bw_fp_fadds
#define ppc_fsubs bw_fp_fsubs
#define ppc_fadd bw_fp_fadd
#define ppc_fsub bw_fp_fsub
#define ppc_fmuls bw_fp_fmuls
#define ppc_fmul bw_fp_fmul
#define ppc_fdivs bw_fp_fdivs
#define ppc_fcmp bw_fp_fcmp
#define ppc_fma bw_fp_fma
#define ppc_ps_madds0 bw_fp_ps_madds0
#define ppc_ps_madds1 bw_fp_ps_madds1
#define ppc_ps_madd_op bw_fp_ps_madd_op
#define ppc_ps_mul_op bw_fp_ps_mul_op
#define ppc_ps_muls0 bw_fp_ps_muls0
#define ppc_ps_muls1 bw_fp_ps_muls1
#define ppc_ps_add_op bw_fp_ps_add_op
#define ppc_ps_sub_op bw_fp_ps_sub_op
#define ppc_ps_sum0 bw_fp_ps_sum0
#define ppc_ps_sum1 bw_fp_ps_sum1

#endif
