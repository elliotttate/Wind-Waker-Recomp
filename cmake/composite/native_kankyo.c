/* The environment's colour blends (GZLE01 s16_data_ratio_set in d_kankyo and
 * in d_kyeff, kankyo_color_ratio_set), native.
 *
 * dScnKy_env_light_c blends every light, ambient, fog and sky colour of the
 * scene between palettes each frame, a component at a time:
 * kankyo_color_ratio_set makes three calls of s16_data_ratio_set (a + (s16)(t
 * x (b - a)), through the int-to-float conversion's magic double and fctiwz),
 * adds, scales by the scene's colour ratio and clamps to 0..255. Small as they
 * are, at Dragon Roost the two were 1.6 percent of the game thread (0.9 at
 * Forest Haven), nearly all of it the translation's block machinery.
 *
 * Each replays its blocks in order on the same values (native_replay.h: the
 * translation's inline floating-point paths on a local FPSCR, fctiwz as
 * GXRuntime's ppc_fctiw leaves it, the clock block by block) and commits only
 * at the end: every stack store in place (kankyo_color_ratio_set's frame,
 * with its saved f29..f31 and r28..r31, and s16_data_ratio_set's below it as
 * its last call leaves it), the registers as the last instruction to write
 * each left them (r0, r3, r4 and r11; f0, f1 and f2, with the second halves
 * the single operations wrote; the second halves of f29..f31 as the paired
 * save and restore round them), CR0 from the last compare, the FPSCR, the
 * reservation, the cycles (20 for s16_data_ratio_set; 134 to 136 for
 * kankyo_color_ratio_set, with its inline register save and restore) and the
 * last cycle suffix (stfd's 6, mtlr's 2), pc at the return address.
 *
 * It declines, changing nothing, unless that is certain: FP available,
 * rounding to nearest, paired singles unscaled, no exception pending, no
 * write journal, the frames and the constants it reads plain RAM with the
 * constants clear of the frames, the conversion's magic double the usual
 * 0x4330000080000000, every block prepaid and none stopped for the budget,
 * and every floating-point operation on the translation's inline path (a
 * non-finite ratio or scale, which would go to the interpreter, declines).
 *
 * tests/native_kankyo_test.c compares all three with the translation, every
 * register and byte. No identifier here may be `ctx`. */
#include "native_kankyo.h"
#include "native_replay.h"

#include <stdio.h>

int bluewake_native_kankyo_enabled;

enum { KY_S16, KY_COLOR, KY_KYEFF_S16, KY_COUNT };
static unsigned long long s_ky_runs[KY_COUNT], s_ky_declined[KY_COUNT];

void bluewake_native_kankyo_report(void) {
    fprintf(stderr,
            "[native-kankyo] s16-ratio=%llu/%llu color-ratio=%llu/%llu kyeff-s16-ratio=%llu/%llu (native/declined)\n",
            s_ky_runs[0], s_ky_declined[0], s_ky_runs[1], s_ky_declined[1], s_ky_runs[2], s_ky_declined[2]);
}

#define KY_MAGIC 0x4330000080000000ull
#define KY_MAGIC_HIGH 0x43300000u
#define KY_KANKYO_MAGIC (-21080)     /* lfd f2, -21080(r2) in d_kankyo */
#define KY_KYEFF_MAGIC (-20784)      /* lfd f2, -20784(r2) in d_kyeff */
#define KY_ALL_COL_RATIO 0x803E56B0u /* g_env_light.mAllColRatio: lis r3,0x803E; addi r3,r3,0x4AB4; lfs f0,3068(r3) */

/* One s16_data_ratio_set(r3, r4, f1) on values: what it leaves. */
typedef struct KyS16 {
    u32 r0, r3, w12;
    u64 stored;  /* f0 as stfd writes it, and as the register holds it */
    f64 product; /* fmuls's result: f0's second half */
} KyS16;

static inline __attribute__((always_inline)) KyS16 ky_s16_calc(NrFp* fp, u32 r3, u32 r4, f64 ratio) {
    KyS16 c;
    const u32 a = (u32)(s32)(s16)r3;          /* extsh r3, r3 */
    const s32 diff = (s32)(s16)r4 - (s32)a;   /* extsh r0, r4; subf r0, r3, r0 */
    c.w12 = (u32)diff ^ 0x80000000u;          /* xoris r0, r0, 0x8000; stw r0, 12(r1) */
    /* lis r0, 17200; stw; lfd f0, 8(r1); fsubs f0, f0, f2: the conversion's
     * double less the magic is diff itself, exactly (|diff| < 2^17, so the
     * single rounding keeps it too), and its FPRF is the next instruction's
     * to replace. fmuls f0, f1, f0: diff is its own 25-bit multiplier. */
    if (!nr_finite(ratio))
        fp->bad = true;
    const f32 single = nr_round(fp, ratio * (f64)diff);
    nr_fprf(fp, nr_class32(single));
    fp->fpscr &= ~(NR_FPSCR_FI | NR_FPSCR_FR);
    c.product = (f64)single;
    f64 f0 = c.product;
    u64 bits;
    if (nr_fctiwz(fp, c.product, &bits)) /* fctiwz f0, f0 */
        f0 = f64_value(bits);
    c.stored = f64_bits(f0);                         /* stfd f0, 16(r1); lwz r0, 20(r1) */
    c.r0 = a + (u32)(s32)(s16)(u32)c.stored;         /* extsh r0, r0; add r0, r3, r0 */
    c.r3 = (u32)(s32)(s16)c.r0;                      /* extsh r3, r0 */
    return c;
}

