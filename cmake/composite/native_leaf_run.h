#ifndef BLUEWAKE_NATIVE_LEAF_RUN_H
#define BLUEWAKE_NATIVE_LEAF_RUN_H

/* The seventh set of natives' shared pieces (native_libm.c, native_bgblk.c,
 * native_rot.c): leaf compute code - functions that run to their return
 * without calling anything not replayed with them - each its translation
 * replayed on local registers, as the fifth set's (native_gx_run.h, whose
 * clock, loads, conversions and FP arithmetic these use as they are), for
 * code that writes RAM and nothing else.
 *
 * - A store is made in place in plain RAM (MEM1 proper, unaliased), its old
 *   bytes kept in the run's undo log; a store anywhere else (the gather pipe,
 *   another hardware register, a mirror) marks the run bad and it declines.
 *   So a native here never hands the host anything, and needs none of the
 *   pipe's writers.
 * - The double divide and the interpreter's fctiw, frsqrte and frsp (which
 *   the translation hands GXRuntime's interpreter in every case) are those
 *   very functions, run on a scratch CPU state holding the run's FPSCR and
 *   the registers they read; they write the FPRs, the FPSCR and nothing else.
 * - A decline puts back every byte the run stored (latest first) and says
 *   why (lf_why, which the group's entry counts): the state at entry (an
 *   exception pending, a write journal, an alias over MEM1, FP unavailable,
 *   a rounding mode other than nearest, scaled paired singles), the clock
 *   (the turn's budget spent, or a deadline inside the work: a block not
 *   prepaid), an address that is not plain RAM, an FP operand the translation
 *   would hand the interpreter (a NaN or an infinity, a zero divisor), the
 *   host (a boundary between chunks that would not pass silently), or a path
 *   the native does not replay.
 *
 * Each group's file includes its generated natives (native_<group>_gen.inc,
 * scripts/windows/native_leaf_gen.py) and then native_leaf_group.inc, which
 * makes each native's entry point (the hook's call), the decline gate, the
 * counts and the report.
 *
 * No identifier here may be `ctx`. */

#include "native_gx_run.h"

enum {
    LF_STATE,  /* at entry: exception, journal, aliases, FP, rounding, quantised pairs */
    LF_CLOCK,  /* the budget spent, or a deadline inside the work */
    LF_MEMORY, /* a load or store not plain RAM (or the undo log full) */
    LF_FP,     /* an FP operand off the translation's inline path */
    LF_HOST,   /* a boundary between chunks the host would be asked about */
    LF_PATH,   /* a call or jump the native does not replay */
    LF_BLOCK,  /* (a block's test: the clock, an address or an FP operand, told apart below) */
    LF_WHY_COUNT = LF_BLOCK
};

/* The last decline's reason, for the group's entry to count (the game
 * thread's: the natives run only there, one at a time). */
static unsigned lf_why;

/* The conditions every native here starts from: no exception pending, no
 * write journal, no alias over MEM1, MEM1 present, a positive budget not
 * spent, and the downcount at most zero. */
GX_RUN bool lf_start(GxRun* s, GxLog* log, CPUState* cpu) {
    s->cpu = cpu;
    s->log = log;
    s->stores = s->pipe_length = 0u;
    s->ram = cpu->ram;
    s->ram_size = cpu->ram_size;
    s->downcount = cpu->downcount;
    s->budget = cpu->cycle_budget;
    s->deadline = cpu->cycle_deadline_budget;
    s->suffix = cpu->cycle_observation_suffix;
    s->reserve_addr = cpu->reserve_addr;
    s->reserve = cpu->reserve_valid;
    s->bad = false;
    s->depth = 0u;
    s->checkpoint = 0u;
    s->fp.fpscr = cpu->fpscr;
    s->fp.bad = false;
    return cpu->exception == 0u && g_mem_write_journal == NULL && !g_ppc_guest_aliases_overlap_mem1 &&
           s->ram != NULL && s->ram_size <= 0x02000000u && s->budget > 0 && s->downcount <= 0 &&
           s->downcount > -s->budget;
}

/* The clock's floor: the lowest downcount a block may leave. A block of C
 * cycles is entered prepaid and unstopped (native_gx_run.h's gx_block, the
 * translation's leader) when the downcount D before it is above -budget and,
 * with a deadline, deadline + D >= C: that is, D - C >= -deadline. Here a
 * block asks only D - C >= floor, floor = -min(budget, deadline) (-budget
 * without a deadline): the deadline's test exactly, and D - C >= -budget,
 * which implies D > -budget (C is at least 1) - stricter than the
 * translation's by a block's cycles at the budget's end, where a native then
 * declines that the translation would have run on (always exact, and rare: a
 * turn ends at its deadline far more often). */
