#include "crash_handler.h"

#ifdef _WIN32

#include <windows.h>

#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <tig/debug.h>

#include "net/tig_net.h"

/* dbghelp, loaded on demand (no link dependency). */
typedef struct _MINIDUMP_EXCEPTION_INFORMATION_PT {
    DWORD ThreadId;
    PEXCEPTION_POINTERS ExceptionPointers;
    BOOL ClientPointers;
} MINIDUMP_EXCEPTION_INFORMATION_PT;
typedef BOOL(WINAPI* MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);

static char crash_build[32] = "unknown";
static volatile LONG crash_entered;
static LPTOP_LEVEL_EXCEPTION_FILTER crash_prev_filter;

static FILE* crash_txt;

static void crash_out(const char* fmt, ...)
{
    char line[512];
    va_list args;

    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    tig_debug_crash_write(line);
    if (crash_txt != NULL) {
        fputs(line, crash_txt);
        fflush(crash_txt);
    }
}

static void crash_exe_dir(char* out, size_t size)
{
    char* slash;

    out[0] = '\0';
    if (GetModuleFileNameA(NULL, out, (DWORD)size) == 0) {
        out[0] = '\0';
        return;
    }
    slash = strrchr(out, '\\');
    if (slash != NULL) {
        slash[1] = '\0';
    } else {
        out[0] = '\0';
    }
}

/* "module+0xoffset" for any code address. */
static void crash_describe(DWORD64 addr, char* out, size_t size)
{
    HMODULE mod = NULL;
    char path[MAX_PATH];
    const char* name = "?";

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)(uintptr_t)addr, &mod)
        && mod != NULL && GetModuleFileNameA(mod, path, sizeof(path)) != 0) {
        name = strrchr(path, '\\') != NULL ? strrchr(path, '\\') + 1 : path;
        snprintf(out, size, "%s+0x%llx", name, (unsigned long long)(addr - (DWORD64)(uintptr_t)mod));
    } else {
        snprintf(out, size, "%s", name);
    }
}

static void crash_stack(CONTEXT* ctx)
{
    void* frames[62];
    USHORT n;
    USHORT i;
    char where[MAX_PATH + 32];

    (void)ctx;
#if defined(_M_X64) || defined(__x86_64__)
    if (ctx != NULL) {
        /* Walk the faulting thread's stack from the exception context with
           the x64 unwind tables (no PDB / dbghelp needed). */
        CONTEXT c = *ctx;
        int depth;

        crash_out("stack (from the exception context):\n");
        for (depth = 0; depth < 64 && c.Rip != 0; depth++) {
            DWORD64 image_base = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &image_base, NULL);

            crash_describe(c.Rip, where, sizeof(where));
            crash_out("  #%02d 0x%016llx %s\n", depth, (unsigned long long)c.Rip, where);
            if (fn == NULL) {
                /* leaf: return address on top of the stack */
                if (c.Rsp == 0 || IsBadReadPtr((void*)(uintptr_t)c.Rsp, sizeof(DWORD64))) {
                    break;
                }
                c.Rip = *(DWORD64*)(uintptr_t)c.Rsp;
                c.Rsp += 8;
            } else {
                void* handler_data = NULL;
                DWORD64 establisher = 0;

                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, fn, &c, &handler_data, &establisher, NULL);
            }
        }
        return;
    }
#endif
    n = RtlCaptureStackBackTrace(0, 62, frames, NULL);
    crash_out("stack (captured in the handler):\n");
    for (i = 0; i < n; i++) {
        crash_describe((DWORD64)(uintptr_t)frames[i], where, sizeof(where));
        crash_out("  #%02u 0x%016llx %s\n", (unsigned)i, (unsigned long long)(uintptr_t)frames[i], where);
    }
}

