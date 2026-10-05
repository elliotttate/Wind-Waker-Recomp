/* cmake/composite/native_draw.c (the sixth set: the particle draw code and the
 * sea's waves, with stops) against the translations it stands in for, end to
 * end through the hooked chunks.
 *
 * Build and run from the worktree root (x64; the Visual Studio environment);
 * %TREE% is a copy of the builder's composite-src chunks this test links
 * (TEST_CHUNKS below) with generated.h and bw_edge_watch.inc, hooked by
 * scripts/windows/native_entries.py %TREE%:
 *
 *   for each chunk in TEST_CHUNKS:
 *     clang -c -O2 -march=x86-64-v3 -ffp-contract=off -fno-slp-vectorize
 *       -mllvm -large-interval-freq-threshold=10
 *       -DMODULE_GAME_ID=\"GZLE01\" -DDOLRECOMP_CPU_HEADER=\"core/cpu.h\"
 *       -DBW_GUEST_MEM1=bw_guest_mem1 -DBW_GUEST_MEM1_SIZE=0x02000000u
 *       -DBLUEWAKE_EDGE_FILTER=1 -DBLUEWAKE_GATHER_PIPE_BATCH=1
 *       -Icmake/composite -I%TREE% -I...GXRuntime\include -I...StaticRecomp
 *       %TREE%\chunks_dol\chunk_NNNN.c -o chunk_NNNN.o
 *   clang -O2 -march=x86-64-v3 -ffp-contract=off -DNATIVE5_HOOKED=1
 *     -DBW_GUEST_MEM1=bw_guest_mem1 -DBW_GUEST_MEM1_SIZE=0x02000000u
 *     -DDRAW_WATCH_LIST=\"%TREE%/bw_edge_watch.inc\"
 *     -Icmake/composite -Itests -I...GXRuntime\include -I...StaticRecomp
 *     tests/native_draw_test.c cmake/composite/native_draw.c cmake/composite/native_gx.c
 *     cmake/composite/direct_calls.c cmake/composite/gather_pipe.c cmake/composite/guest_cpu.c
 *     chunk_*.o ...\gxruntime.lib -o native_draw_test.exe
 *   native_draw_test MODULE.dll [CASES_PER_FUNCTION=60000] [BENCH_CALLS=200000] [NAME_PREFIX,...]
 *
 * MODULE.dll is a Windows game module (gGZLE01_recomp.dll) built from the
 * same composite-src; the test reads it and writes nothing but its own memory.
 *
 * Each case: random registers, flags, FPSCR, reservation and cycle state
 * (deadlines and budgets near and inside the work); a heap of 256 objects of
 * 256 bytes whose first word is a vtable (one table, its slots the functions
 * the natives call through vtables - JPABaseShapeArc's and JPAExtraShapeArc's
 * getters, the calc visitors - and sometimes other code) and whose other
 * words are pointers to objects (linked lists ending in NULL), NULL, ordinary
 * singles, small integers or anything; the draw clipboard's function
 * pointers the functions the game puts there, sometimes not; __GXData as the
 * fifth set's test sets it up; r2's and r13's small data; MEM1's first page
 * (where the module's translation puts a store to a small address: a NULL
 * pointer's field); a write journal, aliases over MEM1, an exception pending;
 * the host quiet, busy, the edge filter off or the natives' boundaries
 * watched; the gather pipe batched (its flush point inside the run's bytes
 * or not), a word at a time, or unset. drawWave's cases start at its loop's
 * head with the waves, the frame and the statics it reads laid out (see
 * wave_case), fdlibm's constants where sin's code loads them mostly.
 *
 * Plain: the native alone from the function's entry; where it runs to the
 * blr, every byte of the CPU state, the writable pages and the pipe's stream
 * against the module's translation; where it declines, nothing changed;
 * where it stops, counted (the hooked run checks it). Hooked: every case
 * also run through the hooked chunks from the entry - the natives on (the
 * hooks at entries and resumes, stops going on with the translation), then
 * off - each against the module's translation, byte for byte. */
#include "native_draw.h"
#include "native_gx.h"
#include "native5_harness.h"

#define STACK 0x80F00000u /* 0x4000 */
#define SDA2 0x80F10000u  /* 0x8000: r2's small data */
#define R2 (SDA2 + 0x4000u)
#define GXD 0x80F20000u /* 0x4000: __GXData */
#define SDA 0x80F2C000u /* 0x8000: r13's small data */
#define R13 (SDA + 0x8000u)
#define HEAP 0x80F40000u /* 256 objects of 0x100 bytes, and a guard of 0x1000 */
#define OBJECTS 256u
#define VT 0x80F52000u /* 0x400: the vtable every object points to */

#define LOW 0x80000000u /* 0x1000: where the module's translation puts a store to a small address */
static const Region k_regions[] = {
    {STACK, 0x4000u}, {SDA2, 0x8000u}, {GXD, 0x4000u}, {SDA, 0x8000u}, {HEAP, 0x11000u}, {VT, 0x400u},
    {LOW, 0x1000u},
};

/* --- The functions the natives call through pointers. ------------------- */

static const u32 k_getters[] = {
    0x80257550u, 0x80257560u, 0x80257570u, 0x8025757Cu, 0x8025758Cu, 0x802575F8u, 0x80257608u, 0x80257618u,
    0x80257628u, 0x80257634u, 0x80257640u, 0x8025764Cu, 0x80257654u, 0x8025765Cu, 0x80257668u, 0x80257674u,
    0x80257684u, 0x80257694u, 0x802576B0u, 0x802576CCu, 0x802576E8u, 0x80257704u, 0x80257720u, 0x8025773Cu,
    0x8025774Cu, 0x8025775Cu, 0x80257778u, 0x80257788u, 0x80257798u, 0x802577B4u, 0x802577D0u, 0x802577ECu,
    0x802577F8u, 0x80257804u, 0x80257814u, 0x80257824u, 0x80257834u, 0x8025784Cu, 0x8025785Cu, 0x8025786Cu,
    0x80257878u, 0x80257888u, 0x80257898u, 0x802578ACu, 0x802578B8u, 0x802578CCu, 0x802578D8u, 0x802578E8u,
    0x802578F4u, 0x80257904u, 0x80257910u, 0x8025791Cu, 0x80257928u, 0x80257934u, 0x80257940u, 0x8025794Cu,
    0x80257958u, 0x80257964u, 0x80257970u, 0x8025797Cu, 0x80257B4Cu, 0x80257B5Cu, 0x80257B6Cu, 0x80257B7Cu,
    0x80257B8Cu, 0x80257B9Cu, 0x80257BACu, 0x80257BB8u, 0x80257BC4u, 0x80257BD0u, 0x80257BDCu, 0x80257BECu,
    0x80257BFCu, 0x80257C08u, 0x80257C14u, 0x80257C1Cu, 0x80257C24u, 0x80257C2Cu, 0x80257C34u, 0x80257C44u,
    0x80257C54u, 0x80257C60u, 0x80257C70u, 0x80257C80u, 0x80257C8Cu, 0x80257C98u, 0x80257CA4u, 0x80257CB0u,
    0x80257CC0u, 0x80257CCCu, 0x80257CD8u, 0x80257CE4u, 0x80257CF0u, 0x80257CF8u, 0x80257D00u, 0x80257D10u,
    0x80257D1Cu, 0x80257D28u, 0x80257D34u, 0x80257D40u,
};
static const u32 k_calc_ptcl[] = {
    0x80264C88u, 0x80264DB8u, 0x80264EE8u, 0x802650B8u, 0x80265294u, 0x802652A4u, 0x80265374u,
    0x80265444u, 0x80265588u, 0x802656CCu, 0x80265734u, 0x8026579Cu, 0x802657E8u, 0x80265880u,
    0x80265918u, 0x802659C4u, 0x80265A90u, 0x80265B14u, 0x80265C40u, 0x80265D54u, 0x80265EC4u,
    0x80266048u, 0x80266100u, 0x802661B4u, 0x80266284u, 0x8026636Cu, 0x8026640Cu, 0x80266420u,
};
static const u32 k_dir_types[] = {0x8026134Cu, 0x80261368u, 0x80261384u, 0x802613C4u, 0x802613E8u};
static const u32 k_rot_types[] = {0x802614A8u, 0x802614E8u, 0x80261528u, 0x80261568u, 0x802615C4u};
static const u32 k_base_planes[] = {0x8026161Cu, 0x80261654u};
/* The vtables' slots (offset 8 + 4 x slot): JPABaseShapeArc's and JPAExtraShapeArc's
 * functions (native_draw_gen.py's BASE_SLOTS, EXTRA_SLOTS; an overloaded slot both). */
