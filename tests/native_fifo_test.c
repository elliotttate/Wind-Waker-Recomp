/* cmake/composite/native_fifo.c against the translations it stands in for.
 *
 * Build and run from the worktree root (x64; the Visual Studio environment):
 *
 *   clang -O2 -march=x86-64-v3 -ffp-contract=off -Icmake/composite
 *     -IE:\Github\Wind-Waker-Recomp\ref\recompcore\GXRuntime\include
 *     -IE:\Github\Wind-Waker-Recomp\ref\recompcore\Source\Core\Core\PowerPC\StaticRecomp
 *     tests/native_fifo_test.c cmake/composite/native_fifo.c cmake/composite/gather_pipe.c
 *     E:\Github\Wind-Waker-Recomp\build\windows\app\gxruntime_build\gxruntime.lib -o native_fifo_test.exe
 *   native_fifo_test MODULE.dll [CASES_PER_FUNCTION=60000] [BENCH_CALLS=2000000]
 *
 * MODULE.dll is a Windows game module (gGZLE01_recomp.dll) without these
 * natives; the test reads it and writes nothing but its own memory. For each
 * function: random registers, flags, FPSCR, reservation and cycle state; a
 * matrix in RAM (aligned, unaligned, at the ends of MEM1, or not in RAM at
 * all: a mirror, the hardware, past MEM1's end); the turn's budget spent or
 * not; deadlines near and inside the block; a write journal, aliases over
 * MEM1, an exception pending; and the gather pipe in each of its host modes
 * (a run of bytes at a time, partly filled so the batch flushes inside the
 * function or not; a word at a time; no writer). The function runs through
 * the module's translation (twice: with its pipe batched and word by word)
 * and through bluewake_native_fifo; every byte of the CPU state, of RAM and
 * of what the pipe hands the host must match, the last access's cycle
 * suffix included - or, where the native declines, nothing may have changed,
 * the pipe included. RAM outside the test area is read-only in both images,
 * so a stray write anywhere faults.
 *
 * Then a microbenchmark: the translation through the module's dispatcher
 * (which includes the dispatch itself; a one-instruction function's dispatch
 * is measured for scale) against the native, ns per call. */
#include "native_fifo.h"
#include "gather_pipe.h"
#include "StaticRecompABI.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* gather_pipe.c, linked in: the test's own pipe, the one the native uses. */
void bluewake_composite_set_gather_pipe(BwGatherPipeWrite write);
void bluewake_composite_set_gather_pipe_bytes(BwGatherPipeBytes bytes);

#define AREA 0x80100000u
#define AREA_BYTES 0x2000u
#define EMPTY_FUNCTION 0x802DB978u /* draw__9J3DPacketFv: blr */

static u32 seed = 0x0F1F0A3Bu;
static u32 next(void) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

/* What a pipe hands the host, in order: 'W' size value for a word to the
 * writer, 'B' length bytes for a run of bytes. */
typedef struct PipeLog {
    u8 data[8192];
    u32 length;
} PipeLog;
static PipeLog log_module, log_native, log_model;
static PipeLog* log_native_target = &log_native;

static void log_word(PipeLog* log, u64 value, u8 size) {
    if (log->length + 10u > sizeof log->data) {
        fprintf(stderr, "pipe log overflow\n");
        exit(1);
    }
    log->data[log->length++] = 'W';
    log->data[log->length++] = size;
    memcpy(log->data + log->length, &value, 8);
    log->length += 8u;
}

static void log_bytes(PipeLog* log, const u8* bytes, u32 size) {
    if (log->length + 5u + size > sizeof log->data) {
        fprintf(stderr, "pipe log overflow\n");
        exit(1);
    }
    log->data[log->length++] = 'B';
    memcpy(log->data + log->length, &size, 4);
    memcpy(log->data + log->length + 4u, bytes, size);
    log->length += 4u + size;
}

static void module_word(u64 value, u8 size) { log_word(&log_module, value, size); }
static void module_bytes(const u8* bytes, u32 size) { log_bytes(&log_module, bytes, size); }
static void native_word(u64 value, u8 size) { log_word(log_native_target, value, size); }
static void native_bytes(const u8* bytes, u32 size) { log_bytes(log_native_target, bytes, size); }
static void sink_word(u64 value, u8 size) { (void)value; (void)size; }
static void sink_bytes(const u8* bytes, u32 size) { (void)bytes; (void)size; }

