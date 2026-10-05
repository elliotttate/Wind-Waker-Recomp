/* The seventh set (cmake/composite/native_libm.c, native_bgblk.c, native_rot.c,
 * native_calc.c, native_geom.c, native_jas.c: leaf compute code) against the
 * translations it stands in for.
 *
 * Build and run from the worktree root (x64; the Visual Studio environment;
 * SNAP = E:\Github\Wind-Waker-Recomp-natives7-snap), each command started
 * through `cmd /c start "" /b /wait /affinity FFFF`:
 *
 *   clang -O2 -march=x86-64-v3 -ffp-contract=off -Icmake/composite -Itests
 *     -I%SNAP%\recompcore\GXRuntime\include
 *     -I%SNAP%\recompcore\Source\Core\Core\PowerPC\StaticRecomp
 *     tests/native_leaf_test.c cmake/composite/native_libm.c cmake/composite/native_bgblk.c
 *     cmake/composite/native_rot.c cmake/composite/native_calc.c cmake/composite/native_geom.c
 *     cmake/composite/native_jas.c cmake/composite/direct_calls.c
 *     %SNAP%\gxruntime.lib -o build/natives7/native_leaf_test.exe
 *   build\natives7\native_leaf_test %SNAP%\gGZLE01_recomp.dll [CASES_PER_FUNCTION=60000]
 *     [BENCH_CALLS=1000000] [NAME_PREFIX,...]
 *
 * The hooked build (-DNATIVE7_HOOKED=1, with the hooked chunks: the recipe is
 * in native_leaf_hooked.h) also runs every case the native runs through the
 * hooked chunks from the function's entry, natives on and off.
 *
 * MODULE.dll is a Windows game module; the test reads it and writes nothing
 * but its own memory. For each function: random registers, flags, FPSCR,
 * reservation (sometimes on a granule the function stores to) and cycle
 * state; its arguments and the data it reads (the constants in r2's small
 * data, the random seeds in r13's, a collision background's vertices,
 * triangles and blocks, the sine table, the arc tangent table, the point
 * winds) as play has them mostly and anything sometimes - zeros, denormals,
 * huge values, infinities and NaNs, fmod's every path (exact multiples,
 * equal magnitudes, subnormal operands and results, long exponent gaps);
 * pointers in RAM, sometimes not (unaligned, a mirror, the hardware, past
 * MEM1), sometimes aliasing each other; the turn's budget spent or not;
 * deadlines near and inside the work; a write journal, aliases over MEM1, an
 * exception pending; the host busy (the module then asks its edge service at
 * every boundary it crosses, so a native that ran across one fails the
 * comparison), the edge filter off and the boundaries watched. Every byte of
 * the CPU state and of the writable pages must match - or, where the native
 * declines, nothing may have changed. */
#include "native_bgblk.h"
#include "native_calc.h"
#include "native_geom.h"
#include "native_jas.h"
#include "native_libm.h"
#include "native_rot.h"
#include "native7_harness.h"

#include <math.h>

#ifndef NATIVE7_HOOKED
/* native_gx.c's, which the test does not link: every call tried. */
int bluewake_native_gate_enabled = 1;
#endif

#define STACK 0x80F00000u  /* 0x2000: the frames */
#define SDA2 0x80F10000u   /* 0x8000: r2's small data (the constants), r2 at its end */
#define R2 (SDA2 + 0x8000u)
#define SDA 0x80F18000u    /* 0x8000: r13's small data (cM_rnd's seeds, the sine table's pointers) */
#define R13 (SDA + 0x8000u)
#define DATA 0x80F20000u   /* 0x8000: objects, vectors, a background's tables, point winds */
#define TABLES 0x80F28000u /* 0x8000: the sine table (JMANewSinTable(12): 5120 floats) */
#define FMOD_ZERO 0x80371178u   /* __ieee754_fmod's Zero[] = {0.0, -0.0} */
#define ATAN_TABLE 0x803952C8u  /* cM_atan2s's atntable (1025 halves) */
#define LIBM_NAN 0x803F6800u    /* __float_nan, __float_huge (vectle_calc's sqrt) */
#define ENV_LIGHT 0x803E4AB4u   /* g_env_light */
#define WIND (ENV_LIGHT + 1936u) /* its mpWindInfluence[30] */
#define OSC_CURVES 0x80368898u  /* TOscillator::calc's curve tables (3 x 17 floats) */
#define OSC_FORCE_STOP 0x80398CC0u /* oscTableForceStop */
#define CVT_CONSTANTS 0x80370FC8u /* __cvt_fp2unsigned's 0, 2^32, 2^31 */
#define CALC_SW_TABLE 0x80398C28u /* JASystem::Driver::calc_sw_table (28 calc types, 3 bytes each) */
#define MIX_L0_TABLE 0x80398C9Cu  /* updateMixer's jump tables: its first part's source */
#define MIX_L1_TABLE 0x80398C7Cu  /* and its second's */

static const Region k_regions[] = {
    {STACK, 0x2000u}, {SDA2, 0x8000u},       {SDA, 0x8000u},         {DATA, 0x8000u},
    {TABLES, 0x8000u}, {0x80371000u, 0x1000u}, {0x80395000u, 0x1000u}, {0x803F6000u, 0x1000u},
    {0x803E4000u, 0x2000u}, {0x80368000u, 0x1000u}, {0x80370000u, 0x1000u}, {0x80398000u, 0x2000u},
};

typedef struct Native {
    u32 entry;
    const char* name;
    s64 cycles;      /* about its path, for the clock's edges */
    bool boundaries; /* every path crosses a boundary between chunks */
    NativeFn fn;
} Native;

static int native_libm(CPUState* cpu, u32 address) { return bluewake_native_libm(cpu, address); }
static int native_bgblk(CPUState* cpu, u32 address) { return bluewake_native_bgblk(cpu, address); }
static int native_rot(CPUState* cpu, u32 address) { return bluewake_native_rot(cpu, address); }
static int native_calc(CPUState* cpu, u32 address) { return bluewake_native_calc(cpu, address); }
static int native_geom(CPUState* cpu, u32 address) { return bluewake_native_geom(cpu, address); }
static int native_jas(CPUState* cpu, u32 address) { return bluewake_native_jas(cpu, address); }

enum {
    N_IEEE_FMOD, N_FMOD, N_RAD2S, N_RND, N_RNDF, N_RNDFX, N_SIN, N_COS, N_TAN, N_BLCK_MIN_MAX, N_BLCK_BND,
    N_EULER_TO_QUAT, N_ATAN2S, N_CALC_PLA, N_POLAR_VAL,
    N_PNTWIND, N_OSC_OFFSET, N_OSC_CALC, N_EFFECTOR, NATIVE_COUNT
};

static const Native k_natives[NATIVE_COUNT] = {
    {0x8032EC1Cu, "__ieee754_fmod", 110, false, native_libm},
    {0x80330E34u, "fmod", 120, false, native_libm},
    {0x80246044u, "cM_rad2s", 140, true, native_libm},
    {0x802462C8u, "cM_rnd", 360, true, native_libm},
    {0x802463B0u, "cM_rndF", 380, true, native_libm},
    {0x802463E8u, "cM_rndFX", 390, true, native_libm},
    {0x80330C84u, "sin", 160, false, native_libm},
    {0x8033071Cu, "cos", 160, false, native_libm},
    {0x80330D5Cu, "tan", 200, false, native_libm},
    {0x80247C4Cu, "cBgW::MakeBlckMinMax", 30, false, native_bgblk},
    {0x80247CD4u, "cBgW::MakeBlckBnd", 800, false, native_bgblk},
    {0x80301150u, "JMAEulerToQuat", 50, false, native_rot},
    {0x802460D0u, "cM_atan2s", 70, false, native_calc},
    {0x8024A6F0u, "cM3d_CalcPla", 110, true, native_geom},
    {0x80254214u, "cSPolar::Val", 250, true, native_geom},
    {0x8008A230u, "dKyw_pntwind_get_info", 200, false, native_geom},
    {0x8028DF2Cu, "JASystem::TOscillator::getOffset", 150, false, native_jas},
    {0x8028E238u, "JASystem::TOscillator::calc", 150, false, native_jas},
    {0x8028C3A8u, "JASystem::TChannel::updateEffectorParam", 600, false, native_jas},
};

