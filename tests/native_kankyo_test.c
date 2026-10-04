/* cmake/composite/native_kankyo.c against the translations it stands in for.
 *
 * Build and run from the worktree root (x64, in the Visual Studio environment):
 *
 *   clang -O2 -march=x86-64-v3 -ffp-contract=off -Icmake/composite
 *     -IE:\Github\Wind-Waker-Recomp\ref\recompcore\GXRuntime\include
 *     -IE:\Github\Wind-Waker-Recomp\ref\recompcore\Source\Core\Core\PowerPC\StaticRecomp
 *     tests/native_kankyo_test.c cmake/composite/native_kankyo.c cmake/composite/direct_calls.c
 *     E:\Github\Wind-Waker-Recomp\build\windows\app\gxruntime_build\gxruntime.lib -o native_kankyo_test.exe
 *   native_kankyo_test MODULE.dll [CASES_PER_FUNCTION=60000] [BENCH_CALLS=1000000]
 *
 * MODULE.dll is a Windows game module (gGZLE01_recomp.dll) without these
 * natives (E:\Github\Wind-Waker-Recomp\build\windows\W-final\gGZLE01_recomp.dll),
 * read, never written.
 *
 * For each function: random colour components (and random upper bits, which
 * the masks and sign extensions must drop), ratios, scales and the scene's
 * colour ratio - mostly ordinary, sometimes zeros, denormals, huge values,
 * values that overflow the conversion, infinities and NaNs, and doubles that
 * are not singles; random registers (both halves), FPSCR (its enables, VE
 * among them, so an invalid conversion writes nothing; NI, sometimes with the
 * host's flush to zero as the module arms it), reservations on the frames,
 * stacks at any alignment; and the declining cases: the magic double not the
 * usual one, frames over the constants, out of RAM, FP off, rounding not to
 * nearest, quantised pairs, an exception pending, a write journal, aliases
 * over MEM1, the budget or the deadline inside the function (and cases right
 * at the edge, which must run). Every byte of the CPU state (the cycle suffix
 * included) and of the writable pages must match - or, where the native
 * declines, nothing may have changed. RAM outside the test's pages is
 * read-only in both images.
 *
 * Then a microbenchmark: the translation through the module's dispatcher
 * against the native, ns per call. */
#include "native_kankyo.h"
#include "native4_harness.h"

#define AREA 0x80100000u
#define AREA_BYTES 0x10000u
#define ENV_PAGE 0x803E5000u
#define ALL_COL_RATIO 0x803E56B0u
#define STACK (AREA + 0x8000u)
#define MAGIC 0x4330000080000000ull

static const Region TEST_REGIONS[] = {{AREA, AREA_BYTES}, {ENV_PAGE, 0x1000u}};

static const u32 FUNCTIONS[] = {BLUEWAKE_KANKYO_S16_RATIO, BLUEWAKE_KANKYO_COLOR_RATIO, BLUEWAKE_KYEFF_S16_RATIO};
static const char* const NAMES[] = {"s16_data_ratio_set (d_kankyo)", "kankyo_color_ratio_set",
                                    "s16_data_ratio_set (d_kyeff)"};
static const s32 MAGIC_OFFSET[] = {-21080, -21080, -20784};

typedef struct Case {
    CPUState cpu;
    bool must_decline, journal, aliases, flush;
} Case;

static u32 component(void) {
    switch (below(8u)) {
    case 0: return next();                         /* upper bits too */
    case 1: return below(2u) ? 0u : 0xFFu;
    default: return below(256u) | (below(4u) == 0u ? next() & 0xFFFFFF00u : 0u);
    }
}

static f64 ratio(void) {
    switch (below(24u)) {
    case 0: return any_register(32u);
    case 1: return (f64)(f32)(unit() * 4.0e9);     /* an overflowing conversion */
    case 2: return 0.0;
    case 3: return 1.0;
    case 4: return -(f64)(f32)unit();
    default: return (f64)(f32)unit();
    }
}

