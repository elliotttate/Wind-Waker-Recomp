#include "haptics.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE

// iOS: the game's own motor bits drive the device's rumble, as before.
void bluewake_haptics_attach(CPUState* cpu) { (void)cpu; }
void bluewake_haptics_retrace(void) {}
void bluewake_haptics_reload(void) {}
void bluewake_haptics_block(bool blocked) { (void)blocked; }
bool bluewake_haptics_forward_motor(void) { return true; }

#else

#include "gxruntime/platform.h"

#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// dVibration_c (tww d_vibration.h), the play info's mVibration:
// g_dComIfG_gameInfo (0x803C4C08) + play (0x12A0) + 0x4700. The game builds
// it after the host attaches: it is looked for (its vtable) each retrace until
// it is there, and nothing else is read before.
#define kVibration 0x803CA5A8u
#define kVibrationVtable 0x8037D460u // __vt__12dVibration_c
enum {
    kMotorShock = 0x48, // mMotor.mShock: { patternIdx, pattern, length, rounds, currentFrame, stopFrame }
    kMotorQuake = 0x60, // mMotor.mQuake, the same
    kPatternIdx = 0x00,
    kPattern = 0x04,
    kLength = 0x08,
    kRounds = 0x0C,
    kCurrentFrame = 0x10,
    kFrameIdx = 0x78,    // one a Run, one a game frame
    kRumbleState = 0x7C, // -1 paused (dVibration_c::Pause)
    kVtable = 0x80,
};
// The patterns StartShock and StartQuake take (for the test hook): { u16
// rounds, u16 length, u32 pattern }; a motor quake takes its rounds from the
// camera's table.
#define kMotorShockPatterns 0x80352FE0u  // MS_patt[26]
#define kMotorQuakePatterns 0x80353180u  // MQ_patt[12]
#define kCameraQuakePatterns 0x803531E0u // CQ_patt[12]

typedef enum { MODE_OFF, MODE_CLASSIC, MODE_ENHANCED } Mode;

static CPUState* g_cpu;
static bool g_found; // the vibration object is where it should be
static unsigned long long g_looked;
static Mode g_mode = MODE_ENHANCED;
static double g_strength = 0.8;
static bool g_triggers = true;
static bool g_trace;
static bool g_trace_raw; // BLUEWAKE_HAPTICS_TRACE=2: the object's motor half each retrace
static bool g_blocked;
static unsigned long long g_retrace;

// The game's vibration as last read.
static s32 g_frame_idx;
static bool g_frame_seen;
static Uint64 g_advanced_ns;
static s32 g_shock_idx = -1, g_shock_frame = -99;
static s32 g_quake_idx = -1;
static bool g_quake_on;

// The shock being felt: its bits from game frame `start`.
static struct {
    bool on;
    u32 pattern;
    s32 length;
    s32 start;
    double amplitude;
} g_shock;

// Motor levels, 0 to 1, after the release.
static double g_heavy, g_light, g_trigger;

// What was last sent, so a command goes out when a level changes and again
// before the last one expires.
#define kExpiryMs 120u
#define kRefreshNs 50000000ull
static Uint16 g_sent_low, g_sent_high, g_sent_trigger;
static Uint64 g_sent_ns;
static bool g_sent_any;

// A DualSense's trigger vibration, per controller: on while the trigger level
// is. Its effect does not expire as rumble does, so a timer on SDL's own thread
// clears it should the game thread stop sending: each command re-arms it (they
// come at least every 50 ms while it is on), and one that went off is sent
// again.
#define kMaxPads 8
#define kWatchdogMs 250u
static struct {
    SDL_JoystickID id;
    int amplitude; // 1-8 while on, 0 off
    SDL_TimerID watchdog;
    SDL_AtomicInt cleared; // the watchdog turned it off
} g_dualsense[kMaxPads];

// --- settings -----------------------------------------------------------------

static bool env_is(const char* name, char value) {
    const char* text = getenv(name);
    return text != NULL && text[0] == value;
}

