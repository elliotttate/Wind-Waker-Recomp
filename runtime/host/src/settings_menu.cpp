// The options menu (the Mac host): the settings the host reads from the
// environment, in a window over the paused game, saved to a settings file.
#include "settings_menu.h"

// The host's modules are C.
extern "C" {
#include "climb.h"
#include "fast_load.h"
#include "game_options.h"
#include "forest_water.h"
#include "haptics.h"
#include "jump_button.h"
#include "mouse_camera.h"
#include "quick_doors.h"
#include "save_state.h"
#include "sprint.h"
#include "simulation_mode.h"
}

#include "gxruntime/aurora_backend.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <atomic>
#include <cmath>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <sys/stat.h>
#include <vector>

extern "C" {
void aurora_set_frame_buffer_scale(float scale);
void aurora_set_frame_interpolation(bool enabled);
void aurora_set_frame_interp_steps(int steps);
void aurora_set_fps_overlay(bool enabled);
void aurora_set_forced_anisotropy(unsigned samples);
}

namespace {

// The settings the menu writes, in the file's order. The rest of the file (keys
// the menu does not show) is kept as it was.
const char* const kKeys[] = {
    "BLUEWAKE_ASPECT",          "DOL_AURORA_FULLSCREEN",    "DOL_AURORA_RENDER_SCALE",
    "DOL_AURORA_FRAME_INTERP",  "DOL_AURORA_FRAME_INTERP_STEPS", "DOL_AURORA_SHOW_FPS", "DOL_AURORA_FORCE_ANISO",
    "DOL_AURORA_TEXTURE_PACK",  "BLUEWAKE_MODS",            "BLUEWAKE_OPTIONS",
    "BLUEWAKE_FADE_FRAMES",     "BLUEWAKE_FAST_FORWARD",    "BLUEWAKE_QUICK_DOORS",
    "BLUEWAKE_JUMP_BUTTON",
    "BLUEWAKE_SPRINT_SPEED",    "BLUEWAKE_MOUSE_CAMERA",    "BLUEWAKE_MOUSE_SENSITIVITY",
    "BLUEWAKE_MOUSE_INVERT_Y",  "BLUEWAKE_SIMULATION_60HZ", "BLUEWAKE_STICK_CAMERA",    "BLUEWAKE_STICK_CAMERA_SPEED",
    "BLUEWAKE_STICK_CAMERA_INVERT_X", "BLUEWAKE_STICK_CAMERA_INVERT_Y", "BLUEWAKE_STICK_AIM_SPEED",
    "BLUEWAKE_HAPTICS",         "BLUEWAKE_HAPTICS_STRENGTH", "BLUEWAKE_HAPTICS_TRIGGERS",
    "BLUEWAKE_CLIMB",           "BLUEWAKE_CLIMB_STAMINA",
    "BLUEWAKE_FOREST_WATER_KEEP_TREES", "BLUEWAKE_FOREST_WATER_30_MINUTES",
};

std::string g_path;                          // the settings file ("" when none)
std::map<std::string, std::string> g_other;  // its other keys, kept as they were
// Atomic: the Android touch overlay reads it from its own thread (bluewake_settings_is_open).
std::atomic<bool> g_open{false};
bool g_dirty = false;
bool g_nav_set = false;

// Settings that take effect at the next launch, as chosen now.
std::string g_aspect;
bool g_betterww = false;
std::vector<std::pair<std::string, bool>> g_options; // name, on
std::vector<std::string> g_option_titles;
char g_texture_pack[1024];
bool g_restart_pending = false;
// BLUEWAKE_SETTINGS_TEST_OPEN=at:for (seconds after the menu is installed;
// testing only): opens the menu, and closes it after `for` seconds.
double g_test_open_at = -1.0, g_test_open_for = 0.0;
Uint64 g_installed_ms = 0;
bool g_test_done = false;

std::string env(const char* key, const char* fallback = "") {
    const char* value = std::getenv(key);
    return value != nullptr ? value : fallback;
}

bool env_on(const char* key, bool fallback) {
    const char* value = std::getenv(key);
    if (value == nullptr || value[0] == '\0')
        return fallback;
    return value[0] != '0';
}

void set_env(const char* key, const std::string& value) {
    setenv(key, value.c_str(), 1);
    g_dirty = true;
}

std::string default_path() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0')
        return "";
    return std::string(home) + "/Library/Application Support/Wind Waker Recomp/settings.ini";
}

