// The options menu (the Mac host): the settings the host reads from the
// environment, in a window over the paused game, saved to a settings file.
#include "settings_menu.h"

// The host's modules are C.
extern "C" {
#include "climb.h"
#include "controller_glyphs.h"
#include "fast_load.h"
#include "game_options.h"
#include "forest_water.h"
#include "gamepads.h"
#include "haptics.h"
#include "input_bindings.h"
#include "jump_button.h"
#include "mouse_camera.h"
#include "pad_remap.h"
#include "quick_doors.h"
#include "save_state.h"
#include "sprint.h"
}

#include "gxruntime/aurora_backend.h"

#include <SDL3/SDL.h>
#include <dolphin/pad.h>
#include <imgui.h>

#include <cmath>
#include <deque>

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
    "BLUEWAKE_MOUSE_INVERT_Y",  "BLUEWAKE_STICK_CAMERA",    "BLUEWAKE_STICK_CAMERA_SPEED",
    "BLUEWAKE_STICK_CAMERA_INVERT_X", "BLUEWAKE_STICK_CAMERA_INVERT_Y", "BLUEWAKE_STICK_AIM_SPEED",
    "BLUEWAKE_HAPTICS",         "BLUEWAKE_HAPTICS_STRENGTH", "BLUEWAKE_HAPTICS_TRIGGERS",
    "BLUEWAKE_CLIMB",           "BLUEWAKE_CLIMB_STAMINA",
    "BLUEWAKE_FOREST_WATER_KEEP_TREES", "BLUEWAKE_FOREST_WATER_30_MINUTES",
    "BLUEWAKE_PAD_PORT0",       "BLUEWAKE_PAD_PAUSE_ON_DISCONNECT",
    "BLUEWAKE_PAD_ACTIONS",     "BLUEWAKE_KEY_ACTIONS",
};

std::string g_path;                          // the settings file ("" when none)
std::map<std::string, std::string> g_other;  // its other keys, kept as they were
bool g_open = false;
bool g_dirty = false;
bool g_nav_set = false;
// The controller and keyboard mapping (Aurora's) changed: written as the menu closes.
bool g_pad_dirty = false;
// The menu opened because player 1's controller went.
bool g_opened_by_disconnect = false;

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
// BLUEWAKE_SETTINGS_TEST_TAB=controllers (testing only): the tab the menu opens on.
bool g_test_tab_controllers = false;

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

void cancel_capture();

void close_menu() {
    if (!g_open)
        return;
    cancel_capture();
    g_open = false;
    g_opened_by_disconnect = false;
    bluewake_haptics_block(false);
    if (g_pad_dirty) {
        bluewake_padmap_save();
        g_pad_dirty = false;
    }
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

    // Smooth Motion: the game's 30 frames a second, or in-between frames for
    // 60 (one each) or 120 (three each, for a 120 Hz display such as a
    // MacBook Pro's).
    static const char* const kSmooth[] = {"Off (30, the game's)", "60 frames a second",
                                          "120 frames a second (120 Hz displays)"};
    int smooth = !env_on("DOL_AURORA_FRAME_INTERP", false)                      ? 0
                 : std::atoi(env("DOL_AURORA_FRAME_INTERP_STEPS", "1").c_str()) >= 3 ? 2
                                                                                    : 1;
    if (combo("Smooth Motion", &smooth, kSmooth, 3)) {
        set_env("DOL_AURORA_FRAME_INTERP", smooth != 0 ? "1" : "0");
        set_env("DOL_AURORA_FRAME_INTERP_STEPS", smooth == 2 ? "3" : "1");
        aurora_set_frame_interp_steps(smooth == 2 ? 3 : 1);
        aurora_set_frame_interpolation(smooth != 0);
    }

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
    if (ImGui::Checkbox("Jump button (Space, left bumper; set under Controllers)", &jump)) {
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

// --- controllers ----------------------------------------------------------------

void open_menu();

const ImVec4 kWarning(1.f, 0.8f, 0.3f, 1.f);

BWPadFamily family_of(const BWGamepad* pad) {
    return pad != nullptr ? bluewake_pad_family(pad->type, pad->vendor, pad->product) : BW_FAMILY_GENERIC;
}

// Player 1's controller in the registry, or nullptr.
const BWGamepad* player_one() {
    SDL_Gamepad* pad = bluewake_gamepads_for_port(0);
    return pad != nullptr ? bluewake_gamepads_find(SDL_GetGamepadID(pad)) : nullptr;
}

const char* button_label(const BWGamepad* pad, int button) {
    return bluewake_glyph_button(family_of(pad), pad != nullptr ? pad->type : SDL_GAMEPAD_TYPE_STANDARD, button);
}

// Remapping: a binding's button listens for the input to use. A controller
// must first be let go (the press that chose the binding is not the answer),
// and while it listens ImGui does not move on the controller's input.
enum class Target { None, PadButton, PadAxis, Action, Key, AxisKey, ActionKey };
struct Capture {
    Target target = Target::None;
    unsigned id = 0; // a GameCube button or axis, or an action
    Uint64 started = 0;
    bool armed = false;
};
Capture g_capture;
bool g_nav_resume = false; // controller navigation back once the controller is let go
constexpr Uint64 kCaptureMs = 6000;

int held_button(SDL_Gamepad* pad) {
    for (int b = 0; pad != nullptr && b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
        if (SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b)))
            return b;
    return -1;
}

// An axis pulled past 60 percent (SDL's triggers rest at 0, never negative).
bool pulled_axis(SDL_Gamepad* pad, int* axis, bool* positive) {
    for (int a = 0; pad != nullptr && a < SDL_GAMEPAD_AXIS_COUNT; ++a) {
        const int value = SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(a));
        if (value >= 20000 || value <= -20000) {
            *axis = a;
            *positive = value > 0;
            return true;
        }
    }
    return false;
}

