/* J3D's joint matrix calculations (GZLE01 J3DMtxCalcBasic::calcTransform,
 * J3DMtxCalcSoftimage::calcTransform, J3DMtxCalcMaya::calcTransform),
 * native, with the calls they make: the register save and restore routines
 * (inline in the translation: scripts/windows/inline_save_restore_gpr.py),
 * J3DGetTranslateRotateMtx (both forms), PSMTXConcat and PSMTXCopy.
 *
 * Every joint of every animated model goes through one of them each frame:
 * on Outset with Link running they were 0.46 (Maya), 0.14 (Basic) and a
 * little (Softimage) percent of the game thread exclusive of their callees,
 * which they reach across three other chunks (and the Maya form's own body
 * is split between two chunks).
 *
 * Each reads its inputs, computes everything in host floats, and only then
 * writes - the guest's memory as the function leaves it (its frame and the
 * frames of its callees, J3DSys's current matrix and scales, the joint's
 * scale flag and animation matrix) and every register: the scratch ones its
 * callees leave (PSMTXConcat's f14, f15 and f31 restored, with their second
 * halves as it leaves them), CR0 and XER as the last compare and sraw leave
 * them, FPSCR's FPRF from the last arithmetic (and the fcmpu codes or'ed in
 * after it), FI and FR cleared by the fmuls, LR as restored, the cycles of
 * each block and inline save or restore on the path taken, the last cycle
 * suffix (mtlr's, 2), pc at the return address.
 *
 * The arithmetic is the translation's: inline_fp.h's inline paths compute in
 * doubles and round to single (fmuls with its multiplier rounded to 25 bits,
 * which leaves a single unchanged; the fused multiply-adds with their tie
 * correction), and on singles that is the single operation correctly
 * rounded - host float arithmetic. That holds while nothing is denormal,
 * infinite or a NaN, and no rounding is near the denormal range (where NI
 * would flush the double first), so: every input is zero or a normal
 * single, every result is checked to be zero or at least 2^-125 in
 * magnitude and finite, and the rounding mode must be round to nearest.
 * Otherwise - and wherever exactness is not certain - it declines, changing
 * nothing: FP off, quantised pairs (GQR0 types, HID2 LSQE), an exception
 * pending, a write journal, any address it or its callees touch not plain
 * RAM, a store landing on another store's range or on an input, the turn's
 * budget or the next deadline inside the longest path, or a boundary between
 * chunks the translation crosses - the calls and returns, and the Maya
 * form's chunk boundary - that would not pass without the host as in play
 * (the edge filter on, the host's edge service quiet, none of those
 * addresses watched: direct_calls.h).
 *
 * tests/native_mtxcalc_test.c compares all three with the translation, every
 * register and byte. No identifier here may be `ctx`. */
#include "native_mtxcalc.h"
#include "inline_fp.h"
#include "direct_calls.h"

#include <math.h>
#include <stdio.h>

int bluewake_native_mtxcalc_enabled;

enum { MC_BASIC, MC_SOFTIMAGE, MC_MAYA, MC_COUNT };
static unsigned long long s_mc_runs[MC_COUNT], s_mc_declined[MC_COUNT];

void bluewake_native_mtxcalc_report(void) {
    fprintf(stderr, "[native-mtxcalc] basic=%llu/%llu softimage=%llu/%llu maya=%llu/%llu (native/declined)\n",
            s_mc_runs[0], s_mc_declined[0], s_mc_runs[1], s_mc_declined[1], s_mc_runs[2], s_mc_declined[2]);
}

#define MC static inline __attribute__((always_inline))

#define J3D_SYS_MODEL 0x803EDA90u    /* j3dSys.mModel (j3dSys 0x803EDA58 + 56) */
#define J3D_CURRENT_MTX 0x803EDB80u  /* J3DSys::mCurrentMtx */
#define J3D_CURRENT_S 0x803EDBB0u    /* J3DSys::mCurrentS */
#define J3D_PARENT_S 0x803EDBBCu     /* J3DSys::mParentS */
#define J3D_STATICS_SIZE (48u + 24u) /* the three, together */
#define PSMTX_UNIT 0x803F66F0u       /* PSMTXConcat's (0, 1) pair */

/* --- Memory, on ranges already found to be plain RAM. ------------------- */

MC u8* mc_at(const CPUState* cpu, u32 address) { return cpu->ram + (address - GC_RAM_BASE); }
MC u32 mc_word(const CPUState* cpu, u32 address) { return read_be32(mc_at(cpu, address)); }