std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return "";
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool managed(const std::string& key) {
    for (const char* k : kKeys)
        if (key == k)
            return true;
    return false;
}

// Better Wind Waker as a mod the user chose: BLUEWAKE_MODS without the
// widescreen mod, which BLUEWAKE_ASPECT adds at every launch.
std::string chosen_mods() { return g_betterww ? "betterww" : ""; }

std::string options_value() {
    std::string list = "none";
    for (const auto& [name, on] : g_options)
        if (on)
            list += "," + name;
    return list;
}

void save() {
    if (g_path.empty())
        return;
    const auto slash = g_path.rfind('/');
    if (slash != std::string::npos) {
        // Application Support/Wind Waker Recomp, one level at a time.
        std::string dir = g_path.substr(0, slash);
        for (size_t at = 1; (at = dir.find('/', at)) != std::string::npos; ++at)
            mkdir(dir.substr(0, at).c_str(), 0755);
        mkdir(dir.c_str(), 0755);
    }
    FILE* file = std::fopen(g_path.c_str(), "w");
    if (file == nullptr) {
        std::fprintf(stderr, "[settings] cannot write %s: %s\n", g_path.c_str(), std::strerror(errno));
        return;
    }
    std::fputs("# Wind Waker Recomp settings, written by the options menu (Esc or F1 in the game).\n"
               "# KEY=VALUE, the host's environment settings; these win over the launch command's.\n",
               file);
    for (const char* key : kKeys) {
        std::string value;
        if (std::strcmp(key, "BLUEWAKE_ASPECT") == 0)
            value = g_aspect;
        else if (std::strcmp(key, "BLUEWAKE_MODS") == 0)
            value = chosen_mods();
        else if (std::strcmp(key, "BLUEWAKE_OPTIONS") == 0)
            value = g_options.empty() ? env(key) : options_value();
        else if (std::strcmp(key, "DOL_AURORA_TEXTURE_PACK") == 0)
            value = g_texture_pack;
        else
            value = env(key);
        std::fprintf(file, "%s=%s\n", key, value.c_str());
    }
    for (const auto& [key, value] : g_other)
        std::fprintf(file, "%s=%s\n", key.c_str(), value.c_str());
    std::fclose(file);
    g_dirty = false;
    std::fprintf(stderr, "[settings] saved %s\n", g_path.c_str());
}

// The launch-time choices as they are now (the environment after the launch
// and the settings file, and the options the game started with).
void capture_launch_choices() {
    g_aspect = env("BLUEWAKE_ASPECT", "4:3");
    const std::string mods = env("BLUEWAKE_MODS");
    g_betterww = mods.find("betterww") != std::string::npos;
    std::snprintf(g_texture_pack, sizeof g_texture_pack, "%s", env("DOL_AURORA_TEXTURE_PACK").c_str());
    g_options.clear();
    g_option_titles.clear();
    for (u32 i = 0;; ++i) {
        const char* title = nullptr;
        bool default_on = false, on = false;
        const char* name = bluewake_game_options_describe(i, &title, &default_on, &on);
        if (name == nullptr)
            break;
        g_options.emplace_back(name, g_betterww ? on : default_on);
        g_option_titles.emplace_back(title != nullptr ? title : name);
    }
}

SDL_Window* game_window() {
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    SDL_Window* window = windows != nullptr && count > 0 ? windows[0] : nullptr;
    SDL_free(windows);
    return window;
}

