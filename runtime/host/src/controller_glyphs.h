#ifndef BLUEWAKE_CONTROLLER_GLYPHS_H
#define BLUEWAKE_CONTROLLER_GLYPHS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// What a controller's buttons are called on that controller: the bottom face
// button is A on an Xbox controller, Cross on a PlayStation one and B on a
// Nintendo one. `type` is an SDL_GamepadType, `button` an SDL_GamepadButton,
// `axis` an SDL_GamepadAxis.

typedef enum BWPadFamily {
    BW_FAMILY_GENERIC = 0,
    BW_FAMILY_XBOX,
    BW_FAMILY_PLAYSTATION,
    BW_FAMILY_NINTENDO,
    BW_FAMILY_GAMECUBE,
} BWPadFamily;

BWPadFamily bluewake_pad_family(int type, unsigned short vendor, unsigned short product);
const char* bluewake_pad_family_name(BWPadFamily family); // "Xbox", "PlayStation", ...

const char* bluewake_glyph_button(BWPadFamily family, int type, int button); // "Cross", "L1", "-"...
// An axis direction: positive is right, down (SDL's y) or a trigger pulled.
const char* bluewake_glyph_axis(BWPadFamily family, int axis, bool positive); // "Left stick up", "L2"...

// A key (SDL_Scancode) as the menu shows it; "-" for none.
const char* bluewake_glyph_key(int scancode);

#ifdef __cplusplus
}
#endif

#endif
