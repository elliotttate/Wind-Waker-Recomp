/* The fifth set's comparison harness (tests/native_gx_test.c): the fourth
 * set's (native4_harness.h) - a Windows game module (gGZLE01_recomp.dll)
 * loaded read-only, its translation run from a function's entry as in play
 * (direct calls and the edge filter on, the host's flags quiet, an edge
 * service that ends the run at the return address and counts any other
 * question), two RAM images whose every page is read-only but the test's
 * own - with the gather pipe added: what the module's translation hands the
 * host and what the native hands it, compared byte for byte and call for
 * call.
 *
 * The pipe. The module's own pipe (its gather_pipe.c) is set to hand every
 * store to the test one at a time (bluewake_composite_set_gather_pipe, no
 * batch): that is the translation's stream of stores, in order, each with
 * its size. The native runs on the test's own pipe (gather_pipe.c, linked
 * in) in one of its host modes: a batch partly filled with random bytes (so
 * the function's bytes fit, reach the flush point exactly, or cross it), a
 * word at a time, or no writer at all. The expected result is the
 * translation's stores put through the test's pipe in the same mode from the
 * same state (bw_gather_pipe_put, one by one, exactly as the translation's
 * out-of-line store puts them): every call the pipe makes to the host - a
 * word, or a run of bytes when the batch fills - and, at the end, the batch's
 * contents must match.
 *
 * Each test hands harness_load its writable regions. */
#ifndef BLUEWAKE_NATIVE5_HARNESS_H
#define BLUEWAKE_NATIVE5_HARNESS_H

#include "direct_calls.h"
#include "gather_pipe.h"
#include "StaticRecompABI.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <xmmintrin.h>

#ifndef NATIVE5_HOOKED
/* direct_calls.c, linked in for the edge filter's state the natives read,
 * names these. */
BwChunkFn bw_find_chunk(u32 address) {
    (void)address;
    return NULL;
}
BwChunkFn* const bw_chunk_fns = NULL;
#endif

/* gather_pipe.c, linked in: the test's own pipe, the one the native uses. */
void bluewake_composite_set_gather_pipe(BwGatherPipeWrite write);
void bluewake_composite_set_gather_pipe_bytes(BwGatherPipeBytes bytes);

#define RETURN_ADDRESS 0xFFFFFFFCu

typedef struct Region {
    u32 start, bytes;
} Region;

static const Region* REGIONS; /* the test's writable pages */
static unsigned REGION_COUNT;

/* --- Random numbers. ----------------------------------------------------- */

static u32 seed = 0x5E7A11C3u;
static u32 next(void) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}
static u32 below(u32 n) { return next() % n; }

static void put32(u8* ram, u32 address, u32 value) { write_be32(ram + (address - GC_RAM_BASE), value); }
static void put8(u8* ram, u32 address, u8 value) { ram[address - GC_RAM_BASE] = value; }

/* A single's bits: mostly ordinary, sometimes a zero, a denormal, a huge
 * value, an infinity or a NaN. */
static u32 any_single(unsigned specials_in_64) {
    const u32 sign = next() & 0x80000000u;
    if (below(64u) < specials_in_64) {
        switch (below(6u)) {
        case 0: return sign;
        case 1: return sign | (1u + below(0x007FFFFFu));
        case 2: return sign | 0x7F800000u;
        case 3: return sign | 0x7FC00000u | (next() & 0x003FFFFFu);
        case 4: return sign | 0x7F800000u | (1u + below(0x003FFFFFu));
        default: return sign | ((200u + below(54u)) << 23) | (next() & 0x007FFFFFu);
        }
    }
    return sign | ((110u + below(30u)) << 23) | (next() & 0x007FFFFFu);
}

/* --- Random machine state. ----------------------------------------------- */

