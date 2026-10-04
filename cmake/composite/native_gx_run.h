#ifndef BLUEWAKE_NATIVE_GX_RUN_H
#define BLUEWAKE_NATIVE_GX_RUN_H

/* The fifth set of natives' shared pieces (native_gx.c): a translated
 * function's prepaid blocks replayed on local registers, its gather-pipe
 * stores collected and handed to the module's batch as one run of bytes.
 *
 * The game writes the GX FIFO (the gather pipe at 0xCC008000) a byte, a
 * halfword or a word at a time. In the translation each such store leaves the
 * RAM fast path (bw_writeN_at: the instruction's pc and cycle suffix, then
 * gather_pipe.h's out-of-line store, which appends the value's bytes to the
 * module's batch - bw_gather_pipe_put - and hands the batch to the host's
 * batch writer when it reaches BW_GATHER_PIPE_BATCH bytes). A native here
 * collects its pipe stores' bytes in order and, at its end, appends them to
 * the batch: at once where the batch takes them without reaching its flush
 * point, or after handing the batch over first where it would not - a run of
 * bytes through the batch entry point is the same stream, the same bytes in
 * the same order. Where the host takes the pipe a word at a time (no batch
 * writer: the FIFO trace, BLUEWAKE_GATHER_PIPE_BATCH=0) the natives decline.
 *
 * RAM stores are made in place, each with the bytes it replaces kept in an
 * undo log, and loads read RAM as it stands - the run's own stores included,
 * as the translation's loads see them. A native that declines puts every
 * byte back (in reverse order) before it returns, so it leaves nothing
 * changed; what it wrote in between is what the translation, which then runs
 * from the same state, writes at the same places. No guest code runs and no
 * host call is made in between.
 *
 * What a replayed block requires, as native_replay.h's clock: the block
 * prepaid (dolrecomp_block_can_precharge) and the turn's budget not spent at
 * its leader; with the downcount at most zero on entry, a prepaid copy's
 * deadline refund cannot happen. A path that leaves the chunk (a call or
 * return into another chunk, a bctr, a call through a pointer) is replayed
 * only where the boundary passes without the host: the edge filter on, the
 * host quiet, the address not watched (direct_calls.h) - the edge filter's
 * own test. Loads are plain RAM (MEM1 proper, no alias over it); a store is
 * plain RAM or the gather pipe; anything else (another hardware register, a
 * mirror, an alias) declines.
 *
 * No identifier here may be `ctx`. */

#include "core/cpu.h"
#include "direct_calls.h"
#include "gather_pipe.h"
#include "native_replay.h"

#include <string.h>

#define GX_RUN static inline __attribute__((always_inline))

#define GX_MAX_STORES 256u
#define GX_MAX_PIPE 1024u

/* A RAM store made in place: where, and the bytes it replaced. */
typedef struct GxUndo {
    u32 address, size;
    u64 old;
} GxUndo;

/* The run's undo log and pipe bytes (their counts are the run's own, in
 * GxRun: locals the compiler keeps in registers, where the RAM stores, made
 * through byte pointers, would make it reload a count kept here). */
typedef struct GxLog {
    GxUndo undo[GX_MAX_STORES];
    u8 pipe[GX_MAX_PIPE + 8u];
} GxLog;

typedef struct GxRun {
    CPUState* cpu;
    GxLog* log;
    u8* ram;
    u32 ram_size;
    u32 stores, pipe_length; /* the log's entries and bytes */
    s64 downcount, budget, deadline;
    u32 suffix;       /* the last access's cycle suffix, as the run leaves it */
    u32 reserve_addr; /* the reservation, as the stores leave it */
    bool reserve;
    bool bad;         /* the translation would have left the path replayed here */
    unsigned depth;   /* calls entered and not yet returned from */
    NrFp fp;          /* the FPSCR as the run leaves it; fp.bad: an FP operation off its inline path */
} GxRun;

/* --- Entry, and declining. ---------------------------------------------- */

/* The conditions every native here starts from: no exception pending, no
 * write journal, no alias over MEM1, the host's pipe writers set (without
 * the word writer a pipe store goes to the host's MMIO handler; without the
 * batch writer it goes to the host word by word), a positive budget not
 * spent, and the downcount at most zero. */
