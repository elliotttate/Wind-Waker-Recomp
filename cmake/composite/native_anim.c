/* J3D's key-frame animation (GZLE01 JMAHermiteInterpolation,
 * J3DGetKeyFrameInterpolation<f32>, J3DGetKeyFrameInterpolationS with
 * J3DHermiteInterpolationS, J3DAnmTransformKey::calcTransform) and
 * J3DPSCalcInverseTranspose, native.
 *
 * Every animated joint of every model runs calcTransform each frame: nine
 * channels (scale, rotation and translation in x, y and z), each a constant,
 * one key, or a binary search over its keys and a Hermite spline between two
 * of them - J3DGetKeyFrameInterpolation<f32> in another chunk, which calls
 * JMAHermiteInterpolation in a third, for scale and translation, and
 * J3DGetKeyFrameInterpolationS with J3DHermiteInterpolationS (signed shorts
 * through GQR5) for rotation. Together they were 2.2 to 2.5 percent of the
 * game thread at Forest Haven and Dragon Roost; the second set's partial
 * calcTransform (zero and one-key channels only) ran in about a fifth of the
 * calls, and its J3DGetKeyFrameInterpolationS in about a fifth too (it took
 * only word-aligned tables). J3DPSCalcInverseTranspose, the normal matrices'
 * paired-single leaf, was 0.7 percent at Forest Haven.
 *
 * Each replays the translation's blocks in order on the same values
 * (native_replay.h: the inline floating-point paths on a local FPSCR, the
 * clock block by block, a binary search's iterations included), keeping the
 * registers it touches in a local copy and its stores in a log, and commits
 * only at the end: every frame store (each callee's frame as its last call
 * leaves it), the transform's nine fields, every register as the last
 * instruction to write it left it (r0, r1, r3 to r8, r11, f0 to f13 with the
 * second halves the single operations and loads wrote, f31's second half as
 * the paired save and restore round it, CR0 from the last compare or cror,
 * XER's CA from the search's srawi and addze, LR), the FPSCR (each fcmpo's
 * code or'ed in, the FPRF of the last arithmetic), the reservation, the
 * cycles, the last cycle suffix (mtlr's 2; the leaves' last load's), pc at
 * the return address.
 *
 * It declines, changing nothing, unless that is certain: FP available,
 * rounding to nearest, paired singles enabled (and unscaled for the saves;
 * GQR5 loading signed shorts), no exception pending, no write journal, every
 * address read plain RAM and clear of every store (the frames, the
 * transform), the stores plain RAM, the conversion's magic double the usual
 * one, every block prepaid and none stopped for the budget, every
 * floating-point operation on the translation's inline path (a NaN key time,
 * say, which fcmpo hands the interpreter, declines), and every boundary
 * between chunks the path crosses (the calls into
 * J3DGetKeyFrameInterpolation<f32> and JMAHermiteInterpolation and their
 * returns) passing without the host, as in play (the edge filter on, the
 * host's edge service quiet, none of those addresses watched).
 *
 * tests/native_anim_test.c compares each with the translation, every
 * register and byte. No identifier here may be `ctx`. */
#include "native_anim.h"
#include "native_replay.h"
#include "direct_calls.h"

#include <stdio.h>

int bluewake_native_anim_enabled;

enum { AN_HERMITE, AN_KEY_F, AN_KEY_S, AN_TRANSFORM, AN_INVERSE_TRANSPOSE, AN_COUNT };
static unsigned long long s_an_runs[AN_COUNT], s_an_declined[AN_COUNT], s_an_general[AN_COUNT];

void bluewake_native_anim_report(void) {
    fprintf(stderr,
            "[native-anim] hermite=%llu/%llu key-f=%llu/%llu key-s=%llu/%llu transform=%llu/%llu "
            "inverse-transpose=%llu/%llu (native/declined; general replays %llu %llu %llu %llu %llu)\n",
            s_an_runs[0], s_an_declined[0], s_an_runs[1], s_an_declined[1], s_an_runs[2], s_an_declined[2],
            s_an_runs[3], s_an_declined[3], s_an_runs[4], s_an_declined[4], s_an_general[0], s_an_general[1],
            s_an_general[2], s_an_general[3], s_an_general[4]);
}

/* How many calls the general replay ran (the tests read it). */
unsigned long long bluewake_native_anim_general(unsigned which) { return which < AN_COUNT ? s_an_general[which] : 0u; }

#define AN static inline __attribute__((always_inline))
/* The callees' replays: out of line, so the transform's nine channels share one copy. */
#define AN_CALLEE static __attribute__((noinline))
#define AN_MAGIC 0x4330000080000000ull
#define AN_LOG_MAX 96u

/* --- The replay's machine. ------------------------------------------------ */

typedef struct AnStore {
    u32 address, size;
    u64 value;
} AnStore;

typedef struct AnLog {
    unsigned count;
    AnStore stores[AN_LOG_MAX];
} AnLog;

typedef struct An {
    CPUState* cpu;
    AnLog* log;
    u32 g[32];
    f64 f[32], p[32];
    u32 cr, xer, lr;
    NrFp fp;
    NrClock k;
    u32 suffix;
    bool bad, crossed;
    bool retry; /* the fast replay did not apply: the general one may */
    /* The fast replay: its reads lie in ranges checked before it starts
     * (an_valid), so it reads them unchecked; it takes the splines only in
     * single precision. The general replay checks each read. */
    bool fast;
    const u8* ram;
    /* Every store lands in [lo0, hi0) or [lo1, hi1); no read may touch them. */
    u32 lo0, hi0, lo1, hi1;
} An;

/* The registers a replay here may write: loaded at the start, written back
 * at the end (unchanged where untouched). */
static const unsigned AN_GPRS[] = {0, 1, 3, 4, 5, 6, 7, 8, 11, 28, 29, 30, 31};
#define AN_GPR_COUNT (sizeof AN_GPRS / sizeof AN_GPRS[0])
#define AN_FPR_LAST 13u /* f0..f13, and f31 */

AN void an_start(An* s, CPUState* cpu, AnLog* log) {
    s->cpu = cpu;
    s->log = log;
    log->count = 0u;
    for (unsigned i = 0; i < AN_GPR_COUNT; ++i)
        s->g[AN_GPRS[i]] = cpu->gpr[AN_GPRS[i]];
    for (unsigned r = 0; r <= AN_FPR_LAST; ++r) {
        s->f[r] = cpu->fpr[r];
        s->p[r] = cpu->ps1[r];
    }
    s->f[31] = cpu->fpr[31];
    s->p[31] = cpu->ps1[31];
    s->cr = cpu->cr;
    s->xer = cpu->xer;
    s->lr = cpu->lr;
    s->fp = (NrFp){cpu->fpscr, false};
    s->suffix = cpu->cycle_observation_suffix;
    s->bad = s->crossed = s->retry = false;
    s->fast = false;
    s->ram = cpu->ram;
    s->lo0 = s->hi0 = s->lo1 = s->hi1 = 0u;
}

AN void an_commit(An* s) {
    CPUState* cpu = s->cpu;
    const NrOut o = nr_out(cpu);
    for (unsigned i = 0; i < s->log->count; ++i) {
        const AnStore* w = &s->log->stores[i];
        switch (w->size) {
        case 2: nr_store16(&o, w->address, (u16)w->value); break;
        case 4: nr_store32(&o, w->address, (u32)w->value); break;
        default: nr_store64(&o, w->address, w->value); break;
        }
    }
    for (unsigned i = 0; i < AN_GPR_COUNT; ++i)
        cpu->gpr[AN_GPRS[i]] = s->g[AN_GPRS[i]];
    for (unsigned r = 0; r <= AN_FPR_LAST; ++r) {
        cpu->fpr[r] = s->f[r];
        cpu->ps1[r] = s->p[r];
    }
    cpu->fpr[31] = s->f[31];
    cpu->ps1[31] = s->p[31];
    cpu->cr = s->cr;
    cpu->xer = s->xer;
    cpu->lr = s->lr;
    cpu->fpscr = s->fp.fpscr;
    cpu->downcount = s->k.downcount;
    cpu->cycle_observation_suffix = s->suffix;
    cpu->pc = s->lr & ~3u;
}

AN bool an_ok(const An* s) { return !s->bad && !s->fp.bad; }

AN void an_log(An* s, u32 address, u32 size, u64 value) {
    AnLog* log = s->log;
    if (log->count >= AN_LOG_MAX) {
        s->bad = true;
        return;
    }
    log->stores[log->count++] = (AnStore){address, size, value};
}

/* A read: plain RAM and clear of every store. */
AN bool an_range(const An* s, u32 address, u32 size) {
    return nr_ram(s->cpu, address, size) && nr_apart(address, size, s->lo0, s->hi0 - s->lo0) &&
           nr_apart(address, size, s->lo1, s->hi1 - s->lo1);
}

