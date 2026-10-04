// BlueWake Windows entry shim.
//
// The Windows counterpart of apple/ios/src/ios_entry.m: it fills in default
// paths beside the executable and in the player's data folder, starts the
// session log, then runs the unchanged host (runtime/host/src/main.c, compiled
// with main renamed to bluewake_host_main).
//
// App folder (scripts/windows/build.py writes it, and
// scripts/windows/package_release.py makes a download of it without the game):
//   BlueWake.exe, SDL3.dll, webgpu_dawn.dll    the host and its runtime DLLs
//   gGZLE01_recomp.dll                         the translated game module
//   game\GZLE01.iso                            the disc image the game reads
//   game\main.dol, game\rels\                  prepared from that disc
//   dsp\dsp_rom.bin, dsp\dsp_coef.bin          only for BLUEWAKE_DSP_MODE=lle
//   initial_pipeline_cache.db                  Aurora's bundled pipeline seed
//   nodtool.exe                                a download's: unpacks a .rvz
// A download has no game\ folder: the first launch asks for the player's disc
// and prepares it into the data folder (win_disc.c).
// Player data, kept outside the app folder so a rebuild never touches it:
//   %APPDATA%\BlueWake\GZLE01.card             the memory card (saves)
//   %APPDATA%\BlueWake\disc.txt, game\         a download's disc, prepared
//   %APPDATA%\BlueWake\sram.bin                the console's settings
//   %APPDATA%\BlueWake\logs\session-*.log      the newest eight sessions
//   %APPDATA%\BlueWake\states\*.bwstate        save states (F5 saves, F8 loads)
// Every default is only a default: an environment variable that is already
// set (BLUEWAKE_DISC, BLUEWAKE_CARD_PATH, ...) wins.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>

#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL3/SDL.h>
#include <aurora/aurora.h>

#include "win_disc.h"
#include "win_settings.h"

int bluewake_host_main(int argc, char** argv);

static char g_exe_dir[MAX_PATH * 4];
static char g_data_dir[MAX_PATH * 4];
static char g_log_path[MAX_PATH * 4];

