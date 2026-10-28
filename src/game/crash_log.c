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

/* Offset into the exe image, or false when the address is outside it (NULL
   jump, a system DLL): then only the raw address is meaningful. */
static int crash_log_in_exe(unsigned long long addr, unsigned long long base, unsigned long long* off)
{
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)(uintptr_t)base;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(uintptr_t)(base + (unsigned long long)dos->e_lfanew);
    unsigned long long size = nt->OptionalHeader.SizeOfImage;

    if (addr < base || addr - base >= size) {
        return 0;
    }
    *off = addr - base;
    return 1;
}

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
    {
        unsigned long long ip = (unsigned long long)(uintptr_t)info->ExceptionRecord->ExceptionAddress;
        unsigned long long off = 0;
        char where[48];

        if (crash_log_in_exe(ip, base, &off)) {
            snprintf(where, sizeof(where), "exe+0x%llx", off);
        } else {
            snprintf(where, sizeof(where), "outside exe");
        }
        tig_debug_printf("CRASH: exception 0x%08lx at 0x%llx (%s) (thread %lu), access %s addr 0x%llx, exe base 0x%llx\n",
            (unsigned long)info->ExceptionRecord->ExceptionCode, ip, where,
            (unsigned long)GetCurrentThreadId(),
            info->ExceptionRecord->NumberParameters > 0 && info->ExceptionRecord->ExceptionInformation[0] == 1 ? "write" : "read",
            info->ExceptionRecord->NumberParameters > 1 ? (unsigned long long)info->ExceptionRecord->ExceptionInformation[1] : 0ULL,
            base);
    }
    n = CaptureStackBackTrace(0, 24, frames, NULL);
    len = (size_t)snprintf(line, sizeof(line), "CRASH: stack (exe+off, or raw=0x...):");
    for (i = 0; i < n && len < sizeof(line) - 32; i++) {
        unsigned long long fa = (unsigned long long)(uintptr_t)frames[i];
        unsigned long long off = 0;

        if (crash_log_in_exe(fa, base, &off)) {
            len += (size_t)snprintf(line + len, sizeof(line) - len, " %llx", off);
        } else {
            len += (size_t)snprintf(line + len, sizeof(line) - len, " raw=0x%llx", fa);
        }
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
