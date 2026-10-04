/* The GX SDK's FIFO writers, native: the matrix loads (GXLoadPosMtxImm,
 * GXLoadNrmMtxImm, GXSetCurrentMtx, __GXSetMatrixIndex), the texture loads
 * (GXLoadTexObj, GXLoadTexObjPreLoaded, GXGetTexObjFmt), the TEV, channel,
 * pixel and vertex-format state (GXSetTevColor and the rest of native_gx_gen.inc's
 * list), GXBegin with the dirty state's callees (__GXSetSUTexRegs,
 * __GXUpdateBPMask, __GXSetGenMode, __GXSetVCD, __GXXfVtxSpecs, __GXSetVAT,
 * __GXCalculateVLim) and GXCallDisplayList.
 *
 * Every draw the game makes goes through these: a matrix, a texture, the TEV
 * and vertex state, then GXBegin or GXCallDisplayList. Each writes the
 * gather pipe a byte, halfword or word at a time and keeps a shadow of the
 * GX state in __GXData (r2 - 12848). In the translation every pipe store
 * leaves the RAM fast path for gather_pipe.h's out-of-line store, and every
 * block pays its leader's machinery; together they were about 5 percent of
 * the game thread at Forest Haven, the largest part of it no earlier native
 * covered.
 *
 * Each native here is its function's translation itself, replayed on local
 * registers: scripts/windows/native_gx_gen.py took the instructions' C from
 * the blocks' prepaid copies (native_gx_gen.inc), and native_gx_run.h runs
 * it - RAM stores in place with their old bytes kept, so a decline puts them
 * back; the pipe's bytes collected and appended to the module's batch at the
 * end (the batch handed over first where the bytes would take it past its
 * flush point: the same stream). Callees a native stands in for are replayed
 * with it: GXLoadTexObj's region callback (__GXDefaultTexRegionCallback, with
 * GXGetTexObjFmt) and GXLoadTexObjPreLoaded with its TLUT callback
 * (__GXDefaultTlutRegionCallback); __GXSetSUTexRegs's __SetSURegs; __GXSetVCD's
 * __GXXfVtxSpecs; GXSetTexCoordGen2's and GXSetCurrentMtx's __GXSetMatrixIndex.
 * GXBegin replays only its own blocks: where its dirty state needs a callee,
 * it declines, and the translation makes the calls - each callee native at
 * its own hook.
 *
 * A native run leaves what the translation leaves: every register as the
 * last instruction to write it left it (both halves of the FPRs the paired
 * loads wrote), CR, XER, LR, CTR, the FPSCR, the reservation (cleared by a
 * store to its granule), the cycles block by block, the last access's cycle
 * suffix (a pipe store's, an FP or paired access's, mflr and mtlr's, the
 * inline register save's, a RAM store's where a reservation was held), every
 * byte of RAM, pc at the return address - and the gather pipe the same bytes
 * in the same order through the batch entry point.
 *
 * It declines, changing nothing, unless that is certain: no exception
 * pending, no write journal, no alias over MEM1, both of the host's pipe
 * writers set (the natives run only where the module batches the pipe), FP
 * available and the paired singles unscaled where the function uses them,
 * every block prepaid and none stopped for the budget, every load plain RAM
 * and every store plain RAM or the pipe, a call through a pointer reaching
 * the function the native replays (the SDK's default callbacks), a jump
 * table's target a block of the function, and every boundary between chunks
 * on its path passing without the host (the edge filter's own test). A path
 * into anything not replayed here - __GXSetDirtyState from GXCallDisplayList
 * (a watched address: the host is asked there), __GXSendFlushPrim, GXBegin's
 * dirty-state calls - declines.
 *
 * tests/native_gx_test.c compares each with the translation: every register,
 * RAM byte and byte the pipe hands the host. No identifier here may be
 * `ctx`. */
#include "native_gx.h"
#include "native_gx_run.h"

#include <stdio.h>

int bluewake_native_gx_enabled;

/* A run's undo log and pipe bytes (the game thread's: the natives run only
 * there, and none runs inside another). Static, so a native's frame stays
 * small. */
static GxLog s_gx_log;

/* The interpreter's FP functions run on this, holding a run's FPSCR. */
CPUState g_gx_fp_scratch;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-label"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#include "native_gx_gen.inc"
#pragma clang diagnostic pop

enum {
#define GX_INDEX(entry, fn, name, id) GXI_##id,
    GX_NATIVES(GX_INDEX)
#undef GX_INDEX
    GX_COUNT
};

static const char* const k_gx_names[GX_COUNT] = {
#define GX_NAME(entry, fn, name, id) name,
    GX_NATIVES(GX_NAME)
#undef GX_NAME
};

static unsigned long long s_gx_runs[GX_COUNT], s_gx_declined[GX_COUNT];

void bluewake_native_gx_report(void) {
    fprintf(stderr, "[native-gx]");
    for (unsigned i = 0; i < GX_COUNT; ++i)
        fprintf(stderr, " %s=%llu/%llu", k_gx_names[i], s_gx_runs[i], s_gx_declined[i]);
    fprintf(stderr, " (native/declined)\n");
}

int bluewake_native_gx(CPUState* cpu, u32 address) {
    unsigned which;
    int done;
    switch (address) {
#define GX_CASE(entry, fn, name, id) \
    case entry:                      \
        which = GXI_##id;            \
        done = fn(cpu, entry);       \
        break;
        GX_NATIVES(GX_CASE)
#undef GX_CASE
    default:
        return 0;
    }
    if (done)
        s_gx_runs[which]++;
    else
        s_gx_declined[which]++;
    return done;
}
