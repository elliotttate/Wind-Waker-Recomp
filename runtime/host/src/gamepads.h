#ifndef BLUEWAKE_GAMEPADS_H
#define BLUEWAKE_GAMEPADS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The connected controllers and which of them is player 1 (the GameCube's
// port 0), as they come and go while the game runs.
//
// Aurora opens each controller as it connects and reads port 0 from the one
// SDL gives player index 0; it never picks one itself, so a controller SDL
// leaves without an index never reaches the game. The registry gives port 0
// a controller by its policy:
//
//   BLUEWAKE_PAD_PORT0=auto|last|fixed    (default auto)
//     auto   port 0 empty: the controller used last (or connected last) takes it
//     last   as auto, and a press on another controller moves player 1 to it
//     fixed  Aurora's saved choice only (the menu's "Player 1" choice)
//   BLUEWAKE_PAD_PAUSE_ON_DISCONNECT=1|0  (default 1) the options menu opens
//                                         when player 1's controller goes
//
// Only the SDL gamepad API is used here (the port is SDL's player index, as
// Aurora's), so the policy runs without Aurora in tests.

typedef enum BWPort0Policy {
    BW_PORT0_AUTO = 0,
    BW_PORT0_LAST,
    BW_PORT0_FIXED,
} BWPort0Policy;

typedef struct BWGamepad {
    unsigned id;          // SDL_JoystickID
    int port;             // 0-3, or -1
    int type;             // SDL_GamepadType
    unsigned short vendor, product;
    bool gamecube;        // a GameCube controller (adapter, or Nintendo Switch Online)
    char name[96];
    unsigned long long connected_ms, last_input_ms;
} BWGamepad;

struct SDL_Gamepad;

void bluewake_gamepads_reload(void);
// Every SDL event, before anything else may take it (also while the options
// menu is open). Aurora has already opened or closed the controller.
void bluewake_gamepads_event(const void* sdl_event);
// Once a frame: controllers connected before the events reached the host,
// and the policy.
void bluewake_gamepads_sync(void);

int bluewake_gamepads_count(void);
const BWGamepad* bluewake_gamepads_at(int index);
const BWGamepad* bluewake_gamepads_find(unsigned id);
// Player `port`'s controller, from SDL itself (needs no registry: the iOS app
// and tests use it too); NULL when no controller has that port.
struct SDL_Gamepad* bluewake_gamepads_for_port(unsigned port);
// The controller player 1 uses, else any connected one (the host's own
// buttons keep working while no controller has port 0).
struct SDL_Gamepad* bluewake_gamepads_player_pad(void);

BWPort0Policy bluewake_gamepads_policy(void);
// The menu's choice of player 1 for this session (auto and last); fixed goes
// through Aurora's saved choice (see bluewake_gamepads_set_hooks).
void bluewake_gamepads_use_for_port0(unsigned id);

// True once after player 1's controller disconnected (pause on disconnect on).
bool bluewake_gamepads_take_port0_lost(void);
// The oldest notice not yet shown ("Xbox Wireless Controller connected:
// player 1"); false when none.
bool bluewake_gamepads_take_notice(char* text, size_t size);

// Aurora's side (pad_remap.cpp): `handover` runs when player 1 changes hands
// (held buttons must not carry over); `persist` saves the fixed choice.
typedef struct BWGamepadHooks {
    void (*handover)(void);
    void (*persist)(unsigned id);
} BWGamepadHooks;
void bluewake_gamepads_set_hooks(const BWGamepadHooks* hooks);

#ifdef __cplusplus
}
#endif

#endif
