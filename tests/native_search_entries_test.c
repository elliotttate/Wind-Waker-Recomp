/* The third set's hooks (scripts/windows/native_entries.py: strcmp and
 * dStage_searchName), end to end: the hooked chunks, compiled as the module
 * compiles them, against the module's unhooked translation.
 *
 * The chunks are the player's own translated sources, prepared by the
 * Windows source steps and then by native_entries.py (on a copy, never the
 * builder's tree); none is distributed. Build, from the worktree root, with
 * HOOKED = a copy of composite-src's chunks_dol after native_entries.py and
 * COMPOSITE = that composite-src (for generated.h):
 *
 *   for each chunk in 0015_text1_8003D6E0 0203_text1_8032D6E0:
 *     clang -c -O2 -march=x86-64-v3 -ffp-contract=off -fno-slp-vectorize
 *       -mllvm -large-interval-freq-threshold=10
 *       -DMODULE_GAME_ID="GZLE01" -DDOLRECOMP_CPU_HEADER="core/cpu.h"
 *       -DBW_GUEST_MEM1=bw_guest_mem1 -DBW_GUEST_MEM1_SIZE=0x02000000u
 *       -DBLUEWAKE_EDGE_FILTER=1 -DBLUEWAKE_GATHER_PIPE_BATCH=1
 *       -Icmake/composite -I%COMPOSITE% -I...GXRuntime\include -I...StaticRecomp
 *       %HOOKED%\chunk_NNNN.c -o chunk_NNNN.o
 *   clang -O2 -march=x86-64-v3 -ffp-contract=off -DBW_GUEST_MEM1=bw_guest_mem1
 *     -DBW_GUEST_MEM1_SIZE=0x02000000u -Icmake/composite -I...GXRuntime\include
 *     -I...StaticRecomp tests/native_search_entries_test.c chunk_0015.o chunk_0203.o
 *     cmake/composite/native_search.c cmake/composite/direct_calls.c
 *     cmake/composite/gather_pipe.c cmake/composite/guest_cpu.c
 *     ...\gxruntime.lib -o native_search_entries_test.exe
 *   native_search_entries_test MODULE.dll [CASES_PER_FUNCTION=6000]
 *
 * For both functions: random ordinary inputs (strings at every alignment, an
 * object-name table of 825 entries and a name in it or not) and some that
 * make the natives decline (a budget spent inside the work, a deadline in
 * it, a reservation on the frame), run (1) through the module's translation,
 * run as in play (direct calls, its edge filter, the host quiet), (2) through
 * the hooked chunks with the natives on, (3) through the hooked chunks with
 * them off; a small chassis loop runs the chunks (a chunk left at a boundary
 * goes on in the next). Every CPU byte and the RAM the functions touch must
 * match across all three, and the full images every 256 cases.
 *
 * Then ns per call through the hooked chunks with the natives on and off:
 * the chunk's entry and return dispatch included, the module's dispatcher
 * not - the closest this gets to a call in play. */
#include "native_search.h"
#include "direct_calls.h"
#include "gather_pipe.h"
#include "StaticRecompABI.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* The hooked chunks (their translated functions), by table index. */
void func_8003D6E0(CPUState*);
void func_8032D6E0(CPUState*);
static const struct {
    unsigned index;
    u32 start;
    BwChunkFn fn;
} CHUNKS[] = {{15, 0x8003D6E0u, func_8003D6E0}, {203, 0x8032D6E0u, func_8032D6E0}};

static void missing_chunk(CPUState* cpu) {
    fprintf(stderr, "a call into a chunk the test does not link (pc %08X)\n", cpu->pc);
    exit(1);
}
static BwChunkFn s_table[256];
BwChunkFn* const bw_chunk_fns = s_table;

BwChunkFn bw_find_chunk(u32 address) {
    for (unsigned i = 0; i < sizeof CHUNKS / sizeof CHUNKS[0]; ++i)
        if (address - CHUNKS[i].start < 0x4000u)
            return CHUNKS[i].fn;
    return NULL;
}

/* The module's own leaf natives (direct_calls.h) are not on these paths. */
int bw_native_call(CPUState* cpu, u32 address) {
    (void)cpu;
    (void)address;
    return 0;
}