static void journal(u32 offset, u32 size, void* user) {
    (void)offset;
    (void)size;
    (void)user;
    abort();
}

/* Both images: all of MEM1 read-only but the test area. */
static u8* protect_image(u8* p) {
    DWORD old;
    if (p == NULL || !VirtualProtect(p, GC_MAIN_RAM_SIZE, PAGE_READONLY, &old) ||
        !VirtualProtect(p + (AREA - GC_RAM_BASE), AREA_BYTES, PAGE_READWRITE, &old))
        return NULL;
    return p;
}

/* A module built with BW_GUEST_MEM1 (the Windows builder's) runs its
 * translated code on its own MEM1 array, whatever a state's ram says. */
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

static const u32 FUNCTIONS[] = {BLUEWAKE_J3D_FIFO_POS_MTX, BLUEWAKE_J3D_FIFO_NRM_MTX, BLUEWAKE_J3D_FIFO_NRM_MTX33};
static const s64 CYCLES[] = {32, 29, 29};

enum { PIPE_BATCH, PIPE_WORDS, PIPE_NONE };

typedef struct Case {
    CPUState cpu;
    unsigned pipe;
    u32 prefill;
    bool journal, aliases;
} Case;

static Case build(u8* ram, unsigned which, unsigned scenario) {
    Case k;
    memset(&k, 0, sizeof k);
    for (u32 i = 0; i < AREA_BYTES; i += 4u)
        write_be32(ram + (AREA - GC_RAM_BASE) + i, next());
    CPUState* c = &k.cpu;
    c->ram = ram;
    c->ram_size = GC_MAIN_RAM_SIZE;
    for (unsigned r = 0; r < 32; ++r) {
        c->gpr[r] = next();
        c->fpr[r] = f64_value(((u64)next() << 32) | next());
        c->ps1[r] = f64_value(((u64)next() << 32) | next());
    }
    c->gpr[3] = AREA + 4u * (next() % 1024u);
    switch (scenario % 23u) {
    case 1: c->gpr[3] += 1u + next() % 3u; break;           /* unaligned: plain reads too */
    case 2: c->gpr[3] = 0xCC000000u + 4u * (next() % 64u); break; /* the hardware */
    case 3: c->gpr[3] |= 0x40000000u; break;                 /* the uncached mirror */
    case 4: c->gpr[3] = GC_RAM_BASE + GC_MAIN_RAM_SIZE - 4u * (next() % 16u); break; /* across MEM1's end */
    case 5: c->gpr[3] = 0x90000000u; break;                  /* not MEM1 */
    default: break;
    }
    if (scenario % 7u == 3u)
        c->gpr[4] = next() & 0xFFFFu;
    c->lr = 0xFFFFFFFCu | (next() & 3u);
    c->pc = FUNCTIONS[which];
    c->ctr = next();
    c->cr = next();
    c->xer = next();
    c->msr = next();
    c->hid2 = next();
    for (unsigned g = 0; g < 8; ++g)
        c->gqr[g] = next();
    c->fpscr = next() & ~3u;
    c->reserve_valid = (scenario & 1u) != 0u;
    c->reserve_addr = scenario % 4u == 1u ? 0xCC008000u : c->gpr[3] & ~31u;
    c->cycle_observation_suffix = next();
    c->cycle_budget = 16384;
    c->downcount = -(s64)(next() % 64u);
    c->cycle_deadline_budget = scenario % 4u == 0u ? 0 : 100000;
    const s64 cycles = CYCLES[which];
    switch (scenario % 29u) {
    case 1: c->cycle_deadline_budget = cycles - c->downcount + (s64)(next() % 5u) - 2; break; /* at the block's end */
    case 2: c->cycle_deadline_budget = cycles - 1 - (s64)(next() % 8u); break;               /* inside it */
    case 3: c->cycle_deadline_budget = -(s64)(next() % 100u); break;                         /* none */
    case 4: c->downcount = -c->cycle_budget; break;                                          /* budget spent */
    case 5: c->downcount = -c->cycle_budget + 1 + (s64)(next() % 4u); break;                 /* nearly spent */
    case 6: c->cycle_budget = 1 + (s64)(next() % 40u); c->downcount = 0; break;
    case 7: c->downcount = (s64)(next() % 8u); break;
    case 8: c->exception = 1u; break;
    case 9: c->cycle_budget = 0; break;
    default: break;
    }
    k.pipe = scenario % 9u == 4u ? PIPE_NONE : scenario % 3u == 1u ? PIPE_WORDS : PIPE_BATCH;
    /* A batch partly filled: the function's bytes fit, reach the flush point
     * exactly, or cross it. */
    k.prefill = scenario % 5u == 0u ? 0u : next() % BW_GATHER_PIPE_BATCH;
    k.journal = scenario % 31u == 7u;
    k.aliases = scenario % 37u == 8u;
    return k;
}

