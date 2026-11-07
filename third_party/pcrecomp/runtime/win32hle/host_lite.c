/*
 * host_lite.c - the portable spine of a win32hle host (see win32hle.h).
 *
 * Owns the recomp32 register file, the machine lock, each thread's simulated
 * TIB (g_fs_base), the dispatch lookups, the shim registry, and the explicit
 * native -> guest call. No inline asm and no OS exception handling: every
 * import is a C shim, so the guest -> native and native -> guest paths are both
 * ordinary C calls. Builds under gcc/clang -m32 and MSVC x86 alike.
 *
 * "lite" because it is the spine without a PE loader: enough to drive lifted
 * code and the shim layer (the selftest does exactly that). A full game host
 * adds image mapping and IAT binding on top; those are separate.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include "win32hle.h"

/* ------------------------------------------------------------ register file
 * recomp_types.h declares these extern; exactly one translation unit defines
 * them, and for a win32hle host that is here. (Mirrors native32.c.) */
RECOMP_TLS uint32_t  g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp;
RECOMP_TLS double    g_st[8];
RECOMP_TLS int       g_fp_top;
RECOMP_TLS uint16_t  g_fpu_cw = 0x027F;
RECOMP_TLS uint16_t  g_seg_cs, g_seg_ds, g_seg_es, g_seg_fs, g_seg_gs, g_seg_ss;
RECOMP_TLS uint64_t  g_mm[8];
RECOMP_TLS xmm_t     g_xmm[8];
RECOMP_TLS uint32_t  g_mxcsr = 0x1F80;
RECOMP_TLS uint32_t  g_fs_base, g_gs_base;
ptrdiff_t g_mem_base = 0;   /* flat: 0; based: set by hle_mem_init (guest_mem.c) */
RECOMP_TLS uint32_t  g_cur_func;
RECOMP_TLS const char *g_cur_import;
RECOMP_TLS uint32_t  g_icall_trace[ICALL_TRACE_SIZE], g_icall_from[ICALL_TRACE_SIZE];
RECOMP_TLS uint32_t  g_icall_trace_idx, g_icall_count;

int win32hle_trace;

void hle_fatal(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[win32hle] FATAL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    abort();
}

/* --------------------------------------------------------------- the lock
 * One mutex serialises the (global) register file. A thread holds it while it
 * runs lifted code; mach_enter/mach_leave nest per thread (a shim that calls
 * back into the guest nests the claim) and only the outermost pair takes and
 * gives the mutex, saving and loading the thread's registers as it does.
 *
 * A thread gives the machine to others mid-run only through hle_yield and the
 * hle_block_begin/end pair, which save its registers first. (Unlocking from a
 * nested depth without that is how another thread's registers end up in this
 * one's lifted code.) Every import is a yield point when another thread is
 * waiting (shim_entry), as every native call is under native32, and so is
 * every RECOMP_YIELD_EVERY-th loop back-edge (recomp_yield_hook). */
static pthread_mutex_t g_mach = PTHREAD_MUTEX_INITIALIZER;
static volatile int      g_waiters;     /* threads blocked in mach_take */
pthread_t nfs5_guest_owner; /* diagnostic: thread currently owning the guest machine */
static volatile unsigned g_owner_gen;   /* bumped on every take */
static __thread uint64_t g_slice_start_ns;
static uint64_t scheduler_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

/* Parallel mode (RECOMP_THREADED_CPU build + NFS5_PARALLEL=1): every guest
 * thread owns its CPU state (thread-local), so lifted code runs concurrently,
 * as on a multi-core Windows PC. The machine lock then only serialises the
 * host: it is claimed around each shim (import/COM call) and around callbacks
 * the host makes into the guest while it holds it; g_lock_depth nests those
 * claims per thread. Default mode: one lock held while any lifted code runs. */
static __thread int g_lock_depth;
static int g_parallel = -1;
int hle_parallel(void) {
    if (g_parallel < 0) {
#ifdef RECOMP_THREADED_CPU
        const char *v = getenv("NFS5_PARALLEL");
        g_parallel = v && *v && strcmp(v, "0") != 0;
#else
        g_parallel = 0;
#endif
        fprintf(stderr, "[host] guest threads: %s\n", g_parallel ? "parallel (lock per host call)" : "serialised (one machine lock)");
    }
    return g_parallel;
}
static void mach_take(void);
/* NFS5_LOCKPROF=1: who holds the machine lock in parallel mode and who waits
 * for it (per shim/host-entry label, main game thread vs others), every 2 s */