static void read_settings(void) {
    const char* mode = getenv("BLUEWAKE_HAPTICS");
    g_mode = mode == NULL || mode[0] == '\0' || mode[0] == 'e' || mode[0] == '1' ? MODE_ENHANCED
             : mode[0] == 'c'                                                   ? MODE_CLASSIC
                                                                                : MODE_OFF;
    const char* strength = getenv("BLUEWAKE_HAPTICS_STRENGTH");
    const double percent = strength != NULL && strength[0] != '\0' ? atof(strength) : 80.0;
    g_strength = percent < 0.0 ? 0.0 : percent > 100.0 ? 1.0 : percent / 100.0;
    g_triggers = !env_is("BLUEWAKE_HAPTICS_TRIGGERS", '0');
    g_trace = env_is("BLUEWAKE_HAPTICS_TRACE", '1') || env_is("BLUEWAKE_HAPTICS_TRACE", '2');
    g_trace_raw = env_is("BLUEWAKE_HAPTICS_TRACE", '2');
}

// Classic, and Enhanced until the vibration object is found (on a disc this
// was not written for, never): the game's own on-off motor.
bool bluewake_haptics_forward_motor(void) { return g_mode == MODE_CLASSIC || (g_mode == MODE_ENHANCED && !g_found); }

void bluewake_haptics_block(bool blocked) { g_blocked = blocked; }

// --- output ---------------------------------------------------------------------

// The DualSense's output report effects (SDL's testcontroller.c, DS5EffectsState_t):
// byte 0 the enable bits (0x04 the right trigger's effect, 0x08 the left's),
// 10 the right trigger's 11 bytes, 21 the left's.
#define kDS5EffectsSize 47
static void dualsense_trigger_bytes(Uint8 effect[11], int amplitude) {
    memset(effect, 0, 11);
    if (amplitude <= 0) {
        effect[0] = 0x05; // off
        return;
    }
    // Vibration (0x26) from the trigger's rest to its end: ten zones of its
    // travel, each on with a strength of 0-7, at a frequency in Hz.
    const unsigned strength = (unsigned)(amplitude - 1) & 7u;
    unsigned zones = 0, strengths = 0;
    for (int i = 0; i < 10; i++) {
        zones |= 1u << i;
        strengths |= strength << (3 * i);
    }
    effect[0] = 0x26;
    effect[1] = (Uint8)zones;
    effect[2] = (Uint8)(zones >> 8);
    effect[3] = (Uint8)strengths;
    effect[4] = (Uint8)(strengths >> 8);
    effect[5] = (Uint8)(strengths >> 16);
    effect[6] = (Uint8)(strengths >> 24);
    effect[9] = 45; // Hz: a buzz in the trigger, below the motors' rumble
}

static bool dualsense_send(SDL_Gamepad* pad, int amplitude) {
    Uint8 effects[kDS5EffectsSize] = {0};
    effects[0] = 0x04 | 0x08;
    dualsense_trigger_bytes(&effects[10], amplitude);
    dualsense_trigger_bytes(&effects[21], amplitude);
    return SDL_SendGamepadEffect(pad, effects, sizeof effects);
}

static Uint32 SDLCALL dualsense_watchdog(void* userdata, SDL_TimerID timer, Uint32 interval) {
    (void)timer;
    (void)interval;
    const int slot = (int)(uintptr_t)userdata;
    SDL_Gamepad* pad = SDL_GetGamepadFromID(g_dualsense[slot].id);
    if (pad != NULL)
        dualsense_send(pad, 0);
    SDL_SetAtomicInt(&g_dualsense[slot].cleared, 1);
    return 0; // once
}

static void dualsense_triggers(SDL_Gamepad* pad, SDL_JoystickID id, double level) {
    int slot = -1;
    for (int i = 0; i < kMaxPads; i++)
        if (g_dualsense[i].id == id || (slot < 0 && g_dualsense[i].id == 0))
            slot = i;
    if (slot < 0)
        return;
    g_dualsense[slot].id = id;
    if (g_dualsense[slot].watchdog != 0) {
        SDL_RemoveTimer(g_dualsense[slot].watchdog);
        g_dualsense[slot].watchdog = 0;
    }
    if (SDL_GetAtomicInt(&g_dualsense[slot].cleared) != 0) {
        SDL_SetAtomicInt(&g_dualsense[slot].cleared, 0);
        g_dualsense[slot].amplitude = 0;
    }
    // Amplitude in steps of 1-8; a change of a step or more is sent.
    const int amplitude = level < 0.05 ? 0 : 1 + (int)lround(level * 7.0);
    if (amplitude != g_dualsense[slot].amplitude) {
        if (!dualsense_send(pad, amplitude))
            return; // not over HIDAPI (no effect reports): its rumble still works
        g_dualsense[slot].amplitude = amplitude;
    }
    if (amplitude > 0)
        g_dualsense[slot].watchdog = SDL_AddTimer(kWatchdogMs, dualsense_watchdog, (void*)(uintptr_t)slot);
}

