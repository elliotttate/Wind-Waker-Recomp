/* PSMTXMultVecSR (cmake/composite/native_vec.c, bluewake_native_vec_sr)
 * against the translation it stands in for.
 *
 * Build and run from the worktree root (x64; the Visual Studio environment):
 *
 *   clang -O2 -march=x86-64-v3 -ffp-contract=off -Icmake/composite
 *     -IE:\Github\Wind-Waker-Recomp\ref\recompcore\GXRuntime\include
 *     -IE:\Github\Wind-Waker-Recomp\ref\recompcore\Source\Core\Core\PowerPC\StaticRecomp
 *     tests/native_vec_sr_test.c cmake/composite/native_vec.c
 *     E:\Github\Wind-Waker-Recomp\build\windows\app\gxruntime_build\gxruntime.lib -o native_vec_sr_test.exe
 *   native_vec_sr_test MODULE.dll [CASES=60000] [BENCH_CALLS=2000000]
 *
 * MODULE.dll is a Windows game module (gGZLE01_recomp.dll) without this
 * native; the test reads it and writes nothing but its own memory. Each case:
 * a random matrix and vector (zeros, denormals, huge values, infinities,
 * quiet and signalling NaNs, ordinary values), the output separate, on the
 * vector, on the matrix or overlapping either, unaligned addresses, random
 * registers (both paired-single halves), FPSCR (NI, rounding mode, enables,
 * sticky bits) and host rounding/flush mode, reservations, cycle state;
 * declining cases too (FP off, LSQE off, quantised GQR0, an exception
 * pending, a write journal, aliases over MEM1, ranges that leave RAM, the
 * budget spent, a deadline inside the block). The leaf runs through the
 * module's translation and through the native; every byte of the CPU state
 * (the last access's cycle suffix included) and of RAM must match - or,
 * where the native declines, nothing may have changed. RAM outside the test
 * area is read-only in both images. Then ns per call, translation through
 * the dispatcher (an empty dispatch measured for scale) against native. */
#include "native_vec.h"
#include "StaticRecompABI.h"

#include <immintrin.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define AREA 0x80100000u
#define AREA_BYTES 0x1000u
#define EMPTY_FUNCTION 0x802DB978u /* draw__9J3DPacketFv: blr */

static u32 seed = 0x3C6EF372u;
static u32 next(void) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

/* Special values (infinities, NaNs, huge ones) in about a third of the
 * cases, where the native runs the translation's statements on the
 * registers; the rest bounded, where it runs on values. */
static bool s_specials;
static u32 float_bits(void) {
    const u32 sign = next() & 0x80000000u;
    switch (next() % (s_specials ? 24u : 64u)) {
    case 0: return sign;
    case 1: return sign | (next() & 0x007FFFFFu);                            /* denormal */
    case 2: return s_specials ? sign | 0x7F800000u : sign;                   /* infinity */
    case 3: return s_specials ? sign | 0x7FC00000u | (next() & 0x003FFFFFu) : 0x3F800000u; /* quiet NaN */
    case 4: return s_specials ? sign | 0x7F800000u | (1u + next() % 0x003FFFFFu) : 0xBF800000u; /* signalling NaN */
    case 5: return sign | ((s_specials ? 200u + next() % 54u : 180u + next() % 9u) << 23) |
                   (next() & 0x007FFFFFu); /* huge, or just under the bound */
    case 6: return sign | ((1u + next() % 30u) << 23) | (next() & 0x007FFFFFu);   /* tiny */
    case 7: return 0x3F800000u;
    default: return sign | ((110u + next() % 30u) << 23) | (next() & 0x007FFFFFu);
    }
}

static f64 register_value(void) {
    switch (next() % 4u) {
    case 0: return f64_value(((u64)next() << 32) | next());
    case 1: return 1.0 / 3.0;
    default: return f64_value(convert_to_double(float_bits()));
    }
}

static void journal(u32 offset, u32 size, void* user) {
    (void)offset;
    (void)size;
    (void)user;
    abort();
}

static u8* protect_image(u8* p) {
    DWORD old;
    if (p == NULL || !VirtualProtect(p, GC_MAIN_RAM_SIZE, PAGE_READONLY, &old) ||
        !VirtualProtect(p + (AREA - GC_RAM_BASE), AREA_BYTES, PAGE_READWRITE, &old))
        return NULL;
    return p;
}

