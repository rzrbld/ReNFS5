/*
 * nfs5_prof.c - low-overhead sampling profiler + frame statistics for NFS5.
 *
 * See runtime/profiling/nfs5_prof.h for the design and the environment
 * variables. Two builds are supported from this one source:
 *
 *   in-process : the game Makefile compiles runtime/profiling/ into the
 *                binary (the constructor self-starts when NFS5_PROFILE is set).
 *   LD_PRELOAD : gcc -shared -fPIC -DNFS5_PROF_PRELOAD ... ; this adds
 *                interposers for the SDL present functions so the *existing*
 *                binary can be measured without rebuilding it.
 *
 * The lifted runtime publishes the guest VA of the function it is executing in
 * `g_cur_func` (RECOMP_ENTER). We resolve it with dlsym(RTLD_DEFAULT) so this
 * TU does not need to be linked into the game to reference it, and so a preload
 * build works against the -rdynamic executable.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <malloc.h>
#include <dlfcn.h>
#include <errno.h>

#include "nfs5_prof.h"

/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */
#define PROF_MAX_PATH 512

typedef struct {
    int    enabled;
    long   interval_us;
    long   timeline_us;
    long   snapshot_us;
    int    top_n;
    char   out[PROF_MAX_PATH];
    char   fps_csv[PROF_MAX_PATH];
    char   mem_csv[PROF_MAX_PATH];
    char   catalog_dir[PROF_MAX_PATH];
} prof_cfg_t;

static prof_cfg_t g_cfg;

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */
#define SAMPLE_RING (1u << 18)          /* 262144 samples ~ 21 min at 200 Hz */
#define HASH_CAP    (1u << 15)          /* 32768 distinct functions */
#define FRAME_CAP   (1u << 18)          /* 262144 presented frames */

typedef struct { uint64_t t_us; uint32_t va; } sample_t;
typedef struct { uint32_t va; uint32_t hits; } hotbucket_t;

static sample_t      *g_samples;
static volatile uint32_t g_nsamples;    /* samples recorded (capped at ring) */
static hotbucket_t    g_hash[HASH_CAP];
static uint32_t       g_nhash;
static uint64_t      *g_frames;
static volatile uint32_t g_nframes;

/* named phase timers / counters for subsystems (renderer, audio, ...) */
typedef struct { const char *name; uint64_t count, total_ns, open_ns; int used; } phase_t;
typedef struct { const char *name; uint64_t total, count; int used; } counter_t;
static phase_t   g_phases[NFS5_PROF_MAX_PHASES];
static counter_t g_counters[NFS5_PROF_MAX_COUNTERS];
static uint64_t       g_t0_us;          /* CLOCK_MONOTONIC at start */
static uint64_t       g_t0_wall;        /* CLOCK_REALTIME at start */
static volatile int   g_running;
static volatile int   g_stop;
static pthread_t      g_thread;
static pthread_mutex_t g_dump_lock = PTHREAD_MUTEX_INITIALIZER;
static int            g_have_dump_lock;
static int            g_finalized;
static uint32_t      *g_cur_p;          /* &g_cur_func, resolved by dlsym */
static uint64_t       g_zero_samples;   /* samples outside lifted code */
static uint64_t       g_total_samples;
static long           g_rss_kb, g_rss_peak_kb;
static long           g_heap_kb, g_heap_peak_kb;
static uint64_t       g_last_mem_us;
static char           g_event[256];
static int            g_have_event;

/* catalog: guest VA -> symbol name */
typedef struct {
    uint32_t va;
    uint32_t insns;
    char     module[16];
    const char *sym;        /* points into g_catbuf */
} catent_t;
static catent_t *g_cat;
static int       g_ncat;

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */
static uint64_t mono_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}
static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static long env_long(const char *name, long def) {
    const char *v = getenv(name);
    if (!v || !*v) return def;
    char *end = NULL;
    long x = strtol(v, &end, 10);
    return (end && end != v) ? x : def;
}

static void env_path(const char *name, char *dst, size_t cap) {
    const char *v = getenv(name);
    dst[0] = 0;
    if (v && *v) snprintf(dst, cap, "%s", v);
}

