#include "tig/debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "tig/memory.h"

/*
 * PT23: optional log file (-log <path> or ARCANUM_LOG_FILE). On Windows the
 * exe is a GUI-subsystem program: SDL_Log only reaches a console it can
 * attach to, so a .bat "2> file" redirect stayed empty. Every line is also
 * written here, with a millisecond timestamp, and flushed at once (a crash
 * keeps the tail).
 */
static FILE* tig_debug_log_file;
static bool tig_debug_log_file_tried;
static char tig_debug_log_path[1024];
/* PT53c: tig_debug_printf may run off the main thread (net); its static
   line buffer and the log-file repeat state are guarded by this lock. */
static SDL_SpinLock tig_debug_lock;

void tig_debug_set_log_file(const char* path)
{
    SDL_LockSpinlock(&tig_debug_lock);
    if (tig_debug_log_file != NULL) {
        fclose(tig_debug_log_file);
        tig_debug_log_file = NULL;
    }
    tig_debug_log_path[0] = '\0';
    if (path != NULL) {
        SDL_strlcpy(tig_debug_log_path, path, sizeof(tig_debug_log_path));
    }
    tig_debug_log_file_tried = false;
    SDL_UnlockSpinlock(&tig_debug_lock);
}

/* PT56a: last co-op line, for the crash report. */
static char tig_debug_last_coop[256];

/* PT77p: the last co-op / travel lines, for the crash report (static, so a
   corrupted heap does not matter). */
#define TIG_DEBUG_CRUMBS 64
#define TIG_DEBUG_CRUMB_LEN 200
static char tig_debug_crumbs[TIG_DEBUG_CRUMBS][TIG_DEBUG_CRUMB_LEN];
static unsigned int tig_debug_crumb_next;

static void tig_debug_crumb_add(const char* str)
{
    unsigned int ms = (unsigned int)SDL_GetTicks();
    char* dst = tig_debug_crumbs[tig_debug_crumb_next % TIG_DEBUG_CRUMBS];

    SDL_snprintf(dst, TIG_DEBUG_CRUMB_LEN, "[%u.%03u] %s", ms / 1000u, ms % 1000u, str);
    tig_debug_crumb_next++;
}

const char* tig_debug_breadcrumb(int index)
{
    unsigned int n = tig_debug_crumb_next < TIG_DEBUG_CRUMBS ? tig_debug_crumb_next : TIG_DEBUG_CRUMBS;

    if (index < 0 || (unsigned int)index >= n) {
        return NULL;
    }
    return tig_debug_crumbs[(tig_debug_crumb_next - n + (unsigned int)index) % TIG_DEBUG_CRUMBS];
}

const char* tig_debug_last_coop_line(void)
{
    return tig_debug_last_coop;
}

/* PT56a: crash handler only - no lock (the crash may hold it), no repeat
   state; straight to the log file and flushed. */
void tig_debug_crash_write(const char* str)
{
    if (tig_debug_log_file != NULL) {
        fputs(str, tig_debug_log_file);
        fflush(tig_debug_log_file);
    }
}

