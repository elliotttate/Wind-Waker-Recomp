#include "forest_water.h"

#include <stdlib.h>

bool bluewake_forest_water_enabled;
u32 bluewake_forest_water_tree_timer_check;
static bool g_keep_trees, g_thirty_minutes;

void bluewake_forest_water_set_ftree_text(u32 linked_start) {
    // Module 317, section 1: action_waitM_main, immediately after getTimer.
    // The recompiler supplies the stable linked address, independent of the
    // game's changing REL allocation and save-state restoration.
    bluewake_forest_water_tree_timer_check =
        linked_start != 0u ? linked_start + 0x176Cu : 0u;
}

static bool enabled(const char* key) {
    const char* value = getenv(key);
    return value != NULL && value[0] == '1';
}

void bluewake_forest_water_reload(void) {
    g_keep_trees = enabled("BLUEWAKE_FOREST_WATER_KEEP_TREES");
    g_thirty_minutes = enabled("BLUEWAKE_FOREST_WATER_30_MINUTES");
    bluewake_forest_water_enabled = g_keep_trees || g_thirty_minutes;
}

void bluewake_forest_water_enter(CPUState* cpu, u32 address) {
    if (cpu == NULL || cpu->ram == NULL)
        return;
    // GZLE01 revision 0: g_dComIfG_gameInfo, player item record and event flags.
    // Match the caller as well as the function/arguments: unrelated timer resets,
    // normal tree state changes and initializing a new save retain game behavior.
    if (g_keep_trees && bluewake_forest_water_tree_timer_check != 0u &&
        address == bluewake_forest_water_tree_timer_check &&
        (u16)cpu->gpr[3] == 0u) {
        // A watered tree checks the global timer before entering its wilt
        // animation (which also clears its own progress bit). Keep that tree
        // alive without changing the real timer or Link's expiration path.
        cpu->gpr[3] = 1u;
    } else if (g_thirty_minutes && address == 0x8005987Cu &&
        cpu->lr == 0x80153134u && cpu->gpr[3] == 0x803C4C6Eu &&
        (u16)cpu->gpr[4] == 36000u) {
        // procBottleSwing's fresh Forest Water scoop -> resetTimer (30 Hz).
        cpu->gpr[4] = 54000u;
    } else if (g_keep_trees && address == 0x8005CB58u &&
               cpu->lr == 0x801218F0u && cpu->gpr[3] == 0x803C522Cu &&
               (u16)cpu->gpr[4] == 0x9EFFu && (u8)cpu->gpr[5] == 0u) {
        // Link's execute, after the expired-water message: retain the eight
        // watered-tree bits while allowing the bottle/timer/message to expire.
        // The original setEventReg still runs with the retained value.
        cpu->gpr[5] = mem_read8(cpu, 0x803C52CAu);
    }
}
