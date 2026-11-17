/* NexusOS Wine campaign: console Win32 test program, built for x86_64-windows-gnu and x86-windows-gnu (zig cc).
 *
 *   winetest.exe all [--peer PATH_OF_OTHER_BITNESS_BUILD] [--host IP:PORT] [--dir WORKDIR]
 *   winetest.exe <test> ...        one group: env time memory threads seh files registry process bitness winsock
 *   winetest.exe child-...          helper modes used by the process/bitness groups
 *
 * Prints "WT PASS <group>: <what>" / "WT FAIL <group>: <what> (<detail>)" lines and ends with
 * "WT SUMMARY bits=<32|64> pass=N fail=M". Exit code = number of failures (0 = all passed). */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int passes, failures;
static const char* group = "";
static char peer[MAX_PATH], host[64] = "10.0.2.2:8921", workdir[MAX_PATH];
static const int BITS = (int)(sizeof(void*) * 8);

static void check(int ok, const char* what, const char* format, ...) {
    char detail[512] = "";
    if (!ok && format) { va_list list; va_start(list, format); vsnprintf(detail, sizeof(detail), format, list); va_end(list); }
    if (ok) { passes++; printf("WT PASS %s: %s\n", group, what); }
    else { failures++; printf("WT FAIL %s: %s (%s; GetLastError=%lu)\n", group, what, detail, (unsigned long)GetLastError()); }
    fflush(stdout);
}
static void info(const char* format, ...) {
    char text[1024]; va_list list; va_start(list, format); vsnprintf(text, sizeof(text), format, list); va_end(list);
    printf("WT INFO %s: %s\n", group, text); fflush(stdout);
}
#define CHECK(cond, what) check((cond), what, "%s", #cond)

