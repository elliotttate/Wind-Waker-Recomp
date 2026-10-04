// The connected controllers and player 1's (gamepads.h).
#include "gamepads.h"

#include <SDL3/SDL.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { kMaxPads = 16, kNotices = 8 };

// A tilt or a trigger pulled this far is a person using the controller, not
// drift (SDL's axes, 0-32767).
static const int kInputAxis = 20000;

static BWGamepad g_pads[kMaxPads];
static bool g_owned[kMaxPads]; // opened here (tests); Aurora opens the app's
static int g_count;
static BWPort0Policy g_policy = BW_PORT0_AUTO;
static bool g_pause_on_disconnect = true;
static bool g_port0_lost;
static BWGamepadHooks g_hooks;

static char g_notices[kNotices][128];
static unsigned g_notice_head, g_notice_count;

static void notice(const char* format, ...) SDL_PRINTF_VARARG_FUNC(1);
static void notice(const char* format, ...) {
    char text[128];
    va_list args;
    va_start(args, format);
    SDL_vsnprintf(text, sizeof text, format, args);
    va_end(args);
    fprintf(stderr, "[pads] %s\n", text);
    if (g_notice_count == kNotices) { // the oldest goes
        g_notice_head = (g_notice_head + 1u) % kNotices;
        --g_notice_count;
    }
    SDL_strlcpy(g_notices[(g_notice_head + g_notice_count) % kNotices], text, sizeof g_notices[0]);
    ++g_notice_count;
}

static bool is_gamecube(unsigned short vendor, unsigned short product) {
    // The GameCube adapter (four ports, one device each) and the Nintendo
    // Switch Online GameCube controller.
    return vendor == 0x057Eu && (product == 0x0337u || product == 0x2073u);
}

static int find(unsigned id) {
    for (int i = 0; i < g_count; ++i)
        if (g_pads[i].id == id)
            return i;
    return -1;
}

// A controller already closed (Aurora closes it before the host sees its
// removal) keeps the port it last had, so its removal knows it was player 1.
static void refresh_ports(void) {
    for (int i = 0; i < g_count; ++i) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(g_pads[i].id);
        if (pad != NULL)
            g_pads[i].port = SDL_GetGamepadPlayerIndex(pad);
    }
}

static void add(unsigned id) {
    if (find(id) >= 0 || g_count == kMaxPads)
        return;
    SDL_Gamepad* pad = SDL_GetGamepadFromID(id);
    bool owned = false;
    if (pad == NULL) {
        pad = SDL_OpenGamepad(id);
        owned = pad != NULL;
    }
    if (pad == NULL)
        return;
    BWGamepad* entry = &g_pads[g_count];
    memset(entry, 0, sizeof *entry);
    entry->id = id;
    entry->vendor = SDL_GetGamepadVendor(pad);
    entry->product = SDL_GetGamepadProduct(pad);
    if (entry->vendor == 0x05ACu && entry->product == 3u) { // an Apple TV remote (Aurora ignores it too)
        if (owned)
            SDL_CloseGamepad(pad);
        return;
    }
    entry->type = (int)SDL_GetGamepadType(pad);
    entry->gamecube = is_gamecube(entry->vendor, entry->product);
    entry->port = SDL_GetGamepadPlayerIndex(pad);
    entry->connected_ms = SDL_GetTicks();
    const char* name = SDL_GetGamepadName(pad);
    SDL_strlcpy(entry->name, name != NULL ? name : "Controller", sizeof entry->name);
    g_owned[g_count] = owned;
    ++g_count;
    notice("%s connected", entry->name);
}

static void remove_at(int index) {
    const BWGamepad gone = g_pads[index];
    if (g_owned[index]) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(gone.id);
        if (pad != NULL)
            SDL_CloseGamepad(pad);
    }
    for (int i = index; i + 1 < g_count; ++i) {
        g_pads[i] = g_pads[i + 1];
        g_owned[i] = g_owned[i + 1];
    }
    --g_count;
    notice("%s disconnected", gone.name);
    if (gone.port == 0 && g_pause_on_disconnect)
        g_port0_lost = true;
}

// `entry` takes `port`; the controller that had it takes `entry`'s old one.
static void assign(BWGamepad* entry, int port) {
    SDL_Gamepad* pad = SDL_GetGamepadFromID(entry->id);
    if (pad == NULL || entry->port == port)
        return;
    const int old = entry->port;
    for (int i = 0; i < g_count; ++i) {
        if (&g_pads[i] != entry && g_pads[i].port == port) {
            SDL_Gamepad* other = SDL_GetGamepadFromID(g_pads[i].id);
            if (other != NULL)
                SDL_SetGamepadPlayerIndex(other, old);
            g_pads[i].port = old;
        }
    }
    SDL_SetGamepadPlayerIndex(pad, port);
    entry->port = port;
    if (g_hooks.handover != NULL && port == 0)
        g_hooks.handover();
    if (port == 0)
        notice("%s: player 1", entry->name);
}

