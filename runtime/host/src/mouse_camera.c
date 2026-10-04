#include "mouse_camera.h"
#include "gamepads.h"
#include "input_bindings.h"
#include "pad_remap.h"
#include "jump_button.h"
#include "settings_menu.h"
#include "save_state.h"

#include "gxruntime/aurora_backend.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// The game's camera (GZLE01): dComIfGp_getCamera(0), the camera_process_class
// in g_dComIfG_gameInfo.play.mCameraInfo[0]. Its view (view_class) is at +0,
// with the eye, center and up the frame is drawn from (mLookat) at +0xD8, and
// its dCamera_c at +0x244. The camera's type routine (the follow camera,
// usually) eases dCamera_c::mViewCache every frame and bumpCheck makes the
// final eye from it (walls), which camera_execute copies to the view; then
// camera_draw makes the view matrix from mLookat.
enum {
    kCameraPointer = 0x803CA718u,
    kCameraDraw = BLUEWAKE_MOUSE_CAMERA_DRAW,
    kCameraBump = BLUEWAKE_MOUSE_CAMERA_BUMP,
    kLookatEye = 0xD8u,
    kLookatCenter = 0xE4u,
    kCameraAngleY = 0x232u, // camera_class::mAngle.y (fopCamM_GetAngleY)
    kCameraBody = 0x244u,
    kFinalRadius = 0x08u, // dCamera_c::mDirection (cSGlobe: radius, V, U)
    kFinalPitch = 0x0Cu,
    kFinalYaw = 0x0Eu,
    kFinalEye = 0x1Cu,    // dCamera_c::mEye
    kFinalFovy = 0x38u,   // dCamera_c::mFovy
    kViewRadius = 0x3Cu,  // dCamera_c::mViewCache.mDirection
    kViewPitch = 0x40u,
    kViewYaw = 0x42u,
    kViewCenter = 0x44u,  // mViewCache.mCenter
    kViewEye = 0x50u,     // mViewCache.mEye
    kReady = 0x100u,      // m100, m101, m102: the mode's entry is done
    kMode = 0x13Cu,       // dCamera_c::mCurMode
    kWaterHeight = 0x310u,  // mBG.m5C.m58 and m354: the heights the subject
    kGroundHeight = 0x354u, // camera keeps its eye 5 units above
    kFollowMinRadius = 0x398u, // mWork.follow.m398 / m39C: the follow
    kFollowMaxRadius = 0x39Cu, // camera's distance limits this frame
    kSubjectTag = 0x378u,   // mWork.subject.m378 == 'SUBJ' once subjectCamera ran
    kSubjectFree = 0x37Cu,  // m37C: the aim is the player's (not steered at a target)
    kSubjectPitch = 0x388u, // m388: pitch / p19, -1..1 (positive looks down)
    kSubjectZoom = 0x38Cu,  // m38C: telescope / Picto Box zoom, 0..1 (1x..9x)
    kSubjectStep = 0x3C4u,  // m3C4: the C-stick's push down in first person, 0, 1 (a little), 2 (out)
    kCameraStyle = 0x750u,  // mCamParam.mpStyle -> dCamParam_c::styles[mCurStyle]
    kEventMode = 0x803C9EA2u, // g_dComIfG_gameInfo.play.mEvtCtrl's mode
    kPlayerPointer = 0x803CA74Cu,
    kPlayerStatus0 = 0x803CA8D0u, // g_dComIfG_gameInfo.play.mPlayerStatus[0][0]
    kPlayerExecute = BLUEWAKE_MOUSE_PLAYER_EXECUTE, // r3: the player
    kPlayerAngleY = 0x206u, // fopAc_ac_c::current.angle.y
    kPlayerShapeY = 0x20Eu, // fopAc_ac_c::shape_angle.y
    kPlayerBodyX = 0x2B4u,  // daPy_py_c::mBodyAngle.x (the aim's pitch)
    kPlayerDemoMode = 0x314u,
    // daPy_lk_c::execute (USA) starts by putting back the shape_angle and
    // current.angle it kept at the end of the last update (l_debug_shape_angle,
    // l_debug_current_angle), so a turn made between updates has to go into
    // those too.
    kKeptAngleY = 0x803F6F12u, // l_debug_current_angle.y
    kKeptShapeY = 0x803F6F1Au, // l_debug_shape_angle.y
};

// dCamera__Style (dCamParam_c::styles, 0x84 bytes each): name, engine,
// 30 parameters, flags.
enum {
    kStyleEngine = 0x04u,
    kStyleParams = 0x08u,
    kStyleFlags = 0x80u,
    kEngineFollow = 1,
    kEngineSubject = 4, // subjectCamera: first person and every item's aim
    kEngineFollow2 = 18,
    kStyleZoom = 0x0010u, // dCamPrmFlg_UNK010: the view zooms (telescope, Picto Box)
};

enum {
    kSubjectTagValue = 0x5355424Au,   // 'SUBJ'
    kStatusHookshotOut = 0x00040000u, // daPyStts0_UNK40000_e: the aim is frozen
    kStatusCrawl = 0x08000000u,       // daPyStts0_CRAWL_e: no pitch
};

// Degrees a point of pointer travel turns the camera, at sensitivity 1, and
// how far the camera may tilt: from a little below Link, looking up, to high
// above, looking down (the game sits at about 6 degrees above).
static const double kDegreesPerPoint = 0.18;
static const double kPitchMin = -35.0, kPitchMax = 75.0;
static const double kAngleUnits = 65536.0 / 360.0;
// A notch of the wheel in third person scales the follow camera's distance
// by this much, within these limits; in the telescope and the Picto Box a
// notch is one step of their 1x-9x zoom.
static const double kZoomStep = 1.12, kZoomMin = 0.5, kZoomMax = 2.0;
static const double kScopeZoomPerNotch = 0.125;