/* ------------------------------------------------------------------ env */
static void test_env(void) {
    group = "env"; char buffer[MAX_PATH * 2]; DWORD n;
    CHECK(SetEnvironmentVariableA("NEXUS_WT", "value 1") && GetEnvironmentVariableA("NEXUS_WT", buffer, sizeof(buffer)) == 7 && !strcmp(buffer, "value 1"), "set/get environment variable");
    n = ExpandEnvironmentStringsA("%SystemRoot%\\system32", buffer, sizeof(buffer));
    check(n && !_stricmp(buffer, "C:\\windows\\system32"), "expand %SystemRoot%", "got %s", buffer);
    n = GetWindowsDirectoryA(buffer, sizeof(buffer)); check(n && !_stricmp(buffer, "C:\\windows"), "GetWindowsDirectory", "got %s", buffer);
    n = GetSystemDirectoryA(buffer, sizeof(buffer)); check(n && !_stricmp(buffer, "C:\\windows\\system32"), "GetSystemDirectory", "got %s", buffer);
    DWORD size = sizeof(buffer); CHECK(GetComputerNameA(buffer, &size) && size > 0, "GetComputerName"); info("computer name %s", buffer);
    size = sizeof(buffer); CHECK(GetUserNameA(buffer, &size) && size > 1, "GetUserName"); info("user name %s", buffer);
    OSVERSIONINFOEXA version = {.dwOSVersionInfoSize = sizeof(version)};
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    CHECK(GetVersionExA((OSVERSIONINFOA*)&version), "GetVersionEx");
#pragma GCC diagnostic pop
    info("version %lu.%lu build %lu", version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber);
    SYSTEM_INFO native; GetNativeSystemInfo(&native);
    check(native.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64, "native architecture is AMD64", "arch %u", native.wProcessorArchitecture);
    check(native.dwNumberOfProcessors >= 1 && native.dwPageSize == 4096, "processor count and page size", "cpus %lu page %lu", native.dwNumberOfProcessors, native.dwPageSize);
    info("processors %lu, allocation granularity %lu", native.dwNumberOfProcessors, native.dwAllocationGranularity);
    BOOL wow = FALSE; CHECK(IsWow64Process(GetCurrentProcess(), &wow) && wow == (BITS == 32), "IsWow64Process matches the build");
    MEMORYSTATUSEX memory = {.dwLength = sizeof(memory)}; CHECK(GlobalMemoryStatusEx(&memory) && memory.ullTotalPhys > (64ull << 20), "GlobalMemoryStatusEx");
    info("physical memory %llu MiB, available %llu MiB", memory.ullTotalPhys >> 20, memory.ullAvailPhys >> 20);
    char* block = GetEnvironmentStringsA(); CHECK(block && *block, "GetEnvironmentStrings"); FreeEnvironmentStringsA(block);
    CHECK(GetCurrentDirectoryA(sizeof(buffer), buffer) > 2 && buffer[1] == ':', "GetCurrentDirectory is a DOS path");
    info("command line %s", GetCommandLineA());
}
/* ------------------------------------------------------------------ time */
static void test_time(void) {
    group = "time"; FILETIME ft; SYSTEMTIME st, back; ULARGE_INTEGER t;
    GetSystemTimeAsFileTime(&ft); t.LowPart = ft.dwLowDateTime; t.HighPart = ft.dwHighDateTime;
    unsigned long long unix_seconds = (t.QuadPart - 116444736000000000ull) / 10000000ull;
    check(unix_seconds > 1700000000ull && unix_seconds < 4102444800ull, "system time is plausible", "unix %llu", unix_seconds);
    CHECK(FileTimeToSystemTime(&ft, &st) && SystemTimeToFileTime(&st, &ft), "FileTime <-> SystemTime");
    GetLocalTime(&back); info("local time %04u-%02u-%02u %02u:%02u:%02u", back.wYear, back.wMonth, back.wDay, back.wHour, back.wMinute, back.wSecond);
    LARGE_INTEGER f, a, b; CHECK(QueryPerformanceFrequency(&f) && f.QuadPart > 0, "QueryPerformanceFrequency");
    QueryPerformanceCounter(&a); ULONGLONG ticks = GetTickCount64(); Sleep(200); QueryPerformanceCounter(&b);
    double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart; ULONGLONG tick_ms = GetTickCount64() - ticks;
    check(ms >= 180 && ms < 1500, "Sleep(200) measured by QueryPerformanceCounter", "%.1f ms", ms);
    check(tick_ms >= 150 && tick_ms < 1600, "GetTickCount64 advances", "%llu ms", tick_ms);
    info("Sleep(200) = %.1f ms (QPC), %llu ms (tick)", ms, tick_ms);
    /* short sleeps: timer granularity */
    QueryPerformanceCounter(&a); for (int i = 0; i < 20; i++) Sleep(1); QueryPerformanceCounter(&b);
    ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart; info("20 x Sleep(1) = %.1f ms", ms);
    check(ms < 1000, "20 x Sleep(1) completes", "%.1f ms", ms);
    TIME_ZONE_INFORMATION zone; DWORD z = GetTimeZoneInformation(&zone); CHECK(z != TIME_ZONE_ID_INVALID, "GetTimeZoneInformation");
    HANDLE timer = CreateWaitableTimerA(NULL, TRUE, NULL); LARGE_INTEGER due = {.QuadPart = -1000000}; /* 100 ms */
    QueryPerformanceCounter(&a);
    CHECK(timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE) && WaitForSingleObject(timer, 5000) == WAIT_OBJECT_0, "waitable timer fires");
    QueryPerformanceCounter(&b); ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
    check(ms >= 90 && ms < 1500, "waitable timer duration", "%.1f ms", ms); CloseHandle(timer);
}
/* ------------------------------------------------------------------ exceptions (vectored) */
static volatile LONG veh_hits[8];
static volatile void* veh_fix_page;
static volatile int veh_mode;      /* what the handler should do */
static LONG CALLBACK veh(EXCEPTION_POINTERS* e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
#ifdef _WIN64
#define PC(c) ((c)->Rip)
#else
#define PC(c) ((c)->Eip)
#endif
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        if (veh_mode == 1) { InterlockedIncrement(&veh_hits[0]); PC(e->ContextRecord) += 2; return EXCEPTION_CONTINUE_EXECUTION; } /* movl %eax,(%ecx) */
        if (veh_mode == 2 && veh_fix_page) { DWORD old; InterlockedIncrement(&veh_hits[1]); VirtualProtect((void*)veh_fix_page, 4096, PAGE_READWRITE, &old); return EXCEPTION_CONTINUE_EXECUTION; }
        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO: InterlockedIncrement(&veh_hits[2]); PC(e->ContextRecord) += 2; return EXCEPTION_CONTINUE_EXECUTION;
    case EXCEPTION_ILLEGAL_INSTRUCTION: InterlockedIncrement(&veh_hits[3]); PC(e->ContextRecord) += 2; return EXCEPTION_CONTINUE_EXECUTION;
    case EXCEPTION_BREAKPOINT: InterlockedIncrement(&veh_hits[4]); if ((uintptr_t)e->ExceptionRecord->ExceptionAddress == (uintptr_t)PC(e->ContextRecord)) PC(e->ContextRecord) += 1; return EXCEPTION_CONTINUE_EXECUTION;
    case STATUS_GUARD_PAGE_VIOLATION: InterlockedIncrement(&veh_hits[5]); return EXCEPTION_CONTINUE_EXECUTION;
    case 0xE0001234: if (e->ExceptionRecord->NumberParameters == 2 && e->ExceptionRecord->ExceptionInformation[1] == 0x5A5A) InterlockedIncrement(&veh_hits[6]); return EXCEPTION_CONTINUE_EXECUTION;
    case STATUS_STACK_OVERFLOW: InterlockedIncrement(&veh_hits[7]); ExitThread(77);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static int __attribute__((noinline)) recurse(int n) { volatile char pad[4096]; pad[0] = (char)n; pad[4095] = (char)n; return n ? recurse(n - 1) + pad[0] + pad[4095] : 0; }
static DWORD WINAPI overflow_thread(void* p) { (void)p; return (DWORD)recurse(100000); }
#ifdef _WIN64
#define STORE_THROUGH_CX "movl %%eax, (%%rcx)"   /* 89 01 */
#else
#define STORE_THROUGH_CX "movl %%eax, (%%ecx)"   /* 89 01 */
#endif
#ifdef __i386__
typedef struct frame32 { struct frame32* prev; void* handler; } frame32;
static volatile LONG seh32_hits;
static EXCEPTION_DISPOSITION __cdecl seh32_handler(EXCEPTION_RECORD* record, void* frame, CONTEXT* context, void* dispatcher) {
    (void)frame; (void)dispatcher;
    if (record->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) return ExceptionContinueSearch;
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) { InterlockedIncrement(&seh32_hits); context->Eip += 2; return ExceptionContinueExecution; }
    return ExceptionContinueSearch;
}
static void __attribute__((noinline)) seh32_frame(void) {
    frame32 reg; __asm__ volatile("movl %%fs:0, %0" : "=r"(reg.prev)); reg.handler = (void*)seh32_handler;
    __asm__ volatile("movl %0, %%fs:0" :: "r"(&reg) : "memory");
    __asm__ volatile("movl %%eax, (%%ecx)" :: "a"(1), "c"(0) : "memory");
    __asm__ volatile("movl %0, %%fs:0" :: "r"(reg.prev) : "memory");
}
#endif
static void test_seh(void) {
    group = "seh";
    void* handle = AddVectoredExceptionHandler(1, veh); CHECK(handle != NULL, "AddVectoredExceptionHandler");
    veh_mode = 1; __asm__ volatile(STORE_THROUGH_CX :: "a"(1), "c"(0) : "memory"); veh_mode = 0;   /* write to NULL (2-byte instruction) */
    check(veh_hits[0] == 1, "access violation (NULL write) delivered and continued", "hits %ld", veh_hits[0]);
    char* page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READONLY); veh_fix_page = page;
    veh_mode = 2; page[10] = 42; veh_mode = 0;
    check(veh_hits[1] == 1 && page[10] == 42, "write to read-only page fixed by VirtualProtect in the handler", "hits %ld", veh_hits[1]);
    VirtualFree(page, 0, MEM_RELEASE);
    int lo = 7, hi = 0; __asm__ volatile("idivl %%ecx" : "+a"(lo), "+d"(hi) : "c"(0)); (void)lo;
    check(veh_hits[2] == 1, "integer divide by zero", "hits %ld", veh_hits[2]);
    __asm__ volatile("ud2"); check(veh_hits[3] == 1, "illegal instruction", "hits %ld", veh_hits[3]);
    __asm__ volatile("int3"); check(veh_hits[4] == 1, "breakpoint", "hits %ld", veh_hits[4]);
    char* guard = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE | PAGE_GUARD);
    volatile char value = guard[5]; (void)value; value = guard[6];
    check(veh_hits[5] == 1, "PAGE_GUARD raises once, then the page is accessible", "hits %ld", veh_hits[5]);
    VirtualFree(guard, 0, MEM_RELEASE);
    ULONG_PTR args[2] = {1, 0x5A5A}; RaiseException(0xE0001234, 0, 2, args);
    check(veh_hits[6] == 1, "RaiseException with parameters, continued", "hits %ld", veh_hits[6]);
    HANDLE t = CreateThread(NULL, 256 * 1024, overflow_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    DWORD code = 0; CHECK(t && WaitForSingleObject(t, 30000) == WAIT_OBJECT_0 && GetExitCodeThread(t, &code), "stack overflow thread finished");
    check(code == 77 && veh_hits[7] >= 1, "stack overflow raised STATUS_STACK_OVERFLOW", "exit %lu hits %ld", code, veh_hits[7]); CloseHandle(t);
    RemoveVectoredExceptionHandler(handle);
#ifdef __i386__
    seh32_frame(); check(seh32_hits == 1, "fs:[0] SEH frame handler continues after the fault", "hits %ld", seh32_hits);
#endif
    /* UnhandledExceptionFilter path without a debugger: SetUnhandledExceptionFilter is called by the top-level filter */
}
/* ------------------------------------------------------------------ memory */
static void test_memory(void) {
    group = "memory"; MEMORY_BASIC_INFORMATION mbi;
    char* big = VirtualAlloc(NULL, 256u << 20, MEM_RESERVE, PAGE_NOACCESS); CHECK(big != NULL, "reserve 256 MiB");
    CHECK(VirtualQuery(big, &mbi, sizeof(mbi)) && mbi.State == MEM_RESERVE, "VirtualQuery reserved");
    char* part = VirtualAlloc(big + (16u << 20), 64u << 20, MEM_COMMIT, PAGE_READWRITE); CHECK(part == big + (16u << 20), "commit 64 MiB inside the reservation");
    for (size_t i = 0; i < (64u << 20); i += 4096) part[i] = (char)i;
    int good = 1; for (size_t i = 0; i < (64u << 20); i += 4096) if (part[i] != (char)i) good = 0;
    CHECK(good, "touch and verify 16384 pages");
    CHECK(VirtualQuery(part, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE && mbi.RegionSize == (64u << 20), "VirtualQuery committed region");
    DWORD old; CHECK(VirtualProtect(part, 4096, PAGE_READONLY, &old) && old == PAGE_READWRITE, "VirtualProtect read-only");
    CHECK(VirtualQuery(part, &mbi, sizeof(mbi)) && mbi.Protect == PAGE_READONLY && mbi.RegionSize == 4096, "protection split the region");
    CHECK(VirtualFree(part, 4096 * 4, MEM_DECOMMIT) && VirtualQuery(part, &mbi, sizeof(mbi)) && mbi.State == MEM_RESERVE, "decommit");
    CHECK(VirtualFree(big, 0, MEM_RELEASE), "release reservation");
    /* executable memory: mov eax, 42 ; ret */
    unsigned char code[] = {0xB8, 42, 0, 0, 0, 0xC3};
    void* x = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); memcpy(x, code, sizeof(code));
    CHECK(VirtualProtect(x, 4096, PAGE_EXECUTE_READ, &old) && FlushInstructionCache(GetCurrentProcess(), x, 4096), "make generated code executable");
    CHECK(((int (*)(void))x)() == 42, "run generated code"); VirtualFree(x, 0, MEM_RELEASE);
    /* write watch (GetWriteWatch: Wine tracks dirty pages with page protection + faults) */
    char* watched = VirtualAlloc(NULL, 16 * 4096, MEM_COMMIT | MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (watched) {
        void* hits[16]; ULONG_PTR count = 16; DWORD granularity;
        ResetWriteWatch(watched, 16 * 4096); watched[3 * 4096] = 1; watched[9 * 4096 + 7] = 1;
        CHECK(!GetWriteWatch(WRITE_WATCH_FLAG_RESET, watched, 16 * 4096, hits, &count, &granularity) && count == 2 && hits[0] == watched + 3 * 4096 && hits[1] == watched + 9 * 4096, "GetWriteWatch reports the two written pages");
        VirtualFree(watched, 0, MEM_RELEASE);
    } else check(0, "MEM_WRITE_WATCH allocation", "VirtualAlloc failed");
    /* heap */
    HANDLE heap = HeapCreate(0, 0, 0); void* blocks[2000]; int ok = heap != NULL;
    for (int i = 0; i < 2000 && ok; i++) { blocks[i] = HeapAlloc(heap, 0, 16 + (i * 37) % 9000); ok = blocks[i] != NULL; if (ok) memset(blocks[i], i, 16); }
    for (int i = 0; i < 2000 && ok; i += 2) ok = HeapFree(heap, 0, blocks[i]);
    for (int i = 1; i < 2000 && ok; i += 2) ok = ((unsigned char*)blocks[i])[5] == (unsigned char)i;
    CHECK(ok && HeapValidate(heap, 0, NULL) && HeapDestroy(heap), "private heap alloc/free/validate");
    /* file mapping backed by the paging file, shared between two views */
    HANDLE section = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 1 << 20, NULL);
    char* v1 = section ? MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 0) : NULL; char* v2 = section ? MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0) : NULL;
    if (v1 && v2) { strcpy(v1 + 12345, "shared view"); }
    CHECK(v1 && v2 && v1 != v2 && !strcmp(v2 + 12345, "shared view"), "two views of one section see the same bytes");
    if (v1) UnmapViewOfFile(v1); if (v2) UnmapViewOfFile(v2); if (section) CloseHandle(section);
    MEMORY_BASIC_INFORMATION stack; VirtualQuery(&stack, &stack, sizeof(stack)); info("stack page %p protect %#lx", stack.BaseAddress, stack.Protect);
}
/* ------------------------------------------------------------------ threads */
static CRITICAL_SECTION lock; static volatile LONG counter, interlocked;
static DWORD WINAPI adder(void* p) { (void)p; for (int i = 0; i < 20000; i++) { EnterCriticalSection(&lock); counter++; LeaveCriticalSection(&lock); InterlockedIncrement(&interlocked); } return 7; }
static HANDLE ping, pong; static volatile LONG rounds;
static DWORD WINAPI ponger(void* p) { (void)p; for (int i = 0; i < 500; i++) { if (WaitForSingleObject(ping, 5000)) return 1; rounds++; SetEvent(pong); } return 0; }
static volatile LONG spin_count; static volatile int spin_stop;
static DWORD WINAPI spinner(void* p) { (void)p; while (!spin_stop) InterlockedIncrement(&spin_count); return 0; }
static DWORD tls_index; static __thread int tls_var = 5;
static DWORD WINAPI tls_thread(void* p) { TlsSetValue(tls_index, p); tls_var = (int)(uintptr_t)p; Sleep(20); return TlsGetValue(tls_index) == p && tls_var == (int)(uintptr_t)p; }
static SRWLOCK srw = SRWLOCK_INIT; static CONDITION_VARIABLE cv = CONDITION_VARIABLE_INIT; static int ready_flag;
static DWORD WINAPI cv_waker(void* p) { (void)p; Sleep(50); AcquireSRWLockExclusive(&srw); ready_flag = 1; ReleaseSRWLockExclusive(&srw); WakeAllConditionVariable(&cv); return 0; }
static volatile LONG apc_hits; static void CALLBACK apc(ULONG_PTR v) { InterlockedAdd(&apc_hits, (LONG)v); }
static volatile LONG work_hits; static DWORD WINAPI work_item(void* p) { (void)p; InterlockedIncrement(&work_hits); return 0; }
static volatile LONG address_value; static void (WINAPI *wake_single)(void*);
static DWORD WINAPI address_waker(void* p) { (void)p; Sleep(50); InterlockedExchange(&address_value, 1); wake_single((void*)&address_value); return 0; }
static void test_threads(void) {
    group = "threads"; HANDLE t[8]; DWORD code;
    InitializeCriticalSection(&lock);
    for (int i = 0; i < 8; i++) t[i] = CreateThread(NULL, 0, adder, NULL, 0, NULL);
    CHECK(WaitForMultipleObjects(8, t, TRUE, 60000) == WAIT_OBJECT_0, "8 threads finish (wait all)");
    check(counter == 160000 && interlocked == 160000, "critical section + interlocked counters", "counter %ld interlocked %ld", counter, interlocked);
    CHECK(GetExitCodeThread(t[3], &code) && code == 7, "thread exit code");
    for (int i = 0; i < 8; i++) CloseHandle(t[i]);
    ping = CreateEventA(NULL, FALSE, FALSE, NULL); pong = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE p = CreateThread(NULL, 0, ponger, NULL, 0, NULL); LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    int ok = 1; for (int i = 0; i < 500 && ok; i++) { SetEvent(ping); ok = WaitForSingleObject(pong, 5000) == WAIT_OBJECT_0; }
    QueryPerformanceCounter(&b);
    check(ok && rounds == 500, "500 event ping-pong round trips", "rounds %ld", rounds);
    info("event round trip %.1f us", (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)f.QuadPart / 500.0);
    WaitForSingleObject(p, 5000); CloseHandle(p);
    HANDLE semaphore = CreateSemaphoreA(NULL, 2, 2, NULL);
    CHECK(WaitForSingleObject(semaphore, 0) == WAIT_OBJECT_0 && WaitForSingleObject(semaphore, 0) == WAIT_OBJECT_0 && WaitForSingleObject(semaphore, 50) == WAIT_TIMEOUT, "semaphore counts");
    HANDLE mutex = CreateMutexA(NULL, TRUE, NULL); CHECK(ReleaseMutex(mutex) && !ReleaseMutex(mutex), "mutex ownership");
    HANDLE pair[2] = {semaphore, mutex}; CHECK(WaitForMultipleObjects(2, pair, FALSE, 1000) == WAIT_OBJECT_0 + 1, "wait any picks the signaled mutex");
    CloseHandle(semaphore); CloseHandle(mutex);
    /* suspend/resume (Wine: SIGUSR1 to the thread) and thread context */
    HANDLE s = CreateThread(NULL, 0, spinner, NULL, 0, NULL); Sleep(100);
    CHECK(SuspendThread(s) == 0, "SuspendThread");
    LONG c1 = spin_count; Sleep(100); LONG c2 = spin_count;
    check(c1 == c2 && c1 > 0, "suspended thread does not run", "%ld -> %ld", c1, c2);
    CONTEXT context = {.ContextFlags = CONTEXT_FULL};
    CHECK(GetThreadContext(s, &context), "GetThreadContext of a suspended thread");
#ifdef _WIN64
    info("suspended rip %#llx rsp %#llx", context.Rip, context.Rsp);
    check(context.Rip > 0x10000 && context.Rsp > 0x10000, "suspended context has user registers", "rip %#llx", context.Rip);
#else
    info("suspended eip %#lx esp %#lx cs %#lx", context.Eip, context.Esp, context.SegCs);
    check(context.Eip > 0x10000 && context.Esp > 0x10000, "suspended context has user registers", "eip %#lx", context.Eip);
#endif
    CHECK(ResumeThread(s) == 1, "ResumeThread"); Sleep(100); check(spin_count > c2, "resumed thread runs again", "%ld", spin_count);
    spin_stop = 1; WaitForSingleObject(s, 5000); CloseHandle(s);
    tls_index = TlsAlloc(); HANDLE a1 = CreateThread(NULL, 0, tls_thread, (void*)11, 0, NULL), a2 = CreateThread(NULL, 0, tls_thread, (void*)22, 0, NULL);
    DWORD r1 = 0, r2 = 0; WaitForSingleObject(a1, 5000); WaitForSingleObject(a2, 5000); GetExitCodeThread(a1, &r1); GetExitCodeThread(a2, &r2);
    check(r1 == 1 && r2 == 1 && tls_var == 5, "TLS slots and __thread variables are per thread", "%lu %lu %d", r1, r2, tls_var); CloseHandle(a1); CloseHandle(a2);
    HANDLE w = CreateThread(NULL, 0, cv_waker, NULL, 0, NULL); AcquireSRWLockExclusive(&srw); ok = 1;
    while (!ready_flag && ok) ok = SleepConditionVariableSRW(&cv, &srw, 5000, 0); ReleaseSRWLockExclusive(&srw);
    CHECK(ok && ready_flag, "SRW lock + condition variable"); WaitForSingleObject(w, 5000); CloseHandle(w);
    QueueUserAPC(apc, GetCurrentThread(), 5); QueueUserAPC(apc, GetCurrentThread(), 6);
    CHECK(SleepEx(2000, TRUE) == WAIT_IO_COMPLETION && apc_hits == 11, "user APCs run in an alertable wait");
    for (int i = 0; i < 16; i++) QueueUserWorkItem(work_item, NULL, 0);
    for (int i = 0; i < 100 && work_hits < 16; i++) Sleep(20);
    check(work_hits == 16, "thread pool work items", "%ld", work_hits);
    BOOL (WINAPI *wait_on)(volatile void*, void*, SIZE_T, DWORD) = (void*)GetProcAddress(GetModuleHandleA("kernelbase.dll"), "WaitOnAddress");
    wake_single = (void (WINAPI *)(void*))(void*)GetProcAddress(GetModuleHandleA("kernelbase.dll"), "WakeByAddressSingle");
    if (wait_on && wake_single) {
        LONG zero = 0; HANDLE z = CreateThread(NULL, 0, address_waker, NULL, 0, NULL); ok = 1;
        while (address_value == 0 && ok) ok = wait_on(&address_value, &zero, sizeof(zero), 5000);
        CHECK(ok && address_value == 1, "WaitOnAddress / WakeByAddressSingle"); WaitForSingleObject(z, 5000); CloseHandle(z);
    } else check(0, "WaitOnAddress available", "missing export");
    CHECK(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL) || GetLastError() == ERROR_ACCESS_DENIED, "SetThreadPriority");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    DeleteCriticalSection(&lock);
}
/* ------------------------------------------------------------------ files */
static void path_in(char* out, const char* name) { snprintf(out, MAX_PATH, "%s\\%s", workdir, name); }
static int write_file(const char* path, const char* text) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); DWORD n = 0;
    if (h == INVALID_HANDLE_VALUE) return 0; int ok = WriteFile(h, text, (DWORD)strlen(text), &n, NULL) && n == strlen(text); CloseHandle(h); return ok;
}
static void test_files(void) {
    group = "files"; char a[MAX_PATH], b[MAX_PATH], c[MAX_PATH];
    CreateDirectoryA(workdir, NULL);
    path_in(a, "files"); RemoveDirectoryA(a); CHECK(CreateDirectoryA(a, NULL) || GetLastError() == ERROR_ALREADY_EXISTS, "CreateDirectory");
    path_in(a, "files\\A long file name with spaces.txt"); CHECK(write_file(a, "hello long name"), "create a long-named file");
    path_in(b, "files\\a LONG file NAME with SPACES.TXT"); HANDLE h = CreateFileA(b, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    char text[64] = {0}; DWORD n = 0; CHECK(h != INVALID_HANDLE_VALUE && ReadFile(h, text, sizeof(text), &n, NULL) && n == 15 && !memcmp(text, "hello long name", 15), "open with different case and read back");
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    char shortname[MAX_PATH] = {0}; DWORD sn = GetShortPathNameA(a, shortname, sizeof(shortname));
    const char* leaf = strrchr(shortname, '\\'); leaf = leaf ? leaf + 1 : shortname;
    check(sn && strlen(leaf) <= 12 && strchr(leaf, '~'), "GetShortPathName produces an 8.3 name", "%s", shortname); info("short name %s", shortname);
    h = CreateFileA(shortname, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL); memset(text, 0, sizeof(text));
    CHECK(h != INVALID_HANDLE_VALUE && ReadFile(h, text, sizeof(text), &n, NULL) && n == 15, "open the file by its short name"); if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    char longname[MAX_PATH] = {0}; GetLongPathNameA(shortname, longname, sizeof(longname));
    check(!_stricmp(longname, a), "GetLongPathName maps it back", "%s", longname);
    for (int i = 0; i < 5; i++) { char name[64]; snprintf(name, sizeof(name), "files\\item%d.dat", i); path_in(c, name); write_file(c, "x"); }
    path_in(c, "files\\*.dat"); WIN32_FIND_DATAA find; HANDLE f = FindFirstFileA(c, &find); int found = 0;
    if (f != INVALID_HANDLE_VALUE) { do found++; while (FindNextFileA(f, &find)); FindClose(f); }
    check(found == 5, "FindFirstFile/FindNextFile with a wildcard", "found %d", found);
    path_in(c, "files\\*"); f = FindFirstFileA(c, &find); int all = 0, dots = 0;
    if (f != INVALID_HANDLE_VALUE) { do { all++; if (find.cFileName[0] == '.') dots++; } while (FindNextFileA(f, &find)); FindClose(f); }
    check(all == 8 && dots == 2, "directory listing includes . and ..", "all %d dots %d", all, dots);
    /* unicode names (FAT long names are UTF-16) */
    wchar_t wide[MAX_PATH]; swprintf(wide, MAX_PATH, L"%hs\\files\\unicode-\x03A9-\x65E5\x672C.txt", workdir);
    h = CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL); CHECK(h != INVALID_HANDLE_VALUE, "create a Unicode file name"); if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    WIN32_FIND_DATAW wfind; wchar_t pattern[MAX_PATH]; swprintf(pattern, MAX_PATH, L"%hs\\files\\unicode-*", workdir);
    HANDLE wf = FindFirstFileW(pattern, &wfind); CHECK(wf != INVALID_HANDLE_VALUE && !wcscmp(wfind.cFileName, L"unicode-\x03A9-\x65E5\x672C.txt"), "Unicode name round-trips through the directory"); if (wf != INVALID_HANDLE_VALUE) FindClose(wf);
    /* copy, move, attributes, times, size */
    path_in(b, "files\\copy.txt"); CHECK(CopyFileA(a, b, FALSE), "CopyFile");
    path_in(c, "files\\moved.txt"); CHECK(MoveFileExA(b, c, MOVEFILE_REPLACE_EXISTING) && GetFileAttributesA(b) == INVALID_FILE_ATTRIBUTES, "MoveFileEx");
    DWORD attributes = GetFileAttributesA(c); CHECK(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY), "GetFileAttributes on a file");
    path_in(b, "files"); CHECK(GetFileAttributesA(b) & FILE_ATTRIBUTE_DIRECTORY, "GetFileAttributes on a directory");
    BOOL hidden = SetFileAttributesA(c, FILE_ATTRIBUTE_HIDDEN); info("SetFileAttributes(HIDDEN) %s, attributes now %#lx", hidden ? "ok" : "failed", GetFileAttributesA(c));
    SetFileAttributesA(c, FILE_ATTRIBUTE_NORMAL);
    h = CreateFileA(c, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL); FILETIME when; SYSTEMTIME st = {2020, 2, 0, 29, 12, 34, 56, 0};
    SystemTimeToFileTime(&st, &when); CHECK(h != INVALID_HANDLE_VALUE && SetFileTime(h, NULL, NULL, &when), "SetFileTime");
    FILETIME back; LARGE_INTEGER size; CHECK(GetFileTime(h, NULL, NULL, &back) && CompareFileTime(&back, &when) == 0, "GetFileTime round-trips (2 s FAT resolution ok at even seconds)");
    CHECK(GetFileSizeEx(h, &size) && size.QuadPart == 15, "GetFileSizeEx");
    LARGE_INTEGER pos = {.QuadPart = 1 << 20}; CHECK(SetFilePointerEx(h, pos, NULL, FILE_BEGIN) && SetEndOfFile(h) && GetFileSizeEx(h, &size) && size.QuadPart == (1 << 20), "extend with SetEndOfFile");
    OVERLAPPED region = {0}; CHECK(LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 100, 0, &region) && UnlockFileEx(h, 0, 100, 0, &region), "LockFileEx/UnlockFileEx");
    HANDLE again = CreateFileA(c, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    check(again == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION, "share mode 0 blocks a second open", "handle %p", again);
    if (again != INVALID_HANDLE_VALUE) CloseHandle(again);
    CloseHandle(h);
    /* memory-mapped file */
    h = CreateFileA(c, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE map = h != INVALID_HANDLE_VALUE ? CreateFileMappingA(h, NULL, PAGE_READWRITE, 0, 0, NULL) : NULL; char* view = map ? MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, 0) : NULL;
    if (view) { memcpy(view + 4096 * 3, "mapped!", 7); FlushViewOfFile(view, 0); UnmapViewOfFile(view); }
    if (map) CloseHandle(map);
    pos.QuadPart = 4096 * 3; memset(text, 0, sizeof(text));
    CHECK(view && SetFilePointerEx(h, pos, NULL, FILE_BEGIN) && ReadFile(h, text, 7, &n, NULL) && !memcmp(text, "mapped!", 7), "write through a file mapping, read with ReadFile");
    CloseHandle(h);
    path_in(b, "files\\temp-delete.txt"); h = CreateFileA(b, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    CHECK(h != INVALID_HANDLE_VALUE && GetFileAttributesA(b) != INVALID_FILE_ATTRIBUTES, "FILE_FLAG_DELETE_ON_CLOSE file exists while open");
    CloseHandle(h); CHECK(GetFileAttributesA(b) == INVALID_FILE_ATTRIBUTES, "FILE_FLAG_DELETE_ON_CLOSE removes it on close");
    ULARGE_INTEGER avail, total, freeb; CHECK(GetDiskFreeSpaceExA("C:\\", &avail, &total, &freeb) && total.QuadPart > (100ull << 20), "GetDiskFreeSpaceEx");
    info("C: total %llu MiB free %llu MiB, drive type %u", total.QuadPart >> 20, freeb.QuadPart >> 20, GetDriveTypeA("C:\\"));
    char volume[64] = "", fsname[64] = ""; DWORD serial = 0, maxlen = 0, flags = 0;
    CHECK(GetVolumeInformationA("C:\\", volume, sizeof(volume), &serial, &maxlen, &flags, fsname, sizeof(fsname)), "GetVolumeInformation"); info("volume '%s' fs '%s' maxlen %lu", volume, fsname, maxlen);
    char temp[MAX_PATH], tempfile[MAX_PATH]; CHECK(GetTempPathA(sizeof(temp), temp) && GetTempFileNameA(temp, "nwt", 0, tempfile) && DeleteFileA(tempfile), "GetTempPath/GetTempFileName");
    /* change notification (wineserver: inotify) */
    path_in(b, "files"); HANDLE dir = CreateFileA(b, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
    if (dir != INVALID_HANDLE_VALUE) {
        static DWORD notes[1024]; OVERLAPPED ov = {.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL)};
        BOOL issued = ReadDirectoryChangesW(dir, notes, sizeof(notes), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME, NULL, &ov, NULL);
        path_in(c, "files\\notify-me.txt"); write_file(c, "n");
        DWORD got = 0; BOOL done = issued && WaitForSingleObject(ov.hEvent, 5000) == WAIT_OBJECT_0 && GetOverlappedResult(dir, &ov, &got, FALSE);
        FILE_NOTIFY_INFORMATION* note = (FILE_NOTIFY_INFORMATION*)notes;
        check(done && got && note->Action == FILE_ACTION_ADDED && !wcsncmp(note->FileName, L"notify-me.txt", note->FileNameLength / 2), "ReadDirectoryChangesW sees a new file", "issued %d done %d got %lu", issued, done, got);
        CancelIo(dir); CloseHandle(ov.hEvent); CloseHandle(dir);
    } else check(0, "open directory for change notification", "CreateFile failed");
    /* clean up */
    path_in(b, "files\\*"); f = FindFirstFileA(b, &find);
    if (f != INVALID_HANDLE_VALUE) { do { if (find.cFileName[0] != '.') { char victim[MAX_PATH]; snprintf(victim, MAX_PATH, "%s\\files\\%s", workdir, find.cFileName); DeleteFileA(victim); } } while (FindNextFileA(f, &find)); FindClose(f); }
    swprintf(wide, MAX_PATH, L"%hs\\files\\unicode-\x03A9-\x65E5\x672C.txt", workdir); DeleteFileW(wide);
    path_in(b, "files"); CHECK(RemoveDirectoryA(b), "RemoveDirectory after deleting the files");
}
/* ------------------------------------------------------------------ registry */
static void test_registry(void) {
    group = "registry"; HKEY key, sub; DWORD type, size, dword = 0xDEADBEEF; ULONGLONG qword = 0x123456789ABCDEFull; char text[256];
    CHECK(!RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\NexusWineTest", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL), "RegCreateKeyEx HKCU");
    CHECK(!RegSetValueExA(key, "Text", 0, REG_SZ, (const BYTE*)"hello registry", 15), "set REG_SZ");
    CHECK(!RegSetValueExA(key, "Number", 0, REG_DWORD, (const BYTE*)&dword, 4), "set REG_DWORD");
    CHECK(!RegSetValueExA(key, "Big", 0, REG_QWORD, (const BYTE*)&qword, 8), "set REG_QWORD");
    CHECK(!RegSetValueExA(key, "Multi", 0, REG_MULTI_SZ, (const BYTE*)"a\0bb\0\0", 7), "set REG_MULTI_SZ");
    CHECK(!RegCreateKeyExA(key, "Child\\Grandchild", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &sub, NULL), "create nested subkeys"); RegCloseKey(sub);
    size = sizeof(text); CHECK(!RegQueryValueExA(key, "Text", NULL, &type, (BYTE*)text, &size) && type == REG_SZ && !strcmp(text, "hello registry"), "query REG_SZ");
    DWORD d = 0; size = 4; CHECK(!RegQueryValueExA(key, "Number", NULL, &type, (BYTE*)&d, &size) && type == REG_DWORD && d == dword, "query REG_DWORD");
    ULONGLONG q = 0; size = 8; CHECK(!RegQueryValueExA(key, "Big", NULL, &type, (BYTE*)&q, &size) && q == qword, "query REG_QWORD");
    DWORD values = 0, subkeys = 0; CHECK(!RegQueryInfoKeyA(key, NULL, NULL, NULL, &subkeys, NULL, NULL, &values, NULL, NULL, NULL, NULL) && values == 4 && subkeys == 1, "RegQueryInfoKey counts");
    int listed = 0; for (DWORD i = 0;; i++) { char name[64]; DWORD len = sizeof(name); if (RegEnumValueA(key, i, name, &len, NULL, NULL, NULL, NULL)) break; listed++; }
    check(listed == 4, "RegEnumValue", "%d", listed);
    RegCloseKey(key);
    CHECK(!RegDeleteTreeA(HKEY_CURRENT_USER, "Software\\NexusWineTest") && RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\NexusWineTest", 0, KEY_READ, &key) == ERROR_FILE_NOT_FOUND, "RegDeleteTree");
    size = sizeof(text); LONG r = RegGetValueA(HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Windows NT\\CurrentVersion", "ProductName", RRF_RT_REG_SZ, NULL, text, &size);
    CHECK(!r && strstr(text, "Windows"), "read HKLM ProductName"); info("ProductName %s", text);
    size = sizeof(text); r = RegGetValueA(HKEY_LOCAL_MACHINE, "System\\CurrentControlSet\\Control\\Session Manager\\Environment", "PROCESSOR_ARCHITECTURE", RRF_RT_REG_SZ, NULL, text, &size);
    CHECK(!r && !strcmp(text, "AMD64"), "HKLM environment PROCESSOR_ARCHITECTURE");
}
/* ------------------------------------------------------------------ processes */
static int run_child(const char* cmdline, const char* input, char* output, size_t output_size, DWORD* exit_code, DWORD timeout) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE}; HANDLE in_r, in_w, out_r, out_w;
    if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0)) return 0;
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0); SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = {.cb = sizeof(si), .dwFlags = STARTF_USESTDHANDLES, .hStdInput = in_r, .hStdOutput = out_w, .hStdError = out_w};
    PROCESS_INFORMATION pi; char line[2048]; snprintf(line, sizeof(line), "%s", cmdline);
    BOOL started = CreateProcessA(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    CloseHandle(in_r); CloseHandle(out_w);
    if (!started) { CloseHandle(in_w); CloseHandle(out_r); return 0; }
    DWORD n; if (input) WriteFile(in_w, input, (DWORD)strlen(input), &n, NULL); CloseHandle(in_w);
    size_t used = 0; for (;;) { if (used + 1 >= output_size) break; if (!ReadFile(out_r, output + used, (DWORD)(output_size - 1 - used), &n, NULL) || !n) break; used += n; }
    output[used] = 0; CloseHandle(out_r);
    int ok = WaitForSingleObject(pi.hProcess, timeout) == WAIT_OBJECT_0; GetExitCodeProcess(pi.hProcess, exit_code);
    if (!ok) TerminateProcess(pi.hProcess, 99);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return ok;
}
static void self_path(char* out) { GetModuleFileNameA(NULL, out, MAX_PATH); }
static void test_process(void) {
    group = "process"; char self[MAX_PATH], cmd[MAX_PATH * 2], out[4096]; DWORD code = 0;
    self_path(self); snprintf(cmd, sizeof(cmd), "\"%s\" child-echo", self);
    CHECK(run_child(cmd, "nexus wine pipes\n", out, sizeof(out), &code, 60000) && code == 42 && strstr(out, "ECHO[NEXUS WINE PIPES]"), "CreateProcess with stdin/stdout pipes and exit code");
    if (!strstr(out, "ECHO[")) info("child output: %s", out);
    SetEnvironmentVariableA("NEXUS_WT_CHILD", "inherited"); snprintf(cmd, sizeof(cmd), "\"%s\" child-env", self);
    CHECK(run_child(cmd, NULL, out, sizeof(out), &code, 60000) && code == 0 && strstr(out, "ENV inherited"), "environment is inherited by the child");
    CHECK(run_child("cmd.exe /c echo cmd says hi& exit /b 3", NULL, out, sizeof(out), &code, 60000) && code == 3 && strstr(out, "cmd says hi"), "cmd.exe /c with output and exit code");
    /* named pipe between this process and a child */
    HANDLE pipe = CreateNamedPipeA("\\\\.\\pipe\\nexus-wt-pipe", PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 4096, 4096, 0, NULL);
    CHECK(pipe != INVALID_HANDLE_VALUE, "CreateNamedPipe");
    STARTUPINFOA si = {.cb = sizeof(si)}; PROCESS_INFORMATION pi; snprintf(cmd, sizeof(cmd), "\"%s\" child-pipe", self);
    BOOL started = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    BOOL connected = started && (ConnectNamedPipe(pipe, NULL) || GetLastError() == ERROR_PIPE_CONNECTED);
    char message[128] = {0}; DWORD n = 0;
    CHECK(connected && ReadFile(pipe, message, sizeof(message), &n, NULL) && !strcmp(message, "hello over a named pipe"), "named pipe message from the child");
    CHECK(WriteFile(pipe, "reply", 6, &n, NULL), "reply over the named pipe");
    if (started) { CHECK(WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &code) && code == 0, "child read the reply and exited 0"); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    CloseHandle(pipe);
    /* terminate a sleeping child */
    snprintf(cmd, sizeof(cmd), "\"%s\" child-sleep", self);
    started = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    if (started) { Sleep(500); CHECK(TerminateProcess(pi.hProcess, 55) && WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &code) && code == 55, "TerminateProcess"); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    else check(0, "start a child to terminate", "CreateProcess failed");
}
/* ------------------------------------------------------------------ 32 <-> 64-bit interaction */
static void test_bitness(void) {
    group = "bitness"; if (!peer[0]) { info("no --peer given: skipped"); return; }
    char cmd[MAX_PATH * 2], out[4096]; DWORD code = 0; int peer_bits = 0, peer_wow = -1; unsigned long long address = 0; DWORD pid = 0;
    HANDLE section = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 65536, "Local\\nexus-wt-shared");
    char* shared = section ? MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 0) : NULL; if (shared) strcpy(shared, "from the parent");
    HANDLE event = CreateEventA(NULL, TRUE, FALSE, "Local\\nexus-wt-event");
    snprintf(cmd, sizeof(cmd), "\"%s\" child-info", peer);
    CHECK(run_child(cmd, NULL, out, sizeof(out), &code, 120000) && code == 0, "start the other-bitness build with pipes");
    const char* line = strstr(out, "INFO bits="); if (line) sscanf(line, "INFO bits=%d wow=%d", &peer_bits, &peer_wow);
    check(peer_bits == (BITS == 64 ? 32 : 64) && peer_wow == (peer_bits == 32), "peer reports the other bitness and its WoW64 state", "out: %s", out);
    CHECK(strstr(out, "SHARED from the parent") != NULL, "named shared memory visible across bitness");
    CHECK(shared && !strcmp(shared + 1024, "written by the peer"), "peer wrote into the shared section");
    CHECK(WaitForSingleObject(event, 0) == WAIT_OBJECT_0, "peer signaled the named event");
    if (BITS == 32) { info("memory access into a 64-bit process is not possible from WoW64 (as on Windows): skipped"); goto done; }
    /* cross-bitness process memory access: a 64-bit parent reads/writes the 32-bit peer's memory */
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE}; HANDLE out_r, out_w, in_r, in_w; CreatePipe(&out_r, &out_w, &sa, 0); CreatePipe(&in_r, &in_w, &sa, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0); SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = {.cb = sizeof(si), .dwFlags = STARTF_USESTDHANDLES, .hStdInput = in_r, .hStdOutput = out_w, .hStdError = out_w}; PROCESS_INFORMATION pi;
    snprintf(cmd, sizeof(cmd), "\"%s\" child-memory", peer);
    BOOL started = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi); CloseHandle(out_w); CloseHandle(in_r);
    char buffer[256] = {0}; DWORD n = 0; size_t used = 0;
    while (started && used < sizeof(buffer) - 1 && !strchr(buffer, '\n') && ReadFile(out_r, buffer + used, (DWORD)(sizeof(buffer) - 1 - used), &n, NULL) && n) used += n;
    if (sscanf(buffer, "ADDR %llx PID %lu", &address, &pid) == 2 && address) {
        BOOL wow = FALSE; IsWow64Process(pi.hProcess, &wow); check(wow == (BITS == 64), "IsWow64Process on the peer handle", "wow %d", wow);
        char seen[32] = {0}; SIZE_T got = 0;
        CHECK(ReadProcessMemory(pi.hProcess, (void*)(uintptr_t)address, seen, 20, &got) && got == 20 && !strcmp(seen, "peer secret 1234567"), "ReadProcessMemory across bitness");
        CHECK(WriteProcessMemory(pi.hProcess, (void*)(uintptr_t)address, "patched by parent", 18, &got) && got == 18, "WriteProcessMemory across bitness");
        WriteFile(in_w, "go\n", 3, &n, NULL);
        CHECK(WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &code) && code == 0, "peer saw the patched memory (exit 0)");
    } else { check(0, "peer reported its buffer address", "got '%s'", buffer); if (started) TerminateProcess(pi.hProcess, 1); }
    if (started) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    CloseHandle(out_r); CloseHandle(in_w);
