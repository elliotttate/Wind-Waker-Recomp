/* tests/native_gx_test.c's hooked mode (-DNATIVE5_HOOKED=1): the hooks, end
 * to end.
 *
 * The test then also links the five chunks native_entries.py hooks for this
 * set (0171, 0181, 0199, 0200, 0201), compiled as the module compiles
 * them (their sources the player's own, hooked on a copy, never
 * distributed), with native_gx.c, native_fifo.c (the second set's hooks in
 * 0181), guest_cpu.c, direct_calls.c and gather_pipe.c:
 *
 *   (%HOOKED%: composite-src's chunks_dol/chunk_NNNN for those five and
 *    generated.h, copied, then scripts/windows/native_entries.py %HOOKED%)
 *   for each chunk in 0171_text1_802AD6E0 0181_text1_802D56E0 0199_text1_8031D6E0
 *                     0200_text1_803216E0 0201_text1_803256E0:
 *     clang -c -O2 -march=x86-64-v3 -ffp-contract=off -fno-slp-vectorize
 *       -mllvm -large-interval-freq-threshold=10
 *       -DMODULE_GAME_ID=\"GZLE01\" -DDOLRECOMP_CPU_HEADER=\"core/cpu.h\"
 *       -DBW_GUEST_MEM1=bw_guest_mem1 -DBW_GUEST_MEM1_SIZE=0x02000000u
 *       -DBLUEWAKE_EDGE_FILTER=1 -DBLUEWAKE_GATHER_PIPE_BATCH=1
 *       -Icmake/composite -I%HOOKED% -I...GXRuntime\include -I...StaticRecomp
 *       %HOOKED%\chunks_dol\chunk_NNNN.c -o chunk_NNNN.o
 *   clang -O2 -march=x86-64-v3 -ffp-contract=off -DNATIVE5_HOOKED=1
 *     -DBW_GUEST_MEM1=bw_guest_mem1 -DBW_GUEST_MEM1_SIZE=0x02000000u
 *     (the test's usual sources and includes) cmake/composite/guest_cpu.c
 *     chunk_*.o ...\gxruntime.lib -o native_gx_hooked_test.exe
 *
 * Every case the native runs is then also run through the hooked chunks from
 * the function's entry - once with the natives on, once with them off - a
 * small chassis loop going on in the next chunk where one leaves at a
 * boundary; the CPU state, the writable pages and what the pipe hands the
 * host (in the case's own pipe mode) must match the module's translation
 * byte for byte both times. The chunks' MEM1 (guest_cpu.c) is read-only but
 * the test's pages, like the images. */

#include "native_fifo.h"

void func_802AD6E0(CPUState*);
void func_802D56E0(CPUState*);
void func_8031D6E0(CPUState*);
void func_803216E0(CPUState*);
void func_803256E0(CPUState*);
static const struct {
    unsigned index;
    u32 start;
    BwChunkFn fn;
} HOOKED_CHUNKS[] = {{171, 0x802AD6E0u, func_802AD6E0}, {181, 0x802D56E0u, func_802D56E0},
                     {199, 0x8031D6E0u, func_8031D6E0}, {200, 0x803216E0u, func_803216E0},
                     {201, 0x803256E0u, func_803256E0}};

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

/* The leaves behind the chunks' direct calls (the matrix and vector leaves)
 * run translated, in chunks this test does not link: no path the test runs
 * reaches one. */
int bw_native_call(CPUState* cpu, u32 address) {
    (void)cpu;
    (void)address;
    return 0;
}

unsigned dolrecomp_call_depth;
extern CPUState bw_guest_cpu;
extern u8 bw_guest_mem1[];

static u32 s_hooked_entry[128];
static unsigned long long s_hooked_cases[128];
static void hooked_summary(void) {
    for (unsigned i = 0; i < 128u && s_hooked_entry[i] != 0u; ++i)
        printf("%08X: %llu cases also identical through the hooked chunks, natives on and off\n", s_hooked_entry[i],
               s_hooked_cases[i]);
    printf("(each native's count below takes in both its direct calls and its calls from the hooks)\n");
    fflush(stdout);
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
    if (protect_image(bw_guest_mem1) == NULL) {
        fprintf(stderr, "cannot protect the chunks' MEM1\n");
        exit(1);
    }
}