static void tig_debug_log_write(const char* str)
{
    if (SDL_strstr(str, "CoopLite") != NULL) {
        SDL_strlcpy(tig_debug_last_coop, str, sizeof(tig_debug_last_coop));
    }
    if (SDL_strstr(str, "CoopLite") != NULL || SDL_strstr(str, "WMap") != NULL
        || SDL_strstr(str, "tig_net:") != NULL || SDL_strstr(str, "MP:") != NULL
        || SDL_strstr(str, "[NETSTAT]") != NULL
        || SDL_strstr(str, "map_open") != NULL || SDL_strstr(str, "teleport") != NULL) {
        tig_debug_crumb_add(str);
    }
    if (!tig_debug_log_file_tried) {
        const char* path = tig_debug_log_path;

        tig_debug_log_file_tried = true;
        if (path[0] == '\0') {
            path = getenv("ARCANUM_LOG_FILE");
        }
        if (path != NULL && path[0] != '\0') {
            tig_debug_log_file = fopen(path, "w");
        }
    }
    if (tig_debug_log_file != NULL) {
        unsigned long long ms = (unsigned long long)SDL_GetTicks();
        /* PT53b: spam guard. Identical consecutive lines collapse into one
           "repeated N times" line; past 20 MB only co-op/net lines are kept,
           and nothing past 40 MB. */
        static char last[1024];
        static unsigned long long repeats;
        static unsigned long long written;
        static bool capped;
        int n;

        if (str[0] == '\n' && str[1] == '\0' && repeats != 0) {
            return; /* tail of a collapsed over-long line */
        }
        if (str[0] != '\n' || str[1] != '\0') {
            /* PT77q: identical consecutive [CHAT] lines must all reach the
               launcher (a player sending "+" / "ok" twice). */
            if (SDL_strstr(str, "[CHAT]") == NULL && SDL_strstr(str, "[NETSTAT]") == NULL
                && SDL_strcmp(str, last) == 0) {
                repeats++;
                return;
            }
            if (repeats != 0) {
                n = fprintf(tig_debug_log_file, "[%llu.%03llu] (last message repeated %llu times)\n",
                    ms / 1000ULL, ms % 1000ULL, repeats);
                written += n > 0 ? (unsigned long long)n : 0;
                repeats = 0;
            }
            SDL_strlcpy(last, str, sizeof(last));
        }
        if (written >= 20ULL * 1024 * 1024) {
            if (!capped) {
                capped = true;
                fprintf(tig_debug_log_file, "[%llu.%03llu] LOG CAP: 20 MB reached - only CoopLite/tig_net lines from now on\n",
                    ms / 1000ULL, ms % 1000ULL);
                fflush(tig_debug_log_file);
            }
            if (written >= 40ULL * 1024 * 1024
                || (SDL_strstr(str, "CoopLite") == NULL && SDL_strstr(str, "tig_net") == NULL
                    && SDL_strstr(str, "[CHAT]") == NULL && SDL_strstr(str, "[NETSTAT]") == NULL)) {
                return;
            }
        }
        n = fprintf(tig_debug_log_file, "[%llu.%03llu] %s", ms / 1000ULL, ms % 1000ULL, str);
        written += n > 0 ? (unsigned long long)n : 0;
        fflush(tig_debug_log_file);
    }
}

// 0x4FEB10
int tig_debug_init(TigInitInfo* init_info)
{
    time_t now;

    (void)init_info;

    tig_memory_set_output_func(tig_debug_println);
    tig_debug_println("\n");

    time(&now);
    tig_debug_println(asctime(localtime(&now)));

    return 0;
}

// 0x4FEB60
void tig_debug_exit(void)
{
    if (tig_debug_log_file != NULL) {
        fflush(tig_debug_log_file);
    }
}

// 0x4FEB70
void tig_debug_printf(const char* format, ...)
{
    // 0x604214
    static char buffer[1024];

    va_list args;
    size_t len;
    char tmp[1024];

    va_start(args, format);
    SDL_vsnprintf(tmp, sizeof(tmp), format, args);
    va_end(args);

    SDL_LockSpinlock(&tig_debug_lock);

    // NOTE: Typically, debug messages in Arcanum conclude with a newline.
    // However, in certain situations where the developers intended to display
    // progress-like messages on a single line, multiple consecutive messages
    // are utilized, such as "loading..." followed by "done, took: %d ms\n". SDL
    // does not permit such prints - it always appends a newline at the end of
    // log messages (for thread-safety reasons), which reduces the readability
    // of the logs. To support the previous approach, messages that do not end
    // with a newline are buffered.
    len = SDL_strlcat(buffer, tmp, sizeof(buffer));
    /* PT53c: strlcat returns the length it wanted; past the end the line
       is truncated and must be flushed (no read past buffer[]). */
    if (len > sizeof(buffer) - 1) {
        len = sizeof(buffer) - 1;
    }
    if (len != 0 && (len == sizeof(buffer) - 1 || buffer[len - 1] == '\n')) {
        SDL_Log("%s", buffer);
        tig_debug_log_write(buffer);
        if (buffer[len - 1] != '\n') {
            tig_debug_log_write("\n");
        }
        buffer[0] = '\0';
    }
    SDL_UnlockSpinlock(&tig_debug_lock);
}

// 0x4FEBC0
void tig_debug_println(const char* string)
{
    tig_debug_printf("%s\n", string);
}
