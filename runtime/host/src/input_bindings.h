#ifndef BLUEWAKE_INPUT_BINDINGS_H
#define BLUEWAKE_INPUT_BINDINGS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The host's own buttons, which a GameCube controller does not have: the
// jump, the sprint, first person on the camera stick, the telescope's zoom
// and the options menu. Each has a controller button (player 1's controller,
// gamepads.h) and a key. The game's own buttons (A, B, ... Start) are
// Aurora's mapping (pad_remap.h).
//
//   BLUEWAKE_PAD_ACTIONS=jump=leftshoulder,sprint=leftstick,...  SDL's button
//                         names (SDL_GetGamepadStringForButton), none: no button
//   BLUEWAKE_KEY_ACTIONS=jump=Space,sprint=Left Shift,...        SDL's key names
//
// Actions left out keep their defaults.

typedef enum BWAction {
    BW_ACTION_JUMP = 0,
    BW_ACTION_SPRINT,
    BW_ACTION_FIRST_PERSON,
    BW_ACTION_ZOOM_IN,
    BW_ACTION_ZOOM_OUT,
    BW_ACTION_MENU,
    BW_ACTION_COUNT
} BWAction;

void bluewake_actions_reload(void);

const char* bluewake_action_name(BWAction action);  // "jump", as in the settings
const char* bluewake_action_title(BWAction action); // "Jump", as in the menu

// Controller buttons are SDL_GamepadButton values, -1 for none; keys are
// SDL_Scancode values, 0 for none.
int bluewake_action_button(BWAction action);
int bluewake_action_default_button(BWAction action);
void bluewake_action_set_button(BWAction action, int button);
int bluewake_action_key(BWAction action);
int bluewake_action_default_key(BWAction action);
void bluewake_action_set_key(BWAction action, int scancode);
void bluewake_actions_reset(void);

// Held now: on player 1's controller (any controller while none is player 1),
// on the keyboard, or either.
bool bluewake_action_pad_down(BWAction action);
bool bluewake_action_key_down(BWAction action);
bool bluewake_action_down(BWAction action);
// The SDL event is this action's press (a key without its repeats, or a
// controller button).
bool bluewake_action_pressed_by(BWAction action, const void* sdl_event);

// The settings' text (BLUEWAKE_PAD_ACTIONS, BLUEWAKE_KEY_ACTIONS).
void bluewake_actions_format_buttons(char* out, size_t size);
void bluewake_actions_format_keys(char* out, size_t size);
// Reads that text over the current bindings; false if a part was not understood
// (the rest still applies).
bool bluewake_actions_parse_buttons(const char* text);
bool bluewake_actions_parse_keys(const char* text);

// A GameCube controller has no spare buttons: on one, an action whose button
// the game's own mapping also uses is left to the game. `uses` says whether
// player 1's mapping uses a native button (pad_remap.cpp; none: every action
// is left to the game on a GameCube controller).
void bluewake_actions_set_game_button_check(bool (*uses)(int button));

#ifdef __cplusplus
}
#endif

#endif
