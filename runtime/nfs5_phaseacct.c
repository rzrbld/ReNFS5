/* nfs5_phaseacct.c - see nfs5_phaseacct.h. Development-only, env-gated. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include "nfs5_phaseacct.h"

#define MAX_DEPTH 32
static int g_on = -1;
static pthread_t g_owner;   /* the frame-producing thread: it is set by the
                             * first presented frame, because NFS5 renders from
                             * the thread that owns the frame clock. Timer/audio
                             * threads run guest code too and would double count
                             * wall time, so they are ignored. */
static int g_have_owner;
static const char *g_names[NFS5_ACCT_COUNT] = {
    "d3d7", "renderer_draw", "renderer_upload", "readback", "ddraw",
    "dinput", "dsound", "win32", "wait", "convert", "present",
};
typedef struct { int cat; uint64_t t0, child_ns; } frame_t;
static __thread frame_t g_stack[MAX_DEPTH];
static __thread int g_depth;
static uint64_t g_total_ns[NFS5_ACCT_COUNT];
static uint64_t g_prev_ns[NFS5_ACCT_COUNT];
static uint64_t g_t_start, g_t_last;
static uint64_t g_frames, g_prev_frames;

const char *nfs5_acct_name(int cat) {
    return cat >= 0 && cat < NFS5_ACCT_COUNT ? g_names[cat] : "?";
}
static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
int nfs5_acct_on(void) {
    if (g_on < 0) {
        const char *v = getenv("NFS5_PHASE_LIVE");
        g_on = v && *v && strcmp(v, "0") != 0;
        if (g_on) fprintf(stderr, "[acct] live frame-thread phase accounting on\n");
    }
    return g_on;
}
static void print_interval(uint64_t now) {
    uint64_t dt = now - g_t_last;
    if (dt < 1000000000ull) return;                 /* at most one line per second */
    fprintf(stderr, "[acct] wall=%.1fs dt=%.2fs frames=%llu fps=%.1f", now > g_t_start ? (double)(now - g_t_start) / 1e9 : 0.0,
            (double)dt / 1e9, (unsigned long long)g_frames,
            dt ? 1000.0 * (double)(g_frames - g_prev_frames) / ((double)dt / 1e6) : 0.0);
    uint64_t host = 0;
    for (int i = 0; i < NFS5_ACCT_COUNT; i++) {
        uint64_t d = g_total_ns[i] - g_prev_ns[i];
        g_prev_ns[i] = g_total_ns[i];
        host += d;
        if (d || g_total_ns[i])
            fprintf(stderr, " %s=%.1f%%", g_names[i], 100.0 * (double)d / (double)dt);
    }
    fprintf(stderr, " guest=%.1f%%\n", dt > host ? 100.0 * (double)(dt - host) / (double)dt : 0.0);
    g_prev_frames = g_frames;
    g_t_last = now;
}
void nfs5_acct_enter(int cat) {
    if (!nfs5_acct_on() || cat < 0 || cat >= NFS5_ACCT_COUNT) return;
    if (!g_have_owner || !pthread_equal(pthread_self(), g_owner)) return;
    uint64_t now = now_ns();
    if (!g_t_start) g_t_start = g_t_last = now;
    print_interval(now);
    if (g_depth >= MAX_DEPTH) return;
    g_stack[g_depth].cat = cat;
    g_stack[g_depth].t0 = now;
    g_stack[g_depth].child_ns = 0;
    g_depth++;
}
void nfs5_acct_leave(int cat) {
    if (!nfs5_acct_on() || cat < 0 || cat >= NFS5_ACCT_COUNT) return;
    if (!g_have_owner || !pthread_equal(pthread_self(), g_owner)) return;
    if (!g_depth) return;
    frame_t *f = &g_stack[--g_depth];
    if (f->cat != cat) {                             /* unbalanced: ignore */
        g_depth = 0;
        return;
    }
    uint64_t dur = now_ns() - f->t0;
    g_total_ns[cat] += dur > f->child_ns ? dur - f->child_ns : 0;
    if (g_depth) g_stack[g_depth - 1].child_ns += dur;
}
void nfs5_pcprof_arm(void);
void nfs5_pcprof_poll(void);
void nfs5_acct_frame(void) {
    nfs5_pcprof_arm();       /* frame thread: NFS5_PCPROF host-PC sampler */
    nfs5_pcprof_poll();
    if (!nfs5_acct_on()) return;
    if (!g_have_owner) {
        g_owner = pthread_self();
        g_have_owner = 1;
        g_t_start = g_t_last = now_ns();
        fprintf(stderr, "[acct] frame thread is %lu\n", (unsigned long)g_owner);
    }
    if (!pthread_equal(pthread_self(), g_owner)) return;
    g_frames++;
    print_interval(now_ns());
}
