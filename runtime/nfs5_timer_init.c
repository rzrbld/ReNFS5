/* TIMER_init (Porsche.exe 0x565030) with retry.
 *
 * TIMER_init(hz) starts the game's multimedia timer (timeSetEvent -> callback
 * -> event -> timer thread 0x565270 -> tick counter) and then waits only one
 * period's worth of milliseconds (GetTickCount() + hz, i.e. 100 ms) for the
 * first tick. If the tick is late it shuts the timer down (0x564FA0), leaves
 * TIMERHZ (0x5DEB48) at 0 and prints "FAILED TO INITIALIZE WINDOWS MULTIMEDIA
 * TIMER, TRY RE-RUNNING"; a few seconds later the frame-rate code stops the
 * game ("framerate - TIMERHZ NOT SET!", "Error in expression ...",
 * ExitProcess(1)). On the H700 the first tick crosses three threads while the
 * cores are still clocked down and the game is loading, so it is
 * occasionally late.
 *
 * The function shuts any previous timer down on entry, so calling it again is
 * exactly the re-run the message asks for.
 *
 * Shutdown guard (0x564FA0 / timer thread 0x565270): shutting the timer down
 * wakes the timer thread and waits until it has cleared its event handle
 * ([0x6A5BFC] = 0), but the thread's very next store is TIMERHZ = 0. With
 * guest threads running in parallel a restarting TIMER_init could set the new
 * TIMERHZ first and the old thread then wipe it. Never observed (the game
 * starts its timer once), but cheap to rule out: the thread procedure counts
 * itself as running until it has returned and shutdown waits for that.
 * (The observed "TIMERHZ NOT SET" exits were a winmm timeSetEvent race that
 * returned timer id 0; fixed in win32hle/winmm.c.)
 *
 * NFS5_TIMER_TRACE=1 logs init/shutdown/thread events and every change of
 * TIMERHZ, with the recent timeSetEvent results when it drops to 0.
 *
 * tools/patch_local_regs.py renames the lifted bodies to *_lifted; these
 * wrappers keep the original names, so direct calls and the dispatch table
 * reach them.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include "recomp_types.h"
#include "win32hle.h"

#define TIMERHZ_VA 0x5DEB48u
#define TIMER_INIT_TRIES 8

void recomp_nfs_porsche_00565030_lifted(void);
void recomp_nfs_porsche_00564FA0(void);   /* TIMER shutdown: stops the timer, TIMERHZ = 0 */

/* NFS5_TIMER_TRACE=1: log timer init/shutdown/thread events and every change
 * of TIMERHZ (a 1 ms watcher thread), to find what clears it. */
static int g_trace = -1;
static double trace_t0;
static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static void *trace_watch(void *arg) {
    (void)arg;
    uint32_t last = MEM32(TIMERHZ_VA);
    fprintf(stderr, "[timer-trace] %.3f TIMERHZ=%u (watch start)\n", now_s() - trace_t0, last);
    for (;;) {
        uint32_t v = MEM32(TIMERHZ_VA);
        if (v != last) {
            fprintf(stderr, "[timer-trace] %.3f TIMERHZ %u -> %u (running flag [0x6A5C0C]=%u event [0x6A5BFC]=%#x timer [0x6A5C10]=%u)\n",
                    now_s() - trace_t0, last, v, MEM32(0x6A5C0Cu), MEM32(0x6A5BFCu), MEM32(0x6A5C10u));
            if (v == 0) {
                extern void hle_winmm_dump_recent(void);
                hle_winmm_dump_recent();
            }
            last = v;
        }
        usleep(500);
    }
    return NULL;
}
static int trace_on(void) {
    if (g_trace < 0) {
        const char *v = getenv("NFS5_TIMER_TRACE");
        g_trace = v && *v && strcmp(v, "0") != 0;
        if (g_trace) {
            trace_t0 = now_s();
            pthread_t th;
            pthread_create(&th, NULL, trace_watch, NULL);
            pthread_detach(th);
        }
    }
    return g_trace;
}
#define TRACE(...) do { if (trace_on()) { fprintf(stderr, "[timer-trace] %.3f ", now_s() - trace_t0); fprintf(stderr, __VA_ARGS__); } } while (0)

void recomp_nfs_porsche_00565030(void) {
    /* cdecl: [esp] = return slot, [esp+4] = hz; the lifted body pops the slot */
    uint32_t slot = MEM32(g_esp);
    TRACE("TIMER_init(hz=%u) enter, TIMERHZ=%u\n", MEM32(g_esp + 4), MEM32(TIMERHZ_VA));
    for (int attempt = 1;; attempt++) {
        recomp_nfs_porsche_00565030_lifted();
        TRACE("TIMER_init attempt %d -> TIMERHZ=%u\n", attempt, MEM32(TIMERHZ_VA));
        if (attempt == 1 && getenv("NFS5_TIMER_TEST_FAIL")) {
            /* test hook: make the first attempt fail the way a late tick does */
            g_esp -= 4; MEM32(g_esp) = slot;
            recomp_nfs_porsche_00564FA0();
        }
        if (MEM32(TIMERHZ_VA) != 0 || attempt == TIMER_INIT_TRIES) {
            if (attempt > 1)
                fprintf(stderr, "[timer] TIMER_init %s after %d attempts\n",
                        MEM32(TIMERHZ_VA) ? "succeeded" : "still failing", attempt);
            return;
        }
        fprintf(stderr, "[timer] TIMER_init: first tick late (attempt %d), re-initialising\n",
                attempt);
        usleep(20000);
        g_esp -= 4;                 /* re-push the return slot; hz is still above it */
        MEM32(g_esp) = slot;
    }
}

/* ---- shutdown race: wait for the old timer thread to finish completely ---- */
void recomp_nfs_porsche_00565270_lifted(void);
void recomp_nfs_porsche_00564FA0_lifted(void);
static int g_timer_threads;

void recomp_nfs_porsche_00565270(void) {          /* timer thread procedure */
    __atomic_add_fetch(&g_timer_threads, 1, __ATOMIC_SEQ_CST);
    TRACE("timer thread start\n");
    recomp_nfs_porsche_00565270_lifted();
    TRACE("timer thread exit, TIMERHZ=%u\n", MEM32(TIMERHZ_VA));
    __atomic_sub_fetch(&g_timer_threads, 1, __ATOMIC_SEQ_CST);
}

void recomp_nfs_porsche_00564FA0(void) {          /* timer shutdown */
    TRACE("shutdown enter (ret %#x), TIMERHZ=%u running=%u\n", MEM32(g_esp), MEM32(TIMERHZ_VA), MEM32(0x6A5C0Cu));
    recomp_nfs_porsche_00564FA0_lifted();
    int waited = 0;
    while (__atomic_load_n(&g_timer_threads, __ATOMIC_SEQ_CST) > 0 && waited < 500) {
        hle_block_begin();
        usleep(1000);
        hle_block_end();
        waited++;
    }
    if (waited)
        fprintf(stderr, "[timer] shutdown waited %d ms for the old timer thread%s\n", waited,
                __atomic_load_n(&g_timer_threads, __ATOMIC_SEQ_CST) > 0 ? " (still running)" : "");
}