GX_RUN s64 lf_floor(const GxRun* s) {
    return s->deadline > 0 && s->deadline < s->budget ? -s->deadline : -s->budget;
}

/* A block leader entered: prepaid, not spent, and the run not gone bad (a
 * load from outside RAM, an FP operand off the inline path: a loop on such
 * a value stops here). */
GX_RUN bool lf_block(GxRun* s, s64 floor, u32 cycles) {
    s->downcount -= cycles;
    return !(s->bad | s->fp.bad) && s->downcount >= floor;
}

/* A boundary between chunks that passes without the host: native_gx_run.h's
 * gx_silent (the edge filter's own test), with the host's half of it - the
 * filter on, the watch list ready, the host quiet - asked once a run and kept
 * (in the run's checkpoint field, which only the sixth set's natives use: 0
 * not yet asked, 1 silent, 2 not). Nothing of the host's runs during a run;
 * a flag the host raises in the meantime is one raised a moment later, as
 * for the earlier sets' natives. The address's half, the watch list, is the
 * same lookup. A native that crosses a boundary per loop iteration (a call to
 * a leaf in another chunk, a jump table's bctr) asks the host once instead of
 * at each crossing. */
GX_RUN bool lf_silent(GxRun* s, u32 address) {
    if (__builtin_expect(s->checkpoint == 0u, 0))
        s->checkpoint = bw_edge_filter_enabled && bw_edge_watch_ready && bw_host_quiet(s->cpu) ? 1u : 2u;
    return s->checkpoint == 1u && bw_edge_unwatched(address);
}

/* Decline: every byte the run stored put back, the reason kept. A block's
 * test fails for the clock unless the run went bad before it. */
GX_RUN int lf_decline(GxRun* s, unsigned why) {
    if (s->stores != 0u)
        gx_undo(s->log, s->ram, s->stores);
    if (why == LF_BLOCK)
        why = s->bad ? LF_MEMORY : s->fp.bad ? LF_FP : LF_CLOCK;
    lf_why = why;
    return 0;
}

/* bw_writeN_at (the prepaid copies' stores): plain RAM, inline unless a
 * reservation is held, when the store goes out of line (the instruction's
 * suffix) and clears a matching one. Anywhere else: the run is bad. */
GX_RUN void lf_st(GxRun* s, u32 suffix, u32 address, u64 value, u32 size) {
    if (__builtin_expect(!gx_ram(s, address, size), 0)) {
        s->bad = true;
        return;
    }
    if (s->reserve) {
        s->suffix = suffix;
        gx_clear_reservation(s, address);
    }
    gx_ram_store(s, address, value, size);
}

/* bw_mem_writeN (the main path's stores, the FP and paired-single stores,
 * whose code set the suffix before them): plain RAM, a matching reservation
 * cleared. */
GX_RUN void lf_stp(GxRun* s, u32 address, u64 value, u32 size) {
    if (__builtin_expect(!gx_ram(s, address, size), 0)) {
        s->bad = true;
        return;
    }
    gx_clear_reservation(s, address);
    gx_ram_store(s, address, value, size);
}

#define lf_st8(s, pc, suffix, address, value) lf_st((s), (suffix), (address), (u8)(value), 1u)
#define lf_st16(s, pc, suffix, address, value) lf_st((s), (suffix), (address), (u16)(value), 2u)
#define lf_st32(s, pc, suffix, address, value) lf_st((s), (suffix), (address), (u32)(value), 4u)
#define lf_st64(s, pc, suffix, address, value) lf_st((s), (suffix), (address), (u64)(value), 8u)
#define lf_stp8(s, pc, address, value) lf_stp((s), (address), (u8)(value), 1u)
#define lf_stp16(s, pc, address, value) lf_stp((s), (address), (u16)(value), 2u)
#define lf_stp32(s, pc, address, value) lf_stp((s), (address), (u32)(value), 4u)
#define lf_stp64(s, pc, address, value) lf_stp((s), (address), (u64)(value), 8u)

/* ppc_psq_store_inline (type 0): each half flushed to zero, as native_gx_run.h's. */
GX_RUN void lf_psq_st(GxRun* s, u32 pc, f64 first, f64 second, u32 address, bool w) {
    (void)pc;
    lf_stp32(s, pc, address, convert_to_single_ftz(nr_bits(first)));
    if (!w)
        lf_stp32(s, pc, address + 4u, convert_to_single_ftz(nr_bits(second)));
}

/* lfs and the other single loads (dolrecomp_f32_from_bits): a normal
 * single (exponent 1-254) widened by the hardware, which gives the same
 * double, as inline_fp.h's form in the chunks does (tests/f32_from_bits_test.c
 * compares the two over every bit pattern); zeros, denormals, infinities and
 * NaNs by hand (native_replay.h's nr_to_double, types.h's convert_to_double
 * inline: native_gx_run.h's gx_f32_from_bits calls it out of line). */