static void random_cpu(CPUState* c, u8* ram) {
    memset(c, 0, sizeof *c);
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
    c->msr = PPC_MSR_FP | (next() & ~(PPC_MSR_FP | PPC_MSR_EE));
    c->hid2 = PPC_HID2_LSQE | (next() & 0x0FFFFFFFu);
    for (unsigned g = 1; g < 8; ++g)
        c->gqr[g] = next();
    c->gqr[0] = 0u;
    c->fpscr = next() & ~(0x60000003u | 0x4u);
    if (below(4u) == 0u)
        c->fpscr |= 0x4u;
    ppc_fpscr_updated(c);
    c->reserve_valid = below(2u) != 0u;
    c->reserve_addr = next();
    c->cycle_observation_suffix = next();
    c->cycle_budget = 16384;
    c->downcount = -(s64)below(64u);
    c->cycle_deadline_budget = below(4u) == 0u ? 0 : 100000;
}

/* The cycle state at the edges: `cycles` is the function's shortest path. */
static void clock_edges(CPUState* c, unsigned scenario, s64 cycles) {
    switch (scenario % 43u) {
    case 1: c->cycle_deadline_budget = cycles - c->downcount + (s64)below(5u) - 2; break; /* near the end */
    case 2: c->cycle_deadline_budget = 1 + (s64)below((u32)(cycles > 1 ? cycles : 2)); break; /* inside */
    case 3: c->cycle_deadline_budget = -(s64)below(100u); break;                            /* none */
    case 4: c->downcount = -c->cycle_budget; break;                                        /* spent */
    case 5: c->downcount = -c->cycle_budget + 1 + (s64)below((u32)(cycles > 1 ? cycles : 2)); break; /* inside */
    case 6: c->cycle_budget = 1 + (s64)below(60u); c->downcount = 0; break;
    case 7: c->downcount = (s64)below(8u); break;                                           /* positive */
    case 8: c->exception = 1u; break;
    case 9: c->cycle_budget = 0; break;
    case 10: c->cycle_deadline_budget = (s64)below(400u); break;
    case 11: c->downcount = -c->cycle_budget + (s64)below(400u); break;
    default: break;
    }
}

/* --- The pipe logs. ------------------------------------------------------ */

/* What a pipe hands the host, in order: 'W' size value for a word to the
 * writer, 'B' length bytes for a run of bytes. */
typedef struct PipeLog {
    u8 data[1u << 20];
    u32 length;
} PipeLog;
static PipeLog log_module, log_native, log_model;
static PipeLog* log_target = &log_native;

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
static void native_word(u64 value, u8 size) { log_word(log_target, value, size); }
static void native_bytes(const u8* bytes, u32 size) { log_bytes(log_target, bytes, size); }
static void sink_word(u64 value, u8 size) { (void)value; (void)size; }
static void sink_bytes(const u8* bytes, u32 size) { (void)bytes; (void)size; }

enum { PIPE_BATCH, PIPE_WORDS, PIPE_NONE };

static void set_test_pipe(unsigned mode) {
    bluewake_composite_set_gather_pipe(mode == PIPE_NONE ? NULL : native_word);
    bluewake_composite_set_gather_pipe_bytes(mode == PIPE_BATCH ? native_bytes : NULL);
}

static void prefill_test_pipe(u32 count) {
    for (u32 i = 0; i < count; ++i)
        bw_gather_pipe_buffer[i] = (u8)(i * 37u + 11u);
    bw_gather_pipe_length = count;
}

/* The bytes a log's entries carry, in order (a word's big-endian). */
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

/* The same stream of bytes handed to the host: the natives hand a run of
 * bytes to the batch writer in pieces that may differ from the
 * translation's (gather_pipe_batch.h: a run of bytes through the batch entry
 * point is the same stream). */
static bool same_logs(const PipeLog* a, const PipeLog* b) {
    static u8 sa[1u << 20], sb[1u << 20];
    const u32 na = log_stream(a, sa), nb = log_stream(b, sb);
    return na == nb && memcmp(sa, sb, na) == 0;
}