AN bool an_readable(An* s, u32 address, u32 size) {
    if (s->fast)
        return true;
    const bool ok = an_range(s, address, size);
    s->bad |= !ok;
    return ok;
}

/* A range the fast replay will read from: where it is not readable, the
 * fast replay stops and the general one runs. */
AN void an_valid(An* s, u32 address, u32 size) {
    if (s->fast && !an_range(s, address, size))
        s->bad = s->retry = true;
}

AN u32 an_lwz(An* s, u32 address) {
    return an_readable(s, address, 4u) ? read_be32(s->ram + (address - GC_RAM_BASE)) : 0u;
}
AN u32 an_lhz(An* s, u32 address) {
    return an_readable(s, address, 2u) ? read_be16(s->ram + (address - GC_RAM_BASE)) : 0u;
}
AN u32 an_lha(An* s, u32 address) { return (u32)(s32)(s16)an_lhz(s, address); }
AN f64 an_lfs(An* s, u32 address) { return nr_single_bits(an_lwz(s, address)); }
AN u64 an_lfd(An* s, u32 address) {
    return an_readable(s, address, 8u) ? read_be64(s->ram + (address - GC_RAM_BASE)) : 0u;
}

/* A single-precision result or lfs: both halves. */
AN void an_set(An* s, unsigned r, f64 value) {
    s->f[r] = value;
    s->p[r] = value;
}

AN void an_block(An* s, u32 cycles) { s->bad |= !nr_block(&s->k, cycles); }

/* CR0 from a compare with zero/an immediate (SO from XER), from fcmpo (its
 * code), and cror's EQ. */
AN void an_cmpw(An* s, s32 a, s32 b) { s->cr = nr_cr0_signed(s->cr, s->xer, a, b); }
AN void an_cmplw(An* s, u32 a, u32 b) { s->cr = nr_cr0_unsigned(s->cr, s->xer, a, b); }
AN u32 an_fcmpo(An* s, f64 a, f64 b) {
    const u32 code = nr_fcmp(&s->fp, a, b);
    s->cr = (s->cr & 0x0FFFFFFFu) | (code << 28);
    return code;
}
/* cror 2, x, 2: EQ |= LT (x = 0) or GT (x = 1); returns EQ. */
AN bool an_cror_eq(An* s, unsigned x) {
    const u32 bit = (s->cr >> (31u - x)) & 1u;
    s->cr |= bit << 29;
    return (s->cr & 0x20000000u) != 0u;
}

/* srawi rD, rS, 1; addze rE, rD: the signed half (toward zero), and CA. */
AN u32 an_half(An* s, u32 value, u32* shifted) {
    const u32 r0 = (u32)((s32)value >> 1);
    u32 ca = (value & 0x80000000u) && (value << 31) != 0u ? 1u : 0u;
    *shifted = r0;
    const u64 wide = (u64)r0 + ca;
    ca = (u32)(wide >> 32) & 1u;
    s->xer = (s->xer & ~0x20000000u) | (ca << 29);
    return (u32)wide;
}

/* The chunk boundaries a path crossed pass without the host (direct_calls.h,
 * the edge filter's own test). */
AN bool an_silent(const CPUState* cpu, const u32* addresses, unsigned count) {
    if (!bw_edge_filter_enabled || !bw_edge_watch_ready || !bw_host_quiet(cpu))
        return false;
    for (unsigned i = 0; i < count; ++i)
        if (!bw_edge_unwatched(addresses[i]))
            return false;
    return true;
}

/* --- JMAHermiteInterpolation (803012D8): f1 the frame, f2 t0, f3 v0, f4 d0,
 * f5 t1, f6 v1, f7 d1; one block of 46 cycles. -------------------------- */

/* In single precision: false (nothing written) where it does not apply. */
AN bool an_hermite_single(An* s) {
    const u32 r2 = s->g[2];
    f32 x1, x2, x3, x4, x5, x6, x7, k1, k2, k3, k4;
    if (!nr_plain(s->f[1], &x1) || !nr_plain(s->f[2], &x2) || !nr_plain(s->f[3], &x3) ||
        !nr_plain(s->f[4], &x4) || !nr_plain(s->f[5], &x5) || !nr_plain(s->f[6], &x6) ||
        !nr_plain(s->f[7], &x7))
        return false;
    if (!nr_plain_word(an_lwz(s, r2 - 13000u), &k1) || !nr_plain_word(an_lwz(s, r2 - 12996u), &k2) ||
        !nr_plain_word(an_lwz(s, r2 - 12992u), &k3) || !nr_plain_word(an_lwz(s, r2 - 12988u), &k4))
        return false;
    bool bad = false;
    const f32 f9 = nr_r(x1 - x2, &bad);
    const f32 f8 = k1;
    const f32 d = nr_r(x5 - x2, &bad);
    if (bad)
        return false;
    const f32 inv = nr_r(f8 / d, &bad);
    const f32 sq = nr_r(f9 * f9, &bad);
    const f32 f2a = nr_r(inv * sq, &bad);
    const f32 f10 = nr_r(f2a * inv, &bad);
    const f32 f11 = nr_r(f9 * f10, &bad);
    const f32 f12 = nr_r(f11 * inv, &bad);
    const f32 f5 = nr_r(x7 * nr_r(f11 - f2a, &bad), &bad);
    const f32 f2 = nr_r(x4 * nr_r(f9 + nr_r(f11 - nr_r(k2 * f2a, &bad), &bad), &bad), &bad);
    const f32 f4 = nr_r(k3 * f10, &bad);
    const f32 f1a = nr_r(x3 * nr_r(f8 + nr_r(nr_r(k2 * f12, &bad) - f4, &bad), &bad), &bad);
    const f32 f0a = nr_r(x6 * nr_r(nr_r(k4 * f12, &bad) + f4, &bad), &bad);
    const f32 f0 = nr_r(f2 + nr_r(f1a + f0a, &bad), &bad);
    const f32 f1 = nr_r(f5 + f0, &bad);
    if (bad)
        return false;
    an_set(s, 0, f0);
    an_set(s, 1, f1);
    an_set(s, 2, f2);
    an_set(s, 4, f4);
    an_set(s, 5, f5);
    an_set(s, 8, f8);
    an_set(s, 9, f9);
    an_set(s, 10, f10);
    an_set(s, 11, f11);
    an_set(s, 12, f12);
    s->fp.fpscr = (s->fp.fpscr & ~(NR_FPSCR_FI | NR_FPSCR_FR | (0x1Fu << 12))) | (nr_class_plain(f1) << 12);
    return true;
}

AN_CALLEE void an_hermite_double(An* s);

/* `fast`: the replay inline and only in single precision (anything else
 * makes it bad: the entry then runs the general replay instead), so the
 * machine stays in host registers; otherwise the double replay where the
 * single one does not apply. */
AN void an_hermite(An* s, bool fast) {
    an_block(s, 46u);
    s->suffix = 7u; /* lfs f0, -12988(r2) */
    if (!an_hermite_single(s)) {
        if (fast)
            s->bad = s->retry = true;
        else
            an_hermite_double(s);
    }
}

/* The double replay: every input the translation's inline paths take. */
AN_CALLEE void an_hermite_double(An* s) {
    const u32 r2 = s->g[2];
    NrFp* fp = &s->fp;
    f64* f = s->f;
    an_set(s, 9, nr_fsubs(fp, f[1], f[2]));          /* fsubs f9, f1, f2 */
    an_set(s, 8, an_lfs(s, r2 - 13000u));            /* lfs f8, -13000(r2) */
    an_set(s, 0, nr_fsubs(fp, f[5], f[2]));          /* fsubs f0, f5, f2 */
    an_set(s, 1, nr_fdivs(fp, f[8], f[0]));          /* fdivs f1, f8, f0 */
    an_set(s, 0, nr_fmuls(fp, f[9], f[9]));          /* fmuls f0, f9, f9 */
    an_set(s, 2, nr_fmuls(fp, f[1], f[0]));          /* fmuls f2, f1, f0 */
    an_set(s, 10, nr_fmuls(fp, f[2], f[1]));         /* fmuls f10, f2, f1 */
    an_set(s, 11, nr_fmuls(fp, f[9], f[10]));        /* fmuls f11, f9, f10 */
    an_set(s, 12, nr_fmuls(fp, f[11], f[1]));        /* fmuls f12, f11, f1 */
    an_set(s, 0, nr_fsubs(fp, f[11], f[2]));         /* fsubs f0, f11, f2 */
    an_set(s, 5, nr_fmuls(fp, f[7], f[0]));          /* fmuls f5, f7, f0 */
    an_set(s, 1, an_lfs(s, r2 - 12996u));            /* lfs f1, -12996(r2) */
    an_set(s, 0, nr_fmuls(fp, f[1], f[2]));          /* fmuls f0, f1, f2 */
    an_set(s, 0, nr_fsubs(fp, f[11], f[0]));         /* fsubs f0, f11, f0 */
    an_set(s, 0, nr_fadds(fp, f[9], f[0]));          /* fadds f0, f9, f0 */
    an_set(s, 2, nr_fmuls(fp, f[4], f[0]));          /* fmuls f2, f4, f0 */
    an_set(s, 0, an_lfs(s, r2 - 12992u));            /* lfs f0, -12992(r2) */
    an_set(s, 4, nr_fmuls(fp, f[0], f[10]));         /* fmuls f4, f0, f10 */
    an_set(s, 0, nr_fmuls(fp, f[1], f[12]));         /* fmuls f0, f1, f12 */
    an_set(s, 0, nr_fsubs(fp, f[0], f[4]));          /* fsubs f0, f0, f4 */
    an_set(s, 0, nr_fadds(fp, f[8], f[0]));          /* fadds f0, f8, f0 */
    an_set(s, 1, nr_fmuls(fp, f[3], f[0]));          /* fmuls f1, f3, f0 */
    an_set(s, 0, an_lfs(s, r2 - 12988u));            /* lfs f0, -12988(r2) */
    an_set(s, 0, nr_fmuls(fp, f[0], f[12]));         /* fmuls f0, f0, f12 */
    an_set(s, 0, nr_fadds(fp, f[0], f[4]));          /* fadds f0, f0, f4 */
    an_set(s, 0, nr_fmuls(fp, f[6], f[0]));          /* fmuls f0, f6, f0 */
    an_set(s, 0, nr_fadds(fp, f[1], f[0]));          /* fadds f0, f1, f0 */
    an_set(s, 0, nr_fadds(fp, f[2], f[0]));          /* fadds f0, f2, f0 */
    an_set(s, 1, nr_fadds(fp, f[5], f[0]));          /* fadds f1, f5, f0 */
}

