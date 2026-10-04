/* The collision checker's area division (GZLE01
 * cCcD_DivideArea::CalcDivideInfoOverArea), native.
 *
 * Every collider the checker sets each frame has its bounding box divided
 * over the area's 11 x 10 x 11 cells: per axis, the box's two ends scaled to
 * cell indices (fsubs, fmuls, fctiwz through the stack), clamped, and turned
 * into a run of bits; the three runs or'ed into one word. A leaf, with
 * twenty-odd blocks of compares and shifts behind each axis's conversions:
 * 0.8 percent of the game thread at Forest Haven.
 *
 * It replays the blocks in order on the same values (native_replay.h: the
 * translation's inline floating-point paths on a local FPSCR, fctiwz as
 * GXRuntime's ppc_fctiw leaves it, the clock block by block) and commits only
 * at the end: the frame's stores (each conversion slot as its last stfd
 * leaves it), the divide info word, every register as the last instruction to
 * write it left it (r0, r3 and r5 to r10 as each axis uses them, f0 to f2 with
 * the second halves the single operations wrote), CR0 from the last compare,
 * the FPSCR, the reservation, the cycles, the cycle suffix (the last stfd's
 * 3, where an axis converts, or the lean stores' where a reservation is held:
 * the last access's), pc at the return address.
 *
 * It declines, changing nothing, unless that is certain: FP available,
 * rounding to nearest, no exception pending, no write journal, the frame and
 * the info word plain RAM, every read plain RAM and clear of the frame, every
 * block prepaid and none stopped for the budget, and every floating-point
 * operation on the translation's inline path (a NaN or infinite bound or
 * scale declines).
 *
 * tests/native_cc_test.c compares it with the translation, every register
 * and byte. No identifier here may be `ctx`. */
#include "native_cc.h"
#include "native_replay.h"

#include <stdio.h>

int bluewake_native_cc_enabled;
static unsigned long long s_cc_runs, s_cc_declined;

void bluewake_native_cc_report(void) {
    fprintf(stderr, "[native-cc] divide-over-area=%llu/%llu (native/declined)\n", s_cc_runs, s_cc_declined);
}

#define CC static inline __attribute__((always_inline))

/* One axis: its fields in the area (the zero flag, the inverse scaled
 * difference, the minimum), the box's minimum and maximum, the clamp, the
 * full run, the frame slots its two conversions use, and its registers. */
typedef struct CcAxis {
    u32 flag, inverse, minimum, box_min, box_max;
    s32 limit;
    u32 full, slot1, slot3;
    unsigned v1, v3, one, run;
} CcAxis;

static const CcAxis CC_AXES[3] = {
    {28u, 36u, 0u, 0u, 12u, 10, 0x7FFu, 8u, 16u, 9u, 6u, 7u, 8u},
    {40u, 48u, 4u, 4u, 16u, 9, 0x3FFu, 16u, 8u, 10u, 6u, 7u, 9u},
    {52u, 60u, 8u, 8u, 20u, 10, 0x7FFu, 16u, 8u, 7u, 3u, 5u, 6u},
};

typedef struct Cc {
    u32 g[11]; /* r0 .. r10 */
    NrPair f[3];
    u32 cr, xer, suffix;
    NrFp fp;
    NrClock k;
    bool bad;
    u64 slot[2]; /* the frame's 8(r1) and 16(r1) */
    bool slot_written[2];
} Cc;

CC void cc_block(Cc* s, u32 cycles) { s->bad |= !nr_block(&s->k, cycles); }
CC void cc_cmpw(Cc* s, s32 a, s32 b) { s->cr = nr_cr0_signed(s->cr, s->xer, a, b); }
CC u32 cc_slw(u32 value, u32 amount) {
    const u32 sh = amount & 0x3Fu;
    return sh > 31u ? 0u : value << sh;
}

/* lfs fD; fsubs; fmuls; fctiwz; stfd; lwz: one end of the box as a cell
 * index. In single precision where the end, the minimum and the scale are
 * plain singles, their difference and product too, and the product within
 * fctiwz's range (native_replay.h); otherwise the double replay. */
