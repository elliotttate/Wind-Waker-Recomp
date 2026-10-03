/* cmake/composite/native_search.c against the translations it stands in for.
 *
 * Build and run from the worktree root (x64; the Visual Studio environment):
 *
 *   clang -O2 -march=x86-64-v3 -ffp-contract=off -Icmake/composite
 *     -IE:\Github\Wind-Waker-Recomp\ref\recompcore\GXRuntime\include
 *     -IE:\Github\Wind-Waker-Recomp\ref\recompcore\Source\Core\Core\PowerPC\StaticRecomp
 *     tests/native_search_test.c cmake/composite/native_search.c cmake/composite/direct_calls.c
 *     E:\Github\Wind-Waker-Recomp\build\windows-exp\app\gxruntime_build\gxruntime.lib -o native_search_test.exe
 *   native_search_test MODULE.dll [CASES_PER_FUNCTION=60000] [BENCH_CALLS=200000]
 *
 * MODULE.dll is a Windows game module (gGZLE01_recomp.dll) without these
 * natives; the test reads it and writes nothing but its own memory. The
 * module runs as in play: direct calls and its edge filter on, the host's
 * flags quiet, an edge service that ends the run at the return address (and
 * fails the test if it is asked anything anywhere else).
 *
 * strcmp: random strings - equal, differing at any byte, one a prefix of the
 * other, bytes 0x80 and up (which the word loop's zero test cannot tell from
 * a zero), runs of 0x01 - at every pair of alignments, the bytes after each
 * terminator random (the word loop reads them), long strings; pointers that
 * leave RAM at the first byte, in the byte loops and in the word loop.
 *
 * dStage_searchName: an object-name table of 825 random entries at its
 * address (names of one to eight characters, from a wide or a narrow
 * alphabet so that many share their first bytes; procname and argument
 * after each), searched for one of its names (at any index, the first copy
 * of a repeated one winning), for a name it lacks, a prefix or an extension
 * of one, the empty name, names with high bytes; the name at any alignment,
 * inside the table itself, under the function's own frame; the stack at any
 * alignment, at the edge of RAM; a reservation on the frame's words.
 *
 * Both: random registers, flags and cycle state; budgets spent at the start,
 * inside the work and just after it; deadlines inside it and before every
 * suffix; an exception pending; aliases over MEM1; a write journal; and for
 * the calls into strcmp's chunk, the host not quiet (sources dirty, a
 * decrementer or processor interrupt the guest would take), the edge filter
 * off, the call or the return address watched. Every byte of the CPU state
 * (the cycle suffix included) and of the test's RAM must match - or, where
 * the native declines, nothing may have changed. RAM outside the test areas
 * is read-only in both images.
 *
 * Then a microbenchmark: the translation through the module's dispatcher
 * (a one-instruction function's dispatch is measured for scale) against the
 * native, ns per call. */
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

#define AREA 0x80100000u /* strings and stacks */
#define AREA_BYTES 0x10000u
#define TABLE_AREA 0x80372000u /* l_objectName (0x80372818) and what follows it */
#define TABLE_AREA_BYTES 0x4000u
#define TABLE 0x80372818u
#define ENTRIES 825u
#define END_AREA (GC_RAM_BASE + GC_MAIN_RAM_SIZE - 0x1000u) /* the last page of the test's RAM */
#define END_AREA_BYTES 0x1000u
#define EMPTY_FUNCTION 0x802DB978u /* draw__9J3DPacketFv: blr */
#define RETURN_ADDRESS 0xFFFFFFFCu

static u32 seed = 0x9E3779B9u;
static u32 next(void) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static void put(u8* ram, u32 address, u32 value) { write_be32(ram + (address - GC_RAM_BASE), value); }
static u8* at(u8* ram, u32 address) { return ram + (address - GC_RAM_BASE); }

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

static const struct {
    u32 start, bytes;
} AREAS[] = {{AREA, AREA_BYTES}, {TABLE_AREA, TABLE_AREA_BYTES}, {END_AREA, END_AREA_BYTES}};
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
    /* Some watched addresses, as the builder's list has. */
    watch(0x80006000u);
    watch(0x80041500u);
    watch(0x8032DC6Cu);
    watch(0x80245640u);
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

