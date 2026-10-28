/* PT31: a crash leaves nothing in arcanum.log (the process just ends), so a
   Windows exception filter now writes the exception code, the faulting
   address and a short stack, as offsets into the exe, to the log. Match
   them with `nm -n arcanum-ce.exe` of the same build (the sha is the first
   log line). Never handles anything: the normal crash behaviour follows. */
#include "game/crash_log.h"

#include <stdint.h>
#include <stdio.h>

#include <tig/tig.h>

#if defined(_WIN32)
#include <windows.h>

static LONG WINAPI crash_log_filter(EXCEPTION_POINTERS* info)
{
    static volatile LONG once;
    void* frames[24];
    USHORT n;
    USHORT i;
    char line[512];
    size_t len;
    unsigned long long base = (unsigned long long)(uintptr_t)GetModuleHandleA(NULL);

    if (InterlockedExchange(&once, 1) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    tig_debug_printf("CRASH: exception 0x%08lx at exe+0x%llx (thread %lu), access %s addr 0x%llx\n",
        (unsigned long)info->ExceptionRecord->ExceptionCode,
        (unsigned long long)(uintptr_t)info->ExceptionRecord->ExceptionAddress - base,
        (unsigned long)GetCurrentThreadId(),
        info->ExceptionRecord->NumberParameters > 0 && info->ExceptionRecord->ExceptionInformation[0] == 1 ? "write" : "read",
        info->ExceptionRecord->NumberParameters > 1 ? (unsigned long long)info->ExceptionRecord->ExceptionInformation[1] : 0ULL);
    n = CaptureStackBackTrace(0, 24, frames, NULL);
    len = (size_t)snprintf(line, sizeof(line), "CRASH: stack (exe+):");
    for (i = 0; i < n && len < sizeof(line) - 24; i++) {
        len += (size_t)snprintf(line + len, sizeof(line) - len, " %llx",
            (unsigned long long)(uintptr_t)frames[i] - base);
    }
    tig_debug_printf("%s\n", line);
    return EXCEPTION_CONTINUE_SEARCH;
}

void crash_log_install(void)
{
    SetUnhandledExceptionFilter(crash_log_filter);
}
#else
void crash_log_install(void)
{
}
#endif
