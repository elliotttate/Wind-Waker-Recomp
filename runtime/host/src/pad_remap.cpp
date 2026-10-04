// The game's buttons and sticks on player 1's controller and the keyboard,
// through Aurora's mapping (pad_remap.h).
#include "pad_remap.h"

extern "C" {
#include "gamepads.h"
#include "input_bindings.h"
}

#include <SDL3/SDL.h>
#include <dolphin/pad.h>

#include <algorithm>
#include <cstdio>
#include <iterator>

namespace {

struct Named {
    unsigned id;
    const char* title;
};

constexpr Named kButtons[] = {
    {PAD_BUTTON_A, "A"},
    {PAD_BUTTON_B, "B"},
    {PAD_BUTTON_X, "X"},
    {PAD_BUTTON_Y, "Y"},
    {PAD_TRIGGER_Z, "Z"},
    {PAD_TRIGGER_L, "L (full press)"},
    {PAD_TRIGGER_R, "R (full press)"},
    {PAD_BUTTON_START, "Start"},
    {PAD_BUTTON_UP, "D-pad up"},
    {PAD_BUTTON_DOWN, "D-pad down"},
    {PAD_BUTTON_LEFT, "D-pad left"},
    {PAD_BUTTON_RIGHT, "D-pad right"},
};

constexpr Named kAxes[] = {
    {PAD_AXIS_LEFT_Y_POS, "Stick up"},
    {PAD_AXIS_LEFT_Y_NEG, "Stick down"},
    {PAD_AXIS_LEFT_X_NEG, "Stick left"},
    {PAD_AXIS_LEFT_X_POS, "Stick right"},
    {PAD_AXIS_RIGHT_Y_POS, "C-stick up"},
    {PAD_AXIS_RIGHT_Y_NEG, "C-stick down"},
    {PAD_AXIS_RIGHT_X_NEG, "C-stick left"},
    {PAD_AXIS_RIGHT_X_POS, "C-stick right"},
    {PAD_AXIS_TRIGGER_L, "L (analog)"},
    {PAD_AXIS_TRIGGER_R, "R (analog)"},
};

PADButtonMapping* buttons(u32* count) {
    *count = 0;
    PADButtonMapping* mappings = PADGetButtonMappings(0, count);
    return mappings != nullptr && *count > 0 ? mappings : nullptr;
}

PADAxisMapping* axes(u32* count) {
    *count = 0;
    PADAxisMapping* mappings = PADGetAxisMappings(0, count);
    return mappings != nullptr && *count > 0 ? mappings : nullptr;
}

PADAxisMapping* axis_entry(unsigned pad_axis) {
    u32 count;
    PADAxisMapping* mappings = axes(&count);
    for (u32 i = 0; mappings != nullptr && i < count; ++i)
        if (mappings[i].padAxis == pad_axis)
            return &mappings[i];
    return nullptr;
}

// As Aurora's _get_axis_value: the native axis times its sign, or full for a
// held button.
int axis_value(SDL_Gamepad* pad, unsigned pad_axis) {
    const PADAxisMapping* entry = axis_entry(pad_axis);
    if (pad == nullptr || entry == nullptr)
        return 0;
    if (entry->nativeAxis.nativeAxis >= 0) {
        const int value = SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(entry->nativeAxis.nativeAxis)) *
                          static_cast<int>(entry->nativeAxis.sign);
        return std::min(value, static_cast<int>(SDL_JOYSTICK_AXIS_MAX));
    }
    return entry->nativeButton >= 0 && SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(entry->nativeButton))
               ? SDL_JOYSTICK_AXIS_MAX
               : 0;
}

// Aurora's index for an SDL controller (its order changes as controllers come and go).
int aurora_index(unsigned id) {
    for (u32 i = 0; i < PADCount(); ++i) {
        SDL_Gamepad* pad = PADGetSDLGamepadForIndex(i);
        if (pad != nullptr && SDL_GetGamepadID(pad) == id)
            return static_cast<int>(i);
    }
    return -1;
}