/* s16_data_ratio_set's frame stores, at r1 = sp on entry. */
static inline __attribute__((always_inline)) void ky_s16_store(const NrOut* o, u32 sp, const KyS16* c) {
    const u32 frame = sp - 32u;
    nr_store32(o, frame, sp);                 /* stwu r1, -32(r1) */
    nr_store32(o, frame + 12u, c->w12);       /* stw r0, 12(r1) */
    nr_store32(o, frame + 8u, KY_MAGIC_HIGH); /* stw r0, 8(r1) */
    nr_store64(o, frame + 16u, c->stored);    /* stfd f0, 16(r1) */
}

/* s16_data_ratio_set (r3 a, r4 b, f1 the ratio): one block of 20 cycles. */
static int ky_s16(CPUState* cpu, s32 magic_offset) {
    NrClock k;
    if (!nr_clock_start(cpu, &k))
        return 0;
    const u32 sp = cpu->gpr[1], frame = sp - 32u, magic = cpu->gpr[2] + (u32)magic_offset;
    if (!nr_ram(cpu, frame, 24u) || !nr_ram(cpu, magic, 8u) || !nr_apart(magic, 8u, frame, 24u) ||
        nr_dword(cpu, magic) != KY_MAGIC || !nr_block(&k, 20u))
        return 0;
    NrFp fp = {cpu->fpscr, false};
    const KyS16 c = ky_s16_calc(&fp, cpu->gpr[3], cpu->gpr[4], cpu->fpr[1]);
    if (fp.bad)
        return 0;
    const NrOut o = nr_out(cpu);
    ky_s16_store(&o, sp, &c);
    cpu->gpr[0] = c.r0;
    cpu->gpr[3] = c.r3;
    cpu->fpr[0] = f64_value(c.stored);
    cpu->ps1[0] = c.product;
    cpu->fpr[2] = f64_value(KY_MAGIC);
    cpu->fpscr = fp.fpscr;
    cpu->downcount = k.downcount;
    cpu->cycle_observation_suffix = 6u; /* stfd f0, 16(r1) */
    cpu->pc = cpu->lr & ~3u;
    return 1;
}

/* kankyo_color_ratio_set (r3 b0A, r4 b0B, f1 blendAB0, r5 b1A, r6 b1B, f2
 * blendAB1, r7 add, f3 mul). Its frame is 96 bytes; the three calls'
 * frames lie 32 below it. */