static int g_lockprof = -1;
static pthread_t g_main_thread;
__attribute__((constructor)) static void lockprof_main(void) { g_main_thread = pthread_self(); }
typedef struct { const char *label; uint64_t hold_ns[2], holds[2], wait_ns[2], waits[2]; } lockstat_t;
static lockstat_t g_lst[256];
static int g_nlst;
static pthread_mutex_t g_lst_mu = PTHREAD_MUTEX_INITIALIZER;
static __thread const char *g_lock_label;
static __thread uint64_t g_lock_t_acq;
static uint64_t lp_now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static lockstat_t *lst_for(const char *label) {
    for (int i = 0; i < g_nlst; i++) if (g_lst[i].label == label) return &g_lst[i];
    if (g_nlst >= 256) return &g_lst[255];
    g_lst[g_nlst].label = label;
    return &g_lst[g_nlst++];
}
static int lst_cmp_hold(const void *a, const void *b) {
    const lockstat_t *x = a, *y = b;
    uint64_t hx = x->hold_ns[0] + x->hold_ns[1], hy = y->hold_ns[0] + y->hold_ns[1];
    return hx < hy ? 1 : hx > hy ? -1 : 0;
}
static void lockprof_report(uint64_t now) {
    static uint64_t last;
    if (!last) { last = now; return; }
    if (now - last < 2000000000ull) return;
    double dt = (double)(now - last) / 1e9;
    last = now;
    lockstat_t copy[256];
    pthread_mutex_lock(&g_lst_mu);
    int n = g_nlst;
    memcpy(copy, g_lst, sizeof(lockstat_t) * (size_t)n);
    for (int i = 0; i < n; i++) {
        memset(g_lst[i].hold_ns, 0, sizeof g_lst[i].hold_ns); memset(g_lst[i].holds, 0, sizeof g_lst[i].holds);
        memset(g_lst[i].wait_ns, 0, sizeof g_lst[i].wait_ns); memset(g_lst[i].waits, 0, sizeof g_lst[i].waits);
    }
    pthread_mutex_unlock(&g_lst_mu);
    qsort(copy, (size_t)n, sizeof *copy, lst_cmp_hold);
    double mw = 0, ow = 0;
    for (int i = 0; i < n; i++) { mw += copy[i].wait_ns[0] / 1e6; ow += copy[i].wait_ns[1] / 1e6; }
    fprintf(stderr, "[lockprof] %.1fs: main waited %.1f ms/s, others waited %.1f ms/s\n", dt, mw / dt, ow / dt);
    for (int i = 0; i < n && i < 10; i++) {
        lockstat_t *l = &copy[i];
        fprintf(stderr, "[lockprof]   %-34s hold main %6.1f ms/s (%5llu) other %6.1f ms/s (%5llu) | wait main %5.1f other %5.1f ms/s\n",
                l->label, l->hold_ns[0] / 1e6 / dt, (unsigned long long)(l->holds[0] / dt),
                l->hold_ns[1] / 1e6 / dt, (unsigned long long)(l->holds[1] / dt),
                l->wait_ns[0] / 1e6 / dt, l->wait_ns[1] / 1e6 / dt);
    }
}
static void lock_claim_label(const char *label) {
    if (g_lockprof < 0) g_lockprof = getenv("NFS5_LOCKPROF") != NULL;
    if (g_lock_depth++ == 0) {
        uint64_t t0 = g_lockprof ? lp_now() : 0;
        mach_take();
        if (g_lockprof) {
            uint64_t t1 = lp_now();
            int who = !pthread_equal(pthread_self(), g_main_thread);
            pthread_mutex_lock(&g_lst_mu);
            lockstat_t *l = lst_for(label);
            l->wait_ns[who] += t1 - t0; l->waits[who]++;
            pthread_mutex_unlock(&g_lst_mu);
            g_lock_label = label;
            g_lock_t_acq = t1;
        }
    }
}
static void lockprof_hold_end(void) {
    if (!g_lockprof || !g_lock_label) return;
    uint64_t t = lp_now();
    int who = !pthread_equal(pthread_self(), g_main_thread);
    pthread_mutex_lock(&g_lst_mu);
    lockstat_t *l = lst_for(g_lock_label);
    l->hold_ns[who] += t - g_lock_t_acq; l->holds[who]++;
    pthread_mutex_unlock(&g_lst_mu);
    if (!who) lockprof_report(t);
}
static void lock_claim(void) { lock_claim_label("host-entry"); }
static void lock_release(void) {
    if (--g_lock_depth == 0) {
        lockprof_hold_end();
        g_lock_label = NULL;
        __atomic_store_n(&nfs5_guest_owner, 0, __ATOMIC_RELAXED);
        pthread_mutex_unlock(&g_mach);
    }
}
/* does this thread currently hold the machine lock? */
static int g_ms_depth_get(void);
static int holding(void) { return hle_parallel() ? g_lock_depth > 0 : g_ms_depth_get() > 0; }