#ifdef NATIVE7_HOOKED
#include "native_leaf_hooked.h"
#endif

typedef struct Case {
    CPUState cpu;
    bool must_decline, journal, aliases, flush;
} Case;

static void fill(u8* ram, u32 start, u32 bytes) {
    for (u32 i = 0; i < bytes; i += 4u)
        put32(ram, start + i, next());
}

/* A clean case: every pointer plain and every vector ordinary (half the
 * cases of the natives that read many: so that at least half run). */
static bool s_clean;

/* A pointer: in the area mostly; sometimes unaligned, a mirror, the
 * hardware, across or past MEM1's end, or anything but MEM1 (whose pages
 * outside the test's areas are read-only: a store there would fault). */
static u32 pointer(u32 area, u32 span, u32 align) {
    const u32 at = area + (below(span) & ~(align - 1u));
    if (s_clean)
        return at;
    switch (below(96u)) {
    case 0: return at + 1u + below(3u);
    case 1: return at | 0x40000000u;
    case 2: return 0xCC000000u + 4u * below(64u);
    case 3: return 0xCC008000u; /* the gather pipe */
    case 4: return GC_RAM_BASE + GC_MAIN_RAM_SIZE - 2u + 4u * below(4u);
    case 5: {
        const u32 any = next();
        return any - GC_RAM_BASE < 0x02000000u ? any ^ 0x10000000u : any;
    }
    default: return at;
    }
}

/* --- Values. -------------------------------------------------------------- */

/* A double: the shape fmod's paths care about. */
static u64 any_double(void) {
    const u64 sign = (u64)(next() & 1u) << 63;
    switch (below(16u)) {
    case 0: return sign;                                                              /* zero */
    case 1: return sign | (((u64)next() << 32 | next()) & 0x000FFFFFFFFFFFFFull);      /* subnormal */
    case 2: return sign | 0x7FF0000000000000ull;                                      /* infinity */
    case 3: return sign | 0x7FF0000000000000ull | (1ull + (((u64)next() << 32 | next()) & 0x000FFFFFFFFFFFFEull));
    case 4: return ((u64)next() << 32) | next();                                      /* anything */
    case 5: return sign | ((u64)(1u + below(2045u)) << 52) | (((u64)next() << 32 | next()) & 0x000FFFFFFFFFFFFFull);
    default: return sign | ((u64)(1023u - 40u + below(80u)) << 52) | (((u64)next() << 32 | next()) & 0x000FFFFFFFFFFFFFull);
    }
}

static f64 ordinary_single(void) {
    return (f64)single_of((next() & 0x80000000u) | ((110u + below(30u)) << 23) | (next() & 0x007FFFFFu));
}

/* fmod's arguments: x and y as play has them mostly (cM_rnd's sums against
 * 1.0, angles against 2 pi, a few exponents apart), and every path. */
static void fmod_args(f64* x, f64* y) {
    const unsigned kind = below(24u);
    f64 a, b;
    switch (kind) {
    case 0: case 1: case 2: case 3: /* cM_rnd: [0, 3) against 1.0 */
        a = (f64)(f32)((f32)(next() % 30269u) / 30269.0f + (f32)(next() % 30307u) / 30307.0f +
                       (f32)(next() % 30323u) / 30323.0f);
        b = 1.0;
        break;
    case 4: case 5: case 6: /* cM_rad2s: an angle against 2 pi */
        a = (f64)(f32)((f32)((s32)next() % 200000) * 0.01f);
        b = 6.2831854820251465;
        break;
    case 7: case 8: case 9: { /* a few exponents apart */
        const u64 ea = 1023u - 30u + below(60u);
        const u64 eb = ea - below(64u) + 3u;
        a = f64_value(((u64)(next() & 1u) << 63) | (ea << 52) | ((((u64)next() << 32) | next()) & 0x000FFFFFFFFFFFFFull));
        b = f64_value(((u64)(next() & 1u) << 63) | (eb << 52) | ((((u64)next() << 32) | next()) & 0x000FFFFFFFFFFFFFull));
        break;
    }
    case 10: { /* an exact multiple, or the same magnitude */
        b = ordinary_single();
        a = below(3u) == 0u ? -b : b * (f64)(s32)(next() % 2000u - 1000);
        break;
    }
    case 11: { /* a long exponent gap */
        const u64 ea = 1u + below(2046u), eb = 1u + below(2046u);
        a = f64_value((ea << 52) | ((((u64)next() << 32) | next()) & 0x000FFFFFFFFFFFFFull));
        b = f64_value((eb << 52) | ((((u64)next() << 32) | next()) & 0x000FFFFFFFFFFFFFull));
        break;
    }
    case 12: { /* subnormal results: small operands */
        a = f64_value(((u64)(1u + below(60u)) << 52) | ((((u64)next() << 32) | next()) & 0x000FFFFFFFFFFFFFull));
        b = f64_value(((u64)below(40u) << 52) | ((((u64)next() << 32) | next()) & 0x000FFFFFFFFFFFFFull));
        break;
    }
    case 13: /* the low words alike: the |x| = |y| test */
        a = f64_value(any_double());
        b = f64_value((f64_bits(a) & 0xFFFFFFFF00000000ull) ^ ((u64)below(4u) << 32) | (f64_bits(a) & 0xFFFFFFFFull));
        break;
    default:
        a = f64_value(any_double());
        b = f64_value(any_double());
        break;
    }
    *x = a;
    *y = b;
}