static const u32 FUNCTIONS[] = {BLUEWAKE_SEARCH_STRCMP, BLUEWAKE_SEARCH_STAGE_NAME};

/* A random character: printable, a narrow set, or any byte. */
static u8 character(unsigned style) {
    switch (style) {
    case 0: return (u8)(0x21u + next() % 94u);
    case 1: return (u8)("ab01_"[next() % 5u]);
    case 2: return (u8)(1u + next() % 255u);
    case 3: return (u8)(next() % 3u ? 0x01u : 0x80u + next() % 128u);
    default: return (u8)(next() % 4u ? 0x41u + next() % 26u : 0x81u + next() % 127u);
    }
}

/* A string of `length` characters at `address`, its terminator, then
 * random bytes (the word loop reads past a terminator). Returns the length. */
static u32 write_string(u8* ram, u32 address, u32 length, unsigned style) {
    for (u32 i = 0; i < length; ++i)
        *at(ram, address + i) = character(style);
    *at(ram, address + length) = 0u;
    for (u32 i = 1; i <= 8u; ++i)
        *at(ram, address + length + i) = (u8)next();
    return length;
}

static u32 string_length(u32 limit) {
    switch (next() % 8u) {
    case 0: return 0u;
    case 1: return next() % 4u;
    case 2: return 64u + next() % 400u;
    default: return next() % (limit < 24u ? limit : 24u);
    }
}

typedef struct Case {
    CPUState cpu;
    bool journal, aliases, boundary_case;
} Case;

/* strcmp's operands: two strings in AREA (r3 the first, r4 the second). */
static void build_strcmp(u8* ram, CPUState* c, unsigned scenario) {
    const unsigned style = next() % 5u;
    const u32 length = string_length(40u);
    const u32 a = AREA + 0x100u + (next() % 0x3000u);
    u32 b = AREA + 0x4000u + (next() % 0x3000u);
    if (next() % 2u) /* the same alignment, most of the time in play */
        b = (b & ~3u) | (a & 3u);
    write_string(ram, a, length, style);
    memcpy(at(ram, b), at(ram, a), length + 1u);
    for (u32 i = 1; i <= 8u; ++i)
        *at(ram, b + length + i) = (u8)next();
    switch (scenario % 6u) {
    case 0: break; /* equal */
    case 1:        /* differing at one byte */
        if (length != 0u)
            *at(ram, b + next() % length) = character(style);
        break;
    case 2: /* the second a prefix of the first */
        if (length != 0u)
            *at(ram, b + next() % (length + 1u)) = 0u;
        break;
    case 3: /* the first a prefix of the second */
        if (length != 0u)
            *at(ram, a + next() % (length + 1u)) = 0u;
        break;
    case 4: /* the second longer */
        *at(ram, b + length) = character(style);
        *at(ram, b + length + 1u) = next() % 2u ? 0u : character(style);
        break;
    default: /* unrelated */
        write_string(ram, b, string_length(40u), next() % 5u);
        break;
    }
    c->gpr[3] = a;
    c->gpr[4] = b;
    if (next() % 3u == 0u) {
        const u32 t = c->gpr[3];
        c->gpr[3] = c->gpr[4];
        c->gpr[4] = t;
    }
    if (scenario % 53u == 7u)
        c->gpr[4] = c->gpr[3]; /* the same string */
    switch (scenario % 47u) {
    case 1: c->gpr[3] = 0xCC000000u; break;
    case 2: c->gpr[4] = 0x00000010u; break;
    case 3: c->gpr[3] |= 0x40000000u; break; /* the uncached mirror */
    case 4: {
        /* Equal strings running into the end of the test's RAM. */
        const u32 n = 1u + next() % 40u;
        const u32 x = GC_RAM_BASE + GC_MAIN_RAM_SIZE - n, y = END_AREA + (next() % 64u);
        for (u32 i = 0; i < n; ++i)
            *at(ram, x + i) = *at(ram, y + i) = (u8)(0x41u + next() % 26u);
        if (next() % 2u)
            *at(ram, y + n) = 0u;
        c->gpr[3] = x;
        c->gpr[4] = next() % 2u ? y : (y & ~3u) | (x & 3u);
        break;
    }
    default: break;
    }
}