// Every open controller gets the same levels (0-1, before strength).
static void send(double heavy, double light, double trigger, Uint64 now) {
    const Uint16 low = (Uint16)lround(fmin(heavy * g_strength, 1.0) * 65535.0);
    const Uint16 high = (Uint16)lround(fmin(light * g_strength, 1.0) * 65535.0);
    const Uint16 trig = g_triggers ? (Uint16)lround(fmin(trigger * g_strength, 1.0) * 65535.0) : 0;
    const bool quiet = low == 0 && high == 0 && trig == 0;
    const bool changed = low != g_sent_low || high != g_sent_high || trig != g_sent_trigger;
    if (!changed && (quiet || now - g_sent_ns < kRefreshNs))
        return;
    if (quiet && !g_sent_any)
        return;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids != NULL && i < count; i++) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
        if (pad == NULL)
            continue;
        SDL_RumbleGamepad(pad, low, high, kExpiryMs);
        const SDL_PropertiesID props = SDL_GetGamepadProperties(pad);
        if (SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false))
            SDL_RumbleGamepadTriggers(pad, trig, trig, kExpiryMs);
        else if (SDL_GetGamepadType(pad) == SDL_GAMEPAD_TYPE_PS5)
            dualsense_triggers(pad, ids[i], trig / 65535.0);
    }
    SDL_free(ids);
    if (g_trace && changed)
        fprintf(stderr, "[haptics-out] retrace=%llu heavy=%.2f light=%.2f trigger=%.2f\n", g_retrace,
                low / 65535.0, high / 65535.0, trig / 65535.0);
    g_sent_low = low;
    g_sent_high = high;
    g_sent_trigger = trig;
    g_sent_ns = now;
    g_sent_any = !quiet;
}

static void silence(Uint64 now) {
    g_heavy = g_light = g_trigger = 0.0;
    g_shock.on = false;
    send(0.0, 0.0, 0.0, now);
}

// --- testing ---------------------------------------------------------------------

typedef struct {
    unsigned long long retrace;
    char kind; // 's' shock, 'q' quake (stopped after `length` retraces)
    int index;
    unsigned long long length;
} TestVibration;
static TestVibration g_test[32];
static unsigned g_test_count;

static void read_tests(void) {
    g_test_count = 0;
    const char* text = getenv("BLUEWAKE_HAPTICS_TEST");
    for (const char* p = text; p != NULL && *p != '\0' && g_test_count < 32u;) {
        TestVibration test = {0};
        char kind[8] = {0};
        int used = 0;
        if (sscanf(p, "%llu:%7[a-z]:%d%n", &test.retrace, kind, &test.index, &used) != 3)
            break;
        p += used;
        if (*p == ':' && sscanf(p, ":%llu%n", &test.length, &used) == 1)
            p += used;
        test.kind = kind[0];
        g_test[g_test_count++] = test;
        if (*p == ',')
            p++;
    }
}