/* r2's constants (the decomp's literals) mostly; sometimes anything. */
static void constants(u8* ram) {
    if (below(32u) == 0u)
        return; /* the random fill */
    const u64 magic = 0x4330000080000000ull;
    putd(ram, R2 - 16712u, 6.2831854820251465);      /* cM_rad2s: 2 pi */
    putd(ram, R2 - 16704u, 10430.3779296875);        /* 32768 / pi */
    putf(ram, R2 - 16696u, 1024.0f);                 /* U_GetAtanTable */
    putf(ram, R2 - 16692u, 0.0f);                    /* cM_atan2s */
    putf(ram, R2 - 16544u, 3.8146973e-06f);          /* G_CM3D_F_ABS_MIN */
    put64(ram, R2 - 16680u, magic);                  /* the int conversion's */
    putf(ram, R2 - 16688u, 9.58738e-05f);            /* cM_atan2f: pi / 32768 */
    putf(ram, R2 - 16672u, 30269.0f);                /* cM_rnd */
    putf(ram, R2 - 16668u, 30307.0f);
    putf(ram, R2 - 16664u, 30323.0f);
    putd(ram, R2 - 16656u, 1.0);
    putf(ram, R2 - 16644u, 0.5f);                    /* cM_rndFX */
    putf(ram, R2 - 16648u, 2.0f);
    putf(ram, R2 - 16620u, 1000000000.0f);           /* MakeBlckBnd: G_CM3D_F_INF */
    putf(ram, R2 - 16616u, -1000000000.0f);
    putf(ram, R2 - 16612u, 1.0f);
    putf(ram, R2 - 16516u, 0.02f);                   /* cM3d_CalcPla */
    putf(ram, R2 - 16520u, 1.0f);
    putf(ram, R2 - 16540u, 0.0f);
    putf(ram, R2 - 12884u, 0.5f);                    /* PSVECMag */
    putf(ram, R2 - 12880u, 3.0f);
    putf(ram, R2 - 16284u, 0.0f);                    /* cSPolar::Val */
    putd(ram, R2 - 16280u, 0.0);
    putd(ram, R2 - 16272u, 0.5);
    putd(ram, R2 - 16264u, 3.0);
    putf(ram, R2 - 16256u, 10430.378f);
    putf(ram, R2 - 29488u, 0.0f);                    /* dKyw_pntwind_get_info */
    putd(ram, R2 - 29344u, 0.5);
    putd(ram, R2 - 29336u, 3.0);
    putf(ram, R2 - 29472u, 1.0f);
    putd(ram, R2 - 29288u, 0.0);                     /* vectle_calc */
    putd(ram, R2 - 29280u, 0.5);
    putd(ram, R2 - 29272u, 3.0);
    putf(ram, R2 - 29264u, 0.0f);
    /* libm's sin, cos and tan: __ieee754_rem_pio2, the kernels (fdlibm's). */
    putd(ram, R2 - 12208u, 0.0);
    putd(ram, R2 - 12200u, 1.57079632673412561417e+00);  /* pio2_1 */
    putd(ram, R2 - 12192u, 6.07710050650619224932e-11);  /* pio2_1t */
    putd(ram, R2 - 12184u, 6.07710050630396597660e-11);  /* pio2_2 */
    putd(ram, R2 - 12176u, 2.02226624879595063154e-21);  /* pio2_2t */
    putd(ram, R2 - 12168u, 0.5);
    putd(ram, R2 - 12160u, 6.36619772367581382433e-01);  /* invpio2 */
    putd(ram, R2 - 12152u, 2.02226624871116645580e-21);  /* pio2_3 */
    putd(ram, R2 - 12144u, 8.47842766036889956997e-32);  /* pio2_3t */
    putd(ram, R2 - 12136u, 1.67772160000000000000e+07);  /* two24 */
    put64(ram, R2 - 12128u, magic);
    putd(ram, R2 - 12120u, 1.0);                         /* __kernel_cos */
    putd(ram, R2 - 12112u, 4.16666666666666019037e-02);
    putd(ram, R2 - 12104u, -1.38888888888741095749e-03);
    putd(ram, R2 - 12096u, 2.48015872894767294178e-05);
    putd(ram, R2 - 12088u, -2.75573143513906633035e-07);
    putd(ram, R2 - 12080u, 2.08757232129817482790e-09);
    putd(ram, R2 - 12072u, -1.13596475577881948265e-11);
    putd(ram, R2 - 12064u, 0.5);
    putd(ram, R2 - 12056u, 0.28125);
    putd(ram, R2 - 11984u, 8.33333333332248946124e-03);  /* __kernel_sin */
    putd(ram, R2 - 11976u, -1.98412698298579493134e-04);
    putd(ram, R2 - 11968u, 2.75573137070700676789e-06);
    putd(ram, R2 - 11960u, -2.50507602534068634195e-08);
    putd(ram, R2 - 11952u, 1.58969099521155010221e-10);
    putd(ram, R2 - 11944u, -1.66666666666666324348e-01);
    putd(ram, R2 - 11936u, 0.5);
    putd(ram, R2 - 11928u, 1.0);                         /* __kernel_tan */
    putd(ram, R2 - 11920u, -1.0);
    putd(ram, R2 - 11912u, 3.06161699786838301793e-17);  /* pio4lo */
    putd(ram, R2 - 11904u, 7.85398163397448278999e-01);  /* pio4 */
    putd(ram, R2 - 11896u, 0.0);
    putd(ram, R2 - 11888u, 2.0);
    put64(ram, R2 - 11880u, magic);
    putd(ram, R2 - 11832u, 0.0);                         /* cos */
    putd(ram, R2 - 11760u, 0.0);                         /* sin */
    putd(ram, R2 - 11752u, 0.0);                         /* tan */
    {
        static const f64 t[13] = {3.33333333333334091986e-01, 1.33333333333201242699e-01,
                                  5.39682539762260521377e-02, 2.18694882948595424599e-02,
                                  8.86323982359930005737e-03, 3.59207910759131235356e-03,
                                  1.45620945432529025516e-03, 5.88041240820264096874e-04,
                                  2.46463134818469906812e-04, 7.81794442939557092300e-05,
                                  7.14072491382608190305e-05, -1.85586374855275456654e-05,
                                  2.59073051863633712884e-05};
        for (u32 i = 0; i < 13u; ++i)
            putd(ram, 0x80371360u + 8u * i, t[i]);
        static const u32 npio2_hw[32] = {
            0x3FF921FB, 0x400921FB, 0x4012D97C, 0x401921FB, 0x401F6A7A, 0x4022D97C, 0x4025FDBB, 0x402921FB,
            0x402C463A, 0x402F6A7A, 0x4031475C, 0x4032D97C, 0x40346B9C, 0x4035FDBB, 0x40378FDB, 0x403921FB,
            0x403AB41B, 0x403C463A, 0x403DD85A, 0x403F6A7A, 0x40407E4C, 0x4041475C, 0x4042106C, 0x4042D97C,
            0x4043A28C, 0x40446B9C, 0x404534AC, 0x4045FDBB, 0x4046C6CB, 0x40478FDB, 0x404858EB, 0x404921FB};
        for (u32 i = 0; i < 32u; ++i)
            put32(ram, 0x80371290u + 4u * i, npio2_hw[i]);
    }
    putf(ram, R2 - 14976u, 0.0f);                    /* TOscillator */
    putf(ram, R2 - 14972u, 1.0f);
    putd(ram, R2 - 14960u, f64_value(magic));
    putd(ram, R2 - 14952u, f64_value(0x4330000000000000ull));
    putf(ram, R2 - 14968u, 80.0f);
    putf(ram, R2 - 14964u, 600.0f);
    putf(ram, R2 - 14944u, 32768.0f);
    putd(ram, R2 - 14936u, 0.0);
    putf(ram, R2 - 14928u, 16.0f);
    put32(ram, CVT_CONSTANTS, 0u), put32(ram, CVT_CONSTANTS + 4u, 0u);  /* __cvt_fp2unsigned */
    put32(ram, CVT_CONSTANTS + 8u, 0x41F00000u), put32(ram, CVT_CONSTANTS + 12u, 0u);
    put32(ram, CVT_CONSTANTS + 16u, 0x41E00000u), put32(ram, CVT_CONSTANTS + 20u, 0u);
    putf(ram, R2 - 15016u, 0.0f);                    /* TChannel::updateEffectorParam */
    putf(ram, R2 - 15012u, 0.5f);
    putf(ram, R2 - 15032u, 1.0f);
    putf(ram, R2 - 15008u, 4096.0f);
    putf(ram, R2 - 15004u, 127.5f);
    putd(ram, R2 - 15000u, f64_value(0x4330000000000000ull));
    putf(ram, R2 - 15472u, 256.0f);                  /* Calc::sinfT, sinfDolby2 */
    {
        /* updateMixer's switches: each part's source (1 pan, 2 effect, 3
         * surround, 5-7 one less each), 0 and 4 to the end of the switch. */
        static const u32 l0[8] = {0x8028CFB8u, 0x8028CF8Cu, 0x8028CF94u, 0x8028CF9Cu,
                                  0x8028CFB8u, 0x8028CFA4u, 0x8028CFACu, 0x8028CFB4u};
        static const u32 l1[8] = {0x8028D01Cu, 0x8028CFF0u, 0x8028CFF8u, 0x8028D000u,
                                  0x8028D01Cu, 0x8028D008u, 0x8028D010u, 0x8028D018u};
        for (u32 i = 0; i < 8u; ++i) {
            put32(ram, MIX_L0_TABLE + 4u * i, l0[i]);
            put32(ram, MIX_L1_TABLE + 4u * i, l1[i]);
        }
    }
    for (u32 c = 0; c < 3u; ++c) /* the oscillator's curves: 0 to 1, 17 points */
        for (u32 i = 0; i < 17u; ++i)
            putf(ram, OSC_CURVES + 68u * c + 4u * i,
                 c == 0u ? (f32)i / 16.0f : c == 1u ? (f32)(i * i) / 256.0f : (f32)sqrt((f64)i / 16.0));
    /* fmod's Zero[], libm's NaN and infinity, the arc tangent table. */
    putd(ram, FMOD_ZERO, 0.0);
    putd(ram, FMOD_ZERO + 8u, -0.0);
    put32(ram, LIBM_NAN, 0x7FFFFFFFu);
    put32(ram, LIBM_NAN + 4u, 0x7F800000u);
    for (u32 i = 0; i <= 1024u; ++i)
        put16(ram, ATAN_TABLE + 2u * i, (u16)(atan((f64)i / 1024.0) * 32768.0 / 3.14159265358979 + 0.5));
}

