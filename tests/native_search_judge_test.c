/* cmake/composite/native_search.c's batched judge loop (fopAcM_findObjectCB
 * as cNdIt_Judge's judge) against the translation, run to the same boundary.
 *
 * Build and run from the worktree root (x64; the Visual Studio environment):
 *
 *   clang -O2 -march=x86-64-v3 -ffp-contract=off -Icmake/composite
 *     -IE:\Github\Wind-Waker-Recomp\ref\recompcore\GXRuntime\include
 *     -IE:\Github\Wind-Waker-Recomp\ref\recompcore\Source\Core\Core\PowerPC\StaticRecomp
 *     tests/native_search_judge_test.c cmake/composite/native_search.c cmake/composite/direct_calls.c
 *     E:\Github\Wind-Waker-Recomp\build\windows-exp\app\gxruntime_build\gxruntime.lib -o native_search_judge_test.exe
 *   native_search_judge_test MODULE.dll [CASES=60000] [BENCH_ROUNDS=200]
 *
 * MODULE.dll is a Windows game module (gGZLE01_recomp.dll) without these
 * natives; the test reads it and writes nothing but its own memory. The
 * module runs as in play: direct calls and its edge filter on, the host's
 * flags quiet. Its edge service stands in for the host's: at the boundary
 * into cTgIt_JudgeFilter (0x80245640, which the host watches, so the module
 * asks it there at every node) it has nothing to do until the translation
 * has reached it as many times as the native ran nodes, and then ends the
 * run there; asked anywhere else (the walk's end, the return out of
 * cNdIt_Judge after a match, which the test's frame sends to 0xFFFFFFFC), it
 * fails the case.
 *
 * Each case: an actor list of 1 to 40 nodes (cNdIt_Judge at the boundary
 * with the first, r31 the second), the filter (fopAcM_findObjectCB and its
 * search parameter: a name, a mask, a parameter), actors whose procname,
 * argument and parameter match the name's table entry in every combination
 * (so the judge answers NULL at each test, or matches); an object-name table
 * of 825 entries with the name in it or not, the name in MEM1, in a guest
 * alias registered in the module and the test alike (as a REL actor's
 * literal is) or through the uncached mirror; random registers, flags and
 * cycle state. And the ways it must stop early or decline: budgets and
 * deadlines inside the walk and before every suffix, an exception pending,
 * aliases over MEM1, a write journal, the calls inside an iteration in
 * trouble (the host not quiet, the edge filter off, each one's address
 * watched), the state not cNdIt_Judge's at its bctrl (LR, r29, CTR, r30), a
 * judge that is not fopAcM_findObjectCB, a NULL parameter, node, actor and
 * next pointers outside RAM or under the frames' stores, the name under them,
 * a reservation on them. Every byte of the CPU state (the cycle suffix
 * included) and of the test's RAM must match - or, where the native ran no
 * node, nothing may have changed. RAM outside the test areas is read-only in
 * both images.
 *
 * Then a microbenchmark: ns per node, the translation (through the module's
 * dispatcher and this trivial edge service; the host's own service costs
 * more) against the native. */
#include "native_search.h"
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
#define AREA_BYTES 0x10000u
#define TABLE_AREA 0x80372000u
#define TABLE_AREA_BYTES 0x4000u
#define TABLE 0x80372818u
#define ENTRIES 825u
#define PRM (AREA + 0x100u)
#define FILTER (AREA + 0x120u)
#define NAME (AREA + 0x140u)
#define NODES (AREA + 0x1000u)
#define ACTORS (AREA + 0x3000u)
#define ACTOR_BYTES 0x200u
#define MAX_NODES 40u
#define JUDGE_FILTER 0x80245640u
#define FIND_OBJECT 0x8002833Cu
#define NDIT_RETURN 0x80244F88u
#define RETURN_ADDRESS 0xFFFFFFFCu

static u32 seed = 0x7F4A7C15u;
static u32 next(void) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static void put(u8* ram, u32 address, u32 value) { write_be32(ram + (address - GC_RAM_BASE), value); }
static u32 get(const u8* ram, u32 address) { return read_be32(ram + (address - GC_RAM_BASE)); }
/* A guest alias, registered in the module and in the test alike, as a REL
 * module's linked data (where an actor's name literals live). */