GX_RUN bool gx_start(GxRun* s, GxLog* log, CPUState* cpu) {
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
    s->fp.fpscr = cpu->fpscr;
    s->fp.bad = false;
    return cpu->exception == 0u && g_mem_write_journal == NULL && !g_ppc_guest_aliases_overlap_mem1 &&
           bw_gather_pipe_write != NULL && bw_gather_pipe_bytes != NULL && s->ram != NULL &&
           s->ram_size <= 0x02000000u && s->budget > 0 && s->downcount <= 0 && s->downcount > -s->budget;
}

/* Every RAM byte the run wrote put back, latest first. */
static __attribute__((noinline)) void gx_undo(const GxLog* log, u8* ram, u32 stores) {
    for (u32 i = stores; i-- > 0u;) {
        const GxUndo* u = &log->undo[i];
        u8* at = ram + (u->address - GC_RAM_BASE);
        switch (u->size) {
        case 1: at[0] = (u8)u->old; break;
        case 2: write_be16(at, (u16)u->old); break;
        case 4: write_be32(at, (u32)u->old); break;
        default: write_be64(at, u->old); break;
        }
    }
}

/* Decline: nothing changed. */
GX_RUN int gx_decline(GxRun* s) {
    if (s->stores != 0u)
        gx_undo(s->log, s->ram, s->stores);
    return 0;
}

/* Floating point available (the FP instructions' ppc_fp_available_inline). */
GX_RUN bool gx_fp_ready(const CPUState* cpu) { return (cpu->msr & PPC_MSR_FP) != 0u; }

/* FP arithmetic replayed on the host as the translation computes it: the
 * rounding mode round-to-nearest (as native_replay.h's natives require). */
GX_RUN bool gx_fp_arith_ready(const CPUState* cpu) { return (cpu->fpscr & 3u) == 0u; }

/* The paired-single loads and stores take the translation's inline forms
 * (generated.h's ppc_psq_load_inline, ppc_psq_store_inline): each named GQR
 * unscaled (type 0) for the direction used, and HID2's LSQE set. */
GX_RUN bool gx_psq_ready(const CPUState* cpu, u32 load_gqrs, u32 store_gqrs) {
    if ((cpu->hid2 & PPC_HID2_LSQE) == 0u)
        return false;
    for (u32 i = 0; i < 8u; ++i) {
        if ((load_gqrs >> i) & 1u && ((cpu->gqr[i] >> 16) & 7u) != 0u)
            return false;
        if ((store_gqrs >> i) & 1u && (cpu->gqr[i] & 7u) != 0u)
            return false;
    }
    return true;
}

/* --- The clock. -------------------------------------------------------- */

/* A block leader entered: not spent, and prepaid. */
GX_RUN bool gx_block(GxRun* s, u32 cycles) {
    if (s->bad || s->fp.bad || s->downcount <= -s->budget)
        return false;
    if (s->deadline > 0) {
        const s64 remaining = s->deadline + s->downcount;
        if (remaining < 0 || remaining < (s64)cycles)
            return false;
    }
    s->downcount -= cycles;
    return true;
}

/* Not spent: the test a call or a return dispatch makes. */
GX_RUN bool gx_live(const GxRun* s) { return s->downcount > -s->budget; }

/* A boundary between chunks that passes without the host (the edge filter's
 * test, dispatch_loop.h). */
GX_RUN bool gx_silent(const GxRun* s, u32 address) {
    return bw_edge_filter_enabled && bw_edge_watch_ready && bw_host_quiet(s->cpu) && bw_edge_unwatched(address);
}

/* --- Loads. ------------------------------------------------------------ */

/* MEM1 proper: what the translation's fast loads and stores reach without
 * leaving line (BW_RAM_FAST: its MEM1 is at least this large). */
GX_RUN bool gx_ram(const GxRun* s, u32 address, u32 size) { return address - GC_RAM_BASE <= s->ram_size - size; }

#define GX_LOAD(bits, type, read)                                                \
    GX_RUN type gx_ld##bits(GxRun* s, u32 address) {                             \
        if (__builtin_expect(!gx_ram(s, address, bits / 8u), 0)) {               \
            s->bad = true;                                                       \
            return 0;                                                            \
        }                                                                        \
        return read(s->ram + (address - GC_RAM_BASE));                           \
    }
