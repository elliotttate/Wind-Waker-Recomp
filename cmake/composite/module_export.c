/* BlueWake composite module export glue (P3).
 * Adapts the ModernGekko module-template export for the DOL+REL composite.
 */
#include "generated_composite.h"
#include "StaticRecompABI.h"
#include "dispatch_loop.h"
#include "native_math.h"
#include "native_skin.h"
#include "native_vec.h"
#include "native_j3d.h"
/* Certified game math entry hooks. */
#include "native_game_math.h"
/* Certified native entries, the second set (scripts/windows/native_entries.py). */
#include "native_fifo.h"
#include "native_bg.h"
#include "native_mtxcalc.h"
#include <stdlib.h>
#include <stdio.h>
static int s_native_math;
int bluewake_inline_gpr_enabled;
#ifdef BLUEWAKE_SIMULATION_PATCHED
#include "simulation_timing.h"
#endif

extern void ppc_set_mem_write_journal(PPCMemWriteJournal fn, void* user);

unsigned dolrecomp_call_depth = 0;
/* The chunk table for the chunks' direct calls (direct_calls.h): calling
 * through it, a call reaches the mods' variants as a dispatch does. */
void (**const bw_chunk_fns)(CPUState*) = s_dolrecomp_chunk_fns;

/* The dispatcher's lookup for the chunks' indirect direct calls
 * (direct_calls.c, bw_call_translated): its pc cache, the mods' variants and
 * the native entries it resolves. */
DolRecompFunction bw_find_chunk(u32 address)
{
    return dolrecomp_find_original(address);
}
static BluewakeEdgeServiceFn s_edge_service;
static void* s_edge_service_user;

#ifdef BLUEWAKE_NATIVE_MATH_CACHED
static void native_matrix_copy(CPUState* cpu) {
    if (!bluewake_native_math(cpu, 0x8030D0C8u)) func_803096E0(cpu);
}
static void native_matrix_concat(CPUState* cpu) {
    if (!bluewake_native_math(cpu, 0x8030D0FCu)) func_803096E0(cpu);
}
static void native_matrix_vec(CPUState* cpu) {
    if (!bluewake_native_math(cpu, 0x8030DA44u)) func_8030D6E0(cpu);
}
static void native_matrix_array(CPUState* cpu) {
    if (!bluewake_native_math(cpu, 0x8030DA98u)) func_8030D6E0(cpu);
}
static DolRecompFunction bluewake_native_math_find(u32 address) {
    if (!s_native_math) return NULL;
    switch (address) {
    case 0x8030D0C8u: return native_matrix_copy;
    case 0x8030D0FCu: return native_matrix_concat;
    case 0x8030DA44u: return native_matrix_vec;
    case 0x8030DA98u: return native_matrix_array;
    default: return NULL;
    }
}
#endif

/* The x86-64-v3 dispatch clones come only from DolRecomp's LLVM object
 * backend; the C backend the Builder uses emits none, so an x86-64 build (the
 * Windows port) dispatches through dolrecomp_call unless the source says it
 * carries them. */
#if defined(__x86_64__) && defined(DOLRECOMP_HAS_X86_64_V3_DISPATCH)
#define BLUEWAKE_X86_64_V3_DISPATCH 1
#else
#define BLUEWAKE_X86_64_V3_DISPATCH 0
#endif

#if BLUEWAKE_X86_64_V3_DISPATCH
static int host_has_x86_64_v3(void)
{
#if defined(__GNUC__) || defined(__clang__)
    static int supported = -1;
    if (supported < 0)
    {
        __builtin_cpu_init();
        supported = __builtin_cpu_supports("avx") &&
            __builtin_cpu_supports("avx2") &&
            __builtin_cpu_supports("fma") &&
            __builtin_cpu_supports("bmi") &&
            __builtin_cpu_supports("bmi2") &&
            __builtin_cpu_supports("movbe") &&
            __builtin_cpu_supports("lzcnt");
    }
    return supported;
#else
    return 0;
#endif
}
#endif

static int selected_dispatch(CPUState* ctx, u32 address)
{
#ifndef BLUEWAKE_NATIVE_MATH_CACHED
    // Compatibility with an older certified source folder. New preparation
    // resolves these entries in the PC cache, avoiding a check at every block.
    if (s_native_math && address >= 0x8030D0C8u && address <= 0x8030DA98u &&
        bluewake_native_math(ctx, address))
        return 1;
#endif
#if BLUEWAKE_X86_64_V3_DISPATCH
    if (host_has_x86_64_v3())
        return dolrecomp_call__x86_64_v3(ctx, address);
#endif
    return dolrecomp_call(ctx, address);
}

/* direct_calls.h: a leaf's native form, where native math is on. */
int bw_native_call(CPUState* cpu, u32 address)
{
    return s_native_math && (bluewake_native_vec(cpu, address) || bluewake_native_math(cpu, address));
}

void dolrecomp_indirect_dispatch(CPUState* ctx, u32 address)
{
    (void)selected_dispatch(ctx, address);
}