/* --- J3DGetKeyFrameInterpolation<f32> (802F2DAC): r3 the table, r4 the
 * keys, f1 the frame; LR the return address. ------------------------------ */

#define AN_HERMITE_RETURN_0 0x802F2E54u
#define AN_HERMITE_RETURN_1 0x802F2EE8u

AN void an_key_f(An* s, bool fast) {
    if (fast) {
        /* The table, its keys - one more than it has: a search over keys out
         * of order can end on the last - and the spline's constants. */
        an_valid(s, s->g[3], 6u);
        if (!an_ok(s))
            return;
        const u32 count = an_lhz(s, s->g[3]), stride = an_lhz(s, s->g[3] + 4u) != 0u ? 4u : 3u;
        if (count == 0u) {
            s->bad = s->retry = true;
            return;
        }
        an_valid(s, s->g[4], (count + 1u) * stride * 4u);
        an_valid(s, s->g[2] - 13000u, 16u);
        if (!an_ok(s))
            return;
    }
    const u32 sp = s->g[1], frame = sp - 16u;
    /* 802F2DAC (6): stwu r1, -16(r1); mflr r0; stw r0, 20(r1); lhz r0, 4(r3);
     * cmplwi r0, 0; bne 802F2E58 */
    an_block(s, 6u);
    an_log(s, frame, 4u, sp);
    s->g[1] = frame;
    const u32 saved_lr = s->lr;
    s->g[0] = saved_lr;
    s->suffix = 4u;
    an_log(s, sp + 4u, 4u, saved_lr);
    const u32 type = an_lhz(s, s->g[3] + 4u);
    s->g[0] = type;
    an_cmplw(s, type, 0u);
    const bool one = type != 0u; /* separate in and out tangents: four floats a key */
    /* 802F2DC4 / 802F2E58 (3): lfs f0, 0(r4); fcmpo cr0, f1, f0; bge past */
    an_block(s, 3u);
    an_set(s, 0, an_lfs(s, s->g[4]));
    s->suffix = 2u;
    if (an_fcmpo(s, s->f[1], s->f[0]) & 0x8u) {
        /* 802F2DD0 / 802F2E64 (2): lfs f1, 4(r4); b the epilogue */
        an_block(s, 2u);
        an_set(s, 1, an_lfs(s, s->g[4] + 4u));
        s->suffix = 1u;
        goto epilogue;
    }
    {
        /* 802F2DD8 (9) / 802F2E6C (7): lhz r3, 0(r3); addi r0, r3, -1;
         * mulli r0, r0, 12 / rlwinm r0, r0, 4, 0, 27; lfsx f0, r4, r0;
         * fcmpo cr0, f0, f1; cror 2, 0, 2; bne the search */
        an_block(s, one ? 7u : 9u);
        s->g[3] = an_lhz(s, s->g[3]);
        const u32 last = s->g[3] - 1u;
        s->g[0] = one ? last << 4 : (u32)((s64)(s32)last * 12);
        an_set(s, 0, an_lfs(s, s->g[4] + s->g[0]));
        s->suffix = 3u;
        an_fcmpo(s, s->f[0], s->f[1]);
        if (an_cror_eq(s, 0u)) {
            /* 802F2DF4 / 802F2E88 (3): add r3, r4, r0; lfs f1, 4(r3); b the epilogue */
            an_block(s, 3u);
            s->g[3] = s->g[4] + s->g[0];
            an_set(s, 1, an_lfs(s, s->g[3] + 4u));
            s->suffix = 1u;
            goto epilogue;
        }
    }
    /* The search: 802F2E30 / 802F2EC4 (2): cmpwi r3, 1; bgt the body. */
    for (;;) {
        an_block(s, 2u);
        an_cmpw(s, (s32)s->g[3], 1);
        if (!((s32)s->g[3] > 1) || !an_ok(s))
            break;
        /* 802F2E04 (9) / 802F2E98 (7): srawi r0, r3, 1; addze r5, r0;
         * mulli r0, r5, 12 / rlwinm r0, r5, 4, 0, 27; lfsx f0, r4, r0;
         * fcmpo cr0, f1, f0; cror 2, 1, 2; bne 802F2E2C / 802F2EC0 */
        an_block(s, one ? 7u : 9u);
        u32 shifted;
        s->g[5] = an_half(s, s->g[3], &shifted);
        s->g[0] = one ? s->g[5] << 4 : (u32)((s64)(s32)s->g[5] * 12);
        an_set(s, 0, an_lfs(s, s->g[4] + s->g[0]));
        s->suffix = 3u;
        an_fcmpo(s, s->f[1], s->f[0]);
        if (an_cror_eq(s, 1u)) {
            /* 802F2E20 / 802F2EB4 (3): add r4, r4, r0; subf r3, r5, r3 */
            an_block(s, 3u);
            s->g[4] += s->g[0];
            s->g[3] -= s->g[5];
        } else {
            /* 802F2E2C / 802F2EC0 (1): or r3, r5, r5 */
            an_block(s, 1u);
            s->g[3] = s->g[5];
        }
    }
    /* 802F2E38 / 802F2ECC (7): lfs f2..f7 from the key; bl JMAHermiteInterpolation */
    an_block(s, 7u);
    {
        const u32 key = s->g[4];
        an_set(s, 2, an_lfs(s, key));
        an_set(s, 3, an_lfs(s, key + 4u));
        an_set(s, 4, an_lfs(s, key + (one ? 12u : 8u)));
        an_set(s, 5, an_lfs(s, key + (one ? 16u : 12u)));
        an_set(s, 6, an_lfs(s, key + (one ? 20u : 16u)));
        an_set(s, 7, an_lfs(s, key + (one ? 24u : 20u)));
    }
    s->suffix = 1u;
    s->lr = one ? AN_HERMITE_RETURN_1 : AN_HERMITE_RETURN_0;
    s->crossed = true;
    an_hermite(s, fast);
    if (!one)
        an_block(s, 1u); /* 802F2E54 (1): b the epilogue */
epilogue:
    /* 802F2EE8 (5): lwz r0, 20(r1); mtlr r0; addi r1, r1, 16; blr */
    an_block(s, 5u);
    s->g[0] = saved_lr;
    s->lr = saved_lr;
    s->suffix = 2u;
    s->g[1] = sp;
}

/* --- J3DHermiteInterpolationS (802F06D8): r3..r8 the key's shorts (time0,
 * value0, tangent0, time1, value1, tangent1) through GQR5; one block of 37
 * cycles. --------------------------------------------------------------- */

/* psq_l fD, 0(rA), 1, 5 with GQR5 loading signed shorts: the value scaled
 * by 2^-scale, rounded once to single (exact here); 1.0 in the second half. */
AN void an_psq_short(An* s, unsigned r, u32 address, f64 scale) {
    const f64 value = (f64)(f32)((f64)(s16)an_lhz(s, address) * scale);
    s->f[r] = value;
    s->p[r] = 1.0;
}