/* A vector of three singles at `at`: ordinary mostly. */
static void vector(u8* ram, u32 at, unsigned specials_in_64, f32 scale) {
    for (u32 i = 0; i < 3u; ++i) {
        if (!s_clean && below(64u) < specials_in_64)
            put32(ram, at + 4u * i, any_single(64u));
        else
            putf(ram, at + 4u * i, (f32)((s32)(next() % 20001u) - 10000) * scale);
    }
}

/* --- The cases. ----------------------------------------------------------- */

#define BGW (DATA + 0x0000u)   /* a cBgW (0x200) */
#define BGD (DATA + 0x0200u)   /* its cBgD_t (0x40) */
#define BLK (DATA + 0x0240u)   /* its blocks' first triangles (32 halves) */
#define VTX (DATA + 0x0300u)   /* its vertices (256 x 12) */
#define TRI (DATA + 0x1000u)   /* its triangles (400 x 10) */
#define VEC (DATA + 0x2000u)   /* vectors, objects, outputs (0x2000) */
#define INFL (DATA + 0x4000u)  /* point winds (8 x 44) */

static Case build(u8* ram, unsigned which, unsigned scenario) {
    Case k;
    memset(&k, 0, sizeof k);
    s_clean = (which == N_BLCK_BND || which == N_CALC_PLA || which == N_POLAR_VAL || which == N_PNTWIND) &&
              below(2u) == 0u;
    for (unsigned r = 0; r < sizeof k_regions / sizeof k_regions[0]; ++r)
        if (k_regions[r].start != TABLES || below(64u) == 0u)
            fill(ram, k_regions[r].start, k_regions[r].bytes);
    for (u32 i = 0; i < 0x8000u; i += 4u) /* r2's data: singles mostly */
        if (below(8u) != 0u)
            put32(ram, SDA2 + i, any_single(2u));
    constants(ram);
    CPUState* c = &k.cpu;
    random_cpu(c, ram);
    const Native* n = &k_natives[which];
    c->pc = n->entry;
    c->gpr[1] = STACK + 0x1000u + 8u * below(256u);
    c->gpr[2] = below(256u) == 0u ? (0x90000000u | (next() & 0x0FFFFFFFu)) : R2;
    c->gpr[13] = below(256u) == 0u ? (0x90000000u | (next() & 0x0FFFFFFFu)) : R13;
    /* FP arguments: singles mostly (the inline paths take them). */
    for (unsigned r = 1; r <= 8; ++r)
        if (below(16u) != 0u)
            c->fpr[r] = (f64)single_of(any_single(2u));
    switch (which) {
    case N_IEEE_FMOD:
    case N_FMOD:
        fmod_args(&c->fpr[1], &c->fpr[2]);
        break;
    case N_RAD2S:
        switch (below(8u)) {
        case 0: c->fpr[1] = (f64)single_of(any_single(24u)); break;
        case 1: c->fpr[1] = (f64)(f32)((f32)((s32)next() % 2000000) * 0.37f); break;
        default: c->fpr[1] = (f64)(f32)((f32)((s32)next() % 20000) * 0.001f); break;
        }
        break;
    case N_RND:
    case N_RNDF:
    case N_RNDFX:
        for (u32 i = 0; i < 3u; ++i)
            put32(ram, R13 - 28072u + 4u * i, below(16u) == 0u ? next() : 1u + below(30268u));
        if (below(4u) != 0u)
            c->fpr[1] = (f64)(f32)((f32)(next() % 100000u) * 0.01f);
        break;
    case N_SIN:
    case N_COS:
    case N_TAN: {
        /* An angle: a float's mostly, within a turn or a few; sometimes far
         * (rem_pio2's medium path, or past it: __kernel_rem_pio2, declined),
         * tiny, or special. */
        const u32 kind = below(16u);
        if (kind < 9u)
            c->fpr[1] = (f64)(f32)((f32)((s32)(next() % 2000001u) - 1000000) * 0.00001f);
        else if (kind < 11u)
            c->fpr[1] = (f64)(f32)((f32)((s32)next() % 10000000) * 0.37f);
        else if (kind < 12u)
            c->fpr[1] = f64_value(((u64)(next() & 1u) << 63) | ((u64)(1023u - 60u + below(120u)) << 52) |
                                  ((((u64)next() << 32) | next()) & 0x000FFFFFFFFFFFFFull));
        else if (kind < 13u) /* near a multiple of pi/2 */
            c->fpr[1] = 1.5707963267948966 * (f64)(s32)(below(64u) - 32u) + (f64)(s32)(below(64u) - 32u) * 1e-15;
        else
            c->fpr[1] = f64_value(any_double());
        break;
    }
    case N_ATAN2S:
        for (unsigned r = 1; r <= 2u; ++r) {
            const u32 kind = below(16u);
            c->fpr[r] = kind == 0u   ? 0.0
                        : kind == 1u ? (f64)single_of(any_single(64u))
                        : kind == 2u ? (f64)((f32)((s32)below(200u) - 100) * 1e-7f)
                                     : (f64)((f32)((s32)(next() % 20001u) - 10000) * 0.05f);
        }
        if (below(16u) == 0u) /* the same magnitudes */
            c->fpr[2] = below(2u) ? c->fpr[1] : -c->fpr[1];
        break;
    case N_BLCK_MIN_MAX:
    case N_BLCK_BND: {
        /* The background: its vertices, triangles and blocks. */
        const u32 vertices = 1u + below(256u);
        put32(ram, BGW + 144u, below(64u) == 0u ? pointer(VTX, 0xC00u, 4u) : VTX);
        put32(ram, BGW + 148u, below(64u) == 0u ? pointer(BGD, 0x40u, 4u) : BGD);
        put8(ram, BGW + 109u, (u8)(below(4u) == 0u ? 0u : below(4u) == 0u ? next() : 1u));
        vector(ram, BGW + 124u, 1u, 0.01f); /* mTransVel */
        for (u32 v = 0; v < 256u; ++v)
            vector(ram, VTX + 12u * v, 1u, 0.5f);
        const u32 blocks = 1u + below(24u);
        u32 tri = 0;
        for (u32 b = 0; b < 32u; ++b) {
            put16(ram, BLK + 2u * b, (u16)tri);
            tri += below(8u) == 0u ? below(40u) : below(12u);
            if (tri > 399u)
                tri = 399u;
        }
        put32(ram, BGD + 8u, below(32u) == 0u ? below(400u) : tri);  /* m_t_num */
        put32(ram, BGD + 12u, TRI);                                   /* m_t_tbl */
        put32(ram, BGD + 16u, blocks);                                /* m_b_num */
        put32(ram, BGD + 20u, BLK);                                   /* m_b_tbl */
        for (u32 t = 0; t < 400u; ++t)
            for (u32 j = 0; j < 3u; ++j)
                put16(ram, TRI + 10u * t + 2u * j, (u16)(!s_clean && below(512u) == 0u ? next() : below(vertices)));
        c->gpr[3] = !s_clean && below(64u) == 0u ? pointer(BGW, 0x100u, 4u) : BGW;
        c->gpr[4] = which == N_BLCK_MIN_MAX ? (below(32u) == 0u ? next() : below(vertices))
                                            : (below(32u) == 0u ? below(32u) : below(blocks));
        /* The bounds: in VEC, sometimes the same, sometimes over a vertex. */
        c->gpr[5] = pointer(VEC, 0x100u, 4u);
        c->gpr[6] = below(16u) == 0u ? c->gpr[5] : below(16u) == 0u ? VTX + 4u * below(64u) : pointer(VEC + 0x100u, 0x100u, 4u);
        if ((c->gpr[5] & ~3u) - DATA < 0x7FF0u)
            vector(ram, c->gpr[5] & ~3u, 2u, 0.5f);
        if ((c->gpr[6] & ~3u) - DATA < 0x7FF0u)
            vector(ram, c->gpr[6] & ~3u, 2u, 0.5f);
        break;
    }
    case N_OSC_OFFSET:
    case N_OSC_CALC: {
        /* An oscillator: its data (rate, tables, scale, offset), its state
         * (0-6), envelope index, release rate, phases; an envelope table of
         * mode/time/value triples (curves 0-2 mostly, a jump, a hold, an end). */
        const u32 osc = VEC + 0x100u, data = VEC + 0x200u, table = VEC + 0x300u;
        c->gpr[3] = !s_clean && below(64u) == 0u ? pointer(osc, 4u, 4u) : osc;
        put32(ram, osc, below(32u) == 0u ? 0u : below(64u) == 0u ? pointer(data, 4u, 4u) : data);
        put8(ram, osc + 4u, (u8)(below(16u) == 0u ? next() : below(7u)));
        put8(ram, osc + 5u, (u8)below(4u));
        put16(ram, osc + 6u, (u16)(below(32u) == 0u ? next() : below(6u)));
        putf(ram, osc + 8u, below(4u) == 0u ? -(f32)below(100u) * 0.01f : (f32)below(1000u) * 0.1f); /* release rate */
        for (u32 i = 12u; i < 32u; i += 4u)
            putf(ram, osc + i, below(32u) == 0u ? single_of(any_single(32u)) : (f32)((s32)below(2000u) - 1000) * 0.001f);
        if (below(4u) == 0u)
            putf(ram, osc + 28u, 0.0f);
        putf(ram, data + 4u, (f32)below(100u) * 0.1f);
        put32(ram, data + 8u, below(16u) == 0u ? 0u : table);
        put32(ram, data + 12u, below(16u) == 0u ? 0u : table + 6u * below(4u));
        putf(ram, data + 16u, below(8u) == 0u ? 0.0f : (f32)((s32)below(2000u) - 1000) * 0.001f);
        putf(ram, data + 20u, (f32)((s32)below(2000u) - 1000) * 0.001f);
        for (u32 i = 0; i < 16u; ++i) {
            const u32 kind = below(16u);
            const s16 mode = kind < 12u ? (s16)below(3u) : kind == 12u ? 13 : kind == 13u ? 14 : kind == 14u ? 15
                                                                                                           : (s16)next();
            put16(ram, table + 6u * i, (u16)mode);
            put16(ram, table + 6u * i + 2u, (u16)(below(4u) == 0u ? 0u : below(4u) == 0u ? next() : below(200u)));
            put16(ram, table + 6u * i + 4u, (u16)(mode == 13 ? below(16u) : (u32)((s32)next() % 32768)));
        }
        put32(ram, R13 - 31912u, (u32)(below(8u) == 0u ? next() : 1u + below(4u)) << 24); /* the update interval */
        putf(ram, R13 - 31992u, below(16u) == 0u ? single_of(any_single(32u)) : 32000.0f);   /* the DAC rate */
        c->gpr[4] = table;
        break;
    }
    case N_EFFECTOR: {
        /* A channel: its manager (the sends' source when it is the channel's
         * own), its DSP channel's buffer (the auto mixer's), its gains and
         * sends (0 to 1 mostly), its calc types (indices into calc_sw_table,
         * whose entries are 0-3), its mixer configurations (all 0xFFFF for
         * the auto mixer sometimes; else a part's sources 0-7, a word left
         * zero); the output mode (0-2 mostly), the levels, the sine and
         * surround tables (257 floats each). */
        const u32 ch = VEC + 0x800u, mgr = VEC + 0x900u, dsp = VEC + 0xA00u, buf = VEC + 0xA40u;
        const u32 sine = VEC + 0xB00u, dolby = VEC + 0xF10u;
        c->gpr[3] = !s_clean && below(64u) == 0u ? pointer(ch, 4u, 4u) : ch;
        put32(ram, ch + 4u, !s_clean && below(64u) == 0u ? pointer(mgr, 4u, 4u) : mgr);
        put32(ram, ch + 164u, below(2u) ? mgr : next());
        put32(ram, ch + 32u, dsp);
        put32(ram, dsp + 12u, !s_clean && below(64u) == 0u ? pointer(buf, 4u, 4u) : buf);
        static const u32 gains[] = {88u, 92u, 148u, 152u, 168u, 172u};
        for (u32 i = 0; i < 6u; ++i)
            putf(ram, ch + gains[i], !s_clean && below(32u) == 0u ? single_of(any_single(32u)) : (f32)below(2001u) * 0.001f);
        for (u32 i = 0; i < 3u; ++i) { /* calc types: 0-27 (past them, the jump tables' bytes) */
            put8(ram, ch + 96u + i, (u8)(below(32u) == 0u ? below(40u) : below(8u)));
            put8(ram, mgr + 98u + i, (u8)(below(32u) == 0u ? below(40u) : below(8u)));
        }
        for (u32 i = 0; i < 12u; ++i)
            putf(ram, ch + 100u + 4u * i, !s_clean && below(32u) == 0u ? single_of(any_single(32u))
                                                                       : (f32)below(1001u) * 0.001f);
        for (u32 i = 24u; i <= 40u; i += 4u)
            putf(ram, mgr + i, (f32)below(1001u) * 0.001f);
        const bool automix = below(4u) == 0u;
        for (u32 i = 0; i < 6u; ++i) {
            const u32 kind = below(8u);
            const u16 config = automix && i < 2u ? 0xFFFFu
                               : kind == 0u ? 0u
                               : kind == 1u ? (u16)next()
                                            : (u16)(((1u + below(255u)) << 8) | (below(8u) << 4) | below(8u));
            put16(ram, ch + 176u + 2u * i, config);
        }
        for (u32 i = 0; i < 84u; ++i) /* its 28 entries (updateMixer's jump tables follow) */
            put8(ram, CALC_SW_TABLE + i, (u8)(below(16u) == 0u ? next() : below(4u)));
        if (!s_clean && below(32u) == 0u) /* a jump table's entry anywhere */
            put32(ram, (below(2u) ? MIX_L0_TABLE : MIX_L1_TABLE) + 4u * below(8u), below(2u) ? next() : 0x8028CF8Cu);
        put32(ram, R13 - 31916u, below(16u) == 0u ? next() : below(3u));        /* the output mode */
        put16(ram, R13 - 31920u, (u16)(below(8u) == 0u ? next() : 0x7FFFu));    /* the channel level */
        put16(ram, R13 - 31918u, (u16)(below(8u) == 0u ? next() : 0x7FFFu));    /* the auto mixer's */
        put32(ram, R13 - 27880u, sine);
        put32(ram, R13 - 27876u, dolby);
        for (u32 i = 0; i < 257u; ++i) {
            putf(ram, sine + 4u * i, (f32)sin((f64)i / 256.0 * 1.5707963267948966));
            putf(ram, dolby + 4u * i, (f32)sqrt((f64)i / 256.0));
        }
        break;
    }
    case N_EULER_TO_QUAT: {
        /* The sine table (JMANewSinTable(12)), its pointers and shift. */
        if (below(64u) != 0u)
            for (u32 i = 0; i < 5120u; ++i)
                putf(ram, TABLES + 4u * i, (f32)sin((3.14159265358979 * 2.0f / 4096.0) * i));
        put32(ram, R13 - 26456u, below(64u) == 0u ? pointer(TABLES, 0x4000u, 4u) : TABLES);
        put32(ram, R13 - 26452u, below(64u) == 0u ? pointer(TABLES, 0x4000u, 4u) : TABLES + 4096u);
        put32(ram, R13 - 26460u, below(16u) == 0u ? below(40u) : 4u);
        c->gpr[6] = pointer(VEC, 0x1000u, 4u);
        break;
    }
    case N_CALC_PLA: {
        /* Three points of a triangle (sometimes in a line, or the same),
         * the normal and the distance's outputs. */
        for (u32 i = 0; i < 3u; ++i)
            c->gpr[3 + i] = pointer(VEC + 0x40u * i, 0x30u, 4u);
        for (u32 i = 0; i < 3u; ++i)
            if ((c->gpr[3 + i] & ~3u) - VEC < 0x1000u)
                vector(ram, c->gpr[3 + i] & ~3u, 2u, 0.25f);
        if (below(16u) == 0u && (c->gpr[3] & ~3u) - VEC < 0x1000u && (c->gpr[4] & ~3u) - VEC < 0x1000u)
            for (u32 i = 0; i < 12u; i += 4u)
                put32(ram, (c->gpr[4] & ~3u) + i, get32(ram, (c->gpr[3] & ~3u) + i));
        c->gpr[6] = below(16u) == 0u ? c->gpr[3] : pointer(VEC + 0x200u, 0x40u, 4u);
        c->gpr[7] = below(16u) == 0u ? c->gpr[6] + 4u : pointer(VEC + 0x300u, 0x40u, 4u);
        break;
    }
    case N_POLAR_VAL:
        c->gpr[3] = pointer(VEC, 0x100u, 4u);
        c->gpr[4] = below(16u) == 0u ? c->gpr[3] : pointer(VEC + 0x200u, 0x100u, 4u);
        if ((c->gpr[4] & ~3u) - VEC < 0x1000u) {
            vector(ram, c->gpr[4] & ~3u, 3u, below(2u) ? 0.25f : 0.0001f);
            if (below(8u) == 0u) /* on an axis */
                for (u32 i = 0; i < 3u; ++i)
                    if (below(2u))
                        put32(ram, (c->gpr[4] & ~3u) + 4u * i, below(2u) ? 0u : 0x80000000u);
        }
        break;
    case N_PNTWIND: {
        /* The point winds: none mostly, or one to three. */
        for (u32 i = 0; i < 30u; ++i)
            put32(ram, WIND + 4u * i, 0u);
        const u32 count = below(4u) == 0u ? 1u + below(3u) : 0u;
        c->gpr[3] = pointer(VEC, 0x100u, 4u);
        if ((c->gpr[3] & ~3u) - VEC < 0x1000u)
            vector(ram, c->gpr[3] & ~3u, 1u, 0.5f);
        for (u32 i = 0; i < count; ++i) {
            const u32 at = INFL + 48u * below(8u);
            put32(ram, WIND + 4u * below(30u), below(32u) == 0u ? pointer(INFL, 0x180u, 4u) : at);
            if ((c->gpr[3] & ~3u) - VEC < 0x1000u && below(2u))
                for (u32 j = 0; j < 12u; j += 4u) /* near the position */
                    putf(ram, at + j, single_of(get32(ram, (c->gpr[3] & ~3u) + j)) + (f32)((s32)below(200u) - 100));
            else
                vector(ram, at, 1u, 0.5f);
            vector(ram, at + 12u, 1u, 0.0001f);  /* mDir */
            putf(ram, at + 24u, below(16u) == 0u ? single_of(any_single(16u)) : (f32)(below(3000u)));  /* mRadius */
            putf(ram, at + 28u, below(16u) == 0u ? single_of(any_single(16u)) : (f32)below(100u) * 0.01f); /* mStrength */
            put8(ram, at + 40u, (u8)below(2u));  /* mbConstant */
        }
        c->gpr[4] = below(16u) == 0u ? c->gpr[3] : pointer(VEC + 0x200u, 0x100u, 4u);
        c->gpr[5] = below(16u) == 0u ? c->gpr[4] + 8u : pointer(VEC + 0x300u, 0x100u, 4u);
        break;
    }
    default:
        break;
    }
    /* A reservation, sometimes on a granule the function stores to. */
    if (c->reserve_valid) {
        switch (below(4u)) {
        case 0: c->reserve_addr = c->gpr[1] - 8u * below(16u); break;
        case 1: c->reserve_addr = VEC + 4u * below(0x400u); break;
        case 2: c->reserve_addr = R13 - 28072u + 4u * below(3u); break;
        default: break;
        }
        if (below(4u) == 0u)
            c->reserve_addr |= 0x40000000u;
    }
    clock_edges(c, scenario, n->cycles);
    k.journal = scenario % 41u == 7u;
    k.aliases = scenario % 47u == 8u;
    k.flush = below(4u) == 0u;
    k.must_decline = k.journal || k.aliases || c->exception != 0u;
    return k;
}