static void set_native_pipe(unsigned mode) {
    bluewake_composite_set_gather_pipe(mode == PIPE_NONE ? NULL : native_word);
    bluewake_composite_set_gather_pipe_bytes(mode == PIPE_BATCH ? native_bytes : NULL);
}

static void prefill_native_pipe(u32 count) {
    for (u32 i = 0; i < count; ++i)
        bw_gather_pipe_buffer[i] = (u8)(i * 37u + 11u);
    bw_gather_pipe_length = count;
}

static void report_cpu(const CPUState* got, const CPUState* want) {
    for (unsigned b = 0; b < sizeof *got; ++b)
        if (((const u8*)got)[b] != ((const u8*)want)[b])
            fprintf(stderr, "  CPU byte %u: got %02X want %02X\n", b, ((const u8*)got)[b], ((const u8*)want)[b]);
}

static bool same_logs(const PipeLog* a, const PipeLog* b) {
    return a->length == b->length && memcmp(a->data, b->data, a->length) == 0;
}

/* The bytes a log's entries carry, in order (word values big-endian). */
static u32 log_stream(const PipeLog* log, u8* out) {
    u32 n = 0;
    for (u32 i = 0; i < log->length;) {
        if (log->data[i] == 'W') {
            const u8 size = log->data[i + 1];
            u64 value;
            memcpy(&value, log->data + i + 2, 8);
            for (u8 b = 0; b < size; ++b)
                out[n++] = (u8)(value >> (8u * (size - 1u - b)));
            i += 10u;
        } else {
            u32 size;
            memcpy(&size, log->data + i + 1, 4);
            memcpy(out + n, log->data + i + 5, size);
            n += size;
            i += 5u + size;
        }
    }
    return n;
}

