/* The fourth set's comparison harness (tests/native_kankyo_test.c,
 * tests/native_anim_test.c, tests/native_cc_test.c): a Windows game module
 * (gGZLE01_recomp.dll) loaded read-only, its translation run from a
 * function's entry as in play (direct calls and the edge filter on, the
 * host's flags quiet, an edge service that ends the run at the return
 * address and counts any other question), two RAM images whose every page is
 * read-only but the test's own, and the comparisons: every byte of the CPU
 * state and of the writable pages.
 *
 * Each test hands harness_load its writable regions. */
#ifndef BLUEWAKE_NATIVE4_HARNESS_H
#define BLUEWAKE_NATIVE4_HARNESS_H

#include "direct_calls.h"
#include "StaticRecompABI.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <xmmintrin.h>

#ifndef NATIVE4_HOOKED
/* direct_calls.c, linked in for the edge filter's state the natives read,
 * names these. */
BwChunkFn bw_find_chunk(u32 address) {
    (void)address;
    return NULL;
}
BwChunkFn* const bw_chunk_fns = NULL;
#endif

#define RETURN_ADDRESS 0xFFFFFFFCu

typedef struct Region {
    u32 start, bytes;
} Region;

static const Region* REGIONS; /* the test's writable pages */
static unsigned REGION_COUNT;

/* --- Random numbers. ----------------------------------------------------- */

static u32 seed = 0x9E3779B9u;
static u32 next(void) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}
static u32 below(u32 n) { return next() % n; }
static f64 unit(void) { return (f64)(next() >> 8) / 16777216.0; }

/* A single's bits: mostly ordinary, sometimes a zero, a denormal, a huge
 * value, an infinity or a NaN (quiet or signalling). */
static u32 any_single(unsigned specials_in_64) {
    const u32 sign = next() & 0x80000000u;
    const u32 kind = below(64u);
    if (kind < specials_in_64) {
        switch (below(7u)) {
        case 0: return sign;
        case 1: return sign | (1u + below(0x007FFFFFu));
        case 2: return sign | 0x7F800000u;
        case 3: return sign | 0x7FC00000u | (next() & 0x003FFFFFu);
        case 4: return sign | 0x7F800000u | (1u + below(0x003FFFFFu));
        case 5: return sign | ((200u + below(54u)) << 23) | (next() & 0x007FFFFFu);
        default: return sign | ((1u + below(30u)) << 23) | (next() & 0x007FFFFFu);
        }
    }
    return sign | ((110u + below(30u)) << 23) | (next() & 0x007FFFFFu);
}

/* A register double: a single's value mostly, sometimes any double (not a
 * single: more mantissa, a double-only exponent, a denormal, near the top). */
static f64 any_register(unsigned specials_in_64) {
    switch (below(16u)) {
    case 0: return f64_value(((u64)next() << 32) | next());
    case 1: return f64_value(((u64)(next() & 0x800FFFFFu) << 32) | next()); /* a double denormal */
    case 2: return f64_value(((u64)((next() & 0x80000000u) | 0x7FEFFFFFu) << 32) | (0xF8000000u | next()));
    default: return f64_value(convert_to_double(any_single(specials_in_64)));
    }
}

static void put32(u8* ram, u32 address, u32 value) { write_be32(ram + (address - GC_RAM_BASE), value); }
static void put64(u8* ram, u32 address, u64 value) { write_be64(ram + (address - GC_RAM_BASE), value); }
static void put16(u8* ram, u32 address, u16 value) { write_be16(ram + (address - GC_RAM_BASE), value); }
static u32 get32(const u8* ram, u32 address) { return read_be32(ram + (address - GC_RAM_BASE)); }

/* --- Random machine state. ----------------------------------------------- */

