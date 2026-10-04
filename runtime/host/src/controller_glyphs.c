// Button names as each controller family prints them (controller_glyphs.h).
#include "controller_glyphs.h"

#include <SDL3/SDL.h>

BWPadFamily bluewake_pad_family(int type, unsigned short vendor, unsigned short product) {
    if (vendor == 0x057Eu && (product == 0x0337u || product == 0x2073u))
        return BW_FAMILY_GAMECUBE;
    switch (type) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE:
        return BW_FAMILY_XBOX;
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
        return BW_FAMILY_PLAYSTATION;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return BW_FAMILY_NINTENDO;
    case SDL_GAMEPAD_TYPE_GAMECUBE:
        return BW_FAMILY_GAMECUBE;
    default:
        return BW_FAMILY_GENERIC;
    }
}

const char* bluewake_pad_family_name(BWPadFamily family) {
    switch (family) {
    case BW_FAMILY_XBOX:
        return "Xbox";
    case BW_FAMILY_PLAYSTATION:
        return "PlayStation";
    case BW_FAMILY_NINTENDO:
        return "Nintendo";
    case BW_FAMILY_GAMECUBE:
        return "GameCube";
    default:
        return "Controller";
    }
}

static const char* face_label(SDL_GamepadButtonLabel label) {
    switch (label) {
    case SDL_GAMEPAD_BUTTON_LABEL_A:
        return "A";
    case SDL_GAMEPAD_BUTTON_LABEL_B:
        return "B";
    case SDL_GAMEPAD_BUTTON_LABEL_X:
        return "X";
    case SDL_GAMEPAD_BUTTON_LABEL_Y:
        return "Y";
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS:
        return "Cross";
    case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE:
        return "Circle";
    case SDL_GAMEPAD_BUTTON_LABEL_SQUARE:
        return "Square";
    case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE:
        return "Triangle";
    default:
        return NULL;
    }
}

// Per family: Xbox, PlayStation, Nintendo, GameCube, other.
typedef struct {
    int button;
    const char* names[5];
} ButtonNames;

static const ButtonNames kButtons[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, {"A", "Cross", "B", "A", "Bottom button"}},
    {SDL_GAMEPAD_BUTTON_EAST, {"B", "Circle", "A", "X", "Right button"}},
    {SDL_GAMEPAD_BUTTON_WEST, {"X", "Square", "Y", "B", "Left button"}},
    {SDL_GAMEPAD_BUTTON_NORTH, {"Y", "Triangle", "X", "Y", "Top button"}},
    {SDL_GAMEPAD_BUTTON_BACK, {"View", "Create", "-", "Back", "Back"}},
    {SDL_GAMEPAD_BUTTON_GUIDE, {"Xbox button", "PS button", "Home", "Home", "Guide"}},
    {SDL_GAMEPAD_BUTTON_START, {"Menu", "Options", "+", "Start", "Start"}},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, {"Left stick click", "L3", "Left stick click", "Left stick click",
                                     "Left stick click"}},
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, {"Right stick click", "R3", "Right stick click", "C-stick click",
                                      "Right stick click"}},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, {"LB", "L1", "L", "L (click)", "Left bumper"}},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, {"RB", "R1", "R", "Z", "Right bumper"}},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, {"D-pad up", "D-pad up", "D-pad up", "D-pad up", "D-pad up"}},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, {"D-pad down", "D-pad down", "D-pad down", "D-pad down", "D-pad down"}},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, {"D-pad left", "D-pad left", "D-pad left", "D-pad left", "D-pad left"}},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, {"D-pad right", "D-pad right", "D-pad right", "D-pad right", "D-pad right"}},
    {SDL_GAMEPAD_BUTTON_MISC1, {"Share", "Mute", "Capture", "Capture", "Extra button"}},
    {SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, {"P1", "Right Fn", "SR (right)", "Paddle R1", "Right paddle 1"}},
    {SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, {"P3", "Left Fn", "SL (left)", "Paddle L1", "Left paddle 1"}},
    {SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, {"P2", "Right back", "SL (right)", "Paddle R2", "Right paddle 2"}},
    {SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, {"P4", "Left back", "SR (left)", "Paddle L2", "Left paddle 2"}},
    {SDL_GAMEPAD_BUTTON_TOUCHPAD, {"Touchpad", "Touchpad", "Touchpad", "Touchpad", "Touchpad"}},
    {SDL_GAMEPAD_BUTTON_MISC3, {"Extra 3", "Extra 3", "Extra 3", "L (full press)", "Extra 3"}},
    {SDL_GAMEPAD_BUTTON_MISC4, {"Extra 4", "Extra 4", "Extra 4", "R (full press)", "Extra 4"}},
};

static int column(BWPadFamily family) {
    switch (family) {
    case BW_FAMILY_XBOX:
        return 0;
    case BW_FAMILY_PLAYSTATION:
        return 1;
    case BW_FAMILY_NINTENDO:
        return 2;
    case BW_FAMILY_GAMECUBE:
        return 3;
    default:
        return 4;
    }
}

const char* bluewake_glyph_button(BWPadFamily family, int type, int button) {
    if (button < 0 || button >= SDL_GAMEPAD_BUTTON_COUNT)
        return "-";
    // The face buttons: SDL knows the printed letters of more controllers
    // than the table (third-party ones with a known type).
    if (button <= SDL_GAMEPAD_BUTTON_NORTH && family != BW_FAMILY_GAMECUBE) {
        const char* label =
            face_label(SDL_GetGamepadButtonLabelForType((SDL_GamepadType)type, (SDL_GamepadButton)button));
        if (label != NULL)
            return label;
    }
    for (size_t i = 0; i < SDL_arraysize(kButtons); ++i)
        if (kButtons[i].button == button)
            return kButtons[i].names[column(family)];
    const char* name = SDL_GetGamepadStringForButton((SDL_GamepadButton)button);
    return name != NULL ? name : "?";
}

const char* bluewake_glyph_axis(BWPadFamily family, int axis, bool positive) {
    switch (axis) {
    case SDL_GAMEPAD_AXIS_LEFTX:
        return positive ? "Left stick right" : "Left stick left";
    case SDL_GAMEPAD_AXIS_LEFTY:
        return positive ? "Left stick down" : "Left stick up";
    case SDL_GAMEPAD_AXIS_RIGHTX:
        return family == BW_FAMILY_GAMECUBE ? (positive ? "C-stick right" : "C-stick left")
                                            : (positive ? "Right stick right" : "Right stick left");
    case SDL_GAMEPAD_AXIS_RIGHTY:
        return family == BW_FAMILY_GAMECUBE ? (positive ? "C-stick down" : "C-stick up")
                                            : (positive ? "Right stick down" : "Right stick up");
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: {
        static const char* const names[5] = {"LT", "L2", "ZL", "L", "Left trigger"};
        return names[column(family)];
    }
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: {
        static const char* const names[5] = {"RT", "R2", "ZR", "R", "Right trigger"};
        return names[column(family)];
    }
    default:
        return "-";
    }
}

const char* bluewake_glyph_key(int scancode) {
    if (scancode <= 0 || scancode >= SDL_SCANCODE_COUNT)
        return "-";
    const char* name = SDL_GetScancodeName((SDL_Scancode)scancode);
    return name != NULL && name[0] != '\0' ? name : "?";
}
