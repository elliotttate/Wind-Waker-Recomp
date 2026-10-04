/* cmake/composite/native_gx.c (the fifth set: the GX SDK's FIFO writers)
 * against the translations it stands in for.
 *
 * Build and run from the worktree root (x64; the Visual Studio environment):
 *
 *   clang -O2 -march=x86-64-v3 -ffp-contract=off -Icmake/composite -Itests
 *     -IE:\Github\Wind-Waker-Recomp\ref\recompcore\GXRuntime\include
 *     -IE:\Github\Wind-Waker-Recomp\ref\recompcore\Source\Core\Core\PowerPC\StaticRecomp
 *     tests/native_gx_test.c cmake/composite/native_gx.c cmake/composite/direct_calls.c
 *     cmake/composite/gather_pipe.c
 *     E:\Github\Wind-Waker-Recomp\build\windows\app\gxruntime_build\gxruntime.lib -o native_gx_test.exe
 *   native_gx_test MODULE.dll [CASES_PER_FUNCTION=60000] [BENCH_CALLS=1000000]
 *
 * The hooked build (-DNATIVE5_HOOKED=1, with the hooked chunks: the recipe
 * is in native_gx_hooked.h) also runs every case the native runs through the
 * hooked chunks from the function's entry, natives on and off.
 *
 * MODULE.dll is a Windows game module (gGZLE01_recomp.dll); the test reads it
 * and writes nothing but its own memory. For each function: random registers,
 * flags, FPSCR, reservation (sometimes on a granule the function stores to)
 * and cycle state; __GXData, the texture objects, the colours, the matrices,
 * the small-data areas and the table GXSetTevOrder reads filled with random
 * bytes, with the fields that choose paths biased toward the ones play takes
 * (the dirty state clear or set, the region callbacks the SDK's own, the
 * first word nonzero) and sometimes not; pointers in RAM, sometimes not
 * (unaligned, a mirror, the hardware, past MEM1); the turn's budget spent or
 * not; deadlines near and inside the work; a write journal, aliases over
 * MEM1, an exception pending; the host busy (the module then asks its edge
 * service at every boundary it crosses, so a native that ran across one fails
 * the comparison), the edge filter off and a boundary watched (for
 * GXLoadTexObj, which always crosses one: it must decline); and the gather
 * pipe in each of its host modes (native5_harness.h). Every byte of the CPU
 * state, of the writable pages and of what the pipe hands the host must
 * match - or, where the native declines, nothing may have changed. */
#include "native_gx.h"
#include "native5_harness.h"

#define STACK 0x80F00000u /* 0x2000: the frames */
#define SDA2 0x80F10000u  /* 0x8000: r2's small data (the __GXData pointer at r2 - 12848) */
#define R2 (SDA2 + 0x4000u)
#define GXD 0x80F20000u   /* 0x4000: __GXData (and what indices past its tables reach) */
#define SDA 0x80F2C000u   /* 0x1000: r13's small data (GXLoadTexObjPreLoaded's register tables) */
#define R13 (SDA + 0x8000u)
#define OBJ 0x80F38000u   /* 0x2000: texture objects, colours, matrices */
#define TABLE 0x803A1000u /* 0x2000: GXSetTevOrder's channel table (0x803A1E80) */

static const Region k_regions[] = {
    {STACK, 0x2000u}, {SDA2, 0x8000u}, {GXD, 0x4000u}, {SDA, 0x1000u}, {OBJ, 0x2000u}, {TABLE, 0x2000u},
};

typedef struct Native {
    u32 entry;
    const char* name;
    s64 shortest;    /* cycles on the shortest path */
    bool boundaries; /* every path crosses a boundary between chunks */
} Native;