/* In single precision: false (nothing written) where it does not apply. */
AN bool an_hermite_s_single(An* s, f64 scale) {
    f32 x1;
    if (!nr_plain(s->f[1], &x1))
        return false;
    const f32 sc = (f32)scale;
    const f32 t0 = (f32)(s16)an_lhz(s, s->g[3]) * sc, v0 = (f32)(s16)an_lhz(s, s->g[4]) * sc;
    const f32 d0 = (f32)(s16)an_lhz(s, s->g[5]) * sc, t1 = (f32)(s16)an_lhz(s, s->g[6]) * sc;
    const f32 v1 = (f32)(s16)an_lhz(s, s->g[7]) * sc, d1 = (f32)(s16)an_lhz(s, s->g[8]) * sc;
    bool bad = false;
    nr_r(t0, &bad), nr_r(v0, &bad), nr_r(d0, &bad), nr_r(t1, &bad), nr_r(v1, &bad), nr_r(d1, &bad);
    const f32 f4 = nr_r(t1 - t0, &bad);
    const f32 f6a = nr_r(x1 - t0, &bad);
    const f32 f5a = nr_r(v1 - v0, &bad);
    if (bad)
        return false;
    const f32 f6 = nr_r(f6a / f4, &bad);
    const f32 f1a = nr_r(fmaf(d1, f4, v0), &bad);
    const f32 f7 = nr_r(f6 * f6, &bad);
    const f32 f5 = nr_r(-fmaf(f4, d0, -f5a), &bad);
    const f32 f1b = nr_r(nr_r(f1a - v1, &bad) - f5, &bad);
    const f32 f3 = nr_r(f7 * f1b, &bad);
    const f32 f1c = nr_r(fmaf(nr_r(fmaf(f4, d0, f3), &bad), f6, v0), &bad); /* fmadds f1, f4, f0, f3; f1, f6, f2 */
    const f32 f1d = nr_r(fmaf(f5, f7, f1c), &bad);                         /* fmadds f1, f5, f7, f1 */
    const f32 f1 = nr_r(f1d - f3, &bad);                                    /* fsubs f1, f1, f3 */
    if (bad)
        return false;
    s->f[0] = d0;
    s->p[0] = 1.0;
    s->f[2] = v0;
    s->p[2] = 1.0;
    an_set(s, 1, f1);
    an_set(s, 3, f3);
    an_set(s, 4, f4);
    an_set(s, 5, f5);
    an_set(s, 6, f6);
    an_set(s, 7, f7);
    s->fp.fpscr = (s->fp.fpscr & ~(NR_FPSCR_FI | NR_FPSCR_FR | (0x1Fu << 12))) | (nr_class_plain(f1) << 12);
    return true;
}

AN_CALLEE void an_hermite_s_double(An* s, f64 scale);

AN void an_hermite_s(An* s, f64 scale, bool fast) {
    an_block(s, 37u);
    s->suffix = 11u; /* psq_l f0, 0(r5), 1, 5 */
    if (!an_hermite_s_single(s, scale)) {
        if (fast)
            s->bad = s->retry = true;
        else
            an_hermite_s_double(s, scale);
    }
}

/* The double replay: every input the translation's inline paths take. */
AN_CALLEE void an_hermite_s_double(An* s, f64 scale) {
    NrFp* fp = &s->fp;
    f64* f = s->f;
    an_psq_short(s, 0, s->g[3], scale);                    /* psq_l f0, 0(r3), 1, 5 */
    an_psq_short(s, 3, s->g[6], scale);                    /* psq_l f3, 0(r6), 1, 5 */
    an_psq_short(s, 2, s->g[4], scale);                    /* psq_l f2, 0(r4), 1, 5 */
    an_set(s, 4, nr_fsubs(fp, f[3], f[0]));                /* fsubs f4, f3, f0 */
    an_psq_short(s, 3, s->g[7], scale);                    /* psq_l f3, 0(r7), 1, 5 */
    an_set(s, 6, nr_fsubs(fp, f[1], f[0]));                /* fsubs f6, f1, f0 */
    an_psq_short(s, 1, s->g[8], scale);                    /* psq_l f1, 0(r8), 1, 5 */
    an_set(s, 5, nr_fsubs(fp, f[3], f[2]));                /* fsubs f5, f3, f2 */
    an_set(s, 6, nr_fdivs(fp, f[6], f[4]));                /* fdivs f6, f6, f4 */
    an_psq_short(s, 0, s->g[5], scale);                    /* psq_l f0, 0(r5), 1, 5 */
    an_set(s, 1, nr_fmadds(fp, f[1], f[4], f[2], false, false)); /* fmadds f1, f1, f4, f2 */
    an_set(s, 7, nr_fmuls(fp, f[6], f[6]));                /* fmuls f7, f6, f6 */
    an_set(s, 5, nr_fmadds(fp, f[4], f[0], f[5], true, true)); /* fnmsubs f5, f4, f0, f5 */
    an_set(s, 1, nr_fsubs(fp, f[1], f[3]));                /* fsubs f1, f1, f3 */
    an_set(s, 1, nr_fsubs(fp, f[1], f[5]));                /* fsubs f1, f1, f5 */
    an_set(s, 3, nr_fmuls(fp, f[7], f[1]));                /* fmuls f3, f7, f1 */
    an_set(s, 1, nr_fmadds(fp, f[4], f[0], f[3], false, false)); /* fmadds f1, f4, f0, f3 */
    an_set(s, 1, nr_fmadds(fp, f[1], f[6], f[2], false, false)); /* fmadds f1, f1, f6, f2 */
    an_set(s, 1, nr_fmadds(fp, f[5], f[7], f[1], false, false)); /* fmadds f1, f5, f7, f1 */
    an_set(s, 1, nr_fsubs(fp, f[1], f[3]));                /* fsubs f1, f1, f3 */
}

/* --- J3DGetKeyFrameInterpolationS (802F072C): r3 the table, r4 the keys
 * (shorts), f1 the frame; LR the return address. ------------------------- */

#define AN_HERMITE_S_RETURN_0 0x802F0844u
#define AN_HERMITE_S_RETURN_1 0x802F0944u

/* lha r0; xoris r0, r0, 0x8000; stw r0, 12(r1); [lis r4, 17200;] stw r4,
 * 8(r1); lfd f0, 8(r1); fsubs fD, f0, f2: a short as the single it is,
 * exactly; its FPRF. */
AN f64 an_short_value(An* s, u32 value, u32* word) {
    *word = value ^ 0x80000000u;
    const s32 v = (s32)value;
    nr_fprf(&s->fp, v == 0 ? 0x02u : v > 0 ? 0x04u : 0x08u);
    return (f64)v;
}

