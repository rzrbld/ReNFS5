/*
 * kernel32.c - win32hle KERNEL32 core on libc / POSIX: process, heap and
 * virtual memory, time, errors, lstr*, synchronisation (critical sections,
 * events, mutexes, waits, Interlocked*), thread-local storage, and the
 * console/serial/process calls a game links but has no use for on a host.
 * See win32hle.h for the shim ABI (A32/APTR/ASTR in, RET/RETP/RETV out).
 *
 * Guest threads run one at a time (the machine lock), so the guest-side state
 * here (a CRITICAL_SECTION's fields, an event's flag) needs no lock of its own:
 * a wait gives the machine up (hle_block_begin/end) and looks again.
 */
#define _GNU_SOURCE   /* clock_gettime, usleep, MAP_ANONYMOUS */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include "win32hle.h"
#ifdef __APPLE__
#include <mach-o/dyld.h>   /* _NSGetExecutablePath */
#endif

#define HEAP_ZERO_MEMORY            0x00000008u
#define HEAP_REALLOC_IN_PLACE_ONLY  0x00000010u
#define ERROR_INVALID_HANDLE        6u
#define ERROR_NOT_ENOUGH_MEMORY     8u
#define ERROR_INVALID_PARAMETER    87u
#define ERROR_CALL_NOT_IMPLEMENTED 120u
#define ERROR_ALREADY_EXISTS      183u
#define WAIT_OBJECT_0               0u
#define WAIT_TIMEOUT              258u
#define WAIT_FAILED       0xFFFFFFFFu
#define INFINITE          0xFFFFFFFFu

/* --- error --- */
static __thread uint32_t g_last_error;
void hle_set_last_error(uint32_t code) { g_last_error = code; }
uint32_t hle_get_last_error(void) { return g_last_error; }
static void k_GetLastError(void)      { RET(g_last_error, 0); }
static void k_SetLastError(void)      { g_last_error = A32(0); RETV(1); }
static void k_OutputDebugStringA(void){ fprintf(stderr, "[guest] %s", ASTR(0) ? ASTR(0) : ""); RETV(1); }

/* --- process --- */
int hle_exe_path(char *out, size_t n) {
#ifdef __APPLE__
    char raw[4096], real[4096];
    uint32_t len = sizeof raw;
    if (!n || _NSGetExecutablePath(raw, &len) != 0 || !realpath(raw, real) || strlen(real) >= n) return 0;
    memcpy(out, real, strlen(real) + 1);
    return 1;
#else
    ssize_t len = n ? readlink("/proc/self/exe", out, n - 1) : -1;
    if (len <= 0) return 0;
    out[len] = 0;
    return 1;
#endif
}
static const char g_default_cmdline[] = "game.exe";
static const char *g_cmdline = g_default_cmdline;
void hle_set_command_line(const char *cmdline) { g_cmdline = cmdline; }

/* A host may want to close a recording or a window before the process goes. */
void (*hle_on_exit)(int code);
void hle_exit(int code) {
    static volatile int once;
    if (__sync_lock_test_and_set(&once, 1)) _exit(code);
    if (hle_on_exit) hle_on_exit(code);
    fflush(stdout);
    fflush(stderr);
    _exit(code);
}

static void k_GetProcessHeap(void)      { RET(0x00000001u, 0); }        /* every heap is the one heap */
/* The guest keeps the pointer, so it gets a copy in its own memory, made once
 * per command line. */
static void k_GetCommandLineA(void) {
    static const char *copied_from;
    static uint32_t va;
    if (copied_from != g_cmdline) va = hle_guest_strdup(g_cmdline), copied_from = g_cmdline;
    RET(va, 0);
}
static void k_GetCurrentProcess(void)   { RET(0xFFFFFFFFu, 0); }        /* pseudo-handle */
static void k_GetCurrentProcessId(void) { RET((uint32_t)getpid(), 0); }
static void k_ExitProcess(void)         { fprintf(stderr, "[win32hle] ExitProcess(%u)\n", A32(0)); hle_exit((int)A32(0)); }
static void k_TerminateProcess(void) {
    if (A32(0) == 0xFFFFFFFFu) { fprintf(stderr, "[win32hle] TerminateProcess(self, %u)\n", A32(1)); hle_exit((int)A32(1)); }
    g_last_error = ERROR_INVALID_HANDLE;
    RET(0, 2);
}
static void k_FatalAppExitA(void) { fprintf(stderr, "[win32hle] FatalAppExitA: %s\n", ASTR(1) ? ASTR(1) : ""); hle_exit(1); }
static void k_CreateProcessA(void) {
    fprintf(stderr, "[win32hle] CreateProcessA(%s, %s): not on this host\n", ASTR(0) ? ASTR(0) : "", ASTR(1) ? ASTR(1) : "");
    g_last_error = 2;
    RET(0, 10);
}
static void k_MulDiv(void) {                            /* (a, b, c): a*b/c rounded, -1 on c == 0 */
    int64_t a = (int32_t)A32(0), b = (int32_t)A32(1), c = (int32_t)A32(2);
    if (!c) RET(0xFFFFFFFFu, 3);
    int64_t p = a * b, q = (p >= 0) == (c >= 0) ? (p + (c >= 0 ? c : -c) / 2) / c : (p - (c >= 0 ? c : -c) / 2) / c;
    RET((uint32_t)(int32_t)q, 3);
}
/* FormatMessageA: a message for an error number, which is all a game asks. */
static void k_FormatMessageA(void) {                     /* (flags, source, id, lang, buf, size, args) */
    char text[64];
    snprintf(text, sizeof text, "Error %u.\r\n", A32(2));
    uint32_t n = (uint32_t)strlen(text);
    if (A32(0) & 0x100u) {                               /* FORMAT_MESSAGE_ALLOCATE_BUFFER */
        char *p = (char *)hle_alloc(n + 1);
        memcpy(p, text, n + 1);
        if (A32(4)) MEM32(A32(4)) = hle_va(p);
        RET(n, 7);
    }
    if (!A32(4) || A32(5) <= n) RET(0, 7);
    memcpy(APTR(4), text, n + 1);
    RET(n, 7);
}
static void k_GetExitCodeProcess(void) { if (A32(1)) MEM32(A32(1)) = 0; RET(1, 2); }

