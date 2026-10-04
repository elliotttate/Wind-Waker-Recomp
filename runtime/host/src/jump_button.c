#include "jump_button.h"
#include "input_bindings.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// Wind Waker has no jump button: Link jumps only when he runs off a ledge.
// Each frame daPy_lk_c::execute (GZLE01 0x80121870) first lets
// changeAutoJumpProc switch procs (running off an edge becomes
// procAutoJump_init, 0x80115EA4) and then calls the current proc once,
// through its member-function pointer: (this->*mCurProcFunc)(), whose
// __ptmf_scall ends in a bctr - a dispatch boundary the chassis stops at.
// procWait, procFreeWait and procMove are the procs of Link standing, idling
// and walking or running under the player's control. A press is decided at
// Link's next proc call: when it enters one of those three and Link is on the
// ground with nothing else going on, this goes to procAutoJump_init instead,
// with the same this (r3) and return address (lr) - a tail call, as if execute
// had called the jump's init in that proc's place - and otherwise the press is
// dropped. The init returns to execute, which moves Link and checks the ground
// as every frame; from the next frame procAutoJump flies the jump and lands it
// (changeLandProc), as for a jump off a ledge. The jump takes Link's speed, at
// least the auto jump's least (so a jump from standing is a short hop), and
// like the auto jump it goes the way he faces.
enum {
    kProcWait = 0x80113044u,         // procWait__9daPy_lk_cFv
    kProcFreeWait = 0x801134A0u,     // procFreeWait__9daPy_lk_cFv
    kProcMove = 0x80113628u,         // procMove__9daPy_lk_cFv
    kProcAutoJumpInit = 0x80115EA4u, // procAutoJump_init__9daPy_lk_cFv
    kExecute = 0x80121870u,          // execute__9daPy_lk_cFv, to 0x80122D30
    kExecuteEnd = 0x80122D30u,
    kPlayerPointer = 0x803CA74Cu,    // dComIfGp_getPlayer(0)
    kEventMode = 0x803C9EA2u,        // g_dComIfG_gameInfo.play.mEvtCtrl's mode
    kMenuPause = 0x803F7097u,        // dMenu_pause (dMenu_flag): a menu is open

    // fopAc_ac_c / daPy_py_c / daPy_lk_c
    kPos = 0x1F8u,              // current.pos
    kSpeed = 0x220u,            // speed
    kSpeedF = 0x254u,           // speedF
    kNoResetFlg0 = 0x29Cu,      // mNoResetFlg0
    kGrabWear = 0x2B0u,         // field_0x2b0 < 0: checkGrabWear
    kDemoType = 0x304u,         // mDemo.mDemoType (u16)
    kDemoMode = 0x314u,         // mDemo.mDemoMode
    kAcch = 0x46Cu,             // mAcch (dBgS_LinkAcch)
    kAcchFlags = kAcch + 0x28u, // dBgS_Acch::m_flags
    kAcchPos = kAcch + 0x2Cu,   // dBgS_Acch::pm_pos (&current.pos)
    kGrabActor = 0x3190u,       // mActorKeepGrab.mActor
    kCurProc = 0x31D8u,         // mCurProc
    kCurProcFunc = 0x31E4u,     // mCurProcFunc's function (__ptmf f_data)
    kAttention = 0x3480u,       // mpAttention
    kGroundCodeOld = 0x357Cu,   // m357C: the last frame's ground code
    kGroundCode = 0x3580u,      // m3580: this frame's ground code
    kModeFlg = 0x3618u,         // mModeFlg

    // dAttention_c
    kAttnPlayer = 0x00u,    // mpPlayer
    kAttnLockState = 0x18u, // mLockOnState: 0 none, 1 locked, 2 releasing
    kAttnFlags = 0x20u,     // mFlags

    kProcWaitId = 0x04u,     // daPyProc_WAIT_e
    kProcFreeWaitId = 0x05u, // daPyProc_FREE_WAIT_e
    kProcMoveId = 0x06u,     // daPyProc_MOVE_e

