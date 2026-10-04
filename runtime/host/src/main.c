#include "gxruntime/boot.h"
#include "gxruntime/aurora_backend.h"
#include "gxruntime/headless_backend.h"
#include "gxruntime/guest_memory_dirty.h"
#include "gxruntime/loader.h"
#include "gxruntime/platform.h"
#include "gxruntime/interrupts.h"
#include "gxruntime/si.h"
#include "gxruntime/di.h"
#include "gxruntime/dvd.h"
#include "gxruntime/audio_dma.h"
#include "gxruntime/audio_event.h"
#include "gxruntime/vi_clock.h"
#include "gxruntime/aram.h"
#include "core/cpu.h"
#include "StaticRecompABI.h"
#include "aram_dma.h"
#include "actor_search_budget.h"
#include "audio_capture.h"
#include "ipl_sram.h"
#include "card_runtime.h"
#include "edge_intercepts.h"
#include "game_options.h"
#include "forest_water.h"
#include "fast_load.h"
#include "fps_watch.h"
#include "jump_button.h"
#include "settings_menu.h"
#include "simulation_mode.h"
#include "sprint.h"
#include "quick_doors.h"
#include "haptics.h"
#include "draw_tags.h"
#include "mouse_camera.h"
#include "callback_delivery.h"
#include "cycle_domain.h"
#include "interrupt_sources.h"
#include "delivery_digest.h"
#include "cold_fallback.h"
#include "pad_event_schedule.h"
#include "pad_waypoint.h"
#include "pad_wire.h"
#include "rel_scratch_allocator.h"
#include "return_census.h"
#include "scheduler_contract.h"
#include "save_state.h"
#include "climb.h"
#include "gxruntime/hle.h"
#include <aurora/gfx.h>
#include "external_memory.h"
#include "fpu_context.h"
#ifdef BLUEWAKE_HAS_DSP_ADAPTER
#include "dsp_adapter_c.h"
#endif

#include <dlfcn.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#ifndef BLUEWAKE_ENABLE_DEVELOPER_TRACING
#define BLUEWAKE_ENABLE_DEVELOPER_TRACING 0
#endif

#if BLUEWAKE_ENABLE_DEVELOPER_TRACING
#define BLUEWAKE_TRACE_STORAGE(name) static bool name
#define BLUEWAKE_TRACE_ASSIGN(name, environment) \
    ((name) = getenv(environment) != NULL)
#else
#define BLUEWAKE_TRACE_STORAGE(name) enum { name = 0 }
#define BLUEWAKE_TRACE_ASSIGN(name, environment) ((void)0)
#endif

extern void ppc_set_mem_write_journal(PPCMemWriteJournal fn, void* user);

typedef const StaticRecompModuleDesc* (*GetModuleFn)(void);
typedef void (*SetMemWriteJournalFn)(PPCMemWriteJournal fn, void* user);
typedef bool (*HostEdgeServiceFn)(void* user, CPUState* cpu, u32 address);
typedef void (*SetEdgeServiceFn)(HostEdgeServiceFn fn, void* user);
typedef void (*GuestAliasClearFn)(void);
typedef bool (*GuestAliasAddSharedFn)(u32 linked_start, u32 size, u8* storage);
typedef struct BlueWakeRelData {
    u32 module_id;
    u32 section_index;
    u32 linked_start;
    u32 size;
    const u8* bytes;
} BlueWakeRelData;
typedef const BlueWakeRelData* (*GetRelDataFn)(u32* count);
typedef struct BlueWakeRelAlias {
    u32 raw_start;
    u32 raw_end;
    u32 linked_start;
    u32 text_size;
} BlueWakeRelAlias;
typedef struct BlueWakeRelSlot {
    u32 owner;
    u32 address;
    u32 capacity;
} BlueWakeRelSlot;

#define BLUEWAKE_MAX_REL_ALIASES 512u
#define BLUEWAKE_MAX_REL_SLOTS 512u
static BlueWakeRelAlias g_rel_aliases[BLUEWAKE_MAX_REL_ALIASES];
static u32 g_rel_alias_count;
static u32 g_rel_alias_raw_min = ~0u;
static u32 g_rel_alias_raw_max;
static u32 g_module1_raw_base;
static GuestAliasAddSharedFn g_module_alias_add_shared;
static bool g_module336_bss_alias_installed;
static BlueWakeRelSlot g_rel_slots[BLUEWAKE_MAX_REL_SLOTS];
static const BlueWakeRelData* g_rel_data;
static u32 g_rel_data_count;

// The host's guest-alias registry (ppc_guest_alias_*: the REL modules linked
// over MEM1) changes on the game thread as modules are linked, and the GX
// translation worker reads it to resolve a display list, vertex array or
// texture a module keeps in its own data (host_graphics_guest_resolve). An
// insertion moves the registry's sorted entries under a lookup, which then
// returns another entry's storage at a wild offset: the translation worker
// crashed in build_draw_plan_into copying vertices from it (about one launch
// in eight). Changes and the worker's lookups take this lock; the game
// thread's own lookups need none, as nothing else changes the registry.
static pthread_mutex_t g_guest_alias_lock = PTHREAD_MUTEX_INITIALIZER;
// Counts the registry's changes (under the lock): a graphics resolution made
// under one count holds until the next (host_graphics_guest_resolve's cache).
static volatile u32 g_guest_alias_changes;

// Every guest alias the host has registered, in registration order: a save
// state writes each one's storage (linked REL data and BSS live there, not in
// MEM1), and a load registers any the state has that this run has not yet
// (module 336's BSS is added only when that module is linked).
#define HOST_STATE_MAX_ALIASES 4096u
typedef struct HostStateAlias {
    u32 linked_start;
    u32 size;
} HostStateAlias;
static HostStateAlias g_state_aliases[HOST_STATE_MAX_ALIASES];
static u32 g_state_alias_count;

static bool host_add_shared_guest_alias(u32 linked_start, u32 size,
                                        const u8* initial_bytes) {
    u8* storage = NULL;
    if (g_module_alias_add_shared == NULL)
        return false;
    pthread_mutex_lock(&g_guest_alias_lock);
    bool added = ppc_guest_alias_add(linked_start, size, initial_bytes);
    if (added && !ppc_guest_alias_get_storage(linked_start, size, &storage)) {
        ppc_guest_alias_remove(linked_start, size);
        added = false;
    }
    g_guest_alias_changes++;
    pthread_mutex_unlock(&g_guest_alias_lock);
    if (!added)
        return false;
    if (g_module_alias_add_shared(linked_start, size, storage)) {
        if (g_state_alias_count < HOST_STATE_MAX_ALIASES)
            g_state_aliases[g_state_alias_count++] =
                (HostStateAlias){linked_start, size};
        return true;
    }
    pthread_mutex_lock(&g_guest_alias_lock);
    ppc_guest_alias_remove(linked_start, size);
    g_guest_alias_changes++;
    pthread_mutex_unlock(&g_guest_alias_lock);
    return false;
}

// The composite keeps REL code/data at deterministic linked addresses;
// module 1 begins at 0x81F80000, beyond the retail 24 MiB MEM1 ceiling.
// Dynamic raw REL images stay in the lower scratch window and are reclaimed
// on control-object reuse before they can overlap linked code/data. The
// window takes the memory the game never uses between its 24 MiB and the
// linked modules, above the callback return sentinel (0x8180FFF0). It was
// 1.5 MiB from 0x81E00000: 27 minutes of play through many islands left it
// full of the modules the stage had linked, the sea by the pirate ship then
// needed d_a_bb (52 KB) and there was no room, and the game jumped into the
// module it could not load (2026-09-29).
#define BLUEWAKE_LINKED_RAM_SIZE 0x02000000u
#define BLUEWAKE_DYNAMIC_SCRATCH_BASE 0x81820000u
#define BLUEWAKE_DYNAMIC_SCRATCH_LIMIT 0x81F80000u

// Minimal DSP/ARAM control-register handshake needed by __OSInitAudioSystem.
// The full audio engine remains a later P5 subsystem; this state models only
// the hardware-visible control/status transition and keeps it observable.
static u16 g_dsp_control;
static bool g_dsp_aram_complete;
static u32 g_dsp_mail_from;
static bool g_dsp_mail_from_pending;
static unsigned g_dsp_mail_reads_remaining;
static bool g_dsp_mail_to_high_seen;
static unsigned g_dsp_mail_to_reports;
static bool g_dsp_boot_mail_armed;
static bool g_dsp_boot_mail_clear_seen;
static bool g_dsp_boot_handshake_sent;
static bool g_dsp_task_handshake_sent;
static bool g_dsp_task_request_pending;
static bool g_dsp_task_request_armed;
static bool g_dsp_boot_task_ready;
static bool g_dsp_task_boot_started;
static unsigned g_dsp_audio_frame_words_remaining;
static unsigned g_dsp_audio_frame_trace_reports;
static unsigned g_dsp_dma_guest_trace_reports;
static u16 g_dsp_mail_to_high_value;
static unsigned g_dsp_mail_reports;
static unsigned g_dsp_irq_reports;
static unsigned g_dsp_irq_route_reports;
static DolAudioDma g_audio_dma;
static BluewakeAudioCapture g_audio_capture;
// SRAM on EXI channel 0 (ipl_sram.h); off unless BLUEWAKE_SRAM is set.
static BluewakeIplSram g_ipl_sram;
// EFB depth peeks (GXPeekZ reads 0xC8400000 + (y << 12) + (x << 2)) answered
// from Aurora's asynchronous depth snapshot. Wind Waker's sun counts as visible
// only where the peeked depth is far (0xFFFFFF); every peek used to read 0, so
// the sun was always hidden and its glare and lens flare never drew. On with
// the Aurora renderer (BLUEWAKE_EFB_PEEK=0 turns it off); Dolphin with EFB
// access on, its setting for this game, draws the same glare (CURRENT.md,
// 2026-09-24). The headless renderer, which the certified route uses, keeps 0.
static bool g_efb_peek_enabled;
static unsigned g_efb_peek_reports;
static bool g_audio_capture_failure_reported;
static DolAudioEventAdapter g_audio_events;
static unsigned g_audio_event_reports;
static unsigned g_audio_sequence_reports;
static unsigned g_audio_note_reports;
static unsigned g_audio_lifecycle_reports;
static unsigned g_audio_sequence_contract_reports;
static unsigned g_audio_sequence_active_reports;
static u32 g_audio_sequence_active_track;
static bool g_audio_sequence_active_pending;
static u32 g_audio_sequence_root_init_data;
static bool g_audio_sequence_root_init_pending;
static unsigned g_audio_callback_reports;
static unsigned g_audio_start_reports;
static unsigned g_audio_frame_boundary_reports;
static unsigned g_audio_thread_reports;
static unsigned g_audio_dsp_register_reports;
static unsigned g_audio_ai_register_reports;
static unsigned g_audio_message_reports;
static unsigned g_audio_sync_reports;
static DolInterrupts g_interrupts;
static DolSiDevice g_si;
static DolPadState g_virtual_pad[4];
static bool g_live_pad_enabled;
BLUEWAKE_TRACE_STORAGE(g_pad_trace);
BLUEWAKE_TRACE_STORAGE(g_pad_wire_trace);
BLUEWAKE_TRACE_STORAGE(g_pad_si_trace);
BLUEWAKE_TRACE_STORAGE(g_pad_status_trace);
BLUEWAKE_TRACE_STORAGE(g_runqueue_trace);
BLUEWAKE_TRACE_STORAGE(g_gx_fifo_trace);
BLUEWAKE_TRACE_STORAGE(g_room0_trace);
BLUEWAKE_TRACE_STORAGE(g_player_trace);
BLUEWAKE_TRACE_STORAGE(g_pad_conversion_trace);
BLUEWAKE_TRACE_STORAGE(g_pad_conversion_all_trace);
BLUEWAKE_TRACE_STORAGE(g_scene_overlap_trace);
BLUEWAKE_TRACE_STORAGE(g_create_iter_trace);
BLUEWAKE_TRACE_STORAGE(g_bg_create_trace);
BLUEWAKE_TRACE_STORAGE(g_rel_indirect_trace);
BLUEWAKE_TRACE_STORAGE(g_rel_destructors_trace);
BLUEWAKE_TRACE_STORAGE(g_rel_lifecycle_trace);
BLUEWAKE_TRACE_STORAGE(g_rel_calls_trace);
BLUEWAKE_TRACE_STORAGE(g_gx_entry_trace);
// The GX entries BLUEWAKE_TRACE_GX_ENTRY reports (GXBegin, GXCopyDisp, GXLoadTexObj,
// GXCallDisplayList, each up to the next function). Only that trace reads them, and
// direct calls are off while it is on, so the Windows builder's watch list
// (scripts/windows/direct_calls.py) leaves them out: calls to them skip the chassis loop.
// bluewake-unwatched-begin
static const u32 g_gx_entry_trace_ranges[][2] = {
    {0x803230C4u, 0x803231B4u},
    {0x80323D50u, 0x80323EACu},
    {0x80324EE8u, 0x80324F3Cu},
    {0x80326B80u, 0x80326BF0u},
};
// bluewake-unwatched-end

static bool host_gx_entry_traced(u32 pc) {
    for (size_t i = 0; i < sizeof g_gx_entry_trace_ranges / sizeof g_gx_entry_trace_ranges[0]; ++i)
        if (pc >= g_gx_entry_trace_ranges[i][0] && pc < g_gx_entry_trace_ranges[i][1])
            return true;
    return false;
}
BLUEWAKE_TRACE_STORAGE(g_dispatch_terminal_trace);
BLUEWAKE_TRACE_STORAGE(g_alarm_state_trace);
static u32 g_pad_si_data_watch;
static unsigned g_pad_transfer_reports;
static unsigned g_pad_poll_reports;
static unsigned g_pad_active_reports;
static unsigned g_pad_wire_reports;
static unsigned g_pad_si_reports;
static unsigned g_pad_si_data_reports;
static unsigned g_pad_si_status_reports;
static unsigned g_title_input_reports;
static bool g_title_ready_reported;
static u64 g_title_ready_retrace;
static bool g_file_select_reported;
static u64 g_file_select_retrace;
static bool g_name_scene_create_reported;
static u64 g_name_scene_create_retrace;
static u32 g_name_scene_object;
static bool g_name_scene_execute_reported;
static u64 g_name_scene_execute_retrace;
static bool g_memcard_check_reported;
static u64 g_memcard_check_retrace;
static bool g_new_game_intro_reported;
static u64 g_new_game_intro_retrace;
static bool g_name_input_complete_reported;
static u64 g_name_input_complete_retrace;
static bool g_name_scene_change_reported;
static u64 g_name_scene_change_retrace;
static bool g_open_scene_request_reported;
static u64 g_open_scene_request_retrace;
static bool g_play_scene_reported;
static u64 g_play_scene_retrace;
static bool g_opening_complete_reported;
static u64 g_opening_complete_retrace;
static bool g_outset_room_requested;
static u64 g_outset_room_request_retrace;
static u32 g_overlap_last_phase = UINT32_MAX;
static u32 g_overlap_terminal_phase = UINT32_MAX;
static bool g_name_character_jut_hold_reported;
static bool g_name_character_jut_trigger_reported;
static bool g_name_character_cpad_hold_reported;
static bool g_name_character_cpad_trigger_reported;
static BluewakePadEventSchedule g_title_pad_pulse;
static BluewakePadEventSchedule g_title_confirm_pulse;
static BluewakePadEventSchedule g_no_card_dismiss_pulse;
static BluewakePadAxisEventSchedule g_no_save_left_pulse;
static BluewakePadAxisEventSchedule g_player_stick_x_pulse;
static BluewakePadAxisEventSchedule g_player_stick_y_pulse;
static bool g_player_waypoint_configured;
static bool g_player_waypoint_active;
static float g_player_waypoint_x;
static float g_player_waypoint_z;
static s8 g_player_waypoint_stick_x;
// BLUEWAKE_PAD_PLAYER_START_AFTER: the player-ready stick waits for this
// retrace, so a route can start at a chosen phase (the game samples the pad
// every other retrace, and contact with scenery depends on which one).
static u64 g_player_start_after;
static s8 g_player_waypoint_stick_y;
static u64 g_player_waypoint_length;
static bool g_player_ladder_down_configured;
static bool g_player_ladder_entered;
static bool g_player_ladder_move_seen;
static BluewakePadRoute g_player_post_ladder_route;
static bool g_player_post_ladder_route_configured;
static bool g_player_post_ladder_route_started;
static bool g_player_post_ladder_route_active;
static bool g_player_post_ladder_route_complete;
static u64 g_player_post_ladder_route_arrival_retrace;
static BluewakePadEventSchedule g_player_route_confirm_pulse;
static bool g_player_route_confirm_configured;
static bool g_player_route_confirm_triggered;
static BluewakePadEventSchedule g_event_confirm_pulse;
static s32 g_event_confirm_target = -1;
// Confirm whatever message prompt the guest has up, rather than one named
// event. This is the pad-layer equivalent of a person tapping A again until the
// screen moves, which is what scripts/app_acceptance_test.sh does with real
// keys, and it is what makes the authored cutscene after the play scene
// walkable without a key window. See BLUEWAKE_PAD_CONFIRM_EVENT=any.
static bool g_event_confirm_any;
// One line per prompt transition, for finding which event a scene asks about in
// a build without developer tracing compiled in.
static bool g_event_prompt_trace;
static bool g_event_confirm_prompt_active;
static bool g_event_confirm_demo_prompt_active;
static BluewakePadEventSchedule g_no_save_confirm_pulse;
static BluewakePadEventSchedule g_file_slot_select_pulse;
static BluewakePadEventSchedule g_file_start_pulse;
static BluewakePadEventSchedule g_name_character_pulse;
static BluewakePadEventSchedule g_name_end_pulse;
static BluewakePadEventSchedule g_name_confirm_pulse;
// ---------------------------------------------------------------------------
// The in-game save route (P4 milestone 9).
//
// The card the certified route already produces holds the *empty* file the card
// manager creates when a new game starts at file-select (CARDCreateAsync
// "gczelda", 12 blocks, template write), which is why "continue" on it was
// never a save-continue path. Gameplay state only reaches the card when the
// pause menu's Save screen runs, so this drives that screen the way a player
// does: START, the item cursor to Save, A, and the two save-screen prompts.
//
// It is off unless BLUEWAKE_SAVE_ROUTE is set, so the certified route and every
// existing bench are untouched. When it is on it writes nothing to guest
// memory; the pad is the only channel it reaches the guest through, and the
// guest's own fields are the only thing it reads back.
//
// The guest addresses are the retail binary's own, from the pinned decomp
// dependency's config/GZLE01/symbols.txt, which is where this project already
// takes its guest addresses from: dMc_c is d_menu_window.cpp's collect/pause
// menu, dMs_c its save screen. The member offsets are the layout annotations in
// ref/tww/include/d/d_menu_collect.h and d_menu_save.h.
#define BLUEWAKE_GUEST_DMC_C 0x803F6FF0u
#define BLUEWAKE_GUEST_DMS_C 0x803F7000u
#define BLUEWAKE_GUEST_PLAYER 0x803CA74Cu
#define BLUEWAKE_GUEST_EVENT_MODE 0x803C9EA2u
#define BLUEWAKE_DMC_OFF_NOW_ITEM 0x27EDu
#define BLUEWAKE_DMC_OFF_MODE 0x27EEu
// dMenu_Collect_c owns its save screen as a member (d_menu_collect.h's
// /* 0x2784 */ dMenu_save_c* dMs_c), and that is the one the collect page's
// Save entry initializes. The dMs_c at 0x803F7000 is d_menu_window.cpp's own
// file static, which belongs to the name-entry and game-over screens: reading it
// here is why a run that really did save reported "save screen never opened".
#define BLUEWAKE_DMC_OFF_SAVE_MENU 0x2784u
#define BLUEWAKE_DMS_OFF_PROC 0x052Cu
#define BLUEWAKE_DMS_OFF_SAVE_STATUS 0x0531u
// The pause menu's own gate, from d_menu_window.cpp's dMs_Execute: the menu
// only opens on START when dMenu_pause is clear, the menu status is the old
// menu's, the event wait has run out, and no message window is up. Reading them
// is how a run that does not open the menu says which one refused.
#define BLUEWAKE_GUEST_DMENU_PAUSE 0x803F7097u
#define BLUEWAKE_GUEST_MENU_STATUS 0x803F7095u
#define BLUEWAKE_GUEST_MENU_STATUS_OLD 0x803F7096u
#define BLUEWAKE_GUEST_EVENT_WAIT_FRAME 0x803F7004u
#define BLUEWAKE_GUEST_MESG_STATUS 0x803CA7D2u
// play.mHeapLockFlag (0x803C4C08 + 0x12A0 + 0x4962) and the enable bit of
// play.mNextStage (0x803C4C08 + 0x12A0 + 0x3EA0, then dStage_nextStage_c's
// 0xC): two more of dMs_Execute's conditions, both guest state.
#define BLUEWAKE_GUEST_HEAP_LOCK 0x803CA80Au
#define BLUEWAKE_GUEST_NEXT_STAGE_ENABLE 0x803C8AB4u
#define BLUEWAKE_SAVE_MENU_ITEM 7u
#define BLUEWAKE_SAVE_MENU_MODE 3u
// The unattended route's scripted presses end at retrace 20,402. The save route
// does not wait for that: control is admitted at 20,256 and the window it is
// admitted in is narrow, so the route arms on the guest's own readiness, which
// is also the moment the script has nothing left to walk - after it, every A
// press the script would make would land in the pause menu instead of a
// cutscene. This is the retrace the route's own gate admitted control at, kept
// so a run that starts later than it can say so.
#define BLUEWAKE_SAVE_FIRST_RETRACE 20256u
// dMenu_Collect_c::cursorMainMove's item[20][8] navigation table, resolved
// against the pad's own angle convention. JUTGamePad::CStick::update sets
// mAngle = atan2(mPosX, -mPosY), so straight up is angle 0 and mask bit 8,
// right is +0x4000 and bit 2, down is bit 4 and left bit 1; the eight control
// bits map to dMenu_Collect_c::stickDirection's 0..7 in that angle order. For
// each item the cursor can sit on this is the first direction on a shortest
// path to the Save item. Every item 0..19 reaches it - the table is the guest's
// own, not a guess - and the loop re-reads the cursor after every step, so a
// missed or repeated press corrects itself.
static const u8 k_save_route_first_dir[20] = {
    0x02, 0x02, 0x02, 0x02, 0x01, 0x02, 0x02, 0xFF,
    0x00, 0x00, 0x05, 0x06, 0x00, 0x06, 0x05, 0x05,
    0x05, 0x05, 0x05, 0x03,
};
// Direction index to stick deflection. The SI wire byte is stick + 128 and the
// guest reads it as s8, so stick up is a negative y: the same sign
// pad_waypoint.c already steers the player with (forward = -y).
static const s8 k_save_route_dir_x[8] = {
    -127, -127, -127, 0, 127, 127, 127, 0,
};
static const s8 k_save_route_dir_y[8] = {
    127, 0, -127, -127, -127, 0, 127, 127,
};
enum {
    BLUEWAKE_SAVE_STATE_IDLE = 0u,
    BLUEWAKE_SAVE_STATE_PAUSE,
    BLUEWAKE_SAVE_STATE_CURSOR,
    BLUEWAKE_SAVE_STATE_SCREEN,
    BLUEWAKE_SAVE_STATE_MENU,
    BLUEWAKE_SAVE_STATE_QUIT,
    BLUEWAKE_SAVE_STATE_DONE,
};
static BluewakePadEventSchedule g_save_start_pulse;
static BluewakePadEventSchedule g_save_confirm_pulse;
// The pause menu's two pages are the item page and the collect page, and R
// switches between them (d_menu_window.cpp's MENU_STATE_ITEM_MOVE hands R to
// MENU_STATE_ITEM_TO_COLLECT_RIGHT). The collect page is the one that owns the
// Save entry the save screen hangs off, and the page the menu reopens on comes
// from dMenu_getMenuStatus(), which the new-game flow leaves on the item page.
static BluewakePadEventSchedule g_save_page_pulse;
static BluewakePadAxisEventSchedule g_save_stick_x_pulse;
static BluewakePadAxisEventSchedule g_save_stick_y_pulse;
static bool g_save_route_enabled;
static unsigned g_save_route_state;
static unsigned g_save_route_hold;
static unsigned g_save_route_retries;
static unsigned g_save_route_steps;
static u64 g_save_route_control_retrace;
static u64 g_save_route_next_retrace;
static u64 g_save_route_menu_retrace;
static u8 g_save_route_item = 0xFFu;
static u8 g_save_route_mode = 0xFFu;
static u8 g_save_route_proc = 0xFFu;
static u8 g_save_route_status = 0xFFu;
static u8 g_save_route_acted_proc = 0xFFu;
static u8 g_save_route_acted_status = 0xFFu;
static u64 g_save_route_acted_retrace;
static bool g_save_route_right_sent;
// mDoCPd_Read derives its L/R "hold lock" from the analog trigger, not from the
// digital button bit: mDoCPd_R_LOCK_BUTTON reads mHoldLockR, and mHoldLockR is
// set only while mTriggerRight is above the HIO threshold. The pause menu's page
// switch asks for exactly that lock, so a digital R press moves nothing.
static u64 g_save_trigger_start_retrace;
static u64 g_save_trigger_length;
static u8 g_save_trigger_value;
static unsigned g_save_route_wait_reports;
static unsigned g_save_route_page_presses;
// The unattended route's cutscene script presses A blindly every 150 retraces.
// Once the save route has seen control, those presses are no longer walking a
// cutscene - they are pressing A inside the pause menu - so the route turns them
// off for the rest of the run. This is the only way it touches the route input.
static bool g_save_route_script_suppressed;
static bool g_pad_pulse_enabled;
static u64 g_pad_pulse_start_retrace;
static u64 g_pad_pulse_length;
static bool g_pad_pulse2_enabled;
static u64 g_pad_pulse2_start_retrace;
static u64 g_pad_pulse2_length;
static bool g_pad_pulse3_enabled;
static u64 g_pad_pulse3_start_retrace;
static u64 g_pad_pulse3_length;
// The general form of the three fixed pulses above:
//
//     BLUEWAKE_PAD_SCRIPT="retrace:buttons:length,..."
//
// Three presses cannot express a scene that needs a dozen. The authored `awake`
// cutscene - event 38 in the guest's own event table - confirms nine text pages
// between retrace 17,868 and 19,773, and neither existing mechanism covers it:
// the latched schedules walk the new-game menus, and BLUEWAKE_PAD_CONFIRM_EVENT
// is bound to one event index and to the 0x803CA7D2 message status, which stays
// zero for the whole cutscene (measured). A blind scheduled press is what a
// human at the keyboard does, so it is what the harness should be able to do.
#define BLUEWAKE_PAD_SCRIPT_MAX 1024u
typedef struct BluewakePadScriptPress {
    u64 start_retrace;
    u64 length;
    u16 buttons;
    // Optional main-stick deflection for the same span (retrace:buttons:length:x:y),
    // so a route can walk Link and a Dolphin input movie can repeat it.
    bool has_stick;
    s8 stick_x;
    s8 stick_y;
    // And the C-stick (retrace:buttons:length:x:y:cx:cy), for the camera.
    bool has_substick;
    s8 substick_x;
    s8 substick_y;
} BluewakePadScriptPress;
static BluewakePadScriptPress g_pad_script[BLUEWAKE_PAD_SCRIPT_MAX];
static unsigned g_pad_script_count;
static u64 g_host_retrace_count;
static bool g_live_takeover;  // see host_note_live_input

// Guest PC sampler (BLUEWAKE_PC_SAMPLE=path). A thread reads the guest pc the
// translated code keeps in CPUState every 100 microseconds while the retrace
// count is inside [BLUEWAKE_PC_SAMPLE_FROM, BLUEWAKE_PC_SAMPLE_TO), and writes
// "pc count" lines at exit. The read races the game thread on purpose: a sample
// only needs to land in the right function, and the host sampler (sample)
// cannot name guest functions because the composite has one symbol per chunk.
// It sees the pc the compiled code has stored, and the compiler keeps the
// stores at dispatches and drops many inside a block, so short functions that
// are always dispatched to are overweighted: __save_gpr/__restore_gpr read 9
// percent here and a native dispatch of them saved nothing (CURRENT.md,
// 2026-09-24). Use it to name candidates, and the instruction bench to judge.
static volatile const u32* g_pc_sample_source;
static u32* g_pc_sample_counts;
static const char* g_pc_sample_path;
static u64 g_pc_sample_from, g_pc_sample_to;
static volatile bool g_pc_sample_stop;
static pthread_t g_pc_sample_thread;
static bool g_pc_sample_started;
#define PC_SAMPLE_WORDS (0x01800000u / 4u)

static void* pc_sample_main(void* unused) {
    (void)unused;
    const struct timespec period = {0, 100000};
    while (!g_pc_sample_stop) {
        const u64 retrace = *(volatile u64*)&g_host_retrace_count;
        if (retrace >= g_pc_sample_from && retrace < g_pc_sample_to) {
            const u32 pc = *g_pc_sample_source;
            const u32 offset = (pc & 0x3FFFFFFFu) >> 2;
            if (offset < PC_SAMPLE_WORDS)
                g_pc_sample_counts[offset]++;
        }
        nanosleep(&period, NULL);
    }
    return NULL;
}

static void pc_sample_finish(void) {
    if (!g_pc_sample_started)
        return;
    g_pc_sample_stop = true;
    pthread_join(g_pc_sample_thread, NULL);
    g_pc_sample_started = false;
    FILE* out = fopen(g_pc_sample_path, "w");
    if (out == NULL)
        return;
    for (u32 i = 0; i < PC_SAMPLE_WORDS; ++i)
        if (g_pc_sample_counts[i] != 0u)
            fprintf(out, "%08X %u\n", 0x80000000u | (i << 2), g_pc_sample_counts[i]);
    fclose(out);
}

static void pc_sample_start(const CPUState* cpu) {
    g_pc_sample_path = getenv("BLUEWAKE_PC_SAMPLE");
    if (g_pc_sample_path == NULL || g_pc_sample_path[0] == '\0')
        return;
    const char* from = getenv("BLUEWAKE_PC_SAMPLE_FROM");
    const char* to = getenv("BLUEWAKE_PC_SAMPLE_TO");
    g_pc_sample_from = from != NULL ? strtoull(from, NULL, 10) : 0u;
    g_pc_sample_to = to != NULL ? strtoull(to, NULL, 10) : UINT64_MAX;
    g_pc_sample_counts = calloc(PC_SAMPLE_WORDS, sizeof(u32));
    if (g_pc_sample_counts == NULL)
        return;
    g_pc_sample_source = &cpu->pc;
    if (pthread_create(&g_pc_sample_thread, NULL, pc_sample_main, NULL) == 0) {
        g_pc_sample_started = true;
        atexit(pc_sample_finish);
    }
}
// Opt-in deadline attribution census. Counts which term of
// host_cycle_deadline_distance() produced the distance that became the next
// dispatch budget. Inert unless BLUEWAKE_DEADLINE_CENSUS is set; writes only
// to stderr at exit and never touches route state.
enum {
    BLUEWAKE_DEADLINE_SOURCE_VI = 0,
    BLUEWAKE_DEADLINE_SOURCE_DSP,
    BLUEWAKE_DEADLINE_SOURCE_AUDIO,
    BLUEWAKE_DEADLINE_SOURCE_DECREMENTER,
    BLUEWAKE_DEADLINE_SOURCE_CLAMP,
    BLUEWAKE_DEADLINE_SOURCE_NONE,
    BLUEWAKE_DEADLINE_SOURCE_CAP,
    BLUEWAKE_DEADLINE_SOURCE_COUNT
};
static u64 g_deadline_source_counts[BLUEWAKE_DEADLINE_SOURCE_COUNT];
static u64 g_deadline_calls;
static u64 g_deadline_one_cycle_counts[BLUEWAKE_DEADLINE_SOURCE_COUNT];
static u64 g_deadline_window_counts[BLUEWAKE_DEADLINE_SOURCE_COUNT];
static u64 g_deadline_window_calls;
static u64 g_deadline_window_one_cycle_counts[BLUEWAKE_DEADLINE_SOURCE_COUNT];
static u64 g_deadline_census_window;
static bool g_deadline_census_enabled;
// BLUEWAKE_BOUNDARY_CENSUS. The P1 workstream prices candidates with one rule:
// one host instruction removed from the per-block dispatch path is worth about
// 0.22 percent of the play window. That rule came from dividing the edge
// fast-reject's 4.42 percent by an *estimated* twenty instructions per
// dispatch, which is an inference about how many block boundaries a play
// retrace contains. host_chassis_edge_service is called exactly once per guest
// block the chassis runs (cmake/composite/dispatch_loop.c), so counting it
// counts boundaries, and this window's boundary count over this window's
// retraces is the constant the price list needs - measured rather than
// inferred. It is a count rather than a timestamp because instruction counts
// are deterministic on a digest-gated route and wall clock is not.
static bool g_boundary_census_enabled;
#define BOUNDARY_ADDRESS_WORDS (0x01800000u / 4u)
static u32* g_boundary_by_address;  // BLUEWAKE_BOUNDARY_CENSUS_BY_ADDRESS=path
static u64 g_boundary_calls;
static u64 g_boundary_window_calls;
static u64 g_boundary_census_window;
static u64 g_boundary_play_calls;
#if BLUEWAKE_EDGE_CENSUS
// BLUEWAKE_EDGE_CENSUS. The boundary census gave the price list its denominator;
// this one splits the chassis edge-service body, which is the per-boundary
// constant that remains after the intercept predicate was inlined. Counting
// which sub-blocks run tells the loop the live fraction of each piece, which is
// the number the disassembly cannot give: the overlap observation's guard reads
// as though it runs for the whole route and the interrupt refresh reads as
// three device predicates, and the 0.5 percent the overlap cache was worth says
// at least one of those readings is wrong.
//
// Compile-time gated rather than runtime gated, unlike the boundary census: a
// dozen counters each cost a load and a test at every boundary, which is about
// 2.4 percent of the steady window, so the instrument must not be in the
// shipping build at all. Configure with -DBLUEWAKE_EDGE_CENSUS=ON to take it.
static u64 g_edge_calls;
static u64 g_edge_overlap_guard;
static u64 g_edge_overlap_object;
static u64 g_edge_overlap_fast;
static u64 g_edge_overlap_enabled;
static u64 g_edge_overlap_phase_changed;
static u64 g_edge_intro_hits;
static u64 g_edge_predicate_true;
static u64 g_edge_predicate_false;
static u64 g_edge_service_each_block;
static u64 g_edge_interrupt_ee;
static u64 g_edge_scheduler_true;
static u64 g_edge_source_publishes;
static u64 g_edge_source_changes;
// Where the guest's device-register traffic goes. A third of the block
// boundaries run host_mmio_read or host_mmio_write (docs/status/CURRENT.md,
// 2026-09-22), and each of those steps the device cursors and can publish an
// interrupt source, so the ranges that traffic reaches decide what a cheaper
// path can skip. Counted per 0xCC00 page rather than assumed; buckets 16..19 are
// the non-0xCC00 spaces.
#define BLUEWAKE_MMIO_BUCKETS 20u
static u64 g_mmio_read_buckets[BLUEWAKE_MMIO_BUCKETS];
static u64 g_mmio_write_buckets[BLUEWAKE_MMIO_BUCKETS];
static unsigned host_mmio_bucket(u32 address) {
    if (address >= 0xCC000000u && address < 0xCC010000u)
        return (address >> 12) & 0xFu;
    if (address >= 0xC0000000u && address < 0xC0002000u)
        return 16u;
    if (address < 0x10000u)
        return 17u;
    if (address >= 0x80000000u)
        return 18u;
    return 19u;
}
#endif
// Opt-in frame-pacing instrument for scripts/bench.sh. It stamps host wall
// time at every guest retrace so the bench harness can measure frame time
// without touching the route: it writes only to stderr and is inert in the
// shipping configuration.
static bool g_frame_timing_enabled;
/* BLUEWAKE_PERF_LOG: one line per second of wall time with the guest's
   retrace rate (60 is full speed), the longest wall gap between two retraces,
   how many gaps exceeded 50 ms (the game draws at 30 fps, so a normal gap is
   33 ms and 50 ms means a dropped frame), and the share of that second the
   emulation thread spent on the CPU. Cheap enough for device sessions. */
static bool g_perf_log_enabled;

static u64 perf_now_us(clockid_t clock) {
    struct timespec ts;
    clock_gettime(clock, &ts);
    return (u64)ts.tv_sec * 1000000ull + (u64)ts.tv_nsec / 1000ull;
}

/* For the iOS shell's FPS display and per-second log: the guest retrace count
   (60 a second is full speed) and the emulation thread's CPU time. Read from
   the same thread that advances them. */
unsigned long long bluewake_host_retrace_count(void) { return g_host_retrace_count; }

/* BLUEWAKE_WALL_PACE: hold each guest retrace to its wall-clock time (NTSC,
   1001/60000 s apart). Without it the only brake on the emulation was the
   audio queue: the guest ran ahead until 250 ms of sound was queued, then the
   push loop slept in 1 ms steps until the device drained a buffer. iOS drains
   in chunks of tens of milliseconds, so the guest was released in bursts and
   two or three frames a second stayed on screen for 60 ms instead of 33 while
   the game itself was on time (the [late] lines: 2 retraces, 19 ms of CPU,
   no present or GPU wait). A late guest is never hurried: past 100 ms behind
   (a heavy frame, a pause) the schedule restarts from now. */
static bool g_wall_pace_enabled;

/* BLUEWAKE_ASYNC_DRAW_DONE (default on; 0 turns it off): raise the PE finish
   at GXSetDrawDone's return as well as in GXDrawDone. See the main loop. */
static bool g_async_draw_done = true;
static u64 g_async_draw_done_commits;

/* Code mods compiled into the composite (scripts/mods/build_mod_variants.py).
   BLUEWAKE_MODS is a comma-separated list of mod names; the iOS shell sets it
   from the Mods menu. Enabling happens once, before the first dispatch; the
   data writes are applied then and again at every retrace, as Dolphin
   re-applies Gecko codes every frame. */
typedef void (*ModWriteFn)(void* user, u32 address, const u8* bytes, u32 size);
typedef u32 (*ModWritesFn)(u32 mask, u32 per_frame_only, ModWriteFn fn, void* user);
static ModWritesFn g_mod_writes;
static u32 g_mod_mask;
// Whether the mod that carries the game options' sites is on (game_options.h).
static bool g_options_mod;

static void host_mod_write(void* user, u32 address, const u8* bytes, u32 size) {
    // Through the guest accessor, as the REL data images are materialized:
    // REL data lives at linked addresses the aliases map, not in plain RAM.
    for (u32 i = 0; i < size; ++i)
        mem_write8((CPUState*)user, address + i, bytes[i]);
}

static void host_mods_reapply(CPUState* cpu) {
    if (g_mod_mask != 0u && g_mod_writes != NULL)
        g_mod_writes(g_mod_mask, 1u, host_mod_write, cpu);
}

static void host_mods_enable(void* lib, CPUState* cpu) {
    typedef u32 (*CountFn)(void);
    typedef const char* (*NameFn)(u32);
    typedef u32 (*ApplyFn)(u32);
    CountFn count = (CountFn)dlsym(lib, "bluewake_composite_mod_count");
    NameFn name = (NameFn)dlsym(lib, "bluewake_composite_mod_name");
    ApplyFn apply = (ApplyFn)dlsym(lib, "bluewake_composite_apply_mods");
    g_mod_writes = (ModWritesFn)dlsym(lib, "bluewake_composite_mod_writes");
    const u32 available = count ? count() : 0u;
    const char* wanted = getenv("BLUEWAKE_MODS");
    char list[256] = "";
    for (u32 i = 0; i < available && name != NULL; ++i) {
        const char* mod = name(i);
        const size_t len = mod ? strlen(mod) : 0u;
        bool on = false;
        for (const char* p = wanted; p != NULL && *p != '\0';) {
            const char* end = strchr(p, ',');
            const size_t n = end ? (size_t)(end - p) : strlen(p);
            if (n == len && strncmp(p, mod, n) == 0)
                on = true;
            p = end ? end + 1 : NULL;
        }
        if (on)
            g_mod_mask |= 1u << i;
        if (on && strcmp(mod, "betterww") == 0)
            g_options_mod = true;
    }
    // The two widescreen mods patch the same code for different shapes; the
    // composite has no variant for both. 16:10 wins over 16:9 if asked for both.
    int ws169 = -1, ws1610 = -1;
    for (u32 i = 0; i < available && name != NULL; ++i) {
        const char* mod = name(i);
        if (mod != NULL && strcmp(mod, "widescreen") == 0) ws169 = (int)i;
        if (mod != NULL && strcmp(mod, "widescreen1610") == 0) ws1610 = (int)i;
    }
    if (ws169 >= 0 && ws1610 >= 0 && (g_mod_mask & (1u << ws169)) && (g_mod_mask & (1u << ws1610))) {
        fprintf(stderr, "[mods] widescreen and widescreen1610 are exclusive; keeping widescreen1610\n");
        g_mod_mask &= ~(1u << ws169);
    }
    for (u32 i = 0; i < available && name != NULL; ++i) {
        const char* mod = name(i);
        const bool on = (g_mod_mask & (1u << i)) != 0u;
        snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s%s",
                 i ? "," : "", mod ? mod : "?", on ? "+" : "");
    }
    const u32 chunks = (g_mod_mask && apply) ? apply(g_mod_mask) : 0u;
    const u32 writes = (g_mod_mask && g_mod_writes)
                           ? g_mod_writes(g_mod_mask, 0u, host_mod_write, cpu)
                           : 0u;
    fprintf(stderr, "[mods] available=%u list=%s mask=0x%X chunks=%u writes=%u\n",
            available, available ? list : "none", g_mod_mask, chunks, writes);
}

static void host_wall_pace(u64 retrace) {
    static u64 base_ns, base_retrace, last_retrace;
    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    const u64 now = (u64)now_ts.tv_sec * 1000000000ull + (u64)now_ts.tv_nsec;
    // The schedule restarts after retraces that were not paced (a scene
    // change's black, fast-forwarded): they are not waited for afterwards.
    const bool resumed = retrace != last_retrace + 1u;
    last_retrace = retrace;
    if (base_ns == 0u || resumed) {
        base_ns = now;
        base_retrace = retrace;
        return;
    }
    // 1001/60000 s per retrace, in nanoseconds.
    const u64 target = base_ns + (retrace - base_retrace) * 1001000000000ull / 60000ull;
    if (now > target + 100000000ull || target > now + 100000000ull) {
        base_ns = now;
        base_retrace = retrace;
        return;
    }
    if (now < target) {
        const u64 wait = target - now;
        struct timespec sleep_ts = {(time_t)(wait / 1000000000ull), (long)(wait % 1000000000ull)};
        nanosleep(&sleep_ts, NULL);
    }
}
unsigned long long bluewake_host_thread_cpu_us(void) {
    return perf_now_us(CLOCK_THREAD_CPUTIME_ID);
}

static void perf_note_retrace(u64 retrace) {
    static u64 window_start, window_cpu, last, window_retraces, worst, hitches;
    const u64 now = perf_now_us(CLOCK_MONOTONIC);
    if (window_start == 0) {
        window_start = last = now;
        window_cpu = perf_now_us(CLOCK_THREAD_CPUTIME_ID);
        return;
    }
    const u64 gap = now - last;
    last = now;
    window_retraces++;
    if (gap > worst) worst = gap;
    if (gap > 50000u) hitches++;
    const u64 elapsed = now - window_start;
    if (elapsed < 1000000u) return;
    const u64 cpu_now = perf_now_us(CLOCK_THREAD_CPUTIME_ID);
    fprintf(stderr,
            "[perf] retrace=%llu rate=%.1f worst_ms=%.1f hitches=%llu busy=%.0f%%\n",
            (unsigned long long)retrace, (double)window_retraces * 1e6 / (double)elapsed,
            (double)worst / 1000.0, (unsigned long long)hitches,
            100.0 * (double)(cpu_now - window_cpu) / (double)elapsed);
    window_start = now;
    window_cpu = cpu_now;
    window_retraces = worst = hitches = 0;
}
// What the host credited each turn, summarised at exit. See cycle_domain.c.
static u64 g_credit_budget_sum;
static u64 g_credit_budget_min;
static u64 g_credit_budget_max;
static u64 g_credit_lag_sum;
static u64 g_credit_lag_max;
// Why a turn ended, which the credit stream alone cannot say. The chassis
// dispatch loop's exits are observable from the host without touching the
// composite: the loop calls the host's edge service once per block, and the host
// sees that call's return value and can read downcount before and after each
// block. So a turn that ended at the edge service is a turn whose last edge call
// returned true; a turn that ended at the budget is past -cycle_budget at exit
// (the loop tests that *before* the edge service, so the two cannot be
// confused); a turn whose last block charged nothing ended at the zero-charge
// run bound or the no-progress path; the remainder is the dispatcher miss. This
// supersedes the budget/early split, which compared each turn's credit against
// the *previous* turn's budget and disagreed with itself after the tolerance
// landed (docs/status/CURRENT.md, 2026-09-22).
static bool g_turn_census_enabled;
// The retrace the credit census starts counting at. The play window is the
// graded one (13,900) but a host-accounting effect that shows in the boot must
// be visible without a 15,000-retrace run, so it is settable.
static u64 g_credit_census_window = 13900u;
static u64 g_turn_census_turns;
static u64 g_turn_census_blocks;
static u64 g_turn_exit_budget;
static u64 g_turn_exit_edge;
static u64 g_turn_exit_zero_charge;
static u64 g_turn_exit_miss;
static u64 g_turn_exit_other;
static u64 g_turn_blocks_this_turn;
// Which block's dispatch left the downcount unmoved, for turns that ended at
// the zero-charge exit: the composite's loop tolerates a bounded run of those,
// so a run that keeps hitting the bound is naming the blocks that stopped
// charging. Small direct-mapped table, printed at exit.
#define BLUEWAKE_ZC_SLOTS 1024u
static u32 g_zc_pc[BLUEWAKE_ZC_SLOTS];
static u64 g_zc_count[BLUEWAKE_ZC_SLOTS];
static u32 g_turn_last_edge_pc;

static void host_zero_charge_note(u32 pc) {
    u32 slot = (pc >> 2) * 2654435761u % BLUEWAKE_ZC_SLOTS;
    for (u32 probe = 0; probe < 8u; ++probe) {
        const u32 i = (slot + probe) % BLUEWAKE_ZC_SLOTS;
        if (g_zc_count[i] == 0u || g_zc_pc[i] == pc) {
            g_zc_pc[i] = pc;
            g_zc_count[i]++;
            return;
        }
    }
}
static bool g_turn_edge_hit;
static s64 g_turn_downcount_before_last_block;

static void host_turn_exit_classify(CPUState* cpu, int dispatched) {
    g_turn_census_turns++;
    g_turn_census_blocks += g_turn_blocks_this_turn;
    const bool past_budget = cpu->cycle_budget > 0 &&
                             cpu->downcount <= -(s64)cpu->cycle_budget;
    const bool last_block_charged =
        cpu->downcount != g_turn_downcount_before_last_block;
    if (!dispatched)
        g_turn_exit_miss++;
    else if (g_turn_edge_hit)
        g_turn_exit_edge++;
    else if (past_budget)
        g_turn_exit_budget++;
    else if (!last_block_charged) {
        g_turn_exit_zero_charge++;
        host_zero_charge_note(g_turn_last_edge_pc);
    }
    else
        g_turn_exit_other++;
}

static void report_credit_census(void) {
    if (!g_cycle_credit_census)
        return;
    const u64 calls = g_cycle_credit_calls != 0u ? g_cycle_credit_calls : 1u;
    fprintf(stderr,
            "[credit-census] turns=%llu zero=%llu sum=%llu max=%llu "
            "over_budget=%llu mean=%.1f cycles/turn "
            "budget_mean=%.1f budget_min=%llu budget_max=%llu "
            "lag_mean=%.1f lag_max=%llu\n",
            (unsigned long long)g_cycle_credit_calls,
            (unsigned long long)g_cycle_credit_zero,
            (unsigned long long)g_cycle_credit_sum,
            (unsigned long long)g_cycle_credit_max,
            (unsigned long long)g_cycle_credit_over_budget,
            (double)g_cycle_credit_sum / (double)calls,
            (double)g_credit_budget_sum / (double)calls,
            (unsigned long long)g_credit_budget_min,
            (unsigned long long)g_credit_budget_max,
            (double)g_credit_lag_sum / (double)calls,
            (unsigned long long)g_credit_lag_max);
    if (g_turn_exit_zero_charge != 0u) {
        // Top sixteen blocks by zero-charge exits.
        u32 top_pc[16] = {0};
        u64 top_n[16] = {0};
        for (u32 i = 0; i < BLUEWAKE_ZC_SLOTS; ++i) {
            for (u32 k = 0; k < 16u; ++k) {
                if (g_zc_count[i] > top_n[k]) {
                    for (u32 j = 15u; j > k; --j) {
                        top_n[j] = top_n[j - 1u];
                        top_pc[j] = top_pc[j - 1u];
                    }
                    top_n[k] = g_zc_count[i];
                    top_pc[k] = g_zc_pc[i];
                    break;
                }
            }
        }
        for (u32 k = 0; k < 16u && top_n[k] != 0u; ++k)
            fprintf(stderr, "[zero-charge] pc=0x%08X exits=%llu\n", top_pc[k],
                    (unsigned long long)top_n[k]);
    }
    if (g_turn_census_enabled && g_turn_census_turns != 0u) {
        const double turns = (double)g_turn_census_turns;
        fprintf(stderr,
                "[turn-exit] turns=%llu blocks/turn=%.2f budget=%llu(%.1f%%) "
                "edge=%llu(%.1f%%) zero_charge=%llu(%.1f%%) miss=%llu(%.1f%%) "
                "other=%llu(%.1f%%)\n",
                (unsigned long long)g_turn_census_turns,
                (double)g_turn_census_blocks / turns,
                (unsigned long long)g_turn_exit_budget,
                100.0 * (double)g_turn_exit_budget / turns,
                (unsigned long long)g_turn_exit_edge,
                100.0 * (double)g_turn_exit_edge / turns,
                (unsigned long long)g_turn_exit_zero_charge,
                100.0 * (double)g_turn_exit_zero_charge / turns,
                (unsigned long long)g_turn_exit_miss,
                100.0 * (double)g_turn_exit_miss / turns,
                (unsigned long long)g_turn_exit_other,
                100.0 * (double)g_turn_exit_other / turns);
    }
}
// BLUEWAKE_GX_FLUSH_CENSUS. The rendered play window's per-retrace cost is bimodal
// with a two-retrace period, and the retrace that carries the presentation is
// where the whole of the rendered gap sits (docs/status/CURRENT.md, 2026-09-22).
// The main thread meets the translation worker at one barrier - the draw-done
// commit, which drains everything appended so far - so timing that one call says
// whether the slow retrace is waiting on the worker or doing its own work. It is
// a host call site, so this needs no patch inside the pinned dependency.
static bool g_gx_flush_census;
static u64 g_gx_flush_calls;
static u64 g_gx_flush_us_total;
static u64 g_gx_flush_us_max;
static u64 g_gx_flush_retrace;
static u64 g_gx_flush_retrace_calls;
static u64 g_gx_flush_retrace_us;
static unsigned g_gx_flush_lines;
static u16 g_pad_pulse_buttons;
static DolDi g_di;
static unsigned g_di_read_reports;
static unsigned g_vi_ack_reports;
static unsigned g_dvd_open_reports;
static unsigned g_dvd_read_reports;
static unsigned g_archive_reports;
static unsigned g_dynamic_load_reports;
static bool g_dynamic_link_header_reported;
static unsigned g_dyl_state_reports;
static unsigned g_rel_lifecycle_trace_reports;
static unsigned g_rel_call_trace_reports;
static unsigned g_rel_invalid_target_reports;
static unsigned g_rel_indirect_trace_reports;
static unsigned g_rel_solid_hole_reports;
static unsigned g_rel_continuation_reports;
static u32 g_rel_lwood_ctor_watch;
static unsigned g_resource_lookup_reports;
static unsigned g_resource_info_reports;
static unsigned g_aram_dma_trace_reports;
static BluewakeAramDma g_aram_dma;
static unsigned g_aram_dma_completion_reports;
static unsigned g_aram_dma_ack_reports;
static unsigned g_aram_interrupt_trace_reports;
static unsigned g_idle_context_overwrite_reports;
static unsigned g_aram_external_save_reports;
static u32 g_heap_write_watch_dispatch_pc;
static unsigned g_heap_write_watch_reports;
static unsigned g_heap_write_watch_control_reports;
static unsigned g_heap_write_watch_malformed_reports;
static unsigned g_heap_write_watch_external_reports;
static unsigned g_external_dispatch_store_reports;
static unsigned long long g_heap_write_watch_total;
static bool g_heap_write_watch_late;
BLUEWAKE_TRACE_STORAGE(g_audio_object_watch);
static unsigned g_audio_object_watch_reports;
static unsigned g_audio_context_interrupt_reports;
static unsigned g_audio_dsp_handler_reports;
static unsigned g_audio_dsp_callback_reports;
static unsigned g_audio_dsp_handler_step_reports;
static unsigned g_audio_dsp_boot_reports;
static unsigned g_audio_dsp_source_reports;
static unsigned g_audio_interrupt_state_reports;
static unsigned g_audio_msr_transition_reports;
static u32 g_audio_last_msr;
static u32 g_audio_last_msr_pc;
static u32 g_audio_last_msr_context;
static bool g_audio_thread_active;
static unsigned g_audio_dsp_boot_irq_reports;
static unsigned g_audio_dma_chunk_reports;
static unsigned g_audio_dma_irq_reports;
static unsigned g_audio_ai_handler_reports;
static unsigned g_audio_transition_trace_remaining;
BLUEWAKE_TRACE_STORAGE(g_pad_lifecycle_trace);
static unsigned g_pad_lifecycle_reports;
BLUEWAKE_TRACE_STORAGE(g_j2d_object_watch);
static unsigned g_j2d_object_watch_reports;
BLUEWAKE_TRACE_STORAGE(g_j2d_lookup_trace);
static unsigned g_j2d_lookup_reports;
static unsigned g_j2d_target_reports;
static bool g_j2d_file_select_set_active;
static unsigned g_j2d_resource_reports;
static unsigned g_j2d_tree_reports;
static unsigned g_j2d_create_reports;
BLUEWAKE_TRACE_STORAGE(g_j2d_payload_watch);
static unsigned g_j2d_payload_reports;
BLUEWAKE_TRACE_STORAGE(g_j2d_payload_flow_trace);
static unsigned g_j2d_payload_flow_reports;
BLUEWAKE_TRACE_STORAGE(g_title_profile_watch);
static unsigned g_title_profile_watch_reports;
static u32 g_guest_clock_decrementer;
static bool g_guest_clock_decrementer_valid;
static bool g_guest_clock_decrementer_expired;
static u32 g_guest_clock_cycle_remainder;
static unsigned g_guest_clock_reports;
static unsigned g_decrementer_context_reports;
static unsigned g_interrupt_delivery_reports;
BLUEWAKE_TRACE_STORAGE(g_guest_clock_trace);
static bool g_guest_decrementer_pending;
// Opt-in census for W1 (docs/status/REORGANIZATION_2026-09-17.md). The external
// interrupt is delivered only when bluewake_scheduler_interrupt_safe() says the
// pc is an architectural interrupt boundary; the decrementer has no such test.
// This counts how often each line delivers at an unsafe pc, so the claim that a
// chassis-chosen turn boundary is where the difference shows can be measured
// before anything is changed. Inert unless BLUEWAKE_DELIVERY_SAFETY_CENSUS is
// exported; it reads guest state and writes only to stderr.
static bool g_delivery_safety_census_enabled;
static u64 g_delivery_safety_calls;
static u64 g_delivery_safety_unsafe;
static unsigned g_delivery_safety_reports;
// Exact-cycle event timeline. The guest-state trace showed the two
// configurations are bit-identical at identical cycles and then come apart, but
// it samples at host flush boundaries, which differ between them. These two
// lines record the host's own events with the guest cycle they happened at, and
// a guest cycle is the same quantity in both configurations, so the sequences
// compare exactly rather than at best.
static unsigned g_delivery_timeline_reports;
static unsigned g_source_timeline_reports;
// DSP-side work counters. The device summaries say the whole DSP is dead in the
// chassis path (0 DMAs from DSP against 71,538, and 17 ARAM transfers against
// 4,310), so these say whether the host ever asks it to run and how much of the
// guest it was given.
static u64 g_dsp_advance_calls;
static u64 g_dsp_advance_iters;
static u64 g_dsp_run_calls;
static u64 g_dsp_run_cpu_cycles;
static u64 g_dsp_mail_sync_calls;
static u64 g_dsp_mail_sync_slices;
// The same census tracks the links upstream of delivery, because "deliveries are
// 16x too rare" has two very different causes: the host failing to advance the
// guest clock (so no expiry is ever noticed), or the guest refusing delivery by
// holding EE down across many turns. Counting both separates them.
static u64 g_clock_advance_calls;
static u64 g_clock_advance_cycles;
static u64 g_clock_decrementer_expiries;
static u64 g_decrementer_pending_turns;
static u64 g_decrementer_blocked_turns;
static u64 g_decrementer_blocked_streak;
static u64 g_decrementer_blocked_max_streak;
// Host-side event rate. This is the quantity that must NOT depend on how long a
// host turn is: if the chassis changes it, the device service is dropping or
// merging events rather than merely noticing them later.
static u64 g_source_publishes;
static u64 g_source_rise_di;
static u64 g_source_rise_dsp;
static u64 g_source_rise_si;
static u64 g_source_true_publishes;
static bool g_chassis_service_each_block;
// A steered player route (BLUEWAKE_PAD_PLAYER_TARGET_*) starts and steers at
// turns that begin at 0x80122D30 (the player update, player actor in r3). Since
// the chassis runs many blocks a turn, a turn rarely begins there, so the route
// never started, and once started it steered once. Until the route finishes,
// a boundary at that pc goes to the full service and ends the turn
// (host_chassis_edge_service_body); every other boundary stays on the fast path.
static bool g_player_route_waiting;
// Differential guest-state trace (W1, docs/status/REORGANIZATION_2026-09-17.md).
// Every host action that could have been omitted has been measured away, so the
// remaining question is what the guest itself sees differently. This samples the
// same guest-state fingerprint at the same guest cycle in both configurations, so
// the two runs can be compared to first difference instead of to first log line.
static bool g_guest_state_trace_enabled;
static u64 g_guest_state_trace_next = 100000ull;
static unsigned g_guest_state_trace_reports;
static BluewakeCycleDomain g_cycle_domain;
static BluewakeDeliveryDigest g_delivery_digest;
// Bounded delivery-trace window (BLUEWAKE_DELIVERY_TRACE=LO:HI). Prints the
// identity and cycle of individual external deliveries so two runs with
// different turn schedules can be compared record-by-record.
static u64 g_delivery_trace_lo;
static u64 g_delivery_trace_hi;
// Play-scene delivery trace (BLUEWAKE_DELIVERY_TRACE_PLAY=N). Prints the first
// N deliveries that happen after the play-scene milestone, which is the window
// the recorded history cannot reach.
static u64 g_delivery_play_trace;
static DolViClock* g_cycle_vi_clock;
static u64 g_vi_cycle_cursor;
static u64 g_audio_cycle_cursor;
static u64 g_dsp_cycle_cursor;
static u64 g_current_host_block;
static unsigned g_vi_assert_reports;
static u64 g_previous_retrace_timebase;

#define GUEST_CYCLES_PER_TIMEBASE_TICK 12u
#define GUEST_CPU_CYCLES_PER_SECOND 486000000ull
#define GUEST_VI_REFRESH_HZ 60u
#define GUEST_CPU_CYCLES_PER_VI_RETRACE \
    (GUEST_CPU_CYCLES_PER_SECOND / GUEST_VI_REFRESH_HZ)

#ifdef BLUEWAKE_HAS_DSP_ADAPTER
// How often the LLE DSP is stepped, in guest cycles. The deadline census on the
// 2026-09-17 build shows this cadence ending 94.5% of dispatch windows
// (119,999,654 of 127,001,699 deadline calls, with the cycle cap never binding),
// so it is the thing that decides how long a window may be and therefore how
// many host turns the route pays for. Runtime so it can be swept; the shipping
// default is the value this project has always used.
static u64 g_dsp_update_rate = 12600u;
static BluewakeDspAdapter* g_dsp_adapter;
static unsigned g_dsp_adapter_dma_reports;
static u64 g_dsp_adapter_dma_count;
static bool g_dsp_adapter_first_nonzero_reported;
static u64 g_dsp_adapter_first_nonzero_host_retrace;
static u32 g_dsp_adapter_first_nonzero_retail_retrace;
static unsigned g_dsp_adapter_interrupt_reports;
static unsigned g_dsp_adapter_guest_read_reports;
static unsigned g_dsp_adapter_mail_read_reports;
static bool g_dsp_adapter_interrupt_pending;
static u64 g_dsp_adapter_slice_cycles;
static u64 g_dsp_adapter_update_elapsed;
#endif

static void guest_clock_advance(CPUState* cpu, u64 elapsed_cycles);
static void host_sync_cycle_devices(CPUState* cpu);
static void host_refresh_interrupt_sources(CPUState* cpu);
static void host_sync_cycle_devices_end_turn(CPUState* cpu);
static void deliver_external_interrupt(CPUState* cpu);
static void deliver_decrementer_exception(CPUState* cpu);
static u32 host_canonical_linked_pc(u32 pc);
// Interrupt-source recomputation gate; see the block comment at its definition.
// Declared here because the chassis edge service, which runs once per block
// boundary, tests it to skip a call that would return immediately.
static bool g_interrupt_sources_dirty = true;

static u64 host_cycle_min_source(u64 current, u64 candidate, int tag,
                                 int* source) {
    if (candidate < current) {
        *source = tag;
        return candidate;
    }
    return current;
}

static u64 host_cycle_deadline_distance(const CPUState* cpu, void* user) {
    (void)cpu;
    (void)user;
    u64 distance = UINT64_MAX;
    int source = BLUEWAKE_DEADLINE_SOURCE_NONE;

    if (g_cycle_vi_clock != NULL) {
        u64 deadline = 0u;
        const u64 lag = g_cycle_domain.absolute_cycles - g_vi_cycle_cursor;
        const u64 now = dol_vi_clock_now(g_cycle_vi_clock) + lag;
        if (dol_event_clock_next_deadline(&g_cycle_vi_clock->events,
                                          &deadline))
            distance = host_cycle_min_source(
                distance, deadline > now ? deadline - now : 1u,
                BLUEWAKE_DEADLINE_SOURCE_VI, &source);
    }
#ifdef BLUEWAKE_HAS_DSP_ADAPTER
    if (g_dsp_adapter != NULL) {
        const u64 lag = g_cycle_domain.absolute_cycles - g_dsp_cycle_cursor;
        const u64 elapsed = g_dsp_adapter_update_elapsed + lag;
        const u64 dsp_distance =
            elapsed < g_dsp_update_rate
                ? g_dsp_update_rate - elapsed
                : 1u;
        distance = host_cycle_min_source(distance, dsp_distance,
                                         BLUEWAKE_DEADLINE_SOURCE_DSP,
                                         &source);
    }
#endif
    if ((g_audio_dma.control & DOL_AUDIO_DMA_ENABLE) != 0u &&
        g_audio_dma.work_units_per_chunk != 0u) {
        const u64 lag = g_cycle_domain.absolute_cycles - g_audio_cycle_cursor;
        const u64 elapsed = g_audio_dma.work_counter + lag;
        const u64 audio_distance =
            elapsed < g_audio_dma.work_units_per_chunk
                ? g_audio_dma.work_units_per_chunk - elapsed
                : 1u;
        distance = host_cycle_min_source(distance, audio_distance,
                                         BLUEWAKE_DEADLINE_SOURCE_AUDIO,
                                         &source);
    }
    if (g_guest_clock_decrementer_valid &&
        !g_guest_clock_decrementer_expired &&
        (g_guest_clock_decrementer & 0x80000000u) == 0u) {
        const u64 ticks = (u64)g_guest_clock_decrementer + 1u;
        const u64 cycles = ticks * GUEST_CYCLES_PER_TIMEBASE_TICK;
        distance = host_cycle_min_source(
            distance,
            cycles > g_guest_clock_cycle_remainder
                ? cycles - g_guest_clock_cycle_remainder
                : 1u,
            BLUEWAKE_DEADLINE_SOURCE_DECREMENTER, &source);
    }
    if ((cpu->msr & PPC_MSR_EE) != 0u &&
        (g_guest_decrementer_pending ||
         dol_interrupts_external_pending(&g_interrupts)))
        distance = host_cycle_min_source(distance, 1u,
                                         BLUEWAKE_DEADLINE_SOURCE_CLAMP,
                                         &source);
    if (g_deadline_census_enabled) {
        // bounded_budget() clamps the budget to the cycle-domain cap first, so
        // the deadline only owns the budget when its distance is within that
        // cap. Attribute by the term that actually bound, not by the minimum.
        const u64 deadline_cap =
            g_cycle_domain.cap > 0 ? (u64)g_cycle_domain.cap : 256u;
        const int bucket =
            (source == BLUEWAKE_DEADLINE_SOURCE_NONE || distance > deadline_cap)
                ? BLUEWAKE_DEADLINE_SOURCE_CAP
                : source;
        g_deadline_calls++;
        g_deadline_source_counts[bucket]++;
        if (distance == 1u)
            g_deadline_one_cycle_counts[bucket]++;
        if (g_host_retrace_count >= g_deadline_census_window) {
            g_deadline_window_calls++;
            g_deadline_window_counts[bucket]++;
            if (distance == 1u)
                g_deadline_window_one_cycle_counts[bucket]++;
        }
    }
    return distance;
}

static void host_cycle_advance_clock(CPUState* cpu, u64 cycles, void* user) {
    (void)user;
    guest_clock_advance(cpu, cycles);
}

static void host_prepare_guest_dispatch(CPUState* cpu, void* user) {
    BluewakeCycleDomain* domain = user;
    bluewake_cycle_domain_prepare_dispatch(domain, cpu);
}

// BLUEWAKE_CHASSIS_BUDGET. Without an edge service the chassis returns after
// its first dispatch (cmake/composite/dispatch_loop.c), so one host turn is one
// guest block: 18.4 guest cycles per turn in the Outset scene against a 256-to-
// 1024 cycle budget, so the host pays a whole device-service pass - the DSP
// slice, the deadline computation, the interrupt refresh - per block instead of
// per budget window.
//
// The first version of this answered "no host work pending" unconditionally and
// the guest never left early boot. The per-turn body is not only that device
// pass: it also publishes what the devices have made pending and decides whether
// an interrupt has to be delivered at this pc. The chassis runs blocks the host
// never sees, so that decision has to live in the call the chassis already makes
// between blocks. Publishing is cheap by construction - the refresh only rebuilds
// the dispatch budget when the published set changes - and the delivery test is
// the same contract the per-turn body applies. Device advancement stays where it
// is, on the cycle cursors, which are elapsed-based and therefore unaffected by
// how long a turn is.
// Per-block overlap observation cache.
//
// The three reads above run at every block boundary for the whole route once
// the name scene exists, because g_name_scene_object is set once and never
// cleared, so their cost is the address translation rather than the loads: each
// one is a get_ram_ptr call and this program's guest layout has RELs mapped over
// MEM1, which is the case that takes the slow path through the alias resolver.
// Both addresses are stable - the slot is a constant and the object changes
// rarely - so the host pointers are resolved once and revalidated whenever the
// aliasing state or the object address changes, with the runtime accessors as
// the fallback for an address that does not resolve.
//
// The loads use the runtime's own readers because MEM1 holds guest byte order
// and a plain dereference would read it backwards. That is not a hypothetical:
// it is the one way to write this that produces plausible-looking garbage.
static const u8* g_overlap_slot_ptr;
static const u8* g_overlap_fields_ptr;
static u32 g_overlap_cached_object;
// BLUEWAKE_OVERLAP_OBSERVATION=0 skips this observation. It is for the route
// digest, which records the overlap phase; it never changes the guest, and a
// player's session (the Windows app sets it) has no digest to record.
static bool g_overlap_observation = true;
// The guest-alias generation the cached slot pointer was resolved under. It
// starts at a value the counter cannot hold so that the first call resolves,
// which is what g_overlap_cache_valid used to do as a separate flag - one load
// and one test that this replaces. See the census in docs/status/CURRENT.md,
// 2026-09-22: this revalidation runs at every block boundary.
static u32 g_overlap_cached_alias_state = 0xFFFFFFFFu;

static u32 host_overlap_object(CPUState* cpu) {
    const u32 alias_state = g_ppc_guest_alias_generation;
    if (alias_state != g_overlap_cached_alias_state) {
        g_overlap_slot_ptr = get_ram_ptr(cpu, 0x803F6160u, 4u, NULL);
        g_overlap_cached_object = 0u;
        g_overlap_cached_alias_state = alias_state;
    }
    if (g_overlap_slot_ptr == NULL)
        return mem_read32(cpu, 0x803F6160u);
    return read_be32(g_overlap_slot_ptr);
}

// Resolved once per object, asking for the whole field span so the reads at
// +0x04 and +0x1C cannot cross out of the region the pointer came from. A NULL
// answer means the runtime accessors decide, exactly as before.
static const u8* host_overlap_fields(CPUState* cpu, u32 object) {
    if (object != g_overlap_cached_object) {
        g_overlap_fields_ptr = get_ram_ptr(cpu, object, 0x20u, NULL);
        g_overlap_cached_object = object;
    }
    return g_overlap_fields_ptr;
}

static void host_observe_ground_cross_return(CPUState* cpu,
                                             unsigned long long blocks);
static u32 host_f32_bits(f64 value);
static bool host_chassis_edge_service_body(void* user, CPUState* cpu, u32 address) {
    (void)user;
    (void)address;
    if (__builtin_expect(g_player_route_waiting, 0) && address == 0x80122D30u)
        return true;  // let the per-turn body see the route's start pc
#if BLUEWAKE_EDGE_CENSUS
    g_edge_calls++;
#endif
    if (g_boundary_census_enabled) {
        g_boundary_calls++;
        if (g_host_retrace_count >= g_boundary_census_window) {
            g_boundary_window_calls++;
            // Per target: how many boundary-loop iterations land at each guest
            // address (a dispatch into a function entry or a return site).
            if (g_boundary_by_address != NULL) {
                const u32 offset = (address & 0x3FFFFFFFu) >> 2;
                if (offset < BOUNDARY_ADDRESS_WORDS)
                    g_boundary_by_address[offset]++;
            }
        }
        if (g_play_scene_reported)
            g_boundary_play_calls++;
    }
    if (cpu == NULL)
        return false;
    // The per-turn body samples host observations once per turn, and in the
    // shipping build one turn is one block, so they are sampled at every block
    // boundary. The chassis takes fewer turns, so an observation whose value
    // changes inside a window is missed: the overlap phase is the only one the
    // route digest sees, and it was recorded as 5 where shipping recorded 6.
    // The object itself is bit-identical at 201 of 201 retrace-aligned samples
    // in both configurations, so this is observation cadence and not guest
    // state. Sampling it here restores the shipping cadence, which is one look
    // per block boundary - the same call the chassis already makes.
    if (g_overlap_observation && g_name_scene_object >= 0x80000000u &&
        (g_file_start_pulse.triggered || !g_file_start_pulse.configured)) {
#if BLUEWAKE_EDGE_CENSUS
        g_edge_overlap_guard++;
#endif
        const u32 overlap_object = host_overlap_object(cpu);
        if (overlap_object >= 0x80000000u) {
#if BLUEWAKE_EDGE_CENSUS
            g_edge_overlap_object++;
#endif
            const u8* object = host_overlap_fields(cpu, overlap_object);
#if BLUEWAKE_EDGE_CENSUS
            if (object != NULL)
                g_edge_overlap_fast++;
#endif
            const u16 enabled =
                object != NULL ? read_be16(object + 0x04u)
                               : mem_read16(cpu, overlap_object + 0x04u);
            if (enabled == 1u) {
#if BLUEWAKE_EDGE_CENSUS
                g_edge_overlap_enabled++;
#endif
                const u32 phase =
                    object != NULL ? read_be32(object + 0x1Cu)
                                   : mem_read32(cpu, overlap_object + 0x1Cu);
                if (phase != g_overlap_last_phase) {
#if BLUEWAKE_EDGE_CENSUS
                    g_edge_overlap_phase_changed++;
#endif
                    g_overlap_last_phase = phase;
                    g_overlap_terminal_phase = phase;
                }
            }
        }
    }
    // Host observations the per-turn body performs at one specific guest pc.
    // They are host bookkeeping rather than guest state, and they are one-shot,
    // so the cost of returning to the host for them vanishes once each has
    // fired. The milestone here is the one the acceptance summary gates on that
    // a chassis turn cannot reach on its own; the phase read further down is
    // sampled per turn and is recorded separately.
    if (!g_new_game_intro_reported &&
        (address == 0x80018554u || address == 0x8001199Cu)) {
#if BLUEWAKE_EDGE_CENSUS
        g_edge_intro_hits++;
#endif
        return true;
    }
    // The per-turn body is not generic. It carries a pc-keyed interception
    // table - the JAudio DSP task boot handshake at 0x803193AC, which arms the
    // second DSP boot handshake, plus the GX draw-done return, the DVD and
    // archive entries and the module-1 raw alias. Those fire in the shipping
    // build only because it takes a host turn at every block boundary, so a
    // chassis that returns on interrupt conditions alone skips all of them.
    // edge_intercepts.c exists to expose exactly this set and was never wired
    // up; the chassis has to return to the host at those addresses.
    const bool edge_requires =
        bluewake_edge_requires_host(address, host_canonical_linked_pc(address),
                                    g_module1_raw_base);
#if BLUEWAKE_EDGE_CENSUS
    if (edge_requires)
        g_edge_predicate_true++;
    else
        g_edge_predicate_false++;
#endif
    if (edge_requires)
        return true;
    // BLUEWAKE_CHASSIS_SERVICE. Diagnostic form of W1's experiment 2
    // (docs/status/REORGANIZATION_2026-09-17.md). The chassis returns to the
    // host once per budget window, so the per-turn device service and the guest
    // clock advance run once per window instead of once per block. This runs
    // both here, at every block boundary the chassis already stops at, which
    // removes the speedup by construction and answers one question: does the
    // guest's boot depend on the *servicing* being per-block, or on the host
    // *turn* being per-block? If the boot returns with this on, the servicing is
    // the requirement and the recoverable cost is the outer-turn overhead; if it
    // does not, the mechanism is elsewhere and this class of fix is closed.
#if BLUEWAKE_EDGE_CENSUS
    if (g_chassis_service_each_block)
        g_edge_service_each_block++;
#endif
    if (g_chassis_service_each_block) {
        bluewake_card_runtime_service_callback(cpu);
        (void)bluewake_card_runtime_dispatch(cpu);
        (void)bluewake_cycle_domain_flush(&g_cycle_domain, cpu);
        host_sync_cycle_devices_end_turn(cpu);
        // The budget is the decision input: the deadline's own clamp sets it to
        // one cycle whenever an interrupt is pending with EE set, so a chassis
        // window that keeps its opening budget cannot end on that clamp.
        bluewake_cycle_domain_rebudget(&g_cycle_domain, cpu);
    }
    // The refresh is a no-op unless a device event has dirtied the source set,
    // and those events arrive on host entry points that run per turn and per
    // device access - about once in fifty of the boundaries this service is
    // called on. Testing the same flag here rather than inside the callee is
    // exactly equivalent (host_refresh_interrupt_sources starts with this test)
    // and keeps the call, its frame and its argument out of the other
    // forty-nine. It is the edge service's remaining per-boundary call and the
    // boundary census counts 391,432 boundaries a play retrace:
    // docs/status/CURRENT.md, 2026-09-22.
    if (g_interrupt_sources_dirty)
        host_refresh_interrupt_sources(cpu);
    if ((cpu->msr & PPC_MSR_EE) != 0u && g_guest_decrementer_pending) {
#if BLUEWAKE_EDGE_CENSUS
        g_edge_interrupt_ee++;
#endif
        return true;
    }
    const bool scheduler_requires = bluewake_scheduler_interrupt_requires_host(
        cpu->pc, cpu->msr, dol_interrupts_external_pending(&g_interrupts));
#if BLUEWAKE_EDGE_CENSUS
    if (scheduler_requires)
        g_edge_scheduler_true++;
#endif
    // This edge does not end the turn, so the turn body will not see it: make
    // the GroundCross observation here (see host_observe_ground_cross_return).
    if (!scheduler_requires && address == 0x80328F84u &&
        cpu->lr == 0x80246A04u)
        host_observe_ground_cross_return(cpu, 0u);
    return scheduler_requires;
}

// The chassis loop calls this once per block, so it is where a turn's block
// count and its last block's starting downcount are observed; whether the call
// handled the address is the loop's edge-service exit. See
// host_turn_exit_classify.
// The GroundCross return observation (the collision-provenance ABI check): at
// every return through _restgpr_27 to 0x80246A04, f1 must carry the ground
// height the check object holds. It used to run only at host turn starts, so
// 0x80328F84 was an edge the chassis had to return on - 98 percent of all the
// play window's turn exits, 5,700 a retrace, each paying the whole turn body
// (docs/status/CURRENT.md, 2026-09-24). It is now also made at the edge itself,
// by the edge service, whenever that edge does not end the turn, so every
// arrival is observed exactly once either way. blocks is the turn's block
// count when called from the turn body and 0 from an edge (it only labels the
// one-shot log lines; the summary does not carry it).
static u64 g_ground_cross_returns;
static u64 g_ground_cross_deferred_returns;
static bool g_ground_cross_valid_seen;
static bool g_ground_cross_sentinel_seen;
static const char* g_ground_cross_stop_reason;

static void host_observe_ground_cross_return(CPUState* cpu,
                                             unsigned long long blocks) {
    const u32 check = cpu->gpr[31];
    const u32 ground_bits = mem_read32(cpu, check + 52u);
    const u32 return_bits = host_f32_bits(cpu->fpr[1]);
    const u16 poly_index = mem_read16(cpu, check + 20u);
    const u16 bg_index = mem_read16(cpu, check + 22u);
    const u32 caller_lr = mem_read32(cpu, cpu->gpr[1] + 36u);
    const bool sentinel_height = ground_bits == 0xCE6E6B28u;
    const bool sentinel_metadata =
        poly_index == UINT16_MAX && bg_index == 256u;
    g_ground_cross_returns++;
    if ((sentinel_height && !g_ground_cross_sentinel_seen) ||
        (!sentinel_height && !g_ground_cross_valid_seen)) {
        fprintf(stderr,
                "[collision-provenance] ground-cross-return "
                "retrace=%llu blocks=%llu returns=%llu "
                "caller=0x%08X check=0x%08X ground=%08X "
                "poly=%u bg=%u tuple=%s\n",
                (unsigned long long)g_host_retrace_count,
                (unsigned long long)blocks,
                (unsigned long long)g_ground_cross_returns, caller_lr,
                check, ground_bits, poly_index, bg_index,
                sentinel_height ? "sentinel" : "ground");
    }
    g_ground_cross_sentinel_seen |= sentinel_height;
    g_ground_cross_valid_seen |= !sentinel_height;
    const bool fp_materialized =
        bluewake_fpu_registers_materialized(cpu);
    if (!fp_materialized) {
        g_ground_cross_deferred_returns++;
    } else if (return_bits != ground_bits) {
        fprintf(stderr,
                "[cpu-abi] ground-cross-f1-mismatch "
                "boundary=restore-entry retrace=%llu blocks=%llu "
                "returns=%llu caller=0x%08X check=0x%08X "
                "f1=%08X ground=%08X poly=%u bg=%u\n",
                (unsigned long long)g_host_retrace_count,
                (unsigned long long)blocks,
                (unsigned long long)g_ground_cross_returns, caller_lr,
                check, return_bits, ground_bits, poly_index, bg_index);
        g_ground_cross_stop_reason = "GroundCross f1 ABI invariant";
    }
    if (sentinel_height != sentinel_metadata) {
        fprintf(stderr,
                "[collision-provenance] incoherent-ground-cross "
                "retrace=%llu blocks=%llu returns=%llu "
                "caller=0x%08X check=0x%08X ground=%08X "
                "poly=%u bg=%u check_words=%08X,%08X,%08X,%08X\n",
                (unsigned long long)g_host_retrace_count,
                (unsigned long long)blocks,
                (unsigned long long)g_ground_cross_returns, caller_lr,
                check, ground_bits, poly_index, bg_index,
                mem_read32(cpu, check), mem_read32(cpu, check + 4u),
                mem_read32(cpu, check + 8u),
                mem_read32(cpu, check + 12u));
        g_ground_cross_stop_reason = "collision provenance";
    }
}

__attribute__((noinline)) static bool host_chassis_edge_service_full(
    void* user, CPUState* cpu, u32 address) {
    if (g_turn_census_enabled && cpu != NULL) {
        g_turn_blocks_this_turn++;
        g_turn_last_edge_pc = address;
        g_turn_downcount_before_last_block = cpu->downcount;
    }
    const bool handled = host_chassis_edge_service_body(user, cpu, address);
    if (g_turn_census_enabled && handled)
        g_turn_edge_hit = true;
    return handled;
}

// The chassis calls the edge service at every block boundary it stops at,
// through a pointer, so it cannot be inlined into the loop - and the full
// service above has calls on its rare paths, which made every call save and
// restore five register pairs: host_chassis_edge_service was 7.9 percent of
// the simulator's game thread in the heavy Outset view by its own time. This
// front answers the common case with loads and compares only, and hands every
// other case to the full service unchanged. It returns false only where the
// full service would return false having changed nothing: no census, no
// per-block servicing, clean interrupt sources, the overlap phase unchanged
// through the cached pointers, no intercepted address, and no interrupt the
// guest could take. It writes nothing.

// Actor search by process id, run natively between two of its own boundaries.
//
// fopAcM_SearchByID and its relatives walk the actor queue with cNdIt_Judge,
// which calls cTgIt_JudgeFilter through a pointer for every node, which calls
// fpcSch_JudgeByID through a pointer. In the translated code every bctrl and
// every return into another chunk goes back to the chassis, so each node costs
// three boundary-loop iterations, three edge-service calls and three chunk
// entries to compare one word: 204,000 of the 384,000 boundaries a play
// retrace takes (BLUEWAKE_BOUNDARY_CENSUS_BY_ADDRESS over the certified window,
// 68,000 nodes a retrace; docs/status/CURRENT.md, 2026-09-24).
//
// At the boundary that enters cTgIt_JudgeFilter (0x80245640) from cNdIt_Judge's
// bctrl (0x80244F84), this runs whole iterations whose judge answers NULL and
// hands the chassis the same boundary one or more nodes later, with the state
// the translated blocks leave there: the registers they write, CR0 from the
// loop's last cmplwi, the two stack words JudgeFilter stores, downcount charged
// block by block and the observation suffix of the last observing instruction.
// It runs an iteration only when every block in it would take the precharged
// path (no deadline inside the block, no suffix refund) and no budget check in
// it would stop the guest - the leader checks, the two return dispatches, the
// loop back-edge and the chassis check at the next boundary - so every stop,
// refund and precise charge stays with the translated code. The node that
// matches, and the list's last node, are left to the translated code too. Its
// caller has just established that the edge service has nothing to do at this
// boundary, and the loop reads RAM and writes two stack words, so the service
// would have had nothing to do at any boundary it skips. The certified route
// digest is the gate (BLUEWAKE_ACTOR_SEARCH_NATIVE=0 turns it off).
#define BW_SEARCH_JUDGE_FILTER 0x80245640u
#define BW_SEARCH_JUDGE_BY_ID 0x80040068u
#define BW_SEARCH_NDIT_RETURN 0x80244F88u
static bool g_actor_search_native = true;
static u64 g_actor_search_native_nodes;
static u64 g_actor_search_native_runs;

static inline const u8* bw_search_word(CPUState* cpu, u32 address) {
    if ((address & 3u) != 0u)
        return NULL;
    return get_ram_ptr(cpu, address, 4u, NULL);
}

// Out of line: inlined, its registers made every edge-service call save and
// restore eight of them, on the path that runs at every block boundary.
__attribute__((noinline)) static void host_actor_search_native(CPUState* cpu) {
    if (cpu->lr != BW_SEARCH_NDIT_RETURN || cpu->gpr[29] != BW_SEARCH_JUDGE_FILTER ||
        (cpu->ctr & ~3u) != BW_SEARCH_JUDGE_FILTER || cpu->gpr[30] != cpu->gpr[4] ||
        cpu->host_call != NULL || cpu->exception != 0u)
        return;
    const s64 budget = cpu->cycle_budget;
    const s64 deadline = cpu->cycle_deadline_budget;
    // Suffixes in these blocks reach 9; a smaller positive deadline refunds.
    if (budget <= 0 || (deadline > 0 && deadline < 9))
        return;
    const u32 filter = cpu->gpr[4];
    const u8* judge_word = bw_search_word(cpu, filter);
    const u8* user_word = bw_search_word(cpu, filter + 4u);
    if (judge_word == NULL || user_word == NULL ||
        read_be32(judge_word) != BW_SEARCH_JUDGE_BY_ID)
        return;
    const u8* id_word = bw_search_word(cpu, read_be32(user_word));
    const u32 sp = cpu->gpr[1];
    if (id_word == NULL || bw_search_word(cpu, sp - 16u) == NULL ||
        bw_search_word(cpu, sp + 4u) == NULL)
        return;
    const u32 id = read_be32(id_word);

    u32 node = cpu->gpr[3];
    u32 next = cpu->gpr[31];
    u32 last_id = cpu->gpr[5];
    s64 downcount = cpu->downcount;
    u32 nodes = 0;
    for (;;) {
        if (next == 0u)
            break;  // the loop ends after this node
        const u8* tag_data = bw_search_word(cpu, node + 12u);
        if (tag_data == NULL)
            break;
        const u8* proc_id = bw_search_word(cpu, read_be32(tag_data) + 4u);
        const u8* next_next = bw_search_word(cpu, next + 8u);
        if (proc_id == NULL || next_next == NULL)
            break;
        const u32 pid = read_be32(proc_id);
        if (pid == id)
            break;  // the match returns through the translated code
        // JudgeFilter entry (10), JudgeByID (4) and its not-equal tail (2),
        // JudgeFilter's return (5), then cNdIt_Judge: the NULL test (2), the
        // node advance (3), the next load (2), the loop test (2) and the call
        // block (5). Each leader stands for its block's budget check; the
        // return dispatches and the back-edge test the same bound at the same
        // downcount as the leader that follows them.
        if (!bluewake_actor_search_iteration_fits(downcount, budget, deadline))
            break;
        downcount -= 35;
        last_id = pid;
        node = next;
        next = read_be32(next_next);
        nodes++;
    }
    if (nodes == 0u)
        return;
    // JudgeFilter's frame: the back chain and the saved return address.
    mem_write32(cpu, sp - 16u, sp);
    mem_write32(cpu, sp + 4u, BW_SEARCH_NDIT_RETURN);
    cpu->gpr[0] = BW_SEARCH_NDIT_RETURN;   // JudgeFilter's lwz r0, 20(r1)
    cpu->gpr[3] = node;                    // cNdIt_Judge: or r3, r31, r31
    cpu->gpr[4] = filter;                  // or r4, r30, r30
    cpu->gpr[5] = last_id;                 // JudgeByID: lwz r5, 4(r3)
    cpu->gpr[12] = BW_SEARCH_JUDGE_FILTER; // or r12, r29, r29
    cpu->gpr[31] = next;                   // lwz r31, 8(r31)
    cpu->ctr = BW_SEARCH_JUDGE_FILTER;
    cpu->lr = BW_SEARCH_NDIT_RETURN;
    // CR0 from the loop test, cmplwi r3, 0 with r3 a non-null node: GT, and SO
    // copied from XER.
    cpu->cr = (cpu->cr & ~(0xFu << 28)) | ((0x4u | ((cpu->xer >> 31) & 1u)) << 28);
    cpu->cycle_observation_suffix = 1u;    // the call block's mtctr
    cpu->downcount = downcount;
    cpu->pc = BW_SEARCH_JUDGE_FILTER;
    g_actor_search_native_nodes += nodes;
    g_actor_search_native_runs++;
}

static bool host_chassis_edge_service(void* user, CPUState* cpu, u32 address) {
    bluewake_mouse_camera_dispatch(cpu, address);
    bluewake_climb_dispatch(cpu, address);
    bluewake_quick_doors_dispatch(cpu, address);
    bluewake_draw_tags_dispatch(cpu, address);
    bluewake_forest_water_dispatch(cpu, address);
    if (bluewake_jump_button_dispatch(cpu, address))
        return true;
    if (__builtin_expect(cpu == NULL || g_turn_census_enabled ||
                             g_boundary_census_enabled ||
                             g_chassis_service_each_block ||
                             g_interrupt_sources_dirty,
                         0))
        return host_chassis_edge_service_full(user, cpu, address);
    if (g_overlap_observation && g_name_scene_object >= 0x80000000u &&
        (g_file_start_pulse.triggered || !g_file_start_pulse.configured)) {
        if (g_ppc_guest_alias_generation != g_overlap_cached_alias_state ||
            g_overlap_slot_ptr == NULL)
            return host_chassis_edge_service_full(user, cpu, address);
        const u32 object = read_be32(g_overlap_slot_ptr);
        if (object >= 0x80000000u) {
            if (object != g_overlap_cached_object || g_overlap_fields_ptr == NULL)
                return host_chassis_edge_service_full(user, cpu, address);
            if (read_be16(g_overlap_fields_ptr + 0x04u) == 1u &&
                read_be32(g_overlap_fields_ptr + 0x1Cu) != g_overlap_last_phase)
                return host_chassis_edge_service_full(user, cpu, address);
        }
    }
    if (!g_new_game_intro_reported &&
        (address == 0x80018554u || address == 0x8001199Cu))
        return host_chassis_edge_service_full(user, cpu, address);
    // The address first: a compare with an immediate, where the flag is a load.
    if (__builtin_expect(address == 0x80122D30u, 0) && g_player_route_waiting)
        return host_chassis_edge_service_full(user, cpu, address);
    if ((g_module1_raw_base != 0u && address == g_module1_raw_base + 0xD4u) ||
        bluewake_edge_maybe_intercept(host_canonical_linked_pc(address)))
        return host_chassis_edge_service_full(user, cpu, address);
    if ((cpu->msr & PPC_MSR_EE) != 0u &&
        (g_guest_decrementer_pending ||
         (g_interrupts.pi_cause & g_interrupts.pi_mask) != 0u))
        return host_chassis_edge_service_full(user, cpu, address);
    if (address == BW_SEARCH_JUDGE_FILTER && g_actor_search_native)
        host_actor_search_native(cpu);
    return false;
}

// Host-only CPU state has no representation in the retail OSContext image.
typedef struct HostContextShadow {
    u32 context;
    bool valid;
    u32 dar;
    u32 dsisr;
    u32 ear;
    u32 hid2;
    u32 sr[16];
    u32 exception;
    u32 program_exception;
    u32 reserve_addr;
    bool reserve_valid;
} HostContextShadow;

#define HOST_CONTEXT_SHADOW_CAPACITY 64u
static HostContextShadow g_context_shadows[HOST_CONTEXT_SHADOW_CAPACITY];
static u64 g_fpu_switch_counts[4];

static HostContextShadow* host_context_shadow(u32 context, bool create) {
    HostContextShadow* free_slot = NULL;
    for (unsigned index = 0; index < HOST_CONTEXT_SHADOW_CAPACITY; index++) {
        HostContextShadow* shadow = &g_context_shadows[index];
        if (shadow->valid && shadow->context == context)
            return shadow;
        if (!shadow->valid && free_slot == NULL)
            free_slot = shadow;
    }
    if (!create || free_slot == NULL)
        return NULL;
    free_slot->context = context;
    free_slot->valid = true;
    return free_slot;
}

static void host_save_context_shadow(const CPUState* cpu, u32 context) {
    HostContextShadow* shadow = host_context_shadow(context, true);
    if (shadow != NULL) {
        shadow->dar = cpu->dar;
        shadow->dsisr = cpu->dsisr;
        shadow->ear = cpu->ear;
        shadow->hid2 = cpu->hid2;
        memcpy(shadow->sr, cpu->sr, sizeof shadow->sr);
        shadow->exception = cpu->exception;
        shadow->program_exception = cpu->program_exception;
        shadow->reserve_addr = cpu->reserve_addr;
        shadow->reserve_valid = cpu->reserve_valid;
    }
}

static void host_restore_context_shadow(CPUState* cpu, u32 context) {
    HostContextShadow* shadow = host_context_shadow(context, false);
    if (shadow != NULL) {
        cpu->dar = shadow->dar;
        cpu->dsisr = shadow->dsisr;
        cpu->ear = shadow->ear;
        cpu->hid2 = shadow->hid2;
        memcpy(cpu->sr, shadow->sr, sizeof shadow->sr);
        cpu->exception = shadow->exception;
        cpu->program_exception = shadow->program_exception;
        cpu->reserve_addr = shadow->reserve_addr;
        cpu->reserve_valid = shadow->reserve_valid;
    }
}

static u32 host_spr_read(CPUState* cpu, u16 spr, u32 cia) {
    if (spr == 22u || spr == 268u || spr == 269u)
        (void)bluewake_cycle_domain_observe(
            &g_cycle_domain, cpu, cpu->cycle_observation_suffix);
    if (spr == 22u) {
        if (g_guest_clock_trace && g_guest_clock_reports < 32u) {
            fprintf(stderr, "[decrementer] read cia=0x%08X value=0x%08X\n",
                    cia, g_guest_clock_decrementer);
            g_guest_clock_reports++;
        }
        return g_guest_clock_decrementer;
    }
    if (spr == 268u)
        return (u32)cpu->timebase;
    if (spr == 269u)
        return (u32)(cpu->timebase >> 32);
    return 0u;
}

static void host_spr_write(CPUState* cpu, u16 spr, u32 value, u32 cia) {
    if (spr == 284u || spr == 285u) {
        bluewake_cycle_domain_write_timebase(&g_cycle_domain, cpu, spr, value);
        return;
    }
    if (spr != 22u)
        return;
    (void)bluewake_cycle_domain_observe(
        &g_cycle_domain, cpu, cpu->cycle_observation_suffix);
    g_guest_clock_decrementer = value;
    g_guest_clock_decrementer_valid = true;
    g_guest_clock_decrementer_expired = false;
    g_guest_decrementer_pending = false;
    bluewake_cycle_domain_rebudget(&g_cycle_domain, cpu);
    if (g_guest_clock_trace && g_guest_clock_reports < 32u) {
        fprintf(stderr, "[decrementer] write cia=0x%08X value=0x%08X\n",
                cia, value);
        g_guest_clock_reports++;
    }
}

static void host_cache_control(CPUState* cpu, u8 operation, u32 ea, u32 cia) {
    (void)cpu;
    (void)cia;
    if (operation == PPC_CACHE_DCBST || operation == PPC_CACHE_DCBF)
        dol_guest_memory_dirty_mark(ea & ~31u, 32u);
}

static void heap_write_watch(u32 offset, u32 size, void* user) {
    CPUState* cpu = (CPUState*)user;
    g_heap_write_watch_total++;
    const u32 guest_start = 0x80000000u + offset;
    if (g_audio_object_watch && g_audio_object_watch_reports < 256u &&
        guest_start < 0x8076C050u + 68u &&
        guest_start + size > 0x8076C050u && size <= 8u) {
        u32 old_value = 0u;
        if (offset != (u32)-1 && offset + size <= cpu->ram_size) {
            if (size == 1u)
                old_value = cpu->ram[offset];
            else if (size == 2u)
                old_value = ((u32)cpu->ram[offset] << 8) |
                            (u32)cpu->ram[offset + 1u];
            else if (size >= 4u)
                old_value = ((u32)cpu->ram[offset] << 24) |
                            ((u32)cpu->ram[offset + 1u] << 16) |
                            ((u32)cpu->ram[offset + 2u] << 8) |
                            (u32)cpu->ram[offset + 3u];
        }
        fprintf(stderr,
                "[audio-object-write] pc=0x%08X guest=0x%08X size=%u "
                "old=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X "
                "lr=0x%08X\n",
                cpu->pc, guest_start, size, old_value, cpu->gpr[3],
                cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->lr);
        g_audio_object_watch_reports++;
    }
    const u32 audio_dac_start = 0x806AEEA0u;
    const u32 audio_dac_end = audio_dac_start + 0x8C0u;
    if (g_audio_object_watch && g_audio_object_watch_reports < 512u &&
        cpu->pc >= 0x80200000u &&
        guest_start < audio_dac_end && guest_start + size > audio_dac_start) {
        fprintf(stderr,
                "[audio-dac-write] pc=0x%08X guest=0x%08X size=%u "
                "old0=0x%08X old4=0x%08X r3=0x%08X r4=0x%08X "
                "r5=0x%08X r6=0x%08X lr=0x%08X\n",
                cpu->pc, guest_start, size, mem_read32(cpu, audio_dac_start),
                mem_read32(cpu, audio_dac_start + 4u), cpu->gpr[3], cpu->gpr[4],
                cpu->gpr[5], cpu->gpr[6], cpu->lr);
        g_audio_object_watch_reports++;
    }
    if (g_pad_status_trace && g_pad_si_status_reports < 64u &&
        guest_start < 0x803ED820u && guest_start + size > 0x803ED818u) {
        fprintf(stderr,
                "[pad-si-write] pc=0x%08X guest=0x%08X size=%u "
                "status=0x%08X,0x%08X origin_ptr=0x%08X "
                "origin=0x%08X,0x%08X "
                "r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X r7=0x%08X "
                "cr=0x%08X retrace=%llu\n",
                cpu->pc, guest_start, size,
                mem_read32(cpu, 0x803ED818u),
                mem_read32(cpu, 0x803ED81Cu),
                cpu->gpr[3],
                cpu->gpr[3] != 0u ? mem_read32(cpu, cpu->gpr[3]) : 0u,
                cpu->gpr[3] != 0u ? mem_read32(cpu, cpu->gpr[3] + 4u) : 0u,
                cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6],
                cpu->gpr[7], cpu->cr,
                (unsigned long long)g_host_retrace_count);
        g_pad_si_status_reports++;
    }
    if (g_pad_si_trace && g_pad_si_data_reports < 64u &&
        g_pad_si_data_watch != 0u &&
        guest_start < g_pad_si_data_watch + 8u &&
        guest_start + size > g_pad_si_data_watch) {
        fprintf(stderr,
                "[pad-si-data] pc=0x%08X guest=0x%08X size=%u "
                "watch=0x%08X words=0x%08X,0x%08X retrace=%llu\n",
                cpu->pc, guest_start, size, g_pad_si_data_watch,
                mem_read32(cpu, g_pad_si_data_watch),
                mem_read32(cpu, g_pad_si_data_watch + 4u),
                (unsigned long long)g_host_retrace_count);
        g_pad_si_data_reports++;
    }
    if (g_pad_lifecycle_trace && g_pad_lifecycle_reports < 128u &&
        guest_start < 0x803F7BACu && guest_start + size > 0x803F7B94u) {
        fprintf(stderr,
                "[pad-lifecycle-write] pc=0x%08X guest=0x%08X size=%u "
                "enabled_old=0x%08X reset_old=0x%08X waiting_old=0x%08X "
                "pending_old=0x%08X\n",
                cpu->pc, guest_start, size, mem_read32(cpu, 0x803F7B94u),
                mem_read32(cpu, 0x803F7B98u), mem_read32(cpu, 0x803F7BA0u),
                mem_read32(cpu, 0x803F7BA8u));
        g_pad_lifecycle_reports++;
    }
    const u32 payload_start = 0x81513844u;
    const u32 payload_end = payload_start + 0x10u;
    const bool payload_flow_target =
        (cpu->pc == 0x802BD4F0u && cpu->gpr[4] == 0x81512AC0u) ||
        (cpu->pc == 0x802BD6E8u && cpu->gpr[7] >= payload_start &&
         cpu->gpr[7] < payload_end) ||
        ((cpu->pc == 0x802BD61Cu || cpu->pc == 0x802BD730u) &&
         cpu->gpr[30] >= payload_start && cpu->gpr[30] < payload_end) ||
        ((cpu->pc == 0x802BD784u || cpu->pc == 0x802BD84Cu) &&
         cpu->gpr[30] <= payload_start && cpu->gpr[31] > payload_start);
    if (g_j2d_payload_flow_trace && g_j2d_payload_flow_reports < 256u &&
        (payload_flow_target || cpu->pc == 0x802BD324u ||
         cpu->pc == 0x802BD4F0u)) {
        fprintf(stderr,
                "[j2d-payload-flow] entry=0x%08X r3=0x%08X r4=0x%08X "
                "r5=0x%08X r6=0x%08X r7=0x%08X r8=0x%08X r9=0x%08X "
                "r29=0x%08X r30=0x%08X r31=0x%08X lr=0x%08X "
                "src0=0x%08X src4=0x%08X src8=0x%08X srcC=0x%08X "
                "dst0=0x%08X dvd_start=0x%08X dvd_length=0x%08X "
                "pRes0=0x%08X pResD84=0x%08X\n",
                cpu->pc, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5],
                cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], cpu->gpr[9],
                cpu->gpr[29], cpu->gpr[30], cpu->gpr[31], cpu->lr,
                mem_read32(cpu, cpu->gpr[3]), mem_read32(cpu, cpu->gpr[3] + 4u),
                mem_read32(cpu, cpu->gpr[3] + 8u), mem_read32(cpu, cpu->gpr[3] + 12u),
                mem_read32(cpu, cpu->gpr[4]), mem_read32(cpu, cpu->gpr[3] + 0x8Cu),
                mem_read32(cpu, cpu->gpr[3] + 0x90u),
                mem_read32(cpu, 0x81512AC0u), mem_read32(cpu, payload_start));
        g_j2d_payload_flow_reports++;
    }
    if (g_j2d_payload_watch && g_j2d_payload_reports < 256u &&
        guest_start < payload_end && guest_start + size > payload_start) {
        fprintf(stderr,
                "[j2d-payload-write] pc=0x%08X guest=0x%08X size=%u "
                "old0=0x%08X old4=0x%08X source=0x%08X source_byte=0x%02X "
                "r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X r29=0x%08X "
                "r30=0x%08X r31=0x%08X lr=0x%08X\n",
                cpu->pc, guest_start, size, mem_read32(cpu, payload_start),
                mem_read32(cpu, payload_start + 4u), cpu->gpr[9],
                mem_read8(cpu, cpu->gpr[9]), cpu->gpr[3], cpu->gpr[4],
                cpu->gpr[5], cpu->gpr[6], cpu->gpr[29], cpu->gpr[30],
                cpu->gpr[31], cpu->lr);
        g_j2d_payload_reports++;
    }
    if (g_j2d_object_watch && g_j2d_object_watch_reports < 256u &&
        guest_start < 0x81700000u && guest_start + size > 0x81640000u &&
        size <= 16u && cpu->pc >= 0x802CF000u && cpu->pc < 0x802D3000u) {
        fprintf(stderr,
                "[j2d-write] pc=0x%08X guest=0x%08X size=%u "
                "old0=0x%08X old4=0x%08X\n",
                cpu->pc, guest_start, size,
                mem_read32(cpu, guest_start),
                mem_read32(cpu, guest_start + 4u));
        g_j2d_object_watch_reports++;
    }
    if (g_title_profile_watch && g_title_profile_watch_reports < 64u &&
        guest_start < 0x81E02140u && guest_start + size > 0x81E02130u) {
        fprintf(stderr,
                "[title-profile-write] pc=0x%08X guest=0x%08X size=%u "
                "old0=0x%08X oldc=0x%08X\n",
                cpu->pc, guest_start, size,
                mem_read32(cpu, 0x81E02130u), mem_read32(cpu, 0x81E0213Cu));
        g_title_profile_watch_reports++;
    }
    if (guest_start < 0x815581A0u + 0x214u &&
        guest_start + size > 0x815581A0u) {
        fprintf(stderr,
                "[rel-bss-write] pc=0x%08X addr=0x%08X size=%u "
                "head=0x%08X node0=0x%08X node1=0x%08X\n",
                cpu->pc, guest_start, size, mem_read32(cpu, 0x815581A0u),
                mem_read32(cpu, 0x815581F4u), mem_read32(cpu, 0x8155820Cu));
    }
    if (g_heap_write_watch_late &&
        g_heap_write_watch_dispatch_pc == 0x80304DF0u &&
        g_external_dispatch_store_reports < 128u &&
        offset != (u32)-1 && offset < cpu->ram_size && size != 0u) {
        u32 old_value = 0u;
        if (size == 1u)
            old_value = cpu->ram[offset];
        else if (size == 2u && offset + 1u < cpu->ram_size)
            old_value = ((u32)cpu->ram[offset] << 8) |
                        (u32)cpu->ram[offset + 1u];
        else if (offset + 3u < cpu->ram_size)
            old_value = ((u32)cpu->ram[offset] << 24) |
                        ((u32)cpu->ram[offset + 1u] << 16) |
                        ((u32)cpu->ram[offset + 2u] << 8) |
                        (u32)cpu->ram[offset + 3u];
        fprintf(stderr,
                "[external-store] guest=0x%08X size=%u old=0x%08X\n",
                0x80000000u + offset, size, old_value);
        g_external_dispatch_store_reports++;
    }
    const u32 tail_start = 0x00AD2140u;
    const u32 tail_end = 0x00AD2180u;
    const u32 malformed_start = 0x00ADDFDCu;
    const u32 malformed_end = 0x00ADE020u;
    const u32 control_start = 0x0041D0FCu;
    const u32 control_end = 0x0041D108u;
    const bool in_tail = offset >= tail_start + 0x04u &&
                         offset < tail_start + 0x10u;
    const bool in_malformed = offset >= malformed_start && offset < malformed_end;
    const bool in_control = offset >= control_start && offset < control_end;
    const bool in_heap_window = offset >= tail_start && offset < malformed_end;
    const bool in_external_continuation =
        g_heap_write_watch_dispatch_pc == 0x80304DF0u && in_heap_window;
    const u32 watch_end = in_external_continuation
                              ? malformed_end
                              : (in_tail ? tail_end
                                         : (in_malformed ? malformed_end
                                                          : control_end));
    if ((in_tail || in_control || in_malformed || in_external_continuation) &&
        !g_heap_write_watch_late)
        return;
    if (offset == (u32)-1 ||
        (!in_tail && !in_malformed && !in_control &&
         !in_external_continuation) ||
        size == 0u || (in_external_continuation ? size > 128u : size > 8u) ||
        offset + size > watch_end ||
        (in_tail ? g_heap_write_watch_reports >= 512u
                 : (in_external_continuation
                        ? g_heap_write_watch_external_reports >= 128u
                        : (in_malformed ? g_heap_write_watch_malformed_reports >= 256u
                                        : g_heap_write_watch_control_reports >= 256u))))
        return;

    u32 old_value = 0u;
    if (size == 1u)
        old_value = cpu->ram[offset];
    else if (size == 2u)
        old_value = ((u32)cpu->ram[offset] << 8) |
                    (u32)cpu->ram[offset + 1u];
    else
        old_value = ((u32)cpu->ram[offset] << 24) |
                    ((u32)cpu->ram[offset + 1u] << 16) |
                    ((u32)cpu->ram[offset + 2u] << 8) |
                    (u32)cpu->ram[offset + 3u];
    fprintf(stderr,
            "[heap-write] dispatch=0x%08X guest=0x%08X size=%u old=0x%08X\n",
            g_heap_write_watch_dispatch_pc, 0x80000000u + offset, size,
            old_value);
    if (in_external_continuation)
        g_heap_write_watch_external_reports++;
    else if (in_control)
        g_heap_write_watch_control_reports++;
    else if (in_malformed)
        g_heap_write_watch_malformed_reports++;
    else
        g_heap_write_watch_reports++;
}

static u32 host_raw_ram_read32(const CPUState* cpu, u32 guest_address) {
    const u32 offset = guest_address - 0x80000000u;
    if (offset > cpu->ram_size - 4u)
        return 0u;
    return ((u32)cpu->ram[offset] << 24) |
           ((u32)cpu->ram[offset + 1u] << 16) |
           ((u32)cpu->ram[offset + 2u] << 8) |
           (u32)cpu->ram[offset + 3u];
}

#define DVD_CB_STATE 0x0Cu
#define DVD_CB_CURRXFER 0x1Cu
#define DVD_CB_XFERRED 0x20u
#define DVD_FI_STARTADDR 0x30u
#define DVD_FI_LENGTH 0x34u
#define DVD_FI_CALLBACK 0x38u
#define DVD_MIN_TRANSFER_SIZE 0x20u

static void guest_read_cstr(CPUState* cpu, u32 address, char* out,
                            unsigned capacity) {
    if (capacity == 0u)
        return;
    unsigned length = 0;
    while (length + 1u < capacity && address != 0u) {
        const u8 ch = mem_read8(cpu, address + length);
        out[length++] = (char)ch;
        if (ch == 0u)
            break;
    }
    out[capacity - 1u] = '\0';
    if (length == 0u || out[length - 1u] != '\0')
        out[capacity - 1u] = '\0';
}

static bool host_dvd_fill_file_info(CPUState* cpu, s32 entry, u32 file_info) {
    u32 start = 0;
    u32 length = 0;
    if (file_info == 0u || !dvd_entry_info(entry, &start, &length))
        return false;
    mem_write32(cpu, file_info + DVD_FI_STARTADDR, start);
    mem_write32(cpu, file_info + DVD_FI_LENGTH, length);
    mem_write32(cpu, file_info + DVD_FI_CALLBACK, 0u);
    mem_write32(cpu, file_info + DVD_CB_STATE, 0u);
    return true;
}

static void host_remove_rel_aliases(u32 raw_start, u32 raw_end) {
    u32 write = 0u;
    for (u32 read = 0u; read < g_rel_alias_count; ++read) {
        const BlueWakeRelAlias alias = g_rel_aliases[read];
        if (alias.raw_start >= raw_start && alias.raw_start < raw_end)
            continue;
        g_rel_aliases[write++] = alias;
    }
    g_rel_alias_count = write;
    g_rel_alias_raw_min = ~0u;
    g_rel_alias_raw_max = 0u;
    for (u32 i = 0u; i < g_rel_alias_count; ++i) {
        if (g_rel_aliases[i].raw_start < g_rel_alias_raw_min)
            g_rel_alias_raw_min = g_rel_aliases[i].raw_start;
        if (g_rel_aliases[i].raw_end > g_rel_alias_raw_max)
            g_rel_alias_raw_max = g_rel_aliases[i].raw_end;
    }
}

static bool host_rel_slot_is_live(CPUState* cpu, BlueWakeRelSlot slot) {
    return slot.owner != 0u &&
           mem_read32(cpu, slot.owner + 0x10u) == slot.address;
}

static bool host_find_rel_scratch(CPUState* cpu, u32 size, u32* address) {
    const size_t capacity = (size_t)g_rel_data_count + BLUEWAKE_MAX_REL_SLOTS;
    BlueWakeScratchRange* occupied =
        (BlueWakeScratchRange*)malloc(capacity * sizeof(*occupied));
    if (occupied == NULL)
        return false;

    size_t count = 0u;
    for (u32 i = 0u; i < g_rel_data_count; ++i) {
        const BlueWakeRelData* image = &g_rel_data[i];
        if (image->linked_start == 0u || image->size == 0u)
            continue;
        occupied[count++] = (BlueWakeScratchRange){image->linked_start,
                                                   image->size};
    }
    for (u32 i = 0u; i < BLUEWAKE_MAX_REL_SLOTS; ++i) {
        const BlueWakeRelSlot slot = g_rel_slots[i];
        if (!host_rel_slot_is_live(cpu, slot))
            continue;
        occupied[count++] =
            (BlueWakeScratchRange){slot.address, slot.capacity};
    }
    const bool found = bluewake_rel_scratch_first_fit(
        BLUEWAKE_DYNAMIC_SCRATCH_BASE, BLUEWAKE_DYNAMIC_SCRATCH_LIMIT, size,
        occupied, count, address);
    free(occupied);
    return found;
}

/* Opens rels_dir/<name>.rel. The game names some modules with capitals the
   disc's file names do not have (it asks for d_a_obj_Ygush00; the file is
   d_a_obj_ygush00.rel). macOS volumes ignore case, so the exact path works
   there; iOS's does not, and the module was reported unavailable and the game
   jumped into unloaded code (Hyrule, 2026-09-26). Fall back to a
   case-insensitive match in the directory. */
static FILE* host_open_rel(const char* rels_dir, const char* name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.rel", rels_dir, name);
    FILE* file = fopen(path, "rb");
    if (file != NULL)
        return file;
    char wanted[256];
    snprintf(wanted, sizeof(wanted), "%s.rel", name);
    DIR* dir = opendir(rels_dir);
    if (dir == NULL)
        return NULL;
    for (struct dirent* entry; (entry = readdir(dir)) != NULL;) {
        if (strcasecmp(entry->d_name, wanted) == 0) {
            snprintf(path, sizeof(path), "%s/%s", rels_dir, entry->d_name);
            file = fopen(path, "rb");
            break;
        }
    }
    closedir(dir);
    return file;
}

static bool host_materialize_rel(CPUState* cpu, const char* name, u32 owner,
                                 u32* guest_ptr, u32* byte_count) {
    const char* rels_dir = getenv("BLUEWAKE_RELS_DIR");
    if (rels_dir == NULL || rels_dir[0] == '\0')
        rels_dir = "generated/full/rels";
    FILE* file = host_open_rel(rels_dir, name);
    if (file == NULL)
        return false;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    const long size = ftell(file);
    if (size <= 0 || size > 0x00100000L) {
        fclose(file);
        return false;
    }
    rewind(file);
    const u32 aligned = ((u32)size + 31u) & ~31u;
    u8* bytes = (u8*)malloc((size_t)size);
    if (bytes == NULL || fread(bytes, 1, (size_t)size, file) != (size_t)size) {
        free(bytes);
        fclose(file);
        return false;
    }
    fclose(file);
    u32 address = 0u;
    u32 capacity = aligned;
    int slot_index = -1;
    for (u32 i = 0u; i < BLUEWAKE_MAX_REL_SLOTS; ++i) {
        if (g_rel_slots[i].owner == owner &&
            g_rel_slots[i].capacity >= aligned) {
            slot_index = (int)i;
            address = g_rel_slots[i].address;
            capacity = g_rel_slots[i].capacity;
            break;
        }
    }
    if (slot_index < 0) {
        for (u32 i = 0u; i < BLUEWAKE_MAX_REL_SLOTS; ++i) {
            const BlueWakeRelSlot slot = g_rel_slots[i];
            if (host_rel_slot_is_live(cpu, slot))
                continue;
            if (slot.capacity < aligned)
                continue;
            slot_index = (int)i;
            address = slot.address;
            capacity = slot.capacity;
            break;
        }
    }
    if (slot_index < 0) {
        if (!host_find_rel_scratch(cpu, aligned, &address)) {
            free(bytes);
            return false;
        }
        for (u32 i = 0u; i < BLUEWAKE_MAX_REL_SLOTS; ++i) {
            if (!host_rel_slot_is_live(cpu, g_rel_slots[i])) {
                slot_index = (int)i;
                break;
            }
        }
        if (slot_index < 0) {
            free(bytes);
            return false;
        }
    }
    for (u32 i = 0u; i < BLUEWAKE_MAX_REL_SLOTS; ++i) {
        const BlueWakeRelSlot slot = g_rel_slots[i];
        const u64 slot_end = (u64)slot.address + slot.capacity;
        const u64 allocation_end = (u64)address + capacity;
        if ((int)i != slot_index && !host_rel_slot_is_live(cpu, slot) &&
            slot.owner != 0u && (u64)address < slot_end &&
            allocation_end > slot.address) {
            g_rel_slots[i] = (BlueWakeRelSlot){0};
        }
    }
    host_remove_rel_aliases(address, address + capacity);
    g_rel_slots[slot_index] = (BlueWakeRelSlot){owner, address, capacity};
    for (u32 offset = 0; offset < (u32)size; offset++)
        mem_write8(cpu, address + offset, bytes[offset]);
    free(bytes);
    *guest_ptr = address;
    *byte_count = (u32)size;
    return true;
}

static void host_zero_rel_bss(CPUState* cpu, const BlueWakeRelData* rel_data,
                              u32 rel_data_count, u32 module_id) {
    for (u32 i = 0; i < rel_data_count; ++i) {
        const BlueWakeRelData* image = &rel_data[i];
        if (image->module_id != module_id || image->bytes ||
            image->size == 0u || image->linked_start == 0u)
            continue;
        for (u32 offset = 0; offset < image->size; ++offset)
            mem_write8(cpu, image->linked_start + offset, 0u);
    }
}

static u32 host_rel_section_linked_start(const StaticRecompModuleDesc* mod,
                                         u32 module_id, u32 section_index) {
    if (mod == NULL)
        return 0u;
    for (u32 module = 0; module < mod->num_rel_modules; ++module) {
        const StaticRecompRelModule* rel = &mod->rel_modules[module];
        if (rel->module_id != module_id)
            continue;
        for (u32 section = 0; section < rel->num_sections; ++section) {
            const StaticRecompRelSection* linked = &rel->sections[section];
            if (linked->section_index == section_index)
                return linked->linked_start;
        }
        break;
    }
    return 0u;
}

static void host_register_rel_alias(CPUState* cpu,
                                    const StaticRecompModuleDesc* mod,
                                    u32 module) {
    const u32 module_id = mem_read32(cpu, module);
    // Module 1's scratch image overlaps the linked address space used by
    // later RELs, and its prolog/entry already has a dedicated compatibility
    // path below. Never let its raw range shadow linked executable code.
    if (module_id == 1u)
        return;
    const u32 section_count = mem_read32(cpu, module + 0x0Cu);
    const u32 section_info = module + mem_read32(cpu, module + 0x10u);
    const StaticRecompRelModule* rel = NULL;
    for (u32 i = 0; mod && i < mod->num_rel_modules; ++i) {
        if (mod->rel_modules[i].module_id == module_id) {
            rel = &mod->rel_modules[i];
            break;
        }
    }
    if (!rel || section_info == 0u || section_count == 0u)
        return;
    if (module_id == 336u && !g_module336_bss_alias_installed) {
        for (u32 i = 0; i < rel->num_sections; ++i) {
            const StaticRecompRelSection* section = &rel->sections[i];
            if (section->section_index != 6u || section->linked_start == 0u ||
                section->size == 0u)
                continue;
            if (!host_add_shared_guest_alias(section->linked_start,
                                             section->size, NULL)) {
                fprintf(stderr,
                        "[rel] failed module-336 BSS alias start=0x%08X size=0x%08X\n",
                        section->linked_start, section->size);
                return;
            }
            g_module336_bss_alias_installed = true;
            break;
        }
    }
    for (u32 section = 0; section < section_count; ++section) {
        const u32 raw = mem_read32(cpu, section_info + section * 8u);
        const u32 size = mem_read32(cpu, section_info + section * 8u + 4u);
        if ((raw & 1u) == 0u || size == 0u)
            continue;
        for (u32 i = 0; i < rel->num_sections; ++i) {
            const StaticRecompRelSection* linked = &rel->sections[i];
            if (linked->section_index != section || linked->linked_start == 0u)
                continue;
            if (g_rel_alias_count >= BLUEWAKE_MAX_REL_ALIASES)
                return;
            const BlueWakeRelAlias alias = (BlueWakeRelAlias){
                module + (raw & ~1u), module + (raw & ~1u) + size,
                linked->linked_start, linked->size};
            g_rel_aliases[g_rel_alias_count++] = alias;
            if (alias.raw_start < g_rel_alias_raw_min)
                g_rel_alias_raw_min = alias.raw_start;
            if (alias.raw_end > g_rel_alias_raw_max)
                g_rel_alias_raw_max = alias.raw_end;
            fprintf(stderr,
                    "[rel] executable alias module=%u raw=0x%08X linked=0x%08X size=%u\n",
                    module_id, module + (raw & ~1u), linked->linked_start, size);
            return;
        }
    }
}

static bool host_alias_rel_pc(CPUState* cpu) {
    if (cpu->pc < g_rel_alias_raw_min || cpu->pc >= g_rel_alias_raw_max)
        return false;
    for (u32 i = 0; i < g_rel_alias_count; ++i) {
        const BlueWakeRelAlias* alias = &g_rel_aliases[i];
        if (cpu->pc >= alias->raw_start && cpu->pc < alias->raw_end) {
            cpu->pc = alias->linked_start + (cpu->pc - alias->raw_start);
            return true;
        }
    }
    return false;
}

static u32 host_canonical_linked_pc(u32 pc) {
    return pc & ~0x40000000u;
}

static void host_alias_rel_lifecycle_table(CPUState* cpu) {
    const u32 lifecycle_pc = host_canonical_linked_pc(cpu->pc);
    if (lifecycle_pc != 0x80241178u && lifecycle_pc != 0x802411F8u)
        return;
    for (u32 i = 0; i < g_rel_alias_count; ++i) {
        const BlueWakeRelAlias* alias = &g_rel_aliases[i];
        const u32 linked_table = alias->linked_start + alias->text_size;
        if (cpu->gpr[3] >= linked_table &&
            cpu->gpr[3] < linked_table + 0x1000u) {
            cpu->gpr[3] = alias->raw_start + (cpu->gpr[3] - alias->linked_start);
            return;
        }
    }
}

static bool host_activate_rel_profile_list(CPUState* cpu,
                                           const BlueWakeRelData* rel_data,
                                           u32 rel_data_count, u32 module_id) {
    if (module_id != 1u || !rel_data)
        return false;
    for (u32 i = 0; i < rel_data_count; ++i) {
        const BlueWakeRelData* image = &rel_data[i];
        if (image->module_id != module_id || image->section_index != 4u ||
            image->linked_start == 0u || image->size == 0u)
            continue;
        // f_pc_profile_lst::ModuleProlog publishes this module data array as
        // g_fpcPf_ProfileList_p. The generated prolog currently cannot cross
        // its REL constructor dispatch, so preserve that exact link contract
        // at the authentic do_link completion point.
        mem_write32(cpu, 0x803F6A68u, image->linked_start);
        fprintf(stderr,
                "[rel] authentic-link-contract module=%u profile_ptr=0x%08X "
                "data_size=0x%08X\n",
                module_id, image->linked_start, image->size);
        return true;
    }
    return false;
}

static DolDiCommandResult host_di_command(void* user, DolDiCommand* command) {
    (void)user;
    const u32 opcode = command->command[0] & 0xFF000000u;
    const u32 guest_address = 0x80000000u | command->dma_address;

    if (opcode == 0x12000000u) {
        // DVDLowInquiry returns revision/device, release date, and firmware
        // version in the same 32-byte structure used by the retail SDK.
        if (!command->dma || command->dma_length < 32u)
            return DOL_DI_COMMAND_ERROR;
        for (u32 offset = 0; offset < 32u; offset += 4u)
            mem_write32(command->cpu, guest_address + offset, 0u);
        mem_write32(command->cpu, guest_address + 0u, 0x00000002u);
        mem_write32(command->cpu, guest_address + 4u, 0x20060526u);
        mem_write32(command->cpu, guest_address + 8u, 0x41000000u);
        fprintf(stderr, "[dvd] DI inquiry guest=0x%08X length=%u\n",
                guest_address, command->dma_length);
        return DOL_DI_COMMAND_COMPLETE;
    }

    if (opcode == 0xAB000000u) {
        // DVDLowSeek has no DMA transfer; its completion still raises TCINT.
        fprintf(stderr, "[dvd] DI seek disc=0x%08X\n",
                command->command[1] << 2);
        return DOL_DI_COMMAND_COMPLETE;
    }

    if (!command->dma || command->dma_length == 0u ||
        !dvd_image_ready())
        return DOL_DI_COMMAND_ERROR;

    if (opcode == 0xA8000000u) {
        const u32 offset = command->command[1] << 2;
        dvd_read_to_guest(command->cpu, guest_address, offset,
                          command->dma_length);
        if (g_di_read_reports < 8u) {
            fprintf(stderr,
                    "[dvd] DI read opcode=0x%08X guest=0x%08X "
                    "disc=0x%08X length=%u\n",
                    opcode, guest_address, offset, command->dma_length);
            g_di_read_reports++;
        }
        return DOL_DI_COMMAND_COMPLETE;
    }

    fprintf(stderr, "[dvd] unsupported DI opcode=0x%08X\n", opcode);
    return DOL_DI_COMMAND_ERROR;
}

#define PPC_EXC_EXTERNAL 0x00000040u
#define PPC_VECTOR_EXTERNAL 0x00500u

static u64 host_mmio_read(CPUState* ctx, u32 address, u8 size);

static void deliver_external_interrupt(CPUState* cpu) {
    const u32 pi_cause = dol_interrupts_pi_cause(&g_interrupts);
    if (g_audio_object_watch &&
        dol_audio_dma_dsp_interrupt_pending(&g_audio_dma) &&
        g_audio_dma_irq_reports < 16u) {
        fprintf(stderr,
                "[audio-dma-irq] pc=0x%08X cause=0x%08X mask=0x%08X "
                "status=0x%04X ai_handler=0x%08X dsp_handler=0x%08X "
                "ai_callback=0x%08X os_mask=0x%08X os_mask_local=0x%08X "
                "current=0x%08X context=0x%08X\n",
                cpu->pc, pi_cause, dol_interrupts_pi_mask(&g_interrupts),
                (u16)host_mmio_read(cpu, 0xCC00500Au, 2u),
                mem_read32(cpu, 0x80003040u + 5u * 4u),
                mem_read32(cpu, 0x80003040u + 7u * 4u),
                mem_read32(cpu, 0x803F7BBCu),
                mem_read32(cpu, 0x800000C4u), mem_read32(cpu, 0x800000C8u),
                mem_read32(cpu, 0x800000E4u), mem_read32(cpu, 0x800000D4u));
        g_audio_dma_irq_reports++;
    }
    if (g_audio_object_watch && g_dsp_mail_from_pending &&
        g_dsp_mail_from == 0xDCD10000u && g_audio_dsp_boot_irq_reports < 32u) {
        fprintf(stderr,
                "[audio-dsp-boot-irq] pc=0x%08X current=0x%08X context=0x%08X "
                "cause=0x%08X mask=0x%08X control=0x%04X status=0x%04X "
                "mail=0x%08X task_pending=%u task_armed=%u prior=0x%08X "
                "curr=0x%08X srr0=0x%08X srr1=0x%08X msr=0x%08X\n",
                cpu->pc, mem_read32(cpu, 0x800000E4u),
                mem_read32(cpu, 0x800000D4u), pi_cause,
                dol_interrupts_pi_mask(&g_interrupts), g_dsp_control,
                (u16)host_mmio_read(cpu, 0xCC00500Au, 2u), g_dsp_mail_from,
                g_dsp_task_request_pending ? 1u : 0u,
                g_dsp_task_request_armed ? 1u : 0u,
                mem_read32(cpu, 0x803F7570u), mem_read32(cpu, 0x803F7C54u),
                cpu->srr0, cpu->srr1, cpu->msr);
        g_audio_dsp_boot_irq_reports++;
    }
    if (g_audio_object_watch && (pi_cause & DOL_PI_CAUSE_DSP) != 0u &&
        g_dsp_irq_reports < 8u) {
        const u32 handler_slot = 0x80003040u + 7u * 4u;
        fprintf(stderr,
                "[audio-dsp-irq] cause=0x%08X mask=0x%08X control=0x%04X "
                "aram_slot=0x%08X aram_handler=0x%08X dsp_slot=0x%08X "
                "dsp_handler=0x%08X pc=0x%08X current=0x%08X\n",
                pi_cause, dol_interrupts_pi_mask(&g_interrupts), g_dsp_control,
                0x80003040u + 6u * 4u, mem_read32(cpu, 0x80003040u + 6u * 4u),
                handler_slot, mem_read32(cpu, handler_slot), cpu->pc,
                mem_read32(cpu, 0x800000E4u));
        g_dsp_irq_reports++;
    }
    if (g_audio_object_watch &&
        (cpu->pc == 0x80317464u || cpu->pc == 0x803174C8u ||
         cpu->pc == 0x8028ECA0u ||
         cpu->pc == 0x8031907Cu || cpu->pc == 0x8031908Cu) &&
        g_dsp_irq_route_reports < 24u) {
        fprintf(stderr,
                "[audio-dsp-route] pc=0x%08X cause=0x%08X control=0x%04X "
                "aram_pending=%u aid_pending=%u mail_pending=%u\n",
                cpu->pc, pi_cause, g_dsp_control,
                bluewake_aram_dma_interrupt_pending(&g_aram_dma) ? 1u : 0u,
                dol_audio_dma_dsp_interrupt_pending(&g_audio_dma) ? 1u : 0u,
                g_dsp_mail_from_pending ? 1u : 0u);
        g_dsp_irq_route_reports++;
    }
    const u32 context = mem_read32(cpu, 0x800000D4u);
    if (context < 0x80000000u || context >= 0x80000000u + cpu->ram_size)
        return;

    bluewake_delivery_digest_record_external(
        &g_delivery_digest, g_cycle_domain.absolute_cycles, pi_cause, cpu->pc,
        context, (pi_cause & DOL_PI_CAUSE_DSP) != 0u, g_play_scene_reported);
    if (g_delivery_trace_lo != 0u &&
        g_delivery_digest.external_count >= g_delivery_trace_lo &&
        g_delivery_digest.external_count <= g_delivery_trace_hi) {
        fprintf(stderr,
                "[delivery-trace] ordinal=%llu cycle=%llu cause=0x%08X "
                "pc=0x%08X context=0x%08X\n",
                (unsigned long long)g_delivery_digest.external_count,
                (unsigned long long)g_cycle_domain.absolute_cycles, pi_cause,
                cpu->pc, context);
    }
    if (g_delivery_play_trace != 0u && g_play_scene_reported &&
        g_delivery_digest.play_count <= g_delivery_play_trace) {
        fprintf(stderr,
                "[delivery-play-trace] ordinal=%llu cycle=%llu cause=0x%08X "
                "pc=0x%08X context=0x%08X\n",
                (unsigned long long)g_delivery_digest.play_count,
                (unsigned long long)g_cycle_domain.absolute_cycles, pi_cause,
                cpu->pc, context);
    }

    host_save_context_shadow(cpu, context);
    if (g_audio_object_watch && g_audio_context_interrupt_reports < 16u &&
        context == 0x803E9260u && cpu->pc == 0x80307EACu) {
        fprintf(stderr,
                "[audio-context-save] pc=0x%08X cpu_r3=0x%08X "
                "context_r3_before=0x%08X state_before=0x%04X "
                "current_thread=0x%08X current_context=0x%08X\n",
                cpu->pc, cpu->gpr[3], mem_read32(cpu, context + 0x0Cu),
                mem_read16(cpu, context + 0x1A2u),
                mem_read32(cpu, 0x800000E4u), mem_read32(cpu, 0x800000D4u));
        g_audio_context_interrupt_reports++;
    }

    if (g_runqueue_trace &&
        g_aram_external_save_reports < 24u &&
        context == 0x804211E0u && cpu->pc == 0x80307EACu) {
        fprintf(stderr,
                "[sched] aram-external-save #%u host_pc=0x%08X "
                "old_saved_pc=0x%08X old_saved_srr1=0x%08X "
                "worker_state=%u worker_queue=0x%08X run_bits=0x%08X "
                "current_context=0x%08X cpu_srr0=0x%08X cpu_srr1=0x%08X "
                "cpu_msr=0x%08X context_state=0x%04X\n",
                g_aram_external_save_reports + 1u, cpu->pc,
                mem_read32(cpu, context + 0x198u),
                mem_read32(cpu, context + 0x19Cu),
                mem_read16(cpu, context + 0x2C8u),
                mem_read32(cpu, context + 0x2DCu),
                mem_read32(cpu, 0x803F7A30u),
                mem_read32(cpu, 0x800000D4u), cpu->srr0, cpu->srr1,
                cpu->msr, mem_read16(cpu, context + 0x1A2u));
        g_aram_external_save_reports++;
    }

    if (cpu->pc == 0x80307EACu && g_idle_context_overwrite_reports < 12u) {
        fprintf(stderr,
                "[sched] idle-selector interrupt context=0x%08X "
                "current_thread=0x%08X main_srr0=0x%08X idle_srr0=0x%08X "
                "run_bits=0x%08X\n",
                context, mem_read32(cpu, 0x800000E4u),
                mem_read32(cpu, 0x803A2960u + 0x198u),
                mem_read32(cpu, 0x803F09E8u + 0x198u),
                mem_read32(cpu, 0x803F7A30u));
        g_idle_context_overwrite_reports++;
    }

    bluewake_scheduler_save_exception_context(cpu, context);

    cpu->gpr[3] = 4u; // __OS_EXCEPTION_EXTERNAL_INTERRUPT
    cpu->gpr[4] = context;
    cpu->srr0 = cpu->pc;
    cpu->srr1 = cpu->msr;
    cpu->msr &= ~PPC_MSR_EE;
    // ExternalInterruptHandler's retail epilogue allocates this eight-byte
    // linkage area before branching into __OSDispatchInterrupt.
    const u32 interrupted_sp = cpu->gpr[1];
    cpu->gpr[1] = interrupted_sp - 8u;
    mem_write32(cpu, cpu->gpr[1], interrupted_sp);
    cpu->pc = 0x80304AE0u; // __OSDispatchInterrupt
    if (g_interrupt_delivery_reports < 16u) {
        fprintf(stderr,
                "[interrupt] delivered external interrupt context=0x%08X\n",
                context);
        g_interrupt_delivery_reports++;
    }
}

// Record one host-delivered interrupt against the scheduler's own safety
// contract. Called immediately before each delivery; `line` names the source.
static void host_delivery_safety_observe(const CPUState* cpu, const char* line) {
    if (!g_delivery_safety_census_enabled)
        return;
    g_delivery_safety_calls++;
    if (g_delivery_timeline_reports < 400u) {
        fprintf(stderr,
                "[delivery-timeline] #%u %s cycle=%llu pc=0x%08X msr=0x%08X "
                "exception=0x%08X\n",
                g_delivery_timeline_reports + 1u, line,
                (unsigned long long)g_cycle_domain.absolute_cycles, cpu->pc,
                cpu->msr, cpu->exception);
        g_delivery_timeline_reports++;
    }
    if (bluewake_scheduler_interrupt_safe(cpu->pc))
        return;
    g_delivery_safety_unsafe++;
    if (g_delivery_safety_reports < 16u) {
        fprintf(stderr,
                "[delivery-safety] unsafe %s pc=0x%08X msr=0x%08X "
                "exception=0x%08X block=%llu\n",
                line, cpu->pc, cpu->msr, cpu->exception,
                (unsigned long long)g_current_host_block);
        g_delivery_safety_reports++;
    }
}

static void deliver_decrementer_exception(CPUState* cpu) {
    const u32 context = mem_read32(cpu, 0x800000D4u);
    if (context < 0x80000000u || context >= 0x80000000u + cpu->ram_size)
        return;

    host_save_context_shadow(cpu, context);
    // Mirror OSExceptionVector's context writes, then enter the translated
    // retail DecrementerExceptionHandler prologue at its real DOL address.
    bluewake_scheduler_save_exception_context(cpu, context);

    cpu->gpr[3] = 8u; // EXCEPTION_DECREMENTER
    cpu->gpr[4] = context;
    cpu->srr0 = cpu->pc;
    cpu->srr1 = cpu->msr;
    cpu->msr &= ~PPC_MSR_EE;
    cpu->pc = 0x803025D4u; // DecrementerExceptionHandler
    g_guest_decrementer_pending = false;
    if (g_runqueue_trace &&
        g_decrementer_context_reports < 24u) {
        fprintf(stderr,
                "[decrementer] delivered #%u context=0x%08X interrupted=0x%08X "
                "current_thread=0x%08X current_context=0x%08X saved_pc=0x%08X "
                "saved_msr=0x%08X main_state=%u main_saved_pc=0x%08X "
                "run_bits=0x%08X\n",
                g_decrementer_context_reports + 1u, context, cpu->srr0,
                mem_read32(cpu, 0x800000E4u), mem_read32(cpu, 0x800000D4u),
                mem_read32(cpu, context + 0x198u),
                mem_read32(cpu, context + 0x19Cu),
                mem_read16(cpu, 0x803A2960u + 0x2C8u),
                mem_read32(cpu, 0x803A2960u + 0x198u),
                mem_read32(cpu, 0x803F7A30u));
        g_decrementer_context_reports++;
    }
}

// The Gekko core runs at 486 MHz while the GameCube timebase runs at 40.5 MHz:
// one timebase tick represents 12 guest CPU cycles. Keep the fractional cycle
// remainder so block boundaries cannot skew time or decrementer delivery.
// This promoted service owns one deterministic guest clock. Generated blocks
// provide every Gekko cycle charge; host/HLE turns consume no guest time of
// their own. CPUState.timebase and SPR-22 consume the same converted ticks.
static void guest_clock_advance(CPUState* cpu, u64 elapsed_cycles) {
    if (g_delivery_safety_census_enabled) {
        g_clock_advance_calls++;
        g_clock_advance_cycles += elapsed_cycles;
    }
    if (g_guest_state_trace_enabled &&
        g_cycle_domain.absolute_cycles >= g_guest_state_trace_next &&
        g_guest_state_trace_reports < 600u) {
        fprintf(stderr,
                "[guest-state] cycle=%llu pc=0x%08X msr=0x%08X srr0=0x%08X "
                "thread=0x%08X context=0x%08X tb=%llu dec=0x%08X "
                "run=0x%08X main_state=%d\n",
                (unsigned long long)g_cycle_domain.absolute_cycles, cpu->pc,
                cpu->msr, cpu->srr0,
                mem_read32(cpu, 0x800000E4u), mem_read32(cpu, 0x800000D4u),
                (unsigned long long)cpu->timebase, g_guest_clock_decrementer,
                mem_read32(cpu, 0x803F7A30u),
                (s32)mem_read16(cpu, 0x803A2960u + 0x2C8u));
        g_guest_state_trace_reports++;
        // Fixed grid, not a running offset: the two configurations must be
        // sampled at the same guest cycles or the comparison measures the
        // sampling drift instead of the guest.
        g_guest_state_trace_next =
            ((g_cycle_domain.absolute_cycles / 100000ull) + 1ull) * 100000ull;
    }
    const u64 cycle_total = elapsed_cycles + g_guest_clock_cycle_remainder;
    const u64 elapsed_timebase = cycle_total / GUEST_CYCLES_PER_TIMEBASE_TICK;
    g_guest_clock_cycle_remainder =
        (u32)(cycle_total % GUEST_CYCLES_PER_TIMEBASE_TICK);
    cpu->timebase += elapsed_timebase;
    if (!g_guest_clock_decrementer_valid || g_guest_clock_decrementer_expired)
        return;
    const u32 decrement = elapsed_timebase > 0xFFFFFFFFu
                              ? 0xFFFFFFFFu
                              : (u32)elapsed_timebase;
    const u32 before = g_guest_clock_decrementer;
    g_guest_clock_decrementer -= decrement;
    if ((before & 0x80000000u) == 0u &&
        (g_guest_clock_decrementer & 0x80000000u) != 0u) {
        if (g_guest_clock_trace) {
            fprintf(stderr,
                    "[decrementer] expiry timebase=%llu pc=0x%08X value=0x%08X\n",
                    (unsigned long long)cpu->timebase, cpu->pc,
                    g_guest_clock_decrementer);
        }
        g_guest_clock_decrementer_expired = true;
        g_guest_decrementer_pending = true;
        if (g_delivery_safety_census_enabled)
            g_clock_decrementer_expiries++;
    }
}

static void host_queue_dsp_task_request(void) {
    if (g_dsp_task_request_pending && !g_dsp_mail_from_pending)
        g_dsp_task_request_pending = false;
    if (g_dsp_boot_task_ready && !g_dsp_task_request_pending &&
        !g_dsp_mail_from_pending) {
        g_dsp_mail_from = 0xDCD10004u;
        g_dsp_mail_reads_remaining = 1u;
        g_dsp_mail_from_pending = true;
        g_dsp_task_request_pending = true;
        g_dsp_task_request_armed = false;
    }
}

static void host_refresh_interrupt_sources(CPUState* cpu);

// Interrupt-source recomputation gate.
//
// host_interrupt_sources() is a pure function of device state - three booleans
// read out of the DI, the DSP/ARAM/audio modules and the SI - and every mutation
// of that state arrives through one of three host entry points: a guest device
// register access (host_mmio_read and host_mmio_write, which also step the device
// cursors), a device cursor advance (host_sync_cycle_devices and its end-of-turn
// twin) or the DSP adapter paths (host_dsp_run_cpu_cycles, which is where the
// donor DSP raises DIRQ, and the mailbox reads that clear it). The refresh used to
// rebuild all three booleans at every block boundary - 1.305 publishes a boundary,
// and the play window changes its answer 17 times a retrace
// (docs/status/CURRENT.md, 2026-09-22) - so those entry points are its real
// trigger.
//
// The flag is set at every one of them, before the work that can change the
// answer, so a change is published at the next boundary exactly as it was before
// and a missed event can only make a publish late rather than lose one. The gate
// is the same as the increment's: the route digest, the stop pc and turn count,
// and the published-set count the census reports, which must not fall. It is
// declared near the top of the file because the edge service reads it directly.

#ifdef BLUEWAKE_HAS_DSP_ADAPTER
#define DSP_LLE_MAIL_SLICE 72u
#define DSP_LLE_CONTROL_MASK 0x0C07u

static void host_dsp_run_cpu_cycles(u64 cpu_cycles) {
    g_interrupt_sources_dirty = true;
    const u64 dsp_cycles = cpu_cycles / 6u;
    if (g_dsp_adapter == NULL || dsp_cycles == 0u)
        return;
    if (g_delivery_safety_census_enabled) {
        g_dsp_run_calls++;
        g_dsp_run_cpu_cycles += cpu_cycles;
    }
    bluewake_dsp_adapter_run_cycles(
        g_dsp_adapter,
        dsp_cycles > 0x7FFFFFFFu ? 0x7FFFFFFF : (int)dsp_cycles);
}

static void host_dsp_sync_mailbox_read(CPUState* cpu) {
    g_interrupt_sources_dirty = true;
    if (g_delivery_safety_census_enabled)
        g_dsp_mail_sync_calls++;
    if (g_dsp_adapter_slice_cycles > DSP_LLE_MAIL_SLICE) {
        host_dsp_run_cpu_cycles(DSP_LLE_MAIL_SLICE);
        g_dsp_adapter_slice_cycles -= DSP_LLE_MAIL_SLICE;
        if (g_delivery_safety_census_enabled)
            g_dsp_mail_sync_slices++;
        // A donor mailbox slice can raise DIRQ. Publish it at the MMIO
        // instruction that caused the slice, before translated code resumes.
        host_refresh_interrupt_sources(cpu);
    }
}

static void host_dsp_advance_schedule(u64 elapsed_cycles) {
    if (g_delivery_safety_census_enabled)
        g_dsp_advance_calls++;
    g_dsp_adapter_update_elapsed += elapsed_cycles;
    while (g_dsp_adapter_update_elapsed >= g_dsp_update_rate) {
        if (g_delivery_safety_census_enabled)
            g_dsp_advance_iters++;
        const u64 runnable_cycles =
            g_dsp_adapter_slice_cycles - (g_dsp_adapter_slice_cycles % 6u);
        host_dsp_run_cpu_cycles(runnable_cycles);
        g_dsp_adapter_slice_cycles -= runnable_cycles;
        g_dsp_adapter_slice_cycles += g_dsp_update_rate;
        g_dsp_adapter_update_elapsed -= g_dsp_update_rate;
    }
}
#endif

static BluewakeInterruptSources host_interrupt_sources(void) {
    bool donor_dsp_mail_pending = false;
    bool fabricated_dsp_pending = true;
#ifdef BLUEWAKE_HAS_DSP_ADAPTER
    const bool donor_route = g_dsp_adapter != NULL;
    donor_dsp_mail_pending = donor_route && g_dsp_adapter_interrupt_pending;
    fabricated_dsp_pending = !donor_route;
#endif
    return (BluewakeInterruptSources){
        .di = dol_di_interrupt_pending(&g_di),
        .dsp = bluewake_aram_dma_interrupt_pending(&g_aram_dma) ||
               dol_audio_dma_dsp_interrupt_pending(&g_audio_dma) ||
               donor_dsp_mail_pending ||
               (fabricated_dsp_pending &&
                (g_dsp_task_request_pending ||
                 (g_dsp_mail_from_pending &&
                  g_dsp_mail_from == 0xDCD10000u))),
        .si = dol_si_interrupt_pending(&g_si),
    };
}

// The interrupt sources are three booleans and the controller only ever holds
// their current value, so publishing a set identical to the last one is a no-op.
// Tracking the last published set lets the redundant path below be skipped; it
// lives here rather than in interrupt_sources.c so that every publisher of these
// lines in this file goes through it and the cache cannot go stale.
static BluewakeInterruptSources g_published_interrupt_sources;
static bool g_published_interrupt_sources_valid;

// The delivery-safety census is a diagnostic env flag whose body is counters and
// prints. It sits out of line so that the publish path below stays small enough
// to inline into the refresh that calls it 1.3 times a block boundary, of which
// the play window changes the answer 17 times a retrace - the census counts every
// call, changed or not, so this split moves the body without moving the count.
static void host_delivery_safety_census_sources(
    const BluewakeInterruptSources* sources) __attribute__((noinline));
static void host_delivery_safety_census_sources(
    const BluewakeInterruptSources* sources) {
    g_source_publishes++;
    if (sources->di || sources->dsp || sources->si)
        g_source_true_publishes++;
    if (g_published_interrupt_sources_valid) {
        if (!g_published_interrupt_sources.di && sources->di)
            g_source_rise_di++;
        if (!g_published_interrupt_sources.dsp && sources->dsp)
            g_source_rise_dsp++;
        if (!g_published_interrupt_sources.si && sources->si)
            g_source_rise_si++;
        if (g_source_timeline_reports < 400u &&
            ((!g_published_interrupt_sources.di && sources->di) ||
             (!g_published_interrupt_sources.dsp && sources->dsp) ||
             (!g_published_interrupt_sources.si && sources->si))) {
            fprintf(stderr,
                    "[source-timeline] #%u di=%u dsp=%u si=%u cycle=%llu\n",
                    g_source_timeline_reports + 1u, sources->di ? 1u : 0u,
                    sources->dsp ? 1u : 0u, sources->si ? 1u : 0u,
                    (unsigned long long)g_cycle_domain.absolute_cycles);
            g_source_timeline_reports++;
        }
    }
}

// Returns true when the controller was actually told something new.
static inline bool host_publish_changed_sources(
    const BluewakeInterruptSources* sources) {
#if BLUEWAKE_EDGE_CENSUS
    g_edge_source_publishes++;
#endif
    if (g_delivery_safety_census_enabled)
        host_delivery_safety_census_sources(sources);
    if (g_published_interrupt_sources_valid &&
        g_published_interrupt_sources.di == sources->di &&
        g_published_interrupt_sources.dsp == sources->dsp &&
        g_published_interrupt_sources.si == sources->si)
        return false;
    g_published_interrupt_sources = *sources;
    g_published_interrupt_sources_valid = true;
    bluewake_interrupt_sources_publish(&g_interrupts, sources);
#if BLUEWAKE_EDGE_CENSUS
    g_edge_source_changes++;
#endif
    return true;
}

static void host_refresh_interrupt_sources(CPUState* cpu) {
    if (!g_interrupt_sources_dirty)
        return;
    g_interrupt_sources_dirty = false;
    const BluewakeInterruptSources sources = host_interrupt_sources();
    // host_mmio_read and host_mmio_write both call
    // bluewake_cycle_domain_observe first, and observe already recomputes the
    // dispatch budget. Rebuilding it a second time in the same handler only has
    // something to add when the interrupt lines themselves moved, which is what
    // this gate tests. For the deadline's other terms, the device sync leaves
    // the distances invariant: the sync and the deadline both measure the same
    // cycle-domain lag, so a sync that advances a device cursor by n also
    // advances the elapsed term behind that device's deadline by n.
    if (!host_publish_changed_sources(&sources))
        return;
    bluewake_cycle_domain_rebudget(&g_cycle_domain, cpu);
}

static void host_publish_interrupt_sources(void) {
    const BluewakeInterruptSources sources = host_interrupt_sources();
    (void)host_publish_changed_sources(&sources);
}

static u8 host_ipl_sram_read8(void* user, u32 physical) {
    return mem_read8((CPUState*)user, 0x80000000u | physical);
}

static void host_ipl_sram_write8(void* user, u32 physical, u8 value) {
    mem_write8((CPUState*)user, 0x80000000u | physical, value);
}

// BLUEWAKE_MMIO_UNHANDLED=1: report each hardware register address the guest
// touches that no device claims, once, at the point it falls through (a read
// returns 0). This is how the unemulated SRAM hid (every EXI read was 0).
static void host_mmio_unhandled(const char* kind, u32 address, u8 size,
                                u64 value, u32 pc) {
    static int enabled = -1;
    static u32 seen[256];
    static unsigned seen_count;
    if (enabled < 0)
        enabled = getenv("BLUEWAKE_MMIO_UNHANDLED") != NULL ? 1 : 0;
    if (enabled == 0)
        return;
    const u32 key = (address << 1) | (kind[0] == 'w' ? 1u : 0u);
    for (unsigned i = 0; i < seen_count; i++)
        if (seen[i] == key)
            return;
    if (seen_count < 256u)
        seen[seen_count++] = key;
    fprintf(stderr,
            "[mmio-unhandled] %s address=0x%08X size=%u value=0x%llX pc=0x%08X "
            "retrace=%llu\n",
            kind, address, size, (unsigned long long)value, pc,
            (unsigned long long)g_host_retrace_count);
}

static u64 host_mmio_read(CPUState* ctx, u32 address, u8 size) {
#if BLUEWAKE_EDGE_CENSUS
    g_mmio_read_buckets[host_mmio_bucket(address)]++;
#endif
    u64 memory_value = 0u;
    if (bluewake_external_memory_read(ctx, address, size, &memory_value))
        return memory_value;
    if (bluewake_ipl_sram_contains(&g_ipl_sram, address) && size == 4u)
        return bluewake_ipl_sram_read(&g_ipl_sram, address);
    if (g_efb_peek_enabled && size == 4u && (address & 0xFF000000u) == 0xC8000000u &&
        (address & 0x00C00000u) == 0x00400000u) {
        const u16 x = (u16)((address >> 2) & 0x3FFu);
        const u16 y = (u16)((address >> 12) & 0x3FFu);
        u32 z = 0x00FFFFFFu;  // before the first snapshot: the cleared (far) depth
        const bool live = aurora_peek_z(x, y, &z);
        if (g_efb_peek_reports < 12u && getenv("BLUEWAKE_EFB_PEEK_LOG") != NULL) {
            g_efb_peek_reports++;
            fprintf(stderr, "[efb-peek] z x=%u y=%u value=0x%06X snapshot=%u retrace=%llu\n",
                    x, y, z, live ? 1u : 0u, (unsigned long long)g_host_retrace_count);
        }
        return z;
    }

    (void)bluewake_cycle_domain_observe(
        &g_cycle_domain, ctx, ctx->cycle_observation_suffix);
    host_sync_cycle_devices(ctx);
    // A device register access can raise or acknowledge an interrupt after the
    // sync above has already published; the block boundary is where that was
    // published before the gate existed, so mark it here rather than at entry.
    g_interrupt_sources_dirty = true;

    if (dol_di_mmio_contains(address))
        return dol_di_mmio_read(&g_di, address, size);
    if (dol_interrupts_mmio_contains(address))
        return dol_interrupts_mmio_read(&g_interrupts, address, size);
    if (dol_si_mmio_contains(address)) {
        const u64 value = dol_si_mmio_read(&g_si, address, size);
        if (g_pad_wire_trace && size == 4u &&
            (address == DOL_SI_BASE + 0x04u ||
             address == DOL_SI_BASE + 0x08u) &&
            g_pad_wire_reports < 64u) {
            const u32 input_buffer = 0x803F0F30u + ctx->gpr[29] * 8u;
            g_pad_si_data_watch = ctx->gpr[30];
            fprintf(stderr,
                    "[pad-wire] pc=0x%08X read address=0x%08X "
                    "value=0x%08llX input_pre=0x%08X,0x%08X "
                    "data_ptr=0x%08X data_pre=0x%08X,0x%08X retrace=%llu\n",
                    ctx->pc, address, (unsigned long long)value,
                    mem_read32(ctx, input_buffer),
                    mem_read32(ctx, input_buffer + 4u),
                    ctx->gpr[30], mem_read32(ctx, ctx->gpr[30]),
                    mem_read32(ctx, ctx->gpr[30] + 4u),
                    (unsigned long long)g_host_retrace_count);
            g_pad_wire_reports++;
        }
        return value;
    }
    if (address >= 0xCC006C00u && address < 0xCC006C20u) {
        const u32 offset = address - 0xCC006C00u;
        u64 value = 0;
        dol_audio_dma_ai_mmio_read(&g_audio_dma, offset, size, &value);
        return value;
    }
    if (address == 0xCC005004u && size == 2u) {
#ifdef BLUEWAKE_HAS_DSP_ADAPTER
        if (g_dsp_adapter != NULL) {
            host_dsp_sync_mailbox_read(ctx);
            const u32 mailbox =
                bluewake_dsp_adapter_peek_dsp_mailbox(g_dsp_adapter);
            if (g_audio_object_watch && g_dsp_adapter_mail_read_reports < 32u) {
                fprintf(stderr,
                        "[dsp-lle] mail-from-high=0x%04X mailbox=0x%08X "
                        "pc=0x%08X\n",
                        (u16)(mailbox >> 16), mailbox, ctx->pc);
                g_dsp_adapter_mail_read_reports++;
            }
            return mailbox >> 16;
        }
#endif
        if (!g_dsp_mail_from_pending && g_dsp_boot_mail_armed &&
            !g_dsp_boot_mail_clear_seen) {
            g_dsp_boot_mail_clear_seen = true;
            return 0u;
        }
        if (!g_dsp_mail_from_pending && g_dsp_boot_mail_armed &&
            g_dsp_boot_mail_clear_seen && !g_dsp_boot_handshake_sent) {
            g_dsp_mail_from = 0x8071FEEDu;
            g_dsp_mail_reads_remaining = 1u;
            g_dsp_mail_from_pending = true;
            g_dsp_boot_handshake_sent = true;
            host_refresh_interrupt_sources(ctx);
        }
        const u16 value = g_dsp_mail_from_pending
                              ? (u16)(g_dsp_mail_from >> 16)
                              : 0u;
        if (g_audio_object_watch && g_dsp_mail_reports < 24u) {
            fprintf(stderr,
                    "[audio-dsp] mail-from-high=0x%04X pending=%u pc=0x%08X "
                    "lr=0x%08X\n",
                    value, g_dsp_mail_from_pending ? 1u : 0u, ctx->pc,
                    ctx->lr);
            g_dsp_mail_reports++;
        }
        return value;
    }
    if (address == 0xCC005000u && size == 2u) {
#ifdef BLUEWAKE_HAS_DSP_ADAPTER
        if (g_dsp_adapter != NULL) {
            host_dsp_sync_mailbox_read(ctx);
            return bluewake_dsp_adapter_peek_cpu_mailbox(g_dsp_adapter) >> 16;
        }
#endif
        return 0u;
    }
    if (address == 0xCC005002u && size == 2u) {
#ifdef BLUEWAKE_HAS_DSP_ADAPTER
        if (g_dsp_adapter != NULL)
            return bluewake_dsp_adapter_peek_cpu_mailbox(g_dsp_adapter) & 0xFFFFu;
#endif
        return 0u;
    }
    if (address == 0xCC005006u && size == 2u) {
#ifdef BLUEWAKE_HAS_DSP_ADAPTER
        if (g_dsp_adapter != NULL) {
            const u16 value =
                bluewake_dsp_adapter_read_dsp_mailbox_low(g_dsp_adapter);
            // The LLE route clears the DSP interrupt at the mailbox read (the
            // certified digest depends on it). The high-level backend raises
            // the next mail's interrupt from inside this read, so there the
            // interrupt stays pending until the guest acknowledges DSPINT in
            // the control register, as on hardware.
            if (!bluewake_dsp_adapter_is_hle(g_dsp_adapter))
                g_dsp_adapter_interrupt_pending = false;
            host_refresh_interrupt_sources(ctx);
            if (g_audio_object_watch && g_dsp_adapter_mail_read_reports < 32u) {
                fprintf(stderr,
                        "[dsp-lle] mail-from-low=0x%04X mailbox-after=0x%08X "
                        "pc=0x%08X\n",
                        value,
                        bluewake_dsp_adapter_peek_dsp_mailbox(g_dsp_adapter),
                        ctx->pc);
                g_dsp_adapter_mail_read_reports++;
            }
            return value;
        }
#endif
        const u32 mail = g_dsp_mail_from;
        const u16 value = (u16)mail;
        if (g_dsp_mail_reads_remaining > 0u)
            g_dsp_mail_reads_remaining--;
        if (g_dsp_mail_reads_remaining == 0u) {
            g_dsp_mail_from_pending = false;
            if (g_dsp_task_request_pending && mail == 0xDCD10004u) {
                // __DSPHandler consumes the request before syncDSP reads the
                // JAS completion word from the same DSP mailbox stream.
                g_dsp_task_request_pending = false;
                g_dsp_mail_from = 0xF355FF00u;
                g_dsp_mail_reads_remaining = 1u;
                g_dsp_mail_from_pending = true;
            }
            if (mail == 0xDCD10000u) {
                g_dsp_boot_task_ready = true;
                if (g_dsp_task_request_armed) {
                    g_dsp_mail_from = 0xDCD10004u;
                    g_dsp_mail_reads_remaining = 1u;
                    g_dsp_mail_from_pending = true;
                    // DspHandShake consumes this second boot word directly;
                    // it is not the post-boot req_cb task request.
                    g_dsp_task_request_armed = false;
                }
            }
            if (mail == 0xF355FF00u && g_dsp_task_request_armed &&
                g_dsp_boot_task_ready)
                host_queue_dsp_task_request();
        }
        host_refresh_interrupt_sources(ctx);
        if (g_audio_object_watch && g_dsp_mail_reports < 24u) {
            fprintf(stderr,
                    "[audio-dsp] mail-from-low=0x%04X pc=0x%08X lr=0x%08X\n",
                    value, ctx->pc, ctx->lr);
            g_dsp_mail_reports++;
        }
        return value;
    }
    if (address == 0xCC005016u && size == 2u && g_dsp_aram_complete)
        return 0x0001u; // ARInit waits for ARAM mode to become active.
    if (address == 0xCC00500Au && size == 2u) {
        u16 value = g_dsp_control;
        if (dol_audio_dma_dsp_interrupt_pending(&g_audio_dma))
            value |= 0x0008u;
        else
            value &= (u16)~0x0008u;
        // The fabricated boot token is only a control-route status source.
        // Once the donor is active, its mailbox/interrupt state owns DSP_DSP;
        // retaining this token would outrank the authentic DSP-AI status bit.
        bool fabricated_dsp_route = true;
        bool donor_dsp_interrupt_pending = false;
#ifdef BLUEWAKE_HAS_DSP_ADAPTER
        const bool donor_dsp_route = g_dsp_adapter != NULL;
        donor_dsp_interrupt_pending = g_dsp_adapter_interrupt_pending;
        if (donor_dsp_route) {
            const u16 adapter_control =
                bluewake_dsp_adapter_read_control(g_dsp_adapter);
            value = (u16)((value & (u16)~DSP_LLE_CONTROL_MASK) |
                          (adapter_control & DSP_LLE_CONTROL_MASK));
        }
#else
        const bool donor_dsp_route = false;
#endif
#ifdef BLUEWAKE_HAS_DSP_ADAPTER
        fabricated_dsp_route = g_dsp_adapter == NULL;
#endif
        // DSPCore owns the mailbox payload, but the guest still observes its
        // DSP-DSP status bit through the register file before reading it.
        if ((donor_dsp_route && donor_dsp_interrupt_pending) ||
            (fabricated_dsp_route &&
            (g_dsp_task_request_pending ||
             (g_dsp_mail_from_pending && g_dsp_mail_from == 0xDCD10000u))))
            value |= 0x0080u;
        if (bluewake_aram_dma_interrupt_pending(&g_aram_dma))
            value |= 0x0020u;
        else
            value &= (u16)~0x0020u;
        if (g_audio_object_watch && g_dsp_task_request_pending &&
            g_dsp_irq_route_reports < 24u) {
            fprintf(stderr,
                    "[audio-dsp-status] raw=0x%04X effective=0x%04X "
                    "request_pending=%u aram_pending=%u\n",
                    g_dsp_control, value, g_dsp_task_request_pending ? 1u : 0u,
                    bluewake_aram_dma_interrupt_pending(&g_aram_dma) ? 1u : 0u);
            g_dsp_irq_route_reports++;
        }
        return value;
    }
    if (bluewake_aram_dma_contains(address))
        return bluewake_aram_dma_read(&g_aram_dma, address, size);
    if (address >= 0xCC005000u && address < 0xCC005040u) {
        const u32 offset = address - 0xCC005000u;
        u64 value = 0;
        dol_audio_dma_dsp_mmio_read(&g_audio_dma, offset, size, &value);
        return value;
    }
    host_mmio_unhandled("read", address, size, 0u, ctx->pc);
    return 0;
}

static void si_store_be32(u8* bytes, u32 offset, u32 value) {
    bytes[offset] = (u8)(value >> 24);
    bytes[offset + 1u] = (u8)(value >> 16);
    bytes[offset + 2u] = (u8)(value >> 8);
    bytes[offset + 3u] = (u8)value;
}

static u32 si_load_be32(const u8* bytes, u32 offset) {
    return ((u32)bytes[offset] << 24) | ((u32)bytes[offset + 1u] << 16) |
           ((u32)bytes[offset + 2u] << 8) | (u32)bytes[offset + 3u];
}

static u16 host_pad_buttons(u32 channel, u16 fallback) {
    if (g_live_takeover)
        return fallback;
    if (channel == 0u && g_title_pad_pulse.configured)
        fallback = bluewake_pad_event_schedule_sample(
            &g_title_pad_pulse, g_host_retrace_count, fallback);
    if (channel == 0u && g_title_confirm_pulse.configured)
        fallback = bluewake_pad_event_schedule_sample(
            &g_title_confirm_pulse, g_host_retrace_count, fallback);
    if (channel == 0u && g_no_card_dismiss_pulse.configured)
        fallback = bluewake_pad_event_schedule_sample(
            &g_no_card_dismiss_pulse, g_host_retrace_count, fallback);
    if (channel == 0u && g_no_save_confirm_pulse.configured)
        fallback = bluewake_pad_event_schedule_sample(
            &g_no_save_confirm_pulse, g_host_retrace_count, fallback);
    if (channel == 0u && g_file_slot_select_pulse.configured)
        fallback = bluewake_pad_event_schedule_sample(
            &g_file_slot_select_pulse, g_host_retrace_count, fallback);
    if (channel == 0u && g_file_start_pulse.configured)
        fallback = bluewake_pad_event_schedule_sample(
            &g_file_start_pulse, g_host_retrace_count, fallback);
    if (channel == 0u && g_name_character_pulse.configured)
        fallback = bluewake_pad_event_schedule_sample(
            &g_name_character_pulse, g_host_retrace_count, fallback);
    if (channel == 0u && g_name_end_pulse.configured)
        fallback = bluewake_pad_event_schedule_sample(
            &g_name_end_pulse, g_host_retrace_count, fallback);
    if (channel == 0u && g_name_confirm_pulse.configured)
        fallback = bluewake_pad_event_schedule_sample(
            &g_name_confirm_pulse, g_host_retrace_count, fallback);
    if (channel == 0u && g_event_confirm_pulse.configured)
        fallback = bluewake_pad_event_schedule_sample(
            &g_event_confirm_pulse, g_host_retrace_count, fallback);
    if (channel == 0u && g_player_route_confirm_pulse.configured)
        fallback = bluewake_pad_event_schedule_sample(
            &g_player_route_confirm_pulse, g_host_retrace_count, fallback);
    // The script is additive, not an override. An earlier version returned 0 on
    // every retrace no scheduled press covered, which clobbered the latched
    // new-game schedules above and moved name_create from retrace 449 to
    // 18,013 - caught by the milestone summary on the first run. It now only
    // contributes buttons; the fixed pulses below still take precedence and the
    // schedules above still survive.
    if (channel == 0u && g_pad_script_count > 0u &&
        !g_save_route_script_suppressed) {
        for (unsigned i = 0u; i < g_pad_script_count; i++) {
            const BluewakePadScriptPress* press = &g_pad_script[i];
            if (g_host_retrace_count >= press->start_retrace &&
                g_host_retrace_count <
                    press->start_retrace + press->length) {
                fallback = press->buttons;
                break;
            }
        }
    }
    // The save route's own two buttons sit after the script so a press it
    // decides on cannot be overwritten by a scheduled one that happens to
    // overlap it. Both schedules are one-shot and inert unless the route armed
    // them, and the route does not start until the script's last press is past.
    if (channel == 0u && g_save_start_pulse.configured)
        fallback = bluewake_pad_event_schedule_sample(
            &g_save_start_pulse, g_host_retrace_count, fallback);
    if (channel == 0u && g_save_confirm_pulse.configured)
        fallback = bluewake_pad_event_schedule_sample(
            &g_save_confirm_pulse, g_host_retrace_count, fallback);
    if (channel == 0u && g_save_page_pulse.configured)
        fallback = bluewake_pad_event_schedule_sample(
            &g_save_page_pulse, g_host_retrace_count, fallback);
    if (channel == 0u && g_pad_pulse_enabled) {
        const u64 end = g_pad_pulse_start_retrace + g_pad_pulse_length;
        if (g_host_retrace_count >= g_pad_pulse_start_retrace &&
            g_host_retrace_count < end)
            return g_pad_pulse_buttons;
    }
    if (channel == 0u && g_pad_pulse2_enabled) {
        const u64 end = g_pad_pulse2_start_retrace + g_pad_pulse2_length;
        if (g_host_retrace_count >= g_pad_pulse2_start_retrace &&
            g_host_retrace_count < end)
            return g_pad_pulse_buttons;
    }
    if (channel == 0u && g_pad_pulse3_enabled) {
        const u64 end = g_pad_pulse3_start_retrace + g_pad_pulse3_length;
        if (g_host_retrace_count >= g_pad_pulse3_start_retrace &&
            g_host_retrace_count < end)
            return g_pad_pulse_buttons;
    }
    return channel == 0u && (g_pad_pulse_enabled || g_pad_pulse2_enabled ||
                             g_pad_pulse3_enabled)
               ? 0u
               : fallback;
}

// A person took the controls. Test runs script presses (the route pulses, the
// pad script, the event confirmation) and a player who picks up the touch
// controls in the middle of one had A pressed under them: moving while the
// script pressed A made Link roll (user report, 2026-09-24). The first live
// input on channel 0 therefore retires every scripted press and stick for the
// rest of the run (g_live_takeover, declared with the retrace count).

static void host_note_live_input(const DolPadState* live) {
    if (g_live_takeover)
        return;
    // A resting stick with a little drift is not a person; a deflection of a
    // fifth of the travel or any button is.
    const int dead = 20;
    if (live->button == 0u && abs(live->stick_x) < dead && abs(live->stick_y) < dead &&
        abs(live->substick_x) < dead && abs(live->substick_y) < dead &&
        live->trigger_left < 40u && live->trigger_right < 40u)
        return;
    g_live_takeover = true;
    fprintf(stderr,
            "[pad] live input at retrace %llu: scripted presses off for the "
            "rest of the run\n",
            (unsigned long long)g_host_retrace_count);
}

static DolPadState host_pad_state(u32 channel, const DolPadState* source) {
    DolPadState result = *source;
    if (g_live_takeover)
        return result;
    if (channel == 0u && g_pad_script_count > 0u &&
        !g_save_route_script_suppressed) {
        for (unsigned i = 0u; i < g_pad_script_count; i++) {
            const BluewakePadScriptPress* press = &g_pad_script[i];
            if (press->has_stick &&
                g_host_retrace_count >= press->start_retrace &&
                g_host_retrace_count < press->start_retrace + press->length) {
                result.stick_x = press->stick_x;
                result.stick_y = press->stick_y;
                if (press->has_substick) {
                    result.substick_x = press->substick_x;
                    result.substick_y = press->substick_y;
                }
                break;
            }
        }
    }
    if (channel == 0u && g_no_save_left_pulse.configured) {
        result.stick_x = bluewake_pad_axis_event_schedule_sample(
            &g_no_save_left_pulse, g_host_retrace_count, result.stick_x);
    }
    if (channel == 0u && g_player_stick_x_pulse.configured) {
        result.stick_x = bluewake_pad_axis_event_schedule_sample(
            &g_player_stick_x_pulse, g_host_retrace_count, result.stick_x);
    }
    if (channel == 0u && g_player_stick_y_pulse.configured) {
        result.stick_y = bluewake_pad_axis_event_schedule_sample(
            &g_player_stick_y_pulse, g_host_retrace_count, result.stick_y);
    }
    if (channel == 0u && g_player_waypoint_active) {
        result.stick_x = g_player_waypoint_stick_x;
        result.stick_y = g_player_waypoint_stick_y;
    }
    if (channel == 0u && g_save_stick_x_pulse.configured) {
        result.stick_x = bluewake_pad_axis_event_schedule_sample(
            &g_save_stick_x_pulse, g_host_retrace_count, result.stick_x);
    }
    if (channel == 0u && g_save_stick_y_pulse.configured) {
        result.stick_y = bluewake_pad_axis_event_schedule_sample(
            &g_save_stick_y_pulse, g_host_retrace_count, result.stick_y);
    }
    if (channel == 0u && g_save_trigger_length != 0u &&
        g_host_retrace_count >= g_save_trigger_start_retrace &&
        g_host_retrace_count <
            g_save_trigger_start_retrace + g_save_trigger_length) {
        result.trigger_right = g_save_trigger_value;
    }
    return result;
}

// Squeeze the analog R trigger, which is what the pause menu's page switch and
// anything else asking for mDoCPd_R_LOCK_BUTTON reads.
static void save_route_press_analog_r(u64 length) {
    g_save_trigger_value = 255u;
    g_save_trigger_length = length;
    g_save_trigger_start_retrace = g_host_retrace_count + 1u;
}

// Press one button of the save route's own schedules. The schedules are
// one-shot, so every press rearms first.
static void save_route_press(BluewakePadEventSchedule* schedule) {
    bluewake_pad_event_schedule_rearm(schedule);
    bluewake_pad_event_schedule_trigger(schedule, g_host_retrace_count);
}

// Deflect the stick in one of the eight directions, for `length` retraces. The
// axis schedules each carry one value; the axis a direction leaves at zero is
// simply left untriggered, which samples back to the pad's own (idle) value.
static void save_route_press_stick(u8 direction, u64 length) {
    bluewake_pad_axis_event_schedule_rearm(&g_save_stick_x_pulse);
    bluewake_pad_axis_event_schedule_rearm(&g_save_stick_y_pulse);
    if (k_save_route_dir_x[direction] != 0) {
        g_save_stick_x_pulse.value = k_save_route_dir_x[direction];
        g_save_stick_x_pulse.length = length;
        bluewake_pad_axis_event_schedule_trigger(&g_save_stick_x_pulse,
                                                 g_host_retrace_count);
    }
    if (k_save_route_dir_y[direction] != 0) {
        g_save_stick_y_pulse.value = k_save_route_dir_y[direction];
        g_save_stick_y_pulse.length = length;
        bluewake_pad_axis_event_schedule_trigger(&g_save_stick_y_pulse,
                                                 g_host_retrace_count);
    }
}

static void save_route_finish(const char* what, u64 retrace) {
    fprintf(stderr, "[save-milestone] failed=%s retrace=%llu\n", what,
            (unsigned long long)retrace);
    g_save_route_state = BLUEWAKE_SAVE_STATE_DONE;
}

// One retrace of the in-game save route. Called once per retrace, after the
// route's own milestones have been read, so every field it reports is the value
// the guest left at the end of that retrace.
static void save_route_step(CPUState* cpu) {
    if (!g_save_route_enabled ||
        g_save_route_state == BLUEWAKE_SAVE_STATE_DONE)
        return;

    const u64 retrace = g_host_retrace_count;
    const u32 player = mem_read32(cpu, BLUEWAKE_GUEST_PLAYER);
    const bool player_valid = player >= 0x80000000u;
    const u8 event_mode = mem_read8(cpu, BLUEWAKE_GUEST_EVENT_MODE);
    const u32 demo_mode =
        player_valid ? mem_read32(cpu, player + 0x314u) : 0u;
    const u16 demo_type =
        player_valid ? mem_read16(cpu, player + 0x304u) : 0u;
    // play.mMesgStatus: 0x803C4C08 + 0x12A0 (save ends, play begins) + 0x492A.
    const u8 message = mem_read8(cpu, BLUEWAKE_GUEST_MESG_STATUS);
    const u32 dmc = mem_read32(cpu, BLUEWAKE_GUEST_DMC_C);
    const u32 dms = mem_read32(cpu, BLUEWAKE_GUEST_DMS_C);
    const u32 dms_collect =
        dmc >= 0x80000000u ? mem_read32(cpu, dmc + BLUEWAKE_DMC_OFF_SAVE_MENU)
                           : 0u;
    const u8 menu_pause = mem_read8(cpu, BLUEWAKE_GUEST_DMENU_PAUSE);
    const u8 menu_status = mem_read8(cpu, BLUEWAKE_GUEST_MENU_STATUS);
    const u8 menu_status_old =
        mem_read8(cpu, BLUEWAKE_GUEST_MENU_STATUS_OLD);
    const s8 event_wait_frame =
        (s8)mem_read8(cpu, BLUEWAKE_GUEST_EVENT_WAIT_FRAME);
    const u8 mesg_status = mem_read8(cpu, BLUEWAKE_GUEST_MESG_STATUS);
    const u8 heap_lock = mem_read8(cpu, BLUEWAKE_GUEST_HEAP_LOCK);
    const s8 next_stage_enable =
        (s8)mem_read8(cpu, BLUEWAKE_GUEST_NEXT_STAGE_ENABLE);
    const u8 collect_mode =
        dmc >= 0x80000000u
            ? mem_read8(cpu, dmc + BLUEWAKE_DMC_OFF_MODE)
            : 0xFFu;

    if (g_save_route_state == BLUEWAKE_SAVE_STATE_IDLE) {
        // With a steered route armed, save where the route ends (for example
        // inside Link's house), not where control first arrives.
        if (g_player_post_ladder_route_configured &&
            !g_player_post_ladder_route_complete)
            return;
        // Control is not one moment. The route's own gate admits it at retrace
        // 20,256 - measured, and this build reproduces it - but the authored
        // sequence that follows is still walking: at 20,395 the guest sits at
        // demo_type=2 demo_mode=6 event=106 with a message window up, and it
        // stays there for as long as nothing presses A. So the save route asks
        // for the state a human could take control in - play scene up, player
        // record valid, demo playback off, the event system idle, no message
        // window, no instance demo - and holds it for a second and a half
        // before it believes it, which is long enough to step over the
        // transient clear window before that sequence and no longer.
        const bool ready = player_valid && g_play_scene_reported &&
                           event_mode == 0u && demo_mode == 0u &&
                           demo_type == 0u && message == 0u;
        g_save_route_hold = ready ? g_save_route_hold + 1u : 0u;
        // The gate is four guest facts; when it does not open, they are what has
        // to be reported, or the next run is another guess.
        if (g_play_scene_reported && retrace % 300u == 0u &&
            g_save_route_wait_reports < 64u) {
            // Name the event the guest is sitting in, the same way the route's
            // own event probe does: the event table is guest data, so the name
            // is the guest's, not a label invented here.
            char event_name[33];
            event_name[0] = '\0';
            const s16 event_index = (s16)mem_read16(cpu, 0x803C9EB8u);
            const u32 event_table = mem_read32(cpu, 0x803C9ED8u);
            if (event_index >= 0 && event_table >= 0x80000000u) {
                const u32 event = event_table + (u32)event_index * 0xB0u;
                for (u32 i = 0u; i < 32u; ++i) {
                    const u8 ch = mem_read8(cpu, event + i);
                    event_name[i] = ch >= 0x20u && ch <= 0x7Eu ? (char)ch : '\0';
                    if (event_name[i] == '\0') {
                        for (u32 j = i + 1u; j < sizeof event_name; ++j)
                            event_name[j] = '\0';
                        break;
                    }
                }
                event_name[32] = '\0';
            }
            g_save_route_wait_reports++;
            fprintf(stderr,
                    "[save-milestone] waiting retrace=%llu blocks=%llu "
                    "player=0x%08X player_valid=%u play_scene=%u "
                    "event_mode=%u demo_mode=%u demo_type=%u message=%u "
                    "event=%d name=\"%s\" "
                    "dmc=0x%08X dms=0x%08X mode=%u item=%u hold=%u\n",
                    (unsigned long long)retrace, (unsigned long long)g_current_host_block,
                    player,
                    (unsigned)player_valid, (unsigned)g_play_scene_reported,
                    (unsigned)event_mode, demo_mode, (unsigned)demo_type,
                    (unsigned)message, (int)event_index, event_name, dmc, dms,
                    (unsigned)collect_mode,
                    dmc >= 0x80000000u
                        ? (unsigned)mem_read8(cpu, dmc +
                                              BLUEWAKE_DMC_OFF_NOW_ITEM)
                        : 0u,
                    g_save_route_hold);
        }
        if (g_save_route_hold < 90u)
            return;
        g_save_route_control_retrace = retrace;
        fprintf(stderr,
                "[save-milestone] control-ready retrace=%llu blocks=%llu "
                "event_mode=%u demo_mode=%u demo_type=%u message=%u "
                "menu_pause=%u menu_status=%u menu_status_old=%u "
                "event_wait_frame=%d mesg_status=%u heap_lock=%u "
                "next_stage_enable=%d menu_execute=%llu menu_collect=%llu\n",
                (unsigned long long)retrace, (unsigned long long)g_current_host_block,
                (unsigned)event_mode, demo_mode, (unsigned)demo_type,
                (unsigned)message, (unsigned)menu_pause,
                (unsigned)menu_status, (unsigned)menu_status_old,
                (int)event_wait_frame, (unsigned)mesg_status,
                (unsigned)heap_lock, (int)next_stage_enable,
                (unsigned long long)bluewake_edge_menu_execute_calls(),
                (unsigned long long)bluewake_edge_menu_collect_calls());
        save_route_press(&g_save_start_pulse);
        fprintf(stderr,
                "[save-milestone] press=START retrace=%llu attempts=%u "
                "script_presses=%u\n",
                (unsigned long long)(retrace + 1u), g_save_route_retries + 1u,
                (unsigned)g_pad_script_count);
        g_save_route_state = BLUEWAKE_SAVE_STATE_PAUSE;
        g_save_route_next_retrace = retrace;
        return;
    }

    if (g_save_route_state == BLUEWAKE_SAVE_STATE_PAUSE) {
        if (dmc >= 0x80000000u) {
            // The menu is up, so from here the blind cutscene script would be
            // pressing A inside it. It is turned off for the rest of the run.
            g_save_route_script_suppressed = true;
            const u8 item = mem_read8(cpu, dmc + BLUEWAKE_DMC_OFF_NOW_ITEM);
            fprintf(stderr,
                    "[save-milestone] pause-open retrace=%llu dmc=0x%08X "
                    "dms=0x%08X mode=%u item=%u menu_pause=%u "
                    "menu_status=%u menu_status_old=%u scripts_suppressed=%u\n",
                    (unsigned long long)retrace, dmc, dms,
                    (unsigned)collect_mode, (unsigned)item,
                    (unsigned)menu_pause, (unsigned)menu_status,
                    (unsigned)menu_status_old, (unsigned)g_pad_script_count);
            g_save_route_state = BLUEWAKE_SAVE_STATE_CURSOR;
            g_save_route_next_retrace = retrace + 20u;
        } else if (menu_pause != 0u) {
            // The pause menu is up on the item page. R switches it to the
            // collect page, which is where Save is; START here would close the
            // menu again, which is what the first version of this state did.
            if (retrace < g_save_route_next_retrace + 120u)
                return;
            if (++g_save_route_page_presses >= 40u) {
                save_route_finish("collect-page-never-opened", retrace);
                return;
            }
            save_route_press(&g_save_page_pulse);
            save_route_press_analog_r(6u);
            fprintf(stderr,
                    "[save-milestone] press=R+analogR collect-page retrace=%llu "
                    "presses=%u menu_pause=%u menu_status=%u "
                    "menu_status_old=%u dmc=0x%08X\n",
                    (unsigned long long)(retrace + 1u),
                    g_save_route_page_presses, (unsigned)menu_pause,
                    (unsigned)menu_status, (unsigned)menu_status_old, dmc);
            g_save_route_next_retrace = retrace;
        } else if (retrace >= g_save_route_next_retrace + 300u) {
            // START with the menu flags in the same line: if the press is seen
            // and the menu still does not open, these are the values that say
            // why, and they are read from the guest, not inferred.
            if (++g_save_route_retries >= 24u) {
                save_route_finish("pause-never-opened", retrace);
                return;
            }
            save_route_press(&g_save_start_pulse);
            fprintf(stderr,
                    "[save-milestone] press=START retry=%u retrace=%llu "
                    "menu_pause=%u menu_status=%u menu_status_old=%u "
                    "event_wait_frame=%d mesg_status=%u heap_lock=%u "
                    "next_stage_enable=%d event_mode=%u "
                    "demo_mode=%u demo_type=%u message=%u menu_execute=%llu "
                    "menu_collect=%llu\n",
                    g_save_route_retries, (unsigned long long)(retrace + 1u),
                    (unsigned)menu_pause, (unsigned)menu_status,
                    (unsigned)menu_status_old, (int)event_wait_frame,
                    (unsigned)mesg_status, (unsigned)heap_lock,
                    (int)next_stage_enable, (unsigned)event_mode, demo_mode,
                    (unsigned)demo_type, (unsigned)message,
                    (unsigned long long)bluewake_edge_menu_execute_calls(),
                    (unsigned long long)bluewake_edge_menu_collect_calls());
            g_save_route_next_retrace = retrace;
        }
        return;
    }

    if (g_save_route_state == BLUEWAKE_SAVE_STATE_CURSOR) {
        if (dmc < 0x80000000u) {
            save_route_finish("pause-closed-early", retrace);
            return;
        }
        if (collect_mode != g_save_route_mode) {
            g_save_route_mode = collect_mode;
            fprintf(stderr,
                    "[save-milestone] collect-mode retrace=%llu mode=%u\n",
                    (unsigned long long)retrace, (unsigned)collect_mode);
        }
        // A non-zero mode is the menu animating; the cursor is frozen then and
        // a press would be dropped. The settle window after each step is the
        // STControl's own: a fresh press triggers on the first frame it is
        // seen, and the menu needs a frame or two to take it.
        if (collect_mode != 0u || retrace < g_save_route_next_retrace)
            return;
        const u8 item = mem_read8(cpu, dmc + BLUEWAKE_DMC_OFF_NOW_ITEM);
        if (item != g_save_route_item) {
            g_save_route_item = item;
            fprintf(stderr, "[save-milestone] cursor retrace=%llu item=%u\n",
                    (unsigned long long)retrace, (unsigned)item);
        }
        if (item == BLUEWAKE_SAVE_MENU_ITEM) {
            save_route_press(&g_save_confirm_pulse);
            fprintf(stderr,
                    "[save-milestone] press=A open-save retrace=%llu "
                    "item=%u\n",
                    (unsigned long long)(retrace + 1u), (unsigned)item);
            g_save_route_state = BLUEWAKE_SAVE_STATE_SCREEN;
            g_save_route_next_retrace = retrace;
            return;
        }
        if (item >= 20u || ++g_save_route_steps > 80u) {
            fprintf(stderr,
                    "[save-milestone] failed=cursor item=%u steps=%u "
                    "retrace=%llu\n",
                    (unsigned)item, g_save_route_steps,
                    (unsigned long long)retrace);
            g_save_route_state = BLUEWAKE_SAVE_STATE_DONE;
            return;
        }
        const u8 direction = k_save_route_first_dir[item];
        if (direction == 0xFFu) {
            save_route_finish("cursor-no-path", retrace);
            return;
        }
        save_route_press_stick(direction, 3u);
        fprintf(stderr,
                "[save-milestone] press=stick retrace=%llu item=%u dir=%u "
                "stick=(%d,%d)\n",
                (unsigned long long)(retrace + 1u), (unsigned)item,
                (unsigned)direction, (int)k_save_route_dir_x[direction],
                (int)k_save_route_dir_y[direction]);
        g_save_route_next_retrace = retrace + 10u;
        return;
    }

    if (g_save_route_state == BLUEWAKE_SAVE_STATE_SCREEN) {
        if (collect_mode == BLUEWAKE_SAVE_MENU_MODE &&
            dms_collect >= 0x80000000u) {
            const u8 proc =
                mem_read8(cpu, dms_collect + BLUEWAKE_DMS_OFF_PROC);
            const u8 status =
                mem_read8(cpu, dms_collect + BLUEWAKE_DMS_OFF_SAVE_STATUS);
            fprintf(stderr,
                    "[save-milestone] save-screen retrace=%llu mode=%u "
                    "dmc=0x%08X save_menu=0x%08X proc=%u status=%u\n",
                    (unsigned long long)retrace, (unsigned)collect_mode, dmc,
                    dms_collect, (unsigned)proc, (unsigned)status);
            g_save_route_state = BLUEWAKE_SAVE_STATE_MENU;
            g_save_route_menu_retrace = retrace;
            g_save_route_proc = proc;
            g_save_route_status = status;
        } else if (bluewake_card_runtime_write_calls() != 0u) {
            // The card was already written while this state still waited for the
            // save screen's mode: follow the save from the card from here.
            fprintf(stderr,
                    "[save-milestone] save-screen-by-card retrace=%llu "
                    "mode=%u dmc=0x%08X save_menu=0x%08X card_writes=%llu "
                    "card_bytes=%llu\n",
                    (unsigned long long)retrace, (unsigned)collect_mode, dmc,
                    dms_collect,
                    (unsigned long long)bluewake_card_runtime_write_calls(),
                    (unsigned long long)bluewake_card_runtime_write_bytes());
            g_save_route_state = BLUEWAKE_SAVE_STATE_MENU;
            g_save_route_menu_retrace = retrace;
            g_save_route_proc = 0xFFu;
            g_save_route_status = 0xFFu;
        } else if (retrace >= g_save_route_next_retrace + 900u) {
            if (++g_save_route_retries >= 6u) {
                save_route_finish("save-screen-never-opened", retrace);
                return;
            }
            save_route_press(&g_save_confirm_pulse);
            fprintf(stderr,
                    "[save-milestone] press=A open-save retry=%u "
                    "retrace=%llu mode=%u dmc=0x%08X save_menu=0x%08X "
                    "card_writes=%llu\n",
                    g_save_route_retries, (unsigned long long)(retrace + 1u),
                    (unsigned)collect_mode, dmc, dms_collect,
                    (unsigned long long)bluewake_card_runtime_write_calls());
            g_save_route_next_retrace = retrace;
        }
        return;
    }

    if (g_save_route_state == BLUEWAKE_SAVE_STATE_MENU) {
        const u8 proc =
            dms_collect >= 0x80000000u
                ? mem_read8(cpu, dms_collect + BLUEWAKE_DMS_OFF_PROC)
                : 0xFFu;
        const u8 status =
            dms_collect >= 0x80000000u
                ? mem_read8(cpu,
                            dms_collect + BLUEWAKE_DMS_OFF_SAVE_STATUS)
                : 0xFFu;
        // The collect page owns the save screen and frees it between uses, so a
        // vanished object is only a failure while the card has not been written.
        if (dms_collect < 0x80000000u && collect_mode == 0u &&
            bluewake_card_runtime_write_calls() != 0u) {
            fprintf(stderr,
                    "[save-milestone] save-menu-closed retrace=%llu "
                    "card_writes=%llu card_bytes=%llu\n",
                    (unsigned long long)retrace,
                    (unsigned long long)bluewake_card_runtime_write_calls(),
                    (unsigned long long)bluewake_card_runtime_write_bytes());
            g_save_route_state = BLUEWAKE_SAVE_STATE_DONE;
            return;
        }
        if (dms_collect < 0x80000000u) {
            save_route_finish("save-screen-vanished", retrace);
            return;
        }
        if (proc != g_save_route_proc || status != g_save_route_status) {
            g_save_route_proc = proc;
            g_save_route_status = status;
            fprintf(stderr,
                    "[save-milestone] save-proc retrace=%llu mode=%u proc=%u "
                    "status=%u card_writes=%llu card_bytes=%llu\n",
                    (unsigned long long)retrace, (unsigned)collect_mode,
                    (unsigned)proc, (unsigned)status,
                    (unsigned long long)bluewake_card_runtime_write_calls(),
                    (unsigned long long)bluewake_card_runtime_write_bytes());
        }
        // The save screen's own prompts, from d_menu_save.cpp's MenuSaveProc:
        // 0 is the save question (its cursor starts on the Save line, and 0x52F
        // is that cursor), 23 auto-advances the completion message, 25 waits
        // for a key, 29 raises the "return to the title screen?" question with
        // 30 taking its answer, and 31 is that answer acting: it requests the
        // reset. Everything else in the chain is the card work itself.
        if (proc == 0u || proc == 25u) {
            // A prompt that needs a key: press it, and press it again if the
            // save screen has not moved on, because an A that lands while the
            // panel is still fading in is dropped and one press is not proof
            // of anything.
            if (proc == g_save_route_acted_proc &&
                status == g_save_route_acted_status &&
                retrace < g_save_route_acted_retrace + 150u)
                return;
            g_save_route_acted_proc = proc;
            g_save_route_acted_status = status;
            g_save_route_acted_retrace = retrace;
            save_route_press(&g_save_confirm_pulse);
            fprintf(stderr,
                    "[save-milestone] press=A proc=%u retrace=%llu\n",
                    (unsigned)proc, (unsigned long long)(retrace + 1u));
            return;
        }
        if (proc == 30u) {
            if (!g_save_route_right_sent) {
                g_save_route_acted_proc = proc;
                g_save_route_acted_status = status;
                g_save_route_acted_retrace = retrace;
                g_save_route_right_sent = true;
                // 5 is straight right: the second line of the prompt is the
                // one that quits, and the cursor starts on the first.
                save_route_press_stick(5u, 3u);
                g_save_route_next_retrace = retrace + 20u;
                fprintf(stderr,
                        "[save-milestone] press=stick-right quit-prompt "
                        "retrace=%llu\n",
                        (unsigned long long)(retrace + 1u));
                return;
            }
            if (retrace >= g_save_route_next_retrace) {
                save_route_press(&g_save_confirm_pulse);
                fprintf(stderr,
                        "[save-milestone] press=A quit-prompt retrace=%llu\n",
                        (unsigned long long)(retrace + 1u));
                g_save_route_state = BLUEWAKE_SAVE_STATE_QUIT;
                g_save_route_next_retrace = retrace;
            }
            return;
        }
        if (proc == 31u) {
            const u32 reset_data = mem_read32(cpu, 0x803F6968u);
            fprintf(stderr,
                    "[save-milestone] reset-requested retrace=%llu "
                    "mResetData=0x%08X mReset=%d card_writes=%llu "
                    "card_bytes=%llu\n",
                    (unsigned long long)retrace, reset_data,
                    reset_data >= 0x80000000u
                        ? (int)mem_read32(cpu, reset_data)
                        : -1,
                    (unsigned long long)bluewake_card_runtime_write_calls(),
                    (unsigned long long)bluewake_card_runtime_write_bytes());
            g_save_route_state = BLUEWAKE_SAVE_STATE_QUIT;
            g_save_route_next_retrace = retrace;
            return;
        }
        if (proc == 26u || proc == 22u || proc == 18u || proc == 16u ||
            proc == 17u || proc == 1u || proc == 23u || proc == 29u)
            return;
        if (retrace >= g_save_route_menu_retrace + 4000u) {
            save_route_finish("save-menu-timeout", retrace);
            return;
        }
        return;
    }

    if (g_save_route_state == BLUEWAKE_SAVE_STATE_QUIT) {
        const u32 reset_data = mem_read32(cpu, 0x803F6968u);
        if (g_save_route_next_retrace == 0u)
            g_save_route_next_retrace = retrace;
        if (retrace == g_save_route_next_retrace + 600u) {
            fprintf(stderr,
                    "[save-milestone] post-quit retrace=%llu pc=0x%08X "
                    "mResetData=0x%08X mReset=%d\n",
                    (unsigned long long)retrace, (unsigned)cpu->pc, reset_data,
                    reset_data >= 0x80000000u
                        ? (int)mem_read32(cpu, reset_data)
                        : -1);
            g_save_route_state = BLUEWAKE_SAVE_STATE_DONE;
        }
        return;
    }
}

static float host_f32(u32 bits) {
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static u32 host_f32_bits(f64 value) {
    const float single = (float)value;
    u32 bits;
    memcpy(&bits, &single, sizeof(bits));
    return bits;
}

static bool host_guest_cpad_a_released(CPUState* cpu) {
    // GZLE01 g_mDoCPd_cpadInfo[0].mButtonHold.a
    return (mem_read8(cpu, 0x803A4E20u) & 0x01u) == 0u;
}

static bool host_guest_cpad_start_released(CPUState* cpu) {
    // GZLE01 g_mDoCPd_cpadInfo[0].mButtonHold.start
    return (mem_read8(cpu, 0x803A4E21u) & 0x10u) == 0u;
}

static u32 host_find_scene_by_proc_name(CPUState* cpu, u16 proc_name) {
    // GZLE01 g_fopScnTg_SceneList contains scene tags whose data points back
    // to the owning scene process. Bound traversal protects diagnostics from
    // malformed guest lists without altering guest state.
    u32 node = mem_read32(cpu, 0x80372150u);
    for (unsigned i = 0u; i < 16u && node >= 0x80000000u; i++) {
        const u32 scene = mem_read32(cpu, node + 0x0Cu);
        const u32 next = mem_read32(cpu, node + 0x08u);
        if (scene >= 0x80000000u && mem_read16(cpu, scene + 0x08u) == proc_name)
            return scene;
        if (next == node)
            break;
        node = next;
    }
    return 0u;
}

// BLUEWAKE_PAD_TRACE=1: the channel-0 state the guest is given (buttons after
// the scripted and event presses), on change, so a closed-loop run such as a
// steered route can be replayed open loop elsewhere, for example as a Dolphin
// input movie (scripts/pad_trace_to_dtm.py).
static void host_pad_trace(u32 channel, const DolPadState* pad, u16 buttons) {
    static int trace = -1;
    static DolPadState last;
    static u16 last_buttons;
    static bool have_last;
    if (channel != 0u)
        return;
    if (trace < 0)
        trace = getenv("BLUEWAKE_PAD_TRACE") != NULL ? 1 : 0;
    if (trace == 0)
        return;
    if (have_last && last_buttons == buttons && last.stick_x == pad->stick_x &&
        last.stick_y == pad->stick_y && last.substick_x == pad->substick_x &&
        last.substick_y == pad->substick_y &&
        last.trigger_left == pad->trigger_left &&
        last.trigger_right == pad->trigger_right)
        return;
    fprintf(stderr,
            "[pad-trace] retrace=%llu button=0x%04X stick=%d,%d c=%d,%d l=%u "
            "r=%u\n",
            (unsigned long long)g_host_retrace_count, buttons, pad->stick_x,
            pad->stick_y, pad->substick_x, pad->substick_y, pad->trigger_left,
            pad->trigger_right);
    last = *pad;
    last_buttons = buttons;
    have_last = true;
}

static void host_si_complete_pad_transfer(u32 control) {
    const u32 channel = (control >> 1) & 3u;
    const u32 input_length = (control >> 8) & 0x7Fu;
    if (channel >= 4u || input_length == 0u)
        return;

    DolPadState live_pad[4] = {{0}};
    DolPadState merged_pad;
    if (g_live_pad_enabled) {
        (void)dol_platform_pad_read(live_pad);
        bluewake_mouse_camera_pad(&live_pad[0]);
    } else if (bluewake_mouse_camera_scripted()) {
        bluewake_mouse_camera_pad(&live_pad[0]); // BLUEWAKE_STICK_TEST without a controller
    }
    host_note_live_input(&live_pad[0]);
    bluewake_pad_merge(g_live_takeover ? &live_pad[channel] : &g_virtual_pad[channel],
                       &live_pad[channel], &merged_pad);
    const DolPadState effective_pad = host_pad_state(channel, &merged_pad);
    const DolPadState* pad = &effective_pad;
    const u16 buttons = host_pad_buttons(channel, pad->button);
    host_pad_trace(channel, pad, buttons);
    u32 data0 = 0x09000000u; // SI_GC_CONTROLLER
    u32 data1 = 0u;
    if (input_length >= 8u) {
        const u32 command = si_load_be32(g_si.regs, DOL_SI_IO_BUFFER_OFF);
        if (command == 0x41000000u || command == 0x42000000u) {
            // PADRead's origin/calibration transaction must not capture the
            // configured gameplay stick as the neutral origin.
            data0 = 0x09008080u;
            data1 = 0x80800000u;
        } else {
            bluewake_pad_wire_encode(pad, buttons, &data0, &data1);
        }
    }

    const u32 response = 0x04u + channel * 0x0Cu;
    si_store_be32(g_si.regs, response, data0);
    si_store_be32(g_si.regs, response + 4u, data1);
    // SI transfer completion copies guest input from the shared I/O buffer;
    // the per-channel response registers are consumed by polling reads.
    si_store_be32(g_si.regs, DOL_SI_IO_BUFFER_OFF, data0);
    si_store_be32(g_si.regs, DOL_SI_IO_BUFFER_OFF + 4u, data1);
    // A successful transfer replaces the channel error nibble; retaining a
    // stale NO_RESPONSE bit makes retail SIGetStatus classify the pad absent.
    g_si.regs[DOL_SI_STATUS_OFF + channel] =
        (g_si.regs[DOL_SI_STATUS_OFF + channel] & 0xF0u) | 0x20u;
    g_si.regs[DOL_SI_COMCSR_OFF] |= 0x10u;           // RDSTINT
    if (g_pad_trace && g_pad_transfer_reports < 32u) {
        fprintf(stderr,
                "[pad] si-response #%u retrace=%llu channel=%u input=%u "
                "buttons=0x%04X data0=0x%08X data1=0x%08X\n",
                g_pad_transfer_reports + 1u,
                (unsigned long long)g_host_retrace_count, channel,
                input_length, buttons, data0, data1);
        g_pad_transfer_reports++;
    }
}

/* BLUEWAKE_INPUT_LOG: when the game reads a changed pad on channel 0 (a
   button edge or a new stick direction), one line with the retrace. Beside
   the iOS shell's [touch] lines, the session log shows the time from a touch
   to the game seeing it. */
static bool g_input_log_enabled;

static int host_stick_direction(int x, int y) {
    if (x * x + y * y < 30 * 30) return 0;
    static const int sectors[3][3] = {{6, 7, 8}, {5, 0, 1}, {4, 3, 2}};
    const int col = x > 2 * abs(y) / 5 ? 2 : (x < -2 * abs(y) / 5 ? 0 : 1);
    const int row = y > 2 * abs(x) / 5 ? 0 : (y < -2 * abs(x) / 5 ? 2 : 1);
    return sectors[row][col];
}

static void host_input_log_read(const DolPadState* pad, u16 buttons) {
    static u16 last_buttons;
    static int last_dir;
    const int dir = host_stick_direction(pad->stick_x, pad->stick_y);
    if (buttons == last_buttons && dir == last_dir) return;
    fprintf(stderr, "[pad-read] retrace=%llu buttons=0x%04X pressed=0x%04X released=0x%04X stick_dir=%d\n",
            (unsigned long long)g_host_retrace_count, buttons,
            (unsigned)(buttons & ~last_buttons), (unsigned)(last_buttons & ~buttons), dir);
    last_buttons = buttons;
    last_dir = dir;
}

static void host_si_latch_pad_poll(void) {
    const u32 poll = ((u32)g_si.regs[DOL_SI_POLL_OFF] << 24) |
                     ((u32)g_si.regs[DOL_SI_POLL_OFF + 1u] << 16) |
                     ((u32)g_si.regs[DOL_SI_POLL_OFF + 2u] << 8) |
                     (u32)g_si.regs[DOL_SI_POLL_OFF + 3u];
    // SI stores the channel-enable nibble in bits 7..4 of the low byte in
    // hardware channel order (0x80 is channel 0); the runtime API and pad
    // array use ascending channel bits.
    const u32 channel_mask = ((poll >> 7) & 1u) |
                             (((poll >> 6) & 1u) << 1) |
                             (((poll >> 5) & 1u) << 2) |
                             (((poll >> 4) & 1u) << 3);
    if (g_pad_trace && g_pad_poll_reports < 8u) {
        const u32 comcsr = ((u32)g_si.regs[DOL_SI_COMCSR_OFF] << 24) |
                           ((u32)g_si.regs[DOL_SI_COMCSR_OFF + 1u] << 16) |
                           ((u32)g_si.regs[DOL_SI_COMCSR_OFF + 2u] << 8) |
                           (u32)g_si.regs[DOL_SI_COMCSR_OFF + 3u];
        fprintf(stderr,
                "[pad] si-poll #%u register=0x%08X channel-mask=0x%X "
                "comcsr=0x%08X\n",
                g_pad_poll_reports + 1u, poll, channel_mask, comcsr);
        g_pad_poll_reports++;
    }
    if (channel_mask == 0u)
        return;

    DolPadState live_pad[4] = {{0}};
    if (g_live_pad_enabled) {
        (void)dol_platform_pad_read(live_pad);
        bluewake_mouse_camera_pad(&live_pad[0]);
    } else if (bluewake_mouse_camera_scripted()) {
        bluewake_mouse_camera_pad(&live_pad[0]); // BLUEWAKE_STICK_TEST without a controller
    }
    host_note_live_input(&live_pad[0]);
    for (u32 channel = 0; channel < 4u; channel++) {
        if ((channel_mask & (1u << channel)) == 0u)
            continue;
        DolPadState merged_pad;
        bluewake_pad_merge(g_live_takeover ? &live_pad[channel] : &g_virtual_pad[channel],
                           &live_pad[channel], &merged_pad);
        const DolPadState effective_pad = host_pad_state(channel, &merged_pad);
        const DolPadState* pad = &effective_pad;
        const u16 buttons = host_pad_buttons(channel, pad->button);
        host_pad_trace(channel, pad, buttons);
        if (g_input_log_enabled && channel == 0u)
            host_input_log_read(pad, buttons);
        if (g_pad_trace && channel == 0u && buttons != 0u &&
            g_pad_active_reports < 16u) {
            fprintf(stderr,
                    "[pad] si-latch #%u retrace=%llu buttons=0x%04X\n",
                    g_pad_active_reports + 1u,
                    (unsigned long long)g_host_retrace_count, buttons);
            g_pad_active_reports++;
        }
        u32 data0;
        u32 data1;
        bluewake_pad_wire_encode(pad, buttons, &data0, &data1);
        if (g_pad_trace && channel == 0u && g_pad_poll_reports < 16u) {
            fprintf(stderr,
                    "[pad] si-payload retrace=%llu raw_stick=%d,%d "
                    "data0=0x%08X data1=0x%08X\n",
                    (unsigned long long)g_host_retrace_count,
                    pad->stick_x, pad->stick_y, data0, data1);
        }
        const u32 response = 0x04u + channel * 0x0Cu;
        si_store_be32(g_si.regs, response, data0);
        si_store_be32(g_si.regs, response + 4u, data1);
        g_si.regs[DOL_SI_STATUS_OFF + channel] =
            (g_si.regs[DOL_SI_STATUS_OFF + channel] & 0xF0u) | 0x20u;
    }
    g_si.regs[DOL_SI_COMCSR_OFF] |= 0x10u; // RDSTINT
}

static void host_mmio_write(CPUState* ctx, u32 address, u64 value, u8 size) {
#if BLUEWAKE_EDGE_CENSUS
    g_mmio_write_buckets[host_mmio_bucket(address)]++;
#endif
    // The gather pipe first, without the device sync. A FIFO write is bytes
    // for the GX path and nothing else: no backend's gx_write touches a device
    // clock, a deadline or an interrupt source (the PE finish is committed at
    // the draw-done intercept), so observing the cycle domain, syncing every
    // device and refreshing the interrupt sources around it changed nothing
    // they could see - and it was most of host_mmio_write, 10 percent of the
    // game thread in the simulator's heavy Outset view, because the game's GX
    // and J3D code writes the pipe several times per matrix and per draw.
    if (address >= 0xCC008000u && address < 0xCC008020u && !g_gx_fifo_trace) {
        dol_platform_gx_write(value, size);
        return;
    }
    if (bluewake_external_memory_write(ctx, address, value, size))
        return;
    if (bluewake_ipl_sram_contains(&g_ipl_sram, address) && size == 4u) {
        bluewake_ipl_sram_write(&g_ipl_sram, address, (u32)value,
                                host_ipl_sram_read8, host_ipl_sram_write8, ctx);
        return;
    }

    (void)bluewake_cycle_domain_observe(
        &g_cycle_domain, ctx, ctx->cycle_observation_suffix);
    host_sync_cycle_devices(ctx);
    // A device register access can raise or acknowledge an interrupt after the
    // sync above has already published; the block boundary is where that was
    // published before the gate existed, so mark it here rather than at entry.
    g_interrupt_sources_dirty = true;

    if (address >= 0xCC008000u && address < 0xCC008020u) {
        static unsigned gx_fifo_trace_reports = 0;
        if (g_gx_fifo_trace &&
            gx_fifo_trace_reports < 512u) {
            fprintf(stderr,
                    "[gx-fifo] address=0x%08X value=0x%016llX size=%u pc=0x%08X\n",
                    address, (unsigned long long)value, size, ctx->pc);
            gx_fifo_trace_reports++;
        }
        dol_platform_gx_write(value, size);
        goto rebudget;
    }
    if (dol_di_mmio_contains(address)) {
        dol_di_mmio_write(&g_di, ctx, address, size, value);
        goto rebudget;
    }
    if (dol_interrupts_mmio_contains(address)) {
        if (address == 0xCC002030u && size == 2u && g_vi_ack_reports < 16u) {
            fprintf(stderr,
                    "[vi] status-write pc=0x%08X value=0x%04X before=0x%04X "
                    "srr0=0x%08X srr1=0x%08X msr=0x%08X\n",
                    ctx->pc, (u16)value,
                    (u16)dol_interrupts_mmio_read(&g_interrupts, address, size),
                    ctx->srr0, ctx->srr1, ctx->msr);
        }
        dol_interrupts_mmio_write(&g_interrupts, address, size, value);
        if (address == 0xCC002030u && size == 2u && g_vi_ack_reports < 16u) {
            fprintf(stderr, "[vi] status-after=0x%04X\n",
                    (u16)dol_interrupts_mmio_read(&g_interrupts, address, size));
            g_vi_ack_reports++;
        }
        goto rebudget;
    }
    if (dol_si_mmio_contains(address)) {
        dol_si_mmio_write(&g_si, address, size, value);
        // SIC<n>OUTBUF (0x00, 0x0C, 0x18, 0x24): the command word the pad
        // library sends each poll, whose low two bits are the rumble motor
        // (PAD_MOTOR_STOP 0, RUMBLE 1, STOP_HARD 2). Forward a change to the
        // platform, which drives the controller's rumble (Aurora
        // PADControlMotor). Nothing read these bits before, so rumble never
        // reached a controller.
        if (size == 4u && address - DOL_SI_BASE < 0x30u &&
            (address - DOL_SI_BASE) % 0x0Cu == 0u) {
            static u8 s_motor[4];
            const u32 channel = (address - DOL_SI_BASE) / 0x0Cu;
            const u8 motor = (u8)((u32)value & 3u);
            // The command is bits 16-23 (PADControlMotor: (0x40 << 16) |
            // AnalogMode | motor); a check of the top byte never matched.
            if ((((u32)value >> 16) & 0xFFu) == 0x40u && motor != s_motor[channel]) {
                s_motor[channel] = motor;
                // Enhanced haptics render the game's vibration from what it
                // asked for (haptics.c) instead of these on-off bits.
                if (bluewake_haptics_forward_motor())
                    dol_platform_pad_control_motor(channel, motor);
                if (g_input_log_enabled)
                    fprintf(stderr, "[rumble] channel=%u motor=%u retrace=%llu\n", channel,
                            (unsigned)motor, (unsigned long long)g_host_retrace_count);
            }
        }
        if (address == DOL_SI_BASE + DOL_SI_COMCSR_OFF && size == 4u &&
            ((u32)value & DOL_SI_TSTART) != 0u)
            host_si_complete_pad_transfer((u32)value);
        if (address == DOL_SI_BASE + DOL_SI_POLL_OFF && size == 4u &&
            (u32)value != 0u)
            host_si_latch_pad_poll();
        goto rebudget;
    }
    if (address >= 0xCC006C00u && address < 0xCC006C20u) {
        const u32 offset = address - 0xCC006C00u;
        dol_audio_dma_ai_mmio_write(&g_audio_dma, offset, size, value);
        if (g_audio_object_watch && g_audio_ai_register_reports < 32u) {
            fprintf(stderr,
                    "[audio-ai] write offset=0x%02X size=%u value=0x%04llX "
                    "sample_rate=%u\n",
                    offset, size, (unsigned long long)value,
                    dol_audio_dma_sample_rate(&g_audio_dma));
            g_audio_ai_register_reports++;
        }
        goto rebudget;
    }
    if (address >= 0xCC005000u && address < 0xCC005040u) {
        const u32 offset = address - 0xCC005000u;
        if (bluewake_aram_dma_contains(address)) {
            const u64 previous_count = g_aram_dma.transfer_count;
            const bool accepted = bluewake_aram_dma_write(
                &g_aram_dma, ctx, address, size, value);
            if (!accepted) {
                fprintf(stderr,
                        "[aram-dma] rejected register write address=0x%08X "
                        "size=%u value=0x%08llX pending=%u\n",
                        address, size, (unsigned long long)value,
                        bluewake_aram_dma_interrupt_pending(&g_aram_dma) ? 1u
                                                                        : 0u);
            } else if (g_aram_dma.transfer_count != previous_count &&
                       g_aram_dma_completion_reports < 40u) {
                fprintf(stderr,
                        "[aram-dma] committed direction=%u main=0x%08X "
                        "aram=0x%08X length=%u raise=DSP\n",
                        g_aram_dma.last_direction,
                        g_aram_dma.last_main_address,
                        g_aram_dma.last_aram_address,
                        g_aram_dma.last_length);
                g_aram_dma_completion_reports++;
            }
            goto rebudget;
        }
        if (address == 0xCC005000u) {
            g_dsp_mail_to_high_value = (u16)value;
            g_dsp_mail_to_high_seen = true;
            if (g_dsp_task_request_armed && g_dsp_boot_task_ready &&
                ((u16)value & 0xFF00u) == 0x8200u)
                g_dsp_audio_frame_words_remaining = 3u;
            if ((u32)value == 0u)
                g_dsp_boot_mail_armed = true;
            if (g_audio_object_watch && g_dsp_mail_to_reports < 24u) {
                fprintf(stderr,
                        "[audio-dsp] mail-to-high=0x%04llX pc=0x%08X\n",
                        (unsigned long long)value, ctx->pc);
                g_dsp_mail_to_reports++;
            }
            goto rebudget;
        }
        if (address == 0xCC005002u) {
            const bool audio_frame_word =
                g_dsp_audio_frame_words_remaining > 0u ||
                (g_dsp_mail_to_high_value & 0xFF00u) == 0x8200u;
            if (g_audio_object_watch && g_dsp_mail_to_reports < 24u) {
                fprintf(stderr,
                        "[audio-dsp] mail-to-low=0x%04llX pc=0x%08X\n",
                        (unsigned long long)value, ctx->pc);
                g_dsp_mail_to_reports++;
            }
            if (g_dsp_mail_to_high_seen) {
#ifdef BLUEWAKE_HAS_DSP_ADAPTER
                if (g_dsp_adapter != NULL) {
                    bluewake_dsp_adapter_write_cpu_mailbox(
                        g_dsp_adapter,
                        ((u32)g_dsp_mail_to_high_value << 16) | (u16)value);
                }
#endif
                if (audio_frame_word && g_dsp_audio_frame_trace_reports < 24u) {
                    fprintf(stderr,
                            "[audio-dsp-frame-word] value=0x%08X pc=0x%08X "
                            "remaining_before=%u\n",
                            ((u32)g_dsp_mail_to_high_value << 16) | (u16)value,
                            ctx->pc, g_dsp_audio_frame_words_remaining);
                    g_dsp_audio_frame_trace_reports++;
                }
                const bool boot_completion_pending =
                    g_dsp_mail_from_pending && g_dsp_mail_from == 0xDCD10000u;
                if (!boot_completion_pending && !g_dsp_task_request_pending) {
                    g_dsp_mail_from_pending = false;
                    g_dsp_mail_reads_remaining = 0u;
                }
                if (g_dsp_mail_to_high_value == 0x80F3u &&
                    (u16)value == 0xD001u && !g_dsp_boot_task_ready) {
                    // __DSP_boot_task completes by raising DCD10000. Defer
                    // the task request until that interrupt establishes the
                    // registered current task.
                    g_dsp_mail_from = 0xDCD10000u;
                    g_dsp_mail_reads_remaining = 1u;
                    g_dsp_mail_from_pending = true;
                }
                g_dsp_mail_to_high_seen = false;
            }
            if (g_dsp_audio_frame_words_remaining > 0u) {
                g_dsp_audio_frame_words_remaining--;
                if (g_dsp_audio_frame_words_remaining == 0u)
                    host_queue_dsp_task_request();
            }
            goto rebudget;
        }
        if (address == 0xCC00500Au && size == 2u) {
            const u16 command = (u16)value;
#ifdef BLUEWAKE_HAS_DSP_ADAPTER
            if (g_dsp_adapter != NULL) {
                if (bluewake_dsp_adapter_is_hle(g_dsp_adapter) &&
                    (command & 0x0080u) != 0u) {
                    // Write-one-to-clear DSPINT: the guest acknowledged it.
                    g_dsp_adapter_interrupt_pending = false;
                    g_interrupt_sources_dirty = true;
                }
                bluewake_dsp_adapter_write_control(g_dsp_adapter, command);
                const u16 adapter_control =
                    bluewake_dsp_adapter_read_control(g_dsp_adapter);
                g_dsp_control =
                    (u16)((command & (u16)~DSP_LLE_CONTROL_MASK) |
                          (adapter_control & DSP_LLE_CONTROL_MASK));
            } else
#endif
            {
                g_dsp_control = command;
            }
            // Bit 7 is the DSP-DSP interrupt status observed by
            // __OSDispatchInterrupt, not persistent control state. Keep the
            // request-driven status in the effective read below so an
            // acknowledged mailbox cannot retrigger __DSPHandler forever.
            g_dsp_control &= (u16)~0x0080u;
            dol_audio_dma_dsp_mmio_write(&g_audio_dma, offset, size,
                                         (u16)(command & (u16)~0x0001u));
            if (command == 0x0952u && g_dsp_boot_task_ready)
                g_dsp_task_request_armed = true;
            if (!g_dsp_task_handshake_sent && command == 0x0960u) {
                // The DSP task interrupt first delivers its request token to
                // __DSPHandler; that handler then calls syncDSP, which reads
                // the following JAS-prefixed completion word.
                g_dsp_task_request_armed = true;
                if (g_dsp_boot_task_ready) {
                    g_dsp_mail_from = 0xDCD10004u;
                    g_dsp_mail_reads_remaining = 1u;
                    g_dsp_mail_from_pending = true;
                    g_dsp_task_request_pending = true;
                    g_dsp_task_request_armed = false;
                }
                g_dsp_task_handshake_sent = true;
            }
            if ((command & 0x0001u) != 0u) {
                // DSP reset/release is a command; hardware clears the reset
                // bit after each transition completes.
                g_dsp_control = command & (u16)~0x0001u;
                if (!g_dsp_aram_complete) {
                    g_dsp_aram_complete = true;
                    fprintf(stderr,
                            "[mmio] DSP ARAM init complete control=0x%04X\n",
                            command);
                }
            }
            if (bluewake_aram_dma_interrupt_pending(&g_aram_dma) &&
                (command & 0x0020u) != 0u) {
                bluewake_aram_dma_acknowledge(&g_aram_dma);
                if (g_aram_dma_ack_reports < 40u) {
                    fprintf(stderr,
                            "[aram] DSP DMA interrupt acknowledged\n");
                    g_aram_dma_ack_reports++;
                }
            }
            if (g_audio_object_watch && g_audio_dsp_register_reports < 48u) {
                fprintf(stderr,
                        "[audio-dsp] control-write value=0x%04X "
                        "guest_control=0x%04X dma_pending=%u init-src="
                        "%04X%04X\n",
                        command, g_dsp_control,
                        dol_audio_dma_dsp_interrupt_pending(&g_audio_dma) ? 1u : 0u,
                        mem_read16(ctx, 0x81000000u),
                        mem_read16(ctx, 0x81000002u));
                g_audio_dsp_register_reports++;
            }
            goto rebudget;
        }
        dol_audio_dma_dsp_mmio_write(&g_audio_dma, offset, size, value);
#ifdef BLUEWAKE_HAS_DSP_ADAPTER
        if (g_dsp_adapter != NULL && size == 2u) {
            if (g_dsp_dma_guest_trace_reports < 48u &&
                (offset == 0x30u || offset == 0x32u || offset == 0x34u ||
                 offset == 0x36u)) {
                fprintf(stderr,
                        "[audio-dsp-guest-dma] pc=0x%08X offset=0x%02X "
                        "value=0x%04llX r3=0x%08X r4=0x%08X r5=0x%08X\n",
                        ctx->pc, offset, (unsigned long long)value,
                        ctx->gpr[3], ctx->gpr[4], ctx->gpr[5]);
                g_dsp_dma_guest_trace_reports++;
            }
            switch (offset) {
            case 0x30u:
                bluewake_dsp_adapter_write_ifx(g_dsp_adapter, 0xCEu,
                                                (u16)value);
                break;
            case 0x32u:
                bluewake_dsp_adapter_write_ifx(g_dsp_adapter, 0xCFu,
                                                (u16)value);
                break;
            case 0x34u:
                bluewake_dsp_adapter_write_ifx(g_dsp_adapter, 0xCDu,
                                                (u16)value);
                break;
            case 0x36u:
                bluewake_dsp_adapter_write_ifx(g_dsp_adapter, 0xCBu,
                                                (u16)value);
                break;
            default:
                break;
            }
        }
#endif
        if (g_audio_object_watch && g_audio_dsp_register_reports < 48u &&
            (offset == 0x30u || offset == 0x32u || offset == 0x36u)) {
            fprintf(stderr,
                    "[audio-dsp] dma-write offset=0x%02X value=0x%04llX "
                    "control=0x%04X blocks=%u pending=%u\n",
                    offset, (unsigned long long)value,
                    dol_audio_dma_read_control(&g_audio_dma),
                    dol_audio_dma_blocks_left(&g_audio_dma),
                    dol_audio_dma_dsp_interrupt_pending(&g_audio_dma) ? 1u : 0u);
            g_audio_dsp_register_reports++;
        }
        goto rebudget;
    }

rebudget:
    // The write itself may arm a nearer AI, DSP, VI, or PI deadline. Publish
    // that distance before generated code decides whether its prepaid suffix
    // is still safe to execute.
    host_refresh_interrupt_sources(ctx);
}

static bool host_graphics_guest_resolve_uncached(
    CPUState* cpu, u32 address, u32 size, DolGuestAddressSpace space,
    DolGuestResourceKind resource, const void** data, u32* available, bool* any_size);

static bool host_graphics_guest_resolve(
    void* user, u32 address, u32 size, DolGuestAddressSpace space,
    DolGuestResourceKind resource, const void** data, u32* available) {
    CPUState* cpu = (CPUState*)user;
    if (cpu == NULL || data == NULL || available == NULL)
        return false;
    // The translation worker resolves the same arrays, textures and display
    // lists draw after draw: 7 percent of it at native 60 Hz. A result depends
    // only on the address, size and space and on the alias registry, so it is
    // kept, per thread, until the registry changes (g_guest_alias_changes).
    // Where no alias holds the address, no alias holds any range from it, and
    // the result is the memory from the address whatever the size (a range
    // that does not fit it fails): such an entry answers every size (size 0
    // in the entry). A vertex array's size is its indexed span, which changes
    // from draw to draw, so keyed by size each draw missed, and took the
    // registry's lock.
    typedef struct GraphicsResolveEntry {
        u32 address, size, space, changes;
        const void* data;
        u32 available;
    } GraphicsResolveEntry;
    static _Thread_local GraphicsResolveEntry resolved[256];
    const u32 changes = g_guest_alias_changes;
    GraphicsResolveEntry* const entry = &resolved[((address >> 5) ^ (address >> 13)) & 255u];
    if (entry->data != NULL && entry->address == address && entry->space == (u32)space &&
        entry->changes == changes && (entry->size == 0u || entry->size == size)) {
        if (size == 0u || size > entry->available)
            return false;
        *data = entry->data;
        *available = entry->available;
        return true;
    }
    bool any_size = false;
    if (!host_graphics_guest_resolve_uncached(cpu, address, size, space, resource, data, available, &any_size))
        return false;
    *entry = (GraphicsResolveEntry){address, any_size ? 0u : size, (u32)space, changes, *data, *available};
    return true;
}

static bool host_graphics_guest_resolve_uncached(
    CPUState* cpu, u32 address, u32 size, DolGuestAddressSpace space,
    DolGuestResourceKind resource, const void** data, u32* available, bool* any_size) {
    DolGuestAddressResolver resolver;
    DolGuestResolvedRange range;
    // REL modules run at linked addresses from 0xC0400000, inside the range
    // the GX resolver otherwise reads as MEM1's uncached mirror. A display
    // list or vertex array a module keeps in its own data (Wind Waker's
    // d_a_majuu_flag in Hyrule, 0xC0B928C0) was read from unrelated RAM at
    // 0x80B928C0 and failed to parse. The CPU resolves those addresses through
    // the alias registry; resolve graphics reads the same way.
    {
        u8* alias = NULL;
        u32 alias_offset = 0u;
        // The translation worker's thread: see g_guest_alias_lock.
        pthread_mutex_lock(&g_guest_alias_lock);
        const bool aliased = ppc_guest_alias_resolve(address, size, &alias, &alias_offset);
        // Whether an alias holds the address at all: if none does, none holds
        // a range from it of any size, and the result below is every size's.
        u8* held = NULL;
        u32 held_offset = 0u;
        *any_size = !aliased && !ppc_guest_alias_resolve(address, 1u, &held, &held_offset);
        pthread_mutex_unlock(&g_guest_alias_lock);
        if (aliased && alias != NULL) {
            *data = alias;
            *available = size;
            return true;
        }
    }
    dol_guest_address_resolver_init(&resolver, NULL, cpu);
    if (!dol_guest_address_resolver_resolve(&resolver, address, size, space,
                                            resource, &range))
        return false;
    *data = range.data;
    *available = range.available;
    return true;
}

static bool host_audio_dma_read_guest(void* user, u32 source_address,
                                      u8* data, u32 size) {
    CPUState* cpu = (CPUState*)user;
    if (cpu == NULL || data == NULL || source_address > cpu->ram_size ||
        size > cpu->ram_size - source_address)
        return false;
    const u32 guest_address = 0x80000000u | source_address;
    for (u32 i = 0; i < size; i++)
        data[i] = mem_read8(cpu, guest_address + i);
    if (!bluewake_audio_capture_append_be16_stereo(
            &g_audio_capture, data, size,
            dol_audio_dma_sample_rate(&g_audio_dma)) &&
        !g_audio_capture_failure_reported && g_audio_capture.path != NULL &&
        g_audio_capture.path[0] != '\0') {
        fprintf(stderr, "[audio-capture] write failed path=\"%s\"\n",
                g_audio_capture.path);
        g_audio_capture_failure_reported = true;
    }
    return true;
}

static u64 host_cycle_cursor_delta(u64* cursor) {
    const u64 target = g_cycle_domain.absolute_cycles;
    if (*cursor >= target)
        return 0u;
    const u64 delta = target - *cursor;
    *cursor = target;
    return delta;
}

static void host_sync_dsp_cycles(void) {
    const u64 elapsed_cycles = host_cycle_cursor_delta(&g_dsp_cycle_cursor);
#ifdef BLUEWAKE_HAS_DSP_ADAPTER
    if (g_dsp_adapter != NULL && elapsed_cycles != 0u)
        host_dsp_advance_schedule(elapsed_cycles);
#else
    (void)elapsed_cycles;
#endif
}

static void host_sync_audio_cycles(CPUState* cpu) {
    const u64 elapsed_cycles = host_cycle_cursor_delta(&g_audio_cycle_cursor);
    if (elapsed_cycles == 0u)
        return;
    dol_audio_dma_advance_stream(&g_audio_dma, elapsed_cycles);
    if (!dol_audio_dma_consume_pcm16_stereo_work(
            &g_audio_dma, elapsed_cycles, host_audio_dma_read_guest, cpu))
        return;
    if (g_audio_object_watch &&
        (g_audio_dma_chunk_reports < 16u ||
         dol_audio_dma_interrupt_pending(&g_audio_dma))) {
        u64 dsp_control = 0u;
        dol_audio_dma_dsp_mmio_read(
            &g_audio_dma, DOL_AUDIO_DMA_DSP_CONTROL_OFF, 2u, &dsp_control);
        fprintf(stderr,
                "[audio-dma] chunk=%u source=0x%08X rate=%u left=%u "
                "pending=%u gated=%u dsp_control=0x%04llX blocks=%llu\n",
                g_audio_dma_chunk_reports + 1u,
                g_audio_dma.current_source_address >= 32u
                    ? g_audio_dma.current_source_address - 32u
                    : g_audio_dma.current_source_address,
                dol_audio_dma_sample_rate(&g_audio_dma),
                dol_audio_dma_blocks_left(&g_audio_dma),
                dol_audio_dma_interrupt_pending(&g_audio_dma) ? 1u : 0u,
                dol_audio_dma_dsp_interrupt_pending(&g_audio_dma) ? 1u : 0u,
                (unsigned long long)dsp_control,
                (unsigned long long)g_current_host_block);
        g_audio_dma_chunk_reports++;
    }
}

// Opt-in input-chain probe, gated by the same BLUEWAKE_INPUT_PROBE switch as
// the pad-layer probe. The pad probe proved SDL delivers the key and that
// PADRead maps it; this one prints the guest's own pad record and the SI word
// the guest will read, on change, so a press can be followed all the way to
// the value the game itself keeps. Inert when the variable is unset.
static void host_input_chain_probe(CPUState* cpu) {
    static int probe_enabled = -1;
    if (probe_enabled < 0)
        probe_enabled = getenv("BLUEWAKE_INPUT_PROBE") != NULL ? 1 : 0;
    if (probe_enabled == 0)
        return;
    static u32 last_hold = 0xFFFFFFFFu;
    static u32 last_trig = 0xFFFFFFFFu;
    static u32 last_si = 0xFFFFFFFFu;
    static u32 last_jut_hold = 0xFFFFFFFFu;
    static u32 last_jut_trig = 0xFFFFFFFFu;
    static u32 last_name_scene = 0xFFFFFFFFu;
    // GZLE01 g_mDoCPd_cpadInfo[0] lives at 0x803A4E20: a button word whose
    // high half is the GameCube button mask (A = 0x0100), then the trigger
    // word. These are the values the game's own controller code reads.
    const u32 hold = mem_read32(cpu, 0x803A4E20u) >> 16;
    const u32 trig = mem_read32(cpu, 0x803A4E22u) >> 16;
    // The title also consults a second pad record reached through the pointer
    // at 0x803A4DE0, with its hold word at +0x18 and its trigger at +0x1C.
    const u32 game_pad = mem_read32(cpu, 0x803A4DE0u);
    const u32 jut_hold =
        game_pad >= 0x80000000u ? mem_read32(cpu, game_pad + 0x18u) : 0u;
    const u32 jut_trig =
        game_pad >= 0x80000000u ? mem_read32(cpu, game_pad + 0x1Cu) : 0u;
    // Which foreground scene is current when the change happened. This is what
    // turns a dropped press into a press that arrived while a known scene ran.
    const u32 name_scene = host_find_scene_by_proc_name(cpu, 0x000Eu);
    const u32 si_word = ((u32)g_si.regs[DOL_SI_IO_BUFFER_OFF] << 24) |
                        ((u32)g_si.regs[DOL_SI_IO_BUFFER_OFF + 1u] << 16) |
                        ((u32)g_si.regs[DOL_SI_IO_BUFFER_OFF + 2u] << 8) |
                        (u32)g_si.regs[DOL_SI_IO_BUFFER_OFF + 3u];
    if (hold == last_hold && trig == last_trig && si_word == last_si &&
        jut_hold == last_jut_hold && jut_trig == last_jut_trig &&
        name_scene == last_name_scene)
        return;
    last_hold = hold;
    last_trig = trig;
    last_si = si_word;
    last_jut_hold = jut_hold;
    last_jut_trig = jut_trig;
    last_name_scene = name_scene;
    const u32 poll = ((u32)g_si.regs[DOL_SI_POLL_OFF] << 24) |
                     ((u32)g_si.regs[DOL_SI_POLL_OFF + 1u] << 16) |
                     ((u32)g_si.regs[DOL_SI_POLL_OFF + 2u] << 8) |
                     (u32)g_si.regs[DOL_SI_POLL_OFF + 3u];
    fprintf(stderr,
            "[input-chain] retrace=%llu cpad_hold=0x%04X cpad_trig=0x%04X "
            "jut_hold=0x%04X jut_trig=0x%04X name_scene=0x%08X "
            "si_word=0x%08X si_poll=0x%08X si_status=%02X%02X%02X%02X "
            "cpad_words=0x%08X,0x%08X\n",
            (unsigned long long)g_host_retrace_count, hold, trig, jut_hold,
            jut_trig, name_scene, si_word, poll,
            g_si.regs[DOL_SI_STATUS_OFF], g_si.regs[DOL_SI_STATUS_OFF + 1u],
            g_si.regs[DOL_SI_STATUS_OFF + 2u],
            g_si.regs[DOL_SI_STATUS_OFF + 3u], mem_read32(cpu, 0x803A4E20u),
            mem_read32(cpu, 0x803A4E24u));
    // The new-file walk is the first place a real key run has to drive a screen
    // the route only ever drove with synthetic pulses, so "the guest ignored the
    // press" and "the guest was never on the screen we thought" look identical
    // from outside. They are not identical from inside: the route's own name
    // driver gates on this exact tuple (the scene's active proc index at +0x554,
    // the name record at +0x424, and inside that record sel_proc +0x2903,
    // sel_menu +0x2904, cur_pos +0x2907 and the name-input-complete byte
    // +0x290B), but it can only run while its pulses are armed, which is never
    // true when the keys are real. Printing the tuple on change costs nothing
    // while the screen sits still and makes the stall self-describing.
    static u32 last_name_state = 0xFFFFFFFFu;
    if (g_name_scene_object >= 0x80000000u) {
        const u8 main_proc = mem_read8(cpu, g_name_scene_object + 0x554u);
        const u32 name = mem_read32(cpu, g_name_scene_object + 0x424u);
        const u8 sel_proc =
            name >= 0x80000000u ? mem_read8(cpu, name + 0x2903u) : 0xEEu;
        const u8 sel_menu =
            name >= 0x80000000u ? mem_read8(cpu, name + 0x2904u) : 0xEEu;
        const u8 cur_pos =
            name >= 0x80000000u ? mem_read8(cpu, name + 0x2907u) : 0xEEu;
        const u8 name_done =
            name >= 0x80000000u ? mem_read8(cpu, name + 0x290Bu) : 0xEEu;
        const u32 state = ((u32)main_proc << 24) | ((u32)sel_proc << 16) |
                          ((u32)sel_menu << 8) | (u32)cur_pos;
        if (state != last_name_state) {
            last_name_state = state;
            fprintf(stderr,
                    "[name-scene-state] retrace=%llu main_proc=%u "
                    "sel_proc=%u sel_menu=%u cur_pos=%u name_done=%u "
                    "scene=0x%08X name=0x%08X\n",
                    (unsigned long long)g_host_retrace_count, main_proc,
                    sel_proc, sel_menu, cur_pos, name_done,
                    g_name_scene_object, name);
        }
    }
}

static void host_sync_vi_cycles(CPUState* cpu) {
    const u64 elapsed_cycles = host_cycle_cursor_delta(&g_vi_cycle_cursor);
    if (g_cycle_vi_clock == NULL || elapsed_cycles == 0u)
        return;
    dol_vi_clock_advance(g_cycle_vi_clock, elapsed_cycles);
    while (dol_vi_clock_pop_retrace(g_cycle_vi_clock, NULL)) {
        g_host_retrace_count++;
        bluewake_simulation_retrace(g_host_retrace_count);
        aurora_backend_service_present();
        host_mods_reapply(cpu);
        bluewake_game_options_retrace(cpu);
        bluewake_mouse_camera_retrace();
        bluewake_jump_button_retrace();
        bluewake_sprint_retrace();
        bluewake_fps_watch_retrace();
        bluewake_fast_load_retrace(bluewake_host_thread_cpu_us());
        bluewake_quick_doors_retrace();
        bluewake_haptics_retrace();
        if (g_wall_pace_enabled && !bluewake_fast_load_fast_forward())
            host_wall_pace(g_host_retrace_count);
        if (g_perf_log_enabled)
            perf_note_retrace(g_host_retrace_count);
        if (g_frame_timing_enabled) {
            struct timespec frame_now;
            clock_gettime(CLOCK_MONOTONIC, &frame_now);
            fprintf(stderr, "[frame-timing] retrace=%llu us=%llu\n",
                    (unsigned long long)g_host_retrace_count,
                    (unsigned long long)((u64)frame_now.tv_sec * 1000000ull +
                                         (u64)frame_now.tv_nsec / 1000ull));
        }
                host_si_latch_pad_poll();
                host_input_chain_probe(cpu);
                dol_interrupts_assert_vi_retrace(&g_interrupts);
                if (g_delivery_safety_census_enabled &&
                    g_host_retrace_count >= 700u && g_host_retrace_count <= 900u) {
                    const u32 probe_object = mem_read32(cpu, 0x803F6160u);
                    fprintf(stderr,
                            "[overlap-probe] retrace=%llu object=0x%08X live=%d "
                            "phase=%d pid=0x%08X\n",
                            (unsigned long long)g_host_retrace_count, probe_object,
                            probe_object >= 0x80000000u
                                ? (s32)mem_read16(cpu, probe_object + 0x04u) : -1,
                            probe_object >= 0x80000000u
                                ? (s32)mem_read32(cpu, probe_object + 0x1Cu) : -1,
                            probe_object >= 0x80000000u
                                ? mem_read32(cpu, probe_object + 0x30u) : 0u);
                }
        if (g_vi_assert_reports < 16u) {
            const u64 retrace_timebase_delta =
                cpu->timebase - g_previous_retrace_timebase;
            fprintf(stderr,
                    "[vi] guest-clock retrace at block=%llu msr=0x%08X "
                    "cause=0x%08X mask=0x%08X vi_di0=0x%04X "
                    "timebase=%llu delta=%llu\n",
                    (unsigned long long)(g_current_host_block + 1u), cpu->msr,
                    dol_interrupts_pi_cause(&g_interrupts),
                    dol_interrupts_pi_mask(&g_interrupts),
                    (u16)dol_interrupts_mmio_read(&g_interrupts,
                                                 0xCC002030u, 2u),
                    (unsigned long long)cpu->timebase,
                    (unsigned long long)retrace_timebase_delta);
            g_previous_retrace_timebase = cpu->timebase;
            g_vi_assert_reports++;
        }
    }
}

static void host_sync_cycle_devices(CPUState* cpu) {
    g_interrupt_sources_dirty = true;
    host_sync_dsp_cycles();
    host_sync_audio_cycles(cpu);
    host_sync_vi_cycles(cpu);
    // Device advancement can create or clear an interrupt at this exact
    // instruction boundary. Publish sources and the resulting deadline as
    // one operation before generated execution can resume.
    host_refresh_interrupt_sources(cpu);
}

static void host_sync_cycle_devices_end_turn(CPUState* cpu) {
    g_interrupt_sources_dirty = true;
    host_sync_dsp_cycles();
    host_sync_audio_cycles(cpu);
    host_sync_vi_cycles(cpu);
    // Publish device state now; prepare_dispatch computes the only budget that
    // translated code can consume on the next turn.
    host_publish_interrupt_sources();
}

#ifdef BLUEWAKE_HAS_DSP_ADAPTER
static u8 host_dsp_read_guest(void* user, u32 address) {
    CPUState* cpu = (CPUState*)user;
    // DSP task DMA carries the physical MEM1 address (for example
    // 0x00399420 for jdsp), while GXRuntime exposes the cached guest alias.
    const u32 guest_address = address < 0x80000000u
                                  ? address | 0x80000000u
                                  : address;
    const u8 value = mem_read8(cpu, guest_address);
    if (g_audio_object_watch && g_dsp_adapter_guest_read_reports < 32u) {
        fprintf(stderr,
                "[dsp-lle] guest-read address=0x%08X value=0x%02X "
                "pc=0x%08X\n",
                guest_address, value, cpu->pc);
        g_dsp_adapter_guest_read_reports++;
    }
    return value;
}

static void host_dsp_write_guest(void* user, u32 address, u8 value) {
    const u32 guest_address = address < 0x80000000u
                                  ? address | 0x80000000u
                                  : address;
    mem_write8((CPUState*)user, guest_address, value);
}

static u8 host_dsp_read_aram(void* user, u32 address) {
    (void)user;
    return (u8)aram_read(address, 1u);
}

static void host_dsp_write_aram(void* user, u32 address, u8 value) {
    (void)user;
    aram_write(address, value, 1u);
}

static void host_dsp_dma_write(void* user, u32 address, u32 size) {
    CPUState* cpu = (CPUState*)user;
    const bool report_initial = g_dsp_adapter_dma_reports < 32u;
    const bool inspect_payload =
        report_initial || !g_dsp_adapter_first_nonzero_reported;
    u32 nonzero_bytes = 0u;
    u32 payload_hash = 2166136261u;
    g_dsp_adapter_dma_count++;
    if (inspect_payload) {
        for (u32 i = 0; i < size; i++) {
            const u8 byte = mem_read8(cpu, address + i);
            if (byte != 0u)
                nonzero_bytes++;
            payload_hash ^= byte;
            payload_hash *= 16777619u;
        }
    }
    if (nonzero_bytes != 0u && !g_dsp_adapter_first_nonzero_reported) {
        g_dsp_adapter_first_nonzero_reported = true;
        g_dsp_adapter_first_nonzero_host_retrace = g_host_retrace_count;
        g_dsp_adapter_first_nonzero_retail_retrace =
            mem_read32(cpu, 0x803F7B3Cu);
        fprintf(stderr,
                "[dsp-lle] first-nonzero-dma dma=%llu address=0x%08X "
                "size=%u nonzero=%u host_retrace=%llu retail_retrace=%u "
                "hash=0x%08X\n",
                (unsigned long long)g_dsp_adapter_dma_count, address, size,
                nonzero_bytes,
                (unsigned long long)g_dsp_adapter_first_nonzero_host_retrace,
                g_dsp_adapter_first_nonzero_retail_retrace, payload_hash);
    }
    if (report_initial) {
        fprintf(stderr,
                "[dsp-lle] dma-from-dsp address=0x%08X size=%u pc=0x%08X "
                "first=0x%04X nonzero=%u hash=0x%08X\n",
                address, size, cpu->pc, mem_read16(cpu, address),
                nonzero_bytes, payload_hash);
        g_dsp_adapter_dma_reports++;
    }
}

// High-level DSP services. DSP task DMA carries physical MEM1 addresses, as in
// host_dsp_read_guest; the pointer honours REL data aliases like any guest
// access.
static u8* host_dsp_guest_pointer(void* user, u32 address, u32 size) {
    const u32 guest_address = address < 0x80000000u
                                  ? address | 0x80000000u
                                  : address;
    return get_ram_ptr((CPUState*)user, guest_address, size, NULL);
}

// Bus-clock timebase: the CPU runs at 12 cycles per timebase tick.
static u64 host_dsp_timebase(void* user) {
    (void)user;
    return g_cycle_domain.absolute_cycles / 12u;
}

static void host_dsp_interrupt(void* user) {
    CPUState* cpu = (CPUState*)user;
    g_interrupt_sources_dirty = true;
    g_dsp_adapter_interrupt_pending = true;
    if (g_dsp_adapter_interrupt_reports < 16u) {
        fprintf(stderr, "[dsp-lle] dsp-interrupt pc=0x%08X\n", cpu->pc);
        g_dsp_adapter_interrupt_reports++;
    }
}

static bool host_dsp_adapter_init(CPUState* cpu) {
    g_dsp_adapter_dma_reports = 0;
    g_dsp_adapter_dma_count = 0;
    g_dsp_adapter_first_nonzero_reported = false;
    g_dsp_adapter_first_nonzero_host_retrace = 0;
    g_dsp_adapter_first_nonzero_retail_retrace = 0;
    g_dsp_adapter_guest_read_reports = 0;
    g_dsp_adapter_mail_read_reports = 0;
    g_dsp_adapter_interrupt_pending = false;
    g_dsp_adapter_slice_cycles = 0;
    g_dsp_adapter_update_elapsed = 0;
    // BLUEWAKE_DSP_MODE=hle selects Dolphin's high-level Zelda ucode instead of
    // the donor LLE interpreter: far cheaper, with its own acceptance because
    // mail and interrupt timing differ from the certified LLE route.
    const char* dsp_mode = getenv("BLUEWAKE_DSP_MODE");
    if (dsp_mode != NULL && strcmp(dsp_mode, "hle") == 0) {
        aram_init();
        g_dsp_adapter = bluewake_dsp_adapter_create_hle(
            cpu, host_dsp_guest_pointer, aram_buffer(), ARAM_SIZE,
            host_dsp_timebase, host_dsp_interrupt);
        if (g_dsp_adapter == NULL) {
            fprintf(stderr, "[dsp-hle] initialization failed\n");
            return false;
        }
        fprintf(stderr, "[dsp-hle] Dolphin high-level DSP enabled\n");
        return true;
    }
    const char* irom = getenv("BLUEWAKE_DSP_IROM");
    const char* coef = getenv("BLUEWAKE_DSP_COEF");
    // An explicitly empty path means "leave the adapter off". Without this a
    // caller cannot switch it off, because the product defaults below always
    // set the two names and setenv() with overwrite=0 keeps an empty value.
    if (irom == NULL || coef == NULL || irom[0] == '\0' || coef[0] == '\0') {
        fprintf(stderr,
                "[dsp-lle] adapter inactive; set BLUEWAKE_DSP_IROM and "
                "BLUEWAKE_DSP_COEF to enable the donor-backed route\n");
        return true;
    }
    g_dsp_adapter = bluewake_dsp_adapter_create(
        irom, coef, cpu, host_dsp_read_guest, host_dsp_write_guest,
        host_dsp_read_aram, host_dsp_write_aram,
        host_dsp_dma_write, host_dsp_interrupt);
    if (g_dsp_adapter == NULL) {
        fprintf(stderr, "[dsp-lle] adapter initialization failed\n");
        return false;
    }
    fprintf(stderr, "[dsp-lle] authentic DSPCore shadow route enabled\n");
    return true;
}

static void host_dsp_adapter_shutdown(void) {
    bluewake_dsp_adapter_destroy(g_dsp_adapter);
    g_dsp_adapter = NULL;
}
#endif

static bool configure_virtual_pad(DolHeadlessBackend* backend) {
    const char* buttons_env = getenv("BLUEWAKE_PAD_BUTTONS");
    if (buttons_env != NULL && buttons_env[0] != '\0') {
        char* end = NULL;
        const unsigned long buttons = strtoul(buttons_env, &end, 0);
        if (end == buttons_env || *end != '\0' || buttons > 0xFFFFul) {
            fprintf(stderr, "invalid BLUEWAKE_PAD_BUTTONS=%s\n", buttons_env);
            return false;
        }
        backend->pad[0].button = (u16)buttons;
        fprintf(stderr, "[pad] virtual channel 0 buttons=0x%04X\n",
                backend->pad[0].button);
    }

    const char* stick_x_env = getenv("BLUEWAKE_PAD_STICK_X");
    const char* stick_y_env = getenv("BLUEWAKE_PAD_STICK_Y");
    if (stick_x_env != NULL && stick_x_env[0] != '\0') {
        char* end = NULL;
        const long stick_x = strtol(stick_x_env, &end, 0);
        if (end == stick_x_env || *end != '\0' || stick_x < -128 || stick_x > 127) {
            fprintf(stderr, "invalid BLUEWAKE_PAD_STICK_X=%s\n", stick_x_env);
            return false;
        }
        backend->pad[0].stick_x = (s8)stick_x;
    }
    if (stick_y_env != NULL && stick_y_env[0] != '\0') {
        char* end = NULL;
        const long stick_y = strtol(stick_y_env, &end, 0);
        if (end == stick_y_env || *end != '\0' || stick_y < -128 || stick_y > 127) {
            fprintf(stderr, "invalid BLUEWAKE_PAD_STICK_Y=%s\n", stick_y_env);
            return false;
        }
        backend->pad[0].stick_y = (s8)stick_y;
    }
    if (stick_x_env != NULL || stick_y_env != NULL) {
        fprintf(stderr, "[pad] virtual channel 0 stick=(%d,%d)\n",
                backend->pad[0].stick_x, backend->pad[0].stick_y);
    }

    const char* player_stick_x_env = getenv("BLUEWAKE_PAD_PLAYER_STICK_X");
    const char* player_stick_y_env = getenv("BLUEWAKE_PAD_PLAYER_STICK_Y");
    const char* player_stick_length_env =
        getenv("BLUEWAKE_PAD_PLAYER_STICK_LENGTH");
    const bool player_stick_x_set =
        player_stick_x_env != NULL && player_stick_x_env[0] != '\0';
    const bool player_stick_y_set =
        player_stick_y_env != NULL && player_stick_y_env[0] != '\0';
    if (player_stick_x_set || player_stick_y_set) {
        char* end = NULL;
        u64 length = 20u;
        if (player_stick_length_env != NULL &&
            player_stick_length_env[0] != '\0') {
            length = strtoull(player_stick_length_env, &end, 0);
            if (end == player_stick_length_env || *end != '\0' ||
                length == 0u) {
                fprintf(stderr,
                        "invalid BLUEWAKE_PAD_PLAYER_STICK_LENGTH=%s\n",
                        player_stick_length_env);
                return false;
            }
        }
        long value_x = 0;
        long value_y = 0;
        if (player_stick_x_set) {
            value_x = strtol(player_stick_x_env, &end, 0);
            if (end == player_stick_x_env || *end != '\0' ||
                value_x < -128 || value_x > 127 || value_x == 0) {
                fprintf(stderr, "invalid BLUEWAKE_PAD_PLAYER_STICK_X=%s\n",
                        player_stick_x_env);
                return false;
            }
        }
        if (player_stick_y_set) {
            value_y = strtol(player_stick_y_env, &end, 0);
            if (end == player_stick_y_env || *end != '\0' ||
                value_y < -128 || value_y > 127 || value_y == 0) {
                fprintf(stderr, "invalid BLUEWAKE_PAD_PLAYER_STICK_Y=%s\n",
                        player_stick_y_env);
                return false;
            }
        }
        if ((player_stick_x_set &&
             !bluewake_pad_axis_event_schedule_configure(
                 &g_player_stick_x_pulse, (s8)value_x, length)) ||
            (player_stick_y_set &&
             !bluewake_pad_axis_event_schedule_configure(
                 &g_player_stick_y_pulse, (s8)value_y, length))) {
            fprintf(stderr, "player-ready PAD stick sequence is invalid\n");
            return false;
        }
        fprintf(stderr,
                "[pad] channel 0 player-ready stick armed value=(%ld,%ld) "
                "length=%llu\n",
                value_x, value_y, (unsigned long long)length);
    } else if (player_stick_length_env != NULL &&
               player_stick_length_env[0] != '\0' &&
               getenv("BLUEWAKE_PAD_PLAYER_TARGET_X") == NULL &&
               getenv("BLUEWAKE_PAD_PLAYER_TARGET_Z") == NULL) {
        fprintf(stderr,
                "BLUEWAKE_PAD_PLAYER_STICK_LENGTH requires "
                "BLUEWAKE_PAD_PLAYER_STICK_X or "
                "BLUEWAKE_PAD_PLAYER_STICK_Y\n");
        return false;
    }

    const char* waypoint_x_env = getenv("BLUEWAKE_PAD_PLAYER_TARGET_X");
    const char* waypoint_z_env = getenv("BLUEWAKE_PAD_PLAYER_TARGET_Z");
    const bool waypoint_x_set =
        waypoint_x_env != NULL && waypoint_x_env[0] != '\0';
    const bool waypoint_z_set =
        waypoint_z_env != NULL && waypoint_z_env[0] != '\0';
    if (waypoint_x_set != waypoint_z_set) {
        fprintf(stderr, "player target requires both X and Z\n");
        return false;
    }
    if (waypoint_x_set) {
        if (player_stick_x_set || player_stick_y_set) {
            fprintf(stderr,
                    "player target and fixed player stick are mutually "
                    "exclusive\n");
            return false;
        }
        char* end = NULL;
        g_player_waypoint_x = strtof(waypoint_x_env, &end);
        if (end == waypoint_x_env || *end != '\0') {
            fprintf(stderr, "invalid BLUEWAKE_PAD_PLAYER_TARGET_X=%s\n",
                    waypoint_x_env);
            return false;
        }
        g_player_waypoint_z = strtof(waypoint_z_env, &end);
        if (end == waypoint_z_env || *end != '\0') {
            fprintf(stderr, "invalid BLUEWAKE_PAD_PLAYER_TARGET_Z=%s\n",
                    waypoint_z_env);
            return false;
        }
        g_player_waypoint_length = 1500u;
        if (player_stick_length_env != NULL &&
            player_stick_length_env[0] != '\0') {
            g_player_waypoint_length =
                strtoull(player_stick_length_env, &end, 0);
            if (end == player_stick_length_env || *end != '\0' ||
                g_player_waypoint_length == 0u) {
                fprintf(stderr, "invalid BLUEWAKE_PAD_PLAYER_STICK_LENGTH=%s\n",
                        player_stick_length_env);
                return false;
            }
        }
        g_player_waypoint_configured = true;
        g_player_route_waiting = true;
        fprintf(stderr,
                "[pad] channel 0 player-ready target armed "
                "target=(%.3f,%.3f) length=%llu\n",
                g_player_waypoint_x, g_player_waypoint_z,
                (unsigned long long)g_player_waypoint_length);
    }
    const char* ladder_down_env =
        getenv("BLUEWAKE_PAD_PLAYER_LADDER_DOWN");
    if (ladder_down_env != NULL && ladder_down_env[0] != '\0') {
        if (strcmp(ladder_down_env, "1") != 0 ||
            !g_player_waypoint_configured) {
            fprintf(stderr,
                    "BLUEWAKE_PAD_PLAYER_LADDER_DOWN=1 requires a player "
                    "target\n");
            return false;
        }
        g_player_ladder_down_configured = true;
        fprintf(stderr,
                "[pad] player ladder-down phase armed on guest procedure "
                "0x3A\n");
    }
    const char* post_ladder_route_env =
        getenv("BLUEWAKE_PAD_PLAYER_POST_LADDER_ROUTE");
    if (post_ladder_route_env != NULL && post_ladder_route_env[0] != '\0') {
        if (!g_player_ladder_down_configured ||
            !bluewake_pad_route_parse(post_ladder_route_env, 75.0f,
                                      &g_player_post_ladder_route)) {
            fprintf(stderr,
                    "invalid BLUEWAKE_PAD_PLAYER_POST_LADDER_ROUTE; it "
                    "requires ladder-down and X,Z points separated by ';'\n");
            return false;
        }
        g_player_post_ladder_route_configured = true;
        fprintf(stderr,
                "[pad] post-ladder route armed points=%zu radius=%.1f\n",
                g_player_post_ladder_route.count,
                g_player_post_ladder_route.radius);
    }
    // The same waypoint route without the ladder: it starts at the first
    // steered player update, for a save that already stands where the
    // post-ladder route begins (the Outset pier save used by the reference
    // captures stands at its first point). Set the player target to the
    // route's first point so the steering arms.
    const char* player_route_env = getenv("BLUEWAKE_PAD_PLAYER_ROUTE");
    if (player_route_env != NULL && player_route_env[0] != '\0') {
        if (g_player_post_ladder_route_configured ||
            !g_player_waypoint_configured ||
            !bluewake_pad_route_parse(player_route_env, 75.0f,
                                      &g_player_post_ladder_route)) {
            fprintf(stderr,
                    "invalid BLUEWAKE_PAD_PLAYER_ROUTE; it requires a player "
                    "target, no post-ladder route, and X,Z points separated "
                    "by ';'\n");
            return false;
        }
        g_player_ladder_down_configured = true;
        g_player_ladder_move_seen = true;
        g_player_post_ladder_route_configured = true;
        fprintf(stderr, "[pad] player route armed points=%zu radius=%.1f\n",
                g_player_post_ladder_route.count,
                g_player_post_ladder_route.radius);
    }
    g_player_start_after = 0u;
    const char* start_after_env = getenv("BLUEWAKE_PAD_PLAYER_START_AFTER");
    if (start_after_env != NULL && start_after_env[0] != '\0') {
        char* start_end = NULL;
        g_player_start_after = strtoull(start_after_env, &start_end, 0);
        if (start_end == start_after_env || *start_end != '\0') {
            fprintf(stderr, "invalid BLUEWAKE_PAD_PLAYER_START_AFTER=%s\n",
                    start_after_env);
            return false;
        }
        fprintf(stderr, "[pad] player stick waits for retrace %llu\n",
                (unsigned long long)g_player_start_after);
    }
    const char* route_confirm_env =
        getenv("BLUEWAKE_PAD_PLAYER_ROUTE_CONFIRM");
    if (route_confirm_env != NULL && route_confirm_env[0] != '\0') {
        if (strcmp(route_confirm_env, "1") != 0 ||
            !g_player_post_ladder_route_configured ||
            !bluewake_pad_event_schedule_configure(
                &g_player_route_confirm_pulse, 0x0100u, 2u)) {
            fprintf(stderr,
                    "BLUEWAKE_PAD_PLAYER_ROUTE_CONFIRM=1 requires a valid "
                    "post-ladder route\n");
            return false;
        }
        g_player_route_confirm_configured = true;
        fprintf(stderr,
                "[pad] post-ladder route confirmation armed length=2 "
                "buttons=0x0100\n");
    }

    const char* confirm_event_env =
        getenv("BLUEWAKE_PAD_CONFIRM_EVENT");
    if (confirm_event_env != NULL && confirm_event_env[0] != '\0') {
        const bool any_prompt = strcmp(confirm_event_env, "any") == 0 ||
                                strcmp(confirm_event_env, "*") == 0;
        char* end = NULL;
        const long event_idx =
            any_prompt ? 0 : strtol(confirm_event_env, &end, 0);
        if ((!any_prompt &&
             (end == confirm_event_env || *end != '\0' || event_idx < 0 ||
              event_idx > INT32_MAX)) ||
            !bluewake_pad_event_schedule_configure(
                &g_event_confirm_pulse, 0x0100u, 2u)) {
            fprintf(stderr, "invalid BLUEWAKE_PAD_CONFIRM_EVENT=%s\n",
                    confirm_event_env);
            return false;
        }
        g_event_confirm_target = (s32)event_idx;
        g_event_confirm_any = any_prompt;
        g_event_prompt_trace = getenv("BLUEWAKE_EVENT_PROMPT_TRACE") != NULL;
        fprintf(stderr,
                "[pad] channel 0 event-confirm armed event=%s "
                "length=2 buttons=0x0100\n",
                any_prompt ? "any" : confirm_event_env);
    }

    backend->pad[0].error = 0;
    backend->pad_mask = 1u;
    g_virtual_pad[0] = backend->pad[0];

    const char* pulse_retrace = getenv("BLUEWAKE_PAD_PULSE_RETRACE");
    const char* pulse_length = getenv("BLUEWAKE_PAD_PULSE_LENGTH");
    const char* pulse2_retrace = getenv("BLUEWAKE_PAD_PULSE2_RETRACE");
    const char* pulse2_length = getenv("BLUEWAKE_PAD_PULSE2_LENGTH");
    const char* pulse3_retrace = getenv("BLUEWAKE_PAD_PULSE3_RETRACE");
    const char* pulse3_length = getenv("BLUEWAKE_PAD_PULSE3_LENGTH");
    const char* pulse_on_title =
        getenv("BLUEWAKE_PAD_PULSE_ON_TITLE_READY");
    if (pulse_on_title != NULL && pulse_on_title[0] != '\0') {
        if (strcmp(pulse_on_title, "1") != 0 ||
            (pulse_retrace != NULL && pulse_retrace[0] != '\0')) {
            fprintf(stderr,
                    "invalid BLUEWAKE_PAD_PULSE_ON_TITLE_READY=%s\n",
                    pulse_on_title);
            return false;
        }
        char* end = NULL;
        u64 event_length = 2u;
        if (pulse_length != NULL && pulse_length[0] != '\0') {
            event_length = strtoull(pulse_length, &end, 0);
            if (end == pulse_length || *end != '\0') {
                fprintf(stderr, "invalid BLUEWAKE_PAD_PULSE_LENGTH=%s\n",
                        pulse_length);
                return false;
            }
        }
        if (!bluewake_pad_event_schedule_configure(
                &g_title_pad_pulse, backend->pad[0].button, event_length)) {
            fprintf(stderr,
                    "title-ready PAD pulse requires nonzero buttons and length\n");
            return false;
        }
        if (!bluewake_pad_event_schedule_configure_latched(
                &g_file_slot_select_pulse, backend->pad[0].button) ||
            !bluewake_pad_event_schedule_configure_latched(
                &g_file_start_pulse, backend->pad[0].button) ||
            !bluewake_pad_event_schedule_configure_latched(
                &g_name_character_pulse, backend->pad[0].button) ||
            !bluewake_pad_event_schedule_configure_latched(
                &g_name_end_pulse, 0x1000u) ||
            !bluewake_pad_event_schedule_configure_latched(
                &g_name_confirm_pulse, backend->pad[0].button)) {
            fprintf(stderr, "new-game PAD sequence is invalid\n");
            return false;
        }
        if (!bluewake_pad_event_schedule_configure(
                &g_title_confirm_pulse, backend->pad[0].button,
                event_length)) {
            fprintf(stderr, "title confirmation PAD pulse is invalid\n");
            return false;
        }
        if (!bluewake_pad_event_schedule_configure(
                &g_no_card_dismiss_pulse, backend->pad[0].button,
                event_length) ||
            !bluewake_pad_axis_event_schedule_configure(
                &g_no_save_left_pulse, -127, event_length) ||
            !bluewake_pad_event_schedule_configure(
                &g_no_save_confirm_pulse, backend->pad[0].button,
                event_length)) {
            fprintf(stderr, "no-card dialog PAD sequence is invalid\n");
            return false;
        }
        g_pad_pulse_buttons = backend->pad[0].button;
        g_virtual_pad[0].button = 0u;
        fprintf(stderr,
                "[pad] channel 0 pulse armed on title-ready length=%llu "
                "buttons=0x%04X\n",
                (unsigned long long)event_length, g_pad_pulse_buttons);
    }
    // The in-game save route (P4 milestone 9). Its own schedules: START, A,
    // and the two stick axes it steps the pause menu's cursor with. Off unless
    // BLUEWAKE_SAVE_ROUTE is set, so nothing else in this file changes shape.
    const char* save_route_env = getenv("BLUEWAKE_SAVE_ROUTE");
    if (save_route_env != NULL && save_route_env[0] != '\0') {
        char* save_end = NULL;
        const unsigned long save_route = strtoul(save_route_env, &save_end, 0);
        if (save_end == save_route_env || *save_end != '\0' ||
            save_route == 0ul) {
            fprintf(stderr, "invalid BLUEWAKE_SAVE_ROUTE=%s\n", save_route_env);
            return false;
        }
        if (!bluewake_pad_event_schedule_configure(&g_save_start_pulse, 0x1000u,
                                                  3u) ||
            !bluewake_pad_event_schedule_configure(&g_save_confirm_pulse,
                                                   0x0100u, 3u) ||
            !bluewake_pad_event_schedule_configure(&g_save_page_pulse, 0x0020u,
                                                   3u) ||
            !bluewake_pad_axis_event_schedule_configure(&g_save_stick_x_pulse,
                                                        127, 3u) ||
            !bluewake_pad_axis_event_schedule_configure(&g_save_stick_y_pulse,
                                                        127, 3u)) {
            fprintf(stderr, "save route PAD sequence is invalid\n");
            return false;
        }
        g_save_route_enabled = true;
        // The pause menu's own execute and the collect screen beneath it are
        // observed only for this route: the observation costs a host turn at
        // that block, and a host turn is what the certified stops are quoted in.
        bluewake_edge_set_menu_path_observation(true);
        fprintf(stderr,
                "[pad] save route armed: START/A pulses and stick steps "
                "from retrace %u\n",
                (unsigned)BLUEWAKE_SAVE_FIRST_RETRACE);
    }
    if (pulse_retrace != NULL && pulse_retrace[0] != '\0') {
        char* end = NULL;
        g_pad_pulse_start_retrace = strtoull(pulse_retrace, &end, 0);
        if (end == pulse_retrace || *end != '\0') {
            fprintf(stderr, "invalid BLUEWAKE_PAD_PULSE_RETRACE=%s\n",
                    pulse_retrace);
            return false;
        }
        g_pad_pulse_length = 2u;
        if (pulse_length != NULL && pulse_length[0] != '\0') {
            g_pad_pulse_length = strtoull(pulse_length, &end, 0);
            if (end == pulse_length || *end != '\0' ||
                g_pad_pulse_length == 0u) {
                fprintf(stderr, "invalid BLUEWAKE_PAD_PULSE_LENGTH=%s\n",
                        pulse_length);
                return false;
            }
        }
        g_pad_pulse_buttons = backend->pad[0].button;
        g_pad_pulse_enabled = true;
        g_virtual_pad[0].button = 0u;
        fprintf(stderr,
                "[pad] channel 0 pulse retrace=%llu length=%llu buttons=0x%04X\n",
                (unsigned long long)g_pad_pulse_start_retrace,
                (unsigned long long)g_pad_pulse_length, g_pad_pulse_buttons);
    }
    if (pulse2_retrace != NULL && pulse2_retrace[0] != '\0') {
        char* end = NULL;
        g_pad_pulse2_start_retrace = strtoull(pulse2_retrace, &end, 0);
        if (end == pulse2_retrace || *end != '\0') {
            fprintf(stderr, "invalid BLUEWAKE_PAD_PULSE2_RETRACE=%s\n", pulse2_retrace);
            return false;
        }
        g_pad_pulse2_length = 2u;
        if (pulse2_length != NULL && pulse2_length[0] != '\0') {
            g_pad_pulse2_length = strtoull(pulse2_length, &end, 0);
            if (end == pulse2_length || *end != '\0' || g_pad_pulse2_length == 0u) {
                fprintf(stderr, "invalid BLUEWAKE_PAD_PULSE2_LENGTH=%s\n", pulse2_length);
                return false;
            }
        }
        g_pad_pulse2_enabled = true;
        fprintf(stderr, "[pad] channel 0 pulse2 retrace=%llu length=%llu buttons=0x%04X\n",
                (unsigned long long)g_pad_pulse2_start_retrace,
                (unsigned long long)g_pad_pulse2_length, g_pad_pulse_buttons);
    }
    if (pulse3_retrace != NULL && pulse3_retrace[0] != '\0') {
        char* end = NULL;
        g_pad_pulse3_start_retrace = strtoull(pulse3_retrace, &end, 0);
        if (end == pulse3_retrace || *end != '\0') {
            fprintf(stderr, "invalid BLUEWAKE_PAD_PULSE3_RETRACE=%s\n",
                    pulse3_retrace);
            return false;
        }
        g_pad_pulse3_length = 2u;
        if (pulse3_length != NULL && pulse3_length[0] != '\0') {
            g_pad_pulse3_length = strtoull(pulse3_length, &end, 0);
            if (end == pulse3_length || *end != '\0' ||
                g_pad_pulse3_length == 0u) {
                fprintf(stderr, "invalid BLUEWAKE_PAD_PULSE3_LENGTH=%s\n",
                        pulse3_length);
                return false;
            }
        }
        g_pad_pulse3_enabled = true;
        fprintf(stderr,
                "[pad] channel 0 pulse3 retrace=%llu length=%llu buttons=0x%04X\n",
                (unsigned long long)g_pad_pulse3_start_retrace,
                (unsigned long long)g_pad_pulse3_length, g_pad_pulse_buttons);
    }
    const char* pad_script_env = getenv("BLUEWAKE_PAD_SCRIPT");
    if (pad_script_env != NULL && pad_script_env[0] != '\0') {
        const char* cursor = pad_script_env;
        while (*cursor != '\0') {
            bool ok = g_pad_script_count < BLUEWAKE_PAD_SCRIPT_MAX;
            char* end = NULL;
            u64 start = 0u;
            u64 buttons = 0u;
            u64 length = 2u;
            if (ok) {
                start = strtoull(cursor, &end, 0);
                ok = end != cursor && *end == ':';
            }
            if (ok) {
                cursor = end + 1;
                buttons = strtoull(cursor, &end, 0);
                ok = end != cursor;
            }
            if (ok && *end == ':') {
                cursor = end + 1;
                length = strtoull(cursor, &end, 0);
                ok = end != cursor;
            }
            long stick_x = 0, stick_y = 0;
            bool has_stick = false;
            if (ok && *end == ':') {
                cursor = end + 1;
                stick_x = strtol(cursor, &end, 0);
                ok = end != cursor && *end == ':';
                if (ok) {
                    cursor = end + 1;
                    stick_y = strtol(cursor, &end, 0);
                    ok = end != cursor && stick_x >= -128 && stick_x <= 127 &&
                         stick_y >= -128 && stick_y <= 127;
                }
                has_stick = ok;
            }
            long substick_x = 0, substick_y = 0;
            bool has_substick = false;
            if (ok && has_stick && *end == ':') {
                cursor = end + 1;
                substick_x = strtol(cursor, &end, 0);
                ok = end != cursor && *end == ':';
                if (ok) {
                    cursor = end + 1;
                    substick_y = strtol(cursor, &end, 0);
                    ok = end != cursor && substick_x >= -128 && substick_x <= 127 &&
                         substick_y >= -128 && substick_y <= 127;
                }
                has_substick = ok;
            }
            if (ok)
                ok = (*end == '\0' || *end == ',') && length > 0u;
            if (!ok) {
                fprintf(stderr,
                        "invalid BLUEWAKE_PAD_SCRIPT=\"%s\"; expected "
                        "retrace:buttons:length[:stick_x:stick_y[:cstick_x:cstick_y]] entries "
                        "separated by ','\n",
                        pad_script_env);
                return false;
            }
            BluewakePadScriptPress* press =
                &g_pad_script[g_pad_script_count++];
            press->start_retrace = start;
            press->length = length;
            press->buttons = (u16)buttons;
            press->has_stick = has_stick;
            press->stick_x = (s8)stick_x;
            press->stick_y = (s8)stick_y;
            press->has_substick = has_substick;
            press->substick_x = (s8)substick_x;
            press->substick_y = (s8)substick_y;
            cursor = (*end == ',') ? end + 1 : end;
        }
        fprintf(stderr, "[pad] script armed %u scheduled press(es)\n",
                g_pad_script_count);
    }
    return true;
}

static void instruction_fallback(CPUState* ctx, u32 raw, u32 cia) {
    if ((raw >> 26) == 31u) {
        u32 xo = (raw >> 1) & 0x3FFu;
        u16 spr = (u16)(((raw >> 16) & 0x1Fu) | ((raw >> 6) & 0x3E0u));
        u8 reg = (u8)((raw >> 21) & 31u);
        if (xo == 339u && spr == 22u) {
            ctx->gpr[reg] = ppc_mfspr(ctx, spr, cia);
            ctx->pc = cia + 4u;
            return;
        }
        if (xo == 467u && spr == 22u) {
            ppc_mtspr(ctx, spr, ctx->gpr[reg], cia);
            if (!ctx->exception)
                ctx->pc = cia + 4u;
            return;
        }
        if (xo == 982u || xo == 86u || xo == 54u || xo == 470u || xo == 467u) {
            ctx->pc = cia + 4u; return;
        }
        if (xo == 339u) { ctx->gpr[(raw >> 21) & 31u] = 0; ctx->pc = cia + 4u; return; }
    }
    if (bluewake_cold_fallback_execute(ctx, raw, cia))
        return;
    fprintf(stderr, "[fallback] unhandled instr 0x%08X at 0x%08X\n", raw, cia);
    ctx->exception |= PPC_EXC_PROGRAM;
}

typedef struct LogoResourceSyncOperand {
    const char* name;
    u32 pointer_address;
} LogoResourceSyncOperand;

static void report_logo_resource_sync(CPUState* cpu, u32 object_sync_result) {
    static const LogoResourceSyncOperand operands[] = {
        {"anm", 0x803F7220u},       {"fmap", 0x803F7224u},
        {"itemRes", 0x803F7228u},   {"fmapRes", 0x803F722Cu},
        {"dmapRes", 0x803F7230u},   {"clctRes", 0x803F7234u},
        {"optRes", 0x803F7238u},    {"saveRes", 0x803F723Cu},
        {"clothRes", 0x803F7240u},  {"itemicon", 0x803F7244u},
        {"actionicon", 0x803F7248u},{"scopeRes", 0x803F724Cu},
        {"camRes", 0x803F7250u},    {"swimRes", 0x803F7254u},
        {"windRes", 0x803F7258u},   {"nameRes", 0x803F725Cu},
        {"tmsg", 0x803F7260u},      {"dmsg", 0x803F7264u},
        {"errorRes", 0x803F7268u},  {"msgDt", 0x803F726Cu},
        {"msgDt2", 0x803F7270u},    {"msg", 0x803F7274u},
        {"menu", 0x803F7278u},      {"font", 0x803F727Cu},
        {"ruby", 0x803F7280u},      {"particle", 0x803F7284u},
        {"itemTable", 0x803F7288u}, {"ActorData", 0x803F728Cu},
        {"FmapData", 0x803F7290u},  {"lod", 0x803F7294u},
    };
    fprintf(stderr,
            "[boot-resource-sync] retrace=%llu object_sync=%u operands=%zu\n",
            (unsigned long long)g_host_retrace_count, object_sync_result,
            sizeof(operands) / sizeof(operands[0]));
    for (size_t i = 0; i < sizeof(operands) / sizeof(operands[0]); i++) {
        const u32 pointer = mem_read32(cpu, operands[i].pointer_address);
        const u32 status = pointer == 0u ? 0xFFFFFFFFu
                                        : mem_read8(cpu, pointer + 0x0Cu);
        fprintf(stderr,
                "[boot-resource-sync] name=%s pointer=0x%08X status=0x%08X\n",
                operands[i].name, pointer, status);
    }
}

static bool write_rgba_ppm(const char* path, const u8* rgba, u32 width,
                           u32 height) {
    FILE* file = fopen(path, "wb");
    if (file == NULL)
        return false;
    if (fprintf(file, "P6\n%u %u\n255\n", width, height) < 0) {
        fclose(file);
        return false;
    }
    for (u64 pixel = 0u; pixel < (u64)width * height; pixel++) {
        if (fwrite(rgba + pixel * 4u, 1u, 3u, file) != 3u) {
            fclose(file);
            return false;
        }
    }
    return fclose(file) == 0;
}

// Insert the retrace number before the extension of a capture path so a
// periodic diagnostic keeps a frame series instead of overwriting one frame.
static void host_capture_path_for_retrace(const char* base, u64 retrace,
                                          char* out, size_t out_size) {
    const char* dot = strrchr(base, '.');
    if (dot == NULL) {
        snprintf(out, out_size, "%s-%llu.ppm", base,
                 (unsigned long long)retrace);
        return;
    }
    snprintf(out, out_size, "%.*s-%llu%s", (int)(dot - base), base,
             (unsigned long long)retrace, dot);
}

static char g_host_root[4096];

// Resolve the repository root that owns this host binary. A double-clicked
// BlueWake.app inherits no environment and a working directory of "/", so the
// launch path has to be discoverable from the executable itself.
// BLUEWAKE_ROOT overrides the search.
static const char* host_resolve_root(void) {
    const char* env_root = getenv("BLUEWAKE_ROOT");
    if (env_root != NULL && env_root[0] != '\0') {
        snprintf(g_host_root, sizeof g_host_root, "%s", env_root);
        return g_host_root;
    }
#if defined(__APPLE__)
    char exe[4096];
    uint32_t exe_size = (uint32_t)sizeof exe;
    if (_NSGetExecutablePath(exe, &exe_size) == 0) {
        char real[4096];
        if (realpath(exe, real) != NULL) {
            // BlueWake.app/Contents/MacOS/BlueWake lives inside the build
            // tree, so the repository root is an ancestor of the executable.
            for (;;) {
                char* slash = strrchr(real, '/');
                if (slash == NULL || slash == real)
                    break;
                *slash = '\0';
                char probe[4096 + 64];
                if (snprintf(probe, sizeof probe, "%s/generated/full/main.dol",
                             real) < (int)sizeof probe &&
                    access(probe, R_OK) == 0) {
                    snprintf(g_host_root, sizeof g_host_root, "%s", real);
                    return g_host_root;
                }
            }
        }
    }
#endif
    if (getcwd(g_host_root, sizeof g_host_root) == NULL)
        g_host_root[0] = '\0';
    return g_host_root;
}

// Fill in a path the app needs when the human did not export one. An existing
// value always wins, and a default is only used when the file is really there.
static void host_apply_default_env(const char* name, const char* root,
                                   const char* relative) {
    const char* existing = getenv(name);
    if (existing != NULL && existing[0] != '\0')
        return;
    if (root == NULL || root[0] == '\0')
        return;
    char probe[4096 + 128];
    if (snprintf(probe, sizeof probe, "%s/%s", root, relative) >=
        (int)sizeof probe)
        return;
    if (access(probe, R_OK) != 0)
        return;
    (void)setenv(name, probe, 0);
}

/* BLUEWAKE_ASPECT: the picture's shape. 4:3 is the game's own; 16:10 and 16:9
   turn on the matching widescreen mod (a wider camera, culling and HUD) and
   ask the renderer for a frame buffer of that shape (DOL_AURORA_ASPECT_RATIO),
   which also sizes the window unless DOL_AURORA_WINDOW does. Explicit
   BLUEWAKE_MODS or DOL_AURORA_ASPECT_RATIO settings are kept. */
static void host_apply_aspect(void) {
    const char* aspect = getenv("BLUEWAKE_ASPECT");
    if (aspect == NULL || aspect[0] == '\0' || strcmp(aspect, "4:3") == 0)
        return;
    const char* mod = NULL;
    const char* ratio = NULL;
    if (strcmp(aspect, "16:10") == 0) {
        mod = "widescreen1610";
        ratio = "1.6";
    } else if (strcmp(aspect, "16:9") == 0) {
        mod = "widescreen";
        ratio = "1.7778";
    } else {
        fprintf(stderr, "[aspect] unknown BLUEWAKE_ASPECT=%s (4:3, 16:10 or 16:9); keeping 4:3\n", aspect);
        return;
    }
    setenv("DOL_AURORA_ASPECT_RATIO", ratio, 0);
    const char* mods = getenv("BLUEWAKE_MODS");
    char list[256];
    snprintf(list, sizeof list, "%s%s%s", mods && mods[0] ? mods : "", mods && mods[0] ? "," : "", mod);
    setenv("BLUEWAKE_MODS", list, 1);
    fprintf(stderr, "[aspect] %s: mod %s, frame buffer %s\n", aspect, mod, getenv("DOL_AURORA_ASPECT_RATIO"));
}

// ---------------------------------------------------------------------------
// Save states (debugging): the running game written to a file and read back,
// Dolphin-style, so a rendering or performance problem can be reproduced at
// the spot it happens instead of by replaying a route.
//
//   BLUEWAKE_SAVE_STATE=path@retrace[,path@retrace...]
//       save at the first clean point at or after that retrace (see below);
//   BLUEWAKE_LOAD_STATE=path
//       restore right after boot, before the first guest instruction runs;
//   F5 / F9 (the window; fn-F5 / fn-F9 on a Mac keyboard), or the options
//   menu's Save state / Load latest state
//       save to BLUEWAKE_STATE_DIR (default: the working directory; the Mac
//       app's is its data folder's states/) as quick-<retrace>.bwstate / load
//       the last state saved or loaded in this run, else the newest .bwstate
//       in that directory (after a relaunch).
//   BLUEWAKE_LOAD_STATE_FORCE=1 loads a state made by another translation of
//       the game (a different composite), which is otherwise refused.
//   BLUEWAKE_STATE_TEST_LOAD=retrace (testing) presses F9 at that retrace.
//
// Checked (headless, the Outset save walked by a pad script): a state saved
// at retrace 1000 and loaded at boot, or mid-run at 900 or 1300, reaches
// retrace 1500 with MEM1, ARAM, the CPU, the aliases, the DSP and the host
// variables byte for byte those of the run that never loaded; with the
// renderer, a load at boot and one mid-run reach the same 1500 as each other
// (a windowed run's own timing varies from launch to launch).
//
// Where the whole machine is. Between host turns the guest's entire state is
// CPUState plus memory: the chassis returns to the host at block boundaries
// with nothing of the guest on the host stack (a turn can end at any block and
// the next dispatch resumes there, which is what OS thread switches already
// rely on). So a state is taken at the top of a host turn, and holds:
//   CPUState's register prefix (everything before the first host pointer);
//   MEM1 as the host expands it (32 MiB: linked REL code/data above 24 MiB);
//   ARAM (16 MiB); the storage of every guest alias (linked REL data and BSS);
//   the VI clock; the host's device models (PI/VI interrupts, SI, DI, AI and
//   DSP DMA, ARAM DMA, IPL SRAM, the virtual pads), the DSP mail handshake,
//   the guest clock and decrementer, the cycle domain and device cursors, the
//   REL alias/slot registries, the route automation's progress and the
//   milestones the host's per-pc hooks depend on (HOSTVARS, by name);
//   Dolphin's DSP HLE (DSPHLE::DoState through a PointerWrap);
//   the GX front end's register model (CP/XF/BP, TMEM palettes) plus the bytes
//   of any GX command the guest has not finished writing, and the gxcore sink's
//   register state. Without these a frame after a load would be decoded with
//   vertex formats the game set before the save, and the first draw would fail
//   the front end for good.
// Rebuilt instead of saved: the renderer's texture and pipeline caches, EFB
// copies, the in-between (Smooth Motion) frame history, the audio queue, the
// dispatcher's pc cache.
//
// When. A save waits for a clean point: the top of a turn whose pc is
// GXSetDrawDone's return (0x80322BC8) - the frame's GX commands are complete,
// so after a load the next frame is drawn whole - with no exception pending,
// no REL prolog half done, no HLE card callback queued or running, no scene
// change (overlap) or quick door under way. A request that has not met a
// draw-done point within 120 retraces takes the next clean retrace boundary
// instead (the GX front end carries any partial command either way).
#define HOST_STATE_DRAW_DONE_PC 0x80322BC8u
#define HOST_STATE_MAX_REQUESTS 8u

typedef struct HostStateLoop {
    bool* profile_prolog_called;
    bool* rel_prolog_sda_pending;
    u32* rel_prolog_saved_r13;
} HostStateLoop;

typedef struct HostStateRequest {
    char path[1024];
    u64 retrace;
    bool done;
} HostStateRequest;

static HostStateRequest g_state_requests[HOST_STATE_MAX_REQUESTS];
static u32 g_state_request_count;
static volatile bool g_state_hotkey_save;
static volatile bool g_state_hotkey_load;
static bool g_state_save_armed;
static u64 g_state_armed_retrace;
static u64 g_state_poll_retrace = UINT64_MAX;
static bool g_state_aurora;
static char g_state_last_path[1024];
static unsigned g_state_refusals;

typedef struct HostStateHeader {
    u32 version;
    u32 cpu_state_size;
    u32 cpu_pod_size;
    u32 entry_point;
    u32 chunk_count;
    u32 ram_size;
    u64 chunk_digest;
    u64 retrace;
    u32 pc;
    u32 mod_mask;
    u32 dsp_hle;
    u32 aurora;
    char game_id[8];
} HostStateHeader;

#define HS_FIELD(x) {#x, (void*)&(x), (uint32_t)sizeof(x)}
static const BwStateField k_host_state_fields[] = {
    // REL links: raw images, their executable aliases and scratch slots.
    HS_FIELD(g_rel_aliases), HS_FIELD(g_rel_alias_count),
    HS_FIELD(g_rel_alias_raw_min), HS_FIELD(g_rel_alias_raw_max),
    HS_FIELD(g_module1_raw_base), HS_FIELD(g_module336_bss_alias_installed),
    HS_FIELD(g_rel_slots),
    // The DSP mail handshake the host models around the DSP.
    HS_FIELD(g_dsp_control), HS_FIELD(g_dsp_aram_complete), HS_FIELD(g_dsp_mail_from),
    HS_FIELD(g_dsp_mail_from_pending), HS_FIELD(g_dsp_mail_reads_remaining),
    HS_FIELD(g_dsp_mail_to_high_seen), HS_FIELD(g_dsp_boot_mail_armed),
    HS_FIELD(g_dsp_boot_mail_clear_seen), HS_FIELD(g_dsp_boot_handshake_sent),
    HS_FIELD(g_dsp_task_handshake_sent), HS_FIELD(g_dsp_task_request_pending),
    HS_FIELD(g_dsp_task_request_armed), HS_FIELD(g_dsp_boot_task_ready),
    HS_FIELD(g_dsp_task_boot_started), HS_FIELD(g_dsp_audio_frame_words_remaining),
    HS_FIELD(g_dsp_mail_to_high_value),
    // Device models.
    HS_FIELD(g_audio_dma), HS_FIELD(g_interrupts), HS_FIELD(g_si),
    HS_FIELD(g_virtual_pad), HS_FIELD(g_aram_dma),
    HS_FIELD(g_ipl_sram.sram), HS_FIELD(g_ipl_sram.status),
    HS_FIELD(g_ipl_sram.dma_address), HS_FIELD(g_ipl_sram.dma_length),
    HS_FIELD(g_ipl_sram.control), HS_FIELD(g_ipl_sram.data),
    HS_FIELD(g_ipl_sram.command_latched), HS_FIELD(g_ipl_sram.write),
    HS_FIELD(g_ipl_sram.cursor), HS_FIELD(g_ipl_sram.sram_region),
    HS_FIELD(g_ipl_sram.writes),
    HS_FIELD(g_di.status), HS_FIELD(g_di.cover), HS_FIELD(g_di.command),
    HS_FIELD(g_di.dma_address), HS_FIELD(g_di.dma_length), HS_FIELD(g_di.control),
    HS_FIELD(g_di.immediate_data), HS_FIELD(g_di.config),
    // Guest time: the decrementer, the cycle domain and each device's cursor.
    HS_FIELD(g_guest_clock_decrementer), HS_FIELD(g_guest_clock_decrementer_valid),
    HS_FIELD(g_guest_clock_decrementer_expired), HS_FIELD(g_guest_clock_cycle_remainder),
    HS_FIELD(g_guest_decrementer_pending),
    HS_FIELD(g_cycle_domain.absolute_cycles), HS_FIELD(g_cycle_domain.dispatch_cycles),
    HS_FIELD(g_vi_cycle_cursor), HS_FIELD(g_audio_cycle_cursor), HS_FIELD(g_dsp_cycle_cursor),
    HS_FIELD(g_dsp_adapter_interrupt_pending), HS_FIELD(g_dsp_adapter_slice_cycles),
    HS_FIELD(g_dsp_adapter_update_elapsed), HS_FIELD(g_dsp_adapter_dma_count),
    HS_FIELD(g_host_retrace_count), HS_FIELD(g_previous_retrace_timebase), HS_FIELD(g_vi_assert_reports),
    HS_FIELD(g_context_shadows), HS_FIELD(g_delivery_digest),
    HS_FIELD(g_async_draw_done_commits),
    // Milestones: several of the host's per-pc hooks only act before or after
    // one (the edge service, the route's pulses).
    HS_FIELD(g_title_ready_reported), HS_FIELD(g_title_ready_retrace),
    HS_FIELD(g_file_select_reported), HS_FIELD(g_file_select_retrace),
    HS_FIELD(g_name_scene_create_reported), HS_FIELD(g_name_scene_create_retrace),
    HS_FIELD(g_name_scene_object), HS_FIELD(g_name_scene_execute_reported),
    HS_FIELD(g_name_scene_execute_retrace), HS_FIELD(g_memcard_check_reported),
    HS_FIELD(g_memcard_check_retrace), HS_FIELD(g_new_game_intro_reported),
    HS_FIELD(g_new_game_intro_retrace), HS_FIELD(g_name_input_complete_reported),
    HS_FIELD(g_name_input_complete_retrace), HS_FIELD(g_name_scene_change_reported),
    HS_FIELD(g_name_scene_change_retrace), HS_FIELD(g_open_scene_request_reported),
    HS_FIELD(g_open_scene_request_retrace), HS_FIELD(g_play_scene_reported),
    HS_FIELD(g_play_scene_retrace), HS_FIELD(g_opening_complete_reported),
    HS_FIELD(g_opening_complete_retrace), HS_FIELD(g_outset_room_requested),
    HS_FIELD(g_outset_room_request_retrace), HS_FIELD(g_overlap_last_phase),
    HS_FIELD(g_overlap_terminal_phase), HS_FIELD(g_name_character_jut_hold_reported),
    HS_FIELD(g_name_character_jut_trigger_reported),
    HS_FIELD(g_name_character_cpad_hold_reported),
    HS_FIELD(g_name_character_cpad_trigger_reported),
    // The route automation's progress (the pad script itself, BLUEWAKE_PAD_SCRIPT
    // and run_host.sh's EXTRA_PAD, comes from the loading run's environment).
    HS_FIELD(g_title_pad_pulse), HS_FIELD(g_title_confirm_pulse),
    HS_FIELD(g_no_card_dismiss_pulse), HS_FIELD(g_no_save_left_pulse),
    HS_FIELD(g_player_stick_x_pulse), HS_FIELD(g_player_stick_y_pulse),
    HS_FIELD(g_player_waypoint_active), HS_FIELD(g_player_ladder_entered),
    HS_FIELD(g_player_ladder_move_seen), HS_FIELD(g_player_post_ladder_route),
    HS_FIELD(g_player_post_ladder_route_started), HS_FIELD(g_player_post_ladder_route_active),
    HS_FIELD(g_player_post_ladder_route_complete),
    HS_FIELD(g_player_post_ladder_route_arrival_retrace),
    HS_FIELD(g_player_route_confirm_pulse), HS_FIELD(g_player_route_confirm_triggered),
    HS_FIELD(g_event_confirm_pulse), HS_FIELD(g_event_confirm_prompt_active),
    HS_FIELD(g_event_confirm_demo_prompt_active), HS_FIELD(g_no_save_confirm_pulse),
    HS_FIELD(g_file_slot_select_pulse), HS_FIELD(g_file_start_pulse),
    HS_FIELD(g_name_character_pulse), HS_FIELD(g_name_end_pulse),
    HS_FIELD(g_name_confirm_pulse), HS_FIELD(g_save_start_pulse),
    HS_FIELD(g_save_confirm_pulse), HS_FIELD(g_save_page_pulse),
    HS_FIELD(g_save_stick_x_pulse), HS_FIELD(g_save_stick_y_pulse),
    HS_FIELD(g_save_route_state), HS_FIELD(g_save_route_hold),
    HS_FIELD(g_save_route_retries), HS_FIELD(g_save_route_steps),
    HS_FIELD(g_save_route_control_retrace), HS_FIELD(g_save_route_next_retrace),
    HS_FIELD(g_save_route_menu_retrace), HS_FIELD(g_save_route_item),
    HS_FIELD(g_save_route_mode), HS_FIELD(g_save_route_proc),
    HS_FIELD(g_save_route_status), HS_FIELD(g_save_route_acted_proc),
    HS_FIELD(g_save_route_acted_status), HS_FIELD(g_save_route_acted_retrace),
    HS_FIELD(g_save_route_right_sent), HS_FIELD(g_save_trigger_start_retrace),
    HS_FIELD(g_save_trigger_length), HS_FIELD(g_save_trigger_value),
    HS_FIELD(g_save_route_page_presses), HS_FIELD(g_save_route_script_suppressed),
    HS_FIELD(g_live_takeover), HS_FIELD(g_player_route_waiting),
};
#undef HS_FIELD

static u64 host_state_chunk_digest(const StaticRecompModuleDesc* mod) {
    return bw_state_hash(mod->chunk_hashes,
                         (size_t)mod->num_chunk_ranges * sizeof(u64), 0u);
}

static void host_state_header(HostStateHeader* header, const CPUState* cpu,
                              const StaticRecompModuleDesc* mod) {
    memset(header, 0, sizeof(*header));
    header->version = 1u;
    header->cpu_state_size = (u32)sizeof(CPUState);
    header->cpu_pod_size = (u32)offsetof(CPUState, external_read);
    header->entry_point = mod->entry_point;
    header->chunk_count = mod->num_chunk_ranges;
    header->ram_size = cpu->ram_size;
    header->chunk_digest = host_state_chunk_digest(mod);
    header->retrace = g_host_retrace_count;
    header->pc = cpu->pc;
    header->mod_mask = g_mod_mask;
#ifdef BLUEWAKE_HAS_DSP_ADAPTER
    header->dsp_hle = bluewake_dsp_adapter_is_hle(g_dsp_adapter) ? 1u : 0u;
#endif
    header->aurora = g_state_aurora ? 1u : 0u;
    memcpy(header->game_id, mod->game_id, sizeof header->game_id);
}

static u64 host_state_now_us(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (u64)now.tv_sec * 1000000ull + (u64)now.tv_nsec / 1000ull;
}

static bool host_state_save(const char* path, CPUState* cpu,
                            const StaticRecompModuleDesc* mod,
                            const HostStateLoop* loop) {
    const u64 start_us = host_state_now_us();
    // The GX blob first: it drains the translation worker (and may present the
    // frame it finished), which touches no guest state.
    void* gx = NULL;
    const size_t gx_size = g_state_aurora ? dol_aurora_gx_save_state(&gx) : 0u;
    if (g_state_aurora && gx_size == 0u)
        fprintf(stderr, "[state] warning: no GX front-end state (renderer path off or failed)\n");
    BwStateWriter* writer = bw_state_writer_open(path);
    if (writer == NULL) {
        fprintf(stderr, "[state] cannot write %s\n", path);
        free(gx);
        return false;
    }
    HostStateHeader header;
    host_state_header(&header, cpu, mod);
    bool ok = bw_state_write_chunk(writer, "HEADER", &header, sizeof header);
    ok = ok && bw_state_write_chunk(writer, "CPU", cpu, header.cpu_pod_size);
    ok = ok && bw_state_write_chunk(writer, "MEM1", cpu->ram, cpu->ram_size);
    if (aram_buffer() != NULL)
        ok = ok && bw_state_write_chunk(writer, "ARAM", aram_buffer(), ARAM_SIZE);
    // Aliases: count, then (start, size, bytes) in registration order.
    u64 alias_bytes = 4u;
    for (u32 i = 0; i < g_state_alias_count; ++i)
        alias_bytes += 8u + g_state_aliases[i].size;
    u8* aliases = (u8*)malloc((size_t)alias_bytes);
    if (aliases == NULL) {
        ok = false;
    } else {
        u8* at = aliases;
        memcpy(at, &g_state_alias_count, 4u);
        at += 4u;
        for (u32 i = 0; i < g_state_alias_count && ok; ++i) {
            u8* storage = NULL;
            const HostStateAlias alias = g_state_aliases[i];
            if (!ppc_guest_alias_get_storage(alias.linked_start, alias.size, &storage) ||
                storage == NULL) {
                fprintf(stderr, "[state] alias 0x%08X+0x%X has no storage\n",
                        alias.linked_start, alias.size);
                ok = false;
                break;
            }
            memcpy(at, &alias.linked_start, 4u);
            memcpy(at + 4u, &alias.size, 4u);
            memcpy(at + 8u, storage, alias.size);
            at += 8u + alias.size;
        }
        ok = ok && bw_state_write_chunk(writer, "ALIASES", aliases, alias_bytes);
        free(aliases);
    }
    if (g_cycle_vi_clock != NULL)
        ok = ok && bw_state_write_chunk(writer, "VICLOCK", g_cycle_vi_clock,
                                        sizeof(*g_cycle_vi_clock));
    u8* vars = NULL;
    u64 vars_size = 0u;
    if (bw_state_fields_pack(k_host_state_fields,
                             (u32)(sizeof k_host_state_fields / sizeof k_host_state_fields[0]),
                             &vars, &vars_size)) {
        ok = ok && bw_state_write_chunk(writer, "HOSTVARS", vars, vars_size);
        free(vars);
    } else {
        ok = false;
    }
    const BwStateField loop_fields[] = {
        {"profile_prolog_called", loop->profile_prolog_called, sizeof(bool)},
        {"rel_prolog_sda_pending", loop->rel_prolog_sda_pending, sizeof(bool)},
        {"rel_prolog_saved_r13", loop->rel_prolog_saved_r13, sizeof(u32)},
    };
    if (bw_state_fields_pack(loop_fields, 3u, &vars, &vars_size)) {
        ok = ok && bw_state_write_chunk(writer, "LOOPVARS", vars, vars_size);
        free(vars);
    } else {
        ok = false;
    }
#ifdef BLUEWAKE_HAS_DSP_ADAPTER
    if (g_dsp_adapter != NULL) {
        if (!bluewake_dsp_adapter_is_hle(g_dsp_adapter)) {
            fprintf(stderr, "[state] warning: the DSP runs LLE, whose state is not saved "
                            "(BLUEWAKE_DSP_MODE=hle); audio will not survive a load\n");
        } else {
            u8* dsp = NULL;
            const size_t dsp_size = bluewake_dsp_adapter_save_state(g_dsp_adapter, &dsp);
            if (dsp_size == 0u) {
                fprintf(stderr, "[state] DSP HLE state failed\n");
                ok = false;
            } else {
                ok = ok && bw_state_write_chunk(writer, "DSPHLE", dsp, dsp_size);
            }
            free(dsp);
        }
    }
#endif
    if (gx_size != 0u)
        ok = ok && bw_state_write_chunk(writer, "GX", gx, gx_size);
    free(gx);
    ok = bw_state_writer_close(writer) && ok;
    if (!ok) {
        fprintf(stderr, "[state] save to %s failed\n", path);
        return false;
    }
    snprintf(g_state_last_path, sizeof g_state_last_path, "%s", path);
    FILE* file = fopen(path, "rb");
    long file_size = -1;
    if (file != NULL) {
        fseek(file, 0, SEEK_END);
        file_size = ftell(file);
        fclose(file);
    }
    fprintf(stderr,
            "[state] saved %s retrace=%llu pc=0x%08X mem1=0x%08X aliases=%u gx=%zu "
            "bytes=%ld ms=%llu\n",
            path, (unsigned long long)g_host_retrace_count, cpu->pc,
            (u32)bw_state_hash(cpu->ram, cpu->ram_size, 0u), g_state_alias_count,
            gx_size, file_size,
            (unsigned long long)((host_state_now_us() - start_us) / 1000u));
    return true;
}

static bool host_state_load(const char* path, CPUState* cpu,
                            const StaticRecompModuleDesc* mod,
                            const HostStateLoop* loop) {
    const u64 start_us = host_state_now_us();
    BwStateReader reader;
    if (!bw_state_reader_open(&reader, path))
        return false;
    bool ok = false;
    const BwStateChunk* chunk = bw_state_find(&reader, "HEADER");
    HostStateHeader saved;
    HostStateHeader here;
    host_state_header(&here, cpu, mod);
    if (chunk == NULL || chunk->size != sizeof saved) {
        fprintf(stderr, "[state] %s has no usable header\n", path);
        goto done;
    }
    memcpy(&saved, chunk->data, sizeof saved);
    if (saved.version != here.version || saved.cpu_state_size != here.cpu_state_size ||
        saved.cpu_pod_size != here.cpu_pod_size || saved.ram_size != here.ram_size) {
        fprintf(stderr,
                "[state] %s does not fit this host (version %u/%u, CPUState %u/%u, "
                "RAM 0x%X/0x%X)\n",
                path, saved.version, here.version, saved.cpu_state_size,
                here.cpu_state_size, saved.ram_size, here.ram_size);
        goto done;
    }
    if (saved.chunk_digest != here.chunk_digest || saved.chunk_count != here.chunk_count ||
        memcmp(saved.game_id, here.game_id, sizeof saved.game_id) != 0) {
        const char* force = getenv("BLUEWAKE_LOAD_STATE_FORCE");
        fprintf(stderr,
                "[state] %s was made by another translation of the game "
                "(chunks %u/%u, digest %016llX/%016llX)%s\n",
                path, saved.chunk_count, here.chunk_count,
                (unsigned long long)saved.chunk_digest,
                (unsigned long long)here.chunk_digest,
                force != NULL && force[0] == '1' ? "; loading anyway (FORCE)" : "");
        if (force == NULL || force[0] != '1')
            goto done;
    }
    if (saved.mod_mask != here.mod_mask)
        fprintf(stderr,
                "[state] warning: saved with mods 0x%X, this run has 0x%X (their code "
                "differs; per-frame mod writes follow this run)\n",
                saved.mod_mask, here.mod_mask);
    if (saved.dsp_hle != here.dsp_hle)
        fprintf(stderr, "[state] warning: saved with DSP %s, this run is %s\n",
                saved.dsp_hle ? "HLE" : "LLE/off", here.dsp_hle ? "HLE" : "LLE/off");

    chunk = bw_state_find(&reader, "CPU");
    if (chunk == NULL || chunk->size != saved.cpu_pod_size) {
        fprintf(stderr, "[state] %s: CPU chunk missing\n", path);
        goto done;
    }
    const BwStateChunk* mem1 = bw_state_find(&reader, "MEM1");
    if (mem1 == NULL || mem1->size != cpu->ram_size) {
        fprintf(stderr, "[state] %s: MEM1 chunk missing\n", path);
        goto done;
    }
    // Past this point the machine is being replaced; a failure is reported
    // and the load stops, which leaves an inconsistent machine. The FIFO
    // worker finishes what it has first: it reads guest memory (display
    // lists, vertex arrays, textures) as it translates.
    if (g_state_aurora)
        dol_aurora_gx_drain();
    memcpy(cpu, chunk->data, saved.cpu_pod_size);
    memcpy(cpu->ram, mem1->data, cpu->ram_size);
    chunk = bw_state_find(&reader, "ARAM");
    if (chunk != NULL && aram_buffer() != NULL && chunk->size == ARAM_SIZE)
        memcpy(aram_buffer(), chunk->data, ARAM_SIZE);
    else
        fprintf(stderr, "[state] warning: no ARAM in the state\n");

    chunk = bw_state_find(&reader, "ALIASES");
    if (chunk == NULL || chunk->size < 4u) {
        fprintf(stderr, "[state] %s: alias chunk missing\n", path);
        goto done;
    }
    {
        u32 count = 0u;
        memcpy(&count, chunk->data, 4u);
        u64 offset = 4u;
        u32 added = 0u;
        for (u32 i = 0; i < count; ++i) {
            if (offset + 8u > chunk->size) {
                fprintf(stderr, "[state] %s: alias chunk truncated\n", path);
                goto done;
            }
            u32 start = 0u, size = 0u;
            memcpy(&start, chunk->data + offset, 4u);
            memcpy(&size, chunk->data + offset + 4u, 4u);
            offset += 8u;
            if (offset + size > chunk->size) {
                fprintf(stderr, "[state] %s: alias chunk truncated\n", path);
                goto done;
            }
            u8* storage = NULL;
            if (!ppc_guest_alias_get_storage(start, size, &storage) || storage == NULL) {
                // Registered later in the saving run (module 336's BSS).
                if (!host_add_shared_guest_alias(start, size, NULL) ||
                    !ppc_guest_alias_get_storage(start, size, &storage) || storage == NULL) {
                    fprintf(stderr, "[state] cannot register alias 0x%08X+0x%X\n", start, size);
                    goto done;
                }
                added++;
            }
            memcpy(storage, chunk->data + offset, size);
            offset += size;
        }
        if (count != g_state_alias_count)
            fprintf(stderr, "[state] aliases: %u in the state, %u here (%u added)\n", count,
                    g_state_alias_count, added);
    }

    chunk = bw_state_find(&reader, "VICLOCK");
    if (chunk != NULL && g_cycle_vi_clock != NULL && chunk->size == sizeof(*g_cycle_vi_clock))
        memcpy(g_cycle_vi_clock, chunk->data, sizeof(*g_cycle_vi_clock));
    else
        fprintf(stderr, "[state] warning: no VI clock in the state\n");

    chunk = bw_state_find(&reader, "HOSTVARS");
    uint32_t restored = 0u, missing = 0u, mismatched = 0u;
    if (chunk == NULL ||
        !bw_state_fields_unpack(k_host_state_fields,
                                (u32)(sizeof k_host_state_fields / sizeof k_host_state_fields[0]),
                                chunk->data, chunk->size, &restored, &missing, &mismatched)) {
        fprintf(stderr, "[state] %s: host variables unreadable\n", path);
        goto done;
    }
    chunk = bw_state_find(&reader, "LOOPVARS");
    if (chunk != NULL) {
        const BwStateField loop_fields[] = {
            {"profile_prolog_called", loop->profile_prolog_called, sizeof(bool)},
            {"rel_prolog_sda_pending", loop->rel_prolog_sda_pending, sizeof(bool)},
            {"rel_prolog_saved_r13", loop->rel_prolog_saved_r13, sizeof(u32)},
        };
        (void)bw_state_fields_unpack(loop_fields, 3u, chunk->data, chunk->size, NULL, NULL,
                                     NULL);
    }
#ifdef BLUEWAKE_HAS_DSP_ADAPTER
    chunk = bw_state_find(&reader, "DSPHLE");
    if (chunk != NULL && g_dsp_adapter != NULL && bluewake_dsp_adapter_is_hle(g_dsp_adapter)) {
        if (!bluewake_dsp_adapter_load_state(g_dsp_adapter, chunk->data, (size_t)chunk->size))
            fprintf(stderr, "[state] warning: the DSP HLE state did not load; audio may stop\n");
    } else if (g_dsp_adapter != NULL) {
        fprintf(stderr, "[state] warning: no DSP HLE state loaded; audio may stop\n");
    }
#endif
    chunk = bw_state_find(&reader, "GX");
    if (g_state_aurora) {
        if (chunk == NULL)
            fprintf(stderr, "[state] warning: no GX state: the next frames decode from "
                            "reset GX registers\n");
        else if (!dol_aurora_gx_load_state(chunk->data, (size_t)chunk->size))
            fprintf(stderr, "[state] warning: the GX state did not load\n");
    }

    // What the machine derives from the state rather than holds.
    if (mod->on_state_loaded != NULL)
        mod->on_state_loaded(cpu);
    g_overlap_cached_alias_state = 0xFFFFFFFFu;
    g_overlap_cached_object = 0u;
    g_overlap_slot_ptr = NULL;
    g_overlap_fields_ptr = NULL;
    g_published_interrupt_sources_valid = false;
    g_interrupt_sources_dirty = true;
    host_publish_interrupt_sources();
    if (g_audio_dma.sample_rate != 0u)
        dol_platform_audio_set_sample_rate(g_audio_dma.sample_rate);
    dol_guest_memory_dirty_mark(0x80000000u, cpu->ram_size);
    if (g_state_aurora)
        dol_aurora_set_fast_forward(false);
    snprintf(g_state_last_path, sizeof g_state_last_path, "%s", path);
    ok = true;
    fprintf(stderr,
            "[state] loaded %s retrace=%llu pc=0x%08X mem1=0x%08X fields=%u missing=%u "
            "mismatched=%u ms=%llu\n",
            path, (unsigned long long)g_host_retrace_count, cpu->pc,
            (u32)bw_state_hash(cpu->ram, cpu->ram_size, 0u), restored, missing, mismatched,
            (unsigned long long)((host_state_now_us() - start_us) / 1000u));
done:
    bw_state_reader_close(&reader);
    return ok;
}

// Where F5 saves (BLUEWAKE_STATE_DIR, else the working directory).
static const char* host_state_dir(void) {
    const char* dir = getenv("BLUEWAKE_STATE_DIR");
    return dir != NULL && dir[0] != '\0' ? dir : ".";
}

// The newest .bwstate in the state directory: what F9 loads when this run has
// neither saved nor loaded one (after a relaunch).
static bool host_state_latest(char* out, size_t size) {
    DIR* dir = opendir(host_state_dir());
    if (dir == NULL)
        return false;
    time_t newest = 0;
    bool found = false;
    for (struct dirent* entry = readdir(dir); entry != NULL; entry = readdir(dir)) {
        const size_t length = strlen(entry->d_name);
        if (length < 9u || strcmp(entry->d_name + length - 8u, ".bwstate") != 0)
            continue;
        char path[1100];
        snprintf(path, sizeof path, "%s/%s", host_state_dir(), entry->d_name);
        struct stat info;
        if (stat(path, &info) != 0 || !S_ISREG(info.st_mode))
            continue;
        if (!found || info.st_mtime >= newest) {
            newest = info.st_mtime;
            snprintf(out, size, "%s", path);
            found = true;
        }
    }
    closedir(dir);
    return found;
}

// BLUEWAKE_STATE_TEST_LOAD=retrace (testing only): F9 at that retrace, once.
static u64 g_state_test_load_retrace;
static bool g_state_test_load_done;

// BLUEWAKE_SAVE_STATE=path@retrace[,path@retrace...]
static void host_state_parse_requests(void) {
    const char* test_load = getenv("BLUEWAKE_STATE_TEST_LOAD");
    if (test_load != NULL && test_load[0] != '\0')
        g_state_test_load_retrace = strtoull(test_load, NULL, 10);
    const char* env = getenv("BLUEWAKE_SAVE_STATE");
    if (env == NULL || env[0] == '\0')
        return;
    const char* at = env;
    while (*at != '\0' && g_state_request_count < HOST_STATE_MAX_REQUESTS) {
        const char* end = strchr(at, ',');
        const size_t length = end != NULL ? (size_t)(end - at) : strlen(at);
        char item[1100];
        if (length < sizeof item) {
            memcpy(item, at, length);
            item[length] = '\0';
            char* sep = strrchr(item, '@');
            char* number_end = NULL;
            const unsigned long long retrace =
                sep != NULL ? strtoull(sep + 1, &number_end, 10) : 0ull;
            if (sep != NULL && sep != item && number_end != sep + 1 && *number_end == '\0' &&
                (size_t)(sep - item) < sizeof g_state_requests[0].path) {
                HostStateRequest* request = &g_state_requests[g_state_request_count++];
                memcpy(request->path, item, (size_t)(sep - item));
                request->path[sep - item] = '\0';
                request->retrace = retrace;
                request->done = false;
                fprintf(stderr, "[state] will save %s at retrace %llu\n", request->path,
                        (unsigned long long)retrace);
            } else {
                fprintf(stderr, "[state] ignoring BLUEWAKE_SAVE_STATE entry \"%s\" "
                                "(want path@retrace)\n", item);
            }
        }
        if (end == NULL)
            break;
        at = end + 1;
    }
}

// F5 / F9 from the window's event observer (mouse_camera.c), main thread.
// The load key is F8 on Windows, where F9 is the frame rate.
#if defined(_WIN32)
#define HOST_STATE_LOAD_KEY "F8"
#else
#define HOST_STATE_LOAD_KEY "F9"
#endif
void bluewake_save_state_hotkey(bool load) {
    if (load)
        g_state_hotkey_load = true;
    else
        g_state_hotkey_save = true;
}

// What must not be in flight when the machine is written or replaced.
static const char* host_state_unsafe_reason(const CPUState* cpu,
                                            const HostStateLoop* loop) {
    if (cpu->exception != 0u)
        return "exception pending";
    if (*loop->rel_prolog_sda_pending)
        return "REL prolog in progress";
    if (!dol_hle_callback_idle())
        return "memory card callback in flight";
    if (bluewake_quick_doors_busy())
        return "quick door in progress";
    if (mem_read32((CPUState*)cpu, 0x803F6160u) >= 0x80000000u)
        return "scene change in progress";
    return NULL;
}

// Once per retrace: is a save due?
static void host_state_arm(void) {
    if (g_state_save_armed)
        return;
    bool due = g_state_hotkey_save;
    for (u32 i = 0; i < g_state_request_count && !due; ++i)
        due = !g_state_requests[i].done && g_host_retrace_count >= g_state_requests[i].retrace;
    if (due) {
        g_state_save_armed = true;
        g_state_armed_retrace = g_host_retrace_count;
        g_state_refusals = 0u;
    }
}

// At the top of a host turn while a save is due.
static void host_state_try_save(CPUState* cpu, const StaticRecompModuleDesc* mod,
                                const HostStateLoop* loop, bool retrace_boundary) {
    const bool draw_done = cpu->pc == HOST_STATE_DRAW_DONE_PC;
    const u64 waited = g_host_retrace_count - g_state_armed_retrace;
    if (!draw_done && !(retrace_boundary && waited >= 120u))
        return;
    const char* unsafe = host_state_unsafe_reason(cpu, loop);
    if (unsafe != NULL) {
        if (retrace_boundary && g_state_refusals++ % 60u == 0u)
            fprintf(stderr, "[state] save waiting: %s (retrace %llu)\n", unsafe,
                    (unsigned long long)g_host_retrace_count);
        if (waited > 3600u) {
            fprintf(stderr, "[state] no clean point for a minute; save dropped\n");
            for (u32 i = 0; i < g_state_request_count; ++i)
                if (g_state_requests[i].retrace <= g_host_retrace_count)
                    g_state_requests[i].done = true;
            g_state_hotkey_save = false;
            g_state_save_armed = false;
        }
        return;
    }
    if (!draw_done)
        fprintf(stderr, "[state] no draw-done point for %llu retraces; saving at a "
                        "retrace boundary\n", (unsigned long long)waited);
    for (u32 i = 0; i < g_state_request_count; ++i) {
        HostStateRequest* request = &g_state_requests[i];
        if (!request->done && g_host_retrace_count >= request->retrace) {
            host_state_save(request->path, cpu, mod, loop);
            request->done = true;
        }
    }
    if (g_state_hotkey_save) {
        g_state_hotkey_save = false;
        char path[1100];
        mkdir(host_state_dir(), 0755);
        snprintf(path, sizeof path, "%s/quick-%llu.bwstate", host_state_dir(),
                 (unsigned long long)g_host_retrace_count);
        host_state_save(path, cpu, mod, loop);
    }
    g_state_save_armed = false;
}

// The per-turn hook, before the turn begins.
static inline void host_state_turn(CPUState* cpu, const StaticRecompModuleDesc* mod,
                                   const HostStateLoop* loop) {
    bool retrace_boundary = false;
    if (__builtin_expect(g_host_retrace_count != g_state_poll_retrace, 0)) {
        g_state_poll_retrace = g_host_retrace_count;
        retrace_boundary = true;
        host_state_arm();
        if (g_state_test_load_retrace != 0u && !g_state_test_load_done &&
            g_host_retrace_count >= g_state_test_load_retrace) {
            g_state_test_load_done = true;
            g_state_hotkey_load = true;
        }
        if (g_state_hotkey_load) {
            g_state_hotkey_load = false;
            const char* unsafe = host_state_unsafe_reason(cpu, loop);
            if (g_state_last_path[0] == '\0' &&
                !host_state_latest(g_state_last_path, sizeof g_state_last_path))
                fprintf(stderr, "[state] " HOST_STATE_LOAD_KEY ": no state in %s yet (F5 saves one)\n",
                        host_state_dir());
            else if (unsafe != NULL)
                fprintf(stderr, "[state] " HOST_STATE_LOAD_KEY ": not now (%s)\n", unsafe);
            else if (!host_state_load(g_state_last_path, cpu, mod, loop))
                fprintf(stderr, "[state] " HOST_STATE_LOAD_KEY ": load failed; the machine may be inconsistent\n");
            g_state_poll_retrace = g_host_retrace_count;
            return;
        }
    }
    if (__builtin_expect(g_state_save_armed, 0))
        host_state_try_save(cpu, mod, loop, retrace_boundary);
}

// MEM1 is the module's array (bluewake_composite_guest_mem1): cpu_free must
// not free it.
static bool g_guest_mem1_from_module;
static void host_cpu_free(CPUState* cpu) {
    if (g_guest_mem1_from_module)
        cpu->ram = NULL;
    cpu_free(cpu);
}

int main(int argc, char** argv) {
    // The options menu's saved choices, before anything reads the environment.
    bluewake_settings_load();
    host_apply_aspect();
    const char* host_root = host_resolve_root();
    char dylib_scratch[4096 + 128];
    const char* dylib_path = argc > 1 ? argv[1] : NULL;
    if (dylib_path == NULL) {
        if (snprintf(dylib_scratch, sizeof dylib_scratch,
                     "%s/build/composite-cycle-hybrid-o2-v2/"
                     "gGZLE01_recomp.dylib",
                     host_root) < (int)sizeof dylib_scratch &&
            access(dylib_scratch, R_OK) == 0)
            dylib_path = dylib_scratch;
        else
            dylib_path = "build/composite-lib/gGZLE01_recomp.dylib";
    }
    host_apply_default_env("BLUEWAKE_DOL", host_root, "generated/full/main.dol");
    host_apply_default_env("BLUEWAKE_RELS_DIR", host_root,
                           "generated/full/rels");
    host_apply_default_env("BLUEWAKE_DSP_IROM", host_root,
                           "ref/recompcore/Data/Sys/GC/dsp_rom.bin");
    host_apply_default_env("BLUEWAKE_DSP_COEF", host_root,
                           "ref/recompcore/Data/Sys/GC/dsp_coef.bin");
    host_apply_default_env("BLUEWAKE_DISC", host_root,
                           "ref/The Legend Of Zelda The Wind Waker.iso");
    const char* dol_path   = getenv("BLUEWAKE_DOL");
    // A human plays until they close the window, so an unset budget means no
    // cap at all. BLUEWAKE_MAX_BLOCKS bounds an unattended run; zero is the
    // same as unset. The benchmark and play scripts set it explicitly, so
    // every measured configuration keeps the budget it was measured with.
    // A 50M default used to stop a double-clicked BlueWake.app at retrace 502,
    // mid-cutscene, before the title screen was ever reachable.
    unsigned long long max_blocks = 0ull;
    const char* max_blocks_env = getenv("BLUEWAKE_MAX_BLOCKS");
    if (max_blocks_env != NULL && max_blocks_env[0] != '\0')
        max_blocks = strtoull(max_blocks_env, NULL, 10);
    unsigned long long max_retraces = 0ull;
    const char* max_retraces_env = getenv("BLUEWAKE_MAX_RETRACES");
    if (max_retraces_env != NULL && max_retraces_env[0] != '\0') {
        max_retraces = strtoull(max_retraces_env, NULL, 10);
    }
    if (!dol_path) {
        fprintf(stderr,
                "BLUEWAKE_DOL not set and no generated/full/main.dol was \n"
                "found above this executable; set BLUEWAKE_ROOT or "
                "BLUEWAKE_DOL\n");
        return 1;
    }

    void* lib = dlopen(dylib_path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    if (!bluewake_simulation_init(lib)) { dlclose(lib); return 1; }

    GetModuleFn get_module = (GetModuleFn)dlsym(lib, "staticrecomp_get_module");
    if (!get_module) { fprintf(stderr, "dlsym: %s\n", dlerror()); return 1; }

    const StaticRecompModuleDesc* mod = get_module();
    if (!mod) { fprintf(stderr, "module desc is NULL\n"); return 1; }
    if (mod->abi_version != STATICRECOMP_ABI_VERSION ||
        mod->cpu_abi_version != GXRUNTIME_CPU_ABI_VERSION ||
        mod->cpu_state_size != sizeof(CPUState)) {
        fprintf(stderr,
                "module ABI mismatch: module=%u host=%u cpu_abi=%u/%u "
                "cpu_state_size=%u/%zu\n",
                mod->abi_version, STATICRECOMP_ABI_VERSION,
                mod->cpu_abi_version, GXRUNTIME_CPU_ABI_VERSION,
                mod->cpu_state_size, sizeof(CPUState));
        return 1;
    }
    printf("[host] module: game_id=%s abi=%u cpu_abi=%u entry=%#010x ranges=%u chunks=%u rels=%u\n",
           mod->game_id, mod->abi_version, mod->cpu_abi_version,
           mod->entry_point, mod->num_code_ranges, mod->num_chunk_ranges, mod->num_rel_modules);

    DolHeadlessBackend backend;
    dol_headless_backend_init(&backend);
    bool aurora_enabled = false;
    g_pad_pulse_enabled = false;
    g_pad_pulse_start_retrace = 0u;
    g_pad_pulse_length = 0u;
    g_pad_pulse2_enabled = false;
    g_pad_pulse2_start_retrace = 0u;
    g_pad_pulse2_length = 0u;
    g_pad_pulse3_enabled = false;
    g_pad_pulse3_start_retrace = 0u;
    g_pad_pulse3_length = 0u;
    g_pad_script_count = 0u;
    g_host_retrace_count = 0u;
    g_title_ready_reported = false;
    g_title_ready_retrace = 0u;
    g_file_select_reported = false;
    g_file_select_retrace = 0u;
    g_name_scene_create_reported = false;
    g_name_scene_create_retrace = 0u;
    g_name_scene_object = 0u;
    g_name_scene_execute_reported = false;
    g_name_scene_execute_retrace = 0u;
    g_memcard_check_reported = false;
    g_memcard_check_retrace = 0u;
    g_new_game_intro_reported = false;
    g_new_game_intro_retrace = 0u;
    g_name_input_complete_reported = false;
    g_name_input_complete_retrace = 0u;
    g_name_scene_change_reported = false;
    g_name_scene_change_retrace = 0u;
    g_play_scene_reported = false;
    g_play_scene_retrace = 0u;
    g_opening_complete_reported = false;
    g_opening_complete_retrace = 0u;
    g_outset_room_requested = false;
    g_outset_room_request_retrace = 0u;
    g_name_character_jut_hold_reported = false;
    g_name_character_jut_trigger_reported = false;
    g_name_character_cpad_hold_reported = false;
    g_name_character_cpad_trigger_reported = false;
    bluewake_pad_event_schedule_init(&g_title_pad_pulse);
    bluewake_pad_event_schedule_init(&g_title_confirm_pulse);
    bluewake_pad_event_schedule_init(&g_no_card_dismiss_pulse);
    bluewake_pad_axis_event_schedule_init(&g_no_save_left_pulse);
    bluewake_pad_axis_event_schedule_init(&g_player_stick_x_pulse);
    bluewake_pad_axis_event_schedule_init(&g_player_stick_y_pulse);
    g_player_waypoint_configured = false;
    g_player_waypoint_active = false;
    g_player_ladder_down_configured = false;
    g_player_ladder_entered = false;
    g_player_ladder_move_seen = false;
    memset(&g_player_post_ladder_route, 0,
           sizeof(g_player_post_ladder_route));
    g_player_post_ladder_route_configured = false;
    g_player_post_ladder_route_started = false;
    g_player_post_ladder_route_active = false;
    g_player_post_ladder_route_complete = false;
    g_player_post_ladder_route_arrival_retrace = 0u;
    bluewake_pad_event_schedule_init(&g_player_route_confirm_pulse);
    g_player_route_confirm_configured = false;
    g_player_route_confirm_triggered = false;
    bluewake_pad_event_schedule_init(&g_event_confirm_pulse);
    g_event_confirm_target = -1;
    g_event_confirm_any = false;
    g_event_prompt_trace = false;
    g_event_confirm_prompt_active = false;
    g_event_confirm_demo_prompt_active = false;
    bluewake_pad_event_schedule_init(&g_no_save_confirm_pulse);
    bluewake_pad_event_schedule_init(&g_file_slot_select_pulse);
    bluewake_pad_event_schedule_init(&g_file_start_pulse);
    bluewake_pad_event_schedule_init(&g_name_character_pulse);
    bluewake_pad_event_schedule_init(&g_name_end_pulse);
    bluewake_pad_event_schedule_init(&g_name_confirm_pulse);
    bluewake_pad_event_schedule_init(&g_save_start_pulse);
    bluewake_pad_event_schedule_init(&g_save_confirm_pulse);
    bluewake_pad_event_schedule_init(&g_save_page_pulse);
    bluewake_pad_axis_event_schedule_init(&g_save_stick_x_pulse);
    bluewake_pad_axis_event_schedule_init(&g_save_stick_y_pulse);
    g_save_route_enabled = false;
    g_save_route_state = BLUEWAKE_SAVE_STATE_IDLE;
    g_save_route_hold = 0u;
    g_save_route_retries = 0u;
    g_save_route_steps = 0u;
    g_save_route_control_retrace = 0u;
    g_save_route_next_retrace = 0u;
    g_save_route_menu_retrace = 0u;
    g_save_route_item = 0xFFu;
    g_save_route_mode = 0xFFu;
    g_save_route_proc = 0xFFu;
    g_save_route_status = 0xFFu;
    g_save_route_acted_proc = 0xFFu;
    g_save_route_acted_status = 0xFFu;
    g_save_route_acted_retrace = 0u;
    g_save_route_right_sent = false;
    g_save_route_wait_reports = 0u;
    g_save_route_page_presses = 0u;
    g_save_trigger_start_retrace = 0u;
    g_save_trigger_length = 0u;
    g_save_trigger_value = 0u;
    g_save_route_script_suppressed = false;
    g_pad_pulse_buttons = 0u;
    if (!configure_virtual_pad(&backend))
        return 1;
    const char* stop_after_player_stick_env =
        getenv("BLUEWAKE_STOP_AFTER_PLAYER_STICK");
    const bool stop_after_player_stick =
        stop_after_player_stick_env != NULL &&
        stop_after_player_stick_env[0] != '\0';
    if (stop_after_player_stick &&
        strcmp(stop_after_player_stick_env, "1") != 0) {
        fprintf(stderr, "invalid BLUEWAKE_STOP_AFTER_PLAYER_STICK=%s\n",
                stop_after_player_stick_env);
        return 1;
    }
    const bool player_stick_configured =
        g_player_stick_x_pulse.configured ||
        g_player_stick_y_pulse.configured || g_player_waypoint_configured;
    const u64 player_stick_length = g_player_waypoint_configured
                                        ? g_player_waypoint_length
                                        : (g_player_stick_x_pulse.configured
                                               ? g_player_stick_x_pulse.length
                                               : g_player_stick_y_pulse.length);
    if (stop_after_player_stick && !player_stick_configured) {
        fprintf(stderr,
                "BLUEWAKE_STOP_AFTER_PLAYER_STICK requires "
                "BLUEWAKE_PAD_PLAYER_STICK_X or "
                "BLUEWAKE_PAD_PLAYER_STICK_Y or a player target\n");
        return 1;
    }
    const char* renderer = getenv("BLUEWAKE_RENDERER");
    const char* opening_capture_path =
        getenv("BLUEWAKE_CAPTURE_OPENING_FRAME");
    const bool capture_first_nonblank =
        getenv("BLUEWAKE_CAPTURE_FIRST_NONBLANK") != NULL;
    u64 capture_samples = 0u;
    u64 diagnostic_capture_retrace = 0u;
    const char* diagnostic_capture_retrace_env =
        getenv("BLUEWAKE_CAPTURE_RETRACE");
    if (diagnostic_capture_retrace_env != NULL &&
        diagnostic_capture_retrace_env[0] != '\0') {
        char* end = NULL;
        diagnostic_capture_retrace =
            strtoull(diagnostic_capture_retrace_env, &end, 0);
        if (end == diagnostic_capture_retrace_env || *end != '\0' ||
            diagnostic_capture_retrace == 0u) {
            fprintf(stderr, "invalid BLUEWAKE_CAPTURE_RETRACE=%s\n",
                    diagnostic_capture_retrace_env);
            return 1;
        }
    }
    // Periodic frame series: with an interval set, a diagnostic capture is
    // re-armed after every write so one run yields frames across a boundary
    // instead of a single frame.
    u64 diagnostic_capture_interval = 0u;
    const char* diagnostic_capture_interval_env =
        getenv("BLUEWAKE_CAPTURE_INTERVAL");
    if (diagnostic_capture_interval_env != NULL &&
        diagnostic_capture_interval_env[0] != '\0') {
        char* interval_end = NULL;
        diagnostic_capture_interval =
            strtoull(diagnostic_capture_interval_env, &interval_end, 0);
        if (interval_end == diagnostic_capture_interval_env ||
            *interval_end != '\0' || diagnostic_capture_interval == 0u) {
            fprintf(stderr, "invalid BLUEWAKE_CAPTURE_INTERVAL=%s\n",
                    diagnostic_capture_interval_env);
            return 1;
        }
        if (opening_capture_path == NULL || opening_capture_path[0] == '\0' ||
            diagnostic_capture_retrace == 0u) {
            fprintf(stderr,
                    "BLUEWAKE_CAPTURE_INTERVAL requires "
                    "BLUEWAKE_CAPTURE_OPENING_FRAME and "
                    "BLUEWAKE_CAPTURE_RETRACE\n");
            return 1;
        }
    }
    const char* capture_player_ready_env =
        getenv("BLUEWAKE_CAPTURE_PLAYER_READY");
    const bool capture_player_ready =
        capture_player_ready_env != NULL && capture_player_ready_env[0] != '\0';
    if (capture_player_ready &&
        (strcmp(capture_player_ready_env, "1") != 0 ||
         opening_capture_path == NULL || opening_capture_path[0] == '\0' ||
         diagnostic_capture_retrace != 0u)) {
        fprintf(stderr,
                "BLUEWAKE_CAPTURE_PLAYER_READY=1 requires "
                "BLUEWAKE_CAPTURE_OPENING_FRAME and excludes "
                "BLUEWAKE_CAPTURE_RETRACE\n");
        return 1;
    }
    bool player_ready_capture_scheduled = false;
    // How many further player-ready frames are still owed after the one being
    // taken. Two frames are wanted rather than one so that a fade, a mid-scene
    // black, or any other single bad frame cannot stand in for the picture of
    // Outset under control.
    unsigned player_ready_capture_followups = 0u;
    // The player record probe below is the one observation point for the
    // product's second half -- whether the guest has handed control to Link --
    // and it must not depend on a CPU hook. Every earlier attempt read the
    // player through the player-execute site at 0x80122D30, which no run has
    // ever reached, so "controllable gameplay" had no instrument at all.
    const bool player_probe = getenv("BLUEWAKE_PLAYER_PROBE") != NULL;
    bool opening_capture_requested = false;
    bool opening_capture_complete = false;
    // A human launch must see and hear the game. An explicit BLUEWAKE_RENDERER
    // always wins (the bench asks for "headless"); an empty environment gets
    // the window, the audio device and live input, with a headless fallback
    // when no window server is reachable.
    const bool renderer_requested = renderer != NULL && renderer[0] != '\0';
    if (!renderer_requested || strcmp(renderer, "aurora") == 0) {
        const AuroraBackendConfig aurora_config = {
            .app_name = "BlueWake",
            .window_width = 960u,
            .window_height = 720u,
            .vsync = true,
            .allow_texture_dumps = false,
            .info_logging = true,
            .graphics_logging = getenv("DOL_AURORA_RECOMP_GRAPHICS_LOG") != NULL,
            .force_untextured = false,
        };
        if (dol_aurora_initialize(argc, argv, &aurora_config)) {
            aurora_enabled = true;
            bluewake_simulation_renderer_ready();
            // BLUEWAKE_LIVE_PAD=0 (test runs): the pad script alone, even with a
            // controller connected whose resting axes read as a person.
            // BLUEWAKE_TEST_INPUT_ONLY=1 also leaves out the mouse camera and
            // the settings menu.
            const char* test_input = getenv("BLUEWAKE_TEST_INPUT_ONLY");
            const bool scripted_only = test_input && strcmp(test_input, "1") == 0;
            const char* live_pad = getenv("BLUEWAKE_LIVE_PAD");
            g_live_pad_enabled = !scripted_only && (live_pad == NULL || live_pad[0] != '0');
            if (!scripted_only) {
                bluewake_mouse_camera_install();
                bluewake_settings_menu_install();
            } else {
                fprintf(stderr, "[pad] test input only: physical controls disabled\n");
            }
            fprintf(stderr, "[host] renderer=aurora window=%ux%u\n",
                    aurora_config.window_width, aurora_config.window_height);
        } else if (renderer_requested) {
            fprintf(stderr, "[host] Aurora renderer initialization failed\n");
            return 1;
        } else {
            fprintf(stderr,
                    "[host] Aurora renderer unavailable; falling back to "
                    "headless\n");
            dol_headless_backend_install(&backend);
            fprintf(stderr, "[host] renderer=headless\n");
        }
    } else {
        dol_headless_backend_install(&backend);
        fprintf(stderr, "[host] renderer=headless\n");
    }
    if (!dol_platform_pad_init()) {
        fprintf(stderr, "[pad] platform input initialization failed\n");
        if (aurora_enabled)
            dol_aurora_shutdown();
        return 1;
    }
    fprintf(stderr, "[pad] platform input initialized; live input %s at SI\n",
            g_live_pad_enabled ? "merged" : "disabled");
    g_state_aurora = aurora_enabled;

    const char* card_path = getenv("BLUEWAKE_CARD_PATH");
    if (!bluewake_card_runtime_open(card_path)) {
        if (aurora_enabled)
            dol_aurora_shutdown();
        return 1;
    }
    atexit(bluewake_card_runtime_close);

    // The guest CPU's state: the module's own when it keeps one at a fixed
    // address (cmake/composite/guest_cpu.c; the Windows build's translated
    // code addresses it directly rather than through a pointer), else here.
    CPUState cpu_storage;
    CPUState* (*module_guest_cpu)(void) =
        (CPUState* (*)(void))dlsym(lib, "bluewake_composite_guest_cpu");
    CPUState* const cpu_state = module_guest_cpu != NULL ? module_guest_cpu() : &cpu_storage;
    // For an outside sampler: the guest pc is at this address + offsetof(pc).
    fprintf(stderr, "[host] guest cpu state %p (%s), pc at +%u\n", (void*)cpu_state,
            module_guest_cpu != NULL ? "the module's" : "the host's", (unsigned)offsetof(CPUState, pc));
    // `cpu` names that state for the rest of main (#undef after it).
#define cpu (*cpu_state)
    if (!cpu_init(&cpu)) { fprintf(stderr, "cpu_init failed\n"); return 1; }
    // Guest MEM1 likewise: the module's own array when it keeps one
    // (guest_cpu.c, core/cpu.h's BW_GUEST_MEM1; the translated code reaches
    // RAM through that global, so it must be this memory).
    {
        u8* (*module_guest_mem1)(u32*) = (u8* (*)(u32*))dlsym(lib, "bluewake_composite_guest_mem1");
        u32 mem1_size = 0u;
        u8* mem1 = module_guest_mem1 != NULL ? module_guest_mem1(&mem1_size) : NULL;
        if (mem1 != NULL && mem1_size >= BLUEWAKE_LINKED_RAM_SIZE && mem1_size >= cpu.ram_size) {
            memcpy(mem1, cpu.ram, cpu.ram_size);
            free(cpu.ram);
            cpu.ram = mem1;
            cpu.ram_size = mem1_size;
            g_guest_mem1_from_module = true;
            fprintf(stderr, "[host] guest MEM1 %p (the module's), 0x%08X bytes\n", (void*)mem1, mem1_size);
        } else if (module_guest_mem1 != NULL) {
            // The game code would read and write that array while the rest
            // of the host used other memory: stop rather than run it.
            fprintf(stderr, "[host] the game module's MEM1 (0x%08X bytes) is smaller than the guest's (0x%08X)\n",
                    mem1_size, cpu.ram_size);
            host_cpu_free(&cpu);
            return 1;
        }
    }
    DolViClock vi_clock;
    dol_vi_clock_init(&vi_clock);
    dol_vi_clock_configure(&vi_clock, GUEST_CPU_CYCLES_PER_VI_RETRACE,
                           GUEST_VI_REFRESH_HZ,
                           DOL_VI_DEFAULT_TIMEBASE_HZ);
    dol_audio_dma_init(&g_audio_dma);
    dol_audio_dma_set_work_rate(&g_audio_dma, GUEST_CPU_CYCLES_PER_SECOND);
    bluewake_audio_capture_init(&g_audio_capture,
                                getenv("BLUEWAKE_CAPTURE_AUDIO_WAV"));
    g_audio_capture_failure_reported = false;
    bluewake_ipl_sram_init(&g_ipl_sram, getenv("BLUEWAKE_SRAM"));
    {
        const char* peek_env = getenv("BLUEWAKE_EFB_PEEK");
        g_efb_peek_enabled = aurora_enabled &&
                             !(peek_env != NULL && strcmp(peek_env, "0") == 0);
    }
    if (g_ipl_sram.enabled)
        fprintf(stderr, "[sram] EXI SRAM on (%s): flags=0x%02X sound=%s\n",
                g_ipl_sram.path != NULL ? g_ipl_sram.path : "defaults",
                g_ipl_sram.sram[19],
                (g_ipl_sram.sram[19] & 0x04u) != 0u ? "stereo" : "mono");
    dol_audio_event_init(&g_audio_events);
    g_audio_event_reports = 0;
    g_audio_sequence_reports = 0;
    g_audio_note_reports = 0;
    g_audio_lifecycle_reports = 0;
    g_audio_sequence_contract_reports = 0;
    g_audio_sequence_active_reports = 0;
    g_audio_sequence_active_track = 0u;
    g_audio_sequence_active_pending = false;
    g_audio_sequence_root_init_data = 0u;
    g_audio_sequence_root_init_pending = false;
    g_audio_callback_reports = 0;
    g_audio_start_reports = 0;
    g_audio_frame_boundary_reports = 0;
    g_audio_thread_reports = 0;
    g_audio_dsp_register_reports = 0;
    g_audio_ai_register_reports = 0;
    g_audio_message_reports = 0;
    g_audio_sync_reports = 0;
    g_audio_dma_chunk_reports = 0;
    g_audio_dma_irq_reports = 0;
    g_audio_ai_handler_reports = 0;
    g_audio_thread_active = false;
    aram_init();
    bluewake_aram_dma_init(&g_aram_dma);

    if (cpu.ram_size < BLUEWAKE_LINKED_RAM_SIZE) {
        u8* expanded = (u8*)realloc(cpu.ram, BLUEWAKE_LINKED_RAM_SIZE);
        if (!expanded) { fprintf(stderr, "guest RAM expansion failed\n"); return 1; }
        memset(expanded + cpu.ram_size, 0, BLUEWAKE_LINKED_RAM_SIZE - cpu.ram_size);
        cpu.ram = expanded;
        cpu.ram_size = BLUEWAKE_LINKED_RAM_SIZE;
        fprintf(stderr, "[host] expanded guest RAM to 0x%08X for linked REL data\n",
                cpu.ram_size);
    }
    dol_platform_set_guest_address_resolver(host_graphics_guest_resolve, &cpu);

    g_dsp_control = 0;
    g_dsp_aram_complete = false;
    g_dsp_mail_from = 0u;
    g_dsp_mail_from_pending = false;
    g_dsp_mail_reads_remaining = 0;
    g_dsp_mail_to_high_seen = false;
    g_dsp_mail_to_reports = 0;
    g_dsp_boot_mail_armed = false;
    g_dsp_boot_mail_clear_seen = false;
    g_dsp_boot_handshake_sent = false;
    g_dsp_task_handshake_sent = false;
    g_dsp_task_request_pending = false;
    g_dsp_task_request_armed = false;
    g_dsp_audio_frame_trace_reports = 0;
    g_dsp_dma_guest_trace_reports = 0;
    g_dsp_boot_task_ready = false;
    g_dsp_task_boot_started = false;
    g_dsp_mail_to_high_value = 0u;
    g_dsp_mail_reports = 0;
    g_dsp_irq_reports = 0;
    g_dsp_irq_route_reports = 0;
    g_guest_clock_decrementer = 0u;
    g_guest_clock_decrementer_valid = false;
    g_guest_clock_decrementer_expired = false;
    g_guest_clock_cycle_remainder = 0u;
    g_guest_clock_reports = 0u;
    g_decrementer_context_reports = 0u;
    BLUEWAKE_TRACE_ASSIGN(g_guest_clock_trace, "BLUEWAKE_TRACE_DECREMENTER");
    BLUEWAKE_TRACE_ASSIGN(g_pad_trace, "BLUEWAKE_TRACE_PAD");
    BLUEWAKE_TRACE_ASSIGN(g_pad_wire_trace, "BLUEWAKE_TRACE_PAD_WIRE");
    BLUEWAKE_TRACE_ASSIGN(g_pad_si_trace, "BLUEWAKE_TRACE_PAD_SI");
    BLUEWAKE_TRACE_ASSIGN(g_pad_status_trace, "BLUEWAKE_TRACE_PAD_STATUS");
    BLUEWAKE_TRACE_ASSIGN(g_runqueue_trace, "BLUEWAKE_TRACE_RUNQUEUE");
    BLUEWAKE_TRACE_ASSIGN(g_gx_fifo_trace, "BLUEWAKE_TRACE_GX_FIFO");
    BLUEWAKE_TRACE_ASSIGN(g_room0_trace, "BLUEWAKE_TRACE_ROOM0");
    BLUEWAKE_TRACE_ASSIGN(g_player_trace, "BLUEWAKE_TRACE_PLAYER");
    BLUEWAKE_TRACE_ASSIGN(g_pad_conversion_trace,
                          "BLUEWAKE_TRACE_PAD_CONVERSION");
    BLUEWAKE_TRACE_ASSIGN(g_pad_conversion_all_trace,
                          "BLUEWAKE_TRACE_PAD_CONVERSION_ALL");
    BLUEWAKE_TRACE_ASSIGN(g_scene_overlap_trace,
                          "BLUEWAKE_TRACE_SCENE_OVERLAP");
    BLUEWAKE_TRACE_ASSIGN(g_create_iter_trace, "BLUEWAKE_TRACE_CREATE_ITER");
    BLUEWAKE_TRACE_ASSIGN(g_bg_create_trace, "BLUEWAKE_TRACE_BG_CREATE");
    BLUEWAKE_TRACE_ASSIGN(g_rel_indirect_trace,
                          "BLUEWAKE_TRACE_REL_INDIRECT");
    BLUEWAKE_TRACE_ASSIGN(g_rel_destructors_trace,
                          "BLUEWAKE_TRACE_REL_DESTRUCTORS");
    BLUEWAKE_TRACE_ASSIGN(g_rel_lifecycle_trace,
                          "BLUEWAKE_TRACE_REL_LIFECYCLE");
    BLUEWAKE_TRACE_ASSIGN(g_rel_calls_trace, "BLUEWAKE_TRACE_REL_CALLS");
    BLUEWAKE_TRACE_ASSIGN(g_gx_entry_trace, "BLUEWAKE_TRACE_GX_ENTRY");
    BLUEWAKE_TRACE_ASSIGN(g_dispatch_terminal_trace,
                          "BLUEWAKE_TRACE_DISPATCH_TERMINAL");
    BLUEWAKE_TRACE_ASSIGN(g_alarm_state_trace,
                          "BLUEWAKE_TRACE_ALARM_STATE");
    g_pad_si_data_watch = 0u;
    BLUEWAKE_TRACE_ASSIGN(g_pad_lifecycle_trace,
                          "BLUEWAKE_TRACE_PAD_LIFECYCLE");
    g_pad_lifecycle_reports = 0u;
    g_pad_transfer_reports = 0u;
    g_pad_poll_reports = 0u;
    g_pad_active_reports = 0u;
    g_pad_wire_reports = 0u;
    g_pad_si_data_reports = 0u;
    g_pad_si_status_reports = 0u;
    g_pad_si_reports = 0u;
    g_title_input_reports = 0u;
    g_guest_decrementer_pending = false;
    g_di_read_reports = 0;
    g_dvd_open_reports = 0;
    g_dvd_read_reports = 0;
    g_archive_reports = 0;
    g_dynamic_load_reports = 0;
    g_dynamic_link_header_reported = false;
    g_dyl_state_reports = 0;
    g_rel_lifecycle_trace_reports = 0;
    g_rel_call_trace_reports = 0;
    g_rel_invalid_target_reports = 0;
    g_rel_indirect_trace_reports = 0;
    g_rel_solid_hole_reports = 0;
    g_resource_lookup_reports = 0;
    g_resource_info_reports = 0;
    g_rel_lwood_ctor_watch = 0u;
    memset(g_rel_slots, 0, sizeof(g_rel_slots));
    g_rel_alias_count = 0u;
    g_rel_alias_raw_min = ~0u;
    g_rel_alias_raw_max = 0u;
    g_module1_raw_base = 0u;
    g_module_alias_add_shared = NULL;
    g_module336_bss_alias_installed = false;
    dol_di_init(&g_di);
    dol_si_init(&g_si);
    const char* disc_path = getenv("BLUEWAKE_DISC");
    if (disc_path != NULL && dvd_open_image(disc_path)) {
        dol_di_set_disc_present(&g_di, true);
        fprintf(stderr, "[dvd] disc inserted for DI service\n");
    }
    dol_di_set_command_callback(&g_di, host_di_command, NULL);
    dol_interrupts_init(&g_interrupts);
    g_cycle_vi_clock = &vi_clock;
    // Shipping policy. Measured on the regenerated build with the chassis
    // schedule (docs/status/CURRENT.md, 2026-09-17 "the cycle window is a free
    // lever"): a fixed 4096 window gives 33.24 fps in the play window against
    // 31.38 for the previous dynamic 256-in-1024 policy, 73.91 s against
    // 87.56 s on the route, host turns 57,369,093 against 148,527,447, and the
    // route digest UNCHANGED at every cap tested. 16384 is where the cap stops
    // mattering at all: the window is then bounded by the device deadlines
    // rather than by this value - 2,284 host turns per retrace against 57.4M
    // total at 4096 and 148.5M under the old policy - so the margin is what is
    // left after the deadlines, not a tuned constant. BLUEWAKE_CYCLE_CAP=dynamic
    // restores the previous policy and a number selects a fixed cap.
    s64 cycle_cap = 16384;
    bool cycle_dynamic_cap = false;
    const char* cycle_cap_env = getenv("BLUEWAKE_CYCLE_CAP");
    if (cycle_cap_env != NULL) {
        if (strcmp(cycle_cap_env, "dynamic") == 0) {
            cycle_cap = 1024;
            cycle_dynamic_cap = true;
        } else {
            char* end = NULL;
            const unsigned long requested = strtoul(cycle_cap_env, &end, 10);
            if (end != cycle_cap_env && *end == '\0' && requested > 0u &&
                requested <= 65536u) {
                cycle_cap = (s64)requested;
                cycle_dynamic_cap = false;
            } else
                fprintf(stderr,
                        "[clock] ignoring invalid BLUEWAKE_CYCLE_CAP=\"%s\"\n",
                        cycle_cap_env);
        }
    }
    bluewake_cycle_domain_init(&g_cycle_domain, cycle_cap,
                               host_cycle_advance_clock,
                               host_cycle_deadline_distance, NULL);
    if (bluewake_simulation_enabled())
        bluewake_cycle_domain_set_cpu_multiplier(&g_cycle_domain, 2);
    if (cycle_dynamic_cap)
        bluewake_cycle_domain_set_dynamic_cap(&g_cycle_domain, 256, 1024u);
    bluewake_delivery_digest_init(&g_delivery_digest);
    {
        const char* trace_env = getenv("BLUEWAKE_DELIVERY_TRACE");
        if (trace_env != NULL) {
            char* end = NULL;
            const unsigned long long lo = strtoull(trace_env, &end, 10);
            if (end != trace_env && *end == ':' && lo > 0ull) {
                const char* hi_text = end + 1;
                char* hi_end = NULL;
                const unsigned long long hi = strtoull(hi_text, &hi_end, 10);
                if (hi_end != hi_text && *hi_end == '\0' && hi >= lo)
                    g_delivery_trace_lo = (u64)lo;
                else
                    g_delivery_trace_lo = 0u;
                g_delivery_trace_hi = g_delivery_trace_lo != 0u ? (u64)hi : 0u;
            }
            if (g_delivery_trace_lo != 0u)
                fprintf(stderr,
                        "[clock] delivery trace window=%llu:%llu\n",
                        (unsigned long long)g_delivery_trace_lo,
                        (unsigned long long)g_delivery_trace_hi);
        }
    }
    {
        const char* play_env = getenv("BLUEWAKE_DELIVERY_TRACE_PLAY");
        if (play_env != NULL) {
            char* end = NULL;
            const unsigned long long count = strtoull(play_env, &end, 10);
            if (end != play_env && *end == '\0' && count > 0ull) {
                g_delivery_play_trace = (u64)count;
                fprintf(stderr, "[clock] delivery play trace=%llu\n",
                        (unsigned long long)g_delivery_play_trace);
            }
        }
    }
    g_vi_cycle_cursor = g_cycle_domain.absolute_cycles;
    g_audio_cycle_cursor = g_cycle_domain.absolute_cycles;
    g_dsp_cycle_cursor = g_cycle_domain.absolute_cycles;
    g_current_host_block = 0u;
    g_vi_assert_reports = 0u;
    // BLUEWAKE_CLOCK: where the time base starts. The IPL sets it from the RTC,
    // so OSGetTime() is the local date and time since 2000-01-01 (the OS
    // epoch); saves are stamped with it and the file select shows it. Unset,
    // it starts at 0 (every date is 01/01/2000), the recorded behaviour of the
    // certified route. "now" is the host's local time (as Dolphin does); a
    // number is seconds since 2000-01-01, for reproducible runs.
    {
        const char* clock_env = getenv("BLUEWAKE_CLOCK");
        if (clock_env != NULL && clock_env[0] != '\0' &&
            strcmp(clock_env, "0") != 0) {
            long long seconds = 0;
            if (strcmp(clock_env, "now") == 0) {
                const time_t now = time(NULL);
                struct tm local;
                localtime_r(&now, &local);
#if defined(_WIN32)
                // No tm_gmtoff: the local time read back as UTC is the offset.
                const long long gmtoff = (long long)_mkgmtime(&local) - (long long)now;
#else
                const long long gmtoff = (long long)local.tm_gmtoff;
#endif
                seconds = (long long)now + gmtoff - 946684800ll;
            } else {
                char* clock_end = NULL;
                seconds = strtoll(clock_env, &clock_end, 10);
                if (clock_end == clock_env || *clock_end != '\0')
                    seconds = -1;
            }
            if (seconds < 0) {
                fprintf(stderr, "invalid BLUEWAKE_CLOCK=%s\n", clock_env);
                return 1;
            }
            cpu.timebase = (u64)seconds * (GUEST_CPU_CYCLES_PER_SECOND /
                                           GUEST_CYCLES_PER_TIMEBASE_TICK);
            fprintf(stderr, "[clock] time base starts at %lld s since 2000\n",
                    seconds);
        }
    }
    g_previous_retrace_timebase = cpu.timebase;
    cpu.external_read = host_mmio_read;
    cpu.external_write = host_mmio_write;
    cpu.spr_read = host_spr_read;
    cpu.spr_write = host_spr_write;
    cpu.cache_control = host_cache_control;
    if (cycle_dynamic_cap)
        fprintf(stderr, "[clock] cycle-domain cap=dynamic(256/1024)\n");
    else
        fprintf(stderr, "[clock] cycle-domain cap=%lld\n", (long long)cycle_cap);
    if (g_guest_clock_trace) {
        fprintf(stderr, "[clock] default-on guest timebase/decrementer service installed\n");
    }

    DolLayout layout;
    if (!dol_load_into_ram(&cpu, dol_path, &layout)) { fprintf(stderr, "dol_load failed\n"); return 1; }
    printf("[host] DOL loaded: entry=%#010x bss=[%#010x,%#010x)\n",
           layout.entry_point, layout.bss_address, layout.bss_address + layout.bss_size);
#ifdef BLUEWAKE_HAS_DSP_ADAPTER
    if (!host_dsp_adapter_init(&cpu)) {
        host_cpu_free(&cpu);
        return 1;
    }
#endif

    GetRelDataFn get_rel_data = (GetRelDataFn)dlsym(lib, "staticrecomp_get_rel_data");
    u32 rel_data_count = 0u;
    const BlueWakeRelData* rel_data = get_rel_data ? get_rel_data(&rel_data_count) : NULL;
    g_rel_data = rel_data;
    g_rel_data_count = rel_data_count;
    if (rel_data) {
        GuestAliasClearFn module_alias_clear =
            (GuestAliasClearFn)dlsym(lib, "ppc_guest_alias_clear");
        GuestAliasAddSharedFn module_alias_add_shared =
            (GuestAliasAddSharedFn)dlsym(lib, "ppc_guest_alias_add_shared");
        g_module_alias_add_shared = module_alias_add_shared;
        if (module_alias_clear == NULL || module_alias_add_shared == NULL) {
            fprintf(stderr, "[rel] composite is missing guest-data alias ABI\n");
            return 1;
        }
        module_alias_clear();
        pthread_mutex_lock(&g_guest_alias_lock);
        ppc_guest_alias_clear();
        g_guest_alias_changes++;
        pthread_mutex_unlock(&g_guest_alias_lock);
        u32 rel_storage_alias_count = 0u;
        for (u32 i = 0; i < rel_data_count; ++i) {
            const BlueWakeRelData* image = &rel_data[i];
            if (image->size == 0u || image->linked_start == 0u)
                continue;
            // Module 336 is not loaded on the title/opening route. Its linked
            // BSS must not shadow the retail heap's Stage.arc buffer before
            // the module's own lifecycle materializes it.
            if (image->bytes == NULL && image->module_id == 336u &&
                image->section_index == 6u)
                continue;
            if (g_j2d_payload_flow_trace && image->linked_start < 0x81516E20u &&
                (u64)image->linked_start + image->size > 0x81512AC0u) {
                fprintf(stderr,
                        "[rel-alias-target] bss image=%u start=0x%08X size=0x%08X\n",
                        i, image->linked_start, image->size);
            }
            if (!host_add_shared_guest_alias(image->linked_start, image->size,
                                             image->bytes)) {
                fprintf(stderr,
                        "[rel] failed shared linked storage alias index=%u "
                        "start=0x%08X size=0x%08X\n",
                        i,
                        image->linked_start, image->size);
                return 1;
            }
            rel_storage_alias_count++;
        }
        // The data-image export contains file-backed sections only. BSS
        // sections are described by the REL metadata table; shadow those
        // sections too, while leaving linked executable text at its guest
        // address so dispatch and function pointers remain unchanged.
        for (u32 module_index = 0; module_index < mod->num_rel_modules;
             ++module_index) {
            const StaticRecompRelModule* rel = &mod->rel_modules[module_index];
            for (u32 section_index = 0; section_index < rel->num_sections;
                 ++section_index) {
                const StaticRecompRelSection* section =
                    &rel->sections[section_index];
                if (section->linked_start == 0u || section->size == 0u)
                    continue;
                bool is_code = false;
                for (u32 range_index = 0; range_index < mod->num_code_ranges;
                     ++range_index) {
                    const StaticRecompRange* range =
                        &mod->code_ranges[range_index];
                    const u64 section_end = (u64)section->linked_start + section->size;
                    if ((u64)range->start < section_end &&
                        (u64)section->linked_start < range->end) {
                        is_code = true;
                        break;
                    }
                }
                if (is_code)
                    continue;
                bool is_file_backed = false;
                for (u32 data_index = 0; data_index < rel_data_count;
                     ++data_index) {
                    const BlueWakeRelData* image = &rel_data[data_index];
                    if (image->module_id == section->module_id &&
                        image->section_index == section->section_index) {
                        is_file_backed = true;
                        break;
                    }
                }
                if (is_file_backed)
                    continue;
                if (section->module_id == 336u && section->section_index == 6u)
                    continue;
                if (g_j2d_payload_flow_trace &&
                    section->linked_start < 0x81516E20u &&
                    (u64)section->linked_start + section->size > 0x81512AC0u) {
                    fprintf(stderr,
                            "[rel-alias-target] bss section module=%u section=%u "
                            "start=0x%08X size=0x%08X\n",
                            section->module_id, section->section_index,
                            section->linked_start, section->size);
                }
                if (!host_add_shared_guest_alias(section->linked_start,
                                                 section->size, NULL)) {
                    fprintf(stderr,
                            "[rel] failed to install linked BSS alias module=%u section=%u start=0x%08X size=0x%08X\n",
                            section->module_id, section->section_index,
                            section->linked_start, section->size);
                    return 1;
                }
                rel_storage_alias_count++;
            }
        }
        u32 rel_bss_zero_count = 0u;
        for (u32 i = 0; i < rel_data_count; ++i) {
            const BlueWakeRelData* image = &rel_data[i];
            if (image->size == 0u || image->linked_start == 0u)
                continue;
            if (!image->bytes) {
                rel_bss_zero_count++;
                // The shadow alias is already zeroed. Do not materialize BSS
                // into physical RAM when its linked range overlaps the heap.
                continue;
            }
            for (u32 offset = 0; offset < image->size; ++offset)
                mem_write8(&cpu, image->linked_start + offset, image->bytes[offset]);
        }
        fprintf(stderr,
                "[rel] materialized %u relocated data images (%u exported "
                "BSS ranges, %u linked storage aliases)\n",
                rel_data_count, rel_bss_zero_count, rel_storage_alias_count);
        fprintf(stderr, "[rel] module60 BSS head after materialization=0x%08X\n",
                mem_read32(&cpu, 0x815581A0u));
        fprintf(stderr, "[rel] profile_data0=0x%08X profile_data5=0x%08X\n",
                mem_read32(&cpu, 0x81F80178u), mem_read32(&cpu, 0x81F80178u + 5u * 4u));
    } else {
        fprintf(stderr, "[rel] composite has no relocated data-image export\n");
    }
    boot_setup_os_globals(&cpu, &layout);
    cpu.instruction_fallback = instruction_fallback;
    // The composite dylib carries its own lazy-FP policy instance. Set the
    // architectural enable bit on the shared CPU state so generated FP
    // blocks do not enter the unavailable-FP vector during boot.
    cpu.msr |= PPC_MSR_FP;
    // Keep architectural FP availability checks enabled. The guest already
    // restores MSR[FP] with its context; disabling lazy-FP checks would route
    // an otherwise legal FP instruction through the host resume shortcut,
    // which cannot preserve the interrupted MSR[EE] state.
    ppc_lazy_fp_set_enabled(true);

    printf("[host] starting execution at %#010x\n", mod->entry_point);
    cpu.pc = mod->entry_point;
    BLUEWAKE_TRACE_ASSIGN(g_title_profile_watch,
                          "BLUEWAKE_TRACE_TITLE_PROFILE");
    BLUEWAKE_TRACE_ASSIGN(g_j2d_object_watch, "BLUEWAKE_TRACE_J2D_OBJECTS");
    g_j2d_object_watch_reports = 0u;
    BLUEWAKE_TRACE_ASSIGN(g_j2d_lookup_trace, "BLUEWAKE_TRACE_J2D_LOOKUP");
    g_j2d_lookup_reports = 0u;
    g_j2d_target_reports = 0u;
    g_j2d_file_select_set_active = false;
    g_j2d_resource_reports = 0u;
    g_j2d_tree_reports = 0u;
    g_j2d_create_reports = 0u;
    BLUEWAKE_TRACE_ASSIGN(g_j2d_payload_watch, "BLUEWAKE_TRACE_J2D_PAYLOAD");
    g_j2d_payload_reports = 0u;
    BLUEWAKE_TRACE_ASSIGN(g_j2d_payload_flow_trace,
                          "BLUEWAKE_TRACE_J2D_PAYLOAD_FLOW");
    g_j2d_payload_flow_reports = 0u;
    BLUEWAKE_TRACE_ASSIGN(g_audio_object_watch,
                          "BLUEWAKE_TRACE_AUDIO_OBJECT");
    g_audio_object_watch_reports = 0u;
    g_audio_transition_trace_remaining = 0u;
    if (g_runqueue_trace || g_pad_lifecycle_trace ||
        g_pad_si_trace || g_pad_status_trace ||
        g_title_profile_watch ||
        g_j2d_object_watch || g_j2d_payload_watch || g_j2d_payload_flow_trace ||
        g_audio_object_watch) {
        ppc_set_mem_write_journal(heap_write_watch, &cpu);
        SetMemWriteJournalFn set_module_journal =
            (SetMemWriteJournalFn)dlsym(lib, "bluewake_set_mem_write_journal");
        fprintf(stderr, "[heap-journal] module-bridge=%s\n",
                set_module_journal ? "installed" : "missing");
        if (set_module_journal)
            set_module_journal(heap_write_watch, &cpu);
    }

    // The chassis schedule is the shipping default: one host turn per cycle
    // budget window rather than per guest block. Same build, same card:
    // play-window median 31.99 fps against 16.14 for the per-block schedule,
    // route digest UNCHANGED in both, 217/217 host tests pass. The per-turn
    // work the guest depends on is reproduced between blocks by
    // host_chassis_edge_service. BLUEWAKE_PER_BLOCK_TURNS=1 restores one turn
    // per block for a regression. (BLUEWAKE_CHASSIS_BUDGET is still honoured so
    // existing scripts keep working.)
    if (getenv("BLUEWAKE_PER_BLOCK_TURNS") == NULL) {
        g_chassis_service_each_block =
            getenv("BLUEWAKE_CHASSIS_SERVICE") != NULL;
        SetEdgeServiceFn set_edge_service =
            (SetEdgeServiceFn)dlsym(lib, "bluewake_set_edge_service");
        fprintf(stderr, "[chassis] edge-service=%s\n",
                set_edge_service ? "installed" : "missing");
        if (g_chassis_service_each_block)
            fprintf(stderr, "[chassis] service-each-block=on\n");
        if (set_edge_service)
            set_edge_service(host_chassis_edge_service, &cpu);
    }
    host_mods_enable(lib, &cpu);
    bluewake_game_options_enable(lib, &cpu, g_options_mod);
    bluewake_forest_water_set_ftree_text(
        host_rel_section_linked_start(mod, 317u, 1u));
    bluewake_forest_water_reload();
    bluewake_mouse_camera_attach(&cpu);
    bluewake_climb_attach(&cpu);
    bluewake_jump_button_attach(&cpu);
    bluewake_sprint_attach(&cpu);
    bluewake_fps_watch_attach(&cpu);
    bluewake_fast_load_attach(&cpu);
    bluewake_quick_doors_attach(&cpu);
    bluewake_draw_tags_attach(&cpu);
    bluewake_haptics_attach(&cpu);

    unsigned long long blocks = 0;
    const char* stop_reason = NULL;
    const bool return_census_enabled =
        getenv("BLUEWAKE_RETURN_CENSUS") != NULL;
    g_frame_timing_enabled = getenv("BLUEWAKE_FRAME_TIMING") != NULL;
    g_perf_log_enabled = getenv("BLUEWAKE_PERF_LOG") != NULL;
    g_wall_pace_enabled = getenv("BLUEWAKE_WALL_PACE") != NULL &&
                          strcmp(getenv("BLUEWAKE_WALL_PACE"), "0") != 0;
    g_async_draw_done = getenv("BLUEWAKE_ASYNC_DRAW_DONE") == NULL ||
                        strcmp(getenv("BLUEWAKE_ASYNC_DRAW_DONE"), "0") != 0;
    g_input_log_enabled = getenv("BLUEWAKE_INPUT_LOG") != NULL &&
                          strcmp(getenv("BLUEWAKE_INPUT_LOG"), "0") != 0;
    pc_sample_start(&cpu);
    g_gx_flush_census = getenv("BLUEWAKE_GX_FLUSH_CENSUS") != NULL;
    if (getenv("BLUEWAKE_CREDIT_CENSUS") != NULL) {
        g_cycle_credit_census = true;
        g_turn_census_enabled = true;
        {
            const char* window_env = getenv("BLUEWAKE_CREDIT_CENSUS_WINDOW");
            if (window_env != NULL && window_env[0] != '\0')
                g_credit_census_window = strtoull(window_env, NULL, 10);
        }
        atexit(report_credit_census);
    }
    g_deadline_census_enabled = getenv("BLUEWAKE_DEADLINE_CENSUS") != NULL;
    {
        const char* native = getenv("BLUEWAKE_ACTOR_SEARCH_NATIVE");
        g_actor_search_native = native == NULL || strcmp(native, "0") != 0;
    }
    {
        const char* overlap = getenv("BLUEWAKE_OVERLAP_OBSERVATION");
        g_overlap_observation = overlap == NULL || strcmp(overlap, "0") != 0;
    }
    g_boundary_census_enabled = getenv("BLUEWAKE_BOUNDARY_CENSUS") != NULL;
    if (g_boundary_census_enabled && getenv("BLUEWAKE_BOUNDARY_CENSUS_BY_ADDRESS") != NULL)
        g_boundary_by_address = calloc(BOUNDARY_ADDRESS_WORDS, sizeof(u32));
    {
        const char* boundary_window_env =
            getenv("BLUEWAKE_BOUNDARY_CENSUS_WINDOW");
        if (boundary_window_env != NULL) {
            char* boundary_window_end = NULL;
            const unsigned long long requested_window =
                strtoull(boundary_window_env, &boundary_window_end, 10);
            if (boundary_window_end != boundary_window_env &&
                *boundary_window_end == '\0')
                g_boundary_census_window = requested_window;
        }
    }
    g_delivery_safety_census_enabled =
        getenv("BLUEWAKE_DELIVERY_SAFETY_CENSUS") != NULL;
    g_guest_state_trace_enabled =
        getenv("BLUEWAKE_GUEST_STATE_TRACE") != NULL;
    // Direct calls between chunks (cmake/composite/direct_calls.h, the Windows
    // builder's scripts/windows/direct_calls.py): at a call into another chunk
    // the module skips the chassis loop when this host's edge service would
    // have nothing to do, which it reads through these flags. Off in every
    // mode that needs the service, or a turn, at each boundary, and with
    // BLUEWAKE_DIRECT_CALLS=0.
    {
        typedef int (*DirectCallsFn)(bool, const bool*, const bool*, const u32*, const u32*);
        DirectCallsFn direct_calls = (DirectCallsFn)dlsym(lib, "bluewake_composite_direct_calls");
        const char* direct_env = getenv("BLUEWAKE_DIRECT_CALLS");
        const bool want = (direct_env == NULL || strcmp(direct_env, "0") != 0) &&
                          getenv("BLUEWAKE_PER_BLOCK_TURNS") == NULL &&
                          !g_chassis_service_each_block && !g_turn_census_enabled &&
                          !g_boundary_census_enabled &&
                          !g_deadline_census_enabled && !g_delivery_safety_census_enabled &&
                          !g_guest_state_trace_enabled && !g_gx_entry_trace;
        if (direct_calls != NULL)
            fprintf(stderr, "[chassis] direct-calls=%s\n",
                    direct_calls(want, &g_interrupt_sources_dirty, &g_guest_decrementer_pending,
                                 &g_interrupts.pi_cause, &g_interrupts.pi_mask)
                        ? "on"
                        : "off");
        // With them, the module's own test of this service's quiet state and
        // watched addresses at every other boundary, and indirect calls run
        // directly (the builder's watch list; cmake/composite/direct_calls.h).
        typedef int (*EdgeFilterFn)(bool);
        EdgeFilterFn edge_filter = (EdgeFilterFn)dlsym(lib, "bluewake_composite_edge_filter");
        if (edge_filter != NULL)
            fprintf(stderr, "[chassis] edge-filter=%s\n", edge_filter(want) ? "on" : "off");
    }
    // Gather-pipe stores straight to the GX writer (cmake/composite/gather_pipe.h):
    // host_mmio_write does nothing else for them unless the FIFO trace is on.
    // Where the GX backend takes the FIFO as a byte stream, the module collects
    // them and hands over a run of bytes at a time (gather_pipe_batch.h);
    // BLUEWAKE_GATHER_PIPE_BATCH=0 sends each store on its own.
    {
        typedef void (*SetGatherPipeFn)(void (*)(u64, u8));
        typedef void (*SetGatherPipeBytesFn)(void (*)(const u8*, u32));
        SetGatherPipeFn set_gather_pipe = (SetGatherPipeFn)dlsym(lib, "bluewake_composite_set_gather_pipe");
        SetGatherPipeBytesFn set_gather_pipe_bytes =
            (SetGatherPipeBytesFn)dlsym(lib, "bluewake_composite_set_gather_pipe_bytes");
        if (set_gather_pipe != NULL)
            set_gather_pipe(g_gx_fifo_trace ? NULL : dol_platform_gx_write);
        if (set_gather_pipe != NULL && set_gather_pipe_bytes != NULL) {
            const char* batch_env = getenv("BLUEWAKE_GATHER_PIPE_BATCH");
            const bool batch = !g_gx_fifo_trace && dol_platform_gx_write_bytes_available() &&
                               !(batch_env != NULL && batch_env[0] == '0');
            set_gather_pipe_bytes(batch ? dol_platform_gx_write_bytes : NULL);
            fprintf(stderr, "[chassis] gather-pipe-batch=%s\n", batch ? "on" : "off");
        }
    }
#ifdef BLUEWAKE_HAS_DSP_ADAPTER
    {
        const char* dsp_rate_env = getenv("BLUEWAKE_DSP_RATE");
        if (dsp_rate_env != NULL) {
            char* dsp_rate_end = NULL;
            const unsigned long requested = strtoul(dsp_rate_env, &dsp_rate_end, 10);
            if (dsp_rate_end != dsp_rate_env && *dsp_rate_end == '\0' && requested > 0u)
                g_dsp_update_rate = (u64)requested;
        }
    }
#endif
    const char* deadline_census_window_env =
        getenv("BLUEWAKE_DEADLINE_CENSUS_WINDOW");
    if (deadline_census_window_env != NULL) {
        char* deadline_census_window_end = NULL;
        const unsigned long long requested_window =
            strtoull(deadline_census_window_env,
                     &deadline_census_window_end, 10);
        if (deadline_census_window_end != deadline_census_window_env &&
            *deadline_census_window_end == '\0')
            g_deadline_census_window = requested_window;
    }
    BluewakeReturnCensus return_census;
    bluewake_return_census_init(&return_census);
    return_census.edges_enabled =
        getenv("BLUEWAKE_RETURN_CENSUS_EDGES") != NULL;
    const bool force_fp = getenv("BLUEWAKE_FORCE_FP") != NULL;
    bool fp_resume_reported = false;
    bool scheduler_wait_reported = false;
    bool interrupt_state_reported = false;
    bool vi_handler_reported = false;
    bool video_callback_reported = false;
    bool video_callback_state_reported = false;
    bool profile_prolog_called = false;
    bool rel_prolog_sda_pending = false;
    u32 rel_prolog_saved_r13 = 0u;
    bool message_send_reported = false;
    bool audio_message_send_reported = false;
    bool wake_reported = false;
    bool message_send_return_reported = false;
    bool audio_message_return_reported = false;
    bool reschedule_boundary_reported = false;
    unsigned os_load_reports = 0;
    unsigned os_rfi_reports = 0;
    unsigned audio_os_load_reports = 0;
    unsigned audio_os_rfi_reports = 0;
    unsigned audio_scheduler_reports = 0;
    unsigned audio_msr_dispatch_reports = 0;
    u32 last_dvd_wait_queue = 0;
    bool dvd_wait_queue_reported = false;
    unsigned receive_message_reports = 0;
    unsigned worker_queue_trace_reports = 0;
    unsigned resumed_path_reports = 0;
    unsigned vi_callback_reports = 0;
    unsigned message_send_reports = 0;
    unsigned retrace_wait_reports = 0;
    unsigned vi_wait_reports = 0;
    unsigned vi_wait_return_reports = 0;
    unsigned retrace_wakeup_reports = 0;
    unsigned retrace_wakeup_return_reports = 0;
    unsigned main_wakeup_trace_reports = 0;
    bool retrace_wakeup_pending = false;
    unsigned video_wait_reports = 0;
    unsigned gx_finish_reports = 0;
    unsigned gx_finish_interrupt_reports = 0;
    unsigned gx_finish_return_reports = 0;
    unsigned finish_queue_wakeup_return_reports = 0;
    unsigned finish_queue_wakeup_body_reports = 0;
    unsigned finish_queue_wakeup_restore_reports = 0;
    unsigned gx_path_reports = 0;
    unsigned display_continuation_reports = 0;
    unsigned game_loop_reports = 0;
    unsigned game_boundary_reports = 0;
    unsigned render_begin_reports = 0;
    unsigned scene_manager_reports = 0;
    unsigned scene_request_reports = 0;
    unsigned scene_phase_reports = 0;
    unsigned scene_overlap_trace_reports = 0;
    unsigned scene_overlap_child_trace_reports = 0;
    unsigned create_iter_trace_reports = 0;
    unsigned room_create_phase_reports = 0;
    unsigned room_scene_phase_reports = 0;
    unsigned room0_actor_reports = 0;
    unsigned room0_player_draw_reports = 0;
    unsigned room0_draw_queue_reports = 0;
    unsigned room0_draw_tree_reports = 0;
    unsigned room_child_created_reports = 0;
    unsigned room_pending_phase_reports = 0;
    unsigned room_final_pending_reports = 0;
    unsigned bg_create_count_reports = 0;
    unsigned bg_phase_trace_reports = 0;
    unsigned bg_postmethod_trace_reports = 0;
    unsigned logo_state_reports = 0;
    unsigned logo_callback_reports = 0;
    unsigned draw_dispatch_reports = 0;
    unsigned gx_entry_trace_reports = 0;
        unsigned pad_retail_read_reports = 0;
        unsigned pad_conversion_reports = 0;
    unsigned room0_scene_draw_reports = 0;
    unsigned room0_fpc_draw_reports = 0;
    unsigned room0_player_execute_reports = 0;
    bool player_trace_state_seen = false;
    u8 player_trace_event_mode = 0u;
    u16 player_trace_demo_type = 0u;
    u32 player_trace_demo_mode = 0u;
    s32 player_trace_staff_idx = -1;
    s32 player_trace_staff_cut = -1;
    u32 player_trace_staff_action = UINT32_MAX;
    u32 player_trace_pos_x = 0u;
    u32 player_trace_pos_y = 0u;
    u32 player_trace_pos_z = 0u;
    u64 player_trace_last_retrace = 0u;
    bool player_stick_start_captured = false;
    u64 player_stick_start_retrace = 0u;
    u32 player_stick_start_pos_x = 0u;
    u32 player_stick_start_pos_y = 0u;
    u32 player_stick_start_pos_z = 0u;
    u32 player_stick_camera = 0u;
    u32 player_stick_start_eye[3] = {0u, 0u, 0u};
    u32 player_stick_start_center[3] = {0u, 0u, 0u};
    bool lava_ground_provenance_seen = false;
    u16 lava_ground_last_bg_index = UINT16_MAX;
    u16 lava_ground_last_poly_index = UINT16_MAX;
    unsigned lava_ground_provenance_reports = 0u;
    u64 lava_constant_checks = 0u;
    u64 lava_constant_deferred_checks = 0u;
    u64 event_scene_execute_entries = 0u;
    u64 event_execute_wrapper_entries = 0u;
    u64 event_run_proc_entries = 0u;
    u64 aj_event_order_entries = 0u;
    u64 event_control_order_entries = 0u;
    u64 event_control_check_start_entries = 0u;
    u64 event_control_demo_check_entries = 0u;
    u64 event_manager_order_entries = 0u;
    u64 play_scene_delete_entries = 0u;
    u64 event_control_remove_entries = 0u;
    u64 event_control_reset_entries = 0u;
    u64 event_control_end_proc_entries = 0u;
    u64 event_control_demo_end_entries = 0u;
    bool event_control_state_seen = false;
    u8 event_control_last_mode = 0u;
    s16 event_control_last_event = -1;
    unsigned event_control_state_reports = 0u;
    unsigned draw_tag_reports = 0;
    unsigned message_pane_reports = 0;
    unsigned late_message_pane_reports = 0;
    unsigned pane_data_reports = 0;
    unsigned cursor_pane_data_reports = 0;
    unsigned idle_selector_reports = 0;
    unsigned main_sleep_reports = 0;
    unsigned main_context_reports = 0;
    unsigned main_sleep_store_reports = 0;
    unsigned main_dispatch_return_reports = 0;
    unsigned draw_return_reports = 0;
    unsigned management_reports = 0;
    unsigned draw_iter_reports = 0;
    bool post_gx_thread_snapshot_reported = false;
    u32 last_video_used = 0;
    u32 last_run_bits = 0;
    unsigned scheduler_state_change_reports = 0;
    bool audio_wait_reported = false;
    bool audio_tick_wait_reported = false;
    bool ppc_halt_reported = false;
    bool os_panic_reported = false;
    bool os_panic_callsite_reported = false;
    unsigned os_panic_vcall_reports = 0u;
    unsigned os_panic_vcall_after_reports = 0u;
    bool os_panic_bad_prolog_reported = false;
    bool os_panic_bad_header_reported = false;
    bool dvd_thread_snapshot_reported = false;
    unsigned dvd_thread_trace_reports = 0;
    unsigned message_trace_reports = 0;
    unsigned jas_dvd_trace_reports = 0;
    unsigned resource_set_reports = 0;
    unsigned dvd_path_reports = 0;
    unsigned vi_pending_while_disabled_reports = 0;
    unsigned dispatch_return_trace_reports = 0;
    unsigned terminal_dispatch_trace_reports = 0;
    unsigned dispatch_transition_reports = 0;
    unsigned worker_dispatch_trace_reports = 0;
    unsigned worker_selection_trace_reports = 0;
    unsigned worker_receive_trace_reports = 0;
    unsigned worker_wakeup_trace_reports = 0;
    unsigned aram_worker_sleep_trace_reports = 0;
    unsigned aram_worker_receive_resume_reports = 0;
    unsigned aram_worker_receive_dispatch_reports = 0;
    unsigned aram_receive_interrupt_boundary_reports = 0;
    unsigned aram_receive_continuation_reports = 0;
    unsigned aram_receive_command_boundary_reports = 0;
    unsigned aram_late_receive_reports = 0;
    unsigned aram_late_sleep_reports = 0;
    unsigned aram_late_selector_reports = 0;
    unsigned aram_late_context_reports = 0;
    unsigned aram_late_selectthread_reports = 0;
    unsigned aram_sleep_continuation_reports = 0;
    unsigned aram_reschedule_boundary_reports = 0;
    unsigned selector_handoff_reports = 0;
    unsigned selector_load_handoff_reports = 0;
    unsigned selector_post_store_reports = 0;
    unsigned selector_restore_branch_reports = 0;
    unsigned finish_wake_thread_switch_reports = 0;
    unsigned selector_priority8_resume_reports = 0;
    unsigned selector_mask_trace_reports = 0;
    unsigned owner_send_return_reports = 0;
    unsigned aram_wakeup_callsite_reports = 0;
    unsigned aram_real_wakeup_reports = 0;
    u32 selector_selected_context = 0;
    unsigned aram_worker_wake_boundary_reports = 0;
    unsigned aram_worker_wake_resume_reports = 0;
    unsigned aram_manager_send_return_reports = 0;
    unsigned aram_manager_restore_reports = 0;
    unsigned aram_worker_loop_trace_reports = 0;
    unsigned aram_callback_return_trace_reports = 0;
    unsigned aram_message_dispatch_trace_reports = 0;
    unsigned aram_message_internal_trace_reports = 0;
    unsigned aram_context_transition_trace_reports = 0;
    unsigned aram_clear_store_trace_reports = 0;
    unsigned aram_isr_segment_trace_reports = 0;
    unsigned aram_stream_command_trace_reports = 0;
    unsigned aram_stream_constructor_trace_reports = 0;
    unsigned aram_piece_command_trace_reports = 0;
    unsigned aram_piece_message_entry_trace_reports = 0;
    bool heap_tail_watch_initialized = false;
    u32 heap_tail_watch[6] = {0};
    unsigned heap_tail_watch_reports = 0;
    unsigned heap_tail_pre_reports = 0;
    unsigned heap_allocator_tuple_reports = 0;
    bool heap_nominal_state_initialized = false;
    u32 heap_nominal_state[4] = {0};
    unsigned heap_nominal_state_reports = 0;
    unsigned heap_nominal_pre_reports = 0;
    unsigned aram_stream_writer_trace_reports = 0;
    unsigned aram_stream_queue_trace_reports = 0;
    unsigned aram_stream_sync_trace_reports = 0;
    unsigned aram_ripper_sync_trace_reports = 0;
    unsigned aram_cleanup_call_trace_reports = 0;
    unsigned aram_cleanup_unlock_return_reports = 0;
    unsigned aram_ripper_caller_trace_reports = 0;
    unsigned aram_late_command_trace_reports = 0;
    unsigned video_tick_queue_trace_reports = 0;
    unsigned display_tick_wait_trace_reports = 0;
    unsigned display_alarm_trace_reports = 0;
    u32 aram_ripper_command_mutex_a = 0;
    u32 aram_ripper_dvd_mutex_a = 0;
    u32 aram_ripper_command_mutex_b = 0;
    u32 aram_ripper_dvd_mutex_b = 0;
    unsigned aram_pcs_trace_reports = 0;
    unsigned current_context_trace_reports = 0;
    unsigned scheduler_transition_trace_reports = 0;
    unsigned scheduler_return_trace_reports = 0;
    unsigned late_scheduler_return_trace_reports = 0;
    unsigned late_continuation_trace_reports = 0;
    unsigned late_scheduler_budget_trace_reports = 0;
    unsigned late_selector_contract_trace_reports = 0;
    unsigned late_selector_post_dispatch_reports = 0;
    unsigned late_worker_message_trace_reports = 0;
    unsigned late_worker_handler_trace_reports = 0;
    unsigned late_worker_callback_trace_reports = 0;
    unsigned late_worker_command_return_trace_reports = 0;
    unsigned late_scheduler_post_command_trace_reports = 0;
    unsigned late_scheduler_callback_trace_reports = 0;
    unsigned late_selector_callback_return_trace_reports = 0;
    unsigned selector_progress_trace_reports = 0;
    bool alarm_state_snapshot_reported = false;
    bool logo_resource_sync_snapshot_armed = false;
    bool logo_resource_sync_snapshot_reported = false;
    u64 dvd_jas_forwards = 0u;
    u64 dvd_jas_forward_failures = 0u;
    u64 dvd_callback_entries = 0u;
    u64 dvd_callback_returns = 0u;
    u64 dvd_mount_archive_entries = 0u;
    u64 dvd_mount_x_entries = 0u;
    u64 dvd_main_ram_entries = 0u;
    u32 dvd_jas_active_command = 0u;
    u64 jas_state_last_retrace = UINT64_MAX;
    unsigned jas_state_reports = 0u;
    u64 scene_milestone_last_retrace = UINT64_MAX;
    u32 opening_scene_object = 0u;
    u32 opening_scene_last_state = UINT32_MAX;
    u64 scene_draw_before_play[4] = {0u, 0u, 0u, 0u};
    u64 scene_draw_after_play[4] = {0u, 0u, 0u, 0u};
    u64 scene_draw_first_after_retrace[4] = {
        UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX,
    };
    // Save states (see host_state_save): the requests, and a load before the
    // first guest instruction.
    const HostStateLoop state_loop = {&profile_prolog_called, &rel_prolog_sda_pending,
                                      &rel_prolog_saved_r13};
    host_state_parse_requests();
    {
        const char* load_state = getenv("BLUEWAKE_LOAD_STATE");
        if (load_state != NULL && load_state[0] != '\0') {
            if (!host_state_load(load_state, &cpu, mod, &state_loop)) {
                fprintf(stderr, "[state] BLUEWAKE_LOAD_STATE=%s failed\n", load_state);
                if (aurora_enabled)
                    dol_aurora_shutdown();
                return 1;
            }
        }
    }
    while ((max_blocks == 0ull || blocks < max_blocks) && !stop_reason &&
           (max_retraces == 0ull ||
            g_host_retrace_count < max_retraces)) {
        host_state_turn(&cpu, mod, &state_loop);
        g_current_host_block = blocks;
        bluewake_cycle_domain_begin_turn(&g_cycle_domain, &cpu);
        if (dol_platform_should_quit()) { stop_reason = "quit"; break; }
        bluewake_card_runtime_service_callback(&cpu);
        bluewake_card_runtime_dispatch(&cpu);
        bluewake_forest_water_dispatch(&cpu, cpu.pc);
        if (g_host_retrace_count != scene_milestone_last_retrace) {
            scene_milestone_last_retrace = g_host_retrace_count;
            const u32 open_scene = host_find_scene_by_proc_name(&cpu, 0x000Eu);
            if (open_scene != 0u) {
                const u32 proc = mem_read32(&cpu, open_scene + 0x1D0u);
                const u32 state = proc >= 0x80000000u
                                      ? mem_read32(&cpu, proc + 0x2B0u)
                                      : UINT32_MAX;
                if (open_scene != opening_scene_object ||
                    state != opening_scene_last_state) {
                    fprintf(stderr,
                            "[opening-milestone] scene=0x%08X proc=0x%08X "
                            "state=%u timer=%u retrace=%llu blocks=%llu\n",
                            open_scene, proc, state,
                            proc >= 0x80000000u
                                ? mem_read32(&cpu, proc + 0x2ACu)
                                : UINT32_MAX,
                            (unsigned long long)g_host_retrace_count,
                            (unsigned long long)blocks);
                    opening_scene_object = open_scene;
                    opening_scene_last_state = state;
                }
                if (!g_opening_complete_reported && state == 44u) {
                    g_opening_complete_reported = true;
                    g_opening_complete_retrace = g_host_retrace_count;
                    fprintf(stderr,
                            "[boot-milestone] opening-complete retrace=%llu "
                            "blocks=%llu\n",
                            (unsigned long long)g_opening_complete_retrace,
                            (unsigned long long)blocks);
                }
            }
            if (!g_play_scene_reported &&
                host_find_scene_by_proc_name(&cpu, 0x0007u) != 0u) {
                g_play_scene_reported = true;
                g_play_scene_retrace = g_host_retrace_count;
                fprintf(stderr,
                        "[boot-milestone] play-scene retrace=%llu "
                        "blocks=%llu\n",
                        (unsigned long long)g_play_scene_retrace,
                        (unsigned long long)blocks);
            }
            // The guest's own player record, read through the pointer the
            // runtime's player routines already use (0x803CA74C) so it needs no
            // CPU hook. Prints on change, and prints a heartbeat every 120
            // retraces once the play scene is up, so an idle player reads as
            // still and a moved one carries a stamped window. The values are
            // the ones that decide whether a human has control:
            // event_mode with demo_mode is the pair the player-ready capture
            // waits for (event_mode 0 and demo playback off is control;
            // demo_mode 4 with demo_type 1 and event_mode 2 is the authored
            // awake cutscene still running and still eating keys), the pad hold
            // word is the same guest record the key presses land in, and the
            // three stick words are the decoded left stick the guest's player
            // code actually steers with. This is a product instrument, not a
            // route one: nothing here is armed by a pulse.
            save_route_step(&cpu);
            if (player_probe ||
                (capture_player_ready && !player_ready_capture_scheduled)) {
                static bool player_probe_seen;
                static u32 player_probe_last[15];
                static u64 player_probe_last_retrace;
                const u32 probe_player = mem_read32(&cpu, 0x803CA74Cu);
                const bool player_valid = probe_player >= 0x80000000u;
                const u16 probe_demo_type =
                    player_valid ? mem_read16(&cpu, probe_player + 0x304u) : 0u;
                const u32 probe_demo_mode =
                    player_valid ? mem_read32(&cpu, probe_player + 0x314u) : 0u;
                const u32 probe_proc =
                    player_valid ? mem_read32(&cpu, probe_player + 0x31D8u) : 0u;
                const u32 probe_pos_x = mem_read32(&cpu, probe_player + 0x1F8u);
                const u32 probe_pos_y = mem_read32(&cpu, probe_player + 0x1FCu);
                const u32 probe_pos_z = mem_read32(&cpu, probe_player + 0x200u);
                const u8 probe_event_mode = mem_read8(&cpu, 0x803C9EA2u);
                const s16 probe_event = (s16)mem_read16(&cpu, 0x803C9EB8u);
                const u8 probe_msg = mem_read8(&cpu, 0x803CA7D2u);
                const u32 probe_pad = mem_read32(&cpu, 0x803A4DE0u);
                const u32 probe_hold = probe_pad >= 0x80000000u
                                           ? mem_read32(&cpu, probe_pad + 0x18u)
                                           : 0u;
                // The game's own decoded left stick (0x803A4DF0, +4, +8), the
                // value its player code actually steers with. The pad word
                // above is a button word and a stick axis never appears in it,
                // so reading only that word made "the key never reached the
                // guest" and "the key arrived and the scene was not accepting
                // input" look identical. The route's own player trace reads
                // these same three words.
                const u32 probe_stick_x = mem_read32(&cpu, 0x803A4DF0u);
                const u32 probe_stick_y = mem_read32(&cpu, 0x803A4DF4u);
                const u32 probe_stick_mag = mem_read32(&cpu, 0x803A4DF8u);
                // The scene-transition overlap object (0x803F6160) walks its own
                // phase 0..6 while a scene handover runs and rests at phase 6
                // once the handover is complete. It is the very object the
                // route's own control gate reads through g_overlap_terminal_phase,
                // and it is what separates "the play scene has loaded" from "the
                // player can be steered". Without it this gate is wrong in a way
                // that has already cost a run: at the play scene the transition
                // sits at phase 3-4 for roughly twenty retraces BEFORE the awake
                // cutscene starts, event_mode is still 0 across that window, and
                // a gate that tested only the event mode admitted control at
                // retrace 13,985 and then measured a 6 s W hold against the
                // cutscene's first page (demo_type=1 demo_mode=512 event_mode=2).
                // The route's gate excludes that window because phase 6 was not
                // reached yet.
                //
                // Read the phase live for the record, and gate on the latched
                // phase the ROUTE's own control gate reads. They are not the
                // same reading and the difference cost a run: this probe's
                // read lands late in the retrace, after the handover has been
                // torn down, so it logged phase 5 607 times and phase 6 never,
                // while the milestone instrument -- the same guest word, read
                // inside the overlap handler -- logged phase 6 at retrace 14033
                // and held it. A gate that required this probe's latch to equal
                // 6 was therefore unsatisfiable: control-admitted could never
                // be emitted, whatever the guest did. g_overlap_terminal_phase
                // is set from the same object when the handler reports a phase
                // change, is what player_state_ready reads further down this
                // file, and makes this gate and the route's gate agree by
                // construction. The live read stays in the line below so the
                // two values can never again be mistaken for one another.
                const u32 probe_overlap_request = mem_read32(&cpu, 0x803F6160u);
                const bool probe_overlap_live =
                    probe_overlap_request >= 0x80000000u &&
                    mem_read16(&cpu, probe_overlap_request + 0x04u) == 1u;
                const u32 probe_overlap_phase_live =
                    probe_overlap_live
                        ? mem_read32(&cpu, probe_overlap_request + 0x1Cu)
                        : 0u;
                const u32 player_overlap_phase_latch = g_overlap_terminal_phase;
                // Schedule the player-ready frame from the guest's own control
                // state, not from the CPU hook at 0x80122D30. That hook has
                // never fired in any run (this project's own route included),
                // so the capture it armed was always empty; this probe reads
                // the state without a PC, so it cannot be skipped. Nothing
                // here is armed by a route pulse.
                //
                // Control is the guest's own event mode falling to zero with
                // demo playback off. That is the tuple the route's own control
                // gate reads (player_state_ready further down this file tests
                // 0x803C9EA2 == 0), and it is the state its player trace shows
                // a stick actually moving Link in.
                //
                // demo_mode falling from the cutscene 512 to 4 is NOT that
                // state. The route's own player trace at control reads
                // demo_type=0 demo_mode=0 event_mode=0 with the decoded stick
                // at y=1.0 and the position advancing, while demo_mode 4 comes
                // with demo_type=1 event_mode=2 and the authored Outset awake
                // cutscene still on screen. Scheduling the player-ready frame
                // from demo_mode 4 captured a cutscene with a still-running
                // subtitle actor, and a key held there moved nothing because
                // the cutscene was still consuming it. Gating on the event
                // mode is the fix, and it is a product gate: it asks the guest
                // whether the player can be steered, not whether a cutscene
                // stepped through one of its states.
                //
                // It has to hold for a few retraces, so a one-retrace flicker
                // during a scene handover cannot be mistaken for control, and it
                // has to come after the scene handover has finished, which is
                // what the latched overlap phase says. This is the route's own
                // gate -- scene transition complete, player actor valid, event
                // mode clear -- read from the same guest object, not a new
                // criterion invented here.
                const bool probe_control_state =
                    player_valid && g_play_scene_reported &&
                    player_overlap_phase_latch == 6u &&
                    probe_event_mode == 0u && probe_demo_mode == 0u;
                static u32 player_control_hold_retraces;
                if (probe_control_state) {
                    ++player_control_hold_retraces;
                } else {
                    player_control_hold_retraces = 0u;
                }
                static bool player_control_admitted;
                if (!player_control_admitted &&
                    player_control_hold_retraces >= 8u) {
                    player_control_admitted = true;
                    fprintf(stderr,
                            "[player-milestone] control-admitted retrace=%llu "
                            "event_mode=%u demo_type=%u demo_mode=%u ovl=%u "
                            "pos=%08X,%08X,%08X\n",
                            (unsigned long long)g_host_retrace_count,
                            (unsigned)probe_event_mode,
                            (unsigned)probe_demo_type, probe_demo_mode,
                            player_overlap_phase_latch,
                            probe_pos_x, probe_pos_y, probe_pos_z);
                }
                if (capture_player_ready && !player_ready_capture_scheduled &&
                    player_valid && player_control_admitted) {
                    diagnostic_capture_retrace = g_host_retrace_count + 2u;
                    player_ready_capture_scheduled = true;
                    player_ready_capture_followups = 1u;
                    fprintf(stderr,
                            "[frame-capture] awake-action scheduled "
                            "retrace=%llu capture_retrace=%llu path=%s "
                            "source=player-scene-state\n",
                            (unsigned long long)g_host_retrace_count,
                            (unsigned long long)diagnostic_capture_retrace,
                            opening_capture_path != NULL ? opening_capture_path
                                                         : "(none)");
                }
                const u32 probe_now[15] = {
                    probe_player,
                    (u32)probe_demo_type,
                    probe_demo_mode,
                    probe_proc,
                    (u32)probe_event_mode,
                    (u32)(s32)probe_event,
                    (u32)probe_msg,
                    probe_hold,
                    // Position belongs in the change test, not only in the
                    // print. Walking with a real stick moves the player record
                    // and nothing else, so a test that compared the last line
                    // before a held key with the last line after it was
                    // comparing one line with itself and calling a real walk
                    // unmoved. This is that false failure's root cause.
                    probe_pos_x,
                    probe_pos_y,
                    probe_pos_z,
                    probe_stick_x,
                    probe_stick_y,
                    probe_stick_mag,
                    player_overlap_phase_latch,
                };
                // A heartbeat, not a delta: while the play scene runs the probe
                // also prints every 120 retraces, so a stationary player is
                // visibly stationary instead of looking like a dead probe, and
                // a held key has a window of stamped positions to move across.
                const bool probe_heartbeat =
                    player_probe && g_play_scene_reported &&
                    g_host_retrace_count >= player_probe_last_retrace + 120u;
                bool changed = !player_probe_seen;
                for (u32 slot = 0u; slot < 15u; ++slot) {
                    if (player_probe_last[slot] != probe_now[slot]) changed = true;
                }
                if ((changed || probe_heartbeat) && player_probe) {
                    player_probe_seen = true;
                    player_probe_last_retrace = g_host_retrace_count;
                    for (u32 slot = 0u; slot < 15u; ++slot)
                        player_probe_last[slot] = probe_now[slot];
                    fprintf(stderr,
                            "[player-scene-state] retrace=%llu player=0x%08X "
                            "demo_type=%u demo_mode=%u proc=%u event_mode=%u "
                            "event=%d msg=%u pad_hold=0x%08X "
                            "pos=%08X,%08X,%08X "
                            "stick=%08X,%08X,%08X ovl=%u ovl_live=%u\n",
                            (unsigned long long)g_host_retrace_count,
                            probe_player, (unsigned)probe_demo_type,
                            probe_demo_mode, probe_proc,
                            (unsigned)probe_event_mode, (int)probe_event,
                            (unsigned)probe_msg, probe_hold, probe_pos_x,
                            probe_pos_y, probe_pos_z, probe_stick_x,
                            probe_stick_y, probe_stick_mag,
                            player_overlap_phase_latch,
                            probe_overlap_phase_live);
                }
            }
        }
        if (!g_dsp_task_boot_started && cpu.pc == 0x803193ACu) {
            // __DSP_boot_task performs a second DSP-side boot handshake for
            // the JAudio task after OS audio initialization has completed.
            g_dsp_boot_mail_armed = true;
            g_dsp_boot_mail_clear_seen = false;
            g_dsp_boot_handshake_sent = false;
            g_dsp_task_boot_started = true;
        }
        if (!logo_resource_sync_snapshot_reported &&
            g_host_retrace_count >= 475u && cpu.pc == 0x8022CF44u) {
            logo_resource_sync_snapshot_armed = true;
        }
        if (logo_resource_sync_snapshot_armed && cpu.pc == 0x8022CF70u) {
            report_logo_resource_sync(&cpu, cpu.gpr[3]);
            logo_resource_sync_snapshot_armed = false;
            logo_resource_sync_snapshot_reported = true;
        }
        if (cpu.pc == 0x8001826Cu) {
            dvd_jas_forwards++;
            if (cpu.gpr[3] == 0u)
                dvd_jas_forward_failures++;
        } else if (cpu.pc == 0x800181CCu && cpu.lr == 0x8027B6C4u) {
            if (dvd_jas_active_command == 0u) {
                dvd_callback_entries++;
                dvd_jas_active_command = mem_read32(&cpu, cpu.gpr[3]);
            }
        } else if (cpu.pc == 0x800181ECu && dvd_jas_active_command != 0u) {
            dvd_callback_returns++;
            dvd_jas_active_command = 0u;
        } else if (cpu.pc == 0x8001861Cu) {
            dvd_mount_archive_entries++;
        } else if (cpu.pc == 0x8001890Cu) {
            dvd_mount_x_entries++;
        } else if (cpu.pc == 0x80018B08u) {
            dvd_main_ram_entries++;
        }
        if (g_host_retrace_count != jas_state_last_retrace &&
            jas_state_reports < 64u) {
            const u32 jas_thread = 0x806AD7E0u;
            const u32 queue_used = mem_read32(&cpu, 0x806AC790u + 0x1Cu);
            const u32 thread_state = mem_read16(&cpu, jas_thread + 0x2C8u);
            const u32 saved_pc = mem_read32(&cpu, jas_thread + 0x198u);
            const u32 wait_queue = mem_read32(&cpu, jas_thread + 0x2DCu);
            const u32 current_thread = mem_read32(&cpu, 0x800000E4u);
            const u32 current_context = mem_read32(&cpu, 0x800000D4u);
            const u32 run_bits = mem_read32(&cpu, 0x803F7A30u);
            const u32 lod_done = mem_read8(&cpu, 0x80ADEFDCu + 0x0Cu);
            bool dsp_pending = false;
#ifdef BLUEWAKE_HAS_DSP_ADAPTER
            dsp_pending = g_dsp_adapter_interrupt_pending;
#endif
            jas_state_last_retrace = g_host_retrace_count;
            const bool report_retrace =
                g_host_retrace_count != 0u &&
                (g_host_retrace_count <= 8u ||
                 ((g_host_retrace_count & (g_host_retrace_count - 1u)) == 0u) ||
                 g_host_retrace_count == 100u ||
                 g_host_retrace_count == 200u ||
                 g_host_retrace_count == 330u ||
                 g_host_retrace_count == 475u);
            if (report_retrace) {
                fprintf(stderr,
                        "[jas-state] retrace=%llu queue_used=%u state=%u "
                        "saved_pc=0x%08X wait=0x%08X current=0x%08X "
                        "context=0x%08X run_bits=0x%08X lod_done=%u "
                        "dsp_pending=%u\n",
                        (unsigned long long)g_host_retrace_count, queue_used,
                        thread_state, saved_pc, wait_queue, current_thread,
                        current_context, run_bits, lod_done,
                        dsp_pending ? 1u : 0u);
                jas_state_reports++;
            }
        }
        if (g_audio_object_watch && audio_message_send_reported &&
            !audio_message_return_reported &&
            (cpu.pc == 0x803059A4u || cpu.pc == 0x803059ACu ||
             cpu.pc == 0x803059B0u)) {
            const u32 thread = 0x803E9260u;
            const s32 effective = (s32)mem_read32(&cpu, thread + 0x2D0u);
            const u32 expected_run_bit = effective >= 0 && effective <= 31
                                              ? 1u << (31u - (u32)effective)
                                              : 0u;
            fprintf(stderr,
                    "[audio-msg-post] pc=0x%08X thread=0x%08X state=%u "
                    "suspend=%d effective=%d base=%d queue=0x%08X "
                    "recv_head=0x%08X recv_tail=0x%08X run_bits=0x%08X "
                    "expected_run_bit=0x%08X current_thread=0x%08X "
                    "current_context=0x%08X\n",
                    cpu.pc, thread, mem_read16(&cpu, thread + 0x2C8u),
                    (s32)mem_read32(&cpu, thread + 0x2CCu), effective,
                    (s32)mem_read32(&cpu, thread + 0x2D4u),
                    mem_read32(&cpu, thread + 0x2DCu),
                    mem_read32(&cpu, 0x803EA588u),
                    mem_read32(&cpu, 0x803EA58Cu),
                    mem_read32(&cpu, 0x803F7A30u), expected_run_bit,
                    mem_read32(&cpu, 0x800000E4u), mem_read32(&cpu, 0x800000D4u));
            audio_message_return_reported = true;
        }
        // These are the retail GZLE01 sound-start boundaries. Observe the
        // guest ID at the first authentic boundary reached and route it
        // through the promoted event adapter without altering guest state.
        // JAIBasic::startSoundActor carries its sound ID in r4 under PPC ABI.
        if ((cpu.pc == 0x800F0D10u || cpu.pc == 0x802A6720u ||
             cpu.pc == 0x802904ECu) &&
            (g_audio_event_reports + g_audio_sequence_reports) < 64u) {
            const u32 sound_id = (cpu.pc == 0x802A6720u ||
                                  cpu.pc == 0x802904ECu)
                                     ? cpu.gpr[4]
                                     : cpu.gpr[3];
            if ((sound_id & 0xC0000000u) == 0x80000000u) {
                fprintf(stderr,
                        "[audio-sequence] authentic sound_id=0x%08X "
                        "pc=0x%08X blocks=%llu\n",
                        sound_id, cpu.pc, (unsigned long long)blocks);
                g_audio_sequence_reports++;
            } else {
                const s32 voice = dol_audio_event_start(&g_audio_events,
                                                        sound_id, 32767u, 0);
                fprintf(stderr,
                        "[audio-event] authentic sound_id=0x%08X voice=%d "
                        "pc=0x%08X blocks=%llu\n",
                        sound_id, voice, cpu.pc, (unsigned long long)blocks);
                g_audio_event_reports++;
            }
        }
        if ((cpu.pc == 0x8028892Cu || cpu.pc == 0x8028DACC) &&
            g_audio_note_reports < 64u) {
            fprintf(stderr,
                    "[audio-note] authentic pc=0x%08X bank=%d program=%d "
                    "key=%u velocity=%u param=0x%08X blocks=%llu\n",
                    cpu.pc, (s32)cpu.gpr[5], (s32)cpu.gpr[6],
                    cpu.gpr[7] & 0xFFu, cpu.gpr[8] & 0xFFu, cpu.gpr[9],
                    (unsigned long long)blocks);
            g_audio_note_reports++;
        }
        if (g_audio_object_watch &&
            (cpu.pc == 0x80290090u || cpu.pc == 0x8028920Cu) &&
            g_audio_start_reports < 16u) {
            fprintf(stderr,
                    "[audio-start] pc=0x%08X this=0x%08X heap=0x%08X "
                    "aram_size=0x%08X flag=0x%08X blocks=%llu\n",
                    cpu.pc, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.gpr[6],
                    (unsigned long long)blocks);
            g_audio_start_reports++;
        }
        const u32 audio_current_context = mem_read32(&cpu, 0x800000D4u);
        if (g_audio_object_watch && g_audio_thread_active &&
            audio_current_context == 0x803E9260u &&
            cpu.msr != g_audio_last_msr &&
            g_audio_msr_transition_reports < 24u) {
            fprintf(stderr,
                    "[audio-msr-transition] pc=0x%08X old=0x%08X "
                    "new=0x%08X prev_pc=0x%08X prev_context=0x%08X "
                    "current=0x%08X context=0x%08X exception=0x%08X "
                    "blocks=%llu\n",
                    cpu.pc, g_audio_last_msr, cpu.msr, g_audio_last_msr_pc,
                    g_audio_last_msr_context, mem_read32(&cpu, 0x800000E4u),
                    audio_current_context, cpu.exception,
                    (unsigned long long)blocks);
            g_audio_msr_transition_reports++;
        }
        if (g_audio_thread_active && audio_current_context == 0x803E9260u) {
            g_audio_last_msr = cpu.msr;
            g_audio_last_msr_pc = cpu.pc;
            g_audio_last_msr_context = audio_current_context;
        }
        if (g_audio_object_watch &&
            (cpu.pc == 0x8028E780u || cpu.pc == 0x8028EFC0u ||
             cpu.pc == 0x802893E4u || cpu.pc == 0x80289568u ||
             cpu.pc == 0x80288F08u) &&
            g_audio_frame_boundary_reports < 32u) {
            fprintf(stderr,
                    "[audio-frame] pc=0x%08X r3=0x%08X r4=0x%08X "
                    "r5=0x%08X r6=0x%08X blocks=%llu\n",
                    cpu.pc, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.gpr[6],
                    (unsigned long long)blocks);
            g_audio_frame_boundary_reports++;
        }
        if (g_audio_object_watch && g_audio_thread_active &&
            (cpu.pc == 0x80304608u || cpu.pc == 0x80304610u ||
             cpu.pc == 0x8030461Cu || cpu.pc == 0x80304624u ||
             cpu.pc == 0x80304630u || cpu.pc == 0x80304648u) &&
            g_audio_interrupt_state_reports < 32u) {
            fprintf(stderr,
                    "[audio-interrupt-state] pc=0x%08X r3=0x%08X "
                    "r4=0x%08X r5=0x%08X r31=0x%08X msr=0x%08X "
                    "lr=0x%08X blocks=%llu\n",
                    cpu.pc, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.gpr[31],
                    cpu.msr, cpu.lr, (unsigned long long)blocks);
            g_audio_interrupt_state_reports++;
        }
        if (g_audio_object_watch &&
            (cpu.pc == 0x8027BD14u || cpu.pc == 0x8027BDACu ||
             cpu.pc == 0x80289414u) &&
            g_audio_callback_reports < 32u) {
            const u32 call_list = mem_read32(&cpu, 0x803F7430u);
            fprintf(stderr,
                    "[audio-callback] pc=0x%08X call_list=0x%08X init=%u "
                    "slot0_fn=0x%08X slot0_user=0x%08X slot0_kind=%u "
                    "slot1_fn=0x%08X slot1_user=0x%08X slot1_kind=%u "
                    "subframes=0x%08X frame_samples=0x%08X blocks=%llu\n",
                    cpu.pc, call_list, mem_read8(&cpu, 0x803F7434u),
                    call_list != 0u ? mem_read32(&cpu, call_list) : 0u,
                    call_list != 0u ? mem_read32(&cpu, call_list + 4u) : 0u,
                    call_list != 0u ? mem_read32(&cpu, call_list + 8u) : 0u,
                    call_list != 0u ? mem_read32(&cpu, call_list + 12u) : 0u,
                    call_list != 0u ? mem_read32(&cpu, call_list + 16u) : 0u,
                    call_list != 0u ? mem_read32(&cpu, call_list + 20u) : 0u,
                    mem_read32(&cpu, 0x803F63ECu),
                    mem_read32(&cpu, 0x803F63F0u),
                    (unsigned long long)blocks);
            g_audio_callback_reports++;
        }
        if (g_audio_object_watch &&
            ((cpu.pc >= 0x80288F88u && cpu.pc < 0x80289130u) ||
             cpu.pc == 0x80288F08u ||
             cpu.pc == 0x80289130u || cpu.pc == 0x802893E4u ||
             cpu.pc == 0x80289568u || cpu.pc == 0x8028904Cu ||
             cpu.pc == 0x80289050u || cpu.pc == 0x80289058u ||
             cpu.pc == 0x8028905Cu || cpu.pc == 0x8028906Cu ||
             cpu.pc == 0x80289080u || cpu.pc == 0x802890ECu) &&
            g_audio_thread_reports < 32u) {
            if (cpu.pc == 0x80288F88u) {
                g_audio_thread_active = true;
                g_audio_interrupt_state_reports = 0;
    g_audio_msr_transition_reports = 0;
                g_audio_last_msr = cpu.msr;
                g_audio_last_msr_pc = cpu.pc;
                g_audio_last_msr_context = mem_read32(&cpu, 0x800000D4u);
            }
            fprintf(stderr,
                    "[audio-thread] pc=0x%08X r3=0x%08X r4=0x%08X "
                    "r5=0x%08X r0=0x%08X mq_init=%u sn_int=%u dsp_status=%u "
                    "dsp_prior=0x%08X dsp_curr=0x%08X dsp_req=0x%08X "
                    "msr=0x%08X srr1=0x%08X current_context_msr=0x%08X "
                    "blocks=%llu\n",
                    cpu.pc, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5],
                    cpu.gpr[0],
                    mem_read32(&cpu, 0x803F74E8u),
                    mem_read32(&cpu, 0x803F74F8u),
                    mem_read8(&cpu, 0x803F7512u),
                    mem_read32(&cpu, 0x803F7570u),
                    mem_read32(&cpu, 0x803F7C54u),
                    mem_read32(&cpu, 0x803EAF60u + 0x34u),
                    cpu.msr, cpu.srr1,
                    mem_read32(&cpu, mem_read32(&cpu, 0x800000D4u) + 0x19Cu),
                    (unsigned long long)blocks);
            if (cpu.pc == 0x80289024u) {
                const u32 command = cpu.gpr[4];
                fprintf(stderr,
                        "[audio-command] base=0x%08X words="
                        "%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X "
                        "task_words=%08X,%08X,%08X,%08X,%08X,%08X\n",
                        command, mem_read32(&cpu, command),
                        mem_read32(&cpu, command + 4u),
                        mem_read32(&cpu, command + 8u),
                        mem_read32(&cpu, command + 12u),
                        mem_read32(&cpu, command + 16u),
                        mem_read32(&cpu, command + 20u),
                        mem_read32(&cpu, command + 24u),
                        mem_read32(&cpu, command + 28u),
                        mem_read32(&cpu, 0x803EA5E0u),
                        mem_read32(&cpu, 0x803EA5E4u),
                        mem_read32(&cpu, 0x803EA5E8u),
                        mem_read32(&cpu, 0x803EA5ECu),
                        mem_read32(&cpu, 0x803EA5F0u),
                        mem_read32(&cpu, 0x803EA5F4u));
            }
            g_audio_thread_reports++;
        }
        if (g_audio_object_watch && g_audio_dsp_handler_reports < 16u &&
            cpu.pc >= 0x8028ECA0u && cpu.pc < 0x8028EFA4u) {
            const u32 current_task = mem_read32(&cpu, 0x803F7C54u);
            fprintf(stderr,
                    "[audio-dsp-handler] pc=0x%08X r3=0x%08X r4=0x%08X "
                    "current_thread=0x%08X current_context=0x%08X "
                    "task=0x%08X req_cb=0x%08X request_pending=%u "
                    "mail_pending=%u control=0x%04X\n",
                    cpu.pc, cpu.gpr[3], cpu.gpr[4],
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    current_task,
                    current_task != 0u ? mem_read32(&cpu, current_task + 0x34u)
                                       : 0u,
                    g_dsp_task_request_pending ? 1u : 0u,
                    g_dsp_mail_from_pending ? 1u : 0u, g_dsp_control);
            g_audio_dsp_handler_reports++;
        }
        if (g_audio_object_watch && g_audio_ai_handler_reports < 16u &&
            cpu.pc >= 0x80316F0Cu && cpu.pc < 0x80316FB8u) {
            fprintf(stderr,
                    "[audio-ai-handler] pc=0x%08X callback=0x%08X active=%u "
                    "status=0x%04X current=0x%08X context=0x%08X\n",
                    cpu.pc, mem_read32(&cpu, 0x803F7BBCu),
                    mem_read32(&cpu, 0x803F7BCCu),
                    (u16)host_mmio_read(&cpu, 0xCC00500Au, 2u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            g_audio_ai_handler_reports++;
        }
        if (g_audio_object_watch && g_audio_dsp_callback_reports < 16u &&
            (cpu.pc == 0x8028EEE8u || cpu.pc == 0x8028EEFCu ||
             cpu.pc == 0x8028EF00u)) {
            const u32 current_task = mem_read32(&cpu, 0x803F7C54u);
            fprintf(stderr,
                    "[audio-dsp-callback] pc=0x%08X task=0x%08X "
                    "req_cb=0x%08X r3=0x%08X r5=0x%08X r12=0x%08X "
                    "mail_pending=%u\n",
                    cpu.pc, current_task,
                    current_task != 0u ? mem_read32(&cpu, current_task + 0x34u)
                                       : 0u,
                    cpu.gpr[3], cpu.gpr[5], cpu.gpr[12],
                    g_dsp_mail_from_pending ? 1u : 0u);
            if (current_task != 0u && cpu.pc == 0x8028EF00u) {
                fprintf(stderr,
                        "[audio-dsp-task] task=0x%08X state=0x%08X "
                        "flags=0x%08X iram_mem=0x%08X iram_len=0x%08X "
                        "iram_dsp=0x%08X dram_mem=0x%08X dram_len=0x%08X "
                        "dram_dsp=0x%08X init_vec=0x%04X resume_vec=0x%04X "
                        "req=0x%08X\n",
                        current_task, mem_read32(&cpu, current_task),
                        mem_read32(&cpu, current_task + 8u),
                        mem_read32(&cpu, current_task + 0x0Cu),
                        mem_read32(&cpu, current_task + 0x10u),
                        mem_read32(&cpu, current_task + 0x14u),
                        mem_read32(&cpu, current_task + 0x18u),
                        mem_read32(&cpu, current_task + 0x1Cu),
                        mem_read32(&cpu, current_task + 0x20u),
                        mem_read16(&cpu, current_task + 0x24u),
                        mem_read16(&cpu, current_task + 0x26u),
                        mem_read32(&cpu, current_task + 0x34u));
            }
            g_audio_dsp_callback_reports++;
        }
        if (g_audio_object_watch && g_audio_dsp_handler_step_reports < 96u &&
            cpu.pc >= 0x8028ED10u && cpu.pc <= 0x8028EF80u) {
            fprintf(stderr,
                    "[audio-dsp-step] pc=0x%08X r0=0x%08X r3=0x%08X "
                    "r4=0x%08X r5=0x%08X r12=0x%08X\n",
                    cpu.pc, cpu.gpr[0], cpu.gpr[3], cpu.gpr[4], cpu.gpr[5],
                    cpu.gpr[12]);
            g_audio_dsp_handler_step_reports++;
        }
        if (g_audio_object_watch && g_audio_dsp_boot_reports < 24u &&
            (cpu.pc == 0x80288F88u || cpu.pc == 0x8028E8A0u ||
             cpu.pc == 0x8028EC20u ||
             cpu.pc == 0x8028EC9Cu || cpu.pc == 0x8028ECA0u)) {
            if (cpu.pc == 0x80288F88u || cpu.pc == 0x8028E8A0u) {
                g_audio_interrupt_state_reports = 0;
                g_audio_msr_transition_reports = 0;
                g_audio_last_msr = cpu.msr;
                g_audio_last_msr_pc = cpu.pc;
                g_audio_last_msr_context = mem_read32(&cpu, 0x800000D4u);
            }
            fprintf(stderr,
                    "[audio-dsp-boot] pc=0x%08X prior=0x%08X curr=0x%08X "
                    "task_state=0x%08X task_flags=0x%08X task_req=0x%08X "
                    "booted=0x%08X\n",
                    cpu.pc, mem_read32(&cpu, 0x803F7570u),
                    mem_read32(&cpu, 0x803F7C54u),
                    mem_read32(&cpu, 0x803EAF60u),
                    mem_read32(&cpu, 0x803EAF60u + 8u),
                    mem_read32(&cpu, 0x803EAF60u + 0x34u),
                    mem_read32(&cpu, 0x803F74FCu));
            g_audio_dsp_boot_reports++;
        }
        if ((cpu.pc == 0x80007090u || cpu.pc == 0x80007224u ||
             cpu.pc == 0x8029046Cu || cpu.pc == 0x8029057Cu ||
             cpu.pc == 0x802827F0u || cpu.pc == 0x80296828u ||
             cpu.pc == 0x80296838u || cpu.pc == 0x8029683Cu ||
             cpu.pc == 0x80296840u || cpu.pc == 0x80296844u ||
             cpu.pc == 0x80296848u || cpu.pc == 0x8029684Cu ||
             cpu.pc == 0x80296860u || cpu.pc == 0x80297FD0u ||
             cpu.pc == 0x80298208u || cpu.pc == 0x8029B0C4u ||
             cpu.pc == 0x8029B4ACu ||
             cpu.pc == 0x80296E6Cu || cpu.pc == 0x80296F00u ||
             cpu.pc == 0x8029713Cu || cpu.pc == 0x802A9664u) &&
            g_audio_lifecycle_reports < 128u) {
            fprintf(stderr,
                    "[audio-lifecycle] pc=0x%08X r3=0x%08X r4=0x%08X "
                    "r5=0x%08X blocks=%llu\n",
                    cpu.pc, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5],
                    (unsigned long long)blocks);
            g_audio_lifecycle_reports++;
        }
        if ((cpu.pc == 0x80290490u || cpu.pc == 0x80296828u ||
             cpu.pc == 0x80296860u || cpu.pc == 0x802827F0u) &&
            g_audio_sequence_contract_reports < 16u) {
            if (cpu.pc == 0x80290490u || cpu.pc == 0x80296828u ||
                cpu.pc == 0x80296860u) {
                const u32 seq_base = mem_read32(&cpu, cpu.gpr[13] - 27328u);
                const u32 slot0 = mem_read32(&cpu, seq_base + 72u);
                const u32 slot1 = mem_read32(&cpu, seq_base + 80u + 72u);
                const u32 slot2 = mem_read32(&cpu, seq_base + 2u * 80u + 72u);
                const u32 slot4 = mem_read32(&cpu, seq_base + 4u * 80u + 72u);
                fprintf(stderr,
                        "[audio-sequence-contract] %s base=0x%08X "
                        "slot0=0x%08X state=%u flags=0x%08X id=0x%08X "
                        "slot1=0x%08X state=%u flags=0x%08X id=0x%08X "
                        "slot2=0x%08X state=%u flags=0x%08X id=0x%08X "
                        "slot4=0x%08X state=%u flags=0x%08X id=0x%08X "
                        "blocks=%llu\n",
                        cpu.pc == 0x80290490u
                            ? "process-return"
                            : (cpu.pc == 0x80296828u ? "process" : "entered"),
                        seq_base,
                        slot0,
                        slot0 != 0u ? mem_read8(&cpu, slot0 + 5u) : 0u,
                        slot0 != 0u ? mem_read32(&cpu, slot0 + 8u) : 0u,
                        slot0 != 0u ? mem_read32(&cpu, slot0 + 12u) : 0u,
                        slot1,
                        slot1 != 0u ? mem_read8(&cpu, slot1 + 5u) : 0u,
                        slot1 != 0u ? mem_read32(&cpu, slot1 + 8u) : 0u,
                        slot1 != 0u ? mem_read32(&cpu, slot1 + 12u) : 0u,
                        slot2,
                        slot2 != 0u ? mem_read8(&cpu, slot2 + 5u) : 0u,
                        slot2 != 0u ? mem_read32(&cpu, slot2 + 8u) : 0u,
                        slot2 != 0u ? mem_read32(&cpu, slot2 + 12u) : 0u,
                        slot4,
                        slot4 != 0u ? mem_read8(&cpu, slot4 + 5u) : 0u,
                        slot4 != 0u ? mem_read32(&cpu, slot4 + 8u) : 0u,
                        slot4 != 0u ? mem_read32(&cpu, slot4 + 12u) : 0u,
                        (unsigned long long)blocks);
                const u32 metadata = seq_base + 2u * 80u;
                fprintf(stderr,
                        "[audio-sequence-metadata] pc=0x%08X base=0x%08X "
                        "slot=2 f0=%u f1=%u f2=%u f3=%u f4=0x%08X "
                        "f8=0x%08X f48=0x%08X blocks=%llu\n",
                        cpu.pc, metadata, mem_read8(&cpu, metadata + 0u),
                        mem_read8(&cpu, metadata + 1u),
                        mem_read8(&cpu, metadata + 2u),
                        mem_read8(&cpu, metadata + 3u),
                        mem_read32(&cpu, metadata + 4u),
                        mem_read32(&cpu, metadata + 8u),
                        mem_read32(&cpu, metadata + 0x48u),
                        (unsigned long long)blocks);
            } else {
                const u32 track = cpu.gpr[3];
                fprintf(stderr,
                        "[audio-sequence-contract] start track=0x%08X "
                        "track_state=%u seq_state=%u seq_id=0x%08X "
                        "blocks=%llu\n",
                        track, mem_read8(&cpu, track + 0x37Eu),
                        mem_read8(&cpu, track + 0x37Bu),
                        mem_read32(&cpu, track + 0x0Cu),
                        (unsigned long long)blocks);
            }
            g_audio_sequence_contract_reports++;
        }
        if (g_audio_object_watch && cpu.pc == 0x8029E1B4u &&
            cpu.lr == 0x80296EB8u && g_audio_sequence_active_reports < 16u) {
            const u32 track = cpu.gpr[3];
            unsigned child_count = 0u;
            u32 first_child = 0u;
            for (u32 i = 0u; i < 16u; i++) {
                const u32 child = mem_read32(&cpu, track + 0x320u + i * 4u);
                if (child != 0u) {
                    child_count++;
                    if (first_child == 0u)
                        first_child = child;
                }
            }
            fprintf(stderr,
                    "[audio-sequence-active] entry track=0x%08X "
                    "active=%u state=%u children=%u first=0x%08X "
                    "lr=0x%08X blocks=%llu\n",
                    track, mem_read8(&cpu, track + 0x37Eu),
                    mem_read8(&cpu, track + 0x37Bu), child_count, first_child,
                    cpu.lr, (unsigned long long)blocks);
            g_audio_sequence_active_track = track;
            g_audio_sequence_active_pending = true;
            g_audio_sequence_active_reports++;
        }
        if (g_audio_object_watch && g_audio_sequence_active_pending &&
            cpu.pc == 0x80296EB8u) {
            const u32 seq_base = mem_read32(&cpu, cpu.gpr[13] - 27328u);
            const u32 object = mem_read32(&cpu, seq_base + 2u * 80u + 72u);
            fprintf(stderr,
                    "[audio-sequence-active] return track=0x%08X result=%u "
                    "object=0x%08X object_state=%u blocks=%llu\n",
                    g_audio_sequence_active_track, cpu.gpr[3] & 0xFFu, object,
                    object != 0u ? mem_read8(&cpu, object + 5u) : 0u,
                    (unsigned long long)blocks);
            g_audio_sequence_active_pending = false;
        }
        if (g_audio_object_watch && cpu.pc == 0x8029E518u &&
            cpu.lr == 0x80296ED4u && g_audio_sequence_active_reports < 16u) {
            const u32 update_data = cpu.gpr[3];
            const u32 object = mem_read32(&cpu, update_data + 72u);
            fprintf(stderr,
                    "[audio-sequence-active] root-init entry data=0x%08X "
                    "object=0x%08X object_state=%u blocks=%llu\n",
                    update_data, object,
                    object != 0u ? mem_read8(&cpu, object + 5u) : 0u,
                    (unsigned long long)blocks);
            g_audio_sequence_root_init_data = update_data;
            g_audio_sequence_root_init_pending = true;
            g_audio_sequence_active_reports++;
        }
        if (g_audio_object_watch && g_audio_sequence_root_init_pending &&
            cpu.pc == 0x80296ED4u) {
            const u32 object =
                mem_read32(&cpu, g_audio_sequence_root_init_data + 72u);
            fprintf(stderr,
                    "[audio-sequence-active] root-init return data=0x%08X "
                    "object=0x%08X object_state=%u blocks=%llu\n",
                    g_audio_sequence_root_init_data, object,
                    object != 0u ? mem_read8(&cpu, object + 5u) : 0u,
                    (unsigned long long)blocks);
            g_audio_sequence_root_init_pending = false;
        }
        if (cpu.pc == 0x802981F0u &&
            g_audio_sequence_contract_reports < 32u) {
            const u32 object = 0x8076C050u;
            fprintf(stderr,
                    "[audio-sequence-contract] init-return pc=0x%08X "
                    "object=0x%08X state=%u flags=0x%08X id=0x%08X "
                    "r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X "
                    "blocks=%llu\n",
                    cpu.pc, object, mem_read8(&cpu, object + 5u),
                    mem_read32(&cpu, object + 8u),
                    mem_read32(&cpu, object + 12u),
                    cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.gpr[6],
                    (unsigned long long)blocks);
            g_audio_sequence_contract_reports++;
            g_audio_transition_trace_remaining = 16u;
        }
        if (g_audio_transition_trace_remaining != 0u) {
            const u32 object = 0x8076C050u;
            fprintf(stderr,
                    "[audio-sequence-transition] pc=0x%08X state=%u "
                    "flags=0x%08X id=0x%08X r3=0x%08X r4=0x%08X "
                    "lr=0x%08X blocks=%llu remaining=%u\n",
                    cpu.pc, mem_read8(&cpu, object + 5u),
                    mem_read32(&cpu, object + 8u),
                    mem_read32(&cpu, object + 12u), cpu.gpr[3], cpu.gpr[4],
                    cpu.lr, (unsigned long long)blocks,
                    g_audio_transition_trace_remaining);
            g_audio_transition_trace_remaining--;
        }
        if ((cpu.pc == 0x80297FD0u || cpu.pc == 0x80298208u ||
             cpu.pc == 0x8029B0C4u || cpu.pc == 0x8029B4ACu) &&
            g_audio_sequence_contract_reports < 32u) {
            if (cpu.pc == 0x8029B4ACu) {
                fprintf(stderr,
                        "[audio-sequence-contract] link-get pc=0x%08X "
                        "link=0x%08X head=0x%08X tail=0x%08X buffer=0x%08X "
                        "id=0x%08X blocks=%llu\n",
                        cpu.pc, cpu.gpr[3], mem_read32(&cpu, cpu.gpr[3]),
                        mem_read32(&cpu, cpu.gpr[3] + 4u),
                        mem_read32(&cpu, cpu.gpr[3] + 8u), cpu.gpr[5],
                        (unsigned long long)blocks);
            } else if (cpu.pc == 0x80297FD0u) {
                const u32 seq_base = mem_read32(&cpu, cpu.gpr[13] - 27328u);
                const u32 slot = mem_read8(&cpu, cpu.gpr[8] + 5u);
                fprintf(stderr,
                        "[audio-sequence-contract] store pc=0x%08X "
                        "out=0x%08X id=0x%08X state=%u slot=%u "
                        "base=0x%08X slot_ptr=0x%08X blocks=%llu\n",
                        cpu.pc, cpu.gpr[3], cpu.gpr[5],
                        mem_read8(&cpu, cpu.gpr[8] + 5u), slot, seq_base,
                        mem_read32(&cpu, seq_base + slot * 80u + 72u),
                        (unsigned long long)blocks);
            } else {
                fprintf(stderr,
                        "[audio-sequence-contract] call pc=0x%08X "
                        "r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X "
                        "r7=0x%08X r8=0x%08X lr=0x%08X blocks=%llu\n",
                        cpu.pc, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5],
                        cpu.gpr[6], cpu.gpr[7], cpu.gpr[8], cpu.lr,
                        (unsigned long long)blocks);
            }
            g_audio_sequence_contract_reports++;
        }
        host_refresh_interrupt_sources(&cpu);
        if (g_audio_object_watch && g_dsp_mail_from_pending &&
            g_dsp_mail_from == 0xDCD10000u &&
            g_audio_dsp_source_reports < 24u) {
            fprintf(stderr,
                    "[audio-dsp-source] pc=0x%08X msr=0x%08X cause=0x%08X "
                    "mask=0x%08X current=0x%08X context=0x%08X "
                    "task_pending=%u\n",
                    cpu.pc, cpu.msr, dol_interrupts_pi_cause(&g_interrupts),
                    dol_interrupts_pi_mask(&g_interrupts),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    g_dsp_task_request_pending ? 1u : 0u);
            g_audio_dsp_source_reports++;
        }
        if (g_runqueue_trace &&
            aram_piece_command_trace_reports < 128u &&
            (cpu.pc == 0x802B5C60u || cpu.pc == 0x802B5C64u ||
             cpu.pc == 0x802B5C68u || cpu.pc == 0x802B5C6Cu ||
             cpu.pc == 0x802B5C70u || cpu.pc == 0x802B5C74u ||
             cpu.pc == 0x802B5DA4u || cpu.pc == 0x802B5DACu ||
             cpu.pc == 0x802B5DB0u || cpu.pc == 0x802B5DBCu ||
             (cpu.pc == 0x802B5C50u && cpu.gpr[25] == 0x80ADDFECu) ||
             (cpu.pc == 0x802B5DC8u && cpu.gpr[26] == 0x80ADDF44u))) {
            const u32 command = cpu.pc <= 0x802B5C74u ? cpu.gpr[4] : cpu.gpr[26];
            const u32 message = cpu.pc >= 0x802B5DA4u ? cpu.gpr[25] : 0u;
            const u32 heap = mem_read32(&cpu, cpu.gpr[13] - 27064u);
            const u32 heap_vtable = heap != 0u ? mem_read32(&cpu, heap) : 0u;
            const u32 heap_do_alloc = heap_vtable != 0u
                                          ? mem_read32(&cpu, heap_vtable + 36u)
                                          : 0u;
            const u32 free_head = heap != 0u ? mem_read32(&cpu, heap + 0x78u) : 0u;
            const u32 free_tail = heap != 0u ? mem_read32(&cpu, heap + 0x7Cu) : 0u;
            fprintf(stderr,
                    "[aram-piece] pc=0x%08X command=0x%08X message=0x%08X "
                    "r3=0x%08X r4=0x%08X inputs=%08X,%08X,%08X,%08X "
                    "heap=0x%08X vtable=0x%08X do_alloc=0x%08X "
                    "start=0x%08X end=0x%08X size=0x%08X "
                    "mode=0x%02X group=0x%02X field6e=0x%02X "
                    "free=0x%08X tail_free=0x%08X used=0x%08X tail_used=0x%08X "
                    "free0_magic=0x%04X flags=0x%02X group=0x%02X size=0x%08X "
                    "prev=0x%08X next=0x%08X "
                    "freet_magic=0x%04X flags=0x%02X group=0x%02X size=0x%08X "
                    "prev=0x%08X next=0x%08X "
                    "type=0x%08X length=%u src=0x%08X "
                    "dst=0x%08X aram=0x%08X callback=0x%08X lr=0x%08X\n",
                    cpu.pc, command, message, cpu.gpr[3], cpu.gpr[4],
                    cpu.gpr[26], cpu.gpr[27], cpu.gpr[28], cpu.gpr[29],
                    heap, heap_vtable, heap_do_alloc,
                    heap != 0u ? mem_read32(&cpu, heap + 0x30u) : 0u,
                    heap != 0u ? mem_read32(&cpu, heap + 0x34u) : 0u,
                    heap != 0u ? mem_read32(&cpu, heap + 0x38u) : 0u,
                    heap != 0u ? mem_read8(&cpu, heap + 0x6Cu) : 0u,
                    heap != 0u ? mem_read8(&cpu, heap + 0x6Du) : 0u,
                    heap != 0u ? mem_read8(&cpu, heap + 0x6Eu) : 0u,
                    free_head, free_tail,
                    heap != 0u ? mem_read32(&cpu, heap + 0x80u) : 0u,
                    heap != 0u ? mem_read32(&cpu, heap + 0x84u) : 0u,
                    free_head != 0u ? mem_read16(&cpu, free_head) : 0u,
                    free_head != 0u ? mem_read8(&cpu, free_head + 0x02u) : 0u,
                    free_head != 0u ? mem_read8(&cpu, free_head + 0x03u) : 0u,
                    free_head != 0u ? mem_read32(&cpu, free_head + 0x04u) : 0u,
                    free_head != 0u ? mem_read32(&cpu, free_head + 0x08u) : 0u,
                    free_head != 0u ? mem_read32(&cpu, free_head + 0x0Cu) : 0u,
                    free_tail != 0u ? mem_read16(&cpu, free_tail) : 0u,
                    free_tail != 0u ? mem_read8(&cpu, free_tail + 0x02u) : 0u,
                    free_tail != 0u ? mem_read8(&cpu, free_tail + 0x03u) : 0u,
                    free_tail != 0u ? mem_read32(&cpu, free_tail + 0x04u) : 0u,
                    free_tail != 0u ? mem_read32(&cpu, free_tail + 0x08u) : 0u,
                    free_tail != 0u ? mem_read32(&cpu, free_tail + 0x0Cu) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x40u) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x44u) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x48u) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x4Cu) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x50u) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x58u) : 0u,
                    cpu.lr);
            aram_piece_command_trace_reports++;
        }
        if (bluewake_scheduler_interrupt_requires_host(
                cpu.pc, cpu.msr,
                dol_interrupts_external_pending(&g_interrupts))) {
            host_delivery_safety_observe(&cpu, "external");
            deliver_external_interrupt(&cpu);
        } else if (dol_interrupts_external_pending(&g_interrupts) &&
                   vi_pending_while_disabled_reports < 8u) {
            fprintf(stderr,
                    "[interrupt] pending while disabled pc=0x%08X msr=0x%08X "
                    "cause=0x%08X mask=0x%08X vi_di0=0x%04X\n",
                    cpu.pc, cpu.msr, dol_interrupts_pi_cause(&g_interrupts),
                    dol_interrupts_pi_mask(&g_interrupts),
                    (u16)dol_interrupts_mmio_read(&g_interrupts, 0xCC002030u, 2u));
            vi_pending_while_disabled_reports++;
        }
        // VI retrace polling requires a full DolInterrupts setup (P4 milestone 2+).
        if (!vi_handler_reported && cpu.pc == 0x80312FFCu) {
            fprintf(stderr, "[vi] entering __VIRetraceHandler\n");
            vi_handler_reported = true;
        }
        if (cpu.pc == 0x802C8110u && vi_callback_reports < 4u) {
            fprintf(stderr,
                    "[vi] entering JUTVideo::postRetraceProc #%u retraces=%u "
                    "video_used=%u main_state=%u main_queue=0x%08X\n",
                    vi_callback_reports + 1u,
                    mem_read32(&cpu, 0x803F7B3Cu),
                    mem_read32(&cpu, 0x80429EE8u + 0x1Cu),
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read32(&cpu, 0x803A2960u + 0x2DCu));
            if (vi_callback_reports == 3u) {
                const u32 main_thread = 0x803A2960u;
                fprintf(stderr,
                        "[sched] main-thread-at-retrace4 state=%u attr=0x%04X "
                        "suspend=%d effective=%d base=%d queue=0x%08X "
                        "next=0x%08X prev=0x%08X context_state=0x%04X "
                        "saved_pc=0x%08X current_thread=0x%08X "
                        "current_context=0x%08X run_hint=%u reschedule=%d\n",
                        mem_read16(&cpu, main_thread + 0x2C8u),
                        mem_read16(&cpu, main_thread + 0x2CAu),
                        (s32)mem_read32(&cpu, main_thread + 0x2CCu),
                        (s32)mem_read32(&cpu, main_thread + 0x2D0u),
                        (s32)mem_read32(&cpu, main_thread + 0x2D4u),
                        mem_read32(&cpu, main_thread + 0x2DCu),
                        mem_read32(&cpu, main_thread + 0x2E0u),
                        mem_read32(&cpu, main_thread + 0x2E4u),
                        mem_read16(&cpu, main_thread + 0x1A2u),
                        mem_read32(&cpu, main_thread + 0x198u),
                        mem_read32(&cpu, 0x800000E4u),
                        mem_read32(&cpu, 0x800000D4u),
                        mem_read32(&cpu, 0x803F7A34u),
                        (s32)mem_read32(&cpu, 0x803F7A38u));
            }
            vi_callback_reports++;
            video_callback_reported = true;
        }
        if (g_archive_reports < 12u &&
            (cpu.pc == 0x802B8008u || cpu.pc == 0x802B8380u ||
             cpu.pc == 0x802B8450u)) {
            char name[128] = {0};
            const u32 name_address = cpu.pc == 0x802B8450u ? cpu.gpr[5] : cpu.gpr[4];
            if (name_address != 0u)
                guest_read_cstr(&cpu, name_address, name, sizeof(name));
            fprintf(stderr,
                    "[archive] pc=0x%08X this=0x%08X type=0x%08X name=\"%s\" "
                    "r6=0x%08X lr=0x%08X\n",
                    cpu.pc, cpu.gpr[3], cpu.gpr[4], name, cpu.gpr[6], cpu.lr);
            g_archive_reports++;
        }
        if (cpu.pc == 0x8030F2B0u) {
            char path[256] = {0};
            if (cpu.gpr[3] != 0u)
                guest_read_cstr(&cpu, cpu.gpr[3], path, sizeof(path));
            const s32 entry = dvd_path_to_entrynum(path);
            cpu.gpr[3] = (u32)entry;
            cpu.pc = cpu.lr & ~3u;
            if (dvd_path_reports < 16u) {
                fprintf(stderr,
                        "[dvd] path-convert #%u path=\"%s\" entry=%d "
                        "return=0x%08X\n",
                        dvd_path_reports + 1u, path, entry, cpu.pc);
                dvd_path_reports++;
            }
        }
        if (g_resource_lookup_reports < 24u &&
            (cpu.pc == 0x802B7410u || cpu.pc == 0x802B7564u ||
             cpu.pc == 0x802B7630u || cpu.pc == 0x802B7754u ||
             cpu.pc == 0x802B7F04u || cpu.pc == 0x802B7F30u ||
             cpu.pc == 0x802B8380u || cpu.pc == 0x802B8450u)) {
            char resource_name[160] = {0};
            const u32 name_address = (cpu.pc == 0x802B7564u ||
                                      cpu.pc == 0x802B7754u ||
                                      cpu.pc == 0x802B8450u)
                                         ? cpu.gpr[5]
                                         : cpu.gpr[4];
            if (name_address != 0u)
                guest_read_cstr(&cpu, name_address, resource_name,
                                sizeof(resource_name));
            fprintf(stderr,
                    "[res] lookup pc=0x%08X this=0x%08X type=0x%08X "
                    "name=\"%s\" r5=0x%08X r6=0x%08X lr=0x%08X\n",
                    cpu.pc, cpu.gpr[3], cpu.gpr[4], resource_name, cpu.gpr[5],
                    cpu.gpr[6], cpu.lr);
            g_resource_lookup_reports++;
        }
        if (g_resource_info_reports < 24u &&
            (cpu.pc == 0x8006D8F4u || cpu.pc == 0x8006F164u ||
             cpu.pc == 0x8006EF78u || cpu.pc == 0x8006EFC0u)) {
            char resource_name[160] = {0};
            if (cpu.gpr[4] != 0u)
                guest_read_cstr(&cpu, cpu.gpr[4], resource_name,
                                sizeof(resource_name));
            fprintf(stderr,
                    "[res] info-trace pc=0x%08X r3=0x%08X r4=0x%08X "
                    "r5=0x%08X r6=0x%08X r7=0x%08X lr=0x%08X name=\"%s\"\n",
                    cpu.pc, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.gpr[6],
                    cpu.gpr[7], cpu.lr, resource_name);
            g_resource_info_reports++;
        }
        if (g_dynamic_load_reports < 8u &&
            (cpu.pc == 0x80240744u || cpu.pc == 0x80240A48u)) {
            char module_name[96] = {0};
            const u32 object = cpu.gpr[3];
            const u32 name = mem_read32(&cpu, object + 0x1Cu);
            if (name != 0u)
                guest_read_cstr(&cpu, name, module_name, sizeof(module_name));
            fprintf(stderr,
                    "[rel] dynamic-load pc=0x%08X object=0x%08X name=\"%s\" "
                    "module=0x%08X archive=0x%08X cache=0x%08X lr=0x%08X\n",
                    cpu.pc, object, module_name, mem_read32(&cpu, object + 0x10u),
                    mem_read32(&cpu, 0x803F7314u), mem_read32(&cpu, 0x803F7318u),
                    cpu.lr);
            g_dynamic_load_reports++;
        }
        if (!g_dynamic_link_header_reported && cpu.pc == 0x80240C98u) {
            const u32 object = cpu.gpr[31];
            const u32 module = mem_read32(&cpu, object + 0x10u);
            fprintf(stderr,
                    "[rel] do-link-header object=0x%08X module=0x%08X "
                    "id=%u version=%u section_offset=0x%08X "
                    "fix_size=0x%08X lr=0x%08X\n",
                    object, module, mem_read32(&cpu, module),
                    mem_read32(&cpu, module + 0x1Cu),
                    mem_read32(&cpu, module + 0x10u),
                    mem_read32(&cpu, module + 0x48u), cpu.lr);
            g_dynamic_link_header_reported = true;
        }
        if ((cpu.pc == 0x80240744u || cpu.pc == 0x80240A48u) &&
            cpu.gpr[3] != 0u) {
            const u32 object = cpu.gpr[3];
            const u32 name = mem_read32(&cpu, object + 0x1Cu);
            char module_name[96] = {0};
            if (name != 0u)
                guest_read_cstr(&cpu, name, module_name, sizeof(module_name));
            if (strcmp(module_name, "d_a_kamome") == 0) {
                fprintf(stderr,
                        "[rel] kamome-load pc=0x%08X object=0x%08X "
                        "module=0x%08X name=0x%08X lr=0x%08X\n",
                        cpu.pc, object, mem_read32(&cpu, object + 0x10u),
                        name, cpu.lr);
            }
        }
        if (cpu.pc == 0x80240744u) {
            const u32 object = cpu.gpr[3];
            const u32 name_address = mem_read32(&cpu, object + 0x1Cu);
            char module_name[96] = {0};
            if (name_address != 0u)
                guest_read_cstr(&cpu, name_address, module_name, sizeof(module_name));
            if (module_name[0] != '\0' &&
                    mem_read32(&cpu, object + 0x10u) == 0u) {
                u32 module = 0;
                u32 module_size = 0;
                if (host_materialize_rel(&cpu, module_name, object, &module,
                                         &module_size)) {
                    mem_write32(&cpu, object + 0x10u, module);
                    host_register_rel_alias(&cpu, mod, module);
                    const u32 module_id = mem_read32(&cpu, module);
                    host_zero_rel_bss(&cpu, rel_data, rel_data_count, module_id);
                    if (module_id == 1u)
                        g_module1_raw_base = module;
                    // This is the host-side completion of the authentic
                    // do_link contract: the static composite supplies the
                    // linked module body, so publish module 1's profile-list
                    // global at the real link event.
                    if (module_id == 1u && !profile_prolog_called)
                        profile_prolog_called = host_activate_rel_profile_list(
                            &cpu, rel_data, rel_data_count, module_id);
                    fprintf(stderr,
                            "[rel] archived resource materialized name=\"%s\" "
                            "guest=0x%08X size=%u source=private-derived-rel\n",
                            module_name, module, module_size);
                    cpu.gpr[3] = 1u;
                    cpu.pc = cpu.lr & ~3u;
                    continue;
                }
                fprintf(stderr,
                        "[rel] archived resource unavailable name=\"%s\"\n",
                        module_name);
            }
        }
        if (cpu.pc == 0x81E000D4u ||
            (g_module1_raw_base != 0u && cpu.pc == g_module1_raw_base + 0xD4u)) {
            // Preserve the existing module-1 compatibility entry. Generic
            // executable-section aliases are registered at materialization.
            const u32 linked_entry =
                host_rel_section_linked_start(mod, 1u, 1u);
            fprintf(stderr,
                    "[rel] archived module entry alias raw=0x%08X -> "
                    "0x%08X\n", cpu.pc, linked_entry);
            if (linked_entry != 0u)
                cpu.pc = linked_entry;
        }
        if (video_callback_reported && message_send_reports < 4u &&
            cpu.pc == 0x80305908u) {
            fprintf(stderr,
                    "[vi] OSSendMessage entry #%u mq=0x%08X msg=0x%08X flags=%u "
                    "used=%u capacity=%u\n",
                    message_send_reports + 1u, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5],
                    mem_read32(&cpu, cpu.gpr[3] + 0x1Cu),
                    mem_read32(&cpu, cpu.gpr[3] + 0x14u));
            message_send_reports++;
            message_send_reported = true;
        }
        if (cpu.pc == 0x80308B88u &&
            (!wake_reported || cpu.gpr[3] == 0x80429EF0u)) {
            fprintf(stderr,
                    "[vi] OSWakeupThread entry queue=0x%08X head=0x%08X "
                    "tail=0x%08X\n",
                    cpu.gpr[3], mem_read32(&cpu, cpu.gpr[3]),
                    mem_read32(&cpu, cpu.gpr[3] + 4u));
            wake_reported = true;
        }
        if (g_runqueue_trace &&
            retrace_wakeup_reports < 8u && cpu.pc == 0x80308B88u &&
            cpu.gpr[3] == 0x803F7B44u) {
            const u32 queue = cpu.gpr[3];
            const u32 head = mem_read32(&cpu, queue);
            const u32 main_thread = 0x803A2960u;
            fprintf(stderr,
                    "[vi] retrace-wakeup entry #%u queue=0x%08X "
                    "head=0x%08X tail=0x%08X head_state=%u head_queue=0x%08X "
                    "head_next=0x%08X head_prev=0x%08X main_state=%u "
                    "main_queue=0x%08X main_saved_pc=0x%08X run_bits=0x%08X "
                    "current_thread=0x%08X current_context=0x%08X\n",
                    retrace_wakeup_reports + 1u, queue, head,
                    mem_read32(&cpu, queue + 4u),
                    head ? mem_read16(&cpu, head + 0x2C8u) : 0u,
                    head ? mem_read32(&cpu, head + 0x2DCu) : 0u,
                    head ? mem_read32(&cpu, head + 0x2E0u) : 0u,
                    head ? mem_read32(&cpu, head + 0x2E4u) : 0u,
                    mem_read16(&cpu, main_thread + 0x2C8u),
                    mem_read32(&cpu, main_thread + 0x2DCu),
                    mem_read32(&cpu, main_thread + 0x198u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            retrace_wakeup_reports++;
            retrace_wakeup_pending = true;
        }
        if (g_runqueue_trace &&
            video_tick_queue_trace_reports < 32u &&
            (cpu.pc == 0x803059D0u || cpu.pc == 0x80305908u) &&
            cpu.gpr[3] == 0x80429EE8u) {
            fprintf(stderr,
                    "[vi] tick-queue #%u pc=0x%08X queue=0x%08X "
                    "message=0x%08X flags=%u used=%u capacity=%u "
                    "recv=0x%08X send=0x%08X retraces=%u "
                    "thread=0x%08X state=%u queue_link=0x%08X lr=0x%08X\n",
                    video_tick_queue_trace_reports + 1u, cpu.pc, cpu.gpr[3],
                    cpu.gpr[4], cpu.gpr[5], mem_read32(&cpu, cpu.gpr[3] + 0x1Cu),
                    mem_read32(&cpu, cpu.gpr[3] + 0x14u),
                    mem_read32(&cpu, cpu.gpr[3] + 0x8u),
                    mem_read32(&cpu, cpu.gpr[3] + 0xCu),
                    mem_read32(&cpu, 0x803F7B3Cu), mem_read32(&cpu, 0x800000E4u),
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read32(&cpu, 0x803A2960u + 0x2DCu), cpu.lr);
            video_tick_queue_trace_reports++;
        }
        if (g_runqueue_trace &&
            display_tick_wait_trace_reports < 24u &&
            (cpu.pc == 0x80255D34u || cpu.pc == 0x80255E78u ||
             cpu.pc == 0x80255E9Cu || cpu.pc == 0x80255ED0u)) {
            fprintf(stderr,
                    "[display] tick-wait #%u pc=0x%08X r3=0x%08X r4=0x%08X "
                    "r5=0x%08X lr=0x%08X retraces=%u current=0x%08X "
                    "main_state=%u main_queue=0x%08X run_bits=0x%08X\n",
                    display_tick_wait_trace_reports + 1u, cpu.pc, cpu.gpr[3],
                    cpu.gpr[4], cpu.gpr[5], cpu.lr,
                    mem_read32(&cpu, 0x803F7B3Cu), mem_read32(&cpu, 0x800000E4u),
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read32(&cpu, 0x803A2960u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7A30u));
            display_tick_wait_trace_reports++;
        }
        if (g_runqueue_trace &&
            display_alarm_trace_reports < 48u &&
            (((cpu.pc == 0x80301F44u || cpu.pc == 0x803021A4u) &&
              cpu.gpr[3] == 0x8040CC78u) ||
             (cpu.pc == 0x80301F54u && cpu.gpr[3] == 0x8040CC78u) ||
             (cpu.pc == 0x80255E54u && cpu.gpr[3] == 0x8040CC78u) ||
             (cpu.pc == 0x803086A4u && cpu.gpr[3] == 0x803A2960u))) {
            const u32 alarm = 0x8040CC78u;
            fprintf(stderr,
                    "[alarm] #%u pc=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X "
                    "handler=0x%08X tag=0x%08X fire_hi=0x%08X fire_lo=0x%08X "
                    "prev=0x%08X next=0x%08X period_hi=0x%08X period_lo=0x%08X "
                    "thread=0x%08X main_state=%u main_queue=0x%08X "
                    "run_bits=0x%08X lr=0x%08X\n",
                    display_alarm_trace_reports + 1u, cpu.pc, cpu.gpr[3],
                    cpu.gpr[4], cpu.gpr[5], mem_read32(&cpu, alarm + 0x00u),
                    mem_read32(&cpu, alarm + 0x04u), mem_read32(&cpu, alarm + 0x08u),
                    mem_read32(&cpu, alarm + 0x0Cu), mem_read32(&cpu, alarm + 0x10u),
                    mem_read32(&cpu, alarm + 0x14u), mem_read32(&cpu, alarm + 0x18u),
                    mem_read32(&cpu, alarm + 0x1Cu), mem_read32(&cpu, 0x800000E4u),
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read32(&cpu, 0x803A2960u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7A30u), cpu.lr);
            display_alarm_trace_reports++;
        }
        if (g_runqueue_trace &&
            main_wakeup_trace_reports < 16u && cpu.pc == 0x80308B88u &&
            mem_read32(&cpu, 0x803A2960u + 0x2DCu) != 0u &&
            mem_read16(&cpu, 0x803A2960u + 0x2C8u) == 4u) {
            const u32 main_thread = 0x803A2960u;
            fprintf(stderr,
                    "[sched] main-wakeup #%u queue=0x%08X head=0x%08X "
                    "tail=0x%08X main_queue=0x%08X main_next=0x%08X "
                    "main_prev=0x%08X main_state=%u run_bits=0x%08X "
                    "run_hint=%u reschedule=%d retrace_head=0x%08X "
                    "retrace_tail=0x%08X current_thread=0x%08X "
                    "current_context=0x%08X\n",
                    main_wakeup_trace_reports + 1u, cpu.gpr[3],
                    mem_read32(&cpu, cpu.gpr[3]),
                    mem_read32(&cpu, cpu.gpr[3] + 4u),
                    mem_read32(&cpu, main_thread + 0x2DCu),
                    mem_read32(&cpu, main_thread + 0x2E0u),
                    mem_read32(&cpu, main_thread + 0x2E4u),
                    mem_read16(&cpu, main_thread + 0x2C8u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x803F7A34u),
                    (s32)mem_read32(&cpu, 0x803F7A38u),
                    mem_read32(&cpu, 0x803F7B44u),
                    mem_read32(&cpu, 0x803F7B48u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            main_wakeup_trace_reports++;
        }
        if (g_runqueue_trace &&
            ((scheduler_return_trace_reports < 32u && blocks < 31000000ull) ||
             (late_scheduler_return_trace_reports < 32u && blocks >= 31000000ull)) &&
            (cpu.pc == 0x80305F50u || cpu.pc == 0x80305F54u ||
             cpu.pc == 0x80305F68u || cpu.pc == 0x80305F6Cu)) {
            fprintf(stderr,
                    "[sched] return-frame #%u pc=0x%08X lr=0x%08X "
                    "r1=0x%08X r0=0x%08X saved4=0x%08X saved16=0x%08X "
                    "saved20=0x%08X saved24=0x%08X saved28=0x%08X "
                    "saved36=0x%08X context=0x%08X current=0x%08X\n",
                    scheduler_return_trace_reports + 1u, cpu.pc, cpu.lr,
                    cpu.gpr[1], cpu.gpr[0], mem_read32(&cpu, cpu.gpr[1] + 4u),
                    mem_read32(&cpu, cpu.gpr[1] + 16u),
                    mem_read32(&cpu, cpu.gpr[1] + 20u),
                    mem_read32(&cpu, cpu.gpr[1] + 24u),
                    mem_read32(&cpu, cpu.gpr[1] + 28u),
                    mem_read32(&cpu, cpu.gpr[1] + 36u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read32(&cpu, 0x800000E4u));
            if (blocks >= 31000000ull)
                late_scheduler_return_trace_reports++;
            else
                scheduler_return_trace_reports++;
        }
        if (g_runqueue_trace &&
            late_continuation_trace_reports < 128u &&
            blocks >= 31000000ull &&
            (cpu.pc == 0x802B0500u || cpu.pc == 0x802B1904u ||
             cpu.pc == 0x802B1928u)) {
            fprintf(stderr,
                    "[sched] continuation #%u pc=0x%08X lr=0x%08X "
                    "r1=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X "
                    "r12=0x%08X ctr=0x%08X r28=0x%08X r29=0x%08X "
                    "r30=0x%08X r31=0x%08X saved36=0x%08X "
                    "context=0x%08X current=0x%08X run_bits=0x%08X\n",
                    late_continuation_trace_reports + 1u, cpu.pc, cpu.lr,
                    cpu.gpr[1], cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.gpr[12],
                    cpu.ctr, cpu.gpr[28], cpu.gpr[29], cpu.gpr[30], cpu.gpr[31],
                    mem_read32(&cpu, cpu.gpr[1] + 36u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x803F7A30u));
            late_continuation_trace_reports++;
        }
        if (g_runqueue_trace &&
            main_sleep_reports < 8u && cpu.pc == 0x80308A9Cu &&
            cpu.gpr[3] == 0x803F7B44u) {
            const u32 main_thread = 0x803A2960u;
            fprintf(stderr,
                    "[sched] main-sleep entry #%u queue=0x%08X "
                    "head=0x%08X tail=0x%08X state=%u thread_queue=0x%08X "
                    "next=0x%08X prev=0x%08X saved_pc=0x%08X "
                    "current_thread=0x%08X current_context=0x%08X "
                    "run_bits=0x%08X hint=%u reschedule=%d\n",
                    main_sleep_reports + 1u, cpu.gpr[3],
                    mem_read32(&cpu, cpu.gpr[3]),
                    mem_read32(&cpu, cpu.gpr[3] + 4u),
                    mem_read16(&cpu, main_thread + 0x2C8u),
                    mem_read32(&cpu, main_thread + 0x2DCu),
                    mem_read32(&cpu, main_thread + 0x2E0u),
                    mem_read32(&cpu, main_thread + 0x2E4u),
                    mem_read32(&cpu, main_thread + 0x198u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x803F7A34u),
                    (s32)mem_read32(&cpu, 0x803F7A38u));
            main_sleep_reports++;
        }
        if (g_runqueue_trace && retrace_wakeup_pending &&
            retrace_wakeup_return_reports < 8u && cpu.pc == 0x80313208u) {
            const u32 main_thread = 0x803A2960u;
            fprintf(stderr,
                    "[vi] retrace-wakeup return #%u main_state=%u "
                    "main_queue=0x%08X main_saved_pc=0x%08X main_next=0x%08X "
                    "main_prev=0x%08X retrace_head=0x%08X retrace_tail=0x%08X "
                    "run_bits=0x%08X current_thread=0x%08X "
                    "current_context=0x%08X\n",
                    retrace_wakeup_return_reports + 1u,
                    mem_read16(&cpu, main_thread + 0x2C8u),
                    mem_read32(&cpu, main_thread + 0x2DCu),
                    mem_read32(&cpu, main_thread + 0x198u),
                    mem_read32(&cpu, main_thread + 0x2E0u),
                    mem_read32(&cpu, main_thread + 0x2E4u),
                    mem_read32(&cpu, 0x803F7B44u),
                    mem_read32(&cpu, 0x803F7B48u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            retrace_wakeup_return_reports++;
            retrace_wakeup_pending = false;
        }
        if (g_runqueue_trace &&
            worker_wakeup_trace_reports < 24u && blocks >= 3000000ull &&
            cpu.pc == 0x80308B88u && cpu.gpr[3] == 0x8039CD68u) {
            fprintf(stderr,
                    "[sched] worker-wakeup-queue #%u queue=0x%08X "
                    "head=0x%08X tail=0x%08X worker_state=%u "
                    "worker_queue=0x%08X run_bits=0x%08X hint=%u\n",
                    worker_wakeup_trace_reports + 1u, cpu.gpr[3],
                    mem_read32(&cpu, cpu.gpr[3]),
                    mem_read32(&cpu, cpu.gpr[3] + 4u),
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u),
                    mem_read32(&cpu, 0x804211E0u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x803F7A34u));
            worker_wakeup_trace_reports++;
        }
        if (g_runqueue_trace &&
            aram_worker_sleep_trace_reports < 16u &&
            cpu.pc == 0x80308A9Cu && cpu.gpr[3] == 0x8039CD68u) {
            const u32 worker = 0x804211E0u;
            fprintf(stderr,
                    "[sched] aram-worker-sleep entry #%u queue=0x%08X "
                    "head=0x%08X tail=0x%08X worker_state=%u "
                    "worker_queue=0x%08X worker_saved_pc=0x%08X "
                    "run_bits=0x%08X run_hint=%u reschedule=%d "
                    "current_thread=0x%08X current_context=0x%08X\n",
                    aram_worker_sleep_trace_reports + 1u, cpu.gpr[3],
                    mem_read32(&cpu, cpu.gpr[3]),
                    mem_read32(&cpu, cpu.gpr[3] + 4u),
                    mem_read16(&cpu, worker + 0x2C8u),
                    mem_read32(&cpu, worker + 0x2DCu),
                    mem_read32(&cpu, worker + 0x198u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x803F7A34u),
                    (s32)mem_read32(&cpu, 0x803F7A38u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            aram_worker_sleep_trace_reports++;
        }
        if (g_runqueue_trace &&
            aram_worker_receive_resume_reports < 16u &&
            cpu.pc == 0x80305A28u &&
            mem_read32(&cpu, 0x800000E4u) == 0x804211E0u) {
            const u32 worker = 0x804211E0u;
            fprintf(stderr,
                    "[sched] aram-worker-receive resume #%u worker_state=%u "
                    "worker_queue=0x%08X worker_saved_pc=0x%08X "
                    "run_bits=0x%08X run_hint=%u reschedule=%d "
                    "current_context=0x%08X\n",
                    aram_worker_receive_resume_reports + 1u,
                    mem_read16(&cpu, worker + 0x2C8u),
                    mem_read32(&cpu, worker + 0x2DCu),
                    mem_read32(&cpu, worker + 0x198u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x803F7A34u),
                    (s32)mem_read32(&cpu, 0x803F7A38u),
                    mem_read32(&cpu, 0x800000D4u));
            aram_worker_receive_resume_reports++;
        }
        if (g_runqueue_trace && video_callback_reported &&
            !video_callback_state_reported &&
            blocks > 1000000ull && cpu.pc != 0x802C8110u) {
            const u32 video_queue = 0x80429EE8u;
            fprintf(stderr,
                    "[vi] postRetraceProc return used=%u first=%u "
                    "run_bits=0x%08X main_state=%u main_queue=0x%08X\n",
                    mem_read32(&cpu, video_queue + 0x1Cu),
                    mem_read32(&cpu, video_queue + 0x18u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read32(&cpu, 0x803A2960u + 0x2DCu));
            video_callback_state_reported = true;
        }
        if (g_runqueue_trace && message_send_reported &&
            receive_message_reports < 20u &&
            cpu.pc == 0x803059D0u &&
            (blocks >= 3000000ull || cpu.gpr[3] == 0x80429EE8u)) {
            fprintf(stderr,
                    "[sched] OSReceiveMessage entry #%u mq=0x%08X out=0x%08X "
                    "flags=%u used=%u capacity=%u current_thread=0x%08X "
                    "current_context=0x%08X main_state=%u main_queue=0x%08X\n",
                    receive_message_reports + 1u, cpu.gpr[3], cpu.gpr[4],
                    cpu.gpr[5], mem_read32(&cpu, cpu.gpr[3] + 0x1Cu),
                    mem_read32(&cpu, cpu.gpr[3] + 0x14u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read32(&cpu, 0x803A2960u + 0x2DCu));
            receive_message_reports++;
        }
        if (g_runqueue_trace &&
            worker_queue_trace_reports < 24u && blocks >= 3000000ull &&
            cpu.pc == 0x803059D0u && cpu.gpr[3] == 0x8039CD60u) {
            const u32 worker = mem_read32(&cpu, 0x800000E4u);
            fprintf(stderr,
                    "[sched] worker-receive #%u mq=0x%08X used=%u "
                    "thread=0x%08X state=%u priority=%d queue=0x%08X "
                    "next=0x%08X prev=0x%08X srr0=0x%08X srr1=0x%08X "
                    "run_bits=0x%08X hint=%u wait_head=0x%08X wait_tail=0x%08X\n",
                    worker_queue_trace_reports + 1u, cpu.gpr[3],
                    mem_read32(&cpu, cpu.gpr[3] + 0x1Cu), worker,
                    mem_read16(&cpu, worker + 0x2C8u),
                    (s32)mem_read32(&cpu, worker + 0x2D0u),
                    mem_read32(&cpu, worker + 0x2DCu),
                    mem_read32(&cpu, worker + 0x2E0u),
                    mem_read32(&cpu, worker + 0x2E4u),
                    mem_read32(&cpu, worker + 0x198u),
                    mem_read32(&cpu, worker + 0x19Cu),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x803F7A34u),
                    mem_read32(&cpu, cpu.gpr[3] + 0x8u),
                    mem_read32(&cpu, cpu.gpr[3] + 0xCu));
            worker_queue_trace_reports++;
        }
        if (g_runqueue_trace &&
            worker_receive_trace_reports < 24u && blocks >= 3000000ull &&
            cpu.pc == 0x803059D0u &&
            mem_read32(&cpu, 0x800000E4u) == 0x804211E0u) {
            const u32 worker = 0x804211E0u;
            fprintf(stderr,
                    "[sched] worker-receive-any #%u mq=0x%08X used=%u "
                    "capacity=%u thread_state=%u priority=%d queue=0x%08X "
                    "wait_head=0x%08X wait_tail=0x%08X saved_pc=0x%08X "
                    "run_bits=0x%08X hint=%u\n",
                    worker_receive_trace_reports + 1u, cpu.gpr[3],
                    mem_read32(&cpu, cpu.gpr[3] + 0x1Cu),
                    mem_read32(&cpu, cpu.gpr[3] + 0x14u),
                    mem_read16(&cpu, worker + 0x2C8u),
                    (s32)mem_read32(&cpu, worker + 0x2D0u),
                    mem_read32(&cpu, worker + 0x2DCu),
                    mem_read32(&cpu, cpu.gpr[3] + 0x8u),
                    mem_read32(&cpu, cpu.gpr[3] + 0xCu),
                    mem_read32(&cpu, worker + 0x198u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x803F7A34u));
            worker_receive_trace_reports++;
        }
        if (g_runqueue_trace && message_send_reported &&
            resumed_path_reports < 32u &&
            (cpu.pc == 0x80307EACu || cpu.pc == 0x80307FA0u ||
             cpu.pc == 0x80255E14u || cpu.pc == 0x80255E18u)) {
            fprintf(stderr,
                    "[sched] resumed-path pc=0x%08X r1=0x%08X r3=0x%08X "
                    "lr=0x%08X main_state=%u main_queue=0x%08X "
                    "video_used=%u\n",
                    cpu.pc, cpu.gpr[1], cpu.gpr[3], cpu.lr,
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read32(&cpu, 0x803A2960u + 0x2DCu),
                    mem_read32(&cpu, 0x80429EE8u + 0x1Cu));
            resumed_path_reports++;
        }
        if (g_runqueue_trace && message_send_reported && vi_wait_reports < 6u &&
            cpu.pc == 0x80313A04u) {
            fprintf(stderr,
                    "[vi] VIWaitForRetrace entry #%u r1=0x%08X target=%u "
                    "retrace_count=%u queue_head=0x%08X queue_tail=0x%08X\n",
                    vi_wait_reports + 1u, cpu.gpr[1], cpu.gpr[30],
                    mem_read32(&cpu, 0x803F7B3Cu),
                    mem_read32(&cpu, 0x803F7B44u),
                    mem_read32(&cpu, 0x803F7B48u));
            vi_wait_reports++;
        }
        if (g_runqueue_trace && message_send_reported &&
            vi_wait_return_reports < 6u &&
            cpu.pc == 0x80313A2Cu) {
            fprintf(stderr,
                    "[vi] VIWaitForRetrace post-wake #%u r1=0x%08X target=%u "
                    "retrace_count=%u queue_head=0x%08X queue_tail=0x%08X\n",
                    vi_wait_return_reports + 1u, cpu.gpr[1], cpu.gpr[30],
                    mem_read32(&cpu, 0x803F7B3Cu),
                    mem_read32(&cpu, 0x803F7B44u),
                    mem_read32(&cpu, 0x803F7B48u));
            vi_wait_return_reports++;
        }
        if (g_runqueue_trace && message_send_reported &&
            video_wait_reports < 8u &&
            (cpu.pc == 0x802C815Cu || cpu.pc == 0x802C81C0u ||
             cpu.pc == 0x802C81D0u)) {
            fprintf(stderr,
                    "[video] wait-helper pc=0x%08X r1=0x%08X r3=0x%08X "
                    "r4=0x%08X lr=0x%08X flag=%u video_used=%u "
                    "retrace_count=%u\n",
                    cpu.pc, cpu.gpr[1], cpu.gpr[3], cpu.gpr[4], cpu.lr,
                    mem_read8(&cpu, cpu.gpr[3] + 44u),
                    mem_read32(&cpu, 0x80429EE8u + 0x1Cu),
                    mem_read32(&cpu, 0x803F7B3Cu));
            video_wait_reports++;
        }
        if (cpu.pc == 0x80308A9Cu && cpu.lr == 0x80322C38u) {
            // The guest is about to be told the GPU finished, so everything it
            // has written to the FIFO has to be translated and submitted first.
            // This is the draw-done barrier of docs/status/CURRENT.md
            // 2026-09-22; the front end buffers writes and parses in batches.
            struct timespec flush_before;
            if (g_gx_flush_census) {
                if (g_gx_flush_retrace != g_host_retrace_count &&
                    g_gx_flush_retrace >= 13800u && g_gx_flush_lines < 2000u) {
                    g_gx_flush_lines++;
                    fprintf(stderr,
                            "[gx-flush] retrace=%llu calls=%llu us=%llu\n",
                            (unsigned long long)g_gx_flush_retrace,
                            (unsigned long long)g_gx_flush_retrace_calls,
                            (unsigned long long)g_gx_flush_retrace_us);
                }
                if (g_gx_flush_retrace != g_host_retrace_count) {
                    g_gx_flush_retrace = g_host_retrace_count;
                    g_gx_flush_retrace_calls = 0u;
                    g_gx_flush_retrace_us = 0u;
                }
                clock_gettime(CLOCK_MONOTONIC, &flush_before);
            }
            dol_platform_gx_flush();
            if (g_gx_flush_census) {
                struct timespec flush_after;
                clock_gettime(CLOCK_MONOTONIC, &flush_after);
                const u64 spent =
                    (u64)(flush_after.tv_sec - flush_before.tv_sec) * 1000000ull +
                    (u64)(flush_after.tv_nsec - flush_before.tv_nsec) / 1000ull;
                g_gx_flush_calls++;
                g_gx_flush_us_total += spent;
                g_gx_flush_retrace_calls++;
                g_gx_flush_retrace_us += spent;
                if (spent > g_gx_flush_us_max)
                    g_gx_flush_us_max = spent;
            }
            dol_interrupts_commit_pe_finish(&g_interrupts);
            if (g_runqueue_trace && gx_finish_interrupt_reports < 8u) {
                fprintf(stderr,
                        "[gx] PE finish committed cause=0x%08X mask=0x%08X "
                        "pc=0x%08X msr=0x%08X\n",
                        dol_interrupts_pi_cause(&g_interrupts),
                        dol_interrupts_pi_mask(&g_interrupts), cpu.pc, cpu.msr);
            }
        }
        if (cpu.pc == 0x80322BC8u && g_async_draw_done) {
            // GXSetDrawDone's return: the asynchronous draw-done token that
            // JUTVideo::drawDoneStart writes right after the display copy.
            // Hardware raises the PE finish as soon as the GPU drains the
            // FIFO, well inside a millisecond, and the finish callback clears
            // JUTVideo's sDrawWaiting so the next retrace shows the frame.
            // Only GXDrawDone committed the finish before, so sDrawWaiting
            // stayed set until the next frame's endFrame; whenever that fell
            // after the retrace the next beginRender woke on, the XFB
            // exchange found the last frame unshown, cleared the EFB and
            // threw the finished frame away. The game kept rendering 30
            // frames a second while every other one was discarded: the
            // steady 15 FPS phases with doubled draws per present.
            dol_platform_gx_flush();
            dol_interrupts_commit_pe_finish(&g_interrupts);
            g_async_draw_done_commits++;
        }
        if (g_runqueue_trace &&
            gx_finish_interrupt_reports < 8u && cpu.pc == 0x80322F20u) {
            fprintf(stderr,
                    "[gx] GXFinishInterruptHandler entry cause=0x%08X "
                    "mask=0x%08X pe_status=0x%04X finish_head=0x%08X "
                    "finish_tail=0x%08X current_thread=0x%08X\n",
                    dol_interrupts_pi_cause(&g_interrupts),
                    dol_interrupts_pi_mask(&g_interrupts),
                    (u16)dol_interrupts_mmio_read(&g_interrupts, 0xCC00100Au, 2u),
                    mem_read32(&cpu, 0x803F7C9Cu),
                    mem_read32(&cpu, 0x803F7CA0u),
                    mem_read32(&cpu, 0x800000E4u));
            gx_finish_interrupt_reports++;
        }
        if (g_runqueue_trace && message_send_reported &&
            gx_finish_reports < 8u &&
            (cpu.pc == 0x80322BE0u ||
             (cpu.pc == 0x80308A9Cu && cpu.lr == 0x80322C38u))) {
            fprintf(stderr,
                    "[gx] GXDrawDone boundary pc=0x%08X r1=0x%08X "
                    "pe_status=0x%04X finish_queue_head=0x%08X "
                    "finish_queue_tail=0x%08X\n",
                    cpu.pc, cpu.gpr[1], mem_read16(&cpu, 0xCC00100Au),
                    mem_read32(&cpu, 0x803F7C9Cu),
                    mem_read32(&cpu, 0x803F7CA0u));
            gx_finish_reports++;
        }
        if (g_runqueue_trace && message_send_reported &&
            gx_path_reports < 24u &&
            (cpu.pc == 0x80322980u || cpu.pc == 0x80323024u ||
             cpu.pc == 0x80322C10u || cpu.pc == 0x80322C20u ||
             cpu.pc == 0x80322C30u || cpu.pc == 0x80322C34u ||
             cpu.pc == 0x80308A9Cu)) {
            fprintf(stderr,
                    "[gx] path pc=0x%08X r1=0x%08X r3=0x%08X r30=0x%08X "
                    "lr=0x%08X pe_status=0x%04X finish_head=0x%08X "
                    "finish_tail=0x%08X\n",
                    cpu.pc, cpu.gpr[1], cpu.gpr[3], cpu.gpr[30], cpu.lr,
                    mem_read16(&cpu, 0xCC00100Au),
                    mem_read32(&cpu, 0x803F7C9Cu),
                    mem_read32(&cpu, 0x803F7CA0u));
            gx_path_reports++;
        }
        if (g_runqueue_trace && message_send_reported &&
            display_continuation_reports < 16u &&
            (cpu.pc == 0x80255C0Cu || cpu.pc == 0x80255C14u ||
             cpu.pc == 0x802563FCu || cpu.pc == 0x80256408u)) {
            fprintf(stderr,
                    "[display] continuation pc=0x%08X r1=0x%08X "
                    "r3=0x%08X r31=0x%08X lr=0x%08X\n",
                    cpu.pc, cpu.gpr[1], cpu.gpr[3], cpu.gpr[31], cpu.lr);
            display_continuation_reports++;
        }
        if (game_loop_reports < 12u &&
            (cpu.pc == 0x80023218u || cpu.pc == 0x800231E4u)) {
            fprintf(stderr,
                    "[game] fap loop pc=0x%08X r1=0x%08X r3=0x%08X "
                    "lr=0x%08X\n",
                    cpu.pc, cpu.gpr[1], cpu.gpr[3], cpu.lr);
            game_loop_reports++;
        }
        if (game_boundary_reports < 24u &&
            (cpu.pc == 0x800231E4u || cpu.pc == 0x80023218u)) {
            fprintf(stderr,
                    "[frame] fap-boundary pc=0x%08X lr=0x%08X "
                    "current_thread=0x%08X current_context=0x%08X "
                    "main_state=%u main_queue=0x%08X run_bits=0x%08X "
                    "video_used=%u retraces=%u\n",
                    cpu.pc, cpu.lr, mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read32(&cpu, 0x803A2960u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x80429EE8u + 0x1Cu),
                    mem_read32(&cpu, 0x803F7B3Cu));
            game_boundary_reports++;
        }
        if (render_begin_reports < 12u && cpu.pc == 0x802558CCu) {
            fprintf(stderr,
                    "[display] beginRender pc=0x%08X r1=0x%08X r3=0x%08X "
                    "lr=0x%08X\n",
                    cpu.pc, cpu.gpr[1], cpu.gpr[3], cpu.lr);
            render_begin_reports++;
        }
        if (scene_manager_reports < 24u &&
            (cpu.pc == 0x80029F10u || cpu.pc == 0x80029F9Cu ||
             cpu.pc == 0x8022D18Cu ||
             cpu.pc == 0x8022E9B4u || cpu.pc == 0x8022D1DCu ||
             cpu.pc == 0x8022D984u || cpu.pc == 0x8004086Cu)) {
            fprintf(stderr,
                    "[scene] manager pc=0x%08X r1=0x%08X r3=0x%08X "
                    "r4=0x%08X r5=0x%08X lr=0x%08X\n",
                    cpu.pc, cpu.gpr[1], cpu.gpr[3], cpu.gpr[4], cpu.gpr[5],
                    cpu.lr);
            scene_manager_reports++;
            if (logo_state_reports < 24u &&
                (cpu.pc == 0x8022D18Cu || cpu.pc == 0x8022D1DCu ||
                 cpu.pc == 0x8022D984u) &&
                cpu.gpr[3] >= 0x80000000u) {
                const u32 logo = cpu.gpr[3];
                fprintf(stderr,
                        "[logo] state pc=0x%08X object=0x%08X action=%u "
                        "inter=%u field1ea=%u field1eb=%u timer=%u "
                        "duration=%u remaining=%u phase=0x%08X "
                        "init=%d pause=0x%02X subtype=0x%08X "
                        "lytag_layer=0x%08X lytag_list=%u lytag_idx=%u "
                        "lytag_prev=0x%08X lytag_next=0x%08X "
                        "layer_node_prev=0x%08X layer_node_next=0x%08X "
                        "layer_id=0x%08X tree_lists=0x%08X tree_count=%u "
                        "pi_use=%d pi_layer=0x%08X pi_list=%u pi_prio=%u "
                        "current_thread=0x%08X run_bits=0x%08X\n",
                        cpu.pc, logo, mem_read8(&cpu, logo + 0x1E8u),
                        mem_read8(&cpu, logo + 0x1E9u),
                        mem_read8(&cpu, logo + 0x1EAu),
                        mem_read8(&cpu, logo + 0x1EBu),
                        mem_read16(&cpu, logo + 0x1ECu),
                        mem_read16(&cpu, logo + 0x1EEu),
                        mem_read16(&cpu, logo + 0x1F0u),
                        mem_read32(&cpu, logo + 0x1C4u),
                        (s8)mem_read8(&cpu, logo + 0x0Cu),
                        mem_read8(&cpu, logo + 0x0Bu),
                        mem_read32(&cpu, logo + 0xB4u),
                        mem_read32(&cpu, logo + 0x2Cu),
                        mem_read16(&cpu, logo + 0x30u),
                        mem_read16(&cpu, logo + 0x32u),
                        mem_read32(&cpu, logo + 0x18u),
                        mem_read32(&cpu, logo + 0x20u),
                        mem_read32(&cpu, logo + 0xBCu),
                        mem_read32(&cpu, logo + 0xC4u),
                        mem_read32(&cpu, logo + 0xC8u),
                        mem_read32(&cpu, logo + 0xCCu),
                        mem_read32(&cpu, logo + 0xD0u),
                        (int)mem_read8(&cpu, logo + 0x78u),
                        mem_read32(&cpu, logo + 0x98u),
                        mem_read16(&cpu, logo + 0x9Cu),
                        mem_read16(&cpu, logo + 0x9Eu),
                        mem_read32(&cpu, 0x800000E4u),
                        mem_read32(&cpu, 0x803F7A30u));
                logo_state_reports++;
            }
        }
        if (draw_return_reports < 40u &&
            (cpu.pc == 0x8022D1D8u || cpu.pc == 0x8003EF20u ||
             cpu.pc == 0x8003EF24u)) {
            fprintf(stderr,
                    "[frame] return pc=0x%08X lr=0x%08X r3=0x%08X "
                    "r4=0x%08X r30=0x%08X current_thread=0x%08X "
                    "current_context=0x%08X main_state=%u main_queue=0x%08X "
                    "run_bits=0x%08X vi_used=%u\n",
                    cpu.pc, cpu.lr, cpu.gpr[3], cpu.gpr[4], cpu.gpr[30],
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read32(&cpu, 0x803A2960u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x80429EE8u + 0x1Cu));
            draw_return_reports++;
        }
        if ((management_reports < (g_room0_trace ? 64u : 24u) ||
             (g_room0_trace && room0_actor_reports > 0u &&
              management_reports < 96u)) &&
            (cpu.pc == 0x8003EC84u || cpu.pc == 0x8003ED8Cu ||
             cpu.pc == 0x8003ED90u || cpu.pc == 0x800231BCu ||
             cpu.pc == 0x80023200u || cpu.pc == 0x80023208u)) {
            fprintf(stderr,
                    "[frame] management pc=0x%08X lr=0x%08X r3=0x%08X "
                    "r4=0x%08X current_thread=0x%08X current_context=0x%08X "
                    "main_state=%u main_queue=0x%08X run_bits=0x%08X\n",
                    cpu.pc, cpu.lr, cpu.gpr[3], cpu.gpr[4],
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read32(&cpu, 0x803A2960u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7A30u));
            management_reports++;
        }
        if ((draw_iter_reports < (g_room0_trace ? 64u : 16u) ||
             (g_room0_trace && room0_actor_reports > 0u &&
              draw_iter_reports < 96u)) &&
            cpu.pc == 0x8003E338u) {
            const u32 root = 0x803F6178u;
            const u32 lists = mem_read32(&cpu, root + 0x00u);
            const u32 list_one_head = mem_read32(&cpu, lists + 0x0Cu);
            const u32 player_list = lists + (0x68u * 0x0Cu);
            const u32 player_head = mem_read32(&cpu, player_list + 0x00u);
            fprintf(stderr,
                    "[frame] draw-iter root=0x%08X lists=0x%08X count=%u "
                    "sizes=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u "
                    "list1_head=0x%08X priority104_head=0x%08X "
                    "priority104_size=%u node_data=0x%08X tag_data=0x%08X "
                    "tag_use=%u\n",
                    root, lists, mem_read32(&cpu, root + 0x04u),
                    mem_read32(&cpu, lists + 0x08u),
                    mem_read32(&cpu, lists + 0x14u),
                    mem_read32(&cpu, lists + 0x20u),
                    mem_read32(&cpu, lists + 0x2Cu),
                    mem_read32(&cpu, lists + 0x38u),
                    mem_read32(&cpu, lists + 0x44u),
                    mem_read32(&cpu, lists + 0x50u),
                    mem_read32(&cpu, lists + 0x5Cu),
                    mem_read32(&cpu, lists + 0x68u),
                    mem_read32(&cpu, lists + 0x74u), list_one_head,
                    player_head, mem_read32(&cpu, player_list + 0x08u),
                    mem_read32(&cpu, 0x80ABE61Cu + 0x04u),
                    mem_read32(&cpu, 0x80ABE61Cu + 0x0Cu),
                    mem_read8(&cpu, 0x80ABE61Cu + 0x10u));
            if (g_room0_trace) {
                fprintf(stderr,
                        "[room0-draw-list] priority=0x68 size=%u head=0x%08X "
                        "head_data=0x%08X head_prev=0x%08X head_next=0x%08X "
                        "player_tag=0x80ABE61C player_prev=0x%08X "
                        "player_next=0x%08X player_use=%u\n",
                        mem_read32(&cpu, player_list + 0x08u), player_head,
                        mem_read32(&cpu, player_head + 0x04u),
                        mem_read32(&cpu, player_head + 0x00u),
                        mem_read32(&cpu, player_head + 0x08u),
                        mem_read32(&cpu, 0x80ABE61Cu),
                        mem_read32(&cpu, 0x80ABE61Cu + 0x08u),
                        mem_read8(&cpu, 0x80ABE61Cu + 0x10u));
            }
            draw_iter_reports++;
        }
        if (logo_callback_reports < 16u &&
            (cpu.pc == 0x8022D1DCu || cpu.pc == 0x8022D18Cu)) {
            fprintf(stderr,
                    "[logo] callback #%u pc=0x%08X object=0x%08X "
                    "action=%u timer=%u phase=0x%08X current_thread=0x%08X "
                    "current_context=0x%08X retraces=%u\n",
                    logo_callback_reports + 1u, cpu.pc, cpu.gpr[3],
                    mem_read8(&cpu, cpu.gpr[3] + 0x1E8u),
                    mem_read16(&cpu, cpu.gpr[3] + 0x1ECu),
                    mem_read32(&cpu, cpu.gpr[3] + 0x1C4u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read32(&cpu, 0x803F7B3Cu));
            logo_callback_reports++;
        }
        if (draw_dispatch_reports < 16u && cpu.pc == 0x8004042Cu &&
            cpu.gpr[3] >= 0x80000000u) {
            const u32 object = cpu.gpr[3];
            fprintf(stderr,
                    "[frame] draw-dispatch #%u object=0x%08X pause=0x%02X "
                    "subtype=0x%08X method=0x%08X layer=0x%08X "
                    "current_thread=0x%08X retraces=%u\n",
                    draw_dispatch_reports + 1u, object,
                    mem_read8(&cpu, object + 0x0Bu),
                    mem_read32(&cpu, object + 0xB4u),
                    mem_read32(&cpu, object + 0xA8u),
                    mem_read32(&cpu, object + 0x2Cu),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x803F7B3Cu));
            draw_dispatch_reports++;
        }
        if (g_room0_trace &&
            room0_scene_draw_reports < 16u && cpu.pc == 0x80234B9Cu) {
            fprintf(stderr,
                    "[room0-scene-draw] scene=0x%08X name=%d subtype=0x%08X "
                    "method=0x%08X retraces=%u blocks=%llu\n",
                    cpu.gpr[3], (s16)mem_read16(&cpu, cpu.gpr[3] + 0x08u),
                    mem_read32(&cpu, cpu.gpr[3] + 0xB4u),
                    mem_read32(&cpu, cpu.gpr[3] + 0xA8u),
                    mem_read32(&cpu, 0x803F7B3Cu), (unsigned long long)blocks);
            room0_scene_draw_reports++;
        }
        if (g_room0_trace && room0_fpc_draw_reports < 16u &&
            cpu.pc == 0x8003E318u && cpu.gpr[3] == 0x80ABE544u) {
            fprintf(stderr,
                    "[room0-fpc-draw] actor=0x%08X name=%d subtype=0x%08X "
                    "method=0x%08X draw_tag=0x%08X retraces=%u blocks=%llu\n",
                    cpu.gpr[3], (s16)mem_read16(&cpu, cpu.gpr[3] + 0x08u),
                    mem_read32(&cpu, cpu.gpr[3] + 0xB4u),
                    mem_read32(&cpu, cpu.gpr[3] + 0xA8u),
                    cpu.gpr[3] + 0xD8u, mem_read32(&cpu, 0x803F7B3Cu),
                    (unsigned long long)blocks);
            room0_fpc_draw_reports++;
        }
        const u32 player_actor = mem_read32(&cpu, 0x803CA74Cu);
        const bool player_draw_entry =
            g_overlap_terminal_phase == 6u &&
            player_actor >= 0x80000000u && cpu.pc == 0x80108204u &&
            cpu.gpr[3] == player_actor;
        if ((g_room0_trace ||
             ((g_player_trace || g_event_confirm_target >= 0) &&
              g_overlap_terminal_phase == 6u)) &&
            room0_player_draw_reports < 16u && player_draw_entry) {
            fprintf(stderr,
                    "[room0-player-draw] actor=0x%08X name=%d subtype=0x%08X "
                    "method=0x%08X retraces=%u blocks=%llu\n",
                    cpu.gpr[3], (s16)mem_read16(&cpu, cpu.gpr[3] + 0x08u),
                    mem_read32(&cpu, cpu.gpr[3] + 0xB4u),
                    mem_read32(&cpu, cpu.gpr[3] + 0xA8u),
                    mem_read32(&cpu, 0x803F7B3Cu), (unsigned long long)blocks);
            room0_player_draw_reports++;
        }
        if ((g_player_trace || g_event_confirm_target >= 0) &&
            cpu.pc == 0x80234FD0u)
            event_scene_execute_entries++;
        if ((g_player_trace || g_event_confirm_target >= 0) &&
            cpu.pc == 0x800529B8u)
            event_execute_wrapper_entries++;
        if ((g_player_trace || g_event_confirm_target >= 0) &&
            cpu.pc == 0x80074324u)
            event_run_proc_entries++;
        if (g_player_trace || g_event_confirm_target >= 0) {
            if (cpu.pc == 0xC0D615C4u)
                aj_event_order_entries++;
            if (cpu.pc == 0x8006FEE8u || cpu.pc == 0xC006FEE8u)
                event_control_order_entries++;
            if (cpu.pc == 0x80070EA8u || cpu.pc == 0xC0070EA8u)
                event_control_check_start_entries++;
            if (cpu.pc == 0x80070870u || cpu.pc == 0xC0070870u)
                event_control_demo_check_entries++;
            if (cpu.pc == 0x800744ACu || cpu.pc == 0xC00744ACu)
                event_manager_order_entries++;
            if (cpu.pc == 0x802350BCu)
                play_scene_delete_entries++;
            if (cpu.pc == 0x80071468u || cpu.pc == 0xC0071468u)
                event_control_remove_entries++;
            if (cpu.pc == 0x800F0CF8u || cpu.pc == 0xC00F0CF8u)
                event_control_reset_entries++;
            if (cpu.pc == 0x80070D1Cu || cpu.pc == 0xC0070D1Cu)
                event_control_end_proc_entries++;
            if (cpu.pc == 0x800709C0u || cpu.pc == 0xC00709C0u)
                event_control_demo_end_entries++;

            const u8 control_mode = mem_read8(&cpu, 0x803C9EA2u);
            const s16 control_event =
                (s16)mem_read16(&cpu, 0x803C9EB8u);
            if ((!event_control_state_seen ||
                 control_mode != event_control_last_mode ||
                 control_event != event_control_last_event) &&
                event_control_state_reports < 32u) {
                fprintf(stderr,
                        "[event-control-state] mode=%u event=%d pc=0x%08X "
                        "lr=0x%08X scene_delete=%llu remove=%llu reset=%llu "
                        "retraces=%llu blocks=%llu\n",
                        control_mode, control_event, cpu.pc, cpu.lr,
                        (unsigned long long)play_scene_delete_entries,
                        (unsigned long long)event_control_remove_entries,
                        (unsigned long long)event_control_reset_entries,
                        (unsigned long long)g_host_retrace_count,
                        (unsigned long long)blocks);
                const u32 event_table = mem_read32(&cpu, 0x803C9ED8u);
                const u32 staff_table = mem_read32(&cpu, 0x803C9EDCu);
                if (control_event >= 0 && event_table >= 0x80000000u) {
                    const u32 event =
                        event_table + (u32)control_event * 0xB0u;
                    char event_name[33];
                    for (u32 i = 0u; i < 32u; ++i) {
                        const u8 ch = mem_read8(&cpu, event + i);
                        event_name[i] = ch >= 0x20u && ch <= 0x7Eu
                                            ? (char)ch
                                            : '\0';
                        if (event_name[i] == '\0') {
                            for (u32 j = i + 1u; j < sizeof(event_name); ++j)
                                event_name[j] = '\0';
                            break;
                        }
                    }
                    event_name[32] = '\0';
                    const u32 staff_count = mem_read32(&cpu, event + 0x7Cu);
                    fprintf(stderr,
                            "[event-control-data] event=%d name=\"%s\" "
                            "state=%d staff_count=%u staff_table=0x%08X",
                            control_event, event_name,
                            (s32)mem_read32(&cpu, event + 0xA4u), staff_count,
                            staff_table);
                    const u32 report_count = staff_count < 20u ? staff_count : 20u;
                    for (u32 i = 0u; i < report_count; ++i)
                        fprintf(stderr, " staff%u=%d", i,
                                (s32)mem_read32(&cpu, event + 0x2Cu + i * 4u));
                    fprintf(stderr, "\n");
                }
                event_control_state_reports++;
            }
            event_control_state_seen = true;
            event_control_last_mode = control_mode;
            event_control_last_event = control_event;
        }
        const bool player_state_ready =
            g_overlap_terminal_phase == 6u && player_actor >= 0x80000000u &&
            mem_read8(&cpu, 0x803C9EA2u) == 0u;
        if (player_state_ready && cpu.pc == 0x80122D30u &&
            cpu.gpr[3] == player_actor && player_stick_configured &&
            !player_stick_start_captured &&
            g_host_retrace_count >= g_player_start_after) {
            bool triggered = true;
            if (g_player_waypoint_configured) {
                g_player_waypoint_active = true;
            } else if (g_player_stick_x_pulse.configured) {
                triggered = bluewake_pad_axis_event_schedule_trigger(
                    &g_player_stick_x_pulse, g_host_retrace_count);
            }
            if (g_player_stick_y_pulse.configured) {
                triggered =
                    bluewake_pad_axis_event_schedule_trigger(
                        &g_player_stick_y_pulse, g_host_retrace_count) &&
                    triggered;
            }
            if (!triggered) {
                stop_reason = "player stick trigger";
                break;
            }
            player_stick_start_captured = true;
            player_stick_start_retrace = g_host_retrace_count;
            player_stick_start_pos_x = mem_read32(&cpu, cpu.gpr[3] + 0x1F8u);
            player_stick_start_pos_y = mem_read32(&cpu, cpu.gpr[3] + 0x1FCu);
            player_stick_start_pos_z = mem_read32(&cpu, cpu.gpr[3] + 0x200u);
            // g_dComIfG_gameInfo.play.mCameraInfo[0].mpCamera; lookat eye and
            // center are camera_class::view offsets 0xD8 and 0xE4.
            player_stick_camera = mem_read32(&cpu, 0x803CA718u);
            if (player_stick_camera >= 0x80000000u) {
                for (u32 axis = 0u; axis < 3u; axis++) {
                    player_stick_start_eye[axis] =
                        mem_read32(&cpu, player_stick_camera + 0xD8u + axis * 4u);
                    player_stick_start_center[axis] =
                        mem_read32(&cpu, player_stick_camera + 0xE4u + axis * 4u);
                }
            }
            fprintf(stderr,
                    "[pad] player-ready stick trigger start=%llu "
                    "length=%llu value=(%d,%d) pos=%08X,%08X,%08X "
                    "camera=0x%08X eye=%08X,%08X,%08X "
                    "center=%08X,%08X,%08X\n",
                    (unsigned long long)player_stick_start_retrace,
                    (unsigned long long)player_stick_length,
                    g_player_stick_x_pulse.configured
                        ? g_player_stick_x_pulse.value
                        : 0,
                    g_player_stick_y_pulse.configured
                        ? g_player_stick_y_pulse.value
                        : 0,
                    player_stick_start_pos_x, player_stick_start_pos_y,
                    player_stick_start_pos_z, player_stick_camera,
                    player_stick_start_eye[0], player_stick_start_eye[1],
                    player_stick_start_eye[2], player_stick_start_center[0],
                    player_stick_start_center[1], player_stick_start_center[2]);
            if (g_player_waypoint_configured)
                fprintf(stderr,
                        "[pad] player waypoint active target=(%.3f,%.3f)\n",
                        g_player_waypoint_x, g_player_waypoint_z);
        }
        // The route steers at every player update, so the per-block service
        // stays on until it has finished (see g_player_route_waiting).
        if (g_player_route_waiting && player_stick_start_captured &&
            !g_player_waypoint_active) {
            g_player_route_waiting = false;
        }
        if (g_player_waypoint_active && player_stick_start_captured &&
            cpu.pc == 0x80122D30u &&
            cpu.gpr[3] == mem_read32(&cpu, 0x803CA74Cu)) {
            const u32 player = cpu.gpr[3];
            if (g_host_retrace_count >=
                player_stick_start_retrace + player_stick_length) {
                g_player_waypoint_active = false;
                g_player_waypoint_stick_x = 0;
                g_player_waypoint_stick_y = 0;
            } else if (player_stick_camera >= 0x80000000u) {
                BluewakePadWaypointOutput steer;
                const u32 player_proc =
                    mem_read32(&cpu, player + 0x31D8u);
                const bool ladder_proc =
                    player_proc >= 0x38u && player_proc <= 0x3Cu;
                bool steered = false;
                if (g_player_ladder_down_configured && ladder_proc) {
                    if (!g_player_ladder_entered) {
                        g_player_ladder_entered = true;
                        fprintf(stderr,
                                "[pad] ladder-down phase entered "
                                "retrace=%llu proc=%u y=%08X\n",
                                (unsigned long long)g_host_retrace_count,
                                player_proc,
                                mem_read32(&cpu, player + 0x1FCu));
                    }
                    if (player_proc == 0x3Cu)
                        g_player_ladder_move_seen = true;
                    const s16 shape_y =
                        (s16)mem_read16(&cpu, player + 0x20Eu);
                    const float world_angle =
                        (float)shape_y * 3.14159265358979323846f / 32768.0f +
                        3.14159265358979323846f;
                    steered = bluewake_pad_world_angle_steer(
                        world_angle,
                        host_f32(mem_read32(
                            &cpu, player_stick_camera + 0xD8u)),
                        host_f32(mem_read32(
                            &cpu, player_stick_camera + 0xE0u)),
                        host_f32(mem_read32(
                            &cpu, player_stick_camera + 0xE4u)),
                        host_f32(mem_read32(
                            &cpu, player_stick_camera + 0xECu)),
                        100, &steer);
                } else if (g_player_ladder_down_configured &&
                           g_player_ladder_move_seen &&
                           !g_player_post_ladder_route_started) {
                    g_player_post_ladder_route_started = true;
                    g_player_post_ladder_route_active =
                        g_player_post_ladder_route_configured;
                    if (!g_player_post_ladder_route_active) {
                        g_player_waypoint_active = false;
                        g_player_waypoint_stick_x = 0;
                        g_player_waypoint_stick_y = 0;
                    }
                    fprintf(stderr,
                            "[pad] ladder-down phase exited retrace=%llu "
                            "proc=%u pos=%08X,%08X,%08X route=%u\n",
                            (unsigned long long)g_host_retrace_count,
                            player_proc,
                            mem_read32(&cpu, player + 0x1F8u),
                            mem_read32(&cpu, player + 0x1FCu),
                            mem_read32(&cpu, player + 0x200u),
                            g_player_post_ladder_route_active ? 1u : 0u);
                } else if (g_player_post_ladder_route_active) {
                    const float player_x =
                        host_f32(mem_read32(&cpu, player + 0x1F8u));
                    const float player_z =
                        host_f32(mem_read32(&cpu, player + 0x200u));
                    const size_t old_index =
                        g_player_post_ladder_route.index;
                    if (!bluewake_pad_route_update(
                            &g_player_post_ladder_route, player_x, player_z,
                            &g_player_waypoint_x, &g_player_waypoint_z)) {
                        g_player_post_ladder_route_active = false;
                        g_player_post_ladder_route_complete = true;
                        g_player_post_ladder_route_arrival_retrace =
                            g_host_retrace_count;
                        g_player_waypoint_active = false;
                        g_player_waypoint_stick_x = 0;
                        g_player_waypoint_stick_y = 0;
                        fprintf(stderr,
                                "[pad-route] complete retrace=%llu "
                                "pos=%08X,%08X,%08X\n",
                                (unsigned long long)g_host_retrace_count,
                                mem_read32(&cpu, player + 0x1F8u),
                                mem_read32(&cpu, player + 0x1FCu),
                                mem_read32(&cpu, player + 0x200u));
                    } else {
                        if (old_index != g_player_post_ladder_route.index)
                            fprintf(stderr,
                                    "[pad-route] waypoint=%zu/%zu "
                                    "retrace=%llu target=(%.3f,%.3f)\n",
                                    g_player_post_ladder_route.index + 1u,
                                    g_player_post_ladder_route.count,
                                    (unsigned long long)g_host_retrace_count,
                                    g_player_waypoint_x,
                                    g_player_waypoint_z);
                        steered = bluewake_pad_waypoint_steer(
                            player_x, player_z,
                            host_f32(mem_read32(
                                &cpu, player_stick_camera + 0xD8u)),
                            host_f32(mem_read32(
                                &cpu, player_stick_camera + 0xE0u)),
                            host_f32(mem_read32(
                                &cpu, player_stick_camera + 0xE4u)),
                            host_f32(mem_read32(
                                &cpu, player_stick_camera + 0xECu)),
                            g_player_waypoint_x, g_player_waypoint_z, 100,
                            &steer);
                    }
                } else {
                    steered = bluewake_pad_waypoint_steer(
                        host_f32(mem_read32(&cpu, player + 0x1F8u)),
                        host_f32(mem_read32(&cpu, player + 0x200u)),
                        host_f32(mem_read32(&cpu, player_stick_camera + 0xD8u)),
                        host_f32(mem_read32(&cpu, player_stick_camera + 0xE0u)),
                        host_f32(mem_read32(&cpu, player_stick_camera + 0xE4u)),
                        host_f32(mem_read32(&cpu, player_stick_camera + 0xECu)),
                        g_player_waypoint_x, g_player_waypoint_z, 100,
                        &steer);
                }
                if (steered) {
                    g_player_waypoint_stick_x = steer.stick_x;
                    g_player_waypoint_stick_y = steer.stick_y;
                    if ((g_host_retrace_count - player_stick_start_retrace) %
                            120u ==
                        0u)
                        fprintf(stderr,
                                "[pad-waypoint] retrace=%llu stick=(%d,%d) "
                                "distance=%.3f proc=%u\n",
                                (unsigned long long)g_host_retrace_count,
                                g_player_waypoint_stick_x,
                                g_player_waypoint_stick_y, steer.distance,
                                player_proc);
                }
            }
        }
        if (cpu.pc == 0x80328F84u && cpu.lr == 0x80246A04u)
            host_observe_ground_cross_return(&cpu, blocks);
        if (g_ground_cross_stop_reason != NULL && stop_reason == NULL)
            stop_reason = g_ground_cross_stop_reason;
        if (cpu.pc == 0x80120188u) {
            const u32 player = cpu.gpr[29];
            const u32 check = player + 2284u;
            const u32 poly_info = player + 2304u;
            const u32 ground_bits = mem_read32(&cpu, check + 52u);
            const u32 return_bits = host_f32_bits(cpu.fpr[1]);
            const u16 poly_index = mem_read16(&cpu, poly_info);
            const u16 bg_index = mem_read16(&cpu, poly_info + 2u);
            const u32 constant_word = mem_read32(&cpu, cpu.gpr[2] - 23564u);
            lava_constant_checks++;
            const bool fp_materialized =
                bluewake_fpu_registers_materialized(&cpu);
            if (!fp_materialized) {
                lava_constant_deferred_checks++;
            } else if (return_bits != ground_bits) {
                fprintf(stderr,
                        "[cpu-abi] ground-cross-f1-mismatch "
                        "boundary=caller-entry retrace=%llu blocks=%llu "
                        "checks=%llu player=0x%08X check=0x%08X "
                        "f1=%08X ground=%08X poly=%u bg=%u lr=0x%08X\n",
                        (unsigned long long)g_host_retrace_count,
                        (unsigned long long)blocks,
                        (unsigned long long)lava_constant_checks, player,
                        check, return_bits, ground_bits, poly_index, bg_index,
                        cpu.lr);
                stop_reason = "GroundCross f1 ABI invariant";
            }
            if (cpu.gpr[2] != 0x803FFD00u ||
                constant_word != 0xCE6E6B28u) {
                fprintf(stderr,
                        "[cpu-abi] lava-constant-drift retrace=%llu "
                        "blocks=%llu checks=%llu player=0x%08X "
                        "r2=0x%08X address=0x%08X word=%08X "
                        "returned_ground=%08X poly=%u bg=%u lr=0x%08X\n",
                        (unsigned long long)g_host_retrace_count,
                        (unsigned long long)blocks,
                        (unsigned long long)lava_constant_checks, player,
                        cpu.gpr[2], cpu.gpr[2] - 23564u, constant_word,
                        mem_read32(&cpu, check + 52u), poly_index, bg_index,
                        cpu.lr);
                stop_reason = "lava constant invariant";
            }
            if ((!lava_ground_provenance_seen ||
                 bg_index != lava_ground_last_bg_index ||
                 poly_index != lava_ground_last_poly_index) &&
                lava_ground_provenance_reports < 64u) {
                fprintf(stderr,
                        "[collision-provenance] producer retrace=%llu "
                        "blocks=%llu player=0x%08X check=0x%08X "
                        "ground=%.9g poly=%u bg=%u pos=%08X,%08X,%08X "
                        "check_words=%08X,%08X,%08X,%08X\n",
                        (unsigned long long)g_host_retrace_count,
                        (unsigned long long)blocks, player, check,
                        cpu.fpr[1], poly_index, bg_index,
                        mem_read32(&cpu, player + 0x1F8u),
                        mem_read32(&cpu, player + 0x1FCu),
                        mem_read32(&cpu, player + 0x200u),
                        mem_read32(&cpu, check), mem_read32(&cpu, check + 4u),
                        mem_read32(&cpu, check + 8u),
                        mem_read32(&cpu, check + 12u));
                lava_ground_provenance_reports++;
            }
            lava_ground_provenance_seen = true;
            lava_ground_last_bg_index = bg_index;
            lava_ground_last_poly_index = poly_index;
        }
        if (cpu.pc == 0x800A0B60u && cpu.lr == 0x801201C0u) {
            const u32 player = cpu.gpr[29];
            const u32 check = player + 2284u;
            const u32 poly_info = cpu.gpr[4];
            const u16 poly_index = mem_read16(&cpu, poly_info);
            const u16 bg_index = mem_read16(&cpu, poly_info + 2u);
            if (bg_index >= 256u) {
                fprintf(stderr,
                        "[collision-provenance] invalid-consume retrace=%llu "
                        "blocks=%llu player=0x%08X check=0x%08X "
                        "returned_ground=%08X stored_ground=%08X "
                        "acch_ground=%08X player_y=%08X r2=%08X "
                        "constant_word=%08X cr=%08X "
                        "poly=%u bg=%u "
                        "pos=%08X,%08X,%08X old_pos=0x%08X bg_sys=0x%08X "
                        "check_words=%08X,%08X,%08X,%08X\n",
                        (unsigned long long)g_host_retrace_count,
                        (unsigned long long)blocks, player, check,
                        mem_read32(&cpu, check + 52u),
                        mem_read32(&cpu, player + 13780u),
                        mem_read32(&cpu, player + 1280u),
                        mem_read32(&cpu, player + 508u), cpu.gpr[2],
                        mem_read32(&cpu, cpu.gpr[2] - 23564u), cpu.cr, poly_index,
                        bg_index, mem_read32(&cpu, player + 0x1F8u),
                        mem_read32(&cpu, player + 0x1FCu),
                        mem_read32(&cpu, player + 0x200u), cpu.gpr[30],
                        cpu.gpr[31], mem_read32(&cpu, check),
                        mem_read32(&cpu, check + 4u),
                        mem_read32(&cpu, check + 8u),
                        mem_read32(&cpu, check + 12u));
                stop_reason = "collision provenance";
            }
        }
        if (g_player_route_confirm_configured &&
            g_player_post_ladder_route_complete &&
            !g_player_route_confirm_triggered &&
            g_host_retrace_count >=
                g_player_post_ladder_route_arrival_retrace + 15u &&
            host_guest_cpad_a_released(&cpu) &&
            bluewake_pad_event_schedule_trigger(
                &g_player_route_confirm_pulse, g_host_retrace_count)) {
            g_player_route_confirm_triggered = true;
            fprintf(stderr,
                    "[pad-route] door confirm retrace=%llu length=2 "
                    "buttons=0x0100\n",
                    (unsigned long long)g_host_retrace_count);
        }
        if (g_event_confirm_target >= 0) {
            const u8 message_status = mem_read8(&cpu, 0x803CA7D2u);
            const s32 active_event = (s16)mem_read16(&cpu, 0x803C9EB8u);
            const bool event_ok =
                g_event_confirm_any || active_event == g_event_confirm_target;
            const bool prompt_active =
                event_ok &&
                (message_status == 7u || message_status == 10u ||
                 message_status == 16u);
            if (g_event_prompt_trace) {
                // Transitions only. The values are read every instruction while
                // a confirm is armed, so this reports what the scene asked for
                // without needing a developer-tracing build to find it.
                static s32 traced_event = -12345;
                static u8 traced_status = 0xFFu;
                static unsigned traced_lines;
                if ((active_event != traced_event ||
                     message_status != traced_status) &&
                    traced_lines < 512u) {
                    traced_event = active_event;
                    traced_status = message_status;
                    traced_lines++;
                    fprintf(stderr,
                            "[event-prompt] retrace=%llu event=%d status=%u "
                            "pc=0x%08X\n",
                            (unsigned long long)g_host_retrace_count,
                            active_event, message_status, cpu.pc);
                }
            }
            if (!prompt_active && g_event_confirm_prompt_active) {
                bluewake_pad_event_schedule_rearm(&g_event_confirm_pulse);
                g_event_confirm_prompt_active = false;
            }
            if (prompt_active && !g_event_confirm_prompt_active &&
                host_guest_cpad_a_released(&cpu) &&
                bluewake_pad_event_schedule_trigger(
                    &g_event_confirm_pulse, g_host_retrace_count)) {
                g_event_confirm_prompt_active = true;
                fprintf(stderr,
                        "[pad] event-confirm trigger event=%d status=%u "
                        "start=%llu length=%llu buttons=0x%04X\n",
                        active_event, message_status,
                        (unsigned long long)g_event_confirm_pulse.start_retrace,
                        (unsigned long long)g_event_confirm_pulse.length,
                        g_event_confirm_pulse.buttons);
            }
            if (event_ok && cpu.pc == 0x801E6E44u &&
                cpu.gpr[3] >= 0x80000000u) {
                const u8 demo_message_state =
                    mem_read8(&cpu, cpu.gpr[3] + 0x164u);
                const bool demo_prompt_active =
                    demo_message_state == 5u || demo_message_state == 10u;
                if (!demo_prompt_active &&
                    g_event_confirm_demo_prompt_active) {
                    bluewake_pad_event_schedule_rearm(
                        &g_event_confirm_pulse);
                    g_event_confirm_demo_prompt_active = false;
                }
                if (demo_prompt_active &&
                    !g_event_confirm_demo_prompt_active &&
                    host_guest_cpad_a_released(&cpu) &&
                    bluewake_pad_event_schedule_trigger(
                        &g_event_confirm_pulse, g_host_retrace_count)) {
                    g_event_confirm_demo_prompt_active = true;
                    fprintf(stderr,
                            "[pad] event-confirm trigger event=%d "
                            "demo_message_state=%u start=%llu length=%llu "
                            "buttons=0x%04X\n",
                            active_event, demo_message_state,
                            (unsigned long long)
                                g_event_confirm_pulse.start_retrace,
                            (unsigned long long)
                                g_event_confirm_pulse.length,
                            g_event_confirm_pulse.buttons);
                }
            }
        }
        if ((g_room0_trace || capture_player_ready ||
             ((g_player_trace || g_event_confirm_target >= 0) &&
              g_overlap_terminal_phase == 6u)) &&
            cpu.pc == 0x80122D30u &&
            cpu.gpr[3] == mem_read32(&cpu, 0x803CA74Cu)) {
            const u32 player = cpu.gpr[3];
            const u8 event_mode = mem_read8(&cpu, 0x803C9EA2u);
            const u16 demo_type = mem_read16(&cpu, player + 0x304u);
            const u32 demo_mode = mem_read32(&cpu, player + 0x314u);
            // Control, not the cutscene. This path used to fire on
            // demo_type == 1 && demo_mode == 4, which is the authored awake
            // cutscene with its subtitle actor live - v24's log at retrace
            // 19,715 reads proc 4, event_mode 2, with a key being eaten there.
            // It fired on that, won the race against the player-scene-state
            // path, and so the "picture of Outset under control" this feature
            // exists to produce was in fact a cutscene page with a subtitle
            // box. It now requires the same tuple the control gate requires -
            // event_mode 0 with demo playback off, at a completed scene
            // handover - so the two capture paths agree by construction
            // instead of by luck.
            if (capture_player_ready && !player_ready_capture_scheduled &&
                g_play_scene_reported && event_mode == 0u &&
                demo_type == 0u && demo_mode == 0u &&
                g_overlap_terminal_phase == 6u) {
                diagnostic_capture_retrace = g_host_retrace_count + 2u;
                player_ready_capture_scheduled = true;
                player_ready_capture_followups = 1u;
                fprintf(stderr,
                        "[frame-capture] awake-action scheduled retrace=%llu "
                        "capture_retrace=%llu path=%s "
                        "source=overlap-handler\n",
                        (unsigned long long)g_host_retrace_count,
                        (unsigned long long)diagnostic_capture_retrace,
                        opening_capture_path);
            }
            const s32 staff_idx = (s32)mem_read32(&cpu, player + 0x358Cu);
            const u32 staff_table = mem_read32(&cpu, 0x803C9EDCu);
            const u32 staff = staff_idx >= 0 && staff_table >= 0x80000000u
                                  ? staff_table + (u32)staff_idx * 0x50u
                                  : 0u;
            const s32 staff_cut =
                staff != 0u ? (s32)mem_read32(&cpu, staff + 0x38u) : -1;
            const u32 staff_action =
                staff != 0u ? mem_read32(&cpu, staff + 0x3Cu) : UINT32_MAX;
            const u32 pos_x = mem_read32(&cpu, player + 0x1F8u);
            const u32 pos_y = mem_read32(&cpu, player + 0x1FCu);
            const u32 pos_z = mem_read32(&cpu, player + 0x200u);
            if (stop_after_player_stick && player_stick_start_captured &&
                g_host_retrace_count >=
                    player_stick_start_retrace + player_stick_length + 60u) {
                const bool moved = pos_x != player_stick_start_pos_x ||
                                   pos_y != player_stick_start_pos_y ||
                                   pos_z != player_stick_start_pos_z;
                u32 final_eye[3] = {0u, 0u, 0u};
                u32 final_center[3] = {0u, 0u, 0u};
                if (player_stick_camera >= 0x80000000u) {
                    for (u32 axis = 0u; axis < 3u; axis++) {
                        final_eye[axis] = mem_read32(
                            &cpu, player_stick_camera + 0xD8u + axis * 4u);
                        final_center[axis] = mem_read32(
                            &cpu, player_stick_camera + 0xE4u + axis * 4u);
                    }
                }
                const bool camera_changed =
                    player_stick_camera >= 0x80000000u &&
                    (final_eye[0] != player_stick_start_eye[0] ||
                     final_eye[1] != player_stick_start_eye[1] ||
                     final_eye[2] != player_stick_start_eye[2] ||
                     final_center[0] != player_stick_start_center[0] ||
                     final_center[1] != player_stick_start_center[1] ||
                     final_center[2] != player_stick_start_center[2]);
                fprintf(stderr,
                        "[player-control-admission] moved=%u "
                        "start=%08X,%08X,%08X final=%08X,%08X,%08X "
                        "trigger_retrace=%llu final_retrace=%llu blocks=%llu\n",
                        moved ? 1u : 0u, player_stick_start_pos_x,
                        player_stick_start_pos_y, player_stick_start_pos_z,
                        pos_x, pos_y, pos_z,
                        (unsigned long long)player_stick_start_retrace,
                        (unsigned long long)g_host_retrace_count,
                        (unsigned long long)blocks);
                fprintf(stderr,
                        "[camera-response-admission] changed=%u camera=0x%08X "
                        "start_eye=%08X,%08X,%08X final_eye=%08X,%08X,%08X "
                        "start_center=%08X,%08X,%08X "
                        "final_center=%08X,%08X,%08X\n",
                        camera_changed ? 1u : 0u, player_stick_camera,
                        player_stick_start_eye[0], player_stick_start_eye[1],
                        player_stick_start_eye[2], final_eye[0], final_eye[1],
                        final_eye[2], player_stick_start_center[0],
                        player_stick_start_center[1],
                        player_stick_start_center[2], final_center[0],
                        final_center[1], final_center[2]);
                if (!moved || !camera_changed)
                    stop_reason = "player/camera admission";
                break;
            }
            const bool state_changed =
                !player_trace_state_seen || event_mode != player_trace_event_mode ||
                demo_type != player_trace_demo_type ||
                demo_mode != player_trace_demo_mode ||
                staff_idx != player_trace_staff_idx ||
                staff_cut != player_trace_staff_cut ||
                staff_action != player_trace_staff_action ||
                pos_x != player_trace_pos_x ||
                pos_y != player_trace_pos_y || pos_z != player_trace_pos_z;
            const bool periodic =
                g_host_retrace_count >= player_trace_last_retrace + 60u;
            const bool should_report =
                (g_room0_trace && room0_player_execute_reports < 32u) ||
                ((g_player_trace || g_event_confirm_target >= 0) &&
                 room0_player_execute_reports < 128u &&
                 (state_changed || periodic));
            if (should_report) {
                const u32 event_table = mem_read32(&cpu, 0x803C9ED8u);
                const s32 event_state =
                    event_table >= 0x80000000u
                        ? (s32)mem_read32(
                              &cpu, event_table + 29u * 0xB0u + 0xA4u)
                        : -1;
                fprintf(stderr,
                    "[room0-player-execute] actor=0x%08X proc=%u "
                    "pos=%08X,%08X,%08X speed=%08X,%08X,%08X "
                    "stick=%08X,%08X stick_value=%08X proc_stick=%08X "
                    "proc_angle=0x%04X normal_speed=%08X buttons=0x%04X trig=0x%04X "
                    "event_mode=%u demo_type=%u demo_mode=%u "
                    "staff=%d event=%d cut=%d action=%u timer=%d "
                    "advance=%u has_action=%u "
                    "gamepad_ptr=0x%08X pad_status=%08X,%08X,%08X "
                    "pad_reads=%llu retraces=%u blocks=%llu\n",
                    player, mem_read16(&cpu, player + 0x08u),
                    pos_x, pos_y, pos_z,
                    mem_read32(&cpu, player + 0x220u),
                    mem_read32(&cpu, player + 0x224u),
                    mem_read32(&cpu, player + 0x228u),
                    mem_read32(&cpu, 0x803A4DF0u),
                    mem_read32(&cpu, 0x803A4DF4u),
                    mem_read32(&cpu, 0x803A4DF8u),
                    mem_read32(&cpu, player + 0x35B0u),
                    mem_read16(&cpu, player + 0x34E8u),
                    mem_read32(&cpu, player + 0x35BCu),
                    mem_read16(&cpu, 0x803A4E20u),
                    mem_read16(&cpu, 0x803A4E22u),
                    event_mode, demo_type, demo_mode, staff_idx,
                    (s32)mem_read32(&cpu, player + 0x3590u), staff_cut,
                    staff_action,
                    staff != 0u ? (s16)mem_read16(&cpu, staff + 0x42u) : -1,
                    staff != 0u ? mem_read8(&cpu, staff + 0x46u) : 0u,
                    staff != 0u ? mem_read8(&cpu, staff + 0x47u) : 0u,
                    mem_read32(&cpu, 0x803A4DE0u),
                    mem_read16(&cpu, 0x803ED818u),
                    mem_read8(&cpu, 0x803ED81Au),
                    mem_read8(&cpu, 0x803ED81Bu),
                    (unsigned long long)backend.pad_read_count,
                    mem_read32(&cpu, 0x803F7B3Cu),
                    (unsigned long long)blocks);
                fprintf(stderr,
                        "[event-manager] scene_execute=%llu wrapper=%llu "
                        "run_proc=%llu aj_order=%llu control_order=%llu "
                        "check_start=%llu demo_check=%llu manager_order=%llu "
                        "scene_delete=%llu remove=%llu reset=%llu "
                        "end_proc=%llu demo_end=%llu "
                        "queue=%u first=%d mode=%u control_event=%d menu=%u "
                        "event_table=0x%08X event=29/state=%d "
                        "retraces=%llu blocks=%llu\n",
                        (unsigned long long)event_scene_execute_entries,
                        (unsigned long long)event_execute_wrapper_entries,
                        (unsigned long long)event_run_proc_entries,
                        (unsigned long long)aj_event_order_entries,
                        (unsigned long long)event_control_order_entries,
                        (unsigned long long)event_control_check_start_entries,
                        (unsigned long long)event_control_demo_check_entries,
                        (unsigned long long)event_manager_order_entries,
                        (unsigned long long)play_scene_delete_entries,
                        (unsigned long long)event_control_remove_entries,
                        (unsigned long long)event_control_reset_entries,
                        (unsigned long long)event_control_end_proc_entries,
                        (unsigned long long)event_control_demo_end_entries,
                        mem_read8(&cpu, 0x803C9EA0u),
                        (s8)mem_read8(&cpu, 0x803C9EA1u),
                        mem_read8(&cpu, 0x803C9EA2u),
                        (s16)mem_read16(&cpu, 0x803C9EB8u),
                        mem_read8(&cpu, 0x803F7097u), event_table, event_state,
                        (unsigned long long)g_host_retrace_count,
                        (unsigned long long)blocks);
                if (g_event_confirm_target == 29 &&
                    staff_table >= 0x80000000u) {
                    fprintf(stderr, "[event-staff] event=29");
                    for (u32 staff_i = 97u; staff_i <= 100u; ++staff_i) {
                        const u32 event_staff = staff_table + staff_i * 0x50u;
                        fprintf(stderr,
                                " idx=%u/cut=%d/action=%u/timer=%d/advance=%u/has=%u",
                                staff_i,
                                (s32)mem_read32(&cpu, event_staff + 0x38u),
                                mem_read32(&cpu, event_staff + 0x3Cu),
                                (s16)mem_read16(&cpu, event_staff + 0x42u),
                                mem_read8(&cpu, event_staff + 0x46u),
                                mem_read8(&cpu, event_staff + 0x47u));
                    }
                    fprintf(stderr,
                            " retraces=%llu blocks=%llu\n",
                            (unsigned long long)g_host_retrace_count,
                            (unsigned long long)blocks);
                }
                if (staff_table >= 0x80000000u &&
                    (s16)mem_read16(&cpu, 0x803C9EB8u) == 38) {
                    const u32 demo = mem_read32(&cpu, 0x803CA6D0u);
                    fprintf(stderr,
                            "[event-awake] demo=0x%08X frame=%d no_msg=%u "
                            "mode=%d message_status=%u",
                            demo, demo >= 0x80000000u
                                      ? (s32)mem_read32(&cpu, demo + 0xD4u)
                                      : -1,
                            demo >= 0x80000000u
                                ? mem_read32(&cpu, demo + 0xD8u)
                                : 0u,
                            demo >= 0x80000000u
                                ? (s32)mem_read32(&cpu, demo + 0xDCu)
                                : -1,
                            mem_read8(&cpu, 0x803CA7D2u));
                    for (u32 staff_i = 135u; staff_i <= 137u; ++staff_i) {
                        const u32 event_staff = staff_table + staff_i * 0x50u;
                        char staff_name[33];
                        for (u32 i = 0u; i < 32u; ++i) {
                            const u8 ch = mem_read8(&cpu, event_staff + i);
                            staff_name[i] = ch >= 0x20u && ch <= 0x7Eu
                                                ? (char)ch
                                                : '\0';
                            if (staff_name[i] == '\0') {
                                for (u32 j = i + 1u; j < sizeof(staff_name); ++j)
                                    staff_name[j] = '\0';
                                break;
                            }
                        }
                        staff_name[32] = '\0';
                        fprintf(stderr,
                                " idx=%u/name=\"%s\"/cut=%d/action=%u/"
                                "timer=%d/advance=%u/has=%u",
                                staff_i, staff_name,
                                (s32)mem_read32(&cpu, event_staff + 0x38u),
                                mem_read32(&cpu, event_staff + 0x3Cu),
                                (s16)mem_read16(&cpu, event_staff + 0x42u),
                                mem_read8(&cpu, event_staff + 0x46u),
                                mem_read8(&cpu, event_staff + 0x47u));
                    }
                    fprintf(stderr,
                            " retraces=%llu blocks=%llu\n",
                            (unsigned long long)g_host_retrace_count,
                            (unsigned long long)blocks);
                }
                player_trace_state_seen = true;
                player_trace_event_mode = event_mode;
                player_trace_demo_type = demo_type;
                player_trace_demo_mode = demo_mode;
                player_trace_staff_idx = staff_idx;
                player_trace_staff_cut = staff_cut;
                player_trace_staff_action = staff_action;
                player_trace_pos_x = pos_x;
                player_trace_pos_y = pos_y;
                player_trace_pos_z = pos_z;
                player_trace_last_retrace = g_host_retrace_count;
                room0_player_execute_reports++;
            }
        }
        if (g_room0_trace &&
            room0_draw_queue_reports < 16u &&
            cpu.pc == 0x8003C6ECu && cpu.gpr[3] == 0x80ABE61Cu) {
            fprintf(stderr,
                    "[room0-draw-queue] tag=0x%08X priority=%d use=%u "
                    "prev=0x%08X node_data=0x%08X tag_data=0x%08X next=0x%08X "
                    "retraces=%u blocks=%llu\n",
                    cpu.gpr[3], (s16)cpu.gpr[4],
                    mem_read8(&cpu, cpu.gpr[3] + 0x10u),
                    mem_read32(&cpu, cpu.gpr[3] + 0x00u),
                    mem_read32(&cpu, cpu.gpr[3] + 0x04u),
                    mem_read32(&cpu, cpu.gpr[3] + 0x0Cu),
                    mem_read32(&cpu, cpu.gpr[3] + 0x08u),
                    mem_read32(&cpu, 0x803F7B3Cu),
                    (unsigned long long)blocks);
            room0_draw_queue_reports++;
        }
        if (g_room0_trace &&
            room0_draw_tree_reports < 32u &&
            cpu.gpr[5] == 0x80ABE61Cu &&
            (cpu.pc == 0x8024545Cu || cpu.pc == 0x802454A8u ||
             cpu.pc == 0x80244FF0u || cpu.pc == 0x80245030u ||
             cpu.pc == 0x80244A8Cu || cpu.pc == 0x80244AF8u)) {
            fprintf(stderr,
                    "[room0-draw-tree] pc=0x%08X r3=0x%08X r4=%d r5=0x%08X "
                    "r6=0x%08X tree_lists=0x%08X tree_count=%u "
                    "tag_use=%u tag_prev=0x%08X node_data=0x%08X "
                    "tag_next=0x%08X retraces=%u blocks=%llu\n",
                    cpu.pc, cpu.gpr[3], (s16)cpu.gpr[4], cpu.gpr[5],
                    cpu.gpr[6], mem_read32(&cpu, 0x803F6178u),
                    mem_read32(&cpu, 0x803F6178u + 0x04u),
                    mem_read8(&cpu, 0x80ABE61Cu + 0x10u),
                    mem_read32(&cpu, 0x80ABE61Cu + 0x00u),
                    mem_read32(&cpu, 0x80ABE61Cu + 0x04u),
                    mem_read32(&cpu, 0x80ABE61Cu + 0x08u),
                    mem_read32(&cpu, 0x803F7B3Cu),
                    (unsigned long long)blocks);
            room0_draw_tree_reports++;
        }
        if (draw_tag_reports < 16u && cpu.pc == 0x8024560Cu) {
            fprintf(stderr,
                    "[frame] draw-tag #%u tag=0x%08X data=0x%08X "
                    "method_filter=0x%08X method=0x%08X retraces=%u\n",
                    draw_tag_reports + 1u, cpu.gpr[3],
                    mem_read32(&cpu, cpu.gpr[3] + 0x0Cu), cpu.gpr[4],
                    mem_read32(&cpu, cpu.gpr[4]),
                    mem_read32(&cpu, 0x803F7B3Cu));
            draw_tag_reports++;
        }
        if (scene_request_reports < 32u &&
            (cpu.pc == 0x8002A204u || cpu.pc == 0x8002A2ECu ||
             cpu.pc == 0x8003F5D4u || cpu.pc == 0x8003FAC0u ||
             cpu.pc == 0x8003F380u || cpu.pc == 0x8003F328u)) {
            fprintf(stderr,
                    "[scene] request pc=0x%08X r1=0x%08X r3=0x%08X "
                    "r4=0x%08X r5=0x%08X r6=0x%08X lr=0x%08X\n",
                    cpu.pc, cpu.gpr[1], cpu.gpr[3], cpu.gpr[4], cpu.gpr[5],
                    cpu.gpr[6], cpu.lr);
            scene_request_reports++;
        }
        if (g_title_profile_watch && g_title_profile_watch_reports < 64u &&
            (cpu.pc == 0x8004069Cu || cpu.pc == 0x800406C8u)) {
            const u32 request = cpu.pc == 0x8004069Cu ? cpu.gpr[3] : cpu.gpr[31];
            const u16 proc_name = mem_read16(&cpu, request + 0x50u);
            if (proc_name == 449u) {
                const u32 profile_list = mem_read32(&cpu, 0x803F6A68u);
                const u32 profile = mem_read32(
                    &cpu, profile_list + (u32)proc_name * sizeof(u32));
                fprintf(stderr,
                        "[title-profile] pc=0x%08X request=0x%08X "
                        "result=0x%08X list=0x%08X profile=0x%08X "
                        "size=0x%08X other=0x%08X pc_method=0x%08X "
                        "actor_method=0x%08X\n",
                        cpu.pc, request, cpu.gpr[3], profile_list, profile,
                        mem_read32(&cpu, profile + 0x10u),
                        mem_read32(&cpu, profile + 0x14u),
                        mem_read32(&cpu, profile + 0x0Cu),
                        mem_read32(&cpu, profile + 0x24u));
                g_title_profile_watch_reports++;
            }
        }
        if (!g_title_ready_reported &&
            host_canonical_linked_pc(cpu.pc) == 0x81E01B88u &&
            cpu.gpr[3] >= 0x80000000u) {
            g_title_ready_reported = true;
            g_title_ready_retrace = g_host_retrace_count;
            fprintf(stderr,
                    "[boot-milestone] title-ready retrace=%llu "
                    "retail_retrace=%u object=0x%08X blocks=%llu\n",
                    (unsigned long long)g_title_ready_retrace,
                    mem_read32(&cpu, 0x803F7B3Cu), cpu.gpr[3],
                    (unsigned long long)blocks);
            if (bluewake_pad_event_schedule_trigger(
                    &g_title_pad_pulse, g_host_retrace_count)) {
                fprintf(stderr,
                        "[pad] title-ready pulse trigger start=%llu length=%llu "
                        "buttons=0x%04X\n",
                        (unsigned long long)g_title_pad_pulse.start_retrace,
                        (unsigned long long)g_title_pad_pulse.length,
                        g_title_pad_pulse.buttons);
            }
        }
        if (!g_file_select_reported && cpu.pc == 0x802315A8u) {
            g_file_select_reported = true;
            g_file_select_retrace = g_host_retrace_count;
            fprintf(stderr,
                    "[boot-milestone] file-select retrace=%llu blocks=%llu "
                    "scene=0x%08X\n",
                    (unsigned long long)g_file_select_retrace,
                    (unsigned long long)blocks, cpu.gpr[3]);
        }
        if (g_name_character_pulse.triggered &&
            !g_name_character_pulse.released) {
            const u32 game_pad = mem_read32(&cpu, 0x803A4DE0u);
            const u32 jut_hold = game_pad >= 0x80000000u
                                     ? mem_read32(&cpu, game_pad + 0x18u)
                                     : 0u;
            const u32 jut_trigger = game_pad >= 0x80000000u
                                        ? mem_read32(&cpu, game_pad + 0x1Cu)
                                        : 0u;
            if (!g_name_character_jut_hold_reported &&
                (jut_hold & 0x0100u) != 0u) {
                g_name_character_jut_hold_reported = true;
                fprintf(stderr,
                        "[pad-milestone] name-character-jut-hold "
                        "retrace=%llu blocks=%llu hold=0x%08X\n",
                        (unsigned long long)g_host_retrace_count,
                        (unsigned long long)blocks, jut_hold);
            }
            if (!g_name_character_jut_trigger_reported &&
                (jut_trigger & 0x0100u) != 0u) {
                g_name_character_jut_trigger_reported = true;
                fprintf(stderr,
                        "[pad-milestone] name-character-jut-trigger "
                        "retrace=%llu blocks=%llu trigger=0x%08X\n",
                        (unsigned long long)g_host_retrace_count,
                        (unsigned long long)blocks, jut_trigger);
            }
        }
        if (g_name_character_pulse.triggered &&
            !g_name_character_pulse.released) {
            const u8 cpad_hold = mem_read8(&cpu, 0x803A4E20u);
            const u8 cpad_trigger = mem_read8(&cpu, 0x803A4E22u);
            if (!g_name_character_cpad_hold_reported &&
                (cpad_hold & 0x01u) != 0u) {
                g_name_character_cpad_hold_reported = true;
                fprintf(stderr,
                        "[pad-milestone] name-character-cpad-hold "
                        "retrace=%llu blocks=%llu hold=0x%02X\n",
                        (unsigned long long)g_host_retrace_count,
                        (unsigned long long)blocks, cpad_hold);
            }
            if (!g_name_character_cpad_trigger_reported &&
                (cpad_trigger & 0x01u) != 0u) {
                g_name_character_cpad_trigger_reported = true;
                fprintf(stderr,
                        "[pad-milestone] name-character-cpad-trigger "
                        "retrace=%llu blocks=%llu trigger=0x%02X\n",
                        (unsigned long long)g_host_retrace_count,
                        (unsigned long long)blocks, cpad_trigger);
            }
        }
        if (!g_name_scene_create_reported && cpu.pc == 0x8022F9FCu) {
            g_name_scene_create_reported = true;
            g_name_scene_create_retrace = g_host_retrace_count;
            g_name_scene_object = cpu.gpr[3];
            fprintf(stderr,
                    "[boot-milestone] name-scene-create retrace=%llu "
                    "blocks=%llu scene=0x%08X\n",
                    (unsigned long long)g_name_scene_create_retrace,
                    (unsigned long long)blocks, cpu.gpr[3]);
        }
        if (!g_name_scene_execute_reported && cpu.pc == 0x802305E0u) {
            g_name_scene_execute_reported = true;
            g_name_scene_execute_retrace = g_host_retrace_count;
            g_name_scene_object = cpu.gpr[3];
            fprintf(stderr,
                    "[boot-milestone] name-scene-execute retrace=%llu "
                    "blocks=%llu scene=0x%08X main_proc=%u memcard_proc=%u\n",
                    (unsigned long long)g_name_scene_execute_retrace,
                    (unsigned long long)blocks, cpu.gpr[3],
                    mem_read8(&cpu, cpu.gpr[3] + 0x554u),
                    mem_read8(&cpu, cpu.gpr[3] + 0x556u));
        }
        if (!g_memcard_check_reported && cpu.pc == 0x80230A14u) {
            g_memcard_check_reported = true;
            g_memcard_check_retrace = g_host_retrace_count;
            fprintf(stderr,
                    "[boot-milestone] memcard-check retrace=%llu blocks=%llu "
                    "scene=0x%08X main_proc=%u memcard_proc=%u\n",
                    (unsigned long long)g_memcard_check_retrace,
                    (unsigned long long)blocks, cpu.gpr[3],
                    mem_read8(&cpu, cpu.gpr[3] + 0x554u),
                    mem_read8(&cpu, cpu.gpr[3] + 0x556u));
        }
        if (g_name_scene_execute_reported && cpu.pc == 0x8017E798u &&
            bluewake_pad_event_schedule_trigger(
                &g_no_card_dismiss_pulse, g_host_retrace_count)) {
            fprintf(stderr,
                    "[pad] no-card-dismiss-ready pulse trigger start=%llu "
                    "length=%llu buttons=0x%04X\n",
                    (unsigned long long)g_no_card_dismiss_pulse.start_retrace,
                    (unsigned long long)g_no_card_dismiss_pulse.length,
                    g_no_card_dismiss_pulse.buttons);
        }
        if (g_name_scene_execute_reported && cpu.pc == 0x8017E86Cu &&
            cpu.gpr[3] >= 0x80000000u) {
            const u8 yes_no = mem_read8(&cpu, cpu.gpr[3] + 0x2F6u);
            if (yes_no != 0u &&
                bluewake_pad_axis_event_schedule_trigger(
                    &g_no_save_left_pulse, g_host_retrace_count)) {
                fprintf(stderr,
                        "[pad] no-save-left-ready pulse trigger start=%llu "
                        "length=%llu stick_x=%d\n",
                        (unsigned long long)g_no_save_left_pulse.start_retrace,
                        (unsigned long long)g_no_save_left_pulse.length,
                        g_no_save_left_pulse.value);
            } else if (yes_no == 0u && g_no_save_left_pulse.triggered &&
                       bluewake_pad_event_schedule_trigger(
                           &g_no_save_confirm_pulse,
                           g_host_retrace_count)) {
                fprintf(stderr,
                        "[pad] no-save-confirm-ready pulse trigger start=%llu "
                        "length=%llu buttons=0x%04X\n",
                        (unsigned long long)
                            g_no_save_confirm_pulse.start_retrace,
                        (unsigned long long)g_no_save_confirm_pulse.length,
                        g_no_save_confirm_pulse.buttons);
            }
        }
        if (g_file_select_reported && cpu.pc == 0x80181634u &&
            cpu.gpr[3] >= 0x80000000u &&
            mem_read8(&cpu, cpu.gpr[3] + 0x3922u) == 0u &&
            mem_read8(&cpu, cpu.gpr[3] + 0x3914u) != 0u &&
            bluewake_pad_event_schedule_trigger(
                &g_file_slot_select_pulse, g_host_retrace_count)) {
            fprintf(stderr,
                    "[pad] new-file-slot-ready pulse trigger start=%llu "
                    "mode=latched buttons=0x%04X\n",
                    (unsigned long long)g_file_slot_select_pulse.start_retrace,
                    g_file_slot_select_pulse.buttons);
        }
        if (g_file_select_reported && cpu.pc == 0x80182A90u &&
            cpu.gpr[3] >= 0x80000000u &&
            mem_read8(&cpu, cpu.gpr[3] + 0x3922u) == 0u &&
            mem_read8(&cpu, cpu.gpr[3] + 0x3914u) != 0u &&
            mem_read8(&cpu, cpu.gpr[3] + 0x3928u) == 0u) {
            bluewake_pad_event_schedule_release(
                &g_file_slot_select_pulse, g_host_retrace_count);
        }
        if (g_file_select_reported && cpu.pc == 0x80182A90u &&
            cpu.gpr[3] >= 0x80000000u &&
            mem_read8(&cpu, cpu.gpr[3] + 0x3922u) == 0u &&
            mem_read8(&cpu, cpu.gpr[3] + 0x3914u) != 0u &&
            mem_read8(&cpu, cpu.gpr[3] + 0x3928u) == 0u &&
            host_guest_cpad_a_released(&cpu) &&
            bluewake_pad_event_schedule_trigger(
                &g_file_start_pulse, g_host_retrace_count)) {
            fprintf(stderr,
                    "[pad] new-file-start-ready pulse trigger start=%llu "
                    "mode=latched buttons=0x%04X\n",
                    (unsigned long long)g_file_start_pulse.start_retrace,
                    g_file_start_pulse.buttons);
        }
        // The name-scene milestones below are properties of the guest, not of
        // how the input reached it, so they must not exist only for the route.
        // On the route the start pulse gates them and that timing is unchanged.
        // With live keyboard input no route pulse is ever armed, and gating on
        // one that can never be armed left "name-input-complete" invisible to a
        // real double-click run. Report from guest state when the pulse has
        // fired or when no route pulse is configured. The pulse-driven
        // scheduling further down stays gated by the pulse itself, and
        // trigger()/release() are inert when nothing is configured, so a live
        // run schedules nothing here.
        if (g_name_scene_object >= 0x80000000u &&
            (g_file_start_pulse.triggered ||
             !g_file_start_pulse.configured)) {
            const u8 main_proc =
                mem_read8(&cpu, g_name_scene_object + 0x554u);
            const u32 name =
                mem_read32(&cpu, g_name_scene_object + 0x424u);
            if (name >= 0x80000000u &&
                !g_name_input_complete_reported &&
                mem_read8(&cpu, name + 0x290Bu) == 1u) {
                g_name_input_complete_reported = true;
                g_name_input_complete_retrace = g_host_retrace_count;
                fprintf(stderr,
                        "[boot-milestone] name-input-complete retrace=%llu "
                        "blocks=%llu name=0x%08X\n",
                        (unsigned long long)g_name_input_complete_retrace,
                        (unsigned long long)blocks, name);
            }
            if (!g_name_scene_change_reported && main_proc == 9u &&
                mem_read8(&cpu, g_name_scene_object + 0x55Fu) == 1u) {
                g_name_scene_change_reported = true;
                g_name_scene_change_retrace = g_host_retrace_count;
                fprintf(stderr,
                        "[boot-milestone] name-scene-change-ready "
                        "retrace=%llu blocks=%llu scene=0x%08X\n",
                        (unsigned long long)g_name_scene_change_retrace,
                        (unsigned long long)blocks, g_name_scene_object);
            }
            const u32 overlap_request = mem_read32(&cpu, 0x803F6160u);
            if (overlap_request >= 0x80000000u &&
                mem_read16(&cpu, overlap_request + 0x04u) == 1u) {
                const u32 overlap_phase =
                    mem_read32(&cpu, overlap_request + 0x1Cu);
                if (!g_open_scene_request_reported) {
                    g_open_scene_request_reported = true;
                    g_open_scene_request_retrace = g_host_retrace_count;
                    fprintf(stderr,
                            "[boot-milestone] open-scene-request retrace=%llu "
                            "blocks=%llu request=0x%08X scene_request=%u "
                            "fade_proc=%d\n",
                            (unsigned long long)g_open_scene_request_retrace,
                            (unsigned long long)blocks, overlap_request,
                            mem_read32(&cpu, 0x803F6168u),
                            (s16)mem_read16(&cpu, overlap_request + 0x10u));
                }
                if (overlap_phase != g_overlap_last_phase) {
                    g_overlap_last_phase = overlap_phase;
                    g_overlap_terminal_phase = overlap_phase;
                    fprintf(stderr,
                            "[scene-milestone] overlap-phase phase=%u "
                            "retrace=%llu blocks=%llu peek_time=%u "
                            "is_peek=%u pid=0x%08X task=0x%08X "
                            "handler=0x%08X\n",
                            overlap_phase,
                            (unsigned long long)g_host_retrace_count,
                            (unsigned long long)blocks,
                            mem_read16(&cpu, overlap_request + 0x06u),
                            mem_read32(&cpu, overlap_request + 0x08u),
                            mem_read32(&cpu, overlap_request + 0x14u),
                            mem_read32(&cpu, overlap_request + 0x20u),
                            mem_read32(&cpu, overlap_request + 0x18u));
                    if (overlap_phase == 6u && g_new_game_intro_reported &&
                        aurora_enabled && opening_capture_path != NULL &&
                        opening_capture_path[0] != '\0' &&
                        diagnostic_capture_retrace == 0u &&
                        !capture_player_ready &&
                        !opening_capture_requested) {
                        aurora_request_framebuffer_readback();
                        opening_capture_requested = true;
                        fprintf(stderr,
                                "[frame-capture] opening requested "
                                "retrace=%llu path=\"%s\"\n",
                                (unsigned long long)g_host_retrace_count,
                                opening_capture_path);
                    }
                }
            }
            if (main_proc == 7u && name >= 0x80000000u) {
                const u8 sel_proc = mem_read8(&cpu, name + 0x2903u);
                const u8 sel_menu = mem_read8(&cpu, name + 0x2904u);
                const u8 cur_pos = mem_read8(&cpu, name + 0x2907u);
                bluewake_pad_event_schedule_release(
                    &g_file_start_pulse, g_host_retrace_count);
                if (sel_proc == 0u && cur_pos == 0u &&
                    host_guest_cpad_a_released(&cpu) &&
                    bluewake_pad_event_schedule_trigger(
                        &g_name_character_pulse, g_host_retrace_count)) {
                    fprintf(stderr,
                            "[pad] name-character-ready pulse trigger "
                            "start=%llu mode=latched buttons=0x%04X\n",
                            (unsigned long long)
                                g_name_character_pulse.start_retrace,
                            g_name_character_pulse.buttons);
                }
                if (g_name_character_pulse.triggered && cur_pos != 0u) {
                    bluewake_pad_event_schedule_release(
                        &g_name_character_pulse, g_host_retrace_count);
                    if (host_guest_cpad_a_released(&cpu) &&
                        bluewake_pad_event_schedule_trigger(
                            &g_name_end_pulse, g_host_retrace_count)) {
                        fprintf(stderr,
                                "[pad] name-end-ready pulse trigger "
                                "start=%llu mode=latched buttons=0x%04X\n",
                                (unsigned long long)
                                    g_name_end_pulse.start_retrace,
                                g_name_end_pulse.buttons);
                    }
                }
                if (g_name_end_pulse.triggered && sel_proc == 1u &&
                    sel_menu == 4u) {
                    bluewake_pad_event_schedule_release(
                        &g_name_end_pulse, g_host_retrace_count);
                    if (host_guest_cpad_start_released(&cpu) &&
                        bluewake_pad_event_schedule_trigger(
                            &g_name_confirm_pulse, g_host_retrace_count)) {
                        fprintf(stderr,
                                "[pad] name-confirm-ready pulse trigger "
                                "start=%llu mode=latched buttons=0x%04X\n",
                                (unsigned long long)
                                    g_name_confirm_pulse.start_retrace,
                                g_name_confirm_pulse.buttons);
                    }
                }
                if (g_name_confirm_pulse.triggered && sel_proc == 2u) {
                    bluewake_pad_event_schedule_release(
                        &g_name_confirm_pulse, g_host_retrace_count);
                }
            }
        }
        if (host_canonical_linked_pc(cpu.pc) == 0x81E01BA4u &&
            cpu.gpr[3] == 0u &&
            cpu.gpr[31] >= 0x80000000u) {
            const u32 title_proc = mem_read32(&cpu, cpu.gpr[31] + 0x298u);
            if (title_proc >= 0x80000000u &&
                mem_read32(&cpu, title_proc + 0x30u) == 1u &&
                bluewake_pad_event_schedule_trigger(
                    &g_title_confirm_pulse, g_host_retrace_count)) {
                fprintf(stderr,
                        "[pad] title-confirm-ready pulse trigger start=%llu "
                        "length=%llu buttons=0x%04X\n",
                        (unsigned long long)g_title_confirm_pulse.start_retrace,
                        (unsigned long long)g_title_confirm_pulse.length,
                        g_title_confirm_pulse.buttons);
            }
        }
        if (g_pad_trace && g_title_input_reports < 200u &&
            host_canonical_linked_pc(cpu.pc) == 0x81E01B88u &&
            cpu.gpr[3] >= 0x80000000u) {
            const u32 title_proc = mem_read32(&cpu, cpu.gpr[3] + 0x298u);
            fprintf(stderr,
                    "[title-execute] #%u retrace=%llu object=0x%08X "
                    "title_proc=0x%08X enter_mode=%d scene_flag=%u "
                    "hold=0x%04X trig=0x%04X\n",
                    g_title_input_reports + 1u,
                    (unsigned long long)g_host_retrace_count, cpu.gpr[3],
                    title_proc,
                    title_proc >= 0x80000000u
                        ? (s32)mem_read32(&cpu, title_proc + 0x30u)
                        : -1,
                    mem_read8(&cpu, cpu.gpr[3] + 0x29Cu),
                    mem_read16(&cpu, 0x803A4E20u),
                    mem_read16(&cpu, 0x803A4E22u));
            g_title_input_reports++;
        }
        if (g_pad_trace && pad_retail_read_reports < 32u &&
            cpu.pc == 0x80315A20u) {
            fprintf(stderr,
                    "[pad-retail-read] status=0x%08X enabled=0x%08X "
                    "reset=0x%08X waiting=0x%08X pending=0x%08X type=0x%08X "
                    "retrace=%llu blocks=%llu\n",
                    cpu.gpr[3], mem_read32(&cpu, 0x803F7B94u),
                    mem_read32(&cpu, 0x803F7B98u),
                    mem_read32(&cpu, 0x803F7BA0u),
                    mem_read32(&cpu, 0x803F7BA8u),
                    mem_read32(&cpu, 0x803A0780u),
                    (unsigned long long)g_host_retrace_count,
                    (unsigned long long)blocks);
            pad_retail_read_reports++;
        }
        if (g_pad_conversion_trace &&
            pad_conversion_reports <
                (g_pad_conversion_all_trace ? 128u : 24u) &&
            (mem_read32(&cpu, 0x803F7B3Cu) >= 15u ||
             g_pad_conversion_all_trace) &&
            (cpu.pc == 0x802C39A4u || cpu.pc == 0x802C3A38u)) {
            const u32 base = cpu.gpr[31];
            const u32 status = base + 24u;
            const u32 stick = base + 264u;
            fprintf(stderr,
                    "[pad-conversion] pc=0x%08X base=0x%08X "
                    "status0=%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X "
                    "stick_raw=%d,%d stick_pos=%08X,%08X value=%08X "
                    "make_status=0x%08X "
                    "retrace=%llu blocks=%llu\n",
                    cpu.pc, base,
                    mem_read8(&cpu, status + 0u), mem_read8(&cpu, status + 1u),
                    mem_read8(&cpu, status + 2u), mem_read8(&cpu, status + 3u),
                    mem_read8(&cpu, status + 4u), mem_read8(&cpu, status + 5u),
                    mem_read8(&cpu, status + 6u), mem_read8(&cpu, status + 7u),
                    (s8)mem_read8(&cpu, status + 2u),
                    (s8)mem_read8(&cpu, status + 3u),
                    mem_read32(&cpu, stick + 0u), mem_read32(&cpu, stick + 4u),
                    mem_read32(&cpu, stick + 8u),
                    mem_read32(&cpu, 0x803F6754u),
                    (unsigned long long)mem_read32(&cpu, 0x803F7B3Cu),
                    (unsigned long long)blocks);
            pad_conversion_reports++;
        }
        if (g_pad_si_trace && g_pad_si_reports < 64u &&
            (cpu.pc == 0x8030C550u || cpu.pc == 0x803161B8u)) {
            const u32 channel = cpu.pc == 0x8030C550u ? cpu.gpr[29] : cpu.gpr[3];
            const u32 input_buffer = cpu.gpr[5];
            const u32 response_regs = 0xCC006400u + channel * 12u;
            fprintf(stderr,
                    "[pad-si] pc=0x%08X channel=%u r3=0x%08X r5=0x%08X "
                    "r6=0x%08X response=0x%08X,0x%08X input=0x%08X "
                    "words=0x%08X,0x%08X valid=0x%08X status=0x%08X "
                    "retrace=%llu blocks=%llu\n",
                    cpu.pc, channel, cpu.gpr[3], cpu.gpr[5], cpu.gpr[6],
                    mem_read32(&cpu, response_regs + 4u),
                    mem_read32(&cpu, response_regs + 8u), input_buffer,
                    mem_read32(&cpu, input_buffer),
                    mem_read32(&cpu, input_buffer + 4u),
                    cpu.pc == 0x8030C550u
                        ? mem_read32(&cpu, cpu.gpr[3] + 432u)
                        : 0u,
                    cpu.pc == 0x803161B8u ? cpu.gpr[4] : 0u,
                    (unsigned long long)g_host_retrace_count,
                    (unsigned long long)blocks);
            g_pad_si_reports++;
        }
        if (g_pad_lifecycle_trace && g_pad_lifecycle_reports < 128u &&
            cpu.pc == 0x80315604u) {
            fprintf(stderr,
                    "[pad-lifecycle] PADReset entry mask=0x%08X lr=0x%08X enabled=0x%08X "
                    "reset=0x%08X waiting=0x%08X pending=0x%08X retrace=%llu "
                    "blocks=%llu\n",
                    cpu.gpr[3], cpu.lr, mem_read32(&cpu, 0x803F7B94u),
                    mem_read32(&cpu, 0x803F7B98u),
                    mem_read32(&cpu, 0x803F7BA0u),
                    mem_read32(&cpu, 0x803F7BA8u),
                    (unsigned long long)g_host_retrace_count,
                    (unsigned long long)blocks);
            g_pad_lifecycle_reports++;
        }
        if (g_pad_trace &&
            ((message_pane_reports < 96u && blocks < 31300000ull &&
              (cpu.pc == 0x8003BD88u ||
               (cpu.pc >= 0x8003BDD4u && cpu.pc <= 0x8003BDE0u) ||
               cpu.pc == 0x8003BDE4u)) ||
            (late_message_pane_reports < 64u && blocks >= 31300000ull &&
              (cpu.pc == 0x8003BC88u || cpu.pc == 0x8003BD88u ||
               (cpu.pc >= 0x8003BDD4u && cpu.pc <= 0x8003BDE0u))))) {
            const u32 object_word0 = mem_read32(&cpu, cpu.gpr[3]);
            const u32 vtable = mem_read32(&cpu, object_word0);
            const u32 move_target = mem_read32(&cpu, vtable + 16u);
            fprintf(stderr,
                    "[fopmsg] #%u pc=0x%08X lr=0x%08X r3=0x%08X "
                    "r4=0x%08X r5=0x%08X r6=0x%08X retrace=%llu "
                    "blocks=%llu object_word0=0x%08X vtable=0x%08X "
                    "move_target=0x%08X r12=0x%08X ctr=0x%08X "
                    "r31=0x%08X r31_word0=0x%08X r3_word4=0x%08X\n",
                    message_pane_reports + 1u, cpu.pc, cpu.lr, cpu.gpr[3],
                    cpu.gpr[4], cpu.gpr[5], cpu.gpr[6],
                    (unsigned long long)g_host_retrace_count,
                    (unsigned long long)blocks, object_word0, vtable, move_target,
                    cpu.gpr[12], cpu.ctr, cpu.gpr[31],
                    mem_read32(&cpu, cpu.gpr[31]),
                    mem_read32(&cpu, cpu.gpr[3] + 4u));
            if (blocks >= 31300000ull)
                late_message_pane_reports++;
            else
                message_pane_reports++;
        }
        if (g_pad_trace && pane_data_reports < 32u &&
            (cpu.pc == 0x8003BB4Cu || cpu.pc == 0x8003BB78u ||
             cpu.pc == 0x8003BBA4u)) {
            fprintf(stderr,
                    "[fopmsg] set-pane-data #%u pc=0x%08X r3=0x%08X "
                    "r4=0x%08X r5=0x%08X r3_word0=0x%08X "
                    "r3_word4=0x%08X r4_word0=0x%08X blocks=%llu\n",
                    pane_data_reports + 1u, cpu.pc, cpu.gpr[3], cpu.gpr[4],
                    cpu.gpr[5], mem_read32(&cpu, cpu.gpr[3]),
                    mem_read32(&cpu, cpu.gpr[3] + 4u),
                    mem_read32(&cpu, cpu.gpr[4]),
                    (unsigned long long)blocks);
            pane_data_reports++;
        }
        if (g_pad_trace && cursor_pane_data_reports < 16u &&
            cpu.pc == 0x8003BB4Cu && cpu.gpr[3] == 0x8158E754u) {
            fprintf(stderr,
                    "[fopmsg] cursor-pane-data #%u r3=0x%08X r4=0x%08X "
                    "r4_word0=0x%08X r4_word4=0x%08X lr=0x%08X "
                    "blocks=%llu\n",
                    cursor_pane_data_reports + 1u, cpu.gpr[3], cpu.gpr[4],
                    mem_read32(&cpu, cpu.gpr[4]),
                    mem_read32(&cpu, cpu.gpr[4] + 4u), cpu.lr,
                    (unsigned long long)blocks);
            cursor_pane_data_reports++;
        }
        if (g_j2d_lookup_trace && g_j2d_lookup_reports < 64u &&
            (cpu.pc == 0x802D0944u || cpu.pc == 0x802D1150u)) {
            char resource_name[128] = {0};
            if (cpu.pc == 0x802D0944u && cpu.gpr[4] != 0u)
                guest_read_cstr(&cpu, cpu.gpr[4], resource_name,
                                sizeof(resource_name));
            fprintf(stderr,
                    "[j2d-lookup] pc=0x%08X this=0x%08X mtag=0x%08X "
                    "tag=0x%08X archive=0x%08X name=\"%s\" "
                    "word0=0x%08X blocks=%llu\n",
                    cpu.pc, cpu.gpr[3], mem_read32(&cpu, cpu.gpr[3] + 8u),
                    cpu.gpr[4], cpu.gpr[5], resource_name,
                    mem_read32(&cpu, cpu.gpr[3]),
                    (unsigned long long)blocks);
            g_j2d_lookup_reports++;
        }
        if (g_j2d_lookup_trace && g_j2d_target_reports < 128u &&
            (cpu.pc == 0x801898D0u || cpu.pc == 0x801898D4u ||
             cpu.pc == 0x801898E0u || cpu.pc == 0x801898E4u ||
             cpu.pc == 0x802D0680u || cpu.pc == 0x802D0944u ||
             cpu.pc == 0x802D0958u || cpu.pc == 0x802D096Cu ||
             cpu.pc == 0x802D0988u || cpu.pc == 0x802D09E0u ||
             cpu.pc == 0x802D0A28u || cpu.pc == 0x802D1150u)) {
            char resource_name[128] = {0};
            bool target = false;
            if (cpu.pc == 0x802D0680u) {
                const u32 mtag = mem_read32(&cpu, cpu.gpr[3] + 8u);
                const bool cursor_tag = cpu.gpr[4] >= 0x32637531u &&
                                         cpu.gpr[4] <= 0x32637535u;
                const bool cursor_pane = mtag >= 0x32637531u &&
                                         mtag <= 0x32637535u;
                target = cursor_tag &&
                         (mtag == 0x524F4F54u || cursor_pane);
            } else if (cpu.pc == 0x801898D0u || cpu.pc == 0x801898D4u ||
                cpu.pc == 0x801898E0u || cpu.pc == 0x801898E4u) {
                target = true;
            } else if (cpu.pc == 0x802D0944u && cpu.gpr[4] != 0u) {
                guest_read_cstr(&cpu, cpu.gpr[4], resource_name,
                                sizeof(resource_name));
                target = strcmp(resource_name, "file_select.blo") == 0;
                if (target)
                    g_j2d_file_select_set_active = true;
            } else if (cpu.pc == 0x802D0958u || cpu.pc == 0x802D096Cu ||
                       cpu.pc == 0x802D0988u || cpu.pc == 0x802D09E0u) {
                target = g_j2d_file_select_set_active;
            } else if (cpu.pc == 0x802D0A28u) {
                target = g_j2d_file_select_set_active;
            } else if (cpu.pc == 0x802D1150u) {
                target = cpu.gpr[4] >= 0x32637531u &&
                         cpu.gpr[4] <= 0x32637535u;
            }
            if (target) {
                if (cpu.pc == 0x802D0680u) {
                    fprintf(stderr,
                            "[j2d-target] pane-search this=0x%08X "
                            "mtag=0x%08X tag=0x%08X child_head=0x%08X "
                            "child_tail=0x%08X child_count=%u blocks=%llu\n",
                            cpu.gpr[3], mem_read32(&cpu, cpu.gpr[3] + 8u),
                            cpu.gpr[4], mem_read32(&cpu, cpu.gpr[3] + 0xB0u),
                            mem_read32(&cpu, cpu.gpr[3] + 0xB4u),
                            mem_read32(&cpu, cpu.gpr[3] + 0xB8u),
                            (unsigned long long)blocks);
                } else if (cpu.pc == 0x802D0A28u) {
                    fprintf(stderr,
                            "[j2d-target] screen-set-return result=%u "
                            "screen=0x%08X child_head=0x%08X child_tail=0x%08X "
                            "child_count=%u lr=0x%08X blocks=%llu\n",
                            cpu.gpr[3], cpu.gpr[29],
                            mem_read32(&cpu, cpu.gpr[29] + 0xB0u),
                            mem_read32(&cpu, cpu.gpr[29] + 0xB4u),
                            mem_read32(&cpu, cpu.gpr[29] + 0xB8u), cpu.lr,
                            (unsigned long long)blocks);
                    g_j2d_file_select_set_active = false;
                } else if (cpu.pc == 0x802D0988u) {
                    fprintf(stderr,
                            "[j2d-target] file-select-resource expanded-size "
                            "size=0x%08X pRes=0x%08X screen=0x%08X "
                            "res_word0=0x%08X res_word4=0x%08X "
                            "res_d84=0x%08X raw_d84=0x%08X blocks=%llu\n",
                            cpu.gpr[3], cpu.gpr[31], cpu.gpr[29],
                            mem_read32(&cpu, cpu.gpr[31]),
                            mem_read32(&cpu, cpu.gpr[31] + 4u),
                            mem_read32(&cpu, 0x81513844u),
                            host_raw_ram_read32(&cpu, 0x81513844u),
                            (unsigned long long)blocks);
                } else if (cpu.pc == 0x802D09E0u) {
                    fprintf(stderr,
                            "[j2d-target] file-select-resource set-result "
                            "result=%u screen=0x%08X stream_word0=0x%08X "
                            "stream_word4=0x%08X blocks=%llu\n",
                            cpu.gpr[3], cpu.gpr[29],
                            mem_read32(&cpu, cpu.gpr[1] + 8u),
                            mem_read32(&cpu, cpu.gpr[1] + 12u),
                            (unsigned long long)blocks);
                } else if (cpu.pc == 0x802D0958u || cpu.pc == 0x802D096Cu) {
                    fprintf(stderr,
                            "[j2d-target] file-select-resource pc=0x%08X "
                            "r3=0x%08X screen=0x%08X archive=0x%08X "
                            "r31=0x%08X res_word0=0x%08X res_word4=0x%08X "
                            "res_d84=0x%08X raw_d84=0x%08X "
                            "lr=0x%08X "
                            "blocks=%llu\n",
                            cpu.pc, cpu.gpr[3], cpu.gpr[29], cpu.gpr[30],
                            cpu.gpr[31], mem_read32(&cpu, cpu.gpr[3]),
                            mem_read32(&cpu, cpu.gpr[3] + 4u),
                            mem_read32(&cpu, 0x81513844u),
                            host_raw_ram_read32(&cpu, 0x81513844u), cpu.lr,
                            (unsigned long long)blocks);
                } else {
                    fprintf(stderr,
                            "[j2d-target] pc=0x%08X this=0x%08X tag=0x%08X "
                            "name=\"%s\" word0=0x%08X blocks=%llu\n",
                            cpu.pc, cpu.gpr[3], cpu.gpr[4], resource_name,
                            mem_read32(&cpu, cpu.gpr[3]),
                            (unsigned long long)blocks);
                }
                g_j2d_target_reports++;
            }
        }
        if (g_j2d_lookup_trace && g_j2d_file_select_set_active &&
            g_j2d_resource_reports < 16u &&
            (cpu.pc == 0x802D0958u || cpu.pc == 0x802D096Cu ||
             cpu.pc == 0x802D098Cu || cpu.pc == 0x802D09E0u ||
             cpu.pc == 0x802D0A28u)) {
            if (cpu.pc == 0x802D098Cu) {
                fprintf(stderr,
                        "[j2d-resource] expanded-size=0x%08X pRes=0x%08X "
                        "screen=0x%08X res_word0=0x%08X res_word4=0x%08X "
                        "res_d84=0x%08X raw_d84=0x%08X blocks=%llu\n",
                            cpu.gpr[3], cpu.gpr[31], cpu.gpr[29],
                            mem_read32(&cpu, cpu.gpr[31]),
                            mem_read32(&cpu, cpu.gpr[31] + 4u),
                            mem_read32(&cpu, 0x81513844u),
                            host_raw_ram_read32(&cpu, 0x81513844u),
                            (unsigned long long)blocks);
            } else if (cpu.pc == 0x802D09E0u) {
                fprintf(stderr,
                        "[j2d-resource] set-result=%u screen=0x%08X "
                        "child_head=0x%08X child_tail=0x%08X child_count=%u "
                        "blocks=%llu\n",
                        cpu.gpr[3], cpu.gpr[29],
                        mem_read32(&cpu, cpu.gpr[29] + 0xB0u),
                        mem_read32(&cpu, cpu.gpr[29] + 0xB4u),
                        mem_read32(&cpu, cpu.gpr[29] + 0xB8u),
                        (unsigned long long)blocks);
            } else if (cpu.pc == 0x802D0A28u) {
                fprintf(stderr,
                        "[j2d-resource] set-return=%u screen=0x%08X "
                        "child_count=%u lr=0x%08X blocks=%llu\n",
                        cpu.gpr[3], cpu.gpr[29],
                        mem_read32(&cpu, cpu.gpr[29] + 0xB8u), cpu.lr,
                        (unsigned long long)blocks);
                g_j2d_file_select_set_active = false;
            }
            g_j2d_resource_reports++;
        }
        if (g_j2d_lookup_trace && g_j2d_file_select_set_active &&
            ((cpu.pc == 0x802D0B40u && g_j2d_create_reports < 400u) ||
             (cpu.pc != 0x802D0B40u && g_j2d_tree_reports < 256u)) &&
            (cpu.pc == 0x802D0B40u || cpu.pc == 0x802CFB7Cu ||
             cpu.pc == 0x802BEFE4u || cpu.pc == 0x802BF0FCu ||
             cpu.pc == 0x802BF134u)) {
            if (cpu.pc == 0x802D0B40u) {
                fprintf(stderr,
                        "[j2d-tree] create-pane magic=0x%08X size=0x%08X "
                        "stream=0x%08X position=0x%08X parent=0x%08X "
                        "parent_count=%u "
                        "blocks=%llu\n",
                        mem_read32(&cpu, cpu.gpr[4]),
                        mem_read32(&cpu, cpu.gpr[4] + 4u), cpu.gpr[5],
                        mem_read32(&cpu, cpu.gpr[5] + 0x10u), cpu.gpr[6],
                        mem_read32(&cpu, cpu.gpr[6] + 0xB8u),
                        (unsigned long long)blocks);
            } else if (cpu.pc == 0x802CFB7Cu) {
                fprintf(stderr,
                        "[j2d-tree] pane-list-init this=0x%08X owner=0x%08X "
                        "head=0x%08X tail=0x%08X count=%u blocks=%llu\n",
                        cpu.gpr[3], cpu.gpr[28], mem_read32(&cpu, cpu.gpr[3]),
                        mem_read32(&cpu, cpu.gpr[3] + 4u),
                        mem_read32(&cpu, cpu.gpr[3] + 8u),
                        (unsigned long long)blocks);
            } else if (cpu.pc == 0x802BEFE4u) {
                fprintf(stderr,
                        "[j2d-tree] link-init link=0x%08X owner=0x%08X "
                        "list=0x%08X prev=0x%08X next=0x%08X blocks=%llu\n",
                        cpu.gpr[3], cpu.gpr[4], mem_read32(&cpu, cpu.gpr[3] + 4u),
                        mem_read32(&cpu, cpu.gpr[3] + 8u),
                        mem_read32(&cpu, cpu.gpr[3] + 12u),
                        (unsigned long long)blocks);
            } else if (cpu.pc == 0x802BF0FCu) {
                fprintf(stderr,
                        "[j2d-tree] list-init list=0x%08X head=0x%08X "
                        "tail=0x%08X count=%u blocks=%llu\n",
                        cpu.gpr[3], mem_read32(&cpu, cpu.gpr[3]),
                        mem_read32(&cpu, cpu.gpr[3] + 4u),
                        mem_read32(&cpu, cpu.gpr[3] + 8u),
                        (unsigned long long)blocks);
            } else {
                fprintf(stderr,
                        "[j2d-tree] append list=0x%08X link=0x%08X "
                        "list_count=%u link_list=0x%08X link_prev=0x%08X "
                        "link_next=0x%08X blocks=%llu\n",
                        cpu.gpr[3], cpu.gpr[4], mem_read32(&cpu, cpu.gpr[3] + 8u),
                        mem_read32(&cpu, cpu.gpr[4] + 4u),
                        mem_read32(&cpu, cpu.gpr[4] + 8u),
                        mem_read32(&cpu, cpu.gpr[4] + 12u),
                        (unsigned long long)blocks);
            }
            if (cpu.pc == 0x802D0B40u)
                g_j2d_create_reports++;
            else
                g_j2d_tree_reports++;
        }
        if (scene_phase_reports < 48u &&
            (cpu.pc == 0x8003F5F8u || cpu.pc == 0x8003F608u ||
             cpu.pc == 0x8003F60Cu)) {
            const u32 request = cpu.gpr[30];
            fprintf(stderr,
                    "[scene] phase pc=0x%08X request=0x%08X result=%d "
                    "handler=0x%08X target=0x%08X "
                    "parameter=%d request_id=0x%08X creating_id=0x%08X "
                    "proc_name=%d user=0x%08X phase_handler=0x%08X "
                    "current_thread=0x%08X run_bits=0x%08X\n",
                    cpu.pc, request, (s32)cpu.gpr[3], cpu.gpr[12], cpu.ctr,
                    request ? (s32)mem_read32(&cpu, request + 0x40u) : -1,
                    request ? mem_read32(&cpu, request + 0x44u) : 0u,
                    request ? mem_read32(&cpu, request + 0x54u) : 0u,
                    request ? (s32)mem_read16(&cpu, request + 0x58u) : -1,
                    request ? mem_read32(&cpu, request + 0x5Cu) : 0u,
                    request ? mem_read32(&cpu, request + 0x38u) : 0u,
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x803F7A30u));
            scene_phase_reports++;
        }
        if (g_scene_overlap_trace &&
            scene_overlap_trace_reports < 128u &&
            (cpu.pc == 0x800296D4u || cpu.pc == 0x80029708u ||
             cpu.pc == 0x80029730u || cpu.pc == 0x80029764u ||
             cpu.pc == 0x8002986Cu || cpu.pc == 0x800298C8u ||
             cpu.pc == 0x80029914u || cpu.pc == 0x80029964u ||
             cpu.pc == 0x800299D4u || cpu.pc == 0x80029A24u ||
             cpu.pc == 0x80029A84u || cpu.pc == 0x80029B70u ||
             cpu.pc == 0x80029C58u || cpu.pc == 0x8002A048u ||
             cpu.pc == 0x8002A078u || cpu.pc == 0x8002A0A8u)) {
            const u32 overlap = mem_read32(&cpu, 0x803F6160u);
            const u32 request = overlap >= 0x80000000u ? overlap : 0u;
            const u32 task = request != 0u ? mem_read32(&cpu, request + 0x20u) : 0u;
            fprintf(stderr,
                    "[scene-overlap] pc=0x%08X r3=0x%08X "
                    "overlap=0x%08X req_flags=0x%02X field4=%u peek=%u "
                    "pid=0x%08X task=0x%08X task_flags=0x%02X "
                    "task_scene=0x%08X task_req_next=0x%08X "
                    "field_c=0x%08X blocks=%llu\n",
                    cpu.pc, cpu.gpr[3], overlap,
                    request ? mem_read8(&cpu, request + 0x00u) : 0u,
                    request ? mem_read16(&cpu, request + 0x04u) : 0u,
                    request ? mem_read16(&cpu, request + 0x06u) : 0u,
                    request ? mem_read32(&cpu, request + 0x14u) : 0u,
                    task, task ? mem_read8(&cpu, task + 0xC4u) : 0u,
                    task ? mem_read32(&cpu, task + 0xC8u) : 0u,
                    task ? mem_read32(&cpu, task + 0xC4u) : 0u,
                    request ? mem_read32(&cpu, request + 0x0Cu) : 0u,
                    (unsigned long long)blocks);
            scene_overlap_trace_reports++;
        }
        if (g_scene_overlap_trace &&
            scene_overlap_child_trace_reports < 256u &&
            ((cpu.pc >= 0x802235D4u && cpu.pc < 0x802237A4u) ||
             cpu.pc == 0x8002967Cu)) {
            const u32 overlap = mem_read32(&cpu, 0x803F6160u);
            const u32 request = overlap >= 0x80000000u ? overlap : 0u;
            const u32 task = request != 0u ? mem_read32(&cpu, request + 0x20u) : 0u;
            fprintf(stderr,
                    "[scene-overlap-child] pc=0x%08X r3=0x%08X "
                    "overlap=0x%08X req_flags=0x%02X task=0x%08X "
                    "task_flags=0x%02X task_scene=0x%08X blocks=%llu\n",
                    cpu.pc, cpu.gpr[3], overlap,
                    request ? mem_read8(&cpu, request + 0x00u) : 0u,
                    task, task ? mem_read8(&cpu, task + 0xC4u) : 0u,
                    task ? mem_read32(&cpu, task + 0xC8u) : 0u,
                    (unsigned long long)blocks);
            scene_overlap_child_trace_reports++;
        }
        if (g_create_iter_trace &&
            create_iter_trace_reports < 128u && blocks >= 40000000u &&
            (cpu.pc == 0x8024560Cu || cpu.pc == 0x80245640u)) {
            const u32 tag = cpu.gpr[3];
            const u32 filter = cpu.gpr[4];
            const u32 tag_data = tag >= 0x80000000u ? mem_read32(&cpu, tag + 0x0Cu) : 0u;
            const u32 judge = filter >= 0x80000000u ? mem_read32(&cpu, filter + 0x00u) : 0u;
            const u32 user = filter >= 0x80000000u ? mem_read32(&cpu, filter + 0x04u) : 0u;
            fprintf(stderr,
                    "[create-iter] pc=0x%08X tag=0x%08X tag_data=0x%08X "
                    "filter=0x%08X judge=0x%08X user=0x%08X "
                    "tag_next=0x%08X blocks=%llu\n",
                    cpu.pc, tag, tag_data, filter, judge, user,
                    tag >= 0x80000000u ? mem_read32(&cpu, tag + 0x08u) : 0u,
                    (unsigned long long)blocks);
            create_iter_trace_reports++;
        }
        if (room_create_phase_reports < 48u &&
            (cpu.pc == 0x80040648u || cpu.pc == 0x8004069Cu ||
             cpu.pc == 0x80040704u || cpu.pc == 0x8004073Cu ||
             cpu.pc == 0x80040794u || cpu.pc == 0x800407ECu) &&
            mem_read16(&cpu, cpu.gpr[3] + 0x50u) == 0x0014) {
            const u32 request = cpu.gpr[3];
            fprintf(stderr,
                    "[scene-room] phase pc=0x%08X request=0x%08X "
                    "phase_id=%d proc_name=%d result=0x%08X "
                    "res=0x%08X callback=0x%08X\n",
                    cpu.pc, request, (s32)mem_read32(&cpu, request + 0x4Cu),
                    (s32)mem_read16(&cpu, request + 0x50u),
                    mem_read32(&cpu, request + 0x40u),
                    mem_read32(&cpu, request + 0x28u),
                    mem_read32(&cpu, request + 0x58u));
            room_create_phase_reports++;
        }
        if (room_scene_phase_reports < 48u &&
            (cpu.pc == 0x802370B8u || cpu.pc == 0x802371D0u) &&
            cpu.gpr[3] >= 0x80000000u) {
            const u32 room_scene = cpu.gpr[3];
            const u32 room_request = mem_read32(&cpu, room_scene + 0x14u);
            const u32 room_proc = room_request ? mem_read32(&cpu, room_request + 0x40u) : 0u;
            const s32 room_no = (s32)mem_read32(&cpu, room_scene + 0xB0u);
            const s32 internal_phase = (s32)mem_read32(&cpu, room_scene + 0x1C8u);
            fprintf(stderr,
                    "[scene-room-owner] pc=0x%08X scene=0x%08X room=%d "
                    "internal_phase=%d command=0x%08X room_data=0x%08X "
                    "child_creating=%d request=0x%08X request_phase=%d "
                    "proc_name=%d init=%d result=%d prof=0x%08X\n",
                    cpu.pc, room_scene, room_no, internal_phase,
                    mem_read32(&cpu, room_scene + 0x1D4u),
                    mem_read32(&cpu, room_scene + 0x1CCu),
                    room_proc ? (s16)mem_read16(&cpu, room_proc + 0xE4u) : -1,
                    room_request,
                    room_request ? (s32)mem_read32(&cpu, room_request + 0x4Cu) : -1,
                    (s16)mem_read16(&cpu, room_scene + 0x08u),
                    (s8)mem_read8(&cpu, room_scene + 0x0Cu),
                    (s8)mem_read8(&cpu, room_scene + 0x0Du),
                    mem_read32(&cpu, room_scene + 0x10u));
            room_scene_phase_reports++;
            if (g_room0_trace && room_no == 0 &&
                room_scene_phase_reports < 48u) {
                const u32 parent_request = 0x80ACDFACu;
                fprintf(stderr,
                        "[scene-parent] request=0x%08X parameter=%d request_id=0x%08X "
                        "node=0x%08X node_id=0x%08X layer=0x%08X creating_id=0x%08X "
                        "proc_name=%d user=0x%08X fade_request=0x%08X "
                        "phase_id=%d phase_handler=0x%08X\n",
                        parent_request, (s32)mem_read32(&cpu, parent_request + 0x40u),
                        mem_read32(&cpu, parent_request + 0x44u),
                        mem_read32(&cpu, parent_request + 0x48u),
                        mem_read32(&cpu, parent_request + 0x4Cu),
                        mem_read32(&cpu, parent_request + 0x50u),
                        mem_read32(&cpu, parent_request + 0x54u),
                        (s32)mem_read16(&cpu, parent_request + 0x58u),
                        mem_read32(&cpu, parent_request + 0x5Cu),
                        mem_read32(&cpu, parent_request + 0x64u),
                        (s32)mem_read32(&cpu, parent_request + 0x6Cu),
                        mem_read32(&cpu, parent_request + 0x68u));
            }
            if (g_room0_trace && room0_actor_reports < 8u &&
                room_no == 0 && internal_phase >= 3) {
                u32 node = mem_read32(&cpu, 0x80372028u);
                u32 player = 0u;
                unsigned actors = 0u;
                for (unsigned i = 0; i < 512u && node >= 0x80000000u; ++i) {
                    const u32 actor = mem_read32(&cpu, node + 0x0Cu);
                    const u32 next = mem_read32(&cpu, node + 0x08u);
                    if (actor >= 0x80000000u) {
                        actors++;
                        if (mem_read16(&cpu, actor + 0x08u) == 0x00A9u) {
                            player = actor;
                            fprintf(stderr,
                                    "[room0-player] scene=0x%08X actor=0x%08X "
                                    "init=%d subtype=0x%08X actor_type=0x%08X "
                                    "method=0x%08X draw_tag=0x%08X draw_use=%u "
                                    "draw_prev=0x%08X draw_next=0x%08X actors=%u "
                                    "retraces=%u blocks=%llu\n",
                                    room_scene, actor,
                                    (s8)mem_read8(&cpu, actor + 0x0Cu),
                                    mem_read32(&cpu, actor + 0xB4u),
                                    mem_read32(&cpu, actor + 0xC0u),
                                    mem_read32(&cpu, actor + 0xA8u),
                                    actor + 0xD8u,
                                    mem_read8(&cpu, actor + 0xE8u),
                                    mem_read32(&cpu, actor + 0xD8u),
                                    mem_read32(&cpu, actor + 0xE0u), actors,
                                    mem_read32(&cpu, 0x803F7B3Cu),
                                    (unsigned long long)blocks);
                            break;
                        }
                    }
                    if (next == node || next < 0x80000000u)
                        break;
                    node = next;
                }
                if (player == 0u) {
                    fprintf(stderr,
                            "[room0-player] scene=0x%08X player=absent "
                            "actors=%u retraces=%u blocks=%llu\n",
                            room_scene, actors, mem_read32(&cpu, 0x803F7B3Cu),
                            (unsigned long long)blocks);
                }
                room0_actor_reports++;
            }
        }
        if (room_child_created_reports < 48u && cpu.pc == 0x8003D93Cu &&
            cpu.gpr[3] >= 0x80000000u) {
            const u32 layer = cpu.gpr[3];
            const u32 owner = mem_read32(&cpu, layer + 0x18u);
            if (owner >= 0x80000000u && mem_read16(&cpu, owner + 0x08u) == 20) {
                fprintf(stderr,
                        "[scene-room-child-created] layer=0x%08X owner=0x%08X "
                        "creating_before=%d\n",
                        layer, owner, (s16)mem_read16(&cpu, layer + 0x28u));
                room_child_created_reports++;
            }
        }
        if (g_bg_create_trace &&
            bg_create_count_reports < 128u &&
            (cpu.pc == 0x8003D92Cu || cpu.pc == 0x8003D93Cu) &&
            cpu.gpr[3] >= 0x80000000u) {
            const u32 layer = cpu.gpr[3];
            const u32 owner = mem_read32(&cpu, layer + 0x18u);
            const s32 creating = (s16)mem_read16(&cpu, layer + 0x28u);
            if (owner >= 0x80000000u && mem_read16(&cpu, owner + 0x08u) == 20 &&
                creating <= 4) {
                fprintf(stderr,
                        "[scene-bg-create-count] pc=0x%08X layer=0x%08X "
                        "owner=0x%08X creating_before=%d room=%d "
                        "current=0x%08X lr=0x%08X blocks=%llu\n",
                        cpu.pc, layer, owner, creating,
                        (s32)mem_read32(&cpu, owner + 0xB0u),
                        mem_read32(&cpu, 0x800000E4u), cpu.lr,
                        (unsigned long long)blocks);
                bg_create_count_reports++;
            }
        }
        if (room_pending_phase_reports < 96u &&
            (cpu.pc == 0x80040648u || cpu.pc == 0x8004069Cu ||
             cpu.pc == 0x80040704u || cpu.pc == 0x8004073Cu ||
             cpu.pc == 0x80040794u || cpu.pc == 0x800407ECu) &&
            cpu.gpr[3] >= 0x80000000u) {
            const u32 pending_request = cpu.gpr[3];
            const u32 layer = mem_read32(&cpu, pending_request + 0x44u);
            const u32 owner = layer ? mem_read32(&cpu, layer + 0x18u) : 0u;
            if (owner >= 0x80000000u && mem_read16(&cpu, owner + 0x08u) == 20) {
                fprintf(stderr,
                        "[scene-room-pending] request=0x%08X proc_name=%d "
                        "phase_id=%d res=0x%08X layer=0x%08X owner=0x%08X "
                        "creating=%d retraces=%llu\n",
                        pending_request, (s32)mem_read16(&cpu, pending_request + 0x50u),
                        (s32)mem_read32(&cpu, pending_request + 0x4Cu),
                        mem_read32(&cpu, pending_request + 0x40u), layer, owner,
                        (s16)mem_read16(&cpu, layer + 0x28u),
                        (unsigned long long)g_host_retrace_count);
                room_pending_phase_reports++;
            }
        }
        if (room_final_pending_reports < 32u &&
            (cpu.pc == 0x80040648u || cpu.pc == 0x8004069Cu ||
             cpu.pc == 0x80040704u || cpu.pc == 0x8004073Cu ||
             cpu.pc == 0x80040794u || cpu.pc == 0x800407ECu) &&
            cpu.gpr[3] >= 0x80000000u) {
            const u32 pending_request = cpu.gpr[3];
            const u32 layer = mem_read32(&cpu, pending_request + 0x44u);
            const u32 owner = layer ? mem_read32(&cpu, layer + 0x18u) : 0u;
            const s32 creating = layer ? (s16)mem_read16(&cpu, layer + 0x28u) : -1;
            if (creating >= 0 && creating <= 4 && owner >= 0x80000000u &&
                mem_read16(&cpu, owner + 0x08u) == 20) {
                const u32 process = mem_read32(&cpu, pending_request + 0x40u);
                const u32 methods = process ? mem_read32(&cpu, process + 0xA8u) : 0u;
                fprintf(stderr,
                        "[scene-room-final-pending] request=0x%08X proc_name=%d "
                        "phase_pc=0x%08X phase_id=%d res=0x%08X creating=%d "
                        "methods=0x%08X create=0x%08X proc_mtd=0x%08X "
                        "prof=0x%08X prof_name=0x%08X prof_method=0x%08X "
                        "node_mtd=0x%08X params=0x%08X\n",
                        pending_request, (s32)mem_read16(&cpu, pending_request + 0x50u),
                        cpu.pc, (s32)mem_read32(&cpu, pending_request + 0x4Cu),
                        process, creating, methods, methods ? mem_read32(&cpu, methods) : 0u,
                        mem_read32(&cpu, process + 0xA8u), mem_read32(&cpu, process + 0x10u),
                        mem_read32(&cpu, mem_read32(&cpu, process + 0x10u) + 0x08u),
                        mem_read32(&cpu, mem_read32(&cpu, process + 0x10u) + 0x0Cu),
                        mem_read32(&cpu, process + 0xB8u), mem_read32(&cpu, process + 0xB0u));
                room_final_pending_reports++;
            }
        }
        if (scene_request_reports < 48u &&
            (cpu.pc == 0x80040648u || cpu.pc == 0x8004069Cu ||
             cpu.pc == 0x80040704u || cpu.pc == 0x8004073Cu ||
             cpu.pc == 0x80040794u || cpu.pc == 0x800407ECu ||
             cpu.pc == 0x80022B58u || cpu.pc == 0x80022CECu ||
             cpu.pc == 0x80022DF8u || cpu.pc == 0x80022E70u)) {
            fprintf(stderr,
                    "[rel] loader pc=0x%08X r3=0x%08X r4=0x%08X "
                    "r5=0x%08X lr=0x%08X profile_ptr=0x%08X "
                    "rel_data0=0x%08X rel_data5=0x%08X\n",
                    cpu.pc, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.lr,
                    mem_read32(&cpu, 0x803F6A68u),
                    mem_read32(&cpu, 0x81F80178u),
                    mem_read32(&cpu, 0x81F80178u + 5u * 4u));
            scene_request_reports++;
        }
        if (g_dyl_state_reports < 12u &&
            (cpu.pc == 0x80022A80u || cpu.pc == 0x80022B58u ||
             cpu.pc == 0x80022CECu || cpu.pc == 0x80022DF8u)) {
            fprintf(stderr,
                    "[dyl-state] pc=0x%08X r3=0x%08X lr=0x%08X "
                    "DMC_initialized=0x%08X cDyl_Initialized=0x%08X "
                    "guest_r13=0x%08X guest_flag_addr=0x%08X "
                    "guest_flag=0x%08X profile_ptr=0x%08X\n",
                    cpu.pc, cpu.gpr[3], cpu.lr, mem_read32(&cpu, 0x803F69C0u),
                    mem_read32(&cpu, 0x803F69C4u), cpu.gpr[13],
                    cpu.gpr[13] - 30492u,
                    mem_read32(&cpu, cpu.gpr[13] - 30492u),
                    mem_read32(&cpu, 0x803F6A68u));
            g_dyl_state_reports++;
        }
        if (!ppc_halt_reported &&
            (cpu.pc == 0x8030150Cu || cpu.pc == 0x80301510u)) {
            fprintf(stderr,
                    "[halt] PPCHalt entry pc=0x%08X lr=0x%08X r1=0x%08X "
                    "r3=0x%08X msr=0x%08X exception=0x%08X "
                    "srr0=0x%08X current_thread=0x%08X current_context=0x%08X "
                    "run_bits=0x%08X\n",
                    cpu.pc, cpu.lr, cpu.gpr[1], cpu.gpr[3], cpu.msr,
                    cpu.exception, cpu.srr0, mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u), mem_read32(&cpu, 0x803F7A30u));
            u32 frame = cpu.gpr[1];
            for (unsigned depth = 0; depth < 8u && frame >= 0x80000000u; ++depth) {
                const u32 next = mem_read32(&cpu, frame);
                fprintf(stderr, "[halt] frame%u sp=0x%08X back=0x%08X lr=0x%08X\n",
                        depth, frame, next, mem_read32(&cpu, frame + 4u));
                if (next <= frame || next >= 0x82000000u)
                    break;
                frame = next;
            }
            ppc_halt_reported = true;
        }
        if (!os_panic_reported && cpu.pc == 0x80006C4Cu) {
            char panic_file[160] = {0};
            char panic_format[256] = {0};
            if (cpu.gpr[3] != 0u)
                guest_read_cstr(&cpu, cpu.gpr[3], panic_file,
                                sizeof(panic_file));
            if (cpu.gpr[5] != 0u)
                guest_read_cstr(&cpu, cpu.gpr[5], panic_format,
                                sizeof(panic_format));
            fprintf(stderr,
                    "[panic] OSPanic file=0x%08X line=%u format=0x%08X "
                    "r6=0x%08X r7=0x%08X r8=0x%08X lr=0x%08X "
                    "pc=0x%08X retraces=%llu blocks=%llu "
                    "r26=0x%08X r27=0x%08X r28=0x%08X r29=0x%08X "
                    "r30=0x%08X r31=0x%08X file=\"%s\" format_text=\"%s\"\n",
                    cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.gpr[6],
                    cpu.gpr[7], cpu.gpr[8], cpu.lr, cpu.pc,
                    (unsigned long long)g_host_retrace_count,
                    (unsigned long long)blocks, cpu.gpr[26], cpu.gpr[27],
                    cpu.gpr[28], cpu.gpr[29], cpu.gpr[30], cpu.gpr[31],
                    panic_file, panic_format);
            os_panic_reported = true;
        }
        if (!os_panic_callsite_reported && cpu.pc == 0x80F01794u) {
            fprintf(stderr,
                    "[panic] callsite pc=0x%08X r3=0x%08X r4=0x%08X "
                    "r5=0x%08X r6=0x%08X r7=0x%08X r8=0x%08X "
                    "r31=0x%08X lr=0x%08X\n",
                    cpu.pc, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.gpr[6],
                    cpu.gpr[7], cpu.gpr[8], cpu.gpr[31], cpu.lr);
            os_panic_callsite_reported = true;
        }
        if (os_panic_vcall_reports < 16u && cpu.pc == 0x80240ED8u) {
            const u32 object = mem_read32(&cpu, cpu.gpr[31] + 16u);
            if (object == 0x81E04080u) {
                const u32 section_info = mem_read32(&cpu, object + 0x10u);
                const u32 section1 = mem_read32(&cpu, section_info + 8u);
                const u32 target = mem_read32(&cpu, object + 52u);
                fprintf(stderr,
                        "[panic] vcall-load pc=0x%08X r31=0x%08X object=0x%08X "
                        "section_info=0x%08X section1=0x%08X prolog_section=%u "
                        "prolog=0x%08X epilog=0x%08X target=0x%08X fix=0x%08X "
                        "lr=0x%08X\n",
                        cpu.pc, cpu.gpr[31], object, section_info, section1,
                        mem_read8(&cpu, object + 0x30u), mem_read32(&cpu, object + 0x34u),
                        mem_read32(&cpu, object + 0x38u), target,
                        mem_read32(&cpu, object + 0x60u), cpu.lr);
                os_panic_vcall_reports++;
            }
        }
        if (!os_panic_bad_header_reported && cpu.pc == 0x80240ED8u) {
            const u32 object = mem_read32(&cpu, cpu.gpr[31] + 16u);
            const u32 target = mem_read32(&cpu, object + 52u);
            if (target == 0x80F01794u) {
                const u32 section_info = mem_read32(&cpu, object + 0x10u);
                fprintf(stderr,
                        "[panic] bad-header object=0x%08X id=%u "
                        "section_info=0x%08X prolog_section=%u "
                        "prolog=0x%08X epilog=0x%08X target=0x%08X "
                        "r31=0x%08X\n",
                        object, mem_read32(&cpu, object), section_info,
                        mem_read8(&cpu, object + 0x30u),
                        mem_read32(&cpu, object + 0x34u),
                        mem_read32(&cpu, object + 0x38u), target, cpu.gpr[31]);
                os_panic_bad_header_reported = true;
            }
        }
        if (resource_set_reports < 4u && cpu.pc == 0x8006EF34u) {
            char resource_name[160] = {0};
            if (cpu.gpr[3] != 0u)
                guest_read_cstr(&cpu, cpu.gpr[3], resource_name,
                                sizeof(resource_name));
            fprintf(stderr,
                    "[res] dRes_control::setRes entry #%u r3=0x%08X r4=0x%08X "
                    "r5=0x%08X r6=0x%08X r7=0x%08X r8=0x%08X lr=0x%08X\n",
                    resource_set_reports + 1u, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5],
                    cpu.gpr[6], cpu.gpr[7], cpu.gpr[8], cpu.lr);
            fprintf(stderr, "[res] setRes archive=\"%s\" path=0x%08X\n",
                    resource_name, cpu.gpr[6]);
            for (u32 index = 0; index < 64u; index++) {
                const u32 info = cpu.gpr[4] + index * 0x24u;
                const u16 count = mem_read16(&cpu, info + 0x0Eu);
                if (count != 0u || index < 4u) {
                    char archive_name[20] = {0};
                    for (u32 byte = 0; byte < 14u; byte++)
                        archive_name[byte] =
                            (char)mem_read8(&cpu, info + byte);
                    fprintf(stderr,
                            "[res] info[%u] count=%u archive=\"%s\" "
                            "command=0x%08X archive_ptr=0x%08X\n",
                            index, count, archive_name,
                            mem_read32(&cpu, info + 0x10u),
                            mem_read32(&cpu, info + 0x14u));
                }
            }
            resource_set_reports++;
        }
        if (cpu.pc == 0x8030F618u || cpu.pc == 0x8030F5A4u) {
            char path[256];
            const bool fast_open = cpu.pc == 0x8030F5A4u;
            const u32 file_info = fast_open ? cpu.gpr[4] : cpu.gpr[4];
            s32 entry = fast_open ? (s32)cpu.gpr[3] : -1;
            if (!fast_open) {
                guest_read_cstr(&cpu, cpu.gpr[3], path, sizeof(path));
                entry = dvd_path_to_entrynum(path);
            } else {
                path[0] = '\0';
            }
            const bool opened = host_dvd_fill_file_info(&cpu, entry, file_info);
            cpu.gpr[3] = opened ? 1u : 0u;
            cpu.pc = cpu.lr & ~3u;
            if (g_dvd_open_reports < 16u) {
                fprintf(stderr,
                        "[dvd] %s #%u path=\"%s\" entry=%d file=0x%08X "
                        "length=%u result=%u return=0x%08X\n",
                        fast_open ? "fast-open" : "open",
                        g_dvd_open_reports + 1u, path, entry, file_info,
                        opened ? mem_read32(&cpu, file_info + DVD_FI_LENGTH) : 0u,
                        opened ? 1u : 0u, cpu.pc);
                g_dvd_open_reports++;
            }
        }
        if (cpu.pc == 0x80017FD8u) {
            char path[256] = {0};
            if (cpu.gpr[3] != 0u)
                guest_read_cstr(&cpu, cpu.gpr[3], path, sizeof(path));
            const s32 entry = dvd_path_to_entrynum(path);
            cpu.gpr[3] = (u32)entry;
            cpu.pc = cpu.lr & ~3u;
            fprintf(stderr,
                    "[dvd] archive-path path=\"%s\" entry=%d return=0x%08X\n",
                    path, entry, cpu.pc);
        }
        if (cpu.pc == 0x80018554u || cpu.pc == 0x8001199Cu) {
            char path[256] = {0};
            if (cpu.pc == 0x80018554u && cpu.gpr[3] != 0u)
                guest_read_cstr(&cpu, cpu.gpr[3], path, sizeof(path));
            if (!g_new_game_intro_reported &&
                strcmp(path, "/res/Object/Opening.arc") == 0) {
                g_new_game_intro_reported = true;
                g_new_game_intro_retrace = g_host_retrace_count;
                fprintf(stderr,
                        "[boot-milestone] new-game-intro retrace=%llu "
                        "blocks=%llu resource=\"%s\"\n",
                        (unsigned long long)g_new_game_intro_retrace,
                        (unsigned long long)blocks, path);
            }
            if (g_opening_complete_reported && !g_outset_room_requested &&
                strcmp(path, "/res/Stage/sea_T/Room44.arc") == 0) {
                g_outset_room_requested = true;
                g_outset_room_request_retrace = g_host_retrace_count;
                fprintf(stderr,
                        "[boot-milestone] outset-room-request retrace=%llu "
                        "blocks=%llu resource=\"%s\"\n",
                        (unsigned long long)g_outset_room_request_retrace,
                        (unsigned long long)blocks, path);
            }
            fprintf(stderr,
                    "[dvd] mount-create pc=0x%08X r3=0x%08X r4=0x%08X "
                    "r5=0x%08X path=\"%s\" lr=0x%08X\n",
                    cpu.pc, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], path, cpu.lr);
        }
        if (!opening_capture_requested && aurora_enabled &&
            opening_capture_path != NULL && opening_capture_path[0] != '\0' &&
            diagnostic_capture_retrace != 0u &&
            g_host_retrace_count >= diagnostic_capture_retrace) {
            aurora_request_framebuffer_readback();
            opening_capture_requested = true;
            fprintf(stderr,
                    "[frame-capture] diagnostic requested retrace=%llu "
                    "path=\"%s\"\n",
                    (unsigned long long)g_host_retrace_count,
                    opening_capture_path);
        }
        if (opening_capture_requested && !opening_capture_complete) {
            const u8* rgba = NULL;
            u32 width = 0u;
            u32 height = 0u;
            if (aurora_take_framebuffer_readback(&rgba, &width, &height)) {
                capture_samples++;
                u64 hash = 1469598103934665603ull;
                u64 nonblank_pixels = 0u;
                const u64 byte_count = (u64)width * height * 4u;
                for (u64 i = 0u; i < byte_count; i++) {
                    hash ^= rgba[i];
                    hash *= 1099511628211ull;
                }
                for (u64 pixel = 0u; pixel < (u64)width * height; pixel++) {
                    const u8* color = rgba + pixel * 4u;
                    if (color[0] != 0u || color[1] != 0u || color[2] != 0u)
                        nonblank_pixels++;
                }
                const bool keep_monitoring =
                    capture_first_nonblank && nonblank_pixels == 0u;
                char capture_path[1024];
                const char* capture_path_used = opening_capture_path;
                // Player-ready frames carry the retrace in their name too: two
                // of them are taken so one bad frame cannot stand in for the
                // picture, and they must not overwrite one another.
                if (diagnostic_capture_interval != 0u || capture_player_ready) {
                    host_capture_path_for_retrace(
                        opening_capture_path, g_host_retrace_count,
                        capture_path, sizeof(capture_path));
                    capture_path_used = capture_path;
                }
                const bool written = !keep_monitoring && write_rgba_ppm(
                    capture_path_used, rgba, width, height);
                fprintf(stderr,
                        "[frame-capture] sample=%llu retrace=%llu "
                        "width=%u height=%u "
                        "nonblank=%llu hash=0x%016llX written=%u "
                        "continue=%u path=\"%s\"\n",
                        (unsigned long long)capture_samples,
                        (unsigned long long)g_host_retrace_count, width, height,
                        (unsigned long long)nonblank_pixels,
                        (unsigned long long)hash, written ? 1u : 0u,
                        keep_monitoring ? 1u : 0u,
                        capture_path_used);
                if (keep_monitoring) {
                    aurora_request_framebuffer_readback();
                } else if (diagnostic_capture_interval != 0u) {
                    opening_capture_requested = false;
                    diagnostic_capture_retrace =
                        g_host_retrace_count + diagnostic_capture_interval;
                } else if (player_ready_capture_followups > 0u) {
                    // One later frame, so the pair brackets the control moment
                    // rather than sampling a single instant of it. It used to
                    // be 120 retraces later, on the reasoning that a transition
                    // black would be gone by then, and that was 75 retraces too
                    // late: the acceptance harness presses RETURN at the play
                    // scene as soon as its displacement clause is measured, and
                    // on the v48 run that was retrace 20,089 against control at
                    // 20,044. The second frame was therefore a picture of the
                    // pause menu, and the clause could not tell, because it only
                    // checks that the frame is a render rather than an empty
                    // buffer. The first frame at +2 was already a clean picture
                    // with no transition black in it, which is what retires the
                    // original reasoning; 20 retraces keeps the pair inside the
                    // window before the harness acts again.
                    player_ready_capture_followups--;
                    opening_capture_requested = false;
                    diagnostic_capture_retrace = g_host_retrace_count + 20u;
                } else {
                    opening_capture_complete = true;
                }
            }
        }
        if (cpu.pc == 0x8030FADCu || cpu.pc == 0x8030FBCCu) {
            const bool synchronous = cpu.pc == 0x8030FBCCu;
            const u32 file_info = cpu.gpr[3];
            const u32 address = cpu.gpr[4];
            const u32 length = cpu.gpr[5];
            const u32 offset = cpu.gpr[6];
            const u32 callback = cpu.gpr[7];
            const u32 file_start = mem_read32(&cpu, file_info + DVD_FI_STARTADDR);
            const u32 file_length = mem_read32(&cpu, file_info + DVD_FI_LENGTH);
            const u32 callback_data = mem_read32(&cpu, file_info + 0x3Cu);
            const bool valid = file_info != 0u && address != 0u &&
                               offset <= file_length &&
                               length <= file_length - offset + DVD_MIN_TRANSFER_SIZE;
            const u64 read_end = (u64)address + (u64)length;
            const bool dvd_heap_overlap =
                valid && address < 0x80ADE020u && read_end > 0x80AD2140u;
            if (dvd_heap_overlap && g_runqueue_trace) {
                fprintf(stderr,
                        "[dvd-heap-write] before pc=0x%08X guest=0x%08X "
                        "length=%u disc=0x%08X nominal_size=0x%08X "
                        "nominal_prev=0x%08X nominal_next=0x%08X\n",
                        cpu.pc, address, length, file_start + offset,
                        mem_read32(&cpu, 0x80AD2144u),
                        mem_read32(&cpu, 0x80AD2148u),
                        mem_read32(&cpu, 0x80AD214Cu));
            }
            if (valid)
                dvd_read_to_guest(&cpu, address, file_start + offset, length);
            if (dvd_heap_overlap && g_runqueue_trace) {
                fprintf(stderr,
                        "[dvd-heap-write] after nominal_size=0x%08X "
                        "nominal_prev=0x%08X nominal_next=0x%08X\n",
                        mem_read32(&cpu, 0x80AD2144u),
                        mem_read32(&cpu, 0x80AD2148u),
                        mem_read32(&cpu, 0x80AD214Cu));
            }
            mem_write32(&cpu, file_info + DVD_CB_STATE, 0u);
            mem_write32(&cpu, file_info + DVD_CB_CURRXFER, valid ? length : 0u);
            mem_write32(&cpu, file_info + DVD_CB_XFERRED, valid ? length : 0u);
            cpu.gpr[3] = synchronous ? (valid ? length : 0u) : (valid ? 1u : 0u);
            cpu.pc = cpu.lr & ~3u;
            if (!synchronous && valid && callback != 0u) {
                if (g_dvd_read_reports < 4u)
                    fprintf(stderr,
                            "[dvd] callback-enter callback=0x%08X result=%u "
                            "file=0x%08X data=0x%08X\n",
                            callback, length, file_info, callback_data);
                const BluewakeCallbackDeliveryResult delivery =
                    bluewake_deliver_guest_callback(&cpu, mod, callback,
                                                    length, file_info, 4096u,
                                                    host_prepare_guest_dispatch,
                                                    &g_cycle_domain);
                if (!delivery.completed) {
                    fprintf(stderr,
                            "[dvd] callback incomplete callback=0x%08X pc=0x%08X "
                            "steps=%u exception=0x%08X srr0=0x%08X "
                            "queue=0x%08X qfirst=0x%08X qused=%u\n",
                            callback, delivery.terminal_pc,
                            delivery.dispatches, delivery.exception,
                            cpu.srr0, callback_data + 0xC0u,
                            mem_read32(&cpu, callback_data + 0xC0u),
                            mem_read32(&cpu, callback_data + 0xC0u + 0x1Cu));
                    stop_reason = "callback delivery";
                }
            }
            if (g_dvd_read_reports < 16u) {
                fprintf(stderr,
                        "[dvd] %s #%u guest=0x%08X length=%u offset=0x%08X "
                        "disc=0x%08X valid=%u callback=0x%08X file=0x%08X "
                        "user=0x%08X return=0x%08X\n",
                        synchronous ? "read" : "read-async",
                        g_dvd_read_reports + 1u, address, length, offset,
                        file_start + offset, valid ? 1u : 0u, callback, file_info,
                        callback_data, cpu.pc);
                g_dvd_read_reports++;
            }
        }
        if (!post_gx_thread_snapshot_reported && blocks >= 5000000ull &&
            cpu.pc == 0x80307EF4u) {
            fprintf(stderr, "[sched] post-GX thread snapshot blocks=%llu\n",
                    blocks);
            static const u32 threads[] = {
                0x803F06D0u, 0x803A2960u, 0x804211E0u, 0x80425680u,
                0x80429AA0u, 0x804CE100u, 0x803A5F90u, 0x803A7320u,
            };
            for (u32 index = 0; index < sizeof(threads) / sizeof(threads[0]);
                 index++) {
                const u32 thread = threads[index];
                const u16 state = mem_read16(&cpu, thread + 0x2C8u);
                const u32 queue = mem_read32(&cpu, thread + 0x2DCu);
                const u32 saved_pc = mem_read32(&cpu, thread + 0x198u);
                fprintf(stderr,
                        "[sched] thread addr=0x%08X state=%u queue=0x%08X "
                        "saved_pc=0x%08X sp=0x%08X\n",
                        thread, state, queue, saved_pc,
                        mem_read32(&cpu, thread + 0x04u));
            }
            post_gx_thread_snapshot_reported = true;
        }
        if (!dvd_thread_snapshot_reported && cpu.pc == 0x80307EF4u) {
            const u32 param = 0x803A72C0u;
            const u32 queue = param;
            const u32 list = param + 0x24u;
            const u32 command = mem_read32(&cpu, list);
            fprintf(stderr,
                    "[dvd] thread snapshot param=0x%08X queue="
                    "0x%08X first=%u used=%u send=0x%08X recv=0x%08X "
                    "list_head=0x%08X list_tail=0x%08X list_size=%d\n",
                    param, queue, mem_read32(&cpu, queue + 0x18u),
                    mem_read32(&cpu, queue + 0x1Cu), mem_read32(&cpu, queue),
                    mem_read32(&cpu, queue + 0x8u), command,
                    mem_read32(&cpu, list + 0x4u),
                    (s32)mem_read32(&cpu, list + 0x8u));
            const u32 jas_thread = mem_read32(&cpu, 0x803F7428u);
            fprintf(stderr,
                    "[dvd] jas-thread ptr=0x%08X message_queue=0x%08X "
                    "used=%u recv=0x%08X thread_record=0x%08X\n",
                    jas_thread, jas_thread + 0x30u,
                    mem_read32(&cpu, jas_thread + 0x30u + 0x1Cu),
                    mem_read32(&cpu, jas_thread + 0x30u + 0x8u),
                    mem_read32(&cpu, jas_thread + 0x2Cu));
            fprintf(stderr,
                    "[dvd] sync-with-sound=%u current_thread=0x%08X "
                    "current_context=0x%08X dvd_thread_state=%u "
                    "dvd_wait=0x%08X\n",
                    mem_read8(&cpu, 0x803F6970u), mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read16(&cpu, 0x803A5F90u + 0x2C8u),
                    mem_read32(&cpu, 0x803A5F90u + 0x2DCu));
            if (command != 0u) {
                fprintf(stderr,
                        "[dvd] command head=0x%08X done=%u entry=%d "
                        "archive=0x%08X heap=0x%08X prev=0x%08X next=0x%08X\n",
                        command, mem_read8(&cpu, command + 0x0Cu),
                        (s32)mem_read32(&cpu, command + 0x18u),
                        mem_read32(&cpu, command + 0x1Cu),
                        mem_read32(&cpu, command + 0x20u),
                        mem_read32(&cpu, command),
                        mem_read32(&cpu, command + 0x8u));
            }
            dvd_thread_snapshot_reported = true;
        }
        if (jas_dvd_trace_reports < 16u &&
            (cpu.pc == 0x8027B5D8u || cpu.pc == 0x8027B66Cu ||
             cpu.pc == 0x8027B84Cu)) {
            fprintf(stderr,
                    "[dvd] jas-trace pc=0x%08X r3=0x%08X r4=0x%08X "
                    "r5=0x%08X lr=0x%08X sThread=0x%08X\n",
                    cpu.pc, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.lr,
                    mem_read32(&cpu, 0x803F7428u));
            jas_dvd_trace_reports++;
        }
        if (dvd_thread_trace_reports < 32u &&
            (cpu.pc == 0x800180F0u || cpu.pc == 0x80018120u ||
             cpu.pc == 0x80018178u || cpu.pc == 0x8001821Cu ||
             cpu.pc == 0x80018108u || cpu.pc == 0x80018118u ||
             cpu.pc == 0x80018430u || cpu.pc == 0x8001861Cu)) {
            fprintf(stderr,
                    "[dvd] thread-trace pc=0x%08X r3=0x%08X r4=0x%08X "
                    "r5=0x%08X lr=0x%08X queue_used=%u recv=0x%08X "
                    "list_head=0x%08X list_size=%d\n",
                    cpu.pc, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.lr,
                    mem_read32(&cpu, 0x803A72C0u + 0x1Cu),
                    mem_read32(&cpu, 0x803A72C0u + 0x8u),
                    mem_read32(&cpu, 0x803A72C0u + 0x24u),
                    (s32)mem_read32(&cpu, 0x803A72C0u + 0x2Cu));
            dvd_thread_trace_reports++;
        }
        if (g_audio_object_watch && g_audio_message_reports < 24u &&
            (cpu.pc == 0x80305908u || cpu.pc == 0x803059D0u) &&
            cpu.gpr[3] == 0x803EA580u) {
            fprintf(stderr,
                    "[audio-msg] pc=0x%08X queue=0x%08X message=0x%08X "
                    "flags=%u used=%u capacity=%u mq_recv_head=0x%08X "
                    "mq_recv_tail=0x%08X current_thread=0x%08X "
                    "audio_state=%u audio_queue=0x%08X run_bits=0x%08X\n",
                    cpu.pc, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5],
                    mem_read32(&cpu, cpu.gpr[3] + 0x1Cu),
                    mem_read32(&cpu, cpu.gpr[3] + 0x14u),
                    mem_read32(&cpu, cpu.gpr[3] + 0x8u),
                    mem_read32(&cpu, cpu.gpr[3] + 0xCu),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read16(&cpu, 0x803E9260u + 0x2C8u),
                    mem_read32(&cpu, 0x803E9260u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7A30u));
            g_audio_message_reports++;
            if (cpu.pc == 0x80305908u)
                audio_message_send_reported = true;
        }
        if (g_audio_object_watch && g_audio_sync_reports < 16u &&
            (cpu.pc == 0x80289130u || cpu.pc == 0x80289140u ||
             cpu.pc == 0x80289190u || cpu.pc == 0x802891ACu ||
             cpu.pc == 0x802891B0u)) {
            const u32 init_address = cpu.gpr[13] - 27640u;
            fprintf(stderr,
                    "[audio-sync] pc=0x%08X lr=0x%08X r3=0x%08X r4=0x%08X "
                    "r5=0x%08X r13=0x%08X mq_init_addr=0x%08X "
                    "mq_init=%u\n",
                    cpu.pc, cpu.lr, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5],
                    cpu.gpr[13], init_address, mem_read32(&cpu, init_address));
            g_audio_sync_reports++;
        }
        if (message_trace_reports < 64u &&
            (((cpu.pc == 0x80305908u || cpu.pc == 0x803059D0u ||
              cpu.pc == 0x80305A08u || cpu.pc == 0x80305A28u ||
             cpu.pc == 0x80305A7Cu) &&
             (cpu.gpr[3] == 0x803A72C0u || cpu.gpr[31] == 0x803A72C0u ||
              cpu.gpr[3] == 0x806AD740u || cpu.gpr[3] == 0x8039CD60u ||
              cpu.gpr[3] == 0x8039CDC0u)) ||
             cpu.pc == 0x80308B88u)) {
            fprintf(stderr,
                    "[osmsg] pc=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X "
                    "lr=0x%08X used=%u recv_head=0x%08X recv_tail=0x%08X "
                    "thread=0x%08X state=%u wait=0x%08X\n",
                    cpu.pc, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.lr,
                    mem_read32(&cpu, cpu.gpr[3] + 0x1Cu),
                    mem_read32(&cpu, cpu.gpr[3] + 0x8u),
                    mem_read32(&cpu, cpu.gpr[3] + 0xCu),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read16(&cpu, mem_read32(&cpu, 0x800000E4u) + 0x2C8u),
                    mem_read32(&cpu, mem_read32(&cpu, 0x800000E4u) + 0x2DCu));
            message_trace_reports++;
        }
        if (g_runqueue_trace && cpu.pc == 0x803059D0u &&
            message_trace_reports < 100u) {
            fprintf(stderr,
                    "[osmsg] receive-any mq=0x%08X out=0x%08X flags=%u "
                    "used=%u capacity=%u recv_head=0x%08X recv_tail=0x%08X "
                    "thread=0x%08X state=%u wait=0x%08X lr=0x%08X r1=0x%08X\n",
                    cpu.gpr[3], cpu.gpr[4], cpu.gpr[5],
                    mem_read32(&cpu, cpu.gpr[3] + 0x1Cu),
                    mem_read32(&cpu, cpu.gpr[3] + 0x14u),
                    mem_read32(&cpu, cpu.gpr[3] + 0x8u),
                    mem_read32(&cpu, cpu.gpr[3] + 0xCu),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read16(&cpu, 0x803A5F90u + 0x2C8u),
                    mem_read32(&cpu, 0x803A5F90u + 0x2DCu), cpu.lr, cpu.gpr[1]);
            message_trace_reports++;
        }
        if (g_runqueue_trace &&
            aram_stream_queue_trace_reports < 48u &&
            (cpu.pc == 0x80305908u || cpu.pc == 0x803059D0u) &&
            cpu.gpr[3] >= 0x80AD0000u && cpu.gpr[3] < 0x80AE0000u &&
            cpu.gpr[3] != 0x80ADBEA8u) {
            const u32 queue = cpu.gpr[3];
            fprintf(stderr,
                    "[aram] stream-queue #%u pc=0x%08X queue=0x%08X "
                    "message=0x%08X flags=%u used=%u capacity=%u "
                    "recv_head=0x%08X recv_tail=0x%08X thread=0x%08X "
                    "lr=0x%08X command=0x%08X\n",
                    aram_stream_queue_trace_reports + 1u, cpu.pc, queue,
                    cpu.gpr[4], cpu.gpr[5], mem_read32(&cpu, queue + 0x1Cu),
                    mem_read32(&cpu, queue + 0x14u),
                    mem_read32(&cpu, queue + 0x8u),
                    mem_read32(&cpu, queue + 0xCu),
                    mem_read32(&cpu, 0x800000E4u), cpu.lr,
                    queue - 0x2Cu);
            aram_stream_queue_trace_reports++;
        }
        if (g_runqueue_trace &&
            aram_stream_sync_trace_reports < 24u &&
            (cpu.pc == 0x802B6650u || cpu.pc == 0x802B667Cu ||
             cpu.pc == 0x802B66A0u)) {
            fprintf(stderr,
                    "[aram] stream-sync #%u pc=0x%08X command=0x%08X "
                    "message=0x%08X result=0x%08X lr=0x%08X "
                    "thread=0x%08X\n",
                    aram_stream_sync_trace_reports + 1u, cpu.pc, cpu.gpr[31],
                    cpu.gpr[0], cpu.gpr[3], cpu.lr,
                    mem_read32(&cpu, 0x800000E4u));
            aram_stream_sync_trace_reports++;
        }
        if (g_runqueue_trace &&
            aram_ripper_sync_trace_reports < 24u &&
            (cpu.pc == 0x802BDFA4u || cpu.pc == 0x802BDFE0u ||
             cpu.pc == 0x802BDFE8u ||
             cpu.pc == 0x802BE018u || cpu.pc == 0x802BE058u ||
             cpu.pc == 0x802BE064u || cpu.pc == 0x802BE074u)) {
            const u32 command = cpu.gpr[29];
            const u32 dvd_file = cpu.gpr[31];
            if (dvd_file + 0x34u == 0x803A7024u) {
                aram_ripper_command_mutex_a = command;
                aram_ripper_dvd_mutex_a = dvd_file;
            } else if (dvd_file + 0x34u == 0x806AD6B4u) {
                aram_ripper_command_mutex_b = command;
                aram_ripper_dvd_mutex_b = dvd_file;
            }
            fprintf(stderr,
                    "[aram] ripper-sync #%u pc=0x%08X command=0x%08X "
                    "dvd_file=0x%08X stream_command=0x%08X param=%u result=0x%08X "
                    "field44=0x%08X link_prev=0x%08X link_next=0x%08X "
                    "file_stream=0x%08X file_thread=0x%08X "
                    "lr=0x%08X thread=0x%08X\n",
                    aram_ripper_sync_trace_reports + 1u, cpu.pc, command,
                    dvd_file, mem_read32(&cpu, command + 0x4Cu), cpu.gpr[30],
                    cpu.gpr[3], mem_read32(&cpu, command + 0x44u),
                    mem_read32(&cpu, command + 0x00u),
                    mem_read32(&cpu, command + 0x04u),
                    mem_read32(&cpu, dvd_file + 0x54u),
                    mem_read32(&cpu, dvd_file + 0x50u), cpu.lr,
                    mem_read32(&cpu, 0x800000E4u));
            aram_ripper_sync_trace_reports++;
        }
        if (g_runqueue_trace && cpu.pc == 0x80305908u &&
            (cpu.gpr[3] == 0x8039CDC0u || cpu.gpr[3] == 0x8039CD60u ||
             cpu.gpr[3] == 0x80ADDF44u ||
             cpu.gpr[3] == 0x803A70B0u)) {
            if (cpu.lr == 0x802B5DC8u &&
                (cpu.gpr[25] == 0x80ADDFECu || cpu.gpr[26] == 0x80ADDF44u) &&
                aram_piece_message_entry_trace_reports < 32u) {
                const u32 caller_command = cpu.gpr[26];
                fprintf(stderr,
                        "[aram-piece] OSSendMessage entry #%u message=0x%08X "
                        "command_reg=0x%08X message_command=0x%08X "
                        "length=%u src=0x%08X dst=0x%08X command_length=%u "
                        "command_src=0x%08X command_dst=0x%08X\n",
                        aram_piece_message_entry_trace_reports + 1u,
                        cpu.gpr[25], caller_command,
                        cpu.gpr[4] != 0u ? mem_read32(&cpu, cpu.gpr[4] + 0x04u) : 0u,
                        cpu.gpr[4] != 0u && mem_read32(&cpu, cpu.gpr[4] + 0x04u) != 0u
                            ? mem_read32(&cpu, mem_read32(&cpu, cpu.gpr[4] + 0x04u) + 0x44u)
                            : 0u,
                        cpu.gpr[4] != 0u && mem_read32(&cpu, cpu.gpr[4] + 0x04u) != 0u
                            ? mem_read32(&cpu, mem_read32(&cpu, cpu.gpr[4] + 0x04u) + 0x48u)
                            : 0u,
                        cpu.gpr[4] != 0u && mem_read32(&cpu, cpu.gpr[4] + 0x04u) != 0u
                            ? mem_read32(&cpu, mem_read32(&cpu, cpu.gpr[4] + 0x04u) + 0x4Cu)
                            : 0u,
                        caller_command != 0u ? mem_read32(&cpu, caller_command + 0x44u) : 0u,
                        caller_command != 0u ? mem_read32(&cpu, caller_command + 0x48u) : 0u,
                        caller_command != 0u ? mem_read32(&cpu, caller_command + 0x4Cu) : 0u);
                aram_piece_message_entry_trace_reports++;
            }
            fprintf(stderr,
                    "[osmsg] send-interest mq=0x%08X msg=0x%08X flags=%u "
                    "used=%u capacity=%u recv_head=0x%08X recv_tail=0x%08X "
                    "thread=0x%08X lr=0x%08X msg_field0=%u msg_command=0x%08X "
                    "command_length=%u command_src=0x%08X command_dst=0x%08X\n",
                    cpu.gpr[3], cpu.gpr[4], cpu.gpr[5],
                    mem_read32(&cpu, cpu.gpr[3] + 0x1Cu),
                    mem_read32(&cpu, cpu.gpr[3] + 0x14u),
                    mem_read32(&cpu, cpu.gpr[3] + 0x8u),
                    mem_read32(&cpu, cpu.gpr[3] + 0xCu),
                    mem_read32(&cpu, 0x800000E4u), cpu.lr,
                    cpu.gpr[4] != 0u ? mem_read32(&cpu, cpu.gpr[4] + 0x00u) : 0u,
                    cpu.gpr[4] != 0u ? mem_read32(&cpu, cpu.gpr[4] + 0x04u) : 0u,
                    cpu.gpr[4] != 0u && mem_read32(&cpu, cpu.gpr[4] + 0x04u) != 0u ?
                        mem_read32(&cpu, mem_read32(&cpu, cpu.gpr[4] + 0x04u) + 0x44u) : 0u,
                    cpu.gpr[4] != 0u && mem_read32(&cpu, cpu.gpr[4] + 0x04u) != 0u ?
                        mem_read32(&cpu, mem_read32(&cpu, cpu.gpr[4] + 0x04u) + 0x48u) : 0u,
                    cpu.gpr[4] != 0u && mem_read32(&cpu, cpu.gpr[4] + 0x04u) != 0u ?
                        mem_read32(&cpu, mem_read32(&cpu, cpu.gpr[4] + 0x04u) + 0x4Cu) : 0u);
        }
        if (g_runqueue_trace &&
            worker_queue_trace_reports < 24u && blocks >= 3000000ull &&
            cpu.pc == 0x80308B88u && cpu.gpr[3] == 0x8039CD60u) {
            fprintf(stderr,
                    "[sched] worker-wakeup #%u queue=0x%08X head=0x%08X "
                    "tail=0x%08X thread=0x%08X state=%u priority=%d "
                    "thread_queue=0x%08X run_bits=0x%08X hint=%u\n",
                    worker_queue_trace_reports + 1u, cpu.gpr[3],
                    mem_read32(&cpu, cpu.gpr[3]),
                    mem_read32(&cpu, cpu.gpr[3] + 4u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u),
                    (s32)mem_read32(&cpu, 0x804211E0u + 0x2D0u),
                    mem_read32(&cpu, 0x804211E0u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x803F7A34u));
        }
        if (g_runqueue_trace &&
            aram_pcs_trace_reports < 64u &&
            (cpu.pc == 0x802B5ED4u || cpu.pc == 0x802B5F68u)) {
            const u32 command = cpu.pc == 0x802B5F68u ? cpu.gpr[3] : 0u;
            fprintf(stderr,
                    "[aram] pcs #%u pc=0x%08X r3=0x%08X r4=0x%08X "
                    "r5=0x%08X r6=0x%08X r7=0x%08X command=0x%08X "
                    "data_length=%u source=0x%08X destination=0x%08X "
                    "thread=0x%08X lr=0x%08X\n",
                    aram_pcs_trace_reports + 1u, cpu.pc, cpu.gpr[3],
                    cpu.gpr[4], cpu.gpr[5], cpu.gpr[6], cpu.gpr[7], command,
                    command != 0u ? mem_read32(&cpu, command + 0x44u) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x48u) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x4Cu) : 0u,
                    mem_read32(&cpu, 0x800000E4u), cpu.lr);
            aram_pcs_trace_reports++;
        }
        if (g_runqueue_trace &&
            aram_stream_command_trace_reports < 64u &&
            (cpu.pc == 0x802B637Cu || cpu.pc == 0x802B6624u)) {
            const u32 command = cpu.gpr[3];
            fprintf(stderr,
                    "[aram] stream-command #%u pc=0x%08X command=0x%08X "
                    "type=%u address=0x%08X size=%u offset=0x%08X buffer=0x%08X "
                    "buffer_size=%u queue_used=%u queue_head=0x%08X "
                    "queue_tail=0x%08X thread=0x%08X lr=0x%08X\n",
                    aram_stream_command_trace_reports + 1u, cpu.pc, command,
                    command != 0u ? mem_read32(&cpu, command + 0x00u) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x04u) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x08u) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x14u) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x18u) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x1Cu) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x48u) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x34u) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x38u) : 0u,
                    mem_read32(&cpu, 0x800000E4u), cpu.lr);
            aram_stream_command_trace_reports++;
        }
        if (g_runqueue_trace &&
            aram_stream_constructor_trace_reports < 128u &&
            (cpu.pc == 0x802B65B0u || cpu.pc == 0x802B65B4u ||
             cpu.pc == 0x802B65B8u || cpu.pc == 0x802B65BCu ||
             cpu.pc == 0x802B65C8u || cpu.pc == 0x802B65D0u ||
             cpu.pc == 0x802B65D8u || cpu.pc == 0x802B65E0u ||
             cpu.pc == 0x802B65E4u || cpu.pc == 0x802B6604u ||
             cpu.pc == 0x802B6608u)) {
            const u32 command = cpu.gpr[31];
            fprintf(stderr,
                    "[aram-stream-construct] #%u pc=0x%08X command=0x%08X "
                    "r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X "
                    "type=%u address=0x%08X size=%u offset=0x%08X "
                    "stream=0x%08X buffer=0x%08X buffer_size=%u "
                    "field28=0x%08X queue=0x%08X message=0x%08X "
                    "current=0x%08X lr=0x%08X\n",
                    aram_stream_constructor_trace_reports + 1u, cpu.pc,
                    command, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.gpr[6],
                    command != 0u ? mem_read32(&cpu, command + 0x00u) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x04u) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x08u) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x14u) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x10u) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x18u) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x1Cu) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x28u) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x2Cu) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x4Cu) : 0u,
                    mem_read32(&cpu, 0x800000E4u), cpu.lr);
            aram_stream_constructor_trace_reports++;
        }
        if (g_runqueue_trace &&
            aram_late_command_trace_reports < 96u &&
            (cpu.pc == 0x802B637Cu || cpu.pc == 0x802B6624u) &&
            cpu.gpr[3] == 0x80ADDF10u) {
            const u32 command = cpu.gpr[3];
            fprintf(stderr,
                    "[aram] late-command #%u pc=0x%08X command=0x%08X "
                    "type=%u address=0x%08X size=%u offset=0x%08X "
                    "buffer=0x%08X buffer_size=%u mq_used=%u mq_recv=0x%08X "
                    "mq_send=0x%08X message=0x%08X field54=0x%08X field58=0x%08X "
                    "thread=0x%08X lr=0x%08X\n",
                    aram_late_command_trace_reports + 1u, cpu.pc, command,
                    mem_read32(&cpu, command + 0x00u),
                    mem_read32(&cpu, command + 0x04u),
                    mem_read32(&cpu, command + 0x08u),
                    mem_read32(&cpu, command + 0x14u),
                    mem_read32(&cpu, command + 0x18u),
                    mem_read32(&cpu, command + 0x1Cu),
                    mem_read32(&cpu, command + 0x48u),
                    mem_read32(&cpu, command + 0x34u),
                    mem_read32(&cpu, command + 0x38u),
                    mem_read32(&cpu, command + 0x4Cu),
                    mem_read32(&cpu, command + 0x50u),
                    mem_read32(&cpu, command + 0x54u),
                    mem_read32(&cpu, 0x800000E4u), cpu.lr);
            aram_late_command_trace_reports++;
        }
        if (g_runqueue_trace &&
            aram_late_command_trace_reports < 96u &&
            (cpu.pc == 0x802BDFA4u || cpu.pc == 0x802BDFE0u) &&
            cpu.gpr[29] != 0u &&
            mem_read32(&cpu, cpu.gpr[29] + 0x4Cu) == 0x80ADDF10u) {
            const u32 ripper = cpu.gpr[29];
            const u32 stream = mem_read32(&cpu, ripper + 0x4Cu);
            fprintf(stderr,
                    "[aram] late-ripper #%u pc=0x%08X ripper=0x%08X "
                    "stream=0x%08X field44=0x%08X link_prev=0x%08X "
                    "link_next=0x%08X dvd=0x%08X file_thread=0x%08X "
                    "stream_mq_used=%u stream_message=0x%08X param=%u "
                    "thread=0x%08X lr=0x%08X\n",
                    aram_late_command_trace_reports + 1u, cpu.pc, ripper,
                    stream, mem_read32(&cpu, ripper + 0x44u),
                    mem_read32(&cpu, ripper + 0x00u),
                    mem_read32(&cpu, ripper + 0x04u),
                    mem_read32(&cpu, ripper + 0x10u),
                    mem_read32(&cpu, mem_read32(&cpu, ripper + 0x10u) + 0x50u),
                    mem_read32(&cpu, stream + 0x48u),
                    mem_read32(&cpu, stream + 0x50u), cpu.gpr[30],
                    mem_read32(&cpu, 0x800000E4u), cpu.lr);
            aram_late_command_trace_reports++;
        }
        if (g_runqueue_trace &&
            aram_stream_writer_trace_reports < 48u &&
            (cpu.pc == 0x802B6334u || cpu.pc == 0x802B6364u ||
             cpu.pc == 0x802B636Cu || cpu.pc == 0x802B6374u ||
             cpu.pc == 0x802B637Cu || cpu.pc == 0x802B63D8u ||
             cpu.pc == 0x802B63F0u || cpu.pc == 0x802B6400u)) {
            fprintf(stderr,
                    "[aram-stream-writer] #%u pc=0x%08X r3=0x%08X "
                    "r4=0x%08X r5=0x%08X r6=0x%08X r29=0x%08X "
                    "r30=0x%08X r31=0x%08X lr=0x%08X current=0x%08X\n",
                    aram_stream_writer_trace_reports + 1u, cpu.pc,
                    cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.gpr[6],
                    cpu.gpr[29], cpu.gpr[30], cpu.gpr[31], cpu.lr,
                    mem_read32(&cpu, 0x800000E4u));
            aram_stream_writer_trace_reports++;
        }
        if (g_runqueue_trace &&
            (cpu.pc == 0x802B61E4u || cpu.pc == 0x802B6254u ||
             cpu.pc == 0x802B6304u || cpu.pc == 0x802B6568u ||
             cpu.pc == 0x802B6624u)) {
            fprintf(stderr,
                    "[aram-stream] pc=0x%08X r3=0x%08X r4=0x%08X "
                    "r5=0x%08X r6=0x%08X lr=0x%08X current=0x%08X "
                    "smsg_used=%u smsg_recv=0x%08X\n",
                    cpu.pc, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.gpr[6],
                    cpu.lr, mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x8039CDC0u + 0x1Cu),
                    mem_read32(&cpu, 0x8039CDC0u + 0x8u));
        }
        if (g_runqueue_trace &&
            aram_worker_loop_trace_reports < 80u &&
            mem_read32(&cpu, 0x800000E4u) == 0x804211E0u &&
            (cpu.pc == 0x802B5FE0u || cpu.pc == 0x802B61E4u ||
             cpu.pc == 0x802B6254u || cpu.pc == 0x802B6304u ||
             cpu.pc == 0x802B6568u || cpu.pc == 0x802B6624u)) {
            const u32 low_request = mem_read32(&cpu, 0x803F7C2Cu);
            const u32 high_request = mem_read32(&cpu, 0x803F7C28u);
            const u32 request = low_request != 0u ? low_request : high_request;
            const u32 worker = 0x804211E0u;
            fprintf(stderr,
                    "[aram] worker-loop #%u pc=0x%08X lr=0x%08X r1=0x%08X "
                    "r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X "
                    "current=0x%08X arq_lo=0x%08X arq_hi=0x%08X "
                    "cb_lo=0x%08X cb_hi=0x%08X request=0x%08X "
                    "type=%u source=0x%08X destination=0x%08X length=%u "
                    "queue_used=%u recv_head=0x%08X recv_tail=0x%08X "
                    "state=%u priority=%d thread_queue=0x%08X "
                    "run_bits=0x%08X hint=%u\n",
                    aram_worker_loop_trace_reports + 1u, cpu.pc, cpu.lr,
                    cpu.gpr[1], cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.gpr[6],
                    mem_read32(&cpu, 0x800000E4u), low_request, high_request,
                    mem_read32(&cpu, 0x803F7C34u), mem_read32(&cpu, 0x803F7C30u),
                    request, request != 0u ? mem_read32(&cpu, request + 0x08u) : 0u,
                    request != 0u ? mem_read32(&cpu, request + 0x10u) : 0u,
                    request != 0u ? mem_read32(&cpu, request + 0x14u) : 0u,
                    request != 0u ? mem_read32(&cpu, request + 0x18u) : 0u,
                    mem_read32(&cpu, 0x8039CD60u + 0x1Cu),
                    mem_read32(&cpu, 0x8039CD60u + 0x8u),
                    mem_read32(&cpu, 0x8039CD60u + 0xCu),
                    mem_read16(&cpu, worker + 0x2C8u),
                    (s32)mem_read32(&cpu, worker + 0x2D0u),
                    mem_read32(&cpu, worker + 0x2DCu),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x803F7A34u));
            aram_worker_loop_trace_reports++;
        }
        if (g_runqueue_trace &&
            g_aram_dma_trace_reports < 40u &&
            (cpu.pc == 0x80317238u || cpu.pc == 0x80318F10u ||
             cpu.pc == 0x802B5FE0u || cpu.pc == 0x802B5F68u)) {
            fprintf(stderr,
                    "[aram-dma] pc=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X "
                    "r6=0x%08X r7=0x%08X r8=0x%08X r9=0x%08X r10=0x%08X "
                    "lr=0x%08X current=0x%08X\n",
                    cpu.pc, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.gpr[6],
                    cpu.gpr[7], cpu.gpr[8], cpu.gpr[9], cpu.gpr[10], cpu.lr,
                    mem_read32(&cpu, 0x800000E4u));
            g_aram_dma_trace_reports++;
        }
        if (g_runqueue_trace &&
            g_aram_interrupt_trace_reports < 80u &&
            (cpu.pc == 0x803171F4u || cpu.pc == 0x80317390u ||
             cpu.pc == 0x80317464u || cpu.pc == 0x80318DD4u ||
             cpu.pc == 0x80318EA0u || cpu.pc == 0x80304AE0u)) {
            fprintf(stderr,
                    "[aram-int] pc=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X "
                    "r6=0x%08X lr=0x%08X current=0x%08X pi=0x%08X mask=0x%08X\n",
                    cpu.pc, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.gpr[6],
                    cpu.lr, mem_read32(&cpu, 0x800000E4u),
                    dol_interrupts_pi_cause(&g_interrupts),
                    dol_interrupts_pi_mask(&g_interrupts));
            g_aram_interrupt_trace_reports++;
        }
        if (cpu.pc == 0x80308B88u && cpu.gpr[3] == 0x803A72C8u) {
            fprintf(stderr,
                    "[osmsg] dvd-wakeup queue=0x%08X head=0x%08X tail=0x%08X "
                    "dvd_state=%u dvd_wait=0x%08X\n",
                    cpu.gpr[3], mem_read32(&cpu, cpu.gpr[3]),
                    mem_read32(&cpu, cpu.gpr[3] + 4u),
                    mem_read16(&cpu, 0x803A5F90u + 0x2C8u),
                    mem_read32(&cpu, 0x803A5F90u + 0x2DCu));
        }
        if (g_runqueue_trace &&
            (cpu.pc == 0x80307F30u || cpu.pc == 0x80307F78u ||
             cpu.pc == 0x80307FB0u)) {
            fprintf(stderr,
                    "[sched] runqueue-trace pc=0x%08X r31=0x%08X r4=0x%08X "
                    "r5=0x%08X r30=0x%08X bits=0x%08X hint=%u "
                    "dvd_state=%u dvd_queue=0x%08X dvd_next=0x%08X dvd_prev=0x%08X\n",
                    cpu.pc, cpu.gpr[31], cpu.gpr[4], cpu.gpr[5], cpu.gpr[30],
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x803F7A34u),
                    mem_read16(&cpu, 0x803A5F90u + 0x2C8u),
                    mem_read32(&cpu, 0x803A5F90u + 0x2DCu),
                    mem_read32(&cpu, 0x803A5F90u + 0x2E0u),
                    mem_read32(&cpu, 0x803A5F90u + 0x2E4u));
        }
        if (blocks >= 3000000ull && cpu.pc == 0x80304DF8u &&
            dispatch_return_trace_reports < 12u) {
            const u32 current_context = mem_read32(&cpu, 0x800000D4u);
            const u32 dispatch_context = cpu.gpr[30];
            fprintf(stderr,
                    "[interrupt] dispatch-return context=0x%08X "
                    "saved_srr0=0x%08X saved_srr1=0x%08X current=0x%08X "
                    "current_srr0=0x%08X current_srr1=0x%08X cpu_srr0=0x%08X "
                    "cpu_srr1=0x%08X msr=0x%08X\n",
                    dispatch_context, mem_read32(&cpu, dispatch_context + 0x198u),
                    mem_read32(&cpu, dispatch_context + 0x19Cu), current_context,
                    mem_read32(&cpu, current_context + 0x198u),
                    mem_read32(&cpu, current_context + 0x19Cu), cpu.srr0,
                    cpu.srr1, cpu.msr);
            dispatch_return_trace_reports++;
        }
        const u32 dispatch_input_pc = cpu.pc;
        const u32 dispatch_input_lr = cpu.lr;
        const u32 dispatch_input_r3 = cpu.gpr[3];
        const u32 dispatch_input_r4 = cpu.gpr[4];
        const u32 dispatch_input_r30 = cpu.gpr[30];
        const u32 dispatch_input_r28 = cpu.gpr[28];
        const u32 dispatch_input_r29 = cpu.gpr[29];
        const u32 dispatch_input_r31 = cpu.gpr[31];
        const u32 dispatch_input_msr = cpu.msr;
        const u32 dispatch_input_srr0 = cpu.srr0;
        const u32 dispatch_input_srr1 = cpu.srr1;
        unsigned scene_draw_index = 4u;
        switch (dispatch_input_pc) {
        case 0x800D8DB8u: scene_draw_index = 0u; break; /* daBg_Draw */
        case 0x8015E3F0u: scene_draw_index = 1u; break; /* daVrbox_Draw */
        case 0x8015EA5Cu: scene_draw_index = 2u; break; /* daVrbox2_Draw */
        case 0x8015D80Cu: scene_draw_index = 3u; break; /* daSea_Draw */
        default: break;
        }
        if (scene_draw_index < 4u) {
            if (!g_play_scene_reported) {
                scene_draw_before_play[scene_draw_index]++;
            } else {
                scene_draw_after_play[scene_draw_index]++;
                if (scene_draw_first_after_retrace[scene_draw_index] == UINT64_MAX)
                    scene_draw_first_after_retrace[scene_draw_index] =
                        g_host_retrace_count;
            }
        }
        if (g_audio_object_watch && audio_message_return_reported &&
            audio_scheduler_reports < 160u &&
            (dispatch_input_pc == 0x80307EACu ||
             dispatch_input_pc == 0x80303A50u ||
             dispatch_input_pc == 0x80303968u ||
             dispatch_input_pc == 0x803039D0u ||
             dispatch_input_pc == 0x80304630u ||
             dispatch_input_pc == 0x80304608u)) {
            const u32 audio_thread = 0x803E9260u;
            fprintf(stderr,
                    "[audio-scheduler] #%u input=0x%08X output=0x%08X "
                    "r3=0x%08X lr=0x%08X current=0x%08X context=0x%08X "
                    "audio_state=%u audio_queue=0x%08X audio_srr0=0x%08X "
                    "audio_srr1=0x%08X audio_ctx_state=0x%04X "
                    "audio_ctx_r3=0x%08X audio_ctx_lr=0x%08X "
                    "effective=%d base=%d r30=0x%08X r6=0x%08X "
                    "run_bits=0x%08X hint=%u reschedule=%d "
                    "cpu_srr0=0x%08X cpu_srr1=0x%08X\n",
                    audio_scheduler_reports + 1u, dispatch_input_pc, cpu.pc,
                    dispatch_input_r3, dispatch_input_lr,
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read16(&cpu, audio_thread + 0x2C8u),
                    mem_read32(&cpu, audio_thread + 0x2DCu),
                    mem_read32(&cpu, audio_thread + 0x198u),
                    mem_read32(&cpu, audio_thread + 0x19Cu),
                    mem_read16(&cpu, audio_thread + 0x1A2u),
                    mem_read32(&cpu, audio_thread + 0x0Cu),
                    mem_read32(&cpu, audio_thread + 0x84u),
                    (s32)mem_read32(&cpu, audio_thread + 0x2D0u),
                    (s32)mem_read32(&cpu, audio_thread + 0x2D4u),
                    dispatch_input_r30, cpu.gpr[6],
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x803F7A34u),
                    (s32)mem_read32(&cpu, 0x803F7A38u), cpu.srr0, cpu.srr1);
            audio_scheduler_reports++;
        }
        if (g_runqueue_trace &&
            blocks >= 31000000ull && late_selector_contract_trace_reports < 96u &&
            (dispatch_input_pc == 0x80307FA0u ||
             dispatch_input_pc == 0x80303A50u ||
             dispatch_input_pc == 0x80307EF4u)) {
            const u32 selected_context = dispatch_input_pc == 0x80307FA0u
                                             ? dispatch_input_r30
                                             : dispatch_input_r3;
            fprintf(stderr,
                    "[sched] late-selector-contract #%u input=0x%08X "
                    "pc_before=0x%08X selected=0x%08X selected_srr0=0x%08X "
                    "selected_srr1=0x%08X main_state=%u main_queue=0x%08X "
                    "main_next=0x%08X main_prev=0x%08X worker_state=%u "
                    "worker_queue=0x%08X run_bits=0x%08X hint=%u reschedule=%d "
                    "current=0x%08X context=0x%08X msr=0x%08X downcount=%d\n",
                    late_selector_contract_trace_reports + 1u,
                    dispatch_input_pc, cpu.pc, selected_context,
                    selected_context != 0u ? mem_read32(&cpu, selected_context + 0x198u) : 0u,
                    selected_context != 0u ? mem_read32(&cpu, selected_context + 0x19Cu) : 0u,
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read32(&cpu, 0x803A2960u + 0x2DCu),
                    mem_read32(&cpu, 0x803A2960u + 0x2E0u),
                    mem_read32(&cpu, 0x803A2960u + 0x2E4u),
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u),
                    mem_read32(&cpu, 0x804211E0u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x803F7A34u),
                    (s32)mem_read32(&cpu, 0x803F7A38u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u), cpu.msr, (s32)cpu.downcount);
            late_selector_contract_trace_reports++;
        }
        if (g_runqueue_trace &&
            main_sleep_reports < 8u && dispatch_input_pc == 0x80308AB8u &&
            mem_read32(&cpu, 0x800000E4u) == 0x803A2960u) {
            const u32 main_thread = 0x803A2960u;
            fprintf(stderr,
                    "[sched] main-sleep continuation #%u input=0x%08X "
                    "output=0x%08X lr=0x%08X state=%u queue=0x%08X "
                    "next=0x%08X prev=0x%08X saved_pc=0x%08X "
                    "run_bits=0x%08X hint=%u reschedule=%d current_context=0x%08X\n",
                    main_sleep_reports + 1u, dispatch_input_pc, cpu.pc, cpu.lr,
                    mem_read16(&cpu, main_thread + 0x2C8u),
                    mem_read32(&cpu, main_thread + 0x2DCu),
                    mem_read32(&cpu, main_thread + 0x2E0u),
                    mem_read32(&cpu, main_thread + 0x2E4u),
                    mem_read32(&cpu, main_thread + 0x198u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x803F7A34u),
                    (s32)mem_read32(&cpu, 0x803F7A38u),
                    mem_read32(&cpu, 0x800000D4u));
            main_sleep_reports++;
        }
        if (g_runqueue_trace &&
            aram_late_selectthread_reports < 32u && blocks >= 2500000ull &&
            dispatch_input_pc == 0x80304DF8u &&
            mem_read32(&cpu, 0x800000E4u) == 0x804211E0u) {
            fprintf(stderr,
                    "[sched] aram-late-selectthread before #%u dispatcher_r3=%u "
                    "reschedule=%d thread=0x%08X context=0x%08X "
                    "worker_state=%u worker_effective=%d worker_base=%d "
                    "worker_queue=0x%08X run_bits=0x%08X hint=%u p14=0x%08X "
                    "p16=0x%08X\n",
                    aram_late_selectthread_reports + 1u, dispatch_input_r3,
                    (s32)mem_read32(&cpu, 0x803F7A38u),
                    mem_read32(&cpu, 0x800000E4u), mem_read32(&cpu, 0x800000D4u),
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u),
                    (s32)mem_read32(&cpu, 0x804211E0u + 0x2D0u),
                    (s32)mem_read32(&cpu, 0x804211E0u + 0x2D4u),
                    mem_read32(&cpu, 0x804211E0u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x803F7A34u),
                    mem_read32(&cpu, 0x803F0328u),
                    mem_read32(&cpu, 0x803F0338u));
        }
        if (g_runqueue_trace &&
            dispatch_input_pc == 0x80307FA0u) {
            selector_selected_context = dispatch_input_r30;
        }
        if (g_runqueue_trace &&
            selector_load_handoff_reports < 32u &&
            dispatch_input_pc == 0x80303A50u &&
            selector_selected_context != 0u &&
            dispatch_input_r3 == selector_selected_context) {
            fprintf(stderr,
                    "[sched] selector-load before #%u selected=0x%08X "
                    "context=0x%08X saved_srr0=0x%08X saved_srr1=0x%08X "
                    "current_thread=0x%08X current_context=0x%08X "
                    "run_bits=0x%08X worker_state=%u worker_queue=0x%08X\n",
                    selector_load_handoff_reports + 1u,
                    selector_selected_context, dispatch_input_r3,
                    mem_read32(&cpu, dispatch_input_r3 + 0x198u),
                    mem_read32(&cpu, dispatch_input_r3 + 0x19Cu),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u),
                    mem_read32(&cpu, 0x804211E0u + 0x2DCu));
        }
        if (g_runqueue_trace &&
            worker_selection_trace_reports < 16u && blocks >= 3000000ull &&
            cpu.pc == 0x80307FA0u && cpu.gpr[30] == 0x804211E0u) {
            const u32 selected = cpu.gpr[30];
            fprintf(stderr,
                    "[sched] worker-selection #%u selected=0x%08X "
                    "state=%u priority=%d queue=0x%08X next=0x%08X prev=0x%08X "
                    "p6_head=0x%08X p6_tail=0x%08X p14_head=0x%08X "
                    "p14_tail=0x%08X p16_head=0x%08X p16_tail=0x%08X "
                    "run_bits=0x%08X priority6_ready=%u hint=%u current=0x%08X "
                    "context=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X "
                    "r7=0x%08X r13=0x%08X\n",
                    worker_selection_trace_reports + 1u, selected,
                    mem_read16(&cpu, selected + 0x2C8u),
                    (s32)mem_read32(&cpu, selected + 0x2D0u),
                    mem_read32(&cpu, selected + 0x2DCu),
                    mem_read32(&cpu, selected + 0x2E0u),
                    mem_read32(&cpu, selected + 0x2E4u),
                    mem_read32(&cpu, 0x803F02E8u),
                    mem_read32(&cpu, 0x803F02ECu),
                    mem_read32(&cpu, 0x803F0328u),
                    mem_read32(&cpu, 0x803F032Cu),
                    mem_read32(&cpu, 0x803F0338u),
                    mem_read32(&cpu, 0x803F033Cu),
                    mem_read32(&cpu, 0x803F7A30u),
                    (mem_read32(&cpu, 0x803F7A30u) & 0x02000000u) != 0u,
                    mem_read32(&cpu, 0x803F7A34u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u), cpu.gpr[4], cpu.gpr[5],
                    cpu.gpr[6], cpu.gpr[7], cpu.gpr[13]);
            worker_selection_trace_reports++;
        }
        const bool trace_worker_dispatch = g_runqueue_trace &&
                                           worker_dispatch_trace_reports < 24u &&
                                           blocks >= 3000000ull &&
                                           cpu.pc == 0x80304DF8u &&
                                           mem_read32(&cpu, 0x800000E4u) ==
                                               0x804211E0u;
        if (trace_worker_dispatch) {
            fprintf(stderr,
                    "[sched] worker-dispatch-before #%u pc=0x%08X "
                    "msr=0x%08X srr0=0x%08X srr1=0x%08X pending=%d "
                    "cause=0x%08X mask=0x%08X run_bits=0x%08X "
                    "thread=0x%08X context=0x%08X\n",
                    worker_dispatch_trace_reports + 1u, cpu.pc, cpu.msr,
                    cpu.srr0, cpu.srr1,
                    dol_interrupts_external_pending(&g_interrupts),
                    dol_interrupts_pi_cause(&g_interrupts),
                    dol_interrupts_pi_mask(&g_interrupts),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
        }
        if (blocks >= 3000000ull && selector_progress_trace_reports < 160u &&
            cpu.pc >= 0x80307EACu && cpu.pc <= 0x80307FC8u) {
            fprintf(stderr,
                    "[sched] selector-progress pc=0x%08X lr=0x%08X "
                    "r3=0x%08X r30=0x%08X r31=0x%08X run_bits=0x%08X "
                    "thread=0x%08X context=0x%08X\n",
                    cpu.pc, cpu.lr, cpu.gpr[3], cpu.gpr[30], cpu.gpr[31],
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            selector_progress_trace_reports++;
        }
        if (blocks >= 3000000ull && scheduler_transition_trace_reports < 24u &&
            (cpu.pc == 0x80307AB4u || cpu.pc == 0x80307AF4u ||
             cpu.pc == 0x80307FD0u || cpu.pc == 0x80308000u)) {
            fprintf(stderr,
                    "[sched] transition pc=0x%08X run_bits=0x%08X "
                    "run_hint=%u reschedule=%d thread=0x%08X "
                    "context=0x%08X lr=0x%08X\n",
                    cpu.pc, mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x803F7A34u),
                    (s32)mem_read32(&cpu, 0x803F7A38u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u), cpu.lr);
            scheduler_transition_trace_reports++;
        }
        if (blocks >= 3000000ull && cpu.pc == 0x80303968u &&
            current_context_trace_reports < 16u) {
            fprintf(stderr,
                    "[sched] OSSetCurrentContext entry context=0x%08X "
                    "old=0x%08X thread=0x%08X lr=0x%08X\n",
                    cpu.gpr[3], mem_read32(&cpu, 0x800000D4u),
                    mem_read32(&cpu, 0x800000E4u), cpu.lr);
            current_context_trace_reports++;
        }
        if (blocks >= 3000000ull && cpu.pc == 0x803039C4u &&
            current_context_trace_reports < 16u) {
            fprintf(stderr,
                    "[sched] OSSetCurrentContext return new=0x%08X "
                    "thread=0x%08X lr=0x%08X\n",
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read32(&cpu, 0x800000E4u), cpu.lr);
            current_context_trace_reports++;
        }
        if (g_runqueue_trace &&
            main_context_reports < 24u &&
            (cpu.pc == 0x80303968u || cpu.pc == 0x803039C4u) &&
            (cpu.gpr[3] == 0x803A2960u ||
             mem_read32(&cpu, 0x800000E4u) == 0x803A2960u ||
             mem_read32(&cpu, 0x800000D4u) == 0x803A2960u)) {
            fprintf(stderr,
                    "[sched] main-context #%u pc=0x%08X input=0x%08X "
                    "old=0x%08X new=0x%08X lr=0x%08X thread=0x%08X "
                    "current_context=0x%08X main_state=%u main_queue=0x%08X "
                    "run_bits=0x%08X\n",
                    main_context_reports + 1u, cpu.pc, cpu.gpr[3],
                    mem_read32(&cpu, 0x800000D4u),
                    cpu.pc == 0x803039C4u ? mem_read32(&cpu, 0x800000D4u) : 0u,
                    cpu.lr, mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read32(&cpu, 0x803A2960u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7A30u));
            main_context_reports++;
        }
        if (g_runqueue_trace &&
            main_sleep_store_reports < 32u &&
            (cpu.pc == 0x80308AC8u || cpu.pc == 0x80308ACCu ||
             cpu.pc == 0x80308B08u || cpu.pc == 0x80308B10u ||
             cpu.pc == 0x80308B14u || cpu.pc == 0x80308B1Cu ||
             cpu.pc == 0x80308B20u || cpu.pc == 0x80308B28u ||
             cpu.pc == 0x80308B30u || cpu.pc == 0x80308B38u ||
             cpu.pc == 0x80308B40u || cpu.pc == 0x80308B48u) &&
            (cpu.gpr[4] == 0x803A2960u || cpu.gpr[31] == 0x803F7B44u ||
             mem_read32(&cpu, 0x800000E4u) == 0x803A2960u)) {
            const u32 main_thread = 0x803A2960u;
            fprintf(stderr,
                    "[sched] main-sleep-store #%u pc=0x%08X r30=0x%08X "
                    "r4=0x%08X r5=0x%08X state=%u queue=0x%08X "
                    "next=0x%08X prev=0x%08X qhead=0x%08X qtail=0x%08X "
                    "run_bits=0x%08X current_thread=0x%08X\n",
                    main_sleep_store_reports + 1u, cpu.pc, cpu.gpr[30],
                    cpu.gpr[4], cpu.gpr[5],
                    mem_read16(&cpu, main_thread + 0x2C8u),
                    mem_read32(&cpu, main_thread + 0x2DCu),
                    mem_read32(&cpu, main_thread + 0x2E0u),
                    mem_read32(&cpu, main_thread + 0x2E4u),
                    mem_read32(&cpu, 0x803F7B44u),
                    mem_read32(&cpu, 0x803F7B48u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u));
            main_sleep_store_reports++;
        }
        if (g_runqueue_trace &&
            aram_late_context_reports < 32u && blocks >= 2500000ull &&
            dispatch_input_pc == 0x80303968u &&
            mem_read32(&cpu, 0x800000E4u) == 0x804211E0u) {
            const u32 current_context = mem_read32(&cpu, 0x800000D4u);
            fprintf(stderr,
                    "[sched] aram-late-context before #%u input_r3=0x%08X "
                    "thread=0x%08X context=0x%08X worker_state=%u "
                    "worker_saved_pc=0x%08X run_bits=0x%08X\n",
                    aram_late_context_reports + 1u, dispatch_input_r3,
                    mem_read32(&cpu, 0x800000E4u), current_context,
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u),
                    mem_read32(&cpu, 0x804211E0u + 0x198u),
                    mem_read32(&cpu, 0x803F7A30u));
        }
        if (g_runqueue_trace &&
            aram_receive_interrupt_boundary_reports < 32u &&
            dispatch_input_pc == 0x80304608u &&
            dispatch_input_lr == 0x803059FCu &&
            mem_read32(&cpu, 0x800000E4u) == 0x804211E0u) {
            const u32 context = mem_read32(&cpu, 0x800000D4u);
            fprintf(stderr,
                    "[sched] aram-receive-interrupt before #%u "
                    "input=0x%08X lr=0x%08X context=0x%08X "
                    "context_srr0=0x%08X context_srr1=0x%08X "
                    "worker_saved_pc=0x%08X msr=0x%08X\n",
                    aram_receive_interrupt_boundary_reports + 1u,
                    dispatch_input_pc, dispatch_input_lr, context,
                    context ? mem_read32(&cpu, context + 0x198u) : 0u,
                    context ? mem_read32(&cpu, context + 0x19Cu) : 0u,
                    mem_read32(&cpu, 0x804211E0u + 0x198u), cpu.msr);
        }
        const bool trace_dispatch_return = dispatch_input_pc == 0x80304DF8u &&
                                           dispatch_return_trace_reports != 0u;
        const bool trace_priority8_resume =
            g_runqueue_trace &&
            selector_priority8_resume_reports < 24u &&
            dispatch_input_pc == 0x80307EACu &&
            mem_read32(&cpu, 0x800000E4u) == 0x80425680u;
        if (trace_priority8_resume) {
            fprintf(stderr,
                    "[sched] priority8-selector before #%u thread=0x%08X "
                    "context=0x%08X r3=0x%08X r1=0x%08X lr=0x%08X "
                    "state=%u priority=%d queue=0x%08X run_bits=0x%08X\n",
                    selector_priority8_resume_reports + 1u,
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u), cpu.gpr[3], cpu.gpr[1],
                    cpu.lr, mem_read16(&cpu, 0x80425680u + 0x2C8u),
                    (s32)mem_read32(&cpu, 0x80425680u + 0x2D0u),
                    mem_read32(&cpu, 0x80425680u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7A30u));
        }
        if (g_runqueue_trace &&
            selector_mask_trace_reports < 32u &&
            dispatch_input_pc == 0x80307F74u &&
            dispatch_input_r30 == 0x804211E0u) {
            fprintf(stderr,
                    "[sched] selector-mask #%u selected=0x%08X "
                    "source_bits=0x%08X computed_bits=0x%08X clz=%u "
                    "selected_bit=0x%08X source_has_selected=%u "
                    "computed_matches_clear=%u "
                    "p6_head=0x%08X p6_tail=0x%08X p14_head=0x%08X "
                    "p16_head=0x%08X current_thread=0x%08X current_context=0x%08X\n",
                    selector_mask_trace_reports + 1u, dispatch_input_r30,
                    dispatch_input_r4, cpu.gpr[0], cpu.gpr[7],
                    cpu.gpr[7] < 32u ? (1u << (31u - cpu.gpr[7])) : 0u,
                    cpu.gpr[7] < 32u &&
                            (dispatch_input_r4 & (1u << (31u - cpu.gpr[7]))) != 0u,
                    cpu.gpr[7] < 32u &&
                            cpu.gpr[0] ==
                                (dispatch_input_r4 &
                                 ~(1u << (31u - cpu.gpr[7]))),
                    mem_read32(&cpu, 0x803F02E8u),
                    mem_read32(&cpu, 0x803F02ECu),
                    mem_read32(&cpu, 0x803F0328u),
                    mem_read32(&cpu, 0x803F0338u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            selector_mask_trace_reports++;
        }
        if (g_runqueue_trace &&
            finish_queue_wakeup_body_reports < 16u &&
            dispatch_input_pc == 0x80308BA4u &&
            dispatch_input_r30 == 0x803F7C9Cu) {
            fprintf(stderr,
                    "[gx] FinishQueue OSWakeupThread body before pc=0x%08X "
                    "r3=0x%08X r6=0x%08X r31=0x%08X main_state=%u "
                    "main_queue=0x%08X finish_head=0x%08X finish_tail=0x%08X "
                    "main_next=0x%08X main_prev=0x%08X run_bits=0x%08X "
                    "current_thread=0x%08X current_context=0x%08X\n",
                    dispatch_input_pc, dispatch_input_r3, cpu.gpr[6],
                    dispatch_input_r31,
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read32(&cpu, 0x803A2960u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7C9Cu),
                    mem_read32(&cpu, 0x803F7CA0u),
                    mem_read32(&cpu, 0x803A2960u + 0x2E0u),
                    mem_read32(&cpu, 0x803A2960u + 0x2E4u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            finish_queue_wakeup_body_reports++;
        }
        if (g_runqueue_trace &&
            finish_queue_wakeup_restore_reports < 16u &&
            dispatch_input_pc == 0x80304630u && dispatch_input_lr == 0x80308C74u) {
            fprintf(stderr,
                    "[gx] FinishQueue OSWakeupThread restore before pc=0x%08X "
                    "r3=0x%08X r4=0x%08X main_state=%u main_queue=0x%08X "
                    "finish_head=0x%08X finish_tail=0x%08X run_bits=0x%08X "
                    "current_thread=0x%08X current_context=0x%08X\n",
                    dispatch_input_pc, dispatch_input_r3, dispatch_input_r4,
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read32(&cpu, 0x803A2960u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7C9Cu),
                    mem_read32(&cpu, 0x803F7CA0u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            finish_queue_wakeup_restore_reports++;
        }
        if (g_runqueue_trace &&
            finish_queue_wakeup_restore_reports < 16u &&
            dispatch_input_pc == 0x80308C74u &&
            mem_read32(&cpu, 0x803A2960u + 0x2DCu) == 0x803F0338u) {
            fprintf(stderr,
                    "[gx] FinishQueue OSWakeupThread final continuation before "
                    "pc=0x%08X lr=0x%08X main_state=%u main_queue=0x%08X "
                    "finish_head=0x%08X finish_tail=0x%08X run_bits=0x%08X "
                    "current_thread=0x%08X current_context=0x%08X\n",
                    dispatch_input_pc, dispatch_input_lr,
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read32(&cpu, 0x803A2960u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7C9Cu),
                    mem_read32(&cpu, 0x803F7CA0u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            finish_queue_wakeup_restore_reports++;
        }
        const bool trace_finish_wake_thread_switch =
            g_runqueue_trace &&
            finish_wake_thread_switch_reports < 16u &&
            blocks >= 5000000ull && dispatch_input_pc == 0x802B40ECu &&
            (mem_read16(&cpu, 0x803A2960u + 0x2C8u) == 1u ||
             mem_read16(&cpu, 0x803A2960u + 0x2C8u) == 2u);
        if (trace_finish_wake_thread_switch) {
            fprintf(stderr,
                    "[gx] FinishQueue thread-switch before #%u input=0x%08X "
                    "r3=0x%08X r30=0x%08X main_state=%u main_queue=0x%08X "
                    "main_srr0=0x%08X main_srr1=0x%08X run_bits=0x%08X "
                    "current_thread=0x%08X current_context=0x%08X\n",
                    finish_wake_thread_switch_reports + 1u, dispatch_input_pc,
                    dispatch_input_r3, dispatch_input_r30,
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read32(&cpu, 0x803A2960u + 0x2DCu),
                    mem_read32(&cpu, 0x803A2960u + 0x198u),
                    mem_read32(&cpu, 0x803A2960u + 0x19Cu),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
        }
        if (g_runqueue_trace &&
            heap_tail_pre_reports < 128u) {
            const u32 heap = 0x8041D080u;
            const u32 tail = mem_read32(&cpu, heap + 0x7Cu);
            const bool heap_boundary_pc =
                dispatch_input_pc == 0x802B264Cu ||
                dispatch_input_pc == 0x802B27E4u ||
                dispatch_input_pc == 0x802B1DE0u;
            if (heap_boundary_pc && blocks >= 7000000ull) {
                fprintf(stderr,
                        "[heap-boundary] before=0x%08X pc=0x%08X "
                        "tail=0x%08X tail_size=0x%08X prev=0x%08X "
                        "next=0x%08X tail_used=0x%08X head_used=0x%08X "
                        "context=0x%08X msr=0x%08X\n",
                        dispatch_input_pc, cpu.pc, tail,
                        tail != 0u ? mem_read32(&cpu, tail + 0x04u) : 0u,
                        tail != 0u ? mem_read32(&cpu, tail + 0x08u) : 0u,
                        tail != 0u ? mem_read32(&cpu, tail + 0x0Cu) : 0u,
                        mem_read32(&cpu, heap + 0x84u),
                        mem_read32(&cpu, heap + 0x80u),
                        mem_read32(&cpu, 0x800000D4u), cpu.msr);
                const u32 nominal_size = mem_read32(&cpu, 0x80AD2144u);
                const u32 nominal_prev = mem_read32(&cpu, 0x80AD2148u);
                const u32 nominal_next = mem_read32(&cpu, 0x80AD214Cu);
                const bool nominal_malformed =
                    nominal_size > 0x01000000u ||
                    nominal_prev >= 0x80000000u ||
                    nominal_next >= 0x80000000u;
                if (heap_allocator_tuple_reports < 96u && nominal_malformed) {
                    fprintf(stderr,
                            "[heap-tuple] before=0x%08X pc=0x%08X "
                            "r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X "
                            "r7=0x%08X r8=0x%08X r9=0x%08X r10=0x%08X "
                            "r11=0x%08X r12=0x%08X r30=0x%08X r31=0x%08X "
                            "lr=0x%08X tail=0x%08X size=0x%08X "
                            "prev=0x%08X next=0x%08X tail_used=0x%08X "
                            "head_used=0x%08X context=0x%08X\n",
                            dispatch_input_pc, cpu.pc, cpu.gpr[3], cpu.gpr[4],
                            cpu.gpr[5], cpu.gpr[6], cpu.gpr[7], cpu.gpr[8],
                            cpu.gpr[9], cpu.gpr[10], cpu.gpr[11], cpu.gpr[12],
                            cpu.gpr[30], cpu.gpr[31], cpu.lr, tail,
                            tail != 0u ? mem_read32(&cpu, tail + 0x04u) : 0u,
                            tail != 0u ? mem_read32(&cpu, tail + 0x08u) : 0u,
                            tail != 0u ? mem_read32(&cpu, tail + 0x0Cu) : 0u,
                            mem_read32(&cpu, heap + 0x84u),
                            mem_read32(&cpu, heap + 0x80u),
                            mem_read32(&cpu, 0x800000D4u));
                    heap_allocator_tuple_reports++;
                }
            }
            if (tail == 0x80AD2140u) {
                const u32 current[6] = {
                    mem_read32(&cpu, tail + 0x04u),
                    mem_read32(&cpu, tail + 0x08u),
                    mem_read32(&cpu, tail + 0x0Cu),
                    mem_read32(&cpu, heap + 0x84u),
                    mem_read32(&cpu, heap + 0x80u),
                    0u,
                };
                bool changed = !heap_tail_watch_initialized;
                for (unsigned field = 0; field < 6u; field++)
                    changed |= current[field] != heap_tail_watch[field];
                if (changed) {
                    fprintf(stderr,
                            "[heap-sentinel] before=0x%08X pc=0x%08X "
                            "tail_size=0x%08X prev=0x%08X next=0x%08X "
                            "tail_used=0x%08X head_used=0x%08X "
                            "context=0x%08X msr=0x%08X\n",
                            dispatch_input_pc, cpu.pc, current[0], current[1],
                            current[2], current[3], current[4],
                            mem_read32(&cpu, 0x800000D4u), cpu.msr);
                    heap_tail_pre_reports++;
                }
            }
        }
        if (g_runqueue_trace && blocks >= 7000000ull &&
            heap_nominal_state_initialized && heap_nominal_pre_reports < 32u) {
            const u32 current[4] = {
                mem_read32(&cpu, 0x8041D0FCu),
                mem_read32(&cpu, 0x80AD2144u),
                mem_read32(&cpu, 0x80AD2148u),
                mem_read32(&cpu, 0x80AD214Cu),
            };
            bool changed = false;
            for (unsigned field = 0; field < 4u; field++)
                changed |= current[field] != heap_nominal_state[field];
            if (changed) {
                fprintf(stderr,
                        "[heap-pre] input=0x%08X tail=0x%08X size=0x%08X "
                        "prev=0x%08X next=0x%08X prior_tail=0x%08X "
                        "prior_size=0x%08X prior_prev=0x%08X prior_next=0x%08X "
                        "context=0x%08X\n",
                        dispatch_input_pc, current[0], current[1], current[2],
                        current[3], heap_nominal_state[0],
                        heap_nominal_state[1], heap_nominal_state[2],
                        heap_nominal_state[3], mem_read32(&cpu, 0x800000D4u));
                heap_nominal_pre_reports++;
            }
        }
        g_heap_write_watch_late = blocks >= 7000000ull;
        g_heap_write_watch_dispatch_pc = dispatch_input_pc;
        host_alias_rel_pc(&cpu);
        host_alias_rel_lifecycle_table(&cpu);
        // Keep the fixed module-60 BSS image outside the owning solid heap's
        // monotonic head allocation without changing the root heap topology.
        if (dispatch_input_pc == 0x802B0C38u && cpu.gpr[3] == 76u) {
            const u32 heap = mem_read32(&cpu, cpu.gpr[13] - 27060u);
            const u32 solid_head = heap != 0u ? mem_read32(&cpu, heap + 0x70u) : 0u;
            const u32 alignment = 4u;
            const u32 aligned_head = (solid_head + alignment - 1u) &
                                     ~(alignment - 1u);
            const u32 requested_end = aligned_head + ((cpu.gpr[3] + 3u) & ~3u);
            if (aligned_head < 0x815581A0u &&
                requested_end > 0x815581A0u &&
                solid_head < 0x815583B4u) {
                const u32 skipped = 0x815583B4u - solid_head;
                const u32 free_size = mem_read32(&cpu, heap + 0x6Cu);
                mem_write32(&cpu, heap + 0x70u, 0x815583B4u);
                mem_write32(&cpu, heap + 0x6Cu,
                            free_size >= skipped ? free_size - skipped : 0u);
                fprintf(stderr,
                        "[rel-solid-hole] heap=0x%08X head=0x%08X size=0x%08X "
                        "alignment=0x%08X skipped=0x%08X new_head=0x%08X\n",
                        heap, solid_head, cpu.gpr[3], alignment, skipped,
                        0x815583B4u);
                g_rel_solid_hole_reports++;
            }
        }
        if (g_rel_indirect_trace &&
            g_rel_indirect_trace_reports < 32u &&
            (cpu.pc == 0x802411CCu || cpu.pc == 0x802411D0u ||
             cpu.pc == 0x8155586Cu || cpu.pc == 0x802411D4u ||
             cpu.pc == 0x8155016Cu || cpu.pc == 0x81550170u ||
             cpu.pc == 0x81550174u || cpu.pc == 0x81550178u ||
             cpu.pc == 0x8155017Cu || cpu.pc == 0x81550180u ||
             cpu.pc == 0x8155010Cu || cpu.pc == 0x81550110u ||
             cpu.pc == 0x815501B4u || cpu.pc == 0x815501B8u ||
             cpu.pc == 0x815501BCu || cpu.pc == 0x815501C0u ||
             cpu.pc == 0x81550348u)) {
            fprintf(stderr,
                    "[rel-indirect] before pc=0x%08X lr=0x%08X ctr=0x%08X "
                    "r1=0x%08X r3=0x%08X r6=0x%08X r12=0x%08X r31=0x%08X "
                    "r5=0x%08X mem_r6=0x%08X mem_r31=0x%08X mem_r5=0x%08X "
                    "mem_r5_4=0x%08X mem_r5_8=0x%08X\n",
                    cpu.pc, cpu.lr, cpu.ctr, cpu.gpr[1], cpu.gpr[3],
                    cpu.gpr[6], cpu.gpr[12], cpu.gpr[31], cpu.gpr[5],
                    mem_read32(&cpu, cpu.gpr[6]), mem_read32(&cpu, cpu.gpr[31]), mem_read32(&cpu, cpu.gpr[5]),
                    mem_read32(&cpu, cpu.gpr[5] + 4u),
                    mem_read32(&cpu, cpu.gpr[5] + 8u));
            g_rel_indirect_trace_reports++;
        }
        if (g_rel_destructors_trace) {
            const u32 lwood_ctor = mem_read32(&cpu, 0x80B60870u);
            if (lwood_ctor != g_rel_lwood_ctor_watch) {
                fprintf(stderr,
                        "[rel-call] d_a_lwood ctor-slot changed block=%llu "
                        "old=0x%08X new=0x%08X pc=0x%08X input=0x%08X\n",
                        blocks, g_rel_lwood_ctor_watch, lwood_ctor, cpu.pc,
                        dispatch_input_pc);
                g_rel_lwood_ctor_watch = lwood_ctor;
            }
        }
        if (g_rel_lifecycle_trace &&
            g_rel_lifecycle_trace_reports < 24u &&
            cpu.pc >= 0x81E000DCu && cpu.pc < 0x81E02000u) {
            fprintf(stderr,
                    "[rel-trace] input=0x%08X linked_pc=0x%08X lr=0x%08X "
                    "r1=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X\n",
                    dispatch_input_pc, cpu.pc, cpu.lr, cpu.gpr[1], cpu.gpr[3],
                    cpu.gpr[4], cpu.gpr[5]);
            g_rel_lifecycle_trace_reports++;
        }
        if (g_rel_calls_trace &&
            g_rel_call_trace_reports < 24u &&
            (cpu.pc == 0x802411CCu || cpu.pc == 0x802411D0u ||
             cpu.pc == 0x802411D4u)) {
            fprintf(stderr,
                    "[rel-call] pc=0x%08X lr=0x%08X r3=0x%08X r4=0x%08X "
                    "r12=0x%08X r29=0x%08X r31=0x%08X mem_r31=0x%08X\n",
                    cpu.pc, cpu.lr, cpu.gpr[3], cpu.gpr[4], cpu.gpr[12],
                    cpu.gpr[29], cpu.gpr[31], mem_read32(&cpu, cpu.gpr[31]));
            g_rel_call_trace_reports++;
        }
        if (g_rel_destructors_trace &&
            g_rel_continuation_reports < 24u &&
            (cpu.pc == 0x81B10188u || cpu.pc == 0x81B101C0u ||
             cpu.pc == 0x81B10BCCu || cpu.pc == 0x81B1D484u)) {
            fprintf(stderr,
                    "[rel-life] pc=0x%08X lr=0x%08X ctr=0x%08X "
                    "r3=0x%08X r4=0x%08X r5=0x%08X r31=0x%08X "
                    "mem_r31=0x%08X\n",
                    cpu.pc, cpu.lr, cpu.ctr, cpu.gpr[3], cpu.gpr[4],
                    cpu.gpr[5], cpu.gpr[31], mem_read32(&cpu, cpu.gpr[31]));
            g_rel_continuation_reports++;
        }
        if (g_rel_destructors_trace &&
            g_rel_call_trace_reports < 24u &&
            (cpu.pc == 0x80241178u || cpu.pc == 0x802411F8u ||
             cpu.pc == 0x802410B4u || cpu.pc == 0x802410B8u)) {
            fprintf(stderr,
                    "[rel-life-api] pc=0x%08X lr=0x%08X ctr=0x%08X "
                    "r3=0x%08X r4=0x%08X r31=0x%08X mem_r31=0x%08X\n",
                    cpu.pc, cpu.lr, cpu.ctr, cpu.gpr[3], cpu.gpr[4],
                    cpu.gpr[31], mem_read32(&cpu, cpu.gpr[31]));
            g_rel_call_trace_reports++;
        }
        if (rel_prolog_sda_pending && dispatch_input_pc == 0x80240EE8u) {
            fprintf(stderr,
                    "[rel-abi] prolog-return pc=0x%08X restoring_r13=0x%08X "
                    "observed_r13=0x%08X\n",
                    dispatch_input_pc, rel_prolog_saved_r13, cpu.gpr[13]);
            cpu.gpr[13] = rel_prolog_saved_r13;
            rel_prolog_sda_pending = false;
        }
        const bool trace_bg_phase =
            g_bg_create_trace &&
            bg_phase_trace_reports < 32u &&
            dispatch_input_pc == 0x8004073Cu &&
            dispatch_input_r3 >= 0x80000000u &&
            mem_read16(&cpu, dispatch_input_r3 + 0x50u) == 444 &&
            dispatch_input_r3 == 0x80ACC1DCu;
        const bool trace_bg_postmethod =
            g_bg_create_trace &&
            bg_postmethod_trace_reports < 16u &&
            dispatch_input_pc == 0x80040794u &&
            dispatch_input_r3 == 0x80ACC1DCu;
        u32 bg_phase_process = 0u;
        u32 bg_phase_request_layer = 0u;
        u32 bg_phase_process_layer = 0u;
        if (trace_bg_phase) {
            bg_phase_process = mem_read32(&cpu, dispatch_input_r3 + 0x40u);
            bg_phase_request_layer = mem_read32(&cpu, dispatch_input_r3 + 0x44u);
            bg_phase_process_layer = bg_phase_process + 0xBCu;
            fprintf(stderr,
                    "[scene-bg-phase-before] request=0x%08X process=0x%08X "
                    "request_layer=0x%08X request_creating=%d "
                    "derived_area=0x%08X derived_word=%d blocks=%llu\n",
                    dispatch_input_r3, bg_phase_process, bg_phase_request_layer,
                    bg_phase_request_layer >= 0x80000000u
                        ? (s32)mem_read16(&cpu, bg_phase_request_layer + 0x28u) : -1,
                    bg_phase_process_layer,
                    bg_phase_process >= 0x80000000u
                        ? (s32)mem_read16(&cpu, bg_phase_process_layer + 0x28u) : -1,
                    (unsigned long long)blocks);
            fprintf(stderr,
                    "[scene-bg-phase-process] process=0x%08X subtype=0x%08X "
                    "pc_mtd=0x%08X prof=0x%08X proc_name=%d prof_name=%d "
                    "derived_b8=0x%08X derived_bc=0x%02X "
                    "lf_type=0x%08X nd_type=0x%08X\n",
                    bg_phase_process, mem_read32(&cpu, bg_phase_process + 0xB4u),
                    mem_read32(&cpu, bg_phase_process + 0xA8u),
                    mem_read32(&cpu, bg_phase_process + 0x10u),
                    (s32)mem_read16(&cpu, bg_phase_process + 0x08u),
                    (s32)mem_read16(&cpu, bg_phase_process + 0x0Eu),
                    mem_read32(&cpu, bg_phase_process + 0xB8u),
                    mem_read8(&cpu, bg_phase_process + 0xBCu),
                    mem_read32(&cpu, 0x803F6A48u),
                    mem_read32(&cpu, 0x803F6A58u));
        }
        if (trace_bg_postmethod) {
            fprintf(stderr,
                    "[scene-bg-postmethod-before] request=0x%08X "
                    "callback=0x%08X callback_data=0x%08X blocks=%llu\n",
                    dispatch_input_r3, mem_read32(&cpu, dispatch_input_r3 + 0x58u),
                    mem_read32(&cpu, dispatch_input_r3 + 0x5Cu),
                    (unsigned long long)blocks);
        }
        bluewake_cycle_domain_prepare_dispatch(&g_cycle_domain, &cpu);
        // The play window is the graded one; the boot's turn structure is not
        // (docs/status/CURRENT.md, 2026-09-22).
        const bool turn_census =
            g_turn_census_enabled && g_host_retrace_count >= g_credit_census_window;
        if (turn_census) {
            g_turn_blocks_this_turn = 0u;
            g_turn_edge_hit = false;
            g_turn_downcount_before_last_block = cpu.downcount;
        }
        int dispatched = mod->dispatch(&cpu, cpu.pc);
        if (turn_census)
            host_turn_exit_classify(&cpu, dispatched);
        if (return_census_enabled)
            bluewake_return_census_record(&return_census, mod,
                                          dispatch_input_pc, &cpu, dispatched);
        if (g_audio_object_watch && g_audio_thread_active &&
            mem_read32(&cpu, 0x800000D4u) == 0x803E9260u &&
            dispatch_input_msr != cpu.msr &&
            audio_msr_dispatch_reports < 32u) {
            fprintf(stderr,
                    "[audio-msr-dispatch] input_pc=0x%08X output_pc=0x%08X "
                    "old=0x%08X new=0x%08X input_srr0=0x%08X "
                    "input_srr1=0x%08X output_srr0=0x%08X output_srr1=0x%08X "
                    "exception=0x%08X dispatched=%d blocks=%llu\n",
                    dispatch_input_pc, cpu.pc, dispatch_input_msr, cpu.msr,
                    dispatch_input_srr0, dispatch_input_srr1, cpu.srr0,
                    cpu.srr1, cpu.exception,
                    dispatched, (unsigned long long)blocks);
            audio_msr_dispatch_reports++;
        }
        if (trace_bg_postmethod) {
            fprintf(stderr,
                    "[scene-bg-postmethod-after] request=0x%08X output_pc=0x%08X "
                    "output_r3=0x%08X dispatched=%d blocks=%llu\n",
                    dispatch_input_r3, cpu.pc, cpu.gpr[3], dispatched,
                    (unsigned long long)blocks);
            bg_postmethod_trace_reports++;
        }
        if (g_gx_entry_trace && gx_entry_trace_reports < 64u &&
            host_gx_entry_traced(dispatch_input_pc)) {
            fprintf(stderr,
                    "[gx-entry] pc=0x%08X lr=0x%08X r3=0x%08X r4=0x%08X "
                    "r5=0x%08X r6=0x%08X blocks=%llu\n",
                    dispatch_input_pc, dispatch_input_lr, dispatch_input_r3,
                    dispatch_input_r4, cpu.gpr[5], cpu.gpr[6],
                    (unsigned long long)blocks);
            gx_entry_trace_reports++;
        }
        if (trace_bg_phase) {
            fprintf(stderr,
                    "[scene-bg-phase-after] request=0x%08X process=0x%08X "
                    "output_pc=0x%08X output_r3=0x%08X request_creating=%d "
                    "derived_word=%d dispatched=%d blocks=%llu\n",
                    dispatch_input_r3, bg_phase_process, cpu.pc,
                    cpu.gpr[3],
                    bg_phase_request_layer >= 0x80000000u
                        ? (s32)mem_read16(&cpu, bg_phase_request_layer + 0x28u) : -1,
                    bg_phase_process >= 0x80000000u
                        ? (s32)mem_read16(&cpu, bg_phase_process_layer + 0x28u) : -1,
                    dispatched, (unsigned long long)blocks);
            bg_phase_trace_reports++;
        }
        host_alias_rel_pc(&cpu);
        if (g_runqueue_trace &&
            late_worker_message_trace_reports < 48u &&
            blocks >= 31000000ull && dispatch_input_pc == 0x802B45ACu) {
            fprintf(stderr,
                    "[sched] late-worker-message #%u input_r3=0x%08X "
                    "input_r30=0x%08X input_r29=0x%08X output=0x%08X "
                    "output_r3=0x%08X current=0x%08X context=0x%08X "
                    "run_bits=0x%08X worker_state=%u queue_used=%u\n",
                    late_worker_message_trace_reports + 1u,
                    dispatch_input_r3, dispatch_input_r30, dispatch_input_r29,
                    cpu.pc, cpu.gpr[3], mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u),
                    mem_read32(&cpu, 0x8039CD60u + 0x1Cu));
            late_worker_message_trace_reports++;
        }
        if (g_runqueue_trace &&
            late_worker_handler_trace_reports < 48u &&
            blocks >= 31000000ull && dispatch_input_pc == 0x802B0518u) {
            fprintf(stderr,
                    "[sched] late-worker-handler #%u input_r3=0x%08X "
                    "input_r4=0x%08X input_lr=0x%08X output=0x%08X "
                    "output_r3=0x%08X output_r4=0x%08X output_lr=0x%08X "
                    "current=0x%08X context=0x%08X run_bits=0x%08X "
                    "worker_state=%u queue_used=%u\n",
                    late_worker_handler_trace_reports + 1u,
                    dispatch_input_r3, dispatch_input_r4, dispatch_input_lr,
                    cpu.pc, cpu.gpr[3], cpu.gpr[4], cpu.lr,
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u),
                    mem_read32(&cpu, 0x8039CD60u + 0x1Cu));
            late_worker_handler_trace_reports++;
        }
        if (g_runqueue_trace &&
            late_worker_callback_trace_reports < 48u &&
            blocks >= 31000000ull &&
            ((dispatch_input_pc == 0x80328F40u &&
              dispatch_input_lr == 0x802B09C4u) ||
             dispatch_input_pc == 0x802B09C4u)) {
            fprintf(stderr,
                    "[sched] late-worker-callback #%u input=0x%08X "
                    "input_r3=0x%08X input_r4=0x%08X input_lr=0x%08X "
                    "output=0x%08X output_r3=0x%08X output_r4=0x%08X "
                    "output_lr=0x%08X current=0x%08X context=0x%08X "
                    "run_bits=0x%08X worker_state=%u queue_used=%u\n",
                    late_worker_callback_trace_reports + 1u,
                    dispatch_input_pc, dispatch_input_r3, dispatch_input_r4,
                    dispatch_input_lr, cpu.pc, cpu.gpr[3], cpu.gpr[4], cpu.lr,
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u),
                    mem_read32(&cpu, 0x8039CD60u + 0x1Cu));
            late_worker_callback_trace_reports++;
        }
        if (g_runqueue_trace &&
            late_worker_command_return_trace_reports < 48u &&
            blocks >= 31000000ull &&
            (dispatch_input_pc == 0x802B4ED8u ||
             dispatch_input_pc == 0x802B4EECu ||
             dispatch_input_pc == 0x802B45BCu)) {
            fprintf(stderr,
                    "[sched] late-worker-command-return #%u input=0x%08X "
                    "r3=0x%08X r4=0x%08X r28=0x%08X r29=0x%08X "
                    "output=0x%08X output_r3=0x%08X output_r4=0x%08X "
                    "current=0x%08X context=0x%08X run_bits=0x%08X "
                    "worker_state=%u queue_used=%u command=0x%08X\n",
                    late_worker_command_return_trace_reports + 1u,
                    dispatch_input_pc, dispatch_input_r3, dispatch_input_r4,
                    dispatch_input_r28, dispatch_input_r29, cpu.pc, cpu.gpr[3],
                    cpu.gpr[4], mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u),
                    mem_read32(&cpu, 0x8039CD60u + 0x1Cu),
                    mem_read32(&cpu, 0x80ADDF70u));
            late_worker_command_return_trace_reports++;
        }
        if (g_runqueue_trace &&
            late_scheduler_post_command_trace_reports < 24u &&
            blocks >= 31000000ull && dispatch_input_pc == 0x802B40ECu) {
            fprintf(stderr,
                    "[sched] late-scheduler-post-command #%u input_r3=0x%08X "
                    "input_r29=0x%08X input_r30=0x%08X input_lr=0x%08X "
                    "output=0x%08X output_r3=0x%08X output_r29=0x%08X "
                    "output_r30=0x%08X output_lr=0x%08X current=0x%08X "
                    "context=0x%08X run_bits=0x%08X main_state=%u "
                    "main_queue=0x%08X worker_state=%u worker_queue=0x%08X\n",
                    late_scheduler_post_command_trace_reports + 1u,
                    dispatch_input_r3, dispatch_input_r29, dispatch_input_r30,
                    dispatch_input_lr, cpu.pc, cpu.gpr[3], cpu.gpr[29],
                    cpu.gpr[30], cpu.lr, mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read32(&cpu, 0x803A2960u + 0x2DCu),
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u),
                    mem_read32(&cpu, 0x804211E0u + 0x2DCu));
            late_scheduler_post_command_trace_reports++;
        }
        if (g_runqueue_trace &&
            late_scheduler_callback_trace_reports < 48u &&
            blocks >= 31000000ull &&
            ((dispatch_input_pc == 0x80328F40u &&
              dispatch_input_lr == 0x802B4100u) ||
             dispatch_input_pc == 0x802B4100u)) {
            fprintf(stderr,
                    "[sched] late-scheduler-callback #%u input=0x%08X "
                    "input_r3=0x%08X input_r4=0x%08X input_lr=0x%08X "
                    "output=0x%08X output_r3=0x%08X output_r4=0x%08X "
                    "output_lr=0x%08X current=0x%08X context=0x%08X "
                    "run_bits=0x%08X main_state=%u worker_state=%u\n",
                    late_scheduler_callback_trace_reports + 1u,
                    dispatch_input_pc, dispatch_input_r3, dispatch_input_r4,
                    dispatch_input_lr, cpu.pc, cpu.gpr[3], cpu.gpr[4], cpu.lr,
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u));
            late_scheduler_callback_trace_reports++;
        }
        if (g_runqueue_trace &&
            late_selector_callback_return_trace_reports < 24u &&
            blocks >= 31000000ull && dispatch_input_pc == 0x80307ED4u) {
            fprintf(stderr,
                    "[sched] late-selector-callback-return #%u input_r3=0x%08X "
                    "input_r30=0x%08X input_lr=0x%08X output=0x%08X "
                    "output_r3=0x%08X output_r30=0x%08X output_lr=0x%08X "
                    "current=0x%08X context=0x%08X run_bits=0x%08X "
                    "main_state=%u worker_state=%u\n",
                    late_selector_callback_return_trace_reports + 1u,
                    dispatch_input_r3, dispatch_input_r30, dispatch_input_lr,
                    cpu.pc, cpu.gpr[3], cpu.gpr[30], cpu.lr,
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u));
            late_selector_callback_return_trace_reports++;
        }
        if (g_runqueue_trace &&
            late_selector_post_dispatch_reports < 96u &&
            blocks >= 31000000ull &&
            (dispatch_input_pc == 0x80307FA0u ||
             dispatch_input_pc == 0x80303A50u ||
             dispatch_input_pc == 0x80307EF4u)) {
            fprintf(stderr,
                    "[sched] late-selector-post #%u input=0x%08X "
                    "output=0x%08X dispatched=%d srr0=0x%08X srr1=0x%08X "
                    "current=0x%08X context=0x%08X run_bits=0x%08X "
                    "main_state=%u worker_state=%u msr=0x%08X\n",
                    late_selector_post_dispatch_reports + 1u,
                    dispatch_input_pc, cpu.pc, dispatched, cpu.srr0, cpu.srr1,
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u), cpu.msr);
            late_selector_post_dispatch_reports++;
        }
        if (g_runqueue_trace &&
            late_scheduler_budget_trace_reports < 64u &&
            blocks >= 31000000ull &&
            (dispatch_input_pc == 0x802B40ECu ||
             dispatch_input_pc == 0x803039C4u ||
             dispatch_input_pc == 0x803039D0u ||
             dispatch_input_pc == 0x80305F50u ||
             dispatch_input_pc == 0x80307EF4u)) {
            fprintf(stderr,
                    "[sched] late-budget #%u input=0x%08X output=0x%08X "
                    "dispatched=%d downcount=%lld timebase=%llu "
                    "decrementer=0x%08X pending=%d exception=0x%08X "
                    "msr=0x%08X current=0x%08X context=0x%08X "
                    "run_bits=0x%08X\n",
                    late_scheduler_budget_trace_reports + 1u,
                    dispatch_input_pc, cpu.pc, dispatched,
                    (long long)cpu.downcount,
                    (unsigned long long)cpu.timebase,
                    g_guest_clock_decrementer,
                    g_guest_decrementer_pending ? 1 : 0,
                    cpu.exception, cpu.msr,
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read32(&cpu, 0x803F7A30u));
            late_scheduler_budget_trace_reports++;
        }
        if (dispatch_input_pc == 0x80240ED8u &&
            cpu.gpr[12] >= BLUEWAKE_DYNAMIC_SCRATCH_BASE &&
            cpu.gpr[12] < BLUEWAKE_DYNAMIC_SCRATCH_LIMIT) {
            rel_prolog_saved_r13 = cpu.gpr[13];
            rel_prolog_sda_pending = true;
            fprintf(stderr,
                    "[rel-abi] prolog-enter raw=0x%08X linked_pc=0x%08X "
                    "saved_r13=0x%08X\n",
                    cpu.gpr[12], cpu.pc, rel_prolog_saved_r13);
        }
        if (dispatch_input_pc == 0x80240ED8u &&
            (os_panic_vcall_after_reports < 16u ||
             (!os_panic_bad_prolog_reported && cpu.pc == 0x80F01794u))) {
            fprintf(stderr,
                    "[panic] vcall-after input=0x%08X output=0x%08X "
                    "r3=0x%08X r12=0x%08X ctr=0x%08X lr=0x%08X dispatched=%d\n",
                    dispatch_input_pc, cpu.pc, cpu.gpr[3], cpu.gpr[12],
                    cpu.ctr, cpu.lr, dispatched);
            if (cpu.pc == 0x80F01794u)
                os_panic_bad_prolog_reported = true;
            else
                os_panic_vcall_after_reports++;
        }
        if (g_rel_indirect_trace &&
            g_rel_indirect_trace_reports < 32u &&
            (dispatch_input_pc == 0x802411D0u ||
             dispatch_input_pc == 0x8155586Cu ||
             dispatch_input_pc == 0x802411D4u ||
             dispatch_input_pc == 0x8155016Cu ||
             dispatch_input_pc == 0x81550170u ||
             dispatch_input_pc == 0x81550174u ||
             dispatch_input_pc == 0x81550178u ||
             dispatch_input_pc == 0x8155017Cu ||
             dispatch_input_pc == 0x81550180u ||
             dispatch_input_pc == 0x8155010Cu ||
             dispatch_input_pc == 0x81550110u ||
             dispatch_input_pc == 0x815501B4u ||
             dispatch_input_pc == 0x815501B8u ||
             dispatch_input_pc == 0x815501BCu ||
             dispatch_input_pc == 0x815501C0u ||
             dispatch_input_pc == 0x81550348u)) {
            fprintf(stderr,
                    "[rel-indirect] after input=0x%08X output=0x%08X dispatched=%d "
                    "lr=0x%08X ctr=0x%08X r1=0x%08X r3=0x%08X "
                    "r5=0x%08X r6=0x%08X r12=0x%08X r31=0x%08X mem_r6=0x%08X mem_r31=0x%08X "
                    "mem_r5=0x%08X mem_r5_4=0x%08X mem_r5_8=0x%08X\n",
                    dispatch_input_pc, cpu.pc, dispatched, cpu.lr, cpu.ctr, cpu.gpr[1],
                    cpu.gpr[3], cpu.gpr[5], cpu.gpr[6], cpu.gpr[12], cpu.gpr[31],
                    mem_read32(&cpu, cpu.gpr[6]), mem_read32(&cpu, cpu.gpr[31]), mem_read32(&cpu, cpu.gpr[5]),
                    mem_read32(&cpu, cpu.gpr[5] + 4u),
                    mem_read32(&cpu, cpu.gpr[5] + 8u));
            g_rel_indirect_trace_reports++;
        }
        g_heap_write_watch_dispatch_pc = 0u;
        if (g_rel_destructors_trace &&
            g_rel_invalid_target_reports < 16u &&
            cpu.pc < 0x80000000u && cpu.lr == 0x802411D4u) {
            fprintf(stderr,
                    "[rel-call] invalid-target pc=0x%08X lr=0x%08X "
                    "r3=0x%08X r4=0x%08X r12=0x%08X r29=0x%08X "
                    "r31=0x%08X mem_r31=0x%08X\n",
                    cpu.pc, cpu.lr, cpu.gpr[3], cpu.gpr[4], cpu.gpr[12],
                    cpu.gpr[29], cpu.gpr[31], mem_read32(&cpu, cpu.gpr[31]));
            g_rel_invalid_target_reports++;
        }
        if (g_runqueue_trace) {
            const u32 heap = 0x8041D080u;
            const u32 tail = heap != 0u ? mem_read32(&cpu, heap + 0x7Cu) : 0u;
            if (tail == 0x80AD2140u) {
                const u32 current[6] = {
                    mem_read32(&cpu, tail + 0x04u),
                    mem_read32(&cpu, tail + 0x08u),
                    mem_read32(&cpu, tail + 0x0Cu),
                    mem_read32(&cpu, heap + 0x84u),
                    mem_read32(&cpu, heap + 0x80u),
                    0u,
                };
                bool changed = !heap_tail_watch_initialized;
                for (unsigned field = 0; field < 6u; field++)
                    changed |= current[field] != heap_tail_watch[field];
                if (changed && heap_tail_watch_reports < 256u) {
                    fprintf(stderr,
                            "[heap-watch] input=0x%08X output=0x%08X "
                            "dispatched=%d tail_size=0x%08X prev=0x%08X "
                            "next=0x%08X tail_used=0x%08X head_used=0x%08X "
                            "r3=0x%08X\n",
                            dispatch_input_pc, cpu.pc, dispatched, current[0],
                            current[1], current[2], current[3], current[4],
                            cpu.gpr[3]);
                    heap_tail_watch_reports++;
                }
                memcpy(heap_tail_watch, current, sizeof(current));
                heap_tail_watch_initialized = true;
            }
        }
        if (g_runqueue_trace && blocks >= 7000000ull &&
            heap_nominal_state_reports < 256u) {
            const u32 current[4] = {
                mem_read32(&cpu, 0x8041D0FCu),
                mem_read32(&cpu, 0x80AD2144u),
                mem_read32(&cpu, 0x80AD2148u),
                mem_read32(&cpu, 0x80AD214Cu),
            };
            bool changed = !heap_nominal_state_initialized;
            for (unsigned field = 0; field < 4u; field++)
                changed |= current[field] != heap_nominal_state[field];
            const bool interesting =
                current[0] == 0x80AD2140u || current[1] > 0x01000000u ||
                current[2] >= 0x80000000u || current[3] >= 0x80000000u;
            if (changed && interesting) {
                fprintf(stderr,
                        "[heap-state] input=0x%08X output=0x%08X "
                        "dispatched=%d tail=0x%08X size=0x%08X "
                        "prev=0x%08X next=0x%08X context=0x%08X\n",
                        dispatch_input_pc, cpu.pc, dispatched, current[0],
                        current[1], current[2], current[3],
                        mem_read32(&cpu, 0x800000D4u));
                heap_nominal_state_reports++;
            }
            memcpy(heap_nominal_state, current, sizeof(current));
            heap_nominal_state_initialized = true;
        }
        if (g_runqueue_trace &&
            display_alarm_trace_reports < 48u &&
            (dispatch_input_pc == 0x80301F44u ||
             dispatch_input_pc == 0x803021A4u ||
             dispatch_input_pc == 0x80255E54u ||
             dispatch_input_pc == 0x803086A4u) &&
            (dispatch_input_r3 == 0x8040CC78u ||
             dispatch_input_r3 == 0x803A2960u)) {
            const u32 alarm = 0x8040CC78u;
            fprintf(stderr,
                    "[alarm] return #%u input=0x%08X output=0x%08X "
                    "dispatched=%d handler=0x%08X tag=0x%08X fire_hi=0x%08X "
                    "fire_lo=0x%08X prev=0x%08X next=0x%08X period_hi=0x%08X "
                    "period_lo=0x%08X main_state=%u main_queue=0x%08X "
                    "run_bits=0x%08X current=0x%08X lr=0x%08X\n",
                    display_alarm_trace_reports + 1u, dispatch_input_pc, cpu.pc,
                    dispatched, mem_read32(&cpu, alarm + 0x00u),
                    mem_read32(&cpu, alarm + 0x04u), mem_read32(&cpu, alarm + 0x08u),
                    mem_read32(&cpu, alarm + 0x0Cu), mem_read32(&cpu, alarm + 0x10u),
                    mem_read32(&cpu, alarm + 0x14u), mem_read32(&cpu, alarm + 0x18u),
                    mem_read32(&cpu, alarm + 0x1Cu),
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read32(&cpu, 0x803A2960u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7A30u), mem_read32(&cpu, 0x800000E4u),
                    cpu.lr);
            display_alarm_trace_reports++;
        }
        if (g_runqueue_trace &&
            finish_wake_thread_switch_reports < 16u &&
            blocks >= 5000000ull && cpu.pc == 0x802B40ECu &&
            (mem_read16(&cpu, 0x803A2960u + 0x2C8u) == 1u ||
             mem_read16(&cpu, 0x803A2960u + 0x2C8u) == 2u)) {
            fprintf(stderr,
                    "[gx] FinishQueue thread-switch returned #%u "
                    "input=0x%08X output=0x%08X lr=0x%08X r3=0x%08X "
                    "main_state=%u main_queue=0x%08X main_srr0=0x%08X "
                    "main_srr1=0x%08X run_bits=0x%08X current_thread=0x%08X "
                    "current_context=0x%08X\n",
                    finish_wake_thread_switch_reports + 1u, dispatch_input_pc,
                    cpu.pc, cpu.lr, cpu.gpr[3],
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read32(&cpu, 0x803A2960u + 0x2DCu),
                    mem_read32(&cpu, 0x803A2960u + 0x198u),
                    mem_read32(&cpu, 0x803A2960u + 0x19Cu),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            finish_wake_thread_switch_reports++;
        }
        if (trace_finish_wake_thread_switch) {
            fprintf(stderr,
                    "[gx] FinishQueue thread-switch after #%u output=0x%08X "
                    "lr=0x%08X r3=0x%08X main_state=%u main_queue=0x%08X "
                    "main_srr0=0x%08X main_srr1=0x%08X run_bits=0x%08X "
                    "current_thread=0x%08X current_context=0x%08X\n",
                    finish_wake_thread_switch_reports + 1u, cpu.pc, cpu.lr,
                    cpu.gpr[3], mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read32(&cpu, 0x803A2960u + 0x2DCu),
                    mem_read32(&cpu, 0x803A2960u + 0x198u),
                    mem_read32(&cpu, 0x803A2960u + 0x19Cu),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            finish_wake_thread_switch_reports++;
        }
        if (g_runqueue_trace &&
            finish_queue_wakeup_restore_reports < 16u &&
            ((dispatch_input_pc == 0x80304630u && dispatch_input_lr == 0x80308C74u) ||
             dispatch_input_pc == 0x80308C74u)) {
            fprintf(stderr,
                    "[gx] FinishQueue OSWakeupThread restore after input=0x%08X "
                    "output=0x%08X dispatched=%d main_state=%u main_queue=0x%08X "
                    "finish_head=0x%08X finish_tail=0x%08X main_next=0x%08X "
                    "main_prev=0x%08X run_bits=0x%08X current_thread=0x%08X "
                    "current_context=0x%08X\n",
                    dispatch_input_pc, cpu.pc, dispatched,
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read32(&cpu, 0x803A2960u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7C9Cu),
                    mem_read32(&cpu, 0x803F7CA0u),
                    mem_read32(&cpu, 0x803A2960u + 0x2E0u),
                    mem_read32(&cpu, 0x803A2960u + 0x2E4u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            finish_queue_wakeup_restore_reports++;
        }
        if (g_runqueue_trace &&
            finish_queue_wakeup_body_reports < 16u &&
            dispatch_input_pc == 0x80308BA4u &&
            dispatch_input_r30 == 0x803F7C9Cu) {
            fprintf(stderr,
                    "[gx] FinishQueue OSWakeupThread body after output=0x%08X "
                    "dispatched=%d main_state=%u main_queue=0x%08X "
                    "finish_head=0x%08X finish_tail=0x%08X main_next=0x%08X "
                    "main_prev=0x%08X run_bits=0x%08X current_thread=0x%08X "
                    "current_context=0x%08X\n",
                    cpu.pc, dispatched,
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read32(&cpu, 0x803A2960u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7C9Cu),
                    mem_read32(&cpu, 0x803F7CA0u),
                    mem_read32(&cpu, 0x803A2960u + 0x2E0u),
                    mem_read32(&cpu, 0x803A2960u + 0x2E4u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            finish_queue_wakeup_body_reports++;
        }
        if (g_runqueue_trace &&
            finish_queue_wakeup_return_reports < 8u &&
            dispatch_input_pc == 0x80308B88u && dispatch_input_r3 == 0x803F7C9Cu) {
            fprintf(stderr,
                    "[gx] FinishQueue OSWakeupThread return output=0x%08X "
                    "dispatched=%d main_state=%u main_queue=0x%08X "
                    "finish_head=0x%08X finish_tail=0x%08X main_next=0x%08X "
                    "main_prev=0x%08X run_bits=0x%08X current_thread=0x%08X "
                    "current_context=0x%08X\n",
                    cpu.pc, dispatched,
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read32(&cpu, 0x803A2960u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7C9Cu),
                    mem_read32(&cpu, 0x803F7CA0u),
                    mem_read32(&cpu, 0x803A2960u + 0x2E0u),
                    mem_read32(&cpu, 0x803A2960u + 0x2E4u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            finish_queue_wakeup_return_reports++;
        }
        if (g_runqueue_trace &&
            gx_finish_return_reports < 8u && dispatch_input_pc == 0x80322F20u) {
            fprintf(stderr,
                    "[gx] GXFinishInterruptHandler return output=0x%08X "
                    "dispatched=%d draw_done=%u pe_status=0x%04X "
                    "finish_head=0x%08X finish_tail=0x%08X main_state=%u "
                    "main_queue=0x%08X run_bits=0x%08X current_thread=0x%08X "
                    "current_context=0x%08X\n",
                    cpu.pc, dispatched, mem_read8(&cpu, 0x803F7C98u),
                    (u16)dol_interrupts_mmio_read(&g_interrupts, 0xCC00100Au, 2u),
                    mem_read32(&cpu, 0x803F7C9Cu),
                    mem_read32(&cpu, 0x803F7CA0u),
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read32(&cpu, 0x803A2960u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            gx_finish_return_reports++;
        }
        if (g_runqueue_trace &&
            main_dispatch_return_reports < 16u &&
            dispatch_input_pc == 0x80308AB8u &&
            mem_read32(&cpu, 0x800000E4u) == 0x803A2960u) {
            fprintf(stderr,
                    "[sched] main-dispatch-return #%u input=0x%08X "
                    "output=0x%08X dispatched=%d downcount=%lld exception=0x%08X "
                    "msr=0x%08X lr=0x%08X r1=0x%08X r30=0x%08X r31=0x%08X\n",
                    main_dispatch_return_reports + 1u, dispatch_input_pc,
                    cpu.pc, dispatched, (long long)cpu.downcount, cpu.exception,
                    cpu.msr, cpu.lr, cpu.gpr[1], cpu.gpr[30], cpu.gpr[31]);
            fprintf(stderr,
                    "[sched] main-dispatch-state state=%u queue=0x%08X "
                    "next=0x%08X prev=0x%08X retrace_head=0x%08X "
                    "retrace_tail=0x%08X run_bits=0x%08X current_thread=0x%08X "
                    "current_context=0x%08X\n",
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read32(&cpu, 0x803A2960u + 0x2DCu),
                    mem_read32(&cpu, 0x803A2960u + 0x2E0u),
                    mem_read32(&cpu, 0x803A2960u + 0x2E4u),
                    mem_read32(&cpu, 0x803F7B44u),
                    mem_read32(&cpu, 0x803F7B48u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            main_dispatch_return_reports++;
        }
        if (g_runqueue_trace &&
            owner_send_return_reports < 48u && cpu.pc == 0x80305934u &&
            mem_read32(&cpu, 0x800000E4u) == 0x80425680u) {
            const u32 queue = cpu.gpr[28];
            fprintf(stderr,
                    "[osmsg] owner-send-return #%u queue=0x%08X result=%u "
                    "flags=%u used=%u capacity=%u recv_head=0x%08X "
                    "send_head=0x%08X thread_state=%u thread_queue=0x%08X "
                    "run_bits=0x%08X hint=%u\n",
                    owner_send_return_reports + 1u, queue, cpu.gpr[30],
                    cpu.gpr[31], mem_read32(&cpu, queue + 0x1Cu),
                    mem_read32(&cpu, queue + 0x14u),
                    mem_read32(&cpu, queue + 0x8u),
                    mem_read32(&cpu, queue + 0xCu),
                    mem_read16(&cpu, 0x80425680u + 0x2C8u),
                    mem_read32(&cpu, 0x80425680u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x803F7A34u));
            owner_send_return_reports++;
        }
        if (g_runqueue_trace &&
            selector_post_store_reports < 32u &&
            ((dispatch_input_pc == 0x80307FA0u && cpu.pc == 0x80303968u) ||
             (dispatch_input_pc == 0x80307FACu && cpu.pc == 0x80303A50u))) {
            const u32 current_thread = mem_read32(&cpu, 0x800000E4u);
            const u32 current_context = mem_read32(&cpu, 0x800000D4u);
            fprintf(stderr,
                    "[sched] selector-post-store #%u input=0x%08X "
                    "output=0x%08X selected=0x%08X current_thread=0x%08X "
                    "current_context=0x%08X worker_state=%u worker_queue=0x%08X "
                    "run_bits=0x%08X hint=%u reschedule=%d\n",
                    selector_post_store_reports + 1u, dispatch_input_pc,
                    cpu.pc, dispatch_input_r30, current_thread, current_context,
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u),
                    mem_read32(&cpu, 0x804211E0u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x803F7A34u),
                    (s32)mem_read32(&cpu, 0x803F7A38u));
            selector_post_store_reports++;
        }
        if (g_runqueue_trace &&
            selector_restore_branch_reports < 64u &&
            (dispatch_input_pc == 0x80307EA0u ||
             dispatch_input_pc == 0x80307EA4u ||
             dispatch_input_pc == 0x80307EA8u ||
             dispatch_input_pc == 0x80307EACu ||
             dispatch_input_pc == 0x80307EBCu)) {
            const u32 current_thread = mem_read32(&cpu, 0x800000E4u);
            const u32 current_context = mem_read32(&cpu, 0x800000D4u);
            fprintf(stderr,
                    "[sched] selector-restore-branch #%u pc=0x%08X "
                    "output=0x%08X r0=0x%08X cr=0x%08X r3=0x%08X "
                    "msr=0x%08X thread=0x%08X "
                    "context=0x%08X context_state=0x%04X saved_r3=0x%08X "
                    "saved_srr0=0x%08X saved_srr1=0x%08X run_bits=0x%08X "
                    "hint=%u reschedule=%d\n",
                    selector_restore_branch_reports + 1u, dispatch_input_pc,
                    cpu.pc, cpu.gpr[0], cpu.cr, cpu.gpr[3], cpu.msr, current_thread,
                    current_context,
                    current_context ? mem_read16(&cpu, current_context + 0x1A2u) : 0u,
                    current_context ? mem_read32(&cpu, current_context + 0x0Cu) : 0u,
                    current_context ? mem_read32(&cpu, current_context + 0x198u) : 0u,
                    current_context ? mem_read32(&cpu, current_context + 0x19Cu) : 0u,
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x803F7A34u),
                    (s32)mem_read32(&cpu, 0x803F7A38u));
            selector_restore_branch_reports++;
        }
        if (trace_priority8_resume) {
            fprintf(stderr,
                    "[sched] priority8-selector after #%u output=0x%08X "
                    "r3=0x%08X r1=0x%08X lr=0x%08X current_thread=0x%08X "
                    "context=0x%08X state=%u queue=0x%08X run_bits=0x%08X\n",
                    selector_priority8_resume_reports + 1u, cpu.pc,
                    cpu.gpr[3], cpu.gpr[1], cpu.lr,
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read16(&cpu, 0x80425680u + 0x2C8u),
                    mem_read32(&cpu, 0x80425680u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7A30u));
            selector_priority8_resume_reports++;
        }
        if (g_runqueue_trace &&
            aram_late_context_reports < 32u && blocks >= 2500000ull &&
            dispatch_input_pc == 0x80303968u &&
            mem_read32(&cpu, 0x800000E4u) == 0x804211E0u) {
            const u32 current_context = mem_read32(&cpu, 0x800000D4u);
            fprintf(stderr,
                    "[sched] aram-late-context after #%u output_pc=0x%08X "
                    "output_lr=0x%08X thread=0x%08X context=0x%08X "
                    "worker_saved_pc=0x%08X run_bits=0x%08X\n",
                    aram_late_context_reports + 1u, cpu.pc, cpu.lr,
                    mem_read32(&cpu, 0x800000E4u), current_context,
                    mem_read32(&cpu, 0x804211E0u + 0x198u),
                    mem_read32(&cpu, 0x803F7A30u));
            aram_late_context_reports++;
        }
        if (g_runqueue_trace &&
            aram_late_selectthread_reports < 32u && blocks >= 2500000ull &&
            dispatch_input_pc == 0x80304DF8u && cpu.pc == 0x80307EACu &&
            mem_read32(&cpu, 0x800000E4u) == 0x804211E0u) {
            fprintf(stderr,
                    "[sched] aram-late-selectthread after #%u output_pc=0x%08X "
                    "output_lr=0x%08X thread=0x%08X context=0x%08X "
                    "worker_state=%u worker_effective=%d worker_saved_pc=0x%08X "
                    "run_bits=0x%08X hint=%u reschedule=%d dispatched=%d\n",
                    aram_late_selectthread_reports + 1u, cpu.pc, cpu.lr,
                    mem_read32(&cpu, 0x800000E4u), mem_read32(&cpu, 0x800000D4u),
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u),
                    (s32)mem_read32(&cpu, 0x804211E0u + 0x2D0u),
                    mem_read32(&cpu, 0x804211E0u + 0x198u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x803F7A34u),
                    (s32)mem_read32(&cpu, 0x803F7A38u), dispatched);
            aram_late_selectthread_reports++;
        }
        if (g_runqueue_trace &&
            selector_load_handoff_reports < 32u &&
            dispatch_input_pc == 0x80303A50u &&
            selector_selected_context != 0u &&
            dispatch_input_r3 == selector_selected_context) {
            fprintf(stderr,
                    "[sched] selector-load after #%u output=0x%08X "
                    "lr=0x%08X r3=0x%08X r1=0x%08X msr=0x%08X "
                    "saved_r3=0x%08X saved_r1=0x%08X saved_lr=0x%08X "
                    "current_thread=0x%08X current_context=0x%08X "
                    "run_bits=0x%08X worker_state=%u worker_queue=0x%08X\n",
                    selector_load_handoff_reports + 1u, cpu.pc, cpu.lr,
                    cpu.gpr[3], cpu.gpr[1], cpu.msr,
                    mem_read32(&cpu, dispatch_input_r3 + 0x0Cu),
                    mem_read32(&cpu, dispatch_input_r3 + 0x04u),
                    mem_read32(&cpu, dispatch_input_r3 + 0x84u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u),
                    mem_read32(&cpu, 0x804211E0u + 0x2DCu));
            selector_load_handoff_reports++;
            selector_selected_context = 0;
        }
        if (g_runqueue_trace &&
            aram_late_selector_reports < 16u && blocks >= 2500000ull &&
            dispatch_input_pc == 0x80307FA0u && dispatch_input_r30 == 0x804211E0u) {
            const u32 selected = dispatch_input_r30;
            fprintf(stderr,
                    "[sched] aram-late-selector #%u selected=0x%08X "
                    "saved_srr0=0x%08X saved_srr1=0x%08X state=%u "
                    "queue=0x%08X run_bits=0x%08X p14_head=0x%08X "
                    "p16_head=0x%08X current_thread=0x%08X current_context=0x%08X\n",
                    aram_late_selector_reports + 1u, selected,
                    mem_read32(&cpu, selected + 0x198u),
                    mem_read32(&cpu, selected + 0x19Cu),
                    mem_read16(&cpu, selected + 0x2C8u),
                    mem_read32(&cpu, selected + 0x2DCu),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x803F0328u),
                    mem_read32(&cpu, 0x803F0338u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            aram_late_selector_reports++;
        }
        if (g_runqueue_trace &&
            aram_worker_wake_boundary_reports < 32u &&
            dispatch_input_pc == 0x80304608u &&
            dispatch_input_r3 == 0x8039CD68u) {
            fprintf(stderr,
                    "[sched] aram-worker-wake before #%u queue=0x%08X "
                    "head=0x%08X tail=0x%08X worker_state=%u "
                    "worker_queue=0x%08X run_bits=0x%08X current_thread=0x%08X "
                    "current_context=0x%08X manager_used=%u manager_first=%u\n",
                    aram_worker_wake_boundary_reports + 1u,
                    dispatch_input_r3, mem_read32(&cpu, dispatch_input_r3),
                    mem_read32(&cpu, dispatch_input_r3 + 4u),
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u),
                    mem_read32(&cpu, 0x804211E0u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read32(&cpu, 0x8039CD60u + 0x1Cu),
                    mem_read32(&cpu, 0x8039CD60u + 0x18u));
        }
        if (g_runqueue_trace &&
            aram_wakeup_callsite_reports < 48u &&
            dispatch_input_pc == 0x80304608u &&
            dispatch_input_r3 == 0x8039CD68u) {
            const u32 worker = 0x804211E0u;
            fprintf(stderr,
                    "[sched] aram-sleep-disable-boundary #%u input_lr=0x%08X "
                    "output=0x%08X worker_state=%u worker_queue=0x%08X "
                    "worker_saved_pc=0x%08X manager_used=%u manager_first=%u "
                    "wait_head=0x%08X wait_tail=0x%08X run_bits=0x%08X "
                    "current_thread=0x%08X current_context=0x%08X\n",
                    aram_wakeup_callsite_reports + 1u, dispatch_input_lr,
                    cpu.pc, mem_read16(&cpu, worker + 0x2C8u),
                    mem_read32(&cpu, worker + 0x2DCu),
                    mem_read32(&cpu, worker + 0x198u),
                    mem_read32(&cpu, 0x8039CD60u + 0x1Cu),
                    mem_read32(&cpu, 0x8039CD60u + 0x18u),
                    mem_read32(&cpu, 0x8039CD68u),
                    mem_read32(&cpu, 0x8039CD6Cu),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            aram_wakeup_callsite_reports++;
        }
        if (g_runqueue_trace &&
            aram_real_wakeup_reports < 48u && cpu.pc == 0x80308B88u &&
            cpu.gpr[3] == 0x8039CD68u) {
            const u32 worker = 0x804211E0u;
            fprintf(stderr,
                    "[sched] aram-real-wakeup #%u lr=0x%08X "
                    "worker_state=%u worker_queue=0x%08X "
                    "wait_head=0x%08X wait_tail=0x%08X manager_used=%u "
                    "run_bits=0x%08X current_thread=0x%08X current_context=0x%08X\n",
                    aram_real_wakeup_reports + 1u, cpu.lr,
                    mem_read16(&cpu, worker + 0x2C8u),
                    mem_read32(&cpu, worker + 0x2DCu),
                    mem_read32(&cpu, 0x8039CD68u),
                    mem_read32(&cpu, 0x8039CD6Cu),
                    mem_read32(&cpu, 0x8039CD60u + 0x1Cu),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            aram_real_wakeup_reports++;
        }
        if (g_runqueue_trace &&
            aram_worker_wake_boundary_reports < 32u &&
            dispatch_input_pc == 0x80304608u &&
            dispatch_input_r3 == 0x8039CD68u) {
            fprintf(stderr,
                    "[sched] aram-worker-wake after #%u output=0x%08X "
                    "lr=0x%08X queue_head=0x%08X queue_tail=0x%08X "
                    "worker_state=%u worker_queue=0x%08X run_bits=0x%08X "
                    "current_thread=0x%08X current_context=0x%08X\n",
                    aram_worker_wake_boundary_reports + 1u, cpu.pc, cpu.lr,
                    mem_read32(&cpu, dispatch_input_r3),
                    mem_read32(&cpu, dispatch_input_r3 + 4u),
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u),
                    mem_read32(&cpu, 0x804211E0u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            aram_worker_wake_boundary_reports++;
        }
        if (g_runqueue_trace &&
            aram_worker_wake_resume_reports < 32u &&
            dispatch_input_pc == 0x80308BA4u &&
            dispatch_input_r30 == 0x8039CD68u) {
            fprintf(stderr,
                    "[sched] aram-worker-wake resume-before #%u "
                    "queue=0x%08X head=0x%08X tail=0x%08X "
                    "worker_state=%u worker_queue=0x%08X run_bits=0x%08X "
                    "current_thread=0x%08X current_context=0x%08X\n",
                    aram_worker_wake_resume_reports + 1u, dispatch_input_r30,
                    mem_read32(&cpu, dispatch_input_r30),
                    mem_read32(&cpu, dispatch_input_r30 + 4u),
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u),
                    mem_read32(&cpu, 0x804211E0u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
        }
        if (g_runqueue_trace &&
            aram_worker_wake_resume_reports < 32u &&
            dispatch_input_pc == 0x80308BA4u &&
            dispatch_input_r30 == 0x8039CD68u) {
            fprintf(stderr,
                    "[sched] aram-worker-wake resume-after #%u "
                    "output=0x%08X lr=0x%08X queue_head=0x%08X "
                    "queue_tail=0x%08X worker_state=%u worker_queue=0x%08X "
                    "run_bits=0x%08X current_thread=0x%08X "
                    "current_context=0x%08X\n",
                    aram_worker_wake_resume_reports + 1u, cpu.pc, cpu.lr,
                    mem_read32(&cpu, dispatch_input_r30),
                    mem_read32(&cpu, dispatch_input_r30 + 4u),
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u),
                    mem_read32(&cpu, 0x804211E0u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            aram_worker_wake_resume_reports++;
        }
        if (g_runqueue_trace &&
            aram_manager_send_return_reports < 32u &&
            dispatch_input_pc == 0x803059A4u &&
            dispatch_input_r28 == 0x8039CD60u) {
            fprintf(stderr,
                    "[sched] aram-manager-send-return #%u queue=0x%08X "
                    "message=0x%08X used=%u first=%u capacity=%u "
                    "recv_head=0x%08X recv_tail=0x%08X worker_state=%u "
                    "worker_queue=0x%08X run_bits=0x%08X output=0x%08X\n",
                    aram_manager_send_return_reports + 1u,
                    dispatch_input_r28, dispatch_input_r30,
                    mem_read32(&cpu, dispatch_input_r28 + 0x1Cu),
                    mem_read32(&cpu, dispatch_input_r28 + 0x18u),
                    mem_read32(&cpu, dispatch_input_r28 + 0x14u),
                    mem_read32(&cpu, dispatch_input_r28 + 0x8u),
                    mem_read32(&cpu, dispatch_input_r28 + 0xCu),
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u),
                    mem_read32(&cpu, 0x804211E0u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7A30u), cpu.pc);
            aram_manager_send_return_reports++;
        }
        if (g_runqueue_trace &&
            aram_manager_restore_reports < 32u &&
            dispatch_input_pc == 0x80304630u &&
            dispatch_input_lr == 0x803059ACu) {
            const u32 queue = dispatch_input_r28;
            fprintf(stderr,
                    "[sched] aram-manager-restore #%u queue=0x%08X "
                    "message=0x%08X used=%u first=%u capacity=%u "
                    "recv_head=0x%08X recv_tail=0x%08X worker_state=%u "
                    "worker_queue=0x%08X run_bits=0x%08X output=0x%08X "
                    "r3=0x%08X r30=0x%08X r31=0x%08X\n",
                    aram_manager_restore_reports + 1u, queue,
                    dispatch_input_r30, mem_read32(&cpu, queue + 0x1Cu),
                    mem_read32(&cpu, queue + 0x18u),
                    mem_read32(&cpu, queue + 0x14u),
                    mem_read32(&cpu, queue + 0x8u),
                    mem_read32(&cpu, queue + 0xCu),
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u),
                    mem_read32(&cpu, 0x804211E0u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7A30u), cpu.pc,
                    cpu.gpr[3], dispatch_input_r30, dispatch_input_r31);
            aram_manager_restore_reports++;
        }
        if (g_runqueue_trace &&
            aram_receive_command_boundary_reports < 32u &&
            dispatch_input_pc == 0x80304608u &&
            dispatch_input_lr == 0x803059FCu &&
            mem_read32(&cpu, 0x800000E4u) == 0x804211E0u) {
            fprintf(stderr,
                    "[sched] aram-receive-command-boundary #%u "
                    "input=0x%08X output=0x%08X input_lr=0x%08X "
                    "r28=0x%08X r29=0x%08X r31=0x%08X manager_used=%u "
                    "output_ptr_value=0x%08X worker_state=%u\n",
                    aram_receive_command_boundary_reports + 1u,
                    dispatch_input_pc, cpu.pc, dispatch_input_lr,
                    dispatch_input_r28, dispatch_input_r29, dispatch_input_r31,
                    mem_read32(&cpu, 0x8039CD60u + 0x1Cu),
                    dispatch_input_r28 != 0u ?
                        mem_read32(&cpu, dispatch_input_r28) : 0u,
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u));
            aram_receive_command_boundary_reports++;
        }
        if (g_runqueue_trace &&
            aram_receive_continuation_reports < 32u &&
            dispatch_input_pc == 0x803059FCu &&
            mem_read32(&cpu, 0x800000E4u) == 0x804211E0u) {
            const u32 context = mem_read32(&cpu, 0x800000D4u);
            const u32 output_ptr = cpu.gpr[28];
            const u32 message = output_ptr != 0u ? mem_read32(&cpu, output_ptr) : 0u;
            const u32 command = message != 0u ? mem_read32(&cpu, message + 0x04u) : 0u;
            fprintf(stderr,
                    "[sched] aram-receive-continuation #%u "
                    "input=0x%08X output=0x%08X lr=0x%08X "
                    "r3=0x%08X cr=0x%08X context=0x%08X "
                    "context_srr0=0x%08X worker_saved_pc=0x%08X "
                    "worker_state=%u worker_queue=0x%08X dispatched=%d "
                    "manager_used=%u manager_first=%u r4=0x%08X "
                    "r28=0x%08X out_value=0x%08X message=0x%08X "
                    "command=0x%08X type=%u length=%u source=0x%08X "
                    "destination=0x%08X command_field4c=0x%08X\n",
                    aram_receive_continuation_reports + 1u,
                    dispatch_input_pc, cpu.pc, cpu.lr, cpu.gpr[3], cpu.cr,
                    context, context ? mem_read32(&cpu, context + 0x198u) : 0u,
                    mem_read32(&cpu, 0x804211E0u + 0x198u),
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u),
                    mem_read32(&cpu, 0x804211E0u + 0x2DCu), dispatched,
                    mem_read32(&cpu, 0x8039CD60u + 0x1Cu),
                    mem_read32(&cpu, 0x8039CD60u + 0x18u), cpu.gpr[4],
                    output_ptr, message, message, command,
                    command != 0u ? mem_read32(&cpu, command + 0x00u) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x08u) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x10u) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x14u) : 0u,
                    command != 0u ? mem_read32(&cpu, command + 0x4Cu) : 0u);
            aram_receive_continuation_reports++;
        }
        if (g_runqueue_trace &&
            aram_late_receive_reports < 16u && blocks >= 2500000ull &&
            dispatch_input_pc == 0x803059D0u &&
            dispatch_input_r3 == 0x8039CD60u &&
            mem_read32(&cpu, 0x800000E4u) == 0x804211E0u) {
            fprintf(stderr,
                    "[sched] aram-late-receive #%u used=%u first=%u "
                    "worker_state=%u worker_queue=0x%08X saved_pc=0x%08X "
                    "run_bits=0x%08X reschedule=%d wait_head=0x%08X "
                    "wait_tail=0x%08X\n",
                    aram_late_receive_reports + 1u,
                    mem_read32(&cpu, 0x8039CD60u + 0x1Cu),
                    mem_read32(&cpu, 0x8039CD60u + 0x18u),
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u),
                    mem_read32(&cpu, 0x804211E0u + 0x2DCu),
                    mem_read32(&cpu, 0x804211E0u + 0x198u),
                    mem_read32(&cpu, 0x803F7A30u),
                    (s32)mem_read32(&cpu, 0x803F7A38u),
                    mem_read32(&cpu, 0x8039CD60u + 0x8u),
                    mem_read32(&cpu, 0x8039CD60u + 0xCu));
            aram_late_receive_reports++;
        }
        if (g_runqueue_trace &&
            aram_sleep_continuation_reports < 32u &&
            dispatch_input_pc == 0x80308AB8u &&
            mem_read32(&cpu, 0x800000E4u) == 0x804211E0u) {
            const u32 worker = 0x804211E0u;
            fprintf(stderr,
                    "[sched] aram-sleep-continuation #%u "
                    "input=0x%08X output=0x%08X lr=0x%08X "
                    "worker_state=%u worker_queue=0x%08X "
                    "worker_saved_pc=0x%08X current_context=0x%08X "
                    "run_bits=0x%08X hint=%u reschedule=%d dispatched=%d\n",
                    aram_sleep_continuation_reports + 1u, dispatch_input_pc,
                    cpu.pc, cpu.lr, mem_read16(&cpu, worker + 0x2C8u),
                    mem_read32(&cpu, worker + 0x2DCu),
                    mem_read32(&cpu, worker + 0x198u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x803F7A34u),
                    (s32)mem_read32(&cpu, 0x803F7A38u), dispatched);
            aram_sleep_continuation_reports++;
        }
        if (g_runqueue_trace &&
            aram_late_sleep_reports < 16u && blocks >= 2500000ull &&
            dispatch_input_pc == 0x80308AB8u &&
            mem_read32(&cpu, 0x800000E4u) == 0x804211E0u) {
            fprintf(stderr,
                    "[sched] aram-late-sleep #%u worker_state=%u "
                    "worker_queue=0x%08X saved_pc=0x%08X run_bits=0x%08X "
                    "reschedule=%d current_thread=0x%08X current_context=0x%08X\n",
                    aram_late_sleep_reports + 1u,
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u),
                    mem_read32(&cpu, 0x804211E0u + 0x2DCu),
                    mem_read32(&cpu, 0x804211E0u + 0x198u),
                    mem_read32(&cpu, 0x803F7A30u),
                    (s32)mem_read32(&cpu, 0x803F7A38u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            aram_late_sleep_reports++;
        }
        if (g_runqueue_trace &&
            aram_reschedule_boundary_reports < 32u &&
            (dispatch_input_pc == 0x803039C4u ||
             dispatch_input_pc == 0x80307DE0u) &&
            mem_read32(&cpu, 0x800000E4u) == 0x804211E0u) {
            const u32 worker = 0x804211E0u;
            fprintf(stderr,
                    "[sched] aram-reschedule-boundary #%u "
                    "input=0x%08X output=0x%08X lr=0x%08X r3=0x%08X "
                    "current_thread=0x%08X current_context=0x%08X "
                    "worker_state=%u worker_queue=0x%08X "
                    "worker_saved_pc=0x%08X run_bits=0x%08X "
                    "hint=%u reschedule=%d dispatched=%d\n",
                    aram_reschedule_boundary_reports + 1u, dispatch_input_pc,
                    cpu.pc, cpu.lr, cpu.gpr[3],
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read16(&cpu, worker + 0x2C8u),
                    mem_read32(&cpu, worker + 0x2DCu),
                    mem_read32(&cpu, worker + 0x198u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x803F7A34u),
                    (s32)mem_read32(&cpu, 0x803F7A38u), dispatched);
            aram_reschedule_boundary_reports++;
        }
        if (g_runqueue_trace &&
            selector_handoff_reports < 48u &&
            (dispatch_input_pc == 0x803039D0u ||
             dispatch_input_pc == 0x80307EACu ||
             dispatch_input_pc == 0x80307FA0u ||
             dispatch_input_pc == 0x80307FB0u ||
             dispatch_input_pc == 0x80307FD0u) &&
            (mem_read32(&cpu, 0x800000E4u) == 0x804211E0u ||
             mem_read32(&cpu, 0x800000D4u) == 0x804211E0u)) {
            const u32 worker = 0x804211E0u;
            const u32 selected = cpu.gpr[30];
            fprintf(stderr,
                    "[sched] selector-handoff #%u input=0x%08X "
                    "output=0x%08X lr=0x%08X r3=0x%08X r30=0x%08X "
                    "current_thread=0x%08X current_context=0x%08X "
                    "worker_state=%u worker_queue=0x%08X "
                    "worker_saved_pc=0x%08X worker_saved_lr=0x%08X "
                    "worker_context_state=0x%04X r1=0x%08X run_bits=0x%08X hint=%u "
                    "reschedule=%d p14=0x%08X p16=0x%08X selected_state=%u "
                    "selected_priority=%d selected_queue=0x%08X dispatched=%d\n",
                    selector_handoff_reports + 1u, dispatch_input_pc, cpu.pc,
                    cpu.lr, cpu.gpr[3], cpu.gpr[30],
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read16(&cpu, worker + 0x2C8u),
                    mem_read32(&cpu, worker + 0x2DCu),
                    mem_read32(&cpu, worker + 0x198u),
                    mem_read32(&cpu, worker + 0x84u),
                    mem_read16(&cpu, worker + 0x1A2u),
                    cpu.gpr[1],
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x803F7A34u),
                    (s32)mem_read32(&cpu, 0x803F7A38u),
                    mem_read32(&cpu, 0x803F0328u),
                    mem_read32(&cpu, 0x803F0338u),
                    selected ? mem_read16(&cpu, selected + 0x2C8u) : 0u,
                    selected ? (s32)mem_read32(&cpu, selected + 0x2D0u) : 0,
                    selected ? mem_read32(&cpu, selected + 0x2DCu) : 0u,
                    dispatched);
            selector_handoff_reports++;
        }
        if (g_runqueue_trace &&
            selector_handoff_reports < 96u &&
            (dispatch_input_pc == 0x80307EBCu ||
             dispatch_input_pc == 0x80307ED4u ||
             dispatch_input_pc == 0x80307FACu) &&
            (mem_read32(&cpu, 0x800000E4u) == 0x804211E0u ||
             mem_read32(&cpu, 0x800000D4u) == 0x804211E0u)) {
            fprintf(stderr,
                    "[sched] selector-post-restore #%u input=0x%08X "
                    "output=0x%08X r3=0x%08X r4=0x%08X r6=0x%08X "
                    "r12=0x%08X r30=0x%08X lr=0x%08X run_bits=0x%08X "
                    "thread=0x%08X context=0x%08X worker_state=%u "
                    "worker_queue=0x%08X\n",
                    selector_handoff_reports + 1u, dispatch_input_pc, cpu.pc,
                    cpu.gpr[3], cpu.gpr[4], cpu.gpr[6], cpu.gpr[12],
                    cpu.gpr[30], cpu.lr, mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read16(&cpu, 0x804211E0u + 0x2C8u),
                    mem_read32(&cpu, 0x804211E0u + 0x2DCu));
            selector_handoff_reports++;
        }
        if (g_runqueue_trace &&
            selector_handoff_reports < 128u &&
            (dispatch_input_pc == 0x80307F30u ||
             dispatch_input_pc == 0x80307F34u ||
             dispatch_input_pc == 0x80307F38u) &&
            (mem_read32(&cpu, 0x800000E4u) == 0x804211E0u ||
             mem_read32(&cpu, 0x800000D4u) == 0x804211E0u)) {
            fprintf(stderr,
                    "[sched] selector-queue-head #%u input=0x%08X "
                    "output=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X "
                    "r7=%u r30=0x%08X lr=0x%08X run_bits=0x%08X "
                    "thread=0x%08X context=0x%08X selected_state=%u "
                    "selected_priority=%d selected_queue=0x%08X\n",
                    selector_handoff_reports + 1u, dispatch_input_pc, cpu.pc,
                    cpu.gpr[4], cpu.gpr[5], cpu.gpr[6], cpu.gpr[7],
                    cpu.gpr[30], cpu.lr, mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read16(&cpu, cpu.gpr[5] + 0x2C8u),
                    (s32)mem_read32(&cpu, cpu.gpr[5] + 0x2D0u),
                    mem_read32(&cpu, cpu.gpr[5] + 0x2DCu));
            selector_handoff_reports++;
        }
        if (g_runqueue_trace &&
            aram_receive_interrupt_boundary_reports < 32u &&
            dispatch_input_pc == 0x80304608u &&
            dispatch_input_lr == 0x803059FCu &&
            mem_read32(&cpu, 0x800000E4u) == 0x804211E0u) {
            const u32 context = mem_read32(&cpu, 0x800000D4u);
            fprintf(stderr,
                    "[sched] aram-receive-interrupt after #%u "
                    "output=0x%08X lr=0x%08X context=0x%08X "
                    "context_srr0=0x%08X context_srr1=0x%08X "
                    "worker_saved_pc=0x%08X msr=0x%08X dispatched=%d\n",
                    aram_receive_interrupt_boundary_reports + 1u, cpu.pc,
                    cpu.lr, context,
                    context ? mem_read32(&cpu, context + 0x198u) : 0u,
                    context ? mem_read32(&cpu, context + 0x19Cu) : 0u,
                    mem_read32(&cpu, 0x804211E0u + 0x198u), cpu.msr,
                    dispatched);
            aram_receive_interrupt_boundary_reports++;
        }
        if (g_runqueue_trace &&
            aram_worker_receive_dispatch_reports < 24u &&
            dispatch_input_pc == 0x803059D0u &&
            dispatch_input_r3 == 0x8039CD60u &&
            mem_read32(&cpu, 0x800000E4u) == 0x804211E0u) {
            const u32 current_context = mem_read32(&cpu, 0x800000D4u);
            const u32 worker = 0x804211E0u;
            fprintf(stderr,
                    "[sched] aram-worker-receive dispatch #%u input=0x%08X "
                    "output=0x%08X lr=0x%08X dispatched=%d worker_state=%u "
                    "worker_queue=0x%08X worker_saved_pc=0x%08X "
                    "context=0x%08X context_srr0=0x%08X context_srr1=0x%08X "
                    "run_bits=0x%08X run_hint=%u reschedule=%d\n",
                    aram_worker_receive_dispatch_reports + 1u,
                    dispatch_input_pc, cpu.pc, cpu.lr, dispatched,
                    mem_read16(&cpu, worker + 0x2C8u),
                    mem_read32(&cpu, worker + 0x2DCu),
                    mem_read32(&cpu, worker + 0x198u), current_context,
                    current_context ? mem_read32(&cpu, current_context + 0x198u) : 0u,
                    current_context ? mem_read32(&cpu, current_context + 0x19Cu) : 0u,
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x803F7A34u),
                    (s32)mem_read32(&cpu, 0x803F7A38u));
            aram_worker_receive_dispatch_reports++;
        }
        if (g_runqueue_trace &&
            aram_ripper_sync_trace_reports < 40u &&
            dispatch_input_pc == 0x802BDFE0u) {
            const u32 command = dispatch_input_r29;
            const u32 dvd_file = dispatch_input_r31;
            fprintf(stderr,
                    "[aram] ripper-cleanup-return #%u input=0x%08X "
                    "output=0x%08X command=0x%08X field44=0x%08X "
                    "link_prev=0x%08X link_next=0x%08X stream_command=0x%08X "
                    "file_stream=0x%08X file_thread=0x%08X lr=0x%08X\n",
                    aram_ripper_sync_trace_reports + 1u, dispatch_input_pc,
                    cpu.pc, command, mem_read32(&cpu, command + 0x44u),
                    mem_read32(&cpu, command + 0x00u),
                    mem_read32(&cpu, command + 0x04u),
                    mem_read32(&cpu, command + 0x4Cu),
                    mem_read32(&cpu, dvd_file + 0x54u),
                    mem_read32(&cpu, dvd_file + 0x50u), cpu.lr);
            aram_ripper_sync_trace_reports++;
        }
        if (g_runqueue_trace &&
            aram_cleanup_call_trace_reports < 24u &&
            ((dispatch_input_pc == 0x802B0D28u &&
              dispatch_input_lr == 0x802BE01Cu) ||
             (dispatch_input_pc == 0x80305F70u &&
              dispatch_input_lr == 0x802BE05Cu))) {
            fprintf(stderr,
                    "[aram] ripper-cleanup-call #%u input=0x%08X "
                    "output=0x%08X r3=0x%08X r4=0x%08X lr=0x%08X "
                    "thread=0x%08X\n",
                    aram_cleanup_call_trace_reports + 1u, dispatch_input_pc,
                    cpu.pc, cpu.gpr[3], cpu.gpr[4], cpu.lr,
                    mem_read32(&cpu, 0x800000E4u));
            aram_cleanup_call_trace_reports++;
        }
        if (g_runqueue_trace &&
            aram_cleanup_unlock_return_reports < 8u &&
            dispatch_input_pc == 0x80304608u &&
            dispatch_input_lr == 0x80305F90u &&
            ((dispatch_input_r3 == 0x803A7024u &&
              aram_ripper_command_mutex_a != 0u) ||
             (dispatch_input_r3 == 0x806AD6B4u &&
              aram_ripper_command_mutex_b != 0u)) &&
            (mem_read32(&cpu, 0x800000E4u) == 0x803A5F90u ||
             mem_read32(&cpu, 0x800000E4u) == 0x806AD7E0u)) {
            const bool mutex_a = dispatch_input_r3 == 0x803A7024u;
            const u32 command = mutex_a ? aram_ripper_command_mutex_a
                                        : aram_ripper_command_mutex_b;
            const u32 dvd_file = mutex_a ? aram_ripper_dvd_mutex_a
                                          : aram_ripper_dvd_mutex_b;
            fprintf(stderr,
                    "[aram] ripper-unlock-return #%u input=0x%08X "
                    "output=0x%08X r3=0x%08X r4=0x%08X lr=0x%08X "
                    "thread=0x%08X command=0x%08X field44=0x%08X "
                    "link_prev=0x%08X link_next=0x%08X stream_command=0x%08X "
                    "file_stream=0x%08X file_thread=0x%08X "
                    "run_bits=0x%08X current_thread=0x%08X "
                    "main_state=%u main_queue=0x%08X jas_used=%u\n",
                    aram_cleanup_unlock_return_reports + 1u,
                    dispatch_input_pc, cpu.pc, cpu.gpr[3], cpu.gpr[4],
                    cpu.lr, mem_read32(&cpu, 0x800000E4u),
                    command, mem_read32(&cpu, command + 0x44u),
                    mem_read32(&cpu, command + 0x00u),
                    mem_read32(&cpu, command + 0x04u),
                    mem_read32(&cpu, command + 0x4Cu),
                    mem_read32(&cpu, dvd_file + 0x54u),
                    mem_read32(&cpu, dvd_file + 0x50u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read32(&cpu, 0x803A2960u + 0x2DCu),
                    mem_read32(&cpu, 0x806AC790u + 0x1Cu));
            aram_cleanup_unlock_return_reports++;
        }
        if (g_runqueue_trace &&
            aram_ripper_caller_trace_reports < 16u &&
            (dispatch_input_pc == 0x802B0D28u ||
             dispatch_input_pc == 0x802BDAE8u ||
             dispatch_input_pc == 0x802BDB08u)) {
            const u32 command = cpu.gpr[31];
            fprintf(stderr,
                    "[aram] ripper-caller #%u input=0x%08X lr=0x%08X "
                    "output=0x%08X command=0x%08X field44=0x%08X "
                    "r3=0x%08X r4=0x%08X\n",
                    aram_ripper_caller_trace_reports + 1u, dispatch_input_pc,
                    dispatch_input_lr, cpu.pc, command,
                    mem_read32(&cpu, command + 0x44u), cpu.gpr[3], cpu.gpr[4]);
            aram_ripper_caller_trace_reports++;
        }
        if (g_runqueue_trace &&
            aram_isr_segment_trace_reports < 32u &&
            dispatch_input_pc == 0x80318DD4u &&
            mem_read32(&cpu, 0x800000E4u) == 0x804211E0u) {
            fprintf(stderr,
                    "[aram] isr-segment #%u input=0x%08X output=0x%08X "
                    "input_lr=0x%08X output_lr=0x%08X thread=0x%08X "
                    "context=0x%08X pending_lo=0x%08X callback_lo=0x%08X "
                    "pending_hi=0x%08X callback_hi=0x%08X queue_used=%u\n",
                    aram_isr_segment_trace_reports + 1u, dispatch_input_pc,
                    cpu.pc, dispatch_input_lr, cpu.lr,
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read32(&cpu, 0x803F7C2Cu),
                    mem_read32(&cpu, 0x803F7C34u),
                    mem_read32(&cpu, 0x803F7C28u),
                    mem_read32(&cpu, 0x803F7C30u),
                    mem_read32(&cpu, 0x80ADBEA8u + 0x1Cu));
            aram_isr_segment_trace_reports++;
        }
        if (g_runqueue_trace &&
            aram_clear_store_trace_reports < 64u &&
            (dispatch_input_pc == 0x80318DFCu ||
             dispatch_input_pc == 0x80318E00u ||
             dispatch_input_pc == 0x80318E24u ||
             dispatch_input_pc == 0x80318E28u) &&
            mem_read32(&cpu, 0x800000E4u) == 0x804211E0u) {
            fprintf(stderr,
                    "[aram] clear-store #%u input=0x%08X output=0x%08X "
                    "input_lr=0x%08X r1=0x%08X thread=0x%08X context=0x%08X "
                    "pending_lo=0x%08X callback_lo=0x%08X pending_hi=0x%08X "
                    "callback_hi=0x%08X request=0x%08X queue_used=%u\n",
                    aram_clear_store_trace_reports + 1u, dispatch_input_pc,
                    cpu.pc, dispatch_input_lr, cpu.gpr[1],
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read32(&cpu, 0x803F7C2Cu),
                    mem_read32(&cpu, 0x803F7C34u),
                    mem_read32(&cpu, 0x803F7C28u),
                    mem_read32(&cpu, 0x803F7C30u),
                    mem_read32(&cpu, 0x803F7C2Cu) != 0u ?
                        mem_read32(&cpu, 0x803F7C2Cu) :
                        mem_read32(&cpu, 0x803F7C28u),
                    mem_read32(&cpu, 0x80ADBEA8u + 0x1Cu));
            aram_clear_store_trace_reports++;
        }
        if (g_runqueue_trace &&
            aram_context_transition_trace_reports < 64u &&
            (dispatch_input_pc == 0x80303968u ||
             dispatch_input_pc == 0x803039C4u) &&
            mem_read32(&cpu, 0x800000E4u) == 0x804211E0u) {
            fprintf(stderr,
                    "[aram] context-transition #%u input=0x%08X output=0x%08X "
                    "input_lr=0x%08X output_lr=0x%08X r3=0x%08X r4=0x%08X "
                    "thread=0x%08X context=0x%08X srr0=0x%08X srr1=0x%08X "
                    "msr=0x%08X\n",
                    aram_context_transition_trace_reports + 1u,
                    dispatch_input_pc, cpu.pc, dispatch_input_lr, cpu.lr,
                    cpu.gpr[3], cpu.gpr[4], mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u), cpu.srr0, cpu.srr1, cpu.msr);
            aram_context_transition_trace_reports++;
        }
        if (g_runqueue_trace &&
            aram_message_internal_trace_reports < 32u &&
            dispatch_input_pc == 0x80304608u &&
            dispatch_input_lr == 0x80305934u &&
            mem_read32(&cpu, 0x800000E4u) == 0x804211E0u) {
            const u32 current_context = mem_read32(&cpu, 0x800000D4u);
            fprintf(stderr,
                    "[aram] message-internal #%u input=0x%08X output=0x%08X "
                    "input_lr=0x%08X lr=0x%08X r1=0x%08X r3=0x%08X "
                    "msr=0x%08X thread=0x%08X context=0x%08X "
                    "context_srr0=0x%08X context_srr1=0x%08X run_bits=0x%08X\n",
                    aram_message_internal_trace_reports + 1u, dispatch_input_pc,
                    cpu.pc, dispatch_input_lr, cpu.lr, cpu.gpr[1], cpu.gpr[3],
                    cpu.msr, mem_read32(&cpu, 0x800000E4u), current_context,
                    current_context != 0u ?
                        mem_read32(&cpu, current_context + 0x198u) : 0u,
                    current_context != 0u ?
                        mem_read32(&cpu, current_context + 0x19Cu) : 0u,
                    mem_read32(&cpu, 0x803F7A30u));
            aram_message_internal_trace_reports++;
        }
        if (g_runqueue_trace &&
            aram_message_dispatch_trace_reports < 32u &&
            dispatch_input_pc == 0x80305908u &&
            dispatch_input_lr == 0x802B6074u &&
            mem_read32(&cpu, 0x800000E4u) == 0x804211E0u) {
            const u32 current_context = mem_read32(&cpu, 0x800000D4u);
            fprintf(stderr,
                    "[aram] message-dispatch #%u input=0x%08X output=0x%08X "
                    "lr=0x%08X r1=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X "
                    "thread=0x%08X context=0x%08X context_srr0=0x%08X "
                    "context_srr1=0x%08X run_bits=0x%08X queue_used=%u\n",
                    aram_message_dispatch_trace_reports + 1u, dispatch_input_pc,
                    cpu.pc, cpu.lr, cpu.gpr[1], cpu.gpr[3], cpu.gpr[4],
                    cpu.gpr[5], mem_read32(&cpu, 0x800000E4u), current_context,
                    current_context != 0u ?
                        mem_read32(&cpu, current_context + 0x198u) : 0u,
                    current_context != 0u ?
                        mem_read32(&cpu, current_context + 0x19Cu) : 0u,
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x80ADBEA8u + 0x1Cu));
            aram_message_dispatch_trace_reports++;
        }
        if (g_runqueue_trace &&
            aram_callback_return_trace_reports < 32u &&
            dispatch_input_pc == 0x802B5FE0u &&
            mem_read32(&cpu, 0x800000E4u) == 0x804211E0u) {
            const u32 current_context = mem_read32(&cpu, 0x800000D4u);
            fprintf(stderr,
                    "[aram] callback-return #%u input=0x%08X output=0x%08X "
                    "lr=0x%08X r1=0x%08X r30=0x%08X r31=0x%08X "
                    "srr0=0x%08X srr1=0x%08X msr=0x%08X "
                    "thread=0x%08X context=0x%08X context_srr0=0x%08X "
                    "context_srr1=0x%08X arq_lo=0x%08X cb_lo=0x%08X "
                    "queue_used=%u\n",
                    aram_callback_return_trace_reports + 1u, dispatch_input_pc,
                    cpu.pc, cpu.lr, cpu.gpr[1], cpu.gpr[30], cpu.gpr[31],
                    cpu.srr0, cpu.srr1, cpu.msr,
                    mem_read32(&cpu, 0x800000E4u), current_context,
                    current_context != 0u ?
                        mem_read32(&cpu, current_context + 0x198u) : 0u,
                    current_context != 0u ?
                        mem_read32(&cpu, current_context + 0x19Cu) : 0u,
                    mem_read32(&cpu, 0x803F7C2Cu),
                    mem_read32(&cpu, 0x803F7C34u),
                    mem_read32(&cpu, 0x80ADBEA8u + 0x1Cu));
            aram_callback_return_trace_reports++;
        }
        if (trace_dispatch_return && dispatch_transition_reports < 12u) {
            fprintf(stderr,
                    "[interrupt] dispatch-return-after pc=0x%08X "
                    "srr0=0x%08X srr1=0x%08X msr=0x%08X\n",
                    cpu.pc, cpu.srr0, cpu.srr1, cpu.msr);
            dispatch_transition_reports++;
        }
        if (trace_worker_dispatch) {
            fprintf(stderr,
                    "[sched] worker-dispatch-after #%u pc=0x%08X "
                    "msr=0x%08X srr0=0x%08X srr1=0x%08X pending=%d "
                    "cause=0x%08X run_bits=0x%08X thread=0x%08X "
                    "context=0x%08X\n",
                    worker_dispatch_trace_reports + 1u, cpu.pc, cpu.msr,
                    cpu.srr0, cpu.srr1,
                    dol_interrupts_external_pending(&g_interrupts),
                    dol_interrupts_pi_cause(&g_interrupts),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            worker_dispatch_trace_reports++;
        }
        if (dispatch_input_pc != 0x80304DF8u && cpu.pc == 0x80304DF8u &&
            dispatch_transition_reports < 12u) {
            fprintf(stderr,
                    "[interrupt] dispatch-reentered input=0x%08X "
                    "output=0x%08X srr0=0x%08X srr1=0x%08X msr=0x%08X\n",
                    dispatch_input_pc, cpu.pc, cpu.srr0, cpu.srr1, cpu.msr);
            dispatch_transition_reports++;
        }
        if (g_runqueue_trace) {
            const u32 dvd_wait_queue = mem_read32(&cpu, 0x803A5F90u + 0x2DCu);
            if (!dvd_wait_queue_reported || dvd_wait_queue != last_dvd_wait_queue) {
                fprintf(stderr,
                        "[sched] dvd-queue-change pc=0x%08X old=0x%08X "
                        "new=0x%08X state=%u lparam_recv=0x%08X "
                        "lparam_used=%u current_thread=0x%08X saved_pc=0x%08X "
                        "saved_lr=0x%08X saved_sp=0x%08X\n",
                        cpu.pc, last_dvd_wait_queue, dvd_wait_queue,
                        mem_read16(&cpu, 0x803A5F90u + 0x2C8u),
                        mem_read32(&cpu, 0x803A72C0u + 0x8u),
                        mem_read32(&cpu, 0x803A72C0u + 0x1Cu),
                        mem_read32(&cpu, 0x800000E4u),
                        mem_read32(&cpu, 0x803A5F90u + 0x198u),
                        mem_read32(&cpu, 0x803A5F90u + 0x84u),
                        mem_read32(&cpu, 0x803A5F90u + 0x04u));
                if (dvd_wait_queue >= 0x80000000u) {
                    const u32 dvd_file = dvd_wait_queue - 0xC8u;
                    const u32 file_info = dvd_file + 0x5Cu;
                    fprintf(stderr,
                            "[sched] blocked-dvd-file object=0x%08X "
                            "file_info=0x%08X start=0x%08X length=%u "
                            "user=0x%08X queue_array=0x%08X\n",
                            dvd_file, file_info,
                            mem_read32(&cpu, file_info + DVD_FI_STARTADDR),
                            mem_read32(&cpu, file_info + DVD_FI_LENGTH),
                            mem_read32(&cpu, file_info + 0x3Cu),
                            mem_read32(&cpu, dvd_wait_queue + 0x10u));
                }
                last_dvd_wait_queue = dvd_wait_queue;
                dvd_wait_queue_reported = true;
            }
        }
        if (g_dispatch_terminal_trace &&
            terminal_dispatch_trace_reports < 32u &&
            (cpu.pc == 0x00000000u || cpu.pc == 0x80000000u)) {
            fprintf(stderr,
                    "[dispatch-terminal] output=0x%08X input=0x%08X dispatched=%d "
                    "lr=0x%08X r1=0x%08X r3=0x%08X r30=0x%08X r31=0x%08X "
                    "r30_word0=0x%08X r30_word4=0x%08X "
                    "r31_word0=0x%08X r31_word4=0x%08X "
                    "context=0x%08X run_bits=0x%08X\n",
                    cpu.pc, dispatch_input_pc, dispatched, cpu.lr, cpu.gpr[1],
                    cpu.gpr[3], cpu.gpr[30], cpu.gpr[31],
                    mem_read32(&cpu, cpu.gpr[30]),
                    mem_read32(&cpu, cpu.gpr[30] + 4u),
                    mem_read32(&cpu, cpu.gpr[31]),
                    mem_read32(&cpu, cpu.gpr[31] + 4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read32(&cpu, 0x803F7A30u));
            terminal_dispatch_trace_reports++;
        }
        if (!dispatched) {
            if (g_rel_indirect_trace &&
                g_rel_indirect_trace_reports < 32u &&
                cpu.pc == 0x80000000u) {
                fprintf(stderr,
                        "[rel-indirect] unmapped input=0x%08X output=0x%08X "
                        "lr=0x%08X r1=0x%08X r3=0x%08X r5=0x%08X r31=0x%08X\n",
                        dispatch_input_pc, cpu.pc, cpu.lr, cpu.gpr[1],
                        cpu.gpr[3], cpu.gpr[5], cpu.gpr[31]);
                g_rel_indirect_trace_reports++;
            }
            if (cpu.pc == 0x80000500u) {
                // The composite starts at 0x80003100 and therefore omits the
                // retail low-memory vector branch. Continue at the exact
                // translated ExternalInterruptHandler body instead of
                // fabricating a wakeup in the host.
                fprintf(stderr,
                        "[vector] alias external vector 0x80000500 -> 0x80304E24\n");
                const u32 context = mem_read32(&cpu, 0x800000D4u);
                // Mirror the first-level OSExceptionVector state handoff:
                // r3 is the exception number, r4 is the current context, and
                // the interrupted SRR pair is captured before dispatch.
                mem_write32(&cpu, context + 0x0Cu, cpu.gpr[3]);
                mem_write32(&cpu, context + 0x10u, cpu.gpr[4]);
                mem_write32(&cpu, context + 0x14u, cpu.gpr[5]);
                mem_write32(&cpu, context + 0x80u, cpu.cr);
                mem_write32(&cpu, context + 0x84u, cpu.lr);
                mem_write32(&cpu, context + 0x88u, cpu.ctr);
                mem_write32(&cpu, context + 0x8Cu, cpu.xer);
                cpu.gpr[3] = 4u;
                cpu.gpr[4] = context;
                mem_write32(&cpu, context + 0x198u, cpu.srr0);
                mem_write32(&cpu, context + 0x19Cu, cpu.srr1);
                mem_write16(&cpu, context + 0x1A2u,
                            mem_read16(&cpu, context + 0x1A2u) | 0x0002u);
                cpu.pc = 0x80304E24u;
                continue;
            }
            stop_reason = "unmapped pc";
            if (g_pad_trace && cpu.lr == 0x8003BDE4u) {
                fprintf(stderr,
                        "[fopmsg] unmapped-after-move pc=%#010x "
                        "lr=%#010x r1=%#010x r3=%#010x r4=%#010x "
                        "r5=%#010x r6=%#010x r12=%#010x ctr=%#010x "
                        "r3_word0=%#010x r3_word4=%#010x blocks=%llu\n",
                        cpu.pc, cpu.lr, cpu.gpr[1], cpu.gpr[3], cpu.gpr[4],
                        cpu.gpr[5], cpu.gpr[6], cpu.gpr[12], cpu.ctr,
                        mem_read32(&cpu, cpu.gpr[3]),
                        mem_read32(&cpu, cpu.gpr[3] + 4u), blocks);
            }
            fprintf(stderr, "[run] unmapped pc=%#010x lr=%#010x r1=%#010x blocks=%llu\n",
                    cpu.pc, cpu.lr, cpu.gpr[1], blocks);
            break;
        }
        if (g_runqueue_trace && message_send_reported &&
            !message_send_return_reported &&
            cpu.pc == 0x803059A4u) {
            const u32 video_queue = 0x80429EE8u;
            fprintf(stderr,
                    "[vi] OSSendMessage return used=%u first=%u "
                    "main_state=%u main_queue=0x%08X run_bits=0x%08X "
                    "current_thread=0x%08X current_context=0x%08X\n",
                    mem_read32(&cpu, video_queue + 0x1Cu),
                    mem_read32(&cpu, video_queue + 0x18u),
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                    mem_read32(&cpu, 0x803A2960u + 0x2DCu),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            message_send_return_reported = true;
        }
        if (g_runqueue_trace && message_send_reported &&
            !reschedule_boundary_reported &&
            (cpu.pc == 0x80307DA8u || cpu.pc == 0x80307FF0u)) {
            fprintf(stderr,
                    "[sched] reschedule boundary pc=0x%08X run_bits=0x%08X "
                    "current_thread=0x%08X current_context=0x%08X main_state=%u\n",
                    cpu.pc, mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u),
                    mem_read16(&cpu, 0x803A2960u + 0x2C8u));
            reschedule_boundary_reported = true;
        }
        if (cpu.pc == 0x80303A50u)
            host_restore_context_shadow(&cpu, cpu.gpr[3]);
        if ((g_runqueue_trace ||
             (g_audio_object_watch && cpu.gpr[3] == 0x803E9260u)) &&
            cpu.pc == 0x80303A50u &&
            (os_load_reports < 40u ||
             (cpu.gpr[3] == 0x803E9260u && audio_os_load_reports < 16u))) {
            fprintf(stderr,
                    "[sched] OSLoadContext entry #%u context=0x%08X srr0=0x%08X "
                    "srr1=0x%08X cpu_msr=0x%08X cpu_srr0=0x%08X "
                    "cpu_srr1=0x%08X run_bits=0x%08X current_thread=0x%08X "
                    "current_context=0x%08X\n",
                    os_load_reports + 1u, cpu.gpr[3],
                    mem_read32(&cpu, cpu.gpr[3] + 0x198u),
                    mem_read32(&cpu, cpu.gpr[3] + 0x19Cu),
                    cpu.msr, cpu.srr0, cpu.srr1,
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            if (os_load_reports < 40u)
                os_load_reports++;
            if (cpu.gpr[3] == 0x803E9260u)
                audio_os_load_reports++;
        }
        if ((g_runqueue_trace ||
             (g_audio_object_watch &&
              mem_read32(&cpu, 0x800000D4u) == 0x803E9260u)) &&
            cpu.pc == 0x80303B24u &&
            (os_rfi_reports < 40u ||
             (mem_read32(&cpu, 0x800000D4u) == 0x803E9260u &&
              audio_os_rfi_reports < 16u))) {
            fprintf(stderr,
                    "[sched] OSLoadContext rfi #%u context=0x%08X next=0x%08X "
                    "srr1=0x%08X cpu_msr_before=0x%08X gpr1=0x%08X "
                    "run_bits=0x%08X current_thread=0x%08X current_context=0x%08X\n",
                    os_rfi_reports + 1u, mem_read32(&cpu, 0x800000D4u),
                    cpu.srr0, cpu.srr1, cpu.msr, cpu.gpr[1],
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            if (os_rfi_reports < 40u)
                os_rfi_reports++;
            if (mem_read32(&cpu, 0x800000D4u) == 0x803E9260u)
                audio_os_rfi_reports++;
        }
        if (g_runqueue_trace && video_callback_reported) {
            const u32 video_queue = 0x80429EE8u;
            const u32 video_used = mem_read32(&cpu, video_queue + 0x1Cu);
            const u32 run_bits = mem_read32(&cpu, 0x803F7A30u);
            if (video_used != last_video_used || run_bits != last_run_bits) {
                if (scheduler_state_change_reports < 64u) {
                    fprintf(stderr,
                            "[sched] state change pc=0x%08X video_used=%u "
                            "first=%u run_bits=0x%08X main_state=%u "
                            "main_queue=0x%08X current_thread=0x%08X "
                            "current_context=0x%08X\n",
                            cpu.pc, video_used,
                            mem_read32(&cpu, video_queue + 0x18u), run_bits,
                            mem_read16(&cpu, 0x803A2960u + 0x2C8u),
                            mem_read32(&cpu, 0x803A2960u + 0x2DCu),
                            mem_read32(&cpu, 0x800000E4u),
                            mem_read32(&cpu, 0x800000D4u));
                    scheduler_state_change_reports++;
                }
                last_video_used = video_used;
                last_run_bits = run_bits;
                const u32 main_queue = mem_read32(&cpu, 0x803A2960u + 0x2DCu);
                if (retrace_wait_reports < 4u && main_queue == 0x803F7B44u) {
                    u32 frame = mem_read32(&cpu, 0x803A2960u + 0x04u);
                    fprintf(stderr,
                            "[sched] retrace-wait #%u main_sp=0x%08X "
                            "main_srr0=0x%08X main_lr=0x%08X\n",
                            retrace_wait_reports + 1u, frame,
                            mem_read32(&cpu, 0x803A2960u + 0x198u),
                            mem_read32(&cpu, 0x803A2960u + 0x84u));
                    for (unsigned depth = 0; depth < 5u &&
                                         frame >= 0x80000000u; depth++) {
                        fprintf(stderr,
                                "[sched] retrace-frame%u sp=0x%08X lr=0x%08X\n",
                                depth, frame, mem_read32(&cpu, frame + 4u));
                        const u32 next = mem_read32(&cpu, frame);
                        if (next == frame || next < frame)
                            break;
                        frame = next;
                    }
                    retrace_wait_reports++;
                }
            }
        }
        if (force_fp)
            cpu.msr |= PPC_MSR_FP;
        // Consume only generated Gekko cycle charges. Host/HLE turns can
        // legitimately complete without executing translated instructions;
        // assigning them synthetic time would make clock/device state depend
        // on whether dispatch happened inline or through the chassis.
        (void)bluewake_cycle_domain_end_turn(&g_cycle_domain, &cpu);
        // The credit census counts *turns*, which is the per-turn body's own
        // boundary: the entry above it counted flush calls, and flush is reached
        // from end_turn, flush and four observe sites, so its denominator was not
        // the thing the twin's turn inflation is about.
        // Gated to the play window: every earlier census in this ledger counted the
        // boot, whose turn structure is not the graded window's (2026-09-22).
        if (g_cycle_credit_census &&
            g_host_retrace_count >= g_credit_census_window) {
            const u64 credited = (u64)g_cycle_domain.dispatch_cycles;
            /* The host can see whether a turn used its budget without touching the
             * composite: a turn whose credit reached the budget it was handed ended
             * at the loop's budget exit, and every other turn ended somewhere the
             * host cannot see. The windows differ, so this prints every 1,000
             * retraces and the last two lines give the play window's rate. */
            static u64 bw_prev_budget;
            static u64 bw_budget_exits;
            static u64 bw_early_exits;
            if (credited + 1u >= bw_prev_budget)
                bw_budget_exits++;
            else
                bw_early_exits++;
            bw_prev_budget = cpu.cycle_budget > 0 ? (u64)cpu.cycle_budget : 0u;
            if (g_host_retrace_count % 1000u == 0u)
                fprintf(stderr,
                        "[turn-split] retrace=%llu budget_exits=%llu early_exits=%llu\n",
                        (unsigned long long)g_host_retrace_count,
                        (unsigned long long)bw_budget_exits,
                        (unsigned long long)bw_early_exits);
            g_cycle_credit_calls++;
            if (credited == 0u)
                g_cycle_credit_zero++;
            g_cycle_credit_sum += credited;
            if (credited > g_cycle_credit_max)
                g_cycle_credit_max = credited;
            const u64 budget =
                cpu.cycle_budget > 0 ? (u64)cpu.cycle_budget : 0u;
            const u64 lag =
                g_cycle_domain.absolute_cycles > g_dsp_cycle_cursor
                    ? g_cycle_domain.absolute_cycles - g_dsp_cycle_cursor
                    : 0u;
            g_credit_budget_sum += budget;
            if (g_credit_budget_min == 0u || budget < g_credit_budget_min)
                g_credit_budget_min = budget;
            if (budget > g_credit_budget_max)
                g_credit_budget_max = budget;
            g_credit_lag_sum += lag;
            if (lag > g_credit_lag_max)
                g_credit_lag_max = lag;
        }
        host_sync_cycle_devices_end_turn(&cpu);
        if (g_delivery_safety_census_enabled) {
            if (g_guest_decrementer_pending) {
                g_decrementer_pending_turns++;
                if ((cpu.msr & PPC_MSR_EE) != 0u && cpu.exception == 0u) {
                    g_decrementer_blocked_streak = 0;
                } else {
                    g_decrementer_blocked_turns++;
                    g_decrementer_blocked_streak++;
                    if (g_decrementer_blocked_streak >
                        g_decrementer_blocked_max_streak)
                        g_decrementer_blocked_max_streak =
                            g_decrementer_blocked_streak;
                }
            } else {
                g_decrementer_blocked_streak = 0;
            }
        }
        if (g_guest_decrementer_pending && (cpu.msr & PPC_MSR_EE) &&
            cpu.exception == 0u) {
            host_delivery_safety_observe(&cpu, "decrementer");
            deliver_decrementer_exception(&cpu);
        }
        if (!interrupt_state_reported && blocks >= 1000100ull) {
            const u32 video_queue = 0x80429EE8u;
            fprintf(stderr,
                    "[vi] post-interrupt state retraces=%u vi_di0=0x%04X "
                    "retrace_head=0x%08X retrace_tail=0x%08X "
                    "video_used=%u video_first=%u pre_cb=0x%08X post_cb=0x%08X "
                    "video_manager=0x%08X\n",
                    mem_read32(&cpu, 0x803F7B3Cu),
                    mem_read16(&cpu, 0xCC002030u),
                    mem_read32(&cpu, 0x803F7B44u),
                    mem_read32(&cpu, 0x803F7B48u),
                    mem_read32(&cpu, video_queue + 0x1Cu),
                    mem_read32(&cpu, video_queue + 0x18u),
                    mem_read32(&cpu, 0x803F7B4Cu),
                    mem_read32(&cpu, 0x803F7B50u),
                    mem_read32(&cpu, 0x803F78D8u));
            interrupt_state_reported = true;
        }
        if (!audio_wait_reported && cpu.pc == 0x80302ED4u) {
            fprintf(stderr,
                    "[wait] __OSInitAudioSystem pc=0x%08X r31=0x%08X "
                    "status=0x%04X downcount=%lld timebase=%llu\n",
                    cpu.pc, cpu.gpr[31], mem_read16(&cpu, cpu.gpr[31]),
                    (long long)cpu.downcount,
                    (unsigned long long)cpu.timebase);
            audio_wait_reported = true;
        }
        if (!audio_tick_wait_reported && cpu.pc == 0x80302F58u) {
            fprintf(stderr,
                    "[wait] __OSInitAudioSystem tick pc=0x%08X status=0x%04X "
                    "downcount=%lld timebase=%llu\n",
                    cpu.pc, mem_read16(&cpu, 0xCC00500Au),
                    (long long)cpu.downcount,
                    (unsigned long long)cpu.timebase);
            audio_tick_wait_reported = true;
        }
        if (!scheduler_wait_reported && cpu.pc == 0x80307EF4u) {
            fprintf(stderr,
                    "[sched] SelectThread idle run_bits=0x%08X current_thread=0x%08X "
                    "current_context=0x%08X r1=0x%08X\n",
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u), cpu.gpr[1]);
            u32 frame = cpu.gpr[1];
            for (unsigned depth = 0; depth < 8u && frame >= 0x80000000u; depth++) {
                fprintf(stderr, "[sched] frame%u sp=0x%08X lr=0x%08X\n",
                        depth, frame, mem_read32(&cpu, frame + 4u));
                const u32 next = mem_read32(&cpu, frame);
                if (next == frame || next < frame)
                    break;
                frame = next;
            }
            const u32 default_thread = 0x803F06D0u;
            const u32 wait_queue = mem_read32(&cpu, default_thread + 0x2DCu);
            fprintf(stderr,
                    "[sched] default_thread state=%u suspend=%d wait_queue=0x%08X "
                    "head=0x%08X tail=0x%08X\n",
                    mem_read16(&cpu, default_thread + 0x2C8u),
                    (s32)mem_read32(&cpu, default_thread + 0x2CCu), wait_queue,
                    wait_queue ? mem_read32(&cpu, wait_queue) : 0u,
                    wait_queue ? mem_read32(&cpu, wait_queue + 4u) : 0u);
            const u32 audio_thread = 0x803E9260u;
            const u32 audio_queue = mem_read32(&cpu, audio_thread + 0x2DCu);
            fprintf(stderr,
                    "[sched] audio-thread-idle state=%u suspend=%d queue=0x%08X "
                    "saved_pc=0x%08X saved_lr=0x%08X context_state=0x%04X "
                    "mq_used=%u mq_first=%u run_bits=0x%08X current_thread=0x%08X "
                    "current_context=0x%08X\n",
                    mem_read16(&cpu, audio_thread + 0x2C8u),
                    (s32)mem_read32(&cpu, audio_thread + 0x2CCu),
                    audio_queue, mem_read32(&cpu, audio_thread + 0x198u),
                    mem_read32(&cpu, audio_thread + 0x84u),
                    mem_read16(&cpu, audio_thread + 0x1A2u),
                    mem_read32(&cpu, 0x803EA580u + 0x1Cu),
                    mem_read32(&cpu, 0x803EA580u + 0x18u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            u32 active = mem_read32(&cpu, 0x800000DCu);
            for (unsigned index = 0; index < 8u && active >= 0x80000000u; index++) {
                const u32 queue = mem_read32(&cpu, active + 0x2DCu);
                const s32 effective_priority = (s32)mem_read32(&cpu, active + 0x2D0u);
                const u32 expected_run_bit = effective_priority >= 0 &&
                                                     effective_priority <= 31
                                                 ? 1u << (31u - (u32)effective_priority)
                                                 : 0u;
                fprintf(stderr,
                        "[sched] active%u thread=0x%08X state=%u suspend=%d queue=0x%08X "
                        "effective=%d base=%d expected_run_bit=0x%08X run_bits=0x%08X "
                        "head=0x%08X tail=0x%08X srr0=0x%08X lr=0x%08X sp=0x%08X next=0x%08X\n",
                        index, active, mem_read16(&cpu, active + 0x2C8u),
                        (s32)mem_read32(&cpu, active + 0x2CCu),
                        queue, effective_priority,
                        (s32)mem_read32(&cpu, active + 0x2D4u), expected_run_bit,
                        mem_read32(&cpu, 0x803F7A30u),
                        queue ? mem_read32(&cpu, queue) : 0u,
                        queue ? mem_read32(&cpu, queue + 4u) : 0u,
                        mem_read32(&cpu, active + 0x198u),
                        mem_read32(&cpu, active + 0x84u),
                        mem_read32(&cpu, active + 0x04u),
                        mem_read32(&cpu, active + 0x2FCu));
                if (active == 0x803A2960u && queue >= 0x80000000u) {
                    const u32 message_queue = queue - 8u;
                    fprintf(stderr,
                            "[sched] mainThread mq=0x%08X messages=0x%08X "
                            "capacity=%u first=%u used=%u\n",
                            message_queue, mem_read32(&cpu, message_queue + 0x10u),
                            mem_read32(&cpu, message_queue + 0x14u),
                            mem_read32(&cpu, message_queue + 0x18u),
                            mem_read32(&cpu, message_queue + 0x1Cu));
                    u32 saved_frame = mem_read32(&cpu, active + 0x04u);
                    for (unsigned depth = 0; depth < 8u &&
                                            saved_frame >= 0x80000000u; depth++) {
                        fprintf(stderr,
                                "[sched] mainThread frame%u sp=0x%08X lr=0x%08X\n",
                                depth, saved_frame,
                                mem_read32(&cpu, saved_frame + 4u));
                        const u32 next_frame = mem_read32(&cpu, saved_frame);
                        if (next_frame == saved_frame || next_frame < saved_frame)
                            break;
                        saved_frame = next_frame;
                    }
                }
                const u32 next_active = mem_read32(&cpu, active + 0x2FCu);
                if (next_active == active || next_active < 0x80000000u)
                    break;
                active = next_active;
            }
            scheduler_wait_reported = true;
        }
        if (g_alarm_state_trace &&
            !alarm_state_snapshot_reported && cpu.pc == 0x80307EF4u) {
            const u32 queue = 0x803F79C0u;
            const u32 head = mem_read32(&cpu, queue + 0x00u);
            const u32 tail = mem_read32(&cpu, queue + 0x04u);
            const u64 fire = head != 0u
                                 ? ((u64)mem_read32(&cpu, head + 0x08u) << 32) |
                                       mem_read32(&cpu, head + 0x0Cu)
                                 : 0u;
            fprintf(stderr,
                    "[alarm] state-snapshot queue=0x%08X head=0x%08X "
                    "tail=0x%08X handler=0x%08X fire=%llu timebase=%llu "
                    "period=%llu msr=0x%08X current=0x%08X pc=0x%08X\n",
                    queue, head, tail,
                    head != 0u ? mem_read32(&cpu, head + 0x00u) : 0u,
                    (unsigned long long)fire,
                    head != 0u
                        ? (unsigned long long)(((u64)mem_read32(&cpu, head + 0x18u) << 32) |
                                               mem_read32(&cpu, head + 0x1Cu))
                        : 0u,
                    (unsigned long long)cpu.timebase, cpu.msr,
                    mem_read32(&cpu, 0x800000E4u), cpu.pc);
            alarm_state_snapshot_reported = true;
        }
        if (idle_selector_reports < 8u && cpu.pc == 0x80307EF4u) {
            const u32 main_thread = 0x803A2960u;
            fprintf(stderr,
                    "[sched] idle-main #%u state=%u attr=0x%04X suspend=%d "
                    "effective=%d base=%d queue=0x%08X next=0x%08X prev=0x%08X "
                    "saved_pc=0x%08X lr=0x%08X sp=0x%08X run_bits=0x%08X "
                    "current_thread=0x%08X current_context=0x%08X\n",
                    idle_selector_reports + 1u,
                    mem_read16(&cpu, main_thread + 0x2C8u),
                    mem_read16(&cpu, main_thread + 0x2CAu),
                    (s32)mem_read32(&cpu, main_thread + 0x2CCu),
                    (s32)mem_read32(&cpu, main_thread + 0x2D0u),
                    (s32)mem_read32(&cpu, main_thread + 0x2D4u),
                    mem_read32(&cpu, main_thread + 0x2DCu),
                    mem_read32(&cpu, main_thread + 0x2E0u),
                    mem_read32(&cpu, main_thread + 0x2E4u),
                    mem_read32(&cpu, main_thread + 0x198u),
                    mem_read32(&cpu, main_thread + 0x84u),
                    mem_read32(&cpu, main_thread + 0x04u),
                    mem_read32(&cpu, 0x803F7A30u),
                    mem_read32(&cpu, 0x800000E4u),
                    mem_read32(&cpu, 0x800000D4u));
            idle_selector_reports++;
        }
        if (cpu.exception) {
            if (cpu.exception == PPC_EXC_SYSTEM_CALL) {
                cpu.exception = 0;
                ppc_rfi(&cpu, cpu.pc);
            } else if (cpu.exception == PPC_EXC_EXTERNAL) {
                cpu.exception = 0;
            } else if (cpu.exception == PPC_EXC_FP_UNAVAILABLE) {
                const BluewakeFpuSwitchResult switch_result =
                    bluewake_fpu_exception_deliver(&cpu);
                g_fpu_switch_counts[switch_result]++;
                if (!fp_resume_reported) {
                    fprintf(stderr,
                            "[fp] lazy owner switch result=%u "
                            "srr0=0x%08X context=0x%08X owner=0x%08X\n",
                            (unsigned)switch_result, cpu.srr0,
                            mem_read32(&cpu, 0x800000D4u),
                            mem_read32(&cpu, 0x800000D8u));
                    fp_resume_reported = true;
                }
                if (switch_result == BLUEWAKE_FPU_SWITCH_INVALID) {
                    stop_reason = "invalid lazy FPU context";
                    continue;
                }
            } else {
                stop_reason = "exception";
                fprintf(stderr,
                        "[run] exception=%#010x at pc=%#010x srr0=%#010x "
                        "msr=%#010x block=%llu\n",
                        cpu.exception, cpu.pc, cpu.srr0, cpu.msr, blocks);
            }
            continue;
        }
        blocks++;
        if ((blocks % 1000000ull) == 0)
            fprintf(stderr, "[trace] %lluM blocks pc=%#010x\n", blocks / 1000000ull, cpu.pc);
    }

    printf("[run] stopped: %s after %llu blocks at pc=%#010x\n",
           stop_reason ? stop_reason : "normal", blocks, cpu.pc);
    fprintf(stderr,
            "[collision-provenance] summary ground_cross_returns=%llu "
            "deferred_fp=%llu valid_seen=%u sentinel_seen=%u\n",
            (unsigned long long)g_ground_cross_returns,
            (unsigned long long)g_ground_cross_deferred_returns,
            g_ground_cross_valid_seen ? 1u : 0u,
            g_ground_cross_sentinel_seen ? 1u : 0u);
    if (return_census_enabled)
        bluewake_return_census_print(&return_census, stderr);
    if (g_delivery_safety_census_enabled) {
        fprintf(stderr, "[delivery-safety] summary deliveries=%llu unsafe=%llu\n",
                (unsigned long long)g_delivery_safety_calls,
                (unsigned long long)g_delivery_safety_unsafe);
        fprintf(stderr,
                "[delivery-safety] clock advances=%llu cycles=%llu "
                "expiries=%llu pending_turns=%llu blocked_turns=%llu "
                "max_blocked_streak=%llu\n",
                (unsigned long long)g_clock_advance_calls,
                (unsigned long long)g_clock_advance_cycles,
                (unsigned long long)g_clock_decrementer_expiries,
                (unsigned long long)g_decrementer_pending_turns,
                (unsigned long long)g_decrementer_blocked_turns,
                (unsigned long long)g_decrementer_blocked_max_streak);
        fprintf(stderr,
                "[delivery-safety] sources publishes=%llu true=%llu "
                "rise_di=%llu rise_dsp=%llu rise_si=%llu\n",
                (unsigned long long)g_source_publishes,
                (unsigned long long)g_source_true_publishes,
                (unsigned long long)g_source_rise_di,
                (unsigned long long)g_source_rise_dsp,
                (unsigned long long)g_source_rise_si);
        fprintf(stderr,
                "[delivery-safety] dsp advance_calls=%llu iters=%llu "
                "run_calls=%llu cpu_cycles=%llu mail_sync=%llu "
                "mail_slices=%llu\n",
                (unsigned long long)g_dsp_advance_calls,
                (unsigned long long)g_dsp_advance_iters,
                (unsigned long long)g_dsp_run_calls,
                (unsigned long long)g_dsp_run_cpu_cycles,
                (unsigned long long)g_dsp_mail_sync_calls,
                (unsigned long long)g_dsp_mail_sync_slices);
    }
    if (g_boundary_by_address != NULL) {
        FILE* out = fopen(getenv("BLUEWAKE_BOUNDARY_CENSUS_BY_ADDRESS"), "w");
        if (out != NULL) {
            for (u32 i = 0; i < BOUNDARY_ADDRESS_WORDS; ++i)
                if (g_boundary_by_address[i] != 0u)
                    fprintf(out, "%08X %u\n", 0x80000000u | (i << 2), g_boundary_by_address[i]);
            fclose(out);
        }
    }
    if (g_actor_search_native_runs != 0u)
        fprintf(stderr, "[actor-search-native] runs=%llu nodes=%llu\n",
                (unsigned long long)g_actor_search_native_runs,
                (unsigned long long)g_actor_search_native_nodes);
    if (g_boundary_census_enabled) {
        fprintf(stderr,
                "[boundary-census] calls=%llu window_retrace=%llu "
                "window_calls=%llu play_retrace=%llu play_calls=%llu "
                "retraces=%llu turns=%llu\n",
                (unsigned long long)g_boundary_calls,
                (unsigned long long)g_boundary_census_window,
                (unsigned long long)g_boundary_window_calls,
                (unsigned long long)g_play_scene_retrace,
                (unsigned long long)g_boundary_play_calls,
                (unsigned long long)g_host_retrace_count,
                (unsigned long long)blocks);
    }
#if BLUEWAKE_EDGE_CENSUS
    fprintf(stderr,
            "[edge-census] calls=%llu overlap_guard=%llu overlap_object=%llu "
            "overlap_fast=%llu "
            "overlap_enabled=%llu overlap_phase_changed=%llu intro=%llu "
            "predicate_true=%llu predicate_false=%llu service_each_block=%llu "
            "interrupt_ee=%llu scheduler=%llu publishes=%llu published=%llu "
            "retraces=%llu\n",
            (unsigned long long)g_edge_calls,
            (unsigned long long)g_edge_overlap_guard,
            (unsigned long long)g_edge_overlap_object,
            (unsigned long long)g_edge_overlap_fast,
            (unsigned long long)g_edge_overlap_enabled,
            (unsigned long long)g_edge_overlap_phase_changed,
            (unsigned long long)g_edge_intro_hits,
            (unsigned long long)g_edge_predicate_true,
            (unsigned long long)g_edge_predicate_false,
            (unsigned long long)g_edge_service_each_block,
            (unsigned long long)g_edge_interrupt_ee,
            (unsigned long long)g_edge_scheduler_true,
            (unsigned long long)g_edge_source_publishes,
            (unsigned long long)g_edge_source_changes,
            (unsigned long long)g_host_retrace_count);
    fprintf(stderr, "[mmio-census] read");
    for (unsigned i = 0; i < BLUEWAKE_MMIO_BUCKETS; i++)
        if (g_mmio_read_buckets[i] != 0u)
            fprintf(stderr, " b%u=%llu", i,
                    (unsigned long long)g_mmio_read_buckets[i]);
    fprintf(stderr, "\n[mmio-census] write");
    for (unsigned i = 0; i < BLUEWAKE_MMIO_BUCKETS; i++)
        if (g_mmio_write_buckets[i] != 0u)
            fprintf(stderr, " b%u=%llu", i,
                    (unsigned long long)g_mmio_write_buckets[i]);
    fprintf(stderr, "\n");
#endif
    if (g_deadline_census_enabled) {
        static const char* deadline_names[BLUEWAKE_DEADLINE_SOURCE_COUNT] = {
            "vi", "dsp", "audio", "decrementer", "clamp", "none", "cap",
        };
        fprintf(stderr, "[deadline-census] calls=%llu",
                (unsigned long long)g_deadline_calls);
        for (int i = 0; i < BLUEWAKE_DEADLINE_SOURCE_COUNT; i++) {
            fprintf(stderr, " %s=%llu(one_cycle=%llu)", deadline_names[i],
                    (unsigned long long)g_deadline_source_counts[i],
                    (unsigned long long)g_deadline_one_cycle_counts[i]);
        }
        fputc('\n', stderr);
        fprintf(stderr, "[deadline-census] window_retrace=%llu calls=%llu",
                (unsigned long long)g_deadline_census_window,
                (unsigned long long)g_deadline_window_calls);
        for (int i = 0; i < BLUEWAKE_DEADLINE_SOURCE_COUNT; i++) {
            fprintf(stderr, " %s=%llu(one_cycle=%llu)", deadline_names[i],
                    (unsigned long long)g_deadline_window_counts[i],
                    (unsigned long long)g_deadline_window_one_cycle_counts[i]);
        }
        fputc('\n', stderr);
    }
    fprintf(stderr,
            "[fp] summary invalid=%llu same_owner=%llu fresh=%llu "
            "restored=%llu\n",
            (unsigned long long)g_fpu_switch_counts[BLUEWAKE_FPU_SWITCH_INVALID],
            (unsigned long long)g_fpu_switch_counts[BLUEWAKE_FPU_SWITCH_SAME_OWNER],
            (unsigned long long)g_fpu_switch_counts[BLUEWAKE_FPU_SWITCH_FRESH],
            (unsigned long long)g_fpu_switch_counts[BLUEWAKE_FPU_SWITCH_RESTORED]);
    fprintf(stderr,
            "[cpu-abi] summary lava_constant_checks=%llu deferred_fp=%llu "
            "r2=0x%08X constant_word=%08X\n",
            (unsigned long long)lava_constant_checks,
            (unsigned long long)lava_constant_deferred_checks, cpu.gpr[2],
            mem_read32(&cpu, cpu.gpr[2] - 23564u));
    fprintf(stderr,
            "[clock] summary cycles=%llu timebase=%llu retraces=%llu "
            "vi_pending=%u ai_remainder=%llu title_ready=%u "
            "title_retrace=%llu\n",
            (unsigned long long)dol_vi_clock_now(&vi_clock),
            (unsigned long long)cpu.timebase,
            (unsigned long long)g_host_retrace_count,
            (dol_interrupts_pi_cause(&g_interrupts) & DOL_PI_CAUSE_VI) != 0u
                ? 1u
                : 0u,
            (unsigned long long)g_audio_dma.work_counter,
            g_title_ready_reported ? 1u : 0u,
            (unsigned long long)g_title_ready_retrace);
    fprintf(stderr,
            "[cycle-delivery] summary external=%llu hash=%016llX "
            "history=%u history_overflow=%llu "
            "dsp=%llu first_dsp_valid=%u first_dsp_cycle=%llu "
            "first_dsp_cause=0x%08X "
            "first_dsp_pc=0x%08X first_dsp_context=0x%08X\n",
            (unsigned long long)g_delivery_digest.external_count,
            (unsigned long long)g_delivery_digest.hash,
            g_delivery_digest.external_history_count,
            (unsigned long long)g_delivery_digest.external_history_overflow,
            (unsigned long long)g_delivery_digest.dsp_count,
            g_delivery_digest.first_dsp_valid ? 1u : 0u,
            (unsigned long long)g_delivery_digest.first_dsp_cycle,
            g_delivery_digest.first_dsp_cause,
            g_delivery_digest.first_dsp_pc,
            g_delivery_digest.first_dsp_context);
    // Cap-invariance report. The tag is deliberately outside the route-record
    // set so this line cannot alter the accepted route digest.
    fprintf(stderr,
            "[delivery-hash] external=%llu no_cycle=%016llX cycle_sum=%llu\n",
            (unsigned long long)g_delivery_digest.external_count,
            (unsigned long long)g_delivery_digest.hash_no_cycle,
            (unsigned long long)g_delivery_digest.cycle_sum);
    // The phase the recorded history cannot see. Same tags-outside-the-record
    // rule as above: this line may not alter the accepted route digest.
    fprintf(stderr,
            "[delivery-play] count=%llu hash=%016llX no_cycle=%016llX "
            "cycle_sum=%llu first_cycle=%llu last_cycle=%llu\n",
            (unsigned long long)g_delivery_digest.play_count,
            (unsigned long long)g_delivery_digest.play_hash,
            (unsigned long long)g_delivery_digest.play_hash_no_cycle,
            (unsigned long long)g_delivery_digest.play_cycle_sum,
            (unsigned long long)g_delivery_digest.play_first_cycle,
            (unsigned long long)g_delivery_digest.play_last_cycle);
    for (u32 index = 0u;
         index < g_delivery_digest.external_history_count; index++) {
        const BluewakeDeliveryPoint* point =
            &g_delivery_digest.external_history[index];
        fprintf(stderr,
                "[cycle-delivery] external[%u] ordinal=%llu cycle=%llu "
                "prefix=%016llX cause=0x%08X pc=0x%08X context=0x%08X\n",
                index, (unsigned long long)point->ordinal,
                (unsigned long long)point->cycle,
                (unsigned long long)point->prefix_hash,
                point->cause, point->pc, point->context);
    }
    for (u32 index = 0u; index < g_delivery_digest.dsp_sample_count; index++) {
        const BluewakeDeliveryPoint* point =
            &g_delivery_digest.dsp_samples[index];
        fprintf(stderr,
                "[cycle-delivery] dsp[%u] ordinal=%llu cycle=%llu "
                "prefix=%016llX cause=0x%08X pc=0x%08X context=0x%08X\n",
                index, (unsigned long long)point->ordinal,
                (unsigned long long)point->cycle,
                (unsigned long long)point->prefix_hash,
                point->cause, point->pc, point->context);
    }
    fprintf(stderr,
            "[boot-milestone] summary title_ready=%u title_retrace=%llu "
            "name_create=%u name_create_retrace=%llu name_execute=%u "
            "name_execute_retrace=%llu memcard_check=%u "
            "memcard_check_retrace=%llu file_select=%u "
            "file_select_retrace=%llu new_game_intro=%u "
            "new_game_intro_retrace=%llu name_input_complete=%u "
            "name_input_complete_retrace=%llu name_scene_change=%u "
            "name_scene_change_retrace=%llu open_scene_request=%u "
            "open_scene_request_retrace=%llu overlap_phase=%u "
            "opening_complete=%u opening_complete_retrace=%llu "
            "play_scene=%u play_scene_retrace=%llu "
            "outset_room_request=%u outset_room_request_retrace=%llu "
            "name_char_jut=%u/%u "
            "name_char_cpad=%u/%u\n",
            g_title_ready_reported ? 1u : 0u,
            (unsigned long long)g_title_ready_retrace,
            g_name_scene_create_reported ? 1u : 0u,
            (unsigned long long)g_name_scene_create_retrace,
            g_name_scene_execute_reported ? 1u : 0u,
            (unsigned long long)g_name_scene_execute_retrace,
            g_memcard_check_reported ? 1u : 0u,
            (unsigned long long)g_memcard_check_retrace,
            g_file_select_reported ? 1u : 0u,
            (unsigned long long)g_file_select_retrace,
            g_new_game_intro_reported ? 1u : 0u,
            (unsigned long long)g_new_game_intro_retrace,
            g_name_input_complete_reported ? 1u : 0u,
            (unsigned long long)g_name_input_complete_retrace,
            g_name_scene_change_reported ? 1u : 0u,
            (unsigned long long)g_name_scene_change_retrace,
            g_open_scene_request_reported ? 1u : 0u,
            (unsigned long long)g_open_scene_request_retrace,
            g_overlap_terminal_phase,
            g_opening_complete_reported ? 1u : 0u,
            (unsigned long long)g_opening_complete_retrace,
            g_play_scene_reported ? 1u : 0u,
            (unsigned long long)g_play_scene_retrace,
            g_outset_room_requested ? 1u : 0u,
            (unsigned long long)g_outset_room_request_retrace,
            g_name_character_jut_hold_reported ? 1u : 0u,
            g_name_character_jut_trigger_reported ? 1u : 0u,
            g_name_character_cpad_hold_reported ? 1u : 0u,
            g_name_character_cpad_trigger_reported ? 1u : 0u);
    fprintf(stderr,
            "[scene-draw] summary "
            "bg=%llu/%llu@%llu vrbox=%llu/%llu@%llu "
            "vrbox2=%llu/%llu@%llu sea=%llu/%llu@%llu "
            "order=before_play/after_play@first_after_retrace\n",
            (unsigned long long)scene_draw_before_play[0],
            (unsigned long long)scene_draw_after_play[0],
            (unsigned long long)(scene_draw_first_after_retrace[0] == UINT64_MAX
                                     ? 0u
                                     : scene_draw_first_after_retrace[0]),
            (unsigned long long)scene_draw_before_play[1],
            (unsigned long long)scene_draw_after_play[1],
            (unsigned long long)(scene_draw_first_after_retrace[1] == UINT64_MAX
                                     ? 0u
                                     : scene_draw_first_after_retrace[1]),
            (unsigned long long)scene_draw_before_play[2],
            (unsigned long long)scene_draw_after_play[2],
            (unsigned long long)(scene_draw_first_after_retrace[2] == UINT64_MAX
                                     ? 0u
                                     : scene_draw_first_after_retrace[2]),
            (unsigned long long)scene_draw_before_play[3],
            (unsigned long long)scene_draw_after_play[3],
            (unsigned long long)(scene_draw_first_after_retrace[3] == UINT64_MAX
                                     ? 0u
                                     : scene_draw_first_after_retrace[3]));
    fprintf(stderr,
            "[dvd-lifecycle] forwards=%llu failures=%llu callbacks=%llu/%llu "
            "execute_entries mount=%llu mount_x=%llu main_ram=%llu "
            "active=0x%08X active_done=%u jas_queue_used=%u jas_state=%u "
            "jas_saved_pc=0x%08X jas_wait=0x%08X jas_recv=0x%08X/0x%08X "
            "jas_paused=%u\n",
            (unsigned long long)dvd_jas_forwards,
            (unsigned long long)dvd_jas_forward_failures,
            (unsigned long long)dvd_callback_returns,
            (unsigned long long)dvd_callback_entries,
            (unsigned long long)dvd_mount_archive_entries,
            (unsigned long long)dvd_mount_x_entries,
            (unsigned long long)dvd_main_ram_entries,
            dvd_jas_active_command,
            dvd_jas_active_command == 0u
                ? 0u
                : mem_read8(&cpu, dvd_jas_active_command + 0x0Cu),
            mem_read32(&cpu, 0x806AC790u + 0x1Cu),
            mem_read16(&cpu, 0x806AD7E0u + 0x2C8u),
            mem_read32(&cpu, 0x806AD7E0u + 0x198u),
            mem_read32(&cpu, 0x806AD7E0u + 0x2DCu),
            mem_read32(&cpu, 0x806AC790u + 0x08u),
            mem_read32(&cpu, 0x806AC790u + 0x0Cu),
            mem_read8(&cpu, 0x806AC760u + 0x70u));
    fprintf(stderr,
            "[aram-dma] summary transfers=%llu rejected=%llu pending=%u "
            "direction=%u main=0x%08X aram=0x%08X length=%u\n",
            (unsigned long long)g_aram_dma.transfer_count,
            (unsigned long long)g_aram_dma.rejected_count,
            bluewake_aram_dma_interrupt_pending(&g_aram_dma) ? 1u : 0u,
            g_aram_dma.last_direction, g_aram_dma.last_main_address,
            g_aram_dma.last_aram_address, g_aram_dma.last_length);
#ifdef BLUEWAKE_HAS_DSP_ADAPTER
    if (g_dsp_adapter != NULL) {
        fprintf(stderr,
                "[dsp-lle] summary dmas=%llu first_nonzero=%u "
                "host_retrace=%llu retail_retrace=%u\n",
                (unsigned long long)g_dsp_adapter_dma_count,
                g_dsp_adapter_first_nonzero_reported ? 1u : 0u,
                (unsigned long long)g_dsp_adapter_first_nonzero_host_retrace,
                g_dsp_adapter_first_nonzero_retail_retrace);
    }
#endif
    if (g_audio_capture.path != NULL && g_audio_capture.path[0] != '\0') {
        const bool capture_ok = bluewake_audio_capture_close(&g_audio_capture);
        fprintf(stderr,
                "[audio-capture] summary path=\"%s\" rate=%u frames=%llu "
                "nonzero=%llu peak=%d hash=0x%08X ok=%u\n",
                g_audio_capture.path, g_audio_capture.sample_rate,
                (unsigned long long)g_audio_capture.frames,
                (unsigned long long)g_audio_capture.nonzero_samples,
                g_audio_capture.peak_sample, g_audio_capture.sample_hash,
                capture_ok ? 1u : 0u);
    }
    if (g_runqueue_trace)
        fprintf(stderr, "[heap-journal] callback-writes=%llu watched=%u\n",
                g_heap_write_watch_total,
                g_heap_write_watch_reports + g_heap_write_watch_control_reports);
    if (aurora_enabled)
        dol_aurora_shutdown();
#ifdef BLUEWAKE_HAS_DSP_ADAPTER
    host_dsp_adapter_shutdown();
#endif
    bluewake_card_runtime_close();
    host_cpu_free(&cpu);
    // A quit is the player closing the window: a normal end, not a failure.
    // (The Windows app shows an error box for any other status.)
    return stop_reason != NULL && strcmp(stop_reason, "quit") != 0 ? 1 : 0;
}
#undef cpu