AN void an_key_s(An* s, f64 scale, bool fast) {
    if (fast) {
        /* The table, its keys (one more, as above) and the magic double. */
        an_valid(s, s->g[3], 6u);
        if (!an_ok(s))
            return;
        const u32 count = an_lhz(s, s->g[3]), stride = an_lhz(s, s->g[3] + 4u) != 0u ? 4u : 3u;
        if (count == 0u) {
            s->bad = s->retry = true;
            return;
        }
        an_valid(s, s->g[4], (count + 1u) * stride * 2u);
        an_valid(s, s->g[2] - 13160u, 8u);
        if (!an_ok(s))
            return;
    }
    const u32 sp = s->g[1], frame = sp - 16u;
    u32 word = 0u;
    bool converted = false;
    /* 802F072C (7): stwu r1, -16(r1); mflr r0; stw r0, 20(r1); or r8, r4, r4;
     * lhz r0, 4(r3); cmplwi r0, 0; bne 802F0848 */
    an_block(s, 7u);
    an_log(s, frame, 4u, sp);
    s->g[1] = frame;
    const u32 saved_lr = s->lr;
    s->g[0] = saved_lr;
    s->suffix = 5u;
    an_log(s, sp + 4u, 4u, saved_lr);
    s->g[8] = s->g[4];
    const u32 type = an_lhz(s, s->g[3] + 4u);
    s->g[0] = type;
    an_cmplw(s, type, 0u);
    const bool one = type != 0u; /* four shorts a key */
    {
        /* 802F0748 / 802F0848 (10): lha r0, 0(r8); lfd f2, -13160(r2); the
         * conversion; fcmpo cr0, f1, f0; bge past */
        an_block(s, 10u);
        s->g[0] = an_lha(s, s->g[8]);
        const u64 magic = an_lfd(s, s->g[2] - 13160u);
        s->bad |= magic != AN_MAGIC;
        s->f[2] = f64_value(magic);
        s->g[4] = 0x43300000u;
        an_set(s, 0, an_short_value(s, s->g[0], &word));
        s->g[0] = word;
        converted = true;
        s->suffix = 3u;
        if (an_fcmpo(s, s->f[1], s->f[0]) & 0x8u) {
            /* 802F0770 / 802F0870 (7): lha r0, 2(r8); the conversion into f1; b the epilogue */
            an_block(s, 7u);
            s->g[0] = an_lha(s, s->g[8] + 2u);
            s->f[0] = f64_value(((u64)0x43300000u << 32) | (s->g[0] ^ 0x80000000u));
            an_set(s, 1, an_short_value(s, s->g[0], &word));
            s->g[0] = word;
            s->suffix = 2u;
            goto epilogue;
        }
    }
    {
        /* 802F078C (14) / 802F088C (12): lhz r5, 0(r3); addi r0, r5, -1;
         * mulli r3, r0, 6 / rlwinm r3, r0, 3, 0, 28; lhax r0, r8, r3; the
         * conversion; fcmpo cr0, f0, f1; cror 2, 0, 2; bne the search */
        an_block(s, one ? 12u : 14u);
        s->g[5] = an_lhz(s, s->g[3]);
        const u32 last = s->g[5] - 1u;
        s->g[3] = one ? last << 3 : (u32)((s64)(s32)last * 6);
        s->g[0] = an_lha(s, s->g[8] + s->g[3]);
        an_set(s, 0, an_short_value(s, s->g[0], &word));
        s->g[0] = word;
        s->suffix = 4u;
        an_fcmpo(s, s->f[0], s->f[1]);
        if (an_cror_eq(s, 0u)) {
            /* 802F07BC / 802F08BC (8): add r3, r8, r3; lha r0, 2(r3); the
             * conversion into f1; b the epilogue */
            an_block(s, 8u);
            s->g[3] = s->g[8] + s->g[3];
            s->g[0] = an_lha(s, s->g[3] + 2u);
            s->f[0] = f64_value(((u64)0x43300000u << 32) | (s->g[0] ^ 0x80000000u));
            an_set(s, 1, an_short_value(s, s->g[0], &word));
            s->g[0] = word;
            s->suffix = 2u;
            goto epilogue;
        }
    }
    /* The search: 802F0820 / 802F0920 (2): cmpwi r5, 1; bgt the body. */
    for (;;) {
        an_block(s, 2u);
        an_cmpw(s, (s32)s->g[5], 1);
        if (!((s32)s->g[5] > 1) || !an_ok(s))
            break;
        /* 802F07E0 (14) / 802F08E0 (12): srawi r0, r5, 1; addze r6, r0;
         * mulli r3, r6, 6 / rlwinm r3, r6, 3, 0, 28; lhax r0, r8, r3; the
         * conversion; fcmpo cr0, f1, f0; cror 2, 1, 2; bne 802F081C / 802F091C */
        an_block(s, one ? 12u : 14u);
        u32 shifted;
        s->g[6] = an_half(s, s->g[5], &shifted);
        s->g[3] = one ? s->g[6] << 3 : (u32)((s64)(s32)s->g[6] * 6);
        s->g[0] = an_lha(s, s->g[8] + s->g[3]);
        an_set(s, 0, an_short_value(s, s->g[0], &word));
        s->g[0] = word;
        s->suffix = 4u;
        an_fcmpo(s, s->f[1], s->f[0]);
        if (an_cror_eq(s, 1u)) {
            /* 802F0810 / 802F0910 (3): add r8, r8, r3; subf r5, r6, r5 */
            an_block(s, 3u);
            s->g[8] += s->g[3];
            s->g[5] -= s->g[6];
        } else {
            /* 802F081C / 802F091C (1): or r5, r6, r6 */
            an_block(s, 1u);
            s->g[5] = s->g[6];
        }
    }
    /* 802F0828 / 802F0928 (7): r3..r8 the key's shorts; bl J3DHermiteInterpolationS */
    an_block(s, 7u);
    {
        const u32 key = s->g[8];
        s->g[3] = key;
        s->g[4] = key + 2u;
        s->g[5] = key + (one ? 6u : 4u);
        s->g[6] = key + (one ? 8u : 6u);
        s->g[7] = key + (one ? 10u : 8u);
        s->g[8] = key + (one ? 12u : 10u);
    }
    s->lr = one ? AN_HERMITE_S_RETURN_1 : AN_HERMITE_S_RETURN_0;
    an_hermite_s(s, scale, fast);
    if (!one)
        an_block(s, 1u); /* 802F0844 (1): b the epilogue */
epilogue:
    /* 802F0944 (5): lwz r0, 20(r1); mtlr r0; addi r1, r1, 16; blr */
    an_block(s, 5u);
    if (converted) {
        an_log(s, frame + 12u, 4u, word);
        an_log(s, frame + 8u, 4u, 0x43300000u);
    }
    s->g[0] = saved_lr;
    s->lr = saved_lr;
    s->suffix = 2u;
    s->g[1] = sp;
}

/* GQR5 loading signed shorts (type 7), and the scale it applies; false
 * otherwise (the translation's psq_l then reads something else). */
AN bool an_gqr5(const CPUState* cpu, f64* scale) {
    const u32 gqr = cpu->gqr[5];
    if (((gqr >> 16) & 7u) != 7u || (cpu->hid2 & PPC_HID2_LSQE) == 0u)
        return false;
    const s32 shift = sign_extend((gqr >> 24) & 0x3Fu, 6);
    *scale = f64_value((u64)(1023 - shift) << 52); /* 2^-shift, exactly: ldexp(1.0, -shift) */
    return true;
}

/* --- J3DAnmTransformKey::calcTransform (802F0954): r3 this, r4 the joint,
 * r5 the J3DTransformInfo, f1 the frame. -------------------------------- */

#define AN_KEY_F_CALL 0x802F2DACu

/* One channel: `c` 0..8 (scale, rotation, translation; x, y, z). */
typedef struct AnChannel {
    u32 table_offset;  /* the J3DAnmKeyTableBase's offset in the joint's 54 bytes */
    u32 data;          /* the key array's field in this: 16 scale, 20 rotation, 24 translation */
    u32 out;           /* the field in the transform */
    u32 many;          /* the search path's call block */
    u32 return_address;
} AnChannel;

static const AnChannel AN_CHANNELS[9] = {
    {0u, 16u, 0u, 7u, 0x802F09E8u},   {18u, 16u, 4u, 15u, 0x802F0A60u}, {36u, 16u, 8u, 15u, 0x802F0AD8u},
    {6u, 20u, 12u, 9u, 0x802F0B50u},  {24u, 20u, 14u, 16u, 0x802F0BE8u}, {42u, 20u, 16u, 16u, 0x802F0C80u},
    {12u, 24u, 20u, 9u, 0x802F0D04u}, {30u, 24u, 24u, 16u, 0x802F0D80u}, {48u, 24u, 28u, 16u, 0x802F0DFCu},
};

/* The callees' replays out of line, for the transform: each channel's call
 * shares one copy. */
AN_CALLEE void an_key_f_fast(An* s) { an_key_f(s, true); }
AN_CALLEE void an_key_s_fast(An* s, f64 scale) { an_key_s(s, scale, true); }
AN_CALLEE void an_key_f_general(An* s) { an_key_f(s, false); }
AN_CALLEE void an_key_s_general(An* s, f64 scale) { an_key_s(s, scale, false); }

