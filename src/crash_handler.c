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

static HANDLE crash_txt = INVALID_HANDLE_VALUE; /* PT77p: no CRT heap (it may be the corrupt one) */
static char crash_subdir[64]; /* PT68: "coop\\host" etc., "" = next to the exe */

static void crash_out(const char* fmt, ...)
{
    char line[512];
    va_list args;

    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    tig_debug_crash_write(line);
    if (crash_txt != INVALID_HANDLE_VALUE) {
        DWORD written = 0;

        WriteFile(crash_txt, line, (DWORD)strlen(line), &written, NULL);
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

/* PT63: the 07239681 dump was 0 bytes ("FAILED"). Writing a dump of the
   faulting thread from that thread is unreliable (dbghelp suspends it);
   the documented way is a helper thread. Also: dbghelp from System32 (a
   stale copy next to the exe can lack the export), read+write access,
   and GetLastError in the log. */
typedef struct CrashDumpJob {
    MiniDumpWriteDump_t write_dump;
    HANDLE file;
    DWORD thread_id;
    EXCEPTION_POINTERS* ep;
    BOOL ok;
    DWORD err;
} CrashDumpJob;

static DWORD WINAPI crash_minidump_thread(LPVOID param)
{
    CrashDumpJob* job = (CrashDumpJob*)param;
    MINIDUMP_EXCEPTION_INFORMATION_PT mei;

    mei.ThreadId = job->thread_id;
    mei.ExceptionPointers = job->ep;
    mei.ClientPointers = FALSE;
    job->ok = job->write_dump(GetCurrentProcess(), GetCurrentProcessId(), job->file,
        0x00001000 /* MiniDumpWithThreadInfo */ | 0x00000004 /* MiniDumpWithHandleData */,
        job->ep != NULL ? &mei : NULL, NULL, NULL);
    job->err = job->ok ? 0 : GetLastError();
    return 0;
}

static void crash_minidump(const char* path, EXCEPTION_POINTERS* ep)
{
    HMODULE dbghelp = LoadLibraryExA("dbghelp.dll", NULL, 0x00000800 /* LOAD_LIBRARY_SEARCH_SYSTEM32 */);
    CrashDumpJob job;
    HANDLE thread;

    if (dbghelp == NULL) {
        dbghelp = LoadLibraryA("dbghelp.dll");
    }
    if (dbghelp == NULL) {
        crash_out("minidump: dbghelp.dll not available (error %lu)\n", (unsigned long)GetLastError());
        return;
    }
    memset(&job, 0, sizeof(job));
    job.write_dump = (MiniDumpWriteDump_t)(void*)GetProcAddress(dbghelp, "MiniDumpWriteDump");
    if (job.write_dump == NULL) {
        crash_out("minidump: MiniDumpWriteDump not found\n");
        return;
    }
    job.file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (job.file == INVALID_HANDLE_VALUE) {
        crash_out("minidump: cannot create %s (error %lu)\n", path, (unsigned long)GetLastError());
        return;
    }
    job.thread_id = GetCurrentThreadId();
    job.ep = ep;
    thread = CreateThread(NULL, 0, crash_minidump_thread, &job, 0, NULL);
    if (thread != NULL) {
        if (WaitForSingleObject(thread, 60000) != WAIT_OBJECT_0) {
            job.ok = FALSE;
            job.err = WAIT_TIMEOUT;
        }
        CloseHandle(thread);
    } else {
        crash_minidump_thread(&job); /* no thread: try in place */
    }
    CloseHandle(job.file);
    if (job.ok) {
        crash_out("minidump: %s written\n", path);
    } else {
        crash_out("minidump: %s FAILED (error 0x%08lx)\n", path, (unsigned long)job.err);
    }
}

static void crash_report(const char* what, EXCEPTION_POINTERS* ep)
{
    char dir[MAX_PATH];
    char path[MAX_PATH + 64];
    char stamp[32];
    char where[MAX_PATH + 32];
    SYSTEMTIME st;
    HMODULE exe = GetModuleHandleA(NULL);
    const char* last;

    if (InterlockedExchange(&crash_entered, 1) != 0) {
        return; /* a crash inside the report */
    }
    GetLocalTime(&st);
    snprintf(stamp, sizeof(stamp), "%04u%02u%02u-%02u%02u%02u", (unsigned)st.wYear, (unsigned)st.wMonth,
        (unsigned)st.wDay, (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);
    crash_exe_dir(dir, sizeof(dir));
    if (crash_subdir[0] != '\0' && strlen(dir) + strlen(crash_subdir) + 2 < sizeof(dir)) {
        strcat(dir, crash_subdir);
        CreateDirectoryA(dir, NULL); /* usually exists already */
        strcat(dir, "\\");
    }
    snprintf(path, sizeof(path), "%scrash-%s.txt", dir, stamp);
    crash_txt = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

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
    {
        /* PT77p: what led up to it (heap corruption is found late). */
        int i;
        const char* crumb;

        crash_out("last co-op / travel events (oldest first):\n");
        for (i = 0; (crumb = tig_debug_breadcrumb(i)) != NULL; i++) {
            crash_out("  %s%s", crumb, crumb[0] != '\0' && crumb[strlen(crumb) - 1] == '\n' ? "" : "\n");
        }
    }

    snprintf(path, sizeof(path), "%scrash-%s.dmp", dir, stamp);
    crash_minidump(path, ep);
    crash_out("==== END CRASH ====\n");
    if (crash_txt != INVALID_HANDLE_VALUE) {
        CloseHandle(crash_txt);
        crash_txt = INVALID_HANDLE_VALUE;
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

/* PT77p: heap corruption (0xC0000374) and a raised stack-cookie / fail-fast
   exception never reach the unhandled filter: Windows ends the process
   from RtlReportCriticalFailure. A first-chance vectored handler sees them. */
static LONG WINAPI crash_vectored(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep != NULL && ep->ExceptionRecord != NULL ? ep->ExceptionRecord->ExceptionCode : 0;

    if (code == 0xC0000374u /* STATUS_HEAP_CORRUPTION */
        || code == 0xC0000409u /* STATUS_STACK_BUFFER_OVERRUN (raised form) */
        || code == 0xC0000602u /* STATUS_FAIL_FAST_EXCEPTION */) {
        crash_report(code == 0xC0000374u ? "heap corruption" : "fail-fast", ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* PT77p: ARCANUM_COOP_DEBUGHEAP=1 - validate the process heaps 4x a second,
   so a corruption is reported close to (after) the event that caused it. */
static DWORD WINAPI crash_heap_watch(LPVOID param)
{
    (void)param;
    for (;;) {
        HANDLE heaps[64];
        DWORD n = GetProcessHeaps(64, heaps);
        DWORD i;

        if (n > 64) {
            n = 64;
        }
        for (i = 0; i < n; i++) {
            if (!HeapValidate(heaps[i], 0, NULL)) {
                crash_report("heap validation failed (ARCANUM_COOP_DEBUGHEAP)", NULL);
                InterlockedExchange(&crash_entered, 0); /* the real crash still gets its report */
                return 0;
            }
        }
        Sleep(250);
    }
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
    AddVectoredExceptionHandler(1, crash_vectored); /* PT77p */
    HeapSetInformation(NULL, HeapEnableTerminationOnCorruption, NULL, 0);
    {
        const char* dbg = getenv("ARCANUM_COOP_DEBUGHEAP");

        if (dbg != NULL && dbg[0] == '1') {
            HANDLE t = CreateThread(NULL, 0, crash_heap_watch, NULL, 0, NULL);

            if (t != NULL) {
                CloseHandle(t);
            }
            tig_debug_printf("Crash handler: ARCANUM_COOP_DEBUGHEAP=1 - heap validated every 250 ms\n");
        }
    }
    signal(SIGABRT, crash_sigabrt);
#if defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    tig_debug_printf("Crash handler installed (crash-<time>.txt/.dmp next to the exe)\n");
}

void crash_handler_set_subdir(const char* subdir)
{
    snprintf(crash_subdir, sizeof(crash_subdir), "%s", subdir != NULL ? subdir : "");
}

#else

void crash_handler_install(const char* build_id)
{
    (void)build_id;
}

void crash_handler_set_subdir(const char* subdir)
{
    (void)subdir;
}

#endif
