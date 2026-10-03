/* cmake/composite/native_search.c against the translations it stands in for:
 * strcmp, and dStage_searchName from its entry and from each block leader of
 * its loop it resumes at, in windows that end anywhere in a search.
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
 * flags quiet. Its edge service ends a run wherever it is asked (the return
 * address, or a boundary the module watches), and the address is compared.
 * A guest alias is registered in the module and in the test alike (the REL
 * modules' linked data are such aliases, from 0xC0400000 up), the same bytes
 * in each.
 *
 * strcmp: random strings - equal, differing at any byte, one a prefix of the
 * other, bytes 0x80 and up (which the word loop's zero test cannot tell from
 * a zero), runs of 0x01 - at every pair of alignments, the bytes after each
 * terminator random (the word loop reads them), long strings; either string
 * in the alias or through the uncached mirror (the loads out of line, their
 * suffixes stored); pointers that leave RAM at the first byte, in the byte
 * loops and in the word loop.
 *
 * dStage_searchName: an object-name table of 825 random entries at its
 * address (names of one to eight characters, from a wide or a narrow
 * alphabet so that many share their first bytes; procname and argument
 * after each), searched for one of its names (at any index, the first copy
 * of a repeated one winning), for a name it lacks, a prefix or an extension
 * of one, the empty name, names with high bytes; the name at any alignment,
 * in the alias, through the mirror, inside the table itself, under the
 * function's own frame; the stack at any alignment, at the edge of RAM; a
 * reservation on the frame's words. From the entry, and from the loop's call
 * block, a strcmp's return (with a result of 0 or not) and the step at any
 * index, with the frame a search left (the saved words, the return address);
 * loop registers that are not the loop's own.
 *
 * All: random registers, flags and cycle state; budgets spent at the start,
 * inside the work and just after it; windows as the host gives them, a
 * budget of up to 16,000 cycles with the deadline at its end (or none, or
 * beyond it); deadlines inside the work and before every suffix; an
 * exception pending; aliases over MEM1; a write journal; and for the calls
 * into strcmp's chunk, the host not quiet (sources dirty, a decrementer or
 * processor interrupt the guest would take), the edge filter off, the call or
 * the return address watched.
 *
 * Every case the native runs is compared with the translation run from the
 * same state with the same budget (T). Where the native ran the work whole,
 * every byte of the CPU state (the cycle suffix included) and of the test's
 * memory must equal T's. Where it stopped for the window at a strcmp's
 * return, two more runs: the translation with a budget that ends exactly
 * there (its state must equal the native's, but for the budget itself), and
 * the translation run on from the native's state with the original budget
 * (its state, memory and stopping place must equal T's). Where the native
 * declines, nothing may have changed. RAM outside the test areas is read-only
 * in both images.
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
#define ALIAS 0xC1F00000u /* a guest alias, as a REL module's linked data */
#define ALIAS_BYTES 0x2000u
#define MIRROR(address) ((address) | 0x40000000u)
#define EMPTY_FUNCTION 0x802DB978u /* draw__9J3DPacketFv: blr */
#define RETURN_ADDRESS 0xFFFFFFFCu

static u32 seed = 0x9E3779B9u;
static u32 next(void) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static u8 s_alias_native[ALIAS_BYTES], s_alias_module[ALIAS_BYTES];

static void put(u8* ram, u32 address, u32 value) { write_be32(ram + (address - GC_RAM_BASE), value); }
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