AN void an_transform(An* s, f64 scale, bool fast) {
    const u32 sp = s->g[1], frame = sp - 48u;
    const u32 self = s->g[3], dst = s->g[5];
    /* 802F0954 (7): stwu r1, -48(r1); mflr r0; stw r0, 52(r1); stfd f31,
     * 32(r1); psq_st f31, 40(r1), 0, 0; addi r11, r1, 32; bl _savegpr_28 */
    an_block(s, 7u);
    an_log(s, frame, 4u, sp);
    s->g[1] = frame;
    const u32 saved_lr = s->lr;
    s->g[0] = saved_lr;
    an_log(s, sp + 4u, 4u, saved_lr);
    const u64 f31_bits = f64_bits(s->f[31]);
    const f64 f31_half = s->p[31];
    an_log(s, frame + 32u, 8u, f31_bits);
    an_log(s, frame + 40u, 4u, nr_to_single_ftz(f31_bits));
    an_log(s, frame + 44u, 4u, nr_to_single_ftz(f64_bits(f31_half)));
    s->g[11] = frame + 32u;
    s->bad |= !nr_inline_gpr(&s->k);
    const u32 saved_gprs[4] = {s->g[28], s->g[29], s->g[30], s->g[31]};
    for (unsigned i = 0; i < 4u; ++i)
        an_log(s, frame + 16u + 4u * i, 4u, saved_gprs[i]);
    s->lr = 0x802F0970u;
    /* The object's fields and the two constants (the fast replay's reads). */
    an_valid(s, self + 16u, 28u);
    an_valid(s, s->g[2] - 13176u, 8u);
    /* 802F0970 (12): or r28, r3, r3; fmr f31, f1; or r29, r5, r5; lwz r6,
     * 40(r3); rlwinm r30, r4, 0, 16, 31; mulli r31, r30, 54; add r3, r6, r31;
     * lhz r0, 0(r3); cmpwi r0, 1; beq */
    an_block(s, 12u);
    s->g[28] = self;
    s->f[31] = s->f[1];
    s->g[29] = dst;
    s->g[30] = s->g[4] & 0xFFFFu;
    s->g[31] = s->g[30] * 54u;
    _Pragma("clang loop unroll(disable)")
    for (unsigned c = 0; c < 9u && an_ok(s); ++c) {
        const AnChannel* ch = &AN_CHANNELS[c];
        const bool rotation = c >= 3u && c < 6u;
        /* The check: 802F0970's tail, or lwz r6, 40(r28); add r4, r6, r31;
         * lhz r0, OFF(r4); cmpwi r0, 1; beq (5). */
        u32 entry;
        if (c == 0u) {
            s->g[6] = an_lwz(s, self + 40u);
            s->g[3] = s->g[6] + s->g[31];
            entry = s->g[3];
            an_valid(s, entry, 54u); /* the joint's nine tables */
            if (!an_ok(s))
                break;
            if (fast) {
                /* A joint none of whose channels searches is the second set's
                 * (native_game_math.c's transform_simple, whose hook follows
                 * this one's): faster there than here. Declined. */
                u32 searched = 0u;
                for (unsigned i = 0; i < 9u; ++i)
                    searched |= an_lhz(s, entry + AN_CHANNELS[i].table_offset) > 1u;
                if (!searched) {
                    s->bad = true;
                    break;
                }
            }
        } else {
            an_block(s, 5u);
            s->g[6] = an_lwz(s, self + 40u);
            s->g[4] = s->g[6] + s->g[31];
            entry = s->g[4] + ch->table_offset;
        }
        const u32 keys = an_lhz(s, entry);
        s->g[0] = keys;
        an_cmpw(s, (s32)keys, 1);
        if (keys == 1u) {
            /* One key: lwz rX, DATA(r28); lhz r0, OFF+2; rlwinm (x4, or x2);
             * lfsx f0 / lhax r3 (with lwz r0, 36(r28); slw r0, r3, r0); stfs / sth */
            an_block(s, rotation ? 8u : 6u);
            const u32 base = an_lwz(s, self + ch->data);
            const u32 offset = an_lhz(s, entry + 2u);
            if (rotation) {
                s->g[3] = base;
                s->g[0] = offset << 1;
                an_valid(s, base + s->g[0], 2u);
                s->g[3] = an_lha(s, base + s->g[0]);
                s->g[0] = an_lwz(s, self + 36u);
                const u32 sh = s->g[0] & 0x3Fu;
                s->g[0] = sh > 31u ? 0u : s->g[3] << sh;
                an_log(s, dst + ch->out, 2u, (u16)s->g[0]);
            } else {
                if (c == 0u)
                    s->g[4] = base;
                else
                    s->g[3] = base;
                s->g[0] = offset << 2;
                an_valid(s, base + s->g[0], 4u);
                an_set(s, 0, an_lfs(s, base + s->g[0]));
                s->suffix = 1u;
                an_log(s, dst + ch->out, 4u, nr_to_single(f64_bits(s->f[0])));
            }
            continue;
        }
        /* bge the search path (1); cmpwi r0, 0; bge zero (2) */
        an_block(s, 1u);
        if (keys == 0u) {
            an_block(s, 2u);
            an_cmpw(s, 0, 0);
            /* Zero keys (3): lfs f0, 1.0 or 0.0; stfs / li r0, 0; sth */
            an_block(s, 3u);
            if (rotation) {
                s->g[0] = 0u;
                an_log(s, dst + ch->out, 2u, 0u);
            } else {
                an_set(s, 0, an_lfs(s, s->g[2] - (c < 3u ? 13176u : 13172u)));
                s->suffix = 1u;
                an_log(s, dst + ch->out, 4u, nr_to_single(f64_bits(s->f[0])));
            }
            continue;
        }
        /* The search path: fmr f1, f31; r3 the table, r5 the keys' array, r4
         * the channel's first key; bl the interpolation. */
        an_block(s, ch->many);
        s->f[1] = s->f[31];
        s->g[3] = s->g[6] + s->g[31] + ch->table_offset;
        s->g[5] = an_lwz(s, self + ch->data);
        s->g[4] = s->g[6] + s->g[31];
        s->g[0] = an_lhz(s, s->g[4] + ch->table_offset + 2u) << (rotation ? 1u : 2u);
        s->g[4] = s->g[5] + s->g[0];
        s->lr = ch->return_address;
        if (rotation) {
            if (fast)
                an_key_s_fast(s, scale);
            else
                an_key_s_general(s, scale);
            /* fctiwz f0, f1; stfd f0, 8(r1); lwz r3, 12(r1); lwz r0, 36(r28);
             * slw r0, r3, r0; sth r0 (6) */
            an_block(s, 6u);
            u64 bits;
            if (nr_fctiwz(&s->fp, s->f[1], &bits))
                s->f[0] = f64_value(bits);
            const u64 stored = f64_bits(s->f[0]);
            an_log(s, frame + 8u, 8u, stored);
            s->g[3] = (u32)stored;
            s->g[0] = an_lwz(s, self + 36u);
            const u32 sh = s->g[0] & 0x3Fu;
            s->g[0] = sh > 31u ? 0u : s->g[3] << sh;
            an_log(s, dst + ch->out, 2u, (u16)s->g[0]);
            s->suffix = 0u;
        } else {
            s->crossed = true;
            if (fast)
                an_key_f_fast(s);
            else
                an_key_f_general(s);
            /* stfs f1 (1) */
            an_block(s, 1u);
            an_log(s, dst + ch->out, 4u, nr_to_single(f64_bits(s->f[1])));
            s->suffix = 0u;
        }
    }
    /* 802F0E00 (4): psq_l f31, 40(r1), 0, 0; lfd f31, 32(r1); addi r11, r1,
     * 32; bl _restgpr_28; 802F0E10 (5): lwz r0, 52(r1); mtlr r0; addi r1,
     * r1, 48; blr */
    an_block(s, 4u);
    s->f[31] = f64_value(f31_bits);
    s->p[31] = nr_pair_round_trip(f31_half);
    s->g[11] = frame + 32u;
    s->bad |= !nr_inline_gpr(&s->k);
    s->g[28] = saved_gprs[0];
    s->g[29] = saved_gprs[1];
    s->g[30] = saved_gprs[2];
    s->g[31] = saved_gprs[3];
    an_block(s, 5u);
    s->g[0] = saved_lr;
    s->lr = saved_lr;
    s->suffix = 2u;
    s->g[1] = sp;
}

/* --- J3DPSCalcInverseTranspose (802DA584): r3 the 3x4 matrix, r4 the 3x3
 * result; r3 1, or 0 for a zero determinant (then nothing stored). One block
 * of 27 cycles, then 2 or 21. --------------------------------------------- */

typedef struct AnInverse {
    NrPair f[14];
    u32 r3, cr0, suffix, cycles;
    bool stores;
} AnInverse;