CC u32 cc_convert(Cc* s, u32 end_word, u32 slot) {
    const f64 end = nr_single_bits(end_word);
    f32 e, m, c;
    bool bad = !nr_plain_word(end_word, &e) | !nr_plain(s->f[1].a, &m) | !nr_plain(s->f[2].a, &c);
    const f32 d = nr_r(e - m, &bad), p = nr_r(c * d, &bad);
    u64 stored;
    if (!bad && fabsf(p) < 2147483648.0f) {
        /* fsubs's FPRF, then fmuls's (and FI and FR cleared); fctiwz's:
         * FI and FR cleared, and XX with FI where it rounds. */
        const f32 t = truncf(p);
        const s32 r = (s32)t;
        s->fp.fpscr = (s->fp.fpscr & ~((0x1Fu << 12) | NR_FPSCR_FI | NR_FPSCR_FR)) | (nr_class_plain(p) << 12);
        if (t != p) {
            nr_fp_exception(&s->fp, NR_FPSCR_XX);
            s->fp.fpscr |= NR_FPSCR_FI;
        }
        u32 p_bits;
        memcpy(&p_bits, &p, sizeof p_bits);
        stored = 0xFFF8000000000000ull | (u32)r | ((r == 0 && (nr_opaque32(p_bits) >> 31) != 0u) ? 0x100000000ull : 0ull);
        s->f[0] = (NrPair){f64_value(stored), (f64)p};
    } else {
        const f64 difference = nr_fsubs(&s->fp, end, s->f[1].a);       /* fsubs f0, f0, f1 */
        const f64 product = nr_fmuls(&s->fp, s->f[2].a, difference);   /* fmuls f0, f2, f0 */
        s->f[0] = (NrPair){product, product};
        u64 bits;
        if (nr_fctiwz(&s->fp, product, &bits))                         /* fctiwz f0, f0 */
            s->f[0].a = f64_value(bits);
        stored = nr_bits(s->f[0].a);                                   /* stfd f0, slot(r1) */
    }
    s->slot[slot == 16u] = stored;
    s->slot_written[slot == 16u] = true;
    return (u32)stored;                                                /* lwz rD, slot+4(r1) */
}

CC void cc_axis(Cc* s, const CPUState* cpu, const CcAxis* a, u32 self, u32 box) {
    /* The 16-cycle block: the scale, the box's minimum, the area's minimum,
     * the two conversions; cmpwi var1, 0; bge */
    cc_block(s, 16u);
    const f64 scale = nr_single_bits(nr_word(cpu, self + a->inverse));
    s->f[2] = (NrPair){scale, scale};
    const f64 minimum = nr_single_bits(nr_word(cpu, self + a->minimum));
    s->f[1] = (NrPair){minimum, minimum};
    u32* g = s->g;
    g[a->v1] = cc_convert(s, nr_word(cpu, box + a->box_min), a->slot1);
    g[a->v3] = cc_convert(s, nr_word(cpu, box + a->box_max), a->slot3);
    s->suffix = 3u;
    const s32 v1 = (s32)g[a->v1], limit = a->limit;
    cc_cmpw(s, v1, 0);
    bool zero = false;
    if (v1 < 0) {
        cc_block(s, 2u); /* cmpwi var3, 0; blt zero */
        cc_cmpw(s, (s32)g[a->v3], 0);
        zero = (s32)g[a->v3] < 0;
    }
    if (!zero) {
        cc_block(s, 2u); /* cmpwi var1, limit; ble clamp */
        cc_cmpw(s, v1, limit);
        if (v1 > limit) {
            cc_block(s, 2u); /* cmpwi var3, limit; ble clamp */
            cc_cmpw(s, (s32)g[a->v3], limit);
            zero = (s32)g[a->v3] > limit;
        }
    }
    if (zero) {
        cc_block(s, 2u); /* li run, 0; b next */
        g[a->run] = 0u;
        return;
    }
    cc_block(s, 2u); /* cmpwi var3, limit; ble */
    cc_cmpw(s, (s32)g[a->v3], limit);
    if ((s32)g[a->v3] > limit) {
        cc_block(s, 1u); /* li var3, limit */
        g[a->v3] = (u32)limit;
    }
    /* li one, 1; addi r0, var3, 1; slw var3, one, r0; addi run, var3, -1;
     * cmpwi var1, 0; ble next (6) */
    cc_block(s, 6u);
    g[a->one] = 1u;
    g[0] = g[a->v3] + 1u;
    g[a->v3] = cc_slw(1u, g[0]);
    g[a->run] = g[a->v3] - 1u;
    cc_cmpw(s, v1, 0);
    if (v1 > 0) {
        /* addi r0, var1, -1; slw var3, one, r0; addi r0, var3, -1; andc run, run, r0 (5) */
        cc_block(s, 5u);
        g[0] = (u32)v1 - 1u;
        g[a->v3] = cc_slw(1u, g[0]);
        g[0] = g[a->v3] - 1u;
        g[a->run] &= ~g[0];
    }
}

/* The zero flag's test: lbz r0, flag(r3); cmplwi r0, 0; bne the full run. */
CC bool cc_flag(Cc* s, const CPUState* cpu, const CcAxis* a, u32 self) {
    s->g[0] = nr_at(cpu, self + a->flag)[0];
    s->cr = nr_cr0_unsigned(s->cr, s->xer, s->g[0], 0u);
    return s->g[0] != 0u;
}