static bool g_enabled;
static bool g_captured;
static bool g_click; // left button held while the mouse is the camera: A
static SDL_WindowID g_window;
static double g_sum_x, g_sum_y;
static double g_wheel; // notches, positive away from the player (zoom in)
static double g_sensitivity = 1.0;
static double g_invert_y = 1.0;
// The follow camera steers the view itself every frame (it eases the tilt back
// to its own and the yaw toward behind Link), and walls and its smoothing move
// the final eye after that. So from the mouse's first move the mouse owns the
// view's angles until it is let go or the game takes the camera (a cutscene, a
// door, Z-targeting, first person): each frame is drawn at exactly the angles
// the mouse set, at the distance the game chose.
static bool g_held;
static double g_pitch, g_yaw;
// Third-person distance scale (the wheel), kept until the wheel changes it;
// g_zoom_live is the part of it the follow camera has now.
static double g_zoom = 1.0, g_zoom_live = 1.0;
// First person and aiming (aim_frame).
static bool g_aim_ran;        // the player's update ran in an aiming view since the last camera_draw
static unsigned g_aim_wait;   // frames the game has been steering the aim (a lock-on)
static double g_aim_yaw_rest; // fraction of an angle unit not turned yet
static bool g_trace;
static unsigned long long g_retrace;

// BLUEWAKE_MOUSE_TEST=retrace:dx:dy:length[:wheel],...: pointer motion (and
// wheel notches) per retrace, for testing without a mouse.
typedef struct {
    unsigned long long start, length;
    double dx, dy, wheel;
} TestMove;
static TestMove g_test[16];
static unsigned g_test_count;
// BLUEWAKE_MOUSE_TEST_ITEM=item[@retrace] (testing only): puts that item
// (dItemNo, e.g. 0x27 the bow) in its inventory slot and on X, with arrows,
// at the player's first update from that retrace (default 900), so the aiming
// views can be tried on a save that does not have it yet.
static int g_test_item = -1;
static unsigned long long g_test_item_retrace = 900;

// The right stick as the camera (on unless BLUEWAKE_STICK_CAMERA=0). The game's own
// C-stick camera (dCamera_c's manual camera) eases its turn in and out and
// only starts past a quarter of the stick's tilt, which feels floaty next to
// the mouse. In this mode, wherever the mouse would turn the camera (the
// follow camera, the player in control), the stick turns it the same way,
// setting the view's angles directly: a turn rate from its tilt, no easing,
// and the view held where it leaves it. The game then never sees the stick
// there. Its click is the C-stick's push up (first person), and in first
// person the C-stick's push down (back out). In first person and when aiming
// an item it aims, as the mouse does (aim_frame); in the telescope and the
// Picto Box, whose zoom the C-stick's up and down were, the left stick's up and
// down zoom instead (it no longer aims there), as do the D-pad's. Elsewhere
// (Z-targeting, the boat's special cameras, cutscenes) it is the game's C-stick.
// With no controller, or one left resting, nothing changes.
static bool g_stick_on;
static double g_stick_speed = 360.0;     // degrees a second at full tilt, left and right
static double g_stick_aim_speed = 180.0; // the same when aiming (first person and items)
static double g_stick_invert_x = 1.0, g_stick_invert_y = 1.0;
// Tilt (0..1) inside which the stick does nothing, and from which it turns at
// full speed; up and down turn at this share of left and right's speed.
static const double kStickDeadZone = 0.12, kStickFull = 0.95, kStickPitchShare = 0.6;
// Past this tilt the stick is the camera's, not the game's C-stick.
static const double kStickInUse = 0.05;
// A game frame (camera_draw runs once each): the stick turns by game time, so
// a steady tilt turns the same amount every frame and the in-between frames
// show an even turn.
static const double kGameFrameSeconds = 1.0 / 29.97;
static bool g_stick_owns;       // the last camera_draw was the follow camera, the player in control
static bool g_stick_aims;       // ... or an aiming view: the stick aims, as the mouse does (aim_frame)
static bool g_stick_zooms;      // ... one that zooms (telescope, Picto Box): the left stick zooms
// Held on a controller's D-pad in the telescope or the Picto Box (whose zoom
// the C-stick's up and down were), or the left stick pushed all the way: this
// much of their 1x-9x zoom a second.
static const double kPadZoomPerSecond = 1.2;
static bool g_first_person;     // ... or first person (C-stick up's view, SS01)
static bool g_stick_click_down; // the stick's click, as last read
static unsigned long long g_exit_from; // retrace a click in first person started its push down
static int g_subject_step;             // first person's push-down step (subjectCamera's m3C4), or -1

// BLUEWAKE_STICK_TEST=retrace:x:y:length[:click[:zoom[:left_y]]],... (testing
// only): the right stick's tilt (SDL's axes, -1..1, y down), its click, the
// D-pad's zoom (1 up, -1 down) and the left stick's up and down from that
// retrace for `length`.
typedef struct {
    unsigned long long start, length;
    double x, y;
    int click, zoom;
    double left_y;
} TestStick;
static TestStick g_stick_test[16];
static unsigned g_stick_test_count;

static void set_captured(bool captured) {
    SDL_Window* window = g_window != 0 ? SDL_GetWindowFromID(g_window) : NULL;
    if (window == NULL || captured == g_captured)
        return;
    if (!SDL_SetWindowRelativeMouseMode(window, captured))
        return;
    g_captured = captured;
    g_click = false;
    g_sum_x = g_sum_y = 0.0;
    g_wheel = 0.0;
    g_held = false;
    fprintf(stderr, "[mouse] camera %s\n",
            captured ? "on (left click is A, the wheel zooms, Esc gives the mouse back)"
                     : "off (click to turn it on)");
}

