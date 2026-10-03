/* cmake/composite/native_mtxcalc.c against the translations it stands in for.
 *
 * Build and run from the worktree root (x64; the Visual Studio environment):
 *
 *   clang -O2 -march=x86-64-v3 -ffp-contract=off -Icmake/composite
 *     -IE:\Github\Wind-Waker-Recomp\ref\recompcore\GXRuntime\include
 *     -IE:\Github\Wind-Waker-Recomp\ref\recompcore\Source\Core\Core\PowerPC\StaticRecomp
 *     tests/native_mtxcalc_test.c cmake/composite/native_mtxcalc.c cmake/composite/direct_calls.c
 *     E:\Github\Wind-Waker-Recomp\build\windows\app\gxruntime_build\gxruntime.lib -o native_mtxcalc_test.exe
 *   native_mtxcalc_test MODULE.dll [CASES_PER_FUNCTION=60000] [BENCH_CALLS=500000] [--module-natives]
 *
 * MODULE.dll is a Windows game module (gGZLE01_recomp.dll) without these
 * natives; the test reads it and writes nothing but its own memory. The
 * module runs as in play: direct calls and its edge filter on, the host's
 * flags quiet, an edge service that ends the run at the return address (and
 * fails the test if it is asked anything anywhere else); its own natives
 * (PSMTXConcat, PSMTXCopy, J3DGetTranslateRotateMtx) off, so every callee is
 * its translation - or, with --module-natives, on, as in play (they are
 * certified to give the translations' results).
 *
 * For each function: a random model (its data, joint table and nodes with
 * their scale compensation flags, scale flags, animation matrices), transform
 * (scales of exactly one or not, angles, translations), J3DSys's current
 * matrix and scales, sine and cosine tables and their shift, registers (both
 * paired-single halves), FPSCR, reservations and cycle state; every float
 * sometimes zero, denormal, huge, infinite or NaN (so the interpreter's paths
 * run too: a parent scale of zero divides by zero); stores that land on each
 * other (the animation matrix on the current matrix, the scale flag inside
 * it); and the declining cases: addresses that leave RAM at each level,
 * stores on words addresses are loaded from, FP off, quantised pairs, an
 * exception pending, a write journal, aliases over MEM1, the budget or the
 * deadline inside the function, and the host not quiet, the edge filter off
 * or a boundary watched. Every byte of the CPU state (the cycle suffix
 * included) and of RAM must match - or, where the native declines, nothing
 * may have changed. RAM outside the test area and J3DSys's page is
 * read-only in both images.
 *
 * Then a microbenchmark: the translation through the module's dispatcher
 * (its calls direct, as in play) against the native, ns per call, on the
 * common paths. */
#include "native_mtxcalc.h"
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
#define STATICS 0x803ED000u /* j3dSys, J3DSys::mCurrentMtx, mCurrentS, mParentS */
#define STATICS_BYTES 0x1000u
#define UNIT_PAGE 0x803F6000u /* PSMTXConcat's (0, 1) pair at 0x803F66F0 */
#define UNIT_BYTES 0x1000u
#define RETURN_ADDRESS 0xFFFFFFFCu

#define INFO_AT (AREA + 0x0000u)
#define MODEL (AREA + 0x0200u)
#define MODEL_DATA (AREA + 0x0400u)
#define NODE_TABLE (AREA + 0x0500u)
#define NODES (AREA + 0x0600u)
#define FLAGS (AREA + 0x1000u)
#define ANM (AREA + 0x1100u)
#define SDA2 (AREA + 0x2000u)
#define TABLES (AREA + 0x2100u)
#define SINES (AREA + 0x4000u)
#define COSINES (AREA + 0x8000u)
#define STACK (AREA + 0xE000u)

#define J3D_SYS_MODEL 0x803EDA90u
#define J3D_CURRENT_MTX 0x803EDB80u
#define J3D_CURRENT_S 0x803EDBB0u
#define J3D_PARENT_S 0x803EDBBCu
#define PSMTX_UNIT 0x803F66F0u

static u32 seed = 0x1B873593u;
static u32 next(void) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

/* The floats: ordinary in most cases; in some, also denormals and values so
 * small or large that a product leaves the normal range (s_edges); in some,
 * infinities and NaNs too (s_specials). */