/* A table of 825 names, and the name to find. */
static void build_table(u8* ram, unsigned scenario) {
    const unsigned style = scenario % 3u == 0u ? 1u : next() % 2u ? 0u : 4u;
    for (u32 i = 0; i < TABLE_AREA_BYTES; i += 4u)
        put(ram, TABLE_AREA + i, next());
    for (u32 e = 0; e < ENTRIES; ++e) {
        u8* entry = at(ram, TABLE + 12u * e);
        const u32 length = 1u + next() % 8u;
        for (u32 i = 0; i < 8u; ++i)
            entry[i] = i < length ? character(style) : 0u;
        if (length == 8u && next() % 4u == 0u)
            entry[8] = next() % 2u ? 0u : entry[8]; /* an eight-letter name runs into the procname */
    }
}

static void build_stage_name(u8* ram, CPUState* c, unsigned scenario) {
    build_table(ram, scenario);
    const u32 sp = AREA + 0x8000u + 8u * (next() % 0x400u) + (scenario % 23u == 4u ? 1u + next() % 7u : 0u);
    c->gpr[1] = sp;
    u32 name = AREA + 0x100u + next() % 0x3000u;
    const u32 index = next() % 8u == 0u ? ENTRIES - 1u - next() % 4u : next() % ENTRIES;
    const u8* entry = at(ram, TABLE + 12u * index);
    switch (scenario % 9u) {
    case 0: /* a name not in the table (most likely) */
        write_string(ram, name, 1u + next() % 8u, next() % 5u);
        break;
    case 1: /* the empty name */
        write_string(ram, name, 0u, 0u);
        break;
    case 2: { /* a prefix of an entry */
        u32 n = 0;
        while (n < 8u && entry[n] != 0u)
            n++;
        memcpy(at(ram, name), entry, n);
        *at(ram, name + (n > 1u ? next() % n : 0u)) = 0u;
        break;
    }
    case 3: { /* an entry with a character more */
        u32 n = 0;
        while (n < 8u && entry[n] != 0u)
            n++;
        memcpy(at(ram, name), entry, n);
        *at(ram, name + n) = character(0);
        *at(ram, name + n + 1u) = 0u;
        break;
    }
    case 4: /* the entry itself as the name */
        name = TABLE + 12u * index;
        break;
    default: { /* an entry's name (the first entry of that name is found) */
        u32 n = 0;
        while (n < 12u && entry[n] != 0u)
            n++;
        memcpy(at(ram, name), entry, n);
        *at(ram, name + n) = 0u;
        for (u32 i = 1; i <= 8u; ++i)
            *at(ram, name + n + i) = (u8)next();
        break;
    }
    }
    c->gpr[3] = name;
    switch (scenario % 41u) {
    case 1: c->gpr[3] = 0xCC008000u; break;
    case 2: c->gpr[3] = 0u; break;
    case 3: c->gpr[1] = 0xCC010000u; break;
    case 4: c->gpr[1] = GC_RAM_BASE + GC_MAIN_RAM_SIZE - 4u * (next() % 3u); break; /* the frame past the end */
    case 5: c->gpr[1] = GC_RAM_BASE + 16u; break;                                  /* below the start */
    case 6: { /* the name under the frame */
        const u32 n = sp - 32u + next() % 40u;
        memcpy(at(ram, n), entry, 8u);
        *at(ram, n + 8u) = 0u;
        c->gpr[3] = n;
        break;
    }
    case 7: { /* the name just past the frame (no overlap) */
        const u32 n = sp + 8u;
        memcpy(at(ram, n), entry, 8u);
        *at(ram, n + 8u) = 0u;
        c->gpr[3] = n;
        break;
    }
    case 8: c->gpr[1] = END_AREA + 32u + 4u * (next() % 0x3F0u); break; /* the stack in the last page */
    default: break;
    }
    c->reserve_valid = next() % 3u == 0u;
    c->reserve_addr = next() % 2u ? (sp - 12u + 4u * (next() % 5u)) & ~31u : next();
    if (next() % 4u == 0u)
        c->reserve_addr |= 0x40000000u;
}