static void observe(const void* sdl_event, void* user) {
    (void)user;
    const SDL_Event* event = (const SDL_Event*)sdl_event;
    // The controllers first (player 1 changes hands even with the menu open).
    bluewake_gamepads_event(sdl_event);
    // The options menu next: it opens and closes on its keys, and while it
    // is open it has the keyboard, mouse and controller to itself.
    if (bluewake_settings_menu_event(sdl_event))
        return;
    bluewake_jump_button_event(sdl_event);
    // Debug save states: F5 saves, F9 loads the last one (main.c).
    if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat &&
        (event->key.scancode == SDL_SCANCODE_F5 || event->key.scancode == SDL_SCANCODE_F9))
        bluewake_save_state_hotkey(event->key.scancode == SDL_SCANCODE_F9);
    if (!g_enabled)
        return;
    switch (event->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (event->button.button != SDL_BUTTON_LEFT)
            break;
        if (g_captured) {
            g_click = true;
        } else {
            // The click that hands over the mouse is not a press.
            g_window = event->button.windowID;
            set_captured(true);
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event->button.button == SDL_BUTTON_LEFT)
            g_click = false;
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (g_captured) {
            g_sum_x += event->motion.xrel;
            g_sum_y += event->motion.yrel;
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        // Away from the player zooms in, whichever way the system scrolls
        // pages (FLIPPED is macOS's natural scrolling).
        if (g_captured)
            g_wheel += event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event->wheel.y : event->wheel.y;
        break;
    case SDL_EVENT_KEY_DOWN:
        if (event->key.scancode == SDL_SCANCODE_ESCAPE)
            set_captured(false);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_MINIMIZED:
        set_captured(false);
        break;
    default:
        break;
    }
}

void bluewake_mouse_camera_install(void) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
    // An iPad's touches arrive as mouse events too; its controls are on screen.
    return;
#else
    // Installed even with the mouse camera off: the options menu and the
    // jump key read the same events.
    const char* on = getenv("BLUEWAKE_MOUSE_CAMERA");
    g_enabled = on == NULL || on[0] != '0';
    dol_aurora_set_event_observer(observe, NULL);
    if (g_enabled)
        fprintf(stderr, "[mouse] click the game to turn the camera with the mouse\n");
#endif
}

bool bluewake_mouse_camera_captured(void) { return g_captured; }

bool bluewake_mouse_camera_scripted(void) { return g_stick_test_count > 0u; }

void bluewake_mouse_camera_release(void) {
    if (g_captured)
        set_captured(false);
}

static bool env_is(const char* name, char value) {
    const char* text = getenv(name);
    return text != NULL && text[0] == value;
}

static void read_stick_settings(void) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
    // Not tried with the touch controls yet: off unless asked for.
    g_stick_on = env_is("BLUEWAKE_STICK_CAMERA", '1');
#else
    g_stick_on = !env_is("BLUEWAKE_STICK_CAMERA", '0');
#endif
    const char* speed = getenv("BLUEWAKE_STICK_CAMERA_SPEED");
    g_stick_speed = speed != NULL && atof(speed) > 0.0 ? atof(speed) : 360.0;
    const char* aim = getenv("BLUEWAKE_STICK_AIM_SPEED");
    g_stick_aim_speed = aim != NULL && atof(aim) > 0.0 ? atof(aim) : 180.0;
    g_stick_invert_x = env_is("BLUEWAKE_STICK_CAMERA_INVERT_X", '1') ? -1.0 : 1.0;
    g_stick_invert_y = env_is("BLUEWAKE_STICK_CAMERA_INVERT_Y", '1') ? -1.0 : 1.0;
    if (!g_stick_on) {
        g_stick_owns = g_stick_aims = g_stick_zooms = g_first_person = false;
        g_exit_from = 0;
    }
}

void bluewake_mouse_camera_reload(void) {
#if !(defined(__APPLE__) && TARGET_OS_IPHONE)
    const char* on = getenv("BLUEWAKE_MOUSE_CAMERA");
    g_enabled = on == NULL || on[0] != '0';
    if (!g_enabled)
        bluewake_mouse_camera_release();
#endif
    const char* sensitivity = getenv("BLUEWAKE_MOUSE_SENSITIVITY");
    g_sensitivity = sensitivity != NULL && atof(sensitivity) > 0.0 ? atof(sensitivity) : 1.0;
    const char* invert = getenv("BLUEWAKE_MOUSE_INVERT_Y");
    g_invert_y = invert != NULL && invert[0] == '1' ? -1.0 : 1.0;
    read_stick_settings();
}

void bluewake_mouse_camera_attach(CPUState* cpu) {
    (void)cpu;
    const char* sensitivity = getenv("BLUEWAKE_MOUSE_SENSITIVITY");
    if (sensitivity != NULL && atof(sensitivity) > 0.0)
        g_sensitivity = atof(sensitivity);
    const char* invert = getenv("BLUEWAKE_MOUSE_INVERT_Y");
    if (invert != NULL && invert[0] == '1')
        g_invert_y = -1.0;
    read_stick_settings();
    if (g_stick_on)
        fprintf(stderr,
                "[stick] the right stick turns the camera directly (%.0f degrees a second) and aims (%.0f); its "
                "click is first person; the left stick zooms the telescope and the Picto Box\n",
                g_stick_speed, g_stick_aim_speed);
    const char* stick_test = getenv("BLUEWAKE_STICK_TEST");
    for (const char* p = stick_test; p != NULL && *p != '\0' && g_stick_test_count < 16u;) {
        TestStick move = {0};
        int used = 0;
        if (sscanf(p, "%llu:%lf:%lf:%llu%n", &move.start, &move.x, &move.y, &move.length, &used) != 4)
            break;
        p += used;
        if (*p == ':' && sscanf(p, ":%d%n", &move.click, &used) == 1)
            p += used;
        if (*p == ':' && sscanf(p, ":%d%n", &move.zoom, &used) == 1)
            p += used;
        if (*p == ':' && sscanf(p, ":%lf%n", &move.left_y, &used) == 1)
            p += used;
        g_stick_test[g_stick_test_count++] = move;
        if (*p == ',')
            ++p;
    }
    const char* trace = getenv("BLUEWAKE_MOUSE_TRACE");
    g_trace = trace != NULL && trace[0] == '1';
    const char* test = getenv("BLUEWAKE_MOUSE_TEST");
    for (const char* p = test; p != NULL && *p != '\0' && g_test_count < 16u;) {
        TestMove move = {0};
        int used = 0;
        if (sscanf(p, "%llu:%lf:%lf:%llu%n", &move.start, &move.dx, &move.dy, &move.length, &used) != 4)
            break;
        p += used;
        if (*p == ':' && sscanf(p, ":%lf%n", &move.wheel, &used) == 1)
            p += used;
        g_test[g_test_count++] = move;
        if (*p == ',')
            ++p;
    }
    const char* item = getenv("BLUEWAKE_MOUSE_TEST_ITEM");
    if (item != NULL && item[0] != '\0') {
        char* end = NULL;
        g_test_item = (int)(strtol(item, &end, 0) & 0xFF);
        if (end != NULL && *end == '@')
            g_test_item_retrace = strtoull(end + 1, NULL, 0);
    }
}