static void crash_minidump(const char* path, EXCEPTION_POINTERS* ep)
{
    HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    MiniDumpWriteDump_t write_dump;
    HANDLE file;
    MINIDUMP_EXCEPTION_INFORMATION_PT mei;
    BOOL ok;

    if (dbghelp == NULL) {
        crash_out("minidump: dbghelp.dll not available\n");
        return;
    }
    write_dump = (MiniDumpWriteDump_t)(void*)GetProcAddress(dbghelp, "MiniDumpWriteDump");
    if (write_dump == NULL) {
        crash_out("minidump: MiniDumpWriteDump not found\n");
        return;
    }
    file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        crash_out("minidump: cannot create %s\n", path);
        return;
    }
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    ok = write_dump(GetCurrentProcess(), GetCurrentProcessId(), file, 0 /* MiniDumpNormal */,
        ep != NULL ? &mei : NULL, NULL, NULL);
    CloseHandle(file);
    crash_out("minidump: %s %s\n", path, ok ? "written" : "FAILED");
}

static void crash_report(const char* what, EXCEPTION_POINTERS* ep)
{
    char dir[MAX_PATH];
    char path[MAX_PATH + 64];
    char stamp[32];
    char where[MAX_PATH + 32];
    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    HMODULE exe = GetModuleHandleA(NULL);
    const char* last;

    if (InterlockedExchange(&crash_entered, 1) != 0) {
        return; /* a crash inside the report */
    }
    if (tm != NULL) {
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", tm);
    } else {
        snprintf(stamp, sizeof(stamp), "%lld", (long long)now);
    }
    crash_exe_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%scrash-%s.txt", dir, stamp);
    crash_txt = fopen(path, "w");

    crash_out("\n==== CRASH (%s) ====\n", what);
    crash_out("build %s, crash file %s\n", crash_build, path);
    crash_out("exe base 0x%016llx (nm address = 0x140000000 + offset when linked at the default base)\n",
        (unsigned long long)(uintptr_t)exe);
    if (ep != NULL && ep->ExceptionRecord != NULL) {
        EXCEPTION_RECORD* er = ep->ExceptionRecord;

        crash_describe((DWORD64)(uintptr_t)er->ExceptionAddress, where, sizeof(where));
        crash_out("exception 0x%08lx at 0x%016llx %s\n", (unsigned long)er->ExceptionCode,
            (unsigned long long)(uintptr_t)er->ExceptionAddress, where);
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
            crash_out("access violation: %s address 0x%016llx\n",
                er->ExceptionInformation[0] == 0 ? "read" : (er->ExceptionInformation[0] == 1 ? "write" : "execute"),
                (unsigned long long)er->ExceptionInformation[1]);
        }
    }
    crash_out("thread %lu, coop: active %d host %d slot %d\n", (unsigned long)GetCurrentThreadId(),
        tig_net_is_active() ? 1 : 0, tig_net_is_host() ? 1 : 0, tig_net_is_active() ? sub_529520() : -1);
    last = tig_debug_last_coop_line();
    crash_out("last coop event: %s%s", last[0] != '\0' ? last : "(none)",
        last[0] != '\0' && last[strlen(last) - 1] == '\n' ? "" : "\n");
    crash_stack(ep != NULL ? ep->ContextRecord : NULL);

    snprintf(path, sizeof(path), "%scrash-%s.dmp", dir, stamp);
    crash_minidump(path, ep);
    crash_out("==== END CRASH ====\n");
    if (crash_txt != NULL) {
        fclose(crash_txt);
        crash_txt = NULL;
    }
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep)
{
    crash_report("unhandled exception", ep);
    if (crash_prev_filter != NULL) {
        return crash_prev_filter(ep);
    }
    return EXCEPTION_CONTINUE_SEARCH; /* the default: Windows ends the process */
}

static void crash_sigabrt(int sig)
{
    (void)sig;
    crash_report("abort / assert (SIGABRT)", NULL);
    signal(SIGABRT, SIG_DFL);
    abort();
}

void crash_handler_install(const char* build_id)
{
    if (build_id != NULL) {
        snprintf(crash_build, sizeof(crash_build), "%s", build_id);
    }
    crash_prev_filter = SetUnhandledExceptionFilter(crash_filter);
    signal(SIGABRT, crash_sigabrt);
#if defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    tig_debug_printf("Crash handler installed (crash-<time>.txt/.dmp next to the exe)\n");
}

#else

void crash_handler_install(const char* build_id)
{
    (void)build_id;
}

#endif