static int file_exists(const char* path) {
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static int dir_exists(const char* path) {
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static void bw_default(const char* name, const char* value) {
    const char* existing = getenv(name);
    if (existing != NULL && existing[0] != '\0')
        return;
    _putenv_s(name, value);
}

static void bw_default_path(const char* name, const char* dir, const char* relative) {
    char path[MAX_PATH * 4];
    snprintf(path, sizeof path, "%s%s", dir, relative);
    bw_default(name, path);
}

static void resolve_dirs(void) {
    wchar_t wide[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, wide, (DWORD)(sizeof wide / sizeof wide[0]));
    if (n > 0 && n < sizeof wide / sizeof wide[0] &&
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, g_exe_dir, (int)sizeof g_exe_dir, NULL, NULL) > 0) {
        char* slash = strrchr(g_exe_dir, '\\');
        if (slash != NULL)
            slash[1] = '\0';
    } else {
        snprintf(g_exe_dir, sizeof g_exe_dir, ".\\");
    }
    const char* override = getenv("BLUEWAKE_DATA_DIR");
    const char* appdata = getenv("APPDATA");
    if (override != NULL && override[0] != '\0')
        snprintf(g_data_dir, sizeof g_data_dir, "%s\\", override);
    else if (appdata != NULL && appdata[0] != '\0')
        snprintf(g_data_dir, sizeof g_data_dir, "%s\\BlueWake\\", appdata);
    else
        snprintf(g_data_dir, sizeof g_data_dir, "%suser\\", g_exe_dir);
    _mkdir(g_data_dir);
}

// Session log. Everything the host writes to stdout and stderr goes to
// %APPDATA%\BlueWake\logs\session-YYYYMMDD-HHMMSS.log, each line stamped with
// the local time, and still to the terminal BlueWake was started from, if any.
static HANDLE g_console = INVALID_HANDLE_VALUE;
static FILE* g_log_file;
static HANDLE g_log_thread;

static unsigned __stdcall log_pump(void* arg) {
    const int read_fd = (int)(intptr_t)arg;
    char buffer[16384];
    char line[8192];
    size_t line_len = 0;
    for (;;) {
        const int n = _read(read_fd, buffer, sizeof buffer);
        if (n <= 0)
            break;
        if (g_console != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(g_console, buffer, (DWORD)n, &written, NULL);
        }
        for (int i = 0; i < n; i++) {
            if (line_len < sizeof line - 1)
                line[line_len++] = buffer[i];
            if (buffer[i] != '\n')
                continue;
            line[line_len] = '\0';
            SYSTEMTIME t;
            GetLocalTime(&t);
            fprintf(g_log_file, "%02u:%02u:%02u.%03u %s", t.wHour, t.wMinute, t.wSecond,
                    t.wMilliseconds, line);
            fflush(g_log_file);
            line_len = 0;
        }
    }
    return 0;
}

static void prune_logs(const char* dir, int keep) {
    char pattern[MAX_PATH * 4];
    snprintf(pattern, sizeof pattern, "%ssession-*.log", dir);
    char names[256][64];
    int count = 0;
    WIN32_FIND_DATAA data;
    HANDLE find = FindFirstFileA(pattern, &data);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do {
        if (count < 256 && strlen(data.cFileName) < sizeof names[0])
            snprintf(names[count++], sizeof names[0], "%s", data.cFileName);
    } while (FindNextFileA(find, &data));
    FindClose(find);
    // The names sort by time; remove all but the newest `keep`.
    qsort(names, (size_t)count, sizeof names[0], (int (*)(const void*, const void*))strcmp);
    for (int i = 0; i + keep < count; i++) {
        char path[MAX_PATH * 4];
        snprintf(path, sizeof path, "%s%s", dir, names[i]);
        DeleteFileA(path);
    }
}

// At exit, however the host ends: close the pipe's write ends so the pump
// reads to the end, and wait for it, or the last lines (usually the ones that
// explain the exit) would be lost with the process.
static void finish_session_log(void) {
    if (g_log_thread == NULL)
        return;
    fflush(stdout);
    fflush(stderr);
    // NUL in their place rather than closed: the game module's own exit
    // handlers run later, when it is unloaded, and a write to a closed
    // descriptor is an invalid parameter the C runtime ends the process for
    // (0xC0000409). Replacing them still closes the pipe's write ends.
    const int null_fd = _open("NUL", _O_WRONLY);
    if (null_fd >= 0) {
        _dup2(null_fd, _fileno(stdout));
        _dup2(null_fd, _fileno(stderr));
        _close(null_fd);
    } else {
        _close(_fileno(stdout));
        _close(_fileno(stderr));
    }
    WaitForSingleObject(g_log_thread, 3000);
    CloseHandle(g_log_thread);
    g_log_thread = NULL;
    fclose(g_log_file);
}

static void start_session_log(void) {
    // Also echo to where the parent pointed stderr (a console, or a file or
    // pipe it redirected to), else to the terminal BlueWake was started from:
    // the executable is a windowed one, so it has no console of its own. The
    // handle is duplicated because redirecting stderr below closes it.
    HANDLE inherited = GetStdHandle(STD_ERROR_HANDLE);
    if (inherited != NULL && inherited != INVALID_HANDLE_VALUE) {
        if (!DuplicateHandle(GetCurrentProcess(), inherited, GetCurrentProcess(), &g_console, 0,
                             FALSE, DUPLICATE_SAME_ACCESS))
            g_console = INVALID_HANDLE_VALUE;
    } else if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        g_console = CreateFileA("CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
                                NULL);
        SetConsoleOutputCP(CP_UTF8);
    }
    char logs[MAX_PATH * 4];
    snprintf(logs, sizeof logs, "%slogs\\", g_data_dir);
    _mkdir(logs);
    prune_logs(logs, 7);
    time_t now = time(NULL);
    struct tm local;
    localtime_s(&local, &now);
    char name[64];
    strftime(name, sizeof name, "session-%Y%m%d-%H%M%S.log", &local);
    snprintf(g_log_path, sizeof g_log_path, "%s%s", logs, name);
    g_log_file = fopen(g_log_path, "w");
    int fds[2];
    if (g_log_file == NULL || _pipe(fds, 65536, _O_BINARY | _O_NOINHERIT) != 0) {
        g_log_path[0] = '\0';
        return;
    }
    // A windowed process starts without valid standard streams; give them
    // descriptors first, then point both at the pipe.
    if (_fileno(stdout) < 0)
        freopen("NUL", "w", stdout);
    if (_fileno(stderr) < 0)
        freopen("NUL", "w", stderr);
    _dup2(fds[1], _fileno(stdout));
    _dup2(fds[1], _fileno(stderr));
    _close(fds[1]);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    g_log_thread = (HANDLE)_beginthreadex(NULL, 0, log_pump, (void*)(intptr_t)fds[0], 0, NULL);
    atexit(finish_session_log);
}