GX_RUN f64 lf_f32_from_bits(u32 bits) {
    if (__builtin_expect(((bits >> 23) & 0xFFu) - 1u < 254u, 1)) {
        f32 single;
        memcpy(&single, &bits, sizeof single);
        return (f64)single;
    }
    return f64_value(nr_to_double(bits));
}

/* ppc_psq_load_inline (type 0): the first single, then the second (or 1.0). */
GX_RUN void lf_psq_l(GxRun* s, f64* first, f64* second, u32 address, bool w) {
    *first = lf_f32_from_bits(gx_ld32(s, address));
    *second = w ? 1.0 : lf_f32_from_bits(gx_ld32(s, address + 4u));
}

/* The clock, the suffix, the reservation and the FPSCR the run leaves. The
 * caller then writes the registers and pc. */
GX_RUN void lf_commit(GxRun* s) {
    CPUState* cpu = s->cpu;
    cpu->downcount = s->downcount;
    cpu->cycle_observation_suffix = s->suffix;
    cpu->reserve_valid = s->reserve;
    cpu->fpscr = s->fp.fpscr;
}

/* --- The interpreter's own FP functions, on a scratch state. ------------- */

/* Each group's own (static: one per file that includes this). */
static CPUState lf_fp_scratch;

/* fdiv d, a, b (ppc_fdiv): fpr[d] and the FPSCR as the interpreter leaves
 * them - the quotient, or d unchanged where an enabled exception gates it. */
static inline void lf_fdiv(NrFp* f, f64* d, f64 a, f64 b) {
    lf_fp_scratch.fpscr = f->fpscr;
    lf_fp_scratch.fpr[0] = *d;
    lf_fp_scratch.fpr[1] = a;
    lf_fp_scratch.fpr[2] = b;
    ppc_fdiv(&lf_fp_scratch, 0, 1, 2);
    f->fpscr = lf_fp_scratch.fpscr;
    *d = lf_fp_scratch.fpr[0];
}

/* fctiw, fctiwz: toward zero, native_replay.h's nr_fctiwz (ppc_fctiw written
 * out on the run's FPSCR, as the fourth set's natives use it); in the
 * rounding mode (nearest: the natives that convert require it), the
 * interpreter's own. */
static inline bool lf_fctiw(NrFp* f, f64 value, bool toward_zero, u64* result) {
    if (toward_zero)
        return nr_fctiwz(f, value, result);
    lf_fp_scratch.fpscr = f->fpscr;
    const bool written = ppc_fctiw(&lf_fp_scratch, value, toward_zero, result);
    f->fpscr = lf_fp_scratch.fpscr;
    return written;
}

static inline bool lf_frsqrte(NrFp* f, f64 value, f64* result) {
    lf_fp_scratch.fpscr = f->fpscr;
    const bool written = ppc_frsqrte(&lf_fp_scratch, value, result);
    f->fpscr = lf_fp_scratch.fpscr;
    return written;
}

/* frsp d, b: fpr[d] and ps1[d] as ppc_frsp leaves them (or unchanged). Of
 * anything but a NaN, ppc_frsp written out on the run's FPSCR: force_single
 * (nr_round), XX with FI where the single is not the value (FI cleared
 * otherwise), FR where it rounded away from zero, the single's FPRF; its
 * compares are the same SSE compares under the same MXCSR. A NaN: the
 * interpreter's own. */
static inline void lf_frsp(NrFp* f, f64* d, f64* d1, f64 b) {
    if (__builtin_expect(!(b != b), 1)) {
        const f32 rounded = nr_round(f, b);
        const f64 back = (f64)rounded;
        if (b != back) {
            nr_fp_exception(f, NR_FPSCR_XX);
            f->fpscr |= NR_FPSCR_FI;
        } else {
            f->fpscr &= ~NR_FPSCR_FI;
        }
        f->fpscr = (f->fpscr & ~NR_FPSCR_FR) | (fabs(back) > fabs(b) ? NR_FPSCR_FR : 0u);
        nr_fprf(f, nr_class32(rounded));
        *d = back;
        *d1 = back;
        return;
    }
    lf_fp_scratch.fpscr = f->fpscr;
    lf_fp_scratch.fpr[0] = *d;
    lf_fp_scratch.ps1[0] = *d1;
    lf_fp_scratch.fpr[1] = b;
    ppc_frsp(&lf_fp_scratch, 0, 1);
    f->fpscr = lf_fp_scratch.fpscr;
    *d = lf_fp_scratch.fpr[0];
    *d1 = lf_fp_scratch.ps1[0];
}

#endif
