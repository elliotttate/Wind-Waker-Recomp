/* The particle draw code and the sea's waves, native, with stops: the sixth
 * set (native_draw_list.h has the entries and resumes).
 *
 * JPA draws every particle on the CPU: the draw visitors (JPADrawVisitor.cpp)
 * compute each particle's corners and write them to the gather pipe, the calc
 * visitors move its scale, colour and alpha along. Together with the sea's
 * waves (d_kankyo_rain.cpp drawWave) they were the largest part of the game
 * thread no earlier set covered: about 4 percent at Forest Haven for the
 * particles, 0.9 for the waves.
 *
 * Each native is its function's translation itself, replayed on local
 * registers as the fifth set's are (scripts/windows/native_draw_gen.py, with
 * native_gx_gen.py's generator; native_gx_run.h runs it): RAM stores in place
 * with their old bytes kept, the pipe's bytes appended to the module's batch
 * at the end, the paired-single arithmetic and the fused multiply-adds as
 * inline_fp.h's inline paths compute them. Callees it knows are replayed with
 * it: the vtable getters of JPABaseShapeArc and JPAExtraShapeArc, the
 * direction and list-walk functions behind the draw clipboard's pointers,
 * PSMTXMultVec, GXSetTevColor, and GXBegin's own blocks (where its dirty
 * state needs calls - a draw's first after state changes - the native
 * declines and the translation makes them, the fifth set's natives at their
 * hooks). drawWave's native starts at its loop's head (8009A094), not its
 * entry, and replays sin with fdlibm's kernels, GXLoadTexObj, GXSetTevKColor
 * and __register_global_object; GXBegin it leaves to the translation (the
 * waves load a texture before every quad, so its dirty state always needs
 * calls): each wave stops before it and resumes after it.
 *
 * Stops. Where the path reaches code the native does not replay - a call to
 * a function it does not know, a call or return across a boundary the host
 * would be asked about (a watched address, the host busy), the turn's budget
 * spent at a call - and that instruction is in the hook's own chunk, the run
 * ends just before it: every register, the cycles (the block prepaid, as the
 * translation's prepaid copy has charged it), the suffix, RAM and the pipe's
 * bytes committed, pc that instruction. The hook sets cycle_block_prepaid and
 * goes on with the translation at that instruction through the chunk's pc
 * table: the translation makes the call exactly as it would have, with the
 * host's edge service where it asks it (the particle draw tags of
 * runtime/host/src/draw_tags.c included, after every byte the native wrote).
 * The call returns into the translation, and a hook at its return address (a
 * resume) runs the rest natively. A call that is its block's first
 * instruction stops before the block is entered (its cycles given back): the
 * translation enters it itself. Anywhere else (in a callee's chunk) the
 * native declines instead.
 *
 * Checkpoints. At a loop's head in the hook's chunk, a run whose undo log or
 * pipe bytes are past half full commits what it has, as a stop there would,
 * and goes on with both empty; a later decline is then a stop at that head
 * (native_gx_run.h's gx_decline), so a long strip never runs out of log.
 *
 * A native that completes leaves what the translation leaves at the
 * function's blr, pc the return address; one that stops leaves what the
 * translation has just before the stop's instruction. It declines, changing
 * nothing, on the conditions native_gx.c lists: an exception pending, a
 * write journal, an alias over MEM1, the pipe not batched, FP unavailable or
 * not rounding to nearest where it does FP arithmetic, paired singles
 * scaled, a block not prepaid or the budget spent at a leader it enters, a
 * load or store that is not plain RAM or the pipe, an operand the
 * translation would hand the interpreter.
 *
 * tests/native_draw_test.c compares each with the translation, end to end
 * through the hooked chunks (stops and resumes included). No identifier here
 * may be `ctx`. */
#include "native_draw.h"
#include "native_gx_run.h"

#include <stdio.h>

int bluewake_native_draw_enabled;

/* A run's undo log and pipe bytes (the game thread's; no native runs inside
 * another). */
static GxLog s_gx_log;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-label"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#include "native_draw_gen.inc"
#pragma clang diagnostic pop

enum {
#define DRAW_INDEX(entry, fn, name, id) DRI_##id,
    DRAW_NATIVES(DRAW_INDEX)
#undef DRAW_INDEX
    DRAW_COUNT
};

static const char* const k_draw_names[DRAW_COUNT] = {
#define DRAW_NAME(entry, fn, name, id) name,
    DRAW_NATIVES(DRAW_NAME)
#undef DRAW_NAME
};

/* [0] declined, [1] done, [2] stopped */
static unsigned long long s_draw_counts[DRAW_COUNT][3];
/* Calls left to the translation by the decline gate (native_replay.h). */
static unsigned long long s_draw_skipped[DRAW_COUNT];
static NativeGate s_draw_gates[DRAW_COUNT];

void bluewake_native_draw_report(void) {
    fprintf(stderr, "[native-draw]");
    for (unsigned i = 0; i < DRAW_COUNT; ++i)
        if (s_draw_counts[i][0] + s_draw_counts[i][1] + s_draw_counts[i][2] != 0u)
            fprintf(stderr, " %s=%llu/%llu/%llu/%llu", k_draw_names[i], s_draw_counts[i][1], s_draw_counts[i][2],
                    s_draw_counts[i][0], s_draw_skipped[i]);
    fprintf(stderr, " (done/stopped/declined/left to the translation; entries and resumes that ran)\n");
}

/* The counts over every entry and resume (for tests/native_draw_test.c). */
void bluewake_native_draw_totals(unsigned long long totals[3]) {
    totals[0] = totals[1] = totals[2] = 0u;
    for (unsigned i = 0; i < DRAW_COUNT; ++i)
        for (unsigned r = 0; r < 3u; ++r)
            totals[r] += s_draw_counts[i][r];
}

#define DRAW_ENTRY(entry, fn, name, id)            \
    int bluewake_native_draw_##id(CPUState* cpu) { \
        NativeGate* gate = &s_draw_gates[DRI_##id]; \
        if (native_gate_skips(gate)) {             \
            s_draw_skipped[DRI_##id]++;            \
            return 0;                              \
        }                                          \
        const int r = fn(cpu, entry);              \
        s_draw_counts[DRI_##id][r]++;              \
        native_gate_note(gate, r == 0);            \
        return r;                                  \
    }
DRAW_NATIVES(DRAW_ENTRY)
#undef DRAW_ENTRY

int bluewake_native_draw(CPUState* cpu, u32 address) {
    switch (address) {
#define DRAW_CASE(entry, fn, name, id) \
    case entry:                        \
        return bluewake_native_draw_##id(cpu);
        DRAW_NATIVES(DRAW_CASE)
#undef DRAW_CASE
    default:
        return 0;
    }
}