static const Native k_natives[] = {
    {BLUEWAKE_GX_LOAD_POS_MTX_IMM, "GXLoadPosMtxImm", 20, false},
    {BLUEWAKE_GX_LOAD_NRM_MTX_IMM, "GXLoadNrmMtxImm", 22, false},
    {BLUEWAKE_GX_SET_TEV_COLOR, "GXSetTevColor", 29, false},
    {BLUEWAKE_GX_SET_TEV_COLOR_S10, "GXSetTevColorS10", 29, false},
    {BLUEWAKE_GX_SET_TEV_KCOLOR, "GXSetTevKColor", 29, false},
    {BLUEWAKE_GX_SET_ARRAY, "GXSetArray", 24, false},
    {BLUEWAKE_GX_SET_TEV_ORDER, "GXSetTevOrder", 50, false},
    {BLUEWAKE_GX_CALL_DISPLAY_LIST, "GXCallDisplayList", 27, false},
    {BLUEWAKE_GX_BEGIN, "GXBegin", 29, false},
    {BLUEWAKE_GX_LOAD_TEX_OBJ_PRELOADED, "GXLoadTexObjPreLoaded", 89, false},
    {BLUEWAKE_GX_LOAD_TEX_OBJ, "GXLoadTexObj", 120, true},
    {BLUEWAKE_GX_SET_SU_TEX_REGS, "__GXSetSUTexRegs", 34, false},
    {BLUEWAKE_GX_SET_VAT, "__GXSetVAT", 12, false},
    {BLUEWAKE_GX_SET_MATRIX_INDEX, "__GXSetMatrixIndex", 19, false},
    {0x80325CD4u, "__GXUpdateBPMask", 9, false},
    {0x803233B0u, "__GXSetGenMode", 9, false},
    {0x80321958u, "__GXSetVCD", 17, true},
    {0x803214B0u, "__GXXfVtxSpecs", 10, false},
    {0x803219ACu, "__GXCalculateVLim", 10, false},
    {0x80321608u, "GXSetVtxDesc", 12, false},
    {0x80321AD0u, "GXClearVtxDesc", 10, false},
    {0x80321B08u, "GXSetVtxAttrFmt", 12, false},
    {0x80322604u, "GXSetTexCoordGen2", 20, false},
    {0x803228D4u, "GXSetNumTexGens", 10, false},
    {0x80323328u, "GXSetCullMode", 10, false},
    {0x80324390u, "GXSetChanAmbColor", 6, false},
    {0x80324484u, "GXSetChanMatColor", 6, false},
    {0x80324578u, "GXSetNumChans", 10, false},
    {0x803245BCu, "GXSetChanCtrl", 20, false},
    {0x80324D28u, "GXGetTexObjFmt", 2, false},
    {0x80325774u, "GXSetTevIndirect", 20, false},
    {0x80325C00u, "GXSetNumIndStages", 8, false},
    {0x80325C28u, "GXSetTevDirect", 20, false},
    {0x80325E50u, "GXSetTevColorIn", 15, false},
    {0x80325E94u, "GXSetTevAlphaIn", 15, false},
    {0x80325ED8u, "GXSetTevColorOp", 15, false},
    {0x80325F40u, "GXSetTevAlphaOp", 15, false},
    {0x80326104u, "GXSetTevKColorSel", 15, false},
    {0x80326170u, "GXSetTevKAlphaSel", 15, false},
    {0x803261DCu, "GXSetTevSwapMode", 15, false},
    {0x803262C8u, "GXSetAlphaCompare", 15, false},
    {0x80326578u, "GXSetNumTevStages", 10, false},
    {0x80326858u, "GXSetBlendMode", 15, false},
    {0x803268ACu, "GXSetColorUpdate", 10, false},
    {0x803268D8u, "GXSetAlphaUpdate", 10, false},
    {0x80326904u, "GXSetZMode", 10, false},
    {0x80326938u, "GXSetZCompLoc", 10, false},
    {0x80326A8Cu, "GXSetDstAlpha", 10, false},
    {0x80326FD8u, "GXSetCurrentMtx", 10, false},
};
#define NATIVE_COUNT (sizeof k_natives / sizeof k_natives[0])