static bool s_specials, s_edges;
static u32 float_bits(void) {
    const u32 sign = next() & 0x80000000u;
    switch (next() % (s_specials ? 20u : s_edges ? 24u : 64u)) {
    case 0: return sign;
    case 1: return s_edges || s_specials ? sign | (next() & 0x007FFFFFu) : 0x3F800000u;
    case 2: return s_specials ? sign | 0x7F800000u : 0x3F800000u;
    case 3: return s_specials ? sign | 0x7FC00000u | (next() & 0x003FFFFFu) : 0xBF800000u;
    case 4: return s_specials ? sign | 0x7F800000u | (1u + next() % 0x003FFFFFu) : 0x3F000000u;
    case 5: return sign | ((s_specials || s_edges ? 200u + next() % 54u : 140u + next() % 10u) << 23) |
                   (next() & 0x007FFFFFu);
    case 6: return sign | ((s_specials || s_edges ? 1u + next() % 40u : 110u + next() % 10u) << 23) |
                   (next() & 0x007FFFFFu);
    case 7: return 0x3F800000u;
    default: return sign | ((115u + next() % 20u) << 23) | (next() & 0x007FFFFFu);
    }
}

static f64 register_value(void) {
    return next() % 4u == 0u ? f64_value(((u64)next() << 32) | next()) : f64_value(convert_to_double(float_bits()));
}

static void put(u8* ram, u32 address, u32 value) { write_be32(ram + (address - GC_RAM_BASE), value); }
static u32 peek(const u8* ram, u32 address) { return read_be32(ram + (address - GC_RAM_BASE)); }

static void journal(u32 offset, u32 size, void* user) {
    (void)offset;
    (void)size;
    (void)user;
    abort();
}

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