/* Host scenarios: busy (the module asks its edge service at every boundary
 * too), the filter off or the boundaries watched (the natives' own view only). */
enum { HOST_QUIET, HOST_BUSY, HOST_FILTER_OFF, HOST_WATCHED };

static unsigned host_scenario(unsigned scenario) {
    switch (scenario % 37u) {
    case 5: return HOST_BUSY;
    case 11: return HOST_FILTER_OFF;
    case 17: return HOST_WATCHED;
    default: return HOST_QUIET;
    }
}

static void set_host(unsigned host) {
    reset_host();
    if (host == HOST_BUSY)
        s_sources_dirty = true;
    if (host == HOST_FILTER_OFF)
        bw_edge_filter_enabled = false;
    if (host == HOST_WATCHED) {
        static const u32 boundaries[] = {0x80330E34u, 0x80246058u, 0x80246394u, 0x8030DCE0u, 0x80247C24u,
                                         0x80247C34u, 0x8030DD04u, 0x8030DD28u, 0x8030DE68u, 0x8030DEACu,
                                         0x8030DECCu, 0x8024A724u, 0x8024A734u, 0x8024A744u, 0x8024A74Cu,
                                         0x8024A778u, 0x8024A784u, 0x80246270u, 0x802543B0u, 0x802543D8u,
                                         0x8030E0B4u, 0x8008A290u, 0x8008A3B4u, 0x8027A9C8u, 0x8027A9F4u,
                                         0x8028A740u, 0x8028AAC4u, 0x8028AACCu, 0x8028AADCu, 0x8028CF8Cu,
                                         0x8028CFF0u, 0x8028AAE4u, 0x80328E10u};
        for (unsigned i = 0; i < sizeof boundaries / sizeof boundaries[0]; ++i)
            if (below(2u))
                watch(boundaries[i]);
    }
}