static void mach_take(void) {
    __sync_fetch_and_add(&g_waiters, 1);
    pthread_mutex_lock(&g_mach);
    __sync_fetch_and_sub(&g_waiters, 1);
    __atomic_store_n(&nfs5_guest_owner,pthread_self(),__ATOMIC_RELAXED);
    g_owner_gen++;
    g_slice_start_ns = scheduler_now_ns();
}

typedef struct {
    uint32_t eax, ecx, edx, esp, ebx, esi, edi, ebp, fs, cur;
    double   st[8];
    int64_t  st_i64[8];
    int      fp_top;
    uint16_t fpu_cw;
    uint64_t mm[8];
} regs_t;

static void regs_save(regs_t *r) {
    r->eax=g_eax; r->ecx=g_ecx; r->edx=g_edx; r->esp=g_esp;
    r->ebx=g_ebx; r->esi=g_esi; r->edi=g_edi; r->ebp=g_ebp;
    r->fs=g_fs_base; r->cur=g_cur_func;
    memcpy(r->st_i64, g_st_i64, sizeof g_st_i64);
    memcpy(r->st, g_st, sizeof g_st); r->fp_top=g_fp_top; r->fpu_cw=g_fpu_cw;
    memcpy(r->mm, g_mm, sizeof g_mm);
}
static void regs_load(const regs_t *r) {
    g_eax=r->eax; g_ecx=r->ecx; g_edx=r->edx; g_esp=r->esp;
    g_ebx=r->ebx; g_esi=r->esi; g_edi=r->edi; g_ebp=r->ebp;
    g_fs_base=r->fs; g_cur_func=r->cur;
    memcpy(g_st_i64, r->st_i64, sizeof g_st_i64);
    memcpy(g_st, r->st, sizeof g_st); g_fp_top=r->fp_top; g_fpu_cw=r->fpu_cw;
    memcpy(g_mm, r->mm, sizeof g_mm);
}

/* A guest thread's stack and simulated TIB. Lifted code reads fs:[n] as
 * MEM32(g_fs_base + n); the host's own TLS stays separate, so a guest SEH frame
 * never lands on a chain the host walks. */
#define GUEST_STACK (1u << 20)   /* per guest thread; one-shot timer threads are many */

static uint32_t make_tib(uint32_t lo, uint32_t hi) {
    uint32_t *t = (uint32_t *)hle_alloc(0x1000);
    if (!t) hle_fatal("no guest memory for a thread block");
    t[0]        = 0xFFFFFFFFu;                 /* ExceptionList: end of chain */
    t[1]        = hi;                          /* StackBase                   */
    t[2]        = lo;                          /* StackLimit                  */
    t[0x18/4]   = hle_va(t);                   /* Self (fs:[0x18])            */
    return hle_va(t);
}

typedef struct { regs_t r; int depth, inited; uint32_t stack_lo, stack_hi, tib_va; void *tib_host; } mstate;
static __thread mstate g_ms;
static int g_ms_depth_get(void) { return g_ms.depth; }

/* Release a guest thread's stack when its host thread exits. NFS5 creates
 * hundreds of one-shot multimedia timer threads; before this, each leaked a
 * 4 MB guest stack and the guest address space (and the handheld's 1 GB)
 * filled up within seconds of the front-end starting. */
static pthread_key_t  g_stack_key;
static pthread_once_t g_stack_once = PTHREAD_ONCE_INIT;
static void guest_thread_exit(void *p) {
    (void)p;
    if (!g_ms.inited) return;
    if (g_ms.stack_lo) hle_vm_release(g_ms.stack_lo, GUEST_STACK);
    if (g_ms.tib_host) hle_heap_free(g_ms.tib_host);
    g_ms.inited = 0, g_ms.stack_lo = 0, g_ms.tib_host = 0;
}
static void stack_key_once(void) { pthread_key_create(&g_stack_key, guest_thread_exit); }

