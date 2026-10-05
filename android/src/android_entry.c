// BlueWake Android entry shim.
//
// The Android counterpart of windows/src/win_entry.c and apple/ios/src/ios_entry.m:
// it fills in default paths in the app's storage, starts the session log, then
// runs the unchanged host (runtime/host/src/main.c, compiled with main renamed
// to bluewake_host_main). SDL's Java activity (android/java) loads libmain.so
// and calls SDL_main on its own thread.
//
// The APK (scripts/android/build.py makes it):
//   lib/arm64-v8a/libmain.so                 the host, GXRuntime, Aurora, SDL3, Dawn
//   lib/arm64-v8a/libgGZLE01_recomp.so       the translated game module
// The game's files, never in the APK (scripts/android/install.py pushes them):
//   <external>/game/GZLE01.iso               the disc image the game reads
//   <external>/game/main.dol, game/rels/     prepared from that disc
// Player data, beside them in the app's external folder
// (/sdcard/Android/data/<package>/files), reachable over adb:
//   <external>/GZLE01.card                   the memory card (saves)
//   <external>/sram.bin                      the console's settings
//   <external>/settings.ini                  the options menu's choices
//   <external>/logs/session-*.log            the newest eight sessions
// Aurora's shader and pipeline caches go to the internal files folder.
// Every default is only a default: an environment variable that is already
// set wins.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <android/log.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int bluewake_host_main(int argc, char** argv);
// The profiling runtime's writer, in the app's own training build only
// (scripts/android/build.py train_app_on_device: libmain.so compiled with
// -fprofile-instr-generate); null in every other build.
extern int __llvm_profile_write_file(void) __attribute__((weak));
extern void __llvm_profile_reset_counters(void) __attribute__((weak));
extern void __llvm_profile_set_filename(const char* name) __attribute__((weak));

#define BW_TAG "BlueWake"
#define MODULE_NAME "libgGZLE01_recomp.so"

static char g_data_dir[1024];
static char g_internal_dir[1024];
static char g_game_dir[1100];
static FILE* g_log_file;
static int g_log_pumping;   // the pump thread runs and stdout/stderr go to it
static int g_log_pump_done; // set by the pump at the pipe's end