static const u32 k_base_slots[][2] = {
    {0u, 0u},
    {0x8025758Cu, 0x8025758Cu},
    {0x802575F8u, 0x802575F8u},
    {0x80257608u, 0x80257608u},
    {0x80257618u, 0x80257618u},
    {0x80257628u, 0x80257628u},
    {0x80257634u, 0x80257634u},
    {0x80257640u, 0x80257640u},
    {0x8025764Cu, 0x8025764Cu},
    {0x80257654u, 0x80257654u},
    {0x8025765Cu, 0x8025765Cu},
    {0x80257668u, 0x80257668u},
    {0x80257674u, 0x80257674u},
    {0x80257684u, 0x80257684u},
    {0x80257694u, 0x80257694u},
    {0x802576B0u, 0x802576B0u},
    {0x8025773Cu, 0x8025773Cu},
    {0x802576CCu, 0x802576CCu},
    {0x802576E8u, 0x802576E8u},
    {0x80257704u, 0x80257704u},
    {0x80257720u, 0x80257720u},
    {0x8025774Cu, 0x8025774Cu},
    {0x8025775Cu, 0x8025775Cu},
    {0x80257778u, 0x80257778u},
    {0x80257788u, 0x80257788u},
    {0x80257798u, 0x80257798u},
    {0x802577B4u, 0x802577B4u},
    {0x802577D0u, 0x802577D0u},
    {0x802577ECu, 0x802577ECu},
    {0x802577F8u, 0x802577F8u},
    {0x80257804u, 0x80257804u},
    {0x80257814u, 0x80257814u},
    {0x80257824u, 0x80257824u},
    {0x8025757Cu, 0x8025757Cu},
    {0x80257834u, 0x80257834u},
    {0x8025784Cu, 0x8025784Cu},
    {0x80257570u, 0x80257570u},
    {0x8025785Cu, 0x8025786Cu},
    {0x8025785Cu, 0x8025786Cu},
    {0x80257878u, 0x80257878u},
    {0x80257888u, 0x80257888u},
    {0x80257560u, 0x80257560u},
    {0x80257550u, 0x80257550u},
    {0x80257898u, 0x802578ACu},
    {0x802578B8u, 0x802578CCu},
    {0x80257898u, 0x802578ACu},
    {0x802578B8u, 0x802578CCu},
    {0x802578D8u, 0x802578D8u},
    {0x802578E8u, 0x802578E8u},
    {0x802578F4u, 0x802578F4u},
    {0x80257904u, 0x80257904u},
    {0x80257910u, 0x80257910u},
    {0x8025791Cu, 0x8025791Cu},
    {0x80257928u, 0x80257928u},
    {0x80257934u, 0x80257934u},
    {0x80257940u, 0x80257940u},
    {0x8025794Cu, 0x8025794Cu},
    {0x80257958u, 0x80257958u},
    {0x80257964u, 0x80257964u},
    {0x80257970u, 0x80257970u},
    {0x8025797Cu, 0x8025797Cu},
};
static const u32 k_extra_slots[][2] = {
    {0u, 0u},
    {0x80257B4Cu, 0x80257B4Cu},
    {0x80257B5Cu, 0x80257B5Cu},
    {0x80257B6Cu, 0x80257B6Cu},
    {0x80257B7Cu, 0x80257B7Cu},
    {0x80257B8Cu, 0x80257B8Cu},
    {0x80257B9Cu, 0x80257B9Cu},
    {0x80257BACu, 0x80257BACu},
    {0x80257BB8u, 0x80257BB8u},
    {0x80257BC4u, 0x80257BC4u},
    {0x80257BD0u, 0x80257BD0u},
    {0x80257BDCu, 0x80257BDCu},
    {0x80257BECu, 0x80257BECu},
    {0x80257BFCu, 0x80257BFCu},
    {0x80257C08u, 0x80257C08u},
    {0x80257C14u, 0x80257C14u},
    {0x80257C1Cu, 0x80257C1Cu},
    {0x80257C24u, 0x80257C24u},
    {0x80257C2Cu, 0x80257C2Cu},
    {0x80257C34u, 0x80257C34u},
    {0x80257C44u, 0x80257C44u},
    {0x80257C54u, 0x80257C54u},
    {0x80257C60u, 0x80257C60u},
    {0x80257C70u, 0x80257C70u},
    {0x80257C80u, 0x80257C80u},
    {0x80257C8Cu, 0x80257C8Cu},
    {0x80257C98u, 0x80257C98u},
    {0x80257CA4u, 0x80257CA4u},
    {0x80257CB0u, 0x80257CB0u},
    {0x80257CC0u, 0x80257CC0u},
    {0x80257CCCu, 0x80257CCCu},
    {0x80257CD8u, 0x80257CD8u},
    {0x80257CE4u, 0x80257CE4u},
    {0x80257CF0u, 0x80257CF0u},
    {0x80257CF8u, 0x80257CF8u},
    {0x80257D00u, 0x80257D00u},
    {0x80257D10u, 0x80257D10u},
    {0x80257D1Cu, 0x80257D1Cu},
    {0x80257D28u, 0x80257D28u},
    {0x80257D34u, 0x80257D34u},
    {0x80257D40u, 0x80257D40u},
};
/* Code no list names (a stop where the native meets it). */
static const u32 k_other_code[] = {0x80263508u, 0x80263510u, 0x802614A8u, 0x8026161Cu};
static u32 s_case_seed; /* the generator's state as the case began: rerun one with CASE SEED */
static const char* s_phase = "setup"; /* what the test is doing, for a fault's report */
static unsigned s_case_index;
#define PICK(list) (list[below(sizeof list / sizeof list[0])])

enum { KIND_PARTICLE, KIND_EMITTER, KIND_CALC, KIND_CALC_PARTICLE, KIND_WAVE };

typedef struct Native {
    u32 entry;
    const char* name;
    unsigned kind;
    s64 shortest;
} Native;