void mach_enter(void) {
    if (g_ms.depth++ == 0) {
        if (!g_ms.inited) {
            uint32_t lo = hle_vm_reserve(0, GUEST_STACK);
            if (!lo) hle_fatal("no guest memory for a thread's stack");
            g_ms.stack_lo = lo, g_ms.stack_hi = lo + GUEST_STACK;
            g_ms.r.esp    = lo + GUEST_STACK - 64;
            g_ms.r.fs     = make_tib(lo, lo + GUEST_STACK);
            g_ms.tib_va   = g_ms.r.fs;
            g_ms.tib_host = hle_ptr(g_ms.r.fs);
            g_ms.r.fpu_cw = 0x027F;
            g_ms.inited   = 1;
            pthread_once(&g_stack_once, stack_key_once);
            pthread_setspecific(g_stack_key, &g_ms);
        }
        if (!hle_parallel()) mach_take();
        regs_load(&g_ms.r);
    }
    /* parallel: host code entering the machine (timer threads signalling
     * events, callbacks) still gets exclusive access to host state */
    if (hle_parallel()) lock_claim_label("mach_enter (host thread)");
}
void mach_leave(void) {
    if (hle_parallel()) lock_release();
    if (--g_ms.depth == 0) {
        regs_save(&g_ms.r);
        if (!hle_parallel()) {
            __atomic_store_n(&nfs5_guest_owner,0,__ATOMIC_RELAXED);
            pthread_mutex_unlock(&g_mach);
        }
    }
}

/* Give the machine up around a wait (a Sleep, an event, a contended critical
 * section). The registers go with it; nothing between begin and end may touch
 * the register file or guest-shared host state. */
static __thread unsigned g_block_gen;
void hle_block_begin(void) {
    if (!holding()) return;
    if (hle_parallel()) lockprof_hold_end();
    regs_save(&g_ms.r);
    g_block_gen = g_owner_gen;
    __atomic_store_n(&nfs5_guest_owner,0,__ATOMIC_RELAXED);
    pthread_mutex_unlock(&g_mach);
}
/* The mutex is not fair: a thread that gives the machine up for a moment
 * (a wait with no timeout, polled in a loop) took it straight back, and a
 * timer callback waiting for the machine while holding a game mutex never
 * ran, so the loop polling that mutex never ended. Coming back, a thread
 * first lets a waiter have its turn. */
void hle_block_end(void) {
    if (!holding()) return;
    for (int spins = 0; g_waiters && g_owner_gen == g_block_gen && spins < 1000; spins++) sched_yield();
    mach_take();
    if (hle_parallel() && g_lockprof > 0) g_lock_t_acq = lp_now();
    regs_load(&g_ms.r);
}

/* Let a waiting thread have the machine, if there is one. The mutex is not
 * fair, so this waits until someone else has actually taken it. */
void hle_yield(void) {
    /* Movie opens originate on the stream worker while the main guest may
     * spin waiting for I/O, without polling Windows messages. */
    extern void hle_screen_movie_service(void) __attribute__((weak));
    if (hle_screen_movie_service) {
        extern int hle_screen_movie_pending(void) __attribute__((weak));
        if (hle_parallel() && !g_lock_depth) {
            if (hle_screen_movie_pending && hle_screen_movie_pending()) {
                lock_claim_label("movie service"); hle_screen_movie_service(); lock_release();
            }
        }
        else hle_screen_movie_service();
    }
    if (!g_waiters || !holding()) return;
    /* A runnable guest gets a bounded 1 ms slice. Yielding before every
     * graphics/critical-section import makes cooperating workers ping-pong
     * hundreds of times per frame. Explicit blocking calls still release the
     * machine immediately, including contended guest critical sections. */
    if (scheduler_now_ns() - g_slice_start_ns < 1000000ull) return;
    extern void nfs5_acct_enter(int) __attribute__((weak));
    extern void nfs5_acct_leave(int) __attribute__((weak));
    if (nfs5_acct_enter) nfs5_acct_enter(8); /* scheduler wait, not guest CPU */
    unsigned gen = g_owner_gen;
    hle_block_begin();
    for (int spins = 0; g_waiters && g_owner_gen == gen && spins < 1000; spins++) sched_yield();
    hle_block_end();
    if (nfs5_acct_leave) nfs5_acct_leave(8);
}

#define SHIM_BASE  0x7F000000u
#define SHIM_STEP  16u
#define MAX_SHIMS  2048

