// The controller registry (gamepads.c), the host's button bindings
// (input_bindings.c) and the button names (controller_glyphs.c), with SDL's
// virtual controllers plugged in and out as the game runs.
#include "controller_glyphs.h"
#include "gamepads.h"
#include "input_bindings.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

static int g_handovers, g_persisted;
static unsigned g_persisted_id;
static void handover(void) { ++g_handovers; }
static void persist(unsigned id) {
    ++g_persisted;
    g_persisted_id = id;
}

static SDL_JoystickID attach(const char* name, Uint16 vendor, Uint16 product) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.vendor_id = vendor;
    desc.product_id = product;
    desc.name = name;
    return SDL_AttachVirtualJoystick(&desc);
}

// The events as the host's observer gets them.
static void pump(void) {
    SDL_UpdateJoysticks();
    SDL_Event event;
    while (SDL_PollEvent(&event))
        bluewake_gamepads_event(&event);
}

static void press(SDL_JoystickID id, SDL_GamepadButton button, bool down) {
    SDL_Gamepad* pad = SDL_GetGamepadFromID(id);
    SDL_Joystick* joystick = pad != NULL ? SDL_GetGamepadJoystick(pad) : NULL;
    if (joystick != NULL)
        SDL_SetJoystickVirtualButton(joystick, button, down);
    pump();
}

static int port_of(SDL_JoystickID id) {
    const BWGamepad* pad = bluewake_gamepads_find(id);
    return pad != NULL ? pad->port : -2;
}

static void drain_notices(void) {
    char text[128];
    while (bluewake_gamepads_take_notice(text, sizeof text)) {
    }
}

static bool notice_contains(const char* part) {
    char text[128];
    bool found = false;
    while (bluewake_gamepads_take_notice(text, sizeof text))
        found = found || strstr(text, part) != NULL;
    return found;
}