GX_RUN u8 gx_read8(const u8* p) { return *p; }
GX_LOAD(8, u8, gx_read8)
GX_LOAD(16, u16, read_be16)
GX_LOAD(32, u32, read_be32)
GX_LOAD(64, u64, read_be64)
#undef GX_LOAD

/* --- Stores. ----------------------------------------------------------- */

/* The gather pipe (gather_pipe.h's bw_gather_pipe, the writers checked at
 * entry). */
GX_RUN bool gx_pipe_address(u32 address) {
    return (address & 0x40000000u) != 0u && (address & ~0x1Fu) == 0xCC008000u;
}

/* A pipe store's bytes, big-endian, after the run's others. */
GX_RUN void gx_put(GxRun* s, u64 value, u32 size) {
    if (s->pipe_length + size > GX_MAX_PIPE) {
        s->bad = true;
        return;
    }
    u8* out = s->log->pipe + s->pipe_length;
    switch (size) {
    case 1: out[0] = (u8)value; break;
    case 2: write_be16(out, (u16)value); break;
    case 4: write_be32(out, (u32)value); break;
    default: write_be64(out, value); break;
    }
    s->pipe_length += size;
}

/* A RAM store in place, its old bytes kept. */
GX_RUN void gx_ram_store(GxRun* s, u32 address, u64 value, u32 size) {
    if (s->stores >= GX_MAX_STORES) {
        s->bad = true;
        return;
    }
    GxUndo* u = &s->log->undo[s->stores++];
    u8* at = s->ram + (address - GC_RAM_BASE);
    u->address = address;
    u->size = size;
    switch (size) {
    case 1:
        u->old = at[0];
        at[0] = (u8)value;
        break;
    case 2:
        u->old = read_be16(at);
        write_be16(at, (u16)value);
        break;
    case 4:
        u->old = read_be32(at);
        write_be32(at, (u32)value);
        break;
    default:
        u->old = read_be64(at);
        write_be64(at, value);
        break;
    }
}

/* clear_matching_reservation, on the run's copy. */
GX_RUN void gx_clear_reservation(GxRun* s, u32 address) {
    if (s->reserve && (((s->reserve_addr & ~0x40000000u) ^ (address & ~0x40000000u)) & ~31u) == 0u)
        s->reserve = false;
}

/* bw_writeN_at (the prepaid copies' stores): RAM inline unless a reservation
 * is held, when the store goes out of line (the instruction's suffix) and
 * clears a matching one; the pipe out of line. */
