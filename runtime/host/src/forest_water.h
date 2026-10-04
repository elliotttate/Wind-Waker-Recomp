#ifndef BLUEWAKE_FOREST_WATER_H
#define BLUEWAKE_FOREST_WATER_H

#include "core/cpu.h"
#include <stdbool.h>

// Independent, opt-in Forest Water assistance. Reloaded by the settings menu;
// progress stays in the game's own save data, so save states need no host cache.
// BLUEWAKE_FOREST_WATER_KEEP_TREES=1 preserves watering progress on expiry.
// BLUEWAKE_FOREST_WATER_30_MINUTES=1 extends the next fresh scoop to 30 minutes.
void bluewake_forest_water_reload(void);
void bluewake_forest_water_set_ftree_text(u32 linked_start);
extern bool bluewake_forest_water_enabled;
extern u32 bluewake_forest_water_tree_timer_check;
void bluewake_forest_water_enter(CPUState* cpu, u32 address);

// Only argument registers change, before the original translated function.
// As with the camera/door hooks, this does not end a chassis dispatch window.
static inline void bluewake_forest_water_dispatch(CPUState* cpu, u32 address) {
    if (__builtin_expect(bluewake_forest_water_enabled, 0) &&
        (address == 0x8005987Cu || address == 0x8005CB58u ||
         address == bluewake_forest_water_tree_timer_check))
        bluewake_forest_water_enter(cpu, address);
}

#endif