static u8* module_image(HMODULE lib) {
    u8* (*mem1)(u32*) = (u8* (*)(u32*))(void*)GetProcAddress(lib, "bluewake_composite_guest_mem1");
    u32 size = 0;
    u8* ram = mem1 != NULL ? mem1(&size) : NULL;
    if (ram == NULL)
        ram = VirtualAlloc(NULL, GC_MAIN_RAM_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    else if (size < GC_MAIN_RAM_SIZE)
        return NULL;
    else
        memset(ram, 0, GC_MAIN_RAM_SIZE);
    return protect_image(ram);
}

typedef struct Case {
    CPUState cpu;
    u32 mxcsr;
    bool journal, aliases, must_decline;
} Case;

static Case build(u8* ram, unsigned scenario) {
    Case k;
    memset(&k, 0, sizeof k);
    s_specials = scenario % 3u == 2u;
    for (u32 i = 0; i < AREA_BYTES; i += 4u)
        write_be32(ram + (AREA - GC_RAM_BASE) + i, float_bits());
    CPUState* c = &k.cpu;
    c->ram = ram;
    c->ram_size = GC_MAIN_RAM_SIZE;
    for (unsigned r = 0; r < 32; ++r) {
        c->gpr[r] = next();
        c->fpr[r] = register_value();
        c->ps1[r] = register_value();
    }
    const u32 m = AREA + 4u * (next() % 128u), v = AREA + 0x400u + 4u * (next() % 128u);
    u32 out = AREA + 0x800u + 4u * (next() % 128u);
    switch (scenario % 9u) {
    case 1: out = v; break;
    case 2: out = m + 4u * (next() % 12u); break;
    case 3: out = v + 4u; break;
    case 4: out = v - 4u; break;
    case 5: out = m + 40u; break;
    default: break;
    }
    c->gpr[3] = m;
    c->gpr[4] = v;
    c->gpr[5] = out;
    if (scenario % 11u == 3u) {
        c->gpr[3] += 1u + next() % 3u; /* unaligned: plain RAM accesses too */
        c->gpr[5] += 2u;
    }
    c->lr = 0xFFFFFFFCu | (next() & 3u);
    c->pc = BLUEWAKE_PSMTX_MULT_VEC_SR;
    c->ctr = next();
    c->cr = next();
    c->xer = next();
    c->msr = PPC_MSR_FP | (next() & ~(PPC_MSR_FP | PPC_MSR_EE));
    c->hid2 = PPC_HID2_LSQE | (next() & 0x0FFFFFFFu);
    for (unsigned g = 1; g < 8; ++g)
        c->gqr[g] = next();
    c->gqr[0] = scenario % 3u == 0u ? 0x3F003F00u & next() : 0u; /* scales without a type: unquantised */
    c->fpscr = next() & ~0x60000000u;
    c->reserve_valid = (scenario & 1u) != 0u;
    c->reserve_addr = (scenario % 4u == 1u ? out : out + 32u) & ~31u;
    c->cycle_observation_suffix = next();
    c->cycle_budget = 16384;
    c->downcount = -(s64)(next() % 64u);
    c->cycle_deadline_budget = scenario % 4u == 0u ? 0 : 100000;
    /* The host's rounding and flush mode, as the guest's mtfsf would arm it. */
    static const u32 modes[] = {0x1F80u, 0x3F80u, 0x5F80u, 0x7F80u, 0x9FC0u};
    k.mxcsr = scenario % 5u == 2u ? modes[next() % 5u] : 0x1F80u;
    switch (scenario % 37u) {
    case 1: c->msr &= ~PPC_MSR_FP; k.must_decline = true; break;
    case 2: c->hid2 &= ~PPC_HID2_LSQE; k.must_decline = true; break;
    case 3: c->gqr[0] = 0x00040000u; k.must_decline = true; break;
    case 4: c->gqr[0] = 0x00000007u; k.must_decline = true; break;
    case 5: c->exception = 1u; k.must_decline = true; break;
    case 6: k.journal = k.must_decline = true; break;
    case 7: k.aliases = k.must_decline = true; break;
    case 8: c->gpr[3] = 0xCC000000u; k.must_decline = true; break;
    case 9: c->gpr[4] |= 0x40000000u; k.must_decline = true; break;
    case 10: c->gpr[5] = GC_RAM_BASE + GC_MAIN_RAM_SIZE - 8u; k.must_decline = true; break;
    case 11: c->downcount = -c->cycle_budget; k.must_decline = true; break;
    case 12: c->cycle_deadline_budget = 1 + (s64)(next() % 20u); k.must_decline = true; break;
    case 13: c->cycle_deadline_budget = 21 - c->downcount - 1 - (s64)(next() % 3u); k.must_decline = true; break;
    case 14: c->cycle_deadline_budget = 21 - c->downcount; break; /* exactly enough */
    case 15: c->cycle_budget = 0; k.must_decline = true; break;
    case 16: c->downcount = -c->cycle_budget + 1; break; /* one cycle left: the block still runs */
    default: break;
    }
    return k;
}

static void report_cpu(const CPUState* got, const CPUState* want) {
    for (unsigned b = 0; b < sizeof *got; ++b)
        if (((const u8*)got)[b] != ((const u8*)want)[b])
            fprintf(stderr, "  CPU byte %u: got %02X want %02X\n", b, ((const u8*)got)[b], ((const u8*)want)[b]);
}

static double now_ns(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1e9 / (double)freq.QuadPart;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: native_vec_sr_test MODULE.dll [CASES] [BENCH_CALLS]\n");
        return 2;
    }
    _putenv_s("BLUEWAKE_NATIVE_MATH", "0");
    const unsigned cases = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 60000u;
    const unsigned bench_calls = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 2000000u;
    HMODULE lib = LoadLibraryA(argv[1]);
    if (lib == NULL) {
        fprintf(stderr, "cannot load %s (%lu)\n", argv[1], GetLastError());
        return 1;
    }
    StaticRecompGetModuleFn get = (StaticRecompGetModuleFn)(void*)GetProcAddress(lib, STATICRECOMP_GET_MODULE_SYMBOL);
    CPUState* (*guest_cpu)(void) = (CPUState * (*)(void))(void*)GetProcAddress(lib, "bluewake_composite_guest_cpu");
    if (get == NULL || guest_cpu == NULL) {
        fprintf(stderr, "not a BlueWake Windows module\n");
        return 1;
    }
    const StaticRecompModuleDesc* mod = get();
    if (mod->cpu_state_size != sizeof(CPUState) || strcmp(mod->game_id, "GZLE01") != 0) {
        fprintf(stderr, "CPU state size %u, expected %u\n", mod->cpu_state_size, (unsigned)sizeof(CPUState));
        return 1;
    }
    u8* reference_ram = module_image(lib);
    u8* native_ram = protect_image(VirtualAlloc(NULL, GC_MAIN_RAM_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    u8* before = malloc(AREA_BYTES);
    if (reference_ram == NULL || native_ram == NULL || before == NULL)
        return 1;
    const u32 default_mxcsr = _mm_getcsr();
    unsigned ran = 0, declined = 0, nonfinite = 0;
    for (unsigned i = 0; i < cases; ++i) {
        Case k = build(native_ram, i);
        ppc_fpscr_updated(&k.cpu);
        memcpy(before, native_ram + (AREA - GC_RAM_BASE), AREA_BYTES);
        CPUState native = k.cpu;
        const CPUState untouched = native;
        _mm_setcsr(k.mxcsr);
        if (k.journal)
            g_mem_write_journal = journal;
        if (k.aliases)
            g_ppc_guest_aliases_overlap_mem1 = true;
        const int accepted = bluewake_native_vec_sr(&native);
        g_mem_write_journal = NULL;
        g_ppc_guest_aliases_overlap_mem1 = false;
        _mm_setcsr(default_mxcsr);
        if (!accepted) {
            declined++;
            if (memcmp(&native, &untouched, sizeof native) != 0 ||
                memcmp(before, native_ram + (AREA - GC_RAM_BASE), AREA_BYTES) != 0) {
                fprintf(stderr, "case %u: declined but changed state\n", i);
                report_cpu(&native, &untouched);
                return 1;
            }
            continue;
        }
        if (k.must_decline) {
            fprintf(stderr, "case %u: ran where it must decline\n", i);
            return 1;
        }
        ran++;
        for (unsigned r = 8; r <= 13; ++r)
            nonfinite += !(native.fpr[r] - native.fpr[r] == 0.0);

        memcpy(reference_ram + (AREA - GC_RAM_BASE), before, AREA_BYTES);
        CPUState* g = guest_cpu();
        *g = k.cpu;
        g->ram = reference_ram;
        mod->on_state_loaded(g);
        _mm_setcsr(k.mxcsr);
        const int dispatched = mod->dispatch(g, BLUEWAKE_PSMTX_MULT_VEC_SR);
        _mm_setcsr(default_mxcsr);
        if (!dispatched) {
            fprintf(stderr, "case %u: the translation did not run\n", i);
            return 1;
        }
        CPUState reference = *g;
        reference.ram = native.ram;
        if (memcmp(&native, &reference, sizeof native) != 0 ||
            memcmp(native_ram + (AREA - GC_RAM_BASE), reference_ram + (AREA - GC_RAM_BASE), AREA_BYTES) != 0) {
            fprintf(stderr, "case %u (seed %08X): mismatch (fpr at %u, ps1 at %u)\n", i, seed,
                    (unsigned)offsetof(CPUState, fpr), (unsigned)offsetof(CPUState, ps1));
            report_cpu(&native, &reference);
            for (u32 b = 0; b < AREA_BYTES; ++b)
                if (native_ram[AREA - GC_RAM_BASE + b] != reference_ram[AREA - GC_RAM_BASE + b])
                    fprintf(stderr, "  RAM %08X: got %02X want %02X\n", AREA + b, native_ram[AREA - GC_RAM_BASE + b],
                            reference_ram[AREA - GC_RAM_BASE + b]);
            return 1;
        }
    }
    printf("%08X: %u cases, %u identical (%u results not finite), %u declined unchanged, 0 mismatches\n",
           BLUEWAKE_PSMTX_MULT_VEC_SR, cases, ran, nonfinite, declined);
    fflush(stdout);
    if (ran < 30000u && cases >= 60000u)
        return 1; /* at least 30,000 compared cases */

    if (bench_calls != 0u) {
        CPUState base = build(native_ram, 6).cpu;
        for (u32 j = 0; j < 12u; ++j) {
            const f32 value = (f32)(0.25 + 0.125 * j);
            u32 bits;
            memcpy(&bits, &value, 4);
            write_be32(native_ram + (base.gpr[3] - GC_RAM_BASE) + 4u * j, bits);
        }
        for (u32 j = 0; j < 3u; ++j) {
            const f32 value = (f32)(1.5 - j);
            u32 bits;
            memcpy(&bits, &value, 4);
            write_be32(native_ram + (base.gpr[4] - GC_RAM_BASE) + 4u * j, bits);
        }
        base.fpscr = 0;
        base.downcount = 0;
        base.cycle_deadline_budget = 0;
        base.cycle_budget = (s64)bench_calls * 64 + 1000;
        memcpy(reference_ram + (AREA - GC_RAM_BASE), native_ram + (AREA - GC_RAM_BASE), AREA_BYTES);
        CPUState* g = guest_cpu();
        *g = base;
        g->ram = reference_ram;
        mod->on_state_loaded(g);
        double t0 = now_ns();
        for (unsigned i = 0; i < bench_calls; ++i) {
            g->pc = EMPTY_FUNCTION;
            mod->dispatch(g, EMPTY_FUNCTION);
        }
        const double empty = (now_ns() - t0) / bench_calls;
        double best_t = 1e30, best_n = 1e30;
        for (unsigned round = 0; round < 5; ++round) {
            *g = base;
            g->ram = reference_ram;
            mod->on_state_loaded(g);
            t0 = now_ns();
            for (unsigned i = 0; i < bench_calls; ++i) {
                g->pc = BLUEWAKE_PSMTX_MULT_VEC_SR;
                mod->dispatch(g, BLUEWAKE_PSMTX_MULT_VEC_SR);
            }
            const double t = (now_ns() - t0) / bench_calls;
            CPUState native = base;
            t0 = now_ns();
            for (unsigned i = 0; i < bench_calls; ++i)
                if (!bluewake_native_vec_sr(&native))
                    return 1;
            const double n = (now_ns() - t0) / bench_calls;
            if (t < best_t) best_t = t;
            if (n < best_n) best_n = n;
        }
        printf("%08X: translation %.1f ns/call through the dispatcher (an empty dispatch %.1f ns), native %.1f "
               "ns/call\n",
               BLUEWAKE_PSMTX_MULT_VEC_SR, best_t, empty, best_n);
    }
    return 0;
}
