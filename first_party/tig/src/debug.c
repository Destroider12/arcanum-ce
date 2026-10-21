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

void tig_debug_set_log_file(const char* path)
{
    if (tig_debug_log_file != NULL) {
        fclose(tig_debug_log_file);
        tig_debug_log_file = NULL;
    }
    tig_debug_log_path[0] = '\0';
    if (path != NULL) {
        SDL_strlcpy(tig_debug_log_path, path, sizeof(tig_debug_log_path));
    }
    tig_debug_log_file_tried = false;
}

static void tig_debug_log_write(const char* str)
{
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

        fprintf(tig_debug_log_file, "[%llu.%03llu] %s", ms / 1000ULL, ms % 1000ULL, str);
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

    // NOTE: Typically, debug messages in Arcanum conclude with a newline.
    // However, in certain situations where the developers intended to display
    // progress-like messages on a single line, multiple consecutive messages
    // are utilized, such as "loading..." followed by "done, took: %d ms\n". SDL
    // does not permit such prints - it always appends a newline at the end of
    // log messages (for thread-safety reasons), which reduces the readability
    // of the logs. To support the previous approach, messages that do not end
    // with a newline are buffered.
    len = SDL_strlcat(buffer, tmp, sizeof(buffer));
    if (len != 0 && (len == sizeof(buffer) - 1 || buffer[len - 1] == '\n')) {
        SDL_Log("%s", buffer);
        tig_debug_log_write(buffer);
        if (buffer[len - 1] != '\n') {
            tig_debug_log_write("\n");
        }
        buffer[0] = '\0';
    }

    va_end(args);
}

// 0x4FEBC0
void tig_debug_println(const char* string)
{
    tig_debug_printf("%s\n", string);
}