static void fatal_box(const char* message);

// BLUEWAKE_HOST_PROFILE=FILE: a developer's sampling profiler. A thread stops
// the game thread about every millisecond, records where it was, and at exit
// FILE lists the samples by module and offset, most frequent first (symbolize
// with the linker map, -Wl,/MAP). Windows' own profilers need elevation.
#define PROFILE_SLOTS (1u << 20)
static HANDLE g_profile_thread;
static HANDLE g_profile_target;
static volatile LONG g_profile_stop;
static DWORD64* g_profile_pc;
static unsigned* g_profile_count;
static const char* g_profile_path;

static void profile_record(DWORD64 key) {
    unsigned slot = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 44) & (PROFILE_SLOTS - 1);
    for (unsigned probe = 0; probe < 64; probe++, slot = (slot + 1) & (PROFILE_SLOTS - 1)) {
        if (g_profile_pc[slot] == key || g_profile_pc[slot] == 0) {
            g_profile_pc[slot] = key;
            g_profile_count[slot]++;
            return;
        }
    }
}

// BlueWake's own code, the executable and (once loaded) the game module, as
// address ranges. Nothing that takes a lock may run while the game thread is
// stopped: it may be stopped holding that lock (the loader's, for one).
static DWORD64 g_own_lo[2], g_own_hi[2];
static ULONG_PTR g_stack_lo, g_stack_hi;

static void profile_module_range(HMODULE module, int slot) {
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)module;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)((const char*)module + dos->e_lfanew);
    g_own_lo[slot] = (DWORD64)(uintptr_t)module;
    g_own_hi[slot] = g_own_lo[slot] + nt->OptionalHeader.SizeOfImage;
}

static int profile_own(DWORD64 pc) {
    for (int i = 0; i < 2; i++) {
        if (pc >= g_own_lo[i] && pc < g_own_hi[i])
            return 1;
    }
    return 0;
}

// A sample in system code (a wait, the C runtime) is also charged to the
// first BlueWake frame below it, recorded with the top bit set. The walk runs
// after the thread resumes, on a stack that may have moved on: every step is
// kept to the thread's stack and guarded, and a wrong frame only mis-charges
// one sample.
#define PROFILE_CALLER_BIT (1ull << 63)