#include "native_gx_leaders.h"

/* The jump tables (in the game's .data), filled with the function's own
 * block leaders mostly: {table, entries, the leaders}. */
typedef struct JumpTable {
    u32 entry, table, count;
    u32 after; /* the cases follow the bctr (none loops back before it) */
    const u32* leaders;
    u32 leader_count;
} JumpTable;
#define LEADERS(x) x, (u32)(sizeof x / sizeof x[0])
static const JumpTable k_jump_tables[] = {
    {0x80321608u, 0x803A1910u, 26u, 0x80321624u, LEADERS(k_leaders_80321608)}, /* GXSetVtxDesc: attr */
    {0x80321B08u, 0x803A1978u, 17u, 0x80321B40u, LEADERS(k_leaders_80321B08)}, /* GXSetVtxAttrFmt: attr - 9 */
    {0x80322604u, 0x803A1A60u, 21u, 0x80322638u, LEADERS(k_leaders_80322604)}, /* GXSetTexCoordGen2: src */
    {0x80322604u, 0x803A1A44u, 7u, 0x803227C4u, LEADERS(k_leaders_80322604)},  /* and dst */
};

/* A case of a jump table: one of the leaders after its bctr. */
static u32 jump_case(const JumpTable* j) {
    for (;;) {
        const u32 at = j->leaders[below(j->leader_count)];
        if (at > j->after && at - j->entry < 0x1000u)
            return at;
    }
}

#ifdef NATIVE5_HOOKED
#include "native_gx_hooked.h"
#endif

typedef struct Case {
    CPUState cpu;
    PipeCase pipe;
    bool must_decline, journal, aliases, flush;
} Case;

static void fill(u8* ram, u32 start, u32 bytes) {
    for (u32 i = 0; i < bytes; i += 4u)
        put32(ram, start + i, next());
}

/* A pointer: in the area mostly; sometimes unaligned, a mirror, the
 * hardware, across or past MEM1's end, or anything but MEM1 (whose pages
 * outside the test's areas are read-only: a store there would fault). */
static u32 pointer(u32 area, u32 span, u32 align) {
    const u32 at = area + (below(span) & ~(align - 1u));
    switch (below(64u)) {
    case 0: return at + 1u + below(3u);
    case 1: return at | 0x40000000u;
    case 2: return 0xCC000000u + 4u * below(64u);
    case 3: return GC_RAM_BASE + GC_MAIN_RAM_SIZE - 2u + 4u * below(4u);
    case 4: {
        const u32 any = next();
        return any - GC_RAM_BASE < 0x02000000u ? any ^ 0x10000000u : any;
    }
    default: return at;
    }
}