static const char* s_only;
static bool skip(const Native* n) {
    if (s_only == NULL)
        return false;
    for (const char* p = s_only; *p != 0;) { /* name prefixes, comma-separated */
        const char* end = strchr(p, ',');
        const size_t length = end != NULL ? (size_t)(end - p) : strlen(p);
        if (length != 0u && strncmp(n->name, p, length) == 0)
            return false;
        p += length + (end != NULL);
    }
    return true;
}

/* --- The microbenchmark: ordinary inputs, the budget large. -------------- */

/* A benchmark case: the scenario's case, quiet, with the inputs play has. */
static void bench_inputs(u8* ram, unsigned which, CPUState* c, unsigned variant) {
    switch (which) {
    case N_IEEE_FMOD:
    case N_FMOD:
        c->fpr[1] = variant ? 1234.5678 : 2.3456789;
        c->fpr[2] = variant ? 6.2831854820251465 : 1.0;
        break;
    case N_RAD2S:
        c->fpr[1] = 3.75;
        break;
    case N_RND:
    case N_RNDF:
    case N_RNDFX:
        put32(ram, R13 - 28072u, 1234u);
        put32(ram, R13 - 28068u, 5678u);
        put32(ram, R13 - 28064u, 9012u);
        c->fpr[1] = 10.0;
        break;
    case N_SIN:
    case N_COS:
    case N_TAN:
        c->fpr[1] = variant ? 1234.5 : 0.75;
        break;
    case N_ATAN2S:
        c->fpr[1] = 3.5;
        c->fpr[2] = -7.25;
        break;
    case N_OSC_OFFSET:
    case N_OSC_CALC:
        c->gpr[3] = VEC + 0x100u;
        c->gpr[4] = VEC + 0x300u;
        break;
    case N_EFFECTOR: {
        const u32 ch = VEC + 0x800u, mgr = VEC + 0x900u;
        c->gpr[3] = ch;
        put32(ram, ch + 4u, mgr), put32(ram, ch + 164u, 0u);
        static const u32 gains[] = {88u, 92u, 148u, 152u, 168u, 172u};
        for (u32 i = 0; i < 6u; ++i)
            putf(ram, ch + gains[i], 0.75f);
        put8(ram, ch + 96u, 1u), put8(ram, ch + 97u, 2u), put8(ram, ch + 98u, 2u);
        for (u32 i = 0; i < 12u; ++i)
            putf(ram, ch + 100u + 4u * i, 0.25f + 0.05f * (f32)i);
        put16(ram, ch + 176u, 0x0112u), put16(ram, ch + 178u, 0x0151u), put16(ram, ch + 180u, 0x0120u);
        put16(ram, ch + 182u, 0u), put16(ram, ch + 184u, 0u), put16(ram, ch + 186u, 0u);
        for (u32 i = 0; i < 4u; ++i) {
            put8(ram, CALC_SW_TABLE + 3u * i, (u8)(i & 1u ? 3u : 1u));
            put8(ram, CALC_SW_TABLE + 3u * i + 1u, 1u);
            put8(ram, CALC_SW_TABLE + 3u * i + 2u, 0u);
        }
        put32(ram, R13 - 31916u, 1u);
        put16(ram, R13 - 31920u, 0x7FFFu), put16(ram, R13 - 31918u, 0x7FFFu);
        break;
    }
    case N_BLCK_MIN_MAX:
        c->gpr[3] = BGW;
        c->gpr[4] = 7u;
        c->gpr[5] = VEC;
        c->gpr[6] = VEC + 0x100u;
        break;
    case N_BLCK_BND:
        c->gpr[3] = BGW;
        put8(ram, BGW + 109u, 1u);
        put32(ram, BGD + 16u, 8u);
        c->gpr[4] = 3u;
        put16(ram, BLK + 6u, 40u);
        put16(ram, BLK + 8u, 40u + (variant ? 30u : 6u)); /* six (or thirty) triangles */
        c->gpr[5] = VEC;
        c->gpr[6] = VEC + 0x100u;
        break;
    case N_EULER_TO_QUAT:
        for (u32 i = 0; i < 5120u; ++i)
            putf(ram, TABLES + 4u * i, (f32)sin((3.14159265358979 * 2.0f / 4096.0) * i));
        put32(ram, R13 - 26456u, TABLES);
        put32(ram, R13 - 26452u, TABLES + 4096u);
        put32(ram, R13 - 26460u, 4u);
        c->gpr[3] = 0x1234u;
        c->gpr[4] = 0xC567u;
        c->gpr[5] = 0x0890u;
        c->gpr[6] = VEC;
        break;
    case N_CALC_PLA:
        c->gpr[3] = VEC;
        c->gpr[4] = VEC + 0x40u;
        c->gpr[5] = VEC + 0x80u;
        c->gpr[6] = VEC + 0x200u;
        c->gpr[7] = VEC + 0x300u;
        putf(ram, VEC, 1.0f), putf(ram, VEC + 4u, 2.0f), putf(ram, VEC + 8u, 3.0f);
        putf(ram, VEC + 0x40u, 4.0f), putf(ram, VEC + 0x44u, 2.5f), putf(ram, VEC + 0x48u, -1.0f);
        putf(ram, VEC + 0x80u, 0.5f), putf(ram, VEC + 0x84u, 7.0f), putf(ram, VEC + 0x88u, 2.0f);
        break;
    case N_POLAR_VAL:
        c->gpr[3] = VEC;
        c->gpr[4] = VEC + 0x200u;
        putf(ram, VEC + 0x200u, 120.5f), putf(ram, VEC + 0x204u, -33.25f), putf(ram, VEC + 0x208u, 470.0f);
        break;
    case N_PNTWIND:
        for (u32 i = 0; i < 30u; ++i)
            put32(ram, WIND + 4u * i, 0u);
        if (variant) {
            put32(ram, WIND + 8u, INFL);
            putf(ram, INFL, 100.0f), putf(ram, INFL + 4u, 20.0f), putf(ram, INFL + 8u, -50.0f);
            putf(ram, INFL + 12u, 0.0f), putf(ram, INFL + 16u, 1.0f), putf(ram, INFL + 20u, 0.0f);
            putf(ram, INFL + 24u, 500.0f), putf(ram, INFL + 28u, 0.5f);
            put8(ram, INFL + 40u, 0u);
        }
        c->gpr[3] = VEC;
        c->gpr[4] = VEC + 0x200u;
        c->gpr[5] = VEC + 0x300u;
        putf(ram, VEC, 110.0f), putf(ram, VEC + 4u, 30.0f), putf(ram, VEC + 8u, -20.0f);
        break;
    default:
        break;
    }
}