static DWORD64 profile_caller(CONTEXT unwind) {
    __try {
        for (int depth = 0; depth < 24 && unwind.Rip != 0; depth++) {
            if (unwind.Rsp < g_stack_lo || unwind.Rsp + 8 > g_stack_hi)
                return 0;
            DWORD64 image_base = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(unwind.Rip, &image_base, NULL);
            if (function == NULL) {
                unwind.Rip = *(DWORD64*)(uintptr_t)unwind.Rsp;
                unwind.Rsp += 8;
            } else {
                PVOID handler_data;
                DWORD64 frame;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, unwind.Rip, function, &unwind, &handler_data,
                                 &frame, NULL);
            }
            if (profile_own(unwind.Rip))
                return unwind.Rip;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}

static unsigned __stdcall profile_main(void* arg) {
    (void)arg;
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    while (!g_profile_stop) {
        if (g_own_hi[1] == 0) {
            HMODULE game = GetModuleHandleA("gGZLE01_recomp.dll");
            if (game != NULL)
                profile_module_range(game, 1);
        }
        if (SuspendThread(g_profile_target) != (DWORD)-1) {
            CONTEXT context;
            memset(&context, 0, sizeof context);
            context.ContextFlags = CONTEXT_FULL;
            const BOOL ok = GetThreadContext(g_profile_target, &context);
            ResumeThread(g_profile_target);
            if (ok) {
                profile_record(context.Rip);
                const DWORD64 caller = profile_own(context.Rip) ? 0 : profile_caller(context);
                if (caller != 0)
                    profile_record(caller | PROFILE_CALLER_BIT);
            }
        }
        LARGE_INTEGER due;
        due.QuadPart = -10000;  // 1 ms
        if (timer != NULL && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
            WaitForSingleObject(timer, INFINITE);
        else
            Sleep(1);
    }
    if (timer != NULL)
        CloseHandle(timer);
    return 0;
}

static int profile_order(const void* a, const void* b) {
    const unsigned ca = g_profile_count[*(const unsigned*)a], cb = g_profile_count[*(const unsigned*)b];
    return ca < cb ? 1 : ca > cb ? -1 : 0;
}

static void finish_profile(void) {
    if (g_profile_thread == NULL)
        return;
    InterlockedExchange(&g_profile_stop, 1);
    WaitForSingleObject(g_profile_thread, 2000);
    FILE* out = fopen(g_profile_path, "w");
    unsigned* order = (unsigned*)malloc(PROFILE_SLOTS * sizeof *order);
    if (out == NULL || order == NULL)
        return;
    unsigned used = 0;
    unsigned long long total = 0;
    for (unsigned i = 0; i < PROFILE_SLOTS; i++) {
        if (g_profile_count[i] != 0) {
            order[used++] = i;
            if (!(g_profile_pc[i] & PROFILE_CALLER_BIT))
                total += g_profile_count[i];
        }
    }
    qsort(order, used, sizeof *order, profile_order);
    fprintf(out, "# %llu samples of the game thread; 'caller' lines charge system code to BlueWake\n",
            total);
    for (unsigned i = 0; i < used; i++) {
        const int is_caller = (g_profile_pc[order[i]] & PROFILE_CALLER_BIT) != 0;
        const DWORD64 pc = g_profile_pc[order[i]] & ~PROFILE_CALLER_BIT;
        if (is_caller)
            fprintf(out, "caller ");
        HMODULE module = NULL;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)pc, &module)) {
            GetModuleFileNameA(module, name, sizeof name);
            const char* base = strrchr(name, '\\');
            if (base != NULL)
                memmove(name, base + 1, strlen(base));
        }
        fprintf(out, "%u %s 0x%llx\n", g_profile_count[order[i]], name,
                (unsigned long long)(pc - (DWORD64)(uintptr_t)module));
    }
    fclose(out);
    free(order);
}

static void start_profile(void) {
    g_profile_path = getenv("BLUEWAKE_HOST_PROFILE");
    if (g_profile_path == NULL || g_profile_path[0] == '\0')
        return;
    g_profile_pc = (DWORD64*)calloc(PROFILE_SLOTS, sizeof *g_profile_pc);
    g_profile_count = (unsigned*)calloc(PROFILE_SLOTS, sizeof *g_profile_count);
    if (g_profile_pc == NULL || g_profile_count == NULL ||
        !DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                         &g_profile_target, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0))
        return;
    GetCurrentThreadStackLimits(&g_stack_lo, &g_stack_hi);
    profile_module_range(GetModuleHandleA(NULL), 0);
    g_profile_thread = (HANDLE)_beginthreadex(NULL, 0, profile_main, NULL, 0, NULL);
    atexit(finish_profile);
    fprintf(stderr, "[profile] sampling the game thread into %s\n", g_profile_path);
}