// Held buttons do not carry over to the controller that takes player 1:
// Aurora suppresses what is held until it is let go.
void handover() {
    PADBlockInput(true);
    PADBlockInput(false);
}

bool game_uses(int button) { return bluewake_padmap_uses_native_button(button); }

} // namespace

extern "C" void bluewake_padmap_install(void) {
    const BWGamepadHooks hooks = {handover, bluewake_padmap_persist_port0};
    bluewake_gamepads_set_hooks(&hooks);
    bluewake_actions_set_game_button_check(game_uses);
}

extern "C" bool bluewake_padmap_has_controller(void) { return PADGetIndexForPort(0) >= 0; }

extern "C" void bluewake_padmap_save(void) { PADSerializeMappings(); }

extern "C" int bluewake_padmap_button_count(void) { return static_cast<int>(std::size(kButtons)); }

extern "C" unsigned bluewake_padmap_button_at(int index) {
    return index >= 0 && index < bluewake_padmap_button_count() ? kButtons[index].id : 0u;
}

extern "C" const char* bluewake_padmap_button_title(int index) {
    return index >= 0 && index < bluewake_padmap_button_count() ? kButtons[index].title : "";
}

extern "C" int bluewake_padmap_button_native(unsigned pad_button) {
    u32 count;
    const PADButtonMapping* mappings = buttons(&count);
    for (u32 i = 0; mappings != nullptr && i < count; ++i)
        if (mappings[i].padButton == pad_button)
            return mappings[i].nativeButton == PAD_NATIVE_BUTTON_INVALID ? -1
                                                                         : static_cast<int>(mappings[i].nativeButton);
    return -1;
}

extern "C" void bluewake_padmap_set_button(unsigned pad_button, int native) {
    u32 count;
    PADButtonMapping* mappings = buttons(&count);
    PADButtonMapping* target = nullptr;
    for (u32 i = 0; mappings != nullptr && i < count; ++i)
        if (mappings[i].padButton == pad_button)
            target = &mappings[i];
    if (target == nullptr)
        return;
    const u32 wanted = native >= 0 ? static_cast<u32>(native) : PAD_NATIVE_BUTTON_INVALID;
    if (wanted != PAD_NATIVE_BUTTON_INVALID)
        for (u32 i = 0; i < count; ++i)
            if (&mappings[i] != target && mappings[i].nativeButton == wanted)
                mappings[i].nativeButton = target->nativeButton;
    target->nativeButton = wanted;
}

extern "C" bool bluewake_padmap_uses_native_button(int native) {
    if (native < 0)
        return false;
    u32 count;
    const PADButtonMapping* mappings = buttons(&count);
    for (u32 i = 0; mappings != nullptr && i < count; ++i)
        if (mappings[i].nativeButton == static_cast<u32>(native))
            return true;
    const PADAxisMapping* axis_mappings = axes(&count);
    for (u32 i = 0; axis_mappings != nullptr && i < count; ++i)
        if (axis_mappings[i].nativeAxis.nativeAxis < 0 && axis_mappings[i].nativeButton == native)
            return true;
    return false;
}

extern "C" int bluewake_padmap_axis_count(void) { return static_cast<int>(std::size(kAxes)); }

extern "C" unsigned bluewake_padmap_axis_at(int index) {
    return index >= 0 && index < bluewake_padmap_axis_count() ? kAxes[index].id : 0u;
}

extern "C" const char* bluewake_padmap_axis_title(int index) {
    return index >= 0 && index < bluewake_padmap_axis_count() ? kAxes[index].title : "";
}

extern "C" bool bluewake_padmap_axis_native(unsigned pad_axis, int* axis, bool* positive, int* button) {
    const PADAxisMapping* entry = axis_entry(pad_axis);
    *axis = -1;
    *positive = true;
    *button = -1;
    if (entry == nullptr)
        return false;
    *axis = entry->nativeAxis.nativeAxis;
    *positive = entry->nativeAxis.sign == AXIS_SIGN_POSITIVE;
    *button = entry->nativeAxis.nativeAxis < 0 ? entry->nativeButton : -1;
    return *axis >= 0 || *button >= 0;
}

