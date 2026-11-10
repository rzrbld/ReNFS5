/* Profile-guided build support (development only).
 *
 * An instrumented build (scripts/build_target.sh with NFS5_PGO=gen) counts
 * every branch and call; GCC writes the counters at a normal exit, but test
 * runs end with a timeout signal. NFS5_GCOV_DUMP=<seconds> writes them from a
 * helper thread that many seconds after start instead (GCOV_PREFIX picks the
 * directory on the device). Compiled in only with NFS5_PGO_GEN (set by the
 * instrumented build): a weak reference would not pull __gcov_dump out of
 * the static libgcov, so the hook needs a real one. */
#ifdef NFS5_PGO_GEN
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern void __gcov_dump(void);

static void *pgo_dump_thread(void *arg) {
    unsigned secs = (unsigned)(uintptr_t)arg;
    sleep(secs);
    __gcov_dump();
    fprintf(stderr, "[pgo] profile written after %u s\n", secs);
    return NULL;
}

__attribute__((constructor)) static void pgo_init(void) {
    const char *v = getenv("NFS5_GCOV_DUMP");
    if (!v || !*v) return;
    unsigned secs = (unsigned)atoi(v);
    if (!secs) return;
    pthread_t t;
    if (pthread_create(&t, NULL, pgo_dump_thread, (void *)(uintptr_t)secs) == 0) pthread_detach(t);
}
#endif
