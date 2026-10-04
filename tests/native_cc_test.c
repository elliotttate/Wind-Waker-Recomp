/* cmake/composite/native_cc.c against the translations it stands in for.
 *
 * Build and run from the worktree root (x64, in the Visual Studio environment):
 *
 *   clang -O2 -march=x86-64-v3 -ffp-contract=off -Icmake/composite
 *     -IE:\Github\Wind-Waker-Recomp\ref\recompcore\GXRuntime\include
 *     -IE:\Github\Wind-Waker-Recomp\ref\recompcore\Source\Core\Core\PowerPC\StaticRecomp
 *     tests/native_cc_test.c cmake/composite/native_cc.c cmake/composite/direct_calls.c
 *     E:\Github\Wind-Waker-Recomp\build\windows\app\gxruntime_build\gxruntime.lib -o native_cc_test.exe
 *   native_cc_test MODULE.dll [CASES_PER_FUNCTION=60000] [BENCH_CALLS=1000000]
 *
 * Built with -DNATIVE4_HOOKED=1 and the hooked chunks (tests/native4_harness.h
 * says how), every case the native runs also runs through the hooked chunks,
 * natives on and off, against the same translation.
 *
 * MODULE.dll is a Windows game module (gGZLE01_recomp.dll) without this
 * native (E:\Github\Wind-Waker-Recomp\build\windows\W-final\gGZLE01_recomp.dll),
 * read, never written.
 *
 * CalcDivideInfoOverArea: random areas and boxes: the area's minimum and inverse scaled differences
 * and each axis's zero flag, boxes inside, across, before and beyond the
 * area (so every clamp and empty-run path runs), indices beyond the shifts'
 * range, and now and then zeros, denormals, huge values, values that overflow
 * the conversion, infinities and NaNs; random registers (both halves), FPSCR
 * (enables, VE among them, so an invalid conversion writes nothing; NI, with
 * the host's flush to zero sometimes), reservations; and the declining cases:
 * the area, the box, the frame or the info word out of RAM, the area or the
 * box over the frame, FP off, rounding not to nearest, an exception pending,
 * a write journal, aliases over MEM1, the budget or the deadline inside the
 * function (and cases right at the edge, which must run). Every byte of the
 * CPU state (the cycle suffix included) and of the writable pages must match
 * - or, where the native declines, nothing may have changed. RAM outside the
 * test's pages is read-only in both images.
 *
 * cM3dGCyl::SetC: positions of every class (normal, zero, denormal, and
 * the NaNs and values beyond the bounds the asserts catch, which decline),
 * the bounds r2 reaches, the centre and the position over each other or the
 * frame, the same machine state and declining cases.
 *
 * Then microbenchmarks: the translation through the module's dispatcher
 * against the native, ns per call. */
#include "native_cc.h"
#include "native4_harness.h"

#define AREA 0x80100000u
#define AREA_BYTES 0x10000u
#define DIVIDE (AREA + 0x100u)
#define BOX (AREA + 0x200u)
#define INFO (AREA + 0x300u)
#define STACK (AREA + 0x8000u)

static const Region TEST_REGIONS[] = {{AREA, AREA_BYTES}};

typedef struct Case {
    CPUState cpu;
    bool must_decline, journal, aliases, flush;
} Case;

static u32 bits_of(f32 v) {
    u32 b;
    memcpy(&b, &v, 4);
    return b;
}

static u32 value(f32 ordinary, unsigned specials) {
    return below(8u) == 0u ? any_single(specials) : bits_of(ordinary);
}