/* ------------------------------------------------------------------ */
/* catalogs (VA -> symbol), parsed from build/translated/{porsche,dx7z}/catalog.json */
/* for human-readable hotspot rows. Tolerant scanner: the files are     */
/* generated JSON, not arbitrary user input.                            */
/* ------------------------------------------------------------------ */
static int read_file(const char *path, char **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long n = ftell(f);
    if (n <= 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    *out = buf;
    *out_len = got;
    return 0;
}

static char *g_catbufs[4];
static int   g_ncatbufs;

static void catalog_scan(const char *path, const char *module) {
    char *buf = NULL;
    size_t len = 0;
    if (read_file(path, &buf, &len) != 0) return;
    int entries = 0;
    for (const char *p = buf; (p = strstr(p, "\"va\"")) != NULL; p += 4) entries++;
    if (entries <= 0) { free(buf); return; }
    if (g_ncatbufs >= 4) { free(buf); return; }

    catent_t *arr = (catent_t *)calloc((size_t)entries, sizeof(*arr));
    if (!arr) { free(buf); return; }

    const char *p = buf;
    int n = 0;
    while (n < entries && (p = strstr(p, "\"va\"")) != NULL) {
        const char *q = strchr(p, ':');
        if (!q) break;
        const char *h = strstr(q, "0x");
        if (!h) break;
        char *end = NULL;
        unsigned long va = strtoul(h, &end, 16);
        if (end == h) break;
        const char *sym = strstr(end, "\"symbol\"");
        if (!sym) break;
        const char *colon = strchr(sym, ':');
        if (!colon) break;
        const char *s = strchr(colon, '"');
        if (!s) break;
        s++;
        const char *e = strchr(s, '"');
        if (!e) break;
        arr[n].va = (uint32_t)va;
        arr[n].insns = 0;
        arr[n].sym = s;                 /* points into buf; buffer is retained */
        snprintf(arr[n].module, sizeof arr[n].module, "%s", module);
        const char *ins = strstr(e, "\"instructions\"");
        if (ins) {
            const char *c2 = strchr(ins, ':');
            if (c2) arr[n].insns = (uint32_t)strtoul(c2 + 1, NULL, 10);
        }
        if (e - s < 120) ((char *)e)[0] = 0;   /* NUL-terminate in place */
        n++;
        p = e + 1;
    }
    g_catbufs[g_ncatbufs++] = buf;

    catent_t *merged = (catent_t *)realloc(g_cat, sizeof(catent_t) * (size_t)(g_ncat + n));
    if (!merged) { free(arr); return; }
    g_cat = merged;
    memcpy(g_cat + g_ncat, arr, sizeof(catent_t) * (size_t)n);
    g_ncat += n;
    free(arr);
}

static int cmp_cat_va(const void *a, const void *b) {
    uint32_t x = ((const catent_t *)a)->va, y = ((const catent_t *)b)->va;
    return (x > y) - (x < y);
}

static void catalog_load(void) {
    if (g_cat) return;
    const char *dir = g_cfg.catalog_dir[0] ? g_cfg.catalog_dir : "build/translated";
    char path[PROF_MAX_PATH];
    snprintf(path, sizeof path, "%s/porsche/catalog.json", dir);
    catalog_scan(path, "porsche");
    snprintf(path, sizeof path, "%s/dx7z/catalog.json", dir);
    catalog_scan(path, "dx7z");
    if (g_ncat > 1) qsort(g_cat, (size_t)g_ncat, sizeof(catent_t), cmp_cat_va);
}

static const catent_t *catalog_find(uint32_t va) {
    if (!g_cat) return NULL;
    /* generated tables are VA-sorted; fall back to linear if not */
    int lo = 0, hi = g_ncat - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_cat[mid].va == va) return &g_cat[mid];
        if (g_cat[mid].va < va) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

static void va_label(uint32_t va, char *dst, size_t cap) {
    if (va == 0) { snprintf(dst, cap, "host (outside lifted code)"); return; }
    const catent_t *c = catalog_find(va);
    if (c && c->sym) snprintf(dst, cap, "%s:%s", c->module, c->sym);
    else snprintf(dst, cap, "sub_%08X", va);
}

/* ------------------------------------------------------------------ */
/* phase timers and counters (registered once, used from one thread)   */
/* ------------------------------------------------------------------ */
int nfs5_prof_phase_slot(const char *name) {
    if (!__atomic_load_n(&g_running, __ATOMIC_RELAXED) || !name) return -1;
    for (int i = 0; i < NFS5_PROF_MAX_PHASES; i++)
        if (g_phases[i].used && !strcmp(g_phases[i].name, name)) return i;
    for (int i = 0; i < NFS5_PROF_MAX_PHASES; i++)
        if (!g_phases[i].used) {
            g_phases[i].name = name;
            g_phases[i].used = 1;
            return i;
        }
    return -1;
}

void nfs5_prof_phase_begin(int slot) {
    if (slot < 0 || slot >= NFS5_PROF_MAX_PHASES) return;
    if (!__atomic_load_n(&g_running, __ATOMIC_RELAXED)) return;
    g_phases[slot].open_ns = mono_ns();
}

void nfs5_prof_phase_end(int slot) {
    if (slot < 0 || slot >= NFS5_PROF_MAX_PHASES) return;
    if (!__atomic_load_n(&g_running, __ATOMIC_RELAXED)) return;
    uint64_t t0 = g_phases[slot].open_ns;
    if (!t0) return;
    g_phases[slot].open_ns = 0;
    g_phases[slot].total_ns += mono_ns() - t0;
    g_phases[slot].count++;
}

int nfs5_prof_counter_slot(const char *name) {
    if (!__atomic_load_n(&g_running, __ATOMIC_RELAXED) || !name) return -1;
    for (int i = 0; i < NFS5_PROF_MAX_COUNTERS; i++)
        if (g_counters[i].used && !strcmp(g_counters[i].name, name)) return i;
    for (int i = 0; i < NFS5_PROF_MAX_COUNTERS; i++)
        if (!g_counters[i].used) {
            g_counters[i].name = name;
            g_counters[i].used = 1;
            return i;
        }
    return -1;
}

void nfs5_prof_counter_add(int slot, uint64_t value) {
    if (slot < 0 || slot >= NFS5_PROF_MAX_COUNTERS) return;
    if (!__atomic_load_n(&g_running, __ATOMIC_RELAXED)) return;
    __atomic_fetch_add(&g_counters[slot].total, value, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_counters[slot].count, 1, __ATOMIC_RELAXED);
}

/* ------------------------------------------------------------------ */
/* frame statistics                                                    */
/* ------------------------------------------------------------------ */
void nfs5_frame_mark(void) {
    if (!__atomic_load_n(&g_running, __ATOMIC_RELAXED)) return;
    uint32_t i = __atomic_load_n(&g_nframes, __ATOMIC_RELAXED);
    if (i >= FRAME_CAP) return;
    if (!g_frames) return;
    g_frames[i] = mono_ns();
    __atomic_store_n(&g_nframes, i + 1, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------ */
/* memory sampling (RSS + guest-heap proxy)                            */
/* ------------------------------------------------------------------ */
static void read_mem(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            unsigned long kb;
            if (sscanf(line, "VmRSS: %lu kB", &kb) == 1) g_rss_kb = (long)kb;
            else if (sscanf(line, "VmHWM: %lu kB", &kb) == 1) g_rss_peak_kb = (long)kb;
        }
        fclose(f);
    }
    struct mallinfo2 mi = mallinfo2();
    g_heap_kb = (long)(mi.uordblks / 1024);
    if (g_heap_kb > g_heap_peak_kb) g_heap_peak_kb = g_heap_kb;
}

static void mem_csv_append(uint64_t t_us) {
    if (!g_cfg.mem_csv[0]) return;
    FILE *f = fopen(g_cfg.mem_csv, "a");
    if (!f) return;
    if (ftell(f) == 0)
        fprintf(f, "t_ms,rss_kb,rss_peak_kb,heap_inuse_kb,heap_peak_kb\n");
    fprintf(f, "%.1f,%ld,%ld,%ld,%ld\n", (double)(t_us - g_t0_us) / 1000.0,
            g_rss_kb, g_rss_peak_kb, g_heap_kb, g_heap_peak_kb);
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* sampler                                                             */
/* ------------------------------------------------------------------ */
static void hash_record(uint32_t va, uint32_t *nhash) {
    if (*nhash >= HASH_CAP) return;      /* table full: keep the first functions */
    uint32_t mask = HASH_CAP - 1;
    uint32_t i = (va * 2654435761u) & mask;
    for (;;) {
        if (g_hash[i].hits == 0) {       /* empty slot */
            g_hash[i].va = va;
            g_hash[i].hits = 1;
            (*nhash)++;
            return;
        }
        if (g_hash[i].va == va) { g_hash[i].hits++; return; }
        i = (i + 1) & mask;
    }
}

/* Per guest-thread attribution: win32hle runs every guest thread under one
 * machine lock and publishes the owner in nfs5_guest_owner. Sampling it with
 * g_cur_func splits the hotspots by thread (frame thread vs audio mixer vs
 * timers); "idle" means no guest thread held the machine. */
#define OWN_MAX 16
#define OWN_HASH 4096
extern pthread_t nfs5_guest_owner __attribute__((weak));
typedef struct { pthread_t id; uint32_t hits; hotbucket_t h[OWN_HASH]; uint32_t n; } owner_t;
static owner_t *g_own;      /* OWN_MAX entries, calloc'ed on first use */
static uint32_t g_own_n, g_own_idle;
static void owner_record(uint32_t va) {
    if (!&nfs5_guest_owner) return;
    pthread_t o = __atomic_load_n(&nfs5_guest_owner, __ATOMIC_RELAXED);
    if (!o) { g_own_idle++; return; }
    if (!g_own) { g_own = (owner_t *)calloc(OWN_MAX, sizeof(owner_t)); if (!g_own) return; }
    owner_t *w = NULL;
    for (uint32_t i = 0; i < g_own_n; i++) if (pthread_equal(g_own[i].id, o)) { w = &g_own[i]; break; }
    if (!w) { if (g_own_n >= OWN_MAX) return; w = &g_own[g_own_n++]; w->id = o; }
    w->hits++;
    if (!va || w->n >= OWN_HASH / 2) return;
    uint32_t m = OWN_HASH - 1, k = (va * 2654435761u) & m;
    while (w->h[k].va && w->h[k].va != va) k = (k + 1) & m;
    if (!w->h[k].va) { w->h[k].va = va; w->n++; }
    w->h[k].hits++;
}

static void *sampler_main(void *arg) {
    (void)arg;
    struct sched_param sp;
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = 0;
    /* Lowest practical priority: SCHED_IDLE if the kernel allows it for an
     * unprivileged thread, otherwise stay at SCHED_OTHER nice 19. */
    if (pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp) != 0)
        if (nice(19) == -1) { /* best effort */ }

    uint64_t start_us = mono_us();
    /* NFS5_PROFILE_FROM_MS / _TO_MS: only record samples inside a wall-clock
     * window (e.g. the steady-state race), so hotspot lists are not diluted by
     * boot/loading/front-end. Timing and snapshots are unaffected. */
    uint64_t win_from_us = 0, win_to_us = 0;
    {
        const char *v = getenv("NFS5_PROFILE_FROM_MS");
        if (v && *v) win_from_us = (uint64_t)strtoull(v, NULL, 10) * 1000ull;
        v = getenv("NFS5_PROFILE_TO_MS");
        if (v && *v) win_to_us = (uint64_t)strtoull(v, NULL, 10) * 1000ull;
    }
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
        uint32_t va = g_cur_p ? __atomic_load_n(g_cur_p, __ATOMIC_RELAXED) : 0;
        uint64_t t = mono_us();
        int in_window = (t - start_us) >= win_from_us && (!win_to_us || (t - start_us) <= win_to_us);
        if (in_window) {
            uint32_t n = __atomic_load_n(&g_nsamples, __ATOMIC_RELAXED);
            if (n < SAMPLE_RING && g_samples) {
                g_samples[n].t_us = t;
                g_samples[n].va = va;
                __atomic_store_n(&g_nsamples, n + 1, __ATOMIC_RELEASE);
            }
            g_total_samples++;
            if (va) hash_record(va, &g_nhash); else g_zero_samples++;
            owner_record(va);
        }
        if (t - g_last_mem_us >= 200000) {
            g_last_mem_us = t;
            read_mem();
            mem_csv_append(t);
        }
        if (g_cfg.snapshot_us > 0) {
            static uint64_t last_snap;
            if (!last_snap) last_snap = start_us;
            if ((long)(t - last_snap) >= g_cfg.snapshot_us) {
                last_snap = t;
                nfs5_prof_dump();
            }
        }

        /* absolute sleep keeps the sample rate from drifting */
        uint64_t next_ns = (uint64_t)next.tv_sec * 1000000000ull + (uint64_t)next.tv_nsec
                         + (uint64_t)g_cfg.interval_us * 1000ull;
        next.tv_sec = (time_t)(next_ns / 1000000000ull);
        next.tv_nsec = (long)(next_ns % 1000000000ull);
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR) {}
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* report writing                                                      */
/* ------------------------------------------------------------------ */
static int cmp_hot(const void *a, const void *b) {
    const hotbucket_t *x = (const hotbucket_t *)a, *y = (const hotbucket_t *)b;
    if (x->hits != y->hits) return (x->hits < y->hits) ? 1 : -1;
    return (x->va > y->va) - (x->va < y->va);
}
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* One timeline bucket: the top-3 functions by samples in [t0,t1). */
static void bucket_top(uint64_t t0, uint64_t t1, char *dst, size_t cap, int *out_n) {
    uint32_t n = __atomic_load_n(&g_nsamples, __ATOMIC_ACQUIRE);
    struct { uint32_t va; uint32_t c; } local[128];
    int nl = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t t = g_samples[i].t_us;
        if (t < t0 || t >= t1) continue;
        uint32_t va = g_samples[i].va;
        int k;
        for (k = 0; k < nl; k++) if (local[k].va == va) break;
        if (k == nl) { if (nl < 128) { local[nl].va = va; local[nl].c = 0; nl++; } else continue; }
        local[k].c++;
    }
    int total = 0;
    for (int k = 0; k < nl; k++) total += (int)local[k].c;
    *out_n = total;
    dst[0] = 0;
    for (int row = 0; row < 3 && row < nl; row++) {
        int best = -1;
        for (int k = 0; k < nl; k++)
            if (local[k].c > 0 && (best < 0 || local[k].c > local[best].c)) best = k;
        if (best < 0) break;
        char name[160];
        va_label(local[best].va, name, sizeof name);
        size_t used = strlen(dst);
        snprintf(dst + used, cap - used, "%s%s %.0f%%", row ? ", " : "", name,
                 total ? 100.0 * local[best].c / total : 0.0);
        local[best].c = 0;
    }
    if (!dst[0]) snprintf(dst, cap, "(no samples)");
}