    // For BLUEWAKE_JUMP_TEST_TARGET: the play scene's execute runs the
    // attention (dComIfGp_getAttention().Run), which reads
    // g_mDoCPd_cpadInfo[0].mHoldLockL - the game's own L-held state.
    kPlaySceneExecute = 0x80234FD0u, // dScnPly_Execute__FP13dScnPly_ply_c
    kPadHoldLockL = 0x803A4DF0u + 0x35u,
    kPadTrigLockL = 0x803A4DF0u + 0x36u,
};

// Modes Link must not be in: MIDAIR, DAMAGE, WHIDE (barrel), HANG, HOOKSHOT,
// ROPE, IN_SHIP, CLIMB, SWIM, GRAB, PUSHPULL, LADDER, CROUCH, CRAWL, CAUGHT,
// SUBJECT (first person), PARRY.
static const u32 kRefusedModes = 0x00000002u | 0x00000008u | 0x00000010u | 0x00000020u | 0x00000200u |
                                 0x00000800u | 0x00002000u | 0x00010000u | 0x00040000u | 0x00100000u |
                                 0x00200000u | 0x00400000u | 0x00800000u | 0x01000000u | 0x10000000u |
                                 0x20000000u | 0x80000000u;
static const u32 kGroundHit = 0x20u;                 // dBgS_Acch GROUND_HIT
static const u32 kHeavy = 0x40000000u | 0x02000000u; // daPyFlg0_HEAVY_STATE, _EQUIP_HEAVY_BOOTS
// dAttention_c: 0x10000000 L held, 0x20000000 L held with nothing to lock on.
static const u32 kAttnTargeting = 0x20000000u | 0x10000000u;
// Ground code 3: an edge the game will not auto jump off (changeAutoJumpProc
// tests m357C != 3); Link does not jump from it either.
static const u32 kGroundNoJump = 3u;

// A press acts on Link's next frame: at his next proc call he jumps if he can,
// and otherwise the press is dropped, not kept for later (so a press in the
// air, in a roll or in an event never jumps once it is over). If his code does
// not run at all within this many retraces (0.1 s, three of the game's frames:
// a menu, a pause, a scene change), the press lapses too.
static const unsigned long long kWindow = 6u;

bool bluewake_jump_button_armed;
static bool g_pending;
static bool g_enabled = true;
static bool g_trace;
static CPUState* g_cpu;
static atomic_uint g_presses; // jump key presses, from the event thread
static unsigned g_presses_seen;
static unsigned long long g_retrace, g_deadline;
static unsigned g_follow; // retraces to trace after a jump
static unsigned long long g_jumps;
// Player 1's jump button (input_bindings.h; the left bumper unless chosen
// otherwise) jumps too. A GameCube controller's buttons are the game's: on
// one, a button the game's mapping uses does not jump.
static bool g_bumper_was_down;

// BLUEWAKE_JUMP_TEST=retrace,...: presses without a keyboard.
// BLUEWAKE_JUMP_TEST_TARGET=retrace:length: L held (targeting) meanwhile.
static unsigned long long g_test[32];
static unsigned g_test_count;
static unsigned long long g_target_start, g_target_length;
static bool g_targeting;

static bool guest_pointer(u32 address) { return address >= 0x80000000u && address < 0x81800000u; }