static Case build(u8* ram, unsigned scenario) {
    Case k;
    memset(&k, 0, sizeof k);
    CPUState* c = &k.cpu;
    random_cpu(c, ram);
    for (u32 i = 0; i < AREA_BYTES; i += 4u)
        put32(ram, AREA + i, next());
    const unsigned specials = scenario % 7u == 3u ? 24u : 0u;
    const u32 area = DIVIDE + 4u * below(16u), box = BOX + 4u * below(16u);
    for (unsigned axis = 0; axis < 3u; ++axis) {
        const f32 minimum = (f32)((unit() - 0.5) * 20000.0), cells = axis == 1u ? 10.0f : 11.0f;
        const f32 size = (f32)(100.0 + unit() * 10000.0);
        f32 inverse = cells / size;
        if (below(16u) == 0u)
            inverse = (f32)((unit() - 0.3) * 1e9);  /* indices beyond the shifts */
        put32(ram, area + 4u * axis, value(minimum, specials));
        put32(ram, area + 36u + 12u * axis, value(inverse, specials));
        ram[area + 28u + 12u * axis - GC_RAM_BASE] = (u8)(below(6u) == 0u ? 1u + below(255u) : 0u);
        /* The box: inside, across an end, before or beyond, or wide. */
        f32 lo = minimum + (f32)(unit() * size), hi = lo + (f32)(unit() * size * 0.3);
        switch (below(6u)) {
        case 0: lo = minimum - (f32)(unit() * size); break;
        case 1: hi = minimum + size + (f32)(unit() * size); break;
        case 2: lo = minimum - size * 2.0f, hi = minimum - size; break;
        case 3: lo = minimum + size * 2.0f, hi = lo + size; break;
        default: break;
        }
        put32(ram, box + 4u * axis, value(lo, specials));
        put32(ram, box + 12u + 4u * axis, value(hi, specials));
    }
    u32 sp = STACK - 8u * below(64u);
    if (scenario % 9u == 4u)
        sp += 4u;
    c->gpr[1] = sp;
    c->gpr[3] = area;
    c->gpr[4] = INFO + 4u * below(16u);
    c->gpr[5] = box;
    c->pc = BLUEWAKE_CC_DIVIDE_OVER_AREA;
    c->reserve_addr = below(2u) ? sp - 8u * below(5u) : c->gpr[4];
    k.flush = (c->fpscr & 0x4u) != 0u && below(2u) != 0u;
    switch (scenario % 53u) {
    case 1: c->msr &= ~PPC_MSR_FP; k.must_decline = true; break;
    case 2: c->exception = 1u; k.must_decline = true; break;
    case 3: k.journal = k.must_decline = true; break;
    case 4: k.aliases = k.must_decline = true; break;
    case 5: c->fpscr = (c->fpscr & ~3u) | (1u + below(3u)); k.must_decline = true; break;
    case 6: c->gpr[1] = 0x80000000u + 4u * below(6u); k.must_decline = true; break; /* the frame below RAM */
    case 7: c->gpr[4] = 0xCC000000u; k.must_decline = true; break;
    case 8: c->gpr[3] = area | 0x40000000u; k.must_decline = true; break;
    case 9: c->gpr[5] = 0xCC000000u; k.must_decline = true; break;
    case 10: c->gpr[5] = sp - 24u; k.must_decline = true; break; /* the box on the frame */
    case 11: c->gpr[3] = sp - 64u; k.must_decline = true; break; /* the area on the frame */
    case 12: c->gpr[4] = sp - 16u; break;                       /* the info word on a slot: runs */
    case 13: c->downcount = -c->cycle_budget + 1 + (s64)below(120u); break;
    case 14: c->cycle_deadline_budget = 1 + (s64)below(140u); break;
    case 15: c->cycle_budget = 0; k.must_decline = true; break;
    case 16: c->downcount = 1 + (s64)below(8u); k.must_decline = true; break;
    default: break;
    }
    return k;
}

/* cM3dGCyl::SetC: a cylinder and a position; the bounds r2 reaches (the
 * decomp's -1e32 and 1e32, or others now and then). */
#define CYLINDER (AREA + 0x400u)
#define POSITION (AREA + 0x500u)
#define BOUNDS (AREA + 0x600u)