typedef void (*SetPipeFn)(BwGatherPipeWrite);
typedef void (*SetPipeBytesFn)(BwGatherPipeBytes);

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
        fprintf(stderr, "usage: native_fifo_test MODULE.dll [CASES_PER_FUNCTION] [BENCH_CALLS]\n");
        return 2;
    }
    _putenv_s("BLUEWAKE_NATIVE_MATH", "0");
    const unsigned cases = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 60000u;
    const unsigned bench_calls = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 2000000u;
    HMODULE lib = LoadLibraryA(argv[1]);
    if (lib == NULL) {
        fprintf(stderr, "cannot load %s (%lu)\n", argv[1], GetLastError());
        return 1;
    }
    StaticRecompGetModuleFn get = (StaticRecompGetModuleFn)(void*)GetProcAddress(lib, STATICRECOMP_GET_MODULE_SYMBOL);
    CPUState* (*guest_cpu)(void) = (CPUState * (*)(void))(void*)GetProcAddress(lib, "bluewake_composite_guest_cpu");
    SetPipeFn module_set_pipe = (SetPipeFn)(void*)GetProcAddress(lib, "bluewake_composite_set_gather_pipe");
    SetPipeBytesFn module_set_pipe_bytes =
        (SetPipeBytesFn)(void*)GetProcAddress(lib, "bluewake_composite_set_gather_pipe_bytes");
    if (get == NULL || guest_cpu == NULL || module_set_pipe == NULL || module_set_pipe_bytes == NULL) {
        fprintf(stderr, "not a BlueWake Windows module\n");
        return 1;
    }
    const StaticRecompModuleDesc* mod = get();
    if (mod->cpu_state_size != sizeof(CPUState) || strcmp(mod->game_id, "GZLE01") != 0) {
        fprintf(stderr, "CPU state size %u, expected %u\n", mod->cpu_state_size, (unsigned)sizeof(CPUState));
        return 1;
    }
    u8* reference_ram = module_image(lib);
    u8* native_ram = protect_image(VirtualAlloc(NULL, GC_MAIN_RAM_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    u8* before = malloc(AREA_BYTES);
    static u8 stream_a[8192], stream_b[8192];
    if (reference_ram == NULL || native_ram == NULL || before == NULL)
        return 1;

    unsigned ran[3] = {0}, declined[3] = {0}, flushed_inside[3] = {0};
    for (unsigned which = 0; which < 3; ++which) {
        const u32 entry = FUNCTIONS[which];
        for (unsigned i = 0; i < cases; ++i) {
            Case k = build(native_ram, which, i);
            memcpy(reference_ram + (AREA - GC_RAM_BASE), native_ram + (AREA - GC_RAM_BASE), AREA_BYTES);
            memcpy(before, native_ram + (AREA - GC_RAM_BASE), AREA_BYTES);

            /* The native, on the test's own pipe. */
            set_native_pipe(k.pipe);
            if (k.pipe == PIPE_BATCH)
                prefill_native_pipe(k.prefill);
            u8 pipe_before[BW_GATHER_PIPE_BATCH + 8u];
            memcpy(pipe_before, bw_gather_pipe_buffer, sizeof pipe_before);
            const u32 length_before = bw_gather_pipe_length;
            log_native.length = 0;
            CPUState native = k.cpu;
            ppc_fpscr_updated(&native);
            const CPUState untouched = native;
            if (k.journal)
                g_mem_write_journal = journal;
            if (k.aliases)
                g_ppc_guest_aliases_overlap_mem1 = true;
            const int accepted = bluewake_native_fifo(&native, entry);
            g_mem_write_journal = NULL;
            g_ppc_guest_aliases_overlap_mem1 = false;
            if (!accepted) {
                declined[which]++;
                if (memcmp(&native, &untouched, sizeof native) != 0 || log_native.length != 0u ||
                    bw_gather_pipe_length != length_before ||
                    memcmp(pipe_before, bw_gather_pipe_buffer, sizeof pipe_before) != 0 ||
                    memcmp(before, native_ram + (AREA - GC_RAM_BASE), AREA_BYTES) != 0) {
                    fprintf(stderr, "case %u (%08X): declined but changed state\n", i, entry);
                    report_cpu(&native, &untouched);
                    return 1;
                }
                continue;
            }
            if (k.pipe == PIPE_NONE || k.journal || k.aliases || k.cpu.exception) {
                fprintf(stderr, "case %u (%08X): ran where it must decline\n", i, entry);
                return 1;
            }
            ran[which]++;
            if (k.pipe == PIPE_BATCH && log_native.length != 0u)
                flushed_inside[which]++; /* the batch reached its flush point inside */
            if (k.pipe == PIPE_BATCH)
                bw_gather_pipe_flush();

            /* The translation, word by word, then batched. */
            CPUState* g = guest_cpu();
            CPUState words_result;
            u32 stream_length = 0;
            for (unsigned pass = 0; pass < 2; ++pass) {
                memcpy(reference_ram + (AREA - GC_RAM_BASE), before, AREA_BYTES);
                module_set_pipe(module_word);
                module_set_pipe_bytes(pass == 0 ? NULL : module_bytes);
                log_module.length = 0;
                *g = k.cpu;
                g->ram = reference_ram;
                mod->on_state_loaded(g);
                if (!mod->dispatch(g, entry)) {
                    fprintf(stderr, "case %u: the translation did not run\n", i);
                    return 1;
                }
                if (pass == 0) {
                    words_result = *g;
                    stream_length = log_stream(&log_module, stream_a);
                    /* The model: the translation's words through this pipe as
                     * the case left it (the same prefill, the same flushes). */
                    if (k.pipe == PIPE_BATCH) {
                        set_native_pipe(PIPE_BATCH);
                        prefill_native_pipe(k.prefill);
                        log_native_target = &log_model;
                        log_model.length = 0;
                        for (u32 j = 0; j < log_module.length; j += 10u) {
                            u64 value;
                            memcpy(&value, log_module.data + j + 2, 8);
                            bw_gather_pipe_put(value, log_module.data[j + 1]);
                        }
                        bw_gather_pipe_flush();
                        log_native_target = &log_native;
                        if (!same_logs(&log_model, &log_native)) {
                            fprintf(stderr, "case %u (%08X, prefill %u): the batch differs from the stores'\n", i,
                                    entry, k.prefill);
                            return 1;
                        }
                    } else if (!same_logs(&log_module, &log_native)) {
                        fprintf(stderr, "case %u (%08X): the pipe words differ\n", i, entry);
                        return 1;
                    }
                } else {
                    /* The module's two pipe modes agree with each other. */
                    const u32 n = log_stream(&log_module, stream_b);
                    if (memcmp(g, &words_result, sizeof words_result) != 0 || n != stream_length ||
                        memcmp(stream_a, stream_b, n) != 0) {
                        fprintf(stderr, "case %u (%08X): the batched translation differs\n", i, entry);
                        return 1;
                    }
                }
            }
            CPUState reference = words_result;
            reference.ram = native.ram;
            if (memcmp(&native, &reference, sizeof native) != 0 ||
                memcmp(native_ram + (AREA - GC_RAM_BASE), reference_ram + (AREA - GC_RAM_BASE), AREA_BYTES) != 0) {
                fprintf(stderr, "case %u (%08X, seed %08X): mismatch\n", i, entry, seed);
                report_cpu(&native, &reference);
                return 1;
            }
        }
        printf("%08X: %u cases, %u identical (%u with a flush inside), %u declined unchanged, 0 mismatches\n", entry,
               cases, ran[which], flushed_inside[which], declined[which]);
        fflush(stdout);
        if (ran[which] < 30000u && cases >= 60000u)
            return 1; /* at least 30,000 compared cases per function */
    }

    /* ns per call: ordinary matrices, the turn's budget large. */
    if (bench_calls != 0u) {
        module_set_pipe(sink_word);
        module_set_pipe_bytes(sink_bytes);
        bluewake_composite_set_gather_pipe(sink_word);
        bluewake_composite_set_gather_pipe_bytes(sink_bytes);
        CPUState base = build(native_ram, 0, 6).cpu;
        base.gpr[3] = AREA;
        base.exception = 0;
        base.downcount = 0;
        base.cycle_deadline_budget = 0;
        base.cycle_budget = (s64)bench_calls * 64 + 1000;
        memcpy(reference_ram + (AREA - GC_RAM_BASE), native_ram + (AREA - GC_RAM_BASE), AREA_BYTES);
        CPUState* g = guest_cpu();
        *g = base;
        g->ram = reference_ram;
        mod->on_state_loaded(g);
        double t0 = now_ns();
        for (unsigned i = 0; i < bench_calls; ++i) {
            g->pc = EMPTY_FUNCTION;
            mod->dispatch(g, EMPTY_FUNCTION);
        }
        const double empty = (now_ns() - t0) / bench_calls;
        printf("dispatch of a one-instruction function: %.1f ns\n", empty);
        for (unsigned which = 0; which < 3; ++which) {
            const u32 entry = FUNCTIONS[which];
            double best_t = 1e30, best_n = 1e30, best_d = 1e30;
            for (unsigned round = 0; round < 5; ++round) {
                *g = base;
                g->ram = reference_ram;
                mod->on_state_loaded(g);
                t0 = now_ns();
                for (unsigned i = 0; i < bench_calls; ++i) {
                    g->gpr[4] = i & 31u;
                    g->pc = entry;
                    mod->dispatch(g, entry);
                }
                const double t = (now_ns() - t0) / bench_calls;
                CPUState native = base;
                t0 = now_ns();
                for (unsigned i = 0; i < bench_calls; ++i) {
                    native.gpr[4] = i & 31u;
                    native.pc = entry;
                    if (!bluewake_native_fifo(&native, entry))
                        return 1;
                }
                const double n = (now_ns() - t0) / bench_calls;
                /* The dispatcher drains the batch after every call: the same here. */
                native = base;
                t0 = now_ns();
                for (unsigned i = 0; i < bench_calls; ++i) {
                    native.gpr[4] = i & 31u;
                    native.pc = entry;
                    if (!bluewake_native_fifo(&native, entry))
                        return 1;
                    bw_gather_pipe_drain();
                }
                const double d = (now_ns() - t0) / bench_calls;
                if (t < best_t) best_t = t;
                if (n < best_n) best_n = n;
                if (d < best_d) best_d = d;
            }
            printf("%08X: translation %.1f ns/call through the dispatcher (%.1f ns beyond an empty dispatch), "
                   "native %.1f ns/call (%.1f with a drain per call)\n",
                   entry, best_t, best_t - empty, best_n, best_d);
        }
    }
    return 0;
}