/* ------------------------------------------------------------- dispatch
 * recomp_lookup searches the generated dispatch table (declared extern in
 * recomp_types.h; defined by the lifted program, or the selftest). */
/* NFS5: every lifted indirect call (C++ vtables, COM, the THRASH dispatch
 * block, per-vertex callbacks) resolves its target here. Cache the final
 * answer of the whole chain (recovered hooks, live modules, main table) in a
 * large direct-mapped table; the chain's answers only change when a module is
 * loaded or unloaded, which flushes it (hle_lookup_flush, module.c). */
#define LOOKUP_CACHE 16384
/* Each slot is one atomic pointer to an immutable {va, fn} node, so threads
 * running lifted code in parallel never see a torn pair. Nodes are never
 * freed (bounded by the number of distinct call targets). */
typedef struct { uint32_t va; recomp_func_t fn; } lookup_node;
static lookup_node *g_lookup_cache[LOOKUP_CACHE];
void hle_lookup_flush(void) {
    for (unsigned i = 0; i < LOOKUP_CACHE; i++) __atomic_store_n(&g_lookup_cache[i], NULL, __ATOMIC_RELEASE);
}
static recomp_func_t recomp_lookup_slow(uint32_t va);
recomp_func_t recomp_lookup(uint32_t va) {
    static int off = -1;
    if (off < 0) off = getenv("NFS5_NO_LOOKUP_CACHE") != NULL;
    if (off) return recomp_lookup_slow(va);
    unsigned slot = ((va >> 2) ^ (va >> 16)) & (LOOKUP_CACHE - 1);
    lookup_node *n = __atomic_load_n(&g_lookup_cache[slot], __ATOMIC_ACQUIRE);
    if (n && n->va == va) return n->fn;
    recomp_func_t fn = recomp_lookup_slow(va);
    if (va && !(va >= SHIM_BASE && va < SHIM_BASE + SHIM_STEP * MAX_SHIMS)) {
        lookup_node *nn = (lookup_node *)malloc(sizeof *nn);
        if (nn) {
            nn->va = va;
            nn->fn = fn;
            __atomic_store_n(&g_lookup_cache[slot], nn, __ATOMIC_RELEASE);
        }
    }
    return fn;
}
static recomp_func_t recomp_lookup_slow(uint32_t va) {
    extern recomp_func_t nfs_recovered_lookup(uint32_t) __attribute__((weak));
    if(nfs_recovered_lookup) {
        recomp_func_t recovered=nfs_recovered_lookup(va);
        if(recovered)return recovered;
    }
    /* Synthetic imports cannot belong to a translated module. The caller
     * resolves these through recomp_lookup_import immediately after us. */
    if (va >= SHIM_BASE && va < SHIM_BASE + SHIM_STEP * MAX_SHIMS) return NULL;
    extern recomp_func_t nfs_module_lookup(uint32_t) __attribute__((weak));
    if (nfs_module_lookup) {
        recomp_func_t module_fn = nfs_module_lookup(va);
        if (module_fn) return module_fn;
    }
    /* Optional immutable-main cache; keep recovered/live-module precedence. */
    extern int nfs_dispatch_cache_active __attribute__((weak));
    extern recomp_func_t nfs_dispatch_main_lookup(uint32_t) __attribute__((weak));
    if(&nfs_dispatch_cache_active && nfs_dispatch_cache_active && nfs_dispatch_main_lookup)
        return nfs_dispatch_main_lookup(va);
    /* Only the immutable main dispatch table is cached. Recovered hooks
     * and dynamically loaded modules retain precedence on every lookup. */
    static struct { uint32_t va; recomp_func_t fn; } cache[1024];
    unsigned slot = ((va >> 2) ^ (va >> 12)) & 1023;
    /* pair cache: not safe with parallel guest threads (front cache covers it) */
    if (hle_parallel()) {
        uint32_t lo = 0, hi = recomp_dispatch_count;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2, m = recomp_dispatch_table[mid].address;
            if (m == va) return recomp_dispatch_table[mid].func;
            if (m < va) lo = mid + 1; else hi = mid;
        }
        return NULL;
    }
    if (va && cache[slot].va == va) return cache[slot].fn;
    uint32_t lo = 0, hi = recomp_dispatch_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2, m = recomp_dispatch_table[mid].address;
        if (m == va) {
            cache[slot].va = va;
            cache[slot].fn = recomp_dispatch_table[mid].func;
            return cache[slot].fn;
        }
        if (m < va) lo = mid + 1; else hi = mid;
    }
    cache[slot].va = va;
    cache[slot].fn = NULL;
    return NULL;
}
/* Frame-cap pacing: sleep without holding the machine, so guest timer/worker
 * threads keep running while the present path waits for its slot. */
