/* cmake/composite/native_bg.c against the translations it stands in for.
 *
 * Build and run from the worktree root (x64; the Visual Studio environment):
 *
 *   clang -O2 -march=x86-64-v3 -ffp-contract=off -Icmake/composite
 *     -IE:\Github\Wind-Waker-Recomp\ref\recompcore\GXRuntime\include
 *     -IE:\Github\Wind-Waker-Recomp\ref\recompcore\Source\Core\Core\PowerPC\StaticRecomp
 *     tests/native_bg_test.c cmake/composite/native_bg.c cmake/composite/direct_calls.c
 *     E:\Github\Wind-Waker-Recomp\build\windows\app\gxruntime_build\gxruntime.lib -o native_bg_test.exe
 *   native_bg_test MODULE.dll [CASES_PER_FUNCTION=60000] [BENCH_CALLS=2000000]
 *
 * MODULE.dll is a Windows game module (gGZLE01_recomp.dll) without these
 * natives; the test reads it and writes nothing but its own memory. The
 * module runs as in play: direct calls and its edge filter on, the host's
 * flags quiet, an edge service that ends the run at the return address (and
 * fails the test if it is asked anything anywhere else). For each function:
 * random registers, flags and cycle state; random collision data in RAM (the
 * actor ids, the same-actor flag; ChkGrpThrough's background data, polygon
 * group table, group info and pass-check flags, driving every one of its
 * return paths and both of its ways into the next chunk), pointers that leave
 * RAM at each level, budgets spent at each block, deadlines inside blocks,
 * an exception pending, a write journal, aliases over MEM1; and for the
 * chunk boundary, the host not quiet (sources dirty, a decrementer or
 * processor interrupt the guest would take), the edge filter off, the
 * boundary watched. Every byte of the CPU state (the cycle suffix included)
 * and of RAM must match - or, where the native declines, nothing may have
 * changed. RAM outside the test area is read-only in both images.
 *
 * Then a microbenchmark: the translation through the module's dispatcher
 * (a one-instruction function's dispatch is measured for scale) against the
 * native, ns per call, on the common paths. */
#include "native_bg.h"
#include "direct_calls.h"
#include "StaticRecompABI.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* direct_calls.c, linked in for the edge filter's state, names these. */
BwChunkFn bw_find_chunk(u32 address) {
    (void)address;
    return NULL;
}
BwChunkFn* const bw_chunk_fns = NULL;

#define AREA 0x80100000u
#define AREA_BYTES 0x4000u
#define EMPTY_FUNCTION 0x802DB978u /* draw__9J3DPacketFv: blr */
#define RETURN_ADDRESS 0xFFFFFFFCu

static u32 seed = 0x5A17C0DEu;
static u32 next(void) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static void put(u8* ram, u32 address, u32 value) { write_be32(ram + (address - GC_RAM_BASE), value); }

static void journal(u32 offset, u32 size, void* user) {
    (void)offset;
    (void)size;
    (void)user;
    abort();
}