// Player 1's C-stick and stick as the game has them through the mapping (SDL's
// axes, -1..1, y down: a stick swap moves the camera too), whether the first
// person button is held, and when asked for, the zoom buttons (1 in, -1 out,
// 0; input_bindings.h). Without a player 1, the right and left sticks of
// whichever controller has them tilted most.
static void read_stick_left(double* x, double* y, bool* click, int* zoom, double* left_x, double* left_y) {
    *x = *y = 0.0;
    *click = false;
    if (zoom != NULL)
        *zoom = 0;
    if (left_x != NULL)
        *left_x = *left_y = 0.0;
    for (unsigned i = 0; i < g_stick_test_count; ++i) {
        const TestStick* move = &g_stick_test[i];
        if (g_retrace >= move->start && g_retrace < move->start + move->length) {
            *x = move->x;
            *y = move->y;
            *click = move->click != 0;
            if (zoom != NULL)
                *zoom = move->zoom;
            if (left_x != NULL)
                *left_y = move->left_y;
            return;
        }
    }
    *click = bluewake_action_down(BW_ACTION_FIRST_PERSON);
    if (zoom != NULL)
        *zoom = bluewake_action_down(BW_ACTION_ZOOM_IN)    ? 1
                : bluewake_action_down(BW_ACTION_ZOOM_OUT) ? -1
                                                           : 0;
    if (bluewake_padmap_stick(1, x, y)) {
        if (left_x != NULL)
            (void)bluewake_padmap_stick(0, left_x, left_y);
        return;
    }
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    double most = 0.0;
    for (int i = 0; ids != NULL && i < count; ++i) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
        if (pad == NULL)
            continue;
        double px = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0;
        double py = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0;
        px = px < -1.0 ? -1.0 : px;
        py = py < -1.0 ? -1.0 : py;
        if (px * px + py * py > most) {
            most = px * px + py * py;
            *x = px;
            *y = py;
        }
        if (left_x != NULL) {
            double lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0;
            double ly = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0;
            lx = lx < -1.0 ? -1.0 : lx;
            ly = ly < -1.0 ? -1.0 : ly;
            if (lx * lx + ly * ly > *left_x * *left_x + *left_y * *left_y) {
                *left_x = lx;
                *left_y = ly;
            }
        }
    }
    SDL_free(ids);
}

static void read_stick(double* x, double* y, bool* click, int* zoom) {
    read_stick_left(x, y, click, zoom, NULL, NULL);
}

// The turn (degrees) a tilt makes over `seconds`: nothing inside the dead zone,
// then gentle near the middle for fine aim (30 percent linear, 70 quadratic)
// and full speed at the edge. Right turns the view right and up looks up, as
// the mouse does.
static void stick_turn(double x, double y, double seconds, double speed, double* yaw, double* pitch) {
    *yaw = *pitch = 0.0;
    const double tilt = sqrt(x * x + y * y);
    if (tilt <= kStickDeadZone)
        return;
    double n = (tilt - kStickDeadZone) / (kStickFull - kStickDeadZone);
    n = n > 1.0 ? 1.0 : n;
    const double degrees = speed * (0.3 * n + 0.7 * n * n) * seconds / tilt;
    *yaw = x * degrees * g_stick_invert_x;
    *pitch = y * degrees * kStickPitchShare * g_stick_invert_y;
}

void bluewake_mouse_camera_pad(DolPadState* pad) {
    if (g_click)
        pad->button |= 0x0100u; // PAD_BUTTON_A
    if (!g_stick_on)
        return;
    double x, y, left_x, left_y;
    bool click;
    read_stick_left(&x, &y, &click, NULL, &left_x, &left_y);
    const bool pressed = click && !g_stick_click_down;
    if (g_stick_zooms && sqrt(left_x * left_x + left_y * left_y) > kStickInUse)
        pad->stick_x = pad->stick_y = 0; // it zooms (aim_frame), so it does not also aim
    g_stick_click_down = click;
    if (g_stick_owns || g_stick_aims) {
        // view_frame turns the view by the stick, and aim_frame aims by it:
        // the game's own C-stick (its eased camera, first person's push down
        // out, the telescope's zoom) must not also take it. The keyboard's
        // C-stick still goes through while the stick rests.
        if (sqrt(x * x + y * y) > kStickInUse)
            pad->substick_x = pad->substick_y = 0;
    }
    if (g_stick_owns && click) {
        pad->substick_x = 0; // the click is the push up: first person
        pad->substick_y = 127;
    } else if (g_first_person && pressed && g_exit_from == 0) {
        g_exit_from = g_retrace;
    }
    if (g_exit_from != 0) {
        // Out of first person: subjectCamera wants the stick a little down
        // (its m3C4 goes to 1), then past three quarters (to 2, which asks the
        // player to leave). -30 and -127 come out at about -0.25 and -1 after
        // PADClamp. It lets go as soon as the game has taken each step: held
        // once first person has ended, the follow camera would take it for its
        // own push down and switch to the manual camera.
        const unsigned long long n = g_retrace - g_exit_from;
        if (g_first_person && g_subject_step < 2 && n < 30u) {
            pad->substick_x = 0;
            pad->substick_y = g_subject_step < 1 ? -30 : -127;
        } else {
            g_exit_from = 0;
        }
    }
}

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

static bool guest_pointer(u32 address) {
    return address >= 0x80000000u && address < 0x81800000u;
}

// The camera's current style (a dCamera__Style), or 0.
static u32 camera_style(CPUState* cpu, u32 camera) {
    const u32 style = mem_read32(cpu, camera + kCameraStyle);
    return guest_pointer(style) ? style : 0u;
}