/* The general replay: inline_fp.h's paired-single paths on doubles. */
AN void an_inverse_general(NrFp* fp, const u32 m[9], AnInverse* o) {
    NrPair* f = o->f;
    f[0] = (NrPair){nr_single_bits(m[0]), 1.0};                   /* psq_l f0, 0(r3), 1, 0 */
    f[1] = (NrPair){nr_single_bits(m[1]), nr_single_bits(m[2])};  /* psq_l f1, 4(r3), 0, 0 */
    f[2] = (NrPair){nr_single_bits(m[3]), 1.0};                   /* psq_l f2, 16(r3), 1, 0 */
    f[6] = (NrPair){f[1].b, f[0].a};                              /* ps_merge10 f6, f1, f0 */
    f[3] = (NrPair){nr_single_bits(m[4]), nr_single_bits(m[5])};  /* psq_l f3, 20(r3), 0, 0 */
    f[4] = (NrPair){nr_single_bits(m[6]), 1.0};                   /* psq_l f4, 32(r3), 1, 0 */
    f[7] = (NrPair){f[3].b, f[2].a};                              /* ps_merge10 f7, f3, f2 */
    f[5] = (NrPair){nr_single_bits(m[7]), nr_single_bits(m[8])};  /* psq_l f5, 36(r3), 0, 0 */
    f[11] = nr_ps_mul(fp, f[3], f[6]);                            /* ps_mul f11, f3, f6 */
    f[8] = (NrPair){f[5].b, f[4].a};                              /* ps_merge10 f8, f5, f4 */
    f[13] = nr_ps_mul(fp, f[5], f[7]);                            /* ps_mul f13, f5, f7 */
    f[11] = nr_ps_madd(fp, f[1], f[7], f[11], true, false);       /* ps_msub f11, f1, f7, f11 */
    f[12] = nr_ps_mul(fp, f[1], f[8]);                            /* ps_mul f12, f1, f8 */
    f[13] = nr_ps_madd(fp, f[3], f[8], f[13], true, false);       /* ps_msub f13, f3, f8, f13 */
    f[12] = nr_ps_madd(fp, f[5], f[6], f[12], true, false);       /* ps_msub f12, f5, f6, f12 */
    f[10] = nr_ps_mul(fp, f[3], f[4]);                            /* ps_mul f10, f3, f4 */
    f[9] = nr_ps_mul(fp, f[0], f[5]);                             /* ps_mul f9, f0, f5 */
    f[8] = nr_ps_mul(fp, f[1], f[2]);                             /* ps_mul f8, f1, f2 */
    f[10] = nr_ps_madd(fp, f[2], f[5], f[10], true, false);       /* ps_msub f10, f2, f5, f10 */
    f[9] = nr_ps_madd(fp, f[1], f[4], f[9], true, false);         /* ps_msub f9, f1, f4, f9 */
    f[8] = nr_ps_madd(fp, f[0], f[3], f[8], true, false);         /* ps_msub f8, f0, f3, f8 */
    f[7] = nr_ps_mul(fp, f[0], f[13]);                            /* ps_mul f7, f0, f13 */
    f[1] = nr_ps_sub(fp, f[1], f[1]);                             /* ps_sub f1, f1, f1 */
    f[7] = nr_ps_madd(fp, f[2], f[12], f[7], false, false);       /* ps_madd f7, f2, f12, f7 */
    f[7] = nr_ps_madd(fp, f[4], f[11], f[7], false, false);       /* ps_madd f7, f4, f11, f7 */
    o->cr0 = nr_fcmp(fp, f[7].a, f[1].a);                         /* ps_cmpo0 cr0, f7, f1 */
    o->suffix = 19u;                                              /* psq_l f5, 36(r3) */
    if (o->cr0 & 0x2u) {
        o->r3 = 0u;                                               /* li r3, 0; blr (2) */
        o->cycles = 2u;
        o->stores = false;
        return;
    }
    f[0] = nr_ps_res(fp, f[7]);                                   /* ps_res f0, f7 */
    f[6] = nr_ps_add(fp, f[0], f[0]);                             /* ps_add f6, f0, f0 */
    f[5] = nr_ps_mul(fp, f[0], f[0]);                             /* ps_mul f5, f0, f0 */
    f[0] = nr_ps_madd(fp, f[7], f[5], f[6], true, true);          /* ps_nmsub f0, f7, f5, f6 */
    f[6] = nr_ps_add(fp, f[0], f[0]);
    f[5] = nr_ps_mul(fp, f[0], f[0]);
    f[0] = nr_ps_madd(fp, f[7], f[5], f[6], true, true);
    for (unsigned r = 13u; r >= 8u; --r)                          /* ps_muls0 f13 .. f8, f0 */
        f[r] = nr_ps_muls0(fp, f[r], f[0].a);
    o->r3 = 1u;                                                   /* li r3, 1 */
    o->cycles = 21u;
    o->stores = true;
    o->suffix = 1u;                                               /* psq_st f8, 32(r4), 1, 0 */
}

/* The same in single precision, where every value is a zero or a normal
 * single of at least 2^-125 (as the splines'); false where it does not
 * apply. A lane's 1.0 multiplies exactly; ps_msub's product before is
 * rounded, then the fused multiply-subtract. */
AN bool an_inverse_single(NrFp* fp, const u32 m[9], AnInverse* o) {
    f32 a[9];
    for (unsigned i = 0; i < 9u; ++i)
        if (!nr_plain_word(m[i], &a[i]))
            return false;
    bool bad = false;
    const f32 m00 = a[0], m01 = a[1], m02 = a[2], m10 = a[3], m11 = a[4], m12 = a[5], m20 = a[6], m21 = a[7],
              m22 = a[8];
    const f32 f11a = nr_r(fmaf(m01, m12, -nr_r(m11 * m02, &bad)), &bad);
    const f32 f11b = nr_r(fmaf(m02, m10, -nr_r(m12 * m00, &bad)), &bad);
    const f32 f13a = nr_r(fmaf(m11, m22, -nr_r(m21 * m12, &bad)), &bad);
    const f32 f13b = nr_r(fmaf(m12, m20, -nr_r(m22 * m10, &bad)), &bad);
    const f32 f12a = nr_r(fmaf(m21, m02, -nr_r(m01 * m22, &bad)), &bad);
    const f32 f12b = nr_r(fmaf(m22, m00, -nr_r(m02 * m20, &bad)), &bad);
    const f32 f10a = nr_r(fmaf(m10, m21, -nr_r(m11 * m20, &bad)), &bad);
    const f32 f10b = nr_r(m22 - m12, &bad);
    const f32 f9a = nr_r(fmaf(m01, m20, -nr_r(m00 * m21, &bad)), &bad);
    const f32 f9b = nr_r(m02 - m22, &bad);
    const f32 f8a = nr_r(fmaf(m00, m11, -nr_r(m01 * m10, &bad)), &bad);
    const f32 f8b = nr_r(m12 - m02, &bad);
    const f32 f7a = nr_r(fmaf(m20, f11a, nr_r(fmaf(m10, f12a, nr_r(m00 * f13a, &bad)), &bad)), &bad);
    const f32 f7b = nr_r(f11b + nr_r(f12b + f13b, &bad), &bad);
    if (bad)
        return false;
    NrPair* f = o->f;
    f[0] = (NrPair){m00, 1.0};
    f[1] = (NrPair){0.0, 0.0};
    f[2] = (NrPair){m10, 1.0};
    f[3] = (NrPair){m11, m12};
    f[4] = (NrPair){m20, 1.0};
    f[5] = (NrPair){m21, m22};
    f[6] = (NrPair){m02, m00};
    f[7] = (NrPair){f7a, f7b};
    f[8] = (NrPair){f8a, f8b};
    f[9] = (NrPair){f9a, f9b};
    f[10] = (NrPair){f10a, f10b};
    f[11] = (NrPair){f11a, f11b};
    f[12] = (NrPair){f12a, f12b};
    f[13] = (NrPair){f13a, f13b};
    const u32 compare = f7a < 0.0f ? 0x8u : f7a > 0.0f ? 0x4u : 0x2u;
    fp->fpscr = (fp->fpscr & ~(0x1Fu << 12)) | (nr_class_plain(f7a) << 12) | (compare << 12);
    o->cr0 = compare;
    o->suffix = 19u;
    if (compare == 0x2u) {
        o->r3 = 0u;
        o->cycles = 2u;
        o->stores = false;
        return true;
    }
    /* ps_res of two nonzero halves (no exception), and two Newton steps. */
    if (f7b == 0.0f)
        return false;
    f32 n0, n1;
    if (!nr_plain(ppc_approx_reciprocal(f7a), &n0) || !nr_plain(ppc_approx_reciprocal(f7b), &n1))
        return false;
    f32 a0 = 0.0f, a1 = 0.0f, q0 = 0.0f, q1 = 0.0f;
    for (unsigned step = 0; step < 2u; ++step) {
        a0 = nr_r(n0 + n0, &bad);
        a1 = nr_r(n1 + n1, &bad);
        q0 = nr_r(n0 * n0, &bad);
        q1 = nr_r(n1 * n1, &bad);
        n0 = nr_r(-fmaf(f7a, q0, -a0), &bad);
        n1 = nr_r(-fmaf(f7b, q1, -a1), &bad);
    }
    const f32 c[6][2] = {{f13a, f13b}, {f12a, f12b}, {f11a, f11b}, {f10a, f10b}, {f9a, f9b}, {f8a, f8b}};
    f32 scaled[6][2];
    for (unsigned i = 0; i < 6u; ++i) {
        scaled[i][0] = nr_r(c[i][0] * n0, &bad);
        scaled[i][1] = nr_r(c[i][1] * n0, &bad);
    }
    if (bad)
        return false;
    f[0] = (NrPair){n0, n1};
    f[5] = (NrPair){q0, q1};
    f[6] = (NrPair){a0, a1};
    for (unsigned i = 0; i < 6u; ++i)
        f[13u - i] = (NrPair){scaled[i][0], scaled[i][1]};
    fp->fpscr = (fp->fpscr & ~(0x1Fu << 12)) | (nr_class_plain(scaled[5][0]) << 12);
    o->r3 = 1u;
    o->cycles = 21u;
    o->stores = true;
    o->suffix = 1u;
    return true;
}