extern CPUState bw_guest_cpu;
extern u8 bw_guest_mem1[];
int bluewake_composite_direct_calls(bool enabled, const bool* sources_dirty, const bool* decrementer_pending,
                                    const u32* pi_cause, const u32* pi_mask);

#define AREA 0x80100000u /* strings and stacks */
#define AREA_BYTES 0x10000u
#define TABLE_AREA 0x80372000u
#define TABLE_AREA_BYTES 0x4000u
#define TABLE 0x80372818u
#define ENTRIES 825u
#define RETURN_ADDRESS 0xFFFFFFFCu

static u32 seed = 0x1B873593u;
static u32 next(void) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static void put(u8* ram, u32 address, u32 value) { write_be32(ram + (address - GC_RAM_BASE), value); }
static u8* at(u8* ram, u32 address) { return ram + (address - GC_RAM_BASE); }

static unsigned s_unexpected_service;
static int edge_service(void* user, CPUState* cpu, u32 address) {
    (void)user;
    (void)cpu;
    if ((address & ~3u) != RETURN_ADDRESS)
        s_unexpected_service++;
    return 1;
}

static const u32 FUNCTIONS[] = {BLUEWAKE_SEARCH_STRCMP, BLUEWAKE_SEARCH_STAGE_NAME};
static const char* const NAMES[] = {"strcmp", "dStage_searchName"};

static u8 character(unsigned style) {
    switch (style) {
    case 0: return (u8)(0x21u + next() % 94u);
    case 1: return (u8)("ab01_"[next() % 5u]);
    default: return (u8)(next() % 4u ? 0x41u + next() % 26u : 0x81u + next() % 127u);
    }
}

static void write_string(u8* ram, u32 address, u32 length, unsigned style) {
    for (u32 i = 0; i < length; ++i)
        *at(ram, address + i) = character(style);
    *at(ram, address + length) = 0u;
}

/* The data, then each function's operands. */
static CPUState build(u8* ram, unsigned which, unsigned scenario) {
    for (u32 i = 0; i < AREA_BYTES; i += 4u)
        put(ram, AREA + i, next());
    CPUState c;
    memset(&c, 0, sizeof c);
    c.ram_size = GC_MAIN_RAM_SIZE;
    for (unsigned r = 0; r < 32; ++r) {
        c.gpr[r] = next();
        c.fpr[r] = f64_value(((u64)next() << 32) | next());
        c.ps1[r] = f64_value(((u64)next() << 32) | next());
    }
    c.gpr[1] = AREA + 0xF000u - 8u * (next() % 16u);
    c.lr = RETURN_ADDRESS | (next() & 3u);
    c.pc = FUNCTIONS[which];
    c.cr = next();
    c.xer = next();
    c.ctr = next();
    c.msr = PPC_MSR_FP;
    c.fpscr = next() & 0xFFFFF000u;
    c.reserve_valid = (scenario & 1u) != 0u;
    c.reserve_addr = scenario % 4u == 1u ? (c.gpr[1] - 12u) & ~31u : next();
    c.cycle_observation_suffix = next();
    c.cycle_budget = 65536;
    c.downcount = -(s64)(next() % 64u);
    c.cycle_deadline_budget = scenario % 3u == 0u ? 0 : 1000000;
    if (scenario % 17u == 3u)
        c.cycle_deadline_budget = 8 + (s64)(next() % (which ? 9000u : 60u)); /* the natives decline some */
    if (scenario % 19u == 4u)
        c.downcount = -c.cycle_budget + 1 + (s64)(next() % (which ? 9000u : 60u)); /* and some for the budget */
    const unsigned style = next() % 3u;
    if (which == 0) {
        const u32 a = AREA + 0x100u + next() % 0x3000u;
        u32 b = AREA + 0x4000u + next() % 0x3000u;
        if (next() % 2u)
            b = (b & ~3u) | (a & 3u);
        const u32 length = next() % 4u == 0u ? 30u + next() % 100u : next() % 16u;
        write_string(ram, a, length, style);
        memcpy(at(ram, b), at(ram, a), length + 1u);
        switch (scenario % 4u) {
        case 0: break;
        case 1:
            if (length != 0u)
                *at(ram, b + next() % length) = character(style);
            break;
        case 2:
            *at(ram, b + next() % (length + 1u)) = 0u;
            break;
        default:
            *at(ram, b + length) = character(style);
            *at(ram, b + length + 1u) = 0u;
            break;
        }
        c.gpr[3] = a;
        c.gpr[4] = b;
    } else {
        for (u32 i = 0; i < TABLE_AREA_BYTES; i += 4u)
            put(ram, TABLE_AREA + i, next());
        for (u32 e = 0; e < ENTRIES; ++e) {
            u8* entry = at(ram, TABLE + 12u * e);
            const u32 length = 1u + next() % 8u;
            for (u32 i = 0; i < 8u; ++i)
                entry[i] = i < length ? character(style) : 0u;
        }
        const u32 name = AREA + 0x100u + next() % 0x3000u;
        const u8* entry = at(ram, TABLE + 12u * (next() % ENTRIES));
        if (scenario % 3u == 0u) {
            write_string(ram, name, 1u + next() % 8u, next() % 3u);
        } else {
            u32 n = 0;
            while (n < 12u && entry[n] != 0u)
                n++;
            memcpy(at(ram, name), entry, n);
            *at(ram, name + n) = 0u;
        }
        c.gpr[3] = name;
    }
    return c;
}