static float read_f32(CPUState* cpu, u32 address) {
    const u32 bits = mem_read32(cpu, address);
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

// The boundary is Link's proc call for this frame: execute's
// (this->*mCurProcFunc)(), __ptmf_scall's bctr to his current proc.
static bool proc_call(CPUState* cpu, u32 address) {
    const u32 player = cpu->gpr[3];
    return (cpu->ctr & ~3u) == address && cpu->lr >= kExecute && cpu->lr < kExecuteEnd &&
           guest_pointer(player) && player == mem_read32(cpu, kPlayerPointer) &&
           mem_read32(cpu, player + kCurProcFunc) == address;
}

// Why Link cannot jump from the proc he is entering at `address`, or NULL.
static const char* refusal(CPUState* cpu, u32 address) {
    const u32 player = cpu->gpr[3];
    const u32 proc = mem_read32(cpu, player + kCurProc);
    if (!(address == kProcWait && proc == kProcWaitId) && !(address == kProcFreeWait && proc == kProcFreeWaitId) &&
        !(address == kProcMove && proc == kProcMoveId))
        return "Link is not standing or moving";
    if (cpu->exception != 0u)
        return "an exception is pending";
    // The player is laid out as these offsets say.
    if (mem_read32(cpu, player + kAcchPos) != player + kPos)
        return "unexpected player layout";
    const u32 attention = mem_read32(cpu, player + kAttention);
    if (!guest_pointer(attention) || mem_read32(cpu, attention + kAttnPlayer) != player)
        return "unexpected attention layout";

    if (mem_read8(cpu, kEventMode) != 0u)
        return "an event is running";
    if (mem_read16(cpu, player + kDemoType) != 0u || mem_read32(cpu, player + kDemoMode) != 0u)
        return "Link is in a demo";
    if ((mem_read32(cpu, player + kModeFlg) & kRefusedModes) != 0u)
        return "Link's mode does not allow it";
    if ((mem_read32(cpu, player + kAcchFlags) & kGroundHit) == 0u)
        return "Link is not on the ground";
    if (mem_read32(cpu, player + kGrabActor) != 0u || read_f32(cpu, player + kGrabWear) < 0.0f)
        return "Link is carrying something";
    if ((mem_read32(cpu, player + kNoResetFlg0) & kHeavy) != 0u)
        return "Link is heavy (iron boots)";
    if (mem_read32(cpu, player + kGroundCode) == kGroundNoJump ||
        mem_read32(cpu, player + kGroundCodeOld) == kGroundNoJump)
        return "the ground here has no jump";
    if (mem_read8(cpu, attention + kAttnLockState) != 0u ||
        (mem_read32(cpu, attention + kAttnFlags) & kAttnTargeting) != 0u)
        return "Link is targeting";
    return NULL;
}

static void update_armed(void) { bluewake_jump_button_armed = g_pending || g_targeting; }

bool bluewake_jump_button_enter(CPUState* cpu, u32 address) {
    if (cpu == NULL)
        return false;
    if (address == kPlaySceneExecute && g_targeting) {
        // The test's L: held, and pressed on the first frame.
        if (mem_read8(cpu, kPadHoldLockL) == 0u)
            mem_write8(cpu, kPadTrigLockL, 1u);
        mem_write8(cpu, kPadHoldLockL, 1u);
        return false;
    }
    if (!g_pending || !proc_call(cpu, address))
        return false;
    // The press is spent on this frame, jump or not.
    g_pending = false;
    update_armed();
    const char* why = refusal(cpu, address);
    if (why != NULL) {
        if (g_trace)
            fprintf(stderr, "[jump] no jump retrace=%llu: %s (proc %u)\n", g_retrace, why,
                    mem_read32(cpu, cpu->gpr[3] + kCurProc));
        return false;
    }
    ++g_jumps;
    if (g_trace) {
        const u32 player = cpu->gpr[3];
        fprintf(stderr,
                "[jump] jump %llu retrace=%llu from proc=%u pos=%.1f,%.1f,%.1f speedF=%.2f mode=0x%08X "
                "lr=0x%08X\n",
                g_jumps, g_retrace, mem_read32(cpu, player + kCurProc), read_f32(cpu, player + kPos),
                read_f32(cpu, player + kPos + 4u), read_f32(cpu, player + kPos + 8u),
                read_f32(cpu, player + kSpeedF), mem_read32(cpu, player + kModeFlg), cpu->lr);
        g_follow = 90u;
    }
    // The tail call: r3 (this) and lr (back into execute) stay as they are.
    cpu->pc = kProcAutoJumpInit;
    return true;
}

void bluewake_jump_button_event(const void* sdl_event) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
    (void)sdl_event;
#else
    const SDL_Event* event = (const SDL_Event*)sdl_event;
    if (event->type == SDL_EVENT_KEY_DOWN && bluewake_action_pressed_by(BW_ACTION_JUMP, sdl_event))
        atomic_fetch_add_explicit(&g_presses, 1u, memory_order_relaxed);
#endif
}