void nfs5_host_frame_pace(uint32_t ms) {
    hle_block_begin();
    usleep((useconds_t)ms * 1000u);
    hle_block_end();
}
recomp_func_t recomp_lookup_manual(uint32_t va) {
    extern void nfs_guard_dispatch(uint32_t) __attribute__((weak));
    if(nfs_guard_dispatch) {
        /* Resolve once on strict hosts. Imports occupy a direct-indexed range;
         * avoid two full guest-table searches and a linear shim-name scan on
         * every D3D/DirectInput call. Unknown targets still use the guard. */
        recomp_func_t fn=recomp_lookup_import(va);
        if(!fn)fn=recomp_lookup(va);
        if(fn)return fn;
        nfs_guard_dispatch(va);
    }
    return NULL;
}

/* --------------------------------------------------------- shim registry
 * Each shim gets a synthetic VA in a reserved range that no real image uses. */
static int acct_cat_for(const char *name);
static struct { const char *name; recomp_func_t fn; uint32_t va; int acct; int nolock; } g_shim[MAX_SHIMS];
/* Parallel mode: shims that only touch state owned by their calling thread
 * (the D3D7 device and the renderer front end, used by the render thread
 * only) or that synchronise with atomics (critical sections) run without the
 * machine lock. The render thread makes ~20k such calls per second. */
static int shim_nolock(const char *n) {
    return !strncmp(n, "IDirect3DDevice7::", 18) || !strcmp(n, "EnterCriticalSection") ||
           !strcmp(n, "LeaveCriticalSection") || !strcmp(n, "TryEnterCriticalSection") ||
           /* pure clock reads */
           !strcmp(n, "GetTickCount") || !strcmp(n, "timeGetTime") ||
           !strcmp(n, "QueryPerformanceCounter") || !strcmp(n, "QueryPerformanceFrequency");
}
static int g_shim_n;

/* Bring-up aid: NFS5_DUMP_SHIMS=1 prints every registered shim's synthetic VA
 * (SHIM_BASE + 16*index) so a crash's indirect-call targets can be named. */
static void dump_shims(void) {
    static int printed;
    if (!getenv("NFS5_DUMP_SHIMS")) return;
    for (; printed < g_shim_n; printed++)
        fprintf(stderr, "[shim] 0x%08X %s\n", g_shim[printed].va, g_shim[printed].name);
}
int win32hle_register(const win32hle_shim *shims) {
    int added = 0;
    for (; shims->name; shims++) {
        if (g_shim_n >= MAX_SHIMS) hle_fatal("too many shims");
        g_shim[g_shim_n].name = shims->name;
        g_shim[g_shim_n].acct = acct_cat_for(shims->name);
        g_shim[g_shim_n].fn   = shims->fn;
        g_shim[g_shim_n].nolock = shim_nolock(shims->name) && !getenv("NFS5_LOCK_ALL_SHIMS");
        g_shim[g_shim_n].va   = SHIM_BASE + SHIM_STEP * (uint32_t)g_shim_n;
        g_shim_n++; added++;
    }
    dump_shims();
    return added;
}
uint32_t hle_resolve(const char *name) {
    for (int i = 0; i < g_shim_n; i++)
        if (!strcmp(g_shim[i].name, name)) return g_shim[i].va;
    return 0;
}
const char *hle_name(uint32_t va) {
    if(va<SHIM_BASE)return NULL;
    uint32_t offset=va-SHIM_BASE,index=offset/SHIM_STEP;
    if(offset%SHIM_STEP==0 && index<(uint32_t)g_shim_n)return g_shim[index].name;
    return NULL;
}

/* The shim VA last dispatched, so the unimplemented-import stub can name itself
 * (its fn is shared, so it learns which import it is from here). */
static RECOMP_TLS uint32_t g_last_import_va;

/* Main-thread phase accounting (runtime/nfs5_phaseacct.c, NFS5_PHASE_LIVE=1).
 * Weak so the standalone selftests can link this file without the module. */
enum { ACCT_D3D7 = 0, ACCT_RDRAW = 1, ACCT_RUPLOAD = 2, ACCT_READBACK = 3,
       ACCT_DDRAW = 4, ACCT_DINPUT = 5, ACCT_DSOUND = 6, ACCT_WIN32 = 7, ACCT_WAIT = 8 };
