/* (the seventh set: see native_leaf_run.h) */
#include "native_rot.h"
#include "native_leaf_run.h"

#include <stdio.h>

int bluewake_native_rot_enabled;

/* A run's undo log (the game thread's: the natives run only there, and none
 * runs inside another). Static, so a native's frame stays small. */
static GxLog s_lf_log;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-label"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#include "native_rot_gen.inc"
#pragma clang diagnostic pop

#define LF_NATIVES ROT_NATIVES
#define LF_TAG "[native-rot]"
#define LF_ENTRY(id) bluewake_native_rot_##id
#define LF_REPORT bluewake_native_rot_report
#define LF_BY_ADDRESS bluewake_native_rot
#include "native_leaf_group.inc"