static double percentile(const uint64_t *sorted, int n, double p) {
    if (n <= 0) return 0.0;
    int idx = (int)(p * (double)(n - 1) + 0.5);
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    return (double)sorted[idx];
}

static void frame_stats(FILE *out) {
    uint32_t n = __atomic_load_n(&g_nframes, __ATOMIC_ACQUIRE);
    if (n < 2) { fprintf(out, "frames=%u\nfps_avg=0\n", n); return; }
    uint64_t *iv = (uint64_t *)malloc((size_t)(n - 1) * sizeof(uint64_t));
    if (!iv) { fprintf(out, "frames=%u\nfps_avg=0\n", n); return; }
    for (uint32_t i = 1; i < n; i++) iv[i - 1] = g_frames[i] - g_frames[i - 1];
    uint64_t total_ns = g_frames[n - 1] - g_frames[0];
    double fps_avg = total_ns ? (double)(n - 1) * 1e9 / (double)total_ns : 0.0;
    uint64_t max_iv = 0, sum = 0;
    for (uint32_t i = 0; i < n - 1; i++) { if (iv[i] > max_iv) max_iv = iv[i]; sum += iv[i]; }
    qsort(iv, n - 1, sizeof(uint64_t), cmp_u64);
    double p50 = percentile(iv, (int)(n - 1), 0.50);
    double p95 = percentile(iv, (int)(n - 1), 0.95);
    double p99 = percentile(iv, (int)(n - 1), 0.99);
    int worst = (int)((n - 1) / 100);
    if (worst < 1) worst = 1;
    uint64_t wsum = 0;
    for (int i = (int)(n - 1) - worst; i < (int)(n - 1); i++) wsum += iv[i];
    double low_fps = wsum ? (double)worst * 1e9 / (double)wsum : 0.0;
    int sp50 = 0, sp100 = 0;
    for (uint32_t i = 0; i < n - 1; i++) {
        if (iv[i] > 50000000ull) sp50++;
        if (iv[i] > 100000000ull) sp100++;
    }
    fprintf(out, "frames=%u\n", n);
    fprintf(out, "frames_span_s=%.3f\n", (double)total_ns / 1e9);
    fprintf(out, "fps_avg=%.2f\n", fps_avg);
    fprintf(out, "fps_1pct_low=%.2f\n", low_fps);
    fprintf(out, "frame_ms_p50=%.2f\nframe_ms_p95=%.2f\nframe_ms_p99=%.2f\nframe_ms_max=%.2f\n",
            p50 / 1e6, p95 / 1e6, p99 / 1e6, (double)max_iv / 1e6);
    fprintf(out, "frame_ms_mean=%.2f\n", (double)sum / (double)(n - 1) / 1e6);
    fprintf(out, "spikes_gt_50ms=%d\nspikes_gt_100ms=%d\n", sp50, sp100);
    free(iv);
}