bool pad_idle(SDL_Gamepad* pad) {
    int axis;
    bool positive;
    return pad == nullptr || (held_button(pad) < 0 && !pulled_axis(pad, &axis, &positive));
}

void set_nav_gamepad(bool on) {
    ImGuiIO& io = ImGui::GetIO();
    if (on)
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    else
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
}

bool capturing_keys() {
    return g_capture.target == Target::Key || g_capture.target == Target::AxisKey ||
           g_capture.target == Target::ActionKey;
}

void start_capture(Target target, unsigned id) {
    g_capture = Capture{target, id, SDL_GetTicks(), false};
    set_nav_gamepad(false);
}

void cancel_capture() {
    if (g_capture.target == Target::None)
        return;
    g_capture = Capture{};
    g_nav_resume = true;
}

void finish_capture() {
    g_capture = Capture{};
    g_nav_resume = true;
    g_pad_dirty = true;
}

void save_actions() {
    char text[512];
    bluewake_actions_format_buttons(text, sizeof text);
    set_env("BLUEWAKE_PAD_ACTIONS", text);
    bluewake_actions_format_keys(text, sizeof text);
    set_env("BLUEWAKE_KEY_ACTIONS", text);
}

// Each frame while the menu is open.
void update_capture() {
    SDL_Gamepad* pad = bluewake_gamepads_for_port(0);
    if (g_capture.target == Target::None) {
        if (g_nav_resume && pad_idle(pad)) {
            set_nav_gamepad(true);
            g_nav_resume = false;
        }
        return;
    }
    if (SDL_GetTicks() - g_capture.started > kCaptureMs) {
        cancel_capture();
        return;
    }
    if (capturing_keys())
        return; // the key events (bluewake_settings_menu_event)
    if (pad == nullptr) {
        cancel_capture(); // the controller went
        return;
    }
    if (!g_capture.armed) {
        g_capture.armed = pad_idle(pad);
        return;
    }
    const int button = held_button(pad);
    int axis;
    bool positive;
    switch (g_capture.target) {
    case Target::PadButton:
        if (button >= 0) {
            bluewake_padmap_set_button(g_capture.id, button);
            finish_capture();
        }
        break;
    case Target::PadAxis:
        if (pulled_axis(pad, &axis, &positive)) {
            bluewake_padmap_set_axis(g_capture.id, axis, positive);
            finish_capture();
        } else if (button >= 0) {
            bluewake_padmap_set_axis_button(g_capture.id, button);
            finish_capture();
        }
        break;
    case Target::Action:
        if (button >= 0) {
            bluewake_action_set_button(static_cast<BWAction>(g_capture.id), button);
            save_actions();
            finish_capture();
        }
        break;
    default:
        break;
    }
}