void bluewake_jump_button_reload(void) {
    const char* on = getenv("BLUEWAKE_JUMP_BUTTON");
    g_enabled = on == NULL || on[0] != '0';
}

void bluewake_jump_button_attach(CPUState* cpu) {
    g_cpu = cpu;
    bluewake_jump_button_reload();
    const char* trace = getenv("BLUEWAKE_JUMP_TRACE");
    g_trace = trace != NULL && trace[0] == '1';
    const char* test = getenv("BLUEWAKE_JUMP_TEST");
    for (const char* p = test; p != NULL && *p != '\0' && g_test_count < 32u;) {
        char* end = NULL;
        const unsigned long long retrace = strtoull(p, &end, 10);
        if (end == p)
            break;
        g_test[g_test_count++] = retrace;
        p = *end == ',' ? end + 1 : end;
    }
    const char* target = getenv("BLUEWAKE_JUMP_TEST_TARGET");
    if (target != NULL && sscanf(target, "%llu:%llu", &g_target_start, &g_target_length) != 2)
        g_target_length = 0u;
    if (g_test_count > 0u || g_target_length > 0u)
        g_trace = true;
#if !(defined(__APPLE__) && TARGET_OS_IPHONE)
    if (g_enabled)
        fprintf(stderr, "[jump] Space (or a controller's left bumper, or as chosen in the options) makes Link jump\n");
#endif
}

static void follow(void) {
    CPUState* cpu = g_cpu;
    const u32 player = cpu != NULL ? mem_read32(cpu, kPlayerPointer) : 0u;
    if (!guest_pointer(player)) {
        g_follow = 0u;
        return;
    }
    const u32 proc = mem_read32(cpu, player + kCurProc);
    fprintf(stderr, "[jump-trace] retrace=%llu proc=%u pos=%.1f,%.1f,%.1f speed.y=%.2f speedF=%.2f ground=%u\n",
            g_retrace, proc, read_f32(cpu, player + kPos), read_f32(cpu, player + kPos + 4u),
            read_f32(cpu, player + kPos + 8u), read_f32(cpu, player + kSpeed + 4u),
            read_f32(cpu, player + kSpeedF), (mem_read32(cpu, player + kAcchFlags) & kGroundHit) != 0u);
    // Until Link is back in normal control after the landing.
    if (--g_follow > 0u && g_follow < 80u &&
        (proc == kProcWaitId || proc == kProcFreeWaitId || proc == kProcMoveId))
        g_follow = 0u;
}

// A press of player 1's jump button since the last retrace.
static bool bumper_pressed(void) {
    const bool down = bluewake_action_pad_down(BW_ACTION_JUMP);
    const bool pressed = down && !g_bumper_was_down;
    g_bumper_was_down = down;
    return pressed;
}

void bluewake_jump_button_retrace(void) {
    ++g_retrace;
    bool pressed = false;
    const unsigned presses = atomic_load_explicit(&g_presses, memory_order_relaxed);
    if (presses != g_presses_seen) {
        g_presses_seen = presses;
        pressed = true;
    }
    if (g_enabled && bumper_pressed())
        pressed = true;
    for (unsigned i = 0; i < g_test_count; ++i)
        pressed = pressed || g_test[i] == g_retrace;
    const bool targeting = g_retrace >= g_target_start && g_retrace < g_target_start + g_target_length;
    if (targeting != g_targeting && g_trace)
        fprintf(stderr, "[jump] test: L %s retrace=%llu\n", targeting ? "held" : "let go", g_retrace);
    g_targeting = targeting;
    if (g_follow > 0u)
        follow();
    if (!g_enabled) {
        g_pending = false;
    } else if (pressed) {
        g_pending = true;
        g_deadline = g_retrace + kWindow;
        if (g_trace)
            fprintf(stderr, "[jump] press retrace=%llu\n", g_retrace);
    } else if (g_pending && g_retrace > g_deadline) {
        g_pending = false;
        if (g_trace)
            fprintf(stderr, "[jump] no jump retrace=%llu: Link's code did not run (menu %u)\n", g_retrace,
                    g_cpu != NULL ? mem_read8(g_cpu, kMenuPause) : 0u);
    }
    update_armed();
}