static void write_fps_csv(void) {
    if (!g_cfg.fps_csv[0]) return;
    uint32_t n = __atomic_load_n(&g_nframes, __ATOMIC_ACQUIRE);
    char tmp[PROF_MAX_PATH + 32];
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", g_cfg.fps_csv, (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "frame,time_ms,dt_ms\n");
    for (uint32_t i = 0; i < n; i++) {
        double t_ms = i < 1 ? 0.0 : (double)(g_frames[i] - g_frames[0]) / 1e6;
        double dt_ms = i < 1 ? 0.0 : (double)(g_frames[i] - g_frames[i - 1]) / 1e6;
        fprintf(f, "%u,%.3f,%.3f\n", i, t_ms, dt_ms);
    }
    fclose(f);
    rename(tmp, g_cfg.fps_csv);
}

static void event_line(char *dst, size_t cap) {
    if (!g_have_event) { snprintf(dst, cap, "none"); return; }
    snprintf(dst, cap, "%s", g_event);
}

void nfs5_prof_dump(void) {
    if (!__atomic_load_n(&g_running, __ATOMIC_RELAXED)) return;
    if (g_have_dump_lock) pthread_mutex_lock(&g_dump_lock);

    catalog_load();
    uint64_t now = mono_us();
    uint32_t nsamp = __atomic_load_n(&g_nsamples, __ATOMIC_ACQUIRE);

    FILE *out = stderr;
    char tmp[PROF_MAX_PATH + 32];
    int to_file = 0;
    if (g_cfg.out[0]) {
        snprintf(tmp, sizeof tmp, "%s.tmp.%d", g_cfg.out, (int)getpid());
        FILE *f = fopen(tmp, "w");
        if (f) { out = f; to_file = 1; }
    }

    char when[64] = "?";
    time_t w = (time_t)g_t0_wall;
    struct tm tmv;
    if (localtime_r(&w, &tmv)) strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tmv);
    char ev[256];
    event_line(ev, sizeof ev);

    fprintf(out, "# nfs5_prof sampling report\n");
    fprintf(out, "pid=%ld\n", (long)getpid());
    fprintf(out, "started_wall=%s\n", when);
    fprintf(out, "duration_s=%.3f\n", (double)(now - g_t0_us) / 1e6);
    fprintf(out, "sample_interval_us=%ld\n", g_cfg.interval_us);
    fprintf(out, "samples=%llu\n", (unsigned long long)g_total_samples);
    fprintf(out, "samples_ring=%u\n", nsamp);
    fprintf(out, "samples_host=%llu\n", (unsigned long long)g_zero_samples);
    fprintf(out, "distinct_functions=%u\n", g_nhash);
    fprintf(out, "cur_func_symbol=%s\n", g_cur_p ? "resolved" : "MISSING (no g_cur_func)");
    fprintf(out, "rss_kb=%ld\nrss_peak_kb=%ld\n", g_rss_kb, g_rss_peak_kb);
    fprintf(out, "host_heap_inuse_kb=%ld\nhost_heap_peak_kb=%ld\n", g_heap_kb, g_heap_peak_kb);
    {
        uint32_t hw = 0;
        uint32_t (*hw_fn)(void) = nfs5_guest_heap_high_water;   /* weak: NULL if unimpl. */
        const char *src = "mallinfo2-proxy (win32hle heap is malloc-backed)";
        if (hw_fn) { hw = hw_fn(); src = "hle-heap-hook"; }
        else hw = (uint32_t)g_heap_peak_kb;
        fprintf(out, "guest_heap_high_water_kb=%u\nguest_heap_source=%s\n", hw, src);
    }
    fprintf(out, "event=%s\n", ev);
    frame_stats(out);

    {
        int any = 0;
        for (int i = 0; i < NFS5_PROF_MAX_PHASES; i++) if (g_phases[i].used && g_phases[i].count) any = 1;
        if (any) {
            fprintf(out, "\n# phases\nphase,count,total_ms,avg_us\n");
            for (int i = 0; i < NFS5_PROF_MAX_PHASES; i++) {
                if (!g_phases[i].used || !g_phases[i].count) continue;
                fprintf(out, "%s,%llu,%.3f,%.1f\n", g_phases[i].name,
                        (unsigned long long)g_phases[i].count,
                        (double)g_phases[i].total_ns / 1e6,
                        (double)g_phases[i].total_ns / 1e3 / (double)g_phases[i].count);
            }
        }
        any = 0;
        for (int i = 0; i < NFS5_PROF_MAX_COUNTERS; i++) if (g_counters[i].used && g_counters[i].count) any = 1;
        if (any) {
            fprintf(out, "\n# counters\ncounter,count,total\n");
            for (int i = 0; i < NFS5_PROF_MAX_COUNTERS; i++) {
                if (!g_counters[i].used || !g_counters[i].count) continue;
                fprintf(out, "%s,%llu,%llu\n", g_counters[i].name,
                        (unsigned long long)g_counters[i].count,
                        (unsigned long long)g_counters[i].total);
            }
        }
    }

    /* ---------------- timeline ---------------- */
    fprintf(out, "\n# timeline (bucket=%ld ms): where the sampled time went, top 3 per bucket\n",
            g_cfg.timeline_us / 1000);
    uint64_t t_end = now - g_t0_us;
    for (int b = 0; b < 240; b++) {
        uint64_t b0 = (uint64_t)b * (uint64_t)g_cfg.timeline_us;
        if (b0 > t_end) break;
        uint64_t b1 = b0 + (uint64_t)g_cfg.timeline_us;
        char top[512];
        int n_in_bucket = 0;
        bucket_top(g_t0_us + b0, g_t0_us + b1, top, sizeof top, &n_in_bucket);
        if (n_in_bucket <= 0) continue;
        fprintf(out, "t=+%6.3fs n=%-5d %s\n", (double)b0 / 1e6, n_in_bucket, top);
    }

    /* ---------------- hotspots ---------------- */
    fprintf(out, "\n# hotspots: top %d lifted functions by samples\n", g_cfg.top_n);
    fprintf(out, "# %-12s %-10s %6s  %-6s  %s\n", "va", "hits", "pct", "insns", "symbol");
    hotbucket_t *copy = (hotbucket_t *)malloc(sizeof(hotbucket_t) * (HASH_CAP ? HASH_CAP : 1));
    if (copy) {
        uint32_t live = 0;
        for (uint32_t i = 0; i < HASH_CAP; i++)
            if (g_hash[i].hits) copy[live++] = g_hash[i];
        qsort(copy, live, sizeof(hotbucket_t), cmp_hot);
        int rows = (int)live < g_cfg.top_n ? (int)live : g_cfg.top_n;
        for (int r = 0; r < rows; r++) {
            const catent_t *c = catalog_find(copy[r].va);
            char name[192];
            if (c && c->sym) snprintf(name, sizeof name, "%s", c->sym);
            else snprintf(name, sizeof name, "sub_%08X", copy[r].va);
            const char *note = (c && c->insns <= 2) ? "  [jmp thunk: samples are time in the HLE import]"
                                                   : "";
            fprintf(out, "  0x%08X %-10u %5.1f%%  %-6u  %s%s\n",
                    copy[r].va, copy[r].hits,
                    100.0 * (double)copy[r].hits / (double)(g_total_samples ? g_total_samples : 1),
                    c ? c->insns : 0, name, note);
        }
        free(copy);
    }

    if (g_own && g_own_n) {
        uint32_t tot = g_own_idle;
        for (uint32_t i = 0; i < g_own_n; i++) tot += g_own[i].hits;
        fprintf(out, "\n# per guest-thread (machine-lock owner) samples; idle=%u (%.1f%%)\n",
                g_own_idle, 100.0 * g_own_idle / (tot ? tot : 1));
        for (uint32_t i = 0; i < g_own_n; i++) {
            owner_t *w = &g_own[i];
            if (!w->hits) continue;
            fprintf(out, "## thread #%u (pthread %lu): %u samples %.1f%%\n", i, (unsigned long)w->id,
                    w->hits, 100.0 * w->hits / (tot ? tot : 1));
            hotbucket_t tmp[OWN_HASH]; uint32_t live = 0;
            for (uint32_t k = 0; k < OWN_HASH; k++) if (w->h[k].hits) tmp[live++] = w->h[k];
            qsort(tmp, live, sizeof(hotbucket_t), cmp_hot);
            for (uint32_t r = 0; r < live && r < 25; r++)
                fprintf(out, "   0x%08X %6u %5.1f%% of thread\n", tmp[r].va, tmp[r].hits,
                        100.0 * tmp[r].hits / w->hits);
        }
    }

    if (to_file) {
        fclose(out);
        rename(tmp, g_cfg.out);
    }
    write_fps_csv();
    if (g_have_dump_lock) pthread_mutex_unlock(&g_dump_lock);
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */
void nfs5_prof_note_event(const char *what) {
    if (!g_running || !what) return;
    snprintf(g_event, sizeof g_event, "%s", what);
    g_have_event = 1;
}