static void random_cpu(CPUState* c, u8* ram) {
    memset(c, 0, sizeof *c);
    c->ram = ram;
    c->ram_size = GC_MAIN_RAM_SIZE;
    for (unsigned r = 0; r < 32; ++r) {
        c->gpr[r] = next();
        c->fpr[r] = any_register(8u);
        c->ps1[r] = any_register(8u);
    }
    c->lr = RETURN_ADDRESS | (next() & 3u);
    c->ctr = next();
    c->cr = next();
    c->xer = next();
    c->msr = PPC_MSR_FP | (next() & ~(PPC_MSR_FP | PPC_MSR_EE));
    c->hid2 = PPC_HID2_LSQE | (next() & 0x0FFFFFFFu);
    for (unsigned g = 1; g < 8; ++g)
        c->gqr[g] = next();
    c->gqr[0] = 0u;
    /* FPSCR: random flags and enables (VE among them), NI sometimes, rounding
     * mostly to nearest; VX and FEX as ppc_fpscr_updated leaves them. */
    c->fpscr = next() & ~(0x60000003u | 0x4u);
    if (below(4u) == 0u)
        c->fpscr |= 0x4u;
    if (below(32u) == 0u)
        c->fpscr |= 1u + below(3u);
    ppc_fpscr_updated(c);
    c->reserve_valid = below(2u) != 0u;
    c->reserve_addr = next();
    c->cycle_observation_suffix = next();
    c->cycle_budget = 16384;
    c->downcount = -(s64)below(64u);
    c->cycle_deadline_budget = below(4u) == 0u ? 0 : 100000;
}

/* --- The module. --------------------------------------------------------- */

typedef struct Harness {
    HMODULE lib;
    const StaticRecompModuleDesc* mod;
    CPUState* (*guest_cpu)(void);
    int (*filter)(bool);
    u8* reference_ram;
    u8* native_ram;
    u8* before;
} Harness;

static unsigned s_unexpected_service;
static u32 s_unexpected_at;
static int edge_service(void* user, CPUState* cpu, u32 address) {
    (void)user;
    (void)cpu;
    if ((address & ~3u) != RETURN_ADDRESS) {
        s_unexpected_service++;
        s_unexpected_at = address;
    }
    return 1;
}

static void journal(u32 offset, u32 size, void* user) {
    (void)offset;
    (void)size;
    (void)user;
    abort();
}