static const Native k_natives[] = {
    {0x80260D24u, "JPADrawExecRotBillBoard::exec", KIND_PARTICLE, 20},
    {0x80260BACu, "JPADrawExecBillBoard::exec", KIND_PARTICLE, 20},
    {0x80260F2Cu, "JPADrawExecYBillBoard::exec", KIND_PARTICLE, 20},
    {0x8026110Cu, "JPADrawExecRotYBillBoard::exec", KIND_PARTICLE, 20},
    {0x80261AD0u, "JPADrawExecRotDirectional::exec", KIND_PARTICLE, 20},
    {0x80261F60u, "JPADrawExecDirectionalCross::exec", KIND_PARTICLE, 20},
    {0x802624D4u, "JPADrawExecRotDirectionalCross::exec", KIND_PARTICLE, 20},
    {0x80262A98u, "JPADrawExecDirBillBoard::exec", KIND_PARTICLE, 20},
    {0x80262DC0u, "JPADrawExecRotation::exec", KIND_PARTICLE, 20},
    {0x80262FBCu, "JPADrawExecRotationCross::exec", KIND_PARTICLE, 20},
    {0x802632ECu, "JPADrawExecPoint::exec", KIND_PARTICLE, 20},
    {0x80263380u, "JPADrawExecLine::exec", KIND_PARTICLE, 20},
    {0x80263518u, "JPADrawExecStripe::exec", KIND_EMITTER, 20},
    {0x80263A68u, "JPADrawExecStripeCross::exec", KIND_EMITTER, 20},
    {0x802605FCu, "JPADrawExecRegisterPrmCEnv::exec", KIND_PARTICLE, 20},
    {0x80260728u, "JPADrawExecRegisterPrmAEnv::exec", KIND_PARTICLE, 20},
    {0x802643B0u, "JPADrawExecRegisterColorEmitterPE::exec", KIND_PARTICLE, 20},
    {0x80264C88u, "JPADrawCalcScaleX::calc", KIND_CALC, 12},
    {0x80264DB8u, "JPADrawCalcScaleY::calc", KIND_CALC, 12},
    {0x80264EE8u, "JPADrawCalcScaleXBySpeed::calc", KIND_CALC, 12},
    {0x80265B14u, "JPADrawCalcAlpha::calc", KIND_CALC, 12},
    {0x80268940u, "JPADraw::calcParticle", KIND_CALC_PARTICLE, 20},
    {0x8009A094u, "drawWave", KIND_WAVE, 20},
};
#define NATIVE_COUNT (sizeof k_natives / sizeof k_natives[0])

/* The chunks the hooked runs link: the natives' own, and what their paths call. */
#define TEST_CHUNKS(X) \
    X(38, 800996E0) X(149, 802556E0) X(151, 8025D6E0) X(152, 802616E0) X(153, 802656E0) X(194, 803096E0) \
    X(195, 8030D6E0) X(199, 8031D6E0) X(200, 803216E0) X(201, 803256E0) X(203, 8032D6E0)

typedef struct Case {
    CPUState cpu;
    PipeCase pipe;
    bool must_decline, journal, aliases, flush;
} Case;

/* A word that is no address the translation would reach outside the test's
 * pages: not MEM1 (whose other pages are read-only: a store there faults the
 * test), not its uncached mirror, not the hardware. Such a word moves to
 * where the module finds nothing (no RAM, no external handler: a load reads
 * zero, a store goes nowhere) - as anything else outside MEM1 does. */
static u32 safe(u32 word) {
    const u32 top = word >> 24;
    if (top == 0x80u || top == 0x81u || top == 0xC0u || top == 0xC1u || top == 0xCCu)
        return word ^ 0x10000000u;
    return word;
}

static void fill(u8* ram, u32 start, u32 bytes) {
    for (u32 i = 0; i < bytes; i += 4u)
        put32(ram, start + i, safe(next()));
}

static u32 object(void) { return HEAP + 0x100u * below(OBJECTS); }

/* A word of an object: what the game keeps there, roughly. */
/* An ordinary single (0.25 to 10.25, either sign). */
static u32 ordinary(void) {
    const f32 v = (0.25f + (f32)below(1000u) * 0.01f) * (below(2u) ? 1.0f : -1.0f);
    u32 bits;
    memcpy(&bits, &v, sizeof bits);
    return bits;
}

/* Benchmark cases: their singles ordinary (no zeros, denormals, NaNs). */
static bool s_bench_floats;

static u32 heap_word(void) {
    const u32 r = below(100u);
    if (r < 60u)
        return object();
    if (r < 90u)
        return safe(s_bench_floats ? ordinary() : any_single(1u));
    if (r < 92u)
        return 0u;
    if (r < 96u)
        return below(16u);
    if (r < 98u)
        return object() + 4u * below(16u);
    return below(2u) ? below(256u) : safe(next());
}

/* drawWave's loop at its head (d_kankyo_rain.cpp): the waves (r25: 56 bytes
 * each from 24, the wind at 16824 and 16828), the scales and the count (r31:
 * 2404, 2416; the count, a halfword, at 2424), the quad's statics (r30: their
 * registration links at 684, the vectors at 732, r26 to r29 into them), wave
 * r24 of them (r22 56 times it, r23 31 times), the frame (the view matrix at
 * 244, the texture object at 116, the colour at 24, the saved registers from
 * 352, the return address at 564), the statics' guard byte in r13's small
 * data. r2 is placed so that the loop's constants (-29264 to -28428) are in
 * r2's region, and the math kernels' (-12208 to -11936) and __GXData's
 * pointer (-12848) too. */
#define WAVE_R2 (SDA2 + 0x7400u)
/* A single of the waves, the scales or the frame's matrix: only ever loaded as
 * a float (no load goes through it), so not moved by safe(). */
static u32 wave_single(void) { return s_bench_floats ? ordinary() : any_single(1u); }
/* fdlibm's constants (sin, __ieee754_rem_pio2, __kernel_sin, __kernel_cos), by
 * their offsets below r2 in the translation's loads; 12128 is the
 * integer-to-double bias 2^52 + 2^31. */