// A key press while a key binding listens: true when it took it.
bool capture_key(int scancode) {
    if (!capturing_keys())
        return false;
    if (scancode == SDL_SCANCODE_ESCAPE) {
        cancel_capture();
        return true;
    }
    switch (g_capture.target) {
    case Target::Key:
        bluewake_padmap_set_key(g_capture.id, scancode);
        break;
    case Target::AxisKey:
        bluewake_padmap_set_axis_key(g_capture.id, scancode);
        break;
    case Target::ActionKey:
        bluewake_action_set_key(static_cast<BWAction>(g_capture.id), scancode);
        save_actions();
        break;
    default:
        break;
    }
    finish_capture();
    return true;
}

// A binding: its input as a button that listens for a new one when chosen.
void binding_button(const char* current, Target target, unsigned id, const char* prompt) {
    ImGui::PushID(static_cast<int>(target));
    ImGui::PushID(static_cast<int>(id));
    if (g_capture.target == target && g_capture.id == id) {
        const Uint64 elapsed = SDL_GetTicks() - g_capture.started;
        const double left = elapsed < kCaptureMs ? (kCaptureMs - elapsed) / 1000.0 : 0.0;
        ImGui::TextColored(kWarning, "%s... %.0f", g_capture.armed || capturing_keys() ? prompt : "Let go", left);
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel"))
            cancel_capture();
    } else {
        const std::string label = std::string(current) + "##bind";
        if (ImGui::Button(label.c_str(), ImVec2(ImGui::GetFontSize() * 11.f, 0.f)))
            start_capture(target, id);
    }
    ImGui::PopID();
    ImGui::PopID();
}

std::string power_text(SDL_Gamepad* pad) {
    int percent = -1;
    const SDL_PowerState state = pad != nullptr ? SDL_GetGamepadPowerInfo(pad, &percent) : SDL_POWERSTATE_UNKNOWN;
    char text[32];
    switch (state) {
    case SDL_POWERSTATE_ON_BATTERY:
        std::snprintf(text, sizeof text, percent >= 0 ? "%d%%" : "battery", percent);
        return text;
    case SDL_POWERSTATE_CHARGING:
        std::snprintf(text, sizeof text, percent >= 0 ? "%d%% (charging)" : "charging", percent);
        return text;
    case SDL_POWERSTATE_CHARGED:
        return "charged";
    case SDL_POWERSTATE_NO_BATTERY:
        return "wired";
    default:
        return "";
    }
}