GX_RUN void gx_st(GxRun* s, u32 suffix, u32 address, u64 value, u32 size) {
    if (gx_pipe_address(address)) {
        s->suffix = suffix;
        gx_put(s, value, size);
        return;
    }
    if (!gx_ram(s, address, size)) {
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
 * whose code set the suffix before them): the same, with no suffix of its
 * own. */
GX_RUN void gx_stp(GxRun* s, u32 address, u64 value, u32 size) {
    if (gx_pipe_address(address)) {
        gx_put(s, value, size);
        return;
    }
    if (!gx_ram(s, address, size)) {
        s->bad = true;
        return;
    }
    gx_clear_reservation(s, address);
    gx_ram_store(s, address, value, size);
}

/* The instruction's pc stays in the call for the reader (the translation
 * stores it only on the way out of line, where nothing reads it). */
#define gx_st8(s, pc, suffix, address, value) gx_st((s), (suffix), (address), (u8)(value), 1u)
#define gx_st16(s, pc, suffix, address, value) gx_st((s), (suffix), (address), (u16)(value), 2u)
#define gx_st32(s, pc, suffix, address, value) gx_st((s), (suffix), (address), (u32)(value), 4u)
#define gx_st64(s, pc, suffix, address, value) gx_st((s), (suffix), (address), (u64)(value), 8u)
#define gx_stp8(s, pc, address, value) gx_stp((s), (address), (u8)(value), 1u)
#define gx_stp16(s, pc, address, value) gx_stp((s), (address), (u16)(value), 2u)
#define gx_stp32(s, pc, address, value) gx_stp((s), (address), (u32)(value), 4u)
#define gx_stp64(s, pc, address, value) gx_stp((s), (address), (u64)(value), 8u)

/* --- Floating point: the translation's conversions (generated.h). ------ */

GX_RUN u32 gx_rotl32(u32 value, u32 sh) {
    sh &= 31u;
    return sh ? ((value << sh) | (value >> (32u - sh))) : value;
}

/* dolrecomp_f32_from_bits (lfs): types.h's convert_to_double. */
GX_RUN f64 gx_f32_from_bits(u32 bits) { return f64_value(convert_to_double(bits)); }

/* dolrecomp_f32_to_bits (stfs). */
GX_RUN u32 gx_f32_to_bits(f64 value) {
    u64 bits;
    memcpy(&bits, &value, sizeof bits);
    __asm__("" : "+r"(bits));
    const u32 exp = (u32)((bits >> 52) & 0x7FFu);
    if (exp > 896 || (bits & 0x7FFFFFFFFFFFFFFFull) == 0)
        return (u32)(((bits >> 32) & 0xC0000000u) | ((bits >> 29) & 0x3FFFFFFFu));
    if (exp >= 874) {
        u32 result = (u32)(0x80000000u | ((bits & 0x000FFFFFFFFFFFFFull) >> 21));
        result >>= 905 - exp;
        result |= (u32)((bits >> 32) & 0x80000000u);
        return result;
    }
    return (u32)(((bits >> 32) & 0xC0000000u) | ((bits >> 29) & 0x3FFFFFFFu));
}

GX_RUN f64 gx_f64_from_bits(u64 bits) { return f64_value(bits); }
GX_RUN u64 gx_f64_to_bits(f64 value) { return f64_bits(value); }

/* ppc_psq_load_inline (type 0): the first single, then the second (or 1.0). */
GX_RUN void gx_psq_l(GxRun* s, f64* first, f64* second, u32 address, bool w) {
    *first = f64_value(convert_to_double(gx_ld32(s, address)));
    *second = w ? 1.0 : f64_value(convert_to_double(gx_ld32(s, address + 4u)));
}

/* ppc_psq_store_inline (type 0): bw_mem_write32 of each half, flushed to zero. */
GX_RUN void gx_psq_st(GxRun* s, u32 pc, f64 first, f64 second, u32 address, bool w) {
    gx_stp32(s, pc, address, convert_to_single_ftz(f64_bits(first)));
    if (!w)
        gx_stp32(s, pc, address + 4u, convert_to_single_ftz(f64_bits(second)));
}

/* --- Floating-point arithmetic. -----------------------------------------
 *
 * The single operations as native_replay.h replays inline_fp.h's inline
 * paths on values (nr_fadds, nr_fsubs, nr_fmuls, nr_fdivs: an operand the
 * translation would hand the interpreter declines); the double ones written
 * out the same way; fcmpo/fcmpu on any CR field. fctiwz, frsp and frsqrte -
 * which the translation hands GXRuntime's interpreter in every case - call
 * those very functions, on a scratch CPU state that holds the run's FPSCR. */

/* classify_f64, on the bits. */
GX_RUN u32 gx_class64(f64 value) {
    const u64 bits = nr_bits(value);
    const u64 sign = bits >> 63;
    const u64 exponent = bits & 0x7FF0000000000000ull;
    const u64 fraction = bits & 0x000FFFFFFFFFFFFFull;
    if (exponent == 0x7FF0000000000000ull)
        return fraction ? 0x11u : (sign ? 0x09u : 0x05u);
    if (exponent == 0)
        return fraction ? (sign ? 0x18u : 0x14u) : (sign ? 0x12u : 0x02u);
    return sign ? 0x08u : 0x04u;
}

/* bw_fp_fadd, bw_fp_fsub (double results): fpr only. */
GX_RUN f64 gx_fadd(NrFp* f, f64 x, f64 y) {
    if (!(nr_finite(x) && nr_finite(y))) {
        f->bad = true;
        return 0.0;
    }
    const f64 r = x + y;
    nr_fprf(f, gx_class64(r));
    return r;
}

GX_RUN f64 gx_fsub(NrFp* f, f64 x, f64 y) {
    if (!(nr_finite(x) && nr_finite(y))) {
        f->bad = true;
        return 0.0;
    }
    const f64 r = x - y;
    nr_fprf(f, gx_class64(r));
    return r;
}

/* bw_fp_fmul */
GX_RUN f64 gx_fmul(NrFp* f, f64 x, f64 y) {
    if (!(nr_finite(x) && nr_finite(y))) {
        f->bad = true;
        return 0.0;
    }
    const f64 r = x * y;
    nr_fprf(f, gx_class64(r));
    f->fpscr &= ~(NR_FPSCR_FI | NR_FPSCR_FR);
    return r;
}

/* bw_fp_fcmp: the FPCC or'ed into the FPSCR, the code in CR field `field`. */
GX_RUN void gx_fcmp(NrFp* f, u32* cr, u32 field, f64 a, f64 b) {
    const u32 compare = nr_fcmp(f, a, b);
    const u32 shift = 4u * (7u - field);
    *cr = (*cr & ~(0xFu << shift)) | (compare << shift);
}

/* The interpreter's own functions on the run's FPSCR. */
extern CPUState g_gx_fp_scratch;

static inline bool gx_fctiw(NrFp* f, f64 value, bool toward_zero, u64* result) {
    g_gx_fp_scratch.fpscr = f->fpscr;
    const bool written = ppc_fctiw(&g_gx_fp_scratch, value, toward_zero, result);
    f->fpscr = g_gx_fp_scratch.fpscr;
    return written;
}

static inline bool gx_frsqrte(NrFp* f, f64 value, f64* result) {
    g_gx_fp_scratch.fpscr = f->fpscr;
    const bool written = ppc_frsqrte(&g_gx_fp_scratch, value, result);
    f->fpscr = g_gx_fp_scratch.fpscr;
    return written;
}

/* frsp d, b: fpr[d] and ps1[d] as ppc_frsp leaves them (or unchanged). */
static inline void gx_frsp(NrFp* f, f64* d, f64* d1, f64 b) {
    g_gx_fp_scratch.fpscr = f->fpscr;
    g_gx_fp_scratch.fpr[0] = *d;
    g_gx_fp_scratch.ps1[0] = *d1;
    g_gx_fp_scratch.fpr[1] = b;
    ppc_frsp(&g_gx_fp_scratch, 0, 1);
    f->fpscr = g_gx_fp_scratch.fpscr;
    *d = g_gx_fp_scratch.fpr[0];
    *d1 = g_gx_fp_scratch.ps1[0];
}

/* --- The commit. ------------------------------------------------------- */

/* The run's pipe bytes where the batch would reach its flush point inside
 * them: the batch handed over first, then the bytes, a batch at a time. */
static __attribute__((noinline)) void gx_pipe_spill(const u8* bytes, u32 length) {
    bw_gather_pipe_flush();
    while (length >= BW_GATHER_PIPE_BATCH) {
        memcpy(bw_gather_pipe_buffer, bytes, BW_GATHER_PIPE_BATCH);
        bw_gather_pipe_length = BW_GATHER_PIPE_BATCH;
        bw_gather_pipe_flush();
        bytes += BW_GATHER_PIPE_BATCH;
        length -= BW_GATHER_PIPE_BATCH;
    }
    memcpy(bw_gather_pipe_buffer, bytes, length);
    bw_gather_pipe_length = length;
}

/* The pipe's bytes to the batch, and the clock, suffix, reservation and
 * FPSCR the run leaves. The caller then writes the registers and pc. */
GX_RUN void gx_commit(GxRun* s) {
    GxLog* log = s->log;
    CPUState* cpu = s->cpu;
    const u32 length = s->pipe_length;
    if (length != 0u) {
        if (bw_gather_pipe_length + length < BW_GATHER_PIPE_BATCH) {
            /* Eight bytes at a time: the log and the batch both have eight
             * bytes to spare past their ends, and the batch's bytes past its
             * length are written before they are read. */
            u8* out = bw_gather_pipe_buffer + bw_gather_pipe_length;
            for (u32 i = 0; i < length; i += 8u) {
                u64 v;
                memcpy(&v, log->pipe + i, 8u);
                memcpy(out + i, &v, 8u);
            }
            bw_gather_pipe_length += length;
        } else {
            gx_pipe_spill(log->pipe, length);
        }
    }
    cpu->downcount = s->downcount;
    cpu->cycle_observation_suffix = s->suffix;
    cpu->reserve_valid = s->reserve;
    cpu->fpscr = s->fp.fpscr;
}

#endif