// The axis direction that had this native input takes `target`'s old one.
static void swap_into(PADAxisMapping* target, const PADAxisMapping& wanted) {
    u32 count;
    PADAxisMapping* mappings = axes(&count);
    for (u32 i = 0; mappings != nullptr && i < count; ++i) {
        PADAxisMapping& other = mappings[i];
        if (&other == target)
            continue;
        const bool same_axis = wanted.nativeAxis.nativeAxis >= 0 &&
                               other.nativeAxis.nativeAxis == wanted.nativeAxis.nativeAxis &&
                               other.nativeAxis.sign == wanted.nativeAxis.sign;
        const bool same_button = wanted.nativeAxis.nativeAxis < 0 && other.nativeAxis.nativeAxis < 0 &&
                                 other.nativeButton == wanted.nativeButton;
        if (same_axis || same_button) {
            other.nativeAxis = target->nativeAxis;
            other.nativeButton = target->nativeButton;
        }
    }
    target->nativeAxis = wanted.nativeAxis;
    target->nativeButton = wanted.nativeButton;
}

extern "C" void bluewake_padmap_set_axis(unsigned pad_axis, int axis, bool positive) {
    PADAxisMapping* target = axis_entry(pad_axis);
    if (target == nullptr || axis < 0 || axis >= SDL_GAMEPAD_AXIS_COUNT)
        return;
    swap_into(target, PADAxisMapping{{axis, positive ? AXIS_SIGN_POSITIVE : AXIS_SIGN_NEGATIVE},
                                     SDL_GAMEPAD_BUTTON_INVALID, static_cast<PADAxis>(pad_axis)});
}

extern "C" void bluewake_padmap_set_axis_button(unsigned pad_axis, int button) {
    PADAxisMapping* target = axis_entry(pad_axis);
    if (target == nullptr || button < 0 || button >= SDL_GAMEPAD_BUTTON_COUNT)
        return;
    swap_into(target, PADAxisMapping{{-1, AXIS_SIGN_POSITIVE}, button, static_cast<PADAxis>(pad_axis)});
}

extern "C" void bluewake_padmap_reset(void) { PADRestoreDefaultMapping(0); }

extern "C" void bluewake_padmap_nintendo_labels(void) {
    // A Nintendo controller prints A on the right, B at the bottom, X at the
    // top and Y on the left; SDL names its buttons by position.
    bluewake_padmap_set_button(PAD_BUTTON_A, SDL_GAMEPAD_BUTTON_EAST);
    bluewake_padmap_set_button(PAD_BUTTON_B, SDL_GAMEPAD_BUTTON_SOUTH);
    bluewake_padmap_set_button(PAD_BUTTON_X, SDL_GAMEPAD_BUTTON_NORTH);
    bluewake_padmap_set_button(PAD_BUTTON_Y, SDL_GAMEPAD_BUTTON_WEST);
}

extern "C" void bluewake_padmap_swap_sticks(void) {
    constexpr PADAxis kPairs[4][2] = {{PAD_AXIS_LEFT_X_POS, PAD_AXIS_RIGHT_X_POS},
                                      {PAD_AXIS_LEFT_X_NEG, PAD_AXIS_RIGHT_X_NEG},
                                      {PAD_AXIS_LEFT_Y_POS, PAD_AXIS_RIGHT_Y_POS},
                                      {PAD_AXIS_LEFT_Y_NEG, PAD_AXIS_RIGHT_Y_NEG}};
    for (const auto& pair : kPairs) {
        PADAxisMapping* left = axis_entry(pair[0]);
        PADAxisMapping* right = axis_entry(pair[1]);
        if (left == nullptr || right == nullptr)
            return;
        std::swap(left->nativeAxis, right->nativeAxis);
        std::swap(left->nativeButton, right->nativeButton);
    }
}

