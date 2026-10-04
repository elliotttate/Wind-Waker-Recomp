// The host's own buttons and keys (input_bindings.h).
#include "input_bindings.h"

#include "gamepads.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char* name;
    const char* title;
    int button, key; // the defaults
} ActionInfo;

static const ActionInfo kActions[BW_ACTION_COUNT] = {
    [BW_ACTION_JUMP] = {"jump", "Jump", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_SCANCODE_SPACE},
    [BW_ACTION_SPRINT] = {"sprint", "Sprint", SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_SCANCODE_LSHIFT},
    [BW_ACTION_FIRST_PERSON] = {"first_person", "First person (camera stick)", SDL_GAMEPAD_BUTTON_RIGHT_STICK,
                                SDL_SCANCODE_UNKNOWN},
    [BW_ACTION_ZOOM_IN] = {"zoom_in", "Zoom in (telescope, Picto Box)", SDL_GAMEPAD_BUTTON_DPAD_UP,
                           SDL_SCANCODE_UNKNOWN},
    [BW_ACTION_ZOOM_OUT] = {"zoom_out", "Zoom out (telescope, Picto Box)", SDL_GAMEPAD_BUTTON_DPAD_DOWN,
                            SDL_SCANCODE_UNKNOWN},
    [BW_ACTION_MENU] = {"menu", "Options menu", SDL_GAMEPAD_BUTTON_BACK, SDL_SCANCODE_F1},
};

static int g_button[BW_ACTION_COUNT];
static int g_key[BW_ACTION_COUNT];
static bool g_loaded;
static bool (*g_game_uses)(int button);

static void defaults(void) {
    for (int i = 0; i < BW_ACTION_COUNT; ++i) {
        g_button[i] = kActions[i].button;
        g_key[i] = kActions[i].key;
    }
    g_loaded = true;
}

static void ensure_loaded(void) {
    if (!g_loaded)
        bluewake_actions_reload();
}

static bool valid(BWAction action) { return (int)action >= 0 && action < BW_ACTION_COUNT; }

void bluewake_actions_reload(void) {
    defaults();
    const char* buttons = getenv("BLUEWAKE_PAD_ACTIONS");
    if (buttons != NULL && buttons[0] != '\0' && !bluewake_actions_parse_buttons(buttons))
        fprintf(stderr, "[actions] BLUEWAKE_PAD_ACTIONS: some of \"%s\" was not understood\n", buttons);
    const char* keys = getenv("BLUEWAKE_KEY_ACTIONS");
    if (keys != NULL && keys[0] != '\0' && !bluewake_actions_parse_keys(keys))
        fprintf(stderr, "[actions] BLUEWAKE_KEY_ACTIONS: some of \"%s\" was not understood\n", keys);
}

const char* bluewake_action_name(BWAction action) { return valid(action) ? kActions[action].name : ""; }
const char* bluewake_action_title(BWAction action) { return valid(action) ? kActions[action].title : ""; }

int bluewake_action_button(BWAction action) {
    ensure_loaded();
    return valid(action) ? g_button[action] : -1;
}

int bluewake_action_default_button(BWAction action) { return valid(action) ? kActions[action].button : -1; }

void bluewake_action_set_button(BWAction action, int button) {
    ensure_loaded();
    if (valid(action))
        g_button[action] = button >= 0 && button < SDL_GAMEPAD_BUTTON_COUNT ? button : -1;
}

int bluewake_action_key(BWAction action) {
    ensure_loaded();
    return valid(action) ? g_key[action] : 0;
}

int bluewake_action_default_key(BWAction action) { return valid(action) ? kActions[action].key : 0; }

void bluewake_action_set_key(BWAction action, int scancode) {
    ensure_loaded();
    if (valid(action))
        g_key[action] = scancode > 0 && scancode < SDL_SCANCODE_COUNT ? scancode : 0;
}

void bluewake_actions_reset(void) { defaults(); }

// Either Shift sprints while the key is the left one (the default).
static bool key_matches(BWAction action, int scancode) {
    const int key = g_key[action];
    return key != 0 && (scancode == key || (key == SDL_SCANCODE_LSHIFT && scancode == SDL_SCANCODE_RSHIFT));
}

static bool is_gamecube(SDL_Gamepad* pad) {
    return SDL_GetGamepadVendor(pad) == 0x057Eu &&
           (SDL_GetGamepadProduct(pad) == 0x0337u || SDL_GetGamepadProduct(pad) == 0x2073u);
}

// The action may use this controller's button (see the GameCube note in the header).
static bool usable(SDL_Gamepad* pad, int button) {
    if (button < 0)
        return false;
    if (!is_gamecube(pad))
        return true;
    return g_game_uses != NULL && !g_game_uses(button);
}