static Case build_set_c(u8* ram, unsigned scenario) {
    Case k;
    memset(&k, 0, sizeof k);
    CPUState* c = &k.cpu;
    random_cpu(c, ram);
    for (u32 i = 0; i < AREA_BYTES; i += 4u)
        put32(ram, AREA + i, next());
    const unsigned specials = scenario % 5u == 2u ? 16u : 0u;
    const u32 pos = POSITION + 4u * below(16u), self = CYLINDER + 4u * below(16u);
    for (unsigned i = 0; i < 3u; ++i) {
        u32 bits = value((f32)((unit() - 0.5) * 20000.0), specials);
        if (below(8u) == 0u)
            bits = (next() & 0x80000000u) | (below(2u) ? 0u : 1u + below(0x7FFFFFu)); /* a zero or a denormal */
        if (below(32u) == 0u)
            bits = (next() & 0x80000000u) | 0x7F7FFFFFu; /* beyond the bounds: the assert */
        put32(ram, pos + 4u * i, bits);
    }
    put32(ram, BOUNDS, scenario % 23u == 7u ? any_single(16u) : bits_of(-1.0e32f));
    put32(ram, BOUNDS + 4u, scenario % 23u == 9u ? any_single(16u) : bits_of(1.0e32f));
    c->gpr[2] = BOUNDS + 16368u;
    u32 sp = STACK - 8u * below(64u);
    if (scenario % 9u == 4u)
        sp += 4u;
    c->gpr[1] = sp;
    c->gpr[3] = self;
    c->gpr[4] = pos;
    c->pc = BLUEWAKE_CC_CYL_SET_C;
    c->reserve_addr = below(2u) ? sp - 8u * below(5u) : self + 4u * below(3u);
    k.flush = (c->fpscr & 0x4u) != 0u && below(2u) != 0u;
    switch (scenario % 47u) {
    case 1: c->msr &= ~PPC_MSR_FP; k.must_decline = true; break;
    case 2: c->exception = 1u; k.must_decline = true; break;
    case 3: k.journal = k.must_decline = true; break;
    case 4: k.aliases = k.must_decline = true; break;
    case 5: c->fpscr = (c->fpscr & ~3u) | (1u + below(3u)); k.must_decline = true; break;
    case 6: c->gpr[1] = 0x80000000u + 4u * below(6u); k.must_decline = true; break;
    case 7: c->gpr[3] = 0xCC000000u; k.must_decline = true; break;
    case 8: c->gpr[4] = pos | 0x40000000u; k.must_decline = true; break;
    case 9: c->gpr[4] = sp - 16u; k.must_decline = true; break;   /* the position on the frame */
    case 10: c->gpr[3] = pos + 4u; k.must_decline = true; break;  /* the centre over the position */
    case 11: put32(ram, pos + 4u * below(3u), 0x7FC00000u | next()); k.must_decline = true; break; /* a NaN */
    case 12: c->downcount = -c->cycle_budget + 1 + (s64)below(120u); break;
    case 13: c->cycle_deadline_budget = 1 + (s64)below(140u); break;
    case 14: c->cycle_budget = 0; k.must_decline = true; break;
    default: break;
    }
    return k;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: native_cc_test MODULE.dll [CASES] [BENCH_CALLS]\n");
        return 2;
    }
    const unsigned cases = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 60000u;
    const unsigned bench_calls = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 1000000u;
    Harness h;
    if (!harness_load(&h, argv[1], false, TEST_REGIONS, sizeof TEST_REGIONS / sizeof TEST_REGIONS[0]))
        return 1;
    bluewake_native_cc_enabled = 1;
    unsigned ran = 0, declined = 0, edges = 0, full = 0, empty = 0, unwritten = 0;
    for (unsigned i = 0; i < cases; ++i) {
        Case k = build(h.native_ram, i);
        reset_host();
        const int same = harness_case(&h, bluewake_native_cc, BLUEWAKE_CC_DIVIDE_OVER_AREA,
                                      "CalcDivideInfoOverArea", i, &k.cpu, k.must_decline, k.journal, k.aliases,
                                      k.flush);
        if (!same) {
            declined++;
            continue;
        }
        ran++;
        edges += i % 53u == 13u || i % 53u == 14u;
        const CPUState* g = h.guest_cpu();
        const u32 info = g->gpr[8];
        full += info == 0xFFFFFFFFu;
        empty += (info & 0x7FFu) == 0u || (info & 0x1FF800u) == 0u || (info & 0xFFE00000u) == 0u;
        unwritten += (f64_bits(g->fpr[0]) >> 51) != 0x1FFFu;
    }
    printf("8024170C CalcDivideInfoOverArea: %u cases, %u identical (%u at the budget or deadline edge, %u with every "
           "cell, %u with an axis empty, %u with fctiwz writing nothing last), %u declined unchanged, 0 mismatches\n",
           cases, ran, edges, full, empty, unwritten, declined);
    int status = ran < 30000u && cases >= 60000u;

    unsigned set_ran = 0, set_declined = 0, set_edges = 0, set_kinds[6] = {0};
    for (unsigned i = 0; i < cases; ++i) {
        Case k = build_set_c(h.native_ram, i);
        reset_host();
        const int same = harness_case(&h, bluewake_native_cc, BLUEWAKE_CC_CYL_SET_C, "cM3dGCyl::SetC", i, &k.cpu,
                                      k.must_decline, k.journal, k.aliases, k.flush);
        if (!same) {
            set_declined++;
            continue;
        }
        set_ran++;
        set_edges += i % 47u == 12u || i % 47u == 13u;
        for (unsigned j = 0; j < 3u; ++j) {
            const u32 w = get32(h.native_ram, k.cpu.gpr[4] + 4u * j);
            set_kinds[(w & 0x7F800000u) == 0u ? ((w & 0x7FFFFFu) == 0u ? 3u : 5u) : 4u]++;
        }
    }
    printf("80251D88 cM3dGCyl::SetC: %u cases, %u identical (%u at the budget or deadline edge; components: %u "
           "normal, %u zero, %u denormal), %u declined unchanged, 0 mismatches\n",
           cases, set_ran, set_edges, set_kinds[4], set_kinds[3], set_kinds[5], set_declined);
    status |= set_ran < 30000u && cases >= 60000u;

    if (bench_calls != 0u) {
        seed = 0x6A09E667u;
        Case k = build(h.native_ram, 0u);
        CPUState base = k.cpu;
        /* An ordinary box inside an ordinary area. */
        for (unsigned axis = 0; axis < 3u; ++axis) {
            put32(h.native_ram, base.gpr[3] + 4u * axis, bits_of(-1000.0f));
            put32(h.native_ram, base.gpr[3] + 36u + 12u * axis, bits_of(0.005f));
            h.native_ram[base.gpr[3] + 28u + 12u * axis - GC_RAM_BASE] = 0u;
            put32(h.native_ram, base.gpr[5] + 4u * axis, bits_of(-200.0f + 100.0f * (f32)axis));
            put32(h.native_ram, base.gpr[5] + 12u + 4u * axis, bits_of(150.0f + 100.0f * (f32)axis));
        }
        base.exception = 0;
        base.fpscr = 0;
        base.msr = PPC_MSR_FP;
        base.reserve_valid = false;
        base.downcount = 0;
        base.cycle_deadline_budget = 0;
        base.cycle_budget = (s64)bench_calls * 400 + 1000;
        copy_regions(h.reference_ram, h.native_ram);
        CPUState* g = h.guest_cpu();
        double best_t = 1e30, best_n = 1e30;
        for (unsigned round = 0; round < 5; ++round) {
            *g = base;
            g->ram = h.reference_ram;
            h.mod->on_state_loaded(g);
            double t0 = now_ns();
            for (unsigned i = 0; i < bench_calls; ++i) {
                g->gpr[1] = base.gpr[1];
                g->gpr[3] = base.gpr[3];
                g->gpr[4] = base.gpr[4];
                g->gpr[5] = base.gpr[5];
                g->lr = RETURN_ADDRESS;
                g->pc = BLUEWAKE_CC_DIVIDE_OVER_AREA;
                h.mod->dispatch(g, BLUEWAKE_CC_DIVIDE_OVER_AREA);
            }
            const double t = (now_ns() - t0) / bench_calls;
            CPUState native = base;
            t0 = now_ns();
            for (unsigned i = 0; i < bench_calls; ++i) {
                native.gpr[3] = base.gpr[3];
                native.gpr[5] = base.gpr[5];
                native.lr = RETURN_ADDRESS;
                if (!bluewake_native_cc(&native, BLUEWAKE_CC_DIVIDE_OVER_AREA)) {
                    fprintf(stderr, "the benchmark's native declined\n");
                    return 1;
                }
            }
            const double n = (now_ns() - t0) / bench_calls;
            if (t < best_t) best_t = t;
            if (n < best_n) best_n = n;
        }
        printf("8024170C CalcDivideInfoOverArea: translation %.1f ns/call through the dispatcher, native %.1f ns/call\n",
               best_t, best_n);

        seed = 0xBB67AE85u;
        k = build_set_c(h.native_ram, 0u);
        base = k.cpu;
        for (unsigned i = 0; i < 3u; ++i)
            put32(h.native_ram, base.gpr[4] + 4u * i, bits_of(100.0f * (f32)(i + 1u)));
        put32(h.native_ram, BOUNDS, bits_of(-1.0e32f));
        put32(h.native_ram, BOUNDS + 4u, bits_of(1.0e32f));
        base.exception = 0;
        base.fpscr = 0;
        base.msr = PPC_MSR_FP;
        base.reserve_valid = false;
        base.downcount = 0;
        base.cycle_deadline_budget = 0;
        base.cycle_budget = (s64)bench_calls * 400 + 1000;
        copy_regions(h.reference_ram, h.native_ram);
        best_t = best_n = 1e30;
        for (unsigned round = 0; round < 5; ++round) {
            *g = base;
            g->ram = h.reference_ram;
            h.mod->on_state_loaded(g);
            double t0 = now_ns();
            for (unsigned i = 0; i < bench_calls; ++i) {
                g->gpr[1] = base.gpr[1];
                g->gpr[3] = base.gpr[3];
                g->gpr[4] = base.gpr[4];
                g->lr = RETURN_ADDRESS;
                g->pc = BLUEWAKE_CC_CYL_SET_C;
                h.mod->dispatch(g, BLUEWAKE_CC_CYL_SET_C);
            }
            const double t = (now_ns() - t0) / bench_calls;
            CPUState native = base;
            t0 = now_ns();
            for (unsigned i = 0; i < bench_calls; ++i) {
                native.gpr[3] = base.gpr[3];
                native.gpr[4] = base.gpr[4];
                native.lr = RETURN_ADDRESS;
                if (!bluewake_native_cc(&native, BLUEWAKE_CC_CYL_SET_C)) {
                    fprintf(stderr, "the benchmark's native declined\n");
                    return 1;
                }
            }
            const double n = (now_ns() - t0) / bench_calls;
            if (t < best_t) best_t = t;
            if (n < best_n) best_n = n;
        }
        printf("80251D88 cM3dGCyl::SetC: translation %.1f ns/call through the dispatcher, native %.1f ns/call\n", best_t,
               best_n);
    }
    return status;
}