static Case build(u8* ram, unsigned which, unsigned scenario) {
    Case k;
    memset(&k, 0, sizeof k);
    for (unsigned r = 0; r < sizeof k_regions / sizeof k_regions[0]; ++r)
        fill(ram, k_regions[r].start, k_regions[r].bytes);
    CPUState* c = &k.cpu;
    random_cpu(c, ram);
    const Native* n = &k_natives[which];
    c->pc = n->entry;
    c->gpr[1] = STACK + 0x1000u + 8u * below(256u);
    c->gpr[2] = below(128u) == 0u ? (0x90000000u | (next() & 0x0FFFFFFFu)) : R2;
    c->gpr[13] = below(128u) == 0u ? (0x90000000u | (next() & 0x0FFFFFFFu)) : R13;
    /* __GXData: its pointer, the first word (vNumNot, bpSentNot: zero sends a
     * flush primitive first), the dirty state and the region callbacks. */
    put32(ram, R2 - 12848u, below(64u) == 0u ? pointer(GXD, 0x400u, 4u) : GXD);
    if (below(32u) == 0u)
        put32(ram, GXD, 0u);
    /* GXCallDisplayList and GXBegin decline where the dirty state needs a call. */
    const u32 clean = n->entry == BLUEWAKE_GX_CALL_DISPLAY_LIST || n->entry == BLUEWAKE_GX_BEGIN ? 8u : 2u;
    const u32 dirty = below(clean) != 0u ? 0u : below(3u) == 0u ? next() : (next() & 0x3Fu);
    put32(ram, GXD + 1268u, dirty);
    /* The TEV stages' texture maps (indices into __GXData's tables): small,
     * or 0xFF (none), the 0x100 flag sometimes; sometimes anything. */
    for (u32 i = 0; i < 16u; ++i)
        if (below(64u) != 0u)
            put32(ram, GXD + 1180u + 4u * i, (below(4u) == 0u ? 0xFFu : below(8u)) | (below(4u) == 0u ? 0x100u : 0u));
    put32(ram, GXD + 1040u, below(32u) == 0u ? next() : 0x8031FA48u); /* __GXDefaultTexRegionCallback */
    put32(ram, GXD + 1044u, below(32u) == 0u ? next() : 0x8031FAC4u); /* __GXDefaultTlutRegionCallback */
    /* Arguments: small values mostly (enumerations, counts, flags). */
    for (unsigned r = 3; r <= 10; ++r)
        c->gpr[r] = below(4u) == 0u ? below(256u) : below(4u) == 0u ? 0xFFu : below(16u);
    for (unsigned t = 0; t < sizeof k_jump_tables / sizeof k_jump_tables[0]; ++t) {
        const JumpTable* j = &k_jump_tables[t];
        if (j->entry == n->entry)
            for (u32 i = 0; i < j->count; ++i)
                put32(ram, j->table + 4u * i,
                      below(16u) == 0u ? next() : jump_case(j));
    }
    switch (n->entry) {
    case BLUEWAKE_GX_LOAD_POS_MTX_IMM:
    case BLUEWAKE_GX_LOAD_NRM_MTX_IMM:
        c->gpr[3] = pointer(OBJ, 0x1F00u, 4u);
        c->gpr[4] = below(4u) == 0u ? next() : 3u * below(10u);
        /* Zeros, denormals (which the paired store flushes), infinities, NaNs. */
        if (c->gpr[3] - OBJ < 0x1F00u)
            for (u32 i = 0; i < 12u; ++i)
                if (below(4u) == 0u)
                    put32(ram, (c->gpr[3] & ~3u) + 4u * i, any_single(24u));
        break;
    case BLUEWAKE_GX_SET_TEV_COLOR:
    case BLUEWAKE_GX_SET_TEV_COLOR_S10:
    case BLUEWAKE_GX_SET_TEV_KCOLOR:
        c->gpr[3] = below(8u) == 0u ? below(256u) : below(4u);
        c->gpr[4] = pointer(OBJ, 0x1F00u, below(2u) ? 4u : 1u);
        break;
    case BLUEWAKE_GX_SET_ARRAY:
        c->gpr[3] = below(8u) == 0u ? below(256u) : 9u + below(17u);
        c->gpr[5] = below(4u) == 0u ? next() : below(64u);
        break;
    case BLUEWAKE_GX_SET_TEV_ORDER:
        c->gpr[3] = below(8u) == 0u ? below(256u) : below(16u);
        c->gpr[4] = below(4u) == 0u ? 0xFFu : below(4u) == 0u ? next() : below(8u);
        c->gpr[5] = below(4u) == 0u ? 0xFFu : below(4u) == 0u ? (0x100u | below(8u)) : below(8u);
        c->gpr[6] = below(4u) == 0u ? 0xFFu : below(16u) == 0u ? next() : below(9u);
        break;
    case BLUEWAKE_GX_BEGIN:
        c->gpr[3] = below(8u) == 0u ? below(256u) : 0x80u + 8u * below(8u);
        c->gpr[4] = below(8u) == 0u ? below(256u) : below(8u);
        c->gpr[5] = next() & 0xFFFFu;
        break;
    case BLUEWAKE_GX_LOAD_TEX_OBJ_PRELOADED:
        c->gpr[3] = pointer(OBJ, 0x1000u, 4u);
        c->gpr[4] = pointer(OBJ + 0x1000u, 0x0F00u, 4u);
        c->gpr[5] = below(8u) == 0u ? below(256u) : below(8u);
        break;
    case BLUEWAKE_GX_LOAD_TEX_OBJ:
        c->gpr[3] = pointer(OBJ, 0x1000u, 4u);
        c->gpr[4] = below(8u) == 0u ? below(256u) : below(8u);
        break;
    case BLUEWAKE_GX_SET_MATRIX_INDEX:
        c->gpr[3] = below(8u) == 0u ? below(256u) : below(12u);
        break;
    case 0x80321608u: /* GXSetVtxDesc(attr, type) */
        c->gpr[3] = below(8u) == 0u ? below(256u) : below(27u);
        c->gpr[4] = below(8u) == 0u ? below(256u) : below(4u);
        break;
    case 0x80321B08u: /* GXSetVtxAttrFmt(fmt, attr, cnt, type, frac) */
        c->gpr[3] = below(8u) == 0u ? below(256u) : below(8u);
        c->gpr[4] = below(8u) == 0u ? below(256u) : 9u + below(18u);
        break;
    case 0x80322604u: /* GXSetTexCoordGen2(dst, func, src, mtx, normalize, postmtx) */
        c->gpr[3] = below(8u) == 0u ? below(256u) : below(9u);
        c->gpr[4] = below(8u) == 0u ? below(256u) : below(12u);
        c->gpr[5] = below(8u) == 0u ? below(256u) : below(22u);
        c->gpr[6] = below(4u) == 0u ? 60u : 30u + 3u * below(12u);
        c->gpr[8] = below(4u) == 0u ? 125u : 64u + 3u * below(21u);
        break;
    case 0x80324390u: /* GXSetChanAmbColor(chan, color) */
    case 0x80324484u: /* GXSetChanMatColor */
        c->gpr[3] = below(8u) == 0u ? below(256u) : below(7u);
        c->gpr[4] = pointer(OBJ, 0x1F00u, below(2u) ? 4u : 1u);
        break;
    case 0x80324D28u: /* GXGetTexObjFmt(obj) */
        c->gpr[3] = pointer(OBJ, 0x1F00u, 4u);
        break;
    default:
        break;
    }
    /* Texture objects: the TLUT name small mostly, so the callback's region is in __GXData. */
    if (n->entry == BLUEWAKE_GX_LOAD_TEX_OBJ || n->entry == BLUEWAKE_GX_LOAD_TEX_OBJ_PRELOADED) {
        const u32 obj = c->gpr[3] & ~3u;
        if (obj >= OBJ && obj + 32u <= OBJ + 0x2000u) {
            put32(ram, obj + 24u, below(16u) == 0u ? (below(2u) ? next() : 20u + below(4u)) : below(20u));
            if (below(2u) != 0u)
                put8(ram, obj + 31u, (u8)(next() & ~2u)); /* the TLUT path */
        }
    }
    /* A reservation, sometimes on a granule the function stores to. */
    if (c->reserve_valid) {
        switch (below(4u)) {
        case 0: c->reserve_addr = c->gpr[1] - 8u * below(8u); break;
        case 1: c->reserve_addr = GXD + 4u * below(0x140u); break;
        case 2: c->reserve_addr = OBJ + 4u * below(0x800u); break;
        default: break;
        }
        if (below(4u) == 0u)
            c->reserve_addr |= 0x40000000u;
    }
    clock_edges(c, scenario, n->shortest);
    k.pipe = random_pipe(scenario);
    k.journal = scenario % 41u == 7u;
    k.aliases = scenario % 43u == 8u;
    k.flush = below(4u) == 0u;
    k.must_decline = k.journal || k.aliases || c->exception != 0u || k.pipe.mode != PIPE_BATCH;
    return k;
}