void open_menu() {
    if (g_open)
        return;
    bluewake_mouse_camera_release();
    bluewake_haptics_block(true);
    capture_launch_choices();
    g_open = true;
    std::fprintf(stderr, "[settings] menu open (the game is paused)\n");
}

void close_menu() {
    if (!g_open)
        return;
    g_open = false;
    bluewake_haptics_block(false);
    save();
    std::fprintf(stderr, "[settings] menu closed\n");
}

bool hold(void*) { return g_open; }

// A setting that the game takes at the next launch.
void restart_note() {
    ImGui::SameLine();
    ImGui::TextDisabled("(next launch)");
}

bool combo(const char* label, int* index, const char* const* items, int count) {
    return ImGui::Combo(label, index, items, count);
}

void display_tab() {
    static const char* const kAspects[] = {"4:3 (the game's)", "16:10", "16:9"};
    static const char* const kAspectValues[] = {"4:3", "16:10", "16:9"};
    int aspect = 0;
    for (int i = 0; i < 3; ++i)
        if (g_aspect == kAspectValues[i])
            aspect = i;
    if (combo("Aspect ratio", &aspect, kAspects, 3)) {
        g_aspect = kAspectValues[aspect];
        g_dirty = g_restart_pending = true;
    }
    restart_note();

    bool fullscreen = env_on("DOL_AURORA_FULLSCREEN", false);
    if (ImGui::Checkbox("Fullscreen", &fullscreen)) {
        set_env("DOL_AURORA_FULLSCREEN", fullscreen ? "1" : "0");
        if (SDL_Window* window = game_window())
            SDL_SetWindowFullscreen(window, fullscreen);
    }

    static const char* const kScales[] = {"The window's pixels", "1x (480 lines)", "2x (960)", "3x (1440)",
                                          "4x (1920)"};
    int scale = std::atoi(env("DOL_AURORA_RENDER_SCALE", "0").c_str());
    scale = scale < 0 ? 0 : scale > 4 ? 4 : scale;
    if (combo("Render resolution", &scale, kScales, 5)) {
        set_env("DOL_AURORA_RENDER_SCALE", std::to_string(scale));
        aurora_set_frame_buffer_scale(static_cast<float>(scale));
    }

    bool simulation = env_on("BLUEWAKE_SIMULATION_60HZ", false);
    ImGui::BeginDisabled(!bluewake_simulation_supported());
    if (ImGui::Checkbox("60 Hz gameplay (experimental)", &simulation)) {
        set_env("BLUEWAKE_SIMULATION_60HZ", simulation ? "1" : "0");
        g_restart_pending = true;
    }
    restart_note();
    ImGui::EndDisabled();
    if (!bluewake_simulation_supported())
        ImGui::TextWrapped("Rebuild your personal game module to enable this option.");
    else if (simulation || bluewake_simulation_enabled())
        ImGui::TextWrapped("Targets 60 full game updates per second. Needs more CPU power and may run slowly. Timing fixes are incomplete; dialogues, cutscenes and transitions keep their original timing.");

    // Smooth Motion: the game's 30 frames a second, or in-between frames for
    // 60 (one each) or 120 (three each, for a 120 Hz display such as a
    // MacBook Pro's).
    static const char* const kSmooth[] = {"Off (30, the game's)", "60 frames a second",
                                          "120 frames a second (120 Hz displays)"};
    int smooth = !env_on("DOL_AURORA_FRAME_INTERP", false)                      ? 0
                 : std::atoi(env("DOL_AURORA_FRAME_INTERP_STEPS", "1").c_str()) >= 3 ? 2
                                                                                    : 1;
    ImGui::BeginDisabled(bluewake_simulation_enabled());
    if (bluewake_simulation_enabled())
        smooth = 0;
    if (combo("Smooth Motion", &smooth, kSmooth, 3)) {
        set_env("DOL_AURORA_FRAME_INTERP", smooth != 0 ? "1" : "0");
        set_env("DOL_AURORA_FRAME_INTERP_STEPS", smooth == 2 ? "3" : "1");
        aurora_set_frame_interp_steps(smooth == 2 ? 3 : 1);
        aurora_set_frame_interpolation(smooth != 0);
    }
    ImGui::EndDisabled();

    bool fps = env_on("DOL_AURORA_SHOW_FPS", false);
    if (ImGui::Checkbox("Show the frame rate", &fps)) {
        set_env("DOL_AURORA_SHOW_FPS", fps ? "1" : "0");
        aurora_set_fps_overlay(fps);
    }

    static const char* const kAniso[] = {"Off (the game's)", "2x", "4x", "8x", "16x"};
    static const unsigned kAnisoValues[] = {1, 2, 4, 8, 16};
    const unsigned samples = static_cast<unsigned>(std::atoi(env("DOL_AURORA_FORCE_ANISO", "1").c_str()));
    int aniso = 0;
    for (int i = 0; i < 5; ++i)
        if (samples == kAnisoValues[i])
            aniso = i;
    if (combo("Anisotropic filtering", &aniso, kAniso, 5)) {
        set_env("DOL_AURORA_FORCE_ANISO", std::to_string(kAnisoValues[aniso]));
        aurora_set_forced_anisotropy(kAnisoValues[aniso]);
    }

    ImGui::Separator();
    ImGui::TextUnformatted("HD texture pack (a Dolphin pack's GZL folder):");
    ImGui::SetNextItemWidth(-160.f);
    if (ImGui::InputText("##texpack", g_texture_pack, sizeof g_texture_pack))
        g_dirty = g_restart_pending = true;
    ImGui::SameLine();
    if (ImGui::Button("None")) {
        g_texture_pack[0] = '\0';
        g_dirty = g_restart_pending = true;
    }
    restart_note();
}