// A crash leaves its exception, address and module in the session log, so a
// bug report can say where it happened. Windows then ends the process as usual.
static LONG WINAPI report_crash(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    const void* address = record->ExceptionAddress;
    HMODULE module = NULL;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)address, &module)) {
        GetModuleFileNameA(module, name, sizeof name);
        const char* base = strrchr(name, '\\');
        if (base != NULL)
            memmove(name, base + 1, strlen(base));
    }
    fprintf(stderr, "[crash] exception 0x%08lX at %p (%s+0x%llx)", (unsigned long)record->ExceptionCode,
            address, name,
            (unsigned long long)((const char*)address - (const char*)module));
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
        fprintf(stderr, ", %s address 0x%llx",
                record->ExceptionInformation[0] == 1 ? "writing" : "reading",
                (unsigned long long)record->ExceptionInformation[1]);
    fprintf(stderr, " thread %lu\n", GetCurrentThreadId());
    // The calls that led there, as module+offset (the build's PDB names them:
    // llvm-symbolizer --obj=BlueWake.exe), unwound from the exception's own
    // context with the image's unwind data.
    CONTEXT context = *info->ContextRecord;
    for (int frame = 0; frame < 24 && context.Rip != 0; ++frame) {
        HMODULE frame_module = NULL;
        char frame_name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)context.Rip, &frame_module)) {
            GetModuleFileNameA(frame_module, frame_name, sizeof frame_name);
            const char* base = strrchr(frame_name, '\\');
            if (base != NULL)
                memmove(frame_name, base + 1, strlen(base));
        }
        fprintf(stderr, "[crash] #%d %s+0x%llx\n", frame, frame_name,
                (unsigned long long)(context.Rip - (DWORD64)frame_module));
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, NULL);
        if (function == NULL) {
            // A leaf function: the return address is on top of the stack.
            context.Rip = *(DWORD64*)context.Rsp;
            context.Rsp += 8;
        } else {
            void* handler_data = NULL;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                             &handler_data, &establisher, NULL);
        }
    }
    fflush(stderr);
    // Give the log pump a moment to write the line before the process ends.
    Sleep(250);
    fatal_box("BlueWake crashed. The session log records where.");
    return EXCEPTION_CONTINUE_SEARCH;
}

// Desktop hotkeys: F11 toggles fullscreen (Return, the other half of the usual
// Alt+Return, is the game's START), F10 Smooth Motion and F9 the frame rate. A keyboard
// hook on the game thread, which pumps the window's messages, sees them before
// SDL; Aurora's single event-observer slot belongs to the mouse camera.
static HHOOK g_hotkey_hook;

static LRESULT CALLBACK hotkey_hook(int code, WPARAM key, LPARAM flags) {
    const int down = (flags & (1u << 31)) == 0;
    const int repeat = (flags & (1u << 30)) != 0;
    const int alt = (flags & (1u << 29)) != 0;
    static int trace = -1;  // BLUEWAKE_KEY_TRACE=1: every key the hook sees
    if (trace < 0)
        trace = getenv("BLUEWAKE_KEY_TRACE") != NULL && getenv("BLUEWAKE_KEY_TRACE")[0] == '1';
    if (trace)
        fprintf(stderr, "[keys] code=%d vk=0x%02X %s%s%s\n", code, (unsigned)key, down ? "down" : "up",
                repeat ? " repeat" : "", alt ? " alt" : "");
    // A key BlueWake handles (win_settings.cpp) is kept from the game: F1, F9,
    // F10, F11, Esc for the menu, and Alt+Enter, whose Return would be START.
    if (code == HC_ACTION && down && !repeat && bw_settings_key((unsigned)key, alt))
        return 1;
    if (code == HC_ACTION && key == VK_RETURN && alt)
        return 1;
    return CallNextHookEx(g_hotkey_hook, code, key, flags);
}

static void usage(void) {
    fprintf(stderr,
            "usage: BlueWake.exe [options]\n"
            "  --widescreen       16:9 (the widescreen mod: a wider camera and HUD)\n"
            "  --aspect A         4:3 (the game's own), 16:10 or 16:9\n"
            "  --smooth           60 FPS with frame interpolation (the default; F10 toggles it)\n"
            "  --120              120 FPS with frame interpolation (for a 120 Hz display)\n"
            "  --no-smooth        the game's own 30 FPS\n"
            "  --60hz, --30hz     the experimental 60 Hz game logic on or off for this session\n"
            "                     (not recommended: parts of the game run too fast)\n"
            "  --betterww         Better Wind Waker's settings, at their defaults\n"
            "  --options LIST     change them: name,-name,... (mods\\betterww\\options.txt)\n"
            "  --fullscreen       start in fullscreen (F11 toggles it while playing)\n"
            "  --window WxH       the window's size\n"
            "  --scale N          render at N x 480 lines (0: the window's own pixels)\n"
            "  --fps              show the frame rate\n"
            "  --stretch          fill the window instead of keeping the game's aspect\n"
            "  --no-mouse-camera  keep the mouse out of the camera\n"
            "  --lle-audio        run the DSP's own microcode instead of the HLE ucode\n"
            "  --mods LIST        mods compiled into the module, by name\n"
            "  --disc FILE        the disc image to read (default: the one chosen at the first launch)\n"
            "  --module FILE      the translated game module (default gGZLE01_recomp.dll)\n"
            "Keyboard: arrows D-pad, J A, K B, U X, I Y, W/A/S/D stick,\n"
            "H/F/T/G C-stick, E/R L/R, Q Z, Return START. Game controllers work too.\n"
            "Mouse: click the game, then move it to turn the camera; Esc releases it.\n"
            "F1 or Esc settings, F11 or Alt+Enter fullscreen, F10 frame interpolation, F9 frame rate,\n"
            "F5 save state, F8 load the latest state.\n"
            "The settings menu saves to %%APPDATA%%\\BlueWake\\settings.ini; options given here\n"
            "win for the session.\n");
}