extern void nfs5_acct_enter(int) __attribute__((weak));
extern void nfs5_acct_leave(int) __attribute__((weak));
static int acct_cat_for(const char *n) {
    if (!n) return ACCT_WIN32;
    if (!strncmp(n, "IDirect3D", 9)) return ACCT_D3D7;
    if (!strncmp(n, "IDirectDraw", 11)) return ACCT_DDRAW;
    if (!strncmp(n, "IDirectInput", 12)) return ACCT_DINPUT;
    if (!strncmp(n, "IDirectSound", 12)) return ACCT_DSOUND;
    return ACCT_WIN32;
}
/* An import VA that is one of our synthetic shim VAs resolves to its shim,
 * entered through shim_entry: the lookup and the call are back to back on the
 * thread that holds the machine, so the index passes through a global. */
static RECOMP_TLS uint32_t g_pending_shim;   /* set by lookup, read by shim_entry: same thread */
/* NFS5_SHIMSTATS=1: per-shim call counts, dumped at exit. Bring-up aid. */
static uint64_t g_shim_calls[MAX_SHIMS];
static void dump_shim_stats(void) {
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("NFS5_SHIMSTATS") != NULL;
    if (!enabled) return;
    for (int i = 0; i < g_shim_n; i++)
        if (g_shim_calls[i]) fprintf(stderr, "[shimstats] %8llu %s\n",
                                     (unsigned long long)g_shim_calls[i], g_shim[i].name);
}
static int stats_ready;
/* NFS5_SHIMSTATS=1: also dump a live delta every ~2 s (the process is usually
 * killed by timeout, so atexit never runs). */
static void dump_alive(void) {
    static uint64_t last[MAX_SHIMS];
    static uint32_t last_ms;
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("NFS5_SHIMSTATS") != NULL;
    if (!enabled) return;
    uint32_t now = (uint32_t)(hle_ticks_ms());
    if (last_ms && now - last_ms < 2000) return;
    for (int i = 0; i < g_shim_n; i++) {
        uint64_t d = g_shim_calls[i] - last[i];
        last[i] = g_shim_calls[i];
        if (!d) continue;
        const char *n = g_shim[i].name;
        if (strncmp(n, "DID::", 5) && !strstr(n, "joy") && !strstr(n, "Key") && !strstr(n, "Input")
            && !strstr(n, "Focus") && !strstr(n, "Foreground") && !strstr(n, "Active") && !strstr(n, "Cursor")
            && !strstr(n, "Message") && !strstr(n, "Peek") && !strstr(n, "GetMessage") && !strstr(n, "Dispatch"))
            continue;
        fprintf(stderr, "[shimstats-d] %6llu %s\n", (unsigned long long)d, n);
    }
    last_ms = now;
}
static void shim_entry_body(uint32_t i);
static void shim_entry(void) {
    uint32_t i = g_pending_shim;
    if (hle_parallel() && !g_shim[i].nolock) {
        lock_claim_label(g_shim[i].name);
        shim_entry_body(i);
        lock_release();
        return;
    }
    shim_entry_body(i);
}
static void shim_entry_body(uint32_t i) {
    if (g_waiters) hle_yield();
    if (!stats_ready) { stats_ready = 1; atexit(dump_shim_stats); }
    g_shim_calls[i]++;
    dump_alive();
    g_last_import_va = g_shim[i].va;                     /* again: the thread that ran meanwhile set its own */
    g_cur_import = g_shim[i].name;
    int acct = g_shim[i].acct;
    if (nfs5_acct_enter) nfs5_acct_enter(acct);
    g_shim[i].fn();
    if (nfs5_acct_leave) nfs5_acct_leave(acct);
}
recomp_func_t recomp_lookup_import(uint32_t va) {
    if (va < SHIM_BASE || va >= SHIM_BASE + SHIM_STEP * (uint32_t)g_shim_n) return NULL;
    uint32_t i = (va - SHIM_BASE) / SHIM_STEP;
    if (i < (uint32_t)g_shim_n && g_shim[i].va == va) {
        g_last_import_va = va;
        g_cur_import = g_shim[i].name;
        if (win32hle_trace) fprintf(stderr, "[hle] %s\n", g_shim[i].name);
        g_pending_shim = i;
        return shim_entry;
    }
    return NULL;
}