extern "C" bool bluewake_padmap_stick(int stick, double* x, double* y) {
    *x = *y = 0.0;
    SDL_Gamepad* pad = bluewake_gamepads_for_port(0);
    if (pad == nullptr || axis_entry(PAD_AXIS_LEFT_X_POS) == nullptr)
        return false;
    const bool c = stick != 0;
    // As PADRead: half the difference of the two directions; GameCube's y is up.
    const int gx = (axis_value(pad, c ? PAD_AXIS_RIGHT_X_POS : PAD_AXIS_LEFT_X_POS) -
                    axis_value(pad, c ? PAD_AXIS_RIGHT_X_NEG : PAD_AXIS_LEFT_X_NEG)) / 2;
    const int gy = (axis_value(pad, c ? PAD_AXIS_RIGHT_Y_POS : PAD_AXIS_LEFT_Y_POS) -
                    axis_value(pad, c ? PAD_AXIS_RIGHT_Y_NEG : PAD_AXIS_LEFT_Y_NEG)) / 2;
    *x = std::clamp(gx / 32767.0, -1.0, 1.0);
    *y = std::clamp(-gy / 32767.0, -1.0, 1.0);
    return true;
}

extern "C" double bluewake_padmap_trigger(int trigger) {
    SDL_Gamepad* pad = bluewake_gamepads_for_port(0);
    const int value = axis_value(pad, trigger == 0 ? PAD_AXIS_TRIGGER_L : PAD_AXIS_TRIGGER_R);
    return std::clamp(value / 32767.0, 0.0, 1.0);
}

extern "C" bool bluewake_padmap_button_down(unsigned pad_button) {
    SDL_Gamepad* pad = bluewake_gamepads_for_port(0);
    const int native = bluewake_padmap_button_native(pad_button);
    return pad != nullptr && native >= 0 && SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(native));
}

// --- the keyboard -----------------------------------------------------------------

namespace {

PADKeyButtonBinding* key_buttons(u32* count) {
    *count = 0;
    PADKeyButtonBinding* bindings = PADGetKeyButtonBindings(0, count);
    return bindings != nullptr && *count > 0 ? bindings : nullptr;
}

PADKeyAxisBinding* key_axes(u32* count) {
    *count = 0;
    PADKeyAxisBinding* bindings = PADGetKeyAxisBindings(0, count);
    return bindings != nullptr && *count > 0 ? bindings : nullptr;
}

s32 to_aurora_key(int scancode) { return scancode > 0 ? scancode : PAD_KEY_INVALID; }
int from_aurora_key(s32 key) { return key > 0 ? key : 0; }

} // namespace

extern "C" int bluewake_padmap_key(unsigned pad_button) {
    u32 count;
    const PADKeyButtonBinding* bindings = key_buttons(&count);
    for (u32 i = 0; bindings != nullptr && i < count; ++i)
        if (bindings[i].padButton == pad_button)
            return from_aurora_key(bindings[i].scancode);
    return 0;
}

extern "C" void bluewake_padmap_set_key(unsigned pad_button, int scancode) {
    u32 count;
    PADKeyButtonBinding* bindings = key_buttons(&count);
    PADKeyButtonBinding* target = nullptr;
    for (u32 i = 0; bindings != nullptr && i < count; ++i)
        if (bindings[i].padButton == pad_button)
            target = &bindings[i];
    if (target == nullptr)
        return;
    const s32 old = target->scancode, wanted = to_aurora_key(scancode);
    if (wanted != PAD_KEY_INVALID)
        for (u32 i = 0; i < count; ++i)
            if (&bindings[i] != target && bindings[i].scancode == wanted)
                bindings[i].scancode = old;
    target->scancode = wanted;
    // L and R are a button and an analog trigger on the same key (E, R): the
    // trigger follows the button while they were together.
    const unsigned trigger = pad_button == PAD_TRIGGER_L   ? PAD_AXIS_TRIGGER_L
                             : pad_button == PAD_TRIGGER_R ? PAD_AXIS_TRIGGER_R
                                                           : PAD_AXIS_COUNT;
    if (trigger != PAD_AXIS_COUNT && bluewake_padmap_axis_key(trigger) == from_aurora_key(old))
        bluewake_padmap_set_axis_key(trigger, scancode);
}