static void resolve_cur_func(void) {
    if (g_cur_p) return;
    g_cur_p = (uint32_t *)dlsym(RTLD_DEFAULT, "g_cur_func");
}

int nfs5_prof_start(void) {
    if (g_running) return -1;
    g_cfg.enabled = 1;
    g_cfg.interval_us = env_long("NFS5_PROFILE_INTERVAL_MS", 5) * 1000;
    if (g_cfg.interval_us < 100) g_cfg.interval_us = 100;
    g_cfg.timeline_us = env_long("NFS5_PROFILE_TIMELINE_MS", 250) * 1000;
    if (g_cfg.timeline_us <= 0) g_cfg.timeline_us = 250000;
    g_cfg.snapshot_us = env_long("NFS5_PROFILE_SNAPSHOT_MS", 1000) * 1000;
    g_cfg.top_n = (int)env_long("NFS5_PROFILE_TOP", 25);
    if (g_cfg.top_n < 1) g_cfg.top_n = 1;
    if (g_cfg.top_n > 2000) g_cfg.top_n = 2000;
    env_path("NFS5_PROFILE_OUT", g_cfg.out, sizeof g_cfg.out);
    env_path("NFS5_PROFILE_FPS_CSV", g_cfg.fps_csv, sizeof g_cfg.fps_csv);
    env_path("NFS5_PROFILE_MEM", g_cfg.mem_csv, sizeof g_cfg.mem_csv);
    env_path("NFS5_PROFILE_CATALOG_DIR", g_cfg.catalog_dir, sizeof g_cfg.catalog_dir);

    g_samples = (sample_t *)calloc(SAMPLE_RING, sizeof(sample_t));
    g_frames  = (uint64_t *)calloc(FRAME_CAP, sizeof(uint64_t));
    if (!g_samples || !g_frames) {
        free(g_samples); g_samples = NULL;
        free(g_frames); g_frames = NULL;
        return -1;
    }
    resolve_cur_func();
    fprintf(stderr, "[nfs5_prof] sampler start pid=%ld\n", (long)getpid());
    g_t0_us = mono_us();
    {
        struct timespec rt;
        clock_gettime(CLOCK_REALTIME, &rt);
        g_t0_wall = (uint64_t)rt.tv_sec;
    }
    g_nsamples = g_nframes = g_nhash = 0;
    g_zero_samples = g_total_samples = 0;
    g_rss_kb = g_rss_peak_kb = g_heap_kb = g_heap_peak_kb = 0;
    memset(g_phases, 0, sizeof g_phases);
    memset(g_counters, 0, sizeof g_counters);
    g_last_mem_us = 0;
    g_have_event = 0;
    g_finalized = 0;
    g_have_dump_lock = 1;
    g_stop = 0;
    __atomic_store_n(&g_running, 1, __ATOMIC_RELEASE);
    read_mem();
    if (g_cfg.mem_csv[0]) {
        FILE *f = fopen(g_cfg.mem_csv, "w");   /* truncate any stale series */
        if (f) { fprintf(f, "t_ms,rss_kb,rss_peak_kb,heap_inuse_kb,heap_peak_kb\n"); fclose(f); }
    }
    if (pthread_create(&g_thread, NULL, sampler_main, NULL) != 0) {
        __atomic_store_n(&g_running, 0, __ATOMIC_RELEASE);
        return -1;
    }
    atexit(nfs5_prof_stop);
    return 0;
}