static u32 log_words(const PipeLog* log) { return log->length / 10u; }

static void copy_log(PipeLog* to, const PipeLog* from) {
    to->length = from->length;
    memcpy(to->data, from->data, from->length);
}

/* --- The module. --------------------------------------------------------- */

typedef void (*SetPipeFn)(BwGatherPipeWrite);
typedef void (*SetPipeBytesFn)(BwGatherPipeBytes);

typedef struct Harness {
    HMODULE lib;
    const StaticRecompModuleDesc* mod;
    CPUState* (*guest_cpu)(void);
    SetPipeFn set_pipe;
    SetPipeBytesFn set_pipe_bytes;
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
    h->set_pipe = (SetPipeFn)(void*)GetProcAddress(h->lib, "bluewake_composite_set_gather_pipe");
    h->set_pipe_bytes = (SetPipeBytesFn)(void*)GetProcAddress(h->lib, "bluewake_composite_set_gather_pipe_bytes");
    if (get == NULL || h->guest_cpu == NULL || set_edge == NULL || direct == NULL || filter == NULL ||
        h->set_pipe == NULL || h->set_pipe_bytes == NULL) {
        fprintf(stderr, "not a BlueWake Windows module\n");
        return 0;
    }
    h->mod = get();
    if (h->mod->cpu_state_size != sizeof(CPUState) || strcmp(h->mod->game_id, "GZLE01") != 0) {
        fprintf(stderr, "CPU state size %u, expected %u\n", h->mod->cpu_state_size, (unsigned)sizeof(CPUState));
        return 0;
    }
    /* The module reads the test's own host flags, so a case that makes the
     * host busy makes it busy for the translation too. */
    if (!direct(true, &s_sources_dirty, &s_decrementer_pending, &s_pi_cause, &s_pi_mask) || !filter(true)) {
        fprintf(stderr, "the module's direct calls or edge filter are unavailable\n");
        return 0;
    }
    set_edge(edge_service, NULL);
    /* The translation's stores, one by one. */
    h->set_pipe(module_word);
    h->set_pipe_bytes(NULL);
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

static void report_logs(const PipeLog* got, const PipeLog* want) {
    fprintf(stderr, "  pipe: got %u bytes of log, want %u\n", got->length, want->length);
    const u32 n = got->length < want->length ? got->length : want->length;
    for (u32 i = 0; i < n; ++i)
        if (got->data[i] != want->data[i]) {
            fprintf(stderr, "  first difference at log byte %u\n", i);
            break;
        }
}

/* The host's floating-point mode as the module arms it for the guest's FPSCR
 * (NI gives FTZ and DAZ). In a case, sometimes. */
static unsigned s_mxcsr_default;
static void host_fp_mode(bool flush) {
    if (s_mxcsr_default == 0u)
        s_mxcsr_default = _mm_getcsr();
    _mm_setcsr(flush ? (s_mxcsr_default | 0x8040u) : s_mxcsr_default);
}

/* The translation from `entry` on the module's own CPU state and RAM image
 * (its regions as `before`), its pipe stores in log_module: its state at the
 * end, with the native's RAM pointer. */
static CPUState harness_translate(Harness* h, const CPUState* start, u32 entry) {
    copy_regions(h->reference_ram, h->before);
    CPUState* g = h->guest_cpu();
    *g = *start;
    g->ram = h->reference_ram;
    h->mod->on_state_loaded(g);
    s_unexpected_service = 0;
    log_module.length = 0;
    if (!h->mod->dispatch(g, entry)) {
        fprintf(stderr, "the translation did not run at %08X\n", entry);
        exit(1);
    }
    CPUState reference = *g;
    reference.ram = start->ram;
    return reference;
}

/* The translation's stores through the test's pipe in `mode` from the state
 * the case started it in: log_model. */
static void pipe_model(unsigned mode, u32 prefill) {
    set_test_pipe(mode);
    if (mode == PIPE_BATCH)
        prefill_test_pipe(prefill);
    log_target = &log_model;
    log_model.length = 0;
    for (u32 j = 0; j < log_module.length; j += 10u) {
        u64 value;
        memcpy(&value, log_module.data + j + 2, 8);
        bw_gather_pipe_put(value, log_module.data[j + 1]);
    }
    bw_gather_pipe_flush();
    log_target = &log_native;
}

static double now_ns(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1e9 / (double)freq.QuadPart;
}

/* A case's pipe: the mode, and the batch's bytes before it. */
typedef struct PipeCase {
    unsigned mode;
    u32 prefill;
} PipeCase;

static PipeCase random_pipe(unsigned scenario) {
    PipeCase p;
    p.mode = scenario % 23u == 4u ? PIPE_NONE : scenario % 19u == 1u ? PIPE_WORDS : PIPE_BATCH;
    p.prefill = scenario % 7u == 0u ? 0u : below(BW_GATHER_PIPE_BATCH);
    return p;
}

static unsigned long long s_flushed_inside;

#ifdef NATIVE5_HOOKED
static void hooked_case(const Harness* h, u32 entry, const char* name, unsigned index, const CPUState* start,
                        const CPUState* reference, const PipeCase* pipe, bool flush, bool declined);
#endif

/* One case: the native on `start` (in the native image, regions saved in
 * `before`), then - where it ran - the translation, and every byte compared,
 * the pipe's included. Returns 1 identical, 0 declined unchanged; a mismatch
 * exits. */
typedef int (*NativeFn)(CPUState* cpu, u32 address);
static int harness_case(Harness* h, NativeFn native_fn, u32 entry, const char* name, unsigned index,
                        const CPUState* start, const PipeCase* pipe, bool must_decline, bool with_journal,
                        bool with_aliases, bool flush) {
    copy_regions(h->before, h->native_ram);
    set_test_pipe(pipe->mode);
    if (pipe->mode == PIPE_BATCH)
        prefill_test_pipe(pipe->prefill);
    else
        bw_gather_pipe_length = 0;
    u8 pipe_before[BW_GATHER_PIPE_BATCH + 8u];
    memcpy(pipe_before, bw_gather_pipe_buffer, sizeof pipe_before);
    const u32 length_before = bw_gather_pipe_length;
    log_native.length = 0;
    log_target = &log_native;
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
    host_fp_mode(false);
    if (!accepted) {
        if (memcmp(&native, &untouched, sizeof native) != 0 || !same_regions(h->before, h->native_ram) ||
            log_native.length != 0u || bw_gather_pipe_length != length_before ||
            memcmp(pipe_before, bw_gather_pipe_buffer, sizeof pipe_before) != 0) {
            fprintf(stderr, "case %u (%s): declined but changed state\n", index, name);
            report_cpu(&native, &untouched);
            exit(1);
        }
        return 0;
    }
    if (must_decline || pipe->mode != PIPE_BATCH) {
        fprintf(stderr, "case %u (%s): ran where it must decline\n", index, name);
        exit(1);
    }
    if (pipe->mode == PIPE_BATCH && log_native.length != 0u)
        s_flushed_inside++;
    bw_gather_pipe_flush();
    host_fp_mode(flush);
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
    static PipeLog native_log;
    copy_log(&native_log, &log_native);
    pipe_model(pipe->mode, pipe->prefill);
    if (!same_logs(&native_log, &log_model)) {
        fprintf(stderr, "case %u (%s, seed %08X, mode %u, prefill %u): the pipe differs (%u translation stores)\n",
                index, name, seed, pipe->mode, pipe->prefill, log_words(&log_module));
        report_logs(&native_log, &log_model);
        exit(1);
    }
#ifdef NATIVE5_HOOKED
    hooked_case(h, entry, name, index, start, &reference, pipe, flush, false);
#endif
    return 1;
}

#endif
