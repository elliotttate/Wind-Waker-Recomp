#ifndef BLUEWAKE_NATIVE_REPLAY_H
#define BLUEWAKE_NATIVE_REPLAY_H

/* The fourth set of natives' shared pieces (native_kankyo.c, native_anim.c,
 * native_cc.c): the translation's floating-point instructions replayed on
 * values, its cycle accounting replayed on a running downcount, and plain
 * RAM.
 *
 * The floating-point operations are inline_fp.h's inline paths, the very
 * expressions the translated chunks compute (the interpreter's single
 * rounding with NI's flush, the 25-bit multiplier, the fused multiply-add's
 * tie correction), applied to local values and a local FPSCR instead of the
 * guest registers. Where the translation would leave its inline path for the
 * interpreter (a NaN or infinite operand, a NaN product, a zero divisor), the
 * replay marks itself bad and the native declines: it has changed nothing
 * yet, since a native commits its registers and stores only once the whole
 * function has been replayed. So whatever a native accepts - denormals and
 * huge values included - it computes exactly as the translation does, with
 * the same host operations under the same MXCSR. fctiwz is GXRuntime's
 * ppc_fctiw, written out on the local FPSCR (its exception bits, FX, VX and
 * FEX as set_fp_exception and ppc_fpscr_updated leave them).
 *
 * The clock replays the blocks a path enters: each leader's budget test (the
 * translation returns to the chassis loop there) and its precharge test
 * (dolrecomp_block_can_precharge: otherwise the block runs instruction by
 * instruction, its accesses storing cycle suffix 0). A native accepts only
 * paths on which every block is prepaid and nothing returns for the budget.
 * With the downcount at most zero on entry, a prepaid copy's deadline refund
 * (a suffix beyond the deadline) cannot happen either: the precharge test
 * left the deadline at least a block's cycles away, and a suffix is less.
 *
 * No identifier here may be `ctx`. */

#include "core/cpu.h"
#include "inline_fp.h"

#include <math.h>

#define NR static inline __attribute__((always_inline))

#define NR_FPSCR_FX 0x80000000u
#define NR_FPSCR_FEX 0x40000000u
#define NR_FPSCR_VX 0x20000000u
#define NR_FPSCR_XX 0x02000000u
#define NR_FPSCR_VXSNAN 0x01000000u
#define NR_FPSCR_FR 0x00040000u
#define NR_FPSCR_FI 0x00020000u
#define NR_FPSCR_VXCVI 0x00000100u
#define NR_FPSCR_VE 0x00000080u
#define NR_FPSCR_NI 0x00000004u
#define NR_FPSCR_VX_ANY 0x01F80700u /* VXSNAN VXISI VXIDI VXZDZ VXIMZ VXVC VXSOFT VXSQRT VXCVI */

/* --- Plain RAM. ----------------------------------------------------------- */

/* `size` bytes at `address` in MEM1 proper, unaliased: what the translation's
 * fast loads and stores reach without leaving line. */
NR bool nr_ram(const CPUState* cpu, u32 address, u32 size) {
    return ppc_dispatch_poll_read_stable((CPUState*)cpu, address, size);
}

NR u8* nr_at(const CPUState* cpu, u32 address) { return cpu->ram + (address - GC_RAM_BASE); }
NR u32 nr_word(const CPUState* cpu, u32 address) { return read_be32(nr_at(cpu, address)); }
NR u16 nr_half(const CPUState* cpu, u32 address) { return read_be16(nr_at(cpu, address)); }
NR u64 nr_dword(const CPUState* cpu, u32 address) { return read_be64(nr_at(cpu, address)); }

NR bool nr_apart(u32 a, u32 a_size, u32 b, u32 b_size) {
    return (u64)a + a_size <= (u64)b || (u64)b + b_size <= (u64)a;
}

/* Stores, each clearing a reservation on its granule as the translation's do
 * (the slow path it takes while one is held: mem_writeN). Through a copy of
 * the RAM pointer, and with the reservation looked at only when one was held
 * at the start, so the compiler need not reload the guest state after each
 * store (a byte store may alias anything). */
typedef struct NrOut {
    CPUState* cpu;
    u8* ram;
    bool reserved;
} NrOut;

NR NrOut nr_out(CPUState* cpu) { return (NrOut){cpu, cpu->ram, cpu->reserve_valid}; }

NR void nr_clear(const NrOut* o, u32 address) {
    if (o->reserved)
        clear_matching_reservation(o->cpu, address);
}

