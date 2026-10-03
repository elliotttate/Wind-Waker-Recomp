/* The hooks scripts/windows/native_entries.py inserts, end to end: hooked
 * chunks, compiled as the module compiles them, against the module's
 * unhooked translation.
 *
 * The chunks are the player's own translated sources, prepared by the
 * Windows source steps and then by native_entries.py (on a copy, never the
 * builder's tree); none is distributed. Build, from the worktree root, with
 * HOOKED = a copy of composite-src's chunks_dol after native_entries.py and
 * COMPOSITE = that composite-src (for generated.h):
 *
 *   for each chunk in 0041 0042 0145 0181 0182 0188 0189 0194 0195:
 *     clang -c -O2 -march=x86-64-v3 -ffp-contract=off -fno-slp-vectorize
 *       -DMODULE_GAME_ID="GZLE01" -DDOLRECOMP_CPU_HEADER="core/cpu.h"
 *       -DBW_GUEST_MEM1=bw_guest_mem1 -DBW_GUEST_MEM1_SIZE=0x02000000u
 *       -DBLUEWAKE_EDGE_FILTER=1 -DBLUEWAKE_GATHER_PIPE_BATCH=1
 *       -Icmake/composite -I%COMPOSITE% -I...GXRuntime\include -I...StaticRecomp
 *       %HOOKED%\chunk_NNNN_text1_XXXXXXXX.c -o chunk_NNNN.o
 *   clang -O2 -march=x86-64-v3 -ffp-contract=off -DBW_GUEST_MEM1=bw_guest_mem1
 *     -DBW_GUEST_MEM1_SIZE=0x02000000u -Icmake/composite -I...GXRuntime\include
 *     -I...StaticRecomp tests/native_entries_test.c chunk_*.o
 *     cmake/composite/native_fifo.c cmake/composite/native_bg.c cmake/composite/native_vec.c
 *     cmake/composite/native_mtxcalc.c cmake/composite/native_game_math.c
 *     cmake/composite/native_j3d.c cmake/composite/direct_calls.c
 *     cmake/composite/gather_pipe.c cmake/composite/guest_cpu.c
 *     cmake/composite/native_math.c cmake/composite/native_work_pool.c
 *     ...\gxruntime.lib -o native_entries_test.exe
 *   native_entries_test MODULE.dll [CASES_PER_FUNCTION=4000]
 *
 * For each of the nine functions: random ordinary inputs (and some that make
 * the natives decline), run (1) through the module's translation, run as in
 * play (direct calls, its edge filter, the host quiet), (2) through the
 * hooked chunks with the natives on, (3) through the hooked chunks with them
 * off; a small chassis loop runs the chunks (a chunk left at a boundary goes
 * on in the next). Every CPU byte, the RAM the functions touch and the
 * gather pipe's bytes must match across all three, and the full images
 * every 256 cases.
 *
 * Then ns per call through the hooked chunks with the natives on and off,
 * the module's other natives on as in play (PSMTXConcat, PSMTXCopy, the
 * vector leaves through the direct calls; J3DGetTranslateRotateMtx at its
 * hook): the chunk's entry and return dispatch included, the module's
 * dispatcher not - the closest this gets to a call in play. */
#include "native_fifo.h"
#include "native_bg.h"
#include "native_vec.h"
#include "native_mtxcalc.h"
#include "native_game_math.h"
#include "native_j3d.h"
#include "native_math.h"
#include "direct_calls.h"
#include "gather_pipe.h"
#include "StaticRecompABI.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* The hooked chunks (their translated functions), by table index. */
void func_800A56E0(CPUState*);
void func_800A96E0(CPUState*);
void func_802456E0(CPUState*);
void func_802D56E0(CPUState*);
void func_802D96E0(CPUState*);
void func_802F16E0(CPUState*);
void func_802F56E0(CPUState*);
void func_803096E0(CPUState*);
void func_8030D6E0(CPUState*);
static const struct {
    unsigned index;
    u32 start;
    BwChunkFn fn;
} CHUNKS[] = {{41, 0x800A56E0u, func_800A56E0}, {42, 0x800A96E0u, func_800A96E0}, {145, 0x802456E0u, func_802456E0},
              {181, 0x802D56E0u, func_802D56E0}, {182, 0x802D96E0u, func_802D96E0}, {188, 0x802F16E0u, func_802F16E0},
              {189, 0x802F56E0u, func_802F56E0}, {194, 0x803096E0u, func_803096E0}, {195, 0x8030D6E0u, func_8030D6E0}};

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

