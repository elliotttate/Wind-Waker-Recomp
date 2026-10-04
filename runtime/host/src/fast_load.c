#include "fast_load.h"

#include "gxruntime/aurora_backend.h"
#include "quick_doors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Scene changes (fopScnM_ChangeReq with an overlap: a fade or a wipe). While
// one runs, l_fopOvlpM_overlap[0] points at its request; the screen is black
// between the fade out and the fade in, while the old scene is deleted and
// the new one is created phase by phase.
//
// The plain fade (dOvlpFd: overlaps 0, 1, 6, 7 and 8, what doors and most exits
// use) takes 26 game frames to go black and 26 to come back, 1.7 seconds of
// the 2.2 a small room's change takes (the black between is about half a
// second of loading). It counts its own frames (overlap1_class mFadeInTime
// going black, mFadeOutTime coming back) beside the fader's (JUTFader mTimer
// of mFadeTime), and the two add up to 26, so the scene is swapped the frame
// the screen is fully black. Both are shortened here, keeping that sum.
enum {
    kOverlap = 0x803F6160u,   // l_fopOvlpM_overlap[0] (overlap_request_class*)
    kRequestTask = 0x20u,     // overlap_request_class::mpTask (the overlap process)
    kProcName = 0x08u,        // base_process_class::mProcName
    kFadeOutTime = 0xCCu,     // overlap1_class::mFadeOutTime (back to the picture)
    kFadeInTime = 0xD0u,      // overlap1_class::mFadeInTime (going black)
    kFader = 0x803F6898u,     // mDoGph_gInf_c::mFader (JUTFader*)
    kFaderStatus = 0x04u,     // JUTFader::mStatus: 0 WaitOut (covered), 1 WaitIn, 2 FadeIn, 3 FadeOut
    kFaderTime = 0x08u,       // JUTFader::mFadeTime (u16)
    kFaderTimer = 0x0Au,      // JUTFader::mTimer (u16)
    kFaderColor = 0x0Cu,      // JUTFader::mColor (RGBA)
    kNextStage = 0x803C9D48u, // g_dComIfG_gameInfo.play.mNextStage (dStage_nextStage_c)
    kCurStage = 0x803C9D3Cu,  // g_dComIfG_gameInfo.play.mCurStage
};

enum {
    kFaderCovered = 0,
    kFaderUncovering = 2, // JUTFader FadeIn: the picture comes back
    kFaderCovering = 3,   // JUTFader FadeOut: going black
    kGameFade = 26,       // dOvlpFd's frames each way
    kLastFadeOverlap = 4, // fpcNm_OVERLAP0/1/6/7/8 are 0-4, all dOvlpFd
};

// BLUEWAKE_FADE_FRAMES: game frames each way (default 6, 0.2 s); 0 keeps 26.
static unsigned g_fade_frames = 6;

// The black between is the new scene being made, and most of it is waiting:
// the audio side takes 36 game frames from the door (mDoAud_setSceneName's
// load timer, while the old music fades) before the new scene may load its
// sounds, and the scene is created a phase a frame. The CPU is idle for most
// of it, so while the screen is fully black the game runs as fast as it can:
// no wall-clock pacing, and (dol_aurora_set_fast_forward) no presents and no
// queued sound. It starts once the screen has been black for kSettle retraces
// (the black frame is on the screen by then), stops when the picture starts
// coming back, and never runs longer than kFastForwardMax retraces. With quick
// doors (quick_doors.c), a knob door's own fade covering the screen on the way
// to the scene change counts as black too.
// BLUEWAKE_FAST_FORWARD=0 turns it off.
static bool g_ff_enabled = true;
static bool g_ff;
static unsigned g_covered;
static const unsigned kSettle = 6, kFastForwardMax = 600;

static bool guest_pointer(u32 address) { return address >= 0x80000000u && address < 0x81800000u; }

static CPUState* g_cpu;
static bool g_trace;
static unsigned long long g_retrace;
static unsigned long long g_cpu_us;
static unsigned long long g_wall_us;

// BLUEWAKE_TEST_WARP=retrace:stage:room:point[:layer][,...] (testing only): asks
// the game for that scene change at that retrace, as a door or an exit does;
// up to 32, in order (a tour of places: the optimization training, shaders).
typedef struct TestWarp {
    unsigned long long retrace;
    char stage[8];
    int room, point, layer;
} TestWarp;
static TestWarp g_warps[32];
static unsigned g_warp_count, g_warp_next;

static unsigned long long now_us(clockid_t clock) {
    struct timespec ts;
    clock_gettime(clock, &ts);
    return (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)ts.tv_nsec / 1000ull;
}

void bluewake_fast_load_reload(void) {
    const char* fade = getenv("BLUEWAKE_FADE_FRAMES");
    g_fade_frames = 6u;
    if (fade != NULL && fade[0] != '\0') {
        const long frames = strtol(fade, NULL, 10);
        g_fade_frames = frames <= 0 || frames >= kGameFade ? 0u : (unsigned)frames;
    }
    const char* ff = getenv("BLUEWAKE_FAST_FORWARD");
    g_ff_enabled = ff == NULL || ff[0] != '0';
}