/* Thread ids: small and stable per host thread (pthread_self is a pointer). */
static __thread uint32_t g_tid;
static volatile uint32_t g_next_tid = 0x100;
uint32_t hle_thread_id_new(void) { return __sync_add_and_fetch(&g_next_tid, 4); }
void hle_thread_id_adopt(uint32_t tid) { g_tid = tid; }
uint32_t hle_thread_id(void) {
    if (!g_tid) g_tid = hle_thread_id_new();
    return g_tid;
}
static void k_GetCurrentThreadId(void) { RET(hle_thread_id(), 0); }

/* GetVersion: Windows XP (5.1, build 2600, NT). LOWORD = major|minor<<8,
 * HIWORD = build (NT clears the top bit). */
/* The Windows version this host claims. Default: Windows 2000 (NT). A title
 * that deliberately refuses NT (NFS5's renderer selection: "You have DirectX
 * 0" when dwPlatformId==VER_PLATFORM_WIN32_NT) is hosted as Win9x by asking
 * for it: hle_set_os_version(4,10,1998,1), or the NFS5_WIN9X=1 / HLE_WIN9X=1
 * environment variable, which the desktop host honours at first use. */
static uint32_t g_os_major = 5, g_os_minor = 1, g_os_build = 2600, g_os_platform = 2;
void hle_set_os_version(uint32_t major, uint32_t minor, uint32_t build, uint32_t platform) {
    g_os_major = major, g_os_minor = minor, g_os_build = build, g_os_platform = platform;
}
static void os_version_init(void) {
    static int done;
    if (done) return;
    done = 1;
    if (getenv("NFS5_WIN9X") || getenv("HLE_WIN9X"))
        g_os_major = 4, g_os_minor = 10, g_os_build = 1998, g_os_platform = 1;   /* Win98 */
}
static void k_GetVersion(void) {
    os_version_init();
    RET(g_os_major | (g_os_minor << 8) | (g_os_build << 16), 0);
}
static void k_IsDebuggerPresent(void) { RET(0, 0); }      /* no debugger under the recomp host */
static void k_CheckRemoteDebuggerPresent(void) { if (A32(1)) MEM32(A32(1)) = 0; RET(1, 2); }
static void k_GetVersionExA(void) {                      /* OSVERSIONINFOA: size, major, minor, build, platform, csd[128] */
    uint32_t v = A32(0);
    if (!v) RET(0, 1);
    os_version_init();
    MEM32(v + 4) = g_os_major, MEM32(v + 8) = g_os_minor, MEM32(v + 12) = g_os_build, MEM32(v + 16) = g_os_platform;
    snprintf((char *)hle_ptr(v + 20), 128, "%s", g_os_platform == 1 ? " A" : "Service Pack 3");
    RET(1, 1);
}

/* --- heap ---
 * One heap (guest_mem.c). Each block carries its size in front (HeapSize and
 * _msize answer from it, and a Local/Global block is the same kind of block). */
#define blk_alloc   hle_heap_alloc
#define blk_free    hle_heap_free
#define blk_realloc hle_heap_realloc
#define blk_ok      hle_heap_ok
#define blk_size    hle_heap_size

static void k_HeapCreate(void)  { RET(0x00000001u, 3); }
static void k_HeapDestroy(void) { RET(1, 1); }
static void k_HeapAlloc(void) {                          /* (hHeap, flags, size) */
    void *p = blk_alloc(A32(2), (A32(1) & HEAP_ZERO_MEMORY) != 0);
    if (!p) g_last_error = ERROR_NOT_ENOUGH_MEMORY;
    if (getenv("HLE_FILETRACE") && A32(2) >= 0x80000u)
        fprintf(stderr, "[mem] HeapAlloc(%u) -> %s from %#x\n", A32(2), p ? "ok" : "FAIL", g_cur_func);
    RETP(p, 3);
}
static void k_HeapFree(void) { blk_free(APTR(2)); RET(1, 3); }   /* (hHeap, flags, ptr) */
static void k_HeapReAlloc(void) {                        /* (hHeap, flags, ptr, size) */
    uint32_t fl = A32(1), n = A32(3);
    void *u = APTR(2);
    if (fl & HEAP_REALLOC_IN_PLACE_ONLY) {
        if (!blk_ok(u) || n > blk_size(u)) { g_last_error = ERROR_NOT_ENOUGH_MEMORY; RET(0, 4); }
        hle_heap_resize(u, n);
        RETP(u, 4);
    }
    RETP(blk_realloc(u, n, (fl & HEAP_ZERO_MEMORY) != 0), 4);
}
static void k_HeapSize(void)   { void *u = APTR(2); RET(blk_ok(u) ? blk_size(u) : 0xFFFFFFFFu, 3); }
static void k_HeapValidate(void) { RET(1, 3); }
static void k_LocalAlloc(void) { RETP(blk_alloc(A32(1), (A32(0) & 0x40u) != 0), 2); }
static void k_LocalFree(void)  { blk_free(APTR(0)); RET(0, 1); }
static void k_LocalReAlloc(void) { RETP(blk_realloc(APTR(0), A32(1), (A32(2) & 0x40u) != 0), 3); }
static void k_LocalLock(void)  { RETP(APTR(0), 1); }
static void k_LocalUnlock(void){ RET(1, 1); }
static void k_LocalSize(void)  { RET(blk_size(APTR(0)), 1); }
static void k_GlobalAlloc(void) { RETP(blk_alloc(A32(1), (A32(0) & 0x40u) != 0), 2); }
static void k_GlobalFree(void)  { blk_free(APTR(0)); RET(0, 1); }
static void k_GlobalReAlloc(void){ RETP(blk_realloc(APTR(0), A32(1), (A32(2) & 0x40u) != 0), 3); }
static void k_GlobalLock(void)  { RETP(APTR(0), 1); }        /* GMEM_FIXED: the handle is the pointer */
static void k_GlobalUnlock(void){ RET(1, 1); }
static void k_GlobalSize(void)  { RET(blk_size(APTR(0)), 1); }
static void k_GlobalHandle(void){ RET(A32(0), 1); }