/* Before each benchmark call, on both sides: the bounds MakeBlckMinMax
 * widens (which would otherwise stop changing after the first call). */
static void bench_reset(u8* ram, u32 entry) {
    switch (entry) {
    case 0x80247C4Cu:
        putf(ram, VEC, 1e9f), putf(ram, VEC + 4u, 1e9f), putf(ram, VEC + 8u, 1e9f);
        putf(ram, VEC + 0x100u, -1e9f), putf(ram, VEC + 0x104u, -1e9f), putf(ram, VEC + 0x108u, -1e9f);
        break;
    case 0x8028DF2Cu: /* an oscillator part way through a curve segment */
    case 0x8028E238u: {
        const u32 osc = VEC + 0x100u, data = VEC + 0x200u, table = VEC + 0x300u;
        put32(ram, osc, data);
        put8(ram, osc + 4u, 2u), put8(ram, osc + 5u, 1u), put16(ram, osc + 6u, 1u);
        putf(ram, osc + 8u, 20.0f), putf(ram, osc + 12u, 0.25f), putf(ram, osc + 16u, 0.75f);
        putf(ram, osc + 20u, 0.0125f), putf(ram, osc + 24u, 0.0f), putf(ram, osc + 28u, 40.0f);
        putf(ram, data + 4u, 1.0f), put32(ram, data + 8u, table), put32(ram, data + 12u, table);
        putf(ram, data + 16u, 1.0f), putf(ram, data + 20u, 0.0f);
        for (u32 i = 0; i < 4u; ++i)
            put16(ram, table + 6u * i, 1u), put16(ram, table + 6u * i + 2u, 40u), put16(ram, table + 6u * i + 4u, 16000u);
        break;
    }
    default:
        break;
    }
}