#define ALIAS 0xC1F00000u
#define ALIAS_BYTES 0x1000u
static u8 s_alias_native[ALIAS_BYTES], s_alias_module[ALIAS_BYTES];
/* The byte behind a guest address the test lays out: MEM1, its mirror, the
 * alias. */
static u8* at(u8* ram, u32 address) {
    if (address - ALIAS < ALIAS_BYTES)
        return s_alias_native + (address - ALIAS);
    return ram + ((address & ~0x40000000u) - GC_RAM_BASE);
}

static void journal(u32 offset, u32 size, void* user) {
    (void)offset;
    (void)size;
    (void)user;
    abort();
}

/* The host's edge service, as far as this walk goes; for one JudgeFilter
 * call, the end of the run at its return address. */
static unsigned s_arrivals, s_stop_at, s_unexpected_service;
static u32 s_unexpected_address;
static bool s_one_call;
static int edge_service(void* user, CPUState* cpu, u32 address) {
    (void)user;
    (void)cpu;
    if (s_one_call && (address & ~3u) == RETURN_ADDRESS)
        return 1;
    if (!s_one_call && address == JUDGE_FILTER)
        return ++s_arrivals >= s_stop_at;
    s_unexpected_service++;
    s_unexpected_address = address;
    return 1;
}

static const struct {
    u32 start, bytes;
} AREAS[] = {{AREA, AREA_BYTES}, {TABLE_AREA, TABLE_AREA_BYTES}};
#define AREA_COUNT (sizeof AREAS / sizeof AREAS[0])