void bluewake_fast_load_attach(CPUState* cpu) {
    g_cpu = cpu;
    bluewake_fast_load_reload();
    const char* trace = getenv("BLUEWAKE_LOAD_TRACE");
    g_trace = trace != NULL && trace[0] == '1';
    g_warp_count = g_warp_next = 0;
    for (const char* at = getenv("BLUEWAKE_TEST_WARP"); at != NULL && *at != '\0' && g_warp_count < 32u;) {
        TestWarp* w = &g_warps[g_warp_count];
        char stage[16] = "";
        w->layer = -1;
        if (sscanf(at, "%llu:%15[^:]:%d:%d:%d", &w->retrace, stage, &w->room, &w->point, &w->layer) < 4)
            break;
        // A layer is the fifth field only within this entry.
        const char* end = strchr(at, ',');
        int fields = 0;
        for (const char* c = at; *c != '\0' && c != end; ++c)
            fields += *c == ':';
        if (fields < 4)
            w->layer = -1;
        snprintf(w->stage, sizeof w->stage, "%s", stage);
        g_warp_count++;
        g_trace = true;
        at = end != NULL ? end + 1 : NULL;
    }
}

static void warp(CPUState* cpu, const TestWarp* w) {
    for (u32 i = 0; i < 8u; ++i)
        mem_write8(cpu, kNextStage + i, (u8)w->stage[i]);
    mem_write16(cpu, kNextStage + 0x8u, (u16)w->point);
    mem_write8(cpu, kNextStage + 0xAu, (u8)w->room);
    mem_write8(cpu, kNextStage + 0xBu, (u8)w->layer);
    mem_write8(cpu, kNextStage + 0xDu, 0u); // wipe 0: the plain fade
    mem_write8(cpu, kNextStage + 0xCu, 1u); // enable
    fprintf(stderr, "[load] test warp retrace=%llu to %s room %d point %d layer %d\n", g_retrace, w->stage,
            w->room, w->point, w->layer);
}

// Shortens a plain fade the game has just started (the fader at its 26 frames).
static void shorten_fade(CPUState* cpu, u32 request) {
    const u32 task = mem_read32(cpu, request + kRequestTask);
    const u32 fader = mem_read32(cpu, kFader);
    if (!guest_pointer(task) || !guest_pointer(fader) || (u16)mem_read16(cpu, task + kProcName) > kLastFadeOverlap)
        return;
    const u32 status = mem_read32(cpu, fader + kFaderStatus);
    const bool covering = status == kFaderCovering;
    if ((!covering && status != kFaderUncovering) || mem_read16(cpu, fader + kFaderTime) != kGameFade)
        return;
    const u32 frames = g_fade_frames;
    u32 timer = mem_read16(cpu, fader + kFaderTimer);
    if (timer >= frames)
        timer = frames - 1u;
    mem_write16(cpu, fader + kFaderTime, (u16)frames);
    mem_write16(cpu, fader + kFaderTimer, (u16)timer);
    // The overlap's count of the frames left. This runs between retraces, not
    // at a fixed point of the frame, so going black it keeps one frame to spare
    // (the scene is never swapped before the screen is fully black); coming
    // back an early end only lets the fader finish on its own.
    const u32 counter = task + (covering ? kFadeInTime : kFadeOutTime);
    const s32 left = (s32)mem_read32(cpu, counter);
    const s32 want = covering ? (s32)(frames - timer + 1u) : (s32)(frames - timer);
    if (left > want && want > 0)
        mem_write32(cpu, counter, (u32)want);
}

bool bluewake_fast_load_fast_forward(void) { return g_ff; }

void bluewake_fast_load_retrace(unsigned long long cpu_us) {
    ++g_retrace;
    CPUState* cpu = g_cpu;
    if (cpu == NULL)
        return;
    while (g_warp_next < g_warp_count && g_retrace >= g_warps[g_warp_next].retrace) {
        if (g_retrace == g_warps[g_warp_next].retrace)
            warp(cpu, &g_warps[g_warp_next]);
        g_warp_next++;
    }
    const u32 overlap = mem_read32(cpu, kOverlap);
    if (g_fade_frames != 0u && guest_pointer(overlap))
        shorten_fade(cpu, overlap);
    const u32 fader = mem_read32(cpu, kFader);
    const int status = guest_pointer(fader) ? (int)mem_read32(cpu, fader + kFaderStatus) : -1;
    // Or a knob door's own fade covers it on the way to the scene change
    // (quick_doors.c).
    const bool covered = (guest_pointer(overlap) && status == kFaderCovered) || bluewake_quick_doors_covered();
    g_covered = covered ? g_covered + 1u : 0u;
    const bool ff = g_ff_enabled && g_covered >= kSettle && g_covered < kSettle + kFastForwardMax;
    if (ff != g_ff) {
        g_ff = ff;
        dol_aurora_set_fast_forward(ff);
        if (g_trace)
            fprintf(stderr, "[load] fast-forward %s retrace=%llu\n", ff ? "on" : "off", g_retrace);
    }
    if (!g_trace)
        return;
    const unsigned long long wall = now_us(CLOCK_MONOTONIC);
    const int alpha = guest_pointer(fader) ? mem_read8(cpu, fader + kFaderColor + 3u) : -1;
    static bool was_loading;
    const bool loading = guest_pointer(overlap);
    if (loading || was_loading) {
        char stage[9] = {0};
        for (u32 i = 0; i < 8u; ++i)
            stage[i] = (char)mem_read8(cpu, kCurStage + i);
        fprintf(stderr, "[load] retrace=%llu overlap=0x%08X peek=%u fader=%d alpha=%d stage=%s cpu_us=%llu wall_us=%llu\n",
                g_retrace, overlap, loading ? mem_read32(cpu, overlap + 0x08u) : 0u, status, alpha, stage,
                cpu_us - g_cpu_us, wall - g_wall_us);
    }
    was_loading = loading;
    g_cpu_us = cpu_us;
    g_wall_us = wall;
}