static int file_exists(const char* path) {
    struct stat st;
    return path != NULL && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int dir_exists(const char* path) {
    struct stat st;
    return path != NULL && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void bw_default(const char* name, const char* value) {
    const char* existing = getenv(name);
    if (existing != NULL && existing[0] != '\0')
        return;
    setenv(name, value, 1);
}

static void bw_default_path(const char* name, const char* dir, const char* relative) {
    char path[1400];
    snprintf(path, sizeof path, "%s/%s", dir, relative);
    bw_default(name, path);
}

// Session log: everything the host writes to stdout and stderr goes to logcat
// (tag BlueWake) and to <external>/logs/session-YYYYMMDD-HHMMSS.log.
static void* log_pump(void* arg) {
    const int read_fd = (int)(intptr_t)arg;
    char buffer[16384];
    char line[4096];
    size_t line_len = 0;
    for (;;) {
        const ssize_t n = read(read_fd, buffer, sizeof buffer);
        if (n <= 0)
            break;
        for (ssize_t i = 0; i < n; i++) {
            if (line_len < sizeof line - 1)
                line[line_len++] = buffer[i];
            if (buffer[i] != '\n')
                continue;
            line[line_len] = '\0';
            __android_log_write(ANDROID_LOG_INFO, BW_TAG, line);
            if (g_log_file != NULL) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                struct tm t;
                localtime_r(&ts.tv_sec, &t);
                fprintf(g_log_file, "%02d:%02d:%02d.%03ld %s", t.tm_hour, t.tm_min, t.tm_sec,
                        ts.tv_nsec / 1000000, line);
                fflush(g_log_file);
            }
            line_len = 0;
        }
    }
    __atomic_store_n(&g_log_pump_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static int compare_names(const void* a, const void* b) {
    return strcmp((const char*)a, (const char*)b);
}

static void prune_logs(const char* dir, int keep) {
    DIR* d = opendir(dir);
    if (d == NULL)
        return;
    char names[256][64];
    int count = 0;
    struct dirent* entry;
    while ((entry = readdir(d)) != NULL && count < 256) {
        if (strncmp(entry->d_name, "session-", 8) == 0 && strlen(entry->d_name) < sizeof names[0])
            snprintf(names[count++], sizeof names[0], "%s", entry->d_name);
    }
    closedir(d);
    qsort(names, (size_t)count, sizeof names[0], compare_names);
    for (int i = 0; i + keep < count; i++) {
        char path[1400];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        unlink(path);
    }
}

static void start_session_log(void) {
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/logs", g_data_dir);
    mkdir(dir, 0755);
    prune_logs(dir, 7);
    const time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    char path[1400];
    snprintf(path, sizeof path, "%s/session-%04d%02d%02d-%02d%02d%02d.log", dir, t.tm_year + 1900,
             t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    g_log_file = fopen(path, "w");
    int fds[2];
    if (pipe(fds) != 0)
        return;
    // The pump first: stdout and stderr go to the pipe only once something
    // reads it, or the host's writes would block when its buffer filled.
    pthread_t thread;
    if (pthread_create(&thread, NULL, log_pump, (void*)(intptr_t)fds[0]) != 0) {
        close(fds[0]);
        close(fds[1]);
        return;
    }
    pthread_detach(thread);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    g_log_pumping = 1;
    fprintf(stderr, "[android] session log %s\n", path);
}

// Before the process ends: stdout and stderr leave the pipe, which closes its
// last writers, and the pump writes what is left and stops at the pipe's end.
// It is waited for up to five seconds (something else holding a copy of the
// pipe would keep it open).
static void finish_session_log(void) {
    if (!g_log_pumping)
        return;
    fflush(stdout);
    fflush(stderr);
    const int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        close(devnull);
    }
    for (int waited = 0; waited < 500 && !__atomic_load_n(&g_log_pump_done, __ATOMIC_ACQUIRE); waited++)
        usleep(10000);
    g_log_pumping = 0;
}

static void resolve_dirs(void) {
    const char* external = SDL_GetAndroidExternalStoragePath();
    const char* internal = SDL_GetAndroidInternalStoragePath();
    const char* override = getenv("BLUEWAKE_DATA_DIR");
    if (override != NULL && override[0] != '\0')
        snprintf(g_data_dir, sizeof g_data_dir, "%s", override);
    else if (external != NULL && external[0] != '\0')
        snprintf(g_data_dir, sizeof g_data_dir, "%s", external);
    else if (internal != NULL)
        snprintf(g_data_dir, sizeof g_data_dir, "%s", internal);
    snprintf(g_internal_dir, sizeof g_internal_dir, "%s", internal != NULL ? internal : g_data_dir);
    mkdir(g_data_dir, 0755);
    snprintf(g_game_dir, sizeof g_game_dir, "%s/game", g_data_dir);
    mkdir(g_game_dir, 0755);
}

static void fatal_box(const char* message) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Wind Waker Recomp", message, NULL);
}

int main(int argc, char** argv) {
    resolve_dirs();
    if (getenv("BLUEWAKE_SESSION_LOG") == NULL || strcmp(getenv("BLUEWAKE_SESSION_LOG"), "0") != 0)
        start_session_log();

    // Optional launch overrides the adb helper can write, one NAME=value per
    // line (scripts/android/install.py --env): the same switches the Windows
    // command line sets, without rebuilding.
    char env_path[1200];
    snprintf(env_path, sizeof env_path, "%s/launch.env", g_data_dir);
    FILE* env_file = fopen(env_path, "r");
    if (env_file != NULL) {
        char line[1024];
        while (fgets(line, sizeof line, env_file) != NULL) {
            line[strcspn(line, "\r\n")] = '\0';
            char* eq = strchr(line, '=');
            if (line[0] == '#' || eq == NULL)
                continue;
            *eq = '\0';
            setenv(line, eq + 1, 1);
            fprintf(stderr, "[android] launch.env %s=%s\n", line, eq + 1);
        }
        fclose(env_file);
    }

    // The same play configuration as the Windows and iOS apps.
    bw_default("BLUEWAKE_PERF_LOG", "1");
    bw_default("BLUEWAKE_WALL_PACE", "1");
    bw_default("DOL_AUDIO_NO_THROTTLE", "1");
    bw_default("BLUEWAKE_RENDERER", "aurora");
    bw_default("BLUEWAKE_CYCLE_CAP", "16384");
    bw_default("BLUEWAKE_MAX_BLOCKS", "100000000000");
    bw_default("BLUEWAKE_DSP_MODE", "hle");
    bw_default("BLUEWAKE_CLOCK", "now");
    bw_default("BLUEWAKE_OVERLAP_OBSERVATION", "0");
    bw_default("BLUEWAKE_NATIVE_MATH", "1");
    bw_default("DOL_AURORA_FULLSCREEN", "1");
    // A phone's screen is never 4:3: keep the game's shape, as on iOS, and
    // render at twice the GameCube's 480 lines (the options menu changes both).
    bw_default("DOL_AURORA_ASPECT_FIT", "1");
    bw_default("DOL_AURORA_RENDER_SCALE", "2");
    // Touches arrive as mouse events too: the mouse camera would take every
    // tap as a click on the game (A) and a camera grab.
    bw_default("BLUEWAKE_MOUSE_CAMERA", "0");
    bw_default_path("BLUEWAKE_SRAM", g_data_dir, "sram.bin");
    bw_default_path("BLUEWAKE_CARD_PATH", g_data_dir, "GZLE01.card");
    bw_default_path("BLUEWAKE_SETTINGS", g_data_dir, "settings.ini");
    bw_default("DOL_AURORA_CACHE_DIR", g_internal_dir);
    bw_default_path("BLUEWAKE_DOL", g_game_dir, "main.dol");
    bw_default_path("BLUEWAKE_RELS_DIR", g_game_dir, "rels");
    bw_default_path("BLUEWAKE_DISC", g_game_dir, "GZLE01.iso");
    bw_default_path("BLUEWAKE_DSP_IROM", g_game_dir, "dsp_rom.bin");
    bw_default_path("BLUEWAKE_DSP_COEF", g_game_dir, "dsp_coef.bin");

    // Say plainly what is missing instead of failing somewhere in the host.
    const char* missing = NULL;
    const char* missing_path = NULL;
    if (!file_exists(getenv("BLUEWAKE_DOL"))) {
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
                 "Wind Waker Recomp cannot start: %s is missing.\n\n%s\n\n"
                 "Push the game files from your PC with\n"
                 "python scripts/android/install.py",
                 missing, missing_path);
        fprintf(stderr, "[android] %s\n", message);
        fatal_box(message);
        return 1;
    }

    // The module is found by name in the APK's native library folder.
    const char* module_env = getenv("BLUEWAKE_COMPOSITE");
    char module[1400];
    snprintf(module, sizeof module, "%s",
             module_env != NULL && module_env[0] != '\0' ? module_env : MODULE_NAME);
    fprintf(stderr, "[android] data=%s internal=%s module=%s disc=%s\n", g_data_dir, g_internal_dir,
            module, getenv("BLUEWAKE_DISC"));
    SDL_SetAppMetadata("Wind Waker Recomp", "0.1", "dev.bluewake.BlueWake");
    // The Back button or gesture opens the options menu (settings_menu.cpp)
    // instead of closing the game.
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    char* host_argv[3] = {argc > 0 ? argv[0] : "bluewake", module, NULL};
    const int status = bluewake_host_main(2, host_argv);
    fprintf(stderr, "[android] host returned %d\n", status);
    // An optimization-training run (an instrumented module, LLVM_PROFILE_FILE
    // set by launch.env, scripts/android/build.py): write its counts, since an
    // Android app never ends through the C library's exit handlers, and end
    // the process.
    if (getenv("LLVM_PROFILE_FILE") != NULL) {
        fprintf(stderr, "[android] writing the optimization profile to %s\n", getenv("LLVM_PROFILE_FILE"));
        void* handle = dlopen(module, RTLD_NOW | RTLD_NOLOAD);
        // bluewake_profile_write: android/src/profile_flush.c, compiled into
        // training modules (the profiling runtime's own writer is hidden).
        int (*write_profile)(void) =
            handle != NULL ? (int (*)(void))dlsym(handle, "bluewake_profile_write") : NULL;
        if (write_profile != NULL) {
            const int result = write_profile();
            fprintf(stderr, "[android] optimization profile written (%d)\n", result);
        } else {
            fprintf(stderr, "[android] LLVM_PROFILE_FILE is set but the module is not instrumented\n");
        }
        // The app's counts, written here rather than by the runtime's own exit
        // handler, which would run among the static destructors; the counters
        // are then cleared, so that handler adds nothing to the file. The
        // runtime read LLVM_PROFILE_FILE when libmain.so loaded, before
        // launch.env set it, so it is given the name again.
        if (__llvm_profile_write_file != NULL) {
            if (__llvm_profile_set_filename != NULL)
                __llvm_profile_set_filename(getenv("LLVM_PROFILE_FILE"));
            const int result = __llvm_profile_write_file();
            fprintf(stderr, "[android] the app's optimization profile written (%d)\n", result);
            if (__llvm_profile_reset_counters != NULL)
                __llvm_profile_reset_counters();
        }
        finish_session_log();
        exit(status);
    }
    fflush(stdout);
    fflush(stderr);
    if (status != 0) {
        char message[256];
        snprintf(message, sizeof message, "Wind Waker Recomp stopped with an error (status %d).", status);
        fatal_box(message);
    }
    return status;
}