bool bluewake_action_pad_down(BWAction action) {
    ensure_loaded();
    if (!valid(action))
        return false;
    SDL_Gamepad* pad = bluewake_gamepads_player_pad();
    const int button = g_button[action];
    return pad != NULL && usable(pad, button) && SDL_GetGamepadButton(pad, (SDL_GamepadButton)button);
}

bool bluewake_action_key_down(BWAction action) {
    ensure_loaded();
    if (!valid(action) || g_key[action] == 0)
        return false;
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    if (keys == NULL)
        return false;
    const int key = g_key[action];
    if (key < count && keys[key])
        return true;
    return key == SDL_SCANCODE_LSHIFT && SDL_SCANCODE_RSHIFT < count && keys[SDL_SCANCODE_RSHIFT];
}

bool bluewake_action_down(BWAction action) {
    return bluewake_action_pad_down(action) || bluewake_action_key_down(action);
}

bool bluewake_action_pressed_by(BWAction action, const void* sdl_event) {
    ensure_loaded();
    const SDL_Event* event = (const SDL_Event*)sdl_event;
    if (!valid(action) || event == NULL)
        return false;
    if (event->type == SDL_EVENT_KEY_DOWN)
        return !event->key.repeat && key_matches(action, event->key.scancode);
    if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        if (g_button[action] < 0 || event->gbutton.button != g_button[action])
            return false;
        SDL_Gamepad* player = bluewake_gamepads_player_pad();
        return player != NULL && SDL_GetGamepadID(player) == event->gbutton.which &&
               usable(player, g_button[action]);
    }
    return false;
}

static void append(char* out, size_t size, size_t* used, const char* name, const char* value) {
    const int n = snprintf(out + *used, size - *used, "%s%s=%s", *used > 0 ? "," : "", name, value);
    if (n > 0 && *used + (size_t)n < size)
        *used += (size_t)n;
}

void bluewake_actions_format_buttons(char* out, size_t size) {
    ensure_loaded();
    if (size == 0)
        return;
    out[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < BW_ACTION_COUNT; ++i) {
        const char* value = g_button[i] >= 0 ? SDL_GetGamepadStringForButton((SDL_GamepadButton)g_button[i]) : NULL;
        append(out, size, &used, kActions[i].name, value != NULL ? value : "none");
    }
}

void bluewake_actions_format_keys(char* out, size_t size) {
    ensure_loaded();
    if (size == 0)
        return;
    out[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < BW_ACTION_COUNT; ++i) {
        const char* value = g_key[i] != 0 ? SDL_GetScancodeName((SDL_Scancode)g_key[i]) : NULL;
        append(out, size, &used, kActions[i].name, value != NULL && value[0] != '\0' ? value : "none");
    }
}

static int action_named(const char* name, size_t length) {
    for (int i = 0; i < BW_ACTION_COUNT; ++i)
        if (strlen(kActions[i].name) == length && strncmp(kActions[i].name, name, length) == 0)
            return i;
    return -1;
}

// name=value,name=value: `apply` takes each pair and says whether it understood it.
static bool parse(const char* text, bool (*apply)(int action, const char* value)) {
    bool ok = true;
    const char* p = text;
    while (p != NULL && *p != '\0') {
        const char* end = strchr(p, ',');
        const size_t length = end != NULL ? (size_t)(end - p) : strlen(p);
        const char* equals = memchr(p, '=', length);
        char value[64];
        int action = -1;
        if (equals != NULL) {
            const size_t value_length = length - (size_t)(equals + 1 - p);
            action = action_named(p, (size_t)(equals - p));
            if (value_length < sizeof value) {
                memcpy(value, equals + 1, value_length);
                value[value_length] = '\0';
            } else {
                action = -1;
            }
        }
        if (action < 0 || !apply(action, value))
            ok = false;
        p = end != NULL ? end + 1 : NULL;
    }
    return ok;
}

static bool apply_button(int action, const char* value) {
    if (strcmp(value, "none") == 0) {
        g_button[action] = -1;
        return true;
    }
    const SDL_GamepadButton button = SDL_GetGamepadButtonFromString(value);
    if (button == SDL_GAMEPAD_BUTTON_INVALID)
        return false;
    g_button[action] = button;
    return true;
}

static bool apply_key(int action, const char* value) {
    if (strcmp(value, "none") == 0) {
        g_key[action] = 0;
        return true;
    }
    const SDL_Scancode key = SDL_GetScancodeFromName(value);
    if (key == SDL_SCANCODE_UNKNOWN)
        return false;
    g_key[action] = key;
    return true;
}

bool bluewake_actions_parse_buttons(const char* text) {
    if (!g_loaded)
        defaults();
    return parse(text, apply_button);
}

bool bluewake_actions_parse_keys(const char* text) {
    if (!g_loaded)
        defaults();
    return parse(text, apply_key);
}

void bluewake_actions_set_game_button_check(bool (*uses)(int button)) { g_game_uses = uses; }
