/* NFS5_FRAMELOG=<file>: one line per frame event, for frame pacing analysis
 * (development only; zero cost unless set):
 *   F <us>  ddraw Flip returned (after the frame cap's wait)
 *   P <us>  the presenter handed a new frame to the GPU path
 *   S <us>  the window swap finished (GL worker)
 * Times are CLOCK_MONOTONIC microseconds. Lines are buffered in memory and
 * written every 2048 events and at exit. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define FL_CAP 2048        /* written out every 2048 events: test runs end on a signal, not exit() */
static struct { char tag; uint64_t us; } g_fl[FL_CAP];
static unsigned g_fl_n;
static int g_fl_on = -1;
static FILE *g_fl_f;
static pthread_mutex_t g_fl_mu = PTHREAD_MUTEX_INITIALIZER;

static void fl_flush(void) {
    if (!g_fl_f) return;
    for (unsigned i = 0; i < g_fl_n; i++) fprintf(g_fl_f, "%c %llu\n", g_fl[i].tag, (unsigned long long)g_fl[i].us);
    g_fl_n = 0;
    fflush(g_fl_f);
}
static void fl_atexit(void) { pthread_mutex_lock(&g_fl_mu); fl_flush(); pthread_mutex_unlock(&g_fl_mu); }

void nfs5_framelog(char tag) {
    if (g_fl_on == 0) return;
    pthread_mutex_lock(&g_fl_mu);
    if (g_fl_on < 0) {
        const char *p = getenv("NFS5_FRAMELOG");
        g_fl_f = p && *p ? fopen(p, "w") : NULL;
        g_fl_on = g_fl_f != NULL;
        if (g_fl_on) atexit(fl_atexit);
    }
    if (g_fl_on) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        g_fl[g_fl_n].tag = tag;
        g_fl[g_fl_n].us = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
        if (++g_fl_n == FL_CAP) fl_flush();
    }
    pthread_mutex_unlock(&g_fl_mu);
}