static int an_entry_inverse(CPUState* cpu) {
    NrClock k;
    if (!nr_clock_start(cpu, &k) || !nr_pairs_plain(cpu))
        return 0;
    const u32 src = cpu->gpr[3], dst = cpu->gpr[4];
    if (!nr_ram(cpu, src, 44u) || !nr_block(&k, 27u))
        return 0;
    static const u32 at[9] = {0u, 4u, 8u, 16u, 20u, 24u, 32u, 36u, 40u};
    u32 m[9];
    for (unsigned i = 0; i < 9u; ++i)
        m[i] = nr_word(cpu, src + at[i]);
    NrFp fp = {cpu->fpscr, false};
    AnInverse o;
    bool general = false;
    if (!an_inverse_single(&fp, m, &o)) {
        fp = (NrFp){cpu->fpscr, false};
        an_inverse_general(&fp, m, &o);
        if (fp.bad)
            return 0;
        general = true;
    }
    if (!nr_block(&k, o.cycles) || (o.stores && !nr_ram(cpu, dst, 36u)))
        return 0;
    if (o.stores) {
        /* psq_st f13, 0; f12, 12; f11, 24 (both halves); f10, 8; f9, 20; f8, 32 (the first). */
        const NrOut out = nr_out(cpu);
        const NrPair* f = o.f;
        nr_store32(&out, dst + 0u, nr_to_single_ftz(nr_bits(f[13].a)));
        nr_store32(&out, dst + 4u, nr_to_single_ftz(nr_bits(f[13].b)));
        nr_store32(&out, dst + 12u, nr_to_single_ftz(nr_bits(f[12].a)));
        nr_store32(&out, dst + 16u, nr_to_single_ftz(nr_bits(f[12].b)));
        nr_store32(&out, dst + 24u, nr_to_single_ftz(nr_bits(f[11].a)));
        nr_store32(&out, dst + 28u, nr_to_single_ftz(nr_bits(f[11].b)));
        nr_store32(&out, dst + 8u, nr_to_single_ftz(nr_bits(f[10].a)));
        nr_store32(&out, dst + 20u, nr_to_single_ftz(nr_bits(f[9].a)));
        nr_store32(&out, dst + 32u, nr_to_single_ftz(nr_bits(f[8].a)));
    }
    for (unsigned r = 0; r < 14u; ++r) {
        cpu->fpr[r] = o.f[r].a;
        cpu->ps1[r] = o.f[r].b;
    }
    cpu->gpr[3] = o.r3;
    cpu->cr = (cpu->cr & 0x0FFFFFFFu) | (o.cr0 << 28);
    cpu->fpscr = fp.fpscr;
    cpu->downcount = k.downcount;
    cpu->cycle_observation_suffix = o.suffix;
    cpu->pc = cpu->lr & ~3u;
    s_an_general[AN_INVERSE_TRANSPOSE] += general;
    return 1;
}

/* --- The entries. --------------------------------------------------------- */

static const u32 AN_KEY_F_BOUNDARIES[] = {BLUEWAKE_ANIM_HERMITE, AN_HERMITE_RETURN_0, AN_HERMITE_RETURN_1};
static const u32 AN_TRANSFORM_BOUNDARIES[] = {
    AN_KEY_F_CALL, 0x802F09E8u, 0x802F0A60u, 0x802F0AD8u, 0x802F0D04u, 0x802F0D80u, 0x802F0DFCu,
    BLUEWAKE_ANIM_HERMITE, AN_HERMITE_RETURN_0, AN_HERMITE_RETURN_1};

/* Each entry's replay: 1 done (committed), 0 declined, 2 declined where the
 * single-precision splines did not apply (the general replay may run). */
AN int an_try_hermite(CPUState* cpu, bool fast) {
    An s;
    AnLog log;
    if (!nr_clock_start(cpu, &s.k))
        return 0;
    an_start(&s, cpu, &log);
    s.fast = fast;
    s.g[2] = cpu->gpr[2];
    an_valid(&s, s.g[2] - 13000u, 16u);
    an_hermite(&s, fast);
    if (!an_ok(&s))
        return s.retry ? 2 : 0;
    an_commit(&s);
    return 1;
}

AN int an_try_key_f(CPUState* cpu, bool fast) {
    An s;
    AnLog log;
    if (!nr_clock_start(cpu, &s.k))
        return 0;
    an_start(&s, cpu, &log);
    s.fast = fast;
    s.g[2] = cpu->gpr[2];
    const u32 sp = cpu->gpr[1];
    s.lo0 = sp - 16u;
    s.hi0 = sp + 8u;
    if (!nr_ram(cpu, s.lo0, 24u))
        return 0;
    an_key_f(&s, fast);
    if (!an_ok(&s))
        return s.retry ? 2 : 0;
    if (s.crossed && !an_silent(cpu, AN_KEY_F_BOUNDARIES, sizeof AN_KEY_F_BOUNDARIES / sizeof AN_KEY_F_BOUNDARIES[0]))
        return 0;
    an_commit(&s);
    return 1;
}

AN int an_try_key_s(CPUState* cpu, bool fast) {
    An s;
    AnLog log;
    f64 scale;
    if (!nr_clock_start(cpu, &s.k) || !an_gqr5(cpu, &scale))
        return 0;
    an_start(&s, cpu, &log);
    s.fast = fast;
    s.g[2] = cpu->gpr[2];
    const u32 sp = cpu->gpr[1];
    s.lo0 = sp - 16u;
    s.hi0 = sp + 8u;
    if (!nr_ram(cpu, s.lo0, 24u))
        return 0;
    an_key_s(&s, scale, fast);
    if (!an_ok(&s))
        return s.retry ? 2 : 0;
    an_commit(&s);
    return 1;
}

AN int an_try_transform(CPUState* cpu, bool fast) {
    An s;
    AnLog log;
    f64 scale;
    if (!nr_clock_start(cpu, &s.k) || !an_gqr5(cpu, &scale) || !nr_pairs_plain(cpu))
        return 0;
    an_start(&s, cpu, &log);
    s.fast = fast;
    s.g[2] = cpu->gpr[2];
    const u32 sp = cpu->gpr[1], dst = cpu->gpr[5];
    /* The frames: this one's 48 bytes, a callee's 16 below it, and the
     * return address's word above. The transform: 32 bytes. */
    s.lo0 = sp - 64u;
    s.hi0 = sp + 8u;
    s.lo1 = dst;
    s.hi1 = dst + 32u;
    if (!nr_ram(cpu, s.lo0, 72u) || !nr_ram(cpu, dst, 32u) || !nr_apart(dst, 32u, s.lo0, 72u))
        return 0;
    an_transform(&s, scale, fast);
    if (!an_ok(&s))
        return s.retry ? 2 : 0;
    if (s.crossed &&
        !an_silent(cpu, AN_TRANSFORM_BOUNDARIES, sizeof AN_TRANSFORM_BOUNDARIES / sizeof AN_TRANSFORM_BOUNDARIES[0]))
        return 0;
    an_commit(&s);
    return 1;
}

/* The general replays, out of line: they run only where the fast one met a
 * value the single-precision splines do not take. */
AN_CALLEE int an_general_hermite(CPUState* cpu) { return an_try_hermite(cpu, false); }
AN_CALLEE int an_general_key_f(CPUState* cpu) { return an_try_key_f(cpu, false); }
AN_CALLEE int an_general_key_s(CPUState* cpu) { return an_try_key_s(cpu, false); }
AN_CALLEE int an_general_transform(CPUState* cpu) { return an_try_transform(cpu, false); }

static int an_entry_hermite(CPUState* cpu) {
    const int done = an_try_hermite(cpu, true);
    if (done != 2)
        return done;
    const int general = an_general_hermite(cpu);
    s_an_general[AN_HERMITE] += (unsigned)general;
    return general;
}
static int an_entry_key_f(CPUState* cpu) {
    const int done = an_try_key_f(cpu, true);
    if (done != 2)
        return done;
    const int general = an_general_key_f(cpu);
    s_an_general[AN_KEY_F] += (unsigned)general;
    return general;
}
static int an_entry_key_s(CPUState* cpu) {
    const int done = an_try_key_s(cpu, true);
    if (done != 2)
        return done;
    const int general = an_general_key_s(cpu);
    s_an_general[AN_KEY_S] += (unsigned)general;
    return general;
}
static int an_entry_transform(CPUState* cpu) {
    const int done = an_try_transform(cpu, true);
    if (done != 2)
        return done;
    const int general = an_general_transform(cpu);
    s_an_general[AN_TRANSFORM] += (unsigned)general;
    return general;
}

int bluewake_native_anim(CPUState* cpu, u32 address) {
    unsigned which;
    int done;
    switch (address) {
    case BLUEWAKE_ANIM_HERMITE: which = AN_HERMITE; done = an_entry_hermite(cpu); break;
    case BLUEWAKE_ANIM_KEY_F: which = AN_KEY_F; done = an_entry_key_f(cpu); break;
    case BLUEWAKE_ANIM_KEY_S: which = AN_KEY_S; done = an_entry_key_s(cpu); break;
    case BLUEWAKE_ANIM_TRANSFORM: which = AN_TRANSFORM; done = an_entry_transform(cpu); break;
    case BLUEWAKE_ANIM_INVERSE_TRANSPOSE: which = AN_INVERSE_TRANSPOSE; done = an_entry_inverse(cpu); break;
    default: return 0;
    }
    if (done)
        s_an_runs[which]++;
    else
        s_an_declined[which]++;
    return done;
}