static u8* protect_image(u8* p) {
    DWORD old;
    if (p == NULL || !VirtualProtect(p, GC_MAIN_RAM_SIZE, PAGE_READONLY, &old))
        return NULL;
    for (unsigned i = 0; i < REGION_COUNT; ++i)
        if (!VirtualProtect(p + (REGIONS[i].start - GC_RAM_BASE), REGIONS[i].bytes, PAGE_READWRITE, &old))
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

/* The test's own host flags and watch list: the ones the natives read. */
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
    watch(0x80006000u); /* a few unrelated addresses, so the table is not empty */
    watch(0x802F5000u);
    watch(0x8030D000u);
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

static int harness_load(Harness* h, const char* path, bool module_natives, const Region* regions,
                        unsigned region_count) {
    REGIONS = regions;
    REGION_COUNT = region_count;
    _putenv_s("BLUEWAKE_NATIVE_MATH", module_natives ? "1" : "0");
    h->lib = LoadLibraryA(path);
    if (h->lib == NULL) {
        fprintf(stderr, "cannot load %s (%lu)\n", path, GetLastError());
        return 0;
    }
    StaticRecompGetModuleFn get =
        (StaticRecompGetModuleFn)(void*)GetProcAddress(h->lib, STATICRECOMP_GET_MODULE_SYMBOL);
    h->guest_cpu = (CPUState * (*)(void))(void*)GetProcAddress(h->lib, "bluewake_composite_guest_cpu");
    void (*set_edge)(int (*)(void*, CPUState*, u32), void*) =
        (void (*)(int (*)(void*, CPUState*, u32), void*))(void*)GetProcAddress(h->lib, "bluewake_set_edge_service");
    int (*direct)(bool, const bool*, const bool*, const u32*, const u32*) =
        (int (*)(bool, const bool*, const bool*, const u32*, const u32*))(void*)GetProcAddress(
            h->lib, "bluewake_composite_direct_calls");
    int (*filter)(bool) = (int (*)(bool))(void*)GetProcAddress(h->lib, "bluewake_composite_edge_filter");
    if (get == NULL || h->guest_cpu == NULL || set_edge == NULL || direct == NULL || filter == NULL) {
        fprintf(stderr, "not a BlueWake Windows module\n");
        return 0;
    }
    h->mod = get();
    if (h->mod->cpu_state_size != sizeof(CPUState) || strcmp(h->mod->game_id, "GZLE01") != 0) {
        fprintf(stderr, "CPU state size %u, expected %u\n", h->mod->cpu_state_size, (unsigned)sizeof(CPUState));
        return 0;
    }
    /* The module reads the test's own host flags, so a case that makes the
     * host busy makes it busy for the translation too: it then asks its edge
     * service at every boundary it crosses, and a native that ran there fails
     * the comparison. */
    h->filter = filter;
    if (!direct(true, &s_sources_dirty, &s_decrementer_pending, &s_pi_cause, &s_pi_mask) || !filter(true)) {
        fprintf(stderr, "the module's direct calls or edge filter are unavailable\n");
        return 0;
    }
    set_edge(edge_service, NULL);
    h->reference_ram = module_image(h->lib);
    h->native_ram = protect_image(VirtualAlloc(NULL, GC_MAIN_RAM_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    h->before = VirtualAlloc(NULL, GC_MAIN_RAM_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (h->reference_ram == NULL || h->native_ram == NULL || h->before == NULL) {
        fprintf(stderr, "cannot set up the RAM images\n");
        return 0;
    }
    reset_host();
    return 1;
}

static void copy_regions(u8* to, const u8* from) {
    for (unsigned i = 0; i < REGION_COUNT; ++i)
        memcpy(to + (REGIONS[i].start - GC_RAM_BASE), from + (REGIONS[i].start - GC_RAM_BASE), REGIONS[i].bytes);
}

static bool same_regions(const u8* a, const u8* b) {
    for (unsigned i = 0; i < REGION_COUNT; ++i)
        if (memcmp(a + (REGIONS[i].start - GC_RAM_BASE), b + (REGIONS[i].start - GC_RAM_BASE), REGIONS[i].bytes) != 0)
            return false;
    return true;
}

static void report_cpu(const CPUState* got, const CPUState* want) {
    for (unsigned b = 0; b < sizeof *got; ++b)
        if (((const u8*)got)[b] != ((const u8*)want)[b])
            fprintf(stderr, "  CPU byte %u: got %02X want %02X\n", b, ((const u8*)got)[b], ((const u8*)want)[b]);
    fprintf(stderr, "  (gpr at %u, fpr %u, ps1 %u, pc %u, cr %u, fpscr %u, downcount %u, suffix %u)\n",
            (unsigned)offsetof(CPUState, gpr), (unsigned)offsetof(CPUState, fpr), (unsigned)offsetof(CPUState, ps1),
            (unsigned)offsetof(CPUState, pc), (unsigned)offsetof(CPUState, cr), (unsigned)offsetof(CPUState, fpscr),
            (unsigned)offsetof(CPUState, downcount), (unsigned)offsetof(CPUState, cycle_observation_suffix));
}

static void report_ram(const u8* got, const u8* want) {
    int differ = 0;
    for (unsigned r = 0; r < REGION_COUNT; ++r)
        for (u32 i = 0; i < REGIONS[r].bytes; ++i) {
            const u32 at = REGIONS[r].start + i - GC_RAM_BASE;
            if (got[at] != want[at] && differ++ < 48)
                fprintf(stderr, "  RAM %08X: got %02X want %02X\n", at + GC_RAM_BASE, got[at], want[at]);
        }
}

/* The host's floating-point mode as the module arms it for the guest's FPSCR
 * (ppc_fpscr_control_updated): NI gives FTZ and DAZ. In a case, sometimes. */
static unsigned s_mxcsr_default;
static void host_fp_mode(bool flush) {
    if (s_mxcsr_default == 0u)
        s_mxcsr_default = _mm_getcsr();
    _mm_setcsr(flush ? (s_mxcsr_default | 0x8040u) : s_mxcsr_default);
}

/* The translation from `entry` on the module's own CPU state and RAM image
 * (its regions as `before`): its state at the end, with the native's RAM
 * pointer. */
static CPUState harness_translate(Harness* h, const CPUState* start, u32 entry) {
    copy_regions(h->reference_ram, h->before);
    CPUState* g = h->guest_cpu();
    *g = *start;
    g->ram = h->reference_ram;
    h->mod->on_state_loaded(g);
    s_unexpected_service = 0;
    if (!h->mod->dispatch(g, entry)) {
        fprintf(stderr, "the translation did not run at %08X\n", entry);
        exit(1);
    }
    CPUState reference = *g;
    reference.ram = start->ram;
    return reference;
}

static double now_ns(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1e9 / (double)freq.QuadPart;
}

#ifdef NATIVE4_HOOKED
/* --- The hooks, end to end (built with -DNATIVE4_HOOKED=1). ---------------
 *
 * The test then also links the eight chunks native_entries.py hooks for this
 * set, compiled as the module compiles them (their sources the player's own,
 * hooked on a copy, never distributed), with every native the chunks name,
 * guest_cpu.c and gather_pipe.c:
 *
 *   for each chunk in 0099 0101 0144 0148 0182 0187 0188 0191:
 *     clang -c -O2 -march=x86-64-v3 -ffp-contract=off -fno-slp-vectorize
 *       -mllvm -large-interval-freq-threshold=10
 *       -DMODULE_GAME_ID=\"GZLE01\" -DDOLRECOMP_CPU_HEADER=\"core/cpu.h\"
 *       -DBW_GUEST_MEM1=bw_guest_mem1 -DBW_GUEST_MEM1_SIZE=0x02000000u
 *       -DBLUEWAKE_EDGE_FILTER=1 -DBLUEWAKE_GATHER_PIPE_BATCH=1
 *       -Icmake/composite -I%HOOKED% -I...GXRuntime\include -I...StaticRecomp
 *       %HOOKED%\chunks_dol\chunk_NNNN_text1_XXXXXXXX.c -o chunk_NNNN.o
 *   clang -O2 -march=x86-64-v3 -ffp-contract=off -DNATIVE4_HOOKED=1
 *     -DBW_GUEST_MEM1=bw_guest_mem1 -DBW_GUEST_MEM1_SIZE=0x02000000u (the
 *     test's usual sources and includes) chunk_*.o cmake/composite/native_*.c
 *     cmake/composite/guest_cpu.c cmake/composite/gather_pipe.c
 *     ...\gxruntime.lib -o native_X_hooked_test.exe
 *
 * (%HOOKED%: composite-src's chunks_dol and generated.h, copied, then
 * native_entries.py run on the copy.) Every case the native runs is then
 * also run through the hooked chunks from the function's entry - once with
 * the natives on (this set's, the earlier sets' and the leaves' behind the
 * direct calls, as in play), once with them all off - a small chassis loop
 * going on in the next chunk where one leaves at a boundary; the CPU state
 * and the writable pages must match the module's translation byte for byte
 * both times. The chunks' MEM1 (guest_cpu.c) is read-only but the test's
 * pages, like the images. */
#include "gather_pipe.h"
#include "native_anim.h"
#include "native_bg.h"
#include "native_cc.h"
#include "native_fifo.h"
#include "native_game_math.h"
#include "native_j3d.h"
#include "native_kankyo.h"
#include "native_math.h"
#include "native_mtxcalc.h"
#include "native_search.h"
#include "native_skin.h"
#include "native_vec.h"

void func_8018D6E0(CPUState*);
void func_801956E0(CPUState*);
void func_802416E0(CPUState*);
void func_802516E0(CPUState*);
void func_802D96E0(CPUState*);
void func_802ED6E0(CPUState*);
void func_802F16E0(CPUState*);
void func_802FD6E0(CPUState*);
static const struct {
    unsigned index;
    u32 start;
    BwChunkFn fn;
} HOOKED_CHUNKS[] = {{99, 0x8018D6E0u, func_8018D6E0},  {101, 0x801956E0u, func_801956E0},
                     {144, 0x802416E0u, func_802416E0}, {148, 0x802516E0u, func_802516E0},
                     {182, 0x802D96E0u, func_802D96E0}, {187, 0x802ED6E0u, func_802ED6E0},
                     {188, 0x802F16E0u, func_802F16E0}, {191, 0x802FD6E0u, func_802FD6E0}};

static void missing_chunk(CPUState* cpu) {
    fprintf(stderr, "a call into a chunk the test does not link (pc %08X)\n", cpu->pc);
    exit(1);
}
static BwChunkFn s_hooked_table[256];
BwChunkFn* const bw_chunk_fns = s_hooked_table;

BwChunkFn bw_find_chunk(u32 address) {
    for (unsigned i = 0; i < sizeof HOOKED_CHUNKS / sizeof HOOKED_CHUNKS[0]; ++i)
        if (address - HOOKED_CHUNKS[i].start < 0x4000u)
            return HOOKED_CHUNKS[i].fn;
    return NULL;
}

/* The leaves behind the direct calls (module_export.c's form). */
static int s_hooked_leaves;
int bw_native_call(CPUState* cpu, u32 address) {
    return s_hooked_leaves && (bluewake_native_vec(cpu, address) || bluewake_native_math(cpu, address));
}

extern CPUState bw_guest_cpu;
extern u8 bw_guest_mem1[];
void bluewake_composite_set_gather_pipe(BwGatherPipeWrite write);
static void hooked_pipe(u64 value, u8 size) {
    (void)value;
    (void)size;
    fprintf(stderr, "a gather pipe write in the hooked chunks\n");
    exit(1);
}

static u32 s_hooked_entry[16];
static unsigned long long s_hooked_cases[16], s_hooked_declined[16];
static void hooked_summary(void) {
    for (unsigned i = 0; i < 16u && s_hooked_entry[i] != 0u; ++i)
        printf("%08X: %llu cases also identical through the hooked chunks, natives on and off (%llu of them "
               "declined here, for the next hook or the translation)\n",
               s_hooked_entry[i], s_hooked_cases[i], s_hooked_declined[i]);
    printf("(each native's count below takes in both its direct calls and its calls from the hooks)\n");
    fflush(stdout);
    bluewake_native_kankyo_report();
    bluewake_native_anim_report();
    bluewake_native_cc_report();
    bluewake_native_game_math_report(); /* the second set's hooks behind this set's */
}

static void hooked_natives(int on) {
    s_hooked_leaves = on;
    bluewake_native_kankyo_enabled = on;
    bluewake_native_anim_enabled = on;
    bluewake_native_cc_enabled = on;
    bluewake_native_fifo_enabled = on;
    bluewake_native_bg_enabled = on;
    bluewake_native_vec_sr_enabled = on;
    bluewake_native_mtxcalc_enabled = on;
    bluewake_native_game_math_enabled = on;
    bluewake_native_j3d_enabled = on;
    bluewake_native_search_enabled = on;
    bluewake_native_skin_enabled = on;
}

static void hooked_setup(void) {
    static bool ready;
    if (ready)
        return;
    ready = true;
    for (unsigned i = 0; i < 256u; ++i)
        s_hooked_table[i] = missing_chunk;
    for (unsigned i = 0; i < sizeof HOOKED_CHUNKS / sizeof HOOKED_CHUNKS[0]; ++i)
        s_hooked_table[HOOKED_CHUNKS[i].index] = HOOKED_CHUNKS[i].fn;
    bluewake_composite_set_gather_pipe(hooked_pipe);
    atexit(hooked_summary);
    if (protect_image(bw_guest_mem1) == NULL) {
        fprintf(stderr, "cannot protect the chunks' MEM1\n");
        exit(1);
    }
}

/* ns per call through the hooked chunks from `entry` on `base` (its pages
 * as in `ram`): this set's natives off, then on, with the earlier sets' and
 * the leaves' on in both, as in play. r1, r3 to r7, f1 to f7 and LR are set
 * again before each call, as the tests' own benchmarks set theirs; the
 * chunk's entry and return dispatch are in the time, the chassis's own
 * dispatcher is not (a call from translated code reaches the chunk
 * directly). */
static void hooked_bench(const u8* ram, u32 entry, const CPUState* base, unsigned calls, const char* name,
                         const char* shape) {
    hooked_setup();
    copy_regions(bw_guest_mem1, ram);
    double best[2] = {1e30, 1e30};
    for (unsigned round = 0; round < 6u; ++round) {
        const int on = (int)(round & 1u);
        hooked_natives(1);
        bluewake_native_kankyo_enabled = bluewake_native_anim_enabled = bluewake_native_cc_enabled = on;
        bw_guest_cpu = *base;
        bw_guest_cpu.ram = bw_guest_mem1;
        bw_guest_cpu.downcount = 0;
        bw_guest_cpu.cycle_budget = (s64)1 << 40;
        bw_guest_cpu.cycle_deadline_budget = 0;
        ppc_fpscr_updated(&bw_guest_cpu);
        const double t0 = now_ns();
        for (unsigned i = 0; i < calls; ++i) {
            CPUState* c = &bw_guest_cpu;
            c->gpr[1] = base->gpr[1];
            memcpy(&c->gpr[3], &base->gpr[3], 5u * sizeof(u32));
            memcpy(&c->fpr[1], &base->fpr[1], 7u * sizeof(f64));
            c->lr = RETURN_ADDRESS;
            c->pc = entry;
            do
                s_hooked_table[(c->pc - 0x800016E0u) >> 14](c);
            while ((c->pc & ~3u) != RETURN_ADDRESS);
        }
        const double ns = (now_ns() - t0) / calls;
        if (ns < best[on])
            best[on] = ns;
    }
    hooked_natives(0);
    printf("%08X %s%s%s: through the hooked chunks %.1f ns/call with this set's natives off, %.1f on (%.2fx)\n",
           entry, name, shape != NULL ? ", " : "", shape != NULL ? shape : "", best[0], best[1], best[0] / best[1]);
}

/* Entries where an earlier set's hook follows this set's (the second set's
 * key-s and transform-simple): the cases this set's native declines, but not
 * where it must, run through the hooked chunks too, so that hook (or the
 * translation) takes them as in play. */
static bool hooked_followed(u32 entry) {
    return entry == BLUEWAKE_ANIM_KEY_S || entry == BLUEWAKE_ANIM_TRANSFORM;
}

/* The hooked chunks from `entry` on `start` (the regions as `before`), with
 * the natives on and then off, against `reference`. */
static void hooked_case(const Harness* h, u32 entry, const char* name, unsigned index, const CPUState* start,
                        const CPUState* reference, bool flush, bool declined) {
    hooked_setup();
    for (int on = 1; on >= 0; --on) {
        copy_regions(bw_guest_mem1, h->before);
        hooked_natives(on);
        bw_guest_cpu = *start;
        bw_guest_cpu.ram = bw_guest_mem1;
        host_fp_mode(flush);
        ppc_fpscr_updated(&bw_guest_cpu);
        bw_guest_cpu.pc = entry;
        unsigned guard = 0;
        while ((bw_guest_cpu.pc & ~3u) != RETURN_ADDRESS && guard++ < 4096u) {
            BwChunkFn fn = bw_find_chunk(bw_guest_cpu.pc);
            if (fn == NULL) {
                fprintf(stderr, "case %u (%s): no linked chunk at %08X\n", index, name, bw_guest_cpu.pc);
                exit(1);
            }
            fn(&bw_guest_cpu);
        }
        host_fp_mode(false);
        CPUState got = bw_guest_cpu;
        got.ram = start->ram;
        if (memcmp(&got, reference, sizeof got) != 0 || !same_regions(bw_guest_mem1, h->reference_ram)) {
            fprintf(stderr, "case %u (%s, seed %08X): the hooked chunks, natives %s, differ\n", index, name, seed,
                    on ? "on" : "off");
            report_cpu(&got, reference);
            report_ram(bw_guest_mem1, h->reference_ram);
            exit(1);
        }
    }
    hooked_natives(0);
    unsigned slot = 0;
    while (slot < 15u && s_hooked_entry[slot] != 0u && s_hooked_entry[slot] != entry)
        slot++;
    s_hooked_entry[slot] = entry;
    s_hooked_cases[slot]++;
    s_hooked_declined[slot] += declined;
}
#endif

/* One case: the native on `start` (in the native image, regions saved in
 * `before`), then - where it ran - the translation, and every byte compared.
 * Returns 1 identical, 0 declined unchanged; a mismatch exits. */
typedef int (*NativeFn)(CPUState* cpu, u32 address);
static int harness_case(Harness* h, NativeFn native_fn, u32 entry, const char* name, unsigned index,
                        const CPUState* start, bool must_decline, bool with_journal, bool with_aliases, bool flush) {
    copy_regions(h->before, h->native_ram);
    CPUState native = *start;
    const CPUState untouched = native;
    host_fp_mode(flush);
    if (with_journal)
        g_mem_write_journal = journal;
    if (with_aliases)
        g_ppc_guest_aliases_overlap_mem1 = true;
    const int accepted = native_fn(&native, entry);
    g_mem_write_journal = NULL;
    g_ppc_guest_aliases_overlap_mem1 = false;
    if (!accepted) {
        host_fp_mode(false);
        if (memcmp(&native, &untouched, sizeof native) != 0 || !same_regions(h->before, h->native_ram)) {
            fprintf(stderr, "case %u (%s): declined but changed state\n", index, name);
            report_cpu(&native, &untouched);
            exit(1);
        }
#ifdef NATIVE4_HOOKED
        if (!must_decline && !with_journal && !with_aliases && hooked_followed(entry)) {
            host_fp_mode(flush);
            const CPUState reference = harness_translate(h, start, entry);
            host_fp_mode(false);
            if (s_unexpected_service == 0u && (reference.pc & ~3u) == RETURN_ADDRESS)
                hooked_case(h, entry, name, index, start, &reference, flush, true);
        }
#endif
        return 0;
    }
    if (must_decline) {
        fprintf(stderr, "case %u (%s): ran where it must decline\n", index, name);
        exit(1);
    }
    const CPUState reference = harness_translate(h, start, entry);
    host_fp_mode(false);
    if (s_unexpected_service != 0u || (reference.pc & ~3u) != RETURN_ADDRESS ||
        memcmp(&native, &reference, sizeof native) != 0 || !same_regions(h->native_ram, h->reference_ram)) {
        fprintf(stderr, "case %u (%s, seed %08X): mismatch (service asked %u times, last at %08X; pc %08X)\n", index,
                name, seed, s_unexpected_service, s_unexpected_at, reference.pc);
        report_cpu(&native, &reference);
        report_ram(h->native_ram, h->reference_ram);
        exit(1);
    }
#ifdef NATIVE4_HOOKED
    hooked_case(h, entry, name, index, start, &reference, flush, false);
#endif
    return 1;
}

#endif