/* Permissive bind: unresolved imports bind here instead of failing. A stdcall
 * import can't be a silent no-op (it would not pop its args and the stack would
 * drift), so this names itself from the last-dispatched VA and stops — giving
 * the exact import the running program reached and who called it. Implement
 * that import, rerun, get the next. */
static void hle_unimplemented(void) {
    const char *nm = hle_name(g_last_import_va);
    hle_fatal("unimplemented import %s (0x%08X) called from guest 0x%08X",
              nm ? nm : "?", g_last_import_va, g_cur_func);
}

/* Resolve a name to a shim VA, or register a self-naming stub for it and return
 * that. Used as the resolver for a permissive bind. */
uint32_t hle_resolve_or_stub(const char *name) {
    uint32_t va = hle_resolve(name);
    if (va) return va;
    if (g_shim_n >= MAX_SHIMS) hle_fatal("too many shims");
    /* names from the IAT outlive the bind; the loader's image is mapped for the
     * whole run, so keeping the pointer is safe. */
    g_shim[g_shim_n].name = name;
    g_shim[g_shim_n].acct = acct_cat_for(name);
    g_shim[g_shim_n].fn   = hle_unimplemented;
    g_shim[g_shim_n].va   = SHIM_BASE + SHIM_STEP * (uint32_t)g_shim_n;
    return g_shim[g_shim_n++].va;
}

/* ------------------------------------------------------- native -> guest */
uint32_t hle_call_guest(uint32_t va, int nargs, const uint32_t *args) {
    mach_enter();
    recomp_func_t f = recomp_lookup(va);       /* under the machine: shim_entry's index is shared */
    if (!f) f = recomp_lookup_import(va);
    if (!f) hle_fatal("no lifted function at 0x%08X", va);
    regs_t saved; regs_save(&saved);
    for (int i = nargs - 1; i >= 0; i--) PUSH32(g_esp, args[i]);
    PUSH32(g_esp, RECOMP_RETADDR);
    /* parallel: a host thread that did not already hold the machine (thread
     * start, timer callback, the main entry) runs the guest code unlocked;
     * callbacks issued from inside a shim keep the claim they came with */
    int run_unlocked = hle_parallel() && g_lock_depth == 1;
    if (run_unlocked) lock_release();
    f();
    if (run_unlocked) lock_claim_label("guest callback return");
    uint32_t eax = g_eax;
    regs_load(&saved);
    mach_leave();
    return eax;
}

/* Loops in lifted code hand the machine over too (RECOMP_BACKEDGE). */
static void yield_hook(void) { hle_yield(); }
__attribute__((constructor)) static void install_yield_hook(void) { recomp_yield_hook = yield_hook; }

/* ---------------------------------------------------------- handles
 * Kernel objects the guest holds by HANDLE: files, find searches, events,
 * mutexes... A handle is 0x100 + 4*slot (never 0, never INVALID_HANDLE_VALUE),
 * typed so a file call given an event's handle fails as on Windows. Each type
 * may register a closer for CloseHandle. Guest code calls these under the
 * machine; a host thread that touches the table must hold it too. */
#define MAX_HANDLES 4096
static struct { int type; void *obj; } g_handle[MAX_HANDLES];
static void (*g_closer[32])(void *);

uint32_t hle_handle_alloc(int type, void *obj) {
    for (int i = 1; i < MAX_HANDLES; i++)
        if (!g_handle[i].type) { g_handle[i].type = type; g_handle[i].obj = obj; return 0x100u + 4u * (uint32_t)i; }
    return 0;
}
static int handle_slot(uint32_t h) {
    if (h < 0x100u || (h & 3u)) return -1;
    uint32_t i = (h - 0x100u) / 4u;
    return i < MAX_HANDLES && g_handle[i].type ? (int)i : -1;
}
void *hle_handle_obj(uint32_t h, int type) {
    int i = handle_slot(h);
    return i >= 0 && (type == 0 || g_handle[i].type == type) ? g_handle[i].obj : NULL;
}
int hle_handle_type(uint32_t h) { int i = handle_slot(h); return i >= 0 ? g_handle[i].type : 0; }
void hle_handle_set_closer(int type, void (*fn)(void *)) { if (type > 0 && type < 32) g_closer[type] = fn; }
int hle_handle_close(uint32_t h) {
    int i = handle_slot(h);
    if (i < 0) return 0;
    int type = g_handle[i].type;
    void *obj = g_handle[i].obj;
    g_handle[i].type = 0, g_handle[i].obj = NULL;
    if (type > 0 && type < 32 && g_closer[type]) g_closer[type](obj);
    return 1;
}