static const struct {
    u32 offset;
    f64 value;
} k_msl_constants[] = {
    {12208u, 0.0},                       /* rem_pio2: zero */
    {12200u, 1.57079632673412561417e+00}, /* pio2_1 */
    {12192u, 6.07710050650619224932e-11}, /* pio2_1t */
    {12184u, 6.07710050630396597660e-11}, /* pio2_2 */
    {12176u, 2.02226624879595063154e-21}, /* pio2_2t */
    {12168u, 0.5},                       /* half */
    {12160u, 6.36619772367581382433e-01}, /* invpio2 */
    {12152u, 2.02226624871116645580e-21}, /* pio2_3 */
    {12144u, 8.47842766036889956997e-32}, /* pio2_3t */
    {12128u, 0.0},                       /* (the bias, written as bits) */
    {12120u, 1.0},                       /* kernel_cos: one */
    {12112u, 4.16666666666666019037e-02}, /* C1 */
    {12104u, -1.38888888888741095749e-03}, /* C2 */
    {12096u, 2.48015872894767294178e-05}, /* C3 */
    {12088u, -2.75573143513906633035e-07}, /* C4 */
    {12080u, 2.08757232129817482790e-09}, /* C5 */
    {12072u, -1.13596475577881948265e-11}, /* C6 */
    {12064u, 0.5},
    {12056u, 0.28125},
    {11984u, 8.33333333332248946124e-03}, /* kernel_sin: S2 */
    {11976u, -1.98412698298579493134e-04}, /* S3 */
    {11968u, 2.75573137070700676789e-06}, /* S4 */
    {11960u, -2.50507602534068634195e-08}, /* S5 */
    {11952u, 1.58969099521155010221e-10}, /* S6 */
    {11944u, -1.66666666666666324348e-01}, /* S1 */
    {11936u, 0.5},                       /* half */
    {11760u, 0.0},                       /* sin: zero */
};
static void wave_case(u8* ram, CPUState* c) {
    const u32 waves = HEAP + 0x100u * below(0x40u); /* + 16832 inside the heap */
    const u32 count = s_bench_floats ? 16u + below(33u)
                      : below(16u) == 0u ? (below(4u) == 0u ? (next() & 0xFFFFu) & 0x803Fu : below(64u))
                                         : 1u + below(8u);
    for (u32 i = 0; i < 64u * 56u; i += 4u)
        put32(ram, waves + i, s_bench_floats || below(16u) != 0u ? wave_single() : heap_word());
    put32(ram, waves + 16824u, wave_single());
    put32(ram, waves + 16828u, wave_single());
    const u32 scales = HEAP + 0x9000u + 0x100u * below(16u);
    put32(ram, scales + 2404u, wave_single());
    put32(ram, scales + 2416u, wave_single());
    write_be16(ram + (scales + 2424u - GC_RAM_BASE), (u16)count);
    const u32 statics = HEAP + 0xA000u + 4u * below(64u);
    const u32 i = count != 0u && (count & 0x8000u) == 0u && below(16u) != 0u ? below(count) : below(4u);
    c->gpr[25] = waves;
    c->gpr[31] = scales;
    c->gpr[30] = statics;
    c->gpr[29] = statics + 732u;
    c->gpr[28] = statics + 744u;
    c->gpr[27] = statics + 756u;
    c->gpr[26] = statics + 768u;
    if (below(32u) == 0u)
        c->gpr[26 + below(4u)] = heap_word();
    c->gpr[24] = i;
    c->gpr[22] = 56u * i;
    c->gpr[23] = 31u * i;
    if (below(32u) == 0u)
        c->gpr[23] = next();
    /* The frame: r1 + 564 inside the stack's region. */
    const u32 sp = STACK + 0x1000u + 8u * below(64u);
    c->gpr[1] = sp;
    for (u32 w = 0; w < 12u; ++w)
        put32(ram, sp + 244u + 4u * w, wave_single());
    put32(ram, sp + 116u + 24u, below(16u) == 0u ? (below(2u) ? next() : 20u + below(4u)) : below(20u));
    if (below(2u) != 0u)
        put8(ram, sp + 116u + 31u, (u8)(next() & ~2u)); /* the TLUT path */
    put32(ram, sp + 564u, RETURN_ADDRESS | below(4u));
    /* r2, and the constants: singles, and __GXData's pointer (sometimes
     * outside RAM, never inside __GXData's region elsewhere: GXLoadTexObj
     * calls through its words there, and a word that is no code would send
     * the module's translation into code the test's pages do not cover). */
    c->gpr[2] = below(256u) == 0u ? (0x90000000u | (next() & 0x0FFFFFFFu)) : WAVE_R2;
    put32(ram, WAVE_R2 - 12848u, below(64u) == 0u ? 0x90000000u : GXD);
    if (s_bench_floats)
        for (u32 o = 29264u; o >= 28428u; o -= 4u)
            put32(ram, WAVE_R2 - o, ordinary());
    /* The integer-to-float bias (-29248: 2^52 + 2^31) mostly; and, mostly,
     * the math kernels' constants: fdlibm's, where the translation loads
     * them (so the reduction's quotient and the polynomials are as in play). */
    if (s_bench_floats || below(8u) != 0u) {
        put32(ram, WAVE_R2 - 29248u, 0x43300000u);
        put32(ram, WAVE_R2 - 29244u, 0x80000000u);
    }
    if (s_bench_floats || below(8u) != 0u) {
        put32(ram, WAVE_R2 - 29264u, 0u); /* the loop's zero */
        for (unsigned j = 0; j < sizeof k_msl_constants / sizeof k_msl_constants[0]; ++j) {
            u64 bits;
            memcpy(&bits, &k_msl_constants[j].value, sizeof bits);
            if (k_msl_constants[j].offset == 12128u)
                bits = 0x4330000080000000ull;
            put32(ram, WAVE_R2 - k_msl_constants[j].offset, (u32)(bits >> 32));
            put32(ram, WAVE_R2 - k_msl_constants[j].offset + 4u, (u32)bits);
        }
    }
    /* The statics' guard: set mostly (their registration done). */
    put8(ram, R13 - 29770u, below(8u) == 0u ? 0u : (u8)(1u + below(255u)));
    /* f31: the loop's zero mostly. */
    if (below(8u) != 0u)
        c->fpr[31] = c->ps1[31] = 0.0;
}

static Case build(u8* ram, unsigned which, unsigned scenario) {
    Case k;
    memset(&k, 0, sizeof k);
    const Native* n = &k_natives[which];
    for (unsigned r = 0; r < sizeof k_regions / sizeof k_regions[0]; ++r)
        fill(ram, k_regions[r].start, k_regions[r].bytes);
    /* r2's small data: singles mostly (the constants), __GXData's pointer. */
    for (u32 i = 0; i < 0x8000u; i += 4u)
        if (below(8u) != 0u)
            put32(ram, SDA2 + i, safe(any_single(1u)));
    put32(ram, R2 - 12848u, below(64u) == 0u ? (below(2u) ? 0x90000000u : GXD + 4u * below(0x100u)) : GXD);
    /* r13's small data: objects mostly (the clipboard, the draw's statics). */
    for (u32 i = 0; i < 0x8000u; i += 4u)
        put32(ram, SDA + i, below(8u) != 0u ? object() : heap_word());
    /* __GXData: the first word nonzero mostly, the dirty state clear mostly,
     * the TEV stages' texture maps and the region callbacks the SDK's. */
    if (below(32u) == 0u)
        put32(ram, GXD, 0u);
    put32(ram, GXD + 1268u, below(4u) != 0u ? 0u : below(3u) == 0u ? safe(next()) : (next() & 0x3Fu));
    for (u32 i = 0; i < 16u; ++i)
        put32(ram, GXD + 1180u + 4u * i, (below(4u) == 0u ? 0xFFu : below(8u)) | (below(4u) == 0u ? 0x100u : 0u));
    put32(ram, GXD + 1040u, 0x8031FA48u);
    put32(ram, GXD + 1044u, 0x8031FAC4u);
    /* The objects: the vtable first, then pointers, NULLs, singles, small integers. */
    const bool fields_fn = n->kind == KIND_EMITTER || n->kind == KIND_PARTICLE;
    for (u32 o = 0; o < OBJECTS; ++o) {
        const u32 base = HEAP + 0x100u * o;
        put32(ram, base, !s_bench_floats && below(64u) == 0u ? object() : VT);
        for (u32 w = 4; w < 0x100u; w += 4u)
            put32(ram, base + w, heap_word());
        /* The first words of a structure are its pointers mostly (the draw
         * context's emitter, shapes, list and draw). */
        for (u32 w = 4; w < (s_bench_floats ? 0x20u : 0x40u); w += 4u)
            if (below(10u) != 0u)
                put32(ram, base + w, object());
        /* A benchmark's objects past their first pointers: ordinary singles
         * mostly (a particle's positions and velocity, a clipboard's scales),
         * and the clipboard's matrix pointer (0x34). */
        if (s_bench_floats) {
            for (u32 w = 0x20u; w < 0x100u; w += 4u)
                put32(ram, base + w, below(12u) != 0u ? safe(ordinary()) : object());
            put32(ram, base + 0x34u, object());
        }
        /* JPADraw: the calc visitors (0x48) and their counts (0x8C), small. */
        if (n->kind == KIND_CALC_PARTICLE) {
            for (u32 w = 0x48u; w < 0x70u; w += 4u)
                if (below(20u) != 0u)
                    put32(ram, base + w, object());
            for (u32 w = 0x90u; w < 0xB4u; w += 4u) /* its draw context's pointers */
                if (below(10u) != 0u)
                    put32(ram, base + w, object());
            put32(ram, base + 0x8Cu, (below(4u) << 24) | (below(6u) << 16) | (below(4u) << 8) | below(4u));
        }
        /* The draw clipboard's mDirTypeFunc (0xA0); a list link's prev and
         * next (JSUPtrLink: 8, 12), NULL a quarter of the time, so lists end. */
        if (fields_fn) {
            put32(ram, base + 0xA0u, (s_bench_floats || below(16u) != 0u) ? PICK(k_dir_types) : PICK(k_other_code));
            put32(ram, base + 0xA4u, (s_bench_floats || below(16u) != 0u) ? PICK(k_rot_types) : PICK(k_other_code));
            put32(ram, base + 0xA8u, (s_bench_floats || below(16u) != 0u) ? PICK(k_base_planes) : PICK(k_other_code));
            if (below(4u) == 0u)
                put32(ram, base + 8u, 0u);
            if (below(4u) == 0u)
                put32(ram, base + 12u, 0u);
        }
    }
    /* The vtable: in each slot one of the classes' functions there mostly (at
     * 12, for calcParticle, a calc visitor), sometimes other code or an object
     * (a vtable read as a plain object). */
    for (u32 s = 0; s < 0x100u; ++s) {
        u32 f = object();
        const unsigned nb = sizeof k_base_slots / sizeof k_base_slots[0];
        const unsigned ne = sizeof k_extra_slots / sizeof k_extra_slots[0];
        if (s >= 2u && below(16u) != 0u) {
            const unsigned k = s - 2u;
            const bool base = below(2u) != 0u;
            if (base && k < nb && k_base_slots[k][0] != 0u)
                f = k_base_slots[k][below(2u)];
            else if (k < ne && k_extra_slots[k][0] != 0u)
                f = k_extra_slots[k][below(2u)];
            else if (k < nb && k_base_slots[k][0] != 0u)
                f = k_base_slots[k][below(2u)];
        }
        if (s == 3u && n->kind == KIND_CALC_PARTICLE && below(8u) != 0u)
            f = PICK(k_calc_ptcl);
        if (!s_bench_floats && s >= 2u && below(64u) == 0u)
            f = PICK(k_other_code);
        put32(ram, VT + 4u * s, f);
    }
    CPUState* c = &k.cpu;
    random_cpu(c, ram);
    for (unsigned r = 0; r < 32; ++r)
        c->gpr[r] = safe(c->gpr[r]);
    c->ctr = safe(c->ctr);
    c->pc = n->entry;
    c->gpr[1] = STACK + 0x3000u - 8u * below(64u);
    c->gpr[2] = below(256u) == 0u ? (0x90000000u | (next() & 0x0FFFFFFFu)) : R2;
    c->gpr[13] = below(256u) == 0u ? (0x90000000u | (next() & 0x0FFFFFFFu)) : R13;
    c->gpr[3] = below(64u) == 0u ? heap_word() : object();
    c->gpr[4] = below(64u) == 0u ? heap_word() : object();
    c->gpr[5] = below(64u) == 0u ? heap_word() : object();
    /* FP registers: singles mostly. */
    for (unsigned r = 0; r < 32; ++r)
        if (below(8u) != 0u)
            c->fpr[r] = c->ps1[r] = f64_value(convert_to_double(any_single(1u)));
    if (n->kind == KIND_WAVE)
        wave_case(ram, c);
    if (c->reserve_valid) {
        switch (below(4u)) {
        case 0: c->reserve_addr = c->gpr[1] - 8u * below(8u); break;
        case 1: c->reserve_addr = object() + 4u * below(64u); break;
        default: break;
        }
    }
    clock_edges(c, scenario, n->shortest);
    k.pipe = random_pipe(scenario);
    k.journal = scenario % 41u == 7u;
    k.aliases = scenario % 43u == 8u;
    k.flush = below(4u) == 0u;
    k.must_decline = k.journal || k.aliases || c->exception != 0u || k.pipe.mode != PIPE_BATCH;
    return k;
}

