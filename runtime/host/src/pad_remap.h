#ifndef BLUEWAKE_PAD_REMAP_H
#define BLUEWAKE_PAD_REMAP_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// The game's own buttons and sticks on player 1's controller and on the
// keyboard: Aurora's mapping (dolphin/pad.h), which PADRead applies and
// Aurora saves, per controller model, in its user folder (with the keyboard's,
// keyboard_bindings.dat). Changes apply at once; bluewake_padmap_save writes
// them. Native buttons are SDL_GamepadButton values, axes SDL_GamepadAxis.

// Hooks the registry (gamepads.h) and the actions (input_bindings.h) to Aurora.
void bluewake_padmap_install(void);

bool bluewake_padmap_has_controller(void); // player 1 has one
void bluewake_padmap_save(void);

// The GameCube buttons, in the menu's order.
int bluewake_padmap_button_count(void);
unsigned bluewake_padmap_button_at(int index); // PAD_BUTTON_A, ...
const char* bluewake_padmap_button_title(int index);
int bluewake_padmap_button_native(unsigned pad_button); // -1: none
// The native button that had it goes to the GameCube button `native` had.
void bluewake_padmap_set_button(unsigned pad_button, int native);
bool bluewake_padmap_uses_native_button(int native);

// The GameCube axes (stick, C-stick, triggers), each direction on its own.
int bluewake_padmap_axis_count(void);
unsigned bluewake_padmap_axis_at(int index); // PAD_AXIS_LEFT_X_POS, ...
const char* bluewake_padmap_axis_title(int index);
// The native axis and its direction, or a native button (axis -1).
bool bluewake_padmap_axis_native(unsigned pad_axis, int* axis, bool* positive, int* button);
void bluewake_padmap_set_axis(unsigned pad_axis, int axis, bool positive);
void bluewake_padmap_set_axis_button(unsigned pad_axis, int button);

// Presets on player 1's controller.
void bluewake_padmap_reset(void);           // the controller's defaults
void bluewake_padmap_nintendo_labels(void); // A, B, X, Y as printed (a Nintendo controller)
void bluewake_padmap_swap_sticks(void);

// As the game sees it through the mapping, before dead zones. Sticks are
// SDL's way round (-1..1, y down); false without a controller on port 0.
bool bluewake_padmap_stick(int stick, double* x, double* y); // 0 the stick, 1 the C-stick
double bluewake_padmap_trigger(int trigger);                  // 0 L, 1 R: 0..1
bool bluewake_padmap_button_down(unsigned pad_button);

// The keyboard (port 0). Keys are SDL_Scancode values, 0 for none.
int bluewake_padmap_key(unsigned pad_button);
void bluewake_padmap_set_key(unsigned pad_button, int scancode); // swaps with the button that had it
int bluewake_padmap_axis_key(unsigned pad_axis);
void bluewake_padmap_set_axis_key(unsigned pad_axis, int scancode);
void bluewake_padmap_reset_keys(void); // W A S D, J K U I ... (aurora_input.cpp's)

// Player 1's controller kept across launches (BLUEWAKE_PAD_PORT0=fixed), or
// that choice forgotten.
void bluewake_padmap_persist_port0(unsigned id);
void bluewake_padmap_forget_port0(void);

#ifdef __cplusplus
}
#endif

#endif