void nfs5_prof_stop(void) {
    if (!__atomic_load_n(&g_running, __ATOMIC_RELAXED)) return;
    __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
    pthread_join(g_thread, NULL);
    /* dump while g_running is still 1: nfs5_prof_dump() is a no-op once the
     * profiler is marked stopped, and the final report must still be written. */
    nfs5_prof_dump();
    __atomic_store_n(&g_running, 0, __ATOMIC_RELAXED);
    g_finalized = 1;
}

int nfs5_prof_enabled(void) { return __atomic_load_n(&g_running, __ATOMIC_RELAXED); }

/* ------------------------------------------------------------------ */
/* self-start: no main() change needed; zero work when NFS5_PROFILE unset */
/* ------------------------------------------------------------------ */
__attribute__((constructor(102)))
static void nfs5_prof_ctor(void) {
    /* A profiled process may itself spawn children (win32hle's font lookup uses
     * popen("fc-match ..."), for example). Those children inherit LD_PRELOAD and
     * would run a second sampler writing the same report files, corrupting the
     * measurement, so only the first process in the tree profiles: it marks the
     * environment and children skip. */
    const char *active = getenv("NFS5_PROFILE_ACTIVE");
    if (active && *active) return;
    const char *e = getenv("NFS5_PROFILE");
    if (!e || !*e || !strcmp(e, "0")) return;
    setenv("NFS5_PROFILE_ACTIVE", "1", 1);
    nfs5_prof_start();
}