static int compare(const char* what, unsigned i, const CPUState* got, const CPUState* want) {
    if (memcmp(got, want, sizeof *got) == 0)
        return 0;
    fprintf(stderr, "case %u: %s differs\n", i, what);
    for (unsigned b = 0; b < sizeof *got; ++b)
        if (((const u8*)got)[b] != ((const u8*)want)[b])
            fprintf(stderr, "  CPU byte %u: %02X, want %02X\n", b, ((const u8*)got)[b], ((const u8*)want)[b]);
    return 1;
}

static bool same_regions(const u8* a, const u8* b) {
    return memcmp(a + (AREA - GC_RAM_BASE), b + (AREA - GC_RAM_BASE), AREA_BYTES) == 0 &&
           memcmp(a + (TABLE_AREA - GC_RAM_BASE), b + (TABLE_AREA - GC_RAM_BASE), TABLE_AREA_BYTES) == 0;
}

static void copy_regions(u8* to, const u8* from) {
    memcpy(to + (AREA - GC_RAM_BASE), from + (AREA - GC_RAM_BASE), AREA_BYTES);
    memcpy(to + (TABLE_AREA - GC_RAM_BASE), from + (TABLE_AREA - GC_RAM_BASE), TABLE_AREA_BYTES);
}

/* The hooked chunks from `entry` until control reaches the return address:
 * a chunk left at a boundary goes on in the chunk that has the address, as
 * the chassis loop (its edge filter letting every boundary pass) would. A
 * spent budget ends the run, as it ends the loop's. */