NR void nr_store32(const NrOut* o, u32 address, u32 value) {
    nr_clear(o, address);
    write_be32(o->ram + (address - GC_RAM_BASE), value);
}

NR void nr_store64(const NrOut* o, u32 address, u64 value) {
    nr_clear(o, address);
    write_be64(o->ram + (address - GC_RAM_BASE), value);
}

NR void nr_store16(const NrOut* o, u32 address, u16 value) {
    nr_clear(o, address);
    write_be16(o->ram + (address - GC_RAM_BASE), value);
}

NR void nr_store8(const NrOut* o, u32 address, u8 value) {
    nr_clear(o, address);
    o->ram[address - GC_RAM_BASE] = value;
}

/* convert_to_double (types.h), always inline. */
NR u64 nr_to_double(u32 value) {
    const u64 x = value;
    u64 exp = (x >> 23) & 0xFFu;
    u64 frac = x & 0x007FFFFFu;
    if (exp > 0 && exp < 255) {
        const u64 y = !(exp >> 7);
        const u64 z = (y << 61) | (y << 60) | (y << 59);
        return ((x & 0xC0000000u) << 32) | z | ((x & 0x3FFFFFFFu) << 29);
    } else if (exp == 0 && frac != 0) {
        exp = 1023 - 126;
        do {
            frac <<= 1;
            exp -= 1;
        } while ((frac & 0x00800000u) == 0);
        return ((x & 0x80000000u) << 32) | (exp << 52) | ((frac & 0x007FFFFFu) << 29);
    } else {
        const u64 y = exp >> 7;
        const u64 z = (y << 61) | (y << 60) | (y << 59);
        return ((x & 0xC0000000u) << 32) | z | ((x & 0x3FFFFFFFu) << 29);
    }
}

/* convert_to_single_ftz (types.h), always inline. */
NR u32 nr_to_single_ftz(u64 x) {
    const u32 exp = (u32)((x >> 52) & 0x7FFu);
    if (exp > 896 || (x & ~0x8000000000000000ull) == 0)
        return (u32)(((x >> 32) & 0xC0000000u) | ((x >> 29) & 0x3FFFFFFFu));
    return (u32)((x >> 32) & 0x80000000u);
}

/* convert_to_single (types.h): stfs. */
NR u32 nr_to_single(u64 x) {
    const u32 exp = (u32)((x >> 52) & 0x7FFu);
    if (exp > 896 || (x & ~0x8000000000000000ull) == 0) {
        return (u32)(((x >> 32) & 0xC0000000u) | ((x >> 29) & 0x3FFFFFFFu));
    } else if (exp >= 874) {
        u32 t = (u32)(0x80000000u | ((x & 0x000FFFFFFFFFFFFFull) >> 21));
        t = t >> (905 - exp);
        t |= (u32)((x >> 32) & 0x80000000u);
        return t;
    } else {
        return (u32)(((x >> 32) & 0xC0000000u) | ((x >> 29) & 0x3FFFFFFFu));
    }
}

/* lfs, psq_l (type 0): a single's bits as the double the register holds. */
NR f64 nr_single_bits(u32 bits) { return f64_value(nr_to_double(bits)); }

/* psq_st (type 0) of one half, and psq_l of it back: what a paired-single
 * save and restore leaves in that half. */
NR f64 nr_pair_round_trip(f64 value) { return f64_value(nr_to_double(nr_to_single_ftz(f64_bits(value)))); }

/* --- The FPSCR. ---------------------------------------------------------- */

typedef struct NrFp {
    u32 fpscr;
    bool bad; /* the translation would have left its inline path */
} NrFp;

NR bool nr_finite(f64 value) {
    return (f64_bits(value) & 0x7FF0000000000000ull) != 0x7FF0000000000000ull;
}

/* force_25bit_c (bw_fp_25bit), always inline. */
NR f64 nr_25bit(f64 d) {
    u64 integral = f64_bits(d);
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
    return f64_value(integral);
}

/* force_25_bit (bw_fp_25bit_fma: the fused multiply-add's multiplier), always inline. */
NR f64 nr_25bit_fma(f64 value) {
    u64 bits = f64_bits(value);
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
    return f64_value(bits);
}