/* Host scenarios: busy (the module asks its edge service at every boundary
 * too), the filter off or a boundary watched (the natives' own view only). */
enum { HOST_QUIET, HOST_BUSY, HOST_FILTER_OFF, HOST_WATCHED };

static unsigned host_scenario(unsigned scenario) {
    switch (scenario % 37u) {
    case 5: return HOST_BUSY;
    case 11: return HOST_FILTER_OFF;
    case 17: return HOST_WATCHED;
    default: return HOST_QUIET;
    }
}

static void set_host(unsigned host) {
    reset_host();
    if (host == HOST_BUSY)
        s_sources_dirty = true;
    if (host == HOST_FILTER_OFF)
        bw_edge_filter_enabled = false;
    if (host == HOST_WATCHED) {
        watch(0x8031FA48u); /* GXLoadTexObj's region callback */
        watch(0x80324F14u);
        watch(0x8031FAC4u); /* PreLoaded's TLUT callback */
        watch(0x80325CD4u); /* GXBegin's __GXUpdateBPMask */
        watch(0x803214B0u); /* and __GXXfVtxSpecs */
    }
}

static int native_gx(CPUState* cpu, u32 address) { return bluewake_native_gx(cpu, address); }

/* --- The microbenchmark: ordinary inputs, the budget large. -------------- */