void gameplay_tab() {
    if (ImGui::Checkbox("Better Wind Waker", &g_betterww))
        g_dirty = g_restart_pending = true;
    restart_note();
    if (g_options.empty()) {
        ImGui::TextDisabled("Its settings are listed once the game has started.");
    } else {
        ImGui::BeginDisabled(!g_betterww);
        ImGui::Indent();
        for (size_t i = 0; i < g_options.size(); ++i) {
            bool on = g_options[i].second;
            if (ImGui::Checkbox(g_option_titles[i].c_str(), &on)) {
                g_options[i].second = on;
                g_dirty = g_restart_pending = true;
            }
        }
        ImGui::Unindent();
        ImGui::EndDisabled();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Forest Water Challenge");
    bool keep_trees = env_on("BLUEWAKE_FOREST_WATER_KEEP_TREES", false);
    if (ImGui::Checkbox("Keep watered trees when time runs out", &keep_trees)) {
        set_env("BLUEWAKE_FOREST_WATER_KEEP_TREES", keep_trees ? "1" : "0");
        bluewake_forest_water_reload();
    }
    ImGui::TextWrapped("Forest Water still expires. Refill and continue with the remaining trees.");
    bool thirty_minutes = env_on("BLUEWAKE_FOREST_WATER_30_MINUTES", false);
    if (ImGui::Checkbox("30-minute Forest Water timer", &thirty_minutes)) {
        set_env("BLUEWAKE_FOREST_WATER_30_MINUTES", thirty_minutes ? "1" : "0");
        bluewake_forest_water_reload();
    }
    ImGui::TextWrapped("Applies the next time you collect Forest Water. Both options are off by default.");

    ImGui::Separator();
    ImGui::TextUnformatted("Doors and exits");
    int fade = std::atoi(env("BLUEWAKE_FADE_FRAMES", "6").c_str());
    if (fade <= 0 || fade > 26)
        fade = 26;
    if (ImGui::SliderInt("Fade length (game frames; 26 is the game's)", &fade, 2, 26)) {
        set_env("BLUEWAKE_FADE_FRAMES", fade >= 26 ? "0" : std::to_string(fade));
        bluewake_fast_load_reload();
    }
    bool ff = env_on("BLUEWAKE_FAST_FORWARD", true);
    if (ImGui::Checkbox("Skip through the black while loading", &ff)) {
        set_env("BLUEWAKE_FAST_FORWARD", ff ? "1" : "0");
        bluewake_fast_load_reload();
    }
    bool doors = env_on("BLUEWAKE_QUICK_DOORS", true);
    if (ImGui::Checkbox("Quick doors (no walk-in or door closing behind Link)", &doors)) {
        set_env("BLUEWAKE_QUICK_DOORS", doors ? "1" : "0");
        bluewake_quick_doors_reload();
    }

    ImGui::Separator();
    bool climb = env_on("BLUEWAKE_CLIMB", false);
    if (ImGui::Checkbox("Climb any wall, on a stamina wheel (like Breath of the Wild)", &climb)) {
        set_env("BLUEWAKE_CLIMB", climb ? "1" : "0");
        bluewake_climb_reload();
    }
    ImGui::BeginDisabled(!climb);
    float stamina = static_cast<float>(std::atof(env("BLUEWAKE_CLIMB_STAMINA", "12").c_str()));
    if (stamina < 1.f)
        stamina = 12.f;
    if (ImGui::SliderFloat("Climbing stamina", &stamina, 4.f, 30.f, "%.0f seconds")) {
        char text[16];
        std::snprintf(text, sizeof text, "%.0f", stamina);
        set_env("BLUEWAKE_CLIMB_STAMINA", text);
        bluewake_climb_reload();
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    bool jump = env_on("BLUEWAKE_JUMP_BUTTON", true);
    if (ImGui::Checkbox("Jump button (Space, left bumper)", &jump)) {
        set_env("BLUEWAKE_JUMP_BUTTON", jump ? "1" : "0");
        bluewake_jump_button_reload();
    }
    float sprint = static_cast<float>(std::atof(env("BLUEWAKE_SPRINT_SPEED", "1.5").c_str()));
    if (sprint < 1.f)
        sprint = 1.f;
    if (ImGui::SliderFloat("Sprint speed (Shift, left stick click; 1 is off)", &sprint, 1.f, 2.f, "%.2fx")) {
        char text[16];
        std::snprintf(text, sizeof text, "%.2f", sprint);
        set_env("BLUEWAKE_SPRINT_SPEED", text);
        bluewake_sprint_reload();
    }
}

void controls_tab() {
    bool mouse = env_on("BLUEWAKE_MOUSE_CAMERA", true);
    if (ImGui::Checkbox("Mouse camera (click the game to use it)", &mouse)) {
        set_env("BLUEWAKE_MOUSE_CAMERA", mouse ? "1" : "0");
        bluewake_mouse_camera_reload();
    }
    ImGui::BeginDisabled(!mouse);
    float sensitivity = static_cast<float>(std::atof(env("BLUEWAKE_MOUSE_SENSITIVITY", "1.0").c_str()));
    if (sensitivity <= 0.f)
        sensitivity = 1.f;
    if (ImGui::SliderFloat("Mouse sensitivity", &sensitivity, 0.2f, 3.f, "%.2f")) {
        char text[16];
        std::snprintf(text, sizeof text, "%.2f", sensitivity);
        set_env("BLUEWAKE_MOUSE_SENSITIVITY", text);
        bluewake_mouse_camera_reload();
    }
    bool invert = env_on("BLUEWAKE_MOUSE_INVERT_Y", false);
    if (ImGui::Checkbox("Invert the mouse's up and down", &invert)) {
        set_env("BLUEWAKE_MOUSE_INVERT_Y", invert ? "1" : "0");
        bluewake_mouse_camera_reload();
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    bool stick = env_on("BLUEWAKE_STICK_CAMERA", true);
    if (ImGui::Checkbox("Fast right-stick camera and aiming (like the mouse; click the stick for first person)",
                        &stick)) {
        set_env("BLUEWAKE_STICK_CAMERA", stick ? "1" : "0");
        bluewake_mouse_camera_reload();
    }
    ImGui::BeginDisabled(!stick);
    float speed = static_cast<float>(std::atof(env("BLUEWAKE_STICK_CAMERA_SPEED", "360").c_str()));
    if (speed <= 0.f)
        speed = 360.f;
    if (ImGui::SliderFloat("Right-stick turn speed", &speed, 120.f, 720.f, "%.0f degrees a second")) {
        char text[16];
        std::snprintf(text, sizeof text, "%.0f", speed);
        set_env("BLUEWAKE_STICK_CAMERA_SPEED", text);
        bluewake_mouse_camera_reload();
    }
    float aim = static_cast<float>(std::atof(env("BLUEWAKE_STICK_AIM_SPEED", "180").c_str()));
    if (aim <= 0.f)
        aim = 180.f;
    if (ImGui::SliderFloat("Right-stick aim speed (first person, items)", &aim, 60.f, 480.f,
                           "%.0f degrees a second")) {
        char text[16];
        std::snprintf(text, sizeof text, "%.0f", aim);
        set_env("BLUEWAKE_STICK_AIM_SPEED", text);
        bluewake_mouse_camera_reload();
    }
    bool invert_x = env_on("BLUEWAKE_STICK_CAMERA_INVERT_X", false);
    if (ImGui::Checkbox("Invert the right stick's left and right", &invert_x)) {
        set_env("BLUEWAKE_STICK_CAMERA_INVERT_X", invert_x ? "1" : "0");
        bluewake_mouse_camera_reload();
    }
    bool invert_y = env_on("BLUEWAKE_STICK_CAMERA_INVERT_Y", false);
    if (ImGui::Checkbox("Invert the right stick's up and down", &invert_y)) {
        set_env("BLUEWAKE_STICK_CAMERA_INVERT_Y", invert_y ? "1" : "0");
        bluewake_mouse_camera_reload();
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled(stick ? "In the telescope and the Picto Box the left stick (or the D-pad) zooms."
                              : "The game's right stick: its left and right follow Better Wind Waker's "
                                "\"Invert camera\" (Gameplay).");

    // Haptics (haptics.h): the game's vibration rendered from what it asked
    // for, with the triggers; or its own on-off motor; or none.
    ImGui::Separator();
    static const char* const kHaptics[] = {"Off", "Classic (the game's own on and off)",
                                           "Enhanced (shaped, with the triggers)"};
    const std::string mode = env("BLUEWAKE_HAPTICS", "enhanced");
    int haptics = mode[0] == 'o' || mode[0] == '0' ? 0 : mode[0] == 'c' ? 1 : 2;
    if (combo("Controller vibration", &haptics, kHaptics, 3)) {
        set_env("BLUEWAKE_HAPTICS", haptics == 0 ? "off" : haptics == 1 ? "classic" : "enhanced");
        bluewake_haptics_reload();
    }
    ImGui::BeginDisabled(haptics == 0);
    int strength = std::atoi(env("BLUEWAKE_HAPTICS_STRENGTH", "80").c_str());
    if (ImGui::SliderInt("Vibration strength", &strength, 0, 100, "%d%%")) {
        set_env("BLUEWAKE_HAPTICS_STRENGTH", std::to_string(strength));
        bluewake_haptics_reload();
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(haptics != 2);
    bool triggers = env_on("BLUEWAKE_HAPTICS_TRIGGERS", true);
    if (ImGui::Checkbox("Trigger feedback (Xbox impulse triggers, DualSense trigger vibration)", &triggers)) {
        set_env("BLUEWAKE_HAPTICS_TRIGGERS", triggers ? "1" : "0");
        bluewake_haptics_reload();
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled(haptics == 2   ? "Hits, falls, explosions and quakes as the game times them, shaped by their strength."
                        : haptics == 1 ? "The motor on and off, as a GameCube controller's."
                                       : "No vibration. (The game's own Vibration option turns it off too.)");

    ImGui::Separator();
    ImGui::TextUnformatted("Keyboard");
    ImGui::BulletText("WASD move, arrows the D-pad, T F G H the C-stick");
    ImGui::BulletText("J  A     K  B     U  X     I  Y     Q  Z     E  L     R  R     Enter  Start");
    ImGui::BulletText("Space jump, Shift (held) sprint");
    ImGui::BulletText("Mouse: click the game, then move to turn the camera and aim; left click is A,");
    ImGui::BulletText("the wheel zooms; Esc gives the mouse back, and Esc again opens this menu");
    ImGui::TextUnformatted("Controller");
    ImGui::BulletText("Left bumper jump, left stick click sprint (until Link stops), Back this menu");
    ImGui::BulletText("Right stick: turns the camera and aims; its click is first person (and back out)");
    ImGui::BulletText("Telescope and Picto Box: the right stick aims, the left stick (or D-pad) zooms");
}

void open_menu();
void close_menu();

void test_hook() {
    if (g_test_open_at < 0.0 || g_test_done)
        return;
    const double t = (SDL_GetTicks() - g_installed_ms) / 1000.0;
    if (!g_open && t >= g_test_open_at && t < g_test_open_at + g_test_open_for) {
        open_menu();
    } else if (g_open && t >= g_test_open_at + g_test_open_for) {
        close_menu();
        g_test_done = true;
    }
}

// The climbing stamina wheel (climb.c), beside Link in the game's picture.
void draw_climb_wheel() {
    float fraction, x, y, aspect, alpha;
    bool exhausted;
    if (!bluewake_climb_hud(&fraction, &exhausted, &x, &y, &aspect, &alpha))
        return;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    float w = display.x, h = display.y, x0 = 0.f, y0 = 0.f;
    if (h <= 0.f || aspect <= 0.f)
        return;
    if (w / h > aspect) {
        w = h * aspect;
        x0 = (display.x - w) * 0.5f;
    } else {
        h = w / aspect;
        y0 = (display.y - h) * 0.5f;
    }
    const float radius = h * 0.03f, thick = radius * 0.45f, pi = 3.14159265f;
    const ImVec2 center(x0 + x * w + radius * 2.4f, y0 + y * h - radius * 0.6f);
    ImDrawList* list = ImGui::GetForegroundDrawList();
    const auto a = [alpha](float v) { return static_cast<int>(v * alpha); };
    list->PathArcTo(center, radius, 0.f, 2.f * pi, 48);
    list->PathStroke(IM_COL32(20, 30, 20, a(150.f)), 0, thick + 3.f);
    if (fraction <= 0.002f)
        return;
    ImU32 color = IM_COL32(120, 230, 90, a(245.f)); // green
    if (exhausted) {
        const float pulse = 0.65f + 0.35f * std::sin(static_cast<float>(ImGui::GetTime()) * 8.f);
        color = IM_COL32(235, 70, 50, a(245.f * pulse)); // refilling after running out
    } else if (fraction < 0.25f) {
        color = IM_COL32(245, 190, 60, a(245.f)); // nearly out
    }
    list->PathArcTo(center, radius, -0.5f * pi, -0.5f * pi + 2.f * pi * fraction, 48);
    list->PathStroke(color, 0, thick);
}

void draw(void*) {
    test_hook();
    draw_climb_wheel();
    if (!g_open)
        return;
    ImGuiIO& io = ImGui::GetIO();
    if (!g_nav_set) {
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        g_nav_set = true;
    }
    const ImVec2 display = io.DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(display.x * 0.62f, display.y * 0.78f), ImGuiCond_Appearing);
    ImGui::SetNextWindowBgAlpha(0.94f);
    bool open = true;
    if (ImGui::Begin("Wind Waker Recomp options (paused)", &open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::SetWindowFontScale(1.4f);
        if (ImGui::BeginTabBar("##tabs")) {
            if (ImGui::BeginTabItem("Display")) {
                display_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Gameplay")) {
                gameplay_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Controls")) {
                controls_tab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::Separator();
        if (g_restart_pending)
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "Some changes take effect at the next launch.");
        if (!g_path.empty())
            ImGui::TextDisabled("Saved to %s", g_path.c_str());
        if (ImGui::Button("Resume"))
            open = false;
        ImGui::SameLine();
        // Save states (debugging): taken or put back at the game's next clean
        // point once the menu has closed (main.c's host_state_*).
        if (ImGui::Button("Save state (F5)")) {
            bluewake_save_state_hotkey(false);
            open = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Load latest state (F9)")) {
            bluewake_save_state_hotkey(true);
            open = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit the game")) {
            close_menu();
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }
    }
    ImGui::End();
    if (!open)
        close_menu();
}

} // namespace

// Whether the options menu is open, for a touch overlay that must leave the
// menu's touches to it (android/src/android_touch.c). Read from another thread.
extern "C" bool bluewake_settings_is_open(void) {
    return g_open.load(std::memory_order_relaxed);
}

extern "C" void bluewake_settings_load(void) {
    const char* chosen = std::getenv("BLUEWAKE_SETTINGS");
    if (chosen != nullptr && std::strcmp(chosen, "none") == 0)
        return;
    g_path = chosen != nullptr && chosen[0] != '\0' ? chosen : default_path();
    if (g_path.empty())
        return;
    FILE* file = std::fopen(g_path.c_str(), "r");
    if (file == nullptr)
        return;
    char line[2048];
    int count = 0;
    while (std::fgets(line, sizeof line, file) != nullptr) {
        std::string text = trim(line);
        if (text.empty() || text[0] == '#')
            continue;
        const auto equals = text.find('=');
        if (equals == std::string::npos || equals == 0)
            continue;
        const std::string key = trim(text.substr(0, equals));
        const std::string value = trim(text.substr(equals + 1));
        setenv(key.c_str(), value.c_str(), 1);
        if (!managed(key))
            g_other[key] = value;
        ++count;
    }
    std::fclose(file);
    std::fprintf(stderr, "[settings] %d settings from %s\n", count, g_path.c_str());
}

extern "C" void bluewake_settings_menu_install(void) {
    g_installed_ms = SDL_GetTicks();
    if (const char* test = std::getenv("BLUEWAKE_SETTINGS_TEST_OPEN"))
        if (std::sscanf(test, "%lf:%lf", &g_test_open_at, &g_test_open_for) != 2)
            g_test_open_at = -1.0;
    dol_aurora_set_overlay(draw, nullptr);
    dol_aurora_set_hold(hold, nullptr);
    dol_aurora_set_hold_redraw(true);
    std::fprintf(stderr, "[settings] Esc (with the mouse free), F1 or a controller's Back opens the options\n");
}

extern "C" bool bluewake_settings_menu_event(const void* sdl_event) {
    const SDL_Event* event = static_cast<const SDL_Event*>(sdl_event);
    switch (event->type) {
    case SDL_EVENT_KEY_DOWN:
        if (event->key.repeat)
            break;
        // Android's Back button or gesture reads as AC_BACK.
        if (event->key.scancode == SDL_SCANCODE_F1 || event->key.scancode == SDL_SCANCODE_AC_BACK) {
            g_open ? close_menu() : open_menu();
            return true;
        }
        if (event->key.scancode == SDL_SCANCODE_ESCAPE) {
            if (g_open) {
                close_menu();
                return true;
            }
            // Esc with the mouse as the camera gives the mouse back first.
            if (!bluewake_mouse_camera_captured()) {
                open_menu();
                return true;
            }
            return false;
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (event->gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
            g_open ? close_menu() : open_menu();
            return true;
        }
        break;
    default:
        break;
    }
    // While it is open the menu (ImGui) has the input to itself.
    return g_open;
}