// As StartShock(i, 1, ...) and StartQuake(i, 1, ...) set the motor's half of
// the object, and StopQuake(1) clears it; the game's Run plays them.
static void run_tests(void) {
    for (unsigned i = 0; i < g_test_count; i++) {
        const TestVibration* test = &g_test[i];
        const u32 base = kVibration;
        if (g_retrace == test->retrace && test->kind == 's' && test->index > 0 && test->index < 26) {
            const u32 entry = kMotorShockPatterns + 8u * (u32)test->index;
            mem_write32(g_cpu, base + kMotorShock + kPatternIdx, (u32)test->index);
            mem_write32(g_cpu, base + kMotorShock + kCurrentFrame, 0u);
            mem_write32(g_cpu, base + kMotorShock + kPattern, mem_read32(g_cpu, entry + 4u));
            mem_write32(g_cpu, base + kMotorShock + kLength, mem_read32(g_cpu, entry) & 0xFFFFu);
            fprintf(stderr, "[haptics] test: shock %d at retrace %llu\n", test->index, g_retrace);
        } else if (g_retrace == test->retrace && test->kind == 'q' && test->index > 0 && test->index < 12) {
            const u32 entry = kMotorQuakePatterns + 8u * (u32)test->index;
            mem_write32(g_cpu, base + kMotorQuake + kPatternIdx, (u32)test->index);
            mem_write32(g_cpu, base + kMotorQuake + kCurrentFrame, 0u);
            mem_write32(g_cpu, base + kMotorQuake + kPattern, mem_read32(g_cpu, entry + 4u));
            mem_write32(g_cpu, base + kMotorQuake + kLength, mem_read32(g_cpu, entry) & 0xFFFFu);
            mem_write32(g_cpu, base + kMotorQuake + kRounds,
                        mem_read32(g_cpu, kCameraQuakePatterns + 8u * (u32)test->index) >> 16);
            fprintf(stderr, "[haptics] test: quake %d at retrace %llu\n", test->index, g_retrace);
        } else if (test->kind == 'q' && test->length > 0 && g_retrace == test->retrace + test->length &&
                   (s32)mem_read32(g_cpu, base + kMotorQuake + kPatternIdx) != -1) {
            mem_write32(g_cpu, base + kMotorQuake + kPatternIdx, (u32)-1);
            mem_write32(g_cpu, base + kMotorQuake + kCurrentFrame, 0u);
            fprintf(stderr, "[haptics] test: quake stopped at retrace %llu\n", g_retrace);
        }
    }
}

// BLUEWAKE_HAPTICS_VIRTUAL: a controller that logs what it is sent.
static bool SDLCALL virtual_rumble(void* userdata, Uint16 low, Uint16 high) {
    (void)userdata;
    fprintf(stderr, "[haptics-virtual] retrace=%llu rumble low=%u high=%u\n", g_retrace, low, high);
    return true;
}

static bool SDLCALL virtual_trigger_rumble(void* userdata, Uint16 left, Uint16 right) {
    (void)userdata;
    fprintf(stderr, "[haptics-virtual] retrace=%llu triggers left=%u right=%u\n", g_retrace, left, right);
    return true;
}

static bool SDLCALL virtual_effect(void* userdata, const void* data, int size) {
    (void)userdata;
    const Uint8* bytes = (const Uint8*)data;
    if (size >= 32)
        fprintf(stderr,
                "[haptics-virtual] retrace=%llu effect enable=0x%02X right=%02X %02X%02X %02X%02X%02X%02X f=%u "
                "left=%02X\n",
                g_retrace, bytes[0], bytes[10], bytes[12], bytes[11], bytes[16], bytes[15], bytes[14], bytes[13],
                bytes[19], bytes[21]);
    return true;
}

static void attach_virtual(const char* kind) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.Rumble = virtual_rumble;
    if (kind[0] == 'p') {
        desc.vendor_id = 0x054C; // Sony DualSense
        desc.product_id = 0x0CE6;
        desc.name = "BlueWake test DualSense";
        desc.SendEffect = virtual_effect;
    } else {
        desc.vendor_id = 0x045E; // Microsoft Xbox Series
        desc.product_id = 0x0B12;
        desc.name = "BlueWake test Xbox controller";
        desc.RumbleTriggers = virtual_trigger_rumble;
    }
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    SDL_Gamepad* pad = id != 0 ? SDL_OpenGamepad(id) : NULL;
    fprintf(stderr, "[haptics] test: virtual %s %s (%s)\n", desc.name, pad != NULL ? "attached" : "failed",
            pad != NULL ? (SDL_GetGamepadType(pad) == SDL_GAMEPAD_TYPE_PS5 ? "PS5" : "Xbox") : SDL_GetError());
}

// --- the game's vibration ----------------------------------------------------------

static int bits_on(u32 pattern, s32 length) {
    int on = 0;
    for (s32 k = 0; k < length && k < 32; k++)
        on += (int)((pattern >> (31 - k)) & 1u);
    return on;
}