static int cc_divide(CPUState* cpu) {
    Cc s;
    if (!nr_clock_start(cpu, &s.k))
        return 0;
    const u32 sp = cpu->gpr[1], frame = sp - 32u, self = cpu->gpr[3], out = cpu->gpr[4], box = cpu->gpr[5];
    /* Stores: the frame [sp - 32, sp - 8) and the info word. Reads: the
     * area's fields and the box, clear of the frame. */
    if (!nr_ram(cpu, frame, 24u) || !nr_ram(cpu, out, 4u) || !nr_ram(cpu, self, 64u) || !nr_ram(cpu, box, 24u) ||
        !nr_apart(self, 64u, frame, 24u) || !nr_apart(box, 24u, frame, 24u))
        return 0;
    for (unsigned i = 0; i < 11u; ++i)
        s.g[i] = cpu->gpr[i];
    for (unsigned r = 0; r < 3u; ++r)
        s.f[r] = (NrPair){cpu->fpr[r], cpu->ps1[r]};
    s.cr = cpu->cr;
    s.xer = cpu->xer;
    s.suffix = cpu->cycle_observation_suffix;
    s.fp = (NrFp){cpu->fpscr, false};
    s.bad = false;
    s.slot_written[0] = s.slot_written[1] = false;
    /* 8024170C (4): stwu r1, -32(r1); the x flag */
    cc_block(&s, 4u);
    if (cc_flag(&s, cpu, &CC_AXES[0], self)) {
        cc_block(&s, 1u); /* li r8, 2047 */
        s.g[8] = 0x7FFu;
    } else {
        cc_axis(&s, cpu, &CC_AXES[0], self, box);
    }
    /* 802417B8 (3): the y flag */
    cc_block(&s, 3u);
    if (cc_flag(&s, cpu, &CC_AXES[1], self)) {
        cc_block(&s, 1u); /* li r9, 1023 */
        s.g[9] = 0x3FFu;
    } else {
        cc_axis(&s, cpu, &CC_AXES[1], self, box);
    }
    /* 80241860 (5): rlwinm r0, r9, 11, 0, 20; or r8, r8, r0; the z flag */
    cc_block(&s, 5u);
    s.g[0] = ((s.g[9] << 11) | (s.g[9] >> 21)) & 0xFFFFF800u;
    s.g[8] |= s.g[0];
    if (cc_flag(&s, cpu, &CC_AXES[2], self)) {
        cc_block(&s, 1u); /* li r6, 2047 */
        s.g[6] = 0x7FFu;
    } else {
        cc_axis(&s, cpu, &CC_AXES[2], self, box);
    }
    /* 80241910 (5): rlwinm r0, r6, 21, 0, 10; or r8, r8, r0; stw r8, 0(r4);
     * addi r1, r1, 32; blr */
    cc_block(&s, 5u);
    s.g[0] = ((s.g[6] << 21) | (s.g[6] >> 11)) & 0xFFE00000u;
    s.g[8] |= s.g[0];
    if (s.bad || s.fp.bad)
        return 0;

    /* The stores, in order. The stwu and the final stw are lean stores
     * (scripts/windows/lean_memory.py): while a reservation is held they go
     * out of line and store their cycle suffix (3, 2) on the way; the
     * conversions' stfd store theirs always (the last, 3). */
    const NrOut o = nr_out(cpu);
    const bool reserved_first = cpu->reserve_valid;
    nr_store32(&o, frame, sp);
    for (unsigned i = 0; i < 2u; ++i)
        if (s.slot_written[i])
            nr_store64(&o, frame + 8u + 8u * i, s.slot[i]);
    const bool reserved_last = cpu->reserve_valid;
    nr_store32(&o, out, s.g[8]);
    if (reserved_first && !s.slot_written[0] && !s.slot_written[1])
        s.suffix = 3u;
    if (reserved_last)
        s.suffix = 2u;
    for (unsigned i = 0; i < 11u; ++i)
        if (i != 1u && i != 2u && i != 4u)
            cpu->gpr[i] = s.g[i];
    for (unsigned r = 0; r < 3u; ++r) {
        cpu->fpr[r] = s.f[r].a;
        cpu->ps1[r] = s.f[r].b;
    }
    cpu->cr = s.cr;
    cpu->fpscr = s.fp.fpscr;
    cpu->downcount = s.k.downcount;
    cpu->cycle_observation_suffix = s.suffix;
    cpu->pc = cpu->lr & ~3u;
    return 1;
}

int bluewake_native_cc(CPUState* cpu, u32 address) {
    if (address != BLUEWAKE_CC_DIVIDE_OVER_AREA)
        return 0;
    const int done = cc_divide(cpu);
    if (done)
        s_cc_runs++;
    else
        s_cc_declined++;
    return done;
}