// Port 0 empty: the controller used last, else the one connected last.
static void settle(void) {
    refresh_ports();
    if (g_policy == BW_PORT0_FIXED)
        return;
    BWGamepad* best = NULL;
    for (int i = 0; i < g_count; ++i) {
        BWGamepad* entry = &g_pads[i];
        if (entry->port == 0)
            return;
        if (best == NULL || entry->last_input_ms > best->last_input_ms ||
            (entry->last_input_ms == best->last_input_ms && entry->connected_ms >= best->connected_ms))
            best = entry;
    }
    if (best != NULL)
        assign(best, 0);
}

static void note_input(unsigned id) {
    const int index = find(id);
    if (index < 0)
        return;
    BWGamepad* entry = &g_pads[index];
    entry->last_input_ms = SDL_GetTicks();
    if (g_policy == BW_PORT0_LAST && entry->port != 0)
        assign(entry, 0);
}

void bluewake_gamepads_reload(void) {
    const char* policy = getenv("BLUEWAKE_PAD_PORT0");
    g_policy = policy == NULL || policy[0] == '\0' || policy[0] == 'a' ? BW_PORT0_AUTO
               : policy[0] == 'l'                                       ? BW_PORT0_LAST
                                                                        : BW_PORT0_FIXED;
    const char* pause = getenv("BLUEWAKE_PAD_PAUSE_ON_DISCONNECT");
    g_pause_on_disconnect = pause == NULL || pause[0] != '0';
    settle();
}

void bluewake_gamepads_event(const void* sdl_event) {
    const SDL_Event* event = (const SDL_Event*)sdl_event;
    switch (event->type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        add(event->gdevice.which);
        settle();
        break;
    case SDL_EVENT_GAMEPAD_REMOVED: {
        const int index = find(event->gdevice.which);
        if (index >= 0)
            remove_at(index);
        settle();
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        note_input(event->gbutton.which);
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (event->gaxis.value >= kInputAxis || event->gaxis.value <= -kInputAxis)
            note_input(event->gaxis.which);
        break;
    default:
        break;
    }
}

void bluewake_gamepads_sync(void) {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = g_count - 1; i >= 0; --i) {
        bool present = false;
        for (int k = 0; ids != NULL && k < count && !present; ++k)
            present = ids[k] == g_pads[i].id;
        if (!present)
            remove_at(i);
    }
    for (int k = 0; ids != NULL && k < count; ++k)
        add(ids[k]);
    SDL_free(ids);
    settle();
}

int bluewake_gamepads_count(void) { return g_count; }

const BWGamepad* bluewake_gamepads_at(int index) {
    return index >= 0 && index < g_count ? &g_pads[index] : NULL;
}

const BWGamepad* bluewake_gamepads_find(unsigned id) {
    const int index = find(id);
    return index >= 0 ? &g_pads[index] : NULL;
}

SDL_Gamepad* bluewake_gamepads_for_port(unsigned port) {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    SDL_Gamepad* found = NULL;
    for (int i = 0; ids != NULL && i < count && found == NULL; ++i) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
        if (pad != NULL && SDL_GetGamepadPlayerIndex(pad) == (int)port)
            found = pad;
    }
    SDL_free(ids);
    return found;
}

SDL_Gamepad* bluewake_gamepads_player_pad(void) {
    SDL_Gamepad* pad = bluewake_gamepads_for_port(0);
    if (pad != NULL)
        return pad;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids != NULL && i < count && pad == NULL; ++i)
        pad = SDL_GetGamepadFromID(ids[i]);
    SDL_free(ids);
    return pad;
}

BWPort0Policy bluewake_gamepads_policy(void) { return g_policy; }

void bluewake_gamepads_use_for_port0(unsigned id) {
    const int index = find(id);
    if (index < 0)
        return;
    g_pads[index].last_input_ms = SDL_GetTicks();
    if (g_policy == BW_PORT0_FIXED && g_hooks.persist != NULL) {
        g_hooks.persist(id);
        refresh_ports();
        if (g_hooks.handover != NULL)
            g_hooks.handover();
        notice("%s: player 1", g_pads[index].name);
    } else {
        refresh_ports();
        assign(&g_pads[index], 0);
    }
}

bool bluewake_gamepads_take_port0_lost(void) {
    const bool lost = g_port0_lost;
    g_port0_lost = false;
    return lost;
}

bool bluewake_gamepads_take_notice(char* text, size_t size) {
    if (g_notice_count == 0u)
        return false;
    SDL_strlcpy(text, g_notices[g_notice_head], size);
    g_notice_head = (g_notice_head + 1u) % kNotices;
    --g_notice_count;
    return true;
}

void bluewake_gamepads_set_hooks(const BWGamepadHooks* hooks) {
    if (hooks != NULL)
        g_hooks = *hooks;
    else
        memset(&g_hooks, 0, sizeof g_hooks);
}
