// BlueWake Android touch controls: the native end of TouchControlsView.java.
//
// The Java overlay draws the controls and tracks the fingers; each change of
// its state arrives here and becomes Aurora's virtual pad on port 1, as the iOS
// overlay's does (apple/ios/src/touch_controls.cpp). A connected controller
// takes over while the overlay publishes nothing.
#include <jni.h>
#include <stdbool.h>
#include <stdio.h>

#include <dolphin/pad.h>

// Button bits, as TouchControlsView.java sends them (the iOS overlay's).
enum {
    TOUCH_DPAD_LEFT = 1 << 0,
    TOUCH_DPAD_RIGHT = 1 << 1,
    TOUCH_DPAD_DOWN = 1 << 2,
    TOUCH_DPAD_UP = 1 << 3,
    TOUCH_Z = 1 << 4,
    TOUCH_R = 1 << 5,
    TOUCH_L = 1 << 6,
    TOUCH_A = 1 << 8,
    TOUCH_B = 1 << 9,
    TOUCH_X = 1 << 10,
    TOUCH_Y = 1 << 11,
    TOUCH_START = 1 << 12,
};

static u16 to_pad_buttons(int bits) {
    u16 out = 0;
    if (bits & TOUCH_DPAD_LEFT) out |= PAD_BUTTON_LEFT;
    if (bits & TOUCH_DPAD_RIGHT) out |= PAD_BUTTON_RIGHT;
    if (bits & TOUCH_DPAD_DOWN) out |= PAD_BUTTON_DOWN;
    if (bits & TOUCH_DPAD_UP) out |= PAD_BUTTON_UP;
    if (bits & TOUCH_Z) out |= PAD_TRIGGER_Z;
    if (bits & TOUCH_R) out |= PAD_TRIGGER_R;
    if (bits & TOUCH_L) out |= PAD_TRIGGER_L;
    if (bits & TOUCH_A) out |= PAD_BUTTON_A;
    if (bits & TOUCH_B) out |= PAD_BUTTON_B;
    if (bits & TOUCH_X) out |= PAD_BUTTON_X;
    if (bits & TOUCH_Y) out |= PAD_BUTTON_Y;
    if (bits & TOUCH_START) out |= PAD_BUTTON_START;
    return out;
}

static s8 scale_stick(int v) {
    if (v > 127) v = 127;
    if (v < -127) v = -127;
    // GameCube sticks read about +/-100 at full tilt.
    return (s8)(v * 100 / 127);
}

JNIEXPORT void JNICALL Java_dev_bluewake_android_TouchControlsView_nativePublish(
    JNIEnv* env, jclass clazz, jint buttons, jint stick_x, jint stick_y, jint c_x, jint c_y) {
    (void)env;
    (void)clazz;
    if (buttons == 0 && stick_x == 0 && stick_y == 0 && c_x == 0 && c_y == 0) {
        PADClearVirtualStatus(PAD_CHAN0);
        return;
    }
    PADStatus status = {0};
    status.button = to_pad_buttons(buttons);
    status.stickX = scale_stick(stick_x);
    status.stickY = scale_stick(stick_y);
    status.substickX = scale_stick(c_x);
    status.substickY = scale_stick(c_y);
    // Touch L/R are ordinary buttons: the digital click plus full pressure.
    if (buttons & TOUCH_L) status.triggerLeft = 255;
    if (buttons & TOUCH_R) status.triggerRight = 255;
    if (buttons & TOUCH_A) status.analogA = 255;
    if (buttons & TOUCH_B) status.analogB = 255;
    PADSetVirtualStatus(PAD_CHAN0, &status);
}

// While the options menu is open, a touch away from the controls is the
// menu's (runtime/host/src/settings_menu.cpp).
bool bluewake_settings_is_open(void);

JNIEXPORT jboolean JNICALL Java_dev_bluewake_android_TouchControlsView_nativeMenuOpen(JNIEnv* env, jclass clazz) {
    (void)env;
    (void)clazz;
    return bluewake_settings_is_open() ? JNI_TRUE : JNI_FALSE;
}