static u32 style_engine(CPUState* cpu, u32 style) {
    return style != 0u ? mem_read32(cpu, style + kStyleEngine) : ~0u;
}

static float style_param(CPUState* cpu, u32 style, unsigned index) {
    return read_f32(cpu, style + kStyleParams + 4u * index);
}

static bool camera_ready(CPUState* cpu, u32 camera) {
    return mem_read8(cpu, camera + kReady) != 0u && mem_read8(cpu, camera + kReady + 1u) != 0u &&
           mem_read8(cpu, camera + kReady + 2u) != 0u;
}

// No event or cutscene has the camera, and the player is not in a demo.
static bool player_in_control(CPUState* cpu) {
    if (mem_read8(cpu, kEventMode) != 0u)
        return false;
    const u32 player = mem_read32(cpu, kPlayerPointer);
    return guest_pointer(player) && mem_read32(cpu, player + kPlayerDemoMode) == 0u;
}

// The player has the camera: the follow camera's mode, no event or cutscene.
static bool camera_free(CPUState* cpu, u32 camera) {
    return mem_read32(cpu, camera + kMode) == 0u && player_in_control(cpu);
}

// First person (C-stick up) or an item's aim. Every one of these views (modes
// 4, 10, 11 and 14: styles SS01, SX01/SX02, SY01, SN15) is the subject camera.
static bool aiming_view(CPUState* cpu, u32 camera) {
    return style_engine(cpu, camera_style(cpu, camera)) == (u32)kEngineSubject;
}

// center + radius along the globe's angles (cSGlobe::Xyz: R cos V sin U,
// R sin V, R cos V cos U), into `eye`.
static void place_eye(CPUState* cpu, u32 center, u32 eye, double radius, double pitch, double yaw) {
    const double v = pitch * M_PI / 180.0, u = yaw * M_PI / 180.0;
    write_f32(cpu, eye, (float)(read_f32(cpu, center) + radius * cos(v) * sin(u)));
    write_f32(cpu, eye + 4u, (float)(read_f32(cpu, center + 4u) + radius * sin(v)));
    write_f32(cpu, eye + 8u, (float)(read_f32(cpu, center + 8u) + radius * cos(v) * cos(u)));
}