static void bench(Harness* h, unsigned calls) {
    h->set_pipe(sink_word);
    h->set_pipe_bytes(sink_bytes);
    bluewake_composite_set_gather_pipe(sink_word);
    bluewake_composite_set_gather_pipe_bytes(sink_bytes);
    reset_host();
    u8* ram = h->native_ram;
    for (unsigned which = 0; which < NATIVE_COUNT; ++which) {
        const Native* n = &k_natives[which];
        for (unsigned variant = 0; variant < 1u; ++variant) {
            Case k;
            unsigned tries = 0;
            /* A case the native runs, quiet. */
            do {
                k = build(ram, which, 6u * (tries + 1u));
                k.cpu.exception = 0;
                k.cpu.downcount = 0;
                k.cpu.cycle_budget = (s64)1 << 40;
                k.cpu.cycle_deadline_budget = 0;
                k.cpu.reserve_valid = false;
                put32(ram, GXD + 1268u, variant ? 0x3Fu : 0u);
                if (n->entry == BLUEWAKE_GX_LOAD_TEX_OBJ || n->entry == BLUEWAKE_GX_LOAD_TEX_OBJ_PRELOADED)
                    put8(ram, (k.cpu.gpr[3] & ~3u) + 31u, 2u); /* no TLUT */
                CPUState probe = k.cpu;
                copy_regions(h->before, ram);
                set_test_pipe(PIPE_BATCH);
                bw_gather_pipe_length = 0;
                const int ran = bluewake_native_gx(&probe, n->entry);
                copy_regions(ram, h->before);
                if (ran)
                    break;
            } while (++tries < 1000u);
            if (tries == 1000u) {
                printf("%08X %s: no benchmark case\n", n->entry, n->name);
                continue;
            }
            const CPUState base = k.cpu;
            bluewake_composite_set_gather_pipe(sink_word); /* the probe set the logging pipe */
            bluewake_composite_set_gather_pipe_bytes(sink_bytes);
            bw_gather_pipe_length = 0;
            copy_regions(h->before, ram);
            double best_t = 1e30, best_n = 1e30;
            for (unsigned round = 0; round < 5u; ++round) {
                copy_regions(h->reference_ram, h->before);
                CPUState* g = h->guest_cpu();
                *g = base;
                g->ram = h->reference_ram;
                h->mod->on_state_loaded(g);
                double t0 = now_ns();
                for (unsigned i = 0; i < calls; ++i) {
                    g->gpr[1] = base.gpr[1];
                    memcpy(&g->gpr[3], &base.gpr[3], 4u * sizeof(u32));
                    g->lr = RETURN_ADDRESS;
                    if (variant)
                        put32(h->reference_ram, GXD + 1268u, 0x3Fu);
                    g->pc = n->entry;
                    h->mod->dispatch(g, n->entry);
                }
                const double t = (now_ns() - t0) / calls;
                CPUState native = base;
                t0 = now_ns();
                for (unsigned i = 0; i < calls; ++i) {
                    native.gpr[1] = base.gpr[1];
                    memcpy(&native.gpr[3], &base.gpr[3], 4u * sizeof(u32));
                    native.lr = RETURN_ADDRESS;
                    if (variant)
                        put32(ram, GXD + 1268u, 0x3Fu);
                    native.pc = n->entry;
                    if (!bluewake_native_gx(&native, n->entry)) {
                        printf("%08X %s: the benchmark case declined\n", n->entry, n->name);
                        break;
                    }
                    bw_gather_pipe_drain();
                }
                const double d = (now_ns() - t0) / calls;
                if (t < best_t)
                    best_t = t;
                if (d < best_n)
                    best_n = d;
            }
            copy_regions(ram, h->before);
            printf("%08X %s%s: translation %.1f ns/call through the dispatcher, native %.1f ns/call (%.2fx)\n",
                   n->entry, n->name, variant ? " (dirty: every callee)" : "", best_t, best_n, best_t / best_n);
#ifdef NATIVE5_HOOKED
            hooked_bench(h, ram, n->entry, &base, calls, n->name, variant);
#endif
            fflush(stdout);
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: native_gx_test MODULE.dll [CASES_PER_FUNCTION] [BENCH_CALLS]\n");
        return 2;
    }
    const unsigned cases = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 60000u;
    const unsigned bench_calls = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 1000000u;
    Harness h;
    if (!harness_load(&h, argv[1], false, k_regions, sizeof k_regions / sizeof k_regions[0]))
        return 1;
    int failed = 0;
    for (unsigned which = 0; which < NATIVE_COUNT; ++which) {
        const Native* n = &k_natives[which];
        unsigned ran = 0, declined = 0;
        s_flushed_inside = 0;
        for (unsigned i = 0; i < cases; ++i) {
            Case k = build(h.native_ram, which, i);
            const unsigned host = host_scenario(i);
            set_host(host);
            const bool must = k.must_decline || (n->boundaries && host != HOST_QUIET);
            const int r = harness_case(&h, native_gx, n->entry, n->name, i, &k.cpu, &k.pipe, must, k.journal,
                                       k.aliases, k.flush);
            if (r && (host == HOST_FILTER_OFF || host == HOST_WATCHED)) {
                /* It ran with the filter off (its path must have crossed no
                 * boundary) or with the callbacks' and callees' boundaries
                 * watched (it must have crossed none of those). With the host
                 * busy, the translation asks its edge service at the first
                 * boundary it crosses. */
                s_sources_dirty = true;
                (void)harness_translate(&h, &k.cpu, n->entry);
                s_sources_dirty = false;
                if (s_unexpected_service != 0u &&
                    (host == HOST_FILTER_OFF || !bw_edge_unwatched(s_unexpected_at))) {
                    fprintf(stderr, "case %u (%s): ran across a boundary (%08X) with the filter off or it watched\n",
                            i, n->name, s_unexpected_at);
                    return 1;
                }
            }
            ran += (unsigned)r;
            declined += (unsigned)(r == 0);
        }
        reset_host();
        printf("%08X %s: %u cases, %u identical (%llu handing the batch over before its bytes), "
               "%u declined unchanged, 0 mismatches\n",
               n->entry, n->name, cases, ran, s_flushed_inside, declined);
        fflush(stdout);
        if (ran < 30000u && cases >= 60000u) {
            printf("  fewer than 30,000 compared cases\n");
            failed = 1;
        }
    }
#ifdef NATIVE5_HOOKED
    hooked_summary();
#endif
    bluewake_native_gx_report();
    if (bench_calls != 0u)
        bench(&h, bench_calls);
    return failed;
}