/* Host scenarios: busy, the edge filter off, or the natives' boundaries watched. */
enum { HOST_QUIET, HOST_BUSY, HOST_FILTER_OFF, HOST_WATCHED };
static unsigned host_scenario(unsigned scenario) {
    switch (scenario % 37u) {
    case 5: return HOST_BUSY;
    case 11: return HOST_FILTER_OFF;
    case 17: return HOST_WATCHED;
    default: return HOST_QUIET;
    }
}

/* The module's own watch list (the tree's bw_edge_watch.inc), so the natives'
 * boundaries are watched where the module's are. */
#include DRAW_WATCH_LIST
static void watch_module(void) {
    for (unsigned i = 0; i < sizeof bw_edge_watch_list / sizeof bw_edge_watch_list[0]; ++i)
        watch(bw_edge_watch_list[i]);
}

/* A tag, as the host's particle draw tags are (runtime/host/src/draw_tags.c):
 * a BP register write into the pipe at a boundary the host is asked about,
 * after every byte before it. The reference's edge service writes one into
 * the module's stream, the hooked runs one at the same boundaries (the ones
 * the module's chassis asks about: dispatch_loop.h, with the module's own
 * watch list), so comparing the streams also compares where each native's
 * bytes fall relative to the host's. */
static u32 tag_word(u32 address) { return 0x4F000000u | ((address >> 2) & 0x00FFFFFFu); }
/* A tag into a log, the same one again with nothing between left out: a
 * boundary asked about again without progress (a deadline at its block: the
 * module's chassis tries it again a few times, counting across runs; the
 * hooked runs until their guard) says nothing about the bytes' order. */
static void put_tag(PipeLog* log, u32* end, u32* at, u32 address) {
    if (log->length == *end && *at == address)
        return;
    log_word(log, 0x61u, 1u);
    log_word(log, tag_word(address), 4u);
    *end = log->length;
    *at = address;
}
static u32 s_module_tag_end = ~0u, s_module_tag_at, s_tag_end = ~0u, s_tag_at;

/* The module's watch list as its edge filter holds it (direct_calls.h's
 * bw_edge_unwatched, over the module's list only: the test's own table also
 * has the HOST_WATCHED scenario's addresses, which the module does not watch). */
static u32 s_module_watch[BW_EDGE_WATCH_SLOTS];
static bool module_unwatched(u32 address) {
    const u32 canonical = address & ~0x40000000u;
    const bool main_code = canonical >= 0x80003100u && canonical < 0x80400000u;
    const bool rel_code = address >= 0xC0400000u && address < 0xC2000000u;
    if (!(main_code || rel_code))
        return false;
    for (u32 slot = (canonical * 0x9E3779B1u) >> 20;; slot = (slot + 1u) & (BW_EDGE_WATCH_SLOTS - 1u)) {
        if (s_module_watch[slot] == 0u)
            return true;
        if (s_module_watch[slot] == canonical)
            return false;
    }
}
static void module_watch_setup(void) {
    for (unsigned i = 0; i < sizeof bw_edge_watch_list / sizeof bw_edge_watch_list[0]; ++i) {
        const u32 canonical = bw_edge_watch_list[i] & ~0x40000000u;
        u32 slot = (canonical * 0x9E3779B1u) >> 20;
        while (s_module_watch[slot] != 0u && s_module_watch[slot] != canonical)
            slot = (slot + 1u) & (BW_EDGE_WATCH_SLOTS - 1u);
        s_module_watch[slot] = canonical;
    }
}

/* The module's edge service for this test: the run ends at the return
 * address; every other boundary the module asks about passes with a tag in
 * the module's stream, counted. */
static unsigned s_services;
static int draw_edge_service(void* user, CPUState* cpu, u32 address) {
    (void)user;
    (void)cpu;
    if ((address & ~3u) == RETURN_ADDRESS)
        return 1;
    put_tag(&log_module, &s_module_tag_end, &s_module_tag_at, address);
    s_services++;
    s_unexpected_service++;
    s_unexpected_at = address;
    return 0;
}

static void set_host(unsigned host) {
    reset_host();
    watch_module();
    if (host == HOST_BUSY)
        s_sources_dirty = true;
    if (host == HOST_FILTER_OFF)
        bw_edge_filter_enabled = false;
    if (host == HOST_WATCHED) {
        watch(0x8030DA44u); /* PSMTXMultVec */
        watch(0x80257674u); /* getListOrder */
        watch(0x8026134Cu); /* dirTypeVel */
        watch(0x80263508u); /* stripeGetNext */
        watch(0x80325FA8u); /* GXSetTevColor */
        watch(0x80264C88u); /* CalcScaleX */
        watch(0x80330C84u); /* sin */
        watch(0x80324EE8u); /* GXLoadTexObj */
        watch(0x80328990u); /* __register_global_object */
    }
}