static Case build(u8* ram, unsigned which, unsigned scenario) {
    Case k;
    memset(&k, 0, sizeof k);
    CPUState* c = &k.cpu;
    random_cpu(c, ram);
    for (u32 i = 0; i < AREA_BYTES; i += 4u)
        put32(ram, AREA + i, next());
    for (u32 i = 0; i < 0x1000u; i += 4u)
        put32(ram, ENV_PAGE + i, next());
    /* The constants: the magic double at r2 + offset, the colour ratio. */
    const u32 magic_at = AREA + 0x100u + 4u * below(64u);
    put64(ram, magic_at, scenario % 41u == 7u ? ((u64)next() << 32) | next() : MAGIC);
    c->gpr[2] = magic_at - (u32)MAGIC_OFFSET[which];
    put32(ram, ALL_COL_RATIO, below(8u) == 0u ? any_single(24u) : (u32)(below(3u) ? 0x3F800000u : 0x3F400000u));
    /* The stack: mostly 8-aligned, sometimes not. */
    u32 sp = STACK - 8u * below(256u);
    if (scenario % 7u == 3u)
        sp += 1u + below(7u);
    c->gpr[1] = sp;
    c->gpr[3] = component();
    c->gpr[4] = component();
    c->gpr[5] = component();
    c->gpr[6] = component();
    c->gpr[7] = below(4u) == 0u ? next() : (u32)(s32)(s16)(below(512u) - 256u);
    c->fpr[1] = ratio();
    c->fpr[2] = ratio();
    c->fpr[3] = below(6u) == 0u ? ratio() * 2.0 : (below(2u) ? 1.0 : (f64)(f32)(unit() * 2.0));
    if (below(16u) == 0u)
        c->fpr[3] = any_register(16u);
    c->pc = FUNCTIONS[which];
    c->reserve_addr = below(3u) ? sp - 8u * below(20u) : next();
    k.flush = (c->fpscr & 0x4u) != 0u && below(2u) != 0u;
    /* The declining cases, and the edges. */
    switch (scenario % 61u) {
    case 1: c->msr &= ~PPC_MSR_FP; k.must_decline = true; break;
    case 2: c->exception = 1u; k.must_decline = true; break;
    case 3: k.journal = k.must_decline = true; break;
    case 4: k.aliases = k.must_decline = true; break;
    case 5: c->fpscr = (c->fpscr & ~3u) | (1u + below(3u)); k.must_decline = true; break;
    case 6:
        if (which == 1) {
            c->gqr[0] = below(2u) ? 0x00040000u : 0x00000005u;
            k.must_decline = true;
        }
        break;
    case 7:
        if (which == 1) {
            c->hid2 &= ~PPC_HID2_LSQE;
            k.must_decline = true;
        }
        break;
    case 8: c->gpr[1] = 0x80000000u + 8u * below(4u); k.must_decline = true; break; /* the frame below RAM */
    case 9: c->gpr[1] = 0xC0108000u; k.must_decline = true; break;
    case 10: c->gpr[2] = sp - 16u - (u32)MAGIC_OFFSET[which]; /* the magic in the frame */
        put64(ram, sp - 16u, MAGIC);
        k.must_decline = true;
        break;
    case 11:
        if (which == 1) {
            c->gpr[1] = ALL_COL_RATIO + 64u; /* the colour ratio in the frame */
            k.must_decline = true;
        }
        break;
    case 12: c->downcount = -c->cycle_budget + 1 + (s64)below(160u); break; /* the budget near: either */
    case 13: c->cycle_deadline_budget = 1 + (s64)below(180u); break;         /* the deadline near: either */
    case 14: c->cycle_budget = 0; k.must_decline = true; break;
    case 15: c->downcount = 1 + (s64)below(8u); k.must_decline = true; break;
    case 16: c->downcount = -c->cycle_budget; k.must_decline = true; break;
    case 17: c->gpr[2] = 0xCC000000u - (u32)MAGIC_OFFSET[which]; k.must_decline = true; break;
    default: break;
    }
    return k;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: native_kankyo_test MODULE.dll [CASES_PER_FUNCTION] [BENCH_CALLS]\n");
        return 2;
    }
    const unsigned cases = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 60000u;
    const unsigned bench_calls = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 1000000u;
    Harness h;
    if (!harness_load(&h, argv[1], false, TEST_REGIONS, sizeof TEST_REGIONS / sizeof TEST_REGIONS[0]))
        return 1;
    bluewake_native_kankyo_enabled = 1;
    int status = 0;
    for (unsigned which = 0; which < 3u; ++which) {
        const u32 entry = FUNCTIONS[which];
        unsigned ran = 0, declined = 0, clamped_low = 0, clamped_high = 0, unwritten = 0, edges = 0;
        for (unsigned i = 0; i < cases; ++i) {
            Case k = build(h.native_ram, which, i);
            const int same = harness_case(&h, bluewake_native_kankyo, entry, NAMES[which], i, &k.cpu, k.must_decline,
                                          k.journal, k.aliases, k.flush);
            if (!same) {
                declined++;
                continue;
            }
            ran++;
            const u32 scenario = i % 61u;
            edges += scenario == 12u || scenario == 13u;
            {
                const CPUState* g = h.guest_cpu(); /* the translation's state at the end */
                if (which == 1) {
                    clamped_low += g->gpr[3] == 0u;
                    clamped_high += g->gpr[3] == 255u;
                }
                unwritten += (f64_bits(g->fpr[0]) >> 51) != 0x1FFFu; /* fctiwz wrote nothing */
            }
        }
        printf("%08X %s: %u cases, %u identical (%u at the budget or deadline edge, %u with fctiwz writing "
               "nothing", entry, NAMES[which], cases, ran, edges, unwritten);
        if (which == 1)
            printf(", %u clamped to 0, %u to 255", clamped_low, clamped_high);
        printf("), %u declined unchanged, 0 mismatches\n", declined);
        fflush(stdout);
        if (ran < 30000u && cases >= 60000u)
            status = 1; /* at least 30,000 compared cases per function */
    }

    if (bench_calls != 0u) {
        CPUState* g = h.guest_cpu();
        for (unsigned which = 0; which < 3u; ++which) {
            const u32 entry = FUNCTIONS[which];
            Case k = build(h.native_ram, which, 0u);
            CPUState base = k.cpu;
            put64(h.native_ram, base.gpr[2] + (u32)MAGIC_OFFSET[which], MAGIC);
            put32(h.native_ram, ALL_COL_RATIO, 0x3F800000u);
            base.gpr[3] = 120u;
            base.gpr[4] = 200u;
            base.gpr[5] = 30u;
            base.gpr[6] = 90u;
            base.gpr[7] = 4u;
            base.fpr[1] = 0.375;
            base.fpr[2] = 0.625;
            base.fpr[3] = 1.0;
            base.exception = 0;
            base.fpscr = 0;
            base.msr = PPC_MSR_FP;
            base.hid2 = PPC_HID2_LSQE;
            base.gqr[0] = 0;
            base.reserve_valid = false;
            base.downcount = 0;
            base.cycle_deadline_budget = 0;
            base.cycle_budget = (s64)bench_calls * 200 + 1000;
            copy_regions(h.reference_ram, h.native_ram);
            double best_t = 1e30, best_n = 1e30;
            for (unsigned round = 0; round < 5; ++round) {
                *g = base;
                g->ram = h.reference_ram;
                h.mod->on_state_loaded(g);
                double t0 = now_ns();
                for (unsigned i = 0; i < bench_calls; ++i) {
                    g->lr = RETURN_ADDRESS;
                    g->pc = entry;
                    h.mod->dispatch(g, entry);
                }
                const double t = (now_ns() - t0) / bench_calls;
                CPUState native = base;
                t0 = now_ns();
                for (unsigned i = 0; i < bench_calls; ++i) {
                    native.lr = RETURN_ADDRESS;
                    if (!bluewake_native_kankyo(&native, entry)) {
                        fprintf(stderr, "the benchmark's native declined\n");
                        return 1;
                    }
                }
                const double n = (now_ns() - t0) / bench_calls;
                if (t < best_t) best_t = t;
                if (n < best_n) best_n = n;
            }
            printf("%08X %s: translation %.1f ns/call through the dispatcher, native %.1f ns/call\n", entry,
                   NAMES[which], best_t, best_n);
        }
    }
    return status;
}
