/* One group of the seventh set's natives (native_libm.c, native_bgblk.c,
 * native_rot.c, native_calc.c, native_geom.c, native_jas.c): each native's own entry point - what its
 * hook calls - with the decline gate, the counts by reason, the report, and
 * the entry by address the tests call. The group's file defines, before
 * including this after its natives (native_<group>_gen.inc):
 *
 *   LF_NATIVES(X)    its list (native_<group>_list.h: X(entry, native, name, digits))
 *   LF_TAG           its report's tag, "[native-<group>]"
 *   LF_ENTRY(id)     its natives' entry points, bluewake_native_<group>_##id
 *   LF_REPORT        bluewake_native_<group>_report
 *   LF_BY_ADDRESS    bluewake_native_<group>
 *
 * No identifier here may be `ctx`. */

enum {
#define LF_INDEX(entry, fn, name, id) LFI_##id,
    LF_NATIVES(LF_INDEX)
#undef LF_INDEX
    LF_COUNT
};

static const char* const k_lf_names[LF_COUNT] = {
#define LF_NAME(entry, fn, name, id) name,
    LF_NATIVES(LF_NAME)
#undef LF_NAME
};

static unsigned long long s_lf_runs[LF_COUNT], s_lf_declined[LF_COUNT][LF_WHY_COUNT], s_lf_skipped[LF_COUNT];

/* An entry that keeps declining stops being tried for a while, as the fifth
 * set's do (native_replay.h's gate): a native that declines has replayed its
 * function as far as it got, and the translation then does all of it again.
 * None of these is expected to decline often - leaf code, with nothing to
 * stop at but the clock - so the gate is a guard, not a filter.
 * bluewake_native_gate_enabled = 0 tries every call (the tests). */
static NativeGate s_lf_gates[LF_COUNT];

void LF_REPORT(void) {
    static const char* const why[LF_WHY_COUNT] = {"state", "clock", "memory", "fp", "host", "path"};
    fprintf(stderr, LF_TAG);
    for (unsigned i = 0; i < LF_COUNT; ++i) {
        unsigned long long declined = 0;
        for (unsigned w = 0; w < LF_WHY_COUNT; ++w)
            declined += s_lf_declined[i][w];
        fprintf(stderr, " %s=%llu/%llu/%llu", k_lf_names[i], s_lf_runs[i], declined, s_lf_skipped[i]);
        if (declined != 0u) {
            fprintf(stderr, "(");
            const char* sep = "";
            for (unsigned w = 0; w < LF_WHY_COUNT; ++w)
                if (s_lf_declined[i][w] != 0u) {
                    fprintf(stderr, "%s%s %llu", sep, why[w], s_lf_declined[i][w]);
                    sep = ", ";
                }
            fprintf(stderr, ")");
        }
    }
    fprintf(stderr, " (native/declined/left to the translation)\n");
}

#define LF_WRAP(entry, fn, name, id)                     \
    int LF_ENTRY(id)(CPUState * cpu) {                   \
        NativeGate* gate = &s_lf_gates[LFI_##id];        \
        if (native_gate_skips(gate)) {                   \
            s_lf_skipped[LFI_##id]++;                    \
            return 0;                                    \
        }                                                \
        const int done = fn(cpu, entry);                 \
        if (done)                                        \
            s_lf_runs[LFI_##id]++;                       \
        else                                             \
            s_lf_declined[LFI_##id][lf_why]++;           \
        native_gate_note(gate, !done);                   \
        return done;                                     \
    }
LF_NATIVES(LF_WRAP)
#undef LF_WRAP

int LF_BY_ADDRESS(CPUState* cpu, u32 address) {
    switch (address) {
#define LF_CASE(entry, fn, name, id) \
    case entry:                      \
        return LF_ENTRY(id)(cpu);
        LF_NATIVES(LF_CASE)
#undef LF_CASE
    default:
        return 0;
    }
}