static void fatal_box(const char* message) {
    // A terminal already shows the message; scripted runs set
    // BLUEWAKE_NO_DIALOG so nothing waits for a click.
    if (g_console != INVALID_HANDLE_VALUE || getenv("BLUEWAKE_NO_DIALOG") != NULL)
        return;
    char text[4096];
    if (g_log_path[0] != '\0')
        snprintf(text, sizeof text, "%s\n\nSession log:\n%s", message, g_log_path);
    else
        snprintf(text, sizeof text, "%s", message);
    MessageBoxA(NULL, text, "BlueWake", MB_OK | MB_ICONERROR);
}

int main(int argc, char** argv) {
    bw_settings_capture_environment();
    resolve_dirs();
    if (getenv("BLUEWAKE_SESSION_LOG") == NULL || strcmp(getenv("BLUEWAKE_SESSION_LOG"), "0") != 0)
        start_session_log();

    // Options become the host's own settings (scripts/mac/run_host.sh lists
    // them): the host turns BLUEWAKE_ASPECT into the widescreen mod and the
    // frame buffer's shape, and BLUEWAKE_MODS=betterww into Better Wind
    // Waker's options.
    const char* module_arg = NULL;
    char mods[256] = "";
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        const int more = i + 1 < argc;
        if (strcmp(a, "--disc") == 0 && more) {
            _putenv_s("BLUEWAKE_DISC", argv[++i]);
        } else if (strcmp(a, "--module") == 0 && more) {
            module_arg = argv[++i];
        } else if (strcmp(a, "--mods") == 0 && more) {
            snprintf(mods + strlen(mods), sizeof mods - strlen(mods), "%s%s", mods[0] ? "," : "", argv[++i]);
        } else if (strcmp(a, "--betterww") == 0) {
            snprintf(mods + strlen(mods), sizeof mods - strlen(mods), "%sbetterww", mods[0] ? "," : "");
        } else if (strcmp(a, "--options") == 0 && more) {
            _putenv_s("BLUEWAKE_OPTIONS", argv[++i]);
        } else if (strcmp(a, "--widescreen") == 0) {
            _putenv_s("BLUEWAKE_ASPECT", "16:9");
        } else if (strcmp(a, "--aspect") == 0 && more) {
            _putenv_s("BLUEWAKE_ASPECT", argv[++i]);
        } else if (strcmp(a, "--smooth") == 0) {
            // Aurora reads DOL_AURORA_FRAME_INTERP and DOL_AURORA_SHOW_FPS in
            // static initializers, before main: set them through its API.
            _putenv_s("DOL_AURORA_FRAME_INTERP", "1");
            _putenv_s("DOL_AURORA_FRAME_INTERP_STEPS", "1");
            aurora_set_frame_interp_steps(1);
            aurora_set_frame_interpolation(true);
        } else if (strcmp(a, "--120") == 0) {
            _putenv_s("DOL_AURORA_FRAME_INTERP", "1");
            _putenv_s("DOL_AURORA_FRAME_INTERP_STEPS", "3");
            aurora_set_frame_interp_steps(3);
            aurora_set_frame_interpolation(true);
        } else if (strcmp(a, "--no-smooth") == 0) {
            _putenv_s("DOL_AURORA_FRAME_INTERP", "0");
            aurora_set_frame_interpolation(false);
        } else if (strcmp(a, "--60hz") == 0) {
            _putenv_s("BLUEWAKE_SIMULATION_60HZ", "1");
        } else if (strcmp(a, "--30hz") == 0) {
            _putenv_s("BLUEWAKE_SIMULATION_60HZ", "0");
        } else if (strcmp(a, "--fullscreen") == 0) {
            _putenv_s("DOL_AURORA_FULLSCREEN", "1");
        } else if (strcmp(a, "--window") == 0 && more) {
            _putenv_s("DOL_AURORA_WINDOW", argv[++i]);
        } else if (strcmp(a, "--scale") == 0 && more) {
            _putenv_s("DOL_AURORA_RENDER_SCALE", argv[++i]);
        } else if (strcmp(a, "--fps") == 0) {
            _putenv_s("DOL_AURORA_SHOW_FPS", "1");
            aurora_set_fps_overlay(true);
        } else if (strcmp(a, "--stretch") == 0) {
            _putenv_s("DOL_AURORA_ASPECT_FIT", "0");
        } else if (strcmp(a, "--no-mouse-camera") == 0) {
            _putenv_s("BLUEWAKE_MOUSE_CAMERA", "0");
        } else if (strcmp(a, "--lle-audio") == 0) {
            _putenv_s("BLUEWAKE_DSP_MODE", "lle");
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "BlueWake: unknown option %s\n", a);
            usage();
            return 2;
        }
    }
    if (mods[0] != '\0')
        _putenv_s("BLUEWAKE_MODS", mods);

    // The player's settings (settings.ini, the F1 menu), where the command
    // line did not already choose.
    bw_settings_load(g_data_dir);
    bw_settings_apply_launch();

    // The same play configuration as the iOS app (ios_entry.m): one perf line
    // a second in the log, wall-clock pacing, Dolphin's HLE Zelda ucode, the
    // real clock for save dates, and the console's SRAM (stereo) kept with the
    // saves.
    bw_default("BLUEWAKE_PERF_LOG", "1");
    bw_default("BLUEWAKE_WALL_PACE", "1");
    bw_default("DOL_AUDIO_NO_THROTTLE", "1");
    bw_default("BLUEWAKE_RENDERER", "aurora");
    bw_default("BLUEWAKE_CYCLE_CAP", "16384");
    bw_default("BLUEWAKE_MAX_BLOCKS", "100000000000");
    bw_default("BLUEWAKE_DSP_MODE", "hle");
    bw_default("BLUEWAKE_CLOCK", "now");
    // The route digest's per-boundary observation: nothing to record in play.
    bw_default("BLUEWAKE_OVERLAP_OBSERVATION", "0");
    // The certified native matrix leaves (cmake/composite/native_math.c): the
    // Windows builder certifies them, and they fall back to the translated
    // code for anything they do not reproduce exactly.
    bw_default("BLUEWAKE_NATIVE_MATH", "1");
    bw_default_path("BLUEWAKE_SRAM", g_data_dir, "sram.bin");
    bw_default_path("BLUEWAKE_CARD_PATH", g_data_dir, "GZLE01.card");
    // Save states (main.c): F5 writes quick-<retrace>.bwstate there, F8 loads
    // the latest (F9 is the frame rate here; mouse_camera.c).
    bw_default_path("BLUEWAKE_STATE_DIR", g_data_dir, "states");
    // The shader and pipeline caches with the rest of this data folder (for the
    // default folder, where they always were), so a second copy run with its
    // own BLUEWAKE_DATA_DIR never writes the same SQLite file at the same time.
    bw_default("DOL_AURORA_CACHE_DIR", g_data_dir);
    // The game's files: the player's disc, asked for the first time (a
    // download), or those beside the app (a folder the builder made).
    const int disc = bw_disc_setup(g_exe_dir, g_data_dir);
    if (disc != 0)
        return disc < 0 ? 1 : 0;
    bw_default_path("BLUEWAKE_DOL", g_exe_dir, "game\\main.dol");
    bw_default_path("BLUEWAKE_RELS_DIR", g_exe_dir, "game\\rels");
    bw_default_path("BLUEWAKE_DISC", g_exe_dir, "game\\GZLE01.iso");
    bw_default_path("BLUEWAKE_DSP_IROM", g_exe_dir, "dsp\\dsp_rom.bin");
    bw_default_path("BLUEWAKE_DSP_COEF", g_exe_dir, "dsp\\dsp_coef.bin");

    char module[MAX_PATH * 4];
    const char* module_env = getenv("BLUEWAKE_COMPOSITE");
    if (module_arg != NULL)
        snprintf(module, sizeof module, "%s", module_arg);
    else if (module_env != NULL && module_env[0] != '\0')
        snprintf(module, sizeof module, "%s", module_env);
    else
        snprintf(module, sizeof module, "%sgGZLE01_recomp.dll", g_exe_dir);

    // Say plainly what is missing instead of failing somewhere in the host.
    const char* missing = NULL;
    const char* missing_path = NULL;
    if (!file_exists(module)) {
        missing = "the translated game module";
        missing_path = module;
    } else if (!file_exists(getenv("BLUEWAKE_DOL"))) {
        missing = "the prepared game executable";
        missing_path = getenv("BLUEWAKE_DOL");
    } else if (!dir_exists(getenv("BLUEWAKE_RELS_DIR"))) {
        missing = "the prepared game modules";
        missing_path = getenv("BLUEWAKE_RELS_DIR");
    } else if (!file_exists(getenv("BLUEWAKE_DISC"))) {
        missing = "the disc image";
        missing_path = getenv("BLUEWAKE_DISC");
    }
    if (missing != NULL) {
        char message[2048];
        snprintf(message, sizeof message,
                 "BlueWake cannot start: %s is missing.\n\n%s\n\n"
                 "Unpack the whole BlueWake download again, or build your own copy with\n"
                 "python scripts\\windows\\build.py YOUR_DISC.iso (or .rvz)",
                 missing, missing_path);
        fprintf(stderr, "[windows] %s\n", message);
        fatal_box(message);
        return 1;
    }

    fprintf(stderr, "[windows] app=%s data=%s module=%s disc=%s\n", g_exe_dir, g_data_dir, module,
            getenv("BLUEWAKE_DISC"));
    SetUnhandledExceptionFilter(report_crash);
    // 1 ms timer resolution: Aurora paces each frame's start with short
    // std::this_thread::sleep_for waits, which the C++ library turns into
    // Sleep(1), and Windows' default tick would stretch each to 15.6 ms.
    timeBeginPeriod(1);
    // Above normal priority, so a program busy in the background takes less from
    // the game: its thread and the graphics threads it waits for every frame keep
    // their order among themselves and run ahead of normal-priority work. DeepSea
    // raises its emulation thread the same way. BLUEWAKE_PRIORITY=0 keeps normal
    // priority.
    const char* priority = getenv("BLUEWAKE_PRIORITY");
    if (priority == NULL || priority[0] != '0') {
        if (SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS))
            fprintf(stderr, "[windows] priority above normal\n");
        // Windows 11 runs a process whose window is not in front with EcoQoS
        // (efficiency cores, lower clocks) and, when the window is hidden,
        // ignores its 1 ms timer resolution: a game left behind another window
        // (a guide, a chat) slowed down. Opting out keeps it at full speed.
        PROCESS_POWER_THROTTLING_STATE throttling = {
            .Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION,
            .ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | 0x4u /* IGNORE_TIMER_RESOLUTION */,
            .StateMask = 0,
        };
        if (SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof throttling))
            fprintf(stderr, "[windows] power throttling off\n");
    }
    // The name the volume mixer shows for the game's audio.
    SDL_SetAppMetadata("BlueWake", "0.1", "dev.bluewake.BlueWake");
    g_hotkey_hook = SetWindowsHookExW(WH_KEYBOARD, hotkey_hook, NULL, GetCurrentThreadId());
    bw_settings_install();
    start_profile();
    char* host_argv[3] = {argv[0], module, NULL};
    const int status = bluewake_host_main(2, host_argv);
    // The menu's Restart now: this session has quit like a closed window and
    // shut down (the memory card closed, the renderer gone), so the new one
    // starts now.
    if (status == 0 && bw_settings_finish_restart() < 0)
        fatal_box("BlueWake could not start again. Start it yourself; the settings are saved.");
    fflush(stdout);
    fflush(stderr);
    if (status != 0) {
        char message[256];
        snprintf(message, sizeof message, "BlueWake stopped with an error (status %d).", status);
        fatal_box(message);
    }
    return status;
}