/* The module's edge service: the run ends wherever it is asked. */
static unsigned s_service_count;
static u32 s_service_address;
static int edge_service(void* user, CPUState* cpu, u32 address) {
    (void)user;
    (void)cpu;
    if (s_service_count++ == 0u)
        s_service_address = address;
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

enum { F_STRCMP, F_ENTRY, F_LOOP, F_RESULT, F_STEP, F_COUNT };
static const u32 FUNCTIONS[F_COUNT] = {BLUEWAKE_SEARCH_STRCMP, BLUEWAKE_SEARCH_STAGE_NAME, BLUEWAKE_SEARCH_NAME_LOOP,
                                       BLUEWAKE_SEARCH_NAME_RESULT, BLUEWAKE_SEARCH_NAME_STEP};
static const char* const NAMES[F_COUNT] = {"strcmp", "dStage_searchName", "dStage_searchName from its call block",
                                           "dStage_searchName from a strcmp's return",
                                           "dStage_searchName from its step"};

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
 * random bytes (the word loop reads past a terminator). */
static void write_string(u8* ram, u32 address, u32 length, unsigned style) {
    for (u32 i = 0; i < length; ++i)
        *at(ram, address + i) = character(style);
    *at(ram, address + length) = 0u;
    for (u32 i = 1; i <= 8u; ++i)
        *at(ram, address + length + i) = (u8)next();
}

static void copy_bytes(u8* ram, u32 to, u32 from, u32 count) {
    for (u32 i = 0; i < count; ++i)
        *at(ram, to + i) = *at(ram, from + i);
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
    bool journal, aliases, boundary_case, malformed;
} Case;

/* Where a string goes: the area, the alias, or the area through the mirror. */
static u32 place(u32 area_address, u32 alias_offset, unsigned kind) {
    switch (kind) {
    case 1: return ALIAS + alias_offset;
    case 2: return MIRROR(area_address);
    default: return area_address;
    }
}

/* strcmp's operands: two strings (r3 the first, r4 the second). */
static void build_strcmp(u8* ram, CPUState* c, unsigned scenario) {
    const unsigned style = next() % 5u;
    const u32 length = string_length(40u);
    const unsigned kind_a = scenario % 7u == 2u ? 1u : scenario % 11u == 3u ? 2u : 0u;
    const unsigned kind_b = scenario % 7u == 4u ? 1u : scenario % 13u == 3u ? 2u : 0u;
    const u32 a = place(AREA + 0x100u + (next() % 0x3000u), next() % 0x600u, kind_a);
    u32 b = place(AREA + 0x4000u + (next() % 0x3000u), 0x800u + next() % 0x600u, kind_b);
    if (next() % 2u) /* the same alignment, most of the time in play */
        b = (b & ~3u) | (a & 3u);
    write_string(ram, a, length, style);
    copy_bytes(ram, b, a, length + 1u);
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
    case 1: c->gpr[3] = 0xCC000000u; break; /* the hardware */
    case 2: c->gpr[4] = 0x00000010u; break;
    case 3: c->gpr[3] = 0xC1000000u + (next() & 0xFFFFu); break; /* an address no alias backs */
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
    case 5: { /* equal strings running out of the alias */
        const u32 n = 1u + next() % 20u;
        const u32 x = ALIAS + ALIAS_BYTES - n, y = AREA + 0x9000u + next() % 64u;
        for (u32 i = 0; i < n; ++i)
            *at(ram, x + i) = *at(ram, y + i) = (u8)(0x41u + next() % 26u);
        c->gpr[3] = x;
        c->gpr[4] = y;
        break;
    }
    default: break;
    }
}

/* A table of 825 names. */
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

/* The name to find, somewhere (r3 for the entry, r29 for a resumption). */
static u32 build_name(u8* ram, unsigned scenario, u32 sp, bool entry_call) {
    const unsigned kind = scenario % 5u == 1u ? 1u : scenario % 7u == 2u ? 2u : 0u;
    u32 name = place(AREA + 0x100u + next() % 0x3000u, next() % 0x1F00u, kind);
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
        for (u32 i = 0; i < n; ++i)
            *at(ram, name + i) = entry[i];
        *at(ram, name + (n > 1u ? next() % n : 0u)) = 0u;
        break;
    }
    case 3: { /* an entry with a character more */
        u32 n = 0;
        while (n < 8u && entry[n] != 0u)
            n++;
        for (u32 i = 0; i < n; ++i)
            *at(ram, name + i) = entry[i];
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
        for (u32 i = 0; i < n; ++i)
            *at(ram, name + i) = entry[i];
        *at(ram, name + n) = 0u;
        for (u32 i = 1; i <= 8u; ++i)
            *at(ram, name + n + i) = (u8)next();
        break;
    }
    }
    if (entry_call && scenario % 41u == 6u) { /* the name under the frame */
        const u32 n = sp - 32u + next() % 40u;
        memcpy(at(ram, n), entry, 8u);
        *at(ram, n + 8u) = 0u;
        name = next() % 2u ? n : MIRROR(n);
    }
    if (entry_call && scenario % 41u == 7u) { /* the name just past the frame (no overlap) */
        const u32 n = sp + 8u;
        memcpy(at(ram, n), entry, 8u);
        *at(ram, n + 8u) = 0u;
        name = n;
    }
    return name;
}

