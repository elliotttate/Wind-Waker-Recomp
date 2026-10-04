#ifndef BLUEWAKE_HAPTICS_H
#define BLUEWAKE_HAPTICS_H

#include "core/cpu.h"

#include <stdbool.h>

// Controller haptics on the Mac and Windows: the game's own vibration, felt as
// it was written. Wind Waker asks for vibration through dVibration_c (the play
// info's mVibration): a shock (StartShock: one of 26 patterns, 3 to 23 game
// frames, from a light tap to a heavy blow, a double hit or three spaced
// taps) or a quake (StartQuake: one of 12 looping patterns, sparse to dense,
// until StopQuake). The GameCube's motor only had on and off, so the game
// plays each pattern as on-off bits, one a game frame; that is all the
// controller got before (Classic). Enhanced reads the object each retrace
// instead and renders what it asked for:
//
//   a shock      both motors, following the pattern's bits with a short
//                release so each pulse is a pulse and not a click; its
//                strength from the pattern (how many frames are on), the light
//                motor sharper on the first frame; strong shocks also kick the
//                triggers (an Xbox controller's impulse triggers, a DualSense's
//                trigger vibration)
//   a quake      a continuous rumble on the heavy motor, as strong as the
//                pattern is dense, textured by its bits
//
// The game's own Vibration option, its pause and its scene changes clear the
// patterns, so they are honoured as they are; nothing is felt while a menu is
// over the game, while the window is in the background, or when the game has
// not run a frame for 150 ms (a stall, a paused menu), and every command
// expires on its own (120 ms) if the host stops sending them.
//
//   BLUEWAKE_HAPTICS=enhanced|classic|off   default enhanced (iOS: the game's own, as before)
//   BLUEWAKE_HAPTICS_STRENGTH=80            percent of full strength
//   BLUEWAKE_HAPTICS_TRIGGERS=0             no trigger rumble or trigger vibration
//   BLUEWAKE_HAPTICS_TRACE=1                log each shock and quake and the motor levels
//   BLUEWAKE_HAPTICS_TRACE=2                also the object's motor half each retrace, and
//                                           the game's own rumble flags
//   BLUEWAKE_HAPTICS_TEST=r:shock:i,r:quake:i:n,...  testing: start the game's shock i,
//                                           or quake i for n retraces, at retrace r (as
//                                           StartShock and StartQuake do, for the motor)
//   BLUEWAKE_HAPTICS_VIRTUAL=xbox|ps5       testing: a virtual controller that logs what
//                                           it is sent

// Once the guest is running (Aurora's window and SDL's gamepads exist).
void bluewake_haptics_attach(CPUState* cpu);
// Once per retrace.
void bluewake_haptics_retrace(void);
// Reads the BLUEWAKE_HAPTICS settings again (the options menu).
void bluewake_haptics_reload(void);
// A menu over the game holds the feedback (true) until it closes (false).
void bluewake_haptics_block(bool blocked);
// Whether the game's motor bits drive the controller directly (Classic).
bool bluewake_haptics_forward_motor(void);

#endif