static int run_routed(u32 entry) {
    CPUState* c = &bw_guest_cpu;
    c->pc = entry;
    for (unsigned guard = 0; guard < 100000u; ++guard) {
        if ((c->pc & ~3u) == RETURN_ADDRESS)
            return 1;
        if (c->cycle_budget > 0 && c->downcount <= -c->cycle_budget)
            return 2;
        BwChunkFn fn = bw_find_chunk(c->pc);
        if (fn == NULL) {
            fprintf(stderr, "no linked chunk at %08X\n", c->pc);
            return 0;
        }
        fn(c);
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: native_search_entries_test MODULE.dll [CASES_PER_FUNCTION]\n");
        return 2;
    }
    _putenv_s("BLUEWAKE_NATIVE_MATH", "0");
    const unsigned cases = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 6000u;
    HMODULE lib = LoadLibraryA(argv[1]);
    if (lib == NULL)
        return 1;
    StaticRecompGetModuleFn get = (StaticRecompGetModuleFn)(void*)GetProcAddress(lib, STATICRECOMP_GET_MODULE_SYMBOL);
    CPUState* (*guest_cpu)(void) = (CPUState * (*)(void))(void*)GetProcAddress(lib, "bluewake_composite_guest_cpu");
    u8* (*mem1)(u32*) = (u8* (*)(u32*))(void*)GetProcAddress(lib, "bluewake_composite_guest_mem1");
    void (*set_edge)(int (*)(void*, CPUState*, u32), void*) =
        (void (*)(int (*)(void*, CPUState*, u32), void*))(void*)GetProcAddress(lib, "bluewake_set_edge_service");
    int (*direct)(bool, const bool*, const bool*, const u32*, const u32*) =
        (int (*)(bool, const bool*, const bool*, const u32*, const u32*))(void*)GetProcAddress(
            lib, "bluewake_composite_direct_calls");
    int (*filter)(bool) = (int (*)(bool))(void*)GetProcAddress(lib, "bluewake_composite_edge_filter");
    if (!get || !guest_cpu || !mem1 || !set_edge || !direct || !filter)
        return 1;
    const StaticRecompModuleDesc* mod = get();
    if (mod->cpu_state_size != sizeof(CPUState) || strcmp(mod->game_id, "GZLE01") != 0)
        return 1;
    static const bool clear = false;
    static const u32 zero = 0u;
    if (!direct(true, &clear, &clear, &zero, &zero) || !filter(true))
        return 1;
    set_edge(edge_service, NULL);
    u32 size = 0;
    u8* reference_ram = mem1(&size);
    memset(reference_ram, 0, GC_MAIN_RAM_SIZE);

    /* The hooked side: its own chunk table, direct calls and edge filter on,
     * the host quiet; MEM1 its own (guest_cpu.c). */
    for (unsigned i = 0; i < 256u; ++i)
        s_table[i] = missing_chunk;
    for (unsigned i = 0; i < sizeof CHUNKS / sizeof CHUNKS[0]; ++i)
        s_table[CHUNKS[i].index] = CHUNKS[i].fn;
    static bool sources_dirty, decrementer_pending;
    static u32 pi_cause, pi_mask;
    bluewake_composite_direct_calls(true, &sources_dirty, &decrementer_pending, &pi_cause, &pi_mask);
    bw_edge_watch_ready = true;
    bw_edge_filter_enabled = true;
    u8* routed_ram = bw_guest_mem1;
    memset(routed_ram, 0, GC_MAIN_RAM_SIZE);
    u8* source = calloc(1, GC_MAIN_RAM_SIZE);
    if (source == NULL)
        return 1;

    for (unsigned which = 0; which < 2u; ++which) {
        unsigned stopped = 0;
        for (unsigned i = 0; i < cases; ++i) {
            const CPUState start = build(source, which, i);
            CPUState results[3];
            int outcome[3] = {1, 1, 1};
            for (unsigned run = 0; run < 3u; ++run) {
                if (run == 0u) {
                    copy_regions(reference_ram, source);
                    CPUState* g = guest_cpu();
                    *g = start;
                    g->ram = reference_ram;
                    mod->on_state_loaded(g);
                    s_unexpected_service = 0;
                    if (!mod->dispatch(g, FUNCTIONS[which]) || s_unexpected_service != 0u) {
                        fprintf(stderr, "case %u (%s): the translation did not finish\n", i, NAMES[which]);
                        return 1;
                    }
                    outcome[0] = (g->pc & ~3u) == RETURN_ADDRESS ? 1 : 2;
                    results[0] = *g;
                    results[0].ram = NULL;
                } else {
                    copy_regions(routed_ram, source);
                    bluewake_native_search_enabled = run == 1u;
                    bw_guest_cpu = start;
                    bw_guest_cpu.ram = routed_ram;
                    ppc_fpscr_updated(&bw_guest_cpu);
                    outcome[run] = run_routed(FUNCTIONS[which]);
                    if (outcome[run] == 0) {
                        fprintf(stderr, "case %u (%s): the hooked chunks did not finish\n", i, NAMES[which]);
                        return 1;
                    }
                    results[run] = bw_guest_cpu;
                    results[run].ram = NULL;
                    if (!same_regions(routed_ram, reference_ram)) {
                        fprintf(stderr, "case %u (%s, natives %s): RAM differs\n", i, NAMES[which],
                                run == 1u ? "on" : "off");
                        return 1;
                    }
                }
            }
            stopped += outcome[0] == 2;
            if (outcome[1] != outcome[0] || outcome[2] != outcome[0] ||
                compare(NAMES[which], i, &results[1], &results[0]) ||
                compare(NAMES[which], i, &results[2], &results[0])) {
                fprintf(stderr, "case %u (%s, seed %08X): mismatch (outcomes %d/%d/%d)\n", i, NAMES[which], seed,
                        outcome[0], outcome[1], outcome[2]);
                return 1;
            }
            if (i % 256u == 255u && memcmp(routed_ram, reference_ram, GC_MAIN_RAM_SIZE) != 0) {
                fprintf(stderr, "case %u (%s): a byte outside the compared ranges differs\n", i, NAMES[which]);
                return 1;
            }
        }
        printf("%08X %s: %u cases identical through the hooked chunks, natives on and off (%u stopped for the "
               "budget inside)\n",
               FUNCTIONS[which], NAMES[which], cases, stopped);
        fflush(stdout);
    }
    bluewake_native_search_report();

    /* ns per call through the hooked chunks, the natives on and off: the
     * chunk's entry and return included, the dispatcher not. */
    static const char* const pairs[3][2] = {
        {"ikada_h", "Ygush00"}, {"ikada_h", "ikada_h"}, {"Background_object_number_twenty_nine_A", "Background_object_number_twenty_nine_B"}};
    CPUState start = build(source, 0, 2u);
    start.cycle_deadline_budget = 0;
    start.downcount = 0;
    start.cycle_budget = (s64)1 << 40;
    start.reserve_valid = false;
    for (unsigned p = 0; p < 5u; ++p) {
        u32 r3, r4;
        if (p < 3u) {
            r3 = AREA + 0x100u;
            r4 = AREA + 0x200u;
            memcpy(at(source, r3), pairs[p][0], strlen(pairs[p][0]) + 1u);
            memcpy(at(source, r4), pairs[p][1], strlen(pairs[p][1]) + 1u);
        } else {
            /* A table whose entries differ from the name at the first byte
             * but for every 16th, "ikada_h" at entry 383; that name, and one
             * it lacks. */
            for (u32 e = 0; e < ENTRIES; ++e) {
                u8* entry = at(source, TABLE + 12u * e);
                entry[0] = e % 16u == 0u ? 'i' : (u8)('A' + e % 26u);
                for (u32 k = 1; k < 8u; ++k)
                    entry[k] = k < 6u ? (u8)("ab01_"[(e + k) % 5u]) : 0u;
            }
            memcpy(at(source, TABLE + 12u * 383u), "ikada_h", 8u);
            r3 = AREA + 0x300u;
            memcpy(at(source, r3), p == 3u ? "ikada_h" : "ikada_x", 8u);
            r4 = 0u;
        }
        const u32 entry = p < 3u ? BLUEWAKE_SEARCH_STRCMP : BLUEWAKE_SEARCH_STAGE_NAME;
        copy_regions(routed_ram, source);
        double best[2] = {1e30, 1e30};
        for (unsigned round = 0; round < 6u; ++round) {
            const int on = (int)(round & 1u);
            bluewake_native_search_enabled = on;
            bw_guest_cpu = start;
            bw_guest_cpu.ram = routed_ram;
            ppc_fpscr_updated(&bw_guest_cpu);
            const unsigned calls = p < 3u ? 400000u : 20000u;
            LARGE_INTEGER f, t0, t1;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&t0);
            for (unsigned k = 0; k < calls; ++k) {
                bw_guest_cpu.gpr[3] = r3;
                bw_guest_cpu.gpr[4] = r4;
                bw_guest_cpu.lr = RETURN_ADDRESS;
                run_routed(entry);
            }
            QueryPerformanceCounter(&t1);
            const double ns = (double)(t1.QuadPart - t0.QuadPart) * 1e9 / (double)f.QuadPart / calls;
            if (ns < best[on])
                best[on] = ns;
        }
        if (p < 3u)
            printf("strcmp(\"%s\", \"%s\"): hooked chunk %.1f ns/call with the native off, %.1f on (%.2fx)\n",
                   pairs[p][0], pairs[p][1], best[0], best[1], best[0] / best[1]);
        else
            printf("dStage_searchName(\"%s\"): hooked chunks %.1f ns/call with the natives off, %.1f on (%.2fx)\n",
                   p == 3u ? "ikada_h" : "ikada_x", best[0], best[1], best[0] / best[1]);
    }
    return 0;
}