/* --- virtual memory ---
 * A reservation is an mmap of the whole range, read-write (pages cost nothing
 * until touched); commit within it hands back the address; decommit drops the
 * pages (they read as zero when committed again); release unmaps the
 * reservation, whose size is remembered here. */
#define MEM_COMMIT   0x1000u
#define MEM_RESERVE  0x2000u
#define MEM_DECOMMIT 0x4000u
#define MEM_RELEASE  0x8000u
#define MAX_REGIONS 1024
static struct { uint32_t base, size; } g_region[MAX_REGIONS];

static int region_of(uint32_t a) {
    for (int i = 0; i < MAX_REGIONS; i++)
        if (g_region[i].size && a >= g_region[i].base && a - g_region[i].base < g_region[i].size) return i;
    return -1;
}

static void k_VirtualAlloc(void) {                       /* (addr, size, type, protect) */
    uint32_t addr = A32(0), size = (A32(1) + 0xFFFu) & ~0xFFFu, type = A32(2);
    if (addr && region_of(addr) >= 0) {                  /* commit inside a reservation */
        RET(addr & ~0xFFFu, 4);
    }
    if (!size) { g_last_error = ERROR_INVALID_PARAMETER; RET(0, 4); }
    uint32_t p = hle_vm_reserve(addr, size);
    if (!p) { g_last_error = ERROR_NOT_ENOUGH_MEMORY; RET(0, 4); }
    for (int i = 0; i < MAX_REGIONS; i++)
        if (!g_region[i].size) { g_region[i].base = p, g_region[i].size = size; break; }
    (void)type;
    RET(p, 4);
}
static void k_VirtualFree(void) {                        /* (addr, size, type) */
    uint32_t addr = A32(0), size = A32(1), type = A32(2);
    int i = region_of(addr);
    if (i < 0) { g_last_error = ERROR_INVALID_PARAMETER; RET(0, 3); }
    if (type & MEM_RELEASE) {
        hle_vm_release(g_region[i].base, g_region[i].size);
        g_region[i].size = 0;
    } else if (type & MEM_DECOMMIT) {
        uint32_t a = addr & ~0xFFFu, end = size ? (addr + size + 0xFFFu) & ~0xFFFu : g_region[i].base + g_region[i].size;
        hle_vm_zero(a, end - a);
    }
    RET(1, 3);
}
static void k_VirtualProtect(void) { if (A32(3)) MEM32(A32(3)) = 0x04u; RET(1, 4); }   /* PAGE_READWRITE before */
static void k_VirtualQuery(void) {                       /* (addr, &MEMORY_BASIC_INFORMATION, len) */
    uint32_t a = A32(0), m = A32(1);
    int i = region_of(a);
    if (!m || A32(2) < 28) RET(0, 3);
    MEM32(m) = a & ~0xFFFu;
    MEM32(m + 4) = i >= 0 ? g_region[i].base : a & ~0xFFFFu;
    MEM32(m + 8) = 0x04u;
    MEM32(m + 12) = i >= 0 ? g_region[i].base + g_region[i].size - (a & ~0xFFFu) : 0x1000u;
    MEM32(m + 16) = MEM_COMMIT, MEM32(m + 20) = 0x04u, MEM32(m + 24) = 0x20000u;
    RET(28, 3);
}
/* Bad pointers: nothing below 64 KB is mapped on either OS; above it the
 * host would have to probe, and a game asks only to be careful. */
static void k_IsBadReadPtr(void)  { RET(A32(0) < 0x10000u && A32(1) ? 1u : 0u, 2); }
static void k_IsBadWritePtr(void) { RET(A32(0) < 0x10000u && A32(1) ? 1u : 0u, 2); }
static void k_IsBadCodePtr(void)  { RET(A32(0) < 0x10000u ? 1u : 0u, 1); }
static void k_FlushInstructionCache(void) { RET(1, 3); }
/* The CRT asks which instruction sets it may use; the lifted code is the
 * same either way, so the oldest paths. */
static void k_IsProcessorFeaturePresent(void) { RET(0, 1); }

/* --- time --- */
static uint32_t ticks_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}
uint32_t hle_ticks_ms(void) { return ticks_ms(); }
static void k_GetTickCount(void) { RET(ticks_ms(), 0); }
static void k_QueryPerformanceFrequency(void) { if (A32(0)) MEM32(A32(0)) = 1000000u, MEM32(A32(0) + 4) = 0; RET(1, 1); }
static void k_QueryPerformanceCounter(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t us = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
    if (A32(0)) MEM32(A32(0)) = (uint32_t)us, MEM32(A32(0) + 4) = (uint32_t)(us >> 32);
    RET(1, 1);
}
/* Sleep gives the machine up for the wait, so another guest thread (a mixer,
 * a timer callback) runs meanwhile. Sleep(0) is a yield. */
static void k_Sleep(void) {
    uint32_t ms = A32(0);
    if (getenv("HLE_FILETRACE") && ms >= 5)
        fprintf(stderr, "[wait] t=%u Sleep(%u) from %#x\n", hle_ticks_ms(), ms, g_cur_func);
    hle_block_begin();
    if (ms) usleep(ms * 1000u); else sched_yield();
    hle_block_end();
    RETV(1);
}
static void k_SleepEx(void) { uint32_t ms = A32(0); hle_block_begin(); if (ms) usleep(ms * 1000u); else sched_yield(); hle_block_end(); RET(0, 2); }