static u8* protect_image(u8* p) {
    DWORD old;
    if (p == NULL || !VirtualProtect(p, GC_MAIN_RAM_SIZE, PAGE_READONLY, &old))
        return NULL;
    for (unsigned i = 0; i < AREA_COUNT; ++i)
        if (!VirtualProtect(p + (AREAS[i].start - GC_RAM_BASE), AREAS[i].bytes, PAGE_READWRITE, &old))
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

static void copy_areas(u8* to, const u8* from) {
    for (unsigned i = 0; i < AREA_COUNT; ++i)
        memcpy(to + (AREAS[i].start - GC_RAM_BASE), from + (AREAS[i].start - GC_RAM_BASE), AREAS[i].bytes);
}

static bool areas_equal(const u8* a, const u8* b) {
    for (unsigned i = 0; i < AREA_COUNT; ++i)
        if (memcmp(a + (AREAS[i].start - GC_RAM_BASE), b + (AREAS[i].start - GC_RAM_BASE), AREAS[i].bytes) != 0)
            return false;
    return true;
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
    /* As the builder's list has them: the host names the boundary itself and
     * cNdIt_Judge's return. */
    watch(JUDGE_FILTER);
    watch(NDIT_RETURN);
    watch(0x80006000u);
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

static u8 character(unsigned style) {
    switch (style) {
    case 0: return (u8)(0x21u + next() % 94u);
    case 1: return (u8)("ab01_"[next() % 5u]);
    default: return (u8)(next() % 4u ? 0x41u + next() % 26u : 0x81u + next() % 127u);
    }
}

typedef struct Case {
    CPUState cpu;
    bool journal, aliases, trouble, malformed;
    u32 entry; /* the table entry the name finds, or 0 */
} Case;

/* The table, the name and the search parameter; the entry the name finds
 * (or 0). */
static u32 build_search(u8* ram, unsigned scenario) {
    const unsigned style = scenario % 3u == 0u ? 1u : next() % 2u ? 0u : 2u;
    for (u32 i = 0; i < TABLE_AREA_BYTES; i += 4u)
        put(ram, TABLE_AREA + i, next());
    for (u32 e = 0; e < ENTRIES; ++e) {
        u8* entry = at(ram, TABLE + 12u * e);
        const u32 length = 1u + next() % 7u;
        for (u32 i = 0; i < 8u; ++i)
            entry[i] = i < length ? character(style) : 0u;
    }
    /* The name in MEM1, in the alias (an actor's own literal) or through
     * the uncached mirror. */
    const u32 name = scenario % 3u == 1u ? ALIAS + next() % 0x800u
                     : scenario % 7u == 3u ? (NAME + next() % 4u) | 0x40000000u
                                           : NAME + next() % 4u;
    for (u32 i = 0; i < ALIAS_BYTES; ++i)
        s_alias_native[i] = (u8)next();
    const u32 index = next() % ENTRIES;
    if (scenario % 5u == 0u) { /* a name it lacks, most likely */
        const u32 length = 1u + next() % 8u;
        for (u32 i = 0; i < length; ++i)
            *at(ram, name + i) = character(style);
        *at(ram, name + length) = 0u;
    } else {
        for (u32 i = 0; i < 8u; ++i)
            *at(ram, name + i) = *at(ram, TABLE + 12u * index + i);
        *at(ram, name + 8u) = 0u;
    }
    put(ram, PRM, name);
    put(ram, PRM + 4u, scenario % 3u == 0u ? 0u : next() % 2u ? 0xFFu : next());
    put(ram, PRM + 8u, next() % 4u);
    put(ram, FILTER, FIND_OBJECT);
    put(ram, FILTER + 4u, PRM);
    /* The entry the translation finds: the first of that name. */
    for (u32 e = 0; e < ENTRIES; ++e)
        if (strncmp((const char*)at(ram, TABLE + 12u * e), (const char*)at(ram, name), 12) == 0 &&
            strnlen((const char*)at(ram, name), 12) < 12u)
            return TABLE + 12u * e;
    return 0u;
}

/* An actor that the judge answers NULL for (at one of its tests), or now
 * and then one it matches. */
static void build_actor(u8* ram, u32 actor, u32 entry, unsigned scenario) {
    for (u32 i = 0; i < ACTOR_BYTES; i += 4u)
        put(ram, actor + i, next());
    if (entry == 0u)
        return;
    const u16 procname = (u16)(at(ram, entry + 8u)[0] << 8 | at(ram, entry + 9u)[0]);
    const u8 argument = at(ram, entry + 10u)[0];
    const u32 mask = get(ram, PRM + 4u), param = get(ram, PRM + 8u);
    const unsigned kind = next() % (scenario % 7u == 1u ? 8u : 40u);
    u16 p = (u16)next();
    u8 a = (u8)next();
    u32 q = next();
    switch (kind) {
    case 0: p = procname; a = argument; q = param; break;                            /* matches if mask covers */
    case 1: case 2: p = procname; a = (u8)(argument + 1u + next() % 255u); break;     /* the argument differs */
    case 3: case 4: p = procname; a = argument; q = (param ^ (mask & (1u << (next() % 32u)))) | 0u; break;
    case 5: p = procname; a = argument; break;
    default: if (p == procname) p++; break;                                           /* the procname differs */
    }
    *at(ram, actor + 14u) = (u8)(p >> 8);
    *at(ram, actor + 15u) = (u8)p;
    *at(ram, actor + 449u) = a;
    put(ram, actor + 176u, q);
}

static Case build(u8* ram, unsigned scenario, bool one_call) {
    Case k;
    memset(&k, 0, sizeof k);
    for (u32 i = 0; i < AREA_BYTES; i += 4u)
        put(ram, AREA + i, next());
    const u32 entry = build_search(ram, scenario);
    k.entry = entry;
    /* The list, its nodes in a random order. */
    const unsigned count = scenario % 13u == 0u ? 1u : 2u + next() % (MAX_NODES - 1u);
    u32 order[MAX_NODES];
    for (unsigned i = 0; i < MAX_NODES; ++i)
        order[i] = i;
    for (unsigned i = MAX_NODES - 1u; i > 0; --i) {
        const unsigned j = next() % (i + 1u), t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    for (unsigned i = 0; i < count; ++i) {
        const u32 node = NODES + 32u * order[i];
        put(ram, node + 8u, i + 1u < count ? NODES + 32u * order[i + 1u] : 0u);
        const u32 actor = ACTORS + ACTOR_BYTES * order[i];
        put(ram, node + 12u, actor);
        build_actor(ram, actor, entry, scenario);
    }
    CPUState* c = &k.cpu;
    c->ram = ram;
    c->ram_size = GC_MAIN_RAM_SIZE;
    for (unsigned r = 0; r < 32; ++r) {
        c->gpr[r] = next();
        c->fpr[r] = f64_value(((u64)next() << 32) | next());
        c->ps1[r] = f64_value(((u64)next() << 32) | next());
    }
    const u32 sp = AREA + 0xC000u + 8u * (next() % 0x300u) + (scenario % 23u == 4u ? 1u + next() % 7u : 0u);
    /* cNdIt_Judge's frame above: its saved LR sends a return out of it to
     * the test's end. */
    put(ram, sp + 36u, RETURN_ADDRESS);
    c->gpr[1] = sp;
    c->gpr[3] = NODES + 32u * order[0];
    c->gpr[31] = get(ram, c->gpr[3] + 8u);
    c->gpr[4] = c->gpr[30] = FILTER;
    c->gpr[29] = JUDGE_FILTER;
    c->ctr = JUDGE_FILTER | (scenario % 9u == 2u ? next() & 3u : 0u);
    c->lr = NDIT_RETURN;
    c->pc = JUDGE_FILTER;
    if (one_call) { /* JudgeFilter called from anywhere, returning to the test's end */
        c->lr = RETURN_ADDRESS | (next() & 3u);
        c->gpr[29] = next();
        c->gpr[30] = next();
        c->ctr = next();
    }
    c->cr = next();
    c->xer = next();
    c->msr = next() & ~PPC_MSR_EE;
    c->hid2 = next();
    c->fpscr = next() & ~3u;
    c->reserve_valid = (scenario & 1u) != 0u;
    c->reserve_addr = next() % 2u ? (sp - 64u + 4u * (next() % 18u)) & ~31u : next();
    c->cycle_observation_suffix = next();
    c->cycle_budget = 400000;
    c->downcount = -(s64)(next() % 64u);
    c->cycle_deadline_budget = scenario % 4u == 0u ? 0 : 100000000;
    /* One node costs the translation 60 to 12,500 cycles. */
    const s64 span = 20000;
    switch (scenario % 29u) {
    case 1: c->cycle_deadline_budget = 1 + (s64)(next() % (u32)span) - c->downcount; break; /* inside the walk */
    case 2: c->cycle_deadline_budget = 1 + (s64)(next() % 12u); break;                    /* before a suffix */
    case 3: c->cycle_deadline_budget = -(s64)(next() % 100u); break;
    case 4: c->downcount = -c->cycle_budget; break;
    case 5: c->downcount = -c->cycle_budget + 1 + (s64)(next() % (u32)span); break; /* spent in the walk */
    case 6: c->cycle_budget = 1 + (s64)(next() % (u32)span); c->downcount = 0; break;
    case 7: c->downcount = (s64)(next() % 8u); break;
    case 8: c->exception = 1u; break;
    case 9: c->cycle_budget = 0; break;
    default: break;
    }
    /* Pointers that leave RAM or lie under the frames, at some node. */
    const u32 victim = NODES + 32u * order[next() % count];
    switch (scenario % 37u) {
    case 1: put(ram, victim + 12u, 0xCC000000u); break;
    case 2: put(ram, victim + 8u, 0xC0100000u); break;
    case 3: put(ram, victim + 12u, sp - 200u - next() % 300u); break; /* an actor whose fields reach the stores */
    case 4: put(ram, victim + 8u, sp - 64u - 8u + 4u * (next() % 4u)); break; /* a next node under them */
    case 5: put(ram, victim + 12u, GC_RAM_BASE + GC_MAIN_RAM_SIZE - 100u); break; /* fields past the end */
    case 6: put(ram, PRM, sp - 60u); break;                                      /* the name under them */
    case 7: c->gpr[1] = GC_RAM_BASE + 32u; break;                                /* the frames below RAM */
    default: break;
    }
    /* Not cNdIt_Judge at its bctrl with fopAcM_findObjectCB's search (one
     * call: only the search counts). */
    switch (one_call && scenario % 53u != 5u && scenario % 53u != 6u ? 0u : scenario % 53u) {
    case 1: k.malformed = true; c->lr = 0x80244F8Cu; break;
    case 2: k.malformed = true; c->gpr[29] = 0x80245644u; break;
    case 3: k.malformed = true; c->ctr = 0x80245600u; break;
    case 4: k.malformed = true; c->gpr[30] = FILTER + 4u; break;
    case 5: k.malformed = true; put(ram, FILTER, 0x80040068u); break; /* fpcSch_JudgeByID */
    case 6: k.malformed = true; put(ram, FILTER + 4u, 0u); break;
    case 7: k.malformed = true; c->pc = 0x80245644u; break;
    default: break;
    }
    k.journal = scenario % 31u == 7u;
    k.aliases = scenario % 41u == 8u;
    k.trouble = scenario % 19u == 5u;
    return k;
}

/* The ways the calls inside an iteration must not be made natively. */
static void boundary_trouble(unsigned scenario, CPUState* cpu) {
    static const u32 calls[] = {FIND_OBJECT, 0x80245664u, 0x80041544u, 0x80028394u, 0x8032DB44u, 0x80041578u};
    switch ((scenario / 19u) % 5u) {
    case 0: s_sources_dirty = true; break;
    case 1: s_decrementer_pending = true; cpu->msr |= PPC_MSR_EE; break;
    case 2: s_pi_cause = s_pi_mask = 0x10u; cpu->msr |= PPC_MSR_EE; break;
    case 3: bw_edge_filter_enabled = false; break;
    default: watch(calls[(scenario / 95u) % 6u] | (scenario & 1u ? 0x40000000u : 0u)); break;
    }
}

static void report_cpu(const CPUState* got, const CPUState* want) {
    for (unsigned b = 0; b < sizeof *got; ++b)
        if (((const u8*)got)[b] != ((const u8*)want)[b])
            fprintf(stderr, "  CPU byte %u: got %02X want %02X\n", b, ((const u8*)got)[b], ((const u8*)want)[b]);
}

static void report_ram(const u8* got, const u8* want) {
    unsigned shown = 0;
    for (unsigned i = 0; i < AREA_COUNT && shown < 16u; ++i)
        for (u32 b = 0; b < AREAS[i].bytes && shown < 16u; ++b) {
            const u32 o = AREAS[i].start - GC_RAM_BASE + b;
            if (got[o] != want[o]) {
                fprintf(stderr, "  RAM %08X: got %02X want %02X\n", GC_RAM_BASE + o, got[o], want[o]);
                shown++;
            }
        }
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
        fprintf(stderr, "usage: native_search_judge_test MODULE.dll [CASES] [BENCH_ROUNDS]\n");
        return 2;
    }
    _putenv_s("BLUEWAKE_NATIVE_MATH", "0");
    const unsigned cases = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 60000u;
    const unsigned bench_rounds = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 200u;
    HMODULE lib = LoadLibraryA(argv[1]);
    if (lib == NULL) {
        fprintf(stderr, "cannot load %s (%lu)\n", argv[1], GetLastError());
        return 1;
    }
    StaticRecompGetModuleFn get_module =
        (StaticRecompGetModuleFn)(void*)GetProcAddress(lib, STATICRECOMP_GET_MODULE_SYMBOL);
    CPUState* (*guest_cpu)(void) = (CPUState * (*)(void))(void*)GetProcAddress(lib, "bluewake_composite_guest_cpu");
    void (*set_edge)(int (*)(void*, CPUState*, u32), void*) =
        (void (*)(int (*)(void*, CPUState*, u32), void*))(void*)GetProcAddress(lib, "bluewake_set_edge_service");
    int (*direct)(bool, const bool*, const bool*, const u32*, const u32*) =
        (int (*)(bool, const bool*, const bool*, const u32*, const u32*))(void*)GetProcAddress(
            lib, "bluewake_composite_direct_calls");
    int (*filter)(bool) = (int (*)(bool))(void*)GetProcAddress(lib, "bluewake_composite_edge_filter");
    if (get_module == NULL || guest_cpu == NULL || set_edge == NULL || direct == NULL || filter == NULL) {
        fprintf(stderr, "not a BlueWake Windows module\n");
        return 1;
    }
    const StaticRecompModuleDesc* mod = get_module();
    if (mod->cpu_state_size != sizeof(CPUState) || strcmp(mod->game_id, "GZLE01") != 0) {
        fprintf(stderr, "CPU state size %u, expected %u\n", mod->cpu_state_size, (unsigned)sizeof(CPUState));
        return 1;
    }
    static const bool clear = false;
    static const u32 zero = 0u;
    if (!direct(true, &clear, &clear, &zero, &zero) || !filter(true)) {
        fprintf(stderr, "the module's direct calls or edge filter are unavailable\n");
        return 1;
    }
    set_edge(edge_service, NULL);
    bool (*module_alias_add)(u32, u32, u8*) =
        (bool (*)(u32, u32, u8*))(void*)GetProcAddress(lib, "ppc_guest_alias_add_shared");
    if (module_alias_add == NULL || !module_alias_add(ALIAS, ALIAS_BYTES, s_alias_module) ||
        !ppc_guest_alias_add_shared(ALIAS, ALIAS_BYTES, s_alias_native) || g_ppc_guest_aliases_overlap_mem1) {
        fprintf(stderr, "cannot register the alias\n");
        return 1;
    }
    u8* reference_ram = module_image(lib);
    u8* native_ram = protect_image(VirtualAlloc(NULL, GC_MAIN_RAM_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    u8* before = VirtualAlloc(NULL, GC_MAIN_RAM_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (reference_ram == NULL || native_ram == NULL || before == NULL)
        return 1;
    bluewake_native_search_enabled = 1;

    /* cTgIt_JudgeFilter called once, through its blr. */
    {
        unsigned ran = 0, declined = 0, trouble_declined = 0, found = 0, matched = 0;
        s_one_call = true;
        for (unsigned i = 0; i < cases; ++i) {
            Case k = build(native_ram, i, true);
            copy_areas(before, native_ram);
            reset_host();
            if (k.trouble)
                boundary_trouble(i, &k.cpu);
            ppc_fpscr_updated(&k.cpu);
            CPUState native = k.cpu;
            const CPUState untouched = native;
            if (k.journal)
                g_mem_write_journal = journal;
            if (k.aliases)
                g_ppc_guest_aliases_overlap_mem1 = true;
            const int accepted = bluewake_native_search(&native, JUDGE_FILTER);
            g_mem_write_journal = NULL;
            g_ppc_guest_aliases_overlap_mem1 = false;
            if (!accepted) {
                declined++;
                trouble_declined += k.trouble;
                if (memcmp(&native, &untouched, sizeof native) != 0 || !areas_equal(before, native_ram)) {
                    fprintf(stderr, "JudgeFilter case %u: declined but changed state\n", i);
                    report_cpu(&native, &untouched);
                    return 1;
                }
                continue;
            }
            if (k.cpu.exception || k.aliases || k.trouble || k.journal || k.malformed) {
                fprintf(stderr, "JudgeFilter case %u: ran where it must decline\n", i);
                return 1;
            }
            ran++;
            found += k.entry != 0u;
            matched += native.gpr[3] != 0u;
            copy_areas(reference_ram, before);
        memcpy(s_alias_module, s_alias_native, ALIAS_BYTES);
            memcpy(s_alias_module, s_alias_native, ALIAS_BYTES);
            CPUState* g = guest_cpu();
            *g = k.cpu;
            g->ram = reference_ram;
            mod->on_state_loaded(g);
            s_unexpected_service = 0;
            if (!mod->dispatch(g, JUDGE_FILTER)) {
                fprintf(stderr, "JudgeFilter case %u: the translation did not run\n", i);
                return 1;
            }
            CPUState reference = *g;
            reference.ram = native.ram;
            if (s_unexpected_service != 0u || (reference.pc & ~3u) != RETURN_ADDRESS ||
                memcmp(&native, &reference, sizeof native) != 0 || !areas_equal(native_ram, reference_ram)) {
                fprintf(stderr, "JudgeFilter case %u (seed %08X): mismatch (asked %u times elsewhere, last at %08X)\n",
                        i, seed, s_unexpected_service, s_unexpected_address);
                report_cpu(&native, &reference);
                report_ram(native_ram, reference_ram);
                return 1;
            }
        }
        printf("%08X JudgeFilter: %u cases, %u identical (%u with the name in the table, %u returning the actor), "
               "%u declined unchanged (%u with a call in trouble), 0 mismatches\n",
               JUDGE_FILTER, cases, ran, found, matched, declined, trouble_declined);
        fflush(stdout);
        if (ran < 30000u && cases >= 60000u)
            return 1; /* at least 30,000 compared cases */
        s_one_call = false;
    }

    /* The walk: the JudgeFilter hook has run, so the module is the certified one. */
    if (!bluewake_native_search_judge_ready) {
        fprintf(stderr, "the JudgeFilter entry did not arm the walk\n");
        return 1;
    }
    unsigned ran = 0, declined = 0, trouble_declined = 0, found = 0;
    unsigned long long nodes_total = 0, whole_lists = 0, histogram[4] = {0};
    for (unsigned i = 0; i < cases; ++i) {
        Case k = build(native_ram, i, false);
        copy_areas(before, native_ram);
        reset_host();
        if (k.trouble)
            boundary_trouble(i, &k.cpu);
        ppc_fpscr_updated(&k.cpu); /* as the module's on_state_loaded does (FEX, VX) */
        CPUState native = k.cpu;
        const CPUState untouched = native;
        if (k.journal)
            g_mem_write_journal = journal;
        if (k.aliases)
            g_ppc_guest_aliases_overlap_mem1 = true;
        const unsigned nodes = bluewake_native_search_judge(&native);
        g_mem_write_journal = NULL;
        g_ppc_guest_aliases_overlap_mem1 = false;
        if (nodes == 0u) {
            declined++;
            trouble_declined += k.trouble;
            if (memcmp(&native, &untouched, sizeof native) != 0 || !areas_equal(before, native_ram)) {
                fprintf(stderr, "case %u: no node run but the state changed\n", i);
                report_cpu(&native, &untouched);
                return 1;
            }
            continue;
        }
        if (k.cpu.exception || k.aliases || k.trouble || k.journal || k.malformed) {
            fprintf(stderr, "case %u: ran where it must decline\n", i);
            return 1;
        }
        ran++;
        nodes_total += nodes;
        histogram[nodes == 1u ? 0 : nodes < 8u ? 1 : nodes < 24u ? 2 : 3]++;
        whole_lists += native.gpr[31] == 0u;
        found += k.entry != 0u;

        copy_areas(reference_ram, before);
        memcpy(s_alias_module, s_alias_native, ALIAS_BYTES);
        CPUState* g = guest_cpu();
        *g = k.cpu;
        g->ram = reference_ram;
        mod->on_state_loaded(g);
        s_arrivals = 0;
        s_stop_at = nodes;
        s_unexpected_service = 0;
        if (!mod->dispatch(g, JUDGE_FILTER)) {
            fprintf(stderr, "case %u: the translation did not run\n", i);
            return 1;
        }
        CPUState reference = *g;
        reference.ram = native.ram;
        if (s_unexpected_service != 0u || s_arrivals != nodes || reference.pc != JUDGE_FILTER ||
            memcmp(&native, &reference, sizeof native) != 0 || !areas_equal(native_ram, reference_ram)) {
            fprintf(stderr,
                    "case %u (seed %08X): mismatch after %u nodes (the translation arrived %u times; asked %u times "
                    "elsewhere, last at %08X; pc %08X)\n",
                    i, seed, nodes, s_arrivals, s_unexpected_service, s_unexpected_address, reference.pc);
            report_cpu(&native, &reference);
            report_ram(native_ram, reference_ram);
            return 1;
        }
    }
    printf("judge loop: %u cases, %u identical (%u with the name in the table; %llu nodes: %llu runs of 1, %llu of "
           "2-7, %llu of 8-23, %llu of 24 or more; %llu ending at the list's last node), %u ran no node and changed "
           "nothing (%u with a call in trouble), 0 mismatches\n",
           cases, ran, found, nodes_total, histogram[0], histogram[1], histogram[2], histogram[3], whole_lists,
           declined, trouble_declined);
    fflush(stdout);
    if (ran < 30000u && cases >= 60000u)
        return 1; /* at least 30,000 compared cases */

    if (bench_rounds != 0u) {
        /* 64 actors none of which matches, the name at entry 383 (as
         * "ikada_h" is) of a table whose entries differ from it at the
         * first byte but for three, and at entry 0. */
        for (unsigned variant = 0; variant < 2u; ++variant) {
            reset_host();
            Case k = build(native_ram, 10u, false);
            for (u32 e = 0; e < ENTRIES; ++e) {
                u8* entry = at(native_ram, TABLE + 12u * e);
                entry[0] = (u8)('A' + e % 26u);
                entry[1] = (u8)('a' + e % 23u);
                entry[2] = 0u;
            }
            for (u32 e = 0; e < 3u; ++e)
                memcpy(at(native_ram, TABLE + 12u * (100u * e + 7u)), "itemFLY", 8u);
            const u32 index = variant == 0u ? 383u : 0u;
            memcpy(at(native_ram, TABLE + 12u * index), "ikada_h", 8u);
            *at(native_ram, TABLE + 12u * index + 8u) = 0x01u; /* its procname */
            *at(native_ram, TABLE + 12u * index + 9u) = 0x23u;
            memcpy(at(native_ram, NAME), "ikada_h", 8u);
            put(native_ram, PRM, NAME);
            put(native_ram, PRM + 4u, 0u);
            put(native_ram, FILTER, FIND_OBJECT);
            put(native_ram, FILTER + 4u, PRM);
            const unsigned count = 64u;
            for (unsigned i = 0; i < count; ++i) {
                const u32 node = NODES + 32u * (i % MAX_NODES) + (i >= MAX_NODES ? 16u : 0u);
                const u32 next_node =
                    i + 1u < count ? NODES + 32u * ((i + 1u) % MAX_NODES) + (i + 1u >= MAX_NODES ? 16u : 0u) : 0u;
                put(native_ram, node + 8u, next_node);
                put(native_ram, node + 12u, ACTORS + ACTOR_BYTES * (i % MAX_NODES));
                *at(native_ram, ACTORS + ACTOR_BYTES * (i % MAX_NODES) + 14u) = 0x7Fu; /* never the entry's */
                *at(native_ram, ACTORS + ACTOR_BYTES * (i % MAX_NODES) + 15u) = 0x00u;
            }
            CPUState base = k.cpu;
            base.gpr[3] = NODES;
            base.gpr[31] = get(native_ram, NODES + 8u);
            base.exception = 0;
            base.downcount = 0;
            base.cycle_deadline_budget = 0;
            base.cycle_budget = (s64)1 << 50;
            base.reserve_valid = false;
            base.ctr = JUDGE_FILTER;
            base.pc = JUDGE_FILTER;
            base.gpr[1] = AREA + 0xC000u;
            put(native_ram, base.gpr[1] + 36u, RETURN_ADDRESS);
            copy_areas(reference_ram, native_ram);
            memcpy(s_alias_module, s_alias_native, ALIAS_BYTES);
            CPUState* g = guest_cpu();
            double best_t = 1e30, best_n = 1e30;
            unsigned ran_nodes = 0;
            for (unsigned round = 0; round < 5u; ++round) {
                double t0 = now_ns();
                for (unsigned r = 0; r < bench_rounds; ++r) {
                    *g = base;
                    g->ram = reference_ram;
                    s_arrivals = 0;
                    s_stop_at = count - 1u;
                    mod->dispatch(g, JUDGE_FILTER);
                }
                const double t = (now_ns() - t0) / bench_rounds / (count - 1u);
                CPUState native;
                t0 = now_ns();
                for (unsigned r = 0; r < bench_rounds; ++r) {
                    native = base;
                    ran_nodes = bluewake_native_search_judge(&native);
                }
                const double n = (now_ns() - t0) / bench_rounds / (count - 1u);
                if (t < best_t) best_t = t;
                if (n < best_n) best_n = n;
            }
            if (ran_nodes != count - 1u) {
                fprintf(stderr, "the benchmark's walk ran %u nodes natively\n", ran_nodes);
                return 1;
            }
            printf("judge loop, the name at entry %u: translation %.1f ns/node through the dispatcher, native %.1f "
                   "ns/node\n",
                   index, best_t, best_n);
            /* One JudgeFilter call, returning to the test's end. */
            s_one_call = true;
            best_t = best_n = 1e30;
            const unsigned calls = bench_rounds * 16u;
            for (unsigned round = 0; round < 5u; ++round) {
                *g = base;
                g->ram = reference_ram;
                g->lr = RETURN_ADDRESS;
                double t0 = now_ns();
                for (unsigned r = 0; r < calls; ++r) {
                    g->gpr[3] = base.gpr[3];
                    g->gpr[4] = base.gpr[4];
                    g->pc = JUDGE_FILTER;
                    mod->dispatch(g, JUDGE_FILTER);
                }
                const double t = (now_ns() - t0) / calls;
                CPUState native = base;
                native.lr = RETURN_ADDRESS;
                t0 = now_ns();
                for (unsigned r = 0; r < calls; ++r) {
                    native.gpr[3] = base.gpr[3];
                    native.gpr[4] = base.gpr[4];
                    if (!bluewake_native_search(&native, JUDGE_FILTER))
                        return 1;
                }
                const double n = (now_ns() - t0) / calls;
                if (t < best_t) best_t = t;
                if (n < best_n) best_n = n;
            }
            s_one_call = false;
            printf("JudgeFilter, the name at entry %u: translation %.1f ns/call through the dispatcher, native %.1f "
                   "ns/call\n",
                   index, best_t, best_n);
        }
    }
    bluewake_native_search_judge_report();
    return 0;
}