static float distance(CPUState* cpu, u32 a, u32 b) {
    const float dx = read_f32(cpu, a) - read_f32(cpu, b), dy = read_f32(cpu, a + 4u) - read_f32(cpu, b + 4u),
                dz = read_f32(cpu, a + 8u) - read_f32(cpu, b + 8u);
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static void add_angle(CPUState* cpu, u32 address, int delta) {
    mem_write16(cpu, address, (u16)(mem_read16(cpu, address) + (u16)delta));
}

// Turns the player (shape_angle.y, and current.angle.y with it, as the aim
// procs set them) by `delta` before its update. False, turning nothing, if
// the copies its update puts back are not what they should be.
static bool turn_player(CPUState* cpu, u32 player, int delta) {
    const u16 shape = mem_read16(cpu, player + kPlayerShapeY);
    if (mem_read16(cpu, kKeptShapeY) != shape ||
        mem_read16(cpu, kKeptAngleY) != mem_read16(cpu, player + kPlayerAngleY))
        return false;
    const u16 yaw = (u16)(shape + (u16)delta);
    mem_write16(cpu, player + kPlayerShapeY, yaw);
    mem_write16(cpu, player + kPlayerAngleY, yaw);
    mem_write16(cpu, kKeptShapeY, yaw);
    mem_write16(cpu, kKeptAngleY, yaw);
    return true;
}

// First person and item aiming, at the player's update (daPy_Execute's
// entry), before this frame's aim is read.
//
// In all of these views the aim is the player's own angles.
// dCamera_c::CalcSubjectAngle (0x80170490) turns shape_angle.y by the stick's
// X (a rate: yaw = shape_angle.y + p24 * m384) and moves mWork.subject.m388
// by the stick's Y (pitch = p19 * m388, m388 held to -1..1). The player's aim
// procs (procSubjectivity, procBowSubject, procHookshotSubject,
// procBoomerangSubject, procRopeSubject and the ship's) call it through
// setBodyAngleToCamera and take its angles as shape_angle.y and mBodyAngle.x
// (checkBodyAngleX keeps the pitch out of walls); the telescope and the Picto
// Box (procScope) instead follow the camera's angle, and subjectCamera calls
// it itself. The camera then eases its view toward those angles (p20 of the
// way a frame).
//
// So the mouse turns the angles CalcSubjectAngle starts from: shape_angle.y
// (with the camera's angle, which procScope copies into it) and m388. The game
// then aims and limits exactly as it does for the stick, which still works on
// top. The same turn goes into the camera's eased view, so the frame is drawn
// at the new aim with no easing lag; anything the game refuses (a wall, the
// stick's own turn) still eases.
static void aim_frame(CPUState* cpu, u32 player) {
    const u32 process = mem_read32(cpu, kCameraPointer);
    if (!guest_pointer(process))
        return;
    const u32 camera = process + kCameraBody;
    const u32 style = camera_style(cpu, camera);
    if (style_engine(cpu, style) != (u32)kEngineSubject)
        return;
    g_aim_ran = true;
    if (!player_in_control(cpu) || mem_read32(cpu, camera + kSubjectTag) != kSubjectTagValue) {
        g_sum_x = g_sum_y = g_wheel = 0.0;
        return;
    }
    // The fast stick camera's right stick aims as the mouse does (at its own
    // speed, a game frame's worth each update), and the D-pad zooms.
    double stick_yaw = 0.0, stick_pitch = 0.0, pad_zoom = 0.0;
    if (g_stick_on) {
        double x, y, left_x, left_y;
        bool click;
        int zoom;
        read_stick_left(&x, &y, &click, &zoom, &left_x, &left_y);
        stick_turn(x, y, kGameFrameSeconds, g_stick_aim_speed, &stick_yaw, &stick_pitch);
        // The left stick's up zooms in, as far as it is pushed (past the
        // dead zone); the D-pad at full speed.
        double push = -left_y;
        push = fabs(push) <= kStickDeadZone ? 0.0 : (push - copysign(kStickDeadZone, push)) / (1.0 - kStickDeadZone);
        if (zoom != 0)
            push = zoom;
        pad_zoom = (mem_read16(cpu, style + kStyleFlags) & kStyleZoom) != 0u
                       ? push * kPadZoomPerSecond * kGameFrameSeconds
                       : 0.0;
    }
    if (g_sum_x == 0.0 && g_sum_y == 0.0 && g_wheel == 0.0 && stick_yaw == 0.0 && stick_pitch == 0.0 &&
        pad_zoom == 0.0)
        return;
    const bool ready = camera_ready(cpu, camera);
    // The wheel (and the D-pad): the telescope's and the Picto Box's own zoom
    // (the C-stick's). It zooms while the game holds the aim too: Aryll's
    // telescope lesson locks the view on the postman and waits for a full zoom.
    if (ready && (g_wheel != 0.0 || pad_zoom != 0.0)) {
        const float level = read_f32(cpu, camera + kSubjectZoom);
        if ((mem_read16(cpu, style + kStyleFlags) & kStyleZoom) != 0u && level >= 0.0f && level <= 1.0f) {
            double next = level + g_wheel * kScopeZoomPerNotch + pad_zoom;
            next = next < 0.0 ? 0.0 : next > 1.0 ? 1.0 : next;
            write_f32(cpu, camera + kSubjectZoom, (float)next);
        }
        g_wheel = 0.0;
    }
    if (ready && mem_read8(cpu, camera + kSubjectFree) == 0u) {
        // The game aims (a lock-on), or this is the entry's last frame, when
        // the player takes the camera's angles: keep the motion a frame or
        // two, then let it go rather than jump when the lock ends.
        if (++g_aim_wait > 2u)
            g_sum_x = g_sum_y = g_wheel = 0.0;
        return;
    }
    g_aim_wait = 0;
    const u32 status = mem_read32(cpu, kPlayerStatus0);
    if ((status & kStatusHookshotOut) != 0u) {
        // The hookshot's chain is out: the game holds the aim still.
        g_sum_x = g_sum_y = g_wheel = 0.0;
        return;
    }

    // A zoomed view turns less for the same motion, so the picture moves as
    // far across the screen at 9x as at 1x.
    const bool zooms = (mem_read16(cpu, style + kStyleFlags) & kStyleZoom) != 0u;
    const float zoom_level = read_f32(cpu, camera + kSubjectZoom);
    const bool zoom_known = zoom_level >= 0.0f && zoom_level <= 1.0f;
    const double zoom_div = zooms && zoom_known ? 1.0 + 8.0 * zoom_level : 1.0;
    const double scale = kDegreesPerPoint * g_sensitivity / zoom_div;

    // Yaw: pointer right turns right (shape_angle.y goes down, as with the
    // stick). It turns during the view's entry too, which follows the
    // player's facing.
    const double turn = -(g_sum_x * scale + stick_yaw / zoom_div) * kAngleUnits + g_aim_yaw_rest;
    const int du = (int)lrint(turn);
    g_aim_yaw_rest = turn - du;
    g_sum_x = 0.0;
    if (du != 0 && turn_player(cpu, player, du)) {
        add_angle(cpu, process + kCameraAngleY, du);
        if (ready)
            add_angle(cpu, camera + kViewYaw, du);
    }
    if (!ready)
        return; // the entry sets its own tilt and zoom: those wait for it

    // Pitch: pointer forward looks up (m388 goes down; stick up looks down).
    const float range = style_param(cpu, style, 19); // p19: the tilt's limit, degrees
    const float before = read_f32(cpu, camera + kSubjectPitch);
    if (range > 1.0f && range < 180.0f && before >= -1.0f && before <= 1.0f && (status & kStatusCrawl) == 0u) {
        float after = (float)(before + (g_sum_y * scale * g_invert_y + stick_pitch / zoom_div) / range);
        after = after < -1.0f ? -1.0f : after > 1.0f ? 1.0f : after;
        // As CalcSubjectAngle does for the stick: with the eye down at the
        // ground or the water, the tilt may only come back toward level.
        const float eye_y = read_f32(cpu, camera + kFinalEye + 4u);
        const bool low = eye_y <= read_f32(cpu, camera + kGroundHeight) + 5.0f ||
                         eye_y <= read_f32(cpu, camera + kWaterHeight) + 5.0f;
        if (low && ((before >= 0.0f && after > before) || (before < 0.0f && after < before)))
            after = before;
        if (after != before) {
            write_f32(cpu, camera + kSubjectPitch, after);
            add_angle(cpu, camera + kViewPitch,
                      (int)(lrint(after * range * kAngleUnits) - lrint(before * range * kAngleUnits)));
        }
    }
    g_sum_y = 0.0;

    if (g_trace) {
        const u32 name = mem_read32(cpu, style);
        fprintf(stderr,
                "[mouse-aim] retrace=%llu mode=%u style=%c%c%c%c status=%08x yaw=%d (%+d) m388=%.3f->%.3f body=%d "
                "view V=%d U=%d zoom=%.3f fovy=%.2f\n",
                g_retrace, mem_read32(cpu, camera + kMode), (char)(name >> 24), (char)(name >> 16),
                (char)(name >> 8), (char)name, status, (s16)mem_read16(cpu, player + kPlayerShapeY), du, before,
                read_f32(cpu, camera + kSubjectPitch), (s16)mem_read16(cpu, player + kPlayerBodyX),
                (s16)mem_read16(cpu, camera + kViewPitch), (s16)mem_read16(cpu, camera + kViewYaw),
                read_f32(cpu, camera + kSubjectZoom), read_f32(cpu, camera + kFinalFovy));
    }
}

// The wheel in third person scales the follow camera's distance. That camera
// keeps its distance anywhere between two limits (mWork.follow.m398 / m39C,
// eased each frame toward the style's p11 / p10 by p14) and leaves it where
// Link's running puts it, so both are scaled: the limits every frame, and the
// eased distance itself (dCamera_c::mViewCache) when the scale changes, a
// third of the way a frame. The camera then does the rest as it would at its
// own distances, and bumpCheck still keeps it out of walls. Other cameras
// (Z-targeting, the boat, fixed cameras, cutscenes) keep the game's distances;
// when the follow camera comes back it starts at its own and the scale comes
// back in smoothly.
static void zoom_frame(CPUState* cpu, u32 camera, bool player_camera) {
    if (g_wheel != 0.0) {
        g_zoom /= pow(kZoomStep, g_wheel);
        g_zoom = g_zoom < kZoomMin ? kZoomMin : g_zoom > kZoomMax ? kZoomMax : g_zoom;
        if (fabs(g_zoom - 1.0) < 0.02)
            g_zoom = 1.0;
        g_wheel = 0.0;
        fprintf(stderr, "[mouse] camera distance x%.2f\n", g_zoom);
    }
    const u32 style = camera_style(cpu, camera);
    const u32 engine = style_engine(cpu, style);
    if (!player_camera || (engine != (u32)kEngineFollow && engine != (u32)kEngineFollow2) ||
        mem_read8(cpu, camera + kReady) == 0u) {
        g_zoom_live = 1.0; // the follow camera's entry starts from its own distances
        return;
    }
    const float far = style_param(cpu, style, 10), near = style_param(cpu, style, 11),
                ease = style_param(cpu, style, 14);
    if (!(near > 1.0f && far >= near && far < 100000.0f && ease >= 0.0f && ease < 1.0f))
        return;
    if (g_zoom_live != g_zoom) {
        double next = g_zoom_live + (g_zoom - g_zoom_live) / 3.0;
        if (fabs(next - g_zoom) < 0.005)
            next = g_zoom;
        const float radius = read_f32(cpu, camera + kViewRadius);
        if (radius > 1.0f && radius < 100000.0f) {
            const float scaled = (float)(radius * next / g_zoom_live);
            write_f32(cpu, camera + kViewRadius, scaled);
            place_eye(cpu, camera + kViewCenter, camera + kViewEye, scaled,
                      (s16)mem_read16(cpu, camera + kViewPitch) / kAngleUnits,
                      (s16)mem_read16(cpu, camera + kViewYaw) / kAngleUnits);
        }
        g_zoom_live = next;
    }
    if (g_zoom_live == 1.0)
        return; // the limits ease back to the style's by themselves
    // m' = m + ease * (p - m) must come out at p * zoom.
    const double before_ease = (g_zoom_live - ease) / (1.0 - ease);
    write_f32(cpu, camera + kFollowMinRadius, (float)(near * before_ease));
    write_f32(cpu, camera + kFollowMaxRadius, (float)(far * before_ease));
}

// At bumpCheck's entry, once a frame in dCamera_c::Run: the camera's routine
// (the follow camera) has eased mViewCache, and bumpCheck is about to make the
// frame's eye from it, pulling it in along the line from Link where that line
// meets a wall or the ground and lifting it out of the water. The mouse's and
// the stick's angles go into mViewCache here, so that eye is theirs and the
// game keeps it clear; placed at camera_draw instead, it went wherever the
// angles said, into the ground or under the sea when tilted low.
static void view_frame(CPUState* cpu, u32 camera) {
    if (aiming_view(cpu, camera) || !camera_free(cpu, camera))
        return; // camera_frame, at the draw, lets go
    double stick_yaw = 0.0, stick_pitch = 0.0;
    if (g_stick_on) {
        double x, y;
        bool click;
        read_stick(&x, &y, &click, NULL);
        stick_turn(x, y, kGameFrameSeconds, g_stick_speed, &stick_yaw, &stick_pitch);
    }
    if (g_sum_x == 0.0 && g_sum_y == 0.0 && stick_yaw == 0.0 && stick_pitch == 0.0 && !g_held)
        return;
    if (!g_held) {
        g_pitch = (s16)mem_read16(cpu, camera + kViewPitch) / kAngleUnits;
        g_yaw = (s16)mem_read16(cpu, camera + kViewYaw) / kAngleUnits;
        g_held = true;
    }
    // Pointer right turns the view right (the camera swings the other way
    // round Link); pointer forward looks up (the camera drops). The stick
    // likewise.
    const double scale = kDegreesPerPoint * g_sensitivity;
    g_yaw = fmod(g_yaw - g_sum_x * scale - stick_yaw, 360.0);
    g_pitch += g_sum_y * scale * g_invert_y + stick_pitch;
    g_pitch = g_pitch < kPitchMin ? kPitchMin : g_pitch > kPitchMax ? kPitchMax : g_pitch;
    g_sum_x = g_sum_y = 0.0;
    // At the distance the follow camera chose; bumpCheck brings it in from
    // there, and the next frame's camera starts from these angles.
    const float radius = read_f32(cpu, camera + kViewRadius);
    if (radius > 1.0f && radius < 100000.0f) {
        mem_write16(cpu, camera + kViewPitch, (u16)(s16)lrint(g_pitch * kAngleUnits));
        mem_write16(cpu, camera + kViewYaw, (u16)(s16)lrint(g_yaw * kAngleUnits));
        place_eye(cpu, camera + kViewCenter, camera + kViewEye, radius, g_pitch, g_yaw);
    }
}

// At camera_draw's entry: this frame's camera is final and about to be drawn.
static void camera_frame(CPUState* cpu, u32 process) {
    const u32 camera = process + kCameraBody;
    const bool aiming = aiming_view(cpu, camera);
    const u32 style = camera_style(cpu, camera);
    g_first_person = aiming && style != 0u && mem_read32(cpu, style) == 0x53533031u; // 'SS01'
    g_subject_step = g_first_person ? (int)mem_read32(cpu, camera + kSubjectStep) : -1;
    g_stick_owns = false;
    g_stick_aims = aiming && g_stick_on && player_in_control(cpu);
    g_stick_zooms = g_stick_aims && (mem_read16(cpu, style + kStyleFlags) & kStyleZoom) != 0u;
    if (aiming && player_in_control(cpu)) {
        // First person or an item's aim: aim_frame, at the player's next
        // update, owns the pointer and the wheel. If it did not run once the
        // view was up, drop what came in rather than let it pile up.
        g_held = false;
        g_zoom_live = 1.0;
        if (!g_aim_ran && mem_read8(cpu, camera + kReady) != 0u)
            g_sum_x = g_sum_y = g_wheel = 0.0;
        g_aim_ran = false;
        return;
    }
    g_aim_ran = false;
    g_aim_wait = 0;
    g_aim_yaw_rest = 0.0;
    const bool player_camera = camera_free(cpu, camera);
    zoom_frame(cpu, camera, player_camera);
    if (!player_camera) {
        // A cutscene or a special camera keeps its own view.
        g_sum_x = g_sum_y = g_wheel = 0.0;
        g_held = false;
        return;
    }
    // The mouse's and the stick's view went in at bumpCheck (view_frame);
    // the stick is the camera's until the next draw says otherwise.
    g_stick_owns = g_stick_on;
}

// Testing only: gives the player an item (BLUEWAKE_MOUSE_TEST_ITEM) on X.
// g_dComIfG_gameInfo (0x803C4C08) starts with the save's dSv_player_c:
// mPlayerStatusA.mSelectItem[X] at +0x09 (an inventory slot),
// mPlayerItem.mItems[21] at +0x3C, the arrow count at +0x69
// (mItemRecord.mArrowNum) and its maximum at +0x6F (mItemMax.mArrowNum); the
// play state's copy of mSelectItem[X] (the item number the player reads) is
// at 0x803CA7DB (play at +0x12A0, mSelectItem at play +0x4933).
static void grant_test_item(CPUState* cpu) {
    static const struct {
        u8 item, slot;
    } kSlots[] = {{0x20, 0}, {0x25, 3},  {0x2D, 5},  {0x34, 6},  {0x23, 8},
                  {0x26, 8}, {0x27, 12}, {0x35, 12}, {0x36, 12}, {0x2F, 19}};
    for (unsigned i = 0; i < sizeof kSlots / sizeof kSlots[0]; ++i) {
        if (kSlots[i].item != g_test_item)
            continue;
        mem_write8(cpu, 0x803C4C44u + kSlots[i].slot, kSlots[i].item);
        mem_write8(cpu, 0x803C4C11u, kSlots[i].slot);
        mem_write8(cpu, 0x803CA7DBu, kSlots[i].item);
        if (kSlots[i].slot == 12) {
            mem_write8(cpu, 0x803C4C71u, 30);
            mem_write8(cpu, 0x803C4C77u, 30);
        }
        fprintf(stderr, "[mouse] test: item 0x%02X in slot %u, on X\n", g_test_item, kSlots[i].slot);
        break;
    }
    g_test_item = -1;
}

static void trace_camera(CPUState* cpu, u32 process) {
    const u32 camera = process + kCameraBody;
    const u32 player = mem_read32(cpu, kPlayerPointer);
    const u32 style = camera_style(cpu, camera);
    const u32 name = style != 0u ? mem_read32(cpu, style) : 0x3F3F3F3Fu;
    fprintf(stderr,
            "[mouse-trace] retrace=%llu mode=%u style=%c%c%c%c event=%u demo=%u view V=%d U=%d final V=%d U=%d "
            "reach=%.1f held=%d stick=%d radius=%.1f limits=%.1f..%.1f zoom=%.2f/%.2f link=%d eye_y=%.1f "
            "center_y=%.1f floor=%.1f/%.1f\n",
            g_retrace, mem_read32(cpu, camera + kMode), (char)(name >> 24), (char)(name >> 16), (char)(name >> 8),
            (char)name, mem_read8(cpu, kEventMode),
            guest_pointer(player) ? mem_read32(cpu, player + kPlayerDemoMode) : 99u,
            (s16)mem_read16(cpu, camera + kViewPitch), (s16)mem_read16(cpu, camera + kViewYaw),
            (s16)mem_read16(cpu, camera + kFinalPitch), (s16)mem_read16(cpu, camera + kFinalYaw),
            distance(cpu, process + kLookatEye, process + kLookatCenter), g_held ? 1 : 0,
            g_stick_owns ? 1 : g_first_person ? 2 : 0,
            read_f32(cpu, camera + kViewRadius), read_f32(cpu, camera + kFollowMinRadius),
            read_f32(cpu, camera + kFollowMaxRadius), g_zoom_live, g_zoom,
            guest_pointer(player) ? (s16)mem_read16(cpu, player + kPlayerShapeY) : 0,
            read_f32(cpu, process + kLookatEye + 4u), read_f32(cpu, process + kLookatCenter + 4u),
            read_f32(cpu, camera + kGroundHeight), read_f32(cpu, camera + kWaterHeight));
}

void bluewake_mouse_camera_hook(CPUState* cpu, u32 address) {
    if (address == kCameraDraw) {
        if (cpu == NULL)
            return;
        const u32 process = cpu->gpr[3];
        if (process != mem_read32(cpu, kCameraPointer) || !guest_pointer(process))
            return;
        camera_frame(cpu, process);
        if (g_trace)
            trace_camera(cpu, process);
    } else if (address == kCameraBump) {
        if (cpu == NULL)
            return;
        const u32 process = mem_read32(cpu, kCameraPointer);
        if (!guest_pointer(process) || cpu->gpr[3] != process + kCameraBody)
            return;
        view_frame(cpu, process + kCameraBody);
    } else if (address == kPlayerExecute) {
        if (cpu == NULL)
            return;
        const u32 player = cpu->gpr[3];
        if (player != mem_read32(cpu, kPlayerPointer) || !guest_pointer(player))
            return;
        if (g_test_item >= 0 && g_retrace >= g_test_item_retrace)
            grant_test_item(cpu);
        aim_frame(cpu, player);
    }
}

void bluewake_mouse_camera_retrace(void) {
    ++g_retrace;
    for (unsigned i = 0; i < g_test_count; ++i) {
        if (g_retrace >= g_test[i].start && g_retrace < g_test[i].start + g_test[i].length) {
            g_sum_x += g_test[i].dx;
            g_sum_y += g_test[i].dy;
            g_wheel += g_test[i].wheel;
        }
    }
}