static bool pattern_bit(u32 pattern, s32 length, s32 k) {
    return k >= 0 && k < length && k < 32 && ((pattern >> (31 - k)) & 1u) != 0u;
}

void bluewake_haptics_attach(CPUState* cpu) {
    g_cpu = cpu;
    read_settings();
    read_tests();
    const char* virtual_kind = getenv("BLUEWAKE_HAPTICS_VIRTUAL");
    if (virtual_kind != NULL && virtual_kind[0] != '\0')
        attach_virtual(virtual_kind);
    fprintf(stderr, "[haptics] %s, strength %.0f%%, triggers %s\n",
            g_mode == MODE_ENHANCED ? "enhanced"
            : g_mode == MODE_CLASSIC ? "classic (the game's own on and off)"
                                     : "off",
            g_strength * 100.0, g_triggers ? "on" : "off");
}

void bluewake_haptics_reload(void) {
    const bool forwarded = bluewake_haptics_forward_motor();
    read_settings();
    if (forwarded && !bluewake_haptics_forward_motor()) {
        for (u32 channel = 0; channel < 4u; channel++)
            dol_platform_pad_control_motor(channel, 2u); // PAD_MOTOR_STOP_HARD
    }
    if (g_mode != MODE_ENHANCED)
        silence(SDL_GetTicksNS());
}

// Whether the game has the keyboard focus (or runs without a window). A test
// run feels nothing otherwise, so a scripted one ignores it.
static bool focused(void) {
    if (g_test_count > 0u)
        return true;
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    SDL_free(windows);
    return count == 0 || SDL_GetKeyboardFocus() != NULL;
}