/* The module's own natives (native_math.c, native_vec.c's leaves, and
 * native_j3d.c's hooks) are off in the comparison, as in the reference:
 * their translations run. The timing turns them on, as in play. */
static int s_module_natives;
int bw_native_call(CPUState* cpu, u32 address) {
    return s_module_natives && (bluewake_native_vec(cpu, address) || bluewake_native_math(cpu, address));
}

extern CPUState bw_guest_cpu;
extern u8 bw_guest_mem1[];
void bluewake_composite_set_gather_pipe(BwGatherPipeWrite write);
int bluewake_composite_direct_calls(bool enabled, const bool* sources_dirty, const bool* decrementer_pending,
                                    const u32* pi_cause, const u32* pi_mask);
void bluewake_composite_set_gather_pipe_bytes(BwGatherPipeBytes bytes);

#define AREA 0x80100000u
#define AREA_BYTES 0x10000u
#define STATICS 0x803ED000u
#define UNIT_PAGE 0x803F6000u
#define PAGE_BYTES 0x1000u
#define RETURN_ADDRESS 0xFFFFFFFCu

static u32 seed = 0x2545F491u;
static u32 next(void) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static u32 float_bits(void) {
    const u32 sign = next() & 0x80000000u;
    switch (next() % 32u) {
    case 0: return sign;
    case 1: return 0x3F800000u;
    case 2: return next() % 8u ? 0x3F000000u : sign | (next() & 0x007FFFFFu); /* now and then a denormal */
    default: return sign | ((115u + next() % 20u) << 23) | (next() & 0x007FFFFFu);
    }
}

static void put(u8* ram, u32 address, u32 value) { write_be32(ram + (address - GC_RAM_BASE), value); }

/* Pipe logs: 'B' length bytes for each run handed over. */
typedef struct PipeLog {
    u8 data[16384];
    u32 length;
} PipeLog;
static PipeLog s_log_module, s_log_routed;
static void log_bytes(PipeLog* log, const u8* bytes, u32 size) {
    if (log->length + 4u + size > sizeof log->data)
        exit(1);
    memcpy(log->data + log->length, &size, 4);
    memcpy(log->data + log->length + 4u, bytes, size);
    log->length += 4u + size;
}
static void module_word(u64 value, u8 size) { (void)value; (void)size; exit(1); }
static void module_bytes(const u8* bytes, u32 size) { log_bytes(&s_log_module, bytes, size); }
static void routed_word(u64 value, u8 size) { (void)value; (void)size; exit(1); }
static void routed_bytes(const u8* bytes, u32 size) { log_bytes(&s_log_routed, bytes, size); }
static void sink_word(u64 value, u8 size) { (void)value; (void)size; }

static unsigned s_unexpected_service;
static int edge_service(void* user, CPUState* cpu, u32 address) {
    (void)user;
    (void)cpu;
    if ((address & ~3u) != RETURN_ADDRESS)
        s_unexpected_service++;
    return 1;
}

static const u32 FUNCTIONS[] = {BLUEWAKE_J3D_FIFO_POS_MTX,   BLUEWAKE_J3D_FIFO_NRM_MTX,  BLUEWAKE_J3D_FIFO_NRM_MTX33,
                                BLUEWAKE_BG_CHK_SAME_ACTOR_PID, BLUEWAKE_BG_CHK_GRP_THROUGH, BLUEWAKE_PSMTX_MULT_VEC_SR,
                                BLUEWAKE_MTXCALC_BASIC,      BLUEWAKE_MTXCALC_SOFTIMAGE, BLUEWAKE_MTXCALC_MAYA};
