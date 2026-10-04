#ifndef BLUEWAKE_SETTINGS_MENU_H
#define BLUEWAKE_SETTINGS_MENU_H

#include <stdbool.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// The options menu (the Mac host): Esc (once the mouse is given back), F1 or a
// controller's Back button (as set under Controllers; Start and Back held for
// a second always) opens it over the paused game, and so does player 1's
// controller disconnecting (gamepads.h). Its choices are the
// settings the host reads from the environment (BLUEWAKE_*, DOL_AURORA_*),
// saved in a settings file and applied at the next launch, and most of them
// at once.
//
//   BLUEWAKE_SETTINGS=path|none   the settings file (default ~/Library/
//                                 Application Support/Wind Waker Recomp/
//                                 settings.ini); none: neither read nor written

#if defined(__APPLE__) && TARGET_OS_IPHONE
// The iOS app has its own menu.
static inline void bluewake_settings_load(void) {}
static inline void bluewake_settings_menu_install(void) {}
static inline bool bluewake_settings_menu_event(const void* sdl_event) {
    (void)sdl_event;
    return false;
}
#else
// First thing in main: the saved settings go into the environment, over what
// the launch gave (the menu's choices win).
void bluewake_settings_load(void);
// Once Aurora is up: the menu's overlay and the pause.
void bluewake_settings_menu_install(void);
// Every SDL event: true when the menu took it (its keys, or anything while it
// is open), so nothing else acts on it.
bool bluewake_settings_menu_event(const void* sdl_event);
#endif

#ifdef __cplusplus
}
#endif

#endif