/* --- strings (KERNEL32's lstr*; the CRT has the rest) --- */
static void k_lstrlenA(void) { char *s = ASTR(0); RET(s ? (uint32_t)strlen(s) : 0u, 1); }
static void k_lstrlenW(void) { const uint16_t *s = (const uint16_t *)APTR(0); uint32_t n = 0; if (s) while (s[n]) n++; RET(n, 1); }
static void k_lstrcpyA(void) { char *d = ASTR(0), *s = ASTR(1); if (d && s) memmove(d, s, strlen(s) + 1); RETP(d, 2); }
static void k_lstrcpynA(void) { char *d = ASTR(0), *s = ASTR(1); int n = (int)A32(2); if (d && s && n > 0) { strncpy(d, s, (size_t)n - 1); d[n - 1] = 0; } RETP(d, 3); }
static void k_lstrcatA(void) { char *d = ASTR(0), *s = ASTR(1); if (d && s) strcat(d, s); RETP(d, 2); }
static void k_lstrcmpA(void) { RET((uint32_t)strcmp(ASTR(0) ? ASTR(0) : "", ASTR(1) ? ASTR(1) : ""), 2); }
static void k_lstrcmpiA(void) { RET((uint32_t)strcasecmp(ASTR(0) ? ASTR(0) : "", ASTR(1) ? ASTR(1) : ""), 2); }

/* STARTUPINFOA is 68 bytes; the CRT reads it early. */
static void k_GetStartupInfoA(void) { uint32_t si = A32(0); if (si) { memset(hle_ptr(si), 0, 68); MEM32(si) = 68; } RETV(1); }

/* --- critical sections ---
 * CRITICAL_SECTION: DebugInfo, LockCount, RecursionCount, OwningThread,
 * LockSemaphore, SpinCount. The guest's own struct holds the state. */
#define CS_REC(cs)   MEM32((cs) + 8)
#define CS_OWNER(cs) MEM32((cs) + 12)
static void k_InitializeCriticalSection(void) { uint32_t cs = A32(0); if (cs) memset(hle_ptr(cs), 0, 24), MEM32(cs + 4) = 0xFFFFFFFFu; RETV(1); }
static void k_DeleteCriticalSection(void) { RETV(1); }
/* Ownership is an atomic compare-and-swap on OwningThread, with acquire /
 * release ordering, so guest threads running truly in parallel (parallel
 * mode, no machine lock around these calls) and the serialised mode share one
 * protocol. RecursionCount/LockCount are only touched by the owner. */