static const char* s_only;
static unsigned s_first;
static u32 s_first_seed;
static bool skip(const Native* n) {
    if (s_only == NULL)
        return false;
    for (const char* p = s_only; *p != 0;) {
        const char* end = strchr(p, ',');
        const size_t length = end != NULL ? (size_t)(end - p) : strlen(p);
        if (length != 0u && strncmp(n->name, p, length) == 0)
            return false;
        p += length + (end != NULL);
    }
    return true;
}

/* --- The hooked chunks. --------------------------------------------------- */

#define DECLARE_CHUNK(index, start) void func_##start(CPUState*);
TEST_CHUNKS(DECLARE_CHUNK)
#undef DECLARE_CHUNK
static const struct {
    unsigned index;
    u32 start;
    BwChunkFn fn;
} HOOKED_CHUNKS[] = {
#define ROW(index, start) {index, 0x##start##u, func_##start},
    TEST_CHUNKS(ROW)
#undef ROW
};

static BwChunkFn s_hooked_table[256];
BwChunkFn* const bw_chunk_fns = s_hooked_table;
static void missing_chunk(CPUState* cpu) { cpu->pc = 0xFFFFFFF0u; /* left the linked chunks */ }

BwChunkFn bw_find_chunk(u32 address) {
    for (unsigned i = 0; i < sizeof HOOKED_CHUNKS / sizeof HOOKED_CHUNKS[0]; ++i)
        if (address - HOOKED_CHUNKS[i].start < 0x4000u)
            return HOOKED_CHUNKS[i].fn;
    return NULL;
}

int bw_native_call(CPUState* cpu, u32 address) {
    (void)cpu;
    (void)address;
    return 0;
}

/* The other sets' hooks in the linked chunks, off (the module runs with them
 * off too: harness_load turns native math off), and the 60 Hz sites at the
 * module's own 30 Hz step. */
int bluewake_native_game_math_enabled;
int bluewake_native_game_math(CPUState* cpu, u32 address) {
    (void)cpu;
    (void)address;
    return 0;
}
int bluewake_native_vec_sr_enabled;
int bluewake_native_vec_sr(CPUState* cpu) {
    (void)cpu;
    return 0;
}
int bluewake_native_search_enabled; /* strcmp's, in sin's chunk */
int bluewake_native_search(CPUState* cpu, u32 address) {
    (void)cpu;
    (void)address;
    return 0;
}
float bluewake_simulation_step = 1.0f;

unsigned dolrecomp_call_depth;
extern CPUState bw_guest_cpu;
extern u8 bw_guest_mem1[];

static void hooked_setup(void) {
    static bool ready;
    if (ready)
        return;
    ready = true;
    for (unsigned i = 0; i < 256u; ++i)
        s_hooked_table[i] = missing_chunk;
    for (unsigned i = 0; i < sizeof HOOKED_CHUNKS / sizeof HOOKED_CHUNKS[0]; ++i)
        s_hooked_table[HOOKED_CHUNKS[i].index] = HOOKED_CHUNKS[i].fn;
    if (protect_image(bw_guest_mem1) == NULL) {
        fprintf(stderr, "cannot protect the chunks' MEM1\n");
        exit(1);
    }
}

static void set_natives(int on) {
    bluewake_native_draw_enabled = on;
    bluewake_native_gx_enabled = on;
}

/* Where runs left the linked chunks (the most frequent, for the log). */
static u32 s_left_pc[64];
static unsigned s_left_count[64];
static void note_left(u32 pc) {
    for (unsigned i = 0; i < 64u; ++i) {
        if (s_left_count[i] == 0u || s_left_pc[i] == pc) {
            s_left_pc[i] = pc;
            s_left_count[i]++;
            return;
        }
    }
}
static void report_left(void) {
    for (unsigned i = 0; i < 64u && s_left_count[i] != 0u; ++i)
        if (s_left_count[i] >= 20u)
            printf("  left at %08X: %u runs\n", s_left_pc[i], s_left_count[i]);
    memset(s_left_count, 0, sizeof s_left_count);
}

/* The hooked chunks from `entry` on `start`, as the module's chassis loop
 * runs them (dispatch_loop.h): on to the return address, ending at an
 * exception or a spent budget, the host asked nothing (draw_edge_service
 * below lets every boundary pass): false where the run reached code in a
 * chunk the test does not link. */
static bool s_tags; /* the hooked runs' tags: off with no pipe writer, and in the benchmark */
static bool hooked_run(u32 entry, const CPUState* start, bool flush) {
    bw_guest_cpu = *start;
    bw_guest_cpu.ram = bw_guest_mem1;
    host_fp_mode(flush);
    ppc_fpscr_updated(&bw_guest_cpu);
    bw_guest_cpu.pc = entry;
    CPUState* c = &bw_guest_cpu;
    bool inside = true;
    s_tag_end = ~0u;
    for (unsigned guard = 0;; ++guard) {
        if ((c->pc & ~3u) == RETURN_ADDRESS || guard >= 4096u)
            break;
        if (guard != 0u && (c->exception != 0u || (c->cycle_budget > 0 && c->downcount <= -c->cycle_budget)))
            break;
        /* A boundary the module's chassis asks the host about: its tag. */
        if (guard != 0u && s_tags && !(bw_host_quiet(c) && module_unwatched(c->pc))) {
            bw_gather_pipe_drain();
            put_tag(log_target, &s_tag_end, &s_tag_at, c->pc);
        }
        BwChunkFn fn = bw_find_chunk(c->pc);
        if (fn == NULL) {
            /* Code in a chunk the test does not link, or no code at all
             * (which the module's dispatcher handles its own way): the run
             * cannot be followed further. */
            inside = false;
            note_left(c->pc);
            break;
        }
        const s64 prior = c->downcount;
        fn(c);
        /* The chassis ends a run whose first dispatch charges nothing (a
         * deadline at the entry's block) before it asks the host anything. */
        if (guard == 0u && c->downcount >= prior)
            break;
    }
    host_fp_mode(false);
    bw_gather_pipe_flush();
    return inside;
}

/* Per native: its cases compared end to end through the hooked chunks with a
 * native of the set running (entry or resume), and of them, how many stopped
 * at least once. */
static unsigned long long s_end_to_end[NATIVE_COUNT], s_with_stops[NATIVE_COUNT], s_outside[NATIVE_COUNT];
static unsigned long long s_with_tags[NATIVE_COUNT]; /* of them, with the host's tags in the stream */
static unsigned long long draw_count(unsigned which) {
    unsigned long long totals[3];
    bluewake_native_draw_totals(totals);
    return which == 3u ? totals[1] + totals[2] : totals[which];
}

/* --- One case. ------------------------------------------------------------ */