/* bw_fp_fma_single: the single fused multiply-add's tie correction, always inline. */
NR f64 nr_fma_single(f64 a, f64 c_round, f64 addend) {
    f64 result = fma(a, c_round, addend);
    u64 bits = f64_bits(result);
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
            result = f64_value(bits);
        }
    }
    return result;
}

/* classify_f32 (bw_fp_class32), always inline. */
NR u32 nr_class32(f32 value) {
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

/* ppc_fpscr_updated */
NR void nr_fpscr_updated(NrFp* s) {
    u32 f = s->fpscr;
    f = (f & ~NR_FPSCR_VX) | ((f & NR_FPSCR_VX_ANY) ? NR_FPSCR_VX : 0u);
    f = (f & ~NR_FPSCR_FEX) | ((((f >> 22) & f & 0xF8u) != 0u) ? NR_FPSCR_FEX : 0u);
    s->fpscr = f;
}

/* set_fp_exception */
NR void nr_fp_exception(NrFp* s, u32 bit) {
    if ((s->fpscr & bit) != bit)
        s->fpscr |= NR_FPSCR_FX;
    s->fpscr |= bit;
    nr_fpscr_updated(s);
}

NR void nr_fprf(NrFp* s, u32 value) { s->fpscr = (s->fpscr & ~(0x1Fu << 12)) | ((value & 0x1Fu) << 12); }

/* force_single (bw_fp_single), on the local FPSCR's NI. */
NR f32 nr_round(const NrFp* s, f64 value) {
    if (s->fpscr & NR_FPSCR_NI) {
        const u64 bits = f64_bits(value);
        if ((bits & 0x7FFFFFFFFFFFFFFFull) < 0x3810000000000000ull) {
            const u32 flushed = (u32)((bits & 0x8000000000000000ull) >> 32);
            f32 zero;
            memcpy(&zero, &flushed, sizeof zero);
            return zero;
        }
    }
    return (f32)value;
}

/* --- The arithmetic: inline_fp.h's inline paths. ------------------------- */

/* fadds, fsubs: the result in both halves. */
NR f64 nr_fadds(NrFp* s, f64 x, f64 y) {
    if (!(nr_finite(x) && nr_finite(y))) {
        s->bad = true;
        return 0.0;
    }
    const f32 r = nr_round(s, x + y);
    nr_fprf(s, nr_class32(r));
    return (f64)r;
}

NR f64 nr_fsubs(NrFp* s, f64 x, f64 y) {
    if (!(nr_finite(x) && nr_finite(y))) {
        s->bad = true;
        return 0.0;
    }
    const f32 r = nr_round(s, x - y);
    nr_fprf(s, nr_class32(r));
    return (f64)r;
}

/* fmuls d, a, c */
NR f64 nr_fmuls(NrFp* s, f64 a, f64 c) {
    if (!(nr_finite(a) && nr_finite(c))) {
        s->bad = true;
        return 0.0;
    }
    const f64 product = a * nr_25bit(c);
    if (product != product) {
        s->bad = true;
        return 0.0;
    }
    const f32 r = nr_round(s, product);
    nr_fprf(s, nr_class32(r));
    s->fpscr &= ~(NR_FPSCR_FI | NR_FPSCR_FR);
    return (f64)r;
}

/* fdivs */
NR f64 nr_fdivs(NrFp* s, f64 x, f64 y) {
    if (!(nr_finite(x) && nr_finite(y)) || y == 0.0) {
        s->bad = true;
        return 0.0;
    }
    const f32 r = nr_round(s, x / y);
    nr_fprf(s, nr_class32(r));
    return (f64)r;
}

/* fmadds, fmsubs, fnmadds, fnmsubs d, a, c, b: the result in both halves. */
NR f64 nr_fmadds(NrFp* s, f64 a, f64 c, f64 b, bool subtract, bool negative) {
    if (!(nr_finite(a) && nr_finite(c) && nr_finite(b))) {
        s->bad = true;
        return 0.0;
    }
    f64 result = (f64)(f32)nr_fma_single(a, nr_25bit_fma(c), subtract ? -b : b);
    if (result != result) {
        s->bad = true;
        return 0.0;
    }
    if (negative)
        result = -result;
    nr_fprf(s, nr_class32((f32)result));
    return result;
}

/* fcmpu, fcmpo cr0: the CR0 code, the FPCC or'ed into the FPSCR. */
NR u32 nr_fcmp(NrFp* s, f64 a, f64 b) {
    if (a != a || b != b) {
        s->bad = true;
        return 0u;
    }
    const u32 compare = a < b ? 0x8u : (a > b ? 0x4u : 0x2u);
    s->fpscr |= compare << 12;
    return compare;
}

/* fctiwz (ppc_fctiw, toward zero): false where it writes nothing (an invalid
 * conversion with VE set). */
NR bool nr_fctiwz(NrFp* s, f64 value, u64* output) {
    const f64 rounded = trunc(value);
    u32 result;
    bool invalid = false;
    if (value != value) {
        const u64 bits = f64_bits(value);
        if ((bits & 0x0008000000000000ull) == 0u)
            nr_fp_exception(s, NR_FPSCR_VXSNAN);
        result = 0x80000000u;
        invalid = true;
    } else if (rounded >= 2147483648.0) {
        result = 0x7FFFFFFFu;
        invalid = true;
    } else if (rounded < -2147483648.0) {
        result = 0x80000000u;
        invalid = true;
    } else {
        result = (u32)(s32)rounded;
    }
    s->fpscr &= ~(NR_FPSCR_FI | NR_FPSCR_FR);
    if (invalid) {
        nr_fp_exception(s, NR_FPSCR_VXCVI);
    } else if (rounded != value) {
        nr_fp_exception(s, NR_FPSCR_XX);
        s->fpscr |= NR_FPSCR_FI;
        if (fabs(rounded) > fabs(value))
            s->fpscr |= NR_FPSCR_FR;
    }
    if (invalid && (s->fpscr & NR_FPSCR_VE))
        return false;
    *output = 0xFFF8000000000000ull | result | ((result == 0u && (f64_bits(value) >> 63) != 0u) ? 0x100000000ull : 0ull);
    return true;
}

/* --- Integer compares: CR0 with XER's SO. --------------------------------- */

NR u32 nr_cr0(u32 cr, u32 xer, bool less, bool greater) {
    const u32 bits = (less ? 0x8u : greater ? 0x4u : 0x2u) | ((xer >> 31) & 1u);
    return (cr & 0x0FFFFFFFu) | (bits << 28);
}

NR u32 nr_cr0_signed(u32 cr, u32 xer, s32 a, s32 b) { return nr_cr0(cr, xer, a < b, a > b); }
NR u32 nr_cr0_unsigned(u32 cr, u32 xer, u32 a, u32 b) { return nr_cr0(cr, xer, a < b, a > b); }

/* --- The clock. ---------------------------------------------------------- */

typedef struct NrClock {
    s64 downcount, budget, deadline;
} NrClock;

/* The conditions every native here starts from: no exception pending, FP
 * available, round to nearest, no write journal, a positive budget not spent,
 * and the downcount at most zero. */
NR bool nr_clock_start(const CPUState* cpu, NrClock* k) {
    k->downcount = cpu->downcount;
    k->budget = cpu->cycle_budget;
    k->deadline = cpu->cycle_deadline_budget;
    return cpu->exception == 0u && (cpu->msr & PPC_MSR_FP) != 0u && (cpu->fpscr & 3u) == 0u &&
           g_mem_write_journal == NULL && k->budget > 0 && k->downcount <= 0 && k->downcount > -k->budget;
}

/* Not spent: the test at a block leader, a call and a return dispatch. */
NR bool nr_live(const NrClock* k) { return k->downcount > -k->budget; }

/* A block leader entered and prepaid. */
NR bool nr_block(NrClock* k, u32 cycles) {
    if (k->downcount <= -k->budget)
        return false;
    if (k->deadline > 0) {
        const s64 remaining = k->deadline + k->downcount;
        if (remaining < 0 || remaining < (s64)cycles)
            return false;
    }
    k->downcount -= cycles;
    return true;
}

/* The inline _savegpr_N/_restgpr_N (scripts/windows/inline_save_restore_gpr.py):
 * five cycles when the budget allows them, and the turn's test after. */
NR bool nr_inline_gpr(NrClock* k) {
    if (!(k->downcount - 4 > -k->budget))
        return false;
    k->downcount -= 5;
    return k->downcount > -k->budget;
}

/* Paired singles unscaled (GQR0 type 0 both ways) and enabled (HID2 LSQE):
 * the translation's psq_l and psq_st inline forms. */
NR bool nr_pairs_plain(const CPUState* cpu) {
    return (cpu->hid2 & PPC_HID2_LSQE) != 0u && (cpu->gqr[0] & 0x00070007u) == 0u;
}

#endif
