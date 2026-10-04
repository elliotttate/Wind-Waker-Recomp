// Applies the player's settings (controller_settings.h) to Aurora: the render
// resolution and, for a physical controller on port 0, the camera-stick
// direction and face-button mapping. Runs on the main thread from the frame
// tick. Aurora resets a gamepad's mapping when it connects, so the controller
// is re-configured whenever the one on port 0 changes.
#include <SDL3/SDL_gamepad.h>
#include <aurora/aurora.h>
#include <dolphin/pad.h>

#include <cstdio>

#include "controller_settings.h"

extern "C" void bluewake_padmap_install(void);  // runtime/host/src/pad_remap.cpp

namespace {

constexpr u16 kRemapPad[BW_REMAP_COUNT] = {PAD_BUTTON_A, PAD_BUTTON_B, PAD_BUTTON_X,
                                           PAD_BUTTON_Y, PAD_TRIGGER_Z, PAD_BUTTON_START};

void apply_controller(const BWSettingsSnapshot& s) {
    // Start from the controller's own defaults (so a reset takes effect and a
    // GameCube adapter keeps its layout), then apply the player's choices.
    PADRestoreDefaultMapping(0);
    // Camera stick: GameCube +X/+Y from the gamepad's right stick, the sign
    // flipped when inverted (SDL's y axis points down, GameCube's up).
    const PADAxisMapping axes[4] = {
        {{SDL_GAMEPAD_AXIS_RIGHTX, s.invert_x ? AXIS_SIGN_NEGATIVE : AXIS_SIGN_POSITIVE},
         SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_RIGHT_X_POS},
        {{SDL_GAMEPAD_AXIS_RIGHTX, s.invert_x ? AXIS_SIGN_POSITIVE : AXIS_SIGN_NEGATIVE},
         SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_RIGHT_X_NEG},
        {{SDL_GAMEPAD_AXIS_RIGHTY, s.invert_y ? AXIS_SIGN_POSITIVE : AXIS_SIGN_NEGATIVE},
         SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_RIGHT_Y_POS},
        {{SDL_GAMEPAD_AXIS_RIGHTY, s.invert_y ? AXIS_SIGN_NEGATIVE : AXIS_SIGN_POSITIVE},
         SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_RIGHT_Y_NEG},
    };
    for (const auto& axis : axes)
        PADSetAxisMapping(0, axis);
    if (s.remapped)
        for (int i = 0; i < BW_REMAP_COUNT; i++)
            PADSetButtonMapping(0, PADButtonMapping{s.native[i], kRemapPad[i]});
}

}  // namespace

extern "C" void bluewake_settings_tick(void) {
    static unsigned applied_generation;
    static int applied_controller = -2;
    static unsigned frames;
    if (applied_generation == 0u) {
        bluewake_settings_changed();
        // The host's jump and sprint leave a GameCube controller's buttons to the game.
        bluewake_padmap_install();
    }
    const BWSettingsSnapshot& s = g_bw_settings;
    const bool changed = applied_generation != s.generation;
    if (changed) {
        aurora_set_frame_buffer_scale(static_cast<float>(s.render_scale));
        if (s.anisotropy > 0)
            aurora_set_forced_anisotropy(static_cast<unsigned>(s.anisotropy));
        aurora_set_frame_interpolation(s.frame_interp);
        std::fprintf(stderr, "[settings] render scale %d, anisotropy %d, 60 fps %d, camera invert x=%d y=%d, "
                     "buttons %s\n",
                     s.render_scale, s.anisotropy, s.frame_interp, s.invert_x, s.invert_y,
                     s.remapped ? "remapped" : "default");
    }
    // A controller that connects starts from Aurora's defaults; look twice a
    // second whether the one on port 0 changed.
    if (!changed && (++frames % 15u) != 0u)
        return;
    const int controller = PADGetIndexForPort(0);
    if (changed || controller != applied_controller) {
        if (controller >= 0) {
            apply_controller(s);
            const char* name = PADGetName(0);
            std::fprintf(stderr, "[settings] controller %d (%s) configured\n", controller,
                         name != nullptr ? name : "?");
        }
        applied_controller = controller;
    }
    applied_generation = s.generation;
}