static unsigned s_unexpected_service;
static int edge_service(void* user, CPUState* cpu, u32 address) {
    (void)user;
    (void)cpu;
    if ((address & ~3u) != RETURN_ADDRESS)
        s_unexpected_service++;
    return 1;
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

/* The test's own host flags and watch list, the ones the native reads. */
static bool s_sources_dirty, s_decrementer_pending;
static u32 s_pi_cause, s_pi_mask;

static void watch(u32 address) {
    const u32 canonical = address & ~0x40000000u;
    u32 slot = (canonical * 0x9E3779B1u) >> 20;
    while (bw_edge_watch_table[slot] != 0u && bw_edge_watch_table[slot] != canonical)
        slot = (slot + 1u) & (BW_EDGE_WATCH_SLOTS - 1u);
    bw_edge_watch_table[slot] = canonical;
}

static void reset_host(void) {
    memset(bw_edge_watch_table, 0, sizeof bw_edge_watch_table);
    /* Some watched addresses, as the builder's list has. */
    watch(0x80006000u);
    watch(0x800A9000u);
    watch(0x8024734Cu + 0x100u);
    bw_edge_watch_ready = true;
    bw_edge_filter_enabled = true;
    bw_direct_enabled = true;
    s_sources_dirty = s_decrementer_pending = false;
    s_pi_cause = s_pi_mask = 0u;
    bw_host_sources_dirty = &s_sources_dirty;
    bw_host_decrementer_pending = &s_decrementer_pending;
    bw_host_pi_cause = &s_pi_cause;
    bw_host_pi_mask = &s_pi_mask;
}

static const u32 FUNCTIONS[] = {BLUEWAKE_BG_CHK_SAME_ACTOR_PID, BLUEWAKE_BG_CHK_GRP_THROUGH};

typedef struct Case {
    CPUState cpu;
    bool journal, aliases, boundary_case;
} Case;

static u32 group_info(void) {
    static const u32 bits[] = {0x100u, 0x200u, 0x400u, 0x80000u, 0x700u, 0x80700u, 0u, 0xFFFFFFFFu};
    u32 info = next() & next() & ~0x80700u;
    for (unsigned k = next() % 4u; k-- > 0;)
        info |= bits[next() % 8u];
    return info;
}

static Case build(u8* ram, unsigned which, unsigned scenario) {
    Case k;
    memset(&k, 0, sizeof k);
    for (u32 i = 0; i < AREA_BYTES; i += 4u)
        put(ram, AREA + i, next());
    CPUState* c = &k.cpu;
    c->ram = ram;
    c->ram_size = GC_MAIN_RAM_SIZE;
    for (unsigned r = 0; r < 32; ++r) {
        c->gpr[r] = next();
        c->fpr[r] = f64_value(((u64)next() << 32) | next());
        c->ps1[r] = f64_value(((u64)next() << 32) | next());
    }
    if (which == 0) {
        /* cBgS_Chk: mActorPid at 8, mSameActorChk at 12. */
        const u32 self = AREA + 4u * (next() % 256u);
        c->gpr[3] = self;
        const u32 pid = next();
        switch (scenario % 8u) {
        case 0: put(ram, self + 8u, 0xFFFFFFFFu); break;
        case 1: c->gpr[4] = 0xFFFFFFFFu; put(ram, self + 8u, pid); break;
        case 2: c->gpr[4] = pid; put(ram, self + 8u, pid); break;
        case 3: c->gpr[4] = pid; put(ram, self + 8u, pid + 1u); break;
        case 4: c->gpr[4] = next() & 0xFF; put(ram, self + 8u, next() & 0xFF); break;
        default: c->gpr[4] = pid; put(ram, self + 8u, next() % 3u ? pid : next()); break;
        }
        ram[self + 12u - GC_RAM_BASE] = scenario % 5u == 0u ? 0u : (u8)next();
        if (scenario % 7u == 3u)
            ram[self + 12u - GC_RAM_BASE] = 0u;
        switch (scenario % 41u) {
        case 1: c->gpr[3] = 0xCC000000u; break;
        case 2: c->gpr[3] = self | 0x40000000u; break;
        case 3: c->gpr[3] = GC_RAM_BASE + GC_MAIN_RAM_SIZE - 12u + (next() % 4u); break; /* the flag past the end */
        case 4: c->gpr[3] = self + 1u + next() % 3u; break;                                 /* unaligned: plain RAM */
        default: break;
        }
    } else {
        /* dBgW: pm_bgd at 148; cBgD_t: m_g_tbl at 36; groups of 52 bytes,
         * m_info at 48; dBgS_GrpPassChk: flags at 4. */
        const u32 self = AREA + 4u * (next() % 64u);
        const u32 bgd = AREA + 0x400u + 4u * (next() % 64u);
        const u32 table = AREA + 0x800u + 4u * (next() % 16u);
        const u32 check = AREA + 0x3800u + 4u * (next() % 64u);
        put(ram, self + 148u, bgd);
        put(ram, bgd + 36u, table);
        const u32 group = next() % 64u;
        for (u32 g = 0; g < 64u; ++g)
            put(ram, table + 52u * g + 48u, group_info());
        put(ram, check + 4u, next() % 3u ? next() & 0x1Fu : next());
        c->gpr[3] = self;
        c->gpr[4] = group;
        c->gpr[5] = scenario % 13u == 1u ? 0u : check;
        c->gpr[6] = scenario % 11u == 1u ? next() % 5u : 2u;
        if (scenario % 17u == 2u)
            c->gpr[6] = 0x80000002u;
        switch (scenario % 43u) {
        case 1: put(ram, self + 148u, 0xCC000000u); break;
        case 2: put(ram, bgd + 36u, 0xC0100000u); break;
        case 3: c->gpr[4] = 0x40000000u + group; break; /* the entry outside RAM */
        case 4: c->gpr[5] = 0x90000000u; break;
        case 5: c->gpr[3] = 0xCC008000u; break;
        case 6: c->gpr[4] = (u32)-(s32)(1u + next() % 4u); break; /* a negative group, inside the area */
        default: break;
        }
        if (scenario % 43u == 6u)
            for (u32 g = 1; g <= 4u; ++g)
                put(ram, table - 52u * g + 48u, group_info());
    }
    c->lr = RETURN_ADDRESS | (next() & 3u);
    c->pc = FUNCTIONS[which];
    c->ctr = next();
    c->cr = next();
    c->xer = next();
    c->msr = next() & ~PPC_MSR_EE;
    c->hid2 = next();
    c->fpscr = next() & ~3u;
    c->reserve_valid = (scenario & 1u) != 0u;
    c->reserve_addr = c->gpr[3] & ~31u;
    c->cycle_observation_suffix = next();
    c->cycle_budget = 16384;
    c->downcount = -(s64)(next() % 64u);
    c->cycle_deadline_budget = scenario % 4u == 0u ? 0 : 100000;
    switch (scenario % 29u) {
    case 1: c->cycle_deadline_budget = 1 + (s64)(next() % 40u) - c->downcount; break; /* inside the function */
    case 2: c->cycle_deadline_budget = 1 + (s64)(next() % 12u); break;
    case 3: c->cycle_deadline_budget = -(s64)(next() % 100u); break;
    case 4: c->downcount = -c->cycle_budget; break;
    case 5: c->downcount = -c->cycle_budget + 1 + (s64)(next() % 30u); break; /* spent at a later block */
    case 6: c->cycle_budget = 1 + (s64)(next() % 30u); c->downcount = 0; break;
    case 7: c->downcount = (s64)(next() % 8u); break;
    case 8: c->exception = 1u; break;
    case 9: c->cycle_budget = 0; break;
    default: break;
    }
    k.journal = scenario % 31u == 7u;
    k.aliases = scenario % 37u == 8u;
    k.boundary_case = which == 1 && scenario % 19u == 5u;
    return k;
}

/* The ways the chunk boundary must not be crossed natively. */
static void boundary_trouble(unsigned scenario) {
    switch ((scenario / 19u) % 7u) {
    case 0: s_sources_dirty = true; break;
    case 1: s_decrementer_pending = true; break; /* with EE, below */
    case 2: s_pi_cause = s_pi_mask = 0x10u; break;
    case 3: bw_edge_filter_enabled = false; break;
    case 4: watch(0x800A96E0u); break;
    case 5: watch(0x800A96F0u); break;
    default: watch(0xC00A96E0u); break; /* the mirror form names the same address */
    }
}

/* Where ChkGrpThrough's path enters the next chunk (all its loads in RAM),
 * or 0 where it does not. */
static u32 crosses(const u8* ram, const CPUState* c) {
    if (c->gpr[6] != 2u || c->gpr[5] == 0u)
        return 0u;
    const u32 bgd = read_be32(ram + (c->gpr[3] + 148u - GC_RAM_BASE));
    const u32 table = read_be32(ram + (bgd + 36u - GC_RAM_BASE));
    const u32 info = read_be32(ram + (table + c->gpr[4] * 52u + 48u - GC_RAM_BASE));
    const u32 flags = read_be32(ram + (c->gpr[5] + 4u - GC_RAM_BASE));
    if ((info & 0x80700u) == 0u && (flags & 1u) != 0u)
        return 0u;
    return (info & 0x100u) != 0u ? 0x800A96E0u : 0x800A96F0u;
}

/* Whether the trouble boundary_trouble made for this scenario applies there. */
static bool trouble_at(unsigned scenario, u32 boundary) {
    const unsigned kind = (scenario / 19u) % 7u;
    if (boundary == 0u)
        return false;
    if (kind == 5u)
        return boundary == 0x800A96F0u;
    if (kind == 4u || kind == 6u)
        return boundary == 0x800A96E0u;
    return true;
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
        fprintf(stderr, "usage: native_bg_test MODULE.dll [CASES_PER_FUNCTION] [BENCH_CALLS]\n");
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
    void (*set_edge)(int (*)(void*, CPUState*, u32), void*) =
        (void (*)(int (*)(void*, CPUState*, u32), void*))(void*)GetProcAddress(lib, "bluewake_set_edge_service");
    int (*direct)(bool, const bool*, const bool*, const u32*, const u32*) =
        (int (*)(bool, const bool*, const bool*, const u32*, const u32*))(void*)GetProcAddress(
            lib, "bluewake_composite_direct_calls");
    int (*filter)(bool) = (int (*)(bool))(void*)GetProcAddress(lib, "bluewake_composite_edge_filter");
    if (get == NULL || guest_cpu == NULL || set_edge == NULL || direct == NULL || filter == NULL) {
        fprintf(stderr, "not a BlueWake Windows module\n");
        return 1;
    }
    const StaticRecompModuleDesc* mod = get();
    if (mod->cpu_state_size != sizeof(CPUState) || strcmp(mod->game_id, "GZLE01") != 0) {
        fprintf(stderr, "CPU state size %u, expected %u\n", mod->cpu_state_size, (unsigned)sizeof(CPUState));
        return 1;
    }
    /* The module as in play: quiet host flags, direct calls, its edge filter. */
    static const bool clear = false;
    static const u32 zero = 0u;
    if (!direct(true, &clear, &clear, &zero, &zero) || !filter(true)) {
        fprintf(stderr, "the module's direct calls or edge filter are unavailable\n");
        return 1;
    }
    set_edge(edge_service, NULL);
    u8* reference_ram = module_image(lib);
    u8* native_ram = protect_image(VirtualAlloc(NULL, GC_MAIN_RAM_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    u8* before = malloc(AREA_BYTES);
    if (reference_ram == NULL || native_ram == NULL || before == NULL)
        return 1;

    unsigned ran[2] = {0}, declined[2] = {0}, true_results[2] = {0}, through_boundary = 0, boundary_declined = 0;
    for (unsigned which = 0; which < 2; ++which) {
        const u32 entry = FUNCTIONS[which];
        for (unsigned i = 0; i < cases; ++i) {
            Case k = build(native_ram, which, i);
            memcpy(before, native_ram + (AREA - GC_RAM_BASE), AREA_BYTES);
            reset_host();
            if (k.boundary_case) {
                boundary_trouble(i);
                if ((i / 19u) % 7u == 1u || (i / 19u) % 7u == 2u) /* an interrupt the guest would take */
                    k.cpu.msr |= PPC_MSR_EE;
            }
            ppc_fpscr_updated(&k.cpu); /* as the module's on_state_loaded does (FEX, VX) */
            CPUState native = k.cpu;
            const CPUState untouched = native;
            if (k.journal)
                g_mem_write_journal = journal;
            if (k.aliases)
                g_ppc_guest_aliases_overlap_mem1 = true;
            const int accepted = bluewake_native_bg(&native, entry);
            g_mem_write_journal = NULL;
            g_ppc_guest_aliases_overlap_mem1 = false;
            if (!accepted) {
                declined[which]++;
                if (k.boundary_case)
                    boundary_declined++;
                if (memcmp(&native, &untouched, sizeof native) != 0 ||
                    memcmp(before, native_ram + (AREA - GC_RAM_BASE), AREA_BYTES) != 0) {
                    fprintf(stderr, "case %u (%08X): declined but changed state\n", i, entry);
                    report_cpu(&native, &untouched);
                    return 1;
                }
                continue;
            }
            /* (Aliases over MEM1 only matter to a path that loads.) A path
             * into the next chunk with the boundary in trouble must decline. */
            if (k.journal || k.cpu.exception || (k.boundary_case && trouble_at(i, crosses(native_ram, &k.cpu)))) {
                fprintf(stderr, "case %u (%08X): ran where it must decline\n", i, entry);
                return 1;
            }
            ran[which]++;
            true_results[which] += native.gpr[3] != 0u;
            if (native.cycle_observation_suffix == 0u && which == 1 && k.cpu.cycle_observation_suffix != 0u)
                through_boundary++;

            memcpy(reference_ram + (AREA - GC_RAM_BASE), before, AREA_BYTES);
            CPUState* g = guest_cpu();
            *g = k.cpu;
            g->ram = reference_ram;
            mod->on_state_loaded(g);
            s_unexpected_service = 0;
            if (!mod->dispatch(g, entry)) {
                fprintf(stderr, "case %u: the translation did not run\n", i);
                return 1;
            }
            CPUState reference = *g;
            reference.ram = native.ram;
            if (s_unexpected_service != 0u || (reference.pc & ~3u) != RETURN_ADDRESS ||
                memcmp(&native, &reference, sizeof native) != 0 ||
                memcmp(native_ram + (AREA - GC_RAM_BASE), reference_ram + (AREA - GC_RAM_BASE), AREA_BYTES) != 0) {
                fprintf(stderr, "case %u (%08X, seed %08X): mismatch (service asked %u times elsewhere)\n", i, entry,
                        seed, s_unexpected_service);
                report_cpu(&native, &reference);
                return 1;
            }
        }
        printf("%08X: %u cases, %u identical (%u returning true), %u declined unchanged, 0 mismatches\n", entry,
               cases, ran[which], true_results[which], declined[which]);
        if (which == 1)
            printf("%08X: %u identical cases crossed into the next chunk through 0x800A96DC; %u declined with the "
                   "boundary in trouble\n",
                   entry, through_boundary, boundary_declined);
        fflush(stdout);
        if (ran[which] < 30000u && cases >= 60000u)
            return 1; /* at least 30,000 compared cases per function */
    }

    if (bench_calls != 0u) {
        reset_host();
        CPUState* g = guest_cpu();
        double t0 = now_ns();
        for (unsigned which = 0; which < 2; ++which) {
            const u32 entry = FUNCTIONS[which];
            /* The common path: a different actor (ChkSameActorPid); a ground
             * group passing all the tests (ChkGrpThrough), and a water group
             * that crosses into the next chunk through 0x800A96DC. */
            for (unsigned variant = 0; variant < (which == 1 ? 2u : 1u); ++variant) {
                Case k = build(native_ram, which, 6);
                CPUState base = k.cpu;
                base.exception = 0;
                base.downcount = 0;
                base.cycle_deadline_budget = 0;
                base.cycle_budget = (s64)bench_calls * 64 + 1000;
                if (which == 0) {
                    put(native_ram, base.gpr[3] + 8u, 1234u);
                    base.gpr[4] = 5678u;
                    native_ram[base.gpr[3] + 12u - GC_RAM_BASE] = 1u;
                } else {
                    base.gpr[5] = AREA + 0x3800u;
                    base.gpr[6] = 2u;
                    const u32 table = read_be32(native_ram + read_be32(native_ram + base.gpr[3] + 148u - GC_RAM_BASE) +
                                                36u - GC_RAM_BASE);
                    put(native_ram, table + 52u * base.gpr[4] + 48u, variant == 0 ? 0x00000000u : 0x00000100u);
                    put(native_ram, base.gpr[5] + 4u, 0x2u);
                }
                memcpy(reference_ram + (AREA - GC_RAM_BASE), native_ram + (AREA - GC_RAM_BASE), AREA_BYTES);
                *g = base;
                g->ram = reference_ram;
                mod->on_state_loaded(g);
                t0 = now_ns();
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
                    const u32 r3 = base.gpr[3], r4 = base.gpr[4];
                    t0 = now_ns();
                    for (unsigned i = 0; i < bench_calls; ++i) {
                        g->gpr[3] = r3;
                        g->gpr[4] = r4;
                        g->pc = entry;
                        mod->dispatch(g, entry);
                    }
                    const double t = (now_ns() - t0) / bench_calls;
                    CPUState native = base;
                    t0 = now_ns();
                    for (unsigned i = 0; i < bench_calls; ++i) {
                        native.gpr[3] = r3;
                        native.gpr[4] = r4;
                        if (!bluewake_native_bg(&native, entry))
                            return 1;
                    }
                    const double n = (now_ns() - t0) / bench_calls;
                    if (t < best_t) best_t = t;
                    if (n < best_n) best_n = n;
                }
                printf("%08X%s: translation %.1f ns/call through the dispatcher (an empty dispatch %.1f ns), "
                       "native %.1f ns/call\n",
                       entry, which == 0 ? "" : variant == 0 ? " (a ground group, branching into the next chunk)" : " (a water group, through 0x800A96DC)",
                       best_t, empty, best_n);
            }
        }
    }
    return 0;
}
