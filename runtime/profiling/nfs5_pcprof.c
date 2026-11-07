/* nfs5_pcprof.c - wall-clock host-PC sampler for the frame thread.
 *
 * NFS5_PCPROF=1: a CLOCK_MONOTONIC timer sends SIGPROF to the frame thread
 * only (SIGEV_THREAD_ID) at NFS5_PCPROF_HZ (default 1000). The handler stores
 * the interrupted host program counter. Unlike the g_cur_func sampler this
 * sees where host time really goes: lifted functions, runtime helpers, libc,
 * libmali, futex waits (a blocked thread is sampled inside the syscall).
 * NFS5_PCPROF_FROM_MS/TO_MS limit the window (process-relative). The report
 * (top symbols, top shared objects) is written at exit to NFS5_PCPROF_OUT or
 * stderr. Development only; zero cost unless enabled.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define RING (1u << 20)
static uintptr_t *g_pc;
static volatile uint32_t g_n;
static int g_state;            /* 0 unknown, 1 armed, -1 off */
static uint64_t g_t0, g_from, g_to;
static timer_t g_timer;

static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
__attribute__((constructor)) static void pcprof_start(void) { g_t0 = now_ms(); }
static void on_prof(int sig, siginfo_t *si, void *ctx) {
    (void)sig; (void)si;
    uint64_t t = now_ms() - g_t0;
    if (t < g_from || (g_to && t > g_to)) return;
    ucontext_t *uc = (ucontext_t *)ctx;
#if defined(__aarch64__)
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc;
#elif defined(__x86_64__)
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#else
    uintptr_t pc = 0;
#endif
    uint32_t n = g_n;
    if (n < RING) { g_pc[n] = pc; g_n = n + 1; }
}

typedef struct { const char *name; uint32_t hits; } agg_t;
static int cmp_agg(const void *a, const void *b) {
    const agg_t *x = a, *y = b;
    return (int)y->hits - (int)x->hits;
}
static void add(agg_t *tab, int *n, int cap, const char *name) {
    for (int i = 0; i < *n; i++) if (tab[i].name == name || !strcmp(tab[i].name, name)) { tab[i].hits++; return; }
    if (*n < cap) { tab[*n].name = name; tab[*n].hits = 1; (*n)++; }
}
static void report(void) {
    if (g_state != 1) return;
    g_state = 2;
    timer_delete(g_timer);
    uint32_t n = g_n;
    const char *path = getenv("NFS5_PCPROF_OUT");
    char buf[600];
    snprintf(buf, sizeof buf, "%s", path && *path ? path : "nfs5_pcprof.txt");
    /* raw PCs + the address map; symbolised offline (dladdr scans the whole
     * -rdynamic symbol table per call, far too slow on the handheld) */
    FILE *f = fopen(buf, "w");
    if (!f) return;
    fprintf(f, "# nfs5_pcprof raw: %u samples window %llu..%llu ms\n", n,
            (unsigned long long)g_from, (unsigned long long)g_to);
    for (uint32_t i = 0; i < n; i++) fprintf(f, "%lx\n", (unsigned long)g_pc[i]);
    fclose(f);
    snprintf(buf + strlen(buf), sizeof buf - strlen(buf), ".maps");
    FILE *m = fopen("/proc/self/maps", "r"), *o = fopen(buf, "w");
    if (m && o) { char line[512]; while (fgets(line, sizeof line, m)) fputs(line, o); }
    if (m) fclose(m);
    if (o) fclose(o);
    fprintf(stderr, "[pcprof] wrote %u samples\n", n);
}

/* called on the frame thread (first presented frame) */
void nfs5_pcprof_arm(void) {
    if (g_state) return;
    const char *v = getenv("NFS5_PCPROF");
    if (!v || !*v || !strcmp(v, "0")) { g_state = -1; return; }
    g_pc = malloc(sizeof *g_pc * RING);
    if (!g_pc) { g_state = -1; return; }
    const char *f = getenv("NFS5_PCPROF_FROM_MS"), *t = getenv("NFS5_PCPROF_TO_MS");
    g_from = f ? strtoull(f, NULL, 10) : 0;
    g_to = t ? strtoull(t, NULL, 10) : 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    struct sigevent se;
    memset(&se, 0, sizeof se);
    se.sigev_notify = SIGEV_THREAD_ID;
    se.sigev_signo = SIGPROF;
    se._sigev_un._tid = (pid_t)syscall(SYS_gettid);
    if (timer_create(CLOCK_MONOTONIC, &se, &g_timer)) { g_state = -1; return; }
    const char *hz = getenv("NFS5_PCPROF_HZ");
    long period = 1000000000L / (hz ? atol(hz) : 1000);
    struct itimerspec its = { { 0, period }, { 0, period } };
    timer_settime(g_timer, 0, &its, NULL);
    g_state = 1;
    atexit(report);
    fprintf(stderr, "[pcprof] sampling frame thread tid %ld\n", (long)se._sigev_un._tid);
}
/* the window may end before exit: dump then too */
void nfs5_pcprof_poll(void) {
    if (g_state == 1 && g_to && now_ms() - g_t0 > g_to) report();
}
