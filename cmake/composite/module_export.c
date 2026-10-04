/* BlueWake composite module export glue (P3).
 * Adapts the ModernGekko module-template export for the DOL+REL composite.
 */
#include "generated_composite.h"
#include "StaticRecompABI.h"
#include "dispatch_loop.h"

extern void ppc_set_mem_write_journal(PPCMemWriteJournal fn, void* user);

unsigned dolrecomp_call_depth = 0;
static BluewakeEdgeServiceFn s_edge_service;
static void* s_edge_service_user;

static int host_has_x86_64_v3(void)
{
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
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

static int selected_dispatch(CPUState* ctx, u32 address)
{
#if defined(__x86_64__) && defined(BLUEWAKE_HAS_X86_64_V3_DISPATCH)
    if (host_has_x86_64_v3())
        return dolrecomp_call__x86_64_v3(ctx, address);
#endif
    return dolrecomp_call(ctx, address);
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