void bluewake_haptics_retrace(void) {
    g_retrace++;
    if (g_cpu == NULL)
        return;
    if (!g_found) {
        // Twice a second until the game has built it (and ten minutes at most).
        if (g_retrace % 30u != 0u || g_looked > 1200u)
            return;
        g_looked++;
        if (mem_read32(g_cpu, kVibration + kVtable) != kVibrationVtable)
            return;
        g_found = true;
        fprintf(stderr, "[haptics] the game's vibration found at retrace %llu\n", g_retrace);
    }
    if (g_test_count > 0u)
        run_tests();
    const Uint64 now = SDL_GetTicksNS();
    if (g_mode != MODE_ENHANCED) {
        if (g_sent_any)
            silence(now);
        return;
    }
    const u32 base = kVibration;
    const s32 frame_idx = (s32)mem_read32(g_cpu, base + kFrameIdx);
    if (!g_frame_seen || frame_idx != g_frame_idx) {
        if (g_frame_seen && frame_idx < g_frame_idx)
            g_shock.on = false; // setDefault: a new scene
        g_frame_idx = frame_idx;
        g_frame_seen = true;
        g_advanced_ns = now;
    }
    const bool paused = (s32)mem_read32(g_cpu, base + kRumbleState) == -1;
    if (g_trace_raw)
        // JUTGamePad::sRumbleSupported (0x803F7878) and CRumble::mEnabled
        // (0x803F7880): the game plays the motor only when both have port 0.
        fprintf(stderr,
                "[haptics-raw] retrace=%llu frame=%d state=%d shock=%d@%d quake=%d@%d supported=%08X enabled=%08X\n",
                g_retrace, frame_idx, (s32)mem_read32(g_cpu, base + kRumbleState),
                (s32)mem_read32(g_cpu, base + kMotorShock + kPatternIdx),
                (s32)mem_read32(g_cpu, base + kMotorShock + kCurrentFrame),
                (s32)mem_read32(g_cpu, base + kMotorQuake + kPatternIdx),
                (s32)mem_read32(g_cpu, base + kMotorQuake + kCurrentFrame), mem_read32(g_cpu, 0x803F7878u),
                mem_read32(g_cpu, 0x803F7880u));
    const bool stalled = now - g_advanced_ns > 150000000ull;

    // A shock: StartShock sets its frame to 0, and each Run counts it up to
    // its length and clears it. A new one is a shock that appears, another
    // pattern, or a count that went back.
    const s32 shock_idx = (s32)mem_read32(g_cpu, base + kMotorShock + kPatternIdx);
    const s32 shock_frame = (s32)mem_read32(g_cpu, base + kMotorShock + kCurrentFrame);
    if (shock_idx >= 0 && shock_frame >= 0 &&
        (g_shock_idx < 0 || shock_idx != g_shock_idx || shock_frame < g_shock_frame)) {
        const u32 pattern = mem_read32(g_cpu, base + kMotorShock + kPattern);
        const s32 length = (s32)mem_read32(g_cpu, base + kMotorShock + kLength);
        if (length > 0 && length <= 32) {
            const int on = bits_on(pattern, length);
            g_shock.on = true;
            g_shock.pattern = pattern;
            g_shock.length = length;
            g_shock.start = frame_idx - (shock_frame > 0 ? shock_frame - 1 : -1);
            // 2 frames on (a tap) to 8 (a long blow): about half to full.
            g_shock.amplitude = fmin(fmax(0.4 + 0.075 * on, 0.5), 1.0);
            if (g_trace)
                fprintf(stderr, "[haptics] retrace=%llu shock %d: %d frames, %d on, amplitude %.2f\n", g_retrace,
                        shock_idx, length, on, g_shock.amplitude);
        }
    }
    g_shock_idx = shock_idx;
    g_shock_frame = shock_frame;

    // A quake: on from StartQuake until StopQuake (or the game's 900 frames).
    const s32 quake_idx = (s32)mem_read32(g_cpu, base + kMotorQuake + kPatternIdx);
    const s32 quake_frame = (s32)mem_read32(g_cpu, base + kMotorQuake + kCurrentFrame);
    const u32 quake_pattern = mem_read32(g_cpu, base + kMotorQuake + kPattern);
    const s32 quake_length = (s32)mem_read32(g_cpu, base + kMotorQuake + kLength);
    const bool quake = quake_idx >= 0 && quake_frame >= 0 && quake_length > 0 && quake_length <= 32;
    double quake_level = 0.0;
    if (quake) {
        const s32 rounds = (s32)mem_read32(g_cpu, base + kMotorQuake + kRounds);
        const double density =
            fmin((bits_on(quake_pattern, quake_length) + (rounds > 0 ? rounds : 0)) / (double)quake_length, 1.0);
        // 12 percent of frames on (the lightest) to 62 (the heaviest).
        quake_level = fmin(fmax(0.15 + 0.9 * density, 0.2), 0.75);
    }
    if (g_trace && (quake != g_quake_on || (quake && quake_idx != g_quake_idx)))
        fprintf(stderr, quake ? "[haptics] retrace=%llu quake %d: level %.2f\n" : "[haptics] retrace=%llu quake off\n",
                g_retrace, quake_idx, quake_level);
    g_quake_on = quake;
    g_quake_idx = quake_idx;

    if (paused || stalled || g_blocked || !focused()) {
        silence(now);
        return;
    }

    double heavy = 0.0, light = 0.0, trigger = 0.0;
    if (g_shock.on) {
        const s32 k = frame_idx - g_shock.start;
        if (k >= g_shock.length) {
            g_shock.on = false;
        } else if (pattern_bit(g_shock.pattern, g_shock.length, k)) {
            const double a = g_shock.amplitude;
            heavy = a;
            // The light motor sharp at a pulse's first frame, then under the heavy one.
            light = a * (pattern_bit(g_shock.pattern, g_shock.length, k - 1) ? 0.5 : 0.9);
            if (a >= 0.7)
                trigger = a - 0.3;
        }
    }
    if (quake) {
        const bool bit = pattern_bit(quake_pattern, quake_length, frame_idx % quake_length);
        heavy = fmax(heavy, quake_level * (bit ? 1.0 : 0.7));
        light = fmax(light, quake_level * 0.25);
        if (quake_level >= 0.5)
            trigger = fmax(trigger, quake_level * 0.3);
    }
    // A pulse rises at once and falls over about two retraces, so a pattern's
    // gaps are felt as gaps.
    g_heavy = fmax(heavy, g_heavy * 0.55);
    g_light = fmax(light, g_light * 0.45);
    g_trigger = fmax(trigger, g_trigger * 0.5);
    if (g_heavy < 0.02)
        g_heavy = 0.0;
    if (g_light < 0.02)
        g_light = 0.0;
    if (g_trigger < 0.02)
        g_trigger = 0.0;
    send(g_heavy, g_light, g_trigger, now);
}

#endif