static inline uint32_t *cs_owner_ptr(uint32_t cs) { return (uint32_t *)hle_ptr(cs + 12); }
void hle_enter_cs(uint32_t cs) {
    uint32_t me = hle_thread_id();
    uint32_t *own = cs_owner_ptr(cs);
    if (__atomic_load_n(own, __ATOMIC_ACQUIRE) != me) {
        for (int spin = 0;; spin++) {
            uint32_t free_ = 0;
            if (__atomic_compare_exchange_n(own, &free_, me, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                break;
            if (spin < 16) { sched_yield(); continue; }
            hle_block_begin();
            usleep(50);
            hle_block_end();
        }
        CS_REC(cs) = 0;
    }
    CS_REC(cs)++;
    MEM32(cs + 4)++;
}
void hle_leave_cs(uint32_t cs) {
    uint32_t *own = cs_owner_ptr(cs);
    if (__atomic_load_n(own, __ATOMIC_RELAXED) != hle_thread_id() || !CS_REC(cs)) return;
    MEM32(cs + 4)--;
    if (--CS_REC(cs) == 0) __atomic_store_n(own, 0, __ATOMIC_RELEASE);
}
static void k_EnterCriticalSection(void) { hle_enter_cs(A32(0)); RETV(1); }
static void k_LeaveCriticalSection(void) { hle_leave_cs(A32(0)); RETV(1); }
static void k_TryEnterCriticalSection(void) {
    uint32_t cs = A32(0), me = hle_thread_id();
    uint32_t *own = cs_owner_ptr(cs), free_ = 0;
    if (__atomic_load_n(own, __ATOMIC_ACQUIRE) != me) {
        if (!__atomic_compare_exchange_n(own, &free_, me, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            RET(0, 1);
        CS_REC(cs) = 0;
    }
    CS_REC(cs)++, MEM32(cs + 4)++;
    RET(1, 1);
}

/* --- Interlocked (on guest memory) --- */
static void k_InterlockedIncrement(void) { RET(__sync_add_and_fetch((volatile uint32_t *)APTR(0), 1u), 1); }
static void k_InterlockedDecrement(void) { RET(__sync_sub_and_fetch((volatile uint32_t *)APTR(0), 1u), 1); }
static void k_InterlockedExchange(void)  { RET(__sync_lock_test_and_set((volatile uint32_t *)APTR(0), A32(1)), 2); }
static void k_InterlockedExchangeAdd(void) { RET(__sync_fetch_and_add((volatile uint32_t *)APTR(0), A32(1)), 2); }
static void k_InterlockedCompareExchange(void) { RET(__sync_val_compare_and_swap((volatile uint32_t *)APTR(0), A32(2), A32(1)), 3); }

/* --- events, mutexes, semaphores, waits --- */
typedef struct { int manual, signaled; } hevent;
typedef struct { uint32_t owner, count; } hmutex;
typedef struct { int32_t count, max; } hsem;
/* Named objects: one namespace for all types, as on Windows (and Wine).
 * An entry holds the object itself, not a handle: every Create/Open of the
 * name gets its own handle, and the object goes when its last handle closes
 * (the entry is then stale and dropped on the next lookup). */
typedef struct { char name[128]; int type; void *obj; } named_t;
static named_t g_named[256];

void *hle_named_lookup(const char *name, int *type) {
    if (!name || !*name) return NULL;
    for (int i = 0; i < 256; i++)
        if (g_named[i].obj && !strcmp(g_named[i].name, name)) {
            if (!hle_handle_obj_alive(g_named[i].obj)) { g_named[i].obj = NULL; continue; }
            if (type) *type = g_named[i].type;
            return g_named[i].obj;
        }
    return NULL;
}
void hle_named_add(const char *name, int type, void *obj) {
    if (!name || !*name || !obj) return;
    for (int i = 0; i < 256; i++)
        if (!g_named[i].obj || !hle_handle_obj_alive(g_named[i].obj)) {
            snprintf(g_named[i].name, sizeof g_named[i].name, "%s", name);
            g_named[i].type = type, g_named[i].obj = obj;
            return;
        }
}
uint32_t hle_named_open(const char *name, int type) {
    int t = 0;
    void *obj = hle_named_lookup(name, &t);
    if (!obj) { g_last_error = 2; return 0; }            /* ERROR_FILE_NOT_FOUND */
    if (t != type) { g_last_error = 6; return 0; }       /* ERROR_INVALID_HANDLE: a name of another type */
    uint32_t h = hle_handle_alloc(type, obj);
    g_last_error = h ? 0 : 4;                            /* ERROR_TOO_MANY_OPEN_FILES */
    return h;
}
/* Create* of a name that exists: a new handle to that object and
 * ERROR_ALREADY_EXISTS; of another type: ERROR_INVALID_HANDLE. 0 = new name. */
static int named_existing(const char *name, int type, uint32_t *h) {
    int t = 0;
    void *obj = hle_named_lookup(name, &t);
    if (!obj) return 0;
    if (t != type) { g_last_error = 6; *h = 0; return 1; }
    *h = hle_handle_alloc(type, obj);
    g_last_error = ERROR_ALREADY_EXISTS;
    return 1;
}
static void free_obj(void *o) { free(o); }

static void k_CreateEventA(void) {                       /* (sa, manual, initial, name) */
    static int closer;
    if (!closer) hle_handle_set_closer(HLE_H_EVENT, free_obj), closer = 1;
    uint32_t h;
    if (named_existing(ASTR(3), HLE_H_EVENT, &h)) RET(h, 4);
    hevent *e = (hevent *)calloc(1, sizeof *e);
    e->manual = A32(1) != 0, e->signaled = A32(2) != 0;
    h = hle_handle_alloc(HLE_H_EVENT, e);
    hle_named_add(ASTR(3), HLE_H_EVENT, e);
    g_last_error = 0;
    if (getenv("HLE_FILETRACE")) fprintf(stderr, "[ev] CreateEventA h=%#x manual=%d init=%d tid=%u from %#x\n",
                                        h, e->manual, e->signaled, hle_thread_id(), g_cur_func);
    RET(h, 4);
}
void hle_set_event(uint32_t h) { hevent *e = (hevent *)hle_handle_obj(h, HLE_H_EVENT); if (e) e->signaled = 1; }
static void k_OpenEventA(void) { RET(hle_named_open(ASTR(2), HLE_H_EVENT), 3); }   /* (access, inherit, name) */
static void k_SetEvent(void)   { hevent *e = (hevent *)hle_handle_obj(A32(0), HLE_H_EVENT);
    if (getenv("HLE_FILETRACE")) fprintf(stderr, "[ev] SetEvent h=%#x tid=%u from %#x\n", A32(0), hle_thread_id(), g_cur_func);
    if (e) e->signaled = 1; RET(e ? 1u : 0u, 1); }
static void k_ResetEvent(void) { hevent *e = (hevent *)hle_handle_obj(A32(0), HLE_H_EVENT); if (e) e->signaled = 0; RET(e ? 1u : 0u, 1); }
static void k_PulseEvent(void) { hevent *e = (hevent *)hle_handle_obj(A32(0), HLE_H_EVENT); if (e) e->signaled = 0; RET(e ? 1u : 0u, 1); }

static void k_CreateMutexA(void) {                       /* (sa, initialOwner, name) */
    static int closer;
    if (!closer) hle_handle_set_closer(HLE_H_MUTEX, free_obj), closer = 1;
    uint32_t h;
    if (named_existing(ASTR(2), HLE_H_MUTEX, &h)) RET(h, 3);   /* existing: initialOwner is ignored, as on Windows */
    hmutex *m = (hmutex *)calloc(1, sizeof *m);
    if (A32(1)) m->owner = hle_thread_id(), m->count = 1;
    h = hle_handle_alloc(HLE_H_MUTEX, m);
    hle_named_add(ASTR(2), HLE_H_MUTEX, m);
    g_last_error = 0;
    RET(h, 3);
}
static void k_OpenMutexA(void) { RET(hle_named_open(ASTR(2), HLE_H_MUTEX), 3); }   /* (access, inherit, name) */
static void k_ReleaseMutex(void) {
    hmutex *m = (hmutex *)hle_handle_obj(A32(0), HLE_H_MUTEX);
    if (!m || m->owner != hle_thread_id()) { g_last_error = 288u; RET(0, 1); }   /* ERROR_NOT_OWNER */
    if (--m->count == 0) m->owner = 0;
    RET(1, 1);
}
static void k_CreateSemaphoreA(void) {                   /* (sa, initial, max, name) */
    static int closer;
    if (!closer) hle_handle_set_closer(HLE_H_SEMAPHORE, free_obj), closer = 1;
    uint32_t h;
    if (named_existing(ASTR(3), HLE_H_SEMAPHORE, &h)) RET(h, 4);
    hsem *s = (hsem *)calloc(1, sizeof *s);
    s->count = (int32_t)A32(1), s->max = (int32_t)A32(2);
    h = hle_handle_alloc(HLE_H_SEMAPHORE, s);
    hle_named_add(ASTR(3), HLE_H_SEMAPHORE, s);
    g_last_error = 0;
    RET(h, 4);
}
static void k_ReleaseSemaphore(void) {
    hsem *s = (hsem *)hle_handle_obj(A32(0), HLE_H_SEMAPHORE);
    if (!s) RET(0, 3);
    if (A32(2)) MEM32(A32(2)) = (uint32_t)s->count;
    s->count += (int32_t)A32(1);
    if (s->count > s->max) s->count = s->max;
    RET(1, 3);
}

/* Try to take one object: 1 if it is signalled (and consumed, as its type
 * says), 0 if not, -1 if it is not a waitable handle. */
int (*hle_wait_hook)(uint32_t h);        /* a host's own objects (threads, sockets) */
static int try_take(uint32_t h) {
    if (hle_wait_hook) { int r = hle_wait_hook(h); if (r >= 0) return r; }
    switch (hle_handle_type(h)) {
    case HLE_H_EVENT: {
        hevent *e = (hevent *)hle_handle_obj(h, HLE_H_EVENT);
        if (!e->signaled) return 0;
        if (!e->manual) e->signaled = 0;
        return 1;
    }
    case HLE_H_MUTEX: {
        hmutex *m = (hmutex *)hle_handle_obj(h, HLE_H_MUTEX);
        if (m->owner && m->owner != hle_thread_id()) return 0;
        m->owner = hle_thread_id(), m->count++;
        return 1;
    }
    case HLE_H_SEMAPHORE: {
        hsem *s = (hsem *)hle_handle_obj(h, HLE_H_SEMAPHORE);
        if (s->count <= 0) return 0;
        s->count--;
        return 1;
    }
    case HLE_H_THREAD:
        return hle_thread_done(h);                       /* kernel32_crt.c */
    case HLE_H_OTHER:
        return 0;                                        /* a duplicated process pseudo-handle: not signalled */
    default:
        return h == 0xFFFFFFFFu || h == 0xFFFFFFFEu ? 0 : -1;   /* this process/thread: never */
    }
}

extern void nfs5_acct_enter(int) __attribute__((weak));
extern void nfs5_acct_leave(int) __attribute__((weak));
#define NFS5_ACCT_WAIT 8
static uint32_t hle_wait_body(int n, const uint32_t *h, int all, uint32_t timeout) {
    uint32_t start = ticks_ms();
    /* Throttle device polling while spinning: on device one pump() can cost
     * milliseconds (SDL controller/event work), and this inner loop runs every
     * 200 us while the front end waits for its frame event. */
    static uint32_t last_pump;
    int traced = getenv("NFS5_WAITTRACE") != NULL;
    if (traced) {
        static int trace_n;
        if (trace_n++ < 40) {
            fprintf(stderr, "[wait] n=%d all=%d timeout=%u from %#x types:", n, all, timeout, g_cur_func);
            for (int i = 0; i < n && i < 4; i++) fprintf(stderr, " %#x/%d", h[i], hle_handle_type(h[i]));
            fprintf(stderr, "\n");
        }
    }
    for (;;) {
        /* An event-driven DirectInput game blocks on its device event and only
         * polls the device after it fires; pump the devices while we are about
         * to block so keyboard/controller input reaches it. */
        uint32_t nowp = ticks_ms();
        if (hle_dinput_pump && (last_pump == 0 || nowp - last_pump >= 2)) {
            hle_dinput_pump();
            last_pump = nowp ? nowp : 1;
        }
        if (all) {
            int ready = 1;
            for (int i = 0; i < n; i++) { int r = try_take(h[i]); if (r < 0) return WAIT_FAILED; if (!r) { ready = 0; break; } }
            if (ready) return WAIT_OBJECT_0;           /* ponytail: takes as it checks; a partial take is not undone */
        } else {
            for (int i = 0; i < n; i++) {
                int r = try_take(h[i]);
                if (r < 0) { g_last_error = ERROR_INVALID_HANDLE; return WAIT_FAILED; }
                if (r) return WAIT_OBJECT_0 + (uint32_t)i;
            }
        }
        if (timeout != INFINITE && ticks_ms() - start >= timeout) return WAIT_TIMEOUT;
        hle_block_begin();
        usleep(timeout == 0 ? 0 : 200);
        hle_block_end();
        if (timeout == 0) return WAIT_TIMEOUT;
    }
}
uint32_t hle_wait(int n, const uint32_t *h, int all, uint32_t timeout) {
    if (nfs5_acct_enter) nfs5_acct_enter(NFS5_ACCT_WAIT);
    uint32_t r = hle_wait_body(n, h, all, timeout);
    if (nfs5_acct_leave) nfs5_acct_leave(NFS5_ACCT_WAIT);
    return r;
}
/* GetOverlappedResult: the completion side of the overlapped file model in
 * kernel32_ext.c. The transfer already happened; this reports its bytes and
 * status, waiting on the OVERLAPPED's event when asked. EOF is FALSE with
 * ERROR_HANDLE_EOF, as Windows reports it. */
static void k_GetOverlappedResult(void) {                /* (hFile, ovl, &bytes, wait) */
    uint32_t h = A32(0), o = A32(1), pb = A32(2);
    if (!o || !hle_handle_obj(h, HLE_H_FILE)) { g_last_error = ERROR_INVALID_HANDLE; RET(0, 4); }
    uint32_t ev = MEM32(o + 16);
    if (A32(3) && ev) { uint32_t e = ev; hle_wait(1, &e, 0, INFINITE); }
    uint32_t status = MEM32(o + 0), n = MEM32(o + 4);
    if (pb) MEM32(pb) = n;
    if (status == 38u /* ERROR_HANDLE_EOF */ || status == 995u /* ERROR_OPERATION_ABORTED */) {
        g_last_error = status;
        RET(0, 4);
    }
    if (status) { g_last_error = status; RET(0, 4); }
    g_last_error = 0;
    RET(1, 4);
}
static void k_WaitForSingleObject(void) {
    uint32_t h = A32(0), t = A32(1);
    if (getenv("HLE_FILETRACE") && t != 0xFFFFFFFFu && t >= 20)
        fprintf(stderr, "[wait] t=%u WaitForSingleObject(%#x, %u) from %#x\n", hle_ticks_ms(), h, t, g_cur_func);
    RET(hle_wait(1, &h, 0, t), 2);
}
static void k_WaitForMultipleObjectsEx(void) {           /* (n, handles, waitAll, timeout, alertable) */
    uint32_t n = A32(0), hs[64];
    if (n > 64) n = 64;
    for (uint32_t i = 0; i < n; i++) hs[i] = MEM32(A32(1) + 4 * i);
    RET(hle_wait((int)n, hs, A32(2) != 0, A32(3)), 5);
}
/* A no-op debug break: on Windows it raises a breakpoint a debugger catches,
 * without one the process dies. A game's assert path calling it should not
 * kill the port, so note it and continue (traced). */
static void k_DebugBreak(void) {
    fprintf(stderr, "[win32hle] DebugBreak from guest 0x%08X (continuing)\n", g_cur_func);
    RETV(0);
}
static void k_DisableThreadLibraryCalls(void) { RET(1, 1); }
/* Named semaphores / file mappings: no named-object namespace here, so the
 * open fails as it does when the object does not exist. */
static void k_OpenSemaphoreA(void) { RET(hle_named_open(ASTR(2), HLE_H_SEMAPHORE), 3); }    /* (access, inherit, name) */
static void k_OpenFileMappingA(void) { RET(hle_named_open(ASTR(2), HLE_H_MAPPING), 3); }
static void k_WaitForSingleObjectEx(void) { uint32_t h = A32(0); RET(hle_wait(1, &h, 0, A32(1)), 3); }
static void k_WaitForMultipleObjects(void) {             /* (n, handles, all, timeout) */
    uint32_t n = A32(0), hs[64];
    if (n > 64) n = 64;
    for (uint32_t i = 0; i < n; i++) hs[i] = MEM32(A32(1) + 4 * i);
    RET(hle_wait((int)n, hs, A32(2) != 0, A32(3)), 4);
}

/* --- thread-local storage: per host thread --- */
#define TLS_SLOTS 64
static __thread uint32_t g_tls[TLS_SLOTS];
static uint8_t g_tls_used[TLS_SLOTS];
static void k_TlsAlloc(void) {
    for (int i = 0; i < TLS_SLOTS; i++) if (!g_tls_used[i]) { g_tls_used[i] = 1; RET((uint32_t)i, 0); }
    RET(0xFFFFFFFFu, 0);
}
static void k_TlsFree(void) { uint32_t i = A32(0); if (i < TLS_SLOTS) g_tls_used[i] = 0; RET(1, 1); }
static void k_TlsGetValue(void) { uint32_t i = A32(0); g_last_error = 0; RET(i < TLS_SLOTS ? g_tls[i] : 0u, 1); }
static void k_TlsSetValue(void) { uint32_t i = A32(0); if (i < TLS_SLOTS) g_tls[i] = A32(1); RET(i < TLS_SLOTS ? 1u : 0u, 2); }

/* --- the console, serial ports, devices: a game links them, a host has none --- */
static void k_ret1_0(void) { RET(1, 0); }
static void k_ret1_1(void) { RET(1, 1); }
static void k_ret1_2(void) { RET(1, 2); }
static void k_fail_1(void) { g_last_error = ERROR_INVALID_HANDLE; RET(0, 1); }
static void k_fail_2(void) { g_last_error = ERROR_INVALID_HANDLE; RET(0, 2); }
static void k_fail_3(void) { g_last_error = ERROR_INVALID_HANDLE; RET(0, 3); }
static void k_fail_4(void) { g_last_error = ERROR_INVALID_HANDLE; RET(0, 4); }
static void k_fail_8(void) { g_last_error = ERROR_INVALID_HANDLE; RET(0, 8); }
static void k_GetConsoleMode(void) { if (A32(1)) MEM32(A32(1)) = 0; RET(0, 2); }
static void k_GetNumberOfConsoleInputEvents(void) { if (A32(1)) MEM32(A32(1)) = 0; RET(0, 2); }
static void k_WriteConsoleA(void) {                      /* (h, buf, n, &written, reserved) */
    fwrite(APTR(1), 1, A32(2), stderr);
    if (A32(3)) MEM32(A32(3)) = A32(2);
    RET(1, 5);
}

/* Atoms: MFC interns its window-class names as global atoms very early in
 * startup. A small intern table numbered from 0xC000 (the Win32 app-atom
 * range); global and local atoms share it, which is all a single guest sees. */
#define ATOM_BASE 0xC000u
static struct { char *s; } g_atoms[1024];
static int g_atom_n;
static uint32_t atom_intern(const char *s) {
    if (!s) return 0;
    for (int i = 0; i < g_atom_n; i++) if (!strcmp(g_atoms[i].s, s)) return ATOM_BASE + (uint32_t)i;
    if (g_atom_n >= 1024) return 0;
    g_atoms[g_atom_n].s = strdup(s);
    return ATOM_BASE + (uint32_t)g_atom_n++;
}
static const char *atom_name(uint32_t a) {
    uint32_t i = a - ATOM_BASE;
    return (a >= ATOM_BASE && (int)i < g_atom_n) ? g_atoms[i].s : NULL;
}
static void k_GlobalAddAtomA(void)  { RET(atom_intern(ASTR(0)), 1); }
static void k_GlobalFindAtomA(void) {
    const char *s = ASTR(0);
    if (s) for (int i = 0; i < g_atom_n; i++) if (!strcmp(g_atoms[i].s, s)) RET(ATOM_BASE + (uint32_t)i, 1);
    RET(0, 1);
}
static void k_GlobalDeleteAtom(void) { RET(0, 1); }   /* kept interned; refcount ignored */
static void k_GlobalGetAtomNameA(void) {              /* (atom, buf, size) */
    const char *n = atom_name(A32(0)); char *buf = ASTR(1); uint32_t sz = A32(2);
    if (buf && sz) { if (n) { strncpy(buf, n, sz - 1); buf[sz - 1] = 0; } else buf[0] = 0; }
    RET(n && buf ? (uint32_t)strlen(buf) : 0u, 3);
}

const win32hle_shim win32hle_kernel32[] = {
    { "GlobalAddAtomA",     k_GlobalAddAtomA },
    { "GlobalFindAtomA",    k_GlobalFindAtomA },
    { "GlobalDeleteAtom",   k_GlobalDeleteAtom },
    { "GlobalGetAtomNameA", k_GlobalGetAtomNameA },
    { "GetProcessHeap",     k_GetProcessHeap },
    { "GetCommandLineA",    k_GetCommandLineA },
    { "GetCurrentProcess",  k_GetCurrentProcess },
    { "GetCurrentProcessId",k_GetCurrentProcessId },
    { "GetCurrentThreadId", k_GetCurrentThreadId },
    { "ExitProcess",        k_ExitProcess },
    { "TerminateProcess",   k_TerminateProcess },
    { "FatalAppExitA",      k_FatalAppExitA },
    { "CreateProcessA",     k_CreateProcessA },
    { "GetExitCodeProcess", k_GetExitCodeProcess },
    { "MulDiv",             k_MulDiv },
    { "FormatMessageA",     k_FormatMessageA },
    { "OpenEventA",         k_OpenEventA },
    { "GetVersion",         k_GetVersion },
    { "IsDebuggerPresent",  k_IsDebuggerPresent }, { "CheckRemoteDebuggerPresent", k_CheckRemoteDebuggerPresent },
    { "GetVersionExA",      k_GetVersionExA },
    { "HeapCreate",         k_HeapCreate },
    { "HeapDestroy",        k_HeapDestroy },
    { "HeapAlloc",          k_HeapAlloc },
    { "HeapFree",           k_HeapFree },
    { "HeapReAlloc",        k_HeapReAlloc },
    { "HeapSize",           k_HeapSize },
    { "HeapValidate",       k_HeapValidate },
    { "LocalAlloc",         k_LocalAlloc },
    { "LocalFree",          k_LocalFree },
    { "LocalReAlloc",       k_LocalReAlloc },
    { "LocalLock",          k_LocalLock },
    { "LocalUnlock",        k_LocalUnlock },
    { "LocalSize",          k_LocalSize },
    { "GlobalAlloc",        k_GlobalAlloc },
    { "GlobalFree",         k_GlobalFree },
    { "GlobalReAlloc",      k_GlobalReAlloc },
    { "GlobalLock",         k_GlobalLock },
    { "GlobalUnlock",       k_GlobalUnlock },
    { "GlobalSize",         k_GlobalSize },
    { "GlobalHandle",       k_GlobalHandle },
    { "VirtualAlloc",       k_VirtualAlloc },
    { "VirtualFree",        k_VirtualFree },
    { "VirtualProtect",     k_VirtualProtect },
    { "VirtualQuery",       k_VirtualQuery },
    { "IsBadReadPtr",       k_IsBadReadPtr },
    { "IsBadWritePtr",      k_IsBadWritePtr },
    { "IsBadCodePtr",       k_IsBadCodePtr },
    { "FlushInstructionCache", k_FlushInstructionCache },
    { "IsProcessorFeaturePresent", k_IsProcessorFeaturePresent },
    { "GetTickCount",       k_GetTickCount },
    { "QueryPerformanceFrequency", k_QueryPerformanceFrequency },
    { "QueryPerformanceCounter",   k_QueryPerformanceCounter },
    { "Sleep",              k_Sleep },
    { "SleepEx",            k_SleepEx },
    { "GetLastError",       k_GetLastError },
    { "SetLastError",       k_SetLastError },
    { "OutputDebugStringA", k_OutputDebugStringA },
    { "lstrlenA",           k_lstrlenA },
    { "lstrlenW",           k_lstrlenW },
    { "lstrcpyA",           k_lstrcpyA },
    { "lstrcpynA",          k_lstrcpynA },
    { "lstrcatA",           k_lstrcatA },
    { "lstrcmpA",           k_lstrcmpA },
    { "lstrcmpiA",          k_lstrcmpiA },
    { "GetStartupInfoA",    k_GetStartupInfoA },
    { "InitializeCriticalSection", k_InitializeCriticalSection },
    { "DeleteCriticalSection",     k_DeleteCriticalSection },
    { "EnterCriticalSection",      k_EnterCriticalSection },
    { "LeaveCriticalSection",      k_LeaveCriticalSection },
    { "TryEnterCriticalSection",   k_TryEnterCriticalSection },
    { "InterlockedIncrement",      k_InterlockedIncrement },
    { "InterlockedDecrement",      k_InterlockedDecrement },
    { "InterlockedExchange",       k_InterlockedExchange },
    { "InterlockedExchangeAdd",    k_InterlockedExchangeAdd },
    { "InterlockedCompareExchange",k_InterlockedCompareExchange },
    { "CreateEventA",       k_CreateEventA },
    { "SetEvent",           k_SetEvent },
    { "ResetEvent",         k_ResetEvent },
    { "PulseEvent",         k_PulseEvent },
    { "CreateMutexA",       k_CreateMutexA },
    { "OpenMutexA",         k_OpenMutexA },
    { "ReleaseMutex",       k_ReleaseMutex },
    { "CreateSemaphoreA",   k_CreateSemaphoreA },
    { "ReleaseSemaphore",   k_ReleaseSemaphore },
    { "WaitForSingleObject",   k_WaitForSingleObject },
    { "WaitForMultipleObjectsEx", k_WaitForMultipleObjectsEx },
    { "DebugBreak",            k_DebugBreak },
    { "DisableThreadLibraryCalls", k_DisableThreadLibraryCalls },
    { "OpenSemaphoreA",        k_OpenSemaphoreA },
    { "OpenFileMappingA",      k_OpenFileMappingA },
    { "WaitForSingleObjectEx", k_WaitForSingleObjectEx },
    { "WaitForMultipleObjects", k_WaitForMultipleObjects },

    { "TlsAlloc",           k_TlsAlloc },
    { "TlsFree",            k_TlsFree },
    { "TlsGetValue",        k_TlsGetValue },
    { "TlsSetValue",        k_TlsSetValue },
    { "AllocConsole",       k_ret1_0 },
    { "FreeConsole",        k_ret1_0 },
    { "GetConsoleMode",     k_GetConsoleMode },
    { "SetConsoleMode",     k_ret1_2 },
    { "SetConsoleCtrlHandler", k_ret1_2 },
    { "GetNumberOfConsoleInputEvents", k_GetNumberOfConsoleInputEvents },
    { "PeekConsoleInputA",  k_fail_4 },
    { "ReadConsoleInputA",  k_fail_4 },
    { "WriteConsoleA",      k_WriteConsoleA },
    { "SetupComm",          k_fail_3 },
    { "PurgeComm",          k_fail_2 },
    { "ClearCommBreak",     k_fail_1 },
    { "SetCommBreak",       k_fail_1 },
    { "ClearCommError",     k_fail_3 },
    { "EscapeCommFunction", k_fail_2 },
    { "GetCommMask",        k_fail_2 },
    { "GetCommModemStatus", k_fail_2 },
    { "GetCommState",       k_fail_2 },
    { "SetCommState",       k_fail_2 },
    { "SetCommTimeouts",    k_fail_2 },
    { "DeviceIoControl",    k_fail_8 },
    { "GetOverlappedResult",k_GetOverlappedResult },
    { "SetPriorityClass",   k_ret1_2 },
    { "SetErrorMode",       k_ret1_1 },
    { 0, 0 }
};