static u8* protect_image(u8* p) {
    DWORD old;
    if (p == NULL || !VirtualProtect(p, GC_MAIN_RAM_SIZE, PAGE_READONLY, &old) ||
        !VirtualProtect(p + (AREA - GC_RAM_BASE), AREA_BYTES, PAGE_READWRITE, &old) ||
        !VirtualProtect(p + (STATICS - GC_RAM_BASE), STATICS_BYTES, PAGE_READWRITE, &old) ||
        !VirtualProtect(p + (UNIT_PAGE - GC_RAM_BASE), UNIT_BYTES, PAGE_READWRITE, &old))
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
    watch(0x80006000u);
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

static const u32 FUNCTIONS[] = {BLUEWAKE_MTXCALC_BASIC, BLUEWAKE_MTXCALC_SOFTIMAGE, BLUEWAKE_MTXCALC_MAYA};
static const char* const NAMES[] = {"J3DMtxCalcBasic", "J3DMtxCalcSoftimage", "J3DMtxCalcMaya"};

typedef struct Case {
    CPUState cpu;
    bool journal, aliases, must_decline, trouble;
    unsigned trouble_kind;
} Case;

static void fill(u8* ram, u32 at, u32 bytes) {
    for (u32 i = 0; i < bytes; i += 4u)
        put(ram, at + i, float_bits());
}

static u32 trig_offset(u32 angle, u32 shift) {
    const u32 sh = shift & 0x3Fu;
    return ((sh == 0u ? angle : sh > 31u ? 0u : angle >> sh) << 2) & 0xFFFFFFFCu;
}

static Case build(u8* ram, unsigned which, unsigned scenario) {
    Case k;
    memset(&k, 0, sizeof k);
    s_specials = scenario % 16u == 3u;
    s_edges = scenario % 16u == 11u;
    CPUState* c = &k.cpu;
    c->ram = ram;
    c->ram_size = GC_MAIN_RAM_SIZE;
    for (unsigned r = 0; r < 32; ++r) {
        c->gpr[r] = next();
        c->fpr[r] = register_value();
        c->ps1[r] = register_value();
    }
    /* The data. */
    fill(ram, AREA, 0x4000u);
    fill(ram, STACK - 0x400u, 0x800u);
    fill(ram, J3D_CURRENT_MTX, 48u + 24u);
    const u32 info = INFO_AT + 4u * (next() % 32u);
    const bool one = scenario % 3u != 0u;
    for (unsigned i = 0; i < 3u; ++i) {
        put(ram, info + 4u * i, one ? 0x3F800000u : float_bits());
        put(ram, J3D_CURRENT_S + 4u * i, one || next() % 2u ? 0x3F800000u : float_bits());
    }
    if (one && scenario % 7u == 1u)
        put(ram, info + 4u * (next() % 3u), float_bits()); /* one axis not one */
    for (unsigned i = 0; i < 3u; ++i)
        write_be16(ram + (info + 12u + 2u * i - GC_RAM_BASE), (u16)next());
    put(ram, J3D_SYS_MODEL, MODEL);
    put(ram, MODEL + 4u, MODEL_DATA);
    put(ram, MODEL + 132u, FLAGS);
    put(ram, MODEL + 140u, ANM);
    put(ram, MODEL_DATA + 44u, NODE_TABLE);
    const u32 joint = next() % 64u;
    for (u32 j = 0; j < 64u; ++j) {
        put(ram, NODE_TABLE + 4u * j, NODES + 32u * j);
        ram[NODES + 32u * j + 27u - GC_RAM_BASE] = (u8)(next() % 3u == 0u ? next() : next() % 2u);
    }
    const u32 shift = scenario % 9u == 4u ? next() & 63u : 4u;
    put(ram, TABLES, shift);
    put(ram, TABLES + 4u, SINES);
    put(ram, TABLES + 8u, COSINES);
    put(ram, SDA2, scenario % 23u == 5u ? float_bits() : 0x3F800000u);
    put(ram, PSMTX_UNIT, scenario % 29u == 6u ? float_bits() : 0u);
    put(ram, PSMTX_UNIT + 4u, scenario % 29u == 6u ? float_bits() : 0x3F800000u);
    if (scenario % 5u == 2u)
        put(ram, J3D_PARENT_S + 4u * (next() % 3u), next() % 2u ? 0u : 0x80000000u); /* divide by zero */
    /* The registers. */
    c->gpr[1] = STACK - 8u * (next() % 16u);
    c->gpr[2] = SDA2 + 13072u;
    c->gpr[13] = TABLES + 26460u;
    c->gpr[4] = (next() & 0xFFFF0000u) | joint;
    c->gpr[5] = info;
    c->lr = RETURN_ADDRESS | (next() & 3u);
    c->pc = FUNCTIONS[which];
    c->ctr = next();
    c->cr = next();
    c->xer = next();
    c->msr = PPC_MSR_FP | (next() & ~(PPC_MSR_FP | PPC_MSR_EE));
    c->hid2 = PPC_HID2_LSQE | (next() & 0x0FFFFFFFu);
    for (unsigned g = 1; g < 8; ++g)
        c->gqr[g] = next();
    c->gqr[0] = scenario % 3u == 0u ? 0x3F003F00u & next() : 0u;
    c->fpscr = (next() & ~0x60000003u) | (scenario % 16u == 5u ? 1u + next() % 3u : 0u); /* RN: mostly nearest */
    c->reserve_valid = (scenario & 1u) != 0u;
    c->reserve_addr = scenario % 4u == 1u ? J3D_CURRENT_MTX + 16u : c->gpr[1] - 64u;
    c->cycle_observation_suffix = next();
    c->cycle_budget = 16384;
    c->downcount = -(s64)(next() % 64u);
    c->cycle_deadline_budget = scenario % 4u == 0u ? 0 : 100000;
    /* Stores landing on each other: allowed, and exact. */
    switch (scenario % 52u) {
    case 3: put(ram, MODEL + 140u, J3D_CURRENT_MTX - joint * 48u); break;      /* the copy in place */
    case 4: put(ram, MODEL + 132u, J3D_CURRENT_MTX + 5u - joint); break;       /* the flag in the matrix */
    case 5: put(ram, MODEL + 140u, c->gpr[1] - 96u - joint * 48u); break;      /* the copy onto the frame */
    case 6: put(ram, MODEL + 132u, J3D_CURRENT_S - joint); break;              /* the flag on mCurrentS */
    default: break;
    }
    const u32 angles[3] = {read_be16(ram + (info + 12u - GC_RAM_BASE)), read_be16(ram + (info + 14u - GC_RAM_BASE)),
                           read_be16(ram + (info + 16u - GC_RAM_BASE))};
    for (unsigned i = 0; i < 3u; ++i) {
        const u32 offset = trig_offset(angles[i], shift);
        if (offset < 0x4000u) {
            put(ram, SINES + offset, float_bits());
            put(ram, COSINES + offset, float_bits());
        }
    }
    /* The declining cases. */
    switch (scenario % 97u) {
    case 1: c->msr &= ~PPC_MSR_FP; k.must_decline = true; break;
    case 2: c->hid2 &= ~PPC_HID2_LSQE; k.must_decline = true; break;
    case 3: c->gqr[0] = 0x00050000u; k.must_decline = true; break;
    case 4: c->exception = 1u; k.must_decline = true; break;
    case 5: k.journal = k.must_decline = true; break;
    case 6: k.aliases = k.must_decline = true; break;
    case 7: put(ram, J3D_SYS_MODEL, 0xCC000000u); k.must_decline = true; break;
    case 8: put(ram, MODEL + 140u, 0xC0100000u); k.must_decline = true; break;
    case 9: put(ram, TABLES + 4u, 0xCC000000u); k.must_decline = true; break;
    case 10: c->gpr[5] = 0xCC000000u; k.must_decline = true; break;
    case 11: c->gpr[1] = 0x80000040u; k.must_decline = true; break;
    case 12: put(ram, MODEL + 140u, info - joint * 48u); k.must_decline = true; break;    /* the copy on the transform */
    case 13: put(ram, MODEL + 132u, MODEL + 140u - joint); k.must_decline = true; break; /* the flag on an address */
    case 14: c->gpr[1] = TABLES + 64u; k.must_decline = true; break;                     /* the frame on the tables */
    case 15: c->downcount = -c->cycle_budget + 1 + (s64)(next() % 200u); k.must_decline = true; break;
    case 16: c->cycle_deadline_budget = 1 + (s64)(next() % 200u); k.must_decline = true; break;
    case 17: c->cycle_budget = 0; k.must_decline = true; break;
    case 18:
        if (which == 2) {
            put(ram, MODEL + 4u, 0xCC000000u);
            k.must_decline = true;
        }
        break;
    case 19:
        if (which == 2) {
            put(ram, NODE_TABLE + 4u * joint, 0xC0000000u);
            k.must_decline = true;
        }
        break;
    case 20:
        k.trouble = k.must_decline = true;
        k.trouble_kind = (scenario / 97u) % 5u;
        break;
    case 21: c->downcount = -c->cycle_budget + 330; break; /* just enough */
    case 22: c->cycle_deadline_budget = 330 - c->downcount; break;
    default: break;
    }
    return k;
}

static void make_trouble(unsigned which, unsigned kind, CPUState* c) {
    switch (kind) {
    case 0: s_sources_dirty = true; break;
    case 1: s_decrementer_pending = true; c->msr |= PPC_MSR_EE; break;
    case 2: bw_edge_filter_enabled = false; break;
    case 3: watch(which == 1 ? 0x802DA724u : 0x802DA64Cu); break; /* the rotation's entry */
    default: watch(which == 0 ? 0x802F5244u : which == 1 ? 0x802F532Cu : 0x802F56E0u); break;
    }
}

static void report_cpu(const CPUState* got, const CPUState* want) {
    for (unsigned b = 0; b < sizeof *got; ++b)
        if (((const u8*)got)[b] != ((const u8*)want)[b])
            fprintf(stderr, "  CPU byte %u: got %02X want %02X\n", b, ((const u8*)got)[b], ((const u8*)want)[b]);
}

static int report_ram(const u8* got, const u8* want, u32 at, u32 bytes) {
    int differ = 0;
    for (u32 i = 0; i < bytes; ++i)
        if (got[at + i - GC_RAM_BASE] != want[at + i - GC_RAM_BASE]) {
            if (differ++ < 32)
                fprintf(stderr, "  RAM %08X: got %02X want %02X\n", at + i, got[at + i - GC_RAM_BASE],
                        want[at + i - GC_RAM_BASE]);
        }
    return differ;
}

static void copy_regions(u8* to, const u8* from) {
    memcpy(to + (AREA - GC_RAM_BASE), from + (AREA - GC_RAM_BASE), AREA_BYTES);
    memcpy(to + (STATICS - GC_RAM_BASE), from + (STATICS - GC_RAM_BASE), STATICS_BYTES);
    memcpy(to + (UNIT_PAGE - GC_RAM_BASE), from + (UNIT_PAGE - GC_RAM_BASE), UNIT_BYTES);
}

static bool same_regions(const u8* a, const u8* b) {
    return memcmp(a + (AREA - GC_RAM_BASE), b + (AREA - GC_RAM_BASE), AREA_BYTES) == 0 &&
           memcmp(a + (STATICS - GC_RAM_BASE), b + (STATICS - GC_RAM_BASE), STATICS_BYTES) == 0 &&
           memcmp(a + (UNIT_PAGE - GC_RAM_BASE), b + (UNIT_PAGE - GC_RAM_BASE), UNIT_BYTES) == 0;
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
        fprintf(stderr, "usage: native_mtxcalc_test MODULE.dll [CASES_PER_FUNCTION] [BENCH_CALLS] [--module-natives]\n");
        return 2;
    }
    const bool module_natives = argc > 4 && strcmp(argv[4], "--module-natives") == 0;
    _putenv_s("BLUEWAKE_NATIVE_MATH", module_natives ? "1" : "0");
    const unsigned cases = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 60000u;
    const unsigned bench_calls = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 500000u;
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
    if (module_natives)
        printf("the module's own natives on (PSMTXConcat, PSMTXCopy, J3DGetTranslateRotateMtx)\n");

    for (unsigned which = 0; which < 3u; ++which) {
        const u32 entry = FUNCTIONS[which];
        unsigned ran = 0, declined = 0, scale_one = 0, compensated = 0, nonfinite = 0;
        s64 most = 0;
        for (unsigned i = 0; i < cases; ++i) {
            Case k = build(native_ram, which, i);
            ppc_fpscr_updated(&k.cpu);
            copy_regions(before, native_ram);
            reset_host();
            if (k.trouble)
                make_trouble(which, k.trouble_kind, &k.cpu);
            CPUState native = k.cpu;
            const CPUState untouched = native;
            if (k.journal)
                g_mem_write_journal = journal;
            if (k.aliases)
                g_ppc_guest_aliases_overlap_mem1 = true;
            const int accepted = bluewake_native_mtxcalc(&native, entry);
            g_mem_write_journal = NULL;
            g_ppc_guest_aliases_overlap_mem1 = false;
            if (!accepted) {
                declined++;
                if (memcmp(&native, &untouched, sizeof native) != 0 || !same_regions(before, native_ram)) {
                    fprintf(stderr, "case %u (%s): declined but changed state\n", i, NAMES[which]);
                    report_cpu(&native, &untouched);
                    return 1;
                }
                continue;
            }
            if (k.must_decline) {
                fprintf(stderr, "case %u (%s): ran where it must decline\n", i, NAMES[which]);
                return 1;
            }
            ran++;
            scale_one += peek(native_ram, FLAGS + (k.cpu.gpr[4] & 0xFFFFu)) >> 24 == 1u;
            nonfinite += !(native.fpr[0] - native.fpr[0] == 0.0);

            copy_regions(reference_ram, before);
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
            if (k.cpu.downcount - reference.downcount > most)
                most = k.cpu.downcount - reference.downcount;
            if (which == 2 && reference.downcount < k.cpu.downcount - 250)
                compensated++;
            if (s_unexpected_service != 0u || (reference.pc & ~3u) != RETURN_ADDRESS ||
                memcmp(&native, &reference, sizeof native) != 0 || !same_regions(native_ram, reference_ram)) {
                fprintf(stderr, "case %u (%s, seed %08X): mismatch (service asked %u times, last at %08X; pc %08X)\n",
                        i, NAMES[which], seed, s_unexpected_service, s_unexpected_at, reference.pc);
                report_cpu(&native, &reference);
                report_ram(native_ram, reference_ram, AREA, AREA_BYTES);
                report_ram(native_ram, reference_ram, STATICS, STATICS_BYTES);
                return 1;
            }
        }
        printf("%08X %s: %u cases, %u identical (%u with the scale one, %u with a non-finite f0; at most %lld "
               "cycles), %u declined unchanged, 0 mismatches\n",
               entry, NAMES[which], cases, ran, scale_one, nonfinite, (long long)most, declined);
        if (which == 2)
            printf("%08X %s: %u identical cases through the scale compensation\n", entry, NAMES[which], compensated);
        fflush(stdout);
        if (ran < 30000u && cases >= 60000u)
            return 1; /* at least 30,000 compared cases per function */
    }

    if (bench_calls != 0u) {
        reset_host();
        CPUState* g = guest_cpu();
        for (unsigned which = 0; which < 3u; ++which) {
            const u32 entry = FUNCTIONS[which];
            for (unsigned variant = 0; variant < 2u; ++variant) {
                /* Ordinary values: the scale one (variant 0) or not, the
                 * Maya form compensating its parent's scale (variant 1). */
                s_specials = s_edges = false;
                Case k = build(native_ram, which, 0u);
                CPUState base = k.cpu;
                const u32 info = base.gpr[5];
                for (unsigned i = 0; i < 3u; ++i)
                    put(native_ram, info + 20u + 4u * i, 0x41200000u + 0x100000u * i);
                for (unsigned i = 0; i < 3u; ++i) {
                    put(native_ram, info + 4u * i, variant == 0 ? 0x3F800000u : 0x3FC00000u);
                    put(native_ram, J3D_CURRENT_S + 4u * i, 0x3F800000u);
                    put(native_ram, J3D_PARENT_S + 4u * i, 0x3F400000u);
                }
                put(native_ram, J3D_SYS_MODEL, MODEL);
                put(native_ram, MODEL + 132u, FLAGS);
                put(native_ram, MODEL + 140u, ANM);
                put(native_ram, SDA2, 0x3F800000u);
                put(native_ram, PSMTX_UNIT, 0u);
                put(native_ram, PSMTX_UNIT + 4u, 0x3F800000u);
                put(native_ram, TABLES, 4u);
                for (u32 j = 0; j < 4096u; ++j) {
                    const f32 s = (f32)(j % 7) * 0.125f - 0.375f, cs = 0.5f;
                    u32 bits;
                    memcpy(&bits, &s, 4);
                    put(native_ram, SINES + 4u * j, bits);
                    memcpy(&bits, &cs, 4);
                    put(native_ram, COSINES + 4u * j, bits);
                }
                for (u32 j = 0; j < 12u; ++j)
                    put(native_ram, J3D_CURRENT_MTX + 4u * j, j % 5u == 0u ? 0x3F800000u : 0x3E000000u);
                for (u32 j = 0; j < 64u; ++j)
                    native_ram[NODES + 32u * j + 27u - GC_RAM_BASE] = (u8)variant;
                base.exception = 0;
                base.fpscr = 0;
                base.msr = PPC_MSR_FP;
                base.hid2 = PPC_HID2_LSQE;
                base.gqr[0] = 0;
                base.downcount = 0;
                base.cycle_deadline_budget = 0;
                base.cycle_budget = (s64)bench_calls * 400 + 1000;
                ppc_fpscr_updated(&base);
                copy_regions(reference_ram, native_ram);
                /* J3DSys's matrix and scales as each call starts (both loops
                 * put them back: repeated, the products would grow without end). */
                u8 statics[48 + 24];
                memcpy(statics, native_ram + (J3D_CURRENT_MTX - GC_RAM_BASE), sizeof statics);
                double best_t = 1e30, best_n = 1e30;
                for (unsigned round = 0; round < 5; ++round) {
                    *g = base;
                    g->ram = reference_ram;
                    mod->on_state_loaded(g);
                    const u32 sp = base.gpr[1], r4 = base.gpr[4];
                    double t0 = now_ns();
                    for (unsigned i = 0; i < bench_calls; ++i) {
                        g->gpr[1] = sp;
                        g->gpr[4] = r4;
                        g->gpr[5] = info;
                        g->lr = RETURN_ADDRESS;
                        g->pc = entry;
                        memcpy(reference_ram + (J3D_CURRENT_MTX - GC_RAM_BASE), statics, sizeof statics);
                        mod->dispatch(g, entry);
                    }
                    const double t = (now_ns() - t0) / bench_calls;
                    CPUState native = base;
                    t0 = now_ns();
                    for (unsigned i = 0; i < bench_calls; ++i) {
                        native.gpr[1] = sp;
                        native.gpr[4] = r4;
                        native.gpr[5] = info;
                        native.lr = RETURN_ADDRESS;
                        memcpy(native_ram + (J3D_CURRENT_MTX - GC_RAM_BASE), statics, sizeof statics);
                        if (!bluewake_native_mtxcalc(&native, entry)) {
                            fprintf(stderr, "the benchmark's native declined\n");
                            return 1;
                        }
                    }
                    const double n = (now_ns() - t0) / bench_calls;
                    if (t < best_t) best_t = t;
                    if (n < best_n) best_n = n;
                }
                printf("%08X %s (%s): translation %.1f ns/call through the dispatcher, native %.1f ns/call\n",
                       entry, NAMES[which], variant == 0 ? "scale one" : which == 2 ? "scaled, compensated" : "scaled",
                       best_t, best_n);
            }
        }
    }
    return 0;
}