void controllers_list() {
    ImGui::TextUnformatted("Connected controllers");
    const int count = bluewake_gamepads_count();
    if (count == 0) {
        ImGui::TextDisabled("None. Connect one at any time: it is set up as it connects.");
    } else if (ImGui::BeginTable("##pads", 5, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        for (int i = 0; i < count; ++i) {
            const BWGamepad* pad = bluewake_gamepads_at(i);
            ImGui::PushID(static_cast<int>(pad->id));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(pad->name);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", bluewake_pad_family_name(family_of(pad)));
            ImGui::TableNextColumn();
            if (pad->port >= 0)
                ImGui::Text("Player %d", pad->port + 1);
            else
                ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", power_text(SDL_GetGamepadFromID(pad->id)).c_str());
            ImGui::TableNextColumn();
            if (pad->port != 0 && ImGui::SmallButton("Make player 1"))
                bluewake_gamepads_use_for_port0(pad->id);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    static const char* const kPolicies[] = {"Automatic (the controller used or connected last)",
                                            "Last used (a press on another controller takes over)",
                                            "Fixed (the one made player 1 here, kept across launches)"};
    static const char* const kPolicyValues[] = {"auto", "last", "fixed"};
    int policy = static_cast<int>(bluewake_gamepads_policy());
    if (combo("Player 1's controller", &policy, kPolicies, 3)) {
        const bool was_fixed = bluewake_gamepads_policy() == BW_PORT0_FIXED;
        set_env("BLUEWAKE_PAD_PORT0", kPolicyValues[policy]);
        if (was_fixed && policy != BW_PORT0_FIXED)
            bluewake_padmap_forget_port0();
        bluewake_gamepads_reload();
        if (policy == BW_PORT0_FIXED)
            if (const BWGamepad* one = player_one())
                bluewake_padmap_persist_port0(one->id);
    }
    bool pause = env_on("BLUEWAKE_PAD_PAUSE_ON_DISCONNECT", true);
    if (ImGui::Checkbox("Pause (open this menu) when player 1's controller disconnects", &pause)) {
        set_env("BLUEWAKE_PAD_PAUSE_ON_DISCONNECT", pause ? "1" : "0");
        bluewake_gamepads_reload();
    }
}

void game_buttons(const BWGamepad* pad) {
    if (!ImGui::BeginTable("##gc-buttons", 2, ImGuiTableFlags_SizingStretchProp))
        return;
    for (int i = 0; i < bluewake_padmap_button_count(); ++i) {
        const unsigned gc = bluewake_padmap_button_at(i);
        const int native = bluewake_padmap_button_native(gc);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(bluewake_padmap_button_title(i));
        ImGui::TableNextColumn();
        binding_button(native >= 0 ? button_label(pad, native) : "-", Target::PadButton, gc, "Press a button");
        if (native < 0 && (gc == PAD_TRIGGER_L || gc == PAD_TRIGGER_R) && g_capture.target == Target::None) {
            ImGui::SameLine();
            ImGui::TextDisabled("(the trigger, pressed fully)");
        }
    }
    ImGui::EndTable();
    if (ImGui::Button("This controller's defaults")) {
        bluewake_padmap_reset();
        g_pad_dirty = true;
    }
    if (family_of(pad) == BW_FAMILY_NINTENDO) {
        ImGui::SameLine();
        if (ImGui::Button("A, B, X and Y as printed")) {
            bluewake_padmap_nintendo_labels();
            g_pad_dirty = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(the defaults go by position, as on a GameCube)");
    }
}

std::string axis_label(const BWGamepad* pad, unsigned pad_axis) {
    int axis, button;
    bool positive;
    if (!bluewake_padmap_axis_native(pad_axis, &axis, &positive, &button))
        return "-";
    return axis >= 0 ? bluewake_glyph_axis(family_of(pad), axis, positive) : button_label(pad, button);
}

int percent_of(u16 value) { return static_cast<int>(std::lround(value * 100.0 / 32767.0)); }
u16 value_of(int percent) { return static_cast<u16>(std::lround(percent * 32767.0 / 100.0)); }

void sticks(const BWGamepad* pad) {
    if (ImGui::BeginTable("##gc-axes", 2, ImGuiTableFlags_SizingStretchProp)) {
        for (int i = 0; i < bluewake_padmap_axis_count(); ++i) {
            const unsigned axis = bluewake_padmap_axis_at(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(bluewake_padmap_axis_title(i));
            ImGui::TableNextColumn();
            binding_button(axis_label(pad, axis).c_str(), Target::PadAxis, axis, "Tilt a stick or pull a trigger");
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Swap the sticks")) {
        bluewake_padmap_swap_sticks();
        g_pad_dirty = true;
    }

    PADDeadZones* zones = PADGetDeadZones(0);
    if (zones == nullptr)
        return;
    ImGui::Separator();
    bool use = zones->useDeadzones;
    if (ImGui::Checkbox("Dead zones (a stick near the middle reads as centred)", &use)) {
        zones->useDeadzones = use;
        g_pad_dirty = true;
    }
    ImGui::BeginDisabled(!use);
    int stick = percent_of(zones->stickDeadZone);
    if (ImGui::SliderInt("Stick dead zone", &stick, 0, 50, "%d%%")) {
        zones->stickDeadZone = value_of(stick);
        g_pad_dirty = true;
    }
    int substick = percent_of(zones->substickDeadZone);
    if (ImGui::SliderInt("C-stick dead zone", &substick, 0, 50, "%d%%")) {
        zones->substickDeadZone = value_of(substick);
        g_pad_dirty = true;
    }
    ImGui::EndDisabled();
    bool emulate = zones->emulateTriggers;
    if (ImGui::Checkbox("A trigger pulled far enough is also L or R's full press", &emulate)) {
        zones->emulateTriggers = emulate;
        g_pad_dirty = true;
    }
    ImGui::BeginDisabled(!emulate);
    int left = percent_of(zones->leftTriggerActivationZone);
    if (ImGui::SliderInt("L's full press at", &left, 30, 100, "%d%%")) {
        zones->leftTriggerActivationZone = value_of(left);
        g_pad_dirty = true;
    }
    int right = percent_of(zones->rightTriggerActivationZone);
    if (ImGui::SliderInt("R's full press at", &right, 30, 100, "%d%%")) {
        zones->rightTriggerActivationZone = value_of(right);
        g_pad_dirty = true;
    }
    ImGui::EndDisabled();
}

void host_actions(const BWGamepad* pad) {
    if (ImGui::BeginTable("##actions", 4, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Action");
        ImGui::TableSetupColumn(pad != nullptr ? "Controller" : "Controller (none connected)");
        ImGui::TableSetupColumn("Keyboard");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (int i = 0; i < BW_ACTION_COUNT; ++i) {
            const BWAction action = static_cast<BWAction>(i);
            const int button = bluewake_action_button(action);
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(bluewake_action_title(action));
            if (action != BW_ACTION_MENU && pad != nullptr && bluewake_padmap_uses_native_button(button)) {
                ImGui::TextColored(kWarning, pad->gamecube ? "(the game's button: off on this controller)"
                                                           : "(also one of the game's buttons)");
            }
            ImGui::TableNextColumn();
            binding_button(button >= 0 ? button_label(pad, button) : "-", Target::Action, static_cast<unsigned>(i),
                           "Press a button");
            ImGui::TableNextColumn();
            binding_button(bluewake_glyph_key(bluewake_action_key(action)), Target::ActionKey,
                           static_cast<unsigned>(i), "Press a key");
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("No button")) {
                bluewake_action_set_button(action, -1);
                save_actions();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Defaults##actions")) {
        bluewake_actions_reset();
        save_actions();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Esc, and Start with Back held for a second, always open this menu.");
}

void keyboard_keys() {
    if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_SizingStretchProp)) {
        for (int i = 0; i < bluewake_padmap_button_count(); ++i) {
            const unsigned gc = bluewake_padmap_button_at(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(bluewake_padmap_button_title(i));
            ImGui::TableNextColumn();
            binding_button(bluewake_glyph_key(bluewake_padmap_key(gc)), Target::Key, gc, "Press a key");
        }
        for (int i = 0; i < bluewake_padmap_axis_count(); ++i) {
            const unsigned axis = bluewake_padmap_axis_at(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(bluewake_padmap_axis_title(i));
            ImGui::TableNextColumn();
            binding_button(bluewake_glyph_key(bluewake_padmap_axis_key(axis)), Target::AxisKey, axis, "Press a key");
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Defaults (W A S D, J K U I...)")) {
        bluewake_padmap_reset_keys();
        g_pad_dirty = true;
    }
}

// The controller as the game sees it through the mapping: its buttons lit,
// its sticks with their dead zones, its triggers.
void input_test() {
    for (int i = 0; i < bluewake_padmap_button_count(); ++i) {
        const unsigned gc = bluewake_padmap_button_at(i);
        if (i > 0)
            ImGui::SameLine();
        const bool down = bluewake_padmap_button_down(gc);
        ImGui::TextColored(down ? ImVec4(0.4f, 1.f, 0.4f, 1.f) : ImVec4(0.5f, 0.5f, 0.5f, 1.f), "%s",
                           bluewake_padmap_button_title(i));
    }
    const PADDeadZones* zones = PADGetDeadZones(0);
    const float radius = ImGui::GetFontSize() * 2.2f;
    ImDrawList* list = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    for (int stick = 0; stick < 2; ++stick) {
        double x, y;
        bluewake_padmap_stick(stick, &x, &y);
        const ImVec2 centre(origin.x + radius + stick * radius * 3.f, origin.y + radius + 4.f);
        list->AddCircle(centre, radius, IM_COL32(160, 160, 160, 255), 32, 1.5f);
        if (zones != nullptr && zones->useDeadzones) {
            const float zone = (stick == 0 ? zones->stickDeadZone : zones->substickDeadZone) / 32767.f;
            list->AddCircleFilled(centre, radius * zone, IM_COL32(120, 120, 120, 90), 24);
        }
        list->AddCircleFilled(ImVec2(centre.x + static_cast<float>(x) * radius, centre.y + static_cast<float>(y) * radius),
                              radius * 0.15f, stick == 0 ? IM_COL32(230, 230, 230, 255) : IM_COL32(250, 210, 60, 255));
    }
    for (int trigger = 0; trigger < 2; ++trigger) {
        const float value = static_cast<float>(bluewake_padmap_trigger(trigger));
        const ImVec2 top(origin.x + radius * 6.5f + trigger * radius * 0.9f, origin.y + 4.f);
        const ImVec2 bottom(top.x + radius * 0.5f, top.y + radius * 2.f);
        list->AddRect(top, bottom, IM_COL32(160, 160, 160, 255));
        list->AddRectFilled(ImVec2(top.x, bottom.y - (bottom.y - top.y) * value), bottom, IM_COL32(120, 200, 255, 255));
    }
    ImGui::Dummy(ImVec2(radius * 8.f, radius * 2.f + 8.f));
    ImGui::TextDisabled("The stick and C-stick (yellow), and L and R as the game reads them.");
}

void controllers_tab() {
    controllers_list();
    ImGui::Separator();
    const BWGamepad* pad = player_one();
    if (pad == nullptr || !bluewake_padmap_has_controller()) {
        ImGui::TextDisabled("Connect a controller, or make one player 1, to set its buttons.");
    } else {
        ImGui::Text("%s (%s): saved for this model of controller", pad->name,
                    bluewake_pad_family_name(family_of(pad)));
        if (ImGui::CollapsingHeader("The game's buttons", ImGuiTreeNodeFlags_DefaultOpen))
            game_buttons(pad);
        if (ImGui::CollapsingHeader("Sticks, triggers and dead zones"))
            sticks(pad);
        if (ImGui::CollapsingHeader("Test the controller"))
            input_test();
    }
    if (ImGui::CollapsingHeader("Wind Waker Recomp's buttons", ImGuiTreeNodeFlags_DefaultOpen))
        host_actions(pad);
    if (ImGui::CollapsingHeader("Keyboard (the game's buttons)"))
        keyboard_keys();
}

// Notices of controllers coming and going, in a corner for a few seconds.
struct Notice {
    std::string text;
    Uint64 at;
};
std::deque<Notice> g_notices;

void draw_notices() {
    char text[128];
    while (bluewake_gamepads_take_notice(text, sizeof text)) {
        g_notices.push_back(Notice{text, SDL_GetTicks()});
        if (g_notices.size() > 4)
            g_notices.pop_front();
    }
    const Uint64 now = SDL_GetTicks();
    while (!g_notices.empty() && now - g_notices.front().at > 3500)
        g_notices.pop_front();
    if (g_notices.empty())
        return;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x - 16.f, 16.f), ImGuiCond_Always, ImVec2(1.f, 0.f));
    ImGui::SetNextWindowBgAlpha(0.7f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##controller-notices", nullptr, flags)) {
        ImGui::SetWindowFontScale(1.2f);
        for (const Notice& notice : g_notices) {
            const float age = (now - notice.at) / 1000.f;
            const float alpha = age > 3.f ? 1.f - (age - 3.f) / 0.5f : 1.f;
            ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, alpha), "%s", notice.text.c_str());
        }
    }
    ImGui::End();
}

// Start and Back held together for a second open the menu, whatever the
// buttons are set to.
void menu_combo() {
    static Uint64 since = 0;
    static bool fired = false;
    SDL_Gamepad* pad = bluewake_gamepads_player_pad();
    const bool both = pad != nullptr && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START) &&
                      SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK);
    if (!both) {
        since = 0;
        fired = false;
        return;
    }
    if (since == 0)
        since = SDL_GetTicks();
    if (!fired && SDL_GetTicks() - since >= 1000) {
        fired = true;
        if (!g_open)
            open_menu();
    }
}

// The controls as they are set now, for the Controls tab.
void controls_summary() {
    const BWGamepad* pad = player_one();
    ImGui::TextUnformatted("Keyboard");
    ImGui::BulletText("The game's buttons: Controllers > Keyboard (W A S D move, J K U I are A B X Y)");
    ImGui::BulletText("Jump %s, sprint (held) %s, this menu %s or Esc",
                      bluewake_glyph_key(bluewake_action_key(BW_ACTION_JUMP)),
                      bluewake_glyph_key(bluewake_action_key(BW_ACTION_SPRINT)),
                      bluewake_glyph_key(bluewake_action_key(BW_ACTION_MENU)));
    ImGui::BulletText("Mouse: click the game, then move to turn the camera and aim; left click is A,");
    ImGui::BulletText("the wheel zooms; Esc gives the mouse back, and Esc again opens this menu");
    if (pad != nullptr)
        ImGui::Text("Controller: %s (%s)", pad->name, bluewake_pad_family_name(family_of(pad)));
    else
        ImGui::TextUnformatted("Controller (none connected)");
    ImGui::BulletText("Jump %s, sprint %s (until Link stops), this menu %s",
                      button_label(pad, bluewake_action_button(BW_ACTION_JUMP)),
                      button_label(pad, bluewake_action_button(BW_ACTION_SPRINT)),
                      button_label(pad, bluewake_action_button(BW_ACTION_MENU)));
    ImGui::BulletText("Right stick: turns the camera and aims; %s is first person (and back out)",
                      button_label(pad, bluewake_action_button(BW_ACTION_FIRST_PERSON)));
    ImGui::BulletText("Telescope and Picto Box: the right stick aims; the left stick, %s and %s zoom",
                      button_label(pad, bluewake_action_button(BW_ACTION_ZOOM_IN)),
                      button_label(pad, bluewake_action_button(BW_ACTION_ZOOM_OUT)));
    ImGui::BulletText("Everything can be changed under Controllers.");
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
    controls_summary();
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
    // Controllers that came or went (gamepads.h), and the pause when player 1's goes.
    bluewake_gamepads_sync();
    if (bluewake_gamepads_take_port0_lost()) {
        open_menu();
        g_opened_by_disconnect = true;
    }
    menu_combo();
    draw_climb_wheel();
    draw_notices();
    if (!g_open)
        return;
    ImGuiIO& io = ImGui::GetIO();
    if (!g_nav_set) {
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        g_nav_set = true;
    }
    update_capture();
    const ImVec2 display = io.DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(display.x * 0.62f, display.y * 0.78f), ImGuiCond_Appearing);
    ImGui::SetNextWindowBgAlpha(0.94f);
    bool open = true;
    if (ImGui::Begin("Wind Waker Recomp options (paused)", &open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::SetWindowFontScale(1.4f);
        if (g_opened_by_disconnect)
            ImGui::TextColored(kWarning, bluewake_gamepads_for_port(0) != nullptr
                                             ? "Player 1 has a controller again: Resume when ready."
                                             : "Player 1's controller disconnected. Connect one (or play on the "
                                               "keyboard) and Resume.");
        if (ImGui::BeginTabBar("##tabs")) {
            if (ImGui::BeginTabItem("Display")) {
                display_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Gameplay")) {
                gameplay_tab();
                ImGui::EndTabItem();
            }
            const ImGuiTabItemFlags controllers_flags =
                g_test_tab_controllers ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            g_test_tab_controllers = false;
            if (ImGui::BeginTabItem("Controllers", nullptr, controllers_flags)) {
                controllers_tab();
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
    if (const char* tab = std::getenv("BLUEWAKE_SETTINGS_TEST_TAB"))
        g_test_tab_controllers = std::strcmp(tab, "controllers") == 0;
    if (const char* test = std::getenv("BLUEWAKE_SETTINGS_TEST_OPEN"))
        if (std::sscanf(test, "%lf:%lf", &g_test_open_at, &g_test_open_for) != 2)
            g_test_open_at = -1.0;
    // The controllers: the host's buttons, Aurora's mapping, and player 1.
    bluewake_actions_reload();
    bluewake_padmap_install();
    bluewake_gamepads_reload();
    bluewake_gamepads_sync();
    dol_aurora_set_overlay(draw, nullptr);
    dol_aurora_set_hold(hold, nullptr);
    dol_aurora_set_hold_redraw(true);
    std::fprintf(stderr, "[settings] Esc (with the mouse free), F1 or a controller's Back (or Start and Back held) "
                         "opens the options\n");
}

extern "C" bool bluewake_settings_menu_event(const void* sdl_event) {
    const SDL_Event* event = static_cast<const SDL_Event*>(sdl_event);
    switch (event->type) {
    case SDL_EVENT_KEY_DOWN:
        if (event->key.repeat)
            break;
        // A key binding listening takes the key; Esc stops any binding listening.
        if (g_open && capture_key(event->key.scancode))
            return true;
        if (g_open && g_capture.target != Target::None && event->key.scancode == SDL_SCANCODE_ESCAPE) {
            cancel_capture();
            return true;
        }
        if (bluewake_action_pressed_by(BW_ACTION_MENU, event)) {
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
        // A binding listening (or the controller not yet let go after one) has the controller.
        if (g_open && (g_capture.target != Target::None || g_nav_resume))
            return true;
        // The menu's button, on any controller.
        if (bluewake_action_button(BW_ACTION_MENU) >= 0 &&
            event->gbutton.button == bluewake_action_button(BW_ACTION_MENU)) {
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