static unsigned long long s_flushed;
static int one_case(Harness* h, unsigned which, unsigned index, const Case* k, bool must_decline) {
    const Native* n = &k_natives[which];
    /* Plain: the native alone. */
    copy_regions(h->before, h->native_ram);
    set_test_pipe(k->pipe.mode);
    if (k->pipe.mode == PIPE_BATCH)
        prefill_test_pipe(k->pipe.prefill);
    else
        bw_gather_pipe_length = 0;
    u8 pipe_before[BW_GATHER_PIPE_BATCH + 8u];
    memcpy(pipe_before, bw_gather_pipe_buffer, sizeof pipe_before);
    const u32 length_before = bw_gather_pipe_length;
    log_native.length = 0;
    log_target = &log_native;
    CPUState native = k->cpu;
    host_fp_mode(k->flush);
    if (k->journal)
        g_mem_write_journal = journal;
    if (k->aliases)
        g_ppc_guest_aliases_overlap_mem1 = true;
    s_phase = "plain native";
    const int r = bluewake_native_draw(&native, n->entry);
    g_mem_write_journal = NULL;
    g_ppc_guest_aliases_overlap_mem1 = false;
    host_fp_mode(false);
    if (r == 0 && (memcmp(&native, &k->cpu, sizeof native) != 0 || !same_regions(h->before, h->native_ram) ||
                   log_native.length != 0u || bw_gather_pipe_length != length_before ||
                   memcmp(pipe_before, bw_gather_pipe_buffer, sizeof pipe_before) != 0)) {
        fprintf(stderr, "case %u (%s): declined but changed state\n", index, n->name);
        report_cpu(&native, &k->cpu);
        exit(1);
    }
    if (r != 0 && must_decline) {
        fprintf(stderr, "case %u (%s): ran where it must decline\n", index, n->name);
        exit(1);
    }
    if (r == 1 && log_native.length != 0u)
        s_flushed++;
    bw_gather_pipe_flush();
    /* The reference: the module's translation from the entry. */
    host_fp_mode(k->flush);
    s_phase = "reference";
    s_module_tag_end = ~0u;
    const unsigned services_before = s_services;
    const CPUState reference = harness_translate(h, &k->cpu, n->entry);
    const bool tagged = s_services != services_before && k->pipe.mode != PIPE_NONE;
    host_fp_mode(false);
    if (r == 1) {
        if (s_unexpected_service != 0u || (reference.pc & ~3u) != RETURN_ADDRESS ||
            memcmp(&native, &reference, sizeof native) != 0 || !same_regions(h->native_ram, h->reference_ram)) {
            fprintf(stderr, "case %u (%s, case seed %08X): mismatch (service asked %u times, last at %08X; pc %08X)\n",
                    index, n->name, s_case_seed, s_unexpected_service, s_unexpected_at, reference.pc);
            report_cpu(&native, &reference);
            report_ram(h->native_ram, h->reference_ram);
            exit(1);
        }
        static PipeLog native_log;
        copy_log(&native_log, &log_native);
        pipe_model(k->pipe.mode, k->pipe.prefill);
        if (!same_logs(&native_log, &log_model)) {
            fprintf(stderr, "case %u (%s, case seed %08X): the pipe differs\n", index, n->name, s_case_seed);
            report_logs(&native_log, &log_model);
            exit(1);
        }
    }
    /* Hooked: natives on, then off, against the reference. With no pipe
     * writer at all a pipe store is an ordinary store that finds nothing
     * there (gather_pipe.h), so nothing reaches the host. */
    hooked_setup();
    if (k->pipe.mode == PIPE_NONE)
        log_model.length = 0;
    else
        pipe_model(k->pipe.mode, k->pipe.prefill);
    static PipeLog model;
    copy_log(&model, &log_model);
    bool counted = false, stopped = false;
    s_tags = k->pipe.mode != PIPE_NONE;
    for (int on = 1; on >= 0; --on) {
        copy_regions(bw_guest_mem1, h->before);
        set_natives(on);
        set_test_pipe(k->pipe.mode);
        if (k->pipe.mode == PIPE_BATCH)
            prefill_test_pipe(k->pipe.prefill);
        else
            bw_gather_pipe_length = 0;
        log_native.length = 0;
        log_target = &log_native;
        /* (The write journal and the aliases are the plain run's: the natives
         * must decline with them, and the translation runs as without.) */
        const unsigned long long runs = draw_count(3u);
        const unsigned long long stops = draw_count(2u);
        s_phase = on ? "hooked, natives on" : "hooked, natives off";
        const bool inside = hooked_run(n->entry, &k->cpu, k->flush);
        if (!inside) {
            s_outside[which]++;
            set_natives(0);
            s_tags = false;
            return r;
        }
        CPUState got = bw_guest_cpu;
        got.ram = k->cpu.ram;
        if (memcmp(&got, &reference, sizeof got) != 0 || !same_regions(bw_guest_mem1, h->reference_ram) ||
            !same_logs(&log_native, &model)) {
            fprintf(stderr, "case %u (%s, case seed %08X): the hooked chunks, natives %s, differ\n", index, n->name, s_case_seed,
                    on ? "on" : "off");
            fprintf(stderr, "  got pc %08X lr %08X exception %u downcount %lld; want pc %08X lr %08X exception %u downcount %lld\n",
                    got.pc, got.lr, got.exception, (long long)got.downcount, reference.pc, reference.lr, reference.exception,
                    (long long)reference.downcount);
            report_cpu(&got, &reference);
            report_ram(bw_guest_mem1, h->reference_ram);
            report_logs(&log_native, &model);
            exit(1);
        }
        if (on) {
            counted = draw_count(3u) != runs;
            stopped = draw_count(2u) != stops;
        }
    }
    set_natives(0);
    s_tags = false;
    if (counted) {
        s_end_to_end[which]++;
        s_with_stops[which] += stopped;
        s_with_tags[which] += tagged;
    }
    return r;
}

/* --- The microbenchmark: through the hooked chunks, natives off and on. -- */

/* A benchmark case, rebuilt from the generator's state before it. */
static Case bench_case(u8* ram, unsigned which, unsigned tries, u32 state) {
    seed = state;
    set_host(HOST_QUIET);
    Case k = build(ram, which, 6u * (tries + 1u));
    k.cpu.exception = 0;
    k.cpu.downcount = 0;
    k.cpu.cycle_budget = 400000;
    k.cpu.cycle_deadline_budget = 0;
    k.cpu.reserve_valid = false;
    put32(ram, GXD + 1268u, 0u);
    return k;
}

/* ns per call through the hooked chunks, the sixth set off and on (the fifth
 * set on in both, as the module has it without this set): over eight of 400
 * cases with ordinary singles in which the native runs and the run reaches
 * the function's return - those where the native runs to the blr first
 * (drawWave aside), then those where it stops, the most work (cycles)
 * first - each timed alone (the best of three rounds), the times summed. */
