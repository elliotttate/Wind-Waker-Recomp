/* tests/native_leaf_test.c's hooked mode (-DNATIVE7_HOOKED=1): the hooks, end
 * to end.
 *
 * The test then also links the eleven chunks the seventh set's natives hook
 * or reach (0034, 0145, 0146, 0148, 0162, 0163, 0191 and 0203 hooked; 0158,
 * 0195 and 0201, whose leaves the natives replay with their callers),
 * compiled as the
 * module compiles them (their sources the player's own, hooked on a copy,
 * never distributed), with every native the chunks name, guest_cpu.c,
 * gather_pipe.c and simulation_timing.c:
 *
 *   (%HOOKED%: the snapshot's composite-src chunks_dol for those eleven and
 *    generated.h, copied, then the builder's source steps run on the copy)
 *   for each chunk in 0034_text1_800896E0 0145_text1_802456E0 0146_text1_802496E0
 *                     0148_text1_802516E0 0158_text1_802796E0 0162_text1_802896E0
 *                     0163_text1_8028D6E0
 *                     0191_text1_802FD6E0 0195_text1_8030D6E0 0201_text1_803256E0
 *                     0203_text1_8032D6E0:
 *     clang -c -O2 -march=x86-64-v3 -ffp-contract=off -fno-slp-vectorize
 *       -mllvm -large-interval-freq-threshold=10
 *       -DMODULE_GAME_ID=\"GZLE01\" -DDOLRECOMP_CPU_HEADER=\"core/cpu.h\"
 *       -DBW_GUEST_MEM1=bw_guest_mem1 -DBW_GUEST_MEM1_SIZE=0x02000000u
 *       -DBLUEWAKE_EDGE_FILTER=1 -DBLUEWAKE_GATHER_PIPE_BATCH=1
 *       -Icmake/composite -I%HOOKED% -I%SNAP%\recompcore\GXRuntime\include
 *       -I%SNAP%\recompcore\Source\Core\Core\PowerPC\StaticRecomp
 *       %HOOKED%\chunks_dol\chunk_NNNN.c -o chunk_NNNN.o
 *   clang -O2 -march=x86-64-v3 -ffp-contract=off -DNATIVE7_HOOKED=1
 *     -DBW_GUEST_MEM1=bw_guest_mem1 -DBW_GUEST_MEM1_SIZE=0x02000000u (the
 *     test's usual sources and includes) chunk_*.o cmake/composite/native_*.c
 *     cmake/composite/guest_cpu.c cmake/composite/gather_pipe.c
 *     cmake/composite/simulation_timing.c %SNAP%\gxruntime.lib -o native_leaf_hooked_test.exe
 *
 * Every case the native runs is then also run through the hooked chunks from
 * the function's entry - once with every native on (this set's, the earlier
 * sets' and the leaves' behind the direct calls, as in play), once with them
 * all off - a small chassis loop going on in the next chunk where one leaves
 * at a boundary; the CPU state and the writable pages must match the
 * module's translation byte for byte both times. The chunks' MEM1
 * (guest_cpu.c) is read-only but the test's pages, like the images. */
#include "gather_pipe.h"
#include "native_anim.h"
#include "native_bg.h"
#include "native_cc.h"
#include "native_fifo.h"
#include "native_game_math.h"
#include "native_gx.h"
#include "native_j3d.h"
#include "native_kankyo.h"
#include "native_math.h"
#include "native_mtxcalc.h"
#include "native_search.h"
#include "native_skin.h"
#include "native_vec.h"

void func_800896E0(CPUState*);
void func_802456E0(CPUState*);
void func_802496E0(CPUState*);
void func_802516E0(CPUState*);
void func_802796E0(CPUState*);
void func_802896E0(CPUState*);
void func_8028D6E0(CPUState*);
void func_802FD6E0(CPUState*);
void func_8030D6E0(CPUState*);
void func_803256E0(CPUState*);
void func_8032D6E0(CPUState*);
static const struct {
    unsigned index;
    u32 start;
    BwChunkFn fn;
} HOOKED_CHUNKS[] = {{34, 0x800896E0u, func_800896E0},  {145, 0x802456E0u, func_802456E0},
                     {146, 0x802496E0u, func_802496E0}, {148, 0x802516E0u, func_802516E0},
                     {158, 0x802796E0u, func_802796E0}, {162, 0x802896E0u, func_802896E0},
                     {163, 0x8028D6E0u, func_8028D6E0},
                     {191, 0x802FD6E0u, func_802FD6E0}, {195, 0x8030D6E0u, func_8030D6E0},
                     {201, 0x803256E0u, func_803256E0}, {203, 0x8032D6E0u, func_8032D6E0}};

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