static int chassis_dispatch(CPUState* ctx, u32 address)
{
    /* The inlined form, so the loop sees selected_dispatch as a static function
     * rather than as a pointer it must reload and call indirectly at every
     * guest edge. Same body as the exported entry point. */
    return bluewake_chassis_dispatch_loop(
        ctx, address, selected_dispatch, s_edge_service,
        s_edge_service_user);
}

static void chassis_on_state_loaded(CPUState* ctx)
{
    ppc_fpscr_updated(ctx);
#ifdef BLUEWAKE_SIMULATION_PATCHED
    bluewake_simulation_reset();
#endif
}

#include "module_tables.inc"
#include "rel_modules.inc"
typedef struct BlueWakeRelData
{
    u32 module_id;
    u32 section_index;
    u32 linked_start;
    u32 size;
    const u8* bytes;
} BlueWakeRelData;
typedef struct BlueWakeRelLifecycle
{
    u32 module_id;
    u32 prolog_address;
} BlueWakeRelLifecycle;
#include "rel_data.inc"

static const StaticRecompModuleDesc s_desc = {
    STATICRECOMP_ABI_VERSION,
    GXRUNTIME_CPU_ABI_VERSION,
    (u32)sizeof(CPUState),
    MODULE_GAME_ID,
    DOLRECOMP_ENTRY_POINT,
    chassis_dispatch,
    chassis_on_state_loaded,
    s_code_ranges,
    MODULE_CODE_RANGE_COUNT,
    s_smc_ranges,
    MODULE_SMC_RANGE_COUNT,
    s_chunk_ranges,
    MODULE_CHUNK_RANGE_COUNT,
    s_chunk_hashes,
    s_rel_modules,
    MODULE_REL_MODULE_COUNT,
};

#if defined(_WIN32)
#define RECOMP_MODULE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define RECOMP_MODULE_EXPORT __attribute__((visibility("default")))
#else
#define RECOMP_MODULE_EXPORT
#endif

RECOMP_MODULE_EXPORT const StaticRecompModuleDesc* staticrecomp_get_module(void)
{
    static int configured;
    if (!configured) {
        configured = 1;
        const char* native = getenv("BLUEWAKE_NATIVE_MATH");
#ifdef BLUEWAKE_NATIVE_MATH_VERIFIED
        s_native_math = native && strcmp(native, "1") == 0 &&
            strcmp(MODULE_GAME_ID, "GZLE01") == 0;
#else
        s_native_math = 0;
        if (native && strcmp(native, "1") == 0)
            fprintf(stderr, "[native-math] unavailable: certify SDK sources before rebuilding\n");
#endif
#ifdef BLUEWAKE_NATIVE_GPR_VERIFIED
        const char* gpr = getenv("BLUEWAKE_NATIVE_GPR");
        bluewake_inline_gpr_enabled = gpr && strcmp(gpr,"1")==0 &&
            strcmp(MODULE_GAME_ID,"GZLE01")==0 &&
            !getenv("BLUEWAKE_BOUNDARY_CENSUS") && !getenv("BLUEWAKE_RETURN_CENSUS") &&
            !getenv("BLUEWAKE_RETURN_CENSUS_EDGES") && !getenv("BLUEWAKE_CREDIT_CENSUS");
#endif
        if (s_native_math || bluewake_inline_gpr_enabled) atexit(bluewake_native_math_report);
        /* J3DModel::calcWeightEnvelopeMtx and the SDK's vector leaves natively
         * where the builder routed their calls there (scripts/windows:
         * native_skin.py, direct_calls.py), with the other certified natives. */
        bluewake_native_skin_enabled = s_native_math;
#ifdef BLUEWAKE_NATIVE_J3D_VERIFIED
        const char* j3d = getenv("BLUEWAKE_NATIVE_J3D");
        bluewake_native_j3d_enabled = s_native_math && !(j3d && strcmp(j3d, "0") == 0);
        if (bluewake_native_j3d_enabled) {
            atexit(bluewake_native_j3d_report);
            fprintf(stderr, "[native-j3d] certified rotation/translation matrices enabled\n");
        }
#endif
        /* Certified game math entry hooks use the existing native opt-in. */
        bluewake_native_game_math_enabled = s_native_math;
        if (s_native_math) atexit(bluewake_native_game_math_report);
        /* Certified native entries, the second set (scripts/windows/native_entries.py
         * hooks them where their translations are the tested ones): the J3D FIFO
         * matrix loads, two collision checks, PSMTXMultVecSR and the joint
         * matrix calculations, with the other certified natives;
         * BLUEWAKE_NATIVE_ENTRIES=0 leaves them to the translation. */
        {
            const char* entries = getenv("BLUEWAKE_NATIVE_ENTRIES");
            const int on = s_native_math && !(entries && strcmp(entries, "0") == 0);
            bluewake_native_fifo_enabled = on;
            bluewake_native_bg_enabled = on;
            bluewake_native_vec_sr_enabled = on;
            bluewake_native_mtxcalc_enabled = on;
            if (on) {
                atexit(bluewake_native_fifo_report);
                atexit(bluewake_native_bg_report);
                atexit(bluewake_native_vec_sr_report);
                atexit(bluewake_native_mtxcalc_report);
            }
        }
        if (s_native_math) {
            atexit(bluewake_native_skin_report);
            atexit(bluewake_native_vec_report);
        }
        if (s_native_math) fprintf(stderr, "[native-math] bounded GZLE01 matrix leaves enabled\n");
        if (bluewake_inline_gpr_enabled) fprintf(stderr,"[native-gpr] certified caller continuations enabled\n");
    }
    return &s_desc;
}