static void bench(Harness* h, unsigned calls) {
    hooked_setup();
    u8* ram = h->native_ram;
    for (unsigned which = 0; which < NATIVE_COUNT; ++which) {
        const Native* n = &k_natives[which];
        if (skip(n))
            continue;
        struct {
            s64 work;
            u32 state;
            unsigned tries;
        } top[8];
        unsigned found = 0;
        s_bench_floats = true;
        for (unsigned tries = 0; tries < 400u; ++tries) {
            const u32 state = seed;
            Case k = bench_case(ram, which, tries, state);
            CPUState probe = k.cpu;
            copy_regions(h->before, ram);
            bluewake_composite_set_gather_pipe(sink_word);
            bluewake_composite_set_gather_pipe_bytes(sink_bytes);
            bw_gather_pipe_length = 0;
            const int ran = bluewake_native_draw(&probe, n->entry);
            copy_regions(ram, h->before);
            if (ran == 0)
                continue;
            copy_regions(bw_guest_mem1, ram);
            set_natives(0);
            bluewake_native_gx_enabled = 1;
            bw_gather_pipe_length = 0;
            if (!hooked_run(n->entry, &k.cpu, false) || (bw_guest_cpu.pc & ~3u) != RETURN_ADDRESS)
                continue;
            /* The cases where the native runs to the blr first (as it does
             * on the game's own data, where nothing it reads is a stray
             * pointer), then those where it stops; the most work first. Not
             * for drawWave, which stops before every quad's GXBegin: its
             * cases that run to the blr draw nothing. */
            const bool first = ran == 1 && n->kind != KIND_WAVE;
            const s64 work = k.cpu.downcount - bw_guest_cpu.downcount + (first ? (s64)1 << 40 : 0);
            unsigned at = found < 8u ? found++ : 8u;
            if (at == 8u) {
                unsigned least = 0;
                for (unsigned i = 1; i < 8u; ++i)
                    if (top[i].work < top[least].work)
                        least = i;
                if (top[least].work >= work)
                    continue;
                at = least;
            }
            top[at].work = work;
            top[at].state = state;
            top[at].tries = tries;
        }
        if (found == 0u) {
            s_bench_floats = false;
            printf("%08X %s: no benchmark case\n", n->entry, n->name);
            continue;
        }
        unsigned completing = 0;
        for (unsigned c8 = 0; c8 < found; ++c8)
            completing += top[c8].work >= (s64)1 << 40;
        double total[2] = {0.0, 0.0};
        unsigned long long lost = 0; /* calls that left the linked chunks (the pages drift between calls) */
        const unsigned per_case = calls / found + 1u;
        for (unsigned c8 = 0; c8 < found; ++c8) {
            const Case k = bench_case(ram, which, top[c8].tries, top[c8].state);
            bluewake_composite_set_gather_pipe(sink_word);
            bluewake_composite_set_gather_pipe_bytes(sink_bytes);
            double best[2] = {1e30, 1e30};
            for (unsigned round = 0; round < 6u; ++round) {
                const int on = (int)(round & 1u);
                copy_regions(bw_guest_mem1, ram);
                set_natives(on);
                bluewake_native_gx_enabled = 1;
                bw_guest_cpu = k.cpu;
                bw_guest_cpu.ram = bw_guest_mem1;
                ppc_fpscr_updated(&bw_guest_cpu);
                const double t0 = now_ns();
                for (unsigned i = 0; i < per_case; ++i) {
                    CPUState* c = &bw_guest_cpu;
                    memcpy(c->gpr, k.cpu.gpr, sizeof c->gpr);
                    memcpy(c->fpr, k.cpu.fpr, sizeof c->fpr);
                    memcpy(c->ps1, k.cpu.ps1, sizeof c->ps1);
                    c->lr = RETURN_ADDRESS;
                    c->pc = n->entry;
                    c->downcount = 0;
                    unsigned guard = 0;
                    do {
                        const u32 slot = (c->pc - 0x800016E0u) >> 14;
                        if (slot >= 256u || s_hooked_table[slot] == missing_chunk) {
                            lost++;
                            break;
                        }
                        s_hooked_table[slot](c);
                    } while ((c->pc & ~3u) != RETURN_ADDRESS && ++guard < 256u);
                    bw_gather_pipe_drain();
                }
                const double ns = (now_ns() - t0) / per_case;
                if (ns < best[on])
                    best[on] = ns;
            }
            total[0] += best[0];
            total[1] += best[1];
        }
        s_bench_floats = false;
        set_natives(0);
        printf("%08X %s: through the hooked chunks %.1f ns/call with the sixth set off, %.1f on (%.2fx; %u cases, "
               "%u of them running to the blr)%s\n",
               n->entry, n->name, total[0] / found, total[1] / found, total[0] / total[1], found, completing,
               lost != 0u ? " (some calls left the linked chunks)" : "");
        fflush(stdout);
    }
}

/* A fault: where, and what the test was doing (for the log). */
static u8* s_fault_images[3];
static LONG WINAPI on_fault(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        fprintf(stderr, "access violation at %p (%s %p) in %s, case %u, seed %08X, guest pc %08X\n",
                e->ExceptionRecord->ExceptionAddress, e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                (void*)e->ExceptionRecord->ExceptionInformation[1], s_phase, s_case_index, seed, bw_guest_cpu.pc);
        {
            HMODULE at = NULL;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)e->ExceptionRecord->ExceptionAddress, &at);
            char where[260] = "?";
            if (at != NULL)
                GetModuleFileNameA(at, where, sizeof where);
            fprintf(stderr, "  at %s + 0x%llx\n", where,
                    (unsigned long long)((u8*)e->ExceptionRecord->ExceptionAddress - (u8*)at));
        }
        for (unsigned i = 0; i < 3u; ++i) {
            const u8* f = (const u8*)e->ExceptionRecord->ExceptionInformation[1];
            if (s_fault_images[i] != NULL && f >= s_fault_images[i] && f < s_fault_images[i] + GC_MAIN_RAM_SIZE)
                fprintf(stderr, "  guest address %08X (image %u)\n", (u32)(f - s_fault_images[i]) + GC_RAM_BASE, i);
        }
        void* ret = *(void**)e->ContextRecord->Rsp;
        HMODULE mod = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ret, &mod);
        char name[260] = "?";
        if (mod != NULL)
            GetModuleFileNameA(mod, name, sizeof name);
        fprintf(stderr, "  [rsp] %p: %s + 0x%llx\n", ret, name, (unsigned long long)((u8*)ret - (u8*)mod));
        fflush(stderr);
        fflush(stdout);
        ExitProcess(3);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char** argv) {
    bluewake_native_gate_enabled = 0; /* every call tried */
    AddVectoredExceptionHandler(1, on_fault);
    if (argc < 2) {
        fprintf(stderr, "usage: native_draw_test MODULE.dll [CASES_PER_FUNCTION] [BENCH_CALLS] [NAME_PREFIX,...]\n");
        return 2;
    }
    const unsigned cases = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 60000u;
    const unsigned bench_calls = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 200000u;
    s_only = argc > 4 ? argv[4] : NULL;
    /* Fifth and sixth: the first case to run and the generator's state then
     * (a mismatch's report gives both), to rerun one case. */
    s_first = argc > 5 ? (unsigned)strtoul(argv[5], NULL, 10) : 0u;
    s_first_seed = argc > 6 ? (u32)strtoul(argv[6], NULL, 16) : 0u;
    Harness h;
    if (!harness_load(&h, argv[1], false, k_regions, sizeof k_regions / sizeof k_regions[0]))
        return 1;
    s_fault_images[0] = h.reference_ram;
    s_fault_images[1] = h.native_ram;
    s_fault_images[2] = bw_guest_mem1;
    module_watch_setup();
    {
        void (*set_edge)(int (*)(void*, CPUState*, u32), void*) = (void (*)(int (*)(void*, CPUState*, u32), void*))(
            void*)GetProcAddress(h.lib, "bluewake_set_edge_service");
        set_edge(draw_edge_service, NULL);
    }
    int failed = 0;
    for (unsigned which = 0; which < NATIVE_COUNT; ++which) {
        const Native* n = &k_natives[which];
        if (skip(n))
            continue;
        unsigned done = 0, stopped = 0, declined = 0;
        s_flushed = 0;
        for (unsigned i = s_first; i < cases; ++i) {
            if (i == s_first && s_first_seed != 0u)
                seed = s_first_seed;
            s_case_seed = seed;
            const unsigned host = host_scenario(i);
            set_host(host);
            s_case_index = i;
            s_phase = "build";
            Case k = build(h.native_ram, which, i);
            const int r = one_case(&h, which, i, &k, k.must_decline);
            done += r == 1;
            stopped += r == 2;
            declined += r == 0;
        }
        reset_host();
        printf("%08X %s: %u cases; plain: %u identical (%llu handing the batch over first), %u stopped, "
               "%u declined unchanged; hooked: %llu identical end to end with a native running (%llu of them "
               "stopping and resuming, %llu with the host's tags in the stream), %llu left the linked chunks; "
               "0 mismatches\n",
               n->entry, n->name, cases, done, s_flushed, stopped, declined, s_end_to_end[which], s_with_stops[which],
               s_with_tags[which], s_outside[which]);
        report_left();
        fflush(stdout);
        if (s_end_to_end[which] < 30000u && cases >= 60000u) {
            printf("  fewer than 30,000 compared cases\n");
            failed = 1;
        }
    }
    bluewake_native_draw_report();
    if (bench_calls != 0u) {
        bench(&h, bench_calls);
        bluewake_native_draw_report();
    }
    return failed;
}