static int ky_color(CPUState* cpu) {
    NrClock k;
    if (!nr_clock_start(cpu, &k) || !nr_pairs_plain(cpu))
        return 0;
    const u32 sp = cpu->gpr[1], frame = sp - 96u, low = sp - 128u, magic = cpu->gpr[2] + (u32)KY_KANKYO_MAGIC;
    /* Every store lies in [sp - 128, sp + 8). */
    if (!nr_ram(cpu, low, 136u) || !nr_ram(cpu, magic, 8u) || !nr_ram(cpu, KY_ALL_COL_RATIO, 4u) ||
        !nr_apart(magic, 8u, low, 136u) || !nr_apart(KY_ALL_COL_RATIO, 4u, low, 136u) ||
        nr_dword(cpu, magic) != KY_MAGIC)
        return 0;
    NrFp fp = {cpu->fpscr, false};
    const f64 f1 = cpu->fpr[1], f2 = cpu->fpr[2], f3 = cpu->fpr[3];
    const KyS16 c1 = ky_s16_calc(&fp, cpu->gpr[3] & 0xFFu, cpu->gpr[4] & 0xFFu, f1);
    const KyS16 c2 = ky_s16_calc(&fp, cpu->gpr[5] & 0xFFu, cpu->gpr[6] & 0xFFu, f1);
    const KyS16 c3 = ky_s16_calc(&fp, c1.r3, c2.r3, f2);
    /* 8018F958: add r0, r3, r30; extsh r3, r0; the conversion; lfs f0, the
     * colour ratio; fmuls f0, f0, f31; fmuls f0, f1, f0; fctiwz; lwz r3. */
    const u32 sum = (u32)(s32)(s16)(c3.r3 + cpu->gpr[7]);
    const u32 w12 = sum ^ 0x80000000u;
    const f64 rt = (f64)(s32)sum; /* fsubs f1, f0, f1: exactly (as in the calls); its FPRF the fmuls's to replace */
    const f64 ratio = nr_single_bits(nr_word(cpu, KY_ALL_COL_RATIO));
    const f64 scale = nr_fmuls(&fp, ratio, f3);
    const f64 product = nr_fmuls(&fp, rt, scale);
    f64 f0 = product;
    u64 bits;
    if (nr_fctiwz(&fp, product, &bits))
        f0 = f64_value(bits);
    if (fp.bad)
        return 0;
    const u64 stored = f64_bits(f0);
    const u32 xer = cpu->xer;
    u32 r3 = (u32)stored, r0 = (u32)(s32)(s16)r3;
    const bool negative = (s32)r0 < 0; /* extsh. r0, r3; bge */
    if (negative)
        r3 = 0u;                       /* li r3, 0 */
    r0 = (u32)(s32)(s16)r3;            /* extsh r0, r3; cmpwi r0, 255; ble */
    const u32 cr = nr_cr0_signed(cpu->cr, xer, (s32)r0, 255);
    const bool over = (s32)r0 > 255;
    if (over)
        r3 = 255u;                     /* li r3, 255 */
    /* The blocks: 8018F8E4 (11), the inline save, 8018F910 (9), the call
     * (20), 8018F934 (5), the call, 8018F948 (4), the call, 8018F958 (19),
     * 8018F9A4 (1) when negative, 8018F9A8 (3), 8018F9B4 (1) when over,
     * 8018F9B8 (8), the inline restore, 8018F9D8 (5). */
    if (!nr_block(&k, 11u) || !nr_inline_gpr(&k) || !nr_block(&k, 9u) || !nr_block(&k, 20u) ||
        !nr_block(&k, 5u) || !nr_block(&k, 20u) || !nr_block(&k, 4u) || !nr_block(&k, 20u) ||
        !nr_block(&k, 19u) || (negative && !nr_block(&k, 1u)) || !nr_block(&k, 3u) ||
        (over && !nr_block(&k, 1u)) || !nr_block(&k, 8u) || !nr_inline_gpr(&k) || !nr_block(&k, 5u))
        return 0;

    /* The stores, from values read first. */
    const u32 lr = cpu->lr;
    u64 saved[3];
    u32 pairs[3][2], gprs[4];
    f64 rounded[3];
    for (unsigned i = 0; i < 3u; ++i) {
        saved[i] = f64_bits(cpu->fpr[31u - i]);
        pairs[i][0] = nr_to_single_ftz(saved[i]);
        pairs[i][1] = nr_to_single_ftz(f64_bits(cpu->ps1[31u - i]));
        rounded[i] = f64_value(nr_to_double(pairs[i][1]));
    }
    for (unsigned i = 0; i < 4u; ++i)
        gprs[i] = cpu->gpr[28u + i];
    const NrOut o = nr_out(cpu);
    nr_store32(&o, frame, sp);              /* stwu r1, -96(r1) */
    nr_store32(&o, sp + 4u, lr);            /* mflr r0; stw r0, 100(r1) */
    for (unsigned i = 0; i < 3u; ++i) {     /* f31 at 80, f30 at 64, f29 at 48: stfd, psq_st */
        const u32 at = frame + 80u - 16u * i;
        nr_store64(&o, at, saved[i]);
        nr_store32(&o, at + 8u, pairs[i][0]);
        nr_store32(&o, at + 12u, pairs[i][1]);
    }
    for (unsigned i = 0; i < 4u; ++i)       /* _savegpr_28 at r11 = r1 + 48 */
        nr_store32(&o, frame + 32u + 4u * i, gprs[i]);
    ky_s16_store(&o, frame, &c3);           /* the three calls' frame, as the last leaves it */
    nr_store32(&o, frame + 12u, w12);
    nr_store32(&o, frame + 8u, KY_MAGIC_HIGH);
    nr_store64(&o, frame + 16u, stored);

    /* The registers: f29..f31 as psq_l, then lfd, restore them. */
    cpu->gpr[0] = lr; /* lwz r0, 100(r1); mtlr r0 */
    cpu->gpr[3] = r3;
    cpu->gpr[4] = c2.r3;
    cpu->gpr[11] = frame + 48u;
    cpu->fpr[0] = f64_value(stored);
    cpu->ps1[0] = product;
    cpu->fpr[1] = cpu->ps1[1] = rt;
    cpu->fpr[2] = f64_value(KY_MAGIC);
    for (unsigned i = 0; i < 3u; ++i)
        cpu->ps1[31u - i] = rounded[i];
    cpu->cr = cr;
    cpu->fpscr = fp.fpscr;
    cpu->downcount = k.downcount;
    cpu->cycle_observation_suffix = 2u; /* mtlr r0 */
    cpu->pc = lr & ~3u;
    return 1;
}

int bluewake_native_kankyo(CPUState* cpu, u32 address) {
    unsigned which;
    int done;
    switch (address) {
    case BLUEWAKE_KANKYO_S16_RATIO: which = KY_S16; done = ky_s16(cpu, KY_KANKYO_MAGIC); break;
    case BLUEWAKE_KANKYO_COLOR_RATIO: which = KY_COLOR; done = ky_color(cpu); break;
    case BLUEWAKE_KYEFF_S16_RATIO: which = KY_KYEFF_S16; done = ky_s16(cpu, KY_KYEFF_MAGIC); break;
    default: return 0;
    }
    if (done)
        s_ky_runs[which]++;
    else
        s_ky_declined[which]++;
    return done;
}
