/* nfs5_stream_lock.c - run the game's stream module one thread at a time.
 *
 * Porsche.exe's \real\cmn\stream.c (0x56AFA0-0x56C840: STREAM_get,
 * STREAM_isendofstream, ... and their helpers) feeds the movies and music from
 * a worker. It was written for single-core PCs: STREAM_get (0x56C540) reads a
 * stream record (its buffer at +0xC) before taking the game's lock, while the
 * worker fills that record. With guest threads running in parallel
 * (NFS5_PARALLEL=1, a large part of the race frame rate) the two collided: on
 * the H700 2 of 8 cold starts crashed in STREAM_get on a NULL buffer
 * (fault at guest 4, called from FEIntro.cpp 0x4DC850) as the intro movie
 * began; with threads serialised, 0 of 8.
 *
 * Every function of the module is wrapped (tools/patch_local_regs.py renames
 * the lifted bodies to *_lifted) so a host mutex covers it; nested calls on
 * one thread count, not re-lock. The mutex is never held across a wait:
 * hle_block_begin/end (every blocking call, a contended critical section)
 * give it up and take it back, and a spinning loop's back-edge yield hands
 * it to a waiting thread. A thread that holds the machine lock waits for the
 * mutex with the machine given up, so the two locks cannot deadlock.
 * NFS5_STREAM_LOCK=0 disables it (the wrappers then only forward). */
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void hle_block_begin(void);
extern void hle_block_end(void);

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static __thread int t_depth, t_held, t_suspended;
static volatile int g_waiters;
static int g_on = -1;

/* Only needed when guest threads run in parallel; with one machine lock the
 * module already runs one thread at a time. */
static int on(void) {
    if (g_on < 0) {
        extern int hle_parallel(void);
        const char *v = getenv("NFS5_STREAM_LOCK");
        g_on = hle_parallel() && !(v && *v && !strcmp(v, "0"));
        fprintf(stderr, "[stream-lock] stream.c %s\n", g_on ? "serialised" : "not locked");
    }
    return g_on;
}
/* Take the mutex. If it is busy, wait with the machine given up (a holder may
 * need the machine for a host call) and counted as a waiter (a holder spinning
 * inside the module then hands it over). */
static void lock_wait(void) {
    if (pthread_mutex_trylock(&g_mu) == 0) return;
    __atomic_add_fetch(&g_waiters, 1, __ATOMIC_ACQ_REL);
    hle_block_begin();                     /* t_held is 0: only the machine is given up */
    pthread_mutex_lock(&g_mu);
    hle_block_end();
    __atomic_sub_fetch(&g_waiters, 1, __ATOMIC_ACQ_REL);
}
static void enter(void) {
    if (!on() || t_depth++) return;
    lock_wait();
    t_held = 1;
}
static void leave(void) {
    if (!on() || t_depth == 0) return;
    if (--t_depth == 0 && t_held) { t_held = 0; pthread_mutex_unlock(&g_mu); }
}

/* host_lite.c: around every wait, and from the back-edge yield */
void hle_section_suspend(void) {
    if (t_held) { t_held = 0; t_suspended = 1; pthread_mutex_unlock(&g_mu); }
}
/* Called from hle_block_end before the machine is taken back, so this wait
 * never holds the machine. */
void hle_section_resume(void) {
    if (!t_suspended) return;
    t_suspended = 0;
    if (pthread_mutex_trylock(&g_mu) != 0) {
        __atomic_add_fetch(&g_waiters, 1, __ATOMIC_ACQ_REL);
        pthread_mutex_lock(&g_mu);
        __atomic_sub_fetch(&g_waiters, 1, __ATOMIC_ACQ_REL);
    }
    t_held = 1;
}
void hle_section_yield(void) {
    if (!t_held || !__atomic_load_n(&g_waiters, __ATOMIC_ACQUIRE)) return;
    t_held = 0;
    pthread_mutex_unlock(&g_mu);
    for (int i = 0; i < 100 && __atomic_load_n(&g_waiters, __ATOMIC_ACQUIRE); i++) sched_yield();
    lock_wait();
    t_held = 1;
}

#define STREAM_FUNCS \
    X(0056AFA0) \
    X(0056AFC0) \
    X(0056AFF0) \
    X(0056B230) \
    X(0056B380) \
    X(0056B3B0) \
    X(0056B4C0) \
    X(0056B540) \
    X(0056B5B0) \
    X(0056B690) \
    X(0056B790) \
    X(0056B800) \
    X(0056B860) \
    X(0056BA20) \
    X(0056BA80) \
    X(0056BAF0) \
    X(0056BCE0) \
    X(0056BD10) \
    X(0056BD70) \
    X(0056BE40) \
    X(0056C000) \
    X(0056C040) \
    X(0056C140) \
    X(0056C380) \
    X(0056C3E0) \
    X(0056C420) \
    X(0056C540) \
    X(0056C620) \
    X(0056C740) \
    X(0056C790) \
    X(0056C7E0) \
    X(0056C840)

#define X(a) void recomp_nfs_porsche_##a##_lifted(void); \
    void recomp_nfs_porsche_##a(void) { enter(); recomp_nfs_porsche_##a##_lifted(); leave(); }
STREAM_FUNCS
#undef X