extern "C" int bluewake_padmap_axis_key(unsigned pad_axis) {
    u32 count;
    const PADKeyAxisBinding* bindings = key_axes(&count);
    for (u32 i = 0; bindings != nullptr && i < count; ++i)
        if (bindings[i].padAxis == pad_axis)
            return from_aurora_key(bindings[i].scancode);
    return 0;
}

extern "C" void bluewake_padmap_set_axis_key(unsigned pad_axis, int scancode) {
    u32 count;
    PADKeyAxisBinding* bindings = key_axes(&count);
    PADKeyAxisBinding* target = nullptr;
    for (u32 i = 0; bindings != nullptr && i < count; ++i)
        if (bindings[i].padAxis == pad_axis)
            target = &bindings[i];
    if (target == nullptr)
        return;
    const s32 old = target->scancode, wanted = to_aurora_key(scancode);
    if (wanted != PAD_KEY_INVALID)
        for (u32 i = 0; i < count; ++i)
            if (&bindings[i] != target && bindings[i].scancode == wanted)
                bindings[i].scancode = old;
    target->scancode = wanted;
    target->influence = 32767;
}

extern "C" void bluewake_padmap_reset_keys(void) {
    PADKeyButtonBinding keys[PAD_BUTTON_COUNT] = {
        {SDL_SCANCODE_LEFT, PAD_BUTTON_LEFT}, {SDL_SCANCODE_RIGHT, PAD_BUTTON_RIGHT},
        {SDL_SCANCODE_DOWN, PAD_BUTTON_DOWN}, {SDL_SCANCODE_UP, PAD_BUTTON_UP},
        {SDL_SCANCODE_Q, PAD_TRIGGER_Z},      {SDL_SCANCODE_R, PAD_TRIGGER_R},
        {SDL_SCANCODE_E, PAD_TRIGGER_L},      {SDL_SCANCODE_J, PAD_BUTTON_A},
        {SDL_SCANCODE_K, PAD_BUTTON_B},       {SDL_SCANCODE_U, PAD_BUTTON_X},
        {SDL_SCANCODE_I, PAD_BUTTON_Y},       {SDL_SCANCODE_RETURN, PAD_BUTTON_START},
    };
    PADKeyAxisBinding key_axes_default[PAD_AXIS_COUNT] = {
        {SDL_SCANCODE_D, PAD_AXIS_LEFT_X_POS, 32767},  {SDL_SCANCODE_A, PAD_AXIS_LEFT_X_NEG, 32767},
        {SDL_SCANCODE_W, PAD_AXIS_LEFT_Y_POS, 32767},  {SDL_SCANCODE_S, PAD_AXIS_LEFT_Y_NEG, 32767},
        {SDL_SCANCODE_H, PAD_AXIS_RIGHT_X_POS, 32767}, {SDL_SCANCODE_F, PAD_AXIS_RIGHT_X_NEG, 32767},
        {SDL_SCANCODE_T, PAD_AXIS_RIGHT_Y_POS, 32767}, {SDL_SCANCODE_G, PAD_AXIS_RIGHT_Y_NEG, 32767},
        {SDL_SCANCODE_E, PAD_AXIS_TRIGGER_L, 32767},   {SDL_SCANCODE_R, PAD_AXIS_TRIGGER_R, 32767},
    };
    PADSetKeyButtonBindings(0, keys);
    PADSetKeyAxisBindings(0, key_axes_default);
    PADSetKeyboardActive(0, TRUE);
}

// --- player 1 across launches ----------------------------------------------------

extern "C" void bluewake_padmap_persist_port0(unsigned id) {
    const int index = aurora_index(id);
    if (index >= 0)
        PADSetPortForIndex(static_cast<u32>(index), 0);
}

extern "C" void bluewake_padmap_forget_port0(void) {
    // Clearing the saved choice takes the controller off port 0 too; it keeps
    // it for this session.
    SDL_Gamepad* pad = bluewake_gamepads_for_port(0);
    PADClearPort(0);
    if (pad != nullptr)
        SDL_SetGamepadPlayerIndex(pad, 0);
    std::fprintf(stderr, "[pads] player 1's saved controller forgotten\n");
}