static unsigned bench_variants(unsigned which) {
    return which == N_IEEE_FMOD || which == N_FMOD || which == N_BLCK_BND || which == N_PNTWIND || which == N_SIN ||
                   which == N_COS || which == N_TAN
               ? 2u
               : 1u;
}

static void bench(Harness* h, unsigned calls) {
    reset_host();
    u8* ram = h->native_ram;
    for (unsigned which = 0; which < NATIVE_COUNT; ++which) {
        const Native* n = &k_natives[which];
        if (skip(n))
            continue;
        for (unsigned variant = 0; variant < bench_variants(which); ++variant) {
            Case k;
            unsigned tries = 0;
            do {
                k = build(ram, which, 6u * (tries + 1u) + 1000u);
                constants(ram);
                CPUState* c = &k.cpu;
                c->exception = 0;
                c->downcount = 0;
                c->cycle_budget = (s64)1 << 40;
                c->cycle_deadline_budget = 0;
                c->reserve_valid = false;
                c->msr |= PPC_MSR_FP;
                c->fpscr &= ~0x7u;
                c->gpr[1] = STACK + 0x1000u;
                c->gpr[2] = R2;
                c->gpr[13] = R13;
                c->lr = RETURN_ADDRESS;
                bench_inputs(ram, which, c, variant);
                CPUState probe = k.cpu;
                copy_regions(h->before, ram);
                const int ran = n->fn(&probe, n->entry);
                copy_regions(ram, h->before);
                if (ran)
                    break;
            } while (++tries < 1000u);
            if (tries == 1000u) {
                printf("%08X %s: no benchmark case\n", n->entry, n->name);
                continue;
            }
            const CPUState base = k.cpu;
            copy_regions(h->before, ram);
            double best_t = 1e30, best_n = 1e30;
            for (unsigned round = 0; round < 5u; ++round) {
                copy_regions(h->reference_ram, h->before);
                CPUState* g = h->guest_cpu();
                *g = base;
                g->ram = h->reference_ram;
                h->mod->on_state_loaded(g);
                double t0 = now_ns();
                for (unsigned i = 0; i < calls; ++i) {
                    memcpy(g->gpr, base.gpr, sizeof base.gpr);
                    memcpy(g->fpr, base.fpr, 9u * sizeof base.fpr[0]);
                    g->lr = RETURN_ADDRESS;
                    g->pc = n->entry;
                    bench_reset(h->reference_ram, n->entry);
                    h->mod->dispatch(g, n->entry);
                }
                const double t = (now_ns() - t0) / calls;
                CPUState native = base;
                t0 = now_ns();
                for (unsigned i = 0; i < calls; ++i) {
                    memcpy(native.gpr, base.gpr, sizeof base.gpr);
                    memcpy(native.fpr, base.fpr, 9u * sizeof base.fpr[0]);
                    native.lr = RETURN_ADDRESS;
                    native.pc = n->entry;
                    bench_reset(ram, n->entry);
                    if (!n->fn(&native, n->entry)) {
                        printf("%08X %s: the benchmark case declined\n", n->entry, n->name);
                        break;
                    }
                }
                const double d = (now_ns() - t0) / calls;
                if (t < best_t)
                    best_t = t;
                if (d < best_n)
                    best_n = d;
            }
            copy_regions(ram, h->before);
            printf("%08X %s%s: translation %.1f ns/call through the dispatcher, native %.1f ns/call (%.2fx)\n",
                   n->entry, n->name, variant ? " (variant)" : "", best_t, best_n, best_t / best_n);
#ifdef NATIVE7_HOOKED
            hooked_bench(ram, n->entry, &base, calls, n->name, variant);
#endif
            fflush(stdout);
        }
    }
}

int main(int argc, char** argv) {
    bluewake_native_gate_enabled = 0; /* every call tried */
    if (argc < 2) {
        fprintf(stderr, "usage: native_leaf_test MODULE.dll [CASES_PER_FUNCTION] [BENCH_CALLS] [NAME_PREFIX,...]\n");
        return 2;
    }
    const unsigned cases = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 60000u;
    const unsigned bench_calls = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 1000000u;
    s_only = argc > 4 ? argv[4] : NULL;
    Harness h;
    if (!harness_load(&h, argv[1], false, k_regions, sizeof k_regions / sizeof k_regions[0]))
        return 1;
    int failed = 0;
    for (unsigned which = 0; which < NATIVE_COUNT; ++which) {
        const Native* n = &k_natives[which];
        if (skip(n))
            continue;
        unsigned ran = 0, declined = 0;
        for (unsigned i = 0; i < cases; ++i) {
            Case k = build(h.native_ram, which, i);
            const unsigned host = host_scenario(i);
            set_host(host);
            const bool must = k.must_decline || (n->boundaries && host != HOST_QUIET && host != HOST_WATCHED);
            const int r = harness_case(&h, n->fn, n->entry, n->name, i, &k.cpu, must, k.journal, k.aliases, k.flush);
            if (r && (host == HOST_FILTER_OFF || host == HOST_WATCHED)) {
                /* It ran with the filter off (its path must have crossed no
                 * boundary) or with boundaries watched (it must have crossed
                 * none of those). With the host busy, the translation asks its
                 * edge service at the first boundary it crosses. */
                s_sources_dirty = true;
                (void)harness_translate(&h, &k.cpu, n->entry);
                s_sources_dirty = false;
                if (s_unexpected_service != 0u &&
                    (host == HOST_FILTER_OFF || !bw_edge_unwatched(s_unexpected_at))) {
                    fprintf(stderr, "case %u (%s): ran across a boundary (%08X) with the filter off or it watched\n",
                            i, n->name, s_unexpected_at);
                    return 1;
                }
            }
            ran += (unsigned)r;
            declined += (unsigned)(r == 0);
        }
        reset_host();
        printf("%08X %s: %u cases, %u identical, %u declined unchanged, 0 mismatches\n", n->entry, n->name, cases,
               ran, declined);
        fflush(stdout);
        if (ran < 30000u && cases >= 60000u) {
            printf("  fewer than 30,000 compared cases\n");
            failed = 1;
        }
    }
#ifdef NATIVE7_HOOKED
    hooked_summary();
#endif
    bluewake_native_libm_report();
    bluewake_native_bgblk_report();
    bluewake_native_rot_report();
    bluewake_native_calc_report();
    bluewake_native_geom_report();
    bluewake_native_jas_report();
    if (bench_calls != 0u)
        bench(&h, bench_calls);
    return failed;
}
