#include "sprint.h"
#include "input_bindings.h"
#include "pad_remap.h"
#include "gamepads.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Link's running speed is daPy_HIO_move_c0::m (GZLE01 0x8035CED4, the
// player's movement parameters): procMove and the move checks set
// mMaxNormalSpeed from its field 0x18 (17) whenever he runs forward, and the
// run animation plays at field 0x48 (2.3) at that speed (setMoveAnime blends by
// speed over mMaxNormalSpeed, so a faster top speed alone would slide his
// feet). Holding Shift scales both; letting go puts them back. Swimming, iron
// boots, targeting and carrying have their own parameters and are unchanged.
//
// On a controller the left stick's click (which the GameCube mapping leaves
// free) is the sprint: a click starts it, and it lasts until the stick comes
// back to the middle (Link stops) or the next click.
enum {
    kMoveParams = 0x8035CED4u,
    kMaxSpeed = kMoveParams + 0x18u,
    kRunAnimRate = kMoveParams + 0x48u,
    kPlayerPointer = 0x803CA74Cu,
    kSpeedF = 0x254u,          // fopAc_ac_c::speedF
    kNormalSpeed = 0x35BCu,    // daPy_lk_c::mNormalSpeed
    kMaxNormalSpeed = 0x35C0u, // daPy_lk_c::mMaxNormalSpeed
};

static CPUState* g_cpu;
static double g_factor = 1.5; // BLUEWAKE_SPRINT_SPEED; 1 or less: off
static bool g_sprinting;
static bool g_trace;
static float g_base_speed, g_base_rate;
static bool g_have_base;
static unsigned long long g_retrace;
// BLUEWAKE_SPRINT_TEST=retrace:length (testing only): Shift held meanwhile.
static unsigned long long g_test_start, g_test_length;
// The controller's sprint (left stick click).
static bool g_pad_sprint;
static bool g_stick_was_down;
static unsigned g_stick_idle; // retraces the left stick has been near the middle
static const unsigned kStickIdleRetraces = 8;
static const float kStickIdle = 0.25f; // of full tilt

static float read_f32(CPUState* cpu, u32 address) {
    const u32 bits = mem_read32(cpu, address);
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static void write_f32(CPUState* cpu, u32 address, float value) {
    u32 bits;
    memcpy(&bits, &value, sizeof bits);
    mem_write32(cpu, address, bits);
}

static void read_factor(void) {
    const char* factor = getenv("BLUEWAKE_SPRINT_SPEED");
    g_factor = factor != NULL && factor[0] != '\0' ? strtod(factor, NULL) : 1.5;
}

void bluewake_sprint_reload(void) {
    read_factor();
    // A sprint under way picks the new speed up at the next retrace.
    if (g_sprinting && g_cpu != NULL && g_have_base) {
        write_f32(g_cpu, kMaxSpeed, g_base_speed);
        write_f32(g_cpu, kRunAnimRate, g_base_rate);
        g_sprinting = false;
    }
}

void bluewake_sprint_attach(CPUState* cpu) {
    g_cpu = cpu;
    read_factor();
    const char* trace = getenv("BLUEWAKE_SPRINT_TRACE");
    g_trace = trace != NULL && trace[0] == '1';
    const char* test = getenv("BLUEWAKE_SPRINT_TEST");
    if (test != NULL && sscanf(test, "%llu:%llu", &g_test_start, &g_test_length) == 2)
        g_trace = true;
    if (g_factor > 1.0)
        fprintf(stderr, "[sprint] Shift (or a click of a controller's left stick) runs %.2fx faster\n", g_factor);
}

static bool shift_held(void) {
    if (g_retrace >= g_test_start && g_retrace < g_test_start + g_test_length)
        return true;
    return bluewake_action_key_down(BW_ACTION_SPRINT);
}

// Player 1's sprint button, and how far the controller's stick (the game's,
// through the mapping) is pushed (0..1). Without a player 1, any connected
// controller's left stick, the largest.
static void read_pads(bool* click, float* tilt) {
    *click = bluewake_action_pad_down(BW_ACTION_SPRINT);
    *tilt = 0.0f;
    double x, y;
    if (bluewake_padmap_stick(0, &x, &y)) {
        *tilt = (float)SDL_sqrt(x * x + y * y);
        return;
    }
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids != NULL && i < count; ++i) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
        if (pad == NULL)
            continue;
        const float x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
        const float y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
        const float t = x * x + y * y;
        if (t > *tilt)
            *tilt = t;
    }
    SDL_free(ids);
    *tilt = *tilt > 0.0f ? SDL_sqrtf(*tilt) : 0.0f;
}

// The controller's sprint: on at a click of the left stick, off at the next or
// once the stick has rested near the middle.
static bool pad_sprint(void) {
    bool click;
    float tilt;
    read_pads(&click, &tilt);
    if (click && !g_stick_was_down) {
        g_pad_sprint = !g_pad_sprint;
        g_stick_idle = 0;
    }
    g_stick_was_down = click;
    if (g_pad_sprint) {
        g_stick_idle = tilt < kStickIdle ? g_stick_idle + 1u : 0u;
        if (g_stick_idle >= kStickIdleRetraces)
            g_pad_sprint = false;
    }
    return g_pad_sprint;
}

void bluewake_sprint_retrace(void) {
    ++g_retrace;
    CPUState* cpu = g_cpu;
    if (cpu == NULL)
        return;
    if (g_factor <= 1.0) {
        g_pad_sprint = false;
        return;
    }
    if (!g_have_base) {
        // The parameters as the disc has them (once the game is loaded).
        const float speed = read_f32(cpu, kMaxSpeed), rate = read_f32(cpu, kRunAnimRate);
        if (!(speed > 1.0f && speed < 100.0f && rate > 0.1f && rate < 20.0f))
            return;
        g_base_speed = speed;
        g_base_rate = rate;
        g_have_base = true;
    }
    const bool pad = pad_sprint();
    const bool sprint = shift_held() || pad;
    if (sprint != g_sprinting) {
        g_sprinting = sprint;
        const float scale = sprint ? (float)g_factor : 1.0f;
        write_f32(cpu, kMaxSpeed, g_base_speed * scale);
        write_f32(cpu, kRunAnimRate, g_base_rate * scale);
        if (g_trace)
            fprintf(stderr, "[sprint] %s retrace=%llu top speed %.1f\n", sprint ? "on" : "off", g_retrace,
                    g_base_speed * scale);
    }
    if (g_trace && (g_retrace % 20u) == 0u) {
        const u32 player = mem_read32(cpu, kPlayerPointer);
        if (player >= 0x80000000u && player < 0x81800000u)
            fprintf(stderr, "[sprint] retrace=%llu speedF=%.2f normal=%.2f max=%.2f\n", g_retrace,
                    read_f32(cpu, player + kSpeedF), read_f32(cpu, player + kNormalSpeed),
                    read_f32(cpu, player + kMaxNormalSpeed));
    }
}