/* ------------------------------------------------------------------ */
/* LD_PRELOAD mode: count presents even without a rebuilt game binary. */
/* The SDL types are opaque pointers; the interposers only need the     */
/* calling convention, and dlsym(RTLD_NEXT) reaches the real SDL2.      */
/* ------------------------------------------------------------------ */
#ifdef NFS5_PROF_PRELOAD
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;

static void (*g_real_render_present)(SDL_Renderer *);
static int  (*g_real_update_window_surface)(SDL_Window *);
static void (*g_real_gl_swap_window)(SDL_Window *);

void SDL_RenderPresent(SDL_Renderer *renderer) {
    if (!g_real_render_present)
        g_real_render_present = (void (*)(SDL_Renderer *))dlsym(RTLD_NEXT, "SDL_RenderPresent");
    nfs5_frame_mark();
    if (g_real_render_present) g_real_render_present(renderer);
}
int SDL_UpdateWindowSurface(SDL_Window *window) {
    if (!g_real_update_window_surface)
        g_real_update_window_surface = (int (*)(SDL_Window *))dlsym(RTLD_NEXT, "SDL_UpdateWindowSurface");
    nfs5_frame_mark();
    return g_real_update_window_surface ? g_real_update_window_surface(window) : -1;
}
void SDL_GL_SwapWindow(SDL_Window *window) {
    if (!g_real_gl_swap_window)
        g_real_gl_swap_window = (void (*)(SDL_Window *))dlsym(RTLD_NEXT, "SDL_GL_SwapWindow");
    nfs5_frame_mark();
    if (g_real_gl_swap_window) g_real_gl_swap_window(window);
}
#endif /* NFS5_PROF_PRELOAD */