static void test_bindings_text(void) {
    unsetenv("BLUEWAKE_PAD_ACTIONS");
    unsetenv("BLUEWAKE_KEY_ACTIONS");
    bluewake_actions_reload();
    CHECK(bluewake_action_button(BW_ACTION_JUMP) == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    CHECK(bluewake_action_button(BW_ACTION_MENU) == SDL_GAMEPAD_BUTTON_BACK);
    CHECK(bluewake_action_key(BW_ACTION_SPRINT) == SDL_SCANCODE_LSHIFT);

    // A part not understood is reported; the rest applies.
    CHECK(!bluewake_actions_parse_buttons("jump=rightshoulder,menu=none,bogus=a,sprint=nosuchbutton"));
    CHECK(bluewake_action_button(BW_ACTION_JUMP) == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    CHECK(bluewake_action_button(BW_ACTION_MENU) == -1);
    CHECK(bluewake_action_button(BW_ACTION_SPRINT) == SDL_GAMEPAD_BUTTON_LEFT_STICK);
    char text[512];
    bluewake_actions_format_buttons(text, sizeof text);
    CHECK(strstr(text, "jump=rightshoulder") != NULL);
    CHECK(strstr(text, "menu=none") != NULL);

    CHECK(bluewake_actions_parse_keys("jump=J,sprint=Right Shift"));
    CHECK(bluewake_action_key(BW_ACTION_JUMP) == SDL_SCANCODE_J);
    CHECK(bluewake_action_key(BW_ACTION_SPRINT) == SDL_SCANCODE_RSHIFT);
    bluewake_actions_format_keys(text, sizeof text);
    CHECK(strstr(text, "sprint=Right Shift") != NULL);

    // The saved text reads back the same.
    setenv("BLUEWAKE_PAD_ACTIONS", "jump=a,first_person=none", 1);
    setenv("BLUEWAKE_KEY_ACTIONS", "menu=F2", 1);
    bluewake_actions_reload();
    CHECK(bluewake_action_button(BW_ACTION_JUMP) == SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(bluewake_action_button(BW_ACTION_FIRST_PERSON) == -1);
    CHECK(bluewake_action_key(BW_ACTION_MENU) == SDL_SCANCODE_F2);
    CHECK(bluewake_action_key(BW_ACTION_JUMP) == SDL_SCANCODE_SPACE);

    // A key event is the action's press; its repeats are not.
    SDL_Event key;
    SDL_zero(key);
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.scancode = SDL_SCANCODE_F2;
    CHECK(bluewake_action_pressed_by(BW_ACTION_MENU, &key));
    key.key.repeat = true;
    CHECK(!bluewake_action_pressed_by(BW_ACTION_MENU, &key));

    unsetenv("BLUEWAKE_PAD_ACTIONS");
    unsetenv("BLUEWAKE_KEY_ACTIONS");
    bluewake_actions_reload();
}

static void test_glyphs(void) {
    CHECK(bluewake_pad_family(SDL_GAMEPAD_TYPE_PS5, 0x054C, 0x0CE6) == BW_FAMILY_PLAYSTATION);
    CHECK(bluewake_pad_family(SDL_GAMEPAD_TYPE_XBOXONE, 0x045E, 0x0B12) == BW_FAMILY_XBOX);
    CHECK(bluewake_pad_family(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO, 0x057E, 0x2009) == BW_FAMILY_NINTENDO);
    CHECK(bluewake_pad_family(SDL_GAMEPAD_TYPE_STANDARD, 0x057E, 0x0337) == BW_FAMILY_GAMECUBE);
    CHECK(strcmp(bluewake_glyph_button(BW_FAMILY_PLAYSTATION, SDL_GAMEPAD_TYPE_PS5, SDL_GAMEPAD_BUTTON_SOUTH),
                 "Cross") == 0);
    CHECK(strcmp(bluewake_glyph_button(BW_FAMILY_XBOX, SDL_GAMEPAD_TYPE_XBOXONE, SDL_GAMEPAD_BUTTON_SOUTH), "A") == 0);
    CHECK(strcmp(bluewake_glyph_button(BW_FAMILY_NINTENDO, SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO,
                                       SDL_GAMEPAD_BUTTON_SOUTH),
                 "B") == 0);
    CHECK(strcmp(bluewake_glyph_button(BW_FAMILY_PLAYSTATION, SDL_GAMEPAD_TYPE_PS5,
                                       SDL_GAMEPAD_BUTTON_LEFT_SHOULDER),
                 "L1") == 0);
    CHECK(strcmp(bluewake_glyph_axis(BW_FAMILY_XBOX, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, true), "LT") == 0);
    CHECK(strcmp(bluewake_glyph_axis(BW_FAMILY_GENERIC, SDL_GAMEPAD_AXIS_LEFTY, false), "Left stick up") == 0);
    CHECK(strcmp(bluewake_glyph_button(BW_FAMILY_XBOX, 0, -1), "-") == 0);
    CHECK(strcmp(bluewake_glyph_key(SDL_SCANCODE_SPACE), "Space") == 0);
    CHECK(strcmp(bluewake_glyph_key(0), "-") == 0);
}

static void test_hotplug(void) {
    const BWGamepadHooks hooks = {handover, persist};
    bluewake_gamepads_set_hooks(&hooks);
    unsetenv("BLUEWAKE_PAD_PORT0");
    unsetenv("BLUEWAKE_PAD_PAUSE_ON_DISCONNECT");
    bluewake_gamepads_reload();
    CHECK(bluewake_gamepads_policy() == BW_PORT0_AUTO);
    CHECK(bluewake_gamepads_count() == 0);
    CHECK(bluewake_gamepads_for_port(0) == NULL);

    // The first controller connected is player 1.
    const SDL_JoystickID xbox = attach("Test Xbox controller", 0x045E, 0x0B12);
    CHECK(xbox != 0);
    pump();
    CHECK(bluewake_gamepads_count() == 1);
    CHECK(port_of(xbox) == 0);
    CHECK(bluewake_gamepads_for_port(0) == SDL_GetGamepadFromID(xbox));
    CHECK(notice_contains("connected"));

    // A second controller does not take player 1 from it.
    const SDL_JoystickID ps5 = attach("Test DualSense", 0x054C, 0x0CE6);
    pump();
    CHECK(bluewake_gamepads_count() == 2);
    CHECK(port_of(xbox) == 0);
    CHECK(port_of(ps5) != 0);
    const BWGamepad* ps5_entry = bluewake_gamepads_find(ps5);
    CHECK(ps5_entry != NULL && ps5_entry->type == SDL_GAMEPAD_TYPE_PS5);
    drain_notices();

    // Player 1's controller: the jump on its left bumper; the other
    // controller's bumper is not player 1's.
    CHECK(!bluewake_action_pad_down(BW_ACTION_JUMP));
    press(ps5, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
    CHECK(!bluewake_action_pad_down(BW_ACTION_JUMP));
    press(ps5, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false);
    press(xbox, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
    CHECK(bluewake_action_pad_down(BW_ACTION_JUMP));
    press(xbox, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false);
    CHECK(!bluewake_action_pad_down(BW_ACTION_JUMP));
    // In auto, a press on the other controller does not move player 1.
    CHECK(port_of(xbox) == 0);

    // Player 1's controller goes: the pause, and the other takes player 1.
    CHECK(!bluewake_gamepads_take_port0_lost());
    SDL_DetachVirtualJoystick(xbox);
    pump();
    CHECK(bluewake_gamepads_count() == 1);
    CHECK(bluewake_gamepads_take_port0_lost());
    CHECK(!bluewake_gamepads_take_port0_lost()); // once
    CHECK(port_of(ps5) == 0);
    CHECK(notice_contains("disconnected"));

    // It comes back: player 1 stays with the one playing.
    const SDL_JoystickID xbox2 = attach("Test Xbox controller", 0x045E, 0x0B12);
    pump();
    CHECK(port_of(ps5) == 0);
    CHECK(port_of(xbox2) != 0);

    // Last used: a press on the other controller hands it player 1, and held
    // buttons do not carry over (the handover hook).
    setenv("BLUEWAKE_PAD_PORT0", "last", 1);
    bluewake_gamepads_reload();
    CHECK(bluewake_gamepads_policy() == BW_PORT0_LAST);
    const int handovers = g_handovers;
    press(xbox2, SDL_GAMEPAD_BUTTON_SOUTH, true);
    press(xbox2, SDL_GAMEPAD_BUTTON_SOUTH, false);
    CHECK(port_of(xbox2) == 0);
    CHECK(port_of(ps5) != 0);
    CHECK(g_handovers == handovers + 1);
    // A stick resting off-centre is not a person; a tilt is.
    SDL_Joystick* ps5_joystick = SDL_GetGamepadJoystick(SDL_GetGamepadFromID(ps5));
    SDL_SetJoystickVirtualAxis(ps5_joystick, SDL_GAMEPAD_AXIS_LEFTX, 6000);
    pump();
    CHECK(port_of(xbox2) == 0);
    SDL_SetJoystickVirtualAxis(ps5_joystick, SDL_GAMEPAD_AXIS_LEFTX, 30000);
    pump();
    CHECK(port_of(ps5) == 0);
    SDL_SetJoystickVirtualAxis(ps5_joystick, SDL_GAMEPAD_AXIS_LEFTX, 0);
    pump();

    // The menu's choice of player 1.
    setenv("BLUEWAKE_PAD_PORT0", "auto", 1);
    bluewake_gamepads_reload();
    bluewake_gamepads_use_for_port0(xbox2);
    CHECK(port_of(xbox2) == 0);
    CHECK(port_of(ps5) != 0);
    // Fixed: the choice goes to Aurora's saved one.
    setenv("BLUEWAKE_PAD_PORT0", "fixed", 1);
    bluewake_gamepads_reload();
    bluewake_gamepads_use_for_port0(ps5);
    CHECK(g_persisted == 1 && g_persisted_id == ps5);
    // Fixed leaves an empty port 0 to Aurora.
    SDL_Gamepad* first = bluewake_gamepads_for_port(0);
    if (first != NULL)
        SDL_SetGamepadPlayerIndex(first, -1);
    bluewake_gamepads_sync();
    CHECK(bluewake_gamepads_for_port(0) == NULL);
    // Back to auto: it fills port 0 at once.
    setenv("BLUEWAKE_PAD_PORT0", "auto", 1);
    bluewake_gamepads_reload();
    CHECK(bluewake_gamepads_for_port(0) != NULL);

    // No pause when it is off.
    setenv("BLUEWAKE_PAD_PAUSE_ON_DISCONNECT", "0", 1);
    bluewake_gamepads_reload();
    SDL_Gamepad* player = bluewake_gamepads_for_port(0);
    const SDL_JoystickID player_id = player != NULL ? SDL_GetGamepadID(player) : 0;
    SDL_DetachVirtualJoystick(player_id);
    pump();
    CHECK(!bluewake_gamepads_take_port0_lost());
    CHECK(bluewake_gamepads_count() == 1);
    CHECK(bluewake_gamepads_for_port(0) != NULL);

    // The last one goes: no controller, the keyboard only.
    const SDL_JoystickID last = bluewake_gamepads_at(0)->id;
    SDL_DetachVirtualJoystick(last);
    pump();
    CHECK(bluewake_gamepads_count() == 0);
    CHECK(bluewake_gamepads_player_pad() == NULL);
    CHECK(!bluewake_action_pad_down(BW_ACTION_JUMP));
    drain_notices();
    unsetenv("BLUEWAKE_PAD_PORT0");
    unsetenv("BLUEWAKE_PAD_PAUSE_ON_DISCONNECT");
    bluewake_gamepads_reload();
}

static bool g_game_uses_left_shoulder;
static bool game_uses(int button) { return g_game_uses_left_shoulder && button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER; }

// A GameCube controller's buttons are the game's: the jump is on one only
// when the game's mapping leaves that button free.
static void test_gamecube(void) {
    bluewake_actions_reload();
    const SDL_JoystickID gc = attach("Test Switch Online GameCube controller", 0x057E, 0x2073);
    pump();
    const BWGamepad* entry = bluewake_gamepads_find(gc);
    CHECK(entry != NULL && entry->gamecube);
    CHECK(port_of(gc) == 0);
    press(gc, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
    bluewake_actions_set_game_button_check(NULL);
    CHECK(!bluewake_action_pad_down(BW_ACTION_JUMP));
    bluewake_actions_set_game_button_check(game_uses);
    g_game_uses_left_shoulder = true;
    CHECK(!bluewake_action_pad_down(BW_ACTION_JUMP));
    g_game_uses_left_shoulder = false;
    CHECK(bluewake_action_pad_down(BW_ACTION_JUMP));
    press(gc, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false);
    bluewake_actions_set_game_button_check(NULL);
    SDL_DetachVirtualJoystick(gc);
    pump();
    CHECK(bluewake_gamepads_count() == 0);
    drain_notices();
}

int main(void) {
    // Virtual controllers only: none the machine has plugged in.
    SDL_SetHint("SDL_JOYSTICK_HIDAPI", "0");
    SDL_SetHint("SDL_JOYSTICK_IOKIT", "0");
    SDL_SetHint("SDL_JOYSTICK_MFI", "0");
    SDL_SetHint("SDL_JOYSTICK_GAMEINPUT", "0");
    SDL_SetHint("SDL_JOYSTICK_RAWINPUT", "0");
    SDL_SetHint("SDL_JOYSTICK_DIRECTINPUT", "0");
    SDL_SetHint("SDL_XINPUT_ENABLED", "0");
    SDL_SetHint("SDL_JOYSTICK_LINUX_CLASSIC", "0");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    pump();
    // A real controller still showing up would change the counts: skip.
    if (bluewake_gamepads_count() != 0) {
        fprintf(stderr, "SKIP: a real controller is connected\n");
        SDL_Quit();
        return 0;
    }
    test_bindings_text();
    test_glyphs();
    test_hotplug();
    test_gamecube();
    SDL_Quit();
    if (g_failures == 0)
        printf("gamepads_test: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
