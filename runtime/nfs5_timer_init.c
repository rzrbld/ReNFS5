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
 * exactly the re-run the message asks for. tools/patch_local_regs.py renames
 * the lifted body to recomp_nfs_porsche_00565030_lifted; this wrapper keeps
 * the original name, so direct calls and the dispatch table reach it.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "recomp_types.h"

#define TIMERHZ_VA 0x5DEB48u
#define TIMER_INIT_TRIES 8

void recomp_nfs_porsche_00565030_lifted(void);
void recomp_nfs_porsche_00564FA0(void);   /* TIMER shutdown: stops the timer, TIMERHZ = 0 */

void recomp_nfs_porsche_00565030(void) {
    /* cdecl: [esp] = return slot, [esp+4] = hz; the lifted body pops the slot */
    uint32_t slot = MEM32(g_esp);
    for (int attempt = 1;; attempt++) {
        recomp_nfs_porsche_00565030_lifted();
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