RECOMP_MODULE_EXPORT const BlueWakeRelData* staticrecomp_get_rel_data(u32* count)
{
    if (count) *count = MODULE_REL_DATA_COUNT;
    return s_rel_data;
}

RECOMP_MODULE_EXPORT const BlueWakeRelLifecycle* staticrecomp_get_rel_lifecycle(u32* count)
{
    if (count) *count = MODULE_REL_LIFECYCLE_COUNT;
    return s_rel_lifecycle;
}

/* Host-only diagnostics need to reach the runtime instance embedded here. */
RECOMP_MODULE_EXPORT void bluewake_set_mem_write_journal(
    PPCMemWriteJournal fn, void* user)
{
    ppc_set_mem_write_journal(fn, user);
}

RECOMP_MODULE_EXPORT void bluewake_set_edge_service(
    BluewakeEdgeServiceFn fn, void* user)
{
    s_edge_service = fn;
    s_edge_service_user = user;
}

/* Mods (scripts/mods/build_mod_variants.py). A mod is compiled in as variant
 * chunks, extra chunks and data writes; the host enables mods once, at boot,
 * before the first dispatch fills the pc cache. The types are declared in
 * generated_composite.h, whose dispatcher consults the extra chunks. */
typedef void (*BlueWakeModWriteFn)(void* user, u32 address, const u8* bytes, u32 size);
#include "mod_variants.inc"
static u32 s_mod_enabled_mask;

static DolRecompFunction bluewake_mod_extra_find(u32 address)
{
#if MODULE_MOD_COUNT > 0
    for (u32 i = 0; i < MODULE_MOD_EXTRA_CHUNK_COUNT; ++i) {
        const BlueWakeModExtraChunk* c = &s_mod_extra_chunks[i];
        if ((s_mod_enabled_mask & (1u << c->mod)) != 0u && c->start <= address &&
            address < c->end && ((address - c->start) & 3u) == 0u)
            return c->fn;
    }
#else
    (void)address;
#endif
    return NULL;
}

RECOMP_MODULE_EXPORT u32 bluewake_composite_mod_count(void)
{
    return MODULE_MOD_COUNT;
}

RECOMP_MODULE_EXPORT const char* bluewake_composite_mod_name(u32 index)
{
#if MODULE_MOD_COUNT > 0
    return index < MODULE_MOD_COUNT ? s_mod_names[index] : NULL;
#else
    (void)index;
    return NULL;
#endif
}

/* Points the chunk table at the enabled mods' variants; returns how many
 * chunks were replaced. A variant applies when every mod it requires is
 * enabled; the table lists combined variants last, so they win. */
RECOMP_MODULE_EXPORT u32 bluewake_composite_apply_mods(u32 mask)
{
    u32 replaced = 0;
    s_mod_enabled_mask = mask;
#if MODULE_MOD_COUNT > 0
    for (u32 i = 0; i < MODULE_MOD_CHUNK_COUNT; ++i) {
        const BlueWakeModChunk* c = &s_mod_chunks[i];
        if ((mask & c->mask) != c->mask)
            continue;
        for (u32 k = 0; k < DOLRECOMP_CHUNK_COUNT; ++k) {
            if (s_dolrecomp_chunk_starts[k] == c->start) {
                s_dolrecomp_chunk_fns[k] = c->fn;
                replaced++;
                break;
            }
        }
    }
#else
    (void)mask;
#endif
    return replaced;
}

/* Hands the enabled mods' data writes to the host, which writes guest RAM:
 * every write at boot (per_frame_only 0), the per-frame ones (a Gecko code's
 * data writes) at every retrace (per_frame_only 1). */
RECOMP_MODULE_EXPORT u32 bluewake_composite_mod_writes(
    u32 mask, u32 per_frame_only, BlueWakeModWriteFn fn, void* user)
{
    u32 count = 0;
#if MODULE_MOD_COUNT > 0
    for (u32 i = 0; i < MODULE_MOD_WRITE_COUNT; ++i) {
        const BlueWakeModWrite* w = &s_mod_writes[i];
        if ((mask & (1u << w->mod)) == 0u)
            continue;
        if (per_frame_only && !w->every_frame)
            continue;
        if (fn)
            fn(user, w->address, w->bytes, w->size);
        count++;
    }
#else
    (void)mask;
    (void)per_frame_only;
    (void)fn;
    (void)user;
#endif
    return count;
}