MC f32 mc_float(u32 bits) {
    f32 value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

MC u32 mc_bits(f32 value) {
    u32 bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

/* The stores, through a copy of the RAM pointer (so the compiler need not
 * reload the guest state after each), each clearing a reservation on its
 * granule as the translation's stores do - only looked at when one was
 * held at entry. */
typedef struct McOut {
    CPUState* cpu;
    u8* ram;
    bool reserved;
} McOut;

MC void mc_store(const McOut* o, u32 address, u32 value) {
    if (o->reserved)
        clear_matching_reservation(o->cpu, address);
    write_be32(o->ram + (address - GC_RAM_BASE), value);
}

MC void mc_store8(const McOut* o, u32 address, u8 value) {
    if (o->reserved)
        clear_matching_reservation(o->cpu, address);
    o->ram[address - GC_RAM_BASE] = value;
}

MC void mc_store64(const McOut* o, u32 address, u64 value) {
    if (o->reserved)
        clear_matching_reservation(o->cpu, address);
    write_be64(o->ram + (address - GC_RAM_BASE), value);
}

/* --- The arithmetic. ---------------------------------------------------- */

/* Zero or a normal single: what lfs and psq_l turn into the same double the
 * host's float conversion gives, and stfs and psq_st back. */
MC bool mc_plain(u32 bits) {
    const u32 exponent = bits & 0x7F800000u;
    return (exponent != 0u && exponent != 0x7F800000u) || (bits & 0x7FFFFFFFu) == 0u;
}

/* A result the translation's double-then-single rounding gives too: zero,
 * or finite and at least 2^-125 (NI's flush tests the double against 2^-126). */
MC f32 mc_ok(f32 result, bool* bad) {
    const u32 bits = mc_bits(result), exponent = bits & 0x7F800000u;
    *bad |= !((bits & 0x7FFFFFFFu) == 0u || (exponent >= (2u << 23) && exponent != 0x7F800000u));
    return result;
}

MC f32 mc_mul(f32 a, f32 b, bool* bad) { return mc_ok(a * b, bad); }
MC f32 mc_add(f32 a, f32 b, bool* bad) { return mc_ok(a + b, bad); }
MC f32 mc_sub(f32 a, f32 b, bool* bad) { return mc_ok(a - b, bad); }
MC f32 mc_div(f32 a, f32 b, bool* bad) { return mc_ok(a / b, bad); }
MC f32 mc_fma(f32 a, f32 b, f32 c, bool* bad) { return mc_ok(fmaf(a, b, c), bad); }

typedef struct McPair {
    f32 x, y;
} McPair;

/* ps_muls0 d, a, c / ps_madds0 and ps_madds1 d, a, c, b: c's half `cx`. */
MC McPair mc_muls(McPair a, f32 cx, bool* bad) { return (McPair){mc_mul(a.x, cx, bad), mc_mul(a.y, cx, bad)}; }
MC McPair mc_madds(McPair a, f32 cx, McPair b, bool* bad) {
    return (McPair){mc_fma(a.x, cx, b.x, bad), mc_fma(a.y, cx, b.y, bad)};
}

/* FPRF's class of a plain single (classify_f32): normal or zero, by sign. */
MC u32 mc_class(f32 value) {
    const u32 bits = mc_bits(value);
    return (bits & 0x7FFFFFFFu) == 0u ? (bits >> 31 ? 0x12u : 0x02u) : (bits >> 31 ? 0x08u : 0x04u);
}

/* fcmpu's code: less, greater, equal (no NaN here). */
MC u32 mc_compare(f32 a, f32 b) { return a < b ? 0x8u : a > b ? 0x4u : 0x2u; }

MC void mc_set(CPUState* cpu, unsigned r, f32 x, f32 y) {
    cpu->fpr[r] = (f64)x;
    cpu->ps1[r] = (f64)y;
}

/* --- The callees' arithmetic. -------------------------------------------- */

/* J3DGetTranslateRotateMtx's rotation (both forms, from their fneg): from
 * the sines and cosines of x, y and z, the 3x3 part of `m`. */
MC void mc_rotation(f32 sx, f32 cx, f32 sy, f32 cy, f32 sz, f32 cz, f32 m[3][4], bool* bad) {
    m[2][0] = -sy;
    m[0][0] = mc_mul(cz, cy, bad);
    m[1][0] = mc_mul(sz, cy, bad);
    m[2][1] = mc_mul(cy, sx, bad);
    m[2][2] = mc_mul(cy, cx, bad);
    const f32 cxsz = mc_mul(cx, sz, bad), sxcz = mc_mul(sx, cz, bad);
    m[0][1] = mc_sub(mc_mul(sxcz, sy, bad), cxsz, bad);
    m[1][2] = mc_sub(mc_mul(cxsz, sy, bad), sxcz, bad);
    const f32 sxsz = mc_mul(sx, sz, bad), cxcz = mc_mul(cx, cz, bad);
    m[0][2] = mc_add(sxsz, mc_mul(cxcz, sy, bad), bad);
    m[1][1] = mc_add(cxcz, mc_mul(sxsz, sy, bad), bad);
}

/* PSMTXConcat(a, b, out) in its own order (8030D0FC): out = a x b, b's
 * fourth row (0, 0, 0, 1) through the unit pair. The registers it leaves:
 * f0 and f2 (row 2), f1, f3, f4, f5 (a), f6..f11 (b), f12 and f13 (row 0),
 * f14 and f15 (row 1, whose first halves it restores), f31 (the unit pair,
 * its first half restored). */
typedef struct McConcat {
    f32 out[3][4];
    McPair f0, f2, f12, f13, f14, f15;
} McConcat;

MC McConcat mc_concat(const f32 a[3][4], const f32 b[3][4], McPair unit, bool* bad) {
    McConcat r;
    const McPair a0 = {a[0][0], a[0][1]}, a1 = {a[0][2], a[0][3]}, a2 = {a[1][0], a[1][1]};
    const McPair a3 = {a[1][2], a[1][3]}, a4 = {a[2][0], a[2][1]}, a5 = {a[2][2], a[2][3]};
    const McPair b6 = {b[0][0], b[0][1]}, b7 = {b[0][2], b[0][3]}, b8 = {b[1][0], b[1][1]};
    const McPair b9 = {b[1][2], b[1][3]}, b10 = {b[2][0], b[2][1]}, b11 = {b[2][2], b[2][3]};
    r.f12 = mc_muls(b6, a0.x, bad);
    r.f13 = mc_muls(b7, a0.x, bad);
    r.f14 = mc_muls(b6, a2.x, bad);
    r.f15 = mc_muls(b7, a2.x, bad);
    r.f12 = mc_madds(b8, a0.y, r.f12, bad);
    r.f14 = mc_madds(b8, a2.y, r.f14, bad);
    r.f13 = mc_madds(b9, a0.y, r.f13, bad);
    r.f15 = mc_madds(b9, a2.y, r.f15, bad);
    r.f12 = mc_madds(b10, a1.x, r.f12, bad);
    r.f13 = mc_madds(b11, a1.x, r.f13, bad);
    r.f14 = mc_madds(b10, a3.x, r.f14, bad);
    r.f15 = mc_madds(b11, a3.x, r.f15, bad);
    r.f2 = mc_muls(b6, a4.x, bad);
    r.f13 = mc_madds(unit, a1.y, r.f13, bad);
    r.f0 = mc_muls(b7, a4.x, bad);
    r.f15 = mc_madds(unit, a3.y, r.f15, bad);
    r.f2 = mc_madds(b8, a4.y, r.f2, bad);
    r.f0 = mc_madds(b9, a4.y, r.f0, bad);
    r.f2 = mc_madds(b10, a5.x, r.f2, bad);
    r.f0 = mc_madds(b11, a5.x, r.f0, bad);
    r.f0 = mc_madds(unit, a5.y, r.f0, bad);
    const McPair rows[6] = {r.f12, r.f13, r.f14, r.f15, r.f2, r.f0};
    for (unsigned i = 0; i < 6u; ++i) {
        r.out[i / 2u][2u * (i % 2u)] = rows[i].x;
        r.out[i / 2u][2u * (i % 2u) + 1u] = rows[i].y;
    }
    return r;
}

/* The registers PSMTXConcat(a, b) leaves past those PSMTXCopy then loads. */
MC void mc_concat_registers(CPUState* cpu, const McConcat* c, const f32 b[3][4], McPair unit) {
    for (unsigned i = 0; i < 6u; ++i)
        mc_set(cpu, 6u + i, b[i / 2u][2u * (i % 2u)], b[i / 2u][2u * (i % 2u) + 1u]);
    mc_set(cpu, 12, c->f12.x, c->f12.y);
    mc_set(cpu, 13, c->f13.x, c->f13.y);
    cpu->ps1[14] = (f64)c->f14.y;
    cpu->ps1[15] = (f64)c->f15.y;
    cpu->ps1[31] = (f64)unit.y;
}

/* PSMTXCopy's f0..f5: the matrix it copied, a pair each. */
MC void mc_copy_registers(CPUState* cpu, const f32 m[3][4]) {
    for (unsigned i = 0; i < 6u; ++i)
        mc_set(cpu, i, m[i / 2u][2u * (i % 2u)], m[i / 2u][2u * (i % 2u) + 1u]);
}

/* --- The checks before anything changes. ------------------------------- */

MC bool mc_ram(const CPUState* cpu, u32 address, u32 size) {
    return ppc_dispatch_poll_read_stable((CPUState*)cpu, address, size);
}

/* Byte ranges [lo, hi), already found in RAM (so no end wraps): the four
 * the function stores to, and up to 16 it reads, unused ones empty. */
typedef struct McRange {
    u32 lo, hi;
} McRange;

#define MC_INPUTS 16u
typedef struct McSpans {
    u32 lo[MC_INPUTS], hi[MC_INPUTS];
} McSpans;

MC bool mc_apart(McRange a, McRange b) { return (a.hi <= b.lo) | (b.hi <= a.lo); }

/* The four stores' ranges apart from each other and from every input;
 * branch free, so the input comparisons run in vectors. */
MC bool mc_disjoint(const McRange stores[4], const McSpans* inputs) {
    const unsigned ok = (unsigned)mc_apart(stores[0], stores[1]) & (unsigned)mc_apart(stores[0], stores[2]) &
                        (unsigned)mc_apart(stores[0], stores[3]) & (unsigned)mc_apart(stores[1], stores[2]) &
                        (unsigned)mc_apart(stores[1], stores[3]) & (unsigned)mc_apart(stores[2], stores[3]);
    u32 hit = 0u;
    for (unsigned i = 0; i < 4u; ++i)
        for (unsigned j = 0; j < MC_INPUTS; ++j)
            hit |= (u32)(stores[i].lo < inputs->hi[j]) & (u32)(inputs->lo[j] < stores[i].hi);
    return ok != 0u && hit == 0u;
}

/* The CPU's state at entry: FP, unquantised pairs, round to nearest, no
 * exception or journal, the turn's budget past the longest path's last
 * charge, and the deadline (if any) beyond it, so the translation prepays
 * every block, never leaves a prepaid copy for the per-instruction path,
 * never stops for the turn, and runs every inline save and restore inline. */
MC bool mc_ready(const CPUState* cpu, s64 longest) {
    const u32 gqr = cpu->gqr[0];
    return cpu->exception == 0u && (cpu->msr & PPC_MSR_FP) != 0u && (cpu->hid2 & PPC_HID2_LSQE) != 0u &&
           ((gqr >> 16) & 7u) == 0u && (gqr & 7u) == 0u && (cpu->fpscr & 3u) == 0u && g_mem_write_journal == NULL &&
           cpu->cycle_budget > 0 && cpu->downcount - longest > -cpu->cycle_budget &&
           (cpu->cycle_deadline_budget <= 0 ||
            (cpu->cycle_deadline_budget >= longest && cpu->cycle_deadline_budget + cpu->downcount >= longest));
}

/* Each boundary between chunks the translation crosses passes without the
 * host: the edge filter's test (dispatch_loop.h), which the direct calls'
 * readiness (direct_calls.h) only narrows. */
MC bool mc_boundaries_silent(const CPUState* cpu, const u32* addresses, unsigned count) {
    if (!bw_edge_filter_enabled || !bw_edge_watch_ready || !bw_host_quiet(cpu))
        return false;
    for (unsigned i = 0; i < count; ++i)
        if (!bw_edge_unwatched(addresses[i]))
            return false;
    return true;
}

/* What the functions share: the transform, J3DSys's matrix and scales, the
 * model's arrays, the tables and the entries the angles select, and the
 * constants - read once, in RAM and plain, with the ranges they come from. */
typedef struct McInputs {
    u32 model, flags, anm, joint, shift, sine, cosine;
    u32 angles[3], offsets[3];
    f32 scale[3], translate[3], s[3], m[3][4], trig[6], one;
    McPair unit;
    McSpans spans;
} McInputs;

MC void mc_span(McSpans* spans, unsigned i, u32 at, u32 size) {
    spans->lo[i] = at;
    spans->hi[i] = at + size;
}

static bool mc_inputs(const CPUState* cpu, u32 info, McInputs* in) {
    const u32 tables = cpu->gpr[13] - 26460u, constant = cpu->gpr[2] - 13072u;
    if (!mc_ram(cpu, info, 32) || !mc_ram(cpu, J3D_SYS_MODEL, 4) || !mc_ram(cpu, J3D_CURRENT_MTX, J3D_STATICS_SIZE) ||
        !mc_ram(cpu, constant, 4) || !mc_ram(cpu, PSMTX_UNIT, 8) || !mc_ram(cpu, tables, 12))
        return false;
    in->model = mc_word(cpu, J3D_SYS_MODEL);
    if (!mc_ram(cpu, in->model + 132u, 4) || !mc_ram(cpu, in->model + 140u, 4))
        return false;
    in->flags = mc_word(cpu, in->model + 132u);
    in->anm = mc_word(cpu, in->model + 140u);
    in->joint = cpu->gpr[4] & 0xFFFFu;
    if (!mc_ram(cpu, in->flags + in->joint, 1) || !mc_ram(cpu, in->anm + in->joint * 48u, 48))
        return false;
    in->shift = mc_word(cpu, tables);
    in->sine = mc_word(cpu, tables + 4u);
    in->cosine = mc_word(cpu, tables + 8u);
    bool odd = false;
    for (unsigned k = 0; k < 3u; ++k) {
        /* lha; rlwinm 16..31; sraw by the shift; rlwinm by 2 */
        in->angles[k] = read_be16(mc_at(cpu, info + 12u + 2u * k));
        const u32 sh = in->shift & 0x3Fu;
        in->offsets[k] = ((sh == 0u ? in->angles[k] : sh > 31u ? 0u : in->angles[k] >> sh) << 2) & 0xFFFFFFFCu;
        if (!mc_ram(cpu, in->sine + in->offsets[k], 4) || !mc_ram(cpu, in->cosine + in->offsets[k], 4))
            return false;
        const u32 sine = mc_word(cpu, in->sine + in->offsets[k]), cosine = mc_word(cpu, in->cosine + in->offsets[k]);
        const u32 scale = mc_word(cpu, info + 4u * k), translate = mc_word(cpu, info + 20u + 4u * k);
        const u32 s = mc_word(cpu, J3D_CURRENT_S + 4u * k);
        in->trig[2u * k] = mc_float(sine);
        in->trig[2u * k + 1u] = mc_float(cosine);
        in->scale[k] = mc_float(scale);
        in->translate[k] = mc_float(translate);
        in->s[k] = mc_float(s);
        odd |= !mc_plain(sine) || !mc_plain(cosine) || !mc_plain(scale) || !mc_plain(translate) || !mc_plain(s);
        for (unsigned j = 0; j < 4u; ++j) {
            const u32 m = mc_word(cpu, J3D_CURRENT_MTX + 16u * k + 4u * j);
            in->m[k][j] = mc_float(m);
            odd |= !mc_plain(m);
        }
    }
    const u32 one = mc_word(cpu, constant), u0 = mc_word(cpu, PSMTX_UNIT), u1 = mc_word(cpu, PSMTX_UNIT + 4u);
    in->one = mc_float(one);
    in->unit = (McPair){mc_float(u0), mc_float(u1)};
    odd |= !mc_plain(one) || !mc_plain(u0) || !mc_plain(u1);
    if (odd)
        return false;
    memset(&in->spans, 0, sizeof in->spans);
    mc_span(&in->spans, 0, info, 32);
    mc_span(&in->spans, 1, J3D_SYS_MODEL, 4);
    mc_span(&in->spans, 2, in->model + 132u, 12); /* 132 and 140 */
    mc_span(&in->spans, 3, tables, 12);
    mc_span(&in->spans, 4, constant, 4);
    mc_span(&in->spans, 5, PSMTX_UNIT, 8);
    for (unsigned k = 0; k < 3u; ++k) {
        mc_span(&in->spans, 6u + 2u * k, in->sine + in->offsets[k], 4);
        mc_span(&in->spans, 7u + 2u * k, in->cosine + in->offsets[k], 4);
    }
    return true;
}

/* Three compares of 1.0 (f1) with `values` in turn, each run only while the
 * last was equal (fcmpu cr0, f1, f0; bne): whether all were equal, how many
 * ran, and the FPSCR codes they or in. */
MC bool mc_all_one(f32 one, const f32 values[3], unsigned* compares, u32* codes) {
    *codes = 0u;
    for (unsigned i = 0; i < 3u; ++i) {
        const u32 code = mc_compare(one, values[i]);
        *codes |= code;
        *compares = i + 1u;
        if (code != 0x2u)
            return false;
    }
    return true;
}

/* The frame's top - back chain, the saved LR, the saved registers - and
 * PSMTXConcat's frame below it (its caller's r1 = `frame`): back chain,
 * f14, f15, f31. Every value read before the first store. */
MC void mc_frames(const McOut* o, u32 sp, u32 size, unsigned first_saved) {
    const CPUState* cpu = o->cpu;
    const u32 lr = cpu->lr, frame = sp - size, low = frame - 64u;
    const u64 f14 = f64_bits(cpu->fpr[14]), f15 = f64_bits(cpu->fpr[15]), f31 = f64_bits(cpu->fpr[31]);
    u32 saved[5];
    for (unsigned r = first_saved; r < 32u; ++r)
        saved[r - first_saved] = cpu->gpr[r];
    mc_store(o, frame, sp);
    mc_store(o, sp + 4u, lr);
    for (unsigned r = first_saved; r < 32u; ++r)
        mc_store(o, sp - 4u * (32u - r), saved[r - first_saved]);
    mc_store(o, low, frame);
    mc_store64(o, low + 8u, f14);
    mc_store64(o, low + 16u, f15);
    mc_store64(o, low + 40u, f31);
}

MC void mc_store_matrix(const McOut* o, u32 at, const f32 m[3][4]) {
    for (unsigned r = 0; r < 3u; ++r)
        for (unsigned c = 0; c < 4u; ++c)
            mc_store(o, at + 16u * r + 4u * c, mc_bits(m[r][c]));
}

MC void mc_cr0(CPUState* cpu, bool less, bool greater) {
    const u32 bits = (less ? 0x8u : greater ? 0x4u : 0x2u) | ((cpu->xer >> 31) & 1u);
    cpu->cr = (cpu->cr & 0x0FFFFFFFu) | (bits << 28);
}

/* The end every form shares: FPSCR (FPRF; FI and FR cleared by the fmuls),
 * XER's carry (sraw's, clear), r0 and LR the saved LR, the cycles, mtlr's
 * suffix, the return. */
MC int mc_finish(CPUState* cpu, u32 fprf, s64 cycles) {
    cpu->fpscr = (cpu->fpscr & ~((0x1Fu << 12) | BW_FP_FPSCR_FI | BW_FP_FPSCR_FR)) | (fprf << 12);
    cpu->xer &= ~0x20000000u;
    cpu->gpr[0] = cpu->lr;
    cpu->downcount -= cycles;
    cpu->cycle_observation_suffix = 2u;
    cpu->pc = cpu->lr & ~3u;
    return 1;
}

/* --- J3DMtxCalcBasic::calcTransform(r4 jnt_no, r5 info) ---------------- */

static int mc_basic(CPUState* cpu) {
    static const u32 boundaries[] = {0x802DA64Cu, 0x802F5188u, 0x8030D0FCu, 0x802F521Cu, 0x8030D0C8u, 0x802F5244u};
    const u32 sp = cpu->gpr[1], info = cpu->gpr[5], frame = sp - 96u;
    McInputs in;
    if (!mc_ready(cpu, 240) || !mc_ram(cpu, sp - 160u, 168) || !mc_inputs(cpu, info, &in))
        return 0;
    const McRange stores[4] = {{sp - 160u, sp + 8u},
                               {J3D_CURRENT_MTX, J3D_CURRENT_MTX + J3D_STATICS_SIZE},
                               {in.flags + in.joint, in.flags + in.joint + 1u},
                               {in.anm + in.joint * 48u, in.anm + in.joint * 48u + 48u}};
    if (!mc_disjoint(stores, &in.spans) ||
        !mc_boundaries_silent(cpu, boundaries, sizeof boundaries / sizeof boundaries[0]))
        return 0;

    /* J3DSys::mCurrentS *= info.mScale; all three 1.0? */
    bool bad = false;
    f32 s[3];
    for (unsigned i = 0; i < 3u; ++i)
        s[i] = mc_mul(in.s[i], in.scale[i], &bad);
    unsigned compares;
    u32 codes;
    const bool one = mc_all_one(in.one, s, &compares, &codes);
    /* J3DGetTranslateRotateMtx(info, mtx), scaled unless one; PSMTXConcat. */
    f32 mtx[3][4];
    mc_rotation(in.trig[0], in.trig[1], in.trig[2], in.trig[3], in.trig[4], in.trig[5], mtx, &bad);
    for (unsigned r = 0; r < 3u; ++r) {
        mtx[r][3] = in.translate[r];
        if (!one)
            for (unsigned c = 0; c < 3u; ++c)
                mtx[r][c] = mc_mul(mtx[r][c], in.scale[c], &bad);
    }
    const McConcat out = mc_concat(in.m, mtx, in.unit, &bad);
    if (bad)
        return 0;

    /* The memory, as the function leaves it. */
    const McOut o = {cpu, cpu->ram, cpu->reserve_valid};
    mc_frames(&o, sp, 96, 29);
    for (unsigned i = 0; i < 3u; ++i) {
        mc_store(&o, J3D_CURRENT_S + 4u * i, mc_bits(s[i]));
        mc_store(&o, frame + 8u + 4u * i, mc_bits(s[i]));
    }
    mc_store8(&o, in.flags + in.joint, one ? 1u : 0u);
    mc_store_matrix(&o, frame + 20u, mtx);
    mc_store_matrix(&o, J3D_CURRENT_MTX, out.out);
    mc_store_matrix(&o, in.anm + in.joint * 48u, out.out);
    /* The registers. */
    mc_copy_registers(cpu, out.out);
    mc_concat_registers(cpu, &out, mtx, in.unit);
    cpu->gpr[3] = J3D_CURRENT_MTX;
    cpu->gpr[4] = in.anm + in.joint * 48u;
    cpu->gpr[5] = J3D_CURRENT_MTX;
    cpu->gpr[6] = PSMTX_UNIT;
    cpu->gpr[7] = in.cosine;
    cpu->gpr[11] = sp;
    mc_cr0(cpu, false, one); /* cmpwi r29, 0 */
    /* 802F5090 (5), _savegpr_29 (4), 802F50A4 (26), the compares' blocks,
     * 802F5124 (2) or 802F512C (1), 802F5130 (2), 802F5138 (9) or 802F515C
     * (8), 802F517C (3), J3DGetTranslateRotateMtx (54), 802F5188 (2),
     * 802F5190 (30) unless one, 802F5208 (5), PSMTXConcat (51), 802F521C
     * (12), PSMTXCopy (13), 802F5244 (2), _restgpr_29 (4), 802F524C (5). */
    const s64 cycles = 5 + 4 + 26 + 3 * (s64)(compares - 1u) + (one ? 2 + 9 : 1 + 8) + 2 + 3 + 54 + 2 +
                       (one ? 0 : 30) + 5 + 51 + 12 + 13 + 2 + 4 + 5;
    return mc_finish(cpu, mc_class(out.out[2][2]), cycles);
}

/* --- J3DMtxCalcSoftimage::calcTransform(r4 jnt_no, r5 info) ------------ */

static int mc_softimage(CPUState* cpu) {
    static const u32 boundaries[] = {0x802DA724u, 0x802F5318u, 0x8030D0FCu, 0x802F532Cu,
                                     0x8030D0C8u, 0x802F54C4u, 0x802F54F0u};
    const u32 sp = cpu->gpr[1], info = cpu->gpr[5], frame = sp - 96u;
    McInputs in;
    if (!mc_ready(cpu, 256) || !mc_ram(cpu, sp - 160u, 168) || !mc_inputs(cpu, info, &in))
        return 0;
    const McRange stores[4] = {{sp - 160u, sp + 8u},
                               {J3D_CURRENT_MTX, J3D_CURRENT_MTX + J3D_STATICS_SIZE},
                               {in.flags + in.joint, in.flags + in.joint + 1u},
                               {in.anm + in.joint * 48u, in.anm + in.joint * 48u + 48u}};
    if (!mc_disjoint(stores, &in.spans) ||
        !mc_boundaries_silent(cpu, boundaries, sizeof boundaries / sizeof boundaries[0]))
        return 0;

    /* J3DGetTranslateRotateMtx(rx, ry, rz, info.mTranslate * mCurrentS, mtx);
     * PSMTXConcat(mCurrentMtx, mtx, mCurrentMtx). */
    bool bad = false;
    f32 mtx[3][4];
    mc_rotation(in.trig[0], in.trig[1], in.trig[2], in.trig[3], in.trig[4], in.trig[5], mtx, &bad);
    for (unsigned r = 0; r < 3u; ++r)
        mtx[r][3] = mc_mul(in.translate[r], in.s[r], &bad);
    const McConcat out = mc_concat(in.m, mtx, in.unit, &bad);
    /* mCurrentS *= info.mScale; all three 1.0? Else the animation matrix is
     * the new current matrix with its columns scaled by mCurrentS. */
    f32 s[3];
    for (unsigned i = 0; i < 3u; ++i)
        s[i] = mc_mul(in.s[i], in.scale[i], &bad);
    unsigned compares;
    u32 codes;
    const bool one = mc_all_one(in.one, s, &compares, &codes);
    f32 anm[3][4];
    for (unsigned r = 0; r < 3u; ++r) {
        for (unsigned c = 0; c < 3u; ++c)
            anm[r][c] = one ? out.out[r][c] : mc_mul(out.out[r][c], s[c], &bad);
        anm[r][3] = out.out[r][3];
    }
    if (bad)
        return 0;

    const McOut o = {cpu, cpu->ram, cpu->reserve_valid};
    mc_frames(&o, sp, 96, 29);
    mc_store_matrix(&o, frame + 20u, one ? mtx : anm);
    mc_store_matrix(&o, J3D_CURRENT_MTX, out.out);
    for (unsigned i = 0; i < 3u; ++i) {
        mc_store(&o, J3D_CURRENT_S + 4u * i, mc_bits(s[i]));
        mc_store(&o, frame + 8u + 4u * i, mc_bits(s[i]));
    }
    mc_store8(&o, in.flags + in.joint, one ? 1u : 0u);
    mc_store_matrix(&o, in.anm + in.joint * 48u, anm);
    mc_copy_registers(cpu, anm);
    mc_concat_registers(cpu, &out, mtx, in.unit);
    cpu->gpr[3] = one ? J3D_CURRENT_MTX : frame + 20u;
    cpu->gpr[4] = in.anm + in.joint * 48u;
    cpu->gpr[5] = J3D_CURRENT_MTX;
    cpu->gpr[6] = PSMTX_UNIT;
    cpu->gpr[7] = in.sine;
    cpu->gpr[8] = in.cosine;
    cpu->gpr[11] = sp;
    mc_cr0(cpu, false, one); /* cmpwi r0, 0 */
    /* 802F52BC (5), _savegpr_29 (4), 802F52D0 (18), J3DGetTranslateRotateMtx
     * (48), 802F5318 (5), PSMTXConcat (51), 802F532C (24), the compares'
     * blocks, 802F53A4 (2) or 802F53AC (1), 802F53B0 (2), 802F53B8 (9) or
     * 802F53DC (8), 802F53FC (2), then 802F5404 (50), PSMTXCopy (13) and
     * 802F54C4 (1) - or 802F54C8 (12) and PSMTXCopy (13) - then 802F54F0
     * (2), _restgpr_29 (4), 802F54F8 (5). FPRF: the last fmuls's, or with the
     * scale one, mCurrentS.z's with the compares' codes. */
    const s64 cycles = 5 + 4 + 18 + 48 + 5 + 51 + 24 + 3 * (s64)(compares - 1u) + (one ? 2 + 9 : 1 + 8) + 2 + 2 +
                       (one ? 12 + 13 : 50 + 13 + 1) + 2 + 4 + 5;
    const u32 fprf = one ? mc_class(s[2]) | codes : mc_class(anm[2][2]);
    return mc_finish(cpu, fprf, cycles);
}

/* --- J3DMtxCalcMaya::calcTransform(r4 jnt_no, r5 info) ----------------- */

static int mc_maya(CPUState* cpu) {
    static const u32 boundaries[] = {0x802DA64Cu, 0x802F55A4u, 0x8030D0FCu, 0x802F56D0u,
                                     0x802F56E0u, 0x8030D0C8u, 0x802F56F0u};
    const u32 sp = cpu->gpr[1], info = cpu->gpr[5], frame = sp - 80u;
    McInputs in;
    if (!mc_ready(cpu, 320) || !mc_ram(cpu, sp - 144u, 152) || !mc_inputs(cpu, info, &in) ||
        !mc_ram(cpu, in.model + 4u, 4))
        return 0;
    /* model->mModelData->mJointNodePointer[jnt_no]->mScaleCompensate */
    const u32 data = mc_word(cpu, in.model + 4u);
    if (!mc_ram(cpu, data + 44u, 4))
        return 0;
    const u32 slot = mc_word(cpu, data + 44u) + ((cpu->gpr[4] << 2) & 0x0003FFFCu);
    if (!mc_ram(cpu, slot, 4))
        return 0;
    const u32 node = mc_word(cpu, slot);
    if (!mc_ram(cpu, node + 27u, 1))
        return 0;
    const u32 compensate = *mc_at(cpu, node + 27u);
    mc_span(&in.spans, 12, in.model + 4u, 4);
    mc_span(&in.spans, 13, data + 44u, 4);
    mc_span(&in.spans, 14, slot, 4);
    mc_span(&in.spans, 15, node + 27u, 1);
    const McRange stores[4] = {{sp - 144u, sp + 8u},
                               {J3D_CURRENT_MTX, J3D_CURRENT_MTX + J3D_STATICS_SIZE},
                               {in.flags + in.joint, in.flags + in.joint + 1u},
                               {in.anm + in.joint * 48u, in.anm + in.joint * 48u + 48u}};
    if (!mc_disjoint(stores, &in.spans) ||
        !mc_boundaries_silent(cpu, boundaries, sizeof boundaries / sizeof boundaries[0]))
        return 0;
    f32 parent[3] = {0.0f, 0.0f, 0.0f};
    if (compensate == 1u)
        for (unsigned i = 0; i < 3u; ++i) {
            const u32 bits = mc_word(cpu, J3D_PARENT_S + 4u * i);
            if (!mc_plain(bits))
                return 0;
            parent[i] = mc_float(bits);
        }

    /* info.mScale all 1.0?; J3DGetTranslateRotateMtx(info, mtx), scaled
     * unless one, its rows divided by J3DSys::mParentS when compensating;
     * PSMTXConcat. */
    bool bad = false;
    unsigned compares;
    u32 codes;
    const bool one = mc_all_one(in.one, in.scale, &compares, &codes);
    f32 mtx[3][4];
    mc_rotation(in.trig[0], in.trig[1], in.trig[2], in.trig[3], in.trig[4], in.trig[5], mtx, &bad);
    for (unsigned r = 0; r < 3u; ++r) {
        mtx[r][3] = in.translate[r];
        if (!one)
            for (unsigned c = 0; c < 3u; ++c)
                mtx[r][c] = mc_mul(mtx[r][c], in.scale[c], &bad);
    }
    if (compensate == 1u)
        for (unsigned r = 0; r < 3u; ++r) {
            const f32 inverse = mc_div(in.one, parent[r], &bad);
            for (unsigned c = 0; c < 3u; ++c)
                mtx[r][c] = mc_mul(mtx[r][c], inverse, &bad);
        }
    const McConcat out = mc_concat(in.m, mtx, in.unit, &bad);
    if (bad)
        return 0;

    const McOut o = {cpu, cpu->ram, cpu->reserve_valid};
    mc_frames(&o, sp, 80, 27);
    mc_store8(&o, in.flags + in.joint, one ? 1u : 0u);
    mc_store_matrix(&o, frame + 8u, mtx);
    mc_store_matrix(&o, J3D_CURRENT_MTX, out.out);
    mc_store_matrix(&o, in.anm + in.joint * 48u, out.out);
    for (unsigned i = 0; i < 3u; ++i)
        mc_store(&o, J3D_PARENT_S + 4u * i, mc_bits(in.scale[i]));
    mc_copy_registers(cpu, out.out);
    mc_concat_registers(cpu, &out, mtx, in.unit);
    mc_set(cpu, 0, in.scale[2], in.scale[2]); /* lfs f0, 8(r31): the last of mParentS = info.mScale */
    cpu->gpr[3] = J3D_PARENT_S;
    cpu->gpr[4] = in.anm + in.joint * 48u;
    cpu->gpr[5] = J3D_CURRENT_MTX;
    cpu->gpr[6] = PSMTX_UNIT;
    cpu->gpr[7] = in.cosine;
    cpu->gpr[11] = sp;
    mc_cr0(cpu, compensate < 1u, compensate > 1u); /* cmplwi r0, 1 */
    /* 802F5508 (5), _savegpr_27 (6), 802F551C (15), the compares' blocks,
     * 802F5570 or 802F5584 (5), 802F5598 (3), J3DGetTranslateRotateMtx (54),
     * 802F55A4 (2), 802F55AC (30) unless one, 802F5624 (3), 802F5630 (83)
     * when compensating, 802F56BC (5), PSMTXConcat (51), 802F56D0 (4),
     * 802F56E0 (6), PSMTXCopy (13), 802F56F0 (9), _restgpr_27 (6), 802F5714
     * (5). */
    const s64 cycles = 5 + 6 + 15 + 3 * (s64)(compares - 1u) + 5 + 3 + 54 + 2 + (one ? 0 : 30) + 3 +
                       (compensate == 1u ? 83 : 0) + 5 + 51 + 4 + 6 + 13 + 9 + 6 + 5;
    return mc_finish(cpu, mc_class(out.out[2][2]), cycles);
}

int bluewake_native_mtxcalc(CPUState* cpu, u32 address) {
    unsigned which;
    int done;
    switch (address) {
    case BLUEWAKE_MTXCALC_BASIC: which = MC_BASIC; done = mc_basic(cpu); break;
    case BLUEWAKE_MTXCALC_SOFTIMAGE: which = MC_SOFTIMAGE; done = mc_softimage(cpu); break;
    case BLUEWAKE_MTXCALC_MAYA: which = MC_MAYA; done = mc_maya(cpu); break;
    default: return 0;
    }
    if (done)
        s_mc_runs[which]++;
    else
        s_mc_declined[which]++;
    return done;
}