static Case build(u8* ram, unsigned which, unsigned scenario) {
    Case k;
    memset(&k, 0, sizeof k);
    for (u32 i = 0; i < AREA_BYTES; i += 4u)
        put(ram, AREA + i, next());
    for (u32 i = 0; i < END_AREA_BYTES; i += 4u)
        put(ram, END_AREA + i, next());
    CPUState* c = &k.cpu;
    c->ram = ram;
    c->ram_size = GC_MAIN_RAM_SIZE;
    for (unsigned r = 0; r < 32; ++r) {
        c->gpr[r] = next();
        c->fpr[r] = f64_value(((u64)next() << 32) | next());
        c->ps1[r] = f64_value(((u64)next() << 32) | next());
    }
    c->lr = RETURN_ADDRESS | (next() & 3u);
    c->ctr = next();
    c->cr = next();
    c->xer = next();
    c->msr = next() & ~PPC_MSR_EE;
    c->hid2 = next();
    c->fpscr = next() & ~3u;
    c->reserve_valid = (scenario & 1u) != 0u;
    c->reserve_addr = next();
    c->cycle_observation_suffix = next();
    if (which == 0)
        build_strcmp(ram, c, scenario);
    else
        build_stage_name(ram, c, scenario);
    c->pc = FUNCTIONS[which];
    c->cycle_budget = 16384 + (which ? 32768 : 0);
    c->downcount = -(s64)(next() % 64u);
    c->cycle_deadline_budget = scenario % 4u == 0u ? 0 : 1000000;
    /* The work's length, roughly: strcmp tens of cycles, dStage_searchName
     * up to about 12,500. */
    const s64 span = which == 0 ? 120 : 13000;
    switch (scenario % 29u) {
    case 1: c->cycle_deadline_budget = 1 + (s64)(next() % (u32)span) - c->downcount; break; /* inside the work */
    case 2: c->cycle_deadline_budget = 1 + (s64)(next() % 12u); break;                    /* before a suffix */
    case 3: c->cycle_deadline_budget = -(s64)(next() % 100u); break;
    case 4: c->downcount = -c->cycle_budget; break;
    case 5: c->downcount = -c->cycle_budget + 1 + (s64)(next() % (u32)span); break; /* spent inside the work */
    case 6: c->cycle_budget = 1 + (s64)(next() % (u32)span); c->downcount = 0; break;
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

/* The ways the calls into strcmp's chunk must not be made natively. */
static void boundary_trouble(unsigned scenario) {
    switch ((scenario / 19u) % 7u) {
    case 0: s_sources_dirty = true; break;
    case 1: s_decrementer_pending = true; break; /* with EE, below */
    case 2: s_pi_cause = s_pi_mask = 0x10u; break;
    case 3: bw_edge_filter_enabled = false; break;
    case 4: watch(0x8032DB44u); break;
    case 5: watch(0x80041578u); break;
    default: watch(0xC032DB44u); break; /* the mirror form names the same address */
    }
}

static void report_cpu(const CPUState* got, const CPUState* want) {
    for (unsigned b = 0; b < sizeof *got; ++b)
        if (((const u8*)got)[b] != ((const u8*)want)[b])
            fprintf(stderr, "  CPU byte %u (gpr %u): got %02X want %02X\n", b, b / 4u, ((const u8*)got)[b],
                    ((const u8*)want)[b]);
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
        fprintf(stderr, "usage: native_search_test MODULE.dll [CASES_PER_FUNCTION] [BENCH_CALLS]\n");
        return 2;
    }
    _putenv_s("BLUEWAKE_NATIVE_MATH", "0");
    const unsigned cases = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 60000u;
    const unsigned bench_calls = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 200000u;
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
    u8* before = VirtualAlloc(NULL, GC_MAIN_RAM_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (reference_ram == NULL || native_ram == NULL || before == NULL)
        return 1;

    unsigned ran[2] = {0}, declined[2] = {0}, zero_results[2] = {0}, aligned_paths = 0, word_paths = 0;
    unsigned found = 0, boundary_declined = 0;
    for (unsigned which = 0; which < 2; ++which) {
        const u32 entry = FUNCTIONS[which];
        for (unsigned i = 0; i < cases; ++i) {
            Case k = build(native_ram, which, i);
            copy_areas(before, native_ram);
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
            const int accepted = bluewake_native_search(&native, entry);
            g_mem_write_journal = NULL;
            g_ppc_guest_aliases_overlap_mem1 = false;
            if (!accepted) {
                declined[which]++;
                if (k.boundary_case)
                    boundary_declined++;
                if (memcmp(&native, &untouched, sizeof native) != 0 || !areas_equal(before, native_ram)) {
                    fprintf(stderr, "case %u (%08X): declined but changed state\n", i, entry);
                    report_cpu(&native, &untouched);
                    return 1;
                }
                continue;
            }
            /* What must decline: anything pending, aliases, the boundaries in
             * trouble, and for dStage_searchName a write journal. */
            if (k.cpu.exception || k.aliases || k.boundary_case || (which == 1 && k.journal)) {
                fprintf(stderr, "case %u (%08X): ran where it must decline\n", i, entry);
                return 1;
            }
            ran[which]++;
            zero_results[which] += native.gpr[3] == 0u;
            if (which == 0) {
                aligned_paths += native.cycle_observation_suffix == 2u && k.cpu.cycle_observation_suffix != 2u;
                word_paths += native.gpr[6] == 0x80808080u && k.cpu.gpr[6] != 0x80808080u;
            }

            copy_areas(reference_ram, before);
            CPUState* g = guest_cpu();
            *g = k.cpu;
            g->ram = reference_ram;
            mod->on_state_loaded(g);
            s_unexpected_service = 0;
            if (k.journal)
                g_mem_write_journal = journal; /* strcmp stores nothing */
            const int dispatched = mod->dispatch(g, entry);
            g_mem_write_journal = NULL;
            if (!dispatched) {
                fprintf(stderr, "case %u: the translation did not run\n", i);
                return 1;
            }
            CPUState reference = *g;
            reference.ram = native.ram;
            if (s_unexpected_service != 0u || (reference.pc & ~3u) != RETURN_ADDRESS ||
                memcmp(&native, &reference, sizeof native) != 0 || !areas_equal(native_ram, reference_ram)) {
                fprintf(stderr, "case %u (%08X, seed %08X): mismatch (service asked %u times elsewhere, pc %08X)\n",
                        i, entry, seed, s_unexpected_service, reference.pc);
                report_cpu(&native, &reference);
                report_ram(native_ram, reference_ram);
                return 1;
            }
            if (which == 1 && native.gpr[3] != 0u)
                found++;
        }
        if (which == 0)
            printf("%08X: %u cases, %u identical (%u returning 0; %u through the aligning loop, %u through the word "
                   "loop), %u declined unchanged, 0 mismatches\n",
                   entry, cases, ran[0], zero_results[0], aligned_paths, word_paths, declined[0]);
        else
            printf("%08X: %u cases, %u identical (%u found), %u declined unchanged (%u with a call in trouble), "
                   "0 mismatches\n",
                   entry, cases, ran[1], found, declined[1], boundary_declined);
        fflush(stdout);
        if (ran[which] < 30000u && cases >= 60000u)
            return 1; /* at least 30,000 compared cases per function */
    }

    if (bench_calls != 0u) {
        reset_host();
        CPUState* g = guest_cpu();
        Case k = build(native_ram, 0, 6);
        CPUState base = k.cpu;
        base.exception = 0;
        base.downcount = 0;
        base.cycle_deadline_budget = 0;
        base.cycle_budget = (s64)1 << 50;
        base.reserve_valid = false;
        double t0 = now_ns();
        *g = base;
        g->ram = reference_ram;
        mod->on_state_loaded(g);
        for (unsigned i = 0; i < bench_calls; ++i) {
            g->pc = EMPTY_FUNCTION;
            mod->dispatch(g, EMPTY_FUNCTION);
        }
        const double empty = (now_ns() - t0) / bench_calls;
        printf("an empty dispatch: %.1f ns\n", empty);
        /* strcmp: a first-byte difference (what dStage_searchName's loop
         * meets at almost every entry), equal 7-letter names (its match),
         * and two 40-letter names equal but for the last letter. */
        static const char* const pairs[3][2] = {
            {"ikada_h", "Ygush00"}, {"ikada_h", "ikada_h"}, {"Background_object_number_twenty_nine_A", "Background_object_number_twenty_nine_B"}};
        for (unsigned p = 0; p < 3; ++p) {
            const u32 a = AREA + 0x100u, b = AREA + 0x200u;
            memcpy(at(native_ram, a), pairs[p][0], strlen(pairs[p][0]) + 1u);
            memcpy(at(native_ram, b), pairs[p][1], strlen(pairs[p][1]) + 1u);
            copy_areas(reference_ram, native_ram);
            double best_t = 1e30, best_n = 1e30;
            for (unsigned round = 0; round < 5; ++round) {
                *g = base;
                g->ram = reference_ram;
                mod->on_state_loaded(g);
                t0 = now_ns();
                for (unsigned i = 0; i < bench_calls; ++i) {
                    g->gpr[3] = a;
                    g->gpr[4] = b;
                    g->lr = RETURN_ADDRESS;
                    g->pc = BLUEWAKE_SEARCH_STRCMP;
                    mod->dispatch(g, BLUEWAKE_SEARCH_STRCMP);
                }
                const double t = (now_ns() - t0) / bench_calls;
                CPUState native = base;
                t0 = now_ns();
                for (unsigned i = 0; i < bench_calls; ++i) {
                    native.gpr[3] = a;
                    native.gpr[4] = b;
                    native.lr = RETURN_ADDRESS;
                    if (!bluewake_native_search(&native, BLUEWAKE_SEARCH_STRCMP))
                        return 1;
                }
                const double n = (now_ns() - t0) / bench_calls;
                if (t < best_t) best_t = t;
                if (n < best_n) best_n = n;
            }
            printf("strcmp(\"%s\", \"%s\"): translation %.1f ns/call through the dispatcher, native %.1f ns/call\n",
                   pairs[p][0], pairs[p][1], best_t, best_n);
        }
        /* dStage_searchName: a table whose entries differ from the name at
         * the first byte but for every 16th (which shares it), the name at
         * entry 383 (as "ikada_h" is), and a name it lacks. */
        build_table(native_ram, 1);
        for (u32 e = 0; e < ENTRIES; ++e) {
            u8* entry = at(native_ram, TABLE + 12u * e);
            entry[0] = e % 16u == 0u ? 'i' : (u8)('A' + e % 26u);
        }
        memcpy(at(native_ram, TABLE + 12u * 383u), "ikada_h", 8u);
        const u32 names[2] = {AREA + 0x100u, AREA + 0x200u};
        memcpy(at(native_ram, names[0]), "ikada_h", 8u);
        memcpy(at(native_ram, names[1]), "ikada_x", 8u);
        copy_areas(reference_ram, native_ram);
        for (unsigned p = 0; p < 2; ++p) {
            double best_t = 1e30, best_n = 1e30;
            const unsigned calls = bench_calls / 20u + 1u;
            for (unsigned round = 0; round < 5; ++round) {
                *g = base;
                g->ram = reference_ram;
                g->gpr[1] = AREA + 0x9000u;
                mod->on_state_loaded(g);
                t0 = now_ns();
                for (unsigned i = 0; i < calls; ++i) {
                    g->gpr[3] = names[p];
                    g->lr = RETURN_ADDRESS;
                    g->pc = BLUEWAKE_SEARCH_STAGE_NAME;
                    mod->dispatch(g, BLUEWAKE_SEARCH_STAGE_NAME);
                }
                const double t = (now_ns() - t0) / calls;
                CPUState native = base;
                native.gpr[1] = AREA + 0x9000u;
                t0 = now_ns();
                for (unsigned i = 0; i < calls; ++i) {
                    native.gpr[3] = names[p];
                    native.lr = RETURN_ADDRESS;
                    if (!bluewake_native_search(&native, BLUEWAKE_SEARCH_STAGE_NAME))
                        return 1;
                }
                const double n = (now_ns() - t0) / calls;
                if (t < best_t) best_t = t;
                if (n < best_n) best_n = n;
            }
            printf("dStage_searchName(\"%s\") (%s): translation %.1f ns/call through the dispatcher, native "
                   "%.1f ns/call\n",
                   p == 0 ? "ikada_h" : "ikada_x", p == 0 ? "entry 383" : "not in the table", best_t, best_n);
        }
    }
    return 0;
}