unsigned dolrecomp_call_depth;
extern CPUState bw_guest_cpu;
extern u8 bw_guest_mem1[];
void bluewake_composite_set_gather_pipe(BwGatherPipeWrite write);
static void hooked_pipe(u64 value, u8 size) {
    (void)value;
    (void)size;
    fprintf(stderr, "a gather pipe write in the hooked chunks\n");
    exit(1);
}

static u32 s_hooked_entry[32];
static unsigned long long s_hooked_cases[32];
static void hooked_summary(void) {
    for (unsigned i = 0; i < 32u && s_hooked_entry[i] != 0u; ++i)
        printf("%08X: %llu cases also identical through the hooked chunks, natives on and off\n", s_hooked_entry[i],
               s_hooked_cases[i]);
    printf("(each native's count below takes in both its direct calls and its calls from the hooks)\n");
    fflush(stdout);
}

static void hooked_natives(int on) {
    s_hooked_leaves = on;
    bluewake_native_libm_enabled = on;
    bluewake_native_bgblk_enabled = on;
    bluewake_native_rot_enabled = on;
    bluewake_native_calc_enabled = on;
    bluewake_native_geom_enabled = on;
    bluewake_native_jas_enabled = on;
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
    bluewake_native_gx_enabled = on;
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
    if (protect_image(bw_guest_mem1) == NULL) {
        fprintf(stderr, "cannot protect the chunks' MEM1\n");
        exit(1);
    }
}

/* The hooked chunks from `entry` on `start` (the regions as `before`), with
 * every native on and then off, against `reference`. */
static void hooked_case(const Harness* h, u32 entry, const char* name, unsigned index, const CPUState* start,
                        const CPUState* reference, bool flush) {
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
        while ((bw_guest_cpu.pc & ~3u) != RETURN_ADDRESS && guard++ < 1000000u) {
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
    while (slot < 31u && s_hooked_entry[slot] != 0u && s_hooked_entry[slot] != entry)
        slot++;
    s_hooked_entry[slot] = entry;
    s_hooked_cases[slot]++;
}

/* ns per call through the hooked chunks from `entry` on `base` (its pages as
 * in `ram`): this set's natives off, then on, with the earlier sets' and the
 * leaves' on in both, as in play. The GPRs, f1 to f8 and LR are set again
 * before each call, as the direct benchmark sets them; the chunk's entry and
 * return dispatch are in the time, the chassis's own dispatcher is not (a
 * call from translated code reaches the chunk directly). */
static void bench_reset(u8* ram, u32 entry); /* native_leaf_test.c */
static void hooked_bench(const u8* ram, u32 entry, const CPUState* base, unsigned calls, const char* name,
                         unsigned variant) {
    hooked_setup();
    copy_regions(bw_guest_mem1, ram);
    double best[2] = {1e30, 1e30};
    for (unsigned round = 0; round < 6u; ++round) {
        const int on = (int)(round & 1u);
        hooked_natives(1);
        bluewake_native_libm_enabled = bluewake_native_bgblk_enabled = on;
        bluewake_native_rot_enabled = bluewake_native_geom_enabled = bluewake_native_calc_enabled = on;
        bluewake_native_jas_enabled = on;
        bw_guest_cpu = *base;
        bw_guest_cpu.ram = bw_guest_mem1;
        ppc_fpscr_updated(&bw_guest_cpu);
        const double t0 = now_ns();
        for (unsigned i = 0; i < calls; ++i) {
            CPUState* c = &bw_guest_cpu;
            memcpy(c->gpr, base->gpr, sizeof base->gpr);
            memcpy(c->fpr, base->fpr, 9u * sizeof base->fpr[0]);
            c->lr = RETURN_ADDRESS;
            c->pc = entry;
            bench_reset(bw_guest_mem1, entry);
            do
                s_hooked_table[(c->pc - 0x800016E0u) >> 14](c);
            while ((c->pc & ~3u) != RETURN_ADDRESS);
        }
        const double ns = (now_ns() - t0) / calls;
        if (ns < best[on])
            best[on] = ns;
    }
    hooked_natives(0);
    printf("%08X %s%s: through the hooked chunks %.1f ns/call with this set's natives off, %.1f on (%.2fx)\n", entry,
           name, variant ? " (variant)" : "", best[0], best[1], best[0] / best[1]);
}