static const char* const NAMES[] = {"J3DFifoLoadPosMtxImm",      "J3DFifoLoadNrmMtxImm", "J3DFifoLoadNrmMtxImm3x3",
                                    "cBgS_Chk::ChkSameActorPid", "dBgW::ChkGrpThrough",  "PSMTXMultVecSR",
                                    "J3DMtxCalcBasic",           "J3DMtxCalcSoftimage",  "J3DMtxCalcMaya"};

/* The data: random words and floats in the area, J3DSys's page and the
 * unit pair; then each function's structures. */
static CPUState build(u8* ram, unsigned which, unsigned scenario) {
    for (u32 i = 0; i < AREA_BYTES; i += 4u)
        put(ram, AREA + i, i < 0x8000u ? float_bits() : next());
    for (u32 i = 0; i < PAGE_BYTES; i += 4u)
        put(ram, STATICS + i, float_bits());
    put(ram, 0x803F66F0u, 0u);
    put(ram, 0x803F66F4u, 0x3F800000u);
    CPUState c;
    memset(&c, 0, sizeof c);
    c.ram_size = GC_MAIN_RAM_SIZE;
    for (unsigned r = 0; r < 32; ++r) {
        c.gpr[r] = next();
        c.fpr[r] = f64_value(convert_to_double(float_bits()));
        c.ps1[r] = f64_value(convert_to_double(float_bits()));
    }
    c.gpr[1] = AREA + 0xF000u - 8u * (next() % 16u);
    c.gpr[2] = AREA + 0x9000u + 13072u;
    put(ram, AREA + 0x9000u, 0x3F800000u);
    c.gpr[13] = AREA + 0x9100u + 26460u;
    put(ram, AREA + 0x9100u, 4u);
    put(ram, AREA + 0x9104u, AREA + 0x0000u); /* sines: the floats at the area's start */
    put(ram, AREA + 0x9108u, AREA + 0x4000u); /* cosines */
    c.lr = RETURN_ADDRESS | (next() & 3u);
    c.pc = FUNCTIONS[which];
    c.cr = next();
    c.xer = next();
    c.ctr = next();
    c.msr = PPC_MSR_FP;
    c.hid2 = PPC_HID2_LSQE;
    c.fpscr = (next() & 0xFFFFF000u) | (scenario % 4u == 0u ? 4u : 0u);
    c.reserve_valid = (scenario & 1u) != 0u;
    c.reserve_addr = scenario % 4u == 1u ? 0x803EDB80u : c.gpr[1] - 64u;
    c.cycle_observation_suffix = next();
    c.cycle_budget = 16384;
    c.downcount = -(s64)(next() % 64u);
    c.cycle_deadline_budget = scenario % 3u == 0u ? 0 : 100000;
    if (scenario % 17u == 3u)
        c.cycle_deadline_budget = 20 + (s64)(next() % 300u); /* the natives decline some */
    switch (which) {
    case 0: case 1: case 2: /* a matrix */
        c.gpr[3] = AREA + 0xA000u + 4u * (next() % 64u);
        break;
    case 3: { /* cBgS_Chk */
        const u32 self = AREA + 0xA000u;
        c.gpr[3] = self;
        const u32 pid = next() % 4u;
        put(ram, self + 8u, scenario % 5u == 0u ? 0xFFFFFFFFu : pid);
        c.gpr[4] = scenario % 7u == 0u ? 0xFFFFFFFFu : next() % 4u;
        ram[self + 12u - GC_RAM_BASE] = (u8)(next() % 2u);
        break;
    }
    case 4: { /* dBgW, its group table, a pass check */
        const u32 self = AREA + 0xA000u, bgd = AREA + 0xA200u, table = AREA + 0xA400u, check = AREA + 0xAE00u;
        put(ram, self + 148u, bgd);
        put(ram, bgd + 36u, table);
        static const u32 bits[] = {0u, 0x100u, 0x200u, 0x400u, 0x80000u, 0x700u, 0x80100u, 0x1u};
        for (u32 g = 0; g < 32u; ++g)
            put(ram, table + 52u * g + 48u, bits[next() % 8u] | bits[next() % 8u]);
        put(ram, check + 4u, next() & 0x1Fu);
        c.gpr[3] = self;
        c.gpr[4] = next() % 32u;
        c.gpr[5] = scenario % 9u == 0u ? 0u : check;
        c.gpr[6] = scenario % 11u == 0u ? 1u : 2u;
        break;
    }
    case 5: /* a matrix, a vector, an output */
        c.gpr[3] = AREA + 0x8000u;
        c.gpr[4] = AREA + 0x8040u;
        c.gpr[5] = scenario % 3u == 0u ? AREA + 0x8040u : AREA + 0x8080u;
        break;
    default: { /* a model, a transform, J3DSys */
        const u32 model = AREA + 0xA000u, data = AREA + 0xA200u, nodes = AREA + 0xA300u;
        const u32 joint = next() % 16u, info = AREA + 0x7000u + 4u * (next() % 64u);
        put(ram, 0x803EDA90u, model);
        put(ram, model + 4u, data);
        put(ram, model + 132u, AREA + 0xA800u);
        put(ram, model + 140u, AREA + 0xA900u);
        put(ram, data + 44u, nodes);
        for (u32 j = 0; j < 16u; ++j) {
            put(ram, nodes + 4u * j, AREA + 0xAC00u + 32u * j);
            ram[AREA + 0xAC00u + 32u * j + 27u - GC_RAM_BASE] = (u8)(next() % 2u);
        }
        for (u32 i = 0; i < 3u; ++i) {
            if (scenario % 2u == 0u) {
                put(ram, info + 4u * i, 0x3F800000u);
                put(ram, 0x803EDBB0u + 4u * i, 0x3F800000u);
            }
            put(ram, 0x803EDBBCu + 4u * i, 0x3F000000u + (next() & 0x007FFFFFu));
            write_be16(ram + (info + 12u + 2u * i - GC_RAM_BASE), (u16)next());
        }
        c.gpr[4] = joint;
        c.gpr[5] = info;
        break;
    }
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
           memcmp(a + (STATICS - GC_RAM_BASE), b + (STATICS - GC_RAM_BASE), PAGE_BYTES) == 0 &&
           memcmp(a + (UNIT_PAGE - GC_RAM_BASE), b + (UNIT_PAGE - GC_RAM_BASE), PAGE_BYTES) == 0;
}

static void copy_regions(u8* to, const u8* from) {
    memcpy(to + (AREA - GC_RAM_BASE), from + (AREA - GC_RAM_BASE), AREA_BYTES);
    memcpy(to + (STATICS - GC_RAM_BASE), from + (STATICS - GC_RAM_BASE), PAGE_BYTES);
    memcpy(to + (UNIT_PAGE - GC_RAM_BASE), from + (UNIT_PAGE - GC_RAM_BASE), PAGE_BYTES);
}

static void set_natives(int on) {
    bluewake_native_fifo_enabled = on;
    bluewake_native_bg_enabled = on;
    bluewake_native_vec_sr_enabled = on;
    bluewake_native_mtxcalc_enabled = on;
}

/* The hooked chunks from `entry` until control reaches the return address:
 * a chunk left at a boundary goes on in the chunk that has the address, as
 * the chassis loop (its edge filter letting every boundary pass) would. */
static int run_routed(u32 entry) {
    CPUState* c = &bw_guest_cpu;
    c->pc = entry;
    for (unsigned guard = 0; guard < 64u; ++guard) {
        if ((c->pc & ~3u) == RETURN_ADDRESS) {
            bw_gather_pipe_drain();
            return 1;
        }
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
        fprintf(stderr, "usage: native_entries_test MODULE.dll [CASES_PER_FUNCTION]\n");
        return 2;
    }
    _putenv_s("BLUEWAKE_NATIVE_MATH", "0");
    const unsigned cases = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 4000u;
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
    void (*module_pipe)(BwGatherPipeWrite) =
        (void (*)(BwGatherPipeWrite))(void*)GetProcAddress(lib, "bluewake_composite_set_gather_pipe");
    void (*module_pipe_bytes)(BwGatherPipeBytes) =
        (void (*)(BwGatherPipeBytes))(void*)GetProcAddress(lib, "bluewake_composite_set_gather_pipe_bytes");
    if (!get || !guest_cpu || !mem1 || !set_edge || !direct || !filter || !module_pipe || !module_pipe_bytes)
        return 1;
    const StaticRecompModuleDesc* mod = get();
    if (mod->cpu_state_size != sizeof(CPUState) || strcmp(mod->game_id, "GZLE01") != 0)
        return 1;
    static const bool clear = false;
    static const u32 zero = 0u;
    if (!direct(true, &clear, &clear, &zero, &zero) || !filter(true))
        return 1;
    set_edge(edge_service, NULL);
    module_pipe(module_word);
    module_pipe_bytes(module_bytes);
    u32 size = 0;
    u8* reference_ram = mem1(&size);
    memset(reference_ram, 0, GC_MAIN_RAM_SIZE);

    /* The hooked side: its own chunk table, direct calls and edge filter on,
     * the host quiet; its own pipe; MEM1 its own (guest_cpu.c). */
    for (unsigned i = 0; i < 256u; ++i)
        s_table[i] = missing_chunk;
    for (unsigned i = 0; i < sizeof CHUNKS / sizeof CHUNKS[0]; ++i)
        s_table[CHUNKS[i].index] = CHUNKS[i].fn;
    static bool sources_dirty, decrementer_pending;
    static u32 pi_cause, pi_mask;
    bluewake_composite_direct_calls(true, &sources_dirty, &decrementer_pending, &pi_cause, &pi_mask);
    bw_edge_watch_ready = true;
    bw_edge_filter_enabled = true;
    bluewake_composite_set_gather_pipe(routed_word);
    bluewake_composite_set_gather_pipe_bytes(routed_bytes);
    u8* routed_ram = bw_guest_mem1;
    memset(routed_ram, 0, GC_MAIN_RAM_SIZE);
    u8* source = calloc(1, GC_MAIN_RAM_SIZE);
    if (source == NULL)
        return 1;

    for (unsigned which = 0; which < sizeof FUNCTIONS / sizeof FUNCTIONS[0]; ++which) {
        for (unsigned i = 0; i < cases; ++i) {
            const CPUState start = build(source, which, i);
            CPUState results[3];
            PipeLog logs[3];
            for (unsigned run = 0; run < 3u; ++run) {
                if (run == 0u) {
                    copy_regions(reference_ram, source);
                    CPUState* g = guest_cpu();
                    *g = start;
                    g->ram = reference_ram;
                    mod->on_state_loaded(g);
                    s_log_module.length = 0;
                    s_unexpected_service = 0;
                    if (!mod->dispatch(g, FUNCTIONS[which]) || s_unexpected_service != 0u) {
                        fprintf(stderr, "case %u (%s): the translation did not finish\n", i, NAMES[which]);
                        return 1;
                    }
                    results[0] = *g;
                    results[0].ram = NULL;
                    logs[0] = s_log_module;
                } else {
                    copy_regions(routed_ram, source);
                    set_natives(run == 1u);
                    bw_guest_cpu = start;
                    bw_guest_cpu.ram = routed_ram;
                    ppc_fpscr_updated(&bw_guest_cpu);
                    s_log_routed.length = 0;
                    if (!run_routed(FUNCTIONS[which])) {
                        fprintf(stderr, "case %u (%s): the hooked chunks did not finish\n", i, NAMES[which]);
                        return 1;
                    }
                    results[run] = bw_guest_cpu;
                    results[run].ram = NULL;
                    logs[run] = s_log_routed;
                    if (!same_regions(routed_ram, reference_ram)) {
                        fprintf(stderr, "case %u (%s, natives %s): RAM differs\n", i, NAMES[which],
                                run == 1u ? "on" : "off");
                        return 1;
                    }
                }
            }
            if (compare(NAMES[which], i, &results[1], &results[0]) ||
                compare(NAMES[which], i, &results[2], &results[0]) || logs[1].length != logs[0].length ||
                memcmp(logs[1].data, logs[0].data, logs[0].length) != 0 || logs[2].length != logs[0].length ||
                memcmp(logs[2].data, logs[0].data, logs[0].length) != 0) {
                fprintf(stderr, "case %u (%s, seed %08X): mismatch (pipe %u/%u/%u bytes)\n", i, NAMES[which], seed,
                        logs[0].length, logs[1].length, logs[2].length);
                return 1;
            }
            if (i % 256u == 255u && memcmp(routed_ram, reference_ram, GC_MAIN_RAM_SIZE) != 0) {
                fprintf(stderr, "case %u (%s): a byte outside the compared ranges differs\n", i, NAMES[which]);
                return 1;
            }
        }
        printf("%08X %s: %u cases identical through the hooked chunks, natives on and off\n", FUNCTIONS[which],
               NAMES[which], cases);
        fflush(stdout);
    }
    /* ns per call through the hooked chunks, the second set's natives on and
     * off, with the module's other natives on as in play: the chunk's entry
     * and return included, the dispatcher not. */
    s_module_natives = 1;
    bluewake_native_j3d_enabled = 1;
    bluewake_composite_set_gather_pipe_bytes(NULL);
    bluewake_composite_set_gather_pipe(sink_word);
    for (unsigned which = 0; which < sizeof FUNCTIONS / sizeof FUNCTIONS[0]; ++which) {
        CPUState start = build(source, which, 2u);
        start.fpscr = 0;
        start.cycle_deadline_budget = 0;
        start.downcount = 0;
        start.cycle_budget = (s64)1 << 40;
        start.reserve_valid = false;
        if (which == 5u)
            start.gpr[5] = AREA + 0x8080u;
        for (u32 i = 0; i < 12u; ++i) /* ordinary matrices */
            put(source, (which == 5u ? AREA + 0x8000u : 0x803EDB80u) + 4u * i, i % 5u == 0u ? 0x3F800000u : 0x3E000000u);
        for (u32 i = 0; i < 3u; ++i)
            put(source, AREA + 0x8040u + 4u * i, 0x3FC00000u);
        copy_regions(routed_ram, source);
        u8 statics[72];
        memcpy(statics, routed_ram + (0x803EDB80u - GC_RAM_BASE), sizeof statics);
        double best[2] = {1e30, 1e30};
        for (unsigned round = 0; round < 6u; ++round) {
            const int on = (int)(round & 1u);
            set_natives(on);
            bw_guest_cpu = start;
            bw_guest_cpu.ram = routed_ram;
            ppc_fpscr_updated(&bw_guest_cpu);
            const unsigned calls = 200000u;
            LARGE_INTEGER f, t0, t1;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&t0);
            for (unsigned k = 0; k < calls; ++k) {
                memcpy(bw_guest_cpu.gpr, start.gpr, 14u * sizeof(u32));
                bw_guest_cpu.lr = start.lr;
                if (which >= 6u)
                    memcpy(routed_ram + (0x803EDB80u - GC_RAM_BASE), statics, sizeof statics);
                run_routed(FUNCTIONS[which]);
            }
            QueryPerformanceCounter(&t1);
            const double ns = (double)(t1.QuadPart - t0.QuadPart) * 1e9 / (double)f.QuadPart / calls;
            if (ns < best[on])
                best[on] = ns;
        }
        printf("%08X %s: hooked chunk %.1f ns/call with the natives off, %.1f on (%.2fx)\n", FUNCTIONS[which],
               NAMES[which], best[0], best[1], best[0] / best[1]);
    }
    bluewake_native_fifo_report();
    bluewake_native_bg_report();
    bluewake_native_vec_sr_report();
    bluewake_native_mtxcalc_report();
    return 0;
}