/* The hooked chunks from `entry` on `start` (the regions as `before`), in the
 * case's pipe mode, natives on and then off, against `reference` and
 * log_model. */
static void hooked_case(const Harness* h, u32 entry, const char* name, unsigned index, const CPUState* start,
                        const CPUState* reference, const PipeCase* pipe, bool flush, bool declined) {
    (void)declined;
    hooked_setup();
    for (int on = 1; on >= 0; --on) {
        copy_regions(bw_guest_mem1, h->before);
        bluewake_native_gx_enabled = on;
        bluewake_native_fifo_enabled = on;
        set_test_pipe(pipe->mode);
        if (pipe->mode == PIPE_BATCH)
            prefill_test_pipe(pipe->prefill);
        else
            bw_gather_pipe_length = 0;
        log_native.length = 0;
        log_target = &log_native;
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
        bw_gather_pipe_flush();
        CPUState got = bw_guest_cpu;
        got.ram = start->ram;
        if (memcmp(&got, reference, sizeof got) != 0 || !same_regions(bw_guest_mem1, h->reference_ram) ||
            !same_logs(&log_native, &log_model)) {
            fprintf(stderr, "case %u (%s, seed %08X): the hooked chunks, natives %s, differ\n", index, name, seed,
                    on ? "on" : "off");
            report_cpu(&got, reference);
            report_ram(bw_guest_mem1, h->reference_ram);
            report_logs(&log_native, &log_model);
            exit(1);
        }
    }
    bluewake_native_gx_enabled = 0;
    bluewake_native_fifo_enabled = 0;
    unsigned slot = 0;
    while (slot < 127u && s_hooked_entry[slot] != 0u && s_hooked_entry[slot] != entry)
        slot++;
    s_hooked_entry[slot] = entry;
    s_hooked_cases[slot]++;
}

/* ns per call through the hooked chunks from `entry` on `base` (its pages as
 * in `ram`): the natives off, then on. r1, r3 to r6, f1 to f4 and LR are set again
 * before each call; the chunk's entry and return dispatch are in the time,
 * the chassis's own dispatcher is not (a call from translated code reaches
 * the chunk directly). */
static void bench_gd_reset(u8* ram); /* native_gx_test.c */
static void hooked_bench(const Harness* h, const u8* ram, u32 entry, const CPUState* base, unsigned calls,
                         const char* name, unsigned variant) {
    (void)h;
    hooked_setup();
    copy_regions(bw_guest_mem1, ram);
    set_test_pipe(PIPE_BATCH);
    bluewake_composite_set_gather_pipe(sink_word);
    bluewake_composite_set_gather_pipe_bytes(sink_bytes);
    double best[2] = {1e30, 1e30};
    for (unsigned round = 0; round < 6u; ++round) {
        const int on = (int)(round & 1u);
        bluewake_native_gx_enabled = on;
        bw_guest_cpu = *base;
        bw_guest_cpu.ram = bw_guest_mem1;
        ppc_fpscr_updated(&bw_guest_cpu);
        const double t0 = now_ns();
        for (unsigned i = 0; i < calls; ++i) {
            CPUState* c = &bw_guest_cpu;
            c->gpr[1] = base->gpr[1];
            memcpy(&c->gpr[3], &base->gpr[3], 4u * sizeof(u32));
            memcpy(&c->fpr[1], &base->fpr[1], 4u * sizeof base->fpr[0]);
            c->lr = RETURN_ADDRESS;
            if (variant)
                put32(bw_guest_mem1, GXD + 1268u, 0x3Fu);
            bench_gd_reset(bw_guest_mem1);
            c->pc = entry;
            do
                s_hooked_table[(c->pc - 0x800016E0u) >> 14](c);
            while ((c->pc & ~3u) != RETURN_ADDRESS);
            bw_gather_pipe_drain();
        }
        const double ns = (now_ns() - t0) / calls;
        if (ns < best[on])
            best[on] = ns;
    }
    bluewake_native_gx_enabled = 0;
    printf("%08X %s%s: through the hooked chunks %.1f ns/call with the natives off, %.1f on (%.2fx)\n", entry, name,
           variant ? " (dirty)" : "", best[0], best[1], best[0] / best[1]);
}