done:
    if (shared) UnmapViewOfFile(shared); if (section) CloseHandle(section); if (event) CloseHandle(event);
}
/* ------------------------------------------------------------------ winsock */
typedef struct { SOCKET listener; int ok; } server_arg;
static DWORD WINAPI loopback_server(void* p) {
    server_arg* s = p; SOCKET c = accept(s->listener, NULL, NULL); char buffer[64] = {0}; int n = 0;
    if (c != INVALID_SOCKET) { n = recv(c, buffer, sizeof(buffer) - 1, 0); if (n > 0) { for (int i = 0; i < n; i++) buffer[i] = (char)toupper((unsigned char)buffer[i]); send(c, buffer, n, 0); s->ok = 1; } closesocket(c); }
    return 0;
}
static void test_winsock(void) {
    group = "winsock"; WSADATA data; CHECK(!WSAStartup(MAKEWORD(2, 2), &data), "WSAStartup 2.2");
    /* loopback TCP inside the process */
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)}; int len = sizeof(a);
    CHECK(l != INVALID_SOCKET && !bind(l, (struct sockaddr*)&a, sizeof(a)) && !listen(l, 4) && !getsockname(l, (struct sockaddr*)&a, &len) && a.sin_port, "bind/listen on 127.0.0.1");
    server_arg arg = {l, 0}; HANDLE t = CreateThread(NULL, 0, loopback_server, &arg, 0, NULL);
    SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); char buffer[256] = {0};
    CHECK(!connect(c, (struct sockaddr*)&a, sizeof(a)) && send(c, "loopback", 8, 0) == 8 && recv(c, buffer, sizeof(buffer), MSG_WAITALL) == 8 && !memcmp(buffer, "LOOPBACK", 8), "loopback TCP round trip");
    closesocket(c); WaitForSingleObject(t, 10000); CloseHandle(t); closesocket(l); CHECK(arg.ok, "loopback server thread accepted");
    /* host test server (QEMU user network) */
    char ip[64]; int port = 0; snprintf(ip, sizeof(ip), "%s", host); char* colon = strchr(ip, ':'); if (colon) { *colon = 0; port = atoi(colon + 1); }
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res = NULL; char portname[16]; snprintf(portname, sizeof(portname), "%d", port);
    CHECK(!getaddrinfo(ip, portname, &hints, &res) && res, "getaddrinfo numeric host");
    if (res) {
        c = socket(res->ai_family, res->ai_socktype, res->ai_protocol); DWORD timeout = 10000; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        char message[64]; snprintf(message, sizeof(message), "PING nexus-%d\n", BITS); memset(buffer, 0, sizeof(buffer));
        int connected = !connect(c, res->ai_addr, (int)res->ai_addrlen); int sent = connected ? send(c, message, (int)strlen(message), 0) : -1;
        int got = 0; while (connected && got < (int)sizeof(buffer) - 1 && !strchr(buffer, '\n')) { int n = recv(c, buffer + got, sizeof(buffer) - 1 - got, 0); if (n <= 0) break; got += n; }
        char expected[64]; snprintf(expected, sizeof(expected), "PONG nexus-%d", BITS);
        check(connected && sent > 0 && strstr(buffer, expected), "TCP to the host test server", "connected %d sent %d got '%s' err %d", connected, sent, buffer, WSAGetLastError());
        closesocket(c); freeaddrinfo(res);
    }
    /* non-blocking connect + select on loopback with a refused port */
    c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); u_long nb = 1; ioctlsocket(c, FIONBIO, &nb); struct sockaddr_in refused = {.sin_family = AF_INET, .sin_port = htons(1), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    int r = connect(c, (struct sockaddr*)&refused, sizeof(refused)); int e = WSAGetLastError();
    fd_set w, x; FD_ZERO(&w); FD_ZERO(&x); FD_SET(c, &w); FD_SET(c, &x); struct timeval tv = {5, 0};
    int ready = (r == SOCKET_ERROR && e == WSAEWOULDBLOCK) ? select(0, NULL, &w, &x, &tv) : -1; int soerr = 0; len = sizeof(soerr); getsockopt(c, SOL_SOCKET, SO_ERROR, (char*)&soerr, &len);
    check(ready == 1 && FD_ISSET(c, &x) && soerr == WSAECONNREFUSED, "non-blocking connect to a closed port reports ECONNREFUSED", "r %d e %d ready %d soerr %d", r, e, ready, soerr);
    closesocket(c);
    char name[256]; CHECK(!gethostname(name, sizeof(name)), "gethostname"); info("hostname %s", name);
    WSACleanup();
}
/* ------------------------------------------------------------------ children */
static int child_mode(const char* mode) {
    char line[256] = {0}; DWORD n = 0;
    if (!strcmp(mode, "child-echo")) {
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE); size_t used = 0;
        while (used < sizeof(line) - 1 && ReadFile(in, line + used, (DWORD)(sizeof(line) - 1 - used), &n, NULL) && n) used += n;
        while (used && (line[used - 1] == '\n' || line[used - 1] == '\r')) line[--used] = 0;
        for (size_t i = 0; i < used; i++) line[i] = (char)toupper((unsigned char)line[i]);
        printf("ECHO[%s]\n", line); fflush(stdout); return 42;
    }
    if (!strcmp(mode, "child-env")) { char v[64] = ""; GetEnvironmentVariableA("NEXUS_WT_CHILD", v, sizeof(v)); printf("ENV %s\n", v); return 0; }
    if (!strcmp(mode, "child-sleep")) { Sleep(60000); return 1; }
    if (!strcmp(mode, "child-pipe")) {
        HANDLE p = CreateFileA("\\\\.\\pipe\\nexus-wt-pipe", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (p == INVALID_HANDLE_VALUE) return 2;
        DWORD mode_flags = PIPE_READMODE_MESSAGE; SetNamedPipeHandleState(p, &mode_flags, NULL, NULL);
        if (!WriteFile(p, "hello over a named pipe", 24, &n, NULL)) return 3;
        if (!ReadFile(p, line, sizeof(line), &n, NULL) || strcmp(line, "reply")) return 4;
        CloseHandle(p); return 0;
    }
    if (!strcmp(mode, "child-info")) {
        BOOL wow = FALSE; IsWow64Process(GetCurrentProcess(), &wow); printf("INFO bits=%d wow=%d\n", BITS, wow);
        HANDLE section = OpenFileMappingA(FILE_MAP_WRITE, FALSE, "Local\\nexus-wt-shared"); char* shared = section ? MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 0) : NULL;
        if (shared) { printf("SHARED %s\n", shared); strcpy(shared + 1024, "written by the peer"); UnmapViewOfFile(shared); }
        HANDLE event = OpenEventA(EVENT_MODIFY_STATE, FALSE, "Local\\nexus-wt-event"); if (event) { SetEvent(event); CloseHandle(event); }
        if (section) CloseHandle(section); fflush(stdout); return shared && event ? 0 : 5;
    }
    if (!strcmp(mode, "child-memory")) {
        static char secret[64] = "peer secret 1234567";
        printf("ADDR %llx PID %lu\n", (unsigned long long)(uintptr_t)secret, GetCurrentProcessId()); fflush(stdout);
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE); ReadFile(in, line, sizeof(line) - 1, &n, NULL);
        return strcmp(secret, "patched by parent") ? 6 : 0;
    }
    return 100;
}
int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc >= 2 && !strncmp(argv[1], "child-", 6)) return child_mode(argv[1]);
    const char* which = argc >= 2 ? argv[1] : "all";
    char temp[MAX_PATH]; GetTempPathA(sizeof(temp), temp); snprintf(workdir, sizeof(workdir), "%snexus-wt-%d", temp, BITS);
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--peer")) snprintf(peer, sizeof(peer), "%s", argv[i + 1]);
        else if (!strcmp(argv[i], "--host")) snprintf(host, sizeof(host), "%s", argv[i + 1]);
        else if (!strcmp(argv[i], "--dir")) snprintf(workdir, sizeof(workdir), "%s", argv[i + 1]);
    }
    CreateDirectoryA(workdir, NULL);
    printf("WT BEGIN bits=%d test=%s\n", BITS, which);
    struct { const char* name; void (*run)(void); } tests[] = {{"env", test_env}, {"time", test_time}, {"memory", test_memory}, {"threads", test_threads},
        {"seh", test_seh}, {"files", test_files}, {"registry", test_registry}, {"process", test_process}, {"bitness", test_bitness}, {"winsock", test_winsock}};
    for (size_t i = 0; i < sizeof(tests) / sizeof(*tests); i++)
        if (!strcmp(which, "all") || !strcmp(which, tests[i].name)) { printf("WT GROUP %s\n", tests[i].name); tests[i].run(); }
    RemoveDirectoryA(workdir);
    printf("WT SUMMARY bits=%d pass=%d fail=%d\n", BITS, passes, failures);
    return failures;
}