/* dStage_searchName from its entry: the name in r3, the frame below r1. */
static void build_entry(u8* ram, CPUState* c, unsigned scenario) {
    build_table(ram, scenario);
    const u32 sp = AREA + 0x8000u + 8u * (next() % 0x400u) + (scenario % 23u == 4u ? 1u + next() % 7u : 0u);
    c->gpr[1] = sp;
    c->gpr[3] = build_name(ram, scenario, sp, true);
    switch (scenario % 41u) {
    case 1: c->gpr[3] = 0xCC008000u; break;
    case 2: c->gpr[3] = 0u; break;
    case 3: c->gpr[1] = 0xCC010000u; break;
    case 4: c->gpr[1] = GC_RAM_BASE + GC_MAIN_RAM_SIZE - 4u * (next() % 3u); break; /* the frame past the end */
    case 5: c->gpr[1] = GC_RAM_BASE + 16u; break;                                  /* below the start */
    case 8: c->gpr[1] = END_AREA + 32u + 4u * (next() % 0x3F0u); break; /* the stack in the last page */
    default: break;
    }
    c->reserve_valid = next() % 3u == 0u;
    c->reserve_addr = next() % 2u ? (sp - 12u + 4u * (next() % 5u)) & ~31u : next();
    if (next() % 4u == 0u)
        c->reserve_addr |= 0x40000000u;
}

/* dStage_searchName at a block leader of its loop: the frame a search left
 * (the caller's r29 to r31 and the return address saved in it), r29 the
 * name, r30 the index and r31 its entry; at a strcmp's return, r3 its
 * result. */
static bool build_resume(u8* ram, CPUState* c, unsigned scenario, unsigned which) {
    build_table(ram, scenario);
    const u32 frame = AREA + 0x8000u + 8u * (next() % 0x400u) + (scenario % 23u == 4u ? 1u + next() % 7u : 0u);
    c->gpr[1] = frame;
    put(ram, frame, frame + 32u);
    put(ram, frame + 20u, next());
    put(ram, frame + 24u, next());
    put(ram, frame + 28u, next());
    put(ram, frame + 36u, RETURN_ADDRESS | (next() & 3u));
    c->gpr[11] = next() % 2u ? frame + 32u : next();
    const u32 name = build_name(ram, scenario, frame + 32u, false);
    c->gpr[29] = name;
    u32 index = next() % ENTRIES;
    if (next() % 6u == 0u)
        index = ENTRIES - 1u - next() % 3u; /* at the table's end */
    c->gpr[30] = index;
    c->gpr[31] = TABLE + 12u * index;
    if (which == F_RESULT)
        c->gpr[3] = next() % 4u == 0u ? 0u : next() % 2u ? (u32)(s32)(s8)next() : next();
    bool malformed = false;
    switch (scenario % 43u) {
    case 1: c->gpr[31] += 4u; malformed = true; break;      /* not the index's entry */
    case 2: c->gpr[30] = ENTRIES + next() % 8u; c->gpr[31] = TABLE + 12u * c->gpr[30]; malformed = true; break;
    case 3: c->gpr[1] = GC_RAM_BASE + GC_MAIN_RAM_SIZE - 20u; break; /* the frame past the end (for the epilogue) */
    case 4: c->gpr[29] = 0xCC008000u; break;
    default: break;
    }
    return malformed;
}

static Case build(u8* ram, unsigned which, unsigned scenario) {
    Case k;
    memset(&k, 0, sizeof k);
    for (u32 i = 0; i < AREA_BYTES; i += 4u)
        put(ram, AREA + i, next());
    for (u32 i = 0; i < END_AREA_BYTES; i += 4u)
        put(ram, END_AREA + i, next());
    for (u32 i = 0; i < ALIAS_BYTES; ++i)
        s_alias_native[i] = (u8)next();
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
    if (which == F_STRCMP)
        build_strcmp(ram, c, scenario);
    else if (which == F_ENTRY)
        build_entry(ram, c, scenario);
    else
        k.malformed = build_resume(ram, c, scenario, which);
    c->pc = FUNCTIONS[which];
    c->cycle_budget = 16384 + (which ? 32768 : 0);
    c->downcount = -(s64)(next() % 64u);
    c->cycle_deadline_budget = scenario % 4u == 0u ? 0 : 1000000;
    /* The work's length, roughly: strcmp tens of cycles, dStage_searchName
     * up to about 12,500. */
    const s64 span = which == F_STRCMP ? 120 : 13000;
    if (which != F_STRCMP && scenario % 2u == 0u) {
        /* A window as the host gives one: up to 16,000 cycles, its end the
         * next device deadline (or no deadline, or one beyond it). */
        c->downcount = next() % 3u ? 0 : -(s64)(next() % 64u);
        c->cycle_budget = 1 + (s64)(next() % 16000u);
        switch (next() % 4u) {
        case 0: c->cycle_deadline_budget = 0; break;
        case 1: c->cycle_deadline_budget = c->cycle_budget + 1 + (s64)(next() % 500u); break;
        default: c->cycle_deadline_budget = c->cycle_budget; break;
        }
    } else {
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
    }
    k.journal = scenario % 31u == 7u;
    k.aliases = scenario % 37u == 8u;
    k.boundary_case = which != F_STRCMP && scenario % 19u == 5u;
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

/* The module, its RAM and its guest CPU. */
static const StaticRecompModuleDesc* s_mod;
static CPUState* (*s_guest_cpu)(void);
static u8* s_reference_ram;

/* The translation from `state` at `pc` on `areas`' RAM (and the alias's
 * bytes as they are): the state it ends in, its RAM left in the module's
 * image, where its edge service was asked. */
static CPUState run_module(const CPUState* state, const u8* areas, u32 pc, unsigned* service_count,
                           u32* service_address) {
    copy_areas(s_reference_ram, areas);
    memcpy(s_alias_module, s_alias_native, ALIAS_BYTES);
    CPUState* g = s_guest_cpu();
    *g = *state;
    g->ram = s_reference_ram;
    g->pc = pc;
    s_mod->on_state_loaded(g);
    s_service_count = 0;
    s_service_address = 0u;
    if (!s_mod->dispatch(g, pc)) {
        fprintf(stderr, "the translation did not run at %08X\n", pc);
        exit(1);
    }
    *service_count = s_service_count;
    *service_address = s_service_address;
    CPUState out = *g;
    out.ram = state->ram;
    return out;
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
    StaticRecompGetModuleFn get_module =
        (StaticRecompGetModuleFn)(void*)GetProcAddress(lib, STATICRECOMP_GET_MODULE_SYMBOL);
    s_guest_cpu = (CPUState * (*)(void))(void*)GetProcAddress(lib, "bluewake_composite_guest_cpu");
    void (*set_edge)(int (*)(void*, CPUState*, u32), void*) =
        (void (*)(int (*)(void*, CPUState*, u32), void*))(void*)GetProcAddress(lib, "bluewake_set_edge_service");
    int (*direct)(bool, const bool*, const bool*, const u32*, const u32*) =
        (int (*)(bool, const bool*, const bool*, const u32*, const u32*))(void*)GetProcAddress(
            lib, "bluewake_composite_direct_calls");
    int (*filter)(bool) = (int (*)(bool))(void*)GetProcAddress(lib, "bluewake_composite_edge_filter");
    bool (*module_alias_add)(u32, u32, u8*) =
        (bool (*)(u32, u32, u8*))(void*)GetProcAddress(lib, "ppc_guest_alias_add_shared");
    if (get_module == NULL || s_guest_cpu == NULL || set_edge == NULL || direct == NULL || filter == NULL ||
        module_alias_add == NULL) {
        fprintf(stderr, "not a BlueWake Windows module\n");
        return 1;
    }
    s_mod = get_module();
    if (s_mod->cpu_state_size != sizeof(CPUState) || strcmp(s_mod->game_id, "GZLE01") != 0) {
        fprintf(stderr, "CPU state size %u, expected %u\n", s_mod->cpu_state_size, (unsigned)sizeof(CPUState));
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
    /* The alias, in the module's registry and in the test's. */
    if (!module_alias_add(ALIAS, ALIAS_BYTES, s_alias_module) || !ppc_guest_alias_add_shared(ALIAS, ALIAS_BYTES, s_alias_native) ||
        g_ppc_guest_aliases_overlap_mem1) {
        fprintf(stderr, "cannot register the alias\n");
        return 1;
    }
    s_reference_ram = module_image(lib);
    u8* native_ram = protect_image(VirtualAlloc(NULL, GC_MAIN_RAM_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    u8* before = VirtualAlloc(NULL, GC_MAIN_RAM_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    u8* t_ram = VirtualAlloc(NULL, GC_MAIN_RAM_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    static u8 alias_before[ALIAS_BYTES];
    if (s_reference_ram == NULL || native_ram == NULL || before == NULL || t_ram == NULL)
        return 1;

    for (unsigned which = 0; which < F_COUNT; ++which) {
        const u32 entry = FUNCTIONS[which];
        unsigned ran = 0, declined = 0, whole = 0, partial = 0, zero_results = 0, found = 0, stopped_t = 0;
        unsigned slow_reads = 0, trouble_declined = 0;
        for (unsigned i = 0; i < cases; ++i) {
            Case k = build(native_ram, which, i);
            copy_areas(before, native_ram);
            memcpy(alias_before, s_alias_native, ALIAS_BYTES);
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
                declined++;
                trouble_declined += k.boundary_case;
                if (memcmp(&native, &untouched, sizeof native) != 0 || !areas_equal(before, native_ram) ||
                    memcmp(alias_before, s_alias_native, ALIAS_BYTES) != 0) {
                    fprintf(stderr, "case %u (%s): declined but changed state\n", i, NAMES[which]);
                    report_cpu(&native, &untouched);
                    return 1;
                }
                continue;
            }
            /* What must decline: anything pending, aliases over MEM1, the calls
             * in trouble, loop registers not the loop's, and for the entry
             * (whose prologue stores) a write journal. */
            if (k.cpu.exception || k.aliases || k.boundary_case || k.malformed || (which == F_ENTRY && k.journal)) {
                fprintf(stderr, "case %u (%s): ran where it must decline\n", i, NAMES[which]);
                return 1;
            }
            ran++;
            /* A string read out of line: in the alias or through the mirror. */
            slow_reads += which == F_STRCMP ? ((k.cpu.gpr[3] | k.cpu.gpr[4]) & 0x40000000u) != 0u
                          : which == F_ENTRY ? (k.cpu.gpr[3] & 0x40000000u) != 0u
                                             : (k.cpu.gpr[29] & 0x40000000u) != 0u;

            /* T: the translation from the same state with the same budget. */
            unsigned t_count, r_count;
            u32 t_address, r_address;
            const CPUState t = run_module(&k.cpu, before, entry, &t_count, &t_address);
            copy_areas(t_ram, s_reference_ram);
            stopped_t += (t.pc & ~3u) != RETURN_ADDRESS;
            const bool stopped_native = which != F_STRCMP && native.pc == BLUEWAKE_SEARCH_NAME_RESULT;
            if (!stopped_native) {
                whole++;
                zero_results += native.gpr[3] == 0u;
                found += which != F_STRCMP && native.gpr[3] != 0u;
                if (t_count != 1u || (t.pc & ~3u) != RETURN_ADDRESS || memcmp(&native, &t, sizeof native) != 0 ||
                    !areas_equal(native_ram, t_ram) || memcmp(s_alias_module, s_alias_native, ALIAS_BYTES) != 0) {
                    fprintf(stderr, "case %u (%s, seed %08X): mismatch (service asked %u times, at %08X; pc %08X)\n",
                            i, NAMES[which], seed, t_count, t_address, t.pc);
                    report_cpu(&native, &t);
                    report_ram(native_ram, t_ram);
                    return 1;
                }
                continue;
            }
            partial++;
            /* The translation with a budget that ends where the native stopped:
             * the same state there. */
            if (native.downcount < 0) {
                CPUState budgeted = k.cpu;
                budgeted.cycle_budget = -native.downcount;
                CPUState r = run_module(&budgeted, before, entry, &r_count, &r_address);
                r.cycle_budget = native.cycle_budget;
                if (r_count != 0u || memcmp(&native, &r, sizeof native) != 0 ||
                    !areas_equal(native_ram, s_reference_ram)) {
                    fprintf(stderr, "case %u (%s, seed %08X): the translation stopped there differs (service "
                                    "asked %u times, pc %08X)\n",
                            i, NAMES[which], seed, r_count, r.pc);
                    report_cpu(&native, &r);
                    report_ram(native_ram, s_reference_ram);
                    return 1;
                }
            }
            /* The translation on from there: where T ends, as T. */
            CPUState r = run_module(&native, native_ram, native.pc, &r_count, &r_address);
            if (r_count != t_count || r_address != t_address || memcmp(&r, &t, sizeof r) != 0 ||
                !areas_equal(s_reference_ram, t_ram)) {
                fprintf(stderr, "case %u (%s, seed %08X): the translation on from the native differs (service "
                                "asked %u/%u times, at %08X/%08X)\n",
                        i, NAMES[which], seed, r_count, t_count, r_address, t_address);
                report_cpu(&r, &t);
                report_ram(s_reference_ram, t_ram);
                return 1;
            }
        }
        if (which == F_STRCMP)
            printf("%08X %s: %u cases, %u identical (%u returning 0; %u through the out-of-line path), %u declined "
                   "unchanged, 0 mismatches\n",
                   entry, NAMES[which], cases, ran, zero_results, slow_reads, declined);
        else
            printf("%08X %s: %u cases, %u identical - %u run whole (%u found), %u stopped for the window at a "
                   "strcmp's return and checked there and on from there (the translation itself stopped in %u); "
                   "%u with the name read out of line - %u declined unchanged (%u with a call in trouble), "
                   "0 mismatches\n",
                   entry, NAMES[which], cases, ran, whole, found, partial, stopped_t, slow_reads, declined,
                   trouble_declined);
        fflush(stdout);
        if (ran < 30000u && cases >= 60000u)
            return 1; /* at least 30,000 compared cases per function */
    }
    bluewake_native_search_report();

    if (bench_calls != 0u) {
        reset_host();
        CPUState* g = s_guest_cpu();
        Case k = build(native_ram, 0, 6);
        CPUState base = k.cpu;
        base.exception = 0;
        base.downcount = 0;
        base.cycle_deadline_budget = 0;
        base.cycle_budget = (s64)1 << 50;
        base.reserve_valid = false;
        double t0 = now_ns();
        *g = base;
        g->ram = s_reference_ram;
        s_mod->on_state_loaded(g);
        for (unsigned i = 0; i < bench_calls; ++i) {
            g->pc = EMPTY_FUNCTION;
            s_mod->dispatch(g, EMPTY_FUNCTION);
        }
        const double empty = (now_ns() - t0) / bench_calls;
        printf("an empty dispatch: %.1f ns\n", empty);
        /* strcmp: a first-byte difference (what dStage_searchName's loop
         * meets at almost every entry), equal 7-letter names (its match),
         * two 40-letter names equal but for the last letter, and a name in
         * the alias (an actor's literal) against one in MEM1. */
        static const char* const pairs[4][2] = {{"ikada_h", "Ygush00"},
                                                {"ikada_h", "ikada_h"},
                                                {"Background_object_number_twenty_nine_A",
                                                 "Background_object_number_twenty_nine_B"},
                                                {"Ikada", "Ikada"}};
        for (unsigned p = 0; p < 4; ++p) {
            const u32 a = p == 3u ? ALIAS + 0x100u : AREA + 0x100u, b = AREA + 0x200u;
            for (u32 i = 0; i <= strlen(pairs[p][0]); ++i)
                *at(native_ram, a + i) = (u8)pairs[p][0][i];
            for (u32 i = 0; i <= strlen(pairs[p][1]); ++i)
                *at(native_ram, b + i) = (u8)pairs[p][1][i];
            copy_areas(s_reference_ram, native_ram);
            memcpy(s_alias_module, s_alias_native, ALIAS_BYTES);
            double best_t = 1e30, best_n = 1e30;
            for (unsigned round = 0; round < 5; ++round) {
                *g = base;
                g->ram = s_reference_ram;
                s_mod->on_state_loaded(g);
                t0 = now_ns();
                for (unsigned i = 0; i < bench_calls; ++i) {
                    g->gpr[3] = a;
                    g->gpr[4] = b;
                    g->lr = RETURN_ADDRESS;
                    g->pc = BLUEWAKE_SEARCH_STRCMP;
                    s_mod->dispatch(g, BLUEWAKE_SEARCH_STRCMP);
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
            printf("strcmp(\"%s\"%s, \"%s\"): translation %.1f ns/call through the dispatcher, native %.1f ns/call\n",
                   pairs[p][0], p == 3u ? " in the alias" : "", pairs[p][1], best_t, best_n);
        }
        /* dStage_searchName whole: tables whose entries differ from the name
         * at the first byte but for three before entry 383 (as at Dragon
         * Roost, where "ikada_h" is entry 383 and only "item", "itemFLY" and
         * "itemDek" share its first letter), or for every 16th; the name at
         * entry 383, and a name the table lacks; the name in MEM1 or in the
         * alias. */
        const u32 names[3] = {AREA + 0x100u, AREA + 0x200u, ALIAS + 0x300u};
        for (unsigned v = 0; v < 4u; ++v) {
            const bool dense = v == 1u;
            const unsigned p = v == 2u ? 1u : v == 3u ? 2u : 0u;
            build_table(native_ram, 1);
            for (u32 e = 0; e < ENTRIES; ++e) {
                u8* entry = at(native_ram, TABLE + 12u * e);
                entry[0] = dense && e % 16u == 0u ? 'i' : (u8)('A' + e % 26u);
            }
            if (!dense)
                for (u32 e = 0; e < 3u; ++e)
                    memcpy(at(native_ram, TABLE + 12u * (100u * e + 7u)), "itemFLY", 8u);
            memcpy(at(native_ram, TABLE + 12u * 383u), "ikada_h", 8u);
            memcpy(at(native_ram, names[0]), "ikada_h", 8u);
            memcpy(at(native_ram, names[1]), "ikada_x", 8u);
            memcpy(at(native_ram, names[2]), "ikada_h", 8u);
            copy_areas(s_reference_ram, native_ram);
            memcpy(s_alias_module, s_alias_native, ALIAS_BYTES);
            double best_t = 1e30, best_n = 1e30;
            const unsigned calls = bench_calls / 20u + 1u;
            for (unsigned round = 0; round < 5; ++round) {
                *g = base;
                g->ram = s_reference_ram;
                g->gpr[1] = AREA + 0x9000u;
                s_mod->on_state_loaded(g);
                t0 = now_ns();
                for (unsigned i = 0; i < calls; ++i) {
                    g->gpr[3] = names[p];
                    g->lr = RETURN_ADDRESS;
                    g->pc = BLUEWAKE_SEARCH_STAGE_NAME;
                    s_mod->dispatch(g, BLUEWAKE_SEARCH_STAGE_NAME);
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
            printf("dStage_searchName(\"%s\"%s) (%s; %s): translation %.1f ns/call through the dispatcher, native "
                   "%.1f ns/call\n",
                   p == 1u ? "ikada_x" : "ikada_h", p == 2u ? " in the alias" : "",
                   p == 1u ? "not in the table" : "entry 383",
                   dense ? "every 16th entry shares its first letter" : "3 entries share its first letter", best_t,
                   best_n);
        }
    }
    return 0;
}
